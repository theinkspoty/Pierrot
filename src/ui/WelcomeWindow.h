// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>

class QComboBox;
class QSpinBox;
class QLineEdit;
class QListWidget;
class QCheckBox;
class QLabel;

class WelcomeWindow : public QDialog {
    Q_OBJECT
public:
    explicit WelcomeWindow(QWidget* parent = nullptr);

    bool newProjectRequested() const { return m_newRequested; }
    QString projectPath() const { return m_projectPath; }
    QString projectName() const { return m_projectName; }
    int projectWidth() const { return m_w; }
    int projectHeight() const { return m_h; }
    int projectFps() const { return m_fps; }

private slots:
    void openSelected();
    void removeSelected();
    void requestNewProject();
    void onResolutionChanged(int idx);
    void onAutoIntervalChanged(int idx);

private:
    void buildLayout();
    void loadRecentProjects();
    int autosaveMinutes() const;
    void saveAutoSettings() const;

    QLabel* m_imageLabel = nullptr;
    QListWidget* m_recent = nullptr;
    QLineEdit* m_name = nullptr;
    QComboBox* m_resolution = nullptr;
    QWidget* m_customWidget = nullptr;
    QSpinBox* m_customW = nullptr;
    QSpinBox* m_customH = nullptr;
    QComboBox* m_fpsBox = nullptr;
    QCheckBox* m_autoSave = nullptr;
    QComboBox* m_autoInterval = nullptr;
    QSpinBox* m_autoCustom = nullptr;
    QLabel* m_devWarn = nullptr;

    QString m_projectPath;
    QString m_projectName;
    int m_w = 1920;
    int m_h = 1080;
    int m_fps = 30;
    bool m_newRequested = false;
};
