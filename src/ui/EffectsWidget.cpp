// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "EffectsWidget.h"

#include <QVBoxLayout>
#include <QLabel>

EffectsWidget::EffectsWidget(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    auto* hint = new QLabel(tr("Painel de efeitos (em construção)"), this);
    hint->setAlignment(Qt::AlignCenter);
    hint->setStyleSheet(QStringLiteral("color:#5f6772; font-size:12px;"));
    lay->addWidget(hint);
    lay->addStretch(1);
}
