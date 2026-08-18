# Pierrot

Editor de vídeo simples — O estilo vem do **Sony Vegas Pro** e **Final Cut Express (2003)**,
desenvolvido **exclusivamente para Linux**, escrito em
**C++** com **Qt Widgets** e **FFmpeg** (decodificação em processo + exportação via CLI).

A ideia do editor é: ser otimizado e ter um fluxo de trabalho rápido.
pra videos sem complexidade
focado no youtube e outras.

## Funcionalidades

- **Media Pool**: importe vídeos/áudios e arraste para a timeline.
- **Timeline multi-faixa**: faixas de vídeo e áudio empilhadas, zoom (Ctrl+roda), rolagem.
- **Thumbnails de vídeo** nos clipes (quadros carregados em thread de fundo).
- **Ondas sonoras** nas faixas de áudio (picos min/max, calculados em thread de fundo).
- **Importação assíncrona**: analisar vários arquivos não trava a interface.
- **Clipes**: mover, selecionar, aparar pelas bordas (trim), duplicar, excluir.
- **Corte no playhead** (tecla `S`).
- **Propriedades por clipe**: volume, opacidade, velocidade (0,1×–4×), fade in/out
  e texto/título sobreposto (vídeo), todos aplicados na exportação.
- **Efeitos de vídeo por clipe**: brilho, contraste, saturação, desfoque,
  preto e branco e **chroma key** (cor + similaridade).
- **Modos de composição por faixa de vídeo**: normal, screen, multiply, overlay,
  darken, lighten, softlight, hardlight, difference, addition, subtract e exclusion
  — refletidos **no preview** e na exportação.
- **Mídia gerada (cor sólida, estilo Vegas)**: crie uma "Cor sólida" no Media
  Pool (botão **Gerador**); é uma mídia virtual (sem arquivo) que funciona como
  fundo/camada, com transparência visível sobre ela no preview e exportada via ffmpeg.
- **Marcadores** na régua (Ctrl+clique para alternar; menu do botão direito).
- **Botões de zoom** (`−`/`+`) na régua, além do Ctrl+roda.
- **Cantos de fade** (setas) desenhados nos clipes que têm fade in/out.
- **Preview** com reprodução em tempo real e seek, incluindo **áudio** (Qt Multimedia, via `libswresample`).
- **Preview compõe múltiplas faixas de vídeo**: mídia transparente (PNG/WebP com
  alpha) mostra a camada de baixo, com blend por faixa, opacidade/fades e a cor
  sólida funcionando como fundo — coerente com a exportação.
- **Undo/Redo** ilimitado (snapshots, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y).
- **Salvar/Abrir projetos** em `.Blanc` (JSON). Projetos antigos `.ovp` ainda abrem.
- **Configurações de projeto**: resolução e quadros/s.
- **Exportação**: MP4 (H.264), MKV e WebM (VP9), com resolução/fps configuráveis,
  compondo múltiplas faixas de vídeo (overlay + blend), efeitos, chroma key, fades,
  textos (drawtext), velocidade (setpts/atempo), opacidade e mix de áudio com volume.
- **Janela e painéis lembrados**: posição/tamanho da janela e o arranjo de docks
  são salvos ao fechar e restaurados ao abrir. Geometria inválida ou vinda de um
  monitor que não está mais conectado (ex.: TV 4K) é descartada — a janela nunca
  abre fora da tela nem maior que a área disponível.
- **Editor de curvas** (docking): curva por propriedade desenhada como splines
  suaves (cubic bezier por segmento, estilo Premiere), agulha arrastável, grade
  de referência, keyframes com alças (bezier) para entortar a curva, seleção
  múltipla (Shift/marquee), interpolações (Linear/Suave/Degrau/Bezier) e
  movimento de keyframes no tempo e valor.
- **Pan/Crop** com keyframes: arrastar a agulha na faixa de tempo (scrub),
  duplo clique cria keyframe, seleção e exclusão de keyframes (Delete) — tudo
  **sincronizado** com o editor de curvas (mesmos dados, um reflete o outro).
  Painel com seções recolhíveis (Recorte / Zoom e Posição / Rotação), escala X/Y
  independente e **safe margins** (title-safe/action-safe) no viewfinder.
- **Timeline**: faixas de vídeo na ordem `V1` no topo; arraste o cabeçalho de
  uma faixa para **reordenar** ou soltá-la numa **pasta (grupo)**; arraste a
  faixa da pasta para mover o grupo inteiro; presets de tamanho de faixa
  (**Estilo**: minimizada/normal/grande) na barra de ferramentas da timeline;
  apagar a **região de loop** com ripple ou deixando espaço.
- **Media Pool**: importe arrastando arquivos do sistema para dentro do painel,
  ou arraste itens do painel para a timeline (com miniatura seguindo o cursor);
  miniaturas com **tamanho ajustável**.
- **Clipes de texto independentes**: animáveis (transform, opacidade, fades,
  keyframes via editor de curvas, pan/crop) com editor dedicado (fonte, cor,
  contorno, fundo, posição) e cópia unificada/compartilhada estilo Vegas.
- **Múltiplas faixas de áudio**: arquivos com vários streams (OBS/câmera) geram
  um clipe de áudio por faixa, cada um na sua faixa e com waveform própria.
- **Velocidade do clipe**: ajuste por diálogo (0,1×–4×), respeitado no preview
  e na exportação (estilo Vegas).
- **Explorador de arquivos** (dock): navegue por Lugares (Início + SSDs externos)
  e importe mídias direto para o Media Pool — por duplo clique, "Importar pasta"
  ou arraste, com modo miniaturas opcional (thumbnails reais).
- **Janela de boas-vindas** moderna: barra de título personalizada (botões à
  esquerda, cantos arredondados, arrastável), imagem da marca, projetos
  recentes e novo projeto.
- **Texto renderizado como mídia transparente**: na exportação, cada clipe de
  texto é gerado como PNG com fundo transparente (igual às imagens) — sem o
  "quadro preto" tampando a camada de baixo, e com cor/fonte/contorno/fundo
  consistentes com o preview.

------

- **Relatório de crash**: se o programa fechar de repente (SIGSEGV/SIGABRT/etc.),
  um relatório com backtrace e informações do sistema é salvo em
  `~/Pierrot-crash-*.txt`; na próxima abertura é exibido um aviso com opção de
  abrir o relatório ou limpá-lo.

## Especificações

| Item                | Detalhe                                                     |
|---------------------|-------------------------------------------------------------|
| Formato de projeto  | `.Blanc` (JSON); abre `.ovp` legado                              |
| Exportação          | MP4 (H.264/AAC), MKV (H.264/AAC), WebM (VP9/Opus)           |
| Resolução           | configurável (padrão 1920×1080)                             |
| Quadros/s           | configurável (padrão 30)                                    |
| Taxa de áudio       | 48 kHz (exportação)                                         |
| Efeitos por clipe   | volume, opacidade, velocidade, fades, texto, brilho, contraste, saturação, desfoque, P&B, chroma key |
| Blend por faixa     | 12 modos: normal, screen, multiply, overlay, darken, lighten, softlight, hardlight, difference, addition, subtract, exclusion |
| Zoom da timeline    | 2 px/s – 4000 px/s                                          |
| Fonte               | C++ (Qt Widgets) + FFmpeg (libav*)                          |

## Atalhos

| Tecla              | Ação                            |
|--------------------|---------------------------------|
| `Espaço`           | Reproduzir / pausar             |
| `S`                | Dividir clipe no playhead       |
| `Delete`           | Excluir clipe selecionado       |
| `Ctrl+Z` / `Ctrl+Y`| Desfazer / Refazer              |
| `Ctrl+S`           | Salvar projeto                  |
| `Ctrl+O`           | Abrir projeto                   |
| `Ctrl+I`           | Importar mídia                  |
| `Ctrl+roda`        | Zoom da timeline                |
| `Ctrl+clique`      | Alterna marcador na régua       |
| Clique + arraste   | Mover clipe / bordas = trim     |
| Botão direito      | Menu do clipe (propriedades, efeitos de vídeo), marcadores, faixas (adicionar, modo de composição) |
| V (timeline)       | Mostrar/ocultar linhas de volume |
| 0 / M / R / E / Z  | Ferramentas da timeline: selecionar, mover, tesoura, envelope, lupa |
| V / P / B (curvas) | Ferramentas do editor de curvas: selecionar, adicionar, curva |
| 1 / 2 / 3 / 4 (curvas) | Interpolação do keyframe: linear, suave, degrau, bezier |
| Delete (curvas/pancrop) | Exclui os keyframes selecionados (não o clipe) |

## Dependências (Ubuntu/Debian)

```bash
sudo apt install cmake g++ pkg-config \
    qt6-base-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

Se usar Qt 5 em vez de Qt 6: substitua `qt6-base-dev` por `qtbase5-dev` e
`qt5-qmake`/`libqt5widgets5`.

## Compilar

```bash
cd Pierrot
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/pierrot
```

O binário também precisa do `ffmpeg` no `PATH` para exportar (qualquer distro).

## AppImage (distribuição portátil)

Para gerar um AppImage único e portátil (Qt + libs FFmpeg + o próprio `ffmpeg`
embutidos), na sua máquina com as dependências instaladas:

```bash
packaging/build-appimage.sh
```

O resultado fica em `dist/Pierrot-<versão>-<arquitetura>.AppImage`. Rode com:

```bash
chmod +x dist/Pierrot-*.AppImage
./dist/Pierrot-*.AppImage
```

O script baixa o `linuxdeploy` na primeira execução e usa o `qmake` do Qt
instalado (`qmake6` ou `qmake`) para copiar os plugins Qt (platforms,
imageformats, iconengines) para dentro do AppImage, já com as dependências.
Variáveis opcionais:
`VERSION`, `OUT_DIR`, `BUNDLE_FFMPEG=0` (não embutir o ffmpeg) e
`UPDATE_INFORMATION` para embutir a string de atualização do **AppImageUpdate**
(ex.: `gh-releases-zsync|usuario|repo|latest|Pierrot-*-x86_64.AppImage.zsync`,
o `.zsync` correspondente também é movido para `dist/`).

## Licença

Pierrot é um software livre: você pode redistribuí-lo e/ou modificá-lo sob os
termos da **GNU General Public License versão 3** ou (a seu critério) qualquer
versão posterior, conforme publicado pela Free Software Foundation.

Este programa é distribuído na esperança de que seja útil, mas **SEM QUALQUER
GARANTIA**; sem sequer a garantia implícita de comercialização ou adequação a
um propósito específico. Veja a GNU GPL para mais detalhes.

- Texto completo da licença: [LICENSE](LICENSE) (GPL-3.0).
- Copyright (C) 2026 **theinkspoty**.
- Você deve ter recebido uma cópia da GNU General Public License junto com
  este programa; caso contrário, veja <https://www.gnu.org/licenses/>.

## Como usar

1. Clique em **Adicionar** no painel Mídia e escolha seus arquivos.
2. Arraste um item da lista para uma faixa de vídeo (ou áudio) na timeline.
3. Mova a régua (clique na parte superior) para posicionar o playhead.
4. `S` divide o clipe na posição do playhead; `Delete` remove o selecionado.
5. **Arquivo → Exportar…** (Ctrl+E) para gerar o vídeo final.

## Arquitetura

```
src/
  models/Project             Modelo: mídia, faixas, clipes + serialização JSON
  ffmpeg/FFmpegDecoder       Decodificação de frames e picos de áudio (libav*)
  ffmpeg/MediaCache          Cache de waveforms/thumbnails em thread de fundo
  export/ProjectExporter     Geração do comando ffmpeg (filter_complex)
  ui/TimelineWidget          Timeline interativa (drag, corte, trim, zoom, reordenação de faixas/pastas, presets de estilo)
  ui/GraphEditorWidget       Editor de curvas (keyframes, splines bezier, interpolações)
  ui/PancropWidget           Pan/Crop com keyframes sincronizados com o editor de curvas
  ui/MediaPoolWidget         Painel de mídia com arrasto do sistema e para a timeline
  ui/PreviewWidget           Preview + reprodução
  ui/ExportDialog            Diálogo de exportação com progresso
  ui/ProjectSettingsDialog   Resolução e fps do projeto
  MainWindow                 Janela principal + undo/redo + salvar/abrir + persistência de geometria/layout
```

## Roadmap

Marcado com 💀 o que já foi implementado.

### Edição
- 💀 Keyframes/curvas de volume, opacidade, transform, crop e efeitos ao longo
  do tempo, com **editor de curvas** (splines suaves por segmento, agulha
  arrastável, alças bezier, grade de referência, interpolação linear/suave/
  degrau/bezier e snap ao frame).
- 💀 Pan/Crop com keyframes **sincronizados com o editor de curvas** (mesmos
  dados): duplo clique cria keyframe, seleção e exclusão na faixa de tempo;
  painel com seções recolhíveis, escala X/Y independente e safe margins.
- 💀 **Mídia gerada (cor sólida, estilo Vegas)**: gerador no Media Pool, sem
  arquivo, funciona como fundo/camada com transparência no preview e na exportação.
- 💀 **Deletar a região de loop** com ripple ou deixando espaço.
- 💀 Texto/título sobreposto por clipe.
- 💀 Reordenar faixas de vídeo/áudio e **pastas (grupos)** por arraste do
  cabeçalho, além de presets de tamanho (**Estilo**: minimizada/normal/grande).
- Transições entre clipes (dissolve, wipe) com duração ajustável.
- 💀 Imagens estáticas como clipes na timeline (PNG/JPEG, com trim e pan/crop).
- Seleção múltipla de clipes (editar, mover, duplicar, excluir em lote).
- **Snap** de clipes a bordas de outros clipes, ao playhead e a marcadores.
- **Ripple/roll/tipos de trim** mais elaborados (empurrar clipes adjacentes).
- **Ripple delete** por padrão e "fechar espaço" com atalho dedicado.
- 💀 Clipes de texto/áudio **renderizados como placeholder na timeline** (mostrar o
  texto e o nome dos áudios no clipe).
- 💀 Clipe de **texto independente** (animável: transform, opacidade, fades,
  keyframes via editor de curvas; pan/crop) com editor de texto dedicado
  (fonte, cor, contorno, fundo, posição) e cópia unificada estilo Vegas.
- 💀 Múltiplas faixas de áudio: um clipe por stream (OBS/câmera), cada um na
  sua faixa, com waveform própria e stream selecionado no preview/exportação.
- 💀 Velocidade do clipe por diálogo (0,1×–4×), respeitada no preview e na
  exportação (estilo Vegas).

### Preview / reprodução
- 💀 Reprodução em frames do projeto com timecode `HH:MM:SS:FF`.
- 💀 **Composição multi-faixa no preview**: mídia transparente (PNG/WebP com
  alpha) mostra a camada de baixo, com blend por faixa, opacidade/fades e cor
  sólida como fundo — coerente com a exportação.
- **Sincronização áudio/vídeo** pelo relógio do decoder (hoje o playhead usa
  relógio de parede; em reproduções longas pode dessincronizar alguns ms).
- **Decodificação por hardware** (VAAPI/NVDEC) para preview suave de 4K.
- **Preview "alta qualidade"** (full res / 100% verdadeiro ao alternar pausa).
- **VU meters** de áudio no monitor.
- Marcadores/safe areas (action/title safe) no monitor.

### Exportação
- 💀 Exportar MP4 (H.264), MKV e WebM (VP9) com blend de faixas, efeitos,
  chroma key, fades, velocidade e mix de áudio.
- 💀 Texto exportado como mídia PNG transparente (fim do fundo preto; cor/
  fonte/contorno/fundo coerentes com o preview).
- 💀 Animações de transform/rotação coerentes com o preview (rotação em
  radianos, contain/letterbox, timebase fixo do texto).
- **Exportar apenas a região de loop** ou o clipe selecionado.
- **Cancelamento** do export a qualquer momento (hoje o processo roda até o fim).
- **Encode em segundo plano** (continua editando enquanto exporta).
- Perfis de exportação lembrados por projeto.
- Exportar **quadro único** (imagem PNG) no playhead.

### Engine / estabilidade
- **Sync de clips por grupo** (editar vídeo+áudio acoplados como uma unidade).
- **Re-vinculação de mídia ausente** ao abrir projeto (procurar arquivo movido).
- **Transcode/Proxy** automático para mídia pesada (4K/HEVC) e troca
  transparente na timeline.
- **Undo mais granular** (passo a passo das operações, em vez de snapshots).
- 💀 Importar arquivos por **arrastar do sistema** para o media pool.
- 💀 **Miniaturas com tamanho ajustável** no media pool.
- **Importar pasta inteira** para o media pool, com subpastas (hoje: importa as
  mídias da pasta atual; subpastas ainda não).
- Suporte a **projetos 60/120 fps** (hoje o preview e os keyframes já respeitam
  o fps do projeto; validar exportação e timestamps).
- Suporte a plugins **OFX (OpenFX)** para efeitos de terceiros.
- 💀 **Relatório de crash**: handler de sinais que salva backtrace e infos em
  `~/Pierrot-crash-*.txt` e avisa o usuário na próxima abertura.

### Janelas / docks
- 💀 **Explorador de arquivos** (dock estilo Premiere: Lugares — Início + SSDs
  externos, importação por duplo clique / "Importar pasta" / arraste, modo
  miniaturas).
- 💀 **Janela de boas-vindas** moderna (barra de título personalizada, cantos
  arredondados, imagem da marca, projetos recentes, novo projeto).
- **Painel de Efeitos** (dock criado como placeholder; a lista/aplicação de
  efeitos será construída aqui).

### Qualidade de vida
- **Auto-save** com versões/backup dos projetos.
- **Templates de projeto** (4K, Full HD, vertical 9:16, etc.) na janela inicial.
- **Atalhos configuráveis**.
- **Tema escuro/claro** e escolha de idioma.
