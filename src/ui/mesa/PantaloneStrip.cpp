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

// --- Faixa de tempo dos keyframes (arraste esquerda/direita) ---

// Converte posição relativa do playhead/keyframe (0..clip.dur) em pixel na strip.
// Calha à esquerda da faixa para os rótulos das pistas (Recorte/Máscara/Resto).
static constexpr int kKfLabelGutter = 54;

static int kfStripX(double rel, const QWidget* view, double dur) {
    const int usable = view->width() - kKfLabelGutter - 4;
    if (dur <= 0.0 || usable <= 0) return kKfLabelGutter;
    const double frac = std::clamp(rel / dur, 0.0, 1.0);
    return kKfLabelGutter + (int)std::lround(frac * usable);
}

// Fração 0..1 a partir da posição x do mouse (desconta a calha e a margem).
static double kfStripFrac(int x, const QWidget* view) {
    const int usable = view->width() - kKfLabelGutter - 8;
    if (usable <= 0) return 0.0;
    return std::clamp((double)(x - kKfLabelGutter - 4) / usable, 0.0, 1.0);
}

// As 3 pistas da faixa: Recorte, Máscara e Resto (transformação agrupada).
static void kfLaneProps(QVector<int> lanes[3]) {
    using P = PantaloneWidget;
    lanes[0] = {P::P_CropL, P::P_CropR, P::P_CropT, P::P_CropB};
    lanes[1] = {P::P_MaskCx, P::P_MaskCy, P::P_MaskRx, P::P_MaskRy,
                P::P_MaskRot, P::P_MaskFeather};
    lanes[2] = {P::P_Scale, P::P_PanX, P::P_PanY, P::P_Rotation,
                P::P_ScaleX, P::P_ScaleY, P::P_AnchorX, P::P_AnchorY,
                P::P_Opacity};
}

void PantaloneWidget::paintKeyframeStrip(QWidget* view) {
    QPainter p(view);
    p.fillRect(view->rect(), QColor(19, 20, 24));
    Clip* c = activeClip();
    if (!c) return;
    const double dur = std::max(c->dur, 1e-3);
    const int w = view->width();
    const int h = view->height();

    QVector<int> lanes[3];
    kfLaneProps(lanes);
    const QString labels[3] = { tr("Recorte"), tr("Máscara"), tr("Resto") };
    const int laneH = std::max(1, h / 3);

    // Fundo, divisores e rótulos das pistas.
    QFont lf = view->font();
    lf.setPointSize(8);
    p.setFont(lf);
    for (int i = 0; i < 3; ++i) {
        const int top = i * laneH;
        const int bot = (i == 2) ? h : top + laneH;
        if (i > 0) {
            p.setPen(QColor(40, 42, 48));
            p.drawLine(0, top, w, top);
        }
        p.setPen(QColor(140, 145, 155));
        p.drawText(QRect(2, top, kKfLabelGutter - 4, bot - top),
                   Qt::AlignVCenter | Qt::AlignLeft, labels[i]);
    }

    const double rel = relPlayhead();
    // Playhead cruza todas as pistas.
    if (rel >= 0.0) {
        const int px = kfStripX(rel, view, dur);
        p.setPen(QPen(QColor(190, 220, 255, 200), 1));
        p.drawLine(px, 2, px, h - 2);
    }

    // Keyframes, uma pista de cada vez.
    for (int i = 0; i < 3; ++i) {
        const int top = i * laneH;
        const int bot = (i == 2) ? h : top + laneH;
        const int cy = (top + bot) / 2;
        QVector<double> times;
        for (int prop : lanes[i]) {
            QVector<Keyframe>* kf = keyframesFor(prop);
            if (!kf) continue;
            for (const Keyframe& k : *kf)
                if (!times.contains(k.time)) times.append(k.time);
        }
        std::sort(times.begin(), times.end());
        for (double t : times) {
            const int x = kfStripX(t, view, dur);
            const bool active = rel >= 0.0 && std::fabs(t - rel) < 1e-6;
            const bool sel = m_selectedTimes.contains(t);
            QPolygon dia = QPolygon()
                << QPoint(x, cy - 5) << QPoint(x + 4, cy)
                << QPoint(x, cy + 5) << QPoint(x - 4, cy);
            p.setPen(QPen(QColor(10, 10, 12), 1));
            // Selecionado = verde; no playhead = laranja; senão cinza.
            p.setBrush(sel ? QColor(120, 230, 150)
                           : active ? QColor(255, 179, 64)
                                    : QColor(200, 205, 215));
            p.drawPolygon(dia);
        }
    }
}

void PantaloneWidget::stripPress(QWidget* view, QMouseEvent* e) {
    m_stripDragging = false;
    m_stripPlayheadDrag = false;
    Clip* c = activeClip();
    if (e->button() != Qt::LeftButton || !c) return;
    const double dur = std::max(c->dur, 1e-3);
    const int h = view->height();
    const int laneH = std::max(1, h / 3);
    const int lane = std::clamp(e->pos().y() / laneH, 0, 2);
    const int laneTop = lane * laneH;
    const int laneBot = (lane == 2) ? h : laneTop + laneH;
    const int laneCy = (laneTop + laneBot) / 2;

    // Encontra o keyframe mais próximo do clique DENTRO da pista clicada.
    // O hit é limitado à zona do losango (vertical) para não roubar o arraste
    // da agulha quando o playhead está exatamente sobre um keyframe.
    QVector<int> lanes[3];
    kfLaneProps(lanes);
    QVector<double> times;
    for (int prop : lanes[lane]) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (const Keyframe& k : *kf)
            if (!times.contains(k.time)) times.append(k.time);
    }
    int bestIdx = -1;
    double bestDist = 1e9;
    for (int i = 0; i < times.size(); ++i) {
        const int x = kfStripX(times[i], view, dur);
        const int dist = std::abs(e->pos().x() - x);
        if (dist <= 9 && std::abs(e->pos().y() - laneCy) <= laneH / 2 + 2 && dist < bestDist) {
            bestDist = dist; bestIdx = i;
        }
    }
    if (bestIdx >= 0) {
        m_stripLane = lane; // mover/apagar só a pista de onde veio o keyframe
        // Shift+clique alterna a seleção sem iniciar arraste.
        if (e->modifiers() & Qt::ShiftModifier) {
            if (m_selectedTimes.contains(times[bestIdx]))
                m_selectedTimes.removeAll(times[bestIdx]);
            else
                m_selectedTimes.append(times[bestIdx]);
            std::sort(m_selectedTimes.begin(), m_selectedTimes.end());
            view->setFocus();
            m_strip->update();
            e->accept();
            return;
        }
        // Ctrl+clique alterna a seleção E permite arrastar (multi-arraste).
        if (e->modifiers() & Qt::ControlModifier) {
            if (m_selectedTimes.contains(times[bestIdx]))
                m_selectedTimes.removeAll(times[bestIdx]);
            else
                m_selectedTimes.append(times[bestIdx]);
            std::sort(m_selectedTimes.begin(), m_selectedTimes.end());
            view->setFocus();
            m_stripDragging = true;
            m_stripDragFrom = times[bestIdx];
            m_stripDragTo = times[bestIdx];
            setCursor(Qt::ClosedHandCursor);
            emit editStart();
            m_undoPushed = true;
            emit keyframeJump(c->pos + times[bestIdx]);
            m_strip->update();
            e->accept();
            return;
        }
        // Clique simples seleciona só este keyframe e permite arrastar.
        m_selectedTimes = { times[bestIdx] };
        view->setFocus();
        m_stripDragging = true;
        m_stripDragFrom = times[bestIdx];
        m_stripDragTo = times[bestIdx];
        setCursor(Qt::ClosedHandCursor);
        emit editStart();
        m_undoPushed = true;
        emit keyframeJump(c->pos + times[bestIdx]);
        m_strip->update();
        e->accept();
        return;
    }

    // Clique vazio / na agulha: limpa a seleção e pula/arrasta o playhead.
    m_selectedTimes.clear();
    m_stripLane = -1;
    m_stripPlayheadDrag = true;
    const double frac = kfStripFrac(e->pos().x(), view);
    emit keyframeJump(c->pos + frac * dur);
    m_strip->update();
    e->accept();
}

void PantaloneWidget::stripMove(QWidget* view, QMouseEvent* e) {
    Clip* c = activeClip();
    if (!c) return;
    const double dur = std::max(c->dur, 1e-3);
    const double frac = kfStripFrac(e->pos().x(), view);
    const double newRel = frac * dur;

    // Arraste da agulha: move o playhead (scrub).
    if (m_stripPlayheadDrag) {
        if (std::fabs(newRel - relPlayhead()) < 1e-6) return;
        emit keyframeJump(c->pos + newRel);
        m_strip->update();
        e->accept();
        return;
    }

    if (!m_stripDragging || m_stripDragTo < 0.0) return;
    if (std::fabs(newRel - m_stripDragTo) < 1e-6) return;
    // Usa a posição ATUAL do keyframe como origem (não a original), senão o
    // keyframe "descola" do cursor já no segundo evento de mouse.
    const double from = m_stripDragTo;
    m_stripDragTo = newRel;

    // Move o keyframe SÓ na pista de onde ele veio (Recorte, Máscara ou
    // Resto) — não sincroniza as outras pistas.
    QVector<int> lanes[3];
    kfLaneProps(lanes);
    const QVector<int> props = (m_stripLane >= 0 && m_stripLane < 3)
                                   ? lanes[m_stripLane]
                                   : activeProps();
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (int i = 0; i < kf->size(); ++i) {
            if (std::fabs((*kf)[i].time - from) < 1e-6) {
                (*kf)[i].time = newRel;
                break;
            }
        }
        std::sort(kf->begin(), kf->end(),
                  [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }
    refreshDiamonds();
    m_view->update();
    m_strip->update();
    // A seleção acompanha o keyframe arrastado.
    if (m_selectedTimes.contains(from)) {
        m_selectedTimes.removeAll(from);
        m_selectedTimes.append(newRel);
        std::sort(m_selectedTimes.begin(), m_selectedTimes.end());
    }
    emit modified();
    e->accept();
}

void PantaloneWidget::stripRelease(QWidget* view, QMouseEvent* e) {
    if (m_stripDragging || m_stripPlayheadDrag) {
        m_stripDragging = false;
        m_stripPlayheadDrag = false;
        m_stripDragFrom = -1.0;
        m_stripDragTo = -1.0;
        setCursor(Qt::ArrowCursor);
        m_strip->update();
    }
    e->accept();
}

// Duplo clique na faixa: cria keyframes em todas as propriedades de
// transformação naquele instante, fixando os valores interpolados.
void PantaloneWidget::stripDoubleClick(QWidget* view, QMouseEvent* e) {
    Clip* c = activeClip();
    if (!c) { e->ignore(); return; }
    const double dur = std::max(c->dur, 1e-3);
    const double frac = kfStripFrac(e->pos().x(), view);
    const double rel = frac * dur;

    const QVector<int> props = activeProps();
    // Já existe keyframe nesse instante: nada a fazer.
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (const Keyframe& k : *kf)
            if (std::fabs(k.time - rel) < 1e-6) { e->accept(); return; }
    }

    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    emit propertyEdited(P_Scale);
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        if (prop >= P_MaskCx) continue; // duplo clique = só props do clipe
        Keyframe k;
        k.time = rel;
        k.interp = KfSmooth;
        switch (prop) {
            case P_CropL: k.value = kfValue(c->kfCropL, c->cropL, rel); break;
            case P_CropR: k.value = kfValue(c->kfCropR, c->cropR, rel); break;
            case P_CropT: k.value = kfValue(c->kfCropT, c->cropT, rel); break;
            case P_CropB: k.value = kfValue(c->kfCropB, c->cropB, rel); break;
            case P_Scale: k.value = kfValue(c->kfScale, c->scale, rel); break;
            case P_PanX:  k.value = kfValue(c->kfTx, c->tx, rel); break;
            case P_PanY:  k.value = kfValue(c->kfTy, c->ty, rel); break;
            case P_Rotation: k.value = kfValue(c->kfRotation, c->rotation, rel); break;
            case P_ScaleX: k.value = kfValue(c->kfScaleX, c->scaleX, rel); break;
            case P_ScaleY: k.value = kfValue(c->kfScaleY, c->scaleY, rel); break;
            case P_AnchorX: k.value = kfValue(c->kfAnchorX, c->anchorX, rel); break;
            case P_AnchorY: k.value = kfValue(c->kfAnchorY, c->anchorY, rel); break;
        }
        kf->append(k);
        std::sort(kf->begin(), kf->end(),
                  [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }
    emit modified();
    refreshDiamonds();
    m_strip->update();
    e->accept();
}

// Apaga os keyframes selecionados na faixa (em todas as propriedades de
// transformação no mesmo instante). Ligado ao Delete/Backspace da faixa.
void PantaloneWidget::deleteSelectedKeyframes() {
    Clip* c = activeClip();
    if (!c || m_selectedTimes.isEmpty()) return;
    QVector<int> lanes[3];
    kfLaneProps(lanes);
    const QVector<int> props = (m_stripLane >= 0 && m_stripLane < 3)
                                   ? lanes[m_stripLane]
                                   : activeProps();
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    for (double t : m_selectedTimes) {
        for (int prop : props) {
            QVector<Keyframe>* kf = keyframesFor(prop);
            if (!kf) continue;
            for (int i = kf->size() - 1; i >= 0; --i) {
                if (std::fabs((*kf)[i].time - t) < 1e-6) kf->removeAt(i);
            }
        }
    }
    m_selectedTimes.clear();
    emit modified();
    refreshDiamonds();
    m_strip->update();
}

// Clique direito num keyframe da faixa: predefinições de suavização estilo
// Vegas (linear, suave, rápido, lento, acentuada).
void PantaloneWidget::stripContextMenu(QWidget* view, QContextMenuEvent* e) {
    Clip* c = activeClip();
    if (!c) { e->ignore(); return; }
    const double dur = std::max(c->dur, 1e-3);

    const QVector<int> props = activeProps();
    QVector<double> times;
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (const Keyframe& k : *kf)
            if (!times.contains(k.time)) times.append(k.time);
    }
    int bestIdx = -1;
    double bestDist = 1e9;
    for (int i = 0; i < times.size(); ++i) {
        const int x = kfStripX(times[i], view, dur);
        const int dist = std::abs(e->pos().x() - x);
        if (dist <= 9 && dist < bestDist) { bestDist = dist; bestIdx = i; }
    }
    if (bestIdx < 0) { e->accept(); return; }
    const double t = times[bestIdx];

    QMenu menu(view);
    menu.addAction(tr("Linear (padrão)"));
    QAction* suave = menu.addAction(tr("Suave"));
    QAction* rapido = menu.addAction(tr("Rápido"));
    QAction* lento = menu.addAction(tr("Lento"));
    QAction* acent = menu.addAction(tr("Acentuada"));
    QAction* act = menu.exec(e->globalPos());
    if (!act) return;

    int preset = 0;
    if (act == suave) preset = 1;
    else if (act == rapido) preset = 2;
    else if (act == lento) preset = 3;
    else if (act == acent) preset = 4;

    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (int i = 0; i < kf->size(); ++i) {
            if (std::fabs((*kf)[i].time - t) < 1e-6) {
                applyEasePreset(*kf, i, preset);
                break;
            }
        }
    }
    emit modified();
    refreshDiamonds();
    m_strip->update();
}

