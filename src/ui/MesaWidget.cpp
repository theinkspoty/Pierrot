// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaWidget.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QShortcut>
#include <QtMath>
#include <algorithm>

Q_LOGGING_CATEGORY(lcMesa, "mesa.widget")

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
    // Atalho do motion blur IGUAL ao Vegas (Ctrl+Shift+B). Usa contexto de
    // janela: funciona mesmo quando o foco está noutro widget (timeline,
    // preview, árvore de mídia) — só precisa de uma mesa aberta no ativo.
    auto* mbShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")), this);
    mbShortcut->setContext(Qt::WindowShortcut);
    connect(mbShortcut, &QShortcut::activated, this, &MesaWidget::toggleMotionBlur);
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

double MesaWidget::timelinePps(int rulerW) const {
    return qMax(20.0, (rulerW - 20.0) / mesaDuration());
}

bool MesaWidget::isInMiniTimeline(const QPoint& p) const {
    if (p.x() < panelWidth()) return false;
    const int tlY = height() - miniTimelineHeight();
    return p.y() >= tlY && p.y() < tlY + miniTimelineHeight();
}

bool MesaWidget::isKfSelected(const KfRef& r) const {
    return m_selectedKfs.contains(r);
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
        qCInfo(lcMesa).noquote() << "[MESA] Delete: removendo kf t="
                                 << QString::number(r.time, 'f', 3) << "s prop=" << r.prop
                                 << (r.source == KfRef::Cam ? "(câmera)" : "'" + r.trackId + "'");
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
    const int tlY = height() - miniTimelineHeight();
    const int x0 = panelWidth();
    const int rulerW = artRect().width();
    const double dur = mesaDuration();
    const double start = m_contentStart;
    const double pps = qMax(20.0, (rulerW - 20.0) / dur);
    const int hitR = 6;  // pixels radius for hit

    // Pista única: keyframes da câmera. Mesma posição visual do drawMiniTimeline.
    const int camLaneY = tlY + 14;

    auto checkKf = [&](const QVector<Keyframe>& vks, KfRef::Source src,
                       const QString& tid, int prop, int laneY) -> KfRef {
        for (const Keyframe& k : vks) {
            const int x = x0 + 10 + (int)qRound((k.time - start) * pps);
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
    return miss;
}

QVector<Keyframe>* MesaWidget::keyframesFor(KfRef::Source src, const QString& trackId, int prop) {
    MesaComposition* mc = currentMesa();
    if (!mc) return nullptr;
    if (src == KfRef::Cam) {
        switch (prop) {
            case PCamX: return &mc->kfCamX;
            case PCamY: return &mc->kfCamY;
            case PCamZ: return &mc->kfCamZoom;
            case PCamR: return &mc->kfCamRotation;
        }
        return nullptr;
    }
    Track* t = findTrack(trackId);
    if (!t) return nullptr;
    switch (prop) {
        case PLayX: return &t->kfMesaX;
        case PLayY: return &t->kfMesaY;
        case PLaySX: return &t->kfMesaScaleX;
        case PLaySY: return &t->kfMesaScaleY;
        case PLayRot: return &t->kfMesaRotation;
        case PLayOp: return &t->kfMesaOpacity;
        case PLayAX: return &t->kfMesaAnchorX;
        case PLayAY: return &t->kfMesaAnchorY;
    }
    return nullptr;
}

void MesaWidget::sortKfs(QVector<Keyframe>& vks) const {
    std::stable_sort(vks.begin(), vks.end(),
                     [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

void MesaWidget::nudgeSelection(double dx, double dy) {
    MesaComposition* mc = currentMesa();
    if (!mc) return;
    const QVector<Track*> tracks = mesaTracks();
    auto snap = [&](double v) {
        return m_snapToGrid ? qRound(v / kGridSize) * kGridSize : v;
    };
    if (!m_selectedIdxs.isEmpty()) {
        for (int i : m_selectedIdxs) {
            if (i < 0 || i >= tracks.size()) continue;
            Track* t = tracks[i];
            if (t->mesaLocked) continue;  // cadeado impede transform por teclado também
            t->mesaX = snap(t->mesaX + dx);
            t->mesaY = snap(t->mesaY + dy);
        }
    } else if (m_cameraSelected) {
        mc->camX = snap(mc->camX + dx);
        mc->camY = snap(mc->camY + dy);
    } else {
        return;  // nada selecionado: setas não mexem em nada
    }
    emit modified();
    emit changesCommitted();
    update();
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
    const int tlY = height() - tlH;
    const int x0 = panelWidth();
    const int rulerW = artRect().width();
    m_miniTimelineRect = QRect(x0, tlY, rulerW, tlH);

    // Fundo com gradiente sutil
    QLinearGradient bgGrad(0, tlY, 0, tlY + tlH);
    bgGrad.setColorAt(0.0, QColor(34, 34, 38));
    bgGrad.setColorAt(1.0, QColor(26, 26, 30));
    p.fillRect(m_miniTimelineRect, bgGrad);

    // Separador superior com glow
    p.setPen(QPen(QColor(70, 70, 76), 1));
    p.drawLine(x0, tlY, x0 + rulerW, tlY);
    p.setPen(QPen(QColor(50, 50, 56), 1));
    p.drawLine(x0, tlY + 1, x0 + rulerW, tlY + 1);

    // Desenha o conteúdo deslocado de x0 (a barra ocupa a área de arte,
    // à direita do painel vertical de camadas).
    p.save();
    if (x0 > 0) p.translate(x0, 0);
    const double dur = mesaDuration();
    const double start = m_contentStart;
    const double pps = qMax(20.0, (rulerW - 20.0) / dur);

    // ── Pista (só a câmera) ──
    p.setPen(Qt::NoPen);
    p.fillRect(QRect(0, tlY + 6, rulerW, 14), QColor(70, 140, 200, 16));

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
    // Cor por PROPRIEDADE da câmera: distingue X, Y, zoom, rotação na
    // mini-timeline (todos na mesma pista, estilo AE). Os keyframes das
    // camadas são editados no Graph Editor — a mini-timeline é só da câmera.
    auto propColor = [](int prop) -> QColor {
        switch (prop) {
            case PCamX: return QColor(96, 178, 255);        // X (azul)
            case PCamY: return QColor(116, 226, 255);       // Y (ciano)
            case PCamZ: return QColor(96, 255, 214);        // zoom (teal)
            case PCamR: return QColor(255, 190, 110);       // rotação (laranja)
        }
        return QColor(180, 180, 180);
    };

    const int camLaneY = tlY + 14;

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

    // Rótulo da pista (só a câmera)
    QFont lf = p.font();
    lf.setPointSizeF(6);
    p.setFont(lf);
    p.setPen(QColor(90, 100, 110));
    p.drawText(2, camLaneY + 3, QStringLiteral("CAM"));

    // Camera keyframes
    const QString camTid;
    drawKfDiamonds(mc->kfCamX, PCamX, KfRef::Cam, camTid, camLaneY);
    drawKfDiamonds(mc->kfCamY, PCamY, KfRef::Cam, camTid, camLaneY);
    drawKfDiamonds(mc->kfCamZoom, PCamZ, KfRef::Cam, camTid, camLaneY);
    drawKfDiamonds(mc->kfCamRotation, PCamR, KfRef::Cam, camTid, camLaneY);

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

        // Chip do tempo atual ao lado do playhead
        QFont cf2 = p.font();
        cf2.setPointSizeF(7);
        p.setFont(cf2);
        const QString tstr = QString::number(m_playheadTime, 'f',
                                             m_playheadTime < 10.0 ? 2 : 1) + "s";
        const QRect chip(phX + 5, tlY + 2, 36, 12);
        p.fillRect(chip, QColor(255, 70, 70, 45));
        p.setPen(QColor(255, 165, 165));
        p.drawText(chip, Qt::AlignLeft | Qt::AlignVCenter, tstr);
    }

    // ── Marquee de seleção (arrastar no vazio da timeline) ──
    if (m_timelineMarquee) {
        const int a = m_marqueeStartPos.x() - x0;
        const int b = m_marqueeCurX - x0;
        const int l = qBound(0, qMin(a, b), rulerW);
        const int r = qBound(0, qMax(a, b), rulerW);
        if (r > l) {
            const QRect mq(l, tlY + 6, r - l, tlH - 12);
            p.fillRect(mq, QColor(80, 180, 255, 40));
            p.setPen(QPen(QColor(120, 200, 255, 190), 1, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            p.drawRect(mq);
        }
    }
    p.restore();
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
        const QPointF c = artCenter();
        const QRect btnRect((int)c.x() - bw / 2, (int)c.y() + 20, bw, bh);
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

    // Tudo que pertence ao canvas fica restrito à área de arte (à direita do
    // painel vertical de camadas); o painel é desenhado depois, por cima.
    p.save();
    p.setClipRect(artRect());

    // ── Grid do workspace ──
    {
        const double gridPx = kGridSize * m_zoom;
        if (gridPx >= 20.0 && gridPx <= 400.0) {
            const QRect ar = artRect();
            const QPointF tl = screenToCanvas(ar.topLeft());
            const QPointF br = screenToCanvas(ar.bottomRight());
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
    // Configura transformação canvas→screen no painter. O m_offset entra no
    // translate para o conteúdo andar JUNTO com o grid/handles no pan (senão
    // a camada ficaria "solta", presa no centro da tela).
    p.save();
    const double cx = artCenter().x();
    const double cy = artCenter().y();
    p.translate(cx + m_offset.x(), cy + m_offset.y());
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
        const bool sel = hasSelection(i);
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

        // Handles de seleção (white AE-style): o gizmo completo (rotate +
        // cantos + âncora) aparece só na primária; as demais selecionadas
        // mostram apenas o contorno tracejado (retângulo da seleção).
        QPointF center, corners[4], rotateHandle;
        layerScreenRect(lb, center, corners, rotateHandle);

        p.setPen(QPen(QColor(255, 255, 255, 200), 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawLine(corners[0], corners[1]);
        p.drawLine(corners[1], corners[2]);
        p.drawLine(corners[2], corners[3]);
        p.drawLine(corners[3], corners[0]);

        if (i == m_selectedIdx) {
            p.setPen(QPen(QColor(255, 255, 255, 200), 1.0));
            p.drawLine(center, rotateHandle);

            p.setPen(QPen(QColor(255, 255, 255), 1.5));
            p.setBrush(QColor(45, 45, 45));
            p.drawEllipse(rotateHandle, 5, 5);

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

            const QPointF anchor = canvasToScreen(QPointF(lb.x, lb.y));
            p.setPen(QPen(QColor(255, 60, 60), 1.5));
            p.drawLine(anchor + QPointF(-6, 0), anchor + QPointF(6, 0));
            p.drawLine(anchor + QPointF(0, -6), anchor + QPointF(0, 6));
        }
    }

    // ── Câmera (guia visual, sempre visível) ──
    {
        const double cXi = kfValue(mc->kfCamX, mc->camX, rel);
        const double cYi = kfValue(mc->kfCamY, mc->camY, rel);
        const double cZi = kfValue(mc->kfCamZoom, mc->camZoom, rel);
        const double cRi = kfValue(mc->kfCamRotation, mc->camRotation, rel);
        const double camW = mc->canvasW / qMax(0.01, cZi);
        const double camH = mc->canvasH / qMax(0.01, cZi);
        const QPointF cc = canvasToScreen(QPointF(cXi, cYi));

        const bool camDefault = qFuzzyCompare(cXi, mc->canvasW / 2.0)
                                && qFuzzyCompare(cYi, mc->canvasH / 2.0)
                                && qFuzzyCompare(cZi, 1.0) && qFuzzyCompare(cRi, 0.0);
        // Selecionada (alvo ativo) = bem visível; senão, sobra só o contorno.
        const double camAlpha = m_cameraSelected ? 230.0
                              : (camDefault ? 60.0 : 200.0);

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
    p.restore();  // fim do clip da área de arte

    // ── Marquee de seleção múltipla ──
    if (m_canvasMarquee && !m_marqueeRect.isNull()) {
        p.setBrush(QColor(80, 150, 255, 40));
        p.setPen(QPen(QColor(80, 170, 255), 1.0));
        p.drawRect(m_marqueeRect);
    }

    // ── UI overlays ──
    if (m_showLayerList) drawLayerList(p);
    drawMiniTimeline(p);

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
        p.drawText(QRect(8, 0, width() - 16, hh), Qt::AlignLeft | Qt::AlignVCenter, label);

        // Botão "MB": toggle de motion blur visível e clicável. Destaque
        // quando ligado (o user precisa VER se está ativo).
        const int bw = 36, bh = 16;
        m_mbButtonRect = QRect(width() - bw - 6, (hh - bh) / 2, bw, bh);
        const bool mbOn = mc->motionBlur;
        p.setPen(QPen(mbOn ? QColor(110, 190, 255) : QColor(120, 120, 120), 1));
        p.setBrush(mbOn ? QColor(70, 150, 220, 90) : QColor(255, 255, 255, 14));
        p.drawRoundedRect(m_mbButtonRect, 4, 4);
        QFont bf = p.font();
        bf.setPointSizeF(7);
        bf.setBold(mbOn);
        p.setFont(bf);
        p.setPen(mbOn ? QColor(170, 215, 255) : QColor(140, 140, 140));
        p.drawText(m_mbButtonRect, Qt::AlignCenter, mbOn
            ? QStringLiteral("MB ON") : QStringLiteral("MB"));
    }

    // ── Info bar ──
    QFont infof = p.font();
    infof.setPointSizeF(7);
    p.setFont(infof);
    p.setPen(QColor(100, 100, 100));
    const int x0 = panelWidth();
    const bool mbOn = currentMesa() && currentMesa()->motionBlur;
    p.drawText(QRect(x0 + 6, height() - miniTimelineHeight() - 14, artRect().width() - 12, 14),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Zoom %1%  |  G: snap %2  |  L: layers  |  Ctrl+Shift+B: MB %3  |  Shift+arrastar: multi")
                   .arg((int)(m_zoom * 100))
                   .arg(m_snapToGrid ? "ON" : "OFF")
                   .arg(mbOn ? "ON" : "OFF"));

    // Se o MB está ligado mas nada tem keyframes, o blur não aparece (só
    // borra o que se move por kfs). Avisa em vez de deixar o user achando
    // que não funcionou.
    if (mbOn) {
        bool hasKf = false;
        for (const Track* t : tracks) {
            if (!t->kfMesaX.isEmpty() || !t->kfMesaY.isEmpty()
                || !t->kfMesaScaleX.isEmpty() || !t->kfMesaScaleY.isEmpty()
                || !t->kfMesaRotation.isEmpty() || !t->kfMesaOpacity.isEmpty()
                || !t->kfMesaAnchorX.isEmpty() || !t->kfMesaAnchorY.isEmpty()) {
                hasKf = true; break;
            }
        }
        if (!hasKf && mc->kfCamX.isEmpty() && mc->kfCamY.isEmpty()
            && mc->kfCamZoom.isEmpty() && mc->kfCamRotation.isEmpty()) {
            p.setPen(QColor(255, 200, 120));
            p.drawText(QRect(x0 + 6, height() - miniTimelineHeight() - 28,
                             artRect().width() - 12, 12),
                       Qt::AlignLeft,
                       QStringLiteral("MB ligado: mova a câmera ou crie keyframes (Graph Editor) pra ver o blur"));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Interface pública
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::fitToContent() {
    MesaComposition* mc = currentMesa();
    if (!mc || width() <= 0 || height() <= 0) return;
    const double pad = 40.0;
    const QRect ar = artRect();
    const double availW = ar.width() - pad * 2;
    const double availH = ar.height() - pad * 2 - 22 - miniTimelineHeight();
    if (availW <= 0 || availH <= 0) return;
    const double sx = availW / mc->canvasW;
    const double sy = availH / mc->canvasH;
    m_zoom = qMin(sx, sy);
    // Centraliza o canvas na área de arte (à direita do painel de camadas):
    // a origem do canvas fica em (art_center - canvasW/2 * zoom).
    const QPointF center = artCenter();
    m_offset = center - QPointF(mc->canvasW / 2.0, mc->canvasH / 2.0) * m_zoom
             - center;  // simplifica para -canvas/2 * zoom
    qCInfo(lcMesa).noquote() << "[MESA] fitToContent: zoom" << (int)(m_zoom * 100)
                             << "% offset" << m_offset << "canvas" << mc->canvasW
                             << "x" << mc->canvasH;
}

void MesaWidget::setMesaId(const QString& id) {
    m_mesaId = id;
    m_selectedIdx = -1;
    m_selectedIdxs.clear();
    m_cameraSelected = false;
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
