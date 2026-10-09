// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MixerWidget.h"
#include "PreviewWidget.h"
#include "colombina/models/Project.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

// ══════════════════════════════════════════════════════════════════════
// Cores flat estilo filmcraft / Premiere (dark)
// ══════════════════════════════════════════════════════════════════════

namespace MixerTheme {
    static const QColor bgDark{0x1a, 0x1a, 0x1a};    // fundo do painel
    static const QColor stripBg{0x24, 0x24, 0x24};    // fundo do strip (#24)
    static const QColor separator{0x32, 0x32, 0x32};  // linha à direita do strip
    static const QColor textMain{0xd1, 0xd1, 0xd1};
    static const QColor textDim{0x82, 0x82, 0x82};
    static const QColor accent{0x2f, 0x6b, 0xdf};     // destaque do cap quando tocado
    static const QColor capIdle{0xe0, 0xe0, 0xe0};    // cap do fader em repouso
    static const QColor grooveBg{0x10, 0x10, 0x10};   // canal do fader
    static const QColor grooveBorder{0x50, 0x50, 0x50};
    static const QColor capFill{0x2a, 0x2a, 0x2a};
    static const QColor vuBg{0x10, 0x10, 0x10};
    static const QColor meterGreenA{0x57, 0x9f, 0x51}; // topo (faixa verde)
    static const QColor meterGreenB{0x70, 0xdc, 0x5d}; // base
    static const QColor meterYellow{0xf0, 0xf0, 0x4f}; // > -12 dB
    static const QColor meterRed{0xe3, 0x48, 0x50};    // > -3 dB
    static const QColor danger{0xd8, 0x50, 0x3f};      // clip / record
    static const QColor muteOn{0x2d, 0x9d, 0x78};      // M verde
    static const QColor soloOn{0xf0, 0xf0, 0x4f};      // S amarelo
    static const QColor knobRing{0xb0, 0xb0, 0xb0};
    static const QColor darkSeg{0x30, 0x30, 0x30};     // clip light apagada
}

// Lei de taper do fader (filmcraft): mais curso em torno da unidade.
// par (dB, posição 0..1).
static const double kTaperDb[]  = { -96, -60, -40, -30, -20, -12, -6, 0, 6, 15 };
static const double kTaperPos[] = { 0.0, 0.05, 0.14, 0.24, 0.38, 0.52, 0.64, 0.76, 0.88, 1.0 };
static const double kFaderMaxDb = 6.0; // topo do fader (+6 dB = vol 2.0)

static double dbToPos(double db) {
    if (db <= kTaperDb[0]) return 0.0;
    for (int i = 0; i < 9; ++i) {
        if (db <= kTaperDb[i + 1]) {
            const double t = (db - kTaperDb[i]) / (kTaperDb[i + 1] - kTaperDb[i]);
            return kTaperPos[i] + t * (kTaperPos[i + 1] - kTaperPos[i]);
        }
    }
    return 1.0;
}

static double posToDb(double p) {
    p = std::clamp(p, 0.0, 1.0);
    for (int i = 0; i < 9; ++i) {
        if (p <= kTaperPos[i + 1]) {
            const double t = (p - kTaperPos[i]) / (kTaperPos[i + 1] - kTaperPos[i]);
            return kTaperDb[i] + t * (kTaperDb[i + 1] - kTaperDb[i]);
        }
    }
    return kTaperDb[9];
}

static QFont smallFont() {
    QFont f;
    f.setPixelSize(8);
    return f;
}

// Norma dBFS do meter: -60 dB → 0, 0 dB → 1.
static float meterNormDb(float db) {
    return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
}

// ══════════════════════════════════════════════════════════════════════
// Conversões dB
// ══════════════════════════════════════════════════════════════════════

double volToDb(double vol) {
    if (vol < 1e-6) return -96.0;
    return 20.0 * std::log10(vol);
}

double dbToVol(double db) {
    if (db < -90.0) return 0.0;
    return std::pow(10.0, db / 20.0);
}

// ══════════════════════════════════════════════════════════════════════
// VuMeter — barra Premiere (gradiente contínuo + peak hold + clip light)
// ══════════════════════════════════════════════════════════════════════

VuMeter::VuMeter(QWidget* parent) : QWidget(parent) {
    m_peakTimer.start();
}

void VuMeter::setLevel(float rms) {
    float clamped = std::clamp(rms, 0.0f, 1.0f);
    float db = (clamped < 1e-6f) ? -60.0f : 20.0f * std::log10(clamped);
    db = std::max(db, -60.0f);
    if (std::fabs(db - m_db) < 0.05f) return;
    m_db = db;
    if (db >= m_peakDb - 0.02f) {
        m_peakDb = db;
        m_peakTimer.restart();
    } else if (m_peakTimer.elapsed() > 1000) {
        // Decadência do pico: ~6 dB/s (como o filmcraft).
        m_peakDb = std::max(-60.0f, m_peakDb - 6.0f * 0.033f);
    }
    update();
}

void VuMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QRect r = rect();
    const int w = r.width();

    // Clip light (4px no topo).
    QColor clipCol = (m_peakDb >= -0.1f) ? MixerTheme::danger : MixerTheme::darkSeg;
    p.fillRect(QRect(r.x(), r.y(), w, 4), clipCol);

    // Fundo da barra.
    const QRect bar(r.x(), r.y() + 6, w, r.height() - 6);
    p.fillRect(bar, MixerTheme::vuBg);

    // Barra contínua com gradiente (Premiere: verde → amarelo → vermelho).
    const float level = meterNormDb(m_db);
    const float bh = bar.height() * level;
    if (bh > 0.0f) {
        const int n = 30;
        for (int i = 0; i < n; ++i) {
            const float f0 = float(i) / n;
            const float f1 = float(i + 1) / n;
            if (f0 * bar.height() > bh) break;
            const int y1 = bar.bottom() - int(f0 * bar.height());
            const int y0 = bar.bottom() - qMin(int(f1 * bar.height()), int(bh));
            if (y0 >= y1) continue;
            const float dbHere = -60.0f + f1 * 60.0f;
            QColor c;
            if (dbHere > -3.0f) {
                c = MixerTheme::meterRed;
            } else if (dbHere > -12.0f) {
                c = MixerTheme::meterYellow;
            } else {
                const float k = std::clamp(f1 / 0.8f, 0.0f, 1.0f);
                c = QColor::fromRgbF(MixerTheme::meterGreenA.redF() + (MixerTheme::meterGreenB.redF() - MixerTheme::meterGreenA.redF()) * k,
                                     MixerTheme::meterGreenA.greenF() + (MixerTheme::meterGreenB.greenF() - MixerTheme::meterGreenA.greenF()) * k,
                                     MixerTheme::meterGreenA.blueF() + (MixerTheme::meterGreenB.blueF() - MixerTheme::meterGreenA.blueF()) * k);
            }
            p.fillRect(QRect(r.x(), y0, w, y1 - y0), c);
        }
    }

    // Linha de pico (amarela; vermelha quando perto do clip).
    const float peakY = bar.bottom() - bar.height() * meterNormDb(m_peakDb);
    if (m_peakDb > -60.0f) {
        p.setPen(m_peakDb > -0.5f ? MixerTheme::danger : MixerTheme::meterYellow);
        p.drawLine(bar.left(), int(peakY), bar.right(), int(peakY));
    }
}

// ══════════════════════════════════════════════════════════════════════
// MeterScale — escala em dB (0..-54) ao lado do meter
// ══════════════════════════════════════════════════════════════════════

MeterScale::MeterScale(QWidget* parent) : QWidget(parent) {}

void MeterScale::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setFont(smallFont());
    p.setPen(MixerTheme::textDim);

    const int H = height();
    const double top = 6.0;
    const double h = H - top;
    for (int d = 0; d >= -54; d -= 6) {
        const double yy = top + h * (-d / 60.0);
        // Tique (maior a cada 18 dB).
        const int tw = (d % 18 == 0) ? 7 : 4;
        p.drawLine(0, int(yy), tw, int(yy));
        if (d % 18 == 0) {
            p.drawText(QRectF(tw + 1, yy - 5, qMax(0, width() - tw - 1), 10),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(d));
        }
    }
    p.drawText(QRectF(0, 0, width(), 6), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("dB"));
}

// ══════════════════════════════════════════════════════════════════════
// PanKnob — anel com ponteiro (estilo filmcraft), ±135°, L/R
// ══════════════════════════════════════════════════════════════════════

PanKnob::PanKnob(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
}

void PanKnob::setPan(double pan) {
    double clamped = std::clamp(pan, -1.0, 1.0);
    if (std::fabs(clamped - m_pan) < 0.005) return;
    m_pan = clamped;
    update();
}

void PanKnob::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(width() / 2.0, 14.0);
    const double radius = 13.0;

    // Anel.
    const QColor ring = m_dragging ? MixerTheme::accent : MixerTheme::knobRing;
    p.setPen(QPen(ring, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, radius, radius);

    // Ponteiro do centro até a borda. Pan 0 aponta para cima; L → -135°.
    const double ang = m_pan * 135.0 * M_PI / 180.0;
    const QPointF p2(c.x() + radius * std::sin(ang), c.y() - radius * std::cos(ang));
    p.setPen(QPen(ring, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c, p2);

    // Ponto central.
    p.setBrush(MixerTheme::capFill);
    p.setPen(Qt::NoPen);
    p.drawEllipse(c, 2.4, 2.4);

    // Rótulos L / R.
    p.setPen(MixerTheme::textDim);
    p.setFont(smallFont());
    p.drawText(QRectF(0, 26, 16, 12), Qt::AlignCenter, QStringLiteral("L"));
    p.drawText(QRectF(width() - 16, 26, 16, 12), Qt::AlignCenter, QStringLiteral("R"));

    // Valor numérico (percentual), como o filmcraft.
    p.drawText(QRectF(0, 36, width(), 8), Qt::AlignCenter, QString::number(qRound(m_pan * 100)));
}

void PanKnob::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastPos = e->pos();
        emit panTouchedUp();
    }
}

void PanKnob::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) return;
    // Incremental (dx - dy), como o filmcraft.
    const QPoint d = e->pos() - m_lastPos;
    m_lastPos = e->pos();
    const double np = std::clamp(m_pan + (d.x() - d.y()) * 0.01, -1.0, 1.0);
    if (np != m_pan) {
        m_pan = np;
        update();
    }
    emit panChanged(m_pan);
}

void PanKnob::mouseReleaseEvent(QMouseEvent*) {
    if (!m_dragging) return;
    m_dragging = false;
    update();
    emit panReleased();
}

void PanKnob::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = false;
        setPan(0.0);
        emit panChanged(m_pan);
    }
}

void PanKnob::wheelEvent(QWheelEvent* e) {
    const double step = (e->angleDelta().y() > 0) ? 0.05 : -0.05;
    setPan(m_pan + step);
    emit panChanged(m_pan);
}

// ══════════════════════════════════════════════════════════════════════
// FaderSlider — fader de volume estilo Premiere (escala dB + cap)
// API: 0..200 = volume x100 (compatível com o antigo QSlider).
// ══════════════════════════════════════════════════════════════════════

FaderSlider::FaderSlider(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
}

void FaderSlider::setValue(int v) {
    const int nv = std::clamp(v, 0, 200);
    if (nv == m_value) return;
    m_value = nv;
    update();
}

// Posição do groove (constantes espelhando o filmcraft).
// labels à direita em x≈26, tiques em 28..31, groove em 33..37.
static const int kScaleRight = 26;
static const int kTick0 = 28;
static const int kTick1 = 31;
static const int kGrooveX = 33;
static const int kGrooveW = 4;
static const int kGrooveTop = 10;
static const int kGrooveBottomMargin = 10;

double FaderSlider::yPosToDb(double y) const {
    const double top = double(kGrooveTop);
    const double h = double(height()) - top - kGrooveBottomMargin;
    return posToDb((height() - y) / h);
}

double FaderSlider::dbToYPos(double db) const {
    const double top = double(kGrooveTop);
    const double h = double(height()) - top - kGrooveBottomMargin;
    return height() - kGrooveBottomMargin - dbToPos(db) * h;
}

void FaderSlider::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int H = height();

    // Escala dB à esquerda.
    p.setFont(smallFont());
    p.drawText(QRectF(0, 0, kScaleRight, 10), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("dB"),
               nullptr);
    for (double db : { -96.0, -60.0, -40.0, -30.0, -20.0, -12.0, -6.0, 0.0, 6.0, 15.0 }) {
        const double yy = dbToYPos(db);
        p.setPen(MixerTheme::textDim);
        const QString label = (db <= -96.0) ? QStringLiteral("-∞") : QString::number(int(db));
        p.drawText(QRectF(0, yy - 5, kScaleRight, 10), Qt::AlignRight | Qt::AlignVCenter, label);
        p.setPen(MixerTheme::textDim);
        p.drawLine(kTick0, int(yy), kTick1, int(yy));
    }

    // Groove.
    const QRectF track(kGrooveX, kGrooveTop, kGrooveW, H - kGrooveTop - kGrooveBottomMargin);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(MixerTheme::grooveBorder);
    p.setBrush(MixerTheme::grooveBg);
    p.drawRoundedRect(track, 1.5, 1.5);

    // Cap (alça) do fader.
    const double vol = m_value / 100.0;
    const double db = volToDb(vol);
    const double cy = dbToYPos(std::clamp(db, -96.0, kFaderMaxDb));
    const bool active = m_drag || underMouse();
    const QColor col = active ? MixerTheme::accent : MixerTheme::capIdle;
    const QRectF cap(track.center().x() - 9.0, cy - 13.0, 18.0, 26.0);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(col, 2.0));
    p.setBrush(MixerTheme::capFill);
    p.drawRoundedRect(cap, 3.0, 3.0);
    p.drawLine(QPointF(cap.left() + 5.0, cy), QPointF(cap.right() - 5.0, cy));
}

void FaderSlider::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_drag = true;
        // Salta o cap para o cursor (como o Premiere).
        const double db = yPosToDb(e->pos().y());
        const double vol = std::clamp(dbToVol(db), 0.0, 2.0);
        const int nv = qRound(vol * 100.0);
        if (nv != m_value) {
            m_value = nv;
            update();
            emit valueChanged(m_value);
        }
        emit sliderPressed();
        e->accept();
    }
}

void FaderSlider::mouseMoveEvent(QMouseEvent* e) {
    if (!m_drag) return;
    const double db = yPosToDb(e->pos().y());
    const double vol = std::clamp(dbToVol(db), 0.0, 2.0);
    const int nv = qRound(vol * 100.0);
    if (nv != m_value) {
        m_value = nv;
        update();
        emit valueChanged(m_value);
    }
    e->accept();
}

void FaderSlider::mouseReleaseEvent(QMouseEvent* e) {
    if (!m_drag) return;
    m_drag = false;
    update();
    emit sliderReleased();
    e->accept();
}

void FaderSlider::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        // Duplo clique: volta para 0 dB (volume unitário).
        m_drag = false;
        setValue(100);
        emit valueChanged(100);
        emit sliderReleased();
        e->accept();
    }
}

void FaderSlider::wheelEvent(QWheelEvent* e) {
    const double db = volToDb(m_value / 100.0);
    const double step = (e->angleDelta().y() > 0) ? 1.0 : -1.0;
    const double ndb = std::clamp(db + step, -96.0, kFaderMaxDb);
    const int nv = qRound(dbToVol(ndb) * 100.0);
    if (nv != m_value) {
        m_value = nv;
        update();
        emit valueChanged(m_value);
    }
    e->accept();
}

// ══════════════════════════════════════════════════════════════════════
// MixerStrip — canal individual
// ══════════════════════════════════════════════════════════════════════

MixerStrip::MixerStrip(const QString& name, int trackIndex, bool isAudio,
                       bool isMaster, QWidget* parent)
    : QWidget(parent), m_trackIndex(trackIndex), m_isAudio(isAudio),
      m_isMaster(isMaster)
{
    // Fundo flat + separador à direita; botões discretos quando inativos.
    setStyleSheet(QStringLiteral(
        "MixerStrip { background-color: %1; border-right: 1px solid %2; }"
        "QPushButton { background-color: transparent; color: %3;"
        "  border: 1px solid #4a4a4a; border-radius: 2px; font-size: 9px;"
        "  font-weight: bold; padding: 0; }"
    ).arg(MixerTheme::stripBg.name(), MixerTheme::separator.name(),
          MixerTheme::textMain.name()));
    // Faixas: largura do filmcraft (~98px) para áudio/master que têm escala;
    // de vídeo, mais estreitas (sem escala de meter).
    setFixedWidth(isMaster || isAudio ? 100 : 74);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(3, 3, 3, 3);
    lay->setSpacing(2);

    // Nome da faixa.
    auto* nameLabel = new QLabel(isMaster ? tr("Mix") : name);
    nameLabel->setAlignment(Qt::AlignCenter);
    nameLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 8px; font-weight: bold;")
                                .arg(MixerTheme::textMain.name()));
    nameLabel->setMaximumHeight(14);
    nameLabel->setToolTip(name);
    lay->addWidget(nameLabel);

    // Pan knob (só em faixas, não no master).
    if (!isMaster) {
        m_panKnob = new PanKnob;
        m_panKnob->setFixedSize(36, 44);
        m_panKnob->setToolTip(tr("Pan (posição estéreo). Duplo clique: centro."));
        lay->addWidget(m_panKnob, 0, Qt::AlignHCenter);

        connect(m_panKnob, &PanKnob::panChanged, this, [this](double pan) {
            if (m_updating) return;
            emit panChanged(m_trackIndex, m_isAudio, pan);
        });
        // Toque/largada do knob (Touch/Latch de pan).
        connect(m_panKnob, &PanKnob::panTouchedUp, this, [this]() {
            emit panTouched(m_trackIndex, m_isAudio);
        });
        connect(m_panKnob, &PanKnob::panReleased, this, [this]() {
            emit panReleased(m_trackIndex, m_isAudio);
        });
    }

    // Botões M / S / R (só em faixas) — acima do fader, como no filmcraft.
    if (!isMaster) {
        auto* btnRow = new QHBoxLayout;
        btnRow->setSpacing(2);
        m_muteBtn = new QPushButton(QStringLiteral("M"));
        m_soloBtn = new QPushButton(QStringLiteral("S"));
        m_autoBtn = new QPushButton(QStringLiteral("R"));
        const QSize btnSize(22, 18);
        m_muteBtn->setFixedSize(btnSize);
        m_soloBtn->setFixedSize(btnSize);
        m_autoBtn->setFixedSize(btnSize);
        m_muteBtn->setCheckable(true);
        m_soloBtn->setCheckable(true);
        m_muteBtn->setToolTip(tr("Mudo"));
        m_soloBtn->setToolTip(tr("Solo"));
        btnRow->addWidget(m_muteBtn);
        btnRow->addWidget(m_soloBtn);
        btnRow->addWidget(m_autoBtn);
        lay->addLayout(btnRow);

        updateAutoButton();

        connect(m_muteBtn, &QPushButton::toggled, this, [this](bool checked) {
            if (m_updating) return;
            m_muteBtn->setStyleSheet(checked
                ? QStringLiteral("background-color: %1; color: black; border: 1px solid %1;")
                      .arg(MixerTheme::muteOn.name())
                : QString());
            emit muteChanged(m_trackIndex, m_isAudio, checked);
        });
        connect(m_soloBtn, &QPushButton::toggled, this, [this](bool checked) {
            if (m_updating) return;
            m_soloBtn->setStyleSheet(checked
                ? QStringLiteral("background-color: %1; color: black; border: 1px solid %1;")
                      .arg(MixerTheme::soloOn.name())
                : QString());
            emit soloChanged(m_trackIndex, m_isAudio, checked);
        });
        connect(m_autoBtn, &QPushButton::clicked, this, [this]() {
            // Ciclo: desligado → Touch → Write → Latch → desligado.
            if (!m_autoArmed) {
                m_autoArmed = true;
                m_autoMode = 0;
            } else if (m_autoMode == 0) {
                m_autoMode = 1;
            } else if (m_autoMode == 1) {
                m_autoMode = 2;
            } else {
                m_autoArmed = false;
            }
            updateAutoButton();
            emit automationToggled(m_trackIndex, m_isAudio, m_autoArmed, m_autoMode);
        });
    }

    // Fader à esquerda, VU + escala à direita.
    auto* meterFaderRow = new QHBoxLayout;
    meterFaderRow->setSpacing(2);
    meterFaderRow->setContentsMargins(0, 0, 0, 0);

    m_fader = new FaderSlider;
    m_fader->setValue(100);      // 0 dB
    m_fader->setFixedWidth(52);
    meterFaderRow->addWidget(m_fader, 1);

    m_meter = new VuMeter;
    m_meter->setFixedWidth(11);
    meterFaderRow->addWidget(m_meter, 0, Qt::AlignVCenter);

    m_meterScale = (isAudio || isMaster) ? new MeterScale : nullptr;
    if (m_meterScale) {
        m_meterScale->setFixedWidth(24);
        meterFaderRow->addWidget(m_meterScale, 0, Qt::AlignVCenter);
    }

    lay->addLayout(meterFaderRow, 1);

    // Label de volume em dB.
    m_volLabel = new QLabel(QStringLiteral("0.0 dB"));
    m_volLabel->setAlignment(Qt::AlignCenter);
    m_volLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 8px;")
                                  .arg(MixerTheme::textDim.name()));
    m_volLabel->setMaximumHeight(13);
    lay->addWidget(m_volLabel);

    // Conexão do fader.
    connect(m_fader, &FaderSlider::valueChanged, this, [this](int val) {
        if (m_updating) return;
        const double vol = val / 100.0;
        const double db = volToDb(vol);
        m_volLabel->setText(QStringLiteral("%1 dB")
                                .arg(db > -90.0 ? QString::number(db, 'f', 1) : QStringLiteral("-∞")));
        emit volumeChanged(m_trackIndex, m_isAudio, vol);
    });

    // Toque no fader (para automação Touch/Latch).
    connect(m_fader, &FaderSlider::sliderPressed, this, [this]() {
        emit volumeTouched(m_trackIndex, m_isAudio);
    });
    connect(m_fader, &FaderSlider::sliderReleased, this, [this]() {
        emit volumeReleased(m_trackIndex, m_isAudio);
    });

    // Trigger inicial do label.
    const double initDb = volToDb(1.0);
    m_volLabel->setText(QStringLiteral("%1 dB").arg(initDb, 0, 'f', 1));
}

void MixerStrip::setVolume(double vol) {
    QSignalBlocker block(m_fader);
    m_updating = true;
    m_fader->setValue(qRound(std::clamp(vol, 0.0, 2.0) * 100.0));
    const double db = volToDb(vol);
    m_volLabel->setText(QStringLiteral("%1 dB")
                            .arg(db > -90.0 ? QString::number(db, 'f', 1) : QStringLiteral("-∞")));
    m_updating = false;
}

void MixerStrip::setPan(double pan) {
    if (m_panKnob) m_panKnob->setPan(pan);
}

void MixerStrip::setMuted(bool m) {
    if (!m_muteBtn) return;
    QSignalBlocker block(m_muteBtn);
    m_updating = true;
    m_muteBtn->setChecked(m);
    m_muteBtn->setStyleSheet(m
        ? QStringLiteral("background-color: %1; color: black; border: 1px solid %1;")
              .arg(MixerTheme::muteOn.name())
        : QString());
    m_updating = false;
}

void MixerStrip::setSolo(bool s) {
    if (!m_soloBtn) return;
    QSignalBlocker block(m_soloBtn);
    m_updating = true;
    m_soloBtn->setChecked(s);
    m_soloBtn->setStyleSheet(s
        ? QStringLiteral("background-color: %1; color: black; border: 1px solid %1;")
              .arg(MixerTheme::soloOn.name())
        : QString());
    m_updating = false;
}

void MixerStrip::setRmsLevel(float rms) {
    m_meter->setLevel(rms);
}

double MixerStrip::volume() const { return m_fader ? m_fader->value() / 100.0 : 1.0; }
double MixerStrip::pan() const { return m_panKnob ? m_panKnob->value() : 0.0; }

void MixerStrip::updateAutoButton() {
    if (!m_autoBtn) return;
    if (!m_autoArmed) {
        // Read: mostra a curva (sem gravar).
        m_autoBtn->setText(QStringLiteral("R"));
        m_autoBtn->setStyleSheet(QString());
        m_autoBtn->setToolTip(tr("Automação desligada (ler envelope). Clique para gravar."));
        return;
    }
    const char* t = (m_autoMode == 0) ? "T" : (m_autoMode == 1) ? "W" : "L";
    m_autoBtn->setText(QString::fromLatin1(t));
    const QColor c = (m_autoMode == 0) ? MixerTheme::meterYellow
                    : (m_autoMode == 1) ? MixerTheme::danger
                    : MixerTheme::muteOn;
    const QColor textCol = (m_autoMode == 0) ? QColor(Qt::black) : QColor(Qt::white);
    m_autoBtn->setStyleSheet(
        QStringLiteral("background-color: %1; color: %2; border: 1px solid %1; font-weight: bold;")
            .arg(c.name(), textCol.name()));
    const QString desc = (m_autoMode == 0) ? tr("Touch (grava enquanto segura)")
                       : (m_autoMode == 1) ? tr("Write (grava durante a reprodução)")
                       : tr("Latch (começa a gravar ao tocar)");
    m_autoBtn->setToolTip(tr("Modo de automação: %1. Clique para alternar T→W→L→desligado.").arg(desc));
}

void MixerStrip::setAutomationArmed(bool armed, int mode) {
    m_autoArmed = armed;
    m_autoMode = mode;
    updateAutoButton();
}

// ══════════════════════════════════════════════════════════════════════
// MixerWidget — dock principal
// ══════════════════════════════════════════════════════════════════════

MixerWidget::MixerWidget(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QStringLiteral("background-color: %1;").arg(MixerTheme::bgDark.name()));

    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(0);

    m_scrollArea = new QScrollArea;
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { background-color: %1; border: none; }"
        "QScrollBar:horizontal { background: %1; height: 8px; }"
        "QScrollBar::handle:horizontal { background: #555; border-radius: 3px; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }")
        .arg(MixerTheme::bgDark.name()));

    auto* container = new QWidget;
    container->setStyleSheet(QStringLiteral("background-color: %1;").arg(MixerTheme::bgDark.name()));
    m_channelsLayout = new QHBoxLayout(container);
    m_channelsLayout->setContentsMargins(4, 4, 4, 4);
    m_channelsLayout->setSpacing(4);
    m_channelsLayout->addStretch(1);
    m_scrollArea->setWidget(container);
    mainLay->addWidget(m_scrollArea);

    m_levelTimer = new QTimer(this);
    m_levelTimer->setInterval(33);
    connect(m_levelTimer, &QTimer::timeout, this, &MixerWidget::updateLevels);
    m_levelTimer->start();
}

void MixerWidget::setProject(Project* p) {
    m_project = p;
    refresh();
}

void MixerWidget::setPreview(PreviewWidget* pw) {
    m_preview = pw;
}

void MixerWidget::clearStrips() {
    for (auto* s : m_videoStrips) { s->deleteLater(); }
    for (auto* s : m_audioStrips) { s->deleteLater(); }
    m_videoStrips.clear();
    m_audioStrips.clear();
    if (m_masterStrip) { m_masterStrip->deleteLater(); m_masterStrip = nullptr; }
}

void MixerWidget::refresh() {
    clearStrips();
    if (!m_project) return;

    auto connectStrip = [this](MixerStrip* strip) {
        connect(strip, &MixerStrip::volumeChanged, this,
            [this](int idx, bool audio, double vol) {
                if (!m_project) return;
                Track& tr = audio ? m_project->audioTracks[idx]
                                  : m_project->videoTracks[idx];
                tr.volume = vol;
                // Grava automação se a faixa estiver armada (Touch/Latch/Write).
                writeAutoPoint(audio, idx, QStringLiteral("volume"), vol);
                emit modified();
            });
        connect(strip, &MixerStrip::panChanged, this,
            [this](int idx, bool audio, double pan) {
                if (!m_project) return;
                Track& tr = audio ? m_project->audioTracks[idx]
                                  : m_project->videoTracks[idx];
                tr.pan = pan;
                writeAutoPoint(audio, idx, QStringLiteral("pan"), pan);
                emit modified();
            });
        connect(strip, &MixerStrip::volumeTouched, this,
            [this](int idx, bool audio) {
                beginTouch(audio, idx, QStringLiteral("volume"));
            });
        connect(strip, &MixerStrip::volumeReleased, this,
            [this](int idx, bool audio) {
                endTouch(audio, idx, QStringLiteral("volume"));
            });
        connect(strip, &MixerStrip::panTouched, this,
            [this](int idx, bool audio) {
                beginTouch(audio, idx, QStringLiteral("pan"));
            });
        connect(strip, &MixerStrip::panReleased, this,
            [this](int idx, bool audio) {
                endTouch(audio, idx, QStringLiteral("pan"));
            });
        connect(strip, &MixerStrip::automationToggled, this,
            [this](int idx, bool audio, bool armed, int mode) {
                if (!m_project) return;
                Track& tr = audio ? m_project->audioTracks[idx]
                                  : m_project->videoTracks[idx];
                tr.automationArmed = armed;
                tr.automationMode = mode;
            });
        connect(strip, &MixerStrip::muteChanged, this,
            [this](int idx, bool audio, bool muted) {
                if (!m_project) return;
                Track& tr = audio ? m_project->audioTracks[idx]
                                  : m_project->videoTracks[idx];
                tr.muted = muted;
                emit modified();
            });
        connect(strip, &MixerStrip::soloChanged, this,
            [this](int idx, bool audio, bool solo) {
                if (!m_project) return;
                Track& tr = audio ? m_project->audioTracks[idx]
                                  : m_project->videoTracks[idx];
                tr.solo = solo;
                emit modified();
            });
    };

    auto addStrip = [&](MixerStrip* strip, int insertPos) {
        connectStrip(strip);
        m_channelsLayout->insertWidget(insertPos, strip);
    };

    int pos = 0;

    // Faixas de vídeo.
    for (int i = 0; i < m_project->videoTracks.size(); ++i) {
        const Track& t = m_project->videoTracks[i];
        auto* strip = new MixerStrip(t.name, i, false);
        strip->setVolume(t.volume);
        strip->setPan(t.pan);
        strip->setMuted(t.muted);
        strip->setSolo(t.solo);
        strip->setAutomationArmed(t.automationArmed, t.automationMode);
        m_videoStrips.append(strip);
        addStrip(strip, pos++);
    }

    // Faixas de áudio.
    for (int i = 0; i < m_project->audioTracks.size(); ++i) {
        const Track& t = m_project->audioTracks[i];
        auto* strip = new MixerStrip(t.name, i, true);
        strip->setVolume(t.volume);
        strip->setPan(t.pan);
        strip->setMuted(t.muted);
        strip->setSolo(t.solo);
        strip->setAutomationArmed(t.automationArmed, t.automationMode);
        m_audioStrips.append(strip);
        addStrip(strip, pos++);
    }

    // Separador visual.
    auto* sep = new QWidget;
    sep->setFixedWidth(1);
    sep->setAutoFillBackground(true);
    QPalette pal = sep->palette();
    pal.setColor(QPalette::Window, MixerTheme::separator);
    sep->setPalette(pal);
    m_channelsLayout->insertWidget(pos++, sep);

    // Master.
    m_masterStrip = new MixerStrip(QStringLiteral("Master"), -1, false, true);
    m_masterStrip->setVolume(m_project->masterVolume);
    m_masterStrip->setToolTip(tr("Volume geral (master)"));

    connect(m_masterStrip, &MixerStrip::volumeChanged, this,
        [this](int, bool, double vol) {
            if (!m_project) return;
            m_project->masterVolume = vol;
            emit modified();
        });

    m_channelsLayout->insertWidget(pos, m_masterStrip);
}

void MixerWidget::updateLevels() {
    if (!m_preview) return;

    const PreviewWidget::AudioLevels levels = m_preview->audioLevels();

    for (auto* strip : m_videoStrips) {
        const auto key = qMakePair(false, strip->trackIndex());
        strip->setRmsLevel(levels.rms.value(key, 0.0f));
    }
    for (auto* strip : m_audioStrips) {
        const auto key = qMakePair(true, strip->trackIndex());
        strip->setRmsLevel(levels.rms.value(key, 0.0f));
    }
    if (m_masterStrip)
        m_masterStrip->setRmsLevel(levels.masterRms);
}

// ══════════════════════════════════════════════════════════════════════
// Automação gravável (estilo Vegas: Touch / Write / Latch)
// ══════════════════════════════════════════════════════════════════════

Track* MixerWidget::findTrack(bool isAudio, int index) {
    if (!m_project) return nullptr;
    QVector<Track>& tracks = isAudio ? m_project->audioTracks : m_project->videoTracks;
    if (index < 0 || index >= tracks.size()) return nullptr;
    return &tracks[index];
}

void MixerWidget::setPlayhead(double t) {
    m_playhead = t;
    if (!m_playing) return;
    // Durante a reprodução: grava automação em modo Write (contínuo) e em Touch
    // (apenas enquanto o fader/knob está sendo segurado).
    for (auto* strip : m_videoStrips) {
        Track* tr = findTrack(false, strip->trackIndex());
        if (tr && tr->automationArmed) writeAutoPoint(false, strip->trackIndex(), QStringLiteral("volume"), strip->volume());
        if (tr && tr->automationArmed) writeAutoPoint(false, strip->trackIndex(), QStringLiteral("pan"), strip->pan());
    }
    for (auto* strip : m_audioStrips) {
        Track* tr = findTrack(true, strip->trackIndex());
        if (tr && tr->automationArmed) writeAutoPoint(true, strip->trackIndex(), QStringLiteral("volume"), strip->volume());
        if (tr && tr->automationArmed) writeAutoPoint(true, strip->trackIndex(), QStringLiteral("pan"), strip->pan());
    }
}

void MixerWidget::setPlaying(bool playing) {
    m_playing = playing;
}

bool MixerWidget::hasAutomation() const {
    if (!m_project) return false;
    for (const Track& t : m_project->videoTracks)
        if (t.hasAutomation()) return true;
    for (const Track& t : m_project->audioTracks)
        if (t.hasAutomation()) return true;
    return false;
}

void MixerWidget::beginTouch(bool isAudio, int index, const QString& prop) {
    m_touching.insert(qMakePair(qMakePair(isAudio, index), prop));
}

void MixerWidget::endTouch(bool isAudio, int index, const QString& prop) {
    m_touching.remove(qMakePair(qMakePair(isAudio, index), prop));
}

void MixerWidget::writeAutoPoint(bool isAudio, int index, const QString& prop,
                                 double value) {
    Track* tr = findTrack(isAudio, index);
    if (!tr || !tr->automationArmed) return;

    // Modo Touch: só grava enquanto o usuário segura o fader/knob.
    const auto touchKey = qMakePair(qMakePair(isAudio, index), prop);
    if (tr->automationMode == 0 && !m_touching.contains(touchKey))
        return;

    QVector<Keyframe>& keys = (prop == QStringLiteral("volume"))
                                  ? tr->kfVolume : tr->kfPan;
    upsertKeyframe(keys, m_playhead, value);
}
