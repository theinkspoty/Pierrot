// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/motion/GraphEditorWidget.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/core/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QPushButton>
#include <QToolButton>
#include <QButtonGroup>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QFrame>
#include <QPixmap>
#include <QIcon>
#include <QFont>
#include <algorithm>
#include <cmath>
#include <functional>

#include "ui/motion/GraphEditorPriv.h"

// --------------------------------------------------------------------------
// KeyframeStrip
// --------------------------------------------------------------------------

KeyframeStrip::KeyframeStrip(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(26);
    setCursor(Qt::PointingHandCursor);
}

QSize KeyframeStrip::sizeHint() const {
    return QSize(160, 26);
}

void KeyframeStrip::setData(Clip* clip, GraphProp prop, double fps) {
    m_clip = clip;
    m_mesaTrack = nullptr;
    m_mesaMode = false;
    m_mesaCamera = nullptr;
    m_camMode = false;
    m_maskIndex = -1;
    m_maskMode = false;
    m_prop = prop;
    m_fps = (fps > 1.0) ? fps : 30.0;
    m_dragIdx = -1;
    m_moved = false;
    update();
}

void KeyframeStrip::setMesaData(Track* track, GraphProp prop, double fps) {
    m_clip = nullptr;
    m_mesaTrack = track;
    m_mesaCamera = nullptr;
    m_mesaMode = (track != nullptr);
    m_camMode = false;
    m_maskIndex = -1;
    m_maskMode = false;
    m_prop = prop;
    m_fps = (fps > 1.0) ? fps : 30.0;
    m_dragIdx = -1;
    m_moved = false;
    update();
}

void KeyframeStrip::setMaskData(Clip* clip, int maskIndex, GraphProp prop, double fps) {
    m_clip = clip;
    m_mesaTrack = nullptr;
    m_mesaCamera = nullptr;
    m_mesaMode = false;
    m_camMode = false;
    m_maskIndex = maskIndex;
    m_maskMode = (maskIndex >= 0);
    m_prop = prop;
    m_fps = (fps > 1.0) ? fps : 30.0;
    m_dragIdx = -1;
    m_moved = false;
    update();
}

const QVector<Keyframe>* KeyframeStrip::keys() const {
    if (m_camMode && m_mesaCamera) return keysForCamera(m_mesaCamera, m_prop);
    if (m_mesaMode && m_mesaTrack) return keysForTrack(m_mesaTrack, m_prop);
    if (m_maskMode) return keysForMask(maskAt(m_clip, m_maskIndex), m_prop);
    return keysFor(m_clip, m_prop);
}

void KeyframeStrip::setCamData(MesaComposition* mc, GraphProp prop, double fps) {
    m_clip = nullptr;
    m_mesaTrack = nullptr;
    m_mesaCamera = mc;
    m_mesaMode = false;
    m_camMode = (mc != nullptr);
    m_maskIndex = -1;
    m_maskMode = false;
    m_prop = prop;
    m_fps = (fps > 1.0) ? fps : 30.0;
    m_dragIdx = -1;
    m_moved = false;
    update();
}

void KeyframeStrip::setPlayhead(double t) {
    m_playhead = t;
    update();
}

double KeyframeStrip::xToT(int x) const {
    double dur;
    if (m_mesaMode || m_camMode) {
        const QVector<Keyframe>* ks = m_camMode ? keysForCamera(m_mesaCamera, m_prop)
                                                : keysForTrack(m_mesaTrack, m_prop);
        double maxT = 1.0;
        if (ks) for (const Keyframe& k : *ks) maxT = qMax(maxT, k.time);
        dur = maxT + 2.0;
    } else {
        dur = m_clip ? std::max(0.05, m_clip->dur) : 1.0;
    }
    const int w = std::max(1, width() - 8);
    const double f = std::clamp((x - 4) / (double)w, 0.0, 1.0);
    return f * dur;
}

int KeyframeStrip::tToX(double t) const {
    double dur;
    if (m_mesaMode || m_camMode) {
        const QVector<Keyframe>* ks = m_camMode ? keysForCamera(m_mesaCamera, m_prop)
                                                : keysForTrack(m_mesaTrack, m_prop);
        double maxT = 1.0;
        if (ks) for (const Keyframe& k : *ks) maxT = qMax(maxT, k.time);
        dur = maxT + 2.0;
    } else {
        dur = m_clip ? std::max(0.05, m_clip->dur) : 1.0;
    }
    const int w = std::max(1, width() - 8);
    return 4 + (int)std::lround(std::clamp(t / dur, 0.0, 1.0) * w);
}

int KeyframeStrip::hitKey(const QPoint& p) const {
    const QVector<Keyframe>* ks = m_camMode ? keysForCamera(m_mesaCamera, m_prop)
                                : m_mesaMode ? keysForTrack(m_mesaTrack, m_prop)
                                             : keysFor(m_clip, m_prop);
    if (!ks) return -1;
    int best = -1;
    int bestD = 6;
    for (int i = 0; i < ks->size(); ++i) {
        const int d = std::abs(tToX((*ks)[i].time) - p.x());
        if (d <= 5 && d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void KeyframeStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    // Mini-timeline estilo Effect Controls do Premiere: área sempre escura,
    // separada da linha do nome por um traço fino.
    p.fillRect(rect(), themeColors().graphBg);
    p.setPen(QPen(themeColors().graphGrid, 1));
    p.drawLine(0, 0, width(), 0);

    const QVector<Keyframe>* ks = keys();
    if (!ks || ks->isEmpty()) return;

    const int midY = height() / 2;
    QColor ln = themeColors().graphGrid;
    ln.setAlpha(190);
    p.setPen(QPen(ln, 1));
    p.drawLine(4, midY, width() - 4, midY);

    const double rel = (m_mesaMode || m_camMode) ? std::max(0.0, m_playhead)
                                  : (m_clip ? std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur) : -1.0);
    if (rel >= 0.0) {
        const int x = tToX(rel);
        QColor ph = themeColors().graphHandle;
        ph.setAlpha(200);
        p.setPen(QPen(ph, 1));
        p.drawLine(x, 2, x, height() - 2);
    }

    for (int i = 0; i < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        const QPointF kp(tToX(k.time), midY);
        const bool active = rel >= 0.0 && std::fabs(k.time - rel) < 1e-6;
        const QColor fill = active ? QColor(255, 179, 64) : themeColors().graphKeyframe;
        drawKeyGlyph(p, kp, k.interp, 4.5, fill);
    }
}

void KeyframeStrip::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || (!m_clip && !m_mesaMode && !m_camMode)) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_dragIdx = hitKey(e->pos());
    m_moved = false;
    if (m_dragIdx >= 0) {
        const QVector<Keyframe>* ks = keys();
        if (ks) m_dragT0 = (*ks)[m_dragIdx].time;
        emit keyClicked(m_dragIdx);
        e->accept();
    }
}

void KeyframeStrip::mouseMoveEvent(QMouseEvent* e) {
    if ((!m_clip && !m_mesaMode && !m_camMode) || m_dragIdx < 0 || !(e->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    QVector<Keyframe>* kp = m_camMode ? keysForCamera(m_mesaCamera, m_prop)
                            : m_mesaMode ? keysForTrack(m_mesaTrack, m_prop)
                            : m_maskMode ? keysForMask(maskAt(m_clip, m_maskIndex), m_prop)
                                        : keysFor(m_clip, m_prop);
    if (!kp) return;
    QVector<Keyframe>& K = *kp;
    if (m_dragIdx >= K.size()) return;
    double dur;
    if (m_mesaMode || m_camMode) {
        double maxT = 1.0;
        for (const Keyframe& k : K) maxT = qMax(maxT, k.time);
        dur = maxT + 2.0;
    } else {
        dur = std::max(0.05, m_clip->dur);
    }
    const double t = std::clamp(xToT(e->pos().x()), 0.0, dur);
    if (!m_moved) {
        if (std::fabs(t - m_dragT0) < 1e-9) return;
        m_moved = true;
        emit dragStart(m_dragIdx);
    }
    emit dragKey(m_dragIdx, t);
    e->accept();
    update();
}

void KeyframeStrip::mouseReleaseEvent(QMouseEvent* e) {
    m_dragIdx = -1;
    m_moved = false;
    e->accept();
}

void KeyframeStrip::mouseDoubleClickEvent(QMouseEvent* e) {
    if ((!m_clip && !m_mesaMode && !m_camMode)) { QWidget::mouseDoubleClickEvent(e); return; }
    const int hit = hitKey(e->pos());
    if (hit < 0) {
        double dur;
        if (m_mesaMode || m_camMode) {
            const QVector<Keyframe>* ks = m_camMode
                ? keysForCamera(m_mesaCamera, m_prop)
                : keysForTrack(m_mesaTrack, m_prop);
            double maxT = 1.0;
            if (ks) for (const Keyframe& k : *ks) maxT = qMax(maxT, k.time);
            dur = maxT + 2.0;
        } else {
            dur = std::max(0.05, m_clip->dur);
        }
        const double t = std::clamp(xToT(e->pos().x()), 0.0, dur);
        emit addKey(t);
    }
    e->accept();
}

