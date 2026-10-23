// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MainWindow.h"
#include "version.h"

#include "ui/panels/MediaPoolWidget.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/monitor/PreviewWidget.h"
#include "ui/mesa/PantaloneWidget.h"
#include "ui/motion/DottoreWidget.h"
#include "ui/dialogs/MaskEditorDialog.h"
#include "ui/motion/GraphEditorWidget.h"
#include "ui/panels/EffectsWidget.h"
#include "ui/timeline/ClipPropertiesWidget.h"
#include "ui/dialogs/TextEditorDialog.h"
#include "ui/motion/LazziDock.h"
#include "ui/motion/AnimPresetDock.h"
#include "ui/panels/ColorDock.h"
#include "ui/panels/ExpressWidget.h"
#include "ui/panels/FileBrowserWidget.h"
#include "ui/audio/MixerWidget.h"
#include "ui/audio/EqualizerWidget.h"
#include "ui/mesa/MesaWidget.h"
#include "ui/monitor/SourceMonitorWidget.h"
#include "ui/motion/VelocityEditorWidget.h"
#include "ui/mesa/PivotWidget.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "ui/dialogs/ExportDialog.h"
#include "ui/dialogs/RenderQueueDialog.h"
#include "colombina/export/NleInterchange.h"
#include "ui/monitor/ScopeWidget.h"
#include "ui/monitor/PreviewMonitor.h"
#include "ui/monitor/PreviewProfiler.h"
#include "ui/dialogs/ProjectSettingsDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/core/Theme.h"
#include "ui/core/TechnicalWindow.h"
#include "colombina/ofx/OfxPluginManager.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QSet>
#include <QSettings>
#include "ui/dialogs/WelcomeWindow.h"
#include "colombina/ffmpeg/MediaCache.h"
#include "colombina/ffmpeg/ProxyManager.h"

#include <QApplication>
#include <QPointer>
#include <QDockWidget>
#include <QListWidget>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QAction>
#include <QKeySequence>
#include <QMouseEvent>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <algorithm>
#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractButton>
#include <QPainter>
#include <QPixmap>
#include <QCloseEvent>
#include <QShowEvent>
#include <QStyle>
#include <QShortcut>
#include <QPainterPath>
#include <QPolygonF>
#include <QComboBox>
#include <QTimer>
#include <QTime>
#include <QProgressBar>
#include <QGuiApplication>
#include <QScreen>
#include <QDataStream>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>
#include <QThreadPool>

namespace {
// Número máximo de cópias do backup rotativo (~/Pierrot/backups/).
constexpr int kBackupCopies = 10;

// Resultado da gravação em disco feita no worker do save assíncrono.
struct SaveWriteResult {
    bool ok = false;
    QString detail; // mensagem de erro se !ok
};

// Backup rotativo: ~/Pierrot/backups/<nome>_AAAA-MM-DD_HH-MM-SS.Blanc, mantendo
// no máximo kBackupCopies cópias do mesmo projeto. Chamado a cada save bem-
// sucedido (manual ou autosave), dentro do worker. Melhor esforço: falha
// silenciosa — nunca bloqueia nem interfere com o save.
void rotatingBackup(const QString& sourcePath) {
    const QFileInfo src(sourcePath);
    QDir dir(QDir::homePath());
    if (!dir.mkpath(QStringLiteral("Pierrot/backups"))) return;
    dir.cd(QStringLiteral("Pierrot/backups"));

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    const QString dest = dir.filePath(src.completeBaseName() + QLatin1Char('_') + stamp
                                      + QLatin1String(".Blanc"));
    if (QFile::copy(sourcePath, dest)) {
        const QString base = src.completeBaseName() + QLatin1Char('_');
        QStringList candidates;
        const QStringList entries = dir.entryList(QStringList() << base + QLatin1String("*.Blanc"),
                                                  QDir::Files, QDir::NoSort);
        for (const QString& e : entries) candidates << dir.filePath(e);
        std::sort(candidates.begin(), candidates.end(),
                  [](const QString& a, const QString& b) {
                      return QFileInfo(a).lastModified() > QFileInfo(b).lastModified();
                  });
        while (candidates.size() > static_cast<std::size_t>(kBackupCopies)) {
            QFile::remove(candidates.takeLast());
        }
    }
}

}

void MainWindow::newProject() {
    if (!confirmDiscardChanges()) return;
    MediaCache::instance().clear();
    m_project = Project();
    ++m_projGen; // troca de projeto: invalida callbacks de save em voo
    for (int i = 0; i < 3; ++i) m_project.addTrack(false);
    for (int i = 0; i < 3; ++i) m_project.addTrack(true);
    m_pantalone->setProject(&m_project);
    m_pantaloneDock->hide();
    m_undoStack.clear();
    m_undoStack.push_back(snapshotState());
    m_undoLabels.clear();
    m_undoLabels.push_back(tr("Início"));
    m_undoBytes = m_undoStack.front().size();
    m_undoIndex = 0;
    updateHistoryList();
    m_currentFile.clear();
    m_modified = false;
    ProxyManager::instance().setProjectUsesProxies(m_project.useProxies);
    applyUndoState();
    // Troca de projeto: MainWindow reusa o mesmo &m_project, então o
    // PreviewWidget::setProject (chamado uma única vez na construção, linha 221)
    // nunca roda aqui — o transporte, a agulha e os caches ficavam do projeto
    // anterior (agulha podia parar além da duração nova e o play seguia em cima
    // do conteúdo trocado). Para o play e recoloca todo mundo no zero.
    m_preview->setProject(&m_project);
    m_timeline->setPlayhead(0.0); // setPlayhead não emite playheadChanged
    updateTitle();
    statusBar()->showMessage(tr("Novo projeto criado."));
}

void MainWindow::openProject() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Abrir projeto"), QString(),
        tr("Pierrot (*.Blanc *.ovp);;Todos os arquivos (*)"));
    if (path.isEmpty()) return;
    openProjectFile(path);
}

// Recarrega o projeto atual do disco (F5). Útil durante o desenvolvimento:
// edita o .Blanc por fora (ou o app grava algo) e atualiza sem reabrir.
// Se não houver arquivo salvo, cai no diálogo de Abrir.
void MainWindow::reloadProject() {
    if (m_currentFile.isEmpty()) { openProject(); return; }
    if (!confirmDiscardChanges()) return;
    openProjectFile(m_currentFile);
}

// Indicador combinado de trabalho em background: a barra da statusBar fica
// visível enquanto houver thumbs/picos (MediaCache) OU proxy (ProxyManager)
// em curso, com a mensagem do passo mais longo (proxy primeiro).
void MainWindow::refreshBackgroundBusy() {
    if (!m_busyBar) return;
    const bool busy = m_bgCacheBusy || m_bgProxyBusy;
    m_busyBar->setVisible(busy);
    if (m_bgProxyBusy) {
        statusBar()->showMessage(m_proxyPending > 0
            ? tr("Gerando proxy: %1 restante(s)…").arg(m_proxyPending)
            : tr("Gerando proxy…"));
    } else if (m_bgCacheBusy) {
        statusBar()->showMessage(tr("Carregando áudio e miniaturas…"));
    } else {
        const QString msg = statusBar()->currentMessage();
        if (msg.contains(tr("Gerando proxy")) || msg.contains(tr("Carregando áudio")))
            statusBar()->clearMessage();
    }
}

// Reabre a janela inicial sem fechar o editor. Se o usuário escolher abrir um
// projeto ou criar um novo, substitui o projeto atual (como em Arquivo → Novo).
void MainWindow::showWelcomeWindow() {
    WelcomeWindow welcome(this);
    if (welcome.exec() != QDialog::Accepted) return;
    if (!welcome.projectPath().isEmpty()) {
        openProjectFile(welcome.projectPath());
    } else if (welcome.newProjectRequested()) {
        createProject(welcome.projectWidth(), welcome.projectHeight(),
                      welcome.projectFps(), welcome.projectName());
    }
}

void MainWindow::openProjectFile(const QString& path) {
    MediaCache::instance().clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Abrir projeto"),
                             tr("Não foi possível abrir o arquivo:\n%1").arg(path));
        return;
    }
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(this, tr("Abrir projeto"),
                             tr("Arquivo de projeto inválido:\n%1").arg(path));
        return;
    }
    // ── Portão de versão do .Blanc (grupos de formato) ────────────────────
    // Arquivo de um grupo MAIS NOVO: recusa ANTES de tocar no modelo atual
    // (interpretar formato desconhecido é corrupção silenciosa). Arquivo de
    // um grupo mais ANTIGO (0.7 ou anterior ao versionamento): avisa que a
    // conversão é one-way e, em caso positivo, migra e já regrava o arquivo
    // no formato novo (o backup rotativo protege o original).
    const QJsonObject obj = doc.object();
    const int fileVer = Project::schemaOf(obj);
    const QString fileGroup = Project::formatGroupOf(obj);
    if (fileVer > Project::kSchemaVersion) {
        QMessageBox::critical(
            this, tr("Projeto de versão mais nova"),
            tr("Este projeto foi salvo por uma versão mais nova do Pierrot\n"
               "(schema %1%2 — este app é o schema %3, grupo " PIERROT_VERSION_GROUP ").\n\n"
               "Atualize o Pierrot para abrir este arquivo.")
                .arg(fileVer)
                .arg(fileGroup.isEmpty() ? QString()
                                         : tr(", grupo %1").arg(fileGroup))
                .arg(Project::kSchemaVersion));
        return;
    }
    bool convertOld = false;
    if (fileVer < Project::kSchemaVersion) {
        const auto btn = QMessageBox::warning(
            this, tr("Projeto de versão antiga"),
            tr("Seu projeto usa uma versão antiga do Laartman — foi salvo por "
               "uma versão anterior do Pierrot%1.\n\n"
               "Ao CONVERTER para o formato atual (grupo " PIERROT_VERSION_GROUP ", "
               "Laartman v0.4), o projeto não poderá mais ser aberto no "
               "Pierrot 0.7 ou anteriores.\n\n"
               "Deseja converter e salvar agora?")
                .arg(fileGroup.isEmpty()
                         ? tr(" (formato anterior ao versionamento por grupo)")
                         : tr(" (grupo %1)").arg(fileGroup)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (btn != QMessageBox::Yes) return; // mantém o projeto atual intacto
        convertOld = true;
    }
    if (!m_project.fromJson(obj)) {
        // Defensivo: o portão acima já recusou, mas fromJson é a autoridade.
        QMessageBox::warning(this, tr("Abrir projeto"),
                             tr("Arquivo de projeto recusado (versão não suportada):\n%1").arg(path));
        return;
    }
    ++m_projGen; // troca de projeto: invalida callbacks de save em voo
    if (m_project.videoTracks.isEmpty()) m_project.addTrack(false);
    if (m_project.audioTracks.isEmpty()) m_project.addTrack(true);

    m_undoStack.clear();
    m_undoStack.push_back(snapshotState());
    m_undoLabels.clear();
    m_undoLabels.push_back(tr("Início"));
    m_undoBytes = m_undoStack.front().size();
    m_undoIndex = 0;
    updateHistoryList();
    m_currentFile = path;
    m_modified = false;
    ProxyManager::instance().setProjectUsesProxies(m_project.useProxies);
    // Sonda as mídias do projeto para enfileirar proxies. Sem isso, abrir um
    // projeto do disco nunca gerava proxy nenhum: probeAndQueue() só era
    // chamado na importação de mídia nova, então um projeto 4K aberto de um
    // .Blanc reproduzia do original a vida inteira (a menos que outra sessão
    // tivesse gerado antes e deixado o metadata.json).
    for (const MediaItem& mi : m_project.media)
        if (mi.hasVideo && !mi.filePath.isEmpty())
            ProxyManager::instance().probeAndQueue(mi.filePath);
    applyUndoState();
    // Mesmo motivo do newProject: sem setProject, abrir um arquivo mantinha a
    // agulha/transporte/caches do projeto anterior (arquivo menor = agulha além
    // da duração, "Sem clipe" e cache velho no monitor).
    m_preview->setProject(&m_project);
    m_timeline->setPlayhead(0.0); // setPlayhead não emite playheadChanged
    updateTitle();
    addRecentProject(path);
    if (convertOld) {
        // Conversão one-way já confirmada: regrava o arquivo no formato do
        // grupo atual. O save é assíncrono e o backup rotativo
        // (~/Pierrot/backups/) preserva o original antes da sobrescrita.
        writeProjectFile(path);
        statusBar()->showMessage(
            tr("Projeto CONVERTIDO para o grupo " PIERROT_VERSION_GROUP
               " (Laartman v0.4) e salvo — não abre mais no Pierrot 0.7."));
    } else {
        statusBar()->showMessage(tr("Projeto aberto: %1").arg(path));
    }
}

// Espera a fila de proxies esvaziar (teto de 10 min) e então abre a janela
// medida. Fora daqui fica o warm-up: abrir decoders e aquecer caches.
void MainWindow::awaitProxiesThenMeasure(double start, double warmupSec, double seconds) {
    auto* poll = new QTimer(this);
    poll->setInterval(500);
    // O timer é dono da janela e pode ficar vivo bem depois deste retorno, então
    // o estado da espera precisa sobreviver à stack: capturar &waited/&tries
    // deixava a lambda lendo memória de pilha destruída (UB) bem depois do fim
    // de awaitProxiesThenMeasure().
    auto st = std::make_shared<ProxyWaitState>();
    st->waited.start();
    connect(poll, &QTimer::timeout, poll, [this, poll, st, start, warmupSec, seconds]() {
        const bool timeout = st->waited.elapsed() > 600000;
        if (ProxyManager::instance().busy() && !timeout && ++st->tries <= 1200) return;
        if (timeout)
            qWarning("[autoplay] proxies ainda em fila após 10 min — medindo assim mesmo.");
        poll->stop();
        poll->deleteLater();
        startMeasuredRun(start, warmupSec, seconds);
    });
    poll->start();
}

// Abre a janela medida: roda `warmupSec` fora da coleta, zera o profiler,
// reproduz `seconds` e grava o JSON.
void MainWindow::startMeasuredRun(double start, double warmupSec, double seconds) {
    m_preview->playFrom(start);
    QTimer::singleShot(int(warmupSec * 1000.0), this, [this, seconds, start]() {
        PreviewProfiler::instance().beginSession(QStringLiteral("autoplay"),
                                                 m_project.useProxies, m_project.fps);
        m_preview->seek(start);
        m_preview->playFrom(start);
        QTimer::singleShot(int(seconds * 1000.0), this, [this]() {
            PreviewProfiler::instance().endSession();
            const auto& prof = PreviewProfiler::instance();
            const bool ok = prof.dumpJson(m_autoplayJsonOut);
            qInfo("[autoplay] relatório %s: %d quadros, fonte=%s, cortes=%d, dropped=%lld",
                  ok ? "gravado" : "FALHOU", prof.frameCount(),
                  prof.wroteProxy() ? "proxy" : "original", prof.cuts(),
                  static_cast<long long>(prof.droppedTotal()));
            m_preview->setLoopEnabled(false);
            m_preview->togglePlay(); // pausa
            // SAI DA APLICAÇÃO. quit() chamado UMA vez pode encerrar só um
            // loop aninhado (Qt interno do pause de áudio/PipeWire) e o loop
            // principal continua vivo — sessões longas nunca saíam (EXIT=124).
            // Solução: uma thread destacada repete quit() até o processo
            // morrer — cada chamada encerra o loop de topo atual; depois do
            // teardown, QCoreApplication::exit é no-op (self nulo/sem loops).
            std::thread([] {
                for (int i = 0; i < 100; ++i) {
                    QThread::msleep(200);
                    QCoreApplication::quit();
                }
                qInfo("[autoplay] 100 rodadas de quit e o processo seguiu vivo!");
            }).detach();
            // Diagnóstico: se existir 8 s depois, algo além dos loops prende.
            QTimer::singleShot(8000, qApp, []() {
                qInfo("[autoplay] WATCHDOG: vivo 8s após a martelada de quit!");
            });
        });
    });
}

// ── Harness de reprodução sem interação (pierrot --autoplay) ──────────────
//
// Existe para o A/B proxy-vs-original ser reproduzível: a comparação depende
// de a partida começar sempre do mesmo lugar, com os proxies no mesmo estado de
// calor. Fica em uma função só, com o mínimo de conhecimento de
// ProxyManager/PreviewProfiler, para não espalhar isso pela classe.
void MainWindow::autoplay(double seconds, int useProxies, double fromSec,
                          double warmupSec, bool waitProxies, const QString& jsonOut) {
    m_autoplayJsonOut = jsonOut;
    if (useProxies >= 0) {
        m_project.useProxies = useProxies != 0;
        ProxyManager::instance().setProjectUsesProxies(m_project.useProxies);
    }
    if (!m_preview || m_project.duration() <= 0.0) {
        qWarning("[autoplay] projeto vazio ou sem duração — nada a medir.");
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }

    // Loop sobre o projeto inteiro: a reprodução não pode parar no fim de uma
    // faixa e polluir a comparação com um trecho sem clipe.
    m_preview->setLoopRange(0.0, m_project.duration());
    m_preview->setLoopEnabled(true);

    const double start = std::clamp(fromSec, 0.0, std::max(0.0, m_project.duration() - 0.5));
    if (waitProxies)
        awaitProxiesThenMeasure(start, warmupSec, seconds);
    else
        startMeasuredRun(start, warmupSec, seconds);
}

// ── Harness de SCRUB (pierrot --scrub <projeto>) ───────────────────────────
// O playback linear cobre metade dos sintomas; o SCRUB (saltos do playhead
// por todo o projeto) é o caminho mais friccionado da edição e nunca tinha
// métrica automática. Padrão DETERMINÍSTICO (reprodutível entre máquinas):
// a cada 250 ms o playhead salta ~7% do projeto, com volta à cabeça a cada
// 8 passos e um salto longo — cobre cortes, bordas, região densa e a
// cabeça da timeline. Espelha a fiação real do mouse: setPlayhead (UI) +
// PreviewWidget::seek (pipeline) — exatamente o que playheadChanged faz.
void MainWindow::scrubTest(double seconds, const QString& jsonOut) {
    m_autoplayJsonOut = jsonOut;
    if (!m_preview || m_project.duration() <= 0.0) {
        qWarning("[scrub] projeto vazio ou sem duração — nada a medir.");
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }
    const double dur = m_project.duration();
    PreviewProfiler::instance().beginSession(QStringLiteral("scrub"),
                                             m_project.useProxies, m_project.fps);
    auto step = std::make_shared<int>(0);
    auto* timer = new QTimer(this);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, [this, dur, step]() {
        const int i = (*step)++;
        double frac;
        if (i % 8 == 7) frac = 0.0;              // volta à cabeça
        else if (i % 16 == 12) frac = 0.97;      // salto longo pro fim
        else frac = std::fmod(i * 0.07, 1.0);    // degraus + bordas
        const double t = std::clamp(frac * dur, 0.0, std::max(0.0, dur - 0.001));
        m_timeline->setPlayhead(t);
        m_preview->seek(t);
        // Amostra por passo (scrub não tem ticks do engine — sem isto o
        // relatório sairia zerado).
        m_preview->profileStep();
    });
    timer->start();
    QTimer::singleShot(int(seconds * 1000.0), this, [this, timer]() {
        timer->stop();
        PreviewProfiler::instance().endSession();
        const auto& prof = PreviewProfiler::instance();
        const bool ok = prof.dumpJson(m_autoplayJsonOut);
        qInfo("[scrub] relatório %s: %d quadros, cortes=%d, dropped=%lld",
              ok ? "gravado" : "FALHOU", prof.frameCount(), prof.cuts(),
              static_cast<long long>(prof.droppedTotal()));
        // Saida (mesma martelada de quit do autoplay): mata qualquer loop
        // interno do Qt até o processo morrer.
        std::thread([] {
            for (int i = 0; i < 100; ++i) {
                QThread::msleep(200);
                QCoreApplication::quit();
            }
        }).detach();
    });
}

void MainWindow::createProject(int width, int height, int fps, const QString& name) {
    newProject();
    m_project.width = width;
    m_project.height = height;
    m_project.fps = fps;
    m_project.name = name;
    applyUndoState();

    // Salva automaticamente em Documentos/ sem pedir o nome de novo — a
    // tela de boas-vindas já perguntou o nome do projeto; o segundo
    // diálogo "Salvar projeto" só confundia (usuário tinha que digitar o
    // nome duas vezes). Se já existir, acrescenta um número.
    QString base = name.trimmed();
    if (base.isEmpty()) base = tr("Sem título");
    base.replace(QLatin1Char('/'), QLatin1Char('-'));
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QDir().mkpath(dir);
    QString path = dir + QLatin1Char('/') + base + QStringLiteral(".Blanc");
    int n = 2;
    while (QFile::exists(path)) {
        path = dir + QLatin1Char('/') + base
               + QStringLiteral(" (%1).Blanc").arg(n++);
    }
    writeProjectFile(path);
    updateTitle();
}

bool MainWindow::saveProject() {
    if (m_currentFile.isEmpty()) return saveProjectAs();
    writeProjectFile(m_currentFile);
    return true;
}

bool MainWindow::saveProjectAs() {
    const QString suggested = m_project.name.trimmed().isEmpty()
        ? QStringLiteral("Sem título.Blanc")
        : m_project.name.trimmed() + ".Blanc";
    QString path = QFileDialog::getSaveFileName(
        this, tr("Salvar projeto"), suggested,
        tr("Pierrot (*.Blanc);;Todos os arquivos (*)"));
    if (path.isEmpty()) return false;
    if (!path.endsWith(".Blanc", Qt::CaseInsensitive)
        && !path.endsWith(".ovp", Qt::CaseInsensitive))
        path += ".Blanc";
    writeProjectFile(path);
    return true;
}

// Save assíncrono: serializa na UI thread (o modelo só é mutado nela — garante
// um snapshot consistente sem data race com a edição), e grava em disco +
// backup rotativo num worker QtConcurrent. A UI não congela em projetos
// grandes. Erros voltam ao usuário via QMessageBox quando o worker termina.
// Detalhes de reentrância (documentados em MainWindow.h):
//   * save durante save em voo → enfileirado e re-disparado ao terminar;
//   * edição durante a gravação → m_modified permanece true (comparação de
//     revision()) para o título não mentir;
//   * no fechamento do app, closeEvent() espera o worker terminar.
void MainWindow::writeProjectFile(const QString& path, bool autoSave) {
    if (m_saveBusy) {
        m_savePending = true;
        m_queuedPath = path;
        m_queuedAuto = autoSave;
        statusBar()->showMessage(tr("Salvamento em andamento — aguarde."));
        return;
    }

    const QByteArray data =
        QJsonDocument(m_project.toJson()).toJson(QJsonDocument::Indented);
    const quint64 rev = m_project.revision;
    const quint64 gen = m_projGen;
    m_saveAuto = autoSave;
    m_saveBusy = true;
    statusBar()->showMessage(tr("Salvando..."));

    auto* watcher = new QFutureWatcher<SaveWriteResult>(this);
    connect(watcher, &QFutureWatcher<SaveWriteResult>::finished, this,
            [this, watcher, path, rev, gen]() {
        watcher->deleteLater();
        m_saveBusy = false;
        const bool queued = m_savePending;
        const QString queuedPath = m_queuedPath;
        const bool queuedAuto = m_queuedAuto;
        m_savePending = false;
        // O projeto pode ter sido trocado enquanto o save estava em voo
        // (confirmDiscardChanges → novo/abrir). Nesse caso o arquivo gravado
        // é do projeto antigo: não mexer no m_currentFile/título/dirty-state.
        const bool sameProj = (m_projGen == gen);

        const SaveWriteResult res = watcher->result();
        if (!res.ok) {
            if (sameProj) {
                m_modified = true;
                updateTitle();
            }
            QMessageBox::warning(this, tr("Salvar projeto"),
                                 tr("Não foi possível gravar o arquivo:\n%1\n\n%2")
                                     .arg(path, res.detail));
        } else if (sameProj) {
            m_currentFile = path;
            addRecentProject(path);
            // Se o usuário editou durante a gravação, o arquivo gravado não
            // reflete o estado atual → mantém o projeto marcado como sujo.
            if (m_project.revision == rev)
                m_modified = false;
            updateTitle();
            statusBar()->showMessage(
                m_saveAuto
                    ? tr("Projeto salvo automaticamente (%1).")
                          .arg(QTime::currentTime().toString(QStringLiteral("HH:mm")))
                    : tr("Projeto salvo: %1").arg(path));
        } else {
            statusBar()->showMessage(tr("Projeto fechado salvo: %1").arg(path));
        }

        if (queued && sameProj)
            writeProjectFile(queuedPath, queuedAuto);
    });

    watcher->setFuture(QtConcurrent::run([path, data]() -> SaveWriteResult {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return { false, file.errorString() };
        file.write(data);
        if (!file.commit())
            return { false, file.errorString() };
        rotatingBackup(path);
        return { true, QString() };
    }));
}

void MainWindow::autoSave() {
    if (m_currentFile.isEmpty()) {
        statusBar()->showMessage(
            tr("Salvamento automático: salve o projeto uma vez (Ctrl+S) para ativar."));
        return;
    }
    writeProjectFile(m_currentFile, true);
}

void MainWindow::addRecentProject(const QString& path) {
    if (path.isEmpty()) return;
    QSettings s;
    QStringList rec = s.value("recentProjects").toStringList();
    rec.removeAll(path);
    rec.prepend(path);
    while (rec.size() > 10) rec.removeLast();
    s.setValue("recentProjects", rec);
}


