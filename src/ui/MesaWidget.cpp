// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaWidget.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QtMath>

// ═══════════════════════════════════════════════════════════════════════
// KfRef — identificador de keyframe no mini-timeline
// ═══════════════════════════════════════════════════════════════════════

uint qHash(const MesaWidget::KfRef& r) {
    uint h = qHash(r.source);
    h ^= qHash(r.trackId) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= qHash(qRound(r.time * 1000.0)) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= qHash(r.prop) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

uint qHash(const MesaWidget::KfRef& r, size_t seed) {
    Q_UNUSED(seed);
    return qHash(r);
}

// ═══════════════════════════════════════════════════════════════════════
// Construtor
// ═══════════════════════════════════════════════════════════════════════

MesaWidget::MesaWidget(QWidget* parent)
    : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 150);
    setStyleSheet("background: #2D2D2D;");
}

// ═══════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════

MesaComposition* MesaWidget::currentMesa() const {
    if (!m_project || m_mesaId.isEmpty()) return nullptr;
    return m_project->findMesa(m_mesaId);
}

Track* MesaWidget::findTrack(const QString& trackId) const {
    if (!m_project) return nullptr;
    for (Track& tr : m_project->videoTracks)
        if (tr.id == trackId) return &tr;
    for (Track& tr : m_project->audioTracks)
        if (tr.id == trackId) return &tr;
    return nullptr;
}

QVector<Track*> MesaWidget::mesaTracks() const {
    MesaComposition* mc = currentMesa();
    const QString currentId = mc ? mc->id : QString();
    quint64 trackHash = 0;
    if (mc)
        for (const QString& tid : mc->trackIds)
            trackHash ^= qHash(tid) + 0x9e3779b9 + (trackHash << 6) + (trackHash >> 2);
    const qint64 ver = mc ? (reinterpret_cast<qintptr>(m_project) ^ mc->trackIds.size()
                             ^ static_cast<qintptr>(trackHash)) : 0;
    if (!m_cachedTracks.isEmpty() && m_cachedMesaId == currentId
        && m_cachedTracksVersion == ver) {
        return m_cachedTracks;
    }
    m_cachedTracks.clear();
    if (!mc || !m_project) return m_cachedTracks;
    for (const QString& tid : mc->trackIds) {
        Track* t = findTrack(tid);
        if (t) m_cachedTracks.append(t);
    }
    m_cachedMesaId = currentId;
    m_cachedTracksVersion = ver;
    return m_cachedTracks;
}

// ═══════════════════════════════════════════════════════════════════════
// Keyframes
// ═══════════════════════════════════════════════════════════════════════

double MesaWidget::mesaDuration() const {
    MesaComposition* mc = currentMesa();
    if (!mc) { m_contentStart = 0.0; return 10.0; }
    double minT = 1e18, maxT = 0.0;
    auto checkKf = [&](const QVector<Keyframe>& vks) {
        for (const Keyframe& k : vks) {
            minT = qMin(minT, k.time);
            maxT = qMax(maxT, k.time);
        }
    };
    checkKf(mc->kfCamX); checkKf(mc->kfCamY);
    checkKf(mc->kfCamZoom); checkKf(mc->kfCamRotation);
    for (const QString& tid : mc->trackIds) {
        Track* t = findTrack(tid);
        if (!t) continue;
        for (const Clip& c : t->clips) {
            minT = qMin(minT, c.pos);
            maxT = qMax(maxT, c.pos + c.dur);
        }
        checkKf(t->kfMesaX); checkKf(t->kfMesaY);
        checkKf(t->kfMesaScaleX); checkKf(t->kfMesaScaleY);
        checkKf(t->kfMesaRotation); checkKf(t->kfMesaOpacity);
        checkKf(t->kfMesaAnchorX); checkKf(t->kfMesaAnchorY);
    }
    if (minT > maxT) { m_contentStart = 0.0; return qMax(5.0, maxT + 2.0); }
    m_contentStart = minT;
    return qMax(5.0, (maxT - minT) + 2.0);
}

double MesaWidget::contentStartTime() const {
    mesaDuration();  // ensure m_contentStart is computed
    return m_contentStart;
}

int MesaWidget::timeToX(double t, int rulerW) const {
    const double dur = mesaDuration();
    const double start = m_contentStart;
    const double pps = qMax(20.0, (rulerW - 20.0) / dur);
    return 10 + (int)qRound((t - start) * pps);
}

double MesaWidget::xToTime(int x, int rulerW) const {
    const double dur = mesaDuration();
    const double start = m_contentStart;
    const double pps = qMax(20.0, (rulerW - 20.0) / dur);
    return qMax(0.0, start + (x - 10.0) / pps);
}

bool MesaWidget::isInMiniTimeline(const QPoint& p) const {
    const int tlY = height() - propPanelHeight() - 16 - miniTimelineHeight();
    return p.y() >= tlY && p.y() < tlY + miniTimelineHeight();
}

bool MesaWidget::isKfSelected(const KfRef& r) const {
    return m_selectedKfs.contains(r);
}

void MesaWidget::toggleKfSelection(const KfRef& r, bool ctrl) {
    if (!ctrl) {
        if (m_selectedKfs.size() == 1 && m_selectedKfs.contains(r))
            return; // already sole selection
        m_selectedKfs.clear();
        m_selectedKfs.insert(r);
    } else {
        if (m_selectedKfs.contains(r))
            m_selectedKfs.remove(r);
        else
            m_selectedKfs.insert(r);
    }
}

void MesaWidget::deleteSelectedKfs() {
    MesaComposition* mc = currentMesa();
    if (!mc) return;

    // Remove APENAS a property representada por cada KfRef selecionado:
    // antes o Delete apagava todos os keyframes no mesmo tempo.
    for (const KfRef& r : m_selectedKfs) {
        QVector<Keyframe>* vks = nullptr;
        if (r.source == KfRef::Cam) {
            switch (r.prop) {
                case PCamX: vks = &mc->kfCamX; break;
                case PCamY: vks = &mc->kfCamY; break;
                case PCamZ: vks = &mc->kfCamZoom; break;
                case PCamR: vks = &mc->kfCamRotation; break;
            }
        } else {
            Track* t = findTrack(r.trackId);
            if (!t) continue;
            switch (r.prop) {
                case PLayX: vks = &t->kfMesaX; break;
                case PLayY: vks = &t->kfMesaY; break;
                case PLaySX: vks = &t->kfMesaScaleX; break;
                case PLaySY: vks = &t->kfMesaScaleY; break;
                case PLayRot: vks = &t->kfMesaRotation; break;
                case PLayOp: vks = &t->kfMesaOpacity; break;
                case PLayAX: vks = &t->kfMesaAnchorX; break;
                case PLayAY: vks = &t->kfMesaAnchorY; break;
                default: break;
            }
        }
        if (!vks) continue;
        for (int i = vks->size() - 1; i >= 0; --i) {
            if (qFuzzyCompare((*vks)[i].time, r.time)) vks->remove(i);
        }
    }
    m_selectedKfs.clear();
    emit modified();
    update();
}

MesaWidget::KfRef MesaWidget::hitTestKf(const QPoint& pos) const {
    KfRef miss{KfRef::Cam, QString(), -1.0, 0};
    if (!isInMiniTimeline(pos)) return miss;
    MesaComposition* mc = currentMesa();
    if (!mc) return miss;
    const int tlY = height() - propPanelHeight() - 16 - miniTimelineHeight();
    const int rulerW = width();
    const double dur = mesaDuration();
    const double start = m_contentStart;
    const double pps = qMax(20.0, (rulerW - 20.0) / dur);
    const int hitR = 6;  // pixels radius for hit

    // Pistas: keyframes de câmera ficam na faixa superior, das camadas na
    // inferior. Mantém a mesma posição visual de desenho do drawMiniTimeline.
    const int camLaneY = tlY + 14;
    const int layerLaneY = tlY + (miniTimelineHeight() / 2) + 8;

    auto checkKf = [&](const QVector<Keyframe>& vks, KfRef::Source src,
                       const QString& tid, int prop, int laneY) -> KfRef {
        for (const Keyframe& k : vks) {
            const int x = 10 + (int)qRound((k.time - start) * pps);
            if (std::abs(pos.x() - x) <= hitR && std::abs(pos.y() - laneY) <= hitR + 2)
                return {src, tid, k.time, prop};
        }
        return miss;
    };

    // Camera
    KfRef r = checkKf(mc->kfCamX, KfRef::Cam, QString(), PCamX, camLaneY);
    if (r.time >= 0) return r;
    r = checkKf(mc->kfCamY, KfRef::Cam, QString(), PCamY, camLaneY);
    if (r.time >= 0) return r;
    r = checkKf(mc->kfCamZoom, KfRef::Cam, QString(), PCamZ, camLaneY);
    if (r.time >= 0) return r;
    r = checkKf(mc->kfCamRotation, KfRef::Cam, QString(), PCamR, camLaneY);
    if (r.time >= 0) return r;

    // Tracks
    for (const QString& tid : mc->trackIds) {
        Track* t = findTrack(tid);
        if (!t) continue;
        r = checkKf(t->kfMesaX, KfRef::MesaTrack, tid, PLayX, layerLaneY);
        if (r.time >= 0) return r;
        r = checkKf(t->kfMesaY, KfRef::MesaTrack, tid, PLayY, layerLaneY);
        if (r.time >= 0) return r;
        r = checkKf(t->kfMesaScaleX, KfRef::MesaTrack, tid, PLaySX, layerLaneY);
        if (r.time >= 0) return r;
        r = checkKf(t->kfMesaScaleY, KfRef::MesaTrack, tid, PLaySY, layerLaneY);
        if (r.time >= 0) return r;
        r = checkKf(t->kfMesaRotation, KfRef::MesaTrack, tid, PLayRot, layerLaneY);
        if (r.time >= 0) return r;
        r = checkKf(t->kfMesaOpacity, KfRef::MesaTrack, tid, PLayOp, layerLaneY);
        if (r.time >= 0) return r;
        r = checkKf(t->kfMesaAnchorX, KfRef::MesaTrack, tid, PLayAX, layerLaneY);
        if (r.time >= 0) return r;
        r = checkKf(t->kfMesaAnchorY, KfRef::MesaTrack, tid, PLayAY, layerLaneY);
        if (r.time >= 0) return r;
    }
    return miss;
}

void MesaWidget::ensureKeyframesAt(double timeSec) {
    MesaComposition* mc = currentMesa();
    if (!mc) return;
    const double rel = qMax(0.0, timeSec);
    auto upsert = [&](QVector<Keyframe>& vks, double val) {
        upsertKeyframe(vks, rel, val, KfLinear);
    };
    upsert(mc->kfCamX, mc->camX);
    upsert(mc->kfCamY, mc->camY);
    upsert(mc->kfCamZoom, mc->camZoom);
    upsert(mc->kfCamRotation, mc->camRotation);
}

// Grava keyframes APENAS da track editada (como no AE: auto-key afeta só as
// propriedades mexidas). Antes, writeAllKeyframes() gravava câmera + todas as
// layers no playhead a cada transform, poluindo tudo de keyframes.
void MesaWidget::writeTrackKeyframes(Track* t) {
    if (!t || !m_project) return;
    const double rel = qMax(0.0, m_playheadTime);
    auto upsert = [&](QVector<Keyframe>& vks, double val) {
        upsertKeyframe(vks, rel, val, KfLinear);
    };
    upsert(t->kfMesaX, t->mesaX);
    upsert(t->kfMesaY, t->mesaY);
    upsert(t->kfMesaScaleX, t->mesaScaleX);
    upsert(t->kfMesaScaleY, t->mesaScaleY);
    upsert(t->kfMesaRotation, t->mesaRotation);
    upsert(t->kfMesaOpacity, t->mesaOpacity);
    upsert(t->kfMesaAnchorX, t->mesaAnchorX);
    upsert(t->kfMesaAnchorY, t->mesaAnchorY);
}

void MesaWidget::nudgeSelection(double dx, double dy) {
    MesaComposition* mc = currentMesa();
    if (!mc) return;
    const QVector<Track*> tracks = mesaTracks();
    const double rel = qMax(0.0, m_playheadTime);
    auto snap = [&](double v) {
        return m_snapToGrid ? qRound(v / kGridSize) * kGridSize : v;
    };
    if (m_selectedIdx >= 0 && m_selectedIdx < tracks.size()) {
        Track* t = tracks[m_selectedIdx];
        if (t->mesaLocked) return;  // cadeado impede transform por teclado também
        t->mesaX = snap(t->mesaX + dx);
        t->mesaY = snap(t->mesaY + dy);
        if (m_autoKey) {
            upsertKeyframe(t->kfMesaX, rel, t->mesaX);
            upsertKeyframe(t->kfMesaY, rel, t->mesaY);
        }
    } else {
        mc->camX = snap(mc->camX + dx);
        mc->camY = snap(mc->camY + dy);
        if (m_autoKey) {
            upsertKeyframe(mc->kfCamX, rel, mc->camX);
            upsertKeyframe(mc->kfCamY, rel, mc->camY);
        }
    }
    emit modified();
    emit changesCommitted();
    update();
}

double MesaWidget::propFieldValue(int kind) const {
    const double rel = qMax(0.0, m_playheadTime);
    if (kind >= PC_X) {
        const MesaComposition* mc = currentMesa();
        if (!mc) return 0.0;
        switch (kind) {
            case PC_X: return kfValue(mc->kfCamX, mc->camX, rel);
            case PC_Y: return kfValue(mc->kfCamY, mc->camY, rel);
            case PC_Z: return kfValue(mc->kfCamZoom, mc->camZoom, rel);
            case PC_R: return kfValue(mc->kfCamRotation, mc->camRotation, rel);
        }
        return 0.0;
    }
    const QVector<Track*> tracks = mesaTracks();
    if (m_selectedIdx < 0 || m_selectedIdx >= tracks.size()) return 0.0;
    const Track* t = tracks[m_selectedIdx];
    switch (kind) {
        case PL_X: return kfValue(t->kfMesaX, t->mesaX, rel);
        case PL_Y: return kfValue(t->kfMesaY, t->mesaY, rel);
        case PL_S: return kfValue(t->kfMesaScaleX, t->mesaScaleX, rel);
        case PL_R: return kfValue(t->kfMesaRotation, t->mesaRotation, rel);
        case PL_O: return kfValue(t->kfMesaOpacity, t->mesaOpacity, rel);
        case PL_AX: return kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel);
        case PL_AY: return kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel);
    }
    return 0.0;
}

// ═══════════════════════════════════════════════════════════════════════
// Mini-timeline (régua de tempo + keyframes + playhead)
// ═══════════════════════════════════════════════════════════════════════

static double niceStepMini(double raw) {
    if (raw <= 0) return 1.0;
    const double mag = qPow(10.0, qFloor(qLn(raw) / qLn(10.0)));
    const double norm = raw / mag;
    double nice;
    if (norm < 1.5)      nice = 1.0;
    else if (norm < 3.0) nice = 2.0;
    else if (norm < 7.0) nice = 5.0;
    else                 nice = 10.0;
    return nice * mag;
}

void MesaWidget::drawMiniTimeline(QPainter& p) {
    MesaComposition* mc = currentMesa();
    if (!mc) return;

    const int tlH = miniTimelineHeight();
    const int propH = propPanelHeight();
    const int infoH = 16;
    const int tlY = height() - propH - infoH - tlH;
    const int rulerW = width();
    m_miniTimelineRect = QRect(0, tlY, rulerW, tlH);

    // Fundo com gradiente sutil
    QLinearGradient bgGrad(0, tlY, 0, tlY + tlH);
    bgGrad.setColorAt(0.0, QColor(34, 34, 38));
    bgGrad.setColorAt(1.0, QColor(26, 26, 30));
    p.fillRect(m_miniTimelineRect, bgGrad);

    // Separador superior com glow
    p.setPen(QPen(QColor(70, 70, 76), 1));
    p.drawLine(0, tlY, rulerW, tlY);
    p.setPen(QPen(QColor(50, 50, 56), 1));
    p.drawLine(0, tlY + 1, rulerW, tlY + 1);

    const double dur = mesaDuration();
    const double start = m_contentStart;
    const double pps = qMax(20.0, (rulerW - 20.0) / dur);

    // ── Régua de tempo (ticks) ──
    const double tStep = niceStepMini(dur / 8.0);
    QFont tf = p.font();
    tf.setPointSizeF(7);
    p.setFont(tf);
    for (double t = start; t <= start + dur + 1e-9; t += tStep) {
        const int x = 10 + (int)qRound((t - start) * pps);
        if (x > rulerW - 5) break;
        // Tick maior
        p.setPen(QPen(QColor(80, 80, 88), 1));
        p.drawLine(x, tlY + 3, x, tlY + 12);
        // Label
        p.setPen(QColor(160, 160, 172));
        QString label;
        if (dur <= 10.0)
            label = QString::number(t, 'f', 1) + "s";
        else
            label = QString::number(t, 'g', 3) + "s";
        p.drawText(x + 3, tlY + 11, label);
        // Ticks menores
        const double subStep = tStep / 4.0;
        p.setPen(QColor(60, 60, 66));
        for (int s = 1; s < 4; ++s) {
            const int sx = 10 + (int)qRound((t + s * subStep - start) * pps);
            if (sx > rulerW - 5) break;
            p.drawLine(sx, tlY + 7, sx, tlY + 12);
        }
    }

    // ── Keyframe diamonds ──
    // Cor por PROPRIEDADE (não por camada): dá pra distinguir o que é X, Y,
    // escala, rotação, opacidade, âncora na mini-timeline. Duas pistas:
    // câmera (superior) e camadas (inferior).
    auto propColor = [](int prop) -> QColor {
        switch (prop) {
            case PCamX: case PLayX: return QColor(96, 178, 255);        // X (azul)
            case PCamY: case PLayY: return QColor(116, 226, 255);       // Y (ciano)
            case PCamZ: return QColor(96, 255, 214);                    // zoom (teal)
            case PLaySX: return QColor(120, 255, 150);                  // escala X (verde)
            case PLaySY: return QColor(180, 255, 120);                  // escala Y (verde-claro)
            case PCamR: case PLayRot: return QColor(255, 190, 110);     // rotação (laranja)
            case PLayOp: return QColor(255, 130, 160);                  // opacidade (rosa)
            case PLayAX: return QColor(255, 210, 120);                  // âncora X (âmbar)
            case PLayAY: return QColor(240, 150, 255);                  // âncora Y (lilás)
        }
        return QColor(180, 180, 180);
    };

    const int camLaneY = tlY + 14;
    const int layerLaneY = tlY + tlH / 2 + 8;

    auto drawKfDiamonds = [&](const QVector<Keyframe>& vks, int prop,
                              KfRef::Source src, const QString& tid, int laneY) {
        for (const Keyframe& k : vks) {
            const int x = 10 + (int)qRound((k.time - start) * pps);
            if (x < 5 || x > rulerW - 5) continue;
            const double sz = 4.0;
            const QPolygonF diamond = QPolygonF()
                << QPointF(x, laneY - sz) << QPointF(x + sz, laneY)
                << QPointF(x, laneY + sz) << QPointF(x - sz, laneY);
            KfRef ref{src, tid, k.time, prop};
            const bool sel = isKfSelected(ref);
            const QColor col = propColor(prop);
            // Selection glow
            if (sel) {
                p.setPen(QPen(QColor(255, 255, 255, 180), 2.0));
                p.setBrush(Qt::NoBrush);
                p.drawPolygon(diamond);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? col.lighter(150) : col);
            p.drawPolygon(diamond);
        }
    };

    // Rótulos das pistas (7px, bem discretos)
    QFont lf = p.font();
    lf.setPointSizeF(6);
    p.setFont(lf);
    p.setPen(QColor(90, 100, 110));
    p.drawText(m_miniTimelineRect.left() + 2, camLaneY + 3, QStringLiteral("CAM"));
    p.drawText(m_miniTimelineRect.left() + 2, layerLaneY + 3, QStringLiteral("LAY"));

    // Camera keyframes (pista superior)
    const QString camTid;
    drawKfDiamonds(mc->kfCamX, PCamX, KfRef::Cam, camTid, camLaneY);
    drawKfDiamonds(mc->kfCamY, PCamY, KfRef::Cam, camTid, camLaneY);
    drawKfDiamonds(mc->kfCamZoom, PCamZ, KfRef::Cam, camTid, camLaneY);
    drawKfDiamonds(mc->kfCamRotation, PCamR, KfRef::Cam, camTid, camLaneY);

    // Track keyframes (pista inferior)
    const QVector<Track*> tracks = mesaTracks();
    for (int i = 0; i < tracks.size(); ++i) {
        Track* t = tracks[i];
        drawKfDiamonds(t->kfMesaX, PLayX, KfRef::MesaTrack, t->id, layerLaneY);
        drawKfDiamonds(t->kfMesaY, PLayY, KfRef::MesaTrack, t->id, layerLaneY);
        drawKfDiamonds(t->kfMesaScaleX, PLaySX, KfRef::MesaTrack, t->id, layerLaneY);
        drawKfDiamonds(t->kfMesaScaleY, PLaySY, KfRef::MesaTrack, t->id, layerLaneY);
        drawKfDiamonds(t->kfMesaRotation, PLayRot, KfRef::MesaTrack, t->id, layerLaneY);
        drawKfDiamonds(t->kfMesaOpacity, PLayOp, KfRef::MesaTrack, t->id, layerLaneY);
        drawKfDiamonds(t->kfMesaAnchorX, PLayAX, KfRef::MesaTrack, t->id, layerLaneY);
        drawKfDiamonds(t->kfMesaAnchorY, PLayAY, KfRef::MesaTrack, t->id, layerLaneY);
    }

    // ── Playhead (linha vertical com gradiente) ──
    const int phX = 10 + (int)qRound((m_playheadTime - start) * pps);
    if (phX >= 5 && phX <= rulerW - 5) {
        QPen phPen(QColor(255, 70, 70), 2);
        phPen.setCapStyle(Qt::RoundCap);
        p.setPen(phPen);
        p.drawLine(phX, tlY + 1, phX, tlY + tlH - 1);
        // Cabeça do playhead
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 70, 70));
        const QPolygonF head = QPolygonF()
            << QPointF(phX - 5, tlY + 1) << QPointF(phX + 5, tlY + 1)
            << QPointF(phX, tlY + 8);
        p.drawPolygon(head);
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Paint — canvas infinito
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Workspace background
    p.fillRect(rect(), QColor(45, 45, 45));

    MesaComposition* mc = currentMesa();
    if (!mc) {
        p.setPen(QColor(120, 120, 120));
        p.setFont(QFont("DejaVu Sans", 10));
        p.drawText(rect().adjusted(0, 0, 0, -40), Qt::AlignCenter,
                   tr("Nenhuma Mesa selecionada\nClique direito na timeline \xe2\x86\x92 Criar Mesa"));

        const int bw = 160, bh = 36;
        const QRect btnRect(width() / 2 - bw / 2, height() / 2 + 20, bw, bh);
        m_createMesaBtnRect = btnRect;
        p.setPen(QPen(QColor(80, 180, 255), 1));
        p.setBrush(QColor(55, 55, 55));
        p.drawRoundedRect(btnRect, 4, 4);
        p.setPen(QColor(80, 180, 255));
        p.setFont(QFont("DejaVu Sans", 9));
        p.drawText(btnRect, Qt::AlignCenter, tr("Criar Mesa"));
        return;
    }

    const double rel = qMax(0.0, m_playheadTime);
    const QVector<Track*> tracks = mesaTracks();

    // ── Grid do workspace ──
    {
        const double gridPx = kGridSize * m_zoom;
        if (gridPx >= 20.0 && gridPx <= 400.0) {
            const QPointF tl = screenToCanvas(QPointF(0, 0));
            const QPointF br = screenToCanvas(QPointF(width(), height()));
            const double majorStep = kGridSize * 5;
            const double majorPx = majorStep * m_zoom;

            if (majorPx >= 40.0) {
                // Grid menor
                const double minorAlpha = qBound(0.0, (gridPx - 20.0) / 40.0, 1.0) * 25.0;
                p.setPen(QPen(QColor(255, 255, 255, (int)minorAlpha), 1.0));
                const double gx0 = qFloor(tl.x() / kGridSize) * kGridSize;
                const double gy0 = qFloor(tl.y() / kGridSize) * kGridSize;
                for (double gx = gx0; gx <= br.x(); gx += kGridSize) {
                    if (qFuzzyIsNull(fmod(gx, majorStep))) continue;
                    const double sx = canvasToScreen(QPointF(gx, 0)).x();
                    p.drawLine(QPointF(sx, 0), QPointF(sx, height()));
                }
                for (double gy = gy0; gy <= br.y(); gy += kGridSize) {
                    if (qFuzzyIsNull(fmod(gy, majorStep))) continue;
                    const double sy = canvasToScreen(QPointF(0, gy)).y();
                    p.drawLine(QPointF(0, sy), QPointF(width(), sy));
                }

                // Grid maior
                const double majorAlpha = qBound(0.0, (majorPx - 40.0) / 80.0, 1.0) * 45.0;
                p.setPen(QPen(QColor(255, 255, 255, (int)majorAlpha), 1.0));
                const double mgx0 = qFloor(tl.x() / majorStep) * majorStep;
                const double mgy0 = qFloor(tl.y() / majorStep) * majorStep;
                for (double gx = mgx0; gx <= br.x(); gx += majorStep) {
                    const double sx = canvasToScreen(QPointF(gx, 0)).x();
                    p.drawLine(QPointF(sx, 0), QPointF(sx, height()));
                }
                for (double gy = mgy0; gy <= br.y(); gy += majorStep) {
                    const double sy = canvasToScreen(QPointF(0, gy)).y();
                    p.drawLine(QPointF(0, sy), QPointF(width(), sy));
                }
            }
        }
    }

    // ── Origin crosshair ──
    {
        const QPointF origin = canvasToScreen(QPointF(0, 0));
        p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
        p.drawLine(QPointF(origin.x(), 0), QPointF(origin.x(), height()));
        p.drawLine(QPointF(0, origin.y()), QPointF(width(), origin.y()));
    }

    // ── Frame da composição (estilo AE: área útil destacada, fora escurecido) ──
    {
        const QPointF c0 = canvasToScreen(QPointF(0, 0));
        const QPointF c1 = canvasToScreen(QPointF(mc->canvasW, mc->canvasH));
        const QRectF compRect = QRectF(c0, c1).normalized();
        QPainterPath outside;
        outside.addRect(QRectF(rect()));
        outside.addRect(compRect);
        outside.setFillRule(Qt::OddEvenFill);
        p.setPen(Qt::NoPen);
        p.fillPath(outside, QColor(0, 0, 0, 96));
        p.setPen(QPen(QColor(255, 255, 255, 110), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(compRect);
    }

    // ── Desenha layers diretamente no canvas infinito ──
    // Configura transformação canvas→screen no painter
    p.save();
    const double cx = width() / 2.0 + m_offset.x();
    const double cy = height() / 2.0 + m_offset.y();
    p.translate(cx, cy);
    p.scale(m_zoom, m_zoom);

    const QString* skipId = nullptr;
    if (m_transformOp != TNone && !m_dragTrackId.isEmpty())
        skipId = &m_dragTrackId;

    // Desenha todas as layers (exceto a sendo arrastada)
    m_renderer.renderToPainter(p, *mc, *m_project, rel, skipId);

    // Se estiver arrastando, desenha a camada arrastada separadamente
    if (skipId) {
        Track* dragTrack = findTrack(m_dragTrackId);
        if (dragTrack) {
            MesaRenderer::LayerPrep prep;
            if (m_renderer.prepareLayer(prep, *mc, *m_project, rel, *dragTrack)) {
                m_renderer.drawTrackImage(p, prep);
            }
        }
    }

    p.restore();

    // ── Selection handles (em screen-space) ──
    for (int i = 0; i < tracks.size(); ++i) {
        const Track* t = tracks[i];
        if (t->mesaHidden) continue;  // camada oculta não mostra nem placeholder
        const bool sel = (i == m_selectedIdx);
        const LayerBounds lb = layerBounds(t, i);

        // Placeholder para layers sem conteúdo — desenhado pelo MESMO quad da
        // seleção (layerScreenRect), incluindo a rotação em torno do âncora:
        // o que você vê é exatamente o que o hit-test enxerga.
        if (!lb.hasContent) {
            QPointF center, corners[4], rotH;
            layerScreenRect(lb, center, corners, rotH);

            // Cor: amarela se agrupada na Mesa, senão cor indexada
            QColor fill;
            const TrackGroup* tg = t->groupId.isEmpty() ? nullptr : m_project->findGroup(t->groupId);
            if (tg && tg->mesaId == m_mesaId) {
                fill = QColor::fromHsv(42, 50, 40, 120);  // amarelo Premier-style
            } else {
                fill = QColor::fromHsv((i * 47 + 180) % 360, 30, 35, 100);
            }

            QPainterPath placeholderPath;
            placeholderPath.moveTo(corners[0]);
            placeholderPath.lineTo(corners[1]);
            placeholderPath.lineTo(corners[2]);
            placeholderPath.lineTo(corners[3]);
            placeholderPath.closeSubpath();

            p.setPen(sel ? QPen(QColor(255, 255, 255), 1.5)
                         : QPen(fill.lighter(130), 1.0, Qt::DashLine));
            p.setBrush(fill);
            p.drawPath(placeholderPath);

            QFont f = p.font();
            f.setPointSizeF(8);
            f.setBold(sel);
            p.setFont(f);
            p.setPen(sel ? QColor(255, 255, 255) : fill.lighter(180));
            const QRectF textRect(center.x() - 80, center.y() - 12, 160, 24);
            p.drawText(textRect, Qt::AlignCenter, t->name);
        }

        if (!sel) continue;

        // Handles de seleção (white AE-style)
        QPointF center, corners[4], rotateHandle;
        layerScreenRect(lb, center, corners, rotateHandle);

        p.setPen(QPen(QColor(255, 255, 255, 200), 1.0));
        p.drawLine(center, rotateHandle);

        p.setPen(QPen(QColor(255, 255, 255), 1.5));
        p.setBrush(QColor(45, 45, 45));
        p.drawEllipse(rotateHandle, 5, 5);

        p.setPen(QPen(QColor(255, 255, 255, 200), 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawLine(corners[0], corners[1]);
        p.drawLine(corners[1], corners[2]);
        p.drawLine(corners[2], corners[3]);
        p.drawLine(corners[3], corners[0]);

        p.setPen(QPen(QColor(0, 0, 0, 120), 1.0));
        p.setBrush(QColor(255, 255, 255));
        const double hs = 4.0;
        for (int j = 0; j < 4; ++j)
            p.drawRect(QRectF(corners[j].x() - hs, corners[j].y() - hs, hs * 2, hs * 2));

        p.setBrush(QColor(220, 220, 220));
        const double hsm = 3.0;
        for (int e = 0; e < 4; ++e) {
            const QPointF mid = (corners[e] + corners[(e + 1) % 4]) / 2.0;
            p.drawRect(QRectF(mid.x() - hsm, mid.y() - hsm, hsm * 2, hsm * 2));
        }

        const QPointF anchor = canvasToScreen(QPointF(lb.anchorX + lb.x, lb.anchorY + lb.y));
        p.setPen(QPen(QColor(255, 60, 60), 1.5));
        p.drawLine(anchor + QPointF(-6, 0), anchor + QPointF(6, 0));
        p.drawLine(anchor + QPointF(0, -6), anchor + QPointF(0, 6));
    }

    // ── Câmera (guia visual, sempre visível) ──
    {
        const double cXi = mc->canvasW / 2.0 + kfValue(mc->kfCamX, mc->camX, rel);
        const double cYi = mc->canvasH / 2.0 + kfValue(mc->kfCamY, mc->camY, rel);
        const double cZi = kfValue(mc->kfCamZoom, mc->camZoom, rel);
        const double cRi = kfValue(mc->kfCamRotation, mc->camRotation, rel);
        const double camW = mc->canvasW / qMax(0.01, cZi);
        const double camH = mc->canvasH / qMax(0.01, cZi);
        const QPointF cc = canvasToScreen(QPointF(cXi, cYi));

        const bool camDefault = qFuzzyCompare(cXi, 0.0) && qFuzzyCompare(cYi, 0.0)
                                && qFuzzyCompare(cZi, 1.0) && qFuzzyCompare(cRi, 0.0);
        const double camAlpha = camDefault ? 60.0 : 200.0;

        // Viewport da câmera no canvas-space
        const double camScreenW = camW * m_zoom;
        const double camScreenH = camH * m_zoom;

        // Só desenha se visível
        if (camScreenW > 2 && camScreenH > 2) {
            p.save();
            p.translate(cc);
            p.rotate(cRi);
            const double hw = camScreenW / 2;
            const double hh = camScreenH / 2;

            // Borda tracejada branca
            p.setPen(QPen(QColor(255, 255, 255, (int)camAlpha), 1.0, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(-hw, -hh, hw * 2, hh * 2);

            // Rule of thirds
            if (camScreenW > 60 && camScreenH > 60) {
                p.setPen(QPen(QColor(255, 255, 255, (int)(camAlpha * 0.3)), 0.5));
                for (int g = 1; g <= 2; ++g) {
                    const double fx = -hw + (hw * 2.0 * g / 3.0);
                    const double fy = -hh + (hh * 2.0 * g / 3.0);
                    p.drawLine(QPointF(fx, -hh), QPointF(fx, hh));
                    p.drawLine(QPointF(-hw, fy), QPointF(hw, fy));
                }
            }

            // Label "CAM"
            QFont cf = p.font();
            cf.setPointSizeF(7);
            cf.setBold(true);
            p.setFont(cf);
            p.setPen(QColor(255, 255, 255, (int)camAlpha));
            p.drawText(QRectF(-hw + 4, -hh + 3, hw * 2, 14), Qt::AlignTop | Qt::AlignLeft,
                       QStringLiteral("CAM"));

            // Handles de canto
            const double hsz = camDefault ? 3.0 : 4.0;
            p.setPen(QPen(QColor(0, 0, 0, 80), 1.0));
            p.setBrush(QColor(255, 255, 255, (int)camAlpha));
            const QPointF chits[4] = {
                { hw,  hh}, {-hw,  hh}, { hw, -hh}, {-hw, -hh}
            };
            for (int i = 0; i < 4; ++i)
                p.drawRect(QRectF(chits[i].x() - hsz, chits[i].y() - hsz, hsz * 2, hsz * 2));

            p.restore();
        }
    }

    // ── UI overlays ──
    if (m_showLayerList) drawLayerList(p);
    drawMiniTimeline(p);
    drawPropertyPanel(p);

    // ── Header bar ──
    {
        const int hh = 22;
        p.fillRect(0, 0, width(), hh, QColor(35, 35, 35));
        p.setPen(QColor(70, 70, 70));
        p.drawLine(0, hh, width(), hh);

        QFont hf = p.font();
        hf.setPointSizeF(8);
        p.setFont(hf);
        p.setPen(QColor(180, 180, 180));
        const QString label = mc->name.isEmpty()
            ? QStringLiteral("Mesa \xe2\x80\x94 %1\xd7%2").arg(mc->canvasW).arg(mc->canvasH)
            : QStringLiteral("%1 \xe2\x80\x94 %2\xd7%3").arg(mc->name).arg(mc->canvasW).arg(mc->canvasH);
        p.drawText(QRect(8, 0, width() - 16 - 92, hh), Qt::AlignLeft | Qt::AlignVCenter, label);

        // Botão AUTO KEY (toggle, estilo AE): ligado = grava keyframe ao editar.
        const QRect akRect(width() - 88, 3, 80, 16);
        m_autoKeyBtnRect = akRect;
        p.setPen(QPen(m_autoKey ? QColor(230, 70, 70) : QColor(100, 100, 100), 1));
        p.setBrush(m_autoKey ? QColor(230, 70, 70, 35) : QColor(80, 80, 80, 60));
        p.drawRoundedRect(akRect, 3, 3);
        p.setPen(m_autoKey ? QColor(255, 120, 120) : QColor(150, 150, 150));
        p.drawText(akRect, Qt::AlignCenter,
                   m_autoKey ? QStringLiteral("\xe2\x97\x8f AUTO KEY")
                             : QStringLiteral("\xe2\x97\x8b AUTO KEY"));
    }

    // ── Info bar ──
    QFont infof = p.font();
    infof.setPointSizeF(7);
    p.setFont(infof);
    p.setPen(QColor(100, 100, 100));
    const int infoY = height() - propPanelHeight() - 16 - miniTimelineHeight();
    p.drawText(QRect(6, infoY, width() - 12, 14), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Zoom %1%  |  %2\xd7%3  |  %4 layers  |  G: snap %5  |  L: layers")
                   .arg((int)(m_zoom * 100)).arg(mc->canvasW).arg(mc->canvasH)
                   .arg(tracks.size())
                   .arg(m_snapToGrid ? "ON" : "OFF"));
}

// ═══════════════════════════════════════════════════════════════════════
// Interface pública
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::fitToContent() {
    MesaComposition* mc = currentMesa();
    if (!mc || width() <= 0 || height() <= 0) return;
    const double pad = 40.0;
    const double availW = width() - pad * 2;
    const double availH = height() - pad * 2 - propPanelHeight() - 22 - miniTimelineHeight();
    if (availW <= 0 || availH <= 0) return;
    const double sx = availW / mc->canvasW;
    const double sy = availH / mc->canvasH;
    m_zoom = qMin(sx, sy);
    // Centraliza o canvas no widget: a origem do canvas fica em
    // (widget_center - canvasW/2 * zoom), não no centro do widget.
    const QPointF center(width() / 2.0, height() / 2.0);
    m_offset = center - QPointF(mc->canvasW / 2.0, mc->canvasH / 2.0) * m_zoom
             - center;  // simplifica para -canvas/2 * zoom
}

void MesaWidget::setMesaId(const QString& id) {
    m_mesaId = id;
    m_selectedIdx = -1;
    m_transformOp = TNone;
    m_cachedTracks.clear();
    m_cachedTracksVersion = 0;
    fitToContent();
    update();
}

void MesaWidget::refresh() {
    m_mediaSizes.clear();
    m_cachedTracks.clear();
    m_cachedTracksVersion = 0;
    update();
}

void MesaWidget::autoSelectMesa() {
    if (!m_project) return;
    if (!m_mesaId.isEmpty() && m_project->findMesa(m_mesaId)) {
        m_cachedTracks.clear();
        m_cachedTracksVersion = 0;
        update();
        return;
    }
    for (const MesaComposition& m : m_project->mesas) {
        for (const QString& tid : m.trackIds) {
            if (findTrack(tid)) {
                m_mesaId = m.id;
                m_selectedIdx = -1;
                m_cachedTracks.clear();
                m_cachedTracksVersion = 0;
                fitToContent();
                update();
                return;
            }
        }
    }
    m_mesaId.clear();
    update();
}
