// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/monitor/BgPrefetchWorker.h"

#include "colombina/CrashReporter.h"
#include "laartman/FFmpegDecoder.h"
#include "ui/monitor/DecoderPool.h"
#include "ui/monitor/PreviewDebug.h"

#include <QDebug>
#include <QElapsedTimer>

BgPrefetchWorker::BgPrefetchWorker(DecoderPool* pool, QObject* parent)
    : QObject(parent), m_pool(pool) {}

void BgPrefetchWorker::decode(const QString& path, double t, int maxW, double step) {
    CrashReporter::setActivity("prefetch do proximo clipe");
    static QElapsedTimer dbgClock;
    const bool dbgOn = playDbg();
    if (dbgOn && !dbgClock.isValid()) dbgClock.start();
    const qint64 dbgT0 = dbgOn ? dbgClock.nsecsElapsed() : 0;

    FFmpegDecoder* d = m_pool->acquire(path);
    if (!d) {
        // Pool sem slot ou o caminho está emprestado ao worker principal
        // (mesmo arquivo): prefetch é best-effort, o corte segue esperando
        // pelo acquire quente do FrameWorker.
        emit failed(path);
        return;
    }
    QImage frame0 = d->frameAt(t, maxW);
    QImage frame1;
    if (!frame0.isNull() && step > 0.0)
        frame1 = d->frameAt(t + step, maxW);
    m_pool->release(path);

    if (dbgOn) {
        const double dbgMs = (dbgClock.nsecsElapsed() - dbgT0) / 1000000.0;
        qDebug().noquote() << QStringLiteral("[w] PREFETCH(bg) t=%1 %2ms")
                  .arg(t, 0, 'f', 3)
                  .arg(dbgMs, 0, 'f', 1);
    }
    emit done(path, t, maxW, frame0, frame1);
}

void BgPrefetchWorker::warm(const QString& path, double t, int maxW) {
    CrashReporter::setActivity("aquecendo clipe a frente");
    static QElapsedTimer dbgClock;
    const bool dbgOn = playDbg();
    if (dbgOn && !dbgClock.isValid()) dbgClock.start();
    const qint64 dbgT0 = dbgOn ? dbgClock.nsecsElapsed() : 0;

    FFmpegDecoder* d = m_pool->acquire(path);
    if (d) {
        d->frameAt(t, maxW); // posiciona o decoder; soft se já quente
        m_pool->release(path);
        if (dbgOn) {
            const double dbgMs = (dbgClock.nsecsElapsed() - dbgT0) / 1000000.0;
            qDebug().noquote() << QStringLiteral("[w] WARM(bg) t=%1 %2ms")
                      .arg(t, 0, 'f', 3)
                      .arg(dbgMs, 0, 'f', 1);
        }
    }
    emit warmed(path);
}
