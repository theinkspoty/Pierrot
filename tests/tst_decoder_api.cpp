// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Contrato da API do FFmpegDecoder (núcleo L3, o "Decoder" do plano v0.4).
//
// O relatório de reestruturação fixa invariantes (seção 9) que só valem se
// forem testados. Este arquivo dá dente a três deles:
//
//   • Determinismo (invariante 1): mesmo (path, t, maxWidth) produz pixels
//     idênticos, em qualquer ordem de pedido e independentemente do cache de
//     frames internos. É a rede de segurança da PARIDADE de frame (critério 3):
//     antes/depois do split de L0/L1, a imagem não pode mudar.
//
//   • Idempotência (invariante 6): close() duas vezes não quebra; open() num
//     arquivo já aberto não reabre; frame() após close devolve vazio, nunca
//     quebra. Isso é o que sustenta "não se sustenta" como classe eliminada.
//
//   • Caminho quente previsível (invariante 4): com o decoder quente e a
//     posição vizinha já em cache, o p99 fica próximo do p50. É a linha de
//     BASE de latência (critério 6) que as Fases 2–6 não podem piorar.
//
// Sem mídia real não haveria o que decodificar, então o arquivo é gerado com o
// ffmpeg CLI. Sem ffmpeg no PATH, os testes pulam — mas o caminho de falha do
// open() (que não precisa de mídia) roda sempre.

#include <QtTest>

#include "laartman/Decoder.h"
#include "laartman/core/FFmpegDecoder.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include <QThread>

#include <unistd.h>

namespace {

int runCmd(const QString& program, const QStringList& args, int timeoutMs = 30000) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(5000) || !p.waitForFinished(timeoutMs)) return -1;
    return p.exitCode();
}

// Abre `dec` garantindo que ele devolva quadros de verdade. Em GPU cuja VAAPI
// abre o device mas não decodifica (NVIDIA com H.264), o primeiro arquivo fica
// preto; o auto-cura só age no open() SEGUINTE. Reabrir é o que cura. Mesma
// adaptação do tst_concurrency, para não mascarar problema ambiental como falha
// de determinismo.
bool openUsable(FFmpegDecoder& dec, const QString& path) {
    if (!dec.open(path)) return false;
    if (!dec.frameAt(1.0).isNull()) return true;
    if (!dec.usesHardware()) return false;
    for (int attempt = 0; attempt < 6 && dec.usesHardware(); ++attempt) {
        dec.close();
        if (!dec.open(path)) return false;
        if (!dec.frameAt(1.0).isNull()) return true;
    }
    return !dec.frameAt(1.0).isNull();
}

// Variante pela fachada Decoder: a política de hw é injetada via DecodeOptions
// (o núcleo não expõe usesHardware). Se o decode sair vazio com hw ligado,
// reabre com hardwareAllowed=false — exatamente a decisão que a política
// tomaria (L5) ao detectar GPU quebrada.
bool openUsableFacade(Decoder& dec, const QString& path, DecodeOptions opt) {
    dec.close();
    if (!dec.open(path, opt)) return false;
    if (!dec.frame(1.0).isNull()) return true;
    opt.hardwareAllowed = false;
    dec.close();
    if (!dec.open(path, opt)) return false;
    return !dec.frame(1.0).isNull();
}

// Compara dois quadros pixel a pixel. Devolve true se idênticos.
bool samePixels(const QImage& a, const QImage& b) {
    if (a.isNull() || b.isNull()) return false;
    if (a.size() != b.size() || a.format() != b.format()) return false;
    return a == b;
}

} // namespace

class tst_DecoderApi : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void openFailureIsClean();
    void closeIsIdempotent();
    void openTwiceDoesNotReopen();
    void frameAfterCloseIsNull();

    void determinismSamePathTimeWidth();
    void determinismIndependentOfOrder();
    void determinismAcrossReopen();

    void seekBackwardForwardMatchesFreshDecode();
    void loopGifWrapsDeterministically();
    void vfrDecodeIsDeterministic();
    void videoAndAudioDecodeConcurrently();
    void oddWidthDecodeStaysClean();

    void hotPathLatencyIsFlat();

    void facadeMatchesEngine();
    void facadeHeaderExposesNoOwnership();

    void probeCacheMatchesAndInvalidatesOnChange();

private:
    QString m_ffmpeg;
    QString m_dir;
    QString m_media;   // vídeo 320x180@30 (3s), GOP fechado, sem áudio
    QString m_av;      // vídeo 320x180@30 (3s) + trilha de áudio (teste AV paralelo)
    QString m_odd;     // vídeo 360x240 (largura %16 != 0 — estouro do swscale)
    QString m_gif;     // GIF animado 2s em loop
    QString m_vfr;     // MP4 com taxa variável (concat 30fps + 10fps)
    double m_duration = 0.0;
};

void tst_DecoderApi::initTestCase() {
    m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (m_ffmpeg.isEmpty())
        QSKIP("ffmpeg não encontrado no PATH — pulando os testes de API do decoder.");
    m_dir = QDir::tempPath() + QStringLiteral("/pierrot-decapi-") + QString::number(::getpid());
    QDir().mkpath(m_dir);
    m_media = m_dir + QStringLiteral("/api.mp4");
    m_gif = m_dir + QStringLiteral("/anim.gif");

    // GOP curto e keyframes regulares: os testes de seek precisam de alvos
    // previsíveis. Sem áudio para isolar o caminho de vídeo.
    const QStringList args {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc=size=320x180:rate=30:duration=3"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-g"), QStringLiteral("15"), QStringLiteral("-keyint_min"), QStringLiteral("15"),
        QStringLiteral("-sc_threshold"), QStringLiteral("0"),
        QStringLiteral("-an"),
        m_media
    };
    QCOMPARE(runCmd(m_ffmpeg, args), 0);

    FFmpegMediaInfo info = FFmpegDecoder::probe(m_media);
    QVERIFY(info.hasVideo);
    QCOMPARE(info.width, 320);
    QCOMPARE(info.height, 180);
    m_duration = info.duration;
    QVERIFY(m_duration > 2.0);

    // Vídeo + áudio no mesmo arquivo: fixture do teste de caminhos
    // concorrentes (vídeo no PacketReader, áudio no demuxer, mesma instância).
    m_av = m_dir + QStringLiteral("/av.mp4");
    const QStringList avArgs {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc=size=320x180:rate=30:duration=3"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("sine=frequency=440:duration=3"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-g"), QStringLiteral("15"), QStringLiteral("-keyint_min"), QStringLiteral("15"),
        QStringLiteral("-sc_threshold"), QStringLiteral("0"),
        QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-b:a"), QStringLiteral("96k"),
        m_av
    };
    if (runCmd(m_ffmpeg, avArgs) != 0)
        m_av.clear(); // AV opcional: se o encoder de áudio não existir, o teste pula

    // Largura 360 (não múltipla de 16): o kernel AVX2 do swscale escrevia
    // 32 B além do QImage no yuv→rgb (issue #11 — vídeo 360×360 do Gumduckk;
    // "malloc(): corrupted top size" no glibc no 2º frameAt). Fixture do
    // teste de regressão da defesa (swsScaleToRgba com linhas alinhadas).
    m_odd = m_dir + QStringLiteral("/odd360.mp4");
    const QStringList oddArgs {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc=size=360x240:rate=30:duration=2"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-g"), QStringLiteral("15"),
        QStringLiteral("-an"),
        m_odd
    };
    if (runCmd(m_ffmpeg, oddArgs) != 0)
        m_odd.clear();

    // GIF animado: usa o encoder GIF do ffmpeg (paleta). 2s, 10fps.
    const QStringList gifArgs {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc=size=160x120:rate=10:duration=2"),
        QStringLiteral("-vf"), QStringLiteral("split[a][b];[a]palettegen[p];[b][p]paletteuse"),
        QStringLiteral("-loop"), QStringLiteral("0"),
        m_gif
    };
    if (runCmd(m_ffmpeg, gifArgs) != 0) {
        m_gif.clear(); // GIF opcional: se o encoder não existir, o teste pula
    }

    // VFR: concatena um trecho a 30 fps com outro a 10 fps. O avg_frame_rate
    // resultante (~20) fica bem abaixo do r_frame_rate (30), então o detector
    // marca o arquivo como taxa variável. GOP curto para seek previsível.
    m_vfr = m_dir + QStringLiteral("/vfr.mp4");
    const QStringList vfrArgs {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc=size=320x180:rate=30:duration=1.5"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc=size=320x180:rate=10:duration=1.5"),
        QStringLiteral("-filter_complex"), QStringLiteral("[0:v][1:v]concat=n=2:v=1:a=0[out]"),
        QStringLiteral("-map"), QStringLiteral("[out]"),
        QStringLiteral("-fps_mode"), QStringLiteral("vfr"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-g"), QStringLiteral("15"), QStringLiteral("-an"),
        m_vfr
    };
    if (runCmd(m_ffmpeg, vfrArgs) != 0) {
        m_vfr.clear(); // VFR opcional: se o concat/mux não existir, o teste pula
    }
}

void tst_DecoderApi::cleanupTestCase() {
    if (!m_dir.isEmpty()) QDir(m_dir).removeRecursively();
}

// ── Invariante 6: idempotência / falha visível ─────────────────────────────

// Caminho inexistente: open() devolve false e o decoder NÃO fica em estado
// meio-aberto (isOpen false, source vazio) — senão o próximo uso confiaria num
// contexto morto.
void tst_DecoderApi::openFailureIsClean() {
    FFmpegDecoder dec;
    QVERIFY(!dec.open(QStringLiteral("/nonexistent/nope.mp4")));
    QVERIFY(!dec.isOpen());
    QVERIFY(dec.source().isEmpty());
    // Pedir um quadro de um decoder fechado devolve vazio, nunca quebra.
    QVERIFY(dec.frameAt(1.0).isNull());
}

// close() duas vezes não quebra nem deixa estado sujo; o decoder continua
// reutilizável depois.
void tst_DecoderApi::closeIsIdempotent() {
    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_media));
    QVERIFY(dec.isOpen());

    dec.close();
    QVERIFY(!dec.isOpen());
    dec.close(); // segunda vez: no-op, não pode quebrar
    QVERIFY(!dec.isOpen());
    QVERIFY(dec.frameAt(0.5).isNull());

    // E ainda dá para reabrir e usar de verdade.
    QVERIFY(openUsable(dec, m_media));
    QVERIFY(!dec.frameAt(0.5).isNull());
}

// open() num arquivo JÁ aberto (mesmo caminho) não reabre: devolve true sem
// mexer no contexto. A prova é o contador s_openCount: um reopen real o
// incrementa; o atalho "já aberto" não. (Antes se provava pelo cache interno,
// que saiu do núcleo na Fase 6.)
void tst_DecoderApi::openTwiceDoesNotReopen() {
    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_media));

    const QImage first = dec.frameAt(1.0, 160);
    QVERIFY(!first.isNull());

    const quint64 before = FFmpegDecoder::openCount();
    QVERIFY(dec.open(m_media)); // deve ser atalho "já aberto"
    QCOMPARE(FFmpegDecoder::openCount(), before); // NÃO reabriu
    // O frame continua idêntico (mesma posição/qualidade, mesmo arquivo).
    const QImage again = dec.frameAt(1.0, 160);
    QVERIFY(samePixels(first, again));
}

// frame() após close devolve vazio (nunca quebra). É o "falha visível" em vez
// de comportamento indefinido.
void tst_DecoderApi::frameAfterCloseIsNull() {
    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_media));
    dec.close();
    QVERIFY(dec.frameAt(0.0).isNull());
    QVERIFY(dec.frameAt(1.0, 64).isNull());
    QVERIFY(dec.frameAt(m_duration + 5.0).isNull()); // além do fim também
}

// ── Invariante 1: determinismo / paridade de frame ─────────────────────────

// Mesmo (path, t, maxWidth) → pixels idênticos, inclusive entre decoders
// independentes (sem estado global compartilhado).
void tst_DecoderApi::determinismSamePathTimeWidth() {
    FFmpegDecoder a, b;
    QVERIFY(openUsable(a, m_media));
    QVERIFY(openUsable(b, m_media));

    const double times[] = { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5 };
    const int widths[] = { 0, 64, 160, 320 };
    for (double t : times) {
        for (int w : widths) {
            const QImage ia = a.frameAt(t, w);
            const QImage ib = b.frameAt(t, w);
            QVERIFY2(!ia.isNull(), qPrintable(QStringLiteral("frame nulo t=%1 w=%2").arg(t).arg(w)));
            QVERIFY2(samePixels(ia, ib),
                     qPrintable(QStringLiteral("pixels divergem t=%1 w=%2").arg(t).arg(w)));
        }
    }
}

// A ordem dos pedidos não muda o resultado — a mesma posição pedida depois de
// uma varredura "suja" (pra frente e pra trás) tem que sair igual à pedida
// isolada. Pega bug de estado residual no decode progressivo.
void tst_DecoderApi::determinismIndependentOfOrder() {
    FFmpegDecoder ref, scr;
    QVERIFY(openUsable(ref, m_media));
    QVERIFY(openUsable(scr, m_media));

    const double target = 1.25;
    const QImage expected = ref.frameAt(target, 160);
    QVERIFY(!expected.isNull());

    // Varredura que mexe no decoder progressivo: pula longe, volta, avança em
    // passos finos, pede no fim e fora de ordem.
    scr.frameAt(2.8, 160);
    scr.frameAt(0.1, 160);
    for (int i = 0; i < 30; ++i) scr.frameAt(i * 0.1, 160);
    scr.frameAt(0.9, 160);
    scr.frameAt(2.4, 160);

    const QImage got = scr.frameAt(target, 160);
    QVERIFY2(samePixels(expected, got),
             "o quadro mudou conforme a ordem dos pedidos — estado residual no decode");
}

// Reabrir o arquivo e pedir a mesma posição devolve os mesmos pixels: a
// paridade não depende de o decoder estar "quente".
void tst_DecoderApi::determinismAcrossReopen() {
    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_media));
    const QImage before = dec.frameAt(1.0, 160);
    QVERIFY(!before.isNull());

    dec.close();
    QVERIFY(openUsable(dec, m_media));
    const QImage after = dec.frameAt(1.0, 160);
    QVERIFY2(samePixels(before, after),
             "reabrir o arquivo mudou o quadro — paridade depende do estado de cache");
}

// ── Contrato de seek / loop ────────────────────────────────────────────────

// Seek para trás e para frente pousa no MESMO quadro que um decode fresco
// daquela posição (decoder recém-aberto). É o contrato de "seek é preciso".
void tst_DecoderApi::seekBackwardForwardMatchesFreshDecode() {
    const double target = 1.0;

    FFmpegDecoder fresh;
    QVERIFY(openUsable(fresh, m_media));
    const QImage expected = fresh.frameAt(target, 160);
    QVERIFY(!expected.isNull());

    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_media));
    // Vai longe pra frente, volta pro alvo; depois vai pro fim e volta de novo.
    dec.frameAt(2.9, 160);
    const QImage back = dec.frameAt(target, 160);
    QVERIFY2(samePixels(expected, back), "seek para TRÁS divergiu do decode fresco");

    dec.frameAt(0.2, 160);
    const QImage fwd = dec.frameAt(target, 160);
    QVERIFY2(samePixels(expected, fwd), "seek para FRENTE divergiu do decode fresco");
}

// O loop do GIF: pedir um tempo além da duração embrulha com fmod sobre a
// duração real e devolve o quadro da posição equivalente — de forma
// determinística (o mesmo tempo embrulhado dá os mesmos pixels).
void tst_DecoderApi::loopGifWrapsDeterministically() {
    if (m_gif.isEmpty())
        QSKIP("encoder GIF ausente — teste de loop pulado");

    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_gif));

    // Duração real do GIF (para calcular a posição embrulhada).
    FFmpegMediaInfo info = FFmpegDecoder::probe(m_gif);
    QVERIFY(info.duration > 0.0);
    const double dur = info.duration;

    // t_inside e t_inside + n*dur devem dar o MESMO quadro (loop). Usa uma
    // posição no meio para evitar a fronteira exata do wrap.
    const double inside = dur * 0.25;
    const QImage a = dec.frameAt(inside, 120);
    QVERIFY2(!a.isNull(), "GIF não devolveu quadro na posição interna");

    const QImage b = dec.frameAt(inside + dur, 120);
    const QImage c = dec.frameAt(inside + 3.0 * dur, 120);
    QVERIFY2(samePixels(a, b), "wrap de 1 loop do GIF divergiu");
    QVERIFY2(samePixels(a, c), "wrap de 3 loops do GIF divergiu");
}

// VFR (taxa de quadros variável): a matemática de tempo passa a usar o
// timestamp real de cada quadro (grade de ms na chave de cache) em vez de
// assumir espaçamento 1/fps. O que se prova: decodificar o mesmo instante por
// caminhos diferentes (fresco, após seek para a frente, após seek para trás)
// continua pixel-idêntico — o detector de VFR não pode quebrar o determinismo.
void tst_DecoderApi::vfrDecodeIsDeterministic() {
    if (m_vfr.isEmpty())
        QSKIP("geração de VFR ausente — teste de VFR pulado");

    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_vfr));

    const double t = 1.0; // cai na metade a 10 fps do clipe concatenado
    const QImage fresh = dec.frameAt(t, 120);
    QVERIFY2(!fresh.isNull(), "VFR não devolveu quadro");

    // Seek para a frente e volta — deve dar o MESMO quadro fresco.
    dec.frameAt(2.5, 120);
    const QImage afterForward = dec.frameAt(t, 120);
    QVERIFY2(samePixels(fresh, afterForward), "decodificação VFR divergiu após seek à frente");

    dec.frameAt(0.2, 120);
    const QImage afterBack = dec.frameAt(t, 120);
    QVERIFY2(samePixels(fresh, afterBack), "decodificação VFR divergiu após seek para trás");
}

// Fase 3 (separação de vídeo/áudio): com o vídeo no PacketReader (contexto
// próprio) e o áudio no demuxer, a MESMA instância decodifica os dois em
// threads concorrentes sem deadlock e sem corromper — antes a cadeia de mutex
// m_mutex → m_audioMutex serializava os caminhos. Um deadlock de locks
// cruzados pendura o teste (o ctest mata o processo pendurado).
void tst_DecoderApi::videoAndAudioDecodeConcurrently() {
    if (m_av.isEmpty())
        QSKIP("geração de AV ausente — teste de caminhos concorrentes pulado");

    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_av));
    QVERIFY(dec.hasAudio());

    std::atomic<bool> videoDone{false}, audioDone{false};
    std::atomic<int> videoFrames{0}, audioBytes{0};

    // Vídeo: varredura com seeks para trás/à frente (exercita o PacketReader).
    std::thread video([&] {
        for (int i = 0; i < 60; ++i) {
            const double t = (i % 30) * 0.1;
            if (!dec.frameAt(t, 160).isNull())
                videoFrames.fetch_add(1);
        }
        videoDone.store(true);
    });
    // Áudio: seeks + drenagem de PCM (exercita o demuxer de áudio).
    std::thread audio([&] {
        char buf[4096];
        for (int i = 0; i < 60; ++i) {
            dec.seekAudio((i % 20) * 0.15);
            const int got = dec.decodeAudio(buf, sizeof(buf));
            if (got > 0) audioBytes.fetch_add(got);
        }
        audioDone.store(true);
    });

    QDeadlineTimer limit(30000);
    while ((!videoDone.load() || !audioDone.load()) && !limit.hasExpired())
        QThread::msleep(10);
    video.join();
    audio.join();

    QVERIFY2(videoDone.load() && audioDone.load(),
             "vídeo+áudio na mesma instância travaram (deadlock de locks)");
    QVERIFY2(videoFrames.load() > 0, "nenhum quadro decodificado no caminho paralelo");
    QVERIFY2(audioBytes.load() > 0, "nenhum byte de PCM decodificado no caminho paralelo");
}

// Regressão da defesa do swscale (issue #11): larguras não múltiplas de 16
// (360 aqui) faziam o kernel AVX2 do yuv→rgb escrever 32 B além do QImage —
// o glibc abortava no 2º frameAt com "malloc(): corrupted top size" e o app
// inteiro quebrava (o "travamento" do Gumduckk com o vídeo AV1 360×360).
// A defesa (swsScaleToRgba com linhas alinhadas) tem que segurar40 frames
// seguidos + manter o determinismo do frame.
void tst_DecoderApi::oddWidthDecodeStaysClean() {
    if (m_odd.isEmpty())
        QSKIP("geração de mídia 360 larga ausente — teste pulado");

    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_odd));

    QImage first;
    for (int i = 0; i < 40; ++i) {
        const double t = (i % 20) * 0.05; // varredura com seeks
        const QImage img = dec.frameAt(t, 0);
        QVERIFY2(!img.isNull(), qPrintable(QStringLiteral("quadro nulo em t=%1").arg(t)));
        QCOMPARE(img.width(), 360); // largura visível exata (não vaza o padding)
        if (i == 0) first = img;
    }
    // Determinismo: mesmo t após reabrir → mesmos pixels (a defesa não muda
    // o conteúdo, só o alojamento).
    dec.close();
    FFmpegDecoder dec2;
    QVERIFY(openUsable(dec2, m_odd));
    QVERIFY2(samePixels(first, dec2.frameAt(0.0, 0)),
             "defesa do swscale alterou os pixels do frame");
}

// ── Invariante 4: caminho quente previsível (linha de base de latência) ─────

// Com o decoder quente e posições vizinhas, o custo por frame não tem cauda
// grosseira: p99 próximo do p50 e sem pico patológico. Isto é a BASE que as
// fases 2–6 (lookahead/cache na política) não podem piorar. Generoso de
// propósito: mede ORDEM de grandeza (sem mutex global/syscall no quente), não
// precisão de benchmark.
void tst_DecoderApi::hotPathLatencyIsFlat() {
    FFmpegDecoder dec;
    QVERIFY(openUsable(dec, m_media));

    // Aquece uma janela contígua de frames (cache de conversão + decode).
    constexpr int kSamples = 60;
    std::vector<double> us;
    us.reserve(kSamples);

    // Primeira passada para aquecer (não medida).
    for (int i = 0; i < kSamples; ++i) dec.frameAt(i * 0.02, 160);

    QElapsedTimer clock;
    for (int i = 0; i < kSamples; ++i) {
        const double t = i * 0.02;
        clock.start();
        const QImage img = dec.frameAt(t, 160);
        us.push_back(clock.nsecsElapsed() / 1000.0);
        QVERIFY(!img.isNull());
    }

    std::vector<double> sorted = us;
    std::sort(sorted.begin(), sorted.end());
    const double p50 = sorted[sorted.size() / 2];
    const double p99 = sorted[(size_t)(sorted.size() * 0.99)];
    qInfo().noquote() << QStringLiteral("[lat] quente p50=%1µs p99=%2µs max=%3µs")
                             .arg(p50, 0, 'f', 1).arg(p99, 0, 'f', 1)
                             .arg(sorted.back(), 0, 'f', 1);

    // A cauda não pode explodir em relação à mediana. 50× é folgado para
    // máquina compartilhada/CI, mas ainda apanha um pico de segunda ordem
    // (ex.: reabrir arquivo, re-seek completo, lock global) que faria a UI
    // engasgar.
    QVERIFY2(p99 <= p50 * 50.0 + 5000.0,
             qPrintable(QStringLiteral("cauda quente alta demais: p50=%1µs p99=%2µs")
                            .arg(p50, 0, 'f', 1).arg(p99, 0, 'f', 1)));
}

// ── Fase 0: a fachada Decoder (L3, API congelada) ──────────────────────────
//
// A fachada tem que entregar exatamente o mesmo que o motor: mesmos pixels,
// mesma paridade. É a garantia de que a superfície pública (que as fases 2–6
// vão consumir) não introduz comportamento novo.
void tst_DecoderApi::facadeMatchesEngine() {
    // probe() da fachada bate com o do motor.
    const MediaInfo mi = Decoder::probe(m_media);
    const FFmpegMediaInfo eng = FFmpegDecoder::probe(m_media);
    QCOMPARE(mi.width, eng.width);
    QCOMPARE(mi.height, eng.height);
    QCOMPARE(mi.hasVideo, eng.hasVideo);
    QVERIFY(qFuzzyCompare(mi.duration, eng.duration));

    Decoder a;
    DecodeOptions opt;
    QVERIFY(openUsableFacade(a, m_media, opt));
    const QImage fa = a.frame(1.0, 160);
    QVERIFY(!fa.isNull());

    FFmpegDecoder b;
    QVERIFY(openUsable(b, m_media));
    const QImage fb = b.frameAt(1.0, 160);

    QVERIFY2(samePixels(fa, fb), "a fachada Decoder divergiu do motor FFmpegDecoder");

    // Idempotência pela fachada também vale.
    a.close();
    a.close();
    QVERIFY(!a.isOpen());
    QVERIFY(a.frame(1.0, 160).isNull());
}

// O memo de probe (§8.6) não pode mudar o RESULTADO: sondar o mesmo arquivo
// duas vezes devolve o mesmo; e reescrever o arquivo invalida (a chave inclui
// mtime+tamanho), então o segundo probe reflete o conteúdo novo — nunca o
// cache velho.
void tst_DecoderApi::probeCacheMatchesAndInvalidatesOnChange() {
    const FFmpegMediaInfo first = FFmpegDecoder::probe(m_media);
    const FFmpegMediaInfo again = FFmpegDecoder::probe(m_media);
    QCOMPARE(again.width, first.width);
    QCOMPARE(again.height, first.height);
    QCOMPARE(again.hasVideo, first.hasVideo);
    QVERIFY(qFuzzyCompare(again.duration, first.duration));

    // Arquivo ausente não é memoizado: sondar antes e depois de existir dá o
    // resultado real, não um vazio fixado.
    const QString ghost = m_dir + QStringLiteral("/ghost.mp4");
    QVERIFY(!QFile::exists(ghost));
    const FFmpegMediaInfo missing = FFmpegDecoder::probe(ghost);
    QVERIFY(!missing.hasVideo);

    // Reescreve um arquivo DEDICADO com DIMENSÕES diferentes → o probe tem que
    // enxergar o novo conteúdo (mtime/tamanho mudam). Sem invalidação, o teste
    // pegaria o cache de 320x180. Não toca em m_media (fixture compartilhada).
    const QString other = m_dir + QStringLiteral("/reprobe.mp4");
    const QStringList big {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc=size=320x180:rate=30:duration=1"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-an"), other
    };
    QCOMPARE(runCmd(m_ffmpeg, big), 0);
    QCOMPARE(FFmpegDecoder::probe(other).width, 320);
    // Substitui pelo mesmo caminho com 160x90 → probe deve invalidar.
    const QStringList small {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc=size=160x90:rate=15:duration=1"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-an"), other
    };
    QCOMPARE(runCmd(m_ffmpeg, small), 0);
    const FFmpegMediaInfo changed = FFmpegDecoder::probe(other);
    QCOMPARE(changed.width, 160);
    QCOMPARE(changed.height, 90);
}

// Invariante 2 (encapsulamento): o header público NÃO pode reexpor posse nem
// decisão de política. Este é o "o CI quebra se alguém reintroduzir" do
// relatório. Lê o próprio header como texto — o teste falha se algum token
// proibido reaparecer.
void tst_DecoderApi::facadeHeaderExposesNoOwnership() {
    const QString path = QStringLiteral(PIERROT_SOURCE_DIR "/src/laartman/Decoder.h");
    QFile f(path);
    QVERIFY2(f.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("não abri o header público: %1").arg(path)));
    const QString raw = QString::fromUtf8(f.readAll());

    // Remove comentários (// ... e /* ... */) antes de procurar os tokens: a
    // própria documentação do header cita os nomes proibidos para dizer que
    // NÃO existem — isso não pode contar como reexposição.
    QString code;
    code.reserve(raw.size());
    bool inLine = false, inBlock = false;
    for (int i = 0; i < raw.size(); ++i) {
        const QChar c = raw[i];
        const QChar n = (i + 1 < raw.size()) ? raw[i + 1] : QChar();
        if (inLine) {
            if (c == QLatin1Char('\n')) { inLine = false; code += c; }
            continue;
        }
        if (inBlock) {
            if (c == QLatin1Char('*') && n == QLatin1Char('/')) { inBlock = false; ++i; }
            continue;
        }
        if (c == QLatin1Char('/') && n == QLatin1Char('/')) { inLine = true; ++i; continue; }
        if (c == QLatin1Char('/') && n == QLatin1Char('*')) { inBlock = true; ++i; continue; }
        code += c;
    }

    // Tokens de posse/política que NÃO podem existir na superfície pública.
    const QStringList forbidden {
        QStringLiteral("acquire"), QStringLiteral("release"),
        QStringLiteral("busy"), QStringLiteral("Pool"),
        QStringLiteral("setHardwareDecodeAllowed"), QStringLiteral("releaseBuffers"),
    };
    for (const QString& tok : forbidden) {
        QVERIFY2(!code.contains(tok),
                 qPrintable(QStringLiteral("o header público reintroduziu '%1' — "
                                           "posse/política não pertence ao núcleo")
                                .arg(tok)));
    }
}

QTEST_MAIN(tst_DecoderApi)
#include "tst_decoder_api.moc"