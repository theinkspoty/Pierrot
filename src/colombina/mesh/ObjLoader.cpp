// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ObjLoader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextStream>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace mesh {

void ObjMesh::normalize(double target) {
    if (vertsXY.isEmpty()) return;
    const double w = std::max(1e-6, maxX - minX);
    const double h = std::max(1e-6, maxY - minY);
    const double zr = std::max(1e-6, maxZ - minZ);
    const double scale = target / std::max({w, h, zr});
    const double cx = (minX + maxX) * 0.5;
    const double cy = (minY + maxY) * 0.5;
    const double cz = (minZ + maxZ) * 0.5;
    // Preenche vertsX/Y/Z 3D se ainda não existirem (a partir de XY+Z).
    if (vertsX.size() != vertsXY.size()) {
        vertsX.resize(vertsXY.size());
        vertsY.resize(vertsXY.size());
    }
    for (int i = 0; i < vertsXY.size(); ++i) {
        const double ox = vertsXY[i].x();
        const double oy = vertsXY[i].y();
        const double oz = vertsZ[i];
        vertsX[i] = (ox - cx) * scale;
        vertsY[i] = (oy - cy) * scale;
        vertsZ[i] = (oz - cz) * scale;
        vertsXY[i] = QPointF(vertsX[i], vertsY[i]);
    }
    minX = -target * 0.5; maxX = target * 0.5;
    minY = -target * 0.5; maxY = target * 0.5;
    minZ = -target * 0.5; maxZ = target * 0.5;
}

bool ObjMesh::loadTexture() {
    if (texturePath.isEmpty()) return false;
    if (!texture.isNull()) return true;
    texture = QImage(texturePath);
    return !texture.isNull();
}

QColor ObjMesh::averageColor() const {
    if (texture.isNull()) return QColor(120, 140, 170);
    const QImage t = texture.scaled(16, 16, Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation);
    qint64 r = 0, g = 0, b = 0;
    int n = 0;
    for (int y = 0; y < t.height(); ++y)
        for (int x = 0; x < t.width(); ++x) {
            const QRgb px = t.pixel(x, y);
            r += qRed(px); g += qGreen(px); b += qBlue(px);
            ++n;
        }
    if (n == 0) return QColor(120, 140, 170);
    return QColor(int(r / n), int(g / n), int(b / n));
}

static QString resolveTexturePath(const QString& objPath, const QString& mapKdIn,
                                  ObjMesh& out) {
    const QDir objDir = QFileInfo(objPath).dir();
    QString mapKd = mapKdIn;
    mapKd.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList candidates = {
        mapKdIn,
        mapKd,
        objDir.filePath(QFileInfo(mapKdIn).fileName()),
        objDir.filePath(QFileInfo(mapKd).fileName()),
        // Basenames comuns ao lado do OBJ (ex.: "Buggy texture.png").
        objDir.filePath(QFileInfo(objPath).completeBaseName() + QStringLiteral(".png")),
        objDir.filePath(QFileInfo(objPath).completeBaseName() + QStringLiteral(" texture.png")),
        objDir.filePath(QFileInfo(objPath).completeBaseName() + QStringLiteral("_texture.png")),
    };
    for (const QString& c : candidates) {
        if (c.isEmpty()) continue;
        if (QFileInfo::exists(c)) return QFileInfo(c).absoluteFilePath();
    }
    // Fallback: primeiro .png no diretório do OBJ.
    const QFileInfoList pngs =
        objDir.entryInfoList(QStringList() << QStringLiteral("*.png"),
                             QDir::Files, QDir::Name);
    if (!pngs.isEmpty()) return pngs.first().absoluteFilePath();
    Q_UNUSED(out);
    return QString();
}

bool loadObjFile(const QString& path, ObjMesh& out, QString* error) {
    out = ObjMesh();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("Não abriu %1").arg(path);
        return false;
    }
    QTextStream ts(&f);
    QVector<double> px, py, pz;
    QVector<QPointF> uvs;
    QString mtllib;
    QString mapKd;
    // Faces: índices de vértice + UV paralelos.
    QVector<QVector<int>> faces;
    QVector<QVector<int>> faceUVs;

    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList parts =
            line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        if (parts[0] == QLatin1String("v") && parts.size() >= 4) {
            bool ok1 = false, ok2 = false, ok3 = false;
            const double x = parts[1].toDouble(&ok1);
            const double y = parts[2].toDouble(&ok2);
            const double z = parts[3].toDouble(&ok3);
            if (ok1 && ok2 && ok3) {
                px.append(x); py.append(y); pz.append(z);
            }
        } else if (parts[0] == QLatin1String("vt") && parts.size() >= 3) {
            bool ok1 = false, ok2 = false;
            const double u = parts[1].toDouble(&ok1);
            const double v = parts[2].toDouble(&ok2);
            if (ok1 && ok2) uvs.append(QPointF(u, v));
        } else if (parts[0] == QLatin1String("mtllib") && parts.size() >= 2) {
            mtllib = parts[1];
            // mtllib pode ter espaços: junta o resto.
            for (int i = 2; i < parts.size(); ++i)
                mtllib += QLatin1Char(' ') + parts[i];
        } else if (parts[0] == QLatin1String("map_Kd") && parts.size() >= 2) {
            mapKd = parts.mid(1).join(QLatin1Char(' '));
        } else if (parts[0] == QLatin1String("f") && parts.size() >= 4) {
            QVector<int> idx;
            QVector<int> uidx;
            for (int i = 1; i < parts.size(); ++i) {
                const QStringList comps = parts[i].split(QLatin1Char('/'));
                bool ok = false;
                int vi = comps.value(0).toInt(&ok);
                if (!ok) continue;
                if (vi < 0) vi = int(px.size()) + vi + 1;
                if (vi < 1 || vi > px.size()) continue;
                idx.append(vi - 1);
                int ui = -1;
                if (comps.size() >= 2 && !comps[1].isEmpty()) {
                    bool okU = false;
                    ui = comps[1].toInt(&okU);
                    if (okU) {
                        if (ui < 0) ui = int(uvs.size()) + ui + 1;
                        if (ui < 1 || ui > uvs.size()) ui = -1;
                        else ui -= 1;
                    }
                }
                uidx.append(ui);
            }
            if (idx.size() >= 3) {
                faces.append(idx);
                faceUVs.append(uidx);
            }
        }
    }
    if (px.isEmpty() || faces.isEmpty()) {
        if (error) *error = QStringLiteral("OBJ sem vértices/faces");
        return false;
    }

    out.vertsXY.resize(px.size());
    out.vertsZ.resize(px.size());
    out.uvs = uvs;
    // Se não houver vt, cria (0,0) por vértice.
    if (out.uvs.size() < px.size())
        out.uvs.resize(px.size());
    out.faces = faces;
    out.faceUVs = faceUVs;

    out.minX = out.maxX = px[0];
    out.minY = out.maxY = py[0];
    out.minZ = out.maxZ = pz[0];
    for (int i = 0; i < px.size(); ++i) {
        out.vertsXY[i] = QPointF(px[i], py[i]);
        out.vertsZ[i] = pz[i];
        out.minX = std::min(out.minX, px[i]);
        out.maxX = std::max(out.maxX, px[i]);
        out.minY = std::min(out.minY, py[i]);
        out.maxY = std::max(out.maxY, py[i]);
        out.minZ = std::min(out.minZ, pz[i]);
        out.maxZ = std::max(out.maxZ, pz[i]);
    }

    // Material / textura.
    if (!mtllib.isEmpty()) {
        const QString mtlPath = QFileInfo(QFileInfo(path).dir().filePath(mtllib))
                                    .absoluteFilePath();
        out.mtlPath = mtlPath;
        QFile mf(mtlPath);
        if (mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ms(&mf);
            while (!ms.atEnd()) {
                const QString ml = ms.readLine().trimmed();
                if (ml.startsWith(QLatin1String("map_Kd")) && ml.size() > 6) {
                    mapKd = ml.mid(6).trimmed();
                    break;
                }
            }
        }
    }
    out.texturePath = resolveTexturePath(path, mapKd, out);
    out.loadTexture();

    out.normalize(200.0);
    return true;
}

void drawMeshTextured(QPainter& painter, const ObjMesh& mesh, double scale,
                      const QColor& fallback, double opacity) {
    if (mesh.isEmpty()) return;
    const bool tex = mesh.hasTexture() && !mesh.texture.isNull();
    const QColor face = tex ? mesh.averageColor() : fallback;

    painter.save();
    painter.setOpacity(opacity);

    // Ordena faces por profundidade (back-to-front).
    struct FaceD { int fi; double z; };
    QVector<FaceD> order;
    order.reserve(mesh.faces.size());
    for (int fi = 0; fi < mesh.faces.size(); ++fi) {
        const QVector<int>& idx = mesh.faces[fi];
        if (idx.size() < 3) continue;
        double zsum = 0; int n = 0;
        for (int vi : idx) {
            if (vi < 0 || vi >= mesh.vertsXY.size()) continue;
            zsum += mesh.vertsZ[vi];
            ++n;
        }
        if (n) order.append({fi, zsum / n});
    }
    std::sort(order.begin(), order.end(),
              [](const FaceD& a, const FaceD& b) { return a.z < b.z; });

    for (const FaceD& fd : order) {
        const QVector<int>& idx = mesh.faces[fd.fi];
        QPolygonF poly;
        for (int vi : idx) {
            if (vi < 0 || vi >= mesh.vertsXY.size()) continue;
            poly << mesh.vertsXY[vi] * scale;
        }
        if (poly.size() < 3) continue;

        if (tex) {
            // Raster aproximado: para cada face, amostra a textura nas UVs
            // e pinta o polígono com a cor interpolada (Gouraud leve).
            // MVP: preenche com a cor média da região UV + desenha a textura
            // recortada no bbox da face.
            QRectF bbox = poly.boundingRect();
            // Amostra textura nas UVs dos vértices.
            const QVector<int>& uvidx = (fd.fi < mesh.faceUVs.size())
                                            ? mesh.faceUVs[fd.fi] : QVector<int>();
            QColor samples[8];
            int ns = 0;
            for (int i = 0; i < idx.size() && ns < 8; ++i) {
                int ui = (i < uvidx.size()) ? uvidx[i] : -1;
                if (ui < 0 || ui >= mesh.uvs.size()) continue;
                const QPointF& uv = mesh.uvs[ui];
                const int tx = std::clamp(int(uv.x() * (mesh.texture.width() - 1)),
                                          0, mesh.texture.width() - 1);
                // V do OBJ: 0 embaixo; QImage: 0 em cima.
                const int ty = std::clamp(int((1.0 - uv.y()) * (mesh.texture.height() - 1)),
                                          0, mesh.texture.height() - 1);
                samples[ns++] = QColor(mesh.texture.pixel(tx, ty));
            }
            QColor fill = face;
            if (ns > 0) {
                int r = 0, g = 0, b = 0;
                for (int i = 0; i < ns; ++i) {
                    r += samples[i].red(); g += samples[i].green(); b += samples[i].blue();
                }
                fill = QColor(r / ns, g / ns, b / ns);
            }
            fill.setAlphaF(std::clamp(fill.alphaF() * opacity, 0.0, 1.0));
            painter.setPen(QPen(fill.darker(150), 1));
            painter.setBrush(fill);
            painter.drawPolygon(poly);

            // Sobreposição da textura recortada no bbox (aproxima UV).
            if (ns >= 2 && bbox.width() > 2 && bbox.height() > 2) {
                painter.save();
                QPainterPath clip;
                clip.addPolygon(poly);
                painter.setClipPath(clip);
                painter.setOpacity(std::clamp(opacity * 0.85, 0.0, 1.0));
                painter.drawImage(bbox, mesh.texture);
                painter.restore();
            }
        } else {
            QColor fc = face;
            fc.setAlphaF(std::clamp(fc.alphaF() * opacity, 0.0, 1.0));
            painter.setPen(QPen(fc.darker(160), 1));
            painter.setBrush(fc);
            painter.drawPolygon(poly);
        }
    }
    painter.restore();
}

} // namespace mesh
