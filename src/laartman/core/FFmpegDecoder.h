// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Laartman · core/ (L1+L2) — MOTOR DE DECODE CONGELADO. Não incluir de fora
// de src/laartman/: quem está acima consome laartman/Decoder.h (a fachada).

#pragma once

#include <QImage>
#include <QString>
#include <QMutex>
#include <QVector>
#include <QHash>
#include <QList>
#include <atomic>
#include <list>
#include <memory>

#include <mutex>

// Ponteiros p/ tipos do FFmpeg (definidos em libavcodec/avcodec.h, incluído
// pelos .cpp). Só há membros por ponteiro, então forward declaration basta e
// evita vazar os headers C do FFmpeg para quem inclui este arquivo.
struct AVPacket;
struct AVFrame;
struct AVStream;
class Demuxer;    // L0 — demuxing (definido em Demuxer.h, incluído só nos .cpp)
class PacketReader; // L0 — leitura de pacotes à frente (PacketReader.h)

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
    // true se o arquivo é um GIF/WebP ANIMADO (múltiplos quadros + duração
    // real). Usado pelo exportador para decidir entre o caminho de imagem
    // estática (-loop 1 -framerate) e o de vídeo em loop (-ignore_loop 0).
    // Falso para PNG/JPEG/BMP/SVG/estáticos (inclusive GIF de um quadro).
    static bool isAnimatedImage(const QString& filePath);
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

    // Índice de tempo para a chave do cache de quadros (política de L4). O
    // núcleo NÃO guarda cache (Fase 6), mas é quem conhece a cadência: em CFR
    // é o número do quadro (t*fps); em VFR (espaçamento irregular) usa uma
    // grade fina de milissegundos, senão o bucket por fps médio colidiria.
    // O chamador de L4 usa isto como chave do FrameCache (colombina/ffmpeg).
    int64_t frameCacheIndex(double t) const;

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
    //
    // `status`, se dado, recebe o DESFECHO tipado (§8.3, invariante 3): distingue
    // "decodificou" de "acabou" (Eof), "cancelado" (Cancelled) e "erro sem
    // recuperação" (Error). Sem isto o chamador só via QImage nula e não sabia
    // qual dos três aconteceu. Opcional para não mexer nos call sites existentes.
    enum class FrameResult {
        Ok,        // quadro decodificado neste pedido
        Eof,       // fim do arquivo, sem novo quadro
        Cancelled, // pedido abortado pelo token de cancelamento
        Error      // falha de decode/leitura sem quadro para entregar
    };
    QImage frameAt(double seconds, int maxWidth = 0,
                   const std::atomic<bool>* cancel = nullptr,
                   FrameResult* status = nullptr);

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
    // true quando o decodeAudio alcançou o FIM do arquivo (codec + resampler
    // drenados). Distingue de "por enquanto não há bytes" (retorno 0 temporário
    // ou erro) — permite ao consumidor (AudioConformCache) parar em vez de
    // re-pedir cegamente. Resetado por seekAudio()/open()/close().
    bool audioFinished() const;

private:
    void freeAllLocked(); // chama com m_mutex E m_audioMutex segurados

    // Adquire os locks do caminho de áudio na ordem fixa (m_mutex →
    // m_audioMutex, a mesma de open()/close(): sem AB-BA). Caso comum — vídeo
    // com contexto PRÓPRIO via PacketReader — só pega m_audioMutex: vídeo e
    // áudio decodificam em paralelo. Compartilhado (imagem / reader falhou)
    // pega os dois.
    void lockAudio(std::unique_lock<QMutex>& video,
                   std::unique_lock<QMutex>& audio) const;

    // Monta o codec de vídeo (caminho VAAPI + fallback software) a partir do
    // stream. `st` vem do PacketReader (vídeo com contexto próprio), do
    // Demuxer (imagens/GIFs/fallback) e `containerDurationSec` é a duração do
    // container em segundos (-1 = desconhecida).
    void setupVideoCodec(AVStream* st, double containerDurationSec,
                         const QString& filePath);
    // Monta o codec de áudio + swr a partir do demuxer (contexto dedicado).
    void setupAudioFromDemuxer(int audioStream);

    std::unique_ptr<Demuxer> m_demuxer; // L0 — demuxing (áudio/imagens/fallback)
    // L0 — leitura de pacotes À FRENTE do vídeo (Fases 3+4 do plano v0.4):
    // thread dedicada com buffer limitado, contexto de demux PRÓPRIO — o vídeo
    // deixa de compartilhar o AVFormatContext (e a cadeia de mutex) com o
    // áudio. Nulo para imagens (single-frame) e em fallback se a abertura
    // falhar (aí o vídeo lê o demuxer compartilhado, como antes).
    std::unique_ptr<PacketReader> m_reader;
    // true quando o caminho de vídeo lê o MESMO demuxer do áudio (m_reader
    // nulo com stream de vídeo presente — imagens ou fallback). Só esse caso
    // exige m_mutex no decodeAudio/seekAudio. Escrito só em open()/freeAllLocked
    // (sob os dois locks); lido via atomic no lockAudio().
    std::atomic<bool> m_videoSharesDemuxer{false};
    // Espelho do contexto do DEMUXER (imagens/GIFs, áudio, fallback). Nulo
    // quando o vídeo vive só no PacketReader (contexto próprio).
    void* m_ctx = nullptr;
    void* m_codec = nullptr;
    int m_stream = -1;
    double m_fps = 0.0;
    // time_base do stream de vídeo, cacheado no open(). O caminho de vídeo NÃO
    // pode mais ler fmt->streams[] do demuxer compartilhado: com o áudio
    // decodificando sem m_mutex (Fase 3), av_read_frame roda concorrente — e em
    // MPEG-TS ele pode realocar fmt->streams ao descobrir streams novos.
    int m_vtbNum = 1;
    int m_vtbDen = 1000000;
    // Taxa de quadros VARIÁVEL (VFR): avg_frame_rate é só uma média, então a
    // matemática de tempo (chave de cache, tolerância) usa o timestamp real de
    // cada quadro em vez de assumir espaçamento constante (1/fps).
    bool m_vfr = false;
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

    // Áudio: lê pacotes do MESMO demuxer que serve de probe/contexto
    // (m_aCtx == m_ctx). Com o vídeo no PacketReader (contexto próprio), o
    // caso comum decodifica áudio e vídeo em paralelo — só m_audioMutex os
    // separa. Quando o vídeo COMPARTILHA o demuxer (imagens, fallback do
    // reader), lockAudio() também pega m_mutex (ordem fixa m_mutex →
    // m_audioMutex). Para áudio e vídeo em threads concorrentes do MESMO
    // arquivo, use instâncias separadas (é o que o AudioConformCache faz).
    void* m_aCtx = nullptr;    // AVFormatContext* (== m_ctx, o do demuxer)
    void* m_aCodec = nullptr;  // AVCodecContext* (áudio)
    void* m_swr = nullptr;     // SwrContext*
    int m_audioStream = -1;
    // Faixa de áudio PEDIDA no open() (índice k-ésimo, -1 = melhor stream).
    // Guardada porque o atalho de reabertura precisa compará-la: sem isto,
    // abrir o mesmo arquivo pedindo outra faixa devolvia true sem reabrir.
    int m_requestedAudioStream = -1;
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
    // true quando decodeAudio() drenou o codec e o resampler (fim do arquivo).
    // A versão anterior retornava 0 tanto para "acabou" quanto para "não há
    // bytes ainda", e o consumidor re-pedida sem fim. Resetado em seekAudio().
    bool m_audioFinished = false;
    mutable QMutex m_audioMutex;

    // Imagem estática (JPEG, PNG, BMP, etc.): frame único, sem seek.
    bool m_isImage = false;
    // Cache do still decodificado via QImage (NÃO via libavcodec). O caminho
    // FFmpeg de imagem estourava SIGSEGV em imagens grandes (issues #3/#4/#5);
    // stills não precisam de avcodec — o Qt carrega via plugins de imagem.
    QImage m_imageCache;
    // GIF animado (múltiplos quadros + NETSCAPE com loop). frameAt() embrulha
    // o tempo pedido com fmod sobre a duração real do arquivo, repetindo a
    // animação para preencher o clipe (paridade com o export, que passa
    // -ignore_loop 0 para o demuxer loopar os pacotes).
    bool m_isLoopGif = false;
    // Duração REAL do arquivo em segundos (fmt->duration / AV_TIME_BASE). Usada
    // só pelo embrulho do loop acima; -1 quando desconhecida/não-loop.
    double m_gifDurationSec = -1.0;

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

    // ── Frame cache: NÃO vive mais aqui (Fase 6) ──────────────────────
    // O LRU de quadros saiu do núcleo para L4 (colombina/ffmpeg/FrameCache).
    // frameAt() sempre decodifica; o cache é política do consumidor.

    // ── Instrumentação: NÃO vive mais aqui (§8.5) ─────────────────────
    // Os contadores (open/seek/discard/work) eram estáticos públicos deste
    // header — estado global mutável dentro do núcleo. Saíram para um header
    // INTERNO (core/DecoderMetrics.h): o núcleo só chama record*() nos pontos
    // quentes; quem lê (overlay/profiler) lê de DecoderMetrics.
};
