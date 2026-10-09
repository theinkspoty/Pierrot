// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QImage>
#include <QString>

// Ponteiros p/ tipos do FFmpeg (forward declaration, como no FFmpegDecoder.h:
// não vaza os headers C do FFmpeg para quem inclui este arquivo).
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct SwsContext;
struct AVFrame;
struct AVPacket;

// FFmpegEncoder — motor de ENCODE in-process (libavcodec + libavformat).
//
// Simétrico ao FFmpegDecoder: recebe quadros RGBA (QImage) já compostos e
// escreve um arquivo de vídeo com o codec/container escolhidos, sem depender
// do binário `ffmpeg`. Hoje é VÍDEO-APENAS (o áudio da exportação continua
// fora deste motor — misturado no pipeline que chamar).
//
// Uso:
//   FFmpegEncoder enc;
//   if (!enc.open(out, W, H, fps, {.codec = "libx264", .bitrate = 8'000'000})) ...
//   for (aces : quadros) enc.addFrame(ace);
//   enc.finish();   // drena o encoder + escreve o trailer
//
// Não é thread-safe: um único produtor por vez (a thread de exportação).
class FFmpegEncoder {
public:
    struct Options {
        QString codec = QStringLiteral("libx264"); // nome p/ avcodec_find_encoder_by_name
        int bitrate = 8'000'000;
        QString preset;  // ex.: "medium"/"fast"; vazio = padrão do codec
        int gopSize = 0; // 0 = automático (~2s)
        int maxBFrames = 2;

        Options() = default;
    };

    FFmpegEncoder();
    ~FFmpegEncoder();

    FFmpegEncoder(const FFmpegEncoder&) = delete;
    FFmpegEncoder& operator=(const FFmpegEncoder&) = delete;

    // Abre a saída. O container é inferido pela extensão de `path`
    // (avformat_alloc_output_context2). `fps` pode ser fracionário (29.97).
    // Dimensões são arredondadas para par (yuv420p).
    bool open(const QString& path, int width, int height, double fps);
    bool open(const QString& path, int width, int height, double fps,
              const Options& opts);

    // Codifica um quadro RGBA. Se o tamanho não bater com o configurado, é
    // reescalado (FastTransformation). Devolve false em erro (a sessão fica
    // inválida; chame close()).
    bool addFrame(const QImage& rgba);

    // Drena o encoder, escreve o trailer e fecha o arquivo. Idempotente.
    bool finish();

    // Libera tudo (chama finish() se ainda estiver aberto). Seguro repetir.
    void close();

    bool isOpen() const { return m_fmt != nullptr; }
    bool isFinished() const { return m_finished; }
    qint64 framesWritten() const { return m_frameIndex; }

private:
    bool encodeFrame(AVFrame* frame); // nullptr = flush/drain
    bool writePackets();
    void freeAll();

    void* m_fmt = nullptr;     // AVFormatContext*
    void* m_codec = nullptr;   // AVCodecContext*
    void* m_stream = nullptr;  // AVStream*
    void* m_sws = nullptr;     // SwsContext*
    void* m_frame = nullptr;   // AVFrame*
    void* m_pkt = nullptr;     // AVPacket*

    int m_width = 0, m_height = 0;
    int m_fpsNum = 30, m_fpsDen = 1;
    qint64 m_frameIndex = 0;
    bool m_finished = false;
};
