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

// lcMesa é definido em MesaWidget.cpp (Q_DECLARE no header).

QString MesaWidget::axisName(int mode) {
    switch (mode) {
    case RotX: return QStringLiteral("X");
    case RotY: return QStringLiteral("Y");
    case RotZ: return QStringLiteral("Z");
    case RotTrackball: return QStringLiteral("Trackball");
    case RotView: return QStringLiteral("View");
    default: return QStringLiteral("—");
    }
}

QString MesaWidget::rotateHud() const {
    QString s = QStringLiteral("Rotate: %1").arg(axisName(m_rotateMode));
    if (!m_rotateDigits.isEmpty())
        s += QStringLiteral("  %1°").arg(m_rotateDigits);
    s += QStringLiteral("  [X/Y/Z eixo · R trackball · Enter ok · Esc cancela]");
    return s;
}

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

    if (e->button() == Qt::LeftButton && m_mbButtonRect.contains(e->pos())) {
        toggleMotionBlur();
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
                    menu.addSeparator();
                    QAction* mbRow = menu.addAction(tr("Motion blur (allow)"));
                    mbRow->setCheckable(true);
                    mbRow->setChecked(t->mesaMotionBlur);
                    QAction* sel2 = menu.exec(e->globalPosition().toPoint());
                    if (sel2 == mbRow) {
                        t->mesaMotionBlur = !t->mesaMotionBlur;
                        qCInfo(lcMesa).noquote() << "[MESA] painel: motion blur '"
                                                 << t->name << "' " << (t->mesaMotionBlur ? "ON" : "OFF");
                        emit changesCommitted();
                        emit modified();
                        update();
                    } else if (sel2 && sel2->text() != t->blendMode) {
                        t->blendMode = sel2->text();
                        qCInfo(lcMesa).noquote() << "[MESA] painel: blend '" << t->name
                                                 << "' ->" << t->blendMode;
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
            QAction* delAct = m_selectedKfs.isEmpty()
                ? nullptr
                : menu.addAction(tr("Deletar keyframes selecionados"));
            QAction* sel = menu.exec(e->globalPosition().toPoint());
            if (sel == addAct) {
                qCInfo(lcMesa).noquote() << "[MESA] menu timeline: Adicionar track";
                emit mesaAddTrackRequested();
            } else if (sel && sel == delAct) {
                qCInfo(lcMesa).noquote() << "[MESA] menu timeline: deletar"
                                         << m_selectedKfs.size() << "keyframes";
                deleteSelectedKfs();
            }
        } else if (mc) {
            // Menu de contexto no canvas (clique direito na área de arte):
            // duplicar/remover/reset, ordem, blend, alinhar/distribuir e
            // criação de camadas sólidas/gradientes.
            int hitIdx = -1;
            const HitZone hz = hitTest(e->position(), hitIdx);
            const bool isLayer = (hitIdx >= 0 && (hz == HitBody || hz == HitCornerTL ||
                hz == HitCornerTR || hz == HitCornerBL || hz == HitCornerBR ||
                hz == HitEdgeT || hz == HitEdgeB || hz == HitEdgeL ||
                hz == HitEdgeR || hz == HitRotate));
            qCInfo(lcMesa).noquote() << "[MESA] canvas: menu de contexto";
            showCanvasContextMenu(e->globalPosition().toPoint(),
                                  isLayer ? hitIdx : -1);
        }
        return;
    }

    if (e->button() != Qt::LeftButton) return;

    // Mini-timeline
    if (isInMiniTimeline(e->pos())) {
        const KfRef hit = hitTestKf(e->pos());
        if (hit.time >= 0) {
            // Seleção de keyframe: clique seleciona, Ctrl+clique soma/alterna.
            const bool ctrl = e->modifiers() & Qt::ControlModifier;
            if (!ctrl) {
                if (!(m_selectedKfs.size() == 1 && m_selectedKfs.contains(hit)))
                    m_selectedKfs.clear();
                m_selectedKfs.insert(hit);
            } else {
                if (m_selectedKfs.contains(hit))
                    m_selectedKfs.remove(hit);
                else
                    m_selectedKfs.insert(hit);
            }
            if (m_selectedKfs.isEmpty()) { update(); return; }
            // Registra o tempo ORIGINAL de cada selecionado: o arrasto
            // horizontal move todos em bloco.
            m_kfDragOrigins.clear();
            for (const KfRef& r : m_selectedKfs)
                m_kfDragOrigins.append({r, r.time});
            m_kfDrag = true;
            m_kfDragStartX = e->pos().x();
            qCInfo(lcMesa).noquote() << "[MESA] timeline: keyframe selecionado t="
                                     << QString::number(hit.time, 'f', 3) << "s prop="
                                     << hit.prop << (ctrl ? " (ctrl)" : QString())
                                     << "-> seleção:" << m_selectedKfs.size();
            update();
            return;
        }
        // Vazio: clique = playhead; arrastar além do limiar = marquee.
        m_selectedKfs.clear();
        m_timelineDrag = true;
        m_timelinePressPending = true;
        m_timelinePressX = e->pos().x();
        m_marqueeStartPos = e->pos();
        m_marqueeCurX = e->pos().x();
        m_timelineMarquee = false;
        const int rulerW = artRect().width();
        const double t = xToTime(e->pos().x() - panelWidth(), rulerW);
        m_playheadTime = t;
        qCInfo(lcMesa).noquote() << "[MESA] timeline: playhead ->"
                                 << QString::number(t, 'f', 3) << "s";
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

            // Row 0 = Câmera: clique alterna a seleção da câmera (independente da
            // camada selecionada). Com ela ativa, arrastar em QUALQUER lugar do
            // canvas move a câmera; sem ela, o vazio vira a mãozinha de pan.
            if (row == 0) {
                m_cameraSelected = !m_cameraSelected;
                qCInfo(lcMesa).noquote() << "[MESA] painel: câmera"
                                         << (m_cameraSelected ? "SELECIONADA" : "desselecionada");
                emit mesaCameraSelected(m_cameraSelected ? mc : nullptr);
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
                        qCInfo(lcMesa).noquote() << "[MESA] painel: '" << t->name
                                                 << "' " << (t->mesaHidden ? "OCULTA" : "visível");
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
                        qCInfo(lcMesa).noquote() << "[MESA] painel: '" << t->name
                                                 << "' " << (t->mesaLocked ? "BLOQUEADA" : "desbloqueada");
                        emit changesCommitted();
                        emit modified();
                        update();
                        return;
                    }
                    if (!z.mb.isNull() && z.mb.contains(e->pos())) {
                        Track* t = tracks[idx];
                        t->mesaMotionBlur = !t->mesaMotionBlur;
                        qCInfo(lcMesa).noquote() << "[MESA] painel: motion blur '"
                                                 << t->name << "' " << (t->mesaMotionBlur ? "ON" : "OFF");
                        emit changesCommitted();
                        emit modified();
                        update();
                        return;
                    }
                    break;
                }

                // Corpo: alterna a seleção da camada (NÃO deseleciona a
                // câmera) + inicia possível arrasto de reordenação.
                // Shift/Ctrl+clique soma/alterna no multi-seleção.
                const bool multiMod = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
                if (multiMod) {
                    toggleSelect(idx);
                } else if (selectionCount() == 1 && hasSelection(idx)) {
                    selectOnly(-1);   // clique na única selecionada: desmarca
                } else {
                    selectOnly(idx);
                }
                emit mesaTrackSelected(hasSelection(idx) ? tracks[idx] : nullptr);
                m_layerListDragIdx = idx;
                m_layerListDragStart = e->pos();
                qCInfo(lcMesa).noquote() << "[MESA] painel: camada"
                                         << (hasSelection(idx) ? "selecionada" : "desselecionada")
                                         << "'" << tracks[idx]->name << "' idx=" << idx
                                         << "sel=" << m_selectedIdxs.size();
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

    // ── Hit test geral
    int hitIdx = -1;
    HitZone hz = hitTest(e->position(), hitIdx);

    // Se a câmera estava selecionada e o clique foi na camada, o bloco acima
    // já desmarcou a câmera e NÃO retornou — continua aqui para selecionar
    // a camada normalmente.

    // ── Câmera selecionada: só cantos redimensionam; corpo move APENAS em
    // área vazia (sem camada por baixo). Assim dá para mexer no modelo.
    if (m_cameraSelected) {
        const double relC = qMax(0.0, m_playheadTime);
        if (hz == HitCameraCorner) {
            m_resizingCamera = true;
            m_resizeCorner = cameraCornerAt(e->position());
            m_resizeStartZoom = kfValue(mc->kfCamZoom, mc->camZoom, relC);
            m_resizeStartPos = e->position();
            setCursor(Qt::SizeFDiagCursor);
            qCInfo(lcMesa).noquote() << "[MESA] canvas: RESIZING camera (cantos)";
        } else if (hz == HitBody || hz == HitCornerTL || hz == HitCornerTR
                   || hz == HitCornerBL || hz == HitCornerBR
                   || hz == HitEdgeT || hz == HitEdgeB
                   || hz == HitEdgeL || hz == HitEdgeR || hz == HitRotate) {
            // Clicou numa camada: desseleciona a câmera e deixa a camada ativa.
            m_cameraSelected = false;
            emit mesaCameraSelected(nullptr);
            // Continua para o handler de camada abaixo (não retorna).
        } else if (hz == HitCamera) {
            // Corpo da câmera em área sem camada: move a câmera.
            m_draggingCamera = true;
            m_cameraDragStart = e->position();
            m_camDragStartX = kfValue(mc->kfCamX, mc->camX, relC);
            m_camDragStartY = kfValue(mc->kfCamY, mc->camY, relC);
            setCursor(Qt::SizeAllCursor);
            qCInfo(lcMesa).noquote()
                << "[MESA] canvas: MOVING camera (área vazia)";
        } else {
            // Vazio: move a câmera (comportamento AE com câmera ativa).
            m_draggingCamera = true;
            m_cameraDragStart = e->position();
            m_camDragStartX = kfValue(mc->kfCamX, mc->camX, relC);
            m_camDragStartY = kfValue(mc->kfCamY, mc->camY, relC);
            setCursor(Qt::SizeAllCursor);
            qCInfo(lcMesa).noquote() << "[MESA] canvas: MOVING camera (vazio)";
        }
        if (m_draggingCamera || m_resizingCamera) {
            update();
            return;
        }
        // Caiu no handler de camada (câmera desmarcada acima).
    }

    // ── Câmera desmarcada: clique no gizmo só SELECIONA (1º clique) ──
    if (hz == HitCameraCorner || hz == HitCamera) {
        m_cameraSelected = true;
        emit mesaCameraSelected(mc);
        qCInfo(lcMesa).noquote() << "[MESA] canvas: câmera selecionada (1º clique)";
        update();
        return;
    }

    // Layer hit
    if (hitIdx >= 0 && (hz == HitBody || hz == HitCornerTL || hz == HitCornerTR ||
        hz == HitCornerBL || hz == HitCornerBR || hz == HitEdgeT ||
        hz == HitEdgeB || hz == HitEdgeL || hz == HitEdgeR || hz == HitRotate)) {

        QVector<Track*> tracks = mesaTracks();
        Track* t = tracks[hitIdx];

        // Multi-seleção: Shift/Ctrl+clique no CORPO soma/alterna (Shift em
        // cantos/bordas continua sendo escala livre); clique num membro do
        // grupo mantém o grupo (só troca a primária); senão colapsa nele.
        const bool bodyHit = (hz == HitBody);
        const bool multiMod = (e->modifiers() & Qt::ControlModifier)
                           || (bodyHit && (e->modifiers() & Qt::ShiftModifier));
        if (multiMod) {
            toggleSelect(hitIdx);
        } else if (selectionCount() > 1 && hasSelection(hitIdx)) {
            m_selectedIdx = hitIdx;
        } else {
            selectOnly(hitIdx);
        }
        emit mesaTrackSelected(t);

        m_dragTrackId = t->id;
        m_dragTrackIndex = hitIdx;

        // Sem seleção (Shift+clique desmarcou a única) → apenas clique.
        if (m_selectedIdxs.isEmpty()) {
            m_dragTrackId.clear();
            m_dragTrackIndex = -1;
            update();
            return;
        }

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

        // Valores iniciais de TODAS as selecionadas: o grupo move/escala/
        // rotaciona junto (escala e rotação relativas ao próprio âncora).
        m_multiStartX.clear(); m_multiStartY.clear();
        m_multiStartSX.clear(); m_multiStartSY.clear(); m_multiStartRot.clear();
        m_multiStartRotX.clear(); m_multiStartRotY.clear();
        for (int i : m_selectedIdxs) {
            if (i < 0 || i >= tracks.size()) continue;
            const Track* ti = tracks[i];
            m_multiStartX.insert(i, ti->mesaX);
            m_multiStartY.insert(i, ti->mesaY);
            m_multiStartSX.insert(i, ti->mesaScaleX);
            m_multiStartSY.insert(i, ti->mesaScaleY);
            m_multiStartRot.insert(i, ti->mesaRotation);
            m_multiStartRotX.insert(i, ti->mesaRotX);
            m_multiStartRotY.insert(i, ti->mesaRotY);
        }

if (hz == HitBody) {
            // Ferramenta ativa pode forçar scale/rotate no corpo (estilo AE
            // com W/R selecionados). Senão: mover.
            if (m_tool == ToolScale) {
                m_transformOp = TScale;
                m_scaleUniform = !(e->modifiers() & Qt::ShiftModifier);
                m_transformFromCenter = (e->modifiers() & Qt::ControlModifier) != 0;
                const LayerBounds lb = layerBounds(t, hitIdx);
                const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
                m_transformStartDist = QLineF(anchor, e->position()).length();
                setCursor(Qt::SizeFDiagCursor);
            } else if (m_tool == ToolRotate) {
                m_transformOp = TRotate;
                const LayerBounds lb = layerBounds(t, hitIdx);
                const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
                m_transformStartAngle = qAtan2(e->position().y() - anchor.y(),
                                                e->position().x() - anchor.x());
                setCursor(Qt::CrossCursor);
            } else {
                m_transformOp = TMove;
                setCursor(Qt::SizeAllCursor);
            }
        } else if (hz == HitRotate || (m_tool == ToolRotate && m_rotateMode != RotNone)) {
            m_transformOp = TRotate;
            m_multiStartRotX.insert(hitIdx, t->mesaRotX);
            m_multiStartRotY.insert(hitIdx, t->mesaRotY);
            const LayerBounds lb = layerBounds(t, hitIdx);
            const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
            m_transformStartAngle = qAtan2(e->position().y() - anchor.y(),
                                            e->position().x() - anchor.x());
            m_transformStartRot = t->mesaRotation;
            setCursor(Qt::CrossCursor);
        } else {
            m_transformOp = TScale;
            const LayerBounds lb = layerBounds(t, hitIdx);
            const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
            m_transformStartDist = QLineF(anchor, e->position()).length();
            // AE: Shift = escala livre por eixo; Ctrl = a partir do centro.
            m_scaleUniform = !(e->modifiers() & Qt::ShiftModifier);
            m_transformFromCenter = (e->modifiers() & Qt::ControlModifier) != 0;
            setCursor(Qt::SizeFDiagCursor);
        }

        qCInfo(lcMesa).noquote() << "[MESA] canvas: camada '" << t->name << "' selecionada. zona:" << int(hz)
                                 << "op:" << int(m_transformOp)
                                 << "sel=" << m_selectedIdxs.size()
                                 << "x/y/s/rot:" << t->mesaX << t->mesaY
                                 << t->mesaScaleX << t->mesaRotation;
        update();
        return;
    }

    // Shift+arrastar no vazio = retângulo de seleção múltipla (com a câmera
    // desmarcada; com ela, arrastar move a câmera — tratado acima).
    if (e->modifiers() & Qt::ShiftModifier) {
        m_canvasMarquee = true;
        m_marqueeOrigin = e->position();
        m_marqueeRect = QRectF();
        m_transformOp = TNone;
        qCInfo(lcMesa).noquote() << "[MESA] canvas: MARQUEE iniciado (Shift+vazio)";
        update();
        return;
    }

    // Nada clicado e câmera desmarcada: a mãozinha dá pan do canvas (navega a
    // vista do canvas infinito, como em apps de desenho). Com a câmera
    // selecionada o arrasto move a CÂMERA (caso acima), não o canvas.
    m_draggingCanvas = true;
    m_canvasDragStart = e->position();
    setCursor(Qt::ClosedHandCursor);
    qCInfo(lcMesa).noquote() << "[MESA] canvas: PAN iniciado (vazio)";
    update();
}

void MesaWidget::mouseMoveEvent(QMouseEvent* e) {
    MesaComposition* mc = currentMesa();

    // Arrasto de keyframes: move o bloco selecionado na mini-timeline
    if (m_kfDrag) {
        const int rulerW = artRect().width();
        const double pps = timelinePps(rulerW);
        const double dt = (e->pos().x() - m_kfDragStartX) / pps;
        for (const KfOrigin& o : m_kfDragOrigins) {
            QVector<Keyframe>* vks = keyframesFor(o.ref.source, o.ref.trackId, o.ref.prop);
            if (!vks) continue;
            const double nt = qMax(0.0, o.time + dt);
            for (Keyframe& k : *vks) {
                if (qFuzzyCompare(k.time, o.time)) {
                    k.time = nt;
                    break;
                }
            }
        }
        QSet<QVector<Keyframe>*> touched;
        for (const KfOrigin& o : m_kfDragOrigins) {
            if (QVector<Keyframe>* vks = keyframesFor(o.ref.source, o.ref.trackId, o.ref.prop))
                touched.insert(vks);
        }
        for (QVector<Keyframe>* vks : touched)
            sortKfs(*vks);
        throttledUpdate();
        return;
    }

    // Marquee já ativo: só atualiza a borda do retângulo
    if (m_canvasMarquee) {
        if ((e->position() - m_marqueeOrigin).manhattanLength() > 4)
            m_marqueeRect = QRectF(m_marqueeOrigin, e->position()).normalized();
        else
            m_marqueeRect = QRectF();
        update();
        return;
    }

    // Marquee já ativo: só atualiza a borda direita
    if (m_timelineMarquee) {
        m_marqueeCurX = e->pos().x();
        update();
        return;
    }

    // Clique no vazio da timeline: vira marquee se arrastar além do limiar
    if (m_timelinePressPending) {
        if (qAbs(e->pos().x() - m_timelinePressX) > 5) {
            m_timelinePressPending = false;
            m_timelineDrag = false;
            m_timelineMarquee = true;
            m_marqueeStartPos = e->pos();
            m_marqueeCurX = e->pos().x();
            qCInfo(lcMesa).noquote() << "[MESA] timeline: MARQUEE iniciado";
            update();
            return;
        }
    }

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
                double nx = m_multiStartX.value(m_transformTrackIdx, t->mesaX) + d.x() / m_zoom;
                double ny = m_multiStartY.value(m_transformTrackIdx, t->mesaY) + d.y() / m_zoom;

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

                // O snap ajusta a posição da PRIMÁRIA; o grupo inteiro segue o
                // mesmo delta (preserva o arranjo interno da seleção).
                const double dxc = nx - m_multiStartX.value(m_transformTrackIdx, nx);
                const double dyc = ny - m_multiStartY.value(m_transformTrackIdx, ny);
                for (int i : m_selectedIdxs) {
                    if (i < 0 || i >= tracks.size()) continue;
                    Track* ti = tracks[i];
                    if (ti->mesaLocked) continue;
                    ti->mesaX = m_multiStartX.value(i, ti->mesaX) + dxc;
                    ti->mesaY = m_multiStartY.value(i, ti->mesaY) + dyc;
                }
                m_transformHud = QStringLiteral("X %1  Y %2")
                    .arg(int(std::lround(nx))).arg(int(std::lround(ny)));
                m_transformHudPos = e->position();

            } else if (m_transformOp == TScale) {
                const LayerBounds lb = layerBounds(t, m_transformTrackIdx);
                // O ponto da âncora NA COMPOSIÇÃO é a posição (lb.x/lb.y);
                // escala é em torno dela, como no AE.
                const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));

                const bool corner = (m_transformZone == HitCornerTL || m_transformZone == HitCornerTR
                                  || m_transformZone == HitCornerBL || m_transformZone == HitCornerBR);
                const bool edgeY  = (m_transformZone == HitEdgeT || m_transformZone == HitEdgeB);
                const bool edgeX  = (m_transformZone == HitEdgeL || m_transformZone == HitEdgeR);

                // Ctrl = escala a partir do centro (meio do quad, não o âncora).
                const QPointF centerPt = canvasToScreen(
                    QPointF(lb.x + lb.w * lb.sx * 0.5, lb.y + lb.h * lb.sy * 0.5));
                const QPointF scaleOrigin = m_transformFromCenter ? centerPt : anchor;

                double fx = 1.0, fy = 1.0;
                auto clamp = [](double v) { return qMax(0.01, v); };

                const double dNow = QLineF(scaleOrigin, e->position()).length();
                const double dRef = m_transformFromCenter
                    ? qMax(1.0, QLineF(scaleOrigin, m_transformStart).length())
                    : qMax(1.0, m_transformStartDist);
                const double fDist = dNow / dRef;

                if (corner) {
                    if (m_scaleUniform) {
                        fx = clamp(fDist);
                        fy = clamp(fDist);
                    } else {
                        const QPointF d = e->position() - scaleOrigin;
                        const QPointF d0 = m_transformStart - scaleOrigin;
                        if (qAbs(d0.x()) > 1.0) fx = clamp(d.x() / d0.x());
                        if (qAbs(d0.y()) > 1.0) fy = clamp(d.y() / d0.y());
                    }
                } else if (edgeY) {
                    fy = clamp(fDist);
                } else if (edgeX) {
                    fx = clamp(fDist);
                }

                for (int i : m_selectedIdxs) {
                    if (i < 0 || i >= tracks.size()) continue;
                    Track* ti = tracks[i];
                    if (ti->mesaLocked) continue;
                    ti->mesaScaleX = qMax(0.01, m_multiStartSX.value(i, ti->mesaScaleX) * fx);
                    ti->mesaScaleY = qMax(0.01, m_multiStartSY.value(i, ti->mesaScaleY) * fy);
                }
                const int pct = int(std::lround(fx * 100.0));
                m_transformHud = QStringLiteral("Scale %1%").arg(pct);
                m_transformHudPos = e->position();

            } else if (m_transformOp == TRotate) {
                const LayerBounds lb = layerBounds(t, m_transformTrackIdx);
                const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
                const double angle = qAtan2(e->position().y() - anchor.y(),
                                            e->position().x() - anchor.x());
                double delta = qRadiansToDegrees(angle - m_transformStartAngle);
                if (e->modifiers() & Qt::ControlModifier) {
                    delta = qRound(delta / 15.0) * 15.0; // snap 15°
                }
                // Eixo ativo (Blender): View/Trackball ≈ Z; X/Y mudam rotX/rotY.
                const int axis = (m_rotateMode == RotX) ? 1
                               : (m_rotateMode == RotY) ? 2
                               : 3; // Z ou view/trackball
                for (int i : m_selectedIdxs) {
                    if (i < 0 || i >= tracks.size()) continue;
                    Track* ti = tracks[i];
                    if (ti->mesaLocked) continue;
                    if (axis == 1)
                        ti->mesaRotX = m_multiStartRotX.value(i, ti->mesaRotX) + delta;
                    else if (axis == 2)
                        ti->mesaRotY = m_multiStartRotY.value(i, ti->mesaRotY) + delta;
                    else
                        ti->mesaRotation = m_multiStartRot.value(i, ti->mesaRotation) + delta;
                }
                double absDeg = m_multiStartRot.value(m_transformTrackIdx, 0.0) + delta;
                if (axis != 3) {
                    absDeg = (axis == 1
                                  ? m_multiStartRotX.value(m_transformTrackIdx, 0.0)
                                  : m_multiStartRotY.value(m_transformTrackIdx, 0.0))
                             + delta;
                }
                absDeg = std::fmod(absDeg, 360.0);
                if (absDeg < 0) absDeg += 360.0;
                m_transformHud = QStringLiteral("Rot %1 %2°")
                    .arg(axisName(m_rotateMode))
                    .arg(int(std::lround(absDeg)));
                m_transformHudPos = e->position();
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
                // Vazio: a mãozinha indica pan do canvas (navega a vista).
                default: setCursor(Qt::OpenHandCursor); break;
            }
        }
    }
}

void MesaWidget::mouseReleaseEvent(QMouseEvent* e) {
    // Fim do transform: limpa o HUD e commita.
    if (m_transformOp != TNone) {
        m_transformOp = TNone;
        m_transformHud.clear();
        emit changesCommitted();
        emit modified();
        throttledUpdate();
        return;
    }
    // Marquee de seleção no canvas (Shift+arrasto no vazio)
    if (m_canvasMarquee) {
        MesaComposition* mcm = currentMesa();
        if (mcm && !m_marqueeRect.isNull()) {
            const QVector<Track*> tks = mesaTracks();
            const bool multiMod = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
            if (!multiMod) selectOnly(-1);
            QList<int> newly;
            for (int i = 0; i < tks.size(); ++i) {
                if (tks[i]->mesaLocked || tks[i]->mesaHidden) continue;
                const LayerBounds lb = layerBounds(tks[i], i);
                QPointF center, corners[4], rotH;
                layerScreenRect(lb, center, corners, rotH);
                QPolygonF poly;
                for (int j = 0; j < 4; ++j) poly << corners[j];
                if (poly.boundingRect().intersects(m_marqueeRect))
                    newly.append(i);
            }
            for (int i : newly) { m_selectedIdxs.insert(i); m_selectedIdx = i; }
            if (!newly.isEmpty()) {
                emit mesaTrackSelected(tks[m_selectedIdx]);
                qCInfo(lcMesa).noquote()
                    << "[MESA] marquee: " << newly.size() << "camada(s) selecionada(s) ->"
                    << m_selectedIdxs.size();
            }
        }
        m_canvasMarquee = false;
        m_marqueeRect = QRectF();
        update();
        return;
    }

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
                    selectOnly(to);
                    emit changesCommitted();
                    emit modified();
                    qCInfo(lcMesa).noquote() << "[MESA] painel: reordenou camada '" << tid
                                             << "' de" << from << "->" << to;
                }
            }
        }
        m_layerListDragIdx = -1;
        update();
        return;
    }

    // Commit do arrasto horizontal de keyframes
    if (m_kfDrag) {
        const int n = m_kfDragOrigins.size();
        m_kfDrag = false;
        m_kfDragOrigins.clear();
        emit changesCommitted();
        emit modified();
        qCInfo(lcMesa).noquote() << "[MESA] timeline: keyframes movidos (bloco de" << n << ")";
        update();
        return;
    }

    // Marquee de seleção: captura os keyframes na faixa de tempo do retângulo
    if (m_timelineMarquee) {
        MesaComposition* mc = currentMesa();
        double tA = 0.0, tB = 0.0;
        if (mc) {
            if (!(e->modifiers() & Qt::ControlModifier)) m_selectedKfs.clear();
            const int rulerW = artRect().width();
            const int a = m_marqueeStartPos.x() - panelWidth();
            const int b = m_marqueeCurX - panelWidth();
            tA = xToTime(qMin(a, b), rulerW);
            tB = xToTime(qMax(a, b), rulerW);
            auto grab = [&](const QVector<Keyframe>& vks, int prop,
                            KfRef::Source src, const QString& tid) {
                for (const Keyframe& k : vks)
                    if (k.time >= tA && k.time <= tB)
                        m_selectedKfs.insert(KfRef{src, tid, k.time, prop});
            };
            grab(mc->kfCamX, PCamX, KfRef::Cam, QString());
            grab(mc->kfCamY, PCamY, KfRef::Cam, QString());
            grab(mc->kfCamZoom, PCamZ, KfRef::Cam, QString());
            grab(mc->kfCamRotation, PCamR, KfRef::Cam, QString());
        }
        m_timelineMarquee = false;
        m_timelinePressPending = false;
        qCInfo(lcMesa).noquote() << "[MESA] timeline: marquee [" << tA << "-" << tB
                                 << "s] ->" << m_selectedKfs.size() << "keyframe(s)";
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
    const bool panned = m_draggingCanvas;
    m_draggingCanvas = false;
    m_timelineDrag = false;
    m_timelinePressPending = false;
    m_timelineMarquee = false;
    m_dragTrackId.clear();
    m_dragTrackIndex = -1;
    setCursor(Qt::ArrowCursor);

    if (panned)
        qCInfo(lcMesa).noquote() << "[MESA] commit: PAN final offset" << m_offset;

    if (wasTransforming || camChanged) {
        MesaComposition* mco = currentMesa();
        if (mco) {
            qCInfo(lcMesa).noquote()
                << "[MESA] commit: cam(x,y,zoom,rot)=" << mco->camX << mco->camY
                << mco->camZoom << mco->camRotation << "| playhead="
                << QString::number(m_playheadTime, 'f', 3);
            const QVector<Track*> tks = mesaTracks();
            if (m_transformTrackIdx >= 0 && m_transformTrackIdx < tks.size()) {
                Track* tt = tks[m_transformTrackIdx];
                qCInfo(lcMesa).noquote()
                    << "[MESA] commit: camada '" << tt->name << "' x/y=" << tt->mesaX
                    << tt->mesaY << "scale=" << tt->mesaScaleX << tt->mesaScaleY
                    << "rot=" << tt->mesaRotation;
            }
        }
        emit changesCommitted();
    }
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
    qCInfo(lcMesa).noquote() << "[MESA] canvas: zoom ->" << (int)(m_zoom * 100)
                             << "% | offset" << m_offset;
    update();
}

// ═══════════════════════════════════════════════════════════════════════
// Teclado
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::keyPressEvent(QKeyEvent* e) {
    // Ferramentas estilo After Effects: V=selecionar/mover, W=escalar, R=rotacionar.
    if (e->key() == Qt::Key_V && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        m_tool = ToolSelect;
        m_rotateMode = RotNone;
        m_rotateDigits.clear();
        qCInfo(lcMesa).noquote() << "[MESA] ferramenta: Selecionar (V)";
        update();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_W && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        m_tool = ToolScale;
        m_rotateMode = RotNone;
        m_rotateDigits.clear();
        qCInfo(lcMesa).noquote() << "[MESA] ferramenta: Escalar (W)";
        update();
        e->accept();
        return;
    }

    // ── Rotação estilo Blender ──────────────────────────────────────────
    if (m_tool == ToolRotate || m_rotateMode != RotNone) {
        // Dígitos enquanto digita graus (R → Z → 45 → Enter).
        if (e->key() >= Qt::Key_0 && e->key() <= Qt::Key_9 && m_rotateMode != RotNone
            && m_rotateMode != RotView && m_rotateMode != RotTrackball) {
            m_rotateDigits += QChar('0' + (e->key() - Qt::Key_0));
            m_transformHud = QStringLiteral("Rot %1 %2°")
                .arg(axisName(m_rotateMode))
                .arg(m_rotateDigits.isEmpty() ? QStringLiteral("…")
                                               : m_rotateDigits);
            m_transformHudPos = QPointF(width() * 0.5, 80);
            qCInfo(lcMesa).noquote() << "[MESA] rotate digits:" << m_rotateDigits;
            update();
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_Backspace && !m_rotateDigits.isEmpty()) {
            m_rotateDigits.chop(1);
            update();
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_Enter || e->key() == Qt::Key_Return) {
            if (!m_rotateDigits.isEmpty() && m_rotateMode != RotNone
                && m_rotateMode != RotView && m_rotateMode != RotTrackball) {
                bool ok = false;
                const double deg = m_rotateDigits.toDouble(&ok);
                if (ok) {
                    emit changesCommitted();
                    for (int i : m_selectedIdxs) {
                        const QVector<Track*> tracks = mesaTracks();
                        if (i < 0 || i >= tracks.size()) continue;
                        Track* ti = tracks[i];
                        if (ti->mesaLocked) continue;
                        if (m_rotateMode == RotX)
                            ti->mesaRotX += deg;
                        else if (m_rotateMode == RotY)
                            ti->mesaRotY += deg;
                        else
                            ti->mesaRotation += deg;
                    }
                    emit modified();
                    qCInfo(lcMesa).noquote() << "[MESA] rotate numeric:" << deg
                                              << "axis" << int(m_rotateMode);
                }
            }
            m_rotateDigits.clear();
            m_rotateMode = RotNone;
            m_transformOp = TNone;
            m_transformHud.clear();
            update();
            e->accept();
            return;
        }
        if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_X
            || e->key() == Qt::Key_Y || e->key() == Qt::Key_Z
            || e->key() == Qt::Key_R) {
            // X/Y/Z trava eixo; R de novo = trackball; Esc cancela.
            if (e->key() == Qt::Key_X) {
                m_rotateMode = RotX;
                qCInfo(lcMesa).noquote() << "[MESA] rotate: eixo X (arraste ou digite graus)";
            } else if (e->key() == Qt::Key_Y) {
                m_rotateMode = RotY;
                qCInfo(lcMesa).noquote() << "[MESA] rotate: eixo Y";
            } else if (e->key() == Qt::Key_Z) {
                m_rotateMode = RotZ;
                qCInfo(lcMesa).noquote() << "[MESA] rotate: eixo Z";
            } else if (e->key() == Qt::Key_R) {
                m_rotateMode = (m_rotateMode == RotTrackball) ? RotView : RotTrackball;
                qCInfo(lcMesa).noquote() << "[MESA] rotate mode:"
                                          << (m_rotateMode == RotTrackball
                                                  ? "TRACKBALL" : "VIEW");
            } else {
                m_rotateMode = RotNone;
                m_rotateDigits.clear();
                m_transformOp = TNone;
                qCInfo(lcMesa).noquote() << "[MESA] rotate cancelado";
            }
            m_tool = ToolRotate;
            m_transformHud = rotateHud();
            m_transformHudPos = QPointF(width() * 0.5, 40);
            update();
            e->accept();
            return;
        }
    }

    if (e->key() == Qt::Key_R && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        m_tool = ToolRotate;
        // Primeiro R: modo livre (view). Segundo R (já em Rotate): trackball.
        if (m_rotateMode == RotNone)
            m_rotateMode = RotView;
        else if (m_rotateMode == RotView)
            m_rotateMode = RotTrackball;
        m_rotateDigits.clear();
        m_transformHud = rotateHud();
        m_transformHudPos = QPointF(width() * 0.5, 40);
        qCInfo(lcMesa).noquote() << "[MESA] rotate:" << rotateHud()
                                  << "— X/Y/Z trava eixo; dígitos+Enter = graus";
        update();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_L) {
        m_showLayerList = !m_showLayerList;
        qCInfo(lcMesa).noquote() << "[MESA] tecla L: painel"
                                 << (m_showLayerList ? "VISÍVEL" : "oculto");
        update();
    } else if (e->key() == Qt::Key_Delete && !m_selectedKfs.isEmpty()) {
        qCInfo(lcMesa).noquote() << "[MESA] Delete: removendo" << m_selectedKfs.size() << "keyframes";
        deleteSelectedKfs();
    } else if (e->key() == Qt::Key_Delete && !m_selectedIdxs.isEmpty()) {
        qCInfo(lcMesa).noquote() << "[MESA] Delete: removendo" << m_selectedIdxs.size()
                                 << "camada(s) da Mesa";
        removeLayersFromMesa();
    } else if (e->key() == Qt::Key_Escape) {
        if (m_selectedIdx >= 0 || !m_selectedIdxs.isEmpty() || m_cameraSelected
            || !m_selectedKfs.isEmpty()
            || m_timelinePressPending || m_kfDrag) {
            m_selectedIdx = -1;
            m_selectedIdxs.clear();
            m_cameraSelected = false;
            m_selectedKfs.clear();
            m_timelinePressPending = false;
            m_timelineMarquee = false;
            m_kfDrag = false;
            m_kfDragOrigins.clear();
            emit mesaTrackSelected(nullptr);
            emit mesaCameraSelected(nullptr);
            qCInfo(lcMesa).noquote() << "[MESA] Esc: seleção limpa";
            update();
        }
    } else if (e->key() == Qt::Key_F) {
        fitToContent();
        qCInfo(lcMesa).noquote() << "[MESA] F: fitToContent -> zoom"
                                 << (int)(m_zoom * 100) << "%";
        update();
    } else if (e->key() == Qt::Key_G) {
        m_snapToGrid = !m_snapToGrid;
        qCInfo(lcMesa).noquote() << "[MESA] G: snap" << (m_snapToGrid ? "ON" : "OFF");
        update();
    } else if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right ||
               e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
        // Nudge estilo AE: setas movem 1px, Shift+setas 10px.
        const double step = (e->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
        const double dx = (e->key() == Qt::Key_Left) ? -step
                        : (e->key() == Qt::Key_Right) ? step : 0.0;
        const double dy = (e->key() == Qt::Key_Up) ? -step
                        : (e->key() == Qt::Key_Down) ? step : 0.0;
        qCInfo(lcMesa).noquote() << "[MESA] nudge dx/dy=" << dx << dy
                                 << "camSel=" << m_cameraSelected;
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
    qCInfo(lcMesa).noquote()
        << "[MESA] duplo-clique: keyframe(s) criados em t=" << QString::number(t, 'f', 3)
        << "s alvo=" << (m_selectedIdx >= 0 ? "camada"
                        : (m_cameraSelected ? "câmera" : "sem seleção"));
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
