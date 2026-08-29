// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>
#include <QProcess>
#include <QVector>
#include "export/ProjectExporter.h"

class QListWidget;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QThread;
class ExportBuildWorker;

// Fila de render: várias exportações (formatos/resoluções/fps diferentes)
// executadas em SEQUÊNCIA por um único ffmpeg por vez. Cada item é adicionado
// selecionando as configurações no ExportDialog (modo de configuração).
// A MONTAGEM do comando (que hoje inclui o pré-render das bandas Mesa) roda em
// THREAD separada com progresso e cancelamento — não congela a UI em projetos
// com composições 2D longas.
class RenderQueueDialog : public QDialog {
    Q_OBJECT
public:
    explicit RenderQueueDialog(Project* project, QWidget* parent = nullptr);
    ~RenderQueueDialog() override;
private slots:
    void addJob();
    void removeSelected();
    void moveSelected(int dir);
    void startQueue();
    void onReadyRead();
    void onJobFinished(int exitCode, QProcess::ExitStatus);
    void onCommandBuilt(const QStringList& args);
    void onBuildFailed(const QString& err);
private:
    void refreshList();
    void startNextJob();
    void log(const QString& line);
    void setBusy(bool busy);
    void finishBuildThread();
    Project* m_project = nullptr;
    Project m_projectSnap;               // cópia isolada para a thread de build
    QVector<ExportSettings> m_jobs;
    int m_current = -1;
    QProcess* m_process = nullptr;
    QThread* m_buildThread = nullptr;
    ExportBuildWorker* m_buildWorker = nullptr;
    QListWidget* m_jobsList = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QPushButton* m_startBtn = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_rmBtn = nullptr;
    QPushButton* m_clearBtn = nullptr;
    QPushButton* m_upBtn = nullptr;
    QPushButton* m_dnBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    double m_total = 0.0;
};