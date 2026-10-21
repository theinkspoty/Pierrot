// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste do FrameCache (política L4, em colombina/ffmpeg).
//
// O cache saiu do núcleo na Fase 6: o motor não promete cache, isso é política
// de L4. A chave é (fonte, bucket de tempo, maxWidth) — a fonte entra na chave
// porque o cache é global. O teste cobre acerto/erro, move-to-front, evicção
// LRU, isolamento entre chaves/fontes, getOrCompute e descarte de imagem nula.

#include <QtTest>

#include "colombina/ffmpeg/FrameCache.h"

#include <QImage>

namespace {

QImage solid(int w, int h, QColor c) {
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(c);
    return img;
}

// Chave com fonte zero (o caso comum de uma só fonte no teste).
FrameCache::Key k(int64_t bucket, int maxW, quint64 src = 0) {
    return FrameCache::Key{src, bucket, maxW};
}

} // namespace

class tst_FrameCache : public QObject {
    Q_OBJECT
private slots:
    void init() { FrameCache::resetStats(); }

    // Miss devolve imagem nula e conta um miss; hit devolve o mesmo quadro.
    void missThenHit() {
        FrameCache c;
        QVERIFY(c.get(k(10, 160)).isNull());
        QCOMPARE(FrameCache::misses(), quint64(1));
        QCOMPARE(FrameCache::hits(), quint64(0));

        c.put(k(10, 160), solid(4, 4, Qt::red));
        QVERIFY(!c.get(k(10, 160)).isNull());
        QCOMPARE(FrameCache::hits(), quint64(1));
        QCOMPARE(c.size(), 1);
    }

    // Chaves diferentes (bucket ou maxW) não colidem.
    void distinctKeysDoNotCollide() {
        FrameCache c;
        c.put(k(1, 160), solid(2, 2, Qt::red));
        c.put(k(1, 320), solid(2, 2, Qt::blue));
        c.put(k(2, 160), solid(2, 2, Qt::green));
        QCOMPARE(c.size(), 3);
        QVERIFY(!c.get(k(1, 160)).isNull());
        QVERIFY(!c.get(k(1, 320)).isNull());
        QVERIFY(!c.get(k(2, 160)).isNull());
        QVERIFY(c.get(k(3, 160)).isNull());
    }

    // A FONTE faz parte da chave: mesmo (bucket, largura) em fontes diferentes
    // são entradas distintas — senão um quadro de B seria servido para A.
    void distinctSourcesDoNotCollide() {
        FrameCache c;
        c.put(k(1, 160, 100), solid(2, 2, Qt::red));
        c.put(k(1, 160, 200), solid(2, 2, Qt::blue));
        QCOMPARE(c.size(), 2);
        QCOMPARE(c.get(k(1, 160, 100)).pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(c.get(k(1, 160, 200)).pixelColor(0, 0), QColor(Qt::blue));
        QVERIFY(c.get(k(1, 160, 300)).isNull());
    }

    // Evicção LRU: ao passar do limite, o MENOS recentemente usado sai. Um get
    // no item A o torna recente, então quem sai é o B.
    void evictsLeastRecentlyUsed() {
        FrameCache c(3);
        const FrameCache::Key a = k(1, 1), b = k(2, 1), d = k(4, 1);
        c.put(a, solid(2, 2, Qt::red));
        c.put(b, solid(2, 2, Qt::green));
        c.put(k(3, 1), solid(2, 2, Qt::blue));
        // Acessa A → move-to-front. Ordem agora: A, C, B.
        QVERIFY(!c.get(a).isNull());
        // Insere D → evicção do fundo (B).
        c.put(d, solid(2, 2, Qt::yellow));
        QCOMPARE(c.size(), 3);
        QVERIFY(!c.get(a).isNull());
        QVERIFY(!c.get(d).isNull());
        FrameCache::resetStats();
        QVERIFY(c.get(b).isNull()); // B foi evictado
    }

    // put() com imagem nula é ignorado (não envenena o cache).
    void nullImageIsIgnored() {
        FrameCache c;
        c.put(k(1, 1), QImage());
        QCOMPARE(c.size(), 0);
        QVERIFY(c.get(k(1, 1)).isNull());
    }

    // clear() esvazia.
    void clearEmpties() {
        FrameCache c;
        c.put(k(1, 1), solid(2, 2, Qt::red));
        c.put(k(2, 1), solid(2, 2, Qt::blue));
        c.clear();
        QCOMPARE(c.size(), 0);
        QVERIFY(c.get(k(1, 1)).isNull());
    }

    // put() na MESMA chave atualiza o conteúdo (não duplica).
    void putSameKeyUpdates() {
        FrameCache c;
        c.put(k(5, 160), solid(2, 2, Qt::red));
        c.put(k(5, 160), solid(2, 2, Qt::green));
        QCOMPARE(c.size(), 1);
        QCOMPARE(c.get(k(5, 160)).pixelColor(0, 0), QColor(Qt::green));
    }

    // getOrCompute: na 1ª vez computa e guarda; na 2ª não computa de novo.
    void getOrComputeCaches() {
        FrameCache c;
        int calls = 0;
        auto compute = [&] { ++calls; return solid(2, 2, Qt::red); };
        QVERIFY(!c.getOrCompute(k(7, 160), compute).isNull());
        QCOMPARE(calls, 1);
        QVERIFY(!c.getOrCompute(k(7, 160), compute).isNull());
        QCOMPARE(calls, 1); // veio do cache
        QCOMPARE(FrameCache::hits(), quint64(1));
    }

    // getOrCompute não guarda resultado nulo (o próximo pedido recomputa).
    void getOrComputeSkipsNull() {
        FrameCache c;
        int calls = 0;
        auto compute = [&] { ++calls; return QImage(); };
        QVERIFY(c.getOrCompute(k(8, 1), compute).isNull());
        QVERIFY(c.getOrCompute(k(8, 1), compute).isNull());
        QCOMPARE(calls, 2);
        QCOMPARE(c.size(), 0);
    }

    // §8.4 — budget de MEMÓRIA: com teto em bytes, a evicção LRU acontece por
    // espaço mesmo com a contagem bem abaixo de maxEntries. É o teto que
    // importa em produção (120 quadros 4K = ~4GB).
    void evictsByByteBudget() {
        const QImage one = solid(64, 64, Qt::red);
        const qint64 per = one.sizeInBytes();
        QVERIFY(per > 0);
        // Cabem exatamente 2 imagens (o teto apertado força a 3ª a evictar).
        FrameCache c(100, per * 2);
        c.put(k(1, 64), one);
        c.put(k(2, 64), one.copy()); // cópia profunda (buffer distinto p/ contar bytes)
        QCOMPARE(c.size(), 2);
        QCOMPARE(c.bytes(), per * 2);
        // 3ª entra → o fundo da LRU (bucket 1) sai.
        c.put(k(3, 64), one.copy());
        QCOMPARE(c.size(), 2);
        QVERIFY(c.bytes() <= per * 2);
        FrameCache::resetStats();
        QVERIFY(c.get(k(1, 64)).isNull()); // evictado por byte budget
        QVERIFY(!c.get(k(3, 64)).isNull());
    }

    // bytes() acompanha put/atualização/clear e o move-to-front não vaza.
    void byteAccountingTracksMutations() {
        FrameCache c;
        QCOMPARE(c.bytes(), qint64(0));
        const QImage a = solid(32, 32, Qt::red);
        c.put(k(1, 32), a);
        QCOMPARE(c.bytes(), a.sizeInBytes());
        // Atualizar a mesma chave com imagem maior soma só a diferença.
        const QImage big = solid(64, 64, Qt::blue);
        c.put(k(1, 32), big);
        QCOMPARE(c.size(), 1);
        QCOMPARE(c.bytes(), big.sizeInBytes());
        c.clear();
        QCOMPARE(c.bytes(), qint64(0));
    }
};

QTEST_MAIN(tst_FrameCache)
#include "tst_framecache.moc"
