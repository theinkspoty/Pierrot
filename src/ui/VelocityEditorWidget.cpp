// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "VelocityEditorWidget.h"

#include "ui/Theme.h"

#include <QDoubleSpinBox>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

// ── VelocityCanvas ───────────────────────────────────────────────────────

VelocityCanvas::VelocityCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

QPoint VelocityCanvas::timeToPos(double t) const {
    const int padL = 40, padR = 12, padT = 12, padB = 24;
    const double dur = (m_clip && m_clip->dur > 0) ? m_clip->dur : 1.0;
    const double x0 = padL, x1 = width() - padR;
    const double y0 = padT, y1 = height() - padB;
    const double tx = x0 + (std::clamp(t, 0.0, dur) / dur) * (x1 - x0);
    const double span = std::max(0.05, m_valHi - m_valLo);
    const double v = m_clip ? clipSpeedAt(*m_clip, std::clamp(t, 0.0, dur)) : 1.0;
    const double ty = y1 - ((std::clamp((v - m_valLo) / span, 0.0, 1.0)) * (y1 - y0));
    return QPoint(int(tx), int(ty));
}

QPoint VelocityCanvas::keyToPos(const Keyframe& k) const {
    const int padL = 40, padR = 12, padT = 12, padB = 24;
    const double dur = (m_clip && m_clip->dur > 0) ? m_clip->dur : 1.0;
    const double x0 = padL, x1 = width() - padR;
    const double y0 = padT, y1 = height() - padB;
    const double tx = x0 + (std::clamp(k.time, 0.0, dur) / dur) * (x1 - x0);
    const double span = std::max(0.05, m_valHi - m_valLo);
    const double ty = y1 - ((std::clamp((k.value - m_valLo) / span, 0.0, 1.0)) * (y1 - y0));
    return QPoint(int(tx), int(ty));
}

double VelocityCanvas::posToTime(const QPoint& p) const {
    const int padL = 40, padR = 12;
    const double dur = (m_clip && m_clip->dur > 0) ? m_clip->dur : 1.0;
    const double x0 = padL, x1 = width() - padR;
    const double f = (p.x() - x0) / std::max(1.0, x1 - x0);
    return std::clamp(f, 0.0, 1.0) * dur;
}

double VelocityCanvas::posToValue(const QPoint& p) const {
    const int padT = 12, padB = 24;
    const double y0 = padT, y1 = height() - padB;
    const double f = (y1 - p.y()) / std::max(1.0, y1 - y0);
    return m_valLo + std::clamp(f, 0.0, 1.0) * (m_valHi - m_valLo);
}

int VelocityCanvas::hitKey(const QPoint& p) const {
    if (!m_clip) return -1;
    for (int i = 0; i < m_clip->kfSpeed.size(); ++i) {
        const QPoint kp = keyToPos(m_clip->kfSpeed[i]);
        if (QPoint(kp - p).manhattanLength() <= 8) return i;
    }
    return -1;
}

// Distância vertical (px) do ponto até a curva de velocidade em `t`.
double VelocityCanvas::distanceToCurve(const QPoint& p) const {
    if (!m_clip) return 1e9;
    const double t = posToTime(p);
    const QPoint onCurve = timeToPos(t); // usa clipSpeedAt
    return std::abs(p.y() - onCurve.y());
}

int VelocityCanvas::hitHandle(const QPoint& p, int* idx, int* side) const {
    if (!m_clip) return -1;
    const double dur = (m_clip->dur > 0) ? m_clip->dur : 1.0;
    const int padL = 40, padR = 12, padT = 12, padB = 24;
    const double x0 = padL, x1 = width() - padR;
    const double y0 = padT, y1 = height() - padB;
    const double span = std::max(0.05, m_valHi - m_valLo);
    auto toPos = [&](double t, double v) {
        const double tx = x0 + (std::clamp(t, 0.0, dur) / dur) * (x1 - x0);
        const double ty = y1 - ((std::clamp((v - m_valLo) / span, 0.0, 1.0)) * (y1 - y0));
        return QPoint(int(tx), int(ty));
    };
    for (int i = 0; i < m_clip->kfSpeed.size(); ++i) {
        const Keyframe& k = m_clip->kfSpeed[i];
        // Alças reais ou estimadas (varinha fantasma quando ox/iy ainda são 0).
        double ox = k.ox, oy = k.oy, ix = k.ix, iy = k.iy;
        if (std::fabs(ox) < 1e-6 && std::fabs(oy) < 1e-6
            && std::fabs(ix) < 1e-6 && std::fabs(iy) < 1e-6) {
            double spanL = 0.25, spanR = 0.25;
            if (i > 0) spanL = std::max(0.04, k.time - m_clip->kfSpeed[i - 1].time);
            if (i + 1 < m_clip->kfSpeed.size())
                spanR = std::max(0.04, m_clip->kfSpeed[i + 1].time - k.time);
            ox = spanR / 3.0; ix = spanL / 3.0;
        }
        const QPoint hOut = toPos(k.time + ox, k.value + oy);
        const QPoint hIn = toPos(k.time - ix, k.value - iy);
        if (QPoint(hOut - p).manhattanLength() <= 7) {
            if (idx) *idx = i;
            if (side) *side = 0;
            return i;
        }
        if (QPoint(hIn - p).manhattanLength() <= 7) {
            if (idx) *idx = i;
            if (side) *side = 1;
            return i;
        }
    }
    return -1;
}

void VelocityCanvas::setClip(Clip* clip, double playhead, double fps) {
    setClipId(clip ? nullptr : nullptr, clip ? clip->id : QString(), playhead);
    // project precisa ser setado antes — via setClipId(Project*, id, t).
    Q_UNUSED(fps);
}

void VelocityCanvas::setClipId(Project* project, const QString& clipId, double playhead) {
    m_project = project;
    m_clipId = clipId;
    m_playhead = playhead;
    m_clip = resolveClip();
    if (m_clip) {
        double lo = m_clip->speed, hi = m_clip->speed;
        for (const Keyframe& k : m_clip->kfSpeed) {
            lo = std::min(lo, k.value);
            hi = std::max(hi, k.value);
        }
        lo = std::min(lo, 0.0);
        hi = std::max(hi, 1.0);
        const double pad = std::max(0.25, (hi - lo) * 0.2);
        m_valLo = lo - pad;
        m_valHi = hi + pad;
    }
    update();
}

void VelocityCanvas::setPlayhead(double t) {
    m_playhead = t;
    update();
}

void VelocityCanvas::commitChange() {
    emit changed();
    update();
}

void VelocityCanvas::ensureDefaults() {
    if (!m_clip) return;
    if (m_clip->kfSpeed.isEmpty()) {
        // Envelope a partir da velocidade base: 2 pontos bezier horizontais
        // (varinha já visível, pronta para entortar).
        Keyframe a;
        a.time = 0.0;
        a.value = m_clip->speed;
        a.interp = KfBezier;
        const double span = std::max(0.05, m_clip->dur);
        a.ox = span / 3.0; a.oy = 0.0; a.ix = 0.0; a.iy = 0.0;
        Keyframe b;
        b.time = std::max(0.05, m_clip->dur);
        b.value = m_clip->speed;
        b.interp = KfLinear;
        b.ox = 0.0; b.oy = 0.0; b.ix = span / 3.0; b.iy = 0.0;
        m_clip->kfSpeed.append(a);
        m_clip->kfSpeed.append(b);
    } else {
        // Garante alças visíveis nos kfs lineares/step (varinha fantasma já
        // pinta; aqui materializa handles para o hit-test ser estável).
        for (int i = 0; i < m_clip->kfSpeed.size(); ++i) {
            Keyframe& k = m_clip->kfSpeed[i];
            if (std::fabs(k.ox) + std::fabs(k.oy) + std::fabs(k.ix) + std::fabs(k.iy) > 1e-6)
                continue;
            double spanL = 0.25, spanR = 0.25;
            if (i > 0) spanL = std::max(0.04, k.time - m_clip->kfSpeed[i - 1].time);
            if (i + 1 < m_clip->kfSpeed.size())
                spanR = std::max(0.04, m_clip->kfSpeed[i + 1].time - k.time);
            const double minT = (m_clip->dur > 0 ? m_clip->dur : 1.0) * 0.08;
            spanL = std::max(spanL, minT);
            spanR = std::max(spanR, minT);
            if (k.interp == KfLinear || k.interp == KfStep || k.interp == KfSmooth) {
                k.interp = KfBezier;
                k.ox = spanR / 3.0; k.oy = 0.0;
                k.ix = spanL / 3.0; k.iy = 0.0;
            }
        }
    }
}

// Curva real amostrada via clipSpeedAt (respeita kfSpeed bezier/linear/step).
void VelocityCanvas::drawCurve(QPainter& p, const QRect& plot) {
    if (!m_clip) return;
    const auto& tc = themeColors();
    const int steps = std::max(8, plot.width());
    QPainterPath path;
    for (int i = 0; i <= steps; ++i) {
        const double t = (double)i / steps * m_clip->dur;
        const QPoint pt = timeToPos(t);
        if (i == 0) path.moveTo(pt);
        else path.lineTo(pt);
    }
    QPainterPath fill = path;
    fill.lineTo(timeToPos(m_clip->dur).x(), plot.bottom());
    fill.lineTo(timeToPos(0.0).x(), plot.bottom());
    fill.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(38, 128, 235, 40));
    p.drawPath(fill);
    p.setPen(QPen(QColor(38, 128, 235, 220), 2));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    Q_UNUSED(tc);
}

Clip* VelocityCanvas::resolveClip() const {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    return nullptr;
}

void VelocityCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& tc = themeColors();
    p.fillRect(rect(), tc.base);

    m_clip = resolveClip();
    const int padL = 40, padR = 12, padT = 12, padB = 24;
    const QRect plot(padL, padT, width() - padL - padR, height() - padT - padB);
    p.setPen(tc.trackBorder);
    p.drawRect(plot);

    if (!m_clip) {
        p.setPen(tc.monitorLabel);
        p.drawText(rect(), Qt::AlignCenter, tr("Selecione um clipe de vídeo"));
        return;
    }

    // Grade horizontal em valores inteiros de velocidade.
    p.setFont(QFont(p.font().family(), 8));
    for (double v = std::ceil(m_valLo); v <= m_valHi + 1e-6; v += 1.0) {
        const double span = std::max(0.05, m_valHi - m_valLo);
        const int yy = plot.bottom() - int(((v - m_valLo) / span) * plot.height());
        p.setPen(QPen(tc.timelineGrid, 1));
        p.drawLine(plot.left(), yy, plot.right(), yy);
        p.setPen(tc.monitorLabel);
        p.drawText(QRect(0, yy - 8, padL - 4, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QString("%1×").arg(v, 0, 'f', 1));
    }

    // Linha de 1× (referência).
    {
        const double span = std::max(0.05, m_valHi - m_valLo);
        const int y1 = plot.bottom() - int(((1.0 - m_valLo) / span) * plot.height());
        p.setPen(QPen(tc.playhead, 1, Qt::DashLine));
        p.drawLine(plot.left(), y1, plot.right(), y1);
    }

    // Playhead do programa.
    const double rel = m_playhead - m_clip->pos;
    if (rel >= 0 && rel <= m_clip->dur) {
        const QPoint ph = timeToPos(rel);
        p.setPen(QPen(tc.playheadHandle, 2));
        p.drawLine(ph.x(), plot.top(), ph.x(), plot.bottom());
    }

    // Curva bezier amostrada (entorta de verdade com kfSpeed).
    drawCurve(p, plot);

    // Keyframes + varinha de alças bezier SEMPRE saindo do losango
// (estimada se o kf ainda não foi entortado — estilo After Effects).
    for (int i = 0; i < m_clip->kfSpeed.size(); ++i) {
        const Keyframe& k = m_clip->kfSpeed[i];
        const QPoint kp = keyToPos(k);
        const double dur = (m_clip->dur > 0) ? m_clip->dur : 1.0;
        const int padL2 = 40, padR2 = 12, padT2 = 12, padB2 = 24;
        const double x0 = padL2, x1 = width() - padR2;
        const double y0 = padT2, y1 = height() - padB2;
        const double span = std::max(0.05, m_valHi - m_valLo);
        auto toPos = [&](double t, double v) {
            const double tx = x0 + (std::clamp(t, 0.0, dur) / dur) * (x1 - x0);
            const double ty = y1 - ((std::clamp((v - m_valLo) / span, 0.0, 1.0)) * (y1 - y0));
            return QPoint(int(tx), int(ty));
        };
        // Handles reais ou estimados (varinha fantasma).
        double ox = k.ox, oy = k.oy, ix = k.ix, iy = k.iy;
        const bool realHandles = std::fabs(ox) + std::fabs(oy) + std::fabs(ix) + std::fabs(iy) > 1e-6;
        if (!realHandles) {
            double spanL = 0.25, spanR = 0.25;
            if (i > 0) spanL = std::max(0.06, k.time - m_clip->kfSpeed[i - 1].time);
            if (i + 1 < m_clip->kfSpeed.size())
                spanR = std::max(0.06, m_clip->kfSpeed[i + 1].time - k.time);
            // Mínimo visível em pixels: ~12% da largura do plot.
            const double minT = dur * 0.08;
            spanL = std::max(spanL, minT);
            spanR = std::max(spanR, minT);
            ox = spanR / 3.0; oy = 0.0;
            ix = spanL / 3.0; iy = 0.0;
        }
        const QPoint hOut = toPos(k.time + ox, k.value + oy);
        const QPoint hIn = toPos(k.time - ix, k.value - iy);
        // Linha da varinha (mais grossa/visível quando selecionado).
        const bool sel = (i == m_dragIdx);
        p.setPen(QPen(sel ? QColor(38, 128, 235) : QColor(220, 220, 220, 190),
                      sel ? 2 : 1.5));
        p.drawLine(kp, hOut);
        p.drawLine(kp, hIn);
        // Bolinhas das alças.
        p.setBrush(sel ? QColor(38, 128, 235) : QColor(245, 245, 245, 230));
        p.setPen(QPen(QColor(30, 30, 30, 180), 1));
        p.drawEllipse(hOut, 4, 4);
        p.drawEllipse(hIn, 4, 4);

        // Losango do keyframe.
        p.setPen(QPen(tc.clipBorderSelect, 1));
        p.setBrush(sel ? tc.playhead : QColor(38, 128, 235));
        const int r = 5;
        QPolygon diamond;
        diamond << QPoint(kp.x(), kp.y() - r) << QPoint(kp.x() + r, kp.y())
                << QPoint(kp.x(), kp.y() + r) << QPoint(kp.x() - r, kp.y());
        p.drawPolygon(diamond);
        const QString pct = QString::number(int(std::lround(k.value * 100)))
                            + QStringLiteral("%");
        const QString tag = k.value > 1.05 ? tr("Rápido")
                            : k.value < 0.95 ? tr("Baixo") : QString();
        QString txt = pct;
        if (!tag.isEmpty()) txt += QStringLiteral(" · ") + tag;
        p.setPen(QColor(255, 255, 255, 210));
        p.setFont(QFont(p.font().family(), 8, QFont::Bold));
        p.drawText(QRect(kp.x() - 30, kp.y() - 18, 60, 14), Qt::AlignCenter, txt);
    }

    // Rótulo de velocidade no playhead.
    const double sp = clipSpeedAt(*m_clip, std::clamp(rel, 0.0, m_clip->dur));
    const QString tag = sp > 1.05 ? tr("Rápido") : sp < 0.95 ? tr("Baixo") : QString();
    p.setPen(tc.spinText);
    p.setFont(QFont(p.font().family(), 9, QFont::Bold));
    p.drawText(QRect(plot.left() + 4, plot.top() + 2, 140, 18), Qt::AlignLeft,
               tag.isEmpty() ? QString("%1×").arg(sp, 0, 'f', 2)
                             : QString("%1× · %2").arg(sp, 0, 'f', 2).arg(tag));

    // Dica da ferramenta ativa.
    if (m_tool == VelTool::Curve) {
        p.setPen(QColor(255, 255, 255, 140));
        p.setFont(QFont(p.font().family(), 8));
        p.drawText(QRect(plot.right() - 160, plot.top() + 2, 156, 14), Qt::AlignRight,
                   tr("Curva: clique cria · arraste alças entorta"));
    }
}

void VelocityCanvas::mousePressEvent(QMouseEvent* e) {
    m_clip = resolveClip();
    if (!m_clip) return;
    if (e->button() != Qt::LeftButton) return;

    int hIdx = -1, hSide = 0;
    const int hh = hitHandle(e->pos(), &hIdx, &hSide);
    if (hh >= 0 && m_tool != VelTool::Add) {
        // Arrastar alça = entortar (converte para bezier se ainda não era).
        emit editStart();
        Keyframe& k = m_clip->kfSpeed[hIdx];
        if (k.interp != KfBezier
            || (std::fabs(k.ox) + std::fabs(k.oy) + std::fabs(k.ix) + std::fabs(k.iy) < 1e-6)) {
            k.interp = KfBezier;
            double spanL = 0.25, spanR = 0.25;
            if (hIdx > 0) spanL = std::max(0.04, k.time - m_clip->kfSpeed[hIdx - 1].time);
            if (hIdx + 1 < m_clip->kfSpeed.size())
                spanR = std::max(0.04, m_clip->kfSpeed[hIdx + 1].time - k.time);
            const double minT = (m_clip->dur > 0 ? m_clip->dur : 1.0) * 0.08;
            spanL = std::max(spanL, minT);
            spanR = std::max(spanR, minT);
            k.ox = spanR / 3.0; k.oy = 0.0;
            k.ix = spanL / 3.0; k.iy = 0.0;
        }
        m_dragIdx = hIdx;
        m_dragHandle = hSide;
        emit statusMessage(hSide == 0 ? tr("Alça de saída — entorta para a direita")
                                      : tr("Alça de entrada — entorta para a esquerda"));
        update();
        return;
    }

    const int hit = hitKey(e->pos());
    if (hit >= 0 && m_tool == VelTool::Select) {
        m_dragIdx = hit;
        m_dragHandle = -1;
        m_dragOff = keyToPos(m_clip->kfSpeed[hit]) - e->pos();
        emit editStart();
    } else if (m_tool == VelTool::Curve || m_tool == VelTool::Add
               || (m_tool == VelTool::Select && distanceToCurve(e->pos()) <= 8.0)) {
        // Cria keyframe. Se o clique está perto da linha, cola o valor na
        // curva (clipSpeedAt) — clicar "em cima da linha" cria kf no ponto
        // exato da velocidade ali.
        emit editStart();
        ensureDefaults();
        const double t = posToTime(e->pos());
        const double vCurve = clipSpeedAt(*m_clip, std::clamp(t, 0.0, m_clip->dur));
        const double vMouse = std::max(0.01, posToValue(e->pos()));
        const double nearLine = distanceToCurve(e->pos()) <= 8.0;
        const double v = nearLine ? vCurve : vMouse;
        const int mode = (m_tool == VelTool::Curve || nearLine) ? KfSmooth : KfLinear;
        upsertKeyframe(m_clip->kfSpeed, t, v, mode);
        m_dragIdx = -1;
        for (int i = 0; i < m_clip->kfSpeed.size(); ++i)
            if (std::fabs(m_clip->kfSpeed[i].time - t) < 1e-6) { m_dragIdx = i; break; }
        m_dragHandle = -1;
        if (mode == KfSmooth && m_dragIdx >= 0) {
            Keyframe& nk = m_clip->kfSpeed[m_dragIdx];
            nk.interp = KfBezier;
            double spanL = 0.25, spanR = 0.25;
            if (m_dragIdx > 0)
                spanL = std::max(0.02, nk.time - m_clip->kfSpeed[m_dragIdx - 1].time);
            if (m_dragIdx + 1 < m_clip->kfSpeed.size())
                spanR = std::max(0.02, m_clip->kfSpeed[m_dragIdx + 1].time - nk.time);
            nk.ox = spanR / 3.0; nk.oy = 0.0;
            nk.ix = spanL / 3.0; nk.iy = 0.0;
            emit statusMessage(nearLine
                                   ? tr("Keyframe criado na curva — arraste as alças")
                                   : tr("Alças criadas — arraste para entortar a velocidade"));
        }
        commitChange();
    } else if (hit >= 0) {
        // Select sobre kf mesmo em modo Curve: move o ponto.
        m_dragIdx = hit;
        m_dragHandle = -1;
        m_dragOff = keyToPos(m_clip->kfSpeed[hit]) - e->pos();
        emit editStart();
    }
    update();
}

void VelocityCanvas::mouseMoveEvent(QMouseEvent* e) {
    m_clip = resolveClip();
    if (!m_clip || m_dragIdx < 0 || m_dragIdx >= m_clip->kfSpeed.size()) return;
    Keyframe& k = m_clip->kfSpeed[m_dragIdx];
    if (m_dragHandle >= 0) {
        // Entortar: alça em unidade de tempo/valor.
        const double dur = (m_clip->dur > 0) ? m_clip->dur : 1.0;
        const int padL = 40, padR = 12, padT = 12, padB = 24;
        const double x0 = padL, x1 = width() - padR;
        const double y0 = padT, y1 = height() - padB;
        const double span = std::max(0.05, m_valHi - m_valLo);
        const double fx = (e->pos().x() - x0) / std::max(1.0, x1 - x0);
        const double fy = (y1 - e->pos().y()) / std::max(1.0, y1 - y0);
        const double ht = std::clamp(fx, 0.0, 1.0) * dur - k.time;
        const double hv = m_valLo + std::clamp(fy, 0.0, 1.0) * span - k.value;
        if (m_dragHandle == 0) { // saída
            k.ox = std::max(0.0, ht);
            k.oy = hv;
        } else { // entrada
            k.ix = std::max(0.0, -ht);
            k.iy = -hv; // handle de entrada: (time-ix, value-iy)
        }
        k.interp = KfBezier;
        update();
        return;
    }
    // Arrastar o keyframe (posição/valor).
    double t = posToTime(e->pos() + m_dragOff);
    if (m_dragIdx > 0) t = std::max(t, m_clip->kfSpeed[m_dragIdx - 1].time + 0.02);
    if (m_dragIdx + 1 < m_clip->kfSpeed.size())
        t = std::min(t, m_clip->kfSpeed[m_dragIdx + 1].time - 0.02);
    t = std::clamp(t, 0.0, m_clip->dur);
    k.time = t;
    k.value = std::max(0.01, posToValue(e->pos()));
    update();
}

void VelocityCanvas::mouseReleaseEvent(QMouseEvent*) {
    if (m_dragIdx >= 0 || m_dragHandle >= 0) {
        m_dragIdx = -1;
        m_dragHandle = -1;
        commitChange();
    }
}

void VelocityCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_clip) return;
    const int hit = hitKey(e->pos());
    if (hit >= 0 && m_clip->kfSpeed.size() > 2) {
        emit editStart();
        m_clip->kfSpeed.removeAt(hit);
        m_dragIdx = -1;
        commitChange();
    } else if (hit >= 0) {
        // Duplo clique no kf: easy ease nele.
        easyEaseAt(m_clip->kfSpeed[hit].time, 0);
    }
}

void VelocityCanvas::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_F9) {
        const double rel = std::clamp(m_playhead - (m_clip ? m_clip->pos : 0.0),
                                      0.0, m_clip ? m_clip->dur : 0.0);
        easyEaseAt(rel, e->modifiers() & Qt::ShiftModifier ? 2
                     : (e->modifiers() & Qt::ControlModifier ? 1 : 0));
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_V) { m_tool = VelTool::Select; emit statusMessage(tr("Selecionar")); e->accept(); return; }
    if (e->key() == Qt::Key_P) { m_tool = VelTool::Add; emit statusMessage(tr("Adicionar")); e->accept(); return; }
    if (e->key() == Qt::Key_B) { m_tool = VelTool::Curve; emit statusMessage(tr("Curva (entortar)")); e->accept(); return; }
    QWidget::keyPressEvent(e);
}

void VelocityCanvas::easyEaseAt(double rel, int mode) {
    if (!m_clip || m_clip->kfSpeed.isEmpty()) return;
    int best = -1;
    double bestD = 1e9;
    for (int i = 0; i < m_clip->kfSpeed.size(); ++i) {
        const double d = std::fabs(m_clip->kfSpeed[i].time - rel);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) return;
    emit editStart();
    Keyframe& k = m_clip->kfSpeed[best];
    k.interp = KfBezier;
    double spanL = 0.25, spanR = 0.25;
    if (best > 0) spanL = std::max(0.02, k.time - m_clip->kfSpeed[best - 1].time);
    if (best + 1 < m_clip->kfSpeed.size())
        spanR = std::max(0.02, m_clip->kfSpeed[best + 1].time - k.time);
    if (mode == 0 || mode == 2) { k.ox = spanR / 3.0; k.oy = 0.0; }
    if (mode == 0 || mode == 1) { k.ix = spanL / 3.0; k.iy = 0.0; }
    if (mode == 2) { k.ix = 0.0; k.iy = 0.0; }
    if (mode == 1) { k.ox = 0.0; k.oy = 0.0; }
    commitChange();
    emit statusMessage(mode == 1 ? tr("Easy Ease In") : mode == 2 ? tr("Easy Ease Out")
                                                                  : tr("Easy Ease (F9)"));
}

// ── VelocityEditorWidget ─────────────────────────────────────────────────

VelocityEditorWidget::VelocityEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);

    m_title = new QLabel(tr("Velocidade do clipe"), this);
    m_title->setStyleSheet(QStringLiteral("font-weight:600;"));
    lay->addWidget(m_title);

    auto* tools = new QHBoxLayout;
    auto* baseLab = new QLabel(tr("Base"), this);
    m_baseSpin = new QDoubleSpinBox(this);
    m_baseSpin->setRange(0.1, 4.0);
    m_baseSpin->setDecimals(2);
    m_baseSpin->setSingleStep(0.05);
    m_baseSpin->setSuffix(QStringLiteral("×"));
    m_baseSpin->setToolTip(tr("Velocidade constante do clipe (sem envelope)"));

    auto* valLab = new QLabel(tr("Valor"), this);
    m_valSpin = new QDoubleSpinBox(this);
    m_valSpin->setRange(0.01, 8.0);
    m_valSpin->setDecimals(2);
    m_valSpin->setSingleStep(0.05);
    m_valSpin->setSuffix(QStringLiteral("×"));
    m_valSpin->setToolTip(tr("Velocidade no keyframe selecionado / no playhead"));

    m_addBtn = new QToolButton(this);
    m_addBtn->setText(tr("kf"));
    m_addBtn->setToolTip(tr("Adicionar keyframe no playhead"));
    m_delBtn = new QToolButton(this);
    m_delBtn->setText(tr("−kf"));
    m_delBtn->setToolTip(tr("Remover keyframe mais próximo do playhead"));
    m_resetBtn = new QToolButton(this);
    m_resetBtn->setText(tr("Constante"));
    m_resetBtn->setToolTip(tr("Remove o envelope e deixa só a velocidade base"));

    // Ferramentas de curva (varinha de entortar), como no Editor de Curvas.
    m_toolSel = new QToolButton(this);
    m_toolSel->setCheckable(true);
    m_toolSel->setChecked(true);
    m_toolSel->setText(tr("Sel"));
    m_toolSel->setToolTip(tr("Selecionar/mover keyframes (V)"));
    m_toolAdd = new QToolButton(this);
    m_toolAdd->setCheckable(true);
    m_toolAdd->setText(tr("+"));
    m_toolAdd->setToolTip(tr("Clique cria keyframe linear (P)"));
    m_toolCurve = new QToolButton(this);
    m_toolCurve->setCheckable(true);
    m_toolCurve->setText(tr("Curva"));
    m_toolCurve->setToolTip(tr("Varinha: clique cria ponto suave; arraste as "
                               "alças entorta a velocidade (B)"));
    auto* toolGroup = new QButtonGroup(this);
    toolGroup->setExclusive(true);
    toolGroup->addButton(m_toolSel, 0);
    toolGroup->addButton(m_toolAdd, 1);
    toolGroup->addButton(m_toolCurve, 2);
    connect(toolGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (!m_canvas) return;
        m_canvas->setTool(static_cast<VelTool>(id));
    });

    tools->addWidget(baseLab);
    tools->addWidget(m_baseSpin);
    tools->addSpacing(8);
    tools->addWidget(valLab);
    tools->addWidget(m_valSpin);
    tools->addStretch();
    tools->addWidget(m_toolSel);
    tools->addWidget(m_toolAdd);
    tools->addWidget(m_toolCurve);
    tools->addWidget(m_addBtn);
    tools->addWidget(m_delBtn);
    tools->addWidget(m_resetBtn);
    lay->addLayout(tools);

    m_canvas = new VelocityCanvas(this);
    lay->addWidget(m_canvas, 1);

    m_info = new QLabel(this);
    m_info->setStyleSheet(QStringLiteral("color:#888;"));
    m_info->setText(tr("Clique no gráfico para criar keyframes; arraste para mover; "
                       "duplo clique remove. Export usa a mesma curva (pré-render)."));
    lay->addWidget(m_info);

    connect(m_canvas, &VelocityCanvas::editStart, this, &VelocityEditorWidget::editStart);
    connect(m_canvas, &VelocityCanvas::changed, this, [this]() {
        refreshUi();
        emit modified();
    });
    connect(m_canvas, &VelocityCanvas::statusMessage, this, [this](const QString& s) {
        if (m_info) m_info->setText(s);
    });
    connect(m_baseSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) { applyBaseSpeed(v); });
    connect(m_valSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double v) {
        Clip* c = activeClip();
        if (!c || c->kfSpeed.isEmpty()) return;
        const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        emit editStart();
        upsertKeyframe(c->kfSpeed, rel, v, KfLinear);
        emit modified();
        refreshUi();
    });
    connect(m_addBtn, &QToolButton::clicked, this, [this]() {
        Clip* c = activeClip();
        if (!c) return;
        emit editStart();
        const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        const double v = c->kfSpeed.isEmpty() ? c->speed : clipSpeedAt(*c, rel);
        upsertKeyframe(c->kfSpeed, rel, v, KfLinear);
        emit modified();
        refreshUi();
    });
    connect(m_delBtn, &QToolButton::clicked, this, [this]() {
        Clip* c = activeClip();
        if (!c || c->kfSpeed.size() <= 2) return;
        const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        int best = -1;
        double bestD = 1e9;
        for (int i = 0; i < c->kfSpeed.size(); ++i) {
            const double d = std::fabs(c->kfSpeed[i].time - rel);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best < 0) return;
        emit editStart();
        c->kfSpeed.removeAt(best);
        emit modified();
        refreshUi();
    });
    connect(m_resetBtn, &QToolButton::clicked, this, [this]() {
        Clip* c = activeClip();
        if (!c) return;
        emit editStart();
        c->kfSpeed.clear();
        emit modified();
        refreshUi();
    });
    // Varinha Easy Ease no keyframe sob o playhead (F9 / botão).
    auto* easeBtn = new QToolButton(this);
    easeBtn->setText(tr("Varinha"));
    easeBtn->setToolTip(tr("Easy Ease no keyframe do playhead (F9) — suaviza a "
                           "transição de velocidade"));
    tools->insertWidget(4, easeBtn);
    connect(easeBtn, &QToolButton::clicked, this, [this]() {
        Clip* c = activeClip();
        if (!c || c->kfSpeed.isEmpty()) return;
        const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        int best = -1;
        double bestD = 1e9;
        for (int i = 0; i < c->kfSpeed.size(); ++i) {
            const double d = std::fabs(c->kfSpeed[i].time - rel);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best < 0) return;
        emit editStart();
        Keyframe& k = c->kfSpeed[best];
        k.interp = KfBezier;
        double spanL = 0.25, spanR = 0.25;
        if (best > 0) spanL = std::max(0.02, k.time - c->kfSpeed[best - 1].time);
        if (best + 1 < c->kfSpeed.size())
            spanR = std::max(0.02, c->kfSpeed[best + 1].time - k.time);
        k.ox = spanR / 3.0; k.oy = 0.0;
        k.ix = spanL / 3.0; k.iy = 0.0;
        emit modified();
        refreshUi();
    });
}

Clip* VelocityEditorWidget::activeClip() const {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    return nullptr;
}

void VelocityEditorWidget::setClipId(const QString& id) {
    m_clipId = id;
    refreshUi();
}

void VelocityEditorWidget::setPlayhead(double t) {
    m_playhead = t;
    if (m_canvas) m_canvas->setPlayhead(t);
    Clip* c = activeClip();
    if (c && c->kfSpeed.isEmpty()) {
        const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        m_valSpin->setValue(clipSpeedAt(*c, rel));
    }
}

void VelocityEditorWidget::applyBaseSpeed(double v) {
    Clip* c = activeClip();
    if (!c) return;
    emit editStart();
    c->speed = std::clamp(v, 0.1, 4.0);
    // Se não há envelope, os keyframes usam a base como valor default.
    emit modified();
    refreshUi();
}

void VelocityEditorWidget::refreshUi() {
    Clip* c = activeClip();
    const bool on = c != nullptr;
    m_baseSpin->setEnabled(on);
    m_valSpin->setEnabled(on && c && !c->kfSpeed.isEmpty());
    m_addBtn->setEnabled(on);
    m_delBtn->setEnabled(on && c && c->kfSpeed.size() > 2);
    m_resetBtn->setEnabled(on && c && !c->kfSpeed.isEmpty());
    if (!c) {
        m_title->setText(tr("Velocidade do clipe"));
        m_info->setText(tr("Selecione um clipe na timeline."));
        if (m_canvas) m_canvas->setClip(nullptr, m_playhead, 30.0);
        return;
    }
    m_title->setText(tr("Velocidade — %1").arg(c->name.isEmpty() ? c->id : c->name));
    m_baseSpin->blockSignals(true);
    m_baseSpin->setValue(c->speed);
    m_baseSpin->blockSignals(false);
    const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    m_valSpin->blockSignals(true);
    m_valSpin->setValue(clipSpeedAt(*c, rel));
    m_valSpin->blockSignals(false);
    m_info->setText(!c->kfSpeed.isEmpty()
                        ? tr("Envelope (%1 kfs). Clique na linha para criar keyframe; "
                             "arraste as alças para entortar.")
                              .arg(c->kfSpeed.size())
                        : tr("Velocidade constante %1×. Clique na linha (ou use Curva) "
                             "para criar o envelope.")
                              .arg(c->speed, 0, 'f', 2));
    if (m_canvas) m_canvas->setClip(c, m_playhead, 30.0);
    if (m_canvas) m_canvas->setClipId(m_project, c->id, m_playhead);
}
