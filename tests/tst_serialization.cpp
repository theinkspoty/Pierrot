// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Testes da serialização .Blanc (Project::toJson/fromJson) — round-trip
// sem perda de dados, cobrindo mídia, geradores, clipes (efeitos, keyframes,
// OFX, transform, correção de cor), faixas, marcadores, pastas, recursos de
// texto e Mesas.

#include <QtTest>

#include "clipattrs.h"
#include "colombina/models/Project.h"

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
    c.chromaKeySoftness = 0.35;
    c.chromaKeySpillSuppress = 0.8;
    c.liftR = 0.1;
    c.gammaG = 1.1;
    c.gainB = -0.1;
    c.cgExposure = 0.35;
    c.cgContrast = 12.0;
    c.cgHighlights = -20.0;
    c.cgShadows = 15.0;
    c.cgSaturation = 8.0;
    c.cgVibrance = 25.0;
    c.cgTemperature = -10.0;
    c.cgTint = 5.0;
    c.cgVignette = -30.0;
    c.cgBlend = 0.85;
    c.cgLutPath = QStringLiteral("/tmp/look.cube");
    c.cgMasterCurve = QVector<QPointF>{QPointF(0, 0), QPointF(0.5, 0.55), QPointF(1, 1)};
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
    c.isMulticam = true;
    c.multicamSources = QStringList{QStringLiteral("m1"), QStringLiteral("m2"),
                                    QStringLiteral("m3")};
    c.multicamIns = QVector<double>{0.0, 0.25, 0.5};
    c.defaultAngle = 1;
    upsertKeyframe(c.kfAngle, 0.0, 1.0, KfStep);
    upsertKeyframe(c.kfAngle, 3.0, 2.0, KfStep);
    Frei0rEffect f0r;
    f0r.pluginName = QStringLiteral("vignette");
    f0r.enabled = true;
    f0r.values = QVector<double>{0.5, 0.25};
    f0r.colors = QVector<QColor>{QColor(255, 0, 0, 200)};
    c.frei0rFx.append(f0r);

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
    void recordingTracksRoundTrip();
    void legacyProjectWithoutRecordingTracksLoads();
    void promoteRecordingTrackMovesItToAudio();
    void clipAttrsRoundTripKeepsChromaKeyBands();
    void clipAttrsLegacyKeepsChromaKeyDefaults();
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
    QVERIFY(qAbs(c.chromaKeySoftness - 0.35) < 1e-9);
    QVERIFY(qAbs(c.chromaKeySpillSuppress - 0.8) < 1e-9);
    QVERIFY(c.hasColorGrade());
    QVERIFY(qAbs(c.cgExposure - 0.35) < 1e-9);
    QVERIFY(qAbs(c.cgContrast - 12.0) < 1e-9);
    QVERIFY(qAbs(c.cgVignette - (-30.0)) < 1e-9);
    QVERIFY(qAbs(c.cgBlend - 0.85) < 1e-9);
    QCOMPARE(c.cgLutPath, QStringLiteral("/tmp/look.cube"));
    QCOMPARE(c.cgMasterCurve.size(), 3);
    QVERIFY(qAbs(c.cgMasterCurve[1].y() - 0.55) < 1e-9);
    QVERIFY(c.hasTransform());
    QVERIFY(c.hasAudioFx());
    QCOMPARE(c.ofxFx.size(), 1);
    QCOMPARE(c.ofxFx[0].pluginId, QStringLiteral("org.openfx.invert"));
    QCOMPARE(c.ofxFx[0].params.size(), 3);
    QCOMPARE(c.kfOpacity.size(), 2);
    QCOMPARE(c.kfOpacity[1].interp, KfBezier);
    QCOMPARE(c.kfSpeed.size(), 2);
    QVERIFY(c.hasMulticam());
    QCOMPARE(c.multicamSources.size(), 3);
    QCOMPARE(c.multicamSources[0], QStringLiteral("m1"));
    QCOMPARE(c.multicamSources[2], QStringLiteral("m3"));
    QCOMPARE(c.multicamIns.size(), 3);
    QVERIFY(qAbs(c.multicamIns[1] - 0.25) < 1e-9);
    QCOMPARE(c.defaultAngle, 1);
    QCOMPARE(c.kfAngle.size(), 2);
    QCOMPARE(c.angleAt(0.0), 1);
    QCOMPARE(c.angleAt(2.9), 1);
    QCOMPARE(c.angleAt(3.5), 2);
    QCOMPARE(c.mediaIdAt(3.5), QStringLiteral("m3"));
    QVERIFY(qAbs(c.multicamInAt(3.5) - 0.5) < 1e-9);
    QCOMPARE(c.frei0rFx.size(), 1);
    QCOMPARE(c.frei0rFx[0].pluginName, QStringLiteral("vignette"));
    QVERIFY(c.frei0rFx[0].enabled);
    QCOMPARE(c.frei0rFx[0].values.size(), 2);
    QVERIFY(qAbs(c.frei0rFx[0].values[0] - 0.5) < 1e-9);
    QCOMPARE(c.frei0rFx[0].colors.size(), 1);
    QCOMPARE(c.frei0rFx[0].colors[0].alpha(), 200);

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

void TestSerialization::recordingTracksRoundTrip() {
    Project p;
    p.addTrack(true);
    p.addTrack(true);
    p.addRecordingTrack();
    p.addRecordingTrack();
    QCOMPARE(p.recordingTracks.size(), 2);

    // A faixa de gravação nasce vermelha e com `audio` ligado (o pipeline de
    // áudio — volume, EQ, export — a trata como faixa de áudio).
    QVERIFY(p.recordingTracks[0].audio);
    QCOMPARE(p.recordingTracks[0].color, recordingTrackColor());
    QCOMPARE(p.recordingTracks[0].name, QStringLiteral("Gravação 1"));
    QCOMPARE(p.recordingTracks[1].name, QStringLiteral("Gravação 2"));

    // Um clipe gravado, com envelope de volume: tem de sobreviver ao round-trip.
    Clip c;
    c.id = QStringLiteral("rec1");
    c.pos = 2.5;
    c.dur = 4.0;
    c.volume = 0.6;
    c.kfVolume.append(Keyframe{0.0, 0.3});
    c.kfVolume.append(Keyframe{4.0, 1.2});
    p.recordingTracks[0].clips.append(c);
    p.recordingTracks[0].kfVolume.append(Keyframe{0.0, 0.9});

    Project q;
    q.fromJson(p.toJson());
    QCOMPARE(q.recordingTracks.size(), 2);
    QCOMPARE(q.recordingTracks[0].id, p.recordingTracks[0].id);
    QCOMPARE(q.recordingTracks[0].name, QStringLiteral("Gravação 1"));
    QCOMPARE(q.recordingTracks[0].color, recordingTrackColor());
    QCOMPARE(q.recordingTracks[0].clips.size(), 1);
    QCOMPARE(q.recordingTracks[0].clips[0].id, QStringLiteral("rec1"));
    QVERIFY(std::fabs(q.recordingTracks[0].clips[0].volume - 0.6) < 1e-9);
    QCOMPARE(q.recordingTracks[0].clips[0].kfVolume.size(), 2);
    QCOMPARE(q.recordingTracks[0].kfVolume.size(), 1);
    QCOMPARE(q.recordingTracks[1].clips.size(), 0);

    // As faixas de áudio e de gravação são listas independentes: uma não vaza
    // pra outra no round-trip.
    QCOMPARE(q.audioTracks.size(), 2);
    QCOMPARE(q.videoTracks.size(), 0);
}

void TestSerialization::legacyProjectWithoutRecordingTracksLoads() {
    // .Blanc gravado antes da seção existir: sem a chave => lista vazia, sem erro.
    QJsonObject o;
    o["name"] = QStringLiteral("legado");
    o["width"] = 640;
    o["height"] = 360;
    o["fps"] = 30;
    Project p;
    p.fromJson(o);
    QCOMPARE(p.recordingTracks.size(), 0);
    QVERIFY(p.toJson()["recordingTracks"].isArray());
    QCOMPARE(p.toJson()["recordingTracks"].toArray().size(), 0);
}

void TestSerialization::promoteRecordingTrackMovesItToAudio() {
    Project p;
    p.addTrack(true);
    p.addTrack(true);
    p.addRecordingTrack();
    p.addRecordingTrack();

    Clip c;
    c.id = QStringLiteral("k9");
    c.pos = 1.0;
    c.dur = 3.0;
    p.recordingTracks[1].clips.append(c);
    const QString promotedId = p.recordingTracks[1].id;

    // Solta no meio das faixas de áudio (índice 1).
    p.promoteRecordingTrack(1, 1);

    QCOMPARE(p.recordingTracks.size(), 1);
    QCOMPARE(p.audioTracks.size(), 3);
    QCOMPARE(p.audioTracks[1].id, promotedId);
    // Clipes e identidade sobrevivem à promoção.
    QCOMPARE(p.audioTracks[1].clips.size(), 1);
    QCOMPARE(p.audioTracks[1].clips[0].id, QStringLiteral("k9"));
    // Índice fora de faixa não promove nada.
    p.promoteRecordingTrack(99, 0);
    QCOMPARE(p.recordingTracks.size(), 1);
    QCOMPARE(p.audioTracks.size(), 3);
}

// Presets gravam/leem softness e spill: sem isto, salvar e reaplicar um preset
// devolvia os campos ao default silenciosamente.
void TestSerialization::clipAttrsRoundTripKeepsChromaKeyBands() {
    Clip c;
    c.chromaKey = true;
    c.chromaKeyColor = QColor(0, 0, 255);
    c.chromaKeySimilarity = 0.42;
    c.chromaKeySoftness = 0.37;
    c.chromaKeySpillSuppress = 0.93;

    const QJsonObject o = clipattrs::toJson(c);
    QVERIFY(o.contains(QStringLiteral("chromaKeySoftness")));
    QVERIFY(o.contains(QStringLiteral("chromaKeySpillSuppress")));

    Clip d;
    clipattrs::applyJson(d, o);
    QVERIFY(d.chromaKey);
    QCOMPARE(d.chromaKeyColor, QColor(0, 0, 255));
    QVERIFY(qAbs(d.chromaKeySimilarity - 0.42) < 1e-9);
    QVERIFY(qAbs(d.chromaKeySoftness - 0.37) < 1e-9);
    QVERIFY(qAbs(d.chromaKeySpillSuppress - 0.93) < 1e-9);
}

// Preset antigo (sem as chaves novas) não pode zerar os campos — tem de manter
// o default do struct.
void TestSerialization::clipAttrsLegacyKeepsChromaKeyDefaults() {
    Clip c;
    QJsonObject o = clipattrs::toJson(c);
    o.remove(QStringLiteral("chromaKeySoftness"));
    o.remove(QStringLiteral("chromaKeySpillSuppress"));

    Clip d;
    d.chromaKeySoftness = 0.10;
    d.chromaKeySpillSuppress = 0.5;
    clipattrs::applyJson(d, o);
    QVERIFY(qAbs(d.chromaKeySoftness - 0.10) < 1e-9);
    QVERIFY(qAbs(d.chromaKeySpillSuppress - 0.5) < 1e-9);
}

QTEST_APPLESS_MAIN(TestSerialization)
#include "tst_serialization.moc"
