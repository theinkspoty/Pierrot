// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/SourceMonitorWidget.h"

#include "laartman/FFmpegDecoder.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/generators.h"

#include <QDoubleSpinBox>
#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
QString fmtTime(double s) {
    if (s < 0.0) s = 0.0;
    const int t = (int)llround(s * 1000.0);
    return QString("%1:%2.%3")
        .arg(t / 60000, 1, 10, QLatin1Char('0'))
        .arg((t / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(t % 1000, 3, 10, QLatin1Char('0'));
}
} // namespace

// ── Prévia com letterbox (preserva o aspecto do quadro) ────────────────────

class AspectLabel : public QWidget {
public:
    explicit AspectLabel(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(160);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void setImage(const QImage& img) { m_img = img; m_msg.clear(); update(); }
    void setMessage(const QString& msg) { m_msg = msg; m_img = QImage(); update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0));
        if (!m_img.isNull()) {
            const QSize sz = m_img.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect dst(QPoint((width() - sz.width()) / 2,
                                   (height() - sz.height()) / 2), sz);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(dst, m_img);
        } else if (!m_msg.isEmpty()) {
            p.setPen(QColor(0x88, 0x88, 0x88));
            p.drawText(rect(), Qt::AlignCenter, m_msg);
        }
    }

private:
    QImage m_img;
    QString m_msg;
};

// ── Barra de scrub com In/Out arrastáveis (estilo Source do Premiere) ──────

class SourceScrub : public QWidget {
    Q_OBJECT
public:
    explicit SourceScrub(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(26);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
    }
    void setDuration(double d) { m_dur = d > 0.0 ? d : 1.0; update(); }
    void setPos(double p) { m_pos = qBound(0.0, p, m_dur); update(); }
    void setRange(double i, double o) {
        m_in = qBound(0.0, std::min(i, o), m_dur);
        m_out = qBound(0.0, std::max(i, o), m_dur);
        update();
    }

signals:
    void posChanged(double pos, bool fromUser);
    void rangeChanged(double in, double out);

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int y = height() / 2;
        const QRect track(0, y - 4, width(), 8);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x2A, 0x2C, 0x30));
        p.drawRoundedRect(track, 3, 3);

        const int xIn = secToX(m_in), xOut = secToX(m_out);
        // Região In..Out destacada (o que será inserido/sobrescrito).
        QRect range(QPoint(std::min(xIn, xOut), y - 4),
                    QPoint(std::max(xIn, xOut), y + 4));
        p.setBrush(QColor(70, 130, 210, 170));
        p.drawRoundedRect(range, 3, 3);

        // Playhead.
        const int xp = secToX(m_pos);
        p.setPen(QPen(QColor(0xEC, 0xEC, 0xEC), 2));
        p.drawLine(xp, 2, xp, height() - 2);

        // Alças In/Out (barras claras).
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xF0, 0xF0, 0xF0));
        p.drawRect(QRect(xIn - 1, 3, 3, height() - 6));
        p.drawRect(QRect(xOut - 1, 3, 3, height() - 6));
        p.setBrush(QColor(70, 130, 210));
        p.drawEllipse(QPoint(xIn, y), 4, 4);
        p.drawEllipse(QPoint(xOut, y), 4, 4);
    }

    void mousePressEvent(QMouseEvent* e) override {
        const int x = (int)e->position().x();
        const int xIn = secToX(m_in), xOut = secToX(m_out);
        if (std::abs(x - xIn) <= 6) {
            m_drag = DragIn;
        } else if (std::abs(x - xOut) <= 6) {
            m_drag = DragOut;
        } else if (x > xIn && x < xOut) {
            m_drag = DragBody;
            m_bodyGrab = xToSec(x) - m_in;
        } else {
            m_drag = DragPos;
            m_pos = xToSec(x);
            emit posChanged(m_pos, true);
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        const int x = (int)e->position().x();
        if (m_drag == DragNone) {
            const int xIn = secToX(m_in), xOut = secToX(m_out);
            setCursor((std::abs(x - xIn) <= 6 || std::abs(x - xOut) <= 6)
                          ? Qt::SizeHorCursor : Qt::PointingHandCursor);
            return;
        }
        const double t = xToSec(x);
        if (m_drag == DragIn) {
            m_in = qBound(0.0, std::min(t, m_out - 0.02), m_dur);
            emit rangeChanged(m_in, m_out);
        } else if (m_drag == DragOut) {
            m_out = qBound(m_in + 0.02, std::max(t, m_in + 0.02), m_dur);
            emit rangeChanged(m_in, m_out);
        } else if (m_drag == DragBody) {
            double len = m_out - m_in;
            double newIn = qBound(0.0, t - m_bodyGrab, m_dur - len);
            m_in = newIn;
            m_out = newIn + len;
            emit rangeChanged(m_in, m_out);
        } else if (m_drag == DragPos) {
            m_pos = t;
            emit posChanged(m_pos, true);
        }
        update();
    }

    void mouseReleaseEvent(QMouseEvent*) override { m_drag = DragNone; }

private:
    enum Drag { DragNone, DragIn, DragOut, DragBody, DragPos };
    double xToSec(int x) const {
        const int w = std::max(1, width() - 1);
        return qBound(0.0, (double)x / w * m_dur, m_dur);
    }
    int secToX(double s) const {
        const int w = std::max(1, width() - 1);
        return (int)std::lround(qBound(0.0, s / m_dur, 1.0) * w);
    }

    double m_dur = 1.0, m_pos = 0.0, m_in = 0.0, m_out = 1.0;
    double m_bodyGrab = 0.0;
    Drag m_drag = DragNone;
};

SourceMonitorWidget::SourceMonitorWidget(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(4);

    m_title = new QLabel(tr("Source: (nenhuma mídia)"), this);
    m_title->setStyleSheet(QStringLiteral("font-weight:600; padding:2px 4px;"));
    lay->addWidget(m_title);

    m_preview = new AspectLabel(this);
    m_preview->setMessage(tr("Duplo-clique na Central de Mídias\npara abrir no Source"));
    lay->addWidget(m_preview, /*stretch=*/1);

    m_scrub = new SourceScrub(this);
    m_scrub->setEnabled(false);
    lay->addWidget(m_scrub);

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

    connect(m_scrub, &SourceScrub::posChanged, this, &SourceMonitorWidget::onScrubPos);
    connect(m_scrub, &SourceScrub::rangeChanged, this, &SourceMonitorWidget::onScrubRange);
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
    m_preview->setMessage(tr("Duplo-clique na Central de Mídias\npara abrir no Source"));
    m_scrub->setEnabled(false);
    m_scrub->setRange(0.0, 1.0);
    m_scrub->setPos(0.0);
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
    m_scrub->setEnabled(true);
    m_scrub->setDuration(dur);
    m_scrub->setRange(m_in, m_out);
    m_playBtn->setEnabled(true);
    m_insertBtn->setEnabled(true);
    m_overwriteBtn->setEnabled(true);
    m_title->setText(tr("Source: %1").arg(m_media.name));
    m_preview->setMessage(QString());
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

// A posição é um double autoritativo; a barra de scrub é só reflexo dela.
// `fromUser` evita que a sincronização da barra reemita posChanged.
void SourceMonitorWidget::setPos(double p, bool fromUser) {
    const double dur = mediaDuration();
    if (dur <= 0.0) {
        m_pos = 0.0;
        return;
    }
    m_pos = qBound(0.0, p, dur);
    m_scrub->setPos(m_pos);
    refreshTimeLabels();
    if (fromUser && !m_mediaId.isEmpty()) m_throttle->start();
}

void SourceMonitorWidget::onScrubPos(double p, bool fromUser) {
    setPos(p, fromUser);
}

void SourceMonitorWidget::onScrubRange(double in, double out) {
    // A barra já limitou in<out; espelha nos spinboxes (que revalidam).
    m_spIn->setValue(in);
    m_spOut->setValue(out);
    clampInOut();
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
    m_scrub->setRange(m_in, m_out);
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
    if (m_media.isSolid) {
        // Mídia gerada (gerador estilo Vegas): gera o quadro diretamente —
        // não há arquivo para decodificar. O padrão é determinístico por id.
        const int w = qMax(1, m_media.width > 0 ? m_media.width : 1920);
        const int h = qMax(1, m_media.height > 0 ? m_media.height : 1080);
        img = generatorFrame(m_media, w, h);
    } else if (m_decoder && !m_mediaId.isEmpty() && !m_media.filePath.isEmpty()) {
        img = m_decoder->frameAt(currentPos(), 640);
    }
    if (!img.isNull()) {
        m_preview->setImage(img);
    } else if (!m_mediaId.isEmpty()) {
        m_preview->setMessage(tr("(sem quadro)"));
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

#include "SourceMonitorWidget.moc"
