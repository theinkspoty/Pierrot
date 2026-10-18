// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>

#include <algorithm>
#include <cmath>

// Helpers compartilhados dos monitores (Program e Source): desenho dos glifos
// monocromáticos do transporte e formatação de timecode. Antes replicados no
// PreviewWidget e no PreviewMonitor, agora vivem só aqui.

// Timecode estilo Vegas/DaVinci: HH:MM:SS:FF (frames, no fps informado).
inline QString fmtTimecode(double t, double fps) {
    const int fr = std::max(1, (int)std::llround(fps));
    int ff = (int)std::llround(t * fr);
    const int h = ff / (3600 * fr); ff %= 3600 * fr;
    const int m = ff / (60 * fr);   ff %= 60 * fr;
    const int s = ff / fr;          ff %= fr;
    return QString("%1:%2:%3:%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ff, 2, 10, QLatin1Char('0'));
}

// Glifos monocromáticos do transporte do monitor — os mesmos desenhos do
enum class MonitorGlyph { StepBack, Play, Pause, StepFwd, Loop, Safe, Fullscreen };

inline QIcon makeMonitorIcon(MonitorGlyph g, const QColor& color) {
    auto draw = [g](QPainter& p, const QColor& c) {
        QPen pen(c, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        switch (g) {
        case MonitorGlyph::StepBack:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF t; t << QPointF(4.5, 8.0) << QPointF(10.5, 3.5) << QPointF(10.5, 12.5);
              p.drawPolygon(t); }
            p.drawRect(QRectF(11.4, 4.0, 2.2, 8.0));
            break;
        case MonitorGlyph::Play:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF t; t << QPointF(5.5, 3.8) << QPointF(13.0, 8.0) << QPointF(5.5, 12.2);
              p.drawPolygon(t); }
            break;
        case MonitorGlyph::Pause:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRect(QRectF(5.0, 3.8, 2.6, 8.4));
            p.drawRect(QRectF(9.0, 3.8, 2.6, 8.4));
            break;
        case MonitorGlyph::StepFwd:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF t; t << QPointF(11.5, 8.0) << QPointF(5.5, 3.5) << QPointF(5.5, 12.5);
              p.drawPolygon(t); }
            p.drawRect(QRectF(2.4, 4.0, 2.2, 8.0));
            break;
        case MonitorGlyph::Loop:
            p.drawArc(QRectF(3.0, 3.5, 10.0, 9.0), -40 * 16, -240 * 16);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF h; h << QPointF(12.4, 3.0) << QPointF(13.6, 7.2) << QPointF(15.6, 5.4);
              p.drawPolygon(h); }
            break;
        case MonitorGlyph::Safe:
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(3.0, 3.0, 10.0, 10.0));
            p.drawLine(QPointF(8.0, 3.0), QPointF(8.0, 3.9));
            p.drawLine(QPointF(8.0, 12.1), QPointF(8.0, 13.0));
            p.drawLine(QPointF(3.0, 8.0), QPointF(3.9, 8.0));
            p.drawLine(QPointF(12.1, 8.0), QPointF(13.0, 8.0));
            break;
        case MonitorGlyph::Fullscreen:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF c1; c1 << QPointF(3.0, 7.0) << QPointF(3.0, 3.0) << QPointF(7.0, 3.0); p.drawPolygon(c1); }
            { QPolygonF c2; c2 << QPointF(13.0, 3.0) << QPointF(9.0, 3.0) << QPointF(13.0, 7.0); p.drawPolygon(c2); }
            { QPolygonF c3; c3 << QPointF(3.0, 9.0) << QPointF(3.0, 13.0) << QPointF(7.0, 13.0); p.drawPolygon(c3); }
            { QPolygonF c4; c4 << QPointF(13.0, 13.0) << QPointF(13.0, 9.0) << QPointF(9.0, 13.0); p.drawPolygon(c4); }
            break;
        }
    };
    QIcon icon;
    for (int s : {1, 2}) {
        QPixmap pm(16 * s, 16 * s);
        pm.setDevicePixelRatio(s);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        draw(p, color);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}