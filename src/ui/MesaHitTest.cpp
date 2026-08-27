// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaWidget.h"
#include <QPainter>
#include <QtMath>

// ═══════════════════════════════════════════════════════════════════════
// Coordenadas
// ═══════════════════════════════════════════════════════════════════════

QPointF MesaWidget::canvasToScreen(const QPointF& p) const {
    const double cx = width() / 2.0 + m_offset.x();
    const double cy = height() / 2.0 + m_offset.y();
    return QPointF(cx + p.x() * m_zoom, cy + p.y() * m_zoom);
}

QPointF MesaWidget::screenToCanvas(const QPointF& p) const {
    const double cx = width() / 2.0 + m_offset.x();
    const double cy = height() / 2.0 + m_offset.y();
    return QPointF((p.x() - cx) / m_zoom, (p.y() - cy) / m_zoom);
}

// ═══════════════════════════════════════════════════════════════════════
// Layer bounds
// ═══════════════════════════════════════════════════════════════════════

MesaWidget::LayerBounds MesaWidget::layerBounds(const Track* t, int trackIdx) const {
    LayerBounds lb;
    MesaComposition* mc = currentMesa();
    if (!t || !mc) return lb;

    const double rel = qMax(0.0, m_playheadTime);
    lb.x = kfValue(t->kfMesaX, t->mesaX, rel);
    lb.y = kfValue(t->kfMesaY, t->mesaY, rel);
    lb.rotation = kfValue(t->kfMesaRotation, t->mesaRotation, rel);
    lb.anchorX = kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel);
    lb.anchorY = kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel);

    double scX = kfValue(t->kfMesaScaleX, t->mesaScaleX, rel);
    double scY = t->kfMesaScaleY.isEmpty() ? scX : kfValue(t->kfMesaScaleY, t->mesaScaleY, rel);

    double frameW = mc->canvasW;
    double frameH = mc->canvasH;
    for (const Clip& c : t->clips) {
        const double cRel = rel - c.pos;
        if (cRel < 0 || cRel >= c.dur) continue;
        if (!c.isText && !c.mediaId.isEmpty()) {
            bool ok = false;
            double mw = 0, mh = 0;
            layerMediaSize(c, ok, mw, mh);
            if (ok && mw > 0 && mh > 0) {
                frameW = mw;
                frameH = mh;
                lb.hasContent = true;
            }
        } else if (c.isText) {
            frameW = mc->canvasW;
            frameH = mc->canvasH;
            lb.hasContent = true;
        }
        break;
    }

    lb.w = frameW * scX;
    lb.h = frameH * scY;
    return lb;
}

void MesaWidget::layerMediaSize(const Clip& c, bool& ok, double& w, double& h) const {
    ok = false; w = 0; h = 0;
    if (c.isText) { ok = true; return; }
    auto it = m_mediaSizes.constFind(c.mediaId);
    if (it != m_mediaSizes.constEnd()) {
        w = it->width(); h = it->height();
        ok = (w > 0 && h > 0);
        return;
    }
    if (!m_project) return;
    const MediaItem* mi = m_project->findMedia(c.mediaId);
    if (mi && mi->width > 0 && mi->height > 0) {
        m_mediaSizes.insert(c.mediaId, QSize(mi->width, mi->height));
        w = mi->width; h = mi->height;
        ok = true;
    }
}

void MesaWidget::layerScreenRect(const LayerBounds& lb, QPointF& center,
                                  QPointF corners[4], QPointF& rotateHandle) const {
    center = canvasToScreen(QPointF(lb.x, lb.y));
    const double hw = lb.w * m_zoom / 2.0;
    const double hh = lb.h * m_zoom / 2.0;
    const double rad = qDegreesToRadians(lb.rotation);
    const double cosR = qCos(rad);
    const double sinR = qSin(rad);

    auto rotPt = [&](double lx, double ly) -> QPointF {
        return center + QPointF(lx * cosR - ly * sinR, lx * sinR + ly * cosR);
    };

    corners[0] = rotPt(-hw, -hh);
    corners[1] = rotPt( hw, -hh);
    corners[2] = rotPt( hw,  hh);
    corners[3] = rotPt(-hw,  hh);

    rotateHandle = rotPt(0, -hh - 30.0 * m_zoom);
}

// ═══════════════════════════════════════════════════════════════════════
// Hit testing
// ═══════════════════════════════════════════════════════════════════════

static double distToSegment(const QPointF& p, const QPointF& a, const QPointF& b) {
    const double dx = b.x() - a.x();
    const double dy = b.y() - a.y();
    const double len2 = dx * dx + dy * dy;
    if (len2 < 1e-6) return QLineF(p, a).length();
    double t = qBound(0.0, ((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / len2, 1.0);
    return QLineF(p, QPointF(a.x() + t * dx, a.y() + t * dy)).length();
}

MesaWidget::HitZone MesaWidget::hitTest(const QPointF& sp, int& outTrackIdx) const {
    outTrackIdx = -1;
    MesaComposition* mc = currentMesa();
    if (!mc) return HitNone;

    const QVector<Track*> tracks = mesaTracks();
    const double handleRadius = 6.0;

    const double rel = qMax(0.0, m_playheadTime);
    const double cXi = kfValue(mc->kfCamX, mc->camX, rel);
    const double cYi = kfValue(mc->kfCamY, mc->camY, rel);
    const double cZi = kfValue(mc->kfCamZoom, mc->camZoom, rel);
    const double cRi = kfValue(mc->kfCamRotation, mc->camRotation, rel);
    const QPointF cc = canvasToScreen(QPointF(cXi, cYi));
    const double camHW = mc->canvasW / qMax(0.01, cZi) * m_zoom / 2;
    const double camHH = mc->canvasH / qMax(0.01, cZi) * m_zoom / 2;

    auto rotPt = [](const QPointF& center, double lx, double ly, double rot) -> QPointF {
        const double rad = qDegreesToRadians(rot);
        return center + QPointF(lx * qCos(rad) - ly * qSin(rad),
                                lx * qSin(rad) + ly * qCos(rad));
    };

    const QPointF camCorners[4] = {
        rotPt(cc,  camHW,  camHH, cRi), rotPt(cc, -camHW,  camHH, cRi),
        rotPt(cc,  camHW, -camHH, cRi), rotPt(cc, -camHW, -camHH, cRi)
    };
    for (int i = 0; i < 4; ++i) {
        if (QLineF(camCorners[i], sp).length() <= handleRadius + 2) {
            return HitCameraCorner;
        }
    }

    {
        const double rad = qDegreesToRadians(cRi);
        const double cosR = qCos(rad), sinR = qSin(rad);
        const QPointF d = sp - cc;
        const double lx =  d.x() * cosR + d.y() * sinR;
        const double ly = -d.x() * sinR + d.y() * cosR;
        if (qAbs(lx) <= camHW && qAbs(ly) <= camHH)
            return HitCamera;
    }

    for (int i = tracks.size() - 1; i >= 0; --i) {
        const Track* t = tracks[i];
        const LayerBounds lb = layerBounds(t, i);
        QPointF center, corners[4], rotateHandle;
        layerScreenRect(lb, center, corners, rotateHandle);

        if (QLineF(rotateHandle, sp).length() <= handleRadius + 2) {
            outTrackIdx = i;
            return HitRotate;
        }

        if (distToSegment(sp, center, rotateHandle) <= 4.0) {
            outTrackIdx = i;
            return HitRotate;
        }

        const struct { QPointF* corner; HitZone zone; } handleMap[] = {
            { &corners[0], HitCornerTL }, { &corners[1], HitCornerTR },
            { &corners[2], HitCornerBR }, { &corners[3], HitCornerBL }
        };
        for (auto& h : handleMap) {
            if (QLineF(*h.corner, sp).length() <= handleRadius) {
                outTrackIdx = i;
                return h.zone;
            }
        }

        for (int e = 0; e < 4; ++e) {
            const QPointF mid = (corners[e] + corners[(e + 1) % 4]) / 2.0;
            if (QLineF(mid, sp).length() <= handleRadius) {
                outTrackIdx = i;
                return (e == 0) ? HitEdgeT : (e == 1) ? HitEdgeR : (e == 2) ? HitEdgeB : HitEdgeL;
            }
        }

        {
            const double rad = qDegreesToRadians(-lb.rotation);
            const double cosR = qCos(rad), sinR = qSin(rad);
            const QPointF d = sp - center;
            const double lx =  d.x() * cosR + d.y() * sinR;
            const double ly = -d.x() * sinR + d.y() * cosR;
            const double hw = lb.w * m_zoom / 2;
            const double hh = lb.h * m_zoom / 2;
            if (qAbs(lx) <= hw && qAbs(ly) <= hh) {
                outTrackIdx = i;
                return HitBody;
            }
        }
    }

    return HitNone;
}

int MesaWidget::trackAt(const QPointF& sp) const {
    int idx = -1;
    HitZone hz = hitTest(sp, idx);
    if (hz == HitBody || hz == HitCornerTL || hz == HitCornerTR ||
        hz == HitCornerBL || hz == HitCornerBR || hz == HitEdgeT ||
        hz == HitEdgeB || hz == HitEdgeL || hz == HitEdgeR || hz == HitRotate)
        return idx;
    return -1;
}

void MesaWidget::cameraCornerPoints(QPointF out[4]) const {
    MesaComposition* mc = currentMesa();
    if (!mc) { out[0] = out[1] = out[2] = out[3] = {}; return; }
    const double rel = qMax(0.0, m_playheadTime);
    const double zi = kfValue(mc->kfCamZoom, mc->camZoom, rel);
    const double xi = kfValue(mc->kfCamX, mc->camX, rel);
    const double yi = kfValue(mc->kfCamY, mc->camY, rel);
    const double cw = mc->canvasW / qMax(0.01, zi);
    const double ch = mc->canvasH / qMax(0.01, zi);
    const QPointF ctr = canvasToScreen(QPointF(xi, yi));
    const double hw = cw * m_zoom / 2;
    const double hh = ch * m_zoom / 2;
    out[0] = ctr + QPointF( hw,  hh);
    out[1] = ctr + QPointF(-hw,  hh);
    out[2] = ctr + QPointF( hw, -hh);
    out[3] = ctr + QPointF(-hw, -hh);
}

int MesaWidget::cameraCornerAt(const QPointF& sp) const {
    QPointF c[4];
    cameraCornerPoints(c);
    for (int i = 0; i < 4; ++i) {
        if (QLineF(c[i], sp).length() <= 8.0) return i;
    }
    return -1;
}
