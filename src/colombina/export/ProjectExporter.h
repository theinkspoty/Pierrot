// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QStringList>
#include <functional>
#include "colombina/models/Project.h"

class Frei0rPluginManager;

struct ExportSettings {
    QString outputPath;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    enum Format { MP4 = 0, MKV, WEBM, GIF };
    Format format = MP4;
    int crf = 18;              // 0–51 (0 = lossless, 51 = pior qualidade)
    int videoBitrateKbps = 0;  // 0 = usar CRF (VBR), >0 = usar -b:v
    int audioBitrateKbps = 192; // bit rate do áudio em kbps
};

namespace ProjectExporter {
// Callback de progresso/atrito da montagem do comando (CPUs: pré-render das
// bandas Mesa). Recebe o percentual 0..100 e retorna true para continuar,
// false para cancelar. Opcional (null = sem progresso/cancelamento).
using Progress = std::function<bool(int pct)>;
// `frei0rManager` (opcional) fornece metadados dos plugins para gerar
// filtros `frei0r=` no filter_complex (paridade com o preview).
QStringList buildCommand(const Project& project, const ExportSettings& settings,
                         QString* error = nullptr, const Progress& progress = nullptr,
                         const Frei0rPluginManager* frei0rManager = nullptr);
}
