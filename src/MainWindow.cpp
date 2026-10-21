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
// Devolve o atalho salvo pelo usuário (Configurações → Atalhos) ou o padrão.
static QKeySequence appKey(const char* id, const QKeySequence& fallback) {
    const QString v = QSettings().value(QStringLiteral("shortcuts/") + QLatin1String(id)).toString();
    return v.isEmpty() ? fallback : QKeySequence(v);
}

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

namespace {
// Versão do arranjo de painéis (docks/toolbar). Aumente para descartar
// estados salvos antigos que estejam com o layout deslocado.
// 4: o Histórico saiu da pilha do Mixer e os Analisadores foram para a pilha
//    da Mesa. Sem este bump, um layout salvo com a pilha de 3 abas embaixo
//    continuaria sendo restaurado e a correção não apareceria.
// 5: o preview saiu de setCentralWidget e virou o dock "Program Monitor"
//    (passo 1 da réplica estrutural do Premiere, ROADMAP 7.1). Precisa do bump
//    porque os layouts antigos não conhecem o dock previewDock e o
//    restoreState() reposicionaria o vídeo numa área que não existe mais.
// 6: revertido — o preview voltou a ser widget central (a premissa do passo 1
//    estava errada: QMainWindow aceita docks em volta do central), e o dock
//    previewDock deixou de existir. Bump para descartar layouts da v5 que
//    ainda referenciam o dock fantasma.
// 8: o dock "Motion Script" (editor de animação por texto) entrou na pilha
//    direita. Bump para que layouts antigos não o mantenham escondido.
constexpr int kLayoutVersion = 8;

// Número máximo de cópias do backup rotativo (~/Pierrot/backups/).
constexpr int kBackupCopies = 10;

// Resultado da gravação em disco feita no worker do save assíncrono.
struct SaveWriteResult {
    bool ok = false;
    QString detail; // mensagem de erro se !ok
};

// Backup rotativo: ~/Pierrot/backups/<nome>_AAAA-MM-DD_HH-MM-SS.Blanc, mantendo
// no máximo kBackupCopies cópias do mesmo projeto. Chamado a cada save bem-
// sucedido (manual ou autosave), dentro do worker. Melhor esforço: falha
// silenciosa — nunca bloqueia nem interfere com o save.
void rotatingBackup(const QString& sourcePath) {
    const QFileInfo src(sourcePath);
    QDir dir(QDir::homePath());
    if (!dir.mkpath(QStringLiteral("Pierrot/backups"))) return;
    dir.cd(QStringLiteral("Pierrot/backups"));

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    const QString dest = dir.filePath(src.completeBaseName() + QLatin1Char('_') + stamp
                                      + QLatin1String(".Blanc"));
    if (QFile::copy(sourcePath, dest)) {
        const QString base = src.completeBaseName() + QLatin1Char('_');
        QStringList candidates;
        const QStringList entries = dir.entryList(QStringList() << base + QLatin1String("*.Blanc"),
                                                  QDir::Files, QDir::NoSort);
        for (const QString& e : entries) candidates << dir.filePath(e);
        std::sort(candidates.begin(), candidates.end(),
                  [](const QString& a, const QString& b) {
                      return QFileInfo(a).lastModified() > QFileInfo(b).lastModified();
                  });
        while (candidates.size() > static_cast<std::size_t>(kBackupCopies)) {
            QFile::remove(candidates.takeLast());
        }
    }
}

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
QStringList MainWindow::workspaceNames() const {
    QSettings settings;
    QStringList names = settings.value("workspaces/names").toStringList();
    // O slot corrente sempre existe como workspace, mesmo sem chave própria.
    if (m_currentWorkspace.isEmpty() || !names.contains(m_currentWorkspace))
        names.prepend(m_currentWorkspace.isEmpty() ? QStringLiteral("Edição")
                                                  : m_currentWorkspace);
    return names;
}

void MainWindow::captureCurrentWorkspace() {
    if (m_currentWorkspace.isEmpty()) return;
    QSettings settings;
    settings.setValue(QStringLiteral("workspaces/%1/state").arg(m_currentWorkspace),
                      saveState());
    settings.setValue(QStringLiteral("workspaces/%1/version").arg(m_currentWorkspace),
                      kLayoutVersion);
}

// ── Presets de workspace (estilo Premiere) ──────────────────────────────
// Não usam restoreState: show/hide + resizeDocks. Assim funcionam mesmo com
// layouts salvos de outras versões de kLayoutVersion.

void MainWindow::applyWorkspacePreset(const QString& name) {
    // Devolve os docks da janela técnica (preset Bancada) antes de rearranjar:
    // todo preset começa com os painéis de volta na janela principal.
    if (m_techWindow) {
        m_techWindow->returnAllDocks();
        m_techWindow->hide();
    }
    auto show = [](QDockWidget* d, bool on) {
        if (!d) return;
        if (on) d->show();
        else d->hide();
    };
    // Base: tudo que não é essencial escondido; o usuário reabre se quiser.
    show(m_fileBrowserDock, false);
    show(m_effectsDock, false);
    show(m_expressDock, false);
    show(m_histDock, false);
    show(m_pantaloneDock, false);
    show(m_mesaDock, false);
    show(m_scopesDock, false);
    show(m_mixerDock, false);
    show(m_propsDock, false);
    show(m_sourceDock, false);
    show(m_graphDock, false);
    show(m_velocityDock, false);
    show(m_pivotDock, false);
    show(m_poolDock, true);
    show(m_timelineDock, true);
    if (m_toolsDock) m_toolsDock->show();

    if (name == QStringLiteral("Áudio")) {
        show(m_mixerDock, true);
        show(m_scopesDock, true);
        if (m_mixerDock && m_timelineDock)
            splitDockWidget(m_timelineDock, m_mixerDock, Qt::Vertical);
        resizeDocks({m_timelineDock, m_mixerDock}, {70, 30}, Qt::Vertical);
        show(m_poolDock, true);
    } else if (name == QStringLiteral("Composição")) {
        show(m_mesaDock, true);
        show(m_pantaloneDock, true);
        show(m_scopesDock, true);
        // O Pivot é ferramenta 3D do clipe: fica junto da Mesa e do Pantalone.
        // Sem isto ele só aparecia pelo menu de contexto do clipe, o que na
        // prática significava "não existe".
        show(m_pivotDock, true);
        if (m_mesaDock) m_mesaDock->raise();
        if (m_pivotDock) m_pivotDock->raise();
        resizeDocks({m_poolDock}, {280}, Qt::Horizontal);
        if (m_mesaDock) resizeDocks({m_mesaDock}, {360}, Qt::Horizontal);
    } else if (name == QStringLiteral("Efeitos")) {
        show(m_effectsDock, true);
        show(m_expressDock, true);
        show(m_propsDock, true);
        if (m_effectsDock) m_effectsDock->raise();
        resizeDocks({m_effectsDock, m_expressDock, m_propsDock}, {280}, Qt::Horizontal);
    } else if (name == QStringLiteral("Bancada")) {
        // Bancada de editorial (referência: estúdios profissionais, ex. a
        // bancada da Disney em "Shaping the Story"): centro = monitores
        // Source + Program, esquerda = bins (Central de Mídias), direita =
        // parte técnica (Filtros/Analisadores), baixo = timeline grande.
        // Com 3+ telas o arranjo se estende: janela técnica (Mixer/Efeitos/
        // Express/…) na tela esquerda, janela principal no centro e o monitor
        // de REFERÊNCIA em tela cheia na direita (ver arrangeBancadaScreens).
        show(m_sourceDock, true);
        show(m_effectsDock, true);
        show(m_scopesDock, true);
        show(m_mixerDock, true);
        if (m_sourceDock) m_sourceDock->raise();
        if (m_effectsDock) m_effectsDock->raise();
        const int nScreens = QGuiApplication::screens().size();
        if (nScreens < 3) {
            // Sem 3ª tela o mixer fica embaixo da timeline (na própria janela).
            if (m_timelineDock && m_mixerDock)
                splitDockWidget(m_timelineDock, m_mixerDock, Qt::Vertical);
            resizeDocks({m_timelineDock, m_mixerDock}, {65, 35}, Qt::Vertical);
        } else {
            // Com 3ª tela mixer/efeitos/express/analisadores vão para a
            // janela técnica; a timeline fica com a faixa inferior inteira.
            show(m_expressDock, true);
        }
        resizeDocks({m_poolDock}, {340}, Qt::Horizontal);
        // Monitor de referência: abre o preview externo (se ainda fechado) e,
        // quando houver telas sobrando, o leva para lá em tela cheia.
        if (m_monitorAction && !m_monitorAction->isChecked())
            m_monitorAction->setChecked(true); // toggled → cria e mostra
        arrangeBancadaScreens();
    } else {
        // Edição / default: timeline grande, pool à esquerda, resto fechado.
        resizeDocks({m_poolDock}, {420}, Qt::Horizontal);
        resizeDocks({m_timelineDock}, {220}, Qt::Vertical);
        if (m_graphDock) show(m_graphDock, true);
    }
}

// Monitor de referência do preset "Bancada": se houver um segundo monitor,
// leva o preview externo para lá em tela cheia (a janela fica "fora" da
// bancada, como na referência dos estúdios). Sem segunda tela, não faz nada —
// o usuário continua podendo arrastar a janela manualmente.
void MainWindow::placeMonitorOnSecondaryScreen() {
    if (!m_monitor) return;
    QScreen* primary = QGuiApplication::screenAt(frameGeometry().center());
    if (!primary) primary = QGuiApplication::primaryScreen();
    QScreen* secondary = nullptr;
    for (QScreen* s : QGuiApplication::screens()) {
        if (s != primary) {
            secondary = s;
            break;
        }
    }
    if (!secondary) return;
    m_monitor->placeFullScreenOn(secondary);
}

// Bancada em 3+ telas: esquerda = janela técnica (Mixer/Efeitos/Express/…,
// escolhível pelo menu "Painéis" da própria janela), centro = janela
// principal, direita = monitor de referência. Com 1–2 telas, só o monitor de
// referência na secundária (comportamento original).
void MainWindow::arrangeBancadaScreens() {
    QList<QScreen*> screens = QGuiApplication::screens();
    std::sort(screens.begin(), screens.end(),
              [](QScreen* a, QScreen* b) { return a->geometry().x() < b->geometry().x(); });
    if (screens.isEmpty()) return;

    if (screens.size() >= 3) {
        QScreen* left = screens.first();
        QScreen* right = screens.last();
        QScreen* center = screens[screens.size() / 2];

        // Geometrias salvas (bancada/…): a coordenada global encodeia a
        // TELA — se ela ainda intersecta um monitor conectado, restaura;
        // senão cai no arranjo determinístico de novo.
        QSettings settings;
        const QRect techSaved =
            settings.value(QStringLiteral("bancada/techGeometry")).toRect();
        const QRect monSaved =
            settings.value(QStringLiteral("bancada/monitorGeometry")).toRect();
        const bool monFullScreen =
            settings.value(QStringLiteral("bancada/monitorFullScreen"), true).toBool();
        auto screenOf = [](const QRect& r) -> QScreen* {
            return r.isValid() ? QGuiApplication::screenAt(r.center()) : nullptr;
        };
        QScreen* techScreen = screenOf(techSaved);
        QScreen* monScreen = screenOf(monSaved);

        // Janela principal: com arranjo salvo, a geometria restaurada do app
        // (restoreGeometry) já a devolveu para a tela certa — não mexe.
        // Sem arranjo (primeira vez / telas mudaram), distribui de novo.
        if (!techScreen || !monScreen) {
            setGeometry(center->availableGeometry());
            showMaximized();
        }

        // Janela técnica à esquerda (ou na tela salva) com o set técnico
        // padrão. O usuário refina pelo menu "Painéis".
        ensureTechnicalWindow();
        for (QDockWidget* d : {m_mixerDock, m_effectsDock, m_expressDock, m_scopesDock})
            if (d) moveDockToTechnical(d, true);
        QRect techGeom = techScreen ? techSaved.intersected(techScreen->availableGeometry())
                                    : left->availableGeometry();
        if (techGeom.width() < 360 || techGeom.height() < 240)
            techGeom = (techScreen ? techScreen : left)->availableGeometry();
        m_techWindow->setGeometry(techGeom);
        m_techWindow->show();
        m_techWindow->raise();

        // Monitor de referência: na tela salva (tela cheia ou flutuando como
        // o usuário deixou), ou à direita em tela cheia no primeiro setup.
        if (m_monitor && !m_monitor->isVisible() && m_monitorAction
            && !m_monitorAction->isChecked())
            m_monitorAction->setChecked(true);
        if (m_monitor) {
            if (monScreen && monFullScreen) {
                m_monitor->placeFullScreenOn(monScreen);
            } else if (monScreen) {
                QRect g = monSaved.intersected(monScreen->availableGeometry());
                if (g.width() < 200 || g.height() < 120) g = monScreen->availableGeometry();
                m_monitor->showNormal(); // sai de tela cheia se estava
                m_monitor->setGeometry(g);
                m_monitor->show();
            } else {
                m_monitor->placeFullScreenOn(right);
            }
        }
    } else {
        // 1–2 telas: sem janela técnica; referência na tela secundária.
        if (m_techWindow) {
            m_techWindow->returnAllDocks();
            m_techWindow->hide();
        }
        if (m_monitorAction && !m_monitorAction->isChecked())
            m_monitorAction->setChecked(true);
        placeMonitorOnSecondaryScreen();
    }
}

void MainWindow::ensureTechnicalWindow() {
    if (m_techWindow) return;
    m_techWindow = new TechnicalWindow; // janela de nível superior
    // Candidatos: tudo que é "parte técnica" da bancada. O menu Painéis da
    // janela lista todos; o default (movido no arrange) é Mixer/Efeitos/
    // Express/Analisadores.
    QVector<QPair<QDockWidget*, QString>> cands = {
        {m_mixerDock, tr("Mixer")},
        {m_equalizerDock, tr("Equalizador")},
        {m_effectsDock, tr("Efeitos")},
        {m_expressDock, tr("Express")},
        {m_scopesDock, tr("Analisadores")},
        {m_propsDock, tr("Propriedades")},
        {m_sourceDock, tr("Source")},
        {m_colorDock, tr("Cor")},
        {m_textDock, tr("Texto")},
        {m_lazziDock, tr("Lazzi")},
        {m_animPresetDock, tr("Animação")},
        {m_histDock, tr("Histórico")},
    };
    m_techWindow->setCandidates(cands, [this](QDockWidget* d, bool in) {
        moveDockToTechnical(d, in);
    });
    // Mantém o check de "Exibir → Painel técnico" em dia quando a janela
    // mostra/esconde por outros caminhos (fechar no X, preset, arrange).
    connect(m_techWindow, &TechnicalWindow::visibleChanged, this, [this](bool v) {
        if (m_techPanelAction) {
            const QSignalBlocker blocker(m_techPanelAction);
            m_techPanelAction->setChecked(v);
        }
    });
}

// Move um dock entre a janela principal e a técnica. Os docks continuam os
// mesmos objetos (sinais/slots intactos) — só trocam de QMainWindow.
void MainWindow::moveDockToTechnical(QDockWidget* dock, bool toTechnical) {
    if (!dock) return;
    ensureTechnicalWindow();
    const bool alreadyIn = m_techWindow->hasDock(dock)
                           || dock->parentWidget() == m_techWindow;
    if (toTechnical) {
        if (alreadyIn) return;
        removeDockWidget(dock);
        m_techWindow->addDockWidget(Qt::LeftDockWidgetArea, dock);
        dock->show();
    } else {
        if (!alreadyIn && dock->parentWidget() != m_techWindow) {
            // Nunca foi para lá (ou já voltou): só garante na janela principal.
            return;
        }
        m_techWindow->removeDockWidget(dock);
        addDockWidget(dockHomeArea(dock), dock);
        dock->show();
        // Restaura as abas canônicas quando o parceiro já voltou.
        auto back = [this](QDockWidget* d) {
            return d && d->parentWidget() != m_techWindow;
        };
        if (dock == m_expressDock && back(m_effectsDock))
            tabifyDockWidget(m_effectsDock, m_expressDock);
        if (dock == m_equalizerDock && back(m_mixerDock))
            tabifyDockWidget(m_mixerDock, m_equalizerDock);
        if (dock == m_scopesDock && back(m_mesaDock))
            tabifyDockWidget(m_mesaDock, m_scopesDock);
    }
}

Qt::DockWidgetArea MainWindow::dockHomeArea(QDockWidget* dock) const {
    if (dock == m_mixerDock || dock == m_equalizerDock)
        return Qt::BottomDockWidgetArea;
    return Qt::RightDockWidgetArea;
}

void MainWindow::seedWorkspacePresets() {
    QSettings settings;
    QStringList names = settings.value("workspaces/names").toStringList();
    bool changed = false;
    for (const QString& p : {QStringLiteral("Edição"), QStringLiteral("Áudio"),
                             QStringLiteral("Composição"), QStringLiteral("Efeitos"),
                             QStringLiteral("Bancada")}) {
        if (!names.contains(p)) {
            names.append(p);
            changed = true;
        }
    }
    if (changed) settings.setValue("workspaces/names", names);
}

void MainWindow::applyWorkspace(const QString& name) {
    if (name == m_currentWorkspace) return;
    // Presets fixos: rearranjo por show/hide (independente de kLayoutVersion).
    static const QSet<QString> presets = {
        QStringLiteral("Edição"), QStringLiteral("Áudio"),
        QStringLiteral("Composição"), QStringLiteral("Efeitos"),
        QStringLiteral("Bancada")};
    if (presets.contains(name)) {
        captureCurrentWorkspace();
        m_restoringSettings = true;
        applyWorkspacePreset(name);
        m_currentWorkspace = name;
        m_restoringSettings = false;
        m_mesa->autoSelectMesa();
        statusBar()->showMessage(tr("Workspace: %1").arg(name), 2500);
        scheduleLayoutSave();
        rebuildWorkspaceMenu();
        return;
    }
    // Grava o layout de onde se está saindo antes de trocar, senão as
    // alterações feitas no workspace anterior se perdem.
    captureCurrentWorkspace();

    m_restoringSettings = true;   // evita que o restoreState dispare autosave
    bool applied = false;
    QSettings settings;
    const QByteArray state = settings.value(QStringLiteral("workspaces/%1/state").arg(name)).toByteArray();
    const int version = settings.value(QStringLiteral("workspaces/%1/version").arg(name), -1).toInt();
    if (!state.isEmpty() && saneLayoutArray(state) && version == kLayoutVersion) {
        restoreState(state);
        applied = true;
    }
    if (!applied) {
        // Workspace ainda sem estado gravado: volta ao arranjo padrão,
        // capturado em createDocks(). Nada de recriar os docks aqui.
        if (!m_defaultLayoutState.isEmpty() && saneLayoutArray(m_defaultLayoutState))
            restoreState(m_defaultLayoutState);
    }
    m_currentWorkspace = name;
    m_restoringSettings = false;

    // O Mesa precisa reencontrar a track depois de qualquer troca de estado.
    m_mesa->autoSelectMesa();
    statusBar()->showMessage(tr("Workspace: %1").arg(name), 2500);
    scheduleLayoutSave();
}

void MainWindow::saveWorkspaceAs(const QString& name) {
    const QString clean = name.trimmed();
    if (clean.isEmpty()) return;
    captureCurrentWorkspace();
    QSettings settings;
    QStringList names = workspaceNames();
    if (!names.contains(clean)) {
        names.append(clean);
        settings.setValue("workspaces/names", names);
    }
    m_currentWorkspace = clean;
    settings.setValue(QStringLiteral("workspaces/%1/state").arg(clean), saveState());
    settings.setValue(QStringLiteral("workspaces/%1/version").arg(clean), kLayoutVersion);
    rebuildWorkspaceMenu();
    statusBar()->showMessage(tr("Workspace salvo como \"%1\".").arg(clean), 2500);
    scheduleLayoutSave();
}

void MainWindow::rebuildWorkspaceMenu() {
    if (!m_workspaceMenu) return;
    m_workspaceMenu->clear();

    m_workspaceGroup = new QActionGroup(this);
    m_workspaceGroup->setExclusive(true);
    for (const QString& name : workspaceNames()) {
        QAction* act = m_workspaceMenu->addAction(name);
        act->setCheckable(true);
        act->setChecked(name == m_currentWorkspace);
        connect(act, &QAction::triggered, this, [this, name]() { applyWorkspace(name); });
        m_workspaceGroup->addAction(act);
    }

    // Presets fixos (se ainda não listados como workspaces salvos).
    m_workspaceMenu->addSeparator();
    QMenu* presetMenu = m_workspaceMenu->addMenu(tr("Presets"));
    const QStringList presetNames = {
        QStringLiteral("Edição"), QStringLiteral("Áudio"),
        QStringLiteral("Composição"), QStringLiteral("Efeitos"),
        QStringLiteral("Bancada")};
    for (const QString& p : presetNames) {
        QAction* act = presetMenu->addAction(p);
        connect(act, &QAction::triggered, this, [this, p]() { applyWorkspace(p); });
    }

    m_workspaceMenu->addSeparator();
    QAction* saveAs = m_workspaceMenu->addAction(tr("Salvar workspace como..."));
    connect(saveAs, &QAction::triggered, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Salvar workspace"),
                                                   tr("Nome do workspace:"),
                                                   QLineEdit::Normal, m_currentWorkspace,
                                                   &ok);
        if (ok) saveWorkspaceAs(name);
    });
    QAction* saveNow = m_workspaceMenu->addAction(tr("Salvar workspace atual"));
    connect(saveNow, &QAction::triggered, this, [this]() { captureCurrentWorkspace(); });
}

void MainWindow::saveSettings() {
    QSettings settings;
    // Só grava o que realmente mudou: os gatilhos de layout são numerosos
    // (mover/redimensionar docks, redimensionar/mover a janela) e reescrever o
    // QSettings a cada micro-evento seria desperdício e risco de gravar um
    // estado transitório por cima de um bom.
    const QByteArray geom = saveGeometry();
    if (!m_layoutStateCached || geom != m_lastSavedGeometry) {
        settings.setValue("geometry", geom);
        m_lastSavedGeometry = geom;
    }
    const QByteArray state = saveState();
    if (!m_layoutStateCached || state != m_lastSavedLayout) {
        settings.setValue("layout", state);
        settings.setValue("layoutVersion", kLayoutVersion);
        m_lastSavedLayout = state;
    }
    settings.setValue("layoutLocked", m_lockAction->isChecked());
    // O workspace corrente também fica salvo no próprio slot nomeado, para
    // que trocar de workspace e voltar devolva o arranjo exato.
    if (!m_currentWorkspace.isEmpty()) {
        settings.setValue(QStringLiteral("workspaces/%1/state").arg(m_currentWorkspace), state);
        settings.setValue(QStringLiteral("workspaces/%1/version").arg(m_currentWorkspace),
                          kLayoutVersion);
    }
    settings.setValue("currentWorkspace", m_currentWorkspace);
    // Bancada: geometria das janelas auxiliares (janela técnica + preview
    // externo) — a coordenada global encodeia a TELA, então reabrir na mesma
    // tela é restaurar a geometria. Salva mesmo com as janelas ocultas
    // (fechar/preset não apaga o objeto).
    if (m_techWindow)
        settings.setValue(QStringLiteral("bancada/techGeometry"),
                          m_techWindow->geometry());
    if (m_monitor) {
        settings.setValue(QStringLiteral("bancada/monitorGeometry"),
                          m_monitor->geometry());
        settings.setValue(QStringLiteral("bancada/monitorFullScreen"),
                          m_monitor->isFullScreen());
    }
    m_layoutStateCached = true;
}

void MainWindow::scheduleLayoutSave() {
    // Antes de restoreSettings() o layout salvo ainda não foi lido; gravar
    // aqui apagaria o arranjo do usuário com o padrão da janela recém-montada.
    if (!m_settingsReady || m_restoringSettings) return;
    if (m_layoutSaveTimer) m_layoutSaveTimer->start();
}

void MainWindow::restoreBackupLayout() {
    QSettings settings;
    const QByteArray backup = settings.value("layoutBackup").toByteArray();
    if (backup.isEmpty() || !saneLayoutArray(backup)) {
        statusBar()->showMessage(tr("Nenhum layout anterior guardado."), 3000);
        return;
    }
    const int backupVersion = settings.value("layoutBackupVersion", -1).toInt();
    QMessageBox box(this);
    box.setWindowTitle(tr("Restaurar layout anterior"));
    box.setIcon(QMessageBox::Question);
    box.setText(tr("Reaplicar o arranjo de painéis guardado (versão de layout %1) "
                   "e substituir o atual?").arg(backupVersion));
    box.setStandardButtons(QMessageBox::Apply | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Apply) return;

    m_restoringSettings = true;
    restoreState(backup);
    m_restoringSettings = false;
    // Adota o arranjo restaurado como o atual, já carimbado com a versão
    // vigente — senão ele voltaria a ser descartado no próximo início.
    m_layoutStateCached = false;
    saveSettings();
    m_mesa->autoSelectMesa();
    statusBar()->showMessage(tr("Layout anterior restaurado."), 3000);
}

bool MainWindow::event(QEvent* e) {
    switch (e->type()) {
    // Ao perder o foco (mudar de janela), agenda o salvamento: cobre o caso de
    // o app ser encerrado logo depois sem passar pelo closeEvent().
    case QEvent::WindowDeactivate:
    // Qualquer rearrranjo dos docks termina num pedido de layout — mover para
    // outra área, redimensionar as divisórias, reanexar/flutuar, reabaixar. O
    // Qt não emite sinal para vários destes gestos (a divisória é desenhada
    // pelo QMainWindowLayout, não é um QSplitter), então observamos o pedido de
    // layout. Resize/Move da própria janela entram pelo mesmo caminho para a
    // geometria não se perder num encerramento anormal. Tudo é debounced e o
    // save ignora estados idênticos ao último gravado.
    case QEvent::LayoutRequest:
    case QEvent::Resize:
    case QEvent::Move:
        scheduleLayoutSave();
        break;
    default:
        break;
    }
    return QMainWindow::event(e);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    // Qt inicia o arraste fantasma do dock no MouseMove mesmo SEM botão
    // pressionado: o QDockWidgetPrivate::mouseMoveEvent não checa buttons(),
    // só o estado interno deixado por um clique anterior na barra de título.
    // Resultado: clique único + mover o mouse sem segurar faz o dock se
    // destacar (preview/ghost) e voltar. Consome o move sem botão — o
    // arrasto de verdade tem botão e continua funcionando.
    if (e->type() == QEvent::MouseMove) {
        if (auto* dock = qobject_cast<QDockWidget*>(obj)) {
            if (static_cast<QMouseEvent*>(e)->buttons() == Qt::NoButton)
                return true;
        }
    }
    // Redimensionar uma divisória entre áreas não muda o tamanho da janela —
    // só a geometria dos docks —, então o Resize do próprio dock é o único
    // aviso de que a largura/altura escolhida pelo usuário precisa ser salva.
    if (e->type() == QEvent::Resize && qobject_cast<QDockWidget*>(obj)) {
        scheduleLayoutSave();
    }
    if (e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        QWidget* fw = focusWidget();
        const bool inPantalone = fw && m_pantalone && m_pantalone->isAncestorOf(fw);
        const bool inGraph = fw && m_graph && m_graph->isAncestorOf(fw);
        if (!inPantalone && !inGraph) {
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

// Mime interno do Qt para arrasto de QDockWidget (o payload é o objectName).
static const char kDockDragMime[] = "application/x-qdockwidget";

// Resolve um dock pelo objectName entre todos os docks da janela principal.
static QDockWidget* dockByNameIn(const QVector<QDockWidget*>& docks,
                                 const QByteArray& objectName) {
    const QString n = QString::fromUtf8(objectName);
    for (QDockWidget* d : docks)
        if (d && d->objectName() == n) return d;
    return nullptr;
}

// Drag&drop entre a janela técnica e a principal: aceita o mime interno de
// docks quando o dock mora na OUTRA janela (drags internos continuam sendo
// do mecanismo próprio do Qt). Sem isso, "docar" um painel na janela técnica
// só funcionaria pelo menu Painéis.
void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (!e->mimeData()->hasFormat(kDockDragMime)) return;
    QDockWidget* d = dockByNameIn(m_allDocks, e->mimeData()->data(kDockDragMime));
    if (d && m_techWindow && m_techWindow->hasDock(d)) e->acceptProposedAction();
}

void MainWindow::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData()->hasFormat(kDockDragMime)) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    if (!e->mimeData()->hasFormat(kDockDragMime)) return;
    QDockWidget* d = dockByNameIn(m_allDocks, e->mimeData()->data(kDockDragMime));
    if (!d || !m_techWindow || !m_techWindow->hasDock(d)) return;
    moveDockToTechnical(d, false);
    e->acceptProposedAction();
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
    // O nome do workspace corrente. Vazio = instalação nova / nunca salvo; usa
    // "Edição" para que o layout atual vire o workspace padrão.
    m_currentWorkspace = settings.value("currentWorkspace").toString();
    if (m_currentWorkspace.isEmpty()) m_currentWorkspace = tr("Edição");

    const int savedLayoutVersion = settings.value("layoutVersion").toInt();
    const QByteArray state = settings.value("layout").toByteArray();
    const bool hasState = !state.isEmpty() && saneLayoutArray(state);
    if (savedLayoutVersion == kLayoutVersion && hasState) {
        restoreState(state);
        // O arranjo salvo traz as larguras que o usuário escolheu; as
        // padrão não devem sobrescrevê-lo.
        m_hasRestoredLayout = true;
    } else if (hasState) {
        // O layout salvo é de uma versão anterior (kLayoutVersion mudou):
        // reaplicá-lo direto pode deslocar painéis, mas descartá-lo em silêncio
        // perde o arranjo do usuário. Guarda um backup que pode ser reaplicado
        // por Exibir → "Restaurar layout anterior".
        settings.setValue("layoutBackup", state);
        settings.setValue("layoutBackupVersion", savedLayoutVersion);
    }
    if (settings.contains("layoutLocked"))
        m_lockAction->setChecked(settings.value("layoutLocked").toBool());
    setDockLocked(m_lockAction->isChecked());
    m_restoringSettings = false;
    // A partir daqui o layout salvo já foi lido: os gatilhos de layout podem
    // gravar com segurança.
    m_settingsReady = true;

    // Garante que a Mesa encontra suas tracks mesmo quando o dock é restaurado
    // como visível — autoSelectMesa() precisa rodar após restoreState().
    m_mesa->autoSelectMesa();

    // Reaplica o preset "Bancada" se ele era o workspace corrente: o
    // saveState() não cobre a janela técnica nem o preview externo, então o
    // arranjo completo (monitores, painéis técnicos, telas) só volta com a
    // reaplicação do preset.
    if (m_currentWorkspace == QStringLiteral("Bancada")) {
        applyWorkspacePreset(QStringLiteral("Bancada"));
        rebuildWorkspaceMenu();
    }
}

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

    // Cor — correção de cor estilo DaVinci (trackballs Lift/Gamma/Gain).
    m_colorWidget = new ColorDock(this);
    m_colorWidget->setTimeline(m_timeline);
    m_colorWidget->setPreview(m_preview);
    m_colorDock = makeDock(QStringLiteral("colorDock"), tr("Cor"),
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
               "velocidade, pivot, analisador e workspace.<br><br>"
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

void MainWindow::pushUndo() {
    // Descarta o "redo" se o usuário desfez e fez uma nova edição.
    const bool hadRedo = (m_undoIndex < (int)m_undoStack.size() - 1);
    if (hadRedo) {
        while ((int)m_undoStack.size() > m_undoIndex + 1) {
            m_undoBytes -= m_undoStack.back().size();
            m_undoStack.pop_back();
            m_undoLabels.pop_back();
        }
    }
    const QByteArray snap = snapshotState();
    // Dedup só é seguro no TOPO da pilha. Depois de um undo/redo o índice não
    // aponta mais para o último estado registrado: o pré-estado da nova edição
    // coincide com o topo e o `back() == snap` engolia o push — o Ctrl+Z
    // seguinte apagava 2 ações de uma vez (ou nada em cima do "Início").
    const bool swallowed = !hadRedo && !m_undoStack.empty()
                           && m_undoStack.back() == snap;
    if (!swallowed) {
        m_undoStack.push_back(snap);
        m_undoLabels.push_back(m_pendingUndoLabel.isEmpty() ? tr("Edição")
                                                            : m_pendingUndoLabel);
        m_undoBytes += snap.size();
        m_undoIndex = (int)m_undoStack.size() - 1;
    }
    m_pendingUndoLabel.clear();
    // Eviction: por contagem e por memória (sempre na frente — O(1) com deque).
    while ((int)m_undoStack.size() > kUndoMaxEntries
           || (m_undoBytes > kUndoMaxBytes && (int)m_undoStack.size() > 1)) {
        m_undoBytes -= m_undoStack.front().size();
        m_undoStack.pop_front();
        m_undoLabels.pop_front();
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
    // Snapshot é sempre do app atual; a recusa (false) só aconteceria num
    // cenário impossível — nesse caso o modelo fica vazio (default) em vez
    // de carregar meio-estado.
    m_project.fromJson(doc.isObject() ? doc.object() : QJsonObject());
}

void MainWindow::undo() {
    if (m_undoIndex <= 0) return;
    const QString sel = m_timeline->lastSelectedId();
    QPointer<QWidget> fw = focusWidget();
    --m_undoIndex;
    applyUndoState();
    // Preserva a seleção e o foco (o setProject da timeline limpa a seleção e
    // o editor de curvas/pantalone perderiam o clipe após o Ctrl+Z).
    if (!sel.isEmpty()) m_timeline->selectClip(sel);
    if (fw) fw->setFocus();
    setModified();
}

void MainWindow::redo() {
    if (m_undoIndex >= (int)m_undoStack.size() - 1) return;
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
    m_pantalone->setProject(&m_project);
    m_graph->refresh();
    m_props->refresh();
    m_preview->refreshView();
    m_mixer->refresh();
    m_equalizer->refresh();
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
    m_redoAction->setEnabled(m_undoIndex < (int)m_undoStack.size() - 1);
}

// Painel de histórico (estilo Vegas): cada entrada = estado da timeline ANTES
// de uma edição. Clicar salta o índice de undo para aquele ponto (undo/redo
// em bloco, sem precisar dar N Ctrl+Z).
void MainWindow::updateHistoryList() {
    if (!m_histList || m_undoLabels.size() != m_undoStack.size()) return;
    m_histList->blockSignals(true);
    m_histList->clear();
    for (int i = 0; i < (int)m_undoLabels.size(); ++i) {
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
    if (index < 0 || index >= (int)m_undoStack.size() || index == m_undoIndex) return;
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
    ++m_projGen; // troca de projeto: invalida callbacks de save em voo
    for (int i = 0; i < 3; ++i) m_project.addTrack(false);
    for (int i = 0; i < 3; ++i) m_project.addTrack(true);
    m_pantalone->setProject(&m_project);
    m_pantaloneDock->hide();
    m_undoStack.clear();
    m_undoStack.push_back(snapshotState());
    m_undoLabels.clear();
    m_undoLabels.push_back(tr("Início"));
    m_undoBytes = m_undoStack.front().size();
    m_undoIndex = 0;
    updateHistoryList();
    m_currentFile.clear();
    m_modified = false;
    ProxyManager::instance().setProjectUsesProxies(m_project.useProxies);
    applyUndoState();
    // Troca de projeto: MainWindow reusa o mesmo &m_project, então o
    // PreviewWidget::setProject (chamado uma única vez na construção, linha 221)
    // nunca roda aqui — o transporte, a agulha e os caches ficavam do projeto
    // anterior (agulha podia parar além da duração nova e o play seguia em cima
    // do conteúdo trocado). Para o play e recoloca todo mundo no zero.
    m_preview->setProject(&m_project);
    m_timeline->setPlayhead(0.0); // setPlayhead não emite playheadChanged
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

// Recarrega o projeto atual do disco (F5). Útil durante o desenvolvimento:
// edita o .Blanc por fora (ou o app grava algo) e atualiza sem reabrir.
// Se não houver arquivo salvo, cai no diálogo de Abrir.
void MainWindow::reloadProject() {
    if (m_currentFile.isEmpty()) { openProject(); return; }
    if (!confirmDiscardChanges()) return;
    openProjectFile(m_currentFile);
}

// Indicador combinado de trabalho em background: a barra da statusBar fica
// visível enquanto houver thumbs/picos (MediaCache) OU proxy (ProxyManager)
// em curso, com a mensagem do passo mais longo (proxy primeiro).
void MainWindow::refreshBackgroundBusy() {
    if (!m_busyBar) return;
    const bool busy = m_bgCacheBusy || m_bgProxyBusy;
    m_busyBar->setVisible(busy);
    if (m_bgProxyBusy) {
        statusBar()->showMessage(m_proxyPending > 0
            ? tr("Gerando proxy: %1 restante(s)…").arg(m_proxyPending)
            : tr("Gerando proxy…"));
    } else if (m_bgCacheBusy) {
        statusBar()->showMessage(tr("Carregando áudio e miniaturas…"));
    } else {
        const QString msg = statusBar()->currentMessage();
        if (msg.contains(tr("Gerando proxy")) || msg.contains(tr("Carregando áudio")))
            statusBar()->clearMessage();
    }
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
    // ── Portão de versão do .Blanc (grupos de formato) ────────────────────
    // Arquivo de um grupo MAIS NOVO: recusa ANTES de tocar no modelo atual
    // (interpretar formato desconhecido é corrupção silenciosa). Arquivo de
    // um grupo mais ANTIGO (0.7 ou anterior ao versionamento): avisa que a
    // conversão é one-way e, em caso positivo, migra e já regrava o arquivo
    // no formato novo (o backup rotativo protege o original).
    const QJsonObject obj = doc.object();
    const int fileVer = Project::schemaOf(obj);
    const QString fileGroup = Project::formatGroupOf(obj);
    if (fileVer > Project::kSchemaVersion) {
        QMessageBox::critical(
            this, tr("Projeto de versão mais nova"),
            tr("Este projeto foi salvo por uma versão mais nova do Pierrot\n"
               "(schema %1%2 — este app é o schema %3, grupo " PIERROT_VERSION_GROUP ").\n\n"
               "Atualize o Pierrot para abrir este arquivo.")
                .arg(fileVer)
                .arg(fileGroup.isEmpty() ? QString()
                                         : tr(", grupo %1").arg(fileGroup))
                .arg(Project::kSchemaVersion));
        return;
    }
    bool convertOld = false;
    if (fileVer < Project::kSchemaVersion) {
        const auto btn = QMessageBox::warning(
            this, tr("Projeto de versão antiga"),
            tr("Seu projeto usa uma versão antiga do Laartman — foi salvo por "
               "uma versão anterior do Pierrot%1.\n\n"
               "Ao CONVERTER para o formato atual (grupo " PIERROT_VERSION_GROUP ", "
               "Laartman v0.4), o projeto não poderá mais ser aberto no "
               "Pierrot 0.7 ou anteriores.\n\n"
               "Deseja converter e salvar agora?")
                .arg(fileGroup.isEmpty()
                         ? tr(" (formato anterior ao versionamento por grupo)")
                         : tr(" (grupo %1)").arg(fileGroup)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (btn != QMessageBox::Yes) return; // mantém o projeto atual intacto
        convertOld = true;
    }
    if (!m_project.fromJson(obj)) {
        // Defensivo: o portão acima já recusou, mas fromJson é a autoridade.
        QMessageBox::warning(this, tr("Abrir projeto"),
                             tr("Arquivo de projeto recusado (versão não suportada):\n%1").arg(path));
        return;
    }
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
    m_currentFile = path;
    m_modified = false;
    ProxyManager::instance().setProjectUsesProxies(m_project.useProxies);
    // Sonda as mídias do projeto para enfileirar proxies. Sem isso, abrir um
    // projeto do disco nunca gerava proxy nenhum: probeAndQueue() só era
    // chamado na importação de mídia nova, então um projeto 4K aberto de um
    // .Blanc reproduzia do original a vida inteira (a menos que outra sessão
    // tivesse gerado antes e deixado o metadata.json).
    for (const MediaItem& mi : m_project.media)
        if (mi.hasVideo && !mi.filePath.isEmpty())
            ProxyManager::instance().probeAndQueue(mi.filePath);
    applyUndoState();
    // Mesmo motivo do newProject: sem setProject, abrir um arquivo mantinha a
    // agulha/transporte/caches do projeto anterior (arquivo menor = agulha além
    // da duração, "Sem clipe" e cache velho no monitor).
    m_preview->setProject(&m_project);
    m_timeline->setPlayhead(0.0); // setPlayhead não emite playheadChanged
    updateTitle();
    addRecentProject(path);
    if (convertOld) {
        // Conversão one-way já confirmada: regrava o arquivo no formato do
        // grupo atual. O save é assíncrono e o backup rotativo
        // (~/Pierrot/backups/) preserva o original antes da sobrescrita.
        writeProjectFile(path);
        statusBar()->showMessage(
            tr("Projeto CONVERTIDO para o grupo " PIERROT_VERSION_GROUP
               " (Laartman v0.4) e salvo — não abre mais no Pierrot 0.7."));
    } else {
        statusBar()->showMessage(tr("Projeto aberto: %1").arg(path));
    }
}

// Espera a fila de proxies esvaziar (teto de 10 min) e então abre a janela
// medida. Fora daqui fica o warm-up: abrir decoders e aquecer caches.
void MainWindow::awaitProxiesThenMeasure(double start, double warmupSec, double seconds) {
    auto* poll = new QTimer(this);
    poll->setInterval(500);
    // O timer é dono da janela e pode ficar vivo bem depois deste retorno, então
    // o estado da espera precisa sobreviver à stack: capturar &waited/&tries
    // deixava a lambda lendo memória de pilha destruída (UB) bem depois do fim
    // de awaitProxiesThenMeasure().
    auto st = std::make_shared<ProxyWaitState>();
    st->waited.start();
    connect(poll, &QTimer::timeout, poll, [this, poll, st, start, warmupSec, seconds]() {
        const bool timeout = st->waited.elapsed() > 600000;
        if (ProxyManager::instance().busy() && !timeout && ++st->tries <= 1200) return;
        if (timeout)
            qWarning("[autoplay] proxies ainda em fila após 10 min — medindo assim mesmo.");
        poll->stop();
        poll->deleteLater();
        startMeasuredRun(start, warmupSec, seconds);
    });
    poll->start();
}

// Abre a janela medida: roda `warmupSec` fora da coleta, zera o profiler,
// reproduz `seconds` e grava o JSON.
void MainWindow::startMeasuredRun(double start, double warmupSec, double seconds) {
    m_preview->playFrom(start);
    QTimer::singleShot(int(warmupSec * 1000.0), this, [this, seconds, start]() {
        PreviewProfiler::instance().beginSession(QStringLiteral("autoplay"),
                                                 m_project.useProxies, m_project.fps);
        m_preview->seek(start);
        m_preview->playFrom(start);
        QTimer::singleShot(int(seconds * 1000.0), this, [this]() {
            PreviewProfiler::instance().endSession();
            const auto& prof = PreviewProfiler::instance();
            const bool ok = prof.dumpJson(m_autoplayJsonOut);
            qInfo("[autoplay] relatório %s: %d quadros, fonte=%s, cortes=%d, dropped=%lld",
                  ok ? "gravado" : "FALHOU", prof.frameCount(),
                  prof.wroteProxy() ? "proxy" : "original", prof.cuts(),
                  static_cast<long long>(prof.droppedTotal()));
            m_preview->setLoopEnabled(false);
            m_preview->togglePlay(); // pausa
            QTimer::singleShot(200, qApp, &QCoreApplication::quit);
        });
    });
}

// ── Harness de reprodução sem interação (pierrot --autoplay) ──────────────
//
// Existe para o A/B proxy-vs-original ser reproduzível: a comparação depende
// de a partida começar sempre do mesmo lugar, com os proxies no mesmo estado de
// calor. Fica em uma função só, com o mínimo de conhecimento de
// ProxyManager/PreviewProfiler, para não espalhar isso pela classe.
void MainWindow::autoplay(double seconds, int useProxies, double fromSec,
                          double warmupSec, bool waitProxies, const QString& jsonOut) {
    m_autoplayJsonOut = jsonOut;
    if (useProxies >= 0) {
        m_project.useProxies = useProxies != 0;
        ProxyManager::instance().setProjectUsesProxies(m_project.useProxies);
    }
    if (!m_preview || m_project.duration() <= 0.0) {
        qWarning("[autoplay] projeto vazio ou sem duração — nada a medir.");
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }

    // Loop sobre o projeto inteiro: a reprodução não pode parar no fim de uma
    // faixa e polluir a comparação com um trecho sem clipe.
    m_preview->setLoopRange(0.0, m_project.duration());
    m_preview->setLoopEnabled(true);

    const double start = std::clamp(fromSec, 0.0, std::max(0.0, m_project.duration() - 0.5));
    if (waitProxies)
        awaitProxiesThenMeasure(start, warmupSec, seconds);
    else
        startMeasuredRun(start, warmupSec, seconds);
}

void MainWindow::createProject(int width, int height, int fps, const QString& name) {
    newProject();
    m_project.width = width;
    m_project.height = height;
    m_project.fps = fps;
    m_project.name = name;
    applyUndoState();

    // Salva automaticamente em Documentos/ sem pedir o nome de novo — a
    // tela de boas-vindas já perguntou o nome do projeto; o segundo
    // diálogo "Salvar projeto" só confundia (usuário tinha que digitar o
    // nome duas vezes). Se já existir, acrescenta um número.
    QString base = name.trimmed();
    if (base.isEmpty()) base = tr("Sem título");
    base.replace(QLatin1Char('/'), QLatin1Char('-'));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QDir().mkpath(dir);
    QString path = dir + QLatin1Char('/') + base + QStringLiteral(".Blanc");
    int n = 2;
    while (QFile::exists(path)) {
        path = dir + QLatin1Char('/') + base
               + QStringLiteral(" (%1).Blanc").arg(n++);
    }
    writeProjectFile(path);
    updateTitle();
}

bool MainWindow::saveProject() {
    if (m_currentFile.isEmpty()) return saveProjectAs();
    writeProjectFile(m_currentFile);
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
    writeProjectFile(path);
    return true;
}

// Save assíncrono: serializa na UI thread (o modelo só é mutado nela — garante
// um snapshot consistente sem data race com a edição), e grava em disco +
// backup rotativo num worker QtConcurrent. A UI não congela em projetos
// grandes. Erros voltam ao usuário via QMessageBox quando o worker termina.
// Detalhes de reentrância (documentados em MainWindow.h):
//   * save durante save em voo → enfileirado e re-disparado ao terminar;
//   * edição durante a gravação → m_modified permanece true (comparação de
//     revision()) para o título não mentir;
//   * no fechamento do app, closeEvent() espera o worker terminar.
void MainWindow::writeProjectFile(const QString& path, bool autoSave) {
    if (m_saveBusy) {
        m_savePending = true;
        m_queuedPath = path;
        m_queuedAuto = autoSave;
        statusBar()->showMessage(tr("Salvamento em andamento — aguarde."));
        return;
    }

    const QByteArray data =
        QJsonDocument(m_project.toJson()).toJson(QJsonDocument::Indented);
    const quint64 rev = m_project.revision;
    const quint64 gen = m_projGen;
    m_saveAuto = autoSave;
    m_saveBusy = true;
    statusBar()->showMessage(tr("Salvando..."));

    auto* watcher = new QFutureWatcher<SaveWriteResult>(this);
    connect(watcher, &QFutureWatcher<SaveWriteResult>::finished, this,
            [this, watcher, path, rev, gen]() {
        watcher->deleteLater();
        m_saveBusy = false;
        const bool queued = m_savePending;
        const QString queuedPath = m_queuedPath;
        const bool queuedAuto = m_queuedAuto;
        m_savePending = false;
        // O projeto pode ter sido trocado enquanto o save estava em voo
        // (confirmDiscardChanges → novo/abrir). Nesse caso o arquivo gravado
        // é do projeto antigo: não mexer no m_currentFile/título/dirty-state.
        const bool sameProj = (m_projGen == gen);

        const SaveWriteResult res = watcher->result();
        if (!res.ok) {
            if (sameProj) {
                m_modified = true;
                updateTitle();
            }
            QMessageBox::warning(this, tr("Salvar projeto"),
                                 tr("Não foi possível gravar o arquivo:\n%1\n\n%2")
                                     .arg(path, res.detail));
        } else if (sameProj) {
            m_currentFile = path;
            addRecentProject(path);
            // Se o usuário editou durante a gravação, o arquivo gravado não
            // reflete o estado atual → mantém o projeto marcado como sujo.
            if (m_project.revision == rev)
                m_modified = false;
            updateTitle();
            statusBar()->showMessage(
                m_saveAuto
                    ? tr("Projeto salvo automaticamente (%1).")
                          .arg(QTime::currentTime().toString(QStringLiteral("HH:mm")))
                    : tr("Projeto salvo: %1").arg(path));
        } else {
            statusBar()->showMessage(tr("Projeto fechado salvo: %1").arg(path));
        }

        if (queued && sameProj)
            writeProjectFile(queuedPath, queuedAuto);
    });

    watcher->setFuture(QtConcurrent::run([path, data]() -> SaveWriteResult {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return { false, file.errorString() };
        file.write(data);
        if (!file.commit())
            return { false, file.errorString() };
        rotatingBackup(path);
        return { true, QString() };
    }));
}

void MainWindow::autoSave() {
    if (m_currentFile.isEmpty()) {
        statusBar()->showMessage(
            tr("Salvamento automático: salve o projeto uma vez (Ctrl+S) para ativar."));
        return;
    }
    writeProjectFile(m_currentFile, true);
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
