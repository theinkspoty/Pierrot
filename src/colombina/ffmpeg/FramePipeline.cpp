// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "colombina/ffmpeg/FramePipeline.h"

#include "colombina/CrashReporter.h"
#include "colombina/ffmpeg/FrameCache.h"
#include "colombina/ffmpeg/SourcePool.h"
#include "laartman/core/FFmpegDecoder.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QMetaObject>
#include <QThread>

namespace {

bool playDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_PLAY_DEBUG");
    return on;
}

// Worker interno: vive na thread do pipeline. Não tem estado próprio além do
// pool — cada pedido é autônomo. É deliberadamente privado (detalhe de
// implementação do FramePipeline); ninguém de fora o vê nem o aciona.
class PrefetchWorker : public QObject {
    Q_OBJECT
public:
    explicit PrefetchWorker(SourcePool* pool) : m_pool(pool) {}

public slots:
    void decode(const QString& path, double t, int maxW, double step) {
        CrashReporter::setActivity("prefetch do proximo clipe");
        static QElapsedTimer clock;
        const bool dbg = playDbg();
        if (dbg && !clock.isValid()) clock.start();
        const qint64 t0 = dbg ? clock.nsecsElapsed() : 0;

        std::shared_ptr<FFmpegDecoder> d = m_pool->acquire(path);
        if (!d) {
            emit failed(path);
            return;
        }
        const quint64 src = qHash(path);
        QImage frame0 = FrameCache::instance().getOrCompute(
            {src, d->frameCacheIndex(t), maxW}, [&] { return d->frameAt(t, maxW); });
        QImage frame1;
        if (!frame0.isNull() && step > 0.0)
            frame1 = FrameCache::instance().getOrCompute(
                {src, d->frameCacheIndex(t + step), maxW}, [&] { return d->frameAt(t + step, maxW); });

        if (dbg) {
            const double ms = (clock.nsecsElapsed() - t0) / 1000000.0;
            qDebug().noquote() << QStringLiteral("[w] PREFETCH(bg) t=%1 %2ms")
                                      .arg(t, 0, 'f', 3).arg(ms, 0, 'f', 1);
        }
        emit done(path, t, maxW, frame0, frame1);
    }

    void warm(const QString& path, double t, int maxW) {
        CrashReporter::setActivity("aquecendo clipe a frente");
        static QElapsedTimer clock;
        const bool dbg = playDbg();
        if (dbg && !clock.isValid()) clock.start();
        const qint64 t0 = dbg ? clock.nsecsElapsed() : 0;

        std::shared_ptr<FFmpegDecoder> d = m_pool->acquire(path);
        if (d) {
            // abre+posiciona; soft se já quente. Aquece também o cache (L4).
            FrameCache::instance().getOrCompute(
                {qHash(path), d->frameCacheIndex(t), maxW}, [&] { return d->frameAt(t, maxW); });
            if (dbg) {
                const double ms = (clock.nsecsElapsed() - t0) / 1000000.0;
                qDebug().noquote() << QStringLiteral("[w] WARM(bg) t=%1 %2ms")
                                          .arg(t, 0, 'f', 3).arg(ms, 0, 'f', 1);
            }
        }
        emit warmed(path);
    }

signals:
    void done(const QString& path, double t, int maxW,
              const QImage& frame0, const QImage& frame1);
    void failed(const QString& path);
    void warmed(const QString& path);

private:
    SourcePool* m_pool; // não dono
};

} // namespace

// O worker é um QObject definido neste .cpp; para o moc funcionar, o
// Q_OBJECT dele exige que este arquivo seja processado pelo AUTOMOC. Como o
// FramePipeline.h não declara o PrefetchWorker, o moc do .cpp cuida dele via
// #include "FramePipeline.moc" no fim.

FramePipeline::FramePipeline(SourcePool* pool, QObject* parent)
    : QObject(parent), m_pool(pool) {
    auto* worker = new PrefetchWorker(m_pool);
    m_worker = worker;
    // Repassa os sinais do worker para a superfície pública do pipeline, na
    // thread da UI (queued: o worker roda na m_thread).
    connect(worker, &PrefetchWorker::done, this,
            [this](const QString& path, double t, int maxW,
                   const QImage& f0, const QImage& f1) {
                {
                    QMutexLocker l(&m_mutex);
                    m_prefetchBusy = false;
                }
                emit framePrefetched(path, t, maxW, f0, f1);
            });
    connect(worker, &PrefetchWorker::failed, this, [this](const QString& path) {
        {
            QMutexLocker l(&m_mutex);
            m_prefetchBusy = false;
        }
        emit prefetchFailed(path);
    });

    // Thread SEPARADA para o warm-ahead: um open() frio de ~1,3s no warm NÃO
    // pode atrasar o prefetch crítico do próximo corte (frame0), que é o que
    // chega primeiro. Antes os dois dividiam a mesma thread e se serializavam.
    auto* warmWorker = new PrefetchWorker(m_pool);
    m_warmWorker = warmWorker;
    connect(warmWorker, &PrefetchWorker::warmed, this, [this](const QString& path) {
        {
            QMutexLocker l(&m_mutex);
            if (m_warmInFlight == path) m_warmInFlight.clear();
        }
        emit warmed(path);
        dispatchNextWarm();
    });
}

FramePipeline::~FramePipeline() {
    stop();
    delete m_worker;
    delete m_warmWorker;
}

void FramePipeline::start() {
    if (m_started) return;
    m_thread = new CrashReporter::TrackedThread("preview-prefetch", this);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
    m_warmThread = new CrashReporter::TrackedThread("preview-warm", this);
    m_warmWorker->moveToThread(m_warmThread);
    connect(m_warmThread, &QThread::finished, m_warmWorker, &QObject::deleteLater);
    m_warmThread->start();
    m_started = true;
}

void FramePipeline::stop() {
    if (!m_started) return;
    m_thread->quit();
    m_thread->wait(5000);
    m_thread = nullptr;
    m_worker = nullptr; // deleteLater na thread encerrada
    m_warmThread->quit();
    m_warmThread->wait(5000);
    m_warmThread = nullptr;
    m_warmWorker = nullptr;
    m_started = false;
}

void FramePipeline::prefetchFrame(const QString& path, double t, int maxW, double step) {
    if (path.isEmpty()) return;
    {
        QMutexLocker l(&m_mutex);
        if (m_prefetchBusy) return; // serial: um frame0 por vez
        m_prefetchBusy = true;
    }
    if (!m_started) return;
    QMetaObject::invokeMethod(m_worker, "decode", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(double, t),
                              Q_ARG(int, maxW), Q_ARG(double, step));
}

void FramePipeline::setWarmQueue(const QVector<WarmTarget>& targets) {
    {
        QMutexLocker l(&m_mutex);
        m_warmQueue = targets;
        if (m_warmQueue.isEmpty()) m_warmInFlight.clear();
    }
    dispatchNextWarm();
}

void FramePipeline::warmNow(const QString& path, double t, int maxW) {
    if (path.isEmpty() || !m_started) return;
    {
        QMutexLocker l(&m_mutex);
        if (!m_warmInFlight.isEmpty()) return; // um warm por vez
        m_warmInFlight = path;
    }
    QMetaObject::invokeMethod(m_warmWorker, "warm", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(double, t),
                              Q_ARG(int, maxW));
}

bool FramePipeline::warmInFlight(const QString& path) const {
    QMutexLocker l(&m_mutex);
    return m_warmInFlight == path;
}

bool FramePipeline::warmQueueEmpty() const {
    QMutexLocker l(&m_mutex);
    return m_warmQueue.isEmpty();
}

void FramePipeline::dispatchNextWarm() {
    if (!m_started) return;
    QString path;
    double t = 0.0;
    int maxW = 0;
    {
        QMutexLocker l(&m_mutex);
        if (!m_warmInFlight.isEmpty() || m_warmQueue.isEmpty()) return;
        const WarmTarget w = m_warmQueue.takeFirst();
        m_warmInFlight = w.path;
        path = w.path;
        t = w.t;
        maxW = w.maxW;
    }
    QMetaObject::invokeMethod(m_warmWorker, "warm", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(double, t),
                              Q_ARG(int, maxW));
}

void FramePipeline::runPrefetchNow(const QString& path, double t, int maxW,
                                   double step, QImage* frame0, QImage* frame1) {
    std::shared_ptr<FFmpegDecoder> d = m_pool->acquire(path);
    FrameCache& c = FrameCache::instance();
    const quint64 src = qHash(path);
    if (frame0) *frame0 = d ? c.getOrCompute({src, d->frameCacheIndex(t), maxW},
                                             [&] { return d->frameAt(t, maxW); })
                            : QImage();
    if (frame1) *frame1 = (d && !frame0->isNull() && step > 0.0)
                              ? c.getOrCompute({src, d->frameCacheIndex(t + step), maxW},
                                               [&] { return d->frameAt(t + step, maxW); })
                              : QImage();
}

#include "FramePipeline.moc"
