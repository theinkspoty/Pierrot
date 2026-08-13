# Pierrot

Editor de vídeo simples — feito de um editor para outro editor — estilo
**Sony Vegas Pro** para **Linux** (e Windows), escrito em **C++** com
**Qt Widgets** e **FFmpeg** (decodificação em processo + exportação via CLI).

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
  darken, lighten, softlight, hardlight, difference, addition, subtract e exclusion.
- **Marcadores** na régua (Ctrl+clique para alternar; menu do botão direito).
- **Botões de zoom** (`−`/`+`) na régua, além do Ctrl+roda.
- **Cantos de fade** (setas) desenhados nos clipes que têm fade in/out.
- **Preview** com reprodução em tempo real e seek, incluindo **áudio** (Qt Multimedia, via `libswresample`).
- **Undo/Redo** ilimitado (snapshots, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y).
- **Salvar/Abrir projetos** em `.ovp` (JSON).
- **Configurações de projeto**: resolução e quadros/s.
- **Exportação**: MP4 (H.264), MKV e WebM (VP9), com resolução/fps configuráveis,
  compondo múltiplas faixas de vídeo (overlay + blend), efeitos, chroma key, fades,
  textos (drawtext), velocidade (setpts/atempo), opacidade e mix de áudio com volume.
- **Janela e painéis lembrados**: posição/tamanho da janela e o arranjo de docks
  são salvos ao fechar e restaurados ao abrir. Geometria inválida ou vinda de um
  monitor que não está mais conectado (ex.: TV 4K) é descartada — a janela nunca
  abre fora da tela nem maior que a área disponível.

## Especificações

| Item                | Detalhe                                                     |
|---------------------|-------------------------------------------------------------|
| Formato de projeto  | `.ovp` (JSON)                                               |
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

## Windows (MSVC + vcpkg)

Pré-requisitos: **Visual Studio 2022** (build C++), **CMake** e **vcpkg**:

```bat
git clone https://github.com/microsoft/vcpkg
vcpkg\bootstrap-vcpkg.bat
vcpkg install              :: instala qtbase + ffmpeg (usando o vcpkg.json)
cmake -B build ^
  -DCMAKE_TOOLCHAIN_FILE=<caminho>\vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

O executável sai em `build\Release\pierrot.exe` (com ícone e informações de
versão). O FFmpeg é localizado automaticamente pelo `cmake/FindFFmpeg.cmake`
(via vcpkg); no Linux continua usando pkg-config.

Para a **exportação**, copie um `ffmpeg.exe` (ex.: essentials de
https://www.gyan.dev/ffmpeg/builds/) para a mesma pasta do `.exe` ou adicione
ao `PATH`.

Distribuição: use **windeployqt** (vem com o Qt/vcpkg) para copiar as DLLs do
Qt junto do executável:

```bat
windeployqt --release build\Release\pierrot.exe
```



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
  ui/TimelineWidget          Timeline interativa (drag, corte, trim, zoom, propriedades, efeitos, blend)
  ui/MediaPoolWidget         Painel de mídia com drag-and-drop e import assíncrono
  ui/PreviewWidget           Preview + reprodução
  ui/ExportDialog            Diálogo de exportação com progresso
  ui/ProjectSettingsDialog   Resolução e fps do projeto
  MainWindow                 Janela principal + undo/redo + salvar/abrir + persistência de geometria/layout
```

## Roadmap

Marcado com 💀 o que já foi implementado.

### Edição
- 💀 Keyframes/curvas de volume, opacidade, transform, crop e efeitos ao longo
  do tempo (editor de curvas com interpolação linear, suave, segurar e bezier,
  com snap ao frame).
- 💀 Texto/título sobreposto por clipe.
- Transições entre clipes (dissolve, wipe) com duração ajustável.
- Imagens estáticas como clipes na timeline (PNG/JPEG, com trim e pan/crop).
- Seleção múltipla de clipes (editar, mover, duplicar, excluir em lote).
- **Snap** de clipes a bordas de outros clipes, ao playhead e a marcadores.
- **Ripple/roll/tipos de trim** mais elaborados (empurrar clipes adjacentes).
- **Ripple delete** por padrão e "fechar espaço" com atalho dedicado.
- Clipes de texto/áudio **renderizados como placeholder na timeline** (mostrar o
  texto e o nome dos áudios no clipe).

### Preview / reprodução
- 💀 Reprodução em frames do projeto com timecode `HH:MM:SS:FF`.
- **Sincronização áudio/vídeo** pelo relógio do decoder (hoje o playhead usa
  relógio de parede; em reproduções longas pode dessincronizar alguns ms).
- **Decodificação por hardware** (VAAPI/NVDEC) para preview suave de 4K.
- **Preview "alta qualidade"** (full res / 100% verdadeiro ao alternar pausa).
- **VU meters** de áudio no monitor.
- Marcadores/safe areas (action/title safe) no monitor.

### Exportação
- 💀 Exportar MP4 (H.264), MKV e WebM (VP9) com blend de faixas, efeitos,
  chroma key, fades, velocidade e mix de áudio.
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
- **Importar pasta inteira** para o media pool, com subpastas.
- Suporte a **projetos 60/120 fps** (hoje o preview e os keyframes já respeitam
  o fps do projeto; validar exportação e timestamps).
- Suporte a plugins **OFX (OpenFX)** para efeitos de terceiros.

### Qualidade de vida
- **Auto-save** com versões/backup dos projetos.
- **Templates de projeto** (4K, Full HD, vertical 9:16, etc.) na janela inicial.
- **Atalhos configuráveis**.
- **Tema escuro/claro** e escolha de idioma.
