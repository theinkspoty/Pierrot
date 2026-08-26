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
#include "models/Project.h"
#include "ffmpeg/FFmpegDecoder.h"
#include "render/MesaRenderer.h"

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

class OfxPluginManager;

class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;
    void setProject(Project* p);
    void setOfxManager(OfxPluginManager* m) { m_ofxManager = m; }
    void refreshView();
    AudioMixer* audioMixer() const { return m_audioFeed; }
    double playhead() const { return m_playhead; }

    // Níveis de áudio para o MixerWidget (thread-safe).
    struct AudioLevels {
        QHash<QPair<bool,int>, float> rms; // (isAudio, trackIndex) → RMS 0..1
        float masterRms = 0.0;
    };
    AudioLevels audioLevels() const;
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
    void applySeek(double t);        // caminho comum do seek (não mexe nos relógios)
    double audioClockSec() const;    // segundos consumidos pelo sink de áudio (-1 = sem áudio)
    void anchorAudioClock(double t); // mapeia o relógio do áudio para o tempo do projeto
    void drawEmptyMonitor(QPainter& p, const QRect& canvas);
    void drawClipText(QPainter& p, const QRect& canvas, const Clip* clip, double k);
    void updateFrame();
    void applyCrop();
    void applyBasicEffects(QImage& img);
    static void applyBasicEffectsOn(QImage& img, const Clip& c);
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
    // Se o clipe ativo pertence a um grupo Mesa, renderiza a composição
    // (com transform de câmera) e devolve true preenchendo m_frameFull/m_frame.
    bool tryRenderMesa(const Clip* clip);
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
    // Sincronismo A/V: o playhead segue o relógio do sink de áudio (o que se
    // ouve), evitando o desvio acumulado do relógio de parede em reproduções
    // longas. m_audioAnchor converte processedUSecs() -> tempo do projeto.
    double m_audioAnchor = 0.0;
    bool m_audioClockOn = false;
    // Última leitura bruta do relógio do sink. Serve para detectar underrun:
    // quando processedUSecs() congela (decoder de áudio reabrindo/seek lento em
    // arquivos grandes), a correção A/V NÃO pode puxar o vídeo para trás — é
    // exatamente isso que fazia a agulha "voltar ao início" ao clicar na
    // timeline durante a reprodução.
    double m_audioLastRaw = -1.0;
    // m_clock.elapsed() registrado na última ancoragem do relógio de áudio.
    // Dá um período de graça após seek/loop: nesse intervalo o desvio ainda é
    // resíduo da troca de posição, não drift real — corrigir nele é o que
    // teleportava a agulha para trás em arquivos longos.
    qint64 m_lastAnchorClockMs = -1;
    // Geração do sink de áudio: invalida a criação async pendente quando o
    // stopAudio() roda no meio (evita sink órfão tocando após parar).
    std::atomic<int> m_audioGen{0};
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

    // Áudio do preview (mixer com um decoder por clipe ativo).
    AudioMixer* m_audioFeed = nullptr;
    QAudioSink* m_audioSink = nullptr;
    QAudioOutput* m_audioOut = nullptr;

    // Decodificação de vídeo em thread própria (não trava a UI na reprodução).
    QThread* m_frameThread = nullptr;
    FrameWorker* m_frameWorker = nullptr;
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

    // ── OFX plugin manager ────────────────────────────────────────────
    OfxPluginManager* m_ofxManager = nullptr;
    QVector<OfxPluginInstance> m_clipOfxFx; // efeitos OFX do clipe ativo

    // ── Grade visual do preview (estilo Vegas) ────────────────────────
    bool m_showGrid = false;
    int m_gridDivisions = 3; // NxN linhas de grade
    QToolButton* m_gridBtn = nullptr;
    void drawGrid(QPainter& p, const QRect& canvas);

    // Renderiza o canvas de uma Mesa (sem câmera) quando o clipe ativo
    // pertence a um grupo Mesa. Usa um MesaRenderer dedicado.
    MesaRenderer m_mesaRenderer;
};
