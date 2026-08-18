// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QListWidget>
#include <QImage>
#include <QHash>
#include <QPoint>
#include "models/Project.h"

class QPushButton;
class QProgressBar;
class QLineEdit;
class QMouseEvent;
class QEvent;
class QRubberBand;
class QLabel;
class QPixmap;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;

// Lista de mídia com arrasto manual (não usa o DnD do compositor, que pode
// falhar em alguns ambientes/Wayland). O arrasto inteiro acontece dentro do
// próprio aplicativo: um filtro global de eventos acompanha o cursor e emite a
// posição para o feedback na timeline, soltando direto no alvo no release.
// Clique+arraste num item arrasta a mídia para a timeline (com uma miniatura
// seguindo o cursor); clique+arraste no vazio seleciona em caixa (rubber band).
class PoolList : public QListWidget {
    Q_OBJECT
public:
    explicit PoolList(QWidget* parent = nullptr);
signals:
    void dragHover(const QPoint& globalPos);
    void dragHoverCleared();
    void mediaDropped(const QStringList& mediaIds, const QPoint& globalPos);
    // Arquivos arrastados do sistema para importar no painel.
    void filesDropped(const QStringList& files);
protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
private:
    QStringList selectedIds() const;
    void cancelDrag();
    void updateBand(const QPoint& globalPos);
    void finalizeBand();
    void cancelBand();
    void showDragIcon(const QPoint& globalPos);
    void moveDragIcon(const QPoint& globalPos);
    void hideDragIcon();
    QPixmap makeDragPixmap() const;
    QPoint m_pressPos;
    QListWidgetItem* m_pressItem = nullptr;
    bool m_pressWasSelected = false;
    bool m_pressCtrl = false;
    bool m_dragging = false;
    QRubberBand* m_band = nullptr;
    bool m_bandActive = false;
    bool m_bandAdd = false;
    QLabel* m_dragIcon = nullptr;
};

class MediaPoolWidget : public QWidget {
    Q_OBJECT
public:
    explicit MediaPoolWidget(QWidget* parent = nullptr);
    void setProject(Project* p);
public slots:
    void addFiles();
    void importPaths(const QStringList& files);
    void addSolidColor();
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
protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
private:
    void refresh();
    void setThumb(const QString& mediaId, const QImage& img);
    Project* m_project = nullptr;
    PoolList* m_list = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_removeBtn = nullptr;
    QLineEdit* m_search = nullptr;
    QProgressBar* m_importBar = nullptr;
    QHash<QString, QImage> m_thumbs; // mediaId -> thumb (independente do item)
    QImage m_audioIcon;
    QImage m_videoPlaceholder;
};
