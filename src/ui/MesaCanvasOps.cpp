// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaWidget.h"

#include <QMenu>
#include <QAction>
#include <QColorDialog>
#include <QtMath>

// ═══════════════════════════════════════════════════════════════════════
// Seleção múltipla
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::selectOnly(int idx) {
    m_selectedIdxs.clear();
    if (idx >= 0) {
        m_selectedIdxs.insert(idx);
        m_selectedIdx = idx;
    } else {
        m_selectedIdx = -1;
    }
}

void MesaWidget::toggleSelect(int idx) {
    if (m_selectedIdxs.contains(idx)) {
        m_selectedIdxs.remove(idx);
        if (m_selectedIdx == idx)
            m_selectedIdx = m_selectedIdxs.isEmpty()
                ? -1 : *m_selectedIdxs.constBegin();
    } else {
        m_selectedIdxs.insert(idx);
        m_selectedIdx = idx;
    }
}

void MesaWidget::toggleMotionBlur() {
    MesaComposition* mc = currentMesa();
    if (!mc) return;
    mc->motionBlur = !mc->motionBlur;
    qCInfo(lcMesa).noquote() << "[MESA] motion blur" << (mc->motionBlur ? "ON" : "OFF");
    emit changesCommitted();
    emit modified();
    update();
}

// ═══════════════════════════════════════════════════════════════════════
// Operações de composição (remover, reset, ordem)
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::removeLayersFromMesa() {
    MesaComposition* mc = currentMesa();
    if (!mc || m_selectedIdxs.isEmpty()) return;

    // Índices em ordem decrescente para remover sem deslocar os demais.
    QList<int> idxs = m_selectedIdxs.values();
    std::sort(idxs.begin(), idxs.end(), std::greater<int>());
    for (int i : idxs) {
        if (i >= 0 && i < mc->trackIds.size()) {
            qCInfo(lcMesa).noquote() << "[MESA] remover da Mesa: '"
                                     << mc->trackIds[i] << "'";
            mc->trackIds.remove(i);
        }
    }
    m_selectedIdxs.clear();
    m_selectedIdx = -1;
    emit changesCommitted();
    emit modified();
    update();
}

void MesaWidget::resetLayersTransform() {
    MesaComposition* mc = currentMesa();
    if (!mc || m_selectedIdxs.isEmpty()) return;

    const QVector<Track*> tracks = mesaTracks();
    for (int i : m_selectedIdxs) {
        if (i < 0 || i >= tracks.size()) continue;
        Track* t = tracks[i];
        if (t->mesaLocked) continue;
        t->mesaX = mc->canvasW / 2.0;
        t->mesaY = mc->canvasH / 2.0;
        t->mesaScaleX = 1.0;
        t->mesaScaleY = 1.0;
        t->mesaRotation = 0.0;
        t->mesaOpacity = 1.0;
        t->mesaAnchorX = 0.0;
        t->mesaAnchorY = 0.0;
        t->kfMesaX.clear();  t->kfMesaY.clear();
        t->kfMesaScaleX.clear(); t->kfMesaScaleY.clear();
        t->kfMesaRotation.clear(); t->kfMesaOpacity.clear();
        t->kfMesaAnchorX.clear(); t->kfMesaAnchorY.clear();
        qCInfo(lcMesa).noquote() << "[MESA] reset transform: '" << t->name << "'";
    }
    emit changesCommitted();
    emit modified();
    update();
}

// delta: -1 = recuar um, +1 = avançar um; valores além dos limites viram
// send-to-back / bring-to-front no menu (delta grande o suficiente).
void MesaWidget::moveLayerOrder(int idx, int delta) {
    MesaComposition* mc = currentMesa();
    if (!mc || idx < 0 || idx >= mc->trackIds.size()) return;

    const int n = mc->trackIds.size();
    int to = idx + delta;
    to = qBound(0, to, n - 1);
    if (to == idx) return;

    const QString tid = mc->trackIds[idx];
    mc->trackIds.remove(idx);
    mc->trackIds.insert(to, tid);
    selectOnly(to);
    emit changesCommitted();
    emit modified();
    qCInfo(lcMesa).noquote() << "[MESA] ordem: camada '" << tid
                             << "' de" << idx << "->" << to;
    update();
}

// ═══════════════════════════════════════════════════════════════════════
// Alinhar / distribuir (estilo AE: alinha no retângulo em volta da seleção;
// distribuir espaça os CENTROS no eixo escolhido mantendo o primeiro e o
// último na posição atual).
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::alignLayers(AlignMode mode) {
    MesaComposition* mc = currentMesa();
    if (!mc || m_selectedIdxs.isEmpty()) return;

    const QVector<Track*> tracks = mesaTracks();
    double L = 1e18, R = -1e18, T = 1e18, B = -1e18;
    bool any = false;
    for (int i : m_selectedIdxs) {
        if (i < 0 || i >= tracks.size()) continue;
        if (tracks[i]->mesaLocked) continue;
        const LayerBounds lb = layerBounds(tracks[i], i);
        const double hw = (lb.w * lb.sx) / 2.0;
        const double hh = (lb.h * lb.sy) / 2.0;
        L = qMin(L, lb.x - hw);
        R = qMax(R, lb.x + hw);
        T = qMin(T, lb.y - hh);
        B = qMax(B, lb.y + hh);
        any = true;
    }
    if (!any) return;

    const double midX = (L + R) / 2.0;
    const double midY = (T + B) / 2.0;

    for (int i : m_selectedIdxs) {
        if (i < 0 || i >= tracks.size()) continue;
        Track* t = tracks[i];
        if (t->mesaLocked) continue;
        const LayerBounds lb = layerBounds(t, i);
        const double hw = (lb.w * lb.sx) / 2.0;
        const double hh = (lb.h * lb.sy) / 2.0;
        switch (mode) {
            case AlignLeft:     t->mesaX = L + hw; break;
            case AlignCenterH:  t->mesaX = midX;  break;
            case AlignRight:    t->mesaX = R - hw; break;
            case AlignTop:      t->mesaY = T + hh; break;
            case AlignCenterV:  t->mesaY = midY;  break;
            case AlignBottom:   t->mesaY = B - hh; break;
        }
    }
    emit changesCommitted();
    emit modified();
    update();
}

void MesaWidget::distributeLayers(bool horizontal) {
    MesaComposition* mc = currentMesa();
    if (!mc || m_selectedIdxs.size() < 3) return;

    const QVector<Track*> tracks = mesaTracks();
    QVector<QPair<int, double>> items;  // (índice, posição do centro no eixo)
    for (int i : m_selectedIdxs) {
        if (i < 0 || i >= tracks.size()) continue;
        if (tracks[i]->mesaLocked) continue;
        const LayerBounds lb = layerBounds(tracks[i], i);
        items.append({i, horizontal ? lb.x : lb.y});
    }
    if (items.size() < 3) return;

    std::sort(items.begin(), items.end(),
              [](const QPair<int, double>& a, const QPair<int, double>& b) {
                  return a.second < b.second;
              });
    const int n = items.size();
    const double first = items.first().second;
    const double last = items.last().second;

    for (int k = 0; k < n; ++k) {
        const double val = first + (last - first) * k / double(n - 1);
        Track* t = tracks[items[k].first];
        if (horizontal) t->mesaX = val;
        else            t->mesaY = val;
    }
    emit changesCommitted();
    emit modified();
    update();
}

// ═══════════════════════════════════════════════════════════════════════
// Menu de contexto no canvas (clique direito na área de arte)
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::showCanvasContextMenu(const QPoint& globalPos, int hitIdx) {
    MesaComposition* mc = currentMesa();
    if (!mc || !m_project) return;
    const QVector<Track*> tracks = mesaTracks();

    // Right-click numa camada fora da seleção atual colapsa a seleção nela
    // (as ações abaixo agem na camada clicada).
    if (hitIdx >= 0 && hitIdx < tracks.size() && !hasSelection(hitIdx)) {
        selectOnly(hitIdx);
        emit mesaTrackSelected(tracks[hitIdx]);
        update();
    }

    enum CtxAct {
        ActNone,
        ActProps, ActDup, ActRemove, ActReset,
        ActToFront, ActToBack, ActFwd, ActBwd,
        ActAlignL, ActAlignCH, ActAlignR, ActAlignT, ActAlignCV, ActAlignB,
        ActDistH, ActDistV,
        ActSolid, ActGradient
    };

    QMenu menu(this);
    QMenu* blendMenu = nullptr;
    if (hitIdx >= 0 && hitIdx < tracks.size()) {
        Track* t = tracks[hitIdx];
        QAction* propsAct = menu.addAction(tr("Propriedades da camada…"));
        propsAct->setData(ActProps);
        QAction* dupAct = menu.addAction(tr("Duplicar camada"));
        dupAct->setData(ActDup);
        QAction* remAct = menu.addAction(tr("Remover da Mesa"));
        remAct->setData(ActRemove);
        QAction* rstAct = menu.addAction(tr("Reset de transformação"));
        rstAct->setData(ActReset);

        blendMenu = menu.addMenu(tr("Modo de mesclagem"));
        const QStringList modes = { QStringLiteral("normal"),
            QStringLiteral("add"), QStringLiteral("multiply"),
            QStringLiteral("screen"), QStringLiteral("overlay"),
            QStringLiteral("softlight"), QStringLiteral("difference") };
        for (const QString& m : modes) {
            QAction* a = blendMenu->addAction(m);
            a->setCheckable(true);
            a->setChecked(t->blendMode == m);
        }

        QMenu* orderMenu = menu.addMenu(tr("Ordem"));
        QAction* frontAct = orderMenu->addAction(tr("Trazer para a frente"));
        frontAct->setData(ActToFront);
        QAction* backAct = orderMenu->addAction(tr("Enviar para trás"));
        backAct->setData(ActToBack);
        QAction* fwdAct = orderMenu->addAction(tr("Avançar um"));
        fwdAct->setData(ActFwd);
        QAction* bwdAct = orderMenu->addAction(tr("Recuar um"));
        bwdAct->setData(ActBwd);

        menu.addSeparator();
    }

    if (selectionCount() >= 1) {
        QMenu* alignMenu = menu.addMenu(tr("Alinhar"));
        const struct { const char* label; int act; } aligns[] = {
            { "Esquerda", ActAlignL }, { "Centro horizontal", ActAlignCH },
            { "Direita", ActAlignR },  { "Topo", ActAlignT },
            { "Centro vertical", ActAlignCV }, { "Base", ActAlignB },
        };
        for (const auto& a : aligns) {
            QAction* act = alignMenu->addAction(tr(a.label));
            act->setData(a.act);
        }
        if (selectionCount() >= 3) {
            QMenu* distMenu = menu.addMenu(tr("Distribuir"));
            QAction* dh = distMenu->addAction(tr("Horizontal"));
            dh->setData(ActDistH);
            QAction* dv = distMenu->addAction(tr("Vertical"));
            dv->setData(ActDistV);
        }
    }

    QMenu* newMenu = menu.addMenu(tr("Nova camada"));
    QAction* solidAct = newMenu->addAction(tr("Sólido…"));
    solidAct->setData(ActSolid);
    QAction* gradAct = newMenu->addAction(tr("Gradiente…"));
    gradAct->setData(ActGradient);

    menu.addSeparator();
    QAction* mbAct = menu.addAction(tr("Motion blur (Ctrl+Shift+B)"));
    mbAct->setCheckable(true);
    mbAct->setChecked(mc->motionBlur);

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;

    // Motion blur da composição inteira (câmera + camadas no preview/export).
    if (chosen == mbAct) {
        toggleMotionBlur();
        return;
    }

    // Modo de mesclagem: ação do submenu (identificada pelo pai ser o QMenu).
    if (blendMenu && chosen->parent() == blendMenu && hitIdx < tracks.size()) {
        Track* t = tracks[hitIdx];
        if (chosen->text() != t->blendMode) {
            t->blendMode = chosen->text();
            qCInfo(lcMesa).noquote() << "[MESA] canvas: blend '" << t->name
                                     << "' ->" << t->blendMode;
            emit changesCommitted();
            emit modified();
            update();
        }
        return;
    }

    const int act = chosen->data().toInt();
    switch (act) {
        case ActProps:
            if (hitIdx >= 0 && hitIdx < tracks.size())
                emit mesaLayerPropsRequested(tracks[hitIdx]->id);
            break;
        case ActDup:
            if (hitIdx >= 0 && hitIdx < tracks.size())
                emit mesaDuplicateLayerRequested(mc->id, tracks[hitIdx]->id);
            break;
        case ActRemove:
            removeLayersFromMesa();
            break;
        case ActReset:
            resetLayersTransform();
            break;
        case ActToFront: moveLayerOrder(hitIdx, 1e9); break;
        case ActToBack:  moveLayerOrder(hitIdx, -1e9); break;
        case ActFwd:     moveLayerOrder(hitIdx, 1); break;
        case ActBwd:     moveLayerOrder(hitIdx, -1); break;
        case ActAlignL:  alignLayers(AlignLeft); break;
        case ActAlignCH: alignLayers(AlignCenterH); break;
        case ActAlignR:  alignLayers(AlignRight); break;
        case ActAlignT:  alignLayers(AlignTop); break;
        case ActAlignCV: alignLayers(AlignCenterV); break;
        case ActAlignB:  alignLayers(AlignBottom); break;
        case ActDistH:   distributeLayers(true); break;
        case ActDistV:   distributeLayers(false); break;
        case ActSolid: {
            QColor a = QColorDialog::getColor(Qt::black, this, tr("Cor da camada sólida"));
            if (!a.isValid()) return;
            a.setAlpha(255);
            emit mesaAddSolidRequested(QString(), a, a);
            break;
        }
        case ActGradient: {
            QColor a = QColorDialog::getColor(Qt::white, this, tr("Cor inicial do gradiente"));
            if (!a.isValid()) return;
            QColor b = QColorDialog::getColor(Qt::black, this, tr("Cor final do gradiente"));
            if (!b.isValid()) return;
            emit mesaAddSolidRequested(QStringLiteral("gradient"), a, b);
            break;
        }
        default: break;
    }
}