// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/PreviewMonitor.h"

#include "ui/Theme.h"
#include "ui/MonitorUtils.h"

#include <QComboBox>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

const double kZooms[] = {0.0, 0.25, 0.50, 0.75, 1.0, 1.5, 2.0};

} // namespace

PreviewMonitor::PreviewMonitor(QWidget* parent) : QWidget(parent) {
    setWindowTitle(tr("Preview externo — Pierrot"));
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(360, 220);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    m_playIcon = makeMonitorIcon(MonitorGlyph::Play, themeColors().monitorLabel);
    m_pauseIcon = makeMonitorIcon(MonitorGlyph::Pause, themeColors().monitorLabel);

    buildControls();
    applyBarStyle();

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    m_hideTimer->setInterval(2000);
    connect(m_hideTimer, &QTimer::timeout, this, [this]() {
        if (!m_fullScreen) return;
        m_controlsRevealed = false;
        if (m_bar) m_bar->hide();
        updateCursorMode();
        update();
    });
}

void PreviewMonitor::buildControls() {
    const QColor glyph = themeColors().monitorLabel;

    m_bar = new QWidget(this);
    m_bar->setObjectName(QStringLiteral("pmExtBar"));
    m_bar->setFixedHeight(30);
    auto* lay = new QHBoxLayout(m_bar);
    lay->setContentsMargins(6, 2, 6, 2);
    lay->setSpacing(4);

    m_stepBackBtn = new QToolButton(m_bar);
    m_stepBackBtn->setIcon(makeMonitorIcon(MonitorGlyph::StepBack, glyph));
    m_stepBackBtn->setIconSize(QSize(18, 18));
    m_stepBackBtn->setFixedSize(30, 26);
    m_stepBackBtn->setToolTip(tr("Quadro anterior (←)"));
    m_stepBackBtn->setCursor(Qt::PointingHandCursor);
    connect(m_stepBackBtn, &QToolButton::clicked, this, [this]() { emit stepRequested(-1); });
    lay->addWidget(m_stepBackBtn);

    m_playBtn = new QPushButton(m_bar);
    m_playBtn->setIcon(m_playIcon);
    m_playBtn->setIconSize(QSize(20, 20));
    m_playBtn->setFixedSize(36, 26);
    m_playBtn->setToolTip(tr("Reproduzir/pausar (Espaço)"));
    m_playBtn->setCursor(Qt::PointingHandCursor);
    connect(m_playBtn, &QPushButton::clicked, this, [this]() { emit togglePlayRequested(); });
    lay->addWidget(m_playBtn);

    m_stepFwdBtn = new QToolButton(m_bar);
    m_stepFwdBtn->setIcon(makeMonitorIcon(MonitorGlyph::StepFwd, glyph));
    m_stepFwdBtn->setIconSize(QSize(18, 18));
    m_stepFwdBtn->setFixedSize(30, 26);
    m_stepFwdBtn->setToolTip(tr("Quadro seguinte (→)"));
    m_stepFwdBtn->setCursor(Qt::PointingHandCursor);
    connect(m_stepFwdBtn, &QToolButton::clicked, this, [this]() { emit stepRequested(1); });
    lay->addWidget(m_stepFwdBtn);

    m_loopBtn = new QToolButton(m_bar);
    m_loopBtn->setIcon(makeMonitorIcon(MonitorGlyph::Loop, glyph));
    m_loopBtn->setIconSize(QSize(18, 18));
    m_loopBtn->setFixedSize(30, 26);
    m_loopBtn->setCheckable(true);
    m_loopBtn->setToolTip(tr("Loop (repete o trecho in/out)"));
    m_loopBtn->setCursor(Qt::PointingHandCursor);
    connect(m_loopBtn, &QToolButton::toggled, this, &PreviewMonitor::loopToggled);
    lay->addWidget(m_loopBtn);

    lay->addStretch(1);

    m_safeBtn = new QToolButton(m_bar);
    m_safeBtn->setIcon(makeMonitorIcon(MonitorGlyph::Safe, glyph));
    m_safeBtn->setIconSize(QSize(18, 18));
    m_safeBtn->setFixedSize(30, 26);
    m_safeBtn->setCheckable(true);
    m_safeBtn->setToolTip(tr("Margens de segurança (Ctrl+G)"));
    m_safeBtn->setCursor(Qt::PointingHandCursor);
    connect(m_safeBtn, &QToolButton::toggled, this, [this](bool on) {
        m_showSafe = on;
        update();
    });
    lay->addWidget(m_safeBtn);

    m_zoomCombo = new QComboBox(m_bar);
    m_zoomCombo->addItem(tr("Ajustar"));
    for (int p : {25, 50, 75, 100, 150, 200})
        m_zoomCombo->addItem(tr("%1%").arg(p));
    m_zoomCombo->setCurrentIndex(0);
    m_zoomCombo->setToolTip(tr("Zoom do preview"));
    connect(m_zoomCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        m_zoom = kZooms[std::clamp(idx, 0, 6)];
        if (m_zoom <= 0.0) m_pan = QPointF();
        update();
    });
    lay->addWidget(m_zoomCombo);

    m_fullBtn = new QToolButton(m_bar);
    m_fullBtn->setIcon(makeMonitorIcon(MonitorGlyph::Fullscreen, glyph));
    m_fullBtn->setIconSize(QSize(18, 18));
    m_fullBtn->setFixedSize(30, 26);
    m_fullBtn->setToolTip(tr("Tela cheia (F11; Esc sai)"));
    m_fullBtn->setCursor(Qt::PointingHandCursor);
    connect(m_fullBtn, &QToolButton::clicked, this, [this]() { setFullScreen(!m_fullScreen); });
    lay->addWidget(m_fullBtn);
}

void PreviewMonitor::applyBarStyle() {
    if (!m_bar) return;
    QColor hoverBg = themeColors().canvasBorder;
    hoverBg.setAlpha(70);
    QColor checkBg = themeColors().accent;
    checkBg.setAlpha(55);
    m_bar->setStyleSheet(
        QStringLiteral(
            "QWidget#pmExtBar{background:%1;border-top:1px solid %2;}"
            "QToolButton,QPushButton{border:none;border-radius:4px;background:transparent;}"
            "QToolButton:hover,QPushButton:hover{background:%3;}"
            "QToolButton:checked,QPushButton:checked{background:%4;}"
            "QComboBox{background:%5;border:1px solid %2;border-radius:4px;padding:1px 6px;}")
            .arg(themeColors().base.name(), themeColors().trackBorder.name(),
                 hoverBg.name(QColor::HexArgb), checkBg.name(QColor::HexArgb),
                 themeColors().inputBg.name()));
}

void PreviewMonitor::setFrame(const QImage& img) {
    if (img.isNull()) return;
    if (m_frame.constBits() == img.constBits()
        && m_frame.size() == img.size())
        return; // mesmo quadro compartilhado: nada mudou
    m_frame = img;
    update();
}

void PreviewMonitor::clear() {
    if (m_frame.isNull()) return;
    m_frame = QImage();
    update();
}

void PreviewMonitor::setTimecode(double seconds) {
    if (m_time == seconds) return;
    m_time = seconds;
    update(); // o timecode é desenhado sobre o quadro (flutuante)
}

void PreviewMonitor::setFps(double fps) {
    if (fps > 0.0) m_fps = fps;
}

void PreviewMonitor::setPlaying(bool playing) {
    if (m_playing == playing) return;
    m_playing = playing;
    refreshPlayIcon();
}

void PreviewMonitor::setLoopEnabled(bool enabled) {
    if (!m_loopBtn) return;
    QSignalBlocker b(m_loopBtn);
    m_loopBtn->setChecked(enabled);
}

void PreviewMonitor::refreshPlayIcon() {
    if (!m_playBtn) return;
    m_playBtn->setIcon(m_playing ? m_pauseIcon : m_playIcon);
}

void PreviewMonitor::syncZoomCombo() {
    if (!m_zoomCombo) return;
    int best = 0;
    double bestD = 1e9;
    for (int i = 1; i < 7; ++i) {
        const double d = std::abs(kZooms[i] - m_zoom);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (bestD > 0.06) best = 0; // longe de qualquer percentual: mostra "Ajustar"
    QSignalBlocker b(m_zoomCombo);
    m_zoomCombo->setCurrentIndex(best);
}

QRect PreviewMonitor::viewportRect() const {
    QRect r = rect();
    if (m_bar && m_bar->isVisible())
        r.setBottom(r.bottom() - m_bar->height());
    if (r.height() < 1) r.setHeight(1);
    return r;
}

QRect PreviewMonitor::pictureRect() {
    const QRect vp = viewportRect();
    if (m_frame.isNull() || m_frame.width() < 1 || m_frame.height() < 1)
        return QRect();
    const double fit = qMin((double)vp.width() / m_frame.width(),
                            (double)vp.height() / m_frame.height());
    const double sc = m_zoom > 0.0 ? m_zoom : fit;
    const int w = std::max(1, (int)std::lround(m_frame.width() * sc));
    const int h = std::max(1, (int)std::lround(m_frame.height() * sc));
    // Não deixa o quadro sair de cena: o pan fica limitado a metade do
    // excedente em cada eixo (quando a imagem é maior que o viewport).
    const double maxX = std::max(0.0, (w - vp.width()) / 2.0);
    const double maxY = std::max(0.0, (h - vp.height()) / 2.0);
    m_pan.setX(std::clamp(m_pan.x(), -maxX, maxX));
    m_pan.setY(std::clamp(m_pan.y(), -maxY, maxY));
    QRect r(QPoint(0, 0), QSize(w, h));
    r.moveCenter(vp.center() + m_pan.toPoint());
    return r;
}

void PreviewMonitor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const auto& c = themeColors();
    p.fillRect(rect(), c.monitorBg);

    const QRect vp = viewportRect();
    if (m_frame.isNull() || m_frame.width() < 2 || m_frame.height() < 2) {
        p.setPen(c.disabledText);
        p.drawText(vp, Qt::AlignCenter, tr("Sem quadro"));
        return;
    }

    const QRect pic = pictureRect();
    p.save();
    p.setClipRect(vp);
    p.fillRect(pic, c.canvasBg);
    p.drawImage(pic, m_frame);
    p.setPen(QPen(c.canvasBorder, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(pic.adjusted(0, 0, -1, -1));
    if (m_showSafe) drawSafeMargins(p, pic);
    p.restore();

    drawTimecode(p);
}

void PreviewMonitor::drawSafeMargins(QPainter& p, const QRect& pic) {
    const int ax = (int)std::lround(pic.width() * 0.05);
    const int ay = (int)std::lround(pic.height() * 0.05);
    const int tx = (int)std::lround(pic.width() * 0.10);
    const int ty = (int)std::lround(pic.height() * 0.10);
    p.save();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 110), 1.0));
    p.drawRect(pic.adjusted(ax, ay, -ax, -ay));
    p.setPen(QPen(QColor(255, 255, 255, 160), 1.0));
    p.drawRect(pic.adjusted(tx, ty, -tx, -ty));
    // Cruz central.
    const QPoint ctr = pic.center();
    p.drawLine(ctr.x() - 12, ctr.y(), ctr.x() + 12, ctr.y());
    p.drawLine(ctr.x(), ctr.y() - 12, ctr.x(), ctr.y() + 12);
    p.restore();
}

QString PreviewMonitor::timecodeText() const {
    return fmtTimecode(m_time, m_fps);
}

void PreviewMonitor::drawTimecode(QPainter& p) {
    const QRect vp = viewportRect();
    QFont f = font();
    f.setFamily(QStringLiteral("Consolas, Menlo, DejaVu Sans Mono, monospace"));
    f.setPointSize(11);
    f.setBold(true);
    p.setFont(f);
    const QString txt = timecodeText();
    const QFontMetrics fm(f);
    const int tw = fm.horizontalAdvance(txt) + 24;
    const int th = fm.height() + 8;
    const QRect box(vp.left() + 10, vp.bottom() - th - 10, tw, th);
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(themeColors().monitorBg);
    p.drawRoundedRect(box, 3, 3);
    p.setPen(QPen(themeColors().canvasBorder, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(box, 3, 3);
    p.setPen(themeColors().accent);
    p.drawText(box, Qt::AlignCenter, txt);
    p.restore();
}

void PreviewMonitor::resizeEvent(QResizeEvent*) {
    if (m_bar)
        m_bar->setGeometry(0, height() - m_bar->height(), width(), m_bar->height());
}

void PreviewMonitor::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_F11 || e->key() == Qt::Key_F) {
        setFullScreen(!m_fullScreen);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Escape && m_fullScreen) {
        setFullScreen(false);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Space) {
        emit togglePlayRequested();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Left) {
        emit stepRequested(-1);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Right) {
        emit stepRequested(1);
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_G && (e->modifiers() & Qt::ControlModifier)) {
        m_showSafe = !m_showSafe;
        if (m_safeBtn) m_safeBtn->setChecked(m_showSafe);
        update();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void PreviewMonitor::mouseDoubleClickEvent(QMouseEvent* e) {
    setFullScreen(!m_fullScreen);
    e->accept();
}

void PreviewMonitor::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_panning = true;
        m_panLast = e->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void PreviewMonitor::mouseMoveEvent(QMouseEvent* e) {
    if (m_panning) {
        const QPoint p = e->position().toPoint();
        m_pan += QPointF(p - m_panLast);
        m_panLast = p;
        update();
        e->accept();
        return;
    }
    revealControls();
    QWidget::mouseMoveEvent(e);
}

void PreviewMonitor::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && m_panning) {
        m_panning = false;
        updateCursorMode();
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void PreviewMonitor::wheelEvent(QWheelEvent* e) {
    const double steps = e->angleDelta().y() / 120.0;
    if (qFuzzyIsNull(steps)) { e->ignore(); return; }
    const QRect vp = viewportRect();
    const double fit = (m_frame.isNull() || m_frame.width() < 1)
                           ? 1.0
                           : qMin((double)vp.width() / m_frame.width(),
                                  (double)vp.height() / m_frame.height());
    const double cur = m_zoom > 0.0 ? m_zoom : fit;
    m_zoom = std::clamp(cur * std::pow(1.15, steps), 0.05, 8.0);
    syncZoomCombo();
    update();
    e->accept();
}

void PreviewMonitor::enterEvent(QEnterEvent* e) {
    revealControls();
    QWidget::enterEvent(e);
}

void PreviewMonitor::leaveEvent(QEvent* e) {
    if (m_fullScreen) scheduleAutoHide();
    QWidget::leaveEvent(e);
}

void PreviewMonitor::setFullScreen(bool fs) {
    m_fullScreen = fs;
    if (fs)
        showFullScreen();
    else
        showNormal();
    m_controlsRevealed = true;
    if (m_bar) m_bar->setVisible(true);
    scheduleAutoHide();
    updateCursorMode();
    update();
}

void PreviewMonitor::revealControls() {
    if (!m_fullScreen) return;
    if (!m_controlsRevealed) {
        m_controlsRevealed = true;
        updateCursorMode();
        update();
    }
    if (m_bar && !m_bar->isVisible()) m_bar->setVisible(true);
    scheduleAutoHide();
}

void PreviewMonitor::scheduleAutoHide() {
    if (!m_fullScreen) {
        if (m_bar) m_bar->setVisible(true);
        return;
    }
    if (m_hideTimer) m_hideTimer->start();
}

void PreviewMonitor::updateCursorMode() {
    setCursor(m_fullScreen && !m_controlsRevealed ? Qt::BlankCursor
                                                  : Qt::ArrowCursor);
}
