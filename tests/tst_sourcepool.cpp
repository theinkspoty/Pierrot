// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste do SourcePool (política L4, em colombina/ffmpeg).
//
// O pool é o coração do "corte quente" do preview: acesso compartilhado por
// caminho, LRU de entradas ociosas, open pago uma vez por fonte. A garantia
// central do SourcePool é que acquire() NUNCA devolve nullptr por "ocupado" —
// só se o open() falhar de fato. É essa propriedade que elimina a classe de
// falha "sem slot / ocupado" (quadro vazio por bloqueio).
//
// Sem mídia real só o caminho de falha do open roda; com ffmpeg no PATH o
// teste gera clipes sintéticos e exercita reuse/eviction/concorrência.

#include <QtTest>

#include "colombina/ffmpeg/SourcePool.h"

#include "laartman/core/FFmpegDecoder.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <atomic>
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

class tst_SourcePool : public QObject {
    Q_OBJECT
private slots:
    // Caminho inexistente: open falha, acquire devolve nullptr e a entrada
    // NÃO fica residente (senão o pool encheria de entradas mortas).
    void openFailureDoesNotLeakEntry() {
        SourcePool pool(4);
        QVERIFY(pool.acquire(QStringLiteral("/nonexistent/nope.mp4")) == nullptr);
        QCOMPARE(pool.count(), 0);
        QVERIFY(!pool.contains(QStringLiteral("/nonexistent/nope.mp4")));
        // Tentar de novo continua falhando do mesmo jeito (sem estado preso).
        QVERIFY(pool.acquire(QStringLiteral("/nonexistent/nope.mp4")) == nullptr);
        QCOMPARE(pool.count(), 0);
    }

    // A GARANTIA CENTRAL: o mesmo caminho pode ser adquirido quantas vezes
    // quiser, concorrentemente ou não — nunca é recusado por "ocupado". Era
    // exatamente o que o antigo lease `busy` fazia (e o que causava as falhas).
    void samePathIsNeverRefused() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.mp4"));
        QVERIFY(makeClip(a, 1, QColor(Qt::red)));

        SourcePool pool(4);
        auto d1 = pool.acquire(a);
        QVERIFY(d1 != nullptr);
        // Segunda aquisição do mesmo caminho: NÃO é nullptr (acesso compartilhado).
        auto d2 = pool.acquire(a);
        QVERIFY(d2 != nullptr);
        QVERIFY(d1.get() == d2.get()); // mesma instância (fonte quente)
        QCOMPARE(pool.count(), 1);
    }

    // Capacidade mínima é 2 (contrato do header): pool(1) vira 2.
    void evictsLeastRecentlyIdle() {
        SourcePool pool(1);
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

        auto da = pool.acquire(a);
        QVERIFY(da != nullptr);
        QCOMPARE(pool.count(), 1);
        QVERIFY(pool.contains(a));

        // Segundo arquivo entra (capacidade 2). O shared_ptr de `a` ainda está
        // vivo em `da`, então a evicção não pode despejá-la (use_count > 1).
        auto db = pool.acquire(b);
        QVERIFY(db != nullptr);
        QCOMPARE(pool.count(), 2);

        // Solta a antes de b (com folga de relógio: elapsed() tem resolução de
        // ms e um empate deixaria a ordem LRU indefinida), então a vira ociosa.
        pool.release(a);
        da.reset(); // ninguém mais segura a → agora é despejável
        QThread::msleep(5);
        pool.release(b);
        db.reset();

        auto dc = pool.acquire(c);
        QVERIFY(dc != nullptr);
        QCOMPARE(pool.count(), 2); // a (LRU ociosa) foi despejada
        QVERIFY(!pool.contains(a));
        QVERIFY(pool.contains(b));
        QVERIFY(pool.contains(c));
    }

    // Smoke de concorrência: N threads, cada uma pedindo o MESMO conjunto de
    // caminhos em rodízio. Antes (lease `busy`) isso produzia recusas; agora
    // todo acquire devolve um decoder válido, sempre.
    void concurrentSamePathsAreNeverRefused() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        constexpr int kThreads = 4;
        constexpr int kPaths = 2; // menos caminhos que threads: força concorrência
        QStringList paths;
        for (int i = 0; i < kPaths; ++i) {
            const QString p = dir.filePath(QStringLiteral("s%1.mp4").arg(i));
            if (!makeClip(p, 1, QColor(40 * i, 80, 200)))
                QFAIL("falha ao gerar clipe sintético");
            paths << p;
        }
        SourcePool pool(2);
        std::vector<std::thread> workers;
        std::atomic<int> okCount{ 0 };
        std::atomic<int> refused{ 0 };
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&pool, &paths, &okCount, &refused, i] {
                for (int r = 0; r < 20; ++r) {
                    const QString& p = paths[(i + r) % paths.size()];
                    auto d = pool.acquire(p);
                    if (d) {
                        d->frameAt(0.0, 64);
                        okCount.fetch_add(1);
                    } else {
                        refused.fetch_add(1);
                    }
                }
            });
        }
        for (auto& w : workers) w.join();
        QCOMPARE(refused.load(), 0);           // nunca recusado por "ocupado"
        QCOMPARE(okCount.load(), kThreads * 20);
    }
};

QTEST_MAIN(tst_SourcePool)
#include "tst_sourcepool.moc"
