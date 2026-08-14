#pragma once

#include <QWidget>
#include <QListWidget>
#include <QMimeData>
#include <QImage>
#include <QHash>
#include "models/Project.h"

class QPushButton;

class PoolList : public QListWidget {
    Q_OBJECT
public:
    explicit PoolList(QWidget* parent = nullptr);
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    void startDrag(Qt::DropActions supportedActions) override;
};

class MediaPoolWidget : public QWidget {
    Q_OBJECT
public:
    explicit MediaPoolWidget(QWidget* parent = nullptr);
    void setProject(Project* p);
public slots:
    void addFiles();
    void removeSelected();
    void refreshFromProject();
    void onThumbReady(const QString& filePath, double seconds);
signals:
    void mediaAdded(const QString& mediaId);
    void mediaChanged();
    void editStart();
    void importStarted();
    void importProgress(int processed);
    void importFinished(int added, int invalid);
    void mediaToTimeline(const QString& mediaId);
private:
    void refresh();
    void requestPoolThumb(const QString& path, double seconds, const QString& mediaId);
    void setThumb(const QString& mediaId, const QImage& img);
    Project* m_project = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_removeBtn = nullptr;
    QHash<QString, QImage> m_thumbs; // mediaId -> thumb (independente do item)
    QImage m_audioIcon;
    QImage m_videoPlaceholder;
};
