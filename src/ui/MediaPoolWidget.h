// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QListWidget>
#include <QTreeWidget>
#include <QImage>
#include <QHash>
#include <QPoint>
#include "colombina/models/Project.h"

class QPushButton;
class QProgressBar;
class QMouseEvent;
class QEvent;
class QRubberBand;
class QLabel;
class QPixmap;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QTreeWidget;
class QLineEdit;
class QButtonGroup;
class QStackedWidget;

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

// Visualização em LISTA do painel de mídias (padrão do projeto, como o painel
// Project do Premiere): tabela com colunas Nome/Frame Rate/Media Start/Media
// End/Duração/Info de vídeo/Info de áudio. Mesmo arrasto manual da PoolList,
// sem o rubber band (a seleção por linhas fica com a base).
class PoolTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit PoolTree(QWidget* parent = nullptr);
signals:
    void dragHover(const QPoint& globalPos);
    void dragHoverCleared();
    void mediaDropped(const QStringList& mediaIds, const QPoint& globalPos);
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
    void showDragIcon(const QPoint& globalPos);
    void moveDragIcon(const QPoint& globalPos);
    void hideDragIcon();
    QPoint m_pressPos;
    QTreeWidgetItem* m_pressItem = nullptr;
    bool m_dragging = false;
    QLabel* m_dragIcon = nullptr;
    friend class MediaPoolWidget;
};

class MediaPoolWidget : public QWidget {
    Q_OBJECT
public:
    explicit MediaPoolWidget(QWidget* parent = nullptr);
    void setProject(Project* p);
public slots:
    void addFiles();
    void importPaths(const QStringList& files);
    void addGenerator();
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
    // Duplo clique: abre no Source Monitor (fluxo Premiere).
    void mediaToSource(const QString& mediaId);
    // Arrastar/soltar .obj na pool: importa como malha da Mesa (não como mídia).
    void objImportRequested(const QStringList& objPaths);
    // Arrastar/soltar .blend: converte via Blender CLI → malha 3D.
    void blendImportRequested(const QStringList& blendPaths);
    // Arrasto manual da pool para a timeline.
    void dragHover(const QPoint& globalPos);
    void dragHoverCleared();
    void mediaDropped(const QStringList& mediaIds, const QPoint& globalPos);
protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
private slots:
    void showPoolContextMenu(const QPoint& globalPos);
private:
    void refresh();
    void setThumb(const QString& mediaId, const QImage& img);
    void updateStatusLabel();
    QStringList currentSelection() const;
    void restoreSelection(const QStringList& ids);
    Project* m_project = nullptr;
    PoolList* m_list = nullptr;    // visualização em ícones (grade)
    PoolTree* m_tree = nullptr;    // visualização em lista (padrão Premiere)
    QStackedWidget* m_stack = nullptr;
    QLineEdit* m_search = nullptr; // busca (filtra as duas visualizações)
    QButtonGroup* m_viewGroup = nullptr;
    QLabel* m_statusLabel = nullptr;
    QString m_filter;
    QPushButton* m_removeBtn = nullptr;
    QProgressBar* m_importBar = nullptr;
    QHash<QString, QImage> m_thumbs; // mediaId -> thumb (independente do item)
    QImage m_audioIcon;
    QImage m_videoPlaceholder;
};