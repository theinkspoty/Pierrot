// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/motion/GraphEditorWidget.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/core/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QPushButton>
#include <QToolButton>
#include <QButtonGroup>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QFrame>
#include <QPixmap>
#include <QIcon>
#include <QFont>
#include <algorithm>
#include <cmath>
#include <functional>

#include "ui/motion/GraphEditorPriv.h"

// --------------------------------------------------------------------------
// GraphPropRow
// --------------------------------------------------------------------------

GraphPropRow::GraphPropRow(GraphProp p, QWidget* parent) : QFrame(parent), m_prop(p) {
    setObjectName(QStringLiteral("propRow"));
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_expand = new QToolButton(this);
    m_expand->setAutoRaise(true);
    m_expand->setCheckable(true);
    m_expand->setText(QStringLiteral("▸"));
    m_expand->setCursor(Qt::PointingHandCursor);
    m_expand->setToolTip(tr("Mostrar/ocultar linha de keyframes"));

    m_name = new QLabel(propName(p), this);
    m_name->setAttribute(Qt::WA_TransparentForMouseEvents);

    m_value = new QLabel(this);
    m_value->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_value->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(themeColors().graphLabel.name()));

    m_stopwatch = new QToolButton(this);
    m_stopwatch->setAutoRaise(true);
    m_stopwatch->setCheckable(true);
    m_stopwatch->setIcon(makeStopwatchIcon(false));
    m_stopwatch->setCursor(Qt::PointingHandCursor);
    m_stopwatch->setToolTip(tr("Alternar animação"));

    m_prev = new QToolButton(this);
    m_prev->setAutoRaise(true);
    m_prev->setText(QStringLiteral("◀"));
    m_prev->setCursor(Qt::PointingHandCursor);
    m_prev->setToolTip(tr("Ir para o keyframe anterior"));

    m_add = new QToolButton(this);
    m_add->setAutoRaise(true);
    m_add->setText(QStringLiteral("◆"));
    m_add->setCursor(Qt::PointingHandCursor);
    m_add->setToolTip(tr("Adicionar/remover keyframe no playhead"));

    m_next = new QToolButton(this);
    m_next->setAutoRaise(true);
    m_next->setText(QStringLiteral("▶"));
    m_next->setCursor(Qt::PointingHandCursor);
    m_next->setToolTip(tr("Ir para o próximo keyframe"));

    // Botões planos do Effect Controls: sem borda, hover sutil.
    const QString flatBtn = QStringLiteral(
        "QToolButton { background:transparent; border:none; padding:2px;"
        " color:%1; font-size:11px; border-radius:3px; }"
        "QToolButton:hover { background:%2; }")
        .arg(themeColors().iconNormal.name(), themeColors().btnHover.name());
    m_expand->setStyleSheet(flatBtn);
    m_stopwatch->setStyleSheet(flatBtn);
    m_prev->setStyleSheet(flatBtn);
    m_add->setStyleSheet(flatBtn);
    m_next->setStyleSheet(flatBtn);

    m_navBox = new QWidget(this);
    auto* navLay = new QHBoxLayout(m_navBox);
    navLay->setContentsMargins(0, 0, 0, 0);
    navLay->setSpacing(0);
    navLay->addWidget(m_prev);
    navLay->addWidget(m_add);
    navLay->addWidget(m_next);
    m_navBox->setVisible(false);

    m_strip = new KeyframeStrip(this);
    m_strip->setVisible(false);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(2, 1, 2, 1);
    topRow->setSpacing(2);
    topRow->addWidget(m_expand);
    topRow->addWidget(m_name);
    topRow->addStretch(1);
    topRow->addWidget(m_value);
    topRow->addWidget(m_stopwatch);
    topRow->addWidget(m_navBox);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(topRow);
    lay->addWidget(m_strip);

    connect(m_expand, &QToolButton::toggled, this, [this](bool on) {
        m_strip->setVisible(on);
        updateExpandText();
    });
    connect(m_stopwatch, &QToolButton::toggled, this, [this](bool on) {
        m_stopwatch->setIcon(makeStopwatchIcon(on));
        emit stopwatchClicked();
    });
    connect(m_prev, &QToolButton::clicked, this, &GraphPropRow::prevKeyframe);
    connect(m_next, &QToolButton::clicked, this, &GraphPropRow::nextKeyframe);
    connect(m_add, &QToolButton::clicked, this, &GraphPropRow::toggleKeyframe);
    connect(m_strip, &KeyframeStrip::keyClicked, this, &GraphPropRow::stripKeyClicked);
    connect(m_strip, &KeyframeStrip::dragStart, this, &GraphPropRow::stripDragStart);
    connect(m_strip, &KeyframeStrip::dragKey, this, &GraphPropRow::stripDragKey);
    connect(m_strip, &KeyframeStrip::addKey, this, &GraphPropRow::stripAddKey);
}

void GraphPropRow::updateExpandText() {
    m_expand->setText(m_expand->isChecked() ? QStringLiteral("▾") : QStringLiteral("▸"));
}

void GraphPropRow::setAnimated(bool on) {
    const QSignalBlocker b(m_stopwatch);
    m_stopwatch->setChecked(on);
    m_stopwatch->setIcon(makeStopwatchIcon(on));
    m_navBox->setVisible(on);
}

void GraphPropRow::setActive(bool on) {
    if (on) {
        QColor sel = themeColors().accent;
        sel.setAlpha(28);
        setStyleSheet(QStringLiteral(
            "#propRow { background:%1; border-left:2px solid %2; }")
            .arg(sel.name(QColor::HexArgb),
                 themeColors().accent.name()));
    } else {
        setStyleSheet(QStringLiteral(
            "#propRow { background:transparent; border-left:2px solid transparent; }"));
    }
}

void GraphPropRow::setValueText(const QString& s) {
    m_value->setText(s);
}

void GraphPropRow::setStripData(Clip* clip, double fps) {
    m_strip->setData(clip, m_prop, fps);
}

void GraphPropRow::setStripMesaData(Track* track, double fps) {
    m_strip->setMesaData(track, m_prop, fps);
}

void GraphPropRow::setStripCamData(MesaComposition* mc, double fps) {
    m_strip->setCamData(mc, m_prop, fps);
}

void GraphPropRow::setStripMaskData(Clip* clip, int maskIndex, double fps) {
    m_strip->setMaskData(clip, maskIndex, m_prop, fps);
}

void GraphPropRow::setStripPlayhead(double t) {
    m_strip->setPlayhead(t);
}

void GraphPropRow::setExpanded(bool on) {
    const QSignalBlocker b(m_expand);
    m_expand->setChecked(on);
    m_strip->setVisible(on);
    updateExpandText();
}

void GraphPropRow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) emit propertyClicked();
    QFrame::mousePressEvent(e);
}

