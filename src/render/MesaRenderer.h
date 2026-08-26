// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QImage>
#include <QHash>
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

    // Renderiza a composição no instante `time` (segundos da timeline).
    QImage render(const MesaComposition& mesa, const Project& project,
                  double time, double clipPos);

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
        bool valid = false;
    };
    bool prepareLayer(LayerPrep& out, const MesaComposition& mesa,
                      const Project& project, double relTime, const Track& track);

    // Desenha um LayerPrep num painter (aplicando pivot/rotação/âncora/escala
    // e opacidade). Assume o painter já posicionado no sistema de canvas.
    void drawTrackImage(QPainter& acc, const LayerPrep& prep);

    void clearCache();

private:
    // Desenha uma única track (camada) num painter já preparado.
    // Retorna false se nada foi desenhado (sem clip ativo / frame vazio).
    bool drawTrackLayer(QPainter& acc, const Track& track,
                        const MesaComposition& mesa, const Project& project,
                        double relTime);

    QImage decodeFrame(const QString& filePath, double time, int maxW);
    QImage applyCameraTransform(const QImage& canvas, const MesaComposition& mesa,
                                const Project& project, double relTime);

    QHash<QString, FFmpegDecoder*> m_decoders;
};
