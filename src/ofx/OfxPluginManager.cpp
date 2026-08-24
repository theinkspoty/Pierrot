// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "OfxPluginManager.h"
#include "OfxHost.h"

#include <QDir>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QCoreApplication>
#include <QSettings>
#include <QDirIterator>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// ── Helpers para dlopen/dlsym ────────────────────────────────────────────

static void* ofxLoadLibrary(const QString& path) {
#ifdef Q_OS_WIN
    return (void*)LoadLibraryW(reinterpret_cast<const wchar_t*>(path.utf16()));
#else
    return dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
#endif
}

static void* ofxGetSymbol(void* lib, const char* name) {
#ifdef Q_OS_WIN
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

static QString ofxLoadError() {
#ifdef Q_OS_WIN
    return QString::number(GetLastError());
#else
    const char* err = dlerror();
    return err ? QString::fromLatin1(err) : QStringLiteral("unknown");
#endif
}

// ── Construtor / Destrutor ───────────────────────────────────────────────

OfxPluginManager::OfxPluginManager(QObject* parent) : QObject(parent) {}
OfxPluginManager::~OfxPluginManager() { cleanupLibs(); }

void OfxPluginManager::cleanupLibs() {
    for (auto it = m_libs.begin(); it != m_libs.end(); ++it) {
        if (it.value().handle) {
#ifdef Q_OS_WIN
            FreeLibrary((HMODULE)it.value().handle);
#else
            dlclose(it.value().handle);
#endif
        }
    }
    m_libs.clear();
}

// ── Caminhos de busca padrão ────────────────────────────────────────────

QStringList OfxPluginManager::searchPaths() const
{
    QStringList paths;

#ifdef Q_OS_UNIX
    paths << QStringLiteral("/usr/lib/ofx")
          << QStringLiteral("/usr/share/ofx")
          << QStringLiteral("/usr/local/lib/ofx")
          << QStringLiteral("/usr/lib64/ofx")
          << QDir::homePath() + QStringLiteral("/.local/lib/ofx");
#endif
#ifdef Q_OS_WIN
    const QString progFiles = QCoreApplication::applicationDirPath();
    paths << progFiles + QStringLiteral("/../lib/ofx")
          << QStringLiteral("C:/Program Files/OFX/Plugins")
          << QStringLiteral("C:/Program Files (x86)/OFX/Plugins");
#endif

    const QString userDir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/ofx");
    paths << userDir;

    const QString envPath = qgetenv("PIERROT_OFX_PATH");
    if (!envPath.isEmpty()) {
        for (const QString& ep : envPath.split(QLatin1Char(':'), Qt::SkipEmptyParts))
            paths << ep;
    }

    for (const QString& extra : m_extraPaths)
        paths << extra;

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
    cleanupLibs();
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
    // Busca recursiva por diretórios .ofx e por arquivos .of/.ofx.bundle
    QDirIterator it(dir, QStringList() << "*.ofx" << "*.so" << "*.dylib" << "*.dll",
                    QDir::Files | QDir::Dirs, QDirIterator::Subdirectories);

    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();

        if (fi.isDir() && fi.fileName().endsWith(".ofx")) {
            // Bundle OFX: procurar binário dentro
            loadPluginBundle(fi.absoluteFilePath());
        } else if (fi.isFile()) {
            // Arquivo compartilhado direto (.of, .so, .dylib, .dll)
            loadOfxLibrary(fi.absoluteFilePath(),
                           fi.completeBaseName());
        }
    }

    // Também procura por estrutura .ofx/bin/ ou .ofx/Contents/
    // Suporta .ofx e .ofx.bundle (convenção Natron)
    QDir d(dir);
    const QFileInfoList entries = d.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& entry : entries) {
        const QString fn = entry.fileName();
        if (!fn.endsWith(".ofx") && !fn.endsWith(".ofx.bundle")) continue;
        const QString bundlePath = entry.absoluteFilePath();
        // Extrai ID do plugin: "NtscRs.ofx" -> "NtscRs", "X.ofx.bundle" -> "X"
        const QString pluginId = fn.endsWith(".ofx.bundle")
            ? QFileInfo(fn.left(fn.size() - 8)).fileName()
            : QFileInfo(bundlePath).completeBaseName();
        // Procura binário em subdiretórios
        for (const QString& subDir : {"bin", "lib", "Contents", "Contents/MacOS", "Contents/Linux-x86-64"}) {
            QDir sub(bundlePath + "/" + subDir);
            if (!sub.exists()) continue;
            const QFileInfoList bins = sub.entryInfoList(
                QStringList() << "*.so" << "*.dylib" << "*.dll" << "*.of" << "*.ofx",
                QDir::Files);
            for (const QFileInfo& bin : bins) {
                loadOfxLibrary(bin.absoluteFilePath(), pluginId);
            }
        }
    }
}

bool OfxPluginManager::loadPluginBundle(const QString& bundlePath)
{
    // Procura o binário compartilhado dentro do bundle
    static const QStringList searchSubDirs = {
        "", "bin", "lib",
        "Contents", "Contents/MacOS", "Contents/Linux-x86-64",
        "Contents/Win64"
    };
    static const QStringList libPatterns = {
#ifdef Q_OS_WIN
        "*.dll"
#elif defined(Q_OS_MAC)
        "*.dylib", "*.bundle"
#else
        "*.so", "*.ofx"
#endif
    };

    for (const QString& sub : searchSubDirs) {
        QDir d(bundlePath + (sub.isEmpty() ? "" : "/" + sub));
        if (!d.exists()) continue;
        const QFileInfoList files = d.entryInfoList(libPatterns, QDir::Files);
        for (const QFileInfo& fi : files) {
            return loadOfxLibrary(fi.absoluteFilePath(),
                                  QFileInfo(bundlePath).completeBaseName());
        }
    }

    qWarning() << "[OFX] Bundle sem binário:" << bundlePath;
    return false;
}

bool OfxPluginManager::loadOfxLibrary(const QString& libPath, const QString& fallbackId)
{
    qInfo() << "[OFX] Tentando carregar:" << libPath;

    void* lib = ofxLoadLibrary(libPath);
    if (!lib) {
        const QString err = ofxLoadError();
        qWarning() << "[OFX] Falha ao carregar" << libPath << ":" << err;
        emit pluginError(fallbackId, err);
        return false;
    }

    // Procura a função de entrada: OfxGetPlugin (plugin único) ou OfxGetNumberOfPlugins + OfxGetPlugin
    auto* getPlugin = reinterpret_cast<OfxPlugin*(*)(int)>(
        ofxGetSymbol(lib, "OfxGetPlugin"));
    auto* getNumPlugins = reinterpret_cast<int(*)()>(
        ofxGetSymbol(lib, "OfxGetNumberOfPlugins"));

    int numPlugins = 1;
    if (getNumPlugins) {
        numPlugins = getNumPlugins();
        if (numPlugins <= 0) {
            qWarning() << "[OFX] Plugin relata 0 plugins:" << libPath;
#ifdef Q_OS_WIN
            FreeLibrary((HMODULE)lib);
#else
            dlclose(lib);
#endif
            return false;
        }
    }

    if (!getPlugin) {
        qWarning() << "[OFX] Não encontrou OfxGetPlugin:" << libPath;
#ifdef Q_OS_WIN
        FreeLibrary((HMODULE)lib);
#else
        dlclose(lib);
#endif
        return false;
    }

    bool anyLoaded = false;
    for (int i = 0; i < numPlugins; ++i) {
        OfxPlugin* plugin = getPlugin(i);
        if (!plugin) continue;

        // Verifica API
        if (std::strcmp(plugin->pluginApi, kOfxImageEffectPluginApi) != 0) {
            qInfo() << "[OFX] Plugin" << i << "não é ImageEffect, ignorando (api:" << plugin->pluginApi << ")";
            continue;
        }

        // Extrai metadados básicos do OfxPlugin struct
        OfxPluginInfo info;
        info.id = plugin->pluginIdentifier
                  ? QString::fromLatin1(plugin->pluginIdentifier) : fallbackId;
        info.name = info.id; // será refinado no describe
        info.pluginPath = libPath;
        info.versionMajor = plugin->pluginVersionMajor;
        info.versionMinor = plugin->pluginVersionMinor;

        if (findPlugin(info.id)) {
            qInfo() << "[OFX] Plugin duplicado, ignorando:" << info.id;
            continue;
        }

        // Chama setHost
        OfxHostImpl& host = OfxHostImpl::instance();
        if (plugin->setHost)
            plugin->setHost(host.cHost());

        // Armazena runtime info
        OfxPluginLib libInfo;
        libInfo.handle = lib;
        libInfo.entry = plugin->mainEntry;
        libInfo.pluginCount = numPlugins;

        // Describe para extrair nome, grouping, descrição e parâmetros
        OfxEffectInstance tempInst;
        tempInst.pluginId = info.id;
        host.initPlugin(tempInst, lib, plugin->mainEntry, info.id);
        if (host.describe(tempInst)) {
            // Extrai dados do describe — label e grouping são propriedades padrão OFX
            char* plabel = nullptr;
            char* pgrouping = nullptr;
            tempInst.imageEffectProps.getString(kOfxPropLabel, 0,
                                                (const char*&)plabel);
            tempInst.imageEffectProps.getString(kOfxImageEffectPluginPropGrouping, 0,
                                                (const char*&)pgrouping);

            if (plabel && std::strlen(plabel) > 0)
                info.name = QString::fromLatin1(plabel);
            if (pgrouping && std::strlen(pgrouping) > 0)
                info.grouping = QString::fromLatin1(pgrouping);

            // Coleta parâmetros com tipo e label
            QVector<QPair<QString,QPair<QString,QString>>> paramList;
            for (auto& pd : tempInst.paramDefs) {
                // Copia props do storage temporário (preenchido pelo plugin)
                if (pd.tempStorage) {
                    pd.props.m_props = reinterpret_cast<OfxPropSet*>(pd.tempStorage)->m_props;
                    delete reinterpret_cast<OfxPropSet*>(pd.tempStorage);
                    pd.tempStorage = nullptr;
                }
                QString label = pd.props.getStringQt(kOfxPropLabel, 0, pd.name);
                paramList.append({pd.name, {pd.type, label}});
            }

            // Notifica callback
            if (m_describeCb)
                m_describeCb(info.id, info.name, info.grouping, info.description,
                             info.versionMajor, info.versionMinor, paramList);
        }

        // Limpa tempStorage mesmo se describe falhou
        for (auto& pd : tempInst.paramDefs) {
            if (pd.tempStorage) {
                delete reinterpret_cast<OfxPropSet*>(pd.tempStorage);
                pd.tempStorage = nullptr;
            }
        }

        m_libs[info.id] = libInfo;
        m_plugins.append(info);
        anyLoaded = true;

        qInfo() << "[OFX] Plugin carregado:" << info.id
                << "v" << info.versionMajor << "." << info.versionMinor
                << (info.name != info.id ? ("(" + info.name + ")") : "");
        emit pluginLoaded(info.id);
    }

    if (!anyLoaded) {
#ifdef Q_OS_WIN
        FreeLibrary((HMODULE)lib);
#else
        dlclose(lib);
#endif
    }

    return anyLoaded;
}

void OfxPluginManager::describePlugin(OfxPluginInfo& info) {
    // Chamado pelo loadOfxLibrary — já integrado acima
    Q_UNUSED(info);
}

// ── Busca por id ─────────────────────────────────────────────────────────

const OfxPluginInfo* OfxPluginManager::findPlugin(const QString& id) const
{
    for (const OfxPluginInfo& p : m_plugins)
        if (p.id == id) return &p;
    return nullptr;
}

const OfxPluginLib* OfxPluginManager::pluginLib(const QString& pluginId) const {
    auto it = m_libs.find(pluginId);
    return (it != m_libs.end()) ? &it.value() : nullptr;
}

void* OfxPluginManager::libraryHandle(const QString& pluginId) const {
    const auto* lib = pluginLib(pluginId);
    return lib ? lib->handle : nullptr;
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
