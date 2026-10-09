# Funcionalidades do Pierrot v0.7

## Media Pool

- Importe vídeos, áudios e imagens por arraste do sistema ou pelo botão **Importar**.
- Múltiplos arquivos simultâneos com análise assíncrona (não trava a interface).
- Miniaturas com tamanho ajustável.
- **Geradores de mídia** (estilo Vegas): mídia virtual sem arquivo, funciona como fundo/camada — **cor sólida**, **gradiente**, **checkboard** e **ruído (grão)**.
- **Trimmer (in/out)**: ao soltar uma mídia de vídeo na timeline, abre o diálogo Trimmer para escolher o trecho (scrubber, `I`/`O` para marcar in/out, prévia de quadro, tocar). Cancelar aborta a soltura; multi-seleção insere direto. **Desativável** nas Configurações (Qualidade e Timeline ▸ "Abrir o trimmer ao soltar mídia") para inserir direto.
- **Fila de render**: várias exportações (formatos/resoluções/fps diferentes) na fila, rodando em sequência (menu Arquivo ▸ Fila de render…).
- **Correção de cor (Lumetri)**: por clipe (menu de contexto ▸ Correção de cor…),
  com abas **Básico** (exposição, contraste, realces/sombras, brancos/pretos,
  saturação, vibrance, temperatura/tint, filme desbotado, nitidez),
  **Rodas** (Lift/Gamma/Gain RGB), **Curvas** (master + R/G/B) e
  **Vinheta / LUT** (`.cube` 3D + intensidade + mix). Aplicado no preview e
  na exportação (`eq`, `colorbalance`, `curves`, `vignette`, `unsharp`,
  `lut3d`).
- **Analisadores (Scopes)**: painel com waveform, histograma RGB e vectorscope, seguindo o quadro composto do preview (menu Exibir ▸ Analisadores).
- **Explorador de arquivos** integrado (dock): Lugares (Início + SSDs externos), importação por duplo clique, "Importar pasta" ou arraste, modo miniaturas.

## Timeline

- **Multi-faixa**: faixas de vídeo e áudio empilhadas, zoom (Ctrl+roda), rolagem.
- **Thumbnails de vídeo** nos clipes (quadros carregados em thread de fundo).
- **Ondas sonoras** nas faixas de áudio (picos min/max, calculados em thread de fundo).
- **Faixas de áudio independentes**: arquivos com vários streams (OBS/câmera) geram um clipe por faixa.
- **Reordenar faixas** por arraste do cabeçalho.
- **Pastas (grupos)**: arraste faixas para criar/grupos, presets de tamanho (minimizada/normal/grande).
- **Marcadores** na régua (`M` para adicionar/remover na agulha; menu do botão direito).
- **Botões de zoom** (`−`/`+`) na régua.
- **Grade de fundo** e **régua de tempo** toggáveis na barra de ferramentas.
- **Região de loop** (arraste na régua; Q ativa/desativa; Delete com ripple ou sem).

## Workspaces (painéis)

- **Workspaces nomeados** no menu **Exibir ▸ Workspaces** (estilo Premiere):
  layout de docks salvo por nome; o layout atual vira o workspace **Edição**.
- **Presets fixos**: **Edição** (timeline + pool), **Áudio** (mixer + scopes),
  **Composição** (Mesa + Pancrop), **Efeitos** (Efeitos + Express + Propriedades).
- **Menu Exibir agrupado** por região (esquerda / direita / embaixo).
- Layout persistido entre sessões; **Travar layout** (Ctrl+L) evita arrastar
  painéis sem querer.

## Mesa (composição 2D / 3D leve)

- **Mesa**: canvas de composição estilo After Effects — camadas de faixa com
  posição, escala, rotação, opacidade e blend; câmera animável; keyframes.
- **Modo 3D (AE Classic)**: toggle na Mesa (menu / Propriedades); campos
  `mesaZ`, `camZ`, `camFov`, rotações XYZ. Render 3D completo entra na Fase 1.
- **Malha 3D (.obj)**: Mesa ▸ Nova camada ▸ Malha 3D — importa OBJ como
  camada (faces preenchidas; perspectiva simples com modo 3D ON).
- Criada pelo menu da timeline (**Criar Mesa**); faixas podem ser enviadas
  como camadas.
- Preview e exportação usam o mesmo `MesaRenderer` (paridade).

## Edição de Clipes

- **Mover**: arraste clipes na timeline.
- **Agulha sempre à mão**: clique em qualquer ponto da timeline — inclusive sobre um clipe — move a agulha para o cursor (estilo Vegas; `Ctrl+clique` preserva o playhead).
- **Auto-scroll**: ao arrastar a agulha, a região de loop, mover clipes ou trims para perto de uma borda, a timeline rola sozinha acompanhando o cursor (zona de 32 px à esquerda, 16 px à direita).
- **Selecionar**: clique (ou Shift/Ctrl+clique para múltiplos).
- **Trim**: arraste bordas para aparar.
- **Insert / Overwrite** (Source Monitor): `,` insere o trecho In→Out no playhead (ripple); `.` sobrescreve o intervalo sem deslocar o resto. Alvo = faixa de vídeo/áudio selecionada (ou a primeira desbloqueada).
- **Duplicar**: Ctrl+D.
- **Excluir**: Delete (com ripple configurável).
- **Dividir no playhead**: tecla `S` (apenas clipes selecionados; sem seleção = todos).
- **Cantos de fade**: arraste canto superior esquerdo/direito para fade in/out.
- **Opacidade do clipe** (estilo Vegas): arraste parte superior do clipe de vídeo para cima/baixo.
- **Velocidade do clipe**: ajuste por diálogo (0,1×–4×), com barra de velocidade no cabeçalho.
- **Editor de Velocidade (dock)**: envelope time-remapping por clipe — curva
  velocidade×tempo **bezier** (varinha **Curva** / **B**: clique cria ponto com
  alças; arraste entorta a velocidade), keyframes arrastáveis, base 0,1×–4×,
  Easy Ease (**F9**). Menu do clipe ▸ **Editor de velocidade**. Preview e
  export usam a mesma curva (`clipSrcTime` + pré-render de sequência PNG).
- **Banda de velocidade no clipe (timeline)**: desenho estilo Time Remapping
  do Premiere — curva azul, linha de 1×, losangos de keyframe, rótulos
  **Rápido/Baixo** e %; arrastar o corpo do clipe (abaixo do handle de
  opacidade) sobe/desce a velocidade; **duplo clique** alterna keyframe.
- **Presets de clipe** (estilo Vegas): menu de contexto do clipe — **Salvar Preset…** guarda efeitos/transform/pan-crop/keyframes/áudio num preset nomeado persistente; **Aplicar Preset…** aplica o preset salvo a todos os clipes selecionados.
- **Multicâmera**: selecione ≥2 clipes de vídeo → menu **Criar multicam**. O clipe resultante tem N ângulos (fontes); teclas **1..N** cortam o ângulo ativo no playhead (keyframes step). Preview, Mesa e exportação usam o ângulo ativo.

## Ferramentas da Timeline

| Tecla | Ferramenta | Descrição |
|-------|-----------|-----------|
| `0` | Selecionar | Selecionar/mover clipes |
| `M` | Mover | Mover clipes |
| `R` | Tesoura | Corte livre — **segurar R** ativa, **soltar** volta à ferramenta anterior (hold-to-use) |
| `E` | Envelope | Editar linhas de volume/efeitos |
| `Z` | Lupa | Zoom na região arrastada |
| `B` | Ripple | Trim com ripple (empurra adjacentes) |
| `N` | Rolling | Ajustar fronteira entre 2 clipes |
| `Y` | Slip | Mudar in/out sem mudar posição |
| `Ctrl+U` | Slide | Mover clipe e adjacentes ajustam |
| `W` | Esticar Velocidade | Mudar velocidade para preencher espaço |

## Propriedades por Clipe

- **Volume** (0–200%) e **Opacidade** (0–100%).
- **Velocidade** (0,1×–4×).
- **Fade in/out**.
- **Transformação**: posição X/Y, escala, escala X/Y, rotação, âncora X/Y.
- **Recorte**: crop L/R/T/B.
- **Efeitos de vídeo**: brilho, contraste, saturação, desfoque, preto e branco, chroma key.
- **Efeitos de áudio**: **EQ Express** (graves/médios/agudos, −12 a +12 dB) e **Reverb EX** (mix e tamanho) por clipe — com DSP em tempo real no preview (reverb Schroeder) e reproduzidos na exportação (cadeia dry/wet via `asplit`/`aecho`/`amix`).
- **Painel Effects (réplica Premiere)**: árvore única com busca — Presets,
  Audio Effects, Audio Transitions, Video Effects (Adjust/Blur/Key/Stylize +
  Third-Party OFX + frei0r), Video Transitions (Dissolve/Wipes) e
  Text/Animation. Duplo clique aplica no clipe; transições gravam
  `transitionType`; texto cria/edita clipe de texto.
- **LAINKA** (stop motion): skip, jitter, flicker, warp, onion skin, dust, scratch.
- **Motion blur**.
- **Plugins OFX** (OpenFX): efeitos de terceiros.
- **Plugins frei0r** (padrão Kdenlive/Shotcut/MLT): o painel Efeitos lista os
  `.so` de `/usr/lib/frei0r-1` (e `PIERROT_FREI0R_PATH`); arraste para o clipe
  e edite no Express. No export, os mesmos efeitos viram filtros
  `frei0r=` no `filter_complex` (requer ffmpeg com `--enable-frei0r`).
  Pacotes típicos: `frei0r-plugins` (Debian/Ubuntu), plugins do Shotcut/Kdenlive.

## Keyframes e Animação

- **Editor de curvas** (dock): splines suaves (cubic bezier), grade, agulha arrastável.
- **Keyframes com alças bezier** para entortar curvas.
- **Easy Ease (varinha)**: botão **Varinha** no Editor de Curvas + **F9** —
  suaviza os keyframes selecionados com handles horizontais (estilo After
  Effects/Premiere). `Ctrl+F9` = Easy Ease In · `Shift+F9` = Easy Ease Out.
  Também no Editor de Velocidade (keyframe sob o playhead).
- **Interpolações**: Linear, Suave, Degrau, Bezier.
- **Seleção múltipla** de keyframes (Shift/marquee).
- **Pan/Crop com keyframes** sincronizados com o editor de curvas (mesmos dados).
- Painel com seções recolhíveis (Recorte / Zoom e Posição / Rotação).
- Escala X/Y independente, âncora X/Y arrastrável.
- **Safe margins** (title-safe/action-safe) no viewfinder.
- **Âncora visual** (gizmo branco) no viewfinder, arrastrável.

## Clipes de Texto

- Clipe de texto independente (animável: transform, opacidade, fades, keyframes).
- Editor dedicado (fonte, cor, contorno, fundo, posição).
- Cópia unificada/compartilhada estilo Vegas.

## Modos de Composição por Faixa

12 modos de blend: Normal, Screen, Multiply, Overlay, Darken, Lighten, Softlight, Hardlight, Difference, Addition, Subtract, Exclusion.
- Opacidade de faixa (0–100%).

## Preview e Reprodução

- Reprodução em tempo real com seek frame-a-frame.
- **Shuttle JKL** (Vegas): `J` retrocede, `K` pausa, `L` avança; repetir acelera (1×→2×→4×). Áudio silencia fora de 1× dianteiro.
- **Áudio** (Qt Multimedia, via libswresample).
- Composição multi-faixa no preview (blend, opacidade, fades, cor sólida).
- **Transições** (dissolve, wipes 4 direções + 4 diagonais) no preview e exportação.
- **Program Monitor** (central): transporte no monitor, quadro anterior/próximo, loop, qualidade de preview, timecode flutuante, rótulo `Program: <projeto>`.
- **Source Monitor** (dock, estilo Premiere): duplo-clique na Central de Mídias abre a mídia aqui; `I`/`O` marcam In/Out; **Insert** (`,`) insere o trecho no playhead empurrando os clipes seguintes; **Overwrite** (`.`) sobrescreve o intervalo no playhead.
- Qualidade de preview configurável (resolução de decodificação).
- Sincronização A/V pelo relógio do áudio (slew suave, correção de drift).
- **Relógio de áudio** como mestre para evitar dessincronia.

## Mixer (dock)

- **Faders por faixa** de áudio e vídeo (vertical, 0–200% com escala dB).
- **Pan** por faixa (knob rotativo, equal-power).
- **Volume master** com fader dedicado.
- **Mute / Solo** por faixa.
- **VU meters** em tempo real com **peak hold** (barra LED, gradiente verde→amarelo→vermelho).
- Volume master e pan aplicados no preview e na exportação (.Blanc).
- Visual escuro estilo Vegas Pro.
- Acessível via menu **Exibir → Mixer**; layout persistido entre sessões.

## Atalhos de Teclado

| Tecla | Ação |
|-------|------|
| `Espaço` | Reproduzir / pausar |
| `J` / `K` / `L` | Shuttle JKL: retroceder / pausar / avançar (repetir acelera) |
| `Enter` | Pausar e voltar ao cursor |
| `S` | Dividir clipe no playhead |
| `Delete` | Excluir clipe selecionado |
| `←` / `→` | Mover agulha 1 frame |
| `Alt+←` / `Alt+→` | Deslocar clipes selecionados |
| `Home` / `End` | Ir para início / fim |
| `U` / `G` | Desagrupar / agrupar clipes |
| `Ctrl+Z` / `Ctrl+Y` | Desfazer / Refazer |
| `Ctrl+S` | Salvar projeto |
| `Ctrl+O` | Abrir projeto |
| `Ctrl+I` | Importar mídia |
| `Ctrl+E` | Exportar vídeo |
| `Ctrl+D` | Duplicar clipe |
| `Ctrl+roda` | Zoom da timeline |
| `Ctrl+clique` | Alternar marcador / multi-seleção |
| `Q` | Ativar/desativar loop |
| `Shift+V` | Mostrar/ocultar linha de volume/envelope da faixa de áudio |
| `1`–`9` | Multicam: cortar para o ângulo N no playhead |
| `I` / `O` | Source Monitor: marcar In / Out |
| `,` | Inserir trecho do Source no playhead (Insert) |
| `.` | Sobrescrever timeline no playhead com o Source (Overwrite) |

## Undo/Redo

- Ilimitado (snapshots, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y).
- **Painel Histórico de Edições** (Exibir ▸ Histórico de Edições): lista os passos com nome da operação (dividir, excluir, colar, agrupar…); clicar em um passo salta o histórico para ele (undo/redo em bloco).

## Salvar/Abrir

- Formato `.Blanc` (JSON). Abre projetos `.ovp` legados.
- **Salvamento automático** configurável (intervalo em minutos).
- Janela e painéis lembrados (geometria, docks).

## Exportação

- **MP4** (H.264/AAC), **MKV** (H.264/AAC), **WebM** (VP9/Opus).
- Resolução e FPS configuráveis.
- Composição multi-faixa (overlay + blend).
- Efeitos, chroma key, fades, textos (drawtext), velocidade, opacidade.
- Mix de áudio com volume por clipe.
- Texto renderizado como mídia PNG transparente.
- Animações de transform/rotação coerentes com o preview.

## Estabilidade

- **Relatório de crash**: backtrace + infos em `~/Pierrot-crash-*.txt`, aviso na próxima abertura.

## Arquitetura

```
src/
  main.cpp                    Ponto de entrada
  version.h                   Versão do projeto
  CrashReporter               Relatório de crash (backtrace + infos do sistema)
  models/Project              Modelo: mídia, faixas, clipes + serialização JSON
  laartman/FFmpegDecoder      Motor de codecs: decodificação de frames/áudio (libav*)
  ffmpeg/MediaCache           Cache de waveforms/thumbnails em thread de fundo
  export/ProjectExporter      Geração do comando ffmpeg (filter_complex)
  ui/TimelineWidget           Timeline interativa (drag, corte, trim, zoom, reordenação de faixas/pastas, presets de estilo)
  ui/GraphEditorWidget        Editor de curvas (keyframes, splines bezier, interpolações)
  ui/PancropWidget            Pan/Crop com keyframes sincronizados com o editor de curvas
  ui/MediaPoolWidget          Painel de mídia com arrasto do sistema e para a timeline
  ui/PreviewWidget            Preview + reprodução (AudioMixer integrado)
  ui/SourceMonitorWidget      Source Monitor dock: In/Out + Insert/Overwrite (Premiere)
  colombina/frei0r             Host frei0r (plugins Kdenlive/Shotcut/MLT)
  ui/ExportDialog             Diálogo de exportação com progresso
  ui/ProjectSettingsDialog    Resolução e fps do projeto
  ui/EffectsWidget            Painel de efeitos com abas Vídeo/Áudio (internos + OFX + EQ Express/Reverb EX)
  ui/ExpressWidget            Expressões e atalhos
  ui/AudioEffectsDialog       Diálogo de efeitos de áudio por clipe
  ui/FileBrowserWidget        Explorador de arquivos (dock, Lugares + miniaturas)
  ui/SettingsDialog           Configurações gerais + atalhos configuráveis
  ui/TextEditorDialog         Editor de texto para clipes de título
  ui/TransformDialog          Diálogo de transformação (posição, escala, rotação)
  ui/TitleBar                 Barra de título personalizada (janela de boas-vindas)
  ui/WelcomeWindow            Janela de boas-vindas (projetos recentes, novo projeto)
  ui/ClickLogger              Logger de cliques (debug)
  ofx/OfxHost                 Host OFX (Property/Parameter/ImageEffect/Memory/MultiThread/Message/Progress suites)
  ofx/OfxPluginManager        Scanner e loader de plugins .ofx
  ofx/OfxRenderer             Renderização via plugins OFX
  MainWindow                  Janela principal + undo/redo + salvar/abrir + persistência de geometria/layout
```
