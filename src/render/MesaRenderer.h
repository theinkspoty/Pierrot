// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QImage>
#include <QHash>
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
    QImage render(const MesaComposition& mesa, const Project& project,
                  double time);

    // Renderiza apenas o canvas (layers composadas) sem a transform de câmera.
    // Útil para o editor visual (MesaWidget).
    // skipTrackId != nulo: omitir essa track (usado para re-render só a camada
    // arrastada durante um transform, evitando recompor a composição toda).
    QImage renderCanvas(const MesaComposition& mesa, const Project& project,
                        double relTime, const QString* skipTrackId = nullptr);

    // Prepara a renderização de UMA track (decodifica o frame ativo e retorna
    // a transformada a aplicar). Retorna false se não há frame ativo.
    // O resultado pode ser desenhado com drawTrackImage().
    struct LayerPrep {
        QImage frame;
        double pivotX = 0, pivotY = 0; // centro do pivot no canvas
        double rot = 0;
        double anchorX = 0, anchorY = 0;
        double drawW = 0, drawH = 0;
        double opacity = 1.0;
        int blend = 0;  // QPainter::CompositionMode (0 = SourceOver)
        bool valid = false;
    };
    bool prepareLayer(LayerPrep& out, const MesaComposition& mesa,
                      const Project& project, double relTime, const Track& track);

    // Desenha um LayerPrep num painter (aplicando pivot/rotação/âncora/escala
    // e opacidade). Assume o painter já posicionado no sistema de canvas.
    void drawTrackImage(QPainter& acc, const LayerPrep& prep);

    // Desenha todas as layers diretamente num painter já em canvas-space.
    // Evita criar QImage intermediária — ideal para canvas infinito.
    // skipTrackId: omitir essa track (para drag em tempo real).
    void renderToPainter(QPainter& painter, const MesaComposition& mesa,
                         const Project& project, double relTime,
                         const QString* skipTrackId = nullptr);

    void clearCache();

private:
    int blendModeFor(const QString& blend) const;

    // Desenha uma única track (camada) num painter já preparado.
    // Retorna false se nada foi desenhado (sem clip ativo / frame vazio).
    bool drawTrackLayer(QPainter& acc, const Track& track,
                        const MesaComposition& mesa, const Project& project,
                        double relTime);

    QImage decodeFrame(const QString& filePath, double time, int maxW);
    QImage applyCameraTransform(const QImage& canvas, const MesaComposition& mesa,
                                const Project& project, double relTime);

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
