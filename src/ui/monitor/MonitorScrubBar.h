// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

// Faixa de scrub do Program Monitor: widget filho de 14px entre o vídeo e a
// barra de transporte — playhead arrastável, região de loop sombreada e
// picotes dos marcadores na cor de cada um (mesma paleta do model). O mouse
// dela não briga com o overlay de máscara do vídeo porque é um widget
// separado. Extraído de PreviewWidget.cpp (F4); lê o dono via getters
// públicos (PlaybackEngine), sem amizade.

#include <QWidget>

class PreviewWidget;

class MonitorScrubBar : public QWidget {
    Q_OBJECT
public:
    explicit MonitorScrubBar(PreviewWidget* owner);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    double tAt(int x) const;
    void seekAt(const QPoint& pos);
    PreviewWidget* m_owner;
    bool m_dragging = false;
};
