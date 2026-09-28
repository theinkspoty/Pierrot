// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TimelineWidget.h"
#include "colombina/models/Project.h"
#include "ui/SettingsDialog.h"
#include "colombina/ffmpeg/MediaCache.h"
#include "ui/TlLog.h"
#include "ui/Theme.h"

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

namespace {
constexpr int kHeaderW = 130;
constexpr int kRulerH = 26;
constexpr int kZoomW = 64;
constexpr int kFolderH = 22;
constexpr int kResizeHandleH = 5;
constexpr int kVideoRowH = 56;
constexpr int kAudioRowH = 56; // padrão: alto o bastante p/ a barra de volume
constexpr int kHeaderBtnH = 18;
constexpr int kHeaderNameH = 18;
constexpr int kMinRowH = 24;
constexpr int kMaxRowH = 400;
constexpr double kMinPps = 2.0;
constexpr double kMaxPps = 4000.0;
constexpr double kMinDur = 0.04;

enum Tool {
    ToolSelect = 0, ToolMove = 1, ToolScissors = 2, ToolEnvelope = 3, ToolZoom = 4,
    ToolRipple = 5, ToolRolling = 6, ToolSlip = 7, ToolSlide = 8, ToolRateStretch = 9
};

// Vermelho da seção de gravação (divisória e destaque de seleção). O
// `Track::color` das faixas de gravação já vem vermelho do modelo.
QColor recColor() { return recordingTrackColor(); }

// Qual das três seções da timeline um row/faixa pertence.
enum class Zone { Video, Audio, Rec };

QString fmtRuler(double t) {
    const int total = (int)std::floor(t);
    return QString("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Paleta automática de cores de faixa de VÍDEO (estilo Vegas), usada quando
// a faixa não tem cor própria definida pelo usuário.
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

// Paleta automática de cores de faixas de ÁUDIO (labels padrão do Adobe
// Premiere), usada quando a faixa não tem cor própria definida pelo usuário.
// A1 verde, A2 verde-água, A3 azul, A4 violeta, A5 âmbar, A6 cinza.
QColor autoAudioColor(int index) {
    static const QColor pal[] = {
        QColor(80, 168, 94),   // geo
        QColor(62, 152, 168),  // verde-água
        QColor(84, 112, 190),  // azul
        QColor(132, 100, 178), // violeta
        QColor(180, 136, 68),  // âmbar
        QColor(122, 132, 142), // cinza
    };
    return pal[index % (int)(sizeof pal / sizeof pal[0])];
}

// Cor efetiva de uma faixa para desenho (própria se definida, senão paleta).
QColor trackColorAt(const Track& tr, int index) {
    if (tr.color.isValid()) return tr.color;
    return tr.audio ? autoAudioColor(index) : autoTrackColor(index);
}

// Cor sólida de uma faixa de ÁUDIO no matiz/saturação do label, com a
// luminosidade `val` fornecida (estilo Premiere: pista/clipe/header pintados
// na cor da faixa em tons sólidos, nunca em cinza escuro).
QColor audioSolidColor(const Track& tr, int index, qreal val) {
    QColor c = trackColorAt(tr, index);
    qreal hue = c.hslHueF();
    if (hue < 0.0) hue = 0.36;
    qreal sat = c.hslSaturationF();
    if (sat <= 0.0) sat = 0.65;
    return QColor::fromHslF(hue, sat, val);
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
    // Faixa recolhida (seta do cabeçalho, estilo Premiere): altura mínima.
    if (list[idx].collapsed) return kMinRowH;
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
    // rowY(-1, 0) = fundo da seção de vídeo, já contando a barra de pasta
    // que possa abrir a seção de áudio. Gravação é sempre a última seção.
    int y = rowY(-1, 0) + 2; // +2: barra divisória da seção
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
    // Busca binária: clips são ordenados por pos. Encontra o último cujo
    // início é <= t.
    int lo = 0, hi = clips.size() - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (clips[mid].pos <= t) lo = mid + 1;
        else hi = mid - 1;
    }
    if (hi >= 0 && hi < clips.size()) {
        Clip& c = const_cast<Clip&>(clips[hi]);
        if (t >= c.pos && t < c.pos + c.dur) return &c;
    }
    return nullptr;
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
        // A pista vazia fica escura (como no Premiere); a cor do label é
        // aplicada apenas nas faixas (clipes de áudio) e no header.
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
            p.fillRect(0, y, width(), rowH, sel ? QColor(46, 40, 44)
                                                : ((i % 2) ? themeColors().trackBg
                                                           : themeColors().trackBgAlt));
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
            QRect r(cx + 1, y + 4, cw - 2, rowH - 8);
            if (r.width() <= 0 || r.height() <= 0) continue;
            if (r.right() < H || r.left() > width()) continue;
            drawClip(p, r, c, tr, i, audio);
        }
    };
    drawClips(m_project->videoTracks, Zone::Video);
    drawClips(m_project->audioTracks, Zone::Audio);
    drawClips(m_project->recordingTracks, Zone::Rec);

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
    const QColor tint = trackColorAt(tr, trackIndex);
    QColor fill;
    QColor border;
    if (audio) {
        // Clipe de áudio estilo Adobe Premiere: corpo sólido na cor do label
        // (verde por padrão), borda mais clara na mesma cor. Seleção = borda
        // ciano + corpo mais claro. Não é clipe escuro com onda colorida
        // (Vegas).
        fill = sel ? audioSolidColor(tr, trackIndex, 0.50)
                   : sel2 ? audioSolidColor(tr, trackIndex, 0.34)
                          : audioSolidColor(tr, trackIndex, 0.38);
        border = sel ? themeColors().clipBorderSelect
                     : sel2 ? themeColors().clipBorderSecondary
                            : audioSolidColor(tr, trackIndex, 0.55);
    } else {
        fill = sel ? QColor(46, 96, 168) : themeColors().clipBg;
        border = sel ? themeColors().clipBorderSelect
                     : sel2 ? themeColors().clipBorderSecondary
                            : themeColors().clipBorder;
    }
    p.setPen(QPen(border, sel ? 2 : sel2 ? 1.5 : 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, 3, 3);

    const MediaItem* mi = m_project ? m_project->findMedia(c.mediaId) : nullptr;
    const QString path = mi ? mi->filePath : QString();
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

    // Alças de seleção estilo Premiere: quadrados brancos nos cantos de
    // clipes de áudio selecionados (apenas visuais; o redimensionar é
    // feito pelas bordas/ferramentas).
    if (audio && sel && r.width() >= 28 && r.height() >= 18) {
        const int hs = 5;
        QColor hc(255, 255, 255);
        p.fillRect(QRect(r.left() + 1, r.top() + 1, hs, hs), hc);
        p.fillRect(QRect(r.right() - hs, r.top() + 1, hs, hs), hc);
        p.fillRect(QRect(r.left() + 1, r.bottom() - hs, hs, hs), hc);
        p.fillRect(QRect(r.right() - hs, r.bottom() - hs, hs, hs), hc);
    }

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
    const qreal hue = tint.hslHueF() < 0.0 ? 0.36 : tint.hslHueF();
    const qreal sat = tint.hslSaturationF() > 0.0 ? tint.hslSaturationF() : 0.65;

    MediaCache& cache = MediaCache::instance();
    if (!cache.hasPeaks(path, c.audioStreamIndex)) {
        cache.requestPeaks(path, c.audioStreamIndex);
        p.fillRect(r, QColor::fromHslF(hue, sat, 0.38));
        p.setPen(QColor(10, 10, 12, 80));
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
    const double amp = qMax(1.0, r.height() / 2.0 - 4.0);
    const bool sel = isSelected(c.id);
    const bool sel2 = !sel && isSecondarySelected(c.id);

    // Corpo do clipe = cor sólida do label, ton escuro (Premiere); a onda clara
    // fica em destaque por cima.
    p.fillRect(r, sel ? QColor::fromHslF(hue, sat, 0.50)
                      : sel2 ? QColor::fromHslF(hue, sat, 0.34)
                             : QColor::fromHslF(hue, sat, 0.38));

    // Agrega min/max por coluna de pixel e descobre o pico global do clipe,
    // para normalizar a altura (autogain) como o Premiere faz.
    const auto colMinMax = [&](int x, float& mn, float& mx) {
        mn = 0.0f;
        mx = 0.0f;
        const double t0 = c.in + (x - x0) * dur / (double)(x1 - x0 + 1);
        const double t1 = c.in + (x + 1 - x0) * dur / (double)(x1 - x0 + 1);
        int b0 = (int)std::floor(t0 * bps);
        int b1 = (int)std::floor(t1 * bps);
        if (b0 < 0) b0 = 0;
        if (b1 >= pk.min.size()) b1 = pk.min.size() - 1;
        if (b0 > b1) return;
        for (int b = b0; b <= b1; ++b) {
            if (pk.min[b] < mn) mn = pk.min[b];
            if (pk.max[b] > mx) mx = pk.max[b];
        }
    };

    float gPeak = 0.0f;
    for (int x = x0; x <= x1; ++x) {
        float mn = 0.0f, mx = 0.0f;
        colMinMax(x, mn, mx);
        const float p = std::max(std::fabs(mx), std::fabs(mn));
        if (p > gPeak) gPeak = p;
    }
    const double gain = gPeak > 1e-4 ? (0.92 / gPeak) : 1.0;

    // Onda em tom escuro do label, alpha cresce com a amplitude. Sem grade
    // dB, sem linha zero, sem marcadores de tempo — fidelidade ao Premiere.
    // Onda BEM clara (tom quase branco no matiz do label), alpha cresce com a
    // amplitude — destaque forte sobre o fundo colorido, ideal para cortes.
    const qreal waveVal = 0.90;

    for (int x = x0; x <= x1; ++x) {
        float mn = 0.0f, mx = 0.0f;
        colMinMax(x, mn, mx);
        if (mx <= 1e-4f && mn >= -1e-4f) continue;

        const int py0 = (int)std::lround(midY - mx * amp * gain);
        const int py1 = (int)std::lround(midY - mn * amp * gain);
        const int top = qBound(r.top(), py0, r.bottom());
        const int bot = qBound(r.top(), py1, r.bottom());
        if (top >= bot) continue;

        const float norm = std::clamp((float)(std::max(std::fabs(mx), std::fabs(mn)) * gain),
                                      0.0f, 1.0f);
        const qreal alph = qBound(0.55, 0.68 + 0.30 * norm, 1.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor::fromHslF(hue, sat, waveVal, alph));
        // Coluna sólida de -pico a +pico, espelhada em torno do zero, como o
        // waveform "unificado" (mono/estéreo somados) do Premiere.
        p.drawRect(x, top, 1, bot - top + 1);
    }
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

    // ── Fundo: gradiente sutil branco→transparente (estilo Premiere/Vegas).
    // Sem risquinhos diagonais — limpo e profissional.
    {
        QLinearGradient g(r.topLeft(), r.bottomRight());
        g.setColorAt(0.0, QColor(255, 255, 255, 55));
        g.setColorAt(0.5, QColor(255, 255, 255, 30));
        g.setColorAt(1.0, QColor(255, 255, 255, 55));
        p.fillRect(r, g);
    }

    // ── Borda arredondada translúcida.
    {
        p.setPen(QPen(QColor(255, 255, 255, 70), 1));
        p.setBrush(Qt::NoBrush);
        QPainterPath borderPath;
        borderPath.addRoundedRect(QRectF(r), 3.0, 3.0);
        p.drawPath(borderPath);
    }

    // ── Linha diagonal separadora (base→topo, única, limpa).
    // Clipped no retângulo da transição.
    {
        p.setClipRect(r, Qt::IntersectClip);
        p.setPen(QPen(QColor(255, 255, 255, 120), 1.5));
        p.drawLine(r.left(), r.bottom(), r.right(), r.top());
        p.setClipping(false);
    }

    // ── Label pequeno centralizado (nome da transição).
    QFont f = p.font();
    f.setPointSizeF(7.5);
    f.setBold(true);
    p.setFont(f);
    QFontMetrics fm(f);
    QString label;
    if (type == QStringLiteral("dissolve"))
        label = tr("Dissolver");
    else if (type == QStringLiteral("wipeleft"))
        label = tr("Wipe \u2190");
    else if (type == QStringLiteral("wiperight"))
        label = tr("Wipe \u2192");
    else if (type == QStringLiteral("wipeup"))
        label = tr("Wipe \u2191");
    else if (type == QStringLiteral("wipedown"))
        label = tr("Wipe \u2193");
    else if (type == QStringLiteral("wipetl"))
        label = tr("Wipe \u2196");
    else if (type == QStringLiteral("wipetr"))
        label = tr("Wipe \u2197");
    else if (type == QStringLiteral("wipebl"))
        label = tr("Wipe \u2199");
    else if (type == QStringLiteral("wipebr"))
        label = tr("Wipe \u2198");
    else
        label = tr("Transição");

    if (r.width() > fm.horizontalAdvance(label) + 10) {
        const QRect lr = fm.boundingRect(label);
        const int lw = lr.width() + 8;
        const int lh = lr.height() + 3;
        const int lx = r.left() + (r.width() - lw) / 2;
        const int ly = r.top() + (r.height() - lh) / 2;
        const QRect labelRect(lx, ly, lw, lh);

        // Fundo do label: escuro translúcido.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 140));
        QPainterPath labelBg;
        labelBg.addRoundedRect(QRectF(labelRect), 2.0, 2.0);
        p.drawPath(labelBg);

        // Texto branco.
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(labelRect, Qt::AlignCenter, label);
    }
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

// Geometria alinhada entre desenho e hit-test.
QRect TimelineWidget::headerBarRect(int y, int rowH) const {
    const int H = kHeaderW;
    const int barH = 4;
    const int btnY = y + rowH - kResizeHandleH - kHeaderBtnH;
    const int contentBottom = btnY - 4;
    const int contentTop = y + kHeaderNameH;
    const int contentH = contentBottom - contentTop;
    if (contentH < 8) return QRect();
    const int textH = contentH / 2;
    int barY = contentTop + textH + (contentH - textH - barH) / 2;
    return QRect(6, barY, H - 12, barH);
}

QRect TimelineWidget::headerToggleRect(int y) const {
    return QRect(kHeaderW - 20, y + 2, 16, 16);
}

QRect TimelineWidget::headerCollapseRect(int y) const {
    return QRect(4, y + 2, 14, 16);
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

    // ── Fundo: neutro escuro, como os cabeçalhos do Premiere; a cor da faixa
    // fica nos acentos (tira, nome, %, chip FX) e nas faixas (clipes).
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

    QFont basef = p.font();
    const QColor iconCol = tr.locked ? QColor(178, 132, 132) : tcol.lighter(135);
    const bool hidden = !tr.visible;

    // ── Cabeçalho compacto (faixa recolhida): só nome + toggle de saída + seta.
    if (tr.collapsed) {
        QFont nf = basef;
        nf.setBold(true);
        nf.setPointSizeF(8.0);
        p.setFont(nf);
        p.setPen(hidden ? QColor(120, 116, 112) : themeColors().trackLabelText);
        p.drawText(QRect(22, y + 1, H - 22 - 20, rowH - 2),
                   int(Qt::AlignLeft | Qt::AlignVCenter) | Qt::TextSingleLine,
                   tr.name);
        p.setFont(basef);

        // Seta de expandir (recolhida → "›").
        const QRect cr2 = headerCollapseRect(y);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath ar2;
        ar2.moveTo(cr2.x() + 2, cr2.y() + 2);
        ar2.lineTo(cr2.x() + 9, cr2.y() + cr2.height() / 2);
        ar2.lineTo(cr2.x() + 2, cr2.y() + cr2.height() - 2);
        ar2.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(themeColors().trackLabelText);
        p.drawPath(ar2);
        p.setRenderHint(QPainter::Antialiasing, false);

        // Toggle de saída (olho/falante).
        const QRect tr2 = headerToggleRect(y);
        drawOutputToggleIcon(p, tr2, tr, iconCol, hidden);
        return;
    }

    // ── Layout proporcional ─────────────────────────────────────────────
    const int resizeH = kResizeHandleH;  // 5px
    const int btnH = kHeaderBtnH;        // 18
    const int btnGap = 3;
    const int btnY = y + rowH - resizeH - btnH;  // botões na base (acima do resize)
    const int contentBottom = btnY - 4;           // fim da área de conteúdo (acima dos botões)
    const int contentTop = y + kHeaderNameH;      // abaixo do ícone/nome (18px para ícone+nome)
    const int contentH = contentBottom - contentTop;

    // ── Seta de recolher ────────────────────────────────────────────────
    const QRect cr = headerCollapseRect(y);
    p.setRenderHint(QPainter::Antialiasing, true);
    {
        QPainterPath ar;
        ar.moveTo(cr.x() + 3, cr.y() + 3);
        ar.lineTo(cr.x() + cr.width() - 3, cr.y() + 3);
        ar.lineTo(cr.x() + cr.width() / 2, cr.y() + cr.height() - 3);
        ar.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(themeColors().trackLabelText);
        p.drawPath(ar);
    }
    p.setRenderHint(QPainter::Antialiasing, false);

    // ── Ícone (audio/vídeo) na cor da faixa, à direita da seta ──────────
    {
        const QColor c = iconCol;
        p.setRenderHint(QPainter::Antialiasing, true);
        const int ix = cr.right() + 5;
        if (tr.audio) {
            QPainterPath sp;
            sp.moveTo(ix, y + 8);
            sp.lineTo(ix + 4.5, y + 5);
            sp.lineTo(ix + 4.5, y + 11);
            sp.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawPath(sp);
            p.fillRect(QRectF(ix + 4.5, y + 6.5, 4.5, 3), c);
            p.setPen(QPen(c, 1));
            p.drawArc(QRectF(ix + 7, y + 5, 4.5, 6), 0, 180 * 16);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(QRectF(ix, y + 4.5, 10.5, 7), 1.6, 1.6);
            p.setBrush(QColor(22, 24, 28));
            p.drawEllipse(QPointF(ix + 5.2, y + 8), 2.3, 2.3);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // ── Nome da track (à direita da seta+ícone, ocupando o resto) ────────
    QFont f = basef;
    f.setBold(true);
    f.setPointSizeF(8.5);
    p.setFont(f);
    p.setPen(hidden ? QColor(120, 116, 112) : themeColors().trackLabelText);
    p.drawText(QRect(cr.right() + 18, y + 2, H - cr.right() - 18 - 20, 16),
               int(Qt::AlignLeft | Qt::AlignVCenter) | Qt::TextSingleLine,
               tr.name);

    // ── Toggle de saída (olho/falante, à direita) ───────────────────────
    {
        const QRect trt = headerToggleRect(y);
        drawOutputToggleIcon(p, trt, tr, iconCol, hidden);
    }

    // ── Percentual + barra (centralizado na área de conteúdo) ───────────
    if (contentH > 0) {
        QFont vf = basef;
        vf.setPointSizeF(7.5);
        vf.setBold(true);
        p.setFont(vf);

        const double val = tr.audio ? std::clamp(tr.volume, 0.0, 2.0) / 2.0
                                    : std::clamp(tr.opacity, 0.0, 1.0);
        QString pct;
        if (tr.audio)
            pct = QString("%1%").arg((int)llround(tr.volume * 100.0));
        else
            pct = QString("%1%").arg((int)llround(val * 100.0));
        p.setPen(hidden ? QColor(120, 116, 112) : tcol.lighter(150));
        // Texto na metade de cima da área de conteúdo.
        const int textH = contentH / 2;
        p.drawText(QRect(6, contentTop, H - 12, textH),
                   Qt::AlignRight | Qt::AlignVCenter, pct);
        // Barra na metade de baixo (se couber).
        if (contentH >= 8) {
            const QRect bar = headerBarRect(y, rowH);
            if (!bar.isEmpty()) {
                p.setPen(Qt::NoPen);
                p.setBrush(themeColors().trackBorder);
                p.drawRoundedRect(QRectF(bar), 2, 2);
                const int fillW = qMax(2, (int)std::lround(bar.width() * val));
                p.setBrush(hidden ? QColor(90, 86, 82) : themeColors().clipBorder);
                p.drawRoundedRect(QRectF(bar.x(), bar.y(), fillW, bar.height()), 2, 2);
            }
        }
        p.setFont(basef);
    }

    // ── Botões M / S / L ────────────────────────────────────────────────
    // Faixas muito baixas (sem área de conteúdo) ficam só com nome + toggle.
    if (contentH > 0) {
    const bool audible = !tr.muted && !(anySolo && !tr.solo);
    const QColor dim(128, 128, 138);
    const int size = btnH;
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
    drawBtn(0, QStringLiteral("M"), (tr.muted || !audible) && !hidden, QColor(84, 118, 178));
    drawBtn(1, QStringLiteral("S"), tr.solo && !hidden, QColor(72, 150, 176));
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
    } // fim do guard de botões (contentH > 0)

    // ── Alça de redimensionamento ────────────────────────────────────────
    const int gy0 = y + rowH - resizeH;
    p.fillRect(0, gy0, H, resizeH, themeColors().trackLabelBg);
    p.setPen(themeColors().rulerTickMajor);
    const int gx0 = (H - 26) / 2;
    for (int i = 0; i < 4; ++i)
        p.drawLine(gx0 + i * 8, gy0 + 2, gx0 + i * 8, gy0 + 3);
}

void TimelineWidget::drawOutputToggleIcon(QPainter& p, const QRect& r, const Track& tr,
                                          const QColor& active, bool hidden) {
    const QColor c = hidden ? QColor(100, 96, 92) : active;
    const QColor bg = tr.audio ? QColor(48, 56, 72) : QColor(44, 50, 62);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(r), 3, 3);
    p.setPen(QPen(hidden ? QColor(100, 96, 92) : active, 1.2));
    p.setBrush(Qt::NoBrush);
    if (tr.audio) {
        // Alto-falante: caixa + cone + ondas.
        QRectF body(r.x() + 2.0, r.y() + 6.0, 4.5, 4.0);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRect(body);
        QPainterPath cone;
        cone.moveTo(body.right(), body.top() + 0.5);
        cone.lineTo(body.right() + 3.5, body.top() - 2.5);
        cone.lineTo(body.right() + 3.5, body.bottom() + 2.5);
        cone.closeSubpath();
        p.drawPath(cone);
        p.setPen(QPen(c, 1));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(body.right() + 3.0, r.y() + 3.0, 7, 10), -52 * 16, 104 * 16);
        p.drawArc(QRectF(body.right() + 6.0, r.y() + 0.5, 8, 15), -60 * 16, 120 * 16);
    } else {
        // Olho: elipse + pupila.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        const double cx = r.x() + r.width() / 2.0;
        const double cy = r.y() + r.height() / 2.0;
        QPainterPath eye;
        eye.moveTo(cx - 5.5, cy);
        eye.quadTo(cx - 5.5, cy - 5, cx + 5.5, cy);
        eye.quadTo(cx + 5.5, cy + 5, cx - 5.5, cy);
        eye.closeSubpath();
        p.drawPath(eye);
        p.setBrush(QColor(16, 18, 22));
        p.drawEllipse(QPointF(cx, cy), 1.7, 1.7);
    }
    if (hidden) {
        // Barra diagonal indicando saída desligada.
        p.setPen(QPen(QColor(214, 100, 90), 1.4));
        p.drawLine(r.x() + 2, r.y() + r.height() - 2, r.x() + r.width() - 2, r.y() + 2);
    }
    p.setRenderHint(QPainter::Antialiasing, false);
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
