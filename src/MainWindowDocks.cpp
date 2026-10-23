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

QDockWidget* MainWindow::makeDock(const QString& objectName, const QString& title,
                                  QWidget* content, Qt::DockWidgetArea area) {
    QDockWidget* dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);
    if (content) dock->setWidget(content);
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setFeatures(QDockWidget::DockWidgetMovable
                      | QDockWidget::DockWidgetFloatable
                      | QDockWidget::DockWidgetClosable);
    // Sem isto o Qt recusa arrastar um painel para dentro de outro, o gesto
    // que cria um dock flutuante com painel encaixado — no Premiere é a forma
    // mais comum de montar um layout. AnimatedDocks dá a transição suave.
    // (O "dock que se destaca sozinho" NÃO é a animação: é o arraste fantasma
    // do Qt em clique simples, corrigido no eventFilter.)
    setDockOptions(QMainWindow::AllowNestedDocks
                   | QMainWindow::AllowTabbedDocks
                   | QMainWindow::AnimatedDocks);
    addDockWidget(area, dock);
    m_allDocks.append(dock);
    // Filtro que impede o arraste fantasma do dock em clique simples (ver
    // eventFilter): engole MouseMove sem botão sobre o próprio dock.
    dock->installEventFilter(this);
    return dock;
}

// Abas dos docks sempre no topo do grupo (item 6 do roadmap). Sem isto o Qt
// escolhe a posição e, em grupos baixos, a barra de abas pode cair para baixo
// do conteúdo — o oposto do Premiere.
void MainWindow::setTabPositionsUp() {
    for (Qt::DockWidgetArea area : {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea,
                                    Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea})
        setTabPosition(area, QTabWidget::North);
}

void MainWindow::createDocks() {
    m_poolDock = makeDock(QStringLiteral("poolDock"), tr("Central de Mídias"),
                          m_pool, Qt::LeftDockWidgetArea);

    // O widget do dock (ferramentas + timeline) é montado em createActions().
    m_timelineDock = makeDock(QStringLiteral("timelineDock"), tr("Timeline"),
                              nullptr, Qt::BottomDockWidgetArea);

    m_pantaloneDock = makeDock(QStringLiteral("pantaloneDock"), tr("Pantalone"),
                             m_pantalone, Qt::RightDockWidgetArea);
    m_pantaloneDock->hide();

    m_graphDock = makeDock(QStringLiteral("graphDock"), tr("Editor de Curvas"),
                           m_graph, Qt::BottomDockWidgetArea);
    splitDockWidget(m_timelineDock, m_graphDock, Qt::Vertical);

    // Editor de Velocidade — aba do Editor de Curvas (mesma família de UI).
    m_velocityDock = makeDock(QStringLiteral("velocityDock"), tr("Velocidade"),
                              m_velocity, Qt::BottomDockWidgetArea);
    tabifyDockWidget(m_graphDock, m_velocityDock);
    m_velocityDock->hide();

    // Pivot — aba do Editor de Curvas/Velocidade (edição 3D do clipe).
    m_pivotDock = makeDock(QStringLiteral("pivotDock"), tr("Pivot"),
                           m_pivot, Qt::BottomDockWidgetArea);
    tabifyDockWidget(m_graphDock, m_pivotDock);
    tabifyDockWidget(m_velocityDock, m_pivotDock);
    m_pivotDock->hide();

    // Dottore — segundo viewer de edição de motion (formas). Nasce à direita,
    // ao lado do Preview (mesma área do Pantalone), para editar e ver o
    // resultado sem abrir a Mesa.
    m_dottoreDock = makeDock(QStringLiteral("dottoreDock"), tr("Dottore"),
                             m_dottore, Qt::RightDockWidgetArea);
    if (m_pantaloneDock)
        splitDockWidget(m_pantaloneDock, m_dottoreDock, Qt::Vertical);
    m_dottoreDock->hide();
    m_effectsDock = makeDock(QStringLiteral("effectsDock"), tr("Effects"),
                             m_effects, Qt::RightDockWidgetArea);
    m_effectsDock->hide();

    m_expressDock = makeDock(QStringLiteral("expressDock"), tr("Express"),
                             m_express, Qt::RightDockWidgetArea);
    tabifyDockWidget(m_effectsDock, m_expressDock);
    m_expressDock->hide();

    // Editor de Texto (dock opcional, live): edita o clipe de texto
    // selecionado sem abrir o diálogo modal. Some do caminho até o usuário
    // pedir em Exibir → Painéis à direita → Texto.
    m_textEditor = new TextEditorWidget(this);
    m_textEditor->setLive(true);
    m_textDock = makeDock(QStringLiteral("textEditorDock"), tr("Texto"),
                          m_textEditor, Qt::RightDockWidgetArea);
    tabifyDockWidget(m_expressDock, m_textDock);
    m_textDock->hide();
    connect(m_textEditor, &TextEditorWidget::changed, this, [this]() {
        m_timeline->invalidateScene();
        m_preview->refreshView();
        setModified();
    });

    // Lazzi — dock para gerar clipes animados via texto/script
    m_lazziWidget = new LazziDock(this);
    m_lazziWidget->setTimeline(m_timeline);
    m_lazziDock = makeDock(QStringLiteral("motionScriptDock"), tr("Lazzi"),
                          m_lazziWidget, Qt::RightDockWidgetArea);
    tabifyDockWidget(m_textDock, m_lazziDock);
    m_lazziDock->raise();
    connect(m_lazziWidget, &LazziDock::clipConverted, this, [this]() {
        statusBar()->showMessage(tr("Clip Lazzi criado na timeline."), 2500);
    });

    // Animação — presets de keyframes aplicados aos clipes selecionados.
    m_animPresetWidget = new AnimPresetDock(this);
    m_animPresetWidget->setTimeline(m_timeline);
    m_animPresetDock = makeDock(QStringLiteral("animPresetDock"),
                                tr("Animação"), m_animPresetWidget,
                                Qt::RightDockWidgetArea);
    tabifyDockWidget(m_lazziDock, m_animPresetDock);
    connect(m_animPresetWidget, &AnimPresetDock::presetsApplied, this,
            [this](int n) {
                statusBar()->showMessage(
                    tr("Preset de animação aplicado em %1 clipe(s).").arg(n),
                    2500);
            });

    // Technicolor — correção de cor e looks analógicos.
    m_colorWidget = new ColorDock(this);
    m_colorWidget->setTimeline(m_timeline);
    m_colorWidget->setPreview(m_preview);
    m_colorDock = makeDock(QStringLiteral("colorDock"), tr("Technicolor"),
                           m_colorWidget, Qt::RightDockWidgetArea);
    tabifyDockWidget(m_animPresetDock, m_colorDock);
    connect(m_timeline, &TimelineWidget::selectionChanged, m_colorWidget,
            &ColorDock::setClipById);

    m_fileBrowserDock = makeDock(QStringLiteral("fileBrowserDock"),
                                 tr("Explorador de Arquivos"),
                                 m_fileBrowser, Qt::LeftDockWidgetArea);
    m_fileBrowserDock->hide();

    // Inspector: dock à direita — o QMainWindow o posiciona entre o widget
    // central (preview) e a borda direita da janela, encostando no monitor
    // como no Premiere (passo 3 da réplica estrutural).
    m_propsDock = makeDock(QStringLiteral("propsDock"), tr("Propriedades"),
                           m_props, Qt::RightDockWidgetArea);
    m_propsDock->hide();

    // Source Monitor (dock): In/Out + Insert/Overwrite do fluxo Premiere.
    // Fica na pilha do Inspector, ao lado do Program Monitor central.
    m_source = new SourceMonitorWidget(this);
    m_source->setProject(&m_project);
    m_sourceDock = makeDock(QStringLiteral("sourceMonitorDock"), tr("Source"),
                            m_source, Qt::RightDockWidgetArea);
    tabifyDockWidget(m_propsDock, m_sourceDock);
    m_sourceDock->hide();
    connect(m_source, &SourceMonitorWidget::insertSource, m_timeline,
            &TimelineWidget::insertSourceAtPlayhead);
    connect(m_source, &SourceMonitorWidget::overwriteSource, m_timeline,
            &TimelineWidget::overwriteSourceAtPlayhead);
    connect(m_source, &SourceMonitorWidget::statusMessage, this,
            [this](const QString& msg) { statusBar()->showMessage(msg, 2500); });
    connect(m_source, &SourceMonitorWidget::insertSource, this, [this]() {
        if (m_sourceDock && !m_sourceDock->isVisible()) {
            m_sourceDock->show();
            m_sourceDock->raise();
        }
    });
    connect(m_source, &SourceMonitorWidget::overwriteSource, this, [this]() {
        if (m_sourceDock && !m_sourceDock->isVisible()) {
            m_sourceDock->show();
            m_sourceDock->raise();
        }
    });

    // Mixer — dock na parte inferior, ao lado da timeline.
    m_mixer = new MixerWidget(this);
    m_mixer->setProject(&m_project);
    m_mixer->setPreview(m_preview);
    m_mixerDock = makeDock(QStringLiteral("mixerDock"), tr("Mixer"),
                           m_mixer, Qt::BottomDockWidgetArea);
    splitDockWidget(m_timelineDock, m_mixerDock, Qt::Vertical);
    m_mixerDock->hide();

    // Equalizador (gráfico + paramétrico, master + faixas de áudio) — dock
    // tabificado com o Mixer.
    m_equalizer = new EqualizerWidget(this);
    m_equalizer->setProject(&m_project);
    m_equalizerDock = makeDock(QStringLiteral("equalizerDock"),
                               tr("Equalizador"), m_equalizer,
                               Qt::BottomDockWidgetArea);
    tabifyDockWidget(m_mixerDock, m_equalizerDock);
    m_equalizerDock->hide();

    // Mesa (composição 2D) — dock ao lado do preview.
    m_mesa = new MesaWidget(this);
    m_mesa->setProject(&m_project);
    m_mesaDock = makeDock(QStringLiteral("mesaDock"), tr("Mesa"),
                          m_mesa, Qt::RightDockWidgetArea);
    tabifyDockWidget(m_pantaloneDock, m_mesaDock);
    m_mesaDock->hide();

    // Histórico de edições (undo/redo) — dock à direita, agrupado na pilha
    // de Efeitos/Express.
    m_histList = new QListWidget(this);
    m_histList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_histList->setMinimumWidth(180);
    connect(m_histList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        jumpToUndo(item->data(Qt::UserRole).toInt());
    });
    m_histDock = makeDock(QStringLiteral("historyDock"), tr("Histórico de Edições"),
                          m_histList, Qt::RightDockWidgetArea);
    // Agrupado com Efeitos/Express, não com o Mixer: antes o Histórico era
    // tabificado contra o Mixer e acabava na área de baixo, montando uma pilha
    // de 3 abas (Mixer|Histórico|Analisadores) com ~120px de altura útil.
    tabifyDockWidget(m_expressDock, m_histDock);
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
    m_scopesDock = makeDock(QStringLiteral("scopesDock"), tr("Analisadores"),
                            scopeBox, Qt::RightDockWidgetArea);
    // Fica na pilha da Mesa (área direita, ao lado do preview), como o
    // comentário acima descreve. raise() deixa a aba Analyzer selecionada
    // quando o usuário abre o dock.
    tabifyDockWidget(m_mesaDock, m_scopesDock);
    m_scopesDock->raise();
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
    for (QDockWidget* dock : m_allDocks) {
        connect(dock, &QDockWidget::topLevelChanged, this, &MainWindow::scheduleLayoutSave);
        connect(dock, &QDockWidget::visibilityChanged, this, &MainWindow::scheduleLayoutSave);
        // Movimentação entre áreas encaixadas não emite topLevelChanged (o dock
        // nunca flutuou), então sem este sinal o rearranjo só era salvo no
        // fechamento — e se perdia num encerramento anormal.
        connect(dock, &QDockWidget::dockLocationChanged, this, &MainWindow::scheduleLayoutSave);
    }

    setTabPositionsUp();

    // Arr default do app: base de qualquer workspace ainda não salvo.
    m_defaultLayoutState = saveState();
    seedWorkspacePresets();
    rebuildWorkspaceMenu();
}

void MainWindow::applyInitialDockWidths() {
    // Só faz sentido na primeira exibição: antes disso o Qt ainda não
    // calculou o layout e resizeDocks() não teria efeito. E nunca sobrescreve
    // um arranjo já salvo pelo usuário.
    if (m_widthsApplied || m_hasRestoredLayout) return;
    m_widthsApplied = true;
    // Mídias agora tem um painel em LISTA (colunas estilo Premiere), que exige
    // largura — 260px não cabia nem a metade das colunas.
    resizeDocks({m_poolDock}, {420}, Qt::Horizontal);
    resizeDocks({m_fileBrowserDock}, {220}, Qt::Horizontal);
    resizeDocks({m_effectsDock, m_expressDock, m_histDock}, {260}, Qt::Horizontal);
    resizeDocks({m_pantaloneDock, m_mesaDock, m_scopesDock}, {280}, Qt::Horizontal);
    // Paleta de ferramentas: coluna estreita ao lado da timeline (Tools).
    if (m_toolsDock) resizeDocks({m_toolsDock}, {60}, Qt::Horizontal);
}


void MainWindow::setDockLocked(bool locked) {
    for (QDockWidget* dock : m_allDocks) {
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


