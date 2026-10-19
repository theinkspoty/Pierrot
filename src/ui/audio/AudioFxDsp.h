// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

// DSP dos efeitos de áudio do preview (AudioFxFallback/GraphicEq/
// ParametricEqDsp) e o alias `AudioFx` (Rust quando PIERROT_ENABLE_RUST,
// senão o fallback C++). Extraído de PreviewWidget.cpp (F4) para ser
// testável de forma independente (tst_audiofx_dsp).

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <QVector>

#include "colombina/models/Project.h"
#include "colombina/audio/EqBiquad.h"
#ifdef PIERROT_ENABLE_RUST
#include "ui/audio/AudioFxBridge.h"
#endif

// DSP dos efeitos de áudio do preview — réplica em tempo real do que a
// exportação aplica via ffmpeg:
//   equalizer f=120/1000/6000 Q=1  -> biquad peaking por banda
//   aeval (inverter fase)          -> multiplica por -1
//   afftdn (denoise)               -> gate espectral (STFT + piso adaptativo)
//   loudnorm (normalizar -14 LUFS) -> aproximação: AGC lento + soft clip
class AudioFxFallback {
public:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        void peaking(double freq, double q, double gainDb, double fs) {
            const double A = std::pow(10.0, gainDb / 40.0);
            const double w0 = 2.0 * M_PI * freq / fs;
            const double alpha = std::sin(w0) / (2.0 * q);
            const double a0 = 1.0 + alpha / A;
            b0 = (1.0 + alpha * A) / a0;
            b1 = (-2.0 * std::cos(w0)) / a0;
            b2 = (1.0 - alpha * A) / a0;
            a1 = (-2.0 * std::cos(w0)) / a0;
            a2 = (1.0 - alpha / A) / a0;
        }
        inline double tick(double x) {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
        void reset() { x1 = x2 = y1 = y2 = 0.0; }
    };

    // Reverb Schroeder simples (4 combos + 2 allpass num barramento mono).
    struct Comb {
        float buf[8192];
        int len = 1;
        int pos = 0;
        float feedback = 0.0f;
        float damping = 0.0f;
        float filter = 0.0f;
        void setup(int length, float fb, float damp) {
            len = qMax(1, length);
            feedback = fb;
            damping = damp;
            pos = 0;
            filter = 0.0f;
            memset(buf, 0, sizeof(buf));
        }
        inline float tick(float in) {
            const float output = buf[pos];
            filter = output * (1.0f - damping) + filter * damping;
            buf[pos] = in + filter * feedback;
            if (++pos >= len) pos = 0;
            return output;
        }
    };
    struct Allpass {
        float buf[8192];
        int len = 1;
        int pos = 0;
        void setup(int length) {
            len = qMax(1, length);
            pos = 0;
            memset(buf, 0, sizeof(buf));
        }
        inline float tick(float in) {
            const float bufo = buf[pos];
            const float out = -in + bufo;
            buf[pos] = in + bufo * 0.5f;
            if (++pos >= len) pos = 0;
            return out;
        }
    };
    struct SimpleReverb {
        Comb comb[4];
        Allpass allpass[2];
        void setup(double size) {
            const int delays[4] = { 1557, 1617, 1491, 1422 };
            const int apDelays[2] = { 225, 556 };
            const float fb = 0.60f + 0.28f * (float)size;
            const float damp = 0.4f - 0.25f * (float)size;
            for (int i = 0; i < 4; ++i) comb[i].setup(delays[i], fb, damp);
            for (int i = 0; i < 2; ++i) allpass[i].setup(apDelays[i]);
        }
        inline float tick(float in) {
            float o = 0.016f * comb[0].tick(in)
                    + 0.016f * comb[1].tick(in)
                    + 0.023f * comb[2].tick(in)
                    + 0.027f * comb[3].tick(in);
            o = allpass[0].tick(o);
            o = allpass[1].tick(o);
            return o;
        }
    };

    // FFT radix-2 iterativa (idêntica à do kernel Rust).
    static void fftRadix2(QVector<double>& re, QVector<double>& im) {
        const int n = re.size();
        if (n <= 1) return;
        int j = 0;
        for (int i = 1; i < n; ++i) {
            int bit = n >> 1;
            while (j & bit) { j ^= bit; bit >>= 1; }
            j ^= bit;
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const double ang = -2.0 * M_PI / len;
            const double wlenRe = std::cos(ang);
            const double wlenIm = std::sin(ang);
            const int half = len >> 1;
            for (int i = 0; i < n; i += len) {
                double wRe = 1.0, wIm = 0.0;
                for (int k = 0; k < half; ++k) {
                    const double uRe = re[i + k];
                    const double uIm = im[i + k];
                    const int idx = i + k + half;
                    const double vRe = re[idx] * wRe - im[idx] * wIm;
                    const double vIm = re[idx] * wIm + im[idx] * wRe;
                    re[i + k] = uRe + vRe;
                    im[i + k] = uIm + vIm;
                    re[idx] = uRe - vRe;
                    im[idx] = uIm - vIm;
                    const double nWr = wRe * wlenRe - wIm * wlenIm;
                    wIm = wRe * wlenIm + wIm * wlenRe;
                    wRe = nWr;
                }
            }
        }
    }
    static void ifftRadix2(QVector<double>& re, QVector<double>& im) {
        for (double& v : im) v = -v;
        fftRadix2(re, im);
        const double n = (double)re.size();
        for (int i = 0; i < re.size(); ++i) {
            re[i] /= n;
            im[i] = -im[i] / n;
        }
    }

    // Denoise espectral: STFT 512/hop 256 + piso por mediana + gate em dB.
    // Fallback C++ do SpectralDenoise de pierrot_audiofx/src/lib.rs — as duas
    // implementações devem produzir o mesmo áudio.
    //
    // Ver a nota longa no cabeçalho do lib.rs: os pontos que realmente
    // importavam foram (1) `outHead` POR CANAL — quando era um campo só, o
    // canal direito perdia as amostras de índice ímpar e o áudio saía com
    // −3 dB e fase invertida; (2) sqrt-Hann em vez de Hann (−2,3 dB e
    // modulação a 187 Hz); (3) `preroll` POR CANAL, senão o início do clipe
    // saía com fade-in (envelope a 0.48 do steady-state); (5) mediana em vez
    // de mínimo, porque o mínimo de um tom estável É a potência do tom; (6) as
    // duas proteções por cv e por tendência, que impedem o gate de cortar tom,
    // voz e sweep.
    //
    // Medido (mesmos testes do lib.rs): tom 997 Hz a=30 −0,11 dB; sweep
    // 200→3200 Hz a=30 −0,44 dB; voz+chiado a=18 −3,28 dB; chiado branco
    // a=30 −5,66 dB; silêncio permanece silêncio.
    //
    // Limitações conhecidas: a remoção satura perto de −6 dB (a=50 → −6,00 dB)
    // porque o piso mediano subestima o ruído — é o preço de proteger o sinal.
    // E a API de streaming é de tamanho fixo: há latência fixa de N+HOP
    // amostras e os últimos HOP (5,3 ms) de cada clipe não são renderizados.
    struct SpectralDenoise {
        static constexpr int FFT = 512;
        static constexpr int HOP = 256;
        static constexpr int BINS = FFT / 2 + 1;
        static constexpr int MIN_WIN = 40;      // ≈210 ms de histórico por bin
        static constexpr int LEARN_FRAMES = 56; // ≈300 ms sem gate
        static constexpr double FLOOR_BIAS = 0.7; // calibra a mediana
        static constexpr double GAIN_TAU = 0.030; // suavização temporal
        static constexpr double CV_PROTECT = 0.35; // cv < isso ⇒ estacionário
        static constexpr double RISE_PROTECT = 6.0;// mag > mediana·6 ⇒ sinal
        static constexpr double GATE_LO_DB = 0.0; // abaixo disso = no piso
        static constexpr double FS = 48000.0;

        bool enabled = false;
        double amount = 12.0;
        QVector<double> window;   // sqrt-Hann periódica
        QVector<double> inFifo[2];
        QVector<double> ola[2];
        QVector<double> noise[2];   // piso de ruído (potência)
        QVector<double> gainDb[2]; // ganho suavizado em dB
        QVector<double> freqDb, freqDb2; // ping-pong da suavização em frequência
        QVector<double> hist;      // 2·BINS·MIN_WIN, cursor circular
        int hpos[2] = {0, 0};      // POR CANAL
        QVector<double> win;       // janela de trabalho (mediana por bin)
        QVector<double> outFifo[2];
        int outHead[2] = {0, 0};   // POR CANAL (bug: era compartilhado)
        int preroll[2] = {0, 0};   // POR CANAL
        QVector<double> fftRe, fftIm, mag, rawDb;
        int frames = 0;

        SpectralDenoise() {
            window.resize(FFT);
            for (int i = 0; i < FFT; ++i)
                window[i] = std::sqrt(0.5 - 0.5 * std::cos(2.0 * M_PI * i / FFT));
            reset(); // zera todos os buffers
        }
        void reset() {
            inFifo[0].clear(); inFifo[1].clear();
            ola[0].fill(0.0, FFT); ola[1].fill(0.0, FFT);
            noise[0].fill(1e-12, BINS); noise[1].fill(1e-12, BINS);
            gainDb[0].fill(0.0, BINS); gainDb[1].fill(0.0, BINS);
            freqDb.fill(0.0, BINS);
            freqDb2.fill(0.0, BINS);
            win.fill(0.0, MIN_WIN);
            hist.fill(0.0, 2 * BINS * MIN_WIN);
            hpos[0] = 0; hpos[1] = 0;
            outFifo[0].clear(); outFifo[1].clear();
            outHead[0] = 0; outHead[1] = 0;
            preroll[0] = HOP; preroll[1] = HOP;
            fftRe.fill(0.0, FFT); fftIm.fill(0.0, FFT); mag.fill(0.0, BINS);
            rawDb.fill(0.0, BINS);
            frames = 0;
        }
        void setParams(bool en, double amt) {
            enabled = en;
            amount = std::clamp(amt, 1.0, 50.0);
        }
        double popOut(int ch) {
            const int h = outHead[ch];
            if (h >= outFifo[ch].size()) {
                outFifo[ch].clear();
                outHead[ch] = 0;
                return 0.0;
            }
            const double v = outFifo[ch][h];
            outHead[ch] = h + 1;
            // Pré-roll: descarta o 1º segmento, que viria só com uma frame de
            // contribuição (peso w²[0..N/2] incompleto).
            if (preroll[ch] > 0) {
                --preroll[ch];
                return 0.0;
            }
            if (h > 8192 && h * 2 > outFifo[ch].size()) {
                outFifo[ch].remove(0, h);
                outHead[ch] = 0;
            }
            return v;
        }
        void processChannelFrame(int ch) {
            for (int i = 0; i < FFT; ++i) {
                fftRe[i] = inFifo[ch][i] * window[i];
                fftIm[i] = 0.0;
            }
            fftRadix2(fftRe, fftIm);
            for (int b = 0; b < BINS; ++b)
                mag[b] = fftRe[b] * fftRe[b] + fftIm[b] * fftIm[b];

            // Histórico circular (cursor por canal).
            const int hp = hpos[ch];
            for (int b = 0; b < BINS; ++b)
                hist[(ch * BINS + b) * MIN_WIN + hp] = mag[b];
            hpos[ch] = (hp + 1) % MIN_WIN;

            // No warmup o histórico tem posições nunca escritas: pré-preenche
            // com o frame atual para o piso sair de uma estimativa real já na
            // 1ª frame. `frames` incrementa 1x por canal.
            const int filled = qMax(1, qMin(frames, MIN_WIN));
            if (filled < MIN_WIN) {
                for (int b = 0; b < BINS; ++b) {
                    const int base = (ch * BINS + b) * MIN_WIN;
                    for (int k = hp + 1; k < MIN_WIN; ++k)
                        hist[base + k] = mag[b];
                }
            }

            // Piso = mediana da janela, com as duas proteções:
            //   cv < CV_PROTECT  ⇒ bin estacionário (tom cv 0.00, voz 0.02,
            //                        chiado 1.0) ⇒ intocado;
            //   mag atual > mediana·RISE_PROTECT ⇒ o bin está sendo ocupado
            //                        por sinal agora (sweep/música).
            for (int b = 0; b < BINS; ++b) {
                const int base = (ch * BINS + b) * MIN_WIN;
                for (int k = 0; k < filled; ++k) win[k] = hist[base + k];
                for (int k = filled; k < MIN_WIN; ++k) win[k] = std::numeric_limits<double>::infinity();
                std::sort(win.begin(), win.end());
                const double median = win[filled / 2];
                double sum = 0.0;
                for (int k = 0; k < filled; ++k) sum += win[k];
                const double mean = sum / filled;
                double var = 0.0;
                for (int k = 0; k < filled; ++k) {
                    const double d = win[k] - mean;
                    var += d * d;
                }
                var /= filled;
                const double cv = mean > 0.0 ? std::sqrt(var) / mean
                                            : std::numeric_limits<double>::infinity();
                const bool rising = mag[b] > median * RISE_PROTECT;
                noise[ch][b] = (cv < CV_PROTECT || rising)
                    ? 1e-12
                    : std::max(median * FLOOR_BIAS, 1e-12);
            }

            // Gate: rampa de −amount dB no piso até 0 dB em `amount` dB acima.
            if (frames < LEARN_FRAMES) {
                rawDb.fill(0.0, BINS);
            } else {
                for (int b = 0; b < BINS; ++b) {
                    const double snrDb = 10.0 * std::log10(std::max(mag[b], 1e-300) / noise[ch][b]);
                    const double t = std::clamp((snrDb - GATE_LO_DB) / amount, 0.0, 1.0);
                    rawDb[b] = amount * (t - 1.0);
                }
            }

            // Suavização temporal em dB (τ = 30 ms) e em frequência (3 taps × 2).
            const double at = 1.0 - std::exp(-(double)HOP / (FS * GAIN_TAU));
            for (int b = 0; b < BINS; ++b) {
                gainDb[ch][b] += (rawDb[b] - gainDb[ch][b]) * at;
                freqDb[b] = gainDb[ch][b];
            }
            // Ping-pong para não alocar a cada frame.
            for (int pass = 0; pass < 2; ++pass) {
                freqDb.swap(freqDb2);
                for (int b = 1; b < BINS - 1; ++b)
                    freqDb[b] = 0.25 * freqDb2[b - 1] + 0.5 * freqDb2[b] + 0.25 * freqDb2[b + 1];
                freqDb[0] = freqDb2[0];
                freqDb[BINS - 1] = freqDb2[BINS - 1];
                for (int b = 0; b < BINS; ++b) gainDb[ch][b] = freqDb[b];
            }

            for (int b = 0; b < BINS; ++b) {
                const double lin = std::clamp(std::pow(10.0, gainDb[ch][b] / 20.0), 0.0, 1.0);
                fftRe[b] *= lin;
                fftIm[b] *= lin;
            }
            ++frames;

            ifftRadix2(fftRe, fftIm);
            // OLA com a mesma sqrt-Hann: w_a·w_s = Hann, soma 1.0 com 50% overlap.
            for (int i = 0; i < FFT; ++i)
                ola[ch][i] += fftRe[i] * window[i];
            for (int i = 0; i < HOP; ++i)
                outFifo[ch].append(ola[ch][i]);
            for (int i = 0; i < FFT - HOP; ++i)
                ola[ch][i] = ola[ch][i + HOP];
            for (int i = FFT - HOP; i < FFT; ++i)
                ola[ch][i] = 0.0;
            inFifo[ch].remove(0, HOP);
        }
        void processStereo(double* data, int nframes) {
            if (!enabled) return;
            for (int f = 0; f < nframes; ++f) {
                inFifo[0].append(data[2 * f]);
                inFifo[1].append(data[2 * f + 1]);
                while (inFifo[0].size() >= FFT) {
                    processChannelFrame(0);
                    processChannelFrame(1);
                }
                data[2 * f] = popOut(0);
                data[2 * f + 1] = popOut(1);
            }
        }
    };

    void configure(double eqLow, double eqMid, double eqHigh, bool denoise,
                   double denoiseAmt, bool invertPhase, bool normalize,
                   bool reverb, double reverbMix, double reverbSize) {
        const double fs = 48000.0;
        const int key = (int)std::llround(eqLow * 10) * 1000000
                      + (int)std::llround(eqMid * 10) * 1000
                      + (int)std::llround(eqHigh * 10)
                      + (denoise ? 100 : 0)
                      + (invertPhase ? 200 : 0)
                      + (normalize ? 400 : 0)
                      + (int)std::llround(denoiseAmt) * 10000
                      + (reverb ? 800 : 0)
                      + (int)std::llround(reverbMix * 100) * 100000
                      + (int)std::llround(reverbSize * 100) * 10000000;
        if (key == m_key) return; // parâmetros inalterados: mantém o estado
        m_key = key;
        for (int ch = 0; ch < 2; ++ch) {
            low[ch].peaking(120.0, 1.0, std::clamp(eqLow, -12.0, 12.0), fs);
            mid[ch].peaking(1000.0, 1.0, std::clamp(eqMid, -12.0, 12.0), fs);
            high[ch].peaking(6000.0, 1.0, std::clamp(eqHigh, -12.0, 12.0), fs);
        }
        invert = invertPhase;
        dn.setParams(denoise, denoiseAmt);
        agcEnabled = normalize;
        reverbEnabled = reverb;
        reverbMixAmt = std::clamp(reverbMix, 0.0, 1.0);
        reverbSizeAmt = std::clamp(reverbSize, 0.0, 1.0);
        rv.setup(reverbSizeAmt);
        resetState();
    }

    void resetState() {
        for (int ch = 0; ch < 2; ++ch) {
            low[ch].reset(); mid[ch].reset(); high[ch].reset();
        }
        dn.reset();
        agcLevel = 0.0;
        agcGain = 1.0;
        rv.setup(reverbSizeAmt); // limpa os buffers do reverb
    }

    // Processa `frames` amostras estéreo interleaved S16 no próprio buffer.
    void process(int16_t* buf, int frames) {
        QVector<double> tmp((qsizetype)frames * 2);
        for (int f = 0; f < frames; ++f) {
            double l = buf[2 * f] / 32768.0;
            double r = buf[2 * f + 1] / 32768.0;
            l = high[0].tick(mid[0].tick(low[0].tick(l)));
            r = high[1].tick(mid[1].tick(low[1].tick(r)));
            if (invert) { l = -l; r = -r; }
            tmp[2 * f] = l;
            tmp[2 * f + 1] = r;
        }
        dn.processStereo(tmp.data(), frames);
        for (int f = 0; f < frames; ++f) {
            double l = tmp[2 * f];
            double r = tmp[2 * f + 1];
            if (agcEnabled) {
                const double lvl = 0.5 * (l * l + r * r);
                agcLevel = agcLevel * 0.999 + lvl * 0.001;
                const double db = 20.0 * std::log10(std::sqrt(agcLevel) + 1e-9);
                const double want = -14.0 - db; // ganho (dB) rumo a -14
                const double g = std::pow(10.0, std::clamp(want, -12.0, 12.0) / 20.0);
                agcGain = agcGain * 0.95 + g * 0.05;
                l *= agcGain; r *= agcGain;
                const double pk = qMax(std::fabs(l), std::fabs(r));
                if (pk > 0.95) { const double s = 0.95 / pk; l *= s; r *= s; }
            }
            if (reverbEnabled && reverbMixAmt > 0.01) {
                const double dry = 0.5 * (l + r);
                const double wet = rv.tick((float)dry) * 4.0;
                const double w = reverbMixAmt;
                l = l * (1.0 - w) + wet * w;
                r = r * (1.0 - w) + wet * w;
            }
            buf[2 * f] = (int16_t)std::lround(std::clamp(l, -1.0, 1.0) * 32767.0);
            buf[2 * f + 1] = (int16_t)std::lround(std::clamp(r, -1.0, 1.0) * 32767.0);
        }
    }

private:
    int m_key = -1;
    Biquad low[2], mid[2], high[2];
    bool invert = false;
    bool agcEnabled = false;
    double agcLevel = 0.0;
    double agcGain = 1.0;
    bool reverbEnabled = false;
    double reverbMixAmt = 0.0;
    double reverbSizeAmt = 0.5;
    SimpleReverb rv; // DSP do Reverb EX (evita conflito com o parâmetro bool)
    SpectralDenoise dn;
};

// Equalizador gráfico de 10 bandas: cadeia de biquads peaking em série, por
// canal. Os ganhos são em dB (-12..+12) e casam com as bandas de
// `graphicEqFrequencies()` (modelo). Usado tanto no barramento de cada faixa de
// áudio quanto no master. Não depende do kernel Rust (DSP próprio, simples).
class GraphicEq {
public:
    // `gainsDb` deve ter kGraphicEqBands entradas; faltantes = 0 dB (neutro).
    void setGains(const QVector<double>& gainsDb) {
        int key = 0;
        for (int i = 0; i < kGraphicEqBands; ++i) {
            const double v =
                std::clamp(i < gainsDb.size() ? gainsDb[i] : 0.0, -12.0, 12.0);
            m_gain[i] = v;
            key = key * 31 + (int)std::lround(v * 10.0);
        }
        if (key == m_key) return; // parâmetros inalterados: mantém o estado
        m_key = key;
        const double* f = graphicEqFrequencies();
        for (int ch = 0; ch < 2; ++ch)
            for (int b = 0; b < kGraphicEqBands; ++b)
                m_bands[ch][b].peaking(f[b], 1.414, m_gain[b], kSampleRate);
        reset();
    }

    void reset() {
        for (int ch = 0; ch < 2; ++ch)
            for (int b = 0; b < kGraphicEqBands; ++b) m_bands[ch][b].reset();
    }

    // Processa `frames` amostras estéreo interleaved S16 no próprio buffer.
    void process(int16_t* buf, int frames) {
        if (m_key == 0) return; // tudo neutro: não gasta DSP
        for (int fr = 0; fr < frames; ++fr) {
            double l = buf[2 * fr] / 32768.0;
            double r = buf[2 * fr + 1] / 32768.0;
            for (int b = 0; b < kGraphicEqBands; ++b) {
                l = m_bands[0][b].tick(l);
                r = m_bands[1][b].tick(r);
            }
            buf[2 * fr] = (int16_t)std::lround(std::clamp(l, -1.0, 1.0) * 32767.0);
            buf[2 * fr + 1] =
                (int16_t)std::lround(std::clamp(r, -1.0, 1.0) * 32767.0);
        }
    }

    static constexpr double kSampleRate = 48000.0;

private:
    double m_gain[kGraphicEqBands] = { 0.0 };
    int m_key = 0;
    AudioFxFallback::Biquad m_bands[2][kGraphicEqBands];
};

// Equalizador paramétrico: cadeia de biquads (Bell/Shelf) por canal, definida
// por um vetor de `ParametricBand` do modelo. Mesmas fórmulas usadas para
// desenhar a curva na UI (EqBiquad.h) e, conceitualmente, pelo export.
class ParametricEqDsp {
public:
    void setBands(const QVector<ParametricBand>& bands) {
        int key = 0;
        bool active = false;
        for (const ParametricBand& b : bands) {
            key = key * 31 + (b.enabled ? 1 : 0);
            key = key * 31 + b.type;
            key = key * 31 + (int)std::lround(b.freq * 10.0);
            key = key * 31 + (int)std::lround(b.gain * 10.0);
            key = key * 31 + (int)std::lround(b.q * 100.0);
            if (b.enabled && std::fabs(b.gain) > 0.01) active = true;
        }
        if (key == m_key && bands.size() == m_n) return; // inalterado
        m_key = key;
        m_n = bands.size();
        m_active = active;
        for (int ch = 0; ch < 2; ++ch) {
            m_bands[ch].resize(bands.size());
            for (int i = 0; i < bands.size(); ++i) {
                const ParametricBand& b = bands[i];
                EqBiquad& q = m_bands[ch][i];
                if (b.enabled) {
                    if (b.type == EqLowShelf)
                        q.lowShelf(b.freq, b.gain, kSampleRate);
                    else if (b.type == EqHighShelf)
                        q.highShelf(b.freq, b.gain, kSampleRate);
                    else
                        q.peaking(b.freq, b.q, b.gain, kSampleRate);
                } else {
                    q = EqBiquad(); // identidade
                }
            }
        }
        reset();
    }

    void reset() {
        for (int ch = 0; ch < 2; ++ch)
            for (EqBiquad& q : m_bands[ch]) q.reset();
    }

    void process(int16_t* buf, int frames) {
        if (!m_active || m_n <= 0) return;
        for (int fr = 0; fr < frames; ++fr) {
            double l = buf[2 * fr] / 32768.0;
            double r = buf[2 * fr + 1] / 32768.0;
            for (EqBiquad& q : m_bands[0]) l = q.tick(l);
            for (EqBiquad& q : m_bands[1]) r = q.tick(r);
            buf[2 * fr] = (int16_t)std::lround(std::clamp(l, -1.0, 1.0) * 32767.0);
            buf[2 * fr + 1] =
                (int16_t)std::lround(std::clamp(r, -1.0, 1.0) * 32767.0);
        }
    }

    static constexpr double kSampleRate = 48000.0;

private:
    int m_key = -1;
    int m_n = -1;
    bool m_active = false;
    QVector<EqBiquad> m_bands[2];
};

// O DSP dos efeitos vive no kernel Rust (lib pierrot_audiofx) quando o build
// é feito com -DPIERROT_ENABLE_RUST=ON; senão uso o fallback C++ acima. A
// interface (configure/resetState/process) é idêntica, então o resto do
// PreviewWidget não muda de cara.
#ifdef PIERROT_ENABLE_RUST
using AudioFx = AudioFxBridge;
#else
using AudioFx = AudioFxFallback;
#endif
