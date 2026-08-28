// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "FileBrowserWidget.h"
#include "ui/Theme.h"

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
#include <QMimeData>
#include <QUrl>
#include <QSet>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <functional>
#include "ffmpeg/MediaCache.h"

// Mesma lógica de FileBrowserWidget::isMediaFile, local ao provider.
static bool isMediaFileExt(const QString& path) {
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

// Glifos monocromáticos estilo Premiere (ícones da árvore/places).
static QPixmap makeGlyph(int w, int h, const std::function<void(QPainter&)>& draw) {
    QPixmap pm(w * 2, h * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    draw(p);
    return pm;
}

static QPixmap folderGlyph() {
    return makeGlyph(22, 16, [](QPainter& p) {
        QPen pen(QColor(122, 126, 136), 1.2);
        p.setPen(pen);
        p.setBrush(QColor(166, 170, 179));
        QPainterPath tab;
        tab.moveTo(1.5, 4.5);
        tab.lineTo(7.5, 4.5);
        tab.lineTo(9.5, 6.5);
        tab.lineTo(20.2, 6.5);
        tab.lineTo(20.5, 14.5);
        tab.lineTo(1.5, 14.5);
        tab.closeSubpath();
        p.drawPath(tab);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 20, 22));
        p.drawRoundedRect(QRectF(7, 7, 9, 2), 1, 1);
    });
}

static QPixmap driveGlyph() {
    return makeGlyph(22, 16, [](QPainter& p) {
        QPen pen(QColor(122, 126, 136), 1.2);
        p.setPen(pen);
        p.setBrush(QColor(166, 170, 179));
        p.drawRoundedRect(QRectF(1.5, 4, 19, 9), 1.5, 1.5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(90, 176, 150));
        p.drawEllipse(QPointF(17.5, 8.5), 1.4, 1.4);
    });
}

static QPixmap fileGlyph() {
    return makeGlyph(16, 20, [](QPainter& p) {
        QPen pen(QColor(122, 126, 136), 1.2);
        p.setPen(pen);
        p.setBrush(QColor(166, 170, 179));
        QPainterPath page;
        page.moveTo(2, 1.5);
        page.lineTo(10, 1.5);
        page.lineTo(14, 5.5);
        page.lineTo(14, 18.5);
        page.lineTo(2, 18.5);
        page.closeSubpath();
        p.drawPath(page);
        p.setPen(QPen(QColor(122, 126, 136), 1));
        p.drawLine(10, 1.5, 10, 5.5);
        p.drawLine(10, 5.5, 14, 5.5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 20, 22));
        p.drawRoundedRect(QRectF(4.5, 8, 7, 2), 1, 1);
        p.drawRoundedRect(QRectF(4.5, 12, 5, 2), 1, 1);
    });
}

static QPixmap mediaGlyph() {
    return makeGlyph(16, 20, [](QPainter& p) {
        QPen pen(QColor(122, 126, 136), 1.2);
        p.setPen(pen);
        p.setBrush(QColor(166, 170, 179));
        QPainterPath page;
        page.moveTo(2, 1.5);
        page.lineTo(10, 1.5);
        page.lineTo(14, 5.5);
        page.lineTo(14, 18.5);
        page.lineTo(2, 18.5);
        page.closeSubpath();
        p.drawPath(page);
        p.setPen(QPen(QColor(122, 126, 136), 1));
        p.drawLine(10, 1.5, 10, 5.5);
        p.drawLine(10, 5.5, 14, 5.5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(102, 110, 120));
        QPainterPath tri;
        tri.moveTo(6, 9.5);
        tri.lineTo(12.5, 12);
        tri.lineTo(6, 14.5);
        tri.closeSubpath();
        p.drawPath(tri);
    });
}

class MutedIconProvider : public QFileIconProvider {
public:
    QIcon icon(IconType type) const override {
        switch (type) {
        case Folder:   return QIcon(folderGlyph());
        case Drive:    return QIcon(driveGlyph());
        case Computer: return QIcon(driveGlyph());
        default:       return QIcon(fileGlyph());
        }
    }
    QIcon icon(const QFileInfo& info) const override {
        if (info.isDir()) return QIcon(folderGlyph());
        if (isMediaFileExt(info.absoluteFilePath())) return QIcon(mediaGlyph());
        return QIcon(fileGlyph());
    }
};

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
        setIconSize(QSize(96, 56));
        setGridSize(QSize(116, 80));
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
    void setThumbSize(int w, int h) {
        m_thW = w;
        m_thH = h;
        setIconSize(QSize(w, h));
        setGridSize(QSize(w + 20, h + 30));
    }
    int thumbWidth() const { return m_thW; }
signals:
    void thumbSizeChanged();
protected:
    void wheelEvent(QWheelEvent* e) override {
        if (e->modifiers() & Qt::ControlModifier) {
            const int delta = e->angleDelta().y();
            int w = m_thW;
            if (delta > 0) w += 24;
            else if (delta < 0) w -= 24;
            w = qBound(48, w, 480);
            setThumbSize(w, w * 9 / 16);
            emit thumbSizeChanged();
            e->accept();
            return;
        }
        QListWidget::wheelEvent(e);
    }
private:
    int m_thW = 96;
    int m_thH = 56;

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
    m_model->setIconProvider(new MutedIconProvider);

    m_proxy = new SystemDirFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setDynamicSortFilter(true);

    // Lugares: Início + SSDs externos.
    m_places = new QListWidget(this);
    m_places->setMaximumWidth(140);
    m_places->setSpacing(2);
    m_places->setStyleSheet(QStringLiteral(
        "QListWidget{background:%1; border:1px solid %2;"
        " color:%3; outline:0;}"
        "QListWidget::item{padding:4px 6px; border-radius:3px;}"
        "QListWidget::item:selected{background:%4; color:%5;}")
        .arg(themeColors().base.name())
        .arg(themeColors().dockBorder.name())
        .arg(themeColors().text.name())
        .arg(themeColors().highlight.name())
        .arg(themeColors().highlightedText.name()));
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
    m_upBtn->setIcon(QIcon(QStringLiteral(":/icons/arrow-up.png")));
    m_upBtn->setText(QString());
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
    m_fileList->setIconSize(QSize(18, 14));
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
        "QListWidget{background:%1; border:1px solid %2;"
        " color:%3; outline:0;}")
        .arg(themeColors().base.name())
        .arg(themeColors().dockBorder.name())
        .arg(themeColors().text.name()));
    m_thumbList->hide();
    connect(qobject_cast<MediaThumbList*>(m_thumbList), &MediaThumbList::thumbSizeChanged,
            this, [this]() { populateThumbs(); });

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
        "QWidget{background:%1;}"
        "QTreeView{background:%2; border:1px solid %3;"
        " color:%4; selection-background-color:%5;}"
        "QLineEdit{background:%6; border:1px solid %7;"
        " color:%8; padding:2px 6px; border-radius:3px;}"
        "QPushButton{border:1px solid %9; border-radius:3px;"
        " background:%10; color:%11; padding:2px 8px;}"
        "QPushButton:hover{background:%12; border-color:%13;}")
        .arg(themeColors().window.name())
        .arg(themeColors().base.name())
        .arg(themeColors().dockBorder.name())
        .arg(themeColors().text.name())
        .arg(themeColors().highlight.name())
        .arg(themeColors().inputBg.name())
        .arg(themeColors().inputBorder.name())
        .arg(themeColors().text.name())
        .arg(themeColors().inputBorder.name())
        .arg(themeColors().inputBg.name())
        .arg(themeColors().text.name())
        .arg(themeColors().btnHover.name())
        .arg(themeColors().dockBorder.name()));

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
    const QIcon dirIcon(folderGlyph());
    const QIcon driveIcon(driveGlyph());
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
