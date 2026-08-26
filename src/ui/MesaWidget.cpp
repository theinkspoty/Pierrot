// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaWidget.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QtMath>
#include <QCursor>

// ═══════════════════════════════════════════════════════════════════════
// Construtor
// ═══════════════════════════════════════════════════════════════════════

MesaWidget::MesaWidget(QWidget* parent)
    : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 150);
    setStyleSheet("background: #1a1a1a;");
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
    QVector<Track*> result;
    MesaComposition* mc = currentMesa();
    if (!mc || !m_project) return result;
    for (const QString& tid : mc->trackIds) {
        Track* t = findTrack(tid);
        if (t) result.append(t);
    }
    return result;
}

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
    double scY = kfValue(t->kfMesaScaleY, t->mesaScaleY, rel);

    // Tenta achar o clip ativo pra saber o tamanho real
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
    if (c.isText) { ok = true; return; } // texto usa canvas (resolvido no chamador)
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

    // Handle de rotação: 30px acima do centro superior
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

    // Verifica camera primeiro
    const double rel = qMax(0.0, m_playheadTime);
    const double cXi = kfValue(mc->kfCamX, mc->camX, rel);
    const double cYi = kfValue(mc->kfCamY, mc->camY, rel);
    const double cZi = kfValue(mc->kfCamZoom, mc->camZoom, rel);
    const double cRi = kfValue(mc->kfCamRotation, mc->camRotation, rel);
    const QPointF cc = canvasToScreen(QPointF(cXi, cYi));
    const double camHW = mc->canvasW / qMax(0.01, cZi) * m_zoom / 2;
    const double camHH = mc->canvasH / qMax(0.01, cZi) * m_zoom / 2;

    // Camera corners
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

    // Camera body (check with rotated rect)
    {
        const double rad = qDegreesToRadians(cRi);
        const double cosR = qCos(rad), sinR = qSin(rad);
        const QPointF d = sp - cc;
        const double lx =  d.x() * cosR + d.y() * sinR;
        const double ly = -d.x() * sinR + d.y() * cosR;
        if (qAbs(lx) <= camHW && qAbs(ly) <= camHH)
            return HitCamera;
    }

    // Verifica layers (de cima pra baixo)
    for (int i = tracks.size() - 1; i >= 0; --i) {
        const Track* t = tracks[i];
        const LayerBounds lb = layerBounds(t, i);
        QPointF center, corners[4], rotateHandle;
        layerScreenRect(lb, center, corners, rotateHandle);

        // Rotation handle
        if (QLineF(rotateHandle, sp).length() <= handleRadius + 2) {
            outTrackIdx = i;
            return HitRotate;
        }

        // Linha do rotation handle (center → rotateHandle)
        if (distToSegment(sp, center, rotateHandle) <= 4.0) {
            outTrackIdx = i;
            return HitRotate;
        }

        // Scale handles (cantos + bordas)
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

        // Edge handles (meio das bordas)
        for (int e = 0; e < 4; ++e) {
            const QPointF mid = (corners[e] + corners[(e + 1) % 4]) / 2.0;
            if (QLineF(mid, sp).length() <= handleRadius) {
                outTrackIdx = i;
                return (e == 0) ? HitEdgeT : (e == 1) ? HitEdgeR : (e == 2) ? HitEdgeB : HitEdgeL;
            }
        }

        // Corpo da layer (ponto dentro do retângulo rotacionado)
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
    // Camera
    upsert(mc->kfCamX, mc->camX);
    upsert(mc->kfCamY, mc->camY);
    upsert(mc->kfCamZoom, mc->camZoom);
    upsert(mc->kfCamRotation, mc->camRotation);
    // Tracks
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
// Paint
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(18, 18, 18));

    MesaComposition* mc = currentMesa();
    if (!mc) {
        p.setPen(QColor(80, 80, 80));
        p.setFont(QFont("DejaVu Sans", 10));
        p.drawText(rect().adjusted(0, 0, 0, -40), Qt::AlignCenter,
                   tr("Nenhuma Mesa selecionada\nClique direito na timeline → Criar Mesa"));

        const int bw = 160, bh = 36;
        const QRect btnRect(width() / 2 - bw / 2, height() / 2 + 20, bw, bh);
        m_createMesaBtnRect = btnRect;
        p.setPen(QPen(QColor(0, 180, 255), 1));
        p.setBrush(QColor(30, 30, 30));
        p.drawRoundedRect(btnRect, 4, 4);
        p.setPen(QColor(0, 180, 255));
        p.setFont(QFont("DejaVu Sans", 9));
        p.drawText(btnRect, Qt::AlignCenter, tr("Criar Mesa"));
        return;
    }

    const double rel = qMax(0.0, m_playheadTime);

    // ── Renderiza canvas via MesaRenderer ──
    const bool draggingLayer = (m_transformOp != TNone && m_dragTrackIndex >= 0
                                && !m_dragTrackId.isEmpty());
    if (draggingLayer) {
        // Cache base = composição sem a camada arrastada. Recomposto uma vez
        // (ou quando o tempo muda); durante o drag a camada é desenhada por
        // cima direto no painter do widget (sem alocar um canvas inteiro).
        if (m_baseCache.isNull() || !qFuzzyCompare(m_baseCacheTime, rel)
            || m_baseCacheSkipId != m_dragTrackId) {
            m_baseCache = m_renderer.renderCanvas(*mc, *m_project, rel, &m_dragTrackId);
            m_baseCacheTime = rel;
            m_baseCacheSkipId = m_dragTrackId;
        }
        m_canvasCache = QImage(); // força desenhar base + camada no paint
        Track* dragTrack = findTrack(m_dragTrackId);
        m_dragPrepValid = (dragTrack != nullptr)
                          && m_renderer.prepareLayer(m_dragPrep, *mc, *m_project,
                                                     rel, *dragTrack);
    } else if (m_canvasCache.isNull() || !qFuzzyCompare(m_canvasCacheTime, rel)) {
        m_canvasCache = m_renderer.renderCanvas(*mc, *m_project, rel);
        m_canvasCacheTime = rel;
        m_dragPrepValid = false;
    }

    // ── Workspace infinito ──
    // Fundo: preenche o widget inteiro
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(30, 30, 30));
    p.drawRect(rect());

    // Grid infinito (se extensão do widget em canvas-space)
    const double gridStep = qMax(10.0, kGridSize * m_zoom);
    if (gridStep >= 6) {
        const QPointF tl = screenToCanvas(QPointF(0, 0));
        const QPointF br = screenToCanvas(QPointF(width(), height()));
        const double gx0 = qFloor(tl.x() / kGridSize) * kGridSize;
        const double gy0 = qFloor(tl.y() / kGridSize) * kGridSize;
        p.setPen(QPen(QColor(55, 55, 55), 1));
        for (double gx = gx0; gx <= br.x(); gx += kGridSize) {
            const double sx = canvasToScreen(QPointF(gx, 0)).x();
            p.drawLine(QPointF(sx, 0), QPointF(sx, height()));
        }
        for (double gy = gy0; gy <= br.y(); gy += kGridSize) {
            const double sy = canvasToScreen(QPointF(0, gy)).y();
            p.drawLine(QPointF(0, sy), QPointF(width(), sy));
        }
    }

    // ── Frame de composição (retângulo branco = output) ──
    const QPointF canvasOrigin = canvasToScreen(QPointF(-mc->canvasW / 2.0, -mc->canvasH / 2.0));
    const QRectF canvasR(canvasOrigin, QSizeF(mc->canvasW * m_zoom, mc->canvasH * m_zoom));

    // Fundo preto do output
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0));
    p.drawRect(canvasR);

    // Frame renderizado dentro do output
    {
        p.save();
        p.setClipRect(canvasR);
        // Desenha a imagem do canvas escalada no retângulo de saída.
        if (!m_canvasCache.isNull())
            p.drawImage(canvasR, m_canvasCache);
        else if (!m_baseCache.isNull()) {
            p.drawImage(canvasR, m_baseCache);
            // Camada arrastada: desenha em espaço de canvas (via transform),
            // sem recompor a composição inteira.
            if (m_dragPrepValid) {
                p.save();
                p.translate(canvasR.topLeft());
                p.scale(m_zoom, m_zoom);
                m_renderer.drawTrackImage(p, m_dragPrep);
                p.restore();
            }
        }
        p.restore();
    }

    // Borda branca do output (como AE)
    p.setPen(QPen(QColor(200, 200, 200), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(canvasR);

    // ── Layers (handles de seleção) ──
    const QVector<Track*> tracks = mesaTracks();
    for (int i = 0; i < tracks.size(); ++i) {
        const Track* t = tracks[i];
        const bool sel = (i == m_selectedIdx);
        const LayerBounds lb = layerBounds(t, i);

        // Se não tem conteúdo, desenha placeholder (borda tracejada)
        if (!lb.hasContent) {
            QPointF center, corners[4], rotH;
            layerScreenRect(lb, center, corners, rotH);

            p.save();
            p.translate(center);
            p.rotate(lb.rotation);
            const double hw = lb.w * m_zoom / 2;
            const double hh = lb.h * m_zoom / 2;

            // Fundo semi-transparente
            const int hue = (i * 47 + 180) % 360;
            QColor fill = QColor::fromHsv(hue, 40, 30, 80);
            p.setPen(sel ? QPen(QColor(0, 180, 255), 2) : QPen(fill.lighter(130), 1, Qt::DashLine));
            p.setBrush(fill);
            p.drawRect(-hw, -hh, hw * 2, hh * 2);

            // Label
            QFont f = p.font();
            f.setPointSizeF(8);
            f.setBold(sel);
            p.setFont(f);
            p.setPen(sel ? QColor(255, 255, 255) : fill.lighter(180));
            p.drawText(QRectF(-hw, -hh, hw * 2, hh * 2), Qt::AlignCenter, t->name);

            p.restore();
        }

        if (!sel) continue;

        // ── Handles de seleção (AE-style) ──
        QPointF center, corners[4], rotateHandle;
        layerScreenRect(lb, center, corners, rotateHandle);

        // Linha do rotation handle
        p.setPen(QPen(QColor(0, 180, 255), 1));
        p.drawLine(center, rotateHandle);

        // Rotation handle (círculo)
        p.setPen(QPen(QColor(0, 180, 255), 1.5));
        p.setBrush(QColor(30, 30, 30));
        p.drawEllipse(rotateHandle, 5, 5);

        // Borda tracejada
        p.setPen(QPen(QColor(0, 180, 255), 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawLine(corners[0], corners[1]);
        p.drawLine(corners[1], corners[2]);
        p.drawLine(corners[2], corners[3]);
        p.drawLine(corners[3], corners[0]);

        // Handles de escala (cantos)
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 180, 255));
        const double hs = 4.0;
        for (int j = 0; j < 4; ++j)
            p.drawRect(QRectF(corners[j].x() - hs, corners[j].y() - hs, hs * 2, hs * 2));

        // Handles de escala (bordas)
        p.setBrush(QColor(0, 140, 220));
        const double hsm = 3.0;
        for (int e = 0; e < 4; ++e) {
            const QPointF mid = (corners[e] + corners[(e + 1) % 4]) / 2.0;
            p.drawRect(QRectF(mid.x() - hsm, mid.y() - hsm, hsm * 2, hsm * 2));
        }

        // Anchor point (crosshair)
        const QPointF anchor = canvasToScreen(QPointF(lb.anchorX + lb.x, lb.anchorY + lb.y));
        p.setPen(QPen(QColor(255, 80, 80), 1.5));
        p.drawLine(anchor + QPointF(-6, 0), anchor + QPointF(6, 0));
        p.drawLine(anchor + QPointF(0, -6), anchor + QPointF(0, 6));
    }

    // ── Câmera ──
    const double cXi = kfValue(mc->kfCamX, mc->camX, rel);
    const double cYi = kfValue(mc->kfCamY, mc->camY, rel);
    const double cZi = kfValue(mc->kfCamZoom, mc->camZoom, rel);
    const double cRi = kfValue(mc->kfCamRotation, mc->camRotation, rel);
    const double camW = mc->canvasW / qMax(0.01, cZi);
    const double camH = mc->canvasH / qMax(0.01, cZi);
    const QPointF cc = canvasToScreen(QPointF(cXi, cYi));

    p.save();
    p.translate(cc);
    p.rotate(cRi);
    const double hw = camW * m_zoom / 2;
    const double hh = camH * m_zoom / 2;

    // Preenchimento semi-transparente (diferencia do output)
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 18));
    p.drawRect(-hw, -hh, hw * 2, hh * 2);

    // Borda tracejada branca
    p.setPen(QPen(QColor(255, 255, 255), 1.5, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(-hw, -hh, hw * 2, hh * 2);

    // Label
    QFont cf = p.font();
    cf.setPointSizeF(8);
    cf.setBold(true);
    p.setFont(cf);
    p.setPen(QColor(255, 255, 255));
    p.drawText(QRectF(-hw + 3, -hh + 2, hw * 2, 14), Qt::AlignTop | Qt::AlignLeft,
               QStringLiteral("CAM"));

    // Handles maiores
    const double hsz = 7.0;
    p.setPen(QPen(QColor(255, 255, 255), 1.5));
    p.setBrush(QColor(40, 40, 40));
    const QPointF chits[4] = {
        { hw,  hh}, {-hw,  hh}, { hw, -hh}, {-hw, -hh}
    };
    for (int i = 0; i < 4; ++i)
        p.drawRect(QRectF(chits[i].x() - hsz, chits[i].y() - hsz, hsz * 2, hsz * 2));

    p.restore();

    // ── Layer list flutuante ──
    if (m_showLayerList) drawLayerList(p);

    // ── Painel de propriedades ──
    drawPropertyPanel(p);

    // ── Info bar ──
    QFont infof = p.font();
    infof.setPointSizeF(7);
    p.setFont(infof);
    p.setPen(QColor(60, 60, 60));
    const int infoY = height() - propPanelHeight() - 18;
    p.drawText(QRect(6, infoY, width() - 12, 16), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Z: %1%  |  %2×%3  |  %4 layers  |  G: snap %5  |  L: layers")
                   .arg((int)(m_zoom * 100)).arg(mc->canvasW).arg(mc->canvasH)
                   .arg(tracks.size())
                   .arg(m_snapToGrid ? "ON" : "OFF"));
}

void MesaWidget::drawLayerList(QPainter& p) {
    MesaComposition* mc = currentMesa();
    if (!mc) return;

    const QVector<Track*> tracks = mesaTracks();
    const int rowH = 24;
    const int headerH = 26;
    const int listW = 160;
    const int listH = headerH + tracks.size() * rowH + 4;

    m_layerListRect = QRect(8, height() - listH - 28 - propPanelHeight(), listW, listH);

    p.setPen(QPen(QColor(40, 40, 40), 1));
    p.setBrush(QColor(22, 22, 22, 230));
    p.drawRoundedRect(m_layerListRect, 4, 4);

    QFont hf = p.font();
    hf.setPointSizeF(8);
    hf.setBold(true);
    p.setFont(hf);
    p.setPen(QColor(140, 140, 140));
    p.drawText(m_layerListRect.adjusted(8, 4, -8, 0), Qt::AlignLeft | Qt::AlignTop,
               QStringLiteral("LAYERS"));

    QFont rf = p.font();
    rf.setPointSizeF(8);
    rf.setBold(false);
    p.setFont(rf);

    for (int i = 0; i < tracks.size(); ++i) {
        const int rowIdx = tracks.size() - 1 - i;
        const Track* t = tracks[rowIdx];
        const bool sel = (rowIdx == m_selectedIdx);
        const int y = m_layerListRect.top() + headerH + i * rowH;

        if (sel)
            p.fillRect(m_layerListRect.left() + 1, y, listW - 2, rowH, QColor(40, 70, 120));
        else if (rowIdx % 2 == 0)
            p.fillRect(m_layerListRect.left() + 1, y, listW - 2, rowH, QColor(28, 28, 28));

        const int hue = (rowIdx * 47 + 180) % 360;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor::fromHsv(hue, 60, 50));
        p.drawRoundedRect(m_layerListRect.left() + 8, y + 6, 12, 12, 2, 2);

        p.setPen(sel ? QColor(220, 220, 220) : QColor(160, 160, 160));
        p.drawText(QRect(m_layerListRect.left() + 24, y, listW - 32, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, t->name.left(12));
    }
}

void MesaWidget::drawPropertyPanel(QPainter& p) {
    const int ph = propPanelHeight();
    const int w = width();
    const int y = height() - ph;

    p.fillRect(0, y, w, ph, QColor(26, 26, 26));
    p.setPen(QColor(50, 50, 50));
    p.drawLine(0, y, w, y);

    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);

    const QVector<Track*> tracks = mesaTracks();
    const bool hasSel = m_selectedIdx >= 0 && m_selectedIdx < tracks.size();

    auto drawField = [&](int x, const QString& label, const QString& value, int fieldW = 70) {
        p.setPen(QColor(120, 120, 120));
        p.drawText(QRect(x, y + 2, 40, 14), Qt::AlignLeft | Qt::AlignVCenter, label);
        p.fillRect(x + 40, y + 4, fieldW, 24, QColor(38, 38, 38));
        p.setPen(QColor(200, 200, 200));
        p.drawText(QRect(x + 42, y + 4, fieldW - 4, 24), Qt::AlignLeft | Qt::AlignVCenter, value);
    };

    if (hasSel) {
        Track* t = tracks[m_selectedIdx];
        const double rel = qMax(0.0, m_playheadTime);
        const double tx = kfValue(t->kfMesaX, t->mesaX, rel);
        const double ty = kfValue(t->kfMesaY, t->mesaY, rel);
        const double tsx = kfValue(t->kfMesaScaleX, t->mesaScaleX, rel);
        const double tsy = kfValue(t->kfMesaScaleY, t->mesaScaleY, rel);
        const double tr = kfValue(t->kfMesaRotation, t->mesaRotation, rel);
        const double to = kfValue(t->kfMesaOpacity, t->mesaOpacity, rel);

        p.setPen(QColor(0, 180, 255));
        p.drawText(QRect(8, y + 2, 120, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   t->name.left(16));
        p.setPen(QColor(80, 80, 80));
        p.drawLine(130, y + 6, 130, y + ph - 6);

        drawField(140, "X:", QString::number(tx, 'f', 1));
        drawField(270, "Y:", QString::number(ty, 'f', 1));
        drawField(400, "SX:", QString::number(tsx, 'f', 2));
        drawField(510, "SY:", QString::number(tsy, 'f', 2));
        drawField(630, "R:", QString::number(tr, 'f', 1));
        drawField(740, "O:", QString::number(to, 'f', 2));
    } else if (MesaComposition* mc = currentMesa()) {
        const double rel = qMax(0.0, m_playheadTime);
        const double cx = kfValue(mc->kfCamX, mc->camX, rel);
        const double cy = kfValue(mc->kfCamY, mc->camY, rel);
        const double cz = kfValue(mc->kfCamZoom, mc->camZoom, rel);
        const double cr = kfValue(mc->kfCamRotation, mc->camRotation, rel);

        p.setPen(QColor(0, 200, 100));
        p.drawText(QRect(8, y + 2, 120, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Câmera"));
        p.setPen(QColor(80, 80, 80));
        p.drawLine(130, y + 6, 130, y + ph - 6);

        drawField(140, "X:", QString::number(cx, 'f', 1));
        drawField(270, "Y:", QString::number(cy, 'f', 1));
        drawField(400, "Z:", QString::number(cz, 'f', 2));
        drawField(530, "R:", QString::number(cr, 'f', 1));
    } else {
        p.setPen(QColor(80, 80, 80));
        p.drawText(QRect(8, y + 2, w - 16, ph - 4),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Nenhuma Mesa selecionada"));
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Mouse
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::mousePressEvent(QMouseEvent* e) {
    MesaComposition* mc = currentMesa();
    if (!mc) {
        if (e->button() == Qt::LeftButton && m_createMesaBtnRect.contains(e->pos()))
            emit mesaCreateRequested();
        return;
    }

    // Botão do meio → pan
    if (e->button() == Qt::MiddleButton) {
        m_draggingCanvas = true;
        m_canvasDragStart = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (e->button() != Qt::LeftButton) return;

    // Layer list flutuante
    if (m_showLayerList && m_layerListRect.contains(e->pos())) {
        const QVector<Track*> tracks = mesaTracks();
        const int headerH = 26;
        const int rowH = 24;
        const int relY = e->pos().y() - m_layerListRect.top() - headerH;
        if (relY >= 0) {
            const int row = relY / rowH;
            if (row >= 0 && row < tracks.size()) {
                const int idx = tracks.size() - 1 - row;
                m_selectedIdx = (m_selectedIdx == idx) ? -1 : idx;
                m_canvasCache = QImage();
                update();
                return;
            }
        }
        m_selectedIdx = -1;
        m_canvasCache = QImage();
        update();
        return;
    }

    // Hit test geral
    int hitIdx = -1;
    HitZone hz = hitTest(e->position(), hitIdx);

    // Camera corner → resize
    if (hz == HitCameraCorner) {
        m_resizingCamera = true;
        m_resizeCorner = cameraCornerAt(e->position());
        const double relC = qMax(0.0, m_playheadTime);
        m_resizeStartZoom = kfValue(mc->kfCamZoom, mc->camZoom, relC);
        m_resizeStartPos = e->position();
        return;
    }

    // Camera body → mover
    if (hz == HitCamera) {
        const double rel = qMax(0.0, m_playheadTime);
        m_draggingCamera = true;
        m_cameraDragStart = e->position();
        m_camDragStartX = kfValue(mc->kfCamX, mc->camX, rel);
        m_camDragStartY = kfValue(mc->kfCamY, mc->camY, rel);
        return;
    }

    // Layer hit
    if (hitIdx >= 0 && (hz == HitBody || hz == HitCornerTL || hz == HitCornerTR ||
        hz == HitCornerBL || hz == HitCornerBR || hz == HitEdgeT ||
        hz == HitEdgeB || hz == HitEdgeL || hz == HitEdgeR || hz == HitRotate)) {

        m_selectedIdx = hitIdx;
        m_canvasCache = QImage();

        QVector<Track*> tracks = mesaTracks();
        Track* t = tracks[hitIdx];
        const double rel = qMax(0.0, m_playheadTime);

        m_baseCache = QImage();
        m_baseCacheTime = -1.0;
        m_baseCacheSkipId.clear();
        m_dragTrackId = t->id;
        m_dragTrackIndex = hitIdx;

        m_transformTrackIdx = hitIdx;
        m_transformStart = e->position();
        m_transformStartX = t->mesaX;
        m_transformStartY = t->mesaY;
        m_transformStartSX = t->mesaScaleX;
        m_transformStartSY = t->mesaScaleY;
        m_transformStartRot = t->mesaRotation;

        if (hz == HitBody) {
            m_transformOp = TMove;
            setCursor(Qt::SizeAllCursor);
        } else if (hz == HitRotate) {
            m_transformOp = TRotate;
            const LayerBounds lb = layerBounds(t, hitIdx);
            const QPointF anchor = canvasToScreen(QPointF(
                kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel) + lb.x,
                kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel) + lb.y));
            m_transformStartAngle = qAtan2(e->position().y() - anchor.y(),
                                            e->position().x() - anchor.x());
            setCursor(Qt::CrossCursor);
        } else {
            m_transformOp = TScale;
            m_scaleUniform = (e->modifiers() & Qt::ShiftModifier);
            const LayerBounds lb = layerBounds(t, hitIdx);
            const QPointF anchor = canvasToScreen(QPointF(
                kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel) + lb.x,
                kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel) + lb.y));
            m_transformStartDist = QLineF(anchor, e->position()).length();
            setCursor(Qt::SizeFDiagCursor);
        }

        update();
        return;
    }

    // Nada clicado → desselecionar
    m_selectedIdx = -1;
    m_canvasCache = QImage();
    update();
}

void MesaWidget::mouseMoveEvent(QMouseEvent* e) {
    MesaComposition* mc = currentMesa();
    auto snap = [&](double v) {
        return m_snapToGrid ? qRound(v / kGridSize) * kGridSize : v;
    };

    if (m_draggingCanvas) {
        m_offset += e->position() - m_canvasDragStart;
        m_canvasDragStart = e->position();
        update();
        return;
    }

    if (m_resizingCamera && mc) {
        const QPointF d = e->position() - m_resizeStartPos;
        const double delta = (d.x() + d.y()) / 2.0; // arrasto diagonal
        mc->camZoom = qBound(0.05, m_resizeStartZoom * qPow(1.008, delta), 20.0);
        update();
        return;
    }

    if (m_draggingCamera && mc) {
        const QPointF d = e->position() - m_cameraDragStart;
        mc->camX = m_camDragStartX + d.x() / m_zoom;
        mc->camY = m_camDragStartY + d.y() / m_zoom;
        update();
        return;
    }

    if (m_transformOp != TNone && mc && m_transformTrackIdx >= 0) {
        QVector<Track*> tracks = mesaTracks();
        if (m_transformTrackIdx < tracks.size()) {
            Track* t = tracks[m_transformTrackIdx];
            const double rel = qMax(0.0, m_playheadTime);

            if (m_transformOp == TMove) {
                const QPointF d = e->position() - m_transformStart;
                t->mesaX = snap(m_transformStartX + d.x() / m_zoom);
                t->mesaY = snap(m_transformStartY + d.y() / m_zoom);

            } else if (m_transformOp == TScale) {
                const LayerBounds lb = layerBounds(t, m_transformTrackIdx);
                const QPointF anchor = canvasToScreen(QPointF(
                    kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel) + lb.x,
                    kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel) + lb.y));
                const double dist = QLineF(anchor, e->position()).length();
                if (m_transformStartDist > 1.0) {
                    const double factor = dist / m_transformStartDist;
                    if (m_scaleUniform || (e->modifiers() & Qt::ShiftModifier)) {
                        t->mesaScaleX = qMax(0.01, m_transformStartSX * factor);
                        t->mesaScaleY = t->mesaScaleX;
                    } else {
                        // Escala baseada na direção do handle
                        const QPointF d = e->position() - m_transformStart;
                        const double sx = m_transformStartSX * (1.0 + d.x() / 200.0);
                        const double sy = m_transformStartSY * (1.0 + d.y() / 200.0);
                        t->mesaScaleX = qMax(0.01, sx);
                        t->mesaScaleY = qMax(0.01, sy);
                    }
                }

            } else if (m_transformOp == TRotate) {
                const LayerBounds lb = layerBounds(t, m_transformTrackIdx);
                const QPointF anchor = canvasToScreen(QPointF(
                    kfValue(t->kfMesaAnchorX, t->mesaAnchorX, rel) + lb.x,
                    kfValue(t->kfMesaAnchorY, t->mesaAnchorY, rel) + lb.y));
                const double angle = qAtan2(e->position().y() - anchor.y(),
                                            e->position().x() - anchor.x());
                double delta = qRadiansToDegrees(angle - m_transformStartAngle);
                if (e->modifiers() & Qt::ControlModifier) {
                    delta = qRound(delta / 15.0) * 15.0; // snap 15°
                }
                t->mesaRotation = m_transformStartRot + delta;
            }

            update();
        }
        return;
    }

    // Cursor feedback
    int hitIdx = -1;
    HitZone hz = hitTest(e->position(), hitIdx);
    switch (hz) {
        case HitCornerTL: case HitCornerBR: setCursor(Qt::SizeFDiagCursor); break;
        case HitCornerTR: case HitCornerBL: setCursor(Qt::SizeBDiagCursor); break;
        case HitEdgeT: case HitEdgeB: setCursor(Qt::SizeVerCursor); break;
        case HitEdgeL: case HitEdgeR: setCursor(Qt::SizeHorCursor); break;
        case HitRotate: setCursor(Qt::CrossCursor); break;
        case HitBody: setCursor(Qt::SizeAllCursor); break;
        case HitCamera: setCursor(Qt::SizeAllCursor); break;
        case HitCameraCorner: setCursor(Qt::SizeFDiagCursor); break;
        default: setCursor(Qt::ArrowCursor); break;
    }
}

void MesaWidget::mouseReleaseEvent(QMouseEvent*) {
    const bool wasTransforming = (m_transformOp != TNone);
    const bool camChanged = m_draggingCamera || m_resizingCamera;

    if (wasTransforming)
        writeAllKeyframes();
    if (camChanged)
        ensureKeyframesAt(m_playheadTime);

    m_transformOp = TNone;
    m_transformTrackIdx = -1;
    m_draggingCamera = false;
    m_resizingCamera = false;
    m_resizeCorner = -1;
    m_draggingCanvas = false;
    m_dragTrackId.clear();
    m_dragTrackIndex = -1;
    m_baseCache = QImage();
    m_baseCacheTime = -1.0;
    m_baseCacheSkipId.clear();
    m_dragPrepValid = false;
    setCursor(Qt::ArrowCursor);

    if (wasTransforming || camChanged)
        emit changesCommitted();

    // Força re-render do canvas completo no próximo paint (a base cache do
    // drag foi usada apenas para exibição em tempo real).
    m_canvasCacheTime = -1.0;
}

void MesaWidget::wheelEvent(QWheelEvent* e) {
    const double factor = e->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
    m_zoom = qBound(0.02, m_zoom * factor, 20.0);
    m_canvasCache = QImage();
    update();
}

void MesaWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_L) {
        m_showLayerList = !m_showLayerList;
        update();
    } else if (e->key() == Qt::Key_Delete && m_selectedIdx >= 0) {
        MesaComposition* mc = currentMesa();
        if (mc && m_selectedIdx < mc->trackIds.size()) {
            mc->trackIds.remove(m_selectedIdx);
            m_selectedIdx = -1;
            m_canvasCache = QImage();
            emit modified();
            update();
        }
    } else if (e->key() == Qt::Key_F) {
        m_zoom = 1.0;
        m_offset = {0, 0};
        m_canvasCache = QImage();
        update();
    } else if (e->key() == Qt::Key_G) {
        m_snapToGrid = !m_snapToGrid;
        update();
    } else {
        QWidget::keyPressEvent(e);
    }
}

void MesaWidget::resizeEvent(QResizeEvent*) {
    update();
}

// ═══════════════════════════════════════════════════════════════════════
// Interface pública
// ═══════════════════════════════════════════════════════════════════════

void MesaWidget::setMesaId(const QString& id) {
    m_mesaId = id;
    m_selectedIdx = -1;
    m_canvasCache = QImage();
    m_canvasCacheTime = -1.0;
    m_baseCache = QImage();
    m_baseCacheTime = -1.0;
    m_baseCacheSkipId.clear();
    m_transformOp = TNone;
    update();
}

void MesaWidget::refresh() {
    m_canvasCache = QImage();
    m_canvasCacheTime = -1.0;
    m_baseCache = QImage();
    m_baseCacheTime = -1.0;
    m_baseCacheSkipId.clear();
    m_dragPrepValid = false;
    m_mediaSizes.clear();
    update();
}

void MesaWidget::autoSelectMesa() {
    if (!m_project) return;
    // Se já tem uma Mesa selecionada que ainda existe (e tem track), mantém
    if (!m_mesaId.isEmpty() && m_project->findMesa(m_mesaId)) {
        m_canvasCache = QImage();
        update();
        return;
    }
    // Seleciona a primeira Mesa com pelo menos uma track existente
    for (const MesaComposition& m : m_project->mesas) {
        for (const QString& tid : m.trackIds) {
            if (findTrack(tid)) {
                m_mesaId = m.id;
                m_selectedIdx = -1;
                m_canvasCache = QImage();
                m_canvasCacheTime = -1.0;
                update();
                return;
            }
        }
    }
    // Nenhuma Mesa utilizável
    m_mesaId.clear();
    m_canvasCache = QImage();
    update();
}
