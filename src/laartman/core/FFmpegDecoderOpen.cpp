// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Ciclo de vida do decoder: open/close, escolha do codec, caminho VAAPI com
// fallback em software, e o reset de hardware sob demanda. Extraído de
// FFmpegDecoder.cpp (fragmentação do motor).

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

#include <QDebug>
#include <QFileInfo>
#include <QMutex>
#include <QSettings>
#include <QThread>

bool FFmpegDecoder::open(const QString& filePath, int audioStream) {
    // Segura os DOIS mutexes: o vídeo (frameAt) e o áudio (decodeAudio/
    // seekAudio) rodam em threads diferentes e podem estar em andamento
    // quando abrimos outro arquivo. Liberar contextos sem o lock do áudio
    // causava use-after-free na troca de clipe durante a reprodução.
    QMutexLocker vlock(&m_mutex);
    QMutexLocker alock(&m_audioMutex);
    if (m_ctx && m_source == filePath && m_requestedAudioStream == audioStream)
        return true;
    // Só conta/temporiza abertura FRIA (o atalho acima, arquivo já aberto, sai
    // antes). commitOpen() é chamado em todo retorno real, inclusive falhas.
    const auto openT0 = std::chrono::steady_clock::now();
    auto commitOpen = [&]() {
        const quint64 ns = quint64(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - openT0).count());
        s_lastOpenNs.store(ns);
        s_openTotalNs.fetch_add(ns);
        s_openCount.fetch_add(1);
        if (openDbg())
            qDebug().noquote() << QStringLiteral("[dec] open %1 ms  %2")
                      .arg(ns / 1e6, 0, 'f', 1).arg(filePath);
    };
    freeAllLocked();

    // L0: a abertura/probe do arquivo mora no Demuxer; o decoder só consome o
    // AVFormatContext obtido dele (m_ctx aponta para o contexto do demuxer).
    if (!m_demuxer->open(filePath)) {
        commitOpen();
        return false;
    }
    AVFormatContext* fmt = m_demuxer->formatContext();
    m_ctx = fmt;
    m_source = filePath;
    m_requestedAudioStream = audioStream;
    m_lastPtsSec = -1.0;

    // Vídeo é opcional: arquivos só-áudio (mp3/wav) ainda abrem para o
    // preview tocar. frameAt() devolve imagem vazia quando m_stream < 0.
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (idx >= 0) {
        const AVCodec* codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
        if (codec) {
            AVCodecContext* cc = avcodec_alloc_context3(codec);
            if (cc) {
                if (avcodec_parameters_to_context(cc, fmt->streams[idx]->codecpar) < 0) {
                    avcodec_free_context(&cc);
                    cc = nullptr;
                }
                if (cc) {
                    // Imagem estática (JPEG/PNG/WebP…): SEM frame-threading.
                    // O WebP abre threads av:webp:df e com imagens grandes o
                    // avcodec_send_packet podia estourar SIGSEGV dentro do
                    // libavcodec (issue #3). Single-frame não ganha nada com
                    // thread_count > 1 — só arrisca.
                    const bool imageLike = isImagePath(filePath);
                    cc->thread_type = decThreadType();
                    // Imagem/GIF animado é paletado e single-frame: o VAAPI não
                    // decodifica esses codecs e o send_packet com hw_device_ctx
                    // armado estourava SIGSEGV dentro do libavcodec (mesma classe
                    // da issue #3, que já forçou thread_count=1). Sem hw o
                    // caminho de software trata GIF/WebP sem risco — e não há
                    // ganho de GPU num frame.
                    AVBufferRef* hwDev = (imageLike || hwDisabled() || !m_hwAllowed)
                                             ? nullptr : vaapiDevice();
                    if (hwDev) {
                        cc->hw_device_ctx = av_buffer_ref(hwDev);
                        cc->get_format = &hwVaapiGetFormat;
                        // Frame threading + VAAPI é instável; GPU trabalha em 1 thread.
                        cc->thread_count = 1;
                        // Negocia os FRAMES hw ANTES de abrir: pede NV12 explícito.
                        // Sem isso, drivers entregam o sw_format "nativo" (às vezes
                        // P010/10-bit etc.), que o sws não converte → quadro preto.
                        // Se a GPU não aceitar NV12, av_hwframe_ctx_init falha e a
                        // decodificação cai para software abaixo.
                        AVBufferRef* hwFrames = av_hwframe_ctx_alloc(hwDev);
                        if (hwFrames) {
                            AVHWFramesContext* fctx =
                                reinterpret_cast<AVHWFramesContext*>(hwFrames->data);
                            fctx->format = AV_PIX_FMT_VAAPI;
                            fctx->sw_format = AV_PIX_FMT_NV12;
                            fctx->width = cc->width;
                            fctx->height = cc->height;
                            if (av_hwframe_ctx_init(hwFrames) < 0) {
                                av_buffer_unref(&hwFrames);
                                hwFrames = nullptr;
                            }
                        }
                        if (hwFrames) {
                            cc->hw_frames_ctx = av_buffer_ref(hwFrames);
                            av_buffer_unref(&hwFrames); // cc agora segura a ref
                        } else {
                            // Sem NV12 na GPU: remove o hwaccel e abre em software.
                            av_buffer_unref(&cc->hw_device_ctx);
                            cc->hw_device_ctx = nullptr;
                            cc->get_format = nullptr;
                        }
                    } else {
                        cc->thread_count = imageLike ? 1 : qMin(4, QThread::idealThreadCount());
                        if (fastDecode() && !imageLike) cc->flags2 |= AV_CODEC_FLAG2_FAST;
                    }
                    int openErr = avcodec_open2(cc, codec, nullptr);
                    if (openErr != 0 && hwDev) {
                        // Codec sem hwaccel (ex.: PNG, AV1 sem VAAPI): retenta em
                        // software puro, exatamente como antes desta mudança.
                        avcodec_free_context(&cc);
                        cc = avcodec_alloc_context3(codec);
                        if (cc) {
                            if (avcodec_parameters_to_context(cc, fmt->streams[idx]->codecpar) < 0) {
                                avcodec_free_context(&cc);
                                cc = nullptr;
                            } else {
                                cc->thread_count = imageLike ? 1 : qMin(4, QThread::idealThreadCount());
                                cc->thread_type = decThreadType();
                                if (fastDecode() && !imageLike) cc->flags2 |= AV_CODEC_FLAG2_FAST;
                                openErr = avcodec_open2(cc, codec, nullptr);
                                if (openErr != 0) {
                                    avcodec_free_context(&cc);
                                    cc = nullptr;
                                }
                            }
                        }
                    } else if (openErr != 0) {
                        avcodec_free_context(&cc);
                        cc = nullptr;
                    }
                    if (cc) {
                        m_codec = cc;
                        m_stream = idx;
                        m_hw = hwDev && cc->pix_fmt == AV_PIX_FMT_VAAPI;
                        // O get_format do VAAPI pode ser resolvido só quando o
                        // primeiro frame é decodificado (threading de frames):
                        // cc->pix_fmt sai 0 (YUV420P) mesmo com hw_frames_ctx
                        // armado, e os frames saem AV_PIX_FMT_VAAPI com data[0]
                        // nulo (na GPU). Cair em m_hw=false fazia o hostFrame
                        // pular a transferência hw→NV12 e o sws ler ponteiro de
                        // GPU → quadro preto/nenhum. A presencia de hw_frames_ctx
                        // indica o VAAPI negociado (NV12); pix_fmt==VAAPI cobre
                        // drivers que resolvem no open().
                        m_hw = hwDev
                            && (cc->pix_fmt == AV_PIX_FMT_VAAPI || cc->hw_frames_ctx);
                        m_hwPixFmt = m_hw ? (int)AV_PIX_FMT_VAAPI : -1;
                        const AVStream* st = fmt->streams[idx];
                        // time_base cacheado: o caminho de vídeo não lê mais o
                        // demuxer (o áudio decodifica nele sem m_mutex).
                        if (st->time_base.num > 0 && st->time_base.den > 0) {
                            m_vtbNum = st->time_base.num;
                            m_vtbDen = st->time_base.den;
                        }
                        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
                            const double r = av_q2d(st->avg_frame_rate);
                            if (r > 0.0 && r < 240.0) m_fps = r;
                        }
                        // GIF/WebP animado: às vezes avg_frame_rate vem 0 ou instável.
                        // Se não conseguimos fps, calculamos pela duração/nb_frames para
                        // habilitar o cache LRU e evitar 2 decodes por tick.
                        if (m_fps <= 0.0 && isImagePath(filePath)) {
                            if (st->nb_frames > 1 && fmt->duration > 0) {
                                const double sec = fmt->duration / (double)AV_TIME_BASE;
                                if (sec > 0.0) m_fps = (double)st->nb_frames / sec;
                            }
                            if (m_fps <= 0.0 && st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
                                const double r = av_q2d(st->r_frame_rate);
                                if (r > 0.0 && r < 240.0) m_fps = r;
                            }
                        }
                        // VFR (taxa de quadros variável): o avg_frame_rate é só a
                        // média, então assumir espaçamento constante (1/fps) gera
                        // drift e erra a chave de cache. Detecta-se quando o
                        // r_frame_rate (taxa-base) é bem maior que a média — o
                        // padrão em streams VFR — ou quando a média é
                        // desconhecida mas há vídeo. Nesses casos a matemática de
                        // tempo passa a usar o timestamp real de cada quadro.
                        m_vfr = false;
                        if (!m_isImage) {
                            const bool haveAvg = st->avg_frame_rate.num > 0
                                                 && st->avg_frame_rate.den > 0;
                            const bool haveMax = st->r_frame_rate.num > 0
                                                 && st->r_frame_rate.den > 0;
                            if (!haveAvg) {
                                m_vfr = true;
                            } else if (haveMax) {
                                const double avg = av_q2d(st->avg_frame_rate);
                                const double max = av_q2d(st->r_frame_rate);
                                if (avg > 0.5 && max > avg * 1.5) m_vfr = true;
                            }
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
                        // GIF/WebP animado (extensão de imagem mas NÃO singleFrame,
                        // com duração real > 0): frameAt() embrulha o tempo pedido
                        // com fmod sobre a duração real para repetir a animação
                        // enquanto o clipe durar (paridade com o export, que usa
                        // -ignore_loop 0 para o demuxer loopar os pacotes).
                        if (!m_isImage && isImagePath(filePath)) {
                            m_isLoopGif = true;
                            m_gifDurationSec = (fmt->duration > 0)
                                ? fmt->duration / (double)AV_TIME_BASE : -1.0;
                        } else {
                            m_isLoopGif = false;
                            m_gifDurationSec = -1.0;
                        }
                    }
                } // if (cc) after avcodec_parameters_to_context
            }
        }
    }

    // Stream de áudio: reutiliza o mesmo AVFormatContext já aberto. Se o
    // pedido especifica um stream (múltiplas faixas de áudio), usa exatamente
    // ele (o índice é a k-ésima FAIXA DE ÁUDIO, não o índice absoluto do
    // arquivo — na maioria dos MP4 o áudio é o stream 1); caso contrário o
    // "melhor" stream.
    int aidx = resolveAudioStream(fmt, audioStream);
    // Sem isto o default (audioStream = -1, "melhor stream") nunca abria áudio:
    // resolveAudioStream devolve -1 para k < 0 e a guarda abaixo exigia
    // audioStream >= 0, então o av_find_best_stream ficava inalcançável e
    // hasAudio() ficava sempre falso para quem abre sem índice explícito
    // (MediaCache, ProjectExporter e o Bench em dec.hasAudio()).
    if (aidx < 0) {
        aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
    if (aidx >= 0) {
        const AVCodec* acodec = avcodec_find_decoder(fmt->streams[aidx]->codecpar->codec_id);
        if (acodec) {
            AVCodecContext* acc = avcodec_alloc_context3(acodec);
            if (acc) {
                if (avcodec_parameters_to_context(acc, fmt->streams[aidx]->codecpar) < 0) {
                    avcodec_free_context(&acc);
                } else {
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
                } // else avcodec_parameters_to_context succeeded
            }
        }
    }
    // L0 — leitura de pacotes À FRENTE do vídeo (Fases 3+4 do plano v0.4):
    // o PacketReader abre o arquivo num contexto PRÓPIO e uma thread lê os
    // pacotes do stream de vídeo à frente (buffer limitado). O vídeo deixa de
    // ler o demuxer compartilhado com o áudio — sem cadeia de mutex cruzada e
    // sem que o disco engasgado pare o decode. Imagens (single-frame) ficam no
    // demuxer síncrono: um único pacote não ganha nada com thread e o demuxer
    // image2 é frágil. Se a abertura do reader falhar, o caminho de vídeo cai
    // no demuxer compartilhado (comportamento antigo) — nunca bloqueia o open.
    if (m_codec && m_stream >= 0 && !m_isImage) {
        m_reader = std::make_unique<PacketReader>();
        if (!m_reader->open(filePath, m_stream)) {
            m_reader.reset();
            m_videoSharesDemuxer.store(true, std::memory_order_release);
        }
    } else {
        // Sem vídeo (ou imagem): só o áudio (ou ninguém) lê o demuxer.
        m_videoSharesDemuxer.store(m_isImage, std::memory_order_release);
    }

    commitOpen();
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
    if (m_swFrame) {
        av_frame_unref(reinterpret_cast<AVFrame*>(m_swFrame));
        av_frame_free(reinterpret_cast<AVFrame**>(&m_swFrame));
        m_swFrame = nullptr;
    }
    m_hw = false;
    m_hwPixFmt = -1;
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
    if (m_reader) {
        m_reader->close(); // join da thread de leitura + descarta a fila
        m_reader.reset();
    }
    m_videoSharesDemuxer.store(false, std::memory_order_release);
    if (m_ctx) {
        // L0: o Demuxer é dono do AVFormatContext (m_ctx == m_demuxer.formatContext()).
        m_demuxer->close();
        m_ctx = nullptr;
    }
    m_stream = -1;
    m_vtbNum = 1;
    m_vtbDen = 1000000;
    m_isImage = false;
    m_isLoopGif = false;
    m_vfr = false;
    m_gifDurationSec = -1.0;
    m_imageCache = QImage();
    m_audioStream = -1;
    m_requestedAudioStream = -1;
    m_audioSkipFrames = 0;
    m_audioSeekTargetSec = -1.0;
    m_audioFinished = false;
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


void FFmpegDecoder::setHardwareDecodeAllowed(bool allowed) {
    QMutexLocker vlock(&m_mutex);
    if (m_hwAllowed == allowed) return;
    m_hwAllowed = allowed;
    // Já aberto com hardware? Fecha para que o próximo open() respeite a
    // nova preferência — trocar o device no meio da vida do contexto não é
    // seguro.
    if (m_ctx) freeAllLocked();
}
