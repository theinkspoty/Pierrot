# Relatório — Bibliotecas de efeitos de vídeo open source

Levantamento no GitHub (2026-10-02) de bibliotecas/plugins de efeitos que
editores como Premiere, Resolve, Kdenlive e Natron usam — para alimentar o
roadmap de efeitos do Pierrot quando for a hora.

**Contexto do Pierrot hoje:** já existe host OFX em `src/colombina/ofx/`
(`OfxPluginManager` + `OfxHost` + `OfxRenderer`), com scan de bundles
(`PIERROT_OFX_PATH`, caminhos comuns de `/usr/OFX/Plugins`). O export usa CLI
`ffmpeg` (`filter_complex`). O preview de efeitos nativos (brilho, chroma,
EQ…) é QPainter/DSP no `PreviewWidget`.

**Regra de ouro que não muda:** paridade preview ↔ export. Qualquer plugin
OFX/frei0r no preview precisa de caminho no `ProjectExporter` (ffmpeg) ou
pré-render — o ROADMAP já trata disso.

---

## 1. OpenFX (OFX) — o padrão dos hosts profissionais

Premiere/Resolve/Nuke/Vegas consomem efeitos OFX (ou wrappers). O Pierrot já
é host OFX: **instalar bundles é o caminho de menor esforço**.

| Repo | Conteúdo | Licença | Nota |
|---|---|---|---|
| [AcademySoftwareFoundation/openfx](https://github.com/AcademySoftwareFoundation/openfx) | SDK oficial (headers + support lib + samples) | BSD-3 | O que o host usa por baixo |
| [NatronGitHub/openfx-misc](https://github.com/NatronGitHub/openfx-misc) | Grade, ColorCorrect, Blur, Transform, ChromaKeyer, LensDistortion, Retime, CornerPin, TrackerPM, CImg blur/sharpen… | GPL-2+ | ⭐ Catálogo mais “NLE-like” |
| [NatronGitHub/openfx-arena](https://github.com/NatronGitHub/openfx-arena) | Text, Swirl, Wave, Edges, Oilpaint, Cartoon, Polar, Tile… | GPL-2 | Looks artísticos; alguns OCL opcionais |
| [NatronGitHub/openfx-gmic](https://github.com/NatronGitHub/openfx-gmic) | Wrapper OFX do G’MIC (centenas/milhares de filtros de imagem) | GPL-2 / CeCILL | Também há build para AE/Premiere |
| [purzbeats/purzos-ofx](https://github.com/purzbeats/purzos-ofx) | 64 plugins retro/analog/glitch/CRT-VHS/pixelart/grade | MIT | Releases com Linux x64 |
| [gyroflow/gyroflow-plugins](https://github.com/gyroflow/gyroflow-plugins) | Stabilization Gyroflow em OFX **e** frei0r | GPL | Um repo, dois formatos |
| [tuttleofx/TuttleOFX](https://github.com/tuttleofx/TuttleOFX) | Host + biblioteca de plugins + plugins de IO | GPL | Referência de host |
| [chaert-s/spektrafilm-ofx](https://github.com/chaert-s/spektrafilm-ofx) | Film simulation / print / grain (GPU) | GPL-3 | macOS/Windows primeiro; Linux manual |

### Instalação típica (hosts OFX)

- Linux: `/usr/lib/x86_64-linux-gnu/OFX/Plugins/`, `/usr/OFX/Plugins/`,
  `~/.local/share/OFX/Plugins/` ou `OFX_PLUGIN_PATH`.
- Pierrot: `PIERROT_OFX_PATH` + caminhos padrão já em
  `OfxPluginManager::searchPaths()`.

### Licença × Pierrot (GPL-3.0-or-later)

| Licença do plugin | Compatível? |
|---|---|
| GPL-2/3 (misc, arena, gmic, frei0r) | ✅ dinâmico; o Pierrot já é GPL |
| MIT (purzOS) | ✅ |
| BSD (OpenFX SDK) | ✅ |

Não embutir binários de terceiros no repo sem clareza de licença; o AppImage
pode listar plugins opcionais ou deixar o usuário apontar o caminho.

---

## 2. frei0r — padrão dos NLEs FOSS

API C mínima (um header): `f0r_update` em frames RGBA32. Usado por **MLT,
Kdenlive, Shotcut, Flowblade, FFmpeg, GStreamer**. Muito mais barato de
implementar host do que OFX completo.

| Repo | Conteúdo | Licença |
|---|---|---|
| [dyne/frei0r](https://github.com/dyne/frei0r) | 100+ filtros, mixers, sources | GPL-2 |
| [xsbee/frei0r-effects](https://github.com/xsbee/frei0r-effects) | Coleção extra no mesmo API | — |
| [rectalogic/mediafx-frameserver](https://github.com/rectalogic/mediafx-frameserver) | Host frei0r **out-of-process** (plugins em Python/Rust/Node) | — |

### Efeitos que casam com o DNA Vegas do Pierrot

| Categoria | Exemplos |
|---|---|
| Cor / look | `saturat0r`, `white-balance`, `sop-sat` (ASC CDL), `normaliz0r`, `levels`, `tint0r` |
| Key / chroma | `bluescreen0r`, `spillsupress`, `keyspillm0pup`, `select0r` |
| Estilo | `vignette`, `glow`, `softglow`, `edgeglow`, `posterize`, `cartoon`, `pixeliz0r` |
| Transições | `wipe-*` (dois, círculo, barn door…), `xfade0r` |
| Geradores | `nois0r`, `plasma`, `ising0r`, test patterns |
| Blend | dezenas de mixers (`overlay`, `multiply`, `screen`…) |

**Hoje o Pierrot já tem 12 blend modes + transições internas** — o ganho do
frei0r está nos **filtros de look/key/transição extras**, não em reinventar
blend.

---

## 3. Caminhos que não são “plugin”

| Fonte | Papel no Pierrot |
|---|---|
| **FFmpeg filters** | Export já usa `filter_complex` (`eq`, `curves`, `unsharp`, `chromakey`, `gblur`…). Preview pode espelhar mais desses filtros sem novo host. |
| **G’MIC** (OFX ou CLI) | Volume de looks; peso de dependência; melhor como OFX opcional. |
| **GLSL / Vapoursynth** | GPU e script — fora do escopo curto (kernel 100% CPU QPainter hoje). |

---

## 4. Recomendação (ordem de ataque quando abrir o item)

```
Prioridade 1 — sem novo host
  openfx-misc   → Grade, Blur, Transform, ChromaKeyer, ColorCorrect
  openfx-arena  → Text, Swirl, Wave, Edges
  purzOS OFX    → looks retro/glitch (MIT; builds Linux)

Prioridade 2 — host novo, API barata
  frei0r        → vignette, glow, wipes, white-balance; ~100 plugins de brinde
                  e o mesmo padrão do Kdenlive

Prioridade 3 — volume
  openfx-gmic   → centenas de filtros; avaliar tamanho/custo
```

**Paridade preview↔export** (não negociável): para cada efeito de terceiro
no preview, mapear para ffmpeg (`filter_complex`) ou pré-render de sequência
PNG (padrão já usado no `renderVelocitySequence`).

---

## 5. Onde entra no roadmap (pendente de decisão)

- Não implementado ainda; registro para **não se perder** quando a fila de
  efeitos abrir (Fase 2/3 do ROADMAP ou um bloco “Catálogo de efeitos”).
- ROADMAP já marca **VST/CLAP como adiado** (alto custo); OFX/frei0r são o
  caminho realista antes disso.
- Item de doc relacionado: **“Guia de plugin OFX”** (seção 6 do plano vivo).
- Decisões a tomar na hora:
  1. Só documentar/empacotar OFX vs. implementar host frei0r também;
  2. Empacotar plugins no AppImage vs. instruir download;
  3. Fase: 0.7 (catálogo barato) vs. 0.8 (junto de LUTs/máscaras).

---

## Referências

- OpenFX: https://openeffects.org/ · SDK GitHub: AcademySoftwareFoundation/openfx
- frei0r: https://frei0r.dyne.org/ · github.com/dyne/frei0r
- Natron plugins: github.com/NatronGitHub (openfx-misc, openfx-arena, openfx-gmic)
- MLT + OpenFX (em evolução): github.com/mltframework/mlt (módulo openfx)
- G’MIC: https://gmic.eu/
