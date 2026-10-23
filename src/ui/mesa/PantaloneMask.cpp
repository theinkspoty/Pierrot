// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/mesa/PantaloneWidget.h"
#include "ui/core/Theme.h"
#include "ui/monitor/UiDecode.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/generators.h"
#include "colombina/render/TextRender.h"

#include <QPainter>
#include <QPainterPath>
#include <QGroupBox>
#include <QPushButton>
#include <QToolButton>
#include <QSlider>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QShortcut>
#include <QKeySequence>
#include <QScrollArea>
#include <QListWidget>
#include <QCheckBox>
#include <QComboBox>
#include <functional>
#include <algorithm>
#include <cmath>

#include "ui/mesa/PantalonePriv.h"

// ── Máscaras (retângulo/elipse) ────────────────────────────────────────

Mask* PantaloneWidget::activeMask() {
    Clip* c = activeClip();
    if (!c || m_maskSel < 0 || m_maskSel >= c->masks.size()) return nullptr;
    return &c->masks[m_maskSel];
}

Mask* PantaloneWidget::currentMask() {
    return activeMask();
}

void PantaloneWidget::refreshMaskList() {
    if (!m_maskList) return;
    Clip* c = activeClip();
    const QSignalBlocker b(m_maskList);
    m_maskList->clear();
    if (c) {
        for (int i = 0; i < c->masks.size(); ++i) {
            const Mask& m = c->masks[i];
            const QString kind = m.type == QLatin1String("ellipse")
                                     ? tr("elipse")
                                     : tr("retângulo");
            m_maskList->addItem(
                tr("Máscara %1 · %2%3")
                    .arg(i + 1)
                    .arg(kind)
                    .arg(m.enabled ? QString() : tr(" (deslig.)")));
        }
    }
    const int count = m_maskList->count();
    if (m_maskSel < 0 && count > 0) m_maskSel = 0;
    if (m_maskSel >= count) m_maskSel = count - 1;
    m_maskList->setCurrentRow(m_maskSel);
}

void PantaloneWidget::syncMaskControls() {
    Mask* m = activeMask();
    const bool has = m != nullptr;
    if (m_maskEnabled) {
        const QSignalBlocker b(m_maskEnabled);
        m_maskEnabled->setChecked(m && m->enabled);
        m_maskEnabled->setEnabled(has);
    }
    if (m_maskInvert) {
        const QSignalBlocker b(m_maskInvert);
        m_maskInvert->setChecked(m && m->invert);
        m_maskInvert->setEnabled(has);
    }
    if (m_maskFeather) {
        const QSignalBlocker b(m_maskFeather);
        m_maskFeather->setValue(m ? (int)std::lround(m->feather * 100.0) : 0);
        m_maskFeather->setEnabled(has);
    }
    if (m_maskFeatherVal)
        m_maskFeatherVal->setText(
            m ? QString::number(m->feather, 'f', 2) : QStringLiteral("—"));
    if (m_maskDelBtn) m_maskDelBtn->setEnabled(has);
}

void PantaloneWidget::applyMaskEdit() {
    emitChange();
    m_view->update();
}

void PantaloneWidget::addMask(const QString& type) {
    Clip* c = activeClip();
    if (!c) return;
    emitChange();
    Mask d;
    d.type = type;
    d.enabled = true;
    d.cx = 0.5; d.cy = 0.5; d.rx = 0.35; d.ry = 0.35;
    d.rotation = 0.0; d.feather = 0.0; d.invert = false;
    c->masks.append(d);
    m_maskSel = c->masks.size() - 1;
    m_undoPushed = false; // nova ação => novo undo
    refreshMaskList();
    syncMaskControls();
    m_view->update();
}

void PantaloneWidget::removeSelectedMask() {
    Clip* c = activeClip();
    if (!c || m_maskSel < 0 || m_maskSel >= c->masks.size()) return;
    emitChange();
    c->masks.remove(m_maskSel);
    m_undoPushed = false;
    refreshMaskList();
    syncMaskControls();
    m_view->update();
}

bool PantaloneWidget::pickMaskHandle(QWidget* view, const QPoint& pos) {
    Mask* mp = activeMask();
    if (!mp || !mp->hasMask() || m_frame.isNull()) return false;
    const int w0 = m_frame.width(), h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return false;
    const QRectF disp = computeDisp(view->rect());
    auto toDisp = [&](double sx, double sy) {
        return QPointF(disp.left() + sx / w0 * disp.width(),
                       disp.top() + sy / h0 * disp.height());
    };
    // Usa os valores interpolados no playhead (igual ao preview/exportação),
    // para as alças baterem com o gizmo animado.
    Clip* c = activeClip();
    const double rel = c ? std::clamp(m_playhead - c->pos, 0.0,
                                      std::max(0.0, c->dur))
                         : 0.0;
    const QVector<QPointF> pts = maskCtrlPoints(mp->type, mp->cxAt(rel),
                                                mp->cyAt(rel), mp->rxAt(rel),
                                                mp->ryAt(rel), mp->rotAt(rel),
                                                w0, h0);
    const QPointF mouse = rotatedViewPos(view->rect(), pos);
    const double tol = 9.0;
    int best = -1;
    double bestD = tol * tol;
    for (int i = 0; i < pts.size(); ++i) {
        const QPointF sp = toDisp(pts[i].x(), pts[i].y());
        const double dx = sp.x() - mouse.x(), dy = sp.y() - mouse.y();
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestD) { bestD = d2; best = i; }
    }
    if (best < 0) return false;
    m_maskDragHandle = best;
    m_maskDragRot = mp->rotAt(rel);
    m_maskPress = toDisp(pts[0].x(), pts[0].y());
    m_maskDragAng = std::atan2(mouse.y() - m_maskPress.y(),
                               mouse.x() - m_maskPress.x());
    return true;
}

void PantaloneWidget::applyMaskDrag(QWidget* view, const QPoint& pos) {
    Mask* mp = activeMask();
    if (!mp || m_maskDragHandle < 0 || m_frame.isNull()) return;
    const int w0 = m_frame.width(), h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;
    // Posição do mouse no espaço do quadro (px), já desrotacionada.
    double sx, sy;
    screenToSource(view->rect(), pos, &sx, &sy);
    // Centro/rotação de referência interpolados no playhead (igual ao gizmo).
    Clip* c = activeClip();
    const double rel = c ? std::clamp(m_playhead - c->pos, 0.0,
                                      std::max(0.0, c->dur))
                         : 0.0;
    const double cx = mp->cxAt(rel) * w0, cy = mp->cyAt(rel) * h0;
    if (m_maskDragHandle == 0) {           // mover: centro acompanha o cursor
        mp->cx = std::clamp(sx / w0, 0.0, 1.0);
        mp->cy = std::clamp(sy / h0, 0.0, 1.0);
    } else if (m_maskDragHandle == 1) {    // rotação: ângulo ao redor do centro
        const double ang = std::atan2(sy - cy, sx - cx);
        double d = ang - m_maskDragAng;
        while (d > M_PI) d -= 2.0 * M_PI;
        while (d < -M_PI) d += 2.0 * M_PI;
        double rot = m_maskDragRot + d * 180.0 / M_PI;
        while (rot > 180.0) rot -= 360.0;
        while (rot < -180.0) rot += 360.0;
        mp->rotation = rot;
    } else {                              // borda: raio projetado nos eixos
        const double dx = sx - cx, dy = sy - cy;
        const double rad = mp->rotAt(rel) * M_PI / 180.0;
        const double lx =  dx * std::cos(rad) + dy * std::sin(rad);
        const double ly = -dx * std::sin(rad) + dy * std::cos(rad);
        if (std::abs(lx) > 1.0) mp->rx = qBound(0.001, std::abs(lx) / w0, 1.5);
        if (std::abs(ly) > 1.0) mp->ry = qBound(0.001, std::abs(ly) / h0, 1.5);
    }
    // Auto-keyframe (padrão AE/DaVinci): com "● Auto" ligado, arrastar o gizmo
    // grava keyframe das propriedades alteradas no playhead => motion.
    if (m_kfAuto && m_kfAuto->isChecked()) {
        if (m_maskDragHandle == 0) {
            writeKeyframe(P_MaskCx, mp->cx);
            writeKeyframe(P_MaskCy, mp->cy);
        } else if (m_maskDragHandle == 1) {
            writeKeyframe(P_MaskRot, mp->rotation);
        } else {
            writeKeyframe(P_MaskRx, mp->rx);
            writeKeyframe(P_MaskRy, mp->ry);
        }
        refreshDiamonds();
    }
    // Informa a propriedade editada: o editor de curvas passa a mostrar a
    // curva da máscara em tempo real enquanto você arrasta o gizmo.
    if (m_maskDragHandle == 0) emit propertyEdited(P_MaskCx);
    else if (m_maskDragHandle == 1) emit propertyEdited(P_MaskRot);
    else emit propertyEdited(P_MaskRx);
    applyMaskEdit();
}


