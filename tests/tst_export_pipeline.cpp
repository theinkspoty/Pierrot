// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste REAL de exportação (caminho feliz): monta um projeto com clipes
// geradores (sem mídia em disco), roda o comando ffmpeg do ProjectExporter de
// verdade e extrai um quadro do arquivo resultante. O quadro é comparado com o
// `generatorFrame()` — a MESMA função que o preview usa — fechando a paridade
// preview ↔ exportação que os testes anteriores não cobriam.
//
// Se o `ffmpeg` não estiver no PATH, os testes são pulados (QSKIP).

#include <QtTest>

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSettings>

#include <cmath>

#include "colombina/models/Project.h"
#include "colombina/export/ProjectExporter.h"
#include "colombina/generators.h"

namespace {

// Diferença máxima por canal aceita entre o quadro exportado e o esperado.
// O libx264 usa YUV 4:2:0: bordas de transição têm ringing, mas centros de
// célula (longe das bordas) devem reposicionar as cores quase exatas.
const int kTol = 35;

// Executa `args` (já incluindo "-y" etc.) com o binário `program`. Retorna
// o exit code ou -1 se não terminou no tempo.
int runCmd(const QString& program, const QStringList& args, int timeoutMs = 30000) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(5000) || !p.waitForFinished(timeoutMs))
        return -1;
    return p.exitCode();
}

// Roda o ffmpeg e extrai um quadro em `t` (segundos) para um PNG temporário.
bool extractFrame(const QString& ffmpeg, const QString& media, double t,
                  const QString& outPng) {
    const QStringList args {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
        QStringLiteral("error"), QStringLiteral("-ss"), QString::number(t, 'f', 6),
        QStringLiteral("-i"), media, QStringLiteral("-frames:v"), QStringLiteral("1"),
        outPng
    };
    return runCmd(ffmpeg, args) == 0 && QFile::exists(outPng);
}

// Cria um item de mídia gerador com um id determinístico.
MediaItem makeGen(const QString& id, const QString& gen, const QColor& a,
                  const QColor& b, int cells = 4) {
    MediaItem m;
    m.id = id;
    m.name = id;
    m.generator = gen;
    m.solidColor = a;
    m.solidColor2 = b;
    m.genCells = cells;
    m.isSolid = true;
    m.hasVideo = true;
    m.width = 320;
    m.height = 180;
    m.duration = 2.0;
    return m;
}

} // namespace

class TestExportPipeline : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void solidColorExportMatches();
    void checkerboardExportMatchesPreview();
    void exportedDurationIsTimelineDuration();

private:
    QString m_ffmpeg;
    QString m_ffprobe;
    QTemporaryDir m_dir;
};

void TestExportPipeline::initTestCase() {
    // Força codificação por software (libx264) independente da config do usuário,
    // para o teste ser determinístico no CI e em qualquer máquina.
    QSettings().setValue(QStringLiteral("exportHwEncode"), false);
    m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    m_ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (m_ffmpeg.isEmpty())
        QSKIP("ffmpeg não encontrado no PATH — pulando testes de exportação real.");
    if (!m_dir.isValid())
        QSKIP("falhou criar diretório temporário.");
}

void TestExportPipeline::solidColorExportMatches() {
    Project p;
    p.width = 320;
    p.height = 180;
    p.fps = 30;

    const QColor color(30, 140, 220); // azul médio (não RGB puro: mais próximo do YUV)
    p.media.append(makeGen(QStringLiteral("gen-solid"), QString(), color, QColor()));

    p.addTrack(false);
    Clip c;
    c.id = QStringLiteral("c1");
    c.mediaId = p.media[0].id;
    c.pos = 0.0;
    c.in = 0.0;
    c.dur = 1.0;
    p.videoTracks[0].clips.append(c);

    const QString outPath = m_dir.filePath(QStringLiteral("solid.mp4"));
    ExportSettings s;
    s.outputPath = outPath;
    s.width = 320;
    s.height = 180;
    s.fps = 30;

    QString err;
    const QStringList cmd = ProjectExporter::buildCommand(p, s, &err);
    QVERIFY2(!cmd.isEmpty(), qPrintable(err));
    QCOMPARE(runCmd(m_ffmpeg, cmd), 0);

    const QString framePng = m_dir.filePath(QStringLiteral("solid.png"));
    QVERIFY(extractFrame(m_ffmpeg, outPath, 0.5, framePng));
    QImage frame(framePng);
    QCOMPARE(frame.width(), 320);
    QCOMPARE(frame.height(), 180);

    // Amostra o centro e 4 pontos próximos ao centro: tudo deve ser a cor sólida.
    const QPoint pts[] = { QPoint(160, 90), QPoint(40, 40), QPoint(280, 40),
                           QPoint(40, 140), QPoint(280, 140) };
    for (const QPoint& pPt : pts) {
        const QColor got = frame.pixelColor(pPt);
        QVERIFY2(std::abs(got.red() - color.red()) <= kTol
                     && std::abs(got.green() - color.green()) <= kTol
                     && std::abs(got.blue() - color.blue()) <= kTol,
                 qPrintable(QStringLiteral("pixel em (%1,%2) = %3 (%4,%5,%6), esperado %7 (%8,%9,%10)")
                                .arg(pPt.x()).arg(pPt.y())
                                .arg(got.name()).arg(got.red()).arg(got.green()).arg(got.blue())
                                .arg(color.name()).arg(color.red()).arg(color.green()).arg(color.blue())));
    }
}

void TestExportPipeline::checkerboardExportMatchesPreview() {
    Project p;
    p.width = 320;
    p.height = 180;
    p.fps = 30;

    const QColor cA(255, 0, 0);       // vermelho
    const QColor cB(255, 255, 255);   // branco
    const int cells = 4;
    MediaItem m = makeGen(QStringLiteral("gen-check"), QStringLiteral("checkerboard"),
                          cA, cB, cells);
    p.media.append(m);

    p.addTrack(false);
    Clip c;
    c.id = QStringLiteral("c2");
    c.mediaId = p.media[0].id;
    c.pos = 0.0;
    c.in = 0.0;
    c.dur = 1.0;
    p.videoTracks[0].clips.append(c);

    const QString outPath = m_dir.filePath(QStringLiteral("check.mp4"));
    ExportSettings s;
    s.outputPath = outPath;
    s.width = 320;
    s.height = 180;
    s.fps = 30;

    QString err;
    const QStringList cmd = ProjectExporter::buildCommand(p, s, &err);
    QVERIFY2(!cmd.isEmpty(), qPrintable(err));
    QCOMPARE(runCmd(m_ffmpeg, cmd), 0);

    const QString framePng = m_dir.filePath(QStringLiteral("check.png"));
    QVERIFY(extractFrame(m_ffmpeg, outPath, 0.5, framePng));
    QImage frame(framePng);
    QCOMPARE(frame.width(), 320);
    QCOMPARE(frame.height(), 180);

    // Ground truth do preview: generatorFrame() no tamanho do projeto.
    const QImage expected = generatorFrame(m, 320, 180);
    QCOMPARE(expected.width(), frame.width());
    QCOMPARE(expected.height(), frame.height());

    // Compara por célula (amostrando o centro de cada célula, longe de bordas
    // onde o YUV 4:2:0 cria ringing). 4×4 células de 80×45 px.
    const int cw = 320 / cells;
    const int ch = 180 / cells;
    long long badPixels = 0;
    for (int i = 0; i < cells; ++i) {
        for (int j = 0; j < cells; ++j) {
            const int x = i * cw + cw / 2;
            const int y = j * ch + ch / 2;
            const QColor exp = expected.pixelColor(x, y);
            const QColor got = frame.pixelColor(x, y);
            if (std::abs(exp.red() - got.red()) > kTol
                || std::abs(exp.green() - got.green()) > kTol
                || std::abs(exp.blue() - got.blue()) > kTol) {
                ++badPixels;
                qWarning() << "célula" << i << j << "@" << x << y
                           << "esperado" << exp.name() << "obtido" << got.name();
            }
        }
    }
    QCOMPARE(badPixels, 0LL);
}

void TestExportPipeline::exportedDurationIsTimelineDuration() {
    Project p;
    p.width = 320;
    p.height = 180;
    p.fps = 30;
    p.media.append(makeGen(QStringLiteral("gen-dur"), QString(), QColor(0, 120, 0), QColor()));

    p.addTrack(false);
    Clip c;
    c.id = QStringLiteral("c3");
    c.mediaId = p.media[0].id;
    c.pos = 0.0;
    c.in = 0.0;
    c.dur = 2.0;
    p.videoTracks[0].clips.append(c);

    const QString outPath = m_dir.filePath(QStringLiteral("dur.mp4"));
    ExportSettings s;
    s.outputPath = outPath;
    s.width = 320;
    s.height = 180;
    s.fps = 30;

    QString err;
    const QStringList cmd = ProjectExporter::buildCommand(p, s, &err);
    QVERIFY2(!cmd.isEmpty(), qPrintable(err));
    QCOMPARE(runCmd(m_ffmpeg, cmd), 0);

    if (m_ffprobe.isEmpty()) {
        // Sem ffprobe: confere ao menos que o arquivo existe e tem tamanho.
        QVERIFY(QFileInfo(outPath).size() > 0);
        return;
    }
    QProcess probe;
    probe.start(m_ffprobe, {
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
        QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
        outPath
    });
    QVERIFY(probe.waitForFinished(15000));
    bool ok = false;
    const double dur = probe.readAllStandardOutput().trimmed().toDouble(&ok);
    QVERIFY(ok);
    // Timeline = 2.0 s; tolerância de 10% (duração de contêiner tem arredondamento).
    QVERIFY2(std::abs(dur - 2.0) < 0.2,
             qPrintable(QStringLiteral("duração exportada %1 s, esperada ~2.0 s").arg(dur, 0, 'f', 3)));
}

QTEST_APPLESS_MAIN(TestExportPipeline)
#include "tst_export_pipeline.moc"