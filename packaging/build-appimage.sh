#!/usr/bin/env bash
#
# Gera um AppImage do Pierrot usando linuxdeploy.
#
# Os plugins do Qt (platforms/xcb+wayland, imageformats, iconengines e o
# multimídia — obrigatório para o áudio do preview) são copiados manualmente
# para o AppDir (a partir do qmake do sistema) e suas dependências são
# empacotadas com --deploy-deps-only. Isso evita depender do
# linuxdeploy-plugin-qt, que pode falhar silenciosamente com Qt6 recentes.
#
# Uso:
#   packaging/build-appimage.sh
#
# Variáveis de ambiente:
#   VERSION         versão usada no nome do AppImage (padrão: 0.3.5)
#   OUT_DIR         pasta de saída (padrão: dist/)
#   BUNDLE_FFMPEG   0 desabilita embutir o binário ffmpeg (padrão: 1)
#   UPDATE_INFORMATION  string de atualização embutida no AppImage (AppImageUpdate).
#                       Ex.: gh-releases-zsync|usuario|repo|latest|Pierrot-*-x86_64.AppImage.zsync
#
# Pré-requisitos: cmake, g++, pkg-config, Qt (dev), libavformat/avcodec/avutil/
# swscale (dev) e curl ou wget. O ffmpeg embutido fornece a exportação sem
# depender de instalação no sistema.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="$(uname -m)"
TOOLS="$ROOT/packaging/.tools"
VERSION="${VERSION:-0.3.5}"
OUT_DIR="${OUT_DIR:-$ROOT/dist}"
BUNDLE_FFMPEG="${BUNDLE_FFMPEG:-1}"

case "$ARCH" in
    x86_64) LD_NAME="linuxdeploy-x86_64.AppImage" ;;
    aarch64) LD_NAME="linuxdeploy-aarch64.AppImage" ;;
    *)
        echo "Erro: arquitetura não suportada: $ARCH" >&2
        exit 1
        ;;
esac

mkdir -p "$TOOLS" "$OUT_DIR"
LD_TOOL="$TOOLS/$LD_NAME"

fetch() {
    local url="$1" out="$2"
    if [ -x "$out" ]; then
        echo "Ferramenta já presente: $out"
        return 0
    fi
    echo "Baixando $url"
    if command -v curl >/dev/null 2>&1; then
        curl -L --fail --silent --show-error --output "$out" "$url"
    else
        wget -q -O "$out" "$url"
    fi
    chmod +x "$out"
}

fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/$LD_NAME" "$LD_TOOL"

QMAKE_BIN="$(command -v qmake6 || command -v qmake || true)"
if [ -z "$QMAKE_BIN" ]; then
    echo "Erro: qmake não encontrado. Instale o Qt de desenvolvimento" >&2
    echo "      (qt6-base-dev ou qtbase5-dev, conforme o README)." >&2
    exit 1
fi

QT_PLUGIN_DIR="$("$QMAKE_BIN" -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
if [ -z "$QT_PLUGIN_DIR" ] || [ ! -d "$QT_PLUGIN_DIR" ]; then
    echo "Erro: não foi possível localizar os plugins do Qt via qmake." >&2
    exit 1
fi

BUILD_DIR="$ROOT/build-appimage"
APPDIR="$BUILD_DIR/AppDir"

echo "==> Compilando..."
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "==> Instalando em AppDir..."
rm -rf "$APPDIR"
cmake --install "$BUILD_DIR" --prefix "$APPDIR/usr"

echo "==> Copiando plugins do Qt para o AppDir..."
mkdir -p "$APPDIR/usr/plugins"
# Além de platforms/imageformats/iconengines, copiamos também o multimídia
# (Qt6: ffmpegmediaplugin + backends de áudio em multimedia/audio; Qt5:
# audio/mediaservice). Sem ele o QAudioSink/QAudioOutput do preview não
# encontra backend de saída e o áudio fica mudo dentro do AppImage.
for d in platforms imageformats iconengines multimedia audio mediaservice; do
    if [ -d "$QT_PLUGIN_DIR/$d" ]; then
        cp -a "$QT_PLUGIN_DIR/$d" "$APPDIR/usr/plugins/"
    fi
done
cat > "$APPDIR/usr/bin/qt.conf" <<'EOF'
[Paths]
Plugins = ../plugins
EOF

LD_ARGS=(--verbosity=0 --appdir "$APPDIR" --output appimage)
for d in platforms imageformats iconengines multimedia audio mediaservice; do
    if [ -d "$APPDIR/usr/plugins/$d" ]; then
        LD_ARGS+=(--deploy-deps-only "$APPDIR/usr/plugins/$d")
    fi
done
# Qt6: os backends de áudio (libqt_pulse/pipewire/alsa_audio_plugin.so) ficam
# em multimedia/audio e precisam de libpulse/pipewire/asound — empacotar as
# dependências deles garante que o sink de fato produza som.
if [ -d "$APPDIR/usr/plugins/multimedia/audio" ]; then
    LD_ARGS+=(--deploy-deps-only "$APPDIR/usr/plugins/multimedia/audio")
fi

if [ "$BUNDLE_FFMPEG" = "1" ]; then
    FFMPEG_BIN="$(command -v ffmpeg || true)"
    if [ -n "$FFMPEG_BIN" ]; then
        echo "==> Embutindo ffmpeg ($FFMPEG_BIN)."
        LD_ARGS+=(--executable "$FFMPEG_BIN")
    else
        echo "Aviso: ffmpeg não encontrado no PATH — AppImage sem ffmpeg embutido." >&2
        echo "       A exportação exigirá ffmpeg instalado no sistema." >&2
    fi
fi

export VERSION
export APPIMAGE_EXTRACT_AND_RUN=1

if [ -n "${UPDATE_INFORMATION:-}" ]; then
    export UPDATE_INFORMATION
    echo "==> AppImage com auto-atualização: $UPDATE_INFORMATION"
fi

run_tool() {
    if "$@"; then return 0; fi
    local first="$1"
    shift
    echo "==> Sem FUSE disponível; usando --appimage-extract-and-run..."
    "$first" --appimage-extract-and-run "$@"
}

echo "==> Empacotando com linuxdeploy..."
run_tool "$LD_TOOL" "${LD_ARGS[@]}"

PRODUCED="$(ls -t "$BUILD_DIR"/*.AppImage "$ROOT"/*.AppImage 2>/dev/null | head -n 1 || true)"
if [ -n "$PRODUCED" ]; then
    FINAL="$OUT_DIR/Pierrot-$VERSION-$ARCH.AppImage"
    mv "$PRODUCED" "$FINAL"
    echo "==> OK: $FINAL"
    ZSYNC="$(ls -t "$BUILD_DIR"/*.zsync "$ROOT"/*.zsync 2>/dev/null | head -n 1 || true)"
    if [ -n "$ZSYNC" ]; then
        mv "$ZSYNC" "$OUT_DIR/Pierrot-$VERSION-$ARCH.AppImage.zsync"
        echo "==> OK: $OUT_DIR/Pierrot-$VERSION-$ARCH.AppImage.zsync"
    fi
else
    echo "Erro: AppImage não foi gerado." >&2
    exit 1
fi
