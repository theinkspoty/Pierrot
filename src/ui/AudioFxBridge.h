#ifndef PIERROT_AUDIOFX_BRIDGE_H
#define PIERROT_AUDIOFX_BRIDGE_H

#include "pierrot_audiofx.h"

#include <cstdint>

// Ponte para a lib Rust pierrot_audiofx. Mantém a mesma interface do antigo
// AudioFx (configure/resetState/process) para que os pontos de uso no
// PreviewWidget não mudem de cara. Copiável (clone) e moveável.
class AudioFxBridge {
public:
    AudioFxBridge() : m_fx(pierrot_fx_new()) {}
    ~AudioFxBridge() { pierrot_fx_free(m_fx); }
    AudioFxBridge(const AudioFxBridge& o) : m_fx(pierrot_fx_clone(o.m_fx)) {}
    AudioFxBridge& operator=(const AudioFxBridge& o) {
        if (this != &o) {
            auto* c = pierrot_fx_clone(o.m_fx);
            pierrot_fx_free(m_fx);
            m_fx = c;
        }
        return *this;
    }
    AudioFxBridge(AudioFxBridge&& o) noexcept : m_fx(o.m_fx) { o.m_fx = nullptr; }
    AudioFxBridge& operator=(AudioFxBridge&& o) noexcept {
        if (this != &o) {
            pierrot_fx_free(m_fx);
            m_fx = o.m_fx;
            o.m_fx = nullptr;
        }
        return *this;
    }

    void configure(double eqLow, double eqMid, double eqHigh, bool denoise,
                   double denoiseAmt, bool invertPhase, bool normalize,
                   bool reverb, double reverbMix, double reverbSize) {
        pierrot_fx_configure(m_fx, eqLow, eqMid, eqHigh, denoise,
                             denoiseAmt, invertPhase, normalize,
                             reverb, reverbMix, reverbSize);
    }
    void resetState() { pierrot_fx_reset(m_fx); }
    void process(int16_t* buf, int frames) { pierrot_fx_process(m_fx, buf, frames); }

private:
    PierrotFx* m_fx = nullptr;
};

#endif // PIERROT_AUDIOFX_BRIDGE_H