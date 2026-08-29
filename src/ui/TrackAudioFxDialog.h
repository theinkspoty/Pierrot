// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>
#include "models/Project.h"

class QSlider;
class QLabel;
class QCheckBox;

// Diálogo de efeitos de áudio de uma FAIXA inteira (estilo Vegas): EQ Express,
// redução de ruído, inversão de fase e Reverb EX aplicados ao barramento.
class TrackAudioFxDialog : public QDialog {
    Q_OBJECT
public:
    explicit TrackAudioFxDialog(Track* track, QWidget* parent = nullptr);
    void accept() override;

private:
    Track* m_track;
    QSlider* m_low = nullptr;
    QLabel* m_lowLabel = nullptr;
    QSlider* m_mid = nullptr;
    QLabel* m_midLabel = nullptr;
    QSlider* m_high = nullptr;
    QLabel* m_highLabel = nullptr;
    QCheckBox* m_denoise = nullptr;
    QSlider* m_denoiseAmt = nullptr;
    QLabel* m_denoiseAmtLabel = nullptr;
    QCheckBox* m_invert = nullptr;
    QCheckBox* m_reverb = nullptr;
    QSlider* m_reverbMix = nullptr;
    QSlider* m_reverbSize = nullptr;
};