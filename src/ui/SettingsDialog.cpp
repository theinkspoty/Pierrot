// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "SettingsDialog.h"
#include "Theme.h"

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
#include <QKeyEvent>
#include <QMouseEvent>
#include <QKeySequence>
#include <QFileDialog>
#include <QLineEdit>

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
    return v.isEmpty() ? QKeySequence(QString::fromLatin1(sd.def)).toString() : v;
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
    m_themeCombo->addItem(tr("Escuro"));
    m_themeCombo->addItem(tr("Claro (Final Cut)"));
    m_themeCombo->setCurrentIndex(savedTheme() == AppTheme::Light ? 1 : 0);

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
        auto* autoBox = new QGroupBox(tr("Salvamento automático"), page);
        auto* autoLay = new QFormLayout(autoBox);
        autoLay->addRow(m_autoSave);
        autoLay->addRow(tr("Intervalo:"), m_autoInterval);
        v->addWidget(mkvBox);
        v->addWidget(themeBox);
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
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);

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

    v->addWidget(m_shortcuts, 1);
    v->addWidget(hint);
    v->addWidget(reset, 0, Qt::AlignLeft);
    m_stack->addWidget(page);

    m_shortcuts->setFocus();
    if (m_shortcuts->topLevelItemCount() > 0)
        m_shortcuts->setCurrentItem(m_shortcuts->topLevelItem(0)->child(0));
}

void SettingsDialog::refreshShortcutRow() {
    QTreeWidgetItem* it = m_shortcuts->currentItem();
    if (!it) return;
    const QString id = it->data(0, Qt::UserRole).toString();
    for (int i = 0; i < kShortcutCount; ++i)
        if (QString::fromLatin1(kShortcutDefs[i].id) == id) {
            const QString v = m_shortcutMap.value(id);
            it->setText(1, v.isEmpty()
                            ? QKeySequence(QString::fromLatin1(kShortcutDefs[i].def)).toString()
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
    s.setValue("graphSensitivity", m_graphSens->value());
    saveTheme(m_themeCombo->currentIndex() == 1 ? AppTheme::Light : AppTheme::Dark);
    QStringList ofxPaths;
    for (int i = 0; i < m_ofxPaths->count(); ++i)
        ofxPaths.append(m_ofxPaths->item(i)->text());
    s.setValue("ofxSearchPaths", ofxPaths);
    s.setValue("exportDefaultDirEnabled", m_exportDirEnabled->isChecked());
    s.setValue("exportDefaultDir", m_exportDirEdit->text().trimmed());
    for (auto it = m_shortcutMap.constBegin(); it != m_shortcutMap.constEnd(); ++it)
        s.setValue(QString("shortcuts/%1").arg(it.key()), it.value());
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
