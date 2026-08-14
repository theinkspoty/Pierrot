// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include <QApplication>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>
#include <QDialog>
#include "MainWindow.h"
#include "ui/WelcomeWindow.h"
#include "ui/ClickLogger.h"

int main(int argc, char** argv) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QApplication app(argc, argv);
    app.setOrganizationName("Pierrot");
    app.setApplicationName("Pierrot");
    app.setApplicationDisplayName("Pierrot");
    app.setWindowIcon(QIcon(QStringLiteral(":/pierrot.ico")));
    // Fechar a janela de boas-vindas (única janela no início) não pode
    // encerrar o app; o editor deve assumir em seguida.
    app.setQuitOnLastWindowClosed(false);

    ClickLogger::install();

    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette pal;
    pal.setColor(QPalette::Window, QColor(37, 37, 38));
    pal.setColor(QPalette::WindowText, QColor(220, 220, 220));
    pal.setColor(QPalette::Base, QColor(30, 30, 31));
    pal.setColor(QPalette::AlternateBase, QColor(40, 40, 42));
    pal.setColor(QPalette::Text, QColor(220, 220, 220));
    pal.setColor(QPalette::Button, QColor(60, 60, 62));
    pal.setColor(QPalette::ButtonText, QColor(230, 230, 230));
    pal.setColor(QPalette::BrightText, Qt::red);
    pal.setColor(QPalette::Link, QColor(80, 160, 255));
    pal.setColor(QPalette::Highlight, QColor(0, 120, 215));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::ToolTipBase, QColor(60, 60, 62));
    pal.setColor(QPalette::ToolTipText, QColor(230, 230, 230));
    pal.setColor(QPalette::PlaceholderText, QColor(120, 120, 125));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(100, 100, 105));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(100, 100, 105));
    app.setPalette(pal);

    // O editor é criado somente após a janela de boas-vindas, como no fluxo
    // original. Fechar a boas-vindas (X) encerra o exec() com Rejected e abre
    // o editor vazio; criar/abrir projeto carrega o projeto nele.
    WelcomeWindow welcome;
    if (welcome.exec() == QDialog::Accepted) {
        MainWindow w;
        if (!welcome.projectPath().isEmpty()) {
            w.openProjectFile(welcome.projectPath());
        } else if (welcome.newProjectRequested()) {
            w.createProject(welcome.projectWidth(), welcome.projectHeight(),
                            welcome.projectFps(), welcome.projectName());
        }
        w.show();
        return app.exec();
    }

    MainWindow w;
    w.show();
    return app.exec();
}
