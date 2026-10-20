# Changelog

## 0.8 (2026-10-20)

Release de **fechamento da transição Laartman v0.4** — o motor de codecs agora
só decodifica (núcleo congelado L0–L3; pool/prefetch/posse são política L4 do
Pierrot) —, suporte a japonês, tema Japan 95, 48 presets de animação e correções
de desempenho/robustez. Naipe continua **alpha**.

### Decode — fechamento da transição v0.4 (Fases 3+4)

- **`PacketReader` integrado ao caminho de vídeo (L0)** — o decoder lê os
  pacotes de vídeo via `PacketReader` (thread de I/O à frente, buffer de 32,
  **contexto de demux próprio**) em vez do `av_read_frame` síncrono do demuxer
  compartilhado. Se o disco engasga, o decode continua até esvaziar o buffer;
  o `cancel` cooperativo chega ao `read()` do reader e aborta a espera em
  ~20 ms (antes o decode travava esperando o disco). Imagens (single-frame)
  ficam no demuxer síncrono (um pacote não ganha nada com thread; image2 é
  frágil). Se a abertura do reader falhar, o caminho de vídeo cai no demuxer
  compartilhado (comportamento antigo) — nunca bloqueia o `open()`.
- **Vídeo e áudio em paralelo na mesma instância (Fase 3)** — com o vídeo no
  contexto próprio do `PacketReader`, `decodeAudio()`/`seekAudio()` deixam de
  pegar `m_mutex` no caso comum: só `m_audioMutex` os separa e os dois caminhos
  decodificam sem esperar um ao outro (a cadeia `m_mutex → m_audioMutex` só
  permanece quando o vídeo COMPARTILHA o demuxer — imagens/fallback —, via
  `lockAudio()` com revalidação para não correr com `frameAt`). Novo teste
  `videoAndAudioDecodeConcurrently` (mesma instância, duas threads, seek+PCM)
  trava o CI se houver deadlock.
- `PacketReader.cpp` entrou nas fontes de todos os testes que compilam o
  motor diretamente (CMakeLists).

### Estrutura — camadas por estabilidade em `src/laartman/`

- **Subpastas por camada** (ver `src/laartman/README.md`): a raiz fica só a
  fachada pública `Decoder.h/.cpp` (L3 — o que mexeremos muito); o motor de
  decode congelado foi para `core/` (`FFmpegDecoder*`, `ErrorConceal`); demux
  e leitura foram para `io/` (`Demuxer`, `PacketReader` — camada mais funda).
  Consumidores incluem `laartman/core/…` hoje; o alvo é todos usarem só a
  fachada da raiz (migração de call sites pendente).

### Probe (§8.6)

- **Memo de `probe()` por `(caminho canônico, mtime, tamanho)`** — o probe
  abre o container e chama `find_stream_info` (caro) e a MESMA mídia era
  sondada repetidamente (media pool, proxy, arrasto na timeline, export).
  Agora o resultado fica em cache no processo; a chave inclui mtime+tamanho,
  então editar/substituir o arquivo invalida sozinho. Limite de 512 entradas
  (o working set de um projeto é pequeno). Arquivo inexistente/sem permissão
  não é memoizado (não fixa resultado vazio).

### Preview / lookahead (pós-Fase 8)
- **Warm-ahead em thread própria (L4)** — o `FramePipeline` agora tem DUAS
  threads: `preview-prefetch` (o frame0 do próximo corte, crítico) e
  `preview-warm` (os clipes N+2/N+3, especulativos). Antes os dois slots
  dividiam a mesma thread: um `open()` frio de ~1,3 s no warm atrasava o
  prefetch crítico do corte e o worker acabava pagando o open na thread do
  decode (o stall de 1,3 s da sessão de referência). Agora o warm não bloqueia
  o caminho crítico.
- **Janela de lookahead maior** — `updatePrefetch` dispara o prefetch/warm de
  até 12 s antes do corte (era ≤5 s): o `open()` frio (~1,3-1,5 s medido, mais
  em disco lento) tem folga para terminar ANTES de o corte chegar.

### Decode (F1 do plano Laartman v3)

- **Fachada pública `Decoder` (L3, Fase 0)** — criado `src/laartman/Decoder.h`
  como a superfície congelada do núcleo: `probe/open/close/frame/seekAudio/
  readPcm/audioFinished/fps`, com o resultado de sondagem em `MediaInfo` e as
  decisões de política injetadas via `DecodeOptions` (`audioStream`,
  `hardwareAllowed`). **Sem** `acquire/release/busy/pool/setHardwareDecodeAllowed/
  releaseBuffers` — nenhuma noção de posse no header público. Hoje é uma
  fachada fina sobre o `FFmpegDecoder` (não muda comportamento); as fases 2–6
  passam a consumir só esta API. `tst_decoder_api` cobre a paridade
  fachada↔motor e falha se algum token de posse/política reaparecer no header
  (invariante 2: "o CI quebra se alguém reintroduzir").
- **GIF/WebP/imagens não tentam mais VAAPI** — decodificar um GIF animado com o
  `hw_device_ctx` VAAPI armado estourava SIGSEGV dentro do `libavcodec`
  (`avcodec_send_packet`). Codecs paletados/single-frame não são decodificáveis
  por VAAPI e não ganham nada com GPU; agora `isImagePath()` (png/jpg/gif/webp/
  tif/svg…) força software no `open()` — mesma classe da issue #3.
- **Pool global de decoders** — agora em `src/colombina/ffmpeg/SourcePool`
  (política L4), **sem lease exclusivo nem flag `busy`**: todo acesso é
  compartilhado e o `FFmpegDecoder` serializa internamente. `acquire()` NUNCA
  devolve `nullptr` por "ocupado" — só se o `open()` falhar. Elimina por
  construção a classe de falha "sem slot/ocupado" (quadro vazio por bloqueio):
  na sessão de estresse de ~150 s eram **754 falhas de pool** espalhadas por
  15 arquivos. Budget único (`PIERROT_DECODER_POOL`, padrão 10) + eviction LRU
  de entradas ociosas (`use_count==1`); sai o mapa **ilimitado** de decoders do
  `MesaRenderer` (risco de RAM em projeto com muitas fontes 4K). O antigo
  `laartman/DecoderPool` (lease + `busy`) foi removido; `MesaRenderer`,
  `FrameWorker`, `FramePipeline` e `PreviewWidget` migrados para o novo pool.
- **Pipeline de prefetch extraído para L4 (`FramePipeline`, Fase 2)** — a
  política de lookahead saiu do `PreviewWidget` para
  `src/colombina/ffmpeg/FramePipeline`: é ele o dono da thread de background
  (`preview-prefetch`), da fila de aquecimento à frente e do estado busy/in-flight.
  A UI (L6) só resolve **quais** caminhos/tempos pedir (`prefetchFrame`,
  `setWarmQueue`, `warmNow`) e consome os sinais `framePrefetched`/`prefetchFailed`/
  `warmed`; nenhuma noção de thread, fila ou ocupado sobra no widget. O antigo
  `ui/monitor/BgPrefetchWorker` foi dobrado no pipeline (worker privado no `.cpp`).
  `tst_framepipeline` cobre a fila (substituída, não acumulada), o prefetch de
  frame0 (serial, devolve o par `t`/`t+step`) e o warm (aquece o pool, limpa o
  in-flight).
- **Demuxer extraído para L0 (`Demuxer`, Fase 3)** — criada
  `src/laartman/Demuxer.{h,cpp}` com responsabilidade única de abrir arquivos,
  descobrir streams, ler pacotes e fazer seek. Separa vídeo e áudio e centraliza
  a lógica de seleção de streams (`findBestVideoStream`/`findBestAudioStream`/
  `resolveAudioStreamIndex`). O `FFmpegDecoder` agora faz **todo** o demuxing
  (open/`read_packet`/seek, vídeo e áudio) através do `Demuxer`, que é o dono do
  `AVFormatContext` (`m_ctx` aponta para `m_demuxer->formatContext()`); o header
  público segue sem vazar os tipos do FFmpeg (`Demuxer` é `unique_ptr` com
  forward-declaration). `audioPeaks()`/`probe()` mantêm o open próprio de tiro
  único. Integrado ao build do núcleo (LAARTMAN_SOURCES).
- **`PacketReader` (L0) — leitura de pacotes à frente** — criado
  `src/laartman/PacketReader.{h,cpp}`: thread dedicada que lê pacotes do arquivo
  e os enfileira num buffer limitado (teto padrão 32, configurável), separando
  I/O de disco do decode. `read()` bloqueante com cancelamento cooperativo;
  `seek()` para a produção, descarta o buffer e reposiciona; `close()`
  idempotente. Componente standalone (dono do próprio `AVFormatContext`), pronto
  para substituir a leitura síncrona do decoder. `tst_packetreader` cobre o teto
  de memória, EOF, seek e cancel.
- **VFR + correção de timestamp (Fase 4)** — o decoder detecta **taxa de quadros
  variável** (`avg_frame_rate` ausente ou bem abaixo do `r_frame_rate`) e passa a
  usar o timestamp real de cada quadro na matemática de tempo: a chave do cache
  de quadros vira uma grade de milissegundos (`frameCacheIndex`) em vez do
  número do quadro por fps médio, eliminando o drift/colisão em arquivos VFR.
  `frameSec()` prefere `pts`, cai em `best_effort_timestamp` (que já considera
  DTS/B-frames) e firma timestamps negativos em 0. `tst_decoder_api` ganhou
  `vfrDecodeIsDeterministic` (VFR gerado por concat 30 fps + 10 fps).
- **`ErrorConceal` (L1) — ocultação de erro pelo último quadro bom** — criado
  `src/laartman/ErrorConceal.h`: classifica o resultado de decode e decide quando
  entregar o último quadro válido (`FrameStatus::Concealed`) em vez de "imagem
  vazia" ambígua, distinguindo `Ok/Pending/Eof/Error/Cancelled` (invariante 3,
  "falha é tipo, não silêncio"). Puro/testável. `tst_errorconceal` fixa a tabela
  de decisão com `AVFrame` sintéticos.
- **FrameCache extraído do núcleo para L4 (`FrameCache`, Fase 6)** — o LRU de
  quadros (120 entradas) **saiu do `FFmpegDecoder`** e virou
  `src/colombina/ffmpeg/FrameCache.{h,cpp}`: o motor não promete mais cache, isso
  é política de L4 (o núcleo só expõe a cadência via `FFmpegDecoder::frameCacheIndex`).
  Chave `(bucket de tempo, maxWidth)`, move-to-front O(1) por `splice`, evicção
  LRU, `getOrCompute()` (acerta ou computa+guarda) e `instance()` compartilhada
  (como `SourcePool`). O overlay passou a ler os contadores do `FrameCache`. Os
  caminhos quentes de L4 (`FrameWorker`, `FramePipeline`) consultam o cache antes
  de decodificar; `frameAt()` sempre decodifica. Sem dependência de FFmpeg (só
  Qt). `tst_framecache` cobre miss/hit, move-to-front, evicção, isolamento entre
  chaves, descarte de imagem nula e `getOrCompute`.
- **`FFmpegEncoder` saiu do núcleo (Fase 7)** — o motor de encode in-process
  deixou `src/laartman/` (que fica só com decodificação) e foi para
  `src/colombina/ffmpeg/FFmpegEncoder.{h,cpp}`. Nada no app o usava (o export
  continua no CLI via `ProjectExporter`); só o `tst_encoder`, que agora linka a
  `colombina`. O núcleo Laartman encolhe e fica simétrico: só decode.
- **Aceitação da reestruturação (Fase 8a) medida, 0 drops** — sessão de
  referência de 150 s (`teste de potencia.Blanc`) com `PIERROT_PERF_JSON`:
  **`droppedTotal: 0`, `skippedTotal: 0`**, 8.791 quadros, `tickRateHz: 58.82`,
  0 falhas de pool (classe eliminada por construção na Fase 1). Corrigido no
  caminho um regressão da Fase 6: `FrameWorker::decodePrefetch` chamava
  `SourcePool::acquire()` sempre, o que abria o arquivo de forma síncrona na
  thread da camada de baixo quando ela compartilhava o arquivo do topo — cold
  open de até 2 s (`stallMaxMs` 2045 ms → **18 ms**). Agora reusa `m_decoder`
  quando `path == m_curPath`. O stall residual (~1,3 s, 2 eventos em 149 s) é o
  cold open no corte, alvo do `PacketReader`/L3 — não é regressão.
- **Build sob sanitizers (Fase 8b)** — nova opção CMake `PIERROT_SANITIZE`
  (padrão OFF) liga ASan+UBSan em compile e link (`-fno-sanitize-recover=all`
  faz o UBSan abortar, para o `ctest` falhar visível). Uso:
  `cmake -S . -B build-asan -DPIERROT_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug`, build
  e `ctest --test-dir build-asan --output-on-failure`. `build-asan/` no
  `.gitignore`. `tests/asan.supp` suprime o ruído do driver NVIDIA
  (`libcuda`/`nvidia_drv_video` alocam globais no `cuInit` e nunca liberam) —
  aplicado só a esses módulos, nunca a funções do Pierrot.
- **UBSan achou overflow assinado nos hashes de EQ (Fase 8b)** — os três
  detectores de mudança de `AudioFxDsp.h` (master, `GraphicEq`,
  `ParametricEqDsp`) acumulavam `key = key*31 + …` em `int`; com muitos
  parâmetros estourava (`1775007360 * 31 cannot be represented in 'int'`). O
  hash só precisa mudar quando um parâmetro muda, então viraram `quint64`
  (wraparound definido). Sem mudança de comportamento audível.
- **`tst_crashreporter` compatível com UBSan** — o teste provoca um SIGSEGV
  de verdade (deref de nulo) para exercitar o handler; o UBSan abortava o
  processo antes do handler rodar. O UB deliberado foi isolado num helper
  `crashHere()` com `__attribute__((no_sanitize("undefined")))`; o resto do
  teste segue instrumentado.
- **Worker dedicado para camadas inferiores (H3)** — segundo `FrameWorker`
  (`preview-layers`) em paralelo com o do topo: em projeto multi-track as
  camadas deixam de "roubar slot" do clipe exibido. Medido no projeto de
  estresse (6 cortes, 6 faixas, 30 s): **1 quadro perdido em 1.761**,
  stalls máx. 61 ms.
- **M2 resolvido** — o LRU de frames do `FFmpegDecoder` virou intrusivo O(1)
  (`std::list` + splice) em vez de QList com rebuild do índice a cada acesso.
- **Métrica de latência do worker corrigida** — quadros do worker de camadas
  não devem mais ser medidos com o timestamp do dispatch do topo (poluía o
  p99 do profiler com latências falsas de ~1 s).

### Configurações

- **Limiar de proxy editável** (Configurações ▸ Desempenho): "Gerar proxy a
  partir de (largura)", padrão 2560 px. Fontes 2K (ex.: 2048×1152) ficavam
  sem proxy e o corte pagava ~1 s de decode do GOP original; com o limiar em
  1920/2048 elas ganham proxy e os cortes ficam lisos. Persistido em
  `proxyMinWidth`; `PIERROT_PROXY_MIN_WIDTH` continua sobrescrevendo na
  largada. Baixar o limiar re-enfileira as fontes conhecidas na hora.
- **`PIERROT_PERF_JSON=/caminho` grava o relatório também no fechamento**
  (antes só o harness `--autoplay` gravava — teste manual não deixava
  métricas). Nota: no Qt 6.8+, logs só saem para arquivo com
  `QT_FORCE_STDERR_LOGGING=1`.

### CLI

- **`pierrot projeto.Blanc`** abre direto no editor, pulando a janela de
  boas-vindas (testes e uso avançado).

### Janelas auxiliares

- **Fechar a janela principal fecha tudo**: preview externo e painel técnico
  (janelas de nível superior da Bancada) são encerrados junto no
  `closeEvent` — antes podiam sobreviver ao fecho e deixar o processo "em
  segundo plano".

### Grão de filme

- **Efeito "Grão de filme" por clipe** (estética de película 16/35mm):
  ruído monocromático animado por quadro aplicado por cima do clipe —
  intensidade (0..1) e tamanho do grão (1..4 px). Presente no painel de
  efeitos, no Express (atributo `filmGrain`/`filmGrainAmount`/`filmGrainSize`),
  no Propriedades do clipe e serializado no `.Blanc` (com teste de
  round-trip).
- **Paridade preview ↔ export**: o export gera `noise=alls=<str>:allf=t+u`
  (ruído temporal uniforme em luminância) no `filter_complex`, reproduzindo
  o mesmo grão desenhado no preview.
- Tradução JP dos novos rótulos incluída.

### Importação de mídia (performance + indicadores)

- **Fim do probe duplicado na importação**: `ProxyManager::probeAndQueue`
  ganhou a forma `(path, hasVideo, width)` que reusa o resultado do probe
  que a importação já fez — antes cada arquivo de vídeo era probeado duas
  vezes, a segunda **na UI thread** (travava a importação de mídia em
  disco lento/rede). Re-enfileirar proxies (preferência do projeto) também
  não re-proba: a largura fica guardada em `m_videoSrcs`.
- **Indicador de proxy na statusBar**: novo sinal
  `ProxyManager::progressChanged(pending, running)` → mensagem
  "Gerando proxy: N restante(s)…" com a barrinha de atividade — a
  transcodificação de um 4K (minutos) era a maior etapa da importação sem
  nenhum feedback. A barra agora cobre proxy + thumbs/picos de uma vez
  (`MainWindow::refreshBackgroundBusy`).
- **Barra de importação com contagem**: a barra do Central de Mídias mostra
  "Importando X/N" durante o probe (antes era uma barrinha muda).
- **Cache em disco dos picos de áudio** (`~/.cache/pierrot/peaks/*.peaks`):
  binário min+máx por bucket com hash por fonte+mtime+size+stream e poda
  em 512 arquivos — antes, reabrir um projeto com o LRU de RAM estourado
  re-decodificava o **stream de áudio inteiro** (um WAV de 200 MB era lido
  por completo) só para redesenhar a waveform.

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
