# Roadmap — rumo ao nível Vegas

Plano vivo para levar o Pierrot de editor alpha a um NLE profissional no Linux.
Marcamos `[x]` (feito) / `[~]` (parcial) / `[ ]` (não feito).

> Restrição central: **dois caminhos de render andam juntos.** Preview (CPU,
> `QPainter`/`QImage`) e export (`ffmpeg` + `filter_complex`). Toda feature nova
> exige os dois lados — ou o preview mostra algo que o export não reproduz.

---

## Fase 0 — Fundação (estabilidade e confiança)

- [x] **Suíte de testes** — round-trip `.Blanc`, keyframes, gerador do comando
  ffmpeg, `tst_export_pipeline` (roda o ffmpeg de verdade e compara com o
  preview), EDL, conform de áudio, CrashReporter. `ctest` verde.
- [~] **CI** — GitHub Actions com build Qt6 + `ctest` funcionando; falta
  asan/ubsan, AppImage como artefato e `-Wall -Wextra`. Job Qt5 removido (o
  código não compila com Qt5; estava verde-por-mentira).
- [~] **Endurecimento de crash** — `FFmpegDecoder` quase todo serializado, LRU
  com sufixo `Locked`, `CrashReporter` com inventário de threads e testes
  (`tst_crashreporter`). Falta: reduzir os SIGSEGV reais (asan/ubsan em CI).
- [ ] **Projetos grandes** — carregar/salvar com progresso sem travar a UI
  (`toJson`/`fromJson` hoje síncronos).

## Fase 1 — Performance (o "não trava" de verdade)

- [ ] **Proxy files** — versões leves em background, alternância automática
  (reusa `MediaCache`).
- [x] **Cache de render** — quadro composto cacheado por timestamp no
  `MesaRenderer` (LRU 8 quadros), invalidação via `Project::revision`.
- [ ] **Decode multi-thread** — decodificar faixas em paralelo no preview
  (hoje o mutex do `MesaRenderer` serializa isso no paint).
- [ ] **Smart render** — re-exportar trechos intactos com `stream copy`
  (importante para cortes simples).

## Fase 2 — Edição (o "corpo" do Vegas)

- [ ] **Máscaras animadas** — `Clip::masks` + keyframes bezier; preview por
  `QPainterPath`, export via `mask`/`crop`/`geq`. Aceite: paridade.
- [ ] **Track Motion** — transform na faixa inteira (reaproveitar o transform
  por camada que a Mesa já faz).
- [x] **Velocity envelopes** — `kfSpeed`/`clipSrcTime`, paridade preview↔export
  via `renderVelocitySequence`. Pendências: UI da curva no clipe, áudio/OFX com
  envelope.
- [x] **Multicâmera** — `isMulticam` + `kfAngle`, teclas 1..N, preview/export
  expandidos por ângulo. Pendências: sync por áudio, grade de ângulos.
- [ ] **Timeline aninhada** — projeto como mídia dentro de outro.

## Fase 3 — Áudio (motor forte do Vegas)

- [x] **Automação gravável** — gravação em tempo real de `kfVolume`/`kfPan`
  (mixer sincronizado, modos Touch/Write/Latch); preview e export já consumiam
  os envelopes. Pendências: editar/exibir a curva na timeline, keyframe de
  retorno no Touch.
- [ ] **LUFS + normalização** — loudness meters (`ScopeWidget`) + `loudnorm` no
  export.
- [ ] **Mais efeitos por faixa** — compressor, de-esser, limiter (ffmpeg já tem
  `acompressor`/`alimiter`).
- [ ] **VST/CLAP (adiado)** — host via JACK/PipeWire, não plugar direto.
- [~] **Catálogo OFX/frei0r** — host frei0r implementado (scan de paths + pilha
  `Clip::frei0rFx` + preview e export). Pendências: empacotar plugins no
  AppImage, UI de caminhos em Configurações, teste de paridade.

## Fase 4 — Fluxo profissional e ecossistema

- [ ] **EDL/XML/AAF import/export** — EDL e FCPXML primeiro, AAF/OMF por último.
- [ ] **Monitor externo (Decklink)** + **entrada SDI/NDI**.
- [ ] **Scripting/automação** — bindings Lua/Python expondo o `Project`.
- [ ] **Docs + comunidade** — tutorial, guia de plugins, página de presets.

## Ordem de ataque sugerida

1. Fase 0 (testes + CI). 2. Máscaras + velocity envelopes. 3. Proxy + cache.
4. Automação gravável. 5. Multicâmera + track motion. 6. Resto conforme demanda.

> Regra de ouro: nenhuma feature entra sem teste no Fase 0 e sem paridade
> preview↔export.

---

## Contra-check Kdenlive (foco decidido em 2026-09-22)

O Pierrot não compete em feature count: compete em **não perder trabalho**.

### Núcleo de foco (o mínimo que muda o jogo, antes de feature nova)

- [x] **Backup rotativo do `.Blanc`** — `~/Pierrot/backups/`, N=10, em todo
  save/autosave (`MainWindow::makeBackupCopy`).
- [ ] **Tela de "mídia faltando" + relink** — listar arquivos sumidos e
  re-apontar em lote.
- [x] **Teste real de render/export** — `tst_export_pipeline` roda o ffmpeg de
  verdade, extrai quadro e compara com `generatorFrame()`.
- [~] **CI GitHub Actions** — build Qt6 + `ctest`; falta AppImage e sanitizers
  (ver Fase 0).
- [ ] **Save/load assíncrono** — serialização fora da thread da UI.

### Gaps de feature

- **Alta:** proxy workflow, decode multi-thread por faixa, smart render, track
  motion, máscaras animadas.
- **Média:** LUTs 3D, transições por luma-mask, normalização + limiter, freeze
  frame, color tags, lock/hide de faixa, duplicar clipe, áudio scrub.
- **Baixa/cara (só depois):** multicâmera (sync por áudio/timecode), timeline
  aninhada, AAF/OMF, scripting Lua/Python, SDI/NDI, titler, macOS.

---

# Foco geral do Pierrot 2026 (plano vivo)

"Não perder trabalho" e "não travar" primeiro; features em segundo;
profissional/ecossistema em terceiro.

## 1. Confiança do código (QA, CI e ferramentas)

- [~] **CI GitHub Actions** — build Qt6 + `ctest`; falta asan/ubsan, AppImage,
  `-Wall -Wextra` e medir cobertura no CI.
- [ ] **`-Wall -Wextra`** no default; aspirar `-Werror` só no CI.
- [ ] **`.clang-format` + `.clang-tidy`** — rodar format-diff no CI sem
  religar o codebase de uma vez.
- [ ] **Cobertura do kernel `colombina` >50%** — gcov/lcov no CI; medido 15,0%
  em 2026-09-29. Buracos: `MesaRenderer` 0%, `ProxyManager` 0%,
  `FFmpegDecoder` 2,3%.
- [ ] **Fuzz do parser `.Blanc`** — corpus de JSONs malformados não pode
  crashar nem travar (limites de recursão/tamanho).
- [x] **Regressão de crash** — `tst_crashreporter` (fork + SIGSEGV real)
  preservou dois bugs que só aparecem com crash de verdade.
- [ ] **Benchmark no CI** — `Bench --bench/--stress` com limite de tempo por
  execução.

## 2. Robustez do projeto (`.Blanc` e mídia)

- [x] **Escrita atômica no save** — `QSaveFile` (temp + fsync + rename) já.
- [ ] **Versionamento de schema** — `format_version`; abrir versão mais nova
  avisa em vez de desserializar errado.
- [ ] **Migração `.ovp` → `.Blanc`** com backup do original antes de converter.
- [ ] **Recuperação por backup** — se o `.Blanc` não parsear, oferecer o backup
  rotativo mais recente.
- [ ] **Tela "mídia faltando" + relink**.
- [ ] **Limite de RAM em 4K** — auditoria dos buffers do
  `FrameWorker`/`MediaCache` (pendência do `OBSERVACOES.md`).

## 3. Crashes e recuperação

- [x] **Backup rotativo do `.Blanc`** — ver "Núcleo de foco" acima.
- [ ] **Auto-recovery pós-crash** — CrashReporter grava o caminho do `.Blanc`
  ativo; perguntar "restaurar sessão?" na próxima abertura.
- [ ] **Stack de threads secundárias** no relatório de crash.
- [ ] **Save de emergência** no handler — tentar `toJson` antes de abortar.
- [ ] **Watchdog de export** — filho do ffmpeg morreu/timeout → abortar limpo e
  avisar (nunca deixar `.tmp` como resultado).

## 4. Performance

- [x] **Save assíncrono** — snapshot na UI thread, gravação + backup num worker
  `QtConcurrent`; save em voo enfileirado; `closeEvent` espera o worker.
- [x] **Proxy workflow** — geração sob demanda no import (2K+ → H.264 leve),
  preferência `useProxies` serializada; export sempre usa o original.
- [ ] **Toggle proxy/original por faixa** — `Track::proxyPreview` + toggle no
  header (adiado, escolha de escopo).
- [x] **Decode multi-thread por faixa** — `warmTracks()` decodifica as faixas em
  paralelo e aquece o cache antes do paint.
- [x] **Thumbnail cache em disco** — `~/.cache/pierrot/thumbs`, hash por
  mtime+size, poda ~4096; reabrir projeto não regenera do zero.
- [ ] **Métrica de abertura** — template que mede tempo de abrir/salvar um
  projeto 4K sintético.

## 4.5. Backlog kernel (resumo — detalhe na seção "Kernel P0–P6")

- [ ] **Versionamento de schema** (ver 2).
- [ ] **Indexação O(1) por id** — `QHash` no lugar das varreduras lineares
  `findMedia`/`findMesa`/`findMesaForTrack`.
- [ ] **Load `.Blanc` assíncrono e defensivo** — `fromJson` na UI thread;
  juntar com o fuzz do parser.
- [ ] **GPU na composição** — `MesaRenderer` 100% CPU.
- [ ] **Smart render** — `stream copy` no export.
- [ ] **Sanitizers + `-Wall -Wextra` no CI** e cobertura >50%.

## 5. Feature gaps do Kdenlive

Complemento já priorizado na seção "Contra-check Kdenlive" acima. Nada disso
entra antes do bloco 1–4.

## 6. Documentação, lançamento e comunidade

- [ ] **CHANGELOG.md** + nota de versão em cada tag.
- [ ] **Template de issue** (bug report) — distro, Qt, ffmpeg, steps,
  `~/Pierrot-crash-*.txt`.
- [ ] **CONTRIBUTING.md** — build, testes, `--bench` e a regra preview↔export.
- [ ] **Guia de plugin OFX** — mínimo para terceiros.
- [ ] **AppImage na Release** com `UPDATE_INFORMATION`.
- [ ] **Critérios explícitos de v1.0 "estável"** — 1–4 verdes no CI, 0 crash
  conhecido sem fix, abertura 4K < meta, save nunca trunca.

## 7. Interface 0.7 — painéis, workspaces e densidade (2026-09-28)

O que já existe (bom, não redesenhar): 12 docks, layout persistido com
`kLayoutVersion` + `saneLayoutArray()`, abas em pilhas, menu Exibir, 89 tokens
de cor, `setDockLocked`.

- [ ] **Desempilhar Mixer / Histórico / Analisadores** — bug real: os três
  encadeiam numa pilha de 3 abas em ~100 px. Mixer e Analisadores em pilhas
  separadas, Histórico escondido.
- [ ] **Registry de painéis** — `PanelDef { id, title, factory, defaultArea,
  tabGroup }` + laço; adicionar painel vira 1 linha.
- [ ] **Workspaces** — `saveState()` já serializa tudo; workspace = `QByteArray`
  sob um nome + `QComboBox`. Presets: Padrão/Edição/Áudio/Composição/Efeitos.
  Cuidado: bump em `kLayoutVersion` ao mudar o layout padrão.
- [ ] **Agrupamento do menu Exibir** por região (`QActionGroup` + checkboxes).
- [ ] **Larguras iniciais das colunas** — `resizeDocks()` por workspace.
- [ ] **`setTabPosition` por área** — stacks verticais (padrão AE/Premiere).
- [x] **Densidade** — fontes/paddings justos no QSS.
- [x] **Escuro em 3 níveis** — `canvasBg` < `timelineBg` < painéis.
- [x] **Documentar a Mesa no FEATURES.md**.

### 7.1 Réplica estrutural do Premiere

| # | Passo | Risco | Status |
|---|-------|-------|--------|
| 1 | Preview sai do centro e vira dock | Alto | **Revertido** — preview continua `setCentralWidget` (decisão; destrava 2–5) |
| 2 | Source ao lado do Program | Médio | ✅ dock `SourceMonitorWidget` (In/Out, Insert/Overwrite) |
| 3 | Inspector vira dock | Baixo | ✅ `propsDock` |
| 4 | Header de faixa estilo Premiere | Baixo | ✅ (7.3) |
| 5 | Controles inline de áudio na timeline | Médio | ⚠️ M/S/R + VU no header; mixer segue dock |

### 7.2 Program Monitor (réplica)

✅ Transporte no monitor (◀▶ play/loop, `stepFrame`), cluster direito (margens,
resolução, zoom, fullscreen), rótulo `Program:`, timecode flutuante, barra de
transporte inferior removida, paleta de ferramentas vertical docável
(`toolsDock`).

### 7.3 Header de faixa (réplica)

✅ Refeito no layout Premiere CC: botões de controles no topo (seta, ◀◆▶, sync,
olho/falante, M/S/R), nome + cadeado abaixo, VU meter vertical no áudio,
renomear inline, geometria centralizada `header*Rect`, cores fiéis (#2B2B2B,
M/S em azul).

### 7.4 Corpo da faixa (áudio/vídeo)

✅ Fundo chapado sem zebrado, faixa de nome clara no topo do clipe (tint), onda
em tom claro, volume da faixa atrás de **Shift+V** (como Premiere), seleção
`#2A303E` nos três tipos de faixa.

---

## Pendências de bugfix / feature (2026-09-28, auditado em 2026-09-29)

### Bugs

- [x] **Faixa de gravação não pode ser removida** — corrigido: menu de contexto
  da faixa de gravação → "Excluir faixa de gravação" (`removeRecordingTrack`).
- [ ] **Ctrl+Z undo excessivo** — `pushUndo()` empilha snapshot do projeto sem
  coalescing; falta macro/timer que agregue entradas relacionadas.
- [ ] **Bug dock no hover** — diagnóstico: não é `restoreState`; é
  `QMainWindow::AnimatedDocks` + `AllowNestedDocks`. Decisão de produto
  (layout Premiere), não bug.
- [ ] **Fallback de áudio duplicado (Rust vs C++)** — DSP em duas
  implementações e o build padrão usa o C++ (`PIERROT_ENABLE_RUST` OFF).
  Proposta: extrair `AudioFxFallback` + `tst_audiofx_parity` no mesmo buffer.
- [ ] **Selecionador de área sobre o cabeçalho** — gatilho é o scroll
  horizontal: `xToTime()` sem clamp antes de `kHeaderW`. Correção de 1 linha
  (`.intersected()`).
- [x] **Save de janelas dockáveis** — layout persistido + validado
  (`saneLayoutArray`), autosave 500 ms, workspaces nomeados.

### Timeline

- [x] **Keyframes de áudio ocultos por padrão** — `m_showVolLines` false,
  `Shift+V` liga/desliga.
- [x] **M = marcador na timeline** — `M` adiciona marcador; comentário (futuro)
  será `Ctrl+Alt+M`.
- [ ] **Clip resize/move responsivo** — `m_clipPix` recria o pixmap a cada
  frame (`TimelinePaint.cpp:1000-1004`).
- [x] **Mover keyframes horizontal no editor de curvas** — funciona (falta a
  fluência do texto original).

### UI / Layout

- [ ] **Preview responsivo** — `PancropWidget` não tem `resizeEvent`
  (origem do PanCrop não se ajustar).
- [ ] **Barra de ferramentas com scroll** ⚠️ parcial — QToolBar vertical 46px;
  sem código de overflow próprio.
- [ ] **Tamanho mínimo do Media Pool** ⚠️ sem `setMinimumWidth`.
- [x] **Detector de rolagem automática** — edges 16/32 (literais mágicos a
  nomear).
- [x] **Mixer de áudio estilo Premiere** — fader vertical, VU + peak 1500 ms,
  pan rotativo, mute/solo, dB↔linear, strip master. ⚠️ Falta bus send/aux, pan
  law selecionável, leitura numérica de pan.
- [ ] **Sistema de volume** ⚠️ parcial — falta normalização por faixa/master,
  limiter/compressor/gate, leitura em dBFS, pan law selecionável.

### Features

- [ ] **Exportação de GIF transparente** — hoje a cadeia força saída opaca;
  falta caminho `colorkey`/`rgba` + `palettegen` com alpha.
- [ ] **PNG transparente otimizado** — sem cache/lazy decode de PNG com alpha.
- [x] **Auto Track** — criação automática de faixas ao soltar clipe em área
  vazia (`findFreeTrack`, 3 níveis).
- [ ] **Clip composto / Mesa** ⚠️ parcial — a Mesa existe, mas não é nested
  sequence: não há um "clipe Mesa" movível/cortável na timeline.
- [x] **Multicâmera** — ver Fase 2.
- [x] **Reverb melhorado** — 4 comb + 2 allpass. ⚠️ Sem IR, wet mono;
  duplicado Rust/C++.
- [ ] **EQ com gráfico editável** — 3 bandas de Q fixo ajustáveis só por número;
  sem curva de resposta. ⚠️ Duplicado Rust/C++.
- [ ] **Motion blur profissional** ⚠️ parcial — só na Mesa; clipes normais viram
  `boxblur` no export. Faltam shutter em graus e detecção de velocidade.

---

## Consolidação da pasta `Arquivos/` (auditoria 2026-09-29)

> Relatório de estado = leitura; fila de trabalho = tarefa. Detalhes e
> evidências `arquivo:linha` estão nos documentos.

### Kernel — `Plano-Atualizacao-Kernel.md` (P0–P6)

- [ ] **P0 — Versionamento de schema `.Blanc`** — `toJson`/`fromJson` não
  gravam versão; só migração ad-hoc (`mesaPosAbs`).
- [ ] **P1 — Indexação O(1) por id** — finders lineares; `findMesaForTrack` é
  O(n·m); sem `QHash` no modelo.
- [~] **P2 — Load `.Blanc` assíncrono e defensivo** — save é async, load é
  síncrono; parse defensivo ok, falta limite de tamanho/recursão e fuzz.
- [ ] **P3 — GPU na composição** — `MesaRenderer` 100% CPU (VAAPI/nvenc só no
  decode/encode).
- [ ] **P4 — Smart render** — cadeia sempre recodifica (`-map "[vout]"`,
  `-c:v`/`-c:a` explícitos).
- [~] **P5 — Sanitizers + `-Wall -Wextra` no CI** — CI existe, mas zero
  sanitizers e zero warnings flags no CMake.
- [ ] **P6 — Cobertura do kernel >50%** — zero infraestrutura de coverage.
- ⚠️ **`MesaRenderer` sem teste direto** (maior peça do kernel sem cobertura).

### Performance de playback — `Relatorio-Performance-Playback.md` (10 fixes)

- [x] **Fix 10 — throttle de `scopesFrame()`** — timer de 70 ms (já existia).
- [ ] **Fix 1 — `applyCrop()`/`applyBasicEffects()` no worker** — maior
  impacto/risco; falta o snapshot (pré-requisito).
- [ ] **Fix 2 — limitar `requestLowerLayers()` a 1/tick** — correção barata:
  um `break`.
- [ ] **Fix 3 — pré-alocar o buffer de compositing** — hoje alocado por paint;
  `ImgPool` já existe para chroma/blur/motionblur.
- [ ] **Fix 4 — `QPainter::setOpacity()` no lugar do COW manual** — avaliar
  contra blend modes antes de trocar.
- [ ] **Fix 5 — double-buffer no `AudioMixer`** — classe intestável (dentro do
  `.cpp`), lock segura o chunk inteiro.
- [ ] **Fix 6 — cache de mix sources** — separar topologia (invariante) dos
  valores por tick (`vol`/`mediaPos` variam).
- [ ] **Fix 7 — `desengasga` adaptativo** — intervalo literal fixo de 10 s.
- [ ] **Fix 8 — pré-criar/reusar `QAudioSink`** — new/destroy em todo play.
- [ ] **Fix 9 — remover `waitReadyBeforeSink()` da UI** — bloqueia ~300 ms com
  `sleep_for(2ms)`.
- [ ] **M2 — rebuild O(n) do índice do LRU** (`FFmpegDecoder`), ~6 µs/decode.

### Gaps Vegas/FCE — `Relatorio-Features-Vegas-FCE.md`, seção 4

- [ ] **Efeitos em nível de projeto/mídia** — só existe nível clipe/faixa.
- [ ] **"Open Format Timeline"** — fps/resolução só manuais.
- [ ] **Fluxo Premiere** — three-point completo, Expanded Edit/Trim, multicam,
  nesting, adjustment layers, master clips, Lumetri além de LGG, Essential
  Sound/ducking, MOGRT/captions/STT, Project Manager/auto-reframe/scene
  detection, VR/360 + color management ACES + GPU Mercury.
- [ ] **Expanded Edit Mode / Trim Start–End** — só existe `TrimmerDialog`
  (in/out de mídia); a indicatória de borda é feedback, não Expanded Edit.
- [~] **Effects packages / presets de cadeia** — melhor que o doc:
  `saveClipPreset` serializa o JSON do clipe inteiro (OFX + áudio). Falta nível
  faixa/projeto e pacote em arquivo.
- [ ] **Audio scrub / J-cut / L-cut / Voice Over** — inexistentes.
- [ ] **Multicam por matching de áudio + scene detection** — zero.
- [ ] **Speech-to-text / edição por texto** — zero (nem legendas existem).
- [ ] **Scrub no valor numérico** — nenhum spinbox customizado.

### Relatório de áudio — `Relatorio-Audio.md` ("nos conformes")

- [ ] **Ordem de invert/denoise difere preview↔export** — preview: invert
  antes do gate; export: `afftdn` antes do `aeval`. Baixa severidade, barato.
- ℹ️ Reverb/denoise/normalize não são bit-exatos entre os lados — validar
  loudness final no export.
- ✅ `kfSpeed` no áudio e DSP duplicado Rust/C++ já registrados acima.

---

## Critério geral

> Pior feature é a que **perde trabalho**; segunda pior é a que **trava a UI**.
> Toda decisão passa por "isso aumenta ou diminui a chance de perder o projeto
> do usuário?".

---

## Texto e fontes — plano After (2026-10-02)

Estado atual: `TextStyle` básico (fonte, size, bold, outline, x/y/align),
`TextEditorDialog` estático, desenho `QPainterPath` com wrap 90%, animação só
de clipe, fontes do sistema. `TextResource` unificado ✅.
Falta vs AE: tracking, leading, baseline shift, italic/weight, drop shadow,
preview ao vivo, Text Animator, fontes embutidas, estilo por caractere.

### Fase 0 — Character panel (0.8)

- [ ] Novos campos em `TextStyle`: `tracking`, `leading`, `baselineShift`,
  `italic`, `fontWeight`, `dropShadow*`.
- [ ] Serialização `.Blanc` + presets (`clipattrs`).
- [ ] Preview ao vivo no `TextEditorDialog`.
- [ ] UI Character (abas Texto/Aparência/Sombra).
- [ ] Draw nos dois lados via `QTextLayout` (tracking letter-by-letter).
- [ ] Export PNG aplica os novos campos (paridade).

**Aceite:** tracking/leading/italic/sombra visíveis no preview e export;
projetos antigos sem os campos = visual atual.

### Fase 1 — Text Animator (0.8/0.9)

- [ ] Modelo `TextAnimator` no clipe (typewriter/fadeWords/slideUp/slideLeft/
  fadeChars, duração, delay por unidade, unit char/word, reverse) +
  serialização.
- [ ] Preview e export com o **mesmo avaliador** (reaproveitar `renderTextImage`
  com tempo).
- [ ] UI "Animação de texto" no dialog + presets (Typewriter, Fade Words, Slide).

**Aceite:** typewriter e slide funcionam nos dois lados; keyframes de transform
continuam valendo por cima.

### Fase 2 — Fontes e polish (0.9)

- [ ] Fontes embutidas (TTF no qrc + fallback).
- [ ] Estilos por família (`QFontDatabase::styles`).
- [ ] Justify / word-wrap configurável.
- [ ] Estilo por palavra/seleção.
- [ ] Export sempre PNG do texto (unificar com `drawtext`).
- [ ] Docs FEATURES "Texto e fontes".

**Versionamento:** 0.8 = Fase 0+1; 0.9 = Fase 2.
**Fora de escopo:** editar no preview, MOGRT, STT, texto 3D/extrusão.

---

## Mesa 3D — Classic 3D (alvo AE 2010–2018)

Evolução da Mesa (não app separado); timeline Vegas não muda. Sem C4D,
ray-trace, PBR nem simulação — render CPU (QPainter + projeção).

- [x] **Fase 0 — Fundação 3D (0.8)** ✅ 2026-10-02 — `Math3D.h`, `tst_math3d`,
  campos `mesa3d`/`cam*`/`kfCam*`/`mesaZ`/rot, serialização `.Blanc`, flag lida
  no `MesaRenderer`. Aceite: projetos antigos = default 2D.
- [ ] **Fase 1 — Câmera e layers (0.8)** — projeção perspectiva (FOV, camZ),
  câmera POI (lookAt), transform 3D das camadas, depth sort por Z, UI sliders,
  GraphEditor (`GPropMesaz/CamZ/CamFov/CamPoi*`), preview=export. **Aceite:**
  parallax com Z, FOV animado, paridade, sem `mesa3d` = 2D.
- [ ] **Fase 2 — Luzes (0.9)** — parallel/point, UI "add light layer",
  `acceptsLights`, shading lambert em CPU, export mesmo renderer.
- [~] **Fase 3 — Malhas estáticas (0.9)** 🟡 MVP parcial — loader OBJ +
  `Track::meshPath` + desenho + menu ✅; falta glTF, texturas/materiais.
- [ ] **Fase 4 — Polish 3D (1.0)** — gizmos XYZ, pré-comp Mesa 3D, docs,
  benchmark, testes.

**Versionamento:** 0.8 = Fase 0+1; 0.9 = Fase 2+3; 1.0 = polish + docs.

---

## Cor — status (2026-10-02)

- [x] LGG clássico · Lumetri Basic (`colorgrade::applyToImage`) · Curvas RGB ·
  Vinheta nativa · LUT `.cube` (trilinear) · mix `cgBlend` · UI Lumetri ·
  Scopes (waveform/histograma/vectorscope).
- [ ] RGB parade no waveform · HSL Secondary · Color match · Keyframes de grade
  · Curvas desenháveis · Scopes embutidos no diálogo de cor.

## UI — status (2026-10-02)

- [x] Docks + workspaces nomeados · Header de faixa · Program Monitor · Source
  dock · Inspector dock · Tools docável · Exibir agrupado · Presets de
  workspace · Densidade · Escuro 3 níveis · Mesa no FEATURES.
- [ ] Source/Program no mesmo central (Program segue central; Source é dock) ·
  Registry `PanelDef` · Curvas desenháveis / scopes no diálogo de cor.

## Efeitos nativos — inventário (2026-10-02)

Detalhe: `Arquivos/Relatorios/Relatorio-Efeitos-Nativos.md`.

- [x] Brilho/contraste/sat, blur, grayscale, chroma key, masks, LGG+Lumetri,
  LAINKA, motion blur, EQ/Reverb/Denoise + hosts frei0r e OFX.
- [ ] Extrair `NativeFx.h` — chroma/blur/eq/grayscale/masks no kernel (paridade
  por construção; hoje inline em `PreviewWidget.cpp`).
- [ ] `tst_audiofx_parity` — Rust vs fallback C++ no mesmo buffer.
- [ ] Efeitos de look (Posterize, Find Edges, Tint…) — nativos novos ou
  catálogo frei0r curado.
- [ ] Warp Stabilizer / compressor por clipe — P2 (caros).

## Editor de Velocidade (dock) · 2026-10-02

- [x] Dock `velocityDock` (canvas velocidade×tempo, keyframes arrastáveis,
  base 0,1–4×) + menu do clipe "Editor de velocidade". Banda na timeline
  **removida em 2026-10-03** (atrapalhava o arraste); a edição segue no dock.
  Modelo/export prontos (`kfSpeed` + `clipSrcTime`).
- [ ] Diamantes de `kfSpeed` na lista de keyframes do GraphEditor
  (`GPropSpeed`).
- [ ] Áudio com envelope (usa `speed` base).
- [ ] LAINKA/OFX com envelope (usa `speed` base).

## Máquinas mais fracas (2026-10-03)

Anotado para depois (fora do caminho crítico; não entra no fechamento da 0.7).
Contexto: tester em máquina fraca relatou crash e vídeo ausente no viewport —
bug de latência, não de correção.

- [ ] **Ladder de degradação, não degrau** — prefetch → motion blur → largura de
  decode → por último software; cada degrau visível na UI.
- [ ] **Degradação nunca silenciosa** — indicador quando o preview cair para
  software.
- [ ] **Paridade preview↔export mantida na queda** — se o preview degrada, o
  export degrada junto.
- [ ] **Auto-detect por medição** — usar o `PreviewProfiler`, não chute.
- [ ] **Proxy como lever principal** — VAAPI é o maior ganho de CPU; não
  desligar primeiro.
- [ ] **Auditoria de decode síncrono na UI** — `PivotCanvas::ensureFrame()` abre
  decoder por chamada sem guarda `isVisible()`; unificar em `decodeUiFrame()`.
- [ ] **Build stamp no binário** — commit + data em `version.h` via CMake, no
  título/About/crash report (todo build mostra "0.7" hoje).