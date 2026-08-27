// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/TrimmerDialog.h"

#include "ffmpeg/FFmpegDecoder.h"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {
QString fmtTime(double s) {
    if (s < 0.0) s = 0.0;
    const int t = (int)llround(s * 1000.0);
    return QString("%1:%2.%3")
        .arg(t / 60000, 1, 10, QLatin1Char('0'))
        .arg((t / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(t % 1000, 3, 10, QLatin1Char('0'));
}
} // namespace

TrimmerDialog::TrimmerDialog(const MediaItem& media, QWidget* parent)
    : QDialog(parent), m_media(media) {
    setWindowTitle(tr("Trimmer — %1").arg(media.name));
    setMinimumSize(560, 340);

    m_decoder = new FFmpegDecoder();
    if (!media.filePath.isEmpty())
        m_decoder->open(media.filePath, -1);

    const double dur = qMax(media.duration, 0.0);
    m_out = dur;

    auto* lay = new QVBoxLayout(this);

    // Preview
    m_preview = new QLabel(this);
    m_preview->setMinimumHeight(200);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setStyleSheet(QStringLiteral("background:#000;"));
    lay->addWidget(m_preview, /*stretch=*/1);

    // Slider de posição
    m_seek = new QSlider(Qt::Horizontal, this);
    m_seek->setRange(0, 1000);
    lay->addWidget(m_seek);

    m_timeLbl = new QLabel(this);
    m_timeLbl->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_timeLbl);

    // In/Out
    auto* io = new QHBoxLayout;
    m_spIn = new QDoubleSpinBox(this);
    m_spOut = new QDoubleSpinBox(this);
    for (QDoubleSpinBox* s : {m_spIn, m_spOut}) {
        s->setRange(0.0, dur);
        s->setDecimals(2);
        s->setSingleStep(0.04);
        s->setSuffix(tr(" s"));
    }
    m_spIn->setValue(0.0);
    m_spOut->setValue(dur);
    auto* btnIn = new QPushButton(tr("In aqui (I)"), this);
    auto* btnOut = new QPushButton(tr("Out aqui (O)"), this);
    auto* btnSwap = new QPushButton(tr("Trocar in/out"), this);
    io->addWidget(m_spIn);
    io->addWidget(btnIn);
    io->addWidget(m_spOut);
    io->addWidget(btnOut);
    io->addWidget(btnSwap);
    lay->addLayout(io);

    // Inserir/Cancelar
    auto* btnRow = new QHBoxLayout;
    m_playBtn = new QPushButton(tr("▶ Tocar"), this);
    btnRow->addWidget(m_playBtn);
    btnRow->addStretch();
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    btns->button(QDialogButtonBox::Ok)->setText(tr("Inserir"));
    btnRow->addWidget(btns);
    lay->addLayout(btnRow);

    connect(m_seek, &QSlider::valueChanged, this, &TrimmerDialog::seekChanged);
    connect(m_spIn, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { clampInOut(); });
    connect(m_spOut, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { clampInOut(); });
    connect(btnIn, &QPushButton::clicked, this, &TrimmerDialog::markIn);
    connect(btnOut, &QPushButton::clicked, this, &TrimmerDialog::markOut);
    connect(btnSwap, &QPushButton::clicked, this, [this]() {
        const double tmp = m_spIn->value();
        m_spIn->setValue(m_spOut->value());
        m_spOut->setValue(tmp);
    });
    connect(m_playBtn, &QPushButton::clicked, this, &TrimmerDialog::togglePlay);

    m_throttle = new QTimer(this);
    m_throttle->setSingleShot(true);
    m_throttle->setInterval(120);
    connect(m_throttle, &QTimer::timeout, this, &TrimmerDialog::updatePreview);

    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(33);
    connect(m_playTimer, &QTimer::timeout, this, &TrimmerDialog::playTick);

    updatePreview();
    m_seek->setFocus();
}

double TrimmerDialog::currentPos() const {
    const double dur = qMax(m_media.duration, 0.0);
    return m_seek->value() / 1000.0 * dur;
}

void TrimmerDialog::seekChanged(int) {
    m_timeLbl->setText(QString("%1 / %2")
                           .arg(fmtTime(currentPos()), fmtTime(m_media.duration)));
    m_throttle->start();
}

void TrimmerDialog::clampInOut() {
    const double dur = qMax(m_media.duration, 0.0);
    double in = qBound(0.0, m_spIn->value(), dur);
    double out = qBound(0.0, m_spOut->value(), dur);
    if (in >= out) out = qMin(dur, in + 0.05);
    m_in = in;
    m_out = out;
    if (m_spIn->value() != in) m_spIn->setValue(in);
    if (m_spOut->value() != out) m_spOut->setValue(out);
    m_timeLbl->setText(QString("%1 / %2  ·  trecho %3 → %4 (%5 s)")
                           .arg(fmtTime(currentPos()), fmtTime(m_media.duration),
                                fmtTime(m_in), fmtTime(m_out))
                           .arg(m_out - m_in, 0, 'f', 2));
}

void TrimmerDialog::markIn() {
    const double pos = currentPos();
    if (pos >= m_spOut->value())
        m_spOut->setValue(qMin(qMax(m_media.duration, 0.0), pos + 0.1));
    m_spIn->setValue(pos);
}

void TrimmerDialog::markOut() {
    const double pos = currentPos();
    if (pos <= m_spIn->value())
        m_spIn->setValue(qMax(0.0, pos - 0.1));
    m_spOut->setValue(pos);
}

void TrimmerDialog::togglePlay() {
    m_playing = !m_playing;
    m_playBtn->setText(m_playing ? tr("⏸ Parar") : tr("▶ Tocar"));
    if (m_playing) {
        // Tocando de um ponto antes do out: avança até out e para.
        if (currentPos() >= m_out) m_seek->setValue(int(m_in / qMax(m_media.duration, 1e-6) * 1000.0));
        m_playTimer->start();
    } else {
        m_playTimer->stop();
    }
}

void TrimmerDialog::playTick() {
    const double dur = qMax(m_media.duration, 0.0);
    const double step = dur / 1000.0;
    double pos = currentPos() + step;
    if (pos >= m_out) {
        pos = m_in;
        m_playing = false;
        m_playBtn->setText(tr("▶ Tocar"));
        m_playTimer->stop();
    }
    m_seek->setValue(int(pos / dur * 1000.0));
}

void TrimmerDialog::updatePreview() {
    if (!m_preview) return;
    QImage img;
    if (m_decoder && !m_media.filePath.isEmpty()) {
        img = m_decoder->frameAt(currentPos(), 640);
        // frameAt não retorna imagem quando o decode falha (arquivo
        // corrompido/stream sem vídeo decodável): mantém o fundo preto.
    }
    if (!img.isNull()) {
        // Converte para o formato "display" (RGB) para o QLabel; frameAt já
        // respeita a proporção.
        m_preview->setPixmap(QPixmap::fromImage(img));
    } else {
        m_preview->setText(tr("(sem quadro)"));
    }
}

void TrimmerDialog::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_I) { markIn(); e->accept(); return; }
    if (e->key() == Qt::Key_O) { markOut(); e->accept(); return; }
    if (e->key() == Qt::Key_Space) { togglePlay(); e->accept(); return; }
    QDialog::keyPressEvent(e);
}