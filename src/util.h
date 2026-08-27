// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QString>
#include <QStringList>
#include <QFileInfo>

inline bool isImageFile(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    static const QStringList exts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("svg")
    };
    return exts.contains(ext);
}
