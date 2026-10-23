// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/timeline/TimelineWidget.h"
#include "colombina/models/Project.h"
#include "laartman/Decoder.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/monitor/TrimmerDialog.h"
#include "colombina/util.h"

#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QApplication>
#include <QInputDialog>
#include <QDialog>
#include <QScrollBar>
#include <QTimer>
#include <QLineEdit>
#include <QMenu>
#include <QAction>
#include <QSet>
#include <QToolTip>
#include <algorithm>
#include <cmath>

namespace {


double mediaInsertDur(const MediaItem& m) {
    if (isImageFile(m.filePath) && !Decoder::isAnimatedImage(m.filePath))
        return 3.0;
    return m.duration > 0 ? m.duration : 1.0;
}
} // namespace

// ── Drag & Drop (eventos Qt) ───────────────────────────────────────────

void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QLatin1String(kMimeMedia))
        || md->hasUrls()) {
        e->acceptProposedAction();
        e->accept();
    }
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QLatin1String(kMimeMedia))
        || md->hasUrls()) {
        int row = -1;
        bool audio = false;
        QString gid;
        if (folderStripAt(e->position().toPoint().y(), gid)) {
            m_dragHoverRow = -1;
        } else if (rowFromY(e->position().toPoint().y(), row, audio)) {
            m_dragHoverRow = row;
            m_dragHoverAudio = audio;
        } else {
            m_dragHoverRow = -1;
        }
        // Prévia (estilo Premiere): duração representativa da mídia arrastada
        // para desenhar a "fantasma" do clipe no tamanho certo.
        m_dragHoverT = snapTime(std::max(0.0, xToTime(e->position().toPoint().x())));
        m_dragHoverDur = 0.0;
        m_dragHoverName.clear();
        if (m_project && md->hasFormat(QLatin1String(kMimeMedia))) {
            const QByteArray data = md->data(QLatin1String(kMimeMedia));
            for (QByteArray line : data.split('\n')) {
                line = line.trimmed();
                if (line.isEmpty()) continue;
                const MediaItem* m = m_project->findMedia(QString::fromLatin1(line));
                if (m && (m->hasVideo || m->hasAudio)) {
                    m_dragHoverDur = mediaInsertDur(*m);
                    m_dragHoverName = m->name;
                    break;
                }
            }
        } else if (md->hasUrls()) {
            for (const QUrl& u : md->urls()) {
                if (!u.isLocalFile()) continue;
                const QString path = u.toLocalFile();
                // Cache probe results to avoid expensive re-probe on every mouse move.
                MediaInfo info;
                auto it = m_dragProbeCache.find(path);
                if (it != m_dragProbeCache.end()) {
                    info = it.value();
                } else {
                    info = Decoder::probe(path);
                    m_dragProbeCache.insert(path, info);
                }
                if (info.hasVideo || info.hasAudio) {
                    m_dragHoverDur = info.duration > 0 ? info.duration : 1.0;
                    m_dragHoverName = QFileInfo(path).completeBaseName();
                    break;
                }
            }
        }
        if (m_dragHoverDur <= 0.0) m_dragHoverDur = 1.0;
        update();
        e->acceptProposedAction();
        e->accept();
    }
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) {
    m_dragHoverRow = -1;
    m_dragHoverDur = 0.0;
    m_dragHoverName.clear();
    m_dragProbeCache.clear();
    update();
}

void TimelineWidget::dropEvent(QDropEvent* e) {
    if (!m_project) return;
    const QMimeData* md = e->mimeData();

    // .obj / .blend → malha da Mesa (não é mídia FFmpeg)
    if (md->hasUrls()) {
        QStringList objs;
        QStringList blends;
        for (const QUrl& u : md->urls()) {
            if (!u.isLocalFile()) continue;
            const QString path = u.toLocalFile();
            if (path.endsWith(QLatin1String(".obj"), Qt::CaseInsensitive))
                objs.append(path);
            else if (path.endsWith(QLatin1String(".blend"), Qt::CaseInsensitive))
                blends.append(path);
        }
        if (!objs.isEmpty() || !blends.isEmpty()) {
            if (m_project->mesas.isEmpty()) criarMesa();
            const QString mesaId = m_project->mesas.isEmpty()
                                      ? QString()
                                      : m_project->mesas.last().id;
            if (!mesaId.isEmpty()) {
                for (const QString& p : objs) addMeshToMesa(mesaId, p);
                for (const QString& p : blends) Q_UNUSED(importBlendAsMesh(mesaId, p));
                emit mesaOpenRequested(mesaId);
            }
            e->acceptProposedAction();
            return;
        }
    }

    // ── Arrasto de mídia (existente) ─────────────────────────────────────
    QStringList mediaIds;
    if (md->hasFormat(QLatin1String(kMimeMedia))) {
        // Arrasto vindo da lista de mídia do aplicativo (caminho clássico de
        // DnD; a pool também chama finishDrop() direto no arrasto manual).
        const QByteArray data = md->data(QLatin1String(kMimeMedia));
        for (QByteArray line : data.split('\n')) {
            line = line.trimmed();
            if (!line.isEmpty()) mediaIds.append(QString::fromUtf8(line));
        }
    } else if (md->hasUrls()) {
        // Arrasto de arquivos vindos do gerenciador de arquivos: importa para
        // a mídia do projeto e já coloca clipes no ponto solto.
        int added = 0;
        QStringList imported;
        for (const QUrl& u : md->urls()) {
            if (!u.isLocalFile()) continue;
            const QString path = u.toLocalFile();
            // Use cached probe result if available (from dragMoveEvent).
            MediaInfo info;
            auto it = m_dragProbeCache.find(path);
            if (it != m_dragProbeCache.end()) {
                info = it.value();
            } else {
                info = Decoder::probe(path);
            }
            if (!info.hasVideo && !info.hasAudio) continue;
            MediaItem m;
            m.id = newId();
            m.filePath = path;
            m.name = QFileInfo(path).completeBaseName();
            m.duration = info.duration;
            m.width = info.width;
            m.height = info.height;
            m.hasVideo = info.hasVideo;
            m.hasAudio = info.hasAudio;
            m.audioStreams = info.audioStreams;
            m.audioChannels = info.audioChannels;
            m_project->media.append(m);
            mediaIds.append(m.id);
            imported.append(path);
            ++added;
        }
        if (added == 0) return;
        SettingsDialog::warnMkvIfNeeded(this, imported);
        emit mediaImported(); // atualiza a lista de mídia e marca o projeto
    } else {
        return;
    }
    if (mediaIds.isEmpty()) return;

    m_dragProbeCache.clear();
    finishDrop(mediaIds, e->position().toPoint());
}

// ── Drag & Drop (arrasto manual da pool) ────────────────────────────────

void TimelineWidget::showDropHover(const QPoint& globalPos) {
    const QPoint pos = mapFromGlobal(globalPos);
    int row = -1;
    bool audio = false;
    m_dragHoverRow = rowFromY(pos.y(), row, audio) ? row : -1;
    m_dragHoverAudio = audio;
    // Arrasto manual da pool (sem dragMoveEvent): atualiza o instante e usa a
    // duração já conhecida (ou um padrão) para a prévia do clipe.
    m_dragHoverT = snapTime(std::max(0.0, xToTime(pos.x())));
    if (m_dragHoverDur <= 0.0) m_dragHoverDur = 1.0;
    update();
}

void TimelineWidget::hideDropHover() {
    m_dragHoverRow = -1;
    m_dragHoverDur = 0.0;
    m_dragHoverName.clear();
    update();
}

void TimelineWidget::dropMediaAt(const QStringList& mediaIds, const QPoint& globalPos) {
    finishDrop(mediaIds, mapFromGlobal(globalPos));
}

void TimelineWidget::finishDrop(const QStringList& mediaIds, const QPoint& dropPos) {
    if (!m_project || mediaIds.isEmpty()) return;

    // Trimmer (estilo Vegas): ao soltar UMA mídia de arquivo com vídeo, oferece
    // definir in/out antes de inserir. Cancelar aborta a soltura; multi-seleção
    // insere direto (sem diálogo). Ativável nas configurações (padrão: sem trimmer).
    double trimIn = 0.0;
    double trimDur = -1.0;
    if (mediaIds.size() == 1 && SettingsDialog::trimmerEnabled()) {
        const MediaItem* m = m_project->findMedia(mediaIds.first());
        if (m && m->hasVideo && !m->filePath.isEmpty() && m->duration > 0.5) {
            TrimmerDialog dlg(*m, this);
            if (dlg.exec() != QDialog::Accepted) return;
            trimIn = std::max(0.0, dlg.trimIn()); // in nunca negativo (head do arquivo)
            const double out = dlg.trimOut();
            trimDur = out - trimIn;
            if (trimDur <= 0.0) return;
        }
    }

    int row = -1;
    bool audio = false;
    if (!rowFromY(dropPos.y(), row, audio)) {
        // Solta fora de uma faixa (régua, rodapé ou coluna de cabeçalho):
        // usa a primeira faixa desbloqueada compatível com a mídia.
        bool anyVideo = false;
        for (const QString& mid : mediaIds) {
            const MediaItem* m = m_project->findMedia(mid);
            if (m && m->hasVideo) { anyVideo = true; break; }
        }
        audio = !anyVideo;
        row = 0;
    }
    m_dragHoverRow = -1;
    m_dragHoverDur = 0.0;
    m_dragHoverName.clear();

    emit editStart();
    double t = snapTime(std::max(0.0, xToTime(dropPos.x())));
    QString lastPlaced;

    // Acha a faixa (do tipo `audio`) que esteja livre para receber um clipe de
    // duração `dur` em `t`. Prefere `preferRow` (a faixa sob o mouse); se
    // ocupada, procura a primeira faixa desbloqueada que não sobreponha; se
    // nenhuma existir, cria uma nova faixa vazia.
    auto findFreeTrack = [this](bool audio, double t, double dur, int preferRow) {
        auto overlap = [t, dur](const QVector<Clip>& clips) {
            for (const Clip& o : clips)
                if (o.pos < t + dur - 1e-9 && o.pos + o.dur > t + 1e-9) return true;
            return false;
        };
        auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
        if (preferRow >= 0 && preferRow < (int)list.size()
            && !list[preferRow].locked && !overlap(list[preferRow].clips))
            return preferRow;
        for (int i = 0; i < (int)list.size(); ++i)
            if (!list[i].locked && !overlap(list[i].clips)) return i;
        m_project->addTrack(audio);
        return (int)list.size() - 1;
    };

    for (const QString& mid : mediaIds) {
        const MediaItem* m = m_project->findMedia(mid);
        if (!m) continue;
        const bool both = m->hasVideo && m->hasAudio;
        // Arquivos multicanal (OBS/câmera): um clipe de áudio POR stream, cada
        // um na sua faixa. O número de streams pode ser 0 mesmo com hasAudio.
        const int aStreams = m->hasAudio ? qMax(1, m->audioStreams) : 0;
        const double dur = trimDur > 0.0 ? trimDur : mediaInsertDur(*m);

        // Vídeo primeiro: decide a faixa de vídeo e a posição.
        int vRow = -1;
        double vDur = 0.0;
        if (m->hasVideo) {
            vRow = findFreeTrack(false, t, dur, audio ? -1 : row);
            vDur = dur;
        }

        if (vRow < 0 && aStreams == 0) continue;

        // Grupo: vídeo+áudio vinculados OU áudio multicanal (várias faixas do
        // mesmo arquivo) — movem e são excluídos juntos.
        const QString gid = (both || aStreams > 1) ? newId() : QString();
        if (vRow >= 0) {
            Clip c;
            c.id = newId();
            c.groupId = gid;
            c.mediaId = mid;
            c.pos = t;
            c.in = trimIn;
            c.dur = vDur;
            c.name = m->name;
            auto& clips = m_project->videoTracks[vRow].clips;
            auto it = clips.begin();
            while (it != clips.end() && it->pos <= c.pos) ++it;
            clips.insert(it, c);
            // Mídia solta diretamente na faixa de efeitos: aplicar a ela os
            // efeitos habilitados da faixa (o MainWindow trata o update).
            if (m_project->videoTracks[vRow].fxTrack)
                emit fxTrackClipAdded(m_project->videoTracks[vRow].id, c.id);
            lastPlaced = c.id;
        }
        for (int k = 0; k < aStreams; ++k) {
            const int aRow = findFreeTrack(true, t, dur, (k == 0 && audio) ? row : -1);
            if (aRow < 0) continue;
            Clip c;
            c.id = newId();
            c.groupId = gid;
            c.mediaId = mid;
            c.audioStreamIndex = k;
            c.pos = t;
            c.in = trimIn;
            c.dur = dur;
            c.name = aStreams > 1 ? QString("%1 (faixa %2)").arg(m->name).arg(k + 1)
                                  : m->name;
            auto& clips = m_project->audioTracks[aRow].clips;
            auto it = clips.begin();
            while (it != clips.end() && it->pos <= c.pos) ++it;
            clips.insert(it, c);
            if (lastPlaced.isEmpty()) lastPlaced = c.id;
        }
        t += dur;
    }
    if (!lastPlaced.isEmpty()) setSelection(lastPlaced);
    updateScrollRanges();
    update();
    emit modified();
}


