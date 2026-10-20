// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Helpers internos do FFmpegDecoder, compartilhados pelos TUs do motor
// (FFmpegDecoder.cpp, FFmpegDecoderOpen.cpp, FFmpegDecoderAudio.cpp,
// FFmpegDecoderProbe.cpp). NÃO é header público — não incluir de fora de
// src/laartman/.

#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <atomic>

#include <QDebug>
#include <QFileInfo>
#include <QMutex>
#include <QSet>
#include <QSettings>
#include <QString>

// Diagnóstico do caminho de áudio do preview: ligue com PIERROT_AUDIO_DEBUG=1.
inline bool audioDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_AUDIO_DEBUG");
    return on;
}

// Diagnóstico de abertura de arquivo (o custo escondido do corte): ligue com
// PIERROT_PLAY_DEBUG=1 ou PIERROT_PERF_DEBUG=1. Sem isso o open() de 500-800ms
// fica invisível — o seek medido no engine não o enxerga.
inline bool openDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_PLAY_DEBUG")
                        || qEnvironmentVariableIsSet("PIERROT_PERF_DEBUG");
    return on;
}

inline void decoderLogCallback(void*, int, const char*, va_list) {
    // Silencia completamente qualquer log/aviso interno do FFmpeg
}

inline void installLogFilter() {
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
inline bool isImagePath(const QString& filePath) {
    const QString ext = QFileInfo(filePath).suffix().toLower();
    static const QSet<QString> exts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("svg")
    };
    return exts.contains(ext);
}

// Resolve o ÍNDICE ABSOLUTO do k-ésimo stream de áudio do arquivo. O clipe
// guarda `audioStreamIndex` como "k-ésima faixa de áudio" (0 = primeira), mas o
// FFmpeg precisa do índice absoluto no arquivo (que pode ser 1 se o stream 0 é
// vídeo, como na maioria dos MP4). Retorna -1 se não houver k-ésimo áudio.
inline int resolveAudioStream(const AVFormatContext* fmt, int k) {
    if (k < 0) return -1;
    int audioIdx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++audioIdx;
            if (audioIdx == k) return (int)i;
        }
    }
    return -1;
}

// Aceleração de hardware: desligada pela configuração "hwDecode" (Configurações
// → Qualidade e Timeline → Desempenho), pela variável PIERROT_GPU=0 ou pelo
// auto-cura: se o driver entregar quadros vazios repetidamente (formato
// incomum/10-bit, transferência falha, etc.), o Pierrot desativa a GPU sozinho
// para a sessão — o vídeo continua, em software.
inline std::atomic<bool> s_hwBroken{false};
inline std::atomic<int> s_hwFailSeq{0};

inline bool hwDisabled() {
    if (qEnvironmentVariableIsSet("PIERROT_GPU")
        && (QString::fromLatin1(qgetenv("PIERROT_GPU")).trimmed() == QLatin1String("0")))
        return true;
    static const bool cfgDisabled = !QSettings().value("hwDecode", true).toBool();
    return cfgDisabled || s_hwBroken;
}

// Dispositivo VAAPI compartilhado pelo processo: os decoders abrem/fecham a
// todo momento (troca de clipes, prefetch), e criar um device por open custaria
// vários ms no corte. Um AVBufferRef* contado é criado uma única vez (refs por
// decoder via av_buffer_ref). Sem driver/GPU o FFmpeg falha aqui e tudo cai
// para software normalmente.
inline AVBufferRef* vaapiDevice() {
    static AVBufferRef* dev = nullptr;
    static QMutex gHwMutex;
    QMutexLocker l(&gHwMutex);
    if (!dev) {
        AVBufferRef* d = nullptr;
        if (av_hwdevice_ctx_create(&d, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) {
            qDebug() << "[hw] VAAPI indisponível — usando decode por CPU";
            d = nullptr;
        } else {
            qDebug() << "[hw] VAAPI ativo (decodificação por GPU)";
        }
        dev = d;
    }
    return dev;
}

// Callback do AVCodecContext: se o decoder listar AV_PIX_FMT_VAAPI entre os
// formatos aceitos, usa hardware; senão devolve NONE para a tentativa seguinte
// (o open() retenta em software puro).
inline enum AVPixelFormat hwVaapiGetFormat(AVCodecContext* s,
                                           const enum AVPixelFormat* pix_fmts) {
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_VAAPI) return *p;
    return AV_PIX_FMT_NONE;
}

// Threading do decode por SOFTWARE. O padrão é frame-threading (pipeline entre
// quadros, melhor vazão em playback). Slice-threading produz o MESMO quadro
// (decode é determinístico) mas entrega o 1º com menos latência — o gargalo do
// scrub/corte — ao custo de vazão em resoluções altas. O trade-off depende do
// perfil, então fica por switch para medir no Bench: PIERROT_DEC_THREAD=slice.
inline int decThreadType() {
    static const bool slice =
        QString::fromLatin1(qgetenv("PIERROT_DEC_THREAD")).trimmed()
            .compare(QLatin1String("slice"), Qt::CaseInsensitive) == 0;
    return slice ? FF_THREAD_SLICE : FF_THREAD_FRAME;
}

// AV_CODEC_FLAG2_FAST libera atalhos "não conformes" do decoder (mais rápido,
// robustez/qualidade levemente menores em fluxos quebrados). Desligado por
// padrão para não afetar a exportação; ligue com PIERROT_DEC_FAST=1 para medir.
inline bool fastDecode() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_DEC_FAST");
    return on;
}

