// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QString>
#include <QStringList>

// Instala handlers de sinais fatais (SIGSEGV, SIGABRT, etc.) que geram um
// relatório de crash com backtrace e informações do sistema, salvos em um
// arquivo. Deve ser chamado cedo no main().
namespace CrashReporter {
void install();
// Caminho do próximo arquivo de relatório de crash.
QString nextReportPath();
// Lista os arquivos de relatório de crash existentes (para avisar o usuário).
QStringList existingReports();
}
