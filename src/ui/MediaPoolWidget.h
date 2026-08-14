#pragma once

#include <QWidget>
#include <QListWidget>
#include <QImage>
#include <QHash>
#include <QPoint>
#include "models/Project.h"

class QPushButton;
class QMouseEvent;
class QEvent;

// Lista de mídia com arrasto manual (não usa o DnD do compositor, que pode
// falhar em alguns ambientes/Wayland). O arrasto inteiro acontece dentro do
// próprio aplicativo: um filtro global de eventos acompanha o cursor e emite a
// posição para o feedback na timeline, soltando direto no alvo no release.
class PoolList : public QListWidget {
    Q_OBJECT
public:
    explicit PoolList(QWidget* parent = nullptr);
signals:
    void dragHover(const QPoint& globalPos);
    void dragHoverCleared();
    void mediaDropped(const QStringList& mediaIds, const QPoint& globalPos);
protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
private:
    QStringList selectedIds() const;
    void cancelDrag();
    QPoint m_pressPos;
    QListWidgetItem* m_pressItem = nullptr;
    bool m_dragging = false;
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
    // Arrasto manual da pool para a timeline.
    void dragHover(const QPoint& globalPos);
    void dragHoverCleared();
    void mediaDropped(const QStringList& mediaIds, const QPoint& globalPos);
private:
    void refresh();
    void requestPoolThumb(const QString& path, double seconds, const QString& mediaId);
    void setThumb(const QString& mediaId, const QImage& img);
    Project* m_project = nullptr;
    PoolList* m_list = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_removeBtn = nullptr;
    QHash<QString, QImage> m_thumbs; // mediaId -> thumb (independente do item)
    QImage m_audioIcon;
    QImage m_videoPlaceholder;
};
