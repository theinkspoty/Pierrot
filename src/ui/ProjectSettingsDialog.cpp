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

#include "ProjectSettingsDialog.h"

#include <QSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QDialogButtonBox>

ProjectSettingsDialog::ProjectSettingsDialog(int width, int height, int fps,
                                             QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Configurações do projeto"));

    m_w = new QSpinBox(this);
    m_w->setRange(64, 7680);
    m_w->setValue(width);
    m_h = new QSpinBox(this);
    m_h->setRange(64, 4320);
    m_h->setValue(height);

    m_fps = new QComboBox(this);
    for (int f : {24, 25, 30, 50, 60, 120})
        m_fps->addItem(QString("%1 fps").arg(f), f);
    const int idx = m_fps->findData(fps);
    if (idx >= 0) m_fps->setCurrentIndex(idx);

    auto* form = new QFormLayout;
    form->addRow(tr("Largura:"), m_w);
    form->addRow(tr("Altura:"), m_h);
    form->addRow(tr("Quadros/s:"), m_fps);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(buttons);
}

int ProjectSettingsDialog::width() const { return m_w->value(); }
int ProjectSettingsDialog::height() const { return m_h->value(); }
int ProjectSettingsDialog::fps() const { return m_fps->currentData().toInt(); }
