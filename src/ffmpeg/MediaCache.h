#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QList>
#include <QImage>
#include "ffmpeg/FFmpegDecoder.h"

class QThread;

class CacheWorker : public QObject {
    Q_OBJECT
public:
    explicit CacheWorker(QObject* parent = nullptr);
public slots:
    void generatePeaks(const QString& filePath, int bucketsPerSecond);
    void generateThumb(const QString& filePath, double seconds);
signals:
    void peaksReady(const QString& filePath, const FFmpegAudioPeaks& peaks);
    void thumbReady(const QString& filePath, double seconds, const QImage& image);
private:
    FFmpegDecoder m_decoder;
};

class MediaCache : public QObject {
    Q_OBJECT
public:
    static MediaCache& instance();
    ~MediaCache();

    // Retorna referência para evitar cópia a cada paint da timeline.
    const FFmpegAudioPeaks& peaks(const QString& filePath) const;
    QImage thumb(const QString& filePath, double seconds) const;
    bool hasPeaks(const QString& filePath) const;

    void requestPeaks(const QString& filePath);
    void requestThumb(const QString& filePath, double seconds);
signals:
    void waveformReady(const QString& filePath);
    void thumbnailReady(const QString& filePath, double seconds);
    void busyChanged(bool busy);
private slots:
    void onPeaksReady(const QString& filePath, const FFmpegAudioPeaks& peaks);
    void onThumbReady(const QString& filePath, double seconds, const QImage& image);
private:
    MediaCache();
    QThread* m_peaksThread = nullptr;
    CacheWorker* m_peaksWorker = nullptr;
    QThread* m_thumbsThread = nullptr;
    CacheWorker* m_thumbsWorker = nullptr;
    QHash<QString, FFmpegAudioPeaks> m_peaks;
    QHash<QPair<QString, double>, QImage> m_thumbs;
    mutable QList<QString> m_peaksOrder;
    mutable QList<QPair<QString, double>> m_thumbsOrder;
    mutable FFmpegAudioPeaks m_emptyPeaks;
    QSet<QString> m_peaksPending;
    QSet<QPair<QString, double>> m_thumbsPending;

    void touchPeaks(const QString& filePath) const;
    void touchThumb(const QPair<QString, double>& key) const;
    void evictPeaks();
    void evictThumbs();
    bool busy() const;
};
