// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "LainkaRenderer.h"
#include "LainkaFx.h"
#include "PreRenderSession.h"
#include "colombina/models/Project.h"
#include "laartman/Decoder.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QDebug>
#include <QPainter>
#include <QStandardPaths>

// Decodifica um frame do vídeo no tempo dado (segundos), retornando QImage RGB.
static QImage decodeFrame(Decoder& dec, double seconds) {
    return dec.frame(seconds);
}

QString LainkaRenderer::renderClip(const Project& project,
                                    const QString& clipId,
                                    int outputFps,
                                    const LainkaProgressFn& progress,
                                    QString* error) {
    // Encontra o clipe no projeto.
    const Clip* clip = nullptr;
    int trackIdx = -1;
    bool isAudio = false;
    for (int i = 0; i < project.videoTracks.size(); ++i) {
        for (const Clip& c : project.videoTracks[i].clips) {
            if (c.id == clipId) { clip = &c; trackIdx = i; isAudio = false; break; }
        }
        if (clip) break;
    }
    if (!clip) {
        if (error) *error = QString("Clip %1 não encontrado").arg(clipId);
        return {};
    }

    // Sem LAINKA habilitado: retorna vazio (não precisa pré-renderizar).
    if (!clip->lainkaEnabled) return {};

    // Abre o decoder para o arquivo de mídia.
    const MediaItem* mi = project.findMedia(clip->mediaId);
    if (!mi) {
        if (error) *error = QString("Mídia %1 não encontrada").arg(clip->mediaId);
        return {};
    }

    Decoder dec;
    if (!dec.open(mi->filePath)) {
        if (error) *error = QString("Não foi possível abrir %1").arg(mi->filePath);
        return {};
    }

    const double speed = clip->speed > 0 ? clip->speed : 1.0;

    // d é a duração que o buildCommand usa: -t (dur * speed).
    // Se dur=29.1 e speed=1, o buildCommand lê 29.1s do arquivo.
    // O arquivo pré-renderizado DEVE ter exatamente essa duração.
    const double cmdDuration = clip->dur * speed;

    // Número de frames de saída.
    const int totalFrames = std::max(1, (int)std::lround(cmdDuration * outputFps));

    // Cria arquivo temporário.
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        if (error) *error = "Não foi possível criar diretório temporário";
        return {};
    }

    // Determina resolução do clipe: usa SEMPRE as dimensões do projeto,
    // pois o arquivo pré-renderizado deve ter o mesmo tamanho do canvas
    // para o buildCommand() aplicar scale/pad corretamente.
    const int clipW = project.width;
    const int clipH = project.height;

    const QString tmpPath = tmpDir.filePath(
        QString("lainka_%1.mov").arg(clipId.left(8)));

    // Inicia ffmpeg para codificação via pipe.
    prerender::PreRenderSession session(clipW, clipH, outputFps, tmpPath);
    if (!session.start(error, "LAINKA"))
        return {};

    // Estado para motion blur (frame anterior).
    QImage prevFrame;

    // Decodifica e processa frame a frame.
    for (int f = 0; f < totalFrames; ++f) {
        // Verifica cancelamento.
        if (progress && !progress(f, totalFrames)) {
            session.abort();
            if (error) *error = "Pré-renderização LAINKA cancelada";
            return {};
        }

        // Tempo na timeline e tempo de origem.
        // clip->dur é da fonte; timelineDur = dur/speed.
        // Frame f → tempo na timeline = f/outputFps.
        // Tempo na fonte = in + (f/outputFps) * speed.
        const double srcT = clip->in + (double)f / outputFps * speed;

        // Quantiza o tempo para simular stop motion com a MESMA fórmula do
        // preview (PreviewFrames/PreviewReady): skip derivado de targetFps no
        // fps da PROJETO — usar clip->lainkaSkip/fps da fonte dava outra
        // cadência no export e a pré-render divergia do preview.
        const double pfps = project.fps > 0.0 ? project.fps : 30.0;
        const int effSkip = (clip->lainkaTargetFps > 0 && pfps > 1.0)
            ? std::max(1, (int)std::lround(pfps / clip->lainkaTargetFps))
            : std::max(1, clip->lainkaSkip);
        const double fxT = LainkaFx::lainkaQuantizeTime(srcT, effSkip, pfps);

        // Decodifica o frame.
        QImage frame = decodeFrame(dec, fxT);
        if (frame.isNull()) {
            // Frame não decodificado: TRANSPARENTE (não preto opaco — senão o
            // GIF/WebP exportado com alpha ganhava um quadrado preto, issue #7).
            frame = QImage(clipW, clipH, QImage::Format_RGBA8888);
            frame.fill(Qt::transparent);
        }

        // Encaixa no canvas do projeto preservando o aspect ratio (contain +
        // letterbox transparente). IgnoreAspectRatio ESTICAVA a imagem para
        // 16:9 e o filtro de export depois não recuperava (issue #8).
        if (frame.width() != clipW || frame.height() != clipH) {
            const QImage fitted = frame.scaled(clipW, clipH, Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation);
            QImage canvas(clipW, clipH, QImage::Format_RGBA8888);
            canvas.fill(Qt::transparent);
            QPainter p(&canvas);
            p.drawImage((clipW - fitted.width()) / 2,
                        (clipH - fitted.height()) / 2, fitted);
            p.end();
            frame = canvas;
        }

        // Aplica efeito LAINKA.
        frame = LainkaFx::lainkaApplyFx(
            frame, clip->id, fxT,
            clip->lainkaSkip,
            clip->lainkaJitterPos, clip->lainkaJitterRot,
            clip->lainkaJitterScale, clip->lainkaFlicker,
            clip->lainkaFlickerSpeed,
            clip->lainkaWarpAmount, clip->lainkaWarpSpeed, clip->lainkaWarpGrid,
            clip->lainkaOnionSkin, clip->lainkaDustAmount,
            clip->lainkaScratchAmount,
            clip->lainkaMotionBlur, clip->lainkaOpacity,
            clip->lainkaTargetFps, clip->lainkaAntialias,
            prevFrame,
            clip->lainkaHold, clip->lainkaStutter, clip->lainkaPosterize,
            clip->lainkaColorShift, clip->lainkaGate);

        // Salva para motion blur / onion skin do próximo frame.
        if (clip->lainkaMotionBlur > 1e-6 || clip->lainkaOnionSkin > 1e-6)
            prevFrame = frame;

        // Escreve o frame no pipe.
        if (!session.writeFrame(frame, error, "LAINKA"))
            return {};
    }

    // Finaliza o ffmpeg.
    if (!session.finish(error, "LAINKA"))
        return {};

    // Move o arquivo para um local permanente (o QTemporaryDir será destruído).
    return prerender::publishToCache(
        tmpPath, QStringLiteral("lainka"),
        QStringLiteral("lainka_%1.mov").arg(clipId.left(8)), error, "LAINKA");
}

QHash<QString, QString> LainkaRenderer::renderAll(const Project& project,
                                                   int outputFps,
                                                   const LainkaProgressFn& progress,
                                                   QString* error) {
    QHash<QString, QString> result;

    // Coleta todos os clipes com LAINKA.
    QStringList lainkaClipIds;
    for (const Track& t : project.videoTracks)
        for (const Clip& c : t.clips)
            if (c.lainkaEnabled)
                lainkaClipIds.append(c.id);

    if (lainkaClipIds.isEmpty()) return result;

    const int total = lainkaClipIds.size();
    for (int i = 0; i < total; ++i) {
        if (progress && !progress(i, total)) {
            if (error) *error = "Pré-renderização LAINKA cancelada";
            return {};
        }

        QString clipError;
        const QString path = renderClip(project, lainkaClipIds[i], outputFps,
                                         nullptr, &clipError);
        if (!clipError.isEmpty()) {
            qWarning() << "LAINKA render error for clip" << lainkaClipIds[i] << ":" << clipError;
            if (error) *error = clipError;
            continue;
        }
        if (!path.isEmpty())
            result.insert(lainkaClipIds[i], path);
    }

    return result;
}
