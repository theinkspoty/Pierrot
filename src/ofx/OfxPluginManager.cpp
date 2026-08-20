// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "OfxPluginManager.h"

#include <QDir>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QCoreApplication>
#include <QSettings>

// ── Construtor / Destrutor ───────────────────────────────────────────────

OfxPluginManager::OfxPluginManager(QObject* parent) : QObject(parent) {}
OfxPluginManager::~OfxPluginManager() = default;

// ── Caminhos de busca padrão ────────────────────────────────────────────

QStringList OfxPluginManager::searchPaths() const
{
    QStringList paths;

    // Diretórios do sistema (Linux / macOS)
#ifdef Q_OS_UNIX
    paths << QStringLiteral("/usr/lib/ofx")
          << QStringLiteral("/usr/share/ofx")
          << QStringLiteral("/usr/local/lib/ofx");
#endif
#ifdef Q_OS_WIN
    // Windows: Program Files / Application Data
    const QString progFiles = QCoreApplication::applicationDirPath();
    paths << progFiles + QStringLiteral("/../lib/ofx");
#endif

    // Diretório do usuário (~/.config/pierrot/ofx ou XDG)
    const QString userDir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/ofx");
    paths << userDir;

    // Variável de ambiente PIERROT_OFX_PATH
    const QString envPath = qgetenv("PIERROT_OFX_PATH");
    if (!envPath.isEmpty()) {
        for (const QString& ep : envPath.split(QLatin1Char(':'), Qt::SkipEmptyParts))
            paths << ep;
    }

    // Caminhos extras adicionados pelo usuário (via代码).
    for (const QString& extra : m_extraPaths)
        paths << extra;

    // Caminhos salvos nas configurações (Configurações → Plugins OFX).
    const QStringList savedPaths = QSettings().value("ofxSearchPaths").toStringList();
    for (const QString& sp : savedPaths)
        if (!paths.contains(sp))
            paths << sp;

    return paths;
}

void OfxPluginManager::addSearchPath(const QString& path)
{
    if (!path.isEmpty() && !m_extraPaths.contains(path))
        m_extraPaths.append(path);
}

// ── Scan de plugins ─────────────────────────────────────────────────────

int OfxPluginManager::scanPlugins()
{
    m_plugins.clear();

    const QStringList dirs = searchPaths();
    for (const QString& dir : dirs) {
        const QDir d(dir);
        if (d.exists())
            scanDirectory(dir);
    }

    qInfo() << "[OFX]" << m_plugins.size() << "plugin(s) encontrado(s)";
    return m_plugins.size();
}

void OfxPluginManager::scanDirectory(const QString& dir)
{
    const QDir d(dir);
    const QFileInfoList entries = d.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QFileInfo& entry : entries) {
        const QString path = entry.absoluteFilePath();

        // Bundle OFX: diretório terminando em .ofx (padrão OFX 1.4+)
        if (entry.isDir() && entry.fileName().endsWith(".ofx")) {
            loadPluginBundle(path);
            continue;
        }

        // Também procura recursivamente (1 nível)
        QDir sub(path);
        const QFileInfoList subs = sub.entryInfoList(
            QStringList() << "*.ofx", QDir::Dirs);
        for (const QFileInfo& subEntry : subs)
            loadPluginBundle(subEntry.absoluteFilePath());
    }
}

bool OfxPluginManager::loadPluginBundle(const QString& bundlePath)
{
    // TODO(OFX): Implementar dlopen/dlsym no bundle para chamar OfxGetPlugin
    //   e extrair metadados (id, name, grouping, versão).
    //
    // Por enquanto, registra o bundle como plugin "genérico" para
    // validar a infraestrutura de scan. A integração real com a API OFX
    // será feita quando o host OFX (ofxHost.h/.cpp) estiver pronto.

    OfxPluginInfo info;
    info.id = QFileInfo(bundlePath).completeBaseName();
    info.name = info.id;
    info.pluginPath = bundlePath;

    // Evita duplicatas
    if (findPlugin(info.id))
        return false;

    m_plugins.append(info);
    qInfo() << "[OFX] Plugin registrado:" << info.id << "em" << bundlePath;
    return true;
}

// ── Busca por id ─────────────────────────────────────────────────────────

const OfxPluginInfo* OfxPluginManager::findPlugin(const QString& id) const
{
    for (const OfxPluginInfo& p : m_plugins)
        if (p.id == id) return &p;
    return nullptr;
}

// ── Serialização ─────────────────────────────────────────────────────────

QJsonObject OfxPluginManager::toJson() const
{
    QJsonObject o;
    QJsonArray arr;
    for (const OfxPluginInfo& p : m_plugins) {
        QJsonObject pj;
        pj["id"] = p.id;
        pj["name"] = p.name;
        pj["grouping"] = p.grouping;
        pj["description"] = p.description;
        pj["pluginPath"] = p.pluginPath;
        pj["versionMajor"] = p.versionMajor;
        pj["versionMinor"] = p.versionMinor;
        arr.append(pj);
    }
    o["plugins"] = arr;

    QJsonArray extraArr;
    for (const QString& e : m_extraPaths)
        extraArr.append(e);
    o["extraPaths"] = extraArr;

    return o;
}

void OfxPluginManager::fromJson(const QJsonObject& obj)
{
    m_plugins.clear();
    const QJsonArray arr = obj["plugins"].toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject pj = v.toObject();
        OfxPluginInfo info;
        info.id = pj["id"].toString();
        info.name = pj["name"].toString();
        info.grouping = pj["grouping"].toString();
        info.description = pj["description"].toString();
        info.pluginPath = pj["pluginPath"].toString();
        info.versionMajor = pj["versionMajor"].toInt(1);
        info.versionMinor = pj["versionMinor"].toInt(0);
        m_plugins.append(info);
    }

    m_extraPaths.clear();
    const QJsonArray extraArr = obj["extraPaths"].toArray();
    for (const QJsonValue& v : extraArr)
        m_extraPaths.append(v.toString());
}
