# Relatório — Features emblemáticas: Sony/MAGIX Vegas Pro e Apple Final Cut Express

Levantamento de recursos dos dois NLEs para referência do roadmap do Pierrot.
Compilado em 2026-09-22 a partir de documentação oficial, manuais e reviews.
A seção 1 (Vegas) destaca os recursos *marca registrada*; a seção 2 (FCE) é um
inventário funcional baseado na spec oficial (FCE 4) e no manual do FCE HD,
**podado de legado**: captura por fita/FireWire, autorização de DVD/Blu-ray,
saída QuickTime/AIC, LiveType e Soundtrack (apps Apple mortos) foram removidos
por irrelevantes para um NLE novo em 2026.

**Legenda de estado no Pierrot (auditoria do código em 2026-09-22):**
`✓` = já implementado · `◐` = parcial (com ressalva na linha) · (sem marca =
não tem).

---

## 1. Vegas Pro (Sony → MAGIX → Boris FX)

NLE orientado a **evento no tempo** com DNA de editor de áudio (nasceu como
Sound Forge 32-rack para vídeo). Linha ativa até hoje; versões citadas: 12
(Sony), 14–23 (MAGIX), 2026 (Boris FX).

### 1.1 Timeline e edição
- ✓ Edição por **arrastar-e-soltar**: cortar, dividir, mover, copiar, colar
  eventos direto no timeline, sem modos explícitos — `TimelineWidget`,
  `TimelineDrag`, `splitClipAt`.
- **Expanded Edit Mode**: "abre" a junção de dois clipes para ver os frames
  de entrada/saída lado a lado enquanto arrasta a emenda com precisão.
- ✓ **Ripple**, **slip/slide** (deslizar mídia dentro do evento sem mudar a
  posição), **roll/trim**, chaveades para cortar a borda sob o cursor
  (Trim Start–End) — ferramentas `ToolRipple/ToolRolling/ToolSlip/ToolSlide/
  ToolRateStretch` (`TimelineDrag.cpp:45`), `rippleDeleteInTrack`,
  `rippleTrimLeft/Right`, `resizeAdjacent`.
- **Takes** (alternar versões de mídia do mesmo evento) e **Switches**.
- **Grouping** e **Sync Links** (grupos de eventos/faixas que se mantêm
  alinhados ao mover).
- ◐ **Automatic crossfades** ao encostar dois clipes; **quantização a frames**,
  **snapping** e pós-edição **Event handles** na borda do evento — snapping ✓
  (`TimelineWidget::setSnap`, `snapToEdges`); crossfade automático = ✗
  (crossfade só com sobreposição).
- ✓ **Markers, Regions**: marcadores de cor (`TimelineWidget::toggleMarker`) e
  região de loop editável na régua; "command markers" nomeados ✗.
- **Multicam**: sincronização automática pelo áudio, troca de ângulo ao vivo.
- **Nesting** (projetos aninhados) e **Adjustment Tracks**.
- **Editor por texto (Speech to Text)**: transcrição local (Vegas 21+).

### 1.2 Áudio (DNA forte)
- ✓ **Mixagem multitrack** com **Mixing Console** (channel strips por faixa,
  barramento, panning, bussing) — `MixerWidget` (strips, knob de pan, RMS).
- **Gravação multitrack** simultânea (arm tracks, input busses, ASIO de baixa
  latência, punch-in/takes).
- ◐ **Bus de áudio e FX**: efeitos por **evento e faixa** ✓ (`AudioEffectsDialog`,
  `TrackAudioFxDialog`) e barramento por faixa + master ✓ (`PreviewWidget:835`);
  "assignable FX chains"/effects packages reutilizáveis ✗.
- ✓ **Automação** (envelopes) de volume, pan e parâmetros, **gravável em tempo
  real** — modes Touch/Write/Latch (`MixerWidget.h:90-93`, `writeAutoPoint`).
- **Precisão sample-based**, áudio 24-bit/192 kHz — o Pierrot trabalha em
  S16/48k estéreo (sample-accurate sim; 24-bit/192k ✗).
- **Sincronização multicam por vídeo/audio** ✗ (não há multicam).

### 1.3 Efeitos, composição e cor
- ◐ Cadeias de **muitos efeitos por efeito** aplicados em níveis — no Pierrot:
  **clipe e faixa** ✓ (áudio) e clipe/OFX (vídeo); nível **mídia e projeto** ✗.
- ◐ **OFX (OpenFX)** como arquitetura de plugin de vídeo — **host OFX ✓**
  (`src/colombina/ofx/`); "handles desenhados sobre o preview" e packages ✗.
- ◐ **Composição 2D/3D**: o Pierrot tem **Mesa** (compositor por camadas/
  câmeras com keyframes e modos de mistura) + **Track Motion** estilo própria
  ✓; 3D estereoscópico ✗.
- ◐ **Pan-Crop** — ✓ (PancropWidget com zoom/rotação/âncora e keyframes);
  **Shape Masking/FX Masking** ✓ (máscaras Bezier, `MaskEditorDialog`).
- ◐ **Color**: **Scopes** ✓ (Waveform/Histogram/Vectorscope, `ScopeWidget`) +
  brilho/contraste/saturação/gamma ✓; **Color Wheels, Color Match, LAB, ACES** ✗.
- ◐ **Transições**: dissolve/wipe ✓ (`TimelineWidget.cpp:1652-1660`); GL e
  envelope de progresso animável ✗.

### 1.4 Títulos e mídia gerada
- ✓ **Generator Media**: cor sólida, gradiente, checkerboard, ruído e texto
  (`generators.h`, `MesaWidget`, `MediaPoolWidget::addGenerator`); créditos
  rolantes a partir de .txt ✗.
- ◐ **Títulos**: texto editável ✓ (`TextEditorDialog`, `Clip::isText`, cor/fonte);
  títulos 2D/3D com extrusão/luz ✗.
- **Importação de PSD (Photoshop) em camadas** ✗.

### 1.5 Formatos, importação e render
- ◐ **Suporte amplo de formatos** — o Pierrot decodifica via FFmpeg (depende do
  build: RED/XDCAM/P2 dependem dos demuxers compilados).
- ✓ **Smart Proxy / proxy-first** — `ProxyManager` gera proxies via ffmpeg e o
  projeto usa original no render (`ProjectSettingsDialog::usesProxies`).
- ◐ **Interchange**: **EDL CMX3600 import/export ✓** (`NleInterchange.cpp`);
  **AAF ✗**, FCPXML ✗, **Scene/Edit Detection ✗**.
- **Scripting** ✗ (automação moderna hoje é via API/scripting de alto nível,
  não mais .NET/COM).

---

## 2. Apple Final Cut Express (HD 3.5 → 4) — descontinuado (2011)

"Fatia" do Final Cut Pro (baseado no FCP 6/HD), voltado a semiprofissional.
Compartilhava **linguagem de ferramentas/profissional** com o FCP e era elogiado
pela fidelidade ao fluxo de trabalho "de verdade". Genuíno "iMovie com aparência
de FCP". O inventário abaixo é o retrato funcional que ainda importa em 2026.

### 2.1 Interface e fluxo de trabalho
- Janelas **Browser, Viewer, Canvas e Timeline** — o Pierrot usa layout em dock
  próprio (MediaPool/Preview/Timeline/Mixer/Scopes), mesma separação de
  conceitos.
- **Interface mode-free** (sem modos explícitos de edição) — o Pierrot edita
  por arrasto direto no timeline, no mesmo espírito.
- ◐ **Overlays/wireframes no viewport** — o Pierrot tem gizmos no viewfinder
  (Pancrop, Mesa); "motion paths" wireframes ✗.
- **Button bars e layouts de janela configuráveis** ✗.
- ◐ **Jog/Shuttle (JKL)** para transporte — existe transporte/scrub de posição;
  JKL dedicado ✗.
- ✓ **Prévia full-screen / 2º display** — `PreviewMonitor` + F11 (equivalente
  ao Digital Cinema Desktop).
- ✓ **VU meters de áudio** — medidores RMS no MixerWidget/PreviewWidget.
- ✓ **Undo/redo** — snapshots em `m_undoStack` (`MainWindow.h:142-147`).

### 2.2 Importação e organização de mídia
- ◐ **Importação de imagens estáticas** (PSD, BMP, JPEG, PNG, TIFF etc.) — via
  FFmpeg: PNG/JPEG/BMP/TIFF ✓; PSD plano (sem camadas); PICT/SGI/TARGA
  dependem do build.
- ✓ **Bins/organização** — MediaPool em lista/ícones; **search & sort ✗**,
  **clip labels de cor ✗**, **campos de comentário ✗**.
- **Master/affiliate clips** (clones que herdam propriedades do mestre) ✗.
- *(Legado removido: captura de DV/HDV/AVCHD com device control FireWire,
  Capture window, Log and Transfer e import de iMovie '08 — tudo girava em
  torno de fita e apps mortos.)*

### 2.3 Editing e Timeline
- ✓ **Edição nondestructive** (referencia a mídia original sem alterá-la).
- ✓ **Drag-and-drop editing** e edição direta na Timeline.
- **Three-point editing model** ✗ (Pierrot é arrasto direto).
- **Editing palette** com edições de um clique ✗.
- ◐ **Insert e Overwrite com ou sem transição** — overwrite por arrasto ✓;
  insert-edit dedicado ✗.
- **Replace, Fit to Fill e Superimpose edits** ✗.
- ◐ **Blade / Blade All** — `razorSplitAt` ✓; "cortar todas as faixas" ✗;
  **Extend Edit ✗**.
- **Slug** (preencher vão com filler colorido) ✗.
- **Storyboard editing** (montar clipes antes da timeline) ✗.
- ✓ **99 vídeo + 99 áudio** — o Pierrot é multitrack (faixas ilimitadas).
- **Nesting de sequências** ✗; **múltiplas sequences/projetos abertos** ✗.
- ✓ **Track locking** — `trackLocked` (`TimelineCommands.cpp:17`).
- ✓ **Mute/Solo** — `MixerWidget` (botões M/S, `Project.h:550-551`).
- **Active track targeting (V1/A1/A2)** e **Auto Select** por faixa ✗.
- **Detecção/correção de sincronia** áudio/vídeo ✗.
- ✓ **Timecode na régua / zoom do timeline** + snap (`setSnap`);
  redimensionamento independente de faixa ◐.
- ✓ **Keyframe graphs** — `GraphEditorWidget`.
- ✓ **Markers** de clipe/sequência (`toggleMarker`).
- ✓ **Autosave** — autosave + snapshots JSON (`MainWindow`).

### 2.4 Trimming
- ✓ **Ripple (trim+delete), Roll, Slip, Slide** — `rippleDeleteInTrack`,
  `rippleTrimLeft/Right`, `resizeAdjacent` + ferramentas dedicadas.
- **Asymmetric, multitrack trimming** (trim de várias faixas num gesto) ✗.
- **Dynamic JKL trimming** ✗.
- **Trim Edit window** ✗ (trim na timeline; o `TrimmerDialog` é in/out da mídia).
- ✓ **Timeline trimming**.
- **Keyboard/numeric trimming** (digitar valores de trim) ✗.

### 2.5 Transições, Filtros e Efeitos
- ◐ **Mais de 200 transições/filtros/efeitos** — Pierrot: dissolve/wipe (8
  direções) + filtros básicos de vídeo + host OFX; bem abaixo de 200.
- ◐ **Transições** — dissolve ✓; ajuste de duração na timeline ◐;
  copiar/colar transição ✗.
- **Range tool** (aplicar filtro a uma range de clipes) ✗.
- ◐ **Plugins de terceiros (era FxPlug)** — o Pierrot tem **host OFX próprio** ✓.
- ◐ **Effects favorites** — favoritos de efeito ✗; **presets de clipe/faixa** ✓
  (`TimelineWidget.cpp:960-999`).
- ✓ **Copy/paste de atributos de motion/efeito** — `pasteAttributes`
  (`TimelineWidget.cpp:1016`).
- ✓ **Keyframe control + Bezier motion paths + ease in/out** —
  `GraphEditorWidget` (handles Bezier).
- ✓ **Controles numéricos de precisão** (spinboxes/fields) nos painéis.
- ◐ **RT Extreme (multistream realtime)** — o Pierrot mistura multiplaistas em
  tempo real ✓; modos Safe/Unlimited + quality ✗.
- ✓ **Dynamic RT** (qualidade ajustada em voo) — playback adaptativo sob carga
  (`PreviewWidget.cpp:2943-2967`).
- **Opacity overlays no timeline** (rubber band keyframável) ✗.
- **Auto Render** ✗ (render é feito na fila).
- **Filtro Broadcast-safe** ✗.
- **Títulos 2D/3D estilo Boris Calligraphy** ✗.
- ✓ **Composite/transfer modes** — blend modes na Timeline (`TimelineWidget.cpp:
  1722`) e Mesa.

### 2.6 Títulos e texto
- ◐ **Text generators**: standard, scrolling e lower-third — texto básico
  editável ✓ (`TextEditorDialog`); **crawl/scroll/lower-third ✗**.
- ✓ **Combinar títulos com efeitos/composição** — clipes de texto na Mesa.
- **Títulos 2D/3D** (extrusão/luz) ✗.
- ✓ **Unicode / caracteres não-latinos** — Qt: suporte nativo.
- *(Legado removido: LiveType 2.1 — aplicativo Apple descontinuado (LiveFonts,
  FontMaker, conteúdo/templates). Sua essência viva — texto animado com
  keyframes — já existe como conceito ✓ no `GraphEditorWidget`.)*

### 2.7 Compositing e Motion
- **Nesting** ✗ (o Pierrot empilha faixas na Mesa; sem sequences aninhadas).
- **Import PSD em camadas** (1 faixa por layer) ✗.
- ✓ **Bezier curves com motion paths editáveis** e **ease in/out** —
  `GraphEditorWidget`.
- ✓ **Motion via gizmo** — Pancrop (escala/rotação/âncora), Mesa (câmera/
  posição) e `MaskEditorDialog` no preview.
- ◐ **Slow motion com frame blending** — `RateStretch` (`rateStretch`) ✓;
  **frame blending ✗**.
- **Subpixel interpolation rendering** ✗.
- ✓ **Composite modes (multiply, lighten e cia.)** — `{normal,screen,multiply,
  overlay,…}` (`TimelineWidget.cpp:1722`, `PreviewWidget.cpp:1181`).
- **Flicker filter** (frames congelados de vídeo) ✗.

### 2.8 Color Correction
- **Secondary (two-way) color corrector** (wheels lift/gamma/gain + balanço de
  branco) ✗.
- ◐ **Image control filters** — brilho/contraste/saturação/gamma ✓
  (`ClipPropertiesWidget.cpp:539-546`) e scopes de vídeo ✓ (`ScopeWidget`).
- **Broadcast-safe filter** ✗.
- **Flesh tone correction** ✗; **color mixing** ✗; **footage matching** ✗;
  **correção de balanço/exposição** ✗.

### 2.9 Áudio
- ✓ **99 faixas de áudio** (ilimitadas no Pierrot).
- ✓ **Waveform na timeline** — áudio conformado + forma de onda desenhada.
- ◐ **Audio level overlays** — automação no `GraphEditorWidget`; overlay na
  timeline ✗.
- ◐ **Mais de 25 filtros de áudio** — Pierrot: EQ 3-band, denoise, normalize,
  reverb, AGC/gate (Rust `pierrot_audiofx`) — menos que 25, porém reais.
- ✓ **Soft Normalize e Gain** — equivalente **Normalize −14 LUFS** por clipe
  (`AudioEffectsDialog`).
- ✓ **Automação de fader em tempo real** (level/pan) — Touch/Write/Latch +
  `writeAutoPoint` (`MixerWidget.cpp:656-694`).
- ◐ **Filtros de áudio em tempo real** — DSP em tempo real ✓; **editar parâmetro
  durante playback ✗**.
- **Subframe audio keyframing (1/100 frame)** ✗.
- **Áudio de alta resolução (24-bit/96k)** ✗ (48kHz/16-bit estéreo).
- ✓ **Áudio level meters** (RMS; VU/loudness dedicado ◐).
- **Plugins de áudio do sistema (Audio Units)** ✗.
- ✓ **Pan controls** e barramento por faixa.
- ✓ **Mute e Solo**.
- **Logarithmic audio fades** ✗.
- **Audio scrub / pitch-shift scrub** ✗.
- **Voice Over tool** (gravar narração no timeline) ✗.
- **Linked clips com resync** ◐/✗.
- ✓ **Sem re-render ao reativar faixas muted** (mix em tempo real).

### 2.10 Output
- ◐ **Export universal (H.264/MP4/WebM etc.)** via FFmpeg — QuickTime/AIC
  específicos do macOS fora do escopo.
- **Export com markers** ✗.
- ✓ **Prévia full-screen em tempo real** — `PreviewMonitor`/F11.
- *(Legado removido: Print to Video em fita, iDVD/DVD Studio Pro, Export for
  Soundtrack.)*

### 2.11 Limites (o que não existia no Express)
- **Sem multicam** ✗ (o Pierrot também não tem).
- **Sem editor de forma de onda / noise reduction dedicado** — o Pierrot tem
  denoise por clipe ✓.
- **Sem batch encoding** — o Pierrot supera com **fila de render** ✓
  (`RenderQueueDialog`).

---

## 3. Tabela-resumo

| Recurso | Vegas Pro | Final Cut Express | No Pierrot |
|---|---|---|---|
| Edição base | Evento, drag-and-drop | Three-point, FCP tools | ✓ drag-and-drop |
| Trimming | Expanded Edit Mode, ripple, slip/slide | Trim Window, ripple/roll, L/J-cut | ✓ ripple/roll/slip/slide/rate |
| Faixas | Multitrack | 99 vídeo + 99 áudio | ✓ multitrack |
| Multicam | Sim (sync por áudio) | Não | ✗ |
| Áudio | Mixing Console, gravação multitrack, ASIO | Envelopes, Voice Over, audio scrub | ◐ mixer/envelopes; graft ✗ |
| Plugins | OFX + VST/DirectX | FxPlug | ◐ host OFX |
| Efeitos por escala | Evento/faixa/mídia/projeto | Clipe/faixa/range | ◐ clipe+faixa |
| Composição | Track Motion 2D/3D, masks, nesting | Bezier paths, PiP, nesting, PSD | ◐ Mesa+masks; nesting/PSD ✗ |
| Cor | Color Wheels/Match, LAB, S-Log/ACES, scopes | Color wheels estilo FCP | ◐ scopes+basics; wheels ✗ |
| Títulos | Generator + Titles & Text (2D/3D) | Text generators | ◐ texto/generators; 2D/3D ✗ |
| Formato ativo | Proxy-first, AAF, broadcast | Open Format Timeline | ◐ proxy ✓, EDL ✓, AAF ✗ |
| Automação | Envelopes + scripting | Keyframes com Pen no timeline | ✓ envelopes graváveis + graph |
| Render | Broadcast/Blu-ray | Web (H.264) | ✓ fila de render (FFmpeg) |
| Extras | Scripting, scene detection, STT | Interop via XML | ✗ no geral |

---

## 4. Pontos que o Pierrot pode roubar

1. **Nível de aplicação de efeito por escala** (evento → faixa → clipe →
   projeto). *Estado: ◐ — tem clipe+faixa; falta nível mídia/projeto.*
2. **"Open Format Timeline"** — projeto único que aceita qualquer
   resolução/fps e padroniza o primeiro clipe. *Estado: ✗.*
3. **Expanded Edit Mode / Trim Start–End** — precisão de emenda. *Estado: ✗.*
4. **Envelopes de efeito graváveis em tempo real** e **effects packages**
   (salvar cadeia como preset). *Estado: ✓ já grava envelopes; packages ✗.*
5. **Audio scrub / L-cut/J-cut** explícitos e **Voice Over tool**.
   *Estado: ✗.*
6. **Sincronização multicam por matching de áudio** e **scene detection**.
   *Estado: ✗.*
7. **Speech-to-text / text-based editing local**. *Estado: ✗.*
8. **Scrub no valor numérico** (arrastar número para ajustar).
   *Estado: ✗.*

---

## Fontes
- MAGIX/VEGAS — página de features e o que há de novo (vegascreativesoftware.com).
- Manual completo Vegas Pro 14 (Steam/PDF, índice de recursos acima).
- Sony Reviewer's Guide — Vegas Pro 12 (pro.sony.com PDF).
- Apple Support — "Final Cut Express 4 – Technical Specifications"
  (support.apple.com/en-us/112645) — base do inventário da seção 2.
- Apple Newsroom — "Apple Releases Final Cut Express 4" (2007) e
  "Apple Releases Final Cut Express HD 3.5" (2006).
- Videomaker — review Final Cut Express 4 (2008) + ficha técnica.
- Apple — manuais do Final Cut Express HD (Getting Started, manual completo,
  Quick Reference).
- Auditoria local: busca em `src/` do Pierrot (2026-09-22) — referências
  `arquivo:linha` inline.