// Pierrot — editor de vídeo estilo Vegas Pro
//
// Copyright (C) 2026 Pierrot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

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
