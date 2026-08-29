// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/RenderQueueDialog.h"
#include "ui/ExportDialog.h"

#include <QListWidget>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QProcess>
#include <QThread>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QRegularExpression>
#include <QFileInfo>
#include <QTimer>
#include <QMessageBox>

#include <atomic>
#include <utility>

namespace {
const char* formatLabel(int fmt) {
    switch (fmt) {
    case ExportSettings::MKV: return "MKV";
    case ExportSettings::WEBM: return "WebM";
    default: return "MP4";
    }
}
} // namespace

// Worker que monta o comando ffmpeg (incluindo o pré-render das bandas Mesa)
// fora da thread da UI. O Projeto é recebido por cópia (snapshot) — a leitura
// fica isolada de edições simultâneas na timeline. O `progress` da buildCommand
// é repassado como signal; retorna false quando `cancel()` é chamado.
class ExportBuildWorker : public QObject {
    Q_OBJECT
public:
    ExportBuildWorker(Project project, ExportSettings settings)
        : m_project(std::move(project)), m_settings(std::move(settings)) {}
public slots:
    void run() {
        QString err;
        const QStringList args = ProjectExporter::buildCommand(
            m_project, m_settings, &err,
            [this](int pct) {
                emit progress(pct);
                return !m_cancel.load();
            });
        if (args.isEmpty())
            emit failed(err.isEmpty() ? QStringLiteral("Falha ao montar o comando.") : err);
        else
            emit ready(args);
    }
    void cancel() { m_cancel.store(true); }
signals:
    void ready(const QStringList& args);
    void failed(const QString& err);
    void progress(int pct);
private:
    Project m_project;
    ExportSettings m_settings;
    std::atomic<bool> m_cancel{false};
};

RenderQueueDialog::RenderQueueDialog(Project* project, QWidget* parent)
    : QDialog(parent), m_project(project) {
    setWindowTitle(tr("Fila de Render"));
    setMinimumSize(600, 400);

    m_jobsList = new QListWidget(this);
    m_jobsList->setSelectionMode(QAbstractItemView::SingleSelection);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumHeight(140);

    m_addBtn = new QPushButton(tr("Adicionar…"), this);
    m_rmBtn = new QPushButton(tr("Remover"), this);
    m_upBtn = new QPushButton(tr("▲"), this);
    m_dnBtn = new QPushButton(tr("▼"), this);
    m_clearBtn = new QPushButton(tr("Limpar"), this);
    m_startBtn = new QPushButton(tr("Iniciar fila"), this);
    m_cancelBtn = new QPushButton(tr("Cancelar"), this);
    m_cancelBtn->setEnabled(false);

    connect(m_addBtn, &QPushButton::clicked, this, &RenderQueueDialog::addJob);
    connect(m_rmBtn, &QPushButton::clicked, this, &RenderQueueDialog::removeSelected);
    connect(m_clearBtn, &QPushButton::clicked, this, [this]() {
        if (m_process || m_buildThread) return; // não limpa durante a execução
        m_jobs.clear();
        refreshList();
    });
    connect(m_upBtn, &QPushButton::clicked, this, [this]() { moveSelected(-1); });
    connect(m_dnBtn, &QPushButton::clicked, this, [this]() { moveSelected(1); });
    connect(m_startBtn, &QPushButton::clicked, this, &RenderQueueDialog::startQueue);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this]() {
        if (m_buildWorker)
            m_buildWorker->cancel(); // abate o pré-render das Mesas
        else if (m_process && m_process->state() != QProcess::NotRunning)
            m_process->kill();       // mata o ffmpeg atual
    });

    auto* editBtns = new QHBoxLayout;
    editBtns->addWidget(m_addBtn);
    editBtns->addWidget(m_rmBtn);
    editBtns->addWidget(m_upBtn);
    editBtns->addWidget(m_dnBtn);
    editBtns->addWidget(m_clearBtn);
    editBtns->addStretch();
    editBtns->addWidget(m_cancelBtn);
    editBtns->addWidget(m_startBtn);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(tr("As exportações rodam em sequência (um ffmpeg por vez)."), this));
    lay->addWidget(m_jobsList, 1);
    lay->addLayout(editBtns);
    lay->addWidget(m_progress);
    lay->addWidget(m_log);
}

RenderQueueDialog::~RenderQueueDialog() {
    finishBuildThread();
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }
}

void RenderQueueDialog::addJob() {
    ExportSettings s = ExportDialog::askSettings(m_project, this);
    if (s.outputPath.isEmpty()) return; // cancelado
    m_jobs.append(s);
    refreshList();
}

void RenderQueueDialog::refreshList() {
    m_jobsList->blockSignals(true);
    m_jobsList->clear();
    for (int i = 0; i < m_jobs.size(); ++i) {
        const ExportSettings& s = m_jobs[i];
        m_jobsList->addItem(QString("%1  ·  %2 ×%3 @ %4 fps  ·  %5")
                                .arg(QFileInfo(s.outputPath).fileName(),
                                     QString::number(s.width), QString::number(s.height),
                                     QString::number(s.fps),
                                     QString::fromLatin1(formatLabel(s.format))));
    }
    m_jobsList->blockSignals(false);
}

void RenderQueueDialog::removeSelected() {
    if (m_process || m_buildThread || m_jobs.isEmpty()) return;
    const int row = m_jobsList->currentRow();
    if (row < 0 || row >= m_jobs.size()) return;
    m_jobs.removeAt(row);
    refreshList();
}

void RenderQueueDialog::moveSelected(int dir) {
    if (m_process || m_buildThread || m_jobs.isEmpty()) return;
    const int row = m_jobsList->currentRow();
    const int to = row + dir;
    if (row < 0 || to < 0 || to >= m_jobs.size()) return;
    m_jobs.swapItemsAt(row, to);
    refreshList();
    m_jobsList->setCurrentRow(to);
}

void RenderQueueDialog::startQueue() {
    if (m_process || m_buildThread || m_jobs.isEmpty()) return;
    setBusy(true);
    m_current = -1;
    m_progress->setValue(0);
    m_log->clear();
    startNextJob();
}

void RenderQueueDialog::finishBuildThread() {
    if (!m_buildThread) return;
    m_buildThread->quit();
    m_buildThread->wait();
    m_buildThread = nullptr;
    m_buildWorker = nullptr; // deletado via deleteLater quando o thread acaba
}

void RenderQueueDialog::onCommandBuilt(const QStringList& args) {
    finishBuildThread();
    m_progress->setFormat(QString()); // volta ao formato padrão (tempo do ffmpeg)
    m_total = m_projectSnap.duration();
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &RenderQueueDialog::onReadyRead);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RenderQueueDialog::onJobFinished);
    m_process->start("ffmpeg", args);
    if (!m_process->waitForStarted(3000)) {
        log(tr("  Não foi possível iniciar o ffmpeg."));
        m_process->deleteLater();
        m_process = nullptr;
        startNextJob();
    }
}

void RenderQueueDialog::onBuildFailed(const QString& err) {
    finishBuildThread();
    log(tr("  Erro: %1").arg(err));
    QTimer::singleShot(0, this, [this]() { startNextJob(); });
}

void RenderQueueDialog::startNextJob() {
    ++m_current;
    if (m_current >= m_jobs.size()) {
        setBusy(false);
        m_progress->setValue(100);
        log(tr("Fila concluída: %1 exportação(ões) finalizada(s).").arg(m_jobs.size()));
        QMessageBox::information(this, tr("Fila de Render"),
                                 tr("Todas as exportações foram concluídas."));
        return;
    }
    const ExportSettings& s = m_jobs[m_current];
    m_progress->setValue(0);
    log(tr("▶ [%1/%2] %3")
            .arg(m_current + 1).arg(m_jobs.size())
            .arg(QFileInfo(s.outputPath).fileName()));

    // Snapshot do projeto para o worker: o pré-render das bandas Mesa é caro
    // em CPU e roda fora da UI; a cópia evita corrida com edições na timeline.
    m_projectSnap = *m_project;
    m_buildThread = new QThread(this);
    m_buildWorker = new ExportBuildWorker(m_projectSnap, s);
    m_buildWorker->moveToThread(m_buildThread);
    connect(m_buildThread, &QThread::started, m_buildWorker, &ExportBuildWorker::run);
    connect(m_buildWorker, &ExportBuildWorker::ready,
            this, &RenderQueueDialog::onCommandBuilt);
    connect(m_buildWorker, &ExportBuildWorker::failed,
            this, &RenderQueueDialog::onBuildFailed);
    connect(m_buildWorker, &ExportBuildWorker::progress, this, [this](int pct) {
        m_progress->setFormat(QStringLiteral("Mesa %p%"));
        m_progress->setValue(pct);
    });
    connect(m_buildThread, &QThread::finished, m_buildWorker, &QObject::deleteLater);
    connect(m_buildThread, &QThread::finished, m_buildThread, &QObject::deleteLater);
    m_buildThread->start();
}

void RenderQueueDialog::onReadyRead() {
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
    const QStringList lines = text.split('\n');
    for (const QString& ln : lines) {
        if (ln.contains(QStringLiteral("time=")) && ln.contains(QStringLiteral("frame=")))
            continue;
        if (!ln.trimmed().isEmpty()) log(ln);
    }
}

void RenderQueueDialog::onJobFinished(int exitCode, QProcess::ExitStatus) {
    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
    }
    if (exitCode == 0)
        log(tr("  ✓ Concluído."));
    else
        log(tr("  ✗ Falhou (código %1).").arg(exitCode));
    QTimer::singleShot(0, this, [this]() { startNextJob(); });
}

void RenderQueueDialog::setBusy(bool busy) {
    m_startBtn->setEnabled(!busy);
    m_addBtn->setEnabled(!busy);
    m_rmBtn->setEnabled(!busy);
    m_clearBtn->setEnabled(!busy);
    m_cancelBtn->setEnabled(busy);
    if (busy) m_jobsList->setEnabled(false);
    else m_jobsList->setEnabled(true);
}

void RenderQueueDialog::log(const QString& line) {
    m_log->appendPlainText(line);
}

#include "RenderQueueDialog.moc"
