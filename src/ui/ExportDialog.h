// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>
#include <QString>
#include <QHash>
#include "models/Project.h"
#include "export/ProjectExporter.h"

class QLineEdit;
class QComboBox;
class QSpinBox;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QProcess;
class OfxPluginManager;

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    enum Mode { Render = 0, Configure = 1 };
    explicit ExportDialog(Project* project, QWidget* parent = nullptr, Mode mode = Render);
    ~ExportDialog() override;
    // Abre o diálogo só para configurar uma exportação (usado pela fila de
    // render): retorna as configurações escolhidas quando o usuário aceitar,
    // ou settings com outputPath vazio se cancelar.
    static ExportSettings askSettings(Project* project, QWidget* parent);
    // Define o gerenciador de plugins OFX para pré-renderização.
    void setOfxManager(OfxPluginManager* mgr) { m_ofxManager = mgr; }
private slots:
    void browseOutput();
    void startExport();
    void requestCancel();
    void onReadyRead();
    void onFinished(int exitCode);
private:
    ExportSettings currentSettings() const;
    void log(const QString& line);
    void restoreLainkaMedia();
    void restoreOfxMedia();
    Project* m_project = nullptr;
    Mode m_mode = Render;
    QLineEdit* m_outEdit = nullptr;
    QComboBox* m_resCombo = nullptr;
    QComboBox* m_fpsCombo = nullptr;
    QComboBox* m_formatCombo = nullptr;
    QSpinBox* m_crfSpin = nullptr;
    QSpinBox* m_vbitrateSpin = nullptr;
    QSpinBox* m_abitrateSpin = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_logEdit = nullptr;
    QPushButton* m_startBtn = nullptr;
    QProcess* m_process = nullptr;
    double m_total = 0.0;
    QString m_logFile;
    // Mapeamento para restaurar mediaId após exportação LAINKA.
    QHash<QString, QString> m_lainkaOriginalMedia; // clipId → mediaId original
    QHash<QString, double> m_lainkaOriginalIn;     // clipId → in original
    QHash<QString, bool> m_lainkaOriginalEnabled;  // clipId → lainkaEnabled original
    QStringList m_lainkaTempMedia;                 // mediaIds temporários criados
    // Gerenciador de plugins OFX (opcional, para pré-renderização).
    OfxPluginManager* m_ofxManager = nullptr;
    // Mapeamento para restaurar mediaId após exportação OFX.
    QHash<QString, QString> m_ofxOriginalMedia; // clipId → mediaId original
    QHash<QString, double> m_ofxOriginalIn;     // clipId → in original
    QStringList m_ofxTempMedia;                 // mediaIds temporários criados
};
