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
#include <QSettings>
#include <QSize>
#include <QCoreApplication>
#include <QPixmap>

namespace {
QStringList loadRecentList() {
    return QSettings().value("recentProjects").toStringList();
}
}

WelcomeWindow::WelcomeWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Bem-vindo ao Pierrot"));
    setMinimumSize(900, 520);
    resize(980, 560);

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
    // Imagem à esquerda
    m_imageLabel = new QLabel(this);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    QPixmap pm;
    if (pm.load(":/pierrot.jpg")
        || pm.load("imagens/pierrot.jpg")
        || pm.load(QCoreApplication::applicationDirPath() + "/imagens/pierrot.jpg")) {
        QSize disp = pm.size();
        disp.scale(QSize(360, 560), Qt::KeepAspectRatio);
        m_imageLabel->setPixmap(pm.scaled(disp, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_imageLabel->setFixedSize(disp);
    } else {
        m_imageLabel->setText(tr("Pierrot"));
        m_imageLabel->setFixedSize(360, 560);
    }

    // Título central
    auto* title = new QLabel(tr("Pierrot"), this);
    QFont tf = title->font();
    tf.setPointSize(28);
    tf.setBold(true);
    title->setFont(tf);

    auto* subtitle = new QLabel(tr("Editor de vídeo"), this);
    subtitle->setStyleSheet("color: #9a9a9a;");

    // Projetos recentes
    auto* recentBox = new QGroupBox(tr("Projetos recentes"), this);
    m_recent = new QListWidget(recentBox);
    m_recent->setMinimumHeight(150);
    connect(m_recent, &QListWidget::itemDoubleClicked, this,
            &WelcomeWindow::openSelected);

    auto* openBtn = new QPushButton(tr("Abrir"), recentBox);
    connect(openBtn, &QPushButton::clicked, this, &WelcomeWindow::openSelected);

    auto* recentLay = new QVBoxLayout(recentBox);
    recentLay->addWidget(m_recent);
    recentLay->addWidget(openBtn, 0, Qt::AlignRight);

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
    root->addLayout(topRow, 1);
    root->addWidget(hint, 0, Qt::AlignHCenter);
    root->addSpacing(8);
}

void WelcomeWindow::loadRecentProjects() {
    m_recent->clear();
    for (const QString& path : loadRecentList()) {
        if (!QFile::exists(path)) continue;
        auto* it = new QListWidgetItem(QFileInfo(path).fileName(), m_recent);
        it->setData(Qt::UserRole, path);
        it->setToolTip(path);
    }
    if (m_recent->count() == 0) {
        auto* it = new QListWidgetItem(tr("Nenhum projeto recente."), m_recent);
        it->setFlags(Qt::NoItemFlags);
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
