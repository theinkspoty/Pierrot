// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>

class QFileSystemModel;
class QSortFilterProxyModel;
class QTreeView;
class QLineEdit;
class QPushButton;
class QListWidget;
class QListView;

// Explorador de mídia simples (estilo Premiere): à esquerda os "Lugares"
// (Início + SSDs externos), à direita o conteúdo da pasta atual. Duplo clique
// em pasta entra; em arquivo importa. Arraste mídias direto para o Media Pool.
// Botão "Miniaturas" alterna entre lista e grade com thumbnails dos arquivos.
class FileBrowserWidget : public QWidget {
    Q_OBJECT
public:
    explicit FileBrowserWidget(QWidget* parent = nullptr);
    void goTo(const QString& path);
signals:
    void filesImportRequested(const QStringList& files);
private:
    void openIndex(const QModelIndex& idx);
    void importCurrentDir();
    void populatePlaces();
    void setCurrentPath(const QString& path);
    void refreshList();
    void setViewMode(bool thumbs);
    void populateThumbs();
    static bool isMediaFile(const QString& path);
    void openPath(const QString& path);

    QFileSystemModel* m_model = nullptr;
    QSortFilterProxyModel* m_proxy = nullptr;
    QTreeView* m_fileList = nullptr;  // modo lista (colunas)
    QListWidget* m_thumbList = nullptr; // modo miniaturas (grade)
    QListWidget* m_places = nullptr;  // atalhos: Home + SSDs externos
    QLineEdit* m_path = nullptr;
    QPushButton* m_upBtn = nullptr;
    QPushButton* m_viewBtn = nullptr;
    QPushButton* m_importDirBtn = nullptr;
    bool m_thumbsMode = false;
    QString m_currentPath;
};
