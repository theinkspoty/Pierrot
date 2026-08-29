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
#include "ui/Theme.h"
#include "Bench.h"
#include "CrashReporter.h"
#include <QMessageBox>
#include <QFileInfo>
#include <QTextStream>
#include <QPushButton>
#include <QProcess>
#include <QFile>

int main(int argc, char** argv) {
    // Instala o relatório de crash O MAIS CEDO possível: se o app fechar de
    // repente (SIGSEGV/SIGABRT/etc.), grava backtrace e infos em ~/Pierrot-crash-*.txt.
    CrashReporter::install();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QApplication app(argc, argv);
    app.setOrganizationName("Pierrot");
    app.setApplicationName("Pierrot");
    app.setApplicationDisplayName(QString()); // QApplication anexaria " - Pierrot" a toda janela
    app.setWindowIcon(QIcon(QStringLiteral(":/pierrot.ico")));
    // Fechar a janela de boas-vindas (única janela no início) não pode
    // encerrar o app; o editor deve assumir em seguida.
    app.setQuitOnLastWindowClosed(false);

    ClickLogger::install();

    app.setStyle(QStyleFactory::create("Fusion"));
    applyAppPalette(&app, savedTheme());

    // Se houve um crash na última execução, avisa o usuário e mostra onde está
    // o relatório (com opção de abrir o arquivo e de remover os antigos).
    const QStringList reports = CrashReporter::existingReports();
    if (!reports.isEmpty()) {
        QMessageBox box(QMessageBox::Warning,
                        QObject::tr("Pierrot fechou inesperadamente"),
                        QObject::tr("Na última execução o Pierrot fechou de repente "
                                    "(possível falha).\n\nUm relatório de crash foi "
                                    "gerado com detalhes técnicos:\n%1")
                            .arg(reports.join(QLatin1Char('\n'))),
                        QMessageBox::Ok, nullptr);
        QPushButton* openBtn = box.addButton(QObject::tr("Abrir relatório"),
                                             QMessageBox::ActionRole);
        box.addButton(QObject::tr("Limpar relatórios"), QMessageBox::ActionRole);
        box.exec();
        if (box.clickedButton() == openBtn && !reports.isEmpty()) {
            const QString p = reports.last();
            QProcess::startDetached(QStringLiteral("xdg-open"), QStringList{p});
        } else if (box.clickedButton() && box.clickedButton() != openBtn) {
            for (const QString& p : reports) QFile::remove(p);
        }
    }

    app.setStyle(QStyleFactory::create("Fusion"));
    applyAppPalette(&app, savedTheme());
    app.setStyleSheet(flatControlStyleSheet(savedTheme()));

    // Harness de stress (sem GUI):
    //   pierrot --bench <projeto.pjrt>            → mede um projeto existente
    //   pierrot --stress <saida.pjrt> <mídia…>    → gera projeto com 200 cortes
    //     a partir da mídia dada e mede na hora (valida 200 cortes/4K em 1 passo)
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            return runBench(app, QString::fromLocal8Bit(argv[i + 1]));
        }
        if (qstrcmp(argv[i], "--stress") == 0 && i + 2 < argc) {
            QStringList media;
            for (int j = i + 2; j < argc; ++j) media.append(QString::fromLocal8Bit(argv[j]));
            return runStress(app, QString::fromLocal8Bit(argv[i + 1]), media);
        }
    }

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
