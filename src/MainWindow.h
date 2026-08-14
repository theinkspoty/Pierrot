// Pierrot — editor de vídeo estilo Vegas Pro
//
// Copyright (C) 2026 Pierrot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <QMainWindow>
#include <QVector>
#include <QHash>
#include <QByteArray>
#include <QJsonDocument>
#include <QIcon>
#include <QDockWidget>
#include <functional>
#include "models/Project.h"

class MediaPoolWidget;
class TimelineWidget;
class PreviewWidget;
class PancropWidget;
class GraphEditorWidget;
class QAction;
class QColor;
class QPainter;
class QTimer;
class QProgressBar;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void openProjectFile(const QString& path);
    void createProject(int width, int height, int fps, const QString& name);
protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
private slots:
    void pushUndo();
    void setModified();
    void autoSave();
private:
    void createDocks();
    void createActions();
    void saveSettings();
    void restoreSettings();
    void setDockLocked(bool locked);
    QIcon makeIcon(const std::function<void(QPainter&, const QColor&)>& draw) const;
    QIcon padlockIcon(bool locked) const;
    QIcon iconCursor() const;
    QIcon iconMove() const;
    QIcon iconScissors() const;
    QIcon iconRazor() const;
    QIcon iconEnvelope() const;
    QIcon iconZoom() const;
    QIcon iconMagnet() const;
    QIcon iconImport() const;
    QIcon iconExport() const;
    void exportVideo();
    void newProject();
    void openProject();
    bool saveProject();
    bool saveProjectAs();
    void projectSettings();
    void openSettings();
    void undo();
    void redo();
    void applyUndoState();
    QByteArray snapshotState() const;
    void restoreSnapshot(const QByteArray& snap);
    void updateTitle();
    void updateUndoActions();
    void addRecentProject(const QString& path);
    bool writeProjectFile(const QString& path);

    Project m_project;
    // Snapshots de undo em JSON comprimido: guardar 60 cópias em memória do
    // Project inteiro faria a RAM explodir em projetos com muitos cortes.
    QVector<QByteArray> m_undoStack;
    int m_undoIndex = 0;
    QString m_currentFile;
    bool m_modified = false;

    MediaPoolWidget* m_pool = nullptr;
    TimelineWidget* m_timeline = nullptr;
    PreviewWidget* m_preview = nullptr;
    PancropWidget* m_pancrop = nullptr;
    GraphEditorWidget* m_graph = nullptr;
    QDockWidget* m_poolDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    QDockWidget* m_pancropDock = nullptr;
    QDockWidget* m_graphDock = nullptr;
    QHash<QDockWidget*, QDockWidget::DockWidgetFeatures> m_originalFeatures;
    QAction* m_lockAction = nullptr;
    QAction* m_playAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_saveAction = nullptr;
    QAction* m_saveAsAction = nullptr;
    QAction* m_snapAction = nullptr;
    QVector<QAction*> m_toolActions;
    QTimer* m_autoSaveTimer = nullptr;
    QProgressBar* m_busyBar = nullptr;
};
