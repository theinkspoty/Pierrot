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

QImage MesaRenderer::render(const MesaComposition& mesa, const Project& project,
                             double time) {
    const int outW = project.width;
    const int outH = project.height;
    if (outW <= 0 || outH <= 0 || mesa.canvasW <= 0 || mesa.canvasH <= 0) return {};

    QImage out(outW, outH, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setClipRect(0, 0, outW, outH);

    // Câmera no estilo After Effects: um ponto da composição (camX, camY,
    // ABSOLUTO, origem topo-esquerda) fica no centro do frame de saída, com
    // rotação e zoom. zoom = 1.0 com a câmera no centro da comp e a comp
    // proporcional ao output produz mapeamento 1:1 (contido no frame).
    const double camX = kfValue(mesa.kfCamX, mesa.camX, time);
    const double camY = kfValue(mesa.kfCamY, mesa.camY, time);
    const double zoom = qMax(0.001, kfValue(mesa.kfCamZoom, mesa.camZoom, time));
    const double rot = kfValue(mesa.kfCamRotation, mesa.camRotation, time);
    const double fit = qMin(double(outW) / mesa.canvasW,
                            double(outH) / mesa.canvasH);
    const double s = zoom * fit;

    // View = T(centro do output) · R · S · T(-posição da câmera) no espaço da
    // composição. Cada camada empilha a matriz local por cima
    // (T(pos)·R·S·T(-âncora)). Como o painter clipa só no frame de saída, uma
    // imagem pode ficar em QUALQUER coordenada da comp (fora dos limites do
    // canvas) e ainda aparece no preview quando a câmera apontar pra ela —
    // não existe bitmap do tamanho da comp para cortá-la.
    p.translate(outW / 2.0, outH / 2.0);
    p.rotate(rot);
    p.scale(s, s);
    p.translate(-camX, -camY);
    renderToPainter(p, mesa, project, time);

    return out;
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
            // Renderiza texto no TAMANHO NATURAL da camada (a composição).
            // A escala é aplicada pela matriz da camada — antes o texto era
            // escalado duas vezes (frame × sc e de novo no drawImage).
            const TextStyle* ts = project.textStyleFor(c);
            const int fw = qMax(64, (int)(mesa.canvasW));
            const int fh = qMax(32, (int)(mesa.canvasH));
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
    // Posição = coordenada absoluta da âncora na composição (origem topo-left).
    out.posX = tMesaX;
    out.posY = tMesaY;
    out.rot = tRot;
    out.sx = tScX;
    out.sy = tScY;
    // Âncora: offset do centro natural da layer (px da própria layer).
    out.ax = kfValue(track.kfMesaAnchorX, track.mesaAnchorX, relTime);
    out.ay = kfValue(track.kfMesaAnchorY, track.mesaAnchorY, relTime);
    out.opacity = tOp;
    out.valid = true;
    return true;
}

void MesaRenderer::drawTrackImage(QPainter& acc, const LayerPrep& prep) {
    if (!prep.valid || prep.frame.isNull()) return;
    // Matriz local→comp idêntica ao After Effects:
    // M = T(posição) · R(rotação) · S(escala) · T(-âncora),
    // com o frame desenhado com o topo-esquerdo na origem local.
    // QPainter compõe na ordem das chamadas (1ª = mais externa), então a
    // sequência abaixo gera exatamente M (ver desenho do quad em layerScreenRect).
    acc.save();
    acc.translate(prep.posX, prep.posY);
    acc.rotate(prep.rot);
    acc.scale(prep.sx, prep.sy);
    acc.translate(-(prep.frame.width() / 2.0 + prep.ax),
                  -(prep.frame.height() / 2.0 + prep.ay));

    acc.setRenderHint(QPainter::SmoothPixmapTransform);
    if (prep.opacity < 1.0)
        acc.setOpacity(prep.opacity);
    acc.setCompositionMode(static_cast<QPainter::CompositionMode>(prep.blend));
    acc.drawImage(QRectF(0, 0, prep.frame.width(), prep.frame.height()), prep.frame);
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
