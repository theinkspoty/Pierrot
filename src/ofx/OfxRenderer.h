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
#include <QHash>

struct OfxPluginInstance;
struct OfxEffectInstance;
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

    // Limpa o cache de instâncias (chamado ao descarregar plugins ou destruir o renderer).
    static void clearCache();

private:
    // Cache de instâncias OFX por pluginId (evita re-criação a cada frame).
    // A chave é o pluginId; o valor é a instância persistente.
    static QHash<QString, OfxEffectInstance*> s_instanceCache;
};
