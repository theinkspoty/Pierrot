// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QStringList>

#include "colombina/models/Project.h"

// Layout e desenho de TextStyle (títulos) compartilhados entre o preview, o
// pantalone de texto e o export. Antes estas três rotinas duplicavam a quebra de
// linha, o dimensionamento da fonte e a geração do QPainterPath.
namespace textrender {

struct Layout {
    QFont font;
    QStringList wrapped;
    double lineHeight = 0.0;
    double ascent = 0.0;
    double pad = 0.0;
    double bw = 0.0;
    double bh = 0.0;
};

inline Layout layout(const TextStyle& st, double W, double H) {
    Layout L;
    const double sizeFrac = st.textSize > 0.0 ? st.textSize : (1.0 / 18.0);
    const int pxSize = qMax(4, (int)qRound(sizeFrac * H));
    if (!st.fontFamily.isEmpty()) L.font.setFamily(st.fontFamily);
    L.font.setPixelSize(pxSize);
    L.font.setBold(st.textBold);
    const QFontMetricsF fm(L.font);

    const double maxW = W * 0.9;
    for (const QString& raw : st.text.split(QLatin1Char('\n'))) {
        if (raw.isEmpty()) { L.wrapped << QString(); continue; }
        QString cur;
        const QStringList words = raw.split(QLatin1Char(' '));
        for (const QString& w : words) {
            const QString trial = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
            if (fm.horizontalAdvance(trial) <= maxW || cur.isEmpty())
                cur = trial;
            else { L.wrapped << cur; cur = w; }
        }
        L.wrapped << cur;
    }

    double tw = 0.0;
    for (const QString& l : L.wrapped) tw = qMax(tw, fm.horizontalAdvance(l));
    const double th = L.wrapped.size() * fm.height();
    L.lineHeight = fm.height();
    L.ascent = fm.ascent();
    L.pad = pxSize * 0.25;
    L.bw = tw + 2.0 * L.pad;
    L.bh = th + 2.0 * L.pad;
    return L;
}

// Caixa relativa ao centro do quadro (preview e pantalone aplicam o transform
// do clipe em torno do centro).
inline QRectF centeredBox(const TextStyle& st, const Layout& L, double W, double H) {
    double x;
    if (st.textAlign == 1) x = st.textX * W - W / 2.0;
    else if (st.textAlign == 2) x = st.textX * W - L.bw - W / 2.0;
    else x = st.textX * W - L.bw / 2.0 - W / 2.0;
    const double y = st.textY * H - L.bh / 2.0 - H / 2.0;
    return QRectF(x, y, L.bw, L.bh);
}

// Caixa em coordenadas absolutas do quadro (export aplica o transform adiante).
inline QRectF absoluteBox(const TextStyle& st, const Layout& L, double W, double H) {
    double x;
    if (st.textAlign == 1) x = st.textX * W;
    else if (st.textAlign == 2) x = st.textX * W - L.bw;
    else x = st.textX * W - L.bw / 2.0;
    const double y = st.textY * H - L.bh / 2.0;
    return QRectF(x, y, L.bw, L.bh);
}

inline void paint(QPainter& p, const TextStyle& st, const Layout& L,
                  const QRectF& box, double H) {
    if (st.textBackground) p.fillRect(box, st.textBackgroundColor);
    const double baseline = box.top() + L.pad + L.ascent;
    if (st.textOutline > 0.0) {
        QPainterPath path;
        for (int i = 0; i < L.wrapped.size(); ++i)
            path.addText(QPointF(box.left() + L.pad, baseline + i * L.lineHeight),
                         L.font, L.wrapped[i]);
        QPen pen(st.textOutlineColor, qMax(1.0, st.textOutline * H),
                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(path, pen);
    }
    // Preenchimento via drawText (não fillPath): fontes de emoji colorido
    // (Noto Color Emoji etc.) não geram contour via addText e sumiam do
    // frame; drawText renderiza o glifo nativo, mantendo a cor do emoji.
    p.setFont(L.font);
    p.setPen(st.textColor);
    for (int i = 0; i < L.wrapped.size(); ++i)
        p.drawText(QPointF(box.left() + L.pad, baseline + i * L.lineHeight),
                   L.wrapped[i]);
}

} // namespace textrender
