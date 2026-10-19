// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Probe de mídia: abre o container, lê duração/dimensões/fps e detecta
// GIF/WebP animados. Extraído de FFmpegDecoder.cpp (fragmentação do motor).

#include "FFmpegDecoder.h"
#include "FFmpegDecoderPriv.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
}

#include <QDebug>

FFmpegMediaInfo FFmpegDecoder::probe(const QString& filePath) {
    installLogFilter();
    FFmpegMediaInfo info;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) != 0)
        return info;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return info;
    }
    if (fmt->duration > 0)
        info.duration = fmt->duration / (double)AV_TIME_BASE;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !info.hasVideo) {
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
                continue;
            info.hasVideo = true;
            info.width = st->codecpar->width;
            info.height = st->codecpar->height;
            if (info.duration <= 0.0 && st->duration > 0)
                info.duration = st->duration * av_q2d(st->time_base);
            if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
                const double r = av_q2d(st->avg_frame_rate);
                if (r > 0.0 && r < 240.0) info.fps = r;
            }
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            info.hasAudio = true;
            ++info.audioStreams;
            info.audioChannels.append(st->codecpar->ch_layout.nb_channels);
        }
    }
    avformat_close_input(&fmt);
    return info;
}

bool FFmpegDecoder::isAnimatedImage(const QString& filePath) {
    const QString ext = QFileInfo(filePath).suffix().toLower();
    if (ext != QLatin1String("gif") && ext != QLatin1String("webp"))
        return false;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) != 0)
        return false;
    bool animated = false;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        for (unsigned i = 0; i < fmt->nb_streams && !animated; ++i) {
            const AVStream* st = fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                continue;
            const int64_t nb = st->nb_frames;
            // nb_frames é a fonte confiável quando disponível (0 = desconhecido,
            // tratado como animado por segurança). duration só decide se nb_frames
            // não foi preenchido, e exige múltiplos quadros implícitos (>1 frame
            // de atraso), evitando falso positivo em GIF estático com delay longo.
            animated = (nb != 1)
                       && (nb > 1
                           || (st->duration != AV_NOPTS_VALUE && st->duration > 1)
                           || (fmt->duration > AV_TIME_BASE / 4));
        }
    }
    avformat_close_input(&fmt);
    return animated;
}

