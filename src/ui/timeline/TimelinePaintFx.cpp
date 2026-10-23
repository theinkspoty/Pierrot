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

// ═══════════════════════════════════════════════════════════════════════════

// Clipe "ativo" da faixa de efeitos: o que cobre o playhead; se nenhum cobre,
// o primeiro clipe não-texto. Usado para abrir o efeito no Express.
Clip* TimelineWidget::fxActiveClip(const Track& tr) const {
    const Clip* best = nullptr;
    for (const Clip& c : tr.clips) {
        if (c.isText) continue;
        if (m_playhead >= c.pos && m_playhead < c.pos + c.dur)
            if (!best || c.pos > best->pos) best = &c;
    }
    if (!best)
        for (const Clip& c : tr.clips)
            if (!c.isText) { best = &c; break; }
    return const_cast<Clip*>(best);
}

// Rótulo de um efeito do catálogo pelo id (nativo, frei0r ou OFX).
QString TimelineWidget::fxEffectLabel(const QString& id) const {
    if (id == QStringLiteral("pierrot_brightness"))   return tr("Brilho & Contraste");
    if (id == QStringLiteral("pierrot_contrast"))     return tr("Contraste");
    if (id == QStringLiteral("pierrot_saturation"))   return tr("Saturação");
    if (id == QStringLiteral("pierrot_blur"))         return tr("Desfoque");
    if (id == QStringLiteral("pierrot_grayscale"))    return tr("Preto e Branco");
    if (id == QStringLiteral("pierrot_chromakey"))    return tr("Chroma Key");
    if (id == QStringLiteral("pierrot_psx"))          return tr("PSX");
    if (id == QStringLiteral("pierrot_filmgrain"))    return tr("Grão de Filme");
    if (id == QStringLiteral("pierrot_lainka"))       return tr("LAINKA (stop motion)");
    if (id == QStringLiteral("pierrot_motion"))       return tr("MotiOn (motion blur)");
    if (id == QStringLiteral("pierrot_lumetri"))      return tr("Correção de cor (Lumetri)");
    if (id == QStringLiteral("pierrot_audio_eq"))     return tr("EQ Express");
    if (id == QStringLiteral("pierrot_audio_reverb")) return tr("Reverb");
    if (id.startsWith(QStringLiteral("frei0r:"))) {
        const QString name = id.mid(QStringLiteral("frei0r:").size());
        for (const Frei0rPluginInfo& p : m_fxFrei0rPlugins)
            if (p.name == name) return p.label.isEmpty() ? p.name : p.label;
        return name;
    }
    for (const OfxPluginInfo& p : m_fxOfxPlugins)
        if (p.id == id) return p.name.isEmpty() ? p.id : p.name;
    return id;
}

// Monta a lista de efeitos DA FAIXA (Track::fxEffects) em
// `ids`/`labels`/`enabled`. Retorna a quantidade de linhas.
int TimelineWidget::fxEffectRows(const Track& tr, QVector<QString>* ids,
                                 QVector<QString>* labels, QVector<bool>* enabled) const {
    QVector<QString> lids, llabels;
    QVector<bool> len;
    for (const TrackFx& fx : tr.fxEffects) {
        lids.append(fx.id);
        llabels.append(fxEffectLabel(fx.id));
        len.append(fx.enabled);
    }
    if (ids) *ids = lids;
    if (labels) *labels = llabels;
    if (enabled) *enabled = len;
    return lids.size();
}

int TimelineWidget::fxPanelH(const Track& tr) const {
    if (!tr.fxTrack || !isFxExpanded(tr.id)) return 0;
    const int rows = fxEffectRows(tr, nullptr, nullptr, nullptr);
    return kFxGroupH + rows * kFxRowH + kFxPad;
}

QRect TimelineWidget::fxAddBtnRect(int y, int rowH) const {
    (void)rowH;
    return QRect(kHeaderW + 98, y + kVideoRowH + 2, kFxGroupH - 4, kFxGroupH - 4);
}

QRect TimelineWidget::fxRowRect(int y, int rowH, int idx) const {
    (void)rowH;
    return QRect(kHeaderW, y + kVideoRowH + kFxGroupH + idx * kFxRowH,
                 qMax(1, width() - kHeaderW), kFxRowH);
}

QRect TimelineWidget::fxRowEyeRect(int y, int rowH, int idx) const {
    const QRect r = fxRowRect(y, rowH, idx);
    return QRect(r.x() + 5, r.y() + 2, 16, kFxRowH - 4);
}

QRect TimelineWidget::fxRowDelRect(int y, int rowH, int idx) const {
    const QRect r = fxRowRect(y, rowH, idx);
    return QRect(r.right() - 21, r.y() + 2, 16, kFxRowH - 4);
}

bool TimelineWidget::fxPanelHit(const QPoint& pos, Clip*& clip, int& part,
                                QString& effectId, bool& enabled) const {
    clip = nullptr;
    part = 0;
    if (!m_project || pos.x() < kHeaderW || pos.y() < kRulerH) return false;
    int row = -1;
    bool audio = false;
    if (!rowFromY(pos.y(), row, audio) || audio) return false;
    if (row < 0 || row >= (int)m_project->videoTracks.size()) return false;
    if (!trackVisible(row, false)) return false;
    const Track& tr = m_project->videoTracks[row];
    if (!tr.fxTrack || tr.collapsed || !isFxExpanded(tr.id)) return false;
    const int y = rowY(row, -1);
    const int rowH = trackH(row, false);
    const int panelTop = y + kVideoRowH;
    clip = fxActiveClip(tr);
    if (pos.y() < panelTop) { clip = nullptr; return false; }  // corpo do clipe
    if (pos.y() < panelTop + kFxGroupH) {                       // barra do grupo
        if (fxAddBtnRect(y, rowH).contains(pos)) {
            part = 1;                                           // botão "+"
            return true;
        }
        part = 0;                                               // consome o clique na barra
        return true;
    }
    const int idx = (pos.y() - (panelTop + kFxGroupH)) / kFxRowH;
    if (idx < 0) return false;
    QVector<QString> ids, labels;
    QVector<bool> ens;
    const int n = fxEffectRows(tr, &ids, &labels, &ens);
    if (idx >= n) { part = 0; return true; }                    // respiro inferior: consome
    effectId = ids[idx];
    enabled = ens[idx];
    if (fxRowDelRect(y, rowH, idx).contains(pos)) {
        part = 4;                                               // botão "×"
    } else {
        part = fxRowEyeRect(y, rowH, idx).contains(pos) ? 3 : 2;
    }
    return true;
}

bool TimelineWidget::setFxEffectEnabled(Clip* clip, const QString& effectId, bool enabled) {
    if (!clip) return false;
    if (effectId.startsWith(QStringLiteral("frei0r:"))) {
        const QString name = effectId.mid(QStringLiteral("frei0r:").size());
        for (Frei0rEffect& fx : clip->frei0rFx)
            if (fx.pluginName == name) {
                if (fx.enabled != enabled) { fx.enabled = enabled; return true; }
                return false;
            }
        return false;
    }
    auto setIf = [&](auto& member, bool value) {
        if (member != value) { member = value; return true; }
        return false;
    };
    if (effectId == QStringLiteral("pierrot_lainka"))
        return setIf(clip->lainkaEnabled, enabled);
    if (effectId == QStringLiteral("pierrot_motion"))
        return setIf(clip->motionEnabled, enabled);
    if (effectId == QStringLiteral("pierrot_grayscale"))
        return setIf(clip->grayscale, enabled);
    if (effectId == QStringLiteral("pierrot_chromakey"))
        return setIf(clip->chromaKey, enabled);
    if (effectId == QStringLiteral("pierrot_psx"))
        return setIf(clip->psxEnabled, enabled);
    if (effectId == QStringLiteral("pierrot_filmgrain"))
        return setIf(clip->filmGrain, enabled);
    // Parâmetros numéricos: 0.2/1.2/5.0 = ligado (padrão do Express).
    if (effectId == QStringLiteral("pierrot_brightness")
        || effectId == QStringLiteral("pierrot_contrast")
        || effectId == QStringLiteral("pierrot_saturation")
        || effectId == QStringLiteral("pierrot_blur")) {
        double& v = effectId == QStringLiteral("pierrot_brightness") ? clip->brightness
                   : effectId == QStringLiteral("pierrot_contrast")  ? clip->contrast
                   : effectId == QStringLiteral("pierrot_saturation") ? clip->saturation
                                                                      : clip->blur;
        const double on = effectId == QStringLiteral("pierrot_brightness") ? 0.2
                          : effectId == QStringLiteral("pierrot_contrast")  ? 1.2
                          : effectId == QStringLiteral("pierrot_saturation") ? 1.2
                                                                             : 5.0;
        const double off = effectId == QStringLiteral("pierrot_brightness") ? 0.0
                           : (effectId == QStringLiteral("pierrot_contrast")
                              || effectId == QStringLiteral("pierrot_saturation")) ? 1.0
                                                                                   : 0.0;
        const double want = enabled ? on : off;
        if (std::fabs(v - want) > 1e-9) { v = want; return true; }
        return false;
    }
    for (OfxPluginInstance& fx : clip->ofxFx)
        if (fx.pluginId == effectId) {
            if (fx.enabled != enabled) { fx.enabled = enabled; return true; }
            return false;
        }
    return false;
}

// Ícone de olho do liga/desliga (estilo AE). `togglable=false` deixa o ícone
// apagado (efeitos sem estado desligável, ex.: Lumetri).
static void drawFxEye(QPainter& p, const QRect& r, bool on, bool togglable) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor col = !togglable ? QColor(0x50, 0x50, 0x54)
                      : on ? QColor(0x4A, 0x8F, 0xE0) : QColor(0x66, 0x66, 0x6A);
    const double cx = r.center().x() + 0.5;
    const double cy = r.center().y() + 0.5;
    p.setPen(QPen(col, 1.2));
    p.setBrush(Qt::NoBrush);
    QPainterPath eye;
    eye.moveTo(cx - 6, cy);
    eye.quadTo(cx, cy - 5, cx + 6, cy);
    eye.quadTo(cx, cy + 5, cx - 6, cy);
    p.drawPath(eye);
    if (on) {
        p.setBrush(col);
        p.drawEllipse(QPointF(cx, cy), 2.2, 2.2);
    } else if (togglable) {
        p.drawLine(QPointF(cx - 5, cy + 4), QPointF(cx + 5, cy - 4));
    }
    p.restore();
}

void TimelineWidget::drawFxPanel(QPainter& p, const Track& track, int y, int rowH) {
    if (!track.fxTrack || track.collapsed || !isFxExpanded(track.id)) return;
    p.save();
    // Não deixa o painel vazar para a faixa de baixo se a quantidade de efeitos
    // estourar a altura disponível (kMaxRowH).
    p.setClipRect(QRect(kHeaderW, y, qMax(1, width() - kHeaderW), rowH));
    const int panelTop = y + kVideoRowH;
    const int w = qMax(1, width() - kHeaderW);
    const QFont basef = p.font();

    // Barra do grupo "Efeitos" + botão "+".
    const QRect gb(kHeaderW, panelTop, w, kFxGroupH);
    p.fillRect(gb, QColor(0x24, 0x24, 0x2A));
    p.setPen(QColor(0x3A, 0x3A, 0x42));
    p.drawLine(gb.left(), gb.bottom(), gb.right(), gb.bottom());

    QFont gf = basef;
    gf.setBold(true);
    gf.setPointSizeF(8.0);
    p.setFont(gf);
    p.setPen(QColor(0xC8, 0xC8, 0xCC));
    QVector<QString> ids, labels;
    QVector<bool> ens;
    const int n = fxEffectRows(track, &ids, &labels, &ens);
    p.drawText(QRect(kHeaderW + 8, panelTop, 90, kFxGroupH),
               Qt::AlignLeft | Qt::AlignVCenter,
               n > 0 ? tr("Efeitos (%1)").arg(n) : tr("Efeitos"));

    const QRect add = fxAddBtnRect(y, rowH);
    p.setPen(QPen(QColor(0x5A, 0x6F, 0x9A), 1));
    p.setBrush(QColor(0x33, 0x3A, 0x4C));
    p.drawRoundedRect(add, 3, 3);
    p.setPen(QColor(0xD6, 0xD6, 0xD6));
    QFont af = basef;
    af.setBold(true);
    af.setPointSizeF(10.0);
    p.setFont(af);
    p.drawText(add, Qt::AlignCenter, QStringLiteral("+"));
    p.setFont(basef);

    // Linhas de efeito.
    QFont rf = basef;
    rf.setPointSizeF(8.0);
    for (int i = 0; i < n; ++i) {
        const QRect rr = fxRowRect(y, rowH, i);
        p.fillRect(rr, (i % 2) ? QColor(0x22, 0x22, 0x26) : QColor(0x1E, 0x1E, 0x22));
        const QRect er = fxRowEyeRect(y, rowH, i);
        const bool togglable = ids[i] != QStringLiteral("pierrot_lumetri");
        drawFxEye(p, er, ens[i], togglable);
        p.setFont(rf);
        p.setPen(ens[i] ? QColor(0xD6, 0xD6, 0xD6) : QColor(0x88, 0x88, 0x8C));
        const QString nm = p.fontMetrics().elidedText(
            labels[i], Qt::ElideRight, rr.width() - 62);
        p.drawText(QRect(er.right() + 6, rr.y(), rr.width() - 64, rr.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, nm);
        // Botão "×" (remover efeito da faixa).
        const QRect dr = fxRowDelRect(y, rowH, i);
        p.setPen(QColor(0x9A, 0x9A, 0x9E));
        p.drawLine(dr.left() + 4, dr.top() + 4, dr.right() - 4, dr.bottom() - 4);
        p.drawLine(dr.left() + 4, dr.bottom() - 4, dr.right() - 4, dr.top() + 4);
    }
    p.setFont(basef);
    p.restore();
}

void TimelineWidget::setFxExpanded(const QString& trackId, bool on) {
    if (on) m_fxExpanded.insert(trackId);
    else m_fxExpanded.remove(trackId);
}

// Menu do botão "+": efeitos nativos + submenus frei0r e OFX.
void TimelineWidget::buildFxAddMenu(QMenu* menu) {
    QMenu* natives = menu->addMenu(tr("Nativos"));
    struct Item { const char* id; const char* label; };
    static const Item items[] = {
        { "pierrot_brightness", QT_TR_NOOP("Brilho & Contraste") },
        { "pierrot_contrast",   QT_TR_NOOP("Contraste") },
        { "pierrot_saturation", QT_TR_NOOP("Saturação") },
        { "pierrot_blur",       QT_TR_NOOP("Desfoque") },
        { "pierrot_grayscale",  QT_TR_NOOP("Preto e Branco") },
        { "pierrot_chromakey",  QT_TR_NOOP("Chroma Key") },
        { "pierrot_psx",        QT_TR_NOOP("PSX") },
        { "pierrot_filmgrain",  QT_TR_NOOP("Grão de Filme") },
        { "pierrot_lainka",     QT_TR_NOOP("LAINKA (stop motion)") },
        { "pierrot_motion",     QT_TR_NOOP("MotiOn (motion blur)") },
        { "pierrot_lumetri",    QT_TR_NOOP("Correção de cor (Lumetri)") },
    };
    for (const Item& it : items) {
        QAction* a = natives->addAction(tr(it.label));
        a->setData(QString::fromLatin1(it.id));
    }
    if (!m_fxFrei0rPlugins.isEmpty()) {
        QMenu* f0 = menu->addMenu(tr("frei0r"));
        for (const Frei0rPluginInfo& pl : m_fxFrei0rPlugins) {
            if (!pl.isFilter) continue;
            QAction* a = f0->addAction(pl.label.isEmpty() ? pl.name : pl.label);
            a->setData(QStringLiteral("frei0r:") + pl.name);
        }
    }
    if (!m_fxOfxPlugins.isEmpty()) {
        QMenu* ofx = menu->addMenu(tr("OFX"));
        for (const OfxPluginInfo& pl : m_fxOfxPlugins) {
            QAction* a = ofx->addAction(pl.name.isEmpty() ? pl.id : pl.name);
            a->setData(pl.id);
        }
    }
}

