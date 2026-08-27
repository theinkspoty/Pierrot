// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaWidget.h"
#include <QPainter>

// ═══════════════════════════════════════════════════════════════════════
// Layer list flutuante (AE-style panel)
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::drawLayerList(QPainter& p) {
    MesaComposition* mc = currentMesa();
    if (!mc) return;

    const QVector<Track*> tracks = mesaTracks();
    const int rowH = 24;
    const int headerH = 26;
    const int listW = 170;
    const int listH = headerH + tracks.size() * rowH + 4;

    m_layerListRect = QRect(8, height() - listH - 28 - propPanelHeight(), listW, listH);

    // Shadow
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 40));
    p.drawRoundedRect(m_layerListRect.adjusted(2, 2, 2, 2), 4, 4);

    // Background
    p.setPen(QPen(QColor(60, 60, 60), 1));
    p.setBrush(QColor(32, 32, 32, 240));
    p.drawRoundedRect(m_layerListRect, 4, 4);

    // Header
    QFont hf = p.font();
    hf.setPointSizeF(8);
    hf.setBold(true);
    p.setFont(hf);
    p.setPen(QColor(160, 160, 160));
    p.drawText(m_layerListRect.adjusted(8, 4, -8, 0), Qt::AlignLeft | Qt::AlignTop,
               QStringLiteral("LAYERS"));

    QFont rf = p.font();
    rf.setPointSizeF(8);
    rf.setBold(false);
    p.setFont(rf);

    for (int i = 0; i < tracks.size(); ++i) {
        const int rowIdx = tracks.size() - 1 - i;
        const Track* t = tracks[rowIdx];
        const bool sel = (rowIdx == m_selectedIdx);
        const int y = m_layerListRect.top() + headerH + i * rowH;

        // Selection highlight
        if (sel)
            p.fillRect(m_layerListRect.left() + 1, y, listW - 2, rowH, QColor(50, 80, 130));
        else if (rowIdx % 2 == 0)
            p.fillRect(m_layerListRect.left() + 1, y, listW - 2, rowH, QColor(38, 38, 38));

        // Cor do dot: amarela se agrupada na Mesa, senão cor indexada
        QColor dotColor;
        const TrackGroup* tg = t->groupId.isEmpty() ? nullptr : m_project->findGroup(t->groupId);
        if (tg && tg->mesaId == m_mesaId) {
            dotColor = QColor::fromHsv(42, 70, 80);  // amarelo Premier-style
        } else {
            dotColor = QColor::fromHsv((rowIdx * 47 + 180) % 360, 50, 60);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(dotColor);
        p.drawRoundedRect(m_layerListRect.left() + 8, y + 7, 10, 10, 2, 2);

        // Layer name
        p.setPen(sel ? QColor(240, 240, 240) : QColor(170, 170, 170));
        p.drawText(QRect(m_layerListRect.left() + 24, y, listW - 32, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, t->name.left(14));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Painel de propriedades (bottom bar, Premiere-style)
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::drawPropertyPanel(QPainter& p) {
    const int ph = propPanelHeight();
    const int w = width();
    const int y = height() - ph;

    p.fillRect(0, y, w, ph, QColor(35, 35, 35));
    p.setPen(QColor(60, 60, 60));
    p.drawLine(0, y, w, y);

    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);

    const QVector<Track*> tracks = mesaTracks();
    const bool hasSel = m_selectedIdx >= 0 && m_selectedIdx < tracks.size();

    auto drawField = [&](int x, const QString& label, const QString& value, int fieldW = 68) {
        p.setPen(QColor(140, 140, 140));
        p.drawText(QRect(x, y + 2, 36, 14), Qt::AlignLeft | Qt::AlignVCenter, label);
        p.fillRect(x + 36, y + 4, fieldW, 24, QColor(45, 45, 45));
        p.setPen(QColor(210, 210, 210));
        p.drawText(QRect(x + 38, y + 4, fieldW - 4, 24), Qt::AlignLeft | Qt::AlignVCenter, value);
    };

    if (hasSel) {
        Track* t = tracks[m_selectedIdx];
        const double rel = qMax(0.0, m_playheadTime);
        const double tx = kfValue(t->kfMesaX, t->mesaX, rel);
        const double ty = kfValue(t->kfMesaY, t->mesaY, rel);
        const double ts = kfValue(t->kfMesaScaleX, t->mesaScaleX, rel);
        const double tr = kfValue(t->kfMesaRotation, t->mesaRotation, rel);
        const double to = kfValue(t->kfMesaOpacity, t->mesaOpacity, rel);

        p.setPen(QColor(220, 220, 220));
        p.drawText(QRect(8, y + 2, 110, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   t->name.left(14));
        p.setPen(QColor(70, 70, 70));
        p.drawLine(120, y + 6, 120, y + ph - 6);

        drawField(128, "X:",  QString::number(tx, 'f', 1));
        drawField(254, "Y:",  QString::number(ty, 'f', 1));
        drawField(380, "S:",  QString::number(ts, 'f', 2));
        drawField(490, "R:",  QString::number(tr, 'f', 1));
        drawField(600, "O:",  QString::number(to, 'f', 2));
    } else if (MesaComposition* mc = currentMesa()) {
        const double rel = qMax(0.0, m_playheadTime);
        const double cx = kfValue(mc->kfCamX, mc->camX, rel);
        const double cy = kfValue(mc->kfCamY, mc->camY, rel);
        const double cz = kfValue(mc->kfCamZoom, mc->camZoom, rel);
        const double cr = kfValue(mc->kfCamRotation, mc->camRotation, rel);

        p.setPen(QColor(180, 180, 180));
        p.drawText(QRect(8, y + 2, 110, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Câmera"));
        p.setPen(QColor(70, 70, 70));
        p.drawLine(120, y + 6, 120, y + ph - 6);

        drawField(128, "X:", QString::number(cx, 'f', 1));
        drawField(254, "Y:", QString::number(cy, 'f', 1));
        drawField(380, "Z:", QString::number(cz, 'f', 2));
        drawField(510, "R:", QString::number(cr, 'f', 1));
    } else {
        p.setPen(QColor(90, 90, 90));
        p.drawText(QRect(8, y + 2, w - 16, ph - 4),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Nenhuma Mesa selecionada"));
    }
}
