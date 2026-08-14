// Pierrot — editor de vídeo estilo Vegas Pro
//
// Copyright (C) 2026 Pierrot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "GraphEditorWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QComboBox>
#include <QPushButton>
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
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr int kMarginL = 56;
constexpr int kMarginR = 16;
constexpr int kMarginT = 18;
constexpr int kMarginB = 22;

double niceStep(double raw) {
    if (raw <= 0) return 1.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double nice;
    if (norm < 1.5)      nice = 1.0;
    else if (norm < 3.0) nice = 2.0;
    else if (norm < 7.0) nice = 5.0;
    else                 nice = 10.0;
    return nice * mag;
}

QString propName(GraphProp p) {
    switch (p) {
        case GPropOpacity:   return "Opacity";
        case GPropVolume:    return "Volume";
        case GPropScale:     return "Scale";
        case GPropRotation:  return "Rotation";
        case GPropTx:        return "Position X";
        case GPropTy:        return "Position Y";
        case GPropCropL:     return "Crop Left";
        case GPropCropR:     return "Crop Right";
        case GPropCropT:     return "Crop Top";
        case GPropCropB:     return "Crop Bottom";
    }
    return QString();
}

QString interpName(int interp) {
    switch (interp) {
        case KfSmooth: return "Suave";
        case KfStep:   return "Segurar";
        case KfBezier: return "Bezier";
        default:       return "Linear";
    }
}

} // namespace

// --------------------------------------------------------------------------
// GraphCanvas
// --------------------------------------------------------------------------

GraphCanvas::GraphCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(140);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
}

void GraphCanvas::setData(Clip* clip, GraphProp prop, double playhead, double fps) {
    m_clip = clip;
    m_prop = prop;
    m_playhead = playhead;
    m_fps = (fps > 1.0) ? fps : 30.0;
    valueRange(&m_loProp, &m_hiProp);
    fitValueRange();
    m_dragKey = -1;
    m_dragHandle = -1;
    m_hoverKey = -1;
    m_undoPushed = false;
    m_selKeys.clear();
    m_selOrig.clear();
    m_marqueeActive = false;
    m_t0 = 0.0;
    m_t1 = -1.0;
    update();
}

QVector<Keyframe>* GraphCanvas::keys() const {
    if (!m_clip) return nullptr;
    switch (m_prop) {
        case GPropOpacity:  return &m_clip->kfOpacity;
        case GPropVolume:   return &m_clip->kfVolume;
        case GPropScale:    return &m_clip->kfScale;
        case GPropRotation: return &m_clip->kfRotation;
        case GPropTx:       return &m_clip->kfTx;
        case GPropTy:       return &m_clip->kfTy;
        case GPropCropL:    return &m_clip->kfCropL;
        case GPropCropR:    return &m_clip->kfCropR;
        case GPropCropT:    return &m_clip->kfCropT;
        case GPropCropB:    return &m_clip->kfCropB;
    }
    return nullptr;
}

double GraphCanvas::baseValue() const {
    if (!m_clip) return 0.0;
    switch (m_prop) {
        case GPropOpacity:  return m_clip->opacity;
        case GPropVolume:   return m_clip->volume;
        case GPropScale:    return m_clip->scale;
        case GPropRotation: return m_clip->rotation;
        case GPropTx:       return m_clip->tx;
        case GPropTy:       return m_clip->ty;
        case GPropCropL:    return m_clip->cropL;
        case GPropCropR:    return m_clip->cropR;
        case GPropCropT:    return m_clip->cropT;
        case GPropCropB:    return m_clip->cropB;
    }
    return 0.0;
}

void GraphCanvas::valueRange(double* lo, double* hi) const {
    switch (m_prop) {
        case GPropOpacity:  *lo = 0.0; *hi = 1.0; return;
        case GPropVolume:   *lo = 0.0; *hi = 2.0; return;
        case GPropScale:    *lo = 0.0; *hi = 3.0; return;
        case GPropRotation: *lo = -360.0; *hi = 360.0; return;
        case GPropTx: *lo = -800.0; *hi = 800.0; return;
        case GPropTy: *lo = -450.0; *hi = 450.0; return;
        case GPropCropL: case GPropCropR:
        case GPropCropT: case GPropCropB:
            *lo = 0.0; *hi = 1.0; return;
    }
    *lo = 0.0; *hi = 1.0;
}

QRect GraphCanvas::plotRect() const {
    return QRect(kMarginL, kMarginT,
                 std::max(10, width() - kMarginL - kMarginR),
                 std::max(10, height() - kMarginT - kMarginB));
}

double GraphCanvas::xToT(int x) const {
    const QRect r = plotRect();
    const double t0 = timeStart();
    const double range = timeRange();
    if (range <= 0) return t0;
    return t0 + std::clamp((x - r.left()) / (double)r.width(), 0.0, 1.0) * range;
}

int GraphCanvas::tToX(double t) const {
    const QRect r = plotRect();
    const double t0 = timeStart();
    const double range = timeRange();
    if (range <= 0) return r.left();
    const double f = (t - t0) / range;
    return r.left() + (int)std::lround(std::clamp(f, 0.0, 1.0) * r.width());
}

double GraphCanvas::timeStart() const {
    return (m_t1 > m_t0) ? m_t0 : 0.0;
}

double GraphCanvas::timeRange() const {
    const double dur = m_clip ? std::max(0.05, m_clip->dur) : 1.0;
    if (m_t1 > m_t0) return std::clamp(m_t1 - m_t0, 0.001, dur);
    return dur;
}

// Arredonda o tempo para a grade de frames (1/fps); a precisão livre fica
// disponível segurando Ctrl durante o arrasto.
double GraphCanvas::snapTime(double t) const {
    const double fr = 1.0 / std::max(1.0, m_fps);
    return std::round(t / fr) * fr;
}

QString GraphCanvas::fmtTime(double t) const {
    return QStringLiteral("%1s").arg(t, 0, 'f', 3);
}

QString GraphCanvas::fmtValue(double v) const {
    const int prec = (std::fabs(m_hi - m_lo) < 2.0) ? 4 : 5;
    return QString::number(v, 'g', prec);
}

double GraphCanvas::yToV(int y) const {
    const QRect r = plotRect();
    const double f = std::clamp((r.bottom() - y) / (double)r.height(), 0.0, 1.0);
    return m_lo + (m_hi - m_lo) * f;
}

int GraphCanvas::vToY(double v) const {
    const QRect r = plotRect();
    const double f = (std::clamp(v, m_lo, m_hi) - m_lo) / (m_hi - m_lo);
    return r.bottom() - (int)std::lround(f * r.height());
}

int GraphCanvas::keyframeHit(const QPoint& p) const {
    const QVector<Keyframe>* ks = keys();
    if (!ks) return -1;
    for (int i = 0; i < ks->size(); ++i) {
        const QPointF kp(tToX((*ks)[i].time), vToY((*ks)[i].value));
        if (std::hypot(p.x() - kp.x(), p.y() - kp.y()) <= 7.0) return i;
    }
    return -1;
}

int GraphCanvas::handleHit(const QPoint& p) const {
    const QVector<Keyframe>* ks = keys();
    if (!ks) return -1;
    for (int i = 0; i + 1 < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        if (k.interp != KfSmooth && k.interp != KfBezier) continue;
        const double span = (*ks)[i + 1].time - k.time;
        if (span <= 1e-9) continue;
        double cx = k.hx;
        if (k.interp == KfSmooth)
            cx = std::clamp(span * 0.35, 0.0, span);
        const QPointF hp(tToX(k.time + cx), vToY(k.value + k.hy));
        if (std::hypot(p.x() - hp.x(), p.y() - hp.y()) <= 8.0) return i;
    }
    return -1;
}

void GraphCanvas::sortKeys() {
    QVector<Keyframe>* ks = keys();
    if (!ks) return;
    std::sort(ks->begin(), ks->end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

int GraphCanvas::addKeyframe(double time, double value) {
    if (!m_clip) return -1;
    QVector<Keyframe>& K = *keys();
    for (const Keyframe& k : K)
        if (std::fabs(k.time - time) < 1e-9) return -1;
    Keyframe nk;
    nk.time = time;
    nk.value = value;
    nk.interp = KfSmooth;
    K.append(nk);
    sortKeys();
    fitValueRange();
    // Seleciona o keyframe recém-criado (índice = último com o tempo/valor).
    m_selKeys.clear();
    m_selOrig.clear();
    int found = -1;
    for (int i = 0; i < K.size(); ++i)
        if (std::fabs(K[i].time - time) < 1e-6
            && std::fabs(K[i].value - value) < 1e-6)
            found = i;
    if (found < 0)
        for (int i = 0; i < K.size(); ++i)
            if (std::fabs(K[i].time - time) < 1e-6) found = i;
    if (found < 0) found = K.size() - 1;
    m_selKeys.append(found);
    m_dragKey = found;
    m_dragHandle = -1;
    for (int i : m_selKeys) m_selOrig.append(K[i]);
    update();
    return found;
}

void GraphCanvas::commitChange() {
    // Ordena só se estiver fora de ordem (durante o arrasto a ordem é mantida
    // pelos clamps e reordenar invalidaria os índices da seleção múltipla).
    QVector<Keyframe>* ks = keys();
    if (ks) {
        for (int i = 1; i < ks->size(); ++i) {
            if ((*ks)[i].time < (*ks)[i - 1].time - 1e-9) { sortKeys(); break; }
        }
    }
    emit modified();
    update();
}

// Ajusta a faixa vertical visível aos valores atuais (com folga), mantendo-se
// dentro da faixa natural da propriedade. O fit é refeito quando os dados
// mudam; durante o arrasto a escala fica fixa para não desorientar.
void GraphCanvas::fitValueRange() {
    const QVector<Keyframe>* ks = keys();
    if (!ks || ks->isEmpty()) {
        const double b = baseValue();
        const double lo = std::min(b, m_loProp);
        const double hi = std::max(b, m_hiProp);
        const double pad = (hi - lo) * 0.5;
        m_lo = lo - pad;
        m_hi = hi + pad;
        return;
    }
    double mn = 1e18, mx = -1e18;
    for (int i = 0; i < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        mn = std::min(mn, k.value);
        mx = std::max(mx, k.value);
        // Inclui os extremos dos handles para a curva não estourar a janela.
        mn = std::min(mn, k.value + k.hy);
        mx = std::max(mx, k.value + k.hy);
        if (i + 1 < ks->size()) {
            mn = std::min(mn, (*ks)[i + 1].value - (*ks)[i + 1].hy);
            mx = std::max(mx, (*ks)[i + 1].value - (*ks)[i + 1].hy);
        }
    }
    const double pSpan = m_hiProp - m_loProp;
    if (pSpan > 0.0) {
        mn = std::clamp(mn, m_loProp, m_hiProp);
        mx = std::clamp(mx, m_loProp, m_hiProp);
    }
    double span = mx - mn;
    if (span < 1e-6) span = std::max(1e-6, 0.1 * pSpan);
    const double pad = span * 0.15;
    m_lo = mn - pad;
    m_hi = mx + pad;
    if (pSpan > 0.0) {
        m_lo = std::max(m_lo, m_loProp);
        m_hi = std::min(m_hi, m_hiProp);
    }
    if (m_hi <= m_lo) { m_lo = mn - span * 0.5; m_hi = mx + span * 0.5; }
}

void GraphCanvas::marqueeSelect(const QRect& r, bool add) {
    const QVector<Keyframe>* ks = keys();
    if (!ks) return;
    if (!add) m_selKeys.clear();
    for (int i = 0; i < ks->size(); ++i) {
        const QPointF kp(tToX((*ks)[i].time), vToY((*ks)[i].value));
        if (r.contains(kp.toPoint()) && !m_selKeys.contains(i))
            m_selKeys.append(i);
    }
    m_selOrig.clear();
    for (int i : m_selKeys) m_selOrig.append((*ks)[i]);
    update();
}

// Move todos os keyframes selecionados pelo mesmo delta, sem deixar que um
// cruze um vizinho fora da seleção (nem saia do clipe).
void GraphCanvas::moveSelected(double dT, double dV, bool snap) {
    if (!m_clip) return;
    QVector<Keyframe>& K = *keys();
    if (K.isEmpty() || m_selKeys.isEmpty() || m_selOrig.size() != m_selKeys.size()) return;
    const double dur = std::max(0.05, m_clip->dur);

    double minT = 1e18, maxT = -1e18;
    for (const Keyframe& o : m_selOrig) {
        minT = std::min(minT, o.time);
        maxT = std::max(maxT, o.time);
    }
    // Vizinhos imediatos fora da seleção (fixos durante o arrasto).
    double leftB = 0.0, rightB = dur;
    for (const Keyframe& k : K) {
        if (k.time < minT - 1e-9 && k.time > leftB) leftB = k.time;
        if (k.time > maxT + 1e-9 && k.time < rightB) rightB = k.time;
    }
    double d = dT;
    if (minT + d < leftB + 0.001) d = leftB + 0.001 - minT;
    if (maxT + d > rightB - 0.001) d = rightB - 0.001 - maxT;

    for (int j = 0; j < m_selKeys.size(); ++j) {
        Keyframe& k = K[m_selKeys[j]];
        const Keyframe& o = m_selOrig[j];
        double nt = o.time + d;
        if (snap) nt = snapTime(nt);
        nt = std::clamp(nt, leftB + 0.001, rightB - 0.001);
        k.time = nt;
        k.value = std::clamp(o.value + dV, m_loProp, m_hiProp);
    }
}

void GraphCanvas::updateHover(const QPoint& p) {
    const int hk = (m_clip && keys()) ? keyframeHit(p) : -1;
    if (hk == m_hoverKey) return;
    m_hoverKey = hk;
    if (m_hoverKey >= 0)
        emitKeyInfo(m_hoverKey);
    else
        emit statusMessage(QString());
    setCursor(m_hoverKey >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void GraphCanvas::emitKeyInfo(int idx) {
    const QVector<Keyframe>* ks = keys();
    if (!ks || idx < 0 || idx >= ks->size()) return;
    const Keyframe& k = (*ks)[idx];
    const int frame = (int)std::lround(k.time * m_fps);
    emit statusMessage(tr("Keyframe %1/%2  ·  t = %3 (frame %4)  ·  v = %5")
                           .arg(idx + 1)
                           .arg(ks->size())
                           .arg(fmtTime(k.time))
                           .arg(frame)
                           .arg(fmtValue(k.value)));
}

void GraphCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(28, 28, 32));
    const QRect r = plotRect();
    if (r.width() <= 0 || r.height() <= 0) return;

    // Grade de valores.
    p.setPen(QColor(50, 50, 58));
    const double vStep = niceStep((m_hi - m_lo) / 4.0);
    for (double v = std::ceil(m_lo / vStep) * vStep; v <= m_hi + 1e-9; v += vStep) {
        const int y = vToY(v);
        p.drawLine(r.left(), y, r.right(), y);
        p.setPen(QColor(150, 150, 160));
        p.drawText(QRect(0, y - 8, kMarginL - 6, 16),
                   Qt::AlignRight | Qt::AlignVCenter, fmtValue(v));
        p.setPen(QColor(50, 50, 58));
    }
    // Linha do zero mais visível.
    if (m_lo < 0.0 && m_hi > 0.0) {
        p.setPen(QColor(72, 72, 84));
        p.drawLine(r.left(), vToY(0.0), r.right(), vToY(0.0));
    }
    // Grade de tempo (respeita a janela visível).
    const double dur = m_clip ? std::max(0.05, m_clip->dur) : 1.0;
    const double t0 = timeStart();
    const double range = timeRange();
    const double tStep = niceStep(range / 6.0);
    for (double t = std::ceil(t0 / tStep) * tStep; t <= t0 + range + 1e-9; t += tStep) {
        const int x = tToX(t);
        p.drawLine(x, r.top(), x, r.bottom());
        p.setPen(QColor(150, 150, 160));
        const QString tlab = (tStep < 1.0)
            ? fmtTime(t)
            : QStringLiteral("%1s").arg(QString::number(t, 'g', 3));
        p.drawText(QRect(x - 24, r.bottom() + 4, 48, 16),
                   Qt::AlignHCenter | Qt::AlignVCenter, tlab);
        p.setPen(QColor(50, 50, 58));
    }

    const QVector<Keyframe>* ks = keys();
    if (!ks) return;

    // Curva (sem keyframes: linha base pontilhada).
    QPainterPath path;
    bool havePath = false;
    const int steps = std::max(2, r.width() / 2);
    for (int i = 0; i <= steps; ++i) {
        const double t = dur * i / steps;
        const double v = kfValue(*ks, baseValue(), t);
        const QPointF pt(tToX(t), vToY(v));
        if (!havePath) { path.moveTo(pt); havePath = true; }
        else path.lineTo(pt);
    }
    if (ks->isEmpty()) {
        p.setPen(QPen(QColor(110, 110, 120), 1, Qt::DashLine));
        p.drawLine(r.left(), vToY(baseValue()), r.right(), vToY(baseValue()));
    } else {
        p.setPen(QPen(QColor(90, 180, 255), 2));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawPath(path);
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // Handles ("tracinhos") quando há exatamente um keyframe selecionado.
    const bool singleSel = (m_selKeys.size() == 1);
    const int handleKey = singleSel ? m_selKeys.first() : -1;
    if (handleKey >= 0 && handleKey < ks->size()) {
        const Keyframe& k = (*ks)[handleKey];
        if (k.interp == KfSmooth || k.interp == KfBezier) {
            if (handleKey + 1 < ks->size()) {
                const double span = (*ks)[handleKey + 1].time - k.time;
                if (span > 1e-9) {
                    double cx = k.hx;
                    double cy = k.hy;
                    if (k.interp == KfSmooth) {
                        // Tangente Catmull-Rom (decorativa).
                        const Keyframe& a = k;
                        const Keyframe& b = (*ks)[handleKey + 1];
                        const Keyframe& p0 = (handleKey > 0) ? (*ks)[handleKey - 1] : a;
                        const double m0 = (b.value - p0.value) / (b.time - p0.time);
                        cx = span * 0.35;
                        cy = m0 * span * 0.35;
                    }
                    const QPointF base(tToX(k.time), vToY(k.value));
                    const QPointF hp(tToX(k.time + cx), vToY(k.value + cy));
                    p.setPen(QPen(QColor(255, 200, 90), 1.5, Qt::DashLine));
                    p.drawLine(base, hp);
                    p.setBrush(QColor(255, 200, 90));
                    p.setPen(Qt::NoPen);
                    p.drawEllipse(hp, 4, 4);
                }
            }
        }
    }

    // Keyframes (losangos; selecionados em verde).
    for (int i = 0; i < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        const QPointF kp(tToX(k.time), vToY(k.value));
        const bool sel = m_selKeys.contains(i);
        const QColor fill = sel ? QColor(90, 200, 120) : QColor(210, 210, 220);
        const int size = (sel && i == m_dragKey) ? 7 : 6;
        p.setBrush(fill);
        p.setPen(QPen(QColor(255, 255, 255), 1.2));
        const QPolygonF dia = QPolygonF() << QPointF(kp.x(), kp.y() - size)
                                          << QPointF(kp.x() + size, kp.y())
                                          << QPointF(kp.x(), kp.y() + size)
                                          << QPointF(kp.x() - size, kp.y());
        p.drawPolygon(dia);
    }

    // Leitura exata do keyframe sob o mouse ou sendo arrastado (tempo, frame
    // e valor), para saber com precisão onde cada keyframe está.
    const int infoKey = (m_dragKey >= 0) ? m_dragKey : m_hoverKey;
    if (infoKey >= 0 && infoKey < ks->size()) {
        const Keyframe& k = (*ks)[infoKey];
        const QPointF kp(tToX(k.time), vToY(k.value));
        const QString txt = tr("t = %1  ·  frame %2\nv = %3  ·  %4")
                                .arg(fmtTime(k.time))
                                .arg((int)std::lround(k.time * m_fps))
                                .arg(fmtValue(k.value))
                                .arg(interpName(k.interp));
        QRect box(kp.x() + 12, kp.y() - 34, 168, 36);
        if (box.right() > width() - 4)
            box.moveLeft(kp.x() - 12 - box.width());
        if (box.top() < 4)
            box.moveTop(kp.y() + 14);
        p.setPen(QPen(QColor(255, 200, 90), 1));
        p.setBrush(QColor(40, 40, 46, 232));
        p.drawRect(box);
        p.setPen(QColor(255, 235, 200));
        p.drawText(box, Qt::AlignCenter, txt);
    }

    // Caixa de seleção (marquee).
    if (m_marqueeActive) {
        const QRect mr = m_marqueeRect.normalized();
        if (!mr.isEmpty()) {
            p.fillRect(mr, QColor(120, 180, 255, 40));
            p.setPen(QPen(QColor(150, 200, 255, 230), 1, Qt::DashLine));
            p.drawRect(mr);
        }
    }

    // Linha do playhead (só se estiver na janela visível).
    if (m_clip) {
        const double rel = std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur);
        if (rel >= timeStart() - 1e-9 && rel <= timeStart() + timeRange() + 1e-9) {
            const int x = tToX(rel);
            p.setPen(QPen(QColor(0, 160, 255), 1));
            p.drawLine(x, r.top(), x, r.bottom());
        }
    }
}

void GraphCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !m_clip) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_lastPos = e->pos();
    updateHover(e->pos());
    const int kh = keyframeHit(e->pos());
    if (kh >= 0) {
        // Seleção múltipla: Shift adiciona/alterna; clique simples seleciona só este.
        const bool shift = e->modifiers() & Qt::ShiftModifier;
        if (m_selKeys.contains(kh)) {
            if (shift) {
                m_selKeys.removeAll(kh);
                m_selOrig.clear();
                for (int i : m_selKeys) m_selOrig.append((*keys())[i]);
                update();
                return;
            }
        } else {
            if (!shift) m_selKeys.clear();
            m_selKeys.append(kh);
        }
        m_dragKey = kh;
        m_dragHandle = -1;
        m_grabT = xToT(e->pos().x());
        m_grabV = yToV(e->pos().y());
        m_selOrig.clear();
        for (int i : m_selKeys) m_selOrig.append((*keys())[i]);
        emit editStart();
        m_undoPushed = true;
        setCursor(Qt::ClosedHandCursor);
        update();
        return;
    }
    const int hh = handleHit(e->pos());
    if (hh >= 0) {
        // Handle só com um keyframe selecionado.
        if (!m_selKeys.contains(hh)) { m_selKeys.clear(); m_selKeys.append(hh); }
        m_dragKey = hh;
        m_dragHandle = hh;
        QVector<Keyframe>& K = *keys();
        if (K[hh].interp == KfSmooth) {
            // Pegar o handle de um keyframe suave o converte em bezier
            // (os handles passam a ser manuais, como no DaVinci Resolve).
            K[hh].interp = KfBezier;
            emit editStart();
            m_undoPushed = true;
        }
        setCursor(Qt::SizeFDiagCursor);
        update();
        return;
    }
    // Clique em espaço vazio: inicia a caixa de seleção (Shift soma).
    m_dragKey = -1;
    m_dragHandle = -1;
    if (!(e->modifiers() & Qt::ShiftModifier)) {
        m_selKeys.clear();
        m_selOrig.clear();
    }
    m_marqueeActive = true;
    m_marqueeStart = e->pos();
    m_marqueeRect = QRect(e->pos(), QSize(0, 0));
    update();
    QWidget::mousePressEvent(e);
}

void GraphCanvas::mouseMoveEvent(QMouseEvent* e) {
    m_lastPos = e->pos();
    if (m_marqueeActive && (e->buttons() & Qt::LeftButton)) {
        m_marqueeRect = QRect(m_marqueeStart, e->pos()).normalized();
        update();
        return;
    }
    if (m_dragKey < 0 || !m_clip || !(e->buttons() & Qt::LeftButton)) {
        updateHover(e->pos());
        QWidget::mouseMoveEvent(e);
        return;
    }
    const QVector<Keyframe>* ks = keys();
    if (!ks || m_dragKey >= ks->size()) return;
    QVector<Keyframe>& K = *keys();

    if (m_dragHandle >= 0 && K[m_dragKey].interp == KfBezier
        && m_selKeys.size() == 1) {
        const Keyframe& k = K[m_dragKey];
        const double maxDx = (m_dragKey + 1 < K.size())
            ? (K[m_dragKey + 1].time - k.time) * 0.5 : timeRange() * 0.5;
        const double newDx = std::clamp(xToT(e->pos().x()) - k.time, 0.0, maxDx);
        const double newDy = std::clamp(yToV(e->pos().y()) - k.value,
                                        m_loProp - k.value, m_hiProp - k.value);
        K[m_dragKey].hx = newDx;
        K[m_dragKey].hy = newDy;
        emitKeyInfo(m_dragKey);
        commitChange();
        return;
    }

    // Arrasta o grupo selecionado (todos juntos, mantendo os deltas).
    const double dT = xToT(e->pos().x()) - m_grabT;
    const double dV = yToV(e->pos().y()) - m_grabV;
    const bool snap = m_snap && !(e->modifiers() & Qt::ControlModifier);
    moveSelected(dT, dV, snap);
    emitKeyInfo(m_dragKey);
    commitChange();
}

void GraphCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (m_marqueeActive) {
        m_marqueeActive = false;
        marqueeSelect(m_marqueeRect, e->modifiers() & Qt::ShiftModifier);
        m_marqueeRect = QRect();
        QWidget::mouseReleaseEvent(e);
        return;
    }
    if (m_dragKey >= 0) {
        m_dragKey = -1;
        m_dragHandle = -1;
        m_undoPushed = false;
        m_selOrig.clear();
        updateHover(e->pos());
        update();
    }
    QWidget::mouseReleaseEvent(e);
}

void GraphCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_clip) { QWidget::mouseDoubleClickEvent(e); return; }
    QVector<Keyframe>& K = *keys();

    // Duplo clique em cima de um keyframe: edita tempo/valor exatos.
    const int hit = keyframeHit(e->pos());
    if (hit >= 0) {
        const Keyframe k = K[hit];
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Editar keyframe"));
        auto* tSpin = new QDoubleSpinBox(&dlg);
        tSpin->setRange(0.0, m_clip->dur);
        tSpin->setDecimals(3);
        tSpin->setSingleStep(1.0 / m_fps);
        tSpin->setSuffix(tr(" s"));
        tSpin->setValue(k.time);
        auto* vSpin = new QDoubleSpinBox(&dlg);
        vSpin->setRange(m_loProp, m_hiProp);
        vSpin->setDecimals(4);
        vSpin->setValue(k.value);
        auto* form = new QFormLayout;
        form->addRow(tr("Tempo:"), tSpin);
        form->addRow(tr("Valor:"), vSpin);
        auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        auto* lay = new QVBoxLayout(&dlg);
        lay->addLayout(form);
        lay->addWidget(btns);
        if (dlg.exec() != QDialog::Accepted) return;

        const double nt = snapTime(tSpin->value());
        const double nv = vSpin->value();
        bool dup = false;
        for (int i = 0; i < K.size(); ++i)
            if (i != hit && std::fabs(K[i].time - nt) < 1e-9) { dup = true; break; }
        if (dup) { emit statusMessage(tr("Já existe um keyframe nesse tempo.")); return; }

        emit editStart();
        m_undoPushed = true;
        K[hit].time = nt;
        K[hit].value = nv;
        sortKeys();
        commitChange();
        return;
    }

    const double t = m_snap ? snapTime(xToT(e->pos().x())) : xToT(e->pos().x());
    const double v = kfValue(K, baseValue(), t);
    if (addKeyframe(t, v) < 0) {
        emit statusMessage(tr("Já existe um keyframe nesse tempo."));
        return;
    }
    emit editStart();
    commitChange();
    m_undoPushed = true;
}

void GraphCanvas::wheelEvent(QWheelEvent* e) {
    if (!m_clip || !(e->modifiers() & Qt::ControlModifier)) {
        QWidget::wheelEvent(e);
        return;
    }
    // Ctrl+roda: zoom horizontal (tempo) em torno do cursor.
    const double dur = std::max(0.05, m_clip->dur);
    const double factor = (e->angleDelta().y() > 0) ? 0.75 : 1.0 / 0.75;
    const double range = timeRange();
    const double tc = xToT(e->position().x());
    const double minR = std::min(0.02, 2.0 / m_fps);
    const double nr = std::clamp(range * factor, minR, dur);
    double t0 = tc - (tc - timeStart()) * (nr / range);
    t0 = std::clamp(t0, 0.0, dur - nr);
    m_t0 = t0;
    m_t1 = t0 + nr;
    update();
    e->accept();
}

void GraphCanvas::keyPressEvent(QKeyEvent* e) {
    QVector<Keyframe>* ks = keys();
    const bool ctrl = e->modifiers() & Qt::ControlModifier;

    if (e->key() == Qt::Key_F) { resetZoom(); e->accept(); return; }
    if (e->key() == Qt::Key_S && !ctrl && ks) {
        m_snap = !m_snap;
        emit snapChanged(m_snap);
        emit statusMessage(m_snap ? tr("Snap ligado (Ctrl = livre)")
                                  : tr("Snap desligado (Ctrl = livre)"));
        e->accept();
        return;
    }
    if (ctrl && e->key() == Qt::Key_A && ks) {
        m_selKeys.clear();
        for (int i = 0; i < ks->size(); ++i) m_selKeys.append(i);
        m_selOrig.clear();
        for (int i : m_selKeys) m_selOrig.append((*ks)[i]);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        m_selKeys.clear();
        m_selOrig.clear();
        m_dragKey = -1;
        m_dragHandle = -1;
        e->accept();
        return;
    }
    if (!ks || m_selKeys.isEmpty()) { QWidget::keyPressEvent(e); return; }

    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        emit editStart();
        QVector<int> sel = m_selKeys;
        std::sort(sel.begin(), sel.end(), std::greater<int>());
        for (int i : sel) ks->removeAt(i);
        m_selKeys.clear();
        m_selOrig.clear();
        m_dragKey = -1;
        fitValueRange();
        commitChange();
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right
        || e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
        // Nudge: 1 passo por tecla (Shift = 10). ←/→ tempo; ↑/↓ valor.
        const double frames = (e->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
        const double vStep = std::max(0.001, (m_hiProp - m_loProp) * 0.005);
        double dT = 0.0, dV = 0.0;
        switch (e->key()) {
        case Qt::Key_Left:  dT = -frames / m_fps; break;
        case Qt::Key_Right: dT =  frames / m_fps; break;
        case Qt::Key_Up:    dV =  frames * vStep; break;
        case Qt::Key_Down:  dV = -frames * vStep; break;
        }
        emit editStart();
        m_selOrig.clear();
        for (int i : m_selKeys) m_selOrig.append((*ks)[i]);
        moveSelected(dT, dV, false);
        if (m_dragKey >= 0) emitKeyInfo(m_dragKey);
        commitChange();
        e->accept();
        return;
    }

    // 1/2/3/4 definem a interpolação dos keyframes selecionados.
    if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_4 && !ctrl) {
        const int modes[4] = {KfLinear, KfSmooth, KfStep, KfBezier};
        const int mode = modes[e->key() - Qt::Key_1];
        emit editStart();
        for (int i : m_selKeys) {
            (*ks)[i].interp = mode;
            if (mode == KfSmooth) (*ks)[i].hx = (*ks)[i].hy = 0.0;
        }
        commitChange();
        e->accept();
        return;
    }

    QWidget::keyPressEvent(e);
}

void GraphCanvas::resetZoom() {
    m_t0 = 0.0;
    m_t1 = -1.0;
    update();
}

void GraphCanvas::setSnap(bool on) {
    if (m_snap == on) return;
    m_snap = on;
    emit snapChanged(m_snap);
    update();
}

bool GraphCanvas::snapEnabled() const {
    return m_snap;
}

void GraphCanvas::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_clip) { QWidget::contextMenuEvent(e); return; }
    QVector<Keyframe>& K = *keys();
    const int hit = keyframeHit(e->pos());
    if (hit < 0) {
        QMenu menu(this);
        QAction* addHere = menu.addAction(tr("Adicionar keyframe aqui"));
        QAction* selDel = nullptr;
        if (!m_selKeys.isEmpty()) {
            menu.addSeparator();
            selDel = menu.addAction(tr("Excluir keyframes selecionados"));
        }
        QAction* act = menu.exec(e->globalPos());
        if (!act) return;
        if (act == addHere) {
            const double t = m_snap ? snapTime(xToT(e->pos().x())) : xToT(e->pos().x());
            const double v = kfValue(K, baseValue(), t);
            if (addKeyframe(t, v) < 0) {
                emit statusMessage(tr("Já existe um keyframe nesse tempo."));
                return;
            }
            emit editStart();
            m_undoPushed = true;
            commitChange();
        } else if (act == selDel) {
            emit editStart();
            m_undoPushed = true;
            QVector<int> sel = m_selKeys;
            std::sort(sel.begin(), sel.end(), std::greater<int>());
            for (int i : sel) K.removeAt(i);
            m_selKeys.clear();
            m_dragKey = -1;
            fitValueRange();
            commitChange();
        }
        return;
    }
    if (!m_selKeys.contains(hit)) {
        m_selKeys.clear();
        m_selKeys.append(hit);
    }
    m_dragKey = hit;
    update();

    QMenu menu(this);
    QAction* lin = menu.addAction(tr("Linear"));
    QAction* smo = menu.addAction(tr("Suave"));
    QAction* ste = menu.addAction(tr("Segurar"));
    QAction* bez = menu.addAction(tr("Bezier"));
    menu.addSeparator();
    QAction* del = menu.addAction(m_selKeys.size() > 1
                                      ? tr("Excluir %1 keyframes").arg(m_selKeys.size())
                                      : tr("Excluir keyframe"));
    QAction* act = menu.exec(e->globalPos());
    if (!act) return;
    emit editStart();
    m_undoPushed = true;
    if (act == lin || act == smo || act == ste || act == bez) {
        for (int i : m_selKeys) {
            K[i].interp = (act == lin) ? KfLinear
                          : (act == smo) ? KfSmooth
                          : (act == ste) ? KfStep : KfBezier;
            if (act == smo) { K[i].hx = K[i].hy = 0.0; }
        }
    } else if (act == del) {
        QVector<int> sel = m_selKeys;
        std::sort(sel.begin(), sel.end(), std::greater<int>());
        for (int i : sel) K.removeAt(i);
        m_selKeys.clear();
        m_dragKey = -1;
    }
    commitChange();
}

// --------------------------------------------------------------------------
// GraphEditorWidget
// --------------------------------------------------------------------------

GraphEditorWidget::GraphEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* lbl = new QLabel(tr("Propriedade:"), this);
    m_propCombo = new QComboBox(this);
    connect(m_propCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        m_canvas->setData(activeClip(), currentProp(), m_playhead,
                          m_project ? m_project->fps : 30.0);
    });

    auto* addBtn = new QPushButton(tr("+ no playhead"), this);
    addBtn->setToolTip(tr("Adicionar keyframe no playhead"));
    connect(addBtn, &QPushButton::clicked, this, [this]() {
        Clip* c = activeClip();
        if (!c) return;
        const double rel0 = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        const double fr = 1.0 / std::max(1.0, m_project ? m_project->fps : 30.0);
        const double rel = std::round(rel0 / fr) * fr;
        QVector<Keyframe>& K = *m_canvas->keys();
        const double v = kfValue(K, m_canvas->baseValue(), rel);
        if (m_canvas->addKeyframe(rel, v) < 0) return;
        emit editStart();
        m_canvas->commitChange();
    });

    auto* delBtn = new QPushButton(tr("Excluir keyframe"), this);
    delBtn->setToolTip(tr("Excluir o keyframe mais próximo do playhead"));
    connect(delBtn, &QPushButton::clicked, this, [this]() {
        Clip* c = activeClip();
        if (!c) return;
        QVector<Keyframe>& K = *m_canvas->keys();
        if (K.isEmpty()) return;
        const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        int best = 0;
        double bestD = 1e18;
        for (int i = 0; i < K.size(); ++i) {
            const double d = std::fabs(K[i].time - rel);
            if (d < bestD) { bestD = d; best = i; }
        }
        emit editStart();
        K.removeAt(best);
        m_canvas->fitValueRange();
        m_canvas->commitChange();
    });

    auto* prevBtn = new QPushButton(tr("◀"), this);
    prevBtn->setToolTip(tr("Ir para o keyframe anterior desta propriedade"));
    auto* nextBtn = new QPushButton(tr("▶"), this);
    nextBtn->setToolTip(tr("Ir para o próximo keyframe desta propriedade"));
    auto* fitBtn = new QPushButton(tr("Ajustar"), this);
    fitBtn->setToolTip(tr("Mostrar o clipe inteiro (F; Ctrl+roda dá zoom)"));
    auto* snapBtn = new QPushButton(tr("Snap"), this);
    snapBtn->setCheckable(true);
    snapBtn->setChecked(true);
    snapBtn->setToolTip(tr("Encaixar nos frames (S; Ctrl durante o arrasto = livre)"));

    const auto jump = [this](int dir) {
        Clip* c = activeClip();
        QVector<Keyframe>* kp = m_canvas->keys();
        if (!c || !kp || kp->isEmpty()) return;
        QVector<Keyframe>& K = *kp;
        const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        int best = -1;
        if (dir < 0) {
            for (int i = 0; i < K.size(); ++i)
                if (K[i].time < rel - 1e-9) best = i;
            if (best < 0) best = K.size() - 1;
        } else {
            for (int i = K.size() - 1; i >= 0; --i)
                if (K[i].time > rel + 1e-9) best = i;
            if (best < 0) best = 0;
        }
        emit keyframeJump(c->pos + K[best].time);
    };
    connect(prevBtn, &QPushButton::clicked, this, [this, jump]() { jump(-1); });
    connect(nextBtn, &QPushButton::clicked, this, [this, jump]() { jump(1); });

    auto* top = new QHBoxLayout;
    top->setContentsMargins(6, 4, 6, 2);
    top->addWidget(lbl);
    top->addWidget(m_propCombo, 1);
    top->addWidget(addBtn);
    top->addWidget(delBtn);
    top->addSpacing(8);
    top->addWidget(prevBtn);
    top->addWidget(nextBtn);
    top->addWidget(fitBtn);
    top->addWidget(snapBtn);

    m_canvas = new GraphCanvas(this);
    connect(m_canvas, &GraphCanvas::editStart, this, &GraphEditorWidget::editStart);
    connect(m_canvas, &GraphCanvas::modified, this, &GraphEditorWidget::modified);
    connect(fitBtn, &QPushButton::clicked, m_canvas, &GraphCanvas::resetZoom);
    connect(snapBtn, &QPushButton::toggled, m_canvas, &GraphCanvas::setSnap);
    connect(m_canvas, &GraphCanvas::snapChanged, snapBtn, &QPushButton::setChecked);

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color:#9aa; font-size:11px; padding:2px 6px;"));
    connect(m_canvas, &GraphCanvas::statusMessage, m_status, &QLabel::setText);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(top);
    lay->addWidget(m_canvas, 1);
    lay->addWidget(m_status);
}

// Tamanho padrão do painel: evita que o dock abra com metade da tela na
// primeira execução (sem layout salvo ainda).
QSize GraphEditorWidget::sizeHint() const {
    return QSize(360, 220);
}

void GraphEditorWidget::setProject(Project* p) {
    m_project = p;
    rebuildProperties();
    m_canvas->setData(activeClip(), currentProp(), m_playhead,
                      m_project ? m_project->fps : 30.0);
}

void GraphEditorWidget::setClipId(const QString& id) {
    if (m_clipId == id) return;
    m_clipId = id;
    rebuildProperties();
    m_canvas->setData(activeClip(), currentProp(), m_playhead,
                      m_project ? m_project->fps : 30.0);
}

void GraphEditorWidget::setPlayhead(double t) {
    m_playhead = t;
    m_canvas->update();
}

void GraphEditorWidget::refresh() {
    rebuildProperties();
    m_canvas->setData(activeClip(), currentProp(), m_playhead,
                      m_project ? m_project->fps : 30.0);
}

Clip* GraphEditorWidget::activeClip() const {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    return nullptr;
}

GraphProp GraphEditorWidget::currentProp() const {
    const int idx = m_propCombo->currentIndex();
    if (idx >= 0 && idx < m_props.size()) return m_props[idx];
    return GPropOpacity;
}

void GraphEditorWidget::rebuildProperties() {
    const QSignalBlocker b(m_propCombo);
    m_propCombo->clear();
    m_props.clear();

    Clip* c = activeClip();
    if (!c) return;
    bool hasVideo = false, hasAudio = false;
    if (m_project) {
        const MediaItem* mi = m_project->findMedia(c->mediaId);
        if (mi) { hasVideo = mi->hasVideo; hasAudio = mi->hasAudio; }
    }
    if (hasVideo) {
        const QVector<GraphProp> vid = { GPropOpacity, GPropScale, GPropRotation,
                                         GPropTx, GPropTy,
                                         GPropCropL, GPropCropR, GPropCropT, GPropCropB };
        for (GraphProp p : vid) { m_props.append(p); m_propCombo->addItem(propName(p)); }
    }
    if (hasAudio) {
        m_props.append(GPropVolume);
        m_propCombo->addItem(propName(GPropVolume));
    }
    if (m_props.isEmpty()) {
        m_props.append(GPropOpacity);
        m_propCombo->addItem(propName(GPropOpacity));
    }
}
