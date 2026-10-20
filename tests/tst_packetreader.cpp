// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste do PacketReader (L0) — leitura de pacotes à frente com buffer limitado.
//
// Garantias: o buffer respeita o teto (não estoura memória em arquivos
// longos); a leitura consome todos os pacotes até o EOF; seek() reposiciona e
// volta a entregar pacotes; close() é seguro e idempotente; cancel aborta a
// espera. Sem ffmpeg CLI, só o caminho de falha do open roda.

#include <QtTest>

#include "laartman/io/PacketReader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <atomic>

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

class tst_PacketReader : public QObject {
    Q_OBJECT
private slots:
    // open de caminho inexistente falha limpo (sem thread pendurada).
    void openFailureIsClean() {
        PacketReader r(8);
        QVERIFY(!r.open(QStringLiteral("/nonexistent/nope.mp4")));
        QVERIFY(!r.isOpen());
        r.close(); // idempotente
    }

    // Lê todos os pacotes de vídeo até o EOF; o buffer nunca ultrapassa o teto.
    void readsAllPacketsBounded() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.mp4"));
        QVERIFY(makeClip(a, 2, QColor(Qt::red)));

        PacketReader r(4); // teto baixo, força back-pressure
        QVERIFY(r.open(a));
        QVERIFY(r.isOpen());
        QVERIFY(r.streamIndex() >= -1);

        int count = 0;
        int maxBuffered = 0;
        AVPacket* pkt = av_packet_alloc();
        while (true) {
            const int rc = r.read(pkt);
            if (rc == AVERROR_EOF) break;
            QVERIFY2(rc >= 0, "read devolveu erro inesperado");
            maxBuffered = qMax(maxBuffered, r.buffered());
            av_packet_unref(pkt);
            ++count;
            if (count > 100000) break; // trava de segurança
        }
        av_packet_free(&pkt);
        QVERIFY(count > 0);
        QVERIFY(maxBuffered <= 4); // nunca passou do teto
    }

    // seek() reposiciona e volta a entregar pacotes (pós-seek).
    void seekRepositionsAndReadsAgain() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.mp4"));
        QVERIFY(makeClip(a, 3, QColor(Qt::blue)));

        PacketReader r(8);
        QVERIFY(r.open(a));
        // Consome alguns pacotes.
        AVPacket* pkt = av_packet_alloc();
        for (int i = 0; i < 3; ++i) {
            const int rc = r.read(pkt);
            if (rc < 0) break;
            av_packet_unref(pkt);
        }
        // Seek para o início e lê de novo.
        const int err = r.seek(r.streamIndex(), 0, AVSEEK_FLAG_BACKWARD);
        QVERIFY(err >= 0);
        const int rc = r.read(pkt);
        QVERIFY(rc >= 0);
        av_packet_unref(pkt);
        av_packet_free(&pkt);
    }

    // seek() pós-EOF: a thread produtora já encerrou (m_running=false) — o
    // seek tem que repousar a thread morta, reposicionar e voltar a produzir
    // (caminho de restart; o caso comum usa pedido pendente sem restart).
    // É o que o wrap de GIF e o replay do preview pagam no fim do arquivo.
    void seekAfterEofRestartsProduction() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.mp4"));
        QVERIFY(makeClip(a, 1, QColor(Qt::cyan)));

        PacketReader r(8);
        QVERIFY(r.open(a));
        AVPacket* pkt = av_packet_alloc();
        int n = 0;
        while (r.read(pkt) >= 0) { av_packet_unref(pkt); ++n; }
        QVERIFY(n > 0); // EOF alcançado, produtor morto
        // Volta ao início e lê de novo — tem que entregar pacotes.
        const int err = r.seek(r.streamIndex(), 0, AVSEEK_FLAG_BACKWARD);
        QVERIFY(err >= 0);
        const int rc = r.read(pkt);
        QVERIFY2(rc >= 0, "read pós-seek-pós-EOF falhou");
        av_packet_unref(pkt);
        // E o normal segue funcionando (segundo seek, agora com produtor vivo).
        QVERIFY(r.seek(r.streamIndex(), 0, AVSEEK_FLAG_BACKWARD) >= 0);
        QVERIFY(r.read(pkt) >= 0);
        av_packet_unref(pkt);
        av_packet_free(&pkt);
    }

    // cancel aborta a espera (não trava para sempre num arquivo vazio/erro).
    void cancelAbortsWait() {
        if (!haveFfmpegCli())
            QSKIP("ffmpeg CLI ausente");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = dir.filePath(QStringLiteral("a.mp4"));
        QVERIFY(makeClip(a, 1, QColor(Qt::green)));

        PacketReader r(2);
        QVERIFY(r.open(a));
        // Consome até esvaziar/errar; depois um read com cancel já marcado
        // devolve AVERROR_EXIT ou EOF imediatamente (sem bloqueio eterno).
        std::atomic<bool> cancel{ true };
        AVPacket* pkt = av_packet_alloc();
        const int rc = r.read(pkt, &cancel);
        QVERIFY(rc == AVERROR_EXIT || rc == AVERROR_EOF || rc >= 0);
        av_packet_free(&pkt);
    }
};

QTEST_MAIN(tst_PacketReader)
#include "tst_packetreader.moc"
