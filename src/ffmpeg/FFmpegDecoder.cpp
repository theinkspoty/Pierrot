// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "FFmpegDecoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/log.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
}

#include <cmath>
#include <cfloat>
#include <cstring>
#include <QDebug>
#include <QFileInfo>
#include <QSet>
#include <QThread>

// Diagnóstico do caminho de áudio do preview: ligue com PIERROT_AUDIO_DEBUG=1.
static bool audioDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_AUDIO_DEBUG");
    return on;
}

static void decoderLogCallback(void*, int, const char*, va_list) {
    // Silencia completamente qualquer log/aviso interno do FFmpeg
}

static void installLogFilter() {
    static bool installed = false;
    if (!installed) {
        installed = true;
        av_log_set_level(AV_LOG_QUIET);
        av_log_set_callback(decoderLogCallback);
    }
}

// Imagem estática por extensão (igual ao exportador). O demuxer image2 às
// vezes reporta duração > 0 (0.04s) para um único JPEG, então depender só de
// `fmt->duration <= 0` fazia a maioria dos JPGs cair no caminho de vídeo
// (com seek em demuxer de frame único, frágil). Detectar pela extensão é
// determinístico.
static bool isImagePath(const QString& filePath) {
    const QString ext = QFileInfo(filePath).suffix().toLower();
    static const QSet<QString> exts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("svg")
    };
    return exts.contains(ext);
}

FFmpegDecoder::FFmpegDecoder() {
    installLogFilter();
    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
    m_audioPkt = av_packet_alloc();
    m_audioFrame = av_frame_alloc();
}
FFmpegDecoder::~FFmpegDecoder() {
    close();
    if (m_audioPkt) av_packet_free(&m_audioPkt);
    if (m_audioFrame) av_frame_free(&m_audioFrame);
    if (m_pkt) av_packet_free(&m_pkt);
    if (m_frame) av_frame_free(&m_frame);
}

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
        }
    }
    avformat_close_input(&fmt);
    return info;
}

static double sampleAt(const AVFrame* frame, const AVCodecContext* cc, int channel, int index) {
    const AVSampleFormat sf = static_cast<AVSampleFormat>(frame->format);
    const int planar = av_sample_fmt_is_planar(sf);
    int channels = frame->ch_layout.nb_channels;
    if (channels <= 0) channels = cc->ch_layout.nb_channels;
    const int bytes = av_get_bytes_per_sample(sf);
    const uint8_t* p;
    if (planar) {
        p = frame->data[channel] + (size_t)index * bytes;
    } else {
        p = frame->data[0] + ((size_t)index * channels + channel) * bytes;
    }
    switch (sf) {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P:
            return (*p - 128.0) / 128.0;
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            return *reinterpret_cast<const int16_t*>(p) / 32768.0;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            return *reinterpret_cast<const int32_t*>(p) / 2147483648.0;
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            return *reinterpret_cast<const float*>(p);
        case AV_SAMPLE_FMT_DBL:
        case AV_SAMPLE_FMT_DBLP:
            return *reinterpret_cast<const double*>(p);
        case AV_SAMPLE_FMT_S64:
        case AV_SAMPLE_FMT_S64P:
            return *reinterpret_cast<const int64_t*>(p) / 9223372036854775808.0;
        default:
            return 0.0;
    }
}

// Min/max de um frame inteiro com acesso tipado direto aos buffers. A versão
// anterior chamava sampleAt() para cada amostra (switch + chamada por amostra),
// o que tornava a análise de um áudio longo (~horas) extremamente lenta.
static void frameMinMax(const AVFrame* frame, const AVCodecContext* cc,
                        float& bmin, float& bmax) {
    int channels = frame->ch_layout.nb_channels;
    if (channels <= 0) channels = cc->ch_layout.nb_channels;
    const int nb = frame->nb_samples;
    if (channels <= 0 || nb <= 0) return;
    bmin = FLT_MAX;
    bmax = -FLT_MAX;
    const AVSampleFormat sf = static_cast<AVSampleFormat>(frame->format);
    switch (sf) {
    case AV_SAMPLE_FMT_S16P:
        for (int c = 0; c < channels; ++c) {
            const int16_t* p = reinterpret_cast<const int16_t*>(frame->data[c]);
            for (int i = 0; i < nb; ++i) {
                const float v = p[i] * (1.0f / 32768.0f);
                if (v < bmin) bmin = v;
                if (v > bmax) bmax = v;
            }
        }
        break;
    case AV_SAMPLE_FMT_S16: {
        const int16_t* p = reinterpret_cast<const int16_t*>(frame->data[0]);
        for (int i = 0; i < nb * channels; ++i) {
            const float v = p[i] * (1.0f / 32768.0f);
            if (v < bmin) bmin = v;
            if (v > bmax) bmax = v;
        }
        break;
    }
    case AV_SAMPLE_FMT_FLTP:
        for (int c = 0; c < channels; ++c) {
            const float* p = reinterpret_cast<const float*>(frame->data[c]);
            for (int i = 0; i < nb; ++i) {
                const float v = p[i];
                if (v < bmin) bmin = v;
                if (v > bmax) bmax = v;
            }
        }
        break;
    case AV_SAMPLE_FMT_FLT: {
        const float* p = reinterpret_cast<const float*>(frame->data[0]);
        for (int i = 0; i < nb * channels; ++i) {
            const float v = p[i];
            if (v < bmin) bmin = v;
            if (v > bmax) bmax = v;
        }
        break;
    }
    case AV_SAMPLE_FMT_S32P:
        for (int c = 0; c < channels; ++c) {
            const int32_t* p = reinterpret_cast<const int32_t*>(frame->data[c]);
            for (int i = 0; i < nb; ++i) {
                const float v = p[i] * (1.0f / 2147483648.0f);
                if (v < bmin) bmin = v;
                if (v > bmax) bmax = v;
            }
        }
        break;
    case AV_SAMPLE_FMT_S32: {
        const int32_t* p = reinterpret_cast<const int32_t*>(frame->data[0]);
        for (int i = 0; i < nb * channels; ++i) {
            const float v = p[i] * (1.0f / 2147483648.0f);
            if (v < bmin) bmin = v;
            if (v > bmax) bmax = v;
        }
        break;
    }
    case AV_SAMPLE_FMT_U8P:
        for (int c = 0; c < channels; ++c) {
            const uint8_t* p = frame->data[c];
            for (int i = 0; i < nb; ++i) {
                const float v = (p[i] - 128.0f) * (1.0f / 128.0f);
                if (v < bmin) bmin = v;
                if (v > bmax) bmax = v;
            }
        }
        break;
    case AV_SAMPLE_FMT_U8: {
        const uint8_t* p = frame->data[0];
        for (int i = 0; i < nb * channels; ++i) {
            const float v = (p[i] - 128.0f) * (1.0f / 128.0f);
            if (v < bmin) bmin = v;
            if (v > bmax) bmax = v;
        }
        break;
    }
    case AV_SAMPLE_FMT_DBLP:
        for (int c = 0; c < channels; ++c) {
            const double* p = reinterpret_cast<const double*>(frame->data[c]);
            for (int i = 0; i < nb; ++i) {
                const float v = (float)p[i];
                if (v < bmin) bmin = v;
                if (v > bmax) bmax = v;
            }
        }
        break;
    case AV_SAMPLE_FMT_DBL: {
        const double* p = reinterpret_cast<const double*>(frame->data[0]);
        for (int i = 0; i < nb * channels; ++i) {
            const float v = (float)p[i];
            if (v < bmin) bmin = v;
            if (v > bmax) bmax = v;
        }
        break;
    }
    default: {
        // Formato incomum: caminho genérico (mais lento, usado como garantia).
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < nb; ++i) {
                const float v = (float)sampleAt(frame, cc, c, i);
                if (v < bmin) bmin = v;
                if (v > bmax) bmax = v;
            }
        break;
    }
    }
}

FFmpegAudioPeaks FFmpegDecoder::audioPeaks(const QString& filePath, int bucketsPerSecond) {
    installLogFilter();
    FFmpegAudioPeaks result;
    if (bucketsPerSecond <= 0) return result;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) != 0)
        return result;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return result;
    }

    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (idx < 0) {
        avformat_close_input(&fmt);
        return result;
    }

    const AVCodec* codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return result;
    }

    AVCodecContext* cc = avcodec_alloc_context3(codec);
    if (!cc) {
        avformat_close_input(&fmt);
        return result;
    }
    avcodec_parameters_to_context(cc, fmt->streams[idx]->codecpar);
    if (avcodec_open2(cc, codec, nullptr) < 0) {
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return result;
    }

    const double timeBase = av_q2d(fmt->streams[idx]->time_base);
    const double sampleRate = cc->sample_rate > 0 ? cc->sample_rate : 44100.0;

    const float sentinelMin = FLT_MAX; // bucket vazio
    const float sentinelMax = -FLT_MAX;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    double elapsed = 0.0;

    while (pkt && frame && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == idx) {
            if (avcodec_send_packet(cc, pkt) == 0) {
                while (avcodec_receive_frame(cc, frame) == 0) {
                    int64_t pts = frame->pts;
                    if (pts == AV_NOPTS_VALUE)
                        pts = frame->best_effort_timestamp;
                    double t;
                    if (pts != AV_NOPTS_VALUE)
                        t = pts * timeBase;
                    else
                        t = elapsed;
                    elapsed = t + frame->nb_samples / sampleRate;

                    int bucket = (int)std::llround(t * bucketsPerSecond);
                    if (bucket < 0) bucket = 0;

                    if (result.min.size() <= bucket) {
                        const int old = result.min.size();
                        result.min.resize(bucket + 1);
                        result.max.resize(bucket + 1);
                        for (int i = old; i <= bucket; ++i) {
                            result.min[i] = sentinelMin;
                            result.max[i] = sentinelMax;
                        }
                    }

                    float bmin = FLT_MAX;
                    float bmax = -FLT_MAX;
                    frameMinMax(frame, cc, bmin, bmax);
                    if (bmin < result.min[bucket]) result.min[bucket] = bmin;
                    if (bmax > result.max[bucket]) result.max[bucket] = bmax;
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);

    for (int i = 0; i < result.min.size(); ++i) {
        if (result.min[i] == sentinelMin) {
            result.min[i] = 0.0f;
            result.max[i] = 0.0f;
        }
    }
    result.bucketsPerSecond = bucketsPerSecond;
    return result;
}

bool FFmpegDecoder::open(const QString& filePath) {
    // Segura os DOIS mutexes: o vídeo (frameAt) e o áudio (decodeAudio/
    // seekAudio) rodam em threads diferentes e podem estar em andamento
    // quando abrimos outro arquivo. Liberar contextos sem o lock do áudio
    // causava use-after-free na troca de clipe durante a reprodução.
    QMutexLocker vlock(&m_mutex);
    QMutexLocker alock(&m_audioMutex);
    if (m_ctx && m_source == filePath) return true;
    freeAllLocked();

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) != 0)
        return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }
    m_ctx = fmt;
    m_source = filePath;
    m_lastPtsSec = -1.0;

    // Vídeo é opcional: arquivos só-áudio (mp3/wav) ainda abrem para o
    // preview tocar. frameAt() devolve imagem vazia quando m_stream < 0.
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx >= 0) {
        const AVCodec* codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
        if (codec) {
            AVCodecContext* cc = avcodec_alloc_context3(codec);
            if (cc) {
                avcodec_parameters_to_context(cc, fmt->streams[idx]->codecpar);
                cc->thread_count = qMin(4, QThread::idealThreadCount());
                cc->thread_type = FF_THREAD_FRAME;
                const int openErr = avcodec_open2(cc, codec, nullptr);
                if (openErr == 0) {
                    m_codec = cc;
                    m_stream = idx;
                    const AVStream* st = fmt->streams[idx];
                    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
                        const double r = av_q2d(st->avg_frame_rate);
                        if (r > 0.0 && r < 240.0) m_fps = r;
                    }
                    // Imagem estática: extensão de imagem com UM único frame,
                    // OU duração <= 0 (AV_NOPTS_VALUE ou 0). O check de frame
                    // único evita que GIF/WebP animados (vários frames) caiam
                    // aqui e congelem no primeiro quadro do preview.
                    const bool singleFrame = (st->duration == AV_NOPTS_VALUE
                                             || st->duration <= 1
                                             || (st->nb_frames > 0 && st->nb_frames <= 1));
                    m_isImage = (isImagePath(filePath) && singleFrame)
                                || (fmt->duration <= 0);
                } else {
                    avcodec_free_context(&cc);
                }
            }
        }
    }

    // Stream de áudio: reutiliza o mesmo AVFormatContext já aberto
    const int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aidx >= 0) {
        const AVCodec* acodec = avcodec_find_decoder(fmt->streams[aidx]->codecpar->codec_id);
        if (acodec) {
            AVCodecContext* acc = avcodec_alloc_context3(acodec);
            if (acc) {
                avcodec_parameters_to_context(acc, fmt->streams[aidx]->codecpar);
                acc->thread_count = 0;
                if (avcodec_open2(acc, acodec, nullptr) == 0) {
                    SwrContext* swr = swr_alloc();
                    if (swr) {
                        AVChannelLayout outLayout;
                        av_channel_layout_default(&outLayout, 2);
                        const AVSampleFormat outFmt = AV_SAMPLE_FMT_S16;
                        const int outRate = m_audioOutRate;
                        if (swr_alloc_set_opts2(&swr,
                                &outLayout, outFmt, outRate,
                                &acc->ch_layout, acc->sample_fmt,
                                acc->sample_rate, 0, nullptr) >= 0
                            && swr_init(swr) >= 0) {
                            m_aCtx = fmt;
                            m_aCodec = acc;
                            m_swr = swr;
                            m_audioStream = aidx;
                        } else {
                            swr_free(&swr);
                        }
                    }
                    if (!m_aCodec) avcodec_free_context(&acc);
                } else {
                    avcodec_free_context(&acc);
                }
            }
        }
    }
    return true;
}

void FFmpegDecoder::close() {
    QMutexLocker vlock(&m_mutex);
    QMutexLocker alock(&m_audioMutex);
    freeAllLocked();
}

// Chama com m_mutex E m_audioMutex segurados (nunca sozinha).
void FFmpegDecoder::freeAllLocked() {
    if (m_sws) {
        sws_freeContext(reinterpret_cast<SwsContext*>(m_sws));
        m_sws = nullptr;
    }
    if (m_lastFrame) {
        av_frame_unref(reinterpret_cast<AVFrame*>(m_lastFrame));
        av_frame_free(reinterpret_cast<AVFrame**>(&m_lastFrame));
        m_lastFrame = nullptr;
    }
    if (m_nextFrame) {
        av_frame_unref(reinterpret_cast<AVFrame*>(m_nextFrame));
        av_frame_free(reinterpret_cast<AVFrame**>(&m_nextFrame));
        m_nextFrame = nullptr;
    }
    m_lastFrameSec = -1.0;
    m_nextFrameSec = -1.0;
    if (m_swr) {
        swr_free(reinterpret_cast<SwrContext**>(&m_swr));
        m_swr = nullptr;
    }
    if (m_aCodec) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&m_aCodec));
        m_aCodec = nullptr;
    }
    m_aCtx = nullptr;
    if (m_codec) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&m_codec));
        m_codec = nullptr;
    }
    if (m_ctx) {
        avformat_close_input(reinterpret_cast<AVFormatContext**>(&m_ctx));
        m_ctx = nullptr;
    }
    m_stream = -1;
    m_isImage = false;
    m_audioStream = -1;
    m_source.clear();
    m_lastPtsSec = -1.0;
    m_swsSrcW = m_swsSrcH = 0;
    m_swsDstW = m_swsDstH = 0;
    m_swsSrcFmt = -1;
    if (m_pkt) av_packet_unref(m_pkt);
    if (m_frame) av_frame_unref(m_frame);
    if (m_audioPkt) av_packet_unref(m_audioPkt);
    if (m_audioFrame) av_frame_unref(m_audioFrame);
}

bool FFmpegDecoder::isOpen() const {
    QMutexLocker vlock(&m_mutex);
    return m_ctx != nullptr;
}
QString FFmpegDecoder::source() const {
    QMutexLocker vlock(&m_mutex);
    return m_source;
}
double FFmpegDecoder::fps() const {
    QMutexLocker vlock(&m_mutex);
    return m_fps;
}

QImage FFmpegDecoder::frameAt(double seconds, int maxWidth) {
    QMutexLocker locker(&m_mutex);
    QImage result;
    if (!m_ctx || m_stream < 0) return result;

    AVFormatContext* fmt = static_cast<AVFormatContext*>(m_ctx);
    AVCodecContext* cc = static_cast<AVCodecContext*>(m_codec);
    AVStream* st = fmt->streams[m_stream];

    const double targetSec = m_isImage ? 0.0 : std::max(0.0, seconds);

    // fsec de um quadro no tempo do stream (fallback: o próprio alvo).
    auto frameSec = [&](const AVFrame* fr) -> double {
        int64_t pts = fr->pts;
        if (pts == AV_NOPTS_VALUE)
            pts = fr->best_effort_timestamp;
        return (pts != AV_NOPTS_VALUE) ? (pts * av_q2d(st->time_base)) : targetSec;
    };
    // Guarda um quadro como o candidato de exibição atual (m_lastFrame).
    auto keepAsDisplay = [&](const AVFrame* fr, double fsec) {
        AVFrame* lf = reinterpret_cast<AVFrame*>(m_lastFrame);
        if (!lf) {
            lf = av_frame_alloc();
            m_lastFrame = lf;
        }
        if (lf) {
            av_frame_unref(lf);
            av_frame_ref(lf, fr);
        }
        m_lastFrameSec = fsec;
    };

    // Imagem estática (JPEG, PNG, BMP…): frame único — qualquer timestamp
    // retorna o mesmo frame. A primeira decodificação NÃO usa seek: em demuxer
    // de frame único (image2) o seek é frágil, e repeti-lo a cada tick do
    // preview causava stutter e, em alguns arquivos, corrupção do estado do
    // codec (crash). Lemos os pacotes em sequência, guardamos o primeiro
    // quadro em m_lastFrame e os pedidos seguintes reutilizam o cache,
    // independentemente do pts do frame (que nem sempre é 0).
    if (m_isImage) {
        if (!m_lastFrame || m_lastFrameSec < 0.0) {
            av_packet_unref(m_pkt);
            av_frame_unref(m_frame);
            bool got = false;
            while (!got) {
                while (avcodec_receive_frame(cc, m_frame) == 0) {
                    keepAsDisplay(m_frame, frameSec(m_frame));
                    av_frame_unref(m_frame);
                    got = true;
                    break; // imagens têm um único quadro
                }
                if (got) break;
                const int r = av_read_frame(fmt, m_pkt);
                if (r < 0) {
                    avcodec_send_packet(cc, nullptr); // drena o codec no fim
                    if (avcodec_receive_frame(cc, m_frame) == 0) {
                        keepAsDisplay(m_frame, frameSec(m_frame));
                        av_frame_unref(m_frame);
                        got = true;
                    }
                    break;
                }
                if (m_pkt->stream_index == m_stream)
                    avcodec_send_packet(cc, m_pkt);
                av_packet_unref(m_pkt);
            }
        }
        AVFrame* fr = reinterpret_cast<AVFrame*>(m_lastFrame);
        if (!fr || m_lastFrameSec < 0.0)
            return result; // não conseguiu decodificar o frame único
        const int sw = fr->width > 0 ? fr->width : cc->width;
        const int sh = fr->height > 0 ? fr->height : cc->height;
        if (sw > 0 && sh > 0) {
            int dw = sw, dh = sh;
            if (maxWidth > 0 && sw > maxWidth) {
                dw = maxWidth;
                dh = qMax(2, sh * dw / sw);
                if (dh & 1) dh += 1;
            }
            SwsContext* sws = reinterpret_cast<SwsContext*>(m_sws);
            const AVPixelFormat srcFmt = static_cast<AVPixelFormat>(fr->format);
            if (!sws || m_swsSrcW != sw || m_swsSrcH != sh || m_swsSrcFmt != srcFmt
                || m_swsDstW != dw || m_swsDstH != dh) {
                if (sws) sws_freeContext(sws);
                sws = sws_getContext(sw, sh, srcFmt, dw, dh, AV_PIX_FMT_RGB24,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                m_sws = sws;
                m_swsSrcW = sw;
                m_swsSrcH = sh;
                m_swsSrcFmt = static_cast<int>(srcFmt);
                m_swsDstW = dw;
                m_swsDstH = dh;
            }
            if (sws) {
                QImage img(dw, dh, QImage::Format_RGB888);
                uint8_t* dst[4] = {img.bits(), nullptr, nullptr, nullptr};
                int dstLinesize[4] = {(int)img.bytesPerLine(), 0, 0, 0};
                sws_scale(sws, fr->data, fr->linesize, 0, sh, dst, dstLinesize);
                return img;
            }
        }
        return result;
    }

    const int64_t target = av_rescale_q(
        (int64_t)(targetSec * 1000000.0),
        AVRational{1, 1000000}, st->time_base);

    // Determina se precisa de seek real:
    // Apenas faz seek se:
    // 1) Nunca foi inicializado (m_lastPtsSec < 0)
    // 2) Houve um salto para trás significativo (> 0.5s)
    // 3) Houve um salto para frente grande (> 2.0s)
    const bool needSeek = (m_lastPtsSec < 0.0
                           || targetSec < m_lastPtsSec - 0.5
                           || targetSec > m_lastPtsSec + 2.0);

    if (needSeek) {
        av_seek_frame(fmt, m_stream, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(cc);
        m_lastPtsSec = -1.0;
        if (m_lastFrame)
            av_frame_unref(reinterpret_cast<AVFrame*>(m_lastFrame));
        if (m_nextFrame)
            av_frame_unref(reinterpret_cast<AVFrame*>(m_nextFrame));
        m_lastFrameSec = -1.0;
        m_nextFrameSec = -1.0;
    }

    if (!m_pkt || !m_frame) {
        return result;
    }

    av_packet_unref(m_pkt);
    av_frame_unref(m_frame);

    AVFrame* chosen = nullptr;
    bool atEof = false;
    // Tolerância mínima: cobre apenas ruído de PTS. A seleção é a convenção de
    // EXIBIÇÃO (hold) — o frame mostrado em `targetSec` é o último cujo início
    // (fsec) ainda não passou de `targetSec + tolerance`. Antes selecionávamos
    // "o primeiro frame com fsec >= alvo - 0.4/fps", que aceitava frames que
    // começavam até 40% de um frame ANTES do alvo: no corte, a borda do clipe
    // 2 mostrava o mesmo frame com que o clipe 1 terminava (pedaço do clipe
    // repetido / duplicado) e o vídeo parecia "alguns ms atrasado".
    const double tolerance = (m_fps > 0.0) ? (0.1 / m_fps) : 0.003;

    // Primeiro candidato: o frame que passou do alvo na chamada anterior
    // (decodificado na folga, m_next*). Para o novo alvo ele pode qualificar
    // de novo (sobe para m_lastFrame) ou passar de novo (continua um à frente).
    if (m_nextFrameSec >= 0.0 && m_nextFrame) {
        const double nfSec = m_nextFrameSec;
        if (nfSec <= targetSec + tolerance) {
            keepAsDisplay(reinterpret_cast<AVFrame*>(m_nextFrame), nfSec);
            av_frame_unref(reinterpret_cast<AVFrame*>(m_nextFrame));
            m_nextFrameSec = -1.0;
        } else if (m_lastFrameSec >= 0.0 && m_lastFrame) {
            chosen = reinterpret_cast<AVFrame*>(m_lastFrame);
        } else {
            // Sem quadro anterior: exibe o próprio m_nextFrame (NÃO dá unref —
            // chosen aponta para ele; m_nextFrame continua segurando a referência).
            chosen = reinterpret_cast<AVFrame*>(m_nextFrame);
            m_nextFrameSec = -1.0;
        }
        m_lastPtsSec = nfSec; // progresso do codec
    }

    while (!chosen && !atEof) {
        while (avcodec_receive_frame(cc, m_frame) == 0) {
            const double fsec = frameSec(m_frame);

            if (fsec <= targetSec + tolerance) {
                keepAsDisplay(m_frame, fsec);
                m_lastPtsSec = fsec;
                av_frame_unref(m_frame);
                continue;
            }

            if (m_lastFrameSec >= 0.0 && m_lastFrame) {
                chosen = reinterpret_cast<AVFrame*>(m_lastFrame);
            } else {
                chosen = m_frame;
                m_nextFrameSec = -1.0;
            }
            m_lastPtsSec = fsec;
            if (chosen != m_frame) {
                AVFrame* nf = reinterpret_cast<AVFrame*>(m_nextFrame);
                if (!nf) {
                    nf = av_frame_alloc();
                    m_nextFrame = nf;
                }
                if (nf) {
                    av_frame_unref(nf);
                    av_frame_ref(nf, m_frame);
                    m_nextFrameSec = fsec;
                }
                av_frame_unref(m_frame);
            }
            break;
        }

        if (chosen) break;

        const int r = av_read_frame(fmt, m_pkt);
        if (r < 0) {
            avcodec_send_packet(cc, nullptr);
            while (avcodec_receive_frame(cc, m_frame) == 0) {
                const double fsec = frameSec(m_frame);
                if (fsec > targetSec + tolerance)
                    break;
                keepAsDisplay(m_frame, fsec);
                m_lastPtsSec = fsec;
                av_frame_unref(m_frame);
            }
            atEof = true;
            break;
        }
        if (m_pkt->stream_index == m_stream) {
            avcodec_send_packet(cc, m_pkt);
        }
        av_packet_unref(m_pkt);
    }

    if (!chosen && m_lastFrameSec >= 0.0 && m_lastFrame)
        chosen = reinterpret_cast<AVFrame*>(m_lastFrame);

    if (chosen) {
        const int sw = chosen->width > 0 ? chosen->width : cc->width;
        const int sh = chosen->height > 0 ? chosen->height : cc->height;
        if (sw > 0 && sh > 0) {
            int dw = sw, dh = sh;
            if (maxWidth > 0 && sw > maxWidth) {
                dw = maxWidth;
                dh = qMax(2, sh * dw / sw);
                if (dh & 1) dh += 1;
            }
            SwsContext* sws = reinterpret_cast<SwsContext*>(m_sws);
            const AVPixelFormat srcFmt = static_cast<AVPixelFormat>(chosen->format);
            if (!sws || m_swsSrcW != sw || m_swsSrcH != sh || m_swsSrcFmt != srcFmt
                || m_swsDstW != dw || m_swsDstH != dh) {
                if (sws) sws_freeContext(sws);
                sws = sws_getContext(sw, sh, srcFmt, dw, dh, AV_PIX_FMT_RGB24,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                m_sws = sws;
                m_swsSrcW = sw;
                m_swsSrcH = sh;
                m_swsSrcFmt = static_cast<int>(srcFmt);
                m_swsDstW = dw;
                m_swsDstH = dh;
            }
            if (sws) {
                QImage img(dw, dh, QImage::Format_RGB888);
                uint8_t* dst[4] = {img.bits(), nullptr, nullptr, nullptr};
                int dstLinesize[4] = {(int)img.bytesPerLine(), 0, 0, 0};
                sws_scale(sws, chosen->data, chosen->linesize, 0, sh, dst, dstLinesize);
                result = img; // compartilhamento implícito, sem cópia
            }
        }
    }

    av_frame_unref(m_frame);
    av_packet_unref(m_pkt);
    return result;
}

void FFmpegDecoder::releaseBuffers() {
    QMutexLocker locker(&m_mutex);
    if (m_codec)
        avcodec_flush_buffers(reinterpret_cast<AVCodecContext*>(m_codec));
    // Imagem estática: mantém o frame cacheado — re-decodificar exigiria
    // seek, que não funciona para arquivos de frame único (JPEG, PNG…).
    if (!m_isImage) {
        if (m_lastFrame) {
            av_frame_unref(reinterpret_cast<AVFrame*>(m_lastFrame));
            av_frame_free(reinterpret_cast<AVFrame**>(&m_lastFrame));
            m_lastFrame = nullptr;
        }
        if (m_nextFrame) {
            av_frame_unref(reinterpret_cast<AVFrame*>(m_nextFrame));
            av_frame_free(reinterpret_cast<AVFrame**>(&m_nextFrame));
            m_nextFrame = nullptr;
        }
        m_lastFrameSec = -1.0;
        m_nextFrameSec = -1.0;
        m_lastPtsSec = -1.0;
    }
}

bool FFmpegDecoder::hasAudio() const {
    QMutexLocker locker(&m_audioMutex);
    return m_aCtx != nullptr && m_audioStream >= 0;
}

void FFmpegDecoder::seekAudio(double seconds) {
    QMutexLocker locker(&m_audioMutex);
    if (audioDbg()) qDebug() << "[audio] seek" << seconds << "stream=" << m_audioStream;
    if (!m_aCtx || m_audioStream < 0) return;
    AVFormatContext* afmt = static_cast<AVFormatContext*>(m_aCtx);
    AVCodecContext* acc = static_cast<AVCodecContext*>(m_aCodec);
    const AVStream* ast = afmt->streams[m_audioStream];
    const int64_t target = av_rescale_q(
        (int64_t)(seconds * 1000000.0),
        AVRational{1, 1000000}, ast->time_base);
    av_seek_frame(afmt, m_audioStream, target, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(acc);
}

int FFmpegDecoder::decodeAudio(void* outBuf, int maxBytes) {
    QMutexLocker locker(&m_audioMutex);
    if (!m_aCtx || m_audioStream < 0 || !outBuf || maxBytes <= 0) return 0;
    if (!m_audioPkt || !m_audioFrame) return 0;

    AVFormatContext* afmt = static_cast<AVFormatContext*>(m_aCtx);
    AVCodecContext* acc = static_cast<AVCodecContext*>(m_aCodec);
    SwrContext* swr = static_cast<SwrContext*>(m_swr);

    const int bytesPerSample = 2 * m_audioOutCh; // S16 interleaved
    uint8_t* out = static_cast<uint8_t*>(outBuf);
    int produced = 0;
    bool readEof = false;

    av_packet_unref(m_audioPkt);
    av_frame_unref(m_audioFrame);

    while (produced < maxBytes) {
        const int recv = avcodec_receive_frame(acc, m_audioFrame);
        if (recv == AVERROR(EAGAIN)) {
            // Precisa de mais pacotes (ou flush no fim do arquivo).
            if (readEof) {
                avcodec_send_packet(acc, nullptr);
                continue;
            }
            const int r = av_read_frame(afmt, m_audioPkt);
            if (r < 0) {
                readEof = true;
                avcodec_send_packet(acc, nullptr);
                continue;
            }
            if (m_audioPkt->stream_index == m_audioStream)
                avcodec_send_packet(acc, m_audioPkt);
            av_packet_unref(m_audioPkt);
            continue;
        }
        if (recv == AVERROR_EOF) {
            // Drena o resampler (atraso interno no fim).
            const int cap = (maxBytes - produced) / bytesPerSample;
            if (cap <= 0) break;
            const int got = swr_convert(swr, &out, cap, nullptr, 0);
            if (got <= 0) break;
            produced += got * bytesPerSample;
            out += got * bytesPerSample;
            break;
        }
        if (recv < 0) break;

        const int cap = (maxBytes - produced) / bytesPerSample;
        if (cap <= 0) break;
        const int got = swr_convert(swr, &out, cap,
                                    (const uint8_t**)m_audioFrame->extended_data,
                                    m_audioFrame->nb_samples);
        if (got < 0) break;
        if (got == 0) continue;
        produced += got * bytesPerSample;
        out += got * bytesPerSample;
    }

    av_frame_unref(m_audioFrame);
    av_packet_unref(m_audioPkt);
    if (audioDbg() && produced == 0)
        qDebug() << "[audio] decodeAudio -> 0 bytes (SILÊNCIO)";
    return produced;
}
