// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// O relatório de crash tem que responder "o que cada thread estava fazendo
// quando o processo caiu", que é a única informação que localiza um SIGSEGV em
// decode concorrente. Este teste forka um filho que morre de verdade e confere
// o arquivo gerado.
//
// Ele existe porque dois bugs reais só apareceram com um crash de verdade:
//   1. o registro guardava pthread_self(), que no glibc é um ponteiro para o
//      descritor da thread (0x7f...), não o TID — a comparação com
//      /proc/self/task nunca casava e a thread principal saía duplicada;
//   2. o registro estava no run() do QThread, mas o Qt emite `started` ANTES
//      de run() — um worker ligado a `started` que bloqueia ali nunca
//      chegava ao registro, sumindo justamente do relatório.
// Nenhum teste que não causasse um SIGSEGV real pegaria nenhum dos dois.

#include <QtTest>

#include "colombina/CrashReporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Worker que bloqueia para sempre no slot ligado a `started` — o mesmo padrão
// do ExportBuildWorker, e que expõe o bug (2) acima.
class BlockingWorker : public QObject {
    Q_OBJECT
public slots:
    void run() {
        CrashReporter::setActivity("decodificando quadro do preview");
        for (;;) QThread::msleep(20);
    }
};

class tst_crashreporter : public QObject {
    Q_OBJECT

private slots:
    void reportListsRegisteredThreads();
    void reportMarksCrashingThread();
    void reportListsUnregisteredThreads();

private:
    // Caminho do relatório mais recente em `dir`, ou QString() se não houver.
    static QString latestReport(const QString& dir) {
        QString best;
        qint64 bestT = -1;
        const QFileInfoList files = QDir(dir).entryInfoList(
            QStringList{QStringLiteral("Pierrot-crash-*.txt")}, QDir::Files);
        for (const QFileInfo& fi : files) {
            if (fi.lastModified().toMSecsSinceEpoch() >= bestT) {
                bestT = fi.lastModified().toMSecsSinceEpoch();
                best = fi.absoluteFilePath();
            }
        }
        return best;
    }
    // Lê o relatório e falha o teste (com o texto) se ele não existir.
    static QString readReport(const QString& dir) {
        const QString path = latestReport(dir);
        if (path.isEmpty()) return QString();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        return QString::fromUtf8(f.readAll());
    }
};

void tst_crashreporter::reportListsRegisteredThreads() {
    QTemporaryDir home;
    QVERIFY(home.isValid());

    // fork() direto: o filho instala o handler e mata a si mesmo com SIGSEGV,
    // exercitando exatamente o caminho do app real. Sem QCoreApplication — o
    // handler não precisa de event loop e o _exit() evita qualquer cleanup.
    ::fflush(nullptr);
    const pid_t pid = ::fork();
    QVERIFY(pid >= 0);

    if (pid == 0) {
        ::setenv("HOME", home.path().toLocal8Bit().constData(), 1);
        CrashReporter::install();
        CrashReporter::registerThread("ui-principal");
        BlockingWorker w1, w2;
        CrashReporter::TrackedThread t1("preview-frame");
        CrashReporter::TrackedThread t2("cache-picos");
        w1.moveToThread(&t1);
        w2.moveToThread(&t2);
        QObject::connect(&t1, &QThread::started, &w1, &BlockingWorker::run);
        QObject::connect(&t2, &QThread::started, &w2, &BlockingWorker::run);
        t1.start();
        t2.start();
        QThread::msleep(300); // deixa as threads registrarem
        volatile int* bad = nullptr;
        ::_exit(*bad ? 0 : 1); // inalcançável: o handler de SIGSEGV chama _exit
    }

    int status = 0;
    QCOMPARE(::waitpid(pid, &status, 0), pid);
    // O handler não deixa o processo morrer pelo sinal: ele escreve o relatório
    // e chama _exit(128 + sinal). Então o filho sai pelo NORMAL com código
    // 128+11 = 139, e é isso que a gente confere.
    QVERIFY(WIFEXITED(status));
    QCOMPARE(WEXITSTATUS(status), 128 + SIGSEGV);

    const QString text = readReport(home.path());
    QVERIFY2(!text.isEmpty(), "nenhum relatório de crash foi escrito");

    QVERIFY2(text.contains(QStringLiteral("preview-frame")),
             qPrintable(QStringLiteral("thread registrada ausente:\n%1").arg(text)));
    QVERIFY2(text.contains(QStringLiteral("cache-picos")),
             qPrintable(QStringLiteral("segunda thread registrada ausente:\n%1").arg(text)));
    QVERIFY2(text.contains(QStringLiteral("ui-principal")),
             qPrintable(QStringLiteral("thread principal ausente:\n%1").arg(text)));
    // A atividade é o que diz o que a thread fazia no instante da queda.
    QVERIFY2(text.contains(QStringLiteral("decodificando quadro do preview")),
             qPrintable(QStringLiteral("atividade da thread ausente:\n%1").arg(text)));
    // O backtrace da thread que quebrou continua lá.
    QVERIFY(text.contains(QStringLiteral("Backtrace")));
}

void tst_crashreporter::reportMarksCrashingThread() {
    QTemporaryDir home;
    QVERIFY(home.isValid());

    ::fflush(nullptr);
    const pid_t pid = ::fork();
    QVERIFY(pid >= 0);
    if (pid == 0) {
        ::setenv("HOME", home.path().toLocal8Bit().constData(), 1);
        // O filho grava o próprio TID antes de morrer: é contra isso que o
        // TID relatado é conferido. Um TID válido é o número que o /proc mostra
        // (< 2^22 nesta plataforma); pthread_self() devolveria um ponteiro
        // 0x7f... e a comparação com /proc/self/task nunca casaria.
        QFile tidFile(home.path() + QStringLiteral("/child-tid.txt"));
        if (tidFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
            tidFile.write(QByteArray::number((qlonglong)::syscall(SYS_gettid)));
        tidFile.close();

        CrashReporter::install();
        CrashReporter::registerThread("ui-principal");
        volatile int* bad = nullptr;
        ::_exit(*bad ? 0 : 1);
    }
    int status = 0;
    QCOMPARE(::waitpid(pid, &status, 0), pid);

    QFile tidFile(home.path() + QStringLiteral("/child-tid.txt"));
    QVERIFY2(tidFile.open(QIODevice::ReadOnly), "filho não deixou o TID registrado");
    const QString childTid = QString::fromUtf8(tidFile.readAll()).trimmed();
    tidFile.close();
    QVERIFY(!childTid.isEmpty());

    const QString text = readReport(home.path());
    QVERIFY2(!text.isEmpty(), "nenhum relatório de crash foi escrito");
    QVERIFY2(text.contains(QStringLiteral("[CRESCEU AQUI]")),
             qPrintable(QStringLiteral("thread que quebrou não foi marcada:\n%1").arg(text)));

    // O TID relatado tem de ser o TID de verdade, e a linha marcada tem de ser
    // a da thread principal. É esta checagem que pega o pthread_self(): com ele
    // o TID sai como um ponteiro 0x7f... e a thread principal aparece também
    // na lista de /proc como "NÃO REGISTRADA".
    bool markedOnMain = false;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (!line.contains(QStringLiteral("ui-principal"))) continue;
        QVERIFY2(line.contains(QStringLiteral("tid=") + childTid),
                 qPrintable(QStringLiteral("TID relatado (%1) não bate com o TID real do filho "
                                          "(%2):\n%3")
                                .arg(line.trimmed(), childTid, text)));
        if (line.contains(QStringLiteral("[CRESCEU AQUI]"))) markedOnMain = true;
    }
    QVERIFY2(markedOnMain, qPrintable(QStringLiteral("marcador na linha errada:\n%1").arg(text)));

    // A thread principal não pode sair duplicada como não registrada: isso
    // significaria que o TID do registro não casou com o do /proc.
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        const bool isOurTid = line.contains(QStringLiteral("tid=") + childTid);
        QVERIFY2(!(isOurTid && line.contains(QStringLiteral("NÃO REGISTRADA"))),
                 qPrintable(QStringLiteral("thread do registro reapareceu como não registrada:\n%1")
                                .arg(text)));
    }
}

void tst_crashreporter::reportListsUnregisteredThreads() {
    QTemporaryDir home;
    QVERIFY(home.isValid());

    ::fflush(nullptr);
    const pid_t pid = ::fork();
    QVERIFY(pid >= 0);
    if (pid == 0) {
        ::setenv("HOME", home.path().toLocal8Bit().constData(), 1);
        CrashReporter::install();
        CrashReporter::registerThread("ui-principal");
        // Thread crua, sem registro: representa o pool do Qt e as threads
        // internas do FFmpeg, que o Pierrot não instrumenta.
        ::pthread_t raw;
        ::pthread_create(&raw, nullptr, [](void*) -> void* {
            QThread::msleep(3000);
            return nullptr;
        }, nullptr);
        QThread::msleep(200);
        volatile int* bad = nullptr;
        ::_exit(*bad ? 0 : 1);
    }
    int status = 0;
    QCOMPARE(::waitpid(pid, &status, 0), pid);

    const QString text = readReport(home.path());
    QVERIFY2(!text.isEmpty(), "nenhum relatório de crash foi escrito");
    // A thread não registrada aparece com marcador — é costuma ser a suspect de
    // um travamento, e é justamente a que o Pierrot não conhece.
    QVERIFY2(text.contains(QStringLiteral("NÃO REGISTRADA")),
             qPrintable(QStringLiteral("thread não registrada não listada:\n%1").arg(text)));
}

QTEST_MAIN(tst_crashreporter)
#include "tst_crashreporter.moc"
