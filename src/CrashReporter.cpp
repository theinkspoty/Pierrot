// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "CrashReporter.h"
#include "version.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QProcessEnvironment>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <pthread.h>

namespace {

volatile sig_atomic_t g_seenSignal = 0;

// Descarrega o backtrace para um arquivo. Seguro para chamar de um handler de
// sinal (não aloca, não usa Qt).
void writeCrashReport(const char* signalName, void** backtraceArr,
                      int btSize, char** btSymbols) {
    // Salva na home (ou /tmp se não houver home gravável).
    QString dir = QDir::homePath();
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QDir::tempPath();
    const QString path = dir + "/Pierrot-crash-" +
                         QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") +
                         ".txt";

    FILE* f = std::fopen(path.toLocal8Bit().constData(), "w");
    if (!f) f = std::fopen((QDir::tempPath() + "/Pierrot-crash.txt").toLocal8Bit().constData(), "w");
    if (!f) return;

    std::fprintf(f, "=== Pierrot — Relatório de Crash ===\n");
    std::fprintf(f, "Data/Hora: %s\n", QDateTime::currentDateTime()
                                          .toString("yyyy-MM-dd HH:mm:ss").toLocal8Bit().constData());
    std::fprintf(f, "Versão: %s\n", PIERROT_VERSION);
    std::fprintf(f, "Sinal: %s (%d)\n", signalName, (int)g_seenSignal);
    if (QCoreApplication::instance())
        std::fprintf(f, "Qt: %s\n", qVersion());
    std::fprintf(f, "PID: %d\n", (int)getpid());

    std::fprintf(f, "\n--- Backtrace ---\n");
    for (int i = 0; i < btSize; ++i) {
        std::fprintf(f, "  #%d  %p  %s\n", i, backtraceArr[i],
                     (btSymbols && btSymbols[i]) ? btSymbols[i] : "?");
    }
    std::fclose(f);
}

// Handler de sinal: captura o backtrace e grava o relatório, depois repassa o
// handler original (que aborta) para o sinal ser tratado normalmente.
void crashHandler(int sig) {
    if (g_seenSignal) _exit(128 + sig); // evita loop / reentrada
    g_seenSignal = sig;

    const char* name = "SINAL";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV (acesso inválido à memória)"; break;
        case SIGABRT: name = "SIGABRT (abort)"; break;
        case SIGFPE:  name = "SIGFPE (erro de ponto flutuante)"; break;
        case SIGILL:  name = "SIGILL (instrução ilegal)"; break;
        case SIGBUS:  name = "SIGBUS (erro de barramento)"; break;
        default: break;
    }

    void* bt[64];
    const int n = backtrace(bt, 64);
    char** syms = backtrace_symbols(bt, n);
    writeCrashReport(name, bt, n, syms);
    if (syms) free(syms);
    _exit(128 + sig); // encerra com o código de sinal
}

} // namespace

QString CrashReporter::nextReportPath() {
    return QDir::homePath() + "/Pierrot-crash-" +
           QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".txt";
}

QStringList CrashReporter::existingReports() {
    const QString dir = QDir::homePath().isEmpty() ? QDir::tempPath() : QDir::homePath();
    QStringList files;
    const QDir d(dir);
    for (const QFileInfo& fi : d.entryInfoList(QStringList{QStringLiteral("Pierrot-crash-*.txt")},
                                               QDir::Files))
        files.append(fi.absoluteFilePath());
    return files;
}

void CrashReporter::install() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND; // após o handler, volta ao default (aborta)
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}
