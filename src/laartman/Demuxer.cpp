// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "Demuxer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <QDebug>
#include <QFileInfo>

Demuxer::Demuxer() : m_fmt(nullptr) {}

Demuxer::~Demuxer() {
    close();
}

bool Demuxer::open(const QString& filePath) {
    if (isOpen() && m_source == filePath) return true;

    close();

    if (avformat_open_input(&m_fmt, filePath.toUtf8().constData(), nullptr, nullptr) != 0) {
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        avformat_close_input(&m_fmt);
        m_fmt = nullptr;
        return false;
    }

    m_source = filePath;
    collectStreams();
    return true;
}

void Demuxer::close() {
    if (m_fmt) {
        avformat_close_input(&m_fmt);
        m_fmt = nullptr;
    }
    m_source.clear();
    m_streams.clear();
}

bool Demuxer::isOpen() const {
    return m_fmt != nullptr && !m_source.isEmpty();
}

void Demuxer::collectStreams() {
    m_streams.clear();
    if (!m_fmt) return;
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        StreamInfo si;
        si.index = (int)i;
        si.type = m_fmt->streams[i]->codecpar->codec_type;
        si.params = m_fmt->streams[i]->codecpar;
        si.time_base = m_fmt->streams[i]->time_base;
        si.avg_frame_rate = m_fmt->streams[i]->avg_frame_rate;
        si.r_frame_rate = m_fmt->streams[i]->r_frame_rate;
        si.duration = m_fmt->streams[i]->duration;
        si.nb_frames = m_fmt->streams[i]->nb_frames;
        m_streams.append(si);
    }
}

int Demuxer::findBestVideoStream() const {
    if (!m_fmt) return -1;
    int idx = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    return idx;
}

int Demuxer::findBestAudioStream() const {
    if (!m_fmt) return -1;
    int idx = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    return idx;
}

int Demuxer::resolveAudioStreamIndex(int audioTrack) const {
    if (audioTrack < 0 || !m_fmt) return -1;
    int audioIdx = -1;
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i) {
        if (m_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++audioIdx;
            if (audioIdx == audioTrack) return (int)i;
        }
    }
    return -1;
}

StreamInfo Demuxer::videoStreamInfo() const {
    int idx = findBestVideoStream();
    for (const auto& si : m_streams) {
        if (si.index == idx) return si;
    }
    return StreamInfo();
}

StreamInfo Demuxer::audioStreamInfo(int absoluteIndex) const {
    for (const auto& si : m_streams) {
        if (si.index == absoluteIndex) return si;
    }
    return StreamInfo();
}

int Demuxer::readPacket(AVPacket* pkt) {
    if (!m_fmt || !pkt) return -1;
    return av_read_frame(m_fmt, pkt);
}

int Demuxer::seek(int streamIndex, int64_t timestamp, int flags) {
    if (!m_fmt) return -1;
    return av_seek_frame(m_fmt, streamIndex, timestamp, flags);
}

double Demuxer::duration() const {
    if (!m_fmt) return 0.0;
    if (m_fmt->duration > 0) {
        return m_fmt->duration / (double)AV_TIME_BASE;
    }
    return 0.0;
}
