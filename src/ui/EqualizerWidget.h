// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QVector>

#include "colombina/models/Project.h"

class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QTabWidget;
class QTimer;
class QVBoxLayout;
class EqCurveView;
class Project;

// ── EQ gráfico (10 bandas fixas) — página de curva ────────────────────
class GraphicEqPage : public QWidget {
    Q_OBJECT
public:
    explicit GraphicEqPage(QWidget* parent = nullptr);
    void setGains(QVector<double>* gains); // operado pelo dono (projeto)
    void reload();
signals:
    void editStart();
    void modified();
private:
    void beginEdit();
    QTimer* m_undoTimer = nullptr;
    bool m_undoPushed = false;
    QVector<double>* m_gains = nullptr;
    EqCurveView* m_view = nullptr;
};

// ── EQ paramétrico — página de curva + campos por banda ───────────────
class ParametricEqPage : public QWidget {
    Q_OBJECT
public:
    explicit ParametricEqPage(QWidget* parent = nullptr);
    void setBands(QVector<ParametricBand>* bands);
    void reload();
signals:
    void editStart();
    void modified();
private slots:
    void onNodeMoved(int index, double freq, double gain);
    void onNodeSelected(int index);
private:
    void beginEdit();
    void pushToView();
    void rebuildRows();
    void addBand();
    void removeSelected();
    void applyDefault();
    QTimer* m_undoTimer = nullptr;
    bool m_undoPushed = false;
    bool m_updating = false;
    int m_sel = -1;
    QVector<ParametricBand>* m_bands = nullptr;
    EqCurveView* m_view = nullptr;
    QWidget* m_rowsHost = nullptr;
    QVBoxLayout* m_rowsLayout = nullptr;
    QVector<QDoubleSpinBox*> m_freqSpins;
    QVector<QDoubleSpinBox*> m_gainSpins;
    QVector<QDoubleSpinBox*> m_qSpins;
};

// ── Dock do equalizador: seletor de alvo + abas Gráfico/Paramétrico ───
class EqualizerWidget : public QWidget {
    Q_OBJECT
public:
    explicit EqualizerWidget(QWidget* parent = nullptr);
    void setProject(Project* p);
public slots:
    void refresh();
signals:
    void editStart();
    void modified();
private:
    void loadTarget();
    int targetIndex() const;
    Project* m_project = nullptr;
    QComboBox* m_target = nullptr;
    QTabWidget* m_tabs = nullptr;
    GraphicEqPage* m_graphic = nullptr;
    ParametricEqPage* m_parametric = nullptr;
    bool m_updating = false;
};
