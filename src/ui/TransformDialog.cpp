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

#include "TransformDialog.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QLabel>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QTableWidgetItem>
#include <QList>
#include <QModelIndex>
#include <QPair>
#include <QItemSelectionModel>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr int kColProp = 0;
constexpr int kColTime = 1;
constexpr int kColVal = 2;

enum class Prop {
    Scale, Rotation, X, Y, Opacity, Volume, CropL, CropR, CropT, CropB
};

QString propName(Prop p) {
    switch (p) {
    case Prop::Scale:    return QStringLiteral("Escala");
    case Prop::Rotation: return QStringLiteral("Rotação");
    case Prop::X:        return QStringLiteral("Posição X");
    case Prop::Y:        return QStringLiteral("Posição Y");
    case Prop::Opacity:  return QStringLiteral("Opacidade");
    case Prop::Volume:   return QStringLiteral("Volume");
    case Prop::CropL:    return QStringLiteral("Recorte esq.");
    case Prop::CropR:    return QStringLiteral("Recorte dir.");
    case Prop::CropT:    return QStringLiteral("Recorte topo");
    case Prop::CropB:    return QStringLiteral("Recorte base");
    }
    return QString();
}

// Alimenta o vetor de keyframes de uma propriedade a partir das linhas da tabela.
void collectRows(const QTableWidget* table, const QString& name, QVector<Keyframe>* out) {
    out->clear();
    for (int r = 0; r < table->rowCount(); ++r) {
        const QTableWidgetItem* itProp = table->item(r, kColProp);
        const QTableWidgetItem* itTime = table->item(r, kColTime);
        const QTableWidgetItem* itVal = table->item(r, kColVal);
        if (!itProp || !itTime || !itVal) continue;
        if (itProp->text() != name) continue;
        bool okT = false, okV = false;
        const double t = itTime->text().toDouble(&okT);
        const double v = itVal->text().toDouble(&okV);
        if (!okT || !okV) continue;
        Keyframe k;
        k.time = t;
        k.value = v;
        out->append(k);
    }
    std::sort(out->begin(), out->end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

} // namespace

TransformDialog::TransformDialog(Clip* clip, QWidget* parent)
    : QDialog(parent), m_clip(clip) {
    setWindowTitle(tr("Transformar"));
    setMinimumWidth(460);

    m_tx = new QSpinBox(this);
    m_tx->setRange(-2000, 2000);
    m_tx->setValue((int)llround(m_clip->tx));
    m_ty = new QSpinBox(this);
    m_ty->setRange(-2000, 2000);
    m_ty->setValue((int)llround(m_clip->ty));
    m_scale = new QSpinBox(this);
    m_scale->setRange(10, 400);
    m_scale->setSuffix(tr(" %"));
    m_scale->setValue((int)llround(m_clip->scale * 100.0));
    m_rotation = new QSpinBox(this);
    m_rotation->setRange(-360, 360);
    m_rotation->setSuffix(tr(" °"));
    m_rotation->setValue((int)llround(m_clip->rotation));

    auto makePctSlider = [this](double v, int max, QSlider** s, QLabel** l) {
        *s = new QSlider(Qt::Horizontal, this);
        (*s)->setRange(0, max);
        (*s)->setValue((int)llround(v * 100.0));
        *l = new QLabel(this);
        connect(*s, &QSlider::valueChanged, this, [l](int val) {
            (*l)->setText(QStringLiteral("%1%").arg(val));
        });
        (*l)->setText(QStringLiteral("%1%").arg((*s)->value()));
        (*l)->setMinimumWidth(42);
        (*l)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    };
    makePctSlider(m_clip->opacity, 100, &m_opacity, &m_opacityLabel);
    makePctSlider(m_clip->volume, 300, &m_volume, &m_volumeLabel);

    auto makeCropSlider = [this](double v, QSlider** s, QLabel** l) {
        *s = new QSlider(Qt::Horizontal, this);
        (*s)->setRange(0, 90);
        (*s)->setValue((int)llround(v * 100.0));
        *l = new QLabel(this);
        connect(*s, &QSlider::valueChanged, this, [l](int val) {
            (*l)->setText(QStringLiteral("%1%").arg(val));
        });
        (*l)->setText(QStringLiteral("%1%").arg((*s)->value()));
        (*l)->setMinimumWidth(42);
        (*l)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    };
    auto makeCropRow = [](QSlider* s, QLabel* l) {
        auto* r = new QHBoxLayout;
        r->addWidget(s, 1);
        r->addWidget(l);
        return r;
    };
    makeCropSlider(m_clip->cropL, &m_cropL, &m_cropLLabel);
    makeCropSlider(m_clip->cropR, &m_cropR, &m_cropRLabel);
    makeCropSlider(m_clip->cropT, &m_cropT, &m_cropTLabel);
    makeCropSlider(m_clip->cropB, &m_cropB, &m_cropBLabel);
    auto* cropLRow = makeCropRow(m_cropL, m_cropLLabel);
    auto* cropRRow = makeCropRow(m_cropR, m_cropRLabel);
    auto* cropTRow = makeCropRow(m_cropT, m_cropTLabel);
    auto* cropBRow = makeCropRow(m_cropB, m_cropBLabel);

    auto* opRow = new QHBoxLayout;
    opRow->addWidget(m_opacity, 1);
    opRow->addWidget(m_opacityLabel);
    auto* volRow = new QHBoxLayout;
    volRow->addWidget(m_volume, 1);
    volRow->addWidget(m_volumeLabel);

    auto* form = new QFormLayout;
    form->addRow(tr("Posição X:"), m_tx);
    form->addRow(tr("Posição Y:"), m_ty);
    form->addRow(tr("Escala:"), m_scale);
    form->addRow(tr("Rotação:"), m_rotation);
    form->addRow(tr("Recorte esquerda:"), cropLRow);
    form->addRow(tr("Recorte direita:"), cropRRow);
    form->addRow(tr("Recorte topo:"), cropTRow);
    form->addRow(tr("Recorte base:"), cropBRow);
    form->addRow(tr("Opacidade:"), opRow);
    form->addRow(tr("Volume:"), volRow);

    // ---- Keyframes ----
    auto* kfLabel = new QLabel(tr("Keyframes (tempo relativo ao clipe; "
                                  "sobrepõem o valor base):"), this);
    kfLabel->setWordWrap(true);

    m_prop = new QComboBox(this);
    for (int i = 0; i < 10; ++i)
        m_prop->addItem(propName(static_cast<Prop>(i)));
    m_time = new QDoubleSpinBox(this);
    m_time->setRange(0.0, 3600.0);
    m_time->setDecimals(2);
    m_time->setSuffix(tr(" s"));
    m_time->setValue(0.0);
    m_value = new QDoubleSpinBox(this);
    m_value->setRange(-5000.0, 5000.0);
    m_value->setDecimals(2);
    m_value->setValue(1.0);

    auto* addBtn = new QPushButton(tr("Adicionar"), this);
    connect(addBtn, &QPushButton::clicked, this, &TransformDialog::addKeyframeRow);

    auto* kfInputRow = new QHBoxLayout;
    kfInputRow->addWidget(m_prop);
    kfInputRow->addWidget(m_time);
    kfInputRow->addWidget(m_value);
    kfInputRow->addWidget(addBtn);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({tr("Propriedade"), tr("Tempo (s)"), tr("Valor")});
    m_table->horizontalHeader()->setSectionResizeMode(kColProp, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(kColTime, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(kColVal, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setMinimumHeight(140);

    auto* removeBtn = new QPushButton(tr("Remover selecionados"), this);
    connect(removeBtn, &QPushButton::clicked, this, &TransformDialog::removeSelectedRows);
    auto* removeRow = new QHBoxLayout;
    removeRow->addStretch(1);
    removeRow->addWidget(removeBtn);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addSpacing(8);
    lay->addWidget(kfLabel);
    lay->addLayout(kfInputRow);
    lay->addWidget(m_table);
    lay->addLayout(removeRow);
    lay->addWidget(buttons);

    // Popula a tabela com os keyframes existentes.
    const QVector<QPair<Prop, QVector<Keyframe>*>> sources = {
        {Prop::Scale, &m_clip->kfScale},
        {Prop::Rotation, &m_clip->kfRotation},
        {Prop::X, &m_clip->kfTx},
        {Prop::Y, &m_clip->kfTy},
        {Prop::Opacity, &m_clip->kfOpacity},
        {Prop::Volume, &m_clip->kfVolume},
        {Prop::CropL, &m_clip->kfCropL},
        {Prop::CropR, &m_clip->kfCropR},
        {Prop::CropT, &m_clip->kfCropT},
        {Prop::CropB, &m_clip->kfCropB},
    };
    for (const auto& src : sources)
        for (const Keyframe& k : *src.second) {
            const int row = m_table->rowCount();
            m_table->insertRow(row);
            m_table->setItem(row, kColProp, new QTableWidgetItem(propName(src.first)));
            auto* itT = new QTableWidgetItem(QString::number(k.time, 'f', 2));
            auto* itV = new QTableWidgetItem(QString::number(k.value, 'f', 3));
            m_table->setItem(row, kColTime, itT);
            m_table->setItem(row, kColVal, itV);
        }
}

void TransformDialog::addKeyframeRow() {
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, kColProp, new QTableWidgetItem(m_prop->currentText()));
    auto* itT = new QTableWidgetItem(QString::number(m_time->value(), 'f', 2));
    auto* itV = new QTableWidgetItem(QString::number(m_value->value(), 'f', 3));
    m_table->setItem(row, kColTime, itT);
    m_table->setItem(row, kColVal, itV);
}

void TransformDialog::removeSelectedRows() {
    QList<int> rows;
    const auto sel = m_table->selectionModel()->selectedRows();
    for (const QModelIndex& idx : sel) rows.append(idx.row());
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int r : rows) m_table->removeRow(r);
}

void TransformDialog::rebuildKeyframes() {
    collectRows(m_table, propName(Prop::Scale), &m_clip->kfScale);
    collectRows(m_table, propName(Prop::Rotation), &m_clip->kfRotation);
    collectRows(m_table, propName(Prop::X), &m_clip->kfTx);
    collectRows(m_table, propName(Prop::Y), &m_clip->kfTy);
    collectRows(m_table, propName(Prop::Opacity), &m_clip->kfOpacity);
    collectRows(m_table, propName(Prop::Volume), &m_clip->kfVolume);
    collectRows(m_table, propName(Prop::CropL), &m_clip->kfCropL);
    collectRows(m_table, propName(Prop::CropR), &m_clip->kfCropR);
    collectRows(m_table, propName(Prop::CropT), &m_clip->kfCropT);
    collectRows(m_table, propName(Prop::CropB), &m_clip->kfCropB);
}

void TransformDialog::accept() {
    m_clip->tx = m_tx->value();
    m_clip->ty = m_ty->value();
    m_clip->scale = m_scale->value() / 100.0;
    m_clip->rotation = m_rotation->value();
    m_clip->cropL = m_cropL->value() / 100.0;
    m_clip->cropR = m_cropR->value() / 100.0;
    m_clip->cropT = m_cropT->value() / 100.0;
    m_clip->cropB = m_cropB->value() / 100.0;
    m_clip->opacity = m_opacity->value() / 100.0;
    m_clip->volume = m_volume->value() / 100.0;
    rebuildKeyframes();
    QDialog::accept();
}
