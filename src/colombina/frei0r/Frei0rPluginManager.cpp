// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "Frei0rPluginManager.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QDebug>

#include <algorithm>
#include <dlfcn.h>
#include <vector>

namespace {

void* f0rLoad(const QString& path) {
    return dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL);
}

void* f0rSym(void* lib, const char* name) {
    return dlsym(lib, name);
}

QString f0rErr() {
    const char* e = dlerror();
    return e ? QString::fromLatin1(e) : QStringLiteral("unknown");
}

QString cstr(const char* s) {
    return s ? QString::fromUtf8(s) : QString();
}

// 0xAABBGGRR (frei0r packed) → QColor
QColor fromF0rColor(quint32 c) {
    return QColor(int(c & 0xFF), int((c >> 8) & 0xFF), int((c >> 16) & 0xFF),
                  int((c >> 24) & 0xFF));
}

quint32 toF0rColor(const QColor& col) {
    return (quint32(col.alpha()) << 24) | (quint32(col.blue()) << 16)
         | (quint32(col.green()) << 8) | quint32(col.red());
}

// Metadados de posição: cada POSITION ocupa DOIS slots em Frei0rEffect::values
// (x, y). `paramSlot[i]` = índice inicial em values para o parâmetro i.
struct ParamLayout {
    QVector<int> slot;          // por índice de parâmetro do plugin
    QVector<int> nSlots;        // 1 (bool/double/color) ou 2 (position)
    int totalSlots = 0;
};

ParamLayout layoutFor(const Frei0rPluginInfo& info) {
    ParamLayout L;
    int cursor = 0;
    for (int p = 0; p < info.paramTypes.size(); ++p) {
        const int type = info.paramTypes.value(p, F0R_PARAM_DOUBLE);
        const int n = (type == F0R_PARAM_POSITION) ? 2 : 1;
        L.slot.append(cursor);
        L.nSlots.append(n);
        cursor += n;
    }
    L.totalSlots = cursor;
    return L;
}

} // namespace

Frei0rPluginManager::Frei0rPluginManager(QObject* parent) : QObject(parent) {}

Frei0rPluginManager::~Frei0rPluginManager() { cleanupLibs(); }

void Frei0rPluginManager::cleanupLibs() {
    for (LibEntry& le : m_libs) {
        if (le.handle && le.deinit) le.deinit();
        if (le.handle) dlclose(le.handle);
        le.handle = nullptr;
    }
    m_libs.clear();
    m_plugins.clear();
}

QStringList Frei0rPluginManager::searchPaths() const {
    QStringList paths;
#ifdef Q_OS_UNIX
    paths << QStringLiteral("/usr/lib/frei0r-1")
          << QStringLiteral("/usr/lib64/frei0r-1")
          << QStringLiteral("/usr/local/lib/frei0r-1")
          << QStringLiteral("/usr/lib/x86_64-linux-gnu/frei0r-1")
          << QDir::homePath() + QStringLiteral("/.local/lib/frei0r-1")
          << QDir::homePath() + QStringLiteral("/.frei0r-1");
#endif
    paths << QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                 + QStringLiteral("/frei0r");
    paths << QDir::homePath() + QStringLiteral("/Documentos/pierrot/frei0r")
          << QDir::homePath() + QStringLiteral("/Documents/pierrot/frei0r")
          << QDir::homePath() + QStringLiteral("/frei0r");

    const QByteArray env = qgetenv("PIERROT_FREI0R_PATH");
    if (!env.isEmpty()) {
        for (const QString& ep :
             QString::fromLocal8Bit(env).split(QLatin1Char(':'), Qt::SkipEmptyParts))
            paths << ep;
    }
    for (const QString& extra : m_extraPaths)
        paths << extra;

    const QStringList saved = QSettings().value("frei0rSearchPaths").toStringList();
    for (const QString& sp : saved)
        if (!paths.contains(sp)) paths << sp;
    return paths;
}

void Frei0rPluginManager::addSearchPath(const QString& path) {
    if (!path.isEmpty() && !m_extraPaths.contains(path)) m_extraPaths.append(path);
}

const Frei0rPluginInfo* Frei0rPluginManager::findPlugin(const QString& name) const {
    for (const Frei0rPluginInfo& p : m_plugins)
        if (p.name == name) return &p;
    return nullptr;
}

bool Frei0rPluginManager::loadLibrary(const QString& path) {
    void* handle = f0rLoad(path);
    if (!handle) {
        qDebug() << "[frei0r] dlopen falhou" << path << f0rErr();
        return false;
    }
    LibEntry le;
    le.handle = handle;
    le.path = path;
    le.count = reinterpret_cast<f0r_get_plugin_count_fn>(f0rSym(handle, "f0r_get_plugin_count"));
    le.info = reinterpret_cast<f0r_get_plugin_info_fn>(f0rSym(handle, "f0r_get_plugin_info"));
    le.paramInfo = reinterpret_cast<f0r_get_param_info_fn>(f0rSym(handle, "f0r_get_param_info"));
    le.construct = reinterpret_cast<f0r_construct_fn>(f0rSym(handle, "f0r_construct"));
    le.destruct = reinterpret_cast<f0r_destruct_fn>(f0rSym(handle, "f0r_destruct"));
    le.setParam = reinterpret_cast<f0r_set_param_value_fn>(f0rSym(handle, "f0r_set_param_value"));
    le.getParam = reinterpret_cast<f0r_get_param_value_fn>(f0rSym(handle, "f0r_get_param_value"));
    le.update = reinterpret_cast<f0r_update_fn>(f0rSym(handle, "f0r_update"));
    le.update2 = reinterpret_cast<f0r_update2_fn>(f0rSym(handle, "f0r_update2"));
    le.init = reinterpret_cast<f0r_init_fn>(f0rSym(handle, "f0r_init"));
    le.deinit = reinterpret_cast<f0r_deinit_fn>(f0rSym(handle, "f0r_deinit"));

    if (!le.count || !le.info || !le.construct || !le.destruct || !le.setParam
        || (!le.update && !le.update2)) {
        qDebug() << "[frei0r] símbolos ausentes em" << path;
        dlclose(handle);
        return false;
    }
    if (le.init) le.init();

    const unsigned int n = le.count();
    for (unsigned int i = 0; i < n; ++i) {
        const f0r_plugin_info* pi = le.info(i);
        if (!pi || !pi->name) continue;
        Frei0rPluginInfo fp;
        fp.name = cstr(pi->name);
        fp.label = fp.name;
        fp.author = cstr(pi->author);
        fp.description = cstr(pi->description);
        fp.pluginType = cstr(pi->plugin_type);
        fp.path = path;
        fp.numParams = pi->num_params;
        fp.isFilter = fp.pluginType.isEmpty() || fp.pluginType == QLatin1String("Filter");

        // Defaults: instância 2x2 + get_param_value.
        std::vector<double> dblDef(size_t(std::max(0, pi->num_params)));
        std::vector<quint32> colDef(size_t(std::max(0, pi->num_params)), 0xFFFFFFFFu);
        std::vector<double> posDef(size_t(std::max(0, pi->num_params)) * 2, 0.5);
        f0r_instance_t probe = le.construct(2, 2);
        if (probe) {
            for (int p = 0; p < pi->num_params; ++p) {
                const f0r_param_info* psi = le.paramInfo ? le.paramInfo(pi, p) : nullptr;
                const int type = psi ? psi->type : F0R_PARAM_DOUBLE;
                if (type == F0R_PARAM_DOUBLE) {
                    double v = 0.0;
                    le.getParam(probe, &v, p);
                    dblDef[size_t(p)] = v;
                } else if (type == F0R_PARAM_BOOL) {
                    int v = 0;
                    le.getParam(probe, &v, p);
                    dblDef[size_t(p)] = v ? 1.0 : 0.0;
                } else if (type == F0R_PARAM_POSITION) {
                    double xy[2] = {0.5, 0.5};
                    le.getParam(probe, xy, p);
                    posDef[size_t(p) * 2] = xy[0];
                    posDef[size_t(p) * 2 + 1] = xy[1];
                } else if (type == F0R_PARAM_COLOR) {
                    quint32 c = 0xFFFFFFFFu;
                    le.getParam(probe, &c, p);
                    colDef[size_t(p)] = c;
                }
            }
            le.destruct(probe);
        }

        for (int p = 0; p < pi->num_params; ++p) {
            const f0r_param_info* psi = le.paramInfo ? le.paramInfo(pi, p) : nullptr;
            const QString pn = psi ? cstr(psi->name) : QString();
            const int type = psi ? psi->type : F0R_PARAM_DOUBLE;
            fp.paramNames << (pn.isEmpty() ? QString::number(p) : pn);
            fp.paramHints << (psi ? cstr(psi->explanation) : QString());
            fp.paramTypes << type;
            if (type == F0R_PARAM_POSITION) {
                // Metadados: um slot visual; values usa 2 (x,y) — o layout
                // é calculado em apply/ffmpeg via paramTypes.
                fp.paramDefaults << posDef[size_t(p) * 2];
                fp.paramDefaultColors << QColor(); // y vive no apply
            } else if (type == F0R_PARAM_COLOR) {
                fp.paramDefaults << 0.0;
                fp.paramDefaultColors << fromF0rColor(colDef[size_t(p)]);
            } else {
                fp.paramDefaults << dblDef[size_t(p)];
                fp.paramDefaultColors << QColor();
            }
        }

        bool dup = false;
        for (const Frei0rPluginInfo& ex : m_plugins)
            if (ex.name == fp.name) { dup = true; break; }
        if (!dup) {
            m_plugins.append(fp);
            le.infos.append(fp);
        }
    }
    m_libs.append(le);
    return !le.infos.isEmpty();
}

int Frei0rPluginManager::scanPlugins() {
    cleanupLibs();
    QSet<QString> seen;
    for (const QString& root : searchPaths()) {
        QDir dir(root);
        if (!dir.exists()) continue;
        QDirIterator it(root,
                        QStringList() << QStringLiteral("*.so") << QStringLiteral("*.so.*"),
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString canon = QFileInfo(path).canonicalFilePath();
            if (canon.isEmpty() || seen.contains(canon)) continue;
            seen.insert(canon);
            loadLibrary(canon);
        }
    }
    std::sort(m_plugins.begin(), m_plugins.end(),
              [](const Frei0rPluginInfo& a, const Frei0rPluginInfo& b) {
                  return a.label.localeAwareCompare(b.label) < 0;
              });
    qInfo() << "[frei0r]" << m_plugins.size() << "plugin(s) em" << m_libs.size()
            << "biblioteca(s)";
    return int(m_plugins.size());
}

const Frei0rPluginManager::LibEntry*
Frei0rPluginManager::libForPlugin(const QString& pluginName) const {
    for (const LibEntry& le : m_libs) {
        for (const Frei0rPluginInfo& p : le.infos)
            if (p.name == pluginName) return &le;
    }
    return nullptr;
}

QImage Frei0rPluginManager::applyEffects(const QImage& input,
                                         const QVector<Frei0rEffect>& effects,
                                         Frei0rPluginManager* manager,
                                         double time) {
    if (input.isNull() || effects.isEmpty() || !manager) return input;

    QImage img = input.convertToFormat(QImage::Format_ARGB32);
    for (const Frei0rEffect& fx : effects) {
        if (!fx.enabled) continue;
        const Frei0rPluginInfo* info = manager->findPlugin(fx.pluginName);
        if (!info || !info->isFilter) continue;
        const LibEntry* le = manager->libForPlugin(fx.pluginName);
        if (!le) continue;

        const int w = img.width();
        const int h = img.height();
        if (w <= 0 || h <= 0) continue;

        f0r_instance_t inst = le->construct(unsigned(w), unsigned(h));
        if (!inst) continue;

        const ParamLayout L = layoutFor(*info);
        int ci = 0; // índice em fx.colors (paralelo aos COLOR params)
        for (int p = 0; p < info->paramTypes.size(); ++p) {
            const int type = info->paramTypes.value(p, F0R_PARAM_DOUBLE);
            const int slot = L.slot.value(p, 0);
            if (type == F0R_PARAM_DOUBLE || type == F0R_PARAM_BOOL) {
                double v = fx.values.value(slot, info->paramDefaults.value(p, 0.0));
                if (type == F0R_PARAM_BOOL) {
                    int b = v != 0.0 ? 1 : 0;
                    le->setParam(inst, &b, p);
                } else {
                    le->setParam(inst, &v, p);
                }
            } else if (type == F0R_PARAM_POSITION) {
                double pos[2];
                pos[0] = fx.values.value(slot, 0.5);
                pos[1] = fx.values.value(slot + 1, 0.5);
                le->setParam(inst, pos, p);
            } else if (type == F0R_PARAM_COLOR) {
                QColor col = fx.colors.value(ci, info->paramDefaultColors.value(p));
                if (!col.isValid()) col = QColor(255, 255, 255);
                quint32 c = toF0rColor(col);
                le->setParam(inst, &c, p);
                ++ci;
            }
        }

        std::vector<quint32> in(size_t(w) * size_t(h));
        std::vector<quint32> out(size_t(w) * size_t(h));
        for (int y = 0; y < h; ++y) {
            const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
            quint32* dst = in.data() + size_t(y) * size_t(w);
            for (int x = 0; x < w; ++x) {
                const QRgb px = line[x];
                dst[x] = (quint32(qAlpha(px)) << 24) | (quint32(qBlue(px)) << 16)
                       | (quint32(qGreen(px)) << 8) | quint32(qRed(px));
            }
        }

        if (le->update)
            le->update(inst, time, in.data(), out.data());
        else if (le->update2)
            le->update2(inst, time, in.data(), nullptr, nullptr, out.data());
        else {
            le->destruct(inst);
            continue;
        }

        QImage result(w, h, QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(result.scanLine(y));
            const quint32* src = out.data() + size_t(y) * size_t(w);
            for (int x = 0; x < w; ++x) {
                const quint32 c = src[x];
                line[x] = qRgba(int(c & 0xFF), int((c >> 8) & 0xFF),
                                int((c >> 16) & 0xFF), int((c >> 24) & 0xFF));
            }
        }
        le->destruct(inst);
        img = result;
    }
    return img;
}

QString Frei0rPluginManager::ffmpegFilterFor(const Frei0rEffect& fx,
                                             const Frei0rPluginManager* manager) {
    if (!fx.enabled || !manager) return QString();
    const Frei0rPluginInfo* info = manager->findPlugin(fx.pluginName);
    if (!info || !info->isFilter) return QString();

    const ParamLayout L = layoutFor(*info);
    QString s = QStringLiteral("frei0r=") + fx.pluginName;
    int ci = 0;
    for (int p = 0; p < info->paramTypes.size(); ++p) {
        const int type = info->paramTypes.value(p, F0R_PARAM_DOUBLE);
        const QString pname = info->paramNames.value(p, QString::number(p));
        const int slot = L.slot.value(p, 0);
        if (type == F0R_PARAM_DOUBLE || type == F0R_PARAM_BOOL) {
            const double v = fx.values.value(slot, info->paramDefaults.value(p, 0.0));
            s += QStringLiteral(":%1=%2").arg(pname).arg(v, 0, 'g', 8);
        } else if (type == F0R_PARAM_POSITION) {
            const double x = fx.values.value(slot, 0.5);
            const double y = fx.values.value(slot + 1, 0.5);
            s += QStringLiteral(":%1=%2,%3")
                     .arg(pname)
                     .arg(x, 0, 'g', 8)
                     .arg(y, 0, 'g', 8);
        } else if (type == F0R_PARAM_COLOR) {
            QColor col = fx.colors.value(ci, info->paramDefaultColors.value(p));
            if (!col.isValid()) col = QColor(255, 255, 255);
            s += QStringLiteral(":%1=0x%2%3%4")
                     .arg(pname)
                     .arg(col.red(), 2, 16, QLatin1Char('0'))
                     .arg(col.green(), 2, 16, QLatin1Char('0'))
                     .arg(col.blue(), 2, 16, QLatin1Char('0'));
            ++ci;
        }
    }
    return s;
}
