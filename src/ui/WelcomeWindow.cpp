// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "WelcomeWindow.h"

#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QFile>
#include <QMessageBox>
#include <QShortcut>
#include <QSettings>
#include <QSize>
#include <QCoreApplication>
#include <QPixmap>
#include <QStyle>
#include <QResizeEvent>

namespace {
QStringList loadRecentList() {
    return QSettings().value("recentProjects").toStringList();
}
}

WelcomeWindow::WelcomeWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Bem-vindo ao Pierrot"));
    setMinimumSize(800, 460);
    resize(1020, 600);

    buildLayout();
    loadRecentProjects();

    QSettings s;
    m_autoSave->setChecked(s.value("autosaveEnabled", false).toBool());
    const int mins = qMax(1, s.value("autosaveMinutes", 10).toInt());
    const int idx = m_autoInterval->findData(mins);
    if (idx >= 0) {
        m_autoInterval->setCurrentIndex(idx);
    } else {
        m_autoInterval->setCurrentIndex(m_autoInterval->findData(-1));
        m_autoCustom->setValue(mins);
    }
    onAutoIntervalChanged(m_autoInterval->currentIndex());

    const QSize last(s.value("lastWidth", 1920).toInt(),
                     s.value("lastHeight", 1080).toInt());
    int resIdx = 0;
    for (int i = 0; i < m_resolution->count(); ++i) {
        const QSize r = m_resolution->itemData(i).toSize();
        if (r.width() > 0 && r == last) { resIdx = i; break; }
    }
    m_resolution->setCurrentIndex(resIdx);
    const int fpsIdx = m_fpsBox->findData(s.value("lastFps", 30).toInt());
    if (fpsIdx >= 0) m_fpsBox->setCurrentIndex(fpsIdx);
    onResolutionChanged(m_resolution->currentIndex());
}

void WelcomeWindow::buildLayout() {
    // Imagem à esquerda, adaptável ao tamanho da janela.
    m_imageLabel = new QLabel(this);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setMinimumSize(180, 240);
    m_imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPixmap pm;
    if (pm.load(":/pierrot.jpg")
        || pm.load("imagens/pierrot.jpg")
        || pm.load(QCoreApplication::applicationDirPath() + "/imagens/pierrot.jpg")) {
        m_img = pm;
        QSize disp = pm.size();
        disp.scale(QSize(360, 520), Qt::KeepAspectRatio);
        m_imageLabel->setPixmap(pm.scaled(disp, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        m_imageLabel->setText(tr("Pierrot"));
        QFont pf = m_imageLabel->font();
        pf.setPointSize(30);
        pf.setBold(true);
        m_imageLabel->setFont(pf);
        m_imageLabel->setStyleSheet("color:#9db2c8;");
    }

    // Título central
    auto* title = new QLabel(tr("Pierrot"), this);
    QFont tf = title->font();
    tf.setPointSize(34);
    tf.setBold(true);
    title->setFont(tf);
    title->setStyleSheet("color:#e8ebf0;");

    auto* subtitle = new QLabel(tr("Editor de vídeo"), this);
    subtitle->setStyleSheet("color: #9a9a9a;");

    auto* credits = new QLabel(tr("by InkSpoty"), this);
    credits->setStyleSheet("color: #6b7280; font-size: 11px;");
    credits->setAlignment(Qt::AlignHCenter);

    auto* version = new QLabel(tr("Versão 0.2.0"), this);
    version->setStyleSheet("color: #4e5560; font-size: 10px;");
    version->setAlignment(Qt::AlignHCenter);

    // Projetos recentes
    auto* recentBox = new QGroupBox(tr("Projetos recentes"), this);
    m_recent = new QListWidget(recentBox);
    m_recent->setMinimumHeight(190);
    connect(m_recent, &QListWidget::itemDoubleClicked, this,
            &WelcomeWindow::openSelected);

    auto* delShort = new QShortcut(QKeySequence::Delete, m_recent);
    delShort->setContext(Qt::WidgetShortcut);
    connect(delShort, &QShortcut::activated, this, &WelcomeWindow::removeSelected);

    auto* openBtn = new QPushButton(tr("Abrir"), recentBox);
    connect(openBtn, &QPushButton::clicked, this, &WelcomeWindow::openSelected);

    auto* delBtn = new QPushButton(tr("Excluir"), recentBox);
    delBtn->setToolTip(tr("Remover o projeto selecionado dos recentes"));
    connect(delBtn, &QPushButton::clicked, this, &WelcomeWindow::removeSelected);

    auto* recentBtnRow = new QHBoxLayout;
    recentBtnRow->addStretch(1);
    recentBtnRow->addWidget(openBtn);
    recentBtnRow->addWidget(delBtn);

    auto* recentLay = new QVBoxLayout(recentBox);
    recentLay->addWidget(m_recent);
    recentLay->addLayout(recentBtnRow);

    // Novo projeto
    auto* newBox = new QGroupBox(tr("Novo projeto"), this);

    m_name = new QLineEdit(newBox);
    m_name->setPlaceholderText(tr("Nome do projeto"));
    m_name->setText(tr("Meu projeto"));

    m_resolution = new QComboBox(newBox);
    struct Preset { const char* label; int w, h; };
    const Preset presets[] = {
        { "4K (3840 × 2160)", 3840, 2160 },
        { "Full HD (1920 × 1080)", 1920, 1080 },
        { "2K (2560 × 1440)", 2560, 1440 },
        { "HD (1280 × 720)", 1280, 720 },
        { "SD (720 × 480)", 720, 480 },
        { "640 × 360", 640, 360 },
    };
    for (const Preset& pr : presets)
        m_resolution->addItem(tr(pr.label), QVariant::fromValue(QSize(pr.w, pr.h)));
    m_resolution->addItem(tr("Personalizado…"), QVariant::fromValue(QSize(-1, -1)));
    m_resolution->setCurrentIndex(1);
    connect(m_resolution, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &WelcomeWindow::onResolutionChanged);

    m_customW = new QSpinBox(newBox);
    m_customW->setRange(64, 7680);
    m_customW->setValue(1920);
    m_customW->setSuffix(tr(" px"));
    m_customH = new QSpinBox(newBox);
    m_customH->setRange(64, 4320);
    m_customH->setValue(1080);
    m_customH->setSuffix(tr(" px"));

    auto* customWidget = new QWidget(newBox);
    m_customWidget = customWidget;
    auto* customRow = new QHBoxLayout(customWidget);
    customRow->setContentsMargins(0, 0, 0, 0);
    customRow->addWidget(new QLabel(tr("Largura:"), customWidget));
    customRow->addWidget(m_customW);
    customRow->addWidget(new QLabel(tr("Altura:"), customWidget));
    customRow->addWidget(m_customH);

    m_fpsBox = new QComboBox(newBox);
    for (int f : {24, 25, 30, 50, 60, 120})
        m_fpsBox->addItem(QString("%1 fps").arg(f), f);
    m_fpsBox->setCurrentIndex(m_fpsBox->findData(30));

    auto* createBtn = new QPushButton(tr("Criar projeto"), newBox);
    createBtn->setDefault(true);
    createBtn->setStyleSheet(
        "QPushButton{background:#2c3d57; border:1px solid #4a6a94; border-radius:6px;"
        " color:#dce8f5; padding:9px 24px; font-weight:bold;}"
        "QPushButton:hover{background:#36506f;}"
        "QPushButton:pressed{background:#253550;}");
    connect(createBtn, &QPushButton::clicked, this, &WelcomeWindow::requestNewProject);

    auto* newLay = new QVBoxLayout(newBox);
    newLay->addWidget(new QLabel(tr("Nome:"), newBox));
    newLay->addWidget(m_name);
    newLay->addWidget(new QLabel(tr("Resolução:"), newBox));
    newLay->addWidget(m_resolution);
    newLay->addWidget(customWidget);
    newLay->addWidget(new QLabel(tr("Quadros por segundo:"), newBox));
    newLay->addWidget(m_fpsBox);
    newLay->addWidget(createBtn, 0, Qt::AlignRight);

    // Coluna direita: salvamento automático
    auto* autoBox = new QGroupBox(tr("Salvamento automático"), this);
    m_autoSave = new QCheckBox(tr("Ativar salvamento automático"), autoBox);

    m_autoInterval = new QComboBox(autoBox);
    struct Interval { const char* label; int mins; };
    const Interval intervals[] = {
        { "A cada 5 minutos", 5 },
        { "A cada 10 minutos", 10 },
        { "A cada 20 minutos", 20 },
        { "A cada 30 minutos", 30 },
        { "A cada 1 hora", 60 },
    };
    for (const Interval& iv : intervals)
        m_autoInterval->addItem(tr(iv.label), iv.mins);
    m_autoInterval->addItem(tr("Personalizado…"), -1);
    m_autoInterval->setCurrentIndex(1);
    connect(m_autoInterval, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &WelcomeWindow::onAutoIntervalChanged);

    m_autoCustom = new QSpinBox(autoBox);
    m_autoCustom->setRange(1, 1440);
    m_autoCustom->setValue(15);
    m_autoCustom->setSuffix(tr(" min"));

    auto* autoLay = new QVBoxLayout(autoBox);
    autoLay->addWidget(m_autoSave);
    autoLay->addSpacing(6);
    autoLay->addWidget(new QLabel(tr("Intervalo:"), autoBox));
    autoLay->addWidget(m_autoInterval);
    autoLay->addWidget(m_autoCustom);
    autoLay->addStretch(1);

    auto* hint = new QLabel(tr("Feche esta janela para abrir o editor vazio."), this);
    hint->setStyleSheet("color: #777777;");

    // Aviso de versão de desenvolvimento: exibido apenas em builds de debug.
    m_devWarn = new QLabel(tr("Você está usando uma versão de desenvolvimento do Pierrot. "
                              "Recursos podem estar incompletos ou instáveis."), this);
    m_devWarn->setWordWrap(true);
    m_devWarn->setStyleSheet(
        "background-color: #5a3b00; color: #ffd97a; padding: 6px 10px;"
        "border-radius: 4px; font-weight: bold;");
    m_devWarn->setVisible(false);

    // Montagem geral
    auto* leftCol = new QVBoxLayout;
    leftCol->addWidget(m_imageLabel);
    leftCol->addStretch(1);

    auto* centerCol = new QVBoxLayout;
    centerCol->addWidget(title);
    centerCol->addWidget(subtitle);
    centerCol->addSpacing(14);
    centerCol->addWidget(recentBox);
    centerCol->addWidget(newBox);
    centerCol->addStretch(1);

    auto* rightCol = new QVBoxLayout;
    rightCol->addWidget(autoBox);
    rightCol->addStretch(1);

    auto* topRow = new QHBoxLayout;
    topRow->addLayout(leftCol);
    topRow->addLayout(centerCol, 1);
    topRow->addLayout(rightCol);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 22, 28, 12);
    root->addLayout(topRow, 1);
    // Rodapé: dica à esquerda; créditos e versão no canto inferior direito.
    auto* footer = new QHBoxLayout;
    footer->addWidget(hint);
    footer->addStretch(1);
    footer->addWidget(credits);
    footer->addSpacing(12);
    footer->addWidget(version);
    root->addLayout(footer);
    root->addWidget(m_devWarn);
    root->addSpacing(8);

#if defined(QT_NO_DEBUG)
    m_devWarn->setVisible(false);
#else
    m_devWarn->setVisible(true);
#endif

    // Tema escuro, elegante e consistente com o restante do app.
    setStyleSheet(QStringLiteral(
        "QDialog{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        " stop:0 #24262c, stop:1 #16171a);}"
        "QGroupBox{border:1px solid #2b2e36; border-radius:9px; margin-top:15px;"
        " background:rgba(255,255,255,0.018);}"
        "QGroupBox::title{subcontrol-origin:margin; left:13px; top:0; padding:0 8px;"
        " color:#8fa3bc; font-weight:bold; font-size:11px;}"
        "QListWidget{background:rgba(0,0,0,0.20); border:1px solid #2b2e36;"
        " border-radius:6px; padding:4px; outline:none;}"
        "QListWidget::item{padding:10px 12px; border-radius:5px; margin:2px; color:#d5d9e0;}"
        "QListWidget::item:hover{background:rgba(120,160,220,0.10);}"
        "QListWidget::item:selected{background:rgba(80,130,200,0.26); color:#ffffff;}"
        "QPushButton{border:1px solid #30343c; border-radius:5px; background:#26282e;"
        " color:#d5d9e0; padding:6px 15px;}"
        "QPushButton:hover{background:#2d3037; border-color:#454a55;}"
        "QLineEdit,QComboBox,QSpinBox{background:rgba(0,0,0,0.20);"
        " border:1px solid #30343c; border-radius:5px; padding:5px 9px;"
        " color:#d5d9e0; selection-background-color:#2c3c52;}"
        "QComboBox::drop-down{border:none; width:20px;}"
        "QComboBox QAbstractItemView{background:#1e2026; border:1px solid #30343c;"
        " selection-background-color:#2c3c52; border-radius:4px;}"
        "QCheckBox{color:#c9cdd4;}"
        "QLabel{color:#c9cdd4;}"));
}

void WelcomeWindow::loadRecentProjects() {
    m_recent->clear();
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);
    for (const QString& path : loadRecentList()) {
        if (!QFile::exists(path)) continue;
        auto* it = new QListWidgetItem(fileIcon, QFileInfo(path).fileName(), m_recent);
        it->setData(Qt::UserRole, path);
        it->setToolTip(path);
    }
    if (m_recent->count() == 0) {
        auto* it = new QListWidgetItem(tr("Nenhum projeto recente."), m_recent);
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(QColor(120, 124, 132));
    }
}

void WelcomeWindow::openSelected() {
    QListWidgetItem* it = m_recent->currentItem();
    if (!it) return;
    const QString path = it->data(Qt::UserRole).toString();
    if (path.isEmpty()) return;
    saveAutoSettings();
    m_projectPath = path;
    accept();
}

void WelcomeWindow::removeSelected() {
    QListWidgetItem* it = m_recent->currentItem();
    if (!it) return;
    const QString path = it->data(Qt::UserRole).toString();
    if (path.isEmpty()) return;

    QMessageBox box(QMessageBox::Question,
                    tr("Excluir projeto"),
                    tr("O que deseja fazer com \"%1\"?")
                        .arg(QFileInfo(path).fileName()),
                    QMessageBox::Cancel, this);
    QPushButton* onlyList = box.addButton(tr("Só remover da lista"),
                                          QMessageBox::DestructiveRole);
    QPushButton* alsoFile = box.addButton(tr("Remover e excluir o arquivo"),
                                          QMessageBox::DestructiveRole);
    box.setDefaultButton(onlyList);
    box.exec();
    const QAbstractButton* clicked = box.clickedButton();

    if (clicked == alsoFile) {
        QFile::remove(path);
    } else if (clicked != onlyList) {
        return; // cancelado
    }

    QStringList recents = loadRecentList();
    recents.removeAll(path);
    QSettings().setValue("recentProjects", recents);
    delete m_recent->takeItem(m_recent->row(it));
    if (m_recent->count() == 0)
        loadRecentProjects();
}

void WelcomeWindow::requestNewProject() {
    saveAutoSettings();
    const QSize res = m_resolution->currentData().toSize();
    if (res.width() <= 0) {
        m_w = m_customW->value();
        m_h = m_customH->value();
    } else {
        m_w = res.width();
        m_h = res.height();
    }
    m_fps = m_fpsBox->currentData().toInt();
    if (m_fps <= 0) m_fps = 30;
    m_projectName = m_name->text().trimmed();
    if (m_projectName.isEmpty()) m_projectName = tr("Meu projeto");
    m_newRequested = true;

    QSettings s;
    s.setValue("lastWidth", m_w);
    s.setValue("lastHeight", m_h);
    s.setValue("lastFps", m_fps);
    accept();
}

void WelcomeWindow::onResolutionChanged(int idx) {
    const QSize res = m_resolution->itemData(idx).toSize();
    m_customWidget->setVisible(res.width() <= 0);
}

void WelcomeWindow::onAutoIntervalChanged(int idx) {
    m_autoCustom->setVisible(m_autoInterval->itemData(idx).toInt() < 0);
}

int WelcomeWindow::autosaveMinutes() const {
    const int mins = m_autoInterval->currentData().toInt();
    return mins < 0 ? m_autoCustom->value() : mins;
}

void WelcomeWindow::saveAutoSettings() const {
    QSettings s;
    s.setValue("autosaveEnabled", m_autoSave->isChecked());
    s.setValue("autosaveMinutes", autosaveMinutes());
}

// Escala a imagem da esquerda para acompanhar o tamanho da janela (mantendo a
// proporção), sem loop de redimensionamento.
void WelcomeWindow::resizeEvent(QResizeEvent*) {
    if (m_img.isNull()) return;
    const QSize ls = m_imageLabel->size();
    if (ls.width() <= 20 || ls.height() <= 20) return;
    const QPixmap scaled = m_img.scaled(ls, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QPixmap cur = m_imageLabel->pixmap();
    if (cur.isNull() || cur.size() != scaled.size())
        m_imageLabel->setPixmap(scaled);
}
