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

    QString m_projectPath;
    QString m_projectName;
    int m_w = 1920;
    int m_h = 1080;
    int m_fps = 30;
    bool m_newRequested = false;
};
