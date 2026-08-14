// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QImage>
#include <QString>
#include <QMutex>
#include <QVector>

struct FFmpegMediaInfo {
    double duration = 0.0;
    int width = 0;
    int height = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    int audioStreams = 0;
};

struct FFmpegAudioPeaks {
    QVector<float> min;
    QVector<float> max;
    int bucketsPerSecond = 0;
};

class FFmpegDecoder {
public:
    FFmpegDecoder();
    ~FFmpegDecoder();

    static FFmpegMediaInfo probe(const QString& filePath);
    static FFmpegAudioPeaks audioPeaks(const QString& filePath, int bucketsPerSecond = 100);

    bool open(const QString& filePath);
    void close();
    bool isOpen() const;
    QString source() const;
    double fps() const;

    QImage frameAt(double seconds, int maxWidth = 0);

    // Libera os buffers de quadros decodificados (DPB do codec e o último
    // quadro em memória) sem fechar o arquivo. Depois de uma decodificação
    // pontual (thumbnail, pan/crop) isso devolve a RAM da resolução cheia;
    // o decoder volta a funcionar normalmente (cada frameAt re-seek/flush).
    void releaseBuffers();

    // Áudio: PCM contínuo, interleaved S16, 48 kHz estéreo.
    bool hasAudio() const;
    int audioSampleRate() const { return m_audioOutRate; }
    int audioChannels() const { return m_audioOutCh; }
    void seekAudio(double seconds);
    int decodeAudio(void* outBuf, int maxBytes);

private:
    void freeAllLocked(); // chama com m_mutex E m_audioMutex segurados

    void* m_ctx = nullptr;
    void* m_codec = nullptr;
    int m_stream = -1;
    double m_fps = 0.0;
    QString m_source;
    mutable QMutex m_mutex;

    // Cache de conversão de cor/escala (reutilizado entre frames).
    void* m_sws = nullptr;
    int m_swsSrcW = 0, m_swsSrcH = 0;
    int m_swsDstW = 0, m_swsDstH = 0;
    int m_swsSrcFmt = -1;

    // Decodificação progressiva (playback não precisa re-seek).
    double m_lastPtsSec = -1.0;
    void* m_lastFrame = nullptr; // AVFrame* (referência do último frame)

    // Áudio: contexto separado, para decodificar em outra thread sem
    // disputar o demuxer de vídeo.
    void* m_aCtx = nullptr;    // AVFormatContext* (áudio)
    void* m_aCodec = nullptr;  // AVCodecContext* (áudio)
    void* m_swr = nullptr;     // SwrContext*
    int m_audioStream = -1;
    int m_audioOutRate = 48000;
    int m_audioOutCh = 2;
    mutable QMutex m_audioMutex;

    // Buffers reutilizáveis para evitar alocação a cada decodificação.
    AVPacket* m_pkt = nullptr;        // frameAt()
    AVFrame* m_frame = nullptr;       // frameAt()
    AVPacket* m_audioPkt = nullptr;   // decodeAudio()
    AVFrame* m_audioFrame = nullptr;  // decodeAudio()
};
