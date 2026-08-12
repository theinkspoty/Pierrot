#include "MediaPoolWidget.h"
#include "ffmpeg/FFmpegDecoder.h"
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
#include <cmath>
#include <algorithm>

namespace {
QString formatDuration(double s) {
    const int total = (int)std::ceil(s);
    return QString("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
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
}

QMimeData* PoolList::mimeData(const QList<QListWidgetItem*>& items) const {
    auto* md = new QMimeData();
    QByteArray data;
    for (const QListWidgetItem* it : items)
        data += it->data(Qt::UserRole).toString().toUtf8() + '\n';
    md->setData(QLatin1String(TimelineWidget::kMimeMedia), data);
    return md;
}

MediaPoolWidget::MediaPoolWidget(QWidget* parent) : QWidget(parent) {
    m_list = new PoolList(this);

    m_addBtn = new QPushButton(tr("Adicionar"), this);
    m_removeBtn = new QPushButton(tr("Remover"), this);
    connect(m_addBtn, &QPushButton::clicked, this, &MediaPoolWidget::addFiles);
    connect(m_removeBtn, &QPushButton::clicked, this, &MediaPoolWidget::removeSelected);

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
        m_list->addItem(item);
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
