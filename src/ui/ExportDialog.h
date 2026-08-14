// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>
#include <QString>
#include "models/Project.h"
#include "export/ProjectExporter.h"

class QLineEdit;
class QComboBox;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QProcess;

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExportDialog(Project* project, QWidget* parent = nullptr);
    ~ExportDialog() override;
private slots:
    void browseOutput();
    void startExport();
    void requestCancel();
    void onReadyRead();
    void onFinished(int exitCode);
private:
    ExportSettings currentSettings() const;
    void log(const QString& line);
    Project* m_project = nullptr;
    QLineEdit* m_outEdit = nullptr;
    QComboBox* m_resCombo = nullptr;
    QComboBox* m_fpsCombo = nullptr;
    QComboBox* m_formatCombo = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_logEdit = nullptr;
    QPushButton* m_startBtn = nullptr;
    QProcess* m_process = nullptr;
    double m_total = 0.0;
    QString m_logFile;
};
