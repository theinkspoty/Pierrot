// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MesaRenderer.h"
#include "colombina/ffmpeg/FFmpegDecoder.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/generators.h"
#include "colombina/mesh/ObjLoader.h"
#include "colombina/render/Math3D.h"

#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <cmath>

MesaRenderer::MesaRenderer() {}

MesaRenderer::~MesaRenderer() { clearCache(); }

void MesaRenderer::clearCache() {
    QMutexLocker l(&m_mutex);
    // shared_ptr ensures decoders are only deleted when no longer in use.
    m_decoders.clear();
    m_compositeLru.clear();
}

void MesaRenderer::clearCompositeCache() {
    QMutexLocker l(&m_mutex);
    m_compositeLru.clear();
}

QImage MesaRenderer::compositeFromCache(const CompositeKey& key) {
    QMutexLocker l(&m_mutex);
    for (int i = 0; i < m_compositeLru.size(); ++i) {
        if (m_compositeLru[i].key == key) {
            CompositeEntry e = m_compositeLru.takeAt(i);
            m_compositeLru.prepend(e);
            return e.img;
        }
    }
    return QImage();
}

void MesaRenderer::compositeToCache(const CompositeKey& key, const QImage& img) {
    QMutexLocker l(&m_mutex);
    for (int i = 0; i < m_compositeLru.size(); ++i) {
        if (m_compositeLru[i].key == key) {
            m_compositeLru[i].img = img;
            return;
        }
    }
    CompositeEntry e;
    e.key = key;
    e.img = img;
    m_compositeLru.prepend(e);
    while (m_compositeLru.size() > kCompositeMax)
        m_compositeLru.removeLast();
}

QImage MesaRenderer::decodeFrame(const QString& filePath, double time, int maxW) {
    // Decodifica a partir do proxy (vídeo leve) quando houver; a chave de cache
    // é o vpath, para não misturar original e proxy do mesmo clipe.
    const QString vpath = ProxyManager::instance().resolveVideo(filePath);

    std::shared_ptr<FFmpegDecoder> dec;
    {
        // Seção CURTA com o lock global: só o mapa de decoders e o cache de
        // quadro único são compartilhados entre threads. O decode pesado fica
        // FORA do lock para arquivos distintos decodarem em paralelo.
        QMutexLocker l(&m_mutex);
        if (m_frameCache.key.path == vpath && qFuzzyCompare(m_frameCache.key.time, time)
            && m_frameCache.key.maxW == maxW && !m_frameCache.frame.isNull()) {
            return m_frameCache.frame;
        }
        auto it = m_decoders.find(vpath);
        if (it != m_decoders.end()) {
            dec = it.value();
        } else {
            dec = std::make_shared<FFmpegDecoder>();
            m_decoders.insert(vpath, dec);
        }
    }

    // Decode sem o lock global: cada FFmpegDecoder serializa o PRÓPRIO acesso
    // internamente (m_mutex do decoder), então dois arquivos diferentes podem
    // ser decodificados por threads distintas ao mesmo tempo (worker de decode
    // e thread da UI via requestLowerLayers) sem risco de SIGSEGV.
    if (!dec->isOpen()) {
        if (!dec->open(vpath)) return {};
    }
    QImage frame = dec->frameAt(time, maxW);
    {
        QMutexLocker l(&m_mutex);
        m_frameCache = { { vpath, time, maxW }, frame };
    }
    return frame;
}

QImage MesaRenderer::render(const MesaComposition& mesa, const Project& project,
                             double time) {
    const int outW = project.width;
    const int outH = project.height;
    if (outW <= 0 || outH <= 0 || mesa.canvasW <= 0 || mesa.canvasH <= 0) return {};

    const int fps = qMax(1, project.fps);
    const CompositeKey key{ mesa.id, qRound64(time * fps), outW, outH, project.revision };
    const QImage cached = compositeFromCache(key);
    if (!cached.isNull()) return cached;

    // Pré-decodifica as faixas com mídia real em paralelo (pool global). O paint
    // a seguir decodifica o MESMO (path, tempo, maxW) → cache de quadro do
    // decoder já quente. Menos queda de FPS com várias camadas HD/4K visíveis.
    warmTracks(mesa, project, time);

    QImage result;
    const double frameDur = 1.0 / fps;
    const double extent = mesa.motionBlurShutter * frameDur;

    // Motion blur de CÂMERA: o enquadramento é redefinido em cada sub-passada
    // (mostra o rastro da câmera). Cada passada é uma composição LIMPA da pilha
    // (motionBlurStack desligado) — são as passadas dela que integram o rastro;
    // as layers (já com o transform em ts) borram naturalmente junto.
    if (mesa.motionBlur && mesa.motionBlurSamples >= 2 && extent > 0.0) {
        const int n = qBound(2, mesa.motionBlurSamples, 32);
        QImage out(outW, outH, QImage::Format_ARGB32);
        out.fill(Qt::transparent);
        QPainter p(&out);
        p.setClipRect(0, 0, outW, outH);
        const double w = 1.0 / n;
        for (int i = 0; i < n; ++i) {
            const double frac = n == 1 ? 0.0 : double(i) / double(n - 1);
            const double ts = time - extent / 2.0 + frac * extent;
            QImage sample = renderSample(mesa, project, ts, nullptr, false);
            p.save();
            p.setOpacity(w);
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            p.drawImage(0, 0, sample);
            p.restore();
        }
        result = out;
    } else {
        result = renderSample(mesa, project, time, nullptr);
    }

    if (!result.isNull())
        compositeToCache(key, result);
    return result;
}

// Focal em px de saída para um FOV vertical dado.
//
// UMA fórmula para o projeto inteiro. Havia quatro divergentes: o Preview/export
// usava outH, o canvas da Mesa usava max(canvasW,canvasH), o 2D usava um piso de
// 64 e o renderMeshLayer usava fh*0.75 com camDist fixo em 300 — ignorando
// camZ/camFov. Efeito practicalo: a MESMA track da Mesa mudava de tamanho e
// posição conforme fosse renderizada como clipe de topo ou como camada de
// baixo, e o Preview não batia com o canvas da Mesa.
double MesaRenderer::focalForHeight(int outH, double fovDeg) {
    const double h = std::max(1, outH);
    const double fov = std::max(5.0, fovDeg);
    return (h * 0.5) / std::tan(fov * 3.14159265358979323846 / 360.0);
}

// Desenha uma passada inteira: câmera (no instante `time`) + todas as layers.
// Câmera 2D (padrão): T(centro)·R·S·T(-camX,-camY).
// Câmera 3D (mesa3d=true, AE Classic): projeção perspectiva com FOV + Z;
// camadas ordenadas por Z (back-to-front); malhas OBJ projetadas vertex a vertex.
QImage MesaRenderer::renderSample(const MesaComposition& mesa, const Project& project,
                                  double time, const QString* skipTrackId,
                                  bool motionBlurStack) {
    const int outW = project.width;
    const int outH = project.height;
    if (outW <= 0 || outH <= 0 || mesa.canvasW <= 0 || mesa.canvasH <= 0) return {};

    QImage out(outW, outH, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setClipRect(0, 0, outW, outH);

    const double camX = kfValue(mesa.kfCamX, mesa.camX, time);
    const double camY = kfValue(mesa.kfCamY, mesa.camY, time);
    const double zoom = qMax(0.001, kfValue(mesa.kfCamZoom, mesa.camZoom, time));
    const double rot = kfValue(mesa.kfCamRotation, mesa.camRotation, time);
    const double fit = qMin(double(outW) / mesa.canvasW,
                            double(outH) / mesa.canvasH);

    if (!mesa.mesa3d) {
        // ── Caminho 2D original (zero regressão) ──────────────────────────
        const double s = zoom * fit;
        p.translate(outW / 2.0, outH / 2.0);
        p.rotate(rot);
        p.scale(s, s);
        p.translate(-camX, -camY);
        renderToPainter(p, mesa, project, time, skipTrackId, motionBlurStack);
        return out;
    }

    // ── Fase 1: Mesa 3D (AE Classic) ─────────────────────────────────────
    // Câmera: posição (camX, camY, camZ), FOV, pitch/yaw; POI default = centro.
    const double camZ = kfValue(mesa.kfCamZ, mesa.camZ, time);
    const double camFov = std::max(5.0, kfValue(mesa.kfCamFov, mesa.camFov, time));
    const double camPitch = kfValue(mesa.kfCamPitch, mesa.camPitch, time);
    const double camYaw = kfValue(mesa.kfCamYaw, mesa.camYaw, time);
    const double poiX = kfValue(mesa.kfCamPoiX, mesa.camPoiX, time);
    const double poiY = kfValue(mesa.kfCamPoiY, mesa.camPoiY, time);
    const double poiZ = kfValue(mesa.kfCamPoiZ, mesa.camPoiZ, time);

    // Focal em px de saída a partir do FOV (vertical).
    const double focal = focalForHeight(outH, camFov);

    // Projeção de um ponto do espaço da Mesa (x,y,z) para o frame de saída.
    // Convenção AE Classic: câmera em camZ (frente do canvas, Z+ = p/ câmera);
    // camada em mesaZ. dist = camZ - z (quanto menor, mais perto).
    auto projectPoint = [&](double x, double y, double z, double& sx, double& sy, bool& ok) {
        const double dist = camZ - z;
        if (dist < 1.0) { ok = false; return; } // atrás da câmera
        const double k = focal / dist;
        // Offset da câmera no plano + rotação 2D (roll) + pitch/yaw simples:
        // pitch desloca Y, yaw desloca X (aproximação de órbita leve).
        const double rad = rot * 3.14159265358979323846 / 180.0;
        const double c = std::cos(rad), s = std::sin(rad);
        double lx = (x - camX) * k;
        double ly = (y - camY) * k;
        // Pitch/yaw: deslocam o ponto de vista (como girar a câmera).
        ly -= camPitch * (dist / 100.0) * k * 0.01;
        lx += camYaw * (dist / 100.0) * k * 0.01;
        // POI: se diferente de camX/camY, corrige o enquadramento.
        lx += (camX - poiX) * k * 0.0; // POI já embutido em camX/camY por padrão
        Q_UNUSED(poiY); Q_UNUSED(poiZ);
        const double rx = lx * c - ly * s;
        const double ry = lx * s + ly * c;
        sx = outW * 0.5 + rx;
        sy = outH * 0.5 + ry;
        ok = true;
    };

    // Coleta tracks da Mesa com Z (ordenadas: fundo → topo = Z menor → maior
    // quando Z+ = perto da câmera; empilhamos do MAIOR Z (perto) para o menor
    // se preferirmos transparência — aqui: do fundo (Z pequeno) para frente).
    struct Cand {
        const Track* tr = nullptr;
        double z = 0.0;
        double x = 0.0, y = 0.0;
        double scX = 1.0, scY = 1.0, rotDeg = 0.0;
        double rotX = 0.0, rotY = 0.0;
        double op = 1.0;
        bool isMesh = false;
        QString meshPath;
    };
    QVector<Cand> cands;
    for (const QString& tid : mesa.trackIds) {
        if (skipTrackId && *skipTrackId == tid) continue;
        const Track* track = nullptr;
        for (const Track& tr : project.videoTracks)
            if (tr.id == tid) { track = &tr; break; }
        if (!track)
            for (const Track& tr : project.audioTracks)
                if (tr.id == tid) { track = &tr; break; }
        if (!track || track->mesaHidden) continue;
        Cand cd;
        cd.tr = track;
        const double t = time;
        cd.z = kfValue(track->kfMesaZ, track->mesaZ, t);
        cd.x = kfValue(track->kfMesaX, track->mesaX, t);
        cd.y = kfValue(track->kfMesaY, track->mesaY, t);
        cd.scX = kfValue(track->kfMesaScaleX, track->mesaScaleX, t);
        cd.scY = track->kfMesaScaleY.isEmpty() ? cd.scX
                 : kfValue(track->kfMesaScaleY, track->mesaScaleY, t);
        cd.rotDeg = kfValue(track->kfMesaRotation, track->mesaRotation, t);
        cd.rotX = kfValue(track->kfMesaRotX, track->mesaRotX, t);
        cd.rotY = kfValue(track->kfMesaRotY, track->mesaRotY, t);
        cd.op = std::clamp(kfValue(track->kfMesaOpacity, track->mesaOpacity, t), 0.0, 1.0);
        cd.isMesh = !track->meshPath.isEmpty();
        cd.meshPath = track->meshPath;
        cands.append(cd);
    }
    // Back-to-front: Z menor (longe) primeiro; desempate por trackIds.
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Cand& a, const Cand& b) { return a.z < b.z; });

    for (const Cand& cd : cands) {
        const Track* track = cd.tr;
        if (cd.isMesh) {
            mesh::ObjMesh mesh;
            if (!mesh::loadObjFile(cd.meshPath, mesh) || mesh.isEmpty()) continue;
            if (mesh.vertsXY.isEmpty() || mesh.vertsXY.size() != mesh.vertsZ.size())
                continue;
            // Centróide da malha (pivô de rotação no próprio eixo).
            double cxSum = 0, cySum = 0, czSum = 0;
            for (int i = 0; i < mesh.vertsXY.size(); ++i) {
                cxSum += mesh.vertsXY[i].x();
                cySum += mesh.vertsXY[i].y();
                czSum += mesh.vertsZ[i];
            }
            const double nV = std::max(1, int(mesh.vertsXY.size()));
            const double meshCx = cxSum / nV;
            const double meshCy = cySum / nV;
            const double meshCz = czSum / nV;
            const double radZ = cd.rotDeg * 3.14159265358979323846 / 180.0;
            const double radX = cd.rotX * 3.14159265358979323846 / 180.0;
            const double radY = cd.rotY * 3.14159265358979323846 / 180.0;
            p.save();
            p.setOpacity(cd.op);
            const QColor face = track->color.isValid() ? track->color
                                                       : (mesh.hasTexture() && !mesh.texture.isNull()
                                                              ? mesh.averageColor()
                                                              : QColor(120, 140, 170));
            auto projectVert = [&](int vi, double& sx, double& sy, bool& ok, double& wz) {
                double lx0, ly0, lz0;
                if (mesh.has3D()) {
                    lx0 = mesh.vertsX[vi];
                    ly0 = mesh.vertsY[vi];
                    lz0 = mesh.vertsZ[vi];
                } else {
                    lx0 = mesh.vertsXY[vi].x();
                    ly0 = mesh.vertsXY[vi].y();
                    lz0 = mesh.vertsZ[vi];
                }
                double lx = (lx0 - meshCx) * cd.scX;
                double ly = (ly0 - meshCy) * cd.scY;
                double lz = (lz0 - meshCz) * cd.scX;
                // Rotação XYZ no próprio eixo (em torno do centroide).
                double x1 = lx * std::cos(radZ) - ly * std::sin(radZ);
                double y1 = lx * std::sin(radZ) + ly * std::cos(radZ);
                double z1 = lz;
                double y2 = y1 * std::cos(radX) - z1 * std::sin(radX);
                double z2 = y1 * std::sin(radX) + z1 * std::cos(radX);
                double x3 = x1 * std::cos(radY) + z2 * std::sin(radY);
                double z3 = -x1 * std::sin(radY) + z2 * std::cos(radY);
                wz = cd.z + z3 + meshCz * cd.scX; // mantém profundidade relativa
                projectPoint(cd.x + x3 + meshCx * cd.scX,
                             cd.y + y2 + meshCy * cd.scY,
                             wz, sx, sy, ok);
            };
            // Ordena faces por profundidade média (pintura back-to-front).
            // `face` = índice da face no OBJ, precisa sobreviver ao sort para
            // casar com mesh.faceUVs na textura.
            struct FaceD { QVector<int> idx; double z; int face = -1; };
            QVector<FaceD> faces;
            faces.reserve(mesh.faces.size());
            for (int faceNo = 0; faceNo < mesh.faces.size(); ++faceNo) {
                const QVector<int>& fi = mesh.faces[faceNo];
                if (fi.size() < 3) continue;
                double zsum = 0.0;
                int n = 0;
                for (int vi : fi) {
                    if (vi < 0 || vi >= mesh.vertsXY.size()) continue;
                    double sx = 0, sy = 0, wz = 0;
                    bool ok = false;
                    projectVert(vi, sx, sy, ok, wz);
                    if (!ok) continue;
                    zsum += wz;
                    ++n;
                }
                if (n == 0) continue;
                FaceD fd;
                fd.idx = fi;
                fd.z = zsum / n;
                fd.face = faceNo;
                faces.append(fd);
            }
            std::stable_sort(faces.begin(), faces.end(),
                             [](const FaceD& a, const FaceD& b) { return a.z < b.z; });
            for (const FaceD& fd : faces) {
                QPolygonF poly;
                for (int vi : fd.idx) {
                    if (vi < 0 || vi >= mesh.vertsXY.size()) continue;
                    double sx = 0, sy = 0, wz = 0;
                    bool ok = false;
                    projectVert(vi, sx, sy, ok, wz);
                    if (ok) poly << QPointF(sx, sy);
                }
                if (poly.size() < 3) continue;
                // Sombreamento simples por profundidade.
                const double shade = std::clamp(0.55 + 0.45 * ((fd.z - (cd.z - 80)) / 160.0),
                                                 0.35, 1.0);
                if (mesh.hasTexture() && !mesh.texture.isNull()) {
                    QColor fill = face;
                    fill.setAlphaF(std::clamp(cd.op * shade, 0.0, 1.0));
                    p.setPen(QPen(fill.darker(150), 1.0));
                    p.setBrush(fill);
                    p.drawPolygon(poly);
                    // Textura mapeada por UV. Se faltar UV utilizável,
                    // fillFaceTextured devolve false sem desenhar e a cor
                    // chapada acima fica — nunca a textura esticada dentro da
                    // bbox, que saía deslocada em qualquer rotação/perspectiva.
                    QPainterPath clipPath;
                    clipPath.addPolygon(poly);
                    p.save();
                    p.setClipPath(clipPath);
                    mesh::fillFaceTextured(p, mesh, fd.face, poly,
                                           std::clamp(cd.op * shade * 0.9, 0.0, 1.0));
                    p.restore();
                } else {
                    QColor fc = face;
                    fc.setAlphaF(std::clamp(cd.op * shade, 0.0, 1.0));
                    p.setPen(QPen(fc.darker(160), 1.0));
                    p.setBrush(fc);
                    p.drawPolygon(poly);
                    p.setPen(QPen(fc.lighter(170), 1.0));
                    p.setBrush(Qt::NoBrush);
                    p.drawPolygon(poly);
                }
            }
            p.restore();
            continue;
        }

        // Camada 2D (imagem/texto): projeção da âncora + escala perspectiva.
        double sx = 0, sy = 0;
        bool ok = false;
        projectPoint(cd.x, cd.y, cd.z, sx, sy, ok);
        if (!ok) continue;
        const double dist = std::max(1.0, camZ - cd.z);
        const double persp = focal / dist;

        LayerPrep prep;
        if (!prepareLayer(prep, mesa, project, time, *track, time)) continue;
        // Reancora: o LayerPrep usa posição 2D; sobrepõe a projeção 3D.
        prep.posX = sx;
        prep.posY = sy;
        // Escala relativa: persp já está no painter? Não — aplicamos na imagem.
        p.save();
        p.setOpacity(prep.opacity);
        p.translate(sx, sy);
        p.rotate(prep.rot + rot); // roll extra da câmera
        p.scale(prep.sx * persp / fit, prep.sy * persp / fit);
        // Âncora: desloca o centro natural da imagem.
        p.translate(-prep.ax * persp / fit, -prep.ay * persp / fit);
        p.setCompositionMode(static_cast<QPainter::CompositionMode>(prep.blend));
        p.drawImage(QPointF(-prep.frame.width() * 0.5, -prep.frame.height() * 0.5),
                    prep.frame);
        p.restore();
    }

    return out;
}

// Empilha as camadas da composição num painter já no canvas-space.
// `relTime` fixa o CONTEÚDO; `transformTime` avalia os keyframes de transform.
void MesaRenderer::paintStack(QPainter& painter, const MesaComposition& mesa,
                              const Project& project, double relTime,
                              const QString* skipTrackId, double transformTime) {
    for (const QString& tid : mesa.trackIds) {
        if (skipTrackId && *skipTrackId == tid) continue;

        const Track* track = nullptr;
        for (const Track& tr : project.videoTracks) {
            if (tr.id == tid) { track = &tr; break; }
        }
        if (!track) {
            for (const Track& tr : project.audioTracks) {
                if (tr.id == tid) { track = &tr; break; }
            }
        }
        if (!track) continue;

        drawTrackLayer(painter, *track, mesa, project, relTime, transformTime);
    }
}

// Desenha uma única track (camada) num painter `acc` já preparado.
// Retorna false se nada foi desenhado (sem clip ativo / frame vazio).
bool MesaRenderer::drawTrackLayer(QPainter& acc, const Track& track,
                                  const MesaComposition& mesa, const Project& project,
                                  double relTime, double transformTime) {
    // Malha 3D (OBJ) — Fase 3 MVP: desenha faces preenchidas no espaço da Mesa.
    if (!track.meshPath.isEmpty()) {
        if (track.mesaHidden) return false;
        const double t = (transformTime >= 0.0 && track.mesaMotionBlur) ? transformTime
                                                                        : relTime;
        const double tMesaX = kfValue(track.kfMesaX, track.mesaX, t);
        const double tMesaY = kfValue(track.kfMesaY, track.mesaY, t);
        const double tScX = kfValue(track.kfMesaScaleX, track.mesaScaleX, t);
        const double tScY = kfValue(track.kfMesaScaleY, track.mesaScaleY, t);
        const double tRot = kfValue(track.kfMesaRotation, track.mesaRotation, t);
        const double tZ = kfValue(track.kfMesaZ, track.mesaZ, t);
        const double tOp = std::clamp(kfValue(track.kfMesaOpacity, track.mesaOpacity, t),
                                      0.0, 1.0);
        mesh::ObjMesh mesh;
        if (!mesh::loadObjFile(track.meshPath, mesh) || mesh.isEmpty()) return false;

        // Antes esta branch fazia só translate/rotate/scale 2D, pintava cada
        // face com uma cor chapada e NUNCA olhava a textura — o que fazia o
        // canvas da Mesa mostrar uma mancha cinza enquanto o Preview e o
        // export mostravam a malha texturizada. Agora usa a mesma projeção
        // (câmera + perspectiva por vértice + ordenação por Z) do renderMeshLayer.
        const double kPi = 3.14159265358979323846;
        double focal = 1.0;
        double camDist = 300.0;
        if (mesa.mesa3d) {
            const double fov = std::max(5.0, kfValue(mesa.kfCamFov, mesa.camFov, relTime));
            focal = focalForHeight(mesa.canvasH, fov);
            // camZ == 0 põe a câmera dentro do objeto (divisão degenerada).
            // addMeshToMesa já evita isso, mas uma track editada à mão não tem
            // essa garantia, e aqui o clamp segurava tudo em perspectiva 1.0.
            const double camZ = kfValue(mesa.kfCamZ, mesa.camZ, relTime);
            camDist = (camZ > 1.0) ? camZ : 300.0;
        } else {
            focal = std::max(64.0, mesa.canvasH * 0.5);
        }

        // Centróide como pivô de rotação (a malha entra normalizada na origem).
        int nv = 0;
        double sx0 = 0, sy0 = 0, sz0 = 0;
        for (int i = 0; i < mesh.vertsXY.size(); ++i) {
            sx0 += mesh.has3D() ? mesh.vertsX[i] : mesh.vertsXY[i].x();
            sy0 += mesh.has3D() ? mesh.vertsY[i] : mesh.vertsXY[i].y();
            sz0 += mesh.vertsZ[i];
            ++nv;
        }
        if (nv == 0) return false;
        const double cxm = sx0 / nv, cym = sy0 / nv, czm = sz0 / nv;

        // Projeção por vértice. `dist` é clampado porque uma face atrás da
        // câmera inverteria o polígono em vez de sumir.
        QVector<QPointF> proj(mesh.vertsXY.size());
        QVector<double> vz(mesh.vertsXY.size());
        for (int i = 0; i < mesh.vertsXY.size(); ++i) {
            const double lx = (mesh.has3D() ? mesh.vertsX[i] : mesh.vertsXY[i].x()) - cxm;
            const double ly = (mesh.has3D() ? mesh.vertsY[i] : mesh.vertsXY[i].y()) - cym;
            vz[i] = mesh.vertsZ[i] - czm;
            const double dist = std::max(40.0, camDist - (vz[i] + tZ));
            const double k = focal / dist;
            proj[i] = QPointF(lx * k, ly * k);
        }

        struct FaceP { QPolygonF poly; double z; int face = -1; };
        QVector<FaceP> faces;
        faces.reserve(mesh.faces.size());
        for (int faceNo = 0; faceNo < mesh.faces.size(); ++faceNo) {
            const QVector<int>& idx = mesh.faces[faceNo];
            if (idx.size() < 3) continue;
            QPolygonF poly;
            double mz = 0;
            for (int vi : idx) {
                if (vi < 0 || vi >= proj.size()) continue;
                poly << proj[vi];
                mz += vz[vi];
            }
            if (poly.size() >= 3)
                faces.append(FaceP{poly, mz / poly.size() + tZ, faceNo});
        }
        // Pintor algorítmico: longe primeiro. Estável para não tremer quando
        // duas faces empatam no mesmo Z.
        std::stable_sort(faces.begin(), faces.end(),
                         [](const FaceP& a, const FaceP& b) { return a.z < b.z; });

        const bool textured = mesh.hasTexture() && !mesh.texture.isNull();
        const QColor avg = mesh.averageColor();
        QColor base = track.color.isValid() ? track.color
                   : (avg.isValid() ? avg : QColor(120, 140, 170));

        acc.save();
        acc.setOpacity(tOp);
        acc.translate(tMesaX, tMesaY);
        acc.rotate(tRot);
        acc.scale(tScX, tScY);
        for (const FaceP& f : faces) {
            // Rampa de profundidade: mais perto = mais claro. Mesma curva do
            // renderMeshLayer, senão a malha muda de tom entre Preview e Mesa.
            const double shade = std::clamp(0.45 + 0.55 * ((f.z + 150.0) / 300.0), 0.35, 1.0);
            QColor fc = base;
            fc.setAlphaF(std::clamp(tOp * shade, 0.0, 1.0));
            acc.setPen(QPen(fc.darker(160), 1.0));
            acc.setBrush(fc);
            acc.drawPolygon(f.poly);
            if (textured) {
                QPainterPath clipPath;
                clipPath.addPolygon(f.poly);
                acc.save();
                acc.setClipPath(clipPath);
                mesh::fillFaceTextured(acc, mesh, f.face, f.poly,
                                       std::clamp(tOp * shade * 0.9, 0.0, 1.0));
                acc.restore();
            }
        }
        acc.restore();
        return true;
    }

    LayerPrep prep;
    if (!prepareLayer(prep, mesa, project, relTime, track, transformTime)) return false;
    drawTrackImage(acc, prep);
    return true;
}

// Renderiza uma malha 3D no tamanho da composição, já com o transform do clipe
// (tx/ty, escala, roll, rotX/rotY, Z). A convenção de eixos é a MESMA do
// PivotCanvas (Y sem inversão), para Preview e Pivot não divergirem.
static QImage renderMeshLayer(const mesh::ObjMesh& mesh, const Clip& c,
                              const MesaComposition& mesa, double relTime) {
    const int fw = qMax(64, (int)mesa.canvasW);
    const int fh = qMax(32, (int)mesa.canvasH);
    QImage img(fw, fh, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    if (mesh.isEmpty()) return img;

    const double kPi = 3.14159265358979323846;
    const double radZ = kfValue(c.kfRotation, c.rotation, relTime) * kPi / 180.0;
    const double radX = kfValue(c.kfClipRotX, c.clipRotX, relTime) * kPi / 180.0;
    const double radY = kfValue(c.kfClipRotY, c.clipRotY, relTime) * kPi / 180.0;
    const double sc = std::max(0.01, kfValue(c.kfScale, c.scale, relTime));
    const double z = kfValue(c.kfClipZ, c.clipZ, relTime);
    const double tx = kfValue(c.kfTx, c.tx, relTime);
    const double ty = kfValue(c.kfTy, c.ty, relTime);

    // Centróide como pivô de rotação (a malha entra normalizada na origem).
    int nv = 0;
    double sx0 = 0, sy0 = 0, sz0 = 0;
    for (int i = 0; i < mesh.vertsXY.size(); ++i) {
        const double vx = mesh.has3D() ? mesh.vertsX[i] : mesh.vertsXY[i].x();
        const double vy = mesh.has3D() ? mesh.vertsY[i] : mesh.vertsXY[i].y();
        sx0 += vx; sy0 += vy; sz0 += mesh.vertsZ[i]; ++nv;
    }
    if (nv == 0) return img;
    const double cxm = sx0 / nv, cym = sy0 / nv, czm = sz0 / nv;

    // Mesma câmera do resto do renderer (focalForHeight + camZ real). Antes
    // eram fh*0.75 e camDist fixo em 300, ignorando camFov/camZ: a mesma track
    // da Mesa saía diferente quando renderizada como camada de baixo.
    const double camFov = kfValue(mesa.kfCamFov, mesa.camFov, relTime);
    const double camZ = kfValue(mesa.kfCamZ, mesa.camZ, relTime);
    const double focal = MesaRenderer::focalForHeight(fh, camFov);
    const double camDist = (camZ > 1.0) ? camZ : 300.0;
    const QPointF origin(fw * 0.5 + tx, fh * 0.5 + ty);

    auto projectVert = [&](int vi, QPointF& out, double& wz) {
        if (vi < 0 || vi >= mesh.vertsXY.size()) { out = QPointF(); wz = 0; return; }
        const double lx = ((mesh.has3D() ? mesh.vertsX[vi] : mesh.vertsXY[vi].x()) - cxm) * sc;
        const double ly = ((mesh.has3D() ? mesh.vertsY[vi] : mesh.vertsXY[vi].y()) - cym) * sc;
        const double lz = (mesh.vertsZ[vi] - czm) * sc;
        double x1 = lx * std::cos(radZ) - ly * std::sin(radZ);
        double y1 = lx * std::sin(radZ) + ly * std::cos(radZ);
        double y2 = y1 * std::cos(radX) - lz * std::sin(radX);
        double z2 = y1 * std::sin(radX) + lz * std::cos(radX);
        double x3 = x1 * std::cos(radY) + z2 * std::sin(radY);
        double z3 = -x1 * std::sin(radY) + z2 * std::cos(radY);
        const double dist = std::max(40.0, camDist - (z3 + z));
        const double k = focal / dist;
        // Mesma convenção do PivotCanvas (Y sem inversão): Preview e Pivot têm
        // que mostrar o objeto na MESMA orientação — divergir aqui seria uma
        // paridade preview↔editor quebrada.
        out = QPointF(origin.x() + x3 * k, origin.y() + y2 * k);
        wz = z3 + z;
    };

    struct FaceD { QVector<int> idx; double z; int face = -1; };
    QVector<FaceD> faces;
    faces.reserve(mesh.faces.size());
    for (int faceNo = 0; faceNo < mesh.faces.size(); ++faceNo) {
        const QVector<int>& fi = mesh.faces[faceNo];
        if (fi.size() < 3) continue;
        double zsum = 0.0; int n = 0;
        for (int vi : fi) {
            if (vi < 0 || vi >= mesh.vertsXY.size()) continue;
            QPointF o; double wz = 0;
            projectVert(vi, o, wz);
            zsum += wz; ++n;
        }
        if (n == 0) continue;
        faces.append({ fi, zsum / n, faceNo });
    }
    std::stable_sort(faces.begin(), faces.end(),
                     [](const FaceD& a, const FaceD& b) { return a.z < b.z; });

    const bool textured = mesh.hasTexture() && !mesh.texture.isNull();
    const QColor base = textured ? mesh.averageColor() : QColor(120, 140, 170);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const FaceD& fd : faces) {
        QPolygonF poly;
        for (int vi : fd.idx) {
            if (vi < 0 || vi >= mesh.vertsXY.size()) continue;
            QPointF o; double wz = 0;
            projectVert(vi, o, wz);
            poly << o;
        }
        if (poly.size() < 3) continue;
        // Faces do fundo mais escuras: dá leitura de volume sem shader.
        const double shade = std::clamp(0.45 + 0.55 * ((fd.z + 150.0) / 300.0), 0.35, 1.0);
        QColor fc = base;
        fc.setAlphaF(shade);
        p.setPen(QPen(fc.darker(160), 1.0));
        p.setBrush(fc);
        p.drawPolygon(poly);
        if (textured) {
            QPainterPath clipPath;
            clipPath.addPolygon(poly);
            p.save();
            p.setClipPath(clipPath);
            mesh::fillFaceTextured(p, mesh, fd.face, poly, shade);
            p.restore();
        }
    }
    p.end();
    return img;
}

bool MesaRenderer::prepareLayer(LayerPrep& out, const MesaComposition& mesa,
                                const Project& project, double relTime,
                                const Track& track, double transformTime) {
    // Layer oculta (olho desligado) não existe no empilhamento — nem no
    // canvas do editor, nem no preview, nem (futuro) no export.
    if (track.mesaHidden) return false;
    // tempo de transform separado do tempo de conteúdo: `transformTime` rasteja
    // os keyframes das props de canvas (motion blur); `relTime` fixa o clip
    // ativo. Camada com motion blur desligado (mesaMotionBlur=false) fica
    // FIXA no relTime — é ela não borra nas sub-passadas.
    const double t = (transformTime >= 0.0 && track.mesaMotionBlur) ? transformTime
                                                                    : relTime;
    out.blend = blendModeFor(track.blendMode);
    const double tMesaX = kfValue(track.kfMesaX, track.mesaX, t);
    const double tMesaY = kfValue(track.kfMesaY, track.mesaY, t);
    const double tScX = kfValue(track.kfMesaScaleX, track.mesaScaleX, t);
    const double tScY = kfValue(track.kfMesaScaleY, track.mesaScaleY, t);
    const double tRot = kfValue(track.kfMesaRotation, track.mesaRotation, t);
    const double tOp = std::clamp(kfValue(track.kfMesaOpacity, track.mesaOpacity, t),
                                  0.0, 1.0);

    // Encontra o clip ativo nesta track no tempo rel
    QImage frame;
    for (const Clip& c : track.clips) {
        const double cRel = relTime - c.pos;
        if (cRel < 0 || cRel >= c.dur) continue;

        if (c.isText) {
            // Renderiza texto no TAMANHO NATURAL da camada (a composição).
            // A escala é aplicada pela matriz da camada — antes o texto era
            // escalado duas vezes (frame × sc e de novo no drawImage).
            const TextStyle* ts = project.textStyleFor(c);
            const int fw = qMax(64, (int)(mesa.canvasW));
            const int fh = qMax(32, (int)(mesa.canvasH));
            frame = QImage(fw, fh, QImage::Format_ARGB32);
            frame.fill(Qt::transparent);
            const double sizeFrac = ts->textSize > 0.0 ? ts->textSize : (1.0 / 18.0);
            const int pxSize = qMax(4, (int)qRound(sizeFrac * fh));
            QFont font;
            if (!ts->fontFamily.isEmpty()) font.setFamily(ts->fontFamily);
            font.setPixelSize(pxSize);
            font.setBold(ts->textBold);
            QPainter fp(&frame);
            fp.setRenderHint(QPainter::Antialiasing);
            fp.translate(fw / 2.0, fh / 2.0);
            QPainterPath path;
            path.addText(QPointF(0, 0), font, c.text.text.isEmpty()
                ? ts->text : c.text.text);
            if (ts->textOutline > 0.0)
                fp.strokePath(path, QPen(ts->textOutlineColor,
                                         qMax(1.0, ts->textOutline * fh)));
            fp.fillPath(path, ts->textColor);
        } else if (!c.mediaId.isEmpty() || c.hasMulticam()) {
            const MediaItem* mi = project.findMedia(c.mediaIdAt(cRel));
            if (mi) {
                if (mi->isSolid) {
                    // Mídia virtual (sólido/gradiente/checkerboard/noise):
                    // gerada no tamanho próprio (ou da comp, se não definido).
                    const int fw = mi->width > 0 ? mi->width : (int)mesa.canvasW;
                    const int fh = mi->height > 0 ? mi->height : (int)mesa.canvasH;
                    frame = generatorFrame(*mi, fw, fh);
                } else if (mi->isMesh && !mi->filePath.isEmpty()) {
                    // Malha 3D: renderizada no CPU com o transform do clipe.
                    // Não passa por FFmpeg (um .obj não é mídia decodificável),
                    // por isso o ramo vem antes do `filePath` genérico.
                    mesh::ObjMesh mesh;
                    if (mesh::loadObjFile(mi->filePath, mesh) && !mesh.isEmpty())
                        frame = renderMeshLayer(mesh, c, mesa, cRel);
                } else if (!mi->filePath.isEmpty()) {
                    const double srcT = clipSrcTime(c, cRel);
                    frame = decodeFrame(mi->filePath, srcT, mesa.canvasW);
                }
            }
        }
        if (!frame.isNull()) break;
    }

    if (frame.isNull()) return false;

    out.frame = frame;
    // Posição = coordenada absoluta da âncora na composição (origem topo-left).
    out.posX = tMesaX;
    out.posY = tMesaY;
    out.rot = tRot;
    out.sx = tScX;
    out.sy = tScY;
    // Âncora: offset do centro natural da layer (px da própria layer).
    out.ax = kfValue(track.kfMesaAnchorX, track.mesaAnchorX, relTime);
    out.ay = kfValue(track.kfMesaAnchorY, track.mesaAnchorY, relTime);
    out.opacity = tOp;
    out.valid = true;
    return true;
}

void MesaRenderer::drawTrackImage(QPainter& acc, const LayerPrep& prep) {
    if (!prep.valid || prep.frame.isNull()) return;
    // Matriz local→comp idêntica ao After Effects:
    // M = T(posição) · R(rotação) · S(escala) · T(-âncora),
    // com o frame desenhado com o topo-esquerdo na origem local.
    // QPainter compõe na ordem das chamadas (1ª = mais externa), então a
    // sequência abaixo gera exatamente M (ver desenho do quad em layerScreenRect).
    acc.save();
    acc.translate(prep.posX, prep.posY);
    acc.rotate(prep.rot);
    acc.scale(prep.sx, prep.sy);
    acc.translate(-(prep.frame.width() / 2.0 + prep.ax),
                  -(prep.frame.height() / 2.0 + prep.ay));

    acc.setRenderHint(QPainter::SmoothPixmapTransform);
    if (prep.opacity < 1.0)
        acc.setOpacity(prep.opacity);
    acc.setCompositionMode(static_cast<QPainter::CompositionMode>(prep.blend));
    acc.drawImage(QRectF(0, 0, prep.frame.width(), prep.frame.height()), prep.frame);
    acc.restore();
}

void MesaRenderer::warmTracks(const MesaComposition& mesa, const Project& project,
                              double relTime) {
    // Mesma resolução de clip ativo do prepareLayer(): 1º clip ativo em relTime
    // manda; texto e sólido são gerados na pintura (sem arquivo pra decode).
    QList<WarmJob> jobs;
    for (const QString& tid : mesa.trackIds) {
        const Track* track = nullptr;
        for (const Track& tr : project.videoTracks) {
            if (tr.id == tid) { track = &tr; break; }
        }
        if (!track) {
            for (const Track& tr : project.audioTracks) {
                if (tr.id == tid) { track = &tr; break; }
            }
        }
        // Faixa oculta (olho desligado) também é pulada no paintStack.
        if (!track || track->mesaHidden) continue;

        for (const Clip& c : track->clips) {
            const double cRel = relTime - c.pos;
            if (cRel < 0 || cRel >= c.dur) continue;

            if (!c.isText && (!c.mediaId.isEmpty() || c.hasMulticam())) {
                const MediaItem* mi = project.findMedia(c.mediaIdAt(cRel));
                // `isMesh` fora: malha é rasterizada no paint, não decodificada.
                if (mi && !mi->isSolid && !mi->isMesh && !mi->filePath.isEmpty()) {
                    jobs.append({ mi->filePath, clipSrcTime(c, cRel),
                                  mesa.canvasW });
                }
            }
            break;
        }
    }

    if (jobs.isEmpty()) return;
    if (jobs.size() == 1) {
        // Um único decode: rodar direto é mais barato que o overhead do pool.
        decodeFrame(jobs[0].filePath, jobs[0].srcT, jobs[0].maxW);
        return;
    }

    // Em paralelo: cada FFmpegDecoder serializa o PRÓPRIO acesso (m_mutex do
    // decoder), então arquivos distintos decodam simultaneamente. O QImage de
    // retorno é descartado — o objetivo é aquecer o cache de quadro do decoder;
    // o paint pass seguinte (mesma chave path/tempo/maxW) encontra pronto.
    QtConcurrent::mapped(jobs, [this](const WarmJob& j) {
        return decodeFrame(j.filePath, j.srcT, j.maxW);
    }).results();
}

// Mapeia o blendMode textual do Track para o QPainter::CompositionMode.
int MesaRenderer::blendModeFor(const QString& blend) const {
    if (blend == QStringLiteral("add"))       return QPainter::CompositionMode_Plus;
    if (blend == QStringLiteral("multiply"))  return QPainter::CompositionMode_Multiply;
    if (blend == QStringLiteral("screen"))    return QPainter::CompositionMode_Screen;
    if (blend == QStringLiteral("overlay"))   return QPainter::CompositionMode_Overlay;
    if (blend == QStringLiteral("softlight")) return QPainter::CompositionMode_SoftLight;
    if (blend == QStringLiteral("difference")) return QPainter::CompositionMode_Difference;
    return QPainter::CompositionMode_SourceOver;
}

void MesaRenderer::renderToPainter(QPainter& painter, const MesaComposition& mesa,
                                   const Project& project, double relTime,
                                   const QString* skipTrackId, bool motionBlurStack) {
    const bool mb = motionBlurStack && mesa.motionBlur && mesa.motionBlurSamples >= 2
                 && mesa.motionBlurShutter > 0.0;
    if (!mb) {
        paintStack(painter, mesa, project, relTime, skipTrackId, relTime);
        return;
    }

    // Motion blur por sub-passadas COMPLETAS (estilo AE): cada passada desenha a
    // pilha inteira no instante `ts` (oclusão entre camadas correta em cada uma);
    // as n passadas somam com peso 1/n num buffer do tamanho do clip. Estática:
    // n·(1/n) = identidade (nada borra); em movimento, as bordas ficam com
    // cobertura parcial (as próprias bordas do rastro). O buffer usa o clip do
    // painter para capturar a VIEW atual da câmera sem depender do tamanho do
    // canvas infinito.
    const QRect cr = painter.clipBoundingRect().toAlignedRect();
    if (cr.isEmpty() || cr.width() > 16384 || cr.height() > 16384) {
        paintStack(painter, mesa, project, relTime, skipTrackId, relTime);
        return;
    }

    const int n = qBound(2, mesa.motionBlurSamples, 32);
    const double frameDur = 1.0 / qMax(1, project.fps);
    const double extent = mesa.motionBlurShutter * frameDur;
    const QTransform view = painter.transform();

    QImage acc(cr.size(), QImage::Format_ARGB32_Premultiplied);
    acc.fill(Qt::transparent);
    QPainter ap(&acc);
    ap.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int i = 0; i < n; ++i) {
        const double frac = n == 1 ? 0.0 : double(i) / double(n - 1);
        const double ts = relTime - extent / 2.0 + frac * extent;
        QImage pass(cr.size(), QImage::Format_ARGB32_Premultiplied);
        pass.fill(Qt::transparent);
        QPainter pp(&pass);
        pp.setRenderHint(QPainter::SmoothPixmapTransform);
        // Mesma view da câmera do painter, deslocada para o sistema de
        // coordenadas do buffer: pixel (x,y) do pass = device (x+cr.x, y+cr.y).
        // O QImage clipa sozinho nas bordas; nada além do buffer é pintado.
        QTransform passT = view;
        passT.translate(-cr.x(), -cr.y());   // translate pré-multiplicado (saída)
        pp.setTransform(passT);
        // Conteúdo FIXO em relTime; só o transform das camadas rasteja em ts.
        paintStack(pp, mesa, project, relTime, skipTrackId, ts);
        pp.end();
        ap.save();
        ap.setOpacity(1.0 / n);
        ap.setCompositionMode(QPainter::CompositionMode_Plus);
        ap.drawImage(0, 0, pass);
        ap.restore();
    }
    ap.end();

    // Composição final em DEVICE coordinates (cr está em device): zera o
    // transform do painter para o buffer cair exatamente sobre o região do clip.
    painter.save();
    painter.resetTransform();
    painter.drawImage(cr.topLeft(), acc);
    painter.restore();
}
