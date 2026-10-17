// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ColorWheel.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QtGlobal>
#include <QtMath>
#include <cmath>

namespace {
constexpr double kTau = 6.283185307179586;

// Matiz (0..360) + saturação (0..1) → RGB puro (s=1,v=1), p/ desenhar o
// gradiente cônico de fundo da roda.
QColor hueColor(double deg, int alpha = 255) {
    QColor c;
    c.setHsvF(std::fmod(deg / 360.0, 1.0), 1.0, 1.0, alpha / 255.0);
    return c;
}
} // namespace

ColorWheel::ColorWheel(const QString& title, QWidget* parent)
    : QWidget(parent), m_title(title) {
    setMinimumSize(96, 96);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void ColorWheel::setOffset(const QVector2D& o) {
    const QVector2D clamped = QVector2D(qBound(-1.0f, o.x(), 1.0f),
                                        qBound(-1.0f, o.y(), 1.0f));
    if ((clamped - m_offset).lengthSquared() < 1e-6f) return;
    m_offset = clamped;
    update();
}

void ColorWheel::setMaster(double m) {
    m = qBound(-1.0, m, 1.0);
    if (qFuzzyCompare(m, m_master)) return;
    m_master = m;
    update();
}

QPointF ColorWheel::puckPos() const {
    const QPointF c(width() * 0.5, height() * 0.5);
    const double r = qMin(width(), height()) * 0.5 - 10.0;
    return c + QPointF(m_offset.x() * r, -m_offset.y() * r); // y tela invertido
}

void ColorWheel::setFromPos(const QPointF& p) {
    const QPointF c(width() * 0.5, height() * 0.5);
    const double r = qMin(width(), height()) * 0.5 - 10.0;
    double dx = (p.x() - c.x()) / r;
    double dy = -(p.y() - c.y()) / r; // inverte p/ eixo Y lógico p/ cima
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len > 1.0) { dx /= len; dy /= len; } // trava no anel
    const QVector2D no = QVector2D(float(dx), float(dy));
    if ((no - m_offset).lengthSquared() < 1e-6f) return;
    m_offset = no;
    emit offsetChanged(m_offset);
    update();
}

void ColorWheel::reset() {
    if (m_offset.isNull() && qFuzzyIsNull(m_master)) return;
    m_offset = QVector2D();
    m_master = 0.0;
    emit offsetChanged(m_offset);
    emit masterChanged(m_master);
    emit adjusted();
    update();
}

void ColorWheel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QPointF c(width() * 0.5, height() * 0.5);
    const double r = qMin(width(), height()) * 0.5 - 6.0;

    // Fundo cônico: matiz ao redor, saturação radial (neutro no centro).
    {
        QRadialGradient grad(c, r);
        for (int i = 0; i <= 36; ++i) {
            const double deg = i * 10.0;
            grad.setColorAt(deg / 360.0, hueColor(deg, 40));
        }
        grad.setColorAt(0.0, QColor(40, 40, 44, 255));
        grad.setColorAt(1.0, hueColor(0, 40));
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawEllipse(c, r, r);
    }

    // Anel de bordo.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(20, 20, 24), 2.0));
    p.drawEllipse(c, r, r);

    // Cruz do centro (referência de neutro).
    p.setPen(QPen(QColor(120, 120, 128), 1.0));
    p.drawLine(QPointF(c.x() - 8, c.y()), QPointF(c.x() + 8, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - 8), QPointF(c.x(), c.y() + 8));

    // Puck: círculo com contorno, preenchido pela matiz da direção puxada.
    const QPointF pk = puckPos();
    const double len = m_offset.length();
    QColor fill = (len > 0.02f)
        ? hueColor(std::atan2(-double(m_offset.y()), double(m_offset.x())) * 180.0 / M_PI)
        : QColor(180, 180, 186);
    fill.setAlphaF(0.35 + 0.55 * len);
    p.setPen(QPen(QColor(235, 235, 240), 1.5));
    p.setBrush(fill);
    p.drawEllipse(pk, 8.0, 8.0);

    // Título.
    p.setPen(QColor(210, 210, 216));
    p.setFont(QFont(QStringLiteral("Sans"), 8, QFont::Bold));
    p.drawText(QRectF(0, height() - 14, width(), 14),
               Qt::AlignCenter, m_title);

    // Indicador de master (brilho) como arco no topo.
    if (!qFuzzyIsNull(m_master)) {
        const double frac = (m_master + 1.0) * 0.5; // 0..1
        QPen mp(QColor(m_master > 0 ? 255 : 90, m_master > 0 ? 235 : 170,
                       m_master > 0 ? 120 : 255), 3.0);
        p.setPen(mp);
        const double span = kTau * 0.25;
        const double start = -M_PI / 2.0 - span * 0.5;
        p.drawArc(QRectF(c.x() - r + 2, c.y() - r + 2, (r - 2) * 2, (r - 2) * 2),
                  int(start * 180.0 / M_PI * 16), int(frac * span * 180.0 / M_PI * 16));
    }
}

void ColorWheel::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        setFromPos(e->position());
    } else if (e->button() == Qt::RightButton) {
        reset();
    }
}

void ColorWheel::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragging) setFromPos(e->position());
}

void ColorWheel::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        emit adjusted();
    }
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent*) {
    reset();
}
