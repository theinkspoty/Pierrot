// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QImage>
#include <QPoint>
#include <QHash>
#include "models/Project.h"
#include "ffmpeg/FFmpegDecoder.h"

class QSlider;
class QLabel;
class QPushButton;

// Painel docável de pan/crop estilo DaVinci/Vegas: viewfinder com a caixa de
// recorte/zoom arrastável e sliders de crop/escala/pan.
// O viewfinder fica ao lado (à esquerda) dos controles de transformação, para
// a tela não ficar atrás/embaixo dos sliders. Os keyframes seguem o padrão do
// DaVinci Resolve: um losango por parâmetro alterna keyframe no playhead,
// botões ◀/▶ navegam e o auto-keyframe cria keyframe ao mexer num slider.
class PancropWidget : public QWidget {
    Q_OBJECT
public:
    explicit PancropWidget(QWidget* parent = nullptr);

    void setProject(Project* p);
    void setClipId(const QString& id);
    void setPlayhead(double t);
    QString clipId() const { return m_clipId; }
public slots:
    void refresh();
    void sync();
signals:
    void editStart();
    void modified();
    void keyframeJump(double t);
protected:
    // O viewfinder é um QWidget aninhado (Viewport); o PancropWidget apenas
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
    void viewportWheel(QWidget* view, QWheelEvent* e);

    void paintKeyframeStrip(QWidget* view);
    void stripPress(QWidget* view, QMouseEvent* e);
    void stripMove(QWidget* view, QMouseEvent* e);
    void stripRelease(QWidget* view, QMouseEvent* e);
    void stripContextMenu(QWidget* view, QContextMenuEvent* e);

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
    double propValue(int prop) const;
    void toggleKeyframe(int prop);
    void gotoKeyframe(int dir);
    void writeKeyframe(int prop, double value);
    void refreshDiamonds();

    void computeView(double s, double tx, double ty, int w0, int h0,
                     QRectF* cropS, QRectF* outS) const;
    void screenToSource(const QRect& viewRect, const QPoint& sp,
                        double* sx, double* sy) const;
    void applyPan(double sx, double sy);

    enum DragMode { DragNone, DragPan,
                    DragCropL, DragCropR, DragCropT, DragCropB,
                    DragCropTL, DragCropTR, DragCropBL, DragCropBR };

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;
    bool m_undoPushed = false;

    FFmpegDecoder m_decoder;
    QImage m_frame;
    QString m_framePath;

    QSlider* m_cropL = nullptr;
    QSlider* m_cropR = nullptr;
    QSlider* m_cropT = nullptr;
    QSlider* m_cropB = nullptr;
    QSlider* m_scale = nullptr;
    QSlider* m_panX = nullptr;
    QSlider* m_panY = nullptr;

    QLabel* m_cropLVal = nullptr;
    QLabel* m_cropRVal = nullptr;
    QLabel* m_cropTVal = nullptr;
    QLabel* m_cropBVal = nullptr;
    QLabel* m_scaleVal = nullptr;
    QLabel* m_panXVal = nullptr;
    QLabel* m_panYVal = nullptr;

    QHash<int, QPushButton*> m_kfDiamonds;
    QPushButton* m_kfAuto = nullptr;

    DragMode m_dragMode = DragNone;
    QPointF m_grabOffset{0.0, 0.0};
    QPoint m_lastDrag;
    // Arraste de keyframes na faixa de tempo.
    bool m_stripDragging = false;
    double m_stripDragFrom = -1.0;
    double m_stripDragTo = -1.0;
};
