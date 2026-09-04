// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QImage>
#include <QHash>
#include <QList>
#include <QMutex>
#include "models/Project.h"

class QPainter;
class FFmpegDecoder;

// Renderiza uma MesaComposition (composição 2D) em um QImage.
// As tracks do grupo Mesa são composicionadas no canvas, e uma câmera
// define o enquadramento que vai pro output final.
class MesaRenderer {
public:
    MesaRenderer();
    ~MesaRenderer();

    // Renderiza a composição no instante `time` (segundos absolutos da
    // timeline). Layers e câmera usam o mesmo tempo absoluto — os keyframes
    // são gravados pelo MesaWidget na posição global do playhead.
    // As camadas são compostas DIRETO no frame de saída (matriz da câmera +
    // matriz local da layer), sem bitmap intermediário do tamanho da comp:
    // não existe limite de onde uma imagem pode ficar para aparecer no preview.
    QImage render(const MesaComposition& mesa, const Project& project,
                  double time);

    // Prepara a renderização de UMA track (decodifica o frame ativo e retorna
    // a transformada a aplicar). Retorna false se não há frame ativo.
    // O resultado pode ser desenhado com drawTrackImage().
    //
    // Modelo After Effects: a camada vive no próprio espaço local (origem no
    // CANTO SUPERIOR ESQUERDO do frame, tamanho natural). A matriz local→comp é
    //      M = T(posição) · R(rotação) · S(escala) · T(-âncora)
    // onde o "ponto de âncora" é definido em pixels da layer a partir do topo
    // esquerdo. posX/posY = coordenada ABSOLUTA da âncora na composição.
    struct LayerPrep {
        QImage frame;
        double posX = 0, posY = 0; // posição da âncora na composição (px absolutos)
        double rot = 0;            // rotação em graus (sentido horário)
        double sx = 1, sy = 1;     // escala (multiplicador, 1.0 = 100%)
        double ax = 0, ay = 0;     // âncora: OFFSET do centro natural da layer (px)
        double opacity = 1.0;
        int blend = 0;  // QPainter::CompositionMode (0 = SourceOver)
        bool valid = false;
    };
    // Nesta preparação o `relTime` fixa o CONTEÚDO (clip ativo e decode da
    // mídia); `transformTime` (= -1 → igual a relTime) avalia APENAS os
    // keyframes de transform (posição/escala/rotação/opacidade/âncora). É o
    // gancho do motion blur: conteúdo fixo + transform rastejando.
    bool prepareLayer(LayerPrep& out, const MesaComposition& mesa,
                      const Project& project, double relTime, const Track& track,
                      double transformTime = -1.0);

    // Desenha um LayerPrep num painter (aplicando posição/rotação/escala/
    // âncora e opacidade). Assume o painter já posicionado no sistema de
    // coordenadas da composição.
    void drawTrackImage(QPainter& acc, const LayerPrep& prep);

    // Desenha todas as layers diretamente num painter já em canvas-space.
    // Evita criar QImage intermediária — ideal para canvas infinito e para o
    // preview/export sem limite de espaço (a matriz da câmera já está aplicada
    // no painter, as camadas empilham a matriz local por cima).
    // skipTrackId: omitir essa track (para drag em tempo real).
    // motionBlurStack: quando true e a comp tem motion blur, desenha as camadas
    // como n sub-passadas completas integradas (estilo AE). Em render(),
    // desabilitado por passada de câmera para não sobrepor os dois rastros.
    void renderToPainter(QPainter& painter, const MesaComposition& mesa,
                         const Project& project, double relTime,
                         const QString* skipTrackId = nullptr,
                         bool motionBlurStack = true);

    void clearCache();

    // Descarta o cache de quadros compostos (não fecha os decoders). Chamado
    // ao trocar de projeto (PreviewWidget::setProject) — os decoders podem ser
    // reaproveitados, mas o composto do projeto anterior não serve mais.
    void clearCompositeCache();

private:
    int blendModeFor(const QString& blend) const;

    // ── Cache de composição (Fase 1) ─────────────────────────────────────
    // Guarda o quadro COMPOSTO final de render() (empilhamento + câmera +
    // motion blur), não só o decode. Chave = (mesaId, timeBucket, outW, outH,
    // revision): o `revision` do projeto (Project::touch) invalida entradas
    // quando qualquer edição acontece, evitando quadro obsoleto após cortes/
    // efeitos. Busca linear — o cache é pequeno (kCompositeMax).
    struct CompositeKey {
        QString mesaId;
        qint64 bucket = 0;      // qRound64(time * fps): agrupa o mesmo quadro
        int outW = 0, outH = 0;
        quint64 revision = 0;
        bool operator==(const CompositeKey& o) const {
            return mesaId == o.mesaId && bucket == o.bucket
                && outW == o.outW && outH == o.outH && revision == o.revision;
        }
    };
    struct CompositeEntry {
        CompositeKey key;
        QImage img;
    };
    // Cada quadro é armazenado em resolução CHEIA do projeto (1920×1080 ≈ 8 MB,
    // 4K ≈ 33 MB). 8 quadros mantêm o cache enxuto (~64 MB a 1080p, ~260 MB a
    // 4K) e ainda aceleram o scrub de vai-e-vem sobre trechos já vistos.
    static constexpr int kCompositeMax = 8;
    QList<CompositeEntry> m_compositeLru; // frente = mais recente

    QImage compositeFromCache(const CompositeKey& key);
    void compositeToCache(const CompositeKey& key, const QImage& img);

    // Renderiza uma única passada com o enquadramento da câmera no instante
    // `time` (usado tanto pelo caminho limpo quanto pela integração temporal
    // do motion blur da câmera em render()).
    QImage renderSample(const MesaComposition& mesa, const Project& project,
                        double time, const QString* skipTrackId,
                        bool motionBlurStack = true);

    // Empilha as camadas da composição num painter já no canvas-space.
    // `relTime` fixa o CONTEÚDO (clip ativo/decode); `transformTime` avalia os
    // keyframes de transform de cada camada (a base do motion blur por passada).
    void paintStack(QPainter& painter, const MesaComposition& mesa,
                    const Project& project, double relTime,
                    const QString* skipTrackId, double transformTime);

    // Desenha uma única track (camada) num painter já preparado.
    // Retorna false se nada foi desenhado (sem clip ativo / frame vazio).
    bool drawTrackLayer(QPainter& acc, const Track& track,
                        const MesaComposition& mesa, const Project& project,
                        double relTime, double transformTime);

    QImage decodeFrame(const QString& filePath, double time, int maxW);

    QHash<QString, FFmpegDecoder*> m_decoders;

    // Cache de frames decodificados (evita re-decode no mesmo timestamp)
    struct FrameKey { QString path; double time; int maxW; };
    struct FrameCache { FrameKey key; QImage frame; };
    mutable FrameCache m_frameCache;

    // O MESMO MesaRenderer é usado por threads diferentes no PreviewWidget:
    // tryRenderMesa roda no worker de decode e requestLowerLayers na thread da
    // UI — o FFmpegDecoder NÃO é thread-safe (concorrência em avcodec_send_packet
    // derrubava o app com SIGSEGV). O mutex serializa todo acesso aos decoders
    // e ao cache entre essas threads.
    mutable QMutex m_mutex;
};
