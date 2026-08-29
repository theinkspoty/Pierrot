// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QImage>
#include <QString>
#include <QMutex>
#include <QVector>
#include <QHash>
#include <QList>

// Ponteiros p/ tipos do FFmpeg (definidos em libavcodec/avcodec.h, incluído
// pelos .cpp). Só há membros por ponteiro, então forward declaration basta e
// evita vazar os headers C do FFmpeg para quem inclui este arquivo.
struct AVPacket;
struct AVFrame;

struct FFmpegMediaInfo {
    double duration = 0.0;
    int width = 0;
    int height = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    int audioStreams = 0;
    // Canais de cada stream de áudio, na ordem dos streams (índice do array =
    // índice do stream). Vazio se não houver áudio.
    QVector<int> audioChannels;
    double fps = 0.0; // fps do stream de vídeo (0 se não houver)
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
    // Picos de áudio de um stream específico (índice do stream) ou do melhor
    // stream (streamIndex < 0).
    static FFmpegAudioPeaks audioPeaks(const QString& filePath,
                                       int bucketsPerSecond = 100,
                                       int streamIndex = -1);

    // audioStream: índice do stream de áudio a abrir (-1 = melhor stream).
    bool open(const QString& filePath, int audioStream = -1);
    void close();
    bool isOpen() const;
    QString source() const;
    double fps() const;
    // true se o vídeo está sendo decodificado por hardware (VAAPI ativo).
    bool usesHardware() const;

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

    // Cache de quadros da convenção de exibição/hold do frameAt():
    //  - m_lastFrame: último quadro cujo início NÃO passou do alvo (a exibir);
    //  - m_nextFrame: primeiro quadro que PASSOU do alvo (decodificado na
    //    folga, é o candidato do pedido seguinte).
    // m_lastFrameSec / m_nextFrameSec guardam o fsec de cada um (-1 = vazio).
    void* m_lastFrame = nullptr;  // AVFrame*
    void* m_nextFrame = nullptr;  // AVFrame*
    double m_lastFrameSec = -1.0;
    double m_nextFrameSec = -1.0;

    // Áudio: contexto separado, para decodificar em outra thread sem
    // disputar o demuxer de vídeo.
    void* m_aCtx = nullptr;    // AVFormatContext* (áudio)
    void* m_aCodec = nullptr;  // AVCodecContext* (áudio)
    void* m_swr = nullptr;     // SwrContext*
    int m_audioStream = -1;
    int m_audioOutRate = 48000;
    int m_audioOutCh = 2;
    // Após seekAudio(), descarta os primeiros N frames decodificados
    // (independentemente do PTS) para eliminar a "rebarba" duplicada que
    // o AVSEEK_FLAG_BACKWARD traz do pacote anterior ao ponto de corte.
    int m_audioSkipFrames = 0;
    // Seek de áudio preciso: após av_seek_frame (que pode pousar MUITO antes
    // do alvo em arquivos longos, índice esparso/MKV sem cue), os frames são
    // descartados até atingirem este tempo (PTS), em vez da contagem cega de
    // N frames — que não cobria a distância e fazia o áudio retomar adiantado/
    // atrasado, teleportando o playhead do preview para trás.
    double m_audioSeekTargetSec = -1.0;
    mutable QMutex m_audioMutex;

    // Imagem estática (JPEG, PNG, BMP, etc.): frame único, sem seek.
    bool m_isImage = false;

    // Aceleração de hardware (VAAPI/Linux; desligável com PIERROT_GPU=0).
    // m_hwPixFmt guarda o formato do quadro decodificado (ex.: AV_PIX_FMT_VAAPI);
    // m_swFrame é um AVFrame* auxiliar onde o quadro é transferido para NV12
    // antes do sws (o hw frame mora na GPU e não pode ser lido cru).
    bool m_hw = false;
    int m_hwPixFmt = -1;
    void* m_swFrame = nullptr; // AVFrame*

    // Buffers reutilizáveis para evitar alocação a cada decodificação.
    AVPacket* m_pkt = nullptr;        // frameAt()
    AVFrame* m_frame = nullptr;       // frameAt()
    AVPacket* m_audioPkt = nullptr;   // decodeAudio()
    AVFrame* m_audioFrame = nullptr;  // decodeAudio()

    // ── Frame cache LRU (evita re-decodificação no scrub) ─────────────
    // Chave: (time_bucket, maxWidth). time_bucket = floor(seconds * fps)
    // para agrupar frames do mesmo quadro.
    struct FrameCacheKey {
        int64_t bucket = 0;
        int maxW = 0;
        bool operator==(const FrameCacheKey& o) const {
            return bucket == o.bucket && maxW == o.maxW;
        }
        friend inline uint qHash(const FrameCacheKey& k, uint seed = 0) {
            return qHash(k.bucket, seed) ^ qHash(k.maxW, seed ^ 0x9747b28c);
        }
    };
    struct FrameCacheEntry {
        FrameCacheKey key;
        QImage img;
    };
    static constexpr int kFrameCacheMax = 120; // ~2min a 30fps, ~240MB
    QList<FrameCacheEntry> m_frameCacheLru;     // frente = mais recente
    QHash<FrameCacheKey, int> m_frameCacheIdx;  // key → índice no LRU

    QImage frameFromCache(const FrameCacheKey& key);
    void   frameToCache(const FrameCacheKey& key, const QImage& img);
    void   frameCacheClear();
};
