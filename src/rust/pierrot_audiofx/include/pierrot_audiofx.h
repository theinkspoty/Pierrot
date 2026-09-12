#ifndef PIERROT_AUDIOFX_H
#define PIERROT_AUDIOFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PierrotFx PierrotFx;

// Estado DSP (EQ/gate/AGC/reverb) portado para Rust. A instância detém o
// estado dos filtros entre chunks; parâmetros inalterados preservam o estado.
PierrotFx* pierrot_fx_new(void);
void pierrot_fx_free(PierrotFx* fx);
PierrotFx* pierrot_fx_clone(const PierrotFx* fx);

// eqLow/eqMid/eqHigh em dB (-12..+12); denoiseAmt em dB (1..50);
// reverbMix 0..1; reverbSize 0..1.
void pierrot_fx_configure(PierrotFx* fx,
                          double eqLow, double eqMid, double eqHigh,
                          int denoise, double denoiseAmt,
                          int invertPhase, int normalize,
                          int reverb, double reverbMix, double reverbSize);

void pierrot_fx_reset(PierrotFx* fx);

// Processa `frames` amostras estéreo interleaved S16 no próprio buffer.
void pierrot_fx_process(PierrotFx* fx, int16_t* buf, int frames);

#ifdef __cplusplus
}
#endif

#endif // PIERROT_AUDIOFX_H