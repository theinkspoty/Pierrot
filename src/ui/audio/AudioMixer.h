// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

// Mixer de áudio do preview (QIODevice puxado pelo QAudioSink) e os
// construtores de fontes do mix/warm. Extraído de PreviewWidget.cpp (F4) para
// ser testável de forma independente.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <QHash>
#include <QIODevice>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QVector>

#include "colombina/ffmpeg/AudioConformCache.h"
#include "colombina/models/Project.h"
#include "ui/audio/AudioFxDsp.h"
#include "ui/monitor/PreviewDebug.h"

// Mixer de áudio: soma o PCM de todos os clipes ativos em `t` (clipe de vídeo
// + faixas de áudio), cada um com volume próprio (clipe, envelope e faixa).
// A camada de conform (AudioConformCache) decodifica cada arquivo+stream UMA
// vez, em background, para S16/48 kHz/estéreo; o mixer só lê fatias por índice
// — um corte é leitura contígua do mesmo buffer, então eco de junção é
// impossível. A thread do QAudioSink chama readData(); a UI chama updateSources()
// conforme o playhead avança.

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
        // Ganhos do equalizador gráfico de 10 bandas da faixa (dB, -12..+12).
        QVector<double> trackGraphicEq;
        // Bandas do equalizador paramétrico da faixa.
        QVector<ParametricBand> trackParametricEq;
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
            // Fim da mídia deste clipe (frame S16/48k), independente de o
            // Source ter entrado no meio (warm -> ativo). Limita os pedidos
            // de conform ao trecho que o clipe realmente usa: SFX de 0,2 s
            // não devem enfileirar 6 s de decode cada um.
            s->readEnd = (qint64)std::llround(w.mediaStart * sr)
                       + (qint64)std::llround((s->srcOutEnd - clipStartSr) * s->step);
            s->fx.resetState();
        };
        auto applyParams = [&](Source* s, const SourceInfo& w) {
            s->vol = w.vol;
            s->pan = w.pan;
            s->trackIndex = w.trackIndex;
            s->isAudioTrack = w.isAudioTrack;
            s->step = qMax(0.01, w.speed);
            s->srcOutEnd = (qint64)std::llround((w.clipPos + w.clipDur) * sr);
            const qint64 clipStartSr = (qint64)std::llround(w.clipPos * sr);
            s->readEnd = (qint64)std::llround(w.mediaStart * sr)
                       + (qint64)std::llround((s->srcOutEnd - clipStartSr) * s->step);
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
            applyParams(s, w);
            // Conform: garante o trecho sob o playhead + horizonte de leitura,
            // mas SEM passar do fim do clipe. O pedido de 4 s fixos enchia o
            // worker de janelas inúteis quando havia muitos clipes curtos em
            // sequência — o áudio seguinte chegava sem PCM (issue #13).
            const double mediaEndSec = (double)s->readEnd / sr;
            const double reqLen = qMin(4.0, qMax(0.0, mediaEndSec - w.mediaPos) + 0.1);
            if (reqLen > 0.0)
                conform.request(s->cache, w.mediaPos, reqLen, anchorSec);
            if (w.isAudioTrack) {
                AudioFx& tfx = m_trackFx[qMakePair(w.isAudioTrack, w.trackIndex)];
                tfx.configure(w.trackFxEqLow, w.trackFxEqMid, w.trackFxEqHigh,
                              w.trackFxDenoise, w.trackFxDenoiseAmount,
                              w.trackFxInvertPhase, false,
                              w.trackFxReverb, w.trackFxReverbMix, w.trackFxReverbSize);
                m_trackGraphicEq[qMakePair(w.isAudioTrack, w.trackIndex)]
                    .setGains(w.trackGraphicEq);
                m_trackParametricEq[qMakePair(w.isAudioTrack, w.trackIndex)]
                    .setBands(w.trackParametricEq);
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
            applyParams(s, w);
            // Warm: só o material do próprio clipe (antes: 6 s por SFX, o que
            // atrasava os clipes seguintes no worker).
            const double mediaEndSec = (double)s->readEnd / sr;
            const double reqLen = qMin(6.0, qMax(0.0, mediaEndSec - w.mediaPos) + 0.1);
            if (reqLen > 0.0)
                conform.request(s->cache, w.mediaPos, reqLen, w.clipPos);
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
    void setMasterGraphicEq(const QVector<double>& gains) {
        m_masterGraphicEq.setGains(gains);
    }
    void setMasterParametricEq(const QVector<ParametricBand>& bands) {
        m_masterParametricEq.setBands(bands);
    }

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
            // Do ponto de leitura ATÉ o fim do clipe (não 3 s fixos): o que
            // sobra de um clipe curto não vira trabalho que disputa com o
            // áudio seguinte. due=0: este é o áudio sendo consumido agora.
            const double readSec =
                (double)s->readFrame / (double)AudioConformCache::kSampleRate;
            const double mediaEndSec =
                (double)s->readEnd / (double)AudioConformCache::kSampleRate;
            const double reqLen = qMin(3.0, qMax(0.0, mediaEndSec - readSec));
            if (reqLen > 0.0)
                conform.request(s->cache, readSec, reqLen, 0.0);

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
                    // EQ gráfico da faixa, após os demais efeitos de faixa
                    // (espelha a ordem do export).
                    auto git = m_trackGraphicEq.find(key);
                    if (git != m_trackGraphicEq.cend())
                        git.value().process(buses[bi].data(), totalSamples / 2);
                    auto pit = m_trackParametricEq.find(key);
                    if (pit != m_trackParametricEq.cend())
                        pit.value().process(buses[bi].data(), totalSamples / 2);
                }
                const int16_t* bus = buses[bi].constData();
                for (int i = 0; i < totalSamples; ++i) {
                    const float s = o[i] / 32768.0f + bus[i] / 32768.0f;
                    // 32767 (não 32768): clamp em +1.0 × 32768 estouraria
                    // int16_t (wrap para −32768 = clique na saturação).
                    const int v = std::lround(qBound(-1.0f, s, 1.0f) * 32767.0f);
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

        // EQ gráfico do master no sinal final (após o volume master, igual ao
        // export).
        m_masterGraphicEq.process(reinterpret_cast<int16_t*>(data), capacity / 2 / 2);
        m_masterParametricEq.process(reinterpret_cast<int16_t*>(data),
                                     capacity / 2 / 2);

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
        qint64 readEnd = 0;     // frame de mídia (S16/48k) do fim do clipe
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
    // Equalizador gráfico de 10 bandas por faixa de áudio (aplicado ao
    // barramento da faixa, após os efeitos de faixa) e no master.
    QHash<QPair<bool,int>, GraphicEq> m_trackGraphicEq;
    GraphicEq m_masterGraphicEq;
    // Equalizador paramétrico por faixa de áudio e no master.
    QHash<QPair<bool,int>, ParametricEqDsp> m_trackParametricEq;
    ParametricEqDsp m_masterParametricEq;
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

// Fontes ativas do mix em `t` (clipes de áudio sob o playhead, dedupe,
// mute/solo, envelopes). Usado por PreviewWidget::onMixAudio e pelo warm.
QVector<AudioMixer::SourceInfo> buildMixSources(const Project* p, double t);

// Fontes "warm" à frente do playhead (lookahead do mixer): mesma geometria
// das ativas, com o MESMO ganho da costura, para a troca esquerda→direita
// ser amostra-exata.
QVector<AudioMixer::SourceInfo> buildWarmSources(
    const Project* p, double t, const QVector<AudioMixer::SourceInfo>& active);
