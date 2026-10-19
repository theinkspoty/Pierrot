// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/monitor/MonitorScrubBar.h"

#include "colombina/models/Project.h"
#include "ui/core/Theme.h"
#include "ui/monitor/PreviewWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

MonitorScrubBar::MonitorScrubBar(PreviewWidget* owner)
    : QWidget(owner), m_owner(owner) {
    setFixedHeight(14);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral(
        "Arraste para navegar na sequência — picotes coloridos = marcadores"));
}

double MonitorScrubBar::tAt(int x) const {
    const double dur = m_owner->project() ? m_owner->project()->duration() : 0.0;
    if (dur <= 0.0 || width() <= 0) return 0.0;
    return std::clamp(x * dur / width(), 0.0, dur);
}

void MonitorScrubBar::seekAt(const QPoint& pos) {
    m_owner->seek(tAt(pos.x()));
}

void MonitorScrubBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
    m_dragging = true;
    seekAt(e->pos());
}

void MonitorScrubBar::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragging) seekAt(e->pos());
    else QWidget::mouseMoveEvent(e);
}

void MonitorScrubBar::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        seekAt(e->pos());
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void MonitorScrubBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const auto& c = themeColors();
    p.fillRect(rect(), c.base);
    p.setPen(c.trackBorder);
    p.drawLine(0, 0, width(), 0);

    const double dur = m_owner->project() ? m_owner->project()->duration() : 0.0;
    if (dur <= 0.0) return;

    // Trilha.
    const QRectF track(2.0, 4.0, width() - 4.0, 6.0);
    p.setPen(Qt::NoPen);
    QColor trackCol = c.canvasBorder;
    trackCol.setAlpha(120);
    p.setBrush(trackCol);
    p.drawRoundedRect(track, 3, 3);

    // Região de loop (in/out), por cima da trilha.
    if (m_owner->loopEnabled() && m_owner->loopOut() > m_owner->loopIn()
        && m_owner->loopIn() >= 0.0) {
        const double x0 = track.left() + track.width() * (m_owner->loopIn() / dur);
        const double x1 = track.left() + track.width() * (m_owner->loopOut() / dur);
        QColor loopCol = c.accent;
        loopCol.setAlpha(110);
        p.setBrush(loopCol);
        p.drawRoundedRect(QRectF(x0, track.top(), std::max(1.0, x1 - x0), track.height()),
                          3, 3);
    }

    // Picotes dos marcadores: haste fina + cabeça arredondada na cor do marker.
    for (const Marker& mk : m_owner->project()->markers) {
        if (mk.time < 0.0 || mk.time > dur) continue;
        const double mx = track.left() + track.width() * (mk.time / dur);
        p.setPen(Qt::NoPen);
        p.setBrush(mk.color);
        p.drawRoundedRect(QRectF(mx - 1.0, 2.0, 2.0, 10.0), 1, 1);
        p.drawEllipse(QPointF(mx, 3.0), 2.2, 2.2);
    }

    // Playhead: haste fina + ponta.
    const double px =
        track.left() + track.width() * std::clamp(m_owner->playhead() / dur, 0.0, 1.0);
    p.setPen(QPen(c.monitorLabel, 1));
    p.drawLine(QPointF(px, 1.0), QPointF(px, height() - 1.0));
    p.setPen(Qt::NoPen);
    p.setBrush(c.monitorLabel);
    QPolygonF tip;
    tip << QPointF(px - 3.0, 0.0) << QPointF(px + 3.0, 0.0) << QPointF(px, 4.0);
    p.drawPolygon(tip);
}
