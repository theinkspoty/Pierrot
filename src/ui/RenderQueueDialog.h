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

// Fila de render: várias exportações (formatos/resoluções/fps diferentes)
// executadas em SEQUÊNCIA por um único ffmpeg por vez. Cada item é adicionado
// selecionando as configurações no ExportDialog (modo de configuração).
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
private:
    void refreshList();
    void startNextJob();
    void log(const QString& line);
    void setBusy(bool busy);
    Project* m_project = nullptr;
    QVector<ExportSettings> m_jobs;
    int m_current = -1;
    QProcess* m_process = nullptr;
    QListWidget* m_jobsList = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QPushButton* m_startBtn = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_rmBtn = nullptr;
    QPushButton* m_clearBtn = nullptr;
    QPushButton* m_upBtn = nullptr;
    QPushButton* m_dnBtn = nullptr;
    double m_total = 0.0;
};