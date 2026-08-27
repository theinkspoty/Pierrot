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

    // Right-click context menu
    if (e->button() == Qt::RightButton) {
        MesaComposition* mc = currentMesa();
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

    // Botão AUTO KEY (header)
    if (!m_autoKeyBtnRect.isNull() && m_autoKeyBtnRect.contains(e->pos())) {
        m_autoKey = !m_autoKey;
        update();
        return;
    }

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
        const int rulerW = width();
        const double t = xToTime(e->pos().x(), rulerW);
        m_playheadTime = t;
        emit mesaPlayheadChanged(t);
        update();
        return;
    }

    // Layer list flutuante
    if (m_showLayerList && m_layerListRect.contains(e->pos())) {
        const QVector<Track*> tracks = mesaTracks();
        const int headerH = 26;
        const int rowH = 24;
        const int relY = e->pos().y() - m_layerListRect.top() - headerH;
        if (relY >= 0) {
            const int row = relY / rowH;
            if (row >= 0 && row < tracks.size()) {
                const int idx = tracks.size() - 1 - row;
                m_selectedIdx = (m_selectedIdx == idx) ? -1 : idx;
                update();
                return;
            }
        }
        m_selectedIdx = -1;
        update();
        return;
    }

    // Hit test geral
    int hitIdx = -1;
    HitZone hz = hitTest(e->position(), hitIdx);

    // Camera corner → resize
    if (hz == HitCameraCorner) {
        m_resizingCamera = true;
        m_resizeCorner = cameraCornerAt(e->position());
        const double relC = qMax(0.0, m_playheadTime);
        m_resizeStartZoom = kfValue(mc->kfCamZoom, mc->camZoom, relC);
        m_resizeStartPos = e->position();
        emit mesaCameraSelected(mc);
        return;
    }

    // Camera body → mover
    if (hz == HitCamera) {
        const double rel = qMax(0.0, m_playheadTime);
        m_draggingCamera = true;
        m_cameraDragStart = e->position();
        m_camDragStartX = kfValue(mc->kfCamX, mc->camX, rel);
        m_camDragStartY = kfValue(mc->kfCamY, mc->camY, rel);
        emit mesaCameraSelected(mc);
        return;
    }

    // Layer hit
    if (hitIdx >= 0 && (hz == HitBody || hz == HitCornerTL || hz == HitCornerTR ||
        hz == HitCornerBL || hz == HitCornerBR || hz == HitEdgeT ||
        hz == HitEdgeB || hz == HitEdgeL || hz == HitEdgeR || hz == HitRotate)) {

        m_selectedIdx = hitIdx;

        QVector<Track*> tracks = mesaTracks();
        Track* t = tracks[hitIdx];
        emit mesaTrackSelected(t);
        const double rel = qMax(0.0, m_playheadTime);

        m_dragTrackId = t->id;
        m_dragTrackIndex = hitIdx;

        m_transformTrackIdx = hitIdx;
        m_transformStart = e->position();
        m_transformStartX = t->mesaX;
        m_transformStartY = t->mesaY;
        m_transformStartSX = t->mesaScaleX;
        m_transformStartSY = t->mesaScaleY;
        m_transformStartRot = t->mesaRotation;

        if (hz == HitBody) {
            m_transformOp = TMove;
            setCursor(Qt::SizeAllCursor);
        } else if (hz == HitRotate) {
            m_transformOp = TRotate;
            const LayerBounds lb = layerBounds(t, hitIdx);
            const QPointF anchor = canvasToScreen(QPointF(
                kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel) + lb.x,
                kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel) + lb.y));
            m_transformStartAngle = qAtan2(e->position().y() - anchor.y(),
                                            e->position().x() - anchor.x());
            setCursor(Qt::CrossCursor);
        } else {
            m_transformOp = TScale;
            m_scaleUniform = (e->modifiers() & Qt::ShiftModifier);
            const LayerBounds lb = layerBounds(t, hitIdx);
            const QPointF anchor = canvasToScreen(QPointF(
                kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel) + lb.x,
                kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel) + lb.y));
            m_transformStartDist = QLineF(anchor, e->position()).length();
            setCursor(Qt::SizeFDiagCursor);
        }

        update();
        return;
    }

    // Nada clicado → desselecionar
    m_selectedIdx = -1;
    emit mesaTrackSelected(nullptr);
    emit mesaCameraSelected(nullptr);
    update();
}

void MesaWidget::mouseMoveEvent(QMouseEvent* e) {
    MesaComposition* mc = currentMesa();
    auto snap = [&](double v) {
        return m_snapToGrid ? qRound(v / kGridSize) * kGridSize : v;
    };

    if (m_draggingCanvas) {
        m_offset += e->position() - m_canvasDragStart;
        m_canvasDragStart = e->position();
        update();
        return;
    }

    if (m_timelineDrag) {
        const int rulerW = width();
        const double t = xToTime(e->pos().x(), rulerW);
        m_playheadTime = t;
        emit mesaPlayheadChanged(t);
        update();
        return;
    }

    if (m_resizingCamera && mc) {
        const QPointF d = e->position() - m_resizeStartPos;
        const double delta = (d.x() + d.y()) / 2.0;
        mc->camZoom = qBound(0.05, m_resizeStartZoom * (1.0 + delta * 0.005), 20.0);
        if (m_autoKey) ensureKeyframesAt(m_playheadTime);
        throttledUpdate();
        return;
    }

    if (m_draggingCamera && mc) {
        const QPointF d = e->position() - m_cameraDragStart;
        mc->camX = m_camDragStartX + d.x() / m_zoom;
        mc->camY = m_camDragStartY + d.y() / m_zoom;
        if (m_autoKey) ensureKeyframesAt(m_playheadTime);
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
                t->mesaX = snap(m_transformStartX + d.x() / m_zoom);
                t->mesaY = snap(m_transformStartY + d.y() / m_zoom);

            } else if (m_transformOp == TScale) {
                const LayerBounds lb = layerBounds(t, m_transformTrackIdx);
                const QPointF anchor = canvasToScreen(QPointF(
                    kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel) + lb.x,
                    kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel) + lb.y));
                const double dist = QLineF(anchor, e->position()).length();
                if (m_transformStartDist > 1.0) {
                    const double factor = dist / m_transformStartDist;
                    const double s = qMax(0.01, m_transformStartSX * factor);
                    t->mesaScaleX = s;
                    t->mesaScaleY = s;
                }

            } else if (m_transformOp == TRotate) {
                const LayerBounds lb = layerBounds(t, m_transformTrackIdx);
                const QPointF anchor = canvasToScreen(QPointF(
                    kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel) + lb.x,
                    kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel) + lb.y));
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
                case HitCamera: setCursor(Qt::SizeAllCursor); break;
                case HitCameraCorner: setCursor(Qt::SizeFDiagCursor); break;
                default: setCursor(Qt::ArrowCursor); break;
            }
        }
    }
}

void MesaWidget::mouseReleaseEvent(QMouseEvent*) {
    const bool wasTransforming = (m_transformOp != TNone);
    const bool camChanged = m_draggingCamera || m_resizingCamera;

    // Auto-key (estilo AE): grava keyframe no playhead só do que foi mexido.
    if (m_autoKey) {
        if (wasTransforming && m_transformTrackIdx >= 0) {
            const QVector<Track*> tracks = mesaTracks();
            if (m_transformTrackIdx < tracks.size())
                writeTrackKeyframes(tracks[m_transformTrackIdx]);
        }
        if (camChanged)
            ensureKeyframesAt(m_playheadTime);
    }

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
    const QPointF center(width() / 2.0, height() / 2.0);
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
            emit modified();
            update();
        }
    } else if (e->key() == Qt::Key_F) {
        fitToContent();
        update();
    } else if (e->key() == Qt::Key_G) {
        m_snapToGrid = !m_snapToGrid;
        update();
    } else if (e->key() == Qt::Key_K) {
        m_autoKey = !m_autoKey;
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

    const int rulerW = width();
    const double t = xToTime(e->pos().x(), rulerW);
    m_playheadTime = t;

    ensureKeyframesAt(t);

    // Also create keyframes for all tracks in this Mesa
    const double rel = qMax(0.0, t);
    const QVector<Track*> tracks = mesaTracks();
    for (Track* track : tracks) {
        auto upsert = [&](QVector<Keyframe>& vks, double val) {
            upsertKeyframe(vks, rel, val, KfSmooth);
        };
        upsert(track->kfMesaX, kfValue(track->kfMesaX, track->mesaX, rel));
        upsert(track->kfMesaY, kfValue(track->kfMesaY, track->mesaY, rel));
        upsert(track->kfMesaScaleX, kfValue(track->kfMesaScaleX, track->mesaScaleX, rel));
        upsert(track->kfMesaScaleY, kfValue(track->kfMesaScaleY, track->mesaScaleY, rel));
        upsert(track->kfMesaRotation, kfValue(track->kfMesaRotation, track->mesaRotation, rel));
        upsert(track->kfMesaOpacity, kfValue(track->kfMesaOpacity, track->mesaOpacity, rel));
        upsert(track->kfMesaAnchorX, kfValue(track->kfMesaAnchorX, track->mesaAnchorX, rel));
        upsert(track->kfMesaAnchorY, kfValue(track->kfMesaAnchorY, track->mesaAnchorY, rel));
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
    const double rel = qMax(0.0, m_playheadTime);
    const QVector<Track*> tracks = mesaTracks();
    Track* t = (m_selectedIdx >= 0 && m_selectedIdx < tracks.size())
                   ? tracks[m_selectedIdx] : nullptr;

    auto setTrack = [&](double Track::*base, QVector<Keyframe> Track::*kfs, double val) {
        if (!t) return;
        t->*base = val;
        if (m_autoKey) upsertKeyframe(t->*kfs, rel, val);
    };
    auto setCam = [&](QVector<Keyframe> MesaComposition::*kfs,
                      double MesaComposition::*base, double val) {
        mc->*base = val;
        if (m_autoKey) upsertKeyframe(mc->*kfs, rel, val);
    };

    switch (kind) {
        case PL_X: setTrack(&Track::mesaX, &Track::kfMesaX, v); break;
        case PL_Y: setTrack(&Track::mesaY, &Track::kfMesaY, v); break;
        case PL_S:
            setTrack(&Track::mesaScaleX, &Track::kfMesaScaleX, qMax(0.001, v));
            setTrack(&Track::mesaScaleY, &Track::kfMesaScaleY, qMax(0.001, v));
            break;
        case PL_R: setTrack(&Track::mesaRotation, &Track::kfMesaRotation, v); break;
        case PL_O: setTrack(&Track::mesaOpacity, &Track::kfMesaOpacity,
                            qBound(0.0, v, 1.0)); break;
        case PC_X: setCam(&MesaComposition::kfCamX, &MesaComposition::camX, v); break;
        case PC_Y: setCam(&MesaComposition::kfCamY, &MesaComposition::camY, v); break;
        case PC_Z: setCam(&MesaComposition::kfCamZoom, &MesaComposition::camZoom,
                          qBound(0.05, v, 20.0)); break;
        case PC_R: setCam(&MesaComposition::kfCamRotation, &MesaComposition::camRotation,
                          v); break;
    }
    emit modified();
    emit changesCommitted();
    update();
}
