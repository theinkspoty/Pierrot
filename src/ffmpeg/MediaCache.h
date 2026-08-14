// Pierrot — editor de vídeo estilo Vegas Pro
//
// Copyright (C) 2026 Pierrot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
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

    // Descarta todas as entradas e invalida os pedidos em andamento
    // (chamado ao trocar de projeto para não reter mídia do anterior).
    void clear();
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
    // Chaves cuja decodificação falhou (imagem nula). Não re-pedimos por um
    // tempo: tentar de novo a cada paint apenas enfileira trabalho inútil.
    QSet<QPair<QString, double>> m_thumbsFailed;
    mutable QList<QString> m_peaksOrder;
    mutable QList<QPair<QString, double>> m_thumbsOrder;
    mutable FFmpegAudioPeaks m_emptyPeaks;
    QHash<QString, quint64> m_peaksPending;
    QHash<QPair<QString, double>, quint64> m_thumbsPending;
    quint64 m_epoch = 0; // troca de projeto: descarta resultados enfileirados

    void touchPeaks(const QString& filePath) const;
    void touchThumb(const QPair<QString, double>& key) const;
    void evictPeaks();
    void evictThumbs();
    bool busy() const;
};
