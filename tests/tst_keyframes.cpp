// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Testes da interpolação de keyframes (kfValue/upsertKeyframe) — lógica pura
// do modelo, sem GUI nem FFmpeg.

#include <QtTest>

#include "colombina/models/Project.h"

class TestKeyframes : public QObject {
    Q_OBJECT

private slots:
    void emptyReturnsBase();
    void beforeFirstReturnsBase();
    void afterLastReturnsLast();
    void linearInterpolation();
    void stepHoldsValue();
    void smoothIsBetween();
    void smoothPairIsSoftS();
    void smoothChainNoWildOvershoot();
    void bezierRespectsEndpoints();
    void upsertInsertsSorted();
    void upsertUpdatesExisting();
    void velocitySrcTime();
    void trackAutomation();
};

void TestKeyframes::emptyReturnsBase() {
    const QVector<Keyframe> keys;
    QVERIFY(qAbs(kfValue(keys, 0.75, 3.0) - 0.75) < 1e-9);
}

void TestKeyframes::beforeFirstReturnsBase() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 5.0, 1.0);
    upsertKeyframe(keys, 10.0, 2.0);
    QVERIFY(qAbs(kfValue(keys, 0.5, 4.0) - 0.5) < 1e-9);
}

void TestKeyframes::afterLastReturnsLast() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 5.0, 1.0);
    upsertKeyframe(keys, 10.0, 2.0);
    QVERIFY(qAbs(kfValue(keys, 0.0, 15.0) - 2.0) < 1e-9);
}

void TestKeyframes::linearInterpolation() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 0.0, 0.0, KfLinear);
    upsertKeyframe(keys, 10.0, 100.0, KfLinear);
    QVERIFY(qAbs(kfValue(keys, 0.0, 5.0) - 50.0) < 1e-9);
    QVERIFY(qAbs(kfValue(keys, 0.0, 2.5) - 25.0) < 1e-9);
}

void TestKeyframes::stepHoldsValue() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 0.0, 10.0, KfStep);
    upsertKeyframe(keys, 10.0, 20.0, KfStep);
    QVERIFY(qAbs(kfValue(keys, 0.0, 5.0) - 10.0) < 1e-9);
    QVERIFY(qAbs(kfValue(keys, 0.0, 9.9) - 10.0) < 1e-9);
    QVERIFY(qAbs(kfValue(keys, 0.0, 10.0) - 20.0) < 1e-9);
}

void TestKeyframes::smoothIsBetween() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 0.0, 0.0, KfSmooth);
    upsertKeyframe(keys, 10.0, 100.0, KfSmooth);
    const double v = kfValue(keys, 0.0, 5.0);
    QVERIFY(v > -1e-6 && v < 100.0 + 1e-6);
}

// Par isolado (2 keys) = S suave, não a "frente carregada" da fórmula antiga
// (controle em span cheio deixava a tangente 3× íngreme). No primeiro terço
// o valor antigo batia ~48; agora deve ficar perto de 30 (ease suave).
void TestKeyframes::smoothPairIsSoftS() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 0.0, 0.0, KfSmooth);
    upsertKeyframe(keys, 10.0, 100.0, KfSmooth);
    const double v1 = kfValue(keys, 0.0, 10.0 / 3.0);
    const double v2 = kfValue(keys, 0.0, 20.0 / 3.0);
    QVERIFY(v1 > 15.0 && v1 < 40.0);   // simétrico: ~29.6
    QVERIFY(v2 > 60.0 && v2 < 85.0);   // ~70.4
    // Monótono no par (sem joelhos exagerados)
    QVERIFY(v2 > v1);
}

// Catmull-Rom composto (3+ keys): overshoot apenas suave — a fórmula antiga
// passava ~20% além dos keyframes (chegava a −70 com keys entre −50 e 100).
void TestKeyframes::smoothChainNoWildOvershoot() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 0.0, 0.0, KfSmooth);
    upsertKeyframe(keys, 1.0, 100.0, KfSmooth);
    upsertKeyframe(keys, 2.0, -50.0, KfSmooth);
    upsertKeyframe(keys, 3.0, 0.0, KfSmooth);
    double lo = 0.0, hi = 0.0;
    for (int i = 0; i <= 300; ++i) {
        const double v = kfValue(keys, 0.0, 3.0 * i / 300.0);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    QVERIFY(lo > -60.0);   // keys mín = −50 → overshoot ≤ 10
    QVERIFY(hi < 105.0);   // keys máx = 100 → overshoot ≤ 5
}

void TestKeyframes::bezierRespectsEndpoints() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 0.0, 0.0, KfBezier);
    upsertKeyframe(keys, 10.0, 100.0, KfBezier);
    // Handles manuais para entortar a curva.
    keys[0].ox = 2.0;
    keys[0].oy = 40.0;
    const double v = kfValue(keys, 0.0, 5.0);
    QVERIFY(v > -1e-6 && v < 100.0 + 1e-6);
    QVERIFY(qAbs(kfValue(keys, 0.0, 0.0) - 0.0) < 1e-9);
    QVERIFY(qAbs(kfValue(keys, 0.0, 10.0) - 100.0) < 1e-9);
}

void TestKeyframes::upsertInsertsSorted() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 10.0, 1.0);
    upsertKeyframe(keys, 0.0, 0.0);
    upsertKeyframe(keys, 5.0, 0.5);
    QCOMPARE(keys.size(), 3);
    QVERIFY(keys[0].time < keys[1].time && keys[1].time < keys[2].time);
    QVERIFY(qAbs(keys[0].time - 0.0) < 1e-9);
    QVERIFY(qAbs(keys[1].time - 5.0) < 1e-9);
    QVERIFY(qAbs(keys[2].time - 10.0) < 1e-9);
}

void TestKeyframes::upsertUpdatesExisting() {
    QVector<Keyframe> keys;
    upsertKeyframe(keys, 0.0, 1.0);
    upsertKeyframe(keys, 5.0, 2.0);
    upsertKeyframe(keys, 5.0, 99.0); // mesmo instante → atualiza
    QCOMPARE(keys.size(), 2);
    QVERIFY(qAbs(keys[1].value - 99.0) < 1e-9);
}

void TestKeyframes::velocitySrcTime() {
    Clip c;
    c.in = 10.0;
    c.dur = 10.0;

    // Velocidade constante (sem envelope): srcTime = in + rel·speed.
    c.speed = 2.0;
    QVERIFY(qAbs(clipSrcTime(c, 5.0) - 20.0) < 1e-6);
    QVERIFY(qAbs(clipSrcTime(c, 0.0) - 10.0) < 1e-6);

    // Rampa linear 1×→3×: speed(u) = 1 + 0.2u, integral = rel + 0.1·rel².
    c.speed = 1.0;
    upsertKeyframe(c.kfSpeed, 0.0, 1.0, KfLinear);
    upsertKeyframe(c.kfSpeed, 10.0, 3.0, KfLinear);
    QVERIFY(qAbs(clipSpeedAt(c, 5.0) - 2.0) < 1e-6);
    QVERIFY(qAbs(clipSrcTime(c, 5.0) - (10.0 + 5.0 + 0.1 * 25.0)) < 1e-4);
    QVERIFY(qAbs(clipSrcTime(c, 10.0) - (10.0 + 10.0 + 0.1 * 100.0)) < 1e-4);
    QVERIFY(hasVelocityEnvelope(c));
}

void TestKeyframes::trackAutomation() {
    Track tr;
    tr.volume = 1.0;
    tr.pan = 0.0;

    // Sem envelope: valor estático.
    QVERIFY(!tr.hasAutomation());
    QVERIFY(qAbs(tr.automationVolume(2.0) - 1.0) < 1e-9);
    QVERIFY(qAbs(tr.automationPan(2.0) - 0.0) < 1e-9);

    // Envelope de volume: 1.0 em 0s → 0.5 em 4s.
    upsertKeyframe(tr.kfVolume, 0.0, 1.0, KfLinear);
    upsertKeyframe(tr.kfVolume, 4.0, 0.5, KfLinear);
    QVERIFY(tr.hasAutomation());
    QVERIFY(qAbs(tr.automationVolume(2.0) - 0.75) < 1e-6);

    // Envelope de pan: -1 → +1.
    upsertKeyframe(tr.kfPan, 0.0, -1.0, KfLinear);
    upsertKeyframe(tr.kfPan, 4.0, 1.0, KfLinear);
    QVERIFY(qAbs(tr.automationPan(2.0) - 0.0) < 1e-6);
}

QTEST_APPLESS_MAIN(TestKeyframes)
#include "tst_keyframes.moc"
