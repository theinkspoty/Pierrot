// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QMutex>
#include <QString>
#include <QVector>

// Coletor de métricas da reprodução do preview.
//
// Substitui o `PerfDbg` que vivia dentro do PreviewWidget: aquele só guardava o
// último valor de cada estágio — e o QElapsedTimer nunca era startado, então
// workerMs/prefetchLatMs ficavam em 0 para sempre. Aqui cada quadro vira um
// registro completo, o que permite comparar distribuições (e não um número
// solitário) entre proxy e original.
//
// Toda a coleta acontece na thread da UI (a latência do worker já é medida em
// onFrameReady, que roda nela), então o custo é um push_back sem disputa real.
// Quando desligado, active() é false e cada call site vira uma branch previsível.
class PreviewProfiler {
public:
    struct Frame {
        qint64 seq = 0;           // nº do quadro observado (contador monotônico)
        double playhead = 0.0;    // posição na timeline (s)
        double fps = 0.0;         // fps do projeto
        double tickNs = 0.0;      // tick inteiro (relógio + applySeek + prefetch + mix)
        double clockNs = 0.0;     // só o cálculo do relógio/frame-alvo dentro do tick
        double seekNs = 0.0;      // applySeekVisual -> updateFrame
        double prefetchNs = 0.0;  // onPrefetch
        double mixNs = 0.0;       // onMixAudio
        double workerNs = 0.0;    // kickFrameWorker -> onFrameReady (latência do decode)
        double prefetchLatNs = 0.0; // pedido -> onPrefetchReady
        double paintNs = 0.0;     // paintEvent inteiro
        double compositeNs = 0.0; // etapa de composição dentro do paint
        double resolveNs = 0.0;   // tempo gasto resolvendo proxy vs original
        int dropped = 0;          // DELTA deste tick; a soma da série == total
        qint64 droppedTotal = 0;  // acumulado monotônico, amostrado a cada tick
        quint64 cacheHits = 0;     // LRU do decoder evitou este decode
        quint64 cacheMisses = 0;
        // Indice do QUADRO exibido neste tick. `seq` e contador de ticks e
        // sempre anda de 1 em 1: usá-lo para checar perda de quadro nao prova
        // nada — foi assim que uma validacao anterior deu falso negativo.
        qint64 decSeekNs = 0;      // seek+flush dentro de frameAt (thread do decoder)
        quint64 decDiscard = 0;
        quint64 decWorkNs = 0;     // tempo real dentro de decodeOne    // quadros decodificados e descartados no mesmo frameAt      // seek+flush dentro de frameAt (thread do decoder)
        qint64 frameIdx = -1;
        int skipped = 0;          // frames que o playhead realmente pulou (avanço-1)
        // Atraso do tick em relação ao agendamento: quanto a thread principal
        // demorou A MAIS do que o intervalo do timer. O tick em si é rápido,
        // então é aqui que aparece qualquer bloqueio (paint pesado, I/O,
        // decodificação síncrona) — a métrica que explica "quadro perdido".
        // Zerado em condições normais, mesmo com o timer ocioso entre ticks.
        double stallNs = 0.0;
        qint64 wallMs = 0;        // epoch do início do tick (para correlacionar com disco/log)
        int queue = 0;            // pedidos pendentes no worker
        int layers = 0;           // camadas compostas no paint
        int lowerReq = 0;         // camadas inferiores enfileiradas neste tick
        bool proxy = false;       // o quadro veio de um proxy?
        bool prefetchHit = false; // o prefetch atendeu o tick?
        bool compositeHit = false;    // cache de composição reutilizado?
        bool compositeEvaluated = false; // este quadro chegou ao caminho de composição?
        bool cut = false;         // o playhead cruzou para outro clipe?
    };

    static PreviewProfiler& instance();

    // ativo se PIERROT_PERF_DEBUG (overlay) ou PIERROT_PERF_JSON (dump) existir
    static bool active();

    // Relógio monotônico em ns — barato, use-o para medir qualquer trecho.
    static qint64 nowNs();

    // Marcador de escopo que acumula em um dos slots acima. Uso:
    //   PreviewProfiler::Scope s(&f.seekNs);
    // Quando active() é false o call site nem chega a criar isto.
    class Scope {
    public:
        explicit Scope(double* slot) : m_slot(slot), m_t0(nowNs()) {}
        ~Scope() { if (m_slot) *m_slot += nowNs() - m_t0; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        double* m_slot;
        qint64 m_t0;
    };

    // Zera contadores e inicia a sessão (chamado no início de cada run).
    void beginSession(const QString& label, bool proxyWanted, double projectFps);
    void add(const Frame& f);
    void endSession();

    // Estatísticas de um campo (ns -> ms): p50/p95/p99/max/média.
    struct Stats {
        int n = 0;
        double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0;
    };
    // `field` é o nome do membro de Frame ("tickNs", "workerNs", ...).
    Stats stats(const QString& field) const;

    int frameCount() const;
    qint64 droppedTotal() const;
    bool wroteProxy() const;     // algum quadro veio de proxy?
    int proxyFrames() const;
    int cuts() const;
    int compositeHits() const;
    int compositeMisses() const;
    double playbackSpan() const; // tempo coberto pelos registros (s)
    QString label() const;
    double projectFps() const;

    QString toJson() const;
    // grava em `path` (trunca). Retorna false se não conseguiu escrever.
    bool dumpJson(const QString& path) const;

private:
    PreviewProfiler() = default;
    mutable QMutex m_mutex;
    QVector<Frame> m_frames;
    QString m_label;
    bool m_proxyWanted = true;
    double m_projectFps = 0.0;
    qint64 m_proxyFrames = 0;
    qint64 m_cuts = 0;
    qint64 m_compHits = 0;
    qint64 m_compMiss = 0;
    double m_stallMs = 0.0;     // bloqueio do loop no tick corrente
    double m_stallMaxMs = 0.0;
    double m_stallTotalMs = 0.0;
    int m_stallEvents = 0;      // ticks com bloqueio > 1 ms
    double m_spanStart = -1.0;
    double m_spanEnd = 0.0;
};