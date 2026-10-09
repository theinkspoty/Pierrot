# Relatório do pipeline de áudio do Pierrot

Estado do caminho de áudio (decodificação → DSP → mix no preview / filtros no
export). Baseado na leitura do código em 2026-09-22. Referências `arquivo:linha`.

## Panorama

O áudio do Pierrot tem **duas cadeias independentes que convergem em paridade**:

1. **Preview (tempo real)**: `FFmpegDecoder` → `AudioConformCache` (buffer PCM
   S16/48k estéreo) → `AudioMixer` (lê fatias, Interpola speed≠1, DSP
   `pierrot_audiofx` em Rust via FFI, pan equal-power, de-click, barramentos
   por faixa) → `QAudioSink` (Qt Multimedia).
2. **Exportação (offline)**: `ProjectExporter` sintetiza cadeias do ffmpeg
   (`atempo`, `afade`, `adelay`, `volume`/`aeval`, `afftdn`, `equalizer`,
   `loudnorm`, `aecho`+`asplit`+`amix`) aplicadas a cada clipe e a cada
   barramento de faixa, finalizando em `amix` + `volume` master.

## Cadeia do preview

### Decodificação — `FFmpegDecoder` (src/laartman/FFmpegDecoder.cpp)
- Resample padrão S16 interleaved, 48 kHz estéreo (`swr`, FFmpegDecoder.cpp:595-620).
- Contexto de áudio **separado** do de vídeo com mutex próprio (`m_audioMutex`,
  h:123) → audio e vídeo decodificam em threads distintas; `open()` segura os
  dois mutexes ao fechar contextos (cpp:456-458).
- `seekAudio` (cpp:1126-1148): `av_seek_frame` com `AVSEEK_FLAG_BACKWARD` +
  flush; descarte de "rebarba" (overlap do pacote anterior) agora **guiado por
  PTS** (`m_audioSeekTargetSec`, tolerância 2ms, cpp:1208-1221) em vez da
  contagem fixa (`m_audioSkipFrames`, fallback para stream sem PTS confiável).
- Múltiplos streams OBS/câmera: `resolveAudioStream` mapeia k-ésima faixa →
  stream absoluto (cpp:64-78); `Clip::audioStreamIndex` escolhe a faixa.

### Conform — `AudioConformCache` (src/colombina/ffmpeg/AudioConformCache.cpp)
- Decodifica **uma vez** por `(arquivo, stream)` em worker dedicado
  (`workerLoop`, cpp:309-374) — evita re-decode/ecos de junção em cortes.
- Leitura posicional por índice de amostra (sem `av_seek_frame` no read path);
  speed≠1 com interpolação linear na leitura (preview).
- Refcount + LRU (512 MB, cpp:378-412) só coleta chunks sem leitores externos.
- Rodízio por urgência: menor frame não decodificado mais próximo do playhead
  (cpp:336-340); decoder persistente por chunk (sem re-open); `doFill` marca a
  cauda de fim de arquivo como coberta (silêncio) para não re-seek em loop.

### Mix — `AudioMixer` (src/ui/PreviewWidget.cpp:539-1155, QIODevice)
- Ordem por fonte (clip): DSP (EQ → invert → denoise → normalize → reverb) →
  de-click por detecção (kDeclick=64, só se houve descontinuidade real,
  cpp:959-1018) → soma no barramento da faixa com pan equal-power (centro 1:1,
  0dB).
- Depois, por **barramento de faixa**: DSP de faixa (cpp:1060-1077) →
  master volume → RMS/VU.
- Dedupe de sobreposições por `path|stream` (GeomRec + SeenWin) evita soma 2x.
- `waitReadyBeforeSink` (cpp:756-779) bloqueia até o head do playhead estar
  conformado — evita começo "mudo/dessincronizado".

### DSP — `src/rust/pierrot_audiofx/src/lib.rs` (via FFI, header AudioFxBridge.h)
- EQ Express: 3 biquads peaking (120/1000/6000 Hz, Q=1, ±12 dB) —
  lib.rs:203-207.
- Denoise: gate/expander exponencial (floor −50 dB) — lib.rs:241-253.
- Normalize: AGC lento com alvo ~−14 dB RMS + soft-clip 0.95 — lib.rs:254-269.
- Reverb EX: Schroeder (4 comb + 2 allpass, wet*4, crossfade dry/wet) —
  lib.rs:110-145.
- `configure` cacheia por key inteira (lib.rs:189-202) → reconfiguração só
  quando muda (estados preservados); `pierrot_fx_clone` permite copiar estado
  sem FFI extra. Fallback C++ idêntico (`AudioFxFallback`, PreviewWidget.
  cpp:340-527) quando `PIERROT_ENABLE_RUST` não está definido.

## Cadeia do export (ProjectExporter.cpp)

Por clipe de áudio (1481-1589): `atrim`+`asetpts` → `atempo(speed)` (base,
1496) → `afade` in/out (+ crossfade de transição) → `adelay` → `volume`
(envelope do clipe × envelope da faixa, clamp 0..2, 1523-1529) → `afftdn`
(denoise) → `equalizer` 3 bandas → `aeval` pan equal-power (envelope ou estático)
→ `aformat/aresample` → `loudnorm` (normalize, I=−14, limita pico −1.5 TP) →
`aeval` invert → reverb `aecho`+`asplit`+`amix`.

Por faixa (barramento, 1591-1650): `amix` dos clipes da faixa → `afftdn` →
`equalizer` → `aeval` invert → `aecho` reverb → mistura final `amix` +
`volume` master.

Volume efetivo (comentário 1515-1518): envelope do clipe × volume do clipe ×
envelope da faixa — **mesmo produto do preview** (buildMixSources), clampado a
200%.

## Paridade preview ↔ export (o que importa)

| Aspecto | Preview | Export | Alinhado? |
|---|---|---|---|
| Formato | S16 48k estéreo | fltp → 48k master | ✓ |
| Volume (clip × clipe × faixa) | buildMixSources | `volume='clip(...)'` | ✓ (mesma expressão) |
| Pan equal-power | centro 1:1 (0 dB) | `aeval` equal-power | ✓ (centro não atenua) |
| EQ 3 bandas | biquads Rust (120/1k/6k, Q1) | `equalizer` ffmpeg (mesmos f/q) | ✓ (RBJ vs ffmpeg: pequena diferença de curva) |
| Denoise | gate aproximado | `afftdn` | ~ (algoritmos distintos) |
| Normalize | AGC aproximado | `loudnorm` (real, com TP limit) | ~ (preview aproxima) |
| Invert | × −1 | `aeval=-val` | ✓ |
| Reverb | Schroeder (comb+allpass) | `aecho` multi-tap | ~ (same caráter, não idêntico) |
| Velocidade áudio | `speed` base (interp. linear) | `atempo(speed` base) | ✓ entre si; ⚠ sedo da curva |
| Automação faixa (kfVolume/kfPan) | avaliada em `t` | `kfExpr` em `t` | ✓ |

## O que está "no conformes" (verificado)

1. **Seek preciso por PTS** + fallback — resolve relato antigo de áudio
   "teleportando" o playhead em MKV longo/índice esparso.
2. **Conform elimina eco de junção por construção** (corte = intervalos
   adjacentes do mesmo buffer).
3. **Ordem de FX por faixa espelha a do preview** (denoise → EQ → invert →
   reverb) tanto em clipe quanto em barramento.
4. **Dedupe de sobreposições** e **de-click só em descontinuidade real**.
5. **Denoise e normalize SÃO exportados** (afftdn/loudnorm presentes) — sem
   efeito "fantasma" que só se ouve no preview.
6. Budget de conform (512 MB) com refcount/LRU — sem vazamento com dezenas de
   fontes.

## Pontos de atenção (não são bugs, são limitações registradas)

1. **Áudio × velocidade variável (kfSpeed)**: o vídeo usa `clipSrcTime`
   (integra a curva, pré-render image2); o **áudio segue o `speed` base** no
   preview (`step = max(0.01, speed)`, PreviewWidget.cpp:633) e no export
   (`atempo(speed)`, ProjectExporter.cpp:1496). Preview e export **coincidem**
   (paridade preservada), mas o comportamento ideal (pitch/time variável) é
   pendência explícita do ROADMAP (áudio segue o `speed` base).
2. **Reverb/denoise/normalize são aproximações** diferentes entre os dois
   lados — caráter similar, não bit-exato. Aceitável para preview, mas quem
   confia no loudness final deve validar no export.
3. **`pierrot_audiofx` (Rust) precisa do build Rust**; sem `PIERROT_ENABLE_RUST`
   cai no `AudioFxFallback` C++ (mesma lógica). Risco: as duas cópias podem
   divergir num refactor futuro.
4. **Ordem invert/denoise difere** entre preview (invert antes do gate) e
   export (afftdn antes do invert) — efeito auditivo mínimo, mas é uma
   assimetria de ordem documentável.

## Veredito

**Está nos conformes.** O pipeline está bem construído, com as duas pontas
concordando nos pontos críticos (volume/invert/pan/automação), decodificação
thread-safe e conform que elimina os defeitos clássicos de corte. As
diferenças restantes são aproximações de DSP (reverb/denoise/normalize) e a
pendência conhecida de áudio com envelope de velocidade — ambas registradas no
ROADMAP, não defeitos novos.