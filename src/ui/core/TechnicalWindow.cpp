// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/core/TechnicalWindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include <QSet>
#include <QVariant>

TechnicalWindow::TechnicalWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(tr("Painel técnico"));
    setAttribute(Qt::WA_DeleteOnClose, false); // reutilizada pelo preset
    setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AllowNestedDocks
                   | QMainWindow::AnimatedDocks);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);

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
