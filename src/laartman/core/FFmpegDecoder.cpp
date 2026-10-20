// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Núcleo do decoder: ciclo de vida curto (ctor/dtor), acesso ao estado e o
// caminho de VÍDEO — seek + decode progressivo, convenção de hold, lookahead,
// cancelamento cooperativo, LRU de frames e métricas.
//
// O motor foi fragmentado em TUs (ver FFmpegDecoderPriv.h):
//   FFmpegDecoderOpen.cpp   — open/close/freeAllLocked + caminho VAAPI
//   FFmpegDecoderAudio.cpp  — picos, seek/decode de áudio
//   FFmpegDecoderProbe.cpp  — probe/isAnimatedImage

#include "FFmpegDecoder.h"
#include "FFmpegDecoderPriv.h"
#include "laartman/io/Demuxer.h"
#include "laartman/io/PacketReader.h"

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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#include <QDebug>
#include <QMutex>
#include <QThread>

FFmpegDecoder::FFmpegDecoder() {
    installLogFilter();
    m_demuxer = std::make_unique<Demuxer>();
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

bool FFmpegDecoder::usesHardware() const {
    QMutexLocker vlock(&m_mutex);
    return m_hw;
}

int64_t FFmpegDecoder::frameCacheIndex(double t) const {
    if (m_vfr || m_fps <= 0.0) {
        // VFR/sem fps: grade fina de ms (não assume espaçamento 1/fps).
        return (int64_t)std::llround(t * 1000.0);
    }
    return (int64_t)std::floor(t * m_fps);
}


// ── Estatísticas globais ───────────────────────────────────────────────────

std::atomic<quint64> FFmpegDecoder::s_lastWorkNs{0};
std::atomic<quint64> FFmpegDecoder::s_workTotalNs{0};
quint64 FFmpegDecoder::s_discardCount = 0;
quint64 FFmpegDecoder::s_lastDiscard = 0;
quint64 FFmpegDecoder::s_seekCount = 0;
quint64 FFmpegDecoder::s_seekTotalNs = 0;
quint64 FFmpegDecoder::s_lastSeekNs = 0;
std::atomic<quint64> FFmpegDecoder::s_openCount{0};
std::atomic<quint64> FFmpegDecoder::s_openTotalNs{0};
std::atomic<quint64> FFmpegDecoder::s_lastOpenNs{0};

QImage FFmpegDecoder::frameAt(double seconds, int maxWidth, const std::atomic<bool>* cancel) {
    QMutexLocker locker(&m_mutex);
    // Mede o tempo REAL gasto dentro de frameAt (excluindo a espera pelo
    // lock, que pertence à "fila"). s_lastWorkNs era lida pelo profiler
    // (PreviewWidget → decWorkNs/dwork) mas nunca era escrita: o dwork do
    // overlay ficava sempre 0. RAII cobre todos os retornos do frameAt.
    struct WorkTimer {
        WorkTimer() : t0(std::chrono::steady_clock::now()) {}
        ~WorkTimer() {
            const qint64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
            const quint64 u = ns > 0 ? quint64(ns) : 0;
            s_lastWorkNs.store(u, std::memory_order_relaxed);
            s_workTotalNs.fetch_add(u, std::memory_order_relaxed);
        }
        std::chrono::steady_clock::time_point t0;
    } workTimer;
    QImage result;
    if (!m_ctx || m_stream < 0) return result;

    // Cancelamento cooperativo (Fase 3): a UI marca o token quando um alvo novo
    // supera este decode (scrub, seek, ou corte para outro clipe). Abortamos só
    // em pontos seguros — antes do seek e no topo dos laços — para não deixar
    // estado pela metade. O trabalho já feito (m_lastFrame, m_lastPtsSec,
    // posição do codec) é mantido: o próximo frameAt aproveita ou re-seeka.
    auto cancelled = [cancel]() {
        return cancel && cancel->load(std::memory_order_relaxed);
    };

    // Pacotes de vídeo: PacketReader (thread de I/O à frente, contexto
    // próprio — Fases 3+4) quando presente; fallback síncrono no demuxer
    // compartilhado (imagens / abertura do reader falhou). O cancel chega ao
    // read() do reader, que aborta a espera em ~20 ms — antes o disco lento
    // travava o decode até o pacote chegar.
    auto readVideoPacket = [&]() -> int {
        if (m_reader) return m_reader->read(m_pkt, cancel);
        return m_demuxer->readPacket(m_pkt);
    };

    // Núcleo SEM cache: o LRU de quadros é política de L4 (FrameCache, na
    // colombina). frameAt() sempre decodifica; quem quiser evitar re-decode
    // consulta o FrameCache antes de chamar.
    double targetSec = m_isImage ? 0.0 : std::max(0.0, seconds);
    // GIF/WebP animado: embrulha o tempo pedido sobre a duração real do
    // arquivo, repetindo a animação para preencher o clipe (paridade com o
    // export, que usa -ignore_loop 0 no demuxer).
    if (m_isLoopGif && m_gifDurationSec > 0.0)
        targetSec = std::fmod(targetSec, m_gifDurationSec);

    AVCodecContext* cc = static_cast<AVCodecContext*>(m_codec);
    // time_base do vídeo cacheado no open(): o caminho de vídeo não toca no
    // demuxer compartilhado (o áudio decodifica nele sem m_mutex — Fase 3).
    const AVRational vtb { m_vtbNum, m_vtbDen };

    // fsec de um quadro no tempo do stream (fallback: o próprio alvo).
    // Prefere pts; sem ele, best_effort_timestamp (que já considera DTS para
    // reordenar pacotes com B-frames). DTS/PTS negativos (arquivos reais com
    // timestamps quebrados) são firmados em 0: tempo negativo não faz sentido
    // para exibição e quebraria a comparação com o alvo.
    auto frameSec = [&](const AVFrame* fr) -> double {
        int64_t pts = fr->pts;
        if (pts == AV_NOPTS_VALUE)
            pts = fr->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) return targetSec;
        const double sec = pts * av_q2d(vtb);
        return sec < 0.0 ? 0.0 : sec;
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
        // Stills via QImage — NÃO via libavcodec. O caminho FFmpeg de imagem
        // estourava SIGSEGV em imagens grandes (issues #3/#4/#5: PNG/WebP com
        // frame-threading e também sem threads). O Qt carrega via plugins de
        // imagem, sem avcodec no caminho. Fallback: se o plugin não abrir,
        // cai no decode FFmpeg antigo abaixo (thread_count=1).
        if (m_imageCache.isNull()) {
            QImage src(m_source);
            if (!src.isNull())
                m_imageCache = src.convertToFormat(QImage::Format_RGBA8888);
        }
        if (!m_imageCache.isNull()) {
            int sw = m_imageCache.width();
            int sh = m_imageCache.height();
            int dw = sw, dh = sh;
            if (maxWidth > 0 && sw > maxWidth) {
                dw = maxWidth;
                dh = qMax(2, sh * dw / sw);
                if (dh & 1) dh += 1;
            }
            if (dw != sw || dh != sh)
                return m_imageCache.scaled(dw, dh, Qt::IgnoreAspectRatio,
                                           Qt::FastTransformation);
            return m_imageCache;
        }
        // Fallback FFmpeg (ex.: SVG sem plugin Qt correspondente).
        if (!cc || !m_pkt || !m_frame) return result;
        if (!m_lastFrame || m_lastFrameSec < 0.0) {
            av_packet_unref(m_pkt);
            av_frame_unref(m_frame);
            // Estado limpo: um decode anterior que falhou no meio pode ter
            // deixado o codec com erro pendente (issue #3: retry sem flush).
            avcodec_flush_buffers(cc);
            bool got = false;
            int guard = 0; // teto de pacotes: image2 entrega 1; gifs/webp animados, N
            while (!got && guard++ < 4096) {
                while (avcodec_receive_frame(cc, m_frame) == 0) {
                    keepAsDisplay(m_frame, frameSec(m_frame));
                    av_frame_unref(m_frame);
                    got = true;
                    break; // imagens têm um único quadro
                }
                if (got) break;
                const int r = m_demuxer->readPacket(m_pkt);
                if (r < 0) {
                    avcodec_send_packet(cc, nullptr); // drena o codec no fim
                    if (avcodec_receive_frame(cc, m_frame) == 0) {
                        keepAsDisplay(m_frame, frameSec(m_frame));
                        av_frame_unref(m_frame);
                        got = true;
                    }
                    break;
                }
                if (m_pkt->stream_index == m_stream) {
                    const int sr = avcodec_send_packet(cc, m_pkt);
                    if (sr < 0 && sr != AVERROR(EAGAIN)) {
                        av_packet_unref(m_pkt);
                        break; // codec em erro: não insiste (evita loop/crash)
                    }
                }
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
                sws = sws_getContext(sw, sh, srcFmt, dw, dh, AV_PIX_FMT_RGBA,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                m_sws = sws;
                m_swsSrcW = sw;
                m_swsSrcH = sh;
                m_swsSrcFmt = static_cast<int>(srcFmt);
                m_swsDstW = dw;
                m_swsDstH = dh;
            }
            if (sws)
                return swsScaleToRgba(sws, fr->data, fr->linesize, sh, dw, dh);
        }
        return result;
    }

    const int64_t target = av_rescale_q(
        (int64_t)(targetSec * 1000000.0),
        AVRational{1, 1000000}, vtb);

    // Determina se precisa de seek real:
    // Apenas faz seek se:
    // 1) Nunca foi inicializado (m_lastPtsSec < 0)
    // 2) Houve um salto para trás significativo (> 0.5s)
    // 3) Houve um salto para frente grande (> 2.0s)
    bool needSeek = (m_lastPtsSec < 0.0
                     || targetSec < m_lastPtsSec - 0.5
                     || targetSec > m_lastPtsSec + 2.0);
    // GIF animado com loop: o fmod() volta para 0 no final da duração. Não
    // fazer seek+flush neste wrap evita resetar o canvas do decoder (disposal)
    // e o seek binário sem índice (custo alto). Assim o decoder avança
    // sequencialmente pelo ciclo, muito mais suave no preview.
    if (needSeek && m_isLoopGif && m_gifDurationSec > 0.1) {
        const double diffBack = m_lastPtsSec - targetSec;
        const double wrap = m_gifDurationSec;
        // Wrap: último ~ 0,5s voltou para 0
        if (diffBack >= (wrap - 0.6) && diffBack <= (wrap + 0.6)) {
            needSeek = false;
        }
    }

    if (cancelled()) return result; // desiste antes do seek (caro)

    if (needSeek) {
        const qint64 seekT0 = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        // Seek no contexto do vídeo: PacketReader (descarta a fila e
        // reposiciona o produtor) ou o demuxer compartilhado (fallback).
        const int seekErr = m_reader
            ? m_reader->seek(m_stream, target, AVSEEK_FLAG_BACKWARD)
            : m_demuxer->seek(m_stream, target, AVSEEK_FLAG_BACKWARD);
        if (seekErr < 0) {
            qWarning() << "av_seek_frame failed for video:" << seekErr;
        }
        // Para GIF em loop, não faz flush completo dos buffers de decodificação
        // ao dar wrap (evita resetar o canvas com disposal). Caso contrário,
        // executa flush padrão.
        if (!(m_isLoopGif && m_gifDurationSec > 0.1)) {
            avcodec_flush_buffers(cc);
        } else {
            // Flush mínimo: evita lixo, mas preserva estado interno do decoder GIF
            // quando possível. Libavcodec não expõe opção melhor; optamos por não
            // forçar flush no wrap para reduzir o custo do loop.
        }
        m_lastPtsSec = -1.0;
        if (m_lastFrame)
            av_frame_unref(reinterpret_cast<AVFrame*>(m_lastFrame));
        if (m_nextFrame)
            av_frame_unref(reinterpret_cast<AVFrame*>(m_nextFrame));
        m_lastFrameSec = -1.0;
        m_nextFrameSec = -1.0;
        s_lastSeekNs = quint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - seekT0);
        s_seekTotalNs += s_lastSeekNs;
        ++s_seekCount;
    }

    if (!m_pkt || !m_frame) {
        return result;
    }

    av_packet_unref(m_pkt);
    av_frame_unref(m_frame);

    AVFrame* chosen = nullptr;
    bool atEof = false;
    // Decodificação por hardware: o frame decodificado mora na GPU (formato
    // VAAPI) e não pode ser lido cru pelo sws. `hostFrame` transfere para um
    // buffer NV12 em RAM (m_swFrame) e devolve o frame "host". O fsec é
    // calculado ANTES da transferência — av_hwframe_transfer_data não copia
    // metadados de PTS, e a lógica de exibição (hold) depende dele.
    const AVPixelFormat hwPM = static_cast<AVPixelFormat>(m_hwPixFmt);
    auto hostFrame = [&](AVFrame* hwf, double& fsec) -> AVFrame* {
        fsec = frameSec(hwf);
        if (!(m_hw && hwf->format == hwPM)) return hwf;
        AVFrame* sw = reinterpret_cast<AVFrame*>(m_swFrame);
        if (!sw) {
            sw = av_frame_alloc();
            m_swFrame = sw;
        }
        if (!sw) return nullptr;
        av_frame_unref(sw);
        AVHWFramesContext* hfc = hwf->hw_frames_ctx
            ? reinterpret_cast<AVHWFramesContext*>(hwf->hw_frames_ctx->data) : nullptr;
        const AVPixelFormat swFmt = hfc ? static_cast<AVPixelFormat>(hfc->sw_format)
                                        : AV_PIX_FMT_NV12;
        sw->format = swFmt;
        sw->width = hwf->width;
        sw->height = hwf->height;
        if (av_hwframe_transfer_data(sw, hwf, 0) < 0) {
            av_frame_unref(sw);
            return nullptr;
        }
        return sw;
    };
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

    quint64 discarded = 0;
    s_lastDiscard = 0;
    bool aborted = false;
    while (!chosen && !atEof) {
        if (cancelled()) { aborted = true; break; }
        while (avcodec_receive_frame(cc, m_frame) == 0) {
            ++discarded;
            double fsec = 0.0;
            AVFrame* src = hostFrame(m_frame, fsec);
            if (!src) {
                av_frame_unref(m_frame);
                continue; // transferência hw→sw falhou; descarta o quadro
            }
            const bool usingScratch = (src != m_frame);

            if (fsec <= targetSec + tolerance) {
                ++discarded;
                keepAsDisplay(src, fsec);
                m_lastPtsSec = fsec;
                av_frame_unref(m_frame);
                if (usingScratch) av_frame_unref(reinterpret_cast<AVFrame*>(m_swFrame));
                continue;
            }

            if (m_lastFrameSec >= 0.0 && m_lastFrame) {
                chosen = reinterpret_cast<AVFrame*>(m_lastFrame);
            } else {
                chosen = src;
                m_nextFrameSec = -1.0;
            }
            m_lastPtsSec = fsec;
            if (chosen != src) {
                AVFrame* nf = reinterpret_cast<AVFrame*>(m_nextFrame);
                if (!nf) {
                    nf = av_frame_alloc();
                    m_nextFrame = nf;
                }
                if (nf) {
                    av_frame_unref(nf);
                    av_frame_ref(nf, src);
                    m_nextFrameSec = fsec;
                }
                av_frame_unref(m_frame);
                if (usingScratch) av_frame_unref(reinterpret_cast<AVFrame*>(m_swFrame));
            }
            break;
        }

        if (chosen) break;

        const int r = readVideoPacket();
        // Cancel durante a espera do reader (disco lento): aborta sem drenar
        // o codec — o progresso preservado serve para o próximo pedido.
        if (r == AVERROR_EXIT && cancelled()) { aborted = true; break; }
        if (r < 0) {
            avcodec_send_packet(cc, nullptr);
            while (avcodec_receive_frame(cc, m_frame) == 0) {
                if (cancelled()) { aborted = true; break; }
                double fsec = 0.0;
                AVFrame* src = hostFrame(m_frame, fsec);
                if (!src) {
                    av_frame_unref(m_frame);
                    continue;
                }
                const bool usingScratch = (src != m_frame);
                if (fsec > targetSec + tolerance) {
                    av_frame_unref(m_frame);
                    if (usingScratch) av_frame_unref(reinterpret_cast<AVFrame*>(m_swFrame));
                    break;
                }
                keepAsDisplay(src, fsec);
                m_lastPtsSec = fsec;
                av_frame_unref(m_frame);
                if (usingScratch) av_frame_unref(reinterpret_cast<AVFrame*>(m_swFrame));
            }
            atEof = true;
            break;
        }
        if (m_pkt->stream_index == m_stream) {
            avcodec_send_packet(cc, m_pkt);
        }
        av_packet_unref(m_pkt);
    }

    if (aborted) {
        av_frame_unref(m_frame);
        av_packet_unref(m_pkt);
        return result; // QImage nula: pedido cancelado por alvo mais novo
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
                sws = sws_getContext(sw, sh, srcFmt, dw, dh, AV_PIX_FMT_RGBA,
                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                m_sws = sws;
                m_swsSrcW = sw;
                m_swsSrcH = sh;
                m_swsSrcFmt = static_cast<int>(srcFmt);
                m_swsDstW = dw;
                m_swsDstH = dh;
            }
            if (sws) {
                // swsScaleToRgba cobre o estouro de 32 B do kernel AVX2 do
                // swscale em larguras não múltiplas de 16 (issue #11).
                result = swsScaleToRgba(sws, chosen->data, chosen->linesize,
                                        sh, dw, dh);
            }
        }
    }

    av_frame_unref(m_frame);
    av_packet_unref(m_pkt);

    s_lastDiscard = discarded;
    s_discardCount += discarded;

    // Auto-cura: hardware decodificou quadro(s) mas nada virou imagem (driver
    // com formato quebrado/transferência falha) — ou não decodificou NENHUM
    // quadro (VAAPI da NVIDIA abre o device mas não decodifica H.264). Nos dois
    // casos o resultado é nulo com hw armado; após algumas ocorrências seguidas,
    // desativa o VAAPI para a sessão — o vídeo volta por software.
    if (m_hw && result.isNull()) {
        if (++s_hwFailSeq >= 4) {
            s_hwBroken = true;
            qWarning() << "[hw] decodificação VAAPI produzindo quadros vazios — "
                          "desativada nesta sessão (use as Configurações para controlar)";
        }
    } else if (m_hw) {
        s_hwFailSeq = 0;
    }

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
