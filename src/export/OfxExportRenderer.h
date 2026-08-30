// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Pré-renderizador de efeitos OFX para exportação: decodifica frame-a-frame,
// aplica efeitos OFX e escreve em arquivo temporário via ffmpeg pipe.

#pragma once

#include <QString>
#include <QHash>
#include <functional>

class Project;
class OfxPluginManager;

class OfxExportRenderer {
public:
    using Progress = std::function<bool(int current, int total)>;

    // Pré-renderiza um clipe com efeitos OFX para um arquivo temporário.
    // Retorna o caminho do arquivo ou vazio se não precisar pré-renderização.
    static QString renderClip(const Project& project,
                              const QString& clipId,
                              int outputFps,
                              const OfxPluginManager* ofxManager,
                              const Progress& progress = nullptr,
                              QString* error = nullptr);

    // Pré-renderiza todos os clipes com efeitos OFX do projeto.
    // Retorna mapa clipId → caminho do arquivo pré-renderizado.
    static QHash<QString, QString> renderAll(const Project& project,
                                              int outputFps,
                                              const OfxPluginManager* ofxManager,
                                              const Progress& progress = nullptr,
                                              QString* error = nullptr);
};
