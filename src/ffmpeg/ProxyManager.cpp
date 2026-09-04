// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ProxyManager.h"

#include "ffmpeg/FFmpegDecoder.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QMutexLocker>
#include <QtDebug>

namespace {
QString ffmpegExe() {
    return QStringLiteral("ffmpeg");
}
}

// ── ProxyWorker ─────────────────────────────────────────────────────────

void ProxyWorker::process(const QString& srcPath, const QString& proxyPath) {
    // Transcoda para H.264 baixo bitrate, metade da largura (mantém AR),
    // vídeo-apenas (-an). Escala 1920 na maior dimensão (suficiente p/ preview)
    // mantendo o aspect ratio. Gera um arquivo temporário e move por cima só
    // quando terminado, para nunca deixar um proxy parcial.
    const QString tmp = proxyPath + QStringLiteral(".tmp");
    QFile::remove(tmp);

    QProcess proc;
    QStringList args;
    args << QStringLiteral("-y")
         << QStringLiteral("-i") << srcPath
         << QStringLiteral("-vf") << QStringLiteral("scale='min(1920,iw)':-2")
         << QStringLiteral("-c:v") << QStringLiteral("libx264")
         << QStringLiteral("-preset") << QStringLiteral("veryfast")
         << QStringLiteral("-crf") << QStringLiteral("28")
         << QStringLiteral("-an")
         << QStringLiteral("-sn")
         << tmp;

    proc.start(ffmpegExe(), args);
    if (!proc.waitForStarted(5000)) {
        emit proxyFailed(srcPath);
        return;
    }
    if (!proc.waitForFinished(1200000)) { // 20 min de teto
        proc.kill();
        proc.waitForFinished(2000);
        emit proxyFailed(srcPath);
        return;
    }
    if (proc.exitCode() != 0 || !QFileInfo::exists(tmp)) {
        QFile::remove(tmp);
        emit proxyFailed(srcPath);
        return;
    }
    QFile::remove(proxyPath);
    if (!QFile::rename(tmp, proxyPath)) {
        QFile::remove(tmp);
        emit proxyFailed(srcPath);
        return;
    }
    emit proxyReady(srcPath, proxyPath);
}

// ── ProxyManager ────────────────────────────────────────────────────────

ProxyManager& ProxyManager::instance() {
    static ProxyManager mgr;
    return mgr;
}

ProxyManager::~ProxyManager() {
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(3000);
    }
}

ProxyManager::ProxyManager() {
    QDir().mkpath(proxyDir());
    m_stateFile = proxyDir() + QStringLiteral("/metadata.json");
    loadState();

    m_thread = new QThread(this);
    m_worker = new ProxyWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &ProxyManager::startJob, m_worker, &ProxyWorker::process,
            Qt::QueuedConnection);
    connect(m_worker, &ProxyWorker::proxyReady,
            this, &ProxyManager::onProxyReady, Qt::QueuedConnection);
    connect(m_worker, &ProxyWorker::proxyFailed,
            this, &ProxyManager::onProxyFailed, Qt::QueuedConnection);
    m_thread->start();
}

QString ProxyManager::proxyDir() const {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + QStringLiteral("/proxies");
}

QString ProxyManager::proxyPathFor(const QString& srcPath) const {
    // Hash do caminho → nome de arquivo seguro e estável no disco.
    const QByteArray h = QCryptographicHash::hash(srcPath.toUtf8(), QCryptographicHash::Md5).toHex();
    return proxyDir() + QStringLiteral("/") + QString::fromLatin1(h) + QStringLiteral(".mp4");
}

QString ProxyManager::resolveVideo(const QString& srcPath) const {
    QMutexLocker l(&m_mutex);
    if (!m_enabled || srcPath.isEmpty()) return srcPath;
    if (m_small.contains(srcPath)) return srcPath;
    const QString proxy = proxyPathFor(srcPath);
    if (m_map.value(srcPath) == proxy && QFile::exists(proxy))
        return proxy;
    return srcPath;
}

bool ProxyManager::hasProxy(const QString& srcPath) const {
    QMutexLocker l(&m_mutex);
    if (srcPath.isEmpty()) return false;
    const QString proxy = proxyPathFor(srcPath);
    return m_map.value(srcPath) == proxy && QFile::exists(proxy);
}

void ProxyManager::probeAndQueue(const QString& srcPath) {
    QMutexLocker l(&m_mutex);
    if (!m_enabled || srcPath.isEmpty() || m_small.contains(srcPath) || m_failed.contains(srcPath))
        return;
    if (hasProxy(srcPath)) return;
    if (m_map.contains(srcPath)) return; // já gera/gerou (removido? re-probe)
    if (m_pending.contains(srcPath) || m_activeSrc == srcPath) return;

    // Probe leve: só precisa da largura. Não abre stream pesado.
    const FFmpegMediaInfo info = FFmpegDecoder::probe(srcPath);
    if (!info.hasVideo) { m_small.insert(srcPath); return; }
    if (info.width > 0 && info.width < kThresholdWidth) {
        m_small.insert(srcPath);
        return;
    }

    m_pending.insert(srcPath);
    const bool busy = !m_pending.isEmpty() || m_running;
    l.unlock();
    emit busyChanged(busy);
    pump();
}

void ProxyManager::pump() {
    if (m_running || m_pending.isEmpty()) return;
    QString src = *m_pending.begin();
    m_pending.remove(src);
    m_activeSrc = src;
    m_running = true;
    emit busyChanged(true);
    emit startJob(src, proxyPathFor(src));
}

void ProxyManager::onProxyReady(const QString& srcPath, const QString& proxyPath) {
    {
        QMutexLocker l(&m_mutex);
        m_map.insert(srcPath, proxyPath);
        saveState();
    }
    m_running = false;
    m_activeSrc.clear();
    emit proxyReady(srcPath);
    emit busyChanged(!m_pending.isEmpty() || m_running);
    pump();
}

void ProxyManager::onProxyFailed(const QString& srcPath) {
    {
        QMutexLocker l(&m_mutex);
        m_failed.insert(srcPath);
        saveState();
    }
    m_running = false;
    m_activeSrc.clear();
    emit proxyFailed(srcPath);
    emit busyChanged(!m_pending.isEmpty() || m_running);
    pump();
}

void ProxyManager::clear() {
    m_pending.clear();
    m_activeSrc.clear();
    // Não apaga proxies existentes (são cache reutilizável); só zera a fila.
}

void ProxyManager::loadState() {
    QFile f(m_stateFile);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    const QJsonObject obj = doc.object();
    const QJsonArray arr = obj.value(QStringLiteral("proxies")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        m_map.insert(o.value(QStringLiteral("src")).toString(),
                     o.value(QStringLiteral("proxy")).toString());
    }
    m_small.clear();
    // Valida: se o proxy sumiu do disco, remove do estado (será re-gerado).
    QMutableHashIterator<QString, QString> it(m_map);
    while (it.hasNext()) {
        it.next();
        if (!QFile::exists(it.value()))
            it.remove();
    }
}

void ProxyManager::saveState() const {
    QFile f(m_stateFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QJsonArray arr;
    for (auto it = m_map.cbegin(); it != m_map.cend(); ++it) {
        QJsonObject o;
        o.insert(QStringLiteral("src"), it.key());
        o.insert(QStringLiteral("proxy"), it.value());
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("proxies"), arr);
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}
