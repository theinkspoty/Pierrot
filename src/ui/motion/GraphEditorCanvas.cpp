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
// GraphCanvas
// --------------------------------------------------------------------------

GraphCanvas::GraphCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(84);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);

    // Coalesce o `modified()` durante arrastos: o pipeline ligado a esse sinal
    // (re-render do preview, sync do pantalone, timeline) é pesado; disparar por
    // movimento de mouse travava o arraste horizontal do keyframe. Aqui ele é
    // agrupado em ~30Hz, mantendo a repintura local imediata.
    m_modifiedTimer = new QTimer(this);
    m_modifiedTimer->setSingleShot(true);
    m_modifiedTimer->setInterval(33);
    connect(m_modifiedTimer, &QTimer::timeout, this, [this]() {
        if (m_modifiedPending) {
            m_modifiedPending = false;
            emit modified();
        }
    });
}

void GraphCanvas::setData(Clip* clip, GraphProp prop, double playhead, double fps) {
    cancelModified();
    m_clip = clip;
    m_mesaTrack = nullptr;
    m_mesaMode = false;
    m_prop = prop;
    m_playhead = playhead;
    m_fps = (fps > 1.0) ? fps : 30.0;
    if (hasData() && keys()) sortKeys();
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

void GraphCanvas::setMesaData(Track* track, GraphProp prop, double playhead, double fps) {
    cancelModified();
    m_clip = nullptr;
    m_mesaTrack = track;
    m_mesaCamera = nullptr;
    m_mesaMode = (track != nullptr);
    m_camMode = false;
    m_prop = prop;
    m_playhead = playhead;
    m_fps = (fps > 1.0) ? fps : 30.0;
    if (m_mesaTrack && keys()) sortKeys();
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

void GraphCanvas::setMaskData(Clip* clip, int maskIndex, GraphProp prop, double playhead, double fps) {
    cancelModified();
    m_clip = clip;
    m_mesaTrack = nullptr;
    m_mesaCamera = nullptr;
    m_mesaMode = false;
    m_camMode = false;
    m_maskIndex = maskIndex;
    m_maskMode = (maskIndex >= 0);
    m_prop = prop;
    m_playhead = playhead;
    m_fps = (fps > 1.0) ? fps : 30.0;
    if (m_maskMode && keys()) sortKeys();
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

void GraphCanvas::setCamData(MesaComposition* mc, GraphProp prop, double playhead, double fps) {
    cancelModified();
    m_clip = nullptr;
    m_mesaTrack = nullptr;
    m_mesaCamera = mc;
    m_mesaMode = false;
    m_camMode = (mc != nullptr);
    m_prop = prop;
    m_playhead = playhead;
    m_fps = (fps > 1.0) ? fps : 30.0;
    if (m_mesaCamera && keys()) sortKeys();
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
    if (m_camMode && m_mesaCamera)
        return keysForCamera(m_mesaCamera, m_prop);
    if (m_mesaMode && m_mesaTrack)
        return keysForTrack(m_mesaTrack, m_prop);
    if (m_maskMode)
        return keysForMask(maskAt(m_clip, m_maskIndex), m_prop);
    return keysFor(m_clip, m_prop);
}

double GraphCanvas::baseValue() const {
    if (m_camMode && m_mesaCamera)
        return baseForCamera(m_mesaCamera, m_prop);
    if (m_mesaMode && m_mesaTrack)
        return baseForTrack(m_mesaTrack, m_prop);
    if (m_maskMode)
        return baseForMask(maskAt(m_clip, m_maskIndex), m_prop);
    return baseFor(m_clip, m_prop);
}

void GraphCanvas::valueRange(double* lo, double* hi) const {
    rangeFor(m_prop, lo, hi);
}

QRect GraphCanvas::plotRect() const {
    return QRect(kMarginL, kCanvasRulerH + kMarginT,
                 std::max(10, width() - kMarginL - kMarginR),
                 std::max(10, height() - kCanvasRulerH - kMarginT - kMarginB));
}

QRect GraphCanvas::rulerRect() const {
    return QRect(kMarginL, 0,
                 std::max(10, width() - kMarginL - kMarginR), kCanvasRulerH);
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
    if (m_mesaMode || m_camMode) {
        // Mesa tracks: use the time range from keyframes
        const QVector<Keyframe>* ks = keys();
        double maxT = 1.0;
        if (ks) for (const Keyframe& k : *ks) maxT = qMax(maxT, k.time);
        const double dur = maxT + 2.0;
        if (m_t1 > m_t0) return std::clamp(m_t1 - m_t0, 0.001, dur);
        return dur;
    }
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
    if (!hasData()) return -1;
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
    if (!hasData()) return;
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
    // Repinta na hora para o arraste ficar responsivo; o sinal pesado
    // `modified()` é coalescido pelo timer para não travar o movimento.
    update();
    if (!m_modifiedPending) {
        m_modifiedPending = true;
        m_modifiedTimer->start();
    }
}

void GraphCanvas::cancelModified() {
    if (m_modifiedTimer) m_modifiedTimer->stop();
    m_modifiedPending = false;
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
    QVector<Keyframe>& K = *keys();
    if (K.isEmpty() || m_selKeys.isEmpty() || m_selOrig.size() != m_selKeys.size()) return;
    const double dur = (m_mesaMode || m_camMode) ? (timeRange() + timeStart()) : std::max(0.05, m_clip ? m_clip->dur : 1.0);

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
    // Keyframes podem existir FORA da duração do clipe (ex.: gerados por script
    // ou importados) — nesse caso leftB pode superar rightB. Sem normalizar a
    // ordem, std::clamp recebia low > high e disparava o __glibcxx_assert_fail
    // do libstdc++ (abort). Garante sempre low <= high.
    double loT = std::min(leftB, rightB) + 0.001;
    double hiT = std::max(leftB, rightB) - 0.001;
    if (hiT < loT) {
        const double mid = 0.5 * (leftB + rightB);
        loT = hiT = mid;
    }

    double d = dT;
    if (minT + d < loT) d = loT - minT;
    if (maxT + d > hiT) d = hiT - maxT;

    const double vLo = std::min(m_lo, m_hi);
    const double vHi = std::max(m_lo, m_hi);
    for (int j = 0; j < m_selKeys.size(); ++j) {
        Keyframe& k = K[m_selKeys[j]];
        const Keyframe& o = m_selOrig[j];
        double nt = o.time + d;
        if (snap) nt = snapTime(nt);
        nt = std::clamp(nt, loT, hiT);
        k.time = nt;
        // Movimento vertical restaurado: o valor acompanha o arraste.
        // A sensibilidade (Configurações → Editor de curvas) amplifica o
        // deslocamento vertical para permitir curvas mais exageradas.
        k.value = std::clamp(o.value + dV, vLo, vHi);
    }
}

void GraphCanvas::updateHover(const QPoint& p) {
    const int hk = (hasData() && keys()) ? keyframeHit(p) : -1;
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
    p.fillRect(rect(), themeColors().graphBg);

    const double t0 = timeStart();
    const double range = timeRange();

    // Régua de tempo no topo.
    const QRect rr = rulerRect();
    p.fillRect(rr, themeColors().graphRuler);
    p.setPen(QPen(themeColors().graphAxis, 1));
    p.drawLine(rr.left(), rr.bottom(), rr.right(), rr.bottom());
    const double tStep = niceStep(range / 6.0);
    p.setPen(themeColors().graphRulerText);
    for (double t = std::ceil(t0 / tStep) * tStep; t <= t0 + range + 1e-9; t += tStep) {
        const int x = tToX(t);
        p.drawLine(x, rr.bottom() - 3, x, rr.bottom());
        p.drawText(x + 2, rr.bottom() - 3, fmtRuler(t, range));
    }
    if (hasData()) {
        const double rel = (m_mesaMode || m_camMode) ? std::max(0.0, m_playhead)
                                      : std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur);
        if (rel >= t0 - 1e-9 && rel <= t0 + range + 1e-9) {
            const int x = tToX(rel);
            QFont f = p.font();
            f.setBold(true);
            p.setFont(f);
            p.setPen(themeColors().graphRulerText);
            p.drawText(x + 7, rr.bottom() - 3,
                       tr("%1 · f%2").arg(fmtRuler(rel, range))
                                      .arg((int)std::lround(rel * m_fps)));
        }
    }

    const QRect r = plotRect();
    if (r.width() <= 0 || r.height() <= 0) return;

    // Fundo quadriculado de referência (grade sutil de tempo x valor), sem
    // preenchimento azul — só as linhas dão a noção de posição.
    p.setPen(themeColors().graphGrid);
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
        QColor dv = themeColors().graphLine;
        dv.setAlpha(190);
        p.setPen(QPen(dv, 1.6));
        p.drawLine(r.left(), y, r.right(), y);
        dv.setAlpha(90);
        p.setPen(dv);
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
                    // Mesmo ajuste do kfValue (Project.h): controle em span/3
                    // e secante/2 em par isolado — desenho = animação real.
                    const double iso = ((i == 0) && (i + 2 >= ks->size()))
                                           ? 0.5 : 1.0;
                    path.cubicTo(tToX(a.time + span / 3.0),
                                 vToY(a.value + m0 * span * iso / 3.0),
                                 tToX(b.time - span / 3.0),
                                 vToY(b.value - m3 * span * iso / 3.0),
                                 tToX(b.time), vToY(b.value));
                    break;
                }
                case KfBezier: {
                    path.cubicTo(tToX(a.time + a.ox), vToY(a.value + a.oy),
                                 tToX(b.time - b.ix), vToY(b.value + b.iy),
                                 tToX(b.time), vToY(b.value));
                    break;
                }
                case KfElastic:
                case KfBounce:
                case KfBack: {
                    // Easings amostrados da curva real (kfValue) — o mesmo
                    // cálculo do preview/export, senão o gráfico mostrava
                    // linha reta e escondia o overshoot.
                    constexpr int N = 16;
                    for (int s = 1; s <= N; ++s) {
                        const double t = a.time + span * s / N;
                        addTo(t, kfValue(*ks, a.value, t));
                    }
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
        p.setPen(QPen(themeColors().graphLine, 1.6));
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
                    p.setPen(QPen(themeColors().graphHandle, 1.5, Qt::DashLine));
                    p.drawLine(kp, hp);
                    p.setPen(QPen(QColor(255, 255, 255), 1.5));
                    p.setBrush(themeColors().graphHandle);
                    p.drawEllipse(hp, 4.5, 4.5);
                }
            }
        }
    }

    // Keyframes com glifos por interpolação.
    const double relPh = (m_mesaMode || m_camMode) ? std::max(0.0, m_playhead)
                                    : (m_clip ? std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur) : -1.0);
    for (int i = 0; i < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        const QPointF kp(tToX(k.time), vToY(k.value));
        const bool sel = m_selKeys.contains(i);
        const bool active = relPh >= 0.0 && std::fabs(k.time - relPh) < 1e-6;
        const QColor fill = sel ? QColor(120, 230, 150)
                                : active ? QColor(255, 179, 64)
                                         : themeColors().graphKeyframe;
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
            QColor mf = themeColors().graphHandle;
            mf.setAlpha(40);
            p.fillRect(mr, mf);
            mf.setAlpha(200);
            p.setPen(QPen(mf, 1, Qt::DashLine));
            p.drawRect(mr);
        }
    }

    // Linha do playhead no gráfico (varinha), sem faixa de preenchimento.
    if (hasData()) {
        const double rel = (m_mesaMode || m_camMode) ? std::max(0.0, m_playhead)
                                      : std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur);
        if (rel >= timeStart() - 1e-9 && rel <= timeStart() + timeRange() + 1e-9) {
            const int x = tToX(rel);
            QColor ph = themeColors().graphHandle;
            ph.setAlpha(230);
            p.setPen(QPen(ph, 2));
            p.drawLine(x, rr.bottom() + 1, x, r.bottom());
        }
    }
}

void GraphCanvas::mousePressEvent(QMouseEvent* e) {
    setFocus(); // garante foco para o Delete apagar keyframes, não o clipe
    if (e->button() != Qt::LeftButton || !hasData()) {
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
        const double t = (m_mesaMode || m_camMode) ? snapTime(xToT(e->pos().x()))
                                    : m_clip->pos + snapTime(xToT(e->pos().x()));
        emit keyframeJump(t);
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
            // Pegar a alça de um keyframe suave o converte em bezier manual,
            // preservando a curva (handles = tangente Catmull-Rom corrigida)
            // e deixando o usuário entortar/fazer ondas. Só keys suaves têm
            // alça — Linear/Degrau nem entram aqui (handleHit não as pega).
            double ht0 = 0.0, hv0 = 0.0, ht1 = 0.0, hv1 = 0.0;
            if (bezierHandle(K, hh, true, &ht0, &hv0)) {
                K[hh].ox = ht0; K[hh].oy = hv0;
            }
            if (bezierHandle(K, hh, false, &ht1, &hv1)) {
                K[hh].ix = ht1; K[hh].iy = hv1;
            }
            K[hh].interp = KfBezier;
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
    if (m_tool == CanvasTool::Select && hasData()) {
        const double rel = (m_mesaMode || m_camMode) ? std::max(0.0, m_playhead)
                                      : std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur);
        if (std::abs(e->pos().x() - tToX(rel)) <= 5) {
            m_playheadDrag = true;
            m_dragKey = -1;
            m_dragHandle = -1;
            m_marqueeActive = false;
            setCursor(Qt::SizeHorCursor);
            const double t = (m_mesaMode || m_camMode) ? m_playhead : m_clip->pos + snapTime(xToT(e->pos().x()));
            emit keyframeJump(t);
            e->accept();
            update();
            return;
        }
    }
    // Ferramentas de adicionar/curva: clicar em espaço vazio cria um keyframe.
    if (m_tool != CanvasTool::Select) {
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
        nk.interp = (m_tool == CanvasTool::Curve) ? KfSmooth : KfLinear;
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
        m_dragHandle = (m_tool == CanvasTool::Curve) ? idx : -1;
        m_curveNewKey = (m_tool == CanvasTool::Curve);
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
    if (m_playheadDrag && (e->buttons() & Qt::LeftButton)) {
        const double t = (m_mesaMode || m_camMode) ? snapTime(xToT(e->pos().x()))
                                     : (m_clip ? m_clip->pos + snapTime(xToT(e->pos().x())) : 0.0);
        emit keyframeJump(t);
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
    if (m_dragKey < 0 || !hasData() || !(e->buttons() & Qt::LeftButton)) {
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
    if (!hasData() || m_tool != CanvasTool::Select) {
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
        tSpin->setRange(0.0, (m_mesaMode || m_camMode) ? timeRange() : m_clip->dur);
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
    if (!hasData() || !(e->modifiers() & Qt::ControlModifier)) {
        QWidget::wheelEvent(e);
        return;
    }
    // Ctrl+roda: zoom horizontal (tempo) em torno do cursor.
    const double dur = (m_mesaMode || m_camMode) ? timeRange() : std::max(0.05, m_clip->dur);
    const double factor = (e->angleDelta().y() > 0) ? 0.75 : 1.0 / 0.75;
    const double range = timeRange();
    const double tc = xToT(e->position().x());
    const double minR = std::min(0.02, 2.0 / m_fps);
    // Em mesa/câmera `dur` pode ser menor que minR (range mínimo de 0.001s),
    // o que invertia o clamp e abortava; mantém sempre low <= high.
    const double loR = std::min(minR, dur);
    const double nr = std::clamp(range * factor, loR, dur);
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
    if (e->key() == Qt::Key_V) { setTool(CanvasTool::Select); e->accept(); return; }
    if (e->key() == Qt::Key_P) { setTool(CanvasTool::Add); e->accept(); return; }
    if (e->key() == Qt::Key_B) { setTool(CanvasTool::Curve); e->accept(); return; }
    // F9 = Easy Ease (varinha) nos keyframes selecionados.
    if (e->key() == Qt::Key_F9 && ks && !m_selKeys.isEmpty()) {
        applyEasyEase(e->modifiers() & Qt::ShiftModifier ? 2
                      : (e->modifiers() & Qt::ControlModifier ? 1 : 0));
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

    // 1/2/3/4/5/6/7 definem a interpolação dos keyframes selecionados.
    if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_7 && !ctrl) {
        const int modes[7] = {KfLinear, KfSmooth, KfStep, KfBezier,
                              KfElastic, KfBounce, KfBack};
        const int mode = modes[e->key() - Qt::Key_1];
        emit editStart();
        for (int i : m_selKeys) {
            (*ks)[i].interp = mode;
            if (mode == KfSmooth || mode == KfElastic || mode == KfBounce
                || mode == KfBack) {
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

// Easy Ease: handles horizontais (oy=iy=0) com extensão ~1/3 do span até o
// vizinho — o mesmo que o F9 do Premiere/After Effects. Suaviza a entrada
// e/ou a saída do keyframe sem inventar overshoot.
void GraphCanvas::applyEasyEase(int mode) {
    QVector<Keyframe>* ks = keys();
    if (!ks || m_selKeys.isEmpty()) return;
    emit editStart();
    for (int i : m_selKeys) {
        if (i < 0 || i >= ks->size()) continue;
        Keyframe& k = (*ks)[i];
        k.interp = KfBezier;
        // Span até o vizinho mais próximo (para dimensionar o handle).
        double spanL = 0.25, spanR = 0.25;
        if (i > 0) spanL = std::max(0.02, k.time - (*ks)[i - 1].time);
        if (i + 1 < ks->size()) spanR = std::max(0.02, (*ks)[i + 1].time - k.time);
        const double hL = spanL / 3.0;
        const double hR = spanR / 3.0;
        // mode 0 = ambos; 1 = easy in (só entrada); 2 = easy out (só saída).
        if (mode == 0 || mode == 2) { k.ox = hR; k.oy = 0.0; }
        if (mode == 0 || mode == 1) { k.ix = hL; k.iy = 0.0; }
        if (mode == 2) { k.ix = 0.0; k.iy = 0.0; } // easy out: sem suavizar entrada
        if (mode == 1) { k.ox = 0.0; k.oy = 0.0; } // easy in: sem suavizar saída
    }
    commitChange();
    emit statusMessage(mode == 1 ? tr("Easy Ease In aplicado")
                       : mode == 2 ? tr("Easy Ease Out aplicado")
                                   : tr("Easy Ease aplicado (F9)"));
}

void GraphCanvas::setSnap(bool on) {
    if (m_snap == on) return;
    m_snap = on;
    emit snapChanged(m_snap);
    update();
}

void GraphCanvas::contextMenuEvent(QContextMenuEvent* e) {
    if (!hasData()) { QWidget::contextMenuEvent(e); return; }
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
    QAction* ease = menu.addAction(tr("Easy Ease (varinha)"));
    QAction* easeIn = menu.addAction(tr("Easy Ease In"));
    QAction* easeOut = menu.addAction(tr("Easy Ease Out"));
    QAction* elastic = menu.addAction(tr("Elastic"));
    QAction* bounce = menu.addAction(tr("Bounce"));
    QAction* back = menu.addAction(tr("Back (overshoot)"));
    menu.addSeparator();
    QAction* del = menu.addAction(m_selKeys.size() > 1
                                      ? tr("Excluir %1 keyframes").arg(m_selKeys.size())
                                      : tr("Excluir keyframe"));
    QAction* act = menu.exec(e->globalPos());
    if (!act) return;
    if (act == ease || act == easeIn || act == easeOut) {
        // applyEasyEase faz o próprio editStart/commitChange (um undo só).
        applyEasyEase(act == easeIn ? 1 : act == easeOut ? 2 : 0);
        return;
    }
    emit editStart();
    m_undoPushed = true;
    if (act == lin || act == bez || act == autoBez || act == hold
        || act == elastic || act == bounce || act == back) {
        for (int i : m_selKeys) {
            K[i].interp = (act == lin) ? KfLinear
                          : (act == bez) ? KfBezier
                          : (act == autoBez) ? KfSmooth
                          : (act == hold) ? KfStep
                          : (act == elastic) ? KfElastic
                          : (act == bounce) ? KfBounce : KfBack;
            if (act == autoBez || act == elastic || act == bounce || act == back) {
                K[i].ox = K[i].oy = 0.0;
                K[i].ix = K[i].iy = 0.0;
            }
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

