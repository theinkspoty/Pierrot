// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/core/TechnicalWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHideEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QSet>
#include <QShowEvent>
#include <QVariant>

// Mime interno do Qt para o arrasto de um QDockWidget (usado pelo mecanismo
// interno de docking). O payload é o objectName do dock.
static const char kDockDragMime[] = "application/x-qdockwidget";

TechnicalWindow::TechnicalWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(tr("Painel técnico"));
    setAttribute(Qt::WA_DeleteOnClose, false); // reutilizada pelo preset
    setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks
                   | QMainWindow::AnimatedDocks);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    // Precisa aceitar drops para receber o drag de docks vindos da janela
    // principal (o Qt não move docks entre QMainWindows sozinho).
    setAcceptDrops(true);

    m_panelsMenu = menuBar()->addMenu(tr("Painéis"));
}

void TechnicalWindow::setCandidates(
    const QVector<QPair<QDockWidget*, QString>>& candidates,
    std::function<void(QDockWidget*, bool)> onToggle) {
    m_candidates = candidates;
    m_onToggle = std::move(onToggle);
    rebuildMenu();
}

void TechnicalWindow::noteDockMoved(QDockWidget* dock, bool inside) {
    if (!dock) return;
    if (inside) m_inside.insert(dock);
    else m_inside.remove(dock);
    if (m_panelsMenu) {
        for (QAction* a : m_panelsMenu->actions()) {
            if (a->data().value<QDockWidget*>() == dock)
                a->setChecked(inside);
        }
    }
}

void TechnicalWindow::returnAllDocks() {
    const QList<QDockWidget*> inside = m_inside.values();
    for (QDockWidget* d : inside) {
        if (m_onToggle) m_onToggle(d, false);
        noteDockMoved(d, false);
    }
}

void TechnicalWindow::closeEvent(QCloseEvent* e) {
    returnAllDocks();
    hide();
    e->ignore();
}

void TechnicalWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    emit visibleChanged(true);
}

void TechnicalWindow::hideEvent(QHideEvent* e) {
    QMainWindow::hideEvent(e);
    emit visibleChanged(false);
}

QDockWidget* TechnicalWindow::dockByObjectName(const QByteArray& objectName) const {
    const QString n = QString::fromUtf8(objectName);
    for (const auto& cand : m_candidates) {
        if (cand.first && cand.first->objectName() == n) return cand.first;
    }
    return nullptr;
}

void TechnicalWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (!e->mimeData()->hasFormat(kDockDragMime)) return;
    QDockWidget* d = dockByObjectName(e->mimeData()->data(kDockDragMime));
    // Só aceita docks que moram em OUTRA janela — drags internos (reabas,
    // redock) são do mecanismo próprio do Qt e não devem ser interceptados.
    if (d && !m_inside.contains(d)) e->acceptProposedAction();
}

void TechnicalWindow::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData()->hasFormat(kDockDragMime)) e->acceptProposedAction();
}

void TechnicalWindow::dropEvent(QDropEvent* e) {
    if (!e->mimeData()->hasFormat(kDockDragMime)) return;
    QDockWidget* d = dockByObjectName(e->mimeData()->data(kDockDragMime));
    if (!d || m_inside.contains(d)) return;
    if (m_onToggle) m_onToggle(d, true);
    noteDockMoved(d, true);
    e->acceptProposedAction();
}

void TechnicalWindow::rebuildMenu() {
    m_panelsMenu->clear();
    for (const auto& cand : m_candidates) {
        QDockWidget* dock = cand.first;
        if (!dock) continue;
        QAction* act = m_panelsMenu->addAction(cand.second);
        act->setCheckable(true);
        act->setChecked(m_inside.contains(dock));
        act->setData(QVariant::fromValue(dock));
        connect(act, &QAction::toggled, this, [this, dock](bool on) {
            if (m_onToggle) m_onToggle(dock, on);
            noteDockMoved(dock, on);
        });
    }
}
