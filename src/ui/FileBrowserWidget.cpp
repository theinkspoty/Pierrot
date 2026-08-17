// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "FileBrowserWidget.h"

#include <QFileSystemModel>
#include <QTreeView>
#include <QListView>
#include <QLineEdit>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QDir>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QSortFilterProxyModel>
#include <QListWidget>
#include <QStyle>
#include <QMimeData>
#include <QUrl>
#include <QSet>
#include "ffmpeg/MediaCache.h"

// Esconde diretórios do sistema (boot, dev, etc.) da lista.
class SystemDirFilterProxy : public QSortFilterProxyModel {
public:
    explicit SystemDirFilterProxy(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}
protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
        const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        if (!idx.isValid()) return false;
        const auto* fsm = qobject_cast<const QFileSystemModel*>(sourceModel());
        if (fsm && fsm->isDir(idx)) {
            static const QSet<QString> sys = {
                QStringLiteral("boot"), QStringLiteral("dev"), QStringLiteral("proc"),
                QStringLiteral("sys"), QStringLiteral("etc"), QStringLiteral("run"),
                QStringLiteral("sbin"), QStringLiteral("root"), QStringLiteral("lost+found")
            };
            if (sys.contains(idx.data().toString())) return false;
        }
        return true;
    }
};

bool FileBrowserWidget::isMediaFile(const QString& path) {
    static const QStringList exts = {
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
        QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("m4v"),
        QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("m4a"),
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("svg")
    };
    return exts.contains(QFileInfo(path).suffix().toLower());
}

// Grade de miniaturas com arrasto de arquivos (text/uri-list) para o Media Pool.
class MediaThumbList : public QListWidget {
    Q_OBJECT
public:
    explicit MediaThumbList(QWidget* parent = nullptr) : QListWidget(parent) {
        setViewMode(QListView::IconMode);
        setIconSize(QSize(120, 68));
        setGridSize(QSize(140, 92));
        setResizeMode(QListView::Adjust);
        setMovement(QListView::Static);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setUniformItemSizes(true);
        setSpacing(6);
        setTextElideMode(Qt::ElideRight);
        setWordWrap(true);
    }
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        auto* md = new QMimeData;
        QList<QUrl> urls;
        for (QListWidgetItem* it : items) {
            const QString path = it->data(Qt::UserRole).toString();
            if (!path.isEmpty()) urls.append(QUrl::fromLocalFile(path));
        }
        if (!urls.isEmpty()) md->setUrls(urls);
        return md;
    }
};

FileBrowserWidget::FileBrowserWidget(QWidget* parent) : QWidget(parent) {
    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    m_model->setRootPath(QStringLiteral("/"));

    m_proxy = new SystemDirFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setDynamicSortFilter(true);

    // Lugares: Início + SSDs externos.
    m_places = new QListWidget(this);
    m_places->setMaximumWidth(140);
    m_places->setSpacing(2);
    m_places->setStyleSheet(QStringLiteral(
        "QListWidget{background:#1c1e23; border:1px solid #2a2d34;"
        " color:#c9cdd4; outline:0;}"
        "QListWidget::item{padding:4px 6px; border-radius:3px;}"
        "QListWidget::item:selected{background:#243447; color:#9cc4f0;}"));
    populatePlaces();
    connect(m_places, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        const QString path = it->data(Qt::UserRole).toString();
        if (!path.isEmpty()) setCurrentPath(path);
    });

    // Barra: subir, caminho, importar pasta.
    m_path = new QLineEdit(this);
    m_path->setPlaceholderText(tr("Caminho da pasta…"));
    m_path->setClearButtonEnabled(true);
    m_upBtn = new QPushButton(tr("↑"), this);
    m_upBtn->setToolTip(tr("Subir uma pasta"));
    m_viewBtn = new QPushButton(tr("Miniaturas"), this);
    m_viewBtn->setCheckable(true);
    m_viewBtn->setToolTip(tr("Alternar entre lista e miniaturas"));
    m_importDirBtn = new QPushButton(tr("Importar pasta"), this);
    m_importDirBtn->setToolTip(tr("Importa as mídias desta pasta para o Media Pool"));

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 4, 4, 0);
    bar->setSpacing(4);
    bar->addWidget(m_upBtn);
    bar->addWidget(m_path, 1);
    bar->addWidget(m_viewBtn);
    bar->addWidget(m_importDirBtn);

    // Lista de arquivos da pasta atual (arrastável para o Media Pool).
    m_fileList = new QTreeView(this);
    m_fileList->setModel(m_proxy);
    m_fileList->setRootIsDecorated(true);
    m_fileList->setUniformRowHeights(true);
    m_fileList->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileList->setDragEnabled(true);
    m_fileList->setDragDropMode(QAbstractItemView::DragOnly);
    m_fileList->setDefaultDropAction(Qt::CopyAction);
    m_fileList->header()->setStretchLastSection(true);
    m_fileList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileList->setColumnHidden(1, false); // tamanho
    m_fileList->setColumnHidden(2, false); // tipo
    m_fileList->setColumnHidden(3, false); // data
    m_fileList->setColumnWidth(1, 70);
    m_fileList->setColumnWidth(2, 60);
    m_fileList->setColumnWidth(3, 120);

    // Grade de miniaturas (modo opcional).
    m_thumbList = new MediaThumbList(this);
    m_thumbList->setStyleSheet(QStringLiteral(
        "QListWidget{background:#1c1e23; border:1px solid #2a2d34;"
        " color:#c9cdd4; outline:0;}"));
    m_thumbList->hide();

    auto* body = new QHBoxLayout;
    body->setContentsMargins(4, 2, 4, 4);
    body->setSpacing(4);
    body->addWidget(m_places);
    body->addWidget(m_fileList, 1);
    body->addWidget(m_thumbList, 1);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(2);
    lay->addLayout(bar);
    lay->addLayout(body, 1);

    setStyleSheet(QStringLiteral(
        "QWidget{background:#15161a;}"
        "QTreeView{background:#1c1e23; border:1px solid #2a2d34;"
        " color:#c9cdd4; selection-background-color:#243447;}"
        "QLineEdit{background:#23252b; border:1px solid #2e3138;"
        " color:#c9cdd4; padding:2px 6px; border-radius:3px;}"
        "QPushButton{border:1px solid #2e3138; border-radius:3px;"
        " background:#23252b; color:#c9cdd4; padding:2px 8px;}"
        "QPushButton:hover{background:#2b2e35; border-color:#3c414b;}"));

    connect(m_fileList, &QTreeView::activated, this, [this](const QModelIndex& idx) {
        openIndex(m_proxy->mapToSource(idx));
    });
    connect(m_fileList, &QTreeView::doubleClicked, this, [this](const QModelIndex& idx) {
        openIndex(m_proxy->mapToSource(idx));
    });
    connect(m_upBtn, &QPushButton::clicked, this, [this]() {
        setCurrentPath(QFileInfo(m_currentPath).dir().path());
    });
    connect(m_viewBtn, &QPushButton::toggled, this, [this](bool on) {
        setViewMode(on);
    });
    connect(m_path, &QLineEdit::returnPressed, this, [this]() {
        setCurrentPath(m_path->text().trimmed());
    });
    connect(m_importDirBtn, &QPushButton::clicked, this, &FileBrowserWidget::importCurrentDir);
    connect(m_thumbList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* it) {
        const QString path = it->data(Qt::UserRole).toString();
        if (!path.isEmpty()) openPath(path);
    });
    // Thumbnail pronto -> atualiza o item correspondente na grade.
    connect(&MediaCache::instance(), &MediaCache::thumbnailReady, this,
            [this](const QString& path, double seconds) {
        for (int i = 0; i < m_thumbList->count(); ++i) {
            QListWidgetItem* it = m_thumbList->item(i);
            if (it->data(Qt::UserRole).toString() == path) {
                const QImage img = MediaCache::instance().thumb(path, seconds);
                if (!img.isNull())
                    it->setIcon(QIcon(QPixmap::fromImage(img)));
                break;
            }
        }
    });

    setCurrentPath(QDir::homePath());
}

void FileBrowserWidget::goTo(const QString& path) {
    setCurrentPath(path);
}

void FileBrowserWidget::populatePlaces() {
    if (!m_places) return;
    m_places->clear();
    const QIcon dirIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon driveIcon = style()->standardIcon(QStyle::SP_DriveHDIcon);
    auto addPlace = [&](const QString& label, const QString& path, const QIcon& ic) {
        if (path.isEmpty() || !QDir(path).exists()) return;
        auto* it = new QListWidgetItem(ic, label, m_places);
        it->setData(Qt::UserRole, path);
        it->setToolTip(path);
    };
    addPlace(tr("Início"), QDir::homePath(), dirIcon);
    // SSDs/discos externos montados (Ubuntu: /media/usuário, /run/media/usuário;
    // usuais também em /mnt).
    const QString user = qEnvironmentVariable("USER");
    const QStringList bases = {
        QStringLiteral("/media/%1").arg(user),
        QStringLiteral("/run/media/%1").arg(user),
        QStringLiteral("/mnt"),
    };
    for (const QString& base : bases) {
        const QDir d(base);
        if (!d.exists()) continue;
        for (const QFileInfo& fi : d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                   QDir::Name))
            addPlace(fi.fileName(), fi.absoluteFilePath(), driveIcon);
    }
}

void FileBrowserWidget::setCurrentPath(const QString& path) {
    const QFileInfo fi(path);
    const QString p = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
    if (p.isEmpty() || !QDir(p).exists()) return;
    m_currentPath = p;
    m_path->setText(m_currentPath);
    refreshList();
    if (m_thumbsMode) populateThumbs();
}

void FileBrowserWidget::refreshList() {
    m_fileList->setRootIndex(m_proxy->mapFromSource(m_model->index(m_currentPath)));
}

void FileBrowserWidget::setViewMode(bool thumbs) {
    m_thumbsMode = thumbs;
    m_fileList->setVisible(!thumbs);
    m_thumbList->setVisible(thumbs);
    if (thumbs) populateThumbs();
}

void FileBrowserWidget::populateThumbs() {
    m_thumbList->clear();
    const QDir dir(m_currentPath);
    const QFileIconProvider provider;
    static const QSet<QString> sysDirs = {
        QStringLiteral("boot"), QStringLiteral("dev"), QStringLiteral("proc"),
        QStringLiteral("sys"), QStringLiteral("etc"), QStringLiteral("run"),
        QStringLiteral("sbin"), QStringLiteral("root"), QStringLiteral("lost+found")
    };
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    for (const QFileInfo& fi : entries) {
        if (fi.isDir() && sysDirs.contains(fi.fileName())) continue;
        auto* it = new QListWidgetItem(provider.icon(fi), fi.fileName(), m_thumbList);
        it->setData(Qt::UserRole, fi.absoluteFilePath());
        it->setToolTip(fi.absoluteFilePath());
        it->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        if (fi.isFile() && isMediaFile(fi.absoluteFilePath())) {
            const QImage img = MediaCache::instance().thumb(fi.absoluteFilePath(), 0.0);
            if (!img.isNull())
                it->setIcon(QIcon(QPixmap::fromImage(img)));
            else
                MediaCache::instance().requestThumb(fi.absoluteFilePath(), 0.0);
        }
    }
}

void FileBrowserWidget::openPath(const QString& path) {
    if (QFileInfo(path).isDir()) {
        setCurrentPath(path);
        return;
    }
    emit filesImportRequested(QStringList{path});
}

void FileBrowserWidget::openIndex(const QModelIndex& srcIdx) {
    if (!srcIdx.isValid()) return;
    openPath(m_model->filePath(srcIdx));
}

void FileBrowserWidget::importCurrentDir() {
    if (m_currentPath.isEmpty()) return;
    const QDir dir(m_currentPath);
    QStringList files;
    static const QStringList mediaExts = {
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
        QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("m4v"),
        QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("m4a"),
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("svg")
    };
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Files, QDir::Name)) {
        if (mediaExts.contains(fi.suffix().toLower()))
            files.append(fi.absoluteFilePath());
    }
    if (!files.isEmpty()) emit filesImportRequested(files);
}

#include "FileBrowserWidget.moc"
