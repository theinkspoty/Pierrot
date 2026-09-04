// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "PlaybackEngine.h"

#include "models/Project.h"
#include "ui/TlLog.h"

#include <QTimer>
#include <QPushButton>
#include <algorithm>
#include <cmath>

// Retorna o fps do projeto (fallback 0 se sem projeto).
static double projFps(const Project* p) {
    return p ? p->fps : 0.0;
}

PlaybackEngine::PlaybackEngine() = default;
PlaybackEngine::~PlaybackEngine() = default;

void PlaybackEngine::setProject(Project* p) {
    m_project = p;
    if (!p) {
        stopPlaybackInternal();
        m_playhead = 0.0;
    }
}

void PlaybackEngine::setFrameInterval() {
    const double fps = projFps(m_project);
    if (!m_timer) return;
    // Dispara em metade do período do frame: o tick calcula o frame alvo pelo
    // clock de alta precisão e avança exatamente 1 frame por vez. Com timer
    // grosseiro (1x/frame), o QTimer atrasado pela UI fazia o llround pular frames.
    m_timer->setInterval(fps > 0.0 ? qBound(8, (int)std::lround(1000.0 / fps / 2.0), 40) : 33);
}

void PlaybackEngine::seek(double t) {
    // O pedido de seek nasceu quase no zero com a agulha longe? Quem emitiu
    // o playheadChanged mandou valor errado — registra para o dump automático.
    if (t < 0.25 && m_playhead > 10.0)
        TlLog::note(QStringLiteral("seek(≈0) pedido com playhead em %1 (playing=%2)")
                        .arg(m_playhead, 0, 'f', 2).arg(m_playing ? 1 : 0));
    // Snap ao frame do projeto (playhead sempre em frame cheio).
    const double fps = projFps(m_project);
    const double snapped = (fps > 0.0) ? std::round(t * fps) / fps : std::max(0.0, t);
    if (m_playing) {
        // Scrub durante a reprodução: reancora os dois relógios na nova
        // posição e reposiciona os decoders de áudio (antes o tick ignorava
        // o salto e o playhead voltava na próxima passada). O reposicionamento
        // do mixer é assíncrono: com arquivos grandes ele leva tempo demais
        // para a UI (e congelava o sink, derrubando o relógio de áudio).
        m_playStart = snapped;
        m_clock.restart();
        m_audioLastRaw = -1.0; // próxima leitura só serve para detectar progresso
        anchorAudioClock(snapped);
        onMixAudio(snapped, true);
        TlLog::note(QStringLiteral("seek playing -> %1").arg(snapped, 0, 'f', 2));
    } else if (std::fabs(snapped - m_playhead) > 0.25) {
        TlLog::note(QStringLiteral("seek pausado %1 -> %2")
                        .arg(m_playhead, 0, 'f', 2).arg(snapped, 0, 'f', 2));
    }
    applySeekInternal(snapped);
}

void PlaybackEngine::applySeekInternal(double t) {
    const double fps = projFps(m_project);
    const double prev = m_playhead;
    m_playhead = (fps > 0.0) ? std::round(t * fps) / fps : std::max(0.0, t);
    if (std::fabs(m_playhead - prev) > 0.25)
        TlLog::note(QStringLiteral("applySeek %1 -> %2 (playing=%3)")
                        .arg(prev, 0, 'f', 3).arg(m_playhead, 0, 'f', 3)
                        .arg(m_playing ? 1 : 0));
    m_currentFrameIndex = std::llround(m_playhead * fps);
    onSeek(m_playhead);
    onPlayheadMoved(m_playhead);
}

void PlaybackEngine::anchorAudioClock(double t) {
    const double raw = audioClockSec();
    if (raw < 0.0) { m_audioClockOn = false; return; }
    m_audioAnchor = t - raw;
    m_audioClockOn = true;
    m_lastAnchorClockMs = m_clock.elapsed();
}

void PlaybackEngine::togglePlay() {
    if (m_playing) {
        stopPlaybackInternal();
        return;
    }
    if (!m_project || m_project->duration() <= 0) return;
    if (m_playhead >= m_project->duration() - 1e-6) m_playhead = 0.0;
    if (m_loopEnabled && m_loopOut > m_loopIn) {
        if (m_playhead < m_loopIn || m_playhead >= m_loopOut)
            m_playhead = m_loopIn;
    }
    const double fps = projFps(m_project);
    m_currentFrameIndex = std::llround(m_playhead * fps);
    m_playStart = m_playhead;
    m_clock.start();
    m_playRate = 1.0;
    m_audioClockOn = false; // sink novo: reancora no primeiro tick
    m_playing = true;
    setFrameInterval();
    if (m_timer) m_timer->start();
    if (m_playBtn) m_playBtn->setText(QStringLiteral("Pausar"));
    onStartAudio(m_playhead);
    onStateChanged(true);
}

void PlaybackEngine::shuttle(int dir) {
    if (dir == 0) { // K: pausa (zera a taxa — o próximo Espaço toca 1x normal)
        stopPlaybackInternal();
        return;
    }
    // J/L repetidos aceleram (1x→2x→4x); a direção oposta reinicia em 1x.
    if (m_playing) {
        if (dir > 0)
            m_playRate = (m_playRate > 0.0) ? std::min(4.0, m_playRate * 2.0) : 1.0;
        else
            m_playRate = (m_playRate < 0.0) ? std::max(-4.0, m_playRate * 2.0) : -1.0;
    } else {
        m_playRate = (dir > 0) ? 1.0 : -1.0;
    }
    // Áudio só em 1x dianteiro: ré/acelerado deixa o sink mudo (o relógio de
    // áudio não representa velocidade ≠1 e rolaria dessincronizado).
    if (m_playRate == 1.0)
        onStartAudio(m_playhead);
    else
        onStopAudio();
    if (m_playing) return;
    if (!m_project || m_project->duration() <= 0) return;
    const double fps = projFps(m_project);
    m_currentFrameIndex = std::llround(m_playhead * fps);
    m_playStart = m_playhead;
    m_clock.start();
    m_audioClockOn = false;
    m_playing = true;
    setFrameInterval();
    if (m_timer) m_timer->start();
    if (m_playBtn) m_playBtn->setText(QStringLiteral("Pausar"));
    onStateChanged(true);
}

void PlaybackEngine::playFrom(double t) {
    // Enter (estilo Vegas): busca para a posição e começa a reproduzir dali,
    // mesmo se já estivesse tocando.
    if (!m_project || m_project->duration() <= 0) return;
    seek(std::clamp(t, 0.0, m_project->duration()));
    const double fps = projFps(m_project);
    m_currentFrameIndex = std::llround(m_playhead * fps);
    m_playStart = m_playhead;
    m_clock.start();
    m_playRate = 1.0;
    m_audioClockOn = false; // sink novo: reancora no primeiro tick
    m_playing = true;
    setFrameInterval();
    if (m_timer) m_timer->start();
    if (m_playBtn) m_playBtn->setText(QStringLiteral("Pausar"));
    onStartAudio(m_playhead);
    onStateChanged(true);
}

void PlaybackEngine::setLoopRange(double in, double out) {
    m_loopIn = in;
    m_loopOut = out;
}

void PlaybackEngine::setLoopEnabled(bool enabled) {
    m_loopEnabled = enabled;
}

void PlaybackEngine::stopPlaybackInternal() {
    m_playing = false;
    m_playRate = 1.0;
    if (m_timer) m_timer->stop();
    if (m_playBtn) m_playBtn->setText(QStringLiteral("Reproduzir"));
    m_currentFrameIndex = -1;
    m_audioClockOn = false;
    m_audioLastRaw = -1.0;
    onStopAudio();
    onStopPlaybackUI();
    onStateChanged(false);
}

void PlaybackEngine::tick() {
    if (!m_project) { stopPlaybackInternal(); return; }
    const double fps = projFps(m_project);
    const double dur = m_project->duration();

    // Tempo esperado pelo relógio de parede…
    const double elapsed = m_clock.elapsed() / 1000.0;
    double t = m_playStart + elapsed * m_playRate;

    // …corrigido pelo relógio do áudio (o que se ouve de verdade). Em
    // reproduções longas o relógio de parede desvia do consumo real do sink;
    // seguir o áudio evita o acúmulo de dessincronia. A correção é suave
    // (slew) para não tremer a imagem; desvio grande (underrun longo) faz a
    // imagem saltar de volta para o áudio — MAS só quando o relógio do áudio
    // está realmente avançando. Durante um seek/scrub com arquivo pesado o
    // sink entra em underrun e processedUSecs() congela: corrigir para um
    // relógio parado fazia a agulha saltar DE VOLTA (muitas vezes para perto
    // do início). Se o áudio não avançou desde o tick anterior, confia no
    // relógio de parede até ele voltar a correr.
    const double raw = (m_playRate == 1.0) ? audioClockSec() : -1.0;
    if (raw >= 0.0) {
        const bool audioAdvanced = m_audioLastRaw < 0.0 || raw > m_audioLastRaw + 1e-6;
        m_audioLastRaw = raw;
        if (!m_audioClockOn) anchorAudioClock(t);
        if (m_audioClockOn && audioAdvanced) {
            const double diff = (raw + m_audioAnchor) - t;
            // Período de graça: nos primeiros 400ms após uma ancoragem (seek,
            // loop, play) o desvio ainda é resíduo da troca de posição — os
            // decoders estão sendo reposicionados. Corrigir nesse intervalo
            // teleportava a agulha para a posição velha do áudio.
            const bool graceOver = m_lastAnchorClockMs < 0
                                   || m_clock.elapsed() - m_lastAnchorClockMs > 400;
            if (std::fabs(diff) > 0.30 && graceOver) {
                if (diff < -2.0) {
                    // Áudio MUITO atrás do vídeo embora avançando: os decoders
                    // ficaram numa posição errada (seek impreciso em arquivos
                    // longos, índice esparso). O vídeo é a referência — reancora
                    // o relógio na posição atual em vez de arrastar a agulha
                    // para trás (era o "volta pro começo").
                    TlLog::note(QStringLiteral("tick: reancora (áudio atrás %1s)")
                                    .arg(-diff, 0, 'f', 1));
                    m_audioAnchor = t - raw;
                } else {
                    // Drift moderado ou áudio à frente: vídeo alcança o áudio.
                    TlLog::note(QStringLiteral("tick: pula pra frente %1 -> %2")
                                    .arg(t, 0, 'f', 2).arg(raw + m_audioAnchor, 0, 'f', 2));
                    m_playStart = raw + m_audioAnchor;
                    m_clock.restart();
                    t = m_playStart;
                    m_lastAnchorClockMs = 0;
                }
            } else {
                t += qBound(-0.0025, diff * 0.12, 0.0025);
            }
        }
    }

    // Determina o índice de frame com base no clock de alta precisão
    const qint64 targetFrame = std::llround(t * fps);

    // Se o timer acordou ligeiramente antes de 1 frame inteiro passar, não
    // repete nem duplica o frame. No shuttle o frame anda para trás (ré):
    // avança só quando o alvo cruzou o frame atual na direção da taxa.
    if (m_playRate >= 0.0) {
        if (targetFrame <= m_currentFrameIndex) return;
    } else {
        if (targetFrame >= m_currentFrameIndex) return;
    }

    m_currentFrameIndex = targetFrame;
    if (fps <= 0.0) t = m_playStart + elapsed * m_playRate;

    if (m_loopEnabled && m_loopOut > m_loopIn) {
        if (t >= m_loopOut - 1e-9) {
            m_playStart = m_loopIn;
            m_currentFrameIndex = std::llround(m_loopIn * fps);
            m_clock.restart();
            anchorAudioClock(m_loopIn); // o sink continua rodando: compensa no anchor
            applySeekInternal(m_loopIn);
            if (m_playRate == 1.0) onMixAudio(m_loopIn, true);
            return;
        }
        if (t <= m_loopIn + 1e-9) {
            // Ré/negativo alcançou o início do loop: volta ao fim.
            m_playStart = m_loopOut;
            m_currentFrameIndex = std::llround(m_loopOut * fps);
            m_clock.restart();
            anchorAudioClock(m_loopOut);
            applySeekInternal(m_loopOut);
            return;
        }
    }
    if (t >= dur) {
        applySeekInternal(dur);
        stopPlaybackInternal();
        return;
    }
    if (t <= 0.0) {
        applySeekInternal(0.0);
        stopPlaybackInternal();
        return;
    }

    applySeekInternal(t);
    onPrefetch();
    // Mixer acompanha o playhead: volumes/fades e troca de clipes acontecem
    // aqui, sem reiniciar o sink a cada transição. No shuttle (≠1x) o sink
    // fica mudo (stopAudio) e não deve ser reposicionado por frame.
    if (m_playRate == 1.0) onMixAudio(t, false);
}

// Clipe de vídeo (com mídia) no topo em `t`. Clipes de texto independentes são
// ignorados aqui (não têm quadro para decodificar); o texto é desenhado por
// cima do vídeo no paintEvent.
const Clip* PlaybackEngine::clipAt(double t) const {
    if (!m_project) return nullptr;
    for (int tr = 0; tr < (int)m_project->videoTracks.size(); ++tr) {
        const Track& track = m_project->videoTracks[tr];
        // Track de Mesa gera quadro mesmo sem mídia própria (a composição é a
        // fonte de vídeo).
        const bool mesaTrack = m_project->findMesaForTrack(track.id) != nullptr;
        const Clip* best = nullptr;
        for (const Clip& c : track.clips) {
            if (t >= c.pos && t < c.pos + c.dur && !c.isText) {
                const MediaItem* m = m_project->findMedia(c.mediaId);
                const bool hasVideo = mesaTrack || (m && m->hasVideo);
                if (hasVideo && (!best || c.pos > best->pos)) best = &c;
            }
        }
        if (best) return best;
    }
    return nullptr;
}
