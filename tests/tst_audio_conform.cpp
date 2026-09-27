// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Testes do AudioConformCache — o contrato que o MixerWidget consome:
// leitura posicional de um buffer PCM com falhas (regiões ainda não
// decodificadas) e refcounting manual do buffer.
//
// Não toca em FFmpeg: o worker fica à espera porque nunca pedimos janelas
// (wanted vazio), e as regiões são plantadas à mão em Chunk::filled. Os
// ficheiros não existem — o cache só indexa o áudio quando o worker
// descodifica, o que não acontece aqui.

#include <QtTest>

#include "colombina/ffmpeg/AudioConformCache.h"

using Chunk = AudioConformCache::Chunk;
using Ref = AudioConformCache::Ref;

namespace {

// Caminho único por teste: o cache é um singleton com registo persistente,
// por isso dois testes que partilhassem a chave veriam o mesmo buffer.
int g_seq = 0;
QString uniquePath(const char* tag) {
    return QStringLiteral("/nonexistent/tst_audio_conform_%1_%2.wav")
        .arg(QString::fromLatin1(tag))
        .arg(++g_seq);
}

// Enche [start, start+frames) com um valor derivado de start, para
// distinguir um zero válido de uma lacuna real.
void plant(Chunk* c, qint64 start, qint64 frames, int16_t base = 1000) {
    const qint64 need = (start + frames) * AudioConformCache::kChannels;
    if (c->pcm.size() < need) c->pcm.resize((int)need, 0);
    for (qint64 f = start; f < start + frames; ++f) {
        c->pcm[(int)(f * AudioConformCache::kChannels)] = (int16_t)(base + f);
        c->pcm[(int)(f * AudioConformCache::kChannels) + 1] = (int16_t)(base + f);
    }
    c->filled.append({start, start + frames});
}

QVector<int16_t> zeros(int frames) {
    return QVector<int16_t>(frames * AudioConformCache::kChannels, 0);
}

} // namespace

class TestAudioConformCache : public QObject {
    Q_OBJECT

private slots:
    // get() / registo
    void sameKeyReturnsSameChunk();
    void differentStreamIsDifferentChunk();
    void getRefreshesLastUse();

    // Ref (RAII do refcount)
    void defaultRefIsNull();
    void copyAddAndReleaseRef();
    void assignMovesRef();
    void selfAssignKeepsRef();
    void registryRefKeepsChunkAlive();

    // readFrames: falhas e silêncio
    void readEmptyRegionReturnsZero();
    void readStopsAtFirstGap();
    void readSkipsGapAndContinuesAfter();
    void readFromInsideInterval();
    void readClampsToFilledEnd();
    void readNonPositiveMaxReturnsZero();
    void readNullCacheReturnsZero();
    void readZeroesTailAfterGap();

    // waitReady
    void waitReadyZeroFramesIsInstant();
    void waitReadyNullCacheFails();
    void waitReadyCoveredRegionSucceeds();
    void waitReadyUncoveredTimesOut();

    // request / diagnóstico
    void requestIgnoresInvalidInput();
    void totalBytesStartsAtZero();
};

// ---------------------------------------------------------------- get()

void TestAudioConformCache::sameKeyReturnsSameChunk() {
    auto& cache = AudioConformCache::instance();
    Ref a = cache.get(uniquePath("same"), 0);
    Ref b = cache.get(QStringLiteral("/nonexistent/tst_audio_conform_same_1.wav"), 0);
    QVERIFY(a);
    QVERIFY(b);
    QCOMPARE(a.get(), b.get());
}

void TestAudioConformCache::differentStreamIsDifferentChunk() {
    auto& cache = AudioConformCache::instance();
    const QString path = uniquePath("stream");
    Ref a = cache.get(path, 0);
    Ref b = cache.get(path, 1);
    QVERIFY(a);
    QVERIFY(b);
    QVERIFY(a.get() != b.get());
    QCOMPARE(b->stream, 1);
}

void TestAudioConformCache::getRefreshesLastUse() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("lastuse"), 0);
    const qint64 before = r->lastUseMs;
    r->lastUseMs = 0; // "esquecido"
    cache.get(r->filePath, r->stream);
    QVERIFY(r->lastUseMs > 0);
    Q_UNUSED(before)
}

// ---------------------------------------------------------------- Ref()

void TestAudioConformCache::defaultRefIsNull() {
    Ref r;
    QVERIFY(!r);
    QVERIFY(r.get() == nullptr);
}

void TestAudioConformCache::copyAddAndReleaseRef() {
    auto& cache = AudioConformCache::instance();
    Ref a = cache.get(uniquePath("copy"), 0);
    Chunk* c = a.get();
    const int base = c->refs.load();
    {
        Ref b = a; // cópia deve somar uma ref
        QCOMPARE(c->refs.load(), base + 1);
    }
    QCOMPARE(c->refs.load(), base); // destructor devolve
}

void TestAudioConformCache::assignMovesRef() {
    auto& cache = AudioConformCache::instance();
    Ref a = cache.get(uniquePath("assign"), 0);
    Ref b;
    b = a;
    Chunk* c = a.get();
    QCOMPARE(c->refs.load(), 3); // 1 registo + a + b
    Ref empty;
    b = empty; // solta o chunk
    QCOMPARE(c->refs.load(), 2); // 1 registo + a
}

void TestAudioConformCache::selfAssignKeepsRef() {
    auto& cache = AudioConformCache::instance();
    Ref a = cache.get(uniquePath("self"), 0);
    Chunk* c = a.get();
    const int base = c->refs.load();
    Ref& alias = a;
    a = alias; // não deve libertar nem duplicar
    QCOMPARE(c->refs.load(), base);
    QVERIFY(a);
}

void TestAudioConformCache::registryRefKeepsChunkAlive() {
    auto& cache = AudioConformCache::instance();
    Chunk* c = nullptr;
    {
        Ref r = cache.get(uniquePath("alive"), 0);
        c = r.get();
        QCOMPARE(c->refs.load(), 2); // registo + r
    }
    // r morreu: sobra a ref permanente do registo, logo o buffer vive.
    QCOMPARE(c->refs.load(), 1);
    // E volta a ser encontrável com o mesmo ponteiro.
    Ref again = cache.get(c->filePath, c->stream);
    QCOMPARE(again.get(), c);
}

// -------------------------------------------------------- readFrames()

void TestAudioConformCache::readEmptyRegionReturnsZero() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("empty"), 0);
    QVector<int16_t> out = zeros(32);
    QCOMPARE(cache.readFrames(r, 0, 32, out.data()), 0);
    QCOMPARE(out, zeros(32));
}

void TestAudioConformCache::readStopsAtFirstGap() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("gap"), 0);
    plant(r.get(), 0, 10, 500);
    QVector<int16_t> out = zeros(40);
    // Pede 40, só [0,10) existe: devolve 10 e zera o resto.
    QCOMPARE(cache.readFrames(r, 0, 40, out.data()), 10);
    QCOMPARE(out.at(0), 500);
    QCOMPARE(out.at(9 * AudioConformCache::kChannels), 509);
    for (int f = 10; f < 40; ++f) {
        QCOMPARE(out.at(f * AudioConformCache::kChannels), 0);
        QCOMPARE(out.at(f * AudioConformCache::kChannels + 1), 0);
    }
}

void TestAudioConformCache::readSkipsGapAndContinuesAfter() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("twogaps"), 0);
    plant(r.get(), 0, 10, 500);
    plant(r.get(), 20, 10, 700);
    QVector<int16_t> out = zeros(40);
    // A lacuna [10,20) interrompe: só [0,10) sai, mesmo havendo [20,30).
    QCOMPARE(cache.readFrames(r, 0, 40, out.data()), 10);
}

void TestAudioConformCache::readFromInsideInterval() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("inside"), 0);
    plant(r.get(), 0, 100, 500);
    QVector<int16_t> out = zeros(8);
    QCOMPARE(cache.readFrames(r, 50, 8, out.data()), 8);
    for (int i = 0; i < 8; ++i) {
        const qint64 frame = 50 + i;
        QCOMPARE(out.at(i * AudioConformCache::kChannels), (int16_t)(500 + frame));
        QCOMPARE(out.at(i * AudioConformCache::kChannels + 1), (int16_t)(500 + frame));
    }
}

void TestAudioConformCache::readClampsToFilledEnd() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("clamp"), 0);
    plant(r.get(), 0, 10, 500);
    QVector<int16_t> out = zeros(64);
    QCOMPARE(cache.readFrames(r, 0, 64, out.data()), 10);
}

void TestAudioConformCache::readNonPositiveMaxReturnsZero() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("nonpos"), 0);
    QVector<int16_t> out = zeros(4);
    QCOMPARE(cache.readFrames(r, 0, 0, out.data()), 0);
    QCOMPARE(cache.readFrames(r, 0, -8, out.data()), 0);
}

void TestAudioConformCache::readNullCacheReturnsZero() {
    auto& cache = AudioConformCache::instance();
    QVector<int16_t> out = zeros(8);
    Ref nada;
    QCOMPARE(cache.readFrames(nada, 0, 8, out.data()), 0);
}

void TestAudioConformCache::readZeroesTailAfterGap() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("tail"), 0);
    // [0,10) e [20,30) preenchidos; lê de 10: a lacuna está logo no início.
    plant(r.get(), 0, 10, 500);
    plant(r.get(), 20, 10, 700);
    QVector<int16_t> out = zeros(40);
    for (int i = 0; i < 40; ++i) out[i] = 0x7fff; // pré-poe lixo
    QCOMPARE(cache.readFrames(r, 10, 40, out.data()), 0);
    QCOMPARE(out, zeros(40)); // tudo limpo, sem lixo
}

// -------------------------------------------------------- waitReady()

void TestAudioConformCache::waitReadyZeroFramesIsInstant() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("w0"), 0);
    QVERIFY(cache.waitReady(r, 0, 0, 0));
    QVERIFY(cache.waitReady(r, 0, -4, 0));
}

void TestAudioConformCache::waitReadyNullCacheFails() {
    auto& cache = AudioConformCache::instance();
    Ref nada;
    QVERIFY(!cache.waitReady(nada, 0, 10, 10));
}

void TestAudioConformCache::waitReadyCoveredRegionSucceeds() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("wcov"), 0);
    plant(r.get(), 100, 200, 500);
    QVERIFY(cache.waitReady(r, 150, 40, 0));
    QVERIFY(cache.waitReady(r, 100, 200, 0)); // exactamente a região
    QVERIFY(cache.waitReady(r, 299, 1, 0));   // último frame coberto
    // O intervalo é [100,300): o frame 300 já não pertence a ele.
    QVERIFY(!cache.waitReady(r, 300, 1, 0));
}

void TestAudioConformCache::waitReadyUncoveredTimesOut() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("wunc"), 0);
    plant(r.get(), 0, 10, 500);
    QElapsedTimer t0;
    t0.start();
    QVERIFY(!cache.waitReady(r, 0, 40, 60)); // só [0,10) existe
    QVERIFY2(t0.elapsed() >= 40, "devia esperar pelo timeout, não desistir logo");
}

// ------------------------------------------------- request / diagnóstico

void TestAudioConformCache::requestIgnoresInvalidInput() {
    auto& cache = AudioConformCache::instance();
    Ref r = cache.get(uniquePath("req"), 0);
    Ref nada;
    // Nenhum destes deve crashar nem criar trabalho pendente.
    cache.request(r, 0.0, 0.0);
    cache.request(r, 0.0, -1.0);
    cache.request(r, -1.0, 1.0);
    cache.request(nada, 0.0, 1.0);
    QVERIFY(r->wanted.isEmpty());
}

void TestAudioConformCache::totalBytesStartsAtZero() {
    // Só este teste conta: a cache é singleton e o worker nunca preenche nada,
    // por isso o total tem de continuar a zero ao longo de toda a suite.
    QCOMPARE(AudioConformCache::instance().totalBytes(), (qint64)0);
}

QTEST_APPLESS_MAIN(TestAudioConformCache)
#include "tst_audio_conform.moc"
