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
    if (!mc) return 10.0;
    double maxT = 0.0;
    auto checkMax = [&](const QVector<Keyframe>& vks) {
        for (const Keyframe& k : vks)
            maxT = qMax(maxT, k.time);
    };
    checkMax(mc->kfCamX); checkMax(mc->kfCamY);
    checkMax(mc->kfCamZoom); checkMax(mc->kfCamRotation);
    for (const QString& tid : mc->trackIds) {
        Track* t = findTrack(tid);
        if (!t) continue;
        checkMax(t->kfMesaX); checkMax(t->kfMesaY);
        checkMax(t->kfMesaScaleX); checkMax(t->kfMesaScaleY);
        checkMax(t->kfMesaRotation); checkMax(t->kfMesaOpacity);
        checkMax(t->kfMesaAnchorX); checkMax(t->kfMesaAnchorY);
    }
    return qMax(5.0, maxT + 2.0);
}

int MesaWidget::timeToX(double t, int rulerW) const {
    const double dur = mesaDuration();
    const double pps = qMax(20.0, (rulerW - 20.0) / dur);
    return 10 + (int)qRound(t * pps);
}

double MesaWidget::xToTime(int x, int rulerW) const {
    const double dur = mesaDuration();
    const double pps = qMax(20.0, (rulerW - 20.0) / dur);
    return qMax(0.0, (x - 10.0) / pps);
}

bool MesaWidget::isInMiniTimeline(const QPoint& p) const {
    const int tlY = height() - propPanelHeight() - 16 - miniTimelineHeight();
    return p.y() >= tlY && p.y() < tlY + miniTimelineHeight();
}

void MesaWidget::ensureKeyframesAt(double timeSec) {
    MesaComposition* mc = currentMesa();
    if (!mc) return;
    const double rel = qMax(0.0, timeSec);
    auto upsert = [&](QVector<Keyframe>& vks, double val) {
        for (Keyframe& k : vks)
            if (qFuzzyCompare(k.time, rel)) { k.value = val; return; }
        Keyframe k; k.time = rel; k.value = val; k.interp = KfLinear;
        vks.append(k);
    };
    upsert(mc->kfCamX, mc->camX);
    upsert(mc->kfCamY, mc->camY);
    upsert(mc->kfCamZoom, mc->camZoom);
    upsert(mc->kfCamRotation, mc->camRotation);
}

void MesaWidget::writeAllKeyframes() {
    MesaComposition* mc = currentMesa();
    if (!mc || !m_project) return;
    const double rel = qMax(0.0, m_playheadTime);
    auto upsert = [&](QVector<Keyframe>& vks, double val) {
        for (Keyframe& k : vks)
            if (qFuzzyCompare(k.time, rel)) { k.value = val; return; }
        Keyframe k; k.time = rel; k.value = val; k.interp = KfLinear;
        vks.append(k);
    };
    upsert(mc->kfCamX, mc->camX);
    upsert(mc->kfCamY, mc->camY);
    upsert(mc->kfCamZoom, mc->camZoom);
    upsert(mc->kfCamRotation, mc->camRotation);
    const QVector<Track*> tracks = mesaTracks();
    for (Track* t : tracks) {
        upsert(t->kfMesaX, t->mesaX);
        upsert(t->kfMesaY, t->mesaY);
        upsert(t->kfMesaScaleX, t->mesaScaleX);
        upsert(t->kfMesaScaleY, t->mesaScaleY);
        upsert(t->kfMesaRotation, t->mesaRotation);
        upsert(t->kfMesaOpacity, t->mesaOpacity);
        upsert(t->kfMesaAnchorX, t->mesaAnchorX);
        upsert(t->kfMesaAnchorY, t->mesaAnchorY);
    }
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

    // Fundo
    p.fillRect(m_miniTimelineRect, QColor(30, 30, 33));

    // Separador superior
    p.setPen(QPen(QColor(60, 60, 64), 1));
    p.drawLine(0, tlY, rulerW, tlY);

    const double dur = mesaDuration();
    const double pps = qMax(20.0, (rulerW - 20.0) / dur);
    const int midY = tlY + tlH / 2;

    // ── Régua de tempo (ticks) ──
    const double tStep = niceStepMini(dur / 8.0);
    p.setPen(QColor(90, 90, 96));
    QFont tf = p.font();
    tf.setPointSizeF(7);
    p.setFont(tf);
    for (double t = 0.0; t <= dur + 1e-9; t += tStep) {
        const int x = 10 + (int)qRound(t * pps);
        if (x > rulerW - 5) break;
        // Tick maior
        p.drawLine(x, tlY + 2, x, tlY + 10);
        // Label
        p.setPen(QColor(130, 130, 140));
        QString label;
        if (dur <= 10.0)
            label = QString::number(t, 'f', 1) + "s";
        else
            label = QString::number(t, 'g', 3) + "s";
        p.drawText(x + 2, tlY + 10, label);
        p.setPen(QColor(90, 90, 96));
        // Ticks menores
        const double subStep = tStep / 4.0;
        for (int s = 1; s < 4; ++s) {
            const int sx = 10 + (int)qRound((t + s * subStep) * pps);
            if (sx > rulerW - 5) break;
            p.drawLine(sx, tlY + 5, sx, tlY + 10);
        }
    }

    // ── Keyframe diamonds ──
    // Camera keyframes
    auto drawKfDiamonds = [&](const QVector<Keyframe>& vks, const QColor& col) {
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        for (const Keyframe& k : vks) {
            const int x = 10 + (int)qRound(k.time * pps);
            if (x < 5 || x > rulerW - 5) continue;
            const double sz = 4.0;
            const QPolygonF diamond = QPolygonF()
                << QPointF(x, midY - sz) << QPointF(x + sz, midY)
                << QPointF(x, midY + sz) << QPointF(x - sz, midY);
            p.drawPolygon(diamond);
        }
    };

    // Camera keyframes (cor ciano)
    drawKfDiamonds(mc->kfCamX, QColor(80, 200, 255));
    drawKfDiamonds(mc->kfCamY, QColor(80, 200, 255));
    drawKfDiamonds(mc->kfCamZoom, QColor(80, 200, 255));
    drawKfDiamonds(mc->kfCamRotation, QColor(80, 200, 255));

    // Track keyframes (cor da track ou amarela se agrupada)
    const QVector<Track*> tracks = mesaTracks();
    for (int i = 0; i < tracks.size(); ++i) {
        Track* t = tracks[i];
        QColor col;
        const TrackGroup* tg = t->groupId.isEmpty() ? nullptr : m_project->findGroup(t->groupId);
        if (tg && tg->mesaId == m_mesaId)
            col = QColor(220, 180, 60);  // amarelo Premier-style
        else
            col = QColor::fromHsv((i * 47 + 180) % 360, 60, 80);

        auto draw = [&](const QVector<Keyframe>& vks) {
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            for (const Keyframe& k : vks) {
                const int x = 10 + (int)qRound(k.time * pps);
                if (x < 5 || x > rulerW - 5) continue;
                const double sz = 3.5;
                const QPolygonF diamond = QPolygonF()
                    << QPointF(x, midY - sz) << QPointF(x + sz, midY)
                    << QPointF(x, midY + sz) << QPointF(x - sz, midY);
                p.drawPolygon(diamond);
            }
        };
        draw(t->kfMesaX); draw(t->kfMesaY);
        draw(t->kfMesaScaleX); draw(t->kfMesaScaleY);
        draw(t->kfMesaRotation); draw(t->kfMesaOpacity);
        draw(t->kfMesaAnchorX); draw(t->kfMesaAnchorY);
    }

    // ── Playhead (linha vertical) ──
    const int phX = 10 + (int)qRound(m_playheadTime * pps);
    if (phX >= 5 && phX <= rulerW - 5) {
        p.setPen(QPen(QColor(255, 80, 80), 2));
        p.drawLine(phX, tlY + 1, phX, tlY + tlH - 1);
        // Cabeça do playhead
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 80, 80));
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
        const bool sel = (i == m_selectedIdx);
        const LayerBounds lb = layerBounds(t, i);

        // Placeholder para layers sem conteúdo
        if (!lb.hasContent) {
            QPointF center, corners[4], rotH;
            layerScreenRect(lb, center, corners, rotH);

            p.save();
            p.translate(center);
            p.rotate(lb.rotation);
            const double hw = lb.w * m_zoom / 2;
            const double hh = lb.h * m_zoom / 2;

            // Cor: amarela se agrupada na Mesa, senão cor indexada
            QColor fill;
            const TrackGroup* tg = t->groupId.isEmpty() ? nullptr : m_project->findGroup(t->groupId);
            if (tg && tg->mesaId == m_mesaId) {
                fill = QColor::fromHsv(42, 50, 40, 120);  // amarelo Premier-style
            } else {
                fill = QColor::fromHsv((i * 47 + 180) % 360, 30, 35, 100);
            }
            p.setPen(sel ? QPen(QColor(255, 255, 255), 1.5) : QPen(fill.lighter(130), 1.0, Qt::DashLine));
            p.setBrush(fill);
            p.drawRect(-hw, -hh, hw * 2, hh * 2);

            QFont f = p.font();
            f.setPointSizeF(8);
            f.setBold(sel);
            p.setFont(f);
            p.setPen(sel ? QColor(255, 255, 255) : fill.lighter(180));
            p.drawText(QRectF(-hw, -hh, hw * 2, hh * 2), Qt::AlignCenter, t->name);

            p.restore();
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
        const double cXi = kfValue(mc->kfCamX, mc->camX, rel);
        const double cYi = kfValue(mc->kfCamY, mc->camY, rel);
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
        p.drawText(QRect(8, 0, width() - 16, hh), Qt::AlignLeft | Qt::AlignVCenter, label);
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
    m_offset = QPointF(0, 0);
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
