// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QSettings>
#include <QFileInfo>

// Presets de qualidade do preview: rótulo amigável + largura máxima de
// decodificação. Menor = menos RAM/CPU no preview.
namespace {
constexpr int kPreviewPresets[4] = { 480, 720, 1080, 1920 };

int nearestPresetIndex(int w) {
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (qAbs(kPreviewPresets[i] - w) < qAbs(kPreviewPresets[best] - w)) best = i;
    return best;
}
int presetWidth(int index) {
    return kPreviewPresets[qBound(0, index, 3)];
}
}

bool SettingsDialog::mkvWarningEnabled() {
    return QSettings().value("mkvWarning", true).toBool();
}

int SettingsDialog::maxDecodeWidth() {
    return QSettings().value("maxDecodeWidth", 1920).toInt();
}

int SettingsDialog::thumbMode() {
    return QSettings().value("timelineThumbMode", 0).toInt();
}

bool SettingsDialog::rippleDeleteEnabled() {
    return QSettings().value("timelineRippleDelete", true).toBool();
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Configurações"));
    setMinimumWidth(440);

    QSettings s;

    m_mkvWarn = new QCheckBox(tr("Avisar ao importar arquivos MKV (experimental)"), this);
    m_mkvWarn->setChecked(s.value("mkvWarning", true).toBool());

    m_autoSave = new QCheckBox(tr("Ativar salvamento automático"), this);
    m_autoSave->setChecked(s.value("autosaveEnabled", false).toBool());

    m_autoInterval = new QSpinBox(this);
    m_autoInterval->setRange(1, 1440);
    m_autoInterval->setSuffix(tr(" min"));
    m_autoInterval->setValue(s.value("autosaveMinutes", 10).toInt());

    m_decodeWidth = new QComboBox(this);
    m_decodeWidth->addItem(tr("Baixa (480p)"));
    m_decodeWidth->addItem(tr("Média (720p)"));
    m_decodeWidth->addItem(tr("Alta (1080p)"));
    m_decodeWidth->addItem(tr("Máxima (1920p)"));
    m_decodeWidth->setCurrentIndex(
        nearestPresetIndex(s.value("maxDecodeWidth", 1920).toInt()));

    m_thumbMode = new QComboBox(this);
    m_thumbMode->addItem(tr("Todas (contínuas)"));
    m_thumbMode->addItem(tr("Início e fim"));
    m_thumbMode->addItem(tr("Nenhuma"));
    m_thumbMode->setCurrentIndex(s.value("timelineThumbMode", 0).toInt());

    m_rippleDelete = new QCheckBox(tr("Fechar o vão automaticamente ao excluir (ripple)"), this);
    m_rippleDelete->setChecked(s.value("timelineRippleDelete", true).toBool());

    auto* mkvBox = new QGroupBox(tr("Avisos"), this);
    auto* mkvLay = new QVBoxLayout(mkvBox);
    mkvLay->addWidget(m_mkvWarn);
    auto* mkvHint = new QLabel(tr("MKV é experimental: alguns arquivos podem não abrir "
                                  "ou apresentar problemas de áudio/vídeo."), mkvBox);
    mkvHint->setStyleSheet("color: #9a9a9a;");
    mkvHint->setWordWrap(true);
    mkvLay->addWidget(mkvHint);

    auto* autoBox = new QGroupBox(tr("Salvamento automático"), this);
    auto* autoLay = new QFormLayout(autoBox);
    autoLay->addRow(m_autoSave);
    autoLay->addRow(tr("Intervalo:"), m_autoInterval);

    auto* perfBox = new QGroupBox(tr("Qualidade do preview"), this);
    auto* perfLay = new QFormLayout(perfBox);
    perfLay->addRow(tr("Qualidade:"), m_decodeWidth);
    auto* perfHint = new QLabel(tr("Qualidades mais baixas usam menos RAM/CPU no "
                                   "preview e no scrub. Recomendado em projetos "
                                   "grandes com muitos cortes."), perfBox);
    perfHint->setStyleSheet("color: #9a9a9a;");
    perfHint->setWordWrap(true);
    perfLay->addRow(perfHint);

    auto* tlBox = new QGroupBox(tr("Timeline"), this);
    auto* tlLay = new QFormLayout(tlBox);
    tlLay->addRow(tr("Miniaturas nos clipes:"), m_thumbMode);
    tlLay->addRow(m_rippleDelete);
    auto* tlHint = new QLabel(tr("Como os quadros são exibidos no corpo dos "
                                 "clipes de vídeo. \"Todas\" mostra fatias "
                                 "contínuas; \"Início e fim\" só nos extremos; "
                                 "\"Nenhuma\" deixa os clipes sem miniatura."), tlBox);
    tlHint->setStyleSheet("color: #9a9a9a;");
    tlHint->setWordWrap(true);
    tlLay->addRow(tlHint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(mkvBox);
    lay->addWidget(autoBox);
    lay->addWidget(perfBox);
    lay->addWidget(tlBox);
    lay->addWidget(buttons);
}

bool SettingsDialog::autoSaveEnabled() const { return m_autoSave->isChecked(); }
int SettingsDialog::autoSaveMinutes() const { return m_autoInterval->value(); }
bool SettingsDialog::mkvWarning() const { return m_mkvWarn->isChecked(); }
int SettingsDialog::decodeWidth() const { return presetWidth(m_decodeWidth->currentIndex()); }

void SettingsDialog::accept() {
    QSettings s;
    s.setValue("mkvWarning", m_mkvWarn->isChecked());
    s.setValue("autosaveEnabled", m_autoSave->isChecked());
    s.setValue("autosaveMinutes", m_autoInterval->value());
    s.setValue("maxDecodeWidth", presetWidth(m_decodeWidth->currentIndex()));
    s.setValue("timelineThumbMode", m_thumbMode->currentIndex());
    s.setValue("timelineRippleDelete", m_rippleDelete->isChecked());
    QDialog::accept();
}

void SettingsDialog::warnMkvIfNeeded(QWidget* parent, const QStringList& files) {
    static bool warnedOnce = false;
    if (warnedOnce || !mkvWarningEnabled()) return;
    for (const QString& f : files) {
        if (QFileInfo(f).suffix().compare(QLatin1String("mkv"), Qt::CaseInsensitive) == 0) {
            warnedOnce = true;
            QMessageBox::warning(parent, tr("MKV experimental"),
                tr("Arquivos MKV são suportados experimentalmente e podem apresentar "
                   "problemas de reprodução, áudio ou exportação."));
            return;
        }
    }
}
