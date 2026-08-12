#pragma once

#include <QWidget>
#include <QListWidget>
#include <QMimeData>
#include "models/Project.h"

class QPushButton;

class PoolList : public QListWidget {
    Q_OBJECT
public:
    explicit PoolList(QWidget* parent = nullptr);
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
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
signals:
    void mediaAdded(const QString& mediaId);
    void mediaChanged();
    void editStart();
    void importStarted();
    void importProgress(int processed);
    void importFinished(int added, int invalid);
private:
    void refresh();
    Project* m_project = nullptr;
    QListWidget* m_list = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_removeBtn = nullptr;
};
