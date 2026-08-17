// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>
#include "models/Project.h"

class QPlainTextEdit;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QSlider;
class QPushButton;
class QLabel;

// Criador de texto/título sobreposto de um clipe (estilo Premiere/Vegas/FC).
// Edita o TextStyle efetivo do clipe: se ele estiver vinculado a um
// TextResource (cópia unificada), escreve no recurso (atualiza todos os
// clipes vinculados); senão, escreve no próprio clipe.
class TextEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit TextEditorDialog(Project* project, Clip* clip, QWidget* parent = nullptr);
    void accept() override;

private:
    QPushButton* makeColorButton(QColor color);
    void pickColor(QPushButton* btn);

    Project* m_project;
    Clip* m_clip;
    TextStyle& m_style;
    QPlainTextEdit* m_text = nullptr;
    QComboBox* m_font = nullptr;
    QDoubleSpinBox* m_size = nullptr;   // % da altura do quadro
    QCheckBox* m_bold = nullptr;
    QPushButton* m_fillBtn = nullptr;
    QCheckBox* m_outlineOn = nullptr;
    QDoubleSpinBox* m_outlineW = nullptr; // % da altura
    QPushButton* m_outlineBtn = nullptr;
    QCheckBox* m_bgOn = nullptr;
    QPushButton* m_bgBtn = nullptr;
    QSlider* m_bgAlpha = nullptr;
    QComboBox* m_align = nullptr;
    QSlider* m_posX = nullptr;
    QSlider* m_posY = nullptr;
};
