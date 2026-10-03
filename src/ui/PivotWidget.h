// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Pivot — dock simples de edição 3D do clipe (imagens ou malhas).
// Mais enxuto que a Mesa: gizmo estilo Blender + keyframes embaixo.
// Edita transform do CLIP (tx/ty/scale/rot + clipZ/rotX/rotY).

#pragma once

#include <QWidget>
#include <QImage>
#include <QVector>
#include "colombina/models/Project.h"
#include "colombina/mesh/ObjLoader.h"

class QLabel;
class QSlider;
class QToolButton;
class QTimer;

// Canvas do Pivot: pré-visualiza o clipe + gizmo 3D (X/Y/Z).
class PivotCanvas : public QWidget {
    Q_OBJECT
public:
    explicit PivotCanvas(QWidget* parent = nullptr);

    void setClip(Clip* clip, Project* project, double playhead);
    void setClipId(Project* project, const QString& clipId, double playhead);
    void setPlayhead(double t);
    void commitChange();
    // Decodifica o frame da mídia (fora do paint).
    void ensureFrame();

signals:
    void changed();
    void editStart();
    void statusMessage(const QString& msg);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    enum DragMode { DragNone, DragMove, DragScale, DragRotX, DragRotY, DragRotZ, DragZ, DragTrackball };
    // Resolve o clipe a cada uso (não guarda Clip* cru — evita SIGSEGV ao apagar).
    Clip* resolveClip() const;
    // Área de desenho dentro do canvas. Fonte ÚNICA do retângulo: paint,
    // hit-test e arraste precisam concordar, senão o gizmo clicável não é o
    // gizmo desenhado.
    QRect plotRect() const;
    // Origem 3D (centro do plot + pan do clipe). Mesma razão: o gizmo nasce
    // aqui, então todo mundo precisa derivar daqui.
    QPointF gizmoOrigin(double tx, double ty) const;
    // As 3 circunferências-great-circle do gizmo trackball (X, Y, Z), já
    // projetadas em tela. Usadas no paint E no hit-test, para que o anel
    // desenhado e o anel clicável sejam exatamente o mesmo.
    QVector<QVector<QPointF>> gizmoRings(const QPointF& origin, double rot,
                                         double rotX, double rotY, double sc,
                                         double z) const;
    // Anel mais próximo do ponto (1=X, 2=Y, 3=Z, 0=nenhum) dentro de `tolPx`.
    int ringAt(const QPointF& origin, const QPoint& pos, double tolPx) const;
    // Estado 3D corrente do clipe no playhead, para o gizmo.
    void currentTransform(double& rot, double& rotX, double& rotY,
                          double& sc, double& z, double& tx, double& ty) const;

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;
    QImage m_frame; // quadro de mídia (se houver)
    mesh::ObjMesh m_mesh;
    bool m_hasMesh = false;
    DragMode m_drag = DragNone;
    QPoint m_dragStart;
    double m_origTx = 0, m_origTy = 0, m_origScale = 1;
    double m_origRot = 0, m_origRotX = 0, m_origRotY = 0, m_origZ = 0;
    double m_valLo = 0, m_valHi = 2;
    // Ângulo no momento do clique, para o arraste de circunferência gerar
    // delta (e não absoluto) — é o que faz o eixo acompanhar o cursor.
    double m_ringAngle0 = 0;
    // Eixo do mover armado no clique: 0 = livre, 1 = H, 2 = V, 3 = Z.
    int m_moveAxis = 0;
};

// Dock Pivot: canvas + controles + mini-timeline de keyframes.
class PivotWidget : public QWidget {
    Q_OBJECT
public:
    explicit PivotWidget(QWidget* parent = nullptr);
    void setProject(Project* p) { m_project = p; }

public slots:
    void setClipId(const QString& id);
    void setPlayhead(double t);

signals:
    void editStart();
    void modified();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    Clip* activeClip() const;
    void refreshUi();
    void addKeyAtPlayhead();
    void delKeyNearPlayhead();
    void resetTransform();

    // Canais do transform expostos no mini-timeline de keyframes.
    enum Channel {
        ChNone = -1, ChPosX = 0, ChPosY, ChScale, ChRot, ChZ, ChRotX, ChRotY,
        ChCount
    };
    QVector<Keyframe>* kfChannel(Clip* c, int ch) const;
    static QColor kfChannelColor(int ch);
    QString kfChannelLabel(int ch) const;
    // Converte x do strip em tempo relativo do clipe.
    double kfStripTimeAt(QWidget* strip, double x, double dur) const;
    // Row/y e x de um keyframe — usado no paint e no hit-test, para os dois
    // nunca divergirem (aqui já morou um gizmo que nascia longe do objeto).
    QPointF kfStripPos(QWidget* strip, int ch, double time, double dur) const;
    // Devolve o canal cujo keyframe foi clicado, ou ChNone.
    int kfChannelAt(QWidget* strip, QPoint pos, int* indexOut) const;
    void kfStripRepaint();
    void deleteSelectedKey();

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;

    // Keyframe selecionado no strip. Índice -1 = nada selecionado.
    int m_selChannel = ChNone;
    int m_selIndex = -1;
    bool m_kfDragging = false;
    bool m_kfUndoArmed = false;    // editStart só no primeiro movimento real
    double m_kfDragTime0 = 0.0;   // tempo do keyframe no início do arraste

    QLabel* m_title = nullptr;
    PivotCanvas* m_canvas = nullptr;
    QLabel* m_info = nullptr;
    QToolButton* m_addKf = nullptr;
    QToolButton* m_delKf = nullptr;
    QToolButton* m_resetBtn = nullptr;
    QWidget* m_kfStrip = nullptr;
};
