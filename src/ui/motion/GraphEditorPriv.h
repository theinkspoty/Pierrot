// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Helpers compartilhados entre os .cpp de GraphEditorWidget (Canvas, Strip,
// Rows e o próprio widget). Viviam como anon-namespace/static no .cpp
// único — com o corte eles atravessariam unidades de tradução.

#pragma once

#include "ui/motion/GraphEditorWidget.h"
#include "ui/core/Theme.h"

#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QString>
#include <QVector>

#include <cmath>

namespace {

constexpr int kMarginL = 10;
constexpr int kMarginR = 8;
constexpr int kMarginT = 5;
constexpr int kMarginB = 14;
constexpr int kCanvasRulerH = 16;

double niceStep(double raw) {
    if (raw <= 0) return 1.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double nice;
    if (norm < 1.5)      nice = 1.0;
    else if (norm < 3.0) nice = 2.0;
    else if (norm < 7.0) nice = 5.0;
    else                 nice = 10.0;
    return nice * mag;
}

QString propName(GraphProp p) {
    switch (p) {
        case GPropOpacity:   return QStringLiteral("Opacity");
        case GPropVolume:    return QStringLiteral("Volume");
        case GPropScale:     return QStringLiteral("Scale");
        case GPropScaleX:    return QStringLiteral("Flatt X");
        case GPropScaleY:    return QStringLiteral("Flatt Y");
        case GPropRotation:  return QStringLiteral("Rotation");
        case GPropTx:        return QStringLiteral("Position X");
        case GPropTy:        return QStringLiteral("Position Y");
        case GPropCropL:     return QStringLiteral("Crop Left");
        case GPropCropR:     return QStringLiteral("Crop Right");
        case GPropCropT:     return QStringLiteral("Crop Top");
        case GPropCropB:     return QStringLiteral("Crop Bottom");
        case GPropAnchorX:   return QStringLiteral("Anchor X");
        case GPropAnchorY:   return QStringLiteral("Anchor Y");
        case GPropMaskCx:      return QStringLiteral("Máscara Centro X");
        case GPropMaskCy:      return QStringLiteral("Máscara Centro Y");
        case GPropMaskRx:      return QStringLiteral("Máscara Largura");
        case GPropMaskRy:      return QStringLiteral("Máscara Altura");
        case GPropMaskRot:     return QStringLiteral("Máscara Rotação");
        case GPropMaskFeather: return QStringLiteral("Máscara Suavidade");
        case GPropMesaX:     return QStringLiteral("Mesa X");
        case GPropMesaY:     return QStringLiteral("Mesa Y");
        case GPropMesaScaleX:    return QStringLiteral("Mesa Escala");
        case GPropMesaScaleY:    return QStringLiteral("Mesa Scale Y");
        case GPropMesaRotation:  return QStringLiteral("Mesa Rotation");
        case GPropMesaOpacity:   return QStringLiteral("Mesa Opacity");
        case GPropMesaAnchorX:   return QStringLiteral("Mesa Anchor X");
        case GPropMesaAnchorY:   return QStringLiteral("Mesa Anchor Y");
        case GPropCamX:          return QStringLiteral("Camera X");
        case GPropCamY:          return QStringLiteral("Camera Y");
        case GPropCamZoom:       return QStringLiteral("Camera Zoom");
        case GPropCamRotation:   return QStringLiteral("Camera Rotation");
    }
    return QString();
}

QString interpName(int interp) {
    switch (interp) {
        case KfSmooth:  return QStringLiteral("Auto Bezier");
        case KfStep:    return QStringLiteral("Hold");
        case KfBezier:  return QStringLiteral("Bezier");
        case KfElastic: return QStringLiteral("Elastic");
        case KfBounce:  return QStringLiteral("Bounce");
        case KfBack:    return QStringLiteral("Back");
        default:        return QStringLiteral("Linear");
    }
}

// Acessores compartilhados (editor + minifaixa) para os keyframes de uma
// propriedade de um clipe.
QVector<Keyframe>* keysFor(Clip* c, GraphProp p) {
    if (!c) return nullptr;
    switch (p) {
        case GPropOpacity:  return &c->kfOpacity;
        case GPropVolume:   return &c->kfVolume;
        case GPropScale:    return &c->kfScale;
        case GPropScaleX:   return &c->kfScaleX;
        case GPropScaleY:   return &c->kfScaleY;
        case GPropRotation: return &c->kfRotation;
        case GPropTx:       return &c->kfTx;
        case GPropTy:       return &c->kfTy;
        case GPropCropL:    return &c->kfCropL;
        case GPropCropR:    return &c->kfCropR;
        case GPropCropT:    return &c->kfCropT;
        case GPropCropB:    return &c->kfCropB;
        case GPropAnchorX:  return &c->kfAnchorX;
        case GPropAnchorY:  return &c->kfAnchorY;
    }
    return nullptr;
}

double baseFor(Clip* c, GraphProp p) {
    if (!c) return 0.0;
    switch (p) {
        case GPropOpacity:  return c->opacity;
        case GPropVolume:   return c->volume;
        case GPropScale:    return c->scale;
        case GPropScaleX:   return c->scaleX;
        case GPropScaleY:   return c->scaleY;
        case GPropRotation: return c->rotation;
        case GPropTx:       return c->tx;
        case GPropTy:       return c->ty;
        case GPropCropL:    return c->cropL;
        case GPropCropR:    return c->cropR;
        case GPropCropT:    return c->cropT;
        case GPropCropB:    return c->cropB;
        case GPropAnchorX:  return c->anchorX;
        case GPropAnchorY:  return c->anchorY;
        default: return 0.0;
    }
    return 0.0;
}

// Keyframes/valor base das propriedades da máscara ativa (Pantalone).
QVector<Keyframe>* keysForMask(Mask* m, GraphProp p) {
    if (!m) return nullptr;
    switch (p) {
        case GPropMaskCx:      return &m->kfCx;
        case GPropMaskCy:      return &m->kfCy;
        case GPropMaskRx:      return &m->kfRx;
        case GPropMaskRy:      return &m->kfRy;
        case GPropMaskRot:     return &m->kfRotation;
        case GPropMaskFeather: return &m->kfFeather;
        default: return nullptr;
    }
    return nullptr;
}

double baseForMask(Mask* m, GraphProp p) {
    if (!m) return 0.0;
    switch (p) {
        case GPropMaskCx:      return m->cx;
        case GPropMaskCy:      return m->cy;
        case GPropMaskRx:      return m->rx;
        case GPropMaskRy:      return m->ry;
        case GPropMaskRot:     return m->rotation;
        case GPropMaskFeather: return m->feather;
        default: return 0.0;
    }
    return 0.0;
}

// Resolve uma máscara por ÍNDICE no clipe (seguro: não guarda ponteiro, que
// penduraria quando Clip::masks realoca ou o clipe troca).
static Mask* maskAt(Clip* c, int idx) {
    return (c && idx >= 0 && idx < c->masks.size()) ? &c->masks[idx] : nullptr;
}

QVector<Keyframe>* keysForTrack(Track* t, GraphProp p) {
    if (!t) return nullptr;
    switch (p) {
        case GPropMesaX:       return &t->kfMesaX;
        case GPropMesaY:       return &t->kfMesaY;
        case GPropMesaScaleX:  return &t->kfMesaScaleX;
        case GPropMesaScaleY:  return &t->kfMesaScaleY;
        case GPropMesaRotation: return &t->kfMesaRotation;
        case GPropMesaOpacity:  return &t->kfMesaOpacity;
        case GPropMesaAnchorX:  return &t->kfMesaAnchorX;
        case GPropMesaAnchorY:  return &t->kfMesaAnchorY;
        default: return nullptr;
    }
    return nullptr;
}

double baseForTrack(Track* t, GraphProp p) {
    if (!t) return 0.0;
    switch (p) {
        case GPropMesaX:       return t->mesaX;
        case GPropMesaY:       return t->mesaY;
        case GPropMesaScaleX:  return t->mesaScaleX;
        case GPropMesaScaleY:  return t->mesaScaleY;
        case GPropMesaRotation: return t->mesaRotation;
        case GPropMesaOpacity:  return t->mesaOpacity;
        case GPropMesaAnchorX:  return t->mesaAnchorX;
        case GPropMesaAnchorY:  return t->mesaAnchorY;
        default: return 0.0;
    }
    return 0.0;
}

QVector<Keyframe>* keysForCamera(MesaComposition* mc, GraphProp p) {
    if (!mc) return nullptr;
    switch (p) {
        case GPropCamX:        return &mc->kfCamX;
        case GPropCamY:        return &mc->kfCamY;
        case GPropCamZoom:     return &mc->kfCamZoom;
        case GPropCamRotation: return &mc->kfCamRotation;
        default: return nullptr;
    }
    return nullptr;
}

double baseForCamera(MesaComposition* mc, GraphProp p) {
    if (!mc) return 0.0;
    switch (p) {
        case GPropCamX:        return mc->camX;
        case GPropCamY:        return mc->camY;
        case GPropCamZoom:     return mc->camZoom;
        case GPropCamRotation: return mc->camRotation;
        default: return 0.0;
    }
    return 0.0;
}

void rangeFor(GraphProp p, double* lo, double* hi) {
    switch (p) {
        case GPropOpacity:  *lo = 0.0; *hi = 1.0; return;
        case GPropVolume:   *lo = 0.0; *hi = 2.0; return;
        case GPropScale:    *lo = 0.0; *hi = 3.0; return;
        case GPropScaleX: case GPropScaleY:
            *lo = 0.2; *hi = 3.0; return;
        case GPropRotation: *lo = -360.0; *hi = 360.0; return;
        case GPropTx: *lo = -800.0; *hi = 800.0; return;
        case GPropTy: *lo = -450.0; *hi = 450.0; return;
        case GPropCropL: case GPropCropR:
        case GPropCropT: case GPropCropB:
            *lo = 0.0; *hi = 1.0; return;
        case GPropAnchorX: case GPropAnchorY:
            *lo = -1.0; *hi = 1.0; return;
        case GPropMaskCx: case GPropMaskCy:
            *lo = 0.0; *hi = 1.0; return;
        case GPropMaskRx: case GPropMaskRy:
            *lo = 0.0; *hi = 1.5; return;
        case GPropMaskRot: *lo = -360.0; *hi = 360.0; return;
        case GPropMaskFeather: *lo = 0.0; *hi = 0.25; return;
        case GPropMesaX: *lo = -2000.0; *hi = 2000.0; return;
        case GPropMesaY: *lo = -2000.0; *hi = 2000.0; return;
        case GPropMesaScaleX: case GPropMesaScaleY:
            *lo = 0.01; *hi = 10.0; return;
        case GPropMesaRotation: *lo = -360.0; *hi = 360.0; return;
        case GPropMesaOpacity: *lo = 0.0; *hi = 1.0; return;
        case GPropMesaAnchorX: case GPropMesaAnchorY:
            *lo = -1000.0; *hi = 1000.0; return;
        case GPropCamX: *lo = -2000.0; *hi = 2000.0; return;
        case GPropCamY: *lo = -2000.0; *hi = 2000.0; return;
        case GPropCamZoom: *lo = 0.05; *hi = 20.0; return;
        case GPropCamRotation: *lo = -360.0; *hi = 360.0; return;
    }
    *lo = 0.0; *hi = 1.0;
}

// Glifo do keyframe conforme a interpolação (estilo Premiere):
// Linear = losango agudo, Auto Bezier = círculo, Bezier = losango arredondado,
// Hold = "casa" com topo reto.
void drawKeyGlyph(QPainter& p, const QPointF& c, int interp, qreal s,
                  const QBrush& fill) {
    p.setBrush(fill);
    p.setPen(QPen(QColor(255, 255, 255), 1));
    switch (interp) {
    case KfStep: {
        const QPolygonF pent = QPolygonF()
            << QPointF(c.x() - s * 0.85, c.y() - s * 0.9)
            << QPointF(c.x() + s * 0.85, c.y() - s * 0.9)
            << QPointF(c.x() + s * 0.95, c.y() + s * 0.25)
            << QPointF(c.x(), c.y() + s)
            << QPointF(c.x() - s * 0.95, c.y() + s * 0.25);
        p.drawPolygon(pent);
        return;
    }
    case KfSmooth:
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawEllipse(c, s, s);
        p.setRenderHint(QPainter::Antialiasing, false);
        return;
    case KfBezier: {
        QPainterPath path;
        path.moveTo(c.x(), c.y() - s);
        path.quadTo(c.x() + s * 1.1, c.y() - s * 0.1, c.x(), c.y() + s);
        path.quadTo(c.x() - s * 1.1, c.y() + s * 0.1, c.x(), c.y() - s);
        path.closeSubpath();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillPath(path, fill);
        p.drawPath(path);
        p.setRenderHint(QPainter::Antialiasing, false);
        return;
    }
    case KfLinear:
    default: {
        const QPolygonF dia = QPolygonF()
            << QPointF(c.x(), c.y() - s)
            << QPointF(c.x() + s, c.y())
            << QPointF(c.x(), c.y() + s)
            << QPointF(c.x() - s, c.y());
        p.drawPolygon(dia);
        return;
    }
    case KfElastic: {
        // Estrela — elástico.
        QPolygonF star;
        for (int i = 0; i < 10; ++i) {
            const double ang = -3.14159265358979323846 / 2.0 + i * 3.14159265358979323846 / 5.0;
            const double r = (i % 2 == 0) ? s * 1.15 : s * 0.5;
            star << QPointF(c.x() + r * std::cos(ang), c.y() + r * std::sin(ang));
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawPolygon(star);
        p.setRenderHint(QPainter::Antialiasing, false);
        return;
    }
    case KfBounce: {
        // Casa larga com base larga — quique.
        const QPolygonF house = QPolygonF()
            << QPointF(c.x() - s * 0.95, c.y() + s * 0.7)
            << QPointF(c.x() - s * 0.7, c.y() - s * 0.5)
            << QPointF(c.x(), c.y() - s * 1.05)
            << QPointF(c.x() + s * 0.7, c.y() - s * 0.5)
            << QPointF(c.x() + s * 0.95, c.y() + s * 0.7);
        p.drawPolygon(house);
        return;
    }
    case KfBack: {
        // Círculo com "cauda" de overshoot.
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawEllipse(c, s * 0.65, s * 0.65);
        p.drawLine(QPointF(c.x() - s * 1.15, c.y() + s * 0.85),
                   QPointF(c.x() - s * 0.35, c.y() + s * 0.35));
        p.setRenderHint(QPainter::Antialiasing, false);
        return;
    }
    }
}

QString fmtRuler(double t, double range) {
    if (range <= 2.0)  return QString::number(t, 'f', 2);
    if (range <= 60.0) return QString::number(t, 'f', 1);
    return QString::number(t, 'g', 4);
}

QString fmtVal(double v, double span) {
    const int prec = (span < 2.0) ? 4 : (span < 50.0 ? 3 : 2);
    return QString::number(v, 'g', prec);
}

QIcon makeStopwatchIcon(bool on) {
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c = on ? themeColors().accent : themeColors().iconMuted;
    p.setPen(QPen(c, 1.4));
    QColor fill = themeColors().accent;
    fill.setAlpha(40);
    p.setBrush(on ? fill : Qt::NoBrush);
    p.drawEllipse(QPointF(8, 8), 5.2, 5.2);
    p.drawRect(QRectF(7, 0, 2, 2.6));
    p.drawLine(QPointF(8, 8), QPointF(8, 4.6));
    p.drawLine(QPointF(8, 8), QPointF(10.6, 9.4));
    p.end();
    return QIcon(pm);
}


// Tangente (Catmull-Rom) de um keyframe Auto Bezier, em valor/segundo.
// Nas pontas cai na secante — o mesmo que o kfValue() usa para KfSmooth.
static double autoTangent(const QVector<Keyframe>& K, int i) {
    if (i < 0 || i >= K.size()) return 0.0;
    const Keyframe& k = K[i];
    const Keyframe& prev = (i > 0) ? K[i - 1] : k;
    const Keyframe& next = (i + 1 < K.size()) ? K[i + 1] : k;
    const double dt = next.time - prev.time;
    if (dt <= 1e-9) return 0.0;
    return (next.value - prev.value) / dt;
}

// Ponto de controle do handle (saída se out==true, entrada se false) do
// keyframe i, para interpolações suave (Auto Bezier) e bezier manual.
// Para Auto Bezier o handle é calculado automaticamente ao longo da tangente;
// para Bezier manual são usados os handles gravados.
// ht/hv = (deslocamento em tempo, deslocamento em valor) a partir do keyframe.
// Retorna false se o keyframe não tem handle naquele lado (pontas ou não-bezier).
static bool bezierHandle(const QVector<Keyframe>& K, int i, bool out,
                         double* ht, double* hv) {
    if (i < 0 || i >= K.size()) return false;
    const Keyframe& k = K[i];
    if (k.interp == KfBezier) {
        if (out) {
            if (i + 1 >= K.size()) return false;
            *ht = k.ox; *hv = k.oy;
            return true;
        }
        if (i - 1 < 0) return false;
        *ht = k.ix; *hv = k.iy;
        return true;
    }
    // Só Auto Bezier recebe alça padrão (tangente Catmull-Rom): fica visível
    // para o usuário entortar a curva, e ao arrastá-la o keyframe é convertido
    // para Bezier manual. Linear/Degrau NÃO recebem alça — arrastava "uma
    // alça fantasma" com 3× a inclinação da reta e convertia a reta em S.
    if (k.interp == KfSmooth) {
        const double span = out
            ? ((i + 1 < K.size()) ? K[i + 1].time - k.time : 0.0)
            : ((i - 1 >= 0) ? k.time - K[i - 1].time : 0.0);
        if (span <= 1e-9) return false;
        // Par isolado (2 keys): secante/2 (mesmo critério do kfValue).
        const double m = autoTangent(K, i) * ((K.size() == 2) ? 0.5 : 1.0);
        *ht = span / 3.0;
        *hv = out ? m * span / 3.0 : -m * span / 3.0;
        return true;
    }
    return false;
}
}
