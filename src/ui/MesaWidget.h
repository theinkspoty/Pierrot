// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QVector>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QSize>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include "models/Project.h"
#include "render/MesaRenderer.h"

// Categoria de log da Mesa: habilita/desabilita com
//   Q_LOGGING_RULES="mesa.widget=true|false"
// e filtra no terminal com `grep "\[MESA\]"`.
Q_DECLARE_LOGGING_CATEGORY(lcMesa)

// Dock Mesa — canvas infinito estilo After Effects Composition Panel.
// Tudo é desenhado no canvas: tracks como camadas, câmera, grid.
// Pan: arrastar o vazio com a mãozinha (quando nada está ativo). Zoom: roda. Seleção: clique esquerdo.
// Transform: arrastar corpo = mover, cantos = escalar, handle acima = rotacionar.
// Doc: câmera (estilo AE): clicar no gizmo só SELECIONA; selecionada, arrastar
// em QUALQUER lugar do canvas (vazio, em cima de camada, no próprio gizmo)
// MOVE a câmera — cantos redimensionam. É o jeito fácil de enquadrar. Para
// voltar à mãozinha de pan, desmarque a câmera com Esc ou clicando na linha
// "Câmera" do painel. Sem a câmera selecionada, o vazio dá pan e o gizmo só
// seleciona. O painel é alternado com a tecla L.
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

    // Bounds da layer (mesma convenção AE do MesaRenderer: a layer vive em
    // espaço local com origem no topo-esquerda, matriz local→comp =
    // T(posição)·R·S·T(-âncora); x/y = posição absoluta da âncora na comp).
    struct LayerBounds {
        double x = 0, y = 0;       // posição da âncora na composição (px absolutos)
        double rotation = 0;
        double w = 100, h = 100;   // tamanho NATURAL (não escalado) da camada
        double sx = 1, sy = 1;     // escala (multiplicador)
        double ax = 0, ay = 0;     // âncora: OFFSET do centro natural (px da layer)
        bool hasContent = false;   // tem clip ativo
    };
    LayerBounds layerBounds(const Track* t, int trackIdx) const;

    // Cache de dimensões de mídia (evita varredura linear de findMedia a cada
    // hover/hit test). Preenchido no refresh(); inválido ao alterar o projeto.
    mutable QHash<QString, QSize> m_mediaSizes;
    void layerMediaSize(const Clip& c, bool& ok, double& w, double& h) const;

    // Layer→screen rects para hit testing
    void layerScreenRect(const LayerBounds& lb, QPointF& center,
                         QPointF corners[4], QPointF& rotateHandle) const;

    void nudgeSelection(double dx, double dy);

// Desenho
    void drawLayerList(QPainter& p);
    void drawMiniTimeline(QPainter& p);
    void drawEyeIcon(QPainter& p, const QRect& r, bool visible) const;
    void drawLockIcon(QPainter& p, const QRect& r, bool locked) const;
    QString blendShortName(const QString& blend) const;
    int miniTimelineHeight() const { return 64; }
    // Painel vertical fixo à esquerda (Câmera no topo + camadas). Quando
    // oculto (tecla L), o canvas volta a ocupar a largura inteira.
    static constexpr int kLayerPanelW = 190;
    int panelWidth() const { return m_showLayerList ? kLayerPanelW : 0; }
    QRect artRect() const {
        const int pw = panelWidth();
        return QRect(pw, 0, qMax(1, width() - pw), qMax(1, height()));
    }
    int artWidth() const { return artRect().width(); }
    QPointF artCenter() const {
        const QRect r = artRect();
        return QPointF(r.x() + r.width() / 2.0, r.y() + r.height() / 2.0);
    }

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

    // Seleção NÃO exclusiva: camada (m_selectedIdx) e câmera (m_cameraSelected)
    // são independentes; Esc limpa as duas.
    int m_selectedIdx = -1;
    bool m_cameraSelected = false;

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
    bool m_scaleUniform = true;        // escala de cantos: uniforme (Shift = livre por eixo)

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
    bool m_showLayerList = true;
    QRect m_layerListRect;
    // Zonas clicáveis de cada linha (olho/cadeado/corpo) preenchidas no draw.
    struct LayerRowZone { QRect eye, lock, body; int idx; };
    QVector<LayerRowZone> m_layerZones;
    // Nº de linhas visíveis no painel (ajustado no draw quando a lista
    // estoura a altura do widget). Linha 0 = Câmera.
    int m_layerListRowCount = 0;
    // Reordenação por arrasto: índice da camada sendo arrastada na lista.
    int m_layerListDragIdx = -1;
    QPoint m_layerListDragStart;

    // Mini-timeline
    bool m_timelineDrag = false;
    QRect m_miniTimelineRect;
    mutable double m_contentStart = 0.0;
    static constexpr double kDefaultPps = 80.0;  // pixels per second

    // Mini-timeline keyframe selection
    // Cada keyframe é identificado por (source, trackId, time, prop) para que
    // a seleção/remoção seja POR PROPRIEDADE (X, Y, escala...) e não apague
    // tudo o que existe no mesmo tempo.
    enum PropId { PCamX, PCamY, PCamZ, PCamR,
                  PLayX, PLayY, PLaySX, PLaySY, PLayRot, PLayOp,
                  PLayAX, PLayAY };
    struct KfRef {
        enum Source { Cam, MesaTrack } source;
        QString trackId;       // empty for Cam
        double time = 0.0;
        int prop = 0;          // PropId
        bool operator==(const KfRef& o) const {
            return source == o.source && trackId == o.trackId
                   && qFuzzyCompare(time, o.time) && prop == o.prop;
        }
    };
    friend uint qHash(const KfRef& r);
    friend uint qHash(const KfRef& r, size_t seed);
    QSet<KfRef> m_selectedKfs;
    KfRef hitTestKf(const QPoint& pos) const;
    bool isKfSelected(const KfRef& r) const;

    // Arrasto horizontal e marquee de keyframes na mini-timeline.
    QVector<Keyframe>* keyframesFor(KfRef::Source src, const QString& trackId, int prop);
    void sortKfs(QVector<Keyframe>& vks) const;
    double timelinePps(int rulerW) const;

    bool m_kfDrag = false;              // arrastando os keyframes selecionados
    int m_kfDragStartX = 0;
    struct KfOrigin { KfRef ref; double time; };
    QVector<KfOrigin> m_kfDragOrigins;  // tempo ORIGINAL de cada selecionado
    bool m_timelinePressPending = false;  // clique no vazio: playhead ou marquee
    int m_timelinePressX = 0;
    bool m_timelineMarquee = false;     // retângulo de seleção múltipla
    QPoint m_marqueeStartPos;
    int m_marqueeCurX = 0;

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
