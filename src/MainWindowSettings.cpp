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

namespace {
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


