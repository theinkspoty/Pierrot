// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Fixture de mídia REAL para o motor (lacuna do relatório de estresse §6):
// o CI só tinha mídia sintética (testsrc) — VFR de câmera, MPEG-TS, verticais
// (1080 de largura — o caso do bug do swscale), GIFs longos e AV1 de verdade
// nunca rodavam. Este teste varre PIERROT_TEST_MEDIA_DIR (padrão:
// <raiz do repo>/tests/media) e, para CADA arquivo:
//   1. probe devolve metadados plausíveis;
//   2. open + frames em 0/middle/fim não-nulos;
//   3. determinismo: frame(t) idêntico após reabertura.
// Sem arquivos no diretório, TODOS os casos pulam — o CI não quebra; popule
// o diretório (gitignored ou local) com mídia real para ativá-lo.

#include <QtTest>

#include "laartman/Decoder.h"

#include <QDir>
#include <QSet>
#include <algorithm>
#include <QFileInfo>

#ifndef TEST_MEDIA_DIR
#define TEST_MEDIA_DIR "tests/media"
#endif

class tst_RealMedia : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void probeIsPlausible_data();
    void probeIsPlausible();
    void framesAreNonNull_data();
    void framesAreNonNull();
    void decodeIsDeterministicAcrossReopen_data();
    void decodeIsDeterministicAcrossReopen();

private:
    QStringList m_files;
    static QList<QByteArray> mediaNames() {
        QList<QByteArray> out;
        const QDir dir(QString::fromUtf8(TEST_MEDIA_DIR));
        const auto entries = dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
        for (const QFileInfo& fi : entries)
            out << fi.fileName().toUtf8(); // puro nome p/ o _data()
        return out;
    }
};

void tst_RealMedia::initTestCase() {
    const QDir dir(QString::fromUtf8(TEST_MEDIA_DIR));
    if (!dir.exists())
        QSKIP("diretório de mídia real inexistente — popule tests/media/");
    // Só extensões de mídia (o diretório pode conter README/.gitkeep).
    static const QSet<QString> exts = {
        "mp4","mov","mkv","webm","avi","ts","mts","m2ts","flv","ogv","gif","webp",
        "jpg","jpeg","png","tif","tiff","avif","m4a","mp3","wav","flac","ogg","opus",
    };
    const auto entries = dir.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo& fi : entries)
        if (exts.contains(fi.suffix().toLower())) m_files << fi.absoluteFilePath();
    if (m_files.isEmpty())
        QSKIP("tests/media/ sem mídia — adicione arquivos reais (VFR, TS, vertical, GIF longo, AV1…)");
    qInfo() << "mídia real:" << m_files.size() << "arquivos em" << dir.absolutePath();
}

void tst_RealMedia::probeIsPlausible_data() {
    QTest::addColumn<QByteArray>("file");
    for (const QByteArray& f : mediaNames()) QTest::newRow(f.constData()) << f;
}

void tst_RealMedia::probeIsPlausible() {
    QFETCH(QByteArray, file);
    const QString path = QString::fromUtf8(TEST_MEDIA_DIR) + QLatin1Char('/') + QString::fromUtf8(file);
    const MediaInfo info = Decoder::probe(path);
    QVERIFY2(info.hasVideo || info.hasAudio, "probe não achou nenhum stream");
    if (info.hasVideo) {
        QVERIFY2(info.width > 0 && info.height > 0, "vídeo sem dimensões");
        QVERIFY2(info.duration > 0.0, "vídeo sem duração");
    }
}

void tst_RealMedia::framesAreNonNull_data() { probeIsPlausible_data(); }

void tst_RealMedia::framesAreNonNull() {
    QFETCH(QByteArray, file);
    const QString path = QString::fromUtf8(TEST_MEDIA_DIR) + QLatin1Char('/') + QString::fromUtf8(file);
    const MediaInfo info = Decoder::probe(path);
    if (!info.hasVideo) QSKIP("sem stream de vídeo");

    Decoder dec;
    QVERIFY(dec.open(path, DecodeOptions()));
    const double mid = info.duration * 0.5;
    const double end = std::max(0.0, info.duration - 0.1);
    for (double t : {0.0, mid, end}) {
        const QImage img = dec.frame(t, 640);
        QVERIFY2(!img.isNull(),
                 qPrintable(QStringLiteral("frame nulo em t=%1 (%2)").arg(t).arg(file)));
    }
    dec.close();
}

void tst_RealMedia::decodeIsDeterministicAcrossReopen_data() { probeIsPlausible_data(); }

void tst_RealMedia::decodeIsDeterministicAcrossReopen() {
    QFETCH(QByteArray, file);
    const QString path = QString::fromUtf8(TEST_MEDIA_DIR) + QLatin1Char('/') + QString::fromUtf8(file);
    const MediaInfo info = Decoder::probe(path);
    if (!info.hasVideo || info.duration <= 0.05) QSKIP("sem vídeo usável");

    const double t = info.duration * 0.5;
    Decoder a;
    QVERIFY(a.open(path, DecodeOptions()));
    const QImage first = a.frame(t, 320);
    a.close();
    if (first.isNull()) QSKIP("frame central nulo (midia pode ser só áudio no vídeo)");

    Decoder b;
    QVERIFY(b.open(path, DecodeOptions()));
    const QImage second = b.frame(t, 320);
    QCOMPARE(second.size(), first.size());
    QVERIFY2(second == first, "frame divergiu após reabertura (determinismo)");
}

QTEST_MAIN(tst_RealMedia)
#include "tst_realmedia.moc"
