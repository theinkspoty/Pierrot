// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/SourceMonitorWidget.h"

#include "laartman/FFmpegDecoder.h"
#include "colombina/ffmpeg/ProxyManager.h"

#include <QDoubleSpinBox>
#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPixmap>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {
// Passos do slider. Antes eram 1000 e o tempo saía de volta pro slider, o que
// dava dois bugs de uma vez: o playTick somava dur/1000 a cada 33 ms (playback
// corria dur/33x mais rápido, certo só por volta dos 33 s) e o stepFrame
// reconvergia pro mesmo valor em mídia longa (as setas não faziam nada).
// 100000 mantém a posição em ms e faz o slider arrastar com precisão de 1 ms.
constexpr int kSeekSteps = 100000;

QString fmtTime(double s) {
    if (s < 0.0) s = 0.0;
    const int t = (int)llround(s * 1000.0);
    return QString("%1:%2.%3")
        .arg(t / 60000, 1, 10, QLatin1Char('0'))
        .arg((t / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(t % 1000, 3, 10, QLatin1Char('0'));
}
} // namespace

SourceMonitorWidget::SourceMonitorWidget(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);

    m_title = new QLabel(tr("Source: (nenhuma mídia)"), this);
    m_title->setStyleSheet(QStringLiteral("font-weight:600; padding:2px 4px;"));
    lay->addWidget(m_title);

    m_preview = new QLabel(this);
    m_preview->setMinimumHeight(160);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setStyleSheet(QStringLiteral("background:#000; color:#888;"));
    m_preview->setText(tr("Duplo-clique na Central de Mídias\npara abrir no Source"));
    lay->addWidget(m_preview, /*stretch=*/1);

    m_seek = new QSlider(Qt::Horizontal, this);
    m_seek->setRange(0, kSeekSteps);
    m_seek->setEnabled(false);
    lay->addWidget(m_seek);

    m_timeLbl = new QLabel(this);
    m_timeLbl->setAlignment(Qt::AlignCenter);
    m_timeLbl->setText(QStringLiteral("-- / --"));
    lay->addWidget(m_timeLbl);

    auto* io = new QHBoxLayout;
    m_spIn = new QDoubleSpinBox(this);
    m_spOut = new QDoubleSpinBox(this);
    for (QDoubleSpinBox* s : {m_spIn, m_spOut}) {
        s->setRange(0.0, 0.0);
        s->setDecimals(2);
        s->setSingleStep(0.04);
        s->setSuffix(tr(" s"));
        s->setEnabled(false);
        s->setProperty("pierrotSourceEdit", true);
    }
    auto* btnIn = new QToolButton(this);
    btnIn->setText(tr("In"));
    btnIn->setToolTip(tr("Marcar ponto de entrada (I)"));
    btnIn->setEnabled(false);
    auto* btnOut = new QToolButton(this);
    btnOut->setText(tr("Out"));
    btnOut->setToolTip(tr("Marcar ponto de saída (O)"));
    btnOut->setEnabled(false);
    io->addWidget(m_spIn);
    io->addWidget(btnIn);
    io->addWidget(m_spOut);
    io->addWidget(btnOut);
    lay->addLayout(io);

    m_rangeLbl = new QLabel(this);
    m_rangeLbl->setAlignment(Qt::AlignCenter);
    m_rangeLbl->setText(tr("trecho: —"));
    lay->addWidget(m_rangeLbl);

    auto* tools = new QHBoxLayout;
    m_playBtn = new QToolButton(this);
    m_playBtn->setText(tr("▶"));
    m_playBtn->setToolTip(tr("Tocar/parar o Source (Espaço)"));
    m_playBtn->setEnabled(false);
    auto* prevBtn = new QToolButton(this);
    prevBtn->setText(tr("◀"));
    prevBtn->setToolTip(tr("Quadro anterior"));
    prevBtn->setEnabled(false);
    auto* nextBtn = new QToolButton(this);
    nextBtn->setText(tr("▶|"));
    nextBtn->setToolTip(tr("Próximo quadro"));
    nextBtn->setEnabled(false);

    m_insertBtn = new QToolButton(this);
    m_insertBtn->setText(tr("Insert"));
    m_insertBtn->setToolTip(tr("Inserir o trecho no playhead da timeline, "
                               "empurrando os clipes seguintes (,)"));
    m_insertBtn->setEnabled(false);
    m_overwriteBtn = new QToolButton(this);
    m_overwriteBtn->setText(tr("Overwrite"));
    m_overwriteBtn->setToolTip(tr("Sobrescrever a timeline no playhead com o "
                                  "trecho do Source (.)"));
    m_overwriteBtn->setEnabled(false);

    tools->addWidget(m_playBtn);
    tools->addWidget(prevBtn);
    tools->addWidget(nextBtn);
    tools->addStretch();
    tools->addWidget(m_insertBtn);
    tools->addWidget(m_overwriteBtn);
    lay->addLayout(tools);

    m_decoder = new FFmpegDecoder();
    m_decoder->setHardwareDecodeAllowed(false);

    m_throttle = new QTimer(this);
    m_throttle->setSingleShot(true);
    m_throttle->setInterval(80);
    connect(m_throttle, &QTimer::timeout, this, &SourceMonitorWidget::updatePreview);

    m_playTimer = new QTimer(this);
    m_playTimer->setInterval(33);
    connect(m_playTimer, &QTimer::timeout, this, &SourceMonitorWidget::playTick);

    connect(m_seek, &QSlider::valueChanged, this, &SourceMonitorWidget::seekChanged);
    connect(m_spIn, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { clampInOut(); });
    connect(m_spOut, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double) { clampInOut(); });
    connect(btnIn, &QToolButton::clicked, this, &SourceMonitorWidget::markIn);
    connect(btnOut, &QToolButton::clicked, this, &SourceMonitorWidget::markOut);
    connect(m_playBtn, &QToolButton::clicked, this, &SourceMonitorWidget::togglePlay);
    connect(prevBtn, &QToolButton::clicked, this, [this]() { stepFrame(-1); });
    connect(nextBtn, &QToolButton::clicked, this, [this]() { stepFrame(1); });
    connect(m_insertBtn, &QToolButton::clicked, this, &SourceMonitorWidget::insertRequested);
    connect(m_overwriteBtn, &QToolButton::clicked, this, &SourceMonitorWidget::overwriteRequested);

    // Botões internos habilitam junto com a mídia.
    m_playBtn->setProperty("alwaysEnabled", false);
    setFocusPolicy(Qt::StrongFocus);
}

SourceMonitorWidget::~SourceMonitorWidget() {
    if (m_playTimer) m_playTimer->stop();
    delete m_decoder;
    m_decoder = nullptr;
}

void SourceMonitorWidget::setProject(Project* p) {
    if (m_project == p) return;
    m_project = p;
    // Troca de projeto: o decoder aponta para mídia da sessão anterior.
    if (!m_mediaId.isEmpty()) {
        m_mediaId.clear();
        clearMonitor();
    }
}

void SourceMonitorWidget::clearMonitor() {
    m_playing = false;
    if (m_playTimer) m_playTimer->stop();
    if (m_playBtn) m_playBtn->setText(tr("▶"));
    m_media = MediaItem{};
    m_in = m_out = 0.0;
    m_pos = 0.0;   // senão currentPos() lia a posição da mídia anterior
    m_title->setText(tr("Source: (nenhuma mídia)"));
    m_preview->setPixmap(QPixmap());
    m_preview->setText(tr("Duplo-clique na Central de Mídias\npara abrir no Source"));
    m_seek->setEnabled(false);
    m_spIn->setEnabled(false);
    m_spOut->setEnabled(false);
    m_playBtn->setEnabled(false);
    m_insertBtn->setEnabled(false);
    m_overwriteBtn->setEnabled(false);
    m_timeLbl->setText(QStringLiteral("-- / --"));
    m_rangeLbl->setText(tr("trecho: —"));
    m_spIn->setRange(0.0, 0.0);
    m_spOut->setRange(0.0, 0.0);
}

void SourceMonitorWidget::openDecoderFor(const MediaItem& media) {
    if (m_decoder) {
        // Reabre no arquivo (ou limpa se não houver caminho — geradores).
        if (!media.filePath.isEmpty())
            m_decoder->open(ProxyManager::instance().resolveVideo(media.filePath), -1);
        else
            m_decoder->close();
    }
}

void SourceMonitorWidget::openMedia(const QString& mediaId) {
    if (!m_project) return;
    const MediaItem* m = m_project->findMedia(mediaId);
    if (!m) {
        emit statusMessage(tr("Source: mídia %1 não encontrada.").arg(mediaId));
        return;
    }
    m_media = *m;
    m_mediaId = mediaId;
    m_in = 0.0;
    m_out = qMax(m_media.duration, 0.05);

    openDecoderFor(m_media);

    const double dur = mediaDuration();
    m_spIn->setRange(0.0, dur);
    m_spOut->setRange(0.0, dur);
    m_spIn->setValue(m_in);
    m_spOut->setValue(m_out);
    m_spIn->setEnabled(true);
    m_spOut->setEnabled(true);
    m_seek->setEnabled(true);
    m_playBtn->setEnabled(true);
    m_insertBtn->setEnabled(true);
    m_overwriteBtn->setEnabled(true);
    m_title->setText(tr("Source: %1").arg(m_media.name));
    m_preview->setText(QString());
    setPos(0.0, /*fromUser=*/false);
    updatePreview();
    // Foco no monitor para I/O/Espaço/`,`/`.` funcionarem sem clicar antes.
    setFocus(Qt::OtherFocusReason);
    emit mediaOpened(mediaId);
    emit statusMessage(tr("Source: %1").arg(m_media.name));
}

double SourceMonitorWidget::mediaDuration() const {
    return qMax(m_media.duration, 0.0);
}

double SourceMonitorWidget::currentPos() const {
    if (mediaDuration() <= 0.0) return 0.0;
    return qBound(0.0, m_pos, mediaDuration());
}

// A posição é um double autoritativo; o slider é só reflexo dela. `fromUser`
// evita que a sincronização do slider dispare seekChanged de novo.
void SourceMonitorWidget::setPos(double p, bool fromUser) {
    const double dur = mediaDuration();
    if (dur <= 0.0) {
        m_pos = 0.0;
        return;
    }
    m_pos = qBound(0.0, p, dur);
    m_updatingSeek = true;
    m_seek->setValue((int)llround(m_pos / dur * kSeekSteps));
    m_updatingSeek = false;
    refreshTimeLabels();
    if (fromUser && !m_mediaId.isEmpty()) m_throttle->start();
}

void SourceMonitorWidget::seekChanged(int v) {
    if (m_updatingSeek) return;
    const double dur = mediaDuration();
    if (dur <= 0.0) return;
    setPos((double)v / kSeekSteps * dur, /*fromUser=*/true);
}

void SourceMonitorWidget::refreshTimeLabels() {
    m_timeLbl->setText(QString("%1 / %2")
                           .arg(fmtTime(currentPos()), fmtTime(mediaDuration())));
    m_rangeLbl->setText(tr("trecho: %1 → %2  (%3 s)")
                            .arg(fmtTime(m_in), fmtTime(m_out))
                            .arg(m_out - m_in, 0, 'f', 2));
}

void SourceMonitorWidget::clampInOut() {
    const double dur = mediaDuration();
    double in = qBound(0.0, m_spIn->value(), dur);
    double out = qBound(0.0, m_spOut->value(), dur);
    if (in >= out) out = qMin(dur, in + 0.05);
    m_in = in;
    m_out = out;
    if (m_spIn->value() != in) m_spIn->setValue(in);
    if (m_spOut->value() != out) m_spOut->setValue(out);
    refreshTimeLabels();
}

void SourceMonitorWidget::markIn() {
    if (m_mediaId.isEmpty()) return;
    const double pos = currentPos();
    if (pos >= m_out)
        m_spOut->setValue(qMin(mediaDuration(), pos + 0.1));
    m_spIn->setValue(pos);
}

void SourceMonitorWidget::markOut() {
    if (m_mediaId.isEmpty()) return;
    const double pos = currentPos();
    if (pos <= m_in)
        m_spIn->setValue(qMax(0.0, pos - 0.1));
    m_spOut->setValue(pos);
}

void SourceMonitorWidget::togglePlay() {
    if (m_mediaId.isEmpty()) return;
    m_playing = !m_playing;
    m_playBtn->setText(m_playing ? tr("⏸") : tr("▶"));
    if (m_playing) {
        if (currentPos() >= m_out)
            setPos(m_in, /*fromUser=*/false);
        // O playTick avança pelo TEMPO REAL decorrido, não por um passo fixo.
        m_playClock.start();
        m_playTimer->start();
    } else {
        m_playTimer->stop();
    }
}

void SourceMonitorWidget::playTick() {
    if (mediaDuration() <= 0.0) return;
    const qint64 ms = m_playClock.restart();
    double pos = currentPos() + (ms > 0 ? ms / 1000.0 : 0.0);
    if (pos >= m_out) {
        pos = m_in;
        m_playing = false;
        m_playBtn->setText(tr("▶"));
        m_playTimer->stop();
    }
    setPos(pos, /*fromUser=*/false);
}

void SourceMonitorWidget::stepFrame(int dir) {
    if (m_mediaId.isEmpty()) return;
    m_playing = false;
    m_playTimer->stop();
    m_playBtn->setText(tr("▶"));
    const double fps = m_media.fps > 0.0 ? m_media.fps : 30.0;
    double pos = currentPos() + dir / fps;
    pos = qBound(0.0, pos, mediaDuration());
    setPos(pos, /*fromUser=*/false);
}

void SourceMonitorWidget::updatePreview() {
    QImage img;
    if (m_decoder && !m_mediaId.isEmpty() && !m_media.filePath.isEmpty()) {
        img = m_decoder->frameAt(currentPos(), 640);
    }
    if (!img.isNull()) {
        m_preview->setPixmap(QPixmap::fromImage(img));
        m_preview->setText(QString());
    } else if (!m_mediaId.isEmpty()) {
        m_preview->setPixmap(QPixmap());
        m_preview->setText(m_media.filePath.isEmpty()
                               ? tr("(mídia gerada — sem quadro de arquivo)")
                               : tr("(sem quadro)"));
    }
}

void SourceMonitorWidget::insertRequested() {
    if (m_mediaId.isEmpty()) return;
    if (m_out - m_in <= 1e-4) {
        emit statusMessage(tr("Source: defina In e Out antes de inserir."));
        return;
    }
    emit insertSource(m_mediaId, m_in, m_out);
}

void SourceMonitorWidget::overwriteRequested() {
    if (m_mediaId.isEmpty()) return;
    if (m_out - m_in <= 1e-4) {
        emit statusMessage(tr("Source: defina In e Out antes de sobrescrever."));
        return;
    }
    emit overwriteSource(m_mediaId, m_in, m_out);
}

void SourceMonitorWidget::keyPressEvent(QKeyEvent* e) {
    if (m_mediaId.isEmpty()) { QWidget::keyPressEvent(e); return; }
    // Atalhos do Source (Premiere): I/O, Espaço, setas, `,` e `.`.
    // Ignora se o foco estiver num spinbox editando número.
    const QWidget* fw = QApplication::focusWidget();
    const bool editing = fw && (qobject_cast<const QDoubleSpinBox*>(fw)
                                || fw->property("pierrotSourceEdit").toBool());
    if (editing) { QWidget::keyPressEvent(e); return; }
    if (e->key() == Qt::Key_I) { markIn(); e->accept(); return; }
    if (e->key() == Qt::Key_O) { markOut(); e->accept(); return; }
    if (e->key() == Qt::Key_Space) { togglePlay(); e->accept(); return; }
    if (e->key() == Qt::Key_Left) { stepFrame(-1); e->accept(); return; }
    if (e->key() == Qt::Key_Right) { stepFrame(1); e->accept(); return; }
    if (e->key() == Qt::Key_Comma) { insertRequested(); e->accept(); return; }
    if (e->key() == Qt::Key_Period) { overwriteRequested(); e->accept(); return; }
    QWidget::keyPressEvent(e);
}
