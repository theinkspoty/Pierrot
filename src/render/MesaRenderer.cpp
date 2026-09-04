// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaRenderer.h"
#include "ffmpeg/FFmpegDecoder.h"
#include "generators.h"

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
    m_compositeLru.clear();
}

void MesaRenderer::clearCompositeCache() {
    QMutexLocker l(&m_mutex);
    m_compositeLru.clear();
}

QImage MesaRenderer::compositeFromCache(const CompositeKey& key) {
    QMutexLocker l(&m_mutex);
    for (int i = 0; i < m_compositeLru.size(); ++i) {
        if (m_compositeLru[i].key == key) {
            CompositeEntry e = m_compositeLru.takeAt(i);
            m_compositeLru.prepend(e);
            return e.img;
        }
    }
    return QImage();
}

void MesaRenderer::compositeToCache(const CompositeKey& key, const QImage& img) {
    QMutexLocker l(&m_mutex);
    for (int i = 0; i < m_compositeLru.size(); ++i) {
        if (m_compositeLru[i].key == key) {
            m_compositeLru[i].img = img;
            return;
        }
    }
    CompositeEntry e;
    e.key = key;
    e.img = img;
    m_compositeLru.prepend(e);
    while (m_compositeLru.size() > kCompositeMax)
        m_compositeLru.removeLast();
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

    const int fps = qMax(1, project.fps);
    const CompositeKey key{ mesa.id, qRound64(time * fps), outW, outH, project.revision };
    const QImage cached = compositeFromCache(key);
    if (!cached.isNull()) return cached;

    QImage result;
    const double frameDur = 1.0 / fps;
    const double extent = mesa.motionBlurShutter * frameDur;

    // Motion blur de CÂMERA: o enquadramento é redefinido em cada sub-passada
    // (mostra o rastro da câmera). Cada passada é uma composição LIMPA da pilha
    // (motionBlurStack desligado) — são as passadas dela que integram o rastro;
    // as layers (já com o transform em ts) borram naturalmente junto.
    if (mesa.motionBlur && mesa.motionBlurSamples >= 2 && extent > 0.0) {
        const int n = qBound(2, mesa.motionBlurSamples, 32);
        QImage out(outW, outH, QImage::Format_ARGB32);
        out.fill(Qt::transparent);
        QPainter p(&out);
        p.setClipRect(0, 0, outW, outH);
        const double w = 1.0 / n;
        for (int i = 0; i < n; ++i) {
            const double frac = n == 1 ? 0.0 : double(i) / double(n - 1);
            const double ts = time - extent / 2.0 + frac * extent;
            QImage sample = renderSample(mesa, project, ts, nullptr, false);
            p.save();
            p.setOpacity(w);
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            p.drawImage(0, 0, sample);
            p.restore();
        }
        result = out;
    } else {
        result = renderSample(mesa, project, time, nullptr);
    }

    if (!result.isNull())
        compositeToCache(key, result);
    return result;
}

// Desenha uma passada inteira: câmera (no instante `time`) + todas as layers.
// Câmera no estilo After Effects: um ponto da composição (camX, camY, ABSOLUTO,
// origem topo-esquerda) fica no centro do frame de saída, com rotação e zoom.
// zoom = 1.0 com a câmera no centro da comp e a comp proporcional ao output
// produz mapeamento 1:1 (contido no frame).
QImage MesaRenderer::renderSample(const MesaComposition& mesa, const Project& project,
                                  double time, const QString* skipTrackId,
                                  bool motionBlurStack) {
    const int outW = project.width;
    const int outH = project.height;
    if (outW <= 0 || outH <= 0 || mesa.canvasW <= 0 || mesa.canvasH <= 0) return {};

    QImage out(outW, outH, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setClipRect(0, 0, outW, outH);

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
    renderToPainter(p, mesa, project, time, skipTrackId, motionBlurStack);

    return out;
}

// Empilha as camadas da composição num painter já no canvas-space.
// `relTime` fixa o CONTEÚDO; `transformTime` avalia os keyframes de transform.
void MesaRenderer::paintStack(QPainter& painter, const MesaComposition& mesa,
                              const Project& project, double relTime,
                              const QString* skipTrackId, double transformTime) {
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

        drawTrackLayer(painter, *track, mesa, project, relTime, transformTime);
    }
}

// Desenha uma única track (camada) num painter `acc` já preparado.
// Retorna false se nada foi desenhado (sem clip ativo / frame vazio).
bool MesaRenderer::drawTrackLayer(QPainter& acc, const Track& track,
                                  const MesaComposition& mesa, const Project& project,
                                  double relTime, double transformTime) {
    LayerPrep prep;
    if (!prepareLayer(prep, mesa, project, relTime, track, transformTime)) return false;
    drawTrackImage(acc, prep);
    return true;
}

bool MesaRenderer::prepareLayer(LayerPrep& out, const MesaComposition& mesa,
                                const Project& project, double relTime,
                                const Track& track, double transformTime) {
    // Layer oculta (olho desligado) não existe no empilhamento — nem no
    // canvas do editor, nem no preview, nem (futuro) no export.
    if (track.mesaHidden) return false;
    // tempo de transform separado do tempo de conteúdo: `transformTime` rasteja
    // os keyframes das props de canvas (motion blur); `relTime` fixa o clip
    // ativo. Camada com motion blur desligado (mesaMotionBlur=false) fica
    // FIXA no relTime — é ela não borra nas sub-passadas.
    const double t = (transformTime >= 0.0 && track.mesaMotionBlur) ? transformTime
                                                                    : relTime;
    out.blend = blendModeFor(track.blendMode);
    const double tMesaX = kfValue(track.kfMesaX, track.mesaX, t);
    const double tMesaY = kfValue(track.kfMesaY, track.mesaY, t);
    const double tScX = kfValue(track.kfMesaScaleX, track.mesaScaleX, t);
    const double tScY = kfValue(track.kfMesaScaleY, track.mesaScaleY, t);
    const double tRot = kfValue(track.kfMesaRotation, track.mesaRotation, t);
    const double tOp = std::clamp(kfValue(track.kfMesaOpacity, track.mesaOpacity, t),
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
            if (mi) {
                if (mi->isSolid) {
                    // Mídia virtual (sólido/gradiente/checkerboard/noise):
                    // gerada no tamanho próprio (ou da comp, se não definido).
                    const int fw = mi->width > 0 ? mi->width : (int)mesa.canvasW;
                    const int fh = mi->height > 0 ? mi->height : (int)mesa.canvasH;
                    frame = generatorFrame(*mi, fw, fh);
                } else if (!mi->filePath.isEmpty()) {
                    const double srcT = clipSrcTime(c, cRel);
                    frame = decodeFrame(mi->filePath, srcT, mesa.canvasW);
                }
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
                                   const QString* skipTrackId, bool motionBlurStack) {
    const bool mb = motionBlurStack && mesa.motionBlur && mesa.motionBlurSamples >= 2
                 && mesa.motionBlurShutter > 0.0;
    if (!mb) {
        paintStack(painter, mesa, project, relTime, skipTrackId, relTime);
        return;
    }

    // Motion blur por sub-passadas COMPLETAS (estilo AE): cada passada desenha a
    // pilha inteira no instante `ts` (oclusão entre camadas correta em cada uma);
    // as n passadas somam com peso 1/n num buffer do tamanho do clip. Estática:
    // n·(1/n) = identidade (nada borra); em movimento, as bordas ficam com
    // cobertura parcial (as próprias bordas do rastro). O buffer usa o clip do
    // painter para capturar a VIEW atual da câmera sem depender do tamanho do
    // canvas infinito.
    const QRect cr = painter.clipBoundingRect().toAlignedRect();
    if (cr.isEmpty() || cr.width() > 16384 || cr.height() > 16384) {
        paintStack(painter, mesa, project, relTime, skipTrackId, relTime);
        return;
    }

    const int n = qBound(2, mesa.motionBlurSamples, 32);
    const double frameDur = 1.0 / qMax(1, project.fps);
    const double extent = mesa.motionBlurShutter * frameDur;
    const QTransform view = painter.transform();

    QImage acc(cr.size(), QImage::Format_ARGB32_Premultiplied);
    acc.fill(Qt::transparent);
    QPainter ap(&acc);
    ap.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int i = 0; i < n; ++i) {
        const double frac = n == 1 ? 0.0 : double(i) / double(n - 1);
        const double ts = relTime - extent / 2.0 + frac * extent;
        QImage pass(cr.size(), QImage::Format_ARGB32_Premultiplied);
        pass.fill(Qt::transparent);
        QPainter pp(&pass);
        pp.setRenderHint(QPainter::SmoothPixmapTransform);
        // Mesma view da câmera do painter, deslocada para o sistema de
        // coordenadas do buffer: pixel (x,y) do pass = device (x+cr.x, y+cr.y).
        // O QImage clipa sozinho nas bordas; nada além do buffer é pintado.
        QTransform passT = view;
        passT.translate(-cr.x(), -cr.y());   // translate pré-multiplicado (saída)
        pp.setTransform(passT);
        // Conteúdo FIXO em relTime; só o transform das camadas rasteja em ts.
        paintStack(pp, mesa, project, relTime, skipTrackId, ts);
        pp.end();
        ap.save();
        ap.setOpacity(1.0 / n);
        ap.setCompositionMode(QPainter::CompositionMode_Plus);
        ap.drawImage(0, 0, pass);
        ap.restore();
    }
    ap.end();

    // Composição final em DEVICE coordinates (cr está em device): zera o
    // transform do painter para o buffer cair exatamente sobre o região do clip.
    painter.save();
    painter.resetTransform();
    painter.drawImage(cr.topLeft(), acc);
    painter.restore();
}
