// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "LainkaRenderer.h"
#include "LainkaFx.h"
#include "models/Project.h"
#include "ffmpeg/FFmpegDecoder.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QElapsedTimer>
#include <QDebug>
#include <QStandardPaths>

// Decodifica um frame do vídeo no tempo dado (segundos), retornando QImage RGB.
static QImage decodeFrame(FFmpegDecoder& dec, double seconds) {
    return dec.frameAt(seconds);
}

// Escreve um QImage RGBA no pipe do ffmpeg (rawvideo).
static bool writeFrame(QProcess& ffmpeg, const QImage& img) {
    if (img.isNull()) return false;
    // Converte para RGBA32 se necessário (rawvideo espera RGBA).
    QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    return ffmpeg.write(QByteArray(reinterpret_cast<const char*>(rgba.constBits()),
                                   rgba.sizeInBytes())) != -1;
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

    FFmpegDecoder dec;
    if (!dec.open(mi->filePath)) {
        if (error) *error = QString("Não foi possível abrir %1").arg(mi->filePath);
        return {};
    }

    const double srcFps = dec.fps() > 0 ? dec.fps() : (double)project.fps;
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
        QString("lainka_%1.mp4").arg(clipId.left(8)));

    // Inicia ffmpeg para codificação via pipe.
    QProcess ffmpeg;
    QStringList args;
    args << "-y"
         << "-f" << "rawvideo"
         << "-pixel_format" << "rgba"
         << "-video_size" << QString("%1x%2").arg(clipW).arg(clipH)
         << "-framerate" << QString::number(outputFps)
         << "-i" << "pipe:0"
         << "-c:v" << "libx264"
         << "-preset" << "medium"
         << "-crf" << "18"
         << "-pix_fmt" << "yuv420p"
         << "-movflags" << "+faststart"
         << tmpPath;

    ffmpeg.start("ffmpeg", args);
    if (!ffmpeg.waitForStarted(5000)) {
        if (error) *error = "Não foi possível iniciar o ffmpeg para pré-renderização LAINKA";
        return {};
    }

    // Estado para motion blur (frame anterior).
    QImage prevFrame;

    // Decodifica e processa frame a frame.
    for (int f = 0; f < totalFrames; ++f) {
        // Verifica cancelamento.
        if (progress && !progress(f, totalFrames)) {
            ffmpeg.kill();
            ffmpeg.waitForFinished(3000);
            if (error) *error = "Pré-renderização LAINKA cancelada";
            return {};
        }

        // Tempo na timeline e tempo de origem.
        // clip->dur é da fonte; timelineDur = dur/speed.
        // Frame f → tempo na timeline = f/outputFps.
        // Tempo na fonte = in + (f/outputFps) * speed.
        const double srcT = clip->in + (double)f / outputFps * speed;

        // Quantiza o tempo para simular skip de frames.
        const double fxT = LainkaFx::lainkaQuantizeTime(
            srcT, clip->lainkaSkip, srcFps);

        // Decodifica o frame.
        QImage frame = decodeFrame(dec, fxT);
        if (frame.isNull()) {
            // Frame não decodificado: usa preto.
            frame = QImage(clipW, clipH, QImage::Format_RGBA8888);
            frame.fill(Qt::black);
        }

        // Redimensiona se a resolução do frame diverge da esperada.
        if (frame.width() != clipW || frame.height() != clipH) {
            frame = frame.scaled(clipW, clipH, Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
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
            prevFrame);

        // Salva para motion blur / onion skin do próximo frame.
        if (clip->lainkaMotionBlur > 1e-6 || clip->lainkaOnionSkin > 1e-6)
            prevFrame = frame;

        // Converte para RGBA e escreve no pipe.
        QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
        if (!writeFrame(ffmpeg, rgba)) {
            ffmpeg.kill();
            ffmpeg.waitForFinished(3000);
            if (error) *error = "Erro ao escrever frame no pipe ffmpeg";
            return {};
        }
    }

    // Finaliza o ffmpeg.
    ffmpeg.closeWriteChannel();
    ffmpeg.waitForFinished(30000);

    if (ffmpeg.exitCode() != 0) {
        const QString stderr = QString::fromUtf8(ffmpeg.readAllStandardError());
        if (error) *error = QString("ffmpeg falhou na pré-renderização LAINKA: %1").arg(stderr.left(500));
        return {};
    }

    // Move o arquivo para um local permanente (o QTemporaryDir será destruído).
    const QString permanentDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation) + QStringLiteral("/lainka");
    QDir().mkpath(permanentDir);
    const QString permanentPath = permanentDir + QStringLiteral("/")
        + QStringLiteral("lainka_%1.mp4").arg(clipId.left(8));
    // Remove arquivo anterior se existir.
    QFile::remove(permanentPath);
    if (!QFile::rename(tmpPath, permanentPath)) {
        // Fallback: copia se rename falhar (mesmo filesystem).
        if (!QFile::copy(tmpPath, permanentPath)) {
            if (error) *error = QString("Não foi possível mover arquivo LAINKA para %1").arg(permanentPath);
            return {};
        }
    }
    return permanentPath;
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
