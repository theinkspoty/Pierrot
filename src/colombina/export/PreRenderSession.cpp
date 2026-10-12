// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "PreRenderSession.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace prerender {

PreRenderSession::PreRenderSession(int width, int height, int fps,
                                   const QString& tmpPath)
    : m_tmpPath(tmpPath), m_w(width), m_h(height), m_fps(fps) {}

PreRenderSession::~PreRenderSession() {
    abort();
}

bool PreRenderSession::start(QString* error, const QString& label) {
    QStringList args;
    args << "-y"
         << "-f" << "rawvideo"
         << "-pixel_format" << "rgba"
         << "-video_size" << QString("%1x%2").arg(m_w).arg(m_h)
         << "-framerate" << QString::number(m_fps)
         << "-i" << "pipe:0"
         << "-c:v" << "libx264"
         << "-preset" << "medium"
         << "-crf" << "18"
         << "-pix_fmt" << "yuv420p"
         << "-movflags" << "+faststart"
         << m_tmpPath;

    m_proc.start("ffmpeg", args);
    if (!m_proc.waitForStarted(5000)) {
        if (error)
            *error = QString("Não foi possível iniciar o ffmpeg para pré-renderização %1")
                         .arg(label);
        return false;
    }
    return true;
}

bool PreRenderSession::writeFrame(const QImage& frame, QString* error,
                                  const QString& label) {
    if (frame.isNull()) return false;
    const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
    if (m_proc.write(reinterpret_cast<const char*>(rgba.constBits()),
                     rgba.sizeInBytes()) == -1) {
        abort();
        if (error)
            *error = QString("Erro ao escrever frame no pipe ffmpeg (%1)").arg(label);
        return false;
    }
    return true;
}

bool PreRenderSession::finish(QString* error, const QString& label) {
    m_proc.closeWriteChannel();
    m_proc.waitForFinished(30000);
    if (m_proc.exitCode() != 0) {
        const QString stderr = QString::fromUtf8(m_proc.readAllStandardError());
        if (error)
            *error = QString("ffmpeg falhou na pré-renderização %1: %2")
                         .arg(label, stderr.left(500));
        return false;
    }
    return true;
}

void PreRenderSession::abort() {
    if (m_proc.state() != QProcess::NotRunning) {
        m_proc.kill();
        m_proc.waitForFinished(3000);
    }
}

QString publishToCache(const QString& tmpPath, const QString& dirSuffix,
                       const QString& fileName, QString* error,
                       const QString& label) {
    const QString permanentDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation) + QStringLiteral("/") + dirSuffix;
    QDir().mkpath(permanentDir);
    const QString permanentPath = permanentDir + QStringLiteral("/") + fileName;
    QFile::remove(permanentPath);
    if (!QFile::rename(tmpPath, permanentPath) &&
        !QFile::copy(tmpPath, permanentPath)) {
        if (error)
            *error = QString("Não foi possível mover arquivo %1 para %2")
                         .arg(label, permanentPath);
        return QString();
    }
    return permanentPath;
}

} // namespace prerender
