// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste do FramePipeline (política L4, em colombina/ffmpeg).
//
// O pipeline é a política de lookahead que saiu do PreviewWidget: dono da
// thread de prefetch, da fila de "aquecimento à frente" e do estado busy. A UI
// só resolve QUAIS caminhos/tempos (política de modelo, L6); o pipeline cuida
// da serialização e do in-flight. O que se verifica aqui:
//
//   • a fila de warm é substituída por inteiro e reporta in-flight corretamente;
//   • prefetchFrame é serial (best-effort): um segundo pedido enquanto ocupado
//     é ignorado, e a flag libera ao terminar;
//   • o prefetch de frame0 devolve dois quadros (t e t+step) num caminho real;
//   • runPrefetchNow (síncrono) é determinístico e não depende da thread;
//   • parar o pipeline não deixa pedidos pendentes (thread encerra).
//
// Sem ffmpeg CLI, os testes que precisam de mídia são pulados; o resto (estado
// da fila, stop idempotente) roda sempre.

#include <QtTest>

#include "colombina/ffmpeg/FramePipeline.h"
#include "colombina/ffmpeg/SourcePool.h"

#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

bool haveFfmpegCli() {
    static const bool ok =
        !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();
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

class tst_FramePipeline : public QObject {
    Q_OBJECT
private slots:
    // A fila de warm é substituída por inteiro (não acumula entre chamadas) e
    // o in-flight é consultável por caminho. Sem start(): estado só, sem thread.
    void warmQueueReplacedNotAccumulated() {
        SourcePool pool(2);
        FramePipeline pipe(&pool);
        QVector<FramePipeline::WarmTarget> q{
            { QStringLiteral("/a.mp4"), 1.0, 320 },
            { QStringLiteral("/b.mp4"), 2.0, 320 },
        };
        pipe.setWarmQueue(q);
        // Sem thread iniciada, dispatchNextWarm não consome a fila.
        QVERIFY(!pipe.warmQueueEmpty());
        // Substituir por vazio zera tudo (e limpa o in-flight).
        pipe.setWarmQueue({});
        QVERIFY(pipe.warmQueueEmpty());
        QVERIFY(!pipe.warmInFlight(QStringLiteral("/a.mp4")));
    }

    // runPrefetchNow é síncrono e determinístico: mesmo (path,t,maxW) → pixels
    // idênticos, independente de qualquer estado de thread.
    void runPrefetchNowIsDeterministic() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.mp4"));
        QVERIFY(makeClip(a, 2, QColor(Qt::red)));

        SourcePool pool(2);
        FramePipeline pipe(&pool);
        QImage f0, f1;
        pipe.runPrefetchNow(a, 0.0, 96, 1.0 / 30.0, &f0, &f1);
        QVERIFY(!f0.isNull());
        QVERIFY(!f1.isNull());

        QImage g0, g1;
        pipe.runPrefetchNow(a, 0.0, 96, 1.0 / 30.0, &g0, &g1);
        QCOMPARE(f0, g0); // mesma fonte, mesmo tempo → mesmos pixels
        QCOMPARE(f1, g1);
    }

    // End-to-end na thread interna: framePrefetched chega com o par (t, t+step)
    // e a flag busy libera. Pedido durante o voo vira alvo latest-wins
    // (encadeado, não descartado); depois, novos pedidos voltam a ser aceitos.
    void threadedPrefetchEmitsPairAndClearsBusy() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.mp4"));
        QVERIFY(makeClip(a, 2, QColor(Qt::blue)));

        SourcePool pool(2);
        FramePipeline pipe(&pool);
        pipe.start();

        QSignalSpy done(&pipe, &FramePipeline::framePrefetched);
        QVERIFY(done.isValid());

        pipe.prefetchFrame(a, 0.0, 96, 1.0 / 30.0);
        QVERIFY(pipe.prefetchBusy()); // despachado → ocupado até o sinal

        // Enquanto ocupado, o segundo pedido NÃO é descartado: vira o alvo
        // latest-wins e é despachado quando o voo atual terminar (o descarte
        // anterior fazia o corte pagar open frio — o chamador marcava
        // "requested" e podia não re-pedir).
        pipe.prefetchFrame(a, 1.0, 96, 1.0 / 30.0);
        QVERIFY(done.wait(8000)); // primeiro resultado
        QCOMPARE(done.count(), 1);
        {
            const QList<QVariant> args = done.takeFirst();
            QCOMPARE(args.at(0).toString(), a);
            const QImage f0 = args.at(3).value<QImage>();
            const QImage f1 = args.at(4).value<QImage>();
            QVERIFY(!f0.isNull());
            QVERIFY(!f1.isNull());
        }

        // O alvo latest-wins (t=1.0) foi encadeado e chega em seguida; o busy
        // só libera DEPOIS dele.
        QVERIFY(done.wait(8000));
        QCOMPARE(done.count(), 1);
        {
            const QList<QVariant> second = done.takeFirst();
            QCOMPARE(second.at(0).toString(), a);
            QVERIFY(qAbs(second.at(1).toDouble() - 1.0) < 1e-6); // t=1.0
        }
        QVERIFY(!pipe.prefetchBusy());

        // Já liberado: um novo pedido é aceito e chega.
        pipe.prefetchFrame(a, 1.5, 96, 1.0 / 30.0);
        QVERIFY(done.wait(8000));
        QCOMPARE(done.count(), 1);

        pipe.stop();
    }

    // warmNow num caminho real aquece o pool; se o arquivo não abrir, nada
    // quebra (best-effort). warmInFlight é limpo após o warmed.
    void warmNowPopulatesPoolAndClearsInFlight() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.mp4"));
        QVERIFY(makeClip(a, 1, QColor(Qt::green)));

        SourcePool pool(2);
        FramePipeline pipe(&pool);
        pipe.start();

        QSignalSpy warmed(&pipe, &FramePipeline::warmed);
        QVERIFY(warmed.isValid());
        // warmNow não é guardado por warmInFlight prévio se não houver um ativo…
        pipe.warmNow(a, 0.0, 96);
        QVERIFY(warmed.wait(8000));
        QCOMPARE(warmed.count(), 1);
        QVERIFY(pool.contains(a));      // o pool tem a entrada quente
        QVERIFY(!pipe.warmInFlight(a)); // liberado após terminar

        pipe.stop();
    }

    // stop() é seguro sem start() e idempotente depois de iniciar.
    void stopIsSafeAndIdempotent() {
        SourcePool pool(2);
        FramePipeline pipe(&pool);
        pipe.stop(); // sem start → no-op
        pipe.start();
        pipe.stop();
        pipe.stop(); // segunda vez → no-op
        QVERIFY(true); // não deve travar nem crashar
    }
};

QTEST_MAIN(tst_FramePipeline)
#include "tst_framepipeline.moc"
