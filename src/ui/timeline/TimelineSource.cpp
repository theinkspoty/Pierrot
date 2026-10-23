// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/timeline/TimelineWidget.h"
#include "ui/timeline/TimelineCommands.h"
#include "colombina/util.h"
#include "colombina/models/ClipAttrs.h"

#include "colombina/ffmpeg/MediaCache.h"
#include "laartman/Decoder.h"
#include "colombina/mesh/BlenderBridge.h"
#include "colombina/render/MesaRenderer.h"
#include "ui/dialogs/TransformDialog.h"
#include "ui/audio/AudioEffectsDialog.h"
#include "ui/audio/TrackAudioFxDialog.h"
#include "ui/dialogs/TextEditorDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/timeline/TlLog.h"
#include "ui/core/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QCheckBox>
#include <QCursor>
#include <QList>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QSettings>
#include <QKeySequence>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QScrollBar>
#include <QMenu>
#include <QKeyEvent>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPolygon>
#include <QDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QColorDialog>
#include <QIcon>
#include <QPixmap>
#include <QTabWidget>
#include <QGroupBox>
#include <QListWidget>
#include <QFileDialog>
#include <QActionGroup>
#include <QPair>
#include <QVariantAnimation>
#include <QStyle>
#include <QStyleOptionRubberBand>
#include <QRubberBand>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
// Duração padrão ao inserir uma imagem na timeline (imagens não têm duração
// própria; sem isto entrariam como um clipe de ~1s, estreito demais).
constexpr double kDefaultImageDur = 3.0;

// true para GIF/WebP ANIMADO (múltiplos quadros). Diferente de imagem estática,
// esses têm duração real e devem ser inseridos com ela (o decoder loopa o
// arquivo para preencher um clipe mais longo).
bool isAnimatedMedia(const QString& path) {
    return Decoder::isAnimatedImage(path);
}




// Duração efetiva ao inserir uma mídia na timeline: imagens estáticas usam o
// padrão próprio; GIF/WebP animado, vídeo e áudio usam a duração real (ou 1s
// como fallback).
double mediaInsertDur(const MediaItem& m) {
    if (isImageFile(m.filePath) && !isAnimatedMedia(m.filePath))
        return kDefaultImageDur;
    return m.duration > 0 ? m.duration : 1.0;
}
}

// Adiciona uma mídia do pool no playhead (fallback para o arrastar/soltar):
// vídeo na primeira faixa de vídeo desbloqueada, com áudio vinculado quando a
// mídia tiver ambas as trilhas.
void TimelineWidget::addMediaAtPlayhead(const QString& mediaId) {
    if (!m_project) return;
    const MediaItem* m = m_project->findMedia(mediaId);
    if (!m) return;
    qDebug() << "[TimelineWidget::addMediaAtPlayhead]" << m->name
             << "hasVideo=" << m->hasVideo << "hasAudio=" << m->hasAudio
             << "dur=" << m->duration;
    emit editStart();
    const double t = snapTime(std::max(0.0, m_playhead));
    const double dur = mediaInsertDur(*m);

    QString lastPlaced;
    const auto push = [&](QVector<Clip>& clips, const Clip& c) {
        auto it = clips.begin();
        while (it != clips.end() && it->pos <= c.pos) ++it;
        clips.insert(it, c);
    };
    // Arquivos multicanal (OBS/câmera): um clipe de áudio POR stream, cada um
    // na sua faixa. O número de streams pode ser 0 mesmo com hasAudio.
    const int aStreams = m->hasAudio ? qMax(1, m->audioStreams) : 0;
    const bool multi = aStreams > 1;
    if (m->hasVideo) {
        const int vRow = findFreeTrackFor(false, t, dur, 0);
        Clip c;
        c.id = newId();
        c.groupId = aStreams > 0 ? newId() : QString();
        c.mediaId = mediaId;
        c.pos = t;
        c.in = 0.0;
        c.dur = dur;
        c.name = m->name;
        push(m_project->videoTracks[vRow].clips, c);
        lastPlaced = c.id;
        for (int k = 0; k < aStreams; ++k) {
            const int aRow = findFreeTrackFor(true, t, dur, 0);
            Clip ac = c;
            ac.id = newId();
            ac.audioStreamIndex = k;
            if (multi) ac.name = QString("%1 (faixa %2)").arg(m->name).arg(k + 1);
            push(m_project->audioTracks[aRow].clips, ac);
        }
    } else if (m->hasAudio) {
        const QString gid = multi ? newId() : QString(); // streams juntos no Delete/mover
        for (int k = 0; k < aStreams; ++k) {
            const int aRow = findFreeTrackFor(true, t, dur, 0);
            Clip c;
            c.id = newId();
            c.mediaId = mediaId;
            c.groupId = gid;
            c.audioStreamIndex = k;
            c.pos = t;
            c.in = 0.0;
            c.dur = dur;
            c.name = multi ? QString("%1 (faixa %2)").arg(m->name).arg(k + 1)
                           : m->name;
            push(m_project->audioTracks[aRow].clips, c);
            lastPlaced = c.id;
        }
    }
    if (!lastPlaced.isEmpty()) setSelection(lastPlaced);
    updateScrollRanges();
    update();
    emit modified();
}

// ── Insert / Overwrite do Source Monitor (Premiere) ──────────────────────
// Alvo: faixa de vídeo selecionada (ou a primeira desbloqueada) e, se a
// mídia tiver áudio, a faixa de áudio selecionada (ou a primeira livre).
// Insert: divide o que cruza o playhead, desloca tudo a partir dele e coloca
// o trecho do Source. Overwrite: recorta/clipes sobrepostos no intervalo e
// coloca o trecho sem deslocar o resto.

namespace {
struct SourceTarget {
    int vRow = -1;
    int aRow = -1;
};
} // namespace

void TimelineWidget::insertSourceAtPlayhead(const QString& mediaId, double srcIn, double srcOut) {
    if (!m_project) return;
    const MediaItem* m = m_project->findMedia(mediaId);
    if (!m) return;
    srcIn = std::max(0.0, srcIn); // fonte nunca antes do início do arquivo
    const double t = snapTime(std::max(0.0, m_playhead));
    double dur = srcOut - srcIn;
    if (dur <= kMinDur) {
        if (isImageFile(m->filePath) && !isAnimatedMedia(m->filePath))
            dur = kDefaultImageDur;
        else dur = m->duration > 0 ? m->duration : 1.0;
        srcIn = 0.0;
    }
    if (dur <= 0.0) return;

    emit undoLabel(tr("Inserir do Source"));
    emit editStart();

    SourceTarget tgt;
    for (const TrackSel& s : m_selTracks) {
        if (s.rec) continue;
        if (!s.audio && tgt.vRow < 0 && s.row >= 0
            && s.row < (int)m_project->videoTracks.size()
            && !m_project->videoTracks[s.row].locked)
            tgt.vRow = s.row;
        if (s.audio && tgt.aRow < 0 && s.row >= 0
            && s.row < (int)m_project->audioTracks.size()
            && !m_project->audioTracks[s.row].locked)
            tgt.aRow = s.row;
    }
    if (tgt.vRow < 0) {
        for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
            if (!m_project->videoTracks[i].locked) { tgt.vRow = i; break; }
    }
    if (tgt.vRow < 0 && m->hasVideo) {
        m_project->addTrack(false);
        tgt.vRow = (int)m_project->videoTracks.size() - 1;
    }
    if (tgt.aRow < 0) {
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
            if (!m_project->audioTracks[i].locked) { tgt.aRow = i; break; }
    }

    // 1) Divide o que cruza `t` em todas as faixas desbloqueadas.
    // Coleta IDs antes: splitClipAt insere no vetor e invalidaria referências.
    auto splitStraddling = [&](QVector<Track>& tracks) {
        for (Track& tr : tracks) {
            if (tr.locked) continue;
            QStringList ids;
            for (const Clip& c : tr.clips)
                if (c.pos < t - 1e-6 && c.pos + c.dur > t + 1e-6)
                    ids.append(c.id);
            for (const QString& id : ids) {
                Clip* c = findClipById(id);
                if (c) splitClipAt(c, t, nullptr);
            }
        }
    };
    splitStraddling(m_project->videoTracks);
    splitStraddling(m_project->audioTracks);

    // 2) Desloca tudo com pos >= t (insert ripple). Coleta ids antes: o push
    // posterior pode realocar os vetores de clipes.
    auto shiftFrom = [t, dur](QVector<Track>& tracks) {
        for (Track& tr : tracks) {
            if (tr.locked) continue;
            QStringList ids;
            for (const Clip& c : tr.clips)
                if (c.pos >= t - 1e-9) ids.append(c.id);
            for (const QString& id : ids) {
                for (Clip& c : tr.clips)
                    if (c.id == id) { c.pos += dur; break; }
            }
        }
    };
    shiftFrom(m_project->videoTracks);
    shiftFrom(m_project->audioTracks);

    // 3) Coloca o trecho do Source.
    const bool both = m->hasVideo && m->hasAudio;
    const int aStreams = m->hasAudio ? qMax(1, m->audioStreams) : 0;
    const QString gid = (both || aStreams > 1) ? newId() : QString();
    auto push = [](QVector<Clip>& clips, const Clip& c) {
        auto it = clips.begin();
        while (it != clips.end() && it->pos <= c.pos) ++it;
        clips.insert(it, c);
    };
    QString lastPlaced;
    if (m->hasVideo && tgt.vRow >= 0) {
        Clip c;
        c.id = newId();
        c.groupId = aStreams > 0 ? gid : QString();
        c.mediaId = mediaId;
        c.pos = t;
        c.in = srcIn;
        c.dur = dur;
        c.name = m->name;
        push(m_project->videoTracks[tgt.vRow].clips, c);
        lastPlaced = c.id;
        for (int k = 0; k < aStreams; ++k) {
            int aRow = (k == 0) ? tgt.aRow : -1;
            if (aRow < 0) {
                for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
                    if (!m_project->audioTracks[i].locked) { aRow = i; break; }
            }
            if (aRow < 0) {
                m_project->addTrack(true);
                aRow = (int)m_project->audioTracks.size() - 1;
            }
            Clip ac = c;
            ac.id = newId();
            ac.audioStreamIndex = k;
            if (aStreams > 1) ac.name = QString("%1 (faixa %2)").arg(m->name).arg(k + 1);
            push(m_project->audioTracks[aRow].clips, ac);
            lastPlaced = ac.id;
        }
    } else if (m->hasAudio) {
        const QString g = aStreams > 1 ? gid : QString();
        for (int k = 0; k < aStreams; ++k) {
            int aRow = (k == 0) ? tgt.aRow : -1;
            if (aRow < 0) {
                for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
                    if (!m_project->audioTracks[i].locked) { aRow = i; break; }
            }
            if (aRow < 0) {
                m_project->addTrack(true);
                aRow = (int)m_project->audioTracks.size() - 1;
            }
            Clip c;
            c.id = newId();
            c.groupId = g;
            c.mediaId = mediaId;
            c.audioStreamIndex = k;
            c.pos = t;
            c.in = srcIn;
            c.dur = dur;
            c.name = aStreams > 1 ? QString("%1 (faixa %2)").arg(m->name).arg(k + 1)
                                  : m->name;
            push(m_project->audioTracks[aRow].clips, c);
            lastPlaced = c.id;
        }
    }

    if (!lastPlaced.isEmpty()) setSelection(lastPlaced);
    ensurePlayheadVisible();
    updateScrollRanges();
    invalidateScene();
    update();
    emit modified();
}

void TimelineWidget::overwriteSourceAtPlayhead(const QString& mediaId, double srcIn, double srcOut) {
    if (!m_project) return;
    const MediaItem* m = m_project->findMedia(mediaId);
    if (!m) return;
    srcIn = std::max(0.0, srcIn); // fonte nunca antes do início do arquivo
    const double t = snapTime(std::max(0.0, m_playhead));
    double dur = srcOut - srcIn;
    if (dur <= kMinDur) {
        if (isImageFile(m->filePath) && !isAnimatedMedia(m->filePath))
            dur = kDefaultImageDur;
        else dur = m->duration > 0 ? m->duration : 1.0;
        srcIn = 0.0;
    }
    if (dur <= 0.0) return;

    emit undoLabel(tr("Overwrite do Source"));
    emit editStart();

    SourceTarget tgt;
    for (const TrackSel& s : m_selTracks) {
        if (s.rec) continue;
        if (!s.audio && tgt.vRow < 0 && s.row >= 0
            && s.row < (int)m_project->videoTracks.size()
            && !m_project->videoTracks[s.row].locked)
            tgt.vRow = s.row;
        if (s.audio && tgt.aRow < 0 && s.row >= 0
            && s.row < (int)m_project->audioTracks.size()
            && !m_project->audioTracks[s.row].locked)
            tgt.aRow = s.row;
    }
    if (tgt.vRow < 0) {
        for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
            if (!m_project->videoTracks[i].locked) { tgt.vRow = i; break; }
    }
    if (tgt.vRow < 0 && m->hasVideo) {
        m_project->addTrack(false);
        tgt.vRow = (int)m_project->videoTracks.size() - 1;
    }
    if (tgt.aRow < 0) {
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
            if (!m_project->audioTracks[i].locked) { tgt.aRow = i; break; }
    }

    // Recorta/clipes no intervalo [t, t+dur) da faixa alvo, sem deslocar o resto.
    auto carveRange = [t, dur](QVector<Clip>& clips) {
        QVector<Clip> keep;
        for (Clip c : clips) {
            const double cEnd = c.pos + c.dur;
            if (cEnd <= t + 1e-9 || c.pos >= t + dur - 1e-9) {
                keep.append(c);
                continue;
            }
            if (c.pos < t - 1e-9 && cEnd > t + dur + 1e-9) {
                Clip left = c;
                left.dur = t - c.pos;
                Clip right = c;
                right.id = newId();
                right.pos = t + dur;
                right.in = c.in + (t + dur - c.pos);
                right.dur = cEnd - (t + dur);
                keep.append(left);
                keep.append(right);
            } else if (c.pos < t - 1e-9) {
                Clip left = c;
                left.dur = t - c.pos;
                keep.append(left);
            } else if (cEnd > t + dur + 1e-9) {
                Clip right = c;
                right.pos = t + dur;
                right.in = c.in + (t + dur - c.pos);
                right.dur = cEnd - (t + dur);
                keep.append(right);
            }
            // Fully inside the overwrite range: drop.
        }
        clips = keep;
    };
    if (tgt.vRow >= 0 && tgt.vRow < (int)m_project->videoTracks.size())
        carveRange(m_project->videoTracks[tgt.vRow].clips);
    if (tgt.aRow >= 0 && tgt.aRow < (int)m_project->audioTracks.size())
        carveRange(m_project->audioTracks[tgt.aRow].clips);

    const bool both = m->hasVideo && m->hasAudio;
    const int aStreams = m->hasAudio ? qMax(1, m->audioStreams) : 0;
    const QString gid = (both || aStreams > 1) ? newId() : QString();
    auto push = [](QVector<Clip>& clips, const Clip& c) {
        auto it = clips.begin();
        while (it != clips.end() && it->pos <= c.pos) ++it;
        clips.insert(it, c);
    };
    QString lastPlaced;
    if (m->hasVideo && tgt.vRow >= 0) {
        Clip c;
        c.id = newId();
        c.groupId = aStreams > 0 ? gid : QString();
        c.mediaId = mediaId;
        c.pos = t;
        c.in = srcIn;
        c.dur = dur;
        c.name = m->name;
        push(m_project->videoTracks[tgt.vRow].clips, c);
        lastPlaced = c.id;
        for (int k = 0; k < aStreams; ++k) {
            int aRow = (k == 0) ? tgt.aRow : -1;
            if (aRow < 0) {
                for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
                    if (!m_project->audioTracks[i].locked) { aRow = i; break; }
            }
            if (aRow < 0) {
                m_project->addTrack(true);
                aRow = (int)m_project->audioTracks.size() - 1;
            }
            if (k > 0 || aRow != tgt.aRow) {
                if (aRow >= 0 && aRow < (int)m_project->audioTracks.size())
                    carveRange(m_project->audioTracks[aRow].clips);
            }
            Clip ac = c;
            ac.id = newId();
            ac.audioStreamIndex = k;
            if (aStreams > 1) ac.name = QString("%1 (faixa %2)").arg(m->name).arg(k + 1);
            push(m_project->audioTracks[aRow].clips, ac);
            lastPlaced = ac.id;
        }
    } else if (m->hasAudio) {
        const QString g = aStreams > 1 ? gid : QString();
        for (int k = 0; k < aStreams; ++k) {
            int aRow = (k == 0) ? tgt.aRow : -1;
            if (aRow < 0) {
                for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
                    if (!m_project->audioTracks[i].locked) { aRow = i; break; }
            }
            if (aRow < 0) {
                m_project->addTrack(true);
                aRow = (int)m_project->audioTracks.size() - 1;
            }
            if (k > 0) {
                if (aRow >= 0 && aRow < (int)m_project->audioTracks.size())
                    carveRange(m_project->audioTracks[aRow].clips);
            }
            Clip c;
            c.id = newId();
            c.groupId = g;
            c.mediaId = mediaId;
            c.audioStreamIndex = k;
            c.pos = t;
            c.in = srcIn;
            c.dur = dur;
            c.name = aStreams > 1 ? QString("%1 (faixa %2)").arg(m->name).arg(k + 1)
                                  : m->name;
            push(m_project->audioTracks[aRow].clips, c);
            lastPlaced = c.id;
        }
    }

    if (!lastPlaced.isEmpty()) setSelection(lastPlaced);
    updateScrollRanges();
    invalidateScene();
    update();
    emit modified();
}
