// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste do FFmpegEncoder (LaArtman): fecha o ciclo encode→mux→decode.
// Gera N quadros RGBA, codifica num MP4 e decodifica de volta com o
// FFmpegDecoder para provar que container/codec ficaram válidos. Pula se o
// build do FFmpeg não tiver o libx264.

#include <QtTest>

#include "laartman/FFmpegEncoder.h"
#include "laartman/FFmpegDecoder.h"

#include <QColor>
#include <QFileInfo>
#include <QTemporaryDir>

class tst_encoder : public QObject {
    Q_OBJECT
private slots:
    void encodeThenDecode();
};

void tst_encoder::encodeThenDecode() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out.mp4"));

    const int W = 128, H = 96, N = 12;
    const double fps = 24.0;

    FFmpegEncoder enc;
    FFmpegEncoder::Options opts;
    opts.codec = QStringLiteral("libx264");
    opts.preset = QStringLiteral("ultrafast");
    if (!enc.open(out, W, H, fps, opts))
        QSKIP("encoder libx264 indisponível neste build do FFmpeg");

    for (int i = 0; i < N; ++i) {
        QImage img(W, H, QImage::Format_RGBA8888);
        img.fill(QColor::fromHsv((i * 360) / N, 255, 255));
        QVERIFY2(enc.addFrame(img), "addFrame falhou");
    }
    QVERIFY2(enc.finish(), "finish falhou");
    QCOMPARE(enc.framesWritten(), qint64(N));
    enc.close();

    QVERIFY2(QFileInfo(out).size() > 0, "arquivo de saída vazio");

    const FFmpegMediaInfo info = FFmpegDecoder::probe(out);
    QVERIFY2(info.hasVideo, "o MP4 gerado não tem stream de vídeo");
    QCOMPARE(info.width, W);
    QCOMPARE(info.height, H);
    QVERIFY2(qAbs(info.fps - fps) < 0.01,
             qPrintable(QStringLiteral("fps do arquivo gerado não confere: %1 (esperado %2)")
                            .arg(info.fps).arg(fps)));

    FFmpegDecoder dec;
    dec.setHardwareDecodeAllowed(false); // decode por CPU: independente da VAAPI da máquina
    QVERIFY2(dec.open(out), "não abriu o MP4 gerado");
    const QImage f = dec.frameAt(0.0, W);
    QVERIFY2(!f.isNull(), "não decodificou o primeiro quadro do arquivo gerado");
    QCOMPARE(f.width(), W);
}

QTEST_MAIN(tst_encoder)
#include "tst_encoder.moc"
