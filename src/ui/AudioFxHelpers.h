// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QString>
#include <QWidget>

#include <cmath>

// Widgets e linhas compartilhados pelos dialogs de efeitos de áudio (do clip e
// da faixa), que têm a mesma cara: sliders de EQ em dB e de intensidade em %.
namespace audiofx {

// Slider de -12..+12 dB que mantém um QLabel sincronizado (com sinal de "+").
inline QSlider* makeDbSlider(QWidget* parent, double v, QLabel** label) {
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(-12, 12);
    s->setValue((int)std::llround(v));
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

// Linha «slider à esquerda, valor à direita».
inline QHBoxLayout* makeRow(QSlider* s, QLabel* l) {
    auto* r = new QHBoxLayout;
    r->addWidget(s, 1);
    r->addWidget(l);
    return r;
}

// Slider 0..100% com tooltip.
inline QSlider* makePct(QWidget* parent, int v, const QString& text) {
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(0, 100);
    s->setValue(v);
    s->setToolTip(text);
    s->setMinimumWidth(120);
    return s;
}

} // namespace audiofx
