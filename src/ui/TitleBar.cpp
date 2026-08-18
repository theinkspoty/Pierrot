// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TitleBar.h"

#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QStyle>
#include <QWindow>

// Inicia o movimento da janela pelo compositor, no Wayland (onde move() é
// ignorado). Usa QWindow::startSystemMove(), disponível desde o Qt 5.15 — o
// QWidget::startSystemMove só existe no Qt 6.5+, então operamos no QWindow.
static bool startMoveWindow(QWidget* w) {
    if (!w) return false;
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    if (QWindow* wh = w->windowHandle())
        return wh->startSystemMove();
#endif
    return false;
}

TitleBar::TitleBar(const QString& title, QWidget* window, QWidget* parent)
    : QWidget(parent), m_window(window) {
    setFixedHeight(38);
    // Cantos superiores arredondados para acompanhar a janela arredondada.
    setStyleSheet(QStringLiteral(
        "TitleBar{background:#22242a; border-bottom:1px solid #2a2d34;"
        " border-top-left-radius:18px; border-top-right-radius:18px;}"
        "QLabel{color:#c9cdd4; font-size:13px; font-weight:bold;}"));
    setCursor(Qt::ArrowCursor);

    m_title = new QLabel(title, this);
    m_title->setStyleSheet(QStringLiteral("padding:0;"));
    m_title->setCursor(Qt::ArrowCursor);

    m_minBtn = new QPushButton(tr("—"), this);
    m_maxBtn = new QPushButton(tr("□"), this);
    m_closeBtn = new QPushButton(tr("✕"), this);
    const QString btnStyle = QStringLiteral(
        "QPushButton{border:none; background:transparent; color:#8a919c;"
        " font-size:13px; padding:0 13px; height:38px;}"
        "QPushButton:hover{background:#2b2e37; color:#e0e4ea;}"
        "QPushButton:pressed{background:#23252c;}");
    m_minBtn->setStyleSheet(btnStyle);
    m_maxBtn->setStyleSheet(btnStyle);
    m_closeBtn->setStyleSheet(btnStyle +
        QStringLiteral("QPushButton:hover{background:#c0392b; color:#fff;}"));
    m_closeBtn->setToolTip(tr("Fechar"));

    m_minBtn->setCursor(Qt::PointingHandCursor);
    m_maxBtn->setCursor(Qt::PointingHandCursor);
    m_closeBtn->setCursor(Qt::PointingHandCursor);

    // Botões à esquerda: fechar, minimizar, maximizar; título centralizado.
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_closeBtn);
    lay->addWidget(m_minBtn);
    lay->addWidget(m_maxBtn);
    lay->addWidget(m_title, 1);
    m_title->setAlignment(Qt::AlignCenter);

    connect(m_minBtn, &QPushButton::clicked, this, [this]() {
        m_window->showMinimized();
    });
    connect(m_maxBtn, &QPushButton::clicked, this, &TitleBar::maximizeRestore);
    connect(m_closeBtn, &QPushButton::clicked, m_window, &QWidget::close);

    // Um filtro de eventos captura o arrasto/duplo clique sobre o QLabel do
    // título (que senão engole os eventos de mouse e impede de mover a janela).
    m_title->installEventFilter(this);
}

bool TitleBar::eventFilter(QObject* obj, QEvent* ev) {
    if (obj == m_title) {
        if (ev->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) {
                // No Wayland o compositor inicia o movimento do app.
                if (startMoveWindow(m_window)) {
                    m_dragging = false;
                    return true;
                }
                m_dragging = true;
                m_dragOffset = me->globalPosition().toPoint()
                               - m_window->frameGeometry().topLeft();
                return true;
            }
        } else if (ev->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (m_dragging && (me->buttons() & Qt::LeftButton)) {
                m_window->move(me->globalPosition().toPoint() - m_dragOffset);
                return true;
            }
        } else if (ev->type() == QEvent::MouseButtonRelease) {
            m_dragging = false;
            return true;
        } else if (ev->type() == QEvent::MouseButtonDblClick) {
            auto* me = static_cast<QMouseEvent*>(ev);
            if (me->button() == Qt::LeftButton) {
                maximizeRestore();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void TitleBar::setTitle(const QString& title) {
    if (m_title) m_title->setText(title);
}

void TitleBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        // No Wayland o compositor inicia o movimento do app.
        if (startMoveWindow(m_window)) {
            m_dragging = false;
            e->accept();
            return;
        }
        m_dragging = true;
        m_dragOffset = e->globalPosition().toPoint() - m_window->frameGeometry().topLeft();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void TitleBar::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragging && (e->buttons() & Qt::LeftButton)) {
        m_window->move(e->globalPosition().toPoint() - m_dragOffset);
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        maximizeRestore();
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void TitleBar::mouseReleaseEvent(QMouseEvent* e) {
    m_dragging = false;
    QWidget::mouseReleaseEvent(e);
}

void TitleBar::maximizeRestore() {
    if (!m_window) return;
    if (m_window->isMaximized())
        m_window->showNormal();
    else
        m_window->showMaximized();
}
