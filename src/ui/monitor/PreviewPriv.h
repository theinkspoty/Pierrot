// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Helpers compartilhados entre os vários .cpp de PreviewWidget (o widget é
// fragmentado por responsabilidade: Render, Playback, Audio, Frames, Ready,
// Effects, Overlay). Viviam como static/anon-namespace no .cpp único — com o
// corte eles atravessariam unidades de tradução, então viram inline aqui.

#pragma once

#include "colombina/models/Project.h"

#include <algorithm>
#include <cmath>

// fps do projeto (30 como fallback) — usado por seek, decodificação e
// prefetch para converter entre tempo da timeline e quadros.
inline double projFps(const Project* p) {
    return (p && p->fps > 0.0) ? p->fps : 30.0;
}

// ── Qualidades de preview do combo/chip ─────────────────────────────────
struct PreviewQOpt { int width; const char* label; const char* code; };
inline const PreviewQOpt kPreviewQualities[] = {
    {360,  "360p",  "360p"},
    {480,  "480p",  "480p"},
    {720,  "720p",  "720p"},
    {1080, "1080p", "1080p"},
    {3840, "4K",    "4K"},
};

// Índices do combo de zoom do monitor (0 = Ajustar à área). A faixa em si
// (playhead/loop/marcadores) vive em ui/monitor/MonitorScrubBar.*.
inline const double kMonitorZooms[] = {0.0, 0.25, 0.50, 0.75, 1.0, 1.5, 2.0};

// Crop (pan/crop) do clipe no instante `rel` da timeline.
inline void clipCrop(const Clip& c, double rel,
                     int& cL, int& cR, int& cT, int& cB) {
    cL = (int)std::lround(std::clamp(kfValue(c.kfCropL, c.cropL, rel), 0.0, 0.9) * 1000.0);
    cR = (int)std::lround(std::clamp(kfValue(c.kfCropR, c.cropR, rel), 0.0, 0.9) * 1000.0);
    cT = (int)std::lround(std::clamp(kfValue(c.kfCropT, c.cropT, rel), 0.0, 0.9) * 1000.0);
    cB = (int)std::lround(std::clamp(kfValue(c.kfCropB, c.cropB, rel), 0.0, 0.9) * 1000.0);
}
