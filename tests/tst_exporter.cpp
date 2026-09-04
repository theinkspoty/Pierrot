// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Testes do ProjectExporter::buildCommand nos caminhos de ERRO — determinísticos
// e sem tocar o sistema de arquivos nem rodar o ffmpeg CLI. O caminho "feliz"
// (comando completo) é testado em outro lugar, pois depende de mídia real,
// encoders e renderização Mesa.

#include <QtTest>

#include "export/ProjectExporter.h"

class TestExporter : public QObject {
    Q_OBJECT

private slots:
    void emptyOutputPathFails();
    void emptyTimelineFails();
};

void TestExporter::emptyOutputPathFails() {
    Project p;
    p.addTrack(false);
    ExportSettings s;
    s.outputPath.clear();

    QString error;
    const QStringList cmd = ProjectExporter::buildCommand(p, s, &error);

    QVERIFY(cmd.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestExporter::emptyTimelineFails() {
    Project p; // sem faixas, sem clipes → duration() == 0
    ExportSettings s;
    s.outputPath = QStringLiteral("/tmp/saida.mp4");

    QString error;
    const QStringList cmd = ProjectExporter::buildCommand(p, s, &error);

    QVERIFY(cmd.isEmpty());
    QVERIFY(!error.isEmpty());
    QCOMPARE(p.duration(), 0.0);
}

QTEST_APPLESS_MAIN(TestExporter)
#include "tst_exporter.moc"
