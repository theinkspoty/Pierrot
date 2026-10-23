// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/panels/ColorDock.h"
#include "ui/panels/ColorWheel.h"
#include "ui/timeline/TimelineWidget.h"
#include "ui/monitor/PreviewWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QTabWidget>
#include <QScrollArea>
#include <QFrame>
#include <QSpacerItem>
#include <QSignalBlocker>
#include <QInputDialog>
#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QtMath>
#include <cmath>

ColorDock::ColorDock(QWidget* parent) : QWidget(parent) {
    buildUi();
}

void ColorDock::setPreview(PreviewWidget* p) {
    m_preview = p;
}

// Mapeia o offset do puck (-1..1) num canal LGG. A direção injeta a matiz e a
// distância do centro define a intensidade do tint. Lift/Gain têm neutro 0.
static void offsetToRgb(const QVector2D& o, double& r, double& g, double& b) {
    const double ang = std::atan2(-double(o.y()), double(o.x())); // -pi..pi
    const double dist = qMin(1.0, double(o.length()));
    const double hr = std::cos(ang);
    const double hg = std::cos(ang - 2.0943951); // -120°
    const double hb = std::cos(ang + 2.0943951); // +120°
    r = dist * hr;
    g = dist * hg;
    b = dist * hb;
}

// Inversa exata de offsetToRgb: recupera direção (matiz) + intensidade.
static QVector2D rgbToOffset(double r, double g, double b) {
    const double dist = qMin(1.0, std::sqrt(r * r + g * g + b * b) * 0.577350269);
    if (dist < 1e-4) return QVector2D(0.0f, 0.0f);
    const double ang = std::atan2(-b + g, r);
    return QVector2D(float(dist * std::cos(ang)), float(-dist * std::sin(ang)));
}

void ColorDock::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    auto* title = new QLabel(tr("Technicolor"), this);
    title->setStyleSheet(QStringLiteral("font-weight:bold;"));
    root->addWidget(title);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    root->addWidget(m_tabs, 1);

    buildTabs();

    // Conexões das rodas e sliders
    for (ColorWheel* w : {m_lift, m_gamma, m_gain}) {
        connect(w, &ColorWheel::offsetChanged, this, [this](const QVector2D&) {
            if (m_updating) return;
            writeWorkFromWheels();
            applyLive();
        });
        connect(w, &ColorWheel::adjusted, this, [this]() {
            if (m_updating) return;
            pushUndo();
        });
    }

    if (m_exposure) {
        for (QSlider* s : {m_exposure, m_contrast, m_saturation,
                           m_temperature, m_tint, m_blend}) {
            connect(s, &QSlider::valueChanged, this, [this](int) {
                if (m_updating) return;
                writeWorkFromWheels();
                applyLive();
            });
        }
    }

    if (m_resetBtn) {
        connect(m_resetBtn, &QPushButton::clicked, this, [this]() {
            if (!m_clip) return;
            pushUndo();
            const QString lut = m_work.cgLutPath;
            const double lutStr = m_work.cgLutStrength;
            m_work = Clip{};
            m_work.cgLutPath = lut;
            m_work.cgLutStrength = lutStr;
            readWheelsFromWork();
            applyLive();
        });
    }

    if (m_saveLookBtn) {
        connect(m_saveLookBtn, &QPushButton::clicked, this, [this]() {
            if (!m_clip) return;
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Salvar Look"),
                                                       tr("Nome do look:"), QLineEdit::Normal,
                                                       tr("Meu look"), &ok);
            if (!ok || name.trimmed().isEmpty()) return;
            ColorLook lk = lookFromClip(m_work, name.trimmed(), false);
            lk.id = QStringLiteral("user_%1").arg(QDateTime::currentMSecsSinceEpoch());
            m_looks.append(lk);
            saveUserLooks();
            refreshLooksGrid();
        });
    }

    m_status = new QLabel(tr("Selecione um clipe na timeline."), this);
    m_status->setStyleSheet(QStringLiteral("color:#9a9aa4;"));
    root->addWidget(m_status);

    initBuiltInLooks();
    loadUserLooks();
    setMinimumSize(300, 420);
}

void ColorDock::buildTabs() {
    if (!m_tabs) return;
    auto* corr = new QWidget(m_tabs);
    buildCorrectionTab(corr);
    m_tabs->addTab(corr, tr("Correção"));

    auto* wheels = new QWidget(m_tabs);
    buildWheelsTab(wheels);
    m_tabs->addTab(wheels, tr("Rodas"));

    auto* looks = new QWidget(m_tabs);
    buildLooksTab(looks);
    m_tabs->addTab(looks, tr("Looks"));
}

void ColorDock::buildCorrectionTab(QWidget* parent) {
    auto* root = new QVBoxLayout(parent);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* basic = new QGroupBox(tr("Básico"), parent);
    auto* bl = new QGridLayout(basic);
    auto addSlider = [&](int row, const QString& name, QSlider** out,
                         int lo, int hi, int val) {
        bl->addWidget(new QLabel(name, basic), row, 0);
        auto* s = new QSlider(Qt::Horizontal, basic);
        s->setRange(lo, hi);
        s->setValue(val);
        bl->addWidget(s, row, 1);
        *out = s;
    };
    addSlider(0, tr("Exposição"),   &m_exposure,   -500, 500, 0);
    addSlider(1, tr("Contraste"),   &m_contrast,   -100, 100, 0);
    addSlider(2, tr("Saturação"),   &m_saturation, -100, 100, 0);
    addSlider(3, tr("Temperatura"), &m_temperature,-100, 100, 0);
    addSlider(4, tr("Matiz"),       &m_tint,       -100, 100, 0);
    addSlider(5, tr("Intensidade"), &m_blend,          0, 100, 100);
    root->addWidget(basic);

    auto* btns = new QHBoxLayout;
    m_resetBtn = new QPushButton(tr("Resetar grade"), parent);
    btns->addWidget(m_resetBtn);
    btns->addStretch(1);
    root->addLayout(btns);
    root->addStretch(1);
}

void ColorDock::buildWheelsTab(QWidget* parent) {
    auto* root = new QVBoxLayout(parent);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* wheels = new QGroupBox(tr("Lift / Gamma / Gain"), parent);
    auto* gl = new QGridLayout(wheels);
    gl->setSpacing(8);
    m_lift  = new ColorWheel(tr("LIFT"), wheels);
    m_gamma = new ColorWheel(tr("GAMMA"), wheels);
    m_gain  = new ColorWheel(tr("GAIN"), wheels);
    gl->addWidget(m_lift,  0, 0);
    gl->addWidget(m_gamma, 0, 1);
    gl->addWidget(m_gain,  0, 2);
    root->addWidget(wheels);
    root->addStretch(1);
}

void ColorDock::buildLooksTab(QWidget* parent) {
    auto* root = new QVBoxLayout(parent);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* top = new QHBoxLayout;
    auto* lbl = new QLabel(tr("Looks"), parent);
    lbl->setStyleSheet(QStringLiteral("font-weight:bold;"));
    top->addWidget(lbl);
    top->addStretch(1);
    m_saveLookBtn = new QPushButton(tr("Salvar look atual"), parent);
    top->addWidget(m_saveLookBtn);
    root->addLayout(top);

    auto* hint = new QLabel(tr("Clique para aplicar ao clipe selecionado."), parent);
    hint->setStyleSheet(QStringLiteral("color:#9a9aa4;"));
    root->addWidget(hint);

    m_looksScroll = new QScrollArea(parent);
    m_looksScroll->setWidgetResizable(true);
    m_looksScroll->setFrameShape(QFrame::NoFrame);
    m_looksGridHost = new QWidget(m_looksScroll);
    m_looksGrid = new QGridLayout(m_looksGridHost);
    m_looksGrid->setSpacing(6);
    m_looksGridHost->setLayout(m_looksGrid);
    m_looksScroll->setWidget(m_looksGridHost);
    root->addWidget(m_looksScroll, 1);
}

void ColorDock::setClip(Clip* c) {
    if (m_clip == c) return;
    m_clip = c;
    refreshFromClip();
}

void ColorDock::setClipById(const QString& id) {
    if (!m_timeline) { setClip(nullptr); return; }
    setClip(id.isEmpty() ? nullptr : m_timeline->findClipById(id));
}

void ColorDock::refreshFromClip() {
    if (!m_clip) {
        m_status->setText(tr("Selecione um clipe na timeline."));
        return;
    }
    m_work = *m_clip;
    readWheelsFromWork();
    m_status->setText(tr("Clipe: %1").arg(m_clip->name));
}

void ColorDock::readWheelsFromWork() {
    m_updating = true;
    m_lift->setOffset(rgbToOffset(m_work.liftR,  m_work.liftG,  m_work.liftB));
    m_gamma->setOffset(rgbToOffset(m_work.gammaR - 1.0, m_work.gammaG - 1.0,
                                   m_work.gammaB - 1.0));
    m_gain->setOffset(rgbToOffset(m_work.gainR,  m_work.gainG,  m_work.gainB));
    const QSignalBlocker b1(m_exposure), b2(m_contrast), b3(m_saturation);
    const QSignalBlocker b4(m_temperature), b5(m_tint), b6(m_blend);
    m_exposure->setValue(int(std::lround(m_work.cgExposure * 100.0)));
    m_contrast->setValue(int(std::lround(m_work.cgContrast)));
    m_saturation->setValue(int(std::lround(m_work.cgSaturation)));
    m_temperature->setValue(int(std::lround(m_work.cgTemperature)));
    m_tint->setValue(int(std::lround(m_work.cgTint)));
    m_blend->setValue(int(std::lround(m_work.cgBlend * 100.0)));
    m_updating = false;
}

void ColorDock::writeWorkFromWheels() {
    offsetToRgb(m_lift->offset(),  m_work.liftR, m_work.liftG, m_work.liftB);
    offsetToRgb(m_gamma->offset(), m_work.gammaR, m_work.gammaG, m_work.gammaB);
    m_work.gammaR = qBound(0.1, m_work.gammaR + 1.0, 4.0);
    m_work.gammaG = qBound(0.1, m_work.gammaG + 1.0, 4.0);
    m_work.gammaB = qBound(0.1, m_work.gammaB + 1.0, 4.0);
    offsetToRgb(m_gain->offset(),  m_work.gainR, m_work.gainG, m_work.gainB);
    m_work.cgExposure   = m_exposure->value() / 100.0;
    m_work.cgContrast   = m_contrast->value();
    m_work.cgSaturation = m_saturation->value();
    m_work.cgTemperature= m_temperature->value();
    m_work.cgTint       = m_tint->value();
    m_work.cgBlend      = m_blend->value() / 100.0;
}

void ColorDock::applyLive() {
    if (!m_clip) return;
    *m_clip = m_work;
    updatePreviewFrame();
    if (m_timeline) {
        m_timeline->update();
        m_timeline->invalidateScene();
    }
}

void ColorDock::updatePreviewFrame() {
    // O PreviewWidget já recompõe a partir do clipe a cada refreshView();
    // o applyLive() chama m_timeline->update() que dispara essa recomposição.
    if (m_preview) m_preview->refreshView();
}

void ColorDock::pushUndo() {
    if (m_timeline) emit m_timeline->editStart();
}

// ── Looks ──────────────────────────────────────────────────────────────

QString ColorDock::looksDir() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir d(base);
    if (!d.exists(QStringLiteral("pierrot"))) d.mkpath(QStringLiteral("pierrot"));
    if (!d.cd(QStringLiteral("pierrot"))) return base;
    if (!d.exists(QStringLiteral("looks"))) d.mkpath(QStringLiteral("looks"));
    d.cd(QStringLiteral("looks"));
    return d.absolutePath();
}

void ColorDock::initBuiltInLooks() {
    if (!m_builtInLooks.isEmpty()) return;
    auto mk = [&](const QString& name, double ex, double con, double sat, double temp, double tint,
                  double fr, double liftR, double liftG, double liftB, double gammaR, double gammaG, double gammaB,
                  double gainR, double gainG, double gainB, double vib) {
        ColorLook lk;
        QString nid = name.toLower();
        nid.replace(QStringLiteral(" "), QStringLiteral("_"));
        nid.replace(QStringLiteral("á"), QStringLiteral("a"));
        nid.replace(QStringLiteral("à"), QStringLiteral("a"));
        nid.replace(QStringLiteral("ã"), QStringLiteral("a"));
        nid.replace(QStringLiteral("â"), QStringLiteral("a"));
        nid.replace(QStringLiteral("é"), QStringLiteral("e"));
        nid.replace(QStringLiteral("ê"), QStringLiteral("e"));
        nid.replace(QStringLiteral("í"), QStringLiteral("i"));
        nid.replace(QStringLiteral("ó"), QStringLiteral("o"));
        nid.replace(QStringLiteral("ô"), QStringLiteral("o"));
        nid.replace(QStringLiteral("õ"), QStringLiteral("o"));
        nid.replace(QStringLiteral("ú"), QStringLiteral("u"));
        nid.replace(QStringLiteral("ç"), QStringLiteral("c"));
        lk.id = QStringLiteral("builtin_") + nid;
        lk.name = name;
        lk.builtIn = true;
        lk.cgExposure = ex; lk.cgContrast = con; lk.cgSaturation = sat; lk.cgTemperature = temp; lk.cgTint = tint;
        lk.cgVibrance = vib; lk.cgFadedFilm = fr; lk.cgBlend = 1.0;
        lk.liftR=liftR; lk.liftG=liftG; lk.liftB=liftB;
        lk.gammaR=gammaR; lk.gammaG=gammaG; lk.gammaB=gammaB;
        lk.gainR=gainR; lk.gainG=gainG; lk.gainB=gainB;
        m_builtInLooks.append(lk);
    };
    // Looks inspirados no espírito analógico, sutis para vídeo
    mk(tr("Neutro Revelado"), 0.05, 8, -4, 2, 0, 4, -0.02, -0.01, 0, 1.02, 1.01, 1.0, 0.02, 0.01, 0, 4);
    mk(tr("Cine Suave"), 0.08, 12, -8, 6, -2, 10, -0.04, -0.02, 0, 1.04, 1.02, 0.98, 0.04, 0.02, -0.01, 8);
    mk(tr("Película Quente"), 0.06, 10, -6, 10, -4, 12, -0.03, -0.015, 0.01, 1.03, 1.015, 0.98, 0.05, 0.02, 0, 6);
    mk(tr("Fresco"), -0.02, 6, -2, -6, 4, 6, -0.01, -0.005, 0.02, 1.01, 1.01, 1.02, 0.01, 0.01, 0.03, 4);
    mk(tr("Nostálgico"), 0.04, 6, -12, 12, -6, 18, -0.05, -0.02, 0, 1.02, 1.0, 0.96, 0.03, 0.01, -0.02, 10);
    mk(tr("Crepúsculo"), -0.04, 14, -10, 14, -2, 8, -0.03, -0.015, 0.02, 1.05, 1.02, 0.96, 0.06, 0.03, 0, 10);
    refreshLooksGrid();
}

void ColorDock::loadUserLooks() {
    const QString dir = looksDir();
    QDir d(dir);
    for (const QString& f : d.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile file(d.filePath(f));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const auto doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) continue;
        const auto o = doc.object();
        ColorLook lk;
        lk.id = o.value(QStringLiteral("id")).toString();
        lk.name = o.value(QStringLiteral("name")).toString();
        lk.builtIn = false;
        lk.liftR=o.value(QStringLiteral("liftR")).toDouble(0); lk.liftG=o.value(QStringLiteral("liftG")).toDouble(0); lk.liftB=o.value(QStringLiteral("liftB")).toDouble(0);
        lk.gammaR=o.value(QStringLiteral("gammaR")).toDouble(1); lk.gammaG=o.value(QStringLiteral("gammaG")).toDouble(1); lk.gammaB=o.value(QStringLiteral("gammaB")).toDouble(1);
        lk.gainR=o.value(QStringLiteral("gainR")).toDouble(0); lk.gainG=o.value(QStringLiteral("gainG")).toDouble(0); lk.gainB=o.value(QStringLiteral("gainB")).toDouble(0);
        lk.cgExposure=o.value(QStringLiteral("cgExposure")).toDouble(0); lk.cgContrast=o.value(QStringLiteral("cgContrast")).toDouble(0);
        lk.cgHighlights=o.value(QStringLiteral("cgHighlights")).toDouble(0); lk.cgShadows=o.value(QStringLiteral("cgShadows")).toDouble(0);
        lk.cgWhites=o.value(QStringLiteral("cgWhites")).toDouble(0); lk.cgBlacks=o.value(QStringLiteral("cgBlacks")).toDouble(0);
        lk.cgSaturation=o.value(QStringLiteral("cgSaturation")).toDouble(0); lk.cgVibrance=o.value(QStringLiteral("cgVibrance")).toDouble(0);
        lk.cgTemperature=o.value(QStringLiteral("cgTemperature")).toDouble(0); lk.cgTint=o.value(QStringLiteral("cgTint")).toDouble(0);
        lk.cgFadedFilm=o.value(QStringLiteral("cgFadedFilm")).toDouble(0); lk.cgSharpen=o.value(QStringLiteral("cgSharpen")).toDouble(0);
        lk.cgVignette=o.value(QStringLiteral("cgVignette")).toDouble(0); lk.cgVignetteFeather=o.value(QStringLiteral("cgVignetteFeather")).toDouble(50);
        lk.cgBlend=o.value(QStringLiteral("cgBlend")).toDouble(1.0);
        lk.cgLutPath=o.value(QStringLiteral("cgLutPath")).toString();
        lk.cgLutStrength=o.value(QStringLiteral("cgLutStrength")).toDouble(1.0);
        if (!lk.name.isEmpty()) {
            bool exists = false;
            for (const auto& x : m_looks) if (x.id == lk.id) { exists = true; break; }
            if (!exists) m_looks.append(lk);
        }
    }
    refreshLooksGrid();
}

void ColorDock::saveUserLooks() {
    for (const auto& lk : m_looks) {
        if (lk.builtIn) continue;
        QJsonObject o;
        o[QStringLiteral("id")] = lk.id; o[QStringLiteral("name")] = lk.name;
        o[QStringLiteral("liftR")]=lk.liftR; o[QStringLiteral("liftG")]=lk.liftG; o[QStringLiteral("liftB")]=lk.liftB;
        o[QStringLiteral("gammaR")]=lk.gammaR; o[QStringLiteral("gammaG")]=lk.gammaG; o[QStringLiteral("gammaB")]=lk.gammaB;
        o[QStringLiteral("gainR")]=lk.gainR; o[QStringLiteral("gainG")]=lk.gainG; o[QStringLiteral("gainB")]=lk.gainB;
        o[QStringLiteral("cgExposure")]=lk.cgExposure; o[QStringLiteral("cgContrast")]=lk.cgContrast;
        o[QStringLiteral("cgHighlights")]=lk.cgHighlights; o[QStringLiteral("cgShadows")]=lk.cgShadows;
        o[QStringLiteral("cgWhites")]=lk.cgWhites; o[QStringLiteral("cgBlacks")]=lk.cgBlacks;
        o[QStringLiteral("cgSaturation")]=lk.cgSaturation; o[QStringLiteral("cgVibrance")]=lk.cgVibrance;
        o[QStringLiteral("cgTemperature")]=lk.cgTemperature; o[QStringLiteral("cgTint")]=lk.cgTint;
        o[QStringLiteral("cgFadedFilm")]=lk.cgFadedFilm; o[QStringLiteral("cgSharpen")]=lk.cgSharpen;
        o[QStringLiteral("cgVignette")]=lk.cgVignette; o[QStringLiteral("cgVignetteFeather")]=lk.cgVignetteFeather;
        o[QStringLiteral("cgBlend")]=lk.cgBlend;
        o[QStringLiteral("cgLutPath")]=lk.cgLutPath; o[QStringLiteral("cgLutStrength")]=lk.cgLutStrength;
        QFile file(QDir(looksDir()).filePath(lk.id + QStringLiteral(".json")));
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
}

ColorDock::ColorLook ColorDock::lookFromClip(const Clip& c, const QString& name, bool builtIn) const {
    ColorLook lk;
    lk.name = name; lk.builtIn = builtIn;
    lk.liftR=c.liftR; lk.liftG=c.liftG; lk.liftB=c.liftB;
    lk.gammaR=c.gammaR; lk.gammaG=c.gammaG; lk.gammaB=c.gammaB;
    lk.gainR=c.gainR; lk.gainG=c.gainG; lk.gainB=c.gainB;
    lk.cgExposure=c.cgExposure; lk.cgContrast=c.cgContrast;
    lk.cgHighlights=c.cgHighlights; lk.cgShadows=c.cgShadows;
    lk.cgWhites=c.cgWhites; lk.cgBlacks=c.cgBlacks;
    lk.cgSaturation=c.cgSaturation; lk.cgVibrance=c.cgVibrance;
    lk.cgTemperature=c.cgTemperature; lk.cgTint=c.cgTint;
    lk.cgFadedFilm=c.cgFadedFilm; lk.cgSharpen=c.cgSharpen;
    lk.cgVignette=c.cgVignette; lk.cgVignetteFeather=c.cgVignetteFeather;
    lk.cgBlend=c.cgBlend;
    lk.cgLutPath=c.cgLutPath; lk.cgLutStrength=c.cgLutStrength;
    lk.cgMasterCurve=c.cgMasterCurve; lk.cgRCurve=c.cgRCurve; lk.cgGCurve=c.cgGCurve; lk.cgBCurve=c.cgBCurve;
    return lk;
}

Clip ColorDock::clipFromLook(const ColorDock::ColorLook& look) const {
    Clip c = m_work;
    c.liftR=look.liftR; c.liftG=look.liftG; c.liftB=look.liftB;
    c.gammaR=look.gammaR; c.gammaG=look.gammaG; c.gammaB=look.gammaB;
    c.gainR=look.gainR; c.gainG=look.gainG; c.gainB=look.gainB;
    c.cgExposure=look.cgExposure; c.cgContrast=look.cgContrast;
    c.cgHighlights=look.cgHighlights; c.cgShadows=look.cgShadows;
    c.cgWhites=look.cgWhites; c.cgBlacks=look.cgBlacks;
    c.cgSaturation=look.cgSaturation; c.cgVibrance=look.cgVibrance;
    c.cgTemperature=look.cgTemperature; c.cgTint=look.cgTint;
    c.cgFadedFilm=look.cgFadedFilm; c.cgSharpen=look.cgSharpen;
    c.cgVignette=look.cgVignette; c.cgVignetteFeather=look.cgVignetteFeather;
    c.cgBlend=look.cgBlend;
    if (!look.cgLutPath.isEmpty()) { c.cgLutPath = look.cgLutPath; c.cgLutStrength = look.cgLutStrength; }
    c.cgMasterCurve=look.cgMasterCurve; c.cgRCurve=look.cgRCurve; c.cgGCurve=look.cgGCurve; c.cgBCurve=look.cgBCurve;
    return c;
}

void ColorDock::applyLook(const ColorDock::ColorLook& look) {
    if (!m_clip) return;
    pushUndo();
    m_work = clipFromLook(look);
    readWheelsFromWork();
    applyLive();
    if (m_tabs) m_tabs->setCurrentIndex(0);
    m_status->setText(tr("Look aplicado: %1").arg(look.name));
}

void ColorDock::refreshLooksGrid() {
    if (!m_looksGrid) return;
    while (QLayoutItem* it = m_looksGrid->takeAt(0)) {
        if (it->widget()) { QWidget* w = it->widget(); m_looksGrid->removeWidget(w); w->deleteLater(); }
        delete it;
    }
    QVector<ColorDock::ColorLook> all;
    for (const auto& lk : m_builtInLooks) all.append(lk);
    for (const auto& lk : m_looks) if (!lk.builtIn) all.append(lk);
    int col = 0, row = 0;
    const int cols = 2;
    for (const auto& lk : all) {
        auto* btn = new QPushButton(lk.name, m_looksGridHost);
        btn->setMinimumHeight(36);
        btn->setToolTip(tr("Aplicar look ao clipe selecionado"));
        connect(btn, &QPushButton::clicked, this, [this, lk]() { applyLook(lk); });
        m_looksGrid->addWidget(btn, row, col);
        if (++col >= cols) { col = 0; ++row; }
    }
    m_looksGrid->addItem(new QSpacerItem(10,10,QSizePolicy::Expanding,QSizePolicy::Expanding), row, 0, 1, cols);
}
