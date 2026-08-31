// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaWidget.h"
#include <QPainter>
#include <QPainterPath>

// ═══════════════════════════════════════════════════════════════════════
// Painel vertical fixo à esquerda: Câmera no topo + elementos (clips/fotos)
// empilhados abaixo. Clique numa linha seleciona; a transformação acontece
// no canvas.
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::drawLayerList(QPainter& p) {
    MesaComposition* mc = currentMesa();
    if (!mc) return;

    const QVector<Track*> tracks = mesaTracks();
    const int panelW = kLayerPanelW;
    const int barH = 22;      // header bar do widget (desenhado depois, por cima)
    const int titleH = 22;    // título interno do painel
    const int rowH = 24;
    m_layerListRect = QRect(0, barH, panelW, qMax(1, height() - barH));
    m_layerZones.clear();

    // Fundo do painel (a coluna inteira)
    p.fillRect(m_layerListRect, QColor(28, 28, 31));
    p.setPen(QPen(QColor(50, 50, 54), 1));
    p.drawLine(panelW - 1, barH, panelW - 1, m_layerListRect.bottom());

    // Título
    QFont hf = p.font();
    hf.setPointSizeF(8);
    hf.setBold(true);
    p.setFont(hf);
    p.setPen(QColor(150, 150, 150));
    p.drawText(QRect(8, m_layerListRect.top(), panelW - 16, titleH),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("MESA"));

    QFont rf = p.font();
    rf.setPointSizeF(8);
    rf.setBold(false);
    p.setFont(rf);

    // Linhas visíveis: 0 = Câmera, depois camadas do topo do empilhamento.
    // Se estourar a altura da coluna, as excedentes ficam fora da área
    // clicável/desenhada (rolagem fica como follow-up).
    const int maxRows = qMax(0, (m_layerListRect.height() - titleH) / rowH);
    const int rowCount = qMin((int)tracks.size() + 1, maxRows);
    m_layerListRowCount = rowCount;
    if (rowCount <= 0) return;

    const int left = m_layerListRect.left();
    const int top = m_layerListRect.top() + titleH;

    // ── Linha fixa da Câmera (row 0) ──
    {
        const int y = top;
        const bool sel = m_cameraSelected;
        if (sel)
            p.fillRect(left + 1, y, panelW - 2, rowH, QColor(50, 80, 130));
        else
            p.fillRect(left + 1, y, panelW - 2, rowH, QColor(38, 38, 38));

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(70, 170, 210));
        p.drawRoundedRect(left + 24, y + 7, 10, 10, 2, 2);

        p.setPen(sel ? QColor(240, 240, 240) : QColor(175, 175, 175));
        p.drawText(QRect(left + 40, y, panelW - 40 - 70, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Câmera"));
        const double zi = kfValue(mc->kfCamZoom, mc->camZoom, qMax(0.0, m_playheadTime));
        p.setPen(QColor(120, 185, 205));
        p.drawText(QRect(left + panelW - 66, y, 52, rowH),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(zi, 'f', 2));

        m_layerZones.append({ {}, {}, {}, QRect(left + 2, y, panelW - 4, rowH), -1 });
    }

    for (int i = 0; i + 1 < rowCount; ++i) {
        const int rowIdx = tracks.size() - 1 - i;
        const Track* t = tracks[rowIdx];
        const bool sel = hasSelection(rowIdx);
        const int y = top + (i + 1) * rowH;

        // Selection highlight
        if (sel)
            p.fillRect(left + 1, y, panelW - 2, rowH, QColor(50, 80, 130));
        else if (rowIdx % 2 == 0)
            p.fillRect(left + 1, y, panelW - 2, rowH, QColor(38, 38, 38));
        else
            p.fillRect(left + 1, y, panelW - 2, rowH, QColor(32, 32, 32));

        // Eye (olho): t, clique alterna mesaHidden
        const QRect eyeR(left + 4, y + 5, 14, 14);
        drawEyeIcon(p, eyeR, !t->mesaHidden);

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
        p.drawRoundedRect(left + 24, y + 7, 10, 10, 2, 2);

        // Layer name (recomeça depois do chip MB)
        p.setPen(sel ? QColor(240, 240, 240) : QColor(170, 170, 170));
        p.drawText(QRect(left + 58, y, panelW - 58 - 46, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, t->name.left(10));

        // Chip "MB" de motion blur por camada (Vegas: allow motion blur).
        // Só faz sentido com o blur global ligado; clique alterna o flag da
        // camada (azul = borra, cinza riscado = fica fixa nas sub-passadas).
        if (mc->motionBlur) {
            const QRect mbR(left + 36, y + 4, 20, 16);
            const bool on = t->mesaMotionBlur;
            p.setPen(QPen(on ? QColor(110, 190, 255) : QColor(110, 110, 110), 1));
            p.setBrush(on ? QColor(70, 150, 220, 70) : QColor(255, 255, 255, 10));
            p.drawRoundedRect(mbR, 3, 3);
            QFont mbf = p.font();
            mbf.setPointSizeF(6);
            p.setFont(mbf);
            p.setPen(on ? QColor(150, 210, 255) : QColor(120, 120, 120));
            p.drawText(mbR, Qt::AlignCenter, QStringLiteral("MB"));
            p.setFont(rf);
            if (!on) {
                p.setPen(QPen(QColor(120, 120, 120), 1));
                p.drawLine(mbR.left() + 2, mbR.top() + mbR.height() / 2,
                           mbR.right() - 2, mbR.top() + mbR.height() / 2);
            }
        }

        // Blend mode abreviado (ex.: "N","A","M","S","O"). Clique direito
        // na linha troca o modo.
        if (t->blendMode != QStringLiteral("normal")) {
            p.setPen(QColor(255, 170, 60));
            p.drawText(QRect(left + panelW - 58, y, 22, rowH),
                       Qt::AlignRight | Qt::AlignVCenter, blendShortName(t->blendMode));
        }

        // Lock (cadeado): clique alterna mesaLocked
        const QRect lockR(left + panelW - 30, y + 4, 16, 16);
        drawLockIcon(p, lockR, t->mesaLocked);

        const QRect mbZone(mc->motionBlur
            ? QRect(left + 36, y + 4, 20, 16) : QRect());
        m_layerZones.append({ eyeR, lockR, mbZone, QRect(left + 2, y, panelW - 4, rowH), rowIdx });
    }
}

void MesaWidget::drawEyeIcon(QPainter& p, const QRect& r, bool visible) const {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(r.center());
    if (visible) {
        p.setPen(QPen(QColor(200, 200, 200), 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(-5.5, -3.5, 11, 7));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(200, 200, 200));
        p.drawEllipse(QPointF(0, 0), 1.6, 1.6);
    } else {
        p.setPen(QPen(QColor(130, 130, 130), 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(-5.5, -3.5, 11, 7));
        p.setPen(QPen(QColor(150, 150, 150), 1.2));
        p.drawLine(QPointF(-6, 0), QPointF(6, 0));
    }
    p.restore();
}

void MesaWidget::drawLockIcon(QPainter& p, const QRect& r, bool locked) const {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(r.center());
    const QColor c = locked ? QColor(255, 180, 60) : QColor(120, 120, 120);
    p.setPen(QPen(c, 1.3));
    p.setBrush(Qt::NoBrush);
    // Alça do cadeado (semicírculo): só desenhado quando trancado
    if (locked) {
        QPainterPath shackle;
        shackle.moveTo(-3.5, -2);
        shackle.arcTo(QRectF(-3.5, -6, 7, 8), 0, 180);
        p.drawPath(shackle);
    }
    p.drawRoundedRect(QRectF(-4.5, -2, 9, 7), 1, 1);
    p.setPen(Qt::NoPen);
    p.setBrush(locked ? QColor(255, 180, 60) : Qt::NoBrush);
    p.drawEllipse(QPointF(0, 0.5), 1.2, 1.2);
    p.restore();
}

QString MesaWidget::blendShortName(const QString& blend) const {
    if (blend == QStringLiteral("add")) return QStringLiteral("A");
    if (blend == QStringLiteral("multiply")) return QStringLiteral("M");
    if (blend == QStringLiteral("screen")) return QStringLiteral("S");
    if (blend == QStringLiteral("overlay")) return QStringLiteral("O");
    if (blend == QStringLiteral("softlight")) return QStringLiteral("SL");
    if (blend == QStringLiteral("difference")) return QStringLiteral("D");
    return QStringLiteral("N");
}
