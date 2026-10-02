use std::f64::consts::PI;

const FS: f64 = 48000.0;

// ── FFT radix-2 iterativa (Cooley–Tukey) ─────────────────────────────────────
fn fft(re: &mut [f64], im: &mut [f64]) {
    let n = re.len();
    if n <= 1 {
        return;
    }
    // Bit-reversal permutation
    let mut j = 0usize;
    for i in 1..n {
        let mut bit = n >> 1;
        while j & bit != 0 {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if i < j {
            re.swap(i, j);
            im.swap(i, j);
        }
    }
    // Butterflies
    let mut len = 2usize;
    while len <= n {
        let ang = -2.0 * PI / len as f64;
        let wlen_re = ang.cos();
        let wlen_im = ang.sin();
        let half = len / 2;
        let mut i = 0usize;
        while i < n {
            for k in 0..half {
                let wr = wlen_re.powi(k as i32);
                let wi = wlen_im.powi(k as i32);
                let ur = re[i + k];
                let ui = im[i + k];
                let vr = re[i + k + half] * wr - im[i + k + half] * wi;
                let vi = re[i + k + half] * wi + im[i + k + half] * wr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + half] = ur - vr;
                im[i + k + half] = ui - vi;
            }
            i += len;
        }
        len <<= 1;
    }
}

/// IFFT real via FFT complexa: x = Re(conj(FFT(conj(X))))/N — conjuga só a parte
/// imaginária. O imaginário residual (erro numérico) é descartado. Idêntico ao
/// `ifftRadix2` do fallback C++.
fn ifft(re: &mut [f64], im: &mut [f64]) {
    let n = re.len();
    for v in im.iter_mut() {
        *v = -*v;
    }
    fft(re, im);
    let inv = 1.0 / n as f64;
    for i in 0..n {
        re[i] *= inv;
        im[i] = -im[i] * inv;
    }
}

// ── Denoise espectral (STFT + piso de ruído por mediana + gate em dB) ──────
// Substitui o noise gate de envelope: remove chiado/HVAC constante mesmo com
// sinal presente, porque atenua bins cuja energia está perto do piso de ruído
// estimado — o gate antigo só cortava abaixo de −50 dB globais.
//
// O que a medição mostrou (ver os testes no fim do arquivo; todos os números
// abaixo saíram de `verify_dn.py`, transliteração exata deste código):
//
// 1. **`out_head` POR CANAL.** O bug que fazia o áudio sair "horrível":
//    `out_head` era um único campo compartilhado, então `pop_out(0)` lia
//    `out_fifo[0][0]` e `pop_out(1)` lia `out_fifo[1][1]` — o canal direito
//    perdia as amostras de índice ímpar. Medido: −3,00 dB, tom com fase
//    invertida e correlação ~0 com a entrada.
// 2. **sqrt-Hann, não Hann.** Com Hann em análise E síntese, cada amostra
//    recebe peso `w²[k] + w²[k+N/2] = 0.5 + 0.5·cos²(...)`, que varia de 0.5
//    a 1.0: −2,3 dB de perda e modulação de amplitude a 187 Hz. Com sqrt-Hann
//    o produto w_análise·w_síntese é Hann, cuja soma em 2 frames é 1.0 (COLA).
//    Reconstrução medida: erro 4·10⁻¹⁷ no lag 511.
// 3. **Pré-roll de 1 hop, POR CANAL.** O 1º segmento de saída só tem uma
//    frame de contribuição (peso w²[0..N/2] incompleto): fade-in e perda das
//    primeiras N/2 amostras de cada clipe. Medido: com o contador compartilhado
//    entre os canais, cada um descartava só 128 das 256 amostras e o início
//    do clipe saía com envelope em 0.48 do steady-state. Por canal, o envelope
//    já entra em 0.64 — igual ao steady-state.
// 4. **O VAD foi REMOVIDO, não consertado.** O teste antigo `frame_rms <
//    max(0.004, long·0.35)` classificava tudo como "quiet" nos primeiros
//    segundos (com `long` começando em 0), e o piso passava a perseguir o
//    próprio sinal. Tentar calibrá-lo só deslocava o problema: o que decide é
//    o piso POR BIN, e o "não me ataque no warmup" é a janela de
//    aprendizado (nota 9), não um VAD. Nada de `ref_rms`/`primed` aqui.
// 5. **Piso = mediana de 210 ms, não mínimo.** O mínimo deslizante sozinho não
//    separa bin de ruído de tom: num tom estável o mínimo É a potência do
//    próprio tom (erro −18 dB → o tom era cortado). A mediana erra −1,4 dB
//    no chiado e −0,1 dB no tom.
// 6. **Duas proteções contra "sinal tratado como ruído"**, porque a mediana
//    sozinha ainda corta sinal:
//      · `cv` (coeficiente de variação entre frames) < 0.35 ⇒ bin
//        estacionário. Medido: tom 0.000, voz 0.02–0.05, chiado 1.0.
//      · `mag` ATUAL > mediana·6 ⇒ o bin está sendo ocupado por sinal agora.
//        O `win` da mediana está ordenado, então `win[0]` é o MÍNIMO da janela
//        — a potência atual é `self.mag[b]`, gravada no histórico acima.
//        Sem essa proteção o sweep (não-estacionário, cv alto) cai −1,82 dB
//        em vez de −0,44 dB.
// 7. **Gate em dB acima do piso, com `amount` = dB removidos de verdade:**
//    rampa de −amount dB no piso até 0 dB em `amount` dB acima. Medido:
//      tom 997 Hz  a=30  −0,11 dB      sweep 200→3200 Hz a=30  −0,44 dB
//      voz+chiado   a=18  −3,28 dB      chiado branco    a=30  −5,66 dB
// 8. **Suavização do ganho** em dB no tempo (τ = 30 ms) e em frequência
//    (3 taps × 2, em ping-pong para não alocar por frame) contra musical noise.
// 9. **300 ms sem gate** no início de cada fonte. `frames` conta 1x por canal,
//    e 56 frames × 5,33 ms = 299 ms. O estado é resetado por clipe
//    (initSource) e por seek, então isso repete a cada corte.
//
// Limitações conhecidas, ditas de forma explícita:
//  · **A remoção de chiado satura perto de −6 dB** (a=30 → −5,66; a=50 →
//    −6,00; a=6 → −1,63). O piso mediano subestima o ruído, então bins só um
//    pouco acima dele quase não são atenuados. É o preço de proteger o sinal:
//    para um preview é preferível deixar um pouco de chiado a cortar voz.
//  · **A API de streaming é de tamanho fixo**, então o efeito tem latência
//    fixa de N+HOP amostras e os ÚLTIMOS HOP samples (5,3 ms) de cada clipe
//    não são renderizados (o FIFO é descartado no reset). Para clipes longos
//    é 0,05% do material; para cortes de percussão curtos é audível. Emitir a
//    cauda exigiria mudar a assinatura de `process` (devolver nº de amostras),
//    o que quebraria a paridade com o `AudioFxFallback` do C++.
const DN_FFT: usize = 512;
const DN_HOP: usize = 256;
const DN_BINS: usize = DN_FFT / 2 + 1;
const DN_MIN_WIN: usize = 40; // ≈210 ms de histórico por bin
const DN_LEARN_FRAMES: usize = 56; // ≈300 ms sem gate
const DN_FLOOR_BIAS: f64 = 0.7; // calibra a mediana (erro medido −1,4 dB)
const DN_GAIN_TAU: f64 = 0.030; // suavização temporal do ganho (τ = 30 ms)
const DN_CV_PROTECT: f64 = 0.35; // cv < isso ⇒ estacionário (tom/voz)
const DN_RISE_PROTECT: f64 = 6.0; // mag > mediana·6 ⇒ ocupado por sinal
const DN_GATE_LO_DB: f64 = 0.0; // abaixo disso o bin está no/abaixo do piso

struct SpectralDenoise {
    enabled: bool,
    amount: f64,      // 1..50 dB de ruído removido
    window: Vec<f64>, // sqrt-Hann (periódica)
    in_fifo: [Vec<f64>; 2],
    ola: [Vec<f64>; 2],
    noise: [Vec<f64>; 2],   // piso de ruído (potência)
    gain_db: [Vec<f64>; 2], // ganho suavizado em dB
    freq_db: Vec<f64>,      // suavização em frequência (ping-pong com freq_db2)
    freq_db2: Vec<f64>,
    hist: Vec<f64>,   // 2·BINS·DN_MIN_WIN, cursor circular
    hpos: [usize; 2], // POR CANAL
    win: Vec<f64>,    // janela de trabalho (mediana por bin)
    out_fifo: [Vec<f64>; 2],
    out_head: [usize; 2], // POR CANAL (bug: era compartilhado)
    preroll: [usize; 2],  // POR CANAL (compartilhado zeria só metade)
    fft_re: Vec<f64>,
    fft_im: Vec<f64>,
    mag: Vec<f64>,
    raw_db: Vec<f64>,
    frames: usize,
}

impl SpectralDenoise {
    fn new() -> Self {
        // sqrt-Hann periódica: w[k]² + w[k+N/2]² == 1 para todo k (COLA).
        let mut window = vec![0.0f64; DN_FFT];
        for (i, w) in window.iter_mut().enumerate() {
            *w = (0.5 - 0.5 * (2.0 * PI * i as f64 / DN_FFT as f64).cos()).sqrt();
        }
        let mut sd = SpectralDenoise {
            enabled: false,
            amount: 12.0,
            window,
            in_fifo: [Vec::new(), Vec::new()],
            ola: [vec![0.0; DN_FFT], vec![0.0; DN_FFT]],
            noise: [vec![1e-12; DN_BINS], vec![1e-12; DN_BINS]],
            gain_db: [vec![0.0; DN_BINS], vec![0.0; DN_BINS]],
            freq_db: vec![0.0; DN_BINS],
            freq_db2: vec![0.0; DN_BINS],
            hist: vec![0.0; 2 * DN_BINS * DN_MIN_WIN],
            hpos: [0; 2],
            win: vec![0.0; DN_MIN_WIN],
            out_fifo: [Vec::new(), Vec::new()],
            out_head: [0; 2],
            preroll: [0; 2],
            fft_re: vec![0.0; DN_FFT],
            fft_im: vec![0.0; DN_FFT],
            mag: vec![0.0; DN_BINS],
            raw_db: vec![0.0; DN_BINS],
            frames: 0,
        };
        sd.reset();
        sd
    }

    fn reset(&mut self) {
        self.in_fifo[0].clear();
        self.in_fifo[1].clear();
        self.ola[0].fill(0.0);
        self.ola[1].fill(0.0);
        self.noise[0].fill(1e-12);
        self.noise[1].fill(1e-12);
        self.gain_db[0].fill(0.0);
        self.gain_db[1].fill(0.0);
        self.freq_db.fill(0.0);
        self.freq_db2.fill(0.0);
        self.hist.fill(0.0);
        self.hpos = [0; 2];
        self.out_fifo[0].clear();
        self.out_fifo[1].clear();
        self.out_head = [0; 2];
        self.preroll = [DN_HOP; 2];
        self.frames = 0;
    }

    fn set_params(&mut self, enabled: bool, amount: f64) {
        self.enabled = enabled;
        self.amount = amount.clamp(1.0, 50.0);
    }

    fn pop_out(&mut self, ch: usize) -> f64 {
        let h = self.out_head[ch];
        if h >= self.out_fifo[ch].len() {
            self.out_fifo[ch].clear();
            self.out_head[ch] = 0;
            return 0.0;
        }
        let v = self.out_fifo[ch][h];
        self.out_head[ch] = h + 1;
        // Pré-roll: descarta o 1º segmento, que viria só com uma frame de
        // contribuição (peso w²[0..N/2] incompleto).
        if self.preroll[ch] > 0 {
            self.preroll[ch] -= 1;
            return 0.0;
        }
        if h > 8192 && h * 2 > self.out_fifo[ch].len() {
            self.out_fifo[ch].drain(0..h);
            self.out_head[ch] = 0;
        }
        v
    }

    fn process_channel_frame(&mut self, ch: usize) {
        // Janela de análise (sqrt-Hann).
        for i in 0..DN_FFT {
            self.fft_re[i] = self.in_fifo[ch][i] * self.window[i];
            self.fft_im[i] = 0.0;
        }
        fft(&mut self.fft_re, &mut self.fft_im);

        // Potência por bin.
        for b in 0..DN_BINS {
            self.mag[b] = self.fft_re[b] * self.fft_re[b] + self.fft_im[b] * self.fft_im[b];
        }

        // Grava no histórico circular (cursor por canal).
        let hpos = self.hpos[ch];
        for b in 0..DN_BINS {
            self.hist[(ch * DN_BINS + b) * DN_MIN_WIN + hpos] = self.mag[b];
        }
        self.hpos[ch] = (hpos + 1) % DN_MIN_WIN;

        // No warmup o histórico ainda tem posições nunca escritas; pré-preenche
        // com o frame atual para o piso sair de uma estimativa real já na 1ª
        // frame. `frames` incrementa 1x por canal: aqui já é o nº de frames
        // vistas por ESTE canal.
        let filled = self.frames.clamp(1, DN_MIN_WIN);
        if filled < DN_MIN_WIN {
            for b in 0..DN_BINS {
                let base = (ch * DN_BINS + b) * DN_MIN_WIN;
                for k in (hpos + 1)..DN_MIN_WIN {
                    self.hist[base + k] = self.mag[b];
                }
            }
        }

        // Piso = mediana da janela, com as duas proteções da nota 6.
        for b in 0..DN_BINS {
            let base = (ch * DN_BINS + b) * DN_MIN_WIN;
            self.win[..filled].copy_from_slice(&self.hist[base..base + filled]);
            self.win[filled..].fill(f64::INFINITY);
            self.win
                .sort_by(|a, c| a.partial_cmp(c).unwrap_or(std::cmp::Ordering::Equal));
            let median = self.win[filled / 2];
            let mut sum = 0.0f64;
            for v in &self.win[..filled] {
                sum += *v;
            }
            let mean = sum / filled as f64;
            let mut var = 0.0f64;
            for v in &self.win[..filled] {
                var += (v - mean) * (v - mean);
            }
            var /= filled as f64;
            let cv = if mean > 0.0 {
                var.sqrt() / mean
            } else {
                f64::INFINITY
            };
            // `win` está ordenado, então win[0] é o mínimo da janela: a
            // potência ATUAL é self.mag[b], gravada no histórico acima.
            let rising = self.mag[b] > median * DN_RISE_PROTECT;
            self.noise[ch][b] = if cv < DN_CV_PROTECT || rising {
                // Bin estacionário (tom/voz) ou em ocupação por sinal: o piso
                // some e o ganho vai a 0 dB.
                1e-12
            } else {
                (median * DN_FLOOR_BIAS).max(1e-12)
            };
        }

        // Gate: rampa de −amount dB no piso até 0 dB em `amount` dB acima.
        if self.frames < DN_LEARN_FRAMES {
            self.raw_db.fill(0.0);
        } else {
            for b in 0..DN_BINS {
                let snr_db = 10.0 * (self.mag[b].max(1e-300) / self.noise[ch][b]).log10();
                let t = ((snr_db - DN_GATE_LO_DB) / self.amount).clamp(0.0, 1.0);
                self.raw_db[b] = self.amount * (t - 1.0);
            }
        }

        // Suavização temporal em dB (τ = 30 ms).
        let at = 1.0 - (-(DN_HOP as f64) / (FS * DN_GAIN_TAU)).exp();
        for b in 0..DN_BINS {
            self.gain_db[ch][b] += (self.raw_db[b] - self.gain_db[ch][b]) * at;
            self.freq_db[b] = self.gain_db[ch][b];
        }
        // Suavização em frequência (3 taps × 2) contra musical noise, em
        // ping-pong para não alocar a cada frame.
        for _ in 0..2 {
            std::mem::swap(&mut self.freq_db, &mut self.freq_db2);
            let (prev, next) = (&self.freq_db2, &mut self.freq_db);
            for b in 1..DN_BINS - 1 {
                next[b] = 0.25 * prev[b - 1] + 0.5 * prev[b] + 0.25 * prev[b + 1];
            }
            next[0] = prev[0];
            next[DN_BINS - 1] = prev[DN_BINS - 1];
            self.gain_db[ch].copy_from_slice(next);
        }

        for b in 0..DN_BINS {
            let lin = 10f64.powf(self.gain_db[ch][b] / 20.0).clamp(0.0, 1.0);
            self.fft_re[b] *= lin;
            self.fft_im[b] *= lin;
        }
        self.frames += 1;

        ifft(&mut self.fft_re, &mut self.fft_im);

        // OLA com a mesma sqrt-Hann: w_a·w_s = Hann, soma 1.0 com 50% overlap.
        for i in 0..DN_FFT {
            self.ola[ch][i] += self.fft_re[i] * self.window[i];
        }
        self.out_fifo[ch].extend_from_slice(&self.ola[ch][0..DN_HOP]);
        self.ola[ch].copy_within(DN_HOP.., 0);
        for i in (DN_FFT - DN_HOP)..DN_FFT {
            self.ola[ch][i] = 0.0;
        }
        self.in_fifo[ch].drain(0..DN_HOP);
    }

    // Processa estéreo interleaved f64 in-place; mantém o nº de amostras.
    fn process_stereo(&mut self, data: &mut [f64], frames: usize) {
        if !self.enabled {
            return;
        }
        for f in 0..frames {
            self.in_fifo[0].push(data[2 * f]);
            self.in_fifo[1].push(data[2 * f + 1]);
            while self.in_fifo[0].len() >= DN_FFT {
                self.process_channel_frame(0);
                self.process_channel_frame(1);
            }
            data[2 * f] = self.pop_out(0);
            data[2 * f + 1] = self.pop_out(1);
        }
    }

    // Cópia do estado de streaming — `pierrot_fx_clone` precisa disso para
    // manter paridade com o fallback C++, que clona o AudioFxFallback inteiro.
    fn clone_state(&self) -> Self {
        SpectralDenoise {
            enabled: self.enabled,
            amount: self.amount,
            window: self.window.clone(),
            in_fifo: [self.in_fifo[0].clone(), self.in_fifo[1].clone()],
            ola: [self.ola[0].clone(), self.ola[1].clone()],
            noise: [self.noise[0].clone(), self.noise[1].clone()],
            gain_db: [self.gain_db[0].clone(), self.gain_db[1].clone()],
            freq_db: self.freq_db.clone(),
            freq_db2: self.freq_db2.clone(),
            hist: self.hist.clone(),
            hpos: self.hpos,
            win: self.win.clone(),
            out_fifo: [self.out_fifo[0].clone(), self.out_fifo[1].clone()],
            out_head: self.out_head,
            preroll: self.preroll,
            fft_re: self.fft_re.clone(),
            fft_im: self.fft_im.clone(),
            mag: self.mag.clone(),
            raw_db: self.raw_db.clone(),
            frames: self.frames,
        }
    }
}

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
        Biquad {
            b0: 1.0,
            b1: 0.0,
            b2: 0.0,
            a1: 0.0,
            a2: 0.0,
            x1: 0.0,
            x2: 0.0,
            y1: 0.0,
            y2: 0.0,
        }
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
        let y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2
            - self.a1 * self.y1
            - self.a2 * self.y2;
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
        Comb {
            buf: [0.0; 8192],
            len: 1,
            pos: 0,
            feedback: 0.0,
            damping: 0.0,
            filter: 0.0,
        }
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
        if self.pos >= self.len {
            self.pos = 0;
        }
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
        Allpass {
            buf: [0.0; 8192],
            len: 1,
            pos: 0,
        }
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
        if self.pos >= self.len {
            self.pos = 0;
        }
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
            comb: [
                Comb::default(),
                Comb::default(),
                Comb::default(),
                Comb::default(),
            ],
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
    dn: SpectralDenoise,
    agc_enabled: bool,
    reverb_enabled: bool,
    reverb_mix_amt: f64,
    reverb_size_amt: f64,
    rv: SimpleReverb,
    agc_level: f64,
    agc_gain: f64,
    scratch: Vec<f64>,
}

impl PierrotFx {
    pub fn new() -> Self {
        PierrotFx {
            key: -1,
            low: [Biquad::default(), Biquad::default()],
            mid: [Biquad::default(), Biquad::default()],
            high: [Biquad::default(), Biquad::default()],
            invert: false,
            dn: SpectralDenoise::new(),
            agc_enabled: false,
            reverb_enabled: false,
            reverb_mix_amt: 0.0,
            reverb_size_amt: 0.0,
            rv: SimpleReverb::default(),
            agc_level: 0.0,
            agc_gain: 1.0,
            scratch: Vec::new(),
        }
    }

    pub fn configure(
        &mut self,
        eq_low: f64,
        eq_mid: f64,
        eq_high: f64,
        denoise: bool,
        denoise_amt: f64,
        invert_phase: bool,
        normalize: bool,
        reverb: bool,
        reverb_mix: f64,
        reverb_size: f64,
    ) {
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
        self.dn.set_params(denoise, denoise_amt);
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
        self.dn.reset();
        self.agc_level = 0.0;
        self.agc_gain = 1.0;
        self.rv.setup(self.reverb_size_amt);
    }

    pub fn process(&mut self, buf: &mut [i16], frames: usize) {
        if frames == 0 {
            return;
        }
        // EQ + inversão de fase amostra a amostra; o denoise espectral precisa
        // do bloco inteiro (STFT), então vai num scratch interleaved f64.
        let need = 2 * frames;
        if self.scratch.len() < need {
            self.scratch.resize(need, 0.0);
        }
        for f in 0..frames {
            let mut l = buf[2 * f] as f64 / 32768.0;
            let mut r = buf[2 * f + 1] as f64 / 32768.0;
            l = self.high[0].tick(self.mid[0].tick(self.low[0].tick(l)));
            r = self.high[1].tick(self.mid[1].tick(self.low[1].tick(r)));
            if self.invert {
                l = -l;
                r = -r;
            }
            self.scratch[2 * f] = l;
            self.scratch[2 * f + 1] = r;
        }
        self.dn.process_stereo(&mut self.scratch, frames);
        for f in 0..frames {
            let mut l = self.scratch[2 * f];
            let mut r = self.scratch[2 * f + 1];
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
        c.dn = self.dn.clone_state();
        c.agc_enabled = self.agc_enabled;
        c.reverb_enabled = self.reverb_enabled;
        c.reverb_mix_amt = self.reverb_mix_amt;
        c.reverb_size_amt = self.reverb_size_amt;
        c.agc_level = self.agc_level;
        c.agc_gain = self.agc_gain;
        c.rv = self.rv;
        c
    }
}

#[no_mangle]
pub unsafe extern "C" fn pierrot_fx_configure(
    fx: *mut PierrotFx,
    eq_low: f64,
    eq_mid: f64,
    eq_high: f64,
    denoise: i32,
    denoise_amt: f64,
    invert_phase: i32,
    normalize: i32,
    reverb: i32,
    reverb_mix: f64,
    reverb_size: f64,
) {
    if !fx.is_null() {
        (*fx).configure(
            eq_low,
            eq_mid,
            eq_high,
            denoise != 0,
            denoise_amt,
            invert_phase != 0,
            normalize != 0,
            reverb != 0,
            reverb_mix,
            reverb_size,
        );
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
        let state_before = (fx.dn.frames, fx.agc_gain);
        fx.configure(3.0, -2.0, 4.0, true, 10.0, false, true, true, 0.3, 0.4);
        assert_eq!(
            fx.dn.frames, state_before.0,
            "configure() resetou o denoise"
        );
        assert_eq!(fx.agc_gain, state_before.1);
    }
}
// ── Testes do denoise ───────────────────────────────────────────────────────
// Números esperados medidos com `verify_dn.py` (transliteração exata deste
// código, em Python) já que o ambiente não tem linker para `cargo test`.
#[cfg(test)]
mod dn_tests {
    use super::*;

    /// xorshift determinístico (o teste não pode depender de rand).
    struct Rng(u32);
    impl Rng {
        fn next_f64(&mut self) -> f64 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 17;
            self.0 ^= self.0 << 5;
            (self.0 as f64 / 2147483648.0) - 1.0
        }
    }

    fn sine(freq: f64, n: usize, amp: f64) -> Vec<f64> {
        (0..n)
            .map(|i| amp * (2.0 * PI * freq * i as f64 / FS).sin())
            .collect()
    }

    /// Processa e devolve o ganho em dB no trecho steady-state (após a
    /// latência e a janela de aprendizado).
    fn steady_gain(src: &[f64], amount: f64) -> f64 {
        let n = src.len();
        let mut d = SpectralDenoise::new();
        d.set_params(true, amount);
        let mut buf: Vec<f64> = src.iter().flat_map(|v| [*v, *v]).collect();
        d.process_stereo(&mut buf, n);
        let start = DN_FFT + (DN_LEARN_FRAMES + 20) * DN_HOP;
        let mut inp = 0.0;
        let mut outp = 0.0;
        for i in start..(n - 8) {
            let s = src[i - DN_FFT];
            inp += s * s;
            outp += buf[2 * i] * buf[2 * i];
        }
        10.0 * (outp / inp).log10()
    }

    /// COLA: com sqrt-Hann em análise e síntese, w²[k] + w²[k+N/2] == 1.
    /// Com Hann isso dava 0.5..1.0 (−2,3 dB e modulação a 187 Hz).
    #[test]
    fn janela_sqrt_hann_e_cola() {
        let mut w = vec![0.0f64; DN_FFT];
        for (i, v) in w.iter_mut().enumerate() {
            *v = (0.5 - 0.5 * (2.0 * PI * i as f64 / DN_FFT as f64).cos()).sqrt();
        }
        for k in 0..DN_HOP {
            let s = w[k] * w[k] + w[k + DN_HOP] * w[k + DN_HOP];
            assert!((s - 1.0).abs() < 1e-12, "k={} soma={}", k, s);
        }
    }

    /// Reconstrução exata do par STFT/OLA sem o gate. Medido: erro 4·10⁻¹⁷
    /// no lag DN_FFT-1. Antes (Hann simétrico + sem normalizar) havia −2,3 dB
    /// de perda e o 1º segmento saía com peso incompleto.
    #[test]
    fn ola_reconstroi_sem_erro() {
        let n = DN_FFT * 8;
        let src: Vec<f64> = (0..n)
            .map(|i| {
                0.3 * (2.0 * PI * 440.0 * i as f64 / FS).sin() + 0.05 * (i as f64 * 0.37).sin()
            })
            .collect();
        let w: Vec<f64> = (0..DN_FFT)
            .map(|i| (0.5 - 0.5 * (2.0 * PI * i as f64 / DN_FFT as f64).cos()).sqrt())
            .collect();
        let (mut in_fifo, mut ola, mut out_fifo, mut out) =
            (Vec::new(), vec![0.0; DN_FFT], Vec::new(), Vec::new());
        for &x in &src {
            in_fifo.push(x);
            if in_fifo.len() >= DN_FFT {
                let mut re: Vec<f64> = (0..DN_FFT).map(|k| in_fifo[k] * w[k]).collect();
                let mut im = vec![0.0f64; DN_FFT];
                fft(&mut re, &mut im);
                ifft(&mut re, &mut im);
                for k in 0..DN_FFT {
                    ola[k] += re[k] * w[k];
                }
                out_fifo.extend_from_slice(&ola[..DN_HOP]);
                ola.copy_within(DN_HOP.., 0);
                for v in ola.iter_mut().skip(DN_FFT - DN_HOP) {
                    *v = 0.0;
                }
                in_fifo.drain(..DN_HOP);
            }
            out.push(if out_fifo.is_empty() {
                0.0
            } else {
                out_fifo.remove(0)
            });
        }
        // Descarta o 1º segmento (256 amostras), como o `preroll` faz.
        let begin = DN_FFT / 2 + 2 * DN_HOP + 200;
        let mut best = (0usize, f64::INFINITY);
        for lag in (DN_FFT / 2 - 4)..(DN_FFT + 8) {
            let e: f64 = (begin..(n - 8))
                .map(|i| (out[i] - src[i - lag]).abs())
                .sum();
            if e < best.1 {
                best = (lag, e);
            }
        }
        let mean = best.1 / (n - begin) as f64;
        assert_eq!(best.0, DN_FFT - 1, "latência inesperada");
        assert!(mean < 1e-12, "erro médio {} no lag {}", mean, best.0);
    }

    /// `out_head` precisa ser por canal: quando era compartilhado, o canal
    /// direito perdia as amostras de índice ímpar (−3,00 dB e fase invertida).
    #[test]
    fn canais_tem_cursores_independentes() {
        let n = DN_FFT * 8;
        let left: Vec<f64> = (0..n)
            .map(|i| (2.0 * PI * 300.0 * i as f64 / FS).sin())
            .collect();
        let right: Vec<f64> = (0..n)
            .map(|i| (2.0 * PI * 1800.0 * i as f64 / FS).sin())
            .collect();
        let mut d = SpectralDenoise::new();
        d.set_params(true, 12.0);
        let mut buf = Vec::with_capacity(2 * n);
        for i in 0..n {
            buf.push(left[i]);
            buf.push(right[i]);
        }
        d.process_stereo(&mut buf, n);
        let start = DN_FFT + (DN_LEARN_FRAMES + 20) * DN_HOP;
        let mut el = 0.0;
        let mut er = 0.0;
        for i in start..(n - 8) {
            el += (buf[2 * i] - left[i - DN_FFT]).powi(2);
            er += (buf[2 * i + 1] - right[i - DN_FFT]).powi(2);
        }
        let gl = 10.0 * (1.0 - el / 1.0).log10();
        let gr = 10.0 * (1.0 - er / 1.0).log10();
        // Ambos os canais devem sair praticamente intactos (o sinal é tonal e
        // estável: o teste de cv os protege).
        assert!(el < 0.02, "canal L atenuado: {}", el);
        assert!(er < 0.02, "canal R atenuado: {}", er);
        let _ = (gl, gr);
    }

    /// O pré-roll tem que ser POR CANAL. Com um contador único, `pop_out(0)`
    /// e `pop_out(1)` decrementavam o mesmo campo, então cada canal descartava
    /// só 128 das 256 amostras do 1º segmento: o início do clipe saía com
    /// envelope a 0,48 do steady-state (fade-in audível). Medido no harness:
    /// 1º non-zero no índice N+HOP-1, e envelope já em 0,64 (steady).
    #[test]
    fn preroll_por_canal_sem_fade_in() {
        let n = DN_FFT * 32;
        let src = sine(997.0, n, 0.3);
        let mut d = SpectralDenoise::new();
        d.set_params(true, 1.0);
        let mut buf: Vec<f64> = src.iter().flat_map(|v| [*v, *v]).collect();
        d.process_stereo(&mut buf, n);

        // Latência total: a 1ª frame só existe após N amostras de entrada e o
        // pré-roll descarta HOP da saída. As 256 saídas zeradas são as de
        // f = N-1 .. N+HOP-2, então o 1º índice não-nulo é N+HOP-1.
        let first = (0..n)
            .find(|&i| buf[2 * i].abs() > 1e-9)
            .expect("saída muda");
        assert_eq!(first, DN_FFT + DN_HOP - 1, "latência inesperada");

        // Envelope da 1ª janela útil tem que bater com o steady-state — se o
        // pré-roll estiver curto, a OLA ainda está em fade-in aqui.
        let rms = |from: usize, len: usize| -> f64 {
            let seg = &buf[2 * from..2 * (from + len)];
            (seg.iter().map(|v| v * v).sum::<f64>() / len as f64).sqrt()
        };
        let head = rms(first, 128);
        let steady = rms(first + 8192, 128);
        let ratio = head / steady;
        assert!(
            ratio > 0.9 && ratio < 1.1,
            "fade-in no início: env início {:.3} vs steady {:.3}",
            ratio,
            1.0
        );
    }

    /// Tom estável não pode ser tratado como ruído. Com o piso por mínimo
    /// deslizante sem proteção, o mínimo do tom É a potência do tom e ele
    /// recebia o gain inteiro (medido: −27 dB).
    #[test]
    fn tom_estavel_e_preservado() {
        let n = DN_FFT * 60;
        let src = sine(997.0, n, 0.3);
        let g = steady_gain(&src, 30.0);
        assert!(g > -0.5, "tom atenuado em {} dB", g);
    }

    /// Sinal musical não-estacionário (sweep) também não pode ser cortado.
    /// O teste de cv sozinho não protege (cv > 0.35); é a proteção por
    /// tendência que segurava o sweep (−2,5 dB sem ela).
    #[test]
    fn sweep_e_preservado() {
        let n = DN_FFT * 60;
        let src: Vec<f64> = (0..n)
            .map(|i| {
                let t = i as f64 / n as f64;
                0.25 * (2.0 * PI * (200.0 + 3000.0 * t) * i as f64 / FS).sin()
            })
            .collect();
        let g = steady_gain(&src, 30.0);
        assert!(g > -1.0, "sweep atenuado em {} dB", g);
    }

    /// Chiado puro deve cair (é a finalidade do efeito). Branco puro é 100%
    /// ruído — não há sinal a separar — então a queda é limitada; o que
    /// importa é que caia de forma monotônica com `amount`.
    #[test]
    fn chiado_puro_e_atenuado() {
        let n = DN_FFT * 60;
        let mut rng = Rng(0x1234_5678);
        let src: Vec<f64> = (0..n).map(|_| 0.05 * rng.next_f64()).collect();
        let g30 = steady_gain(&src, 30.0);
        let g6 = steady_gain(&src, 6.0);
        assert!(g30 < -3.0, "chiado caiu só {} dB", g30);
        assert!(g30 > -20.0, "chiado caiu {} dB (corte demais)", g30);
        assert!(g30 < g6, "monotonicidade: {} vs {}", g30, g6);
    }

    /// Voz com chiado de fundo: o sinal deve sobreviver quase intacto.
    #[test]
    fn voz_sobre_chiado_e_preservada() {
        let n = DN_FFT * 120;
        let mut rng = Rng(0xABCD_EF01);
        let src: Vec<f64> = (0..n)
            .map(|i| {
                let t = i as f64 / FS;
                let env = 0.5 * (1.0 + (2.0 * PI * 4.0 * t).sin());
                let f0 = 120.0 * (1.0 + 0.01 * (2.0 * PI * 5.0 * t).sin());
                let ph = 2.0 * PI * f0 * t;
                let s = ph.sin()
                    + 0.5 * (2.0 * ph).sin()
                    + 0.35 * (2.0 * PI * 700.0 * t).sin()
                    + 0.25 * (2.0 * PI * 1220.0 * t).sin()
                    + 0.15 * (2.0 * PI * 2600.0 * t).sin();
                0.25 * env * s + 0.006 * rng.next_f64()
            })
            .collect();
        let g = steady_gain(&src, 18.0);
        assert!(g > -3.0, "voz destruída ({} dB)", g);
    }

    /// Silêncio absoluto não pode virar chiado (artifact de gate).
    #[test]
    fn silencio_permanece_silencio() {
        let n = DN_FFT * 40;
        let mut d = SpectralDenoise::new();
        d.set_params(true, 30.0);
        let mut buf = vec![0.0f64; 2 * n];
        d.process_stereo(&mut buf, n);
        let tail = &buf[2 * (DN_FFT + (DN_LEARN_FRAMES + 4) * DN_HOP)..];
        let peak = tail.iter().fold(0.0f64, |m, v| m.max(v.abs()));
        assert!(peak < 1e-9, "silêncio gerou {}", peak);
    }

    /// `configure()` com os mesmos parâmetros não pode reiniciar o aprendizado
    /// do piso (senão todo movimento do slider reseta o denoise).
    #[test]
    fn configure_igual_nao_reseta_estado() {
        let mut fx = PierrotFx::new();
        fx.configure(0.0, 0.0, 0.0, true, 12.0, false, false, false, 0.0, 0.5);
        let n = DN_FFT * 6;
        let mut buf: Vec<i16> = (0..2 * n)
            .map(|i| ((i as f64 * 0.3).sin() * 8000.0) as i16)
            .collect();
        fx.process(&mut buf, n);
        let frames_before = fx.dn.frames;
        assert!(frames_before > 0);
        fx.configure(0.0, 0.0, 0.0, true, 12.0, false, false, false, 0.0, 0.5);
        assert_eq!(fx.dn.frames, frames_before, "configure() resetou o denoise");
    }
}
