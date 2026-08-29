// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaRenderer.h"
#include "ffmpeg/FFmpegDecoder.h"

#include <QPainter>
#include <QPainterPath>
#include <QtMath>

MesaRenderer::MesaRenderer() {}

MesaRenderer::~MesaRenderer() { clearCache(); }

void MesaRenderer::clearCache() {
    QMutexLocker l(&m_mutex);
    for (auto it = m_decoders.begin(); it != m_decoders.end(); ++it) {
        it.value()->releaseBuffers();
        it.value()->close();
        delete it.value();
    }
    m_decoders.clear();
}

QImage MesaRenderer::decodeFrame(const QString& filePath, double time, int maxW) {
    QMutexLocker l(&m_mutex);
    if (m_frameCache.key.path == filePath && qFuzzyCompare(m_frameCache.key.time, time)
        && m_frameCache.key.maxW == maxW && !m_frameCache.frame.isNull()) {
        return m_frameCache.frame;
    }
    FFmpegDecoder* dec = m_decoders.value(filePath);
    if (!dec) {
        dec = new FFmpegDecoder();
        if (!dec->open(filePath)) { delete dec; return {}; }
        m_decoders.insert(filePath, dec);
    }
    QImage frame = dec->frameAt(time, maxW);
    m_frameCache = { { filePath, time, maxW }, frame };
    return frame;
}

QImage MesaRenderer::applyCameraTransform(const QImage& canvas,
                                           const MesaComposition& mesa,
                                           const Project& project,
                                           double relTime) {
    const int outW = project.width;
    const int outH = project.height;
    const double cx = kfValue(mesa.kfCamX, mesa.camX, relTime);
    const double cy = kfValue(mesa.kfCamY, mesa.camY, relTime);
    const double zoom = qMax(0.01, kfValue(mesa.kfCamZoom, mesa.camZoom, relTime));
    const double rot = kfValue(mesa.kfCamRotation, mesa.camRotation, relTime);

    QImage out(outW, outH, QImage::Format_ARGB32);
    // Transparente (não preto opaco): as áreas do canvas sem conteúdo deixam
    // passar as faixas de VÍDEO INFERIORES no empilhamento (clipes com corte
    // por baixo da composição). No preview sem camadas abaixo, o fundo escuro
    // do monitor assume — visual idêntico ao preto.
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    p.translate(outW / 2.0, outH / 2.0);
    p.rotate(rot);

    const double scale = qMin(outW, outH) * zoom / qMax(canvas.width(), canvas.height());
    p.scale(scale, scale);

    p.translate(-canvas.width() / 2.0 - cx, -canvas.height() / 2.0 - cy);

    p.setClipRect(0, 0, outW, outH);
    p.drawImage(0, 0, canvas);

    return out;
}

QImage MesaRenderer::render(const MesaComposition& mesa, const Project& project,
                             double time) {
    QImage canvas = renderCanvas(mesa, project, time);
    // A câmera é avaliada no mesmo tempo absoluto das layers: os keyframes de
    // câmera são gravados pelo MesaWidget na posição global do playhead.
    return applyCameraTransform(canvas, mesa, project, time);
}

// Desenha uma única track (camada) num painter `acc` já preparado.
// Retorna false se nada foi desenhado (sem clip ativo / frame vazio).
bool MesaRenderer::drawTrackLayer(QPainter& acc, const Track& track,
                                  const MesaComposition& mesa, const Project& project,
                                  double relTime) {
    LayerPrep prep;
    if (!prepareLayer(prep, mesa, project, relTime, track)) return false;
    drawTrackImage(acc, prep);
    return true;
}

bool MesaRenderer::prepareLayer(LayerPrep& out, const MesaComposition& mesa,
                                const Project& project, double relTime,
                                const Track& track) {
    // Layer oculta (olho desligado) não existe no empilhamento — nem no
    // canvas do editor, nem no preview, nem (futuro) no export.
    if (track.mesaHidden) return false;
    out.blend = blendModeFor(track.blendMode);
    const double tMesaX = kfValue(track.kfMesaX, track.mesaX, relTime);
    const double tMesaY = kfValue(track.kfMesaY, track.mesaY, relTime);
    const double tScX = kfValue(track.kfMesaScaleX, track.mesaScaleX, relTime);
    const double tScY = kfValue(track.kfMesaScaleY, track.mesaScaleY, relTime);
    const double tRot = kfValue(track.kfMesaRotation, track.mesaRotation, relTime);
    const double tOp = std::clamp(kfValue(track.kfMesaOpacity, track.mesaOpacity, relTime),
                                  0.0, 1.0);

    // Encontra o clip ativo nesta track no tempo rel
    QImage frame;
    for (const Clip& c : track.clips) {
        const double cRel = relTime - c.pos;
        if (cRel < 0 || cRel >= c.dur) continue;

        if (c.isText) {
            // Renderiza texto
            const TextStyle* ts = project.textStyleFor(c);
            const int fw = qMax(64, (int)(mesa.canvasW * tScX));
            const int fh = qMax(32, (int)(mesa.canvasH * tScY));
            frame = QImage(fw, fh, QImage::Format_ARGB32);
            frame.fill(Qt::transparent);
            const double sizeFrac = ts->textSize > 0.0 ? ts->textSize : (1.0 / 18.0);
            const int pxSize = qMax(4, (int)qRound(sizeFrac * fh));
            QFont font;
            if (!ts->fontFamily.isEmpty()) font.setFamily(ts->fontFamily);
            font.setPixelSize(pxSize);
            font.setBold(ts->textBold);
            QPainter fp(&frame);
            fp.setRenderHint(QPainter::Antialiasing);
            fp.translate(fw / 2.0, fh / 2.0);
            QPainterPath path;
            path.addText(QPointF(0, 0), font, c.text.text.isEmpty()
                ? ts->text : c.text.text);
            if (ts->textOutline > 0.0)
                fp.strokePath(path, QPen(ts->textOutlineColor,
                                         qMax(1.0, ts->textOutline * fh)));
            fp.fillPath(path, ts->textColor);
        } else if (!c.mediaId.isEmpty()) {
            const MediaItem* mi = project.findMedia(c.mediaId);
            if (mi && !mi->filePath.isEmpty()) {
                const double srcT = c.in + cRel * c.speed;
                frame = decodeFrame(mi->filePath, srcT, mesa.canvasW);
            }
        }
        if (!frame.isNull()) break;
    }

    if (frame.isNull()) return false;

    out.frame = frame;
    out.rot = tRot;
    out.opacity = tOp;
    out.anchorX = kfValue(track.kfMesaAnchorX, track.mesaAnchorX, relTime);
    out.anchorY = kfValue(track.kfMesaAnchorY, track.mesaAnchorY, relTime);
    out.pivotX = mesa.canvasW / 2.0 + tMesaX;
    out.pivotY = mesa.canvasH / 2.0 + tMesaY;
    out.drawW = frame.width() * tScX;
    out.drawH = frame.height() * tScY;
    out.valid = true;
    return true;
}

void MesaRenderer::drawTrackImage(QPainter& acc, const LayerPrep& prep) {
    if (!prep.valid || prep.frame.isNull()) return;
    acc.save();
    acc.translate(prep.pivotX, prep.pivotY);
    acc.translate(prep.anchorX, prep.anchorY);
    acc.rotate(prep.rot);
    acc.translate(-prep.anchorX, -prep.anchorY);

    const double drawW = prep.drawW;
    const double drawH = prep.drawH;

    acc.setRenderHint(QPainter::SmoothPixmapTransform);
    if (prep.opacity < 1.0)
        acc.setOpacity(prep.opacity);
    acc.setCompositionMode(static_cast<QPainter::CompositionMode>(prep.blend));
    acc.drawImage(QRectF(-drawW / 2, -drawH / 2, drawW, drawH), prep.frame);
    acc.restore();
}

// Mapeia o blendMode textual do Track para o QPainter::CompositionMode.
int MesaRenderer::blendModeFor(const QString& blend) const {
    if (blend == QStringLiteral("add"))       return QPainter::CompositionMode_Plus;
    if (blend == QStringLiteral("multiply"))  return QPainter::CompositionMode_Multiply;
    if (blend == QStringLiteral("screen"))    return QPainter::CompositionMode_Screen;
    if (blend == QStringLiteral("overlay"))   return QPainter::CompositionMode_Overlay;
    if (blend == QStringLiteral("softlight")) return QPainter::CompositionMode_SoftLight;
    if (blend == QStringLiteral("difference")) return QPainter::CompositionMode_Difference;
    return QPainter::CompositionMode_SourceOver;
}

void MesaRenderer::renderToPainter(QPainter& painter, const MesaComposition& mesa,
                                   const Project& project, double relTime,
                                   const QString* skipTrackId) {
    for (const QString& tid : mesa.trackIds) {
        if (skipTrackId && *skipTrackId == tid) continue;

        const Track* track = nullptr;
        for (const Track& tr : project.videoTracks) {
            if (tr.id == tid) { track = &tr; break; }
        }
        if (!track) {
            for (const Track& tr : project.audioTracks) {
                if (tr.id == tid) { track = &tr; break; }
            }
        }
        if (!track) continue;

        drawTrackLayer(painter, *track, mesa, project, relTime);
    }
}

QImage MesaRenderer::renderCanvas(const MesaComposition& mesa, const Project& project,
                                   double relTime, const QString* skipTrackId) {
    QImage canvas(mesa.canvasW, mesa.canvasH, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);
    QPainter acc(&canvas);
    acc.setRenderHint(QPainter::SmoothPixmapTransform);

    // Para cada track do grupo, renderiza seus clips na posição da track.
    for (const QString& tid : mesa.trackIds) {
        if (skipTrackId && *skipTrackId == tid) continue;

        // Encontra a track
        const Track* track = nullptr;
        for (const Track& tr : project.videoTracks) {
            if (tr.id == tid) { track = &tr; break; }
        }
        if (!track) {
            for (const Track& tr : project.audioTracks) {
                if (tr.id == tid) { track = &tr; break; }
            }
        }
        if (!track) continue;

        drawTrackLayer(acc, *track, mesa, project, relTime);
    }

    acc.end();
    return canvas;
}
