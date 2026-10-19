// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

// Helpers de diagnóstico e configuração compartilhados pelo caminho do preview
// (PreviewWidget, FrameWorker, BgPrefetchWorker, AudioMixer). Cada flag é lida
// uma vez por processo (comportamento herdado do PreviewWidget.cpp original).

#include <QAudioFormat>
#include <QtGlobal>

// Diagnóstico do caminho de áudio do preview: ligue com PIERROT_AUDIO_DEBUG=1.
inline bool audioDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_AUDIO_DEBUG");
    return on;
}

// Diagnóstico da composição multi-faixa do preview: ligue com PIERROT_COMPOSE_DEBUG=1.
inline bool composeDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_COMPOSE_DEBUG");
    return on;
}

// Diagnóstico do playback (tick/frame/pedidos): ligue com PIERROT_PLAY_DEBUG=1.
// Revela o engasgo do vídeo (áudio perfeito): cadência dos ticks, latência do
// decode, skew do relógio e decodes adicionais por frame (camadas inferiores).
inline bool playDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_PLAY_DEBUG");
    return on;
}

// Formato PCM S16/48 kHz/estéreo do sink do preview. Um só lugar para o
// startAudio e o pré-aquecimento do sink não divergirem (o reuso do sink exige
// a mesma saída, não compara o formato).
inline QAudioFormat previewAudioFormat() {
    QAudioFormat fmt;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    fmt.setSampleFormat(QAudioFormat::Int16);
#else
    fmt.setCodec(QStringLiteral("audio/pcm"));
    fmt.setSampleSize(16);
    fmt.setSampleType(QAudioFormat::SignedInt);
    fmt.setByteOrder(QAudioFormat::LittleEndian);
#endif
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    return fmt;
}

// Capacidade do pool (padrão 6; PIERROT_DECODER_POOL=<n> ajusta). 6 dá folga
// para o main + prefetch em andamento + warm-ahead + quentes ociosos sem
// estourar memória (o warm de um arquivo não enche o cache de ~240MB de cada
// decoder; só o clipe tocado o faz).
inline int decoderPoolCapacity() {
    bool ok = false;
    const int n = qEnvironmentVariableIntValue("PIERROT_DECODER_POOL", &ok);
    return (ok && n >= 2) ? n : 6;
}

// Flush periódico do DPB (desengasga). Padrão 0 = desligado; religue com
// PIERROT_DESENGASGA_MS=<ms> se o engasgo reaparecer em alguma fonte.
//
// Medido numa fonte 1080p60 (H.264 High@4.2, 252 s, 13 Mbps), 3 runs de 252 s:
// com o flush a cada 30 s aparecem 7 travamentos de decode de 114-338 ms,
// exatamente a cada 30 s (26/56/86/116/146/176/206 s); desligado, zero picos
// e p99 de decode idêntico (0,55 ms). A degradação de buffer que o flush existia
// para evitar não se confirma: com o flush desligado o p95 de decode ficou plano
// ao longo dos 4 minutos (0,35 -> 0,37 ms). Baixar de 10s para 30s só dilatou o
// custo, não o reduziu, porque o custo é o re-seek frio que o flush provoca —
// não a frequência. Fica desligado por padrão; se o engasgo reaparecer em
// alguma fonte específica, religue por env var em vez de reintroduzir o padrão.
inline qint64 desengasgaIntervalMs() {
    static const qint64 v = [] {
        bool ok = false;
        const qint64 e = qEnvironmentVariableIntValue("PIERROT_DESENGASGA_MS", &ok);
        return ok && e >= 0 ? e : qint64(0);
    }();
    return v;
}
