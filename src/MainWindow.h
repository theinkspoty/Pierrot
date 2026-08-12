#pragma once

#include <QMainWindow>
#include <QVector>
#include <QHash>
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
class QToolBar;
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
    void updateTitle();
    void updateUndoActions();
    void addRecentProject(const QString& path);
    bool writeProjectFile(const QString& path);

    Project m_project;
    QVector<Project> m_undoStack;
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
    QToolBar* m_mainToolBar = nullptr;
    QTimer* m_autoSaveTimer = nullptr;
    QProgressBar* m_busyBar = nullptr;
};
