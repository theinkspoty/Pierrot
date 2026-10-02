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
#include "colombina/CrashReporter.h"
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
    CrashReporter::registerThread("ui-principal");
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

    // Harness de reprodução (A/B proxy vs original, sem interação):
    //   pierrot --autoplay <projeto.Blanc> [--seconds=20] [--from=0]
    //          [--proxy|--no-proxy] [--warmup=3] [--wait-proxy] [--json=/tmp/x.json]
    // Abre o projeto, reproduz, grava o relatório de métricas e sai sozinho.
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--autoplay") == 0 && i + 1 < argc) {
            const QString project = QString::fromLocal8Bit(argv[i + 1]);
            double seconds = 20.0;
            double from = 0.0;
            double warmup = 3.0;
            int useProxies = -1; // -1 = respeita o projeto
            bool waitProxy = false;
            QString json = QStringLiteral("/tmp/pierrot-autoplay.json");
            for (int j = i + 2; j < argc; ++j) {
                const QByteArray a = argv[j];
                auto num = [&](const char* key, double def) {
                    const QByteArray p = QByteArray("--") + key + "=";
                    if (a.startsWith(p)) return a.mid(p.size()).toDouble();
                    return def;
                };
                if (a == "--proxy") useProxies = 1;
                else if (a == "--no-proxy") useProxies = 0;
                else if (a == "--wait-proxy") waitProxy = true;
                else if (a.startsWith("--json=")) json = QString::fromLocal8Bit(a.mid(7));
                else if (a.startsWith("--seconds=")) seconds = num("seconds", 20.0);
                else if (a.startsWith("--from=")) from = num("from", 0.0);
                else if (a.startsWith("--warmup=")) warmup = num("warmup", 3.0);
            }
            // O profiler é o consumidor; sem estas env vars ele nem registra.
            qputenv("PIERROT_PERF_JSON", json.toUtf8());
            MainWindow w;
            w.openProjectFile(project);
            w.show();
            w.autoplay(seconds, useProxies, from, warmup, waitProxy, json);
            return app.exec();
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
