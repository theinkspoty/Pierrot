// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <algorithm>
#include <cmath>

// Biquad (RBJ Audio EQ Cookbook) com estado de filtragem e resposta em
// magnitude. Compartilhado por: DSP do preview (AudioMixer), geração da curva
// nas UIs de EQ e (conceitualmente) pelo export do FFmpeg — assim a curva
// desenhada corresponde ao que se ouve.
struct EqBiquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    // Estado por canal (o objeto é instanciado um por canal).
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    // Bell (peaking). `q` = fator de qualidade (banda).
    void peaking(double freq, double q, double gainDb, double fs) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * freq / fs;
        const double cw = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * std::max(0.05, q));
        const double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * cw) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * cw) / a0;
        a2 = (1.0 - alpha / A) / a0;
    }

    // Low shelf (ganho em 0 Hz). Slope S = 1 (casando com lowshelf do ffmpeg).
    void lowShelf(double freq, double gainDb, double fs) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * freq / fs;
        const double cw = std::cos(w0);
        const double S = 1.0;
        const double alpha =
            0.5 * std::sin(w0) * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
        const double sq = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1.0) + (A - 1.0) * cw + sq;
        b0 = A * ((A + 1.0) - (A - 1.0) * cw + sq) / a0;
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw) / a0;
        b2 = A * ((A + 1.0) - (A - 1.0) * cw - sq) / a0;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw) / a0;
        a2 = ((A + 1.0) + (A - 1.0) * cw - sq) / a0;
    }

    // High shelf (ganho em Nyquist). Slope S = 1.
    void highShelf(double freq, double gainDb, double fs) {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * freq / fs;
        const double cw = std::cos(w0);
        const double S = 1.0;
        const double alpha =
            0.5 * std::sin(w0) * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
        const double sq = 2.0 * std::sqrt(A) * alpha;
        const double a0 = (A + 1.0) - (A - 1.0) * cw + sq;
        b0 = A * ((A + 1.0) + (A - 1.0) * cw + sq) / a0;
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0;
        b2 = A * ((A + 1.0) + (A - 1.0) * cw - sq) / a0;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw) / a0;
        a2 = ((A + 1.0) - (A - 1.0) * cw - sq) / a0;
    }

    inline double tick(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }

    void reset() { x1 = x2 = y1 = y2 = 0.0; }

    // |H(e^{jw})| em dB na frequência `freq` (fs = taxa de amostragem).
    double magnitudeDb(double freq, double fs) const {
        const double w = 2.0 * M_PI * freq / fs;
        const double cw = std::cos(w);
        const double c2 = std::cos(2.0 * w);
        const double num = b0 * b0 + b1 * b1 + b2 * b2
                           + 2.0 * (b0 * b1 + b1 * b2) * cw + 2.0 * b0 * b2 * c2;
        const double den = 1.0 + a1 * a1 + a2 * a2
                           + 2.0 * (a1 + a1 * a2) * cw + 2.0 * a2 * c2;
        return 10.0 * std::log10(std::max(1e-12, num / std::max(1e-12, den)));
    }
};
