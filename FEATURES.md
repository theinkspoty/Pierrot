# Funcionalidades do Pierrot v0.5

## Media Pool

- Importe vídeos, áudios e imagens por arraste do sistema ou pelo botão **Importar**.
- Múltiplos arquivos simultâneos com análise assíncrona (não trava a interface).
- Miniaturas com tamanho ajustável.
- **Gerador de cor sólida** (estilo Vegas): mídia virtual sem arquivo, funciona como fundo/camada.
- **Explorador de arquivos** integrado (dock): Lugares (Início + SSDs externos), importação por duplo clique, "Importar pasta" ou arraste, modo miniaturas.

## Timeline

- **Multi-faixa**: faixas de vídeo e áudio empilhadas, zoom (Ctrl+roda), rolagem.
- **Thumbnails de vídeo** nos clipes (quadros carregados em thread de fundo).
- **Ondas sonoras** nas faixas de áudio (picos min/max, calculados em thread de fundo).
- **Faixas de áudio independentes**: arquivos com vários streams (OBS/câmera) geram um clipe por faixa.
- **Reordenar faixas** por arraste do cabeçalho.
- **Pastas (grupos)**: arraste faixas para criar/grupos, presets de tamanho (minimizada/normal/grande).
- **Marcadores** na régua (Ctrl+clique para alternar; menu do botão direito).
- **Botões de zoom** (`−`/`+`) na régua.
- **Grade de fundo** e **régua de tempo** toggáveis na barra de ferramentas.
- **Região de loop** (arraste na régua; Q ativa/desativa; Delete com ripple ou sem).

## Edição de Clipes

- **Mover**: arraste clipes na timeline.
- **Selecionar**: clique (ou Shift/Ctrl+clique para múltiplos).
- **Trim**: arraste bordas para aparar.
- **Duplicar**: Ctrl+D.
- **Excluir**: Delete (com ripple configurável).
- **Dividir no playhead**: tecla `S` (apenas clipes selecionados; sem seleção = todos).
- **Cantos de fade**: arraste canto superior esquerdo/direito para fade in/out.
- **Opacidade do clipe** (estilo Vegas): arraste parte superior do clipe de vídeo para cima/baixo.
- **Velocidade do clipe**: ajuste por diálogo (0,1×–4×), com barra de velocidade no cabeçalho.

## Ferramentas da Timeline

| Tecla | Ferramenta | Descrição |
|-------|-----------|-----------|
| `0` | Selecionar | Selecionar/mover clipes |
| `M` | Mover | Mover clipes |
| `R` | Tesoura | Corte livre (clique na timeline) |
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
- **LAINKA** (stop motion): skip, jitter, flicker, warp, onion skin, dust, scratch.
- **Motion blur**.
- **Plugins OFX** (OpenFX): efeitos de terceiros.

## Keyframes e Animação

- **Editor de curvas** (dock): splines suaves (cubic bezier), grade, agulha arrastável.
- **Keyframes com alças bezier** para entortar curvas.
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
- **Áudio** (Qt Multimedia, via libswresample).
- Composição multi-faixa no preview (blend, opacidade, fades, cor sólida).
- **Transições** (dissolve, wipe) no preview e exportação.
- **Barra de transporte** abaixo do preview (Início, ← frame, Play/Pause, → frame, Fim).
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
| `V` | Mostrar/ocultar linhas de volume |

## Undo/Redo

- Ilimitado (snapshots, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y).

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
  ffmpeg/FFmpegDecoder        Decodificação de frames e picos de áudio (libav*)
  ffmpeg/MediaCache           Cache de waveforms/thumbnails em thread de fundo
  export/ProjectExporter      Geração do comando ffmpeg (filter_complex)
  ui/TimelineWidget           Timeline interativa (drag, corte, trim, zoom, reordenação de faixas/pastas, presets de estilo)
  ui/GraphEditorWidget        Editor de curvas (keyframes, splines bezier, interpolações)
  ui/PancropWidget            Pan/Crop com keyframes sincronizados com o editor de curvas
  ui/MediaPoolWidget          Painel de mídia com arrasto do sistema e para a timeline
  ui/PreviewWidget            Preview + reprodução (AudioMixer integrado)
  ui/ExportDialog             Diálogo de exportação com progresso
  ui/ProjectSettingsDialog    Resolução e fps do projeto
  ui/EffectsWidget            Painel de efeitos (internos + OFX)
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
