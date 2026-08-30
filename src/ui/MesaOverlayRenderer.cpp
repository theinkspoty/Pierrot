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

        m_layerZones.append({ {}, {}, QRect(left + 2, y, panelW - 4, rowH), -1 });
    }

    for (int i = 0; i + 1 < rowCount; ++i) {
        const int rowIdx = tracks.size() - 1 - i;
        const Track* t = tracks[rowIdx];
        const bool sel = (rowIdx == m_selectedIdx);
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

        // Layer name
        p.setPen(sel ? QColor(240, 240, 240) : QColor(170, 170, 170));
        p.drawText(QRect(left + 40, y, panelW - 40 - 46, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, t->name.left(11));

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

        m_layerZones.append({ eyeR, lockR, QRect(left + 2, y, panelW - 4, rowH), rowIdx });
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

// ═══════════════════════════════════════════════════════════════════════
// Painel de propriedades (bottom bar, Premiere-style)
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::drawPropertyPanel(QPainter& p) {
    const int ph = propPanelHeight();
    const int x0 = panelWidth();
    const int w = artRect().width();
    const int y = height() - ph;

    p.fillRect(x0, y, w, ph, QColor(35, 35, 35));
    p.setPen(QColor(60, 60, 60));
    p.drawLine(x0, y, x0 + w, y);

    // Conteúdo deslocado de x0: o painel ocupa só a área à direita da coluna
    // vertical de camadas.
    p.save();
    if (x0 > 0) p.translate(x0, 0);

    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);

    const QVector<Track*> tracks = mesaTracks();
    const bool hasSel = m_selectedIdx >= 0 && m_selectedIdx < tracks.size();
    const double rel = qMax(0.0, m_playheadTime);

    m_propFields.clear();
    // drawField usa `oy` (offset vertical em px) para empilhar duas linhas:
    // Linha 1 = CÂMERA (sempre acessível), Linha 2 = CAMADA selecionada.
    auto drawField = [&](int x, int oy, int kind, const QString& label,
                         const QString& value, int fieldW = 68) {
        const int fy = y + oy;
        p.setPen(QColor(140, 140, 140));
        p.drawText(QRect(x, fy + 2, 36, 14), Qt::AlignLeft | Qt::AlignVCenter, label);
        p.fillRect(x + 36, fy + 4, fieldW, 24, QColor(45, 45, 45));
        p.setPen(QColor(210, 210, 210));
        p.drawText(QRect(x + 38, fy + 4, fieldW - 4, 24), Qt::AlignLeft | Qt::AlignVCenter, value);
        // O rect é guardado em coordenadas do widget (não da área traduzida),
        // para o hit-test do mouse bater.
        m_propFields.append({QRect(x + 36 + x0, fy + 4, fieldW, 24), kind});
    };

    MesaComposition* mc = currentMesa();

    // ── Linha 1: Câmera (sempre visível/editável mesmo com camada selecionada) ──
    if (mc) {
        const double cxi = kfValue(mc->kfCamX, mc->camX, rel);
        const double cyi = kfValue(mc->kfCamY, mc->camY, rel);
        const double czi = kfValue(mc->kfCamZoom, mc->camZoom, rel);
        const double cri = kfValue(mc->kfCamRotation, mc->camRotation, rel);

        p.setPen(QColor(190, 190, 190));
        p.drawText(QRect(8, y + 2, 110, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("CÂMERA"));
        p.setPen(QColor(70, 70, 70));
        p.drawLine(120, y + 6, 120, y + 18 + 6);

        drawField(128, 2, PC_X, "X:", QString::number(cxi, 'f', 1));
        drawField(254, 2, PC_Y, "Y:", QString::number(cyi, 'f', 1));
        drawField(380, 2, PC_Z, "Z:", QString::number(czi, 'f', 2));
        drawField(510, 2, PC_R, "R:", QString::number(cri, 'f', 1));
    } else {
        p.setPen(QColor(90, 90, 90));
        p.drawText(QRect(8, y + 2, w - 16, ph - 4),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Nenhuma Mesa selecionada"));
    }

    // ── Linha 2: Camada selecionada (X Y Escala Rot Opacidade + Âncora) ──
    if (mc) {
        if (hasSel) {
            Track* t = tracks[m_selectedIdx];
            const double tx = kfValue(t->kfMesaX, t->mesaX, rel);
            const double ty = kfValue(t->kfMesaY, t->mesaY, rel);
            const double ts = kfValue(t->kfMesaScaleX, t->mesaScaleX, rel);
            const double tr = kfValue(t->kfMesaRotation, t->mesaRotation, rel);
            const double to = kfValue(t->kfMesaOpacity, t->mesaOpacity, rel);
            const double ax = kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel);
            const double ay = kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel);

            const QString tag = t->mesaLocked ? QStringLiteral("  \xe2\x94\xbb") : QString();
            p.setPen(QColor(220, 220, 220));
            p.drawText(QRect(8, y + 28, 110, 14), Qt::AlignLeft | Qt::AlignVCenter,
                       t->name.left(11) + tag);
            p.setPen(QColor(70, 70, 70));
            p.drawLine(120, y + 32, 120, y + 48);

            drawField(128, 30, PL_X, "X:", QString::number(tx, 'f', 1));
            drawField(254, 30, PL_Y, "Y:", QString::number(ty, 'f', 1));
            drawField(380, 30, PL_S, "S:", QString::number(ts, 'f', 2));
            drawField(490, 30, PL_R, "R:", QString::number(tr, 'f', 1));
            drawField(600, 30, PL_O, "O:", QString::number(to, 'f', 2));

            p.setPen(QColor(70, 70, 70));
            p.drawLine(686, y + 32, 686, y + 48);
            drawField(694, 30, PL_AX, "AX:", QString::number(ax, 'f', 1), 60);
            drawField(780, 30, PL_AY, "AY:", QString::number(ay, 'f', 1), 60);
        } else {
            p.setPen(QColor(90, 90, 90));
            p.drawText(QRect(8, y + 30, w - 16, 14), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("Selecione uma camada na lista (L) ou no canvas"));
        }
    }
    p.restore();
}
