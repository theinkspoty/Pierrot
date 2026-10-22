// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QHash>
#include <QString>
#include <QTranslator>

// ── Tradutor embutido (inglês) ────────────────────────────────
// Tabela estática gerada direto no .cpp: o build NÃO precisa de
// lupdate/lrelease/qttools — funciona com qualquer toolchain que
// já compila o projeto.
//
// A chave é apenas o texto original (sourceText), sem contexto: o
// mesmo texto traduz igual em qualquer classe, e tr(variável)
// (tabelas de atalhos, presets etc.) funciona porque a busca é em
// tempo de execução.

const QHash<QString, QString>& enTranslationTable();

class EnTranslator final : public QTranslator {
public:
    using QTranslator::QTranslator;

    // Retorna a tradução ou vazio (→ o Qt usa o texto original).
    QString translate(const char* context, const char* sourceText,
                      const char* disambiguation = nullptr,
                      int n = -1) const override;
};
