// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Color grade estilo Lumetri (Premiere). Aplicação CPU em QImage para o
// preview; o ProjectExporter espelha o mesmo pipeline no filter_complex
// do ffmpeg. Header-only para o kernel colombina não depender de UI.

#pragma once

#include "colombina/models/Project.h"

#include <QColor>
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace colorgrade {

// Avalia uma curva RGB (pontos 0..1 ordenados por x) em `x` (0..1).
inline double evalCurve(const QVector<QPointF>& pts, double x) {
    if (pts.isEmpty()) return x;
    x = std::clamp(x, 0.0, 1.0);
    if (x <= pts.first().x()) return std::clamp(pts.first().y(), 0.0, 1.0);
    if (x >= pts.last().x()) return std::clamp(pts.last().y(), 0.0, 1.0);
    for (int i = 1; i < pts.size(); ++i) {
        if (x <= pts[i].x()) {
            const double x0 = pts[i - 1].x(), y0 = pts[i - 1].y();
            const double x1 = pts[i].x(), y1 = pts[i].y();
            if (x1 - x0 < 1e-9) return std::clamp(y1, 0.0, 1.0);
            const double t = (x - x0) / (x1 - x0);
            return std::clamp(y0 + t * (y1 - y0), 0.0, 1.0);
        }
    }
    return x;
}

// Curva master + por canal aplicada a um pixel 0..255.
inline void applyCurves(double& r, double& g, double& b, const Clip& c) {
    const bool master = !c.cgMasterCurve.isEmpty();
    const bool rc = !c.cgRCurve.isEmpty();
    const bool gc = !c.cgGCurve.isEmpty();
    const bool bc = !c.cgBCurve.isEmpty();
    if (!master && !rc && !gc && !bc) return;
    double rn = r / 255.0, gn = g / 255.0, bn = b / 255.0;
    if (master) {
        rn = evalCurve(c.cgMasterCurve, rn);
        gn = evalCurve(c.cgMasterCurve, gn);
        bn = evalCurve(c.cgMasterCurve, bn);
    }
    if (rc) rn = evalCurve(c.cgRCurve, rn);
    if (gc) gn = evalCurve(c.cgGCurve, gn);
    if (bc) bn = evalCurve(c.cgBCurve, bn);
    r = rn * 255.0;
    g = gn * 255.0;
    b = bn * 255.0;
}

// Vibrance: saturação seletiva — cores já saturadas sobem menos.
inline double vibranceFactor(double r, double g, double b, double vib) {
    if (vib == 0.0) return 1.0;
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    const double sat = (mx - mn) / 255.0; // 0..1
    // Quanto mais saturado, menos o vibrance adiciona.
    const double w = (1.0 - sat) * (vib >= 0.0 ? vib : -vib);
    return 1.0 + (vib >= 0.0 ? w : -w);
}

// LUT 3D .cube (simplificada): grid N×N×N, interpolação trilinear.
struct CubeLut {
    int n = 0;
    QVector<float> data; // RGB interleaved, size = n*n*n*3
    bool isValid() const { return n >= 2 && data.size() == n * n * n * 3; }

    QColor apply(const QColor& in) const {
        if (!isValid()) return in;
        const float r = std::clamp(in.redF(), 0.f, 1.f);
        const float g = std::clamp(in.greenF(), 0.f, 1.f);
        const float b = std::clamp(in.blueF(), 0.f, 1.f);
        const float s = float(n - 1);
        const float rf = r * s, gf = g * s, bf = b * s;
        const int r0 = int(rf), g0 = int(gf), b0 = int(bf);
        const int r1 = std::min(r0 + 1, n - 1);
        const int g1 = std::min(g0 + 1, n - 1);
        const int b1 = std::min(b0 + 1, n - 1);
        const float fr = rf - r0, fg = gf - g0, fb = bf - b0;
        auto at = [&](int ri, int gi, int bi, int ch) -> float {
            return data[((ri * n + gi) * n + bi) * 3 + ch];
        };
        float out[3];
        for (int ch = 0; ch < 3; ++ch) {
            const float c000 = at(r0, g0, b0, ch);
            const float c100 = at(r1, g0, b0, ch);
            const float c010 = at(r0, g1, b0, ch);
            const float c110 = at(r1, g1, b0, ch);
            const float c001 = at(r0, g0, b1, ch);
            const float c101 = at(r1, g0, b1, ch);
            const float c011 = at(r0, g1, b1, ch);
            const float c111 = at(r1, g1, b1, ch);
            const float c00 = c000 + fr * (c100 - c000);
            const float c10 = c010 + fr * (c110 - c010);
            const float c01 = c001 + fr * (c101 - c001);
            const float c11 = c011 + fr * (c111 - c011);
            const float c0 = c00 + fg * (c10 - c00);
            const float c1 = c01 + fg * (c11 - c01);
            out[ch] = c0 + fb * (c1 - c0);
        }
        return QColor::fromRgbF(std::clamp(out[0], 0.f, 1.f),
                                std::clamp(out[1], 0.f, 1.f),
                                std::clamp(out[2], 0.f, 1.f),
                                in.alphaF());
    }
};

// Parser mínimo de .cube 1D/3D (IRIDAS/Resolve).
inline bool loadCubeFile(const QString& path, CubeLut& out, QString* err = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = QStringLiteral("Não abriu %1").arg(path);
        return false;
    }
    int n = 0;
    QVector<float> vals;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        if (line.startsWith(QLatin1String("TITLE"), Qt::CaseInsensitive)) continue;
        if (line.startsWith(QLatin1String("DOMAIN_MIN"), Qt::CaseInsensitive)
            || line.startsWith(QLatin1String("DOMAIN_MAX"), Qt::CaseInsensitive))
            continue;
        if (line.startsWith(QLatin1String("LUT_3D_SIZE"), Qt::CaseInsensitive)) {
            const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() >= 2) n = parts.last().toInt();
            continue;
        }
        if (line.startsWith(QLatin1String("LUT_1D_SIZE"), Qt::CaseInsensitive))
            continue; // 1D não suportado no MVP
        if (line.startsWith(QLatin1String("LUT_3D_INPUT_RANGE"), Qt::CaseInsensitive)
            || line.startsWith(QLatin1String("LUT_3D_OUTPUT_RANGE"), Qt::CaseInsensitive))
            continue;
        // Dados: floats separados por espaço/tab.
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("[\\s]+")),
                                             Qt::SkipEmptyParts);
        if (parts.size() < 3) continue;
        bool ok = false;
        const float r = parts[0].toFloat(&ok);
        if (!ok) continue;
        const float g = parts[1].toFloat(&ok);
        const float b = parts[2].toFloat(&ok);
        if (!ok) continue;
        vals << r << g << b;
    }
    if (n < 2 || vals.size() != n * n * n * 3) {
        if (err) *err = QStringLiteral("LUT inválida (size=%1, floats=%2)")
                            .arg(n).arg(vals.size());
        return false;
    }
    out.n = n;
    out.data = vals;
    return true;
}

// Pipeline completo do grade num quadro BGRA/ARGB32.
// Ordem alinhada ao Lumetri e ao ProjectExporter.
inline void applyToImage(QImage& img, const Clip& c, const CubeLut* lut = nullptr) {
    if (img.isNull()) return;
    if (!c.hasColorGrade()) return;

    img = img.convertToFormat(QImage::Format_ARGB32);

    // Precomputa LUT de curvas master (256 entradas) para ir mais rápido.
    uchar lutMaster[256];
    uchar lutR[256], lutG[256], lutB[256];
    const bool useMaster = !c.cgMasterCurve.isEmpty();
    const bool useR = !c.cgRCurve.isEmpty();
    const bool useG = !c.cgGCurve.isEmpty();
    const bool useB = !c.cgBCurve.isEmpty();
    for (int i = 0; i < 256; ++i) {
        const double x = i / 255.0;
        double v = x;
        if (useMaster) v = evalCurve(c.cgMasterCurve, v);
        lutMaster[i] = (uchar)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
        double vr = useMaster ? evalCurve(c.cgMasterCurve, x) : x;
        double vg = useMaster ? evalCurve(c.cgMasterCurve, x) : x;
        double vb = useMaster ? evalCurve(c.cgMasterCurve, x) : x;
        if (useR) vr = evalCurve(c.cgRCurve, vr);
        if (useG) vg = evalCurve(c.cgGCurve, vg);
        if (useB) vb = evalCurve(c.cgBCurve, vb);
        lutR[i] = (uchar)std::lround(std::clamp(vr, 0.0, 1.0) * 255.0);
        lutG[i] = (uchar)std::lround(std::clamp(vg, 0.0, 1.0) * 255.0);
        lutB[i] = (uchar)std::lround(std::clamp(vb, 0.0, 1.0) * 255.0);
    }

    const double exposure = c.cgExposure;          // -5..+5 EV
    const double contrast = c.cgContrast;          // -100..100
    const double highlights = c.cgHighlights;      // -100..100
    const double shadows = c.cgShadows;
    const double whites = c.cgWhites;
    const double blacks = c.cgBlacks;
    const double saturation = c.cgSaturation;      // -100..100
    const double vibrance = c.cgVibrance;
    const double temperature = c.cgTemperature;    // -100..100 (azul↔laranja)
    const double tint = c.cgTint;                  // -100..100 (verde↔magenta)
    const double faded = c.cgFadedFilm;            // 0..100
    const double sharpen = c.cgSharpen;            // 0..100
    const double vig = c.cgVignette;               // -100..100
    const double vigFeather = std::clamp(c.cgVignetteFeather, 0.0, 100.0);
    const double lutMix = std::clamp(c.cgLutStrength, 0.0, 1.0);
    const double gradeMix = std::clamp(c.cgBlend, 0.0, 1.0);

    // Temperatura/tint como offset de canal (aproximação de color temp).
    const double tempR = temperature * 0.35;  // + = mais quente (R↑ B↓)
    const double tempB = -temperature * 0.35;
    const double tintG = -tint * 0.25;        // + = mais magenta (G↓)

    // Faded film: sobe pretos + desatura levemente.
    const double fadeBlack = faded * 0.4;     // 0..40 lift nos pretos
    const double fadeSat = 1.0 - faded * 0.004;

    const double cx = img.width() * 0.5;
    const double cy = img.height() * 0.5;
    const double maxR = std::sqrt(cx * cx + cy * cy);
    const double vigInner = 1.0 - std::clamp(vigFeather, 1.0, 100.0) / 100.0 * 0.7;

    for (int y = 0; y < img.height(); ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < img.width(); ++x) {
            const int si = x * 4;
            double r = line[si + 2], g = line[si + 1], b = line[si + 0];

            // LUT 3D (antes da grade numérica, como Resolve: look primeiro).
            if (lut && lut->isValid() && lutMix > 0.0) {
                const QColor mapped = lut->apply(QColor::fromRgb(
                    int(r), int(g), int(b)));
                r = r + lutMix * (mapped.red() - r);
                g = g + lutMix * (mapped.green() - g);
                b = b + lutMix * (mapped.blue() - b);
            }

            // Exposure (stops).
            if (exposure != 0.0) {
                const double e = std::pow(2.0, exposure);
                r *= e; g *= e; b *= e;
            }

            // Temperature / tint.
            r += tempR; b += tempB; g += tintG;

            // Highlights / shadows / whites / blacks (luma-guided).
            {
                const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                const double n = std::clamp(luma / 255.0, 0.0, 1.0);
                // Peso: sombras baixo n, highlights alto n.
                const double shW = std::pow(1.0 - n, 2.0);
                const double hiW = std::pow(n, 2.0);
                const double whW = std::pow(n, 4.0);
                const double blW = std::pow(1.0 - n, 4.0);
                const double dSh = shadows * 0.5 * shW;
                const double dHi = highlights * 0.5 * hiW;
                const double dWh = whites * 0.4 * whW;
                const double dBl = blacks * 0.4 * blW;
                r += dSh + dHi + dWh + dBl;
                g += dSh + dHi + dWh + dBl;
                b += dSh + dHi + dWh + dBl;
            }

            // Contrast (pivot 0.5).
            if (contrast != 0.0) {
                const double f = 1.0 + contrast / 100.0;
                r = f * (r - 128.0) + 128.0;
                g = f * (g - 128.0) + 128.0;
                b = f * (b - 128.0) + 128.0;
            }

            // Saturation + vibrance.
            {
                double sat = 1.0 + saturation / 100.0;
                const double vib = vibranceFactor(r, g, b, vibrance / 100.0);
                sat *= vib * fadeSat;
                if (std::fabs(sat - 1.0) > 1e-6) {
                    const double gray = 0.299 * r + 0.587 * g + 0.114 * b;
                    r = gray + sat * (r - gray);
                    g = gray + sat * (g - gray);
                    b = gray + sat * (b - gray);
                }
            }

            // Faded film: lift pretos.
            if (fadeBlack > 0.0) {
                r += fadeBlack * (1.0 - r / 255.0);
                g += fadeBlack * (1.0 - g / 255.0);
                b += fadeBlack * (1.0 - b / 255.0);
            }

            // Lift/Gamma/Gain clássico (compat com o grade antigo).
            if (c.hasColorGrade()) {
                const double li[3] = {c.liftR, c.liftG, c.liftB};
                const double gi[3] = {c.gammaR, c.gammaG, c.gammaB};
                const double ga[3] = {c.gainR, c.gainG, c.gainB};
                double chv[3] = {r, g, b};
                for (int i = 0; i < 3; ++i) {
                    double v = chv[i];
                    v += li[i] * (255.0 - v);
                    v = 255.0 * std::pow(std::max(v, 0.0) / 255.0, 1.0 / std::max(0.1, gi[i]));
                    v += ga[i] * v;
                    chv[i] = std::clamp(v, 0.0, 255.0);
                }
                r = chv[0]; g = chv[1]; b = chv[2];
            }

            // Curves RGB.
            applyCurves(r, g, b, c);

            // Blend com o original (cgBlend < 1 = efeito mais suave).
            if (gradeMix < 1.0) {
                const double or_ = line[si + 2], og = line[si + 1], ob = line[si + 0];
                r = or_ + gradeMix * (r - or_);
                g = og + gradeMix * (g - og);
                b = ob + gradeMix * (b - ob);
            }

            // Vignette.
            if (c.cgHasVignette()) {
                const double dx = (x - cx) / maxR;
                const double dy = (y - cy) / maxR;
                const double d = std::sqrt(dx * dx + dy * dy); // 0 centro..1 canto
                double f = 1.0;
                if (d > vigInner) {
                    const double t = (d - vigInner) / std::max(1e-6, 1.0 - vigInner);
                    // vig < 0 = escurece bordas; > 0 = clareia (raro).
                    f = 1.0 + (vig / 100.0) * t;
                }
                r *= f; g *= f; b *= f;
            }

            // Sharpen (unsharp 3x3 leve) — só se sharpen > 0; aproximação
            // por mistura com o vizinho quando disponível fica cara por pixel
            // com vizinhança; usa high-pass simples com o pixel atual vs blur
            // local aproximado (média dos 4 vizinhos quando existirem).
            if (sharpen > 0.0 && x > 0 && y > 0
                && x + 1 < img.width() && y + 1 < img.height()) {
                // Média 3x3 do canal R só para o high-pass (barato).
                const uchar* up = line - img.bytesPerLine();
                const uchar* dn = line + img.bytesPerLine();
                auto avg = [&](int off) {
                    return (up[off + 2] + line[off + 2] + dn[off + 2]
                            + up[off - 4 + 2] + up[off + 4 + 2]
                            + dn[off - 4 + 2] + dn[off + 4 + 2]
                            + line[off - 4 + 2] + line[off + 4 + 2]) / 9.0;
                };
                const double blur = avg(si);
                const double amount = sharpen / 100.0 * 0.8;
                r += amount * (r - blur);
                // G e B usam o mesmo delta de luma para não inventar cor.
                const double d = amount * (r - blur);
                g += d; b += d;
            }

            line[si + 2] = (uchar)std::clamp((int)std::lround(r), 0, 255);
            line[si + 1] = (uchar)std::clamp((int)std::lround(g), 0, 255);
            line[si + 0] = (uchar)std::clamp((int)std::lround(b), 0, 255);
        }
    }
}

// Filtros ffmpeg equivalentes ao grade (paridade preview↔export).
// Retorna lista de strings prontas para o filter_complex (sem vírgula inicial).
inline QStringList ffmpegFilters(const Clip& c, const QString& lutPath = QString()) {
    QStringList out;
    if (c.cgLutStrength > 0.0 && !lutPath.isEmpty()) {
        // lut3d não tem mix nativo; o export aplica a 100% se strength>0.
        // O preview interpola — diferença aceitável se strength=1 (padrão).
        out << QStringLiteral("lut3d=file='%1'").arg(lutPath);
    }
    if (c.cgExposure != 0.0 || c.cgContrast != 0.0 || c.cgSaturation != 0.0) {
        // eq do ffmpeg: brightness em [-1,1] ≈ EV*0.35; contrast em [0,2].
        const double br = std::clamp(c.cgExposure * 0.35, -1.0, 1.0);
        const double ct = std::clamp(1.0 + c.cgContrast / 100.0, 0.0, 2.0);
        const double sa = std::clamp(1.0 + c.cgSaturation / 100.0, 0.0, 2.0);
        out << QStringLiteral("eq=brightness=%1:contrast=%2:saturation=%3")
                   .arg(br, 0, 'f', 4)
                   .arg(ct, 0, 'f', 4)
                   .arg(sa, 0, 'f', 4);
    }
    if (c.cgTemperature != 0.0 || c.cgTint != 0.0) {
        // colortemperature (ffmpeg >= 6) ou approx via colorbalance.
        const double rs = std::clamp(c.cgTemperature * 0.0035, -0.5, 0.5);
        const double bs = -rs;
        const double gs = std::clamp(-c.cgTint * 0.0025, -0.5, 0.5);
        out << QStringLiteral("colorbalance=rs=%1:gs=%2:bs=%3:rm=0:gm=0:bm=0"
                              ":rh=%4:gh=%5:bh=%6")
                   .arg(rs, 0, 'f', 4)
                   .arg(gs, 0, 'f', 4)
                   .arg(bs, 0, 'f', 4)
                   .arg(rs * 0.5, 0, 'f', 4)
                   .arg(gs * 0.5, 0, 'f', 4)
                   .arg(bs * 0.5, 0, 'f', 4);
    }
    if (c.hasColorGrade()) {
        out << QStringLiteral("colorbalance=rs=%1:gs=%2:bs=%3:rm=0:gm=0:bm=0"
                              ":rh=%4:gh=%5:bh=%6")
                   .arg(std::clamp(c.liftR, -1.0, 1.0), 0, 'f', 4)
                   .arg(std::clamp(c.liftG, -1.0, 1.0), 0, 'f', 4)
                   .arg(std::clamp(c.liftB, -1.0, 1.0), 0, 'f', 4)
                   .arg(std::clamp(c.gainR, -1.0, 1.0), 0, 'f', 4)
                   .arg(std::clamp(c.gainG, -1.0, 1.0), 0, 'f', 4)
                   .arg(std::clamp(c.gainB, -1.0, 1.0), 0, 'f', 4);
        out << QStringLiteral("eq=gamma_r=%1:gamma_g=%2:gamma_b=%3")
                   .arg(std::clamp(c.gammaR, 0.1, 4.0), 0, 'f', 4)
                   .arg(std::clamp(c.gammaG, 0.1, 4.0), 0, 'f', 4)
                   .arg(std::clamp(c.gammaB, 0.1, 4.0), 0, 'f', 4);
    }
    if (!c.cgMasterCurve.isEmpty() || !c.cgRCurve.isEmpty()
        || !c.cgGCurve.isEmpty() || !c.cgBCurve.isEmpty()) {
        auto curveSpec = [](const QVector<QPointF>& pts) {
            if (pts.isEmpty()) return QString();
            QStringList parts;
            for (const QPointF& p : pts)
                parts << QStringLiteral("%1/%2")
                             .arg(p.x(), 0, 'f', 4)
                             .arg(p.y(), 0, 'f', 4);
            return parts.join(QLatin1Char(' '));
        };
        const QString master = curveSpec(c.cgMasterCurve);
        const QString rr = curveSpec(c.cgRCurve);
        const QString gg = curveSpec(c.cgGCurve);
        const QString bb = curveSpec(c.cgBCurve);
        QStringList args;
        if (!master.isEmpty()) args << QStringLiteral("all='%1'").arg(master);
        if (!rr.isEmpty()) args << QStringLiteral("r='%1'").arg(rr);
        if (!gg.isEmpty()) args << QStringLiteral("g='%1'").arg(gg);
        if (!bb.isEmpty()) args << QStringLiteral("b='%1'").arg(bb);
        out << QStringLiteral("curves=%1").arg(args.join(QLatin1Char(':')));
    }
    if (c.cgFadedFilm > 0.0) {
        // Approx: lift pretos via eq + leve desaturação.
        const double lift = std::clamp(c.cgFadedFilm * 0.0015, 0.0, 0.15);
        out << QStringLiteral("eq=brightness=%1:saturation=%2")
                   .arg(lift, 0, 'f', 4)
                   .arg(std::clamp(1.0 - c.cgFadedFilm * 0.004, 0.0, 1.0), 0, 'f', 4);
    }
    if (c.cgVignette != 0.0) {
        // ffmpeg vignette: angle em radianos; negativo escurece.
        const double ang = std::clamp(std::fabs(c.cgVignette) / 100.0 * 0.9, 0.0, 0.9);
        out << QStringLiteral("vignette=angle=%1").arg(c.cgVignette < 0 ? -ang : ang, 0, 'f', 4);
    }
    if (c.cgSharpen > 0.0) {
        out << QStringLiteral("unsharp=5:5:%1:5:5:0.0")
                   .arg(std::clamp(c.cgSharpen / 100.0 * 1.5, 0.0, 2.0), 0, 'f', 4);
    }
    return out;
}

} // namespace colorgrade
