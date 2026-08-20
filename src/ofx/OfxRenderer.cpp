// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "OfxRenderer.h"
#include "OfxHost.h"
#include "OfxPluginManager.h"
#include "models/Project.h"

#include <QDebug>

QImage OfxRenderer::applyOfxEffects(const QImage& input,
                                     const QVector<OfxPluginInstance>& effects,
                                     const OfxPluginManager* manager,
                                     double time)
{
    if (input.isNull() || effects.isEmpty() || !manager)
        return input;

    QImage current = input;
    for (const OfxPluginInstance& fx : effects) {
        if (!fx.enabled) continue;
        current = applySingleOfx(current, fx, manager, time);
        if (current.isNull()) return input; // fallback
    }
    return current;
}

QImage OfxRenderer::applySingleOfx(const QImage& input,
                                    const OfxPluginInstance& effect,
                                    const OfxPluginManager* manager,
                                    double time)
{
    if (input.isNull() || !manager) return input;

    const OfxPluginLib* lib = manager->pluginLib(effect.pluginId);
    if (!lib || !lib->entry) {
        qWarning() << "[OFX] Plugin não carregado:" << effect.pluginId;
        return input;
    }

    OfxHostImpl& host = OfxHostImpl::instance();

    // Cria instância temporária do efeito
    OfxEffectInstance inst;
    host.initPlugin(inst, lib->handle, lib->entry, effect.pluginId);

    // Describe (já foi feito no scan, mas precisamos para criar a instância)
    // Na verdade, o describe já extraiu os paramDefs — reutilizamos
    // uma instância "template" se disponível, ou refazemos
    if (!host.describe(inst)) {
        qWarning() << "[OFX] Describe falhou para" << effect.pluginId;
        return input;
    }

    // Cria instância
    host.createInstance(inst);

    // Aplica parâmetros salvos no clipe
    for (const OfxParam& p : effect.params) {
        // Busca o parâmetro na instância
        for (auto& iparam : inst.params) {
            if (iparam.name == p.key) {
                if (p.value.canConvert<double>() &&
                    (iparam.type == kOfxParamTypeDouble || iparam.type == kOfxParamTypeInteger)) {
                    iparam.doubleVal = p.value.toDouble();
                    iparam.intVal = p.value.toInt();
                } else if (p.value.canConvert<bool>() && iparam.type == kOfxParamTypeBoolean) {
                    iparam.boolVal = p.value.toBool();
                } else if (p.value.canConvert<int>() && iparam.type == kOfxParamTypeChoice) {
                    iparam.choiceVal = p.value.toInt();
                } else if (p.value.metaType().id() == QMetaType::QColor &&
                           (iparam.type == kOfxParamTypeRGB || iparam.type == kOfxParamTypeRGBA)) {
                    QColor c = p.value.value<QColor>();
                    iparam.r = c.redF();
                    iparam.g = c.greenF();
                    iparam.b = c.blueF();
                    iparam.a = c.alphaF();
                } else if (p.value.metaType().id() == QMetaType::QString &&
                           iparam.type == kOfxParamTypeString) {
                    iparam.stringVal = p.value.toString();
                }
                break;
            }
        }
    }

    // Renderiza
    QImage output;
    bool ok = host.render(inst, input, output, time, input.width(), input.height());

    host.destroyInstance(inst);

    if (!ok || output.isNull()) {
        qWarning() << "[OFX] Render falhou para" << effect.pluginId;
        return input;
    }

    return output;
}
