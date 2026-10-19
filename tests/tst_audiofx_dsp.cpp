// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Teste do DSP de áudio do preview (AudioFxDsp: AudioFxFallback/GraphicEq/
// ParametricEqDsp) — extraído de PreviewWidget.cpp no F4.
//
// Antes da extração o DSP era uma classe aninhada no .cpp de 5k linhas e o
// ROADMAP marcava o teste de paridade Rust↔C++ como pendente justamente por
// isso. Aqui ficam as invariantes básicas (identidade, energia, clamp) que
// precisam valer em qualquer porta do DSP (C++ fallback ou Rust).

#include <QtTest>

#include "ui/audio/AudioFxDsp.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

// Seno estéreo interleaved S16 de amplitude `amp` e frequência `freq`.
std::vector<int16_t> makeSine(int frames, double freq, double fs, double amp) {
    std::vector<int16_t> buf(frames * 2);
    for (int i = 0; i < frames; ++i) {
        const double v = amp * std::sin(2.0 * M_PI * freq * i / fs);
        const int16_t s = (int16_t)std::lround(std::clamp(v, -1.0, 1.0) * 32767.0);
        buf[2 * i] = s;
        buf[2 * i + 1] = s;
    }
    return buf;
}

double rms(const std::vector<int16_t>& buf) {
    double acc = 0.0;
    for (int16_t s : buf) acc += (double)s * (double)s;
    return std::sqrt(acc / (double)buf.size());
}

} // namespace

class tst_AudioFxDsp : public QObject {
    Q_OBJECT
private slots:
    // Biquad peaking com ganho 0 dB é identidade (impulso → y=x; seno → igual).
    void biquadPeakingZeroGainIsIdentity() {
        AudioFxFallback::Biquad b;
        b.peaking(1000.0, 1.0, 0.0, 48000.0);
        const double xs[] = { 0.0, 0.5, -0.25, 1.0, -1.0, 0.125, 0.0 };
        for (double x : xs) {
            const double y = b.tick(x);
            QVERIFY2(std::fabs(y - x) < 1e-9,
                     qPrintable(QStringLiteral("y=%1 x=%2").arg(y).arg(x)));
        }
    }

    // GraphicEq neutro (todas as bandas em 0 dB) não toca o buffer.
    void graphicEqNeutralIsPassthrough() {
        GraphicEq eq;
        eq.setGains(QVector<double>(kGraphicEqBands, 0.0));
        auto buf = makeSine(480, 440.0, 48000.0, 0.5);
        const auto ref = buf;
        eq.process(buf.data(), 480);
        QCOMPARE(buf, ref);
    }

    // GraphicEq com +6 dB em todas as bandas aumenta a energia de um seno
    // dentro da banda (440 Hz está entre 250 e 500 — ganho total > 0).
    void graphicEqBoostIncreasesEnergy() {
        GraphicEq eq;
        eq.setGains(QVector<double>(kGraphicEqBands, 6.0));
        auto buf = makeSine(4800, 440.0, 48000.0, 0.1);
        const double before = rms(buf);
        eq.process(buf.data(), 4800);
        const double after = rms(buf);
        QVERIFY2(after > before,
                 qPrintable(QStringLiteral("antes=%1 depois=%2").arg(before).arg(after)));
    }

    // ParametricEqDsp com bandas desabilitadas é passthrough; habilitada com
    // ganho muda a energia.
    void parametricEqDisabledIsPassthrough() {
        ParametricEqDsp eq;
        QVector<ParametricBand> bands(1);
        bands[0].enabled = false;
        bands[0].type = EqBell;
        bands[0].freq = 1000.0;
        bands[0].gain = 6.0;
        bands[0].q = 1.0;
        eq.setBands(bands);
        auto buf = makeSine(480, 1000.0, 48000.0, 0.25);
        const auto ref = buf;
        eq.process(buf.data(), 480);
        QCOMPARE(buf, ref);

        bands[0].enabled = true;
        eq.setBands(bands);
        const double before = rms(ref);
        auto boosted = ref;
        eq.process(boosted.data(), 480);
        QVERIFY(rms(boosted) > before);
    }

    // AudioFxFallback: sem efeitos ligados é passthrough; inversão de fase
    // multiplica por -1.
    void fallbackPassthroughAndInvert() {
        AudioFxFallback fx;
        fx.configure(0.0, 0.0, 0.0, /*denoise=*/false, 12.0,
                     /*invertPhase=*/false, /*normalize=*/false,
                     /*reverb=*/false, 0.35, 0.5);
        fx.resetState();
        auto buf = makeSine(960, 440.0, 48000.0, 0.25);
        const auto ref = buf;
        fx.process(buf.data(), 960);
        // Tolerância generosa: o AGC/softclip desligados não devem mexer, mas
        // o denoise desligado pode deixar o estágio de ganho 1.0 exato.
        for (size_t i = 0; i < ref.size(); ++i)
            QVERIFY2(std::abs((int)buf[i] - (int)ref[i]) <= 1,
                     qPrintable(QStringLiteral("i=%1 buf=%2 ref=%3")
                                    .arg(i).arg(buf[i]).arg(ref[i])));

        fx.configure(0.0, 0.0, 0.0, false, 12.0,
                     /*invertPhase=*/true, false, false, 0.35, 0.5);
        fx.resetState();
        auto inv = ref;
        fx.process(inv.data(), 960);
        for (size_t i = 0; i < ref.size(); ++i)
            QVERIFY2(std::abs((int)inv[i] + (int)ref[i]) <= 2,
                     qPrintable(QStringLiteral("i=%1 inv=%2 ref=%3")
                                    .arg(i).arg(inv[i]).arg(ref[i])));
    }

    // O DSP nunca pode estourar o domínio: seno quente (0,95) com +12 dB na
    // banda do meio satura em ±1.0 ANTES da quantização. Se o clamp faltasse,
    // 0,95×4 ≈ 3,8 viraria wrap no S16 e a metade positiva da onda apareceria
    // negativa. Checamos isso depois do transiente inicial (meio do buffer).
    void fallbackClampsHotSine() {
        AudioFxFallback fx;
        fx.configure(0.0, 12.0, 0.0, false, 12.0, false, false, false, 0.35, 0.5);
        fx.resetState();
        constexpr int kFrames = 4800;
        auto in = makeSine(kFrames, 1000.0, 48000.0, 0.95);
        auto out = in;
        fx.process(out.data(), kFrames);
        for (int i = kFrames / 2; i < kFrames; ++i) {
            if (in[2 * i] > 4000) // metade positiva da onda, já com o DSP aquecido
                QVERIFY2(out[2 * i] > 0,
                         qPrintable(QStringLiteral("wrap? in=%1 out=%2 i=%3")
                                        .arg(in[2 * i]).arg(out[2 * i]).arg(i)));
        }
    }
};

QTEST_MAIN(tst_AudioFxDsp)
#include "tst_audiofx_dsp.moc"
