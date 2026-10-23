// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MainWindow.h"
#include "version.h"

#include "ui/panels/MediaPoolWidget.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/monitor/PreviewWidget.h"
#include "ui/mesa/PantaloneWidget.h"
#include "ui/motion/DottoreWidget.h"
#include "ui/dialogs/MaskEditorDialog.h"
#include "ui/motion/GraphEditorWidget.h"
#include "ui/panels/EffectsWidget.h"
#include "ui/timeline/ClipPropertiesWidget.h"
#include "ui/dialogs/TextEditorDialog.h"
#include "ui/motion/LazziDock.h"
#include "ui/motion/AnimPresetDock.h"
#include "ui/panels/ColorDock.h"
#include "ui/panels/ExpressWidget.h"
#include "ui/panels/FileBrowserWidget.h"
#include "ui/audio/MixerWidget.h"
#include "ui/audio/EqualizerWidget.h"
#include "ui/mesa/MesaWidget.h"
#include "ui/monitor/SourceMonitorWidget.h"
#include "ui/motion/VelocityEditorWidget.h"
#include "ui/mesa/PivotWidget.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "ui/dialogs/ExportDialog.h"
#include "ui/dialogs/RenderQueueDialog.h"
#include "colombina/export/NleInterchange.h"
#include "ui/monitor/ScopeWidget.h"
#include "ui/monitor/PreviewMonitor.h"
#include "ui/monitor/PreviewProfiler.h"
#include "ui/dialogs/ProjectSettingsDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/core/Theme.h"
#include "ui/core/TechnicalWindow.h"
#include "colombina/ofx/OfxPluginManager.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QSet>
#include <QSettings>
#include "ui/dialogs/WelcomeWindow.h"
#include "colombina/ffmpeg/MediaCache.h"
#include "colombina/ffmpeg/ProxyManager.h"

#include <QApplication>
#include <QPointer>
#include <QDockWidget>
#include <QListWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QAction>
#include <QKeySequence>
#include <QMouseEvent>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <algorithm>
#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractButton>
#include <QPainter>
#include <QPixmap>
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
#include <QScreen>
#include <QDataStream>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QThreadPool>

#include "MainWindowPriv.h"

// Devolve o atalho salvo pelo usuário (Configurações → Atalhos) ou o padrão.
static QKeySequence appKey(const char* id, const QKeySequence& fallback) {
    const QString v = QSettings().value(QStringLiteral("shortcuts/") + QLatin1String(id)).toString();
    return v.isEmpty() ? fallback : QKeySequence(v);
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
        dlg.setFrei0rManager(m_frei0rManager);
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

    QAction* reloadAction = new QAction(tr("Recarregar"), this);
    reloadAction->setShortcut(appKey("reload", QKeySequence(Qt::Key_F5)));
    reloadAction->setToolTip(tr("Recarregar o projeto do disco (F5)"));
    connect(reloadAction, &QAction::triggered, this, &MainWindow::reloadProject);

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
    fileMenu->addAction(reloadAction);
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
    QAction* insertAction = new QAction(tr("Inserir no playhead (Source)"), this);
    insertAction->setShortcut(QKeySequence(QStringLiteral(",")));
    insertAction->setToolTip(tr("Insere o trecho In→Out do Source Monitor no playhead "
                                "da timeline, empurrando os clipes seguintes"));
    connect(insertAction, &QAction::triggered, this, [this]() {
        if (!m_source || !m_source->hasMedia()) {
            statusBar()->showMessage(tr("Abra uma mídia no Source Monitor primeiro "
                                        "(duplo-clique na Central de Mídias)."), 3000);
            return;
        }
        m_source->insertRequested();
    });
    editMenu->addAction(insertAction);
    QAction* overwriteAction = new QAction(tr("Sobrescrever no playhead (Source)"), this);
    overwriteAction->setShortcut(QKeySequence(QStringLiteral(".")));
    overwriteAction->setToolTip(tr("Sobrescreve a timeline no playhead com o trecho "
                                   "In→Out do Source Monitor"));
    connect(overwriteAction, &QAction::triggered, this, [this]() {
        if (!m_source || !m_source->hasMedia()) {
            statusBar()->showMessage(tr("Abra uma mídia no Source Monitor primeiro "
                                        "(duplo-clique na Central de Mídias)."), 3000);
            return;
        }
        m_source->overwriteRequested();
    });
    editMenu->addAction(overwriteAction);
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
    // Menu agrupado por região (estilo Premiere): Esquerda / Direita / Inferior.
    auto addDockGroup = [&](const QString& title, QList<QDockWidget*> docks) {
        if (docks.isEmpty()) return;
        QMenu* sub = viewMenu->addMenu(title);
        for (QDockWidget* d : docks)
            sub->addAction(d->toggleViewAction());
    };
    QList<QDockWidget*> leftDocks, rightDocks, bottomDocks, otherDocks;
    for (QDockWidget* dock : m_allDocks) {
        switch (dockWidgetArea(dock)) {
        case Qt::LeftDockWidgetArea: leftDocks.append(dock); break;
        case Qt::RightDockWidgetArea: rightDocks.append(dock); break;
        case Qt::BottomDockWidgetArea: bottomDocks.append(dock); break;
        default: otherDocks.append(dock); break;
        }
    }
    addDockGroup(tr("Painéis à esquerda"), leftDocks);
    addDockGroup(tr("Painéis à direita"), rightDocks);
    addDockGroup(tr("Painéis embaixo"), bottomDocks);
    if (!otherDocks.isEmpty()) {
        viewMenu->addSeparator();
        for (QDockWidget* d : otherDocks)
            viewMenu->addAction(d->toggleViewAction());
    }
    viewMenu->addSeparator();

    // Workspaces (roadmap 0.7): arranjos nomeados de painéis, à Premiere.
    m_workspaceMenu = viewMenu->addMenu(tr("Workspaces"));
    rebuildWorkspaceMenu();

    // Recupera o arranjo guardado quando uma atualização subiu kLayoutVersion
    // e descartou o layout antigo (ver restoreSettings()).
    QAction* restoreLayout = viewMenu->addAction(tr("Restaurar layout anterior"));
    restoreLayout->setToolTip(tr("Reaplica o arranjo de painéis guardado antes de uma "
                                 "atualização que dispensou o layout antigo."));
    connect(restoreLayout, &QAction::triggered, this, &MainWindow::restoreBackupLayout);

    // Preview externo: janela própria (segundo monitor) com o mesmo sinal de
    // vídeo do monitor principal, sem overlays, e os controles do Program
    // Monitor (transporte, margens de segurança, zoom e timecode). F11,
    // duplo-clique ou o botão de tela cheia expandem; Esc sai.
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
                // Comandos da janela externa voltam para o mesmo transporte.
                connect(m_monitor, &PreviewMonitor::togglePlayRequested,
                        m_preview, &PreviewWidget::togglePlay);
                connect(m_monitor, &PreviewMonitor::stepRequested,
                        m_preview, &PreviewWidget::stepFrameBy);
                connect(m_monitor, &PreviewMonitor::loopToggled,
                        m_preview, &PreviewWidget::setLoopEnabled);
                // Estado do monitor principal alimenta a janela externa.
                connect(m_preview, &PreviewWidget::playheadMoved,
                        m_monitor, &PreviewMonitor::setTimecode);
                connect(m_preview, &PreviewWidget::stateChanged,
                        m_monitor, &PreviewMonitor::setPlaying);
                m_monitor->setFps(m_project.fps);
                m_monitor->setTimecode(m_timeline ? m_timeline->playhead() : 0.0);
                m_monitor->setPlaying(m_preview->isPlaying());
                m_monitor->setLoopEnabled(m_preview->loopEnabled());
            }
            m_monitor->setFrame(m_preview->compositeFrame());
            m_monitor->show();
            m_monitor->raise();
            m_monitor->activateWindow();
        } else if (m_monitor) {
            m_monitor->close(); // WA_DeleteOnClose → destroyed → desmarca a ação
        }
    });
    viewMenu->addAction(m_monitorAction);
    // Painel técnico: janela de docks da bancada (Mixer/Efeitos/Express/…),
    // independente do número de telas — no preset Bancada com 3+ telas ela
    // vai para a tela da esquerda; aqui o usuário pode abrir/fechar a qualquer
    // momento e docar docks nela por arrasto ou pelo menu Painéis dela.
    m_techPanelAction = new QAction(tr("Painel técnico"), this);
    m_techPanelAction->setCheckable(true);
    m_techPanelAction->setToolTip(tr("Janela de painéis técnicos (Mixer, Efeitos, Express, …) — "
                                     "arraste docks até ela ou use o menu Painéis."));
    connect(m_techPanelAction, &QAction::toggled, this, [this](bool on) {
        if (on) {
            ensureTechnicalWindow();
            // Com 2+ telas, abre na tela da esquerda em tamanho útil; com 1,
            // flutua onde estiver (o usuário arrasta).
            QList<QScreen*> screens = QGuiApplication::screens();
            if (screens.size() >= 2) {
                std::sort(screens.begin(), screens.end(),
                          [](QScreen* a, QScreen* b) {
                              return a->geometry().x() < b->geometry().x();
                          });
                const QRect g = screens.first()->availableGeometry();
                m_techWindow->setGeometry(g.adjusted(24, 24, -qMax(200, g.width() / 3), -24));
            }
            m_techWindow->show();
            m_techWindow->raise();
            m_techWindow->activateWindow();
        } else if (m_techWindow) {
            m_techWindow->returnAllDocks();
            m_techWindow->hide();
        }
    });
    viewMenu->addAction(m_techPanelAction);
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
            tr(" — Protótipo 4<br>"
               "Motores: <b>Colombina</b> v" PIERROT_COLOMBINA_VERSION
               " (kernel) · <b>Laartman</b> v" PIERROT_LAARTMAN_VERSION
               " (codecs)<br><br>"
               "<b style='color:#c07800'>Ainda em desenvolvimento:</b> mesa, "
               "lazzi, technicolor, Dottore, velocidade, pivot, analisador, "
               "workspace, mixer + equalizador e animações.<br><br>"
               "<b>Workspaces:</b> workspaces + workspace bancada.<br>"
               "<b>Temas:</b> Japan 95, R4 e Claro.<br><br>"
               "Pierrot é um editor de vídeo inspirado em 2 programas \"Sony Vegas 8\" "
               "e \"Final Cut Express 2003\", totalmente escrito pra linux. Para uso "
               "pessoal, baseado nas ferramentas que eu acho úteis.<br><br>"
               "<b>Agradecimentos:</b> Gumduck, por ajudar a desenvolver o áudio e "
               "features.<br><br>"
               "<b>Inspirações adicionais</b>"
               "<ul>"
               "<li><b>Adobe Premiere Pro</b> — painel de Efeitos, Effect Controls "
               "(Propriedades), Media Pool, Source Monitor e o mixer de áudio.</li>"
               "<li><b>Adobe After Effects</b> — o canvas de composição (Mesa), a "
               "faixa de efeitos expandível e o Graph Editor de keyframes.</li>"
               "<li><b>DaVinci Resolve</b> — organização por categorias nas "
               "configurações e o visual de cor/timecode.</li>"
               "<li><b>Blender</b> — o HUD de transformação do canvas Mesa.</li>"
               "</ul>"
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
    toolTb->setOrientation(Qt::Vertical);
    toolTb->setIconSize(QSize(24, 24));
    toolTb->setFixedWidth(46);
    toolTb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // Paleta de ferramentas vertical, como a Tools do Premiere: fica ao lado
    // dos cabeçalhos de faixa, com checkout no hover/pressionado do tema.
    const auto& tc = themeColors();
    toolTb->setStyleSheet(QStringLiteral(
        "QToolBar{background:transparent;border:none;padding:4px 2px;spacing:4px;}"
        "QToolBar::separator{background:%1;}"
        "QToolBar::separator:vertical{height:1px;margin:6px 8px;}"
        "QToolBar::separator:horizontal{width:1px;margin:8px 6px;}"
        "QToolBar QToolButton{border:none;border-radius:4px;background:%2;}"
        "QToolBar QToolButton:hover{background:%3;}"
        "QToolBar QToolButton:checked{background:%4;}")
        .arg(tc.transportBorder.name(),
             tc.toolBtnBg.name(QColor::HexArgb),
             tc.toolBtnHover.name(QColor::HexArgb),
             tc.toolBtnChecked.name(QColor::HexArgb)));
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
    m_snapAction->setShortcut(appKey("snap", QKeySequence(Qt::Key_T)));
    m_snapAction->setToolTip(tr("Encaixar no grid e nas bordas dos clipes (T). "
                                "Desligar o ímã também desativa o auto-juntar "
                                "(ripple) ao cortar/excluir."));
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
                            "Desligue para deixar o espaço vazio. "
                            "Inativo quando o ímã está desligado."));
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

    // Timeline volta a ocupar o dock inteiro (a paleta saiu para dock próprio).
    tlLay->addWidget(m_timeline, 1);
    m_timelineDock->setWidget(tlContainer);

    // Paleta de ferramentas independente e DOCÁVEL, como a Tools do Premiere:
    // um dock estreito e vertical, encaixado à esquerda da timeline (atrás dos
    // cabeçalhos de faixa). O usuário pode arrastá-la, encaixá-la em outra
    // área ou deixá-la flutuando. splitDockWidget + resizeDocks definem a fatia
    // inicial; o resto é o layout que o usuário salvar (saveState/restoreState).
    m_toolsDock = makeDock(QStringLiteral("toolsDock"), tr("Ferramentas"),
                           toolTb, Qt::BottomDockWidgetArea);
    m_toolsDock->setMinimumWidth(44);
    splitDockWidget(m_timelineDock, m_toolsDock, Qt::Horizontal);
    resizeDocks({m_timelineDock, m_toolsDock}, {1920, 60}, Qt::Horizontal);
}


