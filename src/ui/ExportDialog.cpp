// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ExportDialog.h"
#include "SettingsDialog.h"

#include <QLineEdit>
#include <QComboBox>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QProcess>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QRegularExpression>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QMessageBox>

ExportDialog::ExportDialog(Project* project, QWidget* parent)
    : QDialog(parent), m_project(project) {
    setWindowTitle(tr("Exportar vídeo"));
    setMinimumWidth(580);

    m_outEdit = new QLineEdit(this);
    m_outEdit->setPlaceholderText(tr("Caminho do arquivo de saída…"));
    auto* browseBtn = new QPushButton(tr("…"), this);
    browseBtn->setFixedWidth(36);
    connect(browseBtn, &QPushButton::clicked, this, &ExportDialog::browseOutput);

    // Pré-preenche o caminho de saída com a pasta padrão de render (quando
    // ativada nas Configurações), usando o nome do projeto como arquivo.
    if (SettingsDialog::exportDefaultDirEnabled()) {
        const QString dir = SettingsDialog::exportDefaultDir();
        if (!dir.isEmpty() && QDir(dir).exists()) {
            QString base = m_project->name.trimmed();
            base.replace(QRegularExpression(QStringLiteral(
                             "[\\\\/:*?\"<>|]")), QStringLiteral("_"));
            if (base.isEmpty()) base = QStringLiteral("export");
            m_outEdit->setText(QDir(dir).filePath(base + QStringLiteral(".mp4")));
        }
    }

    m_resCombo = new QComboBox(this);
    m_resCombo->addItem(tr("Fonte (%1x%2)").arg(project->width).arg(project->height), "source");
    m_resCombo->addItem("3840x2160 (4K)", "3840x2160");
    m_resCombo->addItem("1920x1080 (Full HD)", "1920x1080");
    m_resCombo->addItem("1280x720 (HD)", "1280x720");

    m_fpsCombo = new QComboBox(this);
    for (int f : {24, 25, 30, 50, 60})
        m_fpsCombo->addItem(QString("%1 fps").arg(f), f);
    const int fpsIdx = m_fpsCombo->findData(project->fps);
    if (fpsIdx >= 0) m_fpsCombo->setCurrentIndex(fpsIdx);

    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItem("MP4 (H.264 + AAC)", (int)ExportSettings::MP4);
    m_formatCombo->addItem("MKV (H.264 + AAC)", (int)ExportSettings::MKV);
    m_formatCombo->addItem("WebM (VP9 + Opus)", (int)ExportSettings::WEBM);

    auto* outRow = new QHBoxLayout;
    outRow->setContentsMargins(0, 0, 0, 0);
    outRow->addWidget(m_outEdit, 1);
    outRow->addWidget(browseBtn);

    auto* form = new QFormLayout;
    form->addRow(tr("Saída:"), outRow);
    form->addRow(tr("Resolução:"), m_resCombo);
    form->addRow(tr("Quadros/s:"), m_fpsCombo);
    form->addRow(tr("Formato:"), m_formatCombo);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);

    m_logEdit = new QPlainTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumHeight(120);

    m_startBtn = new QPushButton(tr("Exportar"), this);
    auto* cancelBtn = new QPushButton(tr("Fechar"), this);
    connect(cancelBtn, &QPushButton::clicked, this, &ExportDialog::requestCancel);
    connect(m_startBtn, &QPushButton::clicked, this, &ExportDialog::startExport);

    // Copia todo o log para a área de transferência (útil para reportar erros).
    auto* copyBtn = new QPushButton(tr("Copiar tudo"), this);
    copyBtn->setToolTip(tr("Copiar todo o log para a área de transferência"));
    connect(copyBtn, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_logEdit->toPlainText());
    });

    auto* btnRow = new QHBoxLayout;
    btnRow->addWidget(copyBtn);
    btnRow->addStretch();
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(m_startBtn);

    auto* lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(m_progress);
    lay->addWidget(m_logEdit);
    lay->addLayout(btnRow);
}

ExportDialog::~ExportDialog() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        log(tr("Exportação cancelada pelo encerramento da janela."));
    }
}

void ExportDialog::requestCancel() {
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        reject();
        return;
    }
    const QMessageBox::StandardButton ans = QMessageBox::question(
        this, tr("Cancelar render?"),
        tr("A renderização está em andamento. Deseja cancelar?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ans == QMessageBox::Yes) {
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
        m_startBtn->setEnabled(true);
        m_progress->setValue(0);
        log(tr("Renderização cancelada pelo usuário."));
        reject();
    }
}

void ExportDialog::browseOutput() {
    QString filter = tr("MP4 (*.mp4)");
    QString ext = "mp4";
    const ExportSettings::Format fmt = (ExportSettings::Format)m_formatCombo->currentData().toInt();
    if (fmt == ExportSettings::MKV) { filter = tr("MKV (*.mkv)"); ext = "mkv"; }
    if (fmt == ExportSettings::WEBM) { filter = tr("WebM (*.webm)"); ext = "webm"; }

    // Abre na pasta padrão de render se o usuário ativou essa opção.
    QString startDir = QDir::homePath();
    if (SettingsDialog::exportDefaultDirEnabled()) {
        const QString d = SettingsDialog::exportDefaultDir();
        if (!d.isEmpty() && QDir(d).exists()) startDir = d;
    }

    QString f = QFileDialog::getSaveFileName(this, tr("Salvar vídeo"),
                                             startDir + "/export." + ext, filter);
    if (f.isEmpty()) return;
    if (!f.endsWith("." + ext)) f += "." + ext;
    m_outEdit->setText(f);
}

ExportSettings ExportDialog::currentSettings() const {
    ExportSettings s;
    s.outputPath = m_outEdit->text().trimmed();
    const QString res = m_resCombo->currentData().toString();
    if (res == "source") {
        s.width = m_project->width;
        s.height = m_project->height;
    } else {
        const QStringList parts = res.split('x');
        s.width = parts.value(0).toInt();
        s.height = parts.value(1).toInt();
    }
    s.fps = m_fpsCombo->currentData().toInt();
    s.format = (ExportSettings::Format)m_formatCombo->currentData().toInt();
    return s;
}

void ExportDialog::startExport() {
    if (m_process) { log(tr("Já há uma exportação em andamento.")); return; }

    const ExportSettings s = currentSettings();
    if (s.outputPath.isEmpty()) { log(tr("Informe o caminho de saída.")); return; }

    QString err;
    const QStringList args = ProjectExporter::buildCommand(*m_project, s, &err);
    if (args.isEmpty() && err.isEmpty())
        err = tr("Falha ao montar o comando de exportação.");
    if (!err.isEmpty()) { log(tr("Erro: %1").arg(err)); return; }

    m_total = m_project->duration();
    m_progress->setValue(0);
    m_logEdit->clear();
    m_logFile = QFileInfo(s.outputPath).dir().filePath(
        QStringLiteral("pierrot-render-%1.log")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
    log(tr("Log da renderização: %1").arg(m_logFile));

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &ExportDialog::onReadyRead);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) { onFinished(code); });

    log(tr("ffmpeg %1").arg(args.join(' ')));
    m_process->start("ffmpeg", args);
    if (!m_process->waitForStarted(3000)) {
        log(tr("Não foi possível iniciar o ffmpeg. Instale o ffmpeg e tente novamente."));
        m_process->deleteLater();
        m_process = nullptr;
        return;
    }
    m_startBtn->setEnabled(false);
    log(tr("Exportando…"));
}

void ExportDialog::onReadyRead() {
    if (!m_process) return;
    const QString text = QString::fromUtf8(m_process->readAll());
    static const QRegularExpression re("time=(\\d+):(\\d+):(\\d+\\.?\\d*)");
    const QRegularExpressionMatch m = re.match(text);
    if (m.hasMatch() && m_total > 0) {
        const double secs = m.captured(1).toDouble() * 3600.0
                          + m.captured(2).toDouble() * 60.0
                          + m.captured(3).toDouble();
        m_progress->setValue((int)(secs / m_total * 100.0));
    }
    // Grava a saída do ffmpeg no log (sem as linhas de progresso, para não
    // poluir). Assim o arquivo do log mostra o erro exato de uma falha.
    const QStringList lines = text.split('\n');
    for (const QString& ln : lines) {
        if (ln.contains(QStringLiteral("time=")) && ln.contains(QStringLiteral("frame=")))
            continue;
        if (!ln.trimmed().isEmpty()) log(ln);
    }
}

void ExportDialog::onFinished(int exitCode) {
    m_startBtn->setEnabled(true);
    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (exitCode == 0) {
        m_progress->setValue(100);
        log(tr("Exportação concluída com sucesso."));
        QMessageBox::information(this, tr("Exportar"),
                                 tr("Vídeo exportado com sucesso."));
    } else {
        log(tr("Exportação falhou (código %1).").arg(exitCode));
        QMessageBox::warning(this, tr("Exportar"),
                             tr("Falha na exportação. Veja o log acima."));
    }
}

void ExportDialog::log(const QString& line) {
    m_logEdit->appendPlainText(line);
    if (!m_logFile.isEmpty()) {
        QFile f(m_logFile);
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            f.write(line.toUtf8());
            f.write("\n");
        }
    }
}
