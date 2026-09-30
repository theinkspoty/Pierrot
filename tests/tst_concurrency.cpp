// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste de concorrência do FFmpegDecoder — a rede que faltava.
//
// A medição de cobertura mostrou que FFmpegDecoder.cpp está em 2,3% e
// MesaRenderer.cpp em 0,0%: o código que carrega a serialização dos decoders
// praticamente não rodava em nenhum teste. Isso importava porque o locking
// aqui é manual — nada impede alguém de chamar um método novo sem pegar o
// mutex, e um erro desses só aparece como SIGSEGV em uso, nunca em revisão.
//
// Este teste exercita de verdade os caminhos que o app usa no preview: várias
// threads decodificando o MESMO decoder, `close()` caindo em cima de um
// `frameAt()` em andamento, e os accessors de áudio correndo enquanto o
// arquivo é aberto/fechado. É o que dá dente ao job de ASAN/UBSAN.
//
// Um teste sem mídia real não decodificaria nada e passaria vazio, então o
// arquivo é gerado com o ffmpeg CLI (vídeo + áudio). Sem ffmpeg no PATH, pula.

#include <QtTest>

#include "colombina/ffmpeg/FFmpegDecoder.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

// Barrier de largada. std::barrier é C++20 e o projeto é C++17; aqui só
// precisamos de "ninguém começa antes de todos chegarem", então um latch com
// mutex/cv resolve e ainda deixa a espera ser inspecionável em log.
class StartGate {
public:
    explicit StartGate(int n) : m_remaining(n) {}
    void arrive() {
        std::unique_lock<std::mutex> lk(m_m);
        if (--m_remaining == 0) {
            m_open = true;
            m_cv.notify_all();
        } else {
            m_cv.wait(lk, [this] { return m_open; });
        }
    }
private:
    std::mutex m_m;
    std::condition_variable m_cv;
    int m_remaining;
    bool m_open = false;
};

// Roda `body(i)` em `n` threads, todas soltas ao mesmo tempo, e espera todas
// terminarem. Nenhuma asserção dentro das threads: QVERIFY só vale na thread
// principal — quem verifica é o chamador, depois do join.
template <typename F>
void runParallel(int n, F body) {
    StartGate gate(n);
    std::vector<std::thread> ts;
    ts.reserve(n);
    for (int i = 0; i < n; ++i) {
        ts.emplace_back([&, i] {
            gate.arrive(); // todas começam disputando ao mesmo tempo
            body(i);
        });
    }
    for (auto& t : ts) t.join();
}

int runCmd(const QString& program, const QStringList& args, int timeoutMs = 30000) {
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(5000) || !p.waitForFinished(timeoutMs)) return -1;
    return p.exitCode();
}

// Abre `dec` garantindo que ele devolva quadros de verdade.
//
// Em máquina onde o VAAPI responde mas não produz quadro — caso real e comum:
// a VAAPI da NVIDIA não decodifica H.264, ela abre o device e devolve vazio —
// o primeiro arquivo fica preto. O auto-cura do decoder existe para isso, mas
// só age no open() SEGUINTE (ele desliga o hw para a sessão, e um contexto já
// aberto continua com o hw). Então reabrir é o que efetivamente cura. Sem
// esta adaptação, o teste inteiro passaria a falhar por motivo ambiental, e
// valeria mais ainda como verificação do auto-cura.
bool openUsable(FFmpegDecoder& dec, const QString& path) {
    if (!dec.open(path)) return false;
    if (!dec.frameAt(1.0).isNull()) return true;
    if (!dec.usesHardware()) return false; // não é hw; o quadro vazio é outro problema
    // O auto-cura só desliga o hw depois de várias falhas SEGUIDAS (4), e só
    // age no open() seguinte — um contexto já aberto continua preso ao device
    // quebrado. Uma reabertura não basta: é preciso insistir até ele desarmar.
    for (int attempt = 0; attempt < 6 && dec.usesHardware(); ++attempt) {
        dec.close();
        if (!dec.open(path)) return false;
        if (!dec.frameAt(1.0).isNull()) return true;
    }
    return !dec.frameAt(1.0).isNull();
}

} // namespace

class tst_concurrency : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void concurrentFrameAtOnSharedDecoder();
    void closeDuringFrameAt();
    void audioAccessorsDuringOpen();
    void audioDecodeDuringSeek();
    void releaseBuffersDuringFrameAt();
    void independentDecodersSameFile();
    void hwAutoCureAfterEmptyFrames();

private:
    QString m_ffmpeg;
    QString m_dir;
    QString m_media; // vídeo 320x180@30 (3s) + áudio aac 48 kHz
    double m_duration = 0.0;
};

void tst_concurrency::initTestCase() {
    m_ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (m_ffmpeg.isEmpty())
        QSKIP("ffmpeg não encontrado no PATH — pulando os testes de concorrência do decoder.");
    m_dir = QDir::tempPath() + QStringLiteral("/pierrot-conc-") + QString::number(::getpid());
    QDir().mkpath(m_dir);
    m_media = m_dir + QStringLiteral("/concurso.mp4");

    const QStringList args {
        QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("testsrc=size=320x180:rate=30:duration=3"),
        QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
        QStringLiteral("sine=frequency=440:duration=3:sample_rate=48000"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-shortest"),
        m_media
    };
    QCOMPARE(runCmd(m_ffmpeg, args), 0);

    FFmpegMediaInfo info = FFmpegDecoder::probe(m_media);
    QVERIFY(info.hasVideo);
    QVERIFY(info.hasAudio);
    QCOMPARE(info.width, 320);
    QCOMPARE(info.height, 180);
    m_duration = info.duration;
    QVERIFY(m_duration > 2.0);
}

void tst_concurrency::cleanupTestCase() {
    if (!m_dir.isEmpty()) QDir(m_dir).removeRecursively();
}

// Várias threads chamando frameAt() no MESMO decoder. O m_mutex serializa
// internamente, mas o caminho exercitado aqui é o pior para ASAN: o cache LRU
// guarda imagens derivadas de AVFrame e o sws é reaproveitado entre chamadas,
// então um erro de lock apareceria como use-after-free.
void tst_concurrency::concurrentFrameAtOnSharedDecoder() {
    FFmpegDecoder dec;
    QVERIFY2(openUsable(dec, m_media), "não foi possível abrir um decoder que devolva quadros");

    constexpr int kThreads = 6;
    constexpr int kIters = 40;
    std::atomic<int> valid{0};
    std::atomic<int> wrongSize{0};

    runParallel(kThreads, [&](int tid) {
        for (int i = 0; i < kIters; ++i) {
            // Cada thread varre uma fatia do tempo, para não só reusar o cache.
            const double t = (double)((i * 7 + tid * 3) % 90) / 30.0;
            const QImage img = dec.frameAt(t);
            if (img.isNull()) continue;
            if (valid.fetch_add(1) >= 0 && img.width() != 320) wrongSize.fetch_add(1);
        }
    });

    // O que importa é quealguma decodificação real aconteceu (senão o teste
    // passou vazio) e que nada saiu com dimensão errada.
    QVERIFY2(valid.load() > 0, "nenhum quadro foi decodificado — o teste não exercitou nada");
    QCOMPARE(wrongSize.load(), 0);
}

// O cenário que produz SIGSEGV: close()abrindo/fechando o contexto por baixo
// de um frameAt() já em andamento. freeAllLocked() solta AVFrame/AVPacket/
// SwrContext, então qualquer thread que ficou com ponteiro velho ao cair num
// close() vai ler memória liberada — exatamente o que o ASAN reporta.
void tst_concurrency::closeDuringFrameAt() {
    FFmpegDecoder dec;
    QVERIFY2(openUsable(dec, m_media), "não foi possível abrir um decoder que devolva quadros");

    constexpr int kDecodeThreads = 4;
    constexpr int kIters = 50;
    std::atomic<int> valid{0};

    std::atomic<bool> stop{false};

    // A thread que fecha: alterna close/open enquanto as outras decodificam.
    std::thread closerThread([&] {
        for (int i = 0; i < kIters && !stop.load(); ++i) {
            dec.close();
            dec.open(m_media);
        }
        stop.store(true);
    });

    runParallel(kDecodeThreads, [&](int tid) {
        for (int i = 0; i < kIters; ++i) {
            const double t = (double)((i * 5 + tid * 3) % 90) / 30.0;
            const QImage img = dec.frameAt(t);
            if (!img.isNull()) valid.fetch_add(1);
        }
    });

    closerThread.join();
    stop.store(true);

    // O mais importante: o decoder tem que ficar USÁVEL depois da briga. Um
    // estado meio-fechado passaria despercebido se ninguém usasse de novo.
    dec.close();
    QVERIFY2(openUsable(dec, m_media),
             "decoder ficou inconsistente depois do close/open concorrente");
    QVERIFY2(!dec.frameAt(1.0).isNull(), "quadro ausente após recuperar o decoder");
    QVERIFY2(valid.load() > 0, "nenhum quadro foi lido enquanto o close rodava — janela de corrida não exercitada");
}

// A corrida que o `audioChannels()` tinha: o getter lia `m_audioOutCh` sem
// m_audioMutex enquanto open() escrevia sob ele. Sob ASAN isso passa (é data
// race, não memória corrompida) — quem pega é o TSan — mas exercitar o caminho
// evita a regressão de o getter voltar a ler direto o membro.
void tst_concurrency::audioAccessorsDuringOpen() {
    FFmpegDecoder dec;
    QVERIFY2(openUsable(dec, m_media), "não foi possível abrir um decoder que devolva quadros");
    QCOMPARE(dec.audioChannels(), 2); // saída sempre estéreo, mesmo com mono na fonte

    constexpr int kIters = 200;
    std::atomic<int> chans{0};

    std::atomic<bool> stop{false};
    std::thread opener([&] {
        for (int i = 0; i < kIters && !stop.load(); ++i) {
            dec.open(m_media);
            dec.close();
        }
        stop.store(true);
    });

    runParallel(4, [&](int) {
        for (int i = 0; i < kIters; ++i) {
            // Todos os getters que tocam estado de áudio/vídeo, sem lock visível.
            if (dec.audioChannels() == 2) chans.fetch_add(1);
            dec.hasAudio();
            dec.isOpen();
            dec.fps();
            dec.usesHardware();
            dec.source();
        }
    });

    opener.join();
    stop.store(true);
    QVERIFY2(chans.load() > 0, "audioChannels() nunca devolveu 2 — o getter mudou de comportamento");
}

// seekAudio() e decodeAudio() disputam o mesmo m_audioMutex. Se um dos dois
// largar o lock no meio, o PCM sai corrompido ou o SwrContext é liberado
// embaixo da conversão.
void tst_concurrency::audioDecodeDuringSeek() {
    FFmpegDecoder dec;
    QVERIFY2(openUsable(dec, m_media), "não foi possível abrir um decoder que devolva quadros");
    QVERIFY2(dec.hasAudio(), "o decoder não abriu o stream de áudio — fallback do open() regrediu");

    constexpr int kIters = 60;
    std::atomic<int> samples{0};

    std::atomic<bool> stop{false};
    std::thread seeker([&] {
        for (int i = 0; i < kIters && !stop.load(); ++i) {
            dec.seekAudio((double)(i % 20) / 10.0);
        }
        stop.store(true);
    });

    runParallel(3, [&](int) {
        // 4096 bytes = 1024 samples S16 mono-equivalente; o mixer pede fatias
        // desse tamanho.
        std::vector<short> buf(4096);
        for (int i = 0; i < kIters; ++i) {
            const int n = dec.decodeAudio(buf.data(), int(buf.size() * sizeof(short)));
            if (n > 0) samples.fetch_add(n);
        }
    });

    seeker.join();
    stop.store(true);
    QVERIFY2(samples.load() > 0, "nenhum sample foi decodificado — a janela de corrida não foi exercitada");
}

// releaseBuffers() larga DPB e o último quadro sem fechar o arquivo, e é
// chamado pela UI enquanto o worker pode estar decodificando.
void tst_concurrency::releaseBuffersDuringFrameAt() {
    FFmpegDecoder dec;
    QVERIFY2(openUsable(dec, m_media), "não foi possível abrir um decoder que devolva quadros");

    constexpr int kIters = 40;
    std::atomic<int> valid{0};

    std::atomic<bool> stop{false};
    std::thread releaser([&] {
        for (int i = 0; i < kIters && !stop.load(); ++i) {
            dec.releaseBuffers();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stop.store(true);
    });

    runParallel(4, [&](int tid) {
        for (int i = 0; i < kIters; ++i) {
            const double t = (double)((i * 5 + tid * 3) % 90) / 30.0;
            if (!dec.frameAt(t).isNull()) valid.fetch_add(1);
        }
    });

    releaser.join();
    stop.store(true);
    QVERIFY2(valid.load() > 0, "nenhum quadro sobreviveu ao releaseBuffers concorrente");
}

// O padrão real do MesaRenderer: dois decoders independentes (vídeo +
// prefetch) abertos sobre o MESMO arquivo ao mesmo tempo. Não deve haver
// estado global compartilhado entre instâncias — nem no cache, nem na
// inicialização de hardware.
void tst_concurrency::independentDecodersSameFile() {
    FFmpegDecoder a, b;
    QVERIFY2(openUsable(a, m_media), "decoder A não devolveu quadros");
    QVERIFY2(openUsable(b, m_media), "decoder B não devolveu quadros");

    std::atomic<int> validA{0}, validB{0};

    runParallel(2, [&](int tid) {
        FFmpegDecoder& dec = (tid == 0) ? a : b;
        std::atomic<int>& counter = (tid == 0) ? validA : validB;
        for (int i = 0; i < 30; ++i) {
            const double t = (double)((i * (tid + 1)) % 80) / 30.0;
            if (!dec.frameAt(t).isNull()) counter.fetch_add(1);
        }
    });

    QCOMPARE(a.source(), m_media);
    QCOMPARE(b.source(), m_media);
    QVERIFY2(validA.load() > 0 && validB.load() > 0,
             "um dos decoders independentes devolveu nada — estado global vazando entre instâncias");
}

// O auto-cura de hardware. Em GPU cuja VAAPI abre o device mas não decodifica
// (NVIDIA com H.264 é o caso clássico), o primeiro arquivo fica preto e o
// decoder só se recupera no open() seguinte — porque o auto-cura desliga o hw
// PARA A SESSÃO, mas um contexto já aberto continua preso ao device quebrado.
//
// Sem este teste, essa janela de "primeiro clipe preto" fica invisível: os
// outros testes só funcionariam por causa do openUsable(), que mascara a
// queda para software.
void tst_concurrency::hwAutoCureAfterEmptyFrames() {
    FFmpegDecoder dec;
    QVERIFY(dec.open(m_media));

    if (!dec.usesHardware()) {
        // Máquina sem VAAPI: nada a curar, mas a decodificação tem que
        // funcionar em software mesmo assim.
        QVERIFY2(!dec.frameAt(1.0).isNull(), "sem VAAPI o preview tem que decodificar em software");
        QSKIP("VAAPI indisponível nesta máquina — auto-cura não exercitável.");
    }

    // O driver está em uso: insiste o bastante para o auto-cura desarmar.
    for (int i = 0; i < 8 && dec.usesHardware(); ++i) {
        if (!dec.frameAt(0.5 + i * 0.1).isNull()) break;
    }

    // Reabrir: agora o auto-cura já desligou o hw para a sessão.
    dec.close();
    QVERIFY(dec.open(m_media));
    QVERIFY2(!dec.usesHardware(),
             "hw ainda ativo depois de ManyEmptyFrames — o auto-cura não desligou o VAAPI");
    QVERIFY2(!dec.frameAt(1.0).isNull(),
             "após desativar o hw o decoder ainda não decodifica — o preview ficaria preto em máquina com VAAPI quebrada");
}

QTEST_MAIN(tst_concurrency)
#include "tst_concurrency.moc"
