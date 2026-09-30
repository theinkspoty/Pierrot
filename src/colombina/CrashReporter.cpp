// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "CrashReporter.h"
#include "version.h" // via src/ no include path público de colombina

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QProcessEnvironment>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <execinfo.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>

namespace {

volatile sig_atomic_t g_seenSignal = 0;

// TID do SO. NÃO usar pthread_self(): no glibc ele devolve um ponteiro para o
// descritor da thread (0x7f...), não o número do TID, então a comparação com
// as entradas de /proc/self/task nunca casaria e a thread principal sairia
// duplicada — uma como registrada, outra como órfã.
inline unsigned long currentTid() {
    return (unsigned long)::syscall(SYS_gettid);
}

// ── Registro de threads ────────────────────────────────────────────────────
// Slots de tamanho fixo, escritos só pela thread dona e lidos (só leitura) de
// dentro do handler de sinal. Por isso `name`/`activity` são literais
// estáticos guardados em std::atomic<const char*>: o handler não pode alocar
// nem tocar QString, e reescrever o ponteiro enquanto o relatório lê é seguro.
constexpr int kMaxTrackedThreads = 32;

struct ThreadSlot {
    std::atomic<unsigned long> tid;     // 0 = livre; publicado por último
    std::atomic<const char*>   name;    // literal estático
    std::atomic<const char*>   activity; // literal estático ou nullptr
};

ThreadSlot g_threads[kMaxTrackedThreads];
std::atomic<int> g_threadCount{0};

// Slot da thread corrente. `inline thread_local` evita o custo do TLS dinâmico.
inline thread_local ThreadSlot* t_slot = nullptr;

// Lê um arquivo pequeno (comm, wchan) num buffer de pilha. Só usa open/read/
// close: nada que possa bloquear esperando o malloc, o que importa dentro de
// um handler de sinal.
bool readSmallFile(const char* path, char* buf, int size) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    int total = 0;
    while (total < size - 1) {
        const ssize_t n = ::read(fd, buf + total, size_t(size - 1 - total));
        if (n <= 0) break;
        total += int(n);
    }
    ::close(fd);
    buf[total] = '\0';
    // /proc costuma terminar com '\n'.
    while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == ' '))
        buf[--total] = '\0';
    return total > 0;
}

const ThreadSlot* findSlotForTid(unsigned long tid) {
    for (int i = 0; i < kMaxTrackedThreads; ++i) {
        if (g_threads[i].tid.load(std::memory_order_acquire) == tid)
            return &g_threads[i];
    }
    return nullptr;
}

// Escreve o inventário: uma linha por thread do processo. Usa só stdio e
// open/read — sem QString — porque roda de dentro do handler.
void writeThreadDump(FILE* f) {
    std::fprintf(f, "\n--- Threads (%d registradas / max %d) ---\n",
                 g_threadCount.load(std::memory_order_relaxed), kMaxTrackedThreads);

    const unsigned long selfTid = currentTid();

    // Registro do Pierrot primeiro: são as threads com nome conhecido.
    for (int i = 0; i < kMaxTrackedThreads; ++i) {
        const unsigned long tid = g_threads[i].tid.load(std::memory_order_acquire);
        if (!tid) continue;
        const char* name = g_threads[i].name.load(std::memory_order_relaxed);
        const char* act  = g_threads[i].activity.load(std::memory_order_relaxed);
        std::fprintf(f, "  tid=%-8lu %s%s%s%s\n", tid,
                     name ? name : "(sem nome)",
                     act ? " — " : "", act ? act : "",
                     tid == selfTid ? "   [CRESCEU AQUI]" : "");
    }

    // Depois, TODAS as threads do processo, para pegar as que ninguém
    // registrou (pool do Qt, threads internas do FFmpeg, dos drivers VAAPI).
    // Sem isso o relatório esconde justamente as suspects do decode concorrente.
    DIR* d = ::opendir("/proc/self/task");
    if (!d) {
        std::fprintf(f, "  (não foi possível ler /proc/self/task)\n");
        return;
    }
    int orphans = 0;
    while (const dirent* e = ::readdir(d)) {
        char* end = nullptr;
        const unsigned long tid = std::strtoul(e->d_name, &end, 10);
        if (!end || *end != '\0' || tid == 0) continue; // pula "." e ".."

        if (findSlotForTid(tid)) continue; // já saiu acima

        char path[128];
        char comm[64] = "?";
        char wchan[64] = "?";
        std::snprintf(path, sizeof(path), "/proc/self/task/%lu/comm", tid);
        readSmallFile(path, comm, sizeof(comm));
        std::snprintf(path, sizeof(path), "/proc/self/task/%lu/wchan", tid);
        readSmallFile(path, wchan, sizeof(wchan));

        std::fprintf(f, "  tid=%-8lu %-15s wchan=%-20s [NÃO REGISTRADA]%s\n",
                     tid, comm, wchan, tid == selfTid ? "  [CRESCEU AQUI]" : "");
        ++orphans;
    }
    ::closedir(d);
    if (orphans)
        std::fprintf(f, "  (%d thread(s) sem registro do Pierrot)\n", orphans);
}

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

    std::fprintf(f, "\n--- Backtrace (thread que quebrou) ---\n");
    for (int i = 0; i < btSize; ++i) {
        std::fprintf(f, "  #%d  %p  %s\n", i, backtraceArr[i],
                     (btSymbols && btSymbols[i]) ? btSymbols[i] : "?");
    }

    // O resto do dump usa só stdio: roda mesmo se o crash foi num lugar em
    // que o Qt já não pode ser tocado com segurança.
    writeThreadDump(f);

    std::fprintf(f, "\nDica: o SIGSEGV em decode concorrente quase sempre é a\n"
                    "thread acima marcada [CRESCEU AQUI] reagindo a um buffer\n"
                    "liberado/duplicado por outra; o estado das outras threads\n"
                    "no momento da queda é o que fecha o diagnóstico.\n");
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

void CrashReporter::registerThread(const char* name) {
    if (t_slot) { // já registrada nesta thread: só troca o nome
        t_slot->name.store(name, std::memory_order_relaxed);
        return;
    }
    const unsigned long selfTid = currentTid();
    for (int i = 0; i < kMaxTrackedThreads; ++i) {
        unsigned long expected = 0;
        if (!g_threads[i].tid.compare_exchange_strong(
                expected, selfTid, std::memory_order_acq_rel,
                std::memory_order_relaxed))
            continue;
        g_threads[i].name.store(name, std::memory_order_relaxed);
        g_threadCount.fetch_add(1, std::memory_order_relaxed);
        t_slot = &g_threads[i];
        return;
    }
    // Registro cheio: a thread segue existindo, só não entra no relatório.
    t_slot = nullptr;
}

void CrashReporter::unregisterThread() {
    if (!t_slot) return;
    g_threadCount.fetch_sub(1, std::memory_order_relaxed);
    t_slot->activity.store(nullptr, std::memory_order_relaxed);
    t_slot->name.store(nullptr, std::memory_order_relaxed);
    t_slot->tid.store(0, std::memory_order_release); // libera por último
    t_slot = nullptr;
}

void CrashReporter::setActivity(const char* activity) {
    if (t_slot) t_slot->activity.store(activity, std::memory_order_relaxed);
}

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
