// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QPair>
#include <QElapsedTimer>

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
    QSize sizeHint() const override { return QSize(30, 30); }
    QSize minimumSizeHint() const override { return QSize(24, 24); }
signals:
    void panChanged(double pan);
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
signals:
    void volumeChanged(int trackIndex, bool isAudio, double vol);
    void panChanged(int trackIndex, bool isAudio, double pan);
    void muteChanged(int trackIndex, bool isAudio, bool muted);
    void soloChanged(int trackIndex, bool isAudio, bool solo);
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
    bool m_updating = false;
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
signals:
    void modified();
private:
    void clearStrips();
    Project* m_project = nullptr;
    PreviewWidget* m_preview = nullptr;
    QVector<MixerStrip*> m_videoStrips;
    QVector<MixerStrip*> m_audioStrips;
    MixerStrip* m_masterStrip = nullptr;
    QTimer* m_levelTimer = nullptr;
    QHBoxLayout* m_channelsLayout = nullptr;
    QScrollArea* m_scrollArea = nullptr;
};
