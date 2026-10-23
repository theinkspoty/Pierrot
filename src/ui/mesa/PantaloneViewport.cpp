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

void PantaloneWidget::viewportPress(QWidget* view, QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || m_frame.isNull()) {
        e->ignore();
        return;
    }

    // Alt+clique: move o ponto de âncora direto para o clique (sem trocar de
    // modo). Liberdade estilo AE — reposiciona o pivot de escala/rotação.
    if ((e->modifiers() & Qt::AltModifier) && m_mode == ModeTransform) {
        const double w0 = m_frame.width();
        const double h0 = m_frame.height();
        double sx, sy;
        screenToSource(view->rect(), e->pos(), &sx, &sy);
        const double ax = std::clamp((sx / w0 - 0.5) / 0.5, -1.0, 1.0);
        const double ay = std::clamp((sy / h0 - 0.5) / 0.5, -1.0, 1.0);
        QSignalBlocker bax(m_anchorX), bay(m_anchorY);
        m_anchorX->setValue((int)std::lround(ax * 100.0));
        m_anchorY->setValue((int)std::lround(ay * 100.0));
        updateValueLabels();
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        commitSlider(P_AnchorX, ax);
        commitSlider(P_AnchorY, ay);
        m_view->update();
        e->accept();
        return;
    }
    double sx, sy;
    screenToSource(view->rect(), e->pos(), &sx, &sy);
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double s = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
    const double tx = m_panX->value() / 100.0 * W;
    const double ty = m_panY->value() / 100.0 * H;
    QRectF cropS, outS;
    computeView(s, tx, ty, w0, h0, &cropS, &outS);

    const QRectF disp = computeDisp(view->rect());
    auto toDisp = [&](double cx, double cy) {
        return QPointF(disp.left() + cx / w0 * disp.width(),
                       disp.top() + cy / h0 * disp.height());
    };
    const QPointF tl = toDisp(cropS.left(), cropS.top());
    const QPointF br = toDisp(cropS.right(), cropS.bottom());
    const QPointF tr = toDisp(cropS.right(), cropS.top());
    const QPointF bl = toDisp(cropS.left(), cropS.bottom());
    const QPointF mt = toDisp(cropS.center().x(), cropS.top());
    const QPointF mb = toDisp(cropS.center().x(), cropS.bottom());
    const QPointF ml = toDisp(cropS.left(), cropS.center().y());
    const QPointF mr = toDisp(cropS.right(), cropS.center().y());
    const QPointF pos = rotatedViewPos(view->rect(), e->pos());
    const double tol = 8.0;

    auto near = [&](const QPointF& a, const QPointF& b) {
        return std::hypot(a.x() - b.x(), a.y() - b.y()) <= tol;
    };

    m_dragMode = DragNone;
    m_lastDrag = e->pos();

    // Gizmos de máscara (modo Janelas) têm precedência.
    if (m_mode == ModeMask && pickMaskHandle(view, e->pos())) {
        view->setCursor(Qt::ClosedHandCursor);
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        e->accept();
        return;
    }

    // Malha 3×3 (modo Malha): ativa ao mexer e arrasta um ponto da grade.
    if (m_mode == ModeMesh) {
        Clip* c = activeClip();
        if (c) {
            if (!c->hasMesh()) c->enableDefaultMesh();
            const QRectF mdisp = computeDisp(view->rect());
            auto mToDisp = [&](double nx, double ny) {
                return QPointF(mdisp.left() + nx * mdisp.width(),
                               mdisp.top() + ny * mdisp.height());
            };
            const QPointF mpos = rotatedViewPos(view->rect(), e->pos());
            int best = -1;
            double bestD = 81.0; // 9² px de tolerância
            for (int k = 0; k < c->mesh.size(); ++k) {
                const QPointF d = mToDisp(c->mesh[k].x(), c->mesh[k].y());
                const double dx = d.x() - mpos.x(), dy = d.y() - mpos.y();
                const double d2 = dx * dx + dy * dy;
                if (d2 < bestD) { bestD = d2; best = k; }
            }
            if (best >= 0) {
                m_meshDrag = best;
                if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
                view->setCursor(Qt::ClosedHandCursor);
                e->accept();
                return;
            }
        }
    }

    // Hit-test nas alças de ZOOM (cantos da janela de saída, deslocados para
    // fora) e na alça de ROTAÇÃO (círculo acima do topo): ficam mais para fora
    // que recorte/pan, então têm precedência.
    const QPointF oTL = toDisp(outS.left(), outS.top());
    const QPointF oTR = toDisp(outS.right(), outS.top());
    const QPointF oBL = toDisp(outS.left(), outS.bottom());
    const QPointF oBR = toDisp(outS.right(), outS.bottom());
    const QPointF oC((oTL.x() + oBR.x()) / 2.0, (oTL.y() + oBR.y()) / 2.0);
    const QPointF rotHdl(oC.x(), oTL.y() - 26.0);
    QPointF zHandles[4];
    const QPointF zooms[4] = { oTL, oTR, oBL, oBR };
    for (int i = 0; i < 4; ++i) {
        const QPointF d = zooms[i] - oC;
        const double len = std::hypot(d.x(), d.y());
        zHandles[i] = len > 1e-6 ? zooms[i] + d / len * 9.0 : zooms[i];
    }

    if (std::hypot(pos.x() - rotHdl.x(), pos.y() - rotHdl.y()) <= 12.0) {
        const QPointF C = disp.center();
        m_grabOffset = QPointF(std::atan2(pos.y() - C.y(), pos.x() - C.x()),
                               (double)m_rotation->value());
        m_dragMode = DragRotate;
        view->setCursor(Qt::CrossCursor);
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        emit propertyEdited(P_Rotation);
        e->accept();
        return;
    }

    int zoomIdx = -1;
    for (int i = 0; i < 4; ++i) {
        if (std::hypot(pos.x() - zHandles[i].x(), pos.y() - zHandles[i].y()) <= 10.0) {
            zoomIdx = i;
            break;
        }
    }
    if (m_mode == ModeTransform && zoomIdx >= 0) {
        const double sOld = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
        m_grabOffset = QPointF(std::hypot(zHandles[zoomIdx].x() - oC.x(),
                                          zHandles[zoomIdx].y() - oC.y()),
                               sOld);
        m_zoomPivot = outS.center();
        m_zoomCenterDisp = oC;
        switch (zoomIdx) {
            case 0: m_dragMode = DragZoomTL; view->setCursor(Qt::SizeFDiagCursor); break;
            case 1: m_dragMode = DragZoomTR; view->setCursor(Qt::SizeBDiagCursor); break;
            case 2: m_dragMode = DragZoomBL; view->setCursor(Qt::SizeBDiagCursor); break;
            default: m_dragMode = DragZoomBR; view->setCursor(Qt::SizeFDiagCursor); break;
        }
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        emit propertyEdited(P_Scale);
        e->accept();
        return;
    }

    // Hit-test no gizmo de ancoragem (círculo branco).
    const double axPct = m_anchorX->value() / 100.0;
    const double ayPct = m_anchorY->value() / 100.0;
    const QPointF anchorDisp = toDisp(w0 * 0.5 + axPct * w0 * 0.5,
                                      h0 * 0.5 + ayPct * h0 * 0.5);
    if (m_mode == ModeTransform && near(pos, anchorDisp)) {
        m_dragMode = DragAnchor;
        m_grabOffset = QPointF(axPct, ayPct);
        view->setCursor(Qt::CrossCursor);
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        emit propertyEdited(P_AnchorX);
        e->accept();
        return;
    }

    if (m_mode == ModeCrop) {
        if (near(pos, tl)) m_dragMode = DragCropTL;
        else if (near(pos, tr)) m_dragMode = DragCropTR;
        else if (near(pos, bl)) m_dragMode = DragCropBL;
        else if (near(pos, br)) m_dragMode = DragCropBR;
        else if (pos.x() >= ml.x() - tol && pos.x() <= ml.x() + tol &&
                 pos.y() >= mt.y() && pos.y() <= mb.y())
            m_dragMode = DragCropL;
        else if (pos.x() >= mr.x() - tol && pos.x() <= mr.x() + tol &&
                 pos.y() >= mt.y() && pos.y() <= mb.y())
            m_dragMode = DragCropR;
        else if (pos.y() >= mt.y() - tol && pos.y() <= mt.y() + tol &&
                 pos.x() >= ml.x() && pos.x() <= mr.x())
            m_dragMode = DragCropT;
        else if (pos.y() >= mb.y() - tol && pos.y() <= mb.y() + tol &&
                 pos.x() >= ml.x() && pos.x() <= mr.x())
            m_dragMode = DragCropB;
    }

    if (m_dragMode != DragNone) {
        switch (m_dragMode) {
            case DragCropTL: case DragCropBR: view->setCursor(Qt::SizeFDiagCursor); break;
            case DragCropTR: case DragCropBL: view->setCursor(Qt::SizeBDiagCursor); break;
            case DragCropL:  case DragCropR:  view->setCursor(Qt::SizeHorCursor); break;
            case DragCropT:  case DragCropB:  view->setCursor(Qt::SizeVerCursor); break;
            default: break;
        }
        e->accept();
        return;
    }

    // Só inicia o pan (modo Transformar) se o clique começar dentro (ou perto)
    // da janela de saída.
    if (m_mode == ModeTransform &&
        outS.adjusted(-0.05 * outS.width(), -0.05 * outS.height(),
                      0.05 * outS.width(), 0.05 * outS.height()).contains(QPointF(sx, sy))) {
        m_dragMode = DragPan;
        // Mantém a "pegada": guarda o offset entre o cursor e o centro da
        // janela para o movimento acompanhar o mouse 1:1 (sem pular ao centro).
        m_grabOffset = QPointF(outS.center().x() - sx, outS.center().y() - sy);
        view->setCursor(Qt::ClosedHandCursor);
        m_undoPushed = true;
        emit editStart();
        emit propertyEdited(P_PanX);
        applyPan(sx + m_grabOffset.x(), sy + m_grabOffset.y());
        e->accept();
    } else {
        e->ignore();
    }
}

void PantaloneWidget::viewportMove(QWidget* view, QMouseEvent* e) {
    // Arraste da malha: move o ponto selecionado (warp).
    if (m_meshDrag >= 0) {
        Clip* c = activeClip();
        if (c && c->hasMesh() && m_meshDrag < c->mesh.size() && !m_frame.isNull()) {
            const int w0 = m_frame.width(), h0 = m_frame.height();
            if (w0 > 0 && h0 > 0) {
                double sx, sy;
                screenToSource(view->rect(), e->pos(), &sx, &sy);
                c->mesh[m_meshDrag] = QPointF(std::clamp(sx / w0, -0.5, 1.5),
                                              std::clamp(sy / h0, -0.5, 1.5));
                emitChange();
                m_view->update();
            }
        }
        return;
    }
    // Arraste de máscara ativo: continua enquanto m_maskDragHandle >= 0.
    if (m_maskDragHandle >= 0) {
        applyMaskDrag(view, e->pos());
        return;
    }
    // Hover tracking: atualiza m_hoverMode quando não está arrastando.
    if (m_dragMode == DragNone && !m_frame.isNull()) {
        double sx, sy;
        screenToSource(view->rect(), e->pos(), &sx, &sy);
        const int w0 = m_frame.width();
        const int h0 = m_frame.height();
        const double W = m_project ? m_project->width : 1920.0;
        const double H = m_project ? m_project->height : 1080.0;
        const double s = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
        const double tx = m_panX->value() / 100.0 * W;
        const double ty = m_panY->value() / 100.0 * H;
        QRectF cropS, outS;
        computeView(s, tx, ty, w0, h0, &cropS, &outS);
        const QRectF disp = computeDisp(view->rect());
        auto toDisp = [&](double cx, double cy) {
            return QPointF(disp.left() + cx / w0 * disp.width(),
                           disp.top() + cy / h0 * disp.height());
        };
        const QPointF tl = toDisp(cropS.left(), cropS.top());
        const QPointF tr = toDisp(cropS.right(), cropS.top());
        const QPointF bl = toDisp(cropS.left(), cropS.bottom());
        const QPointF br = toDisp(cropS.right(), cropS.bottom());
        const QPointF mt = toDisp(cropS.center().x(), cropS.top());
        const QPointF mb = toDisp(cropS.center().x(), cropS.bottom());
        const QPointF ml = toDisp(cropS.left(), cropS.center().y());
        const QPointF mr = toDisp(cropS.right(), cropS.center().y());
        const QPointF pos = rotatedViewPos(view->rect(), e->pos());
        const double tol = 8.0;
        auto near = [&](const QPointF& a, const QPointF& b) {
            return std::hypot(a.x() - b.x(), a.y() - b.y()) <= tol;
        };
        DragMode newHover = DragNone;
        // Alças de zoom (cantos da janela de saída) e rotação (círculo acima):
        // precedência sobre recorte/pan por ficarem mais para fora.
        const QPointF oTL = toDisp(outS.left(), outS.top());
        const QPointF oTR = toDisp(outS.right(), outS.top());
        const QPointF oBL = toDisp(outS.left(), outS.bottom());
        const QPointF oBR = toDisp(outS.right(), outS.bottom());
        const QPointF oC((oTL.x() + oBR.x()) / 2.0, (oTL.y() + oBR.y()) / 2.0);
        const QPointF rotHdl(oC.x(), oTL.y() - 26.0);
        bool hitZoom = false;
        const QPointF zooms[4] = { oTL, oTR, oBL, oBR };
        for (int i = 0; i < 4 && !hitZoom; ++i) {
            const QPointF d = zooms[i] - oC;
            const double len = std::hypot(d.x(), d.y());
            const QPointF hp = len > 1e-6 ? zooms[i] + d / len * 9.0 : zooms[i];
            hitZoom = std::hypot(pos.x() - hp.x(), pos.y() - hp.y()) <= 10.0;
        }
    if (m_mode == ModeTransform &&
        std::hypot(pos.x() - rotHdl.x(), pos.y() - rotHdl.y()) <= 12.0) {
            newHover = DragRotate;
        } else if (hitZoom) {
            newHover = DragZoomBR;
        } else if (near(pos, tl)) newHover = DragCropTL;
        else if (near(pos, tr)) newHover = DragCropTR;
        else if (near(pos, bl)) newHover = DragCropBL;
        else if (near(pos, br)) newHover = DragCropBR;
        else if (pos.x() >= ml.x() - tol && pos.x() <= ml.x() + tol &&
                 pos.y() >= mt.y() && pos.y() <= mb.y())
            newHover = DragCropL;
        else if (pos.x() >= mr.x() - tol && pos.x() <= mr.x() + tol &&
                 pos.y() >= mt.y() && pos.y() <= mb.y())
            newHover = DragCropR;
        else if (pos.y() >= mt.y() - tol && pos.y() <= mt.y() + tol &&
                 pos.x() >= ml.x() && pos.x() <= mr.x())
            newHover = DragCropT;
        else if (pos.y() >= mb.y() - tol && pos.y() <= mb.y() + tol &&
                 pos.x() >= ml.x() && pos.x() <= mr.x())
            newHover = DragCropB;

        if (newHover != m_hoverMode) {
            m_hoverMode = newHover;
            view->update();
        }
        // Cursor contextual.
        switch (newHover) {
            case DragRotate: view->setCursor(Qt::CrossCursor); break;
            case DragZoomTL: case DragZoomBR: view->setCursor(Qt::SizeFDiagCursor); break;
            case DragZoomTR: case DragZoomBL: view->setCursor(Qt::SizeBDiagCursor); break;
            case DragCropTL: case DragCropBR: view->setCursor(Qt::SizeFDiagCursor); break;
            case DragCropTR: case DragCropBL: view->setCursor(Qt::SizeBDiagCursor); break;
            case DragCropL:  case DragCropR:  view->setCursor(Qt::SizeHorCursor); break;
            case DragCropT:  case DragCropB:  view->setCursor(Qt::SizeVerCursor); break;
            default: view->setCursor(Qt::ArrowCursor); break;
        }
        e->ignore();
        return;
    }
    if (m_dragMode == DragNone) {
        e->ignore();
        return;
    }
    e->accept();
    m_lastDrag = e->pos();
    double sx, sy;
    screenToSource(view->rect(), e->pos(), &sx, &sy);
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;
    const double L0 = std::clamp(m_cropL->value() / 100.0, 0.0, 1.0);
    const double R0 = std::clamp(m_cropR->value() / 100.0, 0.0, 1.0);
    const double T0 = std::clamp(m_cropT->value() / 100.0, 0.0, 1.0);
    const double B0 = std::clamp(m_cropB->value() / 100.0, 0.0, 1.0);
    switch (m_dragMode) {
        case DragPan:
            applyPan(sx + m_grabOffset.x(), sy + m_grabOffset.y());
            break;
        case DragRotate: {
            // Gira a imagem mantendo o clique: a rotação nova é a atual mais o
            // quanto o ponteiro "andou" em ângulo em torno do centro do frame.
            const QPointF C = computeDisp(view->rect()).center();
            const QPointF p = rotatedViewPos(view->rect(), e->pos());
            const double theta = std::atan2(p.y() - C.y(), p.x() - C.x());
            const double nv = std::clamp(
                m_grabOffset.y() + (theta - m_grabOffset.x()) * 180.0 / M_PI,
                (double)-kRotMax, (double)kRotMax);
            QSignalBlocker sb8(m_rotation);
            m_rotation->setValue((int)std::lround(nv));
            updateValueLabels();
            commitSlider(P_Rotation, nv);
            m_view->update();
            break;
        }
        case DragZoomTL: case DragZoomTR:
        case DragZoomBL: case DragZoomBR: {
            // Zoom "distancia-se" do centro da janela de saída: arrastar o
            // canto dourado para fora aumenta, para dentro diminui.
            const QPointF p = rotatedViewPos(view->rect(), e->pos());
            const double d = std::hypot(p.x() - m_zoomCenterDisp.x(),
                                        p.y() - m_zoomCenterDisp.y());
            const double ratio = std::clamp(d / m_grabOffset.x(), 0.1, 10.0);
            const double sNew = std::clamp(m_grabOffset.y() / ratio,
                                           kMinScale, kMaxScale);
            applyZoomTo(sNew, m_zoomPivot);
            break;
        }
        case DragAnchor: {
            // Move o ponto de ancoragem relativo ao centro do source.
            const double newAx = std::clamp((sx / w0 - 0.5) / 0.5, -1.0, 1.0);
            const double newAy = std::clamp((sy / h0 - 0.5) / 0.5, -1.0, 1.0);
            QSignalBlocker bax(m_anchorX), bay(m_anchorY);
            m_anchorX->setValue((int)std::lround(newAx * 100.0));
            m_anchorY->setValue((int)std::lround(newAy * 100.0));
            updateValueLabels();
            commitSlider(P_AnchorX, newAx);
            commitSlider(P_AnchorY, newAy);
            m_view->update();
            break;
        }
        case DragCropL: case DragCropR:
        case DragCropT: case DragCropB:
        case DragCropTL: case DragCropTR:
        case DragCropBL: case DragCropBR: {
            // Crop interativo. Shift = uniforme:
            //  • borda: recorta os DOIS lados igualmente (centro fixo);
            //  • canto: mantém a proporção da área recortada (aspect do source).
            const bool shift = (e->modifiers() & Qt::ShiftModifier);
            double nL = L0, nR = R0, nT = T0, nB = B0;
            const double aspect = (h0 > 0) ? (double)w0 / (double)h0 : 16.0 / 9.0;
            const bool isEdge = (m_dragMode == DragCropL || m_dragMode == DragCropR
                                 || m_dragMode == DragCropT || m_dragMode == DragCropB);
            if (shift && isEdge) {
                // Borda: recorta os DOIS lados pelo mesmo delta (centro fixo).
                if (m_dragMode == DragCropL) {
                    const double d = sx / w0 - L0; // negativo = corta mais p/ esq
                    nL = std::clamp(L0 + d, 0.0, 1.0 - R0 - 0.01);
                    nR = std::clamp(R0 - d, 0.0, 1.0 - nL - 0.01);
                } else if (m_dragMode == DragCropR) {
                    const double newR = (w0 - sx) / w0;
                    const double d = newR - R0;
                    nR = std::clamp(R0 + d, 0.0, 1.0 - L0 - 0.01);
                    nL = std::clamp(L0 - d, 0.0, 1.0 - nR - 0.01);
                } else if (m_dragMode == DragCropT) {
                    const double d = sy / h0 - T0;
                    nT = std::clamp(T0 + d, 0.0, 1.0 - B0 - 0.01);
                    nB = std::clamp(B0 - d, 0.0, 1.0 - nT - 0.01);
                } else if (m_dragMode == DragCropB) {
                    const double newB = (h0 - sy) / h0;
                    const double d = newB - B0;
                    nB = std::clamp(B0 + d, 0.0, 1.0 - T0 - 0.01);
                    nT = std::clamp(T0 - d, 0.0, 1.0 - nB - 0.01);
                }
            } else if (shift && !isEdge) {
                // Canto: mantém a proporção da área visível.
                // A dimensão arrastada manda; a outra é derivada pelo aspect.
                const double maxW = 1.0 - L0 - R0;
                const double maxH = 1.0 - T0 - B0;
                if (m_dragMode == DragCropTL || m_dragMode == DragCropTR) {
                    const double newW = (m_dragMode == DragCropTL)
                        ? std::clamp(sx / w0 - L0, 0.01, maxW)
                        : std::clamp((w0 - sx) / w0 - R0, 0.01, maxW);
                    const double newH = std::clamp(newW / aspect, 0.01, maxH);
                    // Ancora no topo: T e o lado arrastado mudam; B/L/R fixos.
                    nT = T0 + maxH - newH; // empurra de cima p/ baixo
                    if (m_dragMode == DragCropTL) nL = L0 + maxW - newW;
                    else                          nR = R0 + maxW - newW;
                } else {
                    const double newH = (m_dragMode == DragCropBL)
                        ? std::clamp(sy / h0 - T0, 0.01, maxH)
                        : std::clamp((h0 - sy) / h0 - B0, 0.01, maxH);
                    const double newW = std::clamp(newH * aspect, 0.01, maxW);
                    // Ancora embaixo: B e o lado arrastado mudam; T/L/R fixos.
                    nB = B0 + maxH - newH;
                    if (m_dragMode == DragCropBL) nL = L0 + maxW - newW;
                    else                          nR = R0 + maxW - newW;
                }
            } else {
                // Livre (sem Shift).
                switch (m_dragMode) {
                case DragCropL:  nL = sx / w0; break;
                case DragCropR:  nR = (w0 - sx) / w0; break;
                case DragCropT:  nT = sy / h0; break;
                case DragCropB:  nB = (h0 - sy) / h0; break;
                case DragCropTL: nL = sx / w0; nT = sy / h0; break;
                case DragCropTR: nR = (w0 - sx) / w0; nT = sy / h0; break;
                case DragCropBL: nL = sx / w0; nB = (h0 - sy) / h0; break;
                case DragCropBR: nR = (w0 - sx) / w0; nB = (h0 - sy) / h0; break;
                default: break;
                }
            }
            setCropValues(nL, nR, nT, nB);
            break;
        }
        default: break;
    }
}

// Duplo-clique no viewport, modo Transform: se acertar o gizmo da âncora,
// reseta ao centro do source (liberdade estilo AE).
void PantaloneWidget::viewportDoubleClick(QWidget* view, QMouseEvent* e) {
    if (m_mode != ModeTransform || m_frame.isNull()) return;
    const double w0 = m_frame.width();
    const double h0 = m_frame.height();
    const double axPct = m_anchorX->value() / 100.0;
    const double ayPct = m_anchorY->value() / 100.0;
    const QRectF disp = computeDisp(view->rect());
    const QPointF anchorDisp(
        disp.left() + (w0 * 0.5 + axPct * w0 * 0.5) / w0 * disp.width(),
        disp.top() + (h0 * 0.5 + ayPct * h0 * 0.5) / h0 * disp.height());
    if (std::hypot(e->pos().x() - anchorDisp.x(),
                   e->pos().y() - anchorDisp.y()) > 8.0)
        return;
    QSignalBlocker bax(m_anchorX), bay(m_anchorY);
    m_anchorX->setValue(0);
    m_anchorY->setValue(0);
    updateValueLabels();
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    commitSlider(P_AnchorX, 0.0);
    commitSlider(P_AnchorY, 0.0);
    m_view->update();
    e->accept();
}

void PantaloneWidget::viewportRelease(QWidget* view, QMouseEvent* e) {
    if (m_meshDrag >= 0) {
        m_meshDrag = -1;
        m_undoPushed = false;
        view->setCursor(Qt::ArrowCursor);
    }
    if (m_maskDragHandle >= 0) {
        m_maskDragHandle = -1;
        m_undoPushed = false;
        view->setCursor(Qt::ArrowCursor);
    }
    if (m_dragMode != DragNone) {
        m_dragMode = DragNone;
        view->setCursor(Qt::ArrowCursor);
        m_undoPushed = false;
    }
    e->accept();
}

void PantaloneWidget::viewportWheel(QWidget* view, QWheelEvent* e) {
    if (m_frame.isNull() || !activeClip()) {
        e->ignore();
        return;
    }
    // Alt+roda rotaciona a imagem em passos de kRotStep.
    if (e->modifiers() & Qt::AltModifier) {
        const int nv = std::clamp(m_rotation->value()
                                      + (e->angleDelta().y() > 0 ? kRotStep : -kRotStep),
                                  -kRotMax, kRotMax);
        QSignalBlocker b8(m_rotation);
        m_rotation->setValue(nv);
        commitSlider(P_Rotation, (double)nv);
        updateValueLabels();
        m_view->update();
        e->accept();
        return;
    }
    const double factor = e->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1;
    const double sOld = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
    const double sNew = std::clamp(sOld * factor, kMinScale, kMaxScale);
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double tx = m_panX->value() / 100.0 * W;
    const double ty = m_panY->value() / 100.0 * H;

    QRectF cropS, outS;
    computeView(sOld, tx, ty, w0, h0, &cropS, &outS);
    const QPointF center = outS.center();

    applyZoomTo(sNew, center);
}

void PantaloneWidget::viewportContextMenu(QWidget* view, QContextMenuEvent* e) {
    Clip* c = activeClip();
    if (!c) { e->ignore(); return; }
    QMenu menu(view);

    // Flip
    QAction* flipH = menu.addAction(tr("Espelhar Horizontal"));
    flipH->setCheckable(true);
    flipH->setChecked(c->scaleX < 0.0);
    QAction* flipV = menu.addAction(tr("Espelhar Vertical"));
    flipV->setCheckable(true);
    flipV->setChecked(c->scaleY < 0.0);

    menu.addSeparator();

    // Grid
    QAction* gridAct = menu.addAction(tr("Grade de Terços"));
    gridAct->setCheckable(true);
    gridAct->setChecked(m_showGrid);

    QAction* act = menu.exec(e->globalPos());
    if (!act) return;

    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }

    if (act == flipH) {
        c->scaleX = flipH->isChecked() ? -std::fabs(c->scaleX) : std::fabs(c->scaleX);
        if (m_kfAuto && m_kfAuto->isChecked())
            writeKeyframe(P_ScaleX, c->scaleX);
        emit modified();
        syncFromClip();
        m_view->update();
    } else if (act == flipV) {
        c->scaleY = flipV->isChecked() ? -std::fabs(c->scaleY) : std::fabs(c->scaleY);
        if (m_kfAuto && m_kfAuto->isChecked())
            writeKeyframe(P_ScaleY, c->scaleY);
        emit modified();
        syncFromClip();
        m_view->update();
    } else if (act == gridAct) {
        m_showGrid = gridAct->isChecked();
        m_view->update();
    }
}


