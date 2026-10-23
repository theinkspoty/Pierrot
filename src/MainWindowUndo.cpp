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


