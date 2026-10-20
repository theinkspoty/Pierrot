// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "PacketReader.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}

#include <QDebug>

PacketReader::PacketReader(int bucketSize)
    : m_bucketSize(bucketSize < 2 ? 2 : bucketSize) {}

PacketReader::~PacketReader() {
    close();
}

AVStream* PacketReader::stream() const {
    return m_stream;
}

AVRational PacketReader::timeBase() const {
    return m_stream ? m_stream->time_base : AVRational { 1, AV_TIME_BASE };
}

bool PacketReader::isOpen() const {
    return m_fmt != nullptr;
}

int PacketReader::buffered() const {
    std::lock_guard<std::mutex> l(m_mutex);
    return (int)m_queue.size();
}

bool PacketReader::open(const QString& filePath, int streamIndex) {
    close();

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) != 0)
        return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    int idx = streamIndex;
    if (idx < 0) {
        // Sem restrição explícita: entrega o melhor stream de vídeo.
        idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }
    if (idx >= 0 && idx < (int)fmt->nb_streams)
        m_stream = fmt->streams[idx];
    else
        m_stream = nullptr;

    m_fmt = fmt;
    m_source = filePath;
    m_streamIndex = streamIndex;

    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_running = true;
        m_eof = false;
        m_readErr = 0;
    }
    m_thread = std::thread(&PacketReader::produce, this);
    return true;
}

void PacketReader::close() {
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_running = false;
    }
    m_cvSpace.notify_all();
    m_cvData.notify_all();
    if (m_thread.joinable()) m_thread.join();

    // Descarta o que sobrou na fila.
    std::deque<AVPacket*> leftovers;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        leftovers.swap(m_queue);
    }
    for (AVPacket* p : leftovers) {
        if (p) { av_packet_free(&p); }
    }

    if (m_fmt) {
        avformat_close_input(&m_fmt);
        m_fmt = nullptr;
    }
    m_stream = nullptr;
    m_source.clear();
    m_streamIndex = -1;
}

void PacketReader::flush() {
    std::deque<AVPacket*> leftovers;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        leftovers.swap(m_queue);
        m_eof = false;
        m_readErr = 0;
    }
    for (AVPacket* p : leftovers) {
        if (p) av_packet_free(&p);
    }
    m_cvSpace.notify_all();
}

int PacketReader::seek(int streamIndex, int64_t timestamp, int flags) {
    if (!m_fmt) return -1;

    // Para a produção e limpa a fila antes de reposicionar (o contexto de
    // demuxing não é thread-safe; só a thread do reader o toca).
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_running = false;
    }
    m_cvSpace.notify_all();
    m_cvData.notify_all();
    if (m_thread.joinable()) m_thread.join();

    std::deque<AVPacket*> leftovers;
    {
        std::lock_guard<std::mutex> l(m_mutex);
        leftovers.swap(m_queue);
        m_eof = false;
        m_readErr = 0;
    }
    for (AVPacket* p : leftovers) {
        if (p) av_packet_free(&p);
    }

    if (streamIndex >= 0 && streamIndex < (int)m_fmt->nb_streams
        && streamIndex != m_streamIndex) {
        m_stream = m_fmt->streams[streamIndex];
    }

    const int err = av_seek_frame(m_fmt, streamIndex, timestamp, flags);

    // Reinicia a produção a partir da nova posição.
    {
        std::lock_guard<std::mutex> l(m_mutex);
        m_running = true;
    }
    m_thread = std::thread(&PacketReader::produce, this);
    return err;
}

int PacketReader::read(AVPacket* out, const std::atomic<bool>* cancel) {
    if (!out) return -1;
    std::unique_lock<std::mutex> l(m_mutex);
    while (m_queue.empty() && m_running && !m_eof) {
        if (cancel && cancel->load()) return AVERROR_EXIT;
        // Espera com timeout curto para honrar cancel sem depender de notify.
        m_cvData.wait_for(l, std::chrono::milliseconds(20));
    }
    if (!m_queue.empty()) {
        AVPacket* p = m_queue.front();
        m_queue.pop_front();
        m_cvSpace.notify_one();
        av_packet_move_ref(out, p);
        av_packet_free(&p);
        return 0;
    }
    if (m_readErr != 0) return m_readErr;
    return AVERROR_EOF;
}

void PacketReader::produce() {
    while (true) {
        {
            std::unique_lock<std::mutex> l(m_mutex);
            // Espera espaço (ou ordem de parar).
            m_cvSpace.wait(l, [this] {
                return !m_running || (int)m_queue.size() < m_bucketSize;
            });
            if (!m_running) return;
        }

        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            std::lock_guard<std::mutex> l(m_mutex);
            m_readErr = AVERROR(ENOMEM);
            m_running = false;
            m_cvData.notify_all();
            return;
        }

        const int r = av_read_frame(m_fmt, pkt);
        if (r < 0) {
            av_packet_free(&pkt);
            std::lock_guard<std::mutex> l(m_mutex);
            // EOF normal vs. erro real.
            if (r == AVERROR_EOF) m_eof = true;
            else m_readErr = r;
            m_running = false;
            m_cvData.notify_all();
            return;
        }

        // Filtra pelo stream pedido (quando restrito).
        if (m_streamIndex >= 0 && pkt->stream_index != m_streamIndex) {
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            continue;
        }

        {
            std::lock_guard<std::mutex> l(m_mutex);
            if (!m_running) {
                av_packet_free(&pkt);
                return;
            }
            m_queue.push_back(pkt);
            m_cvData.notify_one();
        }
    }
}
