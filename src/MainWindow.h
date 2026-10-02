// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QMainWindow>
#include <QVector>
#include <QHash>
#include <QByteArray>
#include <QStringList>
#include <QElapsedTimer>
#include <memory>
#include <QJsonDocument>
#include <QIcon>
#include <QDockWidget>
#include <QPointer>
#include <QActionGroup>
class QVBoxLayout;
class QMenu;
#include <functional>
#include <deque>
#include "colombina/models/Project.h"

class MediaPoolWidget;
class TimelineWidget;
class PreviewWidget;
class PancropWidget;
class GraphEditorWidget;
class EffectsWidget;
class ExpressWidget;
class ClipPropertiesWidget;
class FileBrowserWidget;
class MixerWidget;
class MesaWidget;
class OfxPluginManager;
class QListWidget;
class QAction;
class QColor;
class QPainter;
class QTimer;
class QProgressBar;
class QComboBox;
class ScopeWidget;
class MaskEditorDialog;
class PreviewMonitor;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void openProjectFile(const QString& path);
    void createProject(int width, int height, int fps, const QString& name);

    // Harness de reprodução sem interação (`pierrot --autoplay`). Abre o
    // projeto, fixa a preferência de proxy, espera os proxies ficarem prontos
    // (opcional), reproduz por `seconds` e grava o relatório do
    // PreviewProfiler em `jsonOut`. `useProxies` < 0 mantém o que o projeto
    // pede; `warmupSec` descarta o início da reprodução (cache/decoders ainda
    // frios) antes de começar a medir.
    void autoplay(double seconds, int useProxies, double fromSec,
                  double warmupSec, bool waitProxies, const QString& jsonOut);
protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool event(QEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;
private slots:
    void pushUndo();
    void setModified();
    void autoSave();
private:
    // Estado compartilhado da espera por proxies: vive no heap porque o timer
    // de polling sobrevive ao retorno de awaitProxiesThenMeasure().
    struct ProxyWaitState {
        QElapsedTimer waited;
        int tries = 0;
    };
    void awaitProxiesThenMeasure(double start, double warmupSec, double seconds);
    void startMeasuredRun(double start, double warmupSec, double seconds);
    QString m_autoplayJsonOut;

    void setUndoLabel(const QString& label);
    void jumpToUndo(int index);
    void updateHistoryList();
    void createDocks();
    // Cria um dock com o boilerplate padrão (objectName estável para
    // saveState/restoreState, áreas permitidas, features) e o registra em
    // m_allDocks. `content` pode ser nullptr: o widget do dock é montado
    // depois (a Timeline é montada em createActions()).
    QDockWidget* makeDock(const QString& objectName, const QString& title,
                          QWidget* content, Qt::DockWidgetArea area);
    void setTabPositionsUp();
    // Larguras iniciais das colunas de docks (item 5 do roadmap). Aplicado
    // uma vez no primeiro showEvent, quando o Qt já calculou o layout.
    void applyInitialDockWidths();
    // Workspaces (item 3 do roadmap 0.7): N arranjos nomeados de painéis, no
    // estilo Premiere. O slot "layout" do QSettings continua sendo o workspace
    // corrente, então o layout existente do usuário vira o workspace "Edição"
    // sem perda. Os demais ficam em workspaces/<nome>/state.
    void rebuildWorkspaceMenu();
    void applyWorkspace(const QString& name);
    void captureCurrentWorkspace();
    void saveWorkspaceAs(const QString& name);
    QStringList workspaceNames() const;
    void createActions();
    void saveSettings();
    void restoreSettings();
    void scheduleLayoutSave();
    void openMaskEditor(const QString& id);
    void setDockLocked(bool locked);
    void showPropsWindow();
    QIcon makeIcon(const std::function<void(QPainter&, const QColor&)>& draw) const;
    // Ícone SVG (recurso) recolorido para a cor do tema, hiDPI. Vazio se não existir.
    QIcon makeSvgIcon(const QString& resourcePath) const;
    QIcon padlockIcon(bool locked) const;
    QIcon iconCursor() const;
    QIcon iconMove() const;
    QIcon iconScissors() const;
    QIcon iconRazor() const;
    QIcon iconEnvelope() const;
    QIcon iconZoom() const;
    QIcon iconRipple() const;
    QIcon iconRolling() const;
    QIcon iconSlip() const;
    QIcon iconSlide() const;
    QIcon iconRateStretch() const;
    QIcon iconMagnet() const;
    QIcon iconImport() const;
    QIcon iconExport() const;
    QIcon iconPlay() const;
    QIcon iconPause() const;
    QIcon iconStepBack() const;
    QIcon iconStepFwd() const;
    QIcon iconSkipBack() const;
    QIcon iconSkipFwd() const;
    QIcon iconLoopClear() const;
    QIcon iconRippleDelete() const;
    QIcon iconTrackStyle() const;
    QIcon iconGrid() const;
    QIcon iconRuler() const;
    void exportVideo();
    void exportEdl();
    void importEdl();
    void newProject();
    void openProject();
    void reloadProject();
    void showWelcomeWindow();
    bool saveProject();
    bool saveProjectAs();
    // Pergunta se deseja salvar antes de descartar alterações não salvas.
    // Retorna true se pode prosseguir (salvou/descartou/não havia mudanças)
    // e false se o usuário cancelou.
    bool confirmDiscardChanges();
    void projectSettings();
    void openSettings();
    void undo();
    void redo();
    void applyUndoState();
    QByteArray snapshotState() const;
    void restoreSnapshot(const QByteArray& snap);
    bool mixerHasAutomation() const;
    void updateTitle();
    void updateUndoActions();
    void addRecentProject(const QString& path);
    // Save assíncrono: serializa na UI thread (snapshot consistente — o modelo
    // só é mutado nela), grava em disco + faz backup rotativo num worker
    // QtConcurrent. A UI não congela em projetos grandes. autoSave=true só muda
    // a mensagem de status (manual vs. "salvo automaticamente").
    void writeProjectFile(const QString& path, bool autoSave = false);

    // Evita que restoreSettings() dispare saveSettings() via setChecked() do
    // cadeado antes da janela ser montada, sobrescrevendo o layout salvo.
    bool m_restoringSettings = false;
    bool m_layoutRestored = false;

    Project m_project;
    // Snapshots de undo em JSON comprimido. std::deque dá eviction O(1) (a
    // pilha antiga fazia removeAt(0) num QVector, copiando as 59 entradas a
    // cada eviction). Empilhar 60 cópias em memória de um projeto grande faria
    // a RAM explodir, então além do limite de contagem há um teto de bytes.
    static constexpr int kUndoMaxEntries = 60;
    static constexpr qint64 kUndoMaxBytes = 256LL * 1024 * 1024; // 256 MB
    std::deque<QByteArray> m_undoStack;
    int m_undoIndex = 0;
    std::deque<QString> m_undoLabels;    // paralelo a m_undoStack (descrição do passo)
    qint64 m_undoBytes = 0;              // soma dos tamanhos comprimidos (eviction por memória)
    QString m_pendingUndoLabel;    // rótulo do PRÓXIMO pushUndo (definido pela ação)
    QListWidget* m_histList = nullptr;   // painel de histórico undo/redo
    QDockWidget* m_histDock = nullptr;
    QString m_currentFile;
    bool m_modified = false;

    // Estado do save assíncrono (QtConcurrent): a escrita em disco e o backup
    // rotativo rodam fora da UI. Se outro save chegar enquanto um está em voo,
    // o pedido é enfileirado (m_savePending) e re-disparado ao terminar; se o
    // usuário editar durante a gravação, o projeto permanece marcado como sujo
    // (m_modified continua true) via comparação de revision() capturada.
    bool m_saveBusy = false;
    bool m_savePending = false;
    bool m_saveAuto = false;     // tipo (manual/autosave) do save em voo
    bool m_queuedAuto = false;   // tipo do save enfileirado (o último pedido)
    QString m_queuedPath;        // caminho do save enfileirado (se houver)
    // Geração do projeto: incrementada a cada troca de projeto (novo/aberto/
    // importado). O callback do save assíncrono só toca m_currentFile/título/
    // dirty-state se a geração ainda for a mesma — um save disparado por
    // confirmDiscardChanges() nunca pode "vazar" para o projeto recém-aberto.
    quint64 m_projGen = 0;

    MediaPoolWidget* m_pool = nullptr;
    TimelineWidget* m_timeline = nullptr;
    PreviewWidget* m_preview = nullptr;
    PancropWidget* m_pancrop = nullptr;
    GraphEditorWidget* m_graph = nullptr;
    EffectsWidget* m_effects = nullptr;
    ExpressWidget* m_express = nullptr;
    ClipPropertiesWidget* m_props = nullptr;
    FileBrowserWidget* m_fileBrowser = nullptr;
    MixerWidget* m_mixer = nullptr;
    MesaWidget* m_mesa = nullptr;
    QPointer<MaskEditorDialog> m_maskDialog;  // janela de máscara (única)
    QString m_maskDialogClipId;               // clipe que está sendo editado no momento
    QDockWidget* m_poolDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    QDockWidget* m_toolsDock = nullptr; // paleta vertical de ferramentas (Tools)
    QDockWidget* m_pancropDock = nullptr;
    QDockWidget* m_graphDock = nullptr;
    QDockWidget* m_effectsDock = nullptr;
    QDockWidget* m_expressDock = nullptr;
    QDockWidget* m_fileBrowserDock = nullptr;
    QDockWidget* m_mixerDock = nullptr;
    QDockWidget* m_mesaDock = nullptr;
    ScopeWidget* m_scopes = nullptr;      // analisadores (waveform/histograma/vectorscope)
    QComboBox* m_scopeMode = nullptr;
    QDockWidget* m_scopesDock = nullptr;
    QDockWidget* m_propsDock = nullptr;     // Inspector (passo 3, ROADMAP 7.1)
    // Registro de todos os docks criados por makeDock(), na ordem de criação.
    // O menu Exibir e o salvamento do layout percorrem esta lista em vez de
    // repetir os nomes à mão.
    QVector<QDockWidget*> m_allDocks;
    // Workspaces nomeados. m_workspaceMenu fica sob Exibir; o QActionGroup
    // garante que só um workspace apareça marcado (item 4 do roadmap).
    QMenu* m_workspaceMenu = nullptr;
    QActionGroup* m_workspaceGroup = nullptr;
    QString m_currentWorkspace;
    // Arranjo padrão dos docks, capturado logo após createDocks(). É o
    // fallback de um workspace que ainda não tem estado salvo.
    QByteArray m_defaultLayoutState;
    bool m_widthsApplied = false;   // larguras padrão já aplicadas (1x)
    bool m_hasRestoredLayout = false;  // havia arranjo salvo no QSettings
    QVBoxLayout* m_centralLay = nullptr;
    QHash<QDockWidget*, QDockWidget::DockWidgetFeatures> m_originalFeatures;
    QAction* m_lockAction = nullptr;
    QAction* m_playAction = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_saveAction = nullptr;
    QAction* m_saveAsAction = nullptr;
    QAction* m_snapAction = nullptr;
    QAction* m_monitorAction = nullptr;
    QVector<QAction*> m_toolActions;
    QTimer* m_autoSaveTimer = nullptr;
    QTimer* m_layoutSaveTimer = nullptr;
    QTimer* m_monitorTimer = nullptr;
    QProgressBar* m_busyBar = nullptr;
    OfxPluginManager* m_ofxManager = nullptr;
    PreviewMonitor* m_monitor = nullptr; // preview externo (janela própria/2º monitor)
};
