// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/monitor/PreviewWidget.h"
#include <cstdint>

#include "ui/dialogs/SettingsDialog.h"
#include "ui/timeline/TlLog.h"
#include "colombina/CrashReporter.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/ffmpeg/AudioConformCache.h"
#include "colombina/audio/EqBiquad.h"
#include "colombina/ofx/OfxRenderer.h"
#include "colombina/ofx/OfxPluginManager.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "colombina/fx/ColorGrade.h"
#include "ui/monitor/ScopeWidget.h"
#include "colombina/export/LainkaFx.h"
#include "ui/core/Theme.h"
#include "ui/monitor/MonitorUtils.h"
#include "colombina/generators.h"
#include "colombina/render/TextRender.h"
// DSP dos efeitos de áudio (AudioFx/GraphicEq/ParametricEqDsp) e os helpers
// de diagnóstico/config do preview (playDbg/audioDbg/composeDbg/
// previewAudioFormat/desengasgaIntervalMs) — vidrados em
// headers próprios, compartilhados com FrameWorker/FramePipeline/AudioMixer.
#include "ui/audio/AudioFxDsp.h"
#include "ui/audio/AudioMixer.h"
#include "ui/monitor/PreviewDebug.h"
#include "colombina/ffmpeg/SourcePool.h"
#include "colombina/ffmpeg/FramePipeline.h"
#include "laartman/core/DecoderMetrics.h"
#include "ui/monitor/FrameWorker.h"
#include "ui/monitor/MonitorScrubBar.h"

#include <QPainter>
#include <QPixmap>
#include <QPolygon>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QLabel>
#include <QComboBox>
#include <QSignalBlocker>
#include <QPointer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QShortcut>
#include <QIODevice>
#include <QAudioFormat>
#include <QPainterPath>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSettings>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif
#include <algorithm>
#include <atomic>
#include <QDebug>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <utility>
#include <QHash>
#include <QMutex>


#include "ui/monitor/PreviewPriv.h"

void PreviewWidget::drawGrid(QPainter& p, const QRect& canvas) {
    if (!m_showGrid || canvas.width() < 2 || canvas.height() < 2) return;

    p.save();
    p.setClipRect(canvas);
    p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::SolidLine));

    // Margens de segurança do Premiere: dois retângulos concêntricos —
    // "Action Safe" a 90% e "Title Safe" a 80% do quadro, com os cantos
    // marcados (é assim que o Premiere as desenha, não uma grade NxN).
    const auto insetRect = [&](double frac) {
        const double w = canvas.width() * frac;
        const double h = canvas.height() * frac;
        return QRectF(canvas.center().x() - w / 2.0, canvas.center().y() - h / 2.0,
                      w, h);
    };
    const auto cornerTicks = [&](const QRectF& r, int len) {
        const double x0 = r.left(), x1 = r.right(), y0 = r.top(), y1 = r.bottom();
        p.drawLine(QPointF(x0, y0), QPointF(x0 + len, y0));
        p.drawLine(QPointF(x0, y0), QPointF(x0, y0 + len));
        p.drawLine(QPointF(x1, y0), QPointF(x1 - len, y0));
        p.drawLine(QPointF(x1, y0), QPointF(x1, y0 + len));
        p.drawLine(QPointF(x0, y1), QPointF(x0 + len, y1));
        p.drawLine(QPointF(x0, y1), QPointF(x0, y1 - len));
        p.drawLine(QPointF(x1, y1), QPointF(x1 - len, y1));
        p.drawLine(QPointF(x1, y1), QPointF(x1, y1 - len));
    };

    const QRectF action = insetRect(0.90);
    const QRectF title = insetRect(0.80);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::SolidLine));
    p.drawRect(action);
    p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::SolidLine));
    p.drawRect(title);
    cornerTicks(action, 10);
    cornerTicks(title, 8);

    p.restore();
}


// ════════════════════════════════════════════════════════════════════════
// Overlay de edição de máscara: desenha as formas do clipe sobre o monitor
// com alças arrastáveis (mover centro, redimensionar bordas, rotacionar) e
// repassa as edições via sinal. O usuário arrasta direto no preview, como no
// Pantalone/Transform do Vegas — só é aplicado ao clipe quando o dialog salva.
// ════════════════════════════════════════════════════════════════════════
namespace {

const Clip* previewFindClip(const Project* p, const QString& id) {
    if (!p || id.isEmpty()) return nullptr;
    for (const Track& t : p->videoTracks)
        for (const Clip& c : t.clips)
            if (c.id == id) return &c;
    for (const Track& t : p->audioTracks)
        for (const Clip& c : t.clips)
            if (c.id == id) return &c;
    return nullptr;
}

// Dimensões do quadro onde a máscara vive (o frame já cortado). Para o clipe
// do topo usa m_frame; senão deriva da mídia + pan/crop (mesmo espaço do
// applyMasks). Só a proporção importa para o overlay.
QSize previewMaskAnchorSize(const Project* p, const Clip* c,
                            const QImage& topFrame, const QString& topId) {
    if (c && c->id == topId && !topFrame.isNull())
        return topFrame.size();
    const MediaItem* m = c && p ? p->findMedia(c->mediaId) : nullptr;
    int w = m ? m->width : 0, h = m ? m->height : 0;
    if (w <= 0 || h <= 0) { w = p ? p->width : 1920; h = p ? p->height : 1080; }
    const double cl = std::clamp(c ? c->cropL : 0.0, 0.0, 1000.0);
    const double cr = std::clamp(c ? c->cropR : 0.0, 0.0, 1000.0);
    const double ct = std::clamp(c ? c->cropT : 0.0, 0.0, 1000.0);
    const double cb = std::clamp(c ? c->cropB : 0.0, 0.0, 1000.0);
    const int cw = qMax(1, (int)std::lround(w * (1.0 - (cl + cr) / 1000.0)));
    const int ch = qMax(1, (int)std::lround(h * (1.0 - (ct + cb) / 1000.0)));
    return QSize(cw, ch);
}

// Mesma aritmética da drawLayer (paintEvent): imagem do clipe → monitor.
QTransform previewLayerToScreen(const Project* p, const Clip& c,
                                const QSize& imgSize, double rel,
                                double k, const QPointF& center) {
    QTransform t;
    const double W = p ? p->width : 1920.0;
    const double H = p ? p->height : 1080.0;
    const double iw = qMax(1, imgSize.width());
    const double ih = qMax(1, imgSize.height());
    const double fit = qMin(W / iw, H / ih);
    const double s  = kfValue(c.kfScale, c.scale, rel);
    const double sX = kfValue(c.kfScaleX, c.scaleX, rel);
    const double sY = kfValue(c.kfScaleY, c.scaleY, rel);
    const double rot = kfValue(c.kfRotation, c.rotation, rel);
    const double tx = kfValue(c.kfTx, c.tx, rel);
    const double ty = kfValue(c.kfTy, c.ty, rel);
    t.translate(center.x() + tx * k, center.y() + ty * k);
    t.rotate(rot);
    t.scale(k * fit * s * sX, k * fit * s * sY);
    t.translate(-iw / 2.0, -ih / 2.0);
    return t;
}

// Controles da máscara em pixels do espaço da imagem:
// pts[0]=centro, pts[1]=alça de rotação, pts[2..5]=extremos (E, topo, O, baixo).
struct MaskCtrl {
    QVector<QPointF> pts;
};
MaskCtrl previewMaskCtrl(const Mask& m, const QSize& size, double rel) {
    MaskCtrl o;
    const double W = size.width(), H = size.height();
    const double cx = m.cxAt(rel) * W, cy = m.cyAt(rel) * H;
    const double rx = std::max(0.0, m.rxAt(rel)) * W;
    const double ry = std::max(0.0, m.ryAt(rel)) * H;
    const double rad = m.rotAt(rel) * M_PI / 180.0;
    QVector<QPointF> base;
    if (m.type == QLatin1String("rect")) {
        base = { QPointF(cx - rx, cy - ry), QPointF(cx + rx, cy - ry),
                 QPointF(cx + rx, cy + ry), QPointF(cx - rx, cy + ry) };
    } else { // ellipse: extremos cardeais
        base = { QPointF(cx - rx, cy), QPointF(cx, cy - ry),
                 QPointF(cx + rx, cy), QPointF(cx, cy + ry) };
    }
    if (rad != 0.0) {
        const double cs = std::cos(rad), sn = std::sin(rad);
        for (QPointF& b : base) {
            const double dx = b.x() - cx, dy = b.y() - cy;
            b = QPointF(cx + dx * cs - dy * sn, cy + dx * sn + dy * cs);
        }
    }
    const double arm = qMax(1.0, std::min(rx, ry));
    o.pts = { QPointF(cx, cy),
              QPointF(cx - arm * std::sin(rad), cy - arm * std::cos(rad)) };
    for (const QPointF& b : base) o.pts.append(b);
    return o;
}

void maskHandleSquare(QPainter& p, const QPointF& pt, const QColor& accent) {
    const double hs = 4.5;
    p.setBrush(Qt::white);
    p.setPen(QPen(accent, 1.4));
    p.drawRect(QRectF(pt.x() - hs, pt.y() - hs, 2.0 * hs, 2.0 * hs));
}

} // namespace

void PreviewWidget::setMaskOverlay(const QString& clipId, const QVector<Mask>& masks) {
    m_maskOverlayClipId = clipId;
    m_maskOverlay = masks;
    m_maskDragIndex = -1;
    m_maskDragHandle = -1;
    update();
}

void PreviewWidget::drawMaskOverlay(QPainter& p, const QRect& canvas, double k) {
    if (m_maskOverlayClipId.isEmpty() || !m_project || m_maskOverlay.isEmpty()) return;
    const Clip* clip = previewFindClip(m_project, m_maskOverlayClipId);
    if (!clip) { setMaskOverlay(QString(), {}); return; }
    const double rel = m_playhead - clip->pos;
    if (m_playhead < clip->pos - 1e-6 || m_playhead > clip->pos + clip->dur + 1e-6) return;
    const QSize imgSize = previewMaskAnchorSize(m_project, clip, m_frame, m_maskOverlayClipId);
    if (imgSize.isEmpty()) return;
    const QPointF center(canvas.center());
    m_maskToScreen = previewLayerToScreen(m_project, *clip, imgSize, rel, k, center);
    m_maskAnchorSize = imgSize;

    const QColor accent = themeColors().accent;
    const QColor band(130, 215, 255); // contorno vivo, visível sobre qualquer mídia
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_maskOverlay.size(); ++i) {
        const Mask& m = m_maskOverlay[i];
        if (m.type.isEmpty()) continue;
        const double cx = m.cxAt(rel) * imgSize.width();
        const double cy = m.cyAt(rel) * imgSize.height();
        const double rx = std::max(0.001, m.rxAt(rel)) * imgSize.width();
        const double ry = std::max(0.001, m.ryAt(rel)) * imgSize.height();
        const double rot = m.rotAt(rel);
        QPainterPath path;
        if (m.type == QLatin1String("rect"))
            path.addRect(QRectF(cx - rx, cy - ry, 2.0 * rx, 2.0 * ry));
        else if (m.type == QLatin1String("ellipse"))
            path.addEllipse(QPointF(cx, cy), rx, ry);
        else continue;
        if (rot != 0.0) {
            QTransform r; r.translate(cx, cy); r.rotate(rot); r.translate(-cx, -cy);
            path = r.map(path);
        }
        const bool en = m.hasMask();
        p.setPen(QPen(en ? band : QColor(band.red(), band.green(), band.blue(), 110),
                       1.6, Qt::DashLine));
        p.setBrush(en ? QColor(accent.red(), accent.green(), accent.blue(), 34)
                      : Qt::NoBrush);
        p.drawPath(m_maskToScreen.map(path));
        if (!en) continue;

        const MaskCtrl ctl = previewMaskCtrl(m, imgSize, rel);
        const QPointF c0 = m_maskToScreen.map(ctl.pts[0]);
        const QPointF c1 = m_maskToScreen.map(ctl.pts[1]);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.2));
        p.drawLine(c0, c1);
        maskHandleSquare(p, c1, accent);
        maskHandleSquare(p, c0, accent);
        for (int b = 2; b <= 5; ++b)
            maskHandleSquare(p, m_maskToScreen.map(ctl.pts[b]), accent);
    }
    p.restore();
}

bool PreviewWidget::pickMaskHandle(const QPoint& pos) {
    if (m_maskOverlayClipId.isEmpty() || !m_project) return false;
    const Clip* clip = previewFindClip(m_project, m_maskOverlayClipId);
    if (!clip) return false;
    const double rel = m_playhead - clip->pos;
    if (m_playhead < clip->pos - 1e-6 || m_playhead > clip->pos + clip->dur + 1e-6)
        return false;
    const QSize imgSize = previewMaskAnchorSize(m_project, clip, m_frame, m_maskOverlayClipId);
    if (imgSize.isEmpty()) return false;
    // Reconstrói canvas/escala como no paintEvent (para as alças baterem).
    const QRect work = m_videoRect.adjusted(12, 12, -12, -12);
    double k = m_zoom > 0.0 ? m_zoom
                            : qMin(work.width() / double(m_project->width),
                                   work.height() / double(m_project->height));
    QRect canvas(QPoint(0, 0), QSize(qMax(1, (int)(m_project->width * k)),
                                     qMax(1, (int)(m_project->height * k))));
    canvas.moveCenter(work.center());
    canvas = canvas.intersected(work);
    const QPointF center(canvas.center());
    m_maskToScreen = previewLayerToScreen(m_project, *clip, imgSize, rel, k, center);
    m_maskAnchorSize = imgSize;
    const QTransform imgToScreen = m_maskToScreen;
    const double tol = 9.0; // px de tolerância das alças
    for (int i = 0; i < m_maskOverlay.size(); ++i) {
        const Mask& m = m_maskOverlay[i];
        if (m.type.isEmpty() || !m.hasMask()) continue;
        const MaskCtrl ctl = previewMaskCtrl(m, imgSize, rel);
        int best = -1;
        double bestD = tol * tol;
        for (int h = 0; h < ctl.pts.size(); ++h) {
            const QPointF sp = imgToScreen.map(ctl.pts[h]);
            const double dx = sp.x() - pos.x(), dy = sp.y() - pos.y();
            const double d2 = dx * dx + dy * dy;
            if (d2 < bestD) { bestD = d2; best = h; }
        }
        if (best >= 0) {
            m_maskDragIndex = i;
            m_maskDragHandle = best;
            m_maskDragLast = pos;
            m_maskDragPressRot = m.rotAt(rel);
            const QPointF cur = m_maskToScreen.inverted().map(QPointF(pos));
            m_maskDragPressAng = std::atan2(cur.y() - m.cyAt(rel) * imgSize.height(),
                                            cur.x() - m.cxAt(rel) * imgSize.width());
            return true;
        }
    }
    return false;
}

void PreviewWidget::applyMaskDrag(const QPoint& pos) {
    if (m_maskDragIndex < 0 || m_maskDragIndex >= m_maskOverlay.size()) return;
    Mask& m = m_maskOverlay[m_maskDragIndex];
    const double iw = m_maskAnchorSize.width(), ih = m_maskAnchorSize.height();
    if (iw < 1 || ih < 1) return;
    const QPointF cur = m_maskToScreen.inverted().map(QPointF(pos));
    const double cx = m.cxAt(0.0) * iw, cy = m.cyAt(0.0) * ih;
    if (m_maskDragHandle == 0) {
        // Mover: o centro acompanha o cursor.
        m.cx = std::clamp(cur.x() / iw, 0.0, 1.0);
        m.cy = std::clamp(cur.y() / ih, 0.0, 1.0);
    } else if (m_maskDragHandle == 1) {
        // Rotação: ângulo do cursor ao redor do centro (contínuo).
        const double ang = std::atan2(cur.y() - cy, cur.x() - cx);
        double d = ang - m_maskDragPressAng;
        while (d > M_PI) d -= 2.0 * M_PI;
        while (d < -M_PI) d += 2.0 * M_PI;
        double rot = m_maskDragPressRot + d * 180.0 / M_PI;
        while (rot > 180.0) rot -= 360.0;
        while (rot < -180.0) rot += 360.0;
        m.rotation = rot;
    } else {
        // Borda: raio = distância do centro projetada nos eixos da forma.
        const double dx = cur.x() - cx, dy = cur.y() - cy;
        const double rad = m.rotAt(0.0) * M_PI / 180.0;
        const double lx =  dx * std::cos(rad) + dy * std::sin(rad);
        const double ly = -dx * std::sin(rad) + dy * std::cos(rad);
        if (std::abs(lx) > 1.0) m.rx = qBound(0.001, std::abs(lx) / iw, 1.5);
        if (std::abs(ly) > 1.0) m.ry = qBound(0.001, std::abs(ly) / ih, 1.5);
    }
    m_maskDragLast = pos;
    emit maskEdited(m_maskDragIndex, m);
    update();
}

void PreviewWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && pickMaskHandle(e->pos())) {
        update();
        return;
    }
    QWidget::mousePressEvent(e);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_maskDragIndex >= 0) {
        applyMaskDrag(e->pos());
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (m_maskDragIndex >= 0) {
        m_maskDragIndex = -1;
        m_maskDragHandle = -1;
        emit maskDragEnd();
        update();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}
