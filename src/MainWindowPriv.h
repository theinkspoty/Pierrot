// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Helpers compartilhados entre os vários .cpp de MainWindow (o arquivo é
// fragmentado por responsabilidade: Workspace, Settings, Docks, Actions,
// Undo, Projects). Viviam como static/anon-namespace no .cpp único — com o
// corte eles atravessariam unidades de tradução, então viram aqui.

#pragma once

#include <QByteArray>
#include <QDataStream>

// Versão do arranjo de painéis (docks/toolbar). Aumente para descartar
// estados salvos antigos que estejam com o layout deslocado.
// 4: o Histórico saiu da pilha do Mixer e os Analisadores foram para a pilha
//    da Mesa. Sem este bump, um layout salvo com a pilha de 3 abas embaixo
//    continuaria sendo restaurado e a correção não apareceria.
// 5: o preview saiu de setCentralWidget e virou o dock "Program Monitor"
//    (passo 1 da réplica estrutural do Premiere, ROADMAP 7.1). Precisa do bump
//    porque os layouts antigos não conhecem o dock previewDock e o
//    restoreState() reposicionaria o vídeo numa área que não existe mais.
// 6: revertido — o preview voltou a ser widget central (a premissa do passo 1
//    estava errada: QMainWindow aceita docks em volta do central), e o dock
//    previewDock deixou de existir. Bump para descartar layouts da v5 que
//    ainda referenciam o dock fantasma.
// 8: o dock "Motion Script" (editor de animação por texto) entrou na pilha
//    direita. Bump para que layouts antigos não o mantenham escondido.
constexpr int kLayoutVersion = 8;

// QMainWindow::saveState sempre começa pelo magic 0xff; qualquer outra coisa
// é estado corrompido ou gravado por outra versão do app.
inline bool saneLayoutArray(const QByteArray& state) {
    QDataStream in(state);
    in.setVersion(QDataStream::Qt_4_0);
    quint32 magic;
    in >> magic;
    return in.status() == QDataStream::Ok && magic == 0xff;
}
