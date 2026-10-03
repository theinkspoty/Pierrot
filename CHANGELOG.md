# Changelog

## 0.7 (2026-10-02)

Release de fluxo Premiere + efeitos + UI. Naipe continua **alpha**.

### Fluxo de edição (Premiere)

- **Source Monitor** (dock): duplo-clique na Central de Mídias abre a mídia;
  In/Out com `I`/`O`; **Insert** (`,`) e **Overwrite** (`.`) no playhead.
- Menu da pool: **Abrir no Source** / **Inserir no playhead**.
- **Multicâmera**: selecione ≥2 clipes de vídeo → **Criar multicam**;
  teclas `1..N` cortam o ângulo no playhead; preview/Mesa/export usam o
  ângulo ativo (`kfAngle`).

### Velocidade (Time Remapping)

- Dock **Editor de Velocidade**: curva velocidade×tempo bezier.
- **Varinha (Curva/B)**: clique na linha cria keyframe; arraste as alças
  entorta a velocidade; **F9** Easy Ease.
- Banda de velocidade desenhada no clipe da timeline (Rápido/Baixo + %).

### Efeitos

- Host **frei0r** (padrão Kdenlive/Shotcut/MLT): painel Efeitos, aba Express,
  export via `frei0r=` no `filter_complex`.
- Host **OFX** já existia (Natron/openfx-misc/purzOS).
- Relatórios: bibliotecas de efeitos e inventário de nativos em
  `Arquivos/Relatorios/`.

### Cor (Lumetri)

- **Color grade** por clipe: exposure, H/S/W/B, sat/vibrance, temperature/tint,
  Lift/Gamma/Gain, curvas RGB, vinheta, LUT `.cube`, sharpen, blend.
- UI com abas Básico / Rodas / Curvas / Vinheta-LUT.
- Export espelha o preview (`eq`, `colorbalance`, `curves`, `vignette`,
  `unsharp`, `lut3d`).

### UI (painéis / Premiere)

- **Workspaces** nomeados + presets Edição / Áudio / Composição / Efeitos.
- **Menu Exibir agrupado** por região (esquerda/direita/embaixo).
- Densidade (paddings mais justos) e **escuro em 3 níveis**.
- **Mini-waveform** no Program Monitor.
- Mesa documentada no FEATURES.

### Outros

- Easy Ease (**varinha**) no Editor de Curvas (`F9`) e no Editor de Velocidade.
- Serialização de multicam, frei0r e campos Lumetri no `.Blanc` e presets.
- Testes: round-trip de multicam/frei0r/grade em `tst_serialization`.

### Conhecido / pendente

- Áudio com envelope de velocidade (ainda usa `speed` base).
- Three-point completo e Expanded Edit.
- Essential Sound / ducking.
- Nesting de sequence (Mesa ≠ nested sequence).
- HSL Secondary, RGB parade, keyframes de grade.
- `tst_concurrency` pode falhar se o ambiente não decodificar a mídia sintética.

---

## 0.6

Versão anterior (alpha): timeline Vegas-style, LGG + scopes, LAINKA, OFX host,
CI, save assíncrono, proxy, etc. Ver `ROADMAP.md` e commits anteriores.
