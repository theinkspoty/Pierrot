// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Testes do intercâmbio EDL (CMX3600): geração e round-trip de cortes.

#include <QtTest>

#include "export/NleInterchange.h"

class TestEdl : public QObject {
    Q_OBJECT

private slots:
    void timecodeFormat();
    void buildEdlContainsEdits();
    void roundTripPreservesCuts();
    void importRejectsEmptyFile();
};

static Project makeEdlProject() {
    Project p;
    p.name = QStringLiteral("Teste");
    p.fps = 25;
    p.width = 1920;
    p.height = 1080;

    MediaItem mv;
    mv.id = QStringLiteral("mv");
    mv.filePath = QStringLiteral("/tmp/video.mp4");
    mv.hasVideo = true;
    mv.hasAudio = true;
    p.media.append(mv);

    MediaItem ma;
    ma.id = QStringLiteral("ma");
    ma.filePath = QStringLiteral("/tmp/audio.wav");
    ma.hasVideo = false;
    ma.hasAudio = true;
    p.media.append(ma);

    p.videoTracks.append(Track());
    p.videoTracks[0].id = QStringLiteral("tv");
    p.videoTracks[0].name = QStringLiteral("Video 1");
    Clip cv;
    cv.id = QStringLiteral("cv");
    cv.mediaId = QStringLiteral("mv");
    cv.pos = 1.0;
    cv.in = 10.0;
    cv.dur = 5.0;
    p.videoTracks[0].clips.append(cv);

    p.audioTracks.append(Track());
    p.audioTracks[0].id = QStringLiteral("ta");
    p.audioTracks[0].name = QStringLiteral("Audio 1");
    p.audioTracks[0].audio = true;
    Clip ca;
    ca.id = QStringLiteral("ca");
    ca.mediaId = QStringLiteral("ma");
    ca.pos = 0.0;
    ca.in = 2.0;
    ca.dur = 4.0;
    p.audioTracks[0].clips.append(ca);

    return p;
}

void TestEdl::timecodeFormat() {
    QCOMPARE(NleInterchange::edlTimecode(0.0, 25), QStringLiteral("00:00:00:00"));
    // 1s a 25fps = frame 25 = já avança para 00:00:01:00
    QCOMPARE(NleInterchange::edlTimecode(1.0, 25), QStringLiteral("00:00:01:00"));
    // 67.5s a 25fps = 1687.5 → frame 1688 (arredonda) = 1:07:12
    QCOMPARE(NleInterchange::edlTimecode(67.48, 25).left(8), QStringLiteral("00:01:07"));
}

void TestEdl::buildEdlContainsEdits() {
    const Project p = makeEdlProject();
    const QString edl = NleInterchange::buildEdl(p);
    QVERIFY(edl.contains(QStringLiteral("TITLE: Teste")));
    QVERIFY(edl.contains(QStringLiteral("video")));      // reel "video" (baseName)
    QVERIFY(edl.contains(QStringLiteral("audio")));      // reel "audio"
}

void TestEdl::roundTripPreservesCuts() {
    const Project p = makeEdlProject();

    NleInterchange::EdlImportResult res;
    res.project = Project();
    // Usa o EDL gerado para importar de volta (via arquivo temporário).
    const QString edlText = NleInterchange::buildEdl(p);
    QTemporaryFile tmp;
    QVERIFY(tmp.open());
    tmp.write(edlText.toUtf8());
    tmp.flush();

    QVERIFY(NleInterchange::importEdl(tmp.fileName(), res));

    // Dois clips: 1 vídeo + 1 áudio.
    int totalClips = 0;
    for (const Track& t : res.project.videoTracks) totalClips += t.clips.size();
    for (const Track& t : res.project.audioTracks) totalClips += t.clips.size();
    QCOMPARE(totalClips, 2);

    // Verifica o clipe de vídeo.
    bool foundVideo = false;
    for (const Track& t : res.project.videoTracks) {
        for (const Clip& c : t.clips) {
            QCOMPARE(qRound(c.in * 100), 1000);    // 10.0s
            QCOMPARE(qRound(c.dur * 100), 500);     // 5.0s
            QCOMPARE(qRound(c.pos * 100), 100);     // 1.0s
            foundVideo = true;
        }
    }
    QVERIFY(foundVideo);
}

void TestEdl::importRejectsEmptyFile() {
    NleInterchange::EdlImportResult res;
    QVERIFY(!NleInterchange::importEdl(QStringLiteral("/caminho/inexistente/nada.edl"), res));
}

QTEST_APPLESS_MAIN(TestEdl)
#include "tst_edl.moc"
