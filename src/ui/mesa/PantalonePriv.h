// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Helpers compartilhados entre os .cpp de PantaloneWidget (Data, Keys, Mask,
// Paint, Viewport, Strip). Viviam como anon-namespace no .cpp único — com o
// corte eles atravessariam unidades de tradução.

#pragma once

#include "ui/mesa/PantaloneWidget.h"
#include "colombina/render/TextRender.h"

#include <QGridLayout>
#include <QPointF>
#include <QString>
#include <QToolButton>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 4.0;
constexpr int kMinStretch = 20;   // % (0.2×) do achatamento/esticamento não-uniforme
constexpr int kMaxStretch = 300;  // % (3.0×)
constexpr int kCropMax = 100;  // %
constexpr int kPanMax = 100;   // % da largura/altura do projeto
constexpr int kRotMax = 180;   // graus (±)
constexpr double kStretchCenter = 1.0; // neutro (1.0×)

// Renderiza um TextStyle sobre um frame transparente (usado no pantalone de
// clipes de texto, que não têm quadro decodificado). Aplica o transform do
// clipe (pan/zoom/rotação) para o texto aparecer ONDE o preview mostra —
// antes ficava centralizado no pantalone mesmo com o clipe deslocado.
void renderTextStyleFrame(QImage& img, const TextStyle& st,
                          double tx, double ty, double rotDeg, double s,
                          double sX = 1.0, double sY = 1.0) {
    const double W = img.width();
    const double H = img.height();
    const textrender::Layout L = textrender::layout(st, W, H);
    const QRectF box = textrender::centeredBox(st, L, W, H);

    QPainter p(&img);
    p.translate(W / 2.0 + tx, H / 2.0 + ty);
    p.rotate(rotDeg);
    p.scale(s * sX, s * sY);
    textrender::paint(p, st, L, box, H);
}
constexpr int kRotStep = 5;    // graus por tick da roda (Alt+roda)

// Pontos de controle da máscara em px do quadro, já rotacionados, a partir dos
// valores RESOLVIDOS (normalizados 0..1 + rotação em graus):
// [0]=centro, [1]=alça de rotação, [2..]=bordas (cantos p/ retângulo,
// cardeais p/ elipse). O mesmo layout do previewMaskCtrl, para o hit-test e o
// desenho do Pantalone batem com o overlay do preview.
QVector<QPointF> maskCtrlPoints(const QString& type, double cxN, double cyN,
                                double rxN, double ryN, double rotDeg,
                                int w0, int h0) {
    const double cx = cxN * w0, cy = cyN * h0;
    const double rx = std::max(0.001, rxN) * w0;
    const double ry = std::max(0.001, ryN) * h0;
    const double rad = rotDeg * M_PI / 180.0;
    QVector<QPointF> base;
    if (type == QLatin1String("rect"))
        base = { QPointF(cx - rx, cy - ry), QPointF(cx + rx, cy - ry),
                 QPointF(cx + rx, cy + ry), QPointF(cx - rx, cy + ry) };
    else
        base = { QPointF(cx - rx, cy), QPointF(cx, cy - ry),
                 QPointF(cx + rx, cy), QPointF(cx, cy + ry) };
    if (rad != 0.0) {
        const double cs = std::cos(rad), sn = std::sin(rad);
        for (QPointF& b : base) {
            const double dx = b.x() - cx, dy = b.y() - cy;
            b = QPointF(cx + dx * cs - dy * sn, cy + dx * sn + dy * cs);
        }
    }
    const double arm = std::max(1.0, std::min(rx, ry));
    QVector<QPointF> pts;
    pts << QPointF(cx, cy)
        << QPointF(cx - arm * std::sin(rad), cy - arm * std::cos(rad));
    for (const QPointF& b : base) pts << b;
    return pts;
}

// Seção recolhível estilo "Effect Controls" do Premiere: um cabeçalho com seta
// de expansão que esconde/mostra o corpo (grade de sliders).
struct CollapsibleSection {
    QWidget* container = nullptr;
    QWidget* body = nullptr;
    QToolButton* header = nullptr;
    QGridLayout* grid = nullptr;
};

// Presets de suavização "estilo Vegas" aplicados ao segmento que sai do
// keyframe (ou, no último, ao segmento que chega).
void applyEasePreset(QVector<Keyframe>& kf, int i, int preset) {
    Keyframe* k = &kf[i];
    if (i + 1 < kf.size()) {
        const double span = kf[i + 1].time - k->time;
        const double delta = kf[i + 1].value - k->value;
        switch (preset) {
            case 1: // Suave
                k->interp = KfSmooth; k->ox = k->oy = k->ix = k->iy = 0.0; break;
            case 2: // Rápido (começa rápido, desacelera)
                k->interp = KfBezier; k->ox = span / 3.0; k->oy = 0.85 * delta;
                k->ix = 0.0; k->iy = 0.0; break;
            case 3: // Lento (começa devagar, acelera)
                k->interp = KfBezier; k->ox = span / 3.0; k->oy = 0.15 * delta;
                k->ix = 0.0; k->iy = 0.0; break;
            case 4: // Acentuada (S acentuado no meio)
                k->interp = KfBezier; k->ox = span / 3.0; k->oy = 0.55 * delta;
                k->ix = 0.0; k->iy = 0.0; break;
            default: // Linear
                k->interp = KfLinear; break;
        }
    } else if (i > 0) {
        Keyframe* p = &kf[i - 1];
        const double span = k->time - p->time;
        const double delta = k->value - p->value;
        switch (preset) {
            case 1:
                p->interp = KfSmooth; p->ox = p->oy = p->ix = p->iy = 0.0; break;
            case 2:
                p->interp = KfBezier; p->ox = span / 3.0; p->oy = 0.85 * delta;
                p->ix = 0.0; p->iy = 0.0; break;
            case 3:
                p->interp = KfBezier; p->ox = span / 3.0; p->oy = 0.15 * delta;
                p->ix = 0.0; p->iy = 0.0; break;
            case 4:
                p->interp = KfBezier; p->ox = span / 3.0; p->oy = 0.55 * delta;
                p->ix = 0.0; p->iy = 0.0; break;
            default:
                p->interp = KfLinear; break;
        }
    }
}
}

// Viewfinder: widget que exibe o frame com a caixa de recorte/janela de saída
// e recebe os eventos de mouse/roda. Só delega para o PantaloneWidget dono.
class PantaloneWidget::Viewport : public QWidget {
public:
    explicit Viewport(PantaloneWidget* owner) : QWidget(owner), m_owner(owner) {
        setMinimumSize(180, 150);
    }
protected:
    void paintEvent(QPaintEvent*) override { m_owner->paintViewfinder(this); }
    void mousePressEvent(QMouseEvent* e) override { m_owner->viewportPress(this, e); }
    void mouseMoveEvent(QMouseEvent* e) override { m_owner->viewportMove(this, e); }
    void mouseReleaseEvent(QMouseEvent* e) override { m_owner->viewportRelease(this, e); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { m_owner->viewportDoubleClick(this, e); }
    void wheelEvent(QWheelEvent* e) override { m_owner->viewportWheel(this, e); }
    void contextMenuEvent(QContextMenuEvent* e) override { m_owner->viewportContextMenu(this, e); }
    void leaveEvent(QEvent*) override {
        if (m_owner->m_hoverMode != DragNone) {
            m_owner->m_hoverMode = DragNone;
            update();
        }
        setCursor(Qt::ArrowCursor);
    }
private:
    PantaloneWidget* m_owner;
};

// Faixa de tempo dos keyframes do clipe: mostra a posição dos keyframes
// (relativa ao início do clipe) e permite arrastá-los para a esquerda/direita.
class PantaloneWidget::KeyframeStrip : public QWidget {
public:
    explicit KeyframeStrip(PantaloneWidget* owner) : QWidget(owner), m_owner(owner) {
        setMinimumHeight(72); // 3 pistas (Recorte · Máscara · Resto)
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::ClickFocus);
    }
protected:
    void paintEvent(QPaintEvent*) override { m_owner->paintKeyframeStrip(this); }
    void mousePressEvent(QMouseEvent* e) override { m_owner->stripPress(this, e); }
    void mouseMoveEvent(QMouseEvent* e) override { m_owner->stripMove(this, e); }
    void mouseReleaseEvent(QMouseEvent* e) override { m_owner->stripRelease(this, e); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { m_owner->stripDoubleClick(this, e); }
    void contextMenuEvent(QContextMenuEvent* e) override { m_owner->stripContextMenu(this, e); }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
            m_owner->deleteSelectedKeyframes();
            e->accept();
        } else {
            QWidget::keyPressEvent(e);
        }
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ShortcutOverride) {
            const int key = static_cast<QKeyEvent*>(e)->key();
            // Impede o atalho global do MainWindow de apagar o clipe quando a
            // faixa de keyframes do pantalone está com foco.
            if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
                e->accept();
                return true;
            }
        }
        return QWidget::event(e);
    }
private:
    PantaloneWidget* m_owner;
};
