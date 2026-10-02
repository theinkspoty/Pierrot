// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "PreviewProfiler.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>

namespace {
// Relógio compartilhado: um QElapsedTimer estático é monotônico, sem custo de
// syscall (clock_gettime vDSO) e vale para todos os call sites.
QElapsedTimer& monoClock() {
    static QElapsedTimer t;
    if (!t.isValid()) t.start();
    return t;
}
} // namespace

PreviewProfiler& PreviewProfiler::instance() {
    static PreviewProfiler p;
    return p;
}

bool PreviewProfiler::active() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_PERF_DEBUG")
                        || qEnvironmentVariableIsSet("PIERROT_PERF_JSON");
    return on;
}

qint64 PreviewProfiler::nowNs() {
    return monoClock().nsecsElapsed();
}

void PreviewProfiler::beginSession(const QString& label, bool proxyWanted, double projectFps) {
    QMutexLocker l(&m_mutex);
    m_frames.clear();
    m_label = label;
    m_proxyWanted = proxyWanted;
    m_projectFps = projectFps;
    m_proxyFrames = 0;
    m_cuts = 0;
    m_compHits = 0;
    m_compMiss = 0;
    m_spanStart = -1.0;
    m_spanEnd = 0.0;
    m_stallMs = 0.0;
    m_stallMaxMs = 0.0;
    m_stallTotalMs = 0.0;
    m_stallEvents = 0;
}

void PreviewProfiler::add(const Frame& f) {
    QMutexLocker l(&m_mutex);
    m_frames.append(f);
    if (f.proxy) ++m_proxyFrames;
    if (f.cut) ++m_cuts;
    m_stallMs = f.stallNs / 1e6;
    if (m_stallMs > m_stallMaxMs) m_stallMaxMs = m_stallMs;
    m_stallTotalMs += m_stallMs;
    if (m_stallMs > 1.0) ++m_stallEvents;

    const double end = f.playhead + 1.0 / std::max(1.0, f.fps);
    if (m_spanStart < 0.0 || f.playhead < m_spanStart) m_spanStart = f.playhead;
    if (end > m_spanEnd) m_spanEnd = end;
    // Só conta o que passou pelo caminho de composição: antes, todo quadro sem
    // composição (preview de camada única) era contado como "miss", o que
    // transformava um número honesto num alerta falso permanente.
    if (f.compositeEvaluated) {
        if (f.compositeHit) ++m_compHits; else ++m_compMiss;
    }
}

void PreviewProfiler::endSession() {
    // A sessão não é "fechada" por software: o fim é definido pelo chamador.
    // Mantido como ponto de extensão explícito para quando houver flush
    // intermediário (ex.: relatório a cada 30 s durante um teste longo).
}

// Extrai um campo de duração (ns) do registro. Chamado só na hora de gerar o
// relatório — o if-chain custa irrelevante e evita offsetof/reinterpret_cast.
static bool frameField(const PreviewProfiler::Frame& f, const QString& field, double& out) {
    const QStringView v(field);
    if      (v == u"tickNs")        out = f.tickNs;
    else if (v == u"clockNs")       out = f.clockNs;
    else if (v == u"seekNs")        out = f.seekNs;
    else if (v == u"prefetchNs")    out = f.prefetchNs;
    else if (v == u"mixNs")         out = f.mixNs;
    else if (v == u"workerNs")      out = f.workerNs;
    else if (v == u"prefetchLatNs") out = f.prefetchLatNs;
    else if (v == u"paintNs")       out = f.paintNs;
    else if (v == u"compositeNs")   out = f.compositeNs;
    else if (v == u"resolveNs")     out = f.resolveNs;
    else return false;
    return true;
}

PreviewProfiler::Stats PreviewProfiler::stats(const QString& field) const {
    QMutexLocker l(&m_mutex);
    QVector<double> v;
    v.reserve(m_frames.size());
    for (const Frame& f : m_frames) {
        double d = 0.0;
        if (frameField(f, field, d)) v.append(d);
    }
    if (v.isEmpty()) return {};

    std::sort(v.begin(), v.end());
    Stats s;
    s.n = v.size();
    for (double d : v) s.mean += d;
    s.mean /= v.size();
    const auto q = [&](double p) {
        return v[std::min<qsizetype>(v.size() - 1, qsizetype(p * v.size()))];
    };
    s.p50 = q(0.50);
    s.p95 = q(0.95);
    s.p99 = q(0.99);
    s.max = v.last();
    // ns -> ms na saída: o relatório é lido em milissegundos.
    s.mean /= 1e6; s.p50 /= 1e6; s.p95 /= 1e6; s.p99 /= 1e6; s.max /= 1e6;
    return s;
}

int PreviewProfiler::frameCount() const {
    QMutexLocker l(&m_mutex);
    return m_frames.size();
}

qint64 PreviewProfiler::droppedTotal() const {
    QMutexLocker l(&m_mutex);
    qint64 last = 0;
    for (const Frame& f : m_frames) last = f.dropped;
    return last;
}

bool PreviewProfiler::wroteProxy() const {
    QMutexLocker l(&m_mutex);
    return m_proxyFrames > 0;
}

int PreviewProfiler::proxyFrames() const {
    QMutexLocker l(&m_mutex);
    return int(m_proxyFrames);
}

int PreviewProfiler::cuts() const {
    QMutexLocker l(&m_mutex);
    return int(m_cuts);
}

int PreviewProfiler::compositeHits() const {
    QMutexLocker l(&m_mutex);
    return int(m_compHits);
}

int PreviewProfiler::compositeMisses() const {
    QMutexLocker l(&m_mutex);
    return int(m_compMiss);
}

double PreviewProfiler::playbackSpan() const {
    QMutexLocker l(&m_mutex);
    return m_spanStart < 0.0 ? 0.0 : m_spanEnd - m_spanStart;
}

QString PreviewProfiler::label() const {
    QMutexLocker l(&m_mutex);
    return m_label;
}

double PreviewProfiler::projectFps() const {
    QMutexLocker l(&m_mutex);
    return m_projectFps;
}

QString PreviewProfiler::toJson() const {
    QMutexLocker l(&m_mutex);

    const auto fieldStats = [&](const char* name) {
        QVector<double> v;
        v.reserve(m_frames.size());
        for (const Frame& f : m_frames) {
            double d = 0.0;
            if (frameField(f, QString::fromLatin1(name), d)) v.append(d);
        }
        QJsonObject o;
        if (v.isEmpty()) { o.insert(QStringLiteral("n"), 0); return o; }
        std::sort(v.begin(), v.end());
        const auto q = [&](double p) {
            return v[std::min<qsizetype>(v.size() - 1, qsizetype(p * v.size()))] / 1e6;
        };
        double mean = 0.0;
        for (double d : v) mean += d;
        o.insert(QStringLiteral("n"), v.size());
        o.insert(QStringLiteral("meanMs"), mean / v.size() / 1e6);
        o.insert(QStringLiteral("p50Ms"), q(0.50));
        o.insert(QStringLiteral("p95Ms"), q(0.95));
        o.insert(QStringLiteral("p99Ms"), q(0.99));
        o.insert(QStringLiteral("maxMs"), v.last() / 1e6);
        return o;
    };

    int prefetchHits = 0, lowerReq = 0;
    int droppedLast = 0;
    long long skippedSum = 0;
    for (const Frame& f : m_frames) {
        if (f.prefetchHit) ++prefetchHits;
        lowerReq += f.lowerReq;
        droppedLast = f.dropped;
        skippedSum += f.skipped;
    }

    QJsonObject root;
    root.insert(QStringLiteral("label"), m_label);
    root.insert(QStringLiteral("proxyWanted"), m_proxyWanted);
    root.insert(QStringLiteral("projectFps"), m_projectFps);
    root.insert(QStringLiteral("frames"), m_frames.size());
    root.insert(QStringLiteral("playbackSpanSec"), m_spanEnd - std::max(0.0, m_spanStart));
    root.insert(QStringLiteral("stallMaxMs"), m_stallMaxMs);
    root.insert(QStringLiteral("stallTotalMs"), m_stallTotalMs);
    root.insert(QStringLiteral("stallEvents"), qint64(m_stallEvents));
    root.insert(QStringLiteral("proxyFrames"), qint64(m_proxyFrames));
    root.insert(QStringLiteral("sourceUsed"),
                m_proxyFrames > 0 ? QStringLiteral("proxy")
                                  : (m_frames.isEmpty() ? QStringLiteral("n/d")
                                                        : QStringLiteral("original")));
    root.insert(QStringLiteral("cuts"), qint64(m_cuts));
    root.insert(QStringLiteral("droppedTotal"), m_frames.isEmpty() ? 0 : m_frames.back().droppedTotal);
    const qint64 fcHits = m_frames.isEmpty() ? 0 : qint64(m_frames.back().cacheHits);
    const qint64 fcMiss = m_frames.isEmpty() ? 0 : qint64(m_frames.back().cacheMisses);
    root.insert(QStringLiteral("cacheHits"), fcHits);
    root.insert(QStringLiteral("cacheMisses"), fcMiss);
    root.insert(QStringLiteral("skippedTotal"), skippedSum);
    // Quadros que o playhead pediu e o pipeline não entregou a tempo.
    root.insert(QStringLiteral("tickRateHz"),
                m_spanEnd > m_spanStart
                    ? double(m_frames.size()) / (m_spanEnd - m_spanStart) : 0.0);
    root.insert(QStringLiteral("prefetchHits"), prefetchHits);
    root.insert(QStringLiteral("compositeCacheHits"), qint64(m_compHits));
    root.insert(QStringLiteral("compositeCacheMisses"), qint64(m_compMiss));
    root.insert(QStringLiteral("lowerLayerReqs"), lowerReq);
    {
        QJsonObject ticks;
        ticks.insert(QStringLiteral("tickNs"), fieldStats("tickNs"));
        ticks.insert(QStringLiteral("clockNs"), fieldStats("clockNs"));
        ticks.insert(QStringLiteral("seekNs"), fieldStats("seekNs"));
        ticks.insert(QStringLiteral("prefetchNs"), fieldStats("prefetchNs"));
        ticks.insert(QStringLiteral("mixNs"), fieldStats("mixNs"));
        root.insert(QStringLiteral("stages"), ticks);
    }
    {
        QJsonObject async_;
        async_.insert(QStringLiteral("workerNs"), fieldStats("workerNs"));
        async_.insert(QStringLiteral("prefetchLatNs"), fieldStats("prefetchLatNs"));
        root.insert(QStringLiteral("asyncStages"), async_);
    }
    {
        QJsonObject paint;
        paint.insert(QStringLiteral("paintNs"), fieldStats("paintNs"));
        paint.insert(QStringLiteral("compositeNs"), fieldStats("compositeNs"));
        paint.insert(QStringLiteral("resolveNs"), fieldStats("resolveNs"));
        root.insert(QStringLiteral("paintStages"), paint);
    }

    // Série completa: só com PIERROT_PERF_VERBOSE=1 (o JSON de resumo já basta
    // para o A/B; a série é para quando o número não explica o sintoma).
    if (qEnvironmentVariableIsSet("PIERROT_PERF_VERBOSE")) {
        QJsonArray arr;
        const auto dbl = [](double v) { return std::isfinite(v) ? v / 1e6 : 0.0; };
        for (const Frame& f : m_frames) {
            QJsonObject o;
            o.insert(QStringLiteral("seq"), f.seq);
            o.insert(QStringLiteral("fr"), qint64(f.frameIdx));
            o.insert(QStringLiteral("dseek"), f.decSeekNs / 1e6);
            o.insert(QStringLiteral("ddrop"), qint64(f.decDiscard));
            o.insert(QStringLiteral("dwork"), f.decWorkNs / 1e6);
            o.insert(QStringLiteral("ph"), f.playhead);
            o.insert(QStringLiteral("tick"), dbl(f.tickNs));
            o.insert(QStringLiteral("seek"), dbl(f.seekNs));
            o.insert(QStringLiteral("pf"), dbl(f.prefetchNs));
            o.insert(QStringLiteral("mix"), dbl(f.mixNs));
            o.insert(QStringLiteral("wrk"), dbl(f.workerNs));
            o.insert(QStringLiteral("pflat"), dbl(f.prefetchLatNs));
            o.insert(QStringLiteral("paint"), dbl(f.paintNs));
            o.insert(QStringLiteral("comp"), dbl(f.compositeNs));
            o.insert(QStringLiteral("res"), dbl(f.resolveNs));
            o.insert(QStringLiteral("drop"), f.dropped);
            o.insert(QStringLiteral("skip"), f.skipped);
            o.insert(QStringLiteral("q"), f.queue);
            o.insert(QStringLiteral("layers"), f.layers);
            o.insert(QStringLiteral("lower"), f.lowerReq);
            o.insert(QStringLiteral("px"), f.proxy);
            o.insert(QStringLiteral("pfhit"), f.prefetchHit);
            o.insert(QStringLiteral("chit"), f.compositeHit);
            o.insert(QStringLiteral("stall"), f.stallNs / 1e6);
            o.insert(QStringLiteral("wall"), double(f.wallMs));
            o.insert(QStringLiteral("cut"), f.cut);
            arr.append(o);
        }
        root.insert(QStringLiteral("series"), arr);
    }

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool PreviewProfiler::dumpJson(const QString& path) const {
    const QString json = toJson();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    return f.write(json.toUtf8()) >= 0;
}