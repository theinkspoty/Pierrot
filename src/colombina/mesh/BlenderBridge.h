// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Ponte Blender CLI: converte .blend em .obj via `blender -b`.
// Requer Blender instalado e alcançável pelo PATH (ou por PIERROT_BLENDER).

#pragma once

#include <QString>

namespace mesh {

// Converte .blend → .obj. Retorna o caminho do .obj gerado, ou QString()
// com `error` preenchido. Timeout padrão 120s (o processo é bloqueante: roda
// na thread da UI, então o app congela durante a conversão).
//
// `outDir` é onde o .obj (e o .mtl/texturas ao lado) são deixados. Vazio
// significa "ao lado do .blend de origem", com fallback para o AppData se o
// diretório não for gravável — o que importa é nunca cair em /tmp, já que
// esse caminho vai para Track::meshPath e precisa durar mais que um reboot.
QString blendToObj(const QString& blendPath, QString* error = nullptr,
                   int timeoutMs = 120000, const QString& outDir = QString());

// true se existe um executável `blender` utilizável.
bool blenderAvailable(QString* resolvedPath = nullptr);

} // namespace mesh
