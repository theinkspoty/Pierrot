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
