// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TimelineWidget.h"
#include "models/Project.h"
#include "ui/SettingsDialog.h"
#include "ffmpeg/MediaCache.h"
#include "ui/TlLog.h"
#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionRubberBand>
#include <QRubberBand>
#include <QStyle>
#include <QFileInfo>
#include <QtMath>
#include <QElapsedTimer>
#include <QFont>

namespace {
constexpr int kHeaderW = 130;
constexpr int kRulerH = 26;
constexpr int kZoomW = 64;
constexpr int kFolderH = 22;
constexpr int kResizeHandleH = 5;
constexpr int kVideoRowH = 56;
constexpr int kAudioRowH = 44;
constexpr int kMinRowH = 24;
constexpr int kMaxRowH = 400;
constexpr double kMinPps = 2.0;
constexpr double kMaxPps = 4000.0;
constexpr double kMinDur = 0.04;

enum Tool {
    ToolSelect = 0, ToolMove = 1, ToolScissors = 2, ToolEnvelope = 3, ToolZoom = 4,
    ToolRipple = 5, ToolRolling = 6, ToolSlip = 7, ToolSlide = 8, ToolRateStretch = 9
};

QString fmtRuler(double t) {
    const int total = (int)std::floor(t);
    return QString("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Paleta automática de cores de faixa (estilo Vegas), usada quando a faixa
// não tem cor própria definida pelo usuário.
QColor autoTrackColor(int index) {
    // Paleta em tons de azul (estilo "cool"): matizes que variam de nobre
    // profundo a ciano, mantendo distinção entre faixas vizinhas.
    static const QColor pal[] = {
        QColor(66, 108, 188),  // azul profundo
        QColor(60, 154, 190),  // ciano aço
        QColor(52, 78, 150),   // navy
        QColor(88, 162, 214),  // celeste
        QColor(96, 92, 178),   // índigo
        QColor(54, 128, 158),  // azul-petróleo
        QColor(58, 122, 220),  // azul elétrico
        QColor(64, 162, 168),  // turquesa azulado
        QColor(48, 66, 132),   // azul-marinho
        QColor(104, 140, 196), // aço claro
    };
    return pal[index % (int)(sizeof pal / sizeof pal[0])];
}

// Cor efetiva de uma faixa para desenho (própria se definida, senão paleta).
QColor trackColorAt(const Track& tr, int index) {
    return tr.color.isValid() ? tr.color : autoTrackColor(index);
}
}

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
    const int h = list[idx].height;
    if (h >= kMinRowH) return std::min(h, kMaxRowH);
    return audio ? kAudioRowH : kVideoRowH;
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

Clip* TimelineWidget::clipAt(int row, bool audio, double t) const {
    if (!m_project) return nullptr;
    auto top = [t](QVector<Clip>& clips) -> Clip* {
        Clip* best = nullptr;
        for (auto& c : clips)
            if (t >= c.pos && t < c.pos + c.dur)
                if (!best || c.pos > best->pos) best = &c;
        return best;
    };
    if (audio) {
        if (row < 0 || row >= (int)m_project->audioTracks.size()) return nullptr;
        return top(m_project->audioTracks[row].clips);
    } else {
        if (row < 0 || row >= (int)m_project->videoTracks.size()) return nullptr;
        return top(m_project->videoTracks[row].clips);
    }
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
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
        const Track& tr = m_project->audioTracks[i];
        if (tr.kfVolume.isEmpty() || !trackVisible(i, true)) continue;
        const int y = rowY(-1, i);
        if (p.y() < y || p.y() >= y + trackH(i, true)) continue;
        for (int k = 0; k < tr.kfVolume.size(); ++k) {
            const int kx = (int)(kHeaderW + (tr.kfVolume[k].time - m_viewStart) * m_pps);
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
    const int rowH = trackH(row, true);
    const int pad = 6;
    const double t0 = m_viewStart;
    const double t1 = t0 + (width() - Hx) / m_pps;
    p.save();
    p.setClipRect(QRect(Hx, kRulerH, width() - Hx, height() - kRulerH));

    // Preenchimento suave sob a curva (0% no fundo da faixa até a curva).
    QPolygon band;
    band << QPoint(Hx, rowY(-1, row) + rowH);
    for (int px = Hx; px <= width(); px += 2) {
        const double t = t0 + (px - Hx) / m_pps;
        if (t > t1) break;
        const double v = kfValue(tr.kfVolume, tr.volume, t);
        band << QPoint(px, trackVolLineYAt(row, v));
    }
    band << QPoint(width(), rowY(-1, row) + rowH);
    QColor fill = themeColors().accentGold;
    fill.setAlpha(34);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPolygon(band);

    // Curva principal.
    QPolygon poly;
    poly.reserve((width() - Hx) / 2 + 2);
    for (int px = Hx; px <= width(); px += 2) {
        const double t = t0 + (px - Hx) / m_pps;
        if (t > t1) break;
        const double v = kfValue(tr.kfVolume, tr.volume, t);
        poly << QPoint(px, trackVolLineYAt(row, v));
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(themeColors().accentGold, 1, Qt::SolidLine));
    p.drawPolyline(poly);

    // Diamantes dos keyframes (maiores quando o ponto está sendo arrastado).
    for (int k = 0; k < tr.kfVolume.size(); ++k) {
        const int kx = (int)(Hx + (tr.kfVolume[k].time - m_viewStart) * m_pps);
        const int ky = trackVolLineYAt(row, tr.kfVolume[k].value);
        const bool hot = m_dragMode == TrackEnvVol && m_envRow == row && m_envKf == k;
        const int r = hot ? 4 : 3;
        p.setPen(QPen(hot ? QColor(255, 255, 255) : QColor(255, 224, 130),
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

    for (const Marker& mk : m_project->markers) {
        const int mx = (int)(H + (mk.time - m_viewStart) * m_pps);
        if (mx < H || mx > width()) continue;
        p.setPen(QPen(mk.color, 1));
        p.drawLine(mx, 2, mx, R - 4);
        QPolygon flag;
        flag << QPoint(mx, 2) << QPoint(mx + 6, 2) << QPoint(mx + 6, 8) << QPoint(mx, 8);
        p.setPen(Qt::NoPen);
        p.setBrush(mk.color);
        p.drawPolygon(flag);
        if (!mk.name.isEmpty()) {
            p.setPen(mk.color);
            p.drawText(mx + 8, R - 8, mk.name);
        }
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
        p.fillRect(0, y, width(), rowH, sel ? QColor(44, 50, 64)
                                            : ((i % 2) ? themeColors().trackBgAlt : themeColors().trackBg));
        if (sel) {
            p.fillRect(0, y, 4, rowH, themeColors().accent);
            p.setPen(QPen(themeColors().accent, 1));
            p.drawRect(0, y, width() - 1, rowH - 1);
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
        p.fillRect(0, y, width(), rowH, sel ? QColor(42, 48, 62)
                                            : ((i % 2) ? themeColors().trackBg : themeColors().trackBgAlt));
        if (sel) {
            p.fillRect(0, y, 4, rowH, themeColors().accent);
            p.setPen(QPen(themeColors().accent, 1));
            p.drawRect(0, y, width() - 1, rowH - 1);
        }
        p.setPen(themeColors().trackBorder);
        p.drawLine(0, y + rowH, width(), y + rowH);
        drawTrackHeader(p, y, rowH, m_project->audioTracks[i], i, sel);
    }

    for (const TrackGroup& g : m_project->trackGroups)
        drawFolderStrip(p, g);

    p.restore();

    p.save();
    p.setClipRect(QRect(H, R, width() - H, height() - R));
    auto drawClips = [&](const QVector<Track>& tracks, bool audio) {
        QVector<QPair<int, const Clip*>> order;
        for (int i = 0; i < (int)tracks.size(); ++i) {
            if (!trackVisible(i, audio)) continue;
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
            const int y = audio ? rowY(-1, i) : rowY(i, -1);
            const int rowH = audio ? trackH(i, true) : trackH(i, false);
            const int cx = (int)(H + (c.pos - m_viewStart) * m_pps);
            const int cw = std::max(2, (int)(c.dur * m_pps));
            QRect r(cx + 1, y + 4, cw - 2, rowH - 8);
            if (r.width() <= 0 || r.height() <= 0) continue;
            if (r.right() < H || r.left() > width()) continue;
            drawClip(p, r, c, tr, i, audio);
        }
    };
    drawClips(m_project->videoTracks, false);
    drawClips(m_project->audioTracks, true);

    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i) {
        if (!trackVisible(i, false)) continue;
        const QVector<Clip>& clips = m_project->videoTracks[i].clips;
        const int y = rowY(i, -1);
        const int rowH = trackH(i, false);
        for (int k = 1; k < (int)clips.size(); ++k) {
            const Clip& prev = clips[k - 1];
            const Clip& cur = clips[k];
            const double end = prev.pos + prev.dur;
            if (cur.pos >= end - 1e-6) continue;
            const int x0 = (int)(H + (cur.pos - m_viewStart) * m_pps);
            const int x1 = (int)(H + (end - m_viewStart) * m_pps);
            if (x1 < H || x0 > width()) continue;
            const QRect r(x0 + 1, y + 4, std::max(2, x1 - x0 - 2), rowH - 8);
            const QString type = isTransition(prev.transitionType)
                                     ? prev.transitionType
                                     : QStringLiteral("dissolve");
            drawTransitionIndicator(p, r, type);
        }
    }
    p.restore();

    // Linha/envelope de volume por faixa (estilo Vegas): visível só com a
    // tecla V, junto com as linhas de volume por clipe — clique na linha
    // alterna um ponto, em qualquer ferramenta.
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
    } else if (m_dragMode == ZoomSelect) {
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

void TimelineWidget::drawClip(QPainter& p, const QRect& r, const Clip& c,
                              const Track& tr, int trackIndex, bool audio) {
    if (r.width() <= 0 || r.height() <= 0) return;
    const bool sel = isSelected(c.id);
    const bool sel2 = !sel && isSecondarySelected(c.id);
    QColor fill = audio ? QColor(26, 86, 66) : themeColors().clipBg;
    QColor border = audio ? QColor(70, 160, 120) : themeColors().clipBorder;
    if (sel) {
        fill = audio ? QColor(40, 120, 92) : QColor(46, 96, 168);
        border = themeColors().clipBorderSelect;
    } else if (sel2) {
        fill = audio ? QColor(32, 100, 78) : QColor(38, 78, 138);
        border = themeColors().clipBorderSecondary;
    }
    p.setPen(QPen(border, sel ? 2 : sel2 ? 1.5 : 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, 3, 3);

    const MediaItem* mi = m_project ? m_project->findMedia(c.mediaId) : nullptr;
    const QString path = mi ? mi->filePath : QString();
    const QColor tint = trackColorAt(tr, trackIndex);
    const ClipVisKey key{c.id, r.width(), r.height(), m_clipEpoch,
                         tint.rgba()};
    QPixmap content = m_clipPix.value(key);
    if (content.isNull() || content.size() != r.size()) {
        content = QPixmap(r.size());
        content.fill(Qt::transparent);
        QPainter cp(&content);
        const QRect cr(0, 0, r.width(), r.height());
        if (audio)
            drawAudioWaveform(cp, cr, c, path, tint);
        else if (c.isText)
            drawTextClipBody(cp, cr, c);
        else if (mi && mi->isSolid)
            cp.fillRect(cr, mi->solidColor);
        else
            drawVideoThumbs(cp, cr, c, path);
        if (!audio)
            drawOpacityHandle(cp, cr, c);
        drawFadeCorners(cp, cr, c);
        if (m_tool == ToolEnvelope && (m_showVolLines || !audio))
            drawEnvelope(cp, cr, c, audio);
        const qint64 bytes = (qint64)content.width() * content.height()
                             * (content.depth() / 8);
        if (m_clipBytes + bytes > 96LL * 1024 * 1024) {
            m_clipPix.clear();
            m_clipBytes = 0;
        }
        m_clipBytes += bytes;
        m_clipPix.insert(key, content);
    }
    p.drawPixmap(r.topLeft(), content);

    if (m_tool != ToolEnvelope)
        drawKeyframeDiamonds(p, r, c, audio);

    QString label = c.name.isEmpty() ? tr.name : c.name;
    if (audio) {
        label += QString("  \u00b7  v %1%").arg((int)llround(c.volume * 100.0));
        if (c.hasAudioFx())
            label += QString("  \u00b7  FX");
    }
    if (std::fabs(c.speed - 1.0) > 1e-4)
        label += QString("  \u00b7  %1\u00d7").arg(c.speed, 0, 'g', 3);
    QFont f = p.font();
    f.setPointSizeF(8.5);
    p.setFont(f);
    QFontMetrics fm(f);
    label = fm.elidedText(label, Qt::ElideRight, std::max(1, r.width() - 10));
    QRect labelRect(r.left() + 3, r.top() + 2, r.width() - 6, fm.height());
    p.fillRect(labelRect, QColor(0, 0, 0, 110));
    // Ícone de áudio (alto-falante) para clipes de áudio.
    int textX = r.left() + 5;
    if (audio) {
        static QPixmap audioIcon;
        if (audioIcon.isNull()) {
            audioIcon = QPixmap(QStringLiteral(":/imagens/audio.svg"));
            if (!audioIcon.isNull())
                audioIcon = audioIcon.scaled(14, 14, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        if (!audioIcon.isNull()) {
            p.drawPixmap(textX, r.top() + 3, audioIcon);
            textX += 17;
        }
    }
    p.setPen(themeColors().clipText);
    QRect textRect(textX, r.top() + 2, r.right() - 3 - textX, fm.height());
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);

    if (!audio) {
        QRect obar(r.left() + 5, r.top() + 22, std::max(1, r.width() - 10), 3);
        const int w = (int)(obar.width() * c.opacity);
        p.fillRect(obar, QColor(0, 0, 0, 110));
        p.fillRect(QRect(obar.x(), obar.y(), w, obar.height()), QColor(255, 255, 255, 190));
    }

    if (!audio && c.id == m_hoverGripClip) {
        const int cx = r.center().x();
        QPainterPath tab;
        const int tw = 28;
        tab.moveTo(cx - tw / 2, r.top() + 1);
        tab.lineTo(cx + tw / 2, r.top() + 1);
        tab.lineTo(cx, r.top() + 14);
        tab.closeSubpath();
        p.setPen(QPen(QColor(255, 255, 255), 2));
        p.setBrush(QColor(180, 215, 255, 230));
        p.drawPath(tab);
    }

    if (c.id == m_hoverCornerClip && m_hoverCornerSide != 0) {
        const int s = 17;
        QPainterPath tab;
        if (m_hoverCornerSide < 0) {
            tab.moveTo(r.left() + 1, r.top() + 1);
            tab.lineTo(r.left() + 1, r.top() + s);
            tab.lineTo(r.left() + s, r.top() + 1);
        } else {
            tab.moveTo(r.right() - 1, r.top() + 1);
            tab.lineTo(r.right() - 1, r.top() + s);
            tab.lineTo(r.right() - s, r.top() + 1);
        }
        tab.closeSubpath();
        p.setPen(QPen(QColor(255, 255, 255), 2));
        p.setBrush(QColor(255, 220, 130, 240));
        p.drawPath(tab);
    }

    const QString range = fmtRuler(c.in) + " \u2013 " + fmtRuler(c.in + c.dur);
    f.setPointSizeF(7.5);
    p.setFont(f);
    QFontMetrics fm2(f);
    const QString rng = fm2.elidedText(range, Qt::ElideRight, std::max(1, r.width() - 10));
    QRect rangeRect(r.left() + 3, r.bottom() - fm2.height() - 2, r.width() - 6, fm2.height());
    p.fillRect(rangeRect, QColor(0, 0, 0, 110));
    p.setPen(themeColors().clipText);
    p.drawText(rangeRect, Qt::AlignLeft | Qt::AlignVCenter, rng);
}

void TimelineWidget::drawTextClipBody(QPainter& p, const QRect& r, const Clip& c) {
    const TextStyle& st = *m_project->textStyleFor(c);
    QFont f = p.font();
    f.setPointSizeF(9.5);
    f.setBold(true);
    p.setFont(f);
    p.setPen(st.textColor);
    p.drawText(r.adjusted(5, 3, -5, -3), Qt::AlignLeft | Qt::AlignTop, tr("T"));
    f.setPointSizeF(8.5);
    f.setBold(false);
    p.setFont(f);
    p.setPen(themeColors().clipText);
    QString txt = st.text.simplified();
    if (txt.isEmpty()) txt = tr("(texto vazio)");
    const QFontMetrics fm(f);
    txt = fm.elidedText(txt, Qt::ElideRight, std::max(10, r.width() - 16));
    p.drawText(QRect(r.left() + 4, r.top() + 18, std::max(10, r.width() - 8),
                     std::max(10, r.height() - 22)),
               Qt::AlignLeft | Qt::AlignTop, txt);
}


void TimelineWidget::drawAudioWaveform(QPainter& p, const QRect& r, const Clip& c,
                                       const QString& path, const QColor& tint) {
    if (path.isEmpty() || r.width() < 2) return;
    MediaCache& cache = MediaCache::instance();
    if (!cache.hasPeaks(path, c.audioStreamIndex)) {
        cache.requestPeaks(path, c.audioStreamIndex);
        p.setPen(QColor(255, 255, 255, 45));
        p.drawLine(r.left(), r.center().y(), r.right(), r.center().y());
        return;
    }

    const FFmpegAudioPeaks& pk = cache.peaks(path, c.audioStreamIndex);
    if (pk.min.isEmpty()) return;

    const int bps = pk.bucketsPerSecond > 0 ? pk.bucketsPerSecond : 1;
    const int x0 = r.left();
    const int x1 = r.right();
    const double dur = c.dur;
    const int midY = r.center().y();
    const double amp = r.height() / 2.0 - 2.0;
    const bool sel = isSelected(c.id);
    const bool sel2 = !sel && isSecondarySelected(c.id);

    // ── Fundo: faixa escura com leve gradiente para profundidade ──────
    p.fillRect(r, sel ? QColor(18, 52, 42) : sel2 ? QColor(16, 44, 36) : QColor(14, 14, 17));

    // ── Grade de referência dB (estilo profissional) ──────────────────
    // -6dB = 50%, -12dB = 25%, -18dB = 12.5% — linhas horizontais
    // Tracejas sutis para não poluir, mas visíveis o suficiente para
    // medir dynamic range durante a edição.
    struct DbLine { double frac; QColor color; };
    const DbLine dbLines[] = {
        { 0.50, QColor(80, 80, 90, 100) },   // -6dB
        { 0.25, QColor(65, 65, 75, 80) },     // -12dB
        { 0.125, QColor(55, 55, 65, 60) },    // -18dB
    };
    for (const DbLine& dl : dbLines) {
        const int dy = (int)std::lround(dl.frac * amp);
        p.setPen(QPen(dl.color, 1, Qt::DotLine));
        p.drawLine(x0, midY - dy, x1, midY - dy);
        p.drawLine(x0, midY + dy, x1, midY + dy);
    }

    // ── Linha zero (centro exato da onda) ────────────────────────────
    p.setPen(QPen(QColor(255, 255, 255, 50), 1));
    p.drawLine(x0, midY, x1, midY);

    // ── Marcadores de tempo (a cada 0.5s ou 1s conforme zoom) ─────────
    const double pxPerSec = r.width() / dur;
    const double timeStep = (pxPerSec > 200.0) ? 0.1 : (pxPerSec > 80.0) ? 0.25 : 0.5;
    const double startTime = std::ceil(c.in / timeStep) * timeStep;
    for (double t = startTime; t < c.in + dur; t += timeStep) {
        const int mx = x0 + (int)std::lround((t - c.in) / dur * r.width());
        if (mx < x0 || mx > x1) continue;
        const bool major = std::fabs(std::fmod(t, 1.0)) < 1e-6
                           || std::fabs(std::fmod(t, 1.0) - 1.0) < 1e-6;
        p.setPen(QPen(major ? QColor(255, 255, 255, 35) : QColor(255, 255, 255, 18), 1));
        p.drawLine(mx, midY - 3, mx, midY + 3);
    }

    // ── Onda preenchida (simétrica, estilo profissional) ──────────────
    // Cada coluna de pixel: preenche da borda superior (max) até a
    // inferior (min) com cor que escala com a amplitude. Seções mais
    // altas ficam mais brilhantes — permite identificar picos e silêncio
    // rapidamente ao editar.
    for (int x = x0; x <= x1; ++x) {
        const double t0 = c.in + (x - x0) * dur / (double)(x1 - x0 + 1);
        const double t1 = c.in + (x + 1 - x0) * dur / (double)(x1 - x0 + 1);
        int b0 = (int)std::floor(t0 * bps);
        int b1 = (int)std::floor(t1 * bps);
        if (b0 < 0) b0 = 0;
        if (b1 >= pk.min.size()) b1 = pk.min.size() - 1;
        if (b0 > b1) continue;

        float mn = 0.0f;
        float mx = 0.0f;
        for (int b = b0; b <= b1; ++b) {
            if (pk.min[b] < mn) mn = pk.min[b];
            if (pk.max[b] > mx) mx = pk.max[b];
        }

        const int py0 = (int)std::lround(midY - mx * amp);
        const int py1 = (int)std::lround(midY - mn * amp);
        const int top = qBound(r.top(), py0, r.bottom());
        const int bot = qBound(r.top(), py1, r.bottom());
        if (top >= bot) continue;

        // Amplitude normalizada (0..1) para escalar cor e brilho.
        const float peakAmp = std::max(std::fabs(mx), std::fabs(mn));
        const float norm = std::clamp(peakAmp, 0.0f, 1.0f);

        // Gradiente por matiz da faixa (estilo Vegas: o waveform usa a cor da
        // faixa). Silêncio = tom escuro, alto = matiz vivo e brilhante.
        qreal hue = tint.hslHueF();
        if (hue < 0.0) hue = 0.36; // cor indefinida → verde
        const qreal sat = qBound(0.55, 0.55 + norm * 0.35, 0.95);
        const qreal val = qBound(0.42, 0.42 + norm * 0.42, 0.88);
        const qreal alph = qBound(0.55, 0.55 + norm * 0.4, 1.0);
        const QColor col = QColor::fromHslF(hue, sat, val, alph);

        // Preenchimento simétrico: espelha a onda acima e abaixo do zero.
        // A parte "positiva" (topo) fica levemente mais clara; a
        // "negativa" (base) fica levemente mais escura — dá noção de
        // polaridade sem precisar de channel split.
        p.setPen(Qt::NoPen);
        if (top < midY) {
            p.setBrush(col.lighter(106));
            p.drawRect(x, top, 1, midY - top);
        }
        if (bot > midY) {
            p.setBrush(col.darker(106));
            p.drawRect(x, midY, 1, bot - midY);
        }

        // Borda de pico (1px branco sutil no extremo) para definir contorno.
        if (peakAmp > 0.01f) {
            p.setPen(QPen(QColor(255, 255, 255, (int)(40 + norm * 60)), 1));
            if (mx > 0.01f) p.drawPoint(x, top);
            if (mn < -0.01f) p.drawPoint(x, bot);
        }
    }

    // ── Borda do clipe (contorno sutil) ──────────────────────────────
    p.setPen(QPen(sel ? themeColors().clipBorderSecondary : QColor(70, 160, 120, 80), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0, 0, -1, -1));
}

void TimelineWidget::drawVideoThumbs(QPainter& p, const QRect& r, const Clip& c,
                                     const QString& path) {
    if (path.isEmpty()) return;
    const int mode = SettingsDialog::thumbMode();
    if (mode == 2) return;

    MediaCache& cache = MediaCache::instance();

    const auto drawCover = [&p](const QRect& slice, const QImage& img) {
        const double scale = std::max(slice.width() / (double)img.width(),
                                      slice.height() / (double)img.height());
        const int tw = (int)std::ceil(img.width() * scale);
        const int th = (int)std::ceil(img.height() * scale);
        p.drawImage(QRect(slice.x() + (slice.width() - tw) / 2,
                          slice.y() + (slice.height() - th) / 2, tw, th),
                    img);
    };

    QPainterPath clipPath;
    clipPath.addRoundedRect(r, 3, 3);
    p.save();
    p.setClipPath(clipPath);

    if (mode == 1) {
        const int sw = std::min(96, std::max(1, r.width() / 3));
        const QRect slices[2] = {
            QRect(r.left(), r.top(), sw, r.height()),
            QRect(r.right() - sw + 1, r.top(), sw, r.height()),
        };
        const double ts[2] = { c.in, std::max(c.in, c.in + c.dur - 0.01) };
        QList<double> want;
        for (int i = 0; i < 2; ++i) {
            if (i == 1 && r.width() < sw * 2 + 2) break;
            const double k = std::round(ts[i] * 10.0) / 10.0;
            QImage img = cache.thumb(path, k);
            if (img.isNull()) {
                want.append(k);
                p.fillRect(slices[i].adjusted(1, 1, -1, -1), QColor(0, 0, 0, 70));
                continue;
            }
            drawCover(slices[i], img);
        }
        if (!want.isEmpty()) cache.requestThumbs(path, want);
        p.restore();
        return;
    }

    const int sliceW = 96;
    const int maxSlices = 20;
    const int n = std::clamp(std::max(1, r.width() / sliceW), 1, maxSlices);

    QList<double> want;
    for (int i = 0; i < n; ++i) {
        const int sliceLeft = r.left() + i * r.width() / n;
        const int sliceRight = r.left() + (i + 1) * r.width() / n;
        const int sw = sliceRight - sliceLeft;
        const QRect sliceRect(sliceLeft, r.top(), sw, r.height());

        const double t = c.in + (i + 0.5) / n * c.dur;
        const double k = std::round(t * 10.0) / 10.0;
        QImage img = cache.thumb(path, k);
        if (img.isNull()) {
            want.append(k);
            p.fillRect(sliceRect.adjusted(1, 1, -1, -1), QColor(0, 0, 0, 70));
            continue;
        }

        drawCover(sliceRect, img);
    }
    if (!want.isEmpty()) cache.requestThumbs(path, want);
    p.restore();
}

void TimelineWidget::drawEnvelope(QPainter& p, const QRect& r, const Clip& c, bool audio) {
    const double maxV = audio ? 3.0 : 1.0;
    const QVector<Keyframe>& keys = audio ? c.kfVolume : c.kfOpacity;

    QPainterPath path;
    const int steps = std::max(2, r.width() / 2);
    for (int i = 0; i <= steps; ++i) {
        const double t = (double)i / steps * c.dur;
        const double v = audio ? kfValue(c.kfVolume, c.volume, t)
                               : kfValue(c.kfOpacity, c.opacity, t);
        const double cl = std::clamp(v, 0.0, maxV) / maxV;
        const int x = r.left() + (int)std::lround((double)i / steps * r.width());
        const int y = r.bottom() - (int)std::lround(cl * (r.height() - 4.0)) - 2;
        if (i == 0) path.moveTo(x, y);
        else path.lineTo(x, y);
    }
    p.setPen(QPen(themeColors().accentGold, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    for (const Keyframe& k : keys) {
        if (k.time < -1e-6 || k.time > c.dur + 1e-6) continue;
        const int x = r.left() + (int)std::lround(k.time / c.dur * r.width());
        const double cl = std::clamp(k.value, 0.0, maxV) / maxV;
        const int y = r.bottom() - (int)std::lround(cl * (r.height() - 4.0)) - 2;
        p.setPen(Qt::NoPen);
        p.setBrush(themeColors().accentGold);
        p.drawEllipse(QPoint(x, y), 3, 3);
    }
}

void TimelineWidget::drawFadeCorners(QPainter& p, const QRect& r, const Clip& c) {
    const double dur = std::max(c.dur, kMinDur);
    const int fi = (int)std::round(std::min(c.fadeIn, dur) / dur * r.width());
    const int fo = (int)std::round(std::min(c.fadeOut, dur) / dur * r.width());
    auto drawCornerTab = [&](bool right, int len) {
        const bool active = len > 0;
        const QColor fill = active ? QColor(255, 195, 70) : QColor(255, 255, 255, 120);
        const int s = 13;
        QPainterPath tab;
        if (!right) {
            tab.moveTo(r.left() + 1, r.top() + 1);
            tab.lineTo(r.left() + 1, r.top() + s);
            tab.lineTo(r.left() + s, r.top() + 1);
        } else {
            tab.moveTo(r.right() - 1, r.top() + 1);
            tab.lineTo(r.right() - 1, r.top() + s);
            tab.lineTo(r.right() - s, r.top() + 1);
        }
        tab.closeSubpath();
        p.setPen(QPen(QColor(255, 245, 210, 190), 1));
        p.setBrush(fill);
        p.drawPath(tab);
    };
    drawCornerTab(false, fi);
    drawCornerTab(true, fo);

    QPainterPath path;
    if (fi > 0) {
        path.moveTo(r.left(), r.top());
        path.lineTo(r.left() + fi, r.top());
        path.lineTo(r.left() + fi, r.top() + fi);
        path.closeSubpath();
    }
    if (fo > 0) {
        path.moveTo(r.right(), r.top());
        path.lineTo(r.right() - fo, r.top());
        path.lineTo(r.right() - fo, r.top() + fo);
        path.closeSubpath();
    }
    if (!path.isEmpty()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawPath(path);
    }
}

void TimelineWidget::drawOpacityHandle(QPainter& p, const QRect& r, const Clip& c) {
    if (r.width() < 16 || r.height() < 10) return;
    const bool active = c.opacity < 1.0 - 1e-4;
    const QColor col = active ? themeColors().accent : QColor(255, 255, 255, 70);

    const int lineY = r.top() + (int)std::lround((1.0 - c.opacity) * r.height());
    const int visY = qBound(r.top() + 1, lineY, r.bottom());
    if (active) {
        const int h = std::max(0, visY - r.top());
        p.fillRect(r.left(), r.top(), r.width(), h, QColor(0, 0, 0, 100));
    }
    p.setPen(QPen(active ? themeColors().accent : QColor(255, 255, 255, 80), 1));
    p.drawLine(r.left(), visY, r.right(), visY);

    const int cx = r.center().x();
    const int tw = 22;
    QPainterPath tab;
    tab.moveTo(cx - tw / 2, r.top() + 1);
    tab.lineTo(cx + tw / 2, r.top() + 1);
    tab.lineTo(cx, r.top() + 12);
    tab.closeSubpath();
    p.setPen(QPen(active ? QColor(150, 200, 255) : QColor(255, 255, 255, 130), 1));
    p.setBrush(col);
    p.drawPath(tab);

    if (active) {
        p.save();
        QFont f = p.font();
        f.setPointSizeF(7.5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(140, 180, 255));
        p.drawText(r.left() + 4, r.top() + 14, QString("%1%").arg((int)llround(c.opacity * 100.0)));
        p.restore();
    }
}

void TimelineWidget::drawTransitionIndicator(QPainter& p, const QRect& r,
                                             const QString& type) {
    if (r.width() < 3 || r.height() < 3) return;
    p.fillRect(r, QColor(255, 170, 40, 70));
    p.setPen(QPen(QColor(255, 190, 80, 120), 1));
    const int step = 5;
    for (int x = r.left() - r.height(); x < r.right() + r.height(); x += step)
        p.drawLine(x, r.bottom(), x + r.height(), r.top());
    QFont f = p.font();
    f.setPointSizeF(8.5);
    f.setBold(true);
    p.setFont(f);
    QFontMetrics fm(f);
    QString glyph;
    if (type == QStringLiteral("wipeleft"))
        glyph = QStringLiteral("\u2190");
    else if (type == QStringLiteral("wiperight"))
        glyph = QStringLiteral("\u2192");
    else if (type == QStringLiteral("wipeup"))
        glyph = QStringLiteral("\u2191");
    else if (type == QStringLiteral("wipedown"))
        glyph = QStringLiteral("\u2193");
    else if (type == QStringLiteral("wipetl"))
        glyph = QStringLiteral("\u2196");
    else if (type == QStringLiteral("wipetr"))
        glyph = QStringLiteral("\u2197");
    else if (type == QStringLiteral("wipebl"))
        glyph = QStringLiteral("\u2199");
    else if (type == QStringLiteral("wipebr"))
        glyph = QStringLiteral("\u2198");
    else
        glyph = QStringLiteral("\u2715");
    const QRect symRect(r.left(), r.top(), r.width(), fm.height());
    p.setPen(QColor(255, 220, 140));
    p.drawText(symRect, Qt::AlignHCenter | Qt::AlignTop, glyph);
}

void TimelineWidget::drawKeyframeDiamonds(QPainter& p, const QRect& r,
                                          const Clip& c, bool audio) {
    struct KfSet { const QVector<Keyframe>* keys; QColor color; };
    QVector<KfSet> sets;
    if (audio) {
        sets.append(KfSet{&c.kfVolume, QColor(110, 235, 185)});
    } else {
        sets.append(KfSet{&c.kfOpacity, QColor(255, 255, 255)});
        sets.append(KfSet{&c.kfScale, QColor(90, 200, 255)});
        sets.append(KfSet{&c.kfScaleX, QColor(80, 220, 220)});
        sets.append(KfSet{&c.kfScaleY, QColor(80, 220, 220)});
        sets.append(KfSet{&c.kfRotation, QColor(210, 150, 255)});
        sets.append(KfSet{&c.kfTx, QColor(255, 200, 90)});
        sets.append(KfSet{&c.kfTy, QColor(255, 160, 90)});
        sets.append(KfSet{&c.kfCropL, QColor(120, 255, 140)});
        sets.append(KfSet{&c.kfCropR, QColor(120, 255, 140)});
        sets.append(KfSet{&c.kfCropT, QColor(120, 255, 140)});
        sets.append(KfSet{&c.kfCropB, QColor(120, 255, 140)});
    }
    const double dur = std::max(c.dur, kMinDur);
    int lane = 0;
    for (const KfSet& s : sets) {
        if (!s.keys || s.keys->isEmpty()) continue;
        const int laneY = r.bottom() - (audio ? 20 : 22) - (lane % 2) * 10;
        for (const Keyframe& k : *s.keys) {
            const int x = r.left() + (int)std::lround(
                std::clamp(k.time, 0.0, dur) / dur * r.width());
            const QPolygonF dia = QPolygonF()
                << QPointF(x, laneY - 4) << QPointF(x + 4, laneY)
                << QPointF(x, laneY + 4) << QPointF(x - 4, laneY);
            p.setPen(QPen(themeColors().trackBorder, 1));
            p.setBrush(s.color);
            p.drawPolygon(dia);
        }
        ++lane;
    }
}

void TimelineWidget::drawTrackHeader(QPainter& p, int y, int rowH, const Track& tr, int index, bool selected) {
    const int H = kHeaderW;
    const QColor tcol = trackColorAt(tr, index);
    bool anySolo = false;
    if (m_project) {
        for (const Track& t : m_project->videoTracks) if (t.solo) { anySolo = true; break; }
        if (!anySolo)
            for (const Track& t : m_project->audioTracks) if (t.solo) { anySolo = true; break; }
    }

    // ── Fundo: neutro escuro, como os cabeçalhos do Premiere (sem tinta por
    // faixa). A cor da faixa fica nos acentos (tira, nome, %, chip FX).
    QColor base = selected
        ? (tr.audio ? QColor(38, 43, 54) : QColor(40, 45, 56))
        : themeColors().trackLabelBg;
    if (tr.locked) base = selected ? QColor(50, 41, 41) : QColor(40, 37, 37);
    p.fillRect(0, y, H, rowH, base);

    // Left accent: cor da faixa em tira (3px) — contido, estilo Premiere;
    // seleção em ciano por cima; lock em vermelho suave.
    if (tr.locked) {
        p.fillRect(0, y, 3, rowH, QColor(126, 82, 82));
    } else if (selected) {
        p.fillRect(0, y, 3, rowH, themeColors().accent);
    } else {
        p.fillRect(0, y, 3, rowH, tcol);
    }

    // ── Layout proporcional ─────────────────────────────────────────────
    const int resizeH = kResizeHandleH;  // 5px
    const int btnH = 18;
    const int btnGap = 3;
    const int btnY = y + rowH - resizeH - btnH;  // botões na base (acima do resize)
    const int contentBottom = btnY - 4;           // fim da área de conteúdo (acima dos botões)
    const int contentTop = y + 18;                // abaixo do ícone/nome (18px para ícone+nome)
    const int contentH = contentBottom - contentTop;

    // ── Nome da track ───────────────────────────────────────────────────
    QFont basef = p.font();
    QFont f = basef;
    f.setBold(true);
    f.setPointSizeF(8.5);
    p.setFont(f);
    p.setPen(themeColors().trackLabelText);
    p.drawText(QRect(12, y + 2, H - 20, 16), Qt::AlignLeft | Qt::AlignVCenter, tr.name);

    // ── Ícone (audio/vídeo) na cor da faixa ────────────────────────────
    p.setRenderHint(QPainter::Antialiasing, true);
    if (tr.audio) {
        const QColor c = tr.locked ? QColor(178, 132, 132) : tcol.lighter(135);
        QPainterPath sp;
        sp.moveTo(4.5, y + 8);
        sp.lineTo(9.0, y + 5);
        sp.lineTo(9.0, y + 11);
        sp.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(sp);
        p.fillRect(QRectF(9, y + 6.5, 4.5, 3), c);
        p.setPen(QPen(c, 1));
        p.drawArc(QRectF(11.5, y + 5, 4.5, 6), 0, 180 * 16);
    } else {
        const QColor c = tr.locked ? QColor(178, 132, 132) : tcol.lighter(135);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(3, y + 4.5, 10.5, 7), 1.6, 1.6);
        p.setBrush(QColor(22, 24, 28));
        p.drawEllipse(QPointF(8.2, y + 8), 2.3, 2.3);
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setFont(basef);

    // ── Percentual + barra (centralizado na área de conteúdo) ───────────
    if (contentH > 0) {
        QFont vf = basef;
        vf.setPointSizeF(7.5);
        vf.setBold(true);
        p.setFont(vf);

        if (tr.audio) {
            // Áudio: percentual de volume centralizado na área disponível.
            const QString pct = QString("%1%").arg((int)llround(tr.volume * 100.0));
            p.setPen(tcol.lighter(150));
            p.drawText(QRect(6, contentTop, H - 12, contentH),
                       Qt::AlignRight | Qt::AlignVCenter, pct);
        } else {
            // Vídeo: percentual + barra de opacidade.
            const QString pct = QString("%1%").arg((int)llround(tr.opacity * 100.0));
            p.setPen(tcol.lighter(150));
            // Texto na metade de cima da área de conteúdo.
            const int textH = contentH / 2;
            p.drawText(QRect(6, contentTop, H - 12, textH),
                       Qt::AlignRight | Qt::AlignVCenter, pct);
            // Barra na metade de baixo (se couber).
            if (contentH >= 8) {
                const int barH = 4;
                const int barY = contentTop + textH + (contentH - textH - barH) / 2;
                const int barX0 = 6;
                const int barW = H - 12;
                p.setPen(Qt::NoPen);
                p.setBrush(themeColors().trackBorder);
                p.drawRoundedRect(QRectF(barX0, barY, barW, barH), 2, 2);
                const int fillW = qMax(2, (int)std::lround(barW * std::clamp(tr.opacity, 0.0, 1.0)));
                p.setBrush(themeColors().clipBorder);
                p.drawRoundedRect(QRectF(barX0, barY, fillW, barH), 2, 2);
            }
        }
        p.setFont(basef);
    }

    // ── Botões M / S / L ────────────────────────────────────────────────
    const bool audible = !tr.muted && !(anySolo && !tr.solo);
    const QColor dim(128, 128, 138);
    const int size = 18;
    const int bx0 = 6;
    auto drawBtn = [&](int idx, const QString& label, bool active, const QColor& on) {
        const int bx = bx0 + idx * (size + btnGap);
        const QRect r(bx, btnY, size, size);
        p.setPen(QPen(active ? on.lighter(140) : themeColors().trackBorder, 1));
        p.setBrush(active ? on : themeColors().trackLabelBg);
        p.drawRect(r);
        p.setPen(active ? QColor(255, 255, 255) : dim);
        QFont bf = basef;
        bf.setBold(true);
        bf.setPointSizeF(7.5);
        p.setFont(bf);
        p.drawText(r, Qt::AlignCenter, label);
        p.setFont(basef);
    };
    drawBtn(0, QStringLiteral("M"), tr.muted || !audible, QColor(84, 118, 178));
    drawBtn(1, QStringLiteral("S"), tr.solo, QColor(72, 150, 176));
    drawBtn(2, QStringLiteral("L"), tr.locked, QColor(96, 108, 176));
    if (tr.audio) {
        // Chip FX (estilo Vegas): abre o menu de efeitos de áudio da faixa.
        // Acende em azul quando há efeito ativo.
        const int fxIdx = 3;
        const int fx = bx0 + fxIdx * (size + btnGap);
        const QRect r(fx, btnY, size, size);
        const bool hasFx = tr.hasAudioFx();
        p.setPen(QPen(hasFx ? QColor(120, 160, 214) : themeColors().trackBorder, 1));
        p.setBrush(hasFx ? QColor(70, 104, 156) : themeColors().trackLabelBg);
        p.drawRect(r);
        p.setPen(hasFx ? QColor(226, 236, 255) : dim);
        QFont bf = basef;
        bf.setBold(true);
        bf.setPointSizeF(7.5);
        p.setFont(bf);
        p.drawText(r, Qt::AlignCenter, QStringLiteral("FX"));
        p.setFont(basef);
    }

    // ── Alça de redimensionamento ────────────────────────────────────────
    const int gy0 = y + rowH - resizeH;
    p.fillRect(0, gy0, H, resizeH, themeColors().trackLabelBg);
    p.setPen(themeColors().rulerTickMajor);
    const int gx0 = (H - 26) / 2;
    for (int i = 0; i < 4; ++i)
        p.drawLine(gx0 + i * 8, gy0 + 2, gx0 + i * 8, gy0 + 3);
}

void TimelineWidget::drawFolderStrip(QPainter& p, const TrackGroup& g) {
    const QRect r = folderStripRect(g);
    if (r.isEmpty()) return;
    const bool isMesa = !g.mesaId.isEmpty();
    p.setPen(Qt::NoPen);
    // Mesa groups: teal/dark cyan; regular groups: brown/amber
    if (isMesa) {
        p.setBrush(QColor(50, 110, 120));
        p.drawRect(r);
        p.setPen(QColor(38, 85, 95));
        p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());
    } else {
        p.setBrush(QColor(150, 118, 60));
        p.drawRect(r);
        p.setPen(QColor(120, 92, 46));
        p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());
    }
    const QRect ar = folderArrowRect(g);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(isMesa ? QColor(140, 220, 230) : QColor(235, 220, 180));
    p.setPen(Qt::NoPen);
    QPolygon tri;
    const int cx = ar.center().x();
    const int cy = ar.center().y();
    if (g.collapsed)
        tri << QPoint(cx - 4, cy - 5) << QPoint(cx + 3, cy) << QPoint(cx - 4, cy + 5);
    else
        tri << QPoint(cx - 5, cy - 4) << QPoint(cx + 5, cy - 4) << QPoint(cx, cy + 4);
    p.drawPolygon(tri);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setBrush(isMesa ? QColor(100, 190, 200) : QColor(224, 192, 112));
    p.drawRect(QRect(r.left() + 24, r.top() + 5, 11, 5));
    p.drawRect(QRect(r.left() + 22, r.top() + 8, 17, 12));
    p.setPen(isMesa ? QColor(200, 240, 245) : QColor(245, 235, 210));
    QFont f = p.font();
    f.setBold(true);
    f.setPointSizeF(8.5);
    p.setFont(f);
    p.drawText(QRect(r.left() + 46, r.top(), r.width() - 54, kFolderH),
               Qt::AlignLeft | Qt::AlignVCenter, g.name);
}
