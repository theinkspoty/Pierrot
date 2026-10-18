// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QObject>

class QEvent;
class QMouseEvent;

// Grava cada clique do mouse num arquivo (ver ClickLogger.cpp) para
// depuração: identifica o widget clicado, o texto do botão/menu, a posição
// e a cadeia de widgets. Ativado pela variável de ambiente PIERROT_CLICK_LOG
// (valor = caminho do arquivo de log; padrão /tmp/pierrot-clicks.log).
class ClickLogger : public QObject {
    Q_OBJECT
public:
    static void install();
protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
private:
    explicit ClickLogger(QObject* parent = nullptr);
};
