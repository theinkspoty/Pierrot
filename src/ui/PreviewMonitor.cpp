// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/PreviewMonitor.h"

#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>

#include <cmath>

PreviewMonitor::PreviewMonitor(QWidget* parent) : QWidget(parent) {
    setWindowTitle(tr("Preview externo — Pierrot"));
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(320, 180);
    setStyleSheet(QStringLiteral("background:#000;"));
}

void PreviewMonitor::setFrame(const QImage& img) {
    if (img.isNull()) return;
    if (m_frame.constBits() == img.constBits()
        && m_frame.size() == img.size())
        return; // mesmo quadro compartilhado: nada mudou
    m_frame = img;
    update();
}

void PreviewMonitor::clear() {
    if (m_frame.isNull()) return;
    m_frame = QImage();
    update();
}

void PreviewMonitor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0));
    if (m_frame.isNull() || m_frame.width() < 2 || m_frame.height() < 2) {
        p.setPen(QColor(96, 96, 96));
        p.drawText(rect(), Qt::AlignCenter, tr("Sem quadro"));
        return;
    }
    const QRect work = rect().adjusted(8, 8, -8, -8);
    const double k = qMin((double)work.width() / m_frame.width(),
                          (double)work.height() / m_frame.height());
    const int w = qMax(1, (int)std::lround(m_frame.width() * k));
    const int h = qMax(1, (int)std::lround(m_frame.height() * k));
    QRect target(QPoint(0, 0), QSize(w, h));
    target.moveCenter(work.center());
    p.drawImage(target, m_frame);
}

void PreviewMonitor::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_F11 || e->key() == Qt::Key_F) {
        setFullScreen(!m_fullScreen);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Escape && m_fullScreen) {
        setFullScreen(false);
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void PreviewMonitor::mouseDoubleClickEvent(QMouseEvent* e) {
    setFullScreen(!m_fullScreen);
    e->accept();
}

void PreviewMonitor::setFullScreen(bool fs) {
    m_fullScreen = fs;
    if (fs)
        showFullScreen();
    else
        showNormal();
}