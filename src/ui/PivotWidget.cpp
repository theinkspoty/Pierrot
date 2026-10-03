// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "PivotWidget.h"
#include "ui/Theme.h"
#include "colombina/ffmpeg/FFmpegDecoder.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/render/Math3D.h"

#include <QDoubleSpinBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

// ── PivotCanvas ──────────────────────────────────────────────────────────

// Mantém o ângulo no intervalo (-180, 180] sem trancar a rotação: o usuário
// pode girar livremente e o valor sempre é comparável/serializável.
static double normDeg(double a) {
    a = std::fmod(a, 360.0);
    if (a > 180.0) a -= 360.0;
    if (a <= -180.0) a += 360.0;
    return a;
}

namespace {

constexpr double kPi = 3.14159265358979323846;
// Parâmetros de câmera do canvas do Pivot (uma única transformação, aplicada
// uma única vez — ver PivotCanvas::paintEvent).
constexpr double kFocal = 280.0;
constexpr double kCamDist = 300.0;
// Raio do gizmo em unidades de malha normalizada (normalize() visa 200, logo a
// meia-extensão fica ~100).
constexpr double kGizRadius = 112.0;
constexpr int kRingSamples = 56;

// Rotação + perspectiva de um ponto 3D local → tela. `sc` entra aqui para que
// malha e gizmo compartilhem a mesma escala (o gizmo "gruda" no objeto).
// `wz`, quando pedido, recebe a profundidade JÁ ROTACIONADA — é ela que ordena
// as faces (back-to-front) e sombreia o fundo.
inline QPointF project3(double lx, double ly, double lz, double radZ, double radX,
                        double radY, double sc, double z, const QPointF& origin,
                        double* wz = nullptr) {
    lx *= sc; ly *= sc; lz *= sc;
    const double x1 = lx * std::cos(radZ) - ly * std::sin(radZ);
    const double y1 = lx * std::sin(radZ) + ly * std::cos(radZ);
    const double y2 = y1 * std::cos(radX) - lz * std::sin(radX);
    const double z2 = y1 * std::sin(radX) + lz * std::cos(radX);
    const double x3 = x1 * std::cos(radY) + z2 * std::sin(radY);
    const double z3 = -x1 * std::sin(radY) + z2 * std::cos(radY);
    const double dist = std::max(40.0, kCamDist - (z3 + z));
    const double k = kFocal / dist;
    if (wz) *wz = z3 * sc;
    return QPointF(origin.x() + x3 * k, origin.y() + y2 * k);
}

} // namespace

QRect PivotCanvas::plotRect() const {
    return QRect(20, 20, std::max(10, width() - 40), std::max(10, height() - 50));
}

QPointF PivotCanvas::gizmoOrigin(double tx, double ty) const {
    const QPointF c = plotRect().center();
    return QPointF(c.x() + tx, c.y() + ty);
}

// Estado 3D do clipe no playhead — o gizmo e a malha leem daqui, para que
// nunca fiquem dessincronizados entre si.
void PivotCanvas::currentTransform(double& rot, double& rotX, double& rotY,
                                   double& sc, double& z, double& tx,
                                   double& ty) const {
    rot = rotX = rotY = z = tx = ty = 0.0;
    sc = 1.0;
    const Clip* c = resolveClip();
    if (!c) return;
    const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    rot  = kfValue(c->kfRotation, c->rotation, rel);
    rotX = kfValue(c->kfClipRotX, c->clipRotX, rel);
    rotY = kfValue(c->kfClipRotY, c->clipRotY, rel);
    sc   = std::max(0.01, kfValue(c->kfScale, c->scale, rel));
    z    = kfValue(c->kfClipZ, c->clipZ, rel);
    tx   = kfValue(c->kfTx, c->tx, rel);
    ty   = kfValue(c->kfTy, c->ty, rel);
}

// As 3 circunferências do gizmo, na ordem X, Y, Z. Cada uma é um círculo no
// plano perpendicular ao eixo, projetado com a MESMA transformação da malha —
// por isso os anéis giram junto com o objeto, como no Blender.
QVector<QVector<QPointF>> PivotCanvas::gizmoRings(const QPointF& origin, double rot,
                                                  double rotX, double rotY,
                                                  double sc, double z) const {
    const double rz = rot * kPi / 180.0;
    const double rx = rotX * kPi / 180.0;
    const double ry = rotY * kPi / 180.0;
    QVector<QVector<QPointF>> out;
    out.reserve(3);
    for (int axis = 0; axis < 3; ++axis) {
        QVector<QPointF> poly;
        poly.reserve(kRingSamples + 1);
        for (int i = 0; i <= kRingSamples; ++i) {
            const double a = 2.0 * kPi * i / kRingSamples;
            const double ca = std::cos(a) * kGizRadius;
            const double sa = std::sin(a) * kGizRadius;
            double lx, ly, lz;
            if (axis == 0)      { lx = 0.0;   ly = ca; lz = sa; } // X: plano YZ
            else if (axis == 1) { lx = ca;    ly = 0.0; lz = sa; } // Y: plano XZ
            else                { lx = ca;    ly = sa; lz = 0.0; } // Z: plano XY
            poly << project3(lx, ly, lz, rz, rx, ry, sc, z, origin);
        }
        out.append(poly);
    }
    return out;
}

int PivotCanvas::ringAt(const QPointF& origin, const QPoint& pos, double tolPx) const {
    double r, rotX, rotY, sc, z, tx, ty;
    currentTransform(r, rotX, rotY, sc, z, tx, ty);
    const QVector<QVector<QPointF>> rings = gizmoRings(origin, r, rotX, rotY, sc, z);
    const QPointF c(pos);
    int bestAxis = 0;
    double bestDist = tolPx;
    for (int axis = 0; axis < rings.size(); ++axis) {
        const QVector<QPointF>& poly = rings[axis];
        for (int i = 0; i + 1 < poly.size(); ++i) {
            // Distância do ponto ao segmento (não só aos vérticesSamples): sem
            // isso o clique cai entre dois samples e o anel "some".
            const QPointF a = poly[i], b = poly[i + 1];
            const QPointF ab = b - a;
            const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
            double t = 0.0;
            if (len2 > 1e-9) t = std::clamp(((c - a).x() * ab.x() + (c - a).y() * ab.y()) / len2,
                                           0.0, 1.0);
            const QPointF proj(a.x() + ab.x() * t, a.y() + ab.y() * t);
            const double d = std::hypot(c.x() - proj.x(), c.y() - proj.y());
            if (d < bestDist) { bestDist = d; bestAxis = axis + 1; }
        }
    }
    return bestAxis;
}

PivotCanvas::PivotCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

Clip* PivotCanvas::resolveClip() const {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    return nullptr;
}

void PivotCanvas::setClipId(Project* project, const QString& clipId, double playhead) {
    m_project = project;
    m_clipId = clipId;
    m_playhead = playhead;
    m_frame = QImage();
    m_mesh = mesh::ObjMesh();
    m_hasMesh = false;
    Clip* m_clip = resolveClip();
    if (m_clip && m_project) {
        const MediaItem* m = m_project->findMedia(m_clip->mediaId);
        if (m) {
            if (m->isMesh && !m->filePath.isEmpty()) {
                m_hasMesh = mesh::loadObjFile(m->filePath, m_mesh);
            } else if (!m->filePath.isEmpty() && m->hasVideo) {
                ensureFrame();
            }
        }
        double lo = m_clip->tx, hi = m_clip->tx;
        lo = std::min({lo, m_clip->ty, m_clip->scale});
        hi = std::max({hi, m_clip->ty, m_clip->scale});
        if (!m_clip->kfSpeed.isEmpty()) {
            for (const Keyframe& k : m_clip->kfSpeed) {
                lo = std::min(lo, k.value);
                hi = std::max(hi, k.value);
            }
        }
        m_valLo = lo - 0.5;
        m_valHi = hi + 0.5;
    }
    update();
}

void PivotCanvas::setClip(Clip* clip, Project* project, double playhead) {
    setClipId(project, clip ? clip->id : QString(), playhead);
}

void PivotCanvas::setPlayhead(double t) {
    m_playhead = t;
    // NÃO decodifica aqui (thread de paint/UI): só atualiza o tempo.
    // O quadro de mídia é carregado em setClip / em idle via throttle.
    update();
}

void PivotCanvas::commitChange() {
    emit changed();
    update();
}

// Carrega o frame da mídia sob demanda (não no paint).
void PivotCanvas::ensureFrame() {
    Clip* m_clip = resolveClip();
    if (!m_clip || !m_project || m_hasMesh) return;
    const MediaItem* m = m_project->findMedia(m_clip->mediaId);
    if (!m || m->isSolid || m->filePath.isEmpty() || !m->hasVideo) return;
    FFmpegDecoder dec;
    dec.setHardwareDecodeAllowed(false);
    dec.open(ProxyManager::instance().resolveVideo(m->filePath), -1);
    const double srcT = clipSrcTime(*m_clip,
        std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur));
    m_frame = dec.frameAt(srcT, 480);
}

void PivotCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& tc = themeColors();
    p.fillRect(rect(), tc.base);

    Clip* m_clip = resolveClip();
    if (!m_clip || !m_project) {
        p.setPen(tc.monitorLabel);
        p.drawText(rect(), Qt::AlignCenter,
                   tr("Selecione um clipe 3D (imagem ou malha)"));
        return;
    }

    const QRect plot = plotRect();
    p.setPen(tc.trackBorder);
    p.drawRect(plot);

    double rot = 0, rotX = 0, rotY = 0, sc = 1, z = 0, tx = 0, ty = 0;
    currentTransform(rot, rotX, rotY, sc, z, tx, ty);
    const double radZ = rot * kPi / 180.0;
    const double radX = rotX * kPi / 180.0;
    const double radY = rotY * kPi / 180.0;
    // Origem 3D = centro do plot deslocado pelo pan do clipe. O gizmo nasce
    // AQUI, e não no centro fixo: senão ele descola do objeto assim que o
    // clipe é movido, e parece que a rotação gira no lugar errado.
    const QPointF origin = gizmoOrigin(tx, ty);

    // Uma única transformação para o objeto inteiro. A versão anterior
    // empilhava `p.rotate(rot)`/`p.scale(sc*persp)` no painter E aplicava
    // radZ/radX/radY + outra perspectiva dentro — rotação e escala contadas
    // duas vezes, o que deformava os eixos e fazia a rotação "não encaixar".
    if (m_hasMesh && !m_mesh.isEmpty() && !m_mesh.vertsXY.isEmpty()
            && m_mesh.vertsXY.size() == m_mesh.vertsZ.size()) {
        const QColor face = m_mesh.hasTexture() && !m_mesh.texture.isNull()
                                ? m_mesh.averageColor()
                                : QColor(120, 150, 200);
        // Centróide como pivô: a rotação acontece no centro da malha.
        int nv = 0;
        double sx0 = 0, sy0 = 0, sz0 = 0;
        for (int i = 0; i < m_mesh.vertsXY.size(); ++i) {
            sx0 += m_mesh.has3D() ? m_mesh.vertsX[i] : m_mesh.vertsXY[i].x();
            sy0 += m_mesh.has3D() ? m_mesh.vertsY[i] : m_mesh.vertsXY[i].y();
            sz0 += m_mesh.vertsZ[i];
            ++nv;
        }
        const double cxm = nv ? sx0 / nv : 0.0;
        const double cym = nv ? sy0 / nv : 0.0;
        const double czm = nv ? sz0 / nv : 0.0;

        struct FaceD { QVector<int> idx; double wz; };
        QVector<FaceD> faces;
        faces.reserve(m_mesh.faces.size());
        for (const QVector<int>& fi : m_mesh.faces) {
            if (fi.size() < 3) continue;
            double zsum = 0.0; int n = 0;
            for (int vi : fi) {
                if (vi < 0 || vi >= m_mesh.vertsXY.size()) continue;
                double wz = 0.0;
                project3(m_mesh.has3D() ? m_mesh.vertsX[vi] - cxm
                                        : m_mesh.vertsXY[vi].x() - cxm,
                         m_mesh.has3D() ? m_mesh.vertsY[vi] - cym
                                        : m_mesh.vertsXY[vi].y() - cym,
                         m_mesh.vertsZ[vi] - czm,
                         radZ, radX, radY, sc, z, origin, &wz);
                zsum += wz;
                ++n;
            }
            if (n == 0) continue;
            faces.append({ fi, zsum / n });
        }
        std::stable_sort(faces.begin(), faces.end(),
                         [](const FaceD& a, const FaceD& b) { return a.wz < b.wz; });

        const bool textured = m_mesh.hasTexture() && !m_mesh.texture.isNull();
        for (const FaceD& fd : faces) {
            QPolygonF poly;
            for (int vi : fd.idx) {
                if (vi < 0 || vi >= m_mesh.vertsXY.size()) continue;
                poly << project3(m_mesh.has3D() ? m_mesh.vertsX[vi] - cxm
                                                : m_mesh.vertsXY[vi].x() - cxm,
                                 m_mesh.has3D() ? m_mesh.vertsY[vi] - cym
                                                : m_mesh.vertsXY[vi].y() - cym,
                                 m_mesh.vertsZ[vi] - czm,
                                 radZ, radX, radY, sc, z, origin);
            }
            if (poly.size() < 3) continue;
            const double shade = std::clamp(0.45 + 0.55 * ((fd.wz + 150.0) / 300.0),
                                            0.35, 1.0);
            QColor fc = face;
            fc.setAlphaF(shade);
            p.setPen(QPen(fc.darker(160), 1));
            p.setBrush(fc);
            p.drawPolygon(poly);
            if (textured) {
                const QRectF bbox = poly.boundingRect();
                QPainterPath clipPath;
                clipPath.addPolygon(poly);
                p.save();
                p.setClipPath(clipPath);
                p.setOpacity(shade);
                p.drawImage(bbox, m_mesh.texture);
                p.restore();
            }
        }
    } else if (!m_frame.isNull()) {
        // Imagem 2D: sem profundidade, então a projeção 3D não se aplica —
        // o painter resolve rotate+scale diretamente.
        const QImage img = m_frame.scaled(160, 120, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
        p.save();
        p.translate(origin);
        p.rotate(rot);
        p.scale(sc, sc);
        p.drawImage(QPointF(-img.width() * 0.5, -img.height() * 0.5), img);
        p.restore();
    } else {
        p.save();
        p.translate(origin);
        p.setPen(QColor(120, 120, 120));
        p.drawRect(QRectF(-40, -30, 80, 60));
        p.drawText(QRectF(-40, -30, 80, 60), Qt::AlignCenter, tr("3D"));
        p.restore();
    }

    // ── Gizmo trackball (estilo Blender) ──────────────────────────────────
    // Esfera com 3 circunferências: arrastar um anel gira SÓ aquele eixo;
    // arrastar o miolo gira livre (X+Y). As circunferências são as mesmas do
    // hit-test, então o que você vê é exatamente o que você clica.
    {
        const QVector<QVector<QPointF>> rings = gizmoRings(origin, rot, rotX, rotY, sc, z);
        const QColor cols[3] = { QColor(235, 80, 80),    // X
                                 QColor(90, 220, 110),   // Y
                                 QColor(110, 160, 255) };// Z
        const int rotMode = property("pivotRotMode").toInt();

        // Halo da esfera: dá a leitura de "miolo" para o trackball livre.
        double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
        for (const QVector<QPointF>& ring : rings)
            for (const QPointF& pt : ring) {
                minX = std::min(minX, pt.x()); maxX = std::max(maxX, pt.x());
                minY = std::min(minY, pt.y()); maxY = std::max(maxY, pt.y());
            }
        if (minX <= maxX) {
            p.setPen(QPen(QColor(255, 255, 255, 46), 1));
            p.setBrush(QColor(255, 255, 255, 22));
            p.drawEllipse(QRectF(QPointF(minX, minY), QPointF(maxX, maxY)));
        }

        // Anel sob o cursor ganha espessura — é o feedback do Blender.
        const int hoverAxis = (m_drag == DragRotX || m_drag == DragRotY
                               || m_drag == DragRotZ)
                                  ? (m_drag == DragRotX ? 1 : m_drag == DragRotY ? 2 : 3)
                                  : 0;
        p.setBrush(Qt::NoBrush);
        for (int axis = 0; axis < rings.size(); ++axis) {
            const bool active = hoverAxis == axis + 1 || rotMode == axis + 1;
            p.setPen(QPen(cols[axis], active ? 3.2 : 1.7));
            p.drawPolyline(rings[axis]);
        }

        // AxisVisible: bolinha + rótulo na ponta de cada eixo.
        p.setFont(QFont(p.font().family(), 8, QFont::Bold));
        const char* names[3] = { "X", "Y", "Z" };
        for (int axis = 0; axis < 3; ++axis) {
            const QVector<QPointF>& ring = rings[axis];
            // A "ponta" do eixo é o ponto do anel mais afastado do centro.
            int far = 0;
            double bestD = -1.0;
            for (int i = 0; i < ring.size(); ++i) {
                const double d = std::hypot(ring[i].x() - origin.x(),
                                           ring[i].y() - origin.y());
                if (d > bestD) { bestD = d; far = i; }
            }
            const QPointF tip = ring[far];
            p.setPen(Qt::NoPen);
            p.setBrush(cols[axis]);
            p.drawEllipse(tip, 4, 4);
            p.setPen(cols[axis]);
            p.drawText(tip + QPointF(7, -4), QString::fromLatin1(names[axis]));
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 150), 1));
        p.drawEllipse(origin, 5, 5);
    }


    // Rótulo.
    p.setPen(tc.spinText);
    p.setFont(QFont(p.font().family(), 8, QFont::Bold));
    p.drawText(QRect(plot.left() + 4, plot.top() + 2, plot.width() - 8, 14),
               Qt::AlignLeft,
               tr("Pivot · %1  |  arraste=girar  Shift=Z  Ctrl=escala/snap  RX/RY/RZ=eixo")
                   .arg(m_hasMesh ? tr("malha") : tr("imagem")));
}

void PivotCanvas::mousePressEvent(QMouseEvent* e) {
    Clip* m_clip = resolveClip();
    if (!m_clip || e->button() != Qt::LeftButton) return;
    emit editStart();
    m_dragStart = e->pos();
    const double rel = std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur);
    m_origTx = m_clip->tx;
    m_origTy = m_clip->ty;
    m_origScale = m_clip->scale;
    m_origRot = m_clip->rotation;
    m_origRotX = m_clip->clipRotX;
    m_origRotY = m_clip->clipRotY;
    m_origZ = m_clip->clipZ;

    // Sobrescrever mousePressEvent sem chamar a base impede o Qt de dar foco,
    // e sem foco o nudge por setas nunca chega em keyPressEvent.
    setFocus(Qt::MouseFocusReason);

    // Prioridade 0: ferramenta de mover armada (botões Mover/H/V/Z). Quando
    // nenhum botão está marcado, o arraste é rotação — o default agora é
    // girar, porque é o que se faz 90% das vezes neste dock.
    m_moveAxis = property("pivotMoveMode").toInt();
    if (m_moveAxis > 0 && !(e->modifiers() & Qt::ControlModifier)) {
        m_drag = DragMove;
        emit statusMessage(m_moveAxis == 2 ? tr("Pivot: mover no eixo H")
                          : m_moveAxis == 3 ? tr("Pivot: mover no eixo V")
                          : m_moveAxis == 4 ? tr("Pivot: mover no eixo Z (profundidade)")
                                            : tr("Pivot: mover livre (H e V)"));
        Q_UNUSED(rel);
        update();
        return;
    }

    // Blender: Shift=Z, Ctrl=escala, RX/RY/RZ = eixo; default = trackball.
    const int rotMode = property("pivotRotMode").toInt();
    if (e->modifiers() & Qt::ShiftModifier) {
        m_drag = DragZ;
        emit statusMessage(tr("Pivot: arrastar = Z (profundidade)"));
    } else if (e->modifiers() & Qt::ControlModifier) {
        m_drag = DragScale;
        emit statusMessage(tr("Pivot: arrastar = escala"));
    } else {
        // Prioridade 1: circunferência do gizmo sob o cursor → eixo travado.
        double r0, rx0, ry0, sc0, z0, tx0, ty0;
        currentTransform(r0, rx0, ry0, sc0, z0, tx0, ty0);
        const QPointF origin = gizmoOrigin(tx0, ty0);
        const int axis = ringAt(origin, e->pos(), 11.0);
        if (axis == 1) {
            m_drag = DragRotX;
            m_ringAngle0 = std::atan2(e->pos().y() - origin.y(), e->pos().x() - origin.x());
            emit statusMessage(tr("Pivot: anel X — rotação travada no eixo X"));
        } else if (axis == 2) {
            m_drag = DragRotY;
            m_ringAngle0 = std::atan2(e->pos().y() - origin.y(), e->pos().x() - origin.x());
            emit statusMessage(tr("Pivot: anel Y — rotação travada no eixo Y"));
        } else if (axis == 3) {
            m_drag = DragRotZ;
            m_ringAngle0 = std::atan2(e->pos().y() - origin.y(), e->pos().x() - origin.x());
            emit statusMessage(tr("Pivot: anel Z — rotação travada no eixo Z"));
        } else if (rotMode == 1) {
            m_drag = DragRotX;
            emit statusMessage(tr("Pivot: arrastar = rotX"));
        } else if (rotMode == 2) {
            m_drag = DragRotY;
            emit statusMessage(tr("Pivot: arrastar = rotY"));
        } else if (rotMode == 3) {
            m_drag = DragRotZ;
            emit statusMessage(tr("Pivot: arrastar = rotZ (roll)"));
        } else {
            // Miolo da esfera: trackball livre (X + Y), como o Blender.
            m_drag = DragTrackball;
            emit statusMessage(tr("Pivot: trackball livre — arraste na esfera (Ctrl=snap 15°)"));
        }
    }
    Q_UNUSED(rel);
    update();
}

void PivotCanvas::mouseMoveEvent(QMouseEvent* e) {
    Clip* m_clip = resolveClip();
    if (!m_clip || m_drag == DragNone) return;
    const QPointF d = e->pos() - m_dragStart;
    const bool snap = e->modifiers() & Qt::ControlModifier;
    // Aplica um delta em graus respeitando o snap de 15° do Blender.
    auto applySnap = [this, m_clip, snap](double base, double delta) {
        const double v = base + delta;
        return snap ? std::round(v / 15.0) * 15.0 : v;
    };

    if (m_drag == DragMove) {
        // `pivotMoveMode`: 1 = livre, 2 = trava H, 3 = trava V, 4 = Z.
        // Travar o eixo é o que torna o arraste utilizável para posicionamento
        // preciso — no modo livre o mouse nunca "cola" no valor que você quer.
        switch (m_moveAxis) {
        case 2: m_clip->tx = m_origTx + d.x() * 0.5; break;
        case 3: m_clip->ty = m_origTy + d.y() * 0.5; break;
        case 4: m_clip->clipZ = std::clamp(m_origZ - d.y() * 2.0, -2000.0, 2000.0); break;
        default: // 1 = livre (e 0 = fallback defensivo)
            m_clip->tx = m_origTx + d.x() * 0.5;
            m_clip->ty = m_origTy + d.y() * 0.5;
            break;
        }
    } else if (m_drag == DragScale) {
        const double f = 1.0 + d.x() / 200.0;
        m_clip->scale = std::clamp(m_origScale * f, 0.05, 20.0);
    } else if (m_drag == DragZ) {
        m_clip->clipZ = std::clamp(m_origZ - d.y() * 2.0, -2000.0, 2000.0);
    } else if (m_drag == DragTrackball) {
        // Livre: horizontal gira Y, vertical gira X. Mesma resposta do trackball
        // do Blender mapeado em Euler — o modelo de dados continua Euler, então
        // os keyframes de rotX/rotY seguem funcionando sem mudança de schema.
        const double k = 0.45;
        m_clip->clipRotY = normDeg(m_origRotY + d.x() * k);
        m_clip->clipRotX = normDeg(m_origRotX + d.y() * k);
    } else {
        // Anel: o ângulo do cursor em volta do centro é o que comanda o eixo.
        // É isso que faz o anel "colado" no dedo.
        double r0, rx0, ry0, sc0, z0, tx0, ty0;
        currentTransform(r0, rx0, ry0, sc0, z0, tx0, ty0);
        const QPointF origin = gizmoOrigin(tx0, ty0);
        const double a1 = std::atan2(e->pos().y() - origin.y(), e->pos().x() - origin.x());
        double delta = (a1 - m_ringAngle0) * 180.0 / kPi;
        // Despasso de ±180° do atan2 não deve teleportar o objeto.
        if (delta > 180.0) delta -= 360.0;
        if (delta < -180.0) delta += 360.0;
        const double k = 0.6;
        if (m_drag == DragRotX)      m_clip->clipRotX = normDeg(applySnap(m_origRotX, delta * k));
        else if (m_drag == DragRotY) m_clip->clipRotY = normDeg(applySnap(m_origRotY, delta * k));
        else                         m_clip->rotation  = normDeg(applySnap(m_origRot,  delta * k));
    }
    update();
}

void PivotCanvas::keyPressEvent(QKeyEvent* e) {
    Clip* m_clip = resolveClip();
    if (!m_clip) { QWidget::keyPressEvent(e); return; }

    // Nudge fino: é a ferramenta de posicionamento precisa. Setas = 1 px,
    // Shift+setas = 10 px. Shift aqui é coerente com o Blender (passo maior).
    const bool coarse = e->modifiers() & Qt::ShiftModifier;
    const double step = coarse ? 10.0 : 1.0;
    double dtx = 0.0, dty = 0.0;
    switch (e->key()) {
    case Qt::Key_Left:  dtx = -step; break;
    case Qt::Key_Right: dtx =  step; break;
    case Qt::Key_Up:    dty = -step; break;
    case Qt::Key_Down:  dty =  step; break;
    case Qt::Key_Escape:
        // Esc limpa a ferramenta armada (volta para rotação).
        setProperty("pivotMoveMode", 0);
        setProperty("pivotRotMode", 0);
        emit statusMessage(tr("Pivot: ferramenta limpa — o arraste volta a girar"));
        update();
        e->accept();
        return;
    default:
        QWidget::keyPressEvent(e);
        return;
    }
    emit editStart();
    m_clip->tx += dtx;
    m_clip->ty += dty;
    emit changed();
    update();
    e->accept();
}

void PivotCanvas::mouseReleaseEvent(QMouseEvent*) {
    if (m_drag != DragNone) {
        m_drag = DragNone;
        commitChange();
    }
}

// ── PivotWidget (dock) ───────────────────────────────────────────────────

PivotWidget::PivotWidget(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);

    m_title = new QLabel(tr("Pivot — 3D do clipe"), this);
    m_title->setStyleSheet(QStringLiteral("font-weight:600;"));
    lay->addWidget(m_title);

    auto* tools = new QHBoxLayout;
    m_addKf = new QToolButton(this);
    m_addKf->setText(tr("+kf"));
    m_addKf->setToolTip(tr("Adiciona keyframe de transform no playhead"));
    m_delKf = new QToolButton(this);
    m_delKf->setText(tr("−kf"));
    m_delKf->setToolTip(tr("Remove keyframe mais próximo do playhead"));
    m_resetBtn = new QToolButton(this);
    m_resetBtn->setText(tr("Reset"));
    m_resetBtn->setToolTip(tr("Zera transform 2D/3D do clipe"));
    auto* rotXBtn = new QToolButton(this);
    rotXBtn->setText(tr("RX"));
    rotXBtn->setToolTip(tr("Arrastar no canvas gira no eixo X"));
    rotXBtn->setCheckable(true);
    auto* rotYBtn = new QToolButton(this);
    rotYBtn->setText(tr("RY"));
    rotYBtn->setToolTip(tr("Arrastar no canvas gira no eixo Y"));
    rotYBtn->setCheckable(true);
    auto* rotZBtn = new QToolButton(this);
    rotZBtn->setText(tr("RZ"));
    rotZBtn->setToolTip(tr("Arrastar no canvas gira no eixo Z (roll)"));
    rotZBtn->setCheckable(true);
    tools->addWidget(m_addKf);
    tools->addWidget(m_delKf);
    tools->addWidget(m_resetBtn);
    tools->addSpacing(8);
    tools->addWidget(rotXBtn);
    tools->addWidget(rotYBtn);
    tools->addWidget(rotZBtn);
    tools->addSpacing(10);
    // Ferramenta de mover. Nenhum botão marcado = arraste gira (default).
    // "Mover" = livre; H/V travam o eixo; Z mexe na profundidade.
    auto* mvFree = new QToolButton(this);
    mvFree->setText(tr("Mover"));
    mvFree->setToolTip(tr("Arrastar move o clipe em H e V ao mesmo tempo"));
    mvFree->setCheckable(true);
    auto* mvH = new QToolButton(this);
    mvH->setText(tr("H"));
    mvH->setToolTip(tr("Arrastar move só na horizontal"));
    mvH->setCheckable(true);
    auto* mvV = new QToolButton(this);
    mvV->setText(tr("V"));
    mvV->setToolTip(tr("Arrastar move só na vertical"));
    mvV->setCheckable(true);
    auto* mvZ = new QToolButton(this);
    mvZ->setText(tr("Z"));
    mvZ->setToolTip(tr("Arrastar move só na profundidade (Z)"));
    mvZ->setCheckable(true);
    tools->addWidget(mvFree);
    tools->addWidget(mvH);
    tools->addWidget(mvV);
    tools->addWidget(mvZ);
    tools->addStretch();
    lay->addLayout(tools);

    m_canvas = new PivotCanvas(this);
    lay->addWidget(m_canvas, 1);

    // Mini-timeline de keyframes (embaixo, como pedido).
    auto* kfStrip = new QWidget(this);
    kfStrip->setObjectName(QStringLiteral("pivotKfStrip"));
    // 7 canais (pos X/Y, escala, rot, Z, RX, RY). Com 36 px ficavam ~5 px por
    // canal e as bolinhas se sobrepunham num borrão só.
    kfStrip->setFixedHeight(66);
    kfStrip->setFocusPolicy(Qt::StrongFocus);
    kfStrip->setMouseTracking(true);
    kfStrip->setToolTip(tr("Keyframes do clipe — clique seleciona, arraste move no tempo, "
                           "Delete apaga. Ctrl=ignora o playhead, Shift=passo de 1 frame."));
    lay->addWidget(kfStrip);
    kfStrip->installEventFilter(this);
    m_kfStrip = kfStrip;

    m_info = new QLabel(this);
    m_info->setStyleSheet(QStringLiteral("color:#888;"));
    m_info->setText(tr("Selecione um clipe. Arraste=girar · Mover/H/V/Z=posição "
                         "· setas=nudge (Shift=10px) · Ctrl=escala/snap"));
    lay->addWidget(m_info);

    // Teclas do canvas: R gira (ciclo Z→X→Y), Esc limpa ferramenta.
    connect(m_canvas, &PivotCanvas::editStart, this, &PivotWidget::editStart);
    connect(m_canvas, &PivotCanvas::changed, this, [this]() {
        refreshUi();
        emit modified();
    });
    connect(m_canvas, &PivotCanvas::statusMessage, this, [this](const QString& s) {
        if (m_info) m_info->setText(s);
    });
    connect(m_addKf, &QToolButton::clicked, this, &PivotWidget::addKeyAtPlayhead);
    connect(m_delKf, &QToolButton::clicked, this, &PivotWidget::delKeyNearPlayhead);
    connect(m_resetBtn, &QToolButton::clicked, this, &PivotWidget::resetTransform);

    // Liga botões de eixo ao canvas via property. Marcar um botão de MOVER
    // desmarca os de ROTACAO (e vice-versa): são modos exclusivos, e deixar
    // os dois armed ao mesmo tempo deixa o arraste ambíguo.
    QList<QToolButton*> rotBtns = {rotXBtn, rotYBtn, rotZBtn};
    QList<QToolButton*> movBtns = {mvFree, mvH, mvV, mvZ};

    auto armRot = [this, movBtns, rotBtns](QToolButton* self, int mode) {
        for (QToolButton* b : rotBtns) if (b != self) b->setChecked(false);
        for (QToolButton* b : movBtns) b->setChecked(false);
        m_canvas->setProperty("pivotMoveMode", 0);
        m_canvas->setProperty("pivotRotMode", mode);
        m_canvas->update();
    };
    connect(rotXBtn, &QToolButton::clicked, this, [armRot, rotXBtn]() {
        armRot(rotXBtn, 1);
    });
    connect(rotYBtn, &QToolButton::clicked, this, [armRot, rotYBtn]() {
        armRot(rotYBtn, 2);
    });
    connect(rotZBtn, &QToolButton::clicked, this, [armRot, rotZBtn]() {
        armRot(rotZBtn, 3);
    });

    auto armMove = [this, rotBtns, movBtns](QToolButton* self, int mode) {
        for (QToolButton* b : movBtns) if (b != self) b->setChecked(false);
        for (QToolButton* b : rotBtns) b->setChecked(false);
        m_canvas->setProperty("pivotRotMode", 0);
        m_canvas->setProperty("pivotMoveMode", mode);
        m_canvas->update();
    };
    connect(mvFree, &QToolButton::clicked, this, [armMove, mvFree]() {
        armMove(mvFree, 1);
    });
    connect(mvH, &QToolButton::clicked, this, [armMove, mvH]() {
        armMove(mvH, 2);
    });
    connect(mvV, &QToolButton::clicked, this, [armMove, mvV]() {
        armMove(mvV, 3);
    });
    connect(mvZ, &QToolButton::clicked, this, [armMove, mvZ]() {
        armMove(mvZ, 4);
    });
}

// ── Keyframes: canais e geometria do strip ────────────────────────────────
// Largura reservada à coluna de rótulos à esquerda do strip.
static constexpr double kKfLabelW = 26.0;

bool PivotWidget::eventFilter(QObject* obj, QEvent* ev) {
    auto* w = qobject_cast<QWidget*>(obj);
    const bool onStrip = w && w->objectName() == QLatin1String("pivotKfStrip");
    if (!onStrip) return QWidget::eventFilter(obj, ev);

    Clip* c = activeClip();
    const double dur = (c && c->dur > 0) ? c->dur : 0.0;

    // ── Mouse: selecionar e arrastar no tempo ────────────────────────────
    if (ev->type() == QEvent::MouseButtonPress && c) {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() == Qt::LeftButton) {
            int idx = -1;
            const int ch = kfChannelAt(w, me->pos().toPoint(), &idx);
            w->setFocus(Qt::MouseFocusReason);
            if (ch == ChNone || idx < 0) {
                m_selChannel = ChNone;
                m_selIndex = -1;
                m_kfDragging = false;
                m_info->setText(tr("Seleção de keyframe limpa."));
            } else {
                m_selChannel = ch;
                m_selIndex = idx;
                m_kfDragging = true;
                m_kfUndoArmed = false;
                m_kfDragTime0 = (*kfChannel(c, ch))[idx].time;
                m_info->setText(tr("%1 · %2 s — arraste para mover no tempo")
                                    .arg(kfChannelLabel(ch))
                                    .arg(m_kfDragTime0, 0, 'f', 2));
            }
            kfStripRepaint();
            me->accept();
            return true;
        }
    }

    if (ev->type() == QEvent::MouseMove && c && m_kfDragging
        && m_selChannel != ChNone && m_selIndex >= 0) {
        auto* me = static_cast<QMouseEvent*>(ev);
        QVector<Keyframe>* ks = kfChannel(c, m_selChannel);
        if (!ks || m_selIndex >= ks->size()) { m_kfDragging = false; return true; }
        const double tNew = kfStripTimeAt(w, me->pos().x(), dur);
        if (!qFuzzyIsNull(tNew - (*ks)[m_selIndex].time)) {
            // Um undo por gesto: só dispara na primeira vez que o tempo muda,
            // senão clicar sem arrastar sujaria a pilha.
            if (!m_kfUndoArmed) { emit editStart(); m_kfUndoArmed = true; }
            // O vetor precisa continuar ordenado por tempo (kfValue e
            // upsertKeyframe assumem isso), então tira e reinsere em vez de
            // sobrescrever o tempo no lugar.
            Keyframe k = (*ks)[m_selIndex];
            ks->removeAt(m_selIndex);
            k.time = tNew;
            auto it = std::lower_bound(ks->begin(), ks->end(), tNew,
                [](const Keyframe& kf, double t) { return kf.time < t; });
            const int newIdx = int(it - ks->begin());
            ks->insert(it, k);
            m_selIndex = newIdx;
            m_info->setText(tr("%1 · %2 s — arraste para mover no tempo")
                                .arg(kfChannelLabel(m_selChannel))
                                .arg(tNew, 0, 'f', 2));
        }
        kfStripRepaint();
        if (m_canvas) m_canvas->update();
        me->accept();
        return true;
    }

    if (ev->type() == QEvent::MouseButtonRelease && c && m_kfDragging) {
        m_kfDragging = false;
        if (m_kfUndoArmed) emit modified();
        kfStripRepaint();
        return true;
    }

    // Duplo clique apaga — atalho de mão para o botão −Kf.
    if (ev->type() == QEvent::MouseButtonDblClick && c) {
        auto* me = static_cast<QMouseEvent*>(ev);
        int idx = -1;
        const int ch = kfChannelAt(w, me->pos().toPoint(), &idx);
        if (ch != ChNone && idx >= 0) {
            m_selChannel = ch;
            m_selIndex = idx;
            deleteSelectedKey();
            me->accept();
            return true;
        }
    }

    // ── Teclado: apagar e navegar passo a passo ──────────────────────────
    if (ev->type() == QEvent::KeyPress && c) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) {
            if (m_selChannel != ChNone && m_selIndex >= 0) deleteSelectedKey();
            else m_info->setText(tr("Selecione um keyframe para apagar."));
            ke->accept();
            return true;
        }
        // ←/→ movem o keyframe selecionado em passos de 1 frame.
        if ((ke->key() == Qt::Key_Left || ke->key() == Qt::Key_Right)
            && c->dur > 0 && m_selChannel != ChNone && m_selIndex >= 0) {
            if (QVector<Keyframe>* ks = kfChannel(c, m_selChannel)) {
                if (m_selIndex < ks->size()) {
                    const double fps = (m_project && m_project->fps > 0)
                                           ? m_project->fps : 30.0;
                    const double step = (ke->key() == Qt::Key_Right ? 1.0 : -1.0) / fps;
                    const double tNew =
                        std::clamp((*ks)[m_selIndex].time + step, 0.0, c->dur);
                    if (!qFuzzyIsNull(tNew - (*ks)[m_selIndex].time)) {
                        emit editStart();
                        Keyframe k = (*ks)[m_selIndex];
                        ks->removeAt(m_selIndex);
                        k.time = tNew;
                        auto it = std::lower_bound(ks->begin(), ks->end(), tNew,
                            [](const Keyframe& kf, double t) { return kf.time < t; });
                        m_selIndex = int(it - ks->begin());
                        ks->insert(it, k);
                        emit modified();
                        refreshUi();
                    }
                    ke->accept();
                    return true;
                }
            }
        }
    }

    // ── Paint ─────────────────────────────────────────────────────────────
    if (ev->type() == QEvent::Paint) {
        QPainter p(w);
        const auto& tc = themeColors();
        p.fillRect(w->rect(), tc.base);
        if (!c || c->dur <= 0 || !m_project) {
            p.setPen(tc.monitorLabel);
            p.drawText(w->rect(), Qt::AlignCenter, tr("keyframes do clipe"));
            return true;
        }
        for (int ch = 0; ch < ChCount; ++ch) {
            const QVector<Keyframe>* ks = kfChannel(c, ch);
            if (!ks) continue;
            const bool rowSel = (m_selChannel == ch);
            const double rowY = kfStripPos(w, ch, 0.0, c->dur).y();
            if (rowSel)  // faixa da linha selecionada: acha o canal num olhar
                p.fillRect(0, int(rowY) - 6, w->width(), 12, QColor(255, 255, 255, 18));
            p.setPen(QPen(tc.monitorLabel, 1));
            p.drawText(QRect(2, int(rowY) - 6, int(kKfLabelW), 12),
                       Qt::AlignLeft | Qt::AlignVCenter, kfChannelLabel(ch));
            for (int i = 0; i < ks->size(); ++i) {
                const QPointF pos = kfStripPos(w, ch, (*ks)[i].time, c->dur);
                const bool sel = rowSel && i == m_selIndex;
                p.setPen(Qt::NoPen);
                p.setBrush(kfChannelColor(ch));
                p.drawEllipse(pos, sel ? 5.0 : 3.0, sel ? 5.0 : 3.0);
                if (sel) {
                    p.setPen(QPen(Qt::white, 1.5));
                    p.setBrush(Qt::NoBrush);
                    p.drawEllipse(pos, 7.0, 7.0);
                }
            }
        }
        const double phX = kfStripPos(w, 0, std::clamp(m_playhead - c->pos, 0.0, c->dur),
                                      c->dur).x();
        p.setPen(QPen(tc.playhead, 2));
        p.drawLine(int(phX), 0, int(phX), w->height());
        return true;
    }
    return QWidget::eventFilter(obj, ev);
}

QVector<Keyframe>* PivotWidget::kfChannel(Clip* c, int ch) const {
    if (!c) return nullptr;
    switch (ch) {
    case ChPosX:  return &c->kfTx;
    case ChPosY:  return &c->kfTy;
    case ChScale: return &c->kfScale;
    case ChRot:   return &c->kfRotation;
    case ChZ:     return &c->kfClipZ;
    case ChRotX:  return &c->kfClipRotX;
    case ChRotY:  return &c->kfClipRotY;
    default: return nullptr;
    }
}

QColor PivotWidget::kfChannelColor(int ch) {
    switch (ch) {
    case ChPosX: case ChPosY: return QColor(120, 200, 255);
    case ChScale: return QColor(120, 220, 140);
    case ChRot:   return QColor(220, 160, 80);
    case ChZ:     return QColor(160, 140, 255);
    case ChRotX:  return QColor(220, 90, 90);
    case ChRotY:  return QColor(90, 200, 100);
    default: return QColor(180);
    }
}

QString PivotWidget::kfChannelLabel(int ch) const {
    switch (ch) {
    case ChPosX: return QStringLiteral("X");
    case ChPosY: return QStringLiteral("Y");
    case ChScale: return QStringLiteral("ESC");
    case ChRot: return QStringLiteral("RZ");
    case ChZ: return QStringLiteral("Z");
    case ChRotX: return QStringLiteral("RX");
    case ChRotY: return QStringLiteral("RY");
    default: return QString();
    }
}

double PivotWidget::kfStripTimeAt(QWidget* strip, double x, double dur) const {
    if (!strip || dur <= 0) return 0.0;
    const double usable = qMax(1.0, strip->width() - 8.0 - kKfLabelW);
    return std::clamp((x - 4.0 - kKfLabelW) / usable * dur, 0.0, dur);
}

QPointF PivotWidget::kfStripPos(QWidget* strip, int ch, double time, double dur) const {
    const double usable = qMax(1.0, strip->width() - 8.0 - kKfLabelW);
    const double rowH = qMax(1.0, strip->height() - 8.0) / ChCount;
    return QPointF(4.0 + kKfLabelW + (dur > 0 ? time / dur * usable : 0.0),
                   4.0 + rowH * (ch + 0.5));
}

int PivotWidget::kfChannelAt(QWidget* strip, QPoint pos, int* indexOut) const {
    if (indexOut) *indexOut = -1;
    Clip* c = activeClip();
    if (!strip || !c || c->dur <= 0) return ChNone;
    // Tolerância de 8 px em X e meia-linha em Y: 7 canais numa faixa de 66 px
    // dá ~9 px de altura, então exigir precisão de pixel é injogável.
    const double tolY = qMax(5.0, (strip->height() - 8.0) / ChCount * 0.5);
    for (int ch = 0; ch < ChCount; ++ch) {
        QVector<Keyframe>* ks = kfChannel(c, ch);
        if (!ks || ks->isEmpty()) continue;
        const double rowY = kfStripPos(strip, ch, 0.0, c->dur).y();
        if (std::fabs(pos.y() - rowY) > tolY) continue;
        int best = -1;
        double bestD = 1e9;
        for (int i = 0; i < ks->size(); ++i) {
            const double d = std::fabs(kfStripPos(strip, ch, (*ks)[i].time, c->dur).x()
                                       - pos.x());
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best >= 0 && bestD <= 8.0) {
            if (indexOut) *indexOut = best;
            return ch;
        }
    }
    return ChNone;
}

void PivotWidget::kfStripRepaint() {
    if (m_kfStrip) m_kfStrip->update();
}

void PivotWidget::deleteSelectedKey() {
    Clip* c = activeClip();
    if (!c || m_selChannel == ChNone) return;
    QVector<Keyframe>* ks = kfChannel(c, m_selChannel);
    if (!ks || m_selIndex < 0 || m_selIndex >= ks->size()) return;
    emit editStart();
    ks->removeAt(m_selIndex);
    m_selIndex = -1;
    m_selChannel = ChNone;
    emit modified();
    refreshUi();
}

Clip* PivotWidget::activeClip() const {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    return nullptr;
}

void PivotWidget::setClipId(const QString& id) {
    // Trocar de clipe invalida a selecao: o indice guardado pertence ao
    // vetor do clipe anterior e apontaria para o keyframe errado.
    if (m_clipId != id) {
        m_selChannel = ChNone;
        m_selIndex = -1;
        m_kfDragging = false;
    }
    m_clipId = id;
    refreshUi();
}

void PivotWidget::setPlayhead(double t) {
    m_playhead = t;
    if (m_canvas) m_canvas->setPlayhead(t);
    // Redesenha o strip.
    for (QWidget* w : findChildren<QWidget*>())
        if (w->objectName() == QLatin1String("pivotKfStrip")) w->update();
}

void PivotWidget::refreshUi() {
    Clip* c = activeClip();
    const bool on = c != nullptr;
    m_addKf->setEnabled(on);
    m_delKf->setEnabled(on && c && (c->kfTx.size() + c->kfClipZ.size() > 1));
    m_resetBtn->setEnabled(on);
    if (!c && m_selChannel != ChNone) {   // clipe sumiu: selecao orfa
        m_selChannel = ChNone;
        m_selIndex = -1;
    }
    kfStripRepaint();
    if (!c) {
        m_title->setText(tr("Pivot — 3D do clipe"));
        m_info->setText(tr("Selecione um clipe 3D (imagem ou malha)."));
        if (m_canvas) m_canvas->setClip(nullptr, m_project, m_playhead);
        return;
    }
    const MediaItem* m = m_project ? m_project->findMedia(c->mediaId) : nullptr;
    m_title->setText(tr("Pivot — %1").arg(c->name.isEmpty() ? c->id : c->name));
    m_info->setText(m && m->isMesh
                        ? tr("Malha 3D · arraste=girar · Shift=Z · Ctrl=escala/snap · RX/RY/RZ · +kf")
                        : tr("Imagem · arraste=girar · Shift=Z (2.5D) · Ctrl=escala · +kf"));
    if (m_canvas) m_canvas->setClipId(m_project, c->id, m_playhead);
}

void PivotWidget::addKeyAtPlayhead() {
    Clip* c = activeClip();
    if (!c || !m_project) return;
    emit editStart();
    const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    upsertKeyframe(c->kfTx, rel, c->tx, KfLinear);
    upsertKeyframe(c->kfTy, rel, c->ty, KfLinear);
    upsertKeyframe(c->kfScale, rel, c->scale, KfLinear);
    upsertKeyframe(c->kfRotation, rel, c->rotation, KfLinear);
    if (std::fabs(c->clipZ) > 1e-6 || !c->kfClipZ.isEmpty())
        upsertKeyframe(c->kfClipZ, rel, c->clipZ, KfLinear);
    if (std::fabs(c->clipRotX) > 1e-6 || !c->kfClipRotX.isEmpty())
        upsertKeyframe(c->kfClipRotX, rel, c->clipRotX, KfLinear);
    if (std::fabs(c->clipRotY) > 1e-6 || !c->kfClipRotY.isEmpty())
        upsertKeyframe(c->kfClipRotY, rel, c->clipRotY, KfLinear);
    emit modified();
    refreshUi();
}

void PivotWidget::delKeyNearPlayhead() {
    Clip* c = activeClip();
    if (!c) return;
    // Com um keyframe selecionado no strip, o botao apaga ESSE. Sem selecao,
    // mantem o comportamento antigo (o mais proximo do playhead) — que era
    // cego: pegava qualquer canal e nao dizia qual.
    if (m_selChannel != ChNone && m_selIndex >= 0) {
        deleteSelectedKey();
        return;
    }
    const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    auto near = [&](QVector<Keyframe>& ks) {
        for (int i = 0; i < ks.size(); ++i) {
            if (std::fabs(ks[i].time - rel) < 0.08) { ks.removeAt(i); return true; }
        }
        return false;
    };
    emit editStart();
    bool any = false;
    any |= near(c->kfTx);
    any |= near(c->kfTy);
    any |= near(c->kfScale);
    any |= near(c->kfRotation);
    any |= near(c->kfClipZ);
    any |= near(c->kfClipRotX);
    any |= near(c->kfClipRotY);
    if (any) emit modified();
    refreshUi();
}

void PivotWidget::resetTransform() {
    Clip* c = activeClip();
    if (!c) return;
    emit editStart();
    c->tx = 0; c->ty = 0; c->scale = 1.0; c->rotation = 0.0;
    c->clipZ = 0; c->clipRotX = 0; c->clipRotY = 0;
    c->kfTx.clear(); c->kfTy.clear(); c->kfScale.clear(); c->kfRotation.clear();
    c->kfClipZ.clear(); c->kfClipRotX.clear(); c->kfClipRotY.clear();
    emit modified();
    refreshUi();
}
