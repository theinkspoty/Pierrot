// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "WelcomeWindow.h"
#include "version.h"
#include "ui/TitleBar.h"

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
#include <QSignalBlocker>
#include <QSettings>
#include <QSize>
#include <QCoreApplication>
#include <QPixmap>
#include <QStyle>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QColor>
#include <QFont>
#include <QFontMetrics>

namespace {
QStringList loadRecentList() {
    return QSettings().value("recentProjects").toStringList();
}

// Recorta uma imagem com os cantos arredondados (moldura).
QPixmap makeRounded(const QPixmap& src, int radius) {
    if (src.isNull()) return src;
    QPixmap out(src.size());
    out.fill(Qt::transparent);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, src.width(), src.height()), radius, radius);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipPath(path);
    p.drawPixmap(0, 0, src);
    return out;
}

// Região de cantos arredondados para recortar a janela (borda arredondada).
}

WelcomeWindow::WelcomeWindow(QWidget* parent) : QDialog(parent) {
    // Janela sem as bordas do sistema: a barra de título é personalizada.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    // Transparência com cantos arredondados: o paintEvent pinta o fundo opaco
    // (gradiente) dentro de um retângulo arredondado; fora do raio é
    // transparente, dando os cantos arredondados à janela.
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("Bem-vindo ao Pierrot"));
    setMinimumSize(820, 560);
    resize(1000, 660);

    buildLayout();
    loadRecentProjects();

    QSettings s;
    m_autoSave->setChecked(s.value("autosaveEnabled", false).toBool());
    // Aviso quando o usuário habilita o salvamento automático (experimental).
    connect(m_autoSave, &QCheckBox::toggled, this, [this](bool on) {
        if (!on) return;
        QMessageBox box(QMessageBox::Warning,
                        tr("Recurso experimental"),
                        tr("O salvamento automático é um recurso experimental e pode "
                           "apresentar falhas. Deseja habilitá-lo?"),
                        QMessageBox::Ok | QMessageBox::Cancel, this);
        if (box.exec() != QMessageBox::Ok) {
            QSignalBlocker b(m_autoSave);
            m_autoSave->setChecked(false);
        }
    });
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
    // Imagem da marca com bordas arredondadas (moldura), no lado esquerdo.
    QPixmap pm;
    if (pm.load(":/pierrot.jpg")
        || pm.load("imagens/pierrot.jpg")
        || pm.load(QCoreApplication::applicationDirPath() + "/imagens/pierrot.jpg")) {
        m_img = pm;
    } else {
        m_img = QPixmap(360, 480);
        m_img.fill(QColor(30, 32, 38));
        QPainter pt(&m_img);
        pt.setPen(Qt::NoPen);
        pt.setBrush(QColor(60, 90, 130));
        pt.drawEllipse(m_img.rect().center(), 80, 80);
    }
    auto* imageFrame = new QLabel(this);
    m_imageFrame = imageFrame;
    imageFrame->setAlignment(Qt::AlignCenter);
    imageFrame->setMinimumSize(300, 400);
    imageFrame->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
    imageFrame->setPixmap(makeRounded(m_img.scaled(QSize(320, 440), Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation), 18));

    // Nome + versão pequena ao lado (mesma fonte/bold, menor), centralizado.
    auto* nameLabel = new QLabel(tr("Pierrot"), this);
    QFont nf = nameLabel->font();
    nf.setPointSize(30);
    nf.setBold(true);
    nameLabel->setFont(nf);
    nameLabel->setStyleSheet("color:#f2f5fa;");

    auto* verLabel = new QLabel(tr("0.3.5"), this);
    QFont vf = verLabel->font();
    vf.setPointSize(12);
    vf.setBold(true);
    verLabel->setFont(vf);
    verLabel->setStyleSheet("color:#7f92ab;");

    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(6);
    nameRow->addStretch(1);
    nameRow->addWidget(nameLabel);
    nameRow->addWidget(verLabel, 0, Qt::AlignBottom);
    nameRow->addStretch(1);

    auto* slogan = new QLabel(tr("para Linux"), this);
    slogan->setStyleSheet("color:#8a93a2; font-size:13px; letter-spacing:1px;");
    slogan->setAlignment(Qt::AlignHCenter);

    auto* imageCol = new QVBoxLayout;
    imageCol->setSpacing(6);
    imageCol->addWidget(imageFrame, 1);
    imageCol->addLayout(nameRow);
    imageCol->addWidget(slogan);

    auto* credits = new QLabel(tr("by InkSpoty"), this);
    credits->setStyleSheet("color: #6b7280; font-size: 11px;");
    credits->setAlignment(Qt::AlignHCenter);

    auto* version = new QLabel(tr("Versão %1").arg(QStringLiteral(PIERROT_VERSION)), this);
    version->setStyleSheet("color: #4e5560; font-size: 10px;");
    version->setAlignment(Qt::AlignHCenter);

    // Projetos recentes
    auto* recentBox = new QGroupBox(tr("Projetos recentes"), this);
    recentBox->setMinimumHeight(320);
    m_recent = new QListWidget(recentBox);
    m_recent->setMinimumHeight(260);
    connect(m_recent, &QListWidget::itemDoubleClicked, this,
            &WelcomeWindow::openSelected);

    auto* delShort = new QShortcut(QKeySequence::Delete, m_recent);
    delShort->setContext(Qt::WidgetShortcut);
    connect(delShort, &QShortcut::activated, this, &WelcomeWindow::removeSelected);

    auto* openBtn = new QPushButton(tr("Abrir"), recentBox);
    openBtn->setToolTip(tr("Abrir o projeto selecionado"));
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
    newBox->setMinimumHeight(320);

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
    createBtn->setCursor(Qt::PointingHandCursor);
    createBtn->setMinimumHeight(40);
    createBtn->setStyleSheet(
        "QPushButton{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        " stop:0 #2f6fb3, stop:1 #3d8fd4);"
        " border:none; border-radius:8px;"
        " color:#ffffff; padding:10px 28px; font-weight:bold; font-size:14px;}"
        "QPushButton:hover{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        " stop:0 #3780c5, stop:1 #4a9adf);}"
        "QPushButton:pressed{background:#2a5f96;}");
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

    // Aviso de fase inicial de desenvolvimento, no canto inferior esquerdo.
    auto* betaWarn = new QLabel(
        tr("O editor está no início do desenvolvimento e pode apresentar falhas."), this);
    betaWarn->setStyleSheet("color: #b08a3c; font-size: 10px; font-style: italic;");
    betaWarn->setWordWrap(false);

    // Aviso de versão de desenvolvimento: exibido apenas em builds de debug.
    m_devWarn = new QLabel(tr("Você está usando uma versão de desenvolvimento do Pierrot. "
                              "Recursos podem estar incompletos ou instáveis."), this);
    m_devWarn->setWordWrap(true);
    m_devWarn->setStyleSheet(
        "background-color: #5a3b00; color: #ffd97a; padding: 6px 10px;"
        "border-radius: 4px; font-weight: bold;");
    m_devWarn->setVisible(false);

    // Montagem geral: imagem (moldura) à esquerda, recentes ao centro,
    // "Novo projeto" + auto-save à direita.
    auto* midCol = new QVBoxLayout;
    midCol->addStretch(1);
    midCol->addWidget(recentBox);
    midCol->addStretch(1);

    auto* rightCol = new QVBoxLayout;
    rightCol->addWidget(newBox);
    rightCol->addSpacing(12);
    rightCol->addWidget(autoBox);
    rightCol->addStretch(1);

    auto* body = new QHBoxLayout;
    body->setSpacing(24);
    body->addLayout(imageCol, 0);
    body->addLayout(midCol, 1);
    body->addLayout(rightCol, 1);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    // Barra de título personalizada (sem as bordas do sistema).
    auto* titleBar = new TitleBar(tr("Bem-vindo ao Pierrot"), this);
    root->addWidget(titleBar);

    auto* content = new QWidget(this);
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(28, 20, 28, 12);
    contentLay->setSpacing(8);
    contentLay->addLayout(body, 1);
    // Rodapé: aviso à esquerda; créditos e versão no canto inferior direito.
    auto* footer = new QHBoxLayout;
    footer->addWidget(betaWarn);
    footer->addSpacing(16);
    footer->addWidget(hint);
    footer->addStretch(1);
    footer->addWidget(credits);
    footer->addSpacing(12);
    footer->addWidget(version);
    contentLay->addLayout(footer);
    contentLay->addWidget(m_devWarn);
    contentLay->addSpacing(8);
    root->addWidget(content, 1);

#if defined(QT_NO_DEBUG)
    m_devWarn->setVisible(false);
#else
    m_devWarn->setVisible(true);
#endif

    // Tema escuro, elegante e consistente com o restante do app. O fundo é
    // pintado no paintEvent (com cantos arredondados), então não colocamos
    // background aqui.
    setStyleSheet(QStringLiteral(
        "QGroupBox{border:1px solid #343945; border-radius:12px; margin-top:18px;"
        " background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "  stop:0 #2a2d34, stop:1 #202228);}"
        "QGroupBox::title{subcontrol-origin:margin; left:15px; top:3px; padding:0 9px;"
        " color:#a7b8cf; font-weight:bold; font-size:12px; letter-spacing:0.6px;}"
        "QListWidget{background:#1b1d22; border:1px solid #383e4b;"
        " border-radius:8px; padding:5px; outline:none;}"
        "QListWidget::item{padding:13px 14px; border-radius:6px; margin:3px; color:#d8dce3;}"
        "QListWidget::item:hover{background:rgba(110,160,230,0.12);}"
        "QListWidget::item:selected{background:rgba(70,130,210,0.32); color:#ffffff;}"
        "QPushButton{border:1px solid #3a404c; border-radius:7px; background:#2b2e36;"
        " color:#dde2ea; padding:8px 18px; font-weight:bold;}"
        "QPushButton:hover{background:#333843; border-color:#4d5462;}"
        "QPushButton:pressed{background:#25282f;}"
        "QLineEdit,QComboBox,QSpinBox{background:#1b1d22;"
        " border:1px solid #383e4b; border-radius:7px; padding:7px 11px;"
        " color:#e4e8ee; selection-background-color:#2c4a6e;}"
        "QLineEdit:focus,QComboBox:focus,QSpinBox:focus{border-color:#4a6a94;}"
        "QComboBox::drop-down{border:none; width:24px;}"
        "QComboBox QAbstractItemView{background:#1e2026; border:1px solid #383e4b;"
        " selection-background-color:#2c4a6e; border-radius:5px;}"
        "QCheckBox{color:#cdd2da; spacing:6px;}"
        "QLabel{color:#cdd2da;}"));
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

// Pinta o fundo da janela com cantos arredondados (gradiente escuro). Fora do
// raio fica transparente (WA_TranslucentBackground), dando a borda arredonda.
void WelcomeWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), 18, 18);
    QLinearGradient g(0, 0, 0, height());
    g.setColorAt(0, QColor(38, 40, 46));
    g.setColorAt(1, QColor(21, 22, 26));
    p.fillPath(path, g);
}

void WelcomeWindow::resizeEvent(QResizeEvent* ev) {
    update(); // repinta com o novo tamanho (cantos arredondados)
    if (m_img.isNull() || !m_imageFrame) { QDialog::resizeEvent(ev); return; }
    const QSize ls = m_imageFrame->size();
    const int w = qMax(200, ls.width() - 8);
    const int h = qMax(240, ls.height() - 8);
    const int radius = 16;
    const QPixmap scaled = m_img.scaled(QSize(qMin(w, 480), qMin(h, 640)),
                                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QPixmap cur = m_imageFrame->pixmap();
    if (cur.isNull() || cur.size() != scaled.size())
        m_imageFrame->setPixmap(makeRounded(scaled, radius));
    QDialog::resizeEvent(ev);
}
