// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

// Segunda janela de docks da bancada de editorial (preset "Bancada" com 3+
// telas): um QMainWindow dedicado à tela esquerda, que hospeda os painéis
// técnicos (Mixer, Efeitos, Express, …) como docks de verdade — o usuário
// ainda arrasta, tabifica e redimensiona dentro dela.
//
// A janela não decide o que vai para lá: a MainWindow registra os candidatos
// e recebe o callback `onToggle(dock, dentro)` para mover/remover os docks
// entre as duas janelas. Fechar a janela devolve todos os docks à principal.

#include <QMainWindow>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

class QDockWidget;
class QAction;
class QMenu;

class TechnicalWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit TechnicalWindow(QWidget* parent = nullptr);

    // Lista de painéis que podem morar aqui. O menu "Painéis" mostra todos;
    // marcado = o dock está nesta janela (estado via `noteDockMoved`).
    void setCandidates(const QVector<QPair<QDockWidget*, QString>>& candidates,
                       std::function<void(QDockWidget*, bool)> onToggle);

    // A MainWindow chama depois de mover um dock (para dentro ou fora),
    // mantendo os checks do menu em dia.
    void noteDockMoved(QDockWidget* dock, bool inside);

    // Devolve todos os docks de dentro ao chamador (via onToggle(dock,false)).
    void returnAllDocks();

    bool hasDock(QDockWidget* dock) const { return m_inside.contains(dock); }

signals:
    // Avisos de show/hide para a MainWindow sincronizar o action do menu.
    void visibleChanged(bool visible);

protected:
    // Fechar = devolver os docks e esconder (a janela é reaproveitada pelo
    // preset Bancada; não destruir evita perder os candidatos/conexões).
    void closeEvent(QCloseEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    // Arrastar um dock de outra janela até aqui: o Qt não move docks entre
    // QMainWindows nativamente, então aceitamos o mime interno
    // "application/x-qdockwidget" e pedimos para a MainWindow movê-lo.
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void rebuildMenu();
    QDockWidget* dockByObjectName(const QByteArray& objectName) const;

    QVector<QPair<QDockWidget*, QString>> m_candidates;
    std::function<void(QDockWidget*, bool)> m_onToggle;
    QSet<QDockWidget*> m_inside;
    QMenu* m_panelsMenu = nullptr;
};
