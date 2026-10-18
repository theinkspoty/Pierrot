// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/audio/EqCurveView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include "colombina/audio/EqBiquad.h"
#include "colombina/models/Project.h"

namespace {

constexpr double kFmin = 20.0;
constexpr double kFmax = 20000.0;
constexpr double kFs = 48000.0;

QString freqText(double f) {
    if (f >= 1000.0) {
        const double k = f / 1000.0;
        return (k == std::floor(k)) ? QStringLiteral("%1k").arg((int)k)
                                    : QStringLiteral("%1k").arg(k, 0, 'f', 1);
    }
    return QString::number((int)f);
}

} // namespace

EqCurveView::EqCurveView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(170);
}

void EqCurveView::setNodes(const QVector<EqNode>& nodes) {
    m_nodes = nodes;
    if (m_selected >= m_nodes.size()) m_selected = -1;
    update();
}

void EqCurveView::setGainRange(double maxDb) {
    m_maxDb = std::max(1.0, maxDb);
    update();
}

void EqCurveView::setInteractive(bool on) { m_interactive = on; }

void EqCurveView::setSelected(int index) {
    m_selected = (index >= -1 && index < m_nodes.size()) ? index : -1;
    update();
}

QRect EqCurveView::plotRect() const {
    return QRect(int(m_padL), int(m_padT),
                 std::max(1, width() - int(m_padL + m_padR)),
                 std::max(1, height() - int(m_padT + m_padB)));
}

double EqCurveView::xForFreq(double f) const {
    const QRect r = plotRect();
    const double l0 = std::log10(kFmin);
    const double l1 = std::log10(kFmax);
    const double t = (std::log10(std::clamp(f, kFmin, kFmax)) - l0) / (l1 - l0);
    return r.left() + t * r.width();
}

double EqCurveView::freqForX(double x) const {
    const QRect r = plotRect();
    const double l0 = std::log10(kFmin);
    const double l1 = std::log10(kFmax);
    const double t =
        std::clamp((x - r.left()) / std::max(1, r.width()), 0.0, 1.0);
    return std::pow(10.0, l0 + t * (l1 - l0));
}

double EqCurveView::yForGain(double g) const {
    const QRect r = plotRect();
    const double t = (std::clamp(g, -m_maxDb, m_maxDb) + m_maxDb) / (2.0 * m_maxDb);
    return r.bottom() - t * r.height();
}

double EqCurveView::gainForY(double y) const {
    const QRect r = plotRect();
    const double t =
        std::clamp((r.bottom() - y) / std::max(1, r.height()), 0.0, 1.0);
    return -m_maxDb + t * (2.0 * m_maxDb);
}

int EqCurveView::nodeAt(const QPoint& p) const {
    int best = -1;
    double bd = 12.0 * 12.0;
    for (int i = 0; i < m_nodes.size(); ++i) {
        const double dx = xForFreq(m_nodes[i].freq) - p.x();
        const double dy = yForGain(m_nodes[i].gain) - p.y();
        const double d = dx * dx + dy * dy;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

void EqCurveView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRect r = plotRect();
    const QColor accent(46, 196, 138);
    const QColor grid = palette().color(QPalette::Mid);
    const QColor text = palette().color(QPalette::Text);
    const QColor base = palette().color(QPalette::Base);

    p.fillRect(rect(), palette().color(QPalette::Window));
    p.fillRect(r, base);

    QFont f = p.font();
    f.setPointSizeF(7.5);
    p.setFont(f);

    const double vf[] = { 20, 50, 100, 200, 500, 1000,
                          2000, 5000, 10000, 20000 };

    // Grade.
    p.setPen(grid);
    for (double fq : vf) {
        const int x = int(xForFreq(fq));
        p.drawLine(x, r.top(), x, r.bottom());
    }
    for (double g = -m_maxDb; g <= m_maxDb + 1e-6; g += m_maxDb / 2.0) {
        const int y = int(yForGain(g));
        p.drawLine(r.left(), y, r.right(), y);
    }
    // Linha de 0 dB destacada.
    const int y0 = int(yForGain(0.0));
    p.setPen(QPen(grid.darker(130), 1.2));
    p.drawLine(r.left(), y0, r.right(), y0);

    // Rótulos.
    p.setPen(text);
    for (double fq : vf) {
        const int x = int(xForFreq(fq));
        p.drawText(QRect(x - 24, r.bottom() + 2, 48, 16), Qt::AlignHCenter,
                   freqText(fq));
    }
    for (double g = -m_maxDb; g <= m_maxDb + 1e-6; g += m_maxDb / 2.0) {
        const int y = int(yForGain(g));
        p.drawText(QRect(0, y - 8, r.left() - 4, 16),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1").arg((int)std::lround(g)));
    }

    // Coeficientes dos biquads (uma vez) para avaliar a resposta por coluna.
    QVector<EqBiquad> bq;
    for (const EqNode& n : m_nodes) {
        if (!n.enabled) continue;
        EqBiquad q;
        if (n.type == EqLowShelf)
            q.lowShelf(n.freq, n.gain, kFs);
        else if (n.type == EqHighShelf)
            q.highShelf(n.freq, n.gain, kFs);
        else
            q.peaking(n.freq, n.q, n.gain, kFs);
        bq.append(q);
    }

    QPainterPath path, fill;
    bool first = true;
    for (int x = r.left(); x <= r.right(); ++x) {
        const double fr = freqForX(x);
        double sum = 0.0;
        for (const EqBiquad& q : bq) sum += q.magnitudeDb(fr, kFs);
        sum = std::clamp(sum, -m_maxDb, m_maxDb);
        const double y = yForGain(sum);
        if (first) {
            path.moveTo(x, y);
            fill.moveTo(x, y0);
            fill.lineTo(x, y);
            first = false;
        } else {
            path.lineTo(x, y);
            fill.lineTo(x, y);
        }
    }
    if (!first) {
        fill.lineTo(r.right(), y0);
        fill.closeSubpath();
        QColor fa = accent;
        fa.setAlpha(40);
        p.fillPath(fill, fa);
        p.setPen(QPen(accent, 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    // Nós.
    for (int i = 0; i < m_nodes.size(); ++i) {
        const EqNode& n = m_nodes[i];
        const int x = int(xForFreq(n.freq));
        const int y = int(yForGain(n.gain));
        const bool sel = (i == m_selected);
        const QColor col = n.enabled ? accent : grid;
        p.setPen(QPen(sel ? text.darker(130) : col, sel ? 2.0 : 1.5));
        p.setBrush(sel ? QBrush(accent) : QBrush(base));
        if (n.type == EqBell) {
            const int rad = sel ? 6 : 5;
            p.drawEllipse(QPoint(x, y), rad, rad);
        } else {
            const int s = sel ? 6 : 5;
            p.drawRect(QRect(x - s, y - s, 2 * s, 2 * s));
        }
    }
}

void EqCurveView::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const int idx = nodeAt(e->pos());
    m_selected = idx;
    emit nodeSelected(idx);
    if (idx >= 0 && m_interactive) {
        m_drag = idx;
        emit editStart();
    }
    update();
}

void EqCurveView::mouseMoveEvent(QMouseEvent* e) {
    if (m_drag < 0 || !m_interactive) return;
    EqNode& n = m_nodes[m_drag];
    n.gain = std::clamp(gainForY(e->pos().y()), -m_maxDb, m_maxDb);
    if (n.movableFreq)
        n.freq = std::clamp(freqForX(e->pos().x()), kFmin, kFmax);
    emit nodeMoved(m_drag, n.freq, n.gain);
    update();
}

void EqCurveView::mouseReleaseEvent(QMouseEvent*) { m_drag = -1; }
