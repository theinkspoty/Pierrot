// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// PlaybackEngine — motor de transporte extraído do PreviewWidget.
// Controla: play/pause, seek, shuttle JKL, loop e sincronização A/V
// (wall clock vs. audio sink clock) e o avanço de quadro.
//
// NOTA: PlaybackEngine é uma classe PLAIN (não-QObject). O PreviewWidget
// (QWidget) herda dela em paralelo e implementa os hooks virtuais
// (onSeek/onPrefetch/onMixAudio/onStartAudio/onStopAudio) para decodificação,
// áudio e rendering. Os sinais (playheadMoved/stateChanged) ficam no widget.
//
// O QTimer é de propriedade do widget; o engine recebe um ponteiro via
// setTimer(). O timeout do timer chama tick(); o engine liga/desliga o timer
// quando necessário. O botão de play também é setado por referência.

#pragma once

#include <QElapsedTimer>

#include "PreviewProfiler.h"

class QTimer;
class QPushButton;
class Project;
struct Clip;

class PlaybackEngine {
public:
    explicit PlaybackEngine();
    virtual ~PlaybackEngine();

    void setProject(Project* p);
    void setTimer(QTimer* timer) { m_timer = timer; }
    void setPlayButton(QPushButton* btn) { m_playBtn = btn; }

    // Contador de frames perdidos (skip automático). Reseta ao ler.
    qint64 consumeDroppedFrames();
    qint64 totalDroppedMonotonic() const { return m_droppedMonotonic; }

    // Métodos de transporte (chamados pelos slots do widget).
    void seek(double t);
    void togglePlay();
    void shuttle(int dir);
    void playFrom(double t);
    void setLoopRange(double in, double out);
    void setLoopEnabled(bool enabled);

    // Estado atual (usado pela janela de preview externo para espelhar os
    // botões de loop/play do monitor principal).
    bool loopEnabled() const { return m_loopEnabled; }
    bool isPlaying() const { return m_playing; }

    // Chamado pelo timeout do QTimer do widget.
    void tick();

protected:
    // Hooks que o PreviewWidget implementa (decodificação/áudio/rendering).
    virtual void onSeek(double t) = 0;
    virtual void onPrefetch() = 0;
    virtual void onMixAudio(double t, bool reseek) = 0;
    virtual void onStartAudio(double t) = 0;
    virtual void onStopAudio() = 0;
    virtual void onStopPlaybackUI() {}
    // Estado do transporte → o widget repassa aos sinais.
    virtual void onPlayheadMoved(double t) = 0;
    virtual void onStateChanged(bool playing) = 0;
    // Relógio do sink de áudio (segundos consumidos; -1 = sem áudio).
    virtual double audioClockSec() const = 0;
    // Clipe de vídeo no topo em `t` (default: faixas de vídeo).
    virtual const Clip* clipAt(double t) const;

    Project* m_project = nullptr;

    // ── Transport state ─────────────────────────────────────────────
    double m_playhead = 0.0;
    double m_loopIn = -1.0;
    double m_loopOut = -1.0;
    bool m_loopEnabled = false;
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock;
    double m_playStart = 0.0;
    double m_audioAnchor = 0.0;
    bool m_audioClockOn = false;
    double m_audioLastRaw = -1.0;
    qint64 m_lastAnchorClockMs = -1;
    // Segura o vídeo no frame inicial até o sink de áudio começar a consumir
    // de verdade (dispositivo a acordar): evita o vídeo andar adiante e
    // "pular" a agulha quando o áudio finalmente toca. Timeout opcional em ms.
    bool m_awaitingAudio = false;
    qint64 m_awaitAudioDeadlineMs = -1;
    qint64 m_currentFrameIndex = -1;
    double m_lastTickSec = -1.0;   // momento do tick anterior (rate-limit do slew)
    bool m_playing = true;
    double m_playRate = 1.0;
    QPushButton* m_playBtn = nullptr;
    qint64 m_droppedFrames = 0;

    // Registro do tick em curso. O tick o zera, mede as etapas e o submete ao
    // PreviewProfiler; o PreviewWidget (derivado) escreve nos mesmos campos
    // nas suas próprias fases (worker, prefetch, paint, proxy) — assim as
    // medições de pipeline landing em fases diferentes do tick acabar no mesmo
    // registro, sem um segundo canal de comunicação.
    PreviewProfiler::Frame m_prof;
    qint64 m_lastTickStartNs = 0; // início do tick anterior (mede atraso do loop)
    qint64 m_stallNs = 0;          // atraso do último tick sobre o agendamento
    // Atraso acima do qual o bloqueio do event loop vale um qWarning. Baixo o
    // bastante para pegar um travamento de meio segundo sem poluir o log.
    static constexpr qint64 kStallReportNs = 100000000; // 100 ms
    // m_droppedFrames é drenado por consumeDroppedFrames() (que o zera) para o
    // overlay, então não serve como total: somar a série contaria o mesmo drop
    // várias vezes. Estes dois são monotônicos e nunca zeram — a série passa a
    // registrar o delta por tick e a soma da série bate com o total reportado.
    qint64 m_droppedMonotonic = 0;
    qint64 m_droppedProfSnapshot = 0;
    qint64 m_profSeq = 0;

protected:
    // Slot do registro do tick atual, para o widget preenchê-lo.
    PreviewProfiler::Frame& profFrame() { return m_prof; }

private:
    void applySeekInternal(double t);
    void anchorAudioClock(double t);
    void stopPlaybackInternal();
    void setFrameInterval();
    void tickImpl();
};
