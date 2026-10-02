// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TimelineWidget.h"
#include "colombina/models/Project.h"

bool TimelineWidget::isSelected(const QString& id) const {
    return m_selected.contains(id);
}

bool TimelineWidget::isSecondarySelected(const QString& id) const {
    return m_secondarySelected.contains(id);
}

void TimelineWidget::setSelection(const QString& id) {
    clearTrackSelection();
    m_selected.clear();
    m_secondarySelected.clear();
    m_selected.append(id);
    refreshView();
    emit selectionChanged(id);
}

void TimelineWidget::toggleSelection(const QString& id) {
    clearTrackSelection();
    m_secondarySelected.clear();
    const int i = m_selected.indexOf(id);
    if (i >= 0) m_selected.removeAt(i);
    else m_selected.append(id);
    refreshView();
    emit selectionChanged(m_selected.isEmpty() ? QString() : m_selected.last());
}

void TimelineWidget::setSecondarySelection(const QStringList& ids) {
    m_secondarySelected = ids;
    update();
}

bool TimelineWidget::isTrackSelected(int row, bool audio) const {
    return m_selTracks.contains(TrackSel{row, audio, false});
}

void TimelineWidget::setTrackSel(int row, bool audio) {
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        m_secondarySelected.clear();
        emit selectionChanged(QString());
    }
    m_selTracks.clear();
    m_selTracks.append(TrackSel{row, audio, false});
    m_selAnchor = TrackSel{row, audio, false};
    m_hasAnchor = true;
}

// Seleção na seção de gravação: mesma mecânica da de áudio, com `rec` ligado
// pra não colidir com uma faixa de áudio de mesmo índice.
bool TimelineWidget::isRecTrackSelected(int row) const {
    return m_selTracks.contains(TrackSel{row, true, true});
}

void TimelineWidget::setRecTrackSel(int row) {
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        m_secondarySelected.clear();
        emit selectionChanged(QString());
    }
    m_selTracks.clear();
    m_selTracks.append(TrackSel{row, true, true});
    m_selAnchor = TrackSel{row, true, true};
    m_hasAnchor = true;
}

void TimelineWidget::toggleTrackSel(int row, bool audio) {
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        m_secondarySelected.clear();
        emit selectionChanged(QString());
    }
    const TrackSel ts{row, audio};
    const int i = m_selTracks.indexOf(ts);
    if (i >= 0) m_selTracks.removeAt(i);
    else m_selTracks.append(ts);
    m_selAnchor = ts;
    m_hasAnchor = true;
}

void TimelineWidget::selectTrackRange(int row, bool audio) {
    if (!m_hasAnchor || m_selAnchor.audio != audio) {
        setTrackSel(row, audio);
        return;
    }
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        m_secondarySelected.clear();
        emit selectionChanged(QString());
    }
    int a = m_selAnchor.row;
    int b = row;
    if (a > b) std::swap(a, b);
    m_selTracks.clear();
    for (int k = a; k <= b; ++k)
        m_selTracks.append(TrackSel{k, audio});
}

void TimelineWidget::selectTrackRightClick(int row, bool audio) {
    if (isTrackSelected(row, audio)) return;
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        m_secondarySelected.clear();
        emit selectionChanged(QString());
    }
    m_selTracks.clear();
    m_selTracks.append(TrackSel{row, audio});
    m_selAnchor = TrackSel{row, audio};
    m_hasAnchor = true;
}

void TimelineWidget::clearTrackSelection() {
    m_selTracks.clear();
    m_hasAnchor = false;
}

void TimelineWidget::selectAllClips() {
    if (!m_project) return;
    clearTrackSelection();
    m_selected.clear();
    m_secondarySelected.clear();
    for (const Track& t : m_project->videoTracks)
        for (const Clip& c : t.clips) m_selected.append(c.id);
    for (const Track& t : m_project->audioTracks)
        for (const Clip& c : t.clips) m_selected.append(c.id);
    refreshView();
    emit selectionChanged(m_selected.isEmpty() ? QString() : m_selected.last());
}

void TimelineWidget::selectInMarquee(bool add) {
    if (!m_project) return;
    // Recorta defensivamente à área de conteúdo: o paint já clipa o rubber
    // band, mas a seleção usava o rect bruto — marquee que "sobia" para o
    // header/régua selecionava clipes com tempo errado (xToTime < viewStart).
    const QRect content(kHeaderW, kRulerH,
                        qMax(1, width() - kHeaderW),
                        qMax(1, height() - kRulerH));
    const QRect mr = m_marqueeRect.normalized().intersected(content);
    if (mr.isEmpty()) return;
    const double t0 = xToTime(mr.left());
    const double t1 = xToTime(mr.right());
    if (t1 - t0 < 1e-9) return;
    QStringList found;
    auto collect = [&](const Track& tr, bool audio, int row) {
        const int y = audio ? rowY(-1, row) : rowY(row, -1);
        const int rowH = trackH(row, audio);
        if (mr.bottom() < y || mr.top() > y + rowH) return;
        for (const Clip& c : tr.clips)
            if (c.pos + c.dur > t0 && c.pos < t1)
                found.append(c.id);
    };
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (trackVisible(i, false))
            collect(m_project->videoTracks[i], false, i);
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        if (trackVisible(i, true))
            collect(m_project->audioTracks[i], true, i);
    if (!add) m_selected.clear();
    m_secondarySelected.clear();
    for (const QString& id : found)
        if (!m_selected.contains(id)) m_selected.append(id);
    clearTrackSelection();
    refreshView();
    emit selectionChanged(m_selected.isEmpty() ? QString() : m_selected.last());
    update();
}
