# -*- rpm-spec -*-
#
# Spec do Pierrot para distros RPM (Fedora/openSUSE/RHEL).
#
# Como o CMakeLists.txt instala o binário, o .desktop e o ícone, o
# empacotamento é só compilar + instalar com DESTDIR. O binário fica
# ligado dinamicamente ao Qt6 e ao FFmpeg do sistema (o AppImage é que
# embute tudo).
#
# Build local (fora do Fedora, sem as BuildRequires resolvidas):
#   rpmbuild -bb --nodeps packaging/pierrot.spec
#
# Build "de verdade" (Fedora/COPR): as BuildRequires abaixo são resolvidas
# pelo mock/dnf, e o tarball tem que estar na fonte:
#   spectool -g -R packaging/pierrot.spec && rpmbuild -ba packaging/pierrot.spec

%global debug_package %{nil}

Name:           pierrot
Version:        0.8
Release:        1%{?dist}
Summary:        Editor de vídeo estilo Vegas Pro para Linux

License:        GPL-3.0-or-later
URL:            https://github.com/theinkspoty/Pierrot
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtmultimedia-devel
BuildRequires:  qt6-qtsvg-devel
BuildRequires:  ffmpeg-devel

Requires:       qt6-qtbase
Requires:       qt6-qtmultimedia
Requires:       qt6-qtsvg
Requires:       ffmpeg

%description
Pierrot é um editor de vídeo não-linear para Linux, com timeline
multi-faixa, preview com áudio, painel de efeitos (frei0r/OFX), Mesa 3D,
multicâmera e exportação via FFmpeg.

Este pacote traz o binário ligado às bibliotecas do sistema. Para uma
versão portátil, sem dependências, use o AppImage.

%prep
%setup -q

%build
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=%{_prefix}
cmake --build build --parallel "$(nproc)"

%install
DESTDIR=%{buildroot} cmake --install build

%files
%{_bindir}/pierrot
%{_datadir}/applications/pierrot.desktop
%{_datadir}/icons/hicolor/scalable/apps/pierrot.svg

%changelog
* Tue Oct 20 2026 theinkspoty <theinkspoty@users.noreply.github.com> - 0.8-1
- Pierrot 0.8: fechamento da transição Laartman v0.4 (motor de codecs só
  decode, PacketReader com contexto próprio, áudio em paralelo), memo de
  probe, budget de memória do cache de quadros, japonês, tema Japan 95,
  48 presets de animação e correções de desempenho.
* Sat Oct 17 2026 theinkspoty <theinkspoty@users.noreply.github.com> - 0.7.1-1
- Pierrot 0.7.1: dock de presets de animação com miniaturas animadas,
  correção do KfSmooth (Catmull-Rom) e do loop de GIF/WebP na timeline.
* Mon Oct 12 2026 theinkspoty <theinkspoty@users.noreply.github.com> - 0.7-1
- Pierrot 0.7: Source Monitor, multicâmera, editor de velocidade, frei0r/OFX.
