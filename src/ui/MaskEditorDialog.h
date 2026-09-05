// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>
#include <functional>
#include <QVector>
#include "models/Project.h"

class QListWidget;
class QComboBox;
class QCheckBox;
class QSlider;
class QDoubleSpinBox;

// Editor de máscaras de um clipe (rect/ellipse, por enquanto — polígono fica
// para uma próxima etapa). A janela trabalha numa CÓPIA das máscaras
// (m_work): os sliders e o arrasto no preview (via PreviewWidget) mexem só na
// cópia, com atualização em tempo real do overlay. Ao aceitar, emite
// editStart() (undo), grava no clipe e emite modified() — mesmo padrão do
// TransformDialog/ClipPropertiesWidget.
class MaskEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit MaskEditorDialog(Clip* clip, QWidget* parent = nullptr);

    // Commit: grava a cópia de trabalho no clipe (undo via editStart).
    void accept() override;

    // Handler vindo do overlay do preview durante o arrasto das alças.
    void applyExternalEdit(int index, const Mask& updated);
    // Cópia de trabalho atual (para o overlay inicial, antes dos connects).
    const QVector<Mask>& masks() const { return m_work; }

signals:
    // Estado de trabalho mudou (sliders ou arrasto) — o overlay do preview
    // acompanha em tempo real, sem tocar no clipe.
    void masksChanged(const QVector<Mask>& masks);
    // Commit: hora de empurrar o snapshot de undo (precede a gravação).
    void editStart();
    // Commit feito: timeline/preview/MainWindow reagem (com undo já salvo).
    void modified();

private:
    void buildUi();
    void syncFormFromModel();
    void syncModelFromForm();
    void refreshList();
    Mask& currentMask();
    void emitWork();

    Clip* m_clip = nullptr;          // clipe alvo (ponteiro estável na sessão)
    QVector<Mask> m_work;            // cópia de trabalho (não toca o clipe)
    bool m_loading = false;          // suprime sinais ao sincronizar os widgets

    QListWidget* m_list = nullptr;
    QComboBox* m_type = nullptr;
    QCheckBox* m_enabled = nullptr;
    QCheckBox* m_invert = nullptr;
    QSlider* m_cxS = nullptr;   QDoubleSpinBox* m_cxV = nullptr;
    QSlider* m_cyS = nullptr;   QDoubleSpinBox* m_cyV = nullptr;
    QSlider* m_rxS = nullptr;   QDoubleSpinBox* m_rxV = nullptr;
    QSlider* m_ryS = nullptr;   QDoubleSpinBox* m_ryV = nullptr;
    QSlider* m_rotS = nullptr;  QDoubleSpinBox* m_rotV = nullptr;
    QSlider* m_feS = nullptr;   QDoubleSpinBox* m_feV = nullptr;
};