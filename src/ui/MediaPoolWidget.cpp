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

#include "MediaPoolWidget.h"
#include "ffmpeg/FFmpegDecoder.h"
#include "ffmpeg/MediaCache.h"
#include "ui/TimelineWidget.h"
#include "ui/SettingsDialog.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QMimeData>
#include <QMessageBox>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QIcon>
#include <QListView>
#include <QDrag>
#include <cmath>
#include <algorithm>

namespace {
QString formatDuration(double s) {
    const int total = (int)std::ceil(s);
    return QString("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Frame representativo do vídeo para a "capa" no pool de mídia.
double poolThumbTime(double duration) {
    return std::clamp(duration * 0.5, 0.2, 2.0);
}

QPixmap makePlaceholderPixmap() {
    QPixmap pm(96, 54);
    pm.fill(QColor(24, 26, 30));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor(70, 74, 82));
    p.drawRect(QRect(1, 1, pm.width() - 2, pm.height() - 2));
    QPolygonF tri;
    tri << QPointF(40, 17) << QPointF(40, 37) << QPointF(58, 27);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(150, 160, 175));
    p.drawPolygon(tri);
    return pm;
}

QPixmap makeAudioIconPixmap() {
    QPixmap pm(96, 54);
    pm.fill(QColor(24, 26, 30));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor(70, 74, 82));
    p.drawRect(QRect(1, 1, pm.width() - 2, pm.height() - 2));
    static const int bars[] = {20, 34, 26, 42, 30, 22, 38, 28, 34, 18, 30, 24};
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(120, 210, 170));
    int x = 22;
    for (int h : bars) {
        const int y = (54 - h) / 2;
        p.drawRoundedRect(QRectF(x, y, 3.5, h), 1, 1);
        x += 6;
    }
    return pm;
}

struct ProbeResult {
    QString path;
    FFmpegMediaInfo info;
};

ProbeResult probeFile(const QString& path) {
    ProbeResult r;
    r.path = path;
    r.info = FFmpegDecoder::probe(path);
    return r;
}
}

PoolList::PoolList(QWidget* parent) : QListWidget(parent) {
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setUniformItemSizes(true);
    setWordWrap(true);
    setViewMode(QListView::IconMode);
    setIconSize(QSize(96, 54));
    setGridSize(QSize(140, 92));
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setSpacing(8);
    setTextElideMode(Qt::ElideRight);
}

QMimeData* PoolList::mimeData(const QList<QListWidgetItem*>& items) const {
    if (items.isEmpty()) return nullptr;
    auto* md = new QMimeData();
    QByteArray data;
    for (const QListWidgetItem* it : items)
        data += it->data(Qt::UserRole).toString().toUtf8() + '\n';
    md->setData(QLatin1String(TimelineWidget::kMimeMedia), data);
    return md;
}

void PoolList::startDrag(Qt::DropActions supportedActions) {
    const QList<QListWidgetItem*> items = selectedItems();
    if (items.isEmpty()) return;

    auto* md = new QMimeData();
    QByteArray data;
    for (const QListWidgetItem* it : items)
        data += it->data(Qt::UserRole).toString().toUtf8() + '\n';
    md->setData(QLatin1String(TimelineWidget::kMimeMedia), data);

    auto* drag = new QDrag(this);
    drag->setMimeData(md);

    QPixmap pm;
    if (items.size() == 1) {
        pm = items.first()->icon().pixmap(96, 54);
    } else {
        pm = QPixmap(80, 60);
        pm.fill(QColor(0, 0, 0, 170));
        QPainter p(&pm);
        p.setPen(Qt::white);
        p.drawText(pm.rect(), Qt::AlignCenter, QString::number(items.size()));
    }
    if (!pm.isNull()) {
        drag->setPixmap(pm);
        drag->setHotSpot(QPoint(pm.width() / 2, pm.height() / 2));
    }

    drag->exec(supportedActions, Qt::CopyAction);
}

MediaPoolWidget::MediaPoolWidget(QWidget* parent) : QWidget(parent) {
    m_list = new PoolList(this);

    m_addBtn = new QPushButton(tr("Adicionar"), this);
    m_removeBtn = new QPushButton(tr("Remover"), this);
    connect(m_addBtn, &QPushButton::clicked, this, &MediaPoolWidget::addFiles);
    connect(m_removeBtn, &QPushButton::clicked, this, &MediaPoolWidget::removeSelected);
    connect(&MediaCache::instance(), &MediaCache::thumbnailReady,
            this, &MediaPoolWidget::onThumbReady);
    // Duplo clique: adiciona a mídia na timeline no playhead (fallback para o
    // arrastar/soltar, que pode falhar em alguns ambientes/Wayland).
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        if (it) emit mediaToTimeline(it->data(Qt::UserRole).toString());
    });

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->addWidget(m_addBtn);
    bar->addWidget(m_removeBtn);
    bar->addStretch();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);
    lay->addLayout(bar);
    lay->addWidget(m_list, 1);
}

void MediaPoolWidget::setProject(Project* p) {
    m_project = p;
    refresh();
}

void MediaPoolWidget::refreshFromProject() {
    refresh();
}

void MediaPoolWidget::refresh() {
    m_list->clear();
    if (!m_project) return;
    if (m_audioIcon.isNull()) m_audioIcon = makeAudioIconPixmap().toImage();
    if (m_videoPlaceholder.isNull()) m_videoPlaceholder = makePlaceholderPixmap().toImage();
    for (const MediaItem& m : m_project->media) {
        auto* item = new QListWidgetItem();
        QString tags;
        if (m.hasVideo) tags += QString("Vídeo %1x%2  ·  ").arg(m.width).arg(m.height);
        if (m.hasAudio) tags += "Áudio  ·  ";
        if (QFileInfo(m.filePath).suffix().compare(QLatin1String("mkv"), Qt::CaseInsensitive) == 0)
            tags += QString("[MKV experimental]  ·  ");
        item->setText(QString("%1\n%2%3").arg(m.name, tags, formatDuration(m.duration)));
        item->setData(Qt::UserRole, m.id);
        item->setToolTip(m.filePath);
        if (m.hasVideo) {
            if (m_thumbs.contains(m.id)) {
                item->setIcon(QIcon(QPixmap::fromImage(m_thumbs.value(m.id))));
            } else {
                item->setIcon(QIcon(QPixmap::fromImage(m_videoPlaceholder)));
                requestPoolThumb(m.filePath, poolThumbTime(m.duration), m.id);
            }
        } else {
            item->setIcon(QIcon(QPixmap::fromImage(m_audioIcon)));
        }
        m_list->addItem(item);
    }
}

void MediaPoolWidget::requestPoolThumb(const QString& path, double seconds, const QString& mediaId) {
    MediaCache& cache = MediaCache::instance();
    const QImage img = cache.thumb(path, seconds);
    if (!img.isNull()) {
        setThumb(mediaId, img);
        return;
    }
    cache.requestThumb(path, seconds);
}

void MediaPoolWidget::onThumbReady(const QString& filePath, double seconds) {
    if (!m_project) return;
    const QImage img = MediaCache::instance().thumb(filePath, seconds);
    if (img.isNull()) return;
    for (const MediaItem& m : m_project->media) {
        if (m.hasVideo && m.filePath == filePath && !m_thumbs.contains(m.id))
            setThumb(m.id, img);
    }
}

void MediaPoolWidget::setThumb(const QString& mediaId, const QImage& img) {
    if (img.isNull()) return;
    m_thumbs.insert(mediaId, img);
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem* it = m_list->item(i);
        if (it && it->data(Qt::UserRole).toString() == mediaId)
            it->setIcon(QIcon(QPixmap::fromImage(img)));
    }
}

void MediaPoolWidget::addFiles() {
    if (!m_project) return;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Importar mídia"), QString(),
        tr("Vídeo e áudio (*.mp4 *.mov *.mkv *.avi *.webm *.m4v *.ts *.flv *.wmv "
           "*.mp3 *.wav *.aac *.flac *.ogg *.m4a *.opus *.wma);;Todos os arquivos (*)"));
    if (files.isEmpty()) return;
    emit editStart();

    auto* watcher = new QFutureWatcher<ProbeResult>(this);
    connect(watcher, &QFutureWatcher<ProbeResult>::finished, this, [this, watcher]() {
        const auto results = watcher->future().results();
        int added = 0;
        int invalid = 0;
        QStringList imported;
        for (const ProbeResult& r : results) {
            const FFmpegMediaInfo& info = r.info;
            if (!info.hasVideo && !info.hasAudio) {
                ++invalid;
                QMessageBox::warning(this, tr("Mídia inválida"),
                                     tr("Não foi possível ler o arquivo:\n%1").arg(r.path));
                continue;
            }
            MediaItem m;
            m.id = newId();
            m.filePath = r.path;
            m.name = QFileInfo(r.path).completeBaseName();
            m.duration = info.duration;
            m.width = info.width;
            m.height = info.height;
            m.hasVideo = info.hasVideo;
            m.hasAudio = info.hasAudio;
            m_project->media.append(m);
            imported.append(r.path);
            ++added;
        }
        SettingsDialog::warnMkvIfNeeded(this, imported);
        if (added > 0) {
            refresh();
            emit mediaAdded(QString());
        }
        emit importFinished(added, invalid);
        watcher->deleteLater();
    });
    connect(watcher, &QFutureWatcher<ProbeResult>::progressValueChanged, this,
            [this](int v) { emit importProgress(v); });

    emit importStarted();
    watcher->setFuture(QtConcurrent::mapped(files, probeFile));
}

void MediaPoolWidget::removeSelected() {
    if (!m_project) return;
    const auto items = m_list->selectedItems();
    if (items.isEmpty()) return;
    emit editStart();
    for (const QListWidgetItem* it : items) {
        const QString id = it->data(Qt::UserRole).toString();
        m_thumbs.remove(id);
        for (int i = 0; i < m_project->media.size(); ++i) {
            if (m_project->media[i].id == id) {
                m_project->media.removeAt(i);
                break;
            }
        }
        const auto removeClip = [id](Track& t) {
            t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(),
                                         [id](const Clip& c) { return c.mediaId == id; }),
                          t.clips.end());
        };
        for (Track& t : m_project->videoTracks) removeClip(t);
        for (Track& t : m_project->audioTracks) removeClip(t);
    }
    refresh();
    emit mediaChanged();
}
