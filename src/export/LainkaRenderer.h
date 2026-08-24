// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Pré-renderizador LAINKA: decodifica frame-a-frame, aplica lainkaApplyFx,
// e codifica para arquivo temporário via ffmpeg pipe.

#pragma once

#include <QObject>
#include <QString>
#include <QImage>
#include <functional>

class Project;

// Callback de progresso: (frameAtual, totalFrames) → bool (false = cancelar).
using LainkaProgressFn = std::function<bool(int, int)>;

class LainkaRenderer : public QObject {
    Q_OBJECT
public:
    // Pré-renderiza um clipe com LAINKA para um arquivo temporário.
    // Retorna o caminho do arquivo temporário (vazio se falhou).
    static QString renderClip(const Project& project,
                              const QString& clipId,
                              int outputFps,
                              const LainkaProgressFn& progress = nullptr,
                              QString* error = nullptr);

    // Pré-renderiza todos os clipes com LAINKA do projeto.
    // Retorna um mapa clipId → arquivo temporário.
    static QHash<QString, QString> renderAll(const Project& project,
                                             int outputFps,
                                             const LainkaProgressFn& progress = nullptr,
                                             QString* error = nullptr);
};
