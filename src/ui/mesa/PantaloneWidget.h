// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QImage>
#include <QPoint>
#include <QHash>
#include <QShowEvent>
#include "colombina/models/Project.h"
#include "laartman/core/FFmpegDecoder.h"

class QSlider;
class QLabel;
class QPushButton;
class QToolButton;
class QListWidget;
class QCheckBox;
class QComboBox;
class QPainter;

// Painel docável de pan/crop estilo DaVinci/Vegas: viewfinder com a caixa de
// recorte/zoom arrastável e sliders de crop/escala/pan.
// O viewfinder fica ao lado (à esquerda) dos controles de transformação, para
// a tela não ficar atrás/embaixo dos sliders. Os keyframes seguem o padrão do
// DaVinci Resolve: um losango por parâmetro alterna keyframe no playhead,
// botões ◀/▶ navegam e o auto-keyframe cria keyframe ao mexer num slider.
class PantaloneWidget : public QWidget {
    Q_OBJECT
public:
    // Propriedades de transformação do pantalone (mesmas usadas nos sliders).
    enum Prop {
        P_CropL = 0, P_CropR, P_CropT, P_CropB, P_Scale, P_PanX, P_PanY,
        P_Rotation, P_ScaleX, P_ScaleY, P_AnchorX, P_AnchorY, P_Opacity,
        // Propriedades da máscara ATIVA (valem enquanto houver m_maskSel).
        P_MaskCx, P_MaskCy, P_MaskRx, P_MaskRy, P_MaskRot, P_MaskFeather
    };

    // Modos do viewer: cada um mostra só as próprias alças (acaba o
    // emaranhado de gizmos sobrepostos). Tudo se manipula no viewer.
    enum ViewMode { ModeCrop, ModeTransform, ModeMask, ModeMesh };

    explicit PantaloneWidget(QWidget* parent = nullptr);

    void setProject(Project* p);
    void setClipId(const QString& id);
    void setPlayhead(double t);
    QString clipId() const { return m_clipId; }
    // Máscara selecionada (para o editor de curvas), ou nullptr.
    Mask* currentMask();
    // Índice da máscara selecionada (-1 = nenhuma). O editor de curvas usa o
    // índice (não o ponteiro) para não depender de Clip::masks.
    int currentMaskIndex() const { return m_maskSel; }
public slots:
    void refresh();
    void sync();
signals:
    void editStart();
    void modified();
    void keyframeJump(double t);
    // Informa qual propriedade de transformação foi editada/criou keyframe,
    // para o editor de curvas exibir a curva correspondente.
    void propertyEdited(int prop);
protected:
    // Chamado quando o dock é exibido: o frame não é decodificado enquanto o
    // painel está escondido (ver loadFrame), então é aqui que ele é buscado.
    void showEvent(QShowEvent* e) override;
    // O viewfinder é um QWidget aninhado (Viewport); o PantaloneWidget apenas
    // monta o layout lado a lado (viewfinder | controles) e delega o desenho
    // e os eventos de mouse/roda do viewfinder.
private:
    class Viewport;
    class KeyframeStrip;
    Viewport* m_view = nullptr;
    KeyframeStrip* m_strip = nullptr;

    void paintViewfinder(QWidget* view);
    void viewportPress(QWidget* view, QMouseEvent* e);
    void viewportMove(QWidget* view, QMouseEvent* e);
    void viewportRelease(QWidget* view, QMouseEvent* e);
    void viewportDoubleClick(QWidget* view, QMouseEvent* e);
    void viewportWheel(QWidget* view, QWheelEvent* e);
    void viewportContextMenu(QWidget* view, QContextMenuEvent* e);

    void paintKeyframeStrip(QWidget* view);
    void stripPress(QWidget* view, QMouseEvent* e);
    void stripMove(QWidget* view, QMouseEvent* e);
    void stripRelease(QWidget* view, QMouseEvent* e);
    void stripDoubleClick(QWidget* view, QMouseEvent* e);
    void stripContextMenu(QWidget* view, QContextMenuEvent* e);
    void deleteSelectedKeyframes();

    Clip* activeClip();
    void syncFromClip();
    void loadFrame();
    void emitChange();
    void commitSlider(int prop, double baseValue);
    void updateValueLabels();
    void setCropValues(double L, double R, double T, double B);

    // Keyframes (estilo DaVinci).
    double relPlayhead();
    QVector<Keyframe>* keyframesFor(int prop);
    // Props keyframáveis agora: as 12 do clipe + as 6 da máscara ativa.
    QVector<int> activeProps();
    double propValue(int prop);
    void toggleKeyframe(int prop);
    void gotoKeyframe(int dir);
    void writeKeyframe(int prop, double value);
    void refreshDiamonds();

    void computeView(double s, double tx, double ty, int w0, int h0,
                     QRectF* cropS, QRectF* outS) const;
    // Área (em px do widget) onde o frame é desenhado, modelo "contain".
    QRectF computeDisp(const QRect& viewRect) const;
    // Aplica zoom mantendo o centro da janela de saída fixo no source
    // (usa o mesmo modelo de pan do zoom com a roda do mouse).
    void applyZoomTo(double sNew, const QPointF& centerSrc);
    void screenToSource(const QRect& viewRect, const QPoint& sp,
                        double* sx, double* sy) const;
    // Rotaciona um ponto da tela de volta para o espaço não-rotacionado do
    // viewfinder (para hit-test e arraste funcionarem com a imagem girada).
    QPointF rotatedViewPos(const QRect& viewRect, const QPoint& sp) const;
    void applyPan(double sx, double sy);

    // ── Máscaras (rect/ellipse) — gizmos no viewfinder ────────────────
    // A máscara é normalizada 0..1 ao quadro (m_frame), igual ao modelo Mask e
    // ao overlay do preview; aqui ela é desenhada/arrastada no disp do Pantalone.
    Mask* activeMask();
    void refreshMaskList();
    void syncMaskControls();
    void applyMaskEdit();
    void addMask(const QString& type);
    void removeSelectedMask();
    bool pickMaskHandle(QWidget* view, const QPoint& pos);
    void applyMaskDrag(QWidget* view, const QPoint& pos);
    void drawMaskGizmos(QPainter& p, const QRectF& disp, int w0, int h0);
    void drawMeshGizmo(QPainter& p, const QRectF& disp, int w0, int h0);

    enum DragMode { DragNone, DragPan, DragAnchor, DragRotate,
                    DragZoomTL, DragZoomTR, DragZoomBL, DragZoomBR,
                    DragCropL, DragCropR, DragCropT, DragCropB,
                    DragCropTL, DragCropTR, DragCropBL, DragCropBR };

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;
    bool m_undoPushed = false;

    FFmpegDecoder m_decoder;
    QImage m_frame;
    QString m_framePath;
    QString m_lastDecodedClipId;  // para forçar reset do decoder ao trocar de clipe

    QSlider* m_cropL = nullptr;
    QSlider* m_cropR = nullptr;
    QSlider* m_cropT = nullptr;
    QSlider* m_cropB = nullptr;
    QSlider* m_scale = nullptr;
    QSlider* m_panX = nullptr;
    QSlider* m_panY = nullptr;
    QSlider* m_rotation = nullptr;
    QSlider* m_scaleX = nullptr;
    QSlider* m_scaleY = nullptr;
    QSlider* m_anchorX = nullptr;
    QSlider* m_anchorY = nullptr;
    QSlider* m_opacity = nullptr; // 0..100 (%) — Clip::opacity/kfOpacity

    QLabel* m_cropLVal = nullptr;
    QLabel* m_cropRVal = nullptr;
    QLabel* m_cropTVal = nullptr;
    QLabel* m_cropBVal = nullptr;
    QLabel* m_scaleVal = nullptr;
    QLabel* m_panXVal = nullptr;
    QLabel* m_panYVal = nullptr;
    QLabel* m_rotationVal = nullptr;
    QLabel* m_scaleXVal = nullptr;
    QLabel* m_scaleYVal = nullptr;
    QLabel* m_anchorXVal = nullptr;
    QLabel* m_anchorYVal = nullptr;
    QLabel* m_opacityVal = nullptr;

    QHash<int, QPushButton*> m_kfDiamonds;
    QHash<int, QToolButton*> m_resetBtns;
    QPushButton* m_kfAuto = nullptr;
    bool m_showGrid = true;

    // ── Máscaras ──────────────────────────────────────────────────────
    QListWidget* m_maskList = nullptr;
    QCheckBox* m_maskEnabled = nullptr;
    QCheckBox* m_maskInvert = nullptr;
    QSlider* m_maskFeather = nullptr;
    QLabel* m_maskFeatherVal = nullptr;
    QToolButton* m_maskDelBtn = nullptr;
    int m_maskSel = -1;        // índice da máscara ativa (-1 = nenhuma)
    int m_maskDragHandle = -1; // alça em arraste (-1 = solto)
    double m_maskDragRot = 0.0;
    double m_maskDragAng = 0.0;
    QPointF m_maskPress;       // posição (disp) no início do arraste
    bool m_maskUndoPushed = false;

    // ── Malha 3×3 (warp) ──────────────────────────────────────────────
    int m_meshDrag = -1;       // ponto da malha em arraste (-1 = solto)

    // ── Modos do viewer ──────────────────────────────────────────────
    ViewMode m_mode = ModeTransform;
    QHash<int, QToolButton*> m_modeBtns;
    // Seções da sidebar que seguem o modo (mostra só as relevantes).
    QWidget* m_secCrop = nullptr;
    QWidget* m_secMove = nullptr;
    QWidget* m_secRot = nullptr;
    QWidget* m_secMesh = nullptr; // resolução da malha (modo Malha)
    QComboBox* m_meshRes = nullptr;
    void setViewMode(ViewMode m);

    DragMode m_dragMode = DragNone;
    DragMode m_hoverMode = DragNone;  // Handle sob o mouse (para highlight)
    QPointF m_grabOffset{0.0, 0.0};
    // Centro da janela (px do source) e da caixa (px do viewfinder) fixados no
    // início do arraste de zoom, para o gizmo acompanhar o mouse.
    QPointF m_zoomPivot{0.0, 0.0};
    QPointF m_zoomCenterDisp{0.0, 0.0};
    QPoint m_lastDrag;
    // Arraste de keyframes na faixa de tempo.
    bool m_stripDragging = false;
    double m_stripDragFrom = -1.0;
    double m_stripDragTo = -1.0;
    int m_stripLane = -1; // pista do keyframe selecionado (mover/apagar só ela)
    // Tempos (relativos ao clipe) dos keyframes selecionados na faixa.
    QVector<double> m_selectedTimes;
    // Arraste da agulha (playhead) na faixa de tempo.
    bool m_stripPlayheadDrag = false;
};
