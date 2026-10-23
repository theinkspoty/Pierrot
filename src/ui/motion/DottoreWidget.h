// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Dottore — dock de edição de motion graphics (formas geométricas).
// Um segundo viewer de edição, dockável ao lado do Preview: mostra o quadro
// do clipe selecionado e deixa mover/escalar/rotacionar a forma por gizmos,
// enquanto o Preview segue exibindo o resultado final. Diferente do Pantalone,
// não depende da Mesa: a forma é uma mídia geradora (`generator == "shape"`)
// que entra na timeline como qualquer clipe e é animada pelas keyframes
// normais (transform/opacidade) + Editor de Curvas.

#pragma once

#include <QWidget>
#include <QShowEvent>
#include <QPointF>
#include "colombina/models/Project.h"

class QLabel;
class QDoubleSpinBox;
class QPushButton;
class QComboBox;
class QColor;

// Canvas do Dottore: pré-visualiza o quadro do clipe e sobrepõe a caixa da
// forma (bounding box + alças) para arraste/escala/rotação.
class DottoreCanvas : public QWidget {
    Q_OBJECT
public:
    explicit DottoreCanvas(QWidget* parent = nullptr);

    void setClipId(Project* project, const QString& clipId, double playhead);
    void setPlayhead(double t);
    void commitChange();
    // Auto-keyframe: quando ligado, mover/escalar/girar grava keyframes no
    // playhead (em vez do valor base). Caso contrário escreve o valor base.
    void setAutoKey(bool on) { m_autoKey = on; }
    bool autoKey() const { return m_autoKey; }

signals:
    void changed();
    void editStart();
    void statusMessage(const QString& msg);

protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent* e) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    enum DragMode {
        DragNone, DragMove, DragScaleTL, DragScaleTR, DragScaleBL, DragScaleBR,
        DragRotate
    };

    // Resolve o clipe a cada uso (nunca guarda Clip* cru — evita SIGSEGV ao
    // apagar o clipe/track com o dock aberto).
    Clip* resolveClip() const;
    // Área de desenho dentro do canvas. Fonte ÚNICA do retângulo: paint,
    // hit-test e arraste precisam concordar.
    QRect plotRect() const;
    // Caixa da forma em coordenadas de widget (centro recebe pan do clipe e o
    // tamanho recebe escala × shapeW/H). `outRot` recebe o ângulo em graus.
    QRectF shapeRect(const Clip& c, double* outRot) const;
    // Converte um delta em px de widget para deslocamento em unidades de
    // imagem (o canvas mostra a imagem com "contain").
    double pxToImage(double px) const;
    int handleAt(const QPointF& pos, const QRectF& box, double rot) const;

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;

    DragMode m_drag = DragNone;
    bool m_autoKey = true;
    QPointF m_dragStart;
    double m_origTx = 0.0, m_origTy = 0.0;
    double m_origScaleX = 1.0, m_origScaleY = 1.0;
    double m_dragAngle0 = 0.0;
    // Índice do canto em arraste (0=TL,1=TR,2=BR,3=BL) para o resize.
    int m_dragCorner = -1;
};

// Dock Dottore: canvas + controles de forma (tipo, fill/stroke, dimensões).
class DottoreWidget : public QWidget {
    Q_OBJECT
public:
    explicit DottoreWidget(QWidget* parent = nullptr);
    void setProject(Project* p) { m_project = p; }

public slots:
    void setClipId(const QString& id);
    void setPlayhead(double t);
    void refresh();

signals:
    void editStart();
    void modified();
    // Emitido quando o auto-key grava/atualiza keyframes de transform, para o
    // Editor de Curvas recarregar as curvas do clipe.
    void keyframesChanged();

private:
    Clip* activeClip() const;
    MediaItem* activeShapeMedia();
    void syncControls();

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;
    bool m_loading = false; // evita loop sinal→syncControls durante carga

    DottoreCanvas* m_canvas = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_info = nullptr;
    QComboBox* m_typeCombo = nullptr;
    QPushButton* m_autoBtn = nullptr;
    QDoubleSpinBox* m_wSpin = nullptr;
    QDoubleSpinBox* m_hSpin = nullptr;
    QDoubleSpinBox* m_rSpin = nullptr;
    QDoubleSpinBox* m_sSpin = nullptr;
};
