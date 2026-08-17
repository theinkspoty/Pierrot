// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>

// Painel docável de Efeitos. Por enquanto é uma janela vazia (placeholder);
// a lista/aplicação de efeitos será construída aqui nas próximas versões.
class EffectsWidget : public QWidget {
    Q_OBJECT
public:
    explicit EffectsWidget(QWidget* parent = nullptr);
};
