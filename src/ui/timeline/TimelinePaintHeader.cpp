// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/timeline/TimelineWidget.h"
#include "colombina/models/Project.h"
#include "ui/dialogs/SettingsDialog.h"
#include "colombina/ffmpeg/MediaCache.h"
#include "colombina/generators.h"
#include "ui/timeline/TlLog.h"
#include "ui/core/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QStyleOptionRubberBand>
#include <QRubberBand>
#include <QStyle>
#include <QFileInfo>
#include <QtMath>
#include <QElapsedTimer>
#include <QFont>
#include <QMenu>
#include <QAction>

#include <algorithm>
#include <cmath>

#include "ui/timeline/TimelinePaintPriv.h"

// ── Geometria dos controles do cabeçalho (desenho e hit-test usam a mesma) ──
// Layout réplica do Premiere (CC 2018+, tema escuro "Main"):
//   • Linha superior de controles (y+2, 18px): recolher, navegação de
//     keyframes (◀ ◆ ▶), sync lock (chip; sem backend), toggle de saída
//     (olho/falante) e, no áudio, os botões M / S / R (gravação de voz).
//   • Abaixo da linha (y+21): o NOME da faixa à esquerda e o CADEADO (lock)
//     à direita, como no Premiere (nome entre os controles e o cadeado).
//   • Áudio ainda tem o VU meter vertical na borda direita (da linha do nome
//     ao fim), como os meters de faixa do Premiere. Vídeo não tem meter.
//   • Faixa recolhida/baixa vira uma tira única: nome + olho/falante + cadeado.
// A largura das alças e o cursor (kResizeHandleH) fecham o layout.

// Cadeado pequeno do cabeçalho (definido abaixo de drawTrackHeader).
static void drawTrackLockIcon(QPainter& p, const QRect& r, bool locked);

// Linha superior: cada slot tem x fixo (a largura total cobre a linha do
// áudio, cujos M/S/R terminam em 148; cadeado e meter ficam na borda direita).
QRect TimelineWidget::headerBtnRect(int y, int slot) const {
    const int yTop = y + 2;
    // Tudo tem de caber em kHeaderW (150): o slot 8 (R) terminava em 164 e
    // vazava para a área de clipes. Layout compacto, com folga de 3px à
    // direita — o VU meter fica na linha de baixo (y+21), sem conflito.
    switch (slot) {
      case 0: return QRect(2,  yTop, 14, 18); // recolher/expandir
      case 1: return QRect(18, yTop, 15, 18); // keyframe anterior
      case 2: return QRect(33, yTop, 15, 18); // adicionar/remover keyframe
      case 3: return QRect(48, yTop, 15, 18); // próximo keyframe
      case 4: return QRect(66, yTop, 15, 18); // sync lock (sem backend)
      case 5: return QRect(84, yTop, 18, 18); // toggle de saída (olho/falante)
      case 6: return QRect(105, yTop, 14, 18);// M (áudio)
      case 7: return QRect(119, yTop, 14, 18);// S (áudio)
      case 8: return QRect(133, yTop, 14, 18);// R (áudio; sem backend)
    }
    return QRect();
}

// Nome da faixa, na linha abaixo dos controles (como o nome no Premiere) —
// também posiciona o editor inline de nome. Reserva o cadeado à direita.
QRect TimelineWidget::headerNameRect(int y) const {
    return QRect(6, y + 21, kHeaderW - 28 - 6, 16);
}

// Cadeado: fim da faixa do nome (borda direita, antes do VU meter do áudio).
QRect TimelineWidget::headerLockRect(int y) const {
    return QRect(kHeaderW - 28, y + 21, 18, 16);
}

// ── Faixa recolhida (tira única): seta + nome + olho/falante + cadeado ──────
QRect TimelineWidget::headerMiniNameRect(int y) const {
    return QRect(18, y + 3, kHeaderW - 64, 16);
}
QRect TimelineWidget::headerMiniToggleRect(int y) const {
    return QRect(kHeaderW - 46, y + 3, 18, 18);
}
QRect TimelineWidget::headerMiniLockRect(int y) const {
    return QRect(kHeaderW - 28, y + 3, 18, 16);
}

// VU meter vertical do áudio, na borda direita (da linha do nome ao fim).
QRect TimelineWidget::headerMeterRect(int y, int rowH) const {
    const int top = y + 21;
    const int bottom = y + rowH - kResizeHandleH;
    if (bottom - top < 8) return QRect();
    return QRect(kHeaderW - 6, top, 4, bottom - top);
}

// Seta de recolher/expandir do cabeçalho (▾ expandida, › recolhida).
static void drawCollapseArrow(QPainter& p, const QRect& r, bool collapsed) {
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(themeColors().iconNormal);
    QPainterPath ar;
    if (collapsed) {
        ar.moveTo(r.x() + 2, r.y() + 3);
        ar.lineTo(r.x() + 9, r.y() + r.height() - 3);
        ar.lineTo(r.x() + 2, r.y() + r.height() - 3);
    } else {
        ar.moveTo(r.x() + 3, r.y() + 3);
        ar.lineTo(r.x() + r.width() - 4, r.y() + 3);
        ar.lineTo(r.x() + (r.width() - 1) / 2, r.y() + r.height() - 4);
    }
    ar.closeSubpath();
    p.drawPath(ar);
    p.setRenderHint(QPainter::Antialiasing, false);
}

void TimelineWidget::drawTrackHeader(QPainter& p, int y, int rowH, const Track& tr, int index, bool selected) {
    const int H = kHeaderW;

    // ── Fundo do cabeçalho (trackLabelBg do tema): a seleção acende no accent
    // do tema (o indicador principal da faixa ativa), sem pintar a pista.
    // A faixa de efeitos foge do fundo: o cabeçalho usa o azul da faixa, para
    // casar com o corpo azul dos clipes que estão nela.
    QColor base = themeColors().trackLabelBg;
    if (tr.fxTrack) {
        const QColor c = tr.color.isValid() ? tr.color : fxTrackColor();
        base = c.darker(160);
    }
    p.fillRect(0, y, H, rowH, base);
    if (selected) {
        QColor selTint = themeColors().accent;
        selTint.setAlpha(140);
        p.fillRect(0, y, H, rowH, selTint);
    }
    p.setPen(themeColors().trackBorder);
    p.drawLine(0, y + rowH - 1, H, y + rowH - 1); // separador entre faixas

    QFont basef = p.font();
    const bool hidden = !tr.visible;
    (void)index; // cabeçalho do Premiere é monocromático (sem cor por índice)

    const int contentH = rowH - kResizeHandleH;

    // ── Faixa recolhida / baixa: tira única (seta + nome + toggle + cadeado) ─
    if (tr.collapsed || contentH < 22) {
        drawCollapseArrow(p, headerBtnRect(y, 0), tr.collapsed);

        QFont nf = basef;
        nf.setBold(true);
        nf.setPointSizeF(8.5);
        p.setFont(nf);
        p.setPen(hidden ? themeColors().disabledText : themeColors().trackLabelText);
        const QString nm = p.fontMetrics().elidedText(
            tr.name, Qt::ElideRight, headerMiniNameRect(y).width());
        p.drawText(headerMiniNameRect(y), int(Qt::AlignLeft | Qt::AlignVCenter), nm);
        p.setFont(basef);

        drawOutputToggleIcon(p, headerMiniToggleRect(y), tr, themeColors().iconNormal, hidden);
        drawTrackLockIcon(p, headerMiniLockRect(y), tr.locked);
        return;
    }

    // Cores dos controles (Premiere): chips sobre o fundo do tema, glifos
    // claros; quando ativo M/S acendem no accent do tema com legenda clara.
    const QColor chipBg = themeColors().button;
    const QColor chipBd = themeColors().inputBorder;
    const QColor glyph = themeColors().iconNormal;
    const QColor glyphDim = themeColors().iconMuted;
    const QColor activeBlue = themeColors().accent;

    // Keyframes do envelope da faixa (só áudio tem kfVolume; vídeo não tem
    // envelope de faixa → navegação fica apagada, como sem keyframes).
    const QVector<Keyframe>& kfs = tr.kfVolume;
    const bool canKf = tr.audio;
    const double playT = m_playhead;
    int prevKf = -1, nextKf = -1;
    bool atKf = false;
    if (canKf) {
        for (int i = 0; i < kfs.size(); ++i) {
            const double t = kfs[i].time;
            if (std::fabs(t - playT) < 1e-3) atKf = true;
            if (t < playT - 1e-3 && (prevKf < 0 || t > kfs[prevKf].time)) prevKf = i;
            if (t > playT + 1e-3 && (nextKf < 0 || t < kfs[nextKf].time)) nextKf = i;
        }
    }

    // ── Faixa fina do topo (seta + keyframes ◀ ◆ ▶ + sync lock) ──────────
    // Na faixa de efeitos a seta expande o painel de efeitos (estilo AE):
    // ▸ recolhida (sem painel), ▾ expandida (lista de efeitos abaixo).
    drawCollapseArrow(p, headerBtnRect(y, 0), tr.fxTrack ? !isFxExpanded(tr.id) : false);

    auto keyGlyph = [&](int slot, bool enabled, bool accentOn) {
        const QRect r = headerBtnRect(y, slot);
        const double cx = r.x() + r.width() / 2.0;
        const double cy = r.y() + r.height() / 2.0;
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        if (slot == 2) { // losango central: adicionar/remover keyframe
            const QColor c = !canKf ? glyphDim : (accentOn ? activeBlue : glyph);
            p.setBrush(c);
            QPolygonF dia;
            dia << QPointF(cx, cy - 4) << QPointF(cx + 4, cy)
                << QPointF(cx, cy + 4) << QPointF(cx - 4, cy);
            p.drawPolygon(dia);
        } else {
            QPainterPath ar;
            if (slot == 1) {
                ar.moveTo(cx + 3, cy - 3.5);
                ar.lineTo(cx - 3, cy);
                ar.lineTo(cx + 3, cy + 3.5);
            } else {
                ar.moveTo(cx - 3, cy - 3.5);
                ar.lineTo(cx + 3, cy);
                ar.lineTo(cx - 3, cy + 3.5);
            }
            ar.closeSubpath();
            p.setBrush(enabled ? glyph : glyphDim);
            p.drawPath(ar);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    };
    keyGlyph(1, canKf && prevKf >= 0, false); // keyframe anterior
    keyGlyph(2, canKf, atKf);                 // adicionar/remover (acende em kf)
    keyGlyph(3, canKf && nextKf >= 0, false); // próximo keyframe

    // Sync lock ("corrente"; sem backend ainda).
    {
        const QRect r = headerBtnRect(y, 4);
        p.setPen(QPen(chipBd, 1));
        p.setBrush(chipBg);
        p.drawRect(r);
        p.setPen(Qt::NoPen);
        p.setBrush(glyphDim);
        p.drawEllipse(QPointF(r.x() + 5, r.y() + r.height() / 2.0 - 1), 3, 2.6);
        p.drawEllipse(QPointF(r.x() + 11, r.y() + r.height() / 2.0 + 1), 3, 2.6);
    }

    // ── Toggle de saída (olho/falante) ──────────────────────────────────
    drawOutputToggleIcon(p, headerBtnRect(y, 5), tr, themeColors().iconNormal, hidden);

    // ── Botões M / S / R do áudio ───────────────────────────────────────
    if (tr.audio) {
        auto chip = [&](int slot, const QString& label, bool active) {
            const QRect r = headerBtnRect(y, slot);
            p.setPen(QPen(active ? activeBlue.lighter(140) : chipBd, 1));
            p.setBrush(active ? activeBlue : chipBg);
            p.drawRect(r);
            p.setPen(active ? themeColors().highlightedText : themeColors().iconMuted);
            QFont bf = basef;
            bf.setBold(true);
            bf.setPointSizeF(7.5);
            p.setFont(bf);
            p.drawText(r, Qt::AlignCenter, label);
            p.setFont(basef);
        };
        chip(6, QStringLiteral("M"), tr.muted);
        chip(7, QStringLiteral("S"), tr.solo);
        chip(8, QStringLiteral("R"), false); // gravação de voz: sem backend
    }

    // ── Nome (linha abaixo dos controles) + cadeado ─────────────────────
    if (contentH >= 26) {
        QFont f = basef;
        f.setBold(true);
        f.setPointSizeF(8.5);
        p.setFont(f);
        p.setPen(hidden ? themeColors().disabledText : themeColors().trackLabelText);
        const QString nm = p.fontMetrics().elidedText(
            tr.name, Qt::ElideRight, headerNameRect(y).width());
        p.drawText(headerNameRect(y), int(Qt::AlignLeft | Qt::AlignVCenter), nm);
        p.setFont(basef);
        drawTrackLockIcon(p, headerLockRect(y), tr.locked);
    }

    // ── VU meter vertical do áudio (borda direita, como no Premiere) ────
    if (tr.audio) {
        const QRect mr = headerMeterRect(y, rowH);
        if (!mr.isEmpty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(themeColors().trackBg); // slot do meter
            p.drawRect(mr);
            // segmentos do meter (leitura retificada, como no Premiere)
            p.setPen(themeColors().trackLabelBg);
            for (int sy = mr.y() + 4; sy < mr.y() + mr.height(); sy += 4)
                p.drawLine(mr.x(), sy, mr.x() + mr.width(), sy);
        }
    }

    // ── Alça de redimensionamento ────────────────────────────────────────
    const int gy0 = y + rowH - kResizeHandleH;
    p.fillRect(0, gy0, H, kResizeHandleH, themeColors().trackBg);
    p.setPen(themeColors().iconMuted);
    const int gx0 = (H - 26) / 2;
    for (int i = 0; i < 4; ++i)
        p.drawLine(gx0 + i * 8, gy0 + 2, gx0 + i * 8, gy0 + 3);
}

// Cadeado pequeno do cabeçalho, como no Premiere: ícone no tom muted do tema;
// travado acende no texto do cabeçalho (não há "vermelho de lock").
static void drawTrackLockIcon(QPainter& p, const QRect& r, bool locked) {
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c = locked ? themeColors().trackLabelText : themeColors().iconMuted;
    const double cx = r.center().x();
    const double top = r.y() + 2.0;
    // Alça (arreio) do cadeado.
    p.setPen(QPen(c, 1.6));
    p.setBrush(Qt::NoBrush);
    const int hinge = 3;
    p.drawArc(QRectF(cx - hinge, top, hinge * 2, hinge * 2 + 2), 0, 180 * 16);
    // Corpo.
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(cx - 4.5, top + 4, 9, 7), 1.5, 1.5);
    // Buraco da fechadura.
    p.setBrush(themeColors().base);
    p.drawRoundedRect(QRectF(cx - 1.4, top + 6.2, 2.8, 3), 0.8, 0.8);
    p.setRenderHint(QPainter::Antialiasing, false);
}

void TimelineWidget::drawOutputToggleIcon(QPainter& p, const QRect& r, const Track& tr,
                                          const QColor& active, bool hidden) {
    // Ícone flat, sem caixa de fundo — como o olho/falante do Premiere.
    const QColor c = hidden ? themeColors().iconMuted : active;
    p.setRenderHint(QPainter::Antialiasing, true);
    const double cx = r.x() + r.width() / 2.0;
    const double cy = r.y() + r.height() / 2.0;
    if (tr.audio) {
        // Alto-falante: corpo + cone + ondas.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(cx - 6.0, cy - 2.0, 5.0, 4.0), 0.8, 0.8);
        QPainterPath cone;
        cone.moveTo(cx - 1.0, cy - 3.0);
        cone.lineTo(cx + 3.0, cy - 5.0);
        cone.lineTo(cx + 3.0, cy + 5.0);
        cone.lineTo(cx - 1.0, cy + 3.0);
        cone.closeSubpath();
        p.drawPath(cone);
        p.setPen(QPen(c, 1.1));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(cx + 2.5, cy - 5.0, 7, 10), -55 * 16, 110 * 16);
        p.drawArc(QRectF(cx + 5.0, cy - 8.0, 10, 16), -62 * 16, 124 * 16);
    } else {
        // Olho: contorno de elipse + pupila.
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        QPainterPath eye;
        eye.moveTo(cx - 6.0, cy);
        eye.quadTo(cx - 6.0, cy - 5.2, cx + 6.0, cy);
        eye.quadTo(cx + 6.0, cy + 5.2, cx - 6.0, cy);
        eye.closeSubpath();
        p.drawPath(eye);
        p.setPen(Qt::NoPen);
        p.setBrush(themeColors().base);
        p.drawEllipse(QPointF(cx + 1.0, cy), 2.0, 2.0);
    }
    if (hidden) {
        // Barra diagonal do tema indicando saída desligada (olho/falante diet).
        p.setPen(QPen(themeColors().iconMuted, 1.3));
        p.drawLine(r.x() + 2, r.y() + r.height() - 2, r.x() + r.width() - 2, r.y() + 2);
    }
    p.setRenderHint(QPainter::Antialiasing, false);
}

void TimelineWidget::drawFolderStrip(QPainter& p, const TrackGroup& g) {
    const QRect r = folderStripRect(g);
    if (r.isEmpty()) return;
    const bool isMesa = !g.mesaId.isEmpty();
    p.setPen(Qt::NoPen);
    // Mesa groups: teal/dark cyan; regular groups: brown/amber
    if (isMesa) {
        p.setBrush(QColor(50, 110, 120));
        p.drawRect(r);
        p.setPen(QColor(38, 85, 95));
        p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());
    } else {
        p.setBrush(QColor(150, 118, 60));
        p.drawRect(r);
        p.setPen(QColor(120, 92, 46));
        p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());
    }
    const QRect ar = folderArrowRect(g);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(isMesa ? QColor(140, 220, 230) : QColor(235, 220, 180));
    p.setPen(Qt::NoPen);
    QPolygon tri;
    const int cx = ar.center().x();
    const int cy = ar.center().y();
    if (g.collapsed)
        tri << QPoint(cx - 4, cy - 5) << QPoint(cx + 3, cy) << QPoint(cx - 4, cy + 5);
    else
        tri << QPoint(cx - 5, cy - 4) << QPoint(cx + 5, cy - 4) << QPoint(cx, cy + 4);
    p.drawPolygon(tri);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setBrush(isMesa ? QColor(100, 190, 200) : QColor(224, 192, 112));
    p.drawRect(QRect(r.left() + 24, r.top() + 5, 11, 5));
    p.drawRect(QRect(r.left() + 22, r.top() + 8, 17, 12));
    p.setPen(isMesa ? QColor(200, 240, 245) : QColor(245, 235, 210));
    QFont f = p.font();
    f.setBold(true);
    f.setPointSizeF(8.5);
    p.setFont(f);
    p.drawText(QRect(r.left() + 46, r.top(), r.width() - 54, kFolderH),
               Qt::AlignLeft | Qt::AlignVCenter, g.name);
}

// ═══════════════════════════════════════════════════════════════════════════
// Faixa de efeitos expandida (painel estilo After Effects)

