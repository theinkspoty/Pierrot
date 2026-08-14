#pragma once

#include <QDialog>
#include "models/Project.h"

class QSpinBox;
class QDoubleSpinBox;
class QSlider;
class QLabel;
class QComboBox;
class QTableWidget;

// Diálogo de transformação e keyframes de um clipe de vídeo.
class TransformDialog : public QDialog {
    Q_OBJECT
public:
    explicit TransformDialog(Clip* clip, QWidget* parent = nullptr);
    void accept() override;

private:
    void addKeyframeRow();
    void removeSelectedRows();
    void rebuildKeyframes();

    Clip* m_clip;
    QSpinBox* m_tx = nullptr;
    QSpinBox* m_ty = nullptr;
    QSpinBox* m_scale = nullptr;
    QSpinBox* m_rotation = nullptr;
    QSlider* m_cropL = nullptr;
    QLabel* m_cropLLabel = nullptr;
    QSlider* m_cropR = nullptr;
    QLabel* m_cropRLabel = nullptr;
    QSlider* m_cropT = nullptr;
    QLabel* m_cropTLabel = nullptr;
    QSlider* m_cropB = nullptr;
    QLabel* m_cropBLabel = nullptr;
    QSlider* m_opacity = nullptr;
    QLabel* m_opacityLabel = nullptr;
    QSlider* m_volume = nullptr;
    QLabel* m_volumeLabel = nullptr;
    QComboBox* m_prop = nullptr;
    QDoubleSpinBox* m_time = nullptr;
    QDoubleSpinBox* m_value = nullptr;
    QTableWidget* m_table = nullptr;
};
