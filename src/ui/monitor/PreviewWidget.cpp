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
#include "laartman/core/DecoderMetrics.h"
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


#include "ui/monitor/PreviewPriv.h"


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
        m_genMemo.clear();
        m_prefetch = PrefetchFrame();
        m_shownPath.clear();
        m_shownClipId.clear(); // troca de projeto: sem posse herdada
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

// Quadro de mídia GERADA com memo — ver declaração. A chave carrega os
// parâmetros relevantes (id, tipo, cores, células, tamanho): editar qualquer
// um invalida sozinho, sem precisar de hook de edição.
QImage PreviewWidget::generatedFrameCached(const MediaItem& m, int w, int h) {
    w = qMax(1, w);
    h = qMax(1, h);
    quint64 key = qHash(m.id);
    key = key * 1099511628211ull + quint64(uint(m.generator.size()));
    for (const QChar c : m.generator)
        key = key * 1099511628211ull + c.unicode();
    key = key * 1099511628211ull + quint64(m.solidColor.rgba());
    key = key * 1099511628211ull + quint64(m.solidColor2.rgba());
    key = key * 1099511628211ull + quint64(m.genCells);
    key = key * 1099511628211ull + ((quint64(uint(w)) << 32) | quint64(uint(h)));
    const auto it = m_genMemo.constFind(key);
    if (it != m_genMemo.constEnd()) return it.value();
    QImage img = generatorFrame(m, w, h);
    // Teto: cada entrada é um quadro CHEIO; editar um gradiente ou redimensionar
    // a janela gera chaves novas em série. Sem poda, a sessão acumularia
    // quadros indefinidamente. É memo de conveniência (reconstruir custa um
    // generatorFrame), então limpar o cache inteiro ao passar do teto é
    // aceitável — mantém o pico de memória limitado.
    constexpr int kGenMemoMax = 64;
    if (m_genMemo.size() >= kGenMemoMax) m_genMemo.clear();
    m_genMemo.insert(key, img);
    return img;
}

