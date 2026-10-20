// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QString>
#include <QVector>

class SourcePool;
class QThread;

// ── L4 · FramePipeline — lookahead/prefetch (a política que saiu do worker) ─
//
// O núcleo (Laartman) decodifica um frame num tempo e pronto. TODA a decisão de
// "abrir um arquivo à frente, posicionar o decoder, aquecer a vizinhança do
// corte" é política e mora aqui, em L4 — não no FrameWorker nem no
// PreviewWidget.
//
// O FramePipeline é dono de:
//   • a thread de prefetch (uma só, fora da thread de decode principal);
//   • a fila de "aquecimento à frente" (abrir+posicionar clipes que ainda vão
//     entrar) e o estado in-flight;
//   • o flag de ocupado do prefetch de frame0.
//
// Ele NÃO conhece o modelo (Project, clipes, tracks): quem decide QUAIS
// caminhos/tempos aquecer é a UI (L6), que chama os métodos abaixo com
// caminhos já resolvidos. Isso mantém o pipeline testável sem um Project.
//
// Posse: o pipeline usa o SourcePool por referência (não é dono). Não há lease
// — o prefetch só aquece entradas; o decode principal as reusa por acesso
// compartilhado.

class FramePipeline : public QObject {
    Q_OBJECT
public:
    // O pool precisa viver mais que o pipeline (é o singleton global do
    // processo). parent opcional (dono Qt).
    explicit FramePipeline(SourcePool* pool, QObject* parent = nullptr);
    ~FramePipeline() override;

    // Inicia a thread interna de prefetch. Sem isto, os pedidos são enfileirados
    // e não executados (útil em testes que rodam síncronos com runNow).
    void start();
    // Para a thread e espera o fim (chamar antes de destruir/fechar a janela).
    void stop();

    // ── Comandos (thread da UI) ────────────────────────────────────────────

    // Prefetch do frame0 do PRÓXIMO clipe (o que entra no corte): decodifica
    // `t` e `t+step` e devolve via framePrefetched. Best-effort e serial: se já
    // houver um em andamento, o pedido é ignorado (o chamador re-tenta no corte).
    void prefetchFrame(const QString& path, double t, int maxW, double step);

    // Substitui a fila de aquecimento à frente. Cada item é um clipe futuro que
    // vale abrir+posicionar antes do corte. Entra um por vez na thread de
    // prefetch (nunca compete com o frame0, que é despachado antes).
    struct WarmTarget { QString path; double t = 0.0; int maxW = 0; };
    void setWarmQueue(const QVector<WarmTarget>& targets);

    // Aquece um único alvo AGORA, sem passar pela fila (playhead parado). Se um
    // warm já estiver em andamento, é ignorado (best-effort).
    void warmNow(const QString& path, double t, int maxW);

    // Estado consultável pela UI (evita reaquecer o que já está quente).
    bool prefetchBusy() const { return m_prefetchBusy; }
    bool warmInFlight(const QString& path) const;
    bool warmQueueEmpty() const;

    // Executa um pedido de prefetch de frame0 de forma SÍNCRONA na thread
    // chamadora. Só para testes — a produção usa a thread interna.
    void runPrefetchNow(const QString& path, double t, int maxW, double step,
                        QImage* frame0, QImage* frame1);

signals:
    // frame0 do próximo clipe pronto (ou nulo, se o open falhou).
    void framePrefetched(const QString& path, double t, int maxW,
                         const QImage& frame0, const QImage& frame1);
    void prefetchFailed(const QString& path);
    // Um alvo da fila terminou de aquecer (o próximo é despachado).
    void warmed(const QString& path);

private:
    void dispatchNextWarm(); // pega m_frameMutex, despacha o próximo da fila

    SourcePool* m_pool;      // não dono
    QThread* m_thread = nullptr;
    QObject* m_worker = nullptr; // prefetch (frame0 do corte), na m_thread
    // Thread própria do warm-ahead: um open() frio no warm não pode atrasar o
    // prefetch crítico do próximo corte. Ver FramePipeline.cpp.
    QThread* m_warmThread = nullptr;
    QObject* m_warmWorker = nullptr;

    // Estado da fila de warm (protegido por m_mutex; lido pela UI).
    mutable QMutex m_mutex;
    QVector<WarmTarget> m_warmQueue;
    QString m_warmInFlight;
    bool m_prefetchBusy = false;
    bool m_started = false;
};
