// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QRect>
#include <QMutex>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
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

class QToolButton;
class QMenu;

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
    // Toca a partir de uma posição (Enter: início no ponteiro da timeline).
    void playFrom(double t);
    void setLoopRange(double in, double out);
    void setLoopEnabled(bool enabled); // "Q": liga/desliga o loop de reprodução
    void setZoom(double z);
    void setPreviewQuality(double q); // qualidade de decodificação do preview
signals:
    void playheadMoved(double t);
    void stateChanged(bool playing);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void tick();
    void drawEmptyMonitor(QPainter& p, const QRect& canvas);
    void drawClipText(QPainter& p, const QRect& canvas, const Clip* clip, double k);
    void updateFrame();
    void applyCrop();
    QImage applyCropTo(const QImage& img, int cL, int cR, int cT, int cB);
    // Pedidos asíncronos de quadro: primário (clipe do topo) e camadas
    // inferiores (empilhamento multi-faixa). clipId identifica o destino.
    void requestFrame(const QString& clipId, const QString& path, double t, int maxW);
    void requestLowerLayers(int decW);
    void kickFrameWorker();
    void onFrameReady(const QString& clipId, const QString& path, double t, int maxW, const QImage& img);
    void onPrefetchReady(const QString& path, double t, int maxW, const QImage& img);
    void updatePrefetch();
    void stopPlayback();
    void startAudio(double t);
    void stopAudio();
    void updateMixAudio(double t, bool reseek);
    const Clip* clipAt(double t) const;
    Project* m_project = nullptr;
    double m_playhead = 0.0;
    double m_loopIn = -1.0;
    double m_loopOut = -1.0;
    bool m_loopEnabled = false; // loop de reprodução só quando ativado ("Q")
    QImage m_frame;
    QImage m_frameFull;
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock;
    double m_playStart = 0.0;
    qint64 m_currentFrameIndex = -1;
    bool m_playing = true;
    QPushButton* m_playBtn = nullptr;
    QLabel* m_timeLabel = nullptr;
    QComboBox* m_zoomCombo = nullptr;
    QWidget* m_topBar = nullptr;
    double m_zoom = 0.0; // 0 = ajustar à área; senão fração (1.0 = 100%)
    double m_previewQuality = 1.0; // fator de resolução de decodificação (0..1)
    QToolButton* m_qualityBtn = nullptr;
    QMenu* m_qualityMenu = nullptr;
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
    // Thread dedicada para prefetch: evita que decodePrefetch fique bloqueado
    // quando o worker principal está ocupado decodificando frames.
    QThread* m_prefetchThread = nullptr;
    FrameWorker* m_prefetchWorker = nullptr;
    // Pedido de decodificação. clipId diz para qual clipe o quadro se destina:
    // o clipe do topo alimenta m_frame; os demais alimentam m_layerCache.
    struct FrameReq {
        QString clipId;
        QString path;
        double t = 0.0;
        double dt = 1.0 / 30.0;
        int maxW = 0;
    };
    struct PrefetchFrame {
        QString path;
        double t = 0.0;
        int maxW = 0;
        QImage img;
        bool valid = false;
        bool requested = false;
    };
    // Quadro decodificado de um clipe de camada inferior (não-topo), com a
    // chave de cache para não re-decodificar enquanto o playhead não mudou.
    struct LayerFrame {
        QImage img;   // já com pan/crop aplicado
        QString path;
        double t = 0.0;
        int maxW = 0;
    };
    QMutex m_frameMutex;
    QVector<FrameReq> m_reqQueue;                       // fila de decodificações
    QHash<QString, LayerFrame> m_layerCache;            // clipId -> quadro inferior
    PrefetchFrame m_prefetch;
    bool m_workerBusy = false;
    QString m_shownPath;
    double m_shownT = -1.0;
    int m_shownW = -1;

    // Transição ativa: quadro do clipe de trás (A) para compor com o da frente
    // (B) durante a sobreposição. m_transAlpha = progresso 0..1 (-1 = nenhuma).
    QImage m_underFrame;
    QString m_underPath;
    double m_underT = -1.0;
    int m_underW = -1;
    bool m_underRequested = false;
    int m_underCropL = 0, m_underCropR = 0, m_underCropT = 0, m_underCropB = 0;
    double m_transAlpha = -1.0;
    QString m_transType;
};
