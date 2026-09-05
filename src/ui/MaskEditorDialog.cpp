// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MaskEditorDialog.h"

#include <QListWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QDialogButtonBox>

#include "ui/Theme.h"

namespace {

// Slider (0..1000) ↔ valor real no intervalo [lo, hi].
double sliderToValue(int v, double lo, double hi) {
    return lo + (v / 1000.0) * (hi - lo);
}
int valueToSlider(double val, double lo, double hi) {
    return qBound(0, (int)std::lround((val - lo) / (hi - lo) * 1000.0), 1000);
}

QString typeName(const QString& type) {
    if (type == QLatin1String("ellipse")) return QObject::tr("elipse");
    if (type == QLatin1String("poly"))    return QObject::tr("polígono");
    return QObject::tr("retângulo");
}

} // namespace

struct MaskRow {
    QSlider* slider = nullptr;
    QDoubleSpinBox* spin = nullptr;
};

MaskEditorDialog::MaskEditorDialog(Clip* clip, QWidget* parent)
    : QDialog(parent), m_clip(clip) {
    m_work = clip ? clip->masks : QVector<Mask>();
    if (m_work.isEmpty()) {
        Mask d;
        d.type = QStringLiteral("rect");
        m_work.append(d);
    }
    setWindowTitle(tr("Máscara — %1")
                       .arg(clip && !clip->name.isEmpty() ? clip->name : tr("clipe")));
    buildUi();
    m_list->setCurrentRow(0);
    m_loading = true;
    syncFormFromModel();
    m_loading = false;
    emitWork();
}

void MaskEditorDialog::buildUi() {
    const ThemeColors& th = themeColors();
    setStyleSheet(QStringLiteral(
        "QDialog{background:%1;} QLabel{color:%2;}"
        "QListWidget,QComboBox,QDoubleSpinBox{background:%3; border:1px solid %4;"
        " border-radius:6px; padding:3px 6px; color:%2;}"
        "QListWidget::item:selected{background:%5; color:%6;}"
        "QSlider::groove:horizontal{height:4px; border-radius:2px; background:%4;}"
        "QSlider::handle:horizontal{width:13px; margin:-5px 0; border-radius:6px;"
        " background:%7;}"
        "QCheckBox{color:%2;} QPushButton{background:%5; color:%6; border:none;"
        " border-radius:6px; padding:6px 14px; font-weight:600;}"
        "QPushButton:hover{background:%7;}")
        .arg(th.window.name())
        .arg(th.text.name())
        .arg(th.base.name())
        .arg(th.windowText.name())
        .arg(th.btnPrimary.name())
        .arg(th.btnPrimaryText.name())
        .arg(th.accent.name()));

    auto* root = new QVBoxLayout(this);

    auto* tip = new QLabel(tr("A forma aparece no monitor com alças — arraste para "
                              "mover, redimensionar ou rotacionar. Os valores aqui "
                              "acompanham em tempo real. Aplicar grava no clipe."), this);
    tip->setWordWrap(true);
    root->addWidget(tip);

    m_list = new QListWidget(this);
    m_list->setFixedHeight(84);
    root->addWidget(m_list);

    auto* rowB = new QHBoxLayout();
    auto* addB = new QPushButton(tr("+ Máscara"), this);
    auto* delB = new QPushButton(tr("−"), this);
    delB->setFixedWidth(40);
    rowB->addWidget(addB);
    rowB->addWidget(delB);
    rowB->addStretch();
    root->addLayout(rowB);

    auto* rowT = new QHBoxLayout();
    rowT->addWidget(new QLabel(tr("Tipo:"), this));
    m_type = new QComboBox(this);
    m_type->addItem(tr("Rectângulo"));
    m_type->addItem(tr("Elipse"));
    rowT->addWidget(m_type);
    rowT->addSpacing(18);
    m_enabled = new QCheckBox(tr("Ativa"), this);
    m_invert = new QCheckBox(tr("Inverter"), this);
    rowT->addWidget(m_enabled);
    rowT->addWidget(m_invert);
    rowT->addStretch();
    root->addLayout(rowT);

    auto* grid = new QGridLayout();
    const auto mkRow = [&](int row, const QString& label, double lo, double hi,
                           double step, std::function<void(double)> onValue) -> MaskRow {
        MaskRow r;
        r.slider = new QSlider(Qt::Horizontal, this);
        r.slider->setRange(0, 1000);
        r.slider->setFixedWidth(170);
        r.spin = new QDoubleSpinBox(this);
        r.spin->setRange(lo, hi);
        r.spin->setDecimals(3);
        r.spin->setSingleStep(step);
        grid->addWidget(new QLabel(label, this), row, 0);
        grid->addWidget(r.slider, row, 1);
        grid->addWidget(r.spin, row, 2);
        connect(r.spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, r, lo, hi, onValue](double v) {
                    if (m_loading) return;
                    r.slider->setValue(valueToSlider(v, lo, hi));
                    onValue(v);
                });
        connect(r.slider, &QSlider::valueChanged, this,
                [this, r, lo, hi](int sv) {
                    if (m_loading) return;
                    r.spin->setValue(sliderToValue(sv, lo, hi));
                });
        return r;
    };

    const MaskRow rows[] = {
        mkRow(0, tr("Centro X"),    0.0, 1.0,  0.01,  [this](double v){ currentMask().cx = v; emitWork(); }),
        mkRow(1, tr("Centro Y"),    0.0, 1.0,  0.01,  [this](double v){ currentMask().cy = v; emitWork(); }),
        mkRow(2, tr("Tamanho X"),   0.0, 1.0,  0.01,  [this](double v){ currentMask().rx = v; emitWork(); }),
        mkRow(3, tr("Tamanho Y"),   0.0, 1.0,  0.01,  [this](double v){ currentMask().ry = v; emitWork(); }),
        mkRow(4, tr("Rotação"),   -180.0, 180.0, 1.0, [this](double v){ currentMask().rotation = v; emitWork(); }),
        mkRow(5, tr("Feather"),     0.0, 0.25, 0.005, [this](double v){ currentMask().feather = v; emitWork(); }),
    };
    m_cxS = rows[0].slider; m_cxV = rows[0].spin;
    m_cyS = rows[1].slider; m_cyV = rows[1].spin;
    m_rxS = rows[2].slider; m_rxV = rows[2].spin;
    m_ryS = rows[3].slider; m_ryV = rows[3].spin;
    m_rotS = rows[4].slider; m_rotV = rows[4].spin;
    m_feS = rows[5].slider;  m_feV = rows[5].spin;
    root->addLayout(grid);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Aplicar"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(buttons, &QDialogButtonBox::accepted, this, &MaskEditorDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &MaskEditorDialog::reject);
    root->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) {
        m_loading = true;
        syncFormFromModel();
        m_loading = false;
    });
    connect(m_type, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (m_loading) return;
        currentMask().type = (i == 1) ? QStringLiteral("ellipse") : QStringLiteral("rect");
        refreshList();
        emitWork();
    });
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading) return;
        currentMask().enabled = v;
        refreshList();
        emitWork();
    });
    connect(m_invert, &QCheckBox::toggled, this, [this](bool v) {
        if (m_loading) return;
        currentMask().invert = v;
        emitWork();
    });
    connect(addB, &QPushButton::clicked, this, [this]() {
        Mask d;
        d.type = QStringLiteral("rect");
        m_work.append(d);
        refreshList();
        m_list->setCurrentRow(m_list->count() - 1);
        m_loading = true;
        syncFormFromModel();
        m_loading = false;
        emitWork();
    });
    connect(delB, &QPushButton::clicked, this, [this]() {
        const int i = m_list->currentRow();
        if (i < 0 || m_work.size() <= 1) return;
        m_work.removeAt(i);
        refreshList();
        m_list->setCurrentRow(qMin(i, m_list->count() - 1));
        m_loading = true;
        syncFormFromModel();
        m_loading = false;
        emitWork();
    });

    refreshList();
}

Mask& MaskEditorDialog::currentMask() {
    const int i = m_list ? m_list->currentRow() : 0;
    return m_work[qBound(0, i, m_work.size() - 1)];
}

void MaskEditorDialog::refreshList() {
    m_list->clear();
    for (int i = 0; i < m_work.size(); ++i) {
        const Mask& m = m_work[i];
        const QString on = m.enabled ? tr("ativa") : tr("desativada");
        new QListWidgetItem(tr("Máscara %1 · %2 (%3)")
                                .arg(i + 1).arg(typeName(m.type)).arg(on), m_list);
    }
    m_list->setCurrentRow(qMin(qMax(0, m_list->currentRow()), m_list->count() - 1));
}

void MaskEditorDialog::syncFormFromModel() {
    if (m_work.isEmpty()) return;
    const Mask& m = currentMask();
    m_loading = true;
    m_type->setCurrentIndex(m.type == QLatin1String("ellipse") ? 1 : 0);
    m_enabled->setChecked(m.enabled);
    m_invert->setChecked(m.invert);
    m_cxS->setValue(valueToSlider(m.cx, 0.0, 1.0));
    m_cyS->setValue(valueToSlider(m.cy, 0.0, 1.0));
    m_rxS->setValue(valueToSlider(m.rx, 0.0, 1.0));
    m_ryS->setValue(valueToSlider(m.ry, 0.0, 1.0));
    m_rotS->setValue(valueToSlider(m.rotation, -180.0, 180.0));
    m_feS->setValue(valueToSlider(m.feather, 0.0, 0.25));
    m_cxV->setValue(m.cx);
    m_cyV->setValue(m.cy);
    m_rxV->setValue(m.rx);
    m_ryV->setValue(m.ry);
    m_rotV->setValue(m.rotation);
    m_feV->setValue(m.feather);
    m_loading = false;
}

void MaskEditorDialog::applyExternalEdit(int index, const Mask& updated) {
    if (index < 0 || index >= m_work.size()) return;
    m_work[index] = updated;
    if (m_list && m_list->currentRow() == index) {
        m_loading = true;
        syncFormFromModel();
        m_loading = false;
    }
}

void MaskEditorDialog::emitWork() {
    if (m_loading) return;
    emit masksChanged(m_work);
}

void MaskEditorDialog::accept() {
    if (m_clip) {
        emit editStart();
        m_clip->masks = m_work;
        emit modified();
    }
    QDialog::accept();
}