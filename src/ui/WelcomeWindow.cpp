// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "WelcomeWindow.h"
#include "version.h"
#include "ui/TitleBar.h"
#include "ui/Theme.h"

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
#include <QIcon>
#include <QLinearGradient>
#include <QImage>
#include <QWidget>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include "ffmpeg/MediaCache.h"
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

// Primeira mídia de vídeo (com arquivo) do projeto .Blanc: o frame dela vira a
// miniatura real do projeto. Percorre o JSON direto, sem instanciar o projeto.
constexpr double kProjectThumbTime = 0.5;
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
    if (tmp.width() < w && tmp.height() < h) {
        tmp = tmp.scaled(w, h, Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
    }
    const double scale = qMax(w / double(tmp.width()), h / double(tmp.height()));
    const int W = qCeil(tmp.width() * scale);
    const int H = qCeil(tmp.height() * scale);
    tmp = tmp.scaled(W, H, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    tmp = tmp.copy((W - w) / 2, (H - h) / 2, w, h);
    return QPixmap::fromImage(tmp);
}

// "Miniatura" de um projeto recente: mesmo esquema visual dos clipes da
// timeline (clipBg) — um tom mais claro no topo, mais escuro embaixo, borda
// sutil e nome na barra inferior. Se houver frame real (vídeo/proxy), ele
// preenche por cima; sem frame fica o placeholder escuro estilo timeline.
QPixmap makeProjectThumb(const QString& path, const QString& name,
                         const QImage& frame = QImage()) {
    Q_UNUSED(path);
    const int W = 132, H = 74;
    const auto& tc = themeColors();
    QPixmap out(W, H);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);

    QLinearGradient g(0, 0, W, H);
    g.setColorAt(0, tc.clipBg.lighter(115));
    g.setColorAt(1, tc.clipBg.darker(150));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(0, 0, W - 1, H - 1, 6, 6);

    QPainterPath clipPath;
    clipPath.addRoundedRect(2, 2, W - 4, H - 4, 5, 5);
    p.save();
    p.setClipPath(clipPath);
    if (!frame.isNull()) {
        const QPixmap pm = coverThumb(frame, W - 4, H - 4);
        p.drawPixmap(2, 2, pm);
    } else {
        // Placeholder sem frame: "janela" escura no centro, sem tons claros.
        p.setBrush(QColor(0, 0, 0, 90));
        p.drawRect(0, 0, W, H);
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.setBrush(QColor(0, 0, 0, 60));
        p.drawRoundedRect((W - 34) / 2, H / 2 - 12, 34, 24, 3, 3);
        p.setPen(QColor(255, 255, 255, 40));
        p.drawLine((W - 16) / 2, H / 2 - 4, (W - 16) / 2 + 12, H / 2);
        p.drawLine((W - 16) / 2, H / 2, (W - 16) / 2 + 12, H / 2 + 4);
    }
    // Faixa escura inferior com o nome (legível em qualquer frame).
    p.fillRect(0, H - 15, W, 15, QColor(0, 0, 0, 110));
    p.setPen(QColor(255, 255, 255, 235));
    QFont f;
    f.setPointSize(6);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(3, H - 15, W - 6, 14), Qt::AlignCenter, name.left(15));
    p.restore();

    p.setPen(QPen(tc.clipBorder, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(0, 0, W - 1, H - 1, 6, 6);
    p.end();
    return out;
}

// Estilo dos campos (nome, resolução, fps, intervalo…): fundo sempre escuro e
// borda limpa, com foco ciano sutil — fixado no nível do grupo (independe do
// fallback de tema, como a lista e os botões).
QString inputSheet() {
    return QStringLiteral(
        "QLineEdit,QComboBox,QSpinBox{background:#17191C;"
        " border:1px solid #383C42; border-radius:7px;"
        " padding:7px 11px; color:#DCDEE2; selection-background-color:#0086C8;}"
        "QLineEdit:focus,QComboBox:focus,QSpinBox:focus{border-color:#00A3E4;}"
        "QComboBox::drop-down{border:none; width:24px;}"
        "QComboBox QAbstractItemView{background:#1E2024;"
        " border:1px solid #383C42; selection-background-color:#0086C8;"
        " border-radius:5px;}");
}

// Botão primário azul (gradiente) com letras brancas — usado no Criar projeto,
// Abrir e Excluir. Estilo fixado no widget para não depender do fallback de tema.
QString primaryBtnSheet(int fontSize = 13) {
    return QStringLiteral(
        "QPushButton{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        " stop:0 %1, stop:1 %2);"
        " border:none; border-radius:8px;"
        " color:%3; padding:10px 28px; font-weight:bold; font-size:%7px;}"
        "QPushButton:hover{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        " stop:0 %4, stop:1 %5);}"
        "QPushButton:pressed{background:%6;}")
        .arg(themeColors().welcomeBtnGradStart.name())
        .arg(themeColors().welcomeBtnGradEnd.name())
        .arg(themeColors().highlightedText.name())
        .arg(themeColors().btnPrimary.name())
        .arg(themeColors().accent.name())
        .arg(themeColors().btnActive.name())
        .arg(fontSize);
}
}

// Cor média de uma faixa horizontal central da imagem (a região que fica
// visível na faixa do topo), usada para o degradê de transição com o fundo.
QColor avgBandColor(const QImage& img) {
    if (img.isNull()) return QColor(8, 10, 14);
    const int y0 = img.height() * 45 / 100;
    const int y1 = qMax(y0 + 1, img.height() * 55 / 100);
    qlonglong r = 0, g = 0, b = 0, n = 0;
    for (int y = y0; y < y1; y += 3) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 4) {
            const QRgb px = line[x];
            r += qRed(px); g += qGreen(px); b += qBlue(px); ++n;
        }
    }
    if (!n) return QColor(8, 10, 14);
    return QColor(int(r / n), int(g / n), int(b / n));
}

// Widget que desenha o banner cobrindo toda a área (semântica "cover"): a
// imagem é escalada para preencher a largura E a altura, e o excesso é cortado
// (crop) — vira uma faixa de ponta a ponta sem distorcer a proporção.
// Vivo no escopo global pois WelcomeWindow.h o declara (membro do tipo ponteiro).
class BannerWidget : public QWidget {
public:
    explicit BannerWidget(QWidget* parent = nullptr) : QWidget(parent) {}
    void setImage(const QPixmap& pm) { m_pm = pm; update(); }
    // Cor média da faixa visível: a metade inferior da imagem derrete nessa cor
    // para a transição com o fundo ficar contínua (sem costura).
    void setFadeColor(const QColor& c) { m_fade = c; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        if (m_pm.isNull()) return;
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const double scale = qMax(rect().width() / double(m_pm.width()),
                                  rect().height() / double(m_pm.height()));
        const int w = qCeil(m_pm.width() * scale);
        const int h = qCeil(m_pm.height() * scale);
        QPointF origin((rect().width() - w) / 2.0, (rect().height() - h) / 2.0);
        p.translate(origin);
        p.scale(scale, scale);
        p.drawPixmap(0, 0, m_pm);
        if (m_fade.isValid()) {
            // Suaviza a parte de baixo da faixa visível até a cor média
            // (dissolve) — em coordenadas do widget, então acompanha o crop.
            p.resetTransform();
            QLinearGradient g(0, rect().height() * 0.45, 0, rect().height());
            QColor c0 = m_fade; c0.setAlpha(0);
            QColor c1 = m_fade; c1.setAlpha(150);
            g.setColorAt(0.0, c0);
            g.setColorAt(1.0, c1);
            p.fillRect(rect(), g);
        }
    }
private:
    QPixmap m_pm;
    QColor m_fade;
};

WelcomeWindow::WelcomeWindow(QWidget* parent) : QDialog(parent) {
    // Janela sem as bordas do sistema: a barra de título é personalizada.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    // Transparência com cantos arredondados: o paintEvent pinta o fundo opaco
    // (gradiente) dentro de um retângulo arredondado; fora do raio é
    // transparente, dando os cantos arredondados à janela.
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("Bem-vindo ao Pierrot"));
    setMinimumSize(880, 600);
    resize(1100, 700);

    buildLayout();
    loadRecentProjects();

    // Miniatura real chega em segundo plano (decodificação no MediaCache).
    connect(&MediaCache::instance(), &MediaCache::thumbnailReady, this,
            [this](const QString& filePath, double seconds) {
                if (qAbs(seconds - kProjectThumbTime) < 0.051) applyThumb(filePath);
            });

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
    // Faixa do topo: a imagem vai de ponta a ponta (faixa fina) — a janela
    // corta o excesso para se adequar, sem distorcer a proporção. BannerWidget
    // desenha com semântica "cover". Embaixo entra um degradê de transição.
    m_banner = new BannerWidget(this);
    m_banner->setObjectName(QStringLiteral("welcomeBanner"));
    m_banner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_banner->setFixedHeight(96);
    m_bannerImg = QPixmap();
    if (m_bannerImg.load(":/pierrot_banner.png")
        || m_bannerImg.load("imagens/pierrot banner.jpg")
        || m_bannerImg.load(QCoreApplication::applicationDirPath() + "/imagens/pierrot banner.jpg")) {
        // carregou da resource ou do disco
    } else {
        m_bannerImg = QPixmap(8, 8);
        m_bannerImg.fill(Qt::transparent);
    }
    m_banner->setImage(m_bannerImg);

    // Cor média da faixa visível: usada para o banner derreter nela e para o
    // degradê de transição com o fundo (combina com qualquer imagem).
    const QColor band = avgBandColor(m_bannerImg.toImage());
    m_banner->setFadeColor(band);

    // Degradê de transição entre a faixa e o fundo da janela: começa na mesma
    // cor média em que o banner já dissolveu e vai até transparente.
    auto* fade = new QWidget(this);
    fade->setAttribute(Qt::WA_StyledBackground, true);
    fade->setFixedHeight(24);
    const int fadeA = band.lightness() < 120 ? 120 : 85;
    fade->setStyleSheet(QStringLiteral(
        "background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        " stop:0 rgba(%1,%2,%3,%4), stop:1 rgba(%1,%2,%3,0));")
        .arg(band.red()).arg(band.green()).arg(band.blue()).arg(fadeA));

    auto* credits = new QLabel(tr("by InkSpoty"), this);
    credits->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(themeColors().disabledText.name()));
    credits->setAlignment(Qt::AlignHCenter);

    auto* version = new QLabel(tr("Versão %1").arg(QStringLiteral(PIERROT_VERSION)), this);
    version->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(themeColors().disabledText.name()));
    version->setAlignment(Qt::AlignHCenter);

    // Projetos recentes
    auto* recentBox = new QGroupBox(tr("Projetos recentes"), this);
    recentBox->setMinimumHeight(320);
    m_recent = new QListWidget(recentBox);
    m_recent->setMinimumHeight(260);
    m_recent->setIconSize(QSize(132, 74));
    m_recent->setUniformItemSizes(true);
    m_recent->setSpacing(2);
    // Fundo sempre escuro (estilo da timeline), item texto claro — não depende
    // do fallback de tema (evita lista branca no tema escuro).
    m_recent->setStyleSheet(
        "QListWidget{background:#131518; border:1px solid #23262c;"
        " border-radius:8px; padding:5px; outline:none;}"
        "QListWidget::item{padding:13px 14px; border-radius:6px; margin:3px;"
        " color:#dcdfe4;}"
        "QListWidget::item:hover{background:rgba(110,160,230,0.12);}"
        "QListWidget::item:selected{background:rgba(70,130,210,0.32); color:white;}");
    connect(m_recent, &QListWidget::itemDoubleClicked, this,
            &WelcomeWindow::openSelected);

    auto* delShort = new QShortcut(QKeySequence::Delete, m_recent);
    delShort->setContext(Qt::WidgetShortcut);
    connect(delShort, &QShortcut::activated, this, &WelcomeWindow::removeSelected);

    auto* openBtn = new QPushButton(tr("Abrir"), recentBox);
    openBtn->setToolTip(tr("Abrir o projeto selecionado"));
    openBtn->setStyleSheet(primaryBtnSheet());
    connect(openBtn, &QPushButton::clicked, this, &WelcomeWindow::openSelected);

    auto* delBtn = new QPushButton(tr("Excluir"), recentBox);
    delBtn->setToolTip(tr("Remover o projeto selecionado dos recentes"));
    delBtn->setStyleSheet(primaryBtnSheet());
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
    newBox->setStyleSheet(inputSheet());

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
    createBtn->setStyleSheet(primaryBtnSheet(14));
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
    autoBox->setStyleSheet(inputSheet());
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
    hint->setStyleSheet(QStringLiteral("color: %1;").arg(themeColors().disabledText.name()));

    // Aviso de fase inicial de desenvolvimento, no canto inferior esquerdo.
    auto* betaWarn = new QLabel(
        tr("O editor está no início do desenvolvimento e pode apresentar falhas."), this);
    betaWarn->setStyleSheet(QStringLiteral("color: %1; font-size: 10px; font-style: italic;").arg(themeColors().accentGold.name()));
    betaWarn->setWordWrap(false);

    // Aviso de versão de desenvolvimento: exibido apenas em builds de debug.
    m_devWarn = new QLabel(tr("Você está usando uma versão de desenvolvimento do Pierrot. "
                              "Recursos podem estar incompletos ou instáveis."), this);
    m_devWarn->setWordWrap(true);
    m_devWarn->setStyleSheet(
        QStringLiteral("background-color: %1; color: %2; padding: 6px 10px;"
        "border-radius: 4px; font-weight: bold;")
        .arg(themeColors().btnActive.name())
        .arg(themeColors().accentGold.name()));
    m_devWarn->setVisible(false);

    // Montagem principal: recentes à esquerda (destaque, como no Vegas),
    // "Novo projeto" + auto-save à direita.
    auto* leftCol = new QVBoxLayout;
    leftCol->addWidget(recentBox, 1);

    auto* rightCol = new QVBoxLayout;
    rightCol->addWidget(newBox);
    rightCol->addSpacing(12);
    rightCol->addWidget(autoBox);
    rightCol->addStretch(1);

    auto* body = new QHBoxLayout;
    body->setSpacing(24);
    body->addLayout(leftCol, 3);
    body->addLayout(rightCol, 2);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    // Barra de título personalizada (sem as bordas do sistema).
    auto* titleBar = new TitleBar(tr("Bem-vindo ao Pierrot"), this);
    root->addWidget(titleBar);

    // Faixa do topo + degradê, sem margens: vão de um canto a outro da janela.
    root->addWidget(m_banner);
    root->addWidget(fade);

    auto* content = new QWidget(this);
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(28, 10, 28, 12);
    contentLay->setSpacing(10);
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
        "QGroupBox{border:1px solid %1; border-radius:12px; margin-top:18px;"
        " background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "  stop:0 %2, stop:1 %3);}"
        "QGroupBox::title{subcontrol-origin:margin; left:15px; top:2px; padding:0 9px;"
        " color:white; font-weight:700; font-size:13px; letter-spacing:0.5px;}"
        "QListWidget{background:%5; border:1px solid %6;"
        " border-radius:8px; padding:5px; outline:none;}"
        "QListWidget::item{padding:13px 14px; border-radius:6px; margin:3px; color:%7;}"
        "QListWidget::item:hover{background:rgba(110,160,230,0.12);}"
        "QListWidget::item:selected{background:rgba(70,130,210,0.32); color:%8;}"
        "QPushButton{border:1px solid %9; border-radius:7px; background:%10;"
        " color:%11; padding:8px 18px; font-weight:bold;}"
        "QPushButton:hover{background:%12; border-color:%13;}"
        "QPushButton:pressed{background:%14;}"
        "QLineEdit,QComboBox,QSpinBox{background:%15;"
        " border:1px solid %16; border-radius:7px; padding:7px 11px;"
        " color:%17; selection-background-color:%18;}"
        "QLineEdit:focus,QComboBox:focus,QSpinBox:focus{border-color:%19;}"
        "QComboBox::drop-down{border:none; width:24px;}"
        "QComboBox QAbstractItemView{background:%20; border:1px solid %21;"
        " selection-background-color:%22; border-radius:5px;}"
        "QCheckBox{color:%23; spacing:6px;}"
        "QLabel{color:%24;}")
        .arg(themeColors().dockBorder.name())
        .arg(themeColors().welcomeBgTop.name())
        .arg(themeColors().welcomeBgBottom.name())
        .arg(themeColors().dockTitleText.name())
        .arg(themeColors().base.name())
        .arg(themeColors().inputBorder.name())
        .arg(themeColors().text.name())
        .arg(themeColors().highlightedText.name())
        .arg(themeColors().inputBorder.name())
        .arg(themeColors().button.name())
        .arg(themeColors().buttonText.name())
        .arg(themeColors().btnHover.name())
        .arg(themeColors().dockBorder.name())
        .arg(themeColors().btnActive.name())
        .arg(themeColors().base.name())
        .arg(themeColors().inputBorder.name())
        .arg(themeColors().text.name())
        .arg(themeColors().highlight.name())
        .arg(themeColors().inputFocus.name())
        .arg(themeColors().alternateBase.name())
        .arg(themeColors().inputBorder.name())
        .arg(themeColors().highlight.name())
        .arg(themeColors().text.name())
        .arg(themeColors().text.name()));
}

void WelcomeWindow::loadRecentProjects() {
    m_pendingThumbs.clear();
    m_recent->clear();
    for (const QString& path : loadRecentList()) {
        if (!QFile::exists(path)) continue;
        auto* it = new QListWidgetItem(
            QIcon(makeProjectThumb(path, QFileInfo(path).fileName())),
            QFileInfo(path).fileName(), m_recent);
        it->setData(Qt::UserRole, path);
        it->setToolTip(path);
        // Miniatura real: o primeiro vídeo do projeto vira a thumb, decodificada
        // em segundo plano pelo MediaCache (não bloqueia a janela). O tile
        // gradiente fica como placeholder até o frame chegar.
        const QString media = firstVideoMedia(path);
        if (media.isEmpty()) continue;
        const QImage cached = MediaCache::instance().thumb(media, kProjectThumbTime);
        const QString fn = QFileInfo(path).fileName();
        if (!cached.isNull()) {
            it->setIcon(QIcon(makeProjectThumb(path, fn, cached)));
        } else {
            m_pendingThumbs[media].append(it);
            MediaCache::instance().requestThumb(media, kProjectThumbTime);
        }
    }
    if (m_recent->count() == 0) {
        auto* it = new QListWidgetItem(tr("Nenhum projeto recente."), m_recent);
        it->setFlags(Qt::NoItemFlags);
        it->setForeground(QColor(120, 124, 132));
    }
}

void WelcomeWindow::applyThumb(const QString& filePath) {
    const QImage img = MediaCache::instance().thumb(filePath, kProjectThumbTime);
    if (img.isNull()) return; // falhou: mantém o tile placeholder
    const auto it = m_pendingThumbs.constFind(filePath);
    if (it == m_pendingThumbs.constEnd()) return;
    for (QListWidgetItem* item : it.value()) {
        if (item->listWidget() != m_recent) continue; // item foi removido
        const QString path = item->data(Qt::UserRole).toString();
        item->setIcon(QIcon(makeProjectThumb(path, QFileInfo(path).fileName(), img)));
    }
    m_pendingThumbs.remove(filePath);
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
    g.setColorAt(0, themeColors().welcomeBgTop);
    g.setColorAt(1, themeColors().welcomeBgBottom);
    p.fillPath(path, g);
}

void WelcomeWindow::resizeEvent(QResizeEvent* ev) {
    update(); // repinta com o novo tamanho (cantos arredondados)
    QDialog::resizeEvent(ev);
}
