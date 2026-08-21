// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace TlLog {
// Registra um evento na memória (anel de 64 linhas). Sempre ligado, custo
// desprezível — não escreve em disco.
void note(const QString& msg);

// Despeja o histórico recente em ~/Pierrot-timeline-debug.txt com o motivo.
// Chamado automaticamente quando a agulha salta para trás ou a view colapsa,
// para capturar a sequência exata que precedeu o sintoma.
void dump(const QString& reason);
}
