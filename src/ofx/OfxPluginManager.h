// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Suporte a plugins OFX (OpenFX) — gerenciador de plugins.
// Estruturas de dados (OfxPluginInfo, OfxParam, OfxPluginInstance) estão
// definidas em models/Project.h para uso em todo o projeto.

#pragma once

#include <QString>
#include <QVector>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include "models/Project.h"

// Forward declaration do tipo de entry point OFX
#include <ofxCore.h>

struct OfxPluginLib {
    void* handle = nullptr;       // dlopen handle
    OfxPluginEntryPoint* entry = nullptr; // OfxGetPlugin entry point
    int pluginCount = 0;
};

// Metadata completa de um parâmetro OFX (extraída durante o describe).
struct OfxParamDefInfo {
    QString name;               // nome interno do parâmetro
    QString type;               // kOfxParamTypeDouble, etc.
    QString label;              // nome legível
    QString hint;               // dica/descrição do parâmetro
    QString parent;             // nome do group/page pai (vazio = raiz)
    double minVal = -99999.0;   // kOfxParamPropMin
    double maxVal = 99999.0;    // kOfxParamPropMax
    double displayMin = 0.0;    // kOfxParamPropDisplayMin
    double displayMax = 0.0;    // kOfxParamPropDisplayMax
    double increment = 0.0;     // kOfxParamPropIncrement
    int digits = 0;             // kOfxParamPropDigits
    bool enabled = true;        // kOfxParamPropEnabled
    bool secret = false;        // kOfxParamPropSecret
    bool animatable = true;     // kOfxParamPropAnimatable
    bool persistant = true;     // kOfxParamPropPersistant
    QStringList choiceOptions;  // kOfxParamPropChoiceOption (para Choice)
    QVariant defaultValue;      // valor padrão
    // Para Double2D/Integer2D/Double3D/Integer3D
    int dimension = 1;
};

class OfxPluginManager : public QObject {
    Q_OBJECT
public:
    explicit OfxPluginManager(QObject* parent = nullptr);
    ~OfxPluginManager() override;

    int scanPlugins();

    const QVector<OfxPluginInfo>& plugins() const { return m_plugins; }
    const OfxPluginInfo* findPlugin(const QString& id) const;

    QStringList searchPaths() const;
    void addSearchPath(const QString& path);
    void clearSearchPaths() { m_extraPaths.clear(); }

    QJsonObject toJson() const;
    void fromJson(const QJsonObject& obj);

    // Acesso ao runtime do plugin (para ofxHost)
    const OfxPluginLib* pluginLib(const QString& pluginId) const;
    void* libraryHandle(const QString& pluginId) const;

    // Callbacks para descrever e extrair parâmetros
    using DescribeCallback = std::function<void(const QString& pluginId,
                                                const QString& name,
                                                const QString& grouping,
                                                const QString& description,
                                                int versionMajor, int versionMinor,
                                                const QVector<OfxParamDefInfo>& params)>;
    void setDescribeCallback(DescribeCallback cb) { m_describeCb = cb; }

signals:
    void pluginLoaded(const QString& pluginId);
    void pluginError(const QString& pluginId, const QString& error);

private:
    QVector<OfxPluginInfo> m_plugins;
    QStringList m_extraPaths;
    QMap<QString, OfxPluginLib> m_libs;  // pluginId -> runtime lib
    DescribeCallback m_describeCb;

    void scanDirectory(const QString& dir);
    bool loadPluginBundle(const QString& bundlePath);
    bool loadOfxLibrary(const QString& libPath, const QString& pluginId);
    void describePlugin(OfxPluginInfo& info);
    void cleanupLibs();

    // kOfxPropIcon traz um caminho RELATIVO à pasta Resources do bundle.
    // Resolve para um caminho absoluto existente (svg/png) ou vazio.
    QString resolveIconPath(const QString& libPath, const QString& relPath) const;
};
