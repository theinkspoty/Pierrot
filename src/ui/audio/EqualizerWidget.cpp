// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/audio/EqualizerWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "ui/audio/EqCurveView.h"

namespace {

// Ganhos do EQ gráfico do alvo: -1 = master; senão faixa de áudio `idx`.
QVector<double>* graphicGainsFor(Project* p, int idx) {
    if (!p) return nullptr;
    if (idx < 0) return &p->masterGraphicEq;
    if (idx < (int)p->audioTracks.size()) return &p->audioTracks[idx].graphicEq;
    return nullptr;
}

// Bandas do EQ paramétrico do alvo.
QVector<ParametricBand>* parametricBandsFor(Project* p, int idx) {
    if (!p) return nullptr;
    if (idx < 0) return &p->masterParametricEq;
    if (idx < (int)p->audioTracks.size())
        return &p->audioTracks[idx].parametricEq;
    return nullptr;
}

} // namespace

// ── EQ gráfico ────────────────────────────────────────────────────────

GraphicEqPage::GraphicEqPage(QWidget* parent) : QWidget(parent) {
    m_undoTimer = new QTimer(this);
    m_undoTimer->setSingleShot(true);
    m_undoTimer->setInterval(600);
    connect(m_undoTimer, &QTimer::timeout, this,
            [this]() { m_undoPushed = false; });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 6, 0, 0);
    root->setSpacing(6);

    m_view = new EqCurveView(this);
    m_view->setGainRange(12.0);
    connect(m_view, &EqCurveView::nodeMoved, this,
            [this](int index, double, double gain) {
                if (!m_gains) return;
                if (index < 0 || index >= kGraphicEqBands) return;
                beginEdit();
                while (m_gains->size() < kGraphicEqBands) m_gains->append(0.0);
                (*m_gains)[index] = gain;
                emit modified();
            });
    root->addWidget(m_view, 1);

    auto* hint =
        new QLabel(tr("Arraste os pontos na vertical (ganho ±12 dB)."), this);
    hint->setAlignment(Qt::AlignHCenter);
    root->addWidget(hint);
}

void GraphicEqPage::setGains(QVector<double>* gains) {
    m_gains = gains;
    reload();
}

void GraphicEqPage::reload() {
    if (!m_view) return;
    const double* f = graphicEqFrequencies();
    QVector<EqNode> nodes;
    for (int b = 0; b < kGraphicEqBands; ++b) {
        EqNode n;
        n.freq = f[b];
        n.gain = (m_gains && b < m_gains->size()) ? (*m_gains)[b] : 0.0;
        n.q = 1.414;
        n.type = EqBell;
        n.movableFreq = false;
        nodes.append(n);
    }
    m_view->setNodes(nodes);
}

void GraphicEqPage::beginEdit() {
    if (!m_undoPushed) {
        emit editStart();
        m_undoPushed = true;
    }
    m_undoTimer->start();
}

// ── EQ paramétrico ────────────────────────────────────────────────────

ParametricEqPage::ParametricEqPage(QWidget* parent) : QWidget(parent) {
    m_undoTimer = new QTimer(this);
    m_undoTimer->setSingleShot(true);
    m_undoTimer->setInterval(600);
    connect(m_undoTimer, &QTimer::timeout, this,
            [this]() { m_undoPushed = false; });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 6, 0, 0);
    root->setSpacing(6);

    auto* btns = new QHBoxLayout();
    btns->setSpacing(6);
    auto* addBtn = new QPushButton(tr("+ Banda"), this);
    auto* delBtn = new QPushButton(tr("Remover"), this);
    auto* defBtn = new QPushButton(tr("Padrão"), this);
    addBtn->setToolTip(tr("Adiciona uma banda Bell"));
    delBtn->setToolTip(tr("Remove a banda selecionada"));
    defBtn->setToolTip(tr("Substitui por 5 bandas sugeridas"));
    connect(addBtn, &QPushButton::clicked, this, [this]() { addBand(); });
    connect(delBtn, &QPushButton::clicked, this, [this]() { removeSelected(); });
    connect(defBtn, &QPushButton::clicked, this, [this]() { applyDefault(); });
    btns->addWidget(addBtn);
    btns->addWidget(delBtn);
    btns->addWidget(defBtn);
    btns->addStretch(1);
    root->addLayout(btns);

    m_view = new EqCurveView(this);
    m_view->setGainRange(kParamEqMaxGain);
    connect(m_view, &EqCurveView::nodeMoved, this,
            &ParametricEqPage::onNodeMoved);
    connect(m_view, &EqCurveView::nodeSelected, this,
            &ParametricEqPage::onNodeSelected);
    root->addWidget(m_view, 3);

    auto* header = new QWidget(this);
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(6);
    auto* hOn = new QLabel(tr("On"), header);
    auto* hType = new QLabel(tr("Tipo"), header);
    auto* hFreq = new QLabel(tr("Freq"), header);
    auto* hGain = new QLabel(tr("Ganho"), header);
    auto* hQ = new QLabel(tr("Q"), header);
    hOn->setFixedWidth(26);
    hType->setFixedWidth(94);
    hFreq->setFixedWidth(88);
    hGain->setFixedWidth(78);
    hQ->setFixedWidth(66);
    hl->addWidget(hOn);
    hl->addWidget(hType);
    hl->addWidget(hFreq);
    hl->addWidget(hGain);
    hl->addWidget(hQ);
    hl->addStretch(1);
    root->addWidget(header);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_rowsHost = new QWidget(scroll);
    m_rowsLayout = new QVBoxLayout(m_rowsHost);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(4);
    m_rowsLayout->addStretch(1);
    scroll->setWidget(m_rowsHost);
    root->addWidget(scroll, 2);
}

void ParametricEqPage::setBands(QVector<ParametricBand>* bands) {
    m_bands = bands;
    m_sel = -1;
    reload();
}

void ParametricEqPage::reload() {
    rebuildRows();
    pushToView();
}

void ParametricEqPage::pushToView() {
    if (!m_view) return;
    QVector<EqNode> nodes;
    if (m_bands) {
        for (const ParametricBand& b : *m_bands) {
            EqNode n;
            n.freq = b.freq;
            n.gain = b.gain;
            n.q = b.q;
            n.type = b.type;
            n.enabled = b.enabled;
            n.movableFreq = true;
            nodes.append(n);
        }
    }
    m_view->setNodes(nodes);
    m_view->setSelected(m_sel);
}

void ParametricEqPage::rebuildRows() {
    if (!m_rowsLayout) return;
    // Limpa as linhas existentes.
    while (m_rowsLayout->count() > 1) {
        QLayoutItem* it = m_rowsLayout->takeAt(0);
        if (QWidget* w = it->widget()) w->deleteLater();
        delete it;
    }
    m_freqSpins.clear();
    m_gainSpins.clear();
    m_qSpins.clear();
    if (!m_bands) return;
    for (int i = 0; i < m_bands->size(); ++i) {
        auto* row = new QWidget(m_rowsHost);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(6);

        auto* en = new QCheckBox(row);
        en->setChecked((*m_bands)[i].enabled);
        en->setFixedWidth(26);
        en->setToolTip(tr("Ativar/desativar banda"));

        auto* type = new QComboBox(row);
        type->addItem(tr("Bell"));
        type->addItem(tr("Low Shelf"));
        type->addItem(tr("High Shelf"));
        type->setCurrentIndex((*m_bands)[i].type);
        type->setFixedWidth(94);

        auto* freq = new QDoubleSpinBox(row);
        freq->setRange(kParamEqMinFreq, kParamEqMaxFreq);
        freq->setDecimals(0);
        freq->setSingleStep(10.0);
        freq->setSuffix(QStringLiteral(" Hz"));
        freq->setValue((*m_bands)[i].freq);
        freq->setFixedWidth(88);

        auto* gain = new QDoubleSpinBox(row);
        gain->setRange(-kParamEqMaxGain, kParamEqMaxGain);
        gain->setDecimals(1);
        gain->setSingleStep(0.5);
        gain->setSuffix(QStringLiteral(" dB"));
        gain->setValue((*m_bands)[i].gain);
        gain->setFixedWidth(78);

        auto* q = new QDoubleSpinBox(row);
        q->setRange(0.1, 30.0);
        q->setDecimals(2);
        q->setSingleStep(0.1);
        q->setValue((*m_bands)[i].q);
        q->setFixedWidth(66);
        q->setEnabled((*m_bands)[i].type == EqBell);

        rl->addWidget(en);
        rl->addWidget(type);
        rl->addWidget(freq);
        rl->addWidget(gain);
        rl->addWidget(q);
        rl->addStretch(1);
        m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, row);
        m_freqSpins.append(freq);
        m_gainSpins.append(gain);
        m_qSpins.append(q);

        connect(en, &QCheckBox::toggled, this, [this, i](bool on) {
            if (m_updating || !m_bands || i >= m_bands->size()) return;
            beginEdit();
            (*m_bands)[i].enabled = on;
            pushToView();
            emit modified();
        });
        connect(type,
                QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, i, q](int t) {
                    if (m_updating || !m_bands || i >= m_bands->size()) return;
                    beginEdit();
                    (*m_bands)[i].type = t;
                    q->setEnabled(t == EqBell);
                    pushToView();
                    emit modified();
                });
        connect(freq,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, i](double v) {
                    if (m_updating || !m_bands || i >= m_bands->size()) return;
                    beginEdit();
                    (*m_bands)[i].freq = v;
                    pushToView();
                    emit modified();
                });
        connect(gain,
                QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, i](double v) {
                    if (m_updating || !m_bands || i >= m_bands->size()) return;
                    beginEdit();
                    (*m_bands)[i].gain = v;
                    pushToView();
                    emit modified();
                });
        connect(q, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, i](double v) {
                    if (m_updating || !m_bands || i >= m_bands->size()) return;
                    beginEdit();
                    (*m_bands)[i].q = v;
                    pushToView();
                    emit modified();
                });
    }
}

void ParametricEqPage::onNodeMoved(int index, double freq, double gain) {
    if (m_updating || !m_bands) return;
    if (index < 0 || index >= m_bands->size()) return;
    beginEdit();
    (*m_bands)[index].freq = freq;
    (*m_bands)[index].gain = gain;
    // Reflete nos campos (sem disparar os spins de novo).
    if (index < m_freqSpins.size() && index < m_gainSpins.size()) {
        m_updating = true;
        m_freqSpins[index]->setValue(freq);
        m_gainSpins[index]->setValue(gain);
        m_updating = false;
    }
    emit modified();
}

void ParametricEqPage::onNodeSelected(int index) {
    m_sel = index;
}

void ParametricEqPage::addBand() {
    if (!m_bands) return;
    ParametricBand b;
    b.freq = 1000.0;
    b.gain = 0.0;
    b.q = 1.0;
    b.type = EqBell;
    m_bands->append(b);
    m_sel = m_bands->size() - 1;
    beginEdit();
    rebuildRows();
    pushToView();
    emit modified();
}

void ParametricEqPage::removeSelected() {
    if (!m_bands || m_sel < 0 || m_sel >= m_bands->size()) return;
    m_bands->removeAt(m_sel);
    if (m_sel >= m_bands->size()) m_sel = m_bands->size() - 1;
    beginEdit();
    rebuildRows();
    pushToView();
    emit modified();
}

void ParametricEqPage::applyDefault() {
    if (!m_bands) return;
    *m_bands = defaultParametricEq();
    m_sel = m_bands->isEmpty() ? -1 : 0;
    beginEdit();
    rebuildRows();
    pushToView();
    emit modified();
}

void ParametricEqPage::beginEdit() {
    if (!m_undoPushed) {
        emit editStart();
        m_undoPushed = true;
    }
    m_undoTimer->start();
}

// ── Dock do equalizador ───────────────────────────────────────────────

EqualizerWidget::EqualizerWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel(tr("Alvo:"), this));
    m_target = new QComboBox(this);
    m_target->setMinimumWidth(180);
    connect(m_target, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (!m_updating) loadTarget();
            });
    top->addWidget(m_target);
    top->addStretch(1);
    root->addLayout(top);

    m_tabs = new QTabWidget(this);
    m_graphic = new GraphicEqPage(m_tabs);
    m_parametric = new ParametricEqPage(m_tabs);
    m_tabs->addTab(m_graphic, tr("Gráfico"));
    m_tabs->addTab(m_parametric, tr("Paramétrico"));
    root->addWidget(m_tabs, 1);

    connect(m_graphic, &GraphicEqPage::editStart, this,
            &EqualizerWidget::editStart);
    connect(m_graphic, &GraphicEqPage::modified, this,
            &EqualizerWidget::modified);
    connect(m_parametric, &ParametricEqPage::editStart, this,
            &EqualizerWidget::editStart);
    connect(m_parametric, &ParametricEqPage::modified, this,
            &EqualizerWidget::modified);
}

void EqualizerWidget::setProject(Project* p) {
    m_project = p;
    refresh();
}

void EqualizerWidget::refresh() {
    if (!m_target) return;
    const int prev = targetIndex();
    m_updating = true;
    m_target->clear();
    m_target->addItem(tr("Master"), -1);
    if (m_project) {
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
            const Track& t = m_project->audioTracks[i];
            const QString name =
                t.name.isEmpty() ? tr("Faixa %1").arg(i + 1) : t.name;
            m_target->addItem(tr("%1: %2").arg(i + 1).arg(name), i);
        }
    }
    int sel = 0;
    for (int i = 0; i < m_target->count(); ++i)
        if (m_target->itemData(i).toInt() == prev) {
            sel = i;
            break;
        }
    m_target->setCurrentIndex(sel);
    m_updating = false;
    loadTarget();
}

int EqualizerWidget::targetIndex() const {
    if (!m_target || m_target->currentIndex() < 0) return -1;
    return m_target->currentData().toInt();
}

void EqualizerWidget::loadTarget() {
    const int idx = targetIndex();
    m_graphic->setGains(graphicGainsFor(m_project, idx));
    m_parametric->setBands(parametricBandsFor(m_project, idx));
}
