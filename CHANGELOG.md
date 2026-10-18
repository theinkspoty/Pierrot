# Changelog

## Unreleased

### Tema Japan 95

- **Novo tema claro com vermelho** (Ajustes ▸ Interface ▸ Tema ▸ Japan 95):
  papel off-white, tinta preta e vermelho hinomaru (#C8102E) para tudo que é
  ativo — seleção, playhead, foco de input, botão primário, tab ativa,
  ferramenta marcada, curvas do editor de keyframes. Clipes em papel-manilha
  sobre faixas brancas com borda de seleção vermelha; monitor continua escuro
  (vídeo pede fundo neutro). Referência: gráfica japonesa dos anos 90.
- **Tipografia japonesa no tema Japan 95**: app inteiro usa a pilha de
  gothics japoneses (Noto Sans CJK JP → IPAGothic → TakaoGothic → Hiragino →
  Yu Gothic → Meiryo; sem nenhuma instalada, cai no sans padrão). Menu,
  abas, títulos de dock e de grupo em negrito.

### Tema por paleta (.pierrot-theme)

- **Tema completo de ponta a ponta** (Ajustes ▸ Interface ▸ Tema por paleta):
  um arquivo `.pierrot-theme` define as **cores semânticas** de `ThemeColors`
  (`chave = #RRGGBB`, ~96 linhas). Como **toda** a UI lê dessa paleta —
  inclusive o que é **desenhado por código** (timeline, playhead "agulha",
  monitor, gráficos, docks, botões) — este formato cobre o app inteiro.
  Chaves ausentes herdam do tema embutido escolhido em "Tema:". Botões
  **Importar…**, **Exportar tema atual…**, **Remover** e **Abrir pasta**.
  Requer reiniciar o app.
- **Implementação**: `serializeThemeColors()`/`applyThemeColorText()` e o
  override em `paletteFor()` (`Theme.cpp`) — o arquivo é lido uma vez e
  mesclado sobre a paleta embutida; `flatControlStyleSheet`,
  `globalStyleSheet`, `dialogStyleSheet`, o `QPalette` e os `paintEvent`
  seguem essa paleta.
- **Cabeçalho das faixas passou a seguir o tema**: fundo (`trackLabelBg`),
  texto (`trackLabelText`), separadores (`trackBorder`), chips M/S/R, glifos,
  cadeado, olho/falante, meter e alça de resize deixaram de ser cores fixas
  estilo Premiere e agora leem `themeColors()` — vale também para os temas
  embutidos (antes o cabeçalho ficava sempre escuro).

### Idioma japonês

- **Suporte completo a 日本語** (Ajustes ▸ Interface ▸ Idioma ▸ 日本語): menus,
  diálogos, chips, atalhos e mensagens traduzidos (1174 entradas; `%1`…
  preservados). Requer reiniciar o app. O japonês também troca a fonte da
  interface pela pilha de gothics japoneses, em qualquer tema.
- **Tradutor embutido em C++** (`src/ui/core/JaTranslator`): tabela `QHash` estática
  em `QStringLiteral`, instalada no `main` só quando o idioma salvo é `ja`.
  Não usa `lupdate`/`lrelease`/`.qm` — o build segue sem depender de qttools.
  Texto sem tradução cai no original em pt-BR; funciona também em `tr(variável)`
  (tabelas de atalhos, presets etc.), porque a busca é em tempo de execução.
- **Fonte das traduções versionada** em `tools/i18n/`: `strings.txt` (lista
  extraída do código) e `ja.part1.txt` (formato `origem|||日本語`).
  `python3 tools/i18n/gen_ja.py` regenera o bloco entre os marcadores
  `BEGIN-GENERATED`/`END-GENERATED` do `.cpp`.
- Item **"Inglês (v0.7.5)"** já reservado no seletor (por ora cai no pt-BR).

### Presets de animação

- **48 presets em 6 grupos** (antes 19 em 5): entradas ganham **Bounce de
  Cima** (queda com quique + squash no impacto), **Girar Entrando**, **Cortina
  Vertical**, **Slide Baixo** e **Fade Cinemático**; saídas completam o
  conjunto (**Sair p/ Esquerda**, **Descendo**, **Encolhendo**, **Girando**,
  **Saída em Fade**); novo grupo **Câmera (Ken Burns)** — Push In, Pull Out,
  Pan Direita/Esquerda, Subida Suave e Dr. Holandês (movimento lento na
  duração inteira); idle ganha **Pêndulo**, **Órbita**, **Pulso (latida)**,
  **Deriva Lenta** e **Tremida Leve**; efeitos ganham **Giro 180**,
  **Impacto**, **Flip Horizontal**, **Zoom por Cortes** (Degrau estilo
  vlog/TikTok) e **Stroboscópio**; elásticos ganham **Salto Elástico**
  (antecipação + voo + aterrissagem), **Recoil** (recuo de arma/câmera) e
  **Estalo** (snap elástico).
- **Preset clicado não aplicava quando tinha acento**: `applyPreset`
  comparava `QString == QLatin1String` (bytes UTF-8 lidos como Latin-1) —
  "Pop Elástico", "Flutuação", "Stroboscópio"... nunca casavam e o clique era
  silenciosamente ignorado. Agora `QString::fromUtf8` nos dois lados.
- **Presets de laço perdiam a interpolação do 1º ciclo**: `upsertKeyframe`
  no mesmo instante só troca o valor e preserva a interp do primeiro — o
  seed Linear de t=0 engolia a interp real. **Wiggle** começava retinho,
  **Flicker** fazia fade em vez de corte seco, **Respiração** perdia o
  quique. `setKf` agora substitui a key inteira (seed → key de laço).
- **"Giro 360" saía reto**: a interp Suave estava na última key (inerte —
  a interp do segmento mora na1ª); corrigido.

### Program Monitor

- **Faixa de scrub arrastável** sob o vídeo (novo widget filho): playhead
  arrastável para navegar na sequência direto no monitor, região de loop
  sombreada e **picotes dos marcadores nas suas cores** (mesma paleta do
  marcador da timeline).
- **Timecode com duração**: `00:00:05:12 / 00:01:23:00` (posição / tamanho
  da sequência) — antes só a posição, sem referência do total.
- **Chip de qualidade** (`720p`, `4K`…) no canto superior direito do vídeo:
  lembra quando a imagem não está na resolução cheia.
- **Preferências persistentes**: zoom, qualidade de preview e margens de
  segurança são gravadas no `QSettings` e reabertas no próximo start.
- **Tela vazia caprichada**: ícone de play grande + dica de como começar
  (antes, só "Sem clipe de vídeo aqui" em cinza).
- **Sombra suave** atrás do quadro no letterbox (3 anéis translúcidos,
  custo ~zero) — separa o "programa" do fundo do console.

- **Margens de segurança não somiam mais em projetos com 2+ camadas**: a
  grade era desenhada no fim do `renderFrame`, mas os caminhos do composto
  (cache hit e composição multi-faixa) retornam antes — ela só aparecia no
  caminho de 1 clipe. Agora é um overlay do `paintEvent`, como os badges de
  playback.

### Marcadores

- **Nome automático**: todo marcador novo ganha "Marcador N" (menor N livre)
  — antes o nome vazio era só uma bandeirinha sem legenda.
- **Balão colorido com o nome**, desenhado **dentro da régua** (clipado): o
  texto ia colado na borda e a linha quase encostava na faixa — dava a
  impressão de a cor do marcador vazar para a pista.
- **4 cores escolhíveis** (vermelho, azul, amarelo default, roxo):
  ao criar (submenu "Adicionar marcador" na régua, com amostra de cor) ou a
  qualquer momento (clique-direito **sobre o marcador**: renomear…, cor,
  excluir). A cor já era serializada no `.Blanc` — paleta nova não quebra
  projetos antigos.

### Sincronização AV na edição (ripple)

- **Ripple delete sincronizado** (`TimelineCommands`): o fechamento do vão
  era **por faixa** — só puxava as clipes seguintes nas faixas onde algo foi
  removido. Excluir um clipe **sem áudio** deixava a faixa de áudio sem
  remoção nenhuma e os pares de áudio dos clipes seguintes ficavam para trás
  (o vídeo era puxado, o som dessincronizava). Os intervalos excluídos agora
  são a **união global** (vídeo+áudio) e o deslocamento vale para **todas** as
  faixas (sync lock estilo Premiere). Afeta `Excluir` (Delete com ripple) e
  `Cortar e Excluir` (Ctrl+K com ripple).
- **Ripple Edit (ferramenta B) com sync**: `rippleTrimLeft/Right` também
  mexia só na faixa do clipe. Agora ajustam o **par vinculado** (grupo
  vídeo+áudio, mesmas operações em `pos/in/dur`) e deslocam os subsequentes
  em **todas** as faixas. O snapshot do arraste ao vivo (`m_rippleOrig`) foi
  alargado para as todas as faixas, mantendo o rollback por mousemove.

### Performance de reprodução (freeze no play)

- **Causa raiz identificada**: a primeira criação do `QAudioSink` inicializava
  o backend de áudio do Qt (~1 s) dentro do `startAudio`, travando a UI por
  ~1,2 s no 1º play (evidência: `qt.multimedia.ffmpeg` + primeiro tick com
  `skip=37`).
- **`prewarmAudioSink()`** — cria o sink no startup (`QTimer::singleShot(0)`)
  e revalida no warm do playhead; o play só reusa, sem recriar.
- **Aquecimento do playhead parado** (`scheduleIdleWarm`/`warmPlayhead`,
  debounce 180 ms): ao parar a agulha (ou abrir o projeto) abre+posiciona o
  decoder do clipe sob ela (evita o `open()` frio de 500–800 ms) e antecipa a
  conform de áudio do trecho (evita o "começa mudo").
- **Instrumentação** com `PIERROT_PLAY_DEBUG=1`: `[play] 1º quadro Nms após o
  play` e `[play] startAudio: updateSources=Xms sink=Yms`.

### Estado do transporte do preview

- **`m_playing` começava `true`** (`PlaybackEngine.h`): o timer estava parado
  mas o motor acreditava estar tocando, então o 1º Espaço caía no ramo de
  parar (sincronizava o estado sem dar play — o 2º é que reproduzia), o J/L
  primeiro dobrava a taxa sem tocar nada, os passos ◀▶ disparavam um "pausar"
  fantasma e o monitor externo abria mostrando estado de play. Agora começa
  `false`; todos os caminhos de partida (`togglePlay`/`shuttle`/`playFrom`)
  setam `true` explicitamente.
- **`setProject` para o transporte em qualquer troca** (antes só com
  `p == nullptr`, que nunca acontecia): engasga útil se o projeto for
  recarregado com o play rodando.
- **Abrir/criar projeto não mexia no preview**: `PreviewWidget::setProject`
  só era chamado uma vez (construção da janela, `MainWindow.cpp:221`), então
  `Arquivo → Novo/Abrir` mantinha play, agulha e caches do projeto anterior —
  agulha parada além da duração nova resultava em "Sem clipe" e cache velho.
  `newProject()` e `openProjectFile()` agora chamam
  `m_preview->setProject(&m_project)` + `m_timeline->setPlayhead(0.0)`.
- **Rótulo "Program: <nome>"** atualizado também no `refreshView()` (antes só
  no `setProject`, que roda uma vez): `createProject()` define
  `m_project.name` depois dele e o monitor ficava "Sem projeto".

## 0.7.1 (2026-10-17)

Release de animação + correções de curvas. Naipe continua **alpha**.

### Animação

- **Dock "Animação"** com presets de keyframes e **miniaturas animadas
  estilo Vegas Pro**: fileiras de 3 colunas sem espremer; a miniatura só
  reproduz com o mouse em cima (repouso = quadro estático representativo).
- Novo grupo **Elásticos**: **Slam Elástico** (queda + squash antifase),
  **Estica e Some** (anticipation + fuga elástica) e **Mola (laço)**
  (bob + twist antifase em loop).

### Curvas (editor de keyframes)

- **KfSmooth (Auto Bezier) corrigido**: controles em `span/3` (a fórmula
  antiga deixava a tangente 3× íngreme — S exagerado que passava dos
  keyframes); par isolado de 2 keys ganha S suave (ease). Desenho do editor,
  animação e exportação usam a mesma fórmula.
- **Varinha só em keys Suave** — Linear/Degrau ficam retos (antes, alças
  "fantasma" convertiam a reta em S ao serem tocadas).

### Correções

- **Texto da aba selecionada** era branco fixo (`#FFFFFF`) — invisível no
  tema Light (aba cinza-clara) e no Japan 95 (aba papel). Agora usa cor do
  tema (`tabSelectedText`).
- **Loop de GIF/WebP animado** na timeline: duração real na inserção;
  exportação com `-ignore_loop 0` respeitando o loop do arquivo.
- Correções do relatório de bugs (B-01…B-17) e versionamento `.Blanc`
  com migração (`kSchemaVersion`).

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
