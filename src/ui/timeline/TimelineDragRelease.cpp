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

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e) {
    // Ctrl+press num clipe sem arrasto: vira clique = alterna a seleção
    // (Ctrl+clique normal). Se arrastou, o m_ctrlPending já virou
    // duplicação no mouseMove e aqui não chega.
    if (m_ctrlPending) {
        m_ctrlPending = false;
        Clip* c = findClipById(m_ctrlClipId);
        if (c) toggleSelection(c->id);
        update();
        return;
    }

    switch (m_dragMode) {
    case PlayheadDrag:
        // Fim do arrasto da agulha: garante que o preview receba a posição
        // EXATA (o throttle pode ter engolido o último pedido).
        flushPlayheadNow();
        break;
    case RulerLoop:
    case RulerLoopEdge:
        m_loopEdge = LoopEdgeNone;
        setCursor(Qt::ArrowCursor);
        break;
    case Razor:
        razorSplitAt(m_razorT);
        break;
    case Marquee:
        if (m_marqueeRect.width() < 4 && m_marqueeRect.height() < 4) {
            // Clique simples no vazio: desfaz a seleção (Ctrl mantém).
            if (!(e->modifiers() & Qt::ControlModifier)) {
                m_selected.clear();
                m_secondarySelected.clear();
                refreshView();
                emit selectionChanged(QString());
            }
        } else {
            selectInMarquee(e->modifiers() & Qt::ControlModifier);
            m_envPending = false; // marquee real: não é clique na linha
        }
        m_marqueeRect = QRect();
        break;
    case ZoomSelect:
        if (std::fabs(m_zoomT1 - m_zoomT0) < 0.02)
            zoomBy(1.6, m_zoomT0);
        else
            applyZoomRect(m_zoomT0, m_zoomT1);
        break;
    case TrackDrag:
        finishTrackDrag();
        break;
    case TrackEnvVol:
        // Clique simples (sem arraste) sobre um diamante remove o keyframe.
        if (m_envRow >= 0 && m_envKf >= 0 && m_envAudio
            && (e->pos() - m_dragStart).manhattanLength() < 4) {
            Track& t = m_project->audioTracks[m_envRow];
            if (m_envKf < t.kfVolume.size()) {
                t.kfVolume.removeAt(m_envKf);
                update();
                emit modified();
            }
        }
        break;
    default:
        break;
    }

    // Clique simples na linha do envelope de volume de uma faixa (sem
    // arraste): alterna um ponto naquele instante (estilo Vegas). Um arraste
    // vertical já teria convertido para TrackVol no mouseMove.
    if (m_envPending) {
        m_envPending = false;
        if (m_volRow >= 0 && m_volRow < (int)m_project->audioTracks.size()) {
            const double t = std::max(0.0, snapTime(xToTime(e->pos().x())));
            trackEnvelopePress(m_volRow, true, t);
        }
    }

    m_dragMode = None;
    m_dragClip.clear();
    m_dragUndoPushed = false;
    // Se algum mouseMove emitiu clipGeometryChanged, agora — no fim do gesto —
    // dispara o modified() completo UMA vez (painéis/undo dão refresh sério).
    if (m_geometryChangedDuringDrag) {
        m_geometryChangedDuringDrag = false;
        emit modified();
    }
    m_dragOrig.clear();
    m_rippleOrig.clear();
    m_rippleMove = false;
    m_rippleMoveOrig.clear();
    m_volRow = -1;
    m_volClip.clear();
    m_snapLineX = -1.0; // limpa guia de alinhamento
    m_trimEdgeX = -1.0; // limpa indicador de trim
    m_trimEdgeRow = -1;
    m_volPending = false;
    m_envPending = false;
    m_envRow = -1;
    m_envKf = -1;
    m_envAudio = false;
    m_ctrlPending = false;
    m_ctrlClipId.clear();
    m_ctrlCanMove = false;
    m_trackDragActive = false;
    m_dragTrackRow = -1;
    m_dragGroupId.clear();
    m_dropRow = -1;
    m_dropGroup.clear();
    stopAutoScroll();
    setCursor(Qt::ArrowCursor);
    // Ressincroniza o range depois do arraste: durante o gesto só a barra
    // horizontal era alargada, então um encurtamento fica defasado até aqui.
    updateScrollRanges();
    update();
    // Rede de segurança: se o gesto terminou por outro caminho (o switch acima
    // já flushou no caso PlayheadDrag), o timer do throttle não fica armado
    // para o próximo gesto.
    if (m_playheadThrottleArmed) flushPlayheadNow();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_project) return;
    // Duplo clique na faixa de uma pasta (fora da seta) renomeia a pasta.
    if (e->pos().x() < kHeaderW) {
        QString gid;
        if (folderStripAt(e->pos().y(), gid)) {
            TrackGroup* g = m_project->findGroup(gid);
            const QRect ar = g ? folderArrowRect(*g) : QRect();
            if (g && !(e->pos().x() >= ar.left() && e->pos().x() <= ar.right())) {
                renameTrackGroup(gid);
                return;
            }
        }
    }
    // Duplo clique na agulha, ou na região do loop (na régua), limpa o loop.
    if (m_loopOut > m_loopIn) {
        const double px = timeToX(m_playhead);
        const double t = xToTime(e->pos().x());
        if (std::fabs(e->pos().x() - px) <= 6
            || (e->pos().y() < kRulerH && t >= m_loopIn && t <= m_loopOut)) {
            clearLoop();
            return;
        }
    }
    int row;
    bool audio;
    // Duplo clique no nome da faixa (linha inferior do cabeçalho): renomeia
    // inline, estilo Premiere (editor sobre a faixa, sem diálogo modal).
    if (e->pos().x() < kHeaderW && e->pos().y() >= kRulerH
        && rowFromY(e->pos().y(), row, audio)) {
        const int y = audio ? rowY(-1, row) : rowY(row, -1);
        const Track& tr = (audio ? m_project->audioTracks : m_project->videoTracks)[row];
        // Não renomeia ao clicar nos controles do cabeçalho (seta, keyframes,
        // sync, olho/falante, M/S/R, cadeado).
        int btnRow;
        bool btnAudio;
        if (headerBtnAt(e->pos(), btnRow, btnAudio) >= 0) return;
        const bool mini = tr.collapsed || trackH(row, audio) - kResizeHandleH < 22;
        if (mini) {
            if (e->pos().y() >= y + 1
                && e->pos().y() <= y + kResizeHandleH + kHeaderNameH + 2) {
                beginTrackRename(row, audio);
                return;
            }
        } else if (e->pos().y() >= y + 20 && e->pos().y() <= y + kHeaderNameH + 22) {
            beginTrackRename(row, audio);
            return;
        }
    }
    if (rowFromY(e->pos().y(), row, audio)) {
        Clip* clip = clipAt(row, audio, xToTime(e->pos().x()));
        if (clip) {
            // Duplo clique num clipe com texto (ou num clipe de texto) abre a
            // mesma janela usada para criar — vira a janela de edição do clipe.
            if (!audio && (clip->isText || !m_project->textStyleFor(*clip)->isEmpty())) {
                showTextEditorDialog(clip);
                return;
            }
            toggleSelection(clip->id);
            update();
        }
    }
}


