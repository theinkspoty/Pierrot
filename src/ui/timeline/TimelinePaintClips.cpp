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
        // Clipe na faixa de efeitos: corpo na cor AZUL da faixa, para diferir
        // visualmente (mas renderiza a própria mídia normalmente — o efeito da
        // faixa é copiado para os efeitos do clipe ao entrar na faixa).
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
    p.setPen(QPen(sel ? QColor(255,255,255,235) : border, sel ? 2 : sel2 ? 1.5 : 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, 6, 6);

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
    const qreal hue = tint.hslHueF() < 0.0 ? 0.58 : tint.hslHueF();
    const qreal sat = tint.hslSaturationF() > 0.0 ? qMin(0.9, tint.hslSaturationF() + 0.15) : 0.7;

    MediaCache& cache = MediaCache::instance();
    if (!cache.hasPeaks(path, c.audioStreamIndex)) {
        cache.requestPeaks(path, c.audioStreamIndex);
        QLinearGradient bg(r.topLeft(), r.bottomLeft());
        bg.setColorAt(0.0, QColor::fromHslF(hue, sat, 0.32));
        bg.setColorAt(1.0, QColor::fromHslF(hue, sat, 0.26));
        p.fillRect(r, bg);
        p.setPen(QPen(QColor(255, 255, 255, 40), 1));
        p.drawLine(r.left(), r.center().y(), r.right(), r.center().y());
        return;
    }

    const FFmpegAudioPeaks& pk = cache.peaks(path, c.audioStreamIndex);
    const int bps = pk.bucketsPerSecond > 0 ? pk.bucketsPerSecond : 1;
    const int x0 = r.left();
    const int x1 = r.right();
    const double dur = c.dur;
    const int midY = r.center().y();
    const double amp = qMax(2.0, r.height() / 2.0 - 3.0);
    const bool sel = isSelected(c.id);
    const bool sel2 = !sel && isSecondarySelected(c.id);

    // Corpo do clipe com gradiente mais limpo
    {
        QLinearGradient bg(r.topLeft(), r.bottomLeft());
        if (sel) {
            bg.setColorAt(0.0, QColor::fromHslF(hue, sat, 0.52));
            bg.setColorAt(1.0, QColor::fromHslF(hue, sat, 0.38));
        } else if (sel2) {
            bg.setColorAt(0.0, QColor::fromHslF(hue, sat, 0.42));
            bg.setColorAt(1.0, QColor::fromHslF(hue, sat, 0.32));
        } else {
            bg.setColorAt(0.0, QColor::fromHslF(hue, sat, 0.38));
            bg.setColorAt(1.0, QColor::fromHslF(hue, sat, 0.28));
        }
        p.fillRect(r, bg);
    }

    // Linha central mais sutil
    p.setPen(QPen(QColor(0, 0, 0, 70), 1));
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

    // Autogain por percentil p95
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
                                              (qsizetype)(peaks.size() * 0.96));
        gPeak = peaks[idx];
        if (gPeak < 1e-4f) gPeak = peaks.last();
    }
    constexpr double kMaxWaveGain = 24.0;
    const double gain = gPeak > 1e-4 ? std::min(0.98 / gPeak, kMaxWaveGain) : 1.0;

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QPainterPath fillPath;
    QPainterPath linePath;
    bool first = true;
    for (int x = x0; x <= x1; ++x) {
        float mn = 0.0f, mx = 0.0f;
        colMinMax(x, mn, mx);
        const float absPeak = std::max(std::fabs(mx), std::fabs(mn));
        if (absPeak <= 8e-5f) {
            if (!first) {
                // manter continuidade suave
                const int yMid = midY;
                if (first) { fillPath.moveTo(x, yMid); linePath.moveTo(x, yMid); first = false; }
                else { fillPath.lineTo(x, yMid); linePath.lineTo(x, yMid); }
            }
            continue;
        }
        const int top = qBound(r.top() + 1, (int)std::lround(midY - mx * amp * gain), r.bottom() - 1);
        const int bot = qBound(r.top() + 1, (int)std::lround(midY - mn * amp * gain), r.bottom() - 1);
        if (top > bot) continue;
        if (first) {
            fillPath.moveTo(x, midY);
            linePath.moveTo(x, top);
            first = false;
        }
        linePath.lineTo(x, top);
    }
    // fechar fill
    for (int x = x1; x >= x0; --x) {
        float mn = 0.0f, mx = 0.0f;
        colMinMax(x, mn, mx);
        const float absPeak = std::max(std::fabs(mx), std::fabs(mn));
        if (absPeak <= 8e-5f) {
            fillPath.lineTo(x, midY);
            continue;
        }
        const int bot = qBound(r.top() + 1, (int)std::lround(midY - mn * amp * gain), r.bottom() - 1);
        fillPath.lineTo(x, bot);
    }
    fillPath.closeSubpath();

    // Sombra da waveform pra destacar
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 60));
    p.translate(0, 1);
    p.drawPath(fillPath);
    p.translate(0, -1);

    // Fill da waveform
    QLinearGradient wf(r.topLeft(), r.bottomLeft());
    wf.setColorAt(0.0, QColor(235, 245, 255, 230));
    wf.setColorAt(0.5, QColor(210, 230, 255, 210));
    wf.setColorAt(1.0, QColor(190, 215, 255, 190));
    p.setBrush(wf);
    p.drawPath(fillPath);

    // Contorno da waveform
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 160), 1));
    p.drawPath(linePath);

    // Segundo contorno mais fino pra dar definição
    p.setPen(QPen(QColor(120, 170, 255, 90), 0.8));
    QPainterPath linePath2 = linePath;
    p.drawPath(linePath2);

    p.restore();
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

// Indicador de sobreposição em faixa de ÁUDIO: rampas de fade em X sobre a
// waveform, mostrando o crossfade em potência constante (o clipe de trás cai
// cos(π/2·p) e o da frente sobe sin(π/2·p) — os quadrados somam 1). Sem label,
// para não poluir a onda; as duas curvas cruzam no centro (X).
void TimelineWidget::drawAudioCrossfade(QPainter& p, const QRect& r) {
    if (r.width() < 3 || r.height() < 4) return;

    constexpr double kHalfPi = 1.5707963267948966; // π/2
    p.save();
    p.setClipRect(r, Qt::IntersectClip);

    // Banda de sobreposição: escurece levemente a região do crossfade para
    // destacá-la da waveform.
    p.fillRect(r, QColor(0, 0, 0, 55));

    const int n = std::max(2, r.width());
    const double h = r.height() - 1.0;
    QPainterPath outCurve; // clipe de trás: gain 1 → 0
    QPainterPath inCurve;  // clipe da frente: gain 0 → 1
    for (int i = 0; i <= n; ++i) {
        const double pr = (double)i / n;
        const int x = r.left() + (int)std::lround(pr * (r.width() - 1));
        const int yOut = r.bottom() - (int)std::lround(std::cos(kHalfPi * pr) * h);
        const int yIn = r.bottom() - (int)std::lround(std::sin(kHalfPi * pr) * h);
        if (i == 0) { outCurve.moveTo(x, yOut); inCurve.moveTo(x, yIn); }
        else { outCurve.lineTo(x, yOut); inCurve.lineTo(x, yIn); }
    }
    p.setBrush(Qt::NoBrush);
    // Sombra sutil embaixo de cada curva para leitura sobre waveform clara.
    p.setPen(QPen(QColor(0, 0, 0, 150), 2.6));
    p.drawPath(outCurve);
    p.drawPath(inCurve);
    p.setPen(QPen(QColor(255, 214, 130, 235), 1.4)); // saída (quente)
    p.drawPath(outCurve);
    p.setPen(QPen(QColor(140, 220, 255, 235), 1.4)); // entrada (fria)
    p.drawPath(inCurve);

    // Bordas verticais delimitando a sobreposição.
    p.setPen(QPen(QColor(255, 255, 255, 70), 1));
    p.drawLine(r.left(), r.top(), r.left(), r.bottom());
    p.drawLine(r.right(), r.top(), r.right(), r.bottom());
    p.restore();
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

