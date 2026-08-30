// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "OfxHost.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <QDebug>
#include <QThread>
#include <QThreadPool>
#include <QMutexLocker>
#include <QtConcurrent/QtConcurrent>

// OfxPropSet — implementation
// ──────────────────────────────────────────────────────────────────────────

static OfxPropSet::PropEntry& ensureEntry(OfxPropSet& ps, const char* key) {
    return ps.m_props[QString::fromLatin1(key)];
}

static const OfxPropSet::PropEntry* findEntry(const OfxPropSet& ps, const char* key) {
    auto it = ps.m_props.find(QString::fromLatin1(key));
    return (it != ps.m_props.end()) ? &it.value() : nullptr;
}

void OfxPropSet::setPointer(const char* key, int index, void* value) {
    auto& e = ensureEntry(*this, key);
    if (index >= e.ptrs.size()) e.ptrs.resize(index + 1);
    e.ptrs[index] = value;
}

void OfxPropSet::setString(const char* key, int index, const char* value) {
    auto& e = ensureEntry(*this, key);
    if (index >= e.strings.size()) e.strings.resize(index + 1);
    e.strings[index] = value ? QString::fromLatin1(value) : QString();
}

void OfxPropSet::setDouble(const char* key, int index, double value) {
    auto& e = ensureEntry(*this, key);
    if (index >= e.doubles.size()) e.doubles.resize(index + 1);
    e.doubles[index] = value;
}

void OfxPropSet::setInt(const char* key, int index, int value) {
    auto& e = ensureEntry(*this, key);
    if (index >= e.ints.size()) e.ints.resize(index + 1);
    e.ints[index] = value;
}

bool OfxPropSet::getPointer(const char* key, int index, void** value) const {
    const auto* e = findEntry(*this, key);
    if (!e || index >= e->ptrs.size()) { if (value) *value = nullptr; return false; }
    if (value) *value = e->ptrs[index];
    return true;
}

bool OfxPropSet::getString(const char* key, int index, const char*& value) const {
    static thread_local QByteArray tlBuf;
    const auto* e = findEntry(*this, key);
    if (!e || index >= e->strings.size()) { value = ""; return false; }
    tlBuf = e->strings[index].toLatin1();
    value = tlBuf.constData();
    return true;
}

bool OfxPropSet::getDouble(const char* key, int index, double& value) const {
    const auto* e = findEntry(*this, key);
    if (!e || index >= e->doubles.size()) { value = 0; return false; }
    value = e->doubles[index];
    return true;
}

bool OfxPropSet::getInt(const char* key, int index, int& value) const {
    const auto* e = findEntry(*this, key);
    if (!e || index >= e->ints.size()) { value = 0; return false; }
    value = e->ints[index];
    return true;
}

int OfxPropSet::getDimension(const char* key) const {
    const auto* e = findEntry(*this, key);
    if (!e) return 0;
    int d = e->ptrs.size();
    if (e->strings.size() > d) d = e->strings.size();
    if (e->doubles.size() > d) d = e->doubles.size();
    if (e->ints.size() > d) d = e->ints.size();
    return d;
}

QString OfxPropSet::getStringQt(const char* key, int index, const QString& def) const {
    const auto* e = findEntry(*this, key);
    if (!e || index >= e->strings.size()) return def;
    return e->strings[index];
}

double OfxPropSet::getDoubleVal(const char* key, int index, double def) const {
    double v = def;
    getDouble(key, index, v);
    return v;
}

int OfxPropSet::getIntVal(const char* key, int index, int def) const {
    int v = def;
    getInt(key, index, v);
    return v;
}

// ── OfxHost ──────────────────────────────────────────────────────────────

static OfxHostImpl* s_instance = nullptr;

OfxHostImpl& OfxHostImpl::instance() {
    if (!s_instance) s_instance = new OfxHostImpl;
    return *s_instance;
}

OfxHostImpl::OfxHostImpl() {
    std::memset(&m_cHost, 0, sizeof(m_cHost));
    m_cHost.fetchSuite = fetchSuite;
    // Allocate a host property set that plugins can access via m_cHost.host
    auto* hostProps = new PropSetStorage;
    hostProps->setString(kOfxPropType, 0, kOfxTypeImageEffectHost);
    hostProps->setString(kOfxPropName, 0, "Pierrot");
    hostProps->setInt(kOfxPropVersion, 0, 1);
    hostProps->setString(kOfxPropVersionLabel, 0, "0.5");
    hostProps->setInt(kOfxImageEffectHostPropIsBackground, 0, 0);
    hostProps->setInt(kOfxImageEffectPropSupportsMultiResolution, 0, 0);
    hostProps->setInt(kOfxImageEffectPropSupportsTiles, 0, 0);
    hostProps->setInt(kOfxImageEffectPropTemporalClipAccess, 0, 1);
    hostProps->setInt(kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    hostProps->setInt(kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
    hostProps->setString(kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    hostProps->setString(kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    m_cHost.host = reinterpret_cast<OfxPropertySetHandle>(hostProps);
}

OfxHostImpl::~OfxHostImpl() {
    s_instance = nullptr;
}

// ── Handle helpers ───────────────────────────────────────────────────────

OfxEffectInstance* OfxHostImpl::instFromHandle(OfxImageEffectHandle h) {
    return reinterpret_cast<EffectStorage*>(h);
}

OfxPropSet* OfxHostImpl::propSetFromHandle(OfxPropertySetHandle h) {
    return reinterpret_cast<PropSetStorage*>(h);
}

OfxParamInst* OfxHostImpl::paramFromHandle(OfxParamHandle h) {
    return reinterpret_cast<ParamStorage*>(h);
}

// ── fetchSuite ───────────────────────────────────────────────────────────

const void* OfxHostImpl::fetchSuite(OfxPropertySetHandle, const char* suiteName, int /*version*/) {
    if (!std::strcmp(suiteName, kOfxPropertySuite))
        return const_cast<OfxPropertySuiteV1*>(&s_propertySuite);
    if (!std::strcmp(suiteName, kOfxParameterSuite))
        return const_cast<OfxParameterSuiteV1*>(&s_parameterSuite);
    if (!std::strcmp(suiteName, kOfxImageEffectSuite))
        return const_cast<OfxImageEffectSuiteV1*>(&s_imageEffectSuite);
    if (!std::strcmp(suiteName, kOfxMemorySuite))
        return const_cast<OfxMemorySuiteV1*>(&s_memorySuite);
    if (!std::strcmp(suiteName, kOfxMultiThreadSuite))
        return const_cast<OfxMultiThreadSuiteV1*>(&s_multiThreadSuite);
    if (!std::strcmp(suiteName, kOfxMessageSuite))
        return const_cast<OfxMessageSuiteV2*>(&s_messageSuite);
    if (!std::strcmp(suiteName, kOfxProgressSuite))
        return const_cast<OfxProgressSuiteV1*>(&s_progressSuite);
    return nullptr;
}

// ── Property Suite ───────────────────────────────────────────────────────

const OfxPropertySuiteV1 OfxHostImpl::s_propertySuite = {
    propSetPointer, propSetString, propSetDouble, propSetInt,
    propSetPointerN, propSetStringN, propSetDoubleN, propSetIntN,
    propGetPointer, propGetString, propGetDouble, propGetInt,
    propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN,
    propReset, propGetDimension
};

#define PROP_HANDLE(h) \
    auto* ps = propSetFromHandle(h); \
    if (!ps) return kOfxStatErrBadHandle;

OfxStatus OfxHostImpl::propSetPointer(OfxPropertySetHandle h, const char* p, int i, void* v) {
    PROP_HANDLE(h); ps->setPointer(p, i, v); return kOfxStatOK;
}
OfxStatus OfxHostImpl::propSetString(OfxPropertySetHandle h, const char* p, int i, const char* v) {
    PROP_HANDLE(h); ps->setString(p, i, v); return kOfxStatOK;
}
OfxStatus OfxHostImpl::propSetDouble(OfxPropertySetHandle h, const char* p, int i, double v) {
    PROP_HANDLE(h); ps->setDouble(p, i, v); return kOfxStatOK;
}
OfxStatus OfxHostImpl::propSetInt(OfxPropertySetHandle h, const char* p, int i, int v) {
    PROP_HANDLE(h); ps->setInt(p, i, v); return kOfxStatOK;
}
OfxStatus OfxHostImpl::propGetPointer(OfxPropertySetHandle h, const char* p, int i, void** v) {
    PROP_HANDLE(h);
    if (!ps->getPointer(p, i, v)) {
        qWarning() << "[OFX] propGetPointer FAILED:" << p << "[" << i << "]";
        return kOfxStatErrUnknown;
    }
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propGetString(OfxPropertySetHandle h, const char* p, int i, char** v) {
    PROP_HANDLE(h);
    static thread_local QByteArray tlBuf;
    const char* c = nullptr;
    if (!ps->getString(p, i, c)) {
        qWarning() << "[OFX] propGetString FAILED:" << p << "[" << i << "]";
        return kOfxStatErrUnknown;
    }
    tlBuf = QByteArray(c);
    if (v) *v = tlBuf.data();
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propGetDouble(OfxPropertySetHandle h, const char* p, int i, double* v) {
    PROP_HANDLE(h);
    double d = 0;
    if (!ps->getDouble(p, i, d)) {
        qWarning() << "[OFX] propGetDouble FAILED:" << p << "[" << i << "]";
        return kOfxStatErrUnknown;
    }
    if (v) *v = d;
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propGetInt(OfxPropertySetHandle h, const char* p, int i, int* v) {
    PROP_HANDLE(h);
    int iv = 0;
    if (!ps->getInt(p, i, iv)) {
        qWarning() << "[OFX] propGetInt FAILED:" << p << "[" << i << "]";
        return kOfxStatErrUnknown;
    }
    if (v) *v = iv;
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propGetDimension(OfxPropertySetHandle h, const char* p, int* c) {
    PROP_HANDLE(h);
    if (c) *c = ps->getDimension(p);
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propReset(OfxPropertySetHandle h, const char* p) {
    PROP_HANDLE(h);
    ps->m_props.remove(QString::fromLatin1(p));
    return kOfxStatOK;
}

// N-ary stubs
OfxStatus OfxHostImpl::propSetPointerN(OfxPropertySetHandle h, const char* p, int c, void* const* v) {
    for (int i = 0; i < c; ++i) { auto s = propSetPointer(h, p, i, v[i]); if (s != kOfxStatOK) return s; }
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propSetStringN(OfxPropertySetHandle h, const char* p, int c, const char* const* v) {
    for (int i = 0; i < c; ++i) { auto s = propSetString(h, p, i, v[i]); if (s != kOfxStatOK) return s; }
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propSetDoubleN(OfxPropertySetHandle h, const char* p, int c, const double* v) {
    for (int i = 0; i < c; ++i) { auto s = propSetDouble(h, p, i, v[i]); if (s != kOfxStatOK) return s; }
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propSetIntN(OfxPropertySetHandle h, const char* p, int c, const int* v) {
    for (int i = 0; i < c; ++i) { auto s = propSetInt(h, p, i, v[i]); if (s != kOfxStatOK) return s; }
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propGetPointerN(OfxPropertySetHandle h, const char* p, int c, void** v) {
    for (int i = 0; i < c; ++i) { auto s = propGetPointer(h, p, i, &v[i]); if (s != kOfxStatOK) return s; }
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propGetStringN(OfxPropertySetHandle h, const char* p, int c, char** v) {
    for (int i = 0; i < c; ++i) { auto s = propGetString(h, p, i, &v[i]); if (s != kOfxStatOK) return s; }
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propGetDoubleN(OfxPropertySetHandle h, const char* p, int c, double* v) {
    for (int i = 0; i < c; ++i) { auto s = propGetDouble(h, p, i, &v[i]); if (s != kOfxStatOK) return s; }
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::propGetIntN(OfxPropertySetHandle h, const char* p, int c, int* v) {
    for (int i = 0; i < c; ++i) { auto s = propGetInt(h, p, i, &v[i]); if (s != kOfxStatOK) return s; }
    return kOfxStatOK;
}

// ── Parameter Suite ──────────────────────────────────────────────────────

const OfxParameterSuiteV1 OfxHostImpl::s_parameterSuite = {
    paramDefine, paramGetHandle,
    paramSetGetPropertySet, paramGetPropertySet,
    paramGetValue, paramGetValueAtTime,
    nullptr, nullptr, // derivative, integral
    paramSetValue, paramSetValueAtTime,
    paramGetNumKeys, paramGetKeyTime, paramGetKeyIndex,
    paramDeleteKey, paramDeleteAllKeys,
    paramCopy, paramEditBegin, paramEditEnd
};

OfxStatus OfxHostImpl::paramDefine(OfxParamSetHandle h, const char* type,
                               const char* name, OfxPropertySetHandle* props) {
    if (!h || !type || !name) return kOfxStatErrBadHandle;
    auto* defs = reinterpret_cast<QVector<OfxParamDef>*>(h);
    for (const auto& d : *defs)
        if (d.name == QString::fromLatin1(name)) return kOfxStatErrExists;

    OfxParamDef def;
    def.name = QString::fromLatin1(name);
    def.type = QString::fromLatin1(type);
    if (props) {
        auto* storage = new PropSetStorage;
        *props = reinterpret_cast<OfxPropertySetHandle>(storage);
        def.tempStorage = storage; // plugin vai preencher; copiar depois do describe
    }
    defs->append(def);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::paramGetHandle(OfxParamSetHandle h, const char* name,
                                  OfxParamHandle* param, OfxPropertySetHandle* props) {
    if (!h || !name) return kOfxStatErrBadHandle;
    auto* defs = reinterpret_cast<QVector<OfxParamDef>*>(h);
    QString qName = QString::fromLatin1(name);

    // Busca por nome
    for (int i = 0; i < defs->size(); ++i) {
        if ((*defs)[i].name == qName) {
            if (param) {
                // Verifica se já existe um handle cacheado para este parâmetro
                // Nota: o cache é por instância, mas aqui só temos o paramSetHandle
                // Por enquanto, criamos um novo handle (o cache será implementado
                // no nível da instância do efeito)
                auto* storage = new ParamStorage;
                storage->name = (*defs)[i].name;
                storage->type = (*defs)[i].type;
                // Copia valores default do descritor
                if ((*defs)[i].type == kOfxParamTypeDouble)
                    storage->doubleVal = (*defs)[i].props.getDoubleVal(kOfxParamPropDefault, 0, 0.0);
                else if ((*defs)[i].type == kOfxParamTypeInteger)
                    storage->intVal = (*defs)[i].props.getIntVal(kOfxParamPropDefault, 0, 0);
                else if ((*defs)[i].type == kOfxParamTypeBoolean)
                    storage->boolVal = (*defs)[i].props.getIntVal(kOfxParamPropDefault, 0, 0) != 0;
                else if ((*defs)[i].type == kOfxParamTypeChoice)
                    storage->choiceVal = (*defs)[i].props.getIntVal(kOfxParamPropDefault, 0, 0);
                else if ((*defs)[i].type == kOfxParamTypeRGB || (*defs)[i].type == kOfxParamTypeRGBA) {
                    storage->r = (*defs)[i].props.getDoubleVal(kOfxParamPropDefault, 0, 0.0);
                    storage->g = (*defs)[i].props.getDoubleVal(kOfxParamPropDefault, 1, 0.0);
                    storage->b = (*defs)[i].props.getDoubleVal(kOfxParamPropDefault, 2, 0.0);
                    if ((*defs)[i].type == kOfxParamTypeRGBA)
                        storage->a = (*defs)[i].props.getDoubleVal(kOfxParamPropDefault, 3, 1.0);
                }
                *param = reinterpret_cast<OfxParamHandle>(storage);
            }
            if (props) {
                auto* ps = new PropSetStorage;
                ps->m_props = (*defs)[i].props.m_props;
                *props = reinterpret_cast<OfxPropertySetHandle>(ps);
            }
            return kOfxStatOK;
        }
    }
    return kOfxStatErrUnknown;
}

OfxStatus OfxHostImpl::paramSetGetPropertySet(OfxParamSetHandle h, OfxPropertySetHandle* p) {
    if (!h) return kOfxStatErrBadHandle;
    if (p) *p = nullptr; // não há prop set global no param set
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::paramGetPropertySet(OfxParamHandle h, OfxPropertySetHandle* p) {
    if (!h) return kOfxStatErrBadHandle;
    auto* inst = paramFromHandle(h);
    // Retorna um property set temporário com o tipo
    auto* ps = new PropSetStorage;
    ps->setString(kOfxParamPropType, 0, inst->type.toLatin1().constData());
    if (p) *p = reinterpret_cast<OfxPropertySetHandle>(ps);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::paramGetValue(OfxParamHandle h, ...) {
    if (!h) return kOfxStatErrBadHandle;
    auto* inst = paramFromHandle(h);
    va_list args;
    va_start(args, h);

    if (inst->type == kOfxParamTypeDouble) {
        double* ptr = va_arg(args, double*);
        if (ptr) *ptr = inst->doubleVal;
    } else if (inst->type == kOfxParamTypeInteger) {
        int* ptr = va_arg(args, int*);
        if (ptr) *ptr = inst->intVal;
    } else if (inst->type == kOfxParamTypeBoolean) {
        int* ptr = va_arg(args, int*);
        if (ptr) *ptr = inst->boolVal ? 1 : 0;
    } else if (inst->type == kOfxParamTypeChoice) {
        int* ptr = va_arg(args, int*);
        if (ptr) *ptr = inst->choiceVal;
    } else if (inst->type == kOfxParamTypeRGB) {
        double* r = va_arg(args, double*);
        double* g = va_arg(args, double*);
        double* b = va_arg(args, double*);
        if (r) *r = inst->r;
        if (g) *g = inst->g;
        if (b) *b = inst->b;
    } else if (inst->type == kOfxParamTypeRGBA) {
        double* r = va_arg(args, double*);
        double* g = va_arg(args, double*);
        double* b = va_arg(args, double*);
        double* a = va_arg(args, double*);
        if (r) *r = inst->r;
        if (g) *g = inst->g;
        if (b) *b = inst->b;
        if (a) *a = inst->a;
    } else if (inst->type == kOfxParamTypeString) {
        // Para strings via varargs não funciona bem (char**), retornamos OK
    }
    va_end(args);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::paramGetValueAtTime(OfxParamHandle h, OfxTime time, ...) {
    // Sem animação, retorna valor atual
    if (!h) return kOfxStatErrBadHandle;
    // Redireciona para paramGetValue — ignora time
    (void)time;
    va_list args;
    va_start(args, time);
    auto* inst = paramFromHandle(h);
    if (inst->type == kOfxParamTypeDouble) {
        double* ptr = va_arg(args, double*);
        if (ptr) *ptr = inst->doubleVal;
    } else if (inst->type == kOfxParamTypeInteger) {
        int* ptr = va_arg(args, int*);
        if (ptr) *ptr = inst->intVal;
    } else if (inst->type == kOfxParamTypeBoolean) {
        int* ptr = va_arg(args, int*);
        if (ptr) *ptr = inst->boolVal ? 1 : 0;
    } else if (inst->type == kOfxParamTypeChoice) {
        int* ptr = va_arg(args, int*);
        if (ptr) *ptr = inst->choiceVal;
    } else if (inst->type == kOfxParamTypeRGB || inst->type == kOfxParamTypeRGBA) {
        double* r = va_arg(args, double*);
        double* g = va_arg(args, double*);
        double* b = va_arg(args, double*);
        if (r) *r = inst->r;
        if (g) *g = inst->g;
        if (b) *b = inst->b;
        if (inst->type == kOfxParamTypeRGBA) {
            double* a = va_arg(args, double*);
            if (a) *a = inst->a;
        }
    }
    va_end(args);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::paramSetValue(OfxParamHandle h, ...) {
    if (!h) return kOfxStatErrBadHandle;
    auto* inst = paramFromHandle(h);
    va_list args;
    va_start(args, h);

    if (inst->type == kOfxParamTypeDouble) {
        inst->doubleVal = va_arg(args, double);
    } else if (inst->type == kOfxParamTypeInteger) {
        inst->intVal = va_arg(args, int);
    } else if (inst->type == kOfxParamTypeBoolean) {
        inst->boolVal = va_arg(args, int) != 0;
    } else if (inst->type == kOfxParamTypeChoice) {
        inst->choiceVal = va_arg(args, int);
    } else if (inst->type == kOfxParamTypeRGB) {
        inst->r = va_arg(args, double);
        inst->g = va_arg(args, double);
        inst->b = va_arg(args, double);
    } else if (inst->type == kOfxParamTypeRGBA) {
        inst->r = va_arg(args, double);
        inst->g = va_arg(args, double);
        inst->b = va_arg(args, double);
        inst->a = va_arg(args, double);
    }
    va_end(args);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::paramSetValueAtTime(OfxParamHandle h, OfxTime time, ...) {
    // Sem suporte a keyframes — retorna valor atual
    (void)time;
    // Sem suporte a keyframes — retorna valor atual
    if (!h) return kOfxStatErrBadHandle;
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::paramGetNumKeys(OfxParamHandle, unsigned int* n) {
    if (n) *n = 0;
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::paramGetKeyTime(OfxParamHandle, unsigned int, OfxTime*) {
    return kOfxStatErrBadIndex;
}
OfxStatus OfxHostImpl::paramGetKeyIndex(OfxParamHandle, OfxTime, int, int* idx) {
    if (idx) *idx = -1;
    return kOfxStatFailed;
}
OfxStatus OfxHostImpl::paramDeleteKey(OfxParamHandle, OfxTime) {
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::paramDeleteAllKeys(OfxParamHandle) {
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) {
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::paramEditBegin(OfxParamSetHandle h, const char*) {
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::paramEditEnd(OfxParamSetHandle) {
    return kOfxStatOK;
}

// ── Image Effect Suite ───────────────────────────────────────────────────

const OfxImageEffectSuiteV1 OfxHostImpl::s_imageEffectSuite = {
    ieGetPropSet, ieGetParamSet,
    ieClipDefine, ieClipGetHandle, ieClipGetPropSet,
    ieClipGetImage, ieClipReleaseImage,
    ieClipGetRegionOfDef,
    ieAbort, ieImageMemAlloc, ieImageMemFree, ieImageMemLock, ieImageMemUnlock
};

OfxStatus OfxHostImpl::ieGetPropSet(OfxImageEffectHandle h, OfxPropertySetHandle* p) {
    if (!h) { qWarning() << "[OFX] ieGetPropSet: null handle"; return kOfxStatErrBadHandle; }
    auto* inst = instFromHandle(h);
    if (p) *p = reinterpret_cast<OfxPropertySetHandle>(&inst->props);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::ieGetParamSet(OfxImageEffectHandle h, OfxParamSetHandle* p) {
    if (!h) { qWarning() << "[OFX] ieGetParamSet: null handle"; return kOfxStatErrBadHandle; }
    auto* inst = instFromHandle(h);
    if (p) *p = reinterpret_cast<OfxParamSetHandle>(&inst->paramDefs);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::ieClipDefine(OfxImageEffectHandle h, const char* name, OfxPropertySetHandle* props) {
    if (!h || !name) return kOfxStatErrBadHandle;
    auto* inst = instFromHandle(h);
    OfxClip clip;
    clip.name = QString::fromLatin1(name);
    if (props) {
        auto* ps = new PropSetStorage;
        *props = reinterpret_cast<OfxPropertySetHandle>(ps);
        clip.props.m_props = ps->m_props;
    }
    inst->clips[clip.name] = clip;
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::ieClipGetHandle(OfxImageEffectHandle h, const char* name,
                                    OfxImageClipHandle* clip, OfxPropertySetHandle* props) {
    if (!h || !name) { qWarning() << "[OFX] ieClipGetHandle: null handle/name"; return kOfxStatErrBadHandle; }
    auto* inst = instFromHandle(h);
    QString qname = QString::fromLatin1(name);
    if (!inst->clips.contains(qname)) {
        qWarning() << "[OFX] ieClipGetHandle: clip" << qname << "not found (available:" << inst->clips.keys() << ")";
        return kOfxStatErrUnknown;
    }
    qInfo() << "[OFX] ieClipGetHandle:" << qname;

    if (clip) *clip = reinterpret_cast<OfxImageClipHandle>(&inst->clips[qname]);
    if (props) *props = reinterpret_cast<OfxPropertySetHandle>(&inst->clips[qname].props);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::ieClipGetPropSet(OfxImageClipHandle h, OfxPropertySetHandle* p) {
    if (!h) return kOfxStatErrBadHandle;
    auto* clip = reinterpret_cast<OfxClip*>(h);
    if (p) *p = reinterpret_cast<OfxPropertySetHandle>(&clip->props);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::ieClipGetImage(OfxImageClipHandle h, OfxTime t, const OfxRectD* r,
                                    OfxPropertySetHandle* image) {
    if (!h) { qWarning() << "[OFX] ieClipGetImage: null handle"; return kOfxStatErrBadHandle; }
    auto* clip = reinterpret_cast<OfxClip*>(h);
    if (clip->image.isNull()) {
        qWarning() << "[OFX] ieClipGetImage: null image for clip" << clip->name;
        return kOfxStatFailed;
    }
    qInfo() << "[OFX] ieClipGetImage:" << clip->name << "time" << t
            << "size" << clip->image.width() << "x" << clip->image.height();

    // Cria um property set com ponteiro para a QImage
    // Usamos propSetPointer com um ponteiro para a QImage
    auto* ps = new PropSetStorage;
    ps->setPointer(kOfxImagePropData, 0, clip->image.bits());
    ps->setInt(kOfxImagePropRowBytes, 0, clip->image.bytesPerLine());
    ps->setInt(kOfxImagePropBounds, 0, 0);
    ps->setInt(kOfxImagePropBounds, 1, 0);
    ps->setInt(kOfxImagePropBounds, 2, clip->image.width());
    ps->setInt(kOfxImagePropBounds, 3, clip->image.height());
    ps->setInt(kOfxImagePropRegionOfDefinition, 0, 0);
    ps->setInt(kOfxImagePropRegionOfDefinition, 1, 0);
    ps->setInt(kOfxImagePropRegionOfDefinition, 2, clip->image.width());
    ps->setInt(kOfxImagePropRegionOfDefinition, 3, clip->image.height());
    ps->setString(kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthByte);
    ps->setString(kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
    ps->setPointer("OfxImageClipPropImage", 0, &clip->image); // pointer para QImage
    ps->setDouble(kOfxImageEffectPropRenderScale, 0, 1.0);
    ps->setDouble(kOfxImageEffectPropRenderScale, 1, 1.0);

    if (image) *image = reinterpret_cast<OfxPropertySetHandle>(ps);
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::ieClipReleaseImage(OfxPropertySetHandle h) {
    if (h) {
        auto* ps = propSetFromHandle(h);
        delete ps;
    }
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::ieClipGetRegionOfDef(OfxImageClipHandle h, OfxTime, OfxRectD* bounds) {
    if (!h) return kOfxStatErrBadHandle;
    auto* clip = reinterpret_cast<OfxClip*>(h);
    if (bounds) {
        bounds->x1 = 0;
        bounds->y1 = 0;
        bounds->x2 = clip->image.width();
        bounds->y2 = clip->image.height();
    }
    return kOfxStatOK;
}

int OfxHostImpl::ieAbort(OfxImageEffectHandle) {
    return 0; // nunca aborta
}

OfxStatus OfxHostImpl::ieImageMemAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* mem) {
    if (mem) *mem = reinterpret_cast<OfxImageMemoryHandle>(std::malloc(n));
    return *mem ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus OfxHostImpl::ieImageMemFree(OfxImageMemoryHandle mem) {
    std::free(mem);
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::ieImageMemLock(OfxImageMemoryHandle mem, void** ptr) {
    if (ptr) *ptr = mem;
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::ieImageMemUnlock(OfxImageMemoryHandle) {
    return kOfxStatOK;
}

// ── Memory Suite ─────────────────────────────────────────────────────────

const OfxMemorySuiteV1 OfxHostImpl::s_memorySuite = { memAlloc, memFree };

OfxStatus OfxHostImpl::memAlloc(void*, size_t n, void** ptr) {
    if (ptr) *ptr = std::calloc(1, n);
    return *ptr ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus OfxHostImpl::memFree(void* ptr) {
    std::free(ptr);
    return kOfxStatOK;
}

// ── Message Suite ────────────────────────────────────────────────────────

const OfxMessageSuiteV2 OfxHostImpl::s_messageSuite = {
    msgMessage, msgSetPersistentMessage, msgClearPersistentMessage
};

OfxStatus OfxHostImpl::msgMessage(void*, const char* messageType, const char*, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char* raw = nullptr;
    if (vasprintf(&raw, format, args) < 0) raw = nullptr;
    va_end(args);
    QByteArray buf = raw ? QByteArray(raw) : QByteArray();
    free(raw);
    qInfo() << "[OFX Plugin]" << messageType << ":" << buf.constData();
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::msgSetPersistentMessage(void*, const char* messageType, const char*, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char* raw = nullptr;
    if (vasprintf(&raw, format, args) < 0) raw = nullptr;
    va_end(args);
    QByteArray buf = raw ? QByteArray(raw) : QByteArray();
    free(raw);
    qWarning() << "[OFX Plugin persistent]" << messageType << ":" << buf.constData();
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::msgClearPersistentMessage(void*) {
    return kOfxStatOK;
}

// ── Progress Suite ───────────────────────────────────────────────────────

const OfxProgressSuiteV1 OfxHostImpl::s_progressSuite = {
    progStart, progUpdate, progEnd
};

OfxStatus OfxHostImpl::progStart(void*, const char* label) {
    qInfo() << "[OFX Progress]" << label;
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::progUpdate(void*, double progress) {
    qInfo() << "[OFX Progress]" << qRound(progress * 100) << "%";
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::progEnd(void*) {
    qInfo() << "[OFX Progress] done";
    return kOfxStatOK;
}

// ── MultiThread Suite ────────────────────────────────────────────────────

const OfxMultiThreadSuiteV1 OfxHostImpl::s_multiThreadSuite = {
    mtMultiThread, mtNumCPUs, mtMultiThreadIndex, mtMultiThreadIsSpawnedThread,
    mtMutexCreate, mtMutexDestroy, mtMutexLock, mtMutexUnLock, mtMutexTryLock
};

OfxStatus OfxHostImpl::mtMultiThread(OfxThreadFunctionV1 func, unsigned int n, void* arg) {
    if (n <= 1) {
        for (unsigned int i = 0; i < n; ++i)
            func(i, n, arg);
        return kOfxStatOK;
    }
    // Para n pequeno (2-4), executa sequencialmente — o overhead de dispatch
    // para o thread pool supera o ganho de paralelismo.
    if (n <= 4) {
        for (unsigned int i = 0; i < n; ++i)
            func(i, n, arg);
        return kOfxStatOK;
    }
    // Executa em paralelo usando QtConcurrent::map.
    std::vector<unsigned int> indices(n);
    for (unsigned int i = 0; i < n; ++i) indices[i] = i;

    QtConcurrent::map(indices, [&](unsigned int& i) {
        func(i, n, arg);
    }).waitForFinished();

    return kOfxStatOK;
}

OfxStatus OfxHostImpl::mtNumCPUs(unsigned int* n) {
    if (n) *n = QThread::idealThreadCount();
    return kOfxStatOK;
}

OfxStatus OfxHostImpl::mtMultiThreadIndex(unsigned int* idx) {
    if (idx) *idx = 0;
    return kOfxStatOK;
}

int OfxHostImpl::mtMultiThreadIsSpawnedThread() {
    return 0;
}

struct OfxMutex {
    QMutex m;
};

OfxStatus OfxHostImpl::mtMutexCreate(OfxMutexHandle* m, int) {
    if (m) *m = new OfxMutex;
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::mtMutexDestroy(OfxMutexHandle m) {
    delete m;
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::mtMutexLock(OfxMutexHandle m) {
    if (m) m->m.lock();
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::mtMutexUnLock(OfxMutexHandle m) {
    if (m) m->m.unlock();
    return kOfxStatOK;
}
OfxStatus OfxHostImpl::mtMutexTryLock(OfxMutexHandle m) {
    if (m) {
        m->m.tryLock();
    }
    return kOfxStatOK;
}

// ── Plugin lifecycle ─────────────────────────────────────────────────────

void OfxHostImpl::initPlugin(OfxEffectInstance& inst, void* libHandle,
                          OfxPluginEntryPoint* entry, const QString& pluginId) {
    inst.pluginLib = libHandle;
    inst.entry = entry;
    inst.pluginId = pluginId;
}

bool OfxHostImpl::describe(OfxEffectInstance& inst) {
    if (!inst.entry) return false;

    // Propriedades do host (passadas ao plugin durante kOfxActionLoad)
    auto* pluginProps = new PropSetStorage;
    pluginProps->setString(kOfxPropType, 0, kOfxTypeImageEffectHost);
    pluginProps->setString(kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
    pluginProps->setString(kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);
    pluginProps->setString(kOfxImageEffectPropSupportedContexts, 2, kOfxImageEffectContextGenerator);
    pluginProps->setString(kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
    pluginProps->setString(kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
    pluginProps->setString(kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);
    pluginProps->setString(kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    pluginProps->setString(kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentRGB);
    pluginProps->setString(kOfxImageEffectPropSupportedComponents, 2, kOfxImageComponentAlpha);

    OfxPropertySetHandle propsHandle = reinterpret_cast<OfxPropertySetHandle>(pluginProps);

    // Handle do efeito (passado como primeiro argumento das ações)
    OfxImageEffectHandle effectHandle = reinterpret_cast<OfxImageEffectHandle>(&inst);

    // kOfxActionLoad
    OfxStatus s = inst.entry(kOfxActionLoad, effectHandle, propsHandle, nullptr);
    if (s != kOfxStatOK && s != kOfxStatReplyDefault) {
        qWarning() << "[OFX] Plugin" << inst.pluginId << "kOfxActionLoad failed:" << s;
    }

    // kOfxActionDescribe
    s = inst.entry(kOfxActionDescribe, effectHandle, propsHandle, nullptr);
    if (s != kOfxStatOK && s != kOfxStatReplyDefault) {
        qWarning() << "[OFX] Plugin" << inst.pluginId << "kOfxActionDescribe failed:" << s;
        return false;
    }

    // Extrai metadados do descritor
    inst.imageEffectProps.m_props = pluginProps->m_props;

    // kOfxImageEffectActionDescribeInContext para filtro (context = filter)
    auto* contextProps = new PropSetStorage;
    contextProps->setString(kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);
    OfxPropertySetHandle ctxHandle = reinterpret_cast<OfxPropertySetHandle>(contextProps);

    s = inst.entry(kOfxImageEffectActionDescribeInContext, effectHandle, ctxHandle, nullptr);
    if (s != kOfxStatOK && s != kOfxStatReplyDefault) {
        // Tenta context geral
        contextProps->setString(kOfxImageEffectPropContext, 0, kOfxImageEffectContextGeneral);
        s = inst.entry(kOfxImageEffectActionDescribeInContext, effectHandle, ctxHandle, nullptr);
    }

    delete contextProps;
    return true;
}

bool OfxHostImpl::createInstance(OfxEffectInstance& inst) {
    if (!inst.entry) return false;

    // Copia propriedades do descritor para a instância
    inst.props.m_props = inst.imageEffectProps.m_props;

    // Garante que clips Source e Output existem
    if (!inst.clips.contains(QStringLiteral("Source"))) {
        OfxClip source;
        source.name = QStringLiteral("Source");
        inst.clips["Source"] = source;
    }
    if (!inst.clips.contains(QStringLiteral("Output"))) {
        OfxClip output;
        output.name = QStringLiteral("Output");
        inst.clips["Output"] = output;
    }

    // Cria instâncias de parâmetros a partir das definições
    inst.params.clear();
    for (const auto& pd : inst.paramDefs) {
        OfxParamInst pi;
        pi.name = pd.name;
        pi.type = pd.type;
        if (pd.type == kOfxParamTypeDouble)
            pi.doubleVal = pd.props.getDoubleVal(kOfxParamPropDefault, 0, 0.0);
        else if (pd.type == kOfxParamTypeInteger)
            pi.intVal = pd.props.getIntVal(kOfxParamPropDefault, 0, 0);
        else if (pd.type == kOfxParamTypeBoolean)
            pi.boolVal = pd.props.getIntVal(kOfxParamPropDefault, 0, 0) != 0;
        else if (pd.type == kOfxParamTypeChoice)
            pi.choiceVal = pd.props.getIntVal(kOfxParamPropDefault, 0, 0);
        else if (pd.type == kOfxParamTypeRGB || pd.type == kOfxParamTypeRGBA) {
            pi.r = pd.props.getDoubleVal(kOfxParamPropDefault, 0, 0.0);
            pi.g = pd.props.getDoubleVal(kOfxParamPropDefault, 1, 0.0);
            pi.b = pd.props.getDoubleVal(kOfxParamPropDefault, 2, 0.0);
            if (pd.type == kOfxParamTypeRGBA)
                pi.a = pd.props.getDoubleVal(kOfxParamPropDefault, 3, 1.0);
        }
        inst.params.append(pi);
    }

    // Chama kOfxActionCreateInstance
    OfxPropertySetHandle propsHandle = reinterpret_cast<OfxPropertySetHandle>(&inst.props);
    OfxImageEffectHandle effectHandle = reinterpret_cast<OfxImageEffectHandle>(&inst);
    OfxStatus s = inst.entry(kOfxActionCreateInstance, effectHandle, propsHandle, nullptr);

    return s == kOfxStatOK || s == kOfxStatReplyDefault;
}

bool OfxHostImpl::render(OfxEffectInstance& inst, const QImage& input, QImage& output,
                      double time, int width, int height) {
    if (!inst.entry || input.isNull()) return false;

    qInfo() << "[OFX] Render" << inst.pluginId
            << "- clips disponíveis:" << inst.clips.keys()
            << "- params:" << inst.params.size()
            << "- input:" << input.width() << "x" << input.height();

    // Encontra o clip de entrada (Source ou primeiro clip não-Output)
    QString sourceName = QStringLiteral("Source");
    if (!inst.clips.contains(sourceName)) {
        // Tenta encontrar qualquer clip que não seja Output
        for (auto it = inst.clips.begin(); it != inst.clips.end(); ++it) {
            if (it.key() != QStringLiteral("Output")) {
                sourceName = it.key();
                break;
            }
        }
    }

    // Encontra o clip de saída (Output ou último clip)
    QString outputName = QStringLiteral("Output");
    if (!inst.clips.contains(outputName)) {
        // Tenta encontrar o último clip que não é o source
        for (auto it = inst.clips.begin(); it != inst.clips.end(); ++it) {
            if (it.key() != sourceName) {
                outputName = it.key();
                break;
            }
        }
    }

    // Atribui a imagem de entrada ao clip de source e suas propriedades
    // Converte para RGBA8888 para corresponder ao que o plugin espera
    if (inst.clips.contains(sourceName)) {
        inst.clips[sourceName].image = input.convertToFormat(QImage::Format_RGBA8888);
        inst.clips[sourceName].props.setString(kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthByte);
        inst.clips[sourceName].props.setString(kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
    }

    // Prepara a imagem de saída (RGBA8888 corresponde ao kOfxImageComponentRGBA)
    output = QImage(width, height, QImage::Format_RGBA8888);
    output.fill(Qt::transparent);
    if (inst.clips.contains(outputName)) {
        inst.clips[outputName].image = output;
        inst.clips[outputName].props.setString(kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthByte);
        inst.clips[outputName].props.setString(kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
    }

    // Prepara argumentos de render
    auto* inArgs = new PropSetStorage;
    inArgs->setDouble(kOfxPropTime, 0, time);
    inArgs->setDouble(kOfxImageEffectPropRenderScale, 0, 1.0);
    inArgs->setDouble(kOfxImageEffectPropRenderScale, 1, 1.0);
    inArgs->setInt(kOfxImageEffectPropFieldToRender, 0, 0);
    // Render window
    inArgs->setInt(kOfxImageEffectPropRenderWindow, 0, 0);
    inArgs->setInt(kOfxImageEffectPropRenderWindow, 1, 0);
    inArgs->setInt(kOfxImageEffectPropRenderWindow, 2, width);
    inArgs->setInt(kOfxImageEffectPropRenderWindow, 3, height);

    // Também seta no property set do efeito (o plugin lê daqui via ieGetPropSet)
    inst.props.setDouble(kOfxImageEffectPropRenderScale, 0, 1.0);
    inst.props.setDouble(kOfxImageEffectPropRenderScale, 1, 1.0);

    OfxPropertySetHandle inHandle = reinterpret_cast<OfxPropertySetHandle>(inArgs);
    OfxPropertySetHandle outHandle = nullptr;

    OfxImageEffectHandle effectHandle = reinterpret_cast<OfxImageEffectHandle>(&inst);
    OfxStatus s = inst.entry(kOfxImageEffectActionRender, effectHandle, inHandle, outHandle);

    qInfo() << "[OFX] Render resultado para" << inst.pluginId
            << "- status:" << s
            << "- source:" << sourceName
            << "- output:" << outputName;

    if (s == kOfxStatOK) {
        if (inst.clips.contains(outputName))
            output = inst.clips[outputName].image;
    } else {
        qWarning() << "[OFX] Render failed for" << inst.pluginId
                   << "- status:" << s
                   << "(0=OK,1=Failed,2=Fatal,3=Unknown,4=MissingHost,5=Unsupported)";
        output = input; // fallback: retorna input
    }

    delete inArgs;
    return s == kOfxStatOK;
}

void OfxHostImpl::destroyInstance(OfxEffectInstance& inst) {
    if (inst.entry) {
        OfxImageEffectHandle effectHandle = reinterpret_cast<OfxImageEffectHandle>(&inst);
        OfxPropertySetHandle propsHandle = reinterpret_cast<OfxPropertySetHandle>(&inst.props);
        inst.entry(kOfxActionDestroyInstance, effectHandle, propsHandle, nullptr);
    }
    inst.privateData = nullptr;
    inst.params.clear();
    // Limpa cache de handles de parâmetros
    inst.paramHandleCache.clear();
    for (auto& pd : inst.paramDefs) {
        if (pd.tempStorage) {
            delete reinterpret_cast<PropSetStorage*>(pd.tempStorage);
            pd.tempStorage = nullptr;
        }
    }
    inst.paramDefs.clear();
    inst.clips.clear();
}
