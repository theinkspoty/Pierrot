// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

// Worker de decode do preview: decodifica quadros de vídeo na própria thread
// (com acesso compartilhado ao SourcePool: um para o clipe principal, e o
// prefetch aquecendo o pool para o swap). O PreviewWidget pede o "último"
// quadro desejado e descarta intermediários. Extraído de PreviewWidget.cpp (F4).

#include <QObject>
#include <QString>
#include <QImage>
#include <QElapsedTimer>
#include <atomic>
#include <cmath>
#include <memory>

#include <QDebug>

#include "colombina/CrashReporter.h"
#include "laartman/FFmpegDecoder.h"
#include "colombina/ffmpeg/SourcePool.h"
#include "colombina/ffmpeg/FrameCache.h"
#include "ui/monitor/PreviewDebug.h"
#include "ui/monitor/PreviewWidget.h" // FrameFx + estáticos de efeito

class OfxPluginManager;
class Frei0rPluginManager;

// Decode com cache L4: consulta o FrameCache antes de decodificar (Fase 6). O
// núcleo não guarda cache; a chave é (índice de tempo do decoder, largura).
// Quebra o laço de re-decode em scrub/replay sem tocar no núcleo.
inline QImage decodeCached(FFmpegDecoder* dec, double t, int maxW,
                           const std::atomic<bool>* cancel) {
    if (!dec) return QImage();
    FrameCache& cache = FrameCache::instance();
    const FrameCache::Key key{qHash(dec->source()), dec->frameCacheIndex(t), maxW};
    return cache.getOrCompute(key, [&] { return dec->frameAt(t, maxW, cancel); });
}

// Decodifica quadros de vídeo na própria thread (acesso compartilhado ao
// SourcePool: um para o clipe principal, e o prefetch aquecendo o pool para o
// swap). O PreviewWidget pede o "último" quadro desejado e descarta
// intermediários.
class FrameWorker : public QObject {
    Q_OBJECT
public:
    explicit FrameWorker(SourcePool* pool, QObject* parent = nullptr)
        : QObject(parent), m_pool(pool) {}

    // Managers de efeitos (lidados pela UI; o worker só lê durante o decode).
    void setEffectManagers(OfxPluginManager* ofx, Frei0rPluginManager* f0r) {
        m_ofxMgr = ofx;
        m_frei0rMgr = f0r;
    }

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
            // Acesso compartilhado ao SourcePool: pega o decoder quente deste
            // arquivo (o prefetch em background deixou o próximo clipe já
            // aberto/aquecido na posição do corte). acquire() nunca devolve
            // nullptr por "ocupado" — só se o open() falhar.
            std::shared_ptr<FFmpegDecoder> d = m_pool->acquire(path);
            if (!d) {
                qWarning().noquote() << QStringLiteral("[dec] open falhou para %1").arg(path);
                emit frameReady(clipId, path, t, maxW, QImage(), false);
                return;
            }
            m_decoder = d;
            m_curPath = path;
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
            img = decodeCached(m_decoder.get(), t, maxW, &m_cancel);
        }
        if (dbgOn) {
            const double dbgMs = (dbgClock.nsecsElapsed() - dbgT0) / 1000000.0;
            if (dbgMs > 3.0 || dbgWasReady)
                qDebug().noquote() << QStringLiteral("[w] t=%1 %2 %3ms")
                          .arg(t, 0, 'f', 3)
                          .arg(dbgWasReady ? QStringLiteral("ready") : QStringLiteral("dec"), 6)
                          .arg(dbgMs, 0, 'f', 1);
        }
        // Caminho rápido: crop + efeitos básicos/máscaras + MotiOn/OFX/frei0r
        // aplicados AQUI, na thread de vídeo — o C2 (copy do applyCropTo), o C3
        // (per-pixel dos básicos) e a cadeia temporal saem do tiquinho da UI.
        // LAINKA continua fora (fx.active=false): onion skin/motion blur dependem
        // do quadro anterior mantido na UI.
        const bool fxApplied = fx.active && !img.isNull();
        if (fxApplied) {
            img = PreviewWidget::applyCropTo(img, fx.cropL, fx.cropR, fx.cropT, fx.cropB);
            PreviewWidget::applyBasicEffectsOn(img, fx.clip, fx.rel);
            PreviewWidget::applyTemporalFxOn(img, fx.clip, fx.absTime, fx.rel,
                                             m_ofxMgr, m_frei0rMgr);
        }
        emit frameReady(clipId, path, t, maxW, img, fxApplied);

        // Decodifica o próximo frame na folga para o próximo pedido (apenas se
        // o decoder ainda estiver no mesmo arquivo).
        if (!img.isNull() && m_decoder && m_decoder->isOpen() && m_decoder->source() == path) {
            m_readyImg = decodeCached(m_decoder.get(), t + step, maxW, &m_cancel);
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
        // Mesmo arquivo do topo: reusa o decoder já aberto (decodePrefetch roda
        // na MESMA thread do decodeOne, seriado) em vez de chamar acquire() —
        // assim não há open() frio (500-800ms) no caminho da camada de baixo
        // quando ela compartilha o arquivo do topo. Para outro arquivo, o
        // SourcePool devolve o decoder já aquecido pelo FramePipeline (warm em
        // background); acquire() só abre de fato se o warm ainda não chegou.
        std::shared_ptr<FFmpegDecoder> d =
            (path == m_curPath && m_decoder) ? m_decoder : m_pool->acquire(path);
        if (!d) {
            emit prefetchReady(path, t, maxW, QImage());
            return;
        }
        const QImage img = decodeCached(d.get(), t, maxW, nullptr);
        if (!img.isNull() && step > 0.0) decodeCached(d.get(), t + step, maxW, nullptr);
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
        // re-seek frio fica só no pre-decode seguinte.
        if (m_decoder && m_decoder->isOpen()) m_decoder->releaseBuffers();
    }

signals:
    void frameReady(const QString& clipId, const QString& path, double t, int maxW, const QImage& img,
                    bool processed);
    void prefetchReady(const QString& path, double t, int maxW, const QImage& img);

private:
    SourcePool* m_pool;                          // não dono (vida do PreviewWidget)
    std::shared_ptr<FFmpegDecoder> m_decoder;    // decoder do arquivo atual (compartilhado)
    QString m_curPath;                           // caminho do decoder atual
    OfxPluginManager* m_ofxMgr = nullptr;      // não dono
    Frei0rPluginManager* m_frei0rMgr = nullptr; // não dono

    // Frame decodificado adiante (pipeline de 1 frame à frente).
    QImage m_readyImg;
    QString m_readyPath;
    double m_readyT = -1.0;
    int m_readyMaxW = 0;
    bool m_readyValid = false;

    std::atomic<bool> m_cancel{false};       // pedido de cancelamento pendente
    std::atomic<bool> m_cancelArmed{false};  // true somente durante decodeOne
};
