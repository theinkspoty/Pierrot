use std::f64::consts::PI;

const FS: f64 = 48000.0;

#[derive(Clone, Copy)]
struct Biquad {
    b0: f64,
    b1: f64,
    b2: f64,
    a1: f64,
    a2: f64,
    x1: f64,
    x2: f64,
    y1: f64,
    y2: f64,
}

impl Biquad {
    fn default() -> Self {
        Biquad { b0: 1.0, b1: 0.0, b2: 0.0, a1: 0.0, a2: 0.0, x1: 0.0, x2: 0.0, y1: 0.0, y2: 0.0 }
    }
    fn peaking(&mut self, freq: f64, q: f64, gain_db: f64, fs: f64) {
        let a = 10f64.powf(gain_db / 40.0);
        let w0 = 2.0 * PI * freq / fs;
        let alpha = w0.sin() / (2.0 * q);
        let a0 = 1.0 + alpha / a;
        self.b0 = (1.0 + alpha * a) / a0;
        self.b1 = (-2.0 * w0.cos()) / a0;
        self.b2 = (1.0 - alpha * a) / a0;
        self.a1 = (-2.0 * w0.cos()) / a0;
        self.a2 = (1.0 - alpha / a) / a0;
    }
    #[inline]
    fn tick(&mut self, x: f64) -> f64 {
        let y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2 - self.a1 * self.y1 - self.a2 * self.y2;
        self.x2 = self.x1;
        self.x1 = x;
        self.y2 = self.y1;
        self.y1 = y;
        y
    }
    fn reset(&mut self) {
        self.x1 = 0.0;
        self.x2 = 0.0;
        self.y1 = 0.0;
        self.y2 = 0.0;
    }
}

#[derive(Clone, Copy)]
struct Comb {
    buf: [f32; 8192],
    len: usize,
    pos: usize,
    feedback: f32,
    damping: f32,
    filter: f32,
}

impl Comb {
    fn default() -> Self {
        Comb { buf: [0.0; 8192], len: 1, pos: 0, feedback: 0.0, damping: 0.0, filter: 0.0 }
    }
    fn setup(&mut self, length: usize, fb: f32, damp: f32) {
        self.len = length.max(1);
        self.feedback = fb;
        self.damping = damp;
        self.pos = 0;
        self.filter = 0.0;
        self.buf.fill(0.0);
    }
    #[inline]
    fn tick(&mut self, input: f32) -> f32 {
        let output = self.buf[self.pos];
        self.filter = output * (1.0 - self.damping) + self.filter * self.damping;
        self.buf[self.pos] = input + self.filter * self.feedback;
        self.pos += 1;
        if self.pos >= self.len { self.pos = 0; }
        output
    }
}

#[derive(Clone, Copy)]
struct Allpass {
    buf: [f32; 8192],
    len: usize,
    pos: usize,
}

impl Allpass {
    fn default() -> Self {
        Allpass { buf: [0.0; 8192], len: 1, pos: 0 }
    }
    fn setup(&mut self, length: usize) {
        self.len = length.max(1);
        self.pos = 0;
        self.buf.fill(0.0);
    }
    #[inline]
    fn tick(&mut self, input: f32) -> f32 {
        let bufo = self.buf[self.pos];
        let out = -input + bufo;
        self.buf[self.pos] = input + bufo * 0.5;
        self.pos += 1;
        if self.pos >= self.len { self.pos = 0; }
        out
    }
}

#[derive(Clone, Copy)]
struct SimpleReverb {
    comb: [Comb; 4],
    allpass: [Allpass; 2],
}

impl SimpleReverb {
    fn default() -> Self {
        SimpleReverb {
            comb: [Comb::default(), Comb::default(), Comb::default(), Comb::default()],
            allpass: [Allpass::default(), Allpass::default()],
        }
    }
    fn setup(&mut self, size: f64) {
        let delays = [1557usize, 1617, 1491, 1422];
        let ap_delays = [225usize, 556];
        let fb = 0.60f32 + 0.28f32 * size as f32;
        let damp = 0.4f32 - 0.25f32 * size as f32;
        for i in 0..4 {
            self.comb[i].setup(delays[i], fb, damp);
        }
        for i in 0..2 {
            self.allpass[i].setup(ap_delays[i]);
        }
    }
    #[inline]
    fn tick(&mut self, input: f32) -> f32 {
        let mut o = 0.016f32 * self.comb[0].tick(input)
            + 0.016f32 * self.comb[1].tick(input)
            + 0.023f32 * self.comb[2].tick(input)
            + 0.027f32 * self.comb[3].tick(input);
        o = self.allpass[0].tick(o);
        o = self.allpass[1].tick(o);
        o
    }
}

pub struct PierrotFx {
    key: i64,
    low: [Biquad; 2],
    mid: [Biquad; 2],
    high: [Biquad; 2],
    invert: bool,
    gate_enabled: bool,
    gate_amount: f64,
    agc_enabled: bool,
    reverb_enabled: bool,
    reverb_mix_amt: f64,
    reverb_size_amt: f64,
    rv: SimpleReverb,
    gate_env: f64,
    agc_level: f64,
    agc_gain: f64,
}

impl PierrotFx {
    pub fn new() -> Self {
        PierrotFx {
            key: -1,
            low: [Biquad::default(), Biquad::default()],
            mid: [Biquad::default(), Biquad::default()],
            high: [Biquad::default(), Biquad::default()],
            invert: false,
            gate_enabled: false,
            gate_amount: 1.0,
            agc_enabled: false,
            reverb_enabled: false,
            reverb_mix_amt: 0.0,
            reverb_size_amt: 0.0,
            rv: SimpleReverb::default(),
            gate_env: 0.0,
            agc_level: 0.0,
            agc_gain: 1.0,
        }
    }

    pub fn configure(&mut self, eq_low: f64, eq_mid: f64, eq_high: f64, denoise: bool,
                     denoise_amt: f64, invert_phase: bool, normalize: bool,
                     reverb: bool, reverb_mix: f64, reverb_size: f64) {
        let key = (eq_low * 10.0).round() as i64 * 1_000_000
            + (eq_mid * 10.0).round() as i64 * 1_000
            + (eq_high * 10.0).round() as i64
            + if denoise { 100 } else { 0 }
            + if invert_phase { 200 } else { 0 }
            + if normalize { 400 } else { 0 }
            + (denoise_amt).round() as i64 * 10_000
            + if reverb { 800 } else { 0 }
            + (reverb_mix * 100.0).round() as i64 * 100_000
            + (reverb_size * 100.0).round() as i64 * 10_000_000;
        if key == self.key {
            return;
        }
        self.key = key;
        for ch in 0..2 {
            self.low[ch].peaking(120.0, 1.0, eq_low.clamp(-12.0, 12.0), FS);
            self.mid[ch].peaking(1000.0, 1.0, eq_mid.clamp(-12.0, 12.0), FS);
            self.high[ch].peaking(6000.0, 1.0, eq_high.clamp(-12.0, 12.0), FS);
        }
        self.invert = invert_phase;
        self.gate_enabled = denoise;
        self.gate_amount = denoise_amt.clamp(1.0, 50.0);
        self.agc_enabled = normalize;
        self.reverb_enabled = reverb;
        self.reverb_mix_amt = reverb_mix.clamp(0.0, 1.0);
        self.reverb_size_amt = reverb_size.clamp(0.0, 1.0);
        self.rv.setup(self.reverb_size_amt);
        self.reset_state();
    }

    pub fn reset_state(&mut self) {
        for ch in 0..2 {
            self.low[ch].reset();
            self.mid[ch].reset();
            self.high[ch].reset();
        }
        self.gate_env = 0.0;
        self.agc_level = 0.0;
        self.agc_gain = 1.0;
        self.rv.setup(self.reverb_size_amt);
    }

    pub fn process(&mut self, buf: &mut [i16], frames: usize) {
        for f in 0..frames {
            let mut l = buf[2 * f] as f64 / 32768.0;
            let mut r = buf[2 * f + 1] as f64 / 32768.0;
            l = self.high[0].tick(self.mid[0].tick(self.low[0].tick(l)));
            r = self.high[1].tick(self.mid[1].tick(self.low[1].tick(r)));
            if self.invert {
                l = -l;
                r = -r;
            }
            if self.gate_enabled {
                let peak = l.abs().max(r.abs());
                self.gate_env = if peak > self.gate_env { peak } else { self.gate_env * 0.999 };
                let db = 20.0 * (self.gate_env + 1e-9).log10();
                let floor_db = -50.0;
                let mut g = 1.0;
                if db < floor_db {
                    let depth = 1.0 - (db - floor_db) / (0.0 - floor_db);
                    g = 10f64.powf(-(self.gate_amount * depth) / 20.0);
                }
                l *= g;
                r *= g;
            }
            if self.agc_enabled {
                let lvl = 0.5 * (l * l + r * r);
                self.agc_level = self.agc_level * 0.999 + lvl * 0.001;
                let db = 20.0 * (self.agc_level.sqrt() + 1e-9).log10();
                let want = -14.0 - db;
                let g = 10f64.powf(want.clamp(-12.0, 12.0) / 20.0);
                self.agc_gain = self.agc_gain * 0.95 + g * 0.05;
                l *= self.agc_gain;
                r *= self.agc_gain;
                let pk = l.abs().max(r.abs());
                if pk > 0.95 {
                    let s = 0.95 / pk;
                    l *= s;
                    r *= s;
                }
            }
            if self.reverb_enabled && self.reverb_mix_amt > 0.01 {
                let dry = 0.5 * (l + r);
                let wet = self.rv.tick(dry as f32) as f64 * 4.0;
                let w = self.reverb_mix_amt;
                l = l * (1.0 - w) + wet * w;
                r = r * (1.0 - w) + wet * w;
            }
            buf[2 * f] = (l.clamp(-1.0, 1.0) * 32768.0).round() as i16;
            buf[2 * f + 1] = (r.clamp(-1.0, 1.0) * 32768.0).round() as i16;
        }
    }
}

#[no_mangle]
pub extern "C" fn pierrot_fx_new() -> *mut PierrotFx {
    Box::into_raw(Box::new(PierrotFx::new()))
}

#[no_mangle]
pub unsafe extern "C" fn pierrot_fx_free(fx: *mut PierrotFx) {
    if !fx.is_null() {
        drop(Box::from_raw(fx));
    }
}

#[no_mangle]
pub unsafe extern "C" fn pierrot_fx_clone(fx: *const PierrotFx) -> *mut PierrotFx {
    if fx.is_null() {
        return std::ptr::null_mut();
    }
    Box::into_raw(Box::new((*fx).clone_like_cpp()))
}

impl PierrotFx {
    fn clone_like_cpp(&self) -> Self {
        let mut c = PierrotFx::new();
        c.key = self.key;
        c.low = self.low;
        c.mid = self.mid;
        c.high = self.high;
        c.invert = self.invert;
        c.gate_enabled = self.gate_enabled;
        c.gate_amount = self.gate_amount;
        c.agc_enabled = self.agc_enabled;
        c.reverb_enabled = self.reverb_enabled;
        c.reverb_mix_amt = self.reverb_mix_amt;
        c.reverb_size_amt = self.reverb_size_amt;
        c.gate_env = self.gate_env;
        c.agc_level = self.agc_level;
        c.agc_gain = self.agc_gain;
        c.rv = self.rv;
        c
    }
}

#[no_mangle]
pub unsafe extern "C" fn pierrot_fx_configure(
    fx: *mut PierrotFx,
    eq_low: f64, eq_mid: f64, eq_high: f64,
    denoise: i32, denoise_amt: f64,
    invert_phase: i32, normalize: i32,
    reverb: i32, reverb_mix: f64, reverb_size: f64,
) {
    if !fx.is_null() {
        (*fx).configure(eq_low, eq_mid, eq_high, denoise != 0,
                        denoise_amt, invert_phase != 0, normalize != 0,
                        reverb != 0, reverb_mix, reverb_size);
    }
}

#[no_mangle]
pub unsafe extern "C" fn pierrot_fx_reset(fx: *mut PierrotFx) {
    if !fx.is_null() {
        (*fx).reset_state();
    }
}

#[no_mangle]
pub unsafe extern "C" fn pierrot_fx_process(fx: *mut PierrotFx, buf: *mut i16, frames: i32) {
    if fx.is_null() || buf.is_null() || frames <= 0 {
        return;
    }
    let n = frames as usize;
    let slice = std::slice::from_raw_parts_mut(buf, n * 2);
    (*fx).process(slice, n);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_fx() -> PierrotFx {
        let mut fx = PierrotFx::new();
        fx.configure(3.0, -2.0, 4.0, true, 10.0, false, true, true, 0.3, 0.4);
        fx
    }

    #[test]
    fn silence_in_silence_out() {
        let mut fx = make_fx();
        let mut buf = [0i16; 4096];
        fx.reset_state();
        fx.process(&mut buf, 2048);
        assert!(buf.iter().all(|&s| s == 0));
    }

    #[test]
    fn constant_input_stays_bounded() {
        let mut fx = make_fx();
        let mut buf = [0i16; 256];
        for s in buf.iter_mut() {
            *s = 10000;
        }
        let before = buf;
        fx.process(&mut buf, 128);
        for (b, a) in before.iter().zip(buf.iter()) {
            let diff = (*a as i32 - *b as i32).abs();
            assert!(diff < 8000, "amostra explodiu: {b} -> {a}");
        }
    }

    #[test]
    fn configure_twice_keeps_state() {
        let mut fx = make_fx();
        let state_before = (fx.gate_env, fx.agc_gain);
        fx.configure(3.0, -2.0, 4.0, true, 10.0, false, true, true, 0.3, 0.4);
        assert_eq!(fx.gate_env, state_before.0);
        assert_eq!(fx.agc_gain, state_before.1);
    }
}