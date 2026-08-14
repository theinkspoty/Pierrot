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
#include <QStringList>

class QCheckBox;
class QSpinBox;
class QComboBox;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // Configurações compartilhadas (lidas de qualquer lugar do app).
    static bool mkvWarningEnabled();
    static int  maxDecodeWidth();

    // Valores escolhidos no diálogo (aplicar depois do OK).
    bool autoSaveEnabled() const;
    int  autoSaveMinutes() const;
    bool mkvWarning() const;
    int  decodeWidth() const;

    // Avisa (uma vez por sessão) quando importa arquivos MKV experimentais.
    static void warnMkvIfNeeded(QWidget* parent, const QStringList& files);
protected:
    void accept() override;
private:
    QCheckBox* m_mkvWarn = nullptr;
    QCheckBox* m_autoSave = nullptr;
    QSpinBox*  m_autoInterval = nullptr;
    QComboBox* m_decodeWidth = nullptr;
};
