// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "laartman/Decoder.h"

#include "laartman/core/FFmpegDecoder.h"

// A fachada é deliberadamente fina: cada método é uma tradução 1:1 para o motor
// atual. Nenhuma decisão de política mora aqui. Quando as fases 3–6 fatiarem o
// FFmpegDecoder em L0–L2, só o corpo destes métodos muda — a API não.

struct Decoder::Impl {
    FFmpegDecoder engine;
};

Decoder::Decoder() : d(new Impl) {}
Decoder::~Decoder() { delete d; }

MediaInfo Decoder::probe(const QString& path) {
    const FFmpegMediaInfo m = FFmpegDecoder::probe(path);
    MediaInfo out;
    out.duration = m.duration;
    out.width = m.width;
    out.height = m.height;
    out.fps = m.fps;
    out.hasVideo = m.hasVideo;
    out.hasAudio = m.hasAudio;
    out.audioStreams = m.audioStreams;
    return out;
}

bool Decoder::isAnimatedImage(const QString& path) {
    return FFmpegDecoder::isAnimatedImage(path);
}

bool Decoder::open(const QString& path, const DecodeOptions& opt) {
    // hardwareAllowed é decisão da política injetada (L1 recebe, não escolhe).
    // Precisa ser aplicado ANTES do open, contrato do motor.
    d->engine.setHardwareDecodeAllowed(opt.hardwareAllowed);
    return d->engine.open(path, opt.audioStream);
}

void Decoder::close() { d->engine.close(); }

bool Decoder::isOpen() const { return d->engine.isOpen(); }

QString Decoder::source() const { return d->engine.source(); }

double Decoder::fps() const { return d->engine.fps(); }

QImage Decoder::frame(double tAbsoluteSec, int maxWidth,
                      const std::atomic<bool>* cancel) {
    return d->engine.frameAt(tAbsoluteSec, maxWidth, cancel);
}

bool Decoder::seekAudio(double tAbsoluteSec) {
    d->engine.seekAudio(tAbsoluteSec);
    return true;
}

int Decoder::readPcm(void* out, int maxBytes) {
    return d->engine.decodeAudio(out, maxBytes);
}

bool Decoder::audioFinished() const { return d->engine.audioFinished(); }

bool Decoder::hasAudio() const { return d->engine.hasAudio(); }
