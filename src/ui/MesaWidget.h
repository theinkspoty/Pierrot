// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QVector>
#include <QTimer>
#include <QHash>
#include <QSize>
#include <QPointer>
#include <QElapsedTimer>
#include "models/Project.h"
#include "render/MesaRenderer.h"

class QLineEdit;

// Dock Mesa — canvas infinito estilo After Effects Composition Panel.
// Tudo é desenhado no canvas: tracks como camadas, câmera, grid.
// Pan: botão do meio. Zoom: roda do mouse. Seleção: clique esquerdo.
// Transform: arrastar corpo = mover, cantos = escalar, handle acima = rotacionar.
class MesaWidget : public QWidget {
    Q_OBJECT
public:
    explicit MesaWidget(QWidget* parent = nullptr);

    void setProject(Project* p) { m_project = p; }
    Project* project() const { return m_project; }
    void setMesaId(const QString& id);
    void setPlayheadPosition(double timeSec) {
        if (!qFuzzyCompare(m_playheadTime, timeSec)) {
            m_playheadTime = timeSec;
            update();
        }
    }
    double playheadPosition() const { return m_playheadTime; }
    QString mesaId() const { return m_mesaId; }
    void refresh();
    // Seleciona a primeira Mesa disponível no projeto (usado ao abrir/undo).
    void autoSelectMesa();
    bool hasSelectedKeyframes() const { return !m_selectedKfs.isEmpty(); }
    void deleteSelectedKfs();

signals:
    void modified();
    void changesCommitted();
    void mesaCreateRequested();
    void mesaPlayheadChanged(double t);
    void mesaTrackSelected(Track* track);
    void mesaCameraSelected(MesaComposition* mc);
    void mesaAddTrackRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    MesaComposition* currentMesa() const;
    Track* findTrack(const QString& trackId) const;
    QVector<Track*> mesaTracks() const;

    // Coordenadas
    QPointF canvasToScreen(const QPointF& p) const;
    QPointF screenToCanvas(const QPointF& p) const;

    // Hit testing
    enum HitZone { HitNone, HitBody, HitCornerTL, HitCornerTR, HitCornerBL, HitCornerBR,
                   HitEdgeT, HitEdgeB, HitEdgeL, HitEdgeR, HitRotate,
                   HitCamera, HitCameraCorner };
    HitZone hitTest(const QPointF& screenPos, int& outTrackIdx) const;
    int trackAt(const QPointF& screenPos) const;
    int cameraCornerAt(const QPointF& screenPos) const;
    void cameraCornerPoints(QPointF out[4]) const;

    // Bounds da layer (calcula retângulo no canvas-space)
    struct LayerBounds {
        double x = 0, y = 0;       // centro no canvas
        double w = 100, h = 100;   // tamanho
        double rotation = 0;
        double anchorX = 0, anchorY = 0;
        bool hasContent = false;    // tem clip ativo
    };
    LayerBounds layerBounds(const Track* t, int trackIdx) const;

    // Cache de dimensões de mídia (evita varredura linear de findMedia a cada
    // hover/hit test). Preenchido no refresh(); inválido ao alterar o projeto.
    mutable QHash<QString, QSize> m_mediaSizes;
    void layerMediaSize(const Clip& c, bool& ok, double& w, double& h) const;

    // Layer→screen rects para hit testing
    void layerScreenRect(const LayerBounds& lb, QPointF& center,
                         QPointF corners[4], QPointF& rotateHandle) const;

    // Keyframes
    void ensureKeyframesAt(double timeSec);      // câmera no tempo dado
    void writeTrackKeyframes(Track* t);          // só a track dada (auto-key)
    bool m_autoKey = true;                       // toggle estilo AE (botão/K)
    QRect m_autoKeyBtnRect;
    void nudgeSelection(double dx, double dy);

    // Desenho
    void drawLayerList(QPainter& p);
    void drawPropertyPanel(QPainter& p);
    void drawMiniTimeline(QPainter& p);
    int propPanelHeight() const { return 36; }
    int miniTimelineHeight() const { return 64; }

    // Campos editáveis do painel de propriedades (clique → digita o valor).
    enum PropKind { PL_X, PL_Y, PL_S, PL_R, PL_O, PC_X, PC_Y, PC_Z, PC_R };
    struct PropField { QRect rect; int kind; };
    QVector<PropField> m_propFields;   // preenchido no drawPropertyPanel
    QPointer<QLineEdit> m_propEdit;
    double propFieldValue(int kind) const;
    void startPropEdit(const PropField& f);
    void commitPropEdit(int kind, const QString& text);
    void fitToContent();
    void throttledUpdate();  // máx ~60fps durante drag

    // Mini-timeline helpers
    double mesaDuration() const;
    double contentStartTime() const;
    int timeToX(double t, int rulerW) const;
    double xToTime(int x, int rulerW) const;
    bool isInMiniTimeline(const QPoint& p) const;

    // Cache de tracks (evita alloc por frame)
    mutable QVector<Track*> m_cachedTracks;
    mutable QString m_cachedMesaId;
    mutable qint64 m_cachedTracksVersion = 0;

    // Throttle de updates durante drag (máx ~60fps)
    QElapsedTimer m_lastUpdateTimer;

    Project* m_project = nullptr;
    QString m_mesaId;
    double m_playheadTime = 0.0;

    // Viewport
    double m_zoom = 1.0;
    QPointF m_offset = {0, 0};

    // Seleção
    int m_selectedIdx = -1;

    // Transform (move/scale/rotate)
    enum TransformOp { TNone, TMove, TScale, TRotate };
    TransformOp m_transformOp = TNone;
    HitZone m_transformZone = HitNone;
    int m_transformTrackIdx = -1;
    QPointF m_transformStart;       // posição do mouse no início
    double m_transformStartX = 0;   // valor inicial da propriedade
    double m_transformStartY = 0;
    double m_transformStartSX = 1;
    double m_transformStartSY = 1;
    double m_transformStartRot = 0;
    double m_transformStartAngle = 0;  // ângulo inicial (para rotate)
    double m_transformStartDist = 0;   // distância inicial (para scale uniforme)
    bool m_scaleUniform = false;       // Shift mantido

    // Dragging camera
    bool m_draggingCamera = false;
    QPointF m_cameraDragStart;
    double m_camDragStartX = 0.0;
    double m_camDragStartY = 0.0;

    // Resizing camera
    bool m_resizingCamera = false;
    int m_resizeCorner = -1;
    double m_resizeStartZoom = 1.0;
    QPointF m_resizeStartPos;

    // Canvas pan
    bool m_draggingCanvas = false;
    QPointF m_canvasDragStart;

    // Layer list flutuante
    bool m_showLayerList = false;
    QRect m_layerListRect;

    // Mini-timeline
    bool m_timelineDrag = false;
    QRect m_miniTimelineRect;
    mutable double m_contentStart = 0.0;
    static constexpr double kDefaultPps = 80.0;  // pixels per second

    // Mini-timeline keyframe selection
    struct KfRef {
        enum Source { Cam, MesaTrack } source;
        QString trackId;       // empty for Cam
        double time = 0.0;
        bool operator==(const KfRef& o) const {
            return source == o.source && trackId == o.trackId
                   && qFuzzyCompare(time, o.time);
        }
    };
    friend uint qHash(const KfRef& r);
    QSet<int> m_selectedKfs;  // hash of KfRef
    KfRef hitTestKf(const QPoint& pos) const;
    bool isKfSelected(const KfRef& r) const;
    void toggleKfSelection(const KfRef& r, bool ctrl);

    // Snap
    bool m_snapToGrid = false;
    static constexpr double kGridSize = 50.0;

    // Renderer (decodifica e compõe frames reais)
    MesaRenderer m_renderer;

    // Drag: id da camada sendo arrastada (skip no render principal)
    QString m_dragTrackId;
    int m_dragTrackIndex = -1;

    // Botão "Criar Mesa" (estado vazio)
    QRect m_createMesaBtnRect;
};
