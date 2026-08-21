// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QImage>
#include "ffmpeg/FFmpegDecoder.h"

class QThread;

// Lote de instantes de um mesmo arquivo para uma única passada de decodificação.
Q_DECLARE_METATYPE(QList<double>)

class CacheWorker : public QObject {
    Q_OBJECT
public:
    explicit CacheWorker(QObject* parent = nullptr);
public slots:
    void generatePeaks(const QString& filePath, int streamIndex, int bucketsPerSecond);
    void generateThumb(const QString& filePath, double seconds);
    void generateThumbs(const QString& filePath, const QList<double>& seconds);
signals:
    void peaksReady(const QString& filePath, int streamIndex, const FFmpegAudioPeaks& peaks);
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
    // O stream index distingue as faixas de áudio de um arquivo multicanal.
    const FFmpegAudioPeaks& peaks(const QString& filePath, int streamIndex = 0) const;
    QImage thumb(const QString& filePath, double seconds) const;
    bool hasPeaks(const QString& filePath, int streamIndex = 0) const;

    void requestPeaks(const QString& filePath, int streamIndex = 0);
    void requestThumb(const QString& filePath, double seconds);
    // Decodifica vários instantes do mesmo arquivo numa única passada
    // (reaproveita a abertura/seek entre pedidos do mesmo arquivo).
    void requestThumbs(const QString& filePath, const QList<double>& seconds);

    // Durante a reprodução o preview precisa de toda a CPU: os thumbs são
    // conteúdo estático e ficam adiados até a reprodução parar. O TimelineWidget
    // continua pedindo normalmente (sem custo de UI); apenas a decodificação
    // em segundo plano é pausada.
    void setPlaybackActive(bool active);

    // Descarta todas as entradas e invalida os pedidos em andamento
    // (chamado ao trocar de projeto para não reter mídia do anterior).
    void clear();
signals:
    void waveformReady(const QString& filePath, int streamIndex);
    void thumbnailReady(const QString& filePath, double seconds);
    void busyChanged(bool busy);
private slots:
    void onPeaksReady(const QString& filePath, int streamIndex, const FFmpegAudioPeaks& peaks);
    void onThumbReady(const QString& filePath, double seconds, const QImage& image);
private:
    MediaCache();
    QThread* m_peaksThread = nullptr;
    CacheWorker* m_peaksWorker = nullptr;
    QThread* m_thumbsThread = nullptr;
    CacheWorker* m_thumbsWorker = nullptr;
    QHash<QPair<QString, int>, FFmpegAudioPeaks> m_peaks;
    QHash<QPair<QString, double>, QImage> m_thumbs;
    // Chaves cuja decodificação falhou (imagem nula). Não re-pedimos por um
    // tempo: tentar de novo a cada paint apenas enfileira trabalho inútil.
    QSet<QPair<QString, double>> m_thumbsFailed;
    mutable QList<QPair<QString, int>> m_peaksOrder;
    mutable QList<QPair<QString, double>> m_thumbsOrder;
    mutable FFmpegAudioPeaks m_emptyPeaks;
    QHash<QPair<QString, int>, quint64> m_peaksPending;
    QHash<QPair<QString, double>, quint64> m_thumbsPending;
    // Pedidos de thumb adiados durante a reprodução (sem custo na UI).
    QSet<QPair<QString, double>> m_thumbsDeferred;
    // Pedidos de pico adiados durante a reprodução: a varredura completa do
    // áudio disputa disco/CPU com o decode em tempo real e causa stutter no
    // primeiro play de um projeto recém-carregado (picos ainda não em cache).
    QSet<QPair<QString, int>> m_peaksDeferred;
    bool m_playbackActive = false;
    quint64 m_epoch = 0; // troca de projeto: descarta resultados enfileirados

    void touchPeaks(const QPair<QString, int>& key) const;
    void touchThumb(const QPair<QString, double>& key) const;
    void evictPeaks();
    void evictThumbs();
    bool busy() const;
};
