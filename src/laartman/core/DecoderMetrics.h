// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

// DecoderMetrics — instrumentação do motor (núcleo, interno).
//
// Saiu da SUPERFÍCIE PÚBLICA do núcleo no §8.5: os contadores (open/seek/
// discard/work) eram estáticos públicos do `FFmpegDecoder` (no header da
// fachada). Isso é estado GLOBAL mutável exposto pelo núcleo — o invariante 2
// do plano v0.4 proíbe ("estado mutável só existe em L2, é privado, e nunca
// escapa"). Aqui eles viram um singleton INTERNO: o núcleo escreve via
// record*() nos pontos quentes e quem observa (L4/L6 — overlay e profiler)
// lê daqui.
//
// Por que NÃO em colombina (L4): `colombina` depende de `laartman` (é o
// consumidor), nunca o contrário. Pôr isto em L4 faria o núcleo incluir um
// header de quem depende dele — inversão de camadas. Sendo um header interno
// (não a fachada `Decoder.h`), o núcleo continua independente e a superfície
// pública fica limpa, que é o ponto do §8.5.
//
// Custo no caminho quente: o MESMO de antes (um store/fetch_add relaxado) — a
// mudança é de POSSE/visibilidade, não de comportamento.
//
// Singleton de processo, como SourcePool/FrameCache::instance().

#include <atomic>

#include <QtGlobal>

class DecoderMetrics {
public:
    static DecoderMetrics& instance() {
        static DecoderMetrics m;
        return m;
    }

    // ── Open: avformat_open_input + find_stream_info ──────────────────────
    // O que dói num corte não é decodificar (p99 < 1 ms) e sim reabrir/parsear
    // o container. Atômico porque open() roda na thread do FrameWorker E na do
    // prefetch/warm.
    void recordOpen(quint64 ns) {
        m_lastOpenNs.store(ns, std::memory_order_relaxed);
        m_openTotalNs.fetch_add(ns, std::memory_order_relaxed);
        m_openCount.fetch_add(1, std::memory_order_relaxed);
    }
    quint64 openCount()   const { return m_openCount.load(std::memory_order_relaxed); }
    quint64 openTotalNs() const { return m_openTotalNs.load(std::memory_order_relaxed); }
    quint64 lastOpenNs()  const { return m_lastOpenNs.load(std::memory_order_relaxed); }

    // ── Seek: seek + flush dentro de frameAt ──────────────────────────────
    // O `seek` medido no engine só enxerga o tick; o seek real acontece na
    // thread do decoder. Sem isto um seek de 300 ms aparecia como "seek=0 ms".
    void recordSeek(quint64 ns) {
        m_lastSeekNs.store(ns, std::memory_order_relaxed);
        m_seekTotalNs.fetch_add(ns, std::memory_order_relaxed);
        m_seekCount.fetch_add(1, std::memory_order_relaxed);
    }
    quint64 seekCount()   const { return m_seekCount.load(std::memory_order_relaxed); }
    quint64 seekTotalNs() const { return m_seekTotalNs.load(std::memory_order_relaxed); }
    quint64 lastSeekNs()  const { return m_lastSeekNs.load(std::memory_order_relaxed); }

    // ── Discard: quadros decodificados e descartados antes do alvo ────────
    // Custo escondido de um seek com GOP aberto: do keyframe até o alvo.
    // lastDiscard é por-chamada (o reset em frameAt), o total é acumulado.
    void beginFrameDiscard() { m_lastDiscard.store(0, std::memory_order_relaxed); }
    void recordDiscard(quint64 n) {
        m_lastDiscard.store(n, std::memory_order_relaxed);
        m_discardTotal.fetch_add(n, std::memory_order_relaxed);
    }
    quint64 lastDiscard()  const { return m_lastDiscard.load(std::memory_order_relaxed); }
    quint64 discardTotal() const { return m_discardTotal.load(std::memory_order_relaxed); }

    // ── Work: tempo total dentro de frameAt ───────────────────────────────
    // Comparado com a latência dispatch→pronto (wrk), separa "o worker demorou"
    // de "o pedido ficou na fila esperando".
    void recordWork(quint64 ns) {
        m_lastWorkNs.store(ns, std::memory_order_relaxed);
        m_workTotalNs.fetch_add(ns, std::memory_order_relaxed);
    }
    quint64 lastWorkNs()  const { return m_lastWorkNs.load(std::memory_order_relaxed); }
    quint64 workTotalNs() const { return m_workTotalNs.load(std::memory_order_relaxed); }

    // ── Reset (testes / nova sessão de medição) ───────────────────────────
    void reset() {
        m_openCount.store(0); m_openTotalNs.store(0); m_lastOpenNs.store(0);
        m_seekCount.store(0); m_seekTotalNs.store(0); m_lastSeekNs.store(0);
        m_discardTotal.store(0); m_lastDiscard.store(0);
        m_lastWorkNs.store(0); m_workTotalNs.store(0);
    }

private:
    DecoderMetrics() = default;
    DecoderMetrics(const DecoderMetrics&) = delete;
    DecoderMetrics& operator=(const DecoderMetrics&) = delete;

    std::atomic<quint64> m_openCount{0}, m_openTotalNs{0}, m_lastOpenNs{0};
    std::atomic<quint64> m_seekCount{0}, m_seekTotalNs{0}, m_lastSeekNs{0};
    std::atomic<quint64> m_discardTotal{0}, m_lastDiscard{0};
    std::atomic<quint64> m_lastWorkNs{0}, m_workTotalNs{0};
};
