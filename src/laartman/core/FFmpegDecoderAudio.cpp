// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Caminho de áudio: picos (waveforms), seek e decode de amostras S16/48k.
// Extraído de FFmpegDecoder.cpp (fragmentação do motor).

#include "FFmpegDecoder.h"
#include "FFmpegDecoderPriv.h"
#include "laartman/io/Demuxer.h"

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

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <QDebug>

static double sampleAt(const AVFrame* frame, const AVCodecContext* cc, int channel, int index) {
    const AVSampleFormat sf = static_cast<AVSampleFormat>(frame->format);
    const int planar = av_sample_fmt_is_planar(sf);
    int channels = frame->ch_layout.nb_channels;
    if (channels <= 0) channels = cc->ch_layout.nb_channels;
    if (index < 0 || index >= frame->nb_samples || channel < 0 || channel >= channels)
        return 0.0;
    const int bytes = av_get_bytes_per_sample(sf);
    if (bytes <= 0) return 0.0;
    const uint8_t* p;
    if (planar) {
        if (!frame->data[channel]) return 0.0;
        p = frame->data[channel] + (size_t)index * bytes;
    } else {
        if (!frame->data[0]) return 0.0;
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

FFmpegAudioPeaks FFmpegDecoder::audioPeaks(const QString& filePath, int bucketsPerSecond,
                                           int streamIndex) {
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

    // Valida/sanitiza o índice do stream desejado (ou usa o melhor stream).
    // streamIndex é a k-ésima FAIXA DE ÁUDIO (0 = primeira), não o índice
    // absoluto no arquivo.
    int idx = resolveAudioStream(fmt, streamIndex);
    if (idx < 0 && streamIndex >= 0) {
        avformat_close_input(&fmt);
        return result;
    }
    if (idx < 0) {
        idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
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
    if (avcodec_parameters_to_context(cc, fmt->streams[idx]->codecpar) < 0) {
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return result;
    }
    if (avcodec_open2(cc, codec, nullptr) < 0) {
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return result;
    }

    const double timeBase = av_q2d(fmt->streams[idx]->time_base);
    const double sampleRate = cc->sample_rate > 0 ? cc->sample_rate : 44100.0;

    // Duração estimada (para limitar a resolução em arquivos longos).
    double duration = 0.0;
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
        duration = fmt->duration / (double)AV_TIME_BASE;
    else if (fmt->streams[idx]->duration != AV_NOPTS_VALUE
             && fmt->streams[idx]->duration > 0)
        duration = fmt->streams[idx]->duration * timeBase;

    // Teto de buckets: a 1000 buckets/s, 3 h dariam ~10,8 M buckets × 2 floats
    // ≈ 86 MB — e o vetor só cresce. Acima do teto reduz a RESOLUÇÃO efetiva
    // (reportada em result.bucketsPerSecond, que é o que o pintor da timeline
    // usa para mapear tempo→bucket) em vez de estourar a memória.
    constexpr int kMaxBuckets = 2'000'000; // ~16 MB (min+máx)
    int effectiveBps = bucketsPerSecond;
    if (duration > 0.0 && duration * effectiveBps > kMaxBuckets)
        effectiveBps = std::max(1, (int)(kMaxBuckets / duration));
    if (duration > 0.0) {
        const int est = (int)std::min<double>(duration * effectiveBps + 1.0,
                                              (double)kMaxBuckets + 2.0);
        result.min.reserve(est);
        result.max.reserve(est);
    }

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

                    int bucket = (int)std::llround(t * effectiveBps);
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
    result.bucketsPerSecond = effectiveBps;
    return result;
}


bool FFmpegDecoder::hasAudio() const {
    QMutexLocker locker(&m_audioMutex);
    return m_aCtx != nullptr && m_audioStream >= 0;
}

// m_audioOutCh é escrito por open() sob m_audioMutex; a UI lê isto de outra
// thread ao montar o mixer, então o getter precisa do mesmo lock — sem ele a
// leitura corre junto com a escrita (data race) enquanto um arquivo é aberto.
int FFmpegDecoder::audioChannels() const {
    QMutexLocker locker(&m_audioMutex);
    return m_audioOutCh;
}

bool FFmpegDecoder::audioFinished() const {
    QMutexLocker locker(&m_audioMutex);
    return m_audioFinished;
}

// Adquire os locks do caminho de áudio na ordem fixa m_mutex → m_audioMutex
// (a mesma de open()/close(): sem AB-BA). Caso comum — vídeo com contexto
// PRÓPRIO via PacketReader — só m_audioMutex: vídeo e áudio decodificam em
// paralelo, sem esperar um ao outro (Fase 3 do plano v0.4: sem cadeia de
// mutex cruzada). Compartilhado (imagem / reader falhou) pega os dois.
//
// O flag m_videoSharesDemuxer só muda em open()/freeAllLocked(), que exigem
// os DOIS locks — então, com audio preso, ele está estável. A revalidação
// abaixo cobre o caso de open() terminar ENTRE a leitura do flag e o lock do
// áudio: sem ela, o decodeAudio poderia correr sem m_mutex enquanto o frameAt
// de uma imagem lia o demuxer compartilhado.
void FFmpegDecoder::lockAudio(std::unique_lock<QMutex>& video,
                              std::unique_lock<QMutex>& audio) const {
    for (;;) {
        video = std::unique_lock<QMutex>(m_mutex, std::defer_lock);
        if (m_videoSharesDemuxer.load(std::memory_order_acquire))
            video.lock();
        audio = std::unique_lock<QMutex>(m_audioMutex);
        if (!video.owns_lock()
            && m_videoSharesDemuxer.load(std::memory_order_acquire)) {
            audio.unlock();
            continue; // estado mudou: reavalia com o flag novo
        }
        return;
    }
}

void FFmpegDecoder::seekAudio(double seconds) {
    std::unique_lock<QMutex> vlock, alock;
    lockAudio(vlock, alock);
    if (audioDbg()) qDebug() << "[audio] seek" << seconds << "stream=" << m_audioStream;
    if (!m_aCtx || m_audioStream < 0) return;
    AVFormatContext* afmt = static_cast<AVFormatContext*>(m_aCtx);
    AVCodecContext* acc = static_cast<AVCodecContext*>(m_aCodec);
    const AVStream* ast = afmt->streams[m_audioStream];
    const int64_t target = av_rescale_q(
        (int64_t)(seconds * 1000000.0),
        AVRational{1, 1000000}, ast->time_base);
    const int seekErr = m_demuxer->seek(m_audioStream, target, AVSEEK_FLAG_BACKWARD);
    if (seekErr < 0) {
        qWarning() << "av_seek_frame failed for audio:" << seekErr;
    }
    avcodec_flush_buffers(acc);
    if (m_swr) swr_init(static_cast<SwrContext*>(m_swr));
    // Fallback para streams sem PTS confiável; o descarte principal é por PTS
    // (m_audioSeekTargetSec), preciso independentemente de onde o seek pousou.
    m_audioSkipFrames = 30;
    m_audioSeekTargetSec = seconds;
    m_audioFinished = false;
    if (audioDbg())
        qDebug() << "[audio] seekAudio: target=" << seconds << "stream=" << m_audioStream;
}

int FFmpegDecoder::decodeAudio(void* outBuf, int maxBytes) {
    std::unique_lock<QMutex> vlock, alock;
    lockAudio(vlock, alock);
    if (!m_aCtx || m_audioStream < 0 || !outBuf || maxBytes <= 0) return 0;
    if (!m_audioPkt || !m_audioFrame) return 0;

    AVFormatContext* afmt = static_cast<AVFormatContext*>(m_aCtx);
    AVCodecContext* acc = static_cast<AVCodecContext*>(m_aCodec);
    SwrContext* swr = static_cast<SwrContext*>(m_swr);
    const AVStream* ast = afmt->streams[m_audioStream];

    const int bytesPerSample = 2 * m_audioOutCh; // S16 interleaved
    uint8_t* out = static_cast<uint8_t*>(outBuf);
    int produced = 0;
    bool readEof = false;

    av_packet_unref(m_audioPkt);
    av_frame_unref(m_audioFrame);

    while (produced < maxBytes) {
        if (audioDbg() && m_audioSkipFrames > 0 && produced == 0)
            qDebug() << "[audio] decodeAudio: entrou no loop com skipFrames=" << m_audioSkipFrames;
        const int recv = avcodec_receive_frame(acc, m_audioFrame);
        if (recv == AVERROR(EAGAIN)) {
            // Precisa de mais pacotes (ou flush no fim do arquivo).
            if (readEof) {
                avcodec_send_packet(acc, nullptr);
                continue;
            }
            const int r = m_demuxer->readPacket(m_audioPkt);
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
            // Drena o atraso interno do resampler no fim do arquivo. Só marca
            // FIM quando o swr PARAR de devolver amostras: marcar cedo fazia o
            // consumidor (AudioConformCache) largar a cauda que ainda restava
            // no resampler quando o buffer pedido era menor que o atraso.
            const int cap = (maxBytes - produced) / bytesPerSample;
            if (cap <= 0) break;
            const int got = swr_convert(swr, &out, cap, nullptr, 0);
            if (got > 0) {
                produced += got * bytesPerSample;
                out += got * bytesPerSample;
            } else {
                m_audioFinished = true; // resampler esvaziou de vez
            }
            break;
        }
        if (recv < 0) break;

        // Após seek, descarta frames até ATINGIR o alvo por PTS. O
        // AVSEEK_FLAG_BACKWARD pousa no ponto de sincronia ANTERIOR ao alvo —
        // em arquivos longos isso pode ficar a vários segundos (às vezes perto
        // do início do arquivo). A contagem cega de N frames não cobria essa
        // distância: o áudio retomava antes da posição pedida e o relógio do
        // preview saltava para trás. Aqui o PTS manda; sem PTS confiável cai
        // para o fallback de contagem.
        if (m_audioSeekTargetSec >= 0.0) {
            int64_t pts = m_audioFrame->pts;
            if (pts == AV_NOPTS_VALUE) pts = m_audioFrame->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE) {
                const double fsec = pts * av_q2d(ast->time_base);
                // Fallback encerrado: o PTS assumiu o controle do descarte.
                m_audioSkipFrames = 0;
                if (fsec < m_audioSeekTargetSec - 0.002) {
                    av_frame_unref(m_audioFrame);
                    continue;
                }
                m_audioSeekTargetSec = -1.0; // alvo alcançado
            }
        }
        if (m_audioSkipFrames > 0) {
            if (audioDbg())
                qDebug() << "[audio] decodeAudio: skipFrame restantes=" << m_audioSkipFrames - 1;
            m_audioSkipFrames--;
            av_frame_unref(m_audioFrame);
            continue;
        }

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
