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


// ── Autoscroll ──────────────────────────────────────────────────────────

// Liga o autoscroll quando o mouse (durante PlayheadDrag, RulerLoop ou o
// arraste de um clipe) está perto da borda esquerda/direita da view. A rolagem
// contínua faz a agulha/clipe acompanhar o cursor sem que ele saia da janela.
void TimelineWidget::startAutoScroll(QMouseEvent* e) {
    if (!m_project) return;
    m_autoScrollMouse = e->pos();
    const int leftEdge = 32;
    const int rightEdge = 16; // lado direito menor (menos área de "varredura")
    const int vw = width() - m_vbar->sizeHint().width();
    const int mx = m_autoScrollMouse.x();
    int dir = 0;
    if (mx >= kHeaderW && mx <= kHeaderW + leftEdge)
        dir = -1;
    else if (mx >= vw - rightEdge)
        dir = +1;
    if (dir != 0) {
        m_autoScrollDir = dir;
        if (!m_autoScroll->isActive()) m_autoScroll->start();
    } else {
        stopAutoScroll();
    }
}

void TimelineWidget::stopAutoScroll() {
    m_autoScroll->stop();
    m_autoScrollDir = 0;
}

// Registra onde desenhar o indicador de trim: a borda do clipe sob arraste,
// na faixa onde ela está. `ripple` escolhe a cor (amarelo no Premiere para
// ripple, vermelho para trim regular e roll).
void TimelineWidget::markTrimEdge(double tEdge, bool ripple) {
    m_trimEdgeX = timeToX(tEdge);
    m_trimEdgeRipple = ripple;
    m_trimEdgeRow = -1;
    m_trimEdgeAudio = false;
    if (m_dragClip.isEmpty()) return;
    int row = -1;
    bool audio = false;
    if (clipTrackIndex(m_dragClip, row, audio)) {
        m_trimEdgeRow = row;
        m_trimEdgeAudio = audio;
    }
}

// Rola a timeline e, quando aplicável, reposiciona a agulha/loop/arrasto sob o
// cursor para que a operação continue na nova área visível.
void TimelineWidget::autoScrollTick() {
    if (m_dragMode != PlayheadDrag && m_dragMode != RulerLoop
        && m_dragMode != RulerLoopEdge
        && m_dragMode != MoveClip && m_dragMode != TrimLeft
        && m_dragMode != TrimRight) {
        stopAutoScroll();
        return;
    }
    const int viewW = width() - m_vbar->sizeHint().width();
    // Passo sutil: ~1/60 da largura por tick (16ms) → ~1 tela/s, em vez de
    // ~5 telas/s do viewW/24 que "arrastava" a timeline agressivamente.
    const int step = qMax(4, viewW / 60) * m_autoScrollDir;
    int v = m_hbar->value() + step;
    if (v < m_hbar->minimum()) v = m_hbar->minimum();
    if (v > m_hbar->maximum()) v = m_hbar->maximum();
    m_hbar->setValue(v);

    // Recalcula a operação na nova posição da view.
    const double autoT = std::max(0.0, snapTime(xToTime(m_autoScrollMouse.x())));
    if (m_dragMode == RulerLoopEdge) {
        if (m_loopEdge == LoopEdgeIn)
            m_loopIn = std::min(autoT, m_loopEdgeOther - 0.02);
        else
            m_loopOut = std::max(autoT, m_loopEdgeOther + 0.02);
        emit loopChanged(m_loopIn, m_loopOut);
        update();
        return;
    }
    if (m_dragMode == PlayheadDrag || m_dragMode == RulerLoop) {
        const double t2 = std::max(0.0, snapTime(xToTime(m_autoScrollMouse.x())));
        setPlayhead(t2);
        emit playheadChanged(t2);
        update();
        return;
    }
    // MoveClip/TrimLeft/TrimRight: reposiciona a operação sob o cursor.
    QMouseEvent ev(QEvent::MouseMove, m_autoScrollMouse,
                   mapToGlobal(m_autoScrollMouse), Qt::LeftButton,
                   Qt::LeftButton, Qt::NoModifier);
    mouseMoveEvent(&ev);
}

// ── Hit-test de cabeçalho e faixas ──────────────────────────────────────

// Retorna o índice da alça de redimensionamento na qual o cursor está (0) ou
// -1 se fora. Só vale na coluna de cabeçalho (x < kHeaderW).
int TimelineWidget::resizeHandleAt(const QPoint& pos, int& row, bool& audio) const {
    if (pos.x() >= kHeaderW || pos.y() < kRulerH || !m_project) return -1;
    if (!rowFromY(pos.y(), row, audio)) return -1;
    const int y = audio ? rowY(-1, row) : rowY(row, -1);
    const int rowH = trackH(row, audio);
    if (pos.y() >= y + rowH - kResizeHandleH && pos.y() < y + rowH)
        return 0;
    return -1;
}

// Se o ponto está sobre a linha de volume de um clipe de áudio, retorna o
// clipe (dentro de uma tolerância vertical e do intervalo horizontal dele).
Clip* TimelineWidget::clipVolAt(const QPoint& pos, int& row) const {
    if (!m_project || pos.x() < kHeaderW || pos.y() < kRulerH) return nullptr;
    int r = -1;
    bool audio = false;
    if (!rowFromY(pos.y(), r, audio) || !audio) return nullptr;
    const double t = xToTime(pos.x());
    Clip* c = clipAt(r, true, t);
    if (!c) return nullptr;
    const int ly = clipVolLineY(r, *c);
    if (std::abs(pos.y() - ly) <= 4) { row = r; return c; }
    return nullptr;
}

// Se o ponto está sobre uma faixa de áudio (na área de conteúdo, fora de um
// clipe), retorna o índice da faixa para o ajuste de volume. Toda a altura da
// faixa é aceita: segurar sobre a faixa e arrastar para cima/baixo ajusta o
// volume (estilo Vegas), sem precisar acertar a linha. -1 se fora.
int TimelineWidget::volRowAt(const QPoint& pos, int& row) const {
    if (!m_project || pos.x() < kHeaderW || pos.y() < kRulerH) return -1;
    int r = -1;
    bool audio = false;
    if (!rowFromY(pos.y(), r, audio) || !audio) return -1;
    if (pos.y() < rowY(-1, r) || pos.y() >= rowY(-1, r) + trackH(r, true)) return -1;
    row = r;
    return 0;
}

// Botões do cabeçalho (ordem da linha superior do Premiere):
// 0=recolher/expandir, 1=keyframe anterior, 2=adicionar/remover keyframe,
// 3=próximo keyframe, 4=sync lock (sem backend), 5=toggle de saída
// (olho/falante), 6=M, 7=S, 8=R (áudio; sem backend); 9=cadeado (linha do nome).
// -1 = nenhum botão (nome, VU meter, corpo: seleção/renomeio normal).
int TimelineWidget::headerBtnAt(const QPoint& pos, int& row, bool& audio) const {
    if (pos.x() >= kHeaderW || pos.y() < kRulerH || !m_project) return -1;
    if (!rowFromY(pos.y(), row, audio)) return -1;
    const int rowH = trackH(row, audio);
    const int y = audio ? rowY(-1, row) : rowY(row, -1);
    const QVector<Track>& list = audio ? m_project->audioTracks : m_project->videoTracks;
    if (row < 0 || row >= list.size()) return -1;
    const Track& tr = list[row];

    const bool mini = tr.collapsed || rowH - kResizeHandleH < 22;
    if (mini) {
        if (headerBtnRect(y, 0).contains(pos)) return 0;
        if (headerMiniToggleRect(y).contains(pos)) return 5;
        if (headerMiniLockRect(y).contains(pos)) return 9;
        return -1;
    }

    // Linha superior (só os slots que existem para o tipo de faixa).
    for (int slot = 0; slot <= 4; ++slot)
        if (headerBtnRect(y, slot).contains(pos)) return slot;
    if (headerBtnRect(y, 5).contains(pos)) return 5;
    if (audio) {
        for (int slot = 6; slot <= 8; ++slot)
            if (headerBtnRect(y, slot).contains(pos)) return slot;
    }
    // Cadeado na linha do nome.
    if (headerLockRect(y).contains(pos)) return 9;
    return -1;
}

// ── Reordenação de faixas (arrasto de header) ──────────────────────────

void TimelineWidget::finishTrackDrag() {
    if (!m_project) return;
    // Arrasto de GRUPO (pela faixa de pasta): move todas as faixas do grupo.
    if (!m_dragGroupId.isEmpty()) {
        TrackGroup* g = m_project->findGroup(m_dragGroupId);
        if (g && m_dropRow >= 0) {
            QVector<int> vidx, aidx;
            for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
                if (m_project->videoTracks[i].groupId == g->id) vidx.append(i);
            for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
                if (m_project->audioTracks[i].groupId == g->id) aidx.append(i);
            emit editStart();
            if (m_dropAudio) moveTracksTo(true, aidx, m_dropRow);
            else moveTracksTo(false, vidx, m_dropRow);
            pruneEmptyGroups();
            updateScrollRanges();
            invalidateScene();
            emit modified();
        }
        m_dragGroupId.clear();
        m_dragTrackRow = -1;
        m_dropRow = -1;
        m_dropGroup.clear();
        return;
    }

    if (m_dragTrackRow < 0) return;
    const int from = m_dragTrackRow;
    const bool audio = m_dragTrackAudio;

    // Faixa de gravação solta na seção de áudio: promove. Ela vira uma faixa
    // de áudio comum, mantendo id, nome, clipes e envelopes, e some da seção
    // vermelha. Solta em outro lugar (ou em pasta) não promove.
    if (m_dragTrackRec) {
        if (m_dropRow >= 0 && m_dropAudio) {
            emit editStart();
            m_project->promoteRecordingTrack(from, m_dropRow);
            clearTrackSelection();
            updateScrollRanges();
            invalidateScene();
            refreshView();
            emit modified();
        } else {
            clearTrackSelection();
            refreshView();
        }
        m_dragTrackRow = -1;
        m_dragTrackRec = false;
        return;
    }

    if (!m_dropGroup.isEmpty()) {
        // Soltou sobre uma pasta: entra no grupo.
        if (m_project->findGroup(m_dropGroup)) {
            emit editStart();
            auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
            if (from >= 0 && from < (int)list.size())
                list[from].groupId = m_dropGroup;
            pruneEmptyGroups();
            updateScrollRanges();
            invalidateScene();
            emit modified();
        }
        m_dropGroup.clear();
        m_dragTrackRow = -1;
        return;
    }
    // Soltou sobre uma faixa do mesmo tipo: reordena. Uma faixa de grupo, ao
    // ser movida para outro lugar, sai do grupo (fica por cima/abaixo da
    // hierarquia de pastas).
    if (m_dropRow >= 0 && m_dropAudio == audio && m_dropRow != from) {
        emit editStart();
        auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
        if (from >= 0 && from < (int)list.size())
            list[from].groupId.clear();
        moveTrack(audio, from, m_dropRow);
        pruneEmptyGroups();
        updateScrollRanges();
        invalidateScene();
        emit modified();
    }
    m_dragTrackRow = -1;
}

// ── Snap e posicionamento ───────────────────────────────────────────────

double TimelineWidget::snapToEdges(double t, const QString& excludeId) const {
    if (excludeId.isEmpty()) return snapToEdges(t, QSet<QString>());
    QSet<QString> ids;
    ids.insert(excludeId);
    return snapToEdges(t, ids);
}

double TimelineWidget::snapToEdges(double t, const QSet<QString>& excludeIds) const {
    if (!m_snap) return t;
    const double thresh = 8.0 / m_pps;
    double best = t;
    double bestD = thresh;
    auto consider = [&](double x) {
        const double d = std::fabs(x - t);
        if (d < bestD) { bestD = d; best = x; }
    };
    consider(0.0);
    consider(m_playhead);
    auto considerClip = [&](const Clip& c) {
        if (excludeIds.contains(c.id)) return;
        consider(c.pos);
        consider(c.pos + c.dur);
    };
    if (m_project) {
        for (const Track& tr : m_project->videoTracks)
            for (const Clip& c : tr.clips)
                considerClip(c);
        for (const Track& tr : m_project->audioTracks)
            for (const Clip& c : tr.clips)
                considerClip(c);
        // Premiere alinha também com marcadores, não só com bordas de clipe e
        // agulha (ver "Snap in Timeline").
        for (const Marker& mk : m_project->markers)
            consider(mk.time);
    }
    return best;
}

double TimelineWidget::clampPosToTrack(Clip* c, double newPos,
                                       const QSet<QString>& moving) const {
    if (!m_project || !c) return newPos;
    Q_UNUSED(moving)
    // Clipes podem se sobrepor na mesma faixa: a sobreposição vira uma
    // transição (estilo Vegas). Só garante que não vá para o negativo.
    return std::max(0.0, newPos);
}

bool TimelineWidget::clipTrackIndex(const QString& id, int& row, bool& audio) const {
    if (!m_project) return false;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        for (const Clip& c : m_project->videoTracks[i].clips)
            if (c.id == id) { row = i; audio = false; return true; }
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        for (const Clip& c : m_project->audioTracks[i].clips)
            if (c.id == id) { row = i; audio = true; return true; }
    return false;
}
