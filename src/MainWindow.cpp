// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MainWindow.h"
#include "version.h"

#include "ui/MediaPoolWidget.h"
#include "ui/TimelineWidget.h"
#include "ui/PreviewWidget.h"
#include "ui/PancropWidget.h"
#include "ui/GraphEditorWidget.h"
#include "ui/EffectsWidget.h"
#include "ui/ClipPropertiesWidget.h"
#include "ui/ExpressWidget.h"
#include "ui/FileBrowserWidget.h"
#include "ui/MixerWidget.h"
#include "ui/MesaWidget.h"
#include "ui/ExportDialog.h"
#include "ui/RenderQueueDialog.h"
#include "export/NleInterchange.h"
#include "ui/ScopeWidget.h"
#include "ui/PreviewMonitor.h"
#include "ui/ProjectSettingsDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"
#include "ofx/OfxPluginManager.h"

#include <QSettings>
// Devolve o atalho salvo pelo usuário (Configurações → Atalhos) ou o padrão.
static QKeySequence appKey(const char* id, const QKeySequence& fallback) {
    const QString v = QSettings().value(QStringLiteral("shortcuts/") + QLatin1String(id)).toString();
    return v.isEmpty() ? fallback : QKeySequence(v);
}
#include "ui/WelcomeWindow.h"
#include "ffmpeg/MediaCache.h"

#include <QApplication>
#include <QPointer>
#include <QDockWidget>
#include <QListWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QAction>
#include <QKeySequence>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractButton>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QCloseEvent>
#include <QShowEvent>
#include <QStyle>
#include <QShortcut>
#include <QPainterPath>
#include <QPolygonF>
#include <QComboBox>
#include <QTimer>
#include <QTime>
#include <QProgressBar>
#include <QGuiApplication>
#include <QCursor>
#include <QScreen>
#include <QDataStream>

namespace {
// Versão do arranjo de painéis (docks/toolbar). Aumente para descartar
// estados salvos antigos que estejam com o layout deslocado.
constexpr int kLayoutVersion = 3;

// QWidget::saveGeometry grava o array em big-endian na estrutura:
//   int version (== 1) | quint32 screen | QRect geometry | QRect frameGeometry
//   | QRect normalGeometry | int screenWidth | int screenHeight
// Arrays gravados por versões antigas ou com bytes corrompidos (ex.: TV 4K
// desligada a meio de um save) não seguem esse formato e quebravam o layout
// na primeira exibição. Só aceita dados que decodifiquem como geometria real.
bool saneGeometryArray(const QByteArray& geom) {
    QDataStream in(geom);
    in.setVersion(QDataStream::Qt_4_0);
    if (in.atEnd())
        return false;
    int version;
    in >> version;
    if (version != 1)
        return false;
    if (in.atEnd())
        return false;
    quint32 screen;
    in >> screen;
    QRect rect;
    in >> rect;
    if (in.status() != QDataStream::Ok)
        return false;
    const int w = rect.width();
    const int h = rect.height();
    if (w < 320 || w > 20000 || h < 240 || h > 20000)
        return false;
    return true;
}

// QMainWindow::saveState sempre começa pelo magic 0xff; qualquer outra coisa
// é estado corrompido ou gravado por outra versão do app.
bool saneLayoutArray(const QByteArray& state) {
    QDataStream in(state);
    in.setVersion(QDataStream::Qt_4_0);
    quint32 magic;
    in >> magic;
    return in.status() == QDataStream::Ok && magic == 0xff;
}
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Pierrot %1 — Editor de Vídeo").arg(QStringLiteral(PIERROT_VERSION)));
    resize(1280, 800);

    for (int i = 0; i < 3; ++i) m_project.addTrack(false);
    for (int i = 0; i < 3; ++i) m_project.addTrack(true);

    m_pool = new MediaPoolWidget(this);
    m_pool->setProject(&m_project);
    m_timeline = new TimelineWidget(this);
    m_timeline->setProject(&m_project);
    m_preview = new PreviewWidget(this);
    m_preview->setProject(&m_project);
    // O preview ocupa a área central e o monitor escala junto com a janela.
    auto* centralHost = new QWidget;
    m_centralLay = new QVBoxLayout(centralHost);
    m_centralLay->setContentsMargins(4, 4, 4, 4);
    m_centralLay->setSpacing(2);
    m_centralLay->addWidget(m_preview, 1);
    setCentralWidget(centralHost);

    m_pancrop = new PancropWidget(this);
    m_pancrop->setProject(&m_project);

    m_graph = new GraphEditorWidget(this);
    m_graph->setProject(&m_project);
    m_graph->setMinimumHeight(170);

    m_effects = new EffectsWidget(this);
    m_express = new ExpressWidget(this);
    m_fileBrowser = new FileBrowserWidget(this);

    m_props = new ClipPropertiesWidget(this);
    m_props->setWindowFlag(Qt::Window);
    m_props->setWindowTitle(tr("Propriedades"));
    m_props->resize(300, 640);
    m_props->hide();
    m_props->setProject(&m_project);

    // Gerenciador de plugins OFX — escaneia diretórios conhecidos.
    m_ofxManager = new OfxPluginManager(this);

    // Conecta callback de describe para popular parâmetros no Express.
    // DEVE ser definido ANTES de scanPlugins() para capturar os parâmetros.
    m_ofxManager->setDescribeCallback([this](const QString& pluginId,
                                             const QString& name,
                                             const QString& grouping,
                                             const QString& description,
                                             int versionMajor, int versionMinor,
                                             const QVector<OfxParamDefInfo>& params) {
        Q_UNUSED(name); Q_UNUSED(grouping); Q_UNUSED(description);
        Q_UNUSED(versionMajor); Q_UNUSED(versionMinor);
        m_express->setOfxParamDefs(pluginId, params);
    });

    m_ofxManager->scanPlugins();
    m_preview->setOfxManager(m_ofxManager);

    // Inicializa o painel de efeitos.
    m_effects->setProject(&m_project);
    m_effects->setOfxPlugins(m_ofxManager->plugins());

    // Inicializa o Express (editor de efeitos do clipe).
    m_express->setProject(&m_project);
    m_express->setOfxPlugins(m_ofxManager->plugins());
    connect(m_express, &ExpressWidget::modified, this, &MainWindow::setModified);
    connect(m_express, &ExpressWidget::modified, this, [this]() { m_preview->refreshView(); });

    // Clique/arrasto de efeito no painel → Express.
    connect(m_effects, &EffectsWidget::effectSelected, m_express, &ExpressWidget::addEffect);

    // Conecta seleção de clipes na timeline ao painel de efeitos, ao Express
    // e ao painel de Propriedades (Effect Controls).
    connect(m_timeline, &TimelineWidget::selectionChanged, this, [this](const QString& id) {
        Clip* clip = nullptr;
        if (!id.isEmpty()) {
            for (Track& t : m_project.videoTracks)
                for (Clip& c : t.clips)
                    if (c.id == id) { clip = &c; break; }
            if (!clip)
                for (Track& t : m_project.audioTracks)
                    for (Clip& c : t.clips)
                        if (c.id == id) { clip = &c; break; }
        }
        m_effects->setSelectedClip(clip);
        m_express->setSelectedClip(clip);
        m_props->showClip(id);
    });

    createDocks();
    createActions();

    // Barra de transporte abaixo do preview.
    auto* transportBar = new QToolBar(tr("Transporte"), this);
    transportBar->setMovable(false);
    transportBar->setIconSize(QSize(18, 18));
    transportBar->setStyleSheet(QStringLiteral(
        "QToolBar{spacing:2px; background:%1; border-top:1px solid %2;}")
        .arg(themeColors().transportBg.name(), themeColors().transportBorder.name()));

    QAction* goToStart = new QAction(iconSkipBack(), tr("Início"), this);
    goToStart->setToolTip(tr("Ir para o início (Home)"));
    goToStart->setShortcut(QKeySequence(Qt::Key_Home));
    connect(goToStart, &QAction::triggered, this, [this]() {
        m_timeline->setPlayhead(0.0);
        m_preview->seek(0.0);
        m_timeline->update();
    });
    transportBar->addAction(goToStart);

    QAction* stepBack = new QAction(iconStepBack(), tr("Voltar 1 frame"), this);
    stepBack->setToolTip(tr("Voltar 1 frame (←)"));
    connect(stepBack, &QAction::triggered, this, [this]() {
        Project* p = m_timeline->project();
        const double t = m_timeline->playhead() - 1.0 / (p ? p->fps : 30.0);
        m_timeline->setPlayhead(std::max(0.0, t));
        m_preview->seek(m_timeline->playhead());
        m_timeline->update();
    });
    transportBar->addAction(stepBack);

    transportBar->addAction(m_playAction);

    QAction* stepFwd = new QAction(iconStepFwd(), tr("Avançar 1 frame"), this);
    stepFwd->setToolTip(tr("Avançar 1 frame (→)"));
    connect(stepFwd, &QAction::triggered, this, [this]() {
        Project* p = m_timeline->project();
        const double dur = p ? p->duration() : 10.0;
        const double t = m_timeline->playhead() + 1.0 / (p ? p->fps : 30.0);
        m_timeline->setPlayhead(std::min(dur, t));
        m_preview->seek(m_timeline->playhead());
        m_timeline->update();
    });
    transportBar->addAction(stepFwd);

    QAction* goToEnd = new QAction(iconSkipFwd(), tr("Fim"), this);
    goToEnd->setToolTip(tr("Ir para o fim (End)"));
    goToEnd->setShortcut(QKeySequence(Qt::Key_End));
    connect(goToEnd, &QAction::triggered, this, [this]() {
        Project* p = m_timeline->project();
        const double dur = p ? p->duration() : 10.0;
        m_timeline->setPlayhead(dur);
        m_preview->seek(dur);
        m_timeline->update();
    });
    transportBar->addAction(goToEnd);

    m_centralLay->addWidget(transportBar);

    // Intercepta setas ←/→ globalmente via eventFilter no qApp.
    qApp->installEventFilter(this);

    m_undoStack.append(snapshotState());
    m_undoLabels.append(tr("Início"));
    m_undoIndex = 0;
    updateUndoActions();

    connect(m_timeline, &TimelineWidget::playheadChanged, m_preview, &PreviewWidget::seek);
    connect(m_preview, &PreviewWidget::playheadMoved, m_timeline, &TimelineWidget::setPlayhead);
    connect(m_preview, &PreviewWidget::stateChanged, m_timeline, &TimelineWidget::setPlaying);
    connect(m_timeline, &TimelineWidget::playPauseRequested, m_preview, &PreviewWidget::togglePlay);
    // Enter (estilo Vegas): tocar a partir da posição da agulha/ponteiro.
    connect(m_timeline, &TimelineWidget::playFromCursor, this, [this]() {
        const double t = m_timeline->cursorPos() >= 0.0
                         ? m_timeline->cursorPos()
                         : m_timeline->playhead();
        m_preview->playFrom(t);
    });
    connect(m_preview, &PreviewWidget::stateChanged, this, [this](bool playing) {
        m_playAction->setText(playing ? tr("Pausar") : tr("Reproduzir"));
        m_playAction->setIcon(playing ? iconPause() : iconPlay());
        // Durante a reprodução os thumbs ficam adiados (ver MediaCache).
        MediaCache::instance().setPlaybackActive(playing);
    });
    // Automação do mixer: gravação segue o playhead e o estado de reprodução.
    connect(m_preview, &PreviewWidget::playheadMoved, m_mixer, &MixerWidget::setPlayhead);
    connect(m_preview, &PreviewWidget::stateChanged, m_mixer, &MixerWidget::setPlaying);
    // Ao parar, consolida a automação gravada em uma única entrada de undo.
    connect(m_preview, &PreviewWidget::stateChanged, this, [this](bool playing) {
        if (playing) return;
        if (mixerHasAutomation())
            pushUndo();
    });
    connect(m_timeline, &TimelineWidget::modified, this, [this]() {
        m_preview->refreshView();
        if (m_mixer) m_mixer->refresh();
        if (m_mesa) m_mesa->refresh();
    });
    connect(m_timeline, &TimelineWidget::modified, this, &MainWindow::setModified);
    connect(m_mixer, &MixerWidget::modified, this, &MainWindow::pushUndo);
    connect(m_mixer, &MixerWidget::modified, this, &MainWindow::setModified);
    connect(m_mixer, &MixerWidget::modified, this, [this]() {
        m_preview->refreshView();
    });
    connect(m_mesa, &MesaWidget::modified, this, &MainWindow::setModified);
    connect(m_mesa, &MesaWidget::changesCommitted, this, &MainWindow::pushUndo);
    connect(m_mesa, &MesaWidget::modified, this, [this]() {
        m_preview->refreshView();
        m_timeline->update();
        m_graph->refresh();
    });
    connect(m_mesa, &MesaWidget::mesaPlayheadChanged, this, [this](double t) {
        m_timeline->setPlayhead(t);
        m_preview->seek(t);
        m_graph->setPlayhead(t);
    });
    connect(m_mesa, &MesaWidget::mesaTrackSelected, this, [this](Track* t) {
        m_graph->setMesaTrack(t);
        m_props->showMesaLayer(t ? t->id : QString());
    });
    connect(m_mesa, &MesaWidget::mesaCameraSelected, this, [this](MesaComposition* mc) {
        m_graph->setMesaCamera(mc);
        m_props->showMesaCamera(mc ? mc->id : QString());
    });
    connect(m_mesa, &MesaWidget::mesaCreateRequested, this, [this]() {
        m_timeline->criarMesa();
    });
    connect(m_mesa, &MesaWidget::mesaAddTrackRequested, this, [this]() {
        m_timeline->addTrackToMesa(m_mesa->mesaId());
    });
    // Sólido/gradiente gerados na própria Mesa (menu de contexto do canvas).
    connect(m_mesa, &MesaWidget::mesaAddSolidRequested,
            this, [this](const QString& gen, const QColor& c1, const QColor& c2) {
        m_timeline->addSolidToMesa(m_mesa->mesaId(), gen, c1, c2);
    });
    // Duplicar camada da Mesa (menu de contexto do canvas).
    connect(m_mesa, &MesaWidget::mesaDuplicateLayerRequested,
            this, [this](const QString& mesaId, const QString& trackId) {
        m_timeline->duplicateMesaTrack(mesaId, trackId);
    });
    connect(m_timeline, &TimelineWidget::mesaOpenRequested, this, [this](const QString& mesaId) {
        m_mesa->setMesaId(mesaId);
        m_mesaDock->show();
        m_mesaDock->raise();
        m_mesa->refresh();
    });
    // Tracks enviadas para a Mesa → atualiza o dock se for a Mesa aberta.
    connect(m_timeline, &TimelineWidget::mesaChanged, this, [this](const QString& mesaId) {
        if (m_mesa && m_mesa->mesaId() == mesaId)
            m_mesa->refresh();
    });
    connect(m_timeline, &TimelineWidget::mediaImported, this, [this]() {
        m_pool->refreshFromProject();
        setModified();
        statusBar()->showMessage(tr("Mídia importada por arrasto."));
    });
    connect(m_pool, &MediaPoolWidget::mediaAdded, this, &MainWindow::setModified);
    connect(m_pool, &MediaPoolWidget::mediaChanged, this, [this]() {
        m_timeline->updateScrollRanges();
        m_timeline->update();
        m_preview->refreshView();
    });
    connect(m_pool, &MediaPoolWidget::mediaToTimeline, m_timeline,
            &TimelineWidget::addMediaAtPlayhead);
    // Explorador de arquivos: importar direto para o Media Pool (duplo clique /
    // botão "Importar pasta"); arraste do explorador também funciona, pois o
    // pool aceita arquivos locais soltos sobre ele.
    connect(m_fileBrowser, &FileBrowserWidget::filesImportRequested, m_pool,
            &MediaPoolWidget::importPaths);
    // Arrasto manual da pool de mídia: feedback na timeline e soltura direta.
    // Não depende do DnD do compositor (falha em alguns ambientes/Wayland).
    connect(m_pool, &MediaPoolWidget::dragHover, m_timeline, &TimelineWidget::showDropHover);
    connect(m_pool, &MediaPoolWidget::dragHoverCleared, m_timeline,
            &TimelineWidget::hideDropHover);
    connect(m_pool, &MediaPoolWidget::mediaDropped, this,
            [this](const QStringList& ids, const QPoint& g) {
        if (m_timeline->rect().contains(m_timeline->mapFromGlobal(g)))
            m_timeline->dropMediaAt(ids, g);
    });
    connect(m_timeline, &TimelineWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_timeline, &TimelineWidget::undoLabel, this, &MainWindow::setUndoLabel);
    connect(m_timeline, &TimelineWidget::loopChanged, m_preview, &PreviewWidget::setLoopRange);
    connect(m_timeline, &TimelineWidget::loopEnabledChanged, m_preview, &PreviewWidget::setLoopEnabled);    connect(m_pool, &MediaPoolWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_pool, &MediaPoolWidget::importStarted, this, [this]() {
        statusBar()->showMessage(tr("Importando mídia…"));
    });
    connect(m_pool, &MediaPoolWidget::importProgress, this, [this](int v) {
        statusBar()->showMessage(tr("Importando mídia… (%1)").arg(v));
    });
    connect(m_pool, &MediaPoolWidget::importFinished, this, [this](int added, int invalid) {
        QString msg = tr("Importação concluída: %1 arquivo(s) adicionado(s).").arg(added);
        if (invalid > 0)
            msg += tr("  (%1 ignorado(s))").arg(invalid);
        statusBar()->showMessage(msg);
    });

    connect(m_timeline, &TimelineWidget::selectionChanged, m_pancrop, &PancropWidget::setClipId);
    connect(m_timeline, &TimelineWidget::playheadChanged, m_pancrop, &PancropWidget::setPlayhead);
    connect(m_timeline, &TimelineWidget::selectionChanged, m_graph, &GraphEditorWidget::setClipId);
    connect(m_timeline, &TimelineWidget::playheadChanged, m_graph, &GraphEditorWidget::setPlayhead);
    connect(m_timeline, &TimelineWidget::playheadChanged, m_props, &ClipPropertiesWidget::setPlayhead);
    connect(m_timeline, &TimelineWidget::playheadChanged, this, [this](double t) {
        if (m_mesa) { m_mesa->setPlayheadPosition(t); m_mesa->refresh(); }
    });
    connect(m_graph, &GraphEditorWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_graph, &GraphEditorWidget::modified, this, [this]() {
        m_timeline->update();
        m_preview->refreshView();
        m_pancrop->sync();
        setModified();
    });
    connect(m_timeline, &TimelineWidget::modified, m_graph, &GraphEditorWidget::refresh);
    connect(m_pancrop, &PancropWidget::modified, m_graph, &GraphEditorWidget::refresh);
    // Correspondência pancrop ↔ editor de curvas: ao animar uma propriedade
    // no pancrop, o editor de curvas exibe a curva correspondente.
    connect(m_pancrop, &PancropWidget::propertyEdited, this, [this](int prop) {
        GraphProp gp;
        switch (prop) {
            case PancropWidget::P_CropL: gp = GPropCropL; break;
            case PancropWidget::P_CropR: gp = GPropCropR; break;
            case PancropWidget::P_CropT: gp = GPropCropT; break;
            case PancropWidget::P_CropB: gp = GPropCropB; break;
            case PancropWidget::P_Scale: gp = GPropScale; break;
            case PancropWidget::P_PanX:  gp = GPropTx; break;
            case PancropWidget::P_PanY:  gp = GPropTy; break;
            case PancropWidget::P_Rotation: gp = GPropRotation; break;
            default: return;
        }
        m_graph->setProperty(gp);
    });
    connect(m_timeline, &TimelineWidget::pancropRequested, this, [this](const QString& id) {
        m_pancrop->setClipId(id);
        m_pancropDock->show();
        m_pancropDock->raise();
    });
    connect(m_pancrop, &PancropWidget::keyframeJump, this, [this](double t) {
        m_timeline->setPlayhead(t);
        m_preview->seek(t);
        m_pancrop->setPlayhead(t);
        m_graph->setPlayhead(t);
    });
    connect(m_graph, &GraphEditorWidget::keyframeJump, this, [this](double t) {
        m_timeline->setPlayhead(t);
        m_preview->seek(t);
        m_pancrop->setPlayhead(t);
        m_graph->setPlayhead(t);
    });
    connect(m_pancrop, &PancropWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_pancrop, &PancropWidget::modified, this, [this]() {
        m_timeline->update();
        m_preview->refreshView();
        setModified();
    });

    // Painel de Propriedades (Effect Controls): edições com undo e refresh
    // em vivo do preview, timeline, editor de curvas e canvas da Mesa.
    connect(m_props, &ClipPropertiesWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_props, &ClipPropertiesWidget::modified, this, [this]() {
        m_timeline->update();
        m_preview->refreshView();
        m_graph->refresh();
        setModified();
        if (m_mesa) m_mesa->update();
    });
    // Qualquer mudança estrutural na timeline/Mesa re-sincroniza o painel
    // (evita ponteiros pendentes e mostra valores atualizados após undo).
    connect(m_timeline, &TimelineWidget::modified, m_props, &ClipPropertiesWidget::refresh);
    connect(m_mesa, &MesaWidget::modified, m_props, &ClipPropertiesWidget::refresh);
    // "Propriedades…" no menu de contexto da timeline abre a janela do painel.
    connect(m_timeline, &TimelineWidget::propertiesRequested, this,
            [this](const QString& id) {
                showPropsWindow();
                m_props->showClip(id);
            });
    // "Propriedades da camada…" no canvas da Mesa abre a janela do painel.
    connect(m_mesa, &MesaWidget::mesaLayerPropsRequested, this,
            [this](const QString& trackId) {
                showPropsWindow();
                m_props->showMesaLayer(trackId);
            });

    updateTitle();
    statusBar()->showMessage(tr("Pronto — arraste mídia para a timeline."));

    // Barrinha de atividade: mostra quando ondas de áudio/thumbnails estão
    // sendo geradas em segundo plano (para o usuário saber que não travou).
    m_busyBar = new QProgressBar(this);
    m_busyBar->setRange(0, 0);
    m_busyBar->setFixedWidth(150);
    m_busyBar->setFixedHeight(14);
    m_busyBar->setTextVisible(false);
    m_busyBar->hide();
    statusBar()->addWidget(m_busyBar);
    connect(&MediaCache::instance(), &MediaCache::busyChanged, this, [this](bool busy) {
        m_busyBar->setVisible(busy);
        if (busy)
            statusBar()->showMessage(tr("Carregando áudio e miniaturas…"));
        else if (statusBar()->currentMessage().contains(tr("Carregando áudio")))
            statusBar()->clearMessage();
    });

    // Salvamento automático configurado na janela de boas-vindas
    m_autoSaveTimer = new QTimer(this);
    connect(m_autoSaveTimer, &QTimer::timeout, this, &MainWindow::autoSave);
    QSettings autosave;
    const int autosaveMin = qMax(1, autosave.value("autosaveMinutes", 10).toInt());
    if (autosave.value("autosaveEnabled", false).toBool()) {
        m_autoSaveTimer->setInterval(autosaveMin * 60 * 1000);
        m_autoSaveTimer->start();
    }

    // Salva o layout dos painéis um pouco depois de qualquer mudança (arrastar,
    // flutuar, mostrar/ocultar), para não perder a área de trabalho se o app
    // fechar de forma anormal antes do closeEvent().
    m_layoutSaveTimer = new QTimer(this);
    m_layoutSaveTimer->setSingleShot(true);
    m_layoutSaveTimer->setInterval(500);
    connect(m_layoutSaveTimer, &QTimer::timeout, this, &MainWindow::saveSettings);
}

void MainWindow::saveSettings() {
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("layout", saveState());
    settings.setValue("layoutVersion", kLayoutVersion);
    settings.setValue("layoutLocked", m_lockAction->isChecked());
}

void MainWindow::scheduleLayoutSave() {
    if (m_restoringSettings) return;
    if (m_layoutSaveTimer) m_layoutSaveTimer->start();
}

bool MainWindow::event(QEvent* e) {
    // Ao perder o foco (mudar de janela), agenda o salvamento: cobre o caso de
    // o app ser encerrado logo depois sem passar pelo closeEvent().
    if (e->type() == QEvent::WindowDeactivate && !m_restoringSettings)
        scheduleLayoutSave();
    return QMainWindow::event(e);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    if (e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        QWidget* fw = focusWidget();
        const bool inPancrop = fw && m_pancrop && m_pancrop->isAncestorOf(fw);
        const bool inGraph = fw && m_graph && m_graph->isAncestorOf(fw);
        if (!inPancrop && !inGraph) {
            if (ke->key() == Qt::Key_Left && !(ke->modifiers() & Qt::AltModifier)) {
                Project* p = m_timeline->project();
                const double t = m_timeline->playhead() - 1.0 / (p ? p->fps : 30.0);
                m_timeline->setPlayhead(std::max(0.0, t));
                m_preview->seek(m_timeline->playhead());
                m_timeline->update();
                return true;
            }
            if (ke->key() == Qt::Key_Right && !(ke->modifiers() & Qt::AltModifier)) {
                Project* p = m_timeline->project();
                const double dur = p ? p->duration() : 10.0;
                const double t = m_timeline->playhead() + 1.0 / (p ? p->fps : 30.0);
                m_timeline->setPlayhead(std::min(dur, t));
                m_preview->seek(m_timeline->playhead());
                m_timeline->update();
                return true;
            }
            if (ke->modifiers() & Qt::AltModifier) {
                if (ke->key() == Qt::Key_Left) {
                    m_timeline->nudgeSelected(-1);
                    return true;
                }
                if (ke->key() == Qt::Key_Right) {
                    m_timeline->nudgeSelected(1);
                    return true;
                }
            }
        }
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::restoreSettings() {
    m_restoringSettings = true;
    QSettings settings;

    // Só restaura a geometria se o array for do formato gravado pelo
    // saveGeometry() atual; dados corrompidos/legados quebravam o show().
    const QByteArray geom = settings.value("geometry").toByteArray();
    if (!geom.isEmpty() && saneGeometryArray(geom))
        restoreGeometry(geom);

    // A geometria restaurada pode vir de um monitor que não está mais
    // conectado (ex.: TV 4K). Garante que a janela nunca fique maior que a
    // tela disponível nem fora dela; se tocar em tela nenhuma, usa o padrão.
    QScreen* screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect avail = screen->availableGeometry();
        if (!avail.intersects(frameGeometry())) {
            resize(qMin(1280, avail.width()), qMin(800, avail.height()));
            move(avail.center().x() - width() / 2,
                 avail.center().y() - height() / 2);
        } else if (width() > avail.width() || height() > avail.height()) {
            resize(qMin(width(), avail.width()), qMin(height(), avail.height()));
        }
    }

    // Só restaura o arranjo dos painéis se for da versão atual do layout e
    // estiver num formato válido; estados antigos podem ter a toolbar
    // deslocada por um dock no topo.
    if (settings.value("layoutVersion").toInt() == kLayoutVersion) {
        const QByteArray state = settings.value("layout").toByteArray();
        if (!state.isEmpty() && saneLayoutArray(state))
            restoreState(state);
    }
    if (settings.contains("layoutLocked"))
        m_lockAction->setChecked(settings.value("layoutLocked").toBool());
    setDockLocked(m_lockAction->isChecked());
    m_restoringSettings = false;

    // Garante que a Mesa encontra suas tracks mesmo quando o dock é restaurado
    // como visível — autoSelectMesa() precisa rodar após restoreState().
    m_mesa->autoSelectMesa();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Se houver alterações não salvas, pergunta antes de sair.
    if (!confirmDiscardChanges()) { event->ignore(); return; }
    saveSettings();
    QMainWindow::closeEvent(event);
    QApplication::quit();
}

// Diálogo "Salvar projeto?" reutilizado ao fechar o app e ao criar/abrir um
// novo projeto. Retorna false se o usuário decidir continuar onde está.
bool MainWindow::confirmDiscardChanges() {
    if (!m_modified) return true;
    QMessageBox box(this);
    box.setWindowTitle(tr("Salvar projeto?"));
    box.setIcon(QMessageBox::Question);
    box.setText(tr("O projeto tem alterações não salvas. Deseja salvá-las antes de continuar?"));
    QPushButton* saveBtn = box.addButton(tr("Salvar"), QMessageBox::AcceptRole);
    QPushButton* discardBtn = box.addButton(tr("Descartar"), QMessageBox::DestructiveRole);
    QPushButton* cancelBtn = box.addButton(tr("Cancelar"), QMessageBox::RejectRole);
    box.setDefaultButton(saveBtn);
    box.exec();
    QAbstractButton* b = box.clickedButton();
    // Cancelar (ou fechar a caixa) interrompe a operação.
    if (!b || b == cancelBtn) return false;
    // Salvar: se o usuário cancelar a caixa de salvar, também interrompe.
    if (b == saveBtn) return saveProject();
    // Descartar segue direto para a operação.
    return true;
}

// Janela normal (não-dockável) do painel de Propriedades: abre perto do
// cursor ao ser pedida pelo menu de contexto e segue a seleção enquanto
// aberta.
void MainWindow::showPropsWindow() {
    const QPoint cur = QCursor::pos();
    m_props->show();
    if (QScreen* s = m_props->screen()) {
        const QRect avail = s->availableGeometry();
        int x = cur.x() - 16;
        int y = cur.y() - 16;
        x = qBound(avail.left(), x, avail.right() - m_props->width() + 1);
        y = qBound(avail.top(), y, avail.bottom() - m_props->height() + 1);
        m_props->move(x, y);
    }
    m_props->raise();
    m_props->activateWindow();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // Restaura geometria e arranjo dos painéis somente agora: o layout dos
    // docks só é calculado na primeira exibição, então restaurar antes do
    // show() não aplicava corretamente painéis fechados, movidos ou redimensio
    // nados. Aplica uma única vez.
    if (!m_layoutRestored) {
        m_layoutRestored = true;
        restoreSettings();
    }
    // Features de dock aplicadas antes do show() podem ser redefinidas quando
    // o Qt monta o layout dos painéis na primeira exibição. Reaplica o
    // travamento agora para o cadeado valer de verdade no início.
    setDockLocked(m_lockAction->isChecked());
}

void MainWindow::createDocks() {
    m_poolDock = new QDockWidget(tr("Central de Mídias"), this);
    m_poolDock->setObjectName(QStringLiteral("poolDock"));
    m_poolDock->setWidget(m_pool);
    m_poolDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_poolDock->setFeatures(QDockWidget::DockWidgetMovable
                            | QDockWidget::DockWidgetFloatable
                            | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::LeftDockWidgetArea, m_poolDock);

    m_timelineDock = new QDockWidget(tr("Timeline"), this);
    m_timelineDock->setObjectName(QStringLiteral("timelineDock"));
    m_timelineDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_timelineDock->setFeatures(QDockWidget::DockWidgetMovable
                                | QDockWidget::DockWidgetFloatable
                                | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
    // O widget do dock (ferramentas + timeline) é montado em createActions().

    m_pancropDock = new QDockWidget(tr("Pancrop"), this);
    m_pancropDock->setObjectName(QStringLiteral("pancropDock"));
    m_pancropDock->setWidget(m_pancrop);
    m_pancropDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_pancropDock->setFeatures(QDockWidget::DockWidgetMovable
                               | QDockWidget::DockWidgetFloatable
                               | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, m_pancropDock);
    m_pancropDock->hide();

    m_graphDock = new QDockWidget(tr("Editor de Curvas"), this);
    m_graphDock->setObjectName(QStringLiteral("graphDock"));
    m_graphDock->setWidget(m_graph);
    m_graphDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_graphDock->setFeatures(QDockWidget::DockWidgetMovable
                             | QDockWidget::DockWidgetFloatable
                             | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, m_graphDock);
    splitDockWidget(m_timelineDock, m_graphDock, Qt::Vertical);
    m_graphDock->setMinimumHeight(170);

    m_effectsDock = new QDockWidget(tr("Efeitos"), this);
    m_effectsDock->setObjectName(QStringLiteral("effectsDock"));
    m_effectsDock->setWidget(m_effects);
    m_effectsDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_effectsDock->setFeatures(QDockWidget::DockWidgetMovable
                               | QDockWidget::DockWidgetFloatable
                               | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, m_effectsDock);
    m_effectsDock->hide();

    m_expressDock = new QDockWidget(tr("Express"), this);
    m_expressDock->setObjectName(QStringLiteral("expressDock"));
    m_expressDock->setWidget(m_express);
    m_expressDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_expressDock->setFeatures(QDockWidget::DockWidgetMovable
                               | QDockWidget::DockWidgetFloatable
                               | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, m_expressDock);
    tabifyDockWidget(m_effectsDock, m_expressDock);
    m_expressDock->hide();

    m_fileBrowserDock = new QDockWidget(tr("Explorador de Arquivos"), this);
    m_fileBrowserDock->setObjectName(QStringLiteral("fileBrowserDock"));
    m_fileBrowserDock->setWidget(m_fileBrowser);
    m_fileBrowserDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_fileBrowserDock->setFeatures(QDockWidget::DockWidgetMovable
                                   | QDockWidget::DockWidgetFloatable
                                   | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::LeftDockWidgetArea, m_fileBrowserDock);
    m_fileBrowserDock->hide();

    // Mixer — dock na parte inferior, ao lado da timeline.
    m_mixer = new MixerWidget(this);
    m_mixer->setProject(&m_project);
    m_mixer->setPreview(m_preview);
    m_mixerDock = new QDockWidget(tr("Mixer"), this);
    m_mixerDock->setObjectName(QStringLiteral("mixerDock"));
    m_mixerDock->setWidget(m_mixer);
    m_mixerDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_mixerDock->setFeatures(QDockWidget::DockWidgetMovable
                             | QDockWidget::DockWidgetFloatable
                             | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::BottomDockWidgetArea, m_mixerDock);
    splitDockWidget(m_timelineDock, m_mixerDock, Qt::Vertical);
    m_mixerDock->setMinimumHeight(120);
    m_mixerDock->hide();

    // Mesa (composição 2D) — dock ao lado do preview.
    m_mesa = new MesaWidget(this);
    m_mesa->setProject(&m_project);
    m_mesaDock = new QDockWidget(tr("Mesa"), this);
    m_mesaDock->setObjectName(QStringLiteral("mesaDock"));
    m_mesaDock->setWidget(m_mesa);
    m_mesaDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_mesaDock->setFeatures(QDockWidget::DockWidgetMovable
                             | QDockWidget::DockWidgetFloatable
                             | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, m_mesaDock);
    tabifyDockWidget(m_pancropDock, m_mesaDock);
    m_mesaDock->hide();

    // Histórico de edições (undo/redo) — dock no canto inferior direito,
    // agrupado com o Mixer.
    m_histList = new QListWidget(this);
    m_histList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_histList->setMinimumWidth(180);
    connect(m_histList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        jumpToUndo(item->data(Qt::UserRole).toInt());
    });
    m_histDock = new QDockWidget(tr("Histórico de Edições"), this);
    m_histDock->setObjectName(QStringLiteral("historyDock"));
    m_histDock->setWidget(m_histList);
    m_histDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_histDock->setFeatures(QDockWidget::DockWidgetMovable
                            | QDockWidget::DockWidgetFloatable
                            | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, m_histDock);
    tabifyDockWidget(m_mixerDock, m_histDock);
    m_histDock->hide();
    updateHistoryList();

    // Analisadores de vídeo (waveform/histograma/vectorscope) — dock à direita,
    // seguindo o preview. Um timer discreto (~15 fps) copia o quadro composto;
    // a cópia é minúscula (160×90) e só acontece com o dock visível.
    m_scopes = new ScopeWidget(this);
    m_scopeMode = new QComboBox(this);
    m_scopeMode->addItem(tr("Waveform"), (int)ScopeWidget::Waveform);
    m_scopeMode->addItem(tr("Histograma"), (int)ScopeWidget::Histogram);
    m_scopeMode->addItem(tr("Vectorscope"), (int)ScopeWidget::Vectorscope);
    connect(m_scopeMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        m_scopes->setMode((ScopeWidget::Mode)m_scopeMode->itemData(idx).toInt());
        m_scopes->refreshFrom(m_preview->scopesFrame());
    });
    auto* scopeBox = new QWidget(this);
    auto* scopeLay = new QVBoxLayout(scopeBox);
    scopeLay->setContentsMargins(6, 6, 6, 6);
    scopeLay->addWidget(m_scopeMode);
    scopeLay->addWidget(m_scopes, 1);
    m_scopesDock = new QDockWidget(tr("Analisadores"), this);
    m_scopesDock->setObjectName(QStringLiteral("scopesDock"));
    m_scopesDock->setWidget(scopeBox);
    m_scopesDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_scopesDock->setFeatures(QDockWidget::DockWidgetMovable
                              | QDockWidget::DockWidgetFloatable
                              | QDockWidget::DockWidgetClosable);
    addDockWidget(Qt::RightDockWidgetArea, m_scopesDock);
    tabifyDockWidget(m_histDock, m_scopesDock);
    m_scopesDock->hide();
    auto* scopeTimer = new QTimer(this);
    scopeTimer->setInterval(70);
    connect(scopeTimer, &QTimer::timeout, this, [this]() {
        if (!m_scopesDock->isVisible()) return;
        m_scopes->refreshFrom(m_preview->scopesFrame());
    });
    scopeTimer->start();

    // Visual das barras de título dos painéis dockáveis.
    setStyleSheet(globalStyleSheet(savedTheme()));

    // Qualquer mudança de arranjo dos painéis agenda o salvamento do layout.
    for (QDockWidget* dock : {m_poolDock, m_timelineDock, m_pancropDock, m_graphDock,
                              m_effectsDock, m_expressDock, m_fileBrowserDock,
                              m_mixerDock, m_mesaDock, m_histDock, m_scopesDock}) {
        connect(dock, &QDockWidget::topLevelChanged, this, &MainWindow::scheduleLayoutSave);
        connect(dock, &QDockWidget::visibilityChanged, this, &MainWindow::scheduleLayoutSave);
    }
}

void MainWindow::createActions() {
    const auto stdIcon = [this](QStyle::StandardPixmap sp) {
        return style()->standardIcon(sp);
    };

    QAction* addMedia = new QAction(tr("Importar mídia…"), this);
    addMedia->setShortcut(appKey("import", QKeySequence("Ctrl+I")));
    addMedia->setIcon(iconImport());
    addMedia->setToolTip(tr("Importar mídia… (Ctrl+I)"));
    connect(addMedia, &QAction::triggered, m_pool, &MediaPoolWidget::addFiles);

    QAction* exportAct = new QAction(tr("Exportar…"), this);
    exportAct->setShortcut(appKey("export", QKeySequence("Ctrl+E")));
    exportAct->setIcon(iconExport());
    exportAct->setToolTip(tr("Exportar vídeo… (Ctrl+E)"));
    connect(exportAct, &QAction::triggered, this, &MainWindow::exportVideo);
    QAction* queueAct = new QAction(tr("Fila de render…"), this);
    queueAct->setToolTip(tr("Várias exportações em sequência (formatos/resoluções diferentes)"));
    connect(queueAct, &QAction::triggered, this, [this]() {
        RenderQueueDialog dlg(&m_project, this);
        dlg.exec();
    });

    QAction* edlExportAct = new QAction(tr("Exportar EDL…"), this);
    edlExportAct->setToolTip(tr("Exportar apenas cortes como EDL CMX3600 (compatível com Davinci, Premiere)."));
    connect(edlExportAct, &QAction::triggered, this, &MainWindow::exportEdl);

    QAction* edlImportAct = new QAction(tr("Importar EDL…"), this);
    edlImportAct->setToolTip(tr("Importar EDL CMX3600 em um novo projeto (apenas cortes)."));
    connect(edlImportAct, &QAction::triggered, this, &MainWindow::importEdl);

    QAction* quit = new QAction(tr("Sair"), this);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &MainWindow::close);

    m_playAction = new QAction(tr("Reproduzir"), this);
    m_playAction->setShortcut(appKey("play", QKeySequence(Qt::Key_Space)));
    m_playAction->setIcon(iconPlay());
    m_playAction->setToolTip(tr("Reproduzir/Pausar (Espaço)"));
    connect(m_playAction, &QAction::triggered, m_preview, &PreviewWidget::togglePlay);

    // Shuttle JKL (estilo Vegas): L avança, J retrocede, K pausa; repetir
    // acelera (1x→2x→4x). Contexto WidgetWithChildrenShortcut nas duas
    // áreas de edição para não roubar o L/K do MesaCanvas (toggle de camadas
    // e autokey) quando ele estiver focado.
    QAction* shuttleRev = new QAction(tr("Retroceder (JKL)"), this);
    shuttleRev->setShortcut(appKey("shuttleRev", QKeySequence(Qt::Key_J)));
    shuttleRev->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    shuttleRev->setToolTip(tr("Reproduzir para trás (J); repetir acelera"));
    connect(shuttleRev, &QAction::triggered, m_preview, [this]() { m_preview->shuttle(-1); });

    QAction* shuttleFwd = new QAction(tr("Avançar (JKL)"), this);
    shuttleFwd->setShortcut(appKey("shuttleFwd", QKeySequence(Qt::Key_L)));
    shuttleFwd->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    shuttleFwd->setToolTip(tr("Reproduzir para frente (L); repetir acelera"));
    connect(shuttleFwd, &QAction::triggered, m_preview, [this]() { m_preview->shuttle(1); });

    QAction* shuttlePause = new QAction(tr("Pausar (JKL)"), this);
    shuttlePause->setShortcut(appKey("shuttlePause", QKeySequence(Qt::Key_K)));
    shuttlePause->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    shuttlePause->setToolTip(tr("Pausar (K)"));
    connect(shuttlePause, &QAction::triggered, m_preview, [this]() { m_preview->shuttle(0); });
    if (m_timeline) {
        m_timeline->addAction(shuttleRev);
        m_timeline->addAction(shuttleFwd);
        m_timeline->addAction(shuttlePause);
    }
    m_preview->addAction(shuttleRev);
    m_preview->addAction(shuttleFwd);
    m_preview->addAction(shuttlePause);

    QAction* cutAction = new QAction(tr("Dividir no playhead"), this);
    cutAction->setShortcut(appKey("cut", QKeySequence(Qt::Key_S)));
    cutAction->setIcon(iconScissors());
    cutAction->setToolTip(tr("Dividir clipe no playhead (S)"));
    connect(cutAction, &QAction::triggered, m_timeline, &TimelineWidget::cutAtPlayhead);

    QAction* deleteAction = new QAction(tr("Excluir clipe"), this);
    deleteAction->setShortcut(appKey("delete", QKeySequence(Qt::Key_Delete)));
    deleteAction->setIcon(stdIcon(QStyle::SP_TrashIcon));
    deleteAction->setToolTip(tr("Excluir faixas selecionadas ou, se não houver, os clipes selecionados (Delete)"));
    connect(deleteAction, &QAction::triggered, this, [this]() {
        if (m_mesa && m_mesa->hasSelectedKeyframes()) {
            m_mesa->deleteSelectedKfs();
        } else {
            m_timeline->deleteSelection();
        }
    });

    m_undoAction = new QAction(tr("Desfazer"), this);
    m_undoAction->setShortcut(appKey("undo", QKeySequence::Undo));
    m_undoAction->setIcon(stdIcon(QStyle::SP_ArrowBack));
    m_undoAction->setToolTip(tr("Desfazer (Ctrl+Z)"));
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undo);

    m_redoAction = new QAction(tr("Refazer"), this);
    m_redoAction->setShortcut(appKey("redo", QKeySequence("Ctrl+Shift+Z")));
    m_redoAction->setIcon(stdIcon(QStyle::SP_ArrowForward));
    m_redoAction->setToolTip(tr("Refazer (Ctrl+Shift+Z)"));
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::redo);
    {
        QAction* redo2 = new QAction(this);
        redo2->setShortcut(QKeySequence("Ctrl+Y"));
        connect(redo2, &QAction::triggered, this, &MainWindow::redo);
        addAction(redo2);
    }

    QAction* newAction = new QAction(tr("Novo"), this);
    newAction->setShortcut(appKey("new", QKeySequence::New));
    newAction->setIcon(stdIcon(QStyle::SP_FileIcon));
    newAction->setToolTip(tr("Novo projeto (Ctrl+N)"));
    connect(newAction, &QAction::triggered, this, &MainWindow::newProject);

    QAction* openAction = new QAction(tr("Abrir…"), this);
    openAction->setShortcut(appKey("open", QKeySequence::Open));
    openAction->setIcon(stdIcon(QStyle::SP_DirOpenIcon));
    openAction->setToolTip(tr("Abrir projeto… (Ctrl+O)"));
    connect(openAction, &QAction::triggered, this, &MainWindow::openProject);

    m_saveAction = new QAction(tr("Salvar"), this);
    m_saveAction->setShortcut(appKey("save", QKeySequence::Save));
    m_saveAction->setIcon(stdIcon(QStyle::SP_DialogSaveButton));
    m_saveAction->setToolTip(tr("Salvar projeto (Ctrl+S)"));
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::saveProject);

    m_saveAsAction = new QAction(tr("Salvar como…"), this);
    m_saveAsAction->setShortcut(appKey("saveas", QKeySequence("Ctrl+Shift+S")));
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);

    QAction* settingsAction = new QAction(tr("Configurações do projeto…"), this);
    connect(settingsAction, &QAction::triggered, this, &MainWindow::projectSettings);

    // Reabre a janela inicial (boas-vindas) sem fechar o editor.
    QAction* homeAction = new QAction(tr("Janela inicial"), this);
    homeAction->setToolTip(tr("Abrir a janela inicial (novo projeto / recentes)"));
    connect(homeAction, &QAction::triggered, this, &MainWindow::showWelcomeWindow);

    QMenu* fileMenu = menuBar()->addMenu(tr("&Arquivo"));
    fileMenu->addAction(newAction);
    fileMenu->addAction(openAction);
    fileMenu->addAction(homeAction);
    fileMenu->addAction(m_saveAction);
    fileMenu->addAction(m_saveAsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(addMedia);
    fileMenu->addSeparator();
    fileMenu->addAction(exportAct);
    fileMenu->addAction(queueAct);
    fileMenu->addSeparator();
    fileMenu->addAction(edlExportAct);
    fileMenu->addAction(edlImportAct);
    fileMenu->addSeparator();
    QAction* stillAct = new QAction(tr("Exportar quadro atual (PNG)…"), this);
    stillAct->setShortcut(QKeySequence("Ctrl+Shift+F"));
    stillAct->setToolTip(tr("Salva o quadro composto no playhead como imagem PNG "
                            "(a mesma imagem final do monitor)."));
    connect(stillAct, &QAction::triggered, this, [this]() {
        const QImage img = m_preview->compositeFrame();
        if (img.isNull()) {
            QMessageBox::information(this, tr("Exportar quadro"),
                                     tr("Não há quadro para exportar neste momento."));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Exportar quadro atual"),
            SettingsDialog::exportDefaultDir() + QStringLiteral("/quadro.png"),
            tr("Imagem PNG (*.png)"));
        if (path.isEmpty()) return;
        if (!img.save(path, "PNG"))
            QMessageBox::warning(this, tr("Exportar quadro"),
                                 tr("Não foi possível salvar a imagem."));
    });
    fileMenu->addAction(stillAct);
    fileMenu->addSeparator();
    fileMenu->addAction(quit);

    QMenu* editMenu = menuBar()->addMenu(tr("&Editar"));
    editMenu->addAction(m_undoAction);
    editMenu->addAction(m_redoAction);
    editMenu->addSeparator();
    editMenu->addAction(cutAction);
    editMenu->addAction(deleteAction);
    editMenu->addSeparator();
    QAction* delFrontAction = new QAction(tr("Excluir clipe anterior (D)"), this);
    delFrontAction->setShortcut(QKeySequence(Qt::Key_D));
    connect(delFrontAction, &QAction::triggered, m_timeline, &TimelineWidget::deleteClipBeforePlayhead);
    editMenu->addAction(delFrontAction);
    QAction* delBackAction = new QAction(tr("Excluir clipe posterior (F)"), this);
    delBackAction->setShortcut(QKeySequence(Qt::Key_F));
    connect(delBackAction, &QAction::triggered, m_timeline, &TimelineWidget::deleteClipAfterPlayhead);
    editMenu->addAction(delBackAction);

    QMenu* projMenu = menuBar()->addMenu(tr("&Projeto"));
    projMenu->addAction(settingsAction);

    QMenu* playMenu = menuBar()->addMenu(tr("&Reproduzir"));
    playMenu->addAction(m_playAction);
    playMenu->addSeparator();
    QAction* loopInAction = new QAction(tr("Marcar início do loop"), this);
    loopInAction->setShortcut(QKeySequence("Alt+["));
    connect(loopInAction, &QAction::triggered, m_timeline, &TimelineWidget::setLoopInAtPlayhead);
    playMenu->addAction(loopInAction);
    QAction* loopOutAction = new QAction(tr("Marcar fim do loop"), this);
    loopOutAction->setShortcut(QKeySequence("Alt+]"));
    connect(loopOutAction, &QAction::triggered, m_timeline, &TimelineWidget::setLoopOutAtPlayhead);
    playMenu->addAction(loopOutAction);
    QAction* clearLoopAction = new QAction(tr("Limpar região de loop"), this);
    connect(clearLoopAction, &QAction::triggered, m_timeline, &TimelineWidget::clearLoop);
    playMenu->addAction(clearLoopAction);

    m_lockAction = new QAction(padlockIcon(false), tr("Destravar layout"), this);
    m_lockAction->setCheckable(true);
    m_lockAction->setChecked(false);
    m_lockAction->setShortcut(QKeySequence("Ctrl+L"));
    m_lockAction->setToolTip(tr("Travar/destravar o layout dos painéis (Ctrl+L)"));
    connect(m_lockAction, &QAction::toggled, this, [this](bool locked) {
        setDockLocked(locked);
        m_lockAction->setText(locked ? tr("Travar layout") : tr("Destravar layout"));
        m_lockAction->setIcon(padlockIcon(locked));
        if (m_restoringSettings) return;
        // Persiste o arranjo dos painéis na hora: o usuário espera que
        // "Travar" fixe o layout atual, não só no fechamento do app.
        saveSettings();
        statusBar()->showMessage(locked ? tr("Layout travado e salvo.")
                                        : tr("Layout destravado — arraste os painéis para reorganizar."));
    });

    QMenu* viewMenu = menuBar()->addMenu(tr("&Exibir"));
    viewMenu->addAction(m_poolDock->toggleViewAction());
    viewMenu->addAction(m_timelineDock->toggleViewAction());
    viewMenu->addAction(m_pancropDock->toggleViewAction());
    viewMenu->addAction(m_graphDock->toggleViewAction());
    viewMenu->addAction(m_effectsDock->toggleViewAction());
    viewMenu->addAction(m_expressDock->toggleViewAction());
    viewMenu->addAction(m_fileBrowserDock->toggleViewAction());
    viewMenu->addAction(m_mixerDock->toggleViewAction());
    viewMenu->addAction(m_mesaDock->toggleViewAction());
    viewMenu->addAction(m_histDock->toggleViewAction());
    viewMenu->addAction(m_scopesDock->toggleViewAction());
    viewMenu->addSeparator();

    // Preview externo: janela própria (segundo monitor) com o mesmo sinal de
    // vídeo do monitor principal, sem overlays. F11 ou duplo-clique = tela cheia.
    m_monitorAction = new QAction(tr("Janela de preview externo"), this);
    m_monitorAction->setCheckable(true);
    m_monitorAction->setToolTip(tr("Abre o preview em uma janela própria — arraste para "
                                   "um segundo monitor e use F11 para tela cheia."));
    connect(m_monitorAction, &QAction::toggled, this, [this](bool on) {
        if (on) {
            if (!m_monitor) {
                m_monitor = new PreviewMonitor;
                connect(m_monitor, &QObject::destroyed, this, [this]() {
                    m_monitor = nullptr;
                    if (m_monitorAction) m_monitorAction->setChecked(false);
                });
            }
            m_monitor->setFrame(m_preview->compositeFrame());
            m_monitor->show();
            m_monitor->raise();
        } else if (m_monitor) {
            m_monitor->close(); // WA_DeleteOnClose → destroyed → desmarca a ação
        }
    });
    viewMenu->addAction(m_monitorAction);
    viewMenu->addSeparator();
    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(33); // ~30 Hz: basta para acompanhar o preview
    connect(m_monitorTimer, &QTimer::timeout, this, [this]() {
        if (m_monitor && m_monitor->isVisible())
            m_monitor->setFrame(m_preview->compositeFrame());
    });
    m_monitorTimer->start();

    viewMenu->addAction(m_lockAction);

    QAction* appSettingsAction = new QAction(tr("Configurações do app…"), this);
    appSettingsAction->setShortcut(QKeySequence("Ctrl+,"));
    appSettingsAction->setToolTip(tr("Abrir as configurações do aplicativo"));
    connect(appSettingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    QMenu* cfgMenu = menuBar()->addMenu(tr("&Configurações"));
    QMenu* helpMenu = menuBar()->addMenu(tr("&Ajuda"));
    QAction* aboutAction = helpMenu->addAction(tr("Sobre o Pierrot…"));
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox box(this);
        box.setWindowTitle(tr("Sobre o Pierrot"));
        box.setIcon(QMessageBox::Information);
        box.setTextFormat(Qt::RichText);
        box.setText(
            tr("<b>Pierrot</b> — editor de vídeo de código aberto<br>"
               "Versão ") +
            QString::fromLatin1(PIERROT_VERSION) +
            tr("<br><br>"
               "sempre quis migrar para o linux, mas a falta de editor sempre me "
               "fazia voltar ao windows... 0s editores que existiam nas lojas não "
               "respondiam o estilo de edição que eu fazia. comecei esse projeto para "
               "ser um programa útil para mim, mas deve ter outros editores como eu, "
               "então deixei ele de código aberto para que todos possam usar.<br><br>"
               "<a href='https://github.com/theinkspoty/Pierrot'>github.com/theinkspoty/Pierrot</a>"));
        box.setTextInteractionFlags(Qt::TextBrowserInteraction);
        box.exec();
    });
    cfgMenu->addAction(appSettingsAction);

    // As ferramentas da timeline ficam ancoradas acima da própria timeline,
    // dentro do dock, em vez de na barra superior da janela.
    auto* tlContainer = new QWidget;
    auto* tlLay = new QVBoxLayout(tlContainer);
    tlLay->setContentsMargins(0, 0, 0, 0);
    tlLay->setSpacing(0);

    QToolBar* toolTb = new QToolBar(tr("Ferramentas da timeline"), tlContainer);
    toolTb->setMovable(false);
    toolTb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    const QStringList toolNames = {
        tr("Selecionar (0)"), tr("Mover (M)"), tr("Tesoura (R)"),
        tr("Envelope (E)"), tr("Lupa (Z)"),
        tr("Ripple (B)"), tr("Rolamento (N)"), tr("Deslizar (Y)"),
        tr("Escorregar (Ctrl+U)"), tr("Esticar Velocidade (W)")
    };
    const QList<QIcon> toolIcons = {
        iconCursor(), iconMove(), iconRazor(), iconEnvelope(), iconZoom(),
        iconRipple(), iconRolling(), iconSlip(), iconSlide(), iconRateStretch()
    };
    const QList<QKeySequence> toolKeys = {
        QKeySequence(Qt::Key_0), QKeySequence(Qt::Key_M), QKeySequence(Qt::Key_R),
        QKeySequence(Qt::Key_E), QKeySequence(Qt::Key_Z),
        QKeySequence(Qt::Key_B), QKeySequence(Qt::Key_N), QKeySequence(Qt::Key_Y),
        QKeySequence(Qt::CTRL | Qt::Key_U), QKeySequence(Qt::Key_W)
    };
    QActionGroup* toolGroup = new QActionGroup(this);
    for (int i = 0; i < toolNames.size(); ++i) {
        QAction* a = new QAction(toolIcons[i], toolNames[i], this);
        a->setCheckable(true);
        // A Tesoura (i==2) não ganha atalho fixo: a tecla (default R) é tratada
        // no TimelineWidget em modo hold-to-use (segurar ativa, soltar restaura).
        if (i != 2)
            a->setShortcut(appKey(("tool" + QString::number(i)).toLatin1().constData(), toolKeys[i]));
        a->setToolTip(toolNames[i]);
        a->setChecked(i == 0);
        toolGroup->addAction(a);
        toolTb->addAction(a);
        m_toolActions.append(a);
        connect(a, &QAction::triggered, this, [this, i]() { m_timeline->setTool(i); });
    }
    connect(m_timeline, &TimelineWidget::toolChanged, this, [this](int t) {
        if (t >= 0 && t < m_toolActions.size()) {
            QSignalBlocker blocker(m_toolActions[t]);
            m_toolActions[t]->setChecked(true);
        }
    });
    toolTb->addSeparator();
    m_snapAction = new QAction(iconMagnet(), tr("Snap (ímã)"), this);
    m_snapAction->setCheckable(true);
    m_snapAction->setChecked(true);
    m_snapAction->setToolTip(tr("Encaixar no grid e nas bordas dos clipes"));
    connect(m_snapAction, &QAction::triggered, m_timeline, &TimelineWidget::setSnap);
    toolTb->addAction(m_snapAction);
    toolTb->addSeparator();
    QAction* clearLoopTb = new QAction(iconLoopClear(), tr("Limpar loop"), this);
    clearLoopTb->setToolTip(tr("Limpar região de loop"));
    connect(clearLoopTb, &QAction::triggered, m_timeline, &TimelineWidget::clearLoop);
    toolTb->addAction(clearLoopTb);
    QAction* rippleTb = new QAction(iconRippleDelete(), tr("Ripple"), this);
    rippleTb->setCheckable(true);
    rippleTb->setChecked(SettingsDialog::rippleDeleteEnabled());
    rippleTb->setToolTip(tr("Fechar o vão automaticamente ao excluir (ripple). "
                            "Desligue para deixar o espaço vazio."));
    connect(rippleTb, &QAction::toggled, this, [](bool on) {
        QSettings s;
        s.setValue("timelineRippleDelete", on);
    });
    toolTb->addAction(rippleTb);
    QAction* styleAct = new QAction(iconTrackStyle(), tr("Estilo"), this);
    styleAct->setToolTip(tr("Estilo das faixas: minimizada, normal ou grande (experimental)"));
    connect(styleAct, &QAction::triggered, m_timeline, &TimelineWidget::showTrackPresetMenu);
    toolTb->addAction(styleAct);
    toolTb->addSeparator();
    // Botões de grid e régua (estilo Vegas).
    QAction* gridAct = new QAction(iconGrid(), tr("Grid"), this);
    gridAct->setCheckable(true);
    gridAct->setChecked(true);
    gridAct->setToolTip(tr("Mostrar/ocultar grade vertical na timeline"));
    connect(gridAct, &QAction::toggled, m_timeline, &TimelineWidget::setGridVisible);
    toolTb->addAction(gridAct);
    QAction* rulerAct = new QAction(iconRuler(), tr("Régua"), this);
    rulerAct->setCheckable(true);
    rulerAct->setChecked(true);
    rulerAct->setToolTip(tr("Mostrar/ocultar régua de tempo na timeline"));
    connect(rulerAct, &QAction::toggled, m_timeline, &TimelineWidget::setRulerVisible);
    toolTb->addAction(rulerAct);

    tlLay->addWidget(toolTb);

    tlLay->addWidget(m_timeline, 1);
    m_timelineDock->setWidget(tlContainer);
}

void MainWindow::setDockLocked(bool locked) {
    for (QDockWidget* dock : {m_poolDock, m_timelineDock, m_pancropDock, m_graphDock,
                              m_effectsDock, m_expressDock, m_fileBrowserDock,
                              m_mixerDock, m_mesaDock, m_histDock, m_scopesDock}) {
        if (locked) {
            if (!m_originalFeatures.contains(dock))
                m_originalFeatures.insert(dock, dock->features());
            dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        } else {
            dock->setFeatures(
                m_originalFeatures.value(dock, QDockWidget::DockWidgetClosable |
                                                   QDockWidget::DockWidgetMovable |
                                                   QDockWidget::DockWidgetFloatable));
        }
    }
}

QIcon MainWindow::padlockIcon(bool locked) const {
    const qreal dpr = devicePixelRatioF();
    QPixmap pm(qRound(24 * dpr), qRound(24 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor c = palette().color(QPalette::WindowText);

    p.setPen(QPen(c, 1.6));
    p.setBrush(locked ? QBrush(c) : QBrush());
    p.drawRoundedRect(QRectF(5.5, 10.5, 13, 10), 2.0, 2.0);

    if (locked) {
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Window));
        p.drawEllipse(QPointF(12, 15.5), 1.8, 1.8);
        p.drawRect(QRectF(11.4, 16.0, 1.2, 2.0));
    } else {
        p.setPen(QPen(c, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 15.5), 1.6, 1.6);
    }

    QPen shacklePen(c, 2.2);
    shacklePen.setCapStyle(Qt::RoundCap);
    p.setPen(shacklePen);
    p.setBrush(Qt::NoBrush);
    const QRectF shackleRect(8.0, 4.5, 8.0, 9.0);
    if (locked)
        p.drawArc(shackleRect, 180 * 16, -180 * 16);
    else
        p.drawArc(shackleRect, 200 * 16, -140 * 16);

    p.end();
    return QIcon(pm);
}

QIcon MainWindow::makeIcon(const std::function<void(QPainter&, const QColor&)>& draw) const {
    const qreal dpr = devicePixelRatioF();
    QPixmap pm(qRound(32 * dpr), qRound(32 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor fg = palette().color(QPalette::WindowText);
    draw(p, fg);
    p.end();
    return QIcon(pm);
}

QIcon MainWindow::iconCursor() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF arrow;
        arrow << QPointF(5, 2) << QPointF(5, 16) << QPointF(8, 13)
              << QPointF(13, 21) << QPointF(16, 19) << QPointF(12, 11)
              << QPointF(17, 11);
        p.drawPolygon(arrow);
    });
}

QIcon MainWindow::iconMove() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        // Track Select do Premiere: trilho de filme perfurado + seta à direita.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRect(QRectF(6, 6, 22, 3.2));
        p.drawRect(QRectF(6, 13.8, 22, 3.2));
        const double xs[] = {8.5, 13.5, 18.5, 23.5};
        for (double x : xs)
            p.drawRect(QRectF(x, 9, 3.2, 6.5));
        QPolygonF head;
        head << QPointF(19, 19) << QPointF(19, 28) << QPointF(27, 23.5);
        p.drawPolygon(head);
        p.drawRect(QRectF(11, 20.5, 8, 3.4));
    });
}

QIcon MainWindow::iconScissors() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(8, 20), QPointF(25, 6));
        p.drawLine(QPointF(24, 20), QPointF(7, 6));
        p.drawEllipse(QPointF(8, 20), 3.5, 3.5);
        p.drawEllipse(QPointF(24, 20), 3.5, 3.5);
    });
}

QIcon MainWindow::iconRazor() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        // Lâmina diagonal clássica do Premiere (Razor) com cabo.
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(c);
        QPolygonF blade;
        blade << QPointF(8.5, 9) << QPointF(11.5, 6.5)
              << QPointF(27.5, 22.5) << QPointF(24.5, 25);
        p.drawPolygon(blade);
        p.drawRoundedRect(QRectF(5, 12, 4, 12), 2, 2);
    });
}

QIcon MainWindow::iconEnvelope() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 1.8);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(5, 22);
        path.cubicTo(9, 9, 17, 28, 27, 8);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        const QPointF nodes[] = {QPointF(5, 22), QPointF(17, 19), QPointF(27, 8)};
        for (const QPointF& pt : nodes) {
            QPolygonF d;
            d << pt + QPointF(0, -3) << pt + QPointF(3, 0)
              << pt + QPointF(0, 3) << pt + QPointF(-3, 0);
            p.drawPolygon(d);
        }
    });
}

QIcon MainWindow::iconZoom() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        // Lupa do Premiere: lente vazia (sem "+") + cabo diagonal.
        QPen pen(c, 2.6);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 13), 7.5, 7.5);
        p.drawLine(QPointF(17.5, 18.5), QPointF(27, 28));
    });
}

QIcon MainWindow::iconRipple() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        // Ripple do Premiere: três ondulações verticais.
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const double cx[] = {9.5, 16.0, 22.5};
        for (double xc : cx) {
            QPolygonF w;
            const double amp[7] = {1.8, -1.8, 1.8, -1.8, 1.8, -1.8, 1.8};
            for (int k = 0; k < 7; ++k)
                w << QPointF(xc + amp[k], 8.0 + k * 2.45);
            p.drawPolyline(w);
        }
    });
}

QIcon MainWindow::iconRolling() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        // Rolling do Premiere: setas opostas (→|←) com divisória central.
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(c);
        p.drawLine(QPointF(5, 16), QPointF(14, 16));
        QPolygonF l;
        l << QPointF(11, 12.5) << QPointF(15.5, 16) << QPointF(11, 19.5);
        p.drawPolygon(l);
        p.drawLine(QPointF(27, 16), QPointF(18, 16));
        QPolygonF r;
        r << QPointF(21, 12.5) << QPointF(16.5, 16) << QPointF(21, 19.5);
        p.drawPolygon(r);
        p.setPen(QPen(c, 1.4));
        p.drawLine(QPointF(16, 9), QPointF(16, 23));
    });
}

QIcon MainWindow::iconSlip() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        // Slip do Premiere: quadro de filme com seta deslizando ao centro.
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(6, 7, 20, 18));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF head;
        head << QPointF(18, 13.5) << QPointF(18, 18.5) << QPointF(25, 16);
        p.drawPolygon(head);
        p.drawRect(QRectF(9, 14.6, 8, 2.8));
    });
}

QIcon MainWindow::iconSlide() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        // Slide do Premiere: quadro de filme com setas em cima e embaixo.
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(7, 10, 18, 12));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF top;
        top << QPointF(13, 6.5) << QPointF(13, 11) << QPointF(21, 8.75);
        p.drawPolygon(top);
        p.drawRect(QRectF(8, 7.8, 5.5, 1.9));
        QPolygonF bot;
        bot << QPointF(13, 21) << QPointF(13, 25.5) << QPointF(21, 23.25);
        p.drawPolygon(bot);
        p.drawRect(QRectF(8, 22.3, 5.5, 1.9));
    });
}

QIcon MainWindow::iconRateStretch() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        // Rate Stretch do Premiere: relógio + seta de velocidade.
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(11, 16), 7, 7);
        p.drawLine(QPointF(11, 16), QPointF(11, 10.5));
        p.drawLine(QPointF(11, 16), QPointF(15.5, 16));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRect(QRectF(20, 13.7, 4.5, 4.6));
        QPolygonF arrow;
        arrow << QPointF(24, 12) << QPointF(24, 20) << QPointF(29.5, 16);
        p.drawPolygon(arrow);
    });
}

QIcon MainWindow::iconMagnet() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPainterPath path;
        path.moveTo(7, 24);
        path.lineTo(7, 12);
        path.arcTo(QRectF(7, 3, 18, 18), 180, -180);
        path.lineTo(25, 24);
        QPen pen(c, 2.2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(4, 24, 6, 4), 1, 1);
        p.drawRoundedRect(QRectF(22, 24, 6, 4), 1, 1);
    });
}

QIcon MainWindow::iconImport() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(c);
        p.drawLine(QPointF(16, 4), QPointF(16, 14));
        QPolygonF head;
        head << QPointF(16, 19) << QPointF(10, 13) << QPointF(22, 13);
        p.drawPolygon(head);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(6, 19), QPointF(6, 25));
        p.drawLine(QPointF(6, 25), QPointF(26, 25));
        p.drawLine(QPointF(26, 25), QPointF(26, 19));
        p.drawLine(QPointF(6, 19), QPointF(11, 19));
        p.drawLine(QPointF(21, 19), QPointF(26, 19));
    });
}

QIcon MainWindow::iconExport() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(c);
        p.drawLine(QPointF(16, 18), QPointF(16, 7));
        QPolygonF head;
        head << QPointF(16, 3) << QPointF(10, 9) << QPointF(22, 9);
        p.drawPolygon(head);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(6, 21), QPointF(6, 27));
        p.drawLine(QPointF(6, 27), QPointF(26, 27));
        p.drawLine(QPointF(26, 27), QPointF(26, 21));
    });
}

QIcon MainWindow::iconPlay() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF tri;
        tri << QPointF(9, 6) << QPointF(9, 22) << QPointF(22, 14);
        p.drawPolygon(tri);
    });
}

QIcon MainWindow::iconPause() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(9.5, 6, 3.5, 16), 1, 1);
        p.drawRoundedRect(QRectF(17.5, 6, 3.5, 16), 1, 1);
    });
}

QIcon MainWindow::iconSkipBack() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF t1;
        t1 << QPointF(13, 7) << QPointF(13, 21) << QPointF(5, 14);
        p.drawPolygon(t1);
        QPolygonF t2;
        t2 << QPointF(22, 7) << QPointF(22, 21) << QPointF(14, 14);
        p.drawPolygon(t2);
    });
}

QIcon MainWindow::iconSkipFwd() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF t1;
        t1 << QPointF(10, 7) << QPointF(10, 21) << QPointF(18, 14);
        p.drawPolygon(t1);
        QPolygonF t2;
        t2 << QPointF(19, 7) << QPointF(19, 21) << QPointF(27, 14);
        p.drawPolygon(t2);
    });
}

QIcon MainWindow::iconStepBack() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(9.5, 6.5, 2.2, 15), 1, 1);
        QPolygonF tri;
        tri << QPointF(19, 7) << QPointF(19, 21) << QPointF(11, 14);
        p.drawPolygon(tri);
    });
}

QIcon MainWindow::iconStepFwd() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF tri;
        tri << QPointF(10, 7) << QPointF(10, 21) << QPointF(19, 14);
        p.drawPolygon(tri);
        p.drawRoundedRect(QRectF(20.5, 6.5, 2.2, 15), 1, 1);
    });
}

QIcon MainWindow::iconLoopClear() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 1.8);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath br;
        br.moveTo(4, 21);
        br.lineTo(4, 9);
        br.lineTo(10, 9);
        p.drawPath(br);
        QPainterPath br2;
        br2.moveTo(28, 21);
        br2.lineTo(28, 9);
        br2.lineTo(22, 9);
        p.drawPath(br2);
        QPen xpen(c, 2.2);
        xpen.setCapStyle(Qt::RoundCap);
        p.setPen(xpen);
        p.drawLine(QPointF(13, 12), QPointF(20, 19));
        p.drawLine(QPointF(20, 12), QPointF(13, 19));
    });
}

QIcon MainWindow::iconRippleDelete() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 1.8);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(9, 9, 14, 3), 1, 1);
        QPainterPath body;
        body.moveTo(10.5, 13);
        body.lineTo(12, 26);
        body.lineTo(20, 26);
        body.lineTo(21.5, 13);
        p.drawPath(body);
        p.drawLine(QPointF(14, 16), QPointF(14, 23));
        p.drawLine(QPointF(18, 16), QPointF(18, 23));
    });
}

QIcon MainWindow::iconTrackStyle() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 1.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(7, 6, 18, 4), 1, 1);
        p.drawRoundedRect(QRectF(7, 12.5, 18, 5), 1, 1);
        p.drawRoundedRect(QRectF(7, 20.5, 18, 7), 1, 1);
    });
}

QIcon MainWindow::iconGrid() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 1.2);
        pen.setStyle(Qt::DotLine);
        p.setPen(pen);
        for (int x = 8; x <= 24; x += 4)
            p.drawLine(QPointF(x, 6), QPointF(x, 26));
        for (int y = 12; y <= 20; y += 8)
            p.drawLine(QPointF(6, y), QPointF(26, y));
    });
}

QIcon MainWindow::iconRuler() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 1.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(5, 21), QPointF(27, 21));
        for (int x = 7; x <= 25; x += 2) {
            const int h = (x % 4 == 3) ? 8 : 4;
            p.drawLine(QPointF(x, 21), QPointF(x, 21 - h));
        }
    });
}

void MainWindow::pushUndo() {
    if (m_undoIndex < m_undoStack.size() - 1) {
        m_undoStack.resize(m_undoIndex + 1);
        m_undoLabels.resize(m_undoIndex + 1);
    }
    m_undoStack.append(snapshotState());
    m_undoLabels.append(m_pendingUndoLabel.isEmpty() ? tr("Edição") : m_pendingUndoLabel);
    m_pendingUndoLabel.clear();
    m_undoIndex = m_undoStack.size() - 1;
    while (m_undoStack.size() > 60) {
        m_undoStack.removeAt(0);
        m_undoLabels.removeAt(0);
        --m_undoIndex;
    }
    setModified();
    updateHistoryList();
}

void MainWindow::setUndoLabel(const QString& label) {
    // Guarda o RÓTULO, não o snapshot: o pushUndo é emitido pelo widget logo
    // em seguida (via editStart) e é quem agrega a entrada.
    m_pendingUndoLabel = label;
}

// Serializa o projeto (JSON compacto + compressão). A pilha de undo guarda
// isso em vez de cópias em memória do Project: muito menos RAM em projetos
// grandes, ao custo de alguns ms por edição.
QByteArray MainWindow::snapshotState() const {
    return qCompress(QJsonDocument(m_project.toJson()).toJson(QJsonDocument::Compact), 6);
}

void MainWindow::restoreSnapshot(const QByteArray& snap) {
    const QJsonDocument doc = QJsonDocument::fromJson(qUncompress(snap));
    m_project.fromJson(doc.isObject() ? doc.object() : QJsonObject());
}

void MainWindow::undo() {
    if (m_undoIndex <= 0) return;
    const QString sel = m_timeline->lastSelectedId();
    QPointer<QWidget> fw = focusWidget();
    --m_undoIndex;
    applyUndoState();
    // Preserva a seleção e o foco (o setProject da timeline limpa a seleção e
    // o editor de curvas/pancrop perderiam o clipe após o Ctrl+Z).
    if (!sel.isEmpty()) m_timeline->selectClip(sel);
    if (fw) fw->setFocus();
    setModified();
}

void MainWindow::redo() {
    if (m_undoIndex >= m_undoStack.size() - 1) return;
    const QString sel = m_timeline->lastSelectedId();
    QPointer<QWidget> fw = focusWidget();
    ++m_undoIndex;
    applyUndoState();
    if (!sel.isEmpty()) m_timeline->selectClip(sel);
    if (fw) fw->setFocus();
    setModified();
}

void MainWindow::applyUndoState() {
    restoreSnapshot(m_undoStack[m_undoIndex]);
    m_timeline->setProject(&m_project);
    m_pool->refreshFromProject();
    m_pancrop->setProject(&m_project);
    m_graph->refresh();
    m_props->refresh();
    m_preview->refreshView();
    m_mixer->refresh();
    m_mesa->refresh();
    m_mesa->autoSelectMesa();
    updateUndoActions();
    updateHistoryList();
}

void MainWindow::setModified() {
    m_modified = true;
    m_project.touch(); // invalida caches de composição (MesaRenderer)
    updateTitle();
    updateUndoActions();
}

bool MainWindow::mixerHasAutomation() const {
    return m_mixer && m_mixer->hasAutomation();
}

void MainWindow::updateUndoActions() {
    m_undoAction->setEnabled(m_undoIndex > 0);
    m_redoAction->setEnabled(m_undoIndex < m_undoStack.size() - 1);
}

// Painel de histórico (estilo Vegas): cada entrada = estado da timeline ANTES
// de uma edição. Clicar salta o índice de undo para aquele ponto (undo/redo
// em bloco, sem precisar dar N Ctrl+Z).
void MainWindow::updateHistoryList() {
    if (!m_histList || m_undoLabels.size() != m_undoStack.size()) return;
    m_histList->blockSignals(true);
    m_histList->clear();
    for (int i = 0; i < m_undoLabels.size(); ++i) {
        auto* item = new QListWidgetItem(QString::number(i + 1) + QLatin1String(". ") + m_undoLabels[i]);
        item->setData(Qt::UserRole, i);
        if (i == m_undoIndex)
            item->setForeground(QColor(110, 210, 255));
        m_histList->addItem(item);
    }
    m_histList->setCurrentRow(m_undoIndex);
    m_histList->scrollToItem(m_histList->currentItem());
    m_histList->blockSignals(false);
}

void MainWindow::jumpToUndo(int index) {
    if (index < 0 || index >= m_undoStack.size() || index == m_undoIndex) return;
    const QString sel = m_timeline->lastSelectedId();
    QPointer<QWidget> fw = focusWidget();
    m_undoIndex = index;
    applyUndoState();
    if (!sel.isEmpty()) m_timeline->selectClip(sel);
    if (fw) fw->setFocus();
    setModified();
}

void MainWindow::updateTitle() {
    const QString name = m_currentFile.isEmpty()
        ? tr("Sem título")
        : QFileInfo(m_currentFile).fileName();
    setWindowTitle(m_modified ? tr("%1 *").arg(name) : name);
}

void MainWindow::newProject() {
    if (!confirmDiscardChanges()) return;
    MediaCache::instance().clear();
    m_project = Project();
    for (int i = 0; i < 3; ++i) m_project.addTrack(false);
    for (int i = 0; i < 3; ++i) m_project.addTrack(true);
    m_pancrop->setProject(&m_project);
    m_pancropDock->hide();
    m_undoStack.clear();
    m_undoStack.append(snapshotState());
    m_undoLabels.clear();
    m_undoLabels.append(tr("Início"));
    m_undoIndex = 0;
    updateHistoryList();
    m_currentFile.clear();
    m_modified = false;
    applyUndoState();
    updateTitle();
    statusBar()->showMessage(tr("Novo projeto criado."));
}

void MainWindow::openProject() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Abrir projeto"), QString(),
        tr("Pierrot (*.Blanc *.ovp);;Todos os arquivos (*)"));
    if (path.isEmpty()) return;
    openProjectFile(path);
}

// Reabre a janela inicial sem fechar o editor. Se o usuário escolher abrir um
// projeto ou criar um novo, substitui o projeto atual (como em Arquivo → Novo).
void MainWindow::showWelcomeWindow() {
    WelcomeWindow welcome(this);
    if (welcome.exec() != QDialog::Accepted) return;
    if (!welcome.projectPath().isEmpty()) {
        openProjectFile(welcome.projectPath());
    } else if (welcome.newProjectRequested()) {
        createProject(welcome.projectWidth(), welcome.projectHeight(),
                      welcome.projectFps(), welcome.projectName());
    }
}

void MainWindow::openProjectFile(const QString& path) {
    MediaCache::instance().clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Abrir projeto"),
                             tr("Não foi possível abrir o arquivo:\n%1").arg(path));
        return;
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(this, tr("Abrir projeto"),
                             tr("Arquivo de projeto inválido:\n%1").arg(path));
        return;
    }
    m_project.fromJson(doc.object());
    if (m_project.videoTracks.isEmpty()) m_project.addTrack(false);
    if (m_project.audioTracks.isEmpty()) m_project.addTrack(true);

    m_undoStack.clear();
    m_undoStack.append(snapshotState());
    m_undoLabels.clear();
    m_undoLabels.append(tr("Início"));
    m_undoIndex = 0;
    updateHistoryList();
    m_currentFile = path;
    m_modified = false;
    applyUndoState();
    updateTitle();
    addRecentProject(path);
    statusBar()->showMessage(tr("Projeto aberto: %1").arg(path));
}

void MainWindow::createProject(int width, int height, int fps, const QString& name) {
    newProject();
    m_project.width = width;
    m_project.height = height;
    m_project.fps = fps;
    m_project.name = name;
    applyUndoState();
    updateTitle();
    saveProjectAs();
}

bool MainWindow::saveProject() {
    if (m_currentFile.isEmpty()) return saveProjectAs();
    if (!writeProjectFile(m_currentFile)) return false;
    statusBar()->showMessage(tr("Projeto salvo: %1").arg(m_currentFile));
    return true;
}

bool MainWindow::saveProjectAs() {
    const QString suggested = m_project.name.trimmed().isEmpty()
        ? QStringLiteral("Sem título.Blanc")
        : m_project.name.trimmed() + ".Blanc";
    QString path = QFileDialog::getSaveFileName(
        this, tr("Salvar projeto"), suggested,
        tr("Pierrot (*.Blanc);;Todos os arquivos (*)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(".Blanc", Qt::CaseInsensitive)
        && !path.endsWith(".ovp", Qt::CaseInsensitive))
        path += ".Blanc";
    if (!writeProjectFile(path)) return false;
    m_currentFile = path;
    updateTitle();
    addRecentProject(path);
    statusBar()->showMessage(tr("Projeto salvo: %1").arg(path));
    return true;
}

bool MainWindow::writeProjectFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Salvar projeto"),
                             tr("Não foi possível gravar o arquivo:\n%1").arg(path));
        return false;
    }
    const QJsonDocument doc(m_project.toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
    m_modified = false;
    updateTitle();
    return true;
}

void MainWindow::autoSave() {
    if (m_currentFile.isEmpty()) {
        statusBar()->showMessage(
            tr("Salvamento automático: salve o projeto uma vez (Ctrl+S) para ativar."));
        return;
    }
    if (writeProjectFile(m_currentFile))
        statusBar()->showMessage(
            tr("Projeto salvo automaticamente (%1).")
                .arg(QTime::currentTime().toString("HH:mm")));
}

void MainWindow::addRecentProject(const QString& path) {
    if (path.isEmpty()) return;
    QSettings s;
    QStringList rec = s.value("recentProjects").toStringList();
    rec.removeAll(path);
    rec.prepend(path);
    while (rec.size() > 10) rec.removeLast();
    s.setValue("recentProjects", rec);
}

void MainWindow::openSettings() {
    SettingsDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;

    // Re-renderiza o conteúdo dos clipes se o modo de miniatura mudou.
    m_timeline->refreshSettings();

    if (dlg.autoSaveEnabled()) {
        m_autoSaveTimer->setInterval(qMax(1, dlg.autoSaveMinutes()) * 60 * 1000);
        m_autoSaveTimer->start();
    } else {
        m_autoSaveTimer->stop();
    }
    statusBar()->showMessage(tr("Configurações aplicadas."));
}

void MainWindow::projectSettings() {
    ProjectSettingsDialog dlg(m_project.width, m_project.height, m_project.fps, this);
    if (dlg.exec() != QDialog::Accepted) return;
    if (dlg.width() == m_project.width && dlg.height() == m_project.height
        && dlg.fps() == m_project.fps)
        return;
    pushUndo();
    m_project.width = dlg.width();
    m_project.height = dlg.height();
    m_project.fps = dlg.fps();
    m_timeline->setProject(&m_project);
    m_pool->refreshFromProject();
    m_pancrop->setProject(&m_project);
    m_preview->refreshView();
    statusBar()->showMessage(tr("Configurações do projeto atualizadas."));
}

void MainWindow::exportVideo() {
    ExportDialog dlg(&m_project, this);
    dlg.setOfxManager(m_ofxManager);
    dlg.exec();
}

void MainWindow::exportEdl() {
    QString path = QFileDialog::getSaveFileName(
        this, tr("Exportar EDL"), QStringLiteral("timeline.edl"),
        tr("EDL CMX3600 (*.edl);;Todos os arquivos (*)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".edl", Qt::CaseInsensitive))
        path += QLatin1String(".edl");
    QString err;
    if (!NleInterchange::exportEdl(m_project, path, &err))
        QMessageBox::warning(this, tr("Exportar EDL"), err);
    else
        statusBar()->showMessage(tr("EDL exportado: %1").arg(path));
}

void MainWindow::importEdl() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Importar EDL"), QString(),
        tr("EDL CMX3600 (*.edl);;Todos os arquivos (*)"));
    if (path.isEmpty()) return;

    NleInterchange::EdlImportResult res;
    if (!NleInterchange::importEdl(path, res)) {
        QMessageBox::warning(this, tr("Importar EDL"), tr("Não foi possível ler o arquivo."));
        return;
    }

    // Substitui o projeto atual pelo importado (novo contexto de edição).
    m_project = res.project;
    if (m_project.videoTracks.isEmpty()) m_project.addTrack(false);
    if (m_project.audioTracks.isEmpty()) m_project.addTrack(true);
    m_undoStack.clear();
    m_undoStack.append(snapshotState());
    m_undoLabels.clear();
    m_undoLabels.append(tr("Início"));
    m_undoIndex = 0;
    updateHistoryList();
    m_currentFile.clear();
    m_modified = true;
    applyUndoState();
    updateTitle();

    if (!res.unresolvedReels.isEmpty())
        QMessageBox::information(
            this, tr("Importar EDL"),
            tr("Alguns Reels não tinham caminho absoluto e ficaram sem mídia:\n%1")
                .arg(res.unresolvedReels.join(QLatin1String(", "))));
    if (!res.warnings.isEmpty())
        statusBar()->showMessage(res.warnings.join(QLatin1String(" | ")),
                                 8000);
    else
        statusBar()->showMessage(tr("EDL importado: %1").arg(path));
}
