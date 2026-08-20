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
#include <QObject>
#include "models/Project.h"

class OfxPluginManager : public QObject {
    Q_OBJECT
public:
    explicit OfxPluginManager(QObject* parent = nullptr);
    ~OfxPluginManager() override;

    // Escaneia os diretórios de plugins e popula m_plugins.
    // Retorna a quantidade de plugins encontrados.
    int scanPlugins();

    // Retorna a lista de plugins descobertos.
    const QVector<OfxPluginInfo>& plugins() const { return m_plugins; }

    // Busca um plugin pelo id. Retorna nullptr se não encontrado.
    const OfxPluginInfo* findPlugin(const QString& id) const;

    // Retorna diretórios onde plugins são procurados.
    QStringList searchPaths() const;

    // Caminhos extras adicionados pelo usuário.
    void addSearchPath(const QString& path);
    void clearSearchPaths() { m_extraPaths.clear(); }

    // Serialização (para salvar config do projeto).
    QJsonObject toJson() const;
    void fromJson(const QJsonObject& obj);

private:
    QVector<OfxPluginInfo> m_plugins;
    QStringList m_extraPaths;

    void scanDirectory(const QString& dir);
    bool loadPluginBundle(const QString& bundlePath);
};
