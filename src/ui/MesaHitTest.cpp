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
    // Fonte de verdade visual = a MESMA transform de drawTrackImage
    // (MesaRenderer): a layer gira/escala em torno do ponto do âncora
    // (pivot + anchor), NÃO do centro do retângulo. Qualquer mudança aqui
    // precisa continuar espelhando o render.
    center = canvasToScreen(QPointF(lb.x, lb.y));
    const QPointF anchorPt = canvasToScreen(QPointF(lb.x + lb.anchorX,
                                                    lb.y + lb.anchorY));
    const double rad = qDegreesToRadians(lb.rotation);
    const double cosR = qCos(rad);
    const double sinR = qSin(rad);
    const double zoom = m_zoom;

    // (lx, ly) em canvas-space relativo ao pivot. O quad local é rotacionado
    // em torno de (anchorX, anchorY) exatamente como o render desenha a imagem.
    auto rotPt = [&](double lx, double ly) -> QPointF {
        const double vx = (lx - lb.anchorX) * cosR - (ly - lb.anchorY) * sinR;
        const double vy = (lx - lb.anchorX) * sinR + (ly - lb.anchorY) * cosR;
        return anchorPt + QPointF(vx, vy) * zoom;
    };

    corners[0] = rotPt(-lb.w / 2.0, -lb.h / 2.0);
    corners[1] = rotPt( lb.w / 2.0, -lb.h / 2.0);
    corners[2] = rotPt( lb.w / 2.0,  lb.h / 2.0);
    corners[3] = rotPt(-lb.w / 2.0,  lb.h / 2.0);

    rotateHandle = rotPt(0.0, -lb.h / 2.0 - 30.0);
}

// ═══════════════════════════════════════════════════════════════════════
// Hit testing
// ═══════════════════════════════════════════════════════════════════════

static bool pointInPolygon(const QPointF& p, const QPointF poly[4]) {
    bool inside = false;
    for (int i = 0, j = 3; i < 4; j = i++) {
        const QPointF& a = poly[j];
        const QPointF& b = poly[i];
        if (((b.y() > p.y()) != (a.y() > p.y()))
            && (p.x() < (a.x() - b.x()) * (p.y() - b.y()) / (a.y() - b.y()) + b.x()))
            inside = !inside;
    }
    return inside;
}

// Prioridade de hit (cônica, resolve o "câmera come o clique da layer"):
//   1. camadas COM conteúdo ativo (do topo para o fundo);
//   2. câmera (cantos redimensionam, corpo move — só onde nenhuma layer cobre);
//   3. placeholders de camadas sem conteúdo (o quad vazio visível no canvas).
// A rotação NÃO pode ser acionada por uma linha invisível: só o handle.
MesaWidget::HitZone MesaWidget::hitTest(const QPointF& sp, int& outTrackIdx) const {
    outTrackIdx = -1;
    MesaComposition* mc = currentMesa();
    if (!mc) return HitNone;

    const QVector<Track*> tracks = mesaTracks();
    const double handleRadius = 6.0;

    auto layerHit = [&](const LayerBounds& lb) -> HitZone {
        QPointF center, corners[4], rotateHandle;
        layerScreenRect(lb, center, corners, rotateHandle);

        if (QLineF(rotateHandle, sp).length() <= handleRadius + 2)
            return HitRotate;

        const struct { const QPointF* corner; HitZone zone; } handleMap[] = {
            { &corners[0], HitCornerTL }, { &corners[1], HitCornerTR },
            { &corners[2], HitCornerBR }, { &corners[3], HitCornerBL }
        };
        for (auto& h : handleMap)
            if (QLineF(*h.corner, sp).length() <= handleRadius)
                return h.zone;

        for (int e = 0; e < 4; ++e) {
            const QPointF mid = (corners[e] + corners[(e + 1) % 4]) / 2.0;
            if (QLineF(mid, sp).length() <= handleRadius)
                return (e == 0) ? HitEdgeT : (e == 1) ? HitEdgeR
                     : (e == 2) ? HitEdgeB : HitEdgeL;
        }

        if (pointInPolygon(sp, corners))
            return HitBody;

        return HitNone;
    };

    // 1) Camadas com conteúdo (topo → fundo)
    // Oculta (olho off) ou trancada (cadeado) não participa do hit test.
    for (int i = tracks.size() - 1; i >= 0; --i) {
        const Track* tr = tracks[i];
        if (tr->mesaHidden || tr->mesaLocked) continue;
        const LayerBounds lb = layerBounds(tr, i);
        if (lb.hasContent) {
            const HitZone z = layerHit(lb);
            if (z != HitNone) { outTrackIdx = i; return z; }
        }
    }

    // 2) Câmera
    {
        const double rel = qMax(0.0, m_playheadTime);
        const double cXi = kfValue(mc->kfCamX, mc->camX, rel);
        const double cYi = kfValue(mc->kfCamY, mc->camY, rel);
        const double cZi = kfValue(mc->kfCamZoom, mc->camZoom, rel);
        const double cRi = kfValue(mc->kfCamRotation, mc->camRotation, rel);
        const QPointF cc = canvasToScreen(QPointF(cXi, cYi));
        const double camHW = mc->canvasW / qMax(0.01, cZi) * m_zoom / 2;
        const double camHH = mc->canvasH / qMax(0.01, cZi) * m_zoom / 2;

        auto camRot = [](const QPointF& center, double lx, double ly, double rot) -> QPointF {
            const double rad = qDegreesToRadians(rot);
            return center + QPointF(lx * qCos(rad) - ly * qSin(rad),
                                    lx * qSin(rad) + ly * qCos(rad));
        };
        const QPointF camCorners[4] = {
            camRot(cc,  camHW,  camHH, cRi), camRot(cc, -camHW,  camHH, cRi),
            camRot(cc,  camHW, -camHH, cRi), camRot(cc, -camHW, -camHH, cRi)
        };
        for (int i = 0; i < 4; ++i)
            if (QLineF(camCorners[i], sp).length() <= handleRadius + 2)
                return HitCameraCorner;

        {
            const double rad = qDegreesToRadians(cRi);
            const double cosR = qCos(rad), sinR = qSin(rad);
            const QPointF d = sp - cc;
            const double lx =  d.x() * cosR + d.y() * sinR;
            const double ly = -d.x() * sinR + d.y() * cosR;
            if (qAbs(lx) <= camHW && qAbs(ly) <= camHH)
                return HitCamera;
        }
    }

    // 3) Placeholders (camadas sem conteúdo ativo no playhead)
    for (int i = tracks.size() - 1; i >= 0; --i) {
        const Track* tr = tracks[i];
        if (tr->mesaHidden || tr->mesaLocked) continue;
        const LayerBounds lb = layerBounds(tr, i);
        if (!lb.hasContent) {
            const HitZone z = layerHit(lb);
            if (z != HitNone) { outTrackIdx = i; return z; }
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
