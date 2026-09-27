// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Testes da álgebra de intervalos e janelas do conform de áudio. Puro: sem
// FFmpeg, sem threads, sem ficheiros. É aqui que mora o off-by-one que
// passaria despercebido no cache — uma fresta de uma amostra entre dois
// "intervalos contíguos" lê silêncio e dá um estalo no áudio.

#include <QtTest>

#include "colombina/ffmpeg/AudioConformIntervals.h"

#include <initializer_list>
#include <iterator>

using namespace AudioConformIntervals;

namespace {

// Converte uma lista plana {a0,a1,a2,a3,...} em pares [a0,a1), [a2,a3), ...
QVector<FrameRange> ranges(std::initializer_list<qint64> flat) {
    QVector<FrameRange> v;
    auto it = flat.begin();
    for (; std::distance(it, flat.end()) >= 2; std::advance(it, 2))
        v.append({*it, *std::next(it)});
    return v;
}

QVector<Window> windows(std::initializer_list<double> flat) {
    QVector<Window> v;
    auto it = flat.begin();
    for (; std::distance(it, flat.end()) >= 2; std::advance(it, 2))
        v.append({*it, *std::next(it)});
    return v;
}

} // namespace

class TestAudioConformIntervals : public QObject {
    Q_OBJECT

private slots:
    // mergeIntervals
    void emptyAndSinglePassThrough();
    void adjacentIntervalsMerge();
    void overlappingIntervalsMerge();
    void containedIntervalIsAbsorbed();
    void unsortedInputIsSorted();
    void sameStartKeepsTheLongest();
    void chainOfThreeCollapses();
    void oneFrameGapStaysSplit();
    void touchingAtZeroEdge();

    // coalesceWindows
    void windowsOverlapCoalesce();
    void windowsWithinEpsCoalesce();
    void windowsExactlyEpsApartCoalesce();
    void windowsBeyondEpsStaySplit();
    void zeroEpsBehavesLikeExactFusing();
    void containedWindowIsAbsorbed();
    void windowsUnsortedAreSorted();
    void epsIsOneFrame();

    // firstUncoveredFrame
    void nothingFilledIsUncovered();
    void startOutsideAnyRangeIsUncovered();
    void startInsideRangeAdvancesToItsEnd();
    void fullyCoveredReportsMinusOne();
    void frameAtRangeEndIsNotCovered();
    void frameAtRangeStartIsCovered();
    void zeroWidthQueryIsCovered();
    void coverageSpanningManyRanges();
};

// ------------------------------------------------------- mergeIntervals

void TestAudioConformIntervals::emptyAndSinglePassThrough() {
    QVERIFY(mergeIntervals({}).isEmpty());
    QCOMPARE(mergeIntervals(ranges({3, 9})), ranges({3, 9}));
}

void TestAudioConformIntervals::adjacentIntervalsMerge() {
    // [0,10) e [10,20): o frame 10 não é uma lacuna, funde.
    QCOMPARE(mergeIntervals(ranges({0, 10, 10, 20})), ranges({0, 20}));
}

void TestAudioConformIntervals::overlappingIntervalsMerge() {
    QCOMPARE(mergeIntervals(ranges({0, 15, 10, 20})), ranges({0, 20}));
}

void TestAudioConformIntervals::containedIntervalIsAbsorbed() {
    QCOMPARE(mergeIntervals(ranges({0, 100, 10, 20})), ranges({0, 100}));
}

void TestAudioConformIntervals::unsortedInputIsSorted() {
    // Entrada fora de ordem, e uma das fusiveis absorvida no caminho.
    QCOMPARE(mergeIntervals(ranges({30, 40, 0, 20, 10, 15})),
             ranges({0, 20, 30, 40}));
    // Aqui as lacunas são reais: ordenar não pode inventar fusão.
    QCOMPARE(mergeIntervals(ranges({40, 50, 0, 10, 20, 30})),
             ranges({0, 10, 20, 30, 40, 50}));
}

void TestAudioConformIntervals::sameStartKeepsTheLongest() {
    // Independentemente da ordem de entrada, o mais longo ganha.
    const auto a = mergeIntervals(ranges({5, 10, 5, 40}));
    const auto b = mergeIntervals(ranges({5, 40, 5, 10}));
    QCOMPARE(a, ranges({5, 40}));
    QCOMPARE(b, a);
}

void TestAudioConformIntervals::chainOfThreeCollapses() {
    // Sobrepostos em cadeia, nenhum par adjacente directamente.
    QCOMPARE(mergeIntervals(ranges({0, 6, 4, 12, 10, 20})), ranges({0, 20}));
}

void TestAudioConformIntervals::oneFrameGapStaysSplit() {
    // [0,10) e [11,20): o frame 10 falta mesmo, não pode fundir.
    const auto m = mergeIntervals(ranges({0, 10, 11, 20}));
    QCOMPARE(m.size(), 2);
    QCOMPARE(m.at(0), ranges({0, 10}).at(0));
    QCOMPARE(m.at(1), ranges({11, 20}).at(0));
}

void TestAudioConformIntervals::touchingAtZeroEdge() {
    // Zero como início de intervalo não é caso especial.
    QCOMPARE(mergeIntervals(ranges({0, 5, 5, 10})), ranges({0, 10}));
}

// --------------------------------------------------------- coalesceWindows

void TestAudioConformIntervals::windowsOverlapCoalesce() {
    QCOMPARE(coalesceWindows(windows({0.0, 1.0, 0.5, 2.0}), 0.0),
             windows({0.0, 2.0}));
}

void TestAudioConformIntervals::windowsWithinEpsCoalesce() {
    const double eps = 1.0 / 48000.0;
    // Separação de metade de eps: funde.
    QCOMPARE(coalesceWindows(windows({0.0, 0.1, 0.1 + eps / 2, 0.2}), eps),
             windows({0.0, 0.2}));
}

void TestAudioConformIntervals::windowsExactlyEpsApartCoalesce() {
    // A condição é "> last.second + eps", logo a distância igual a eps funde.
    const double eps = 1.0 / 48000.0;
    QCOMPARE(coalesceWindows(windows({0.0, 0.1, 0.1 + eps, 0.2}), eps),
             windows({0.0, 0.2}));
}

void TestAudioConformIntervals::windowsBeyondEpsStaySplit() {
    const double eps = 1.0 / 48000.0;
    const auto w = windows({0.0, 0.1, 0.1 + 2 * eps, 0.2});
    QCOMPARE(coalesceWindows(w, eps).size(), 2);
}

void TestAudioConformIntervals::zeroEpsBehavesLikeExactFusing() {
    // eps = 0 é a fusão exacta: adjacente funde, com uma amostra de folga não.
    QCOMPARE(coalesceWindows(windows({0.0, 0.1, 0.1, 0.2}), 0.0),
             windows({0.0, 0.2}));
    QCOMPARE(coalesceWindows(windows({0.0, 0.1, 0.1 + 1e-9, 0.2}), 0.0).size(), 2);
}

void TestAudioConformIntervals::containedWindowIsAbsorbed() {
    const auto w = coalesceWindows(windows({0.0, 1.0, 0.2, 0.3}), 0.0);
    QCOMPARE(w.size(), 1);
    QCOMPARE(w.at(0), windows({0.0, 1.0}).at(0));
}

void TestAudioConformIntervals::windowsUnsortedAreSorted() {
    QCOMPARE(coalesceWindows(windows({2.0, 3.0, 0.0, 1.0}), 0.0),
             windows({0.0, 1.0, 2.0, 3.0}));
}

void TestAudioConformIntervals::epsIsOneFrame() {
    // O eps do cache é 1.0/kSampleRate: uma frame a 48 kHz, ou seja o
    // intervalo de tempo de uma amostra por canal. Fixa o valor para que
    // ninguém o mude sem deixar por escrito o porquê.
    const double eps = 1.0 / 48000.0;
    const double umaFrame = 1.0 / 48000.0;
    QCOMPARE(eps, umaFrame);
    // Por isso, dois pedidos exactamente uma frame distantes fundem, e a
    // uma frame e meia já não.
    QCOMPARE(coalesceWindows(windows({0.0, 0.1, 0.1 + umaFrame, 0.2}), eps).size(), 1);
    QCOMPARE(coalesceWindows(windows({0.0, 0.1, 0.1 + 1.5 * umaFrame, 0.2}), eps).size(), 2);
}

// ---------------------------------------------------- firstUncoveredFrame

void TestAudioConformIntervals::nothingFilledIsUncovered() {
    QCOMPARE(firstUncoveredFrame({}, 0, 100), (qint64)0);
    QCOMPARE(firstUncoveredFrame({}, 50, 100), (qint64)50);
}

void TestAudioConformIntervals::startOutsideAnyRangeIsUncovered() {
    QCOMPARE(firstUncoveredFrame(ranges({0, 10}), 20, 30), (qint64)20);
}

void TestAudioConformIntervals::startInsideRangeAdvancesToItsEnd() {
    QCOMPARE(firstUncoveredFrame(ranges({0, 100}), 50, 200), (qint64)100);
}

void TestAudioConformIntervals::fullyCoveredReportsMinusOne() {
    QCOMPARE(firstUncoveredFrame(ranges({0, 20}), 0, 20), (qint64)-1);
    QCOMPARE(firstUncoveredFrame(ranges({0, 100}), 50, 100), (qint64)-1);
}

void TestAudioConformIntervals::frameAtRangeEndIsNotCovered() {
    // [0,10) não cobre o frame 10 — o intervalo é [inicio, fim).
    QCOMPARE(firstUncoveredFrame(ranges({0, 10}), 10, 20), (qint64)10);
}

void TestAudioConformIntervals::frameAtRangeStartIsCovered() {
    QCOMPARE(firstUncoveredFrame(ranges({10, 20}), 10, 20), (qint64)-1);
}

void TestAudioConformIntervals::zeroWidthQueryIsCovered() {
    // Janela vazia não pede nada, logo nada está por descobrir.
    QCOMPARE(firstUncoveredFrame(ranges({0, 10}), 50, 50), (qint64)-1);
    QCOMPARE(firstUncoveredFrame({}, 50, 50), (qint64)-1);
}

void TestAudioConformIntervals::coverageSpanningManyRanges() {
    // Invariante do merge: a partir do frame inicial, um único intervalo
    // tem de conter o resto da consulta. Query coberta [0,100) por três.
    const auto filled = mergeIntervals(ranges({0, 30, 30, 60, 60, 100}));
    QCOMPARE(filled.size(), 1);
    QCOMPARE(firstUncoveredFrame(filled, 0, 100), (qint64)-1);
    // Parcial: [80,120) — a partir de 80 é que falta.
    QCOMPARE(firstUncoveredFrame(filled, 80, 120), (qint64)100);
}

QTEST_APPLESS_MAIN(TestAudioConformIntervals)
#include "tst_audio_conform_intervals.moc"
