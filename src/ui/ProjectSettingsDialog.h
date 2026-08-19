// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>

class QSpinBox;
class QComboBox;
class QLabel;

class ProjectSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectSettingsDialog(int width, int height, int fps,
                                   QWidget* parent = nullptr);
    int width() const;
    int height() const;
    int fps() const;
private:
    void updateAspect();
    QSpinBox* m_w = nullptr;
    QSpinBox* m_h = nullptr;
    QComboBox* m_fps = nullptr;
    QComboBox* m_preset = nullptr;
    QLabel* m_aspect = nullptr;
    bool m_applyingPreset = false;
};
