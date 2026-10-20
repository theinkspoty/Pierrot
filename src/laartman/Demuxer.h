// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QString>
#include <QVector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// L0 — Demuxer: abre arquivo, descobre streams, lê pacotes.
// Responsabilidade única: demuxing. Não decodifica, não faz seek de playback,
// não gerencia hardware. Vídeo e áudio compartilham o mesmo AVFormatContext
// (como hoje) mas a lógica de seleção/separação fica centralizada aqui.

struct StreamInfo {
    int index = -1;           // índice absoluto no arquivo
    AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
    AVCodecParameters* params = nullptr;
    AVRational time_base = {1, AV_TIME_BASE};
    AVRational avg_frame_rate = {0, 1};
    AVRational r_frame_rate = {0, 1};
    int64_t duration = AV_NOPTS_VALUE;
    int64_t nb_frames = 0;
};

class Demuxer {
public:
    Demuxer();
    ~Demuxer();

    bool open(const QString& filePath);
    void close();
    bool isOpen() const;

    const QString& source() const { return m_source; }
    AVFormatContext* formatContext() const { return m_fmt; }

    // Encontra índices absolutos de streams por tipo
    int findBestVideoStream() const;
    int findBestAudioStream() const;
    // Resolve k-ésima faixa de áudio (0 = primeira) -> índice absoluto
    int resolveAudioStreamIndex(int audioTrack) const;

    // Info dos streams
    const QVector<StreamInfo>& streams() const { return m_streams; }
    StreamInfo videoStreamInfo() const;
    StreamInfo audioStreamInfo(int absoluteIndex) const;

    // Leitura de pacotes (demuxing puro)
    // Retorna 0 se leu pacote com sucesso, < 0 em erro/fim
    int readPacket(AVPacket* pkt);

    // Seek
    int seek(int streamIndex, int64_t timestamp, int flags);

    double duration() const;
    QString filePath() const { return m_source; }

private:
    void collectStreams();

    AVFormatContext* m_fmt = nullptr;
    QString m_source;
    QVector<StreamInfo> m_streams;
};
