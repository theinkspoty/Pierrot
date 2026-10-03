// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Editor de Velocidade do clipe (dock, estilo Time Remapping do Premiere).
// Edita Clip::speed (base) e Clip::kfSpeed (envelope animado). O preview e a
// exportação já consomem kfSpeed via clipSrcTime / renderVelocitySequence.

#pragma once

#include <QWidget>
#include <QVector>
#include "colombina/models/Project.h"

class QLabel;
class QDoubleSpinBox;
class QToolButton;
class QTimer;

// Ferramentas do canvas de velocidade (espelho do Editor de Curvas).
enum class VelTool {
    Select = 0, // arrastar keyframes
    Add,        // clique cria keyframe linear
    Curve       // clique cria ponto suave; arrastar alças entorta a curva
};

// Canvas da curva de velocidade: eixo Y = multiplicador (×), eixo X = tempo
// relativo do clipe (0..dur). Curva bezier (kfSpeed) com alças arrastáveis.
class VelocityCanvas : public QWidget {
    Q_OBJECT
public:
    explicit VelocityCanvas(QWidget* parent = nullptr);

    void setClip(Clip* clip, double playhead, double fps);
    void setClipId(Project* project, const QString& clipId, double playhead);
    void setPlayhead(double t);
    void commitChange(); // emite changed() depois de mutação
    void setTool(VelTool t) { m_tool = t; update(); }
    VelTool tool() const { return m_tool; }
    // Easy Ease no keyframe mais próximo de `rel` (ou no arrastado).
    void easyEaseAt(double rel, int mode = 0);

signals:
    void changed(); // clipe mutado (kfSpeed/speed)
    void editStart();
    void statusMessage(const QString& msg);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    QPoint timeToPos(double t) const; // usa clipSpeedAt (curva real)
    QPoint keyToPos(const Keyframe& k) const; // usa o valor do keyframe
    double posToTime(const QPoint& p) const;
    double posToValue(const QPoint& p) const;
    int hitKey(const QPoint& p) const;
    // Distância vertical em pixels até a curva em posToTime(p).
    double distanceToCurve(const QPoint& p) const;
    // Alça: idx do kf, side 0=out (ox/oy), 1=in (ix/iy).
    int hitHandle(const QPoint& p, int* idx, int* side) const;
    void ensureDefaults();
    void drawCurve(QPainter& p, const QRect& plot);
    // Resolve o clipe por id (evita SIGSEGV ao apagar o clipe).
    Clip* resolveClip() const;

    Project* m_project = nullptr;
    QString m_clipId;
    Clip* m_clip = nullptr; // cache; revalidado via resolveClip()
    double m_playhead = 0.0;
    double m_fps = 30.0;
    double m_valLo = 0.0, m_valHi = 2.0;
    VelTool m_tool = VelTool::Select;
    int m_dragIdx = -1;
    int m_dragHandle = -1; // -1 = arrasta o keyframe; 0=out; 1=in
    QPoint m_dragOff;
    bool m_panning = false;
};

class VelocityEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit VelocityEditorWidget(QWidget* parent = nullptr);

    void setProject(Project* p) { m_project = p; }

public slots:
    void setClipId(const QString& id);
    void setPlayhead(double t);

signals:
    void editStart();
    void modified();

private:
    Clip* activeClip() const;
    void refreshUi();
    void applyBaseSpeed(double v);

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;

    QLabel* m_title = nullptr;
    VelocityCanvas* m_canvas = nullptr;
    QDoubleSpinBox* m_baseSpin = nullptr;
    QDoubleSpinBox* m_valSpin = nullptr; // valor do kf selecionado / preview
    QToolButton* m_addBtn = nullptr;
    QToolButton* m_delBtn = nullptr;
    QToolButton* m_resetBtn = nullptr;
    QToolButton* m_toolSel = nullptr;
    QToolButton* m_toolAdd = nullptr;
    QToolButton* m_toolCurve = nullptr; // varinha de entortar
    QLabel* m_info = nullptr;
};
