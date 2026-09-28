// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QRect>
#include <QMutex>
#include <atomic>
#include <QElapsedTimer>
#include <QHash>
#include <QPair>
#include <QImage>
#include <QIcon>
#include <QTransform>
#include "colombina/models/Project.h"
#include "colombina/ffmpeg/FFmpegDecoder.h"
#include "colombina/render/MesaRenderer.h"
#include "ui/PlaybackEngine.h"

class QTimer;
class QPushButton;
class QLabel;
class QComboBox;
class QThread;

class AudioMixer; // QIODevice que mistura o PCM de todos os clipes ativos
class FrameWorker; // decodifica quadros de vídeo fora da thread da UI
class BgPrefetchWorker; // prefetch em thread separada (não bloqueia o worker)
class QAudioSink;
class QAudioOutput;

class QToolButton;
class QMenu;

class OfxPluginManager;

class PreviewWidget : public QWidget, public PlaybackEngine {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;
    void setProject(Project* p);
    void setOfxManager(OfxPluginManager* m) { m_ofxManager = m; }
    void refreshView();

    // Retorna uma cópia REDUZIDA (160×90) do quadro composto atual, para os
    // analisadores (waveform/vectorscope/histograma). Vazio se sem quadro.
    QImage scopesFrame() const;

    // Cópia (implicitamente compartilhada) do sinal de vídeo ATUAL do monitor:
    // a composição final com todas as camadas inferiores, SEM overlays da
    // interface. Usada pela janela de preview externo. Vazia se sem quadro.
    QImage compositeFrame() const;

    // Níveis de áudio para o MixerWidget (thread-safe).
    struct AudioLevels {
        QHash<QPair<bool,int>, float> rms; // (isAudio, trackIndex) → RMS 0..1
        float masterRms = 0.0;
    };
    AudioLevels audioLevels() const;

    // ── Overlay de edição de máscara ─────────────────────────────────
    // Ativa o desenho interativo das forma(s) do clipe `clipId` sobre o
    // monitor, com alças arrastáveis (mover, redimensionar, rotacionar).
    // `masks` vazio ou `clipId` vazio desliga o overlay.
    void setMaskOverlay(const QString& clipId, const QVector<Mask>& masks);
public slots:
    // Transporte: repassa ao PlaybackEngine. Necessário para os connect() de
    // QAction/QPushButton continuarem funcionando via slots do widget.
    void seek(double t);
    void togglePlay();
    void shuttle(int dir);
    void playFrom(double t);
    void setLoopRange(double in, double out);
    void setLoopEnabled(bool enabled);
    void setZoom(double z);
    void setPreviewQuality(int width); // largura máxima de decodificação (360/480/720/1080/3840)
signals:
    void playheadMoved(double t);
    void stateChanged(bool playing);
    // Durante o arrasto das alças do overlay de máscara: índice + máscara
    // atualizada (já aplicada ao overlay). O dialog aplica à cópia de trabalho.
    void maskEdited(int maskIndex, const Mask& updated);
    void maskDragEnd();
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    const Clip* clipAt(double t) const override;
    double audioClockSec() const override;
 private:
    // Aplica o QSS do contador de tempo a partir dos tokens do tema, para que
    // ele acompanhe a troca claro/escuro.
    void refreshTimeLabelStyle();
    // Quadro a quadro (Premiere): pausa a reprodução e busca ±1 frame.
    void stepFrame(int dir);
    // ── Hooks do PlaybackEngine (chamados por onSeek/onPrefetch/...) ──
    void applySeekVisual(double t);  // decodifica e desenha o quadro em `t`
    void onSeek(double t) override;
    void onPrefetch() override;
    void onMixAudio(double t, bool reseek) override;
    void onStartAudio(double t) override;
    void onStopAudio() override;
    void onStopPlaybackUI() override;
    void onPlayheadMoved(double t) override;
    void onStateChanged(bool playing) override;
    void drawEmptyMonitor(QPainter& p, const QRect& canvas);
    void drawClipText(QPainter& p, const QRect& canvas, const Clip* clip, double k);
    void updateFrame();
    void applyCrop();
    void applyBasicEffects(QImage& img);
    static void applyBasicEffectsOn(QImage& img, const Clip& c, double rel = 0.0);
    // Aplica as máscaras (shapes rect/ellipse) do clipe ao quadro, multiplicando
    // o alpha pela cobertura (feather + invert, em união). `rel` = tempo
    // relativo do clipe (avalia os keyframes das máscaras).
    static void applyMasks(QImage& img, const Clip& c, double rel);
    QImage applyCropTo(const QImage& img, int cL, int cR, int cT, int cB);
    // Pedidos asíncronos de quadro: primário (clipe do topo) e camadas
    // inferiores (empilhamento multi-faixa). clipId identifica o destino.
    void requestFrame(const QString& clipId, const QString& path, double t, int maxW);
    void requestLowerLayers(int decW);
    void kickFrameWorker();
    void drawPerfOverlay(QPainter& p);
    void onFrameReady(const QString& clipId, const QString& path, double t, int maxW, const QImage& img);
    void onPrefetchReady(const QString& path, double t, int maxW, const QImage& img);
    void onBgPrefetchDone(const QString& path, double t, int maxW,
                          const QImage& frame0, const QImage& frame1, FFmpegDecoder* decoder);
    void onBgPrefetchFailed(const QString& path);
    void updatePrefetch();
    void stopAudio();
    void startAudio(double t);
    void updateMixAudio(double t, bool reseek);
    // Se o clipe ativo pertence a um grupo Mesa, renderiza a composição
    // (com transform de câmera) e devolve true preenchendo m_frameFull/m_frame.
    bool tryRenderMesa(const Clip* clip);
    QImage m_frame;
    QImage m_frameFull;
    QLabel* m_timeLabel = nullptr;
    QComboBox* m_zoomCombo = nullptr;
    QWidget* m_topBar = nullptr;    // aba "Program: <nome>" (topo, como no Premiere)
    QWidget* m_bottomBar = nullptr; // transporte + quality/zoom (rodapé, como no Premiere)
    double m_zoom = 0.0; // 0 = ajustar à área; senão fração (1.0 = 100%)
    int m_previewQuality = 720; // largura máxima de decodificação (360/480/720/1080/3840)
    QToolButton* m_qualityBtn = nullptr;
    QMenu* m_qualityMenu = nullptr;
    // Transporte do monitor (estilo Premiere): quadro anterior, play/pausa,
    // quadro seguinte e loop. Botões planos com ícones desenhados do tema.
    QToolButton* m_stepBackBtn = nullptr;
    QToolButton* m_stepFwdBtn = nullptr;
    QToolButton* m_loopBtn = nullptr;
    QToolButton* m_fullscreenBtn = nullptr;
    QLabel* m_programLabel = nullptr; // "Program: <nome da sequência>"
    QIcon m_playIcon;
    QIcon m_pauseIcon;
    QRect m_videoRect;
    double m_lastSrcT = -1.0;
    int m_lastDecodeW = -1;
    QString m_lastFile;
    int m_lastCropL = -1, m_lastCropR = -1, m_lastCropT = -1, m_lastCropB = -1;

    // Estado do efeito LAINKA (stop motion) do clipe ativo.
    bool m_clipLainkaEnabled = false;
    int m_clipLainkaSkip = 1;
    double m_clipLainkaJitterPos = 0.0;
    double m_clipLainkaJitterRot = 0.0;
    double m_clipLainkaJitterScale = 0.0;
    double m_clipLainkaFlicker = 0.0;
    double m_clipLainkaFlickerSpeed = 50.0;
    double m_clipLainkaWarpAmount = 0.0;
    double m_clipLainkaWarpSpeed = 50.0;
    int m_clipLainkaWarpGrid = 8;
    double m_clipLainkaOnionSkin = 0.0;
    double m_clipLainkaDustAmount = 0.0;
    double m_clipLainkaScratchAmount = 0.0;
    int m_clipLainkaTargetFps = 8;
    double m_clipLainkaMotionBlur = 0.0;
    double m_clipLainkaOpacity = 100.0;
    int m_clipLainkaAntialias = 1;
    QString m_clipLainkaId;
    double m_lainkaQuantizedTime = -1.0; // tempo quantizado para efeitos LAINKA
    QImage m_lainkaPrevFrame; // para onion skin (ghosting)

    // ── MotiOn cached state ───────────────────────────────────────────
    bool m_clipMotionEnabled = false;
    double m_clipMotionAmount = 0.0;
    double m_clipMotionAngle = 0.0;
    int m_clipMotionSamples = 8;

    // ── Efeitos básicos cached state ─────────────────────────────────
    double m_clipBrightness = 0.0;
    double m_clipContrast = 1.0;
    double m_clipSaturation = 1.0;
    double m_clipBlur = 0.0;
    bool m_clipGrayscale = false;
    bool m_clipChromaKey = false;
    QColor m_clipChromaKeyColor{Qt::green};
    double m_clipChromaKeySimilarity = 0.15;
    QVector<Mask> m_clipMasks; // máscaras do clipe ativo (aplicadas no crop)

    // Áudio do preview (mixer com um decoder por clipe ativo).
    AudioMixer* m_audioFeed = nullptr;
    QAudioSink* m_audioSink = nullptr;
    QAudioOutput* m_audioOut = nullptr;
    bool m_audioConformWarmed = false; // true após a primeira reprodução (pula wait no 1º play)

    // Decodificação de vídeo em thread própria (não trava a UI na reprodução).
    QThread* m_frameThread = nullptr;
    FrameWorker* m_frameWorker = nullptr;
    // Thread dedicada ao prefetch: decodifica o próximo clipe sem bloquear o
    // FrameWorker principal (que mantém o pipeline m_ready em cadência).
    QThread* m_bgPrefetchThread = nullptr;
    BgPrefetchWorker* m_bgPrefetchWorker = nullptr;
    // true enquanto um prefetch em background está em andamento
    bool m_bgPrefetchBusy = false;
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
        double clipEnd = -1.0; // fim do clipe atual no momento da solicitação
        int maxW = 0;
        QImage img;
        bool valid = false;
        bool requested = false;
        bool invoked = false;   // decodePrefetch já foi despachado (não re-despachar)
    };
    // Quadro decodificado de um clipe de camada inferior (não-topo), com a
    // chave de cache para não re-decodificar enquanto o playhead não mudou.
    struct LayerFrame {
        QImage img;   // já com pan/crop aplicado
        QString path;
        double t = 0.0;
        int maxW = 0;
    };
    mutable QMutex m_frameMutex;
    QVector<FrameReq> m_reqQueue;                       // fila de decodificações
    QHash<QString, LayerFrame> m_layerCache;            // clipId -> quadro inferior
    PrefetchFrame m_prefetch;
    bool m_workerBusy = false;
    QString m_shownPath;
    // Diagnóstico de performance (overlay com PIERROT_PERF_DEBUG=1).
    QElapsedTimer m_perfT;
    qint64 m_perfWorkerStartNs = 0;
    qint64 m_perfPrefetchStartNs = 0;
    struct PerfDbg {
        qint64 seekMs = 0;      // applySeek() dentro do tick
        qint64 prefetchMs = 0;  // updatePrefetch() dentro do tick
        qint64 mixMs = 0;       // updateMixAudio() dentro do tick
        qint64 totalMs = 0;     // soma dos três
        qint64 workerMs = 0;    // kick -> onFrameReady (latência do decode)
        qint64 prefetchLatMs = 0; // pedido -> prefetchReady (swap pronto?)
        bool cut = false;       // playhead cruzou para outro clipe
        int cutCount = 0;       // cortes cruzados (para estatística)
        qint64 droppedTotal = 0; // frames perdidos (acumulado session)
    } m_perf;

    // ── Adaptive quality ────────────────────────────────────────────
    // Monitora latência do decode e baixa resolução automaticamente
    // quando o decode não acompanha o playback.
    double m_adaptiveDecodeMsAvg = 0.0;   // média móvel de latência decode
    int m_adaptiveSlowCount = 0;          // ticks consecutivos onde decode > frame interval
    int m_adaptiveBaseQuality = 720;      // qualidade original (antes de baixar)
    bool m_adaptiveActive = false;        // estamos em modo adaptativo?
    double m_shownT = -1.0;
    int m_shownW = -1;
    // Desengasgo periódico do vídeo: a cada ~10s de reprodução o FrameWorker
    // dá um flush leve (libera DPB/caches). Vídeo longo engasga e áudio fica
    // perfeito porque o decode contínuo degrada com o tempo.
    QElapsedTimer m_desengasgaT;
    qint64 m_desengasgaLastMs = 0;

    // Cache do frame composto (evita recomposição a cada paintEvent).
    QImage m_compositedCache;
    quint64 m_compositedEpoch = 0;      // frame index quando o cache foi gerado
    int m_compositedLayerCount = 0;     // número de camadas quando o cache foi gerado

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

    // ── OFX plugin manager ────────────────────────────────────────────
    OfxPluginManager* m_ofxManager = nullptr;
    QVector<OfxPluginInstance> m_clipOfxFx; // efeitos OFX do clipe ativo

    // ── Margens de segurança do monitor (Action 90% + Title 80%) ─────────
    bool m_showGrid = false;
    QToolButton* m_gridBtn = nullptr;
    void drawGrid(QPainter& p, const QRect& canvas);

    // ── Overlay de edição de máscara ─────────────────────────────────
    QString m_maskOverlayClipId;   // clipe sob edição (vazio = nenhum)
    QVector<Mask> m_maskOverlay;   // cópia de trabalho das máscaras
    int m_maskDragIndex = -1;      // máscara sob o mouse durante o arrasto
    int m_maskDragHandle = -1;     // 0=centro, 1=topo(rotação), 2..5=bordas
    QPoint m_maskDragLast;         // último ponto do mouse (coords do widget)
    double m_maskDragPressRot = 0.0; // rotação no início do arrasto (graus)
    double m_maskDragPressAng = 0.0; // ângulo cursor→centro no início (rad)
    QTransform m_maskToScreen;     // imagem (crop) do clipe → monitor
    QSize m_maskAnchorSize;        // dimensões da imagem de referência
    void drawMaskOverlay(QPainter& p, const QRect& canvas, double k);
    // Marca a alça sob `pos` como arrastada; false se não houver alça.
    bool pickMaskHandle(const QPoint& pos);
    void applyMaskDrag(const QPoint& pos);

    // Renderiza o canvas de uma Mesa (sem câmera) quando o clipe ativo
    // pertence a um grupo Mesa. Usa um MesaRenderer dedicado.
    MesaRenderer m_mesaRenderer;
};
