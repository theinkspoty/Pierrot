// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QHash>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

class FFmpegDecoder;

// Camada de "conform" estilo Olive para o MixerWidget de áudio do preview.
//
// Em vez de manter um FFmpegDecoder por clipe (que re-decodifica ~30ms na
// janela retroativa de um corte e ecoa a cauda do sink), o áudio de cada
// arquivo+stream é decodificado UMA vez, lazy e em background, para um buffer
// PCM linear S16/48kHz/estéreo — o mesmo formato que o decoder já resampla.
// O mixer lê FATIAS desse buffer por índice de amostra:
//
//   - corte = dois intervalos adjacentes do MESMO buffer ⇒ continuidade
//     amostra-exata, eco/duplicação de junção impossível POR CONSTRUÇÃO;
//   - seek/loop = releitura posicional instantânea (sem av_seek_frame);
//   - speed ≠ 1 = interpolação linear no momento da leitura (corrige pitch e
//     duração, que reprodução contínua com decoder fazia errado).
//
// Preenchimento: criar pedidos de leitura (request()) emite janelas de
// decodificação para um worker em thread dedicada. Ele faz seek BACKWARD
// (keyframe ≤ alvo, com descarte por PTS, como o decoder já faz) e grava o
// trecho decodificado no buffer do cache. Regiões ainda não decodificadas
// retornam silêncio — o preenchimento alcança o playhead em tempo real.
//
// Memória: refcount manual (cada leitor segura um Ref). O registro mantém uma
// ref permanente e o LRU devolve o buffer à medida que caches sem uso externo
// ultrapassam a cota de memória.
class AudioConformCache {
public:
    static AudioConformCache& instance();

    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;

    struct Chunk {
        QString filePath;
        int stream = 0;
        mutable QMutex mtx;
        // S16 interleaved (L,R). Tamanho = amostras alocadas (preenchidas ou
        // não); a região só é considerada pronta se estiver em `filled`.
        QVector<int16_t> pcm;
        // Intervalos [inicioFrames, fimFrames) já decodificados (mergeados).
        QVector<QPair<qint64, qint64>> filled;
        // Janelas pedidas, em SEGUNDOS: [inicio, fim). Coalescidas.
        QVector<QPair<double, double>> wanted;
        qint64 lastUseMs = 0;
        std::atomic<int> refs{1}; // 1 permanente do registro
        void addRef() { refs.fetch_add(1, std::memory_order_relaxed); }
        void release() {
            if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
        }
    };

    // Handle RAII: incrementa/decrementa o refcount do Chunk.
    struct Ref {
        Chunk* c = nullptr;
        Ref() = default;
        explicit Ref(Chunk* ch) : c(ch) { if (c) c->addRef(); }
        Ref(const Ref& o) : c(o.c) { if (c) c->addRef(); }
        Ref& operator=(const Ref& o) {
            if (c == o.c) return *this;
            if (c) c->release();
            c = o.c;
            if (c) c->addRef();
            return *this;
        }
        ~Ref() { if (c) c->release(); }
        Chunk* get() const { return c; }
        Chunk* operator->() const { return c; }
        explicit operator bool() const { return c != nullptr; }
    };

    // Pega (ou cria) o cache do arquivo+stream. O refcount evita que o LRU
    // colete o buffer enquanto houver leitor (o mixer) segurando um Ref.
    Ref get(const QString& filePath, int stream);

    // Garante, async, que [startSec, startSec+lenSec) do cache seja
    // decodificado. Coalesce janelas próximas e pode ser chamado de qualquer
    // thread. Já preenchido ⇒ não adiciona trabalho.
    void request(const Ref& cache, double startSec, double lenSec);

    // Lê até `maxFrames` frames (S16 estéreo) a partir de `startFrame`.
    // Preenche `out`; as amostras de regiões ainda não decodificadas saem em
    // silêncio. Retorna quantos frames estavam disponíveis só até a primeira
    // lacuna não preenchida (o resto do buffer é zerado). Thread-safe.
    int readFrames(const Ref& cache, qint64 startFrame, int maxFrames,
                   int16_t* out);

    // Bloqueia até o trecho contíguo [startFrame, startFrame+frames) estar
    // decodificado, ou `timeoutMs` se esgotar (devolve false). Usado para dar
    // partida no sink só com o head do playhead pronto — evita o "começa
    // mudo/dessincronizado" do warm-up frio.
    bool waitReady(const Ref& cache, qint64 startFrame, int frames, int timeoutMs);

    // Total aproximado de bytes em todas as caches (para diagnóstico).
    qint64 totalBytes() const { return m_totalBytes.load(); }

private:
    AudioConformCache();
    ~AudioConformCache();
    AudioConformCache(const AudioConformCache&) = delete;
    AudioConformCache& operator=(const AudioConformCache&) = delete;

    void doFill(Chunk* c, double startSec, double horizonSec,
                FFmpegDecoder& dec, qint64& decPos);
    void workerLoop();
    qint64 nextFillWork(Chunk* c, double startSec, double& outStartSec,
                        double& outHorizonSec);
    void fillRegion(Chunk* c, qint64 startFrame, const int16_t* data, qint64 frames);
    void evictIfOverBudget();
    void mergeWanted(Chunk* c, double a, double b);

    // Número de chunks com trabalho pendente (para encurtar sessões quando
    // várias fontes disputam o worker). Chamar sob m_regMtx.
    int countPendingChunks() const;

    QHash<QString, Chunk*> m_byKey;
    mutable QMutex m_regMtx;

    std::mutex m_workMtx;
    std::condition_variable m_workCv;
    std::thread m_worker;
    bool m_stop = false;
    std::atomic<qint64> m_totalBytes{0};
    static constexpr qint64 kBudgetBytes = 512LL * 1024 * 1024; // ~512 MB
};