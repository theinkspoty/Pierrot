# Relatório — Efeitos nativos do Pierrot vs Premiere

Levantamento em 2026-10-02 para o roadmap. Status dos efeitos **embutidos**
(no app, sem plugin) e do que falta para o nível “Effect Controls” do Premiere.

**Regra de ouro:** paridade preview ↔ export. Todo efeito nativo novo precisa
de caminho nos dois lados (ou pré-render como LAINKA/OFX).

---

## 1. Inventário atual

### 1.1 Em módulos puros (testáveis, header-only)

| Módulo | Efeitos | Preview | Export |
|---|---|---|---|
| `src/colombina/fx/ColorGrade.h` | Lumetri: exposure, H/S/W/B, sat/vibrance, temp/tint, LGG, curves, vignette, LUT `.cube`, sharpen, blend | ✅ `applyToImage` | ✅ `ffmpegFilters` (`eq`, `colorbalance`, `curves`, `vignette`, `unsharp`, `lut3d`) |
| `src/colombina/export/LainkaFx.h` | LAINKA (stop motion): skip, jitter, flicker, warp, onion skin, dust, scratch, motion blur, opacity | ✅ `lainkaApplyFx` | ✅ pré-render `LainkaRenderer` |

### 1.2 Inline em `PreviewWidget::applyBasicEffectsOn` (não extraído)

| Efeito | Campos no `Clip` | Export ffmpeg |
|---|---|---|
| Brilho / contraste / saturação | `brightness`, `contrast`, `saturation` | `eq=` |
| Desfoque | `blur` | `boxblur=` |
| Preto e branco | `grayscale` | `colorchannelmixer=` |
| Chroma key + softness + despill | `chromaKey*` | `chromakey=` |
| Máscaras rect/ellipse + feather | `masks` | `geq=` |
| Motion blur (por clipe) | `motionEnabled`, `motionAmount/Angle/Samples` | `motionblur=` |

### 1.3 Áudio nativo (DSP)

| Efeito | Preview | Export |
|---|---|---|
| EQ Express (3 bandas) | C++/Rust | ffmpeg EQ |
| Reverb EX (Schroeder/Freeverb) | C++/Rust | `aecho`/`amix` |
| Denoise (STFT no Rust) | Rust + fallback C++ | `afftdn` |
| Normalize / invert phase | modelo + mix | expressões no export |

Porta Rust: `src/rust/pierrot_audiofx` (OFF por padrão em `CMakeLists.txt`).
Fallback C++: `AudioFxFallback` em `PreviewWidget.cpp`. **Sem teste de paridade**
entre as duas implementações (registrado no ROADMAP).

### 1.4 Plugins de terceiros (hosts prontos)

| Host | Status | Modelo |
|---|---|---|
| **frei0r** | ✅ implementado | `Clip::frei0rFx`, painel Efeitos, export `frei0r=` |
| **OFX** | ✅ já existia | `Clip::ofxFx`, export pré-render `OfxExportRenderer` |

---

## 2. Comparação com o Premiere (Effect Controls)

| Premiere (nativo) | Pierrot | Gap |
|---|---|---|
| Lumetri Color | ✅ ColorGrade | — |
| Transform | ✅ tx/ty/scale/rot + keyframes | — |
| Crop | ✅ crop + masks | — |
| Opacity / blend | ✅ por clipe e faixa | — |
| Gaussian / Fast Blur | ✅ boxblur | qualidade vs gaussian |
| Tint / Black & White | ✅ grayscale + tint parcial (LUT/temp) | sem “Tint” dedicado |
| Find Edges / Posterize / Solarize | ❌ | frei0r pode cobrir |
| Replace Color / Leave Color | ❌ | — |
| Warp Stabilizer | ❌ | alto custo |
| Change to Color | ❌ | — |
| Extract | ❌ | — |
| Vertical Hold | ❌ | fácil (shift de linha) |
| Audio: compressor/limiter nativos no clip | ⚠️ mixer tem strip; sem compressor por clipe completo | — |

### Score

```
Efeitos nativos embutidos (funcionam)     ██████░░░░  60%
Biblioteca nativa organizada (módulos)    ██░░░░░░░░  20%  (só ColorGrade + Lainka)
Plugins (frei0r/OFX)                      ████████░░  80%
Paridade áudio Rust↔C++                   █░░░░░░░░░  10%  (sem tst_audiofx_parity)
```

---

## 3. Fila sugerida (quando abrir o item)

### P0 — Organizar o que já existe

1. **Extrair `NativeFx.h`** em `src/colombina/fx/` — chroma, blur, eq, grayscale,
   masks; preview e export chamam a **mesma** função (paridade por construção).
2. **`tst_audiofx_parity.cpp`** — mesmo buffer no Rust e no C++ fallback.
3. Listar nativos no painel Efeitos sob categoria **“Nativos”** (hoje os IDs
   `pierrot_*` já aparecem; falta unificar com ColorGrade/LAINKA no mesmo catálogo).

### P1 — Efeitos de look que o Premiere tem e o Pierrot não

| Efeito | Esforço | Alternativa imediata |
|---|---|---|
| Vertical Hold | baixo | frei0r `verticalhold` se existir |
| Posterize | baixo | frei0r `posterize` |
| Find Edges / Emboss | baixo | frei0r / openfx-misc |
| Tint dedicado | médio | LUT/temp já cobrem boa parte |
| Replace Color | médio | — |

### P2 — Caros (depois de QA/kernel estável)

- Warp Stabilizer  
- Compressor/limiter nativos por clipe (audiofx)  
- GPU na composição  

---

## 4. Decisões pendentes

1. Extrair `NativeFx.h` agora ou só quando precisar de teste unitário?
2. Embutir catálogo frei0r “curado” (vignette, saturat0r, posterize…) no AppImage?
3. Ordem: P0 (organizar) → P1 (looks) → P2 (caros)?

---

## Referências

- `Arquivos/Relatorios/Relatorio-Bibliotecas-Efeitos-Video.md` — OFX/frei0r
- `ROADMAP.md` — seção Cor + “Catálogo de efeitos open source”
- `src/colombina/fx/ColorGrade.h`, `src/colombina/export/LainkaFx.h`
- `src/rust/pierrot_audiofx/` — DSP de áudio
