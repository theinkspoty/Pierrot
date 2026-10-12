// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "AudioEffectsDialog.h"
#include "AudioFxHelpers.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QCheckBox>
#include <QDialogButtonBox>

#include <cmath>

using namespace audiofx;

AudioEffectsDialog::AudioEffectsDialog(Clip* clip, QWidget* parent)
    : QDialog(parent), m_clip(clip) {
    setWindowTitle(tr("Efeitos de áudio"));
    setMinimumWidth(460);

    m_low = makeDbSlider(this, m_clip->eqLow, &m_lowLabel);
    m_mid = makeDbSlider(this, m_clip->eqMid, &m_midLabel);
    m_high = makeDbSlider(this, m_clip->eqHigh, &m_highLabel);
    auto* lowRow = makeRow(m_low, m_lowLabel);
    auto* midRow = makeRow(m_mid, m_midLabel);
    auto* highRow = makeRow(m_high, m_highLabel);

    m_denoise = new QCheckBox(tr("Redução de ruído"), this);
    m_denoise->setChecked(m_clip->denoise);
    m_denoiseAmt = new QSlider(Qt::Horizontal, this);
    m_denoiseAmt->setRange(1, 50);
    m_denoiseAmt->setValue((int)llround(m_clip->denoiseAmount));
    m_denoiseAmt->setEnabled(m_clip->denoise);
    connect(m_denoise, &QCheckBox::toggled, m_denoiseAmt, &QSlider::setEnabled);
    m_denoiseAmtLabel = new QLabel(QString("%1 dB").arg(m_denoiseAmt->value()), this);
    connect(m_denoiseAmt, &QSlider::valueChanged, this, [this](int v) {
        m_denoiseAmtLabel->setText(QString("%1 dB").arg(v));
    });
    m_denoiseAmtLabel->setMinimumWidth(56);
    m_denoiseAmtLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* denoiseRow = makeRow(m_denoiseAmt, m_denoiseAmtLabel);

    m_normalize = new QCheckBox(tr("Normalizar (-14 LUFS)"), this);
    m_normalize->setChecked(m_clip->normalize);

    m_invert = new QCheckBox(tr("Inverter fase"), this);
    m_invert->setChecked(m_clip->invertPhase);

    auto* form = new QFormLayout;
    form->addRow(tr("Graves (120 Hz):"), lowRow);
    form->addRow(tr("Médios (1 kHz):"), midRow);
    form->addRow(tr("Agudos (6 kHz):"), highRow);
    form->addRow(m_denoise);
    form->addRow(tr("Intensidade:"), denoiseRow);
    form->addRow(m_normalize);
    form->addRow(m_invert);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(buttons);
}

void AudioEffectsDialog::accept() {
    m_clip->eqLow = m_low->value();
    m_clip->eqMid = m_mid->value();
    m_clip->eqHigh = m_high->value();
    m_clip->denoise = m_denoise->isChecked();
    m_clip->denoiseAmount = m_denoiseAmt->value();
    m_clip->normalize = m_normalize->isChecked();
    m_clip->invertPhase = m_invert->isChecked();
    QDialog::accept();
}
