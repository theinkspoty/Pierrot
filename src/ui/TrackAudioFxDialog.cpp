// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TrackAudioFxDialog.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QCheckBox>
#include <QDialogButtonBox>

#include <algorithm>
#include <cmath>

namespace {

QSlider* makeDbSlider(QWidget* parent, double v, QLabel** label) {
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(-12, 12);
    s->setValue((int)llround(v));
    *label = new QLabel(parent);
    QObject::connect(s, &QSlider::valueChanged, parent, [label](int val) {
        QString t = QString("%1 dB").arg(val);
        if (val > 0) t.prepend('+');
        (*label)->setText(t);
    });
    QString t = QString("%1 dB").arg(s->value());
    if (s->value() > 0) t.prepend('+');
    (*label)->setText(t);
    (*label)->setMinimumWidth(56);
    (*label)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return s;
}

QHBoxLayout* makeRow(QSlider* s, QLabel* l) {
    auto* r = new QHBoxLayout;
    r->addWidget(s, 1);
    r->addWidget(l);
    return r;
}

QSlider* makePct(QWidget* parent, int v, const QString& text) {
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(0, 100);
    s->setValue(v);
    s->setToolTip(text);
    s->setMinimumWidth(120);
    return s;
}

} // namespace

TrackAudioFxDialog::TrackAudioFxDialog(Track* track, QWidget* parent)
    : QDialog(parent), m_track(track) {
    setWindowTitle(tr("Efeitos de áudio da faixa"));
    setMinimumWidth(480);

    m_low = makeDbSlider(this, m_track->eqLow, &m_lowLabel);
    m_mid = makeDbSlider(this, m_track->eqMid, &m_midLabel);
    m_high = makeDbSlider(this, m_track->eqHigh, &m_highLabel);
    auto* lowRow = makeRow(m_low, m_lowLabel);
    auto* midRow = makeRow(m_mid, m_midLabel);
    auto* highRow = makeRow(m_high, m_highLabel);

    m_denoise = new QCheckBox(tr("Redução de ruído"), this);
    m_denoise->setChecked(m_track->denoise);
    m_denoiseAmt = new QSlider(Qt::Horizontal, this);
    m_denoiseAmt->setRange(1, 50);
    m_denoiseAmt->setValue((int)llround(m_track->denoiseAmount));
    m_denoiseAmt->setEnabled(m_track->denoise);
    connect(m_denoise, &QCheckBox::toggled, m_denoiseAmt, &QSlider::setEnabled);
    m_denoiseAmtLabel = new QLabel(QString("%1 dB").arg(m_denoiseAmt->value()), this);
    connect(m_denoiseAmt, &QSlider::valueChanged, this, [this](int v) {
        m_denoiseAmtLabel->setText(QString("%1 dB").arg(v));
    });
    m_denoiseAmtLabel->setMinimumWidth(56);
    m_denoiseAmtLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* denoiseRow = makeRow(m_denoiseAmt, m_denoiseAmtLabel);

    m_invert = new QCheckBox(tr("Inverter fase"), this);
    m_invert->setChecked(m_track->invertPhase);

    m_reverb = new QCheckBox(tr("Reverb EX"), this);
    m_reverb->setChecked(m_track->reverb);
    m_reverbMix = makePct(this, (int)llround(m_track->reverbMix * 100.0),
                          tr("Mistura úmida (0–100%)"));
    m_reverbSize = makePct(this, (int)llround(m_track->reverbSize * 100.0),
                           tr("Tamanho da sala (0–100%)"));
    m_reverbMix->setEnabled(m_track->reverb);
    m_reverbSize->setEnabled(m_track->reverb);
    connect(m_reverb, &QCheckBox::toggled, this, [this](bool on) {
        m_reverbMix->setEnabled(on);
        m_reverbSize->setEnabled(on);
    });

    auto* form = new QFormLayout;
    form->addRow(tr("Graves (120 Hz):"), lowRow);
    form->addRow(tr("Médios (1 kHz):"), midRow);
    form->addRow(tr("Agudos (6 kHz):"), highRow);
    form->addRow(m_denoise);
    form->addRow(tr("Intensidade:"), denoiseRow);
    form->addRow(m_invert);
    form->addRow(m_reverb);
    form->addRow(tr("Mix (úmido):"), m_reverbMix);
    form->addRow(tr("Sala:"), m_reverbSize);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(buttons);
}

void TrackAudioFxDialog::accept() {
    m_track->eqLow = m_low->value();
    m_track->eqMid = m_mid->value();
    m_track->eqHigh = m_high->value();
    m_track->denoise = m_denoise->isChecked();
    m_track->denoiseAmount = m_denoiseAmt->value();
    m_track->invertPhase = m_invert->isChecked();
    m_track->reverb = m_reverb->isChecked();
    m_track->reverbMix = m_reverbMix->value() / 100.0;
    m_track->reverbSize = m_reverbSize->value() / 100.0;
    if (!m_track->reverb) m_track->reverbMix = 0.35;
    QDialog::accept();
}