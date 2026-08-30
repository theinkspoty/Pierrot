// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaWidget.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QMenu>
#include <QAction>
#include <QStringList>
#include <QtMath>
#include <QCursor>
#include <QLineEdit>
#include <QShortcut>

// ═══════════════════════════════════════════════════════════════════════
// Mouse
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::mousePressEvent(QMouseEvent* e) {
    MesaComposition* mc = currentMesa();
    if (!mc) {
        if (e->button() == Qt::LeftButton && m_createMesaBtnRect.contains(e->pos()))
            emit mesaCreateRequested();
        return;
    }

    if (e->button() == Qt::MiddleButton) {
        m_draggingCanvas = true;
        m_canvasDragStart = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    // Right-click context menu (mini-timeline: adicionar track; lista de
    // camadas: blend mode da camada).
    if (e->button() == Qt::RightButton) {
        if (m_showLayerList && m_layerListRect.contains(e->pos())) {
            const QVector<Track*> tracks = mesaTracks();
            const int headerH = 22, rowH = 24;
            const int relY = e->pos().y() - m_layerListRect.top() - headerH;
            if (relY >= 0) {
                // Row 0 = Câmera (não tem blend); camadas nas rows seguintes.
                const int row = relY / rowH;
                const int rowCount = m_layerListRowCount > 0 ? m_layerListRowCount
                                                             : (int)tracks.size() + 1;
                if (row >= 1 && row < rowCount && row - 1 < (int)tracks.size()) {
                    Track* t = tracks[tracks.size() - row];
                    QMenu menu(this);
                    const QStringList modes = { QStringLiteral("normal"),
                        QStringLiteral("add"), QStringLiteral("multiply"),
                        QStringLiteral("screen"), QStringLiteral("overlay"),
                        QStringLiteral("softlight"), QStringLiteral("difference") };
                    for (const QString& m : modes) {
                        QAction* a = menu.addAction(m);
                        a->setCheckable(true);
                        a->setChecked(t->blendMode == m);
                    }
                    QAction* sel2 = menu.exec(e->globalPosition().toPoint());
                    if (sel2 && sel2->text() != t->blendMode) {
                        t->blendMode = sel2->text();
                        emit changesCommitted();
                        emit modified();
                        update();
                    }
                    return;
                }
            }
        }
        if (mc && isInMiniTimeline(e->pos())) {
            QMenu menu(this);
            QAction* addAct = menu.addAction(tr("Adicionar track"));
            QAction* sel = menu.exec(e->globalPosition().toPoint());
            if (sel == addAct) {
                emit mesaAddTrackRequested();
            }
        }
        return;
    }

    if (e->button() != Qt::LeftButton) return;

    // Campos do painel de propriedades (clique → editar valor)
    for (const PropField& f : m_propFields) {
        if (f.rect.contains(e->pos())) {
            startPropEdit(f);
            return;
        }
    }

    // Mini-timeline
    if (isInMiniTimeline(e->pos())) {
        // Check if clicking on a keyframe (selection)
        const KfRef hit = hitTestKf(e->pos());
        if (hit.time >= 0) {
            toggleKfSelection(hit, e->modifiers() & Qt::ControlModifier);
            update();
            return;
        }
        // Otherwise: scrub
        m_timelineDrag = true;
        m_selectedKfs.clear();
        const int rulerW = artRect().width();
        const double t = xToTime(e->pos().x() - panelWidth(), rulerW);
        m_playheadTime = t;
        emit mesaPlayheadChanged(t);
        update();
        return;
    }

    // Layer list (painel vertical à esquerda)
    if (m_showLayerList && m_layerListRect.contains(e->pos())) {
        const QVector<Track*> tracks = mesaTracks();
        const int headerH = 22;
        const int rowH = 24;
        const int rowCount = m_layerListRowCount > 0 ? m_layerListRowCount
                                                     : (int)tracks.size() + 1;  // +1 = linha Câmera
        const int relY = e->pos().y() - m_layerListRect.top() - headerH;
        if (relY >= 0 && relY < rowCount * rowH) {
            const int row = relY / rowH;

            // Row 0 = Câmera: seleção confiável mesmo coberta por camadas.
            if (row == 0) {
                m_cameraSelected = true;
                m_selectedIdx = -1;
                emit mesaTrackSelected(nullptr);
                emit mesaCameraSelected(mc);
                m_layerListDragIdx = -1;
                update();
                return;
            }
            const int idx = (int)tracks.size() - row;   // row 1 = camada do topo
            if (idx >= 0 && idx < tracks.size()) {

                // Zonas de ícone (olho/cadeado) têm prioridade sobre o corpo
                for (const LayerRowZone& z : m_layerZones) {
                    if (z.idx != idx) continue;
                    if (z.eye.contains(e->pos())) {
                        Track* t = tracks[idx];
                        t->mesaHidden = !t->mesaHidden;
                        if (t->mesaHidden && m_selectedIdx == idx) {
                            m_selectedIdx = -1;
                            emit mesaTrackSelected(nullptr);
                        }
                        emit changesCommitted();
                        emit modified();
                        update();
                        return;
                    }
                    if (z.lock.contains(e->pos())) {
                        Track* t = tracks[idx];
                        t->mesaLocked = !t->mesaLocked;
                        emit changesCommitted();
                        emit modified();
                        update();
                        return;
                    }
                    break;
                }

                // Corpo: seleciona (exclui a câmera) + inicia possível arrasto
                // de reordenação
                m_selectedIdx = (m_selectedIdx == idx) ? -1 : idx;
                if (m_selectedIdx == idx) {
                    m_cameraSelected = false;
                    emit mesaCameraSelected(nullptr);
                }
                m_layerListDragIdx = idx;
                m_layerListDragStart = e->pos();
                update();
                return;
            }
        }
        m_selectedIdx = -1;
        m_cameraSelected = false;
        m_layerListDragIdx = -1;
        emit mesaTrackSelected(nullptr);
        emit mesaCameraSelected(nullptr);
        update();
        return;
    }

    // Hit test geral
    int hitIdx = -1;
    HitZone hz = hitTest(e->position(), hitIdx);

    // Câmera (estilo AE): o primeiro clique só SELECIONA. Já selecionada,
    // o clique ativa a operação direto (corpo = mover, canto = redimensionar).
    if (hz == HitCameraCorner || hz == HitCamera) {
        const bool alreadySel = m_cameraSelected;
        m_cameraSelected = true;
        m_selectedIdx = -1;
        emit mesaTrackSelected(nullptr);
        emit mesaCameraSelected(mc);
        if (alreadySel) {
            const double relC = qMax(0.0, m_playheadTime);
            if (hz == HitCameraCorner) {
                m_resizingCamera = true;
                m_resizeCorner = cameraCornerAt(e->position());
                m_resizeStartZoom = kfValue(mc->kfCamZoom, mc->camZoom, relC);
                m_resizeStartPos = e->position();
            } else {
                m_draggingCamera = true;
                m_cameraDragStart = e->position();
                m_camDragStartX = kfValue(mc->kfCamX, mc->camX, relC);
                m_camDragStartY = kfValue(mc->kfCamY, mc->camY, relC);
            }
        }
        update();
        return;
    }

    // Layer hit
    if (hitIdx >= 0 && (hz == HitBody || hz == HitCornerTL || hz == HitCornerTR ||
        hz == HitCornerBL || hz == HitCornerBR || hz == HitEdgeT ||
        hz == HitEdgeB || hz == HitEdgeL || hz == HitEdgeR || hz == HitRotate)) {

        m_selectedIdx = hitIdx;
        m_cameraSelected = false;
        emit mesaCameraSelected(nullptr);

        QVector<Track*> tracks = mesaTracks();
        Track* t = tracks[hitIdx];
        emit mesaTrackSelected(t);

        m_dragTrackId = t->id;
        m_dragTrackIndex = hitIdx;

        m_transformTrackIdx = hitIdx;
        m_transformStart = e->position();
        m_transformStartX = t->mesaX;
        m_transformStartY = t->mesaY;
        m_transformStartSX = t->mesaScaleX;
        m_transformStartSY = t->mesaScaleY;
        m_transformStartRot = t->mesaRotation;
        m_transformZone = hz;
        // Escala: padrão uniforme em torno do âncora; Shift = livre por eixo.
        // (nas bordas a escala é de um eixo só, Shift não interfere)
        m_scaleUniform = !(e->modifiers() & Qt::ShiftModifier);

        if (hz == HitBody) {
            m_transformOp = TMove;
            setCursor(Qt::SizeAllCursor);
        } else if (hz == HitRotate) {
            m_transformOp = TRotate;
            const LayerBounds lb = layerBounds(t, hitIdx);
            // O ponto da âncora NA COMPOSIÇÃO é a própria posição (lb.x/lb.y).
            const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
            m_transformStartAngle = qAtan2(e->position().y() - anchor.y(),
                                            e->position().x() - anchor.x());
            setCursor(Qt::CrossCursor);
        } else {
            m_transformOp = TScale;
            const LayerBounds lb = layerBounds(t, hitIdx);
            const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
            m_transformStartDist = QLineF(anchor, e->position()).length();
            setCursor(Qt::SizeFDiagCursor);
        }

        update();
        return;
    }

    // Nada clicado: com a câmera selecionada, arrastar em qualquer lugar do
    // canvas move a câmera (o alvo ativo responde no canvas todo, como no AE).
    // Caso contrário, clique em vazio desseleciona.
    if (m_cameraSelected) {
        const double relC = qMax(0.0, m_playheadTime);
        m_draggingCamera = true;
        m_cameraDragStart = e->position();
        m_camDragStartX = kfValue(mc->kfCamX, mc->camX, relC);
        m_camDragStartY = kfValue(mc->kfCamY, mc->camY, relC);
        update();
        return;
    }
    m_selectedIdx = -1;
    emit mesaTrackSelected(nullptr);
    emit mesaCameraSelected(nullptr);
    update();
}

void MesaWidget::mouseMoveEvent(QMouseEvent* e) {
    MesaComposition* mc = currentMesa();

    if (m_draggingCanvas) {
        m_offset += e->position() - m_canvasDragStart;
        m_canvasDragStart = e->position();
        update();
        return;
    }

    if (m_timelineDrag) {
        const int rulerW = artRect().width();
        const double t = xToTime(e->pos().x() - panelWidth(), rulerW);
        m_playheadTime = t;
        emit mesaPlayheadChanged(t);
        update();
        return;
    }

    if (m_resizingCamera && mc) {
        const QPointF d = e->position() - m_resizeStartPos;
        const double delta = (d.x() + d.y()) / 2.0;
        mc->camZoom = qBound(0.05, m_resizeStartZoom * (1.0 + delta * 0.005), 20.0);
        throttledUpdate();
        return;
    }

    if (m_draggingCamera && mc) {
        const QPointF d = e->position() - m_cameraDragStart;
        mc->camX = m_camDragStartX + d.x() / m_zoom;
        mc->camY = m_camDragStartY + d.y() / m_zoom;
        throttledUpdate();
        return;
    }

    if (m_transformOp != TNone && mc && m_transformTrackIdx >= 0) {
        QVector<Track*> tracks = mesaTracks();
        if (m_transformTrackIdx < tracks.size()) {
            Track* t = tracks[m_transformTrackIdx];
            const double rel = qMax(0.0, m_playheadTime);

            if (m_transformOp == TMove) {
                const QPointF d = e->position() - m_transformStart;
                double nx = m_transformStartX + d.x() / m_zoom;
                double ny = m_transformStartY + d.y() / m_zoom;

                if (m_snapToGrid) {
                    nx = qRound(nx / kGridSize) * kGridSize;
                    ny = qRound(ny / kGridSize) * kGridSize;

                    // Snap de bordas/cantos/centros contra as outras camadas,
                    // a câmera e o centro da comp (8px na tela). Tudo em
                    // coordenadas ABSOLUTAS da composição (px, topo-esquerda).
                    const LayerBounds lb = layerBounds(t, m_transformTrackIdx);
                    const double hw = (lb.w * lb.sx) / 2.0, hh = (lb.h * lb.sy) / 2.0;
                    const double thr = 8.0 * (1.0 / m_zoom);

                    QVector<double> xs, ys;
                    for (int oi = 0; oi < tracks.size(); ++oi) {
                        if (oi == m_transformTrackIdx) continue;
                        const Track* o = tracks[oi];
                        if (o->mesaHidden) continue;
                        const LayerBounds ob = layerBounds(o, oi);
                        const double ow = (ob.w * ob.sx) / 2.0;
                        const double oh = (ob.h * ob.sy) / 2.0;
                        xs << ob.x - ow << ob.x + ow << ob.x;
                        ys << ob.y - oh << ob.y + oh << ob.y;
                    }
                    {
                        const double cZi = qMax(0.01, kfValue(mc->kfCamZoom, mc->camZoom, rel));
                        const double camW = mc->canvasW / cZi;
                        const double camH = mc->canvasH / cZi;
                        const double cXi = kfValue(mc->kfCamX, mc->camX, rel);
                        const double cYi = kfValue(mc->kfCamY, mc->camY, rel);
                        xs << cXi - camW / 2.0 << cXi + camW / 2.0 << cXi;
                        ys << cYi - camH / 2.0 << cYi + camH / 2.0 << cYi;
                    }
                    // Centro da composição = (canvasW/2, canvasH/2)
                    xs << mc->canvasW / 2.0;
                    ys << mc->canvasH / 2.0;

                    auto snapAxis = [&](double val, double half,
                                        const QVector<double>& targets) -> double {
                        double best = val, bestD = thr;
                        for (double tv : targets) {
                            for (double off : { -half, half, 0.0 }) {
                                const double dv = tv - (val + off);
                                const double ad = qAbs(dv);
                                if (ad < bestD) { bestD = ad; best = val + dv; }
                            }
                        }
                        return best;
                    };
                    nx = snapAxis(nx, hw, xs);
                    ny = snapAxis(ny, hh, ys);
                }

                t->mesaX = nx;
                t->mesaY = ny;

            } else if (m_transformOp == TScale) {
                const LayerBounds lb = layerBounds(t, m_transformTrackIdx);
                // O ponto da âncora NA COMPOSIÇÃO é a posição (lb.x/lb.y);
                // escala é em torno dela, como no AE.
                const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
                const double dist = QLineF(anchor, e->position()).length();

                const bool corner = (m_transformZone == HitCornerTL || m_transformZone == HitCornerTR
                                  || m_transformZone == HitCornerBL || m_transformZone == HitCornerBR);
                const bool edgeY  = (m_transformZone == HitEdgeT || m_transformZone == HitEdgeB);
                const bool edgeX  = (m_transformZone == HitEdgeL || m_transformZone == HitEdgeR);

                double sx = m_transformStartSX, sy = m_transformStartSY;
                auto clamp = [](double v) { return qMax(0.01, v); };

                if (corner) {
                    if (m_scaleUniform && m_transformStartDist > 1.0) {
                        // Padrão: proporcional, mantendo a proporção original.
                        const double f = dist / m_transformStartDist;
                        sx = qMax(0.01, sx * f);
                        sy = qMax(0.01, sy * f);
                    } else if (!m_scaleUniform) {
                        // Shift: escala livre ao longo dos EIXOS do mundo.
                        const QPointF d = e->position() - anchor;
                        const QPointF d0 = m_transformStart - anchor;
                        if (qAbs(d0.x()) > 1.0) sx = clamp(sx * (d.x() / d0.x()));
                        if (qAbs(d0.y()) > 1.0) sy = clamp(sy * (d.y() / d0.y()));
                    }
                } else if (edgeY && m_transformStartDist > 1.0) {
                    sy = clamp(sy * (dist / m_transformStartDist));
                } else if (edgeX && m_transformStartDist > 1.0) {
                    sx = clamp(sx * (dist / m_transformStartDist));
                }

                t->mesaScaleX = qMax(0.01, sx);
                t->mesaScaleY = qMax(0.01, sy);

            } else if (m_transformOp == TRotate) {
                const LayerBounds lb = layerBounds(t, m_transformTrackIdx);
                const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
                const double angle = qAtan2(e->position().y() - anchor.y(),
                                            e->position().x() - anchor.x());
                double delta = qRadiansToDegrees(angle - m_transformStartAngle);
                if (e->modifiers() & Qt::ControlModifier) {
                    delta = qRound(delta / 15.0) * 15.0;
                }
                t->mesaRotation = m_transformStartRot + delta;
            }

            throttledUpdate();
        }
        return;
    }

    // Cursor feedback
    if (m_transformOp == TNone && !m_draggingCamera && !m_resizingCamera && !m_draggingCanvas && !m_timelineDrag) {
        if (isInMiniTimeline(e->pos())) {
            setCursor(Qt::SizeHorCursor);
        } else {
            int hitIdx = -1;
            HitZone hz = hitTest(e->position(), hitIdx);
            switch (hz) {
                case HitCornerTL: case HitCornerBR: setCursor(Qt::SizeFDiagCursor); break;
                case HitCornerTR: case HitCornerBL: setCursor(Qt::SizeBDiagCursor); break;
                case HitEdgeT: case HitEdgeB: setCursor(Qt::SizeVerCursor); break;
                case HitEdgeL: case HitEdgeR: setCursor(Qt::SizeHorCursor); break;
                case HitRotate: setCursor(Qt::CrossCursor); break;
                case HitBody: setCursor(Qt::SizeAllCursor); break;
                // Câmera não selecionada = apenas clicável (seta); selecionada,
                // mostra que o canvas inteiro arrasta.
                case HitCamera: setCursor(m_cameraSelected ? Qt::SizeAllCursor
                                                           : Qt::ArrowCursor); break;
                case HitCameraCorner: setCursor(m_cameraSelected ? Qt::SizeFDiagCursor
                                                                 : Qt::ArrowCursor); break;
                default: setCursor(m_cameraSelected ? Qt::SizeAllCursor
                                                    : Qt::ArrowCursor); break;
            }
        }
    }
}

void MesaWidget::mouseReleaseEvent(QMouseEvent* e) {
    // Drop de reordenação na lista de camadas (arrasto de linha → linha)
    if (m_layerListDragIdx >= 0) {
        MesaComposition* mc = currentMesa();
        if (mc && m_showLayerList) {
            const QVector<Track*> tracks = mesaTracks();
            const int headerH = 22, rowH = 24;
            const int rowCount = m_layerListRowCount > 0 ? m_layerListRowCount
                                                         : (int)tracks.size() + 1;  // +1 = linha Câmera
            const int relY = e->pos().y() - m_layerListRect.top() - headerH;
            if (relY >= 0 && relY < rowCount * rowH) {
                const int row = relY / rowH;
                // Row 0 = Câmera: fixa, não participa de reordenamento.
                const int to = (row == 0) ? -1 : (int)tracks.size() - row;
                const int from = m_layerListDragIdx;
                if (to >= 0 && to < (int)mc->trackIds.size() && to != from) {
                    const QString tid = mc->trackIds[from];
                    mc->trackIds.remove(from);
                    mc->trackIds.insert(to, tid);
                    m_selectedIdx = to;
                    emit changesCommitted();
                    emit modified();
                }
            }
        }
        m_layerListDragIdx = -1;
        update();
        return;
    }

    const bool wasTransforming = (m_transformOp != TNone);
    const bool camChanged = m_draggingCamera || m_resizingCamera;

    m_transformOp = TNone;
    m_transformTrackIdx = -1;
    m_draggingCamera = false;
    m_resizingCamera = false;
    m_resizeCorner = -1;
    m_draggingCanvas = false;
    m_timelineDrag = false;
    m_dragTrackId.clear();
    m_dragTrackIndex = -1;
    setCursor(Qt::ArrowCursor);

    if (wasTransforming || camChanged)
        emit changesCommitted();
}

void MesaWidget::wheelEvent(QWheelEvent* e) {
    const double factor = e->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
    const double newZoom = qBound(0.02, m_zoom * factor, 20.0);
    if (qFuzzyCompare(newZoom, m_zoom)) return;
    // Zoom ancorado no cursor: o ponto do canvas sob o mouse fica parado,
    // em vez de a vista "fugir" para longe do ponteiro.
    const QPointF canvasPt = screenToCanvas(e->position());
    m_zoom = newZoom;
    const QPointF center = artCenter();
    m_offset = e->position() - center - canvasPt * m_zoom;
    update();
}

// ═══════════════════════════════════════════════════════════════════════
// Teclado
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_L) {
        m_showLayerList = !m_showLayerList;
        update();
    } else if (e->key() == Qt::Key_Delete && !m_selectedKfs.isEmpty()) {
        deleteSelectedKfs();
    } else if (e->key() == Qt::Key_Delete && m_selectedIdx >= 0) {
        MesaComposition* mc = currentMesa();
        if (mc && m_selectedIdx < mc->trackIds.size()) {
            mc->trackIds.remove(m_selectedIdx);
            m_selectedIdx = -1;
            emit changesCommitted();
            emit modified();
            update();
        }
    } else if (e->key() == Qt::Key_Escape) {
        if (m_selectedIdx >= 0 || m_cameraSelected) {
            m_selectedIdx = -1;
            m_cameraSelected = false;
            emit mesaTrackSelected(nullptr);
            emit mesaCameraSelected(nullptr);
            update();
        }
    } else if (e->key() == Qt::Key_F) {
        fitToContent();
        update();
    } else if (e->key() == Qt::Key_G) {
        m_snapToGrid = !m_snapToGrid;
        update();
    } else if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right ||
               e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
        // Nudge estilo AE: setas movem 1px, Shift+setas 10px.
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
        const double dx = (e->key() == Qt::Key_Left) ? -step
                        : (e->key() == Qt::Key_Right) ? step : 0.0;
        const double dy = (e->key() == Qt::Key_Up) ? -step
                        : (e->key() == Qt::Key_Down) ? step : 0.0;
        nudgeSelection(dx, dy);
    } else {
        QWidget::keyPressEvent(e);
    }
}

void MesaWidget::resizeEvent(QResizeEvent*) {
    fitToContent();
    update();
}

// ═══════════════════════════════════════════════════════════════════════
// Throttle
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (!isInMiniTimeline(e->pos())) return;

    MesaComposition* mc = currentMesa();
    if (!mc || !m_project) return;

    const int rulerW = artRect().width();
    const double t = xToTime(e->pos().x() - panelWidth(), rulerW);
    m_playheadTime = t;
    const double rel = qMax(0.0, t);

    // Duplo-clique = inserir UM keyframe no playhead do ALVO (a layer
    // selecionada ou a câmera), com o VALOR AVALIADO pelas curvas atuais.
    // Sempre KfLinear e apenas no alvo — antes o duplo-clique enchia o
    // projeto de keyframes (câmera + TODAS as layers, KfSmooth) que
    // nenhum usuário pediu.
    auto upsertEval = [rel](QVector<Keyframe>& vks, double val) {
        upsertKeyframe(vks, rel, val, KfLinear);
    };

    const QVector<Track*> tracks = mesaTracks();
    if (m_selectedIdx >= 0 && m_selectedIdx < tracks.size()) {
        Track* t = tracks[m_selectedIdx];
        upsertEval(t->kfMesaX, kfValue(t->kfMesaX, t->mesaX, rel));
        upsertEval(t->kfMesaY, kfValue(t->kfMesaY, t->mesaY, rel));
        upsertEval(t->kfMesaScaleX, kfValue(t->kfMesaScaleX, t->mesaScaleX, rel));
        upsertEval(t->kfMesaScaleY, kfValue(t->kfMesaScaleY, t->mesaScaleY, rel));
        upsertEval(t->kfMesaRotation, kfValue(t->kfMesaRotation, t->mesaRotation, rel));
        upsertEval(t->kfMesaOpacity, kfValue(t->kfMesaOpacity, t->mesaOpacity, rel));
        upsertEval(t->kfMesaAnchorX, kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel));
        upsertEval(t->kfMesaAnchorY, kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel));
    } else {
        upsertEval(mc->kfCamX, kfValue(mc->kfCamX, mc->camX, rel));
        upsertEval(mc->kfCamY, kfValue(mc->kfCamY, mc->camY, rel));
        upsertEval(mc->kfCamZoom, kfValue(mc->kfCamZoom, mc->camZoom, rel));
        upsertEval(mc->kfCamRotation, kfValue(mc->kfCamRotation, mc->camRotation, rel));
    }

    emit modified();
    emit mesaPlayheadChanged(t);
    update();
}

void MesaWidget::throttledUpdate() {
    if (!m_lastUpdateTimer.isValid()) {
        m_lastUpdateTimer.start();
        update();
        return;
    }
    const qint64 elapsed = m_lastUpdateTimer.elapsed();
    if (elapsed >= 16) {  // ~60fps
        m_lastUpdateTimer.restart();
        update();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Edição dos campos do painel de propriedades (clique → digita o valor)
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::startPropEdit(const PropField& f) {
    if (m_propEdit) {
        m_propEdit->disconnect();
        m_propEdit->deleteLater();
        m_propEdit = nullptr;
    }
    QLineEdit* ed = new QLineEdit(this);
    m_propEdit = ed;
    ed->setGeometry(f.rect.adjusted(-1, -2, 1, 2));
    ed->setText(QString::number(propFieldValue(f.kind), 'f', 2));
    ed->setStyleSheet(QStringLiteral(
        "QLineEdit{background:#1e1e1e;color:#f0f0f0;border:1px solid #5a9fff;"
        "padding:1px 3px;}"));
    ed->show();
    ed->setFocus();
    ed->selectAll();

    QShortcut* esc = new QShortcut(QKeySequence(Qt::Key_Escape), ed);
    connect(esc, &QShortcut::activated, this, [ed]() {
        ed->disconnect();
        ed->deleteLater();
    });
    connect(ed, &QLineEdit::editingFinished, this, [this, kind = f.kind, ed]() {
        ed->disconnect();
        commitPropEdit(kind, ed->text());
        ed->deleteLater();
    });
}

void MesaWidget::commitPropEdit(int kind, const QString& text) {
    m_propEdit = nullptr;
    QString s = text.trimmed();
    s.replace(',', '.');
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (!ok) { update(); return; }

    MesaComposition* mc = currentMesa();
    if (!mc) return;
    const QVector<Track*> tracks = mesaTracks();
    Track* t = (m_selectedIdx >= 0 && m_selectedIdx < tracks.size())
                   ? tracks[m_selectedIdx] : nullptr;

    auto setTrack = [&](double Track::*base, double val) {
        if (!t) return;
        t->*base = val;
    };
    auto setCam = [&](double MesaComposition::*base, double val) {
        mc->*base = val;
    };

    switch (kind) {
        case PL_X: setTrack(&Track::mesaX, v); break;
        case PL_Y: setTrack(&Track::mesaY, v); break;
        case PL_S:
            setTrack(&Track::mesaScaleX, qMax(0.001, v));
            setTrack(&Track::mesaScaleY, qMax(0.001, v));
            break;
        case PL_R: setTrack(&Track::mesaRotation, v); break;
        case PL_O: setTrack(&Track::mesaOpacity, qBound(0.0, v, 1.0)); break;
        case PL_AX: setTrack(&Track::mesaAnchorX, v); break;
        case PL_AY: setTrack(&Track::mesaAnchorY, v); break;
        case PC_X: setCam(&MesaComposition::camX, v); break;
        case PC_Y: setCam(&MesaComposition::camY, v); break;
        case PC_Z: setCam(&MesaComposition::camZoom, qBound(0.05, v, 20.0)); break;
        case PC_R: setCam(&MesaComposition::camRotation, v); break;
    }
    emit modified();
    emit changesCommitted();
    update();
}
