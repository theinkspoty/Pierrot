// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Renderer OFX — processa imagens através da cadeia de plugins OFX.

#pragma once

#include <QImage>
#include <QVector>
#include <QString>

struct OfxPluginInstance;
class OfxPluginManager;

class OfxRenderer {
public:
    // Processa uma imagem através de todos os efeitos OFX de um clipe.
    // Retorna a imagem processada (ou a original se não houver efeitos).
    static QImage applyOfxEffects(const QImage& input,
                                  const QVector<OfxPluginInstance>& effects,
                                  const OfxPluginManager* manager,
                                  double time = 0.0);

    // Processa um único efeito OFX.
    static QImage applySingleOfx(const QImage& input,
                                 const OfxPluginInstance& effect,
                                 const OfxPluginManager* manager,
                                 double time = 0.0);
};
