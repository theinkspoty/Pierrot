// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

// Worker de decode do preview: decodifica quadros de vídeo na própria thread
// (com lease do DecoderPool: um para o clipe principal, e o prefetch aquecendo
// o pool para o swap). O PreviewWidget pede o "último" quadro desejado e
// descarta intermediários. Extraído de PreviewWidget.cpp (F4).

#include <QObject>
#include <QString>
#include <QImage>
#include <QElapsedTimer>
#include <atomic>
#include <cmath>

#include <QDebug>

#include "colombina/CrashReporter.h"
#include "laartman/FFmpegDecoder.h"
#include "laartman/DecoderPool.h"
#include "ui/monitor/PreviewDebug.h"
#include "ui/monitor/PreviewWidget.h" // FrameFx + estáticos de efeito

class OfxPluginManager;
class Frei0rPluginManager;

// Decodifica quadros de vídeo na própria thread (com lease do DecoderPool: um
// para o clipe principal, e o prefetch aquecendo o pool para o swap).
// O PreviewWidget pede o "último" quadro desejado e descarta intermediários.
class FrameWorker : public QObject {
    Q_OBJECT
public:
    explicit FrameWorker(DecoderPool* pool, QObject* parent = nullptr)
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
