// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ProxyManager.h"

#include "laartman/FFmpegDecoder.h"

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
#include <QSettings>
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
    // O temporário PRECISA manter a extensão do container: com "...mp4.tmp" o
    // ffmpeg não consegue inferir o formato de saída e aborta com exit 234
    // ("Unable to choose an output format"), o que fazia TODA geração de proxy
    // falhar silenciosamente, em qualquer resolução.
    const QString tmp = proxyPath + QStringLiteral(".tmp.mp4");
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
        qWarning("[proxy] nao consegui iniciar ffmpeg para %s: %s",
                 qPrintable(srcPath), qPrintable(proc.errorString()));
        emit proxyFailed(srcPath);
        return;
    }
    if (!proc.waitForFinished(1200000)) { // 20 min de teto
        proc.kill();
        proc.waitForFinished(2000);
        qWarning("[proxy] tempo esgotado (20 min) em %s", qPrintable(srcPath));
        emit proxyFailed(srcPath);
        return;
    }
    if (proc.exitCode() != 0 || !QFileInfo::exists(tmp)) {
        // A falha era silenciosa: o preview só continuava usando o original e
        // nada indicava por quê. A cauda do stderr do ffmpeg é o que separa
        // "codec ausente", "disco cheio" e "fonte corrompida".
        const QByteArray errTail = proc.readAllStandardError().right(600);
        qWarning("[proxy] ffmpeg falhou (exit %d) em %s\n%s",
                 proc.exitCode(), qPrintable(srcPath), errTail.constData());
        QFile::remove(tmp);
        emit proxyFailed(srcPath);
        return;
    }
    if (!QFile::rename(tmp, proxyPath)) {
        qWarning("[proxy] rename falhou (%s -> %s)",
                 qPrintable(tmp), qPrintable(proxyPath));
        QFile::remove(tmp);
        emit proxyFailed(srcPath);
        return;
    }
    qInfo("[proxy] pronto: %s (%lld KB)",
          qPrintable(srcPath), static_cast<long long>(QFileInfo(proxyPath).size() / 1024));
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
    m_enabled = QSettings().value("proxiesEnabled", true).toBool();
    // Limiar sobrescrevível: o padrão ignora fontes < 2560px, o que numa
    // biblioteca só 1080p significa proxy nunca gerado — e o proxy é gerado a
    // 1920px, então para uma fonte 1080p ele ainda é uma reencode bem mais
    // barata de decodificar. PIERROT_PROXY_MIN_WIDTH=<px> sobrescreve (0 = não
    // filtrar por largura).
    bool ok = false;
    const int envW = qEnvironmentVariableIntValue("PIERROT_PROXY_MIN_WIDTH", &ok);
    m_thresholdWidth = (ok && envW >= 0) ? envW : kThresholdWidth;
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
    if (!m_enabled || !m_projectUsesProxies || srcPath.isEmpty()) return srcPath;
    if (m_small.contains(srcPath)) return srcPath;
    // Memoizado: sem isto cada chamada fazia MD5 do caminho completo +
    // QStandardPaths::writableLocation() + QFile::exists() (um stat), tudo sob
    // o mutex — e isso é chamado várias vezes por quadro só para descobrir que
    // a resposta não mudou. Custo medido: 0,13 ms por quadro (era ~1/3 do tick
    // inteiro com proxy ligado).
    const auto it = m_resolved.constFind(srcPath);
    if (it != m_resolved.constEnd()) return it.value();
    const QString proxy = proxyPathFor(srcPath);
    const QString out = (m_map.value(srcPath) == proxy && QFile::exists(proxy))
                            ? proxy : srcPath;
    m_resolved.insert(srcPath, out);
    return out;
}

bool ProxyManager::hasProxy(const QString& srcPath) const {
    QMutexLocker l(&m_mutex);
    if (srcPath.isEmpty()) return false;
    const QString proxy = proxyPathFor(srcPath);
    return m_map.value(srcPath) == proxy && QFile::exists(proxy);
}

void ProxyManager::probeAndQueue(const QString& srcPath) {
    QMutexLocker l(&m_mutex);
    if (!m_enabled) { return; }
    if (srcPath.isEmpty()) return;
    if (m_small.contains(srcPath)) { return; }
    if (m_failed.contains(srcPath)) { return; }
    const QString proxy = proxyPathFor(srcPath);
    if (m_map.value(srcPath) == proxy && QFile::exists(proxy)) { return; }
    if (m_map.contains(srcPath)) { return; }
    if (m_pending.contains(srcPath)) { return; }
    if (m_activeSrc == srcPath) { return; }

    // Probe leve: só precisa da largura. Não abre stream pesado.
    const FFmpegMediaInfo info = FFmpegDecoder::probe(srcPath);
    if (!info.hasVideo) { m_small.insert(srcPath); return; }
    // Registrar ANTES da decisão de largura: toda fonte com vídeo é candidata
    // potencial, e é esta lista que permite reavaliar quando o limiar muda.
    m_videoSrcs.insert(srcPath);
    if (info.width > 0 && info.width < m_thresholdWidth) {
        m_small.insert(srcPath);
        return;
    }

    // Candidata a proxy: sempre registrada, mesmo com a preferência do projeto
    // desligada (assim, ao reativar, dá para enfileirar sem re-probar do zero).
    // Preferência do projeto OFF: preview usa o original e nada é gerado agora.
    if (!m_projectUsesProxies) { return; }

    m_pending.insert(srcPath);
    const bool busy = !m_pending.isEmpty() || m_running;
    l.unlock();
    emit busyChanged(busy);
    pump();
}

// Preferência do projeto: quando liga de novo, re-enfileira a geração das
// candidatas conhecidas que ainda não têm proxy. NÃO mexe em proxies já
// gerados (cache reutilizável) nem nos descartados (m_failed: não re-tentar).
void ProxyManager::setProjectUsesProxies(bool on) {
    {
        QMutexLocker l(&m_mutex);
        m_projectUsesProxies = on;
    }
    if (!on) return;
    QStringList toQueue;
    {
        QMutexLocker l(&m_mutex);
        for (auto it = m_videoSrcs.cbegin(); it != m_videoSrcs.cend(); ++it) {
            const QString proxy = proxyPathFor(*it);
            const bool has = m_map.value(*it) == proxy && QFile::exists(proxy);
            if (!has) toQueue.append(*it);
        }
    }
    for (const QString& s : toQueue)
        probeAndQueue(s);
}

bool ProxyManager::projectUsesProxies() const {
    QMutexLocker l(&m_mutex);
    return m_projectUsesProxies;
}

bool ProxyManager::busy() const {
    QMutexLocker l(&m_mutex);
    return !m_pending.isEmpty() || m_running;
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
        m_resolved.insert(srcPath, proxyPath);
    }
    saveState(); // I/O outside mutex to avoid blocking resolveVideo()
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
        // O proxy não veio: a fonte volta a ser o original.
        m_resolved.remove(srcPath);
    }
    saveState(); // I/O outside mutex to avoid blocking resolveVideo()
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
        m_resolved.clear(); // revalidado na primeira resolveVideo()
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
