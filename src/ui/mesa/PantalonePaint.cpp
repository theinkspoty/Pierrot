// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/mesa/PantaloneWidget.h"
#include "ui/core/Theme.h"
#include "ui/monitor/UiDecode.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/generators.h"
#include "colombina/render/TextRender.h"

#include <QPainter>
#include <QPainterPath>
#include <QGroupBox>
#include <QPushButton>
#include <QToolButton>
#include <QSlider>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QShortcut>
#include <QKeySequence>
#include <QScrollArea>
#include <QListWidget>
#include <QCheckBox>
#include <QComboBox>
#include <functional>
#include <algorithm>
#include <cmath>

#include "ui/mesa/PantalonePriv.h"

void PantaloneWidget::drawMeshGizmo(QPainter& p, const QRectF& disp, int w0, int h0) {
    Clip* c = activeClip();
    if (!c || !c->hasMesh() || w0 <= 0 || h0 <= 0) return;
    auto toDisp = [&](double nx, double ny) {
        return QPointF(disp.left() + nx * disp.width(),
                       disp.top() + ny * disp.height());
    };
    const QVector<QPointF>& mesh = c->mesh;
    const int cols = c->meshCols, rows = c->meshRows;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    // Grade: linhas horizontais e verticais conectando os pontos.
    p.setPen(QPen(QColor(130, 215, 255, 140), 1));
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols - 1; ++i)
            p.drawLine(toDisp(mesh[cols * j + i].x(), mesh[cols * j + i].y()),
                       toDisp(mesh[cols * j + i + 1].x(), mesh[cols * j + i + 1].y()));
    for (int i = 0; i < cols; ++i)
        for (int j = 0; j < rows - 1; ++j)
            p.drawLine(toDisp(mesh[cols * j + i].x(), mesh[cols * j + i].y()),
                       toDisp(mesh[cols * (j + 1) + i].x(), mesh[cols * (j + 1) + i].y()));
    // Pontos de controle (alças).
    p.setPen(QPen(QColor(12, 14, 18), 1));
    for (int k = 0; k < mesh.size(); ++k) {
        const QPointF d = toDisp(mesh[k].x(), mesh[k].y());
        p.setBrush(k == m_meshDrag ? QColor(255, 186, 60) : QColor(255, 255, 255));
        p.drawEllipse(d, 4.5, 4.5);
    }
    p.restore();
}

void PantaloneWidget::drawMaskGizmos(QPainter& p, const QRectF& disp, int w0, int h0) {
    Clip* c = activeClip();
    if (!c || c->masks.isEmpty() || w0 <= 0 || h0 <= 0) return;
    auto toDisp = [&](double sx, double sy) {
        return QPointF(disp.left() + sx / w0 * disp.width(),
                       disp.top() + sy / h0 * disp.height());
    };
    const QColor band(130, 215, 255);
    const QColor accent = themeColors().accent;
    const double rel = std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur));
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < c->masks.size(); ++i) {
        const Mask& m = c->masks[i];
        if (m.type.isEmpty()) continue;
        const bool active = (i == m_maskSel);
        // Valores interpolados no playhead (igual ao preview/exportação), para
        // o gizmo seguir a animação ao dar scrub.
        const double cx = m.cxAt(rel) * w0, cy = m.cyAt(rel) * h0;
        const double rx = std::max(0.001, m.rxAt(rel)) * w0;
        const double ry = std::max(0.001, m.ryAt(rel)) * h0;
        const double rotDeg = m.rotAt(rel);
        const double feath = m.featherAt(rel);
        const QPointF ctr = toDisp(cx, cy);
        // Forma (rotacionada) desenhada em torno do centro, no espaço de disp.
        p.save();
        p.translate(ctr);
        p.rotate(rotDeg);
        const double hw = rx / w0 * disp.width();
        const double hh = ry / h0 * disp.height();
        p.setBrush(Qt::NoBrush);
        if (feath > 0.0) {
            const double fx = feath * disp.width();
            const double fy = feath * disp.height();
            p.setPen(QPen(QColor(130, 215, 255, 90), 1, Qt::DashLine));
            if (m.type == QLatin1String("rect"))
                p.drawRect(QRectF(-hw - fx, -hh - fy, 2 * (hw + fx), 2 * (hh + fy)));
            else
                p.drawEllipse(QPointF(0, 0), hw + fx, hh + fy);
        }
        p.setPen(QPen(active ? QColor(255, 255, 255, 235)
                             : QColor(band.red(), band.green(), band.blue(), 130),
                      active ? 2 : 1));
        if (m.type == QLatin1String("rect"))
            p.drawRect(QRectF(-hw, -hh, 2 * hw, 2 * hh));
        else
            p.drawEllipse(QPointF(0, 0), hw, hh);
        p.restore();
        if (!active) continue;
        // Alças da máscara ativa (centro, rotação e bordas), já rotacionadas.
        const QVector<QPointF> pts = maskCtrlPoints(m.type, m.cxAt(rel), m.cyAt(rel),
                                                    m.rxAt(rel), m.ryAt(rel),
                                                    rotDeg, w0, h0);
        p.setPen(QPen(accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawLine(ctr, toDisp(pts[1].x(), pts[1].y()));
        p.setBrush(Qt::white);
        p.setPen(QPen(accent, 1.4));
        for (int k = 2; k < pts.size(); ++k) {
            const QPointF h = toDisp(pts[k].x(), pts[k].y());
            p.drawRect(QRectF(h.x() - 4.5, h.y() - 4.5, 9.0, 9.0));
        }
        p.drawEllipse(ctr, 4.5, 4.5);
        p.setBrush(QColor(255, 186, 60));
        p.drawEllipse(toDisp(pts[1].x(), pts[1].y()), 5.0, 5.0);
    }
    p.restore();
}

// Área (em px do widget) onde o frame do source é desenhado no viewfinder
// (modelo "contain", centralizado). Compartilhada pelo painter e pelos
// hit-tests, para a geometria dos gizmos nunca descolar do desenho.
QRectF PantaloneWidget::computeDisp(const QRect& viewRect) const {
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return QRectF();
    const int M = 12;
    const QRectF screen = QRectF(viewRect).adjusted(M, M, -M, -M);
    const double fr = (double)h0 / w0;
    const double sr = screen.height() / screen.width();
    QRectF disp;
    if (fr < sr) {
        disp.setWidth(screen.width());
        disp.setHeight(screen.width() * fr);
        disp.moveLeft(screen.left());
        disp.moveTop(screen.top() + (screen.height() - disp.height()) / 2.0);
    } else {
        disp.setHeight(screen.height());
        disp.setWidth(screen.height() / fr);
        disp.moveTop(screen.top());
        disp.moveLeft(screen.left() + (screen.width() - disp.width()) / 2.0);
    }
    return disp;
}

// Aplica zoom (escala) mantendo o centro da janela de saída NO MESMO ponto da
// imagem — mesmo comportamento do zoom com a roda, que redefine o pan para a
// janela não "andar" enquanto o usuário dá zoom (usado por roda e gizmo).
void PantaloneWidget::applyZoomTo(double sNew, const QPointF& centerSrc) {
    if (m_frame.isNull()) return;
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double L = std::clamp(m_cropL->value() / 100.0, 0.0, 1.0);
    const double R = std::clamp(m_cropR->value() / 100.0, 0.0, 1.0 - L);
    const double T = std::clamp(m_cropT->value() / 100.0, 0.0, 1.0);
    const double B = std::clamp(m_cropB->value() / 100.0, 0.0, 1.0 - T);
    const double kx = L * w0;
    const double ky = T * h0;
    const double kw = std::max(1.0, w0 * (1.0 - L - R));
    const double kh = std::max(1.0, h0 * (1.0 - T - B));
    const double k = std::min(W / kw, H / kh);
    const double ks = std::max(k * sNew, 1e-6);
    const double ntx = (kx + kw / 2.0 - centerSrc.x()) * ks;
    const double nty = (ky + kh / 2.0 - centerSrc.y()) * ks;
    QSignalBlocker b5(m_scale), b6(m_panX), b7(m_panY);
    m_scale->setValue((int)std::lround(sNew * 100.0));
    m_panX->setValue((int)std::lround(std::clamp(ntx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(nty / H * 100.0, -100.0, 100.0)));
    commitSlider(P_Scale, sNew);
    commitSlider(P_PanX, ntx);
    commitSlider(P_PanY, nty);
    updateValueLabels();
    m_view->update();
}

void PantaloneWidget::paintViewfinder(QWidget* view) {
    QPainter p(view);
    // Fundo quadriculado sutil (estilo DaVinci/After Effects) atrás do frame.
    const QColor tileA(42, 44, 50);
    const QColor tileB(33, 35, 41);
    const int ts = 13;
    p.fillRect(view->rect(), tileB);
    p.setPen(Qt::NoPen);
    p.setBrush(tileA);
    for (int y = 0; y < view->height(); y += ts) {
        const int off = ((y / ts) & 1) * ts;
        for (int x = off; x < view->width(); x += 2 * ts)
            p.drawRect(x, y, ts, ts);
    }
    // Borda sutil do monitor.
    p.setPen(QPen(QColor(51, 51, 51), 1));
    p.drawRect(view->rect().adjusted(0, 0, -1, -1));
    if (m_frame.isNull()) {
        p.setPen(QColor(140, 140, 150));
        p.drawText(view->rect(), Qt::AlignCenter,
                   tr("Selecione um clipe de vídeo para editar o pantalone."));
        return;
    }
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;

    const QRectF disp = computeDisp(view->rect());

    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double s = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
    const double tx = m_panX->value() / 100.0 * W;
    const double ty = m_panY->value() / 100.0 * H;

    QRectF cropS, outS;
    computeView(s, tx, ty, w0, h0, &cropS, &outS);

    auto toDisp = [&](double sx, double sy) {
        return QPointF(disp.left() + sx / w0 * disp.width(),
                       disp.top() + sy / h0 * disp.height());
    };

    p.save();
    // Gira a cena inteira em torno do centro da área de exibição.
    const double rot = m_rotation->value();
    p.translate(disp.center());
    p.rotate(rot);
    p.translate(-disp.center());
    p.drawImage(QRectF(disp.left(), disp.top(), disp.width(), disp.height()), m_frame);

    // Janela de saída (borda azul-ciano de destaque) e caixas/gizmos.
    const QRectF cropDisp(toDisp(cropS.left(), cropS.top()),
                          toDisp(cropS.right(), cropS.bottom()));
    const QRectF outDisp(toDisp(outS.left(), outS.top()),
                         toDisp(outS.right(), outS.bottom()));

    // Máscara escura (estilo Premiere): véu do que está fora do recorte no
    // modo Recortar; do que está fora da janela de saída no modo Transformar.
    if (m_mode == ModeCrop) {
        QPainterPath outsideCrop;
        outsideCrop.addRect(disp);
        QPainterPath cropHole;
        cropHole.addRect(cropDisp.adjusted(-3, -3, 3, 3));
        p.fillPath(outsideCrop.subtracted(cropHole), QColor(0, 0, 0, 110));
    } else if (m_mode == ModeTransform && cropDisp != outDisp) {
        QPainterPath outsideOut;
        outsideOut.addRect(cropDisp);
        QPainterPath outHole;
        outHole.addRect(outDisp);
        p.fillPath(outsideOut.subtracted(outHole), QColor(0, 0, 0, 130));
    }

    // Grade de terços + cruz central (modo Transformar, de enquadramento).
    if (m_mode == ModeTransform && m_showGrid) {
        p.setPen(QPen(QColor(255, 255, 255, 45), 1));
        const QPointF c = outDisp.center();
        p.drawLine(QPointF(outDisp.left(), c.y()), QPointF(outDisp.right(), c.y()));
        p.drawLine(QPointF(c.x(), outDisp.top()), QPointF(c.x(), outDisp.bottom()));
        for (int i = 1; i <= 2; ++i) {
            const double fx = i / 3.0;
            const double fy = i / 3.0;
            p.drawLine(QPointF(outDisp.left() + outDisp.width() * fx, outDisp.top()),
                       QPointF(outDisp.left() + outDisp.width() * fx, outDisp.bottom()));
            p.drawLine(QPointF(outDisp.left(), outDisp.top() + outDisp.height() * fy),
                       QPointF(outDisp.right(), outDisp.top() + outDisp.height() * fy));
        }
    }

    // Safe margins + caixa de saída (modo Transformar) — cinza discreto.
    if (m_mode == ModeTransform) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 32), 1));
        p.drawRect(outDisp.adjusted(outDisp.width() * 0.05, outDisp.height() * 0.05,
                                    -outDisp.width() * 0.05, -outDisp.height() * 0.05));
        p.setPen(QColor(255, 255, 255, 20));
        p.drawRect(outDisp.adjusted(outDisp.width() * 0.10, outDisp.height() * 0.10,
                                    -outDisp.width() * 0.10, -outDisp.height() * 0.10));
        // Caixa de ZOOM/saída (borda azul-ciano em destaque).
        p.setPen(QPen(QColor(0, 163, 224), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(outDisp);
    }
    if (m_mode == ModeCrop) {
        // Caixa de RECORTE (borda branca tracejada sutil).
        p.setPen(QPen(QColor(255, 255, 255, 150), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cropDisp);
    }

    const bool hasHover = (m_hoverMode != DragNone && m_dragMode == DragNone);
    auto drawHandle = [&](const QPointF& h, double sz, int fr, int fg, int fb,
                          int fa) {
        p.setBrush(hasHover ? QColor(fr, fg, fb, 255)
                            : QColor(fr, fg, fb, fa));
        p.drawRoundedRect(QRectF(h.x() - sz / 2.0, h.y() - sz / 2.0, sz, sz), 2.0, 2.0);
    };

    // Alças de recorte (modo Recortar): quadrados brancos no perímetro do crop.
    if (m_mode == ModeCrop) {
        const double hd = hasHover ? 10.0 : 8.0;
        p.setPen(QPen(QColor(12, 14, 18), 1));
        const QPointF tl(cropDisp.left(), cropDisp.top());
        const QPointF tr(cropDisp.right(), cropDisp.top());
        const QPointF bl(cropDisp.left(), cropDisp.bottom());
        const QPointF br(cropDisp.right(), cropDisp.bottom());
        const QPointF mt(cropDisp.center().x(), cropDisp.top());
        const QPointF mb(cropDisp.center().x(), cropDisp.bottom());
        const QPointF ml(cropDisp.left(), cropDisp.center().y());
        const QPointF mr(cropDisp.right(), cropDisp.center().y());
        const QPointF handles[8] = { tl, tr, bl, br, mt, mb, ml, mr };
        for (const QPointF& h : handles)
            drawHandle(h, hd, 220, 225, 235, 210);
    }

    // Modo Transformar: alças de zoom (douradas), rotação e âncora.
    if (m_mode == ModeTransform) {
        const double zd = hasHover ? 12.0 : 9.0;
        p.setPen(QPen(QColor(10, 12, 16), 1));
        const QPointF outCorners[4] = { outDisp.topLeft(), outDisp.topRight(),
                                        outDisp.bottomLeft(), outDisp.bottomRight() };
        for (const QPointF& cp : outCorners) {
            const QPointF d = cp - outDisp.center();
            const double len = std::hypot(d.x(), d.y());
            const QPointF pos = len > 1e-6 ? cp + d / len * 9.0 : cp;
            drawHandle(pos, zd, 255, 186, 60, 235);
        }
        // Alça de ROTAÇÃO (estilo Premiere).
        const QPointF rotHdl(outDisp.center().x(), outDisp.top() - 26.0);
        p.setPen(QPen(QColor(255, 186, 60, 220), 1));
        p.setBrush(Qt::NoBrush);
        p.drawLine(outDisp.center().x(), outDisp.top(), rotHdl.x(), rotHdl.y());
        p.setPen(QPen(QColor(10, 12, 16), 1));
        p.setBrush(hasHover ? QColor(255, 216, 120) : QColor(255, 186, 60, 235));
        p.drawEllipse(rotHdl, 5.5, 5.5);
        // Gizmo de ponto de ancoragem (anel branco com cruz).
        const double axPct = m_anchorX->value() / 100.0;
        const double ayPct = m_anchorY->value() / 100.0;
        const double anchorSrcX = w0 * 0.5 + axPct * w0 * 0.5;
        const double anchorSrcY = h0 * 0.5 + ayPct * h0 * 0.5;
        const QPointF anchorDisp = toDisp(anchorSrcX, anchorSrcY);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(anchorDisp, 7.0, 7.0);
        p.drawLine(QPointF(anchorDisp.x() - 4.0, anchorDisp.y()),
                   QPointF(anchorDisp.x() + 4.0, anchorDisp.y()));
        p.drawLine(QPointF(anchorDisp.x(), anchorDisp.y() - 4.0),
                   QPointF(anchorDisp.x(), anchorDisp.y() + 4.0));
    }

    // Gizmos de máscara (modo Janelas) — no espaço rotacionado, com a imagem.
    if (m_mode == ModeMask)
        drawMaskGizmos(p, disp, w0, h0);

    // Malha (modo Malha) — grade 3×3 de espremer/esticar a mídia.
    if (m_mode == ModeMesh)
        drawMeshGizmo(p, disp, w0, h0);

    // ── Indicador de resolução do projeto (estilo Vegas) ─────────────
    p.restore();  // restaura antes de desenhar o overlay sem rotação
    p.save();
    const QString resLabel = QString("%1×%2").arg((int)W).arg((int)H);
    QFont f = view->font();
    f.setPointSize(9);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(0, 0, 0, 160));
    const QRect r = view->rect();
    p.drawText(r.adjusted(8, 0, -8, -8), Qt::AlignBottom | Qt::AlignRight,
               resLabel);
    p.setPen(QColor(255, 255, 255, 200));
    p.drawText(r.adjusted(7, -1, -9, -9), Qt::AlignBottom | Qt::AlignRight,
               resLabel);
    p.restore();
}


