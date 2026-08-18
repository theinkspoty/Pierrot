// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QPoint>
#include <QObject>
#include <QEvent>

class QMouseEvent;
class QLabel;

class QLabel;
class QPushButton;

// Barra de título personalizada de uma janela (sem as bordas do sistema).
// Mostra o título à esquerda e os botões de janela à direita (minimizar,
// maximizar/restaurar, fechar). Arrastar na barra move a janela.
class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(const QString& title, QWidget* window, QWidget* parent = nullptr);
    void setTitle(const QString& title);

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    void maximizeRestore();

    QWidget* m_window;
    QLabel* m_title = nullptr;
    QPushButton* m_minBtn = nullptr;
    QPushButton* m_maxBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QPoint m_dragOffset;
    bool m_dragging = false;
};
