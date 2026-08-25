// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ProjectSettingsDialog.h"
#include "ui/Theme.h"

#include <QSpinBox>
#include <QComboBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>

namespace {
int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }
}

// Presets de projeto estilo Vegas: pasta de resolução/fps.
struct ProjectPreset { const char* label; int w, h, fps; };
static const ProjectPreset kPresets[] = {
    { "720p (HD) · 25fps",   1280, 720,  25 },
    { "720p (HD) · 30fps",   1280, 720,  30 },
    { "1080p (Full HD) · 24fps", 1920, 1080, 24 },
    { "1080p (Full HD) · 30fps", 1920, 1080, 30 },
    { "1080p (Full HD) · 60fps", 1920, 1080, 60 },
    { "4K UHD · 30fps",      3840, 2160, 30 },
    { "Personalizado",       0, 0, 0 },
};
constexpr int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

ProjectSettingsDialog::ProjectSettingsDialog(int width, int height, int fps,
                                             QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Configurações do projeto"));
    setMinimumWidth(480);

    setStyleSheet(QStringLiteral(R"(
        QDialog { background: %1; }
        QLabel { color: %2; }
        QGroupBox {
            font-weight: 600;
            border: 1px solid %3;
            border-radius: 6px;
            margin-top: 14px;
            background: %4;
            color: %5;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            top: 4px;
            padding: 0 4px;
            color: %6;
        }
        QSpinBox, QComboBox {
            background: %7;
            border: 1px solid %8;
            border-radius: 4px;
            color: %9;
            padding: 4px 8px;
            min-height: 18px;
        }
        QSpinBox:focus, QComboBox:focus { border-color: %10; }
        QComboBox::drop-down { border: none; }
        QComboBox QAbstractItemView {
            background: %11;
            border: 1px solid %12;
            color: %13;
            selection-background-color: %14;
        }
        QDialogButtonBox QPushButton {
            background: %15;
            border: 1px solid %16;
            border-radius: 4px;
            color: %17;
            padding: 5px 14px;
        }
        QDialogButtonBox QPushButton:hover { background: %18; border-color: %19; }
        QDialogButtonBox QPushButton:pressed { background: %20; }
    )")
        .arg(themeColors().window.name())
        .arg(themeColors().text.name())
        .arg(themeColors().dockBorder.name())
        .arg(themeColors().button.name())
        .arg(themeColors().buttonText.name())
        .arg(themeColors().accent.name())
        .arg(themeColors().inputBg.name())
        .arg(themeColors().inputBorder.name())
        .arg(themeColors().text.name())
        .arg(themeColors().inputFocus.name())
        .arg(themeColors().button.name())
        .arg(themeColors().inputBorder.name())
        .arg(themeColors().text.name())
        .arg(themeColors().highlight.name())
        .arg(themeColors().button.name())
        .arg(themeColors().inputBorder.name())
        .arg(themeColors().buttonText.name())
        .arg(themeColors().btnHover.name())
        .arg(themeColors().dockBorder.name())
        .arg(themeColors().btnActive.name()));

    m_preset = new QComboBox(this);
    for (const ProjectPreset& p : kPresets)
        m_preset->addItem(tr(p.label));

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

    // Rótulo da proporção (ex.: 16:9).
    m_aspect = new QLabel(this);
    m_aspect->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_aspect->setStyleSheet(QStringLiteral("color:%1;").arg(themeColors().iconMuted.name()));

    // Detecta qual preset corresponde às configurações atuais.
    for (int i = 0; i < kPresetCount - 1; ++i)
        if (kPresets[i].w == width && kPresets[i].h == height
            && kPresets[i].fps == fps) {
            m_preset->setCurrentIndex(i);
            break;
        }
    connect(m_preset, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i) {
        if (i >= 0 && i < kPresetCount - 1) {
            const ProjectPreset& p = kPresets[i];
            m_applyingPreset = true;
            m_w->setValue(p.w);
            m_h->setValue(p.h);
            const int fi = m_fps->findData(p.fps);
            if (fi >= 0) m_fps->setCurrentIndex(fi);
            m_applyingPreset = false;
        }
        updateAspect();
    });
    auto* resLay = new QHBoxLayout;
    resLay->addWidget(m_w, 1);
    resLay->addWidget(new QLabel(tr("x"), this));
    resLay->addWidget(m_h, 1);

    auto* vidForm = new QFormLayout;
    vidForm->addRow(tr("Preset:"), m_preset);
    vidForm->addRow(tr("Resolução:"), resLay);
    vidForm->addRow(tr("Proporção:"), m_aspect);
    vidForm->addRow(tr("Quadros/s:"), m_fps);

    auto* videoBox = new QGroupBox(tr("Vídeo"), this);
    videoBox->setLayout(vidForm);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));

    // Mudar manualmente marca como "Personalizado".
    auto markCustom = [this]() {
        if (m_applyingPreset) return;
        m_preset->setCurrentIndex(kPresetCount - 1);
        updateAspect();
    };
    connect(m_w, QOverload<int>::of(&QSpinBox::valueChanged), this, markCustom);
    connect(m_h, QOverload<int>::of(&QSpinBox::valueChanged), this, markCustom);
    connect(m_fps, QOverload<int>::of(&QComboBox::currentIndexChanged), this, markCustom);

    updateAspect();

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 12, 16, 12);
    lay->setSpacing(10);
    lay->addWidget(videoBox);
    lay->addWidget(buttons);
}

void ProjectSettingsDialog::updateAspect() {
    const int w = m_w->value(), h = m_h->value();
    const int g = gcd(qMax(1, w), qMax(1, h));
    const QString ratio = (g > 0) ? QString("%1:%2").arg(w / g).arg(h / g) : QStringLiteral("-");
    m_aspect->setText(QString("%1 × %2 (%3)").arg(w).arg(h).arg(ratio));
}

int ProjectSettingsDialog::width() const { return m_w->value(); }
int ProjectSettingsDialog::height() const { return m_h->value(); }
int ProjectSettingsDialog::fps() const { return m_fps->currentData().toInt(); }
