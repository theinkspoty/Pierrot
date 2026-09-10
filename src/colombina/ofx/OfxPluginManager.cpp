// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "OfxPluginManager.h"
#include "OfxHost.h"
#include "OfxRenderer.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>
#include <QSettings>
#include <QDirIterator>
#include <cstring>

#include <dlfcn.h>

// ── Helpers para dlopen/dlsym ────────────────────────────────────────────

static void* ofxLoadLibrary(const QString& path) {
    return dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
}

static void* ofxGetSymbol(void* lib, const char* name) {
    return dlsym(lib, name);
}

static QString ofxLoadError() {
    const char* err = dlerror();
    return err ? QString::fromLatin1(err) : QStringLiteral("unknown");
}

// ── Construtor / Destrutor ───────────────────────────────────────────────

OfxPluginManager::OfxPluginManager(QObject* parent) : QObject(parent) {}
OfxPluginManager::~OfxPluginManager() { cleanupLibs(); }

void OfxPluginManager::cleanupLibs() {
    // Limpa cache de instâncias OFX antes de descarregar bibliotecas
    OfxRenderer::clearCache();

    for (auto it = m_libs.begin(); it != m_libs.end(); ++it) {
        if (it.value().handle) {
            dlclose(it.value().handle);
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

    const QString userDir =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/ofx");
    paths << userDir;

    // Caminhos comuns onde usuários instalam plugins OFX
    paths << QDir::homePath() + QStringLiteral("/Documentos/pierrot/ofx")
          << QDir::homePath() + QStringLiteral("/Documents/pierrot/ofx")
          << QDir::homePath() + QStringLiteral("/ofx")
          << QDir::homePath() + QStringLiteral("/.ofx");

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
    QDirIterator it(dir, QStringList() << "*.ofx" << "*.so" << "*.dylib",
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
                QStringList() << "*.so" << "*.dylib" << "*.of" << "*.ofx",
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
        "Contents", "Contents/MacOS", "Contents/Linux-x86-64"
    };
    static const QStringList libPatterns = {
#if defined(Q_OS_MAC)
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
            dlclose(lib);
            return false;
        }
    }

    if (!getPlugin) {
        qWarning() << "[OFX] Não encontrou OfxGetPlugin:" << libPath;
        dlclose(lib);
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
        qInfo() << "[OFX] Chamando describe para" << info.id;
        if (host.describe(tempInst)) {
            qInfo() << "[OFX] Describe OK para" << info.id
                    << "- clips:" << tempInst.clips.keys()
                    << "- paramDefs:" << tempInst.paramDefs.size();
            // Extrai dados do describe — label e grouping são propriedades padrão OFX
            char* plabel = nullptr;
            char* pgrouping = nullptr;
            char* picon = nullptr;
            tempInst.imageEffectProps.getString(kOfxPropLabel, 0,
                                                (const char*&)plabel);
            tempInst.imageEffectProps.getString(kOfxImageEffectPluginPropGrouping, 0,
                                                (const char*&)pgrouping);
            tempInst.imageEffectProps.getString(kOfxPropIcon, 0,
                                                (const char*&)picon);

            if (plabel && std::strlen(plabel) > 0)
                info.name = QString::fromLatin1(plabel);
            if (pgrouping && std::strlen(pgrouping) > 0)
                info.grouping = QString::fromLatin1(pgrouping);
            if (picon && std::strlen(picon) > 0)
                info.iconPath = resolveIconPath(libPath, QString::fromLatin1(picon));
            // Fallback de convenção: procura nomes comuns na pasta Resources
            // caso o plugin não declare kOfxPropIcon.
            if (info.iconPath.isEmpty()) {
                static const char* const iconCandidates[] = {
                    "Icon.png", "icon.png", "Icon.svg", "icon.svg", "Plugin.png"
                };
                for (const char* nm : iconCandidates) {
                    const QString p = resolveIconPath(libPath, QString::fromUtf8(nm));
                    if (!p.isEmpty()) { info.iconPath = p; break; }
                }
            }

            // Coleta parâmetros com metadata completa
            QVector<OfxParamDefInfo> paramList;
            for (auto& pd : tempInst.paramDefs) {
                // Copia props do storage temporário (preenchido pelo plugin)
                if (pd.tempStorage) {
                    pd.props.m_props = reinterpret_cast<OfxPropSet*>(pd.tempStorage)->m_props;
                    delete reinterpret_cast<OfxPropSet*>(pd.tempStorage);
                    pd.tempStorage = nullptr;
                }
                OfxParamDefInfo info;
                info.name = pd.name;
                info.type = pd.type;
                info.label = pd.props.getStringQt(kOfxPropLabel, 0, pd.name);
                info.hint = pd.props.getStringQt(kOfxParamPropHint, 0, QString());
                info.parent = pd.props.getStringQt(kOfxParamPropParent, 0, QString());
                info.enabled = pd.props.getIntVal(kOfxParamPropEnabled, 0, 1) != 0;
                info.secret = pd.props.getIntVal(kOfxParamPropSecret, 0, 0) != 0;
                info.animatable = pd.props.getIntVal(kOfxParamPropAnimates, 0, 1) != 0;
                info.persistant = pd.props.getIntVal(kOfxParamPropPersistant, 0, 1) != 0;

                // Min/Max para tipos numéricos
                if (pd.type == kOfxParamTypeDouble || pd.type == kOfxParamTypeInteger
                    || pd.type == kOfxParamTypeDouble2D || pd.type == kOfxParamTypeInteger2D
                    || pd.type == kOfxParamTypeDouble3D || pd.type == kOfxParamTypeInteger3D) {
                    info.minVal = pd.props.getDoubleVal(kOfxParamPropMin, 0, -99999.0);
                    info.maxVal = pd.props.getDoubleVal(kOfxParamPropMax, 0, 99999.0);
                    info.displayMin = pd.props.getDoubleVal(kOfxParamPropDisplayMin, 0, info.minVal);
                    info.displayMax = pd.props.getDoubleVal(kOfxParamPropDisplayMax, 0, info.maxVal);
                    info.increment = pd.props.getDoubleVal(kOfxParamPropIncrement, 0, 0.0);
                    info.digits = pd.props.getIntVal(kOfxParamPropDigits, 0, 0);
                    // Dimensão para tipos 2D/3D
                    if (pd.type == kOfxParamTypeDouble2D || pd.type == kOfxParamTypeInteger2D)
                        info.dimension = 2;
                    else if (pd.type == kOfxParamTypeDouble3D || pd.type == kOfxParamTypeInteger3D)
                        info.dimension = 3;
                }

                // Default value
                if (pd.type == kOfxParamTypeDouble || pd.type == kOfxParamTypeDouble2D
                    || pd.type == kOfxParamTypeDouble3D) {
                    info.defaultValue = pd.props.getDoubleVal(kOfxParamPropDefault, 0, 0.0);
                } else if (pd.type == kOfxParamTypeInteger || pd.type == kOfxParamTypeInteger2D
                           || pd.type == kOfxParamTypeInteger3D) {
                    info.defaultValue = pd.props.getIntVal(kOfxParamPropDefault, 0, 0);
                } else if (pd.type == kOfxParamTypeBoolean) {
                    info.defaultValue = pd.props.getIntVal(kOfxParamPropDefault, 0, 0) != 0;
                } else if (pd.type == kOfxParamTypeChoice) {
                    info.defaultValue = pd.props.getIntVal(kOfxParamPropDefault, 0, 0);
                    // Extrai opções de choice
                    int dim = pd.props.getDimension(kOfxParamPropChoiceOption);
                    for (int ci = 0; ci < dim; ++ci) {
                        QString opt = pd.props.getStringQt(kOfxParamPropChoiceOption, ci, QString());
                        if (!opt.isEmpty())
                            info.choiceOptions.append(opt);
                    }
                }

                paramList.append(info);
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
        dlclose(lib);
    }

    return anyLoaded;
}

void OfxPluginManager::describePlugin(OfxPluginInfo& info) {
    // Chamado pelo loadOfxLibrary — já integrado acima
    Q_UNUSED(info);
}

QString OfxPluginManager::resolveIconPath(const QString& libPath,
                                           const QString& relPath) const {
    // O ícone OFX é relativo à pasta Resources do bundle. O bundle segue o
    // layout padrão:  <Bundle>.ofx/Contents/[Linux/MacOS]/.../Plugin.ofx
    // com a Resources em <Bundle>.ofx/Contents/Resources.
    QString dir = QFileInfo(libPath).absolutePath();
    QString contentsDir;
    for (;;) {
        if (!dir.isEmpty() && QFileInfo(dir).isDir()
            && QFileInfo(dir).fileName().compare(QStringLiteral("Contents"),
                                                 Qt::CaseInsensitive) == 0) {
            contentsDir = dir;
            break;
        }
        const QString parent = QFileInfo(dir + QLatin1String("/..")).absolutePath();
        if (parent.isEmpty() || parent == dir) break;
        dir = parent;
    }

    QStringList bases;
    if (!contentsDir.isEmpty())
        bases << contentsDir + QStringLiteral("/Resources") << contentsDir;
    bases << QFileInfo(libPath).absolutePath();

    for (const QString& base : bases) {
        QString cand = QFileInfo(relPath).isAbsolute()
                ? relPath
                : QDir(base).filePath(relPath);
        if (QFileInfo(cand).isFile()) return cand;
    }
    return QString();
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
        pj["iconPath"] = p.iconPath;
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
        info.iconPath = pj["iconPath"].toString();
        info.versionMajor = pj["versionMajor"].toInt(1);
        info.versionMinor = pj["versionMinor"].toInt(0);
        m_plugins.append(info);
    }

    m_extraPaths.clear();
    const QJsonArray extraArr = obj["extraPaths"].toArray();
    for (const QJsonValue& v : extraArr)
        m_extraPaths.append(v.toString());
}
