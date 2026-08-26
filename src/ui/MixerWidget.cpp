// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "MixerWidget.h"
#include "PreviewWidget.h"
#include "models/Project.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QSlider>
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
// Cores do tema Vegas escuro
// ══════════════════════════════════════════════════════════════════════

namespace MixerTheme {
    static const QColor bgDark{26, 26, 26};       // fundo geral
    static const QColor stripBg{34, 34, 34};      // fundo do strip
    static const QColor grooveBg{50, 50, 50};     // canal do fader
    static const QColor thumbColor{140, 140, 140}; // alça do fader
    static const QColor textMain{200, 200, 200};   // texto principal
    static const QColor textDim{120, 120, 120};    // texto secundário
    static const QColor vuBg{18, 18, 18};          // fundo do VU
    static const QColor vuGreen{50, 180, 70};
    static const QColor vuYellow{210, 190, 40};
    static const QColor vuRed{210, 50, 50};
    static const QColor vuPeak{240, 240, 240};
    static const QColor knobBody{70, 70, 70};
    static const QColor knobIndicator{210, 210, 210};
    static const QColor muteActive{200, 50, 50};
    static const QColor soloActive{50, 100, 200};
    static const QColor separator{60, 60, 60};
}

static QString stripSheet() {
    return QStringLiteral(
        "MixerStrip { background-color: %1; border-radius: 3px; }"
        "QSlider::groove:vertical { background: %2; width: 6px; border-radius: 2px; }"
        "QSlider::handle:vertical { background: %3; height: 14px; width: 18px;"
        "   margin: -4px -6px; border-radius: 3px; }"
        "QSlider::handle:vertical:hover { background: #aaaaaa; }"
        "QSlider::sub-page:vertical { background: #4a7ab5; border-radius: 2px; }"
        "QPushButton { background: %2; color: %4; border: 1px solid #444;"
        "   border-radius: 2px; font-size: 9px; font-weight: bold; }"
        "QPushButton:checked { border: 1px solid #666; }"
    ).arg(MixerTheme::stripBg.name(), MixerTheme::grooveBg.name(),
          MixerTheme::thumbColor.name(), MixerTheme::textMain.name());
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
// VuMeter — barra LED com peak hold
// ══════════════════════════════════════════════════════════════════════

VuMeter::VuMeter(QWidget* parent) : QWidget(parent) {
    m_peakTimer.start();
}

void VuMeter::setLevel(float rms) {
    float clamped = std::clamp(rms, 0.0f, 1.0f);
    if (std::fabs(clamped - m_level) < 0.003f) return;
    m_level = clamped;
    if (clamped >= m_peak) {
        m_peak = clamped;
        m_peakTimer.restart();
    } else if (m_peakTimer.elapsed() > 400) {
        // Decay do pico: cai ~6 dB/s (0.02/frame a 30fps).
        m_peak = std::max(0.0f, m_peak - 0.02f);
    }
    update();
}

void VuMeter::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const QRect r = rect().adjusted(1, 1, -1, -1);
    const int h = r.height();
    const int w = r.width();

    // Fundo.
    p.fillRect(r, MixerTheme::vuBg);

    // Segmentos LED (faixas horizontais).
    const int segH = 3;
    const int gap = 1;
    const int totalSeg = h / (segH + gap);
    const int filledSeg = (int)(totalSeg * m_level);

    for (int i = 0; i < totalSeg; ++i) {
        const int y = r.y() + h - (i + 1) * (segH + gap);
        const float frac = (float)i / totalSeg;
        QColor col;
        if (frac < 0.6f)
            col = MixerTheme::vuGreen;
        else if (frac < 0.85f)
            col = MixerTheme::vuYellow;
        else
            col = MixerTheme::vuRed;

        // Escurece segmentos não preenchidos.
        if (i >= filledSeg) {
            col = col.darker(300);
            col.setAlpha(80);
        }
        p.fillRect(QRect(r.x(), y, w, segH), col);
    }

    // Linha de pico (branca, fina).
    const int peakY = r.y() + h - (int)(totalSeg * m_peak) * (segH + gap);
    if (m_peak > 0.01f) {
        p.setPen(MixerTheme::vuPeak);
        p.drawLine(r.x(), peakY, r.x() + w - 1, peakY);
    }
}

// ══════════════════════════════════════════════════════════════════════
// PanKnob — knob rotativo com mouse drag
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
    const int side = std::min(width(), height());
    const int d = side - 4;
    const int x = (width() - d) / 2;
    const int y = (height() - d) / 2;
    const QRect circle(x, y, d, d);
    const QPointF center(width() / 2.0, height() / 2.0);
    const double radius = d / 2.0;

    // Corpo do knob (gradiente radial escuro).
    QRadialGradient grad(center, radius);
    grad.setColorAt(0.0, QColor(80, 80, 80));
    grad.setColorAt(0.8, QColor(55, 55, 55));
    grad.setColorAt(1.0, QColor(40, 40, 40));
    p.setPen(QPen(QColor(90, 90, 90), 1));
    p.setBrush(grad);
    p.drawEllipse(circle);

    // Indicador: linha do centro até a borda, rotacionada pelo pan.
    // Pan -1 = 225° (canto inferior esquerdo), 0 = 270° (baixo), +1 = 315° (canto inferior direito).
    const double angle = (270.0 + m_pan * 45.0) * M_PI / 180.0;
    const double innerR = radius * 0.3;
    const double outerR = radius * 0.78;
    const QPointF p1(center.x() + innerR * std::cos(angle),
                     center.y() + innerR * std::sin(angle));
    const QPointF p2(center.x() + outerR * std::cos(angle),
                     center.y() + outerR * std::sin(angle));
    p.setPen(QPen(MixerTheme::knobIndicator, 2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(p1, p2);

    // Ponto central.
    p.setBrush(MixerTheme::knobIndicator);
    p.drawEllipse(center, 2, 2);
}

void PanKnob::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        // Calcula pan a partir da posição Y relativa ao centro.
        const double cy = height() / 2.0;
        const double dy = -(e->pos().y() - cy) / cy;
        setPan(std::clamp(dy, -1.0, 1.0));
        emit panChanged(m_pan);
    }
}

void PanKnob::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) return;
    const double cy = height() / 2.0;
    const double dy = -(e->pos().y() - cy) / cy;
    setPan(std::clamp(dy, -1.0, 1.0));
    emit panChanged(m_pan);
}

void PanKnob::mouseReleaseEvent(QMouseEvent*) {
    m_dragging = false;
}

void PanKnob::wheelEvent(QWheelEvent* e) {
    const double step = (e->angleDelta().y() > 0) ? 0.05 : -0.05;
    setPan(m_pan + step);
    emit panChanged(m_pan);
}

// ══════════════════════════════════════════════════════════════════════
// MixerStrip — canal individual
// ══════════════════════════════════════════════════════════════════════

MixerStrip::MixerStrip(const QString& name, int trackIndex, bool isAudio,
                       bool isMaster, QWidget* parent)
    : QWidget(parent), m_trackIndex(trackIndex), m_isAudio(isAudio),
      m_isMaster(isMaster)
{
    setStyleSheet(stripSheet());
    setFixedWidth(isMaster ? 72 : 58);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(3, 3, 3, 3);
    lay->setSpacing(2);

    // Nome da faixa.
    auto* nameLabel = new QLabel(isMaster ? tr("MST") : name);
    nameLabel->setAlignment(Qt::AlignCenter);
    nameLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 8px; font-weight: bold;")
                                .arg(MixerTheme::textMain.name()));
    nameLabel->setMaximumHeight(14);
    nameLabel->setToolTip(name);
    lay->addWidget(nameLabel);

    // Pan knob (só em faixas, não no master).
    if (!isMaster) {
        m_panKnob = new PanKnob;
        m_panKnob->setFixedSize(26, 26);
        m_panKnob->setToolTip(tr("Pan (posição estéreo)"));
        lay->addWidget(m_panKnob, 0, Qt::AlignHCenter);

        connect(m_panKnob, &PanKnob::panChanged, this, [this](double pan) {
            if (m_updating) return;
            emit panChanged(m_trackIndex, m_isAudio, pan);
        });
    }

    // VU meter + fader lado a lado.
    auto* meterFaderRow = new QHBoxLayout;
    meterFaderRow->setSpacing(2);
    meterFaderRow->setContentsMargins(0, 0, 0, 0);

    m_meter = new VuMeter;
    m_meter->setFixedWidth(10);
    meterFaderRow->addWidget(m_meter, 0);

    m_fader = new QSlider(Qt::Vertical);
    m_fader->setRange(0, 200);   // 0..200 = 0%..200%
    m_fader->setValue(100);
    m_fader->setTickPosition(QSlider::NoTicks);
    m_fader->setFixedWidth(24);
    meterFaderRow->addWidget(m_fader, 1);

    lay->addLayout(meterFaderRow, 1);

    // Label de volume em dB.
    m_volLabel = new QLabel(QStringLiteral("0.0 dB"));
    m_volLabel->setAlignment(Qt::AlignCenter);
    m_volLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 8px;")
                                  .arg(MixerTheme::textDim.name()));
    m_volLabel->setMaximumHeight(13);
    lay->addWidget(m_volLabel);

    // Botões M / S (só em faixas).
    if (!isMaster) {
        auto* btnRow = new QHBoxLayout;
        btnRow->setSpacing(2);
        m_muteBtn = new QPushButton(QStringLiteral("M"));
        m_soloBtn = new QPushButton(QStringLiteral("S"));
        const QSize btnSize(22, 16);
        m_muteBtn->setFixedSize(btnSize);
        m_soloBtn->setFixedSize(btnSize);
        m_muteBtn->setCheckable(true);
        m_soloBtn->setCheckable(true);
        m_muteBtn->setToolTip(tr("Mudo"));
        m_soloBtn->setToolTip(tr("Solo"));
        btnRow->addWidget(m_muteBtn);
        btnRow->addWidget(m_soloBtn);
        lay->addLayout(btnRow);

        connect(m_muteBtn, &QPushButton::toggled, this, [this](bool checked) {
            if (m_updating) return;
            m_muteBtn->setStyleSheet(checked
                ? QStringLiteral("background-color: %1; color: white; border: 1px solid %1;")
                      .arg(MixerTheme::muteActive.name())
                : QString());
            emit muteChanged(m_trackIndex, m_isAudio, checked);
        });
        connect(m_soloBtn, &QPushButton::toggled, this, [this](bool checked) {
            if (m_updating) return;
            m_soloBtn->setStyleSheet(checked
                ? QStringLiteral("background-color: %1; color: white; border: 1px solid %1;")
                      .arg(MixerTheme::soloActive.name())
                : QString());
            emit soloChanged(m_trackIndex, m_isAudio, checked);
        });
    }

    // Conexão do fader (depois do bloqueio ser seguro).
    connect(m_fader, &QSlider::valueChanged, this, [this](int val) {
        if (m_updating) return;
        const double vol = val / 100.0;
        const double db = volToDb(vol);
        m_volLabel->setText(QStringLiteral("%1 dB")
                                .arg(db > -90.0 ? QString::number(db, 'f', 1) : QStringLiteral("-∞")));
        emit volumeChanged(m_trackIndex, m_isAudio, vol);
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
        ? QStringLiteral("background-color: %1; color: white; border: 1px solid %1;")
              .arg(MixerTheme::muteActive.name())
        : QString());
    m_updating = false;
}

void MixerStrip::setSolo(bool s) {
    if (!m_soloBtn) return;
    QSignalBlocker block(m_soloBtn);
    m_updating = true;
    m_soloBtn->setChecked(s);
    m_soloBtn->setStyleSheet(s
        ? QStringLiteral("background-color: %1; color: white; border: 1px solid %1;")
              .arg(MixerTheme::soloActive.name())
        : QString());
    m_updating = false;
}

void MixerStrip::setRmsLevel(float rms) {
    m_meter->setLevel(rms);
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
                emit modified();
            });
        connect(strip, &MixerStrip::panChanged, this,
            [this](int idx, bool audio, double pan) {
                if (!m_project) return;
                Track& tr = audio ? m_project->audioTracks[idx]
                                  : m_project->videoTracks[idx];
                tr.pan = pan;
                emit modified();
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
