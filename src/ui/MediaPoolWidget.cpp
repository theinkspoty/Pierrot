// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MediaPoolWidget.h"
#include "ffmpeg/FFmpegDecoder.h"
#include "ffmpeg/MediaCache.h"
#include "ui/TimelineWidget.h"
#include "ui/SettingsDialog.h"
#include "generators.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QList>
#include <QPushButton>
#include <QDebug>
#include <QProgressBar>
#include <QLineEdit>
#include <QLabel>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QIcon>
#include <QListView>
#include <QApplication>
#include <QMouseEvent>
#include <QRubberBand>
#include <QScrollBar>
#include <QItemSelection>
#include <QLabel>
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMenu>
#include <QColorDialog>
#include <QInputDialog>
#include <QCursor>
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
    // O arrasto é feito manualmente (ver mouse*Event abaixo) para funcionar
    // mesmo em ambientes onde o DnD do compositor falha (ex.: Wayland).
    setDragEnabled(false);
    setDragDropMode(QAbstractItemView::NoDragDrop);
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
    // Aceita arquivos arrastados do sistema para importar mídia.
    setAcceptDrops(true);
}

void PoolList::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
    else
        e->ignore();
}

void PoolList::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
    else
        e->ignore();
}

void PoolList::dropEvent(QDropEvent* e) {
    QStringList files;
    const QList<QUrl> urls = e->mimeData()->urls();
    for (const QUrl& u : urls)
        if (u.isLocalFile())
            files << u.toLocalFile();
    if (files.isEmpty()) { e->ignore(); return; }
    emit filesDropped(files);
    e->acceptProposedAction();
}

void PoolList::mousePressEvent(QMouseEvent* e) {
    m_pressPos = e->position().toPoint();
    m_pressItem = itemAt(m_pressPos);
    m_pressWasSelected = m_pressItem
        ? selectionModel()->isSelected(indexFromItem(m_pressItem))
        : false;
    m_pressCtrl = (e->modifiers() & Qt::ControlModifier) != 0;
    m_dragging = false;
    m_bandActive = false;
    m_bandAdd = m_pressCtrl;

    // Nunca deixa o rubber band nativo da base começar: ele apareceria junto
    // com o nosso e o estado ficaria preso em DragSelectingState.
    setState(QAbstractItemView::NoState);

    if (e->button() == Qt::LeftButton) {
        e->accept();
        if (m_pressItem) {
            if (m_pressCtrl) {
                // Ctrl+clique: o toggle é aplicado no release (e no início do
                // arrasto, se virar arrasto). Não repassamos o press à base,
                // para que a mudança de seleção seja única e previsível em
                // qualquer versão/flags dela. Só atualiza o índice atual.
                selectionModel()->setCurrentIndex(indexFromItem(m_pressItem),
                                                  QItemSelectionModel::NoUpdate);
            } else {
                // A base cuida da seleção no clique e do índice atual. Um item
                // já selecionado NÃO muda de seleção no press (command NoUpdate),
                // então arrastar um item de uma seleção múltipla mantém todos
                // selecionados.
                QListWidget::mousePressEvent(e);
            }
        } else {
            // Clique no vazio: seleção em caixa feita à mão (não chama a base,
            // senão o rubber band nativo apareceria junto).
            if (!m_band)
                m_band = new QRubberBand(QRubberBand::Rectangle, viewport());
            m_band->setGeometry(QRect(m_pressPos, QSize(1, 1)));
            m_band->hide();
        }
        return;
    }
    QListWidget::mousePressEvent(e);
}

void PoolList::mouseMoveEvent(QMouseEvent* e) {
    const QPoint p = e->position().toPoint();
    // Cruza o limiar de arrasto: passa a acompanhar o cursor globalmente via
    // filtro de eventos (funciona mesmo no Wayland, onde o grabMouse/cliente
    // é limitado). Nada do DnD do compositor é usado.
    if ((e->buttons() & Qt::LeftButton) && !m_dragging && !m_bandActive
        && (p - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        if (m_pressItem) {
            if (m_pressCtrl) {
                // Um Ctrl+clique que virou arrasto: garante que o item entra na
                // seleção para o arrasto levá-lo junto com os demais.
                const QModelIndex idx = indexFromItem(m_pressItem);
                if (!selectionModel()->isSelected(idx))
                    selectionModel()->select(idx, QItemSelectionModel::Select);
            } else if (!selectionModel()->isSelected(indexFromItem(m_pressItem))) {
                // Garante que o item arrastado está na seleção (senão a soltura
                // viria com a lista vazia e nada seria adicionado).
                selectionModel()->select(indexFromItem(m_pressItem),
                                        QItemSelectionModel::Select);
            }
            m_dragging = true;
            qApp->installEventFilter(this);
            showDragIcon(e->globalPosition().toPoint());
            emit dragHover(e->globalPosition().toPoint());
        } else {
            m_bandActive = true;
            qApp->installEventFilter(this);
            updateBand(e->globalPosition().toPoint());
        }
    }
    // Com o botão esquerdo pressionado nós cuidamos de tudo (arrasto de mídia
    // ou caixa de seleção); repassar à base deixaria o rubber band nativo
    // aparecer junto e/ou mudaria a seleção durante o arrasto.
    if (!(e->buttons() & Qt::LeftButton))
        QListWidget::mouseMoveEvent(e);
}

bool PoolList::eventFilter(QObject* obj, QEvent* ev) {
    if (m_dragging) {
        if (ev->type() == QEvent::MouseMove) {
            const QPoint g = static_cast<QMouseEvent*>(ev)->globalPosition().toPoint();
            moveDragIcon(g);
            emit dragHover(g);
            return false;
        }
        if (ev->type() == QEvent::MouseButtonRelease) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) {
                const QPoint g = me->globalPosition().toPoint();
                cancelDrag();
                emit mediaDropped(selectedIds(), g);
                return true; // não deixa a base "finalizar" e limpar a seleção
            }
        }
        if (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::WindowDeactivate) {
            // Novo clique (ou perda de foco) durante o arrasto: cancela sem soltar.
            cancelDrag();
            return true;
        }
    } else if (m_bandActive) {
        if (ev->type() == QEvent::MouseMove) {
            updateBand(static_cast<QMouseEvent*>(ev)->globalPosition().toPoint());
            return true;
        }
        if (ev->type() == QEvent::MouseButtonRelease) {
            const auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) {
                finalizeBand();
                return true;
            }
        }
        if (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::WindowDeactivate) {
            cancelBand();
            return true;
        }
    }
    return QListWidget::eventFilter(obj, ev);
}

void PoolList::mouseReleaseEvent(QMouseEvent* e) {
    hideDragIcon();
    if (m_dragging || m_bandActive) {
        // O filtro global cuida do release (o cursor pode estar sobre a
        // timeline ou fora da lista); aqui só evita o comportamento padrão.
        e->accept();
        return;
    }
    if (e->button() == Qt::LeftButton) {
        const bool shift = (e->modifiers() & Qt::ShiftModifier) != 0;
        if (!m_pressItem) {
            // Clique simples no vazio (sem arrastar): limpa a seleção (Ctrl mantém).
            if (!(e->modifiers() & Qt::ControlModifier))
                clearSelection();
        } else if (m_pressCtrl) {
            // Ctrl+clique (sem arrasto): alterna o item na seleção, permitindo
            // selecionar várias mídias de uma em uma.
            selectionModel()->select(indexFromItem(m_pressItem),
                                     QItemSelectionModel::Toggle);
        } else if (!shift && !m_pressWasSelected) {
            // Clique num item que não estava selecionado: seleciona só ele.
            // Se já estava selecionado (veio de uma seleção múltipla), mantém
            // todos selecionados para poder arrastá-los juntos depois.
            selectionModel()->select(indexFromItem(m_pressItem),
                                     QItemSelectionModel::ClearAndSelect);
        }
        e->accept();
        m_pressItem = nullptr;
        return;
    }
    QListWidget::mouseReleaseEvent(e);
}

void PoolList::cancelDrag() {
    if (qApp) qApp->removeEventFilter(this);
    hideDragIcon();
    m_dragging = false;
    m_pressItem = nullptr;
    emit dragHoverCleared();
}

void PoolList::updateBand(const QPoint& globalPos) {
    if (!m_band) return;
    const QPoint vp = viewport()->mapFromGlobal(globalPos);
    const QRect rect = QRect(m_pressPos, vp).normalized();
    m_band->setGeometry(rect);
    m_band->show();

    // Auto-scroll perto das bordas para selecionar listas longas.
    const int edge = 24;
    QScrollBar* sb = verticalScrollBar();
    if (vp.y() < edge)
        sb->setValue(sb->value() - 8);
    else if (vp.y() > viewport()->height() - edge)
        sb->setValue(sb->value() + 8);

    QItemSelection sel;
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem* it = item(i);
        if (visualItemRect(it).intersects(rect)) {
            const QModelIndex idx = indexFromItem(it);
            sel.select(idx, idx);
        }
    }
    const auto flags = m_bandAdd ? QItemSelectionModel::Select
                                 : QItemSelectionModel::ClearAndSelect;
    selectionModel()->select(sel, flags);
}

void PoolList::finalizeBand() {
    if (qApp) qApp->removeEventFilter(this);
    if (m_band) m_band->hide();
    const QRect r = m_band ? m_band->geometry() : QRect(m_pressPos, QSize(0, 0));
    if (r.width() < 4 && r.height() < 4 && !m_bandAdd)
        clearSelection(); // clique simples no vazio: desfaz a seleção (Ctrl mantém)
    m_bandActive = false;
    m_pressItem = nullptr;
}

void PoolList::cancelBand() {
    if (qApp) qApp->removeEventFilter(this);
    if (m_band) m_band->hide();
    m_bandActive = false;
    m_pressItem = nullptr;
}

void PoolList::showDragIcon(const QPoint& globalPos) {
    if (!m_dragIcon) {
        // Miniatura que segue o cursor durante o arrasto. Filha da janela
        // principal (fica por cima da timeline) e transparente para o mouse
        // (não intercepta o clique/arrasto que está em andamento).
        m_dragIcon = new QLabel(window());
        m_dragIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_dragIcon->setAttribute(Qt::WA_ShowWithoutActivating);
        m_dragIcon->setMargin(2);
    }
    const QPixmap pm = makeDragPixmap();
    if (pm.isNull()) return;
    m_dragIcon->setPixmap(pm);
    m_dragIcon->adjustSize();
    m_dragIcon->move(window()->mapFromGlobal(globalPos + QPoint(8, 8)));
    m_dragIcon->show();
    m_dragIcon->raise();
}

void PoolList::moveDragIcon(const QPoint& globalPos) {
    if (m_dragIcon && m_dragIcon->isVisible())
        m_dragIcon->move(window()->mapFromGlobal(globalPos + QPoint(8, 8)));
}

void PoolList::hideDragIcon() {
    if (m_dragIcon) m_dragIcon->hide();
}

QPixmap PoolList::makeDragPixmap() const {
    const QList<QListWidgetItem*> items = selectedItems();
    if (items.isEmpty()) return QPixmap();
    QPixmap pm = items.first()->icon().pixmap(96, 54);
    if (pm.isNull()) {
        // Sem miniatura (ex.: áudio sem imagem): um quadro padrão para o
        // arrasto ter feedback visual.
        pm = QPixmap(96, 54);
        pm.fill(QColor(40, 42, 48));
        QPainter pp(&pm);
        pp.setPen(QColor(150, 155, 165));
        pp.drawRect(1, 1, pm.width() - 2, pm.height() - 2);
        pp.drawText(pm.rect(), Qt::AlignCenter, items.first()->text());
    }
    if (items.size() > 1) {
        // Vários itens: selo com a contagem no canto da miniatura.
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 190));
        p.drawEllipse(QRectF(pm.width() - 20, pm.height() - 20, 20, 20));
        p.setPen(Qt::white);
        p.drawText(QRect(pm.width() - 20, pm.height() - 20, 20, 20),
                   Qt::AlignCenter, QString::number(items.size()));
    }
    return pm;
}

QStringList PoolList::selectedIds() const {
    QStringList ids;
    for (const QListWidgetItem* it : selectedItems())
        ids << it->data(Qt::UserRole).toString();
    return ids;
}

MediaPoolWidget::MediaPoolWidget(QWidget* parent) : QWidget(parent) {
    m_list = new PoolList(this);
    // Repassa o arrasto manual da lista para a janela principal, que liga ao
    // feedback e à soltura na timeline.
    connect(m_list, &PoolList::dragHover, this, &MediaPoolWidget::dragHover);
    connect(m_list, &PoolList::dragHoverCleared, this, &MediaPoolWidget::dragHoverCleared);
    connect(m_list, &PoolList::mediaDropped, this, &MediaPoolWidget::mediaDropped);
    // Importa arquivos arrastados do sistema para o painel.
    connect(m_list, &PoolList::filesDropped, this, &MediaPoolWidget::importPaths);
    setAcceptDrops(true);

    m_addBtn = new QPushButton(tr("Adicionar"), this);
    m_removeBtn = new QPushButton(tr("Remover"), this);
    auto* genBtn = new QPushButton(tr("Gerador"), this);
    m_addBtn->setToolTip(tr("Importar mídia (Ctrl+I)"));
    m_removeBtn->setToolTip(tr("Remover a mídia selecionada do projeto"));
    genBtn->setToolTip(tr("Criar mídia gerada (cor sólida, como no Vegas)"));
    connect(m_addBtn, &QPushButton::clicked, this, &MediaPoolWidget::addFiles);
    connect(m_removeBtn, &QPushButton::clicked, this, &MediaPoolWidget::removeSelected);
    connect(genBtn, &QPushButton::clicked, this, &MediaPoolWidget::addGenerator);
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
    bar->addWidget(genBtn);
    bar->addWidget(m_removeBtn);
    bar->addStretch();

    m_importBar = new QProgressBar(this);
    m_importBar->setRange(0, 100);
    m_importBar->setValue(0);
    m_importBar->setTextVisible(false);
    m_importBar->setFixedHeight(10);
    m_importBar->setFormat(QString());
    m_importBar->hide();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);
    lay->addLayout(bar);
    lay->addWidget(m_importBar);
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
    // Agrupa os pedidos de thumb por arquivo: um único lote por caminho em vez
    // de uma decodificação isolada por item (bem mais rápido ao importar N vídeos).
    QHash<QString, QList<double>> wantByFile;
    for (const MediaItem& m : m_project->media) {
        auto* item = new QListWidgetItem();
        QString tags;
        if (m.hasVideo) tags += QString("Vídeo %1x%2  ·  ").arg(m.width).arg(m.height);
        if (m.hasAudio) {
            if (m.audioStreams > 1)
                tags += QString("Áudio × %1  ·  ").arg(m.audioStreams);
            else
                tags += "Áudio  ·  ";
            if (m.audioChannels.size() == m.audioStreams) {
                QStringList chs;
                for (int c : m.audioChannels) chs << QString::number(c);
                tags += QString("%1 canais/faixa  ·  ").arg(chs.join("/"));
            }
        }
        if (QFileInfo(m.filePath).suffix().compare(QLatin1String("mkv"), Qt::CaseInsensitive) == 0)
            tags += QString("[MKV experimental]  ·  ");
        item->setText(QString("%1\n%2%3").arg(m.name, tags, formatDuration(m.duration)));
        item->setData(Qt::UserRole, m.id);
        item->setToolTip(m.isSolid ? tr("Mídia gerada (gerador)") : m.filePath);
        if (m.isSolid) {
            // Gerador de mídia: ícone com o padrão gerado, sem thumb.
            const int iw = qBound(16, m.width > 0 ? m.width : 96, 96);
            const int ih = qBound(9, m.height > 0 ? m.height : 54, 54);
            QImage ic = generatorFrame(m, iw, ih);
            setThumb(m.id, ic);
            item->setIcon(QIcon(QPixmap::fromImage(ic)));
        } else if (m.hasVideo) {
            if (m_thumbs.contains(m.id)) {
                item->setIcon(QIcon(QPixmap::fromImage(m_thumbs.value(m.id))));
            } else {
                item->setIcon(QIcon(QPixmap::fromImage(m_videoPlaceholder)));
                const double t = poolThumbTime(m.duration);
                MediaCache& cache = MediaCache::instance();
                const QImage img = cache.thumb(m.filePath, t);
                if (!img.isNull()) {
                    setThumb(m.id, img);
                } else {
                    wantByFile[m.filePath].append(t);
                }
            }
        } else {
            item->setIcon(QIcon(QPixmap::fromImage(m_audioIcon)));
        }
        m_list->addItem(item);
    }
    for (auto it = wantByFile.constBegin(); it != wantByFile.constEnd(); ++it)
        MediaCache::instance().requestThumbs(it.key(), it.value());
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
           "*.3gp *.mpg *.mpeg *.ogv *.mts *.m2ts *.vob "
           "*.mp3 *.wav *.aac *.flac *.ogg *.m4a *.opus *.wma *.aiff *.aif *.au "
           "*.ac3 *.amr *.ape *.caf *.dts *.mka *.mid *.midi *.mp2 *.oga *.spx "
           "*.w64 *.wv *.alac *.mp1 *.mpa *.ra *.wpl *.tta *.tak *.shn);;"
           "Imagens (*.jpg *.jpeg *.png *.bmp *.gif *.webp *.tif *.tiff *.svg);;"
           "Todos os arquivos (*)"));
    if (files.isEmpty()) return;
    importPaths(files);
}

// Importa uma lista de arquivos (botão Adicionar ou arrastar do sistema).
void MediaPoolWidget::importPaths(const QStringList& files) {
    if (!m_project || files.isEmpty()) return;
    emit editStart();

    auto* watcher = new QFutureWatcher<ProbeResult>(this);
    connect(watcher, &QFutureWatcher<ProbeResult>::finished, this, [this, watcher]() {
        const auto results = watcher->future().results();
        int added = 0;
        int invalid = 0;
        QStringList imported;
        bool has4k = false;
        for (const ProbeResult& r : results) {
            const FFmpegMediaInfo& info = r.info;
            if (!info.hasVideo && !info.hasAudio) {
                ++invalid;
                QMessageBox::warning(this, tr("Mídia inválida"),
                                     tr("Não foi possível ler o arquivo:\n%1").arg(r.path));
                continue;
            }
            // 4K+ em VÍDEO (fotos grandes são decodificadas uma vez e ficam
            // em cache — não engasgam o preview).
            if (info.hasVideo && info.duration > 0.0
                && qMax(info.width, info.height) >= 3840)
                has4k = true;
            MediaItem m;
            m.id = newId();
            m.filePath = r.path;
            m.name = QFileInfo(r.path).completeBaseName();
            m.duration = info.duration;
            m.width = info.width;
            m.height = info.height;
            m.hasVideo = info.hasVideo;
            m.hasAudio = info.hasAudio;
            m.audioStreams = info.audioStreams;
            m.audioChannels = info.audioChannels;
            m_project->media.append(m);
            imported.append(r.path);
            ++added;
        }
        SettingsDialog::warnMkvIfNeeded(this, imported);
        SettingsDialog::warn4kIfNeeded(this, has4k);
        if (added > 0) {
            refresh();
            emit mediaAdded(QString());
        }
        m_importBar->hide();
        emit importFinished(added, invalid);
        watcher->deleteLater();
    });
    connect(watcher, &QFutureWatcher<ProbeResult>::progressValueChanged, this,
            [this, files](int v) {
                m_importBar->setValue(v);
                emit importProgress(v);
            });

    emit importStarted();
    m_importBar->setRange(0, files.size());
    m_importBar->setValue(0);
    m_importBar->show();
    watcher->setFuture(QtConcurrent::mapped(files, probeFile));
}

// Geradores de mídia (estilo Vegas): cria mídia virtual sem arquivo que pode
// ser arrastada para a timeline — cor sólida, gradiente, checkerboard ou
// ruído (grão). Menu com cores rápidas + personalizadas.
void MediaPoolWidget::addGenerator() {
    if (!m_project) return;
    QMenu menu(this);
    struct Quick { QString name; QColor color; };
    const QList<Quick> quicks = {
        {tr("Vermelho"), QColor(255, 0, 0)},
        {tr("Verde"), QColor(0, 180, 0)},
        {tr("Azul"), QColor(0, 90, 255)},
        {tr("Amarelo"), QColor(255, 220, 0)},
        {tr("Branco"), QColor(255, 255, 255)},
        {tr("Preto"), QColor(0, 0, 0)},
        {tr("Laranja"), QColor(255, 130, 0)},
    };
    QMenu* solidMenu = menu.addMenu(tr("Cor sólida"));
    QAction* pick = solidMenu->addAction(tr("Personalizada…"));
    QHash<QAction*, QColor> actions;
    for (const Quick& q : quicks) {
        QAction* a = solidMenu->addAction(q.name);
        actions.insert(a, q.color);
    }
    QAction* gradAct = menu.addAction(tr("Gradiente…"));
    QAction* checkAct = menu.addAction(tr("Checkboard…"));
    QAction* noiseAct = menu.addAction(tr("Ruído (grão)…"));
    QAction* chosen = menu.exec(QCursor::pos());

    MediaItem m;
    auto setupBase = [this, &m]() {
        m.id = newId();
        m.isSolid = true;
        m.hasVideo = true;
        m.width = m_project->width;
        m.height = m_project->height;
        m.duration = m_project->duration() > 0 ? m_project->duration() : 5.0;
    };

    if (chosen == pick || actions.contains(chosen)) {
        QColor color;
        if (chosen == pick) {
            color = QColorDialog::getColor(Qt::black, this, tr("Cor da mídia"));
            if (!color.isValid()) return;
            color.setAlpha(255);
        } else {
            color = actions.value(chosen);
        }
        QString hex = color.name().toUpper();
        m.generator = QString();
        m.solidColor = color;
        m.name = tr("Cor %1").arg(hex.startsWith(QLatin1Char('#')) ? hex.mid(1) : hex);
    } else if (chosen == gradAct) {
        QColor a = QColorDialog::getColor(QColor(255, 255, 255), this, tr("Cor inicial do gradiente"));
        if (!a.isValid()) return;
        QColor b = QColorDialog::getColor(Qt::black, this, tr("Cor final do gradiente"));
        if (!b.isValid()) return;
        m.generator = QStringLiteral("gradient");
        m.solidColor = a;
        m.solidColor2 = b;
        m.name = tr("Gradiente");
    } else if (chosen == checkAct) {
        QColor a = QColorDialog::getColor(Qt::black, this, tr("Cor 1 do checkboard"));
        if (!a.isValid()) return;
        QColor b = QColorDialog::getColor(Qt::white, this, tr("Cor 2 do checkboard"));
        if (!b.isValid()) return;
        bool ok = false;
        const int cells = QInputDialog::getInt(this, tr("Checkboard"),
                                               tr("Células por lado:"), 8, 2, 64, 1, &ok);
        if (!ok) return;
        m.generator = QStringLiteral("checkerboard");
        m.solidColor = a;
        m.solidColor2 = b;
        m.genCells = cells;
        m.name = tr("Checkboard %1×%1").arg(cells);
    } else if (chosen == noiseAct) {
        QColor a = QColorDialog::getColor(Qt::black, this, tr("Cor do ruído"));
        if (!a.isValid()) return;
        QColor b = QColorDialog::getColor(Qt::white, this, tr("Cor do grão"));
        if (!b.isValid()) return;
        m.generator = QStringLiteral("noise");
        m.solidColor = a;
        m.solidColor2 = b;
        m.name = tr("Ruído");
    } else {
        return;
    }

    setupBase();
    emit editStart();
    m_project->media.append(m);
    refresh();
    emit mediaAdded(m.id);
}

// Arrastar arquivos do sistema sobre o painel (fora da lista) também importa.
void MediaPoolWidget::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
    else
        e->ignore();
}

void MediaPoolWidget::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
    else
        e->ignore();
}

void MediaPoolWidget::dropEvent(QDropEvent* e) {
    QStringList files;
    const QList<QUrl> urls = e->mimeData()->urls();
    for (const QUrl& u : urls)
        if (u.isLocalFile())
            files << u.toLocalFile();
    if (files.isEmpty()) { e->ignore(); return; }
    importPaths(files);
    e->acceptProposedAction();
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
