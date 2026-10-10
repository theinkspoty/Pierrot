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
#include <QMetaType>
#include "colombina/models/Project.h"
#include "laartman/FFmpegDecoder.h"
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
class DecoderPool; // pool de decodificadores quentes por caminho de arquivo
class QAudioSink;
class QAudioOutput;

class QToolButton;
class QMenu;

class OfxPluginManager;
class Frei0rPluginManager;
class ScopeWidget;

// Snapshot dos parâmetros de efeitos NÃO temporais (crop + efeitos básicos +
// máscaras) de um clipe, para que o FrameWorker os aplique na thread de vídeo,
// fora do caminho síncrono da UI. Só é ativado para clipes sem LAINKA/MotiOn/
// OFX/frei0r — esses continuam sendo processados na UI. `active=false` preserva
// o caminho atual (quadro cru chega à UI e applyCrop() roda nela).
struct FrameFx {
    bool active = false;
    int cropL = 0;
    int cropR = 0;
    int cropT = 0;
    int cropB = 0;
    double rel = 0.0;   // tempo relativo do clipe p/ keyframes (máscaras/efeitos)
    Clip clip;          // efeitos básicos + máscaras (snapshot; COW barato)
};
Q_DECLARE_METATYPE(FrameFx)

class PreviewWidget : public QWidget, public PlaybackEngine {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;
    void setProject(Project* p);
    void setOfxManager(OfxPluginManager* m) { m_ofxManager = m; }
    void setFrei0rManager(Frei0rPluginManager* m) { m_frei0rManager = m; }
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
    void stepFrameBy(int dir); // ±1 quadro (usado pela janela de preview externo)
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
    // A thread de vídeo (FrameWorker) precisa chamar os estáticos de efeito
    // (applyCropTo/applyBasicEffectsOn) para aplicar o caminho rápido.
    friend class FrameWorker;
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
    static QImage applyCropTo(const QImage& img, int cL, int cR, int cT, int cB);
    // Pedidos asíncronos de quadro: primário (clipe do topo) e camadas
    // inferiores (empilhamento multi-faixa). clipId identifica o destino.
    void requestFrame(const QString& clipId, const QString& path, double t, int maxW,
                      const FrameFx& fx = FrameFx());
    void requestLowerLayers(int decW);
    void kickFrameWorker();
    // resolveVideo() com medição de custo (a chamada faz um QFile::exists sob
    // o mutex do ProxyManager e roda várias vezes por tick).
    QString resolvePreviewVideo(const QString& srcPath);
    void renderFrame(QPainter& p);
    void drawPlaybackBadges(QPainter& p);
    void drawPerfOverlay(QPainter& p);
    void onFrameReady(const QString& clipId, const QString& path, double t, int maxW, const QImage& img,
                      bool processed);
    void onPrefetchReady(const QString& path, double t, int maxW, const QImage& img);
    void onBgPrefetchDone(const QString& path, double t, int maxW,
                          const QImage& frame0, const QImage& frame1);
    void onBgPrefetchFailed(const QString& path);
    void onBgWarmDone(const QString& path);
    void kickWarmQueue();
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
    ScopeWidget* m_miniScope = nullptr; // waveform compacto no Program Monitor
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

    // ── Memo de applyCrop() para LAINKA em stop motion ─────────────────
    // Com skip>1 o tempo quantizado só muda a cada N quadros, mas a cadeia
    // crop -> LAINKA -> MotiOn -> efeitos básicos roda todo quadro. Medido no
    // bogaboga.Blanc (LAINKA warp 8x8 + MotiOn de 7 amostras, PNG 512x512
    // esticado para 1920x1080): 9,9 ms por quadro numa janela de 0,68 s — 18x
    // a linha de base — com saída idêntica em 6 de cada 7 quadros. Como o
    // custo é determinístico no tempo quantizado, guardar o resultado derruba
    // a janela para ~1/6. Só existe enquanto m_clipLainkaEnabled: fora disso o
    // tempo não quantizado muda todo quadro e não haveria acerto, e o cache
    // ficaria segurando uma imagem de 8 MB à toa.
    QImage m_cropMemo;
    QString m_cropMemoClipId;
    double m_cropMemoTime = -1.0; // tempo quantizado (m_lainkaQuantizedTime)
    double m_cropMemoSrcT = -1.0; // tempo fonte do quadro (m_lastSrcT)
    int m_cropMemoSrcW = 0;       // largura de decodificação (m_lastDecodeW)
    // O crop é avaliado nos keyframes com o tempo NÃO quantizado, então pode
    // andar dentro da janela de stop motion: precisa entrar na chave.
    int m_cropMemoCropL = 0, m_cropMemoCropR = 0, m_cropMemoCropT = 0, m_cropMemoCropB = 0;

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
    double m_clipChromaKeySoftness = 0.10;
    double m_clipChromaKeySpillSuppress = 0.5;
    bool m_clipPsxEnabled = false;
    double m_clipPsxDither = 1.0;
    int m_clipPsxBits = 5;
    QVector<Mask> m_clipMasks; // máscaras do clipe ativo (aplicadas no crop)

    // Áudio do preview (mixer com um decoder por clipe ativo).
    AudioMixer* m_audioFeed = nullptr;
    // Sink reutilizado entre play/stop (evita new/destroy de 10-50ms por play).
    // Recriado só quando a saída padrão muda (id guardado em *_Device).
    QAudioSink* m_audioSink = nullptr;
    QAudioOutput* m_audioOut = nullptr;
    QString m_audioSinkDevice;
    QString m_audioOutDevice;

    // Decodificação de vídeo em thread própria (não trava a UI na reprodução).
    QThread* m_frameThread = nullptr;
    FrameWorker* m_frameWorker = nullptr;
    // Pool de decodificadores quentes (vida do PreviewWidget; usado pelos dois
    // workers via acquire/release por caminho de arquivo).
    DecoderPool* m_decPool = nullptr;
    // Thread dedicada ao prefetch: decodifica o próximo clipe sem bloquear o
    // FrameWorker principal (que mantém o pipeline m_ready em cadência).
    QThread* m_bgPrefetchThread = nullptr;
    BgPrefetchWorker* m_bgPrefetchWorker = nullptr;
    // true enquanto um prefetch em background está em andamento
    bool m_bgPrefetchBusy = false;
    // Aquecimento de clipes à frente (Fase 2): fila de caminhos para abrir de
    // antemão na thread de prefetch (além do frame0 do próximo clipe).
    struct WarmReq {
        QString path;
        double t = 0.0;
        int maxW = 0;
    };
    QVector<WarmReq> m_warmQueue;       // alvos; um por vez despachado
    QString m_warmInFlight;             // caminho com warm() em andamento na thread bg
    // Pedido de decodificação. clipId diz para qual clipe o quadro se destina:
    // o clipe do topo alimenta m_frame; os demais alimentam m_layerCache.
    struct FrameReq {
        QString clipId;
        QString path;
        double t = 0.0;
        double dt = 1.0 / 30.0;
        int maxW = 0;
        FrameFx fx;     // parâmetros de efeitos p/ aplicar na thread do worker
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
    // Requisição atualmente em decodificação na thread do worker (para a UI
    // decidir cancelá-la quando um alvo novo a supera; Fase 3).
    FrameReq m_inflightReq;
    bool m_hasInflightReq = false;
    QString m_shownPath;
    // true quando o quadro exibido do TOPO veio pronto da thread do worker
    // (já cortado/efeituado): o caminho de "re-crop sem re-decode" não pode
    // re-aplicar applyCrop() por cima — precisa pedir um novo decode.
    bool m_shownFx = false;
    // Diagnóstico de performance (overlay com PIERROT_PERF_DEBUG=1).
    QElapsedTimer m_perfT;
    qint64 m_perfWorkerStartNs = 0;
    qint64 m_perfPrefetchStartNs = 0;
    QString m_profTopClipId;   // clipe do topo no tick anterior (detecta corte)
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
    Frei0rPluginManager* m_frei0rManager = nullptr;
    QVector<OfxPluginInstance> m_clipOfxFx; // efeitos OFX do clipe ativo
    QVector<Frei0rEffect> m_clipFrei0rFx;   // efeitos frei0r do clipe ativo

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
