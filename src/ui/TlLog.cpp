// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TlLog.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QStringList>

namespace {
QMutex g_mutex;
QElapsedTimer g_clock;
bool g_started = false;
QStringList g_ring;

QFile* file() {
    static QFile* f = nullptr;
    if (!f) {
        f = new QFile(QDir::homePath() + QStringLiteral("/Pierrot-timeline-debug.txt"));
        if (f->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            f->write("Pierrot — diagnóstico da timeline (agulha/view)\n"
                     "Cada bloco 'DUMP' marca o instante de um salto detectado.\n\n");
        else
            f = nullptr;
    }
    return f;
}
}

namespace TlLog {

void note(const QString& msg) {
    QMutexLocker l(&g_mutex);
    if (!g_started) { g_clock.start(); g_started = true; }
    g_ring.append(QStringLiteral("[%1] %2")
                      .arg(g_clock.elapsed(), 8, 10, QLatin1Char('0'))
                      .arg(msg));
    while (g_ring.size() > 64)
        g_ring.removeFirst();
}

void dump(const QString& reason) {
    QMutexLocker l(&g_mutex);
    if (!g_started) { g_clock.start(); g_started = true; }
    QFile* f = file();
    if (f) {
        f->write(QStringLiteral("\n===== DUMP: %1 (t=%2ms) =====\n")
                     .arg(reason)
                     .arg(g_clock.elapsed())
                     .toUtf8());
        for (const QString& s : g_ring)
            f->write((s + QLatin1Char('\n')).toUtf8());
        f->flush();
    }
}
}
