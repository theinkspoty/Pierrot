// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste do DecoderPool (extraído de PreviewWidget.cpp no F4).
//
// O pool é o coração do "corte quente" do preview: lease por caminho, LRU de
// entradas ociosas, open pago uma vez por fonte. Antes da extração era uma
// classe aninhada no .cpp de 5k linhas — intestável por construção.
//
// Sem mídia real só o caminho de falha do open roda; com ffmpeg no PATH o
// teste gera 2 clipes sintéticos e exercita lease/busy/eviction de verdade.

#include <QtTest>

#include "ui/monitor/DecoderPool.h"

#include "laartman/FFmpegDecoder.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <thread>
#include <vector>

namespace {

bool haveFfmpegCli() {
    static const bool ok = !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();
    return ok;
}

bool makeClip(const QString& path, int seconds, const QColor& color) {
    QProcess p;
    p.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")),
            { QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
              QStringLiteral("-i"),
              QStringLiteral("color=c=%1:s=160x120:d=%2").arg(color.name()).arg(seconds),
              QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
              QStringLiteral("-c:v"), QStringLiteral("libx264"),
              QStringLiteral("-preset"), QStringLiteral("ultrafast"),
              path });
    return p.waitForFinished(30000) && p.exitCode() == 0;
}

} // namespace

class tst_DecoderPool : public QObject {
    Q_OBJECT
private slots:
    // Caminho inexistente: open falha, acquire devolve nullptr e a entrada
    // NÃO fica residente (senão o pool encheria de entradas mortas).
    void openFailureDoesNotLeakEntry() {
        DecoderPool pool(4);
        QVERIFY(pool.acquire(QStringLiteral("/nonexistent/nope.mp4")) == nullptr);
        QCOMPARE(pool.count(), 0);
        QVERIFY(!pool.contains(QStringLiteral("/nonexistent/nope.mp4")));
        // Tentar de novo continua falhando do mesmo jeito (sem estado preso).
        QVERIFY(pool.acquire(QStringLiteral("/nonexistent/nope.mp4")) == nullptr);
        QCOMPARE(pool.count(), 0);
    }

    // Capacidade mínima é 2 (contrato do header): pool(1) vira 2.
    void evictsLeastRecentlyIdle() {
        DecoderPool pool(1);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.mp4"));
        const QString b = dir.filePath(QStringLiteral("b.mp4"));
        const QString c = dir.filePath(QStringLiteral("c.mp4"));
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente — só o caminho de falha foi exercitado");
        QVERIFY(makeClip(a, 1, QColor(Qt::red)));
        QVERIFY(makeClip(b, 1, QColor(Qt::green)));
        QVERIFY(makeClip(c, 1, QColor(Qt::blue)));

        FFmpegDecoder* da = pool.acquire(a);
        QVERIFY(da != nullptr);
        QCOMPARE(pool.count(), 1);
        QVERIFY(pool.contains(a));
        // Mesmo caminho, busy: sem segundo lease.
        QVERIFY(pool.acquire(a) == nullptr);
        // Segundo arquivo entra (capacidade 2).
        FFmpegDecoder* db = pool.acquire(b);
        QVERIFY(db != nullptr);
        QCOMPARE(pool.count(), 2);
        // Terceiro arquivo: eviction do ocioso mais antigo. Soltemos a antes
        // de b (com folga de relógio: elapsed() tem resolução de ms e um
        // empate deixaria a ordem LRU indefinida), então a é a vítima.
        pool.release(a);
        QThread::msleep(5);
        pool.release(b);
        FFmpegDecoder* dc = pool.acquire(c);
        QVERIFY(dc != nullptr);
        QCOMPARE(pool.count(), 2); // a (LRU ociosa) foi despejada
        QVERIFY(!pool.contains(a));
        QVERIFY(pool.contains(b));
        QVERIFY(pool.contains(c));
        pool.release(c);
    }

    // Smoke de concorrência: N threads com caminhos DISTINTOS (lease exclusivo
    // é por caminho) não podem corromper o mapa do pool.
    void concurrentDistinctPaths() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        constexpr int kThreads = 4;
        QStringList paths;
        for (int i = 0; i < kThreads; ++i) {
            const QString p = dir.filePath(QStringLiteral("t%1.mp4").arg(i));
            if (!makeClip(p, 1, QColor(40 * i, 80, 200)))
                QFAIL("falha ao gerar clipe sintético");
            paths << p;
        }
        DecoderPool pool(kThreads + 2);
        std::vector<std::thread> workers;
        std::atomic<int> okCount{ 0 };
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&pool, &paths, &okCount, i] {
                for (int r = 0; r < 20; ++r) {
                    FFmpegDecoder* d = pool.acquire(paths[i]);
                    if (d) {
                        d->frameAt(0.0, 64);
                        pool.release(paths[i]);
                        okCount.fetch_add(1);
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
        QCOMPARE(okCount.load(), kThreads * 20);
    }
};

QTEST_MAIN(tst_DecoderPool)
#include "tst_decoderpool.moc"
