// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Host OFX mínimo — implementa as suites necessárias para carregar
// e executar plugins de imagem OpenFX.

#pragma once

#include <ofxCore.h>
#include <ofxProperty.h>
#include <ofxParam.h>
#include <ofxImageEffect.h>
#include <ofxMemory.h>
#include <ofxMultiThread.h>
#include <ofxMessage.h>
#include <ofxProgress.h>

#include <QString>
#include <QMap>
#include <QVector>
#include <QVariant>
#include <QImage>
#include <QMutex>
#include <functional>

// ── Property Set wrapper ─────────────────────────────────────────────────

class OfxPropSet {
public:
    void setPointer(const char* key, int index, void* value);
    void setString(const char* key, int index, const char* value);
    void setDouble(const char* key, int index, double value);
    void setInt(const char* key, int index, int value);

    bool getPointer(const char* key, int index, void** value) const;
    bool getString(const char* key, int index, const char*& value) const;
    bool getDouble(const char* key, int index, double& value) const;
    bool getInt(const char* key, int index, int& value) const;
    int getDimension(const char* key) const;

    // Helpers para Qt
    QString getStringQt(const char* key, int index = 0, const QString& def = {}) const;
    double getDoubleVal(const char* key, int index = 0, double def = 0.0) const;
    int getIntVal(const char* key, int index = 0, int def = 0) const;

    struct PropEntry {
        QVector<void*>    ptrs;
        QVector<QString>  strings;
        QVector<double>   doubles;
        QVector<int>      ints;
    };
    QMap<QString, PropEntry> m_props;
};

// ── Parameter definition (used during describe) ──────────────────────────

struct OfxParamDef {
    QString name;
    QString type;       // kOfxParamTypeDouble, etc.
    OfxPropSet props;   // propriedades do descritor (label, min, max, default, etc.)
    void* tempStorage = nullptr;  // ponteiro temporário para PropSetStorage (preenchido pelo plugin)
};

// ── Parameter instance (per-effect instance) ─────────────────────────────

struct OfxParamInst {
    QString name;
    QString type;
    // Valores atuais (dependem do tipo)
    double  doubleVal = 0.0;
    int     intVal = 0;
    bool    boolVal = false;
    double  r = 0, g = 0, b = 0, a = 1;  // cores
    int     choiceVal = 0;
    QString stringVal;
};

// ── Clip (input/output) ──────────────────────────────────────────────────

struct OfxClip {
    QString name;
    OfxPropSet props;
    QImage image;  // dados da imagem atribuída pelo host
};

// ── Image Effect instance ────────────────────────────────────────────────

struct OfxEffectInstance {
    void* pluginLib = nullptr;   // handle do dlopen
    OfxPluginEntryPoint* entry = nullptr;
    QString pluginId;

    OfxPropSet props;            // propriedades da instância
    OfxPropSet imageEffectProps; // propriedades do plugin (do describe)

    QVector<OfxParamDef> paramDefs;  // definições (do describeInContext)
    QVector<OfxParamInst> params;    // valores das instâncias

    QMap<QString, OfxClip> clips;    // "Source", "Output", etc.

    // Cache de handles de parâmetros (nome -> handle)
    // Evita criar novos ParamStorage a cada chamada de paramGetHandle
    QMap<QString, OfxParamHandle> paramHandleCache;

    void* privateData = nullptr;     // dados privados do plugin
};

// ── OFX Host ─────────────────────────────────────────────────────────────

class OfxHostImpl {
public:
    OfxHostImpl();
    ~OfxHostImpl();

    // Retorna o singleton (ou crie um estático)
    static OfxHostImpl& instance();

    // Inicializa o host (chama setHost no plugin)
    void initPlugin(OfxEffectInstance& inst, void* libHandle,
                    OfxPluginEntryPoint* entry, const QString& pluginId);

    // Chama a action "describe" no plugin para extrair metadados
    bool describe(OfxEffectInstance& inst);

    // Chama "create instance" no plugin
    bool createInstance(OfxEffectInstance& inst);

    // Renderiza um frame
    bool render(OfxEffectInstance& inst, const QImage& input, QImage& output,
                double time, int width, int height);

    // Destrói uma instância
    void destroyInstance(OfxEffectInstance& inst);

    // Acessa a struct OfxHost C para passar ao plugin
    ::OfxHost* cHost() { return &m_cHost; }

private:
    ::OfxHost m_cHost;

    // Suite implementations
    static const OfxPropertySuiteV1 s_propertySuite;
    static const OfxParameterSuiteV1 s_parameterSuite;
    static const OfxImageEffectSuiteV1 s_imageEffectSuite;
    static const OfxMemorySuiteV1 s_memorySuite;
    static const OfxMultiThreadSuiteV1 s_multiThreadSuite;
    static const OfxMessageSuiteV2 s_messageSuite;
    static const OfxProgressSuiteV1 s_progressSuite;

    // fetchSuite callback
    static const void* fetchSuite(OfxPropertySetHandle host, const char* suiteName, int suiteVersion);

    // Property suite callbacks
    static OfxStatus propSetPointer(OfxPropertySetHandle h, const char* p, int i, void* v);
    static OfxStatus propSetString(OfxPropertySetHandle h, const char* p, int i, const char* v);
    static OfxStatus propSetDouble(OfxPropertySetHandle h, const char* p, int i, double v);
    static OfxStatus propSetInt(OfxPropertySetHandle h, const char* p, int i, int v);
    static OfxStatus propGetPointer(OfxPropertySetHandle h, const char* p, int i, void** v);
    static OfxStatus propGetString(OfxPropertySetHandle h, const char* p, int i, char** v);
    static OfxStatus propGetDouble(OfxPropertySetHandle h, const char* p, int i, double* v);
    static OfxStatus propGetInt(OfxPropertySetHandle h, const char* p, int i, int* v);
    static OfxStatus propGetDimension(OfxPropertySetHandle h, const char* p, int* c);
    static OfxStatus propReset(OfxPropertySetHandle h, const char* p);
    // N-ary setters/getters (stub — delegam ao singular)
    static OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* p, int c, void* const* v);
    static OfxStatus propSetStringN(OfxPropertySetHandle h, const char* p, int c, const char* const* v);
    static OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* p, int c, const double* v);
    static OfxStatus propSetIntN(OfxPropertySetHandle h, const char* p, int c, const int* v);
    static OfxStatus propGetPointerN(OfxPropertySetHandle h, const char* p, int c, void** v);
    static OfxStatus propGetStringN(OfxPropertySetHandle h, const char* p, int c, char** v);
    static OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char* p, int c, double* v);
    static OfxStatus propGetIntN(OfxPropertySetHandle h, const char* p, int c, int* v);

    // Parameter suite callbacks
    static OfxStatus paramDefine(OfxParamSetHandle h, const char* type, const char* name, OfxPropertySetHandle* props);
    static OfxStatus paramGetHandle(OfxParamSetHandle h, const char* name, OfxParamHandle* param, OfxPropertySetHandle* props);
    static OfxStatus paramSetGetPropertySet(OfxParamSetHandle h, OfxPropertySetHandle* props);
    static OfxStatus paramGetPropertySet(OfxParamHandle h, OfxPropertySetHandle* props);
    static OfxStatus paramGetValue(OfxParamHandle h, ...);
    static OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime time, ...);
    static OfxStatus paramSetValue(OfxParamHandle h, ...);
    static OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime time, ...);
    static OfxStatus paramGetNumKeys(OfxParamHandle h, unsigned int* n);
    static OfxStatus paramGetKeyTime(OfxParamHandle h, unsigned int nth, OfxTime* t);
    static OfxStatus paramGetKeyIndex(OfxParamHandle h, OfxTime t, int dir, int* idx);
    static OfxStatus paramDeleteKey(OfxParamHandle h, OfxTime t);
    static OfxStatus paramDeleteAllKeys(OfxParamHandle h);
    static OfxStatus paramCopy(OfxParamHandle dst, OfxParamHandle src, OfxTime dstOffset, const OfxRangeD* frameRange);
    static OfxStatus paramEditBegin(OfxParamSetHandle h, const char* name);
    static OfxStatus paramEditEnd(OfxParamSetHandle h);

    // Image Effect suite callbacks
    static OfxStatus ieGetPropSet(OfxImageEffectHandle h, OfxPropertySetHandle* p);
    static OfxStatus ieGetParamSet(OfxImageEffectHandle h, OfxParamSetHandle* p);
    static OfxStatus ieClipDefine(OfxImageEffectHandle h, const char* name, OfxPropertySetHandle* props);
    static OfxStatus ieClipGetHandle(OfxImageEffectHandle h, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* props);
    static OfxStatus ieClipGetPropSet(OfxImageClipHandle h, OfxPropertySetHandle* p);
    static OfxStatus ieClipGetImage(OfxImageClipHandle h, OfxTime time, const OfxRectD* region, OfxPropertySetHandle* image);
    static OfxStatus ieClipReleaseImage(OfxPropertySetHandle image);
    static OfxStatus ieClipGetRegionOfDef(OfxImageClipHandle h, OfxTime time, OfxRectD* bounds);
    static int ieAbort(OfxImageEffectHandle h);
    static OfxStatus ieImageMemAlloc(OfxImageEffectHandle h, size_t n, OfxImageMemoryHandle* mem);
    static OfxStatus ieImageMemFree(OfxImageMemoryHandle mem);
    static OfxStatus ieImageMemLock(OfxImageMemoryHandle mem, void** ptr);
    static OfxStatus ieImageMemUnlock(OfxImageMemoryHandle mem);

    // Memory suite callbacks
    static OfxStatus memAlloc(void* handle, size_t n, void** ptr);
    static OfxStatus memFree(void* ptr);

    // Message suite callbacks
    static OfxStatus msgMessage(void* handle, const char* messageType, const char* messageId, const char* format, ...);
    static OfxStatus msgSetPersistentMessage(void* handle, const char* messageType, const char* messageId, const char* format, ...);
    static OfxStatus msgClearPersistentMessage(void* handle);

    // Progress suite callbacks
    static OfxStatus progStart(void* effectInstance, const char* label);
    static OfxStatus progUpdate(void* effectInstance, double progress);
    static OfxStatus progEnd(void* effectInstance);

    // MultiThread suite callbacks
    static OfxStatus mtMultiThread(OfxThreadFunctionV1 func, unsigned int n, void* arg);
    static OfxStatus mtNumCPUs(unsigned int* n);
    static OfxStatus mtMultiThreadIndex(unsigned int* idx);
    static int mtMultiThreadIsSpawnedThread();
    static OfxStatus mtMutexCreate(OfxMutexHandle* m, int lockCount);
    static OfxStatus mtMutexDestroy(OfxMutexHandle m);
    static OfxStatus mtMutexLock(OfxMutexHandle m);
    static OfxStatus mtMutexUnLock(OfxMutexHandle m);
    static OfxStatus mtMutexTryLock(OfxMutexHandle m);

    // Helpers
    static OfxEffectInstance* instFromHandle(OfxImageEffectHandle h);
    static OfxPropSet* propSetFromHandle(OfxPropertySetHandle h);
    static OfxParamInst* paramFromHandle(OfxParamHandle h);
    static OfxParamDef* paramDefFromHandle(OfxParamSetHandle h, OfxParamHandle h2);

    // Storage para handles (C++ objects)
    struct PropSetStorage : OfxPropSet {};
    struct EffectStorage : OfxEffectInstance {};
    struct ParamSetStorage : QVector<OfxParamDef> {};
    struct ParamStorage : OfxParamInst {};
    struct ClipStorage : OfxClip {};
};
