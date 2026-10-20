// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QString>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// L0 — PacketReader: leitura de pacotes À FRENTE com buffer limitado.
//
// Uma thread dedicada lê pacotes do arquivo e os enfileira (teto configurável,
// ex.: 32). O consumidor (o decode de vídeo) retira um pacote por vez sem
// tocar o disco: se o disco engasga, o decode continua até esvaziar o buffer.
// A fila é limitada para que a leitura à frente não estoure a memória em
// arquivos longos (invariante 7, orçamento de memória).
//
// O reader é dono do seu próprio Demuxer/AVFormatContext (contexto separado de
// vídeo, conforme o desenho v0.4): vídeo e áudio deixam de compartilhar uma
// cadeia de mutex cruzada. `seek()` para a produção, descarta o buffer e
// reposiciona o demuxer.
//
// Ordenação/DTS: os pacotes podem chegar com DTS negativo ou fora de ordem
// (arquivos reais com timestamps quebrados). O reader não reordena — preserva a
// ordem do arquivo — mas `nextTimestamp()`/`streamIndex()` expõem o DTS/PTS
// para o consumidor corrigir a matemática de tempo. A ordenação de exibição é
// responsabilidade de quem decodifica (o decode já espera o frame certo).

class PacketReader {
public:
    // bucket: teto de pacotes na fila (mínimo 2). O excedente bloqueia a
    // thread de leitura até o consumidor retirar.
    explicit PacketReader(int bucketSize = 32);
    ~PacketReader();

    PacketReader(const PacketReader&) = delete;
    PacketReader& operator=(const PacketReader&) = delete;

    // Abre o arquivo e sobe a thread de leitura. `streamIndex` restringe os
    // pacotes entregues àquele stream (os demais são descartados). -1 = todos.
    bool open(const QString& filePath, int streamIndex = -1);
    void close();
    bool isOpen() const;

    // Stream/índice efetivamente filtrado (-1 = todos).
    int streamIndex() const { return m_streamIndex; }
    const QString& source() const { return m_source; }
    AVStream* stream() const;              // nullptr se fechado/sem stream
    AVRational timeBase() const;           // {0,1} se indisponível

    // Melhor stream de ÁUDIO do arquivo (-1 se não houver) e duração do
    // container em segundos (-1 se desconhecida). Leem o CONTEXTO PRÓPRIO do
    // reader (mesmo arquivo) — o motor usa para decidir se abre o demuxer
    // dedicado de áudio sem pagar um find_stream_info extra.
    int findBestAudioStream() const;
    double containerDuration() const;

    // Retira o próximo pacote (bloqueante). Devolve >=0 em sucesso (o pacote
    // está preenchido e deve ser liberado com av_packet_unref pelo chamador),
    // AVERROR_EOF no fim, ou outro erro. `cancel` (atômico) aborta a espera.
    int read(AVPacket* out, const std::atomic<bool>* cancel = nullptr);

    // Reposiciona: para a produção, descarta o buffer, faz seek no demuxer e
    // reinicia a leitura. Thread-safe com read().
    //
    // SEM reiniciar a thread (hot path de scrub/corte): o pedido vira um
    // estado pendente que a PRÓPRIA thread produtora aplica (única que pode
    // tocar o contexto) — matar/join/spawn a cada seek custava um join no
    // disco em toda mexida na agulha. Só pós-EOF (produtor encerrado) o
    // seek ainda paga o restart da thread.
    int seek(int streamIndex, int64_t timestamp, int flags);

    // Reconstrói o contexto após um flush do codec (apenas limpa o buffer; o
    // arquivo/período de leitura continua o mesmo).
    void flush();

    // Diagnóstico: quantos pacotes aguardam na fila.
    int buffered() const;

private:
    void produce();

    int m_bucketSize;
    QString m_source;
    int m_streamIndex = -1;

    AVFormatContext* m_fmt = nullptr; // contexto de propriedade do reader
    AVStream* m_stream = nullptr;

    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_cvSpace;  // produtor espera por espaço
    std::condition_variable m_cvData;   // consumidor espera por dado
    std::condition_variable m_cvSeek;   // seek() espera o produtor aplicar
    std::deque<AVPacket*> m_queue;
    bool m_running = false;
    bool m_eof = false;
    int m_readErr = 0;
    // Seek pedido pelo consumidor e ainda não aplicado (sob m_mutex). O
    // produtor drena a fila, chama av_seek_frame e publica m_seekApplied.
    bool m_seekRequested = false;
    int m_seekStream = -1;
    int64_t m_seekTs = 0;
    int m_seekFlags = 0;
    uint64_t m_seekIssued = 0;  // última geração emitida por seek()
    uint64_t m_seekApplied = 0; // geração já aplicada pelo produtor
    int m_lastSeekErr = 0;
    int64_t m_readPos = -1; // descartável
};
