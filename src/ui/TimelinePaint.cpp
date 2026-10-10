// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TimelineWidget.h"
#include "colombina/models/Project.h"
#include "ui/SettingsDialog.h"
#include "colombina/ffmpeg/MediaCache.h"
#include "colombina/generators.h"
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

#include <algorithm>
#include <cmath>

namespace {


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

// Texto legível sobre uma cor de fundo: claro -> letra escura, escuro ->
// letra clara. Usado na faixa de nome dos clipes (áudio e vídeo), que é a
// mesma anatomia nos dois.
QColor readableTextOn(const QColor& bg) {
    return bg.lightnessF() > 0.55 ? QColor(0x1A, 0x1A, 0x1A) : QColor(0xEA, 0xEA, 0xEA);
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
        // Premiere: fundo chapado em todas as pistas (vídeo e áudio), sem
        // zebrado — a separação vem da linha divisória no rodapé de cada uma.
        // Seleção = tint translúcido de acento (não cor chapada).
        p.fillRect(0, y, width(), rowH, themeColors().trackBg);
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
        // Premiere: a pista de áudio é um fundo chapado, sem listras
        // alternadas — a separação entre faixas vem da linha divisória
        // sutil desenhada no rodapé de cada uma.
        p.fillRect(0, y, width(), rowH, themeColors().trackBg);
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
        // branca + corpo mais claro. Não é clipe escuro com onda colorida
        // (Vegas).
        fill = sel ? audioSolidColor(tr, trackIndex, 0.50)
                   : sel2 ? audioSolidColor(tr, trackIndex, 0.34)
                          : audioSolidColor(tr, trackIndex, 0.38);
        border = sel ? themeColors().clipBorderSelect
                     : sel2 ? themeColors().clipBorderSecondary
                            : audioSolidColor(tr, trackIndex, 0.55);
    } else {
        // Premiere: clipe selecionado mantém a mesma cor do corpo (um pouco
        // mais clara) e ganha BORDA BRANCA — sem mudança de matiz.
        // Clipe de ajuste (faixa de efeitos): corpo na cor AZUL da faixa,
        // para diferir visualmente dos clipes de mídia normais.
        if (tr.fxTrack) {
            fill = sel ? tint.lighter(128) : tint;
            border = sel ? themeColors().clipBorderSelect
                         : sel2 ? themeColors().clipBorderSecondary
                                : tint.lighter(140);
        } else {
            fill = sel ? themeColors().clipBg.lighter(128) : themeColors().clipBg;
            border = sel ? themeColors().clipBorderSelect
                         : sel2 ? themeColors().clipBorderSecondary
                                : themeColors().clipBorder;
        }
    }
    p.setPen(QPen(border, sel ? 2 : sel2 ? 1.5 : 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, 2, 2);

    const MediaItem* mi = m_project ? m_project->findMedia(c.mediaId) : nullptr;
    const QString path = mi ? mi->filePath : QString();
    const ClipVisKey key{c.id, r.width(), r.height(), m_clipEpoch,
                         tint.rgba(), (float)c.in, (float)c.dur};
    QPixmap content = m_clipPix.value(key);
    if (content.isNull() || content.size() != r.size()) {
        content = QPixmap(r.size());
        content.fill(Qt::transparent);
        QPainter cp(&content);
        const QRect cr(0, 0, r.width(), r.height());
        if (audio)
            drawAudioWaveform(cp, cr, c, path, tint);
        else if (tr.fxTrack) {
            // Clipe de ajuste (faixa de efeitos): corpo azul translúcido com
            // rótulo "Efeitos" — não tem mídia, então não há thumbs/onda.
            cp.fillRect(cr, QColor(tint.red(), tint.green(), tint.blue(), 90));
            cp.setPen(QColor(tint.lighter(140)));
            cp.setFont(QFont(cp.font().family(), 8, QFont::Bold));
            cp.drawText(cr.adjusted(4, 4, -4, -4),
                        Qt::AlignLeft | Qt::AlignTop,
                        QStringLiteral("Efeitos"));
        }
        else if (c.isText)
            drawTextClipBody(cp, cr, c);
        else if (mi && mi->isMesh) {
            // Clipe 3D (.obj): corpo sólido + rótulo (sem thumbs de vídeo).
            cp.fillRect(cr, QColor(50, 70, 100));
            cp.setPen(QColor(140, 180, 220));
            cp.setFont(QFont(cp.font().family(), 8, QFont::Bold));
            cp.drawText(cr.adjusted(4, 4, -4, -4),
                        Qt::AlignLeft | Qt::AlignTop,
                        QStringLiteral("3D"));
        } else if (mi && mi->isSolid) {
            // Mídia gerada (gerador estilo Vegas): desenha o padrão real
            // (cor, gradiente, checkerboard ou ruído) no corpo do clipe.
            const QImage gen = generatorFrame(*mi, cr.width(), cr.height());
            cp.drawImage(cr, gen);
        } else
            drawVideoThumbs(cp, cr, c, path);
        if (!audio)
            drawOpacityHandle(cp, cr, c);
        // Banda de velocidade removida do clip body (atrapalhava o arraste).
        // Edição de velocidade fica no dock Velocidade / menu do clipe.
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
    if (c.hasMulticam()) {
        // Badge do ângulo ativo no playhead (ou no início do clipe, se fora).
        const double rel = std::max(0.0, m_playhead - c.pos);
        const int ang = (m_playhead >= c.pos && m_playhead < c.pos + c.dur)
                            ? c.angleAt(rel) : c.angleAt(0.0);
        label += QString("  \u00b7  MC %1/%2").arg(ang + 1).arg(c.multicamSources.size());
    }
    if (mi && mi->isMesh)
        label += QStringLiteral("  \u00b7  3D");
    if (audio && c.hasAudioFx())
        label += QString("  \u00b7  FX");
    if (std::fabs(c.speed - 1.0) > 1e-4)
        label += QString("  \u00b7  %1\u00d7").arg(c.speed, 0, 'g', 3);
    QFont f = p.font();
    f.setPointSizeF(8.5);
    p.setFont(f);
    QFontMetrics fm(f);
    label = fm.elidedText(label, Qt::ElideRight, std::max(1, r.width() - 10));
    QRect labelRect(r.left() + 3, r.top() + 2, r.width() - 6, fm.height());
    QColor textColor = themeColors().clipText;
    // Mesma anatomia nos dois tipos de clipe (como no Premiere): faixa de nome
    // no topo, na cor da faixa, com o nome por cima. Áudio usa um tom mais
    // claro do corpo; vídeo usa a cor da faixa. Só some quando o clipe é
    // baixo demais para caber.
    {
        const int barH = fm.height() + 4;
        if (r.height() >= barH + 8) {
            const QColor bar = audio ? fill.lighter(155) : tint;
            p.fillRect(QRect(r.left() + 1, r.top() + 1, r.width() - 2, barH), bar);
            textColor = readableTextOn(bar);
        } else {
            p.fillRect(labelRect, QColor(0, 0, 0, 110));
        }
    }
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
    p.setPen(textColor);
    QRect textRect(textX, r.top() + 2, r.right() - 3 - textX, fm.height());
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, label);

    if (!audio) {
        QRect obar(r.left() + 5, r.top() + 22, std::max(1, r.width() - 10), 3);
        const int w = (int)(obar.width() * c.opacity);
        p.fillRect(obar, QColor(0, 0, 0, 110));
        p.fillRect(QRect(obar.x(), obar.y(), w, obar.height()), QColor(255, 255, 255, 190));
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

    // Baseline sempre visível (inclusive em silêncio): a onda não "some"
    // em cortes pequenos com poucos picos nem em corte 100% silêncio.
    p.setPen(QPen(QColor(10, 10, 12, 90), 1));
    p.drawLine(x0, midY, x1, midY);

    if (pk.min.isEmpty()) return;

    // Agrega min/max por coluna de pixel.
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

    // Autogain por percentil p95 em vez do pico máximo: um transiente isolado
    // num corte com muito silêncio não estoura a altura nem faz o resto da
    // onda desaparecer. Fallback: se o p95 for ~0 (corte quase todo silêncio),
    // usa o máximo absoluto para pelo menos um pico aparecer.
    QVector<float> peaks;
    peaks.reserve(x1 - x0 + 1);
    for (int x = x0; x <= x1; ++x) {
        float mn = 0.0f, mx = 0.0f;
        colMinMax(x, mn, mx);
        peaks.append(std::max(std::fabs(mx), std::fabs(mn)));
    }
    std::sort(peaks.begin(), peaks.end());
    float gPeak = 0.0f;
    if (!peaks.isEmpty()) {
        const qsizetype idx = qMin<qsizetype>(peaks.size() - 1,
                                              (qsizetype)(peaks.size() * 0.95));
        gPeak = peaks[idx];
        if (gPeak < 1e-4f) gPeak = peaks.last();
    }
    const double gain = gPeak > 1e-4 ? (0.92 / gPeak) : 1.0;

    // Onda em tom CLARO do matiz do clipe, quase opaca (Premiere: waveform
    // claro sobre o corpo sólido, sem transparência crescendo por amplitude).
    const qreal waveVal = 0.78;

    for (int x = x0; x <= x1; ++x) {
        float mn = 0.0f, mx = 0.0f;
        colMinMax(x, mn, mx);
        const float absPeak = std::max(std::fabs(mx), std::fabs(mn));
        // Coluna de silêncio: não desenha barra (a baseline já cobre); a onda
        // contínua evita "buracos" em cortes pequenos com silêncio.
        if (absPeak <= 1e-4f) continue;

        const int top = qBound(r.top(), (int)std::lround(midY - mx * amp * gain),
                               r.bottom());
        const int bot = qBound(r.top(), (int)std::lround(midY - mn * amp * gain),
                               r.bottom());
        if (top >= bot) continue;

        const float norm = std::clamp(absPeak * (float)gain, 0.0f, 1.0f);
        const qreal alph = qBound(0.80, 0.86 + 0.14 * norm, 1.0);
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

void TimelineWidget::drawSpeedEnvelope(QPainter& p, const QRect& r, const Clip& c) {
    // Time Remapping Premiere: banda de velocidade no corpo do clipe.
    // Y: 0..4× (4× no topo, 0 embaixo); 1× é a linha de referência.
    // Mostra sempre em vídeo (sutil); destaque quando há envelope ou speed≠1.
    const double maxV = 4.0;
    const bool envelope = !c.kfSpeed.isEmpty();
    const bool active = envelope || std::fabs(c.speed - 1.0) > 1e-3;

    // Faixa vertical no terço inferior do clipe (não conflita com o handle
    // de opacidade no topo).
    const int bandTop = r.top() + (r.height() * 2) / 5;
    const int bandBot = r.bottom() - 8;
    if (bandBot - bandTop < 12) return;
    const QRect band(r.left() + 2, bandTop, r.width() - 4, bandBot - bandTop);

    auto speedToY = [&](double v) {
        const double cl = std::clamp(v, 0.0, maxV) / maxV;
        return band.bottom() - int(cl * (band.height() - 2)) - 1;
    };

    // Fundo semi-transparente da banda.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, active ? 70 : 35));
    p.drawRect(band);

    // Linha de referência 1×.
    {
        const int y1 = speedToY(1.0);
        p.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::DashLine));
        p.drawLine(band.left(), y1, band.right(), y1);
    }

    // Curva de velocidade.
    QPainterPath path;
    const int steps = std::max(2, band.width());
    for (int i = 0; i <= steps; ++i) {
        const double rel = (double)i / steps * c.dur;
        const double v = clipSpeedAt(c, rel);
        const int x = band.left() + int((double)i / steps * band.width());
        const int y = speedToY(v);
        if (i == 0) path.moveTo(x, y);
        else path.lineTo(x, y);
    }
    // Preenchimento sob a curva.
    QPainterPath fill = path;
    fill.lineTo(band.right(), band.bottom());
    fill.lineTo(band.left(), band.bottom());
    fill.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(38, 128, 235, active ? 55 : 28)); // azul Adobe
    p.drawPath(fill);
    p.setPen(QPen(QColor(38, 128, 235, active ? 220 : 120), 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // Keyframes (losangos) + rótulos rápido/baixo.
    QFont f = p.font();
    f.setPointSizeF(7.5);
    f.setBold(true);
    p.setFont(f);
    for (const Keyframe& k : c.kfSpeed) {
        if (k.time < -1e-6 || k.time > c.dur + 1e-6) continue;
        const int x = band.left() + int(k.time / std::max(c.dur, 1e-6) * band.width());
        const int y = speedToY(k.value);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1));
        p.setBrush(QColor(38, 128, 235));
        const int rr = 4;
        QPolygon diamond;
        diamond << QPoint(x, y - rr) << QPoint(x + rr, y)
                << QPoint(x, y + rr) << QPoint(x - rr, y);
        p.drawPolygon(diamond);

        const QString pct = QString::number(int(std::lround(k.value * 100))) + QStringLiteral("%");
        const QString tag = k.value > 1.05 ? tr("Rápido")
                            : k.value < 0.95 ? tr("Baixo")
                            : QString();
        QString txt = pct;
        if (!tag.isEmpty()) txt += QStringLiteral(" · ") + tag;
        p.setPen(QColor(255, 255, 255, 210));
        p.drawText(QRect(x - 28, y - 16, 56, 12), Qt::AlignCenter, txt);
    }

    // Rótulo da velocidade base se não há envelope (uma vez, no centro).
    if (!envelope && std::fabs(c.speed - 1.0) > 1e-3) {
        const QString tag = c.speed > 1.0 ? tr("Rápido") : tr("Baixo");
        p.setPen(QColor(255, 255, 255, 180));
        p.drawText(band, Qt::AlignCenter,
                   QString("%1× · %2").arg(c.speed, 0, 'g', 3).arg(tag));
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

// ── Geometria dos controles do cabeçalho (desenho e hit-test usam a mesma) ──
// Layout réplica do Premiere (CC 2018+, tema escuro "Main"):
//   • Linha superior de controles (y+2, 18px): recolher, navegação de
//     keyframes (◀ ◆ ▶), sync lock (chip; sem backend), toggle de saída
//     (olho/falante) e, no áudio, os botões M / S / R (gravação de voz).
//   • Abaixo da linha (y+21): o NOME da faixa à esquerda e o CADEADO (lock)
//     à direita, como no Premiere (nome entre os controles e o cadeado).
//   • Áudio ainda tem o VU meter vertical na borda direita (da linha do nome
//     ao fim), como os meters de faixa do Premiere. Vídeo não tem meter.
//   • Faixa recolhida/baixa vira uma tira única: nome + olho/falante + cadeado.
// A largura das alças e o cursor (kResizeHandleH) fecham o layout.

// Cadeado pequeno do cabeçalho (definido abaixo de drawTrackHeader).
static void drawTrackLockIcon(QPainter& p, const QRect& r, bool locked);

// Linha superior: cada slot tem x fixo (a largura total cobre a linha do
// áudio, cujos M/S/R terminam em 148; cadeado e meter ficam na borda direita).
QRect TimelineWidget::headerBtnRect(int y, int slot) const {
    const int yTop = y + 2;
    // Tudo tem de caber em kHeaderW (150): o slot 8 (R) terminava em 164 e
    // vazava para a área de clipes. Layout compacto, com folga de 3px à
    // direita — o VU meter fica na linha de baixo (y+21), sem conflito.
    switch (slot) {
      case 0: return QRect(2,  yTop, 14, 18); // recolher/expandir
      case 1: return QRect(18, yTop, 15, 18); // keyframe anterior
      case 2: return QRect(33, yTop, 15, 18); // adicionar/remover keyframe
      case 3: return QRect(48, yTop, 15, 18); // próximo keyframe
      case 4: return QRect(66, yTop, 15, 18); // sync lock (sem backend)
      case 5: return QRect(84, yTop, 18, 18); // toggle de saída (olho/falante)
      case 6: return QRect(105, yTop, 14, 18);// M (áudio)
      case 7: return QRect(119, yTop, 14, 18);// S (áudio)
      case 8: return QRect(133, yTop, 14, 18);// R (áudio; sem backend)
    }
    return QRect();
}

// Nome da faixa, na linha abaixo dos controles (como o nome no Premiere) —
// também posiciona o editor inline de nome. Reserva o cadeado à direita.
QRect TimelineWidget::headerNameRect(int y) const {
    return QRect(6, y + 21, kHeaderW - 28 - 6, 16);
}

// Cadeado: fim da faixa do nome (borda direita, antes do VU meter do áudio).
QRect TimelineWidget::headerLockRect(int y) const {
    return QRect(kHeaderW - 28, y + 21, 18, 16);
}

// ── Faixa recolhida (tira única): seta + nome + olho/falante + cadeado ──────
QRect TimelineWidget::headerMiniNameRect(int y) const {
    return QRect(18, y + 3, kHeaderW - 64, 16);
}
QRect TimelineWidget::headerMiniToggleRect(int y) const {
    return QRect(kHeaderW - 46, y + 3, 18, 18);
}
QRect TimelineWidget::headerMiniLockRect(int y) const {
    return QRect(kHeaderW - 28, y + 3, 18, 16);
}

// VU meter vertical do áudio, na borda direita (da linha do nome ao fim).
QRect TimelineWidget::headerMeterRect(int y, int rowH) const {
    const int top = y + 21;
    const int bottom = y + rowH - kResizeHandleH;
    if (bottom - top < 8) return QRect();
    return QRect(kHeaderW - 6, top, 4, bottom - top);
}

// Seta de recolher/expandir do cabeçalho (▾ expandida, › recolhida).
static void drawCollapseArrow(QPainter& p, const QRect& r, bool collapsed) {
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xD3, 0xD3, 0xD3));
    QPainterPath ar;
    if (collapsed) {
        ar.moveTo(r.x() + 2, r.y() + 3);
        ar.lineTo(r.x() + 9, r.y() + r.height() - 3);
        ar.lineTo(r.x() + 2, r.y() + r.height() - 3);
    } else {
        ar.moveTo(r.x() + 3, r.y() + 3);
        ar.lineTo(r.x() + r.width() - 4, r.y() + 3);
        ar.lineTo(r.x() + (r.width() - 1) / 2, r.y() + r.height() - 4);
    }
    ar.closeSubpath();
    p.drawPath(ar);
    p.setRenderHint(QPainter::Antialiasing, false);
}

void TimelineWidget::drawTrackHeader(QPainter& p, int y, int rowH, const Track& tr, int index, bool selected) {
    const int H = kHeaderW;

    // ── Fundo do cabeçalho (cinza do Premiere): a seleção acende no azul do
    // tema (o indicador principal da faixa ativa), sem pintar a pista.
    const QColor base = QColor(0x2B, 0x2B, 0x2C);
    p.fillRect(0, y, H, rowH, base);
    if (selected) {
        QColor selTint = themeColors().accent;
        selTint.setAlpha(140);
        p.fillRect(0, y, H, rowH, selTint);
    }
    p.setPen(QColor(0x15, 0x15, 0x15));
    p.drawLine(0, y + rowH - 1, H, y + rowH - 1); // separador entre faixas

    QFont basef = p.font();
    const bool hidden = !tr.visible;
    (void)index; // cabeçalho do Premiere é monocromático (sem cor por índice)

    const int contentH = rowH - kResizeHandleH;

    // ── Faixa recolhida / baixa: tira única (seta + nome + toggle + cadeado) ─
    if (tr.collapsed || contentH < 22) {
        drawCollapseArrow(p, headerBtnRect(y, 0), tr.collapsed);

        QFont nf = basef;
        nf.setBold(true);
        nf.setPointSizeF(8.5);
        p.setFont(nf);
        p.setPen(hidden ? QColor(0x66, 0x66, 0x66) : QColor(0xD6, 0xD6, 0xD6));
        const QString nm = p.fontMetrics().elidedText(
            tr.name, Qt::ElideRight, headerMiniNameRect(y).width());
        p.drawText(headerMiniNameRect(y), int(Qt::AlignLeft | Qt::AlignVCenter), nm);
        p.setFont(basef);

        drawOutputToggleIcon(p, headerMiniToggleRect(y), tr, QColor(0xD0, 0xD0, 0xD0), hidden);
        drawTrackLockIcon(p, headerMiniLockRect(y), tr.locked);
        return;
    }

    // Cores dos controles (Premiere): chips #333/#4C, glifos claros; quando
    // ativo M/S acendem no azul do Premiere com legenda branca.
    const QColor chipBg(0x33, 0x33, 0x33);
    const QColor chipBd(0x4C, 0x4C, 0x4C);
    const QColor glyph(0xC4, 0xC4, 0xC4);
    const QColor glyphDim(0x55, 0x55, 0x55);
    const QColor activeBlue(0x4A, 0x6F, 0xA5);

    // Keyframes do envelope da faixa (só áudio tem kfVolume; vídeo não tem
    // envelope de faixa → navegação fica apagada, como sem keyframes).
    const QVector<Keyframe>& kfs = tr.kfVolume;
    const bool canKf = tr.audio;
    const double playT = m_playhead;
    int prevKf = -1, nextKf = -1;
    bool atKf = false;
    if (canKf) {
        for (int i = 0; i < kfs.size(); ++i) {
            const double t = kfs[i].time;
            if (std::fabs(t - playT) < 1e-3) atKf = true;
            if (t < playT - 1e-3 && (prevKf < 0 || t > kfs[prevKf].time)) prevKf = i;
            if (t > playT + 1e-3 && (nextKf < 0 || t < kfs[nextKf].time)) nextKf = i;
        }
    }

    // ── Faixa fina do topo (seta + keyframes ◀ ◆ ▶ + sync lock) ──────────
    drawCollapseArrow(p, headerBtnRect(y, 0), false);

    auto keyGlyph = [&](int slot, bool enabled, bool accentOn) {
        const QRect r = headerBtnRect(y, slot);
        const double cx = r.x() + r.width() / 2.0;
        const double cy = r.y() + r.height() / 2.0;
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        if (slot == 2) { // losango central: adicionar/remover keyframe
            const QColor c = !canKf ? glyphDim : (accentOn ? activeBlue : glyph);
            p.setBrush(c);
            QPolygonF dia;
            dia << QPointF(cx, cy - 4) << QPointF(cx + 4, cy)
                << QPointF(cx, cy + 4) << QPointF(cx - 4, cy);
            p.drawPolygon(dia);
        } else {
            QPainterPath ar;
            if (slot == 1) {
                ar.moveTo(cx + 3, cy - 3.5);
                ar.lineTo(cx - 3, cy);
                ar.lineTo(cx + 3, cy + 3.5);
            } else {
                ar.moveTo(cx - 3, cy - 3.5);
                ar.lineTo(cx + 3, cy);
                ar.lineTo(cx - 3, cy + 3.5);
            }
            ar.closeSubpath();
            p.setBrush(enabled ? glyph : glyphDim);
            p.drawPath(ar);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    };
    keyGlyph(1, canKf && prevKf >= 0, false); // keyframe anterior
    keyGlyph(2, canKf, atKf);                 // adicionar/remover (acende em kf)
    keyGlyph(3, canKf && nextKf >= 0, false); // próximo keyframe

    // Sync lock ("corrente"; sem backend ainda).
    {
        const QRect r = headerBtnRect(y, 4);
        p.setPen(QPen(chipBd, 1));
        p.setBrush(chipBg);
        p.drawRect(r);
        p.setPen(Qt::NoPen);
        p.setBrush(glyphDim);
        p.drawEllipse(QPointF(r.x() + 5, r.y() + r.height() / 2.0 - 1), 3, 2.6);
        p.drawEllipse(QPointF(r.x() + 11, r.y() + r.height() / 2.0 + 1), 3, 2.6);
    }

    // ── Toggle de saída (olho/falante) ──────────────────────────────────
    drawOutputToggleIcon(p, headerBtnRect(y, 5), tr, QColor(0xD6, 0xD6, 0xD6), hidden);

    // ── Botões M / S / R do áudio ───────────────────────────────────────
    if (tr.audio) {
        auto chip = [&](int slot, const QString& label, bool active) {
            const QRect r = headerBtnRect(y, slot);
            p.setPen(QPen(active ? activeBlue.lighter(140) : chipBd, 1));
            p.setBrush(active ? activeBlue : chipBg);
            p.drawRect(r);
            p.setPen(active ? QColor(255, 255, 255) : QColor(0x9C, 0x9C, 0x9C));
            QFont bf = basef;
            bf.setBold(true);
            bf.setPointSizeF(7.5);
            p.setFont(bf);
            p.drawText(r, Qt::AlignCenter, label);
            p.setFont(basef);
        };
        chip(6, QStringLiteral("M"), tr.muted);
        chip(7, QStringLiteral("S"), tr.solo);
        chip(8, QStringLiteral("R"), false); // gravação de voz: sem backend
    }

    // ── Nome (linha abaixo dos controles) + cadeado ─────────────────────
    if (contentH >= 26) {
        QFont f = basef;
        f.setBold(true);
        f.setPointSizeF(8.5);
        p.setFont(f);
        p.setPen(hidden ? QColor(0x66, 0x66, 0x66) : QColor(0xD6, 0xD6, 0xD6));
        const QString nm = p.fontMetrics().elidedText(
            tr.name, Qt::ElideRight, headerNameRect(y).width());
        p.drawText(headerNameRect(y), int(Qt::AlignLeft | Qt::AlignVCenter), nm);
        p.setFont(basef);
        drawTrackLockIcon(p, headerLockRect(y), tr.locked);
    }

    // ── VU meter vertical do áudio (borda direita, como no Premiere) ────
    if (tr.audio) {
        const QRect mr = headerMeterRect(y, rowH);
        if (!mr.isEmpty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x1C, 0x1C, 0x20)); // slot escuro #1C1C20
            p.drawRect(mr);
            // segmentos do meter (leitura retificada, como no Premiere)
            p.setPen(QColor(0x2B, 0x2B, 0x2C));
            for (int sy = mr.y() + 4; sy < mr.y() + mr.height(); sy += 4)
                p.drawLine(mr.x(), sy, mr.x() + mr.width(), sy);
        }
    }

    // ── Alça de redimensionamento ────────────────────────────────────────
    const int gy0 = y + rowH - kResizeHandleH;
    p.fillRect(0, gy0, H, kResizeHandleH, QColor(0x24, 0x24, 0x24));
    p.setPen(QColor(0x44, 0x44, 0x44));
    const int gx0 = (H - 26) / 2;
    for (int i = 0; i < 4; ++i)
        p.drawLine(gx0 + i * 8, gy0 + 2, gx0 + i * 8, gy0 + 3);
}

// Cadeado pequeno do cabeçalho, como no Premiere: ícone cinza; travado acende
// em cinza claro (não há "vermelho de lock" — o bloqueio é só o ícone).
static void drawTrackLockIcon(QPainter& p, const QRect& r, bool locked) {
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c = locked ? QColor(0xD3, 0xD3, 0xD3) : QColor(0x6E, 0x6E, 0x6E);
    const double cx = r.center().x();
    const double top = r.y() + 2.0;
    // Alça (arreio) do cadeado.
    p.setPen(QPen(c, 1.6));
    p.setBrush(Qt::NoBrush);
    const int hinge = 3;
    p.drawArc(QRectF(cx - hinge, top, hinge * 2, hinge * 2 + 2), 0, 180 * 16);
    // Corpo.
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(cx - 4.5, top + 4, 9, 7), 1.5, 1.5);
    // Buraco da fechadura.
    p.setBrush(QColor(28, 28, 32));
    p.drawRoundedRect(QRectF(cx - 1.4, top + 6.2, 2.8, 3), 0.8, 0.8);
    p.setRenderHint(QPainter::Antialiasing, false);
}

void TimelineWidget::drawOutputToggleIcon(QPainter& p, const QRect& r, const Track& tr,
                                          const QColor& active, bool hidden) {
    // Ícone flat, sem caixa de fundo — como o olho/falante do Premiere.
    const QColor c = hidden ? QColor(0x6E, 0x6E, 0x6E) : active;
    p.setRenderHint(QPainter::Antialiasing, true);
    const double cx = r.x() + r.width() / 2.0;
    const double cy = r.y() + r.height() / 2.0;
    if (tr.audio) {
        // Alto-falante: corpo + cone + ondas.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(cx - 6.0, cy - 2.0, 5.0, 4.0), 0.8, 0.8);
        QPainterPath cone;
        cone.moveTo(cx - 1.0, cy - 3.0);
        cone.lineTo(cx + 3.0, cy - 5.0);
        cone.lineTo(cx + 3.0, cy + 5.0);
        cone.lineTo(cx - 1.0, cy + 3.0);
        cone.closeSubpath();
        p.drawPath(cone);
        p.setPen(QPen(c, 1.1));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(cx + 2.5, cy - 5.0, 7, 10), -55 * 16, 110 * 16);
        p.drawArc(QRectF(cx + 5.0, cy - 8.0, 10, 16), -62 * 16, 124 * 16);
    } else {
        // Olho: contorno de elipse + pupila.
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        QPainterPath eye;
        eye.moveTo(cx - 6.0, cy);
        eye.quadTo(cx - 6.0, cy - 5.2, cx + 6.0, cy);
        eye.quadTo(cx + 6.0, cy + 5.2, cx - 6.0, cy);
        eye.closeSubpath();
        p.drawPath(eye);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(18, 20, 24));
        p.drawEllipse(QPointF(cx + 1.0, cy), 2.0, 2.0);
    }
    if (hidden) {
        // Barra diagonal cinza indicando saída desligada (olho/falante diet).
        p.setPen(QPen(QColor(0xA0, 0xA0, 0xA0), 1.3));
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
