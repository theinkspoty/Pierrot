# Pierrot v0.4.2

Editor de vídeo para **Linux**, inspirado no **Sony Vegas Pro** e **Final Cut Express (2003)**.

Escrito em **C++** com **Qt Widgets** e **FFmpeg** (decodificação em processo + exportação via CLI).

A ideia do Pierrot é ser otimizado e ter um fluxo de trabalho rápido — focado em vídeos sem complexidade, YouTube e produções independentes.

---

## Captura de tela

*(adicionar screenshot aqui)*

---

## Instalação

### Dependências (Ubuntu/Debian)

```bash
sudo apt install cmake g++ pkg-config \
    qt6-base-dev qt6-multimedia-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
```

Se usar Qt 5 em vez de Qt 6: substitua `qt6-base-dev` por `qtbase5-dev`,
`qt6-multimedia-dev` por `qtmultimedia5-dev`.

### Compilar

```bash
cd Pierrot
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/pierrot
```

O binário também precisa do `ffmpeg` no `PATH` para exportar (qualquer distro).

### AppImage (distribuição portátil)

```bash
packaging/build-appimage.sh
```

O resultado fica em `dist/Pierrot-<versão>-<arquitetura>.AppImage`.

---

## Como usar

1. Clique em **Adicionar** no painel Mídia e escolha seus arquivos.
2. Arraste um item da lista para uma faixa de vídeo (ou áudio) na timeline.
3. Mova a régua (clique na parte superior) para posicionar o playhead.
4. `S` divide o clipe na posição do playhead; `Delete` remove o selecionado.
5. **Arquivo → Exportar…** (Ctrl+E) para gerar o vídeo final.

---

## Funcionalidades

Para uma lista completa, veja **[FEATURES.md](FEATURES.md)**.

---

## Licença

Pierrot é um software livre: você pode redistribuí-lo e/ou modificá-lo sob os
termos da **GNU General Public License versão 3** ou (a seu critério) qualquer
versão posterior, conforme publicado pela Free Software Foundation.

- Texto completo da licença: [LICENSE](LICENSE) (GPL-3.0).
- Copyright (C) 2026 **theinkspoty**.
