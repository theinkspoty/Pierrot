// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>
#include "colombina/models/Project.h"

class ColorWheel;
class QSlider;
class QDoubleSpinBox;
class QLabel;
class PreviewWidget;
class TimelineWidget;

// Dock "Cor" — correção de cor estilo DaVinci Resolve. Três trackballs
// Lift/Gamma/Gain mapeados no motor ColorGrade (ColorGrade.h) já existente,
// com pré-visualização ao vivo no monitor e reflexo imediato no clipe.
class ColorDock : public QWidget {
    Q_OBJECT
public:
    explicit ColorDock(QWidget* parent = nullptr);

    void setTimeline(TimelineWidget* t) { m_timeline = t; }
    void setPreview(PreviewWidget* p);
    void setClip(Clip* c);

public slots:
    void setClipById(const QString& id);
    void refreshFromClip(); // relê o clipe e atualiza as rodas

private slots:
    void applyLive();       // aplica a cópia de trabalho ao clipe + preview

private:
    void buildUi();
    void pushUndo();
    void writeWorkFromWheels();
    void readWheelsFromWork();
    void updatePreviewFrame();

    TimelineWidget* m_timeline = nullptr;
    PreviewWidget* m_preview = nullptr;
    Clip* m_clip = nullptr;
    Clip m_work;            // cópia de trabalho (espelha o diálogo de grading)
    bool m_updating = false;

    ColorWheel* m_lift = nullptr;
    ColorWheel* m_gamma = nullptr;
    ColorWheel* m_gain = nullptr;

    // Básico (Lumetri) — sliders.
    QSlider* m_exposure = nullptr;
    QSlider* m_contrast = nullptr;
    QSlider* m_saturation = nullptr;
    QSlider* m_temperature = nullptr;
    QSlider* m_tint = nullptr;
    QSlider* m_blend = nullptr;

    QLabel* m_status = nullptr;
};
