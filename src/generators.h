// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <cmath>
#include <cstdlib>

#include <QImage>
#include <QLinearGradient>
#include <QPainter>

#include "models/Project.h"

// Gera um quadro ARGB32 para mídia virtual (geradores estilo Vegas):
//   ""          → cor sólida (solidColor)
//   "gradient"  → gradiente linear vertical solidColor → solidColor2
//   "checkerboard" → tabuleiro de genCells×genCells (alternando as duas cores)
//   "noise"     → grão aleatório entre solidColor e solidColor2
// O ruído é determinístico por id: o mesmo gerador sempre produz o mesmo
// padrão, independente do tamanho/tempo pedido.
inline QImage generatorFrame(const MediaItem& m, int w, int h) {
    w = qMax(1, w);
    h = qMax(1, h);
    QImage img(w, h, QImage::Format_ARGB32);
    const QString& g = m.generator;

    if (g == QLatin1String("gradient")) {
        QPainter p(&img);
        QLinearGradient grad(0, 0, 0, h);
        grad.setColorAt(0.0, m.solidColor);
        grad.setColorAt(1.0, m.solidColor2);
        p.fillRect(0, 0, w, h, grad);
        return img;
    }
    if (g == QLatin1String("checkerboard")) {
        const int n = qMax(1, m.genCells);
        const int cw = qMax(1, w / n);
        const int ch = qMax(1, h / n);
        const QRgb ca = m.solidColor.rgba();
        const QRgb cb = m.solidColor2.rgba();
        for (int y = 0; y < h; ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
            const int r = (y / ch) & 1;
            for (int x = 0; x < w; ++x)
                line[x] = (((x / cw) & 1) ^ r) ? ca : cb;
        }
        return img;
    }
    if (g == QLatin1String("noise")) {
        unsigned seed = 0;
        for (const QChar ch : m.id) seed = seed * 31u + ch.unicode();
        std::srand(seed);
        const qreal r0 = m.solidColor.red();
        const qreal g0 = m.solidColor.green();
        const qreal b0 = m.solidColor.blue();
        const qreal dr = m.solidColor2.red() - r0;
        const qreal dg = m.solidColor2.green() - g0;
        const qreal db = m.solidColor2.blue() - b0;
        for (int y = 0; y < h; ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const qreal t = std::rand() / (qreal)RAND_MAX;
                line[x] = qRgb((int)std::lround(r0 + t * dr),
                               (int)std::lround(g0 + t * dg),
                               (int)std::lround(b0 + t * db));
            }
        }
        return img;
    }

    img.fill(m.solidColor);
    return img;
}