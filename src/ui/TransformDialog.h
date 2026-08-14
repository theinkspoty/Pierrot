// Pierrot — editor de vídeo estilo Vegas Pro
//
// Copyright (C) 2026 Pierrot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

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
