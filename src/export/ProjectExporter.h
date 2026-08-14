// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QStringList>
#include "models/Project.h"

struct ExportSettings {
    QString outputPath;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    enum Format { MP4 = 0, MKV, WEBM };
    Format format = MP4;
};

namespace ProjectExporter {
QStringList buildCommand(const Project& project, const ExportSettings& settings,
                         QString* error = nullptr);
}
