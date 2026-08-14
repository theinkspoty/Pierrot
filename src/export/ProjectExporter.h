// Pierrot — editor de vídeo estilo Vegas Pro
//
// Copyright (C) 2026 Pierrot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

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
