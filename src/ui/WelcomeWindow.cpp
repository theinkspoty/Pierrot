// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "WelcomeWindow.h"
#include "version.h"
#include "ui/Theme.h"
#include "colombina/ffmpeg/MediaCache.h"

#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QButtonGroup>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileInfo>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QShortcut>
#include <QSettings>
#include <QSize>
#include <QCoreApplication>
#include <QPixmap>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QColor>
#include <QFont>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace {
QStringList loadRecentList() {
    return QSettings().value("recentProjects").toStringList();
}

constexpr double kProjectThumbTime = 0.5;

// Primeira mídia de vídeo (com arquivo) do projeto .Blanc: o frame dela vira a
// miniatura real do projeto. Percorre o JSON direto, sem instanciar o projeto.
QString firstVideoMedia(const QString& projectPath) {
    QFile f(projectPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) return {};
    const QJsonObject root = doc.object();
    if (!root.contains("media")) return {};
    const QJsonArray media = root["media"].toArray();
    for (const QJsonValue& v : media) {
        const QJsonObject o = v.toObject();
        const QString file = o["filePath"].toString();
        if (o["hasVideo"].toBool() && !file.isEmpty() && QFile::exists(file)) {
            return file;
        }
    }
    return {};
}

// Recorta a imagem para preencher exatamente w×h (semântica "cover", central).
QPixmap coverThumb(const QImage& img, int w, int h) {
    if (img.isNull()) return {};
    QImage tmp = img;
    const double scale = qMax(w / double(tmp.width()), h / double(tmp.height()));
    const int W = qMax(w, qCeil(tmp.width() * scale));
    const int H = qMax(h, qCeil(tmp.height() * scale));
    tmp = tmp.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    tmp = tmp.copy((W - w) / 2, (H - h) / 2, w, h);
    return QPixmap::fromImage(tmp);
}

// Miniatura de um projeto recente: tile sólido arredondado. Com frame real
// (vídeo/proxy) ele preenche por cima; sem frame fica um placeholder neutro.
// O nome não vai aqui — o QListWidget desenha o texto abaixo do ícone.
QPixmap makeRecentTile(const QImage& frame) {
    const auto& tc = themeColors();
    const int W = 168, H = 94;
    QPixmap out(W, H);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF tile(0.5, 0.5, W - 1, H - 1);

    QPainterPath clip;
    clip.addRoundedRect(tile, 5, 5);
    p.fillPath(clip, tc.inputBg);

    if (!frame.isNull()) {
        p.save();
        p.setClipPath(clip);
        p.drawPixmap(0, 0, coverThumb(frame, W, H));
        p.restore();
    } else {
        // Placeholder: um "claquete"/tela simples, só com traço, sem cores.
        p.setPen(QPen(tc.disabledText, 2));
        p.setBrush(Qt::NoBrush);
        const QRect r(W / 2 - 22, H / 2 - 15, 44, 30);
        p.drawRoundedRect(r, 3, 3);
        p.drawLine(r.left() + 11, r.top(), r.left() + 11, r.bottom());
        p.drawLine(r.right() - 11, r.top(), r.right() - 11, r.bottom());
    }

    p.setPen(QPen(tc.inputBorder, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(tile, 5, 5);
    p.end();
    return out;
}
} // namespace

WelcomeWindow::WelcomeWindow(QWidget* parent) : QDialog(parent) {
    // Moldura de janela padrão, como o editor. Nada de frameless/translúcido:
    // arrastar, redimensionar e os botões ficam por conta do sistema.
    setWindowTitle(tr("Bem-vindo ao Pierrot"));
    setMinimumSize(900, 620);
    resize(1060, 680);

    buildUi();
    loadRecentProjects();

    // Miniatura real chega em segundo plano (decodificação no MediaCache).
    connect(&MediaCache::instance(), &MediaCache::thumbnailReady, this,
            [this](const QString& filePath, double seconds) {
                if (qAbs(seconds - kProjectThumbTime) < 0.051) applyThumb(filePath);
            });

    // Restaura as últimas escolhas do usuário.
    QSettings s;
    const bool grid = s.value("welcomeRecentView", "grid").toString() != "list";
    (grid ? m_gridBtn : m_listBtn)->setChecked(true);
    setRecentGrid(grid);

    m_autoSave->setChecked(s.value("autosaveEnabled", false).toBool());
    onAutoSaveToggled(m_autoSave->isChecked());

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
    int resIdx = 1;
    for (int i = 0; i < m_resolution->count(); ++i) {
        const QSize r = m_resolution->itemData(i).toSize();
        if (r.width() > 0 && r == last) { resIdx = i; break; }
    }
    m_resolution->setCurrentIndex(resIdx);
    const int fpsIdx = m_fpsBox->findData(s.value("lastFps", 30).toInt());
    if (fpsIdx >= 0) m_fpsBox->setCurrentIndex(fpsIdx);
    onResolutionChanged(m_resolution->currentIndex());

    m_name->setFocus();
}

void WelcomeWindow::buildUi() {
    const ThemeColors& tc = themeColors();
    auto hx = [](const QColor& c) { return c.name(QColor::HexRgb); };

    // ── Cabeçalho: ícone + marca + versão (faixa sólida) ─────────────────
    auto* header = new QWidget(this);
    header->setObjectName("welcomeHeader");
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setFixedHeight(64);
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(20, 10, 20, 10);
    hl->setSpacing(12);

    auto* logo = new QLabel(header);
    logo->setFixedSize(38, 38);
    logo->setAlignment(Qt::AlignCenter);
    const QPixmap icon(":/pierrot.png");
    if (!icon.isNull())
        logo->setPixmap(icon.scaled(38, 38, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    hl->addWidget(logo);

    auto* titles = new QVBoxLayout;
    titles->setSpacing(0);
    auto* wordmark = new QLabel(tr("Pierrot"), header);
    wordmark->setObjectName("welcomeWordmark");
    auto* subtitle = new QLabel(tr("Editor de vídeo"), header);
    subtitle->setObjectName("welcomeSubtitle");
    titles->addWidget(wordmark);
    titles->addWidget(subtitle);
    hl->addLayout(titles);
    hl->addStretch(1);

    auto* version = new QLabel(tr("Versão %1").arg(QStringLiteral(PIERROT_VERSION)), header);
    version->setObjectName("welcomeVersion");
    hl->addWidget(version);

    // ── Aviso de build de desenvolvimento (só em debug) ─────────────────
    m_devWarn = new QLabel(
        tr("Build de desenvolvimento — recursos podem estar incompletos ou instáveis."), this);
    m_devWarn->setObjectName("welcomeDevWarn");
    m_devWarn->setWordWrap(true);

    // ── Projetos recentes ───────────────────────────────────────────────
    auto* recentBox = new QGroupBox(tr("Projetos recentes"), this);
    m_recent = new QListWidget(recentBox);
    m_recent->setObjectName("recentList");
    m_recent->setUniformItemSizes(true);
    m_recent->setMovement(QListView::Static);
    m_recent->setTextElideMode(Qt::ElideMiddle);
    m_recent->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_recent, &QListWidget::itemDoubleClicked, this, &WelcomeWindow::openSelected);
    connect(m_recent, &QListWidget::itemActivated, this, &WelcomeWindow::openSelected);

    auto* delShort = new QShortcut(QKeySequence::Delete, m_recent);
    delShort->setContext(Qt::WidgetShortcut);
    connect(delShort, &QShortcut::activated, this, &WelcomeWindow::removeSelected);

    // Alternador Grade/Lista (exclusivo): grade com miniaturas ou lista compacta.
    m_listBtn = new QPushButton(tr("Lista"), recentBox);
    m_gridBtn = new QPushButton(tr("Grade"), recentBox);
    m_listBtn->setObjectName("viewToggle");
    m_gridBtn->setObjectName("viewToggle");
    m_listBtn->setCheckable(true);
    m_gridBtn->setCheckable(true);
    m_listBtn->setCursor(Qt::PointingHandCursor);
    m_gridBtn->setCursor(Qt::PointingHandCursor);
    auto* viewGroup = new QButtonGroup(this);
    viewGroup->setExclusive(true);
    viewGroup->addButton(m_listBtn, 0);
    viewGroup->addButton(m_gridBtn, 1);
    connect(viewGroup, &QButtonGroup::idClicked, this,
            [this](int id) { setRecentGrid(id == 1); });

    auto* viewRow = new QHBoxLayout;
    viewRow->setSpacing(4);
    viewRow->addStretch(1);
    viewRow->addWidget(m_listBtn);
    viewRow->addWidget(m_gridBtn);

    auto* browseBtn = new QPushButton(tr("Abrir do disco…"), recentBox);
    browseBtn->setObjectName("secondaryButton");
    browseBtn->setCursor(Qt::PointingHandCursor);
    connect(browseBtn, &QPushButton::clicked, this, &WelcomeWindow::browseProject);

    auto* delBtn = new QPushButton(tr("Excluir"), recentBox);
    delBtn->setObjectName("secondaryButton");
    delBtn->setCursor(Qt::PointingHandCursor);
    delBtn->setToolTip(tr("Remover o projeto selecionado dos recentes"));
    connect(delBtn, &QPushButton::clicked, this, &WelcomeWindow::removeSelected);

    auto* openBtn = new QPushButton(tr("Abrir"), recentBox);
    openBtn->setObjectName("primaryButton");
    openBtn->setCursor(Qt::PointingHandCursor);
    openBtn->setToolTip(tr("Abrir o projeto selecionado"));
    connect(openBtn, &QPushButton::clicked, this, &WelcomeWindow::openSelected);

    auto* recentBtns = new QHBoxLayout;
    recentBtns->addWidget(browseBtn);
    recentBtns->addStretch(1);
    recentBtns->addWidget(delBtn);
    recentBtns->addWidget(openBtn);

    auto* recentLay = new QVBoxLayout(recentBox);
    recentLay->addLayout(viewRow);
    recentLay->addWidget(m_recent, 1);
    recentLay->addLayout(recentBtns);

    // ── Novo projeto ────────────────────────────────────────────────────
    auto* newBox = new QGroupBox(tr("Novo projeto"), this);

    m_name = new QLineEdit(newBox);
    m_name->setPlaceholderText(tr("Nome do projeto"));
    m_name->setText(tr("Meu projeto"));
    connect(m_name, &QLineEdit::returnPressed, this, &WelcomeWindow::requestNewProject);

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

    m_customWidget = new QWidget(newBox);
    auto* customRow = new QHBoxLayout(m_customWidget);
    customRow->setContentsMargins(0, 0, 0, 0);
    customRow->setSpacing(6);
    customRow->addWidget(new QLabel(tr("L:"), m_customWidget));
    customRow->addWidget(m_customW, 1);
    customRow->addSpacing(6);
    customRow->addWidget(new QLabel(tr("A:"), m_customWidget));
    customRow->addWidget(m_customH, 1);

    m_fpsBox = new QComboBox(newBox);
    for (int f : {24, 25, 30, 50, 60, 120})
        m_fpsBox->addItem(QString("%1 fps").arg(f), f);
    m_fpsBox->setCurrentIndex(m_fpsBox->findData(30));

    auto* createBtn = new QPushButton(tr("Criar projeto"), newBox);
    createBtn->setObjectName("primaryButton");
    createBtn->setCursor(Qt::PointingHandCursor);
    createBtn->setDefault(true);
    createBtn->setMinimumHeight(36);
    connect(createBtn, &QPushButton::clicked, this, &WelcomeWindow::requestNewProject);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(10);
    form->addRow(tr("Nome"), m_name);
    form->addRow(tr("Resolução"), m_resolution);
    form->addRow(QString(), m_customWidget);
    form->addRow(tr("Quadros/s"), m_fpsBox);

    auto* createRow = new QHBoxLayout;
    createRow->addStretch(1);
    createRow->addWidget(createBtn);

    auto* newLay = new QVBoxLayout(newBox);
    newLay->addLayout(form);
    newLay->addSpacing(4);
    newLay->addLayout(createRow);

    // ── Salvamento automático ───────────────────────────────────────────
    auto* autoBox = new QGroupBox(tr("Salvamento automático"), this);
    m_autoSave = new QCheckBox(tr("Ativar salvamento automático"), autoBox);
    connect(m_autoSave, &QCheckBox::toggled, this, &WelcomeWindow::onAutoSaveToggled);

    auto* autoNote = new QLabel(tr("Recurso experimental — pode apresentar falhas."), autoBox);
    autoNote->setObjectName("welcomeNote");

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

    auto* autoForm = new QFormLayout;
    autoForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    autoForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    autoForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    autoForm->setHorizontalSpacing(14);
    autoForm->setVerticalSpacing(10);
    autoForm->addRow(tr("Intervalo"), m_autoInterval);
    autoForm->addRow(QString(), m_autoCustom);

    auto* autoLay = new QVBoxLayout(autoBox);
    autoLay->addWidget(m_autoSave);
    autoLay->addWidget(autoNote);
    autoLay->addSpacing(6);
    autoLay->addLayout(autoForm);
    autoLay->addStretch(1);

    // ── Montagem: recentes à esquerda; novo projeto + auto-save à direita ─
    auto* rightCol = new QVBoxLayout;
    rightCol->addWidget(newBox);
    rightCol->addSpacing(14);
    rightCol->addWidget(autoBox);
    rightCol->addStretch(1);

    auto* body = new QHBoxLayout;
    body->setSpacing(18);
    body->addWidget(recentBox, 3);
    body->addLayout(rightCol, 2);

    auto* content = new QWidget(this);
    content->setObjectName("welcomeContent");
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(20, 16, 20, 12);
    contentLay->setSpacing(12);
    contentLay->addLayout(body, 1);

    // ── Rodapé ──────────────────────────────────────────────────────────
    auto* hint = new QLabel(tr("Feche esta janela para abrir o editor vazio."), content);
    hint->setObjectName("welcomeHint");
    auto* credits = new QLabel(tr("by InkSpoty"), content);
    credits->setObjectName("welcomeHint");

    auto* footer = new QHBoxLayout;
    footer->addWidget(hint);
    footer->addStretch(1);
    footer->addWidget(credits);
    contentLay->addLayout(footer);
    contentLay->addWidget(m_devWarn);

#if defined(QT_NO_DEBUG)
    m_devWarn->setVisible(false);
#else
    m_devWarn->setVisible(true);
#endif

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(header);
    root->addWidget(content, 1);

    // ── Folha de estilo sólida (sem gradientes), um único acento azul ────
    const QString sheet = QStringLiteral(
        "QDialog{background:%1;}"
        "QWidget#welcomeHeader{background:%2; border-bottom:1px solid %3;}"
        "QLabel#welcomeWordmark{color:%4; font-size:18px; font-weight:700;}"
        "QLabel#welcomeSubtitle{color:%5; font-size:11px;}"
        "QLabel#welcomeVersion{color:%5; font-size:11px;}"
        "QLabel#welcomeHint{color:%5; font-size:11px;}"
        "QLabel#welcomeNote{color:%6; font-size:10px; font-style:italic;}"
        "QLabel#welcomeDevWarn{background:%7; color:%8; padding:6px 10px;"
        " border-radius:4px; font-weight:600;}"
        "QLabel{color:%4;}"
        "QGroupBox{background:%9; border:1px solid %17; border-radius:6px;"
        " margin-top:16px; padding:14px 12px 12px 12px;}"
        "QGroupBox::title{subcontrol-origin:margin; left:12px; top:1px;"
        " padding:0 6px; color:%10; font-weight:600; font-size:12px;}"
        "QPushButton#primaryButton{background:%11; color:%12; border:none;"
        " border-radius:4px; padding:9px 22px; font-weight:600;}"
        "QPushButton#primaryButton:hover{background:%13;}"
        "QPushButton#primaryButton:pressed{background:%14;}"
        "QPushButton#secondaryButton{background:%15; color:%16;"
        " border:1px solid %17; border-radius:4px; padding:8px 16px;}"
        "QPushButton#secondaryButton:hover{background:%18; border-color:%19;}"
        "QPushButton#secondaryButton:pressed{background:%20;}"
        "QPushButton#viewToggle{background:transparent; color:%4;"
        " border:1px solid %17; border-radius:4px; padding:3px 10px;"
        " font-size:11px;}"
        "QPushButton#viewToggle:hover{background:%18;}"
        "QPushButton#viewToggle:checked{background:%11; color:%12;"
        " border-color:%11;}"
        "QLineEdit,QComboBox,QSpinBox{background:%21; color:%4;"
        " border:1px solid %17; border-radius:4px; padding:6px 10px;"
        " selection-background-color:%11;}"
        "QLineEdit:focus,QComboBox:focus,QSpinBox:focus{border-color:%19;}"
        "QComboBox::drop-down{border:none; width:22px;}"
        "QComboBox QAbstractItemView{background:%9; color:%4;"
        " border:1px solid %17; selection-background-color:%11;"
        " selection-color:%12; outline:none;}"
        "QListWidget#recentList{background:%21; color:%4;"
        " border:1px solid %17; border-radius:4px; padding:6px; outline:none;}"
        "QListWidget#recentList::item{padding:6px; border-radius:4px;"
        " margin:2px; color:%4;}"
        "QListWidget#recentList::item:hover{background:%18;}"
        "QListWidget#recentList::item:selected{background:%11; color:%12;}"
        "QCheckBox{color:%4; spacing:6px;}"
        "QCheckBox::indicator{width:15px; height:15px; border:1px solid %17;"
        " border-radius:3px; background:%21;}"
        "QCheckBox::indicator:checked{background:%11; border-color:%11;}")
        .arg(hx(tc.window))          // 1
        .arg(hx(tc.dockTitleBg))     // 2
        .arg(hx(tc.dockBorder))      // 3
        .arg(hx(tc.windowText))      // 4
        .arg(hx(tc.disabledText))    // 5
        .arg(hx(tc.accentGold))      // 6
        .arg(hx(tc.btnActive))       // 7
        .arg(hx(tc.accentGold))      // 8
        .arg(hx(tc.base))            // 9
        .arg(hx(tc.dockTitleText))   // 10
        .arg(hx(tc.btnPrimary))      // 11
        .arg(hx(tc.btnPrimaryText))  // 12
        .arg(hx(tc.btnPrimary.lighter(118))) // 13
        .arg(hx(tc.btnPrimary.darker(118)))  // 14
        .arg(hx(tc.button))          // 15
        .arg(hx(tc.buttonText))      // 16
        .arg(hx(tc.inputBorder))     // 17
        .arg(hx(tc.ctrlHover))       // 18
        .arg(hx(tc.inputFocus))      // 19
        .arg(hx(tc.ctrlPress))       // 20
        .arg(hx(tc.inputBg));        // 21
    setStyleSheet(sheet);
}

void WelcomeWindow::loadRecentProjects() {
    m_pendingThumbs.clear();
    m_recent->clear();
    for (const QString& path : loadRecentList()) {
        if (!QFile::exists(path)) continue;
        auto* it = new QListWidgetItem(QIcon(makeRecentTile(QImage())),
                                       QFileInfo(path).fileName(), m_recent);
        it->setData(Qt::UserRole, path);
        it->setToolTip(path);
        // Miniatura real: o primeiro vídeo do projeto vira a thumb, decodificada
        // em segundo plano pelo MediaCache (não bloqueia a janela). O tile
        // placeholder fica até o frame chegar.
        const QString media = firstVideoMedia(path);
        if (media.isEmpty()) continue;
        const QImage cached = MediaCache::instance().thumb(media, kProjectThumbTime);
        if (!cached.isNull()) {
            it->setIcon(QIcon(makeRecentTile(cached)));
        } else {
            m_pendingThumbs[media].append(it);
            MediaCache::instance().requestThumb(media, kProjectThumbTime);
        }
    }
    if (m_recent->count() == 0) {
        auto* it = new QListWidgetItem(tr("Nenhum projeto recente."), m_recent);
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(themeColors().disabledText);
    }
}

void WelcomeWindow::setRecentGrid(bool grid) {
    if (grid) {
        m_recent->setViewMode(QListView::IconMode);
        m_recent->setFlow(QListView::LeftToRight);
        m_recent->setIconSize(QSize(168, 94));
        m_recent->setGridSize(QSize(186, 132));
        m_recent->setSpacing(6);
        m_recent->setResizeMode(QListView::Adjust);
        m_recent->setWordWrap(true);
    } else {
        m_recent->setViewMode(QListView::ListMode);
        m_recent->setIconSize(QSize(128, 72));
        m_recent->setGridSize(QSize());
        m_recent->setSpacing(2);
        m_recent->setResizeMode(QListView::Fixed);
        m_recent->setWordWrap(false);
        m_recent->setFlow(QListView::TopToBottom);
    }
    QSettings().setValue("welcomeRecentView", grid ? "grid" : "list");
}

void WelcomeWindow::applyThumb(const QString& mediaPath) {
    const QImage img = MediaCache::instance().thumb(mediaPath, kProjectThumbTime);
    if (img.isNull()) return; // falhou: mantém o tile placeholder
    const auto it = m_pendingThumbs.constFind(mediaPath);
    if (it == m_pendingThumbs.constEnd()) return;
    for (QListWidgetItem* item : it.value()) {
        if (item->listWidget() != m_recent) continue; // item foi removido
        item->setIcon(QIcon(makeRecentTile(img)));
    }
    m_pendingThumbs.remove(mediaPath);
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

void WelcomeWindow::browseProject() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Abrir projeto"), QString(),
        tr("Pierrot (*.Blanc *.ovp);;Todos os arquivos (*)"));
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

void WelcomeWindow::onAutoSaveToggled(bool on) {
    m_autoInterval->setEnabled(on);
    m_autoCustom->setEnabled(on);
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
