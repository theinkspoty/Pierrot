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
