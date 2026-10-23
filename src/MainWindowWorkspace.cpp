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
        {m_colorDock, tr("Technicolor")},
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


