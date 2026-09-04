// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QElapsedTimer>

#include "models/Project.h"

class QSlider;
class QLabel;
class QPushButton;
class QTimer;
class QHBoxLayout;
class QScrollArea;
class Project;
class PreviewWidget;

// ── Conversões dB ↔ linear ─────────────────────────────────────────────

double volToDb(double vol);   // 0→-inf, 1→0 dB, 2→+6 dB
double dbToVol(double db);    // -inf→0, 0→1, +6→2

// ── VU Meter vertical estilo LED com peak hold ────────────────────────

class VuMeter : public QWidget {
    Q_OBJECT
public:
    explicit VuMeter(QWidget* parent = nullptr);
    void setLevel(float rms); // 0..1
    QSize sizeHint() const override { return QSize(12, 180); }
    QSize minimumSizeHint() const override { return QSize(8, 60); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    float m_level = 0.0f;
    float m_peak = 0.0f;
    QElapsedTimer m_peakTimer;
};

// ── Knob de pan rotativo ─────────────────────────────────────────────

class PanKnob : public QWidget {
    Q_OBJECT
public:
    explicit PanKnob(QWidget* parent = nullptr);
    void setPan(double pan); // -1..+1
    double value() const { return m_pan; } // -1..+1
    QSize sizeHint() const override { return QSize(30, 30); }
    QSize minimumSizeHint() const override { return QSize(24, 24); }
signals:
    void panChanged(double pan);
    void panTouchedUp(); // mouse press (início do toque)
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    double m_pan = 0.0;
    bool m_dragging = false;
};

// ── Strip individual (faixa ou master) ────────────────────────────────

class MixerStrip : public QWidget {
    Q_OBJECT
public:
    MixerStrip(const QString& name, int trackIndex, bool isAudio,
               bool isMaster = false, QWidget* parent = nullptr);
    void setVolume(double vol);   // 0..2
    void setPan(double pan);      // -1..+1
    void setMuted(bool m);
    void setSolo(bool s);
    void setRmsLevel(float rms);  // 0..1
    void setTrackIndex(int idx) { m_trackIndex = idx; }
    void setIsAudio(bool a) { m_isAudio = a; }
    int trackIndex() const { return m_trackIndex; }
    bool isAudio() const { return m_isAudio; }
    double volume() const;        // 0..2 (posição atual do fader)
    double pan() const;           // -1..+1 (posição atual do knob)
    // Arma/desarma a escrita de automação e atualiza o visual do botão.
    // armed==false em modo "read" mostra o envelope (verde); armed mostra
    // o botão aceso conforme o modo (T/W/L).
    void setAutomationArmed(bool armed, int mode);
    int automationMode() const { return m_autoMode; }
    bool automationArmed() const { return m_autoArmed; }
signals:
    void volumeChanged(int trackIndex, bool isAudio, double vol);
    void panChanged(int trackIndex, bool isAudio, double pan);
    void muteChanged(int trackIndex, bool isAudio, bool muted);
    void soloChanged(int trackIndex, bool isAudio, bool solo);
    // Emitido quando o usuário toca o fader/knob (início da gravação Touch).
    void volumeTouched(int trackIndex, bool isAudio);
    void panTouched(int trackIndex, bool isAudio);
    // Quando o usuário solta o fader/knob (fim do toque Touch).
    void volumeReleased(int trackIndex, bool isAudio);
    void panReleased(int trackIndex, bool isAudio);
    // Botão de automação clicado: alterna armed e emite o modo resultante.
    void automationToggled(int trackIndex, bool isAudio, bool armed, int mode);
private:
    int m_trackIndex;
    bool m_isAudio;
    bool m_isMaster;
    QSlider* m_fader = nullptr;
    QLabel* m_volLabel = nullptr;
    PanKnob* m_panKnob = nullptr;
    QPushButton* m_muteBtn = nullptr;
    QPushButton* m_soloBtn = nullptr;
    VuMeter* m_meter = nullptr;
    QPushButton* m_autoBtn = nullptr;   // botão de automação (T/W/L/read)
    bool m_updating = false;
    bool m_autoArmed = false;
    int m_autoMode = 0;   // 0 Touch, 1 Write, 2 Latch
    void updateAutoButton();
};

// ── Widget principal do Mixer (dock) ──────────────────────────────────

class MixerWidget : public QWidget {
    Q_OBJECT
public:
    explicit MixerWidget(QWidget* parent = nullptr);
    void setProject(Project* p);
    void setPreview(PreviewWidget* pw);
public slots:
    void refresh();
    void updateLevels();
    // Chamado pela MainWindow a cada movimento de playhead (playheadMoved) e
    // transição de reprodução (stateChanged): grava automação nas faixas
    // armadas (modos Write/Touch/Latch) na posição `t`.
    void setPlayhead(double t);
    void setPlaying(bool playing);
    // true se alguma faixa tem automação gravada (para consolidar o undo).
    bool hasAutomation() const;
signals:
    void modified();
private:
    void clearStrips();
    // Grava um keyframe de automação na faixa (se armada e permitido pelo modo).
    Track* findTrack(bool isAudio, int index);
    void beginTouch(bool isAudio, int index, const QString& prop);
    void endTouch(bool isAudio, int index, const QString& prop);
    void writeAutoPoint(bool isAudio, int index, const QString& prop, double value);
    Project* m_project = nullptr;
    PreviewWidget* m_preview = nullptr;
    QVector<MixerStrip*> m_videoStrips;
    QVector<MixerStrip*> m_audioStrips;
    MixerStrip* m_masterStrip = nullptr;
    QTimer* m_levelTimer = nullptr;
    QHBoxLayout* m_channelsLayout = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    double m_playhead = 0.0;
    bool m_playing = false;
    // Toque ativo por faixa+propriedade (para modo Touch gravar só enquanto seguro).
    QSet<QPair<QPair<bool,int>, QString>> m_touching;
};
