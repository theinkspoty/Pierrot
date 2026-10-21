// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// Laartman · L3 — a API pública congelada (fachada fina). Ver README.md.
// Consumidores de fora incluem SÓ este header; as camadas congeladas moram
// em core/ (motor) e io/ (demux) e não são para tocar.

#pragma once

#include <QImage>
#include <QString>

#include <atomic>

// ── Laartman · L3 — a API pública congelada (fachada fina) ────────────────
//
// Este é o ÚNICO header que a camada de política (colombina/ffmpeg, ui/) deve
// incluir para decodificar. Ele descreve o núcleo imutável do plano v0.4: um
// motor que decodifica um frame num tempo absoluto e não coordena NADA além
// disso. As camadas congeladas moram em core/ (motor) e io/ (demux) — ver
// README.md deste diretório.
//
// O que NÃO existe aqui (e o CI deve garantir que não volte):
//   • posse/"lease": acquire, release, busy, pool;
//   • decisão de política: setHardwareDecodeAllowed, escolha de proxy;
//   • gestão de memória do chamador: releaseBuffers;
//   • estado de pipeline: lookahead, prefetch, cache.
//
// Quem decide "quantos abertos, qual fonte, qual qualidade, quando prefetch" é
// o Pierrot (L4–L6). O núcleo recebe a decisão via DecodeOptions e obedece.
//
// Hoje `Decoder` é uma fachada FINA sobre o motor atual (FFmpegDecoder): não
// muda comportamento, só estabelece a superfície pública que as fases 2–6 vão
// consumir. A migração dos call sites é incremental e reversível.

// L3 — o resultado de uma sondagem: metadados suficientes para a política
// decidir (proxy, qualidade, layout) sem abrir o arquivo de decodificação.
struct MediaInfo {
    double duration = 0.0;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    bool hasVideo = false;
    bool hasAudio = false;
    int audioStreams = 0;
};

// L5 → L3 — as decisões de política que o núcleo aceita ao abrir. O núcleo não
// as inventa: ele recebe. `audioStream` é a k-ésima faixa (-1 = melhor stream);
// `hardwareAllowed=false` força software para esta instância.
struct DecodeOptions {
    int audioStream = -1;
    bool hardwareAllowed = true;
};

// L3 — desfecho de um pedido de quadro. "Falha é tipo, não silêncio"
// (invariante 3): a imagem nula sozinha confundia cancelamento, fim de arquivo
// e erro. Este enum dá nome ao que aconteceu, sem vazar o FFmpeg.
enum class DecodeStatus {
    Ok,        // imagem decodificada neste pedido
    Eof,       // fim da mídia, sem novo quadro
    Cancelled, // pedido abortado pelo token de cancelamento
    Error      // falha de decode/leitura, sem imagem a entregar
};

// L3 — resultado de um pedido tipado: a imagem (nula quando não há o que
// exibir) e o porquê. `frame()` (compat) devolve só a imagem; `frameDecoded()`
// devolve este par para quem precisa distinguir os desfechos.
struct DecodedFrame {
    QImage image;
    DecodeStatus status = DecodeStatus::Error;
};

// ── L3 — o CONTRATO de leitura (§8.8) ─────────────────────────────────────
//
// A superfície substituível do núcleo: tudo o que um leitor de mídia precisa
// oferecer para o Pierrot exibir e exportar — abrir/fechar, identidade, cadência
// e os pedidos de quadro/áudio. `Decoder` é a implementação concreta atual; um
// dia pode ser um leitor sobre outra pilha, sem que nenhum call site mude.
//
// O contrato é deliberadamente MÍNIMO: nada de posse, pool, qualidade, prefetch
// ou cache — essas decisões são do Pierrot (L4–L6), não do leitor. Sondagem
// (probe/isAnimatedImage) fica como estática em `Decoder`: não precisa de
// instância nem de substituição.
//
// Uso com substituição: guarde `MediaReader*`/`std::unique_ptr<MediaReader>` e
// chame pelo contrato. Uso direto (hoje): instancie `Decoder` como sempre.
class MediaReader {
public:
    virtual ~MediaReader() = default;

    // Abre `path` segundo `opt`. Idempotente (mesmo arquivo+opções = atalho).
    virtual bool open(const QString& path, const DecodeOptions& opt) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual QString source() const = 0;
    virtual double fps() const = 0;

    // Frame em `tAbsoluteSec`; `maxWidth=0` = largura nativa.
    virtual QImage frame(double tAbsoluteSec, int maxWidth,
                         const std::atomic<bool>* cancel) = 0;
    virtual DecodedFrame frameDecoded(double tAbsoluteSec, int maxWidth,
                                      const std::atomic<bool>* cancel) = 0;

    // Áudio: PCM interleaved S16, 48 kHz estéreo.
    virtual bool seekAudio(double tAbsoluteSec) = 0;
    virtual int  readPcm(void* out, int maxBytes) = 0;
    virtual bool audioFinished() const = 0;
    virtual bool hasAudio() const = 0;
};

class Decoder : public MediaReader {
public:
    Decoder();
    ~Decoder() override;

    // Não copiável (segura um contexto de decoder mutável); movível.
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Sondagem estática: não abre um decoder, só lê metadados.
    static MediaInfo probe(const QString& path);
    // true se o arquivo é GIF/WebP ANIMADO (múltiplos quadros + duração real).
    static bool isAnimatedImage(const QString& path);

    // Abre `path` segundo `opt`. Idempotente: abrir o mesmo arquivo já aberto
    // com as mesmas opções é atalho que devolve true sem reabrir.
    bool open(const QString& path, const DecodeOptions& opt = DecodeOptions()) override;
    void close() override;
    bool isOpen() const override;
    QString source() const override;
    double fps() const override;

    // Decodifica o frame em `tAbsoluteSec` (absoluto no arquivo). `maxWidth=0`
    // devolve a largura nativa. `cancel`, se dado, aborta cooperativamente e
    // devolve imagem nula; o progresso é preservado para o próximo pedido.
    QImage frame(double tAbsoluteSec, int maxWidth = 0,
                 const std::atomic<bool>* cancel = nullptr) override;

    // Variante tipada (§8.3): mesmo decode, mas devolve também o DESFECHO
    // (Ok/Eof/Cancelled/Error) — quem precisa reagir a "acabou" ou "cancelado"
    // não fica adivinhando a partir da imagem nula. `frame()` é o atalho
    // quando só a imagem importa.
    DecodedFrame frameDecoded(double tAbsoluteSec, int maxWidth = 0,
                              const std::atomic<bool>* cancel = nullptr) override;

    // Áudio: PCM interleaved S16, 48 kHz estéreo.
    bool   seekAudio(double tAbsoluteSec) override;
    int    readPcm(void* out, int maxBytes) override;
    bool   audioFinished() const override;
    bool   hasAudio() const override;

private:
    // O motor atual. Na v0.4 o conteúdo é fatiado em L0–L2; a superfície
    // pública acima não muda. Tipo escondido (pimpl) para não vazar o header
    // do FFmpegDecoder nem acoplar a política ao motor concreto.
    struct Impl;
    Impl* d = nullptr;
};
