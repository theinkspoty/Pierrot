// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/timeline/TimelineWidget.h"
#include "colombina/models/Project.h"
#include "ui/dialogs/SettingsDialog.h"
#include "colombina/ffmpeg/MediaCache.h"
#include "colombina/generators.h"
#include "ui/timeline/TlLog.h"
#include "ui/core/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QStyleOptionRubberBand>
#include <QRubberBand>
#include <QStyle>
#include <QFileInfo>
#include <QtMath>
#include <QElapsedTimer>
#include <QFont>
#include <QMenu>
#include <QAction>

#include <algorithm>
#include <cmath>

#include "ui/timeline/TimelinePaintPriv.h"

// ── Geometry helpers ────────────────────────────────────────────────────────

double TimelineWidget::timeToX(double t) const {
    return kHeaderW + (t - m_viewStart) * m_pps;
}

double TimelineWidget::xToTime(int x) const {
    return m_viewStart + (x - kHeaderW) / m_pps;
}

int TimelineWidget::trackH(int idx, bool audio) const {
    if (!m_project) return audio ? kAudioRowH : kVideoRowH;
    const QVector<Track>& list = audio ? m_project->audioTracks : m_project->videoTracks;
    if (idx < 0 || idx >= (int)list.size()) return audio ? kAudioRowH : kVideoRowH;
    // Faixa recolhida (seta do cabeçalho, estilo Premiere): altura mínima.
    if (list[idx].collapsed) return kMinRowH;
    int h = list[idx].height;
    if (h < kMinRowH) h = audio ? kAudioRowH : kVideoRowH;
    // Faixa de efeitos expandida (estilo AE): soma o painel de efeitos abaixo
    // do corpo do clipe (a tira do clipe fica com a altura padrão de vídeo).
    if (list[idx].fxTrack && isFxExpanded(list[idx].id))
        h = qMax(h, kVideoRowH) + fxPanelH(list[idx]);
    return std::min(h, kMaxRowH);
}

int TimelineWidget::rowY(int videoIdx, int audioIdx) const {
    const int n = (int)m_project->videoTracks.size();
    int y = kRulerH - m_viewTop;
    if (videoIdx >= 0) {
        for (int i = 0; i < videoIdx; ++i)
            if (trackVisible(i, false)) y += trackH(i, false);
        y += folderStripsAboveVideo(videoIdx) * kFolderH;
        return y;
    }
    for (int i = 0; i < n; ++i)
        if (trackVisible(i, false)) y += trackH(i, false);
    y += folderStripsAboveVideo(-1) * kFolderH;
    y += folderStripsAboveAudio(audioIdx) * kFolderH;
    for (int i = 0; i < audioIdx; ++i)
        if (trackVisible(i, true)) y += trackH(i, true);
    return y;
}

bool TimelineWidget::rowFromY(int y, int& row, bool& audio) const {
    if (!m_project) return false;
    int rem = y + m_viewTop - kRulerH;
    if (rem < 0) return false;
    const int n = (int)m_project->videoTracks.size();
    for (int i = 0; i < n; ++i) {
        if (!trackVisible(i, false)) continue;
        const int above = folderStripsAboveVideo(i);
        const int below = (i > 0) ? folderStripsAboveVideo(i - 1) : 0;
        rem -= (above - below) * kFolderH;
        const int h = trackH(i, false);
        if (rem < h) { row = i; audio = false; return true; }
        rem -= h;
    }
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
        if (!trackVisible(i, true)) continue;
        const int above = folderStripsAboveAudio(i);
        const int below = (i > 0) ? folderStripsAboveAudio(i - 1) : 0;
        rem -= (above - below) * kFolderH;
        const int h = trackH(i, true);
        if (rem < h) { row = i; audio = true; return true; }
        rem -= h;
    }
    return false;
}

// ── Seção de gravação (vermelha) ──────────────────────────────────────
// Fica abaixo das faixas de áudio. A altura padrão acompanha a de áudio: a
// faixa vai receber os clipes gravados, que têm barra de volume como os
// outros clipes de áudio.

int TimelineWidget::recTrackH(int idx) const {
    if (!m_project) return kAudioRowH;
    if (idx < 0 || idx >= (int)m_project->recordingTracks.size()) return kAudioRowH;
    if (m_project->recordingTracks[idx].collapsed) return kMinRowH;
    const int h = m_project->recordingTracks[idx].height;
    if (h >= kMinRowH) return std::min(h, kMaxRowH);
    return kAudioRowH;
}

bool TimelineWidget::recTrackVisible(int idx) const {
    if (!m_project) return true;
    if (idx < 0 || idx >= (int)m_project->recordingTracks.size()) return true;
    const QString& gid = m_project->recordingTracks[idx].groupId;
    if (gid.isEmpty()) return true;
    const TrackGroup* g = m_project->findGroup(gid);
    return g ? !g->collapsed : true;
}

int TimelineWidget::recRowY(int idx) const {
    if (!m_project) return kRulerH;
    // Gravação é sempre a última seção, então começa abaixo de TODAS as
    // faixas de áudio. rowY(-1, audioTracks.size()) dá o fundo da última
    // faixa de áudio, já contando as barras de pasta.
    int y = rowY(-1, (int)m_project->audioTracks.size()) + 2; // +2: divisória
    for (int i = 0; i < idx; ++i)
        if (recTrackVisible(i)) y += recTrackH(i);
    return y;
}

bool TimelineWidget::recRowFromY(int y, int& row) const {
    if (!m_project) return false;
    int rem = y - recRowY(0);
    if (rem < 0) return false; // acima da seção: é vídeo/áudio
    for (int i = 0; i < (int)m_project->recordingTracks.size(); ++i) {
        if (!recTrackVisible(i)) continue;
        const int h = recTrackH(i);
        if (rem < h) { row = i; return true; }
        rem -= h;
    }
    return false;
}

Clip* TimelineWidget::clipAt(int row, bool audio, double t) const {
    if (!m_project) return nullptr;
    const auto& clips = audio
        ? (row >= 0 && row < (int)m_project->audioTracks.size()
           ? m_project->audioTracks[row].clips : QVector<Clip>{})
        : (row >= 0 && row < (int)m_project->videoTracks.size()
           ? m_project->videoTracks[row].clips : QVector<Clip>{});
    if (clips.isEmpty()) return nullptr;
    // A lista de clipes NÃO é garantidamente ordenada por pos: colar/duplicar
    // empilham no fim do vetor (push_back). A busca binária antiga assumia
    // ordem cronológica e, com a lista desordenada, errava o alvo — o clipe
    // colado ficava visível mas não respondia a clique/arraste (não dava para
    // selecionar nem mover). Varre e devolve o de maior pos que contém t (em
    // sobreposição de transição, o clipe da frente é o que começa depois).
    Clip* best = nullptr;
    for (const Clip& c : clips) {
        if (t >= c.pos && t < c.pos + c.dur) {
            if (!best || c.pos > best->pos) best = const_cast<Clip*>(&c);
        }
    }
    return best;
}

int TimelineWidget::volLineY(int row, bool audio, const Track& tr) const {
    const int rowH = trackH(row, audio);
    const int y = rowY(-1, row);
    const int pad = 6;
    const double frac = std::clamp(tr.volume, 0.0, 2.0) / 2.0;
    return y + rowH - pad - (int)std::lround(frac * (rowH - pad * 2.0));
}

int TimelineWidget::trackVolLineYAt(int row, double value) const {
    const int rowH = trackH(row, true);
    const int y = rowY(-1, row);
    const int pad = 6;
    const double frac = std::clamp(value, 0.0, 2.0) / 2.0;
    return y + rowH - pad - (int)std::lround(frac * (rowH - pad * 2.0));
}

int TimelineWidget::trackEnvKfAt(const QPoint& p, int& row, bool& audio) const {
    if (!m_project) return -1;
    const double t0 = m_viewStart;
    const double t1 = t0 + (width() - kHeaderW) / m_pps;
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
        const Track& tr = m_project->audioTracks[i];
        if (tr.kfVolume.isEmpty() || !trackVisible(i, true)) continue;
        const int y = rowY(-1, i);
        if (p.y() < y || p.y() >= y + trackH(i, true)) continue;
        for (int k = 0; k < tr.kfVolume.size(); ++k) {
            const double kt = tr.kfVolume[k].time;
            if (kt < t0 - 0.1 || kt > t1 + 0.1) continue; // pula keyframes fora da view
            const int kx = (int)(kHeaderW + (kt - m_viewStart) * m_pps);
            const int ky = trackVolLineYAt(i, tr.kfVolume[k].value);
            if (std::abs(p.x() - kx) <= 6 && std::abs(p.y() - ky) <= 6) {
                row = i;
                audio = true;
                return k;
            }
        }
    }
    return -1;
}

void TimelineWidget::drawTrackVolEnvelope(QPainter& p, int row, const Track& tr) {
    const int Hx = kHeaderW;
    const double t0 = m_viewStart;
    const double t1 = t0 + (width() - Hx) / m_pps;
    p.save();
    p.setClipRect(QRect(Hx, kRulerH, width() - Hx, height() - kRulerH));

    // Premiere: só a curva + os diamantes dos keyframes, sem preenchimento
    // colorido sob a linha (o preenchimento era estilo Vegas).
    QPolygon poly;
    poly.reserve((width() - Hx) / 2 + 2);
    for (int px = Hx; px <= width(); px += 2) {
        const double t = t0 + (px - Hx) / m_pps;
        if (t > t1) break;
        const double v = kfValue(tr.kfVolume, tr.volume, t);
        poly << QPoint(px, trackVolLineYAt(row, v));
    }
    const bool activeCurve = m_dragMode == TrackEnvVol && m_envRow == row;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(activeCurve ? themeColors().accentGold
                              : QColor(255, 255, 255),
                  activeCurve ? 2 : 1, Qt::SolidLine));
    p.drawPolyline(poly);

    // Diamantes dos keyframes (maiores quando o ponto está sendo arrastado).
    for (int k = 0; k < tr.kfVolume.size(); ++k) {
        const int kx = (int)(Hx + (tr.kfVolume[k].time - m_viewStart) * m_pps);
        const int ky = trackVolLineYAt(row, tr.kfVolume[k].value);
        const bool hot = m_dragMode == TrackEnvVol && m_envRow == row && m_envKf == k;
        const int r = hot ? 4 : 3;
        p.setPen(QPen(hot ? QColor(255, 255, 255) : QColor(235, 235, 235),
                      hot ? 2 : 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPoint(kx, ky), r, r);
    }
    p.restore();
}

int TimelineWidget::clipVolLineY(int row, const Clip& c) const {
    const int rowH = trackH(row, true);
    const int y = rowY(-1, row);
    const int pad = 6;
    const double frac = std::clamp(c.volume, 0.0, 2.0) / 2.0;
    return y + rowH - pad - (int)std::lround(frac * (rowH - pad * 2.0));
}

int TimelineWidget::folderStripsAboveVideo(int videoIdx) const {
    int count = 0;
    for (const TrackGroup& g : m_project->trackGroups) {
        int first = -1;
        for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
            if (m_project->videoTracks[i].groupId == g.id) { first = i; break; }
        if (first < 0) continue;
        if (videoIdx < 0 || first <= videoIdx) ++count;
    }
    return count;
}

int TimelineWidget::folderStripsAboveAudio(int audioIdx) const {
    int count = 0;
    for (const TrackGroup& g : m_project->trackGroups) {
        bool hasVideo = false;
        for (const Track& t : m_project->videoTracks)
            if (t.groupId == g.id) { hasVideo = true; break; }
        if (hasVideo) continue;
        int top = -1;
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
            if (m_project->audioTracks[i].groupId == g.id) { top = i; break; }
        if (top >= 0 && top <= audioIdx) ++count;
    }
    return count;
}

QRect TimelineWidget::folderStripRect(const TrackGroup& g) const {
    if (!m_project) return QRect();
    int topVideo = -1;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (m_project->videoTracks[i].groupId == g.id) { topVideo = i; break; }
    if (topVideo >= 0) {
        const int y = rowY(topVideo, -1) - kFolderH;
        return QRect(0, y, width(), kFolderH);
    }
    int topAudio = -1;
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        if (m_project->audioTracks[i].groupId == g.id) { topAudio = i; break; }
    if (topAudio >= 0) {
        const int y = rowY(-1, topAudio) - kFolderH;
        return QRect(0, y, width(), kFolderH);
    }
    return QRect();
}

bool TimelineWidget::folderStripAt(int y, QString& gid) const {
    if (!m_project) return false;
    for (const TrackGroup& g : m_project->trackGroups) {
        const QRect r = folderStripRect(g);
        if (y >= r.top() && y < r.bottom()) { gid = g.id; return true; }
    }
    return false;
}

QRect TimelineWidget::folderArrowRect(const TrackGroup& g) const {
    const QRect r = folderStripRect(g);
    if (r.isEmpty()) return QRect();
    return QRect(r.left() + 2, r.top() + (kFolderH - 14) / 2, 18, 14);
}

bool TimelineWidget::trackVisible(int row, bool audio) const {
    if (!m_project) return true;
    const QVector<Track>& list = audio ? m_project->audioTracks : m_project->videoTracks;
    if (row < 0 || row >= (int)list.size()) return true;
    const QString& gid = list[row].groupId;
    if (gid.isEmpty()) return true;
    const TrackGroup* g = m_project->findGroup(gid);
    return g ? !g->collapsed : true;
}

// ── Painting ────────────────────────────────────────────────────────────────

void TimelineWidget::paintEvent(QPaintEvent*) {
    QElapsedTimer dbg; dbg.start();
    QPainter p(this);
    p.fillRect(rect(), themeColors().timelineBg);
    if (!m_project) return;

    if (m_staticDirty || m_staticCache.size() != size()) {
        if (m_staticCache.size() != size())
            m_staticCache = QPixmap(size());
        m_staticCache.fill(Qt::transparent);
        QPainter sp(&m_staticCache);
        renderScene(sp);
        m_staticDirty = false;
    }
    p.drawPixmap(0, 0, m_staticCache);
    renderOverlays(p);
    m_perfPaintMs = dbg.elapsed();
    static const bool perfOn = qEnvironmentVariableIsSet("PIERROT_PERF_DEBUG");
    if (perfOn) {
        p.save();
        p.setFont(QFont(QStringLiteral("monospace"), 8));
        p.setPen(QColor(255, 255, 255));
        p.fillRect(QRect(width() - 190, height() - 18, 190, 18), QColor(0, 0, 0, 170));
        p.drawText(QRect(width() - 190, height() - 18, 190, 18),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("timeline: paint %1ms setPlayhead %2ms")
                       .arg(m_perfPaintMs).arg(m_perfPlayheadMs));
        p.restore();
    }
}

void TimelineWidget::renderScene(QPainter& p) {
    const int H = kHeaderW;
    const int R = kRulerH;

    // Fundo da régua (só se habilitada).
    if (m_showRuler) {
        p.fillRect(0, 0, width(), R, themeColors().rulerBg);
        p.setPen(themeColors().rulerTickMajor);
        p.drawLine(0, R - 1, width(), R - 1);
    } else {
        p.fillRect(0, 0, width(), R, themeColors().rulerBg);
    }
    p.drawLine(H - 1, R, H - 1, height());

    int rowsBottom = kRulerH - m_viewTop;
    rowsBottom += folderStripsAboveVideo(-1) * kFolderH;
    rowsBottom += folderStripsAboveAudio((int)m_project->audioTracks.size() - 1) * kFolderH;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (trackVisible(i, false)) rowsBottom += trackH(i, false);
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        if (trackVisible(i, true)) rowsBottom += trackH(i, true);
    const int gridBottom = std::min(height(), rowsBottom);
    double step = 1.0;
    while (step * m_pps < 70.0) step *= 2.0;
    int subdiv = 5;
    while (subdiv > 1 && (step / subdiv) * m_pps < 9.0) {
        if (subdiv == 5) subdiv = 4;
        else if (subdiv == 4) subdiv = 2;
        else subdiv = 1;
    }
    const double mstep = step / subdiv;
    const double last = m_viewStart + (width() - H) / m_pps;
    const long long k0 = (long long)std::ceil(m_viewStart / mstep);
    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);
    for (long long k = k0; k * mstep <= last + 1e-9; ++k) {
        const double t = k * mstep;
        const int x = (int)(H + (t - m_viewStart) * m_pps);
        if (x < H - 1) continue;
        const bool major = (k % subdiv) == 0;
        if (major) {
            if (m_showRuler) {
                p.setPen(QPen(themeColors().rulerTickMajor, 1));
                p.drawLine(x, 1, x, R - 1);
                p.setPen(themeColors().rulerText);
                p.drawText(x + 4, R - 6, fmtRuler(t));
            }
            if (m_showGrid) {
                p.setPen(themeColors().timelineGrid);
                p.drawLine(x, R, x, gridBottom);
            }
        } else if (m_showRuler) {
            p.setPen(themeColors().rulerTick);
            p.drawLine(x, R - 12, x, R - 2);
        }
    }

    // Marcadores: linha vertical + balão colorido com o nome. Tudo clipado
    // DENTRO da régua — a cor nunca vaza para a faixa de baixo (antes a
    // linha passava raspando pela borda inferior e o nome ficava solto,
    // colado na faixa, quase ilegível).
    for (const Marker& mk : m_project->markers) {
        const int mx = (int)(H + (mk.time - m_viewStart) * m_pps);
        if (mx < H - 1 || mx > width()) continue;
        p.save();
        p.setClipRect(H - 1, 0, width() - H + 1, R);
        p.setPen(QPen(mk.color, 1));
        p.drawLine(mx, 0, mx, R - 1);
        if (mk.name.isEmpty()) {
            // defensivo (todo marcador novo ganha nome): só a "cabeça"
            p.setPen(Qt::NoPen);
            p.setBrush(mk.color);
            p.drawRect(mx, 2, 5, 5);
        } else {
            QFont f = p.font();
            f.setPointSizeF(8);
            f.setBold(true);
            p.setFont(f);
            const QFontMetrics fm(f);
            const int pad = 4;
            const int bh = R - 8; // régua22px → balão14px
            const QRect badge(mx + 4, (R - bh) / 2,
                              fm.horizontalAdvance(mk.name) + pad * 2, bh);
            p.setPen(Qt::NoPen);
            p.setBrush(mk.color);
            p.drawRoundedRect(badge, 3, 3);
            p.setPen(QPen(mk.color.darker(160), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(badge, 3, 3);
            p.setPen(readableTextOn(mk.color));
            p.drawText(badge, Qt::AlignCenter, mk.name);
        }
        p.restore();
    }

    const int zx0 = 6;
    p.fillRect(zx0, 2, kZoomW, R - 4, themeColors().trackLabelBg);
    p.setPen(themeColors().rulerText);
    QFont zf = p.font();
    zf.setPointSizeF(9);
    zf.setBold(true);
    p.setFont(zf);
    p.drawText(QRect(zx0, 2, kZoomW / 2, R - 4), Qt::AlignCenter, QStringLiteral("\u2212"));
    p.drawText(QRect(zx0 + kZoomW / 2, 2, kZoomW / 2, R - 4), Qt::AlignCenter, QStringLiteral("+"));
    p.drawLine(zx0 + kZoomW / 2, 4, zx0 + kZoomW / 2, R - 6);

    p.save();
    p.setClipRect(QRect(0, R, width(), height() - R));
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i) {
        if (!trackVisible(i, false)) continue;
        const int y = rowY(i, -1);
        const int rowH = trackH(i, false);
        const bool sel = isTrackSelected(i, false);
        // Premiere: fundo chapado em todas as pistas (vídeo e áudio), sem
        // zebrado — a separação vem da linha divisória no rodapé de cada uma.
        // Seleção = tint translúcido de acento (não cor chapada).
        p.fillRect(0, y, width(), rowH, themeColors().trackBg);
        if (sel) {
            p.fillRect(0, y, 4, rowH, themeColors().accent);
            // Outline só na área de conteúdo (não cobre o cabeçalho).
            p.setPen(QPen(themeColors().accent, 1));
            p.drawRect(kHeaderW, y, width() - kHeaderW - 1, rowH - 1);
        }
        p.setPen(themeColors().trackBorder);
        p.drawLine(0, y + rowH, width(), y + rowH);
        drawTrackHeader(p, y, rowH, m_project->videoTracks[i], i, sel);
    }

    // Barra divisória entre seções de vídeo e áudio.
    if (!m_project->videoTracks.isEmpty() && !m_project->audioTracks.isEmpty()) {
        int lastVideoBottom = 0;
        for (int i = 0; i < (int)m_project->videoTracks.size(); ++i) {
            if (!trackVisible(i, false)) continue;
            const int y = rowY(i, -1);
            const int rowH = trackH(i, false);
            lastVideoBottom = qMax(lastVideoBottom, y + rowH);
        }
        if (lastVideoBottom > 0) {
            p.fillRect(0, lastVideoBottom, width(), 2, themeColors().sectionDivider);
        }
    }

    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
        if (!trackVisible(i, true)) continue;
        const int y = rowY(-1, i);
        const int rowH = trackH(i, true);
        const bool sel = isTrackSelected(i, true);
        // Premiere: a pista de áudio é um fundo chapado, sem listras
        // alternadas — a separação entre faixas vem da linha divisória
        // sutil desenhada no rodapé de cada uma.
        p.fillRect(0, y, width(), rowH, themeColors().trackBg);
        if (sel) {
            p.fillRect(0, y, 4, rowH, themeColors().accent);
            // Outline só na área de conteúdo (não cobre o cabeçalho).
            p.setPen(QPen(themeColors().accent, 1));
            p.drawRect(kHeaderW, y, width() - kHeaderW - 1, rowH - 1);
        }
        p.setPen(themeColors().trackBorder);
        p.drawLine(0, y + rowH, width(), y + rowH);
        drawTrackHeader(p, y, rowH, m_project->audioTracks[i], i, sel);
    }

    // ── Seção de gravação ────────────────────────────────────────────
    // Faixa vermelha: é onde o material capturado cai. O fundo é o mesmo
    // escuro das outras seções (a cor vem do header e da barra de volume,
    // como nas faixas normais) — o que marca a seção como "gravação" é o
    // header vermelho e a divisória.
    if (!m_project->recordingTracks.isEmpty()) {
        int top = recRowY(0);
        int lastBottom = 0;
        for (int i = 0; i < (int)m_project->recordingTracks.size(); ++i) {
            if (!recTrackVisible(i)) continue;
            const int y = recRowY(i);
            const int rowH = recTrackH(i);
            lastBottom = qMax(lastBottom, y + rowH);
            const bool sel = isRecTrackSelected(i);
            p.fillRect(0, y, width(), rowH, themeColors().trackBg);
            if (sel) {
                p.fillRect(0, y, 4, rowH, recColor());
                p.setPen(QPen(recColor(), 1));
                p.drawRect(0, y, width() - 1, rowH - 1);
            }
            p.setPen(themeColors().trackBorder);
            p.drawLine(0, y + rowH, width(), y + rowH);
            drawTrackHeader(p, y, rowH, m_project->recordingTracks[i], i, sel);
        }
        // Divisória no topo da seção, separando-a das faixas de áudio.
        p.fillRect(0, top - 2, width(), 2, recColor().darker(160));
    }

    for (const TrackGroup& g : m_project->trackGroups)
        drawFolderStrip(p, g);

    p.restore();

    p.save();
    p.setClipRect(QRect(H, R, width() - H, height() - R));
    auto drawClips = [&](const QVector<Track>& tracks, Zone z) {
        const bool audio = (z != Zone::Video);
        QVector<QPair<int, const Clip*>> order;
        for (int i = 0; i < (int)tracks.size(); ++i) {
            const bool vis = (z == Zone::Rec) ? recTrackVisible(i)
                                             : trackVisible(i, z == Zone::Audio);
            if (!vis) continue;
            for (const Clip& c : tracks[i].clips)
                order.append(QPair<int, const Clip*>(i, &c));
        }
        std::stable_sort(order.begin(), order.end(),
                         [](const QPair<int, const Clip*>& a,
                            const QPair<int, const Clip*>& b) {
                             return a.second->pos < b.second->pos;
                         });
        for (const auto& it : order) {
            const int i = it.first;
            const Clip& c = *it.second;
            const Track& tr = tracks[i];
            const int y = (z == Zone::Rec) ? recRowY(i)
                        : (z == Zone::Audio ? rowY(-1, i) : rowY(i, -1));
            const int rowH = (z == Zone::Rec) ? recTrackH(i)
                                              : trackH(i, z == Zone::Audio);
            const int cx = (int)(H + (c.pos - m_viewStart) * m_pps);
            const int cw = std::max(2, (int)(c.dur * m_pps));
            // Faixa de efeitos expandida: o clipe ocupa só a tira do topo; o
            // painel de efeitos é desenhado abaixo (drawFxPanel).
            int clipH = rowH;
            if (tr.fxTrack && isFxExpanded(tr.id)) clipH = kVideoRowH;
            QRect r(cx + 1, y + 4, cw - 2, clipH - 8);
            if (r.width() <= 0 || r.height() <= 0) continue;
            if (r.right() < H || r.left() > width()) continue;
            drawClip(p, r, c, tr, i, audio);
        }
    };
    drawClips(m_project->videoTracks, Zone::Video);
    drawClips(m_project->audioTracks, Zone::Audio);
    drawClips(m_project->recordingTracks, Zone::Rec);

    // Painel de efeitos das faixas de efeitos expandidas (estilo AE), desenhado
    // no corpo da faixa, abaixo da tira do clipe de ajuste.
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i) {
        if (!trackVisible(i, false)) continue;
        const Track& tr = m_project->videoTracks[i];
        if (tr.fxTrack && isFxExpanded(tr.id) && !tr.collapsed)
            drawFxPanel(p, tr, rowY(i, -1), trackH(i, false));
    }

    // Sobreposições (vira transição/crossfade): percorre os pares adjacentes
    // ORDENADOS por posição, pois a lista de clipes pode não estar em ordem
    // cronológica. Vale para vídeo (indicador de transição) e áudio (rampas de
    // crossfade em X sobre a waveform).
    auto forEachOverlap = [&](const QVector<Track>& tracks, bool audio, auto&& fn) {
        for (int i = 0; i < (int)tracks.size(); ++i) {
            if (!trackVisible(i, audio)) continue;
            QVector<const Clip*> sorted;
            sorted.reserve(tracks[i].clips.size());
            for (const Clip& c : tracks[i].clips) sorted.append(&c);
            std::sort(sorted.begin(), sorted.end(),
                      [](const Clip* a, const Clip* b) { return a->pos < b->pos; });
            const int y = audio ? rowY(-1, i) : rowY(i, -1);
            const int rowH = audio ? trackH(i, true) : trackH(i, false);
            for (int k = 1; k < (int)sorted.size(); ++k) {
                const Clip& prev = *sorted[k - 1];
                const Clip& cur = *sorted[k];
                const double end = prev.pos + prev.dur;
                if (cur.pos >= end - 1e-6) continue;
                const int x0 = (int)(H + (cur.pos - m_viewStart) * m_pps);
                const int x1 = (int)(H + (end - m_viewStart) * m_pps);
                if (x1 < H || x0 > width()) continue;
                const QRect r(x0 + 1, y + 4, std::max(2, x1 - x0 - 2), rowH - 8);
                fn(prev, cur, r);
            }
        }
    };
    forEachOverlap(m_project->videoTracks, false,
                   [&](const Clip& prev, const Clip&, const QRect& r) {
                       const QString type = isTransition(prev.transitionType)
                                                ? prev.transitionType
                                                : QStringLiteral("dissolve");
                       drawTransitionIndicator(p, r, type);
                   });
    forEachOverlap(m_project->audioTracks, true,
                   [&](const Clip&, const Clip&, const QRect& r) {
                       drawAudioCrossfade(p, r);
                   });

    // Emenda nos cortes: borda entre clipes adjacentes (mesma faixa, sem
    // sobreposição) — indica onde o clipe foi dividido. Vídeo, áudio e
    // gravação. Ordena por posição para encontrar cortes mesmo quando a
    // lista original não está em ordem cronológica.
    auto drawCutLines = [&](const QVector<Track>& tracks, bool audio) {
        for (int i = 0; i < (int)tracks.size(); ++i) {
            const bool vis = audio ? trackVisible(i, true) : trackVisible(i, false);
            if (!vis) continue;
            const QVector<Clip>& clips = tracks[i].clips;
            if (clips.size() < 2) continue;
            const int y = audio ? rowY(-1, i) : rowY(i, -1);
            const int rowH = audio ? trackH(i, true) : trackH(i, false);
            // Ordena por posição para encontrar cortes.
            QVector<const Clip*> sorted;
            sorted.reserve(clips.size());
            for (const Clip& c : clips) sorted.append(&c);
            std::sort(sorted.begin(), sorted.end(),
                      [](const Clip* a, const Clip* b) { return a->pos < b->pos; });
            for (int k = 1; k < (int)sorted.size(); ++k) {
                const Clip* prev = sorted[k - 1];
                const Clip* cur = sorted[k];
                const double prevEnd = prev->pos + prev->dur;
                if (std::fabs(cur->pos - prevEnd) > 1e-6) continue;
                const int cx = (int)(H + (cur->pos - m_viewStart) * m_pps);
                if (cx < H || cx > width()) continue;
                // Separador fino e escuro no corte. Antes havia também uma
                // linha branca de altura total, que poluía timelines com muitos
                // cortes; a emenda sozinha já delimita o corte. A cor é a
                // trackBorder do tema (escura no dark, cinza no claro, marrom no
                // amarelo) para continuar legível em qualquer tema sem branco
                // fixo.
                const int seamTop = y + 4;
                const int seamH = std::max(1, rowH - 8);
                p.fillRect(cx - 1, seamTop, 2, seamH, themeColors().trackBorder);
            }
        }
    };
    drawCutLines(m_project->videoTracks, false);
    drawCutLines(m_project->audioTracks, true);

    p.restore();

    // Linha/envelope de volume da faixa: oculta por padrão; Shift+V liga/
    // desliga (como no Premiere, onde o envelope fica escondido até o usuário
    // pedir).
    if (m_showVolLines) {
        p.save();
        p.setClipRect(QRect(H, kRulerH, width() - H, height() - kRulerH));
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
            if (!trackVisible(i, true)) continue;
            const Track& tr = m_project->audioTracks[i];
            if (!tr.kfVolume.isEmpty()) continue; // curva desenhada à parte
            const int ly = volLineY(i, true, tr);
            const bool active = m_dragMode == TrackVol && m_volRow == i;
            if (active)
                p.setPen(QPen(themeColors().accentGold, 2, Qt::SolidLine));
            else
                p.setPen(QPen(QColor(255, 255, 255), 1, Qt::SolidLine));
            p.drawLine(H, ly, width(), ly);
        }
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
            if (!trackVisible(i, true)) continue;
            const Track& tr = m_project->audioTracks[i];
            if (!tr.kfVolume.isEmpty())
                drawTrackVolEnvelope(p, i, tr);
        }
        p.restore();
    }

    // Linha de volume individual do clipe de áudio: só com a tecla V.
    if (m_showVolLines) {
        p.save();
        p.setClipRect(QRect(H, kRulerH, width() - H, height() - kRulerH));
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
            if (!trackVisible(i, true)) continue;
            const Track& tr = m_project->audioTracks[i];
            for (const Clip& c : tr.clips) {
                const int cx = (int)(H + (c.pos - m_viewStart) * m_pps);
                const int cw = std::max(2, (int)(c.dur * m_pps));
                if (cx + cw < H || cx > width()) continue;
                const int ly = clipVolLineY(i, c);
                const bool active = m_dragMode == ClipVol && m_volClip == c.id;
                p.setPen(QPen(active ? themeColors().accentGold : QColor(255, 255, 255, 170),
                              active ? 2 : 1, Qt::SolidLine));
                p.drawLine(cx + 1, ly, cx + cw - 1, ly);
            }
        }
        p.restore();
    }
}

void TimelineWidget::renderOverlays(QPainter& p) {
    const int H = kHeaderW;
    const int R = kRulerH;

    if (m_loopOut > m_loopIn) {
        const int lx0 = (int)timeToX(m_loopIn);
        const int lx1 = (int)timeToX(m_loopOut);
        p.fillRect(QRect(lx0, 0, lx1 - lx0, R), QColor(140, 195, 255, 90));
        p.fillRect(QRect(lx0, R, lx1 - lx0, height() - R), QColor(140, 195, 255, 16));
        if (m_loopEnabled) {
            p.fillRect(QRect(lx0, 0, lx1 - lx0, R), QColor(255, 220, 90, 46));
            p.fillRect(QRect(lx0, R, lx1 - lx0, height() - R), QColor(255, 220, 90, 22));
        }
        p.setPen(QPen(QColor(110, 175, 245, m_loopEnabled ? 255 : 160), m_loopEnabled ? 2 : 1));
        p.drawLine(lx0, R, lx0, height());
        p.drawLine(lx1, R, lx1, height());
        auto drawEdgeTab = [&](int ex) {
            const QRect tab(ex - 4, 0, 8, R);
            p.setPen(QPen(QColor(20, 48, 90), 1));
            p.setBrush(QColor(120, 185, 255));
            p.drawRect(tab);
            p.setPen(QColor(30, 70, 120));
            p.drawLine(ex, 3, ex, R - 3);
        };
        drawEdgeTab(lx0);
        drawEdgeTab(lx1);
        if (m_loopEnabled) {
            const QString tag = QStringLiteral("loop");
            QFont tf = p.font();
            tf.setPointSizeF(7);
            tf.setBold(true);
            p.setFont(tf);
            QFontMetrics tfm(tf);
            const int tw = tfm.horizontalAdvance(tag) + 6;
            const QRect badge(lx0 + 2, 7, tw, 11);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(50, 140, 220));
            p.drawRoundedRect(badge, 2, 2);
            p.setPen(Qt::white);
            p.drawText(badge, Qt::AlignCenter, tag);
            p.setFont(p.font());
        }
    }

    const double px = H + (m_playhead - m_viewStart) * m_pps;
    if (px >= H && px <= width()) {
        p.setPen(themeColors().playhead);
        p.drawLine((int)px, R, (int)px, height());
        QPolygon tri;
        tri << QPoint((int)px - 6, 0) << QPoint((int)px + 6, 0) << QPoint((int)px, 9);
        p.setPen(Qt::NoPen);
        p.setBrush(themeColors().playheadHandle);
        p.drawPolygon(tri);
    }

    if (m_cursorT >= 0.0) {
        const double cx = timeToX(m_cursorT);
        if (cx >= H && cx <= width()) {
            p.setPen(QPen(QColor(255, 255, 255, 230), 1));
            p.drawLine((int)cx, R, (int)cx, height());
            QPolygon ctri;
            ctri << QPoint((int)cx - 5, 0) << QPoint((int)cx + 5, 0) << QPoint((int)cx, 8);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 235));
            p.drawPolygon(ctri);
        }
    }

    if (m_dragMode == Razor) {
        const int rx = (int)timeToX(m_razorT);
        p.setPen(QPen(QColor(255, 255, 255, 190), 1, Qt::DashLine));
        p.drawLine(rx, R, rx, height());
    }

    // Guia de alinhamento: linha branca vertical quando o clipe arrastado
    // encaixa com a borda de outro clipe, um marcador ou a agulha (S).
    if (m_snapLineX >= 0.0) {
        const int sx = (int)m_snapLineX;
        if (sx >= H && sx <= width()) {
            // Duas passadas: um halo translúcido dá legibilidade sobre clipe
            // claro sem engrossar a linha de fato.
            p.setPen(QPen(QColor(255, 255, 255, 70), 3));
            p.drawLine(sx, R, sx, height());
            p.setPen(QPen(QColor(255, 255, 255, 220), 1));
            p.drawLine(sx, R, sx, height());
        }
    }

    // Indicador de trim: a borda sob arraste ganha uma linha grossa na altura
    // da faixa, na cor que o Premiere associa à operação — amarelo = ripple,
    // vermelho = trim regular e roll. As duas barras horizontais nas pontas
    // dão a mesma leitura de "corte" do Premiere.
    if (m_trimEdgeX >= 0.0 && m_trimEdgeRow >= 0 && m_project &&
        trackVisible(m_trimEdgeRow, m_trimEdgeAudio)) {
        const int ex = (int)m_trimEdgeX;
        const int ty = m_trimEdgeAudio ? rowY(-1, m_trimEdgeRow)
                                       : rowY(m_trimEdgeRow, -1);
        const int th = trackH(m_trimEdgeRow, m_trimEdgeAudio);
        if (ex >= H && ex <= width() && th > 0) {
            const QColor c = m_trimEdgeRipple ? QColor(255, 196, 0)
                                              : QColor(235, 64, 52);
            p.setPen(QPen(QColor(0, 0, 0, 120), 4));
            p.drawLine(ex, ty, ex, ty + th);
            p.setPen(QPen(c, 3));
            p.drawLine(ex, ty, ex, ty + th);
            // Barras de extremidade.
            p.setPen(QPen(c, 2));
            p.drawLine(ex - 4, ty, ex + 4, ty);
            p.drawLine(ex - 4, ty + th, ex + 4, ty + th);
        }
    }

    if (m_dragMode == ZoomSelect) {
        const int zx1 = (int)timeToX(m_zoomT0);
        const int zx2 = (int)timeToX(m_zoomT1);
        const QRect zr(QPoint(std::min(zx1, zx2), R), QPoint(std::max(zx1, zx2), height()));
        QStyleOptionRubberBand opt;
        opt.initFrom(this);
        opt.rect = zr;
        opt.shape = QRubberBand::Rectangle;
        opt.opaque = false;
        style()->drawControl(QStyle::CE_RubberBand, &opt, &p, this);
    } else if (m_dragMode == Marquee) {
        // Recorta a caixa à área do timeline (não deve cobrir o cabeçalho
        // das faixas nem a régua).
        const QRect mr = m_marqueeRect.normalized().intersected(
            QRect(H, R, qMax(1, width() - H), qMax(1, height() - R)));
        if (!mr.isEmpty()) {
            QStyleOptionRubberBand opt;
            opt.initFrom(this);
            opt.rect = mr;
            opt.shape = QRubberBand::Rectangle;
            opt.opaque = false;
            style()->drawControl(QStyle::CE_RubberBand, &opt, &p, this);
        }
    }

    if (m_dragHoverRow >= 0 && m_dragHoverDur > 0.0) {
        const int hy = m_dragHoverAudio ? rowY(-1, m_dragHoverRow)
                                        : rowY(m_dragHoverRow, -1);
        const int hh = trackH(m_dragHoverRow, m_dragHoverAudio);
        const int gx0 = (int)timeToX(m_dragHoverT);
        const int gx1 = (int)timeToX(m_dragHoverT + m_dragHoverDur);
        const QRect ghost(gx0, hy + 2, qMax(1, gx1 - gx0), hh - 4);
        p.setPen(QPen(QColor(140, 200, 255), 1));
        p.fillRect(ghost.adjusted(1, 1, -1, -1), QColor(90, 165, 255, 95));
        p.drawRect(ghost);
        if (!m_dragHoverName.isEmpty() && ghost.width() > 40) {
            p.setPen(QColor(210, 235, 255));
            p.drawText(ghost.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       m_dragHoverName);
        }
        p.setPen(QColor(140, 200, 255, 180));
        p.drawLine(gx0, hy, gx0, hy + hh);
    }

    if (m_dragMode == TrackDrag) {
        const bool isGroup = !m_dragGroupId.isEmpty();
        if (isGroup) {
            if (m_dropRow >= 0) {
                const int dy = m_dropAudio ? rowY(-1, m_dropRow) : rowY(m_dropRow, -1);
                p.setPen(QPen(QColor(80, 200, 120), 2));
                p.drawLine(0, dy, width(), dy);
            }
        } else if (!m_dropGroup.isEmpty()) {
            const TrackGroup* g = m_project->findGroup(m_dropGroup);
            if (g) {
                const QRect fr = folderStripRect(*g);
                p.fillRect(fr, QColor(80, 200, 120, 60));
                p.setPen(QPen(QColor(80, 200, 120), 2));
                p.drawRect(fr.adjusted(0, 0, -1, -1));
            }
        } else if (m_dropRow >= 0) {
            const int dy = m_dropAudio ? rowY(-1, m_dropRow) : rowY(m_dropRow, -1);
            p.setPen(QPen(QColor(80, 200, 120), 2));
            p.drawLine(0, dy, width(), dy);
        }
    }
}

