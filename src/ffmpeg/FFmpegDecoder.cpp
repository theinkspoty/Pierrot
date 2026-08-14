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

// Diagnóstico do caminho de áudio do preview: ligue com PIERROT_AUDIO_DEBUG=1.
static bool audioDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_AUDIO_DEBUG");
    return on;
}

// Avisos inofensivos do decoder H.264 ao decodificar a partir do meio de um
// GOP (seeking/scrubbing) — só poluem o stderr; erros reais continuam sendo
// impressos.
static bool isDecoderNoise(const char* fmt) {
    if (!fmt) return false;
    return strstr(fmt, "mmco") != nullptr
        || strstr(fmt, "unref short failure") != nullptr;
}

static void decoderLogCallback(void* avcl, int level, const char* fmt, va_list vl) {
    if (isDecoderNoise(fmt)) return;
    av_log_default_callback(avcl, level, fmt, vl);
}

static void installLogFilter() {
    static bool installed = false;
    if (!installed) {
        installed = true;
        av_log_set_callback(decoderLogCallback);
    }
}

FFmpegDecoder::FFmpegDecoder() { installLogFilter(); }
FFmpegDecoder::~FFmpegDecoder() { close(); }

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
            info.hasVideo = true;
            info.width = st->codecpar->width;
            info.height = st->codecpar->height;
            if (info.duration <= 0.0 && st->duration > 0)
                info.duration = st->duration * av_q2d(st->time_base);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            info.hasAudio = true;
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
                if (avcodec_open2(cc, codec, nullptr) == 0) {
                    m_codec = cc;
                    m_stream = idx;
                    const AVStream* st = fmt->streams[idx];
                    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
                        const double r = av_q2d(st->avg_frame_rate);
                        if (r > 0.0 && r < 240.0) m_fps = r;
                    }
                } else {
                    avcodec_free_context(&cc);
                }
            }
        }
    }

    // Stream de áudio em contexto próprio, com resampler para S16/48k/estéreo.
    const int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aidx >= 0) {
        AVFormatContext* afmt = nullptr;
        if (avformat_open_input(&afmt, filePath.toUtf8().constData(), nullptr, nullptr) == 0
            && avformat_find_stream_info(afmt, nullptr) >= 0) {
            const AVCodec* acodec = avcodec_find_decoder(afmt->streams[aidx]->codecpar->codec_id);
            if (acodec) {
                AVCodecContext* acc = avcodec_alloc_context3(acodec);
                if (acc) {
                    avcodec_parameters_to_context(acc, afmt->streams[aidx]->codecpar);
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
                                m_aCtx = afmt;
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
            if (!m_aCodec) avformat_close_input(&afmt);
        } else {
            if (afmt) avformat_close_input(&afmt);
        }
    }
    if (audioDbg())
        qDebug() << "[audio] open" << filePath
                 << "videoStream=" << m_stream
                 << "audioStream=" << m_audioStream
                 << "audioCtx=" << (m_aCtx != nullptr);
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
    if (m_codec) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&m_codec));
        m_codec = nullptr;
    }
    if (m_ctx) {
        avformat_close_input(reinterpret_cast<AVFormatContext**>(&m_ctx));
        m_ctx = nullptr;
    }
    if (m_swr) {
        swr_free(reinterpret_cast<SwrContext**>(&m_swr));
        m_swr = nullptr;
    }
    if (m_aCodec) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&m_aCodec));
        m_aCodec = nullptr;
    }
    if (m_aCtx) {
        avformat_close_input(reinterpret_cast<AVFormatContext**>(&m_aCtx));
        m_aCtx = nullptr;
    }
    m_stream = -1;
    m_audioStream = -1;
    m_source.clear();
    m_lastPtsSec = -1.0;
    m_swsSrcW = m_swsSrcH = 0;
    m_swsDstW = m_swsDstH = 0;
    m_swsSrcFmt = -1;
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

    const int64_t target = av_rescale_q(
        (int64_t)(seconds * 1000000.0),
        AVRational{1, 1000000}, st->time_base);
    const double targetSec = seconds;

    // Playback contínuo: decodifica adiante sem re-seek.
    const double forwardGap = 3.0;
    if (m_lastPtsSec < 0.0 || targetSec < m_lastPtsSec - 0.2
        || targetSec > m_lastPtsSec + forwardGap) {
        av_seek_frame(fmt, m_stream, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(cc);
        m_lastPtsSec = -1.0;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!pkt || !frame) {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        return result;
    }

    AVFrame* chosen = nullptr;
    bool atEof = false;
    while (!chosen) {
        const int r = av_read_frame(fmt, pkt);
        if (r < 0) { atEof = true; break; }
        if (pkt->stream_index == m_stream) {
            if (avcodec_send_packet(cc, pkt) == 0) {
                while (avcodec_receive_frame(cc, frame) == 0) {
                    int64_t pts = frame->pts;
                    if (pts == AV_NOPTS_VALUE)
                        pts = frame->best_effort_timestamp;
                    if (pts != AV_NOPTS_VALUE) {
                        const double fsec = pts * av_q2d(st->time_base);
                        if (fsec >= targetSec) {
                            chosen = frame;
                            break;
                        }
                        AVFrame* lastFrame = reinterpret_cast<AVFrame*>(m_lastFrame);
                        if (!lastFrame) lastFrame = av_frame_alloc();
                        if (lastFrame) {
                            av_frame_unref(lastFrame);
                            av_frame_ref(lastFrame, frame);
                            m_lastFrame = lastFrame;
                        }
                        m_lastPtsSec = fsec;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }
    (void)atEof;

    // Final do arquivo: devolve o último frame decodificado.
    if (!chosen && m_lastFrame)
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
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
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

    av_frame_free(&frame);
    av_packet_free(&pkt);
    return result;
}

void FFmpegDecoder::releaseBuffers() {
    QMutexLocker locker(&m_mutex);
    if (m_codec)
        avcodec_flush_buffers(reinterpret_cast<AVCodecContext*>(m_codec));
    if (m_lastFrame) {
        av_frame_unref(reinterpret_cast<AVFrame*>(m_lastFrame));
        av_frame_free(reinterpret_cast<AVFrame**>(&m_lastFrame));
        m_lastFrame = nullptr;
    }
    m_lastPtsSec = -1.0;
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

    AVFormatContext* afmt = static_cast<AVFormatContext*>(m_aCtx);
    AVCodecContext* acc = static_cast<AVCodecContext*>(m_aCodec);
    SwrContext* swr = static_cast<SwrContext*>(m_swr);

    const int bytesPerSample = 2 * m_audioOutCh; // S16 interleaved
    uint8_t* out = static_cast<uint8_t*>(outBuf);
    int produced = 0;
    bool readEof = false;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!pkt || !frame) {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        return 0;
    }

    while (produced < maxBytes) {
        const int recv = avcodec_receive_frame(acc, frame);
        if (recv == AVERROR(EAGAIN)) {
            // Precisa de mais pacotes (ou flush no fim do arquivo).
            if (readEof) {
                avcodec_send_packet(acc, nullptr);
                continue;
            }
            const int r = av_read_frame(afmt, pkt);
            if (r < 0) {
                readEof = true;
                avcodec_send_packet(acc, nullptr);
                continue;
            }
            if (pkt->stream_index == m_audioStream)
                avcodec_send_packet(acc, pkt);
            av_packet_unref(pkt);
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
                                    (const uint8_t**)frame->extended_data,
                                    frame->nb_samples);
        if (got < 0) break;
        if (got == 0) continue;
        produced += got * bytesPerSample;
        out += got * bytesPerSample;
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (audioDbg() && produced == 0)
        qDebug() << "[audio] decodeAudio -> 0 bytes (SILÊNCIO)";
    return produced;
}
