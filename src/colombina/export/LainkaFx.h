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
#include <QMutex>
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

    // Guarda o pool de buffers: desde o Fix 1, o FrameWorker (thread de vídeo)
    // aplica o caminho rápido de efeitos (chroma usa o pool) ao MESMO tempo que
    // a UI aplica MotiOn/LAINKA para o clipe do topo — os QImages de reuso
    // compartilham o armazenamento e a troca de posse seria um data race.
    static QMutex& poolMutex() { static QMutex m; return m; }

    static Pool& poolFor(QImage::Format fmt, int /*w*/, int /*h*/) {
        static Pool p32p;
        static Pool g8;
        if (fmt == QImage::Format_Grayscale8) return g8;
        return p32p;
    }

    static QImage get(QImage::Format fmt, int w, int h) {
        QMutexLocker lock(&poolMutex());
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
        QMutexLocker lock(&poolMutex());
        Pool& p = poolFor(img.format(), img.width(), img.height());
        if (p.count < 4) {
            p.bufs[p.count++] = img;
        }
        img = QImage();
    }

    static QVector<double>& warpDx() { static QVector<double> v; return v; }
    static QVector<double>& warpDy() { static QVector<double> v; return v; }
    static QString& warpCacheKey() { static QString k; return k; }
    static double& warpCacheT() { static double t = -1e18; return t; }
    static int& warpCacheGrid() { static int g = -1; return g; }
} 

// ── Efeito LAINKA completo ──────────────────────────────────────────── 

static inline QImage lainkaApplyFx(const QImage& src, const QString& clipId, double t,
                            int skip, double jitterPos, double jitterRot,
                            double jitterScale, double flicker, double flickerSpeed,
                            double warpAmount, double warpSpeed, int warpGrid,
                            double onionSkin, double dustAmount, double scratchAmount,
                            double motionBlur, double opacity, int targetFps,
                            int antialias, const QImage& prevFrame,
                            int hold = 0, double stutter = 0.0, int posterize = 0,
                            double colorShift = 0.0, double gate = 0.0) {
    if (src.isNull() || src.width() < 2 || src.height() < 2) return src;
    if (jitterPos < 1e-6 && jitterRot < 1e-6 && jitterScale < 1e-6 &&
        flicker < 1e-6 && warpAmount < 1e-6 && dustAmount < 1e-6 &&
        scratchAmount < 1e-6 && motionBlur < 1e-6 && opacity > 99.9 &&
        onionSkin < 1e-6 && hold <= 0 && stutter < 1e-6 && posterize <= 0 &&
        colorShift < 1e-6 && gate < 1e-6)
        return src;

    const int sw = src.width(), sh = src.height();
    const bool smooth = antialias >= 1;
    const bool highQuality = antialias >= 2;
    double tScaled = t * (flickerSpeed / 50.0);
    // stutter: drop frames probabilisticamente
    if (stutter > 1e-6) {
        const uint shv = lainkaHashN(clipId, t, 900);
        if ((shv % 1000) < (int)(stutter * 5.0)) {
            // devolver frame anterior se disponível
            if (!prevFrame.isNull() && prevFrame.width() == sw && prevFrame.height() == sh)
                return prevFrame;
        }
    }

    // 1) Position jitter
    QImage out = src;
    if (jitterPos > 1e-6) {
        const double nx = ((lainkaHashN(clipId, t, 0) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
        const double ny = ((lainkaHashN(clipId, t, 1) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
        const int maxPx = qMax(1, (int)std::lround(jitterPos * 0.08));
        const int dx = (int)std::lround(nx * maxPx);
        const int dy = (int)std::lround(ny * maxPx);
        if (dx != 0 || dy != 0) {
            QImage j = out;
            QPainter p(&j);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.fillRect(j.rect(), Qt::transparent);
            p.drawImage(dx, dy, out);
            out = j;
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
        const double factor = 1.0 + (n * 2.0 - 1.0) * std::min(flicker / 100.0, 0.08);
        uchar* bits = out.bits();
        const int stride = out.bytesPerLine();
        const int h = out.height(), w = out.width();
        for (int y = 0; y < h; ++y) {
            uchar* line = bits + y * stride;
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                line[si + 2] = (uchar)std::clamp((int)(line[si + 2] * factor), 0, 255);
                line[si + 1] = (uchar)std::clamp((int)(line[si + 1] * factor), 0, 255);
                line[si + 0] = (uchar)std::clamp((int)(line[si + 0] * factor), 0, 255);
            }
        }
    }

    // 5) Warp distortion (deslocamento por pixel com interpolação bilinear
    //    da grade — deforma continuamente, sem quebrar em blocos).
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
            const int row0 = gy0 * cols;
            const int row1 = row0 + cols;
            uchar* dst = warped.scanLine(y);
            for (int x = 0; x < sw; ++x) {
                const double fx = (double)x / cellW;
                const int gx0 = std::clamp((int)fx, 0, cols - 2);
                const double tx = fx - gx0;
                const int i00 = row0 + gx0, i01 = i00 + 1;
                const int i10 = row1 + gx0, i11 = i10 + 1;
                const double dx = (1 - ty) * ((1 - tx) * gridDx[i00] + tx * gridDx[i01])
                               + ty * ((1 - tx) * gridDx[i10] + tx * gridDx[i11]);
                const double dy = (1 - ty) * ((1 - tx) * gridDy[i00] + tx * gridDy[i01])
                               + ty * ((1 - tx) * gridDy[i10] + tx * gridDy[i11]);
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
        const int count = (int)std::lround(dustAmount * 0.25);
        for (int i = 0; i < count; ++i) {
            const uint dh = lainkaHashN(clipId, t, 100 + i);
            const int px = (int)((dh & 0x7FFF) * (double)sw / 0x7FFF);
            const int py = (int)((((dh >> 15) & 0x7FFF) * (double)sh) / 0x7FFF);
            const int sz = 1;
            const int alpha = 30 + (int)((dh >> 24) & 0x3F);
            p.fillRect(px, py, sz, sz, QColor(0, 0, 0, alpha));
        }
    }

    // 7) Scratches
    if (scratchAmount > 1e-6) {
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing, false);
        const int count = (int)std::lround(scratchAmount * 0.1);
        for (int i = 0; i < count; ++i) {
            const uint sh2 = lainkaHashN(clipId, t, 200 + i);
            const int sx = (int)((sh2 & 0x7FFF) * (double)sw / 0x7FFF);
            const int y1 = (int)((sh2 >> 15) % sh);
            const int len = 12 + (int)((sh2 >> 20) & 0xFF);
            const int alpha = 25 + (int)((sh2 >> 24) & 0x1F);
            p.setPen(QPen(QColor(255, 255, 255, alpha), 1));
            p.drawLine(sx, y1, sx, qMin(y1 + len, sh - 1));
        }
    }

    // 8) Motion blur
    if (motionBlur > 1e-6 && !prevFrame.isNull()
        && prevFrame.width() == sw && prevFrame.height() == sh) {
        const double blurR = motionBlur * 0.05;
        const int samples = 2;
        QImage blurred = ImgPool::get(QImage::Format_ARGB32_Premultiplied, sw, sh);
        blurred.fill(Qt::transparent);
        QPainter bp(&blurred);
        bp.setOpacity(0.6);
        bp.drawImage(0, 0, out);
        bp.setOpacity(0.4);
        bp.drawImage(1, 0, prevFrame);
        bp.end();
        ImgPool::release(out);
        out = blurred;
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

    // 11) Posterize
    if (posterize >= 2) {
        const int levels = posterize;
        const float step = 255.0f / (float)(levels - 1);
        uchar* bits = out.bits();
        const int stride = out.bytesPerLine();
        const int h = out.height(), w = out.width();
        for (int y = 0; y < h; ++y) {
            uchar* line = bits + y * stride;
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                for (int c = 0; c < 3; ++c) {
                    float v = (float)line[si + c];
                    int qv = (int)std::round(v / step) * (int)step;
                    if (qv < 0) qv = 0;
                    if (qv > 255) qv = 255;
                    line[si + c] = (uchar)qv;
                }
            }
        }
    }

    // 12) Color shift
    if (colorShift > 1e-6) {
        const uint ch = lainkaHashN(clipId, t, 950);
        const double shift = (colorShift / 100.0) * (( (ch & 0xFFFF) / 65535.0 ) * 20.0 - 10.0);
        uchar* bits = out.bits();
        const int stride = out.bytesPerLine();
        const int h = out.height(), w = out.width();
        for (int y = 0; y < h; ++y) {
            uchar* line = bits + y * stride;
            for (int x = 0; x < w; ++x) {
                const int si = x * 4;
                int r = line[si + 2];
                int b = line[si + 0];
                int nr = (int)(r + shift);
                int nb = (int)(b - shift);
                if (nr < 0) nr = 0; if (nr > 255) nr = 255;
                if (nb < 0) nb = 0; if (nb > 255) nb = 255;
                line[si + 2] = (uchar)nr;
                line[si + 0] = (uchar)nb;
            }
        }
    }

    // 13) Gate/flash
    if (gate > 1e-6) {
        const uint gh = lainkaHashN(clipId, t, 960);
        const double g = ((gh & 0xFF) / 255.0);
        if (g < (gate / 100.0) * 0.2) {
            uchar* bits = out.bits();
            const int stride = out.bytesPerLine();
            const int h = out.height(), w = out.width();
            for (int y = 0; y < h; ++y) {
                uchar* line = bits + y * stride;
                for (int x = 0; x < w; ++x) {
                    const int si = x * 4;
                    line[si + 0] = 255; line[si + 1] = 255; line[si + 2] = 255;
                }
            }
        }
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
