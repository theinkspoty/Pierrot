// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Testes de Math3D (Fase 0 da Mesa 3D) — lógica pura, sem GUI.

#include <QtTest>

#include "colombina/render/Math3D.h"
#include "colombina/models/Project.h"

using namespace math3d;

class TestMath3d : public QObject {
    Q_OBJECT

private slots:
    void identityMultiply();
    void translatePoint();
    void scalePoint();
    void rotateZ90();
    void perspectiveProjectsForward();
    void lookAtFacesCenter();
    void mesa3dDefaultsAre2D();
    void mesaFieldsSerialize();
};

void TestMath3d::identityMultiply() {
    const Mat4 a = Mat4::identity();
    const Mat4 b = Mat4::translate({1, 2, 3});
    const Mat4 r = a * b;
    QVERIFY(qAbs(r.m[12] - 1.0) < 1e-9);
    QVERIFY(qAbs(r.m[13] - 2.0) < 1e-9);
    QVERIFY(qAbs(r.m[14] - 3.0) < 1e-9);
}

void TestMath3d::translatePoint() {
    const Mat4 t = Mat4::translate({10, 20, 30});
    const Vec3 p = t.transformPoint({1, 2, 3});
    QVERIFY(qAbs(p.x - 11.0) < 1e-9);
    QVERIFY(qAbs(p.y - 22.0) < 1e-9);
    QVERIFY(qAbs(p.z - 33.0) < 1e-9);
}

void TestMath3d::scalePoint() {
    const Mat4 s = Mat4::scale({2, 3, 4});
    const Vec3 p = s.transformPoint({1, 1, 1});
    QVERIFY(qAbs(p.x - 2.0) < 1e-9);
    QVERIFY(qAbs(p.y - 3.0) < 1e-9);
    QVERIFY(qAbs(p.z - 4.0) < 1e-9);
}

void TestMath3d::rotateZ90() {
    // Rotação 90° em Z: (1,0,0) → (0,1,0) na convenção horária de +Z.
    const Mat4 r = Mat4::rotateAxis(2, 90.0);
    const Vec3 p = r.transformPoint({1, 0, 0});
    QVERIFY(qAbs(p.x) < 1e-6);
    QVERIFY(qAbs(p.y - 1.0) < 1e-6);
    QVERIFY(qAbs(p.z) < 1e-6);
}

void TestMath3d::perspectiveProjectsForward() {
    const Mat4 proj = Mat4::perspective(50.0, 16.0 / 9.0);
    // Ponto à frente da câmera (z negativo no eye-space OpenGL).
    const Vec3 near = proj.transformPoint({0, 0, -1});
    const Vec3 far = proj.transformPoint({0, 0, -100});
    // w deve ser negativo para pontos à frente; após divisão, |z_ndc| < 1
    // para o near plane razoável.
    QVERIFY(std::fabs(near.z) <= 1.0 + 1e-6);
    QVERIFY(std::fabs(far.z) <= 1.0 + 1e-6);
}

void TestMath3d::lookAtFacesCenter() {
    const Vec3 eye(0, 0, 10);
    const Vec3 center(0, 0, 0);
    const Vec3 up(0, 1, 0);
    const Mat4 view = Mat4::lookAt(eye, center, up);
    const Vec3 c = view.transformPoint(center);
    // Eye-space OpenGL: olhando para o centro, ele cai no eixo -Z
    // (à frente da câmera); x e y devem ser ~0.
    QVERIFY(qAbs(c.x) < 1e-6);
    QVERIFY(qAbs(c.y) < 1e-6);
    QVERIFY(c.z < 0.0); // 10 unidades à frente (eye em z=+10)
    QVERIFY(qAbs(c.z + 10.0) < 1e-6);
}

void TestMath3d::mesa3dDefaultsAre2D() {
    MesaComposition m;
    QVERIFY(!m.mesa3d);
    QCOMPARE(m.camFov, 50.0);
    QCOMPARE(m.camZ, 0.0);
    QCOMPARE(m.camPitch, 0.0);
    Track t;
    QCOMPARE(t.mesaZ, 0.0);
    QCOMPARE(t.mesaRotX, 0.0);
    QVERIFY(!t.mesaAcceptsLights);
}

void TestMath3d::mesaFieldsSerialize() {
    MesaComposition m;
    m.id = "mesa3d";
    m.mesa3d = true;
    m.camZ = 100.0;
    m.camFov = 35.0;
    m.camPitch = 10.0;
    m.camYaw = -5.0;
    m.camPoiX = 10.0;
    Track t;
    t.id = "tr3d";
    t.mesaZ = 50.0;
    t.mesaRotX = 15.0;
    t.mesaAcceptsLights = true;

    Project p;
    p.mesas.append(m);
    p.videoTracks.append(t);

    const QJsonObject json = p.toJson();
    Project q;
    q.fromJson(json);

    QCOMPARE(q.mesas.size(), 1);
    QVERIFY(q.mesas[0].mesa3d);
    QVERIFY(qAbs(q.mesas[0].camZ - 100.0) < 1e-9);
    QVERIFY(qAbs(q.mesas[0].camFov - 35.0) < 1e-9);
    QVERIFY(qAbs(q.mesas[0].camPitch - 10.0) < 1e-9);
    QCOMPARE(q.videoTracks.size(), 1);
    QVERIFY(qAbs(q.videoTracks[0].mesaZ - 50.0) < 1e-9);
    QVERIFY(q.videoTracks[0].mesaAcceptsLights);
}

QTEST_APPLESS_MAIN(TestMath3d)
#include "tst_math3d.moc"
