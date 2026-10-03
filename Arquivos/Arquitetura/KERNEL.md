# Kernel Pierrot (`src/colombina`) — relatório e comparação com o Vegas Pro

Relatório do núcleo de render/decodificação/exportação do Pierrot, separado da
UI, e comparação com o Sony Vegas Pro (a referência de fluxo do editor).
Atualizado em 2026-09-22 (v0.6 alpha). Código em 2026-10-02: v0.7 alpha
(Source, multicam, frei0r, Lumetri, UI Premiere — ver CHANGELOG.md).

## Números

| Métrica | Valor |
|---|---|
| Kernel `src/colombina` | 17.789 LOC — 40 arquivos |
| DSP de áudio (`src/rust/pierrot_audiofx`) | 398 LOC |
| UI (`src/` fora do kernel e do Rust) | ~34.700 LOC — 77 arquivos |
| Kernel no total do projeto | ~1/3 |

O kernel = `models` + `ffmpeg` + `render` + `export` + `ofx` + `util/generators`.

## Módulos e responsabilidades

| Camada | Arquivos | LOC | Papel |
|---|---|---|---|
| `models/` | `Project.h/.cpp` | ~1.500 | O coração: `Project`, `Track`, `Clip`, `MediaItem`, `Keyframe`, Mesas, recursos de texto, serialização `.Blanc` |
| `ffmpeg/` | `FFmpegDecoder`, `MediaCache`, `ProxyManager`, `AudioConformCache` | ~2.050 | Decodificação, picos/waveforms, thumbs, proxies, buffer de áudio tipo Olive |
| `render/` | `MesaRenderer` | ~630 | Composição 2D (estilo AE): câmera, motion blur, warmTracks paralelo, cache de composto |
| `export/` | `ProjectExporter`, pré-renders, `NleInterchange` | ~2.300 | Geração do `filter_complex`/CLI do ffmpeg, LAINKA, OFX, EDL CMX3600 |
| `ofx/` | Host + Manager + Renderer | ~1.900 | Host OFX com 7 suites (`dlopen` de plugins de terceiros) |
| `util.h` + `generators.h` | — | ~100 | `isImageFile`, 4 geradores determinísticos |
| ⚙️ `src/rust/pierrot_audiofx` | — | 398 | DSP (EQ Express, reverb Schroeder) via FFI |

## Destaques de design

- **Modelo puro Qt, header-heavy**: `kfValue`/`clipSrcTime`/`clipSpeedAt` são
  `inline` — a matemática de keyframes (linear/smooth/step/bezier paramétrico
  com Newton) vive no header e é compartilhada por UI, preview e export sem
  divergência.
- **Paridade preview ↔ export é lei**: `ProjectExporter` gera `filter_complex`
  (blend, texto→PNG, cromakey, velocidade por pré-render `image2`, LAINKA,
  OFX) que reproduz exatamente o que o CPU `QPainter` mostra no preview.
- **Thread-safety em 2 níveis** (`MesaRenderer.h`): mutex global breve + mutex
  por decoder → arquivos diferentes decodam em paralelo (worker de decode +
  thread da UI) sem risco de SIGSEGV no `avcodec_send_packet`.
- **5 camadas de cache**: composto (8 quadros, invalidação por `revision`),
  frame por decoder (LRU 120), thumbs (512 mem + 4096 disco), picos (64),
  áudio conform (até 512 MB, S16 estilo Olive com refcount → corte com
  continuidade amostra-exata por construção).

## Aceleração por GPU (estado atual)

GPU existe apenas em duas pontas, **não** na composição:

| Ponta | Tecnologia | Padrão | Comentário |
|---|---|---|---|
| Decode de vídeo | VAAPI (Linux) | **Ligado** (`hwDecode`, tv. `PIERROT_GPU=0`) | Dispositivo VAAPI compartilhado no processo; auto-desligamento se o driver falhar repetidamente |
| Encode na exportação | `h264_nvenc` (prio NVIDIA) → `h264_vaapi` | **Desligado** (`exportHwEncode`) | Detecta o encoder no ffmpeg do sistema; `-cq`/`-qp` |
| **Composição** (blend, câmera, motion blur, efeitos) | — | — | **100% CPU** (`QPainter`/`QImage`) — maior gap de perf vs. Vegas (OpenCL/CUDA) |

## Comparação com o Vegas Pro

### O que já copia do Vegas (consciente, documentado em FEATURES.md)

- **Interação**: agulha sempre à mão, shuttle JKL, fades nos cantos, opacidade
  no topo do clipe, barra de velocidade no cabeçalho, presets de clipe,
  geradores de mídia, "cópias unificadas" de texto, mixer escuro, painel
  Histórico de Edições.
- **Mixer**: modos Touch/Write/Latch gravando automação nos envelopes
  `Track::kfVolume/kfPan` — reproduz igual no preview e no export.

### Onde o Vegas ainda está à frente

| Área | Vegas | Pierrot hoje |
|---|---|---|
| GPU | composição + export OpenCL/CUDA | só VAAPI no decode; comp é CPU |
| Render | SmartRender (stream copy em trechos intactos) | re-codifica tudo |
| Áudio | motor próprio + VST + automação profunda | DSP Rust básico (EQ/reverb), sem VST, sem LUFS |
| Fluxo | media bins, relink, conform, proxy por faixa, multicam, timeline aninhada | sem multicam/nested; relink em pauta |
| Ecossistema | scripting .NET, device capture, titler avançado, Blu-ray | não previsto no curto prazo |
| Maturidade | ~25+ anos de código | ~6 semanas (v0.7 alpha) |

### Pontos a favor do Pierrot

- **EDL CMX3600 import/export** limpo e sem FFmpeg (`NleInterchange`,
  testável) — o Vegas não exporta EDL completo; diferencial real para
  interoperar com Resolve/Premiere.
- **Host OFX próprio** (~1.000 LOC de suites, `dlopen`) — os mesmos plugins do
  Resolve/Natron rodam aqui (o Vegas também suporta OFX; empate técnico no
  nível do kernel).
- **Canvas multi-camada estilo AE dentro do NLE** (`MesaComposition`):
  posição/escala/rotação/âncora + câmera + motion blur com conteúdo fixo —
  modelo explícito que o Vegas permite, mas raramente expõe assim.

### Veredito

O kernel cobre ~100% do "corte simples" (YouTube/estabilidade) e ~30% do de
produção (GPU, smart render, áudio profissional, multicam). Bate com a decisão
do ROADMAP: fechar confiança (CI, anti-crash, métricas) antes de feature
paridade — é o que separa "estável" de "alpha".

> Nota de build: o alvo de build/teste é Ubuntu (apt tem Qt6/ffmpeg prontos no
> CI; AppImage embarca as libs para qualquer distro) — não é uma limitação do
> produto.