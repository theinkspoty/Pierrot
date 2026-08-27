// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/ScopeWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QImage>
#include <QVector>
#include <cmath>
#include <algorithm>

ScopeWidget::ScopeWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(196, 128);
}

void ScopeWidget::refreshFrom(const QImage& small) {
    m_src = small;
    if (m_src.isNull()) { m_cache = QImage(); }
    else {
        switch (m_mode) {
        case Histogram: buildHistogram(); break;
        case Vectorscope: buildVectorscope(); break;
        case Waveform: default: buildWaveform(); break;
        }
    }
    update();
}

// Waveform: luma Y (0..255, eixo vertical) em função da posição horizontal.
void ScopeWidget::buildWaveform() {
    m_cache = QImage(256, 256, QImage::Format_ARGB32);
    m_cache.fill(Qt::black);
    const int w = m_src.width(), h = m_src.height();
    for (int y = 0; y < h; ++y) {
        const uchar* line = m_src.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const int si = x * 4;
            const double r = line[si + 2], g = line[si + 1], b = line[si + 0];
            const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
            int yy = 256 - 1 - (int)std::lround(std::clamp(lum, 0.0, 255.0));
            yy = std::clamp(yy, 0, 255);
            const int xx = (int)((double)x / (double)w * 255.0);
            m_cache.setPixelColor(xx, yy, QColor(0, 255, 96));
        }
    }
    // Grade de referência (IRE 0/100 e linhas de 25%).
    QPainter p(&m_cache);
    p.setPen(QColor(60, 60, 60));
    for (int i = 0; i < 5; ++i) {
        const int y = 256 - (int)(i * 51.2);
        p.drawLine(0, y, 255, y);
    }
}

// Histograma RGB com cobertura semitransparente por canal.
void ScopeWidget::buildHistogram() {
    int bins[3][256] = {};
    const int w = m_src.width(), h = m_src.height();
    for (int y = 0; y < h; ++y) {
        const uchar* line = m_src.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const int si = x * 4;
            ++bins[0][line[si + 2]];
            ++bins[1][line[si + 1]];
            ++bins[2][line[si + 0]];
        }
    }
    int maxCount = 1;
    for (int c = 0; c < 3; ++c)
        maxCount = std::max(maxCount, *std::max_element(bins[c], bins[c] + 256));

    const QColor col[3] = { QColor(255, 60, 60), QColor(60, 255, 60), QColor(70, 90, 255) };
    m_cache = QImage(256, 256, QImage::Format_ARGB32);
    m_cache.fill(Qt::black);
    QPainter p(&m_cache);
    for (int c = 0; c < 3; ++c) {
        QVector<QPointF> pts(256);
        for (int i = 0; i < 256; ++i) {
            const int hgt = (int)std::lround((double)bins[c][i] / (double)maxCount * 255.0);
            pts[i] = QPointF(i, 256 - hgt);
        }
        QPainterPath path;
        path.moveTo(0, 256);
        for (const QPointF& pt : pts) path.lineTo(pt);
        path.lineTo(255, 256);
        path.closeSubpath();
        QColor f = col[c];
        f.setAlpha(90);
        p.fillPath(path, f);
        p.setPen(col[c]);
        for (const QPointF& pt : pts) p.drawPoint(pt);
    }
}

// Vectorscope: croma (U, V) derivado do YUV; cruza em cinza (U=V=128).
void ScopeWidget::buildVectorscope() {
    m_cache = QImage(256, 256, QImage::Format_ARGB32);
    m_cache.fill(Qt::black);
    const int w = m_src.width(), h = m_src.height();
    for (int y = 0; y < h; ++y) {
        const uchar* line = m_src.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const int si = x * 4;
            const double r = line[si + 2], g = line[si + 1], b = line[si + 0];
            const double yy = 0.299 * r + 0.587 * g + 0.114 * b;
            // U = (B-Y)*coef + 128 ; V = (R-Y)*coef + 128
            int u = (int)std::lround((b - yy) * 0.565 + 128.0);
            int v = (int)std::lround((r - yy) * 0.713 + 128.0);
            u = std::clamp(u, 0, 255);
            v = std::clamp(v, 0, 255);
            m_cache.setPixelColor(u, 255 - v, QColor(0, 230, 120));
        }
    }
    QPainter p(&m_cache);
    p.setPen(QColor(70, 70, 70));
    p.drawLine(128, 0, 128, 255);
    p.drawLine(0, 128, 255, 128);
    p.setPen(QColor(120, 120, 120));
    p.drawRect(15, 15, 226, 226); // alvo aproximado de cores de segurança
}

void ScopeWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_cache.isNull()) {
        p.setPen(QColor(160, 160, 160));
        p.drawText(rect(), Qt::AlignCenter, tr("Sem quadro - mova o playhead"));
        return;
    }
    const double scale = std::min((double)width() / 256.0, (double)height() / 256.0);
    const int dw = (int)(256.0 * scale), dh = (int)(256.0 * scale);
    const QRect dst((width() - dw) / 2, (height() - dh) / 2, dw, dh);
    p.drawImage(dst, m_cache);
    p.setPen(QColor(120, 120, 120));
    p.drawRect(dst.adjusted(-1, -1, 0, 0));
}