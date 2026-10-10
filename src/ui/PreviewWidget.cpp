// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "PreviewWidget.h"

#include "ui/SettingsDialog.h"
#include "ui/TlLog.h"
#include "colombina/CrashReporter.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/ffmpeg/AudioConformCache.h"
#include "colombina/ofx/OfxRenderer.h"
#include "colombina/ofx/OfxPluginManager.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "colombina/fx/ColorGrade.h"
#include "ui/ScopeWidget.h"
#include "colombina/export/LainkaFx.h"
#include "ui/Theme.h"
#include "colombina/generators.h"
#ifdef PIERROT_ENABLE_RUST
#include "AudioFxBridge.h"
#endif

#include <QPainter>
#include <QPixmap>
#include <QPolygon>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QLabel>
#include <QComboBox>
#include <QSignalBlocker>
#include <QPointer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QShortcut>
#include <QIODevice>
#include <QAudioFormat>
#include <QPainterPath>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QResizeEvent>
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif
#include <algorithm>
#include <atomic>
#include <QDebug>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <QHash>
#include <QMutex>

// Diagnóstico do caminho de áudio do preview: ligue com PIERROT_AUDIO_DEBUG=1.
static bool audioDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_AUDIO_DEBUG");
    return on;
}

// Diagnóstico da composição multi-faixa do preview: ligue com PIERROT_COMPOSE_DEBUG=1.
static bool composeDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_COMPOSE_DEBUG");
    return on;
}

// Diagnóstico do playback (tick/frame/pedidos): ligue com PIERROT_PLAY_DEBUG=1.
// Revela o engasgo do vídeo (áudio perfeito): cadência dos ticks, latência do
// decode, skew do relógio e decodes adicionais por frame (camadas inferiores).
static bool playDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_PLAY_DEBUG");
    return on;
}

// Pool de decodificadores "quentes" por caminho de arquivo. Em vez de 2+1
// decoders fixos (main + prefetch + bg), mantém N abertos no arquivo certo:
// o uso vira um lease (acquire/release) por caminho, e o prefetch em background
// só precisa "aquecer" — abre o próximo arquivo e deixa a entrada ociosa no
// pool, sem transferir posse. Assim o open() de 500-800 ms é pago uma vez por
// fonte (ou quando uma entra/sai por LRU), não todo corte.
//
// Segurança: um decoder NUNCA é usado por duas threads ao mesmo tempo —
// acquire() marca a entrada busy, e tudo que é lento (open/frameAt) roda FORA
// do mutex do pool, com o caller como dono exclusivo até release(). O mutex só
// protege o mapa (QLock + QMutex), nunca o decoder.
class DecoderPool {
public:
    explicit DecoderPool(int capacity)
        : m_capacity(qMax(2, capacity)) {
        m_clock.start();
    }

    ~DecoderPool() {
        // As threads de decode já foram paradas (PreviewWidget::~) antes deste
        // ponto; os FFmpegDecoder restantes morrem aqui, fechando seus arquivos.
        QMutexLocker l(&m_mutex);
        for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
            delete it.value();
        m_entries.clear();
    }

    // Lease: devolve um decoder aberto em `path` (já quente, ou abre agora).
    // nullptr se o caminho estiver ocupado por outro lease (ex.: prefetch ainda
    // decodificando) ou o pool não tiver slot livre (todos ocupados).
    FFmpegDecoder* acquire(const QString& path) {
        QMutexLocker l(&m_mutex);
        auto it = m_entries.find(path);
        if (it != m_entries.end()) {
            if (it.value()->busy) return nullptr;
            it.value()->busy = true;
            it.value()->lastUseMs = m_clock.elapsed();
            return it.value()->dec;
        }
        // Caminho novo: despeja ociosos (LRU) até sobrar 1 slot. Se sobrar tudo
        // ocupado, o chamador libera o próprio lease e tenta de novo.
        evictIdleLocked(m_capacity - 1);
        if ((int)m_entries.size() >= m_capacity) return nullptr;
        auto e = new Entry();
        e->busy = true;
        e->lastUseMs = m_clock.elapsed();
        FFmpegDecoder* d = e->dec;
        m_entries.insert(path, e);
        l.unlock();
        // open() é lento (~500-800ms): fora do mutex. Falha remove a entrada.
        // (remove() no mapa de Entry* só apaga a chave; o objeto morre no delete.)
        if (!d->open(path)) {
            QMutexLocker l2(&m_mutex);
            if (m_entries.value(path) == e) m_entries.remove(path);
            delete e;
            return nullptr;
        }
        return d;
    }

    // Devolve a entrada (fica quente, pronta para o próximo acquire).
    void release(const QString& path) {
        QMutexLocker l(&m_mutex);
        auto it = m_entries.find(path);
        if (it != m_entries.end() && it.value()->busy) {
            it.value()->busy = false;
            it.value()->lastUseMs = m_clock.elapsed();
        }
    }

    // Nº de arquivos atualmente abertos (para diagnóstico/overlay).
    int count() const {
        QMutexLocker l(&m_mutex);
        return m_entries.size();
    }

    // Já existe entrada aberta para este caminho (quente)? Usado pelo
    // warm-ahead para não reaquecer o que já está no pool.
    bool contains(const QString& path) const {
        QMutexLocker l(&m_mutex);
        return m_entries.contains(path);
    }

private:
    struct Entry {
        FFmpegDecoder* dec = new FFmpegDecoder();
        bool busy = false;
        qint64 lastUseMs = 0; // último acquire/release (LRU)
        ~Entry() { delete dec; }
    };

    void evictIdleLocked(int keep) {
        while ((int)m_entries.size() > keep) {
            QString victim;
            qint64 oldest = m_clock.elapsed() + 1; // sentinela
            for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
                if (it.value()->busy) continue;
                if (it.value()->lastUseMs < oldest) {
                    oldest = it.value()->lastUseMs;
                    victim = it.key();
                }
            }
            if (victim.isEmpty()) return; // todos ocupados: não há o que despejar
            delete m_entries.take(victim); // destrói o FFmpegDecoder (fecha o arquivo)
        }
    }

    int m_capacity;
    mutable QMutex m_mutex;
    QHash<QString, Entry*> m_entries;
    QElapsedTimer m_clock;
};

// Capacidade do pool (padrão 6; PIERROT_DECODER_POOL=<n> ajusta). 6 dá folga
// para o main + prefetch em andamento + warm-ahead + quentes ociosos sem
// estourar memória (o warm de um arquivo não enche o cache de ~240MB de cada
// decoder; só o clipe tocado o faz).
static int decoderPoolCapacity() {
    bool ok = false;
    const int n = qEnvironmentVariableIntValue("PIERROT_DECODER_POOL", &ok);
    return (ok && n >= 2) ? n : 6;
}

// Decodifica quadros de vídeo na própria thread (com lease do DecoderPool: um
// para o clipe principal, e o prefetch aquecendo o pool para o swap).
// O PreviewWidget pede o "último" quadro desejado e descarta intermediários.
class FrameWorker : public QObject {
    Q_OBJECT
public:
    explicit FrameWorker(DecoderPool* pool, QObject* parent = nullptr)
        : QObject(parent), m_pool(pool) {}

    // Cancelamento cooperativo do frameAt EM ANDAMENTO (Fase 3). Thread-safe:
    // a UI chama direto (não é slot — um invoke enfileirado só rodaria depois
    // de o decode terminar, que é justamente o que queremos evitar). Só tem
    // efeito se um decode estiver armado (janela de decodeOne).
    void requestCancel() {
        if (m_cancelArmed.load(std::memory_order_acquire))
            m_cancel.store(true, std::memory_order_relaxed);
    }

public slots:
    void decodeOne(const QString& clipId, const QString& path, double t, int maxW, double dt,
                   const FrameFx& fx = FrameFx()) {
        CrashReporter::setActivity("decodificando quadro do preview");
        // Arma o token de cancelamento para todo este decode. RAII desarma em
        // QUALQUER retorno (inclusive os "soft" sem slot), para nunca vazar um
        // cancel pendente para o próximo pedido.
        m_cancel.store(false, std::memory_order_relaxed);
        m_cancelArmed.store(true, std::memory_order_release);
        struct Disarm {
            std::atomic<bool>& f;
            explicit Disarm(std::atomic<bool>& x) : f(x) {}
            ~Disarm() { f.store(false, std::memory_order_release); }
        } disarm(m_cancelArmed);
        if (m_curPath != path) {
            // Leva do pool o decoder quente deste arquivo (o prefetch em
            // background deixou o próximo clipe já aberto/aquecido na posição do
            // corte: acquire quente é instantâneo; o antigo volta ocioso, pronto
            // para ser reusado se a timeline voltar a este caminho).
            FFmpegDecoder* d = m_pool->acquire(path);
            if (!d && !m_curPath.isEmpty()) {
                // Pool cheio/ocupado: libera o lease atual (fica quente no pool)
                // e tenta de novo — o LRU despeja o ocioso mais antigo p/ entrar.
                m_pool->release(m_curPath);
                m_curPath.clear();
                m_decoder = nullptr;
                d = m_pool->acquire(path);
            }
            if (!d) {
                // Sem slot livre ou o caminho está no meio de um decode do
                // prefetch: falha "macia" (quadro vazio); o próximo pedido pega
                // já quente. Bloquear aqui só pioraria o tiquinho do corte.
                qWarning().noquote() << QStringLiteral("[dec] pool sem slot/ocupado para %1").arg(path);
                emit frameReady(clipId, path, t, maxW, QImage(), false);
                return;
            }
            if (!m_curPath.isEmpty()) m_pool->release(m_curPath);
            m_curPath = path;
            m_decoder = d;
            // O acquire pode ter sido FRIO (open 500-800ms; decoder ainda não
            // posicionado): os m_ready do arquivo antigo não valem mais aqui.
            m_readyValid = false;
        }

        // Pipeline de 1 frame à frente: quando o próximo frame já foi
        // decodificado na folga (m_ready*), devolve instantâneo e decodifica o
        // seguinte. Esconde a latência do decode quando ele passa de 33ms
        // (fontes pesadas, ex. 4K) — o preview mantém a cadência mesmo que um
        // frame individual demore. Em seek/atraso cai para decode normal.
        // O passo usa a cadência do PROJETO (dt = 1/fps do projeto), não a do
        // arquivo: a UI pede quadros no ritmo do projeto, e prefetchar com o
        // fps do arquivo (fd) deixava o m_ready fora de fase quando os dois
        // diferem, entregando frame repetido ou atrasado.
        const double step = (dt > 0.0) ? dt
                            : ((m_decoder && m_decoder->fps() > 0.0) ? 1.0 / m_decoder->fps() : 1.0 / 30.0);
        QImage img;
        static QElapsedTimer dbgClock;
        const bool dbgOn = playDbg();
        if (dbgOn && !dbgClock.isValid()) dbgClock.start();
        const qint64 dbgT0 = dbgOn ? dbgClock.nsecsElapsed() : 0;
        bool dbgWasReady = false;
        if (m_readyValid && m_readyPath == path && m_readyMaxW == maxW
            && std::fabs(m_readyT - t) <= step * 0.5) {
            img = m_readyImg;
            m_readyValid = false;
            dbgWasReady = true;
        } else {
            img = m_decoder->frameAt(t, maxW, &m_cancel);
        }
        if (dbgOn) {
            const double dbgMs = (dbgClock.nsecsElapsed() - dbgT0) / 1000000.0;
            if (dbgMs > 3.0 || dbgWasReady)
                qDebug().noquote() << QStringLiteral("[w] t=%1 %2 %3ms")
                          .arg(t, 0, 'f', 3)
                          .arg(dbgWasReady ? QStringLiteral("ready") : QStringLiteral("dec"), 6)
                          .arg(dbgMs, 0, 'f', 1);
        }
        // Caminho rápido: clipes sem efeitos temporais (LAINKA/MotiOn/OFX/
        // frei0r) têm o crop + efeitos básicos/máscaras aplicados AQUI, na
        // thread de vídeo — o C2 (copy do applyCropTo) e o C3 (per-pixel dos
        // efeitos básicos) saem do tiquinho da UI.
        const bool fxApplied = fx.active && !img.isNull();
        if (fxApplied) {
            img = PreviewWidget::applyCropTo(img, fx.cropL, fx.cropR, fx.cropT, fx.cropB);
            PreviewWidget::applyBasicEffectsOn(img, fx.clip, fx.rel);
        }
        emit frameReady(clipId, path, t, maxW, img, fxApplied);

        // Decodifica o próximo frame na folga para o próximo pedido (apenas se
        // o decoder ainda estiver no mesmo arquivo).
        if (!img.isNull() && m_decoder && m_decoder->isOpen() && m_decoder->source() == path) {
            m_readyImg = m_decoder->frameAt(t + step, maxW, &m_cancel);
            m_readyT = t + step;
            m_readyPath = path;
            m_readyMaxW = maxW;
            m_readyValid = !m_readyImg.isNull();
        } else {
            m_readyValid = false;
        }
    }

    void decodePrefetch(const QString& path, double t, int maxW, double step) {
        static QElapsedTimer dbgClock;
        const bool dbgOn = playDbg();
        if (dbgOn && !dbgClock.isValid()) dbgClock.start();
        const qint64 dbgT0 = dbgOn ? dbgClock.nsecsElapsed() : 0;
        // Camada de baixo com o MESMO arquivo do topo: o topo já empresta o
        // decoder (busy) — decodePrefetch roda na MESMA thread do decodeOne
        // (seriado), então usar m_decoder é seguro e não precisa do lease.
        const bool own = (path == m_curPath);
        FFmpegDecoder* d = own ? m_decoder : m_pool->acquire(path);
        if (!d) {
            // Pool ocupado/sem slot: deixa a camada de baixo vazia nesta transição.
            emit prefetchReady(path, t, maxW, QImage());
            return;
        }
        const QImage img = d->frameAt(t, maxW);
        if (!img.isNull() && step > 0.0) d->frameAt(t + step, maxW);
        if (!own) m_pool->release(path);
        if (dbgOn) {
            const double dbgMs = (dbgClock.nsecsElapsed() - dbgT0) / 1000000.0;
            qDebug().noquote() << QStringLiteral("[w] PREFETCH t=%1 %2ms")
                      .arg(t, 0, 'f', 3)
                      .arg(dbgMs, 0, 'f', 1);
        }
        emit prefetchReady(path, t, maxW, img);
    }

    void desengasga() {
        // Desengasgo periódico (a cada ~10s de reprodução): o decode contínuo
        // deixa o decoder com o DPB em resolução cheia + caches de 2 frames
        // retidos — degrada o frameAt ao longo de vídeos longos (vídeo engasga,
        // áudio perfeito). Libera os buffers internos SEM invalidar os ready:
        // m_ready é QImage JÁ decodificada (snapshot) — vale mesmo após o flush.
        // Invalidá-lo era o que criava um buraco de 1-2 frames: o primeiro
        // pedido pós-flush caía em decode síncrono e chegava atrasado em relação
        // ao relógio. Com o ready vivo, o próximo frame continua instantâneo e o
        // re-seek frio fica só no pre-decode seguinte. Só vale para o decoder
        // EMPRESTADO (o do pool ocioso não degrada: não está decodificando).
        if (m_decoder && m_decoder->isOpen()) m_decoder->releaseBuffers();
    }

signals:
    void frameReady(const QString& clipId, const QString& path, double t, int maxW, const QImage& img,
                    bool processed);
    void prefetchReady(const QString& path, double t, int maxW, const QImage& img);

private:
    DecoderPool* m_pool;            // não dono (vida do PreviewWidget)
    FFmpegDecoder* m_decoder = nullptr; // lease atual do pool (posse entre decodeOne)
    QString m_curPath;              // caminho do lease atual

    // Frame decodificado adiante (pipeline de 1 frame à frente).
    QImage m_readyImg;
    QString m_readyPath;
    double m_readyT = -1.0;
    int m_readyMaxW = 0;
    bool m_readyValid = false;

    std::atomic<bool> m_cancel{false};       // pedido de cancelamento pendente
    std::atomic<bool> m_cancelArmed{false};  // true somente durante decodeOne
};

// Thread separada para prefetch — decodifica o próximo clipe em background
// sem bloquear o FrameWorker principal (decodePrefetch original levava
// 500-800ms de open+frameAt, congelando o worker e quebrando o m_ready).
// Aqui não há posse: o prefetch só AQUECE o DecoderPool — abre o arquivo na
// posição do corte e deixa a entrada ociosa (release). No corte, o FrameWorker
// reacquire a mesma entrada já quente em µs. Como acquire/release são
// exclusivos (busy), jamais há acesso concorrente ao mesmo decoder.
class BgPrefetchWorker : public QObject {
    Q_OBJECT
public:
    explicit BgPrefetchWorker(DecoderPool* pool, QObject* parent = nullptr)
        : QObject(parent), m_pool(pool) {}

public slots:
    void decode(const QString& path, double t, int maxW, double step) {
        CrashReporter::setActivity("prefetch do proximo clipe");
        static QElapsedTimer dbgClock;
        const bool dbgOn = playDbg();
        if (dbgOn && !dbgClock.isValid()) dbgClock.start();
        const qint64 dbgT0 = dbgOn ? dbgClock.nsecsElapsed() : 0;

        FFmpegDecoder* d = m_pool->acquire(path);
        if (!d) {
            // Pool sem slot ou o caminho está emprestado ao worker principal
            // (mesmo arquivo): prefetch é best-effort, o corte segue esperando
            // pelo acquire quente do FrameWorker.
            emit failed(path);
            return;
        }
        QImage frame0 = d->frameAt(t, maxW);
        QImage frame1;
        if (!frame0.isNull() && step > 0.0)
            frame1 = d->frameAt(t + step, maxW);
        m_pool->release(path);

        if (dbgOn) {
            const double dbgMs = (dbgClock.nsecsElapsed() - dbgT0) / 1000000.0;
            qDebug().noquote() << QStringLiteral("[w] PREFETCH(bg) t=%1 %2ms")
                      .arg(t, 0, 'f', 3)
                      .arg(dbgMs, 0, 'f', 1);
        }
        emit done(path, t, maxW, frame0, frame1);
    }

    // Aquece um clipe mais à frente (Fase 2): só abre e posiciona o decoder no
    // início do clip, sem devolver frame. No corte daquele clip o acquire do
    // FrameWorker volta quente em µs. Serializado com decode() na mesma thread.
    void warm(const QString& path, double t, int maxW) {
        CrashReporter::setActivity("aquecendo clipe a frente");
        static QElapsedTimer dbgClock;
        const bool dbgOn = playDbg();
        if (dbgOn && !dbgClock.isValid()) dbgClock.start();
        const qint64 dbgT0 = dbgOn ? dbgClock.nsecsElapsed() : 0;

        FFmpegDecoder* d = m_pool->acquire(path);
        if (d) {
            d->frameAt(t, maxW); // posiciona o decoder; soft se já quente
            m_pool->release(path);
            if (dbgOn) {
                const double dbgMs = (dbgClock.nsecsElapsed() - dbgT0) / 1000000.0;
                qDebug().noquote() << QStringLiteral("[w] WARM(bg) t=%1 %2ms")
                          .arg(t, 0, 'f', 3)
                          .arg(dbgMs, 0, 'f', 1);
            }
        }
        emit warmed(path);
    }

signals:
    void done(const QString& path, double t, int maxW,
              const QImage& frame0, const QImage& frame1);
    void failed(const QString& path);
    void warmed(const QString& path);

private:
    DecoderPool* m_pool; // não dono (vida do PreviewWidget)
};

// Mixer de áudio: soma o PCM de todos os clipes ativos em `t` (clipe de vídeo
// + faixas de áudio), cada um com volume próprio (clipe, envelope e faixa).
// A camada de conform (AudioConformCache) decodifica cada arquivo+stream UMA
// vez, em background, para S16/48 kHz/estéreo; o mixer só lê fatias por índice
// — um corte é leitura contígua do mesmo buffer, então eco de junção é
// impossível. A thread do QAudioSink chama readData(); a UI chama updateSources()
// conforme o playhead avança.

// DSP dos efeitos de áudio do preview — réplica em tempo real do que a
// exportação aplica via ffmpeg:
//   equalizer f=120/1000/6000 Q=1  -> biquad peaking por banda
//   aeval (inverter fase)          -> multiplica por -1
//   afftdn (denoise)               -> gate espectral (STFT + piso adaptativo)
//   loudnorm (normalizar -14 LUFS) -> aproximação: AGC lento + soft clip
class AudioFxFallback {
public:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        void peaking(double freq, double q, double gainDb, double fs) {
            const double A = std::pow(10.0, gainDb / 40.0);
            const double w0 = 2.0 * M_PI * freq / fs;
            const double alpha = std::sin(w0) / (2.0 * q);
            const double a0 = 1.0 + alpha / A;
            b0 = (1.0 + alpha * A) / a0;
            b1 = (-2.0 * std::cos(w0)) / a0;
            b2 = (1.0 - alpha * A) / a0;
            a1 = (-2.0 * std::cos(w0)) / a0;
            a2 = (1.0 - alpha / A) / a0;
        }
        inline double tick(double x) {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
        void reset() { x1 = x2 = y1 = y2 = 0.0; }
    };

    // Reverb Schroeder simples (4 combos + 2 allpass num barramento mono).
    struct Comb {
        float buf[8192];
        int len = 1;
        int pos = 0;
        float feedback = 0.0f;
        float damping = 0.0f;
        float filter = 0.0f;
        void setup(int length, float fb, float damp) {
            len = qMax(1, length);
            feedback = fb;
            damping = damp;
            pos = 0;
            filter = 0.0f;
            memset(buf, 0, sizeof(buf));
        }
        inline float tick(float in) {
            const float output = buf[pos];
            filter = output * (1.0f - damping) + filter * damping;
            buf[pos] = in + filter * feedback;
            if (++pos >= len) pos = 0;
            return output;
        }
    };
    struct Allpass {
        float buf[8192];
        int len = 1;
        int pos = 0;
        void setup(int length) {
            len = qMax(1, length);
            pos = 0;
            memset(buf, 0, sizeof(buf));
        }
        inline float tick(float in) {
            const float bufo = buf[pos];
            const float out = -in + bufo;
            buf[pos] = in + bufo * 0.5f;
            if (++pos >= len) pos = 0;
            return out;
        }
    };
    struct SimpleReverb {
        Comb comb[4];
        Allpass allpass[2];
        void setup(double size) {
            const int delays[4] = { 1557, 1617, 1491, 1422 };
            const int apDelays[2] = { 225, 556 };
            const float fb = 0.60f + 0.28f * (float)size;
            const float damp = 0.4f - 0.25f * (float)size;
            for (int i = 0; i < 4; ++i) comb[i].setup(delays[i], fb, damp);
            for (int i = 0; i < 2; ++i) allpass[i].setup(apDelays[i]);
        }
        inline float tick(float in) {
            float o = 0.016f * comb[0].tick(in)
                    + 0.016f * comb[1].tick(in)
                    + 0.023f * comb[2].tick(in)
                    + 0.027f * comb[3].tick(in);
            o = allpass[0].tick(o);
            o = allpass[1].tick(o);
            return o;
        }
    };

    // FFT radix-2 iterativa (idêntica à do kernel Rust).
    static void fftRadix2(QVector<double>& re, QVector<double>& im) {
        const int n = re.size();
        if (n <= 1) return;
        int j = 0;
        for (int i = 1; i < n; ++i) {
            int bit = n >> 1;
            while (j & bit) { j ^= bit; bit >>= 1; }
            j ^= bit;
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const double ang = -2.0 * M_PI / len;
            const double wlenRe = std::cos(ang);
            const double wlenIm = std::sin(ang);
            const int half = len >> 1;
            for (int i = 0; i < n; i += len) {
                double wRe = 1.0, wIm = 0.0;
                for (int k = 0; k < half; ++k) {
                    const double uRe = re[i + k];
                    const double uIm = im[i + k];
                    const int idx = i + k + half;
                    const double vRe = re[idx] * wRe - im[idx] * wIm;
                    const double vIm = re[idx] * wIm + im[idx] * wRe;
                    re[i + k] = uRe + vRe;
                    im[i + k] = uIm + vIm;
                    re[idx] = uRe - vRe;
                    im[idx] = uIm - vIm;
                    const double nWr = wRe * wlenRe - wIm * wlenIm;
                    wIm = wRe * wlenIm + wIm * wlenRe;
                    wRe = nWr;
                }
            }
        }
    }
    static void ifftRadix2(QVector<double>& re, QVector<double>& im) {
        for (double& v : im) v = -v;
        fftRadix2(re, im);
        const double n = (double)re.size();
        for (int i = 0; i < re.size(); ++i) {
            re[i] /= n;
            im[i] = -im[i] / n;
        }
    }

    // Denoise espectral: STFT 512/hop 256 + piso por mediana + gate em dB.
    // Fallback C++ do SpectralDenoise de pierrot_audiofx/src/lib.rs — as duas
    // implementações devem produzir o mesmo áudio.
    //
    // Ver a nota longa no cabeçalho do lib.rs: os pontos que realmente
    // importavam foram (1) `outHead` POR CANAL — quando era um campo só, o
    // canal direito perdia as amostras de índice ímpar e o áudio saía com
    // −3 dB e fase invertida; (2) sqrt-Hann em vez de Hann (−2,3 dB e
    // modulação a 187 Hz); (3) `preroll` POR CANAL, senão o início do clipe
    // saía com fade-in (envelope a 0.48 do steady-state); (5) mediana em vez
    // de mínimo, porque o mínimo de um tom estável É a potência do tom; (6) as
    // duas proteções por cv e por tendência, que impedem o gate de cortar tom,
    // voz e sweep.
    //
    // Medido (mesmos testes do lib.rs): tom 997 Hz a=30 −0,11 dB; sweep
    // 200→3200 Hz a=30 −0,44 dB; voz+chiado a=18 −3,28 dB; chiado branco
    // a=30 −5,66 dB; silêncio permanece silêncio.
    //
    // Limitações conhecidas: a remoção satura perto de −6 dB (a=50 → −6,00 dB)
    // porque o piso mediano subestima o ruído — é o preço de proteger o sinal.
    // E a API de streaming é de tamanho fixo: há latência fixa de N+HOP
    // amostras e os últimos HOP (5,3 ms) de cada clipe não são renderizados.
    struct SpectralDenoise {
        static constexpr int FFT = 512;
        static constexpr int HOP = 256;
        static constexpr int BINS = FFT / 2 + 1;
        static constexpr int MIN_WIN = 40;      // ≈210 ms de histórico por bin
        static constexpr int LEARN_FRAMES = 56; // ≈300 ms sem gate
        static constexpr double FLOOR_BIAS = 0.7; // calibra a mediana
        static constexpr double GAIN_TAU = 0.030; // suavização temporal
        static constexpr double CV_PROTECT = 0.35; // cv < isso ⇒ estacionário
        static constexpr double RISE_PROTECT = 6.0;// mag > mediana·6 ⇒ sinal
        static constexpr double GATE_LO_DB = 0.0; // abaixo disso = no piso
        static constexpr double FS = 48000.0;

        bool enabled = false;
        double amount = 12.0;
        QVector<double> window;   // sqrt-Hann periódica
        QVector<double> inFifo[2];
        QVector<double> ola[2];
        QVector<double> noise[2];   // piso de ruído (potência)
        QVector<double> gainDb[2]; // ganho suavizado em dB
        QVector<double> freqDb, freqDb2; // ping-pong da suavização em frequência
        QVector<double> hist;      // 2·BINS·MIN_WIN, cursor circular
        int hpos[2] = {0, 0};      // POR CANAL
        QVector<double> win;       // janela de trabalho (mediana por bin)
        QVector<double> outFifo[2];
        int outHead[2] = {0, 0};   // POR CANAL (bug: era compartilhado)
        int preroll[2] = {0, 0};   // POR CANAL
        QVector<double> fftRe, fftIm, mag, rawDb;
        int frames = 0;

        SpectralDenoise() {
            window.resize(FFT);
            for (int i = 0; i < FFT; ++i)
                window[i] = std::sqrt(0.5 - 0.5 * std::cos(2.0 * M_PI * i / FFT));
            reset(); // zera todos os buffers
        }
        void reset() {
            inFifo[0].clear(); inFifo[1].clear();
            ola[0].fill(0.0, FFT); ola[1].fill(0.0, FFT);
            noise[0].fill(1e-12, BINS); noise[1].fill(1e-12, BINS);
            gainDb[0].fill(0.0, BINS); gainDb[1].fill(0.0, BINS);
            freqDb.fill(0.0, BINS);
            freqDb2.fill(0.0, BINS);
            win.fill(0.0, MIN_WIN);
            hist.fill(0.0, 2 * BINS * MIN_WIN);
            hpos[0] = 0; hpos[1] = 0;
            outFifo[0].clear(); outFifo[1].clear();
            outHead[0] = 0; outHead[1] = 0;
            preroll[0] = HOP; preroll[1] = HOP;
            fftRe.fill(0.0, FFT); fftIm.fill(0.0, FFT); mag.fill(0.0, BINS);
            rawDb.fill(0.0, BINS);
            frames = 0;
        }
        void setParams(bool en, double amt) {
            enabled = en;
            amount = std::clamp(amt, 1.0, 50.0);
        }
        double popOut(int ch) {
            const int h = outHead[ch];
            if (h >= outFifo[ch].size()) {
                outFifo[ch].clear();
                outHead[ch] = 0;
                return 0.0;
            }
            const double v = outFifo[ch][h];
            outHead[ch] = h + 1;
            // Pré-roll: descarta o 1º segmento, que viria só com uma frame de
            // contribuição (peso w²[0..N/2] incompleto).
            if (preroll[ch] > 0) {
                --preroll[ch];
                return 0.0;
            }
            if (h > 8192 && h * 2 > outFifo[ch].size()) {
                outFifo[ch].remove(0, h);
                outHead[ch] = 0;
            }
            return v;
        }
        void processChannelFrame(int ch) {
            for (int i = 0; i < FFT; ++i) {
                fftRe[i] = inFifo[ch][i] * window[i];
                fftIm[i] = 0.0;
            }
            fftRadix2(fftRe, fftIm);
            for (int b = 0; b < BINS; ++b)
                mag[b] = fftRe[b] * fftRe[b] + fftIm[b] * fftIm[b];

            // Histórico circular (cursor por canal).
            const int hp = hpos[ch];
            for (int b = 0; b < BINS; ++b)
                hist[(ch * BINS + b) * MIN_WIN + hp] = mag[b];
            hpos[ch] = (hp + 1) % MIN_WIN;

            // No warmup o histórico tem posições nunca escritas: pré-preenche
            // com o frame atual para o piso sair de uma estimativa real já na
            // 1ª frame. `frames` incrementa 1x por canal.
            const int filled = qMax(1, qMin(frames, MIN_WIN));
            if (filled < MIN_WIN) {
                for (int b = 0; b < BINS; ++b) {
                    const int base = (ch * BINS + b) * MIN_WIN;
                    for (int k = hp + 1; k < MIN_WIN; ++k)
                        hist[base + k] = mag[b];
                }
            }

            // Piso = mediana da janela, com as duas proteções:
            //   cv < CV_PROTECT  ⇒ bin estacionário (tom cv 0.00, voz 0.02,
            //                        chiado 1.0) ⇒ intocado;
            //   mag atual > mediana·RISE_PROTECT ⇒ o bin está sendo ocupado
            //                        por sinal agora (sweep/música).
            for (int b = 0; b < BINS; ++b) {
                const int base = (ch * BINS + b) * MIN_WIN;
                for (int k = 0; k < filled; ++k) win[k] = hist[base + k];
                for (int k = filled; k < MIN_WIN; ++k) win[k] = std::numeric_limits<double>::infinity();
                std::sort(win.begin(), win.end());
                const double median = win[filled / 2];
                double sum = 0.0;
                for (int k = 0; k < filled; ++k) sum += win[k];
                const double mean = sum / filled;
                double var = 0.0;
                for (int k = 0; k < filled; ++k) {
                    const double d = win[k] - mean;
                    var += d * d;
                }
                var /= filled;
                const double cv = mean > 0.0 ? std::sqrt(var) / mean
                                            : std::numeric_limits<double>::infinity();
                const bool rising = mag[b] > median * RISE_PROTECT;
                noise[ch][b] = (cv < CV_PROTECT || rising)
                    ? 1e-12
                    : std::max(median * FLOOR_BIAS, 1e-12);
            }

            // Gate: rampa de −amount dB no piso até 0 dB em `amount` dB acima.
            if (frames < LEARN_FRAMES) {
                rawDb.fill(0.0, BINS);
            } else {
                for (int b = 0; b < BINS; ++b) {
                    const double snrDb = 10.0 * std::log10(std::max(mag[b], 1e-300) / noise[ch][b]);
                    const double t = std::clamp((snrDb - GATE_LO_DB) / amount, 0.0, 1.0);
                    rawDb[b] = amount * (t - 1.0);
                }
            }

            // Suavização temporal em dB (τ = 30 ms) e em frequência (3 taps × 2).
            const double at = 1.0 - std::exp(-(double)HOP / (FS * GAIN_TAU));
            for (int b = 0; b < BINS; ++b) {
                gainDb[ch][b] += (rawDb[b] - gainDb[ch][b]) * at;
                freqDb[b] = gainDb[ch][b];
            }
            // Ping-pong para não alocar a cada frame.
            for (int pass = 0; pass < 2; ++pass) {
                freqDb.swap(freqDb2);
                for (int b = 1; b < BINS - 1; ++b)
                    freqDb[b] = 0.25 * freqDb2[b - 1] + 0.5 * freqDb2[b] + 0.25 * freqDb2[b + 1];
                freqDb[0] = freqDb2[0];
                freqDb[BINS - 1] = freqDb2[BINS - 1];
                for (int b = 0; b < BINS; ++b) gainDb[ch][b] = freqDb[b];
            }

            for (int b = 0; b < BINS; ++b) {
                const double lin = std::clamp(std::pow(10.0, gainDb[ch][b] / 20.0), 0.0, 1.0);
                fftRe[b] *= lin;
                fftIm[b] *= lin;
            }
            ++frames;

            ifftRadix2(fftRe, fftIm);
            // OLA com a mesma sqrt-Hann: w_a·w_s = Hann, soma 1.0 com 50% overlap.
            for (int i = 0; i < FFT; ++i)
                ola[ch][i] += fftRe[i] * window[i];
            for (int i = 0; i < HOP; ++i)
                outFifo[ch].append(ola[ch][i]);
            for (int i = 0; i < FFT - HOP; ++i)
                ola[ch][i] = ola[ch][i + HOP];
            for (int i = FFT - HOP; i < FFT; ++i)
                ola[ch][i] = 0.0;
            inFifo[ch].remove(0, HOP);
        }
        void processStereo(double* data, int nframes) {
            if (!enabled) return;
            for (int f = 0; f < nframes; ++f) {
                inFifo[0].append(data[2 * f]);
                inFifo[1].append(data[2 * f + 1]);
                while (inFifo[0].size() >= FFT) {
                    processChannelFrame(0);
                    processChannelFrame(1);
                }
                data[2 * f] = popOut(0);
                data[2 * f + 1] = popOut(1);
            }
        }
    };

    void configure(double eqLow, double eqMid, double eqHigh, bool denoise,
                   double denoiseAmt, bool invertPhase, bool normalize,
                   bool reverb, double reverbMix, double reverbSize) {
        const double fs = 48000.0;
        const int key = (int)std::llround(eqLow * 10) * 1000000
                      + (int)std::llround(eqMid * 10) * 1000
                      + (int)std::llround(eqHigh * 10)
                      + (denoise ? 100 : 0)
                      + (invertPhase ? 200 : 0)
                      + (normalize ? 400 : 0)
                      + (int)std::llround(denoiseAmt) * 10000
                      + (reverb ? 800 : 0)
                      + (int)std::llround(reverbMix * 100) * 100000
                      + (int)std::llround(reverbSize * 100) * 10000000;
        if (key == m_key) return; // parâmetros inalterados: mantém o estado
        m_key = key;
        for (int ch = 0; ch < 2; ++ch) {
            low[ch].peaking(120.0, 1.0, std::clamp(eqLow, -12.0, 12.0), fs);
            mid[ch].peaking(1000.0, 1.0, std::clamp(eqMid, -12.0, 12.0), fs);
            high[ch].peaking(6000.0, 1.0, std::clamp(eqHigh, -12.0, 12.0), fs);
        }
        invert = invertPhase;
        dn.setParams(denoise, denoiseAmt);
        agcEnabled = normalize;
        reverbEnabled = reverb;
        reverbMixAmt = std::clamp(reverbMix, 0.0, 1.0);
        reverbSizeAmt = std::clamp(reverbSize, 0.0, 1.0);
        rv.setup(reverbSizeAmt);
        resetState();
    }

    void resetState() {
        for (int ch = 0; ch < 2; ++ch) {
            low[ch].reset(); mid[ch].reset(); high[ch].reset();
        }
        dn.reset();
        agcLevel = 0.0;
        agcGain = 1.0;
        rv.setup(reverbSizeAmt); // limpa os buffers do reverb
    }

    // Processa `frames` amostras estéreo interleaved S16 no próprio buffer.
    void process(int16_t* buf, int frames) {
        QVector<double> tmp((qsizetype)frames * 2);
        for (int f = 0; f < frames; ++f) {
            double l = buf[2 * f] / 32768.0;
            double r = buf[2 * f + 1] / 32768.0;
            l = high[0].tick(mid[0].tick(low[0].tick(l)));
            r = high[1].tick(mid[1].tick(low[1].tick(r)));
            if (invert) { l = -l; r = -r; }
            tmp[2 * f] = l;
            tmp[2 * f + 1] = r;
        }
        dn.processStereo(tmp.data(), frames);
        for (int f = 0; f < frames; ++f) {
            double l = tmp[2 * f];
            double r = tmp[2 * f + 1];
            if (agcEnabled) {
                const double lvl = 0.5 * (l * l + r * r);
                agcLevel = agcLevel * 0.999 + lvl * 0.001;
                const double db = 20.0 * std::log10(std::sqrt(agcLevel) + 1e-9);
                const double want = -14.0 - db; // ganho (dB) rumo a -14
                const double g = std::pow(10.0, std::clamp(want, -12.0, 12.0) / 20.0);
                agcGain = agcGain * 0.95 + g * 0.05;
                l *= agcGain; r *= agcGain;
                const double pk = qMax(std::fabs(l), std::fabs(r));
                if (pk > 0.95) { const double s = 0.95 / pk; l *= s; r *= s; }
            }
            if (reverbEnabled && reverbMixAmt > 0.01) {
                const double dry = 0.5 * (l + r);
                const double wet = rv.tick((float)dry) * 4.0;
                const double w = reverbMixAmt;
                l = l * (1.0 - w) + wet * w;
                r = r * (1.0 - w) + wet * w;
            }
            buf[2 * f] = (int16_t)std::lround(std::clamp(l, -1.0, 1.0) * 32768.0);
            buf[2 * f + 1] = (int16_t)std::lround(std::clamp(r, -1.0, 1.0) * 32768.0);
        }
    }

private:
    int m_key = -1;
    Biquad low[2], mid[2], high[2];
    bool invert = false;
    bool agcEnabled = false;
    double agcLevel = 0.0;
    double agcGain = 1.0;
    bool reverbEnabled = false;
    double reverbMixAmt = 0.0;
    double reverbSizeAmt = 0.5;
    SimpleReverb rv; // DSP do Reverb EX (evita conflito com o parâmetro bool)
    SpectralDenoise dn;
};

// O DSP dos efeitos vive no kernel Rust (lib pierrot_audiofx) quando o build
// é feito com -DPIERROT_ENABLE_RUST=ON; senão uso o fallback C++ acima. A
// interface (configure/resetState/process) é idêntica, então o resto do
// PreviewWidget não muda de cara.
#ifdef PIERROT_ENABLE_RUST
using AudioFx = AudioFxBridge;
#else
using AudioFx = AudioFxFallback;
#endif

class AudioMixer : public QIODevice {
public:
    struct SourceInfo {
        QString key;      // id do clipe (estável durante a reprodução)
        QString path;     // arquivo de mídia
        int audioStream = 0; // stream de áudio usado por este clipe
        double mediaPos;  // posição no arquivo de mídia (em segundos)
        double vol = 1.0;
        double eqLow = 0.0;
        double eqMid = 0.0;
        double eqHigh = 0.0;
        bool denoise = false;
        double denoiseAmount = 12.0;
        bool normalize = false;
        bool invertPhase = false;
        bool reverb = false;
        double reverbMix = 0.35;
        double reverbSize = 0.5;
        int trackIndex = -1;   // índice da faixa (dentro de video/audio)
        bool isAudioTrack = false;
        double pan = 0.0;      // -1..+1, 0=centro
        double speed = 1.0;     // frames de mídia por frame de saída (=taxa do clipe)
        double clipPos = 0.0;   // início do clipe na linha do tempo (s)
        double clipDur = 0.0;   // duração do clipe na linha do tempo (s)
        double mediaStart = 0.0; // início do intervalo de mídia do clipe (in, s)
        // FX de áudio da FAIXA (aplicados ao barramento da faixa de áudio,
        // depois da soma dos clipes dela; iguais para todas as fontes dela).
        bool trackFxOn = false;
        double trackFxEqLow = 0.0;
        double trackFxEqMid = 0.0;
        double trackFxEqHigh = 0.0;
        bool trackFxDenoise = false;
        double trackFxDenoiseAmount = 12.0;
        bool trackFxInvertPhase = false;
        bool trackFxReverb = false;
        double trackFxReverbMix = 0.35;
        double trackFxReverbSize = 0.5;
    };

    explicit AudioMixer(QObject* parent = nullptr) : QIODevice(parent) {
        setOpenMode(ReadOnly | Unbuffered);
    }
    ~AudioMixer() override {
        QMutexLocker job(&m_jobMutex);
        QMutexLocker l(&m_mutex);
        qDeleteAll(m_sources);
        m_sources.clear();
    }

    // Atualiza o conjunto de fontes para o playhead atual. Com a camada de
    // conform (AudioConformCache) aqui NÃO há mais decoders: cada fonte lê, por
    // índice de amostra, uma fatia do PCM já decodificado em background.
    //
    // COSTURA SEM LACUNA: cada Source contribui apenas dentro do seu intervalo
    // de TEMPO DE SAÍDA absoluto [srcOutStart, srcOutEnd). O clipe à direita de
    // um corte é criado como fonte ATIVA já no lookahead (warm) — mas gated
    // (contribui zero até o seu início). Assim, na "chunk" que cruza a costura,
    // a esquerda contribui [.., corte) e a direita [corte, ..): é uma troca de
    // contribuição no MESMO buffer, amostra-exata, sem eco nem vazio. O antigo
    // clamp por média (endFrame) injetava silêncio no sink → era o "flick".
    //
    // reseek=true re-ancora o relógio de saída (m_outFrame) no novo playhead e
    // re-inicializa todas as fontes; `seq` ficou sem efeito.
    void updateSources(const QVector<SourceInfo>& want, bool reseek,
                       const QVector<SourceInfo>& warm = QVector<SourceInfo>(),
                       int seq = -1, double anchorSec = 0.0) {
        QMutexLocker job(&m_jobMutex);
        Q_UNUSED(seq);
        QMutexLocker l(&m_mutex);
        if (m_shutdown) return;

        auto& conform = AudioConformCache::instance();

        if (reseek) {
            // Re-âncora o tempo de saída no novo playhead.
            m_outFrame =
                (qint64)std::llround(anchorSec * AudioConformCache::kSampleRate);
            // Limpa o tail persistente: a posição de costura anterior não é
            // mais válida (o playhead pulou).
            m_tailXX.clear();
        }

        // Reconstrói as cadeias FX por faixa a cada atualização: evita
        // entradas órfãs e garante que o estado acompanhe os parâmetros.
        m_trackFx.clear();

        // Fixa a contribuição de um Source no tempo de saída: gating por
        // [srcOutStart, srcOutEnd) e ponteiro de mídia ancorado no frame de
        // saída em que a fonte passa a contribuir (leitura contínua do conform).
        const qint64 sr = AudioConformCache::kSampleRate;
        auto initSource = [&](Source* s, const SourceInfo& w) {
            const qint64 clipStartSr = (qint64)std::llround(w.clipPos * sr);
            const qint64 outStart = qMax(clipStartSr, m_outFrame);
            s->srcOutStart = outStart;
            s->srcOutEnd = (qint64)std::llround((w.clipPos + w.clipDur) * sr);
            s->readFrame = (qint64)std::llround(w.mediaStart * sr)
                         + (qint64)std::llround((outStart - clipStartSr) * w.speed);
            s->step = qMax(0.01, w.speed);
            s->fx.resetState();
        };
        auto applyParams = [&](Source* s, const SourceInfo& w) {
            s->vol = w.vol;
            s->pan = w.pan;
            s->trackIndex = w.trackIndex;
            s->isAudioTrack = w.isAudioTrack;
            s->step = qMax(0.01, w.speed);
            s->srcOutEnd = (qint64)std::llround((w.clipPos + w.clipDur) * sr);
            s->fx.configure(w.eqLow, w.eqMid, w.eqHigh, w.denoise,
                            w.denoiseAmount, w.invertPhase, w.normalize,
                            w.reverb, w.reverbMix, w.reverbSize);
        };
        const bool jumping = reseek;

        // Dedupe por GEOMETRIA DE MÍDIA sobreposta (mesmo arquivo + stream e
        // janelas de saída que se INTERCEPTAM). É a duplicação real: duas
        // cópias do mesmo trecho (ex.: clipe de vídeo + clipe de áudio do
        // mesmo grupo, ou clipe duplicado) contribuem juntas → soma ~2x no
        // corte (pico de volume) e eco (mesmo conteúdo defasado). Mantém a
        // PRIMEIRA cópia vista; a seguinte é descartada. Se a ordem alternar
        // entre ticks, o Source sobrevive sem reinicialização (não-fresh), então
        // a leitura contínua não quebra.
        struct GeomRec { QString ps; qint64 os, oe; };
        QVector<GeomRec> geomSeen;
        auto geomDup = [&](const SourceInfo& w) {
            const qint64 cStartSr = (qint64)std::llround(w.clipPos * sr);
            const qint64 os = qMax(cStartSr, m_outFrame);
            const qint64 oe = (qint64)std::llround((w.clipPos + w.clipDur) * sr);
            const QString ps = w.path + QLatin1Char('|')
                             + QString::number(w.audioStream);
            for (const GeomRec& g : geomSeen)
                if (g.ps == ps && os < g.oe && g.os < oe) return true;
            geomSeen.append({ps, os, oe});
            return false;
        };

        // Ordena as entradas de forma DETERMINÍSTICA (cópia da faixa de áudio
        // primeiro; desempate por key). O overvivente do dedupe de geometria
        // depende da ordem: se ela oscilar entre ticks (QHash do `reps`),
        // a cada tick um Source é apagado e o outro recriado → churn do cache
        // do conform → micro-silêncios = a "flicada". Com ordem estável, o
        // mesmo Source sobrevive a reprodução inteira.
        QVector<SourceInfo> wantO = want;
        QVector<SourceInfo> warmO = warm;
        const auto preferStable = [](const SourceInfo& a, const SourceInfo& b) {
            if (a.isAudioTrack != b.isAudioTrack) return a.isAudioTrack;
            if (a.key != b.key) return a.key < b.key;
            return false;
        };
        std::stable_sort(wantO.begin(), wantO.end(), preferStable);
        std::stable_sort(warmO.begin(), warmO.end(), preferStable);

        QSet<QString> keep;
        for (const SourceInfo& w : wantO) {
            if (geomDup(w)) continue; // duplicata: a primeira cópia já é suficiente
            keep.insert(w.key);
            Source* s = findLocked(w.key);
            if (!s) { s = new Source; s->key = w.key; m_sources.append(s); }
            const bool fresh = jumping || !s->cache
                || s->path != w.path || s->stream != w.audioStream;
            if (fresh) {
                s->cache = conform.get(w.path, w.audioStream);
                s->path = w.path;
                s->stream = w.audioStream;
                initSource(s, w);
            }
            // Conform: garante o trecho sob o playhead + horizonte de leitura.
            // 4s (era 3s): dá folga para o worker preencher à frente sem
            // lacuna no corte, mesmo com sessões do doFill em rodízio.
            conform.request(s->cache, w.mediaPos, 4.0);
            applyParams(s, w);
            if (w.isAudioTrack) {
                AudioFx& tfx = m_trackFx[qMakePair(w.isAudioTrack, w.trackIndex)];
                tfx.configure(w.trackFxEqLow, w.trackFxEqMid, w.trackFxEqHigh,
                              w.trackFxDenoise, w.trackFxDenoiseAmount,
                              w.trackFxInvertPhase, false,
                              w.trackFxReverb, w.trackFxReverbMix, w.trackFxReverbSize);
            }
        }

        // Lookahead (janela adiante): fontes criadas ATIVAS, mas contribuindo
        // zero até o seu frame de saída de início (gating). No corte, o clipe
        // direito já estará no mix na chunk que atravessa a costura.
        for (const SourceInfo& w : warmO) {
            if (keep.contains(w.key)) continue;
            if (geomDup(w)) continue; // duplicata: a primeira cópia já é suficiente
            keep.insert(w.key);
            Source* s = findLocked(w.key);
            if (!s) { s = new Source; s->key = w.key; m_sources.append(s); }
            if (jumping || !s->cache
                || s->path != w.path || s->stream != w.audioStream) {
                s->cache = conform.get(w.path, w.audioStream);
                s->path = w.path;
                s->stream = w.audioStream;
                initSource(s, w);
            }
            conform.request(s->cache, w.mediaPos, 6.0);
            applyParams(s, w);
        }

        // Recolhe fontes fora da janela (dropa o Ref; o LRU libera a memória).
        m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(),
                                       [&](Source* s) {
                                           if (keep.contains(s->key)) return false;
                                           delete s;
                                           return true;
                                       }),
                        m_sources.end());
    }

    // Desliga o mixer de forma síncrona antes do deleteLater: rejeita novos
    // updates e derruba as fontes (os Refs saem; a conform continua viva no
    // registro e será reaproveitada no próximo startAudio).
    void shutdown() {
        QMutexLocker job(&m_jobMutex);
        QMutexLocker l(&m_mutex);
        m_shutdown = true;
        qDeleteAll(m_sources);
        m_sources.clear();
    }

    void setMasterVolume(double v) { m_masterVolume = v; }

    // Espera (com timeout) o head de QUEM vai tocar já agora estar conformado,
    // antes de o sink começar a puxar. Elimina o "começa mudo / dessincronizado"
    // do warm-up frio: o áudio começa do conteúdo certo.
    void waitReadyBeforeSink(int framesAhead, int timeoutMs) {
        if (framesAhead <= 0) return;
        AudioConformCache& conform = AudioConformCache::instance();
        struct Need { AudioConformCache::Ref cache; qint64 frame; int frames; };
        QVector<Need> needs;
        {
            QMutexLocker l(&m_mutex);
            for (Source* s : m_sources) {
                if (!s->cache) continue;
                if (s->srcOutStart > m_outFrame) continue; // só quem toca agora
                if (s->srcOutEnd <= m_outFrame) continue;
                const qint64 needEnd = qMin(s->srcOutEnd,
                                            s->readFrame + framesAhead);
                if (needEnd > s->readFrame)
                    needs.append({s->cache, s->readFrame,
                                  (int)(needEnd - s->readFrame)});
            }
        }
        for (const Need& n : needs) conform.waitReady(n.cache, n.frame, n.frames,
                                                      timeoutMs);
    }

    // Níveis de áudio por faixa e master (thread-safe para MixerWidget).
    struct TrackLevels {
        QHash<QPair<bool,int>, float> rms; // (isAudio, trackIndex) → RMS 0..1
        float masterRms = 0.0;
    };
    TrackLevels currentLevels() const {
        QMutexLocker ll(&m_levelMutex);
        return m_levels;
    }

    qint64 readData(char* data, qint64 maxlen) override {
        QMutexLocker l(&m_mutex);
        const int ch = kChannels;
        const int bytesPerSample = 2 * ch; // S16 interleaved
        const int maxBytes = (int)qMin<qint64>(maxlen, INT_MAX);
        const int capacity = (maxBytes / bytesPerSample) * bytesPerSample;
        memset(data, 0, capacity);

        if (m_shutdown || capacity <= 0) return capacity;

        // Sem fontes no instante (lacuna entre clipes, fim): silêncio, mas o
        // relógio de saída SEGUE em tempo real — do contrário, na volta de um
        // clipe o srcOutStart estaria muito à frente de um m_outFrame congelado
        // (i0 além do buffer) e o áudio perderia o passo com o vídeo.
        if (m_sources.isEmpty()) {
            if (audioDbg())
                qDebug() << "[audio] readData: sem fontes (silêncio)";
            m_outFrame += capacity / bytesPerSample;
            return capacity;
        }

        // Scratch de leitura por fonte: PCM S16 do conform. `winBuf` (8x) cobre
        // a interpolação de speed até ~8x (além disso os índices saturaram).
        const int nFrames = capacity / bytesPerSample;
        if (m_srcBuf.size() < capacity / 2) m_srcBuf.resize(capacity / 2);
        const int winCap = (qMax(16, nFrames * 8 + 4)) * 2;
        if (m_winBuf.size() < winCap) m_winBuf.resize(winCap);
        QVector<int16_t>& srcBuf = m_srcBuf;
        QVector<int16_t>& winBuf = m_winBuf;
        AudioConformCache& conform = AudioConformCache::instance();

        // Acumuladores de RMS por faixa.
        QHash<QPair<bool,int>, double> sumSq;
        QHash<QPair<bool,int>, int>    countSq;

        // Barramentos por faixa (estilo Vegas): cada fonte entra no barramento
        // da SUA faixa de áudio; depois o FX da faixa processa o barramento e
        // por fim ele é somado no master. Fontes de faixas de vídeo entram nos
        // barramentos normalmente (sem FX de faixa), preservando a soma.
        //
        // NOTA: `buses` é LOCAL ao chunk (não reutilizado). A topologia
        // bus↔busOrder precisa ser idêntica entre eles (criados juntos no
        // busOf); persistir em membro desalinha os índices entre chunks e o
        // mix lia lixo (chiado).
        QVector<QVector<int16_t>> buses;
        QList<QPair<bool,int>> busOrder;
        QHash<QPair<bool,int>, int> busIdx;
        auto busOf = [&](bool a, int ti) -> int {
            const QPair<bool,int> k{a, ti};
            auto it = busIdx.constFind(k);
            if (it != busIdx.cend()) return it.value();
            const int idx = (int)buses.size();
            buses.append(QVector<int16_t>(capacity / 2));
            busIdx.insert(k, idx);
            busOrder.append(k);
            return idx;
        };

        // Dedupe: duas fontes com a MESMA janela de mídia (mesmo arquivo,
        // stream, inicio/fim de saida e frame de leitura) só contribuem uma
        // vez. Antes, um clipe duplicado/travado no mesmo spot somava 2x no
        // barramento (volume "estoura" no corte). A tolerância de ~16 frames
        // cobre duplicatas levemente deslocadas (que soariam como COMB/ECO).
        //
        // NOTA: um crossfade legítimo entre clipes do MESMO arquivo na mesma
        // posição é indistinguível de duplicação — nestes casos o mix abre mão
        // da camada (raro e, na prática, eco).
        struct SeenWin { QString ps; qint64 os, oe, rf; };
        QVector<SeenWin> seenWin;
        // Última amostra bruta (pós-FX) dos trechos que terminam no meio do
        // chunk, por faixa e índice: alimenta o declick por detecção.
        // Seed do tail persistente (across chunks) para que o de-click funcione
        // no boundary entre chunks — sem isso, o crossfade não dispara e o
        // corte estoura/popa.
        QHash<QPair<bool,int>, QHash<int, int32_t>> tailXX = m_tailXX;
        const auto windowKey = [](const Source* s0) {
            return s0->path + QLatin1Char('|') + QString::number(s0->stream);
        };
        for (Source* s : m_sources) {
            if (!s->cache) continue;
            // Gating por tempo de SAÍDA absoluto: esta fonte só contribui nos
            // frames de saída [srcOutStart, srcOutEnd) ∩ [m_outFrame, chunkEnd).
            // Na costura de um corte, esquerda cobre [.., corte) e direita já
            // está no mix cobrindo [corte, ..): troca amostra-exata, sem lacuna
            // (o antigo clamp de mídia injetava silêncio no sink = flick).
            const qint64 chunkEnd = m_outFrame + nFrames;
            const qint64 cStart = qMax(m_outFrame, s->srcOutStart);
            const qint64 cEnd = qMin(chunkEnd, s->srcOutEnd);
            if (cEnd <= cStart) continue; // totalmente fora da janela
            const int i0 = (int)(cStart - m_outFrame);
            const int i1 = (int)(cEnd - m_outFrame);
            if (i1 <= i0) continue;
            bool dupWin = false;
            for (const SeenWin& w : seenWin) {
                if (w.ps == windowKey(s) && qAbs(w.os - s->srcOutStart) <= 16
                    && qAbs(w.oe - s->srcOutEnd) <= 16
                    && qAbs(w.rf - s->readFrame) <= 16) {
                    dupWin = true;
                    break;
                }
            }
            if (dupWin) {
                continue;
            }
            seenWin.append({windowKey(s), s->srcOutStart, s->srcOutEnd,
                            s->readFrame});
            memset(srcBuf.data(), 0, capacity);

            const int produced = i1 - i0;
            if (qAbs(s->step - 1.0) < 1e-6) {
                // Caminho rápido (speed 1x): leitura em bloco + silêncio nas
                // lacunas ainda não conformadas. `avail` pode ser menor que a
                // janela se o worker ainda não cobriu: a fonte ESPERA ali (não
                // avança sobre o vazio) — a lacuna é retentada no próximo chunk
                // sem perder trecho nem acelerar o relógio.
                const int avail =
                    conform.readFrames(s->cache, s->readFrame, i1 - i0,
                                       srcBuf.data() + i0 * 2);
                s->readFrame += avail;
            } else {
                // speed ≠ 1: interpolação linear posicional — corrige pitch e
                // duração, que o playback por decoder fazia errado em fluxo.
                const double step = qBound(0.01, s->step, 8.0);
                memset(winBuf.data(), 0, (size_t)winBuf.size() * sizeof(int16_t));
                const int winFrames = winBuf.size() / 2;
                const int spanF = (int)qMin<qint64>(
                    (qint64)winFrames, (qint64)std::llround((i1 - i0) * step) + 2);
                if (spanF > 0)
                    (void)conform.readFrames(s->cache, s->readFrame, spanF,
                                             winBuf.data());
                const int16_t* p = winBuf.constData();
                const int wc = qMax(0, qMin(spanF, winFrames) - 1);
                for (int i = i0; i < i1; ++i) {
                    const double rel = (i - i0) * step;
                    const int f0 = qMin((qint64)wc, (qint64)rel);
                    const double frac = qBound(0.0, rel - f0, 1.0);
                    const int f1 = qMin(f0 + 1, wc);
                    const int al = p[f0 * 2], ar = p[f0 * 2 + 1];
                    const int bl = p[f1 * 2], br = p[f1 * 2 + 1];
                    srcBuf[i * 2] = (int16_t)qBound(
                        -32768, std::lround(al + (bl - al) * frac), 32767);
                    srcBuf[i * 2 + 1] = (int16_t)qBound(
                        -32768, std::lround(ar + (br - ar) * frac), 32767);
                }
                s->readFrame += (qint64)std::llround((i1 - i0) * step);
            }

            // Conform autossustentado: a janela pedida segue o relógio de LEITURA
            // desta fonte (não o tick do frame). Ex.: só-áudio (fps<=0) o tick
            // retorna cedo no PlaybackEngine e nunca re-pediria o trecho —
            // o áudio ia mudo depois da 1ª janela (~metade). Aqui o worker está
            // sempre sendo alimentado na frente do que o sink lê.
            conform.request(s->cache,
                            (double)s->readFrame
                                / (double)AudioConformCache::kSampleRate,
                            3.0);

            s->fx.process(srcBuf.data(), nFrames);

            // Registra a última amostra deste trecho se ele termina no meio
            // do chunk (será a "costura"): o próximo contribuinte compara com
            // a própria primeira amostra para só suavizar quando houver salto
            // real (descontinuidade). Registro por faixa e índice de fim.
            if (i1 < nFrames) {
                const int li = (i1 - 1) * 2;
                QHash<int, int32_t>& m =
                    tailXX[qMakePair(s->isAudioTrack, s->trackIndex)];
                m.insert(i1, ((int32_t)srcBuf[li]) << 16
                                 | ((uint16_t)(uint16_t)srcBuf[li + 1]));
            }

            // De-click POR DETECÇÃO (corte duro sem fade): mede o salto entre
            // a última amostra do trecho anterior (já no barramento) e a
            // primeira deste. Só aplica crossfade curto (~1.3ms) quando há
            // descontinuidade real. Costura CONTÍNUA (subclipes do mesmo
            // arquivo) fica intacta — sem dip = sem "flicada".
            constexpr int kDeclick = 64; // ~1.3ms @48k
            const int prod = i1 - i0;
            const bool headSeam = i0 > 0
                || (s->srcOutStart == m_outFrame && s->srcOutStart > 0);
            if (headSeam && prod > 0) {
                const int n = qMin(kDeclick, prod);
                bool doCrossfade = false;
                const int16_t firstL = srcBuf[i0 * 2];
                const int16_t firstR = srcBuf[i0 * 2 + 1];
                const auto tit =
                    tailXX.constFind(qMakePair(s->isAudioTrack, s->trackIndex));
                bool havePred = false;
                if (tit != tailXX.cend()) {
                    const auto sit = tit->constFind(i0);
                    if (sit != tit->cend()) {
                        havePred = true;
                        const int32_t v = sit.value();
                        const int16_t prevL = (int16_t)(v >> 16);
                        const int16_t prevR = (int16_t)(v & 0xffff);
                        const float jL = std::fabs((float)prevL - firstL) / 32768.0f;
                        const float jR = std::fabs((float)prevR - firstR) / 32768.0f;
                        if (std::max(jL, jR) > 0.25f) doCrossfade = true;
                    }
                }
                if (!havePred
                    && std::max(qAbs((int)firstL), qAbs((int)firstR)) > 8192) {
                    // Início sem antecessor (início de clipe/gap): fade-in se
                    // alto; custura exatamente no limite de chunk rara.
                    doCrossfade = true;
                }
                if (doCrossfade) {
                    if (i0 > 0) {
                        // Fade out do trecho anterior já no barramento, junto
                        // com o fade in deste → crossfade anti-pop sem dip.
                        int16_t* busf =
                            buses[busOf(s->isAudioTrack, s->trackIndex)].data();
                        for (int k = 0; k < n; ++k) {
                            const float g = 0.5f
                                * (1.0f + std::cos(float(M_PI * (k + 1)) / n));
                            const int idx = (i0 - 1 - k) * 2;
                            busf[idx] = (int16_t)std::lround(busf[idx] * g);
                            busf[idx + 1] =
                                (int16_t)std::lround(busf[idx + 1] * g);
                        }
                    }
                    for (int k = 0; k < n; ++k) {
                        const float g = 0.5f
                            * (1.0f - std::cos(float(M_PI * (k + 1)) / n));
                        const int idx = (i0 + k) * 2;
                        srcBuf[idx] = (int16_t)std::lround(srcBuf[idx] * g);
                        srcBuf[idx + 1] =
                            (int16_t)std::lround(srcBuf[idx + 1] * g);
                    }
                }
            }

            const int16_t* src = srcBuf.constData();
            int16_t* bus = buses[busOf(s->isAudioTrack, s->trackIndex)].data();
            // Pan por canal, mesma regra do export: centro = 1:1 (0dB, sem
            // atenuação — senão o preview tocaria ~3dB abaixo) e equal-power
            // quando deslocado. Preserva o stereo na posição central.
            const float pan = (float)s->pan;
            float gL, gR;
            if (std::fabs(pan) <= 1e-3f) {
                gL = 1.0f;
                gR = 1.0f;
            } else {
                gL = std::sqrt(std::max(0.0f, (1.0f - pan) * 0.5f));
                gR = std::sqrt(std::max(0.0f, (1.0f + pan) * 0.5f));
            }
            const float volL = (float)s->vol * gL;
            const float volR = (float)s->vol * gR;
            double localSumSq = 0.0;
            for (int i = i0; i < i1; ++i) {
                const int li = i * 2;
                const int ri = li + 1;
                // Soma SEM clamp intermediário de [-1,1]: o barramento acumula
                // o som real da faixa (o clamp só ocorre na escrita final).
                bus[li] = (int16_t)std::lround(
                    qBound(-32768.0f, bus[li] + src[li] * volL, 32767.0f));
                bus[ri] = (int16_t)std::lround(
                    qBound(-32768.0f, bus[ri] + src[ri] * volR, 32767.0f));
                const float sL = src[li] / 32768.0f * volL;
                const float sR = src[ri] / 32768.0f * volR;
                localSumSq += sL * sL + sR * sR;
            }
            // RMS deste clipe para a faixa correspondente.
            const float rms = std::sqrt(localSumSq / std::max(1, produced * ch));
            const auto key = qMakePair(s->isAudioTrack, s->trackIndex);
            sumSq[key] += rms * rms;
            countSq[key] += 1;
        }
        // O relógio de saída avança uma "chunk" por leitura — sempre.
        m_outFrame += nFrames;

        // Persiste o tail entre chunks: o próximo readData herda as amostras
        // de costura para que o de-click funcione no boundary.
        m_tailXX = tailXX;

        // Aplica o FX de cada faixa de áudio e soma os barramentos no master.
        {
            int16_t* o = reinterpret_cast<int16_t*>(data);
            const int totalSamples = capacity / 2;
            for (int bi = 0; bi < (int)buses.size(); ++bi) {
                const QPair<bool,int>& key = busOrder[bi];
                if (key.first) { // faixa de áudio: processa a cadeia da faixa
                    auto tit = m_trackFx.find(key);
                    if (tit != m_trackFx.cend())
                        tit.value().process(buses[bi].data(), totalSamples / 2);
                }
                const int16_t* bus = buses[bi].constData();
                for (int i = 0; i < totalSamples; ++i) {
                    const float s = o[i] / 32768.0f + bus[i] / 32768.0f;
                    const int v = std::lround(qBound(-1.0f, s, 1.0f) * 32768.0f);
                    o[i] = (int16_t)v;
                }
            }
        }

        // Aplica volume master no buffer final.
        const float mv = (float)m_masterVolume;
        if (std::fabs(mv - 1.0f) > 1e-4f) {
            int16_t* p = reinterpret_cast<int16_t*>(data);
            const int totalSamples = capacity / 2;
            for (int i = 0; i < totalSamples; ++i)
                p[i] = (int16_t)std::lround(qBound(-32768.0f, p[i] * mv, 32767.0f));
        }

        // RMS master (buffer final após aplicar masterVolume).
        double masterSq = 0.0;
        const int16_t* final = reinterpret_cast<const int16_t*>(data);
        const int totalSamples = capacity / 2;
        for (int i = 0; i < totalSamples; ++i) {
            const float s = final[i] / 32768.0f;
            masterSq += s * s;
        }
        const float masterRms = std::sqrt(masterSq / std::max(1, totalSamples));

        // Salva níveis (thread-safe).
        {
            QMutexLocker ll(&m_levelMutex);
            m_levels.masterRms = masterRms;
            m_levels.rms.clear();
            for (auto it = sumSq.cbegin(); it != sumSq.cend(); ++it) {
                const int cnt = countSq.value(it.key(), 1);
                m_levels.rms[it.key()] = std::sqrt(it.value() / cnt);
            }
        }

        return capacity;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
    qint64 bytesAvailable() const override { return 4096; }

private:
    static constexpr int kChannels = 2;
    struct Source {
        QString key;
        AudioConformCache::Ref cache; // conform PCM do arquivo+stream
        QString path;                 // arquivo de mídia
        int stream = -1;              // stream de áudio
        qint64 readFrame = 0;   // mídia (frame S16/48k/estéreo) do 1º frame de
                                // contribuição desta fonte (== mídia(srcOutStart))
        qint64 srcOutStart = 0; // frame de saída absoluto: começa a contribuir
        qint64 srcOutEnd = 0;   // frame de saída absoluto (exclusivo): para
        double step = 1.0;      // frames de mídia por frame de saída (=speed)
        double vol = 1.0;
        AudioFx fx;
        int trackIndex = -1;
        bool isAudioTrack = false;
        double pan = 0.0;
    };
    Source* findLocked(const QString& key) const {
        for (Source* s : m_sources) if (s->key == key) return s;
        return nullptr;
    }
    QVector<Source*> m_sources;
    // Cadeia FX por faixa de áudio (chave = {isAudioTrack, trackIndex});
    // aplicada ao barramento da faixa no readData antes de somar no master.
    QHash<QPair<bool,int>, AudioFx> m_trackFx;
    QMutex m_mutex;
    QMutex m_jobMutex;          // serializa updateSources/shutdown/destrutor
    bool m_shutdown = false; // stopAudio(): rejeita novos updates/decodes
    double m_masterVolume = 1.0;
    qint64 m_outFrame = 0;    // frame de saída absoluto da próxima leitura
    mutable QMutex m_levelMutex;
    TrackLevels m_levels;

    // Scratch de leitura reutilizado entre chunks do thread de áudio em tempo
    // real: só realoca quando a capacidade cresce (estável após o QAudioSink
    // configurar o formato), evitando malloc/allocator por chamada (~10-20ms).
    // (Os barramentos por faixa NÃO são reutilizados — a topologia bus↔busOrder
    // é recriada por chunk para garantir índices alinhados.)
    QVector<int16_t> m_srcBuf;              // PCM de uma fonte (capacity/2)
    QVector<int16_t> m_winBuf;              // interpolação speed≠1
    // Tail persistente entre chunks: última amostra pós-FX de cada faixa no
    // fim do chunk anterior. Alimenta o de-click na costura entre chunks —
    // sem isso, o crossfade não dispara no boundary e o corte estoura/popa.
    QHash<QPair<bool,int>, QHash<int, int32_t>> m_tailXX;
};


namespace {

// Timecode estilo Vegas/DaVinci: HH:MM:SS:FF (frames, no fps do projeto).
QString fmtTimecode(double t, double fps) {
    const int fr = qMax(1, (int)std::llround(fps));
    int ff = (int)std::llround(t * fr);
    const int h = ff / (3600 * fr); ff %= 3600 * fr;
    const int m = ff / (60 * fr);   ff %= 60 * fr;
    const int s = ff / fr;          ff %= fr;
    return QString("%1:%2:%3:%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ff, 2, 10, QLatin1Char('0'));
}

// Glifos monocromáticos do transporte do monitor (como o Premiere desenha os
// botões planos do Program Monitor: setas, play/pausa, loop e margens).
enum MonitorGlyph { MStepBack, MPlay, MPause, MStepFwd, MLoop, MSafe, MFullscreen };

static QIcon makeMonitorIcon(MonitorGlyph g, const QColor& color) {
    auto draw = [g](QPainter& p, const QColor& c) {
        QPen pen(c, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        switch (g) {
        case MStepBack:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF t; t << QPointF(4.5, 8.0) << QPointF(10.5, 3.5) << QPointF(10.5, 12.5);
              p.drawPolygon(t); }
            p.drawRect(QRectF(11.4, 4.0, 2.2, 8.0));
            break;
        case MPlay:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF t; t << QPointF(5.5, 3.8) << QPointF(13.0, 8.0) << QPointF(5.5, 12.2);
              p.drawPolygon(t); }
            break;
        case MPause:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRect(QRectF(5.0, 3.8, 2.6, 8.4));
            p.drawRect(QRectF(9.0, 3.8, 2.6, 8.4));
            break;
        case MStepFwd:
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF t; t << QPointF(11.5, 8.0) << QPointF(5.5, 3.5) << QPointF(5.5, 12.5);
              p.drawPolygon(t); }
            p.drawRect(QRectF(2.4, 4.0, 2.2, 8.0));
            break;
        case MLoop:
            // Seta circular (retorno/loop): arco + cabeça de seta na ponta.
            p.drawArc(QRectF(3.0, 3.5, 10.0, 9.0), -40 * 16, -240 * 16);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF h; h << QPointF(12.4, 3.0) << QPointF(13.6, 7.2) << QPointF(15.6, 5.4);
              p.drawPolygon(h); }
            break;
        case MSafe:
            // Margens de segurança: retângulo com marcas centrais.
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(3.0, 3.0, 10.0, 10.0));
            p.drawLine(QPointF(8.0, 3.0), QPointF(8.0, 3.9));
            p.drawLine(QPointF(8.0, 12.1), QPointF(8.0, 13.0));
            p.drawLine(QPointF(3.0, 8.0), QPointF(3.9, 8.0));
            p.drawLine(QPointF(12.1, 8.0), QPointF(13.0, 8.0));
            break;
        case MFullscreen:
            // Expandi: quatro cantos apontando para fora, como o botão de
            // fullscreen do Premiere.
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            { QPolygonF c1; c1 << QPointF(3.0, 7.0) << QPointF(3.0, 3.0) << QPointF(7.0, 3.0); p.drawPolygon(c1); }
            { QPolygonF c2; c2 << QPointF(13.0, 3.0) << QPointF(9.0, 3.0) << QPointF(13.0, 7.0); p.drawPolygon(c2); }
            { QPolygonF c3; c3 << QPointF(3.0, 9.0) << QPointF(3.0, 13.0) << QPointF(7.0, 13.0); p.drawPolygon(c3); }
            { QPolygonF c4; c4 << QPointF(13.0, 13.0) << QPointF(13.0, 9.0) << QPointF(9.0, 13.0); p.drawPolygon(c4); }
            break;
        }
    };
    QIcon icon;
    for (int s : {1, 2}) {
        QPixmap pm(16 * s, 16 * s);
        pm.setDevicePixelRatio(s);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        draw(p, color);
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

double projFps(const Project* p) {
    return (p && p->fps > 0.0) ? p->fps : 30.0;
}

// Mapeia o modo de composição da faixa (os mesmos 12 modos da exportação)
// para o composition mode do QPainter. "subtract" usa SourceOut como
// aproximação (Dst × (1 - Src)), que é o mais próximo do subtract no Qt.
QPainter::CompositionMode blendModeToQt(const QString& mode) {
    if (mode == QStringLiteral("screen"))    return QPainter::CompositionMode_Screen;
    if (mode == QStringLiteral("multiply"))  return QPainter::CompositionMode_Multiply;
    if (mode == QStringLiteral("overlay"))   return QPainter::CompositionMode_Overlay;
    if (mode == QStringLiteral("darken"))    return QPainter::CompositionMode_Darken;
    if (mode == QStringLiteral("lighten"))   return QPainter::CompositionMode_Lighten;
    if (mode == QStringLiteral("softlight")) return QPainter::CompositionMode_SoftLight;
    if (mode == QStringLiteral("hardlight")) return QPainter::CompositionMode_HardLight;
    if (mode == QStringLiteral("difference"))return QPainter::CompositionMode_Difference;
    if (mode == QStringLiteral("addition"))  return QPainter::CompositionMode_Plus;
    if (mode == QStringLiteral("subtract")) return QPainter::CompositionMode_SourceOut;
    if (mode == QStringLiteral("exclusion")) return QPainter::CompositionMode_Exclusion;
    return QPainter::CompositionMode_SourceOver;
}
}

struct PreviewQOpt { int width; const char* label; const char* code; };
static const PreviewQOpt kPreviewQualities[] = {
    {360,  "360p",  "360p"},
    {480,  "480p",  "480p"},
    {720,  "720p",  "720p"},
    {1080, "1080p", "1080p"},
    {3840, "4K",    "4K"},
};

void PreviewWidget::refreshTimeLabelStyle() {
    if (!m_timeLabel) return;
    const auto& c = themeColors();
    m_timeLabel->setStyleSheet(
        QStringLiteral(
            "QLabel {"
            "  color: %1;"
            "  background-color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 2px;"
            "  padding: 2px 12px;"
            "  letter-spacing: 1px;"
            "}").arg(c.accent.name(), c.monitorBg.name(), c.canvasBorder.name()));
}

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    qRegisterMetaType<FrameFx>("FrameFx"); // snapshot de efeitos p/ o worker
    // O QElapsedTimer da instrumentação precisa estar rodando antes do primeiro
    // tick — sem start() ele fica inválido para sempre e todas as leituras de
    // latência (worker/prefetch) davam 0, mascarando o gargalo de decode.
    m_perfT.start();
    const QColor glyph = themeColors().monitorLabel;
    QColor hoverBg = themeColors().canvasBorder;
    hoverBg.setAlpha(70);
    QColor checkBg = themeColors().accent;
    checkBg.setAlpha(55);
    m_playIcon = makeMonitorIcon(MPlay, glyph);
    m_pauseIcon = makeMonitorIcon(MPause, glyph);

    // Transporte do monitor, espelhado no Program Monitor do Premiere: botões
    // planos no topo — quadro anterior, play/pausa, quadro seguinte e loop.
    m_stepBackBtn = new QToolButton(this);
    m_stepBackBtn->setIcon(makeMonitorIcon(MStepBack, glyph));
    m_stepBackBtn->setIconSize(QSize(18, 18));
    m_stepBackBtn->setFixedSize(30, 26);
    m_stepBackBtn->setToolTip(tr("Quadro anterior"));
    m_stepBackBtn->setCursor(Qt::PointingHandCursor);
    connect(m_stepBackBtn, &QToolButton::clicked, this, [this]() { stepFrame(-1); });

    // O PlaybackEngine troca o rótulo do botão em cada transição; aqui ele
    // fica só com o ícone (o texto é limpo no onStateChanged).
    m_playBtn = new QPushButton(this);
    m_playBtn->setIcon(m_playIcon);
    m_playBtn->setIconSize(QSize(20, 20));
    m_playBtn->setFixedSize(36, 26);
    m_playBtn->setToolTip(tr("Reproduzir/pausar (Espaço)"));
    m_playBtn->setCursor(Qt::PointingHandCursor);

    m_stepFwdBtn = new QToolButton(this);
    m_stepFwdBtn->setIcon(makeMonitorIcon(MStepFwd, glyph));
    m_stepFwdBtn->setIconSize(QSize(18, 18));
    m_stepFwdBtn->setFixedSize(30, 26);
    m_stepFwdBtn->setToolTip(tr("Quadro seguinte"));
    m_stepFwdBtn->setCursor(Qt::PointingHandCursor);
    connect(m_stepFwdBtn, &QToolButton::clicked, this, [this]() { stepFrame(1); });

    m_loopBtn = new QToolButton(this);
    m_loopBtn->setIcon(makeMonitorIcon(MLoop, glyph));
    m_loopBtn->setIconSize(QSize(18, 18));
    m_loopBtn->setFixedSize(30, 26);
    m_loopBtn->setCheckable(true);
    m_loopBtn->setToolTip(tr("Loop (repete o trecho in/out)"));
    m_loopBtn->setCursor(Qt::PointingHandCursor);
    connect(m_loopBtn, &QToolButton::toggled, this, &PreviewWidget::setLoopEnabled);

    // Contador de tempo estilo Premiere (timecode do Program Monitor): fonte
    // mono, azul sobre fundo quase preto, cantos quadrados — a assinatura
    // visual do monitor do Premiere. As cores vêm dos tokens para respeitar o
    // tema claro/escuro, como o resto do preview. Fica flutuando sobre o canto
    // inferior-esquerdo da área de vídeo (posicionado no resizeEvent).
    m_timeLabel = new QLabel(tr("00:00:00:00"), this);
    m_timeLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    QFont tcFont = m_timeLabel->font();
    tcFont.setFamily(QStringLiteral("Consolas, Menlo, DejaVu Sans Mono, monospace"));
    tcFont.setPointSize(11);
    tcFont.setBold(true);
    m_timeLabel->setFont(tcFont);
    refreshTimeLabelStyle();
    m_timeLabel->setMinimumWidth(150);

    // Rótulo da sequência no topo, como o "Program: <nome>" do Premiere.
    m_programLabel = new QLabel(tr("Program: "), this);
    QFont pf = m_programLabel->font();
    pf.setPointSize(9);
    pf.setBold(true);
    m_programLabel->setFont(pf);
    m_programLabel->setStyleSheet(
        QStringLiteral("QLabel{color:%1;}").arg(themeColors().monitorLabel.name()));

    // Fullscreen do monitor (o Premiere expande o Program para tela cheia;
    // Esc sai).
    m_fullscreenBtn = new QToolButton(this);
    m_fullscreenBtn->setIcon(makeMonitorIcon(MFullscreen, glyph));
    m_fullscreenBtn->setIconSize(QSize(18, 18));
    m_fullscreenBtn->setFixedSize(30, 26);
    m_fullscreenBtn->setToolTip(tr("Tela cheia (Esc para sair)"));
    m_fullscreenBtn->setCursor(Qt::PointingHandCursor);
    connect(m_fullscreenBtn, &QToolButton::clicked, this, [this]() {
        QWidget* w = window();
        if (w->isFullScreen()) w->showNormal();
        else w->showFullScreen();
    });
    auto* fsEsc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(fsEsc, &QShortcut::activated, this, [this]() {
        if (window()->isFullScreen()) window()->showNormal();
    });

    // ── Aba do topo: SÓ o rótulo "Program: <nome>", como no Premiere. O
    // transporte do Program Monitor fica na barra de BAIXO (é onde o Premiere
    // põe), não aqui em cima.
    m_topBar = new QWidget(this);
    m_topBar->setObjectName(QStringLiteral("pmTopBar"));
    m_topBar->setFixedHeight(24);
    auto* topLay = new QHBoxLayout(m_topBar);
    topLay->setContentsMargins(8, 0, 8, 0);
    topLay->setSpacing(0);
    topLay->addWidget(m_programLabel);
    topLay->addStretch(1);
    // Seletor por object name: um "QWidget{...}" cascatearia para os filhos e
    // contaminaria combo/rótulos com a borda da barra.
    m_topBar->setStyleSheet(QStringLiteral(
        "QWidget#pmTopBar{background:%1;border-bottom:1px solid %2;}")
        .arg(themeColors().base.name(), themeColors().trackBorder.name()));

    // Resolução do preview (como o menu de resolução do Program Monitor):
    // botão com menu no cluster direito, entre as margens e o zoom.
    m_qualityBtn = new QToolButton(this);
    m_qualityBtn->setPopupMode(QToolButton::InstantPopup);
    m_qualityBtn->setToolTip(tr("Qualidade do preview (resolução de decodificação)"));
    m_qualityMenu = new QMenu(this);
    for (const PreviewQOpt& o : kPreviewQualities) {
        QAction* a = m_qualityMenu->addAction(QString::fromUtf8(o.label));
        a->setCheckable(true);
        a->setData(o.width);
        if (o.width == m_previewQuality) a->setChecked(true);
        connect(a, &QAction::triggered, this, [this, o]() { setPreviewQuality(o.width); });
    }
    m_qualityBtn->setMenu(m_qualityMenu);
    // Mostra a qualidade atual no botão.
    for (const PreviewQOpt& o : kPreviewQualities)
        if (o.width == m_previewQuality) {
            m_qualityBtn->setText(QString::fromUtf8(o.code));
            break;
        }

    // ── Barra de rodapé: timecode à esquerda, transporte no centro-esquerda e
    // o cluster (margens → qualidade → zoom → tela cheia) à direita — a mesma
    // ordem da barra inferior do Program Monitor do Premiere.
    m_bottomBar = new QWidget(this);
    m_bottomBar->setObjectName(QStringLiteral("pmBottomBar"));
    m_bottomBar->setFixedHeight(30);
    auto* bar = new QHBoxLayout(m_bottomBar);
    bar->setContentsMargins(6, 2, 6, 2);
    bar->setSpacing(4);
    m_bottomBar->setStyleSheet(QStringLiteral(
        "QWidget#pmBottomBar{background:%1;border-top:1px solid %2;}"
        "QToolButton,QPushButton{border:none;border-radius:4px;background:transparent;}"
        "QToolButton:hover,QPushButton:hover{background:%3;}"
        "QToolButton:checked,QPushButton:checked{background:%4;}")
        .arg(themeColors().base.name(), themeColors().trackBorder.name(),
             hoverBg.name(QColor::HexArgb), checkBg.name(QColor::HexArgb)));
    // Atualiza o mini-scope a ~15 fps quando o monitor está visível.
    auto* miniTimer = new QTimer(this);
    miniTimer->setInterval(70);
    connect(miniTimer, &QTimer::timeout, this, [this]() {
        if (!m_miniScope || !isVisible()) return;
        m_miniScope->refreshFrom(scopesFrame());
    });
    miniTimer->start();
    // Transporte do Program Monitor: à esquerda, como no Premiere.
    bar->addWidget(m_stepBackBtn);
    bar->addWidget(m_playBtn);
    bar->addWidget(m_stepFwdBtn);
    bar->addWidget(m_loopBtn);
    bar->addSpacing(12);
    // Timecode: entre o transporte e o cluster da direita.
    bar->addWidget(m_timeLabel);
    // Mini-scopes no Program Monitor (Lumetri Premiere): waveform compacto.
    m_miniScope = new ScopeWidget(this);
    m_miniScope->setMode(ScopeWidget::Waveform);
    m_miniScope->setFixedSize(96, 24);
    m_miniScope->setToolTip(tr("Waveform (mini) — dock Analisadores para o full"));
    bar->addWidget(m_miniScope);
    bar->addStretch(1);

    // Margens de segurança do Premiere (Action 90% + Title 80%, Ctrl+G
    // alterna). No Premiere não existe grade NxN no Program Monitor — o que
    // aparece são os dois retângulos concêntricos de segurança.
    m_gridBtn = new QToolButton(this);
    m_gridBtn->setIcon(makeMonitorIcon(MSafe, themeColors().monitorLabel));
    m_gridBtn->setIconSize(QSize(18, 18));
    m_gridBtn->setFixedSize(30, 26);
    m_gridBtn->setCheckable(true);
    m_gridBtn->setChecked(false);
    m_gridBtn->setToolTip(tr("Margens de segurança (Ctrl+G)"));
    m_gridBtn->setCursor(Qt::PointingHandCursor);
    connect(m_gridBtn, &QToolButton::clicked, this, [this](bool checked) {
        m_showGrid = checked;
        update();
    });
    bar->addWidget(m_gridBtn);
    // Cluster direito do rodapé: margens → qualidade → zoom → tela cheia.
    bar->addWidget(m_qualityBtn);

    // Atalho Ctrl+G para alternar a grade.
    auto* gridShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this);
    connect(gridShortcut, &QShortcut::activated, this, [this]() {
        m_showGrid = !m_showGrid;
        m_gridBtn->setChecked(m_showGrid);
        update();
    });

    m_zoomCombo = new QComboBox(this);
    m_zoomCombo->addItem(tr("Ajustar"));
    for (int p : {25, 50, 75, 100, 150, 200})
        m_zoomCombo->addItem(tr("%1%").arg(p));
    m_zoomCombo->setCurrentIndex(0);
    m_zoomCombo->setToolTip(tr("Zoom do preview"));
    connect(m_zoomCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        static const double kZooms[] = {0.0, 0.25, 0.50, 0.75, 1.0, 1.5, 2.0};
        m_zoom = kZooms[qBound(0, idx, 6)];
        update();
    });
    bar->addWidget(m_zoomCombo);
    bar->addSpacing(4);
    bar->addWidget(m_fullscreenBtn);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_topBar);
    lay->addStretch(1);
    lay->addWidget(m_bottomBar);

    // O PlaybackEngine controla o transporte; o timer é deste widget (tem
    // QObject/parent) e repassa o timeout para o motor via tick().
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    // Timer rápido (8ms): o avanço de frame NÃO é decidido pelo jitter do event
    // loop (com 33ms fixo a cadência variava ±5ms → micro-engasgo contínuo).
    // O frame-guard do tick() retorna cedo quando o relógio ainda não cruzou o
    // frame boundary; com 8ms a resolução temporal é ~4x melhor e o playback
    // avança pelo relógio monotônico, não pela chegada do timeout.
    m_timer->setInterval(8);
    setTimer(m_timer);
    setPlayButton(m_playBtn);
    connect(m_timer, &QTimer::timeout, this, [this]() { PlaybackEngine::tick(); });
    connect(m_playBtn, &QPushButton::clicked, this, &PreviewWidget::togglePlay);

    // Pool de decodificadores quentes: os workers emprestam / devolvem entradas
    // por caminho de arquivo (o prefetch só aquece; o main leva no corte).
    m_decPool = new DecoderPool(decoderPoolCapacity());

    // Thread de vídeo: decodificar quadros aqui tira a decodificação (que é
    // cara em arquivos grandes/4K/MKV) do caminho da UI.
    m_frameThread = new CrashReporter::TrackedThread("preview-frame", this);
    m_frameWorker = new FrameWorker(m_decPool);
    m_frameWorker->moveToThread(m_frameThread);
    connect(m_frameThread, &QThread::finished, m_frameWorker, &QObject::deleteLater);
    connect(m_frameWorker, &FrameWorker::frameReady,
            this, &PreviewWidget::onFrameReady, Qt::QueuedConnection);
    connect(m_frameWorker, &FrameWorker::prefetchReady,
            this, &PreviewWidget::onPrefetchReady, Qt::QueuedConnection);
    m_frameThread->start();

    // Thread de prefetch em background: decodifica o próximo clipe fora do
    // caminho do worker (que mantém o pipeline m_ready). Sem isto, o open+2
    // frameAt do prefetch (500-800ms) congelava o worker por corte.
    m_bgPrefetchThread = new CrashReporter::TrackedThread("preview-prefetch", this);
    m_bgPrefetchWorker = new BgPrefetchWorker(m_decPool);
    m_bgPrefetchWorker->moveToThread(m_bgPrefetchThread);
    connect(m_bgPrefetchThread, &QThread::finished, m_bgPrefetchWorker, &QObject::deleteLater);
    connect(m_bgPrefetchWorker, &BgPrefetchWorker::done,
            this, &PreviewWidget::onBgPrefetchDone, Qt::QueuedConnection);
    connect(m_bgPrefetchWorker, &BgPrefetchWorker::failed,
            this, &PreviewWidget::onBgPrefetchFailed, Qt::QueuedConnection);
    connect(m_bgPrefetchWorker, &BgPrefetchWorker::warmed,
            this, &PreviewWidget::onBgWarmDone, Qt::QueuedConnection);
    m_bgPrefetchThread->start();

    setMinimumSize(320, 200);
}

PreviewWidget::~PreviewWidget() {
    stopAudio();
    if (m_frameThread) {
        // Esvazia a fila de decodificação antes de parar: com arquivos grandes
        // o worker podia estar a vários quadros de distância, o wait(2000)
        // estourava e o objeto morria com decode em andamento (crash no fecho).
        {
            QMutexLocker l(&m_frameMutex);
            m_reqQueue.clear();
        }
        m_frameThread->quit();
        m_frameThread->wait(5000);
    }
    if (m_bgPrefetchThread) {
        m_bgPrefetchThread->quit();
        m_bgPrefetchThread->wait(5000);
    }
    // Threads paradas e workers destruídos (deleteLater): nenhum lease ativo —
    // seguro fechar os decoders restantes do pool.
    delete m_decPool;
    m_decPool = nullptr;
}

void PreviewWidget::setProject(Project* p) {
    PlaybackEngine::setProject(p); // seta m_project e para o transporte
    if (m_programLabel) {
        const QString nome = (p && !p->name.isEmpty()) ? p->name : tr("Sem projeto");
        m_programLabel->setText(tr("Program: %1").arg(nome));
    }
    m_playhead = 0.0;
    m_mesaRenderer.clearCompositeCache(); // composto do projeto anterior não vale
    m_frame = QImage();
    m_frameFull = QImage();
    m_compositedCache = QImage();  // invalida cache do frame composto
    m_lastSrcT = -1.0;
    m_lastDecodeW = -1;
    m_lastFile.clear();
    m_lastCropL = m_lastCropR = m_lastCropT = m_lastCropB = -1;
    m_underFrame = QImage();
    m_underPath.clear();
    m_underT = -1.0;
    m_underW = -1;
    m_underRequested = false;
    m_transAlpha = -1.0;
    m_transType.clear();
    {
        QMutexLocker l(&m_frameMutex);
        m_reqQueue.clear();
        m_layerCache.clear();
        m_prefetch = PrefetchFrame();
        m_warmQueue.clear();
        m_warmInFlight.clear();
        m_shownPath.clear();
        m_shownT = -1.0;
        m_shownW = -1;
    }
    updateFrame();
    update();
}

void PreviewWidget::refreshView() {
    // Todo caminho de EDIÇÃO passa por aqui: TimelineWidget::modified,
    // MediaPoolWidget::mediaChanged (mídia trocada), GraphEditorWidget::modified
    // (keyframes de crop/LAINKA), PancropWidget::modified, MaskEditorDialog e
    // ExpressWidget. O tick de playback NÃO passa por aqui (ele vai por
    // applySeekVisual), então este é o ponto exato para invalidar o memo de
    // applyCrop(): a chave carrega tempo e crop, mas não os parâmetros de LAINKA,
    // MotiOn, efeitos básicos, máscaras ou a identidade do arquivo — mudar
    // qualquer um deles com o playhead parado manteria a chave e devolveria o
    // quadro antigo.
    m_cropMemo = QImage();
    // O cache do frame composto é chaveado só por índice de frame + nº de
    // camadas; edições de conteúdo (efeitos de clipe, da faixa de efeitos,
    // blend, opacidade) não mudam essas chaves e deixariam o composto velho
    // com o playhead parado. Invalida aqui, junto com o memo de crop.
    m_compositedCache = QImage();
    updateFrame();
    update();
}

QImage PreviewWidget::scopesFrame() const {
    QImage src;
    {
        QMutexLocker l(&m_frameMutex);
        src = m_compositedCache.isNull() ? m_frame : m_compositedCache;
    }
    if (src.isNull() || src.width() < 2 || src.height() < 2) return QImage();
    return src.scaled(160, 90, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
               .convertToFormat(QImage::Format_ARGB32);
}

QImage PreviewWidget::compositeFrame() const {
    QMutexLocker l(&m_frameMutex);
    return m_compositedCache.isNull() ? m_frame : m_compositedCache;
}

PreviewWidget::AudioLevels PreviewWidget::audioLevels() const {
    if (!m_audioFeed) return {};
    const AudioMixer::TrackLevels tl = m_audioFeed->currentLevels();
    AudioLevels out;
    out.rms = tl.rms;
    out.masterRms = tl.masterRms;
    return out;
}

void PreviewWidget::resizeEvent(QResizeEvent*) {
    // O vídeo ocupa o miolo entre a aba do topo e a barra de rodapé (onde o
    // Premiere põe o transporte).
    const int top = m_topBar ? m_topBar->height() : 24;
    const int bottom = m_bottomBar ? m_bottomBar->height() : 0;
    m_videoRect = QRect(0, top, width(), std::max(0, height() - top - bottom));
}

// ── LAINKA: usa funções compartilhadas de LainkaFx.h ────────────────
using namespace LainkaFx;

void PreviewWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    PreviewProfiler::Scope paintScope(PreviewProfiler::active() ? &profFrame().paintNs : nullptr);
    renderFrame(p);
    // Os indicadores vão SEMPRE por cima, em qualquer caminho de render. Antes
    // eles ficavam no fim de renderFrame(), que tem vários `return` antecipado
    // (projeto vazio, cache de composição, caminho de clip único) — ou seja,
    // em projetos com 2+ camadas o overlay de perf nunca aparecia, que é
    // justamente o cenário que se quer diagnosticar.
    drawPlaybackBadges(p);
}

void PreviewWidget::drawPlaybackBadges(QPainter& p) {
    // Indicador de frames perdidos (visível quando há drops, sem env var).
    if (m_playing && m_perf.droppedTotal > 0) {
        p.save();
        p.resetTransform();
        QFont df = p.font();
        df.setPointSizeF(8);
        df.setBold(true);
        p.setFont(df);
        const QString dropTxt = QStringLiteral("Dropped: %1").arg(m_perf.droppedTotal);
        const QRect dropR(width() - 130, 8, 122, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(180, 50, 50, 180));
        QPainterPath dropBg;
        dropBg.addRoundedRect(QRectF(dropR), 3.0, 3.0);
        p.drawPath(dropBg);
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(dropR, Qt::AlignCenter, dropTxt);
        p.restore();
    }

    // Indicador de qualidade adaptativa (mostra quando auto-baixou).
    if (m_adaptiveActive) {
        p.save();
        p.resetTransform();
        QFont af = p.font();
        af.setPointSizeF(8);
        af.setBold(true);
        p.setFont(af);
        const QString adpTxt = QStringLiteral("Auto: %1p").arg(m_previewQuality);
        const QRect adpR(width() - 130, m_playing && m_perf.droppedTotal > 0 ? 30 : 8, 122, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(50, 120, 180, 180));
        QPainterPath adpBg;
        adpBg.addRoundedRect(QRectF(adpR), 3.0, 3.0);
        p.drawPath(adpBg);
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(adpR, Qt::AlignCenter, adpTxt);
        p.restore();
    }

    // Diagnóstico de performance (PIERROT_PERF_DEBUG=1).
    drawPerfOverlay(p);
}

void PreviewWidget::renderFrame(QPainter& p) {

    // Fundo do console (área ao redor do monitor).
    p.fillRect(m_videoRect, themeColors().monitorBg);

    if (!m_project || m_project->width <= 0 || m_project->height <= 0) {
        drawEmptyMonitor(p, m_videoRect);
        return;
    }

    const double pw = m_project->width;
    const double ph = m_project->height;

    // Área útil do monitor.
    const QRect work = m_videoRect.adjusted(12, 12, -12, -12);

    // Escala: zoom fixo ou "Ajustar" (o quadro inteiro cabe na área).
    double k = m_zoom > 0.0 ? m_zoom
                            : qMin(work.width() / pw, work.height() / ph);
    QRect canvas(QPoint(0, 0), QSize(qMax(1, (int)(pw * k)), qMax(1, (int)(ph * k))));
    canvas.moveCenter(work.center());
    canvas = canvas.intersected(work); // centraliza e recorta quando zoom > 1

    // Monitor: o quadro assenta direto no fundo do painel, sem moldura — como
    // no Premiere (a resolução fica no menu do rodapé, não sobre a imagem).
    p.fillRect(canvas, themeColors().canvasBg);

    // Texto independente ativo no playhead (desenhado mesmo sem quadro de vídeo).
    bool anyText = false;
    if (m_project) {
        for (const Track& tr : m_project->videoTracks) {
            if (!tr.visible) continue;   // faixa oculta (olho) não mostra texto
            for (const Clip& c : tr.clips)
                if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur) {
                    anyText = true;
                    break;
                }
        }
    }

    // Render unificado em "espaço de projeto": o canvas É o quadro do projeto
    // (k = pixels de tela por pixel do projeto). O vídeo é desenhado do mesmo
    // jeito com ou sem transform — sem transform, ele cabe inteiro no quadro;
    // com transform, pan/rot/zoom giram em torno do centro do quadro. Assim,
    // adicionar um keyframe de transform nunca muda o tamanho do vídeo.
    const Clip* clip = clipAt(m_playhead);

    const double S = clip ? kfValue(clip->kfScale, clip->scale, m_playhead - clip->pos) : 1.0;
    const double SX = clip ? kfValue(clip->kfScaleX, clip->scaleX, m_playhead - clip->pos) : 1.0;
    const double SY = clip ? kfValue(clip->kfScaleY, clip->scaleY, m_playhead - clip->pos) : 1.0;
    const double rot = clip ? kfValue(clip->kfRotation, clip->rotation, m_playhead - clip->pos) : 0.0;
    const double tx = clip ? kfValue(clip->kfTx, clip->tx, m_playhead - clip->pos) : 0.0;
    const double ty = clip ? kfValue(clip->kfTy, clip->ty, m_playhead - clip->pos) : 0.0;

    // Desenha uma camada com transform (escala, rotação, pan), centrada no
    // ponto (cx, cy) e na escala kk (qualquer buffer/painter). `clipRect` evita
    // que o conteúdo vaze do monitor (rotação/zoom) para a área ao redor.
    const auto drawLayer = [&](QPainter& qp, const QImage& img, double s, double r,
                               double x, double y, double alpha,
                               double sX, double sY, double kk, double cx, double cy,
                               const QRect& clipRect,
                               double ox = 0.0, double oy = 0.0) {
        const double fit = qMin(pw / img.width(), ph / img.height());
        qp.save();
        qp.setClipRect(clipRect);
        qp.translate(cx + x * kk + ox * kk, cy + y * kk + oy * kk);
        qp.rotate(r);
        qp.scale(kk * fit * s * sX, kk * fit * s * sY);
        qp.translate(-img.width() / 2.0, -img.height() / 2.0);
        if (alpha < 1.0) {
            QImage img2 = img;
            QPainter ip(&img2);
            ip.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            ip.fillRect(img2.rect(), QColor(0, 0, 0, (int)(alpha * 255)));
            ip.end();
            qp.drawImage(0, 0, img2);
        } else {
            qp.drawImage(0, 0, img);
        }
        qp.restore();
    };

    // Uma camada de vídeo pronta para desenhar (na ordem de baixo para cima).
    struct Layer {
        QImage frame;
        double s = 1.0, sX = 1.0, sY = 1.0, rot = 0.0, x = 0.0, y = 0.0;
        double alpha = 1.0, ox = 0.0, oy = 0.0;
        QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;
        // Faixa de efeitos (Adjustment Layer): sem frame próprio; aplica os
        // efeitos de `fxClip` ao acumulado NESTA posição z-order.
        bool isAdjustment = false;
        const Clip* fxClip = nullptr;
    };
    QVector<Layer> layers;

    // Monta o empilhamento: todas as faixas de vídeo (a faixa 0 é o TOPO), de
    // baixo para cima. Cada faixa contribui com o clipe ativo no playhead.
    // Clipes de texto independentes entram depois (desenhados por cima).
    {
        const Clip* topClip = clip;
        for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
            const Track& t = m_project->videoTracks[tr];
            if (!t.visible) continue;   // faixa oculta (olho) não participa da composição
            // Faixa de efeitos (Adjustment Layer): não renderiza mídia. Aplica os
            // efeitos do clipe de ajuste ativo ao acumulado NESTA posição z-order.
            if (t.fxTrack) {
                const Clip* fx = nullptr;
                for (const Clip& cl : t.clips) {
                    if (cl.isText) continue;
                    if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur)
                        if (!fx || cl.pos > fx->pos) fx = &cl;
                }
                if (fx) {
                    Layer A;
                    A.isAdjustment = true;
                    A.fxClip = fx;
                    layers.append(A);
                }
                continue;
            }
            const bool isMesaTrack = m_project->findMesaForTrack(t.id) != nullptr;
            const Clip* c = nullptr;
            for (const Clip& cl : t.clips) {
                if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                    const MediaItem* mm = m_project->findMedia(cl.mediaId);
                    if ((isMesaTrack || (mm && mm->hasVideo)) && (!c || cl.pos > c->pos))
                        c = &cl;
                }
            }
            if (!c) continue;
            const QPainter::CompositionMode mode = blendModeToQt(t.blendMode);

            // Quadro deste clipe: vídeo normal
            QImage clipFrame;
            if (topClip && c->id == topClip->id) {
                QMutexLocker l(&m_frameMutex);
                if (!m_frame.isNull()) {
                    clipFrame = m_frame;
                }
            } else if (isMesaTrack) {
                // Track Mesa em layers inferiores: frame já renderizado pelo
                // MesaRenderer e armazenado no layerCache por requestLowerLayers.
                {
                    QMutexLocker l(&m_frameMutex);
                    clipFrame = m_layerCache.value(c->id).img;
                }
                if (clipFrame.isNull()) continue;
            } else {
                const MediaItem* mm = m_project->findMedia(c->mediaId);
                if (!mm || !mm->hasVideo) continue;
                if (mm->isSolid) {
                    // Mídia gerada: gera no tamanho natural, sem passar de uma
                    // vez pela resolução exibida (não borra e não estoura 4K
                    // a cada paint). Preserva o aspecto da mídia.
                    const int mw = mm->width > 0 ? mm->width : (int)pw;
                    const int mh = mm->height > 0 ? mm->height : (int)ph;
                    const double sc = qMin(1.0, qMin((double)canvas.width() / mw,
                                                     (double)canvas.height() / mh));
                    const int w = qMax(1, (int)std::lround(mw * sc));
                    const int h = qMax(1, (int)std::lround(mh * sc));
                    clipFrame = generatorFrame(*mm, w, h);
                    if (c->lainkaEnabled) {
                        const double srcT = clipSrcTime(*c, m_playhead - c->pos);
                        clipFrame = lainkaApplyFx(clipFrame, c->id, srcT,
                                                  c->lainkaSkip, c->lainkaJitterPos,
                                                  c->lainkaJitterRot, c->lainkaJitterScale,
                                                  c->lainkaFlicker, c->lainkaFlickerSpeed,
                                                  c->lainkaWarpAmount, c->lainkaWarpSpeed,
                                                  c->lainkaWarpGrid, c->lainkaOnionSkin,
                                                  c->lainkaDustAmount, c->lainkaScratchAmount,
                                                  c->lainkaMotionBlur, c->lainkaOpacity,
                                                  c->lainkaTargetFps, c->lainkaAntialias,
                                                  QImage());
                    }
                    applyBasicEffectsOn(clipFrame, *c, m_playhead - c->pos);
                } else {
                    {
                        QMutexLocker l(&m_frameMutex);
                        clipFrame = m_layerCache.value(c->id).img;
                    }
                    if (clipFrame.isNull()) continue;
                }
            }

            if (clipFrame.isNull()) continue;

            // Transição (mesma faixa): o clipe de trás por baixo
            if (topClip && c->id == topClip->id && m_transAlpha >= 0.0 && !m_underFrame.isNull()) {
                Layer u;
                u.frame = m_underFrame;
                layers.append(u);
            }

            Layer L;
            L.frame = clipFrame;
            L.s = kfValue(c->kfScale, c->scale, m_playhead - c->pos);
            L.sX = kfValue(c->kfScaleX, c->scaleX, m_playhead - c->pos);
            L.sY = kfValue(c->kfScaleY, c->scaleY, m_playhead - c->pos);
            L.rot = kfValue(c->kfRotation, c->rotation, m_playhead - c->pos);
            L.x = kfValue(c->kfTx, c->tx, m_playhead - c->pos);
            L.y = kfValue(c->kfTy, c->ty, m_playhead - c->pos);
            const double rel = m_playhead - c->pos;
            double alpha = std::clamp(kfValue(c->kfOpacity, c->opacity, rel), 0.0, 1.0);
            if (c->fadeIn > 1e-6) alpha *= std::min(1.0, rel / c->fadeIn);
            if (c->fadeOut > 1e-6) alpha *= std::min(1.0, (c->dur - rel) / c->fadeOut);
            if (topClip && c->id == topClip->id && m_transAlpha >= 0.0) {
                if (m_transType == QStringLiteral("dissolve")) alpha *= m_transAlpha;
                else if (m_transType == QStringLiteral("wipeleft")) L.ox = pw * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wiperight")) L.ox = -pw * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wipeup")) L.oy = ph * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wipedown")) L.oy = -ph * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wipetl")) { L.ox = pw * (1.0 - m_transAlpha); L.oy = ph * (1.0 - m_transAlpha); }
                else if (m_transType == QStringLiteral("wipetr")) { L.ox = -pw * (1.0 - m_transAlpha); L.oy = ph * (1.0 - m_transAlpha); }
                else if (m_transType == QStringLiteral("wipebr")) { L.ox = -pw * (1.0 - m_transAlpha); L.oy = -ph * (1.0 - m_transAlpha); }
                else if (m_transType == QStringLiteral("wipebl")) { L.ox = pw * (1.0 - m_transAlpha); L.oy = -ph * (1.0 - m_transAlpha); }
            }
            L.alpha = alpha;
            L.alpha *= std::clamp(t.opacity, 0.0, 1.0);
            L.mode = mode;
            layers.append(L);
            if (composeDbg())
                qDebug() << "[compose] tr=" << tr
                         << "clip=" << (c->name.isEmpty() ? c->id : c->name)
                         << "top=" << (topClip && c->id == topClip->id);
        }
    }

    if (composeDbg()) {
        for (const Layer& L : layers) {
            int cornerA = -1;
            if (!L.frame.isNull() && L.frame.width() > 2 && L.frame.height() > 2)
                cornerA = L.frame.pixelColor(1, 1).alpha();
            qDebug() << "  [compose] layer " << L.frame.width() << "x" << L.frame.height()
                     << "alpha=" << L.alpha << "cornerA=" << cornerA << "mode=" << (int)L.mode;
        }
        qDebug() << "  [compose] total=" << layers.size() << "m_frameNull=" << m_frame.isNull();
    }

    if (m_frame.isNull() && layers.isEmpty() && !anyText) {
        drawEmptyMonitor(p, canvas);
        return;
    }

    // Empilhamento multi-faixa: compõe num buffer do tamanho do canvas, faixa
    // a faixa com o modo de composição da faixa (igual ao blend da exportação),
    // de baixo para cima — é o que permite ver transparência (PNG/WebP com
    // alpha) e as camadas que ficam POR BAIXO do clipe do topo.
    // O fundo é PRETO opaco (como o ffmpeg): os modos de blend (multiply,
    // screen…) produzem o mesmo resultado da exportação, e não dependem do
    // fundo do monitor.
    if (layers.size() >= 2 || (m_frame.isNull() && !layers.isEmpty())) {
        // Cache: reutiliza se playhead e camadas não mudaram.
        if (!m_compositedCache.isNull() && m_compositedEpoch == m_currentFrameIndex
            && m_compositedCache.size() == canvas.size()
            && m_compositedLayerCount == layers.size()) {
            if (PreviewProfiler::active()) {
                profFrame().compositeEvaluated = true;
                profFrame().compositeHit = true;
            }
            p.drawImage(canvas.topLeft(), m_compositedCache);
            drawMaskOverlay(p, canvas, k);
            return;
        }
        if (PreviewProfiler::active()) {
            profFrame().compositeEvaluated = true;
            profFrame().compositeHit = false;
            profFrame().layers = layers.size();
        }
        PreviewProfiler::Scope compScope(PreviewProfiler::active() ? &profFrame().compositeNs : nullptr);
        QImage acc(canvas.size(), QImage::Format_ARGB32);
        acc.fill(Qt::black);
        const double cx = acc.width() / 2.0;
        const double cy = acc.height() / 2.0;
        for (const Layer& L : layers) {
            // Faixa de efeitos (Adjustment Layer): aplica os efeitos do clipe de
            // ajuste ao ACUMULADO atual (tudo que está abaixo, nesta posição
            // z-order). Não pode haver painter ativo em `acc` — por isso o draw
            // de cada camada usa um painter próprio (escopado).
            if (L.isAdjustment) {
                if (L.fxClip)
                    applyBasicEffectsOn(acc, *L.fxClip, m_playhead - L.fxClip->pos);
                continue;
            }
            if (L.frame.isNull()) continue;
            QPainter ap(&acc);
            ap.setClipRect(QRect(0, 0, acc.width(), acc.height()));
            ap.setCompositionMode(L.mode);
            drawLayer(ap, L.frame, L.s, L.rot, L.x, L.y, L.alpha, L.sX, L.sY, k, cx, cy,
                      QRect(0, 0, acc.width(), acc.height()), L.ox, L.oy);
        }
        // Texto sempre em SourceOver (o modo de composição da última camada
        // não pode vazar para o texto).
        QPainter ap(&acc);
        ap.setClipRect(QRect(0, 0, acc.width(), acc.height()));
        ap.setCompositionMode(QPainter::CompositionMode_SourceOver);
        // Texto (independente e anexado) por cima das camadas.
        if (m_project) {
            for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
                if (!m_project->videoTracks[tr].visible) continue;   // faixa oculta (olho)
                const Clip* tclip = nullptr;
                for (const Clip& c : m_project->videoTracks[tr].clips)
                    if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur)
                        if (!tclip || c.pos > tclip->pos) tclip = &c;
                if (tclip) drawClipText(ap, acc.rect(), tclip, k);
            }
        }
        if (clip && !clip->isText)
            drawClipText(ap, acc.rect(), clip, k);
        ap.end();
        m_compositedCache = acc;
        m_compositedEpoch = m_currentFrameIndex;
        m_compositedLayerCount = layers.size();
        p.drawImage(canvas.topLeft(), acc);
        drawMaskOverlay(p, canvas, k);
        return;
    }

    // Caminho tradicional (um clipe só, com ou sem transição): desenha direto.
    // Opacidade do clipe + fades de entrada/saída + opacidade da faixa
    // aplicados no alpha (como na exportação), para o fade aparecer mesmo com
    // um clipe sozinho.
    const double clipRel = clip ? (m_playhead - clip->pos) : 0.0;
    double clipAlpha = 1.0;
    double clipTrackOpacity = 1.0;
    if (clip && m_project) {
        clipAlpha = std::clamp(kfValue(clip->kfOpacity, clip->opacity, clipRel), 0.0, 1.0);
        if (clip->fadeIn > 1e-6) clipAlpha *= std::min(1.0, clipRel / clip->fadeIn);
        if (clip->fadeOut > 1e-6) clipAlpha *= std::min(1.0, (clip->dur - clipRel) / clip->fadeOut);
        for (const Track& tr : m_project->videoTracks)
            for (const Clip& c : tr.clips)
                if (c.id == clip->id) { clipTrackOpacity = std::clamp(tr.opacity, 0.0, 1.0); break; }
    }
    clipAlpha *= clipTrackOpacity;
    // Para clipes Mesa, usa o quadro renderizado em vez de m_frame
    const QImage& singleFrame = m_frame;
    const bool transActive = m_transAlpha >= 0.0
                             && !singleFrame.isNull() && !m_underFrame.isNull();
    if (!singleFrame.isNull()) {
        if (transActive) {
            drawLayer(p, m_underFrame, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, k,
                      canvas.center().x(), canvas.center().y(), canvas);
            double ox = 0.0, oy = 0.0;
            if (m_transType == QStringLiteral("wipeleft")) ox = pw * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wiperight")) ox = -pw * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipeup")) oy = ph * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipedown")) oy = -ph * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipetl")) { ox = pw * (1.0 - m_transAlpha); oy = ph * (1.0 - m_transAlpha); }
            else if (m_transType == QStringLiteral("wipetr")) { ox = -pw * (1.0 - m_transAlpha); oy = ph * (1.0 - m_transAlpha); }
            else if (m_transType == QStringLiteral("wipebr")) { ox = -pw * (1.0 - m_transAlpha); oy = -ph * (1.0 - m_transAlpha); }
            else if (m_transType == QStringLiteral("wipebl")) { ox = pw * (1.0 - m_transAlpha); oy = -ph * (1.0 - m_transAlpha); }
            double a = clipAlpha;
            if (m_transType == QStringLiteral("dissolve")) a *= m_transAlpha;
            drawLayer(p, singleFrame, S, rot, tx, ty, a, SX, SY, k,
                      canvas.center().x(), canvas.center().y(), canvas, ox, oy);
        } else {
            drawLayer(p, singleFrame, S, rot, tx, ty, clipAlpha, SX, SY, k,
                      canvas.center().x(), canvas.center().y(), canvas);
        }
    }

    // Clipes de texto independentes: desenha todos os ativos no playhead, da
    // faixa de baixo para a de cima (a faixa 0 é o topo e fica por cima).
    if (m_project) {
        for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
            const Track& tv = m_project->videoTracks[tr];
            if (!tv.visible) continue;   // faixa oculta (olho)
            const Clip* tclip = nullptr;
            for (const Clip& c : tv.clips)
                if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur)
                    if (!tclip || c.pos > tclip->pos) tclip = &c;
            if (tclip) drawClipText(p, canvas, tclip, k);
        }
    }
    // Texto anexado a um clipe de vídeo (comportamento antigo) ainda vale.
    if (clip && !clip->isText)
        drawClipText(p, canvas, clip, k);

    // Grade de referência visual (estilo Vegas), sobre todo o conteúdo.
    drawGrid(p, canvas);

    // Overlay de edição de máscara (formas + alças do clipe sob edição).
    drawMaskOverlay(p, canvas, k);
}

void PreviewWidget::drawPerfOverlay(QPainter& p) {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_PERF_DEBUG");
    if (!on) return;
    // Estágios do tick corrente (ms). O registro vive no PlaybackEngine e é
    // preenchido pelos dois lados — o widget escreve as fases assíncronas
    // (worker, prefetch, paint) no mesmo registro em que o tick está.
    const PreviewProfiler::Frame& fr = profFrame();
    const PreviewProfiler& prof = PreviewProfiler::instance();
    const auto ms = [](double ns) { return ns / 1e6; };

    p.save();
    p.resetTransform();
    const QRect box(8, 8, 392, 190);
    p.fillRect(box, QColor(0, 0, 0, 190));
    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255));
    const QString txt = QStringLiteral(
        "fonte: %1   quadros: %2   p95 tick %3 ms  p95 decode %4 ms\n"
        "tick %5 ms = clock %6 + seek %7 + prefetch %8 + mix %9\n"
        "decode(worker->frame) %10 ms   prefetch->ready %11 ms\n"
        "paint %12 ms (comp %13)   resolve proxy %14 ms\n"
        "prefetch %15   fila %16   camadas %17   cortes %18\n"
        "dropped %19   skip %20   adaptive %21\n"
        "abre %22 fonte(s)   open total %23 ms   ultima %24 ms   pool %25 quentes")
        .arg(prof.wroteProxy() ? QStringLiteral("PROXY")
                               : QStringLiteral("original"))
        .arg(prof.frameCount())
        .arg(ms(prof.stats(QStringLiteral("tickNs")).p95), 0, 'f', 1)
        .arg(ms(prof.stats(QStringLiteral("workerNs")).p95), 0, 'f', 1)
        .arg(ms(fr.tickNs), 0, 'f', 1)
        .arg(ms(fr.clockNs), 0, 'f', 1)
        .arg(ms(fr.seekNs), 0, 'f', 1)
        .arg(ms(fr.prefetchNs), 0, 'f', 1)
        .arg(ms(fr.mixNs), 0, 'f', 1)
        .arg(ms(fr.workerNs), 0, 'f', 1)
        .arg(ms(fr.prefetchLatNs), 0, 'f', 1)
        .arg(ms(fr.paintNs), 0, 'f', 1)
        .arg(ms(fr.compositeNs), 0, 'f', 1)
        .arg(ms(fr.resolveNs), 0, 'f', 1)
        .arg(fr.prefetchHit ? QStringLiteral("HIT") : QStringLiteral("miss"))
        .arg(fr.queue)
        .arg(fr.layers)
        .arg(prof.cuts())
        .arg(m_perf.droppedTotal)
        .arg(fr.skipped)
        .arg(m_adaptiveActive
             ? QStringLiteral("ON (%1p)").arg(m_previewQuality)
             : QStringLiteral("OFF"))
        .arg(FFmpegDecoder::openCount())
        .arg(ms(FFmpegDecoder::openTotalNs()), 0, 'f', 1)
        .arg(ms(FFmpegDecoder::lastOpenNs()), 0, 'f', 1)
        .arg(m_decPool ? m_decPool->count() : 0);
    p.drawText(box.adjusted(10, 8, -6, -6), Qt::AlignLeft | Qt::AlignTop, txt);
    p.restore();
}

// Desenha o texto/título estilizado do clipe sobre o monitor (mesmo resultado
// visual do drawtext da exportação). O desenho acontece em espaço de PROJETO
// mapeado para o canvas (escala k), aplicando o transform animável do clipe
// (escala, rotação, pan) e a opacidade (fades/keyframes).
void PreviewWidget::drawClipText(QPainter& p, const QRect& canvas, const Clip* clip, double k) {
    if (!clip || !m_project) return;
    const TextStyle& st = *m_project->textStyleFor(*clip);
    if (st.isEmpty()) return;
    const double W = m_project->width;
    const double H = m_project->height;
    const double rel = m_playhead - clip->pos;
    double alpha = std::clamp(kfValue(clip->kfOpacity, clip->opacity, rel), 0.0, 1.0);
    if (clip->fadeIn > 1e-6) alpha *= std::min(1.0, rel / clip->fadeIn);
    if (clip->fadeOut > 1e-6) alpha *= std::min(1.0, (clip->dur - rel) / clip->fadeOut);
    if (alpha <= 0.0) return;

    // LAINKA no texto: jitter de posição, rotação, escala e flicker.
    double jtX = 0.0, jtY = 0.0, jtRot = 0.0, jtScale = 1.0;
    if (clip->lainkaEnabled) {
        const uint h = lainkaHash(clip->id, m_playhead);
        if (clip->lainkaJitterPos > 1e-6) {
            const double nx = ((h & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            const double ny = (((h >> 16) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            const double maxPx = clip->lainkaJitterPos * 0.5; // escala para texto
            jtX = nx * maxPx;
            jtY = ny * maxPx;
        }
        if (clip->lainkaJitterRot > 1e-6) {
            const double n = ((lainkaHashN(clip->id, m_playhead, 11) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            jtRot = n * clip->lainkaJitterRot * 0.3;
        }
        if (clip->lainkaJitterScale > 1e-6) {
            const double n = ((lainkaHashN(clip->id, m_playhead, 12) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            jtScale = 1.0 + n * clip->lainkaJitterScale * 0.01;
        }
        if (clip->lainkaFlicker > 1e-6) {
            const double n = ((lainkaHashN(clip->id, m_playhead * (clip->lainkaFlickerSpeed / 50.0), 13) & 0xFF) / 255.0);
            alpha *= (1.0 + (n * 2.0 - 1.0) * (clip->lainkaFlicker / 100.0));
            alpha = std::clamp(alpha, 0.0, 1.0);
        }
    }

    const double sizeFrac = st.textSize > 0.0 ? st.textSize : (1.0 / 18.0);
    const int pxSize = qMax(4, (int)qRound(sizeFrac * H)); // px de projeto
    QFont font;
    if (!st.fontFamily.isEmpty()) font.setFamily(st.fontFamily);
    font.setPixelSize(pxSize);
    font.setBold(st.textBold);
    const QFontMetricsF fm(font);

    // Quebra em linhas dentro de 90% da largura do projeto.
    const double maxW = W * 0.9;
    QStringList wrapped;
    for (const QString& raw : st.text.split(QLatin1Char('\n'))) {
        if (raw.isEmpty()) { wrapped << QString(); continue; }
        QString cur;
        const QStringList words = raw.split(QLatin1Char(' '));
        for (const QString& w : words) {
            const QString trial = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
            if (fm.horizontalAdvance(trial) <= maxW || cur.isEmpty())
                cur = trial;
            else { wrapped << cur; cur = w; }
        }
        wrapped << cur;
    }

    double tw = 0.0;
    for (const QString& l : wrapped) tw = qMax(tw, fm.horizontalAdvance(l));
    const double th = wrapped.size() * fm.height();
    const double pad = pxSize * 0.25;
    const double bw = tw + 2.0 * pad;
    const double bh = th + 2.0 * pad;

    // Posição do texto RELATIVA ao centro do quadro (a origem do transform é
    // o centro + pan). textX=0.5 → x=0 → texto centralizado. Antes desenhávamos
    // em coordenadas absolutas (0.5W) E transladávamos por W/2: o texto caía
    // em 0.5W+0.5W = W (canto inferior direito, fora da tela).
    double x;
    if (st.textAlign == 1) x = st.textX * W - W / 2.0;
    else if (st.textAlign == 2) x = st.textX * W - bw - W / 2.0;
    else x = st.textX * W - bw / 2.0 - W / 2.0;
    const double y = st.textY * H - bh / 2.0 - H / 2.0;
    const QRectF box(x, y, bw, bh);

    p.save();
    p.setClipRect(canvas);
    p.setOpacity(alpha);
    // Mapeia espaço de projeto (0..W x 0..H) para o canvas.
    p.translate(canvas.topLeft());
    p.scale(k, k);
    // Transform animável do clipe, em torno do centro do quadro.
    p.translate(W / 2.0 + kfValue(clip->kfTx, clip->tx, rel) + jtX,
                H / 2.0 + kfValue(clip->kfTy, clip->ty, rel) + jtY);
    p.rotate(kfValue(clip->kfRotation, clip->rotation, rel) + jtRot);
    p.scale(kfValue(clip->kfScale, clip->scale, rel) * jtScale
                * kfValue(clip->kfScaleX, clip->scaleX, rel),
            kfValue(clip->kfScale, clip->scale, rel) * jtScale
                * kfValue(clip->kfScaleY, clip->scaleY, rel));

    if (st.textBackground) {
        p.fillRect(box, st.textBackgroundColor);
    }

    QPainterPath path;
    const double baseline = box.top() + pad + fm.ascent();
    for (int i = 0; i < wrapped.size(); ++i)
        path.addText(QPointF(box.left() + pad, baseline + i * fm.height()), font, wrapped[i]);

    if (st.textOutline > 0.0) {
        const double ow = qMax(1.0, st.textOutline * H);
        QPen pen(st.textOutlineColor, ow, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(path, pen);
    }
    p.fillPath(path, st.textColor);

    p.restore();
}

// Tela vazia: o Premiere não escreve nada no miolo do Program Monitor quando
// não há sequência — só o painel escuro. Mantemos uma dica discreta, mas sem
// moldura nem caixa, só o texto centralizado em cinza apagado.
void PreviewWidget::drawEmptyMonitor(QPainter& p, const QRect& canvas) {
    if (canvas.isEmpty()) return;
    QFont f = p.font();
    f.setPointSizeF(9.0);
    p.setFont(f);
    p.setPen(QColor(110, 110, 120));
    p.drawText(canvas, Qt::AlignCenter, tr("Sem clipe de vídeo aqui"));
}

void PreviewWidget::seek(double t) {
    PlaybackEngine::seek(t);
}

// Quadro a quadro (Premiere: os botões ◀▶ do monitor). Pausa a reprodução
// (senão o seek brigaria com o relógio do playback) e busca ±1 quadro.
void PreviewWidget::stepFrame(int dir) {
    if (!m_project || m_project->duration() <= 0) return;
    if (m_playing) togglePlay();
    const double fps = projFps(m_project);
    const double t = m_playhead + (dir > 0 ? 1.0 : -1.0) / fps;
    seek(std::clamp(t, 0.0, m_project->duration()));
}

// Slot público do passo a passo, para os botões ◀▶ da janela de preview externo.
void PreviewWidget::stepFrameBy(int dir) {
    stepFrame(dir);
}

void PreviewWidget::onSeek(double t) {
    applySeekVisual(t);
}

// Intervalo do "desengasgo" (flush periódico dos buffers do decoder).
// Padrão 0 = desligado. PIERROT_DESENGASGA_MS=<ms> religa.
//
// Medido numa fonte 1080p60 (H.264 High@4.2, 252 s, 13 Mbps), 3 runs de 252 s:
// com o flush a cada 30 s aparecem 7 travamentos de decode de 114-338 ms,
// exatamente a cada 30 s (26/56/86/116/146/176/206 s); desligado, zero picos
// e p99 de decode idêntico (0,55 ms). A degradação de buffer que o flush existia
// para evitar não se confirma: com o flush desligado o p95 de decode ficou plano
// ao longo dos 4 minutos (0,35 -> 0,37 ms). Baixar de 10s para 30s só dilatou o
// custo, não o reduziu, porque o custo é o re-seek frio que o flush provoca —
// não a frequência. Fica desligado por padrão; se o engasgo reaparecer em
// alguma fonte específica, religue por env var em vez de reintroduzir o padrão.
static qint64 desengasgaIntervalMs() {
    static const qint64 v = [] {
        bool ok = false;
        const qint64 e = qEnvironmentVariableIntValue("PIERROT_DESENGASGA_MS", &ok);
        return ok && e >= 0 ? e : qint64(0);
    }();
    return v;
}

// Decodifica e desenha o quadro na posição `t` (o transporte/relógio é do
// PlaybackEngine; aqui só a parte visual/decodificação).
void PreviewWidget::applySeekVisual(double t) {
    Q_UNUSED(t);
    if (m_timeLabel) m_timeLabel->setText(fmtTimecode(m_playhead, projFps(m_project)));

    // Desengasgo periódico: a cada ~10s durante playback contínuo, libera os
    // buffers internos do decoder (DPB + caches de 2 frames) que degradam o
    // frameAt em vídeos longos — a causa do vídeo engasgar com áudio perfeito.
    // O flush é barato (releaseBuffers não fecha o arquivo; o próximo pedido
    // faz re-seek) e roda no worker thread, seriado com os decodeOne.
    if (m_playing && m_frameWorker && desengasgaIntervalMs() > 0) {
        const qint64 nowMs = m_desengasgaT.isValid()
                                 ? m_desengasgaT.elapsed() : 0;
        if (!m_desengasgaT.isValid()) {
            m_desengasgaT.start();
            m_desengasgaLastMs = 0;
        } else if (nowMs - m_desengasgaLastMs >= desengasgaIntervalMs()) {
            // Medido numa fonte 1080p60 (H.264 High@4.2, 252 s, 13 Mbps):
            // cada flush custa um re-seek frio de 114-304 ms, ou seja, 4
            // travamentos de ~0,25 s em 120 s de reprodução — exatamente a cada
            // 30 s. Baixar de 10s para 30s só dilatou o custo, não o reduziu.
            // E o problema que ele existia para evitar não aparece: com o flush
            // desligado o p95 de decode ficou plano (0,35-0,37 ms) nos 4
            // minutos, sem degradação. Por isso o intervalo virou configurável
            // (PIERROT_DESENGASGA_MS=0) em vez de fixo — desligar é o padrão
            // melhor medido, mas se algum dia o engasgo reaparecer em outra
            // fonte, basta ligar de volta por env var.
            m_desengasgaLastMs = nowMs;
            QMetaObject::invokeMethod(m_frameWorker, "desengasga", Qt::QueuedConnection);
        }
    }
    updateFrame();
    update();
}

double PreviewWidget::audioClockSec() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (m_audioSink) return m_audioSink->processedUSecs() / 1.0e6;
#else
    if (m_audioOut) return m_audioOut->processedUSecs() / 1.0e6;
#endif
    return -1.0;
}

void PreviewWidget::togglePlay() {
    PlaybackEngine::togglePlay();
}

void PreviewWidget::shuttle(int dir) {
    PlaybackEngine::shuttle(dir);
}

void PreviewWidget::playFrom(double t) {
    PlaybackEngine::playFrom(t);
}

void PreviewWidget::setLoopRange(double in, double out) {
    PlaybackEngine::setLoopRange(in, out);
}

void PreviewWidget::setLoopEnabled(bool enabled) {
    PlaybackEngine::setLoopEnabled(enabled);
}

// Hooks do PlaybackEngine: o transporte decide quando; aqui a PreviewWidget
// faz o áudio e o rendering.

void PreviewWidget::onStartAudio(double t) { startAudio(t); }
void PreviewWidget::onStopAudio() { stopAudio(); }
void PreviewWidget::onMixAudio(double t, bool reseek) { updateMixAudio(t, reseek); }
void PreviewWidget::onPrefetch() { updatePrefetch(); }

// Limpa buffers de frame quando o transporte para (sem duplicar o estado do
// PlaybackEngine, que já zera o relógio).
void PreviewWidget::onStopPlaybackUI() {
    QMutexLocker l(&m_frameMutex);
    m_prefetch = PrefetchFrame();
    m_warmQueue.clear();
    m_warmInFlight.clear();
}

void PreviewWidget::onPlayheadMoved(double t) {
    emit playheadMoved(t);
}

void PreviewWidget::onStateChanged(bool playing) {
    // O PlaybackEngine troca o rótulo do botão a cada transição; como ele está
    // em modo ícone (estilo Premiere), restauramos o ícone e limpamos o texto.
    if (m_playBtn) {
        m_playBtn->setIcon(playing ? m_pauseIcon : m_playIcon);
        m_playBtn->setText(QString());
    }
    if (m_timeLabel) m_timeLabel->setText(fmtTimecode(m_playhead, projFps(m_project)));
    emit stateChanged(playing);
}

void PreviewWidget::setZoom(double z) {
    m_zoom = z;
    if (m_zoomCombo) {
        static const double kZooms[] = {0.0, 0.25, 0.50, 0.75, 1.0, 1.5, 2.0};
        int idx = 0;
        for (int i = 0; i < 7; ++i) {
            if (std::fabs(kZooms[i] - z) < 1e-6) { idx = i; break; }
        }
        QSignalBlocker b(m_zoomCombo);
        m_zoomCombo->setCurrentIndex(idx);
    }
    update();
}

void PreviewWidget::setPreviewQuality(int width) {
    m_previewQuality = width;
    // Se o usuário mudou manualmente, reseta o estado adaptativo.
    m_adaptiveSlowCount = 0;
    m_adaptiveActive = false;
    m_adaptiveDecodeMsAvg = 0.0;
    // Invalida o quadro atual e força a reconversão na nova resolução.
    {
        QMutexLocker l(&m_frameMutex);
        if (!m_frame.isNull()) m_frame = QImage();
    }
    m_shownW = -1;
    m_lastDecodeW = -1;
    for (QAction* a : m_qualityMenu->actions())
        a->setChecked(a->data().toInt() == width);
    for (const PreviewQOpt& o : kPreviewQualities)
        if (o.width == width) {
            if (m_qualityBtn) m_qualityBtn->setText(QString::fromUtf8(o.code));
            break;
        }
    update();
    updateFrame();
}


// Clipe de vídeo (com mídia) no topo em `t`. Clipes de texto independentes são
// ignorados aqui (não têm quadro para decodificar); o texto é desenhado por
// cima do vídeo no paintEvent.
const Clip* PreviewWidget::clipAt(double t) const {
    if (!m_project) return nullptr;
    // Empilhamento: a faixa de índice 0 é a de TOPO (como V1 no Premiere), e
    // o paintEvent compõe de baixo para cima percorrendo size()-1 → 0. Por isso
    // o PRIMEIRO match vence: ele é o clipe visualmente mais alto.
    //
    // Já houve aqui um `return` no último match, achando que índice maior era
    // o topo. É o contrário — e o efeito era o Preview escolher o clipe do
    // rodapé. Não voltar a isso: para trocar a ordem de composição, o lugar
    // é o paintEvent, não o clipAt.
    for (int tr = 0; tr < (int)m_project->videoTracks.size(); ++tr) {
        const Track& track = m_project->videoTracks[tr];
        if (!track.visible) continue;   // faixa oculta (olho)
        if (track.fxTrack) continue;    // faixa de efeitos não é clipe "topo"
        // Track de Mesa gera quadro mesmo sem mídia própria (a composição é a
        // fonte de vídeo).
        const bool mesaTrack = m_project->findMesaForTrack(track.id) != nullptr;
        const Clip* best = nullptr;
        for (const Clip& c : track.clips) {
            if (t >= c.pos && t < c.pos + c.dur && !c.isText) {
                const MediaItem* m = m_project->findMedia(c.mediaIdAt(t - c.pos));
                const bool hasVideo = mesaTrack || (m && m->hasVideo);
                if (hasVideo && (!best || c.pos > best->pos)) best = &c;
            }
        }
        if (best) return best;
    }
    return nullptr;
}

bool PreviewWidget::tryRenderMesa(const Clip* clip) {
    if (!m_project || !clip) return false;

    // Encontra a faixa do clipe ativo.
    const Track* track = nullptr;
    for (const Track& tr : m_project->videoTracks) {
        for (const Clip& c : tr.clips)
            if (c.id == clip->id) { track = &tr; break; }
    }
    if (!track) return false;

    // A Mesa dona da track é descoberta via mesa.trackIds (fonte da verdade,
    // a mesma usada pelo MesaWidget/MesaRenderer). O caminho antigo
    // track.groupId→group.mesaId quebrava quando a pasta da Mesa era
    // excluída/desagrupada: a composição sumia do preview ao reabrir.
    const MesaComposition* mc = m_project->findMesaForTrack(track->id);
    if (!mc) return false;

    // Renderiza o canvas + transform de câmera (saída no tamanho do projeto).
    const QImage out = m_mesaRenderer.render(*mc, *m_project, m_playhead);
    if (out.isNull()) return false;

    {
        QMutexLocker l(&m_frameMutex);
        m_frameFull = out;
        m_shownPath = QStringLiteral("mesa:") + mc->id;
        m_shownT = m_playhead;
        m_shownW = m_project->width;
        m_lastSrcT = m_playhead;
        m_lastDecodeW = m_project->width;
        m_lastFile.clear();
        m_prefetch.valid = false;
        m_prefetch.requested = false;
    }
    // Nenhum efeito de clipe deve ser aplicado à composição da Mesa.
    m_clipLainkaEnabled = false;
    m_clipMotionEnabled = false;
    m_clipOfxFx.clear();
    m_clipMasks.clear();
    applyCrop();
    return true;
}

namespace {
// Ganho efetivo de um clipe de áudio em `t` (tempo de timeline): volume do
// clipe × envelope do clipe × envelope/volume da faixa × fades × crossfade de
// transição com vizinho da mesma faixa, clampado 0..2. Fonte única para as
// fontes ATIVAS (buildMixSources) e para as pré-aquecidas (buildWarmSources):
// as warm precisam do MESMO ganho da costura, senão o clipe que entra num
// corte toca com o volume cru do clipe e perde o fader da faixa → "mergulho"
// curto no corte até o próximo tick recalcular.
double previewClipAudioVol(const Track& tr, const Clip& c, double t) {
    const double rel = t - c.pos;
    double vol = c.volume * kfValue(c.kfVolume, 1.0, rel)
                 * kfValue(tr.kfVolume, tr.volume, t);
    if (c.fadeIn > 1e-6) vol *= std::min(1.0, rel / c.fadeIn);
    if (c.fadeOut > 1e-6) vol *= std::min(1.0, (c.dur - rel) / c.fadeOut);
    // Crossfade de transição: sobreposição com vizinho da MESMA faixa vira
    // fade-in (da frente) / fade-out (de trás).
    double transIn = 0.0, transOut = 0.0;
    for (const Clip& o : tr.clips) {
        if (o.id == c.id) continue;
        if (o.pos < c.pos - 1e-6 && o.pos + o.dur > c.pos + 1e-6)
            transIn = std::max(transIn, o.pos + o.dur - c.pos);
        if (o.pos > c.pos + 1e-6 && o.pos < c.pos + c.dur - 1e-6)
            transOut = std::max(transOut, c.pos + c.dur - o.pos);
    }
    if (transIn > 1e-6) vol *= std::clamp(rel / transIn, 0.0, 1.0);
    if (transOut > 1e-6) vol *= std::clamp((c.dur - rel) / transOut, 0.0, 1.0);
    return std::clamp(vol, 0.0, 2.0);
}
} // namespace

// Clip de áudio "ativo": o mais acima (vídeo ou áudio) em `t` cujo media tem áudio.
// Clipes de áudio ativos em `t`, prontos para o mixer. Dedupe de vídeo+áudio
// vinculados (mesmo groupId): a faixa de áudio vence, senão o som sobraria
// duas vezes. Respeita mute/solo das faixas, volume de faixa, volume/envelope
// do clipe e fades.
QVector<AudioMixer::SourceInfo> buildMixSources(const Project* p, double t) {
    QVector<AudioMixer::SourceInfo> out;
    if (!p) return out;
    bool anySolo = false;
    // Só faixas de ÁUDIO têm solo com efeito no mix: faixas de vídeo nunca
    // são fonte de áudio neste app (o áudio de um vídeo vive na faixa de áudio
    // pareada), então solo delas não deve silenciar o mix.
    for (const Track& tr : p->audioTracks)
        if (tr.solo) { anySolo = true; break; }

    struct Rep { const Clip* clip; double vol; double pan; int trackIdx; bool isAudio; };
    QHash<QString, Rep> reps;
    // Só faixas de ÁUDIO são fonte de áudio. O áudio de um vídeo vive na faixa
    // de áudio pareada (criada no import via groupId); o clipe de vídeo nunca
    // toca sozinho. Assim, apagar o clipe de áudio silencia o som (em vez de o
    // clipe de vídeo continuar tocando por não ser mais sobrescrito), e faixas
    // de vídeo não ganham strip/fader/VU no mixer.
    auto collect = [&](const QVector<Track>& tracks, bool isAudio) {
        for (int ti = 0; ti < tracks.size(); ++ti) {
            const Track& tr = tracks[ti];
            if (!tr.visible) continue;
            for (const Clip& c : tr.clips) {
                if (!(t >= c.pos && t < c.pos + c.dur)) continue;
                if (tr.muted || (anySolo && !tr.solo)) {
                    if (audioDbg())
                        qDebug().noquote() << QStringLiteral("[audio] MUTE skip t=%1 faixa='%2 base=%3")
                              .arg(t, 0, 'f', 3).arg(tr.name)
                              .arg(c.groupId.isEmpty() ? c.id : c.groupId);
                    continue;
                }
                const MediaItem* m = p->findMedia(c.mediaIdAt(t - c.pos));
                if (!m || !m->hasAudio) continue;
                const double vol = previewClipAudioVol(tr, c, t);
                // O STREAM diferencia as faixas de um arquivo multicanal (cada
                // clipe de áudio usa o seu stream); groupId não é mais preciso
                // aqui, pois só a faixa de áudio contribui.
                const QString key = QStringLiteral("%1|%2").arg(c.id).arg(c.audioStreamIndex);
                reps.insert(key, {&c, vol, kfValue(tr.kfPan, tr.pan, t), ti, isAudio});
            }
        }
    };
    collect(p->audioTracks, true);

    for (auto it = reps.cbegin(); it != reps.cend(); ++it) {
        const Clip* c = it.value().clip;
        const MediaItem* m = p->findMedia(c->mediaIdAt(t - c->pos));
        if (!m || !m->hasAudio) continue;
        if (audioDbg())
            qDebug().noquote() << QStringLiteral("[audio] ativo t=%1 faixa='%2' isAudio=%3 base=%4 vol=%5")
                  .arg(t, 0, 'f', 3)
                  .arg((it.value().isAudio
                            ? p->audioTracks[it.value().trackIdx].name
                            : p->videoTracks[it.value().trackIdx].name))
                  .arg(it.value().isAudio)
                  .arg(!c->groupId.isEmpty() ? c->groupId : c->id)
                  .arg(it.value().vol, 0, 'f', 3);
        AudioMixer::SourceInfo si;
        si.key = c->id;
        si.path = m->filePath;
        si.audioStream = c->audioStreamIndex;
        si.mediaPos = c->in + (t - c->pos) * c->speed;
        si.speed = c->speed;
        si.clipPos = c->pos;
        si.clipDur = c->dur;
        si.mediaStart = c->in;
        si.vol = it.value().vol;
        si.eqLow = c->eqLow;
        si.eqMid = c->eqMid;
        si.eqHigh = c->eqHigh;
        si.denoise = c->denoise;
        si.denoiseAmount = c->denoiseAmount;
        si.normalize = c->normalize;
        si.invertPhase = c->invertPhase;
        si.reverb = c->reverb;
        si.reverbMix = c->reverbMix;
        si.reverbSize = c->reverbSize;
        si.trackIndex = it.value().trackIdx;
        si.isAudioTrack = it.value().isAudio;
        si.pan = it.value().pan;
        if (it.value().isAudio) {
            const int ti = it.value().trackIdx;
            if (ti >= 0 && ti < (int)p->audioTracks.size()) {
                const Track& tr = p->audioTracks[ti];
                si.trackFxOn = tr.hasAudioFx();
                si.trackFxEqLow = tr.eqLow;
                si.trackFxEqMid = tr.eqMid;
                si.trackFxEqHigh = tr.eqHigh;
                si.trackFxDenoise = tr.denoise;
                si.trackFxDenoiseAmount = tr.denoiseAmount;
                si.trackFxInvertPhase = tr.invertPhase;
                si.trackFxReverb = tr.reverb;
                si.trackFxReverbMix = tr.reverbMix;
                si.trackFxReverbSize = tr.reverbSize;
            }
        }
        out.append(si);
    }
    return out;
}

// Fontes "pré-aquecidas": clipes de áudio que começam dentro da janela de
// lookahead à frente do playhead. Entram no mix JÁ ATIVAS, mas contribuindo
// zero até o seu frame de saída de início (gating por [srcOutStart, srcOutEnd)).
// Assim, o clipe que começa num corte já está no mix na "chunk" que atravessa
// a costura → a troca esquerda→direita é amostra-exata, sem lacuna nem eco.
QVector<AudioMixer::SourceInfo> buildWarmSources(
    const Project* p, double t, const QVector<AudioMixer::SourceInfo>& active) {
    QVector<AudioMixer::SourceInfo> out;
    if (!p) return out;
    constexpr double kWarmWin = 0.8; // segundos à frente do playhead
    bool anySolo = false;
    // Só faixas de ÁUDIO têm solo com efeito (consistente com buildMixSources).
    for (const Track& tr : p->audioTracks)
        if (tr.solo) { anySolo = true; break; }

    QSet<QString> have;
    for (const AudioMixer::SourceInfo& a : active) have.insert(a.key);
    // Só faixas de ÁUDIO são fonte de áudio (o áudio de um vídeo vive na faixa
    // de áudio pareada). Consistente com buildMixSources.
    auto warmCollect = [&](const QVector<Track>& tracks, bool isAudio) {
        for (int ti = 0; ti < (int)tracks.size(); ++ti) {
            const Track& tr = tracks[ti];
            if (!tr.visible || tr.muted || (anySolo && !tr.solo)) continue;
            for (const Clip& c : tr.clips) {
                // Já em reprodução agora, ou começa depois da janela: nada a
                // aquecer.
                if (t >= c.pos && t < c.pos + c.dur) continue;
                if (c.pos <= t || c.pos >= t + kWarmWin) continue;
                const MediaItem* m = p->findMedia(c.mediaId);
                if (!m || !m->hasAudio) continue;
                if (have.contains(c.id)) continue;
                have.insert(c.id);
                AudioMixer::SourceInfo si;
                si.key = c.id; // a chave precisa ser a mesma do want futuro
                si.path = m->filePath;
                si.audioStream = c.audioStreamIndex;
                si.mediaPos = c.in; // começo do áudio do clipe
                si.speed = c.speed;
                si.clipPos = c.pos;
                si.clipDur = c.dur;
                si.mediaStart = c.in;
                // Mesmo ganho do instante da costura (início do clipe): inclui
                // fader/automação da faixa e fades. Antes usava `c.volume` cru,
                // então o clipe que entrava num corte saía baixo (sem o fader)
                // até o próximo tick — o "mergulho" no corte.
                si.vol = previewClipAudioVol(tr, c, c.pos);
                si.pan = kfValue(tr.kfPan, tr.pan, c.pos);
                si.trackIndex = ti;
                si.isAudioTrack = isAudio;
                si.eqLow = c.eqLow;
                si.eqMid = c.eqMid;
                si.eqHigh = c.eqHigh;
                si.denoise = c.denoise;
                si.denoiseAmount = c.denoiseAmount;
                si.normalize = c.normalize;
                si.invertPhase = c.invertPhase;
                si.reverb = c.reverb;
                si.reverbMix = c.reverbMix;
                si.reverbSize = c.reverbSize;
                {
                    const Track& tac = p->audioTracks[ti];
                    si.trackFxOn = tac.hasAudioFx();
                    si.trackFxEqLow = tac.eqLow;
                    si.trackFxEqMid = tac.eqMid;
                    si.trackFxEqHigh = tac.eqHigh;
                    si.trackFxDenoise = tac.denoise;
                    si.trackFxDenoiseAmount = tac.denoiseAmount;
                    si.trackFxInvertPhase = tac.invertPhase;
                    si.trackFxReverb = tac.reverb;
                    si.trackFxReverbMix = tac.reverbMix;
                    si.trackFxReverbSize = tac.reverbSize;
                }
                if (audioDbg())
                    qDebug().noquote() << QStringLiteral("[audio] warm t=%1 faixa='%2' isAudio=%3 clip=%4 pos=%5")
                          .arg(t, 0, 'f', 3)
                          .arg(tracks[ti].name)
                          .arg(isAudio)
                          .arg(c.id)
                          .arg(c.pos, 0, 'f', 3);
                out.append(si);
            }
        }
    };
    warmCollect(p->audioTracks, true);
    return out;
}

void PreviewWidget::startAudio(double t) {
    if (!m_project) return;
    stopAudio();
    const QVector<AudioMixer::SourceInfo> sources = buildMixSources(m_project, t);
    if (sources.isEmpty() && audioDbg())
        qDebug() << "[audio] startAudio em t=" << t << "- nenhuma fonte (mixer será criado mudo)";
    if (audioDbg()) {
        qDebug() << "[audio] startAudio em t=" << t << "- fontes:" << sources.size();
        const auto outs = QMediaDevices::audioOutputs();
        for (const auto& d : outs)
            qDebug() << "[audio]   saida:" << d.description() << "default?" << d.isDefault();
        const QAudioDevice def = QMediaDevices::defaultAudioOutput();
        qDebug() << "[audio]   defaultAudioOutput() válido?" << def.isNull() << "-" << def.description();
    }

    QAudioFormat fmt;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    fmt.setSampleFormat(QAudioFormat::Int16);
#else
    fmt.setCodec(QStringLiteral("audio/pcm"));
    fmt.setSampleSize(16);
    fmt.setSampleType(QAudioFormat::SignedInt);
    fmt.setByteOrder(QAudioFormat::LittleEndian);
#endif
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);

    m_audioFeed = new AudioMixer(this);
    m_audioFeed->setMasterVolume(m_project ? m_project->masterVolume : 1.0);
    m_audioFeed->open(QIODevice::ReadOnly | QIODevice::Unbuffered);

    // O mixer lê SÓ do conform (PCM já decodificado em background) → está
    // sempre "pronto": não há decoders a abrir, então o sink já começa. Faltas
    // de trecho são silêncio momentâneo até o worker terminar a janela pedida.
    // Nada de waitReadyBeforeSink() aqui: ele segurava a UI por até 150ms no
    // play (C1 do relatório). `updateSources` já dispara a conform em
    // background; aceitamos 1-2 frames de silêncio no warm-up frio.
    m_audioFeed->updateSources(sources, /*reseek=*/true,
                               QVector<AudioMixer::SourceInfo>(), -1, t);

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    const QAudioDevice def = QMediaDevices::defaultAudioOutput();
    if (def.isNull()) {
        if (audioDbg()) qDebug() << "[audio] defaultAudioOutput nulo — sem saída de áudio";
        return;
    }
    // Reusa o sink entre play/stop (C4): new/destroy custa 10-50ms por play.
    // Recria só se a saída padrão mudou.
    const QString devId = QString::fromUtf8(def.id());
    if (!m_audioSink || m_audioSinkDevice != devId) {
        if (m_audioSink) {
            m_audioSink->stop();
            m_audioSink->deleteLater();
            m_audioSink = nullptr;
        }
        m_audioSink = new QAudioSink(def, fmt, this);
        m_audioSinkDevice = devId;
    }
    m_audioSink->start(m_audioFeed);
#else
    const QAudioDeviceInfo def = QAudioDeviceInfo::defaultOutputDevice();
    if (def.isNull()) {
        if (audioDbg()) qDebug() << "[audio] defaultOutputDevice nulo — sem saída de áudio";
        return;
    }
    const QString devId = def.deviceName();
    if (!m_audioOut || m_audioOutDevice != devId) {
        if (m_audioOut) {
            m_audioOut->stop();
            m_audioOut->deleteLater();
            m_audioOut = nullptr;
        }
        m_audioOut = new QAudioOutput(def, fmt, this);
        m_audioOutDevice = devId;
    }
    m_audioOut->start(m_audioFeed);
#endif
}

void PreviewWidget::stopAudio() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (m_audioSink) {
        // Não destrói: o sink é reutilizado no próximo play (C4). `start()`
        // troca a fonte para o novo m_audioFeed. O `deleteLater` da antiga
        // fonte é seguro porque o sink já foi parado e largou a referência.
        m_audioSink->stop();
    }
#else
    if (m_audioOut) {
        m_audioOut->stop();
    }
#endif
    if (m_audioFeed) {
        // shutdown() síncrono: bloqueia novos updates e libera as fontes antes
        // do mixer ser descartado. As caches do conform continuam no registro.
        m_audioFeed->shutdown();
        m_audioFeed->deleteLater();
        m_audioFeed = nullptr;
    }
}

// Recalcula as fontes de áudio ativas no instante `t` e empurra ao mixer.
// Com o conform, tudo aqui é barato e síncrono (sem open/seekAudio no caminho):
// updateSources só troca ponteiros de leitura e emite pedidos de conform para
// o worker — que decodifica em background, em região por região.
void PreviewWidget::updateMixAudio(double t, bool reseek) {
    if (!m_audioFeed) return;
    m_audioFeed->setMasterVolume(m_project ? m_project->masterVolume : 1.0);
    const QVector<AudioMixer::SourceInfo> sources = buildMixSources(m_project, t);
    const QVector<AudioMixer::SourceInfo> warm =
        buildWarmSources(m_project, t, sources);
    m_audioFeed->updateSources(sources, reseek, warm, -1, t);
}

QString PreviewWidget::resolvePreviewVideo(const QString& srcPath) {
    if (!PreviewProfiler::active())
        return ProxyManager::instance().resolveVideo(srcPath);
    const qint64 t0 = PreviewProfiler::nowNs();
    const QString resolved = ProxyManager::instance().resolveVideo(srcPath);
    profFrame().resolveNs += PreviewProfiler::nowNs() - t0;
    return resolved;
}

void PreviewWidget::updateFrame() {
    m_timeLabel->setText(fmtTimecode(m_playhead, projFps(m_project)));
    if (!m_project) { m_frame = QImage(); m_transAlpha = -1.0; m_underFrame = QImage(); m_clipMasks.clear(); update(); return; }

    const Clip* clip = clipAt(m_playhead);
    if (!clip) {
        m_frame = QImage();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_clipLainkaEnabled = false;
        m_clipOfxFx.clear();
    m_clipMasks.clear();
        update();
        return;
    }

    // Clipe pertence a um grupo Mesa → renderiza a composição inteira
    // (todas as camadas), sem a transform de câmera.
    if (tryRenderMesa(clip)) {
        // Mesmo com Mesa no topo, tracks inferiores precisam ser decodificadas
        // para que paintEvent possa compô-las por baixo (transparência, blend).
        const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
        const int decW = qMax(160, qMax(m_previewQuality, widgetW));
        requestLowerLayers(decW);
        m_underFrame = QImage();
        m_underRequested = false;
        m_transAlpha = -1.0;
        m_clipLainkaEnabled = false;
        m_clipOfxFx.clear();
    m_clipMasks.clear();
        update();
        return;
    }

    const MediaItem* m = m_project->findMedia(clip->mediaIdAt(m_playhead - clip->pos));
    if (!m || !m->hasVideo) {
        m_frame = QImage();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_clipLainkaEnabled = false;
        m_clipOfxFx.clear();
    m_clipMasks.clear();
        update();
        return;
    }

    // Captura estado do LAINKA para uso em applyCrop().
    m_clipLainkaEnabled = clip->lainkaEnabled;
    m_clipLainkaSkip = clip->lainkaSkip;
    m_clipLainkaJitterPos = clip->lainkaJitterPos;
    m_clipLainkaJitterRot = clip->lainkaJitterRot;
    m_clipLainkaJitterScale = clip->lainkaJitterScale;
    m_clipLainkaFlicker = clip->lainkaFlicker;
    m_clipLainkaFlickerSpeed = clip->lainkaFlickerSpeed;
    m_clipLainkaWarpAmount = clip->lainkaWarpAmount;
    m_clipLainkaWarpSpeed = clip->lainkaWarpSpeed;
    m_clipLainkaWarpGrid = clip->lainkaWarpGrid;
    m_clipLainkaOnionSkin = clip->lainkaOnionSkin;
    m_clipLainkaDustAmount = clip->lainkaDustAmount;
    m_clipLainkaScratchAmount = clip->lainkaScratchAmount;
    m_clipLainkaTargetFps = clip->lainkaTargetFps;
    m_clipLainkaMotionBlur = clip->lainkaMotionBlur;
    m_clipLainkaOpacity = clip->lainkaOpacity;
    m_clipLainkaAntialias = clip->lainkaAntialias;
    m_clipLainkaId = clip->id;
    m_clipMotionEnabled = clip->motionEnabled;
    m_clipMotionAmount = clip->motionAmount;
    m_clipMotionAngle = clip->motionAngle;
    m_clipMotionSamples = clip->motionSamples;
    m_clipBrightness = clip->brightness;
    m_clipContrast = clip->contrast;
    m_clipSaturation = clip->saturation;
    m_clipBlur = clip->blur;
    m_clipGrayscale = clip->grayscale;
    m_clipChromaKey = clip->chromaKey;
    m_clipChromaKeyColor = clip->chromaKeyColor;
    m_clipChromaKeySimilarity = clip->chromaKeySimilarity;
    m_clipChromaKeySoftness = clip->chromaKeySoftness;
    m_clipChromaKeySpillSuppress = clip->chromaKeySpillSuppress;
    m_clipPsxEnabled = clip->psxEnabled;
    m_clipPsxDither = clip->psxDither;
    m_clipPsxBits = clip->psxBits;
    m_clipOfxFx = clip->ofxFx;
    m_clipFrei0rFx = clip->frei0rFx;
    m_clipMasks = clip->masks;

    double srcT = clipSrcTime(*clip, m_playhead - clip->pos);

    // LAINKA: quantiza o tempo para simular stop motion.
    // Usa targetFps para calcular o skip (projectFps / targetFps).
    m_lainkaQuantizedTime = -1.0;
    if (clip->lainkaEnabled) {
        const double pfps = projFps(m_project);
        const int effSkip = (clip->lainkaTargetFps > 0 && pfps > 1.0)
            ? std::max(1, (int)std::lround(pfps / clip->lainkaTargetFps))
            : clip->lainkaSkip;
        if (effSkip > 1) {
            srcT = lainkaQuantizeTime(srcT, effSkip, pfps);
            m_lainkaQuantizedTime = srcT;
        }
    }
    // Decodifica na maior entre a qualidade escolhida e a largura exibida:
    // nunca abaixo do tamanho de tela (evita pixelização ao esticar), e nem
    // além do que o monitor precisa, exceto quando a qualidade pede mais.
    const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
    const int decW = qMax(160, qMax(m_previewQuality, widgetW));

    // Camadas inferiores (empilhamento multi-faixa): pede os quadros dos
    // clipes de vídeo que estão por baixo do topo, para compor transparência
    // e blend no preview (antes só o clipe do topo aparecia).
    requestLowerLayers(decW);

    // Mídia gerada (gerador estilo Vegas): gera o quadro (cor, gradiente,
    // checkerboard ou ruído), sem passar pelo decoder (não há arquivo).
    if (m->isSolid) {
        const int w = qMax(1, m->width > 0 ? m->width
                          : (m_project ? m_project->width : 1920));
        const int h = qMax(1, m->height > 0 ? m->height
                          : (m_project ? m_project->height : 1080));
        QImage img = generatorFrame(*m, w, h);
        {
            QMutexLocker l(&m_frameMutex);
            m_frameFull = img;
            m_shownPath = QStringLiteral("solid:") + m->id;
            m_shownT = srcT;
            m_shownW = decW;
            m_lastSrcT = srcT;
            m_lastDecodeW = decW;
            m_lastFile = QString();
            m_prefetch.valid = false;
            m_prefetch.requested = false;
        }
        applyCrop();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
        update();
        return;
    }

    const int cL = (int)std::lround(
        std::clamp(kfValue(clip->kfCropL, clip->cropL, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    const int cR = (int)std::lround(
        std::clamp(kfValue(clip->kfCropR, clip->cropR, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    const int cT = (int)std::lround(
        std::clamp(kfValue(clip->kfCropT, clip->cropT, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    const int cB = (int)std::lround(
        std::clamp(kfValue(clip->kfCropB, clip->cropB, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    m_lastCropL = cL;
    m_lastCropR = cR;
    m_lastCropT = cT;
    m_lastCropB = cB;

    // Transição: procura o clipe anterior da MESMA faixa que se sobrepõe a
    // este (estilo Vegas). O quadro de trás é decodificado pelo canal de
    // prefetch e o paintEvent compõe dissolve/wipe durante a sobreposição.
    const Clip* under = nullptr;
    double overlap = 0.0;
    for (const Track& tr : m_project->videoTracks) {
        bool inTrack = false;
        for (const Clip& c : tr.clips)
            if (c.id == clip->id) { inTrack = true; break; }
        if (!inTrack) continue;
        for (const Clip& o : tr.clips)
            if (o.id != clip->id && o.pos < clip->pos - 1e-6
                && o.pos + o.dur > clip->pos + 1e-6
                && (!under || o.pos > under->pos))
                under = &o;
        break;
    }
    if (under) overlap = under->pos + under->dur - clip->pos;
    if (under && overlap > 1e-6 && m_playhead < clip->pos + overlap - 1e-9) {
        m_transAlpha = std::clamp((m_playhead - clip->pos) / overlap, 0.0, 1.0);
        m_transType = isTransition(under->transitionType)
                          ? under->transitionType
                          : QStringLiteral("dissolve");
        const double rel = m_playhead - under->pos;
        m_underCropL = (int)std::lround(
            std::clamp(kfValue(under->kfCropL, under->cropL, rel), 0.0, 0.9) * 1000.0);
        m_underCropR = (int)std::lround(
            std::clamp(kfValue(under->kfCropR, under->cropR, rel), 0.0, 0.9) * 1000.0);
        m_underCropT = (int)std::lround(
            std::clamp(kfValue(under->kfCropT, under->cropT, rel), 0.0, 0.9) * 1000.0);
        m_underCropB = (int)std::lround(
            std::clamp(kfValue(under->kfCropB, under->cropB, rel), 0.0, 0.9) * 1000.0);
        const MediaItem* um = m_project->findMedia(under->mediaId);
        if (um && um->hasVideo && m_frameWorker) {
            const QString uvpath = resolvePreviewVideo(um->filePath);
            const double uSrcT = clipSrcTime(*under, m_playhead - under->pos);
            QMutexLocker l(&m_frameMutex);
            const bool already = m_underRequested && m_underPath == uvpath
                                 && m_underW == decW
                                 && std::fabs(m_underT - uSrcT) <= 0.5 / projFps(m_project)
                                 && !m_underFrame.isNull();
            if (!already) {
                m_underPath = uvpath;
                m_underT = uSrcT;
                m_underW = decW;
                m_underCropL = m_underCropR = m_underCropT = m_underCropB = 0; // recortado no onFrame
                m_underRequested = true;
                m_underFrame = QImage();
                QMetaObject::invokeMethod(m_frameWorker, "decodePrefetch", Qt::QueuedConnection,
                                          Q_ARG(QString, uvpath),
                                          Q_ARG(double, uSrcT),
                                          Q_ARG(int, decW),
                                          Q_ARG(double, 1.0 / projFps(m_project)));
            }
        }
    } else {
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
    }

    // Snapshot do caminho rápido: clipes sem efeitos temporais levam crop +
    // efeitos básicos + máscaras para a thread de vídeo (FrameWorker), tirando
    // o C2/C3 do tiquinho da UI. O snapshot usa os MESMOS valores que
    // applyCrop() usaria (crop avaliado em m_playhead e máscaras em srcT).
    FrameFx topFx;
    topFx.active = !m_clipLainkaEnabled
                   && !(m_clipMotionEnabled && m_clipMotionAmount > 0.0)
                   && m_clipOfxFx.isEmpty()
                   && m_clipFrei0rFx.isEmpty();
    topFx.cropL = m_lastCropL;
    topFx.cropR = m_lastCropR;
    topFx.cropT = m_lastCropT;
    topFx.cropB = m_lastCropB;
    topFx.rel = srcT;
    if (topFx.active) topFx.clip = *clip;

    // Path efetivo de vídeo do clipe do topo: proxy se houver (consistente com
    // requestFrame/onFrameReady e o cache).
    const QString vpath = resolvePreviewVideo(m->filePath);
    if (PreviewProfiler::active()) {
        // O que o A/B proxy-vs-original precisa saber: de onde saiu o quadro
        // que vai aparecer na tela (o clipe do topo), não quantas camadas
        // inferiores resolveram para proxy.
        profFrame().proxy = (vpath != m->filePath);
        profFrame().cut = (m_profTopClipId != m->id);
        m_profTopClipId = m->id;
    }

    // Se temos um quadro pré-carregado que bate com a posição de entrada do novo clipe, exibe imediatamente.
    bool usedPrefetch = false;
    if (m_transAlpha < 0.0) {
        QMutexLocker l(&m_frameMutex);
        const double frameDur = 1.0 / projFps(m_project);
        if (m_prefetch.valid && m_prefetch.path == vpath
            && std::fabs(m_prefetch.t - srcT) <= frameDur * 0.5
            && m_prefetch.maxW == decW) {
            m_frameFull = m_prefetch.img;
            m_shownPath = vpath;
            m_shownT = srcT;
            m_shownW = decW;
            m_shownFx = false; // quadro cru do prefetch; crop é aplicado na UI
            m_lastSrcT = srcT;
            m_lastDecodeW = decW;
            m_lastFile = vpath;
            m_prefetch.valid = false;
            m_prefetch.requested = false;
            usedPrefetch = true;
            applyCrop();
            update();
        }
    }

    // O quadro do tamanho/posição atuais já está pronto? Apenas reaplica o
    // pan/crop (por exemplo quando só o corte mudou) sem decodificar de novo.
    bool sameShown = false;
    bool shownFx = false;
    {
        QMutexLocker l(&m_frameMutex);
        sameShown = (!usedPrefetch && m_shownPath == vpath && std::fabs(m_shownT - srcT) < 1e-6
                     && m_shownW == decW);
        shownFx = m_shownFx;
    }
    if (sameShown) {
        if (shownFx) {
            // Veio pronto da thread do worker (já cortado/efeituado): aplicar
            // applyCrop() por cima duplicaria o corte. Re-pede o quadro com o
            // snapshot atual — o worker reaplica a cadeia.
            requestFrame(clip->id, vpath, srcT, decW, topFx);
        } else {
            applyCrop();
            update();
        }
        return;
    }
    if (usedPrefetch) {
        if (PreviewProfiler::active()) profFrame().prefetchHit = true;
        // Já no tick do corte, dispara o próximo frame: o decoder trocado está
        // posicionado e decodifica adiante, evitando "segurar" o frame do corte.
        requestFrame(clip->id, vpath, srcT + 1.0 / projFps(m_project), decW, topFx);
        return;
    }
    if (PreviewProfiler::active()) profFrame().prefetchHit = false;
    requestFrame(clip->id, vpath, srcT, decW, topFx);
}

// Pedido "assíncrono": a decodificação acontece na thread do FrameWorker.
// Vários pedidos seguidos entram numa fila; apenas um roda por vez
// (scrub não empilha — a fila só guarda pedidos ainda não enviados).
void PreviewWidget::requestFrame(const QString& clipId, const QString& path, double t, int maxW,
                                 const FrameFx& fx) {
    QMutexLocker l(&m_frameMutex);
    // Fase 3: se um decode já está em andamento e este pedido do TOPO o supera
    // (cortou para outro clipe, ou scrub/seek de mais de 3 frames parado),
    // cancela o decode obsoleto — o worker larga o alvo antigo e pega o novo já.
    // Durante reprodução no MESMO clipe NÃO cancela: o avanço de 1 frame por
    // tick completaria mesmo; cancelar a cada tick travaria o preview.
    if (m_hasInflightReq && m_frameWorker && m_project) {
        // PIERROT_NO_CANCEL=1 desliga (A/B: medir o efeito do cancelamento).
        static const bool kCancelOn = [] {
            return !qEnvironmentVariableIsSet("PIERROT_NO_CANCEL");
        }();
        const Clip* top = clipAt(m_playhead);
        if (kCancelOn && top && top->id == clipId) {
            bool cancel = (m_inflightReq.clipId != clipId);
            if (!cancel && !m_playing) {
                const double fd = projFps(m_project) > 0.0 ? 1.0 / projFps(m_project) : 1.0 / 30.0;
                cancel = std::fabs(m_inflightReq.t - t) > fd * 3.0;
            }
            if (cancel) m_frameWorker->requestCancel();
        }
    }
    // Coalesce: durante o scrub (e na reprodução) chegam vários alvos para o
    // mesmo clipe; só o mais recente interessa — o worker é serial e decodificar
    // posições intermediárias só atrasa a chegada ao alvo final.
    for (FrameReq& r : m_reqQueue)
        if (r.clipId == clipId && r.path == path && r.maxW == maxW) {
            r.fx = fx;
            if (std::fabs(r.t - t) < 1e-6) return; // já enfileirado igual
            r.t = t;
            r.dt = 1.0 / projFps(m_project);
            return;
        }
    m_reqQueue.append({clipId, path, t, 1.0 / projFps(m_project), maxW, fx});
    kickFrameWorker();
}

// Crop (pan/crop) do clipe no instante `rel` da timeline (definição abaixo).
static void clipCrop(const Clip& c, double rel, int& cL, int& cR, int& cT, int& cB);

// Pedidos os quadros dos clipes de vídeo ativos no playhead que ficam POR
// BAIXO do clipe do topo (empilhamento de faixas). Quadros já em cache
// (mesmo arquivo/tempo/tamanho) são pulados.
void PreviewWidget::requestLowerLayers(int decW) {
    if (!m_project) return;
    const Clip* top = clipAt(m_playhead);
    // Quantos decodes de camada inferior são pedidos por frame — candidato nº1
    // a roubar slots do worker que deveria estar decodificando o clipe do topo
    // (vídeo que engasga com áudio perfeito = o quadro do topo chega tarde).
    int dbgLayerDecodes = 0;
    // Camadas que deixaram de estar ativas no playhead saem do cache (evita
    // crescimento sem limite conforme o usuário navega pelo projeto).
    {
        QList<QString> active;
        for (const Track& tr : m_project->videoTracks) {
            if (!tr.visible) continue;
            for (const Clip& cl : tr.clips)
                if (!cl.isText && m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur)
                    active.append(cl.id);
        }
        QMutexLocker l(&m_frameMutex);
        for (auto it = m_layerCache.begin(); it != m_layerCache.end();) {
            if (!active.contains(it.key())) it = m_layerCache.erase(it);
            else ++it;
        }
    }
    for (const Track& tr : m_project->videoTracks) {
        if (!tr.visible) continue;   // faixa oculta (olho)
        const Clip* c = nullptr;
        for (const Clip& cl : tr.clips) {
            if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                const MediaItem* mm = m_project->findMedia(cl.mediaIdAt(m_playhead - cl.pos));
                if (mm && mm->hasVideo && (!c || cl.pos > c->pos)) c = &cl;
            }
        }
        if (!c || (top && c->id == top->id)) continue;
        const MediaItem* m = m_project->findMedia(c->mediaIdAt(m_playhead - c->pos));
        if (!m || !m->hasVideo) continue;
        // Cor sólida não tem arquivo: é gerada na pintura, não pede decode.
        if (m->isSolid) continue;
        // Malha 3D também não é mídia decodificável: um .obj não vai pelo
        // FFmpeg. A camada é rasterizada por `MesaRenderer::prepareLayer`,
        // no loop de tracks Mesa logo abaixo.
        if (m->isMesh) continue;
        const double srcT = clipSrcTime(*c, m_playhead - c->pos);
        const QString vpath = resolvePreviewVideo(m->filePath);
        {
            QMutexLocker l(&m_frameMutex);
            const auto it = m_layerCache.constFind(c->id);
            if (it != m_layerCache.constEnd() && it->path == vpath
                && std::fabs(it->t - srcT) < 3.0 / projFps(m_project)
                && it->maxW == decW && !it->img.isNull())
                continue; // já em cache (3 frames: evita re-decode de background a cada frame)
        }
        requestFrame(c->id, vpath, srcT, decW, [&] {
            // Camada inferior sem LAINKA: crop + efeitos básicos vão para a
            // thread do worker (era o H6: per-pixel na UI a cada quadro).
            FrameFx fx;
            fx.active = !c->lainkaEnabled;
            if (fx.active) {
                const double rel = m_playhead - c->pos;
                clipCrop(*c, rel, fx.cropL, fx.cropR, fx.cropT, fx.cropB);
                fx.rel = rel;
                fx.clip = *c;
            }
            return fx;
        }());
        ++dbgLayerDecodes;
        // Prioriza o clipe do topo: no máximo 1 camada inferior nova por tick.
        // Com N faixas, os decodes atrasados se completam ao longo dos próximos
        // frames em vez de roubarem todos os slots do worker de uma vez.
        break;
    }

    // Tracks Mesa: decodifica o frame individual de cada track via MesaRenderer
    // e armazena no layerCache, para que paintEvent possa compô-las por baixo
    // da composição Mesa do topo (transparência, blend entre faixas).
    for (const Track& tr : m_project->videoTracks) {
        if (!tr.visible) continue;   // faixa oculta (olho)
        const MesaComposition* mc = m_project->findMesaForTrack(tr.id);
        if (!mc) continue;
        const Clip* c = nullptr;
        for (const Clip& cl : tr.clips) {
            if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                if (!c || cl.pos > c->pos) c = &cl;
            }
        }
        if (!c || (top && c->id == top->id)) continue;
        // Cache hit: já renderizado nesta frame.
        {
            QMutexLocker l(&m_frameMutex);
            if (m_layerCache.contains(c->id)) continue;
        }
        MesaRenderer::LayerPrep prep;
        if (m_mesaRenderer.prepareLayer(prep, *mc, *m_project, m_playhead, tr) && prep.valid) {
            QMutexLocker l(&m_frameMutex);
            m_layerCache[c->id] = {prep.frame,
                                   QStringLiteral("mesa:") + tr.id,
                                   m_playhead, decW};
        }
    }
    // Log de diagnóstico: quantos decodes inferiores dispararam este frame — se
    // houver, o worker está dividindo atenção entre o clipe do topo e N camadas
    // novas ao mesmo tempo (candidato nº1 ao engasgo pós-20s em projetos com
    // múltiplas faixas).
    if (playDbg() && dbgLayerDecodes > 0)
        qDebug().noquote() << QStringLiteral("[play] layers decode=%1 t=%2 top=%3 queue=%4")
                  .arg(dbgLayerDecodes)
                  .arg(m_playhead, 0, 'f', 3)
                  .arg(top ? top->id.mid(0,8) : QString())
                  .arg(m_reqQueue.size());
    if (PreviewProfiler::active()) {
        profFrame().lowerReq = dbgLayerDecodes;
        profFrame().queue = m_reqQueue.size();
    }
}

// Chamado com m_frameMutex segurado.
void PreviewWidget::kickFrameWorker() {
    if (m_workerBusy || m_reqQueue.isEmpty() || !m_frameWorker) return;

    // Podas pedidos órfãos: na virada de um corte, o pedido residual do clipe
    // antigo não é mais "vivo" — decodificá-lo gastava um slot inteiro e
    // atrasava o 1º frame do clipe novo (o "tiquinho" de travada). Na fila só
    // interessam clipes que contêm o playhead agora (topo ou camadas ativas).
    if (m_project) {
        for (int i = m_reqQueue.size() - 1; i >= 0; --i) {
            const QString& cid = m_reqQueue[i].clipId;
            bool live = false;
            for (const Track& tr : m_project->videoTracks)
                for (const Clip& c : tr.clips)
                    if (c.id == cid && !c.isText
                        && m_playhead >= c.pos - 1e-6 && m_playhead < c.pos + c.dur + 1e-6) {
                        live = true;
                        break;
                    }
            if (!live) m_reqQueue.removeAt(i);
        }
        if (m_reqQueue.isEmpty()) return;
    }

    m_workerBusy = true;
    if (m_perfT.isValid()) m_perfWorkerStartNs = m_perfT.nsecsElapsed();
    // Se o clipe do topo tem pedido na fila, ele vai primeiro (o vivo); os
    // demais (camadas inferiores) ficam para a folga.
    int idx = 0;
    const Clip* top = clipAt(m_playhead);
    if (top && m_reqQueue.size() > 1) {
        for (int i = 0; i < m_reqQueue.size(); ++i)
            if (m_reqQueue[i].clipId == top->id) { idx = i; break; }
    }
    const FrameReq r = m_reqQueue.takeAt(idx);
    m_inflightReq = r;
    m_hasInflightReq = true;
    QMetaObject::invokeMethod(m_frameWorker, "decodeOne", Qt::QueuedConnection,
                              Q_ARG(QString, r.clipId), Q_ARG(QString, r.path),
                              Q_ARG(double, r.t), Q_ARG(int, r.maxW), Q_ARG(double, r.dt),
                              Q_ARG(FrameFx, r.fx));
}

// Crop (pan/crop) do clipe no instante `rel` da timeline.
static void clipCrop(const Clip& c, double rel, int& cL, int& cR, int& cT, int& cB) {
    cL = (int)std::lround(std::clamp(kfValue(c.kfCropL, c.cropL, rel), 0.0, 0.9) * 1000.0);
    cR = (int)std::lround(std::clamp(kfValue(c.kfCropR, c.cropR, rel), 0.0, 0.9) * 1000.0);
    cT = (int)std::lround(std::clamp(kfValue(c.kfCropT, c.cropT, rel), 0.0, 0.9) * 1000.0);
    cB = (int)std::lround(std::clamp(kfValue(c.kfCropB, c.cropB, rel), 0.0, 0.9) * 1000.0);
}

void PreviewWidget::onFrameReady(const QString& clipId, const QString& path, double t, int maxW,
                                 const QImage& img, bool processed) {
    const qint64 workerLatNs =
        (PreviewProfiler::active() && m_perfT.isValid() && m_perfWorkerStartNs > 0)
            ? m_perfT.nsecsElapsed() - m_perfWorkerStartNs
            : 0;
    if (PreviewProfiler::active())
        profFrame().workerNs = workerLatNs; // latência do decode: dispatch -> quadro pronto
        profFrame().cacheHits = FFmpegDecoder::cacheHits();
        profFrame().cacheMisses = FFmpegDecoder::cacheMisses();
        profFrame().decSeekNs = qint64(FFmpegDecoder::lastSeekNs());
        profFrame().decDiscard = FFmpegDecoder::lastDiscard();
        profFrame().decWorkNs = FFmpegDecoder::lastWorkNs();
    {
        QMutexLocker l(&m_frameMutex);
        m_workerBusy = false;
        m_hasInflightReq = false;
        m_perfWorkerStartNs = 0;
        if (workerLatNs > 0) m_perf.workerMs = workerLatNs / 1000000;
        kickFrameWorker(); // continua com o próximo pedido, se houver
    }

    // ── Adaptive quality: monitora latência do decode ──────────────
    if (m_playing && m_perf.workerMs > 0 && m_project && m_project->fps > 0.0) {
        const double frameMs = 1000.0 / m_project->fps;
        // Média móvel exponencial (suaviza spikes).
        m_adaptiveDecodeMsAvg = m_adaptiveDecodeMsAvg * 0.8 + m_perf.workerMs * 0.2;
        if (m_adaptiveDecodeMsAvg > frameMs * 1.2) {
            ++m_adaptiveSlowCount;
        } else {
            m_adaptiveSlowCount = std::max(0, m_adaptiveSlowCount - 1);
        }
        // 5 ticks consecutivos lentos → baixa resolução.
        if (m_adaptiveSlowCount >= 5 && !m_adaptiveActive) {
            m_adaptiveBaseQuality = m_previewQuality;
            const int levels[] = {3840, 1080, 720, 480, 360};
            for (int i = 0; i < 5; ++i) {
                if (m_previewQuality >= levels[i] && levels[i] < m_previewQuality) {
                    setPreviewQuality(levels[i]);
                    m_adaptiveActive = true;
                    break;
                }
            }
        }
        // Recupera quando a latência normaliza por 30 ticks.
        if (m_adaptiveActive && m_adaptiveDecodeMsAvg < frameMs * 0.7) {
            m_adaptiveSlowCount -= 2;
            if (m_adaptiveSlowCount <= 0) {
                setPreviewQuality(m_adaptiveBaseQuality);
                m_adaptiveActive = false;
                m_adaptiveSlowCount = 0;
            }
        }
        // Acumula dropped frames para o overlay.
        m_perf.droppedTotal += consumeDroppedFrames(); // só o overlay do live
    }

    if (img.isNull() || !m_project) return;

    const Clip* clip = nullptr;
    for (const Track& tr : m_project->videoTracks)
        for (const Clip& c : tr.clips)
            if (c.id == clipId) { clip = &c; break; }
    if (!clip) return;
    const MediaItem* m = m_project->findMedia(clip->mediaId);
    if (!m) return;
    if (resolvePreviewVideo(m->filePath) != path) return;
    if (PreviewProfiler::active())
        profFrame().proxy = (path != m->filePath); // o quadro exibido veio de proxy?

    // Ignora quadros decodificados para outra posição (scrub/seek rápido muito distante).
    const double wantT = clipSrcTime(*clip, m_playhead - clip->pos);
    if (std::fabs(wantT - t) > 1.5) return;

    // Quantum a que o playhead (já cruzou o frame) está à frente do quadro que
    // acabou de decodificar. >1 frame em reprodução = o vídeo está atrasado e
    // vai ENGASGAR (re-exibir) mesmo com o áudio perfeito.
    if (playDbg() && m_playing && std::fabs(t - wantT) <= 1.5) {
        const double aheadFrames = (wantT - t) * projFps(m_project);
        if (std::fabs(aheadFrames) > 0.5)
            qDebug().noquote() << QStringLiteral("[play] frame atraso %1f (dec %2s playhead %3s) lat %4ms q %5")
                      .arg(aheadFrames, 4, 'f', 1)
                      .arg(t, 0, 'f', 3)
                      .arg(wantT, 0, 'f', 3)
                      .arg(static_cast<double>(m_perf.workerMs), 0, 'f', 0)
                      .arg(maxW);
    }

    // Quadro do clipe do TOPO: caminho atual do preview (com prefetch/pan-crop).
    const Clip* top = clipAt(m_playhead);
    if (composeDbg())
        qDebug() << "[compose] frameReady clipId=" << clipId
                 << "path=" << path << "t=" << t
                 << "top=" << (top ? top->id : QString())
                 << "isTop=" << (top && top->id == clipId);
    if (top && top->id == clipId) {
        {
            QMutexLocker l(&m_frameMutex);
            m_shownPath = path;
            m_shownT = t;
            m_shownW = maxW;
            m_shownFx = processed;   // m_frameFull já chegou cortado/efeituado
        }
        m_frameFull = img;
        m_lastSrcT = t;
        m_lastDecodeW = maxW;
        m_lastFile = path;
        if (processed) {
            // Camada rápida: crop + efeitos básicos/máscaras foram aplicados na
            // thread de vídeo — aplicar applyCrop() aqui duplicaria o pipeline.
            m_frame = img;
        } else {
            applyCrop();
        }
        update();
        return;
    }

    // Quadro de uma camada INFERIOR: guarda no cache de camadas (cortado + LAINKA).
    QImage cropped;
    if (processed) {
        // Já veio cortado + com efeitos básicos/máscaras da thread do worker.
        cropped = img;
    } else {
        const double rel = m_playhead - clip->pos;
        int cL, cR, cT, cB;
        clipCrop(*clip, rel, cL, cR, cT, cB);
        cropped = applyCropTo(img, cL, cR, cT, cB);
        // Aplica LAINKA também em camadas inferiores.
        if (clip->lainkaEnabled) {
            double srcT = clipSrcTime(*clip, m_playhead - clip->pos);
            const double pfps = projFps(m_project);
            const int effSkip = (clip->lainkaTargetFps > 0 && pfps > 1.0)
                ? std::max(1, (int)std::lround(pfps / clip->lainkaTargetFps))
                : clip->lainkaSkip;
            if (effSkip > 1)
                srcT = lainkaQuantizeTime(srcT, effSkip, pfps);
            cropped = lainkaApplyFx(cropped, clip->id, srcT,
                                    clip->lainkaSkip, clip->lainkaJitterPos,
                                    clip->lainkaJitterRot, clip->lainkaJitterScale,
                                    clip->lainkaFlicker, clip->lainkaFlickerSpeed,
                                    clip->lainkaWarpAmount, clip->lainkaWarpSpeed,
                                    clip->lainkaWarpGrid, clip->lainkaOnionSkin,
                                    clip->lainkaDustAmount, clip->lainkaScratchAmount,
                                    clip->lainkaMotionBlur, clip->lainkaOpacity,
                                    clip->lainkaTargetFps, clip->lainkaAntialias,
                                    QImage());
        }
        // Aplica efeitos básicos também em camadas inferiores.
        applyBasicEffectsOn(cropped, *clip, m_playhead - clip->pos);
    }
    {
        QMutexLocker l(&m_frameMutex);
        m_layerCache[clipId] = {cropped, path, t, maxW};
    }
    update();
}

void PreviewWidget::onPrefetchReady(const QString& path, double t, int maxW, const QImage& img) {
    const qint64 latNs =
        (PreviewProfiler::active() && m_perfT.isValid() && m_perfPrefetchStartNs > 0)
            ? m_perfT.nsecsElapsed() - m_perfPrefetchStartNs : 0;
    if (PreviewProfiler::active()) profFrame().prefetchLatNs = latNs;
    QMutexLocker l(&m_frameMutex);
    if (latNs > 0) m_perf.prefetchLatMs = latNs / 1000000;
    m_perfPrefetchStartNs = 0;
    // Quadro do clipe de trás (transição ativa).
    if (m_underRequested && m_underPath == path
        && std::fabs(m_underT - t) < 1e-4 && m_underW == maxW) {
        m_underFrame = img.isNull() ? QImage() : applyCropTo(img, m_underCropL, m_underCropR,
                                                             m_underCropT, m_underCropB);
    }
    if (m_prefetch.requested && m_prefetch.path == path
        && std::fabs(m_prefetch.t - t) < 1e-4 && m_prefetch.maxW == maxW) {
        m_prefetch.img = img;
        m_prefetch.valid = !img.isNull();
        m_prefetch.invoked = false; // pronto (ou falhou): permite novo ciclo
        if (img.isNull()) m_prefetch.requested = false; // falhou: encerra o ciclo
    }
    // O prefetch liberou o caminho — retoma os decodeOne's que estavam esperando.
    m_workerBusy = false;
    kickFrameWorker();
}

void PreviewWidget::onBgPrefetchDone(const QString& path, double t, int maxW,
                                     const QImage& frame0, const QImage& frame1) {
    Q_UNUSED(frame1);
    const qint64 latNs =
        (PreviewProfiler::active() && m_perfT.isValid() && m_perfPrefetchStartNs > 0)
            ? m_perfT.nsecsElapsed() - m_perfPrefetchStartNs : 0;
    if (PreviewProfiler::active()) profFrame().prefetchLatNs = latNs;
    {
        QMutexLocker l(&m_frameMutex);
        if (latNs > 0) m_perf.prefetchLatMs = latNs / 1000000;
        m_perfPrefetchStartNs = 0;
        m_bgPrefetchBusy = false;
        if (m_prefetch.requested && m_prefetch.path == path && m_prefetch.maxW == maxW) {
            m_prefetch.img = frame0;
            m_prefetch.valid = !frame0.isNull();
            m_prefetch.invoked = false;
            if (frame0.isNull()) m_prefetch.requested = false;
        }
    }
    // O decoder NÃO vem junto: o prefetch só aqueceu o pool e já devolveu a
    // entrada (release). No corte, o FrameWorker reacquire o mesmo decoder já
    // quente na posição do próximo clipe — sem troca de posse, sem fila.
}

void PreviewWidget::onBgPrefetchFailed(const QString& path) {
    QMutexLocker l(&m_frameMutex);
    m_bgPrefetchBusy = false;
    if (m_prefetch.requested && m_prefetch.path == path) {
        m_prefetch.valid = false;
        m_prefetch.requested = false;
    }
}

// Próximo clipe que começa depois do fim de `c` (varredura igual à do
// updatePrefetch: próximo início de clipe na timeline, preferindo faixa de cima).
static const Clip* clipAfter(const Project* p, const Clip* c) {
    if (!p || !c) return nullptr;
    const double start = c->pos + c->dur;
    const Clip* found = nullptr;
    double nextPos = 1e9;
    for (int tr = (int)p->videoTracks.size() - 1; tr >= 0; --tr) {
        for (const Clip& cc : p->videoTracks[tr].clips) {
            if (cc.pos >= start - 1e-4 && cc.pos < nextPos) {
                nextPos = cc.pos;
                found = &cc;
            }
        }
    }
    return found;
}

void PreviewWidget::updatePrefetch() {
    if (!m_project || !m_frameWorker || !m_playing) return;
    // Durante uma transição o canal de prefetch decodifica o clipe de trás.
    if (m_transAlpha >= 0.0) return;
    const Clip* clip = clipAt(m_playhead);
    if (!clip) {
        QMutexLocker l(&m_frameMutex);
        m_prefetch.valid = false;
        m_prefetch.requested = false;
        return;
    }
    const double remain = (clip->pos + clip->dur) - m_playhead;
    // Janela dinâmica: 5s para clipes longos, no mínimo 2s para curtos.
    const double window = qMin(5.0, qMax(2.0, clip->dur * 0.8));
    if (remain > window || remain <= 0.0) return;

    // Procura o próximo clipe que será exibido no fim do clipe atual
    const Clip* nextClip = nullptr;
    double nextPos = 1e9;
    for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
        for (const Clip& c : m_project->videoTracks[tr].clips) {
            if (c.pos >= clip->pos + clip->dur - 1e-4 && c.pos < nextPos) {
                nextPos = c.pos;
                nextClip = &c;
            }
        }
    }
    if (!nextClip) return;

    const MediaItem* nextMedia = m_project->findMedia(nextClip->mediaId);
    if (!nextMedia || !nextMedia->hasVideo) return;

    const double srcT = nextClip->in;
    // Prefetch no mesmo tamanho do quadro com qualidade (nunca abaixo da tela).
    const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
    const int decW = qMax(160, qMax(m_previewQuality, widgetW));
    const QString vpath = resolvePreviewVideo(nextMedia->filePath);

    // ── Frame0 do próximo clipe (crítico: o corte mais próximo) ──────────
    // Decide ANTES do warm-ahead e despacha logo, para que decode(N1) seja o
    // primeiro da fila da thread bg (nunca atrasado pelo warm do 2º clipe).
    bool needFrame0 = false;
    {
        QMutexLocker l(&m_frameMutex);
        if (m_perfT.isValid()) m_perfPrefetchStartNs = m_perfT.nsecsElapsed();
        if (m_prefetch.requested && m_prefetch.path == vpath
            && std::fabs(m_prefetch.t - srcT) < 1e-4 && m_prefetch.maxW == decW) {
            needFrame0 = false; // já solicitado ou já pronto
        } else {
            m_prefetch.path = vpath;
            m_prefetch.t = srcT;
            m_prefetch.maxW = decW;
            m_prefetch.img = QImage();
            m_prefetch.valid = false;
            m_prefetch.requested = true;
            m_prefetch.invoked = true; // despachado agora (re-despachado se o corte chegar sem terminar)
            m_prefetch.clipEnd = clip->pos + clip->dur;
            needFrame0 = true;
        }
    }
    if (needFrame0 && m_bgPrefetchWorker && !m_bgPrefetchBusy) {
        m_bgPrefetchBusy = true;
        QMetaObject::invokeMethod(m_bgPrefetchWorker, "decode", Qt::QueuedConnection,
                                  Q_ARG(QString, vpath),
                                  Q_ARG(double, srcT),
                                  Q_ARG(int, decW),
                                  Q_ARG(double, 1.0 / projFps(m_project)));
    }

    // ── Warm-ahead (Fase 2): 2º/3º clipe à frente, só abre+posiciona ─────
    // Entra na fila da mesma thread DEPOIS do decode(N1) acima, então nunca
    // compete com o frame do corte próximo. Se não der tempo, o cut daquele
    // clipe paga um open() — na prática há janela de 2-5s + a duração do clipe
    // do meio pra aquecer. Vem SEMPRE aqui (mesmo se N1 já foi pedido/ocupado),
    // para cortes em sequência não reabrirem o 2º arquivo frio.
    QVector<WarmReq> wants;
    const Clip* ahead = nextClip;
    for (int i = 0; i < 2 && ahead; ++i) {
        ahead = clipAfter(m_project, ahead);
        if (!ahead) break;
        const MediaItem* am = m_project->findMedia(ahead->mediaId);
        if (!am || !am->hasVideo) continue;
        const QString apath = resolvePreviewVideo(am->filePath);
        if (apath.isEmpty() || apath == vpath) continue; // mesmo arquivo já coberto
        if (m_decPool->contains(apath) || apath == m_warmInFlight) continue;
        bool dup = false;
        for (const WarmReq& w : wants)
            if (w.path == apath) { dup = true; break; }
        if (!dup) wants.push_back({apath, ahead->in, decW});
    }
    {
        QMutexLocker l(&m_frameMutex);
        m_warmQueue = std::move(wants);
        if (m_warmQueue.isEmpty()) m_warmInFlight.clear();
    }
    kickWarmQueue();
}

// Despacha o próximo aquecimento da fila (seguro: pega m_frameMutex). Todos os
// callers rodam na thread da UI (updatePrefetch e onBgWarmDone); o lock aqui é
// só para manter o invariante com outras leituras da UI.
void PreviewWidget::kickWarmQueue() {
    if (!m_bgPrefetchWorker) return;
    QMutexLocker l(&m_frameMutex);
    if (!m_warmInFlight.isEmpty() || m_warmQueue.isEmpty()) return;
    const WarmReq w = m_warmQueue.takeFirst();
    m_warmInFlight = w.path;
    QMetaObject::invokeMethod(m_bgPrefetchWorker, "warm", Qt::QueuedConnection,
                              Q_ARG(QString, w.path),
                              Q_ARG(double, w.t),
                              Q_ARG(int, w.maxW));
}

void PreviewWidget::onBgWarmDone(const QString& path) {
    {
        QMutexLocker l(&m_frameMutex);
        if (m_warmInFlight == path) m_warmInFlight.clear();
    }
    kickWarmQueue(); // próximo da fila, se houver
}

// Aplica pan/crop sobre um quadro e devolve o recorte.
QImage PreviewWidget::applyCropTo(const QImage& img, int cL, int cR, int cT, int cB) {
    const double w = img.width();
    const double h = img.height();
    if (w <= 1 || h <= 1 || (!cL && !cR && !cT && !cB)) return img;
    const int x = (int)std::lround(w * cL / 1000.0);
    const int y = (int)std::lround(h * cT / 1000.0);
    const int cw = (int)std::lround(w * (1.0 - (cL + cR) / 1000.0));
    const int ch = (int)std::lround(h * (1.0 - (cT + cB) / 1000.0));
    return img.copy(QRect(std::clamp(x, 0, (int)w - 1), std::clamp(y, 0, (int)h - 1),
                          qMin(cw, (int)w - std::clamp(x, 0, (int)w - 1)),
                          qMin(ch, (int)h - std::clamp(y, 0, (int)h - 1))));
}

// Aplica efeitos básicos (brilho, contraste, saturação, desfoque, preto e branco, chroma key)
// sobre o quadro do clipe ativo. Chamado após o crop em applyCrop().
void PreviewWidget::applyBasicEffects(QImage& img) {
    if (img.isNull()) return;
    Clip c;
    c.brightness = m_clipBrightness;
    c.contrast = m_clipContrast;
    c.saturation = m_clipSaturation;
    c.blur = m_clipBlur;
    c.grayscale = m_clipGrayscale;
    c.chromaKey = m_clipChromaKey;
    c.chromaKeyColor = m_clipChromaKeyColor;
    c.chromaKeySimilarity = m_clipChromaKeySimilarity;
    c.chromaKeySoftness = m_clipChromaKeySoftness;
    c.chromaKeySpillSuppress = m_clipChromaKeySpillSuppress;
    c.psxEnabled = m_clipPsxEnabled;
    c.psxDither = m_clipPsxDither;
    c.psxBits = m_clipPsxBits;
    // Máscaras do clipe do topo: aplicadas na mesma ordem (início) e espaço
    // (quadro já cortado) que nas camadas inferiores — o topo também respeita
    // o recorte por forma. `m_lastSrcT` é o tempo relativo do clipe.
    c.masks = m_clipMasks;
    applyBasicEffectsOn(img, c, m_lastSrcT);
}

// Aplica as máscaras do clipe ao quadro: multiplica o alpha pela cobertura da
// forma (rect/ellipse), com feather (borda suave por distância assinada) e
// invert, combinadas em união. Avalia os keyframes em `rel` (tempo relativo).
void PreviewWidget::applyMasks(QImage& img, const Clip& c, double rel) {
    const int w = img.width();
    const int h = img.height();
    if (w < 1 || h < 1) return;
    if (img.format() != QImage::Format_ARGB32
        && img.format() != QImage::Format_ARGB32_Premultiplied)
        return; // precisa de canal alpha por byte

    std::vector<float> cov((size_t)w * (size_t)h, 0.0f);
    for (const Mask& m : c.masks) {
        if (!m.hasMask()) continue;
        if (m.type != QLatin1String("rect") && m.type != QLatin1String("ellipse"))
            continue; // poly: v1 não exporta ainda — ignorar p/ manter paridade
        const double cx = m.cxAt(rel) * w;
        const double cy = m.cyAt(rel) * h;
        const double rx = std::max(0.001, m.rxAt(rel) * w);
        const double ry = std::max(0.001, m.ryAt(rel) * h);
        const double rotRad = m.rotAt(rel) * M_PI / 180.0;
        const double cosR = std::cos(-rotRad);
        const double sinR = std::sin(-rotRad);
        const double fp = m.featherAt(rel) * (double)std::max(w, h);
        const bool invert = m.invert;
        const bool ellipse = (m.type == QLatin1String("ellipse"));

        for (int y = 0; y < h; ++y) {
            const double ly0 = y - cy;
            size_t idx = (size_t)y * w;
            for (int x = 0; x < w; ++x, ++idx) {
                const double lx0 = x - cx;
                const double lx = lx0 * cosR - ly0 * sinR;
                const double ly = lx0 * sinR + ly0 * cosR;
                double d;
                if (ellipse) {
                    const double n = (lx * lx) / (rx * rx) + (ly * ly) / (ry * ry);
                    d = (std::sqrt(std::max(0.0, n)) - 1.0) * std::min(rx, ry);
                } else {
                    d = std::max(std::abs(lx) - rx, std::abs(ly) - ry);
                }
                double v = (fp > 0.5) ? std::clamp(0.5 - d / fp, 0.0, 1.0)
                                      : ((d < 0.0) ? 1.0 : 0.0);
                if (invert) v = 1.0 - v;
                if (v > cov[idx]) cov[idx] = (float)v;
            }
        }
    }

    // Multiplica o alpha do quadro pela cobertura acumulada. (Em imagens
    // premultiplied, só o alpha muda; o RGB já guarda cor · alpha.)
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        const float* cv = &cov[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            uchar& a = line[x * 4 + 3];
            a = (uchar)std::lround(a * cv[x]);
        }
    }
}

void PreviewWidget::applyBasicEffectsOn(QImage& img, const Clip& c, double rel) {
    if (img.isNull()) return;

    // Máscaras: recorta o quadro por forma (rect/ellipse), com feather e
    // invert, em união. Rodam antes do chroma/desfoque/cor para que esses
    // efeitos respeitem a área visível.
    if (c.hasMask())
        applyMasks(img, c, rel);

    // Chroma Key robusto: distância perceptual ponderada, bordas suaves,
    // supressão de spill e curva de transição suave (smoothstep).
    if (c.chromaKey) {
        QImage result = ImgPool::get(QImage::Format_ARGB32, img.width(), img.height());
        result.fill(Qt::transparent);
        const int w = img.width();
        const int h = img.height();
        const QColor key = c.chromaKeyColor;
        const double sim = std::clamp(c.chromaKeySimilarity, 0.0, 1.0);
        const double soft = std::clamp(c.chromaKeySoftness, 0.0, 1.0);
        const double spill = std::clamp(c.chromaKeySpillSuppress, 0.0, 1.0);
        const int kr = key.red(), kg = key.green(), kb = key.blue();

        // Pesos perceptuais (BT.601): olho humano mais sensível ao verde.
        constexpr double wR = 0.299, wG = 0.587, wB = 0.114;

        // Faixa total. A tolerância da borda é ABSOLUTA (distância perceptual),
        // não proporcional à similaridade: com o mapeamento relativo anterior
        // (core * (1 + soft*3)) a rampa default ficava com ~11 de 255 — corte
        // duro — e aumentar a similaridade estrangulava a suavidade.
        const double coreRange = sim * 255.0;
        const double fullRange = coreRange + soft * 255.0;
        const double coreSq = coreRange * coreRange;
        const double fullSq = fullRange * fullRange;

        // Canal dominante da cor-chave: define qual componente o spill remove.
        // Antes isto era fixo em verde, então o controle não fazia nada em
        // fundo azul/vermelho. Ties (ciano, magenta, amarelo, cinza) ficam de
        // fora — sem canal dominante definido não há respingo a atribuir.
        const int keyCh = (kg >= kr && kg >= kb) ? 1 : (kb >= kr ? 2 : 0);
        const int keyVals[3] = { kr, kg, kb };
        const bool keyDominates = keyVals[keyCh] > keyVals[(keyCh + 1) % 3]
                               && keyVals[keyCh] > keyVals[(keyCh + 2) % 3];

        for (int y = 0; y < h; ++y) {
            const uchar* src = img.constScanLine(y);
            uchar* dst = result.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                const int pr = src[si + 0], pg = src[si + 1], pb = src[si + 2];
                const int dr = pr - kr, dg = pg - kg, db = pb - kb;

                // Distância perceptual ponderada (evita sqrt no loop principal).
                const double distSq = wR * dr * dr + wG * dg * dg + wB * db * db;

                if (distSq >= fullSq) {
                    // Fora da faixa: pixel opaco.
                    dst[si + 0] = pr;
                    dst[si + 1] = pg;
                    dst[si + 2] = pb;
                    dst[si + 3] = 255;
                } else {
                    // Dentro da faixa: alpha com transição suave.
                    const double dist = std::sqrt(distSq);
                    double alpha;
                    if (dist <= coreRange) {
                        alpha = 0.0; // match total → totalmente transparente
                    } else {
                        // Curva smoothstep na borda (mais suave que linear).
                        const double t = (dist - coreRange) / (fullRange - coreRange);
                        const double s = t * t * (3.0 - 2.0 * t); // smoothstep
                        alpha = s * 255.0;
                    }

                    dst[si + 0] = pr;
                    dst[si + 1] = pg;
                    dst[si + 2] = pb;

                    // Supressão de spill: remove o respingo da cor-chave nos
                    // pixels da borda (foreground contaminado pela luz do fundo).
                    if (spill > 0.0 && keyDominates) {
                        const int pv[3] = { pr, pg, pb };
                        const int otherA = pv[(keyCh + 1) % 3];
                        const int otherB = pv[(keyCh + 2) % 3];
                        // Só há respingo onde a cor-chave ainda domina o pixel.
                        // O teste antigo era `pixLum > keyLum`, que descartava
                        // justamente os pixels escuros contaminados (cabelo
                        // preto, sombra) — os casos mais visíveis.
                        if (pv[keyCh] > otherA && pv[keyCh] > otherB) {
                            // Peso em sino: o respingo é visível nos pixels de
                            // borda, meio transparentes. `1 - alpha/255` era
                            // máximo onde o pixel é invisível e zero onde é
                            // opaco — invertido em relação à visibilidade.
                            const double a = alpha / 255.0;
                            const double bell = 4.0 * a * (1.0 - a);
                            const double spillAmt = spill * (0.35 + 0.65 * bell);
                            // Despill clássico: puxa o canal da chave em direção
                            // à média dos outros dois. Com spillAmt = 1 o canal
                            // vira essa média; valores menores corrigem em parte.
                            const int replacement = (otherA + otherB) / 2;
                            const int corrected = (int)std::lround(
                                pv[keyCh] - spillAmt * (pv[keyCh] - replacement));
                            dst[si + keyCh] = (uchar)std::clamp(corrected, 0, 255);
                        }
                    }
                    dst[si + 3] = (uchar)std::clamp((int)std::lround(alpha), 0, 255);
                }
            }
        }
        ImgPool::release(img);
        img = result;
    }

    // Desfoque: box blur separável com sliding window — O(W·H) independente do raio.
    if (c.blur > 0.5) {
        const int radius = std::clamp((int)std::lround(c.blur * 0.5), 1, 30);
        const int w = img.width();
        const int h = img.height();

        // Buffers estáticos por thread (reutilizados entre frames).
        static thread_local std::vector<int> s_rowR, s_rowG, s_rowB, s_rowA;
        if ((int)s_rowR.size() < w) {
            s_rowR.resize(w);
            s_rowG.resize(w);
            s_rowB.resize(w);
            s_rowA.resize(w);
        }

        // Passada horizontal com sliding window: resultado em temp.
        QImage temp(w, h, img.format());
        for (int y = 0; y < h; ++y) {
            const uchar* src = img.constScanLine(y);
            uchar* dst = temp.scanLine(y);

            // Inicializa a soma da janela [0, radius].
            int sr = 0, sg = 0, sb = 0, sa = 0;
            const int initEnd = std::min(radius, w - 1);
            for (int sx = 0; sx <= initEnd; ++sx) {
                const int si = sx * 4;
                sr += src[si]; sg += src[si + 1]; sb += src[si + 2]; sa += src[si + 3];
            }
            int cnt = initEnd + 1;

            for (int x = 0; x < w; ++x) {
                // Escreve o pixel médio.
                const int di = x * 4;
                dst[di]     = (uchar)(sr / cnt);
                dst[di + 1] = (uchar)(sg / cnt);
                dst[di + 2] = (uchar)(sb / cnt);
                dst[di + 3] = (uchar)(sa / cnt);

                // Sliding window: adiciona o pixel que entra, remove o que sai.
                const int addX = x + radius + 1;
                const int remX = x - radius;
                if (addX < w) {
                    const int ai = addX * 4;
                    sr += src[ai]; sg += src[ai + 1]; sb += src[ai + 2]; sa += src[ai + 3];
                    ++cnt;
                }
                if (remX >= 0) {
                    const int ri = remX * 4;
                    sr -= src[ri]; sg -= src[ri + 1]; sb -= src[ri + 2]; sa -= src[ri + 3];
                    --cnt;
                }
            }
        }

        // Passada vertical com sliding window: resultado de volta em img.
        int* rowR = s_rowR.data();
        int* rowG = s_rowG.data();
        int* rowB = s_rowB.data();
        int* rowA = s_rowA.data();

        // Inicializa as somas com a banda [0, radius] para cada coluna.
        {
            const uchar* firstRow = temp.constScanLine(0);
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                rowR[x] = firstRow[si];
                rowG[x] = firstRow[si + 1];
                rowB[x] = firstRow[si + 2];
                rowA[x] = firstRow[si + 3];
            }
        }
        const int initEndV = std::min(radius, h - 1);
        for (int sy = 1; sy <= initEndV; ++sy) {
            const uchar* src = temp.constScanLine(sy);
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                rowR[x] += src[si];
                rowG[x] += src[si + 1];
                rowB[x] += src[si + 2];
                rowA[x] += src[si + 3];
            }
        }
        int cntV = initEndV + 1;

        for (int y = 0; y < h; ++y) {
            uchar* dst = img.scanLine(y);

            // Escreve pixel médio para esta linha.
            for (int x = 0; x < w; ++x) {
                const int di = x * 4;
                dst[di]     = (uchar)(rowR[x] / cntV);
                dst[di + 1] = (uchar)(rowG[x] / cntV);
                dst[di + 2] = (uchar)(rowB[x] / cntV);
                dst[di + 3] = (uchar)(rowA[x] / cntV);
            }

            // Sliding window: adiciona a linha que entra, remove a que sai.
            const int addY = y + radius + 1;
            const int remY = y - radius;
            if (addY < h) {
                const uchar* addRow = temp.constScanLine(addY);
                for (int x = 0; x < w; ++x) {
                    const int si = x * 4;
                    rowR[x] += addRow[si];
                    rowG[x] += addRow[si + 1];
                    rowB[x] += addRow[si + 2];
                    rowA[x] += addRow[si + 3];
                }
                ++cntV;
            }
            if (remY >= 0) {
                const uchar* remRow = temp.constScanLine(remY);
                for (int x = 0; x < w; ++x) {
                    const int si = x * 4;
                    rowR[x] -= remRow[si];
                    rowG[x] -= remRow[si + 1];
                    rowB[x] -= remRow[si + 2];
                    rowA[x] -= remRow[si + 3];
                }
                --cntV;
            }
        }
    }

    // Preto e branco.
    if (c.grayscale) {
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                const int si = x * 4;
                const int gray = (int)std::lround(0.299 * line[si + 2] + 0.587 * line[si + 1] + 0.114 * line[si]);
                line[si + 0] = line[si + 1] = line[si + 2] = (uchar)std::clamp(gray, 0, 255);
            }
        }
    }

    // Brilho, contraste, saturação.
    const bool needEq = (c.brightness != 0.0 || c.contrast != 1.0 || c.saturation != 1.0);
    if (needEq) {
        const double br = std::clamp(c.brightness, -1.0, 1.0) * 255.0;
        const double ct = std::clamp(c.contrast, 0.0, 2.0);
        const double factor = (ct == 1.0) ? 1.0 : (259.0 * (ct * 255.0 + 255.0)) / (255.0 * (259.0 - ct * 255.0));
        const double sa = std::clamp(c.saturation, 0.0, 2.0);
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                const int si = x * 4;
                double r = line[si + 2], g = line[si + 1], b = line[si + 0];
                // Brilho
                r += br; g += br; b += br;
                // Contraste
                r = factor * (r - 128.0) + 128.0;
                g = factor * (g - 128.0) + 128.0;
                b = factor * (b - 128.0) + 128.0;
                // Saturação
                if (sa != 1.0) {
                    const double gray = 0.299 * r + 0.587 * g + 0.114 * b;
                    r = gray + sa * (r - gray);
                    g = gray + sa * (g - gray);
                    b = gray + sa * (b - gray);
                }
                line[si + 2] = (uchar)std::clamp((int)std::lround(r), 0, 255);
                line[si + 1] = (uchar)std::clamp((int)std::lround(g), 0, 255);
                line[si + 0] = (uchar)std::clamp((int)std::lround(b), 0, 255);
            }
        }
    }
// Color grade estilo Lumetri (LGG + exposure/curves/LUT/vignette…).
    // Ordem alinhada ao ProjectExporter (colorgrade::ffmpegFilters).
    if (c.hasColorGrade()) {
        colorgrade::CubeLut lut;
        const bool hasLut = !c.cgLutPath.isEmpty()
            && colorgrade::loadCubeFile(c.cgLutPath, lut);
        colorgrade::applyToImage(img, c, hasLut ? &lut : nullptr);
    }

    // PSX: quantização de cor para profundidade baixa + dithering ordenado
    // (padrão Bayer 4x4) — o "granulado" característico do PlayStation 1.
    // O desempenho é importante (loop por pixel na thread da UI), então só o
    // passo de quantização + o deslocamento determinístico do padrão.
    if (c.psxEnabled) {
        const int bits = std::clamp(c.psxBits, 3, 8);
        const double step = 255.0 / (1 << bits);
        const double dith = std::clamp(c.psxDither, 0.0, 1.0) * step;
        // Bayer 4x4 clássico (0..15) → offset -0.5..0.5 do passo.
        static const int bayer[16] = {
            0, 8, 2, 10,
            12, 4, 14, 6,
            3, 11, 1, 9,
            15, 7, 13, 5
        };
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            const int by = (y & 3) * 4;
            for (int x = 0; x < img.width(); ++x) {
                const int si = x * 4;
                const double off = (bayer[by + (x & 3)] - 7.5) / 15.0 * dith;
                for (int ch = 0; ch < 3; ++ch) {
                    double v = line[si + ch] + off;
                    v = std::lround(std::clamp(v, 0.0, 255.0) / step) * step;
                    line[si + ch] = (uchar)std::clamp((int)std::lround(v), 0, 255);
                }
            }
        }
    }
}

// Aplica pan/crop sobre o quadro cheio (m_frameFull) e guarda em m_frame.
void PreviewWidget::applyCrop() {
    // Memo LAINKA: em stop motion o tempo quantizado é constante por N quadros,
    // mas esta cadeia inteira é cara e síncrona na thread da UI (o warp 8x8 são
    // 64 transforms de tile e o MotiOn desenha o quadro N vezes com
    // SmoothPixmapTransform). Como nada aqui depende do tempo NÃO quantizado —
    // o jitter/flicker/warp usam m_lainkaQuantizedTime e as máscaras usam
    // m_lastSrcT, que também é o tempo quantizado — o resultado de um quadro
    // vale para os N-1 seguintes.
    //
    // Duas exceções, ambas com saída dependente do histórico/relógio:
    //  - onion skin e o MotiOn do LAINKA consomem `prevFrame` (o quadro
    //    anterior), que deixa de ser atualizado enquanto o memo acerta;
    //  - OFX recebe m_playhead cru e pode variar a cada quadro.
    // O crop entra na chave porque os keyframes dele são avaliados no tempo
    // NÃO quantizado: com crop animado o recorte anda dentro da janela de stop
    // motion mesmo com o tempo quantizado parado.
    const bool memoOk = m_clipLainkaEnabled
                        && m_clipOfxFx.isEmpty()
                        && m_clipLainkaOnionSkin < 1e-6
                        && m_clipLainkaMotionBlur < 1e-6;
    if (memoOk && !m_cropMemo.isNull() && m_cropMemoClipId == m_clipLainkaId
        && m_cropMemoTime == m_lainkaQuantizedTime
        && m_cropMemoSrcT == m_lastSrcT
        && m_cropMemoSrcW == m_lastDecodeW
        && m_cropMemoCropL == m_lastCropL && m_cropMemoCropR == m_lastCropR
        && m_cropMemoCropT == m_lastCropT && m_cropMemoCropB == m_lastCropB) {
        m_frame = m_cropMemo;
        return;
    }

    m_frame = applyCropTo(m_frameFull, m_lastCropL, m_lastCropR, m_lastCropT, m_lastCropB);
    // Aplica efeito LAINKA (stop motion) após o crop.
    if (m_clipLainkaEnabled) {
        QImage prevFrame = m_lainkaPrevFrame;
        m_lainkaPrevFrame = m_frame;
        // Usa tempo quantizado para que jitter/flicker/warp fiquem
        // congelados no mesmo intervalo de stop motion.
        const double fxT = (m_lainkaQuantizedTime >= 0.0)
            ? m_lainkaQuantizedTime : m_playhead;
        m_frame = lainkaApplyFx(m_frame, m_clipLainkaId, fxT,
                                m_clipLainkaSkip, m_clipLainkaJitterPos,
                                m_clipLainkaJitterRot, m_clipLainkaJitterScale,
                                m_clipLainkaFlicker, m_clipLainkaFlickerSpeed,
                                m_clipLainkaWarpAmount, m_clipLainkaWarpSpeed,
                                m_clipLainkaWarpGrid, m_clipLainkaOnionSkin,
                                m_clipLainkaDustAmount, m_clipLainkaScratchAmount,
                                m_clipLainkaMotionBlur, m_clipLainkaOpacity,
                                m_clipLainkaTargetFps, m_clipLainkaAntialias,
                                prevFrame);
    }
    if (m_clipMotionEnabled && m_clipMotionAmount > 0.0 && !m_frame.isNull()) {
        const double rad = m_clipMotionAngle * M_PI / 180.0;
        const double dist = m_clipMotionAmount * 0.4;
        const int sx = (int)std::lround(std::cos(rad) * dist);
        const int sy = (int)std::lround(std::sin(rad) * dist);
        const int ns = std::clamp(m_clipMotionSamples, 1, 32);
        QImage result = ImgPool::get(m_frame.format(), m_frame.width(), m_frame.height());
        {
            QPainter p(&result);
            p.drawImage(0, 0, m_frame);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            for (int i = 1; i < ns; ++i) {
                const double alpha = 1.0 - (double)i / ns;
                p.setOpacity(alpha / ns);
                p.drawImage(sx * i, sy * i, m_frame);
            }
        }
        ImgPool::release(m_frame);
        m_frame = result;
    }
    // Aplica efeitos básicos (brilho, contraste, saturação, etc.)
    applyBasicEffects(m_frame);

    // Aplica efeitos OFX do clipe.
    if (!m_clipOfxFx.isEmpty() && m_ofxManager && !m_frame.isNull()) {
        qInfo() << "[OFX] Aplicando" << m_clipOfxFx.size() << "efeito(s) OFX no preview"
                << "- efeitos:" << [this]() {
                    QStringList ids;
                    for (const auto& fx : m_clipOfxFx) ids << fx.pluginId;
                    return ids.join(", ");
                }();
        m_frame = OfxRenderer::applyOfxEffects(m_frame, m_clipOfxFx, m_ofxManager,
                                                m_playhead);
    }
    // Efeitos frei0r (padrão Kdenlive/Shotcut) — depois dos OFX; export usa
    // o mesmo filtro ffmpeg `frei0r=` (paridade preview↔export).
    if (!m_clipFrei0rFx.isEmpty() && m_frei0rManager && !m_frame.isNull()) {
        double f0rTime = m_playhead;
        if (m_project && !m_clipLainkaId.isEmpty()) {
            for (const Track& tr : m_project->videoTracks) {
                for (const Clip& c : tr.clips) {
                    if (c.id == m_clipLainkaId) {
                        f0rTime = m_playhead - c.pos;
                        break;
                    }
                }
            }
        }
        m_frame = Frei0rPluginManager::applyEffects(m_frame, m_clipFrei0rFx,
                                                    m_frei0rManager, f0rTime);
    }

    if (memoOk) {
        m_cropMemo = m_frame;
        m_cropMemoClipId = m_clipLainkaId;
        m_cropMemoTime = m_lainkaQuantizedTime;
        m_cropMemoSrcT = m_lastSrcT;
        m_cropMemoSrcW = m_lastDecodeW;
        m_cropMemoCropL = m_lastCropL;
        m_cropMemoCropR = m_lastCropR;
        m_cropMemoCropT = m_lastCropT;
        m_cropMemoCropB = m_lastCropB;
    } else if (!m_cropMemo.isNull()) {
        // Fora do LAINKA (ou com OFX/onion skin/MotiOn temporal) não há acerto
        // possível, então libera a imagem em vez de segurar ~8 MB à toa — o que
        // o comentário do membro promete.
        m_cropMemo = QImage();
    }
}

void PreviewWidget::drawGrid(QPainter& p, const QRect& canvas) {
    if (!m_showGrid || canvas.width() < 2 || canvas.height() < 2) return;

    p.save();
    p.setClipRect(canvas);
    p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::SolidLine));

    // Margens de segurança do Premiere: dois retângulos concêntricos —
    // "Action Safe" a 90% e "Title Safe" a 80% do quadro, com os cantos
    // marcados (é assim que o Premiere as desenha, não uma grade NxN).
    const auto insetRect = [&](double frac) {
        const double w = canvas.width() * frac;
        const double h = canvas.height() * frac;
        return QRectF(canvas.center().x() - w / 2.0, canvas.center().y() - h / 2.0,
                      w, h);
    };
    const auto cornerTicks = [&](const QRectF& r, int len) {
        const double x0 = r.left(), x1 = r.right(), y0 = r.top(), y1 = r.bottom();
        p.drawLine(QPointF(x0, y0), QPointF(x0 + len, y0));
        p.drawLine(QPointF(x0, y0), QPointF(x0, y0 + len));
        p.drawLine(QPointF(x1, y0), QPointF(x1 - len, y0));
        p.drawLine(QPointF(x1, y0), QPointF(x1, y0 + len));
        p.drawLine(QPointF(x0, y1), QPointF(x0 + len, y1));
        p.drawLine(QPointF(x0, y1), QPointF(x0, y1 - len));
        p.drawLine(QPointF(x1, y1), QPointF(x1 - len, y1));
        p.drawLine(QPointF(x1, y1), QPointF(x1, y1 - len));
    };

    const QRectF action = insetRect(0.90);
    const QRectF title = insetRect(0.80);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::SolidLine));
    p.drawRect(action);
    p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::SolidLine));
    p.drawRect(title);
    cornerTicks(action, 10);
    cornerTicks(title, 8);

    p.restore();
}

#include "PreviewWidget.moc"

// ════════════════════════════════════════════════════════════════════════
// Overlay de edição de máscara: desenha as formas do clipe sobre o monitor
// com alças arrastáveis (mover centro, redimensionar bordas, rotacionar) e
// repassa as edições via sinal. O usuário arrasta direto no preview, como no
// PanCrop/Transform do Vegas — só é aplicado ao clipe quando o dialog salva.
// ════════════════════════════════════════════════════════════════════════
namespace {

const Clip* previewFindClip(const Project* p, const QString& id) {
    if (!p || id.isEmpty()) return nullptr;
    for (const Track& t : p->videoTracks)
        for (const Clip& c : t.clips)
            if (c.id == id) return &c;
    for (const Track& t : p->audioTracks)
        for (const Clip& c : t.clips)
            if (c.id == id) return &c;
    return nullptr;
}

// Dimensões do quadro onde a máscara vive (o frame já cortado). Para o clipe
// do topo usa m_frame; senão deriva da mídia + pan/crop (mesmo espaço do
// applyMasks). Só a proporção importa para o overlay.
QSize previewMaskAnchorSize(const Project* p, const Clip* c,
                            const QImage& topFrame, const QString& topId) {
    if (c && c->id == topId && !topFrame.isNull())
        return topFrame.size();
    const MediaItem* m = c && p ? p->findMedia(c->mediaId) : nullptr;
    int w = m ? m->width : 0, h = m ? m->height : 0;
    if (w <= 0 || h <= 0) { w = p ? p->width : 1920; h = p ? p->height : 1080; }
    const double cl = std::clamp(c ? c->cropL : 0.0, 0.0, 1000.0);
    const double cr = std::clamp(c ? c->cropR : 0.0, 0.0, 1000.0);
    const double ct = std::clamp(c ? c->cropT : 0.0, 0.0, 1000.0);
    const double cb = std::clamp(c ? c->cropB : 0.0, 0.0, 1000.0);
    const int cw = qMax(1, (int)std::lround(w * (1.0 - (cl + cr) / 1000.0)));
    const int ch = qMax(1, (int)std::lround(h * (1.0 - (ct + cb) / 1000.0)));
    return QSize(cw, ch);
}

// Mesma aritmética da drawLayer (paintEvent): imagem do clipe → monitor.
QTransform previewLayerToScreen(const Project* p, const Clip& c,
                                const QSize& imgSize, double rel,
                                double k, const QPointF& center) {
    QTransform t;
    const double W = p ? p->width : 1920.0;
    const double H = p ? p->height : 1080.0;
    const double iw = qMax(1, imgSize.width());
    const double ih = qMax(1, imgSize.height());
    const double fit = qMin(W / iw, H / ih);
    const double s  = kfValue(c.kfScale, c.scale, rel);
    const double sX = kfValue(c.kfScaleX, c.scaleX, rel);
    const double sY = kfValue(c.kfScaleY, c.scaleY, rel);
    const double rot = kfValue(c.kfRotation, c.rotation, rel);
    const double tx = kfValue(c.kfTx, c.tx, rel);
    const double ty = kfValue(c.kfTy, c.ty, rel);
    t.translate(center.x() + tx * k, center.y() + ty * k);
    t.rotate(rot);
    t.scale(k * fit * s * sX, k * fit * s * sY);
    t.translate(-iw / 2.0, -ih / 2.0);
    return t;
}

// Controles da máscara em pixels do espaço da imagem:
// pts[0]=centro, pts[1]=alça de rotação, pts[2..5]=extremos (E, topo, O, baixo).
struct MaskCtrl {
    QVector<QPointF> pts;
};
MaskCtrl previewMaskCtrl(const Mask& m, const QSize& size, double rel) {
    MaskCtrl o;
    const double W = size.width(), H = size.height();
    const double cx = m.cxAt(rel) * W, cy = m.cyAt(rel) * H;
    const double rx = std::max(0.0, m.rxAt(rel)) * W;
    const double ry = std::max(0.0, m.ryAt(rel)) * H;
    const double rad = m.rotAt(rel) * M_PI / 180.0;
    QVector<QPointF> base;
    if (m.type == QLatin1String("rect")) {
        base = { QPointF(cx - rx, cy - ry), QPointF(cx + rx, cy - ry),
                 QPointF(cx + rx, cy + ry), QPointF(cx - rx, cy + ry) };
    } else { // ellipse: extremos cardeais
        base = { QPointF(cx - rx, cy), QPointF(cx, cy - ry),
                 QPointF(cx + rx, cy), QPointF(cx, cy + ry) };
    }
    if (rad != 0.0) {
        const double cs = std::cos(rad), sn = std::sin(rad);
        for (QPointF& b : base) {
            const double dx = b.x() - cx, dy = b.y() - cy;
            b = QPointF(cx + dx * cs - dy * sn, cy + dx * sn + dy * cs);
        }
    }
    const double arm = qMax(1.0, std::min(rx, ry));
    o.pts = { QPointF(cx, cy),
              QPointF(cx - arm * std::sin(rad), cy - arm * std::cos(rad)) };
    for (const QPointF& b : base) o.pts.append(b);
    return o;
}

void maskHandleSquare(QPainter& p, const QPointF& pt, const QColor& accent) {
    const double hs = 4.5;
    p.setBrush(Qt::white);
    p.setPen(QPen(accent, 1.4));
    p.drawRect(QRectF(pt.x() - hs, pt.y() - hs, 2.0 * hs, 2.0 * hs));
}

} // namespace

void PreviewWidget::setMaskOverlay(const QString& clipId, const QVector<Mask>& masks) {
    m_maskOverlayClipId = clipId;
    m_maskOverlay = masks;
    m_maskDragIndex = -1;
    m_maskDragHandle = -1;
    update();
}

void PreviewWidget::drawMaskOverlay(QPainter& p, const QRect& canvas, double k) {
    if (m_maskOverlayClipId.isEmpty() || !m_project || m_maskOverlay.isEmpty()) return;
    const Clip* clip = previewFindClip(m_project, m_maskOverlayClipId);
    if (!clip) { setMaskOverlay(QString(), {}); return; }
    const double rel = m_playhead - clip->pos;
    if (m_playhead < clip->pos - 1e-6 || m_playhead > clip->pos + clip->dur + 1e-6) return;
    const QSize imgSize = previewMaskAnchorSize(m_project, clip, m_frame, m_maskOverlayClipId);
    if (imgSize.isEmpty()) return;
    const QPointF center(canvas.center());
    m_maskToScreen = previewLayerToScreen(m_project, *clip, imgSize, rel, k, center);
    m_maskAnchorSize = imgSize;

    const QColor accent = themeColors().accent;
    const QColor band(130, 215, 255); // contorno vivo, visível sobre qualquer mídia
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_maskOverlay.size(); ++i) {
        const Mask& m = m_maskOverlay[i];
        if (m.type.isEmpty()) continue;
        const double cx = m.cxAt(rel) * imgSize.width();
        const double cy = m.cyAt(rel) * imgSize.height();
        const double rx = std::max(0.001, m.rxAt(rel)) * imgSize.width();
        const double ry = std::max(0.001, m.ryAt(rel)) * imgSize.height();
        const double rot = m.rotAt(rel);
        QPainterPath path;
        if (m.type == QLatin1String("rect"))
            path.addRect(QRectF(cx - rx, cy - ry, 2.0 * rx, 2.0 * ry));
        else if (m.type == QLatin1String("ellipse"))
            path.addEllipse(QPointF(cx, cy), rx, ry);
        else continue;
        if (rot != 0.0) {
            QTransform r; r.translate(cx, cy); r.rotate(rot); r.translate(-cx, -cy);
            path = r.map(path);
        }
        const bool en = m.hasMask();
        p.setPen(QPen(en ? band : QColor(band.red(), band.green(), band.blue(), 110),
                       1.6, Qt::DashLine));
        p.setBrush(en ? QColor(accent.red(), accent.green(), accent.blue(), 34)
                      : Qt::NoBrush);
        p.drawPath(m_maskToScreen.map(path));
        if (!en) continue;

        const MaskCtrl ctl = previewMaskCtrl(m, imgSize, rel);
        const QPointF c0 = m_maskToScreen.map(ctl.pts[0]);
        const QPointF c1 = m_maskToScreen.map(ctl.pts[1]);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.2));
        p.drawLine(c0, c1);
        maskHandleSquare(p, c1, accent);
        maskHandleSquare(p, c0, accent);
        for (int b = 2; b <= 5; ++b)
            maskHandleSquare(p, m_maskToScreen.map(ctl.pts[b]), accent);
    }
    p.restore();
}

bool PreviewWidget::pickMaskHandle(const QPoint& pos) {
    if (m_maskOverlayClipId.isEmpty() || !m_project) return false;
    const Clip* clip = previewFindClip(m_project, m_maskOverlayClipId);
    if (!clip) return false;
    const double rel = m_playhead - clip->pos;
    if (m_playhead < clip->pos - 1e-6 || m_playhead > clip->pos + clip->dur + 1e-6)
        return false;
    const QSize imgSize = previewMaskAnchorSize(m_project, clip, m_frame, m_maskOverlayClipId);
    if (imgSize.isEmpty()) return false;
    // Reconstrói canvas/escala como no paintEvent (para as alças baterem).
    const QRect work = m_videoRect.adjusted(12, 12, -12, -12);
    double k = m_zoom > 0.0 ? m_zoom
                            : qMin(work.width() / double(m_project->width),
                                   work.height() / double(m_project->height));
    QRect canvas(QPoint(0, 0), QSize(qMax(1, (int)(m_project->width * k)),
                                     qMax(1, (int)(m_project->height * k))));
    canvas.moveCenter(work.center());
    canvas = canvas.intersected(work);
    const QPointF center(canvas.center());
    m_maskToScreen = previewLayerToScreen(m_project, *clip, imgSize, rel, k, center);
    m_maskAnchorSize = imgSize;
    const QTransform imgToScreen = m_maskToScreen;
    const double tol = 9.0; // px de tolerância das alças
    for (int i = 0; i < m_maskOverlay.size(); ++i) {
        const Mask& m = m_maskOverlay[i];
        if (m.type.isEmpty() || !m.hasMask()) continue;
        const MaskCtrl ctl = previewMaskCtrl(m, imgSize, rel);
        int best = -1;
        double bestD = tol * tol;
        for (int h = 0; h < ctl.pts.size(); ++h) {
            const QPointF sp = imgToScreen.map(ctl.pts[h]);
            const double dx = sp.x() - pos.x(), dy = sp.y() - pos.y();
            const double d2 = dx * dx + dy * dy;
            if (d2 < bestD) { bestD = d2; best = h; }
        }
        if (best >= 0) {
            m_maskDragIndex = i;
            m_maskDragHandle = best;
            m_maskDragLast = pos;
            m_maskDragPressRot = m.rotAt(rel);
            const QPointF cur = m_maskToScreen.inverted().map(QPointF(pos));
            m_maskDragPressAng = std::atan2(cur.y() - m.cyAt(rel) * imgSize.height(),
                                            cur.x() - m.cxAt(rel) * imgSize.width());
            return true;
        }
    }
    return false;
}

void PreviewWidget::applyMaskDrag(const QPoint& pos) {
    if (m_maskDragIndex < 0 || m_maskDragIndex >= m_maskOverlay.size()) return;
    Mask& m = m_maskOverlay[m_maskDragIndex];
    const double iw = m_maskAnchorSize.width(), ih = m_maskAnchorSize.height();
    if (iw < 1 || ih < 1) return;
    const QPointF cur = m_maskToScreen.inverted().map(QPointF(pos));
    const double cx = m.cxAt(0.0) * iw, cy = m.cyAt(0.0) * ih;
    if (m_maskDragHandle == 0) {
        // Mover: o centro acompanha o cursor.
        m.cx = std::clamp(cur.x() / iw, 0.0, 1.0);
        m.cy = std::clamp(cur.y() / ih, 0.0, 1.0);
    } else if (m_maskDragHandle == 1) {
        // Rotação: ângulo do cursor ao redor do centro (contínuo).
        const double ang = std::atan2(cur.y() - cy, cur.x() - cx);
        double d = ang - m_maskDragPressAng;
        while (d > M_PI) d -= 2.0 * M_PI;
        while (d < -M_PI) d += 2.0 * M_PI;
        double rot = m_maskDragPressRot + d * 180.0 / M_PI;
        while (rot > 180.0) rot -= 360.0;
        while (rot < -180.0) rot += 360.0;
        m.rotation = rot;
    } else {
        // Borda: raio = distância do centro projetada nos eixos da forma.
        const double dx = cur.x() - cx, dy = cur.y() - cy;
        const double rad = m.rotAt(0.0) * M_PI / 180.0;
        const double lx =  dx * std::cos(rad) + dy * std::sin(rad);
        const double ly = -dx * std::sin(rad) + dy * std::cos(rad);
        if (std::abs(lx) > 1.0) m.rx = qBound(0.001, std::abs(lx) / iw, 1.5);
        if (std::abs(ly) > 1.0) m.ry = qBound(0.001, std::abs(ly) / ih, 1.5);
    }
    m_maskDragLast = pos;
    emit maskEdited(m_maskDragIndex, m);
    update();
}

void PreviewWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && pickMaskHandle(e->pos())) {
        update();
        return;
    }
    QWidget::mousePressEvent(e);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_maskDragIndex >= 0) {
        applyMaskDrag(e->pos());
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (m_maskDragIndex >= 0) {
        m_maskDragIndex = -1;
        m_maskDragHandle = -1;
        emit maskDragEnd();
        update();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}
