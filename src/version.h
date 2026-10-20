// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

// Versão exibida no app. Mantenha em sincronia com a variável VERSION do
// packaging/build-appimage.sh (usada no nome do AppImage).
#define PIERROT_VERSION "0.8"

// Grupo de compatibilidade do formato .Blanc (major.minor): apps do MESMO
// grupo abrem os projetos um do outro (0.8, 0.8.1, 0.8.4…). Arquivos de um
// grupo MAIS NOVO são recusados na abertura; de um grupo mais ANTIGO são
// convertidos (one-way, com aviso). Mantenha em sincronia com PIERROT_VERSION
// (o grupo é o major.minor).
#define PIERROT_VERSION_GROUP "0.8"

// Versões dos motores internos, exibidas no Sobre — mantenham atualizadas
// quando mexer em src/colombina/ (kernel) ou src/laartman/ (codecs).
#define PIERROT_COLOMBINA_VERSION "0.1" // kernel: modelo, cache, render, export
#define PIERROT_LAARTMAN_VERSION "0.4"  // motor de codecs: só decode (transição v0.4 concluída)