# Relatório: estudo dos modos de decodificação — Pierrot (Laartman) vs Kdenlive vs Flowblade vs FilmCraft

Data: 2026-10-19
Escopo: comparar o motor de decodificação do Pierrot (**Laartman**) com o de outros NLEs,
identificar pontos fortes/fracos de cada e definir o plano de evolução do **Laartman v0.3**.

Fontes: código do Pierrot (`src/laartman/`, `src/ui/monitor/PreviewWidget.*`,
`src/colombina/`, `ROADMAP.md`, `FEATURES.md`, `Arquivos/Arquitetura/KERNEL.md`,
`Arquivos/Relatorios/Relatorio-Performance-Playback.md`), código e documentação públicos
do MLT (`producer_avformat.c`, `mlt_consumer.c`, docs do MLT/Kdenlive) e do projeto
FilmCraft (storytold/filmcraft).

---

## 1. Versões dos motores

| Camada | Versão | Papel |
|---|---|---|
| **Laartman** (`src/laartman/`) | **v0.3** | Motor de codecs: decodificação/decodificação de áudio e vídeo (libav*), busca frame-exata, cache LRU, VAAPI |
| **Colombina** (`src/colombina/`) | **v0.1** | Kernel acima do motor: modelo, cache de mídia, proxies, conform de áudio, render 2D (Mesa), export |

---

## 2. Como cada programa decodifica

### 2.1 Pierrot — Laartman (FFmpeg in-process)

Motor próprio em C++ sobre libav* (`FFmpegDecoder`, 1.531 linhas):

- **Pool de decoders** por arquivo (padrão 6, `PIERROT_DECODER_POOL`), lease por caminho
  com eviction LRU de entradas ociosas; o custo dominante de `open()` (500–800 ms) é pago
  uma vez por fonte, não por corte.
- **Caches por decoder**: LRU de 120 frames (~240 MB), thumbnails (512 mem + 4096 disco),
  picos de áudio (teto 2 M buckets).
- **Conform de áudio estilo Olive** (`AudioConformCache`): PCM S16/48 kHz estéreo indexado
  por intervalos, orçamento 512 MB, worker único persistente com decoder reutilizado;
  corte com continuidade amostra-exata por construção.
- **Pipeline de latência em 3 camadas**: lookahead no decoder (`m_lastFrame`/`m_nextFrame`)
  + pipeline de 1 frame no `FrameWorker` + prefetch/warm do próximo corte (janela
  `min(5s, max(2s, 0.8×dur))`), escondendo o open frio.
- **Seek**: `AVSEEK_FLAG_BACKWARD` no keyframe + decode-discard, com re-seek só se o alvo
  estiver >0,5 s atrás ou >2,0 s à frente; convenção de hold com tolerance 0,1/fps;
  cancelamento cooperativo (token atômico) com preservação de progresso; coalescing de
  pedidos de scrub (guarda só o alvo mais recente).
- **Qualidade de preview**: 360p–4K com degradação adaptativa por latência (badge
  "Auto: Xp"); largura efetiva `max(160, max(qualidade, widget))`.
- **Proxy**: H.264 CRF 28 `veryfast` para fontes ≥2560 px, gerado por subprocesso ffmpeg
  CLI com rename atômico; export e áudio sempre no original.
- **Hardware**: VAAPI apenas, device único compartilhado por refcount, `thread_count=1`,
  negociação NV12 explícita, auto-cura após 4 falhas seguidas (desliga VAAPI na sessão).
  Painéis de UI forçam software (`setHardwareDecodeAllowed(false)`).
- **Instrumentação**: hits/misses de cache, contagem/tempo de seek, custo de open,
  discard de frames, tempo de trabalho — tudo no próprio decoder + `Bench --bench`.

### 2.2 Kdenlive — MLT/libavformat

- Motor de terceiros: **MLT**. O producer `avformat` mantém `AVFormatContext` de vídeo e
  de áudio separados, com `packets_thread` dedicada (pacotes lidos por ahead com
  mutex/cond), decode sob demanda, `image_cache`/`audio_cache` do MLT, detecção de VFR,
  workaround de DTS negativo e error concealment (`last_good_frame`).
- Paralelismo vem do **consumer**: `real_time` = N threads de produção à frente com fila
  de frames, `prefill`, frame-dropping adaptativo (aumenta buffer se cai frames demais).
  É um pipeline de frames compostos prontos, não de decoders por arquivo.
- **Kdenlive adiciona sobre o MLT**:
  - proxy por clipe configurável (perfil editável, limiar de largura, teste comparativo
    de perfis de proxy, proxy gerado por câmera);
  - **preview renderizado em chunks** (`kdenlive_render preview-chunks`): zonas
    verde/amarelo/vermelha na timeline indicando o que já foi pré-renderizado;
  - cache de producers variantes (`ClipController`: cortes áudio/vídeo/speed pré-montados);
  - efeitos GPU (Movit) — hoje desabilitados por incompatibilidade com o MLT.
- Hardware decode no MLT é desilitado por padrão e documentado como pouco efetivo
  (a transferência GPU→RAM é gargalo; o pipeline é CPU-bound por design).

### 2.3 Flowblade — MLT/libavformat (mesmo motor, menos camadas)

- Mesmo pipeline do Kdenlive (MLT + `avformat` producer + consumer `real_time`).
- Adiciona: proxy files em resoluções fixas, media relinking, saída VAAPI/NVENC no
  **encode**.
- Não tem: preview renderizado por região, controle fino de qualidade de preview,
  as melhorias que o Kdenlive empilhou sobre o MLT. hw decode fraco/ausente.
- Projeto menor → menos evolução do motor, mas previsibilidade e simplicidade.

### 2.4 FilmCraft — Rust, codecs próprios (sem FFmpeg)

- Decodificadores escritos do zero em Rust (H.264/HEVC/ProRes/VP9/AV1/DNxHR/…),
  bit-exact testados contra FFmpeg como oráculo externo; ~500–600 fps H.264 1080p em
  software.
- Tempo em **ticks inteiros** (254.016.000.000/s) — 23.976 e 29.97 drop-frame exatos,
  sem drift.
- Compositor GPU (wgpu: Metal/Vulkan/DX12/WebGPU) em luz linear, com caminho CPU
  espelhado e testes golden-image entre os dois.
- **Render previews** cacheados **por conteúdo**: barra verde/amarelo/vermelha na
  timeline; edição só invalida o que toca; undo restaura o verde.
- Hardware decode: VideoToolbox (macOS), Media Foundation (Windows), VAAPI/NVDEC
  (Linux); WebCodecs no browser.
- Limitações: alpha — mídia real menos testada, Linux menos testado; cobertura de
  formatos menor que FFmpeg (sem RAW, E-AC-3); sem plugins (VST/OpenFX); entrega
  software limitada a H.264.

---

## 3. Tabela comparativa — pontos fortes e fracos

| | Pontos fortes | Pontos fracos |
|---|---|---|
| **Pierrot/Laartman** | Pool de decoders com lease (open pago 1× por fonte); prefetch+warm escondendo open de 500–800 ms; cancelamento cooperativo + coalescing de scrub; conform PCM amostra-exato (melhor que o dos dois MLT); qualidade adaptativa com badge; VAAPI com auto-cura; métricas embarcadas + `Bench`; `tst_concurrency` no locking | 1 worker serial para topo+camadas (H3); dois pools de decoder sem coordenação (preview × Mesa); LRU O(n) (M2, ~6 µs); memória do cache explode em 4K (~4 GB cheio); VAAPI único com `thread_count=1` e toggle que exige restart; fallback hw silencioso; `open()` ainda custo dominante em corte frio; sem smart render; sem preview renderado por região; sem seek index; `PreviewWidget.cpp` com 5,5k linhas intestável |
| **Kdenlive (MLT)** | Pipeline de produção paralelo (`real_time` N threads + prefill + drop adaptativo); proxy profundo (perfil editável, por clipe, teste de eficiência, proxy de câmera); preview chunks na timeline; cache de producers variantes; base madura e formatos "grátis" via FFmpeg | Tudo assenta no MLT: hwaccel desabilitado por padrão e pouco efetivo; efeitos GPU (Movit) quebrados/desabilitados; arquitetura de propriedades string + lazy-evaluation difícil de depurar; latência de play menor que a de um NLE moderno (sem prefetch de "corte quente" agressivo como o Pierrot); sem cancelamento cooperativo granular no decode; compostagem em CPU |
| **Flowblade (MLT)** | Simplicidade e previsibilidade; o mesmo pipeline de N threads do MLT; proxy files; media relinking; saída VAAPI/NVENC no encode | Sem preview-renderado-por-região; menos controle fino de qualidade de preview; hw decode fraco/ausente; projeto pequeno → menos evolução |
| **FilmCraft** | Codecs próprios bit-exact e muito rápidos; tempo inteiro exato (zero drift); compositor GPU em luz linear com paridade CPU testada; render previews cacheados por conteúdo com invalidação precisa + undo; hw decode multiplataforma moderna; caminho WebCodecs no browser | Alpha — mídia real menos testada, Linux menos testado; codecs próprios = cobertura menor que FFmpeg; sem plugins (VST/OpenFX); entrega software limitada a H.264 |

### O que cada um tem e o Pierrot não (e vice-versa)

- **Kdenlive tem e Pierrot não**: preview renderizado por chunks na timeline, proxy por
  clipe/perfil editável, teste comparativo de perfis de proxy.
- **FilmCraft tem e Pierrot não**: compositor GPU, tempo inteiro exato, cache de preview
  por conteúdo com invalidação por edição, degradação visível em 3 cores.
- **Pierrot tem e todos os outros não**: prefetch+warm que esconde `open()` (o MLT paga
  open/reopen no corte; Kdenlive mitiga com cache de producers; Flowblade pouco),
  cancelamento cooperativo com preservação de progresso, conform PCM amostra-exato
  indexado, métricas de decode embarcadas no motor.

---

## 4. Débitos técnicos do Laartman hoje (do código e do ROADMAP)

1. **M2 — LRU O(n)**: rebuild do índice `QHash` a cada hit/insert
   (`FFmpegDecoder.cpp:875–912`); ~6 µs/decode; pendente.
2. **Memória do cache de frames**: 120 entradas × QImage por decoder (pool = 6) — até
   ~960 MB a 1080p e ~4 GB a 4K com cache cheio. "Limite de RAM em 4K" aberto.
3. **Um único worker de vídeo para topo + camadas** (H3): camadas inferiores "roubam
   slots" do `FrameWorker` serial; mitigado por prioridade, não resolvido.
4. **Dois pools de decoders paralelos sem coordenação**: `DecoderPool` do preview (6) e
   `m_decoders` do `MesaRenderer` (ilimitado).
5. **Cobertura de testes baixa**: decoder 2,3%, `ProxyManager` 0%, `MesaRenderer` 0%
   (medição de 2026-09-29); `AudioMixer`/`FrameWorker`/`DecoderPool`/`BgPrefetchWorker`
   são classes aninhadas no .cpp de 5,5k linhas — intestáveis por construção; zero
   sanitizers no CI.
6. **VAAPI reativo**: config lida uma vez por processo (`hwDisabled()`,
   `FFmpegDecoder.cpp:98–104`) — mudar o toggle exige restart; fallback silencioso
   (só log).
7. **`open()` ainda é o custo dominante** (500–800 ms) em arquivo novo sem prefetch;
   não há seek index persistido.
8. **`FFmpegEncoder` órfão**: código vivo (encode/mux in-process), usado só pelo
   `tst_encoder`; nenhum caminho do app o chama — é o bloco pronto para proxy
   in-process e preview renderizado por região.
9. **Sem smart render** no export (sempre recodifica; maior gap vs Vegas).
10. **ROADMAP contradiz a si mesmo**: Fase 1 lista Proxy/Decode multi-thread como
    pendentes; a seção 4 os marca como feitos (parcialmente verdade: existe o proxy
    automático global e o `warmTracks` do Mesa; falta o toggle por faixa e o decode
    paralelo do preview "de verdade").

---

## 5. Plano de evolução — Laartman v0.3 e depois

### F1 — Unificar o decode (fecha H3)
1. [x] **Pool global de decoders** — `DecoderPool` movido para
   `src/laartman/` como singleton com DUAS visões sobre o mesmo entry:
   lease exclusivo (preview) e `acquireShared()` (Mesa/composição), budget
   único (PIERROT_DECODER_POOL, padrão 10) + eviction LRU de ociosas.
   O mapa ilimitado de `shared_ptr` do MesaRenderer saiu.
2. Workers por faixa/prioridade: `FrameWorker` deixa de ser serial para
   topo+camadas. (pendente — H3)
3. [x] Corrigir M2: LRU com lista intrusiva (O(1)) em vez de rebuild do
   `QHash`.

### F2 — Latência e visibilidade (roubar do Kdenlive/FilmCraft)
4. Preview renderizado por região (chunks com barra verde/amarelo/vermelha) usando o
   `FFmpegEncoder` hoje órfão — cache por conteúdo como o FilmCraft.
5. Ladder de degradação visível: prefetch → motion blur → largura → software, com badge
   (hoje só largura adaptativa silenciosa + auto-cura VAAPI no log).
6. Proxy completo: toggle por faixa (`Track::proxyPreview`), perfil editável, proxy
   in-process via `FFmpegEncoder` (hoje subprocesso CLI com timeout).

### F3 — Robustez do decoder
7. VAAPI reativo (sem restart do app) + indicador na UI de hw ativo/degradado.
8. Seek index persistido (keyframes por arquivo em cache) para cortes frios.
9. Limite de RAM do cache de frames em 4K (budget global, não 120 × QImage por decoder).

### F4 — QA (pré-requisito de qualquer v3)
10. Extrair `DecoderPool`/`FrameWorker`/`BgPrefetchWorker`/`AudioMixer` para headers
    próprios (hoje classes aninhadas no .cpp de 5,5k linhas).
11. Testes de `frameAt`/seek/hw, sanitizers no CI, meta de cobertura >50% no kernel.

**Não priorizar**: codecs próprios estilo FilmCraft (irrealista; a cobertura do FFmpeg é
vantagem) — mas copiar a ideia de tempo em ticks inteiros se o drift de 23.976/29.97
algum dia aparecer.

---

## 6. Progresso da execução

- [x] Relatório gravado (este arquivo).
- [x] Drift corrigido em `ROADMAP.md` e `KERNEL.md`.
- [x] Versões colombina v0.1 / laartman v0.3 expostas no Sobre.
- [x] F4: `DecoderPool`, `BgPrefetchWorker`, DSP de áudio, `AudioMixer`, `FrameWorker`
      e `MonitorScrubBar` extraídos do `PreviewWidget.cpp` para módulos próprios
      (`src/ui/monitor/DecoderPool.*`, `BgPrefetchWorker.*`, `PreviewDebug.h`,
      `src/ui/audio/AudioFxDsp.*`, `AudioMixer.*`, `src/ui/monitor/FrameWorker.*`,
      `MonitorScrubBar.*`).
- [x] Testes novos: `tst_decoderpool` e `tst_audiofx_dsp` (Qt Test, no CI via ctest).
- [x] **Bug real achado pelo `tst_audiofx_dsp`**: a quantização de volta do DSP
      (`AudioFxFallback`/`GraphicEq`/`ParametricEqDsp` e o somatório do
      `AudioMixer`) usava `clamp(v,-1,1) * 32768.0` → em exatamente +1.0 gerava
      32768, que com wrap para `int16_t` virava **−32768** (clique na
      saturação). Corrigido para `* 32767.0` em todos os pontos. Se o DSP Rust
      (`pierrot_audiofx`) repetir a fórmula, o `tst_audiofx_pity` futuro deve
      cobrir o mesmo caso.
- [x] **Fragmentação do `FFmpegDecoder.cpp`** (1.531 → 4 TUs): núcleo+frameAt
      (590), `FFmpegDecoderOpen.cpp` (334, lifecycle+VAAPI), `FFmpegDecoderAudio.cpp`
      (483, picos+seek/decode), `FFmpegDecoderProbe.cpp` (88) + `FFmpegDecoderPriv.h`
      (helpers internos inline). Testes: 13/13, incluindo `tst_concurrency` (o
      contrato de locking não mudou).
- [x] **Preset "Bancada"** (workspace estilo estúdio, ref. bancada Disney):
      1 tela = Source+Filtros+Scopes+mixer sob a timeline; 2 telas = preview
      externo em tela cheia na secundária (monitor de referência); 3+ telas =
      `TechnicalWindow` (segundo QMainWindow, `src/ui/core/TechnicalWindow.*`)
      na tela esquerda com Mixer/Efeitos/Express/Analisadores por padrão —
      menu "Painéis" escolhe os docks (continuam arrastáveis/tabificáveis);
      janela principal no centro, referência na direita.
- [ ] F1–F3: próximas rodadas (não fazem parte desta execução).
