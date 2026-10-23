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
    // O preview continua como widget central: QMainWindow aceita docks nos 4
    // lados em volta do central (a premissa do ROADMAP 7.1 de que o widget
    // central "não aceita dock em volta" estava errada). Com o vídeo no
    // centro não há o bug de tamanho em que as áreas Left/Right — sem widget
    // central para ancorar — dividem a janela inteira e esticam as docks.
    auto* centralHost = new QWidget;
    m_centralLay = new QVBoxLayout(centralHost);
    m_centralLay->setContentsMargins(4, 4, 4, 4);
    m_centralLay->setSpacing(2);
    m_centralLay->addWidget(m_preview, 1);
    setCentralWidget(centralHost);
    // Aceita o drop de docks vindos da janela técnica (drag entre janelas —
    // o Qt não move docks entre QMainWindows sozinho).
    setAcceptDrops(true);

    m_pantalone = new PantaloneWidget(this);
    m_pantalone->setProject(&m_project);

    m_graph = new GraphEditorWidget(this);
    m_graph->setProject(&m_project);
    // Altura mínima é do dock (o usuário a controla), não do widget: fixar
    // aqui impedia recolher o Editor de Curvas a uma tira fina.

    // Editor de Velocidade (dock, estilo Time Remapping do Premiere).
    m_velocity = new VelocityEditorWidget(this);
    m_velocity->setProject(&m_project);

    // Pivot — edição 3D do clipe (gizmo + keyframes), mais simples que a Mesa.
    m_pivot = new PivotWidget(this);
    m_pivot->setProject(&m_project);

    // Dottore — edição de motion graphics por forma (segundo viewer, dockável
    // ao lado do Preview).
    m_dottore = new DottoreWidget(this);
    m_dottore->setProject(&m_project);

    m_effects = new EffectsWidget(this);
    m_express = new ExpressWidget(this);
    m_fileBrowser = new FileBrowserWidget(this);

    // O painel de Propriedades vira dock no passo 3 da réplica estrutural
    // (ROADMAP 7.1): era uma janela à parte (setWindowFlag(Qt::Window)),
    // agora fica ao lado do Program Monitor como no Premiere. O widget é
    // criado aqui; o dock que o contém é montado em createDocks().
    m_props = new ClipPropertiesWidget(this);
    m_props->setWindowTitle(tr("Propriedades"));
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

    // Host frei0r (padrão Kdenlive/Shotcut/MLT): escaneia /usr/lib/frei0r-1
    // e PIERROT_FREI0R_PATH. Plugins entram no painel Efeitos e no export
    // via filtro ffmpeg `frei0r=`.
    m_frei0rManager = new Frei0rPluginManager(this);
    m_frei0rManager->scanPlugins();
    m_preview->setFrei0rManager(m_frei0rManager);

    // Inicializa o painel de efeitos.
    m_effects->setProject(&m_project);
    m_effects->setOfxPlugins(m_ofxManager->plugins());
    m_effects->setFrei0rPlugins(m_frei0rManager->plugins());

    // Inicializa o Express (editor de efeitos do clipe).
    m_express->setProject(&m_project);
    m_express->setOfxPlugins(m_ofxManager->plugins());
    m_express->setFrei0rPlugins(m_frei0rManager->plugins());
    m_express->setFrei0rManager(m_frei0rManager);
    // Catálogos para o menu "+" do painel de efeitos da faixa de efeitos.
    m_timeline->setFxOfxPlugins(m_ofxManager->plugins());
    m_timeline->setFxFrei0rPlugins(m_frei0rManager->plugins());
    connect(m_express, &ExpressWidget::modified, this, &MainWindow::setModified);
    connect(m_express, &ExpressWidget::modified, this, [this]() { m_preview->refreshView(); });

    // Painel Effects (réplica Premiere): IDs especiais tratados aqui;
    // o resto vai para o Express (nativos/frei0r/OFX).
    connect(m_effects, &EffectsWidget::effectSelected, this,
            [this](const QString& id) {
        if (id.startsWith(QStringLiteral("trans:"))) {
            const QString t = id.mid(QStringLiteral("trans:").size());
            if (t == QStringLiteral("constantpower")) {
                // Crossfade de áudio: vale quando há sobreposição; só registra
                // o tipo no clipe se for transição de VÍDEO — aqui é áudio.
                statusBar()->showMessage(
                    tr("Constant Power: o crossfade de áudio já acompanha a "
                       "sobreposição na mesma faixa."), 3500);
                return;
            }
            // Transição de vídeo no clipe selecionado.
            if (!m_timeline->lastSelectedId().isEmpty()) {
                Clip* c = m_timeline->findClipById(m_timeline->lastSelectedId());
                if (c && !c->isText) {
                    emit m_timeline->editStart();
                    c->transitionType = t;
                    m_timeline->update();
                    setModified();
                    statusBar()->showMessage(
                        tr("Transição: %1 no clipe selecionado").arg(t), 2500);
                    return;
                }
            }
            statusBar()->showMessage(
                tr("Selecione um clipe de vídeo para aplicar a transição."), 3000);
            return;
        }
        if (id == QStringLiteral("pierrot_lumetri")) {
            if (!m_timeline->lastSelectedId().isEmpty())
                m_timeline->openGradingForClip(m_timeline->lastSelectedId());
            else
                statusBar()->showMessage(tr("Selecione um clipe para o Lumetri."), 3000);
            return;
        }
        if (id.startsWith(QStringLiteral("text:"))) {
            const QString act = id.mid(QStringLiteral("text:").size());
            if (act == QStringLiteral("create")) {
                m_timeline->addTextClipAt(0, m_timeline->playhead());
                setModified();
                statusBar()->showMessage(tr("Clipe de texto criado no playhead."), 2500);
                return;
            }
            if (act == QStringLiteral("edit")) {
                if (!m_timeline->lastSelectedId().isEmpty())
                    m_timeline->openTextEditorForClip(m_timeline->lastSelectedId());
                else
                    statusBar()->showMessage(
                        tr("Selecione um clipe de texto para editar."), 3000);
                return;
            }
            if (act == QStringLiteral("fadeIn") || act == QStringLiteral("fadeOut")) {
                Clip* c = m_timeline->findClipById(m_timeline->lastSelectedId());
                if (!c) {
                    statusBar()->showMessage(tr("Selecione um clipe."), 3000);
                    return;
                }
                emit m_timeline->editStart();
                const double dur = std::max(0.1, c->dur);
                if (act == QStringLiteral("fadeIn"))
                    c->fadeIn = std::min(1.0, dur * 0.25);
                else
                    c->fadeOut = std::min(1.0, dur * 0.25);
                m_timeline->update();
                setModified();
                statusBar()->showMessage(
                    act == QStringLiteral("fadeIn") ? tr("Fade In aplicado.")
                                                    : tr("Fade Out aplicado."),
                    2000);
                return;
            }
            return;
        }
        // Efeitos de vídeo/áudio/frei0r/OFX → Express.
        m_express->addEffect(id);
    });

    // Painel de efeitos da faixa de efeitos (estilo AE). O "+" grava a escolha
    // NA FAIXA (Track::fxEffects) e a aplica aos clipes que já estão nela; o
    // olho liga/desliga a escolha propagando aos clipes; o clique na linha abre
    // o efeito no Express (sobre o clipe ativo da faixa). O Lumetri continua
    // indo para o painel de correção de cor.
    auto applyFxToClip = [this](const QString& clipId, const QString& id) {
        Clip* c = m_timeline->findClipById(clipId);
        if (!c) return;
        if (id == QStringLiteral("pierrot_lumetri")) {
            m_timeline->openGradingForClip(clipId);
            return;
        }
        // Garante o clipe selecionado (seleção dispara selectionChanged, que
        // sincroniza o clipe no Express).
        if (m_timeline->lastSelectedId() != clipId)
            m_timeline->selectClip(clipId);
        m_express->addEffect(id);
        if (m_expressDock) { m_expressDock->show(); m_expressDock->raise(); }
        setModified();
        m_timeline->update();
    };
    auto findVideoTrack = [this](const QString& trackId) -> Track* {
        for (Track& t : m_project.videoTracks)
            if (t.id == trackId) return &t;
        return nullptr;
    };
    connect(m_timeline, &TimelineWidget::fxEffectAddRequested, this,
            [this, findVideoTrack](const QString& trackId, const QString& id) {
                Track* tr = findVideoTrack(trackId);
                if (!tr) return;
                pushUndo();
                // Garante a escolha na lista da faixa (idempotente).
                bool exists = false;
                for (TrackFx& fx : tr->fxEffects)
                    if (fx.id == id) { exists = true; fx.enabled = true; break; }
                if (!exists) tr->fxEffects.append(TrackFx{id, true});
                // Aplica aos clipes que já estão na faixa.
                bool any = false;
                for (Clip& c : tr->clips) {
                    if (c.isText) continue;
                    if (m_express->applyEffectToClip(&c, id)) {
                        if (!c.fromFxTrack.contains(id)) c.fromFxTrack.append(id);
                        any = true;
                    }
                }
                if (any) setModified();
                m_timeline->update();
                m_preview->refreshView();
            });
    connect(m_timeline, &TimelineWidget::fxEffectRemoveRequested, this,
            [this, findVideoTrack](const QString& trackId, const QString& id) {
                Track* tr = findVideoTrack(trackId);
                if (!tr) return;
                pushUndo();
                // Remove a escolha da faixa.
                for (int i = 0; i < tr->fxEffects.size(); ++i) {
                    if (tr->fxEffects[i].id == id) {
                        tr->fxEffects.removeAt(i);
                        break;
                    }
                }
                // Remove o efeito dos clipes que estão na faixa.
                bool any = false;
                for (Clip& c : tr->clips) {
                    if (m_express->removeEffectFromClip(&c, id)) any = true;
                    c.fromFxTrack.removeAll(id);
                }
                if (any) m_preview->refreshView();
                setModified();
                m_timeline->update();
            });
    connect(m_timeline, &TimelineWidget::fxEffectEditRequested, this,
            [applyFxToClip](const QString& clipId, const QString& id) {
                applyFxToClip(clipId, id);
            });
    connect(m_timeline, &TimelineWidget::fxEffectToggleRequested, this,
            [this, findVideoTrack](const QString& trackId, const QString& id, bool enabled) {
                Track* tr = findVideoTrack(trackId);
                if (!tr) return;
                pushUndo();
                for (TrackFx& fx : tr->fxEffects)
                    if (fx.id == id) { fx.enabled = enabled; break; }
                bool any = false;
                for (Clip& c : tr->clips) {
                    if (c.isText) continue;
                    if (enabled) {
                        // Cria a instância se faltar; se já existe (desligada
                        // pelo olho), apenas religa.
                        bool ok = m_express->applyEffectToClip(&c, id);
                        if (!ok) ok = m_timeline->setFxEffectEnabled(&c, id, true);
                        if (ok) {
                            if (!c.fromFxTrack.contains(id)) c.fromFxTrack.append(id);
                            any = true;
                        }
                    } else if (m_timeline->setFxEffectEnabled(&c, id, false)) any = true;
                }
                if (any) setModified();
                m_timeline->update();
                m_preview->refreshView();
            });
    connect(m_timeline, &TimelineWidget::fxTrackClipAdded, this,
            [this, findVideoTrack](const QString& trackId, const QString& clipId) {
                Track* tr = findVideoTrack(trackId);
                Clip* c = m_timeline->findClipById(clipId);
                if (!tr || !c) return;
                // Copia os efeitos habilitados da faixa para o clipe.
                bool any = false;
                for (const TrackFx& fx : tr->fxEffects) {
                    if (!fx.enabled) continue;
                    if (m_express->applyEffectToClip(c, fx.id)) {
                        if (!c->fromFxTrack.contains(fx.id)) c->fromFxTrack.append(fx.id);
                        any = true;
                    }
                }
                if (any) {
                    setModified();
                    m_timeline->update();
                    m_preview->refreshView();
                }
            });
    connect(m_timeline, &TimelineWidget::fxTrackClipRemoved, this,
            [this](const QString&, const QString& clipId) {
                // O clipe saiu de uma faixa de efeitos: "sai como entrou",
                // sem os efeitos copiados da faixa.
                Clip* c = m_timeline->findClipById(clipId);
                if (!c || c->fromFxTrack.isEmpty()) return;
                bool any = false;
                for (const QString& id : c->fromFxTrack)
                    if (m_express->removeEffectFromClip(c, id)) any = true;
                c->fromFxTrack.clear();
                if (any) m_preview->refreshView();
                m_timeline->update();
            });
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
        // Editor de Texto (dock): edita o texto do clipe selecionado.
        m_textEditor->setContext(&m_project,
                                 (clip && (clip->isText
                                           || !m_project.textStyleFor(*clip)->isEmpty()))
                                     ? clip : nullptr);
    });

    createDocks();
    createActions();

    // Transporte do projeto foi movido para DENTRO do monitor (Program Monitor
    // do Premiere): o próprio PreviewWidget tem quadro anterior/play/pausa/
    // quadro seguinte/loop no topo. Aqui só restam os saltos Home/End, que
    // continuam como atalhos da janela (sem barra, como no Premiere).
    QAction* goToStart = new QAction(iconSkipBack(), tr("Início"), this);
    goToStart->setToolTip(tr("Ir para o início (Home)"));
    goToStart->setShortcut(QKeySequence(Qt::Key_Home));
    connect(goToStart, &QAction::triggered, this, [this]() {
        m_timeline->setPlayhead(0.0);
        m_preview->seek(0.0);
        m_timeline->update();
    });

    QAction* stepBack = new QAction(iconStepBack(), tr("Voltar 1 frame"), this);
    stepBack->setToolTip(tr("Voltar 1 frame (←)"));
    connect(stepBack, &QAction::triggered, this, [this]() {
        Project* p = m_timeline->project();
        const double t = m_timeline->playhead() - 1.0 / (p ? p->fps : 30.0);
        m_timeline->setPlayhead(std::max(0.0, t));
        m_preview->seek(m_timeline->playhead());
        m_timeline->update();
    });

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
    // Sem barra visual: Home/End e Espaço (play) continuam como atalhos da
    // janela. O play em si vive no monitor (ícone ▶/❚❚ do PreviewWidget).
    addAction(goToStart);
    addAction(goToEnd);
    addAction(m_playAction);

    // Intercepta setas ←/→ globalmente via eventFilter no qApp.
    qApp->installEventFilter(this);

    m_undoStack.clear();
    m_undoStack.push_back(snapshotState());
    m_undoLabels.push_back(tr("Início"));
    m_undoBytes = m_undoStack.front().size();
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
        if (m_equalizer) m_equalizer->refresh();
        if (m_mesa) m_mesa->refresh();
    });
    // Durante o arraste, a timeline só avisa que a GEOMETRIA mudou: repinta e
    // recomputa o preview, sem o rebuild de painéis (mixer/eq/mesa/props) que
    // o modified() faz. Esse rebuild sai uma única vez no fim do gesto (o
    // modified() emitido no mouseRelease). Era o rebuild por mousemove que
    // deixava os gestos com latência perceptível.
    connect(m_timeline, &TimelineWidget::clipGeometryChanged, this, [this]() {
        m_preview->refreshView();
    });
    connect(m_timeline, &TimelineWidget::modified, this, &MainWindow::setModified);
    connect(m_mixer, &MixerWidget::modified, this, &MainWindow::pushUndo);
    connect(m_mixer, &MixerWidget::modified, this, &MainWindow::setModified);
    connect(m_mixer, &MixerWidget::modified, this, [this]() {
        m_preview->refreshView();
    });
    // Equalizador (gráfico + paramétrico): uma entrada de undo por rajada de
    // edição; mudanças em vivo atualizam preview/timeline.
    connect(m_equalizer, &EqualizerWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_equalizer, &EqualizerWidget::modified, this, &MainWindow::setModified);
    connect(m_equalizer, &EqualizerWidget::modified, this, [this]() {
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
    // Malha 3D (.obj ou .blend via Blender CLI) como camada da Mesa.
    connect(m_mesa, &MesaWidget::mesaAddMeshRequested,
            this, [this](const QString& path) {
        if (path.endsWith(QLatin1String(".blend"), Qt::CaseInsensitive)) {
            if (m_project.mesas.isEmpty() || (m_mesa && m_mesa->mesaId().isEmpty()))
                m_timeline->criarMesa();
            const QString mesaId = m_mesa && !m_mesa->mesaId().isEmpty()
                                      ? m_mesa->mesaId()
                                      : (m_project.mesas.isEmpty()
                                             ? QString()
                                             : m_project.mesas.last().id);
            if (mesaId.isEmpty()) {
                statusBar()->showMessage(tr("Crie uma Mesa para importar .blend."), 3000);
                return;
            }
            statusBar()->showMessage(tr("Convertendo .blend com o Blender…"), 0);
            const QString err = m_timeline->importBlendAsMesh(mesaId, path);
            // Antes isto rodava incondicionalmente e anunciava sucesso mesmo
            // com a conversão falha — o erro só ia para o log.
            if (!err.isEmpty())
                statusBar()->showMessage(tr("Falha no .blend — %1").arg(err), 8000);
            else
                statusBar()->showMessage(tr("Importação .blend concluída."), 3500);
            return;
        }
        // Drop em Mesa sem composição: cria uma e usa o path pendente.
        if (m_project.mesas.isEmpty() || (m_mesa && m_mesa->mesaId().isEmpty())) {
            m_timeline->criarMesa();
            const QString mesaId = m_project.mesas.isEmpty()
                                      ? QString()
                                      : m_project.mesas.last().id;
            if (!mesaId.isEmpty()) {
                m_timeline->addMeshToMesa(mesaId, path);
                return;
            }
        }
        const QString mesaId = m_mesa ? m_mesa->mesaId() : QString();
        if (mesaId.isEmpty()) {
            statusBar()->showMessage(tr("Crie uma Mesa para importar .obj."), 3000);
            return;
        }
        m_timeline->addMeshToMesa(mesaId, path);
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
    connect(m_timeline, &TimelineWidget::velocityRequested, this, [this](const QString& id) {
        if (!m_velocity || !m_velocityDock) return;
        m_velocity->setClipId(id);
        m_velocityDock->show();
        m_velocityDock->raise();
    });
    connect(m_timeline, &TimelineWidget::pivotRequested, this, [this](const QString& id) {
        if (!m_pivot || !m_pivotDock) return;
        m_pivot->setClipId(id);
        m_pivotDock->show();
        m_pivotDock->raise();
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
    // Duplo clique na pool → Source Monitor (fluxo Premiere).
    connect(m_pool, &MediaPoolWidget::mediaToSource, this,
            [this](const QString& id) {
        if (!m_source || !m_sourceDock) return;
        m_source->openMedia(id);
        m_sourceDock->show();
        m_sourceDock->raise();
        m_source->setFocus(Qt::OtherFocusReason);
    });
    // .obj na pool/timeline → malha da Mesa (não é mídia FFmpeg).
    connect(m_pool, &MediaPoolWidget::objImportRequested, this,
            [this](const QStringList& paths) {
        if (paths.isEmpty()) return;
        // Precisa de uma Mesa: cria se não houver; senão usa a corrente.
        if (m_project.mesas.isEmpty()) {
            m_timeline->criarMesa();
        }
        const QString mesaId = m_mesa ? m_mesa->mesaId()
                                      : (m_project.mesas.isEmpty()
                                             ? QString()
                                             : m_project.mesas.last().id);
        if (mesaId.isEmpty()) {
            statusBar()->showMessage(tr("Crie uma Mesa antes de importar .obj."), 3500);
            return;
        }
        if (!m_mesaDock->isVisible()) {
            m_mesaDock->show();
            m_mesaDock->raise();
        }
        for (const QString& p : paths)
            m_timeline->addMeshToMesa(mesaId, p);
        statusBar()->showMessage(
            tr("Malha 3D importada na Mesa (%1).").arg(paths.size()), 3000);
    });
    // .blend → Blender CLI → malha 3D.
    connect(m_pool, &MediaPoolWidget::blendImportRequested, this,
            [this](const QStringList& paths) {
        if (paths.isEmpty()) return;
        if (m_project.mesas.isEmpty()) m_timeline->criarMesa();
        const QString mesaId = m_mesa ? m_mesa->mesaId()
                                      : (m_project.mesas.isEmpty()
                                             ? QString()
                                             : m_project.mesas.last().id);
        if (mesaId.isEmpty()) {
            statusBar()->showMessage(tr("Crie uma Mesa antes de importar .blend."), 3500);
            return;
        }
        if (!m_mesaDock->isVisible()) {
            m_mesaDock->show();
            m_mesaDock->raise();
        }
        statusBar()->showMessage(tr("Convertendo .blend com o Blender…"), 0);
        int ok = 0;
        QStringList failed;
        for (const QString& p : paths) {
            if (m_timeline->importBlendAsMesh(mesaId, p).isEmpty()) ++ok;
            else failed.append(QFileInfo(p).fileName());
        }
        if (!failed.isEmpty())
            statusBar()->showMessage(
                tr("Importação .blend: %1 de %2 ok. Falhou: %3")
                    .arg(ok).arg(paths.size()).arg(failed.join(QStringLiteral(", "))),
                8000);
        else
            statusBar()->showMessage(
                tr("Importação .blend concluída (%1 arquivo(s)).").arg(ok), 4000);
    });
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

    connect(m_timeline, &TimelineWidget::selectionChanged, m_pantalone, &PantaloneWidget::setClipId);
    connect(m_timeline, &TimelineWidget::playheadChanged, m_pantalone, &PantaloneWidget::setPlayhead);
    connect(m_timeline, &TimelineWidget::selectionChanged, m_graph, &GraphEditorWidget::setClipId);
    connect(m_timeline, &TimelineWidget::playheadChanged, m_graph, &GraphEditorWidget::setPlayhead);
    connect(m_timeline, &TimelineWidget::selectionChanged, m_velocity, &VelocityEditorWidget::setClipId);
    connect(m_timeline, &TimelineWidget::playheadChanged, m_velocity, &VelocityEditorWidget::setPlayhead);
    connect(m_timeline, &TimelineWidget::selectionChanged, m_pivot, &PivotWidget::setClipId);
    connect(m_timeline, &TimelineWidget::playheadChanged, m_pivot, &PivotWidget::setPlayhead);
    connect(m_pivot, &PivotWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_pivot, &PivotWidget::modified, this, [this]() {
        m_timeline->update();
        m_preview->refreshView();
        setModified();
    });
    connect(m_timeline, &TimelineWidget::selectionChanged, m_dottore, &DottoreWidget::setClipId);
    connect(m_timeline, &TimelineWidget::playheadChanged, m_dottore, &DottoreWidget::setPlayhead);
    connect(m_dottore, &DottoreWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_dottore, &DottoreWidget::modified, this, [this]() {
        m_timeline->update();
        m_timeline->invalidateScene();
        m_preview->refreshView();
        if (m_graph) m_graph->refresh();
        setModified();
    });
    connect(m_dottore, &DottoreWidget::keyframesChanged, this, [this]() {
        if (m_graph) m_graph->refresh();
    });
    connect(m_velocity, &VelocityEditorWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_velocity, &VelocityEditorWidget::modified, this, [this]() {
        m_timeline->update();
        m_preview->refreshView();
        setModified();
    });
    connect(m_timeline, &TimelineWidget::modified, m_velocity, [this]() {
        // Rele o clipe ativo após edições da timeline (split/move).
        if (m_velocity && m_timeline)
            m_velocity->setClipId(m_timeline->lastSelectedId());
    });
    connect(m_timeline, &TimelineWidget::playheadChanged, m_props, &ClipPropertiesWidget::setPlayhead);
    connect(m_timeline, &TimelineWidget::playheadChanged, this, [this](double t) {
        if (m_mesa) { m_mesa->setPlayheadPosition(t); m_mesa->refresh(); }
    });
    connect(m_graph, &GraphEditorWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_graph, &GraphEditorWidget::modified, this, [this]() {
        m_timeline->update();
        m_preview->refreshView();
        m_pantalone->sync();
        setModified();
    });
    connect(m_timeline, &TimelineWidget::modified, m_graph, &GraphEditorWidget::refresh);
    connect(m_pantalone, &PantaloneWidget::modified, m_graph, &GraphEditorWidget::refresh);
    // Correspondência pantalone ↔ editor de curvas: ao animar uma propriedade
    // no pantalone, o editor de curvas exibe a curva correspondente.
    connect(m_pantalone, &PantaloneWidget::propertyEdited, this, [this](int prop) {
        // Props de máscara: mostra a curva da máscara ATIVA no editor de curvas.
        if (prop >= PantaloneWidget::P_MaskCx) {
            GraphProp gp;
            switch (prop) {
                case PantaloneWidget::P_MaskCx: gp = GPropMaskCx; break;
                case PantaloneWidget::P_MaskCy: gp = GPropMaskCy; break;
                case PantaloneWidget::P_MaskRx: gp = GPropMaskRx; break;
                case PantaloneWidget::P_MaskRy: gp = GPropMaskRy; break;
                case PantaloneWidget::P_MaskRot: gp = GPropMaskRot; break;
                case PantaloneWidget::P_MaskFeather: gp = GPropMaskFeather; break;
                default: return;
            }
            m_graph->setActiveMask(m_pantalone->currentMaskIndex(), gp);
            return;
        }
        GraphProp gp;
        switch (prop) {
            case PantaloneWidget::P_CropL: gp = GPropCropL; break;
            case PantaloneWidget::P_CropR: gp = GPropCropR; break;
            case PantaloneWidget::P_CropT: gp = GPropCropT; break;
            case PantaloneWidget::P_CropB: gp = GPropCropB; break;
            case PantaloneWidget::P_Scale: gp = GPropScale; break;
            case PantaloneWidget::P_PanX:  gp = GPropTx; break;
            case PantaloneWidget::P_PanY:  gp = GPropTy; break;
            case PantaloneWidget::P_Rotation: gp = GPropRotation; break;
            case PantaloneWidget::P_Opacity: gp = GPropOpacity; break;
            default: return;
        }
        m_graph->setProperty(gp);
    });
    connect(m_timeline, &TimelineWidget::pantaloneRequested, this, [this](const QString& id) {
        m_pantalone->setClipId(id);
        m_pantaloneDock->show();
        m_pantaloneDock->raise();
    });
    connect(m_timeline, &TimelineWidget::maskRequested, this, &MainWindow::openMaskEditor);
    connect(m_pantalone, &PantaloneWidget::keyframeJump, this, [this](double t) {
        m_timeline->setPlayhead(t);
        m_preview->seek(t);
        m_pantalone->setPlayhead(t);
        m_graph->setPlayhead(t);
    });
    connect(m_graph, &GraphEditorWidget::keyframeJump, this, [this](double t) {
        m_timeline->setPlayhead(t);
        m_preview->seek(t);
        m_pantalone->setPlayhead(t);
        m_graph->setPlayhead(t);
    });
    connect(m_pantalone, &PantaloneWidget::editStart, this, &MainWindow::pushUndo);
    connect(m_pantalone, &PantaloneWidget::modified, this, [this]() {
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

    // Barrinha de atividade: ondas/thumbnails (MediaCache) E geração de
    // proxy (ProxyManager) — o proxy de um 4K leva minutos e era a maior
    // importação do app sem nenhum feedback.
    m_busyBar = new QProgressBar(this);
    m_busyBar->setRange(0, 0);
    m_busyBar->setFixedWidth(150);
    m_busyBar->setFixedHeight(14);
    m_busyBar->setTextVisible(false);
    m_busyBar->hide();
    statusBar()->addWidget(m_busyBar);
    connect(&MediaCache::instance(), &MediaCache::busyChanged, this, [this](bool busy) {
        m_bgCacheBusy = busy;
        refreshBackgroundBusy();
    });
    connect(&ProxyManager::instance(), &ProxyManager::progressChanged, this,
            [this](int pending, bool running) {
        m_bgProxyBusy = running || pending > 0;
        m_proxyPending = pending;
        refreshBackgroundBusy();
    });

    // Depuração manual: PIERROT_PERF_JSON=/caminho grava o relatório de
    // performance também no fechamento normal (antes só o harness
    // --autoplay gravava — o teste manual nunca deixava métricas).
    if (PreviewProfiler::active()) {
        connect(qApp, &QCoreApplication::aboutToQuit, this, []() {
            const QString out = qEnvironmentVariable("PIERROT_PERF_JSON");
            if (!out.isEmpty() && PreviewProfiler::instance().dumpJson(out))
                qInfo("[perf] relatório gravado em %s", qPrintable(out));
        });
    }

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

// Workspaces (roadmap 0.7). O workspace corrente vive nas chaves "layout" /
// "layoutVersion" de sempre — é o que saveSettings()/restoreSettings() já
// gravam. Assim o layout que o usuário já tem se torna o workspace "Edição"
// sem migração, e cada workspace nomeado extra tem seu próprio blob.
void MainWindow::closeEvent(QCloseEvent* event) {
    // Se houver alterações não salvas, pergunta antes de sair.
    if (!confirmDiscardChanges()) { event->ignore(); return; }
    // Espera uma gravação em voo terminar (normalmente alguns ms) para não
    // descartar o último save antes de o processo sair.
    if (m_saveBusy && QThreadPool::globalInstance())
        QThreadPool::globalInstance()->waitForDone();
    saveSettings();
    // Janelas auxiliares da bancada: fechar TUDO junto com a principal.
    // Sem isto, o painel técnico (QMainWindow de nível superior) e o preview
    // externo podiam sobreviver ao fecho da principal — o processo "ficava
    // rodando em segundo plano" depois de o usuário achar que tinha saído.
    if (m_monitor) m_monitor->close(); // WA_DeleteOnClose → destroyed
    if (m_techWindow) {
        m_techWindow->returnAllDocks();
        m_techWindow->hide();
    }
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
    box.addButton(tr("Descartar"), QMessageBox::DestructiveRole);
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

// Mostra o painel de Propriedades, que agora é o dock "propsDock" (passo 3 da
// réplica estrutural). Sem janela para posicionar: basta revelar e trazer para
// frente, que ele acompanha a seleção enquanto estiver aberto.
void MainWindow::showPropsWindow() {
    if (!m_propsDock) return;
    m_propsDock->show();
    m_propsDock->raise();
}

void MainWindow::openMaskEditor(const QString& id) {
    Clip* clip = m_timeline->findClipById(id);
    if (!clip) return;

    // Reaproveita a janela para o MESMO clipe; para outro, troca de alvo.
    if (m_maskDialog && m_maskDialogClipId == id) {
        m_maskDialog->raise();
        m_maskDialog->activateWindow();
        return;
    }
    if (m_maskDialog) {
        m_maskDialog->close();
        m_preview->setMaskOverlay(QString(), {});
    }
    m_maskDialogClipId = id;

    auto* dlg = new MaskEditorDialog(clip, this);
    m_maskDialog = dlg;

    // Overlay no monitor: a cópia de trabalho do dialog vira as formas
    // desenhadas sobre o preview (o clipe só muda ao Aplicar).
    m_preview->setMaskOverlay(id, dlg->masks());
    connect(dlg, &MaskEditorDialog::masksChanged, this, [this, id](const QVector<Mask>& masks) {
        m_preview->setMaskOverlay(id, masks);
    });
    // Arrasto no preview → dialog (atualiza sliders e a cópia em tempo real).
    connect(m_preview, &PreviewWidget::maskEdited, dlg, &MaskEditorDialog::applyExternalEdit);

    // Commit ("Aplicar"): undo antes de gravar, estado sujo após (padrão
    // TransformDialog/ClipPropertiesWidget).
    connect(dlg, &MaskEditorDialog::editStart, this, &MainWindow::pushUndo);
    connect(dlg, &MaskEditorDialog::modified, this, [this]() {
        m_timeline->update();
        m_preview->refreshView();
        setModified();
    });

    // Ao fechar sem aplicar (cancelar/✕), o overlay some. Ao aplicar, o preview
    // já renderiza a máscara no próprio vídeo, então o overlay se desliga junto.
    connect(dlg, &QDialog::rejected, this, [this]() {
        m_maskDialogClipId.clear();
        m_preview->setMaskOverlay(QString(), {});
    });
    connect(dlg, &QDialog::accepted, this, [this]() {
        m_maskDialogClipId.clear();
        m_preview->setMaskOverlay(QString(), {});
    });

    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
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
    // Depois do restoreState(): um arranjo salvo tem prioridade sobre as
    // larguras padrão, senão a primeira abertura sobrescreveria o layout.
    applyInitialDockWidths();
    // Features de dock aplicadas antes do show() podem ser redefinidas quando
    // o Qt monta o layout dos painéis na primeira exibição. Reaplica o
    // travamento agora para o cadeado valer de verdade no início.
    setDockLocked(m_lockAction->isChecked());
}

// Boilerplate comum a todo painel dockável: objectName estável (é por ele que
// saveState/restoreState casam o dock ao layout salvo — não renomear sem
// bumpingar kLayoutVersion), áreas permitidas e features. Registra o dock em
// m_allDocks.
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
    ProjectSettingsDialog dlg(m_project.width, m_project.height, m_project.fps,
                              m_project.useProxies, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const bool resChanged = dlg.width() != m_project.width
                            || dlg.height() != m_project.height
                            || dlg.fps() != m_project.fps;
    const bool proxyChanged = dlg.usesProxies() != m_project.useProxies;
    if (!resChanged && !proxyChanged)
        return;

    m_pendingUndoLabel = tr("Configurações do projeto");
    pushUndo();
    m_project.width = dlg.width();
    m_project.height = dlg.height();
    m_project.fps = dlg.fps();
    m_project.useProxies = dlg.usesProxies();
    if (proxyChanged)
        ProxyManager::instance().setProjectUsesProxies(m_project.useProxies);
    // pushUndo() já chamou setModified() → touch() → caches de composição
    // invalidados, então o preview passa a decodificar a fonte certa.
    m_timeline->setProject(&m_project);
    m_pool->refreshFromProject();
    m_pantalone->setProject(&m_project);
    m_preview->refreshView();
    statusBar()->showMessage(tr("Configurações do projeto atualizadas."));
}

void MainWindow::exportVideo() {
    ExportDialog dlg(&m_project, this);
    dlg.setOfxManager(m_ofxManager);
    dlg.setFrei0rManager(m_frei0rManager);
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
    ++m_projGen; // troca de projeto: invalida callbacks de save em voo
    if (m_project.videoTracks.isEmpty()) m_project.addTrack(false);
    if (m_project.audioTracks.isEmpty()) m_project.addTrack(true);
    m_undoStack.clear();
    m_undoStack.push_back(snapshotState());
    m_undoLabels.clear();
    m_undoLabels.push_back(tr("Início"));
    m_undoBytes = m_undoStack.front().size();
    m_undoIndex = 0;
    updateHistoryList();
    m_currentFile.clear();
    m_modified = true;
    ProxyManager::instance().setProjectUsesProxies(m_project.useProxies);
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
