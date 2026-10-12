// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "OfxExportRenderer.h"
#include "PreRenderSession.h"
#include "colombina/ofx/OfxRenderer.h"
#include "colombina/ofx/OfxPluginManager.h"
#include "colombina/ofx/OfxHost.h"
#include "colombina/models/Project.h"
#include "laartman/FFmpegDecoder.h"

#include <QDir>
#include <QStandardPaths>
#include <QProcess>
#include <QImage>
#include <QDebug>

// Decodifica um frame do vídeo no tempo dado (segundos), retornando QImage.
static QImage decodeFrame(FFmpegDecoder& dec, double seconds) {
    return dec.frameAt(seconds);
}

QString OfxExportRenderer::renderClip(const Project& project,
                                       const QString& clipId,
                                       int outputFps,
                                       const OfxPluginManager* ofxManager,
                                       const Progress& progress,
                                       QString* error)
{
    if (!ofxManager) return {};

    // Encontra o clipe e seu media.
    const Clip* clip = nullptr;
    const MediaItem* media = nullptr;
    for (const Track& t : project.videoTracks) {
        for (const Clip& c : t.clips) {
            if (c.id == clipId) {
                clip = &c;
                media = project.findMedia(c.mediaId);
                break;
            }
        }
        if (clip) break;
    }
    if (!clip || !media || clip->ofxFx.isEmpty()) return {};

    // Verifica se algum efeito OFX está habilitado.
    bool hasOfx = false;
    for (const OfxPluginInstance& fx : clip->ofxFx) {
        if (fx.enabled) { hasOfx = true; break; }
    }
    if (!hasOfx) return {};

    // Abre o decodificador.
    FFmpegDecoder dec;
    if (!dec.open(media->filePath)) {
        if (error) *error = QString("Não foi possível abrir mídia: %1").arg(media->filePath);
        return {};
    }

    const int clipW = project.width;
    const int clipH = project.height;
    const double speed = clip->speed > 0.0 ? clip->speed : 1.0;
    const double timelineDur = clip->dur / speed;
    const int totalFrames = std::max(1, (int)std::ceil(timelineDur * outputFps));

    // Prepara arquivo temporário.
    const QString tmpPath = QDir::tempPath()
        + QString("/pierrot-ofx-%1.mp4").arg(clipId.left(8));

    // Inicia ffmpeg para escrever o arquivo.
    prerender::PreRenderSession session(clipW, clipH, outputFps, tmpPath);
    if (!session.start(error, "OFX"))
        return {};

    // Decodifica e processa frame a frame.
    for (int f = 0; f < totalFrames; ++f) {
        // Verifica cancelamento (via progress lambda OU abort direto).
        if (OfxHostImpl::abortRequested() ||
            (progress && !progress(f, totalFrames))) {
            session.abort();
            if (error) *error = "Pré-renderização OFX cancelada";
            return {};
        }

        // Tempo na timeline.
        const double timelineT = (double)f / outputFps;
        // Tempo na fonte.
        const double srcT = clip->in + timelineT * speed;

        // Decodifica o frame.
        QImage frame = decodeFrame(dec, srcT);
        if (frame.isNull()) {
            frame = QImage(clipW, clipH, QImage::Format_RGBA8888);
            frame.fill(Qt::black);
        }

        // Redimensiona se necessário.
        if (frame.width() != clipW || frame.height() != clipH) {
            frame = frame.scaled(clipW, clipH, Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
        }

        // Aplica efeitos OFX.
        frame = OfxRenderer::applyOfxEffects(frame, clip->ofxFx, ofxManager, timelineT);

        // Escreve o frame no pipe.
        if (!session.writeFrame(frame, error, "OFX"))
            return {};
    }

    // Finaliza o ffmpeg.
    if (!session.finish(error, "OFX"))
        return {};

    // Move para local permanente.
    return prerender::publishToCache(
        tmpPath, QStringLiteral("ofx"),
        QStringLiteral("ofx_%1.mp4").arg(clipId.left(8)), error, "OFX");
}

QHash<QString, QString> OfxExportRenderer::renderAll(const Project& project,
                                                       int outputFps,
                                                       const OfxPluginManager* ofxManager,
                                                       const Progress& progress,
                                                       QString* error)
{
    QHash<QString, QString> result;
    if (!ofxManager) return result;

    // Coleta IDs de clipes com efeitos OFX habilitados.
    QStringList ofxClipIds;
    for (const Track& t : project.videoTracks)
        for (const Clip& c : t.clips)
            if (!c.ofxFx.isEmpty()) {
                bool hasOfx = false;
                for (const OfxPluginInstance& fx : c.ofxFx)
                    if (fx.enabled) { hasOfx = true; break; }
                if (hasOfx) ofxClipIds.append(c.id);
            }

    if (ofxClipIds.isEmpty()) return result;

    const int total = ofxClipIds.size();
    for (int i = 0; i < total; ++i) {
        QString clipError;
        const QString path = renderClip(project, ofxClipIds[i], outputFps,
                                         ofxManager, progress, &clipError);
        if (!path.isEmpty()) {
            result.insert(ofxClipIds[i], path);
        } else if (!clipError.isEmpty()) {
            qWarning() << "OFX render error for clip" << ofxClipIds[i] << ":" << clipError;
            if (error) *error = clipError;
        }
    }

    return result;
}
