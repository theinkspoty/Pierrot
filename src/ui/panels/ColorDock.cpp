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
#include <QSignalBlocker>
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
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    auto* title = new QLabel(tr("Cor — Correction (estilo DaVinci)"), this);
    title->setStyleSheet(QStringLiteral("font-weight:bold; color:#e8e8ee;"));
    root->addWidget(title);

    // Três trackballs Lift / Gamma / Gain.
    auto* wheels = new QGroupBox(this);
    auto* gl = new QGridLayout(wheels);
    gl->setSpacing(8);
    m_lift  = new ColorWheel(tr("LIFT  (sombras)"), wheels);
    m_gamma = new ColorWheel(tr("GAMMA (meios)"), wheels);
    m_gain  = new ColorWheel(tr("GAIN  (realces)"), wheels);
    gl->addWidget(m_lift,  0, 0);
    gl->addWidget(m_gamma, 0, 1);
    gl->addWidget(m_gain,  0, 2);
    root->addWidget(wheels, 0);

    // Básico (Lumetri) — sliders de intervalo tonal.
    auto* basic = new QGroupBox(tr("Básico"), this);
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
    addSlider(0, tr("Exposição"),   &m_exposure,   -500, 500, 0);   // x100 EV
    addSlider(1, tr("Contraste"),   &m_contrast,   -100, 100, 0);
    addSlider(2, tr("Saturação"),   &m_saturation, -100, 100, 0);
    addSlider(3, tr("Temperatura"), &m_temperature,-100, 100, 0);
    addSlider(4, tr("Matiz"),       &m_tint,       -100, 100, 0);
    addSlider(5, tr("Intensidade"), &m_blend,          0, 100, 100);
    root->addWidget(basic, 0);

    // Rodas ao vivo → aplica no work e no preview.
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

    for (QSlider* s : {m_exposure, m_contrast, m_saturation,
                       m_temperature, m_tint, m_blend}) {
        connect(s, &QSlider::valueChanged, this, [this](int) {
            if (m_updating) return;
            writeWorkFromWheels();
            applyLive();
        });
    }

    // Botões de ação.
    auto* btns = new QHBoxLayout;
    auto* reset = new QPushButton(tr("Resetar"), this);
    connect(reset, &QPushButton::clicked, this, [this]() {
        if (!m_clip) return;
        pushUndo();
        const QString lut = m_work.cgLutPath;
        m_work = Clip{};
        m_work.cgLutPath = lut;
        readWheelsFromWork();
        applyLive();
    });
    btns->addWidget(reset);
    btns->addStretch(1);
    root->addLayout(btns);

    m_status = new QLabel(tr("Selecione um clipe de vídeo na timeline."), this);
    m_status->setStyleSheet(QStringLiteral("color:#9a9aa4;"));
    root->addWidget(m_status);
    root->addStretch(1);

    setMinimumSize(320, 420);
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
        m_status->setText(tr("Selecione um clipe de vídeo na timeline."));
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
    if (m_timeline) m_timeline->update();
}

void ColorDock::updatePreviewFrame() {
    // O PreviewWidget já recompõe a partir do clipe a cada refreshView();
    // o applyLive() chama m_timeline->update() que dispara essa recomposição.
    if (m_preview) m_preview->refreshView();
}

void ColorDock::pushUndo() {
    if (m_timeline) emit m_timeline->editStart();
}
