// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/dialogs/SettingsDialog.h"
#include "ui/core/Theme.h"
#include "colombina/ffmpeg/ProxyManager.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QFont>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QListWidget>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QHeaderView>
#include <QPushButton>
#include <QFileDialog>
#include <QFile>
#include <QDesktopServices>
#include <QUrl>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QKeySequence>
#include <QFileDialog>
#include <QLineEdit>
#include <QProcess>
#include <QInputDialog>
#include <QMenu>
#include <QToolButton>
#include <QSignalBlocker>

// Registro dos atalhos remapeáveis (id, rótulo, padrão, categoria).
namespace {
struct ShortcutDef { const char* id; const char* label; const char* def; const char* cat; };
const ShortcutDef kShortcutDefs[] = {
    { "new",     "Novo projeto",        "Ctrl+N",            "Projeto" },
    { "open",    "Abrir projeto",       "Ctrl+O",            "Projeto" },
    { "save",    "Salvar projeto",      "Ctrl+S",            "Projeto" },
    { "saveas",  "Salvar como",         "Ctrl+Shift+S",      "Projeto" },
    { "import",  "Importar mídia",      "Ctrl+I",            "Projeto" },
    { "export",  "Exportar vídeo",      "Ctrl+E",            "Projeto" },
    { "undo",    "Desfazer",            "Ctrl+Z",            "Edição" },
    { "redo",    "Refazer",             "Ctrl+Shift+Z",      "Edição" },
    { "delete",  "Excluir",             "Del",               "Edição" },
    { "cut",     "Dividir no playhead", "S",                 "Edição" },
    { "tool0",   "Selecionar",          "0",                 "Ferramentas" },
    { "tool1",   "Mover",               "M",                 "Ferramentas" },
    { "tool2",   "Tesoura",             "R",                 "Ferramentas" },
    { "tool3",   "Envelope",            "E",                 "Ferramentas" },
    { "tool4",   "Lupa (Zoom)",         "Z",                 "Ferramentas" },
    { "tool5",   "Ripple",              "B",                 "Ferramentas" },
    { "tool6",   "Rolling",             "N",                 "Ferramentas" },
    { "tool7",   "Slip",                "Y",                 "Ferramentas" },
    { "tool8",   "Slide",               "Ctrl+U",            "Ferramentas" },
    { "tool9",   "Esticar Velocidade",  "W",                 "Ferramentas" },
    { "snap",    "Snap (ímã)",          "T",                 "Ferramentas" },
    { "play",    "Reproduzir/Pausar",   "Space",             "Reprodução" },
    { "shuttleRev", "Retroceder (JKL)",  "J",                 "Reprodução" },
    { "shuttleFwd", "Avançar (JKL)",     "L",                 "Reprodução" },
    { "shuttlePause", "Pausar (JKL)",    "K",                 "Reprodução" },
    { "stepL",   "Voltar 1 frame",      "Left",              "Reprodução" },
    { "stepR",   "Avançar 1 frame",     "Right",             "Reprodução" },
    { "nudgeL",  "Deslocar clipes ←",   "Alt+Left",          "Edição" },
    { "nudgeR",  "Deslocar clipes →",   "Alt+Right",         "Edição" },
    { "goStart", "Início",              "Home",              "Reprodução" },
    { "goEnd",   "Fim",                 "End",               "Reprodução" },
};
constexpr int kShortcutCount = (int)(sizeof(kShortcutDefs) / sizeof(kShortcutDefs[0]));

static QString shortcutText(const ShortcutDef& sd) {
    const QString v = QSettings().value(QString("shortcuts/%1").arg(QLatin1String(sd.id))).toString();
    return v.isEmpty() ? QKeySequence(QString::fromUtf8(sd.def)).toString() : v;
}

// ── Perfis de atalhos ───────────────────────────────────────────────────
// Todo perfil vive em shortcutProfiles/<nome>/<id> (lista de nomes em
// shortcutProfiles/names, ativo em "shortcutsProfile"). O "Pierrot" é o
// único embutido — não pode ser renomeado/excluído — e nasce por migração
// a partir das chaves vivas shortcuts/<id> (legado de antes dos perfis).
// As chaves shortcuts/<id> continuam sendo o espelho "vivo" que o app lê
// (appKey), gravado no OK a partir do perfil ativo.
static const QString kPierrotProfile = QStringLiteral("Pierrot");

static QStringList shortcutProfileNames() {
    return QSettings().value(QStringLiteral("shortcutProfiles/names")).toStringList();
}

static void setShortcutProfileNames(const QStringList& names) {
    QSettings().setValue(QStringLiteral("shortcutProfiles/names"), names);
}

// Mapa id → tecla de um perfil (só overrides; o que falta = padrão da def).
static QMap<QString, QString> loadShortcutProfileMap(const QString& name) {
    QSettings s;
    QMap<QString, QString> m;
    for (int i = 0; i < kShortcutCount; ++i) {
        const QString id = QString::fromLatin1(kShortcutDefs[i].id);
        const QString v = s.value(QStringLiteral("shortcutProfiles/%1/%2").arg(name, id))
                              .toString();
        if (!v.isEmpty()) m.insert(id, v);
    }
    return m;
}

// Grava (ou apaga, se o id não está no mapa) as entradas de um perfil.
static void saveShortcutProfileMap(const QString& name, const QMap<QString, QString>& m) {
    QSettings s;
    for (int i = 0; i < kShortcutCount; ++i) {
        const QString id = QString::fromLatin1(kShortcutDefs[i].id);
        const QString key = QStringLiteral("shortcutProfiles/%1/%2").arg(name, id);
        if (m.contains(id)) s.setValue(key, m.value(id));
        else s.remove(key);
    }
}

// Apaga todas as chaves de um perfil do usuário (exclusão).
static void removeShortcutProfile(const QString& name) {
    QSettings s;
    for (int i = 0; i < kShortcutCount; ++i)
        s.remove(QStringLiteral("shortcutProfiles/%1/%2")
                     .arg(name, QString::fromLatin1(kShortcutDefs[i].id)));
}

// Migração única: na primeira execução com o sistema de perfis, o que
// estiver nas chaves vivas (shortcuts/<id>) vira o perfil Pierrot.
static void migrateLegacyShortcuts() {
    QSettings s;
    if (s.contains(QStringLiteral("shortcutsProfile"))) return;
    for (int i = 0; i < kShortcutCount; ++i) {
        const QString id = QString::fromLatin1(kShortcutDefs[i].id);
        const QString v = s.value(QStringLiteral("shortcuts/%1").arg(id)).toString();
        if (!v.isEmpty())
            s.setValue(QStringLiteral("shortcutProfiles/%1/%2").arg(kPierrotProfile, id), v);
    }
    s.setValue(QStringLiteral("shortcutsProfile"), kPierrotProfile);
}
}

bool SettingsDialog::mkvWarningEnabled() {
    return QSettings().value("mkvWarning", true).toBool();
}

bool SettingsDialog::warn4kEnabled() {
    return QSettings().value("warn4k", true).toBool();
}

int SettingsDialog::thumbMode() {
    return QSettings().value("timelineThumbMode", 0).toInt();
}

bool SettingsDialog::rippleDeleteEnabled() {
    return QSettings().value("timelineRippleDelete", true).toBool();
}

bool SettingsDialog::trimmerEnabled() {
    return QSettings().value("timelineTrimmer", false).toBool();
}

double SettingsDialog::graphSensitivity() {
    return QSettings().value("graphSensitivity", 1.0).toDouble();
}

QStringList SettingsDialog::ofxSearchPaths() {
    return QSettings().value("ofxSearchPaths").toStringList();
}

bool SettingsDialog::exportDefaultDirEnabled() {
    return QSettings().value("exportDefaultDirEnabled", false).toBool();
}

QString SettingsDialog::exportDefaultDir() {
    return QSettings().value("exportDefaultDir").toString();
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Configurações"));
    setMinimumSize(860, 600);

    QSettings s;

    m_mkvWarn = new QCheckBox(tr("Avisar ao importar arquivos MKV (experimental)"), this);
    m_mkvWarn->setChecked(s.value("mkvWarning", true).toBool());

    m_autoSave = new QCheckBox(tr("Ativar salvamento automático"), this);
    m_autoSave->setChecked(s.value("autosaveEnabled", false).toBool());

    m_autoInterval = new QSpinBox(this);
    m_autoInterval->setRange(1, 1440);
    m_autoInterval->setSuffix(tr(" min"));
    m_autoInterval->setValue(s.value("autosaveMinutes", 10).toInt());

    m_thumbMode = new QComboBox(this);
    m_thumbMode->addItem(tr("Todas (contínuas)"));
    m_thumbMode->addItem(tr("Início e fim"));
    m_thumbMode->addItem(tr("Nenhuma"));
    m_thumbMode->setCurrentIndex(s.value("timelineThumbMode", 0).toInt());

    // Tema da interface.
    m_themeCombo = new QComboBox(this);
    // A ordem TEM de casar com os valores de AppTheme: o índice do combo é o
    // que vai pro saveTheme().
    m_themeCombo->addItem(tr("Escuro"));
    m_themeCombo->addItem(tr("Claro (Final Cut)"));
    m_themeCombo->addItem(tr("Ridge Racer (R4)"));
    m_themeCombo->addItem(tr("Japan 95"));
    m_themeCombo->setCurrentIndex(int(savedTheme()));

    m_rippleDelete = new QCheckBox(tr("Fechar o vão automaticamente ao excluir (ripple)"), this);
    m_rippleDelete->setChecked(s.value("timelineRippleDelete", true).toBool());

    m_trimmer = new QCheckBox(tr("Abrir o trimmer ao soltar uma mídia na timeline"), this);
    m_trimmer->setChecked(s.value("timelineTrimmer", false).toBool());

    // Navegação por categorias (sidebar vertical estilo DaVinci Resolve).
    m_catList = new QListWidget(this);
    m_catList->setFixedWidth(170);
    m_catList->addItem(tr("Geral"));
    m_catList->addItem(tr("Qualidade e Timeline"));
    m_catList->addItem(tr("Renderizar"));
    m_catList->addItem(tr("Efeitos"));
    m_catList->addItem(tr("Plugins OFX"));
    m_catList->addItem(tr("Atalhos do teclado"));
    m_catList->setToolTip(tr("Escolha uma categoria"));

    m_stack = new QStackedWidget(this);

    // Página: Geral (avisos + salvamento automático).
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(4, 4, 4, 4);
        auto* mkvBox = new QGroupBox(tr("Avisos"), page);
        auto* mkvLay = new QVBoxLayout(mkvBox);
        mkvLay->addWidget(m_mkvWarn);
        auto* mkvHint = new QLabel(tr("MKV é experimental: alguns arquivos podem não abrir "
                                      "ou apresentar problemas de áudio/vídeo."), mkvBox);
        mkvHint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        mkvHint->setWordWrap(true);
        mkvLay->addWidget(mkvHint);
        auto* themeBox = new QGroupBox(tr("Aparência"), page);
        auto* themeLay = new QFormLayout(themeBox);
        themeLay->addRow(tr("Tema:"), m_themeCombo);
        auto* themeHint = new QLabel(tr("Requer reinício do aplicação para tomar efeito."), themeBox);
        themeHint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        themeHint->setWordWrap(true);
        themeLay->addWidget(themeHint);
        // Tema por paleta: define as CORES de toda a UI (ThemeColors),
        // cobrindo inclusive o que é desenhado por código (timeline,
        // playhead, monitor, gráficos). Chaves ausentes herdam do tema
        // escolhido em "Tema:".
        auto* palBox = new QGroupBox(tr("Tema por paleta (.pierrot-theme)"), page);
        auto* palLay = new QVBoxLayout(palBox);
        m_extPaletteCombo = new QComboBox(palBox);
        m_extPaletteCombo->addItem(tr("(nenhum)"));
        for (const QString& name : listExternalPalettes())
            m_extPaletteCombo->addItem(name);
        {
            const QString cur = savedExternalPalette();
            int idx = cur.isEmpty() ? 0 : m_extPaletteCombo->findText(cur);
            m_extPaletteCombo->setCurrentIndex(idx < 0 ? 0 : idx);
        }
        palLay->addWidget(m_extPaletteCombo);
        auto* palRow = new QHBoxLayout;
        auto* palImport = new QPushButton(tr("Importar…"), palBox);
        auto* palExport = new QPushButton(tr("Exportar tema atual…"), palBox);
        auto* palRemove = new QPushButton(tr("Remover"), palBox);
        auto* palOpen = new QPushButton(tr("Abrir pasta"), palBox);
        palRow->addWidget(palImport);
        palRow->addWidget(palExport);
        palRow->addWidget(palRemove);
        palRow->addWidget(palOpen);
        palRow->addStretch(1);
        palLay->addLayout(palRow);
        auto* palHint = new QLabel(tr("Cobre o aplicativo inteiro (timeline, playhead, "
                                      "monitor, gráficos) por definir as cores de toda a "
                                      "UI. Chaves ausentes herdam do tema acima. "
                                      "Requer reinício do aplicativo."), palBox);
        palHint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        palHint->setWordWrap(true);
        palLay->addWidget(palHint);
        // Importar: copia o .pierrot-theme para a pasta de temas e já o ativa.
        connect(palImport, &QPushButton::clicked, this, [this]() {
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Importar tema por paleta (.pierrot-theme)"), QDir::homePath(),
                tr("Tema por paleta Pierrot (*.pierrot-theme)"));
            if (path.isEmpty())
                return;
            const QFileInfo fi(path);
            QDir().mkpath(externalThemesDir());
            const QString dst = externalThemesDir() + QLatin1Char('/') + fi.fileName();
            if (QFileInfo(path).absoluteFilePath() != QFileInfo(dst).absoluteFilePath()) {
                if (QFile::exists(dst))
                    QFile::remove(dst);
                if (!QFile::copy(path, dst)) {
                    QMessageBox::warning(this, tr("Importar tema"),
                                         tr("Não foi possível copiar o arquivo."));
                    return;
                }
            }
            int idx = m_extPaletteCombo->findText(fi.fileName());
            if (idx < 0) {
                m_extPaletteCombo->addItem(fi.fileName());
                idx = m_extPaletteCombo->count() - 1;
            }
            m_extPaletteCombo->setCurrentIndex(idx);
        });
        // Exportar: grava a paleta do tema atual como .pierrot-theme.
        connect(palExport, &QPushButton::clicked, this, [this]() {
            const QString path = QFileDialog::getSaveFileName(
                this, tr("Exportar tema atual"),
                QDir::homePath() + QLatin1String("/meu-tema.pierrot-theme"),
                tr("Tema por paleta Pierrot (*.pierrot-theme)"));
            if (path.isEmpty())
                return;
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Text)
                || f.write(exportCurrentPalette().toUtf8()) < 0) {
                QMessageBox::warning(this, tr("Exportar tema"),
                                     tr("Não foi possível escrever o arquivo."));
            }
        });
        // Remover: apaga o arquivo selecionado da pasta de temas.
        connect(palRemove, &QPushButton::clicked, this, [this]() {
            const int idx = m_extPaletteCombo->currentIndex();
            if (idx <= 0)
                return;
            const QString name = m_extPaletteCombo->currentText();
            if (QMessageBox::question(this, tr("Remover tema"),
                                      tr("Remover o tema \"%1\"?").arg(name))
                != QMessageBox::Yes)
                return;
            QFile::remove(externalThemesDir() + QLatin1Char('/') + name);
            m_extPaletteCombo->removeItem(idx);
            m_extPaletteCombo->setCurrentIndex(0);
        });
        // Abrir pasta: mostra a pasta de temas no gerenciador de arquivos.
        connect(palOpen, &QPushButton::clicked, this, []() {
            QDir().mkpath(externalThemesDir());
            QDesktopServices::openUrl(QUrl::fromLocalFile(externalThemesDir()));
        });
        // Idioma da interface (índices casam com savedLanguage():
        // 0=pt, 1=en, 2=ja).
        auto* langBox = new QGroupBox(tr("Idioma"), page);
        auto* langLay = new QFormLayout(langBox);
        m_langCombo = new QComboBox(page);
        m_langCombo->addItem(tr("Brasileiro"));
        m_langCombo->addItem(tr("Inglês"));
        m_langCombo->addItem(QString::fromUtf8("日本語"));
        m_langCombo->setCurrentIndex(qBound(0, s.value("language", 0).toInt(),
                                            m_langCombo->count() - 1));
        langLay->addRow(tr("Língua:"), m_langCombo);
        auto* langHint = new QLabel(tr("O japonês e o inglês já estão disponíveis. "
                                       "Requer reinício do aplicativo "
                                       "para tomar efeito."), langBox);
        langHint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        langHint->setWordWrap(true);
        langLay->addWidget(langHint);
        auto* autoBox = new QGroupBox(tr("Salvamento automático"), page);
        auto* autoLay = new QFormLayout(autoBox);
        autoLay->addRow(m_autoSave);
        autoLay->addRow(tr("Intervalo:"), m_autoInterval);
        v->addWidget(mkvBox);
        v->addWidget(themeBox);
        v->addWidget(palBox);
        v->addWidget(langBox);
        v->addWidget(autoBox);
        v->addStretch(1);
        m_stack->addWidget(page);
    }

    // Página: Qualidade e Timeline.
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(4, 4, 4, 4);
        auto* tlBox = new QGroupBox(tr("Timeline"), page);
        auto* tlLay = new QFormLayout(tlBox);
        tlLay->addRow(tr("Miniaturas nos clipes:"), m_thumbMode);
        tlLay->addRow(m_rippleDelete);
        tlLay->addRow(m_trimmer);
        m_graphSens = new QDoubleSpinBox(page);
        m_graphSens->setRange(0.5, 5.0);
        m_graphSens->setSingleStep(0.1);
        m_graphSens->setDecimals(1);
        m_graphSens->setSuffix(" ×");
        m_graphSens->setValue(s.value("graphSensitivity", 1.0).toDouble());
        m_graphSens->setToolTip(tr("Sensibilidade do arraste vertical no editor de "
                                   "curvas. Acima de 1× deixa as curvas mais exageradas."));
        tlLay->addRow(tr("Sensibilidade das curvas:"), m_graphSens);
        auto* tlHint = new QLabel(tr("Como os quadros são exibidos no corpo dos "
                                     "clipes de vídeo. \"Todas\" mostra fatias "
                                     "contínuas; \"Início e fim\" só nos extremos; "
                                     "\"Nenhuma\" deixa os clipes sem miniatura."), tlBox);
        tlHint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        tlHint->setWordWrap(true);
        tlLay->addRow(tlHint);
        v->addWidget(tlBox);

        auto* perfBox = new QGroupBox(tr("Desempenho"), page);
        auto* perfLay = new QVBoxLayout(perfBox);
        m_hwDecode = new QCheckBox(tr("Decodificação de vídeo por hardware (VAAPI)"), page);
        m_hwDecode->setChecked(s.value("hwDecode", true).toBool());
        m_hwDecode->setToolTip(tr("Usa a GPU para decodificar H.264/HEVC/MJPEG. "
                                   "Requer suporte VAAPI no Linux (Intel/AMD/NVIDIA com driver)."));
        perfLay->addWidget(m_hwDecode);
        m_proxies = new QCheckBox(tr("Usar proxies em previews e pan/crop (alta resolução)"), page);
        m_proxies->setChecked(s.value("proxiesEnabled", true).toBool());
        m_proxies->setToolTip(tr("Gera versões leves (H.264 baixo bitrate) de vídeos "
                                  "2K ou maiores para preview, pan/crop e thumbnails. "
                                  "A exportação sempre usa o original."));
        perfLay->addWidget(m_proxies);
        // Limiar do proxy: a política "só 2K+ ganha proxy" (2560) deixava
        // fontes 2048×1152 sem proxy e o corte pagava ~1s de GOP do original.
        auto* proxyRow = new QHBoxLayout;
        auto* proxyLbl = new QLabel(tr("Gerar proxy a partir de (largura):"), page);
        m_proxyMinWidth = new QSpinBox(page);
        m_proxyMinWidth->setRange(320, 8192);
        m_proxyMinWidth->setSingleStep(128);
        m_proxyMinWidth->setSuffix(tr(" px"));
        m_proxyMinWidth->setValue(s.value("proxyMinWidth", 2560).toInt());
        m_proxyMinWidth->setToolTip(tr("Vídeos com largura >= este valor ganham proxy "
                                       "leve para o preview. Baixe para 1920/2048 se o "
                                       "computador aguentar — arquivos 2K também ganham "
                                       "proxy e os cortes ficam lisos. A exportação "
                                       "sempre usa o original."));
        proxyRow->addWidget(proxyLbl);
        proxyRow->addWidget(m_proxyMinWidth);
        proxyRow->addStretch(1);
        perfLay->addLayout(proxyRow);
        auto* perfHint = new QLabel(tr("Transfere a decodificação de vídeo para a "
                                       "placa de vídeo, aliviando a CPU (útil em 4K e "
                                       "projetos com muitos cortes). Se algum arquivo "
                                       "tiver problema, desligue — o Pierrot volta a "
                                       "decodificar por software."), perfBox);
        perfHint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        perfHint->setWordWrap(true);
        perfLay->addWidget(perfHint);
        auto* perfHint2 = new QLabel(tr("Requer reinício do aplicativo para tomar efeito."), perfBox);
        perfHint2->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        perfHint2->setWordWrap(true);
        perfLay->addWidget(perfHint2);
        v->addWidget(perfBox);
        v->addStretch(1);
        m_stack->addWidget(page);
    }

    // Página: Renderizar (pasta padrão de exportação).
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(4, 4, 4, 4);

        m_exportDirEnabled = new QCheckBox(tr("Usar pasta padrão ao exportar"), page);
        m_exportDirEnabled->setChecked(
            QSettings().value("exportDefaultDirEnabled", false).toBool());

        m_exportDirEdit = new QLineEdit(page);
        m_exportDirEdit->setPlaceholderText(QDir::homePath());
        m_exportDirEdit->setText(QSettings().value("exportDefaultDir").toString());
        m_exportDirEdit->setEnabled(m_exportDirEnabled->isChecked());

        auto* browseBtn = new QPushButton(tr("…"), page);
        browseBtn->setFixedWidth(36);
        connect(browseBtn, &QPushButton::clicked, this, [this]() {
            const QString dir = QFileDialog::getExistingDirectory(
                this, tr("Selecionar pasta padrão de render"),
                m_exportDirEdit->text().isEmpty() ? QDir::homePath() : m_exportDirEdit->text());
            if (!dir.isEmpty()) m_exportDirEdit->setText(dir);
        });

        connect(m_exportDirEnabled, &QCheckBox::toggled,
                m_exportDirEdit, &QLineEdit::setEnabled);

        auto* row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(m_exportDirEdit, 1);
        row->addWidget(browseBtn);

        auto* exportBox = new QGroupBox(tr("Pasta padrão de renderização"), page);
        auto* form = new QFormLayout(exportBox);
        form->addRow(m_exportDirEnabled);
        form->addRow(tr("Pasta:"), row);

        auto* hint = new QLabel(tr("Quando ativado, o diálogo de exportação abre "
                                   "nessa pasta em vez da sua pasta pessoal. "
                                   "Deixe vazio para usar a pasta pessoal."), exportBox);
        hint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        hint->setWordWrap(true);
        form->addRow(hint);

        v->addWidget(exportBox);

        // Aceleração de hardware na codificação do export.
        m_hwEncode = new QCheckBox(tr("Codificar vídeo por hardware no export (NVENC/VAAPI)"), page);
        m_hwEncode->setChecked(s.value("exportHwEncode", false).toBool());
        m_hwEncode->setToolTip(tr("Usa a GPU (h264_nvenc ou h264_vaapi) para codificar MP4/MKV.\n"
                                  "Se o ffmpeg do sistema não tiver esses encoders, "
                                  "cai automaticamente para o libx264."));
        auto* hwBox = new QGroupBox(tr("Encode acelerado"), page);
        auto* hwLay = new QVBoxLayout(hwBox);
        hwLay->addWidget(m_hwEncode);

        // Seletor de GPU: lista placas NVIDIA (nvidia-smi) e render nodes VAAPI.
        m_hwGpu = new QComboBox(hwBox);
        m_hwGpu->addItem(tr("Automática"), QString());
        {
            // NVIDIA (NVENC): uma entrada por placa via nvidia-smi.
            QProcess nvi;
            nvi.start(QStringLiteral("nvidia-smi"),
                      {QStringLiteral("--query-gpu=index,name"),
                       QStringLiteral("--format=csv,noheader")});
            if (nvi.waitForFinished(3000)) {
                const QStringList lines = QString::fromUtf8(nvi.readAllStandardOutput())
                                              .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                for (const QString& line : lines) {
                    const QStringList parts = line.split(QLatin1String(", "), Qt::SkipEmptyParts);
                    if (parts.size() >= 2) {
                        const QString id = QStringLiteral("nvenc:") + parts[0].trimmed();
                        m_hwGpu->addItem(
                            tr("NVIDIA %1 — GPU %2").arg(parts[1].trimmed(), parts[0].trimmed()),
                            id);
                    }
                }
            }
            // VAAPI (Intel/AMD Linux): render nodes em /dev/dri.
            const QStringList renders = QDir(QStringLiteral("/dev/dri"))
                                            .entryList({QStringLiteral("renderD*")},
                                                       QDir::System, QDir::Name);
            for (const QString& r : renders)
                m_hwGpu->addItem(tr("VAAPI — %1").arg(r),
                                 QStringLiteral("vaapi:/dev/dri/") + r);
        }
        const QString savedGpu = s.value("exportHwDevice").toString();
        const int gpuIdx = m_hwGpu->findData(savedGpu);
        m_hwGpu->setCurrentIndex(gpuIdx >= 0 ? gpuIdx : 0);
        m_hwGpu->setEnabled(m_hwEncode->isChecked());
        connect(m_hwEncode, &QCheckBox::toggled, m_hwGpu, &QWidget::setEnabled);
        m_hwGpu->setToolTip(tr("Placa usada na codificação por hardware.\n"
                               "\"Automática\" deixa o ffmpeg escolher."));

        auto* gpuRow = new QHBoxLayout;
        gpuRow->addWidget(new QLabel(tr("GPU:"), hwBox));
        gpuRow->addWidget(m_hwGpu, 1);
        hwLay->addLayout(gpuRow);

        auto* hwHint = new QLabel(tr("Descarga a codificação da CPU para a placa de vídeo "
                                     "(útil em 4K e projetos longos). Equivale à aceleração "
                                     "por GPU que o Vega oferece na renderização; o retorno "
                                     "ao libx264 é automático se não houver encoders de "
                                     "hardware no sistema."), hwBox);
        hwHint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        hwHint->setWordWrap(true);
        hwLay->addWidget(hwHint);
        v->addWidget(hwBox);
        v->addStretch(1);
        m_stack->addWidget(page);
    }

    // Página: Efeitos (aviso de status de desenvolvimento).
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(4, 4, 4, 4);

        auto* fxBox = new QGroupBox(tr("Efeitos de vídeo"), page);
        auto* fxLay = new QVBoxLayout(fxBox);

        auto* warn = new QLabel(tr("Os efeitos de vídeo estão em fase de criação e "
                                   "ainda não estão bons para uso."), fxBox);
        warn->setWordWrap(true);
        QFont warnFont = warn->font();
        warnFont.setBold(true);
        warn->setFont(warnFont);
        fxLay->addWidget(warn);

        auto* hint = new QLabel(tr("Esse recurso está sendo desenvolvido e pode "
                                   "apresentar problemas de performance ou "
                                   "resultados incorretos no preview e na "
                                   "exportação. Recomendamos acompanhar as "
                                   "próximas versões do Pierrot antes de usá-lo "
                                   "em projetos reais."), fxBox);
        hint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        hint->setWordWrap(true);
        fxLay->addWidget(hint);

        v->addWidget(fxBox);
        v->addStretch(1);
        m_stack->addWidget(page);
    }

    // Página: Plugins OFX (caminhos de busca de plugins de terceiros).
    {
        auto* page = new QWidget;
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(4, 4, 4, 4);

        auto* ofxBox = new QGroupBox(tr("Caminhos de plugins OFX"), page);
        auto* ofxLay = new QVBoxLayout(ofxBox);

        m_ofxPaths = new QListWidget(ofxBox);
        m_ofxPaths->setMinimumHeight(120);
        const QStringList saved = QSettings().value("ofxSearchPaths").toStringList();
        for (const QString& p : saved)
            m_ofxPaths->addItem(p);
        ofxLay->addWidget(m_ofxPaths);

        auto* btnRow = new QHBoxLayout;
        auto* addBtn = new QPushButton(tr("Adicionar…"), ofxBox);
        auto* remBtn = new QPushButton(tr("Remover"), ofxBox);
        btnRow->addWidget(addBtn);
        btnRow->addWidget(remBtn);
        btnRow->addStretch(1);
        ofxLay->addLayout(btnRow);

        connect(addBtn, &QPushButton::clicked, this, [this, ofxBox]() {
            const QString dir = QFileDialog::getExistingDirectory(
                ofxBox, tr("Selecionar pasta de plugins OFX"));
            if (!dir.isEmpty() && !m_ofxPaths->findItems(dir, Qt::MatchExactly).isEmpty() == false)
                m_ofxPaths->addItem(dir);
        });
        connect(remBtn, &QPushButton::clicked, this, [this]() {
            delete m_ofxPaths->takeItem(m_ofxPaths->currentRow());
        });

        auto* hint = new QLabel(tr("Adicione pastas onde seus plugins OFX (.ofx) estão "
                                    "instalados. O Pierrot já procura em "
                                    "~/.config/pierrot/ofx e /usr/lib/ofx por padrão."), ofxBox);
        hint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().placeholderText.name()));
        hint->setWordWrap(true);
        ofxLay->addWidget(hint);

        v->addWidget(ofxBox);
        v->addStretch(1);
        m_stack->addWidget(page);
    }

    // Página: Atalhos do teclado.
    buildShortcutsPage();

    // Corpo: barra lateral + painel da categoria selecionada.
    auto* body = new QHBoxLayout;
    body->setSpacing(10);
    body->setContentsMargins(10, 10, 10, 0);
    body->addWidget(m_catList, 0, Qt::AlignTop);
    body->addWidget(m_stack, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_catList->setCurrentRow(0);
    m_stack->setCurrentIndex(0);
    connect(m_catList, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 8);
    lay->addLayout(body, 1);
    lay->addWidget(buttons);
}

void SettingsDialog::buildShortcutsPage() {
    migrateLegacyShortcuts();
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);

    // ── Perfil de atalhos ───────────────────────────────────────────────
    // Só o "Padrão (Pierrot)" é embutido; os demais perfis são criados e
    // gerenciados pelo usuário pela engrenagem ao lado (salvar como novo,
    // atualizar, renomear, excluir, exportar/importar arquivo).
    auto* profRow = new QHBoxLayout;
    auto* profLabel = new QLabel(tr("Perfil:"), page);
    auto* profCombo = new QComboBox(page);
    profCombo->addItem(tr("Padrão (Pierrot)"), kPierrotProfile);
    for (const QString& n : shortcutProfileNames())
        profCombo->addItem(n, n);
    auto* profGear = new QToolButton(page);
    profGear->setText(QStringLiteral("⚙"));
    profGear->setToolTip(tr("Gerenciar perfis de atalhos"));
    profGear->setAutoRaise(true);
    profGear->setPopupMode(QToolButton::InstantPopup);
    auto* gearMenu = new QMenu(profGear);
    profGear->setMenu(gearMenu);
    profRow->addWidget(profLabel);
    profRow->addWidget(profCombo, 1);
    profRow->addWidget(profGear);
    profRow->addStretch(1);

    // Exportar atalhos como texto simples (id=seq por linha).
    auto doExport = [this]() {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Exportar atalhos"), QStringLiteral("pierrot-atalhos.txt"),
            tr("Texto (*.txt)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        // Grava o mapa atual (temporário) com fallback para o def.
        for (int i = 0; i < kShortcutCount; ++i) {
            const ShortcutDef& sd = kShortcutDefs[i];
            const QString v = m_shortcutMap.value(QLatin1String(sd.id),
                                                   QKeySequence(QString::fromUtf8(sd.def)).toString());
            f.write(QByteArray(sd.id) + "=" + v.toUtf8() + "\n");
        }
    };

    // Importar atalhos de um arquivo id=seq (preenche o mapa em memória;
    // vira perfil do usuário ao "Salvar como novo perfil…").
    auto doImport = [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Importar atalhos"), QString(), tr("Texto (*.txt)"));
        if (path.isEmpty()) return;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        m_shortcutMap.clear();
        while (!f.atEnd()) {
            const QByteArray line = f.readLine().trimmed();
            const int eq = line.indexOf('=');
            if (eq <= 0) continue;
            const QString id = QString::fromUtf8(line.left(eq)).trimmed();
            const QString seq = QString::fromUtf8(line.mid(eq + 1)).trimmed();
            bool known = false;
            for (int i = 0; i < kShortcutCount; ++i)
                if (QLatin1String(kShortcutDefs[i].id) == id) { known = true; break; }
            if (known) m_shortcutMap.insert(id, seq);
        }
        refreshAllShortcutRows();
    };

    // Engrenagem: reconstruída a cada abertura, com as ações válidas para o
    // perfil selecionado (Pierrot não renomeia/exclui).
    connect(gearMenu, &QMenu::aboutToShow, this, [this, profCombo, gearMenu,
                                                  doExport, doImport]() {
        gearMenu->clear();
        const QString cur = profCombo->currentData().toString();
        const bool isUser = !cur.isEmpty() && cur != kPierrotProfile;

        gearMenu->addAction(tr("Salvar como novo perfil…"), this, [this, profCombo]() {
            bool ok = false;
            const QString name = QInputDialog::getText(
                this, tr("Novo perfil"), tr("Nome do perfil:"),
                QLineEdit::Normal, QString(), &ok).trimmed();
            if (!ok || name.isEmpty()) return;
            if (name == kPierrotProfile) {
                QMessageBox::information(this, tr("Novo perfil"),
                    tr("\"Pierrot\" é o perfil padrão e não pode ser sobrescrito."));
                return;
            }
            if (shortcutProfileNames().contains(name)
                && QMessageBox::question(this, tr("Novo perfil"),
                        tr("O perfil \"%1\" já existe. Sobrescrever?").arg(name))
                       != QMessageBox::Yes)
                return;
            saveShortcutProfileMap(name, m_shortcutMap);
            QStringList names = shortcutProfileNames();
            if (!names.contains(name)) {
                names << name;
                setShortcutProfileNames(names);
            }
            const QSignalBlocker b(profCombo);
            if (profCombo->findData(name) < 0) profCombo->addItem(name, name);
            profCombo->setCurrentIndex(profCombo->findData(name));
            m_activeProfile = name;
        });
        if (isUser) {
            gearMenu->addAction(tr("Atualizar perfil…"), this, [this, cur]() {
                saveShortcutProfileMap(cur, m_shortcutMap);
            });
            gearMenu->addAction(tr("Renomear perfil…"), this, [this, profCombo, cur]() {
                bool ok = false;
                const QString name = QInputDialog::getText(
                    this, tr("Renomear perfil"), tr("Novo nome:"),
                    QLineEdit::Normal, cur, &ok).trimmed();
                if (!ok || name.isEmpty() || name == cur || name == kPierrotProfile)
                    return;
                if (shortcutProfileNames().contains(name)) {
                    QMessageBox::warning(this, tr("Renomear perfil"),
                        tr("Já existe um perfil chamado \"%1\".").arg(name));
                    return;
                }
                // Copia as chaves para o novo nome e apaga as antigas.
                saveShortcutProfileMap(name, m_shortcutMap);
                removeShortcutProfile(cur);
                QStringList names = shortcutProfileNames();
                names.removeAll(cur);
                names << name;
                setShortcutProfileNames(names);
                const QSignalBlocker b(profCombo);
                const int idx = profCombo->findData(cur);
                if (idx >= 0) {
                    profCombo->setItemText(idx, name);
                    profCombo->setItemData(idx, name);
                }
                m_activeProfile = name;
            });
            gearMenu->addAction(tr("Excluir perfil…"), this, [this, profCombo, cur]() {
                if (QMessageBox::question(this, tr("Excluir perfil"),
                        tr("Excluir o perfil \"%1\"?").arg(cur))
                    != QMessageBox::Yes)
                    return;
                removeShortcutProfile(cur);
                QStringList names = shortcutProfileNames();
                names.removeAll(cur);
                setShortcutProfileNames(names);
                const QSignalBlocker b(profCombo);
                profCombo->removeItem(profCombo->findData(cur));
                profCombo->setCurrentIndex(0); // Pierrot
                loadShortcutProfile(kPierrotProfile);
            });
        }
        gearMenu->addSeparator();
        gearMenu->addAction(tr("Exportar atalhos…"), this, doExport);
        gearMenu->addAction(tr("Importar atalhos…"), this, doImport);
    });

    m_shortcuts = new QTreeWidget(page);
    m_shortcuts->setColumnCount(2);
    m_shortcuts->setHeaderLabels({tr("Ação"), tr("Atalho")});
    m_shortcuts->setRootIsDecorated(true);
    m_shortcuts->setUniformRowHeights(true);
    m_shortcuts->setAlternatingRowColors(true);
    m_shortcuts->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_shortcuts->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_shortcuts->installEventFilter(this);

    QMap<QString, QTreeWidgetItem*> cats;
    for (int i = 0; i < kShortcutCount; ++i) {
        const ShortcutDef& sd = kShortcutDefs[i];
        const QString cat = tr(sd.cat);
        QTreeWidgetItem* parent = cats.value(cat);
        if (!parent) {
            parent = new QTreeWidgetItem(m_shortcuts);
            parent->setText(0, cat);
            parent->setExpanded(true);
            parent->setFlags(parent->flags() & ~Qt::ItemIsSelectable);
            cats.insert(cat, parent);
        }
        auto* it = new QTreeWidgetItem(parent);
        it->setText(0, tr(sd.label));
        it->setText(1, shortcutText(sd));
        it->setData(0, Qt::UserRole, QString::fromLatin1(sd.id));
    }

    auto* hint = new QLabel(tr("Clique num atalho e, em seguida, pressione a nova "
                               "combinação de teclas para alterá-lo. Esc cancela."), page);
    hint->setStyleSheet("color:#9a9a9a;");
    hint->setWordWrap(true);

    QPushButton* reset = new QPushButton(tr("Restaurar padrão do atalho selecionado"), page);
    connect(reset, &QPushButton::clicked, this, [this]() {
        QTreeWidgetItem* it = m_shortcuts->currentItem();
        if (!it) return;
        m_shortcutMap.remove(it->data(0, Qt::UserRole).toString());
        refreshShortcutRow();
    });

    v->addLayout(profRow);
    v->addWidget(m_shortcuts, 1);
    v->addWidget(hint);
    v->addWidget(reset, 0, Qt::AlignLeft);
    m_stack->addWidget(page);

    // Perfil ativo (ou Pierrot na primeira vez) → mapa em memória + linhas.
    const QString active = QSettings().value(QStringLiteral("shortcutsProfile"),
                                             kPierrotProfile).toString();
    const int idx = profCombo->findData(active);
    profCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    loadShortcutProfile(profCombo->currentData().toString());
    // Troca de perfil = carrega o mapa dele na tabela (edições não salvas do
    // perfil anterior se perdem — o esperado de um seletor de perfil).
    connect(profCombo, &QComboBox::currentIndexChanged, this, [this, profCombo]() {
        loadShortcutProfile(profCombo->currentData().toString());
    });

    m_shortcuts->setFocus();
    if (m_shortcuts->topLevelItemCount() > 0)
        m_shortcuts->setCurrentItem(m_shortcuts->topLevelItem(0)->child(0));
}

void SettingsDialog::loadShortcutProfile(const QString& name) {
    m_activeProfile = name;
    m_shortcutMap = loadShortcutProfileMap(name);
    refreshAllShortcutRows();
}

void SettingsDialog::refreshAllShortcutRows() {
    for (int r = 0; r < m_shortcuts->topLevelItemCount(); ++r) {
        QTreeWidgetItem* cat = m_shortcuts->topLevelItem(r);
        for (int c = 0; c < cat->childCount(); ++c) {
            QTreeWidgetItem* it = cat->child(c);
            const QString id = it->data(0, Qt::UserRole).toString();
            for (int i = 0; i < kShortcutCount; ++i)
                if (QString::fromLatin1(kShortcutDefs[i].id) == id) {
                    const QString v = m_shortcutMap.value(id);
                    it->setText(1, v.isEmpty()
                                    ? QKeySequence(QString::fromUtf8(kShortcutDefs[i].def)).toString()
                                    : v);
                    break;
                }
        }
    }
}

void SettingsDialog::refreshShortcutRow() {
    QTreeWidgetItem* it = m_shortcuts->currentItem();
    if (!it) return;
    const QString id = it->data(0, Qt::UserRole).toString();
    for (int i = 0; i < kShortcutCount; ++i)
        if (QString::fromLatin1(kShortcutDefs[i].id) == id) {
            const QString v = m_shortcutMap.value(id);
            it->setText(1, v.isEmpty()
                            ? QKeySequence(QString::fromUtf8(kShortcutDefs[i].def)).toString()
                            : v);
            break;
        }
}

bool SettingsDialog::eventFilter(QObject* o, QEvent* e) {
    if (o == m_shortcuts && e->type() == QEvent::MouseButtonRelease) {
        QTreeWidgetItem* it = m_shortcuts->itemAt(static_cast<QMouseEvent*>(e)->pos());
        if (it) m_recordId = it->data(0, Qt::UserRole).toString();
        return false;
    }
    if (o == m_shortcuts && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        const int key = ke->key();
        if (key == Qt::Key_Escape) { m_recordId.clear(); return true; }
        if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt
            || key == Qt::Key_Meta || key == Qt::Key_unknown
            || key == Qt::Key_Backtab || key == Qt::Key_Tab) {
            return false; // precisa da tecla principal da combinação
        }
        QKeySequence seq(key | int(ke->modifiers()));
        if (!seq.isEmpty() && !m_recordId.isEmpty()) {
            m_shortcutMap[m_recordId] = seq.toString();
            refreshShortcutRow();
            m_recordId.clear();
            return true;
        }
    }
    return QDialog::eventFilter(o, e);
}

bool SettingsDialog::autoSaveEnabled() const { return m_autoSave->isChecked(); }

bool SettingsDialog::proxiesEnabled() {
    return QSettings().value("proxiesEnabled", true).toBool();
}
int SettingsDialog::autoSaveMinutes() const { return m_autoInterval->value(); }
bool SettingsDialog::mkvWarning() const { return m_mkvWarn->isChecked(); }

void SettingsDialog::accept() {
    QSettings s;
    s.setValue("mkvWarning", m_mkvWarn->isChecked());
    s.setValue("autosaveEnabled", m_autoSave->isChecked());
    s.setValue("autosaveMinutes", m_autoInterval->value());
    s.setValue("timelineThumbMode", m_thumbMode->currentIndex());
    s.setValue("timelineRippleDelete", m_rippleDelete->isChecked());
    s.setValue("timelineTrimmer", m_trimmer->isChecked());
    s.setValue("hwDecode", m_hwDecode->isChecked());
    s.setValue("proxiesEnabled", m_proxies->isChecked());
    ProxyManager::instance().setEnabled(m_proxies->isChecked());
    s.setValue("proxyMinWidth", m_proxyMinWidth->value());
    ProxyManager::instance().setThresholdWidth(m_proxyMinWidth->value());
    s.setValue("exportHwEncode", m_hwEncode->isChecked());
    s.setValue("exportHwDevice", m_hwGpu ? m_hwGpu->currentData().toString() : QString());
    s.setValue("graphSensitivity", m_graphSens->value());
    saveTheme(static_cast<AppTheme>(m_themeCombo->currentIndex()));
    saveExternalPalette(m_extPaletteCombo->currentIndex() <= 0
                            ? QString()
                            : m_extPaletteCombo->currentText());
    s.setValue("language", m_langCombo->currentIndex());
    QStringList ofxPaths;
    for (int i = 0; i < m_ofxPaths->count(); ++i)
        ofxPaths.append(m_ofxPaths->item(i)->text());
    s.setValue("ofxSearchPaths", ofxPaths);
    s.setValue("exportDefaultDirEnabled", m_exportDirEnabled->isChecked());
    s.setValue("exportDefaultDir", m_exportDirEdit->text().trimmed());
    // Atalhos: grava o perfil ativo (qualquer um, inclusive Pierrot) e o
    // espelho vivo que o app lê. Remove as chaves que voltaram ao padrão
    // (sem isto o "Restaurar padrão" não persistia e o valor antigo voltava
    // no restart).
    s.setValue(QStringLiteral("shortcutsProfile"), m_activeProfile);
    saveShortcutProfileMap(m_activeProfile, m_shortcutMap);
    for (int i = 0; i < kShortcutCount; ++i) {
        const QString id = QString::fromLatin1(kShortcutDefs[i].id);
        const QString key = QStringLiteral("shortcuts/%1").arg(id);
        const QString v = m_shortcutMap.value(id);
        if (v.isEmpty()) s.remove(key);
        else s.setValue(key, v);
    }
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

void SettingsDialog::warn4kIfNeeded(QWidget* parent, bool has4k) {
    static bool warnedOnce = false;
    if (!has4k || warnedOnce || !warn4kEnabled()) return;
    warnedOnce = true;
    QMessageBox::information(parent, tr("Mídia 4K detectada"),
        tr("O projeto contém mídia em 4K ou superior. O preview pode engasgar "
           "nesses arquivos — reduza a qualidade do preview (botão no monitor) "
           "para aliviar a reprodução."));
}
