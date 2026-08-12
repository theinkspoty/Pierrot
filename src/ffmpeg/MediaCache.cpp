#include "MediaCache.h"

#include <QThread>
#include <QMetaType>
#include <QMetaObject>
#include <cmath>

namespace {
constexpr int kPeaksPerSecond = 50;
constexpr int kThumbMaxWidth = 320;
constexpr int kMaxPeakCache = 24;
constexpr int kMaxThumbCache = 512;
constexpr int kMaxThumbPending = 96;
constexpr int kMaxPeakPending = 16;

double thumbKey(double seconds) {
    return std::round(seconds * 10.0) / 10.0;
}
}

CacheWorker::CacheWorker(QObject* parent) : QObject(parent) {}

void CacheWorker::generatePeaks(const QString& filePath, int bucketsPerSecond) {
    const FFmpegAudioPeaks peaks = FFmpegDecoder::audioPeaks(filePath, bucketsPerSecond);
    emit peaksReady(filePath, peaks);
}

void CacheWorker::generateThumb(const QString& filePath, double seconds) {
    QImage img;
    if (!m_decoder.isOpen() || m_decoder.source() != filePath)
        m_decoder.open(filePath);
    if (m_decoder.isOpen())
        img = m_decoder.frameAt(seconds, kThumbMaxWidth);
    emit thumbReady(filePath, seconds, img);
}

MediaCache& MediaCache::instance() {
    static MediaCache cache;
    return cache;
}

MediaCache::MediaCache() {
    qRegisterMetaType<FFmpegAudioPeaks>("FFmpegAudioPeaks");

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

const FFmpegAudioPeaks& MediaCache::peaks(const QString& filePath) const {
    const auto it = m_peaks.constFind(filePath);
    if (it == m_peaks.constEnd()) return m_emptyPeaks;
    touchPeaks(filePath);
    return it.value();
}

QImage MediaCache::thumb(const QString& filePath, double seconds) const {
    const auto key = qMakePair(filePath, thumbKey(seconds));
    const auto it = m_thumbs.constFind(key);
    if (it == m_thumbs.constEnd()) return QImage();
    touchThumb(key);
    return it.value();
}

bool MediaCache::hasPeaks(const QString& filePath) const {
    return m_peaks.contains(filePath);
}

void MediaCache::requestPeaks(const QString& filePath) {
    if (!m_peaksWorker) return;
    if (m_peaksPending.size() >= kMaxPeakPending) return;
    if (m_peaks.contains(filePath) || m_peaksPending.contains(filePath)) return;
    const bool wasBusy = busy();
    m_peaksPending.insert(filePath);
    QMetaObject::invokeMethod(m_peaksWorker, "generatePeaks", Qt::QueuedConnection,
                              Q_ARG(QString, filePath), Q_ARG(int, kPeaksPerSecond));
    if (!wasBusy) emit busyChanged(true);
}

void MediaCache::requestThumb(const QString& filePath, double seconds) {
    if (!m_thumbsWorker) return;
    if (m_thumbsPending.size() >= kMaxThumbPending) return;
    const double k = thumbKey(seconds);
    const auto key = qMakePair(filePath, k);
    if (m_thumbs.contains(key) || m_thumbsPending.contains(key)) return;
    const bool wasBusy = busy();
    m_thumbsPending.insert(key);
    QMetaObject::invokeMethod(m_thumbsWorker, "generateThumb", Qt::QueuedConnection,
                              Q_ARG(QString, filePath), Q_ARG(double, k));
    if (!wasBusy) emit busyChanged(true);
}

void MediaCache::onPeaksReady(const QString& filePath, const FFmpegAudioPeaks& peaks) {
    m_peaks[filePath] = peaks;
    m_peaksPending.remove(filePath);
    m_peaksOrder.removeOne(filePath);
    m_peaksOrder.append(filePath);
    evictPeaks();
    emit waveformReady(filePath);
    if (!busy()) emit busyChanged(false);
}

void MediaCache::onThumbReady(const QString& filePath, double seconds, const QImage& image) {
    const auto key = qMakePair(filePath, thumbKey(seconds));
    m_thumbs[key] = image;
    m_thumbsPending.remove(key);
    m_thumbsOrder.removeOne(key);
    m_thumbsOrder.append(key);
    evictThumbs();
    emit thumbnailReady(filePath, thumbKey(seconds));
    if (!busy()) emit busyChanged(false);
}

bool MediaCache::busy() const {
    return !m_peaksPending.isEmpty() || !m_thumbsPending.isEmpty();
}

void MediaCache::touchPeaks(const QString& filePath) const {
    const int i = m_peaksOrder.indexOf(filePath);
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
        const QString oldest = m_peaksOrder.takeFirst();
        m_peaks.remove(oldest);
    }
}

void MediaCache::evictThumbs() {
    while (m_thumbs.size() > kMaxThumbCache && !m_thumbsOrder.isEmpty()) {
        const auto oldest = m_thumbsOrder.takeFirst();
        m_thumbs.remove(oldest);
    }
}
