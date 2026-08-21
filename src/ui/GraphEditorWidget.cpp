// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "GraphEditorWidget.h"
#include "SettingsDialog.h"

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
#include <QFrame>
#include <QPixmap>
#include <QIcon>
#include <QFont>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr int kMarginL = 10;
constexpr int kMarginR = 8;
constexpr int kMarginT = 5;
constexpr int kMarginB = 14;
constexpr int kRulerH = 16;

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
        case GPropOpacity:   return QStringLiteral("Opacity");
        case GPropVolume:    return QStringLiteral("Volume");
        case GPropScale:     return QStringLiteral("Scale");
        case GPropScaleX:    return QStringLiteral("Flatt X");
        case GPropScaleY:    return QStringLiteral("Flatt Y");
        case GPropRotation:  return QStringLiteral("Rotation");
        case GPropTx:        return QStringLiteral("Position X");
        case GPropTy:        return QStringLiteral("Position Y");
        case GPropCropL:     return QStringLiteral("Crop Left");
        case GPropCropR:     return QStringLiteral("Crop Right");
        case GPropCropT:     return QStringLiteral("Crop Top");
        case GPropCropB:     return QStringLiteral("Crop Bottom");
    }
    return QString();
}

QString interpName(int interp) {
    switch (interp) {
        case KfSmooth: return QStringLiteral("Auto Bezier");
        case KfStep:   return QStringLiteral("Hold");
        case KfBezier: return QStringLiteral("Bezier");
        default:       return QStringLiteral("Linear");
    }
}

// Acessores compartilhados (editor + minifaixa) para os keyframes de uma
// propriedade de um clipe.
QVector<Keyframe>* keysFor(Clip* c, GraphProp p) {
    if (!c) return nullptr;
    switch (p) {
        case GPropOpacity:  return &c->kfOpacity;
        case GPropVolume:   return &c->kfVolume;
        case GPropScale:    return &c->kfScale;
        case GPropScaleX:   return &c->kfScaleX;
        case GPropScaleY:   return &c->kfScaleY;
        case GPropRotation: return &c->kfRotation;
        case GPropTx:       return &c->kfTx;
        case GPropTy:       return &c->kfTy;
        case GPropCropL:    return &c->kfCropL;
        case GPropCropR:    return &c->kfCropR;
        case GPropCropT:    return &c->kfCropT;
        case GPropCropB:    return &c->kfCropB;
    }
    return nullptr;
}

double baseFor(Clip* c, GraphProp p) {
    if (!c) return 0.0;
    switch (p) {
        case GPropOpacity:  return c->opacity;
        case GPropVolume:   return c->volume;
        case GPropScale:    return c->scale;
        case GPropScaleX:   return c->scaleX;
        case GPropScaleY:   return c->scaleY;
        case GPropRotation: return c->rotation;
        case GPropTx:       return c->tx;
        case GPropTy:       return c->ty;
        case GPropCropL:    return c->cropL;
        case GPropCropR:    return c->cropR;
        case GPropCropT:    return c->cropT;
        case GPropCropB:    return c->cropB;
    }
    return 0.0;
}

void rangeFor(GraphProp p, double* lo, double* hi) {
    switch (p) {
        case GPropOpacity:  *lo = 0.0; *hi = 1.0; return;
        case GPropVolume:   *lo = 0.0; *hi = 2.0; return;
        case GPropScale:    *lo = 0.0; *hi = 3.0; return;
        case GPropScaleX: case GPropScaleY:
            *lo = 0.2; *hi = 3.0; return;
        case GPropRotation: *lo = -360.0; *hi = 360.0; return;
        case GPropTx: *lo = -800.0; *hi = 800.0; return;
        case GPropTy: *lo = -450.0; *hi = 450.0; return;
        case GPropCropL: case GPropCropR:
        case GPropCropT: case GPropCropB:
            *lo = 0.0; *hi = 1.0; return;
    }
    *lo = 0.0; *hi = 1.0;
}

// Glifo do keyframe conforme a interpolação (estilo Premiere):
// Linear = losango agudo, Auto Bezier = círculo, Bezier = losango arredondado,
// Hold = "casa" com topo reto.
void drawKeyGlyph(QPainter& p, const QPointF& c, int interp, qreal s,
                  const QBrush& fill) {
    p.setBrush(fill);
    p.setPen(QPen(QColor(255, 255, 255), 1));
    switch (interp) {
    case KfStep: {
        const QPolygonF pent = QPolygonF()
            << QPointF(c.x() - s * 0.85, c.y() - s * 0.9)
            << QPointF(c.x() + s * 0.85, c.y() - s * 0.9)
            << QPointF(c.x() + s * 0.95, c.y() + s * 0.25)
            << QPointF(c.x(), c.y() + s)
            << QPointF(c.x() - s * 0.95, c.y() + s * 0.25);
        p.drawPolygon(pent);
        return;
    }
    case KfSmooth:
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawEllipse(c, s, s);
        p.setRenderHint(QPainter::Antialiasing, false);
        return;
    case KfBezier: {
        QPainterPath path;
        path.moveTo(c.x(), c.y() - s);
        path.quadTo(c.x() + s * 1.1, c.y() - s * 0.1, c.x(), c.y() + s);
        path.quadTo(c.x() - s * 1.1, c.y() + s * 0.1, c.x(), c.y() - s);
        path.closeSubpath();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillPath(path, fill);
        p.drawPath(path);
        p.setRenderHint(QPainter::Antialiasing, false);
        return;
    }
    case KfLinear:
    default: {
        const QPolygonF dia = QPolygonF()
            << QPointF(c.x(), c.y() - s)
            << QPointF(c.x() + s, c.y())
            << QPointF(c.x(), c.y() + s)
            << QPointF(c.x() - s, c.y());
        p.drawPolygon(dia);
        return;
    }
    }
}

QString fmtRuler(double t, double range) {
    if (range <= 2.0)  return QString::number(t, 'f', 2);
    if (range <= 60.0) return QString::number(t, 'f', 1);
    return QString::number(t, 'g', 4);
}

QString fmtVal(double v, double span) {
    const int prec = (span < 2.0) ? 4 : (span < 50.0 ? 3 : 2);
    return QString::number(v, 'g', prec);
}

QIcon makeStopwatchIcon(bool on) {
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c = on ? QColor(82, 168, 255) : QColor(150, 155, 165);
    p.setPen(QPen(c, 1.4));
    p.setBrush(on ? QColor(82, 168, 255, 40) : Qt::NoBrush);
    p.drawEllipse(QPointF(8, 8), 5.2, 5.2);
    p.drawRect(QRectF(7, 0, 2, 2.6));
    p.drawLine(QPointF(8, 8), QPointF(8, 4.6));
    p.drawLine(QPointF(8, 8), QPointF(10.6, 9.4));
    p.end();
    return QIcon(pm);
}

} // namespace

// --------------------------------------------------------------------------
// GraphCanvas
// --------------------------------------------------------------------------

// Tangente (Catmull-Rom) de um keyframe Auto Bezier, em valor/segundo.
// Nas pontas cai na secante — o mesmo que o kfValue() usa para KfSmooth.
static double autoTangent(const QVector<Keyframe>& K, int i) {
    if (i < 0 || i >= K.size()) return 0.0;
    const Keyframe& k = K[i];
    const Keyframe& prev = (i > 0) ? K[i - 1] : k;
    const Keyframe& next = (i + 1 < K.size()) ? K[i + 1] : k;
    const double dt = next.time - prev.time;
    if (dt <= 1e-9) return 0.0;
    return (next.value - prev.value) / dt;
}

// Ponto de controle do handle (saída se out==true, entrada se false) do
// keyframe i, para interpolações suave (Auto Bezier) e bezier manual.
// Para Auto Bezier o handle é calculado automaticamente ao longo da tangente;
// para Bezier manual são usados os handles gravados.
// ht/hv = (deslocamento em tempo, deslocamento em valor) a partir do keyframe.
// Retorna false se o keyframe não tem handle naquele lado (pontas ou não-bezier).
static bool bezierHandle(const QVector<Keyframe>& K, int i, bool out,
                         double* ht, double* hv) {
    if (i < 0 || i >= K.size()) return false;
    const Keyframe& k = K[i];
    if (k.interp == KfBezier) {
        if (out) {
            if (i + 1 >= K.size()) return false;
            *ht = k.ox; *hv = k.oy;
            return true;
        }
        if (i - 1 < 0) return false;
        *ht = k.ix; *hv = k.iy;
        return true;
    }
    // Suave, Linear e Degrau recebem uma alça padrão (tangente Catmull-Rom):
    // fica visível para o usuário entortar a curva, e ao arrastá-la o keyframe
    // é convertido para Bezier manual.
    if (k.interp == KfSmooth || k.interp == KfLinear || k.interp == KfStep) {
        const double span = out
            ? ((i + 1 < K.size()) ? K[i + 1].time - k.time : 0.0)
            : ((i - 1 >= 0) ? k.time - K[i - 1].time : 0.0);
        if (span <= 1e-9) return false;
        const double m = autoTangent(K, i);
        *ht = span / 3.0;
        *hv = out ? m * span : -m * span;
        return true;
    }
    return false;
}

GraphCanvas::GraphCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(84);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
}

void GraphCanvas::setData(Clip* clip, GraphProp prop, double playhead, double fps) {
    m_clip = clip;
    m_prop = prop;
    m_playhead = playhead;
    m_fps = (fps > 1.0) ? fps : 30.0;
    // Garante a ordem por tempo ao carregar/refrescar (defensivo contra
    // keyframes fora de ordem vindos de outras edições).
    if (m_clip && keys()) sortKeys();
    valueRange(&m_loProp, &m_hiProp);
    fitValueRange();
    m_dragKey = -1;
    m_dragHandle = -1;
    m_hoverKey = -1;
    m_curveNewKey = false;
    m_playheadDrag = false;
    m_undoPushed = false;
    m_selKeys.clear();
    m_selOrig.clear();
    m_marqueeActive = false;
    m_t0 = 0.0;
    m_t1 = -1.0;
    update();
}

QVector<Keyframe>* GraphCanvas::keys() const {
    return keysFor(m_clip, m_prop);
}

double GraphCanvas::baseValue() const {
    return baseFor(m_clip, m_prop);
}

void GraphCanvas::valueRange(double* lo, double* hi) const {
    rangeFor(m_prop, lo, hi);
}

QRect GraphCanvas::plotRect() const {
    return QRect(kMarginL, kRulerH + kMarginT,
                 std::max(10, width() - kMarginL - kMarginR),
                 std::max(10, height() - kRulerH - kMarginT - kMarginB));
}

QRect GraphCanvas::rulerRect() const {
    return QRect(kMarginL, 0,
                 std::max(10, width() - kMarginL - kMarginR), kRulerH);
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
    return fmtVal(v, m_hi - m_lo);
}

double GraphCanvas::yToV(int y) const {
    const QRect r = plotRect();
    const double f = std::clamp((r.bottom() - y) / (double)r.height(), 0.0, 1.0);
    return m_lo + (m_hi - m_lo) * f;
}

int GraphCanvas::vToY(double v) const {
    const QRect r = plotRect();
    const double span = std::max(1e-9, m_hi - m_lo);
    const double f = (std::clamp(v, m_lo, m_hi) - m_lo) / span;
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

int GraphCanvas::handleHit(const QPoint& p, int* side) const {
    const QVector<Keyframe>* ks = keys();
    if (!ks) return -1;
    for (int i = 0; i < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        for (int s = 0; s < 2; ++s) {
            double ht, hv;
            if (!bezierHandle(*ks, i, s == 0, &ht, &hv)) continue;
            const double tx = (s == 0) ? k.time + ht : k.time - ht;
            const double tv = k.value + hv;
            const QPointF hp(tToX(tx), vToY(tv));
            if (std::hypot(p.x() - hp.x(), p.y() - hp.y()) <= 8.0) {
                if (side) *side = s;
                return i;
            }
        }
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
    // Seleciona o keyframe recém-criado.
    m_selKeys.clear();
    m_selOrig.clear();
    int found = -1;
    for (int i = 0; i < K.size(); ++i)
        if (std::fabs(K[i].time - time) < 1e-9) { found = i; break; }
    if (found < 0) found = K.size() - 1;
    m_selKeys.append(found);
    m_dragKey = found;
    m_dragHandle = -1;
    for (int i : m_selKeys) m_selOrig.append(K[i]);
    update();
    return found;
}

void GraphCanvas::selectKeyIndex(int idx) {
    QVector<Keyframe>* ks = keys();
    m_selKeys.clear();
    m_selOrig.clear();
    m_marqueeActive = false;
    m_dragKey = -1;
    m_dragHandle = -1;
    if (ks && idx >= 0 && idx < ks->size()) m_selKeys.append(idx);
    update();
}

void GraphCanvas::selectKeysAtTimes(const QVector<double>& ts) {
    QVector<Keyframe>* ks = keys();
    m_selKeys.clear();
    m_selOrig.clear();
    m_marqueeActive = false;
    m_dragKey = -1;
    m_dragHandle = -1;
    if (ks) {
        for (double t : ts) {
            for (int i = 0; i < ks->size(); ++i) {
                if (std::fabs((*ks)[i].time - t) < 1e-6) {
                    m_selKeys.append(i);
                    break;
                }
            }
        }
    }
    update();
}

void GraphCanvas::clearSelection() {
    m_selKeys.clear();
    m_selOrig.clear();
    m_dragKey = -1;
    m_dragHandle = -1;
    m_marqueeActive = false;
    update();
}

QVector<double> GraphCanvas::selectionTimes() const {
    QVector<double> out;
    const QVector<Keyframe>* ks = keys();
    if (ks) {
        for (int i : m_selKeys)
            if (i >= 0 && i < ks->size()) out.append((*ks)[i].time);
    }
    return out;
}

void GraphCanvas::setZoom(double t0, double t1) {
    m_t0 = t0;
    m_t1 = t1;
    update();
}

void GraphCanvas::dragStripTime(int idx, double t) {
    if (!m_clip) return;
    QVector<Keyframe>& K = *keys();
    if (idx < 0 || idx >= K.size()) return;
    m_selKeys = { idx };
    m_selOrig = { K[idx] };
    m_grabT = K[idx].time;
    m_grabV = K[idx].value;
    moveSelected(t - m_grabT, 0.0, m_snap);
    commitChange();
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
    double mn, mx;
    if (!ks || ks->isEmpty()) {
        const double b = baseValue();
        mn = mx = b;
    } else {
        mn = 1e18; mx = -1e18;
        for (const Keyframe& k : *ks) {
            mn = std::min(mn, k.value);
            mx = std::max(mx, k.value);
            mn = std::min(mn, k.value + k.oy);
            mx = std::max(mx, k.value + k.oy);
            mn = std::min(mn, k.value + k.iy);
            mx = std::max(mx, k.value + k.iy);
        }
        mn = std::clamp(mn, m_loProp, m_hiProp);
        mx = std::clamp(mx, m_loProp, m_hiProp);
    }
    const double pSpan = std::max(1e-9, m_hiProp - m_loProp);
    // Folga mínima de 25% da faixa natural: quando o dado está num único ponto
    // ou na borda (ex.: opacidade/escala em 1.0), a linha não "some" no topo —
    // a janela fica centralizada e o valor continua no meio.
    double span = mx - mn;
    if (span < pSpan * 0.25) span = pSpan * 0.25;
    const double pad = span * 0.15;
    double lo = mn - pad;
    double hi = mx + pad;
    if (hi - lo < span) {
        const double mid = (mn + mx) / 2.0;
        lo = mid - span / 2.0;
        hi = mid + span / 2.0;
    }
    // Margem de 10% além da faixa natural para valores na borda continuarem
    // visíveis, sem deixar a janela escapar demais.
    const double margin = pSpan * 0.1;
    lo = std::max(lo, m_loProp - margin);
    hi = std::min(hi, m_hiProp + margin);
    if (hi <= lo) { lo = m_loProp; hi = m_hiProp; }
    m_lo = lo;
    m_hi = hi;
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
        // Movimento vertical restaurado: o valor acompanha o arraste.
        // A sensibilidade (Configurações → Editor de curvas) amplifica o
        // deslocamento vertical para permitir curvas mais exageradas.
        k.value = std::clamp(o.value + dV,
                             m_loProp, m_hiProp);
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
    emit statusMessage(tr("Keyframe %1/%2  ·  t = %3 (frame %4)  ·  v = %5  ·  %6")
                           .arg(idx + 1)
                           .arg(ks->size())
                           .arg(fmtTime(k.time))
                           .arg(frame)
                           .arg(fmtValue(k.value))
                           .arg(interpName(k.interp)));
}

void GraphCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(26, 26, 29));

    const double t0 = timeStart();
    const double range = timeRange();

    // Régua de tempo no topo.
    const QRect rr = rulerRect();
    p.fillRect(rr, QColor(24, 24, 27));
    p.setPen(QColor(58, 58, 66));
    p.drawLine(rr.left(), rr.bottom(), rr.right(), rr.bottom());
    const double tStep = niceStep(range / 6.0);
    p.setPen(QColor(150, 152, 160));
    for (double t = std::ceil(t0 / tStep) * tStep; t <= t0 + range + 1e-9; t += tStep) {
        const int x = tToX(t);
        p.drawLine(x, rr.bottom() - 3, x, rr.bottom());
        p.drawText(x + 2, rr.bottom() - 3, fmtRuler(t, range));
    }
    if (m_clip) {
        const double rel = std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur);
        if (rel >= t0 - 1e-9 && rel <= t0 + range + 1e-9) {
            const int x = tToX(rel);
            QFont f = p.font();
            f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(170, 200, 235));
            p.drawText(x + 7, rr.bottom() - 3,
                       tr("%1 · f%2").arg(fmtRuler(rel, range))
                                      .arg((int)std::lround(rel * m_fps)));
        }
    }

    const QRect r = plotRect();
    if (r.width() <= 0 || r.height() <= 0) return;

    // Fundo quadriculado de referência (grade sutil de tempo x valor), sem
    // preenchimento azul — só as linhas dão a noção de posição.
    p.setPen(QColor(255, 255, 255, 10));
    const double vStep = niceStep((m_hi - m_lo) / 5.0);
    for (double v = std::ceil(m_lo / vStep) * vStep; v <= m_hi + 1e-9; v += vStep)
        p.drawLine(r.left(), vToY(v), r.right(), vToY(v));
    const double tStepGrid = niceStep(range / 8.0);
    for (double t = std::ceil(t0 / tStepGrid) * tStepGrid; t <= t0 + range + 1e-9; t += tStepGrid) {
        const int x = tToX(t);
        p.drawLine(x, r.top(), x, r.bottom());
    }

    const QVector<Keyframe>* ks = keys();
    if (!ks) return;

    // Curva da propriedade desenhada como splines — uma cubic bezier por
    // segmento (igual ao Premiere), exata e suave, sem polígono amostrado.
    // O clipping na área do gráfico impede a curva de invadir régua/bordas.
    p.save();
    p.setClipRect(r);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    if (ks->isEmpty()) {
        // Sem keyframes: linha horizontal padrão da propriedade, sincronizada
        // com a duração do clipe (o usuário vê onde os pontos vão nascer).
        const int y = vToY(baseValue());
        p.setPen(QPen(QColor(130, 170, 200, 190), 1.6));
        p.drawLine(r.left(), y, r.right(), y);
        p.setPen(QColor(130, 170, 200, 90));
        p.drawText(r.left() + 4, y - 4, tr("valor padrão"));
    } else {
        const double win0 = timeStart();
        const double win1 = win0 + timeRange();
        const Keyframe& first = ks->first();
        const Keyframe& last = ks->last();
        bool started = false;
        auto addTo = [&](double t, double v) {
            const QPointF pt(tToX(t), vToY(v));
            if (!started) { path.moveTo(pt); started = true; }
            else path.lineTo(pt);
        };
        // Linha plana antes do primeiro keyframe (até a borda da janela).
        const double flat0 = std::min(win0, first.time);
        if (flat0 < first.time - 1e-9) {
            addTo(flat0, first.value);
            addTo(first.time, first.value);
        }
        for (int i = 0; i + 1 < ks->size(); ++i) {
            const Keyframe& a = (*ks)[i];
            const Keyframe& b = (*ks)[i + 1];
            const double span = b.time - a.time;
            if (!started) {
                path.moveTo(tToX(a.time), vToY(a.value));
                started = true;
            }
            switch (a.interp) {
                case KfStep:
                    addTo(b.time, a.value);
                    addTo(b.time, b.value);
                    break;
                case KfSmooth: {
                    const Keyframe& p0 = (i > 0) ? (*ks)[i - 1] : a;
                    const Keyframe& p3 = (i + 2 < ks->size()) ? (*ks)[i + 2] : b;
                    const double dt0 = (i > 0) ? (b.time - p0.time) : span;
                    const double m0 = (dt0 > 1e-9) ? (b.value - p0.value) / dt0 : 0.0;
                    const double dt3 = (i + 2 < ks->size()) ? (p3.time - a.time) : span;
                    const double m3 = (dt3 > 1e-9) ? (p3.value - a.value) / dt3 : 0.0;
                    path.cubicTo(tToX(a.time + span / 3.0), vToY(a.value + m0 * span),
                                 tToX(b.time - span / 3.0), vToY(b.value - m3 * span),
                                 tToX(b.time), vToY(b.value));
                    break;
                }
                case KfBezier: {
                    path.cubicTo(tToX(a.time + a.ox), vToY(a.value + a.oy),
                                 tToX(b.time - b.ix), vToY(b.value + b.iy),
                                 tToX(b.time), vToY(b.value));
                    break;
                }
                case KfLinear:
                default:
                    addTo(b.time, b.value);
                    break;
            }
        }
        // Linha plana depois do último keyframe (até a borda da janela).
        if (win1 > last.time + 1e-9) {
            addTo(last.time, last.value);
            addTo(std::max(win1, last.time), last.value);
        }
        p.setPen(QPen(QColor(110, 200, 255), 1.6));
        p.drawPath(path);
    }

    // Varinha (handles bezier) da curva: aparece ao selecionar UM keyframe,
    // para modelar a curva. Com seleção múltipla a linha fica limpa (sem a
    // "membrana"). Durante a CRIAÇÃO de curva (ferramenta curva) não desenha
    // alças — só a linha, que já se dobra enquanto você arrasta.
    if (m_selKeys.size() == 1 && !m_curveNewKey) {
        const int i = m_selKeys.first();
        if (i >= 0 && i < ks->size()) {
            const Keyframe& k = (*ks)[i];
            if (k.interp != KfStep) {
                const QPointF kp(tToX(k.time), vToY(k.value));
                for (int s = 0; s < 2; ++s) {
                    double ht, hv;
                    if (!bezierHandle(*ks, i, s == 0, &ht, &hv)) continue;
                    const QPointF hp(tToX(k.time + (s == 0 ? ht : -ht)),
                                     vToY(k.value + hv));
                    p.setPen(QPen(QColor(140, 200, 255, 220), 1.5, Qt::DashLine));
                    p.drawLine(kp, hp);
                    p.setPen(QPen(QColor(255, 255, 255), 1.5));
                    p.setBrush(QColor(90, 150, 220));
                    p.drawEllipse(hp, 4.5, 4.5);
                }
            }
        }
    }

    // Keyframes com glifos por interpolação.
    const double relPh = m_clip ? std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur) : -1.0;
    for (int i = 0; i < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        const QPointF kp(tToX(k.time), vToY(k.value));
        const bool sel = m_selKeys.contains(i);
        const bool active = relPh >= 0.0 && std::fabs(k.time - relPh) < 1e-6;
        const QColor fill = sel ? QColor(120, 230, 150)
                                : active ? QColor(255, 179, 64)
                                         : QColor(200, 205, 215);
        const qreal size = (sel && i == m_dragKey) ? 7 : 6;
        drawKeyGlyph(p, kp, k.interp, size, fill);
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    p.restore(); // fim do clipping na área do gráfico

    // O playhead no gráfico é só a linha (varinha) — sem círculo de valor nem
    // faixa de preenchimento, para o valor não "mover vertical com a agulha".

    // Leitura exata do keyframe sob o mouse ou sendo arrastado.
    const int infoKey = (m_dragKey >= 0) ? m_dragKey : m_hoverKey;
    if (infoKey >= 0 && infoKey < ks->size()) {
        const Keyframe& k = (*ks)[infoKey];
        const QPointF kp(tToX(k.time), vToY(k.value));
        const QString txt = tr("t = %1  ·  frame %2\nv = %3  ·  %4")
                                .arg(fmtTime(k.time))
                                .arg((int)std::lround(k.time * m_fps))
                                .arg(fmtValue(k.value))
                                .arg(interpName(k.interp));
        QRect box(kp.x() + 12, kp.y() - 30, 168, 34);
        if (box.right() > width() - 4)
            box.moveLeft(kp.x() - 12 - box.width());
        if (box.top() < rr.bottom() + 4)
            box.moveTop(kp.y() + 12);
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

    // Linha do playhead no gráfico (varinha), sem faixa de preenchimento.
    if (m_clip) {
        const double rel = std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur);
        if (rel >= timeStart() - 1e-9 && rel <= timeStart() + timeRange() + 1e-9) {
            const int x = tToX(rel);
            p.setPen(QPen(QColor(190, 220, 255, 230), 2));
            p.drawLine(x, rr.bottom() + 1, x, r.bottom());
        }
    }
}

void GraphCanvas::mousePressEvent(QMouseEvent* e) {
    setFocus(); // garante foco para o Delete apagar keyframes, não o clipe
    if (e->button() != Qt::LeftButton || !m_clip) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_lastPos = e->pos();
    updateHover(e->pos());
    // Clicou na régua: move/arrasta a agulha (igual às outras agulhas do app).
    if (e->pos().y() <= rulerRect().bottom()) {
        m_playheadDrag = true;
        m_dragKey = -1;
        m_dragHandle = -1;
        m_marqueeActive = false;
        setCursor(Qt::SizeHorCursor);
        emit keyframeJump(m_clip->pos + snapTime(xToT(e->pos().x())));
        e->accept();
        update();
        return;
    }
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
        e->accept();
        update();
        return;
    }
    int hSide = 0;
    const int hh = handleHit(e->pos(), &hSide);
    if (hh >= 0) {
        // Handle só com um keyframe selecionado.
        if (!m_selKeys.contains(hh)) { m_selKeys.clear(); m_selKeys.append(hh); }
        m_dragKey = hh;
        m_dragHandle = hh;
        m_dragSide = hSide;
        QVector<Keyframe>& K = *keys();
        if (K[hh].interp != KfBezier) {
            // Pegar a alça de qualquer keyframe (suave, linear ou degrau) o
            // converte em bezier manual, preservando a curva (handles =
            // tangente Catmull-Rom) e deixando o usuário entortar/fazer ondas.
            double ht0, hv0, ht1, hv1;
            bezierHandle(K, hh, true, &ht0, &hv0);
            bezierHandle(K, hh, false, &ht1, &hv1);
            K[hh].interp = KfBezier;
            K[hh].ox = ht0; K[hh].oy = hv0;
            K[hh].ix = ht1; K[hh].iy = hv1;
            emit editStart();
            m_undoPushed = true;
        }
        setCursor(Qt::SizeFDiagCursor);
        e->accept();
        update();
        return;
    }
    // Perto da linha do playhead (sem keyframe/handle por cima): arrasta a
    // agulha como nas outras janelas.
    if (m_tool == ToolSelect && m_clip) {
        const double rel = std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur);
        if (std::abs(e->pos().x() - tToX(rel)) <= 5) {
            m_playheadDrag = true;
            m_dragKey = -1;
            m_dragHandle = -1;
            m_marqueeActive = false;
            setCursor(Qt::SizeHorCursor);
            emit keyframeJump(m_clip->pos + snapTime(xToT(e->pos().x())));
            e->accept();
            update();
            return;
        }
    }
    // Ferramentas de adicionar/curva: clicar em espaço vazio cria um keyframe.
    if (m_tool != ToolSelect) {
        QVector<Keyframe>& K = *keys();
        const double t = snapTime(xToT(e->pos().x()));
        for (int i = 0; i < K.size(); ++i)
            if (std::fabs(K[i].time - t) < 1e-9) {
                m_selKeys.clear();
                m_selKeys.append(i);
                update();
                return;
            }
        Keyframe nk;
        nk.time = t;
        nk.value = (K.isEmpty() ? baseValue() : kfValue(K, baseValue(), t));
        nk.interp = (m_tool == ToolCurve) ? KfSmooth : KfLinear;
        nk.ox = nk.oy = nk.ix = nk.iy = 0.0;
        K.append(nk);
        sortKeys();
        int idx = -1;
        for (int i = 0; i < K.size(); ++i)
            if (std::fabs(K[i].time - t) < 1e-9) { idx = i; break; }
        m_selKeys.clear();
        m_selKeys.append(idx);
        emit editStart();
        m_undoPushed = true;
        m_dragKey = idx;
        m_dragHandle = (m_tool == ToolCurve) ? idx : -1;
        m_curveNewKey = (m_tool == ToolCurve);
        m_grabT = xToT(e->pos().x());
        m_grabV = yToV(e->pos().y());
        m_selOrig.clear();
        for (int i : m_selKeys) m_selOrig.append(K[i]);
        fitValueRange();
        e->accept();
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
    e->accept();
    update();
}

void GraphCanvas::mouseMoveEvent(QMouseEvent* e) {
    m_lastPos = e->pos();
    // Arrasto da agulha: faz scrub do playhead pela régua.
    if (m_playheadDrag && (e->buttons() & Qt::LeftButton) && m_clip) {
        emit keyframeJump(m_clip->pos + snapTime(xToT(e->pos().x())));
        e->accept();
        update();
        return;
    }
    if (m_marqueeActive && (e->buttons() & Qt::LeftButton)) {
        m_marqueeRect = QRect(m_marqueeStart, e->pos()).normalized();
        e->accept();
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

    // Ponto recém-criado pela ferramenta de curva (suave): o primeiro arrasto
    // o converte em bezier e define os handles, criando a curva.
    if (m_curveNewKey && m_dragHandle >= 0 && m_selKeys.size() == 1
        && K[m_dragKey].interp == KfSmooth) {
        K[m_dragKey].interp = KfBezier;
        const double maxDx = (m_dragKey + 1 < K.size())
            ? (K[m_dragKey + 1].time - K[m_dragKey].time) * 0.5
            : timeRange() * 0.5;
        const double dx = std::clamp(xToT(e->pos().x()) - K[m_dragKey].time,
                                     0.0, maxDx);
        const double dy = std::clamp(yToV(e->pos().y()) - K[m_dragKey].value,
                                     m_loProp - K[m_dragKey].value,
                                     m_hiProp - K[m_dragKey].value);
        const double spanR = (m_dragKey + 1 < K.size())
            ? K[m_dragKey + 1].time - K[m_dragKey].time
            : ((m_dragKey > 0)
                   ? K[m_dragKey].time - K[m_dragKey - 1].time
                   : timeRange());
        K[m_dragKey].ox = dx;
        K[m_dragKey].oy = dy;
        K[m_dragKey].ix = spanR / 3.0;
        K[m_dragKey].iy = 0.0;
        m_dragSide = 0;
        m_curveNewKey = false;
        setCursor(Qt::SizeFDiagCursor);
        emitKeyInfo(m_dragKey);
        commitChange();
        update();
        return;
    }

    if (m_dragHandle >= 0 && K[m_dragKey].interp == KfBezier
        && m_selKeys.size() == 1) {
        const Keyframe& k = K[m_dragKey];
        if (m_dragSide == 0) {
            const double maxDx = (m_dragKey + 1 < K.size())
                ? (K[m_dragKey + 1].time - k.time) * 0.5 : timeRange() * 0.5;
            const double newDx = std::clamp(xToT(e->pos().x()) - k.time,
                                            0.0, maxDx);
            const double newDy = std::clamp(yToV(e->pos().y()) - k.value,
                                            m_loProp - k.value,
                                            m_hiProp - k.value);
            K[m_dragKey].ox = newDx;
            K[m_dragKey].oy = newDy;
        } else {
            const double maxDx = (m_dragKey > 0)
                ? (k.time - K[m_dragKey - 1].time) * 0.5 : timeRange() * 0.5;
            const double newDx = std::clamp(k.time - xToT(e->pos().x()),
                                            0.0, maxDx);
            const double newDy = std::clamp(yToV(e->pos().y()) - k.value,
                                            m_loProp - k.value,
                                            m_hiProp - k.value);
            K[m_dragKey].ix = newDx;
            K[m_dragKey].iy = newDy;
        }
        emitKeyInfo(m_dragKey);
        commitChange();
        return;
    }

    // Arrasta o grupo selecionado (todos juntos, mantendo os deltas). O valor
    // segue o mouse 1:1 limitado à faixa visível — movimento rígido e previsível.
    const double dT = xToT(e->pos().x()) - m_grabT;
    const double dV = yToV(e->pos().y()) - m_grabV;
    const bool snap = m_snap && !(e->modifiers() & Qt::ControlModifier);
    moveSelected(dT, dV, snap);
    emitKeyInfo(m_dragKey);
    commitChange();
    e->accept();
}

void GraphCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (m_playheadDrag) {
        m_playheadDrag = false;
        setCursor(Qt::ArrowCursor);
        updateHover(e->pos());
        e->accept();
        update();
        return;
    }
    if (m_marqueeActive) {
        m_marqueeActive = false;
        marqueeSelect(m_marqueeRect, e->modifiers() & Qt::ShiftModifier);
        m_marqueeRect = QRect();
        e->accept();
        update();
        return;
    }
    if (m_dragKey >= 0) {
        m_dragKey = -1;
        m_dragHandle = -1;
        m_dragSide = 0;
        m_curveNewKey = false;
        m_undoPushed = false;
        m_selOrig.clear();
        updateHover(e->pos());
        e->accept();
        update();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void GraphCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_clip || m_tool != ToolSelect) {
        QWidget::mouseDoubleClickEvent(e);
        return;
    }
    // O press anterior iniciou uma marquee; sem isso, o release do segundo
    // clique chamaria marqueeSelect com retângulo vazio e DESELECIONARIA o
    // keyframe que o duplo clique acabou de criar.
    m_marqueeActive = false;
    m_marqueeRect = QRect();
    QVector<Keyframe>& K = *keys();

    // Duplo clique na régua não cria keyframe (a régua serve para a agulha).
    if (e->pos().y() <= rulerRect().bottom()) {
        e->accept();
        return;
    }

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

bool GraphCanvas::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(e);
        const int key = ke->key();
        // Intercepta Delete/Backspace sempre que o canvas estiver focado: o
        // atalho global do MainWindow não pode apagar o clipe enquanto o
        // usuário está editando keyframes (mesmo sem seleção).
        if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
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
        m_dragSide = 0;
        e->accept();
        return;
    }
    // V/P/B trocam a ferramenta ativa.
    if (e->key() == Qt::Key_V) { setTool(ToolSelect); e->accept(); return; }
    if (e->key() == Qt::Key_P) { setTool(ToolAdd); e->accept(); return; }
    if (e->key() == Qt::Key_B) { setTool(ToolCurve); e->accept(); return; }
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
            if (mode == KfSmooth) {
                (*ks)[i].ox = (*ks)[i].oy = 0.0;
                (*ks)[i].ix = (*ks)[i].iy = 0.0;
            }
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
            const double v = yToV(e->pos().y());
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
    QAction* bez = menu.addAction(tr("Bezier"));
    QAction* autoBez = menu.addAction(tr("Auto Bezier"));
    QAction* hold = menu.addAction(tr("Hold"));
    menu.addSeparator();
    QAction* del = menu.addAction(m_selKeys.size() > 1
                                      ? tr("Excluir %1 keyframes").arg(m_selKeys.size())
                                      : tr("Excluir keyframe"));
    QAction* act = menu.exec(e->globalPos());
    if (!act) return;
    emit editStart();
    m_undoPushed = true;
    if (act == lin || act == bez || act == autoBez || act == hold) {
        for (int i : m_selKeys) {
            K[i].interp = (act == lin) ? KfLinear
                          : (act == bez) ? KfBezier
                          : (act == autoBez) ? KfSmooth : KfStep;
            if (act == autoBez) { K[i].ox = K[i].oy = 0.0; K[i].ix = K[i].iy = 0.0; }
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
    const double dur = m_clip ? std::max(0.05, m_clip->dur) : 1.0;
    const int w = std::max(1, width() - 8);
    const double f = std::clamp((x - 4) / (double)w, 0.0, 1.0);
    return f * dur;
}

int KeyframeStrip::tToX(double t) const {
    const double dur = m_clip ? std::max(0.05, m_clip->dur) : 1.0;
    const int w = std::max(1, width() - 8);
    return 4 + (int)std::lround(std::clamp(t / dur, 0.0, 1.0) * w);
}

int KeyframeStrip::hitKey(const QPoint& p) const {
    const QVector<Keyframe>* ks = keysFor(m_clip, m_prop);
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
    const QVector<Keyframe>* ks = keysFor(m_clip, m_prop);
    if (!ks || ks->isEmpty()) return;

    const int midY = height() / 2;
    p.setPen(QPen(QColor(70, 74, 84), 1));
    p.drawLine(4, midY, width() - 4, midY);

    const double rel = m_clip ? std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur) : -1.0;
    if (rel >= 0.0) {
        const int x = tToX(rel);
        p.setPen(QPen(QColor(190, 220, 255, 200), 1));
        p.drawLine(x, 2, x, height() - 2);
    }

    for (int i = 0; i < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        const QPointF kp(tToX(k.time), midY);
        const bool active = rel >= 0.0 && std::fabs(k.time - rel) < 1e-6;
        const QColor fill = active ? QColor(255, 179, 64) : QColor(200, 205, 215);
        drawKeyGlyph(p, kp, k.interp, 4.5, fill);
    }
}

void KeyframeStrip::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !m_clip) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_dragIdx = hitKey(e->pos());
    m_moved = false;
    if (m_dragIdx >= 0) {
        const QVector<Keyframe>* ks = keysFor(m_clip, m_prop);
        if (ks) m_dragT0 = (*ks)[m_dragIdx].time;
        emit keyClicked(m_dragIdx);
        e->accept();
    }
}

void KeyframeStrip::mouseMoveEvent(QMouseEvent* e) {
    if (!m_clip || m_dragIdx < 0 || !(e->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    QVector<Keyframe>& K = *keysFor(m_clip, m_prop);
    if (m_dragIdx >= K.size()) return;
    const double t = std::clamp(xToT(e->pos().x()), 0.0, std::max(0.05, m_clip->dur));
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
    if (!m_clip) { QWidget::mouseDoubleClickEvent(e); return; }
    const int hit = hitKey(e->pos());
    if (hit < 0) {
        const double t = std::clamp(xToT(e->pos().x()), 0.0, std::max(0.05, m_clip->dur));
        emit addKey(t);
    }
    e->accept();
}

// --------------------------------------------------------------------------
// GraphPropRow
// --------------------------------------------------------------------------

GraphPropRow::GraphPropRow(GraphProp p, QWidget* parent) : QFrame(parent), m_prop(p) {
    setObjectName(QStringLiteral("propRow"));
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_expand = new QToolButton(this);
    m_expand->setAutoRaise(true);
    m_expand->setCheckable(true);
    m_expand->setText(QStringLiteral("▸"));
    m_expand->setCursor(Qt::PointingHandCursor);
    m_expand->setToolTip(tr("Mostrar/ocultar linha de keyframes"));

    m_name = new QLabel(propName(p), this);
    m_name->setAttribute(Qt::WA_TransparentForMouseEvents);

    m_value = new QLabel(this);
    m_value->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_value->setStyleSheet(QStringLiteral("color:#8a9; font-size:11px;"));

    m_stopwatch = new QToolButton(this);
    m_stopwatch->setAutoRaise(true);
    m_stopwatch->setCheckable(true);
    m_stopwatch->setIcon(makeStopwatchIcon(false));
    m_stopwatch->setCursor(Qt::PointingHandCursor);
    m_stopwatch->setToolTip(tr("Alternar animação"));

    m_prev = new QToolButton(this);
    m_prev->setAutoRaise(true);
    m_prev->setText(QStringLiteral("◀"));
    m_prev->setCursor(Qt::PointingHandCursor);
    m_prev->setToolTip(tr("Ir para o keyframe anterior"));

    m_add = new QToolButton(this);
    m_add->setAutoRaise(true);
    m_add->setText(QStringLiteral("◆"));
    m_add->setCursor(Qt::PointingHandCursor);
    m_add->setToolTip(tr("Adicionar/remover keyframe no playhead"));

    m_next = new QToolButton(this);
    m_next->setAutoRaise(true);
    m_next->setText(QStringLiteral("▶"));
    m_next->setCursor(Qt::PointingHandCursor);
    m_next->setToolTip(tr("Ir para o próximo keyframe"));

    m_navBox = new QWidget(this);
    auto* navLay = new QHBoxLayout(m_navBox);
    navLay->setContentsMargins(0, 0, 0, 0);
    navLay->setSpacing(0);
    navLay->addWidget(m_prev);
    navLay->addWidget(m_add);
    navLay->addWidget(m_next);
    m_navBox->setVisible(false);

    m_strip = new KeyframeStrip(this);
    m_strip->setVisible(false);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(2, 1, 2, 1);
    topRow->setSpacing(2);
    topRow->addWidget(m_expand);
    topRow->addWidget(m_name);
    topRow->addStretch(1);
    topRow->addWidget(m_value);
    topRow->addWidget(m_stopwatch);
    topRow->addWidget(m_navBox);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(topRow);
    lay->addWidget(m_strip);

    connect(m_expand, &QToolButton::toggled, this, [this](bool on) {
        m_strip->setVisible(on);
        updateExpandText();
    });
    connect(m_stopwatch, &QToolButton::toggled, this, [this](bool on) {
        m_stopwatch->setIcon(makeStopwatchIcon(on));
        emit stopwatchClicked();
    });
    connect(m_prev, &QToolButton::clicked, this, &GraphPropRow::prevKeyframe);
    connect(m_next, &QToolButton::clicked, this, &GraphPropRow::nextKeyframe);
    connect(m_add, &QToolButton::clicked, this, &GraphPropRow::toggleKeyframe);
    connect(m_strip, &KeyframeStrip::keyClicked, this, &GraphPropRow::stripKeyClicked);
    connect(m_strip, &KeyframeStrip::dragStart, this, &GraphPropRow::stripDragStart);
    connect(m_strip, &KeyframeStrip::dragKey, this, &GraphPropRow::stripDragKey);
    connect(m_strip, &KeyframeStrip::addKey, this, &GraphPropRow::stripAddKey);
}

void GraphPropRow::updateExpandText() {
    m_expand->setText(m_expand->isChecked() ? QStringLiteral("▾") : QStringLiteral("▸"));
}

void GraphPropRow::setAnimated(bool on) {
    const QSignalBlocker b(m_stopwatch);
    m_stopwatch->setChecked(on);
    m_stopwatch->setIcon(makeStopwatchIcon(on));
    m_navBox->setVisible(on);
}

void GraphPropRow::setActive(bool on) {
    if (on)
        setStyleSheet(QStringLiteral(
            "#propRow { background:#1f3145; border:1px solid #2e4a68; border-radius:3px; }"));
    else
        setStyleSheet(QStringLiteral(
            "#propRow { background:transparent; border:1px solid transparent; border-radius:3px; }"));
}

void GraphPropRow::setValueText(const QString& s) {
    m_value->setText(s);
}

void GraphPropRow::setStripData(Clip* clip, double fps) {
    m_strip->setData(clip, m_prop, fps);
}

void GraphPropRow::setStripPlayhead(double t) {
    m_strip->setPlayhead(t);
}

void GraphPropRow::setExpanded(bool on) {
    const QSignalBlocker b(m_expand);
    m_expand->setChecked(on);
    m_strip->setVisible(on);
    updateExpandText();
}

void GraphPropRow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) emit propertyClicked();
    QFrame::mousePressEvent(e);
}

// --------------------------------------------------------------------------
// GraphEditorWidget
// --------------------------------------------------------------------------

GraphEditorWidget::GraphEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* snapBtn = new QPushButton(tr("Snap"), this);
    snapBtn->setCheckable(true);
    snapBtn->setChecked(true);
    snapBtn->setToolTip(tr("Encaixar nos frames (S; Ctrl durante o arrasto = livre)"));

    auto* fitBtn = new QPushButton(tr("Ajustar"), this);
    fitBtn->setToolTip(tr("Mostrar o clipe inteiro (F; Ctrl+roda dá zoom)"));

    auto* topBar = new QHBoxLayout;
    topBar->setContentsMargins(6, 2, 6, 0);
    topBar->setSpacing(4);
    topBar->addWidget(snapBtn);
    topBar->addWidget(fitBtn);
    topBar->addSpacing(8);

    m_toolSel = new QToolButton(this);
    m_toolSel->setCheckable(true);
    m_toolSel->setChecked(true);
    m_toolSel->setText(tr("Selecionar"));
    m_toolSel->setToolTip(tr("Selecionar e mover keyframes (V)"));
    m_toolAdd = new QToolButton(this);
    m_toolAdd->setCheckable(true);
    m_toolAdd->setText(tr("Adicionar"));
    m_toolAdd->setToolTip(tr("Clicar no gráfico adiciona um keyframe (P)"));
    m_toolCurve = new QToolButton(this);
    m_toolCurve->setCheckable(true);
    m_toolCurve->setText(tr("Curva"));
    m_toolCurve->setToolTip(tr("Clicar cria um ponto suave; arrastar define a curva (B)"));

    auto* tools = new QButtonGroup(this);
    tools->setExclusive(true);
    tools->addButton(m_toolSel, ToolSelect);
    tools->addButton(m_toolAdd, ToolAdd);
    tools->addButton(m_toolCurve, ToolCurve);
    topBar->addWidget(m_toolSel);
    topBar->addWidget(m_toolAdd);
    topBar->addWidget(m_toolCurve);
    topBar->addStretch(1);

    m_canvas = new GraphCanvas(this);

    auto* canvasWrap = new QWidget(this);
    auto* cw = new QVBoxLayout(canvasWrap);
    cw->setContentsMargins(0, 0, 0, 0);
    cw->setSpacing(0);
    cw->addLayout(topBar);
    cw->addWidget(m_canvas, 1);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setMinimumWidth(160);

    auto* list = new QWidget(this);
    m_rowsLayout = new QVBoxLayout(list);
    m_rowsLayout->setContentsMargins(2, 2, 2, 2);
    m_rowsLayout->setSpacing(2);
    m_noClip = new QLabel(tr("Selecione um clipe na timeline para\neditar os keyframes."), list);
    m_noClip->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_noClip->setStyleSheet(QStringLiteral("color:#667; font-size:11px; padding:8px 4px;"));
    m_rowsLayout->addWidget(m_noClip);
    m_rowsLayout->addStretch(1);
    m_scroll->setWidget(list);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_scroll);
    splitter->addWidget(canvasWrap);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(5);
    splitter->setSizes({ 240, 600 });

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color:#9aa; font-size:11px; padding:2px 6px;"));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(splitter, 1);
    lay->addWidget(m_status);

    setMinimumHeight(170);

    connect(m_canvas, &GraphCanvas::editStart, this, &GraphEditorWidget::editStart);
    connect(m_canvas, &GraphCanvas::modified, this, &GraphEditorWidget::modified);
    connect(m_canvas, &GraphCanvas::statusMessage, m_status, &QLabel::setText);
    connect(m_canvas, &GraphCanvas::keyframeJump, this, &GraphEditorWidget::keyframeJump);
    connect(m_canvas, &GraphCanvas::snapChanged, snapBtn, &QPushButton::setChecked);
    connect(snapBtn, &QPushButton::toggled, m_canvas, &GraphCanvas::setSnap);
    connect(fitBtn, &QPushButton::clicked, m_canvas, &GraphCanvas::resetZoom);
    connect(tools, &QButtonGroup::idClicked, m_canvas, [this](int id) {
        m_canvas->setTool((CanvasTool)id);
    });
    connect(m_canvas, &GraphCanvas::toolChanged, this, [this, tools](CanvasTool t) {
        if (QAbstractButton* b = tools->button((int)t)) b->setChecked(true);
    });
}

// Tamanho padrão do painel: evita que o dock abra com metade da tela na
// primeira execução (sem layout salvo ainda).
QSize GraphEditorWidget::sizeHint() const {
    return QSize(520, 240);
}

void GraphEditorWidget::setProject(Project* p) {
    m_project = p;
    rebuildRows();
    m_canvas->setData(activeClip(), m_prop, m_playhead,
                      m_project ? m_project->fps : 30.0);
}

void GraphEditorWidget::setClipId(const QString& id) {
    if (m_clipId == id) return;
    m_clipId = id;
    rebuildRows();
    Clip* c = activeClip();
    if (c) {
        // Ao (re)selecionar o clipe, se a propriedade ativa não tiver keyframe
        // mas o clipe tiver em outra, mostra uma propriedade que tenha — assim
        // o editor não "esquece" os keyframes ao sair e voltar.
        if (keysFor(c, m_prop)->isEmpty()) {
            for (GraphProp p : m_props) {
                if (!keysFor(c, p)->isEmpty()) { m_prop = p; break; }
            }
        }
        if (!m_props.contains(m_prop)) m_prop = m_props.first();
    }
    m_canvas->setData(activeClip(), m_prop, m_playhead,
                      m_project ? m_project->fps : 30.0);
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (row) {
            row->setActive(p == m_prop);
            row->setExpanded(p == m_prop);
        }
    }
    syncValueLabels();
}

void GraphEditorWidget::setPlayhead(double t) {
    m_playhead = t;
    m_canvas->setPlayhead(t);
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (row) row->setStripPlayhead(t);
    }
    syncValueLabels();
}

void GraphEditorWidget::refresh() {
    // Se o tipo de mídia (vídeo/áudio) mudou, reconstrói a lista; senão só
    // atualiza os dados preservando zoom e seleção do gráfico.
    bool changed = false;
    {
        Clip* c = activeClip();
        bool hasVideo = false, hasAudio = false;
        if (c && m_project) {
            const MediaItem* mi = m_project->findMedia(c->mediaId);
            if (mi) { hasVideo = mi->hasVideo; hasAudio = mi->hasAudio; }
            // Clipe de texto não tem mídia, mas é animável como vídeo.
            if (c->isText) hasVideo = true;
        }
        bool haveVideo = false, haveAudio = false;
        for (GraphProp p : m_props) {
            if (p == GPropVolume) haveAudio = true; else haveVideo = true;
        }
        changed = (haveVideo != hasVideo) || (haveAudio != hasAudio);
    }
    if (changed) {
        rebuildRows();
        m_canvas->setData(activeClip(), m_prop, m_playhead,
                          m_project ? m_project->fps : 30.0);
        return;
    }

    const double zt0 = m_canvas->zoomT0();
    const double zt1 = m_canvas->zoomT1();
    const QVector<double> selTimes = m_canvas->selectionTimes();
    syncRowData();
    m_canvas->setData(activeClip(), m_prop, m_playhead,
                      m_project ? m_project->fps : 30.0);
    if (zt1 > zt0) m_canvas->setZoom(zt0, zt1);
    if (!selTimes.isEmpty()) m_canvas->selectKeysAtTimes(selTimes);
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

double GraphEditorWidget::propValueAtPlayhead(GraphProp p) const {
    Clip* c = activeClip();
    if (!c) return 0.0;
    const QVector<Keyframe>* K = keysFor(c, p);
    const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    return kfValue(*K, baseFor(c, p), rel);
}

void GraphEditorWidget::setProperty(GraphProp p) {
    if (!m_props.contains(p)) return;
    // Se já está mostrando essa curva, não recarrega (evita perder o zoom a
    // cada movimento de slider no pancrop).
    if (p == m_prop) return;
    m_prop = p;
    m_canvas->setData(activeClip(), p, m_playhead,
                      m_project ? m_project->fps : 30.0);
    for (GraphProp q : m_props) {
        GraphPropRow* row = m_rows.value((int)q);
        if (row) row->setActive(q == p);
    }
    if (GraphPropRow* row = m_rows.value((int)p)) row->setExpanded(true);
    syncValueLabels();
}

void GraphEditorWidget::syncRowData() {
    Clip* c = activeClip();
    const double fps = m_project ? m_project->fps : 30.0;
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (!row) continue;
        row->setStripData(c, fps);
        const QVector<Keyframe>* K = keysFor(c, p);
        row->setAnimated(K && !K->isEmpty());
        row->setActive(p == m_prop);
    }
    syncValueLabels();
}

void GraphEditorWidget::syncValueLabels() {
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (!row) continue;
        const double v = propValueAtPlayhead(p);
        double lo = 0.0, hi = 1.0;
        rangeFor(p, &lo, &hi);
        row->setValueText(fmtVal(v, hi - lo));
    }
}

void GraphEditorWidget::rebuildRows() {
    while (m_rowsLayout->count() > 2) {
        QLayoutItem* it = m_rowsLayout->takeAt(1);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    m_rows.clear();
    m_props.clear();

    Clip* c = activeClip();
    if (!c) {
        m_canvas->setData(nullptr, m_prop, m_playhead,
                          m_project ? m_project->fps : 30.0);
        m_noClip->setVisible(true);
        return;
    }
    bool hasVideo = false, hasAudio = false;
    if (m_project) {
        const MediaItem* mi = m_project->findMedia(c->mediaId);
        if (mi) { hasVideo = mi->hasVideo; hasAudio = mi->hasAudio; }
        // Clipe de texto não tem mídia, mas é animável como vídeo.
        if (c->isText) hasVideo = true;
    }
    if (hasVideo) {
        const QVector<GraphProp> vid = { GPropOpacity, GPropScale, GPropScaleX,
                                         GPropScaleY,
                                         GPropRotation,
                                         GPropTx, GPropTy,
                                         GPropCropL, GPropCropR, GPropCropT, GPropCropB };
        for (GraphProp p : vid) m_props.append(p);
    }
    if (hasAudio) m_props.append(GPropVolume);
    if (m_props.isEmpty()) m_props.append(GPropOpacity);
    if (!m_props.contains(m_prop)) m_prop = m_props.first();
    m_noClip->setVisible(false);

    for (GraphProp p : m_props) {
        auto* row = new GraphPropRow(p);
        m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, row);
        m_rows.insert((int)p, row);

        connect(row, &GraphPropRow::propertyClicked, this, [this, p]() { setProperty(p); });
        connect(row, &GraphPropRow::stopwatchClicked, this, [this, p]() { toggleAnimation(p); });
        connect(row, &GraphPropRow::prevKeyframe, this, [this, p]() { jumpKeyframe(p, -1); });
        connect(row, &GraphPropRow::nextKeyframe, this, [this, p]() { jumpKeyframe(p, 1); });
        connect(row, &GraphPropRow::toggleKeyframe, this, [this, p]() { toggleKeyAtPlayhead(p); });
        connect(row, &GraphPropRow::stripKeyClicked, this, [this, p](int idx) {
            setProperty(p);
            m_canvas->selectKeyIndex(idx);
        });
        connect(row, &GraphPropRow::stripDragStart, this, [this](int) { emit editStart(); });
        connect(row, &GraphPropRow::stripDragKey, this, [this, p](int idx, double t) {
            m_canvas->dragStripTime(idx, t);
        });
        connect(row, &GraphPropRow::stripAddKey, this, [this, p](double t) {
            Clip* c = activeClip();
            if (!c) return;
            const double rel = std::clamp(t, 0.0, c->dur);
            QVector<Keyframe>& K = *keysFor(c, p);
            for (const Keyframe& k : K)
                if (std::fabs(k.time - rel) < 1e-9) {
                    m_status->setText(tr("Já existe um keyframe nesse tempo."));
                    return;
                }
            const double v = kfValue(K, baseFor(c, p), rel);
            emit editStart();
            m_canvas->addKeyframe(rel, v);
            syncRowData();
            m_canvas->commitChange();
        });
    }
    syncRowData();
    if (GraphPropRow* r = m_rows.value((int)m_prop)) r->setExpanded(true);
}

void GraphEditorWidget::toggleAnimation(GraphProp p) {
    Clip* c = activeClip();
    if (!c) return;
    QVector<Keyframe>& K = *keysFor(c, p);
    emit editStart();
    if (K.isEmpty()) {
        const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        Keyframe nk;
        nk.time = rel;
        nk.value = baseFor(c, p);
        nk.interp = KfSmooth;
        K.append(nk);
    } else {
        K.clear();
        m_canvas->clearSelection();
    }
    m_canvas->fitValueRange();
    syncRowData();
    m_canvas->commitChange();
}

void GraphEditorWidget::toggleKeyAtPlayhead(GraphProp p) {
    Clip* c = activeClip();
    if (!c) return;
    QVector<Keyframe>& K = *keysFor(c, p);
    const double rel0 = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    const double fr = 1.0 / std::max(1.0, m_project ? m_project->fps : 30.0);
    const double rel = std::round(rel0 / fr) * fr;
    int at = -1;
    for (int i = 0; i < K.size(); ++i)
        if (std::fabs(K[i].time - rel) < 1e-9) { at = i; break; }
    emit editStart();
    if (at >= 0) {
        K.removeAt(at);
        m_canvas->clearSelection();
    } else {
        const double v = kfValue(K, baseFor(c, p), rel);
        m_canvas->addKeyframe(rel, v);
    }
    m_canvas->fitValueRange();
    syncRowData();
    m_canvas->commitChange();
}

void GraphEditorWidget::jumpKeyframe(GraphProp p, int dir) {
    Clip* c = activeClip();
    if (!c) return;
    const QVector<Keyframe>* K = keysFor(c, p);
    if (!K || K->isEmpty()) return;
    const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    int best = -1;
    if (dir < 0) {
        for (int i = 0; i < K->size(); ++i)
            if ((*K)[i].time < rel - 1e-9) best = i;
        if (best < 0) best = K->size() - 1;
    } else {
        for (int i = K->size() - 1; i >= 0; --i)
            if ((*K)[i].time > rel + 1e-9) best = i;
        if (best < 0) best = 0;
    }
    emit keyframeJump(c->pos + (*K)[best].time);
}
