#include "SettingsDialog.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QSettings>
#include <QFileInfo>

bool SettingsDialog::mkvWarningEnabled() {
    return QSettings().value("mkvWarning", true).toBool();
}

int SettingsDialog::maxDecodeWidth() {
    return QSettings().value("maxDecodeWidth", 1920).toInt();
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

    m_decodeWidth = new QSpinBox(this);
    m_decodeWidth->setRange(320, 3840);
    m_decodeWidth->setSingleStep(160);
    m_decodeWidth->setSuffix(tr(" px"));
    m_decodeWidth->setValue(s.value("maxDecodeWidth", 1920).toInt());

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

    auto* perfBox = new QGroupBox(tr("Desempenho"), this);
    auto* perfLay = new QFormLayout(perfBox);
    perfLay->addRow(tr("Largura máxima de decodificação do preview:"), m_decodeWidth);
    auto* perfHint = new QLabel(tr("Menor = menos RAM/CPU no preview. Padrão 1920."), perfBox);
    perfHint->setStyleSheet("color: #9a9a9a;");
    perfLay->addRow(perfHint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(mkvBox);
    lay->addWidget(autoBox);
    lay->addWidget(perfBox);
    lay->addWidget(buttons);
}

bool SettingsDialog::autoSaveEnabled() const { return m_autoSave->isChecked(); }
int SettingsDialog::autoSaveMinutes() const { return m_autoInterval->value(); }
bool SettingsDialog::mkvWarning() const { return m_mkvWarn->isChecked(); }
int SettingsDialog::decodeWidth() const { return m_decodeWidth->value(); }

void SettingsDialog::accept() {
    QSettings s;
    s.setValue("mkvWarning", m_mkvWarn->isChecked());
    s.setValue("autosaveEnabled", m_autoSave->isChecked());
    s.setValue("autosaveMinutes", m_autoInterval->value());
    s.setValue("maxDecodeWidth", m_decodeWidth->value());
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
