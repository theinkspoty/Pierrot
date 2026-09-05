// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "AudioConformCache.h"

#include "ffmpeg/FFmpegDecoder.h"

#include <QDebug>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

AudioConformCache& AudioConformCache::instance() {
    static AudioConformCache inst;
    return inst;
}

// Converte segundos → frame (S16/48k/estéreo).
static qint64 secToFrame(double sec) {
    return (qint64)std::llround(sec * AudioConformCache::kSampleRate);
}
static double frameToSec(qint64 f) {
    return (double)f / AudioConformCache::kSampleRate;
}

// Chave composta (path|stream) para o registro.
static QString chunkKey(const QString& path, int stream) {
    return QStringLiteral("%1|%2").arg(path).arg(stream);
}

AudioConformCache::AudioConformCache() {
    m_worker = std::thread([this]() { workerLoop(); });
}

AudioConformCache::~AudioConformCache() {
    {
        std::lock_guard<std::mutex> lk(m_workMtx);
        m_stop = true;
    }
    m_workCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    QMutexLocker l(&m_regMtx);
    for (Chunk* c : m_byKey) c->release(); // desfaz a ref permanente do registro
    m_byKey.clear();
}

AudioConformCache::Ref AudioConformCache::get(const QString& filePath, int stream) {
    const QString key = chunkKey(filePath, stream);
    QMutexLocker l(&m_regMtx);
    Chunk*& slot = m_byKey[key];
    if (!slot) {
        slot = new Chunk;
        slot->filePath = filePath;
        slot->stream = stream;
        slot->lastUseMs = nowMs();
    } else {
        QMutexLocker lc(&slot->mtx);
        slot->lastUseMs = nowMs();
    }
    return Ref(slot);
}

void AudioConformCache::request(const Ref& cache, double startSec, double lenSec) {
    Chunk* c = cache.get();
    if (!c || lenSec <= 0.0 || startSec < 0.0) return;
    {
        QMutexLocker l(&c->mtx);
        mergeWanted(c, startSec, startSec + lenSec);
    }
    // Acorda o worker (ou re-scan, se ele já estiver mid-fill).
    m_workCv.notify_all();
}

// Menor frame de [wStart,wEnd) ainda não decodificado; -1 se totalmente
// coberto. Chamar com c->mtx tomado.
static qint64 uncoveredStart(const AudioConformCache::Chunk& c, double wStart,
                             double wEnd) {
    qint64 a = secToFrame(wStart);
    const qint64 b = secToFrame(wEnd);
    for (const auto& f : c.filled) {
        if (f.first <= a && a < f.second) { a = f.second; break; }
    }
    return a < b ? a : -1;
}

// Insere [a,b) nas janelas pedidas, coalescendo com janelas sobrepostas ou
// contíguas (≤ um quadro de folga). Chamar com c->mtx tomado.
void AudioConformCache::mergeWanted(Chunk* c, double a, double b) {
    c->wanted.append({a, b});
    if (c->wanted.size() < 2) return;
    std::sort(c->wanted.begin(), c->wanted.end(),
              [](const QPair<double, double>& x, const QPair<double, double>& y) {
                  if (x.first != y.first) return x.first < y.first;
                  return x.second < y.second;
              });
    QVector<QPair<double, double>> merged;
    const double eps = 1.0 / kSampleRate; // 1 quadro
    for (const auto& w : c->wanted) {
        if (merged.isEmpty() || w.first > merged.last().second + eps) {
            merged.append(w);
        } else {
            merged.last().second = qMax(merged.last().second, w.second);
        }
    }
    c->wanted = std::move(merged);
}

// Próximo trabalho deste chunk: o menor frame ainda não decodificado dentre
// as janelas pedidas (janelas totalmente cobertas são descartadas; as demais
// permanecem até serem preenchidas). Retorna -1 se nada a fazer.
qint64 AudioConformCache::nextFillWork(Chunk* c, double startSec,
                                       double& outStartSec, double& outHorizonSec) {
    Q_UNUSED(startSec);
    QMutexLocker l(&c->mtx);
    QVector<QPair<double, double>> pending;
    qint64 result = -1;
    double bestStart = 0.0, bestHorizon = 0.0;
    for (const auto& w : c->wanted) {
        const qint64 unw = uncoveredStart(*c, w.first, w.second);
        if (unw < 0) continue; // janela totalmente coberta → libera
        if (result < 0 || unw < result) {
            result = unw;
            bestStart = frameToSec(unw);
            bestHorizon = qMax(0.75, w.second - w.first);
        }
        pending.append(w); // mantém enquanto não estiver totalmente coberto
    }
    c->wanted = std::move(pending);
    if (result >= 0) {
        outStartSec = bestStart;
        outHorizonSec = qBound(1.0, bestHorizon * 2.0, 8.0);
    }
    return result;
}

// Grava `frames` frames (S16 estéreo) em [startFrame, startFrame+frames),
// expandindo o buffer e o registro de intervalos cobertos. Chamar com
// sem mutex (aqui só o worker escreve; readers acessam sob c->mtx).
void AudioConformCache::fillRegion(Chunk* c, qint64 startFrame,
                                   const int16_t* data, qint64 frames) {
    if (frames <= 0) return;
    QMutexLocker l(&c->mtx);
    const qint64 oldLen = c->pcm.size();
    const qint64 needFrames = startFrame + frames;
    if (needFrames * kChannels > oldLen) {
        c->pcm.resize((int)(needFrames * kChannels));
        qint64 newStart = oldLen; // zera o que cresceu (amostras de lacuna)
        std::memset(c->pcm.data() + newStart, 0,
                    (size_t)(c->pcm.size() - newStart) * sizeof(int16_t));
        const qint64 delta =
            (qint64)(c->pcm.size() - oldLen) * (qint64)sizeof(int16_t);
        m_totalBytes.fetch_add(delta, std::memory_order_relaxed);
    }
    std::memcpy(c->pcm.data() + startFrame * kChannels, data,
                (size_t)frames * kChannels * sizeof(int16_t));
    c->filled.append({startFrame, startFrame + frames});
    std::sort(c->filled.begin(), c->filled.end(),
              [](const QPair<qint64, qint64>& x, const QPair<qint64, qint64>& y) {
                  return x.first < y.first;
              });
    QVector<QPair<qint64, qint64>> merged;
    for (const auto& f : c->filled) {
        if (merged.isEmpty() || f.first > merged.last().second) {
            merged.append(f);
        } else {
            merged.last().second = qMax(merged.last().second, f.second);
        }
    }
    c->filled = std::move(merged);
    l.unlock();
    evictIfOverBudget();
}

// Lê até maxFrames frames a partir de startFrame; devolve quantos estavam
// disponíveis continuamente (lacuna não preenchida interrompe). Regiões não
// prontas saem em silêncio no `out`. Thread-safe (ler de várias fontes).
int AudioConformCache::readFrames(const Ref& cache, qint64 startFrame,
                                  int maxFrames, int16_t* out) {
    if (maxFrames <= 0) return 0;
    std::memset(out, 0, (size_t)maxFrames * kChannels * sizeof(int16_t));
    Chunk* c = cache.get();
    if (!c) return 0;
    QMutexLocker l(&c->mtx);
    c->lastUseMs = nowMs();
    const qint64 end = startFrame + maxFrames;
    qint64 cur = startFrame;
    while (cur < end) {
        const qint64 start = cur;
        // Procura o intervalo coberto que contém `cur`.
        for (const auto& f : c->filled) {
            if (f.second <= start || f.first > start) continue;
            const qint64 take = qMin(end, f.second) - cur;
            if (take > 0) {
                std::memcpy(out + (cur - startFrame) * kChannels,
                            c->pcm.constData() + cur * kChannels,
                            (size_t)take * kChannels * sizeof(int16_t));
                cur += take;
            }
            break;
        }
        if (cur == start) break; // lacuna não preenchida
    }
    return (int)(cur - startFrame);
}

// Bloqueia (com pequenos sleeps) até [startFrame, startFrame+frames) estar no
// buffer, ou o timeout. O worker normalmente cobre isso em ms.
bool AudioConformCache::waitReady(const Ref& cache, qint64 startFrame, int frames,
                                  int timeoutMs) {
    if (frames <= 0) return true;
    Chunk* c = cache.get();
    if (!c) return false;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            QMutexLocker l(&c->mtx);
            c->lastUseMs = nowMs();
            const qint64 end = startFrame + frames;
            for (const auto& f : c->filled) {
                // Intervalos estão mergeados; cobertura contígua do head =
                // um intervalo que engloba [startFrame, end).
                if (f.first <= startFrame && f.second >= end) return true;
            }
            if (startFrame < 0) return true;
        }
        // Ainda não coberto: espera as próximas iterações do worker.
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Work do worker: uma sessão de decodificação contínua sobre o chunk escolhido.
// Decode → fillRegion → re-scaneia: se o próximo descoberto é exatamente onde
// parou, continua SEM re-seek (fluxo contínuo); senão re-faz seek. Termina
// quando a janela alvo é preenchida ou ao fim do arquivo. Empobrece a cota
// por sessão para múltiplos caches serem atendidos em rodízio.
void AudioConformCache::doFill(Chunk* c, double startSec, double horizonSec,
                               FFmpegDecoder& dec, qint64& decPos) {
    if (!dec.isOpen()) return;

    // Sessão limitada para fontes disputarem o worker em rodízio. O decoder é
    // PERSISTENTE (vive entre sessões do mesmo chunk): continuar o fronterio é
    // decode contínuo SEM re-open/re-seek — essencial para não criar lacunas
    // (o "flick") em reprodução longa de um único arquivo.
    const qint64 goal = qMin(secToFrame(horizonSec), secToFrame(2.0));
    qint64 sessionWritten = 0;

    std::vector<int16_t> buf(4096 * kChannels); // ~85ms por acesso

    while (sessionWritten < goal) {
        double wantStart = 0.0, wantHorizon = 0.0;
        const qint64 target = nextFillWork(c, startSec, wantStart, wantHorizon);
        if (target < 0) return; // todas as janelas cobertas

        if (decPos < 0 || target != decPos) {
            // Pouso do decoder deve coincidir com a posição do diapositivo →
            // o intervalo preenchido começa EXATAMENTE na amostra pedida, e um
            // corte vira duas leituras contíguas do mesmo buffer sem costura.
            dec.seekAudio(frameToSec(target));
            decPos = target;
        }

        qint64 wantEnd = target + secToFrame(wantHorizon);
        int zeros = 0;
        while (decPos < wantEnd && sessionWritten < goal) {
            const int got = dec.decodeAudio(buf.data(),
                                            (int)(buf.size() * sizeof(int16_t)));
            if (got <= 0) {
                if (++zeros >= 8) return; // provavelmente fim de arquivo
                continue;
            }
            zeros = 0;
            const qint64 frames = got / (2 * kChannels);
            if (frames <= 0) continue;
            fillRegion(c, decPos, buf.data(), frames);
            decPos += frames;
            sessionWritten += frames;
        }
    }
}

// Quantos chunks têm janela pendente (trabalho a fazer)? Chamar sob m_regMtx.
int AudioConformCache::countPendingChunks() const {
    int n = 0;
    for (Chunk* c : m_byKey) {
        QMutexLocker lc(&c->mtx);
        for (const auto& w : c->wanted)
            if (uncoveredStart(*c, w.first, w.second) >= 0) { ++n; break; }
    }
    return n;
}

void AudioConformCache::workerLoop() {
    // Decoder PERSISTENTE do worker: continua vivo entre sessões do MESMO
    // chunk (decode contínuo sem re-open/re-seek). Só troca de arquivo quando
    // outro chunk passa a ser mais urgente. Evita o "flick" de lacunas.
    FFmpegDecoder dec;
    Ref decRef; // segura o chunk dono do decoder contra evicção LRU
    qint64 decPos = -1;

    for (;;) {
        Chunk* chosen = nullptr;
        int pending = 0;
        {
            QMutexLocker l(&m_regMtx);
            qint64 best = -1;
            for (Chunk* c : m_byKey) {
                qint64 cand = -1;
                {
                    QMutexLocker lc(&c->mtx);
                    for (const auto& w : c->wanted) {
                        const qint64 uw = uncoveredStart(*c, w.first, w.second);
                        if (uw >= 0) { cand = uw; break; }
                    }
                }
                if (cand < 0) continue;
                ++pending;
                // Atende primeiro o chunk cujo trecho mais próximo do playhead
                // ainda não foi preenchido (rotacionado entre caches).
                if (best < 0 || cand < best) {
                    best = cand;
                    chosen = c;
                }
            }
            if (chosen) chosen->addRef(); // segura o chunk durante a sessão
        }
        if (!chosen) {
            std::unique_lock<std::mutex> lk(m_workMtx);
            m_workCv.wait_for(lk, std::chrono::milliseconds(120),
                              [this]() { return m_stop; });
            if (m_stop) return;
            continue;
        }

        // Troca de arquivo no decoder quando o mais urgente mudou.
        if (decRef.get() != chosen) {
            dec.close();
            decRef = Ref(chosen);
            if (!dec.open(chosen->filePath, chosen->stream)) {
                chosen->release();
                decRef = Ref(); // não mantém pinado um chunk inaberto
                continue;
            }
            decPos = -1;
        }

        // Vários chunks disputando → sessão curta para o rodízio; sozinho →
        // sessão cheia (com decoder persistente, sem custo de re-open).
        const double capSec = (pending > 1) ? 0.75 : 2.0;
        double s0 = 0.0, h0 = 0.0;
        if (nextFillWork(chosen, 0.0, s0, h0) < 0) {
            chosen->release();
            continue;
        }
        doFill(chosen, s0, qMin(h0, capSec), dec, decPos);
        chosen->release();
    }
}

// Se a soma em memória estourou a cota, coleta os chunks sem leitor externo
// (refs == 1: só o registro) começando pelos menos usados recentemente.
void AudioConformCache::evictIfOverBudget() {
    if (m_totalBytes.load(std::memory_order_relaxed) <= kBudgetBytes) return;
    QVector<QPair<Chunk*, qint64>> candidates; // (chunk, lastUseMs)
    {
        QMutexLocker l(&m_regMtx);
        for (Chunk* c : m_byKey) {
            QMutexLocker lc(&c->mtx);
            if (c->refs.load(std::memory_order_acquire) == 1)
                candidates.append({c, c->lastUseMs});
        }
    }
    if (candidates.isEmpty()) return;
    std::sort(candidates.begin(), candidates.end(),
              [](const QPair<Chunk*, qint64>& a, const QPair<Chunk*, qint64>& b) {
                  return a.second < b.second;
              });
    {
        QMutexLocker l(&m_regMtx);
        QVector<Chunk*> doomed;
        for (auto& cand : candidates) {
            if (m_totalBytes.load(std::memory_order_relaxed) <= kBudgetBytes) break;
            Chunk* c = cand.first;
            {
                QMutexLocker lc(&c->mtx);
                if (c->refs.load(std::memory_order_acquire) != 1) continue;
                const qint64 sz = (qint64)c->pcm.size() * (qint64)sizeof(int16_t);
                m_totalBytes.fetch_sub(sz, std::memory_order_relaxed);
            }
            const QString key = chunkKey(c->filePath, c->stream);
            if (m_byKey.contains(key)) m_byKey.remove(key);
            doomed.append(c);
        }
        for (Chunk* c : doomed) c->release(); // queda de 1 → 0 → delete
    }
}