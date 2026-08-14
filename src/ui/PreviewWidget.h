// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QRect>
#include <QMutex>
#include <QElapsedTimer>
#include "models/Project.h"
#include "ffmpeg/FFmpegDecoder.h"

class QTimer;
class QPushButton;
class QLabel;
class QComboBox;
class QThread;

class AudioMixer; // QIODevice que mistura o PCM de todos os clipes ativos
class FrameWorker; // decodifica quadros de vídeo fora da thread da UI
class QAudioSink;
class QAudioOutput;

class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;
    void setProject(Project* p);
    void refreshView();
    double playhead() const { return m_playhead; }
    static int maxDecodeWidth();
    static void setMaxDecodeWidth(int w);
public slots:
    void seek(double t);
    void togglePlay();
    void setLoopRange(double in, double out);
    void setZoom(double z);
signals:
    void playheadMoved(double t);
    void stateChanged(bool playing);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void tick();
    void drawEmptyMonitor(QPainter& p, const QRect& canvas);
    void updateFrame();
    void applyCrop();
    void requestFrame(const QString& path, double t, int maxW);
    void kickFrameWorker();
    void onFrameReady(const QString& path, double t, int maxW, const QImage& img);
    void stopPlayback();
    void startAudio(double t);
    void stopAudio();
    void updateMixAudio(double t, bool reseek);
    const Clip* clipAt(double t) const;
    Project* m_project = nullptr;
    double m_playhead = 0.0;
    double m_loopIn = -1.0;
    double m_loopOut = -1.0;
    QImage m_frame;
    QImage m_frameFull;
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock;
    double m_playStart = 0.0;
    bool m_playing = false;
    QPushButton* m_playBtn = nullptr;
    QLabel* m_timeLabel = nullptr;
    QComboBox* m_zoomCombo = nullptr;
    QWidget* m_topBar = nullptr;
    double m_zoom = 0.0; // 0 = ajustar à área; senão fração (1.0 = 100%)
    QRect m_videoRect;
    double m_lastSrcT = -1.0;
    int m_lastDecodeW = -1;
    QString m_lastFile;
    int m_lastCropL = -1, m_lastCropR = -1, m_lastCropT = -1, m_lastCropB = -1;

    // Áudio do preview (mixer com um decoder por clipe ativo).
    AudioMixer* m_audioFeed = nullptr;
    QAudioSink* m_audioSink = nullptr;
    QAudioOutput* m_audioOut = nullptr;

    // Decodificação de vídeo em thread própria (não trava a UI na reprodução).
    QThread* m_frameThread = nullptr;
    FrameWorker* m_frameWorker = nullptr;
    struct FrameReq { QString path; double t = 0.0; int maxW = 0; bool valid = false; };
    QMutex m_frameMutex;
    FrameReq m_pendingReq;
    bool m_workerBusy = false;
    QString m_shownPath;
    double m_shownT = -1.0;
    int m_shownW = -1;
};
