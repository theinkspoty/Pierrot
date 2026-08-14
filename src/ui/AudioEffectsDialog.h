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

class QSlider;
class QLabel;
class QCheckBox;

// Diálogo de efeitos de áudio de um clipe.
class AudioEffectsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AudioEffectsDialog(Clip* clip, QWidget* parent = nullptr);
    void accept() override;

private:
    Clip* m_clip;
    QSlider* m_low = nullptr;
    QLabel* m_lowLabel = nullptr;
    QSlider* m_mid = nullptr;
    QLabel* m_midLabel = nullptr;
    QSlider* m_high = nullptr;
    QLabel* m_highLabel = nullptr;
    QCheckBox* m_denoise = nullptr;
    QSlider* m_denoiseAmt = nullptr;
    QLabel* m_denoiseAmtLabel = nullptr;
    QCheckBox* m_normalize = nullptr;
    QCheckBox* m_invert = nullptr;
};
