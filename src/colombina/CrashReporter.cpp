// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// _GNU_SOURCE antes de qualquer include: precisamos de REG_RIP e dos gregs de
// ucontext.h, e de dladdr() de dlfcn.h.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "CrashReporter.h"
#include "version.h" // via src/ no include path público de colombina

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <ucontext.h>
#include <unistd.h>

// Preenchidos pelo CMake (git rev-parse --short HEAD e hora do build). Sem
// eles (ex.: compilando o .cpp na mão) o relatório ainda funciona.
#ifndef PIERROT_GIT_COMMIT
#define PIERROT_GIT_COMMIT "desconhecido"
#endif
#ifndef PIERROT_BUILD_TIME
#define PIERROT_BUILD_TIME __DATE__ " " __TIME__
#endif

namespace {

constexpr int kMaxTrackedThreads = 32;
constexpr int kMaxFrames = 64;

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

// Lê um campo de /proc/self/task/<tid>/<field>; deixa "?" se indisponível.
void readThreadFile(unsigned long tid, const char* field, char* buf, int size) {
    char path[160];
    std::snprintf(path, sizeof(path), "/proc/self/task/%lu/%s", tid, field);
    if (!readSmallFile(path, buf, size)) {
        buf[0] = '?';
        buf[1] = '\0';
    }
}

const ThreadSlot* findSlotForTid(unsigned long tid) {
    for (int i = 0; i < kMaxTrackedThreads; ++i) {
        if (g_threads[i].tid.load(std::memory_order_acquire) == tid)
            return &g_threads[i];
    }
    return nullptr;
}

// Escreve o inventário: uma linha por thread do processo. Usa só stdio e
// open/read — sem QString — porque roda de dentro do handler. As threads do
// Pierrot aparecem com o nome/atividade registrados; TODAS as outras do
// processo (pool do Qt, threads internas do FFmpeg, drivers VAAPI) também
// saem, com o wchan do kernel, para não esconder as suspectas de um travamento.
void writeThreadDump(FILE* f) {
    std::fprintf(f, "\n--- Threads (%d registradas / max %d) ---\n",
                 g_threadCount.load(std::memory_order_relaxed), kMaxTrackedThreads);

    const unsigned long selfTid = currentTid();
    DIR* d = ::opendir("/proc/self/task");
    if (!d) {
        std::fprintf(f, "  (não foi possível ler /proc/self/task)\n");
        return;
    }
    while (const dirent* e = ::readdir(d)) {
        char* end = nullptr;
        const unsigned long tid = std::strtoul(e->d_name, &end, 10);
        if (!end || *end != '\0' || tid == 0) continue; // pula "." e ".."

        char comm[64] = "?";
        char wchan[64] = "?";
        readThreadFile(tid, "comm", comm, sizeof(comm));
        readThreadFile(tid, "wchan", wchan, sizeof(wchan));

        const char* marker = (tid == selfTid) ? "   [CRESCEU AQUI]" : "";
        const ThreadSlot* slot = findSlotForTid(tid);
        if (slot) {
            const char* name = slot->name.load(std::memory_order_relaxed);
            const char* act  = slot->activity.load(std::memory_order_relaxed);
            std::fprintf(f, "  tid=%-8lu %-16s wchan=%-20s%s%s%s\n",
                         tid, name ? name : comm, wchan,
                         act ? " — " : "", act ? act : "", marker);
        } else {
            std::fprintf(f, "  tid=%-8lu %-16s wchan=%-20s [NÃO REGISTRADA]%s\n",
                         tid, comm, wchan, marker);
        }
    }
    ::closedir(d);
}

// ── Simbolização ───────────────────────────────────────────────────────────
// Com o executável linkado com -rdynamic, dladdr() resolve também os símbolos
// internos do Pierrot (não só os de libc/Qt), então o relatório sai com nomes
// de função em vez de meros "+0x...". O nome C++ é desmangado.
void formatFrame(char* out, size_t n, int idx, void* addr) {
    Dl_info info;
    if (::dladdr(addr, &info) && info.dli_fname) {
        const unsigned long off = (unsigned long)addr - (unsigned long)info.dli_fbase;
        int status = 0;
        char* dem = info.dli_sname
                        ? abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status)
                        : nullptr;
        const char* sym =
            (dem && status == 0) ? dem : (info.dli_sname ? info.dli_sname : "??");
        std::snprintf(out, n, "  #%-2d  %p  %s(+0x%lx)  %s",
                      idx, addr, info.dli_fname, off, sym);
        if (dem) std::free(dem);
    } else {
        std::snprintf(out, n, "  #%-2d  %p  ?? (módulo desconhecido)", idx, addr);
    }
}

// Base de carga do próprio executável, para reconhecer os frames que dá para
// simbolizar com addr2line direto no binário (os de libs precisam do .so).
void* selfBase() {
    Dl_info info;
    if (::dladdr(reinterpret_cast<void*>(&writeThreadDump), &info))
        return info.dli_fbase;
    return nullptr;
}

#if defined(__x86_64__)
// Registradores da thread que quebrou, do ucontext entregue pelo kernel. É o
// que aconteceu no instante exato da queda (o backtrace pode não incluir o
// valor das variáveis que causaram o bug).
void writeRegisters(FILE* f, void* ucontext) {
    const ucontext_t* uc = static_cast<const ucontext_t*>(ucontext);
    if (!uc) return;
    const greg_t* g = uc->uc_mcontext.gregs;
    std::fprintf(f,
                 "\n--- Registradores (thread que quebrou) ---\n"
                 "  RIP=%016llx  RSP=%016llx  RBP=%016llx\n"
                 "  RAX=%016llx  RBX=%016llx  RCX=%016llx  RDX=%016llx\n"
                 "  RSI=%016llx  RDI=%016llx  R08=%016llx  R09=%016llx\n"
                 "  R10=%016llx  R11=%016llx  R12=%016llx  R13=%016llx\n"
                 "  R14=%016llx  R15=%016llx  RFL=%016llx\n",
                 (unsigned long long)g[REG_RIP], (unsigned long long)g[REG_RSP],
                 (unsigned long long)g[REG_RBP], (unsigned long long)g[REG_RAX],
                 (unsigned long long)g[REG_RBX], (unsigned long long)g[REG_RCX],
                 (unsigned long long)g[REG_RDX], (unsigned long long)g[REG_RSI],
                 (unsigned long long)g[REG_RDI], (unsigned long long)g[REG_R8],
                 (unsigned long long)g[REG_R9],  (unsigned long long)g[REG_R10],
                 (unsigned long long)g[REG_R11], (unsigned long long)g[REG_R12],
                 (unsigned long long)g[REG_R13], (unsigned long long)g[REG_R14],
                 (unsigned long long)g[REG_R15], (unsigned long long)g[REG_EFL]);
}
#else
void writeRegisters(FILE* f, void*) {
    std::fprintf(f, "\n--- Registradores (thread que quebrou) ---\n"
                    "  (arquitetura sem dump de registradores)\n");
}
#endif

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (acesso inválido à memória)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGFPE:  return "SIGFPE (erro de ponto flutuante)";
        case SIGILL:  return "SIGILL (instrução ilegal)";
        case SIGBUS:  return "SIGBUS (erro de barramento)";
        case SIGTRAP: return "SIGTRAP (breakpoint)";
        default:      return "SINAL";
    }
}

// Motivo do SIGSEGV, a partir do si_code.
const char* segvReason(const siginfo_t* info) {
    if (!info) return "motivo desconhecido";
    switch (info->si_code) {
        case SEGV_MAPERR: return "endereço não mapeado (SEGV_MAPERR)";
        case SEGV_ACCERR: return "sem permissão de acesso (SEGV_ACCERR)";
        default:          return "motivo desconhecido";
    }
}

// Diagnóstico específico do sinal — nada de dica genérica de "decode
// concorrente" que não se aplica a um abort de asserção na thread da GUI.
void writeDiagnosis(FILE* f, int sig, const siginfo_t* info) {
    std::fprintf(f, "\n--- Diagnóstico ---\n");
    switch (sig) {
        case SIGABRT:
            std::fprintf(f,
                "SIGABRT: o processo chamou abort(). Causas típicas:\n"
                "  • asserção falhada — ex.: std::clamp(v, lo, hi) com lo > hi\n"
                "    (o libstdc++ aborta em std::__glibcxx_assert_fail);\n"
                "  • exceção C++ não tratada → std::terminate();\n"
                "  • assert() do C ou abort() explícito.\n"
                "O gatilho é a função logo ACIMA de abort() no backtrace — o\n"
                "primeiro frame do executável (não libstdc++/libc).\n");
            break;
        case SIGSEGV:
            std::fprintf(f,
                "SIGSEGV: acesso a endereço inválido (%s).\n"
                "  • endereço pequeno/próximo de 0 = ponteiro nulo ou não inicializado;\n"
                "  • endereço perto de um frame de pilha = buffer estourado / uso após free;\n"
                "  • o frame #0 costuma ser a função que dereferenciou o ponteiro.\n",
                segvReason(info));
            break;
        case SIGFPE:
            std::fprintf(f, "SIGFPE: operação aritmética inválida (divisão por zero, inclusive inteira).\n");
            break;
        case SIGILL:
            std::fprintf(f, "SIGILL: instrução ilegal — binário corrompido ou otimização/CPU incompatível.\n");
            break;
        case SIGBUS:
            std::fprintf(f, "SIGBUS: memória mapeada inválida (mmap/arquivo truncado, alinhamento).\n");
            break;
        default:
            std::fprintf(f, "Sinal %d.\n", sig);
            break;
    }
}

// Descarrega o relatório inteiro. Seguro para chamar de um handler de sinal
// (usa só stdio/open/read; a simbolização aloca, mas o processo já está morto).
void writeCrashReport(int sig, const siginfo_t* info, void* ucontext) {
    QString dir = QDir::homePath();
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QDir::tempPath();
    const QDateTime now = QDateTime::currentDateTime();
    const QString path =
        dir + "/Pierrot-crash-" + now.toString("yyyyMMdd-HHmmss") + ".txt";

    FILE* f = std::fopen(path.toLocal8Bit().constData(), "w");
    if (!f)
        f = std::fopen((QDir::tempPath() + "/Pierrot-crash.txt").toLocal8Bit().constData(), "w");
    if (!f) return;

    char exe[1024] = "";
    const ssize_t exeLen = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (exeLen > 0) exe[exeLen] = '\0';

    const unsigned long selfTid = currentTid();
    const ThreadSlot* selfSlot = findSlotForTid(selfTid);
    const char* selfName = (selfSlot && selfSlot->name.load(std::memory_order_relaxed))
                               ? selfSlot->name.load(std::memory_order_relaxed)
                               : "(sem nome)";

    std::fprintf(f, "=== Pierrot — Relatório de Crash ===\n");
    std::fprintf(f, "Data/Hora: %s\n",
                 now.toString("yyyy-MM-dd HH:mm:ss").toLocal8Bit().constData());
    std::fprintf(f, "Versão: %s  (commit %s, build %s)\n",
                 PIERROT_VERSION, PIERROT_GIT_COMMIT, PIERROT_BUILD_TIME);
    std::fprintf(f, "Sinal: %s (%d)\n", signalName(sig), sig);
    std::fprintf(f, "Thread que quebrou: tid=%lu (%s)\n", selfTid, selfName);
    std::fprintf(f, "PID: %d\n", (int)::getpid());
    if (QCoreApplication::instance())
        std::fprintf(f, "Qt: %s\n", qVersion());
    if (exe[0])
        std::fprintf(f, "Executável: %s\n", exe);

    struct utsname uts;
    if (::uname(&uts) == 0)
        std::fprintf(f, "Kernel: %s %s %s\n", uts.sysname, uts.release, uts.machine);
    const long ncpu = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 0) std::fprintf(f, "CPUs: %ld\n", ncpu);
    char cwd[1024] = "";
    if (::getcwd(cwd, sizeof(cwd))) std::fprintf(f, "CWD: %s\n", cwd);

    // Endereço da falha (só faz sentido para os sinais que carregam si_addr).
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) {
        if (info)
            std::fprintf(f, "Endereço da falha: %p  (%s)\n", info->si_addr,
                         sig == SIGSEGV ? segvReason(info) : "");
    }

    // Backtrace com nomes. Guarda os offsets dos frames do próprio executável
    // para montar, no fim, o comando addr2line já pronto.
    std::fprintf(f, "\n--- Backtrace (thread que quebrou) ---\n");
    void* bt[kMaxFrames];
    const int n = ::backtrace(bt, kMaxFrames);
    void* base = selfBase();
    unsigned long exeOffsets[kMaxFrames];
    int exeCount = 0;
    char line[1024];
    for (int i = 0; i < n; ++i) {
        formatFrame(line, sizeof(line), i, bt[i]);
        std::fprintf(f, "%s\n", line);
        Dl_info fi;
        if (base && ::dladdr(bt[i], &fi) && fi.dli_fbase == base && exeCount < kMaxFrames)
            exeOffsets[exeCount++] = (unsigned long)bt[i] - (unsigned long)base;
    }

    if (exeCount > 0 && exe[0]) {
        std::fprintf(f, "\n--- Simbolizar com arquivo/linha ---\n  addr2line -f -C -e <%s>", exe);
        for (int i = 0; i < exeCount; ++i)
            std::fprintf(f, " 0x%lx", exeOffsets[i]);
        std::fprintf(f, "\n  (o binário precisa ter sido compilado com -g para dar a linha)\n");
    }

    writeRegisters(f, ucontext);
    writeThreadDump(f);
    writeDiagnosis(f, sig, info);
    std::fclose(f);
}

// Handler de sinal: captura o backtrace, grava o relatório e encerra com o
// código 128+sinal (o waitpid dos testes e do shell enxerga a causa).
void crashHandler(int sig, siginfo_t* info, void* ucontext) {
    if (g_seenSignal) _exit(128 + sig); // evita loop / reentrada
    g_seenSignal = sig;
    writeCrashReport(sig, info, ucontext);
    _exit(128 + sig);
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
    sa.sa_sigaction = crashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND; // após o handler, volta ao default
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
}
