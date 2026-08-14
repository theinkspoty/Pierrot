// Pierrot — editor de vídeo estilo Vegas Pro
//
// Copyright (C) 2026 Pierrot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <QDialog>
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
};
