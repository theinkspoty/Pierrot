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
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace {

// Memo de probe por (caminho canônico, mtime, tamanho). O probe abre o
// container e chama find_stream_info — caro — e a MESMA mídia é sondada várias
// vezes (media pool, proxy, arrasto na timeline, export). O resultado é
// determinístico enquanto o arquivo não muda, então cacheamos. A chave inclui
// mtime+tamanho para invalidar automaticamente quando o arquivo é reescrito.
struct ProbeKey {
    QString path;
    qint64 mtimeMs = 0;
    qint64 size = 0;
    bool operator==(const ProbeKey& o) const {
        return mtimeMs == o.mtimeMs && size == o.size && path == o.path;
    }
};
inline uint qHash(const ProbeKey& k, uint seed = 0) {
    return qHash(k.path, seed) ^ uint(k.mtimeMs) ^ uint(k.size >> 4);
}

// Limite simples: acima disto a tabela é limpa (working set de um projeto é
// pequeno; não vale uma LRU aqui).
constexpr int kProbeCacheCap = 512;

QMutex g_probeMutex;
QHash<ProbeKey, FFmpegMediaInfo> g_probeCache;

} // namespace

// Sonda a mídia, reusando o resultado em cache se o arquivo não mudou. A
// sondagem real é a função logo abaixo (probeUncached).
static FFmpegMediaInfo probeUncached(const QString& filePath);

FFmpegMediaInfo FFmpegDecoder::probe(const QString& filePath) {
    const QFileInfo fi(filePath);
    ProbeKey key{ fi.canonicalFilePath().isEmpty() ? filePath : fi.canonicalFilePath(),
                  fi.lastModified().toMSecsSinceEpoch(), fi.size() };
    // canonicalFilePath vazio (arquivo ainda não existe / permissão) → não
    // memoiza, para não fixar um resultado vazio.
    const bool cacheable = !key.path.isEmpty() && fi.exists();
    if (cacheable) {
        QMutexLocker l(&g_probeMutex);
        const auto it = g_probeCache.constFind(key);
        if (it != g_probeCache.constEnd())
            return it.value();
    }
    const FFmpegMediaInfo info = probeUncached(filePath);
    if (cacheable) {
        QMutexLocker l(&g_probeMutex);
        if (g_probeCache.size() >= kProbeCacheCap)
            g_probeCache.clear();
        g_probeCache.insert(key, info);
    }
    return info;
}

static FFmpegMediaInfo probeUncached(const QString& filePath) {
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

