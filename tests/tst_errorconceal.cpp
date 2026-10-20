// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste do ErrorConceal (L1) — ocultação de erro pelo último quadro bom.
//
// A regra central: um erro de decode (ou fim no meio de um GOP) NÃO deve virar
// "imagem vazia" ambígua. Se há um quadro bom, ele é devolvido com status
// Concealed; se não há, o erro transparece como Error/Eof. Este teste fixa a
// tabela de decisão sem depender de mídia real (usa AVFrame sintéticos).

#include <QtTest>

#include "laartman/core/ErrorConceal.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace {

AVFrame* makeFrame(int value) {
    AVFrame* f = av_frame_alloc();
    if (!f) return nullptr;
    f->width = 2;
    f->height = 2;
    f->format = AV_PIX_FMT_RGBA;
    // Buffer real (refcounted): av_frame_ref só copia campos úteis (pts etc.)
    // de um quadro com buffer válido; um AVFrame "cabeça" sem buffer perde o
    // pts na referência.
    if (av_frame_get_buffer(f, 32) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    // Marca o quadro para distinguir bom/novo nos asserts.
    f->pts = value;
    return f;
}

} // namespace

class tst_ErrorConceal : public QObject {
    Q_OBJECT
private slots:
    // Sem quadro bom, um erro é Error (não silencia); o fim limpo é Eof.
    void noGoodFrameDoesNotSilence() {
        ErrorConceal ec;
        AVFrame* out = nullptr;
        QCOMPARE(ec.decide(/*decoded*/ false, /*err*/ true, /*eof*/ false,
                           /*cancel*/ false, &out),
                 FrameStatus::Error);
        QVERIFY(out == nullptr);

        QCOMPARE(ec.decide(false, false, true, false, &out), FrameStatus::Eof);
        QVERIFY(out == nullptr);

        QCOMPARE(ec.decide(false, false, false, false, &out), FrameStatus::Pending);
        QVERIFY(out == nullptr);
    }

    // Com quadro bom, erro ou EOF-mid-GOP oculta: devolve o último bom.
    void concealsWithLastGood() {
        ErrorConceal ec;
        AVFrame* good = makeFrame(42);
        ec.rememberGood(good);
        QVERIFY(ec.hasGood());

        AVFrame* out = nullptr;
        QCOMPARE(ec.decide(false, /*err*/ true, false, false, &out),
                 FrameStatus::Concealed);
        QVERIFY(out == ec.lastGood());

        out = nullptr;
        QCOMPARE(ec.decide(false, false, /*eof*/ true, false, &out),
                 FrameStatus::Concealed);
        QVERIFY(out == ec.lastGood());

        av_frame_free(&good);
    }

    // Quadro recém-decodificado tem prioridade (Ok), mesmo havendo um bom.
    void decodedWinsAsOk() {
        ErrorConceal ec;
        AVFrame* good = makeFrame(1);
        ec.rememberGood(good);

        AVFrame* out = nullptr;
        QCOMPARE(ec.decide(/*decoded*/ true, false, false, false, &out),
                 FrameStatus::Ok);
        QVERIFY(out == nullptr); // o chamador já tem o novo quadro
        av_frame_free(&good);
    }

    // Cancelamento sempre vence, com ou sem quadro bom.
    void cancelWins() {
        ErrorConceal ec;
        AVFrame* good = makeFrame(7);
        ec.rememberGood(good);
        AVFrame* out = nullptr;
        QCOMPARE(ec.decide(true, true, true, /*cancel*/ true, &out),
                 FrameStatus::Cancelled);
        QVERIFY(out == nullptr);
        av_frame_free(&good);
    }

    // rememberGood guarda uma REFERÊNCIA: liberar o original não invalida a
    // cópia, e um novo bom substitui o anterior.
    void rememberGoodHoldsReference() {
        ErrorConceal ec;
        AVFrame* a = makeFrame(10);
        ec.rememberGood(a);
        av_frame_free(&a); // libera o original
        QVERIFY(ec.hasGood());
        QCOMPARE(ec.lastGood()->pts, (int64_t)10);

        AVFrame* b = makeFrame(20);
        ec.rememberGood(b);
        av_frame_free(&b);
        QCOMPARE(ec.lastGood()->pts, (int64_t)20); // substituído

        ec.clear();
        QVERIFY(!ec.hasGood());
    }

    // Classificadores auxiliares.
    void classifiesReturns() {
        QVERIFY(ErrorConceal::isAgain(AVERROR(EAGAIN)));
        QVERIFY(ErrorConceal::isEof(AVERROR_EOF));
        QVERIFY(!ErrorConceal::isAgain(AVERROR_EOF));
    }
};

QTEST_MAIN(tst_ErrorConceal)
#include "tst_errorconceal.moc"
