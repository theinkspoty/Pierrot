// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TextEditorDialog.h"

#include <QPlainTextEdit>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFontDatabase>
#include <QFont>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QPixmap>
#include <QIcon>
#include <cmath>

TextEditorDialog::TextEditorDialog(Project* project, Clip* clip, QWidget* parent)
    : QDialog(parent), m_project(project), m_clip(clip),
      m_style(project ? *project->textStyleFor(*clip) : clip->text) {
    setWindowTitle(tr("Texto do clipe"));
    resize(480, 560);

    auto* root = new QVBoxLayout(this);

    m_text = new QPlainTextEdit(m_style.text, this);
    m_text->setPlaceholderText(tr("Digite o texto/título…"));
    m_text->setMinimumHeight(110);
    root->addWidget(new QLabel(tr("Texto:"), this));
    root->addWidget(m_text);

    // ---- Fonte ----
    m_font = new QComboBox(this);
    const QStringList fams = QFontDatabase().families();
    m_font->addItems(fams);
    int fi = fams.indexOf(m_style.fontFamily.isEmpty() ? QFont().family() : m_style.fontFamily);
    if (fi < 0) fi = fams.indexOf(QStringLiteral("DejaVu Sans"));
    if (fi < 0) fi = 0;
    m_font->setCurrentIndex(fi);
    m_font->setEditable(true);

    m_size = new QDoubleSpinBox(this);
    m_size->setRange(1.0, 40.0);
    m_size->setSuffix(QStringLiteral(" %"));
    m_size->setValue(m_style.textSize > 0.0 ? m_style.textSize * 100.0 : 5.6);

    m_bold = new QCheckBox(tr("Negrito"), this);
    m_bold->setChecked(m_style.textBold);

    auto* fontRow = new QHBoxLayout;
    fontRow->addWidget(m_font, 1);
    fontRow->addWidget(m_size);
    fontRow->addWidget(m_bold);

    // ---- Cor do texto ----
    m_fillBtn = makeColorButton(m_style.textColor);

    auto* fillRow = new QHBoxLayout;
    fillRow->addWidget(new QLabel(tr("Cor do texto:"), this));
    fillRow->addWidget(m_fillBtn, 1);
    fillRow->addStretch(1);

    // ---- Contorno ----
    m_outlineOn = new QCheckBox(tr("Contorno"), this);
    m_outlineOn->setChecked(m_style.textOutline > 0.0);
    m_outlineW = new QDoubleSpinBox(this);
    m_outlineW->setRange(0.1, 10.0);
    m_outlineW->setSuffix(QStringLiteral(" %"));
    m_outlineW->setValue(m_style.textOutline > 0.0 ? m_style.textOutline * 100.0 : 2.0);
    m_outlineW->setEnabled(m_outlineOn->isChecked());
    m_outlineBtn = makeColorButton(m_style.textOutlineColor);
    m_outlineBtn->setEnabled(m_outlineOn->isChecked());
    connect(m_outlineOn, &QCheckBox::toggled, this, [this](bool on) {
        m_outlineW->setEnabled(on);
        m_outlineBtn->setEnabled(on);
    });
    auto* outRow = new QHBoxLayout;
    outRow->addWidget(m_outlineOn);
    outRow->addWidget(m_outlineW);
    outRow->addWidget(m_outlineBtn);

    // ---- Fundo ----
    m_bgOn = new QCheckBox(tr("Fundo"), this);
    m_bgOn->setChecked(m_style.textBackground);
    m_bgBtn = makeColorButton(m_style.textBackgroundColor);
    m_bgBtn->setEnabled(m_style.textBackground);
    m_bgAlpha = new QSlider(Qt::Horizontal, this);
    m_bgAlpha->setRange(0, 100);
    m_bgAlpha->setValue(m_style.textBackgroundColor.alpha() * 100 / 255);
    m_bgAlpha->setEnabled(m_style.textBackground);
    connect(m_bgOn, &QCheckBox::toggled, this, [this](bool on) {
        m_bgBtn->setEnabled(on);
        m_bgAlpha->setEnabled(on);
    });
    auto* bgRow = new QHBoxLayout;
    bgRow->addWidget(m_bgOn);
    bgRow->addWidget(m_bgBtn);
    bgRow->addWidget(new QLabel(tr("Opac."), this));
    bgRow->addWidget(m_bgAlpha, 1);

    // ---- Alinhamento e posição ----
    m_align = new QComboBox(this);
    m_align->addItem(tr("Centro"));
    m_align->addItem(tr("Esquerda"));
    m_align->addItem(tr("Direita"));
    m_align->setCurrentIndex(qBound(0, m_style.textAlign, 2));

    m_posX = new QSlider(Qt::Horizontal, this);
    m_posX->setRange(0, 100);
    m_posX->setValue((int)llround(m_style.textX * 100.0));
    m_posY = new QSlider(Qt::Horizontal, this);
    m_posY->setRange(0, 100);
    m_posY->setValue((int)llround(m_style.textY * 100.0));

    auto* alignRow = new QHBoxLayout;
    alignRow->addWidget(new QLabel(tr("Alinhamento:"), this));
    alignRow->addWidget(m_align, 1);

    auto* posXRow = new QHBoxLayout;
    posXRow->addWidget(new QLabel(tr("Posição X:"), this));
    posXRow->addWidget(m_posX, 1);
    auto* posYRow = new QHBoxLayout;
    posYRow->addWidget(new QLabel(tr("Posição Y:"), this));
    posYRow->addWidget(m_posY, 1);

    auto* form = new QFormLayout;
    form->addRow(tr("Fonte:"), fontRow);
    form->addRow(fillRow);
    form->addRow(outRow);
    form->addRow(bgRow);
    form->addRow(alignRow);
    form->addRow(posXRow);
    form->addRow(posYRow);
    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

QPushButton* TextEditorDialog::makeColorButton(QColor color) {
    auto* b = new QPushButton(this);
    QPixmap pm(18, 18);
    pm.fill(color);
    b->setIcon(QIcon(pm));
    b->setToolTip(color.name(QColor::HexArgb));
    connect(b, &QPushButton::clicked, this, [this, b]() { pickColor(b); });
    return b;
}

void TextEditorDialog::pickColor(QPushButton* btn) {
    QColor cur = btn->icon().pixmap(18, 18).toImage().pixelColor(9, 9);
    const QColor c = QColorDialog::getColor(cur, this);
    if (!c.isValid()) return;
    QPixmap pm(18, 18);
    pm.fill(c);
    btn->setIcon(QIcon(pm));
    btn->setToolTip(c.name(QColor::HexArgb));
}

void TextEditorDialog::accept() {
    m_style.text = m_text->toPlainText().trimmed();
    m_style.fontFamily = m_font->currentText().trimmed();
    m_style.textSize = m_size->value() / 100.0;
    m_style.textBold = m_bold->isChecked();
    m_style.textColor = m_fillBtn->icon().pixmap(18, 18).toImage().pixelColor(9, 9);
    m_style.textOutline = m_outlineOn->isChecked() ? m_outlineW->value() / 100.0 : 0.0;
    m_style.textOutlineColor = m_outlineBtn->icon().pixmap(18, 18).toImage().pixelColor(9, 9);
    m_style.textBackground = m_bgOn->isChecked();
    QColor bg = m_bgBtn->icon().pixmap(18, 18).toImage().pixelColor(9, 9);
    bg.setAlpha(m_bgAlpha->value() * 255 / 100);
    m_style.textBackgroundColor = bg;
    m_style.textAlign = m_align->currentIndex();
    m_style.textX = m_posX->value() / 100.0;
    m_style.textY = m_posY->value() / 100.0;
    QDialog::accept();
}
