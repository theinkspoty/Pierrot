// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Testes do gerador de forma (`generatorFrame` com generator == "shape"):
// a MESMA função usada pelo Preview, Pantalone, Timeline e exportação. Render
// offscreen com amostragem de pixels.

#include <QtTest>

#include "colombina/generators.h"
#include "colombina/models/Project.h"

namespace {
MediaItem makeShape(const QString& type) {
    MediaItem m;
    m.id = QStringLiteral("shape1");
    m.name = QStringLiteral("Forma");
    m.isSolid = true;
    m.hasVideo = true;
    m.generator = QStringLiteral("shape");
    m.shapeType = type;
    m.solidColor = QColor(255, 0, 0);
    m.solidColor2 = QColor(255, 255, 255);
    m.shapeW = 0.5;
    m.shapeH = 0.5;
    m.width = 100;
    m.height = 100;
    m.duration = 2.0;
    return m;
}
} // namespace

class TestShapeGenerator : public QObject {
    Q_OBJECT

private slots:
    void rectFillsCenter();
    void ellipseLeavesCornersEmpty();
    void strokeOnlyDrawsBorder();
    void transparentOutsideShape();
    void roundedRectClipsCorner();
};

void TestShapeGenerator::rectFillsCenter() {
    const MediaItem m = makeShape(QStringLiteral("rect"));
    const QImage img = generatorFrame(m, 100, 100);
    QCOMPARE(img.size(), QSize(100, 100));
    // A forma ocupa 50% central: (50,50) é fill.
    QCOMPARE(img.pixelColor(50, 50), QColor(255, 0, 0));
}

void TestShapeGenerator::ellipseLeavesCornersEmpty() {
    const MediaItem m = makeShape(QStringLiteral("ellipse"));
    const QImage img = generatorFrame(m, 100, 100);
    QCOMPARE(img.pixelColor(50, 50), QColor(255, 0, 0));
    // Canto superior-esquerdo do bounding box (25,25) fica fora da elipse.
    QCOMPARE(img.pixelColor(25, 25).alpha(), 0);
}

void TestShapeGenerator::strokeOnlyDrawsBorder() {
    MediaItem m = makeShape(QStringLiteral("rect"));
    m.shapeStrokeWidth = 4.0;
    const QImage img = generatorFrame(m, 100, 100);
    // Centro continua fill; a borda esquerda (x=25) vira stroke branco.
    QCOMPARE(img.pixelColor(50, 50), QColor(255, 0, 0));
    const QColor edge = img.pixelColor(26, 50);
    QVERIFY(edge.green() > 200 && edge.blue() > 200);
}

void TestShapeGenerator::transparentOutsideShape() {
    const MediaItem m = makeShape(QStringLiteral("rect"));
    const QImage img = generatorFrame(m, 100, 100);
    // Fora da forma (centro 50%): canto transparente.
    QCOMPARE(img.pixelColor(5, 5).alpha(), 0);
}

void TestShapeGenerator::roundedRectClipsCorner() {
    MediaItem m = makeShape(QStringLiteral("rect"));
    m.shapeRoundness = 0.5;
    const QImage img = generatorFrame(m, 100, 100);
    // Com cantos muito arredondados (círculo inscrito), o canto do bbox fica
    // transparente enquanto o centro permanece preenchido.
    QCOMPARE(img.pixelColor(25, 25).alpha(), 0);
    QCOMPARE(img.pixelColor(50, 50), QColor(255, 0, 0));
}

QTEST_APPLESS_MAIN(TestShapeGenerator)
#include "tst_shape_generator.moc"
