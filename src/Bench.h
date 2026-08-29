// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// ─────────────────────────────────────────────────────────────────────────────
// Harness de stress: mede com números reais se o motor aguenta cortes/frames/
// seeks. Modo de linha de comando (sem GUI) acionado com:
//     pierrot --bench <projeto.pjrt>
// Imprime um relatório com os custos de abrir+decodificar cada corte, o custo
// por frame em streaming contínuo, seeks precisos, o custo de áudio por corte
// e o consumo de memória (VmHWM). Útil para validar 200 cortes/4K e para
// regressões depois de mexer no decoder/pipeline.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QString>
#include <QStringList>

class QApplication;

int runBench(QApplication& app, const QString& projectPath);
int runStress(QApplication& app, const QString& outputPath, const QStringList& mediaFiles);