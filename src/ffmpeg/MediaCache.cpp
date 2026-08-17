// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MediaCache.h"

#include <QThread>
#include <QMetaType>
#include <QMetaObject>
#include <QDebug>
#include <QFileInfo>
#include <cmath>
#include <algorithm>

namespace {
constexpr int kPeaksPerSecond = 50;
constexpr int kThumbMaxWidth = 160;
constexpr int kMaxPeakCache = 32;
constexpr int kMaxThumbCache = 512;
constexpr int kMaxThumbPending = 192;
constexpr int kMaxPeakPending = 32;

bool isImageFile(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    static const QStringList exts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("svg")
    };
    return exts.contains(ext);
}

QImage loadImageThumb(const QString& path, int maxWidth) {
    QImage img(path);
    if (img.isNull()) return img;
    if (maxWidth > 0 && img.width() > maxWidth) {
        img = img.scaledToWidth(maxWidth, Qt::SmoothTransformation);
    }
    return img;
}

double thumbKey(double seconds) {
    return std::round(seconds * 10.0) / 10.0;
}
}

CacheWorker::CacheWorker(QObject* parent) : QObject(parent) {}

void CacheWorker::generatePeaks(const QString& filePath, int streamIndex,
                                int bucketsPerSecond) {
    const FFmpegAudioPeaks peaks = FFmpegDecoder::audioPeaks(filePath, bucketsPerSecond,
                                                             streamIndex);
    emit peaksReady(filePath, streamIndex, peaks);
}

void CacheWorker::generateThumb(const QString& filePath, double seconds) {
    QImage img;
    // Imagem estática: usa QImage direto (FFmpeg não consegue seek em frame único).
    if (isImageFile(filePath)) {
        img = loadImageThumb(filePath, kThumbMaxWidth);
    } else {
        if (!m_decoder.isOpen() || m_decoder.source() != filePath) {
            m_decoder.open(filePath);
        }
        if (m_decoder.isOpen()) {
            img = m_decoder.frameAt(seconds, kThumbMaxWidth);
            m_decoder.releaseBuffers();
        }
    }
    emit thumbReady(filePath, seconds, img);
}

// Vários instantes do mesmo arquivo: ordena para decodificar em sequência e
// reaproveitar o decoder aberto (sem re-abrir o arquivo a cada pedido).
void CacheWorker::generateThumbs(const QString& filePath, const QList<double>& seconds) {
    // Imagem estática: todas as thumbnails são a mesma imagem carregada via QImage.
    if (isImageFile(filePath)) {
        const QImage img = loadImageThumb(filePath, kThumbMaxWidth);
        for (double s : seconds)
            emit thumbReady(filePath, s, img);
        return;
    }
    if (!m_decoder.isOpen() || m_decoder.source() != filePath) {
        m_decoder.open(filePath);
    }
    QList<double> sorted = seconds;
    std::sort(sorted.begin(), sorted.end());
    for (double s : sorted) {
        QImage img;
        if (m_decoder.isOpen())
            img = m_decoder.frameAt(s, kThumbMaxWidth);
        emit thumbReady(filePath, s, img);
    }
    m_decoder.releaseBuffers();
}

MediaCache& MediaCache::instance() {
    static MediaCache cache;
    return cache;
}

MediaCache::MediaCache() {
    qRegisterMetaType<FFmpegAudioPeaks>("FFmpegAudioPeaks");
    qRegisterMetaType<QList<double>>("QList<double>");

    // Dois workers em threads separadas: gerar os picos de um áudio longo
    // não deve travar a geração de thumbnails (e vice-versa).
    m_peaksThread = new QThread(this);
    m_peaksWorker = new CacheWorker;
    m_peaksWorker->moveToThread(m_peaksThread);
    connect(m_peaksThread, &QThread::finished, m_peaksWorker, &QObject::deleteLater);
    connect(m_peaksWorker, &CacheWorker::peaksReady,
            this, &MediaCache::onPeaksReady, Qt::QueuedConnection);
    m_peaksThread->start();

    m_thumbsThread = new QThread(this);
    m_thumbsWorker = new CacheWorker;
    m_thumbsWorker->moveToThread(m_thumbsThread);
    connect(m_thumbsThread, &QThread::finished, m_thumbsWorker, &QObject::deleteLater);
    connect(m_thumbsWorker, &CacheWorker::thumbReady,
            this, &MediaCache::onThumbReady, Qt::QueuedConnection);
    m_thumbsThread->start();
}

MediaCache::~MediaCache() {
    if (m_peaksThread) {
        m_peaksThread->quit();
        m_peaksThread->wait(2000);
    }
    if (m_thumbsThread) {
        m_thumbsThread->quit();
        m_thumbsThread->wait(2000);
    }
}

const FFmpegAudioPeaks& MediaCache::peaks(const QString& filePath, int streamIndex) const {
    const auto it = m_peaks.constFind(qMakePair(filePath, streamIndex));
    if (it == m_peaks.constEnd()) return m_emptyPeaks;
    touchPeaks(qMakePair(filePath, streamIndex));
    return it.value();
}

QImage MediaCache::thumb(const QString& filePath, double seconds) const {
    const auto key = qMakePair(filePath, thumbKey(seconds));
    const auto it = m_thumbs.constFind(key);
    if (it == m_thumbs.constEnd()) return QImage();
    touchThumb(key);
    return it.value();
}

bool MediaCache::hasPeaks(const QString& filePath, int streamIndex) const {
    return m_peaks.contains(qMakePair(filePath, streamIndex));
}

void MediaCache::requestPeaks(const QString& filePath, int streamIndex) {
    if (!m_peaksWorker) return;
    const auto key = qMakePair(filePath, streamIndex);
    if (m_peaksPending.size() >= kMaxPeakPending) return;
    if (m_peaks.contains(key) || m_peaksPending.contains(key)) return;
    const bool wasBusy = busy();
    m_peaksPending.insert(key, m_epoch);
    QMetaObject::invokeMethod(m_peaksWorker, "generatePeaks", Qt::QueuedConnection,
                              Q_ARG(QString, filePath), Q_ARG(int, streamIndex),
                              Q_ARG(int, kPeaksPerSecond));
    if (!wasBusy) emit busyChanged(true);
}

void MediaCache::requestThumb(const QString& filePath, double seconds) {
    if (!m_thumbsWorker) return;
    if (m_thumbsPending.size() >= kMaxThumbPending) return;
    const double k = thumbKey(seconds);
    const auto key = qMakePair(filePath, k);
    if (m_thumbs.contains(key) || m_thumbsPending.contains(key)
        || m_thumbsFailed.contains(key)) return;
    const bool wasBusy = busy();
    if (m_playbackActive) {
        // Reproduzindo: o thumb é estático, pode esperar. Evita que a
        // decodificação em background dispute CPU com o preview e cause
        // stutter. Os pedidos são liberados ao pausar (setPlaybackActive).
        if (m_thumbsDeferred.size() >= kMaxThumbPending) return;
        m_thumbsDeferred.insert(key);
        return;
    }
    m_thumbsPending.insert(key, m_epoch);
    QMetaObject::invokeMethod(m_thumbsWorker, "generateThumb", Qt::QueuedConnection,
                              Q_ARG(QString, filePath), Q_ARG(double, k));
    if (!wasBusy) emit busyChanged(true);
}

void MediaCache::requestThumbs(const QString& filePath, const QList<double>& seconds) {
    if (!m_thumbsWorker) return;
    // Filtra apenas os instantes ainda não disponíveis/pendentes e envia um
    // único pedido: o worker decodifica todos de uma vez na mesma passada.
    QList<double> missing;
    for (double s : seconds) {
        if (missing.size() >= kMaxThumbPending) break;
        const double k = thumbKey(s);
        const auto key = qMakePair(filePath, k);
        if (m_thumbs.contains(key) || m_thumbsPending.contains(key)
            || m_thumbsFailed.contains(key)) continue;
        if (m_playbackActive) {
            if (m_thumbsDeferred.size() >= kMaxThumbPending) break;
            m_thumbsDeferred.insert(key);
            continue;
        }
        m_thumbsPending.insert(key, m_epoch);
        missing.append(k);
    }
    if (missing.isEmpty()) return;
    const bool wasBusy = busy();
    QMetaObject::invokeMethod(m_thumbsWorker, "generateThumbs", Qt::QueuedConnection,
                              Q_ARG(QString, filePath), Q_ARG(QList<double>, missing));
    if (!wasBusy) emit busyChanged(true);
}

void MediaCache::setPlaybackActive(bool active) {
    m_playbackActive = active;
    if (active) return;
    // Pausou: libera os pedidos adiados, agrupando por arquivo para aproveitar
    // uma única passada de decodificação por caminho.
    QHash<QString, QList<double>> wantByFile;
    for (auto it = m_thumbsDeferred.constBegin(); it != m_thumbsDeferred.constEnd(); ++it) {
        const auto key = *it;
        if (m_thumbs.contains(key) || m_thumbsFailed.contains(key)) continue;
        if (m_thumbsPending.contains(key)) continue;
        wantByFile[key.first].append(key.second);
    }
    m_thumbsDeferred.clear();
    const bool wasBusy = busy();
    for (auto it = wantByFile.constBegin(); it != wantByFile.constEnd(); ++it) {
        if (m_thumbsPending.size() >= kMaxThumbPending) break;
        QList<double> missing;
        for (double s : it.value()) {
            if (m_thumbsPending.size() >= kMaxThumbPending) break;
            const auto key = qMakePair(it.key(), thumbKey(s));
            if (m_thumbs.contains(key) || m_thumbsFailed.contains(key)
                || m_thumbsPending.contains(key)) continue;
            m_thumbsPending.insert(key, m_epoch);
            missing.append(thumbKey(s));
        }
        if (missing.isEmpty()) continue;
        QMetaObject::invokeMethod(m_thumbsWorker, "generateThumbs", Qt::QueuedConnection,
                                  Q_ARG(QString, it.key()), Q_ARG(QList<double>, missing));
    }
    if (!wasBusy && busy()) emit busyChanged(true);
}

void MediaCache::clear() {
    const bool wasBusy = busy();
    ++m_epoch;
    m_peaks.clear();
    m_peaksOrder.clear();
    m_thumbs.clear();
    m_thumbsFailed.clear();
    m_thumbsOrder.clear();
    m_peaksPending.clear();
    m_thumbsPending.clear();
    m_thumbsDeferred.clear();
    if (wasBusy) emit busyChanged(false);
}

void MediaCache::onPeaksReady(const QString& filePath, int streamIndex,
                              const FFmpegAudioPeaks& peaks) {
    const auto key = qMakePair(filePath, streamIndex);
    const auto it = m_peaksPending.constFind(key);
    if (it == m_peaksPending.constEnd()) return;            // já limpo/descartado
    if (it.value() != m_epoch) {                            // projeto antigo
        m_peaksPending.remove(key);
        return;
    }
    m_peaksPending.erase(it);
    m_peaks[key] = peaks;
    m_peaksOrder.removeOne(key);
    m_peaksOrder.append(key);
    evictPeaks();
    emit waveformReady(filePath, streamIndex);
    if (!busy()) emit busyChanged(false);
}

void MediaCache::onThumbReady(const QString& filePath, double seconds, const QImage& image) {
    const auto key = qMakePair(filePath, thumbKey(seconds));
    const auto it = m_thumbsPending.constFind(key);
    if (it == m_thumbsPending.constEnd()) return;           // já limpo/descartado
    if (it.value() != m_epoch) {                            // projeto antigo
        m_thumbsPending.remove(key);
        return;
    }
    m_thumbsPending.erase(it);
    if (image.isNull()) {
        m_thumbsFailed.insert(key);
        if (!busy()) emit busyChanged(false);
        return;
    }
    m_thumbs[key] = image;
    m_thumbsOrder.removeOne(key);
    m_thumbsOrder.append(key);
    evictThumbs();
    emit thumbnailReady(filePath, thumbKey(seconds));
    if (!busy()) emit busyChanged(false);
}

bool MediaCache::busy() const {
    return !m_peaksPending.isEmpty() || !m_thumbsPending.isEmpty();
}

void MediaCache::touchPeaks(const QPair<QString, int>& key) const {
    const int i = m_peaksOrder.indexOf(key);
    if (i >= 0 && i < m_peaksOrder.size() - 1)
        m_peaksOrder.move(i, m_peaksOrder.size() - 1);
}

void MediaCache::touchThumb(const QPair<QString, double>& key) const {
    const int i = m_thumbsOrder.indexOf(key);
    if (i >= 0 && i < m_thumbsOrder.size() - 1)
        m_thumbsOrder.move(i, m_thumbsOrder.size() - 1);
}

void MediaCache::evictPeaks() {
    while (m_peaks.size() > kMaxPeakCache && !m_peaksOrder.isEmpty()) {
        const QPair<QString, int> oldest = m_peaksOrder.takeFirst();
        m_peaks.remove(oldest);
    }
}

void MediaCache::evictThumbs() {
    while (m_thumbs.size() > kMaxThumbCache && !m_thumbsOrder.isEmpty()) {
        const auto oldest = m_thumbsOrder.takeFirst();
        m_thumbs.remove(oldest);
    }
}
