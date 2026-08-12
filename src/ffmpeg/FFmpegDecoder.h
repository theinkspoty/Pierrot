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

    // Áudio: PCM contínuo, interleaved S16, 48 kHz estéreo.
    bool hasAudio() const;
    int audioSampleRate() const { return m_audioOutRate; }
    int audioChannels() const { return m_audioOutCh; }
    void seekAudio(double seconds);
    int decodeAudio(void* outBuf, int maxBytes);

private:
    void* m_ctx = nullptr;
    void* m_codec = nullptr;
    int m_stream = -1;
    double m_fps = 0.0;
    QString m_source;
    QMutex m_mutex;

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
    QMutex m_audioMutex;
};
