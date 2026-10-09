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
#include <QPoint>
#include <QElapsedTimer>

#include "colombina/models/Project.h"

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

// ── VU Meter vertical estilo Premiere: gradiente contínuo, peak hold
//    e clip light no topo (liga em vermelho quando pico >= -0.1 dBFS).

class VuMeter : public QWidget {
    Q_OBJECT
public:
    explicit VuMeter(QWidget* parent = nullptr);
    void setLevel(float rms); // 0..1 (linear)
    QSize sizeHint() const override { return QSize(11, 180); }
    QSize minimumSizeHint() const override { return QSize(9, 60); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    float m_db = -60.0f;
    float m_peakDb = -60.0f;
    QElapsedTimer m_peakTimer;
};

// ── Escala em dB (0..-54) colada à direita do VU meter ──────────────

class MeterScale : public QWidget {
    Q_OBJECT
public:
    explicit MeterScale(QWidget* parent = nullptr);
    QSize sizeHint() const override { return QSize(24, 180); }
    QSize minimumSizeHint() const override { return QSize(22, 60); }
protected:
    void paintEvent(QPaintEvent*) override;
};

// ── Knob de pan rotativo estilo filmcraft: anel com ponteiro, L/R ────

class PanKnob : public QWidget {
    Q_OBJECT
public:
    explicit PanKnob(QWidget* parent = nullptr);
    void setPan(double pan); // -1..+1
    double value() const { return m_pan; } // -1..+1
    QSize sizeHint() const override { return QSize(36, 44); }
    QSize minimumSizeHint() const override { return QSize(34, 40); }
signals:
    void panChanged(double pan);
    void panTouchedUp(); // mouse press (início do toque)
    void panReleased();  // mouse release (fim do toque)
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    double m_pan = 0.0;
    bool m_dragging = false;
    QPoint m_lastPos;
};

// ── Fader de volume estilo Premiere: escala dB + groove + cap ────────
// Lei de taper do filmcraft (mais curso próximo da unidade).
// API compativel com o antigo QSlider (0..200 = volume x100).

class FaderSlider : public QWidget {
    Q_OBJECT
public:
    explicit FaderSlider(QWidget* parent = nullptr);
    int value() const { return m_value; }      // 0..200 (vol*100)
    void setValue(int v);                      // não emite signal
    void setRange(int, int) {}                 // compat (fixo 0..200)
    QSize sizeHint() const override { return QSize(52, 180); }
    QSize minimumSizeHint() const override { return QSize(48, 60); }
signals:
    void valueChanged(int value);
    void sliderPressed();
    void sliderReleased();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    double yPosToDb(double y) const;
    double dbToYPos(double db) const;
    int m_value = 100;
    bool m_drag = false;
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
    FaderSlider* m_fader = nullptr;
    QLabel* m_volLabel = nullptr;
    PanKnob* m_panKnob = nullptr;
    QPushButton* m_muteBtn = nullptr;
    QPushButton* m_soloBtn = nullptr;
    VuMeter* m_meter = nullptr;
    MeterScale* m_meterScale = nullptr;
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
