#include "MainWindow.h"

#include <QIcon>
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <functional>

namespace {
// Ícones SVG monocromáticos são recoloridos para a cor do tema (WindowText).
// Troca qualquer token de cor hex (#rgb/#rrggbb) pela cor alvo; "none" não casa
// e é preservado. Assim um mesmo SVG serve no tema claro e escuro.
QString recolorSvg(const QByteArray& raw, const QColor& color) {
    static const QRegularExpression hexRe(
        QStringLiteral("#[0-9a-fA-F]{6}\\b|#[0-9a-fA-F]{3}\\b"));
    return QString::fromUtf8(raw).replace(hexRe, color.name());
}
} // namespace

QIcon MainWindow::padlockIcon(bool locked) const {
    const qreal dpr = devicePixelRatioF();
    QPixmap pm(qRound(24 * dpr), qRound(24 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor c = palette().color(QPalette::WindowText);

    p.setPen(QPen(c, 1.6));
    p.setBrush(locked ? QBrush(c) : QBrush());
    p.drawRoundedRect(QRectF(5.5, 10.5, 13, 10), 2.0, 2.0);

    if (locked) {
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Window));
        p.drawEllipse(QPointF(12, 15.5), 1.8, 1.8);
        p.drawRect(QRectF(11.4, 16.0, 1.2, 2.0));
    } else {
        p.setPen(QPen(c, 1.3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 15.5), 1.6, 1.6);
    }

    QPen shacklePen(c, 2.2);
    shacklePen.setCapStyle(Qt::RoundCap);
    p.setPen(shacklePen);
    p.setBrush(Qt::NoBrush);
    const QRectF shackleRect(8.0, 4.5, 8.0, 9.0);
    if (locked)
        p.drawArc(shackleRect, 180 * 16, -180 * 16);
    else
        p.drawArc(shackleRect, 200 * 16, -140 * 16);

    p.end();
    return QIcon(pm);
}

QIcon MainWindow::makeIcon(const std::function<void(QPainter&, const QColor&)>& draw) const {
    const qreal dpr = devicePixelRatioF();
    QPixmap pm(qRound(32 * dpr), qRound(32 * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor fg = palette().color(QPalette::WindowText);
    draw(p, fg);
    p.end();
    return QIcon(pm);
}

// Ícone a partir de SVG embutido (recurso), recolorido com a cor do tema e
// rasterizado em várias densidades (hiDPI). Vazio se o recurso faltar — o
// chamador mantém o fallback em QPainter.
QIcon MainWindow::makeSvgIcon(const QString& resourcePath) const {
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) return QIcon();
    const QString fg = palette().color(QPalette::WindowText).name();
    QSvgRenderer renderer(recolorSvg(f.readAll(), QColor(fg)).toUtf8());
    if (!renderer.isValid()) return QIcon();
    const qreal dpr = devicePixelRatioF();
    QIcon icon;
    const int sizes[] = {16, 20, 24, 32, 48};
    for (int size : sizes) {
        QPixmap pm(qRound(size * dpr), qRound(size * dpr));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&p, QRectF(0, 0, size, size));
        p.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QIcon MainWindow::iconCursor() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/cursor.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Cursor gordinho: seta clássica preenchida com contorno arredondado.
        QPainterPath path;
        path.moveTo(4.5, 5);
        path.lineTo(4.5, 16);
        path.lineTo(7.6, 13.6);
        path.lineTo(10.6, 23);
        path.lineTo(13.6, 21.5);
        path.lineTo(10.8, 15.2);
        path.lineTo(14.8, 15.2);
        path.closeSubpath();
        p.setPen(QPen(c, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(c);
        p.drawPath(path);
    });
}

QIcon MainWindow::iconMove() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/move.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Mover em cruz: 4 setas grossas a partir do centro (estilo move).
        QPen pen(c, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(16, 14), QPointF(16, 9));
        p.drawLine(QPointF(16, 18), QPointF(16, 23));
        p.drawLine(QPointF(14, 16), QPointF(9, 16));
        p.drawLine(QPointF(18, 16), QPointF(23, 16));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF up; up << QPointF(16, 4.5)  << QPointF(12.5, 9.5)  << QPointF(19.5, 9.5);
        QPolygonF dn; dn << QPointF(16, 27.5) << QPointF(12.5, 22.5) << QPointF(19.5, 22.5);
        QPolygonF lf; lf << QPointF(4.5, 16)  << QPointF(9.5, 12.5)  << QPointF(9.5, 19.5);
        QPolygonF rg; rg << QPointF(27.5, 16) << QPointF(22.5, 12.5) << QPointF(22.5, 19.5);
        p.drawPolygon(up); p.drawPolygon(dn); p.drawPolygon(lf); p.drawPolygon(rg);
    });
}

QIcon MainWindow::iconScissors() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(8, 20), QPointF(25, 6));
        p.drawLine(QPointF(24, 20), QPointF(7, 6));
        p.drawEllipse(QPointF(8, 20), 3.5, 3.5);
        p.drawEllipse(QPointF(24, 20), 3.5, 3.5);
    });
}

QIcon MainWindow::iconRazor() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/razor.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Lâmina diagonal clássica do Premiere (Razor) com cabo gordinho.
        QPainterPath blade;
        blade.moveTo(9, 10);
        blade.lineTo(12, 7);
        blade.lineTo(27.5, 22.5);
        blade.lineTo(24.5, 25.5);
        blade.closeSubpath();
        p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(c);
        p.drawPath(blade);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(5, 12.5, 5, 13), 2.5, 2.5);
    });
}

QIcon MainWindow::iconEnvelope() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/envelope.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(5, 22);
        path.cubicTo(9, 9, 17, 28, 27, 8);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        const QPointF nodes[] = {QPointF(5, 22), QPointF(17, 19), QPointF(27, 8)};
        for (const QPointF& pt : nodes)
            p.drawEllipse(pt, 3.4, 3.4);
    });
}

QIcon MainWindow::iconZoom() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/zoom.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Lupa gordinha: lente vazia (sem "+") + cabo grosso arredondado.
        QPen pen(c, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(12, 13), 7.5, 7.5);
        p.drawLine(QPointF(18, 19), QPointF(27, 28));
    });
}

QIcon MainWindow::iconRipple() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/ripple.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Ripple do Premiere: três ondulações verticais grossas.
        QPen pen(c, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const double cx[] = {9.5, 16.0, 22.5};
        for (double xc : cx) {
            QPolygonF w;
            const double amp[7] = {2.2, -2.2, 2.2, -2.2, 2.2, -2.2, 2.2};
            for (int k = 0; k < 7; ++k)
                w << QPointF(xc + amp[k], 7.5 + k * 2.55);
            p.drawPolyline(w);
        }
    });
}

QIcon MainWindow::iconRolling() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/rolling.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Rolling do Premiere: setas opostas (→|←) com divisória central.
        QPen pen(c, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5, 16), QPointF(11, 16));
        p.drawLine(QPointF(27, 16), QPointF(21, 16));
        p.setPen(QPen(c, 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(16, 8), QPointF(16, 24));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF l;
        l << QPointF(3.5, 16) << QPointF(9, 12.5) << QPointF(9, 19.5);
        p.drawPolygon(l);
        QPolygonF r;
        r << QPointF(28.5, 16) << QPointF(23, 12.5) << QPointF(23, 19.5);
        p.drawPolygon(r);
    });
}

QIcon MainWindow::iconSlip() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/slip.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Slip do Premiere: quadro de filme com seta deslizando ao centro.
        QPen pen(c, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(6, 7, 20, 18), 4, 4);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF head;
        head << QPointF(18, 13) << QPointF(18, 19) << QPointF(25.5, 16);
        p.drawPolygon(head);
        p.drawRoundedRect(QRectF(8.5, 14, 9, 4), 2, 2);
    });
}

QIcon MainWindow::iconSlide() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/slide.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Slide do Premiere: quadro de filme com setas em cima e embaixo.
        QPen pen(c, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(7, 10, 18, 12), 3.5, 3.5);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF top;
        top << QPointF(13, 6) << QPointF(13, 11) << QPointF(21.5, 8.5);
        p.drawPolygon(top);
        p.drawRoundedRect(QRectF(7.5, 7.6, 6.5, 2.8), 1.4, 1.4);
        QPolygonF bot;
        bot << QPointF(13, 21) << QPointF(13, 26) << QPointF(21.5, 23.5);
        p.drawPolygon(bot);
        p.drawRoundedRect(QRectF(7.5, 21.6, 6.5, 2.8), 1.4, 1.4);
    });
}

QIcon MainWindow::iconRateStretch() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/ratestretch.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Rate Stretch do Premiere: relógio + seta de velocidade gordinha.
        QPen pen(c, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(11, 16), 7, 7);
        p.drawLine(QPointF(11, 16), QPointF(11, 10.5));
        p.drawLine(QPointF(11, 16), QPointF(15.5, 16));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(20.5, 13.5, 5, 5), 1.5, 1.5);
        QPolygonF arrow;
        arrow << QPointF(25.5, 11.5) << QPointF(25.5, 20.5) << QPointF(29.5, 16);
        p.drawPolygon(arrow);
    });
}

QIcon MainWindow::iconMagnet() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/magnet.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        QPainterPath path;
        path.moveTo(7, 24);
        path.lineTo(7, 12);
        path.arcTo(QRectF(7, 3, 18, 18), 180, -180);
        path.lineTo(25, 24);
        QPen pen(c, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(4, 24, 6, 4.5), 2, 2);
        p.drawRoundedRect(QRectF(22, 24, 6, 4.5), 2, 2);
    });
}

QIcon MainWindow::iconImport() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(c);
        p.drawLine(QPointF(16, 4), QPointF(16, 14));
        QPolygonF head;
        head << QPointF(16, 19) << QPointF(10, 13) << QPointF(22, 13);
        p.drawPolygon(head);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(6, 19), QPointF(6, 25));
        p.drawLine(QPointF(6, 25), QPointF(26, 25));
        p.drawLine(QPointF(26, 25), QPointF(26, 19));
        p.drawLine(QPointF(6, 19), QPointF(11, 19));
        p.drawLine(QPointF(21, 19), QPointF(26, 19));
    });
}

QIcon MainWindow::iconExport() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        QPen pen(c, 2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(c);
        p.drawLine(QPointF(16, 18), QPointF(16, 7));
        QPolygonF head;
        head << QPointF(16, 3) << QPointF(10, 9) << QPointF(22, 9);
        p.drawPolygon(head);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(6, 21), QPointF(6, 27));
        p.drawLine(QPointF(6, 27), QPointF(26, 27));
        p.drawLine(QPointF(26, 27), QPointF(26, 21));
    });
}

QIcon MainWindow::iconPlay() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF tri;
        tri << QPointF(9, 6) << QPointF(9, 22) << QPointF(22, 14);
        p.drawPolygon(tri);
    });
}

QIcon MainWindow::iconPause() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(9.5, 6, 3.5, 16), 1, 1);
        p.drawRoundedRect(QRectF(17.5, 6, 3.5, 16), 1, 1);
    });
}

QIcon MainWindow::iconSkipBack() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF t1;
        t1 << QPointF(13, 7) << QPointF(13, 21) << QPointF(5, 14);
        p.drawPolygon(t1);
        QPolygonF t2;
        t2 << QPointF(22, 7) << QPointF(22, 21) << QPointF(14, 14);
        p.drawPolygon(t2);
    });
}

QIcon MainWindow::iconSkipFwd() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF t1;
        t1 << QPointF(10, 7) << QPointF(10, 21) << QPointF(18, 14);
        p.drawPolygon(t1);
        QPolygonF t2;
        t2 << QPointF(19, 7) << QPointF(19, 21) << QPointF(27, 14);
        p.drawPolygon(t2);
    });
}

QIcon MainWindow::iconStepBack() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(9.5, 6.5, 2.2, 15), 1, 1);
        QPolygonF tri;
        tri << QPointF(19, 7) << QPointF(19, 21) << QPointF(11, 14);
        p.drawPolygon(tri);
    });
}

QIcon MainWindow::iconStepFwd() const {
    return makeIcon([](QPainter& p, const QColor& c) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        QPolygonF tri;
        tri << QPointF(10, 7) << QPointF(10, 21) << QPointF(19, 14);
        p.drawPolygon(tri);
        p.drawRoundedRect(QRectF(20.5, 6.5, 2.2, 15), 1, 1);
    });
}

QIcon MainWindow::iconLoopClear() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/loopclear.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Laço de loop + X de "remover", tudo grosso e redondo.
        QPen pen(c, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath br;
        br.moveTo(4, 21);
        br.lineTo(4, 9);
        br.lineTo(10, 9);
        p.drawPath(br);
        QPainterPath br2;
        br2.moveTo(28, 21);
        br2.lineTo(28, 9);
        br2.lineTo(22, 9);
        p.drawPath(br2);
        p.drawLine(QPointF(12.5, 11.5), QPointF(19.5, 18.5));
        p.drawLine(QPointF(19.5, 11.5), QPointF(12.5, 18.5));
    });
}

QIcon MainWindow::iconRippleDelete() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/rippledelete.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Lixeira gordinha (excluir com ripple): tampa + corpo fechado.
        QPen pen(c, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(9, 9, 14, 3.5), 1.75, 1.75);
        QPainterPath body;
        body.moveTo(10.5, 13.5);
        body.lineTo(12, 26.5);
        body.lineTo(20, 26.5);
        body.lineTo(21.5, 13.5);
        body.closeSubpath();
        p.drawPath(body);
        p.drawLine(QPointF(14, 16), QPointF(14, 23.5));
        p.drawLine(QPointF(18, 16), QPointF(18, 23.5));
    });
}

QIcon MainWindow::iconTrackStyle() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/trackstyle.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Estilo das faixas: três pastilhas de altura crescente.
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(7, 6, 18, 4), 2, 2);
        p.drawRoundedRect(QRectF(7, 12.5, 18, 5), 2.5, 2.5);
        p.drawRoundedRect(QRectF(7, 20, 18, 7), 3.5, 3.5);
    });
}

QIcon MainWindow::iconGrid() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/grid.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Grade pontilhada, traço mais grosso e arredondado.
        QPen pen(c, 1.8, Qt::DotLine, Qt::RoundCap);
        p.setPen(pen);
        for (int x = 8; x <= 24; x += 4)
            p.drawLine(QPointF(x, 6), QPointF(x, 26));
        for (int y = 12; y <= 20; y += 8)
            p.drawLine(QPointF(6, y), QPointF(26, y));
    });
}

QIcon MainWindow::iconRuler() const {
    const QIcon svg = makeSvgIcon(QStringLiteral(":/icons/ruler.svg"));
    if (!svg.isNull()) return svg;
    return makeIcon([](QPainter& p, const QColor& c) {
        // Régua gordinha: corpo arredondado com marcas de medição.
        QPen pen(c, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(5, 19, 22, 5), 2.5, 2.5);
        for (int x = 7; x <= 25; x += 2) {
            const int h = (x % 4 == 3) ? 8 : 5;
            p.drawLine(QPointF(x, 24), QPointF(x, 24 - h));
        }
    });
}

