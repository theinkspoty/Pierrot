// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "OfxRenderer.h"
#include "OfxHost.h"
#include "OfxPluginManager.h"
#include "models/Project.h"

#include <QDebug>

// Cache estático de instâncias OFX por pluginId.
QHash<QString, OfxEffectInstance*> OfxRenderer::s_instanceCache;

void OfxRenderer::clearCache()
{
    OfxHostImpl& host = OfxHostImpl::instance();
    for (auto it = s_instanceCache.begin(); it != s_instanceCache.end(); ++it) {
        OfxEffectInstance* inst = it.value();
        if (inst) {
            host.destroyInstance(*inst);
            delete inst;
        }
    }
    s_instanceCache.clear();
}

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

    // Tenta reutilizar instância do cache
    OfxEffectInstance* inst = nullptr;
    auto cacheIt = s_instanceCache.find(effect.pluginId);
    if (cacheIt != s_instanceCache.end()) {
        inst = cacheIt.value();
        qInfo() << "[OFX] Reutilizando instância cacheada para" << effect.pluginId
                << "- clips:" << inst->clips.keys()
                << "- params:" << inst->params.size();
    } else {
        // Cria nova instância e cacheia
        inst = new OfxEffectInstance;
        host.initPlugin(*inst, lib->handle, lib->entry, effect.pluginId);

        // Describe (necessário para criar a instância)
        qInfo() << "[OFX] Chamando describe para" << effect.pluginId;
        if (!host.describe(*inst)) {
            qWarning() << "[OFX] Describe falhou para" << effect.pluginId;
            delete inst;
            return input;
        }
        qInfo() << "[OFX] Describe OK - clips:" << inst->clips.keys()
                << "- paramDefs:" << inst->paramDefs.size();

        // Cria instância
        qInfo() << "[OFX] Chamando createInstance para" << effect.pluginId;
        if (!host.createInstance(*inst)) {
            qWarning() << "[OFX] createInstance falhou para" << effect.pluginId;
            delete inst;
            return input;
        }
        qInfo() << "[OFX] createInstance OK - clips:" << inst->clips.keys()
                << "- params:" << inst->params.size();

        s_instanceCache[effect.pluginId] = inst;
    }

    // Atualiza parâmetros na instância cacheada
    for (const OfxParam& p : effect.params) {
        for (auto& iparam : inst->params) {
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
    bool ok = host.render(*inst, input, output, time, input.width(), input.height());

    if (!ok || output.isNull()) {
        qWarning() << "[OFX] Render falhou para" << effect.pluginId;
        return input;
    }

    // Converte de volta para ARGB32_Premultiplied (formato nativo do preview)
    return output.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
