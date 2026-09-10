// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Funções de efeito LAINKA extraídas do PreviewWidget para reuso no export.
// Header-only: todas as funções são inline/estáticas.

#pragma once

#include <QImage>
#include <QPainter>
#include <QTransform>
#include <QVector>
#include <QString>
#include <cmath>
#include <algorithm>
#include <climits>

namespace LainkaFx {

// ── Hash determinístico por clip+tempo ────────────────────────────────

static inline uint lainkaHash(const QString& clipId, double t) {
    uint h = qHash(clipId);
    const auto* p = reinterpret_cast<const uint*>(&t);
    h ^= qHash(*p) * 2654435761u;
    return h;
}

static inline uint lainkaHashN(const QString& clipId, double t, uint n) {
    uint h = lainkaHash(clipId, t) ^ (n * 0x9E3779B9u);
    h ^= h >> 16;
    return h;
}

static inline double lainkaQuantizeTime(double srcT, int skip, double fps) {
    if (skip <= 1) return srcT;
    const double interval = skip / fps;
    const double q = std::floor(srcT / interval) * interval;
    return q + interval * 0.5;
}

// ── Image Pool ────────────────────────────────────────────────────────

namespace ImgPool {
    struct Pool {
        QImage bufs[4];
        int count = 0;
    };

    static Pool& poolFor(QImage::Format fmt, int /*w*/, int /*h*/) {
        static Pool p32p;
        static Pool g8;
        if (fmt == QImage::Format_Grayscale8) return g8;
        return p32p;
    }

    static QImage get(QImage::Format fmt, int w, int h) {
        Pool& p = poolFor(fmt, w, h);
        for (int i = 0; i < p.count; ++i) {
            if (p.bufs[i].width() == w && p.bufs[i].height() == h
                && p.bufs[i].format() == fmt) {
                QImage img = p.bufs[i];
                p.bufs[i] = p.bufs[--p.count];
                return img;
            }
        }
        return QImage(w, h, fmt);
    }

    static void release(QImage& img) {
        if (img.isNull()) return;
        Pool& p = poolFor(img.format(), img.width(), img.height());
        if (p.count < 4) {
            p.bufs[p.count++] = img;
        }
        img = QImage();
    }

    static QVector<double>& warpDx() { static QVector<double> v; return v; }
    static QVector<double>& warpDy() { static QVector<double> v; return v; }
}

// ── Efeito LAINKA completo ────────────────────────────────────────────

static inline QImage lainkaApplyFx(const QImage& src, const QString& clipId, double t,
                            int skip, double jitterPos, double jitterRot,
                            double jitterScale, double flicker, double flickerSpeed,
                            double warpAmount, double warpSpeed, int warpGrid,
                            double onionSkin, double dustAmount, double scratchAmount,
                            double motionBlur, double opacity, int targetFps,
                            int antialias, const QImage& prevFrame) {
    if (src.isNull() || src.width() < 2 || src.height() < 2) return src;
    if (jitterPos < 1e-6 && jitterRot < 1e-6 && jitterScale < 1e-6 &&
        flicker < 1e-6 && warpAmount < 1e-6 && dustAmount < 1e-6 &&
        scratchAmount < 1e-6 && motionBlur < 1e-6 && opacity > 99.9 &&
        onionSkin < 1e-6)
        return src;

    const int sw = src.width(), sh = src.height();
    const bool smooth = antialias >= 1;
    const bool highQuality = antialias >= 2;
    double tScaled = t * (flickerSpeed / 50.0);

    // 1) Position jitter
    QImage out = src;
    if (jitterPos > 1e-6) {
        const double nx = ((lainkaHashN(clipId, t, 0) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
        const double ny = ((lainkaHashN(clipId, t, 1) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
        const int maxPx = qMax(1, (int)std::lround(jitterPos * 0.08));
        const int dx = (int)std::lround(nx * maxPx);
        const int dy = (int)std::lround(ny * maxPx);
        if (dx != 0 || dy != 0) {
            QPainter p(&out);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.fillRect(out.rect(), Qt::transparent);
            p.drawImage(dx, dy, src);
        }
    }

    // 2) Rotation jitter
    if (jitterRot > 1e-6) {
        const double n = ((lainkaHashN(clipId, tScaled, 2) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
        const double angle = n * jitterRot * 0.015;
        if (std::abs(angle) > 0.01) {
            QTransform tf;
            tf.translate(sw / 2.0, sh / 2.0);
            tf.rotate(angle);
            tf.translate(-sw / 2.0, -sh / 2.0);
            QImage rotated = out.transformed(tf, smooth ? Qt::SmoothTransformation : Qt::FastTransformation);
            QImage padded = ImgPool::get(QImage::Format_ARGB32_Premultiplied, sw, sh);
            padded.fill(Qt::transparent);
            QPainter pp(&padded);
            pp.drawImage((sw - rotated.width()) / 2,
                         (sh - rotated.height()) / 2, rotated);
            ImgPool::release(out);
            out = padded;
        }
    }

    // 3) Scale jitter
    if (jitterScale > 1e-6) {
        const double n = ((lainkaHashN(clipId, tScaled, 3) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
        const double factor = 1.0 + n * jitterScale * 0.001;
        if (std::abs(factor - 1.0) > 1e-6) {
            const int nw = qMax(1, (int)std::lround(sw * factor));
            const int nh = qMax(1, (int)std::lround(sh * factor));
            QImage scaled = out.scaled(nw, nh, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
            QImage padded = ImgPool::get(QImage::Format_ARGB32_Premultiplied, sw, sh);
            padded.fill(Qt::transparent);
            QPainter pp(&padded);
            pp.drawImage((sw - nw) / 2, (sh - nh) / 2, scaled);
            ImgPool::release(out);
            out = padded;
        }
    }

    // 4) Flicker
    if (flicker > 1e-6) {
        const double n = ((lainkaHashN(clipId, tScaled, 4) & 0xFF) / 255.0);
        const double factor = 1.0 + (n * 2.0 - 1.0) * std::min(flicker / 100.0, 0.15);
        for (int y = 0; y < out.height(); ++y) {
            uchar* line = out.scanLine(y);
            for (int x = 0; x < out.width(); ++x) {
                const int si = x * 4;
                line[si + 2] = (uchar)std::clamp((int)(line[si + 2] * factor), 0, 255);
                line[si + 1] = (uchar)std::clamp((int)(line[si + 1] * factor), 0, 255);
                line[si + 0] = (uchar)std::clamp((int)(line[si + 0] * factor), 0, 255);
            }
        }
    }

    // 5) Warp distortion
    if (warpAmount > 1e-6 && warpGrid >= 2) {
        const double warpT = t * (warpSpeed / 50.0);
        const double amp = warpAmount * 0.008;
        const int cols = warpGrid + 1, rows = warpGrid + 1;
        const double cellW = (double)sw / (cols - 1);
        const double cellH = (double)sh / (rows - 1);

        QVector<double>& gridDx = ImgPool::warpDx();
        QVector<double>& gridDy = ImgPool::warpDy();
        const int gridSize = cols * rows;
        gridDx.resize(gridSize);
        gridDy.resize(gridSize);
        for (int gy = 0; gy < rows; ++gy) {
            for (int gx = 0; gx < cols; ++gx) {
                const uint wh = lainkaHashN(clipId, warpT, gx * 1000 + gy);
                gridDx[gy * cols + gx] = ((wh & 0xFF) / 255.0 - 0.5) * amp;
                gridDy[gy * cols + gx] = (((wh >> 8) & 0xFF) / 255.0 - 0.5) * amp;
            }
        }

        // Usa o mesmo formato do source para evitar conversão profunda por frame.
        QImage warped = ImgPool::get(src.format(), sw, sh);
        warped.fill(Qt::transparent);

        for (int y = 0; y < sh; ++y) {
            const double fy = (double)y / cellH;
            const int gy0 = std::clamp((int)fy, 0, rows - 2);
            const double ty = fy - gy0;
            uchar* dst = warped.scanLine(y);
            for (int x = 0; x < sw; ++x) {
                const double fx = (double)x / cellW;
                const int gx0 = std::clamp((int)fx, 0, cols - 2);
                const double tx = fx - gx0;
                const double d00x = gridDx[gy0 * cols + gx0];
                const double d10x = gridDx[gy0 * cols + gx0 + 1];
                const double d01x = gridDx[(gy0 + 1) * cols + gx0];
                const double d11x = gridDx[(gy0 + 1) * cols + gx0 + 1];
                const double dx = (1 - ty) * ((1 - tx) * d00x + tx * d10x)
                                + ty * ((1 - tx) * d01x + tx * d11x);
                const double d00y = gridDy[gy0 * cols + gx0];
                const double d10y = gridDy[gy0 * cols + gx0 + 1];
                const double d01y = gridDy[(gy0 + 1) * cols + gx0];
                const double d11y = gridDy[(gy0 + 1) * cols + gx0 + 1];
                const double dy = (1 - ty) * ((1 - tx) * d00y + tx * d10y)
                                + ty * ((1 - tx) * d01y + tx * d11y);
                const int sx = std::clamp((int)std::lround(x + dx * cellW), 0, sw - 1);
                const int sy = std::clamp((int)std::lround(y + dy * cellH), 0, sh - 1);
                const uchar* srcLine = src.constScanLine(sy);
                const int si = sx * 4;
                const int di = x * 4;
                dst[di]     = srcLine[si];
                dst[di + 1] = srcLine[si + 1];
                dst[di + 2] = srcLine[si + 2];
                dst[di + 3] = srcLine[si + 3];
            }
        }
        ImgPool::release(out);
        out = warped;
    }

    // 6) Dust & dirt
    if (dustAmount > 1e-6) {
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing, false);
        const int count = (int)std::lround(dustAmount * 0.5);
        for (int i = 0; i < count; ++i) {
            const uint dh = lainkaHashN(clipId, t, 100 + i);
            const int px = (int)((dh & 0x7FFF) * (double)sw / 0x7FFF);
            const int py = (int)((((dh >> 15) & 0x7FFF) * (double)sh) / 0x7FFF);
            const int sz = 1 + (dh >> 30);
            const int alpha = 40 + (int)((dh >> 24) & 0x3F);
            p.fillRect(px, py, sz, sz, QColor(0, 0, 0, alpha));
        }
    }

    // 7) Scratches
    if (scratchAmount > 1e-6) {
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing, false);
        const int count = (int)std::lround(scratchAmount * 0.2);
        for (int i = 0; i < count; ++i) {
            const uint sh2 = lainkaHashN(clipId, t, 200 + i);
            const int sx = (int)((sh2 & 0x7FFF) * (double)sw / 0x7FFF);
            const int y1 = (int)((sh2 >> 15) % sh);
            const int len = 20 + (int)((sh2 >> 20) & 0x3FF);
            const int alpha = 30 + (int)((sh2 >> 24) & 0x1F);
            p.setPen(QPen(QColor(255, 255, 255, alpha), 1));
            p.drawLine(sx, y1, sx, qMin(y1 + len, sh - 1));
        }
    }

    // 8) Motion blur
    if (motionBlur > 1e-6 && !prevFrame.isNull()
        && prevFrame.width() == sw && prevFrame.height() == sh) {
        const double blurR = motionBlur * 0.05;
        const int samples = highQuality ? 8 : (smooth ? 5 : 3);

        const int blk = 5;
        const int searchR = std::max(2, (int)std::lround(blurR * 4));
        const int cx = sw / 2, cy = sh / 2;
        const QImage curGray = out.convertToFormat(QImage::Format_Grayscale8);
        const QImage prevGray = prevFrame.convertToFormat(QImage::Format_Grayscale8);

        double bestDx = 0, bestDy = 0;
        int bestErr = INT_MAX;
        for (int dy = -searchR; dy <= searchR; dy += 2) {
            for (int dx = -searchR; dx <= searchR; dx += 2) {
                int err = 0;
                for (int by = -blk; by <= blk; ++by) {
                    const uchar* cur = curGray.constScanLine(cy + by);
                    const uchar* prv = prevGray.constScanLine(cy + by + dy);
                    for (int bx = -blk; bx <= blk; ++bx) {
                        const int cx2 = cx + bx, px2 = cx + bx + dx;
                        if (cx2 >= 0 && cx2 < sw && px2 >= 0 && px2 < sw)
                            err += std::abs((int)cur[cx2] - (int)prv[px2]);
                    }
                }
                if (err < bestErr) { bestErr = err; bestDx = dx; bestDy = dy; }
            }
        }

        if (std::abs(bestDx) > 0.5 || std::abs(bestDy) > 0.5) {
            const double scale = blurR / std::max(1.0, std::sqrt(bestDx * bestDx + bestDy * bestDy));
            const double baseDx = bestDx * scale;
            const double baseDy = bestDy * scale;
            QImage blurred = ImgPool::get(QImage::Format_ARGB32_Premultiplied, sw, sh);
            blurred.fill(Qt::transparent);
            QPainter bp(&blurred);
            for (int s = 0; s < samples; ++s) {
                const double frac = (double)s / (samples - 1);
                const double ox = baseDx * (frac - 0.5) * 2.0;
                const double oy = baseDy * (frac - 0.5) * 2.0;
                bp.setOpacity(1.0 / samples);
                bp.drawImage((int)std::lround(ox), (int)std::lround(oy), out);
            }
            bp.end();
            ImgPool::release(out);
            out = blurred;
        }
    }

    // 9) Onion Skin
    if (onionSkin > 1e-6 && !prevFrame.isNull()
        && prevFrame.width() == sw && prevFrame.height() == sh) {
        QImage ghosted = ImgPool::get(QImage::Format_ARGB32_Premultiplied, sw, sh);
        {
            QPainter cp(&ghosted);
            cp.drawImage(0, 0, out);
            cp.setOpacity(onionSkin / 100.0);
            cp.drawImage(0, 0, prevFrame);
        }
        ImgPool::release(out);
        out = ghosted;
    }

    // 10) Opacidade global
    if (opacity < 99.9) {
        QImage blended = ImgPool::get(QImage::Format_ARGB32_Premultiplied, sw, sh);
        blended.fill(Qt::transparent);
        QPainter bp(&blended);
        bp.setOpacity(opacity / 100.0);
        bp.drawImage(0, 0, out);
        bp.end();
        ImgPool::release(out);
        out = blended;
    }

    return out;
}

} // namespace LainkaFx
