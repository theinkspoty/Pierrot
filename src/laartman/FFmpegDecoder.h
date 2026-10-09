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
#include <atomic>

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

    // Permite/desabilita a decodificação por hardware NESTA instância.
    // Usado por quem decodifica um único quadro sob demanda no thread da UI:
    // o ganho do VAAPI é irrelevante para um frame, mas o device VAAPI é
    // compartilhado por processo com os workers de preview, e o conflito de
    // agendamento derrubava o app. Precisa ser chamado antes de open().
    void setHardwareDecodeAllowed(bool allowed);

    // `cancel` (Fase 3): token cooperativo opcional. Quando a UI marca o
    // token enquanto este frameAt roda, ele aborta no próximo ponto seguro e
    // devolve QImage nula — o progresso já decodificado é preservado, então o
    // próximo frameAt continua de onde paramos (ou faz seek, se o alvo exigir).
    // Lido de outra thread: só std::atomic<bool>, nunca tocado sob m_mutex.
    QImage frameAt(double seconds, int maxWidth = 0,
                   const std::atomic<bool>* cancel = nullptr);

    // Libera os buffers de quadros decodificados (DPB do codec e o último
    // quadro em memória) sem fechar o arquivo. Depois de uma decodificação
    // pontual (thumbnail, pan/crop) isso devolve a RAM da resolução cheia;
    // o decoder volta a funcionar normalmente (cada frameAt re-seek/flush).
    void releaseBuffers();

    // Áudio: PCM contínuo, interleaved S16, 48 kHz estéreo.
    bool hasAudio() const;
    int audioChannels() const;
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
    bool m_hwAllowed = true; // por instância; ver setHardwareDecodeAllowed()
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

    // O cache é válido SOMENTE sob m_mutex (por isso o sufixo Locked, mesmo
    // idioma de freeAllLocked). O índice é reconstruído a cada inserção e
    // frameAt() roda em thread própria: tocar a lista fora do lock já
    // racingaria com o decode e corromperia o índice.
    QImage frameFromCacheLocked(const FrameCacheKey& key);

public:
    // Diagnóstico: quantas vezes o LRU realmente evitou um decode. Sem isso
    // não há como saber se aumentar kFrameCacheMax ajuda ou só gasta memória.
    static quint64 s_cacheHits, s_cacheMisses;
    static void resetCacheStats() { s_cacheHits = s_cacheMisses = 0; }
    static quint64 cacheHits()   { return s_cacheHits; }
    static quint64 cacheMisses() { return s_cacheMisses; }
    // Tempo gasto em seek+flush dentro de frameAt. Até agora isso era
    // invisível: o `seek` medido no engine só enxerga o tick, não o seek que
    // acontece na thread do decoder — por isso um seek de 300ms aparecia como
    // "seek=0ms" e o custo sobrava sem dono.
    static quint64 s_seekCount, s_seekTotalNs, s_lastSeekNs;
    static void resetSeekStats() { s_seekCount = s_seekTotalNs = s_lastSeekNs = 0; }
    static quint64 seekCount()    { return s_seekCount; }
    static quint64 seekTotalNs()  { return s_seekTotalNs; }
    static quint64 lastSeekNs()   { return s_lastSeekNs; }
    // Custo de ABRIR o arquivo (avformat_open_input + find_stream_info). O que
    // dói num corte não é decodificar (p99 < 1ms) e sim reabrir/parsear o
    // container (~500-800ms). Aberturas atômicas porque open() roda tanto na
    // thread do FrameWorker quanto na do prefetch em background.
    static std::atomic<quint64> s_openCount, s_openTotalNs, s_lastOpenNs;
    static void resetOpenStats() { s_openCount = 0; s_openTotalNs = 0; s_lastOpenNs = 0; }
    static quint64 openCount()    { return s_openCount.load(); }
    static quint64 openTotalNs()  { return s_openTotalNs.load(); }
    static quint64 lastOpenNs()   { return s_lastOpenNs.load(); }
    // Quantos quadros o laço de frameAt decodificou e_DESCARTOU antes de achar
    // o alvo. É o custo escondido de um seek: com GOP aberto, decodificar do
    // keyframe até o alvo paga dezenas de quadros que ninguém vê.
    static quint64 s_discardCount, s_lastDiscard;
    static quint64 lastDiscard()  { return s_lastDiscard; }
    // Tempo total dentro de decodeOne. Comparado com a latência dispatch->pronto
    // (wrk), separa "o worker demorou" de "o pedido ficou na fila esperando".
    static quint64 s_lastWorkNs, s_workTotalNs;
    static quint64 lastWorkNs()   { return s_lastWorkNs; }
    static quint64 discardTotal() { return s_discardCount; }
    void   frameToCacheLocked(const FrameCacheKey& key, const QImage& img);
    void   frameCacheClearLocked();
};
