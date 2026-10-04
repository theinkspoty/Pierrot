// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Loader de OBJ (Wavefront) com textura — malhas para a Mesa 3D / Pivot.
// Suporta: v, vt, f (v / v/vt / v/vt/vn), mtllib + map_Kd.
// Resolve textura: caminho do MTL → mesmo diretório do OBJ → *.png vizinho.

#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QVector>

namespace mesh {

struct ObjMesh {
    QVector<QPointF> vertsXY;   // posição 2D (após normalize) — fallback
    QVector<double> vertsZ;     // profundidade por vértice
    // Coordenadas 3D completas (após normalize, centradas na origem).
    QVector<double> vertsX;
    QVector<double> vertsY;
    QVector<QPointF> uvs;       // UV por vértice (0,0 se ausente)
    QVector<QVector<int>> faces; // índices de VÉRTICE por face
    QVector<QVector<int>> faceUVs; // índices de UV por face (-1 = sem)
    double minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
    QString mtlPath;
    QString texturePath; // caminho resolvido da textura (pode ser vazio)
    QImage texture;      // carregada sob demanda

    bool isEmpty() const { return faces.isEmpty() || vertsXY.isEmpty(); }
    bool hasTexture() const { return !texturePath.isEmpty(); }
    bool has3D() const { return !vertsX.isEmpty() && vertsX.size() == vertsY.size()
                              && vertsX.size() == vertsZ.size(); }
    void normalize(double target = 200.0);
    bool loadTexture();
    QColor averageColor() const;
};

bool loadObjFile(const QString& path, ObjMesh& out, QString* error = nullptr);

// Desenha uma malha texturada (ou sólida) num painter já posicionado
// no centro da malha. `scale` multiplica o tamanho normalizado.
void drawMeshTextured(QPainter& painter, const ObjMesh& mesh, double scale,
                      const QColor& fallback, double opacity = 1.0);

// Preenche `poly` (face já projetada na tela, mesmos índices de `faces`)
// com a textura mapeada pelas UVs da face, por AFFINE.
//
// Antes os renderers faziam `drawImage(bbox, mesh.texture)` — jogava a
// imagem inteira esticada dentro da caixa da face, o que só fica "certo"
// numa frente ortográfica perfectly frontal. Qualquer rotação/perspectiva
// arrastava a textura para fora da malha.
//
// Polígonos com mais de 3 vértices viram leque (0,1,2),(0,2,3)... usando as
// UVs correspondentes. Devolve false sem textura, sem UVs ou com triângulo
// degenerado — nesse caso o CHAMADOR preenche com cor chapada, porque uma
// textura errada é pior que nenhuma.
bool fillFaceTextured(QPainter& painter, const ObjMesh& mesh, int faceIndex,
                      const QPolygonF& poly, double opacity = 1.0);

} // namespace mesh
