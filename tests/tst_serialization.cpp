// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Testes da serialização .Blanc (Project::toJson/fromJson) — round-trip
// sem perda de dados, cobrindo mídia, geradores, clipes (efeitos, keyframes,
// OFX, transform, correção de cor), faixas, marcadores, pastas, recursos de
// texto e Mesas.

#include <QtTest>

#include "models/Project.h"

static Project makeProject() {
    Project p;
    p.name = QStringLiteral("Projeto Teste");
    p.width = 1280;
    p.height = 720;
    p.fps = 24;
    p.audioRate = 44100.0;
    p.masterVolume = 0.8;

    MediaItem m;
    m.id = QStringLiteral("m1");
    m.filePath = QStringLiteral("/tmp/video.mp4");
    m.name = QStringLiteral("clipe");
    m.duration = 10.0;
    m.width = 1920;
    m.height = 1080;
    m.hasVideo = true;
    m.hasAudio = true;
    m.audioStreams = 2;
    m.audioChannels = {2, 2};
    p.media.append(m);

    MediaItem solid;
    solid.id = QStringLiteral("m2");
    solid.isSolid = true;
    solid.generator = QStringLiteral("gradient");
    solid.solidColor = QColor(255, 0, 0, 128);
    solid.solidColor2 = QColor(0, 0, 255);
    solid.genCells = 16;
    p.media.append(solid);

    Track vt;
    vt.id = QStringLiteral("t1");
    vt.name = QStringLiteral("V1");
    vt.audio = false;
    vt.blendMode = QStringLiteral("screen");
    vt.volume = 1.0;
    vt.pan = 0.25;
    vt.opacity = 0.9;
    vt.color = QColor(10, 20, 30);
    vt.muted = true;
    vt.solo = false;
    vt.locked = true;
    vt.height = 120;

    Clip c;
    c.id = QStringLiteral("c1");
    c.mediaId = QStringLiteral("m1");
    c.pos = 1.5;
    c.in = 0.0;
    c.dur = 8.0;
    c.name = QStringLiteral("take 1");
    c.transitionType = QStringLiteral("wipeleft");
    c.volume = 1.5;
    c.opacity = 0.7;
    c.fadeIn = 0.5;
    c.fadeOut = 0.5;
    c.speed = 2.0;
    c.brightness = 0.1;
    c.contrast = 1.2;
    c.saturation = 0.8;
    c.blur = 0.3;
    c.grayscale = true;
    c.chromaKey = true;
    c.chromaKeyColor = QColor(0, 255, 0);
    c.chromaKeySimilarity = 0.2;
    c.liftR = 0.1;
    c.gammaG = 1.1;
    c.gainB = -0.1;
    c.lainkaEnabled = true;
    c.lainkaSkip = 4;
    c.motionEnabled = true;
    c.motionAmount = 50.0;
    c.tx = 10.0;
    c.ty = 20.0;
    c.scale = 1.5;
    c.scaleX = 1.2;
    c.scaleY = 0.9;
    c.rotation = 30.0;
    c.anchorX = 0.5;
    c.anchorY = -0.5;
    c.cropL = 0.1;
    c.cropT = 0.05;
    c.eqLow = 3.0;
    c.eqHigh = -2.0;
    c.denoise = true;
    c.normalize = true;
    c.invertPhase = true;
    c.reverb = true;
    c.reverbMix = 0.4;
    c.reverbSize = 0.6;

    upsertKeyframe(c.kfOpacity, 0.0, 0.0, KfLinear);
    upsertKeyframe(c.kfOpacity, 5.0, 1.0, KfBezier);
    c.kfOpacity[1].ox = 0.5;
    c.kfOpacity[1].oy = 0.1;
    upsertKeyframe(c.kfSpeed, 0.0, 1.0, KfLinear);
    upsertKeyframe(c.kfSpeed, 4.0, 3.0, KfLinear);

    OfxPluginInstance fx;
    fx.pluginId = QStringLiteral("org.openfx.invert");
    fx.enabled = true;
    OfxParam p1;
    p1.key = QStringLiteral("amount");
    p1.value = 0.5;
    OfxParam p2;
    p2.key = QStringLiteral("color");
    p2.value = QColor(255, 128, 0, 200);
    OfxParam p3;
    p3.key = QStringLiteral("flag");
    p3.value = true;
    fx.params = {p1, p2, p3};
    c.ofxFx.append(fx);
    vt.clips.append(c);

    Clip tc;
    tc.id = QStringLiteral("tc1");
    tc.isText = true;
    tc.pos = 0.0;
    tc.dur = 3.0;
    tc.text.text = QStringLiteral("Olá");
    tc.text.textColor = QColor(255, 255, 0, 180);
    vt.clips.append(tc);

    p.videoTracks.append(vt);

    Track at;
    at.id = QStringLiteral("a1");
    at.name = QStringLiteral("A1");
    at.audio = true;
    at.volume = 0.9;
    at.pan = -0.5;
    at.eqMid = 1.0;
    at.reverb = true;
    upsertKeyframe(at.kfVolume, 0.0, 0.0);
    upsertKeyframe(at.kfVolume, 2.0, 1.5);
    upsertKeyframe(at.kfPan, 0.0, -1.0);
    Clip ac;
    ac.id = QStringLiteral("ac1");
    ac.mediaId = QStringLiteral("m1");
    ac.audioStreamIndex = 1;
    ac.pos = 0.0;
    ac.in = 0.0;
    ac.dur = 10.0;
    at.clips.append(ac);
    p.audioTracks.append(at);

    Marker mk;
    mk.id = QStringLiteral("k1");
    mk.time = 3.0;
    mk.name = QStringLiteral("corte");
    mk.color = QColor(255, 200, 40);
    p.addMarker(mk);

    TrackGroup g;
    g.id = QStringLiteral("g1");
    g.name = QStringLiteral("Pasta");
    g.collapsed = true;
    p.trackGroups.append(g);

    TextResource tr;
    tr.id = QStringLiteral("tr1");
    tr.text.text = QStringLiteral("Compartilhado");
    tr.text.textSize = 0.1;
    p.textResources.append(tr);

    MesaComposition mesa;
    mesa.id = QStringLiteral("mesa1");
    mesa.name = QStringLiteral("Comp");
    mesa.canvasW = 640;
    mesa.canvasH = 360;
    mesa.trackIds = {QStringLiteral("t1")};
    mesa.camX = 320.0;
    mesa.camY = 180.0;
    mesa.camZoom = 1.5;
    mesa.camRotation = 10.0;
    upsertKeyframe(mesa.kfCamX, 0.0, 320.0, KfSmooth);
    mesa.motionBlur = true;
    mesa.motionBlurSamples = 12;
    p.mesas.append(mesa);

    return p;
}

class TestSerialization : public QObject {
    Q_OBJECT

private slots:
    void roundTripIsLossless();
    void roundTripPreservesKeyFields();
    void legacyProjectWithoutMesaPosAbsShiftsCamera();
};

void TestSerialization::roundTripIsLossless() {
    const Project p = makeProject();
    Project q;
    q.fromJson(p.toJson());
    QCOMPARE(q.toJson(), p.toJson());
}

void TestSerialization::roundTripPreservesKeyFields() {
    const Project p = makeProject();
    Project q;
    q.fromJson(p.toJson());

    QCOMPARE(q.name, p.name);
    QCOMPARE(q.width, p.width);
    QCOMPARE(q.height, p.height);
    QCOMPARE(q.fps, p.fps);
    QCOMPARE(q.media.size(), p.media.size());
    QCOMPARE(q.videoTracks.size(), p.videoTracks.size());
    QCOMPARE(q.audioTracks.size(), p.audioTracks.size());
    QCOMPARE(q.markers.size(), p.markers.size());
    QCOMPARE(q.trackGroups.size(), p.trackGroups.size());
    QCOMPARE(q.textResources.size(), p.textResources.size());
    QCOMPARE(q.mesas.size(), p.mesas.size());

    const Clip& c = q.videoTracks[0].clips[0];
    QCOMPARE(c.mediaId, QStringLiteral("m1"));
    QCOMPARE(c.transitionType, QStringLiteral("wipeleft"));
    QVERIFY(c.grayscale);
    QVERIFY(c.chromaKey);
    QVERIFY(c.hasColorGrade());
    QVERIFY(c.hasTransform());
    QVERIFY(c.hasAudioFx());
    QCOMPARE(c.ofxFx.size(), 1);
    QCOMPARE(c.ofxFx[0].pluginId, QStringLiteral("org.openfx.invert"));
    QCOMPARE(c.ofxFx[0].params.size(), 3);
    QCOMPARE(c.kfOpacity.size(), 2);
    QCOMPARE(c.kfOpacity[1].interp, KfBezier);
    QCOMPARE(c.kfSpeed.size(), 2);

    const MediaItem& solid = q.media[1];
    QVERIFY(solid.isSolid);
    QCOMPARE(solid.generator, QStringLiteral("gradient"));
    QCOMPARE(solid.solidColor.alpha(), 128);

    const Track& at = q.audioTracks[0];
    QCOMPARE(at.clips[0].audioStreamIndex, 1);
    QCOMPARE(at.kfVolume.size(), 2);
    QCOMPARE(at.kfPan.size(), 1);

    QCOMPARE(q.mesas[0].trackIds.size(), 1);
    QCOMPARE(q.mesas[0].trackIds[0], QStringLiteral("t1"));
    QVERIFY(q.mesas[0].motionBlur);
    QCOMPARE(q.mesas[0].motionBlurSamples, 12);
}

void TestSerialization::legacyProjectWithoutMesaPosAbsShiftsCamera() {
    // Projeto v1: mesaX/camX como OFFSET do centro (default 0). Sem o campo
    // "mesaPosAbs", o load deve deslocar para px absolutos (origem topo-esq).
    QJsonObject o;
    o["name"] = QStringLiteral("legado");
    o["width"] = 640;
    o["height"] = 360;
    o["fps"] = 30;
    QJsonObject mesa;
    mesa["id"] = QStringLiteral("m");
    mesa["canvasW"] = 640;
    mesa["canvasH"] = 360;
    mesa["camX"] = 0.0; // v1: centro
    mesa["camY"] = 0.0;
    QJsonArray mesas;
    mesas.append(mesa);
    o["mesas"] = mesas;

    Project p;
    p.fromJson(o);
    QCOMPARE(p.mesas.size(), 1);
    QVERIFY(qAbs(p.mesas[0].camX - 320.0) < 1e-9);
    QVERIFY(qAbs(p.mesas[0].camY - 180.0) < 1e-9);
}

QTEST_APPLESS_MAIN(TestSerialization)
#include "tst_serialization.moc"
