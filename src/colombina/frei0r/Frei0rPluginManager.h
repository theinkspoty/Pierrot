// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Host frei0r — o padrão de efeitos dos NLEs FOSS (Kdenlive, Shotcut, MLT,
// Flowblade, FFmpeg). Plugins são .so carregados via dlopen (sem linkar
// libfrei0r); a API é a oficial frei0r-1 (include/frei0r.h da dyne/frei0r).

#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "colombina/models/Project.h" // Frei0rEffect

// ── API C oficial frei0r-1 (declarações mínimas; o SDK não é linkado) ────
extern "C" {
typedef void* f0r_instance_t;
typedef unsigned int f0r_color_t; // 0xAABBGGRR (packed)
typedef void* f0r_param_t;

enum F0rParamType {
    F0R_PARAM_BOOL = 0,
    F0R_PARAM_DOUBLE = 1,
    F0R_PARAM_COLOR = 2,
    F0R_PARAM_POSITION = 3
};

struct f0r_plugin_info {
    const char* name;
    const char* author;
    const char* plugin_type;  // "Filter" | "Source" | "Mixer2" | ...
    const char* color_model;  // "RGBA" | "PACKED32"
    const char* description;
    const char* url;
    int num_params;
};

struct f0r_param_info {
    const char* name;
    int type; // F0rParamType
    const char* explanation;
};

typedef int (*f0r_init_fn)(void);
typedef void (*f0r_deinit_fn)(void);
typedef unsigned int (*f0r_get_plugin_count_fn)(void);
typedef const f0r_plugin_info* (*f0r_get_plugin_info_fn)(unsigned int index);
typedef const f0r_param_info* (*f0r_get_param_info_fn)(const f0r_plugin_info* info,
                                                       int param_idx);
typedef f0r_instance_t (*f0r_construct_fn)(unsigned int width, unsigned int height);
typedef void (*f0r_destruct_fn)(f0r_instance_t instance);
typedef void (*f0r_set_param_value_fn)(f0r_instance_t instance, f0r_param_t param,
                                       int param_idx);
typedef void (*f0r_get_param_value_fn)(f0r_instance_t instance, f0r_param_t param,
                                       int param_idx);
typedef void (*f0r_update_fn)(f0r_instance_t instance, double time,
                              const uint32_t* inframe, uint32_t* outframe);
typedef void (*f0r_update2_fn)(f0r_instance_t instance, double time,
                               const uint32_t* inframe1, const uint32_t* inframe2,
                               const uint32_t* inframe3, uint32_t* outframe);
} // extern "C"

// ── Metadados de um plugin frei0r ────────────────────────────────────────
struct Frei0rPluginInfo {
    QString name;        // id estável usado em .Blanc e no ffmpeg (`frei0r=`)
    QString label;       // nome legível
    QString author;
    QString description;
    QString pluginType;  // "Filter", ...
    QString path;        // .so no disco
    int numParams = 0;
    // Nomes e defaults dos parâmetros (paralelos 0..numParams-1).
    QStringList paramNames;
    QStringList paramHints;
    QVector<int> paramTypes;     // F0rParamType
    QVector<double> paramDefaults;
    QVector<QColor> paramDefaultColors;
    bool isFilter = true;        // só Filter entra na pilha de clipe
};

// Frei0rEffect está em Project.h (modelo serializado), ao lado de OfxPluginInstance.

// ── Gerenciador: scan + dlopen + apply ───────────────────────────────────
class Frei0rPluginManager : public QObject {
    Q_OBJECT
public:
    explicit Frei0rPluginManager(QObject* parent = nullptr);
    ~Frei0rPluginManager() override;

    int scanPlugins();
    const QVector<Frei0rPluginInfo>& plugins() const { return m_plugins; }
    const Frei0rPluginInfo* findPlugin(const QString& name) const;

    QStringList searchPaths() const;
    void addSearchPath(const QString& path);

    // Aplica a pilha frei0r de um clipe numa imagem (paridade preview).
    // `time` em segundos (relativo ao clipe ou absoluto — o host usa o valor
    // como o tempo do efeito; plugins estáticos ignoram). Devolve a imagem
    // processada (cópia). Em falha de plugin, pula aquele efeito.
    static QImage applyEffects(const QImage& input,
                               const QVector<Frei0rEffect>& effects,
                               Frei0rPluginManager* manager,
                               double time);

    // String ffmpeg para a exportação (`frei0r=nome:p=v:...`). Vazia se o
    // plugin não existir no manager ou estiver desabilitado.
    static QString ffmpegFilterFor(const Frei0rEffect& fx,
                                   const Frei0rPluginManager* manager);

private:
    struct LibEntry {
        void* handle = nullptr;
        QString path;
        QVector<Frei0rPluginInfo> infos; // plugins desta .so
        f0r_init_fn init = nullptr;
        f0r_deinit_fn deinit = nullptr;
        f0r_get_plugin_count_fn count = nullptr;
        f0r_get_plugin_info_fn info = nullptr;
        f0r_get_param_info_fn paramInfo = nullptr;
        f0r_construct_fn construct = nullptr;
        f0r_destruct_fn destruct = nullptr;
        f0r_set_param_value_fn setParam = nullptr;
        f0r_get_param_value_fn getParam = nullptr;
        f0r_update_fn update = nullptr;
        f0r_update2_fn update2 = nullptr;
        bool inited = false;
    };

    void cleanupLibs();
    bool loadLibrary(const QString& path);
    const LibEntry* libForPlugin(const QString& pluginName) const;
    static QString defaultCMakeHint();

    QVector<Frei0rPluginInfo> m_plugins;
    QVector<LibEntry> m_libs;
    QStringList m_extraPaths;
};
