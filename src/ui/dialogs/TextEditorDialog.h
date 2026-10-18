// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QDialog>
#include <QColor>
#include "colombina/models/Project.h"

class QPlainTextEdit;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QSlider;
class QPushButton;
class QLabel;
class TextPreview;

// Formulário de edição de texto/título, reutilizado em dois contextos:
//   • dentro do diálogo modal (TextEditorDialog), aplicando só no OK;
//   • dentro de um painel dockável (live), aplicando a cada mudança.
// Edita o TextStyle efetivo do clipe: se ele estiver vinculado a um
// TextResource (cópia unificada), escreve no recurso (atualiza os vinculados).
class TextEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit TextEditorWidget(QWidget* parent = nullptr);

    // Define o clipe editado (nullptr desabilita o formulário).
    void setContext(Project* project, Clip* clip);
    // Modo live: cada mudança é aplicada na hora (emite changed()). Desligado,
    // o formulário só escreve quando apply() é chamado (diálogo modal).
    void setLive(bool live) { m_live = live; }
    // Escreve os controles no estilo do clipe e emite changed().
    void apply();

signals:
    void changed();

private:
    QPushButton* makeColorButton(const QColor& color);
    void pickColor(QPushButton* btn);
    void syncFromStyle();
    void refreshPreview();
    void onEdited();
    TextStyle* style() const;

    Project* m_project = nullptr;
    Clip* m_clip = nullptr;
    bool m_live = false;
    bool m_updating = false;   // evita reentrância ao carregar os controles

    TextPreview* m_preview = nullptr;
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

// Diálogo modal clássico (criação/edição de um clipe de texto).
class TextEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit TextEditorDialog(Project* project, Clip* clip, QWidget* parent = nullptr);
    void accept() override;

private:
    TextEditorWidget* m_editor = nullptr;
};
