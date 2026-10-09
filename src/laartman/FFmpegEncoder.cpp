// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "FFmpegEncoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

#include <cmath>
#include <algorithm>

#include <QDebug>
#include <QByteArray>

FFmpegEncoder::FFmpegEncoder() = default;

FFmpegEncoder::~FFmpegEncoder() { close(); }

bool FFmpegEncoder::open(const QString& path, int width, int height, double fps) {
    return open(path, width, height, fps, Options());
}

bool FFmpegEncoder::open(const QString& path, int width, int height, double fps,
                         const Options& opts) {
    close();
    if (path.isEmpty() || width <= 0 || height <= 0 || fps <= 0.0) {
        qWarning().noquote() << QStringLiteral("[enc] parâmetros inválidos: %1 %2x%3 @%4fps")
            .arg(path).arg(width).arg(height).arg(fps);
        return false;
    }
    // yuv420p exige lados pares (o 2×2 chroma não tem meio pixel).
    m_width = width & ~1;
    m_height = height & ~1;
    if (m_width <= 0 || m_height <= 0) {
        qWarning() << "[enc] dimensões inválidas após arredondar para par";
        return false;
    }

    // fps → fração reduzida (ex.: 29.97 → 30000/1001). 1000 de base cobre as
    // taxas comuns; av_reduce simplifica (30.0 → 30/1).
    av_reduce(&m_fpsNum, &m_fpsDen,
              (int64_t)std::llround(fps * 1000.0), 1000, 1 << 30);

    AVFormatContext* fmt = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_alloc_output_context2(&fmt, nullptr, nullptr, pathUtf8.constData()) < 0
        || !fmt) {
        qWarning().noquote() << QStringLiteral("[enc] não inferiu o container para %1").arg(path);
        return false;
    }

    const QByteArray codecName = opts.codec.toUtf8();
    const AVCodec* codec = avcodec_find_encoder_by_name(codecName.constData());
    if (!codec) {
        qWarning().noquote() << QStringLiteral("[enc] encoder não encontrado: %1").arg(opts.codec);
        avformat_free_context(fmt);
        return false;
    }

    AVCodecContext* cc = avcodec_alloc_context3(codec);
    if (!cc) {
        avformat_free_context(fmt);
        return false;
    }

    cc->width = m_width;
    cc->height = m_height;
    cc->time_base = AVRational{m_fpsDen, m_fpsNum};
    cc->framerate = AVRational{m_fpsNum, m_fpsDen};
    cc->pix_fmt = AV_PIX_FMT_YUV420P;
    cc->bit_rate = opts.bitrate;
    cc->max_b_frames = std::max(0, opts.maxBFrames);
    cc->gop_size = opts.gopSize > 0
        ? opts.gopSize
        : std::max(1, (int)std::lround(double(m_fpsNum) / double(m_fpsDen) * 2.0));
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
        cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (!opts.preset.isEmpty())
        av_opt_set(cc->priv_data, "preset", opts.preset.toUtf8().constData(), 0);

    if (avcodec_open2(cc, codec, nullptr) < 0) {
        qWarning().noquote() << QStringLiteral("[enc] avcodec_open2 falhou para %1").arg(opts.codec);
        avcodec_free_context(&cc);
        avformat_free_context(fmt);
        return false;
    }

    AVStream* st = avformat_new_stream(fmt, nullptr);
    if (!st || avcodec_parameters_from_context(st->codecpar, cc) < 0) {
        qWarning() << "[enc] não foi possível criar o stream de saída";
        avcodec_free_context(&cc);
        avformat_free_context(fmt);
        return false;
    }
    st->time_base = cc->time_base;
    st->avg_frame_rate = cc->framerate;
    st->r_frame_rate = cc->framerate;

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt->pb, pathUtf8.constData(), AVIO_FLAG_WRITE) < 0) {
            qWarning().noquote() << QStringLiteral("[enc] avio_open falhou em %1").arg(path);
            avcodec_free_context(&cc);
            avformat_free_context(fmt);
            return false;
        }
    }

    if (avformat_write_header(fmt, nullptr) < 0) {
        qWarning() << "[enc] avformat_write_header falhou";
        if (fmt->pb) avio_closep(&fmt->pb);
        avcodec_free_context(&cc);
        avformat_free_context(fmt);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    if (frame) {
        frame->format = cc->pix_fmt;
        frame->width = m_width;
        frame->height = m_height;
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            frame = nullptr;
        }
    }

    SwsContext* sws = sws_getContext(
        m_width, m_height, AV_PIX_FMT_RGBA,
        m_width, m_height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    AVPacket* pkt = av_packet_alloc();

    if (!frame || !sws || !pkt) {
        qWarning() << "[enc] falha ao alocar buffers de encode";
        if (sws) sws_freeContext(sws);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        avcodec_free_context(&cc);
        if (fmt->pb) avio_closep(&fmt->pb);
        avformat_free_context(fmt);
        return false;
    }

    m_fmt = fmt;
    m_codec = cc;
    m_stream = st;
    m_frame = frame;
    m_sws = sws;
    m_pkt = pkt;
    m_frameIndex = 0;
    m_finished = false;
    return true;
}

bool FFmpegEncoder::addFrame(const QImage& rgba) {
    if (!m_fmt || !m_codec || m_finished || !m_frame) return false;

    QImage src = rgba;
    if (src.format() != QImage::Format_RGBA8888)
        src = src.convertToFormat(QImage::Format_RGBA8888);
    if (src.width() != m_width || src.height() != m_height)
        src = src.scaled(m_width, m_height, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    if (src.isNull()) return false;

    AVFrame* frame = static_cast<AVFrame*>(m_frame);
    if (av_frame_make_writable(frame) < 0) return false;

    const uint8_t* srcData[4] = { src.constBits(), nullptr, nullptr, nullptr };
    const int srcLines[4] = { (int)src.bytesPerLine(), 0, 0, 0 };
    sws_scale(static_cast<SwsContext*>(m_sws), srcData, srcLines, 0, m_height,
              frame->data, frame->linesize);

    frame->pts = m_frameIndex;
    if (!encodeFrame(frame)) return false;
    ++m_frameIndex;
    return true;
}

bool FFmpegEncoder::encodeFrame(AVFrame* frame) {
    AVCodecContext* cc = static_cast<AVCodecContext*>(m_codec);
    if (avcodec_send_frame(cc, frame) < 0) {
        qWarning() << "[enc] avcodec_send_frame falhou";
        return false;
    }
    return writePackets();
}

bool FFmpegEncoder::writePackets() {
    AVCodecContext* cc = static_cast<AVCodecContext*>(m_codec);
    AVFormatContext* fmt = static_cast<AVFormatContext*>(m_fmt);
    AVStream* st = static_cast<AVStream*>(m_stream);
    AVPacket* pkt = static_cast<AVPacket*>(m_pkt);
    for (;;) {
        const int r = avcodec_receive_packet(cc, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return true;
        if (r < 0) {
            qWarning() << "[enc] avcodec_receive_packet falhou:" << r;
            return false;
        }
        if (pkt->duration <= 0) pkt->duration = 1; // 1 tick em cc->time_base
        av_packet_rescale_ts(pkt, cc->time_base, st->time_base);
        pkt->stream_index = st->index;
        const int w = av_interleaved_write_frame(fmt, pkt);
        av_packet_unref(pkt);
        if (w < 0) {
            qWarning() << "[enc] av_interleaved_write_frame falhou:" << w;
            return false;
        }
    }
}

bool FFmpegEncoder::finish() {
    if (!m_fmt || m_finished) return m_fmt != nullptr;
    m_finished = true;

    bool ok = encodeFrame(nullptr); // drena os quadros retidos (B-frames etc.)

    AVFormatContext* fmt = static_cast<AVFormatContext*>(m_fmt);
    if (ok && av_write_trailer(fmt) < 0) {
        qWarning() << "[enc] av_write_trailer falhou";
        ok = false;
    }
    if (fmt && !(fmt->oformat->flags & AVFMT_NOFILE) && fmt->pb)
        avio_closep(&fmt->pb);
    return ok;
}

void FFmpegEncoder::freeAll() {
    if (m_sws) { sws_freeContext(static_cast<SwsContext*>(m_sws)); m_sws = nullptr; }
    if (m_pkt) { AVPacket* p = static_cast<AVPacket*>(m_pkt); av_packet_free(&p); m_pkt = nullptr; }
    if (m_frame) { AVFrame* f = static_cast<AVFrame*>(m_frame); av_frame_free(&f); m_frame = nullptr; }
    if (m_codec) { AVCodecContext* c = static_cast<AVCodecContext*>(m_codec); avcodec_free_context(&c); m_codec = nullptr; }
    if (m_fmt) { AVFormatContext* f = static_cast<AVFormatContext*>(m_fmt); avformat_free_context(f); m_fmt = nullptr; }
    m_stream = nullptr;
}

void FFmpegEncoder::close() {
    if (m_fmt && !m_finished)
        finish(); // fecha com trailer mesmo se o chamador esquecer
    freeAll();
    m_frameIndex = 0;
    m_finished = false;
    m_width = m_height = 0;
}
