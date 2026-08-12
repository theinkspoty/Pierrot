#include "MainWindow.h"

#include "ui/MediaPoolWidget.h"
#include "ui/TimelineWidget.h"
#include "ui/PreviewWidget.h"
#include "ui/PancropWidget.h"
#include "ui/GraphEditorWidget.h"
#include "ui/ExportDialog.h"
#include "ui/ProjectSettingsDialog.h"
#include "ui/SettingsDialog.h"

#include <QApplication>
#include <QDockWidget>
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
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QCloseEvent>
#include <QStyle>
#include <QPainterPath>
#include <QPolygonF>
#include <QTimer>
#include <QTime>
#include <QProgressBar>
#include <QGuiApplication>
#include <QScreen>
#include <QDataStream>

#include "ffmpeg/MediaCache.h"

namespace {
// Versão do arranjo de painéis (docks/toolbar). Aumente para descartar
// estados salvos antigos que estejam com o layout deslocado.
constexpr int kLayoutVersion = 2;

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
    setWindowTitle(tr("Pierrot — Editor de Vídeo"));
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
    auto* centralLay = new QVBoxLayout(centralHost);
    centralLay->setContentsMargins(4, 4, 4, 4);
    centralLay->addWidget(m_preview, 1);
    setCentralWidget(centralHost);

    m_pancrop = new PancropWidget(this);
    m_pancrop->setProject(&m_project);

    m_graph = new GraphEditorWidget(this);
    m_graph->setProject(&m_project);
    m_graph->setMinimumHeight(180);

    createDocks();
    createActions();

    m_undoStack.append(m_project);
    m_undoIndex = 0;
    updateUndoActions();

    connect(m_timeline, &TimelineWidget::playheadChanged, m_preview, &PreviewWidget::seek);
    connect(m_preview, &PreviewWidget::playheadMoved, m_timeline, &TimelineWidget::setPlayhead);
    connect(m_timeline, &TimelineWidget::playPauseRequested, m_preview, &PreviewWidget::togglePlay);
    connect(m_preview, &PreviewWidget::stateChanged, this, [this](bool playing) {
        m_playAction->setText(playing ? tr("Pausar") : tr("Reproduzir"));
        m_playAction->setIcon(style()->standardIcon(
            playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    });
    connect(m_timeline, &TimelineWidget::modified, this, [this]() {
        m_preview->refreshView();
    });
    connect(m_timeline, &TimelineWidget::modified, this, &MainWindow::setModified);
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
    connect(m_timeline, &TimelineWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_timeline, &TimelineWidget::loopChanged, m_preview, &PreviewWidget::setLoopRange);
    connect(m_pool, &MediaPoolWidget::editStart, this, &MainWindow::pushUndo);
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
    connect(m_graph, &GraphEditorWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_graph, &GraphEditorWidget::modified, this, [this]() {
        m_timeline->update();
        m_preview->refreshView();
        m_pancrop->sync();
        setModified();
    });
    connect(m_timeline, &TimelineWidget::modified, m_graph, &GraphEditorWidget::refresh);
    connect(m_pancrop, &PancropWidget::modified, m_graph, &GraphEditorWidget::refresh);
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
    connect(m_pancrop, &PancropWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_pancrop, &PancropWidget::modified, this, [this]() {
        m_timeline->update();
        m_preview->refreshView();
        setModified();
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

    restoreSettings();

    // Salvamento automático configurado na janela de boas-vindas
    m_autoSaveTimer = new QTimer(this);
    connect(m_autoSaveTimer, &QTimer::timeout, this, &MainWindow::autoSave);
    QSettings autosave;
    const int autosaveMin = qMax(1, autosave.value("autosaveMinutes", 10).toInt());
    if (autosave.value("autosaveEnabled", false).toBool()) {
        m_autoSaveTimer->setInterval(autosaveMin * 60 * 1000);
        m_autoSaveTimer->start();
    }
}

void MainWindow::saveSettings() {
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("layout", saveState());
    settings.setValue("layoutVersion", kLayoutVersion);
    settings.setValue("layoutLocked", m_lockAction->isChecked());
}

void MainWindow::restoreSettings() {
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
    // Garante que a barra de ferramentas principal fique sempre no topo,
    // à esquerda, mesmo que um layout antigo salvo a tenha deslocado.
    if (m_mainToolBar)
        addToolBar(Qt::TopToolBarArea, m_mainToolBar);
    if (settings.contains("layoutLocked"))
        m_lockAction->setChecked(settings.value("layoutLocked").toBool());
    setDockLocked(m_lockAction->isChecked());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    QMainWindow::closeEvent(event);
    QApplication::quit();
}

void MainWindow::createDocks() {
    m_poolDock = new QDockWidget(tr("Mídia"), this);
    m_poolDock->setWidget(m_pool);
    m_poolDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, m_poolDock);

    m_timelineDock = new QDockWidget(tr("Timeline"), this);
    m_timelineDock->setAllowedAreas(Qt::BottomDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
    // O widget do dock (ferramentas + timeline) é montado em createActions().

    m_pancropDock = new QDockWidget(tr("Pancrop"), this);
    m_pancropDock->setWidget(m_pancrop);
    m_pancropDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, m_pancropDock);
    m_pancropDock->hide();

    m_graphDock = new QDockWidget(tr("Editor de Curvas"), this);
    m_graphDock->setWidget(m_graph);
    m_graphDock->setAllowedAreas(Qt::BottomDockWidgetArea);
    addDockWidget(Qt::BottomDockWidgetArea, m_graphDock);
    splitDockWidget(m_timelineDock, m_graphDock, Qt::Horizontal);
    m_graphDock->setMinimumWidth(260);
    m_graphDock->resize(320, m_graphDock->height());
}

void MainWindow::createActions() {
    const auto stdIcon = [this](QStyle::StandardPixmap sp) {
        return style()->standardIcon(sp);
    };

    QAction* addMedia = new QAction(tr("Importar mídia…"), this);
    addMedia->setShortcut(QKeySequence("Ctrl+I"));
    addMedia->setIcon(iconImport());
    addMedia->setToolTip(tr("Importar mídia… (Ctrl+I)"));
    connect(addMedia, &QAction::triggered, m_pool, &MediaPoolWidget::addFiles);

    QAction* exportAct = new QAction(tr("Exportar…"), this);
    exportAct->setShortcut(QKeySequence("Ctrl+E"));
    exportAct->setIcon(iconExport());
    exportAct->setToolTip(tr("Exportar vídeo… (Ctrl+E)"));
    connect(exportAct, &QAction::triggered, this, &MainWindow::exportVideo);

    QAction* quit = new QAction(tr("Sair"), this);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &MainWindow::close);

    m_playAction = new QAction(tr("Reproduzir"), this);
    m_playAction->setShortcut(QKeySequence(Qt::Key_Space));
    m_playAction->setIcon(stdIcon(QStyle::SP_MediaPlay));
    m_playAction->setToolTip(tr("Reproduzir/Pausar (Espaço)"));
    connect(m_playAction, &QAction::triggered, m_preview, &PreviewWidget::togglePlay);

    QAction* cutAction = new QAction(tr("Dividir no playhead"), this);
    cutAction->setShortcut(QKeySequence(Qt::Key_S));
    cutAction->setIcon(iconScissors());
    cutAction->setToolTip(tr("Dividir clipe no playhead (S)"));
    connect(cutAction, &QAction::triggered, m_timeline, &TimelineWidget::cutAtPlayhead);

    QAction* deleteAction = new QAction(tr("Excluir clipe"), this);
    deleteAction->setShortcut(QKeySequence(Qt::Key_Delete));
    deleteAction->setIcon(stdIcon(QStyle::SP_TrashIcon));
    deleteAction->setToolTip(tr("Excluir clipe selecionado e fechar o espaço (Delete; Shift+Delete deixa espaço)"));
    connect(deleteAction, &QAction::triggered, m_timeline, &TimelineWidget::deleteSelected);

    m_undoAction = new QAction(tr("Desfazer"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_undoAction->setIcon(stdIcon(QStyle::SP_ArrowBack));
    m_undoAction->setToolTip(tr("Desfazer (Ctrl+Z)"));
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::undo);

    m_redoAction = new QAction(tr("Refazer"), this);
    m_redoAction->setShortcut(QKeySequence("Ctrl+Shift+Z"));
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
    newAction->setShortcut(QKeySequence::New);
    newAction->setIcon(stdIcon(QStyle::SP_FileIcon));
    newAction->setToolTip(tr("Novo projeto (Ctrl+N)"));
    connect(newAction, &QAction::triggered, this, &MainWindow::newProject);

    QAction* openAction = new QAction(tr("Abrir…"), this);
    openAction->setShortcut(QKeySequence::Open);
    openAction->setIcon(stdIcon(QStyle::SP_DirOpenIcon));
    openAction->setToolTip(tr("Abrir projeto… (Ctrl+O)"));
    connect(openAction, &QAction::triggered, this, &MainWindow::openProject);

    m_saveAction = new QAction(tr("Salvar"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setIcon(stdIcon(QStyle::SP_DialogSaveButton));
    m_saveAction->setToolTip(tr("Salvar projeto (Ctrl+S)"));
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::saveProject);

    m_saveAsAction = new QAction(tr("Salvar como…"), this);
    m_saveAsAction->setShortcut(QKeySequence("Ctrl+Shift+S"));
    connect(m_saveAsAction, &QAction::triggered, this, &MainWindow::saveProjectAs);

    QAction* settingsAction = new QAction(tr("Configurações do projeto…"), this);
    connect(settingsAction, &QAction::triggered, this, &MainWindow::projectSettings);

    QMenu* fileMenu = menuBar()->addMenu(tr("&Arquivo"));
    fileMenu->addAction(newAction);
    fileMenu->addAction(openAction);
    fileMenu->addAction(m_saveAction);
    fileMenu->addAction(m_saveAsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(exportAct);
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

    m_lockAction = new QAction(padlockIcon(true), tr("Travar layout"), this);
    m_lockAction->setCheckable(true);
    m_lockAction->setChecked(true);
    m_lockAction->setShortcut(QKeySequence("Ctrl+L"));
    m_lockAction->setToolTip(tr("Travar/destravar o layout dos painéis (Ctrl+L)"));
    connect(m_lockAction, &QAction::toggled, this, [this](bool locked) {
        setDockLocked(locked);
        m_lockAction->setText(locked ? tr("Travar layout") : tr("Destravar layout"));
        m_lockAction->setIcon(padlockIcon(locked));
        statusBar()->showMessage(locked ? tr("Layout travado.")
                                        : tr("Layout destravado — arraste os painéis para reorganizar."));
    });

    QMenu* viewMenu = menuBar()->addMenu(tr("&Exibir"));
    viewMenu->addAction(m_poolDock->toggleViewAction());
    viewMenu->addAction(m_timelineDock->toggleViewAction());
    viewMenu->addAction(m_pancropDock->toggleViewAction());
    viewMenu->addAction(m_graphDock->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction(m_lockAction);

    QAction* appSettingsAction = new QAction(tr("Configurações do app…"), this);
    appSettingsAction->setShortcut(QKeySequence("Ctrl+,"));
    appSettingsAction->setToolTip(tr("Abrir as configurações do aplicativo"));
    connect(appSettingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    QMenu* cfgMenu = menuBar()->addMenu(tr("&Configurações"));
    cfgMenu->addAction(appSettingsAction);

    m_mainToolBar = addToolBar(tr("Principal"));
    QToolBar* tb = m_mainToolBar;
    tb->setMovable(false);
    tb->setAllowedAreas(Qt::TopToolBarArea);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    // Arquivo
    tb->addAction(newAction);
    tb->addAction(openAction);
    tb->addAction(m_saveAction);
    tb->addAction(addMedia);
    tb->addSeparator();
    // Editar
    tb->addAction(m_undoAction);
    tb->addAction(m_redoAction);
    tb->addSeparator();
    tb->addAction(cutAction);
    tb->addAction(deleteAction);
    tb->addSeparator();
    // Transporte
    tb->addAction(m_playAction);
    tb->addSeparator();
    // Exportar / layout
    tb->addAction(exportAct);
    tb->addSeparator();
    tb->addAction(m_lockAction);

    // As ferramentas da timeline ficam ancoradas acima da própria timeline,
    // dentro do dock, em vez de na barra superior da janela.
    auto* tlContainer = new QWidget;
    auto* tlLay = new QVBoxLayout(tlContainer);
    tlLay->setContentsMargins(0, 0, 0, 0);
    tlLay->setSpacing(0);

    QToolBar* toolTb = new QToolBar(tr("Ferramentas da timeline"), tlContainer);
    toolTb->setMovable(false);
    toolTb->setToolButtonStyle(Qt::ToolButtonTextOnly);
    const QStringList toolNames = {
        tr("Selecionar (0)"), tr("Mover (M)"), tr("Tesoura (R)"),
        tr("Envelope (E)"), tr("Lupa (Z)")
    };
    const QList<QIcon> toolIcons = {
        iconCursor(), iconMove(), iconRazor(), iconEnvelope(), iconZoom()
    };
    const QList<QKeySequence> toolKeys = {
        QKeySequence(Qt::Key_0), QKeySequence(Qt::Key_M), QKeySequence(Qt::Key_R),
        QKeySequence(Qt::Key_E), QKeySequence(Qt::Key_Z)
    };
    QActionGroup* toolGroup = new QActionGroup(this);
    for (int i = 0; i < toolNames.size(); ++i) {
        QAction* a = new QAction(toolIcons[i], toolNames[i], this);
        a->setCheckable(true);
        a->setShortcut(toolKeys[i]);
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
    QAction* clearLoopTb = new QAction(stdIcon(QStyle::SP_BrowserReload), tr("Limpar loop"), this);
    clearLoopTb->setToolTip(tr("Limpar região de loop"));
    connect(clearLoopTb, &QAction::triggered, m_timeline, &TimelineWidget::clearLoop);
    toolTb->addAction(clearLoopTb);

    tlLay->addWidget(toolTb);
    tlLay->addWidget(m_timeline, 1);
    m_timelineDock->setWidget(tlContainer);
}

void MainWindow::setDockLocked(bool locked) {
    for (QDockWidget* dock : {m_poolDock, m_timelineDock, m_pancropDock, m_graphDock}) {
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
    QPixmap pm(24, 24);
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
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(c);
        p.drawLine(QPointF(16, 8), QPointF(16, 24));
        p.drawLine(QPointF(8, 16), QPointF(24, 16));
        QPolygonF up;
        up << QPointF(16, 3) << QPointF(11, 9) << QPointF(21, 9);
        p.drawPolygon(up);
        QPolygonF down;
        down << QPointF(16, 29) << QPointF(11, 23) << QPointF(21, 23);
        p.drawPolygon(down);
        QPolygonF left;
        left << QPointF(3, 16) << QPointF(9, 11) << QPointF(9, 21);
        p.drawPolygon(left);
        QPolygonF right;
        right << QPointF(29, 16) << QPointF(23, 11) << QPointF(23, 21);
        p.drawPolygon(right);
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
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(c);
        QPolygonF blade;
        blade << QPointF(6, 7) << QPointF(17, 5) << QPointF(15, 22) << QPointF(8, 24);
        p.drawPolygon(blade);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(17, 5), QPointF(15, 22));
        p.drawLine(QPointF(11, 23), QPointF(19, 31));
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
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 12), 8, 8);
        p.drawLine(QPointF(18, 18), QPointF(28, 28));
        p.drawLine(QPointF(12, 7), QPointF(12, 17));
        p.drawLine(QPointF(7, 12), QPointF(17, 12));
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

void MainWindow::pushUndo() {
    if (m_undoIndex < m_undoStack.size() - 1)
        m_undoStack.resize(m_undoIndex + 1);
    m_undoStack.append(m_project);
    m_undoIndex = m_undoStack.size() - 1;
    while (m_undoStack.size() > 60) {
        m_undoStack.removeAt(0);
        --m_undoIndex;
    }
    setModified();
}

void MainWindow::undo() {
    if (m_undoIndex <= 0) return;
    --m_undoIndex;
    applyUndoState();
    setModified();
}

void MainWindow::redo() {
    if (m_undoIndex >= m_undoStack.size() - 1) return;
    ++m_undoIndex;
    applyUndoState();
    setModified();
}

void MainWindow::applyUndoState() {
    m_project = m_undoStack[m_undoIndex];
    m_timeline->setProject(&m_project);
    m_pool->refreshFromProject();
    m_pancrop->setProject(&m_project);
    m_preview->refreshView();
    m_timeline->setFocus();
    updateUndoActions();
}

void MainWindow::setModified() {
    m_modified = true;
    updateTitle();
    updateUndoActions();
}

void MainWindow::updateUndoActions() {
    m_undoAction->setEnabled(m_undoIndex > 0);
    m_redoAction->setEnabled(m_undoIndex < m_undoStack.size() - 1);
}

void MainWindow::updateTitle() {
    const QString name = m_currentFile.isEmpty()
        ? tr("Sem título")
        : QFileInfo(m_currentFile).fileName();
    setWindowTitle(tr("Pierrot — %1%2")
                       .arg(name, m_modified ? tr(" *") : QString()));
}

void MainWindow::newProject() {
    m_project = Project();
    for (int i = 0; i < 3; ++i) m_project.addTrack(false);
    for (int i = 0; i < 3; ++i) m_project.addTrack(true);
    m_pancrop->setProject(&m_project);
    m_pancropDock->hide();
    m_undoStack.clear();
    m_undoStack.append(m_project);
    m_undoIndex = 0;
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

void MainWindow::openProjectFile(const QString& path) {
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
    m_undoStack.append(m_project);
    m_undoIndex = 0;
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

    PreviewWidget::setMaxDecodeWidth(dlg.decodeWidth());

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
    dlg.exec();
}
