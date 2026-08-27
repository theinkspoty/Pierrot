// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>
#include <QImage>

#include "models/Project.h"

class QLabel;
class QSlider;
class QDoubleSpinBox;
class QTimer;
class QKeyEvent;
class FFmpegDecoder;
class QPushButton;

// Trimmer simples (estilo Vegas): define os pontos de entrada/saída (in/out)
// de uma mídia com vídeo antes de inseri-la na timeline. Aberto ao soltar UMA
// mídia de arquivo com vídeo na timeline; Inserir cria o clipe já com in/out.
class TrimmerDialog : public QDialog {
    Q_OBJECT
public:
    explicit TrimmerDialog(const MediaItem& media, QWidget* parent = nullptr);

    double trimIn() const { return m_in; }
    double trimOut() const { return m_out; }

private slots:
    void seekChanged(int);
    void markIn();
    void markOut();
    void togglePlay();
    void playTick();

private:
    void keyPressEvent(QKeyEvent* e) override;
    double currentPos() const;
    void updatePreview();
    void clampInOut();

    const MediaItem& m_media;
    FFmpegDecoder* m_decoder = nullptr;
    double m_in = 0.0;
    double m_out = 0.0;
    bool m_playing = false;

    QLabel* m_preview = nullptr;
    QSlider* m_seek = nullptr;
    QDoubleSpinBox* m_spIn = nullptr;
    QDoubleSpinBox* m_spOut = nullptr;
    QLabel* m_timeLbl = nullptr;
    QTimer* m_throttle = nullptr;
    QTimer* m_playTimer = nullptr;
    QPushButton* m_playBtn = nullptr;
};