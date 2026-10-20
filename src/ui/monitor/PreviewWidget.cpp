// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/monitor/PreviewWidget.h"
#include <cstdint>

#include "ui/dialogs/SettingsDialog.h"
#include "ui/timeline/TlLog.h"
#include "colombina/CrashReporter.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/ffmpeg/AudioConformCache.h"
#include "colombina/audio/EqBiquad.h"
#include "colombina/ofx/OfxRenderer.h"
#include "colombina/ofx/OfxPluginManager.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "colombina/fx/ColorGrade.h"
#include "ui/monitor/ScopeWidget.h"
#include "colombina/export/LainkaFx.h"
#include "ui/core/Theme.h"
#include "ui/monitor/MonitorUtils.h"
#include "colombina/generators.h"
#include "colombina/render/TextRender.h"
// DSP dos efeitos de áudio (AudioFx/GraphicEq/ParametricEqDsp) e os helpers
// de diagnóstico/config do preview (playDbg/audioDbg/composeDbg/
// previewAudioFormat/desengasgaIntervalMs) — vidrados em
// headers próprios, compartilhados com FrameWorker/FramePipeline/AudioMixer.
#include "ui/audio/AudioFxDsp.h"
#include "ui/audio/AudioMixer.h"
#include "ui/monitor/PreviewDebug.h"
#include "colombina/ffmpeg/SourcePool.h"
#include "colombina/ffmpeg/FramePipeline.h"
#include "ui/monitor/FrameWorker.h"
#include "ui/monitor/MonitorScrubBar.h"

#include <QPainter>
#include <QPixmap>
#include <QPolygon>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QLabel>
#include <QComboBox>
#include <QSignalBlocker>
#include <QPointer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QShortcut>
#include <QIODevice>
#include <QAudioFormat>
#include <QPainterPath>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSettings>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif
#include <algorithm>
#include <atomic>
#include <QDebug>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <utility>
#include <QHash>
#include <QMutex>


namespace {

double projFps(const Project* p) {
    return (p && p->fps > 0.0) ? p->fps : 30.0;
}

// Mapeia o modo de composição da faixa (os mesmos 12 modos da exportação)
// para o composition mode do QPainter. "subtract" usa SourceOut como
// aproximação (Dst × (1 - Src)), que é o mais próximo do subtract no Qt.
QPainter::CompositionMode blendModeToQt(const QString& mode) {
    if (mode == QStringLiteral("screen"))    return QPainter::CompositionMode_Screen;
    if (mode == QStringLiteral("multiply"))  return QPainter::CompositionMode_Multiply;
    if (mode == QStringLiteral("overlay"))   return QPainter::CompositionMode_Overlay;
    if (mode == QStringLiteral("darken"))    return QPainter::CompositionMode_Darken;
    if (mode == QStringLiteral("lighten"))   return QPainter::CompositionMode_Lighten;
    if (mode == QStringLiteral("softlight")) return QPainter::CompositionMode_SoftLight;
    if (mode == QStringLiteral("hardlight")) return QPainter::CompositionMode_HardLight;
    if (mode == QStringLiteral("difference"))return QPainter::CompositionMode_Difference;
    if (mode == QStringLiteral("addition"))  return QPainter::CompositionMode_Plus;
    if (mode == QStringLiteral("subtract")) return QPainter::CompositionMode_SourceOut;
    if (mode == QStringLiteral("exclusion")) return QPainter::CompositionMode_Exclusion;
    return QPainter::CompositionMode_SourceOver;
}
}

struct PreviewQOpt { int width; const char* label; const char* code; };
static const PreviewQOpt kPreviewQualities[] = {
    {360,  "360p",  "360p"},
    {480,  "480p",  "480p"},
    {720,  "720p",  "720p"},
    {1080, "1080p", "1080p"},
    {3840, "4K",    "4K"},
};

void PreviewWidget::refreshTimeLabelStyle() {
    if (!m_timeLabel) return;
    const auto& c = themeColors();
    m_timeLabel->setStyleSheet(
        QStringLiteral(
            "QLabel {"
            "  color: %1;"
            "  background-color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 2px;"
            "  padding: 2px 12px;"
            "  letter-spacing: 1px;"
            "}").arg(c.accent.name(), c.monitorBg.name(), c.canvasBorder.name()));
}

// ── Faixa de scrub do Program Monitor ────────────────────────────────
// Índices do combo de zoom do monitor (0 = Ajustar à área). A faixa em si
// (playhead/loop/marcadores) vive em ui/monitor/MonitorScrubBar.*.
const double kMonitorZooms[] = {0.0, 0.25, 0.50, 0.75, 1.0, 1.5, 2.0};

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    qRegisterMetaType<FrameFx>("FrameFx"); // snapshot de efeitos p/ o worker
    // O QElapsedTimer da instrumentação precisa estar rodando antes do primeiro
    // tick — sem start() ele fica inválido para sempre e todas as leituras de
    // latência (worker/prefetch) davam 0, mascarando o gargalo de decode.
    m_perfT.start();
    const QColor glyph = themeColors().monitorLabel;
    QColor hoverBg = themeColors().canvasBorder;
    hoverBg.setAlpha(70);
    QColor checkBg = themeColors().accent;
    checkBg.setAlpha(55);
    m_playIcon = makeMonitorIcon(MonitorGlyph::Play, glyph);
    m_pauseIcon = makeMonitorIcon(MonitorGlyph::Pause, glyph);

    // Transporte do monitor, espelhado no Program Monitor do Premiere: botões
    // planos no topo — quadro anterior, play/pausa, quadro seguinte e loop.
    m_stepBackBtn = new QToolButton(this);
    m_stepBackBtn->setIcon(makeMonitorIcon(MonitorGlyph::StepBack, glyph));
    m_stepBackBtn->setIconSize(QSize(18, 18));
    m_stepBackBtn->setFixedSize(30, 26);
    m_stepBackBtn->setToolTip(tr("Quadro anterior"));
    m_stepBackBtn->setCursor(Qt::PointingHandCursor);
    connect(m_stepBackBtn, &QToolButton::clicked, this, [this]() { stepFrame(-1); });

    // O PlaybackEngine troca o rótulo do botão em cada transição; aqui ele
    // fica só com o ícone (o texto é limpo no onStateChanged).
    m_playBtn = new QPushButton(this);
    m_playBtn->setIcon(m_playIcon);
    m_playBtn->setIconSize(QSize(20, 20));
    m_playBtn->setFixedSize(36, 26);
    m_playBtn->setToolTip(tr("Reproduzir/pausar (Espaço)"));
    m_playBtn->setCursor(Qt::PointingHandCursor);

    m_stepFwdBtn = new QToolButton(this);
    m_stepFwdBtn->setIcon(makeMonitorIcon(MonitorGlyph::StepFwd, glyph));
    m_stepFwdBtn->setIconSize(QSize(18, 18));
    m_stepFwdBtn->setFixedSize(30, 26);
    m_stepFwdBtn->setToolTip(tr("Quadro seguinte"));
    m_stepFwdBtn->setCursor(Qt::PointingHandCursor);
    connect(m_stepFwdBtn, &QToolButton::clicked, this, [this]() { stepFrame(1); });

    m_loopBtn = new QToolButton(this);
    m_loopBtn->setIcon(makeMonitorIcon(MonitorGlyph::Loop, glyph));
    m_loopBtn->setIconSize(QSize(18, 18));
    m_loopBtn->setFixedSize(30, 26);
    m_loopBtn->setCheckable(true);
    m_loopBtn->setToolTip(tr("Loop (repete o trecho in/out)"));
    m_loopBtn->setCursor(Qt::PointingHandCursor);
    connect(m_loopBtn, &QToolButton::toggled, this, &PreviewWidget::setLoopEnabled);

    // Contador de tempo estilo Premiere (timecode do Program Monitor): fonte
    // mono, azul sobre fundo quase preto, cantos quadrados — a assinatura
    // visual do monitor do Premiere. As cores vêm dos tokens para respeitar o
    // tema claro/escuro, como o resto do preview. Fica flutuando sobre o canto
    // inferior-esquerdo da área de vídeo (posicionado no resizeEvent).
    m_timeLabel = new QLabel(tr("00:00:00:00"), this);
    m_timeLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    QFont tcFont = m_timeLabel->font();
    tcFont.setFamily(QStringLiteral("Consolas, Menlo, DejaVu Sans Mono, monospace"));
    tcFont.setPointSize(11);
    tcFont.setBold(true);
    m_timeLabel->setFont(tcFont);
    refreshTimeLabelStyle();
    m_timeLabel->setMinimumWidth(230); // "00:00:05:12 / 00:01:23:00"

    // Rótulo da sequência no topo, como o "Program: <nome>" do Premiere.
    m_programLabel = new QLabel(tr("Program: "), this);
    QFont pf = m_programLabel->font();
    pf.setPointSize(9);
    pf.setBold(true);
    m_programLabel->setFont(pf);
    m_programLabel->setStyleSheet(
        QStringLiteral("QLabel{color:%1;}").arg(themeColors().monitorLabel.name()));

    // Fullscreen do monitor (o Premiere expande o Program para tela cheia;
    // Esc sai).
    m_fullscreenBtn = new QToolButton(this);
    m_fullscreenBtn->setIcon(makeMonitorIcon(MonitorGlyph::Fullscreen, glyph));
    m_fullscreenBtn->setIconSize(QSize(18, 18));
    m_fullscreenBtn->setFixedSize(30, 26);
    m_fullscreenBtn->setToolTip(tr("Tela cheia (Esc para sair)"));
    m_fullscreenBtn->setCursor(Qt::PointingHandCursor);
    connect(m_fullscreenBtn, &QToolButton::clicked, this, [this]() {
        QWidget* w = window();
        if (w->isFullScreen()) w->showNormal();
        else w->showFullScreen();
    });
    auto* fsEsc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(fsEsc, &QShortcut::activated, this, [this]() {
        if (window()->isFullScreen()) window()->showNormal();
    });

    // ── Aba do topo: SÓ o rótulo "Program: <nome>", como no Premiere. O
    // transporte do Program Monitor fica na barra de BAIXO (é onde o Premiere
    // põe), não aqui em cima.
    m_topBar = new QWidget(this);
    m_topBar->setObjectName(QStringLiteral("pmTopBar"));
    m_topBar->setFixedHeight(24);
    auto* topLay = new QHBoxLayout(m_topBar);
    topLay->setContentsMargins(8, 0, 8, 0);
    topLay->setSpacing(0);
    topLay->addWidget(m_programLabel);
    topLay->addStretch(1);
    // Seletor por object name: um "QWidget{...}" cascatearia para os filhos e
    // contaminaria combo/rótulos com a borda da barra.
    m_topBar->setStyleSheet(QStringLiteral(
        "QWidget#pmTopBar{background:%1;border-bottom:1px solid %2;}")
        .arg(themeColors().base.name(), themeColors().trackBorder.name()));

    // Resolução do preview (como o menu de resolução do Program Monitor):
    // botão com menu no cluster direito, entre as margens e o zoom.
    m_qualityBtn = new QToolButton(this);
    m_qualityBtn->setPopupMode(QToolButton::InstantPopup);
    m_qualityBtn->setToolTip(tr("Qualidade do preview (resolução de decodificação)"));
    m_qualityMenu = new QMenu(this);
    for (const PreviewQOpt& o : kPreviewQualities) {
        QAction* a = m_qualityMenu->addAction(QString::fromUtf8(o.label));
        a->setCheckable(true);
        a->setData(o.width);
        if (o.width == m_previewQuality) a->setChecked(true);
        connect(a, &QAction::triggered, this, [this, o]() { setPreviewQuality(o.width); });
    }
    m_qualityBtn->setMenu(m_qualityMenu);
    // Mostra a qualidade atual no botão.
    for (const PreviewQOpt& o : kPreviewQualities)
        if (o.width == m_previewQuality) {
            m_qualityBtn->setText(QString::fromUtf8(o.code));
            break;
        }

    // ── Barra de rodapé: timecode à esquerda, transporte no centro-esquerda e
    // o cluster (margens → qualidade → zoom → tela cheia) à direita — a mesma
    // ordem da barra inferior do Program Monitor do Premiere.
    m_bottomBar = new QWidget(this);
    m_bottomBar->setObjectName(QStringLiteral("pmBottomBar"));
    m_bottomBar->setFixedHeight(30);
    auto* bar = new QHBoxLayout(m_bottomBar);
    bar->setContentsMargins(6, 2, 6, 2);
    bar->setSpacing(4);
    m_bottomBar->setStyleSheet(QStringLiteral(
        "QWidget#pmBottomBar{background:%1;border-top:1px solid %2;}"
        "QToolButton,QPushButton{border:none;border-radius:4px;background:transparent;}"
        "QToolButton:hover,QPushButton:hover{background:%3;}"
        "QToolButton:checked,QPushButton:checked{background:%4;}")
        .arg(themeColors().base.name(), themeColors().trackBorder.name(),
             hoverBg.name(QColor::HexArgb), checkBg.name(QColor::HexArgb)));
    // Atualiza o mini-scope a ~15 fps quando o monitor está visível.
    // Durante a reprodução pula: copiar+escalar o frame inteiro na GUI a cada
    // 70ms competia com o paint e gerava micro-stutter ("frameback" leve).
    auto* miniTimer = new QTimer(this);
    miniTimer->setInterval(70);
    connect(miniTimer, &QTimer::timeout, this, [this]() {
        if (!m_miniScope || !isVisible()) return;
        if (m_playing) return; // não rouba tempo da GUI durante o play
        m_miniScope->refreshFrom(scopesFrame());
    });
    miniTimer->start();
    // Transporte do Program Monitor: à esquerda, como no Premiere.
    bar->addWidget(m_stepBackBtn);
    bar->addWidget(m_playBtn);
    bar->addWidget(m_stepFwdBtn);
    bar->addWidget(m_loopBtn);
    bar->addSpacing(12);
    // Timecode: entre o transporte e o cluster da direita.
    bar->addWidget(m_timeLabel);
    // Mini-scopes no Program Monitor (Lumetri Premiere): waveform compacto.
    m_miniScope = new ScopeWidget(this);
    m_miniScope->setMode(ScopeWidget::Waveform);
    m_miniScope->setFixedSize(96, 24);
    m_miniScope->setToolTip(tr("Waveform (mini) — dock Analisadores para o full"));
    bar->addWidget(m_miniScope);
    bar->addStretch(1);

    // Margens de segurança do Premiere (Action 90% + Title 80%, Ctrl+G
    // alterna). No Premiere não existe grade NxN no Program Monitor — o que
    // aparece são os dois retângulos concêntricos de segurança.
    m_gridBtn = new QToolButton(this);
    m_gridBtn->setIcon(makeMonitorIcon(MonitorGlyph::Safe, themeColors().monitorLabel));
    m_gridBtn->setIconSize(QSize(18, 18));
    m_gridBtn->setFixedSize(30, 26);
    m_gridBtn->setCheckable(true);
    m_gridBtn->setChecked(false);
    m_gridBtn->setToolTip(tr("Margens de segurança (Ctrl+G)"));
    m_gridBtn->setCursor(Qt::PointingHandCursor);
    connect(m_gridBtn, &QToolButton::clicked, this, [this](bool checked) {
        m_showGrid = checked;
        QSettings().setValue(QStringLiteral("monitor/safeAreas"), checked);
        update();
    });
    bar->addWidget(m_gridBtn);
    // Cluster direito do rodapé: margens → qualidade → zoom → tela cheia.
    bar->addWidget(m_qualityBtn);

    // Atalho Ctrl+G para alternar a grade.
    auto* gridShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this);
    connect(gridShortcut, &QShortcut::activated, this, [this]() {
        m_showGrid = !m_showGrid;
        m_gridBtn->setChecked(m_showGrid);
        QSettings().setValue(QStringLiteral("monitor/safeAreas"), m_showGrid);
        update();
    });

    m_zoomCombo = new QComboBox(this);
    m_zoomCombo->addItem(tr("Ajustar"));
    for (int p : {25, 50, 75, 100, 150, 200})
        m_zoomCombo->addItem(tr("%1%").arg(p));
    m_zoomCombo->setCurrentIndex(0);
    m_zoomCombo->setToolTip(tr("Zoom do preview"));
    connect(m_zoomCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        setZoom(kMonitorZooms[qBound(0, idx, 6)]); // setZoom grava o QSettings
    });
    bar->addWidget(m_zoomCombo);
    bar->addSpacing(4);
    bar->addWidget(m_fullscreenBtn);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_topBar);
    lay->addStretch(1);
    // Faixa de scrub: playhead arrastável + loop + picotes dos marcadores,
    // logo acima da barra de transporte (estilo DaVinci/Premiere).
    m_scrubBar = new MonitorScrubBar(this);
    lay->addWidget(m_scrubBar);
    lay->addWidget(m_bottomBar);

    // Preferências do monitor (zoom/qualidade/margens) persistem entre
    // sessões — antes eram esquecidas ao fechar o app.
    {
        QSettings s;
        setZoom(kMonitorZooms[qBound(0, s.value(QStringLiteral("monitor/zoom"), 0).toInt(), 6)]);
        const int q = s.value(QStringLiteral("monitor/quality"), 720).toInt();
        for (const PreviewQOpt& o : kPreviewQualities)
            if (o.width == q) { setPreviewQuality(q); break; }
        m_showGrid = s.value(QStringLiteral("monitor/safeAreas"), false).toBool();
        m_gridBtn->setChecked(m_showGrid);
    }

    // O PlaybackEngine controla o transporte; o timer é deste widget (tem
    // QObject/parent) e repassa o timeout para o motor via tick().
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    // Timer rápido (8ms): o avanço de frame NÃO é decidido pelo jitter do event
    // loop (com 33ms fixo a cadência variava ±5ms → micro-engasgo contínuo).
    // O frame-guard do tick() retorna cedo quando o relógio ainda não cruzou o
    // frame boundary; com 8ms a resolução temporal é ~4x melhor e o playback
    // avança pelo relógio monotônico, não pela chegada do timeout.
    m_timer->setInterval(8);
    setTimer(m_timer);
    setPlayButton(m_playBtn);
    connect(m_timer, &QTimer::timeout, this, [this]() { PlaybackEngine::tick(); });
    connect(m_playBtn, &QPushButton::clicked, this, &PreviewWidget::togglePlay);

    // Pool de decodificadores quentes: os workers usam acesso compartilhado
    // por caminho de arquivo (o prefetch só aquece; o main pega no corte).
    m_decPool = &SourcePool::instance();

    // Thread de vídeo: decodificar quadros aqui tira a decodificação (que é
    // cara em arquivos grandes/4K/MKV) do caminho da UI.
    m_frameThread = new CrashReporter::TrackedThread("preview-frame", this);
    m_frameWorker = new FrameWorker(m_decPool);
    m_frameWorker->setEffectManagers(m_ofxManager, m_frei0rManager);
    m_frameWorker->moveToThread(m_frameThread);
    connect(m_frameThread, &QThread::finished, m_frameWorker, &QObject::deleteLater);
    connect(m_frameWorker, &FrameWorker::frameReady,
            this, &PreviewWidget::onFrameReady, Qt::QueuedConnection);
    connect(m_frameWorker, &FrameWorker::prefetchReady,
            this, &PreviewWidget::onPrefetchReady, Qt::QueuedConnection);
    m_frameThread->start();

    // Worker DEDICADO das camadas inferiores (H3): mesmo formato do principal,
    // thread própria — as camadas decodificam em paralelo com o topo em vez
    // de serializarem no mesmo worker (multi-track não "rouba slot" mais).
    m_layerThread = new CrashReporter::TrackedThread("preview-layers", this);
    m_layerWorker = new FrameWorker(m_decPool);
    m_layerWorker->setEffectManagers(m_ofxManager, m_frei0rManager);
    m_layerWorker->moveToThread(m_layerThread);
    connect(m_layerThread, &QThread::finished, m_layerWorker, &QObject::deleteLater);
    connect(m_layerWorker, &FrameWorker::frameReady, this,
            [this](const QString& clipId, const QString& path, double t, int maxW,
                   const QImage& img, bool processed) {
                handleFrameReady(clipId, path, t, maxW, img, processed, /*fromLayer=*/true);
            },
            Qt::QueuedConnection);
    m_layerThread->start();

    // Pipeline de prefetch (L4): dono da thread de background, da fila de
    // aquecimento à frente e do estado busy. A UI só resolve caminhos e
    // alimenta o pipeline — a decisão de lookahead não mora mais aqui.
    m_pipeline = new FramePipeline(m_decPool, this);
    connect(m_pipeline, &FramePipeline::framePrefetched,
            this, &PreviewWidget::onBgPrefetchDone, Qt::QueuedConnection);
    connect(m_pipeline, &FramePipeline::prefetchFailed,
            this, &PreviewWidget::onBgPrefetchFailed, Qt::QueuedConnection);
    connect(m_pipeline, &FramePipeline::warmed,
            this, &PreviewWidget::onBgWarmDone, Qt::QueuedConnection);
    m_pipeline->start();

    // Debounce do aquecimento do playhead parado (F1): ao parar a agulha fora
    // da reprodução, abre+posiciona o decoder e conforma o áudio do clipe sob
    // ela. 180ms evita disparar a cada passo de um scrub contínuo.
    m_idleWarmTimer = new QTimer(this);
    m_idleWarmTimer->setSingleShot(true);
    m_idleWarmTimer->setInterval(180);
    connect(m_idleWarmTimer, &QTimer::timeout, this, &PreviewWidget::warmPlayhead);

    // Pré-aquece o backend de áudio do Qt (1ª criação do QAudioSink inicializa
    // o plugin ffmpeg do Qt multimedia, ~1s) assim que o event loop começa —
    // fora do caminho do play, que era onde esse custo aparecia como freeze.
    QTimer::singleShot(0, this, [this]() { prewarmAudioSink(); });

    setMinimumSize(320, 200);
}

PreviewWidget::~PreviewWidget() {
    stopAudio();
    if (m_frameThread) {
        // Esvazia a fila de decodificação antes de parar: com arquivos grandes
        // o worker podia estar a vários quadros de distância, o wait(2000)
        // estourava e o objeto morria com decode em andamento (crash no fecho).
        {
            QMutexLocker l(&m_frameMutex);
            m_reqQueue.clear();
        }
        m_frameThread->quit();
        m_frameThread->wait(5000);
    }
    if (m_layerThread) {
        // Mesma razão do worker principal: fila esvaziada antes de parar.
        {
            QMutexLocker l(&m_frameMutex);
            m_reqQueue.clear();
        }
        m_layerThread->quit();
        m_layerThread->wait(5000);
    }
    // Pipeline parado: encerra a thread de prefetch e espera (nenhum warm/decode
    // ativo). O pool é o singleton global (colombina/ffmpeg/SourcePool) — não é
    // daqui; ele fecha os decoders restantes no fim do processo.
    if (m_pipeline) m_pipeline->stop();
    m_decPool = nullptr;
}

void PreviewWidget::setProject(Project* p) {
    PlaybackEngine::setProject(p); // seta m_project e para o transporte
    updateProgramLabel();
    m_playhead = 0.0;
    m_mesaRenderer.clearCompositeCache(); // composto do projeto anterior não vale
    m_frame = QImage();
    m_frameFull = QImage();
    m_compositedCache = QImage();  // invalida cache do frame composto
    m_lastSrcT = -1.0;
    m_lastDecodeW = -1;
    m_lastFile.clear();
    m_lastCropL = m_lastCropR = m_lastCropT = m_lastCropB = -1;
    m_underFrame = QImage();
    m_underPath.clear();
    m_underT = -1.0;
    m_underW = -1;
    m_underRequested = false;
    m_underPending = false;
    m_transAlpha = -1.0;
    m_transType.clear();
    {
        QMutexLocker l(&m_frameMutex);
        m_reqQueue.clear();
        m_layerCache.clear();
        m_prefetch = PrefetchFrame();
        m_shownPath.clear();
        m_shownT = -1.0;
        m_shownW = -1;
    }
    updateFrame();
    update();
    // Projeto recém-aberto com a agulha em repouso: aquece o clipe sob ela.
    scheduleIdleWarm();
}

void PreviewWidget::updateProgramLabel() {
    if (!m_programLabel) return;
    const QString nome =
        (m_project && !m_project->name.isEmpty()) ? m_project->name : tr("Sem projeto");
    m_programLabel->setText(tr("Program: %1").arg(nome));
}

// "HH:MM:SS:FF / HH:MM:SS:FF": posição atual / duração da sequência, como no
// Premiere — antes só a posição, sem referência do tamanho total.
void PreviewWidget::updateTimecodeLabel() {
    if (!m_timeLabel) return;
    const double fps = projFps(m_project);
    const double dur = m_project ? std::max(0.0, m_project->duration()) : 0.0;
    m_timeLabel->setText(fmtTimecode(m_playhead, fps) + QStringLiteral(" / ") +
                         fmtTimecode(dur, fps));
}

void PreviewWidget::refreshView() {
    // Todo caminho de EDIÇÃO passa por aqui: TimelineWidget::modified,
    // MediaPoolWidget::mediaChanged (mídia trocada), GraphEditorWidget::modified
    // (keyframes de crop/LAINKA), PantaloneWidget::modified, MaskEditorDialog e
    // ExpressWidget. O tick de playback NÃO passa por aqui (ele vai por
    // applySeekVisual), então este é o ponto exato para invalidar o memo de
    // applyCrop(): a chave carrega tempo e crop, mas não os parâmetros de LAINKA,
    // MotiOn, efeitos básicos, máscaras ou a identidade do arquivo — mudar
    // qualquer um deles com o playhead parado manteria a chave e devolveria o
    // quadro antigo.
    m_cropMemo = QImage();
    // O cache do frame composto é chaveado só por índice de frame + nº de
    // camadas; edições de conteúdo (efeitos de clipe, da faixa de efeitos,
    // blend, opacidade) não mudam essas chaves e deixariam o composto velho
    // com o playhead parado. Invalida aqui, junto com o memo de crop.
    m_compositedCache = QImage();
    // O cache de camadas guarda o quadro já COM os efeitos aplicados (inclusive
    // a máscara); editar o clipe (ex.: criar máscara no Pantalone) deixa ele
    // velho. Limpa para o efeito aparecer no preview.
    m_layerCache.clear();
    // O nome do projeto pode ter mudado desde o setProject (createProject só
    // define m_project.name depois dele); o rótulo acompanha aqui.
    updateProgramLabel();
    updateFrame();
    if (m_scrubBar) m_scrubBar->update(); // duração/marcadores podem ter mudado
    update();
}

QImage PreviewWidget::scopesFrame() const {
    QImage src;
    {
        QMutexLocker l(&m_frameMutex);
        src = m_compositedCache.isNull() ? m_frame : m_compositedCache;
    }
    if (src.isNull() || src.width() < 2 || src.height() < 2) return QImage();
    // FastTransformation: o mini-scope é 96×24 px — suavização é desperdício.
    return src.scaled(160, 90, Qt::IgnoreAspectRatio, Qt::FastTransformation)
               .convertToFormat(QImage::Format_ARGB32);
}

QImage PreviewWidget::compositeFrame() const {
    QMutexLocker l(&m_frameMutex);
    return m_compositedCache.isNull() ? m_frame : m_compositedCache;
}

PreviewWidget::AudioLevels PreviewWidget::audioLevels() const {
    if (!m_audioFeed) return {};
    const AudioMixer::TrackLevels tl = m_audioFeed->currentLevels();
    AudioLevels out;
    out.rms = tl.rms;
    out.masterRms = tl.masterRms;
    return out;
}

void PreviewWidget::resizeEvent(QResizeEvent*) {
    // O vídeo ocupa o miolo entre a aba do topo e a barra de rodapé (onde o
    // Premiere põe o transporte).
    const int top = m_topBar ? m_topBar->height() : 24;
    const int bottom = m_bottomBar ? m_bottomBar->height() : 0;
    m_videoRect = QRect(0, top, width(), std::max(0, height() - top - bottom));
}

// ── LAINKA: usa funções compartilhadas de LainkaFx.h ────────────────
using namespace LainkaFx;

void PreviewWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    PreviewProfiler::Scope paintScope(PreviewProfiler::active() ? &profFrame().paintNs : nullptr);
    renderFrame(p);
    // Os indicadores vão SEMPRE por cima, em qualquer caminho de render. Antes
    // eles ficavam no fim de renderFrame(), que tem vários `return` antecipado
    // (projeto vazio, cache de composição, caminho de clip único) — ou seja,
    // em projetos com 2+ camadas o overlay de perf nunca aparecia, que é
    // justamente o cenário que se quer diagnosticar. A margem de segurança
    // tinha o mesmo problema (sumia no caminho do composto).
    drawGrid(p, m_lastCanvas);
    drawQualityChip(p);
    drawPlaybackBadges(p);
}

// Chip "720p"/"4K" no canto superior direito do vídeo: lembra que a imagem
// pode não estar na resolução cheia (estilo Premiere). Sutil: fundo preto
// translúcido, texto claro 8pt.
void PreviewWidget::drawQualityChip(QPainter& p) {
    if (m_videoRect.isEmpty()) return;
    QString code;
    for (const PreviewQOpt& o : kPreviewQualities)
        if (o.width == m_previewQuality) {
            code = QString::fromUtf8(o.code);
            break;
        }
    if (code.isEmpty()) return;
    p.save();
    p.setClipRect(m_videoRect);
    QFont f = p.font();
    f.setPointSizeF(8);
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    const QRect chip(m_videoRect.right() - fm.horizontalAdvance(code) - 18,
                     m_videoRect.top() + 10, fm.horizontalAdvance(code) + 12, 16);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 150));
    p.drawRoundedRect(chip, 3, 3);
    p.setPen(QColor(255, 255, 255, 220));
    p.drawText(chip, Qt::AlignCenter, code);
    p.restore();
}

void PreviewWidget::drawPlaybackBadges(QPainter& p) {
    // Indicador de frames perdidos (visível quando há drops, sem env var).
    if (m_playing && m_perf.droppedTotal > 0) {
        p.save();
        p.resetTransform();
        QFont df = p.font();
        df.setPointSizeF(8);
        df.setBold(true);
        p.setFont(df);
        const QString dropTxt = QStringLiteral("Dropped: %1").arg(m_perf.droppedTotal);
        const QRect dropR(width() - 130, 8, 122, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(180, 50, 50, 180));
        QPainterPath dropBg;
        dropBg.addRoundedRect(QRectF(dropR), 3.0, 3.0);
        p.drawPath(dropBg);
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(dropR, Qt::AlignCenter, dropTxt);
        p.restore();
    }

    // Indicador de qualidade adaptativa (mostra quando auto-baixou).
    if (m_adaptiveActive) {
        p.save();
        p.resetTransform();
        QFont af = p.font();
        af.setPointSizeF(8);
        af.setBold(true);
        p.setFont(af);
        const QString adpTxt = QStringLiteral("Auto: %1p").arg(m_previewQuality);
        const QRect adpR(width() - 130, m_playing && m_perf.droppedTotal > 0 ? 30 : 8, 122, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(50, 120, 180, 180));
        QPainterPath adpBg;
        adpBg.addRoundedRect(QRectF(adpR), 3.0, 3.0);
        p.drawPath(adpBg);
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(adpR, Qt::AlignCenter, adpTxt);
        p.restore();
    }

    // Diagnóstico de performance (PIERROT_PERF_DEBUG=1).
    drawPerfOverlay(p);
}

void PreviewWidget::renderFrame(QPainter& p) {

    // Fundo do console (área ao redor do monitor).
    p.fillRect(m_videoRect, themeColors().monitorBg);

    if (!m_project || m_project->width <= 0 || m_project->height <= 0) {
        m_lastCanvas = QRect(); // sem canvas válido: sem margens de segurança
        drawEmptyMonitor(p, m_videoRect);
        return;
    }

    const double pw = m_project->width;
    const double ph = m_project->height;

    // Área útil do monitor.
    const QRect work = m_videoRect.adjusted(12, 12, -12, -12);

    // Escala: zoom fixo ou "Ajustar" (o quadro inteiro cabe na área).
    double k = m_zoom > 0.0 ? m_zoom
                            : qMin(work.width() / pw, work.height() / ph);
    QRect canvas(QPoint(0, 0), QSize(qMax(1, (int)(pw * k)), qMax(1, (int)(ph * k))));
    canvas.moveCenter(work.center());
    canvas = canvas.intersected(work); // centraliza e recorta quando zoom > 1
    m_lastCanvas = canvas; // margens de segurança desenhadas no paintEvent

    // Monitor: o quadro assenta direto no fundo do painel, com uma sombra
    // suave atrás (3 anéis pretos translúcidos — custo ~zero, sem blur) para
    // separar o "programa" do fundo do console.
    for (int i = 3; i >= 1; --i) {
        QColor sh(0, 0, 0, 12 / i);
        p.setPen(Qt::NoPen);
        p.setBrush(sh);
        p.drawRect(canvas.adjusted(-i, -i, i, i));
    }
    p.fillRect(canvas, themeColors().canvasBg);

    // Texto independente ativo no playhead (desenhado mesmo sem quadro de vídeo).
    bool anyText = false;
    if (m_project) {
        for (const Track& tr : m_project->videoTracks) {
            if (!tr.visible) continue;   // faixa oculta (olho) não mostra texto
            for (const Clip& c : tr.clips)
                if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur) {
                    anyText = true;
                    break;
                }
        }
    }

    // Render unificado em "espaço de projeto": o canvas É o quadro do projeto
    // (k = pixels de tela por pixel do projeto). O vídeo é desenhado do mesmo
    // jeito com ou sem transform — sem transform, ele cabe inteiro no quadro;
    // com transform, pan/rot/zoom giram em torno do centro do quadro. Assim,
    // adicionar um keyframe de transform nunca muda o tamanho do vídeo.
    const Clip* clip = clipAt(m_playhead);

    const double S = clip ? kfValue(clip->kfScale, clip->scale, m_playhead - clip->pos) : 1.0;
    const double SX = clip ? kfValue(clip->kfScaleX, clip->scaleX, m_playhead - clip->pos) : 1.0;
    const double SY = clip ? kfValue(clip->kfScaleY, clip->scaleY, m_playhead - clip->pos) : 1.0;
    const double rot = clip ? kfValue(clip->kfRotation, clip->rotation, m_playhead - clip->pos) : 0.0;
    const double tx = clip ? kfValue(clip->kfTx, clip->tx, m_playhead - clip->pos) : 0.0;
    const double ty = clip ? kfValue(clip->kfTy, clip->ty, m_playhead - clip->pos) : 0.0;

    // Desenha uma camada com transform (escala, rotação, pan), centrada no
    // ponto (cx, cy) e na escala kk (qualquer buffer/painter). `clipRect` evita
    // que o conteúdo vaze do monitor (rotação/zoom) para a área ao redor.
    const auto drawLayer = [&](QPainter& qp, const QImage& img, double s, double r,
                               double x, double y, double alpha,
                               double sX, double sY, double kk, double cx, double cy,
                               const QRect& clipRect,
                               double ox = 0.0, double oy = 0.0) {
        const double fit = qMin(pw / img.width(), ph / img.height());
        qp.save();
        qp.setClipRect(clipRect);
        qp.translate(cx + x * kk + ox * kk, cy + y * kk + oy * kk);
        qp.rotate(r);
        qp.scale(kk * fit * s * sX, kk * fit * s * sY);
        qp.translate(-img.width() / 2.0, -img.height() / 2.0);
        // Opacidade via QPainter: antes multiplicava o alpha num QImage copiado
        // (DestinationIn) — a cópia forçava o COW a desprender o quadro inteiro
        // (8MB 1080p / 33MB 4K) a CADA paint em que alpha < 1. Como o fade da
        // camada de cima deixa alpha < 1 durante toda a transição, isso alocava
        // e copiava o quadro por frame e engasgava o vídeo. setOpacity aplica a
        // mesma mistura sem copiar.
        qp.setOpacity(alpha);
        qp.drawImage(0, 0, img);
        qp.restore();
    };

    // Uma camada de vídeo pronta para desenhar (na ordem de baixo para cima).
    struct Layer {
        QImage frame;
        double s = 1.0, sX = 1.0, sY = 1.0, rot = 0.0, x = 0.0, y = 0.0;
        double alpha = 1.0, ox = 0.0, oy = 0.0;
        QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;
    };
    QVector<Layer> layers;

    // Monta o empilhamento: todas as faixas de vídeo (a faixa 0 é o TOPO), de
    // baixo para cima. Cada faixa contribui com o clipe ativo no playhead.
    // Clipes de texto independentes entram depois (desenhados por cima).
    {
        const Clip* topClip = clip;
        for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
            const Track& t = m_project->videoTracks[tr];
            if (!t.visible) continue;   // faixa oculta (olho) não participa da composição
            const bool isMesaTrack = m_project->findMesaForTrack(t.id) != nullptr;
            const Clip* c = nullptr;
            for (const Clip& cl : t.clips) {
                if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                    const MediaItem* mm = m_project->findMedia(cl.mediaId);
                    if ((isMesaTrack || (mm && mm->hasVideo)) && (!c || cl.pos > c->pos))
                        c = &cl;
                }
            }
            if (!c) continue;
            const QPainter::CompositionMode mode = blendModeToQt(t.blendMode);

            // Quadro deste clipe: vídeo normal
            QImage clipFrame;
            if (topClip && c->id == topClip->id) {
                QMutexLocker l(&m_frameMutex);
                if (!m_frame.isNull()) {
                    clipFrame = m_frame;
                }
            } else if (isMesaTrack) {
                // Track Mesa em layers inferiores: frame já renderizado pelo
                // MesaRenderer e armazenado no layerCache por requestLowerLayers.
                {
                    QMutexLocker l(&m_frameMutex);
                    clipFrame = m_layerCache.value(c->id).img;
                }
                if (clipFrame.isNull()) continue;
            } else {
                const MediaItem* mm = m_project->findMedia(c->mediaId);
                if (!mm || !mm->hasVideo) continue;
                if (mm->isSolid) {
                    // Mídia gerada: gera no tamanho natural, sem passar de uma
                    // vez pela resolução exibida (não borra e não estoura 4K
                    // a cada paint). Preserva o aspecto da mídia.
                    const int mw = mm->width > 0 ? mm->width : (int)pw;
                    const int mh = mm->height > 0 ? mm->height : (int)ph;
                    const double sc = qMin(1.0, qMin((double)canvas.width() / mw,
                                                     (double)canvas.height() / mh));
                    const int w = qMax(1, (int)std::lround(mw * sc));
                    const int h = qMax(1, (int)std::lround(mh * sc));
                    clipFrame = generatorFrame(*mm, w, h);
                    if (c->lainkaEnabled) {
                        const double srcT = clipSrcTime(*c, m_playhead - c->pos);
                        clipFrame = lainkaApplyFx(clipFrame, c->id, srcT,
                                                  c->lainkaSkip, c->lainkaJitterPos,
                                                  c->lainkaJitterRot, c->lainkaJitterScale,
                                                  c->lainkaFlicker, c->lainkaFlickerSpeed,
                                                  c->lainkaWarpAmount, c->lainkaWarpSpeed,
                                                  c->lainkaWarpGrid, c->lainkaOnionSkin,
                                                  c->lainkaDustAmount, c->lainkaScratchAmount,
                                                  c->lainkaMotionBlur, c->lainkaOpacity,
                                                  c->lainkaTargetFps, c->lainkaAntialias,
                                                  QImage());
                    }
                    applyBasicEffectsOn(clipFrame, *c, m_playhead - c->pos);
                } else {
                    {
                        QMutexLocker l(&m_frameMutex);
                        clipFrame = m_layerCache.value(c->id).img;
                    }
                    if (clipFrame.isNull()) continue;
                }
            }

            if (clipFrame.isNull()) continue;

            // Transição (mesma faixa): o clipe de trás por baixo
            if (topClip && c->id == topClip->id && m_transAlpha >= 0.0 && !m_underFrame.isNull()) {
                Layer u;
                u.frame = m_underFrame;
                layers.append(u);
            }

            Layer L;
            L.frame = clipFrame;
            L.s = kfValue(c->kfScale, c->scale, m_playhead - c->pos);
            L.sX = kfValue(c->kfScaleX, c->scaleX, m_playhead - c->pos);
            L.sY = kfValue(c->kfScaleY, c->scaleY, m_playhead - c->pos);
            L.rot = kfValue(c->kfRotation, c->rotation, m_playhead - c->pos);
            L.x = kfValue(c->kfTx, c->tx, m_playhead - c->pos);
            L.y = kfValue(c->kfTy, c->ty, m_playhead - c->pos);
            const double rel = m_playhead - c->pos;
            double alpha = std::clamp(kfValue(c->kfOpacity, c->opacity, rel), 0.0, 1.0);
            if (c->fadeIn > 1e-6) alpha *= std::min(1.0, rel / c->fadeIn);
            if (c->fadeOut > 1e-6) alpha *= std::min(1.0, (c->dur - rel) / c->fadeOut);
            if (topClip && c->id == topClip->id && m_transAlpha >= 0.0) {
                if (m_transType == QStringLiteral("dissolve")) alpha *= m_transAlpha;
                else if (m_transType == QStringLiteral("wipeleft")) L.ox = pw * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wiperight")) L.ox = -pw * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wipeup")) L.oy = ph * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wipedown")) L.oy = -ph * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wipetl")) { L.ox = pw * (1.0 - m_transAlpha); L.oy = ph * (1.0 - m_transAlpha); }
                else if (m_transType == QStringLiteral("wipetr")) { L.ox = -pw * (1.0 - m_transAlpha); L.oy = ph * (1.0 - m_transAlpha); }
                else if (m_transType == QStringLiteral("wipebr")) { L.ox = -pw * (1.0 - m_transAlpha); L.oy = -ph * (1.0 - m_transAlpha); }
                else if (m_transType == QStringLiteral("wipebl")) { L.ox = pw * (1.0 - m_transAlpha); L.oy = -ph * (1.0 - m_transAlpha); }
            }
            L.alpha = alpha;
            L.alpha *= std::clamp(t.opacity, 0.0, 1.0);
            L.mode = mode;
            layers.append(L);
            if (composeDbg())
                qDebug() << "[compose] tr=" << tr
                         << "clip=" << (c->name.isEmpty() ? c->id : c->name)
                         << "top=" << (topClip && c->id == topClip->id);
        }
    }

    if (composeDbg()) {
        for (const Layer& L : layers) {
            int cornerA = -1;
            if (!L.frame.isNull() && L.frame.width() > 2 && L.frame.height() > 2)
                cornerA = L.frame.pixelColor(1, 1).alpha();
            qDebug() << "  [compose] layer " << L.frame.width() << "x" << L.frame.height()
                     << "alpha=" << L.alpha << "cornerA=" << cornerA << "mode=" << (int)L.mode;
        }
        qDebug() << "  [compose] total=" << layers.size() << "m_frameNull=" << m_frame.isNull();
    }

    if (m_frame.isNull() && layers.isEmpty() && !anyText) {
        drawEmptyMonitor(p, canvas);
        return;
    }

    // Empilhamento multi-faixa: compõe num buffer do tamanho do canvas, faixa
    // a faixa com o modo de composição da faixa (igual ao blend da exportação),
    // de baixo para cima — é o que permite ver transparência (PNG/WebP com
    // alpha) e as camadas que ficam POR BAIXO do clipe do topo.
    // O fundo é PRETO opaco (como o ffmpeg): os modos de blend (multiply,
    // screen…) produzem o mesmo resultado da exportação, e não dependem do
    // fundo do monitor.
    if (layers.size() >= 2 || (m_frame.isNull() && !layers.isEmpty())) {
        // Cache: reutiliza se playhead e camadas não mudaram.
        if (!m_compositedCache.isNull() && m_compositedEpoch == m_currentFrameIndex
            && m_compositedCache.size() == canvas.size()
            && m_compositedLayerCount == layers.size()) {
            if (PreviewProfiler::active()) {
                profFrame().compositeEvaluated = true;
                profFrame().compositeHit = true;
            }
            p.drawImage(canvas.topLeft(), m_compositedCache);
            drawMaskOverlay(p, canvas, k);
            return;
        }
        if (PreviewProfiler::active()) {
            profFrame().compositeEvaluated = true;
            profFrame().compositeHit = false;
            profFrame().layers = layers.size();
        }
        PreviewProfiler::Scope compScope(PreviewProfiler::active() ? &profFrame().compositeNs : nullptr);
        // Buffer reutilizado entre frames (é o próprio m_compositedCache): alocar
        // um ARGB32 novo do canvas a cada quadro (~8 MB @1080p, 33 MB @4K) era
        // megas de allocator por frame à toa. fill() no mesmo buffer mantém a
        // capacidade; o epoch garante que repaints do mesmo frame só redesenham.
        if (m_compositedCache.size() != canvas.size()
            || m_compositedCache.format() != QImage::Format_ARGB32) {
            m_compositedCache = QImage(canvas.size(), QImage::Format_ARGB32);
        }
        QImage& acc = m_compositedCache;
        acc.fill(Qt::black);
        const double cx = acc.width() / 2.0;
        const double cy = acc.height() / 2.0;
        for (const Layer& L : layers) {
            if (L.frame.isNull()) continue;
            QPainter ap(&acc);
            ap.setClipRect(QRect(0, 0, acc.width(), acc.height()));
            ap.setCompositionMode(L.mode);
            drawLayer(ap, L.frame, L.s, L.rot, L.x, L.y, L.alpha, L.sX, L.sY, k, cx, cy,
                      QRect(0, 0, acc.width(), acc.height()), L.ox, L.oy);
        }
        // Texto sempre em SourceOver (o modo de composição da última camada
        // não pode vazar para o texto).
        QPainter ap(&acc);
        ap.setClipRect(QRect(0, 0, acc.width(), acc.height()));
        ap.setCompositionMode(QPainter::CompositionMode_SourceOver);
        // Texto (independente e anexado) por cima das camadas.
        if (m_project) {
            for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
                if (!m_project->videoTracks[tr].visible) continue;   // faixa oculta (olho)
                const Clip* tclip = nullptr;
                for (const Clip& c : m_project->videoTracks[tr].clips)
                    if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur)
                        if (!tclip || c.pos > tclip->pos) tclip = &c;
                if (tclip) drawClipText(ap, acc.rect(), tclip, k);
            }
        }
        if (clip && !clip->isText)
            drawClipText(ap, acc.rect(), clip, k);
        ap.end();
        m_compositedEpoch = m_currentFrameIndex;
        m_compositedLayerCount = layers.size();
        p.drawImage(canvas.topLeft(), acc);
        drawMaskOverlay(p, canvas, k);
        return;
    }

    // Caminho tradicional (um clipe só, com ou sem transição): desenha direto.
    // Opacidade do clipe + fades de entrada/saída + opacidade da faixa
    // aplicados no alpha (como na exportação), para o fade aparecer mesmo com
    // um clipe sozinho.
    const double clipRel = clip ? (m_playhead - clip->pos) : 0.0;
    double clipAlpha = 1.0;
    double clipTrackOpacity = 1.0;
    if (clip && m_project) {
        clipAlpha = std::clamp(kfValue(clip->kfOpacity, clip->opacity, clipRel), 0.0, 1.0);
        if (clip->fadeIn > 1e-6) clipAlpha *= std::min(1.0, clipRel / clip->fadeIn);
        if (clip->fadeOut > 1e-6) clipAlpha *= std::min(1.0, (clip->dur - clipRel) / clip->fadeOut);
        for (const Track& tr : m_project->videoTracks)
            for (const Clip& c : tr.clips)
                if (c.id == clip->id) { clipTrackOpacity = std::clamp(tr.opacity, 0.0, 1.0); break; }
    }
    clipAlpha *= clipTrackOpacity;
    // Para clipes Mesa, usa o quadro renderizado em vez de m_frame
    const QImage& singleFrame = m_frame;
    const bool transActive = m_transAlpha >= 0.0
                             && !singleFrame.isNull() && !m_underFrame.isNull();
    if (!singleFrame.isNull()) {
        if (transActive) {
            drawLayer(p, m_underFrame, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, k,
                      canvas.center().x(), canvas.center().y(), canvas);
            double ox = 0.0, oy = 0.0;
            if (m_transType == QStringLiteral("wipeleft")) ox = pw * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wiperight")) ox = -pw * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipeup")) oy = ph * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipedown")) oy = -ph * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipetl")) { ox = pw * (1.0 - m_transAlpha); oy = ph * (1.0 - m_transAlpha); }
            else if (m_transType == QStringLiteral("wipetr")) { ox = -pw * (1.0 - m_transAlpha); oy = ph * (1.0 - m_transAlpha); }
            else if (m_transType == QStringLiteral("wipebr")) { ox = -pw * (1.0 - m_transAlpha); oy = -ph * (1.0 - m_transAlpha); }
            else if (m_transType == QStringLiteral("wipebl")) { ox = pw * (1.0 - m_transAlpha); oy = -ph * (1.0 - m_transAlpha); }
            double a = clipAlpha;
            if (m_transType == QStringLiteral("dissolve")) a *= m_transAlpha;
            drawLayer(p, singleFrame, S, rot, tx, ty, a, SX, SY, k,
                      canvas.center().x(), canvas.center().y(), canvas, ox, oy);
        } else {
            drawLayer(p, singleFrame, S, rot, tx, ty, clipAlpha, SX, SY, k,
                      canvas.center().x(), canvas.center().y(), canvas);
        }
    }

    // Clipes de texto independentes: desenha todos os ativos no playhead, da
    // faixa de baixo para a de cima (a faixa 0 é o topo e fica por cima).
    if (m_project) {
        for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
            const Track& tv = m_project->videoTracks[tr];
            if (!tv.visible) continue;   // faixa oculta (olho)
            const Clip* tclip = nullptr;
            for (const Clip& c : tv.clips)
                if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur)
                    if (!tclip || c.pos > tclip->pos) tclip = &c;
            if (tclip) drawClipText(p, canvas, tclip, k);
        }
    }
    // Texto anexado a um clipe de vídeo (comportamento antigo) ainda vale.
    if (clip && !clip->isText)
        drawClipText(p, canvas, clip, k);

    // Overlay de edição de máscara (formas + alças do clipe sob edição).
    // (A margem de segurança saiu daqui: os caminhos do composto retornam
    // antes, e ela sumia em projetos com 2+ camadas — ver paintEvent.)
    drawMaskOverlay(p, canvas, k);
}

void PreviewWidget::drawPerfOverlay(QPainter& p) {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_PERF_DEBUG");
    if (!on) return;
    // Estágios do tick corrente (ms). O registro vive no PlaybackEngine e é
    // preenchido pelos dois lados — o widget escreve as fases assíncronas
    // (worker, prefetch, paint) no mesmo registro em que o tick está.
    const PreviewProfiler::Frame& fr = profFrame();
    const PreviewProfiler& prof = PreviewProfiler::instance();
    const auto ms = [](double ns) { return ns / 1e6; };

    p.save();
    p.resetTransform();
    const QRect box(8, 8, 392, 190);
    p.fillRect(box, QColor(0, 0, 0, 190));
    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255));
    const QString txt = QStringLiteral(
        "fonte: %1   quadros: %2   p95 tick %3 ms  p95 decode %4 ms\n"
        "tick %5 ms = clock %6 + seek %7 + prefetch %8 + mix %9\n"
        "decode(worker->frame) %10 ms   prefetch->ready %11 ms\n"
        "paint %12 ms (comp %13)   resolve proxy %14 ms\n"
        "prefetch %15   fila %16   camadas %17   cortes %18\n"
        "dropped %19   skip %20   adaptive %21\n"
        "abre %22 fonte(s)   open total %23 ms   ultima %24 ms   pool %25 quentes")
        .arg(prof.wroteProxy() ? QStringLiteral("PROXY")
                               : QStringLiteral("original"))
        .arg(prof.frameCount())
        .arg(ms(prof.stats(QStringLiteral("tickNs")).p95), 0, 'f', 1)
        .arg(ms(prof.stats(QStringLiteral("workerNs")).p95), 0, 'f', 1)
        .arg(ms(fr.tickNs), 0, 'f', 1)
        .arg(ms(fr.clockNs), 0, 'f', 1)
        .arg(ms(fr.seekNs), 0, 'f', 1)
        .arg(ms(fr.prefetchNs), 0, 'f', 1)
        .arg(ms(fr.mixNs), 0, 'f', 1)
        .arg(ms(fr.workerNs), 0, 'f', 1)
        .arg(ms(fr.prefetchLatNs), 0, 'f', 1)
        .arg(ms(fr.paintNs), 0, 'f', 1)
        .arg(ms(fr.compositeNs), 0, 'f', 1)
        .arg(ms(fr.resolveNs), 0, 'f', 1)
        .arg(fr.prefetchHit ? QStringLiteral("HIT") : QStringLiteral("miss"))
        .arg(fr.queue)
        .arg(fr.layers)
        .arg(prof.cuts())
        .arg(m_perf.droppedTotal)
        .arg(fr.skipped)
        .arg(m_adaptiveActive
             ? QStringLiteral("ON (%1p)").arg(m_previewQuality)
             : QStringLiteral("OFF"))
        .arg(FFmpegDecoder::openCount())
        .arg(ms(FFmpegDecoder::openTotalNs()), 0, 'f', 1)
        .arg(ms(FFmpegDecoder::lastOpenNs()), 0, 'f', 1)
        .arg(m_decPool ? m_decPool->count() : 0);
    p.drawText(box.adjusted(10, 8, -6, -6), Qt::AlignLeft | Qt::AlignTop, txt);
    p.restore();
}

// Desenha o texto/título estilizado do clipe sobre o monitor (mesmo resultado
// visual do drawtext da exportação). O desenho acontece em espaço de PROJETO
// mapeado para o canvas (escala k), aplicando o transform animável do clipe
// (escala, rotação, pan) e a opacidade (fades/keyframes).
void PreviewWidget::drawClipText(QPainter& p, const QRect& canvas, const Clip* clip, double k) {
    if (!clip || !m_project) return;
    const TextStyle& st = *m_project->textStyleFor(*clip);
    if (st.isEmpty()) return;
    const double W = m_project->width;
    const double H = m_project->height;
    const double rel = m_playhead - clip->pos;
    double alpha = std::clamp(kfValue(clip->kfOpacity, clip->opacity, rel), 0.0, 1.0);
    if (clip->fadeIn > 1e-6) alpha *= std::min(1.0, rel / clip->fadeIn);
    if (clip->fadeOut > 1e-6) alpha *= std::min(1.0, (clip->dur - rel) / clip->fadeOut);
    if (alpha <= 0.0) return;

    // LAINKA no texto: jitter de posição, rotação, escala e flicker.
    double jtX = 0.0, jtY = 0.0, jtRot = 0.0, jtScale = 1.0;
    if (clip->lainkaEnabled) {
        const uint h = lainkaHash(clip->id, m_playhead);
        if (clip->lainkaJitterPos > 1e-6) {
            const double nx = ((h & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            const double ny = (((h >> 16) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            const double maxPx = clip->lainkaJitterPos * 0.5; // escala para texto
            jtX = nx * maxPx;
            jtY = ny * maxPx;
        }
        if (clip->lainkaJitterRot > 1e-6) {
            const double n = ((lainkaHashN(clip->id, m_playhead, 11) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            jtRot = n * clip->lainkaJitterRot * 0.3;
        }
        if (clip->lainkaJitterScale > 1e-6) {
            const double n = ((lainkaHashN(clip->id, m_playhead, 12) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            jtScale = 1.0 + n * clip->lainkaJitterScale * 0.01;
        }
        if (clip->lainkaFlicker > 1e-6) {
            const double n = ((lainkaHashN(clip->id, m_playhead * (clip->lainkaFlickerSpeed / 50.0), 13) & 0xFF) / 255.0);
            alpha *= (1.0 + (n * 2.0 - 1.0) * (clip->lainkaFlicker / 100.0));
            alpha = std::clamp(alpha, 0.0, 1.0);
        }
    }

    const textrender::Layout L = textrender::layout(st, W, H);
    const QRectF box = textrender::centeredBox(st, L, W, H);

    p.save();
    p.setClipRect(canvas);
    p.setOpacity(alpha);
    // Mapeia espaço de projeto (0..W x 0..H) para o canvas.
    p.translate(canvas.topLeft());
    p.scale(k, k);
    // Transform animável do clipe, em torno do centro do quadro.
    p.translate(W / 2.0 + kfValue(clip->kfTx, clip->tx, rel) + jtX,
                H / 2.0 + kfValue(clip->kfTy, clip->ty, rel) + jtY);
    p.rotate(kfValue(clip->kfRotation, clip->rotation, rel) + jtRot);
    p.scale(kfValue(clip->kfScale, clip->scale, rel) * jtScale
                * kfValue(clip->kfScaleX, clip->scaleX, rel),
            kfValue(clip->kfScale, clip->scale, rel) * jtScale
                * kfValue(clip->kfScaleY, clip->scaleY, rel));

    textrender::paint(p, st, L, box, H);

    p.restore();
}

// Tela vazia: ícone de play grande + título + dica de como começar, no tom
// monitorLabel com alfa (respeita o tema claro/escuro).
void PreviewWidget::drawEmptyMonitor(QPainter&, const QRect&) {
    // Tela preta limpa quando não há clipe — sem ícone nem texto.
}

void PreviewWidget::seek(double t) {
    PlaybackEngine::seek(t);
}

// Quadro a quadro (Premiere: os botões ◀▶ do monitor). Pausa a reprodução
// (senão o seek brigaria com o relógio do playback) e busca ±1 quadro.
void PreviewWidget::stepFrame(int dir) {
    if (!m_project || m_project->duration() <= 0) return;
    if (m_playing) togglePlay();
    const double fps = projFps(m_project);
    const double t = m_playhead + (dir > 0 ? 1.0 : -1.0) / fps;
    seek(std::clamp(t, 0.0, m_project->duration()));
}

// Slot público do passo a passo, para os botões ◀▶ da janela de preview externo.
void PreviewWidget::stepFrameBy(int dir) {
    stepFrame(dir);
}

void PreviewWidget::onSeek(double t) {
    applySeekVisual(t);
}

// Intervalo do "desengasgo" (flush periódico dos buffers do decoder): vidrado
// em `desengasgaIntervalMs()` (PreviewDebug.h) — inclui a medição que justifica
// o padrão desligado (0).
//
// Decodifica e desenha o quadro na posição `t` (o transporte/relógio é do
// PlaybackEngine; aqui só a parte visual/decodificação).
void PreviewWidget::applySeekVisual(double t) {
    Q_UNUSED(t);
    updateTimecodeLabel();
    if (m_scrubBar) m_scrubBar->update();

    // Desengasgo periódico: a cada ~10s durante playback contínuo, libera os
    // buffers internos do decoder (DPB + caches de 2 frames) que degradam o
    // frameAt em vídeos longos — a causa do vídeo engasgar com áudio perfeito.
    // O flush é barato (releaseBuffers não fecha o arquivo; o próximo pedido
    // faz re-seek) e roda no worker thread, seriado com os decodeOne.
    if (m_playing && m_frameWorker && desengasgaIntervalMs() > 0) {
        const qint64 nowMs = m_desengasgaT.isValid()
                                 ? m_desengasgaT.elapsed() : 0;
        if (!m_desengasgaT.isValid()) {
            m_desengasgaT.start();
            m_desengasgaLastMs = 0;
        } else if (nowMs - m_desengasgaLastMs >= desengasgaIntervalMs()) {
            // Medido numa fonte 1080p60 (H.264 High@4.2, 252 s, 13 Mbps):
            // cada flush custa um re-seek frio de 114-304 ms, ou seja, 4
            // travamentos de ~0,25 s em 120 s de reprodução — exatamente a cada
            // 30 s. Baixar de 10s para 30s só dilatou o custo, não o reduziu.
            // E o problema que ele existia para evitar não aparece: com o flush
            // desligado o p95 de decode ficou plano (0,35-0,37 ms) nos 4
            // minutos, sem degradação. Por isso o intervalo virou configurável
            // (PIERROT_DESENGASGA_MS=0) em vez de fixo — desligar é o padrão
            // melhor medido, mas se algum dia o engasgo reaparecer em outra
            // fonte, basta ligar de volta por env var.
            m_desengasgaLastMs = nowMs;
            QMetaObject::invokeMethod(m_frameWorker, "desengasga", Qt::QueuedConnection);
        }
    }
    updateFrame();
    // Agulha parada: aquece o clipe sob ela para o próximo play sair quente.
    scheduleIdleWarm();
    update();
}

double PreviewWidget::audioClockSec() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (m_audioSink) return m_audioSink->processedUSecs() / 1.0e6;
#else
    if (m_audioOut) return m_audioOut->processedUSecs() / 1.0e6;
#endif
    return -1.0;
}

void PreviewWidget::togglePlay() {
    PlaybackEngine::togglePlay();
}

void PreviewWidget::shuttle(int dir) {
    PlaybackEngine::shuttle(dir);
}

void PreviewWidget::playFrom(double t) {
    PlaybackEngine::playFrom(t);
}

void PreviewWidget::setLoopRange(double in, double out) {
    PlaybackEngine::setLoopRange(in, out);
    if (m_scrubBar) m_scrubBar->update(); // região de loop sombreada
}

void PreviewWidget::setLoopEnabled(bool enabled) {
    PlaybackEngine::setLoopEnabled(enabled);
    if (m_scrubBar) m_scrubBar->update();
}

// Hooks do PlaybackEngine: o transporte decide quando; aqui a PreviewWidget
// faz o áudio e o rendering.

void PreviewWidget::onStartAudio(double t) { startAudio(t); }
void PreviewWidget::onStopAudio() { stopAudio(); }
void PreviewWidget::onMixAudio(double t, bool reseek) { updateMixAudio(t, reseek); }
void PreviewWidget::onPrefetch() { updatePrefetch(); }

// Limpa buffers de frame quando o transporte para (sem duplicar o estado do
// PlaybackEngine, que já zera o relógio).
void PreviewWidget::onStopPlaybackUI() {
    QMutexLocker l(&m_frameMutex);
    m_prefetch = PrefetchFrame();
}

void PreviewWidget::onPlayheadMoved(double t) {
    updateTimecodeLabel();
    if (m_scrubBar) m_scrubBar->update();
    emit playheadMoved(t);
}

void PreviewWidget::onStateChanged(bool playing) {
    // O PlaybackEngine troca o rótulo do botão a cada transição; como ele está
    // em modo ícone (estilo Premiere), restauramos o ícone e limpamos o texto.
    if (m_playBtn) {
        m_playBtn->setIcon(playing ? m_pauseIcon : m_playIcon);
        m_playBtn->setText(QString());
    }
    updateTimecodeLabel();
    // Marco do play para medir "apertar play -> 1º quadro" (o log sai em
    // onFrameReady, com PIERROT_PLAY_DEBUG=1).
    m_playStartNs = playing ? PreviewProfiler::nowNs() : 0;
    emit stateChanged(playing);
}

void PreviewWidget::setZoom(double z) {
    m_zoom = z;
    if (m_zoomCombo) {
        int idx = 0;
        for (int i = 0; i < 7; ++i) {
            if (std::fabs(kMonitorZooms[i] - z) < 1e-6) { idx = i; break; }
        }
        QSettings().setValue(QStringLiteral("monitor/zoom"), idx);
        QSignalBlocker b(m_zoomCombo);
        m_zoomCombo->setCurrentIndex(idx);
    }
    update();
}

void PreviewWidget::setPreviewQuality(int width) {
    m_previewQuality = width;
    QSettings().setValue(QStringLiteral("monitor/quality"), width);
    // Se o usuário mudou manualmente, reseta o estado adaptativo.
    m_adaptiveSlowCount = 0;
    m_adaptiveActive = false;
    m_adaptiveDecodeMsAvg = 0.0;
    // Invalida o quadro atual e força a reconversão na nova resolução.
    {
        QMutexLocker l(&m_frameMutex);
        if (!m_frame.isNull()) m_frame = QImage();
    }
    m_shownW = -1;
    m_lastDecodeW = -1;
    for (QAction* a : m_qualityMenu->actions())
        a->setChecked(a->data().toInt() == width);
    for (const PreviewQOpt& o : kPreviewQualities)
        if (o.width == width) {
            if (m_qualityBtn) m_qualityBtn->setText(QString::fromUtf8(o.code));
            break;
        }
    update();
    updateFrame();
}

bool PreviewWidget::tryRenderMesa(const Clip* clip) {
    if (!m_project || !clip) return false;

    // Encontra a faixa do clipe ativo.
    const Track* track = nullptr;
    for (const Track& tr : m_project->videoTracks) {
        for (const Clip& c : tr.clips)
            if (c.id == clip->id) { track = &tr; break; }
    }
    if (!track) return false;

    // A Mesa dona da track é descoberta via mesa.trackIds (fonte da verdade,
    // a mesma usada pelo MesaWidget/MesaRenderer). O caminho antigo
    // track.groupId→group.mesaId quebrava quando a pasta da Mesa era
    // excluída/desagrupada: a composição sumia do preview ao reabrir.
    const MesaComposition* mc = m_project->findMesaForTrack(track->id);
    if (!mc) return false;

    // Renderiza o canvas + transform de câmera (saída no tamanho do projeto).
    const QImage out = m_mesaRenderer.render(*mc, *m_project, m_playhead);
    if (out.isNull()) return false;

    {
        QMutexLocker l(&m_frameMutex);
        m_frameFull = out;
        m_shownPath = QStringLiteral("mesa:") + mc->id;
        m_shownT = m_playhead;
        m_shownW = m_project->width;
        m_lastSrcT = m_playhead;
        m_lastDecodeW = m_project->width;
        m_lastFile.clear();
        m_prefetch.valid = false;
        m_prefetch.requested = false;
    }
    // Nenhum efeito de clipe deve ser aplicado à composição da Mesa.
    m_clipLainkaEnabled = false;
    m_clipOfxFx.clear(); m_clipMotionEnabled = false; m_clipFrei0rFx.clear();
    m_clipMasks.clear();
    applyCrop();
    return true;
}


void PreviewWidget::prewarmAudioSink() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (m_audioSink) {
        const QAudioDevice def = QMediaDevices::defaultAudioOutput();
        if (def.isNull() || m_audioSinkDevice == QString::fromUtf8(def.id())) return;
    }
    const QAudioDevice def = QMediaDevices::defaultAudioOutput();
    if (def.isNull()) return;
    const QString devId = QString::fromUtf8(def.id());
    if (m_audioSink) {
        m_audioSink->stop();
        m_audioSink->deleteLater();
        m_audioSink = nullptr;
    }
    // Primeira criação = inicializa o backend de áudio do Qt (~1s). Paga aqui,
    // ocioso, em vez de travar o 1º play.
    m_audioSink = new QAudioSink(def, previewAudioFormat(), this);
    m_audioSinkDevice = devId;
#else
    if (m_audioOut) {
        const QAudioDeviceInfo def = QAudioDeviceInfo::defaultOutputDevice();
        if (def.isNull() || m_audioOutDevice == def.deviceName()) return;
    }
    const QAudioDeviceInfo def = QAudioDeviceInfo::defaultOutputDevice();
    if (def.isNull()) return;
    const QString devId = def.deviceName();
    if (m_audioOut) {
        m_audioOut->stop();
        m_audioOut->deleteLater();
        m_audioOut = nullptr;
    }
    m_audioOut = new QAudioOutput(def, previewAudioFormat(), this);
    m_audioOutDevice = devId;
#endif
}

void PreviewWidget::startAudio(double t) {
    if (!m_project) return;
    QElapsedTimer tStage;
    tStage.start();
    stopAudio();
    const QVector<AudioMixer::SourceInfo> sources = buildMixSources(m_project, t);
    if (sources.isEmpty() && audioDbg())
        qDebug() << "[audio] startAudio em t=" << t << "- nenhuma fonte (mixer será criado mudo)";
    if (audioDbg()) {
        qDebug() << "[audio] startAudio em t=" << t << "- fontes:" << sources.size();
        const auto outs = QMediaDevices::audioOutputs();
        for (const auto& d : outs)
            qDebug() << "[audio]   saida:" << d.description() << "default?" << d.isDefault();
        const QAudioDevice def = QMediaDevices::defaultAudioOutput();
        qDebug() << "[audio]   defaultAudioOutput() válido?" << def.isNull() << "-" << def.description();
    }

    const QAudioFormat fmt = previewAudioFormat();

    m_audioFeed = new AudioMixer(this);
    m_audioFeed->setMasterVolume(m_project ? m_project->masterVolume : 1.0);
    if (m_project) {
        m_audioFeed->setMasterGraphicEq(m_project->masterGraphicEq);
        m_audioFeed->setMasterParametricEq(m_project->masterParametricEq);
    }
    m_audioFeed->open(QIODevice::ReadOnly | QIODevice::Unbuffered);

    // O mixer lê SÓ do conform (PCM já decodificado em background) → está
    // sempre "pronto": não há decoders a abrir, então o sink já começa. Faltas
    // de trecho são silêncio momentâneo até o worker terminar a janela pedida.
    // Nada de waitReadyBeforeSink() aqui: ele segurava a UI por até 150ms no
    // play (C1 do relatório). `updateSources` já dispara a conform em
    // background; aceitamos 1-2 frames de silêncio no warm-up frio.
    m_audioFeed->updateSources(sources, /*reseek=*/true,
                               QVector<AudioMixer::SourceInfo>(), -1, t);
    const qint64 afterSourcesMs = tStage.elapsed();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QAudioDevice def = QMediaDevices::defaultAudioOutput();
    if (def.isNull()) {
        if (audioDbg()) qDebug() << "[audio] defaultAudioOutput nulo — sem saída de áudio";
        return;
    }
    // Reusa o sink entre play/stop (C4): new/destroy custa 10-50ms por play.
    // Recria só se a saída padrão mudou.
    const QString devId = QString::fromUtf8(def.id());
    if (!m_audioSink || m_audioSinkDevice != devId) {
        if (m_audioSink) {
            m_audioSink->stop();
            m_audioSink->deleteLater();
            m_audioSink = nullptr;
        }
        m_audioSink = new QAudioSink(def, fmt, this);
        m_audioSinkDevice = devId;
    }
    m_audioSink->start(m_audioFeed);
#else
    const QAudioDeviceInfo def = QAudioDeviceInfo::defaultOutputDevice();
    if (def.isNull()) {
        if (audioDbg()) qDebug() << "[audio] defaultOutputDevice nulo — sem saída de áudio";
        return;
    }
    const QString devId = def.deviceName();
    if (!m_audioOut || m_audioOutDevice != devId) {
        if (m_audioOut) {
            m_audioOut->stop();
            m_audioOut->deleteLater();
            m_audioOut = nullptr;
        }
        m_audioOut = new QAudioOutput(def, fmt, this);
        m_audioOutDevice = devId;
    }
    m_audioOut->start(m_audioFeed);
#endif
    if (playDbg())
        qDebug().noquote()
            << QStringLiteral("[play] startAudio: updateSources=%1ms sink=%2ms")
                   .arg(afterSourcesMs)
                   .arg(tStage.elapsed() - afterSourcesMs);
}

void PreviewWidget::stopAudio() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (m_audioSink) {
        // Não destrói: o sink é reutilizado no próximo play (C4). `start()`
        // troca a fonte para o novo m_audioFeed. O `deleteLater` da antiga
        // fonte é seguro porque o sink já foi parado e largou a referência.
        m_audioSink->stop();
    }
#else
    if (m_audioOut) {
        m_audioOut->stop();
    }
#endif
    if (m_audioFeed) {
        // shutdown() síncrono: bloqueia novos updates e libera as fontes antes
        // do mixer ser descartado. As caches do conform continuam no registro.
        m_audioFeed->shutdown();
        m_audioFeed->deleteLater();
        m_audioFeed = nullptr;
    }
}

// Recalcula as fontes de áudio ativas no instante `t` e empurra ao mixer.
// Com o conform, tudo aqui é barato e síncrono (sem open/seekAudio no caminho):
// updateSources só troca ponteiros de leitura e emite pedidos de conform para
// o worker — que decodifica em background, em região por região.
void PreviewWidget::updateMixAudio(double t, bool reseek) {
    if (!m_audioFeed) return;
    m_audioFeed->setMasterVolume(m_project ? m_project->masterVolume : 1.0);
    if (m_project) {
        m_audioFeed->setMasterGraphicEq(m_project->masterGraphicEq);
        m_audioFeed->setMasterParametricEq(m_project->masterParametricEq);
    }
    const QVector<AudioMixer::SourceInfo> sources = buildMixSources(m_project, t);
    const QVector<AudioMixer::SourceInfo> warm =
        buildWarmSources(m_project, t, sources);
    m_audioFeed->updateSources(sources, reseek, warm, -1, t);
}

QString PreviewWidget::resolvePreviewVideo(const QString& srcPath) {
    if (!PreviewProfiler::active())
        return ProxyManager::instance().resolveVideo(srcPath);
    const qint64 t0 = PreviewProfiler::nowNs();
    const QString resolved = ProxyManager::instance().resolveVideo(srcPath);
    profFrame().resolveNs += PreviewProfiler::nowNs() - t0;
    return resolved;
}

void PreviewWidget::setOfxManager(OfxPluginManager* m) {
    m_ofxManager = m;
    if (m_frameWorker) m_frameWorker->setEffectManagers(m_ofxManager, m_frei0rManager);
}

void PreviewWidget::setFrei0rManager(Frei0rPluginManager* m) {
    m_frei0rManager = m;
    if (m_frameWorker) m_frameWorker->setEffectManagers(m_ofxManager, m_frei0rManager);
}

void PreviewWidget::updateFrame() {
    updateTimecodeLabel();
    if (!m_project) { m_frame = QImage(); m_transAlpha = -1.0; m_underFrame = QImage(); m_underRequested = false; m_underPending = false; m_clipMasks.clear(); update(); return; }

    const Clip* clip = clipAt(m_playhead);
    if (!clip) {
        m_frame = QImage();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
        m_underPending = false;
        m_clipLainkaEnabled = false;
        m_clipOfxFx.clear(); m_clipMotionEnabled = false; m_clipFrei0rFx.clear();
    m_clipMasks.clear();
        update();
        return;
    }

    // Clipe pertence a um grupo Mesa → renderiza a composição inteira
    // (todas as camadas), sem a transform de câmera.
    if (tryRenderMesa(clip)) {
        // Mesmo com Mesa no topo, tracks inferiores precisam ser decodificadas
        // para que paintEvent possa compô-las por baixo (transparência, blend).
        const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
        const int decW = qMax(160, qMax(m_previewQuality, widgetW));
        requestLowerLayers(decW);
        m_underFrame = QImage();
        m_underRequested = false;
        m_underPending = false;
        m_transAlpha = -1.0;
        m_clipLainkaEnabled = false;
        m_clipOfxFx.clear(); m_clipMotionEnabled = false; m_clipFrei0rFx.clear();
    m_clipMasks.clear();
        update();
        return;
    }

    const MediaItem* m = m_project->findMedia(clip->mediaIdAt(m_playhead - clip->pos));
    if (!m || !m->hasVideo) {
        m_frame = QImage();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
        m_underPending = false;
        m_clipLainkaEnabled = false;
        m_clipOfxFx.clear(); m_clipMotionEnabled = false; m_clipFrei0rFx.clear();
    m_clipMasks.clear();
        update();
        return;
    }

    // Captura estado do LAINKA para uso em applyCrop().
    m_clipLainkaEnabled = clip->lainkaEnabled;
    m_clipLainkaSkip = clip->lainkaSkip;
    m_clipLainkaJitterPos = clip->lainkaJitterPos;
    m_clipLainkaJitterRot = clip->lainkaJitterRot;
    m_clipLainkaJitterScale = clip->lainkaJitterScale;
    m_clipLainkaFlicker = clip->lainkaFlicker;
    m_clipLainkaFlickerSpeed = clip->lainkaFlickerSpeed;
    m_clipLainkaWarpAmount = clip->lainkaWarpAmount;
    m_clipLainkaWarpSpeed = clip->lainkaWarpSpeed;
    m_clipLainkaWarpGrid = clip->lainkaWarpGrid;
    m_clipLainkaOnionSkin = clip->lainkaOnionSkin;
    m_clipLainkaDustAmount = clip->lainkaDustAmount;
    m_clipLainkaScratchAmount = clip->lainkaScratchAmount;
    m_clipLainkaTargetFps = clip->lainkaTargetFps;
    m_clipLainkaMotionBlur = clip->lainkaMotionBlur;
    m_clipLainkaOpacity = clip->lainkaOpacity;
    m_clipLainkaAntialias = clip->lainkaAntialias;
    m_clipLainkaId = clip->id;
    m_clipMotionEnabled = clip->motionEnabled;
    m_clipMotionAmount = clip->motionAmount;
    m_clipMotionAngle = clip->motionAngle;
    m_clipMotionSamples = clip->motionSamples;
    m_clipBrightness = clip->brightness;
    m_clipContrast = clip->contrast;
    m_clipSaturation = clip->saturation;
    m_clipBlur = clip->blur;
    m_clipGrayscale = clip->grayscale;
    m_clipChromaKey = clip->chromaKey;
    m_clipChromaKeyColor = clip->chromaKeyColor;
    m_clipChromaKeySimilarity = clip->chromaKeySimilarity;
    m_clipChromaKeySoftness = clip->chromaKeySoftness;
    m_clipChromaKeySpillSuppress = clip->chromaKeySpillSuppress;
    m_clipPsxEnabled = clip->psxEnabled;
    m_clipPsxDither = clip->psxDither;
    m_clipPsxBits = clip->psxBits;
    m_clipFilmGrain = clip->filmGrain;
    m_clipFilmGrainAmount = clip->filmGrainAmount;
    m_clipFilmGrainSize = clip->filmGrainSize;
    m_clipOfxFx = clip->ofxFx;
    m_clipFrei0rFx = clip->frei0rFx;
    m_clipMasks = clip->masks;

    double srcT = clipSrcTime(*clip, m_playhead - clip->pos);

    // LAINKA: quantiza o tempo para simular stop motion.
    // Usa targetFps para calcular o skip (projectFps / targetFps).
    m_lainkaQuantizedTime = -1.0;
    if (clip->lainkaEnabled) {
        const double pfps = projFps(m_project);
        const int effSkip = (clip->lainkaTargetFps > 0 && pfps > 1.0)
            ? std::max(1, (int)std::lround(pfps / clip->lainkaTargetFps))
            : clip->lainkaSkip;
        if (effSkip > 1) {
            srcT = lainkaQuantizeTime(srcT, effSkip, pfps);
            m_lainkaQuantizedTime = srcT;
        }
    }
    // Decodifica na maior entre a qualidade escolhida e a largura exibida:
    // nunca abaixo do tamanho de tela (evita pixelização ao esticar), e nem
    // além do que o monitor precisa, exceto quando a qualidade pede mais.
    const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
    const int decW = qMax(160, qMax(m_previewQuality, widgetW));

    // Camadas inferiores (empilhamento multi-faixa): pede os quadros dos
    // clipes de vídeo que estão por baixo do topo, para compor transparência
    // e blend no preview (antes só o clipe do topo aparecia).
    requestLowerLayers(decW);

    // Mídia gerada (gerador estilo Vegas): gera o quadro (cor, gradiente,
    // checkerboard ou ruído), sem passar pelo decoder (não há arquivo).
    if (m->isSolid) {
        const int w = qMax(1, m->width > 0 ? m->width
                          : (m_project ? m_project->width : 1920));
        const int h = qMax(1, m->height > 0 ? m->height
                          : (m_project ? m_project->height : 1080));
        QImage img = generatorFrame(*m, w, h);
        {
            QMutexLocker l(&m_frameMutex);
            m_frameFull = img;
            m_shownPath = QStringLiteral("solid:") + m->id;
            m_shownT = srcT;
            m_shownW = decW;
            m_lastSrcT = srcT;
            m_lastDecodeW = decW;
            m_lastFile = QString();
            m_prefetch.valid = false;
            m_prefetch.requested = false;
        }
        applyCrop();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
        m_underPending = false;
        update();
        return;
    }

    const int cL = (int)std::lround(
        std::clamp(kfValue(clip->kfCropL, clip->cropL, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    const int cR = (int)std::lround(
        std::clamp(kfValue(clip->kfCropR, clip->cropR, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    const int cT = (int)std::lround(
        std::clamp(kfValue(clip->kfCropT, clip->cropT, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    const int cB = (int)std::lround(
        std::clamp(kfValue(clip->kfCropB, clip->cropB, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    m_lastCropL = cL;
    m_lastCropR = cR;
    m_lastCropT = cT;
    m_lastCropB = cB;

    // Transição: procura o clipe anterior da MESMA faixa que se sobrepõe a
    // este (estilo Vegas). O quadro de trás é decodificado pelo canal de
    // prefetch e o paintEvent compõe dissolve/wipe durante a sobreposição.
    const Clip* under = nullptr;
    double overlap = 0.0;
    for (const Track& tr : m_project->videoTracks) {
        bool inTrack = false;
        for (const Clip& c : tr.clips)
            if (c.id == clip->id) { inTrack = true; break; }
        if (!inTrack) continue;
        for (const Clip& o : tr.clips)
            if (o.id != clip->id && o.pos < clip->pos - 1e-6
                && o.pos + o.dur > clip->pos + 1e-6
                && (!under || o.pos > under->pos))
                under = &o;
        break;
    }
    // A transição não pode durar mais que o clipe da frente (paridade com o
    // clamp do export): se a sobreposição passar da duração dele, a transição
    // completa no fim do clipe em vez de nunca chegar a 100%.
    if (under)
        overlap = std::min(under->pos + under->dur - clip->pos, clip->dur);
    if (under && overlap > 1e-6 && m_playhead < clip->pos + overlap - 1e-9) {
        m_transAlpha = std::clamp((m_playhead - clip->pos) / overlap, 0.0, 1.0);
        m_transType = isTransition(under->transitionType)
                          ? under->transitionType
                          : QStringLiteral("dissolve");
        const double rel = m_playhead - under->pos;
        m_underCropL = (int)std::lround(
            std::clamp(kfValue(under->kfCropL, under->cropL, rel), 0.0, 0.9) * 1000.0);
        m_underCropR = (int)std::lround(
            std::clamp(kfValue(under->kfCropR, under->cropR, rel), 0.0, 0.9) * 1000.0);
        m_underCropT = (int)std::lround(
            std::clamp(kfValue(under->kfCropT, under->cropT, rel), 0.0, 0.9) * 1000.0);
        m_underCropB = (int)std::lround(
            std::clamp(kfValue(under->kfCropB, under->cropB, rel), 0.0, 0.9) * 1000.0);
        const MediaItem* um = m_project->findMedia(under->mediaId);
        if (um && um->hasVideo && m_frameWorker) {
            const QString uvpath = resolvePreviewVideo(um->filePath);
            const double uSrcT = clipSrcTime(*under, m_playhead - under->pos);
            QMutexLocker l(&m_frameMutex);
            // Mesmo alvo (clipe/ largura / tempo) que o quadro atual.
            const bool sameTarget = m_underRequested && m_underPath == uvpath
                                    && m_underW == decW
                                    && std::fabs(m_underT - uSrcT)
                                       <= 0.5 / projFps(m_project);
            if (m_underPending) {
                // Já há um decode em voo: não emite outro. O resultado em
                // onPrefetchReady limpa o flag; se o playhead andou muito
                // (sameTarget falso), o próximo tick re-mira o alvo.
            } else if (sameTarget && !m_underFrame.isNull()) {
                // Quadro atual serve (até ~0.5 frame de atraso): nada a fazer.
            } else {
                // Numa transição longa o playhead avança a cada frame, então o
                // alvo (uSrcT) muda continuamente. Zerar o quadro aqui fazia o
                // clipe de trás desaparecer até o decode chegar → "piscada" no
                // meio do dissolve. Mantém o quadro anterior na tela enquanto o
                // novo é decodificado; só descarta quando o alvo muda de clipe
                // (path ou largura), aí o quadro antigo não serve mais.
                if (m_underPath != uvpath || m_underW != decW)
                    m_underFrame = QImage();
                m_underPath = uvpath;
                m_underT = uSrcT;
                m_underW = decW;
                m_underCropL = m_underCropR = m_underCropT = m_underCropB = 0; // recortado no onFrame
                m_underRequested = true;
                m_underPending = true;
                QMetaObject::invokeMethod(m_frameWorker, "decodePrefetch", Qt::QueuedConnection,
                                          Q_ARG(QString, uvpath),
                                          Q_ARG(double, uSrcT),
                                          Q_ARG(int, decW),
                                          Q_ARG(double, 1.0 / projFps(m_project)));
            }
        }
    } else {
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
        m_underPending = false;
    }

    // Snapshot do caminho rápido: crop + efeitos básicos + máscaras + MotiOn/
    // OFX/frei0r vão para a thread de vídeo (FrameWorker), tirando a cadeia
    // inteira per-pixel do tiquinho da UI. Só LAINKA desliga o fast path
    // (onion skin/motion blur dependem do quadro anterior da UI).
    // O snapshot usa os MESMOS valores que applyCrop() usaria (crop avaliado
    // em m_playhead e máscaras em srcT).
    FrameFx topFx;
    topFx.active = !m_clipLainkaEnabled;
    topFx.cropL = m_lastCropL;
    topFx.cropR = m_lastCropR;
    topFx.cropT = m_lastCropT;
    topFx.cropB = m_lastCropB;
    topFx.rel = srcT;
    topFx.absTime = m_playhead;
    if (topFx.active) topFx.clip = *clip;

    // Path efetivo de vídeo do clipe do topo: proxy se houver (consistente com
    // requestFrame/onFrameReady e o cache).
    const QString vpath = resolvePreviewVideo(m->filePath);
    if (PreviewProfiler::active()) {
        // O que o A/B proxy-vs-original precisa saber: de onde saiu o quadro
        // que vai aparecer na tela (o clipe do topo), não quantas camadas
        // inferiores resolveram para proxy.
        profFrame().proxy = (vpath != m->filePath);
        profFrame().cut = (m_profTopClipId != m->id);
        m_profTopClipId = m->id;
    }

    // Se temos um quadro pré-carregado que bate com a posição de entrada do novo clipe, exibe imediatamente.
    bool usedPrefetch = false;
    if (m_transAlpha < 0.0) {
        QMutexLocker l(&m_frameMutex);
        const double frameDur = 1.0 / projFps(m_project);
        if (m_prefetch.valid && m_prefetch.path == vpath
            && std::fabs(m_prefetch.t - srcT) <= frameDur * 0.5
            && m_prefetch.maxW == decW) {
            m_frameFull = m_prefetch.img;
            m_shownPath = vpath;
            m_shownT = srcT;
            m_shownW = decW;
            m_shownFx = false; // quadro cru do prefetch; crop é aplicado na UI
            m_lastSrcT = srcT;
            m_lastDecodeW = decW;
            m_lastFile = vpath;
            m_prefetch.valid = false;
            m_prefetch.requested = false;
            usedPrefetch = true;
            applyCrop();
            update();
        }
    }

    // O quadro do tamanho/posição atuais já está pronto? Apenas reaplica o
    // pan/crop (por exemplo quando só o corte mudou) sem decodificar de novo.
    bool sameShown = false;
    bool shownFx = false;
    {
        QMutexLocker l(&m_frameMutex);
        sameShown = (!usedPrefetch && m_shownPath == vpath && std::fabs(m_shownT - srcT) < 1e-6
                     && m_shownW == decW);
        shownFx = m_shownFx;
    }
    if (sameShown) {
        if (shownFx) {
            // Veio pronto da thread do worker (já cortado/efeituado): aplicar
            // applyCrop() por cima duplicaria o corte. Re-pede o quadro com o
            // snapshot atual — o worker reaplica a cadeia.
            requestFrame(clip->id, vpath, srcT, decW, topFx);
        } else {
            applyCrop();
            update();
        }
        return;
    }
    if (usedPrefetch) {
        if (PreviewProfiler::active()) profFrame().prefetchHit = true;
        // Já no tick do corte, dispara o próximo frame: o decoder trocado está
        // posicionado e decodifica adiante, evitando "segurar" o frame do corte.
        requestFrame(clip->id, vpath, srcT + 1.0 / projFps(m_project), decW, topFx);
        return;
    }
    if (PreviewProfiler::active()) profFrame().prefetchHit = false;
    requestFrame(clip->id, vpath, srcT, decW, topFx);
}

// Pedido "assíncrono": a decodificação acontece na thread do FrameWorker.
// Vários pedidos seguidos entram numa fila; apenas um roda por vez
// (scrub não empilha — a fila só guarda pedidos ainda não enviados).
void PreviewWidget::requestFrame(const QString& clipId, const QString& path, double t, int maxW,
                                 const FrameFx& fx) {
    QMutexLocker l(&m_frameMutex);
    // Fase 3: se um decode já está em andamento e este pedido do TOPO o supera
    // (cortou para outro clipe, ou scrub/seek de mais de 3 frames parado),
    // cancela o decode obsoleto — o worker larga o alvo antigo e pega o novo já.
    // Durante reprodução no MESMO clipe NÃO cancela: o avanço de 1 frame por
    // tick completaria mesmo; cancelar a cada tick travaria o preview.
    if (m_hasInflightReq && m_frameWorker && m_project) {
        // PIERROT_NO_CANCEL=1 desliga (A/B: medir o efeito do cancelamento).
        static const bool kCancelOn = [] {
            return !qEnvironmentVariableIsSet("PIERROT_NO_CANCEL");
        }();
        const Clip* top = clipAt(m_playhead);
        if (kCancelOn && top && top->id == clipId) {
            bool cancel = (m_inflightReq.clipId != clipId);
            if (!cancel && !m_playing) {
                const double fd = projFps(m_project) > 0.0 ? 1.0 / projFps(m_project) : 1.0 / 30.0;
                cancel = std::fabs(m_inflightReq.t - t) > fd * 3.0;
            }
            if (cancel) m_frameWorker->requestCancel();
        }
    }
    // Coalesce: durante o scrub (e na reprodução) chegam vários alvos para o
    // mesmo clipe; só o mais recente interessa — o worker é serial e decodificar
    // posições intermediárias só atrasa a chegada ao alvo final.
    for (FrameReq& r : m_reqQueue)
        if (r.clipId == clipId && r.path == path && r.maxW == maxW) {
            r.fx = fx;
            if (std::fabs(r.t - t) < 1e-6) return; // já enfileirado igual
            r.t = t;
            r.dt = 1.0 / projFps(m_project);
            return;
        }
    m_reqQueue.append({clipId, path, t, 1.0 / projFps(m_project), maxW, fx});
    kickFrameWorker();
}

// Crop (pan/crop) do clipe no instante `rel` da timeline (definição abaixo).
static void clipCrop(const Clip& c, double rel, int& cL, int& cR, int& cT, int& cB);

// Pedidos os quadros dos clipes de vídeo ativos no playhead que ficam POR
// BAIXO do clipe do topo (empilhamento de faixas). Quadros já em cache
// (mesmo arquivo/tempo/tamanho) são pulados.
void PreviewWidget::requestLowerLayers(int decW) {
    if (!m_project) return;
    const Clip* top = clipAt(m_playhead);
    // Quantos decodes de camada inferior são pedidos por frame — candidato nº1
    // a roubar slots do worker que deveria estar decodificando o clipe do topo
    // (vídeo que engasga com áudio perfeito = o quadro do topo chega tarde).
    int dbgLayerDecodes = 0;
    // Camadas que deixaram de estar ativas no playhead saem do cache (evita
    // crescimento sem limite conforme o usuário navega pelo projeto).
    {
        QList<QString> active;
        for (const Track& tr : m_project->videoTracks) {
            if (!tr.visible) continue;
            for (const Clip& cl : tr.clips)
                if (!cl.isText && m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur)
                    active.append(cl.id);
        }
        QMutexLocker l(&m_frameMutex);
        for (auto it = m_layerCache.begin(); it != m_layerCache.end();) {
            if (!active.contains(it.key())) it = m_layerCache.erase(it);
            else ++it;
        }
    }
    for (const Track& tr : m_project->videoTracks) {
        if (!tr.visible) continue;   // faixa oculta (olho)
        const Clip* c = nullptr;
        for (const Clip& cl : tr.clips) {
            if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                const MediaItem* mm = m_project->findMedia(cl.mediaIdAt(m_playhead - cl.pos));
                if (mm && mm->hasVideo && (!c || cl.pos > c->pos)) c = &cl;
            }
        }
        if (!c || (top && c->id == top->id)) continue;
        const MediaItem* m = m_project->findMedia(c->mediaIdAt(m_playhead - c->pos));
        if (!m || !m->hasVideo) continue;
        // Cor sólida não tem arquivo: é gerada na pintura, não pede decode.
        if (m->isSolid) continue;
        // Malha 3D também não é mídia decodificável: um .obj não vai pelo
        // FFmpeg. A camada é rasterizada por `MesaRenderer::prepareLayer`,
        // no loop de tracks Mesa logo abaixo.
        if (m->isMesh) continue;
        const double srcT = clipSrcTime(*c, m_playhead - c->pos);
        const QString vpath = resolvePreviewVideo(m->filePath);
        bool pending = false;
        {
            QMutexLocker l(&m_frameMutex);
            const auto it = m_layerCache.constFind(c->id);
            if (it != m_layerCache.constEnd() && it->path == vpath
                && std::fabs(it->t - srcT) < 3.0 / projFps(m_project)
                && it->maxW == decW && !it->img.isNull())
                continue; // já em cache (3 frames: evita re-decode de background a cada frame)
            // Já em voo (worker ou fila)? Passa adiante — senão a camada de
            // cima, que fica "expirada" até o decode terminar, monopolizava o
            // loop e as camadas 3+ nunca eram pedidas (frame velho/ausente).
            if ((m_hasInflightReq && m_inflightReq.clipId == c->id)
                || (m_hasLayerInflight && m_layerInflight.clipId == c->id)) {
                pending = true; // em decode: não dá pra re-mirar, só esperar
            } else {
                for (const FrameReq& r : m_reqQueue)
                    if (r.clipId == c->id) { pending = true; break; }
            }
        }
        if (pending) continue; // em voo/fila: não conta nem segura as demais
        requestFrame(c->id, vpath, srcT, decW, [&] {
            // Camada inferior sem LAINKA: crop + efeitos básicos + MotiOn/OFX/
            // frei0r vão para a thread do worker (era o H6: per-pixel na UI a
            // cada quadro; MotiOn/OFX/frei0r das inferiores nem sequer eram
            // aplicados — lacuna de paridade com a exportação).
            FrameFx fx;
            fx.active = !c->lainkaEnabled;
            if (fx.active) {
                const double rel = m_playhead - c->pos;
                clipCrop(*c, rel, fx.cropL, fx.cropR, fx.cropT, fx.cropB);
                fx.rel = rel;
                fx.absTime = m_playhead;
                fx.clip = *c;
            }
            return fx;
        }());
        ++dbgLayerDecodes;
        // Teto de decodes NOVAS por tick: prioriza o clipe do topo sem afogar
        // o worker, mas cobre 2 camadas novas — com 1 por tick e 3+ faixas
        // empilhadas, o pedido em voo re-expirava a cada tick e as camadas
        // fundas nunca alcançavam.
        if (dbgLayerDecodes >= 2) break;
    }

    // Tracks Mesa: decodifica o frame individual de cada track via MesaRenderer
    // e armazena no layerCache, para que paintEvent possa compô-las por baixo
    // da composição Mesa do topo (transparência, blend entre faixas).
    for (const Track& tr : m_project->videoTracks) {
        if (!tr.visible) continue;   // faixa oculta (olho)
        const MesaComposition* mc = m_project->findMesaForTrack(tr.id);
        if (!mc) continue;
        const Clip* c = nullptr;
        for (const Clip& cl : tr.clips) {
            if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                if (!c || cl.pos > c->pos) c = &cl;
            }
        }
        if (!c || (top && c->id == top->id)) continue;
        // Cache hit: já renderizado nesta frame.
        {
            QMutexLocker l(&m_frameMutex);
            if (m_layerCache.contains(c->id)) continue;
        }
        MesaRenderer::LayerPrep prep;
        if (m_mesaRenderer.prepareLayer(prep, *mc, *m_project, m_playhead, tr) && prep.valid) {
            QMutexLocker l(&m_frameMutex);
            m_layerCache[c->id] = {prep.frame,
                                   QStringLiteral("mesa:") + tr.id,
                                   m_playhead, decW};
        }
    }
    // Log de diagnóstico: quantos decodes inferiores dispararam este frame — se
    // houver, o worker está dividindo atenção entre o clipe do topo e N camadas
    // novas ao mesmo tempo (candidato nº1 ao engasgo pós-20s em projetos com
    // múltiplas faixas).
    if (playDbg() && dbgLayerDecodes > 0)
        qDebug().noquote() << QStringLiteral("[play] layers decode=%1 t=%2 top=%3 queue=%4")
                  .arg(dbgLayerDecodes)
                  .arg(m_playhead, 0, 'f', 3)
                  .arg(top ? top->id.mid(0,8) : QString())
                  .arg(m_reqQueue.size());
    if (PreviewProfiler::active()) {
        profFrame().lowerReq = dbgLayerDecodes;
        profFrame().queue = m_reqQueue.size();
    }
}

// Chamado com m_frameMutex segurado. Despacha para DOIS workers (H3): o do
// topo (prioridade absoluta — é o quadro exibido) e o dedicado de camadas
// inferiores (roda em paralelo; antes tudo serializava num worker só).
void PreviewWidget::kickFrameWorker() {
    if (m_reqQueue.isEmpty() || !m_frameWorker) return;

    // Podas pedidos órfãos: na virada de um corte, o pedido residual do clipe
    // antigo não é mais "vivo" — decodificá-lo gastava um slot inteiro e
    // atrasava o 1º frame do clipe novo (o "tiquinho" de travada). Na fila só
    // interessam clipes que contêm o playhead agora (topo ou camadas ativas).
    if (m_project) {
        for (int i = m_reqQueue.size() - 1; i >= 0; --i) {
            const QString& cid = m_reqQueue[i].clipId;
            bool live = false;
            for (const Track& tr : m_project->videoTracks)
                for (const Clip& c : tr.clips)
                    if (c.id == cid && !c.isText
                        && m_playhead >= c.pos - 1e-6 && m_playhead < c.pos + c.dur + 1e-6) {
                        live = true;
                        break;
                    }
            if (!live) m_reqQueue.removeAt(i);
        }
        if (m_reqQueue.isEmpty()) return;
    }

    const Clip* top = clipAt(m_playhead);

    // 1) TOPO → worker principal (prioridade absoluta: é o quadro exibido).
    if (!m_workerBusy && top) {
        for (int i = 0; i < m_reqQueue.size(); ++i) {
            if (m_reqQueue[i].clipId != top->id) continue;
            const FrameReq r = m_reqQueue.takeAt(i);
            m_workerBusy = true;
            m_inflightReq = r;
            m_hasInflightReq = true;
            if (m_perfT.isValid()) m_perfWorkerStartNs = m_perfT.nsecsElapsed();
            QMetaObject::invokeMethod(m_frameWorker, "decodeOne", Qt::QueuedConnection,
                                      Q_ARG(QString, r.clipId), Q_ARG(QString, r.path),
                                      Q_ARG(double, r.t), Q_ARG(int, r.maxW),
                                      Q_ARG(double, r.dt), Q_ARG(FrameFx, r.fx));
            break;
        }
    }

    // 2) Camadas inferiores → worker DEDICADO (H3), em paralelo com o topo.
    if (!m_layerBusy && m_layerWorker && !m_reqQueue.isEmpty()) {
        const FrameReq r = m_reqQueue.takeFirst();
        m_layerBusy = true;
        m_layerInflight = r;
        m_hasLayerInflight = true;
        QMetaObject::invokeMethod(m_layerWorker, "decodeOne", Qt::QueuedConnection,
                                  Q_ARG(QString, r.clipId), Q_ARG(QString, r.path),
                                  Q_ARG(double, r.t), Q_ARG(int, r.maxW),
                                  Q_ARG(double, r.dt), Q_ARG(FrameFx, r.fx));
    }
}

// Crop (pan/crop) do clipe no instante `rel` da timeline.
static void clipCrop(const Clip& c, double rel, int& cL, int& cR, int& cT, int& cB) {
    cL = (int)std::lround(std::clamp(kfValue(c.kfCropL, c.cropL, rel), 0.0, 0.9) * 1000.0);
    cR = (int)std::lround(std::clamp(kfValue(c.kfCropR, c.cropR, rel), 0.0, 0.9) * 1000.0);
    cT = (int)std::lround(std::clamp(kfValue(c.kfCropT, c.cropT, rel), 0.0, 0.9) * 1000.0);
    cB = (int)std::lround(std::clamp(kfValue(c.kfCropB, c.cropB, rel), 0.0, 0.9) * 1000.0);
}

void PreviewWidget::onFrameReady(const QString& clipId, const QString& path, double t, int maxW,
                                 const QImage& img, bool processed) {
    handleFrameReady(clipId, path, t, maxW, img, processed, /*fromLayer=*/false);
}

void PreviewWidget::handleFrameReady(const QString& clipId, const QString& path, double t, int maxW,
                                     const QImage& img, bool processed, bool fromLayer) {
    // Latência do decode SÓ do worker do topo: o m_perfWorkerStartNs é o
    // dispatch do último quadro do topo — se um resultado de CAMADA chegasse
    // segundos depois, a subtração gerava latência falsa de 1 s (poluía o
    // p99 do profiler; o H3 separou os workers, separe a métrica também).
    const qint64 workerLatNs =
        (!fromLayer && PreviewProfiler::active() && m_perfT.isValid() && m_perfWorkerStartNs > 0)
            ? m_perfT.nsecsElapsed() - m_perfWorkerStartNs
            : 0;
    if (PreviewProfiler::active() && !fromLayer) {
        profFrame().workerNs = workerLatNs; // latência do decode: dispatch -> quadro pronto
        profFrame().cacheHits = FrameCache::hits();
        profFrame().cacheMisses = FrameCache::misses();
        profFrame().decSeekNs = qint64(FFmpegDecoder::lastSeekNs());
        profFrame().decDiscard = FFmpegDecoder::lastDiscard();
        profFrame().decWorkNs = FFmpegDecoder::lastWorkNs();
    }
    // Latência "apertar play -> 1º quadro na tela" (só diagnostica; com o
    // warmPlayhead no playhead parado deve cair de centenas de ms para dezenas).
    if (m_playStartNs > 0 && !img.isNull()) {
        const double dtMs = (PreviewProfiler::nowNs() - m_playStartNs) / 1e6;
        if (playDbg())
            qDebug().noquote()
                << QStringLiteral("[play] 1º quadro %1ms após o play").arg(dtMs, 0, 'f', 0);
        m_playStartNs = 0; // mede só o primeiro quadro de cada play
    }
    {
        QMutexLocker l(&m_frameMutex);
        if (fromLayer) {
            // Resultado do worker DEDICADO de camadas (H3): só o estado dele
            // zera; o worker principal segue com o quadro do topo.
            m_layerBusy = false;
            m_hasLayerInflight = false;
        } else {
            m_workerBusy = false;
            m_hasInflightReq = false;
            m_perfWorkerStartNs = 0;
            if (workerLatNs > 0) m_perf.workerMs = workerLatNs / 1000000;
        }
        kickFrameWorker(); // continua com o próximo pedido, se houver
    }

    // ── Adaptive quality: monitora latência do decode ──────────────
    if (m_playing && m_perf.workerMs > 0 && m_project && m_project->fps > 0.0) {
        const double frameMs = 1000.0 / m_project->fps;
        // Média móvel exponencial (suaviza spikes).
        m_adaptiveDecodeMsAvg = m_adaptiveDecodeMsAvg * 0.8 + m_perf.workerMs * 0.2;
        if (m_adaptiveDecodeMsAvg > frameMs * 1.2) {
            ++m_adaptiveSlowCount;
        } else {
            m_adaptiveSlowCount = std::max(0, m_adaptiveSlowCount - 1);
        }
        // 5 ticks consecutivos lentos → baixa resolução.
        if (m_adaptiveSlowCount >= 5 && !m_adaptiveActive) {
            m_adaptiveBaseQuality = m_previewQuality;
            const int levels[] = {3840, 1080, 720, 480, 360};
            for (int i = 0; i < 5; ++i) {
                if (m_previewQuality >= levels[i] && levels[i] < m_previewQuality) {
                    setPreviewQuality(levels[i]);
                    m_adaptiveActive = true;
                    break;
                }
            }
        }
        // Recupera quando a latência normaliza por 30 ticks.
        if (m_adaptiveActive && m_adaptiveDecodeMsAvg < frameMs * 0.7) {
            m_adaptiveSlowCount -= 2;
            if (m_adaptiveSlowCount <= 0) {
                setPreviewQuality(m_adaptiveBaseQuality);
                m_adaptiveActive = false;
                m_adaptiveSlowCount = 0;
            }
        }
        // Acumula dropped frames para o overlay.
        m_perf.droppedTotal += consumeDroppedFrames(); // só o overlay do live
    }

    if (img.isNull() || !m_project) return;

    const Clip* clip = nullptr;
    for (const Track& tr : m_project->videoTracks)
        for (const Clip& c : tr.clips)
            if (c.id == clipId) { clip = &c; break; }
    if (!clip) return;
    // Multicam: a mídia ativa é a do ângulo no playhead (mediaIdAt), não a base
    // (mediaId). Validar pela base descartava todo frame correto do ângulo ativo
    // e travava o monitor no conteúdo do clipe anterior ("mostra outro clipe").
    const MediaItem* m = m_project->findMedia(clip->mediaIdAt(m_playhead - clip->pos));
    if (!m) return;
    if (resolvePreviewVideo(m->filePath) != path) return;
    if (PreviewProfiler::active())
        profFrame().proxy = (path != m->filePath); // o quadro exibido veio de proxy?

    // Ignora quadros decodificados para outra posição (scrub/seek rápido muito
    // distante). Durante a REPRODUÇÃO exige proximidade de poucos quadros: antes
    // aceitava qualquer frame a até 1,5 s, o que fazia a tela exibir um trecho
    // errado do clipe (parecia "outro clipe") enquanto o áudio ia no lugar certo.
    const double wantT = clipSrcTime(*clip, m_playhead - clip->pos);
    const double tol = m_playing ? (3.0 / std::max(1.0, projFps(m_project))) : 1.5;
    if (std::fabs(wantT - t) > tol) return;

    // Quantum a que o playhead (já cruzou o frame) está à frente do quadro que
    // acabou de decodificar. >1 frame em reprodução = o vídeo está atrasado e
    // vai ENGASGAR (re-exibir) mesmo com o áudio perfeito.
    if (playDbg() && m_playing && std::fabs(t - wantT) <= tol) {
        const double aheadFrames = (wantT - t) * projFps(m_project);
        if (std::fabs(aheadFrames) > 0.5)
            qDebug().noquote() << QStringLiteral("[play] frame atraso %1f (dec %2s playhead %3s) lat %4ms q %5")
                      .arg(aheadFrames, 4, 'f', 1)
                      .arg(t, 0, 'f', 3)
                      .arg(wantT, 0, 'f', 3)
                      .arg(static_cast<double>(m_perf.workerMs), 0, 'f', 0)
                      .arg(maxW);
    }

    // Quadro do clipe do TOPO: caminho atual do preview (com prefetch/pan-crop).
    const Clip* top = clipAt(m_playhead);
    if (composeDbg())
        qDebug() << "[compose] frameReady clipId=" << clipId
                 << "path=" << path << "t=" << t
                 << "top=" << (top ? top->id : QString())
                 << "isTop=" << (top && top->id == clipId);
    if (top && top->id == clipId) {
        {
            QMutexLocker l(&m_frameMutex);
            m_shownPath = path;
            m_shownT = t;
            m_shownW = maxW;
            m_shownFx = processed;   // m_frameFull já chegou cortado/efeituado
        }
        m_frameFull = img;
        m_lastSrcT = t;
        m_lastDecodeW = maxW;
        m_lastFile = path;
        if (processed) {
            // Camada rápida: crop + efeitos básicos/máscaras foram aplicados na
            // thread de vídeo — aplicar applyCrop() aqui duplicaria o pipeline.
            m_frame = img;
        } else {
            applyCrop();
        }
        update();
        return;
    }

    // Quadro de uma camada INFERIOR: guarda no cache de camadas (cortado + LAINKA).
    QImage cropped;
    if (processed) {
        // Já veio cortado + com efeitos básicos/máscaras da thread do worker.
        cropped = img;
    } else {
        const double rel = m_playhead - clip->pos;
        int cL, cR, cT, cB;
        clipCrop(*clip, rel, cL, cR, cT, cB);
        cropped = applyCropTo(img, cL, cR, cT, cB);
        // Aplica LAINKA também em camadas inferiores.
        if (clip->lainkaEnabled) {
            double srcT = clipSrcTime(*clip, m_playhead - clip->pos);
            const double pfps = projFps(m_project);
            const int effSkip = (clip->lainkaTargetFps > 0 && pfps > 1.0)
                ? std::max(1, (int)std::lround(pfps / clip->lainkaTargetFps))
                : clip->lainkaSkip;
            if (effSkip > 1)
                srcT = lainkaQuantizeTime(srcT, effSkip, pfps);
            cropped = lainkaApplyFx(cropped, clip->id, srcT,
                                    clip->lainkaSkip, clip->lainkaJitterPos,
                                    clip->lainkaJitterRot, clip->lainkaJitterScale,
                                    clip->lainkaFlicker, clip->lainkaFlickerSpeed,
                                    clip->lainkaWarpAmount, clip->lainkaWarpSpeed,
                                    clip->lainkaWarpGrid, clip->lainkaOnionSkin,
                                    clip->lainkaDustAmount, clip->lainkaScratchAmount,
                                    clip->lainkaMotionBlur, clip->lainkaOpacity,
                                    clip->lainkaTargetFps, clip->lainkaAntialias,
                                    QImage());
        }
        // Aplica efeitos básicos também em camadas inferiores.
        applyBasicEffectsOn(cropped, *clip, m_playhead - clip->pos);
        // MotiOn/OFX/frei0r das camadas inferiores (paridade com a exportação —
        // antes só o topo recebia; com LAINKA o worker não pôde aplicar).
        applyTemporalFxOn(cropped, *clip, m_playhead, m_playhead - clip->pos,
                          m_ofxManager, m_frei0rManager);
    }
    {
        QMutexLocker l(&m_frameMutex);
        m_layerCache[clipId] = {cropped, path, t, maxW};
    }
    update();
}

void PreviewWidget::onPrefetchReady(const QString& path, double t, int maxW, const QImage& img) {
    const qint64 latNs =
        (PreviewProfiler::active() && m_perfT.isValid() && m_perfPrefetchStartNs > 0)
            ? m_perfT.nsecsElapsed() - m_perfPrefetchStartNs : 0;
    if (PreviewProfiler::active()) profFrame().prefetchLatNs = latNs;
    QMutexLocker l(&m_frameMutex);
    if (latNs > 0) m_perf.prefetchLatMs = latNs / 1000000;
    m_perfPrefetchStartNs = 0;
    // Quadro do clipe de trás (transição ativa). Resposta do pedido em voo:
    // limpa o flag mesmo se for descartada (largura/tempo divergente), para o
    // próximo tick poder re-mirar o alvo.
    if (m_underRequested && m_underPath == path) {
        m_underPending = false;
        if (std::fabs(m_underT - t) < 1e-4 && m_underW == maxW)
            m_underFrame = img.isNull() ? QImage() : applyCropTo(img, m_underCropL,
                                                                  m_underCropR,
                                                                  m_underCropT,
                                                                  m_underCropB);
    }
    if (m_prefetch.requested && m_prefetch.path == path
        && std::fabs(m_prefetch.t - t) < 1e-4 && m_prefetch.maxW == maxW) {
        m_prefetch.img = img;
        m_prefetch.valid = !img.isNull();
        m_prefetch.invoked = false; // pronto (ou falhou): permite novo ciclo
        if (img.isNull()) m_prefetch.requested = false; // falhou: encerra o ciclo
    }
    // O prefetch liberou o caminho — retoma os decodeOne's que estavam esperando.
    m_workerBusy = false;
    kickFrameWorker();
}

void PreviewWidget::onBgPrefetchDone(const QString& path, double t, int maxW,
                                     const QImage& frame0, const QImage& frame1) {
    Q_UNUSED(frame1);
    const qint64 latNs =
        (PreviewProfiler::active() && m_perfT.isValid() && m_perfPrefetchStartNs > 0)
            ? m_perfT.nsecsElapsed() - m_perfPrefetchStartNs : 0;
    if (PreviewProfiler::active()) profFrame().prefetchLatNs = latNs;
    {
        QMutexLocker l(&m_frameMutex);
        if (latNs > 0) m_perf.prefetchLatMs = latNs / 1000000;
        m_perfPrefetchStartNs = 0;
        if (m_prefetch.requested && m_prefetch.path == path && m_prefetch.maxW == maxW) {
            m_prefetch.img = frame0;
            m_prefetch.valid = !frame0.isNull();
            m_prefetch.invoked = false;
            if (frame0.isNull()) m_prefetch.requested = false;
        }
    }
    // O decoder NÃO vem junto: o prefetch só aqueceu o pool (a entrada segue
    // quente). No corte, o FrameWorker pega o mesmo decoder já quente na
    // posição do próximo clipe — sem troca de posse, sem fila.
}

void PreviewWidget::onBgPrefetchFailed(const QString& path) {
    QMutexLocker l(&m_frameMutex);
    if (m_prefetch.requested && m_prefetch.path == path) {
        m_prefetch.valid = false;
        m_prefetch.requested = false;
    }
}

// Próximo clipe que começa depois do fim de `c` (varredura igual à do
// updatePrefetch: próximo início de clipe na timeline, preferindo faixa de cima).
static const Clip* clipAfter(const Project* p, const Clip* c) {
    if (!p || !c) return nullptr;
    const double start = c->pos + c->dur;
    const Clip* found = nullptr;
    double nextPos = 1e9;
    for (int tr = (int)p->videoTracks.size() - 1; tr >= 0; --tr) {
        for (const Clip& cc : p->videoTracks[tr].clips) {
            if (cc.pos >= start - 1e-4 && cc.pos < nextPos) {
                nextPos = cc.pos;
                found = &cc;
            }
        }
    }
    return found;
}

void PreviewWidget::updatePrefetch() {
    if (!m_project || !m_frameWorker || !m_playing) return;
    // Durante uma transição o canal de prefetch decodifica o clipe de trás.
    if (m_transAlpha >= 0.0) return;
    const Clip* clip = clipAt(m_playhead);
    if (!clip) {
        QMutexLocker l(&m_frameMutex);
        m_prefetch.valid = false;
        m_prefetch.requested = false;
        return;
    }
    const double remain = (clip->pos + clip->dur) - m_playhead;
    // Janela dinâmica: 5s para clipes longos, no mínimo 2s para curtos.
    const double window = qMin(5.0, qMax(2.0, clip->dur * 0.8));
    if (remain > window || remain <= 0.0) return;

    // Procura o próximo clipe que será exibido no fim do clipe atual
    const Clip* nextClip = nullptr;
    double nextPos = 1e9;
    for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
        for (const Clip& c : m_project->videoTracks[tr].clips) {
            if (c.pos >= clip->pos + clip->dur - 1e-4 && c.pos < nextPos) {
                nextPos = c.pos;
                nextClip = &c;
            }
        }
    }
    if (!nextClip) return;

    const MediaItem* nextMedia = m_project->findMedia(nextClip->mediaId);
    if (!nextMedia || !nextMedia->hasVideo) return;

    const double srcT = nextClip->in;
    // Prefetch no mesmo tamanho do quadro com qualidade (nunca abaixo da tela).
    const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
    const int decW = qMax(160, qMax(m_previewQuality, widgetW));
    const QString vpath = resolvePreviewVideo(nextMedia->filePath);

    // ── Frame0 do próximo clipe (crítico: o corte mais próximo) ──────────
    // Decide ANTES do warm-ahead e despacha logo, para que decode(N1) seja o
    // primeiro da fila da thread bg (nunca atrasado pelo warm do 2º clipe).
    bool needFrame0 = false;
    {
        QMutexLocker l(&m_frameMutex);
        if (m_perfT.isValid()) m_perfPrefetchStartNs = m_perfT.nsecsElapsed();
        if (m_prefetch.requested && m_prefetch.path == vpath
            && std::fabs(m_prefetch.t - srcT) < 1e-4 && m_prefetch.maxW == decW) {
            needFrame0 = false; // já solicitado ou já pronto
        } else {
            m_prefetch.path = vpath;
            m_prefetch.t = srcT;
            m_prefetch.maxW = decW;
            m_prefetch.img = QImage();
            m_prefetch.valid = false;
            m_prefetch.requested = true;
            m_prefetch.invoked = true; // despachado agora (re-despachado se o corte chegar sem terminar)
            m_prefetch.clipEnd = clip->pos + clip->dur;
            needFrame0 = true;
        }
    }
    if (needFrame0 && m_pipeline) {
        m_pipeline->prefetchFrame(vpath, srcT, decW, 1.0 / projFps(m_project));
    }

    // ── Warm-ahead (L4): 2º/3º clipe à frente, só abre+posiciona ─────
    // A UI resolve QUAIS clipes/paths (é política de modelo, L6); o
    // FramePipeline cuida da fila, da serialização e do estado in-flight.
    QVector<FramePipeline::WarmTarget> wants;
    const Clip* ahead = nextClip;
    for (int i = 0; i < 2 && ahead; ++i) {
        ahead = clipAfter(m_project, ahead);
        if (!ahead) break;
        const MediaItem* am = m_project->findMedia(ahead->mediaId);
        if (!am || !am->hasVideo) continue;
        const QString apath = resolvePreviewVideo(am->filePath);
        if (apath.isEmpty() || apath == vpath) continue; // mesmo arquivo já coberto
        if (m_decPool->contains(apath) || m_pipeline->warmInFlight(apath)) continue;
        bool dup = false;
        for (const auto& w : wants)
            if (w.path == apath) { dup = true; break; }
        if (!dup) wants.push_back({apath, ahead->in, decW});
    }
    if (m_pipeline) m_pipeline->setWarmQueue(wants);
}

// Um alvo da fila de warm terminou. A fila e o despacho do próximo vivem no
// FramePipeline; aqui só se reagenda o idle-warm se a agulha estiver parada.
void PreviewWidget::onBgWarmDone(const QString& path) {
    Q_UNUSED(path); // a fila e o próximo despacho agora vivem no FramePipeline
}

// Agenda (debounce) o aquecimento do clipe sob a agulha. Só fora da reprodução:
// durante o play o prefetch e o warm-ahead já mantêm o pool quente. O timer de
// play ativo é a fonte da verdade do transporte (m_playing pode estar defasado).
void PreviewWidget::scheduleIdleWarm() {
    if (!m_idleWarmTimer) return;
    if (m_timer && m_timer->isActive()) return;
    m_idleWarmTimer->start();
}

// Aquece o clipe sob a agulha parada: abre+posiciona o decoder (vídeo) e
// começa a conformar a janela de áudio. Assim o próximo play não paga o open()
// frio (~500-800ms) nem o "começa mudo" do conform. Best-effort: se o decoder
// já está no pool (o FrameWorker decodificou o quadro da agulha), nada a fazer.
void PreviewWidget::warmPlayhead() {
    if (!m_project) return;
    if (m_timer && m_timer->isActive()) return;
    // Sink já criado no startup; revalida caso a saída padrão tenha mudado.
    prewarmAudioSink();
    const Clip* clip = clipAt(m_playhead);
    if (!clip) return;
    const MediaItem* media = m_project->findMedia(clip->mediaId);
    if (!media) return;

    const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
    const int decW = qMax(160, qMax(m_previewQuality, widgetW));

    // Vídeo: aquece na thread de prefetch (ociosa fora do play). O pipeline
    // ignora o pedido se já houver um warm em andamento — o quadro chega de
    // qualquer forma pelo FrameWorker.
    if (media->hasVideo && m_pipeline && m_decPool
        && !media->filePath.isEmpty()) {
        const QString vpath = resolvePreviewVideo(media->filePath);
        if (!vpath.isEmpty() && !m_decPool->contains(vpath)) {
            // Mesmo tempo-fonte que o quadro da agulha (respeita in/speed).
            const double srcT = clipSrcTime(*clip, m_playhead - clip->pos);
            m_pipeline->warmNow(vpath, srcT, decW);
        }
    }

    // Áudio: pede (async) a janela de conform do trecho sob a agulha. No play,
    // updateSources() pede a MESMA janela — já preenchida, o sink parte com o
    // conteúdo certo em vez de silêncio.
    const QVector<AudioMixer::SourceInfo> sources = buildMixSources(m_project, m_playhead);
    if (!sources.isEmpty()) {
        AudioConformCache& conform = AudioConformCache::instance();
        for (const AudioMixer::SourceInfo& w : sources) {
            if (w.path.isEmpty()) continue;
            const AudioConformCache::Ref c = conform.get(w.path, w.audioStream);
            conform.request(c, w.mediaPos, 6.0);
        }
    }
}

// Aplica pan/crop sobre um quadro e devolve o recorte.
QImage PreviewWidget::applyCropTo(const QImage& img, int cL, int cR, int cT, int cB) {
    const double w = img.width();
    const double h = img.height();
    if (w <= 1 || h <= 1 || (!cL && !cR && !cT && !cB)) return img;
    const int x = (int)std::lround(w * cL / 1000.0);
    const int y = (int)std::lround(h * cT / 1000.0);
    const int cw = (int)std::lround(w * (1.0 - (cL + cR) / 1000.0));
    const int ch = (int)std::lround(h * (1.0 - (cT + cB) / 1000.0));
    return img.copy(QRect(std::clamp(x, 0, (int)w - 1), std::clamp(y, 0, (int)h - 1),
                          qMin(cw, (int)w - std::clamp(x, 0, (int)w - 1)),
                          qMin(ch, (int)h - std::clamp(y, 0, (int)h - 1))));
}

// Aplica efeitos básicos (brilho, contraste, saturação, desfoque, preto e branco, chroma key)
// sobre o quadro do clipe ativo. Chamado após o crop em applyCrop().
void PreviewWidget::applyBasicEffects(QImage& img) {
    if (img.isNull()) return;
    Clip c;
    c.brightness = m_clipBrightness;
    c.contrast = m_clipContrast;
    c.saturation = m_clipSaturation;
    c.blur = m_clipBlur;
    c.grayscale = m_clipGrayscale;
    c.chromaKey = m_clipChromaKey;
    c.chromaKeyColor = m_clipChromaKeyColor;
    c.chromaKeySimilarity = m_clipChromaKeySimilarity;
    c.chromaKeySoftness = m_clipChromaKeySoftness;
    c.chromaKeySpillSuppress = m_clipChromaKeySpillSuppress;
    c.psxEnabled = m_clipPsxEnabled;
    c.psxDither = m_clipPsxDither;
    c.psxBits = m_clipPsxBits;
    c.filmGrain = m_clipFilmGrain;
    c.filmGrainAmount = m_clipFilmGrainAmount;
    c.filmGrainSize = m_clipFilmGrainSize;
    // Máscaras do clipe do topo: aplicadas na mesma ordem (início) e espaço
    // (quadro já cortado) que nas camadas inferiores — o topo também respeita
    // o recorte por forma. `m_lastSrcT` é o tempo relativo do clipe.
    c.masks = m_clipMasks;
    applyBasicEffectsOn(img, c, m_lastSrcT);
}

// Aplica as máscaras do clipe ao quadro: multiplica o alpha pela cobertura da
// forma (rect/ellipse), com feather (borda suave por distância assinada) e
// invert, combinadas em união. Avalia os keyframes em `rel` (tempo relativo).
// Warp de malha 3×3 (espremer/esticar a mídia). Os 9 pontos são as posições
// de DESTINO dos vértices da grade regular (2×2 células). Para cada pixel de
// destino inverte o mapeamento bilinear (Newton) e amostra o fonte.
static bool meshIsIdentity(const QVector<QPointF>& mesh, int cols, int rows) {
    const double eps = 1e-4;
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            const QPointF& p = mesh[cols * j + i];
            if (std::abs(p.x() - i / double(cols - 1)) > eps
                || std::abs(p.y() - j / double(rows - 1)) > eps)
                return false;
        }
    }
    return true;
}

static void warpByMesh(QImage& img, const QVector<QPointF>& mesh, int cols, int rows) {
    if (cols < 2 || rows < 2 || mesh.size() != cols * rows) return;
    const int w = img.width(), h = img.height();
    if (w < 2 || h < 2) return;
    // Grade regular (padrão) = identidade: pula o warp inteiro. Sem isso, todo
    // clipe com malha ativa rodava Newton por pixel sem alterar a imagem —
    // causava BLOQUEIO de segundos na UI e pressão de memória (4GB peak).
    if (meshIsIdentity(mesh, cols, rows)) return;
    const QImage src = img.convertToFormat(QImage::Format_ARGB32);
    if (src.isNull()) return;
    QImage dst(w, h, QImage::Format_ARGB32);
    if (dst.isNull()) return;
    dst.fill(Qt::transparent);

    auto S = [&](int i, int j) { // vértice regular (fonte) em px
        return QPointF(i * (w - 1) / (cols - 1.0), j * (h - 1) / (rows - 1.0));
    };
    auto D = [&](int i, int j) { // ponto de controle (destino) em px
        const QPointF& p = mesh[cols * j + i];
        return QPointF(std::clamp(p.x(), -0.5, 1.5) * (w - 1),
                       std::clamp(p.y(), -0.5, 1.5) * (h - 1));
    };
    auto bil = [](const QPointF& P00, const QPointF& P10, const QPointF& P11,
                  const QPointF& P01, double u, double v) {
        return (1 - v) * ((1 - u) * P00 + u * P10) + v * ((1 - u) * P01 + u * P11);
    };

    for (int cj = 0; cj < rows - 1; ++cj) {
        for (int ci = 0; ci < cols - 1; ++ci) {
            const QPointF P00 = D(ci, cj), P10 = D(ci + 1, cj);
            const QPointF P11 = D(ci + 1, cj + 1), P01 = D(ci, cj + 1);
            const double minX = std::min(std::min(P00.x(), P10.x()), std::min(P11.x(), P01.x()));
            const double maxX = std::max(std::max(P00.x(), P10.x()), std::max(P11.x(), P01.x()));
            const double minY = std::min(std::min(P00.y(), P10.y()), std::min(P11.y(), P01.y()));
            const double maxY = std::max(std::max(P00.y(), P10.y()), std::max(P11.y(), P01.y()));
            const int x0 = std::max(0, (int)std::floor(minX));
            const int x1 = std::min(w - 1, (int)std::ceil(maxX));
            const int y0 = std::max(0, (int)std::floor(minY));
            const int y1 = std::min(h - 1, (int)std::ceil(maxY));
            for (int y = y0; y <= y1; ++y) {
                QRgb* dline = reinterpret_cast<QRgb*>(dst.scanLine(y));
                for (int x = x0; x <= x1; ++x) {
                    const QPointF q(x, y);
                    double u = 0.5, v = 0.5;
                    for (int it = 0; it < 10; ++it) {
                        const QPointF cur = bil(P00, P10, P11, P01, u, v);
                        const QPointF e = cur - q;
                        if (std::abs(e.x()) < 0.3 && std::abs(e.y()) < 0.3) break;
                        const QPointF dPdu = (1 - v) * (P10 - P00) + v * (P11 - P01);
                        const QPointF dPdv = (1 - u) * (P01 - P00) + u * (P11 - P10);
                        const double det = dPdu.x() * dPdv.y() - dPdu.y() * dPdv.x();
                        if (std::abs(det) < 1e-9) break;
                        u += (-e.x() * dPdv.y() + e.y() * dPdv.x()) / det;
                        v += (-dPdu.x() * e.y() + dPdu.y() * e.x()) / det;
                        u = std::clamp(u, 0.0, 1.0);
                        v = std::clamp(v, 0.0, 1.0);
                    }
                    if (u < -0.001 || u > 1.001 || v < -0.001 || v > 1.001) continue;
                    const QPointF s = bil(S(ci, cj), S(ci + 1, cj),
                                          S(ci + 1, cj + 1), S(ci, cj + 1), u, v);
                    const int sx = std::clamp((int)std::lround(s.x()), 0, w - 1);
                    const int sy = std::clamp((int)std::lround(s.y()), 0, h - 1);
                    dline[x] = reinterpret_cast<const QRgb*>(src.constScanLine(sy))[sx];
                }
            }
        }
    }
    img = dst;
}

void PreviewWidget::applyMasks(QImage& img, const Clip& c, double rel) {
    const int w = img.width();
    const int h = img.height();
    if (w < 1 || h < 1) return;
    // A máscara modifica o canal alpha. Se a fonte não tem alpha por byte
    // (vídeo decodificado em RGB32), converte — antes retornava e a máscara
    // simplesmente não aparecia no preview.
    if (img.format() != QImage::Format_ARGB32
        && img.format() != QImage::Format_ARGB32_Premultiplied)
        img = img.convertToFormat(QImage::Format_ARGB32);

    std::vector<float> cov((size_t)w * (size_t)h, 0.0f);
    for (const Mask& m : c.masks) {
        if (!m.hasMask()) continue;
        if (m.type != QLatin1String("rect") && m.type != QLatin1String("ellipse"))
            continue; // poly: v1 não exporta ainda — ignorar p/ manter paridade
        const double cx = m.cxAt(rel) * w;
        const double cy = m.cyAt(rel) * h;
        const double rx = std::max(0.001, m.rxAt(rel) * w);
        const double ry = std::max(0.001, m.ryAt(rel) * h);
        const double rotRad = m.rotAt(rel) * M_PI / 180.0;
        const double cosR = std::cos(-rotRad);
        const double sinR = std::sin(-rotRad);
        const double fp = m.featherAt(rel) * (double)std::max(w, h);
        const bool invert = m.invert;
        const bool ellipse = (m.type == QLatin1String("ellipse"));

        for (int y = 0; y < h; ++y) {
            const double ly0 = y - cy;
            size_t idx = (size_t)y * w;
            for (int x = 0; x < w; ++x, ++idx) {
                const double lx0 = x - cx;
                const double lx = lx0 * cosR - ly0 * sinR;
                const double ly = lx0 * sinR + ly0 * cosR;
                double d;
                if (ellipse) {
                    const double n = (lx * lx) / (rx * rx) + (ly * ly) / (ry * ry);
                    d = (std::sqrt(std::max(0.0, n)) - 1.0) * std::min(rx, ry);
                } else {
                    d = std::max(std::abs(lx) - rx, std::abs(ly) - ry);
                }
                double v = (fp > 0.5) ? std::clamp(0.5 - d / fp, 0.0, 1.0)
                                      : ((d < 0.0) ? 1.0 : 0.0);
                if (invert) v = 1.0 - v;
                if (v > cov[idx]) cov[idx] = (float)v;
            }
        }
    }

    // Multiplica o alpha do quadro pela cobertura acumulada. (Em imagens
    // premultiplied, só o alpha muda; o RGB já guarda cor · alpha.)
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        const float* cv = &cov[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            uchar& a = line[x * 4 + 3];
            a = (uchar)std::lround(a * cv[x]);
        }
    }
}

// ── Grão de filme: máscaras pré-geradas ──────────────────────────────
// Em vez de calcular hash + lround + clamp por pixel a cada quadro (caro), o
// ruído é pré-renderizado em tiles pequenos (Grayscale8, valor = 128 + offset)
// guardados num cache. Aplicar o grão vira uma soma/subtração de bytes com wrap
// do tile — O(W·H) mas sem ramificações nem ponto flutuante por pixel. As
// variantes animam o grão (o índice muda por quadro); o amount só seleciona o
// nível pré-escalado (offsets multiplicados), então a intensidade não custa nada
// em runtime.
namespace {
constexpr int kGrainTile = 128;      // lado do tile em pixels
constexpr int kGrainVariants = 24;   // texturas distintas para a animação
constexpr int kGrainAmountLevels = 16; // quantização do amount (0..16)

struct GrainMaskKey {
    int variant = 0;
    int amountLevel = 0;
    bool operator==(const GrainMaskKey& o) const {
        return variant == o.variant && amountLevel == o.amountLevel;
    }
};

// Gera um tile de ruído monocromático (1 pixel de grão): offset em
// [-maxOff, +maxOff] escalado por amountLevel/16, codificado como 128+offset
// (centro neutro). O tamanho do grão é aplicado na leitura (subamostragem), então
// o mesmo tile serve para qualquer `size`.
inline QImage makeGrainTile(int variant, int amountLevel)
{
    QImage tile(kGrainTile, kGrainTile, QImage::Format_Grayscale8);
    const double maxOff = 255.0 * 0.5; // desvio máximo (amount = 1)
    const double scale = (double)amountLevel / (double)kGrainAmountLevels;
    uint32_t state = (uint32_t)(variant * 2654435761u + 0x9E3779B9u);
    auto next = [&state]() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return state;
    };
    for (int y = 0; y < kGrainTile; ++y) {
        uchar* line = tile.scanLine(y);
        for (int x = 0; x < kGrainTile; ++x) {
            const double n = ((double)(next() & 0xFFFFu) / 32767.5) - 1.0;
            line[x] = (uchar)std::clamp(
                (int)std::lround(128 + n * maxOff * scale), 0, 255);
        }
    }
    return tile;
}

// Cache thread-safe das máscaras (o grão roda na thread do worker de vídeo e,
// com LAINKA, na UI thread). Retorna por valor: QImage é COW, então a cópia
// compartilha os bytes — e evita referência invalidada por realocação.
QImage grainMask(int variant, int amountLevel)
{
    static QMutex mtx;
    static std::vector<std::pair<GrainMaskKey, QImage>> cache;
    QMutexLocker lock(&mtx);
    for (auto& e : cache)
        if (e.first.variant == variant && e.first.amountLevel == amountLevel)
            return e.second;
    if (cache.size() >= 256) cache.erase(cache.begin());
    cache.emplace_back(GrainMaskKey{variant, amountLevel},
                       makeGrainTile(variant, amountLevel));
    return cache.back().second;
}
} // namespace

void PreviewWidget::applyBasicEffectsOn(QImage& img, const Clip& c, double rel) {
    if (img.isNull()) return;

    // Malha 3×3 (warp): espreime/estica a mídia antes das máscaras.
    if (c.hasMesh())
        warpByMesh(img, c.mesh, c.meshCols, c.meshRows);

    // Máscaras: recorta o quadro por forma (rect/ellipse), com feather e
    // invert, em união. Rodam antes do chroma/desfoque/cor para que esses
    // efeitos respeitem a área visível.
    if (c.hasMask())
        applyMasks(img, c, rel);

    // Chroma Key robusto: distância perceptual ponderada, bordas suaves,
    // supressão de spill e curva de transição suave (smoothstep).
    if (c.chromaKey) {
        QImage result = ImgPool::get(QImage::Format_ARGB32, img.width(), img.height());
        result.fill(Qt::transparent);
        const int w = img.width();
        const int h = img.height();
        const QColor key = c.chromaKeyColor;
        const double sim = std::clamp(c.chromaKeySimilarity, 0.0, 1.0);
        const double soft = std::clamp(c.chromaKeySoftness, 0.0, 1.0);
        const double spill = std::clamp(c.chromaKeySpillSuppress, 0.0, 1.0);
        const int kr = key.red(), kg = key.green(), kb = key.blue();

        // Pesos perceptuais (BT.601): olho humano mais sensível ao verde.
        constexpr double wR = 0.299, wG = 0.587, wB = 0.114;

        // Faixa total. A tolerância da borda é ABSOLUTA (distância perceptual),
        // não proporcional à similaridade: com o mapeamento relativo anterior
        // (core * (1 + soft*3)) a rampa default ficava com ~11 de 255 — corte
        // duro — e aumentar a similaridade estrangulava a suavidade.
        const double coreRange = sim * 255.0;
        const double fullRange = coreRange + soft * 255.0;
        const double fullSq = fullRange * fullRange;

        // Canal dominante da cor-chave: define qual componente o spill remove.
        // Antes isto era fixo em verde, então o controle não fazia nada em
        // fundo azul/vermelho. Ties (ciano, magenta, amarelo, cinza) ficam de
        // fora — sem canal dominante definido não há respingo a atribuir.
        const int keyCh = (kg >= kr && kg >= kb) ? 1 : (kb >= kr ? 2 : 0);
        const int keyVals[3] = { kr, kg, kb };
        const bool keyDominates = keyVals[keyCh] > keyVals[(keyCh + 1) % 3]
                               && keyVals[keyCh] > keyVals[(keyCh + 2) % 3];

        for (int y = 0; y < h; ++y) {
            const uchar* src = img.constScanLine(y);
            uchar* dst = result.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                const int pr = src[si + 0], pg = src[si + 1], pb = src[si + 2];
                const int dr = pr - kr, dg = pg - kg, db = pb - kb;

                // Distância perceptual ponderada (evita sqrt no loop principal).
                const double distSq = wR * dr * dr + wG * dg * dg + wB * db * db;

                if (distSq >= fullSq) {
                    // Fora da faixa: pixel opaco.
                    dst[si + 0] = pr;
                    dst[si + 1] = pg;
                    dst[si + 2] = pb;
                    dst[si + 3] = 255;
                } else {
                    // Dentro da faixa: alpha com transição suave.
                    const double dist = std::sqrt(distSq);
                    double alpha;
                    if (dist <= coreRange) {
                        alpha = 0.0; // match total → totalmente transparente
                    } else {
                        // Curva smoothstep na borda (mais suave que linear).
                        const double t = (dist - coreRange) / (fullRange - coreRange);
                        const double s = t * t * (3.0 - 2.0 * t); // smoothstep
                        alpha = s * 255.0;
                    }

                    dst[si + 0] = pr;
                    dst[si + 1] = pg;
                    dst[si + 2] = pb;

                    // Supressão de spill: remove o respingo da cor-chave nos
                    // pixels da borda (foreground contaminado pela luz do fundo).
                    if (spill > 0.0 && keyDominates) {
                        const int pv[3] = { pr, pg, pb };
                        const int otherA = pv[(keyCh + 1) % 3];
                        const int otherB = pv[(keyCh + 2) % 3];
                        // Só há respingo onde a cor-chave ainda domina o pixel.
                        // O teste antigo era `pixLum > keyLum`, que descartava
                        // justamente os pixels escuros contaminados (cabelo
                        // preto, sombra) — os casos mais visíveis.
                        if (pv[keyCh] > otherA && pv[keyCh] > otherB) {
                            // Peso em sino: o respingo é visível nos pixels de
                            // borda, meio transparentes. `1 - alpha/255` era
                            // máximo onde o pixel é invisível e zero onde é
                            // opaco — invertido em relação à visibilidade.
                            const double a = alpha / 255.0;
                            const double bell = 4.0 * a * (1.0 - a);
                            const double spillAmt = spill * (0.35 + 0.65 * bell);
                            // Despill clássico: puxa o canal da chave em direção
                            // à média dos outros dois. Com spillAmt = 1 o canal
                            // vira essa média; valores menores corrigem em parte.
                            const int replacement = (otherA + otherB) / 2;
                            const int corrected = (int)std::lround(
                                pv[keyCh] - spillAmt * (pv[keyCh] - replacement));
                            dst[si + keyCh] = (uchar)std::clamp(corrected, 0, 255);
                        }
                    }
                    dst[si + 3] = (uchar)std::clamp((int)std::lround(alpha), 0, 255);
                }
            }
        }
        ImgPool::release(img);
        img = result;
    }

    // Desfoque: box blur separável com sliding window — O(W·H) independente do raio.
    if (c.blur > 0.5) {
        const int radius = std::clamp((int)std::lround(c.blur * 0.5), 1, 30);
        const int w = img.width();
        const int h = img.height();

        // Buffers estáticos por thread (reutilizados entre frames).
        static thread_local std::vector<int> s_rowR, s_rowG, s_rowB, s_rowA;
        if ((int)s_rowR.size() < w) {
            s_rowR.resize(w);
            s_rowG.resize(w);
            s_rowB.resize(w);
            s_rowA.resize(w);
        }

        // Passada horizontal com sliding window: resultado em temp (buffer do
        // pool, evitando alocar uma imagem cheia por quadro).
        QImage temp = ImgPool::get(img.format(), w, h);
        for (int y = 0; y < h; ++y) {
            const uchar* src = img.constScanLine(y);
            uchar* dst = temp.scanLine(y);

            // Inicializa a soma da janela [0, radius].
            int sr = 0, sg = 0, sb = 0, sa = 0;
            const int initEnd = std::min(radius, w - 1);
            for (int sx = 0; sx <= initEnd; ++sx) {
                const int si = sx * 4;
                sr += src[si]; sg += src[si + 1]; sb += src[si + 2]; sa += src[si + 3];
            }
            int cnt = initEnd + 1;

            for (int x = 0; x < w; ++x) {
                // Escreve o pixel médio.
                const int di = x * 4;
                dst[di]     = (uchar)(sr / cnt);
                dst[di + 1] = (uchar)(sg / cnt);
                dst[di + 2] = (uchar)(sb / cnt);
                dst[di + 3] = (uchar)(sa / cnt);

                // Sliding window: adiciona o pixel que entra, remove o que sai.
                const int addX = x + radius + 1;
                const int remX = x - radius;
                if (addX < w) {
                    const int ai = addX * 4;
                    sr += src[ai]; sg += src[ai + 1]; sb += src[ai + 2]; sa += src[ai + 3];
                    ++cnt;
                }
                if (remX >= 0) {
                    const int ri = remX * 4;
                    sr -= src[ri]; sg -= src[ri + 1]; sb -= src[ri + 2]; sa -= src[ri + 3];
                    --cnt;
                }
            }
        }

        // Passada vertical com sliding window: resultado de volta em img.
        int* rowR = s_rowR.data();
        int* rowG = s_rowG.data();
        int* rowB = s_rowB.data();
        int* rowA = s_rowA.data();

        // Inicializa as somas com a banda [0, radius] para cada coluna.
        {
            const uchar* firstRow = temp.constScanLine(0);
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                rowR[x] = firstRow[si];
                rowG[x] = firstRow[si + 1];
                rowB[x] = firstRow[si + 2];
                rowA[x] = firstRow[si + 3];
            }
        }
        const int initEndV = std::min(radius, h - 1);
        for (int sy = 1; sy <= initEndV; ++sy) {
            const uchar* src = temp.constScanLine(sy);
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                rowR[x] += src[si];
                rowG[x] += src[si + 1];
                rowB[x] += src[si + 2];
                rowA[x] += src[si + 3];
            }
        }
        int cntV = initEndV + 1;

        for (int y = 0; y < h; ++y) {
            uchar* dst = img.scanLine(y);

            // Escreve pixel médio para esta linha.
            for (int x = 0; x < w; ++x) {
                const int di = x * 4;
                dst[di]     = (uchar)(rowR[x] / cntV);
                dst[di + 1] = (uchar)(rowG[x] / cntV);
                dst[di + 2] = (uchar)(rowB[x] / cntV);
                dst[di + 3] = (uchar)(rowA[x] / cntV);
            }

            // Sliding window: adiciona a linha que entra, remove a que sai.
            const int addY = y + radius + 1;
            const int remY = y - radius;
            if (addY < h) {
                const uchar* addRow = temp.constScanLine(addY);
                for (int x = 0; x < w; ++x) {
                    const int si = x * 4;
                    rowR[x] += addRow[si];
                    rowG[x] += addRow[si + 1];
                    rowB[x] += addRow[si + 2];
                    rowA[x] += addRow[si + 3];
                }
                ++cntV;
            }
            if (remY >= 0) {
                const uchar* remRow = temp.constScanLine(remY);
                for (int x = 0; x < w; ++x) {
                    const int si = x * 4;
                    rowR[x] -= remRow[si];
                    rowG[x] -= remRow[si + 1];
                    rowB[x] -= remRow[si + 2];
                    rowA[x] -= remRow[si + 3];
                }
                --cntV;
            }
        }
        ImgPool::release(temp);
    }

    // Preto e branco.
    if (c.grayscale) {
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                const int si = x * 4;
                const int gray = (int)std::lround(0.299 * line[si + 2] + 0.587 * line[si + 1] + 0.114 * line[si]);
                line[si + 0] = line[si + 1] = line[si + 2] = (uchar)std::clamp(gray, 0, 255);
            }
        }
    }

    // Brilho, contraste, saturação.
    const bool needEq = (c.brightness != 0.0 || c.contrast != 1.0 || c.saturation != 1.0);
    if (needEq) {
        const double br = std::clamp(c.brightness, -1.0, 1.0) * 255.0;
        const double ct = std::clamp(c.contrast, 0.0, 2.0);
        const double factor = (ct == 1.0) ? 1.0 : (259.0 * (ct * 255.0 + 255.0)) / (255.0 * (259.0 - ct * 255.0));
        const double sa = std::clamp(c.saturation, 0.0, 2.0);
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            for (int x = 0; x < img.width(); ++x) {
                const int si = x * 4;
                double r = line[si + 2], g = line[si + 1], b = line[si + 0];
                // Brilho
                r += br; g += br; b += br;
                // Contraste
                r = factor * (r - 128.0) + 128.0;
                g = factor * (g - 128.0) + 128.0;
                b = factor * (b - 128.0) + 128.0;
                // Saturação
                if (sa != 1.0) {
                    const double gray = 0.299 * r + 0.587 * g + 0.114 * b;
                    r = gray + sa * (r - gray);
                    g = gray + sa * (g - gray);
                    b = gray + sa * (b - gray);
                }
                line[si + 2] = (uchar)std::clamp((int)std::lround(r), 0, 255);
                line[si + 1] = (uchar)std::clamp((int)std::lround(g), 0, 255);
                line[si + 0] = (uchar)std::clamp((int)std::lround(b), 0, 255);
            }
        }
    }
// Color grade estilo Lumetri (LGG + exposure/curves/LUT/vignette…).
    // Ordem alinhada ao ProjectExporter (colorgrade::ffmpegFilters).
    if (c.hasColorGrade()) {
        colorgrade::CubeLut lut;
        const bool hasLut = !c.cgLutPath.isEmpty()
            && colorgrade::loadCubeFile(c.cgLutPath, lut);
        colorgrade::applyToImage(img, c, hasLut ? &lut : nullptr);
    }

    // PSX: quantização de cor para profundidade baixa + dithering ordenado
    // (padrão Bayer 4x4) — o "granulado" característico do PlayStation 1.
    // O desempenho é importante (loop por pixel na thread da UI), então só o
    // passo de quantização + o deslocamento determinístico do padrão.
    if (c.psxEnabled) {
        const int bits = std::clamp(c.psxBits, 3, 8);
        const double step = 255.0 / (1 << bits);
        const double dith = std::clamp(c.psxDither, 0.0, 1.0) * step;
        // Bayer 4x4 clássico (0..15) → offset -0.5..0.5 do passo.
        static const int bayer[16] = {
            0, 8, 2, 10,
            12, 4, 14, 6,
            3, 11, 1, 9,
            15, 7, 13, 5
        };
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            const int by = (y & 3) * 4;
            for (int x = 0; x < img.width(); ++x) {
                const int si = x * 4;
                const double off = (bayer[by + (x & 3)] - 7.5) / 15.0 * dith;
                for (int ch = 0; ch < 3; ++ch) {
                    double v = line[si + ch] + off;
                    v = std::lround(std::clamp(v, 0.0, 255.0) / step) * step;
                    line[si + ch] = (uchar)std::clamp((int)std::lround(v), 0, 255);
                }
            }
        }
    }

    // Grão de filme: ruído monocromático animado por quadro. A textura vem de um
    // tile pré-gerado (grainMask) cuja variante muda a cada quadro; aplicar é só
    // somar (offset mantido em 128 → subtrai-se 128) nos três canais, com wrap.
    if (c.filmGrain && c.filmGrainAmount > 0.0) {
        const int w = img.width();
        const int h = img.height();
        const int size = std::clamp(c.filmGrainSize, 1, 4);
        const double amount = std::clamp(c.filmGrainAmount, 0.0, 1.0);
        const int amountLevel = std::clamp((int)std::lround(amount * kGrainAmountLevels),
                                           1, kGrainAmountLevels);
        // Variante por quadro: muda rápido o bastante para "ferver" (usa o tempo
        // relativo em ~fps), mas estável em replays (determinístico por tempo).
        const int variant = (int)(((int64_t)std::llround(rel * 48.0)) % kGrainVariants
                                  + kGrainVariants) % kGrainVariants;
        const QImage mask = grainMask(variant, amountLevel);
        const int mw = mask.width();
        const int mh = mask.height();
        // Caminha por blocos de `size` px: o índice na máscara só avança quando
        // cruza um bloco, então não há divisão/módulo por pixel.
        for (int y = 0; y < h; ++y) {
            uchar* line = img.scanLine(y);
            const uchar* mline = mask.constScanLine((y / size) % mh);
            int mx = 0;
            int col = 0;
            for (int x = 0; x < w; ++x) {
                if (col == size) { col = 0; if (++mx == mw) mx = 0; }
                ++col;
                const int off = (int)mline[mx] - 128;
                if (off == 0) continue;
                const int si = x * 4;
                line[si + 0] = (uchar)std::clamp(line[si + 0] + off, 0, 255);
                line[si + 1] = (uchar)std::clamp(line[si + 1] + off, 0, 255);
                line[si + 2] = (uchar)std::clamp(line[si + 2] + off, 0, 255);
            }
        }
    }
}

// Aplica pan/crop sobre o quadro cheio (m_frameFull) e guarda em m_frame.
void PreviewWidget::applyMotionOn(QImage& img, const Clip& c) {
    if (img.isNull() || !c.motionEnabled || c.motionAmount <= 0.0) return;
    const double rad = c.motionAngle * M_PI / 180.0;
    const double dist = c.motionAmount * 0.4;
    const int sx = (int)std::lround(std::cos(rad) * dist);
    const int sy = (int)std::lround(std::sin(rad) * dist);
    const int ns = std::clamp(c.motionSamples, 1, 32);
    QImage result = ImgPool::get(img.format(), img.width(), img.height());
    {
        QPainter p(&result);
        p.drawImage(0, 0, img);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        for (int i = 1; i < ns; ++i) {
            const double alpha = 1.0 - (double)i / ns;
            p.setOpacity(alpha / ns);
            p.drawImage(sx * i, sy * i, img);
        }
    }
    ImgPool::release(img);
    img = result;
}

void PreviewWidget::applyTemporalFxOn(QImage& img, const Clip& c, double absTime, double rel,
                                      OfxPluginManager* ofxMgr, Frei0rPluginManager* frei0rMgr) {
    if (img.isNull()) return;
    applyMotionOn(img, c);
    if (!c.ofxFx.isEmpty() && ofxMgr)
        img = OfxRenderer::applyOfxEffects(img, c.ofxFx, ofxMgr, absTime);
    if (!c.frei0rFx.isEmpty() && frei0rMgr)
        img = Frei0rPluginManager::applyEffects(img, c.frei0rFx, frei0rMgr, rel);
}

void PreviewWidget::applyCrop() {
    // Memo LAINKA: em stop motion o tempo quantizado é constante por N quadros,
    // mas esta cadeia inteira é cara e síncrona na thread da UI (o warp 8x8 são
    // 64 transforms de tile e o MotiOn desenha o quadro N vezes com
    // SmoothPixmapTransform). Como nada aqui depende do tempo NÃO quantizado —
    // o jitter/flicker/warp usam m_lainkaQuantizedTime e as máscaras usam
    // m_lastSrcT, que também é o tempo quantizado — o resultado de um quadro
    // vale para os N-1 seguintes.
    //
    // Duas exceções, ambas com saída dependente do histórico/relógio:
    //  - onion skin e o MotiOn do LAINKA consomem `prevFrame` (o quadro
    //    anterior), que deixa de ser atualizado enquanto o memo acerta;
    //  - OFX recebe m_playhead cru e pode variar a cada quadro.
    // O crop entra na chave porque os keyframes dele são avaliados no tempo
    // NÃO quantizado: com crop animado o recorte anda dentro da janela de stop
    // motion mesmo com o tempo quantizado parado.
    const bool memoOk = m_clipLainkaEnabled
                        && m_clipOfxFx.isEmpty()
                        && m_clipLainkaOnionSkin < 1e-6
                        && m_clipLainkaMotionBlur < 1e-6;
    if (memoOk && !m_cropMemo.isNull() && m_cropMemoClipId == m_clipLainkaId
        && m_cropMemoFile == m_lastFile
        && m_cropMemoTime == m_lainkaQuantizedTime
        && m_cropMemoSrcT == m_lastSrcT
        && m_cropMemoSrcW == m_lastDecodeW
        && m_cropMemoCropL == m_lastCropL && m_cropMemoCropR == m_lastCropR
        && m_cropMemoCropT == m_lastCropT && m_cropMemoCropB == m_lastCropB) {
        m_frame = m_cropMemo;
        return;
    }

    m_frame = applyCropTo(m_frameFull, m_lastCropL, m_lastCropR, m_lastCropT, m_lastCropB);
    // Aplica efeito LAINKA (stop motion) após o crop.
    if (m_clipLainkaEnabled) {
        QImage prevFrame = m_lainkaPrevFrame;
        m_lainkaPrevFrame = m_frame;
        // Usa tempo quantizado para que jitter/flicker/warp fiquem
        // congelados no mesmo intervalo de stop motion.
        const double fxT = (m_lainkaQuantizedTime >= 0.0)
            ? m_lainkaQuantizedTime : m_playhead;
        m_frame = lainkaApplyFx(m_frame, m_clipLainkaId, fxT,
                                m_clipLainkaSkip, m_clipLainkaJitterPos,
                                m_clipLainkaJitterRot, m_clipLainkaJitterScale,
                                m_clipLainkaFlicker, m_clipLainkaFlickerSpeed,
                                m_clipLainkaWarpAmount, m_clipLainkaWarpSpeed,
                                m_clipLainkaWarpGrid, m_clipLainkaOnionSkin,
                                m_clipLainkaDustAmount, m_clipLainkaScratchAmount,
                                m_clipLainkaMotionBlur, m_clipLainkaOpacity,
                                m_clipLainkaTargetFps, m_clipLainkaAntialias,
                                prevFrame);
    }
    if (m_clipMotionEnabled && m_clipMotionAmount > 0.0 && !m_frame.isNull()) {
        Clip motionSnap;
        motionSnap.motionEnabled = true;
        motionSnap.motionAmount = m_clipMotionAmount;
        motionSnap.motionAngle = m_clipMotionAngle;
        motionSnap.motionSamples = m_clipMotionSamples;
        applyMotionOn(m_frame, motionSnap);
    }
    // Aplica efeitos básicos (brilho, contraste, saturação, etc.)
    applyBasicEffects(m_frame);

    // MotiOn/OFX/frei0r — mesma pilha do worker (applyTemporalFxOn). O tempo
    // do frei0r é o relativo do clipe do topo (paridade com o fast path).
    {
        Clip temporal;
        temporal.motionEnabled = false; // MotiOn já aplicado acima (snapshot)
        temporal.ofxFx = m_clipOfxFx;
        temporal.frei0rFx = m_clipFrei0rFx;
        double f0rRel = m_lastSrcT;
        if (m_project && !m_clipLainkaId.isEmpty()) {
            for (const Track& tr : m_project->videoTracks) {
                for (const Clip& c : tr.clips) {
                    if (c.id == m_clipLainkaId) {
                        f0rRel = m_playhead - c.pos;
                        break;
                    }
                }
            }
        }
        if (!temporal.ofxFx.isEmpty() && m_ofxManager && !m_frame.isNull())
            m_frame = OfxRenderer::applyOfxEffects(m_frame, temporal.ofxFx, m_ofxManager,
                                                   m_playhead);
        // Efeitos frei0r (padrão Kdenlive/Shotcut) — depois dos OFX; export
        // usa o mesmo filtro ffmpeg `frei0r=` (paridade preview↔export).
        if (!temporal.frei0rFx.isEmpty() && m_frei0rManager && !m_frame.isNull())
            m_frame = Frei0rPluginManager::applyEffects(m_frame, temporal.frei0rFx,
                                                        m_frei0rManager, f0rRel);
    }

    if (memoOk) {
        m_cropMemo = m_frame;
        m_cropMemoClipId = m_clipLainkaId;
        m_cropMemoFile = m_lastFile;
        m_cropMemoTime = m_lainkaQuantizedTime;
        m_cropMemoSrcT = m_lastSrcT;
        m_cropMemoSrcW = m_lastDecodeW;
        m_cropMemoCropL = m_lastCropL;
        m_cropMemoCropR = m_lastCropR;
        m_cropMemoCropT = m_lastCropT;
        m_cropMemoCropB = m_lastCropB;
    } else if (!m_cropMemo.isNull()) {
        // Fora do LAINKA (ou com OFX/onion skin/MotiOn temporal) não há acerto
        // possível, então libera a imagem em vez de segurar ~8 MB à toa — o que
        // o comentário do membro promete.
        m_cropMemo = QImage();
    }
}

void PreviewWidget::drawGrid(QPainter& p, const QRect& canvas) {
    if (!m_showGrid || canvas.width() < 2 || canvas.height() < 2) return;

    p.save();
    p.setClipRect(canvas);
    p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::SolidLine));

    // Margens de segurança do Premiere: dois retângulos concêntricos —
    // "Action Safe" a 90% e "Title Safe" a 80% do quadro, com os cantos
    // marcados (é assim que o Premiere as desenha, não uma grade NxN).
    const auto insetRect = [&](double frac) {
        const double w = canvas.width() * frac;
        const double h = canvas.height() * frac;
        return QRectF(canvas.center().x() - w / 2.0, canvas.center().y() - h / 2.0,
                      w, h);
    };
    const auto cornerTicks = [&](const QRectF& r, int len) {
        const double x0 = r.left(), x1 = r.right(), y0 = r.top(), y1 = r.bottom();
        p.drawLine(QPointF(x0, y0), QPointF(x0 + len, y0));
        p.drawLine(QPointF(x0, y0), QPointF(x0, y0 + len));
        p.drawLine(QPointF(x1, y0), QPointF(x1 - len, y0));
        p.drawLine(QPointF(x1, y0), QPointF(x1, y0 + len));
        p.drawLine(QPointF(x0, y1), QPointF(x0 + len, y1));
        p.drawLine(QPointF(x0, y1), QPointF(x0, y1 - len));
        p.drawLine(QPointF(x1, y1), QPointF(x1 - len, y1));
        p.drawLine(QPointF(x1, y1), QPointF(x1, y1 - len));
    };

    const QRectF action = insetRect(0.90);
    const QRectF title = insetRect(0.80);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::SolidLine));
    p.drawRect(action);
    p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::SolidLine));
    p.drawRect(title);
    cornerTicks(action, 10);
    cornerTicks(title, 8);

    p.restore();
}

#include "ui/monitor/PreviewWidget.moc"

// ════════════════════════════════════════════════════════════════════════
// Overlay de edição de máscara: desenha as formas do clipe sobre o monitor
// com alças arrastáveis (mover centro, redimensionar bordas, rotacionar) e
// repassa as edições via sinal. O usuário arrasta direto no preview, como no
// Pantalone/Transform do Vegas — só é aplicado ao clipe quando o dialog salva.
// ════════════════════════════════════════════════════════════════════════
namespace {

const Clip* previewFindClip(const Project* p, const QString& id) {
    if (!p || id.isEmpty()) return nullptr;
    for (const Track& t : p->videoTracks)
        for (const Clip& c : t.clips)
            if (c.id == id) return &c;
    for (const Track& t : p->audioTracks)
        for (const Clip& c : t.clips)
            if (c.id == id) return &c;
    return nullptr;
}

// Dimensões do quadro onde a máscara vive (o frame já cortado). Para o clipe
// do topo usa m_frame; senão deriva da mídia + pan/crop (mesmo espaço do
// applyMasks). Só a proporção importa para o overlay.
QSize previewMaskAnchorSize(const Project* p, const Clip* c,
                            const QImage& topFrame, const QString& topId) {
    if (c && c->id == topId && !topFrame.isNull())
        return topFrame.size();
    const MediaItem* m = c && p ? p->findMedia(c->mediaId) : nullptr;
    int w = m ? m->width : 0, h = m ? m->height : 0;
    if (w <= 0 || h <= 0) { w = p ? p->width : 1920; h = p ? p->height : 1080; }
    const double cl = std::clamp(c ? c->cropL : 0.0, 0.0, 1000.0);
    const double cr = std::clamp(c ? c->cropR : 0.0, 0.0, 1000.0);
    const double ct = std::clamp(c ? c->cropT : 0.0, 0.0, 1000.0);
    const double cb = std::clamp(c ? c->cropB : 0.0, 0.0, 1000.0);
    const int cw = qMax(1, (int)std::lround(w * (1.0 - (cl + cr) / 1000.0)));
    const int ch = qMax(1, (int)std::lround(h * (1.0 - (ct + cb) / 1000.0)));
    return QSize(cw, ch);
}

// Mesma aritmética da drawLayer (paintEvent): imagem do clipe → monitor.
QTransform previewLayerToScreen(const Project* p, const Clip& c,
                                const QSize& imgSize, double rel,
                                double k, const QPointF& center) {
    QTransform t;
    const double W = p ? p->width : 1920.0;
    const double H = p ? p->height : 1080.0;
    const double iw = qMax(1, imgSize.width());
    const double ih = qMax(1, imgSize.height());
    const double fit = qMin(W / iw, H / ih);
    const double s  = kfValue(c.kfScale, c.scale, rel);
    const double sX = kfValue(c.kfScaleX, c.scaleX, rel);
    const double sY = kfValue(c.kfScaleY, c.scaleY, rel);
    const double rot = kfValue(c.kfRotation, c.rotation, rel);
    const double tx = kfValue(c.kfTx, c.tx, rel);
    const double ty = kfValue(c.kfTy, c.ty, rel);
    t.translate(center.x() + tx * k, center.y() + ty * k);
    t.rotate(rot);
    t.scale(k * fit * s * sX, k * fit * s * sY);
    t.translate(-iw / 2.0, -ih / 2.0);
    return t;
}

// Controles da máscara em pixels do espaço da imagem:
// pts[0]=centro, pts[1]=alça de rotação, pts[2..5]=extremos (E, topo, O, baixo).
struct MaskCtrl {
    QVector<QPointF> pts;
};
MaskCtrl previewMaskCtrl(const Mask& m, const QSize& size, double rel) {
    MaskCtrl o;
    const double W = size.width(), H = size.height();
    const double cx = m.cxAt(rel) * W, cy = m.cyAt(rel) * H;
    const double rx = std::max(0.0, m.rxAt(rel)) * W;
    const double ry = std::max(0.0, m.ryAt(rel)) * H;
    const double rad = m.rotAt(rel) * M_PI / 180.0;
    QVector<QPointF> base;
    if (m.type == QLatin1String("rect")) {
        base = { QPointF(cx - rx, cy - ry), QPointF(cx + rx, cy - ry),
                 QPointF(cx + rx, cy + ry), QPointF(cx - rx, cy + ry) };
    } else { // ellipse: extremos cardeais
        base = { QPointF(cx - rx, cy), QPointF(cx, cy - ry),
                 QPointF(cx + rx, cy), QPointF(cx, cy + ry) };
    }
    if (rad != 0.0) {
        const double cs = std::cos(rad), sn = std::sin(rad);
        for (QPointF& b : base) {
            const double dx = b.x() - cx, dy = b.y() - cy;
            b = QPointF(cx + dx * cs - dy * sn, cy + dx * sn + dy * cs);
        }
    }
    const double arm = qMax(1.0, std::min(rx, ry));
    o.pts = { QPointF(cx, cy),
              QPointF(cx - arm * std::sin(rad), cy - arm * std::cos(rad)) };
    for (const QPointF& b : base) o.pts.append(b);
    return o;
}

void maskHandleSquare(QPainter& p, const QPointF& pt, const QColor& accent) {
    const double hs = 4.5;
    p.setBrush(Qt::white);
    p.setPen(QPen(accent, 1.4));
    p.drawRect(QRectF(pt.x() - hs, pt.y() - hs, 2.0 * hs, 2.0 * hs));
}

} // namespace

void PreviewWidget::setMaskOverlay(const QString& clipId, const QVector<Mask>& masks) {
    m_maskOverlayClipId = clipId;
    m_maskOverlay = masks;
    m_maskDragIndex = -1;
    m_maskDragHandle = -1;
    update();
}

void PreviewWidget::drawMaskOverlay(QPainter& p, const QRect& canvas, double k) {
    if (m_maskOverlayClipId.isEmpty() || !m_project || m_maskOverlay.isEmpty()) return;
    const Clip* clip = previewFindClip(m_project, m_maskOverlayClipId);
    if (!clip) { setMaskOverlay(QString(), {}); return; }
    const double rel = m_playhead - clip->pos;
    if (m_playhead < clip->pos - 1e-6 || m_playhead > clip->pos + clip->dur + 1e-6) return;
    const QSize imgSize = previewMaskAnchorSize(m_project, clip, m_frame, m_maskOverlayClipId);
    if (imgSize.isEmpty()) return;
    const QPointF center(canvas.center());
    m_maskToScreen = previewLayerToScreen(m_project, *clip, imgSize, rel, k, center);
    m_maskAnchorSize = imgSize;

    const QColor accent = themeColors().accent;
    const QColor band(130, 215, 255); // contorno vivo, visível sobre qualquer mídia
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_maskOverlay.size(); ++i) {
        const Mask& m = m_maskOverlay[i];
        if (m.type.isEmpty()) continue;
        const double cx = m.cxAt(rel) * imgSize.width();
        const double cy = m.cyAt(rel) * imgSize.height();
        const double rx = std::max(0.001, m.rxAt(rel)) * imgSize.width();
        const double ry = std::max(0.001, m.ryAt(rel)) * imgSize.height();
        const double rot = m.rotAt(rel);
        QPainterPath path;
        if (m.type == QLatin1String("rect"))
            path.addRect(QRectF(cx - rx, cy - ry, 2.0 * rx, 2.0 * ry));
        else if (m.type == QLatin1String("ellipse"))
            path.addEllipse(QPointF(cx, cy), rx, ry);
        else continue;
        if (rot != 0.0) {
            QTransform r; r.translate(cx, cy); r.rotate(rot); r.translate(-cx, -cy);
            path = r.map(path);
        }
        const bool en = m.hasMask();
        p.setPen(QPen(en ? band : QColor(band.red(), band.green(), band.blue(), 110),
                       1.6, Qt::DashLine));
        p.setBrush(en ? QColor(accent.red(), accent.green(), accent.blue(), 34)
                      : Qt::NoBrush);
        p.drawPath(m_maskToScreen.map(path));
        if (!en) continue;

        const MaskCtrl ctl = previewMaskCtrl(m, imgSize, rel);
        const QPointF c0 = m_maskToScreen.map(ctl.pts[0]);
        const QPointF c1 = m_maskToScreen.map(ctl.pts[1]);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.2));
        p.drawLine(c0, c1);
        maskHandleSquare(p, c1, accent);
        maskHandleSquare(p, c0, accent);
        for (int b = 2; b <= 5; ++b)
            maskHandleSquare(p, m_maskToScreen.map(ctl.pts[b]), accent);
    }
    p.restore();
}

bool PreviewWidget::pickMaskHandle(const QPoint& pos) {
    if (m_maskOverlayClipId.isEmpty() || !m_project) return false;
    const Clip* clip = previewFindClip(m_project, m_maskOverlayClipId);
    if (!clip) return false;
    const double rel = m_playhead - clip->pos;
    if (m_playhead < clip->pos - 1e-6 || m_playhead > clip->pos + clip->dur + 1e-6)
        return false;
    const QSize imgSize = previewMaskAnchorSize(m_project, clip, m_frame, m_maskOverlayClipId);
    if (imgSize.isEmpty()) return false;
    // Reconstrói canvas/escala como no paintEvent (para as alças baterem).
    const QRect work = m_videoRect.adjusted(12, 12, -12, -12);
    double k = m_zoom > 0.0 ? m_zoom
                            : qMin(work.width() / double(m_project->width),
                                   work.height() / double(m_project->height));
    QRect canvas(QPoint(0, 0), QSize(qMax(1, (int)(m_project->width * k)),
                                     qMax(1, (int)(m_project->height * k))));
    canvas.moveCenter(work.center());
    canvas = canvas.intersected(work);
    const QPointF center(canvas.center());
    m_maskToScreen = previewLayerToScreen(m_project, *clip, imgSize, rel, k, center);
    m_maskAnchorSize = imgSize;
    const QTransform imgToScreen = m_maskToScreen;
    const double tol = 9.0; // px de tolerância das alças
    for (int i = 0; i < m_maskOverlay.size(); ++i) {
        const Mask& m = m_maskOverlay[i];
        if (m.type.isEmpty() || !m.hasMask()) continue;
        const MaskCtrl ctl = previewMaskCtrl(m, imgSize, rel);
        int best = -1;
        double bestD = tol * tol;
        for (int h = 0; h < ctl.pts.size(); ++h) {
            const QPointF sp = imgToScreen.map(ctl.pts[h]);
            const double dx = sp.x() - pos.x(), dy = sp.y() - pos.y();
            const double d2 = dx * dx + dy * dy;
            if (d2 < bestD) { bestD = d2; best = h; }
        }
        if (best >= 0) {
            m_maskDragIndex = i;
            m_maskDragHandle = best;
            m_maskDragLast = pos;
            m_maskDragPressRot = m.rotAt(rel);
            const QPointF cur = m_maskToScreen.inverted().map(QPointF(pos));
            m_maskDragPressAng = std::atan2(cur.y() - m.cyAt(rel) * imgSize.height(),
                                            cur.x() - m.cxAt(rel) * imgSize.width());
            return true;
        }
    }
    return false;
}

void PreviewWidget::applyMaskDrag(const QPoint& pos) {
    if (m_maskDragIndex < 0 || m_maskDragIndex >= m_maskOverlay.size()) return;
    Mask& m = m_maskOverlay[m_maskDragIndex];
    const double iw = m_maskAnchorSize.width(), ih = m_maskAnchorSize.height();
    if (iw < 1 || ih < 1) return;
    const QPointF cur = m_maskToScreen.inverted().map(QPointF(pos));
    const double cx = m.cxAt(0.0) * iw, cy = m.cyAt(0.0) * ih;
    if (m_maskDragHandle == 0) {
        // Mover: o centro acompanha o cursor.
        m.cx = std::clamp(cur.x() / iw, 0.0, 1.0);
        m.cy = std::clamp(cur.y() / ih, 0.0, 1.0);
    } else if (m_maskDragHandle == 1) {
        // Rotação: ângulo do cursor ao redor do centro (contínuo).
        const double ang = std::atan2(cur.y() - cy, cur.x() - cx);
        double d = ang - m_maskDragPressAng;
        while (d > M_PI) d -= 2.0 * M_PI;
        while (d < -M_PI) d += 2.0 * M_PI;
        double rot = m_maskDragPressRot + d * 180.0 / M_PI;
        while (rot > 180.0) rot -= 360.0;
        while (rot < -180.0) rot += 360.0;
        m.rotation = rot;
    } else {
        // Borda: raio = distância do centro projetada nos eixos da forma.
        const double dx = cur.x() - cx, dy = cur.y() - cy;
        const double rad = m.rotAt(0.0) * M_PI / 180.0;
        const double lx =  dx * std::cos(rad) + dy * std::sin(rad);
        const double ly = -dx * std::sin(rad) + dy * std::cos(rad);
        if (std::abs(lx) > 1.0) m.rx = qBound(0.001, std::abs(lx) / iw, 1.5);
        if (std::abs(ly) > 1.0) m.ry = qBound(0.001, std::abs(ly) / ih, 1.5);
    }
    m_maskDragLast = pos;
    emit maskEdited(m_maskDragIndex, m);
    update();
}

void PreviewWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && pickMaskHandle(e->pos())) {
        update();
        return;
    }
    QWidget::mousePressEvent(e);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_maskDragIndex >= 0) {
        applyMaskDrag(e->pos());
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (m_maskDragIndex >= 0) {
        m_maskDragIndex = -1;
        m_maskDragHandle = -1;
        emit maskDragEnd();
        update();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}
