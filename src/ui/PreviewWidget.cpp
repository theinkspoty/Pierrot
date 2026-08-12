#include "PreviewWidget.h"

#include "ui/SettingsDialog.h"

#include <QPainter>
#include <QLinearGradient>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QSignalBlocker>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QIODevice>
#include <QAudioFormat>
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif
#include <algorithm>
#include <cmath>
#include <climits>

static int s_maxDecodeWidth = -1;

int PreviewWidget::maxDecodeWidth() {
    if (s_maxDecodeWidth < 0)
        s_maxDecodeWidth = qMax(320, SettingsDialog::maxDecodeWidth());
    return s_maxDecodeWidth;
}

void PreviewWidget::setMaxDecodeWidth(int w) {
    s_maxDecodeWidth = qMax(320, w);
}

// Decodifica quadros de vídeo na própria thread (com seu FFmpegDecoder).
// O PreviewWidget pede o "último" quadro desejado e descarta intermediários.
class FrameWorker : public QObject {
    Q_OBJECT
public:
    explicit FrameWorker(QObject* parent = nullptr) : QObject(parent) {}
public slots:
    void decodeOne(const QString& path, double t, int maxW) {
        if (!m_decoder.isOpen() || m_decoder.source() != path)
            if (!m_decoder.open(path)) {
                emit frameReady(path, t, maxW, QImage());
                return;
            }
        emit frameReady(path, t, maxW, m_decoder.frameAt(t, maxW));
    }
signals:
    void frameReady(const QString& path, double t, int maxW, const QImage& img);
private:
    FFmpegDecoder m_decoder;
};

// Puxa PCM contínuo do FFmpegDecoder quando o QAudioSink precisa de dados.
class AudioFeed : public QIODevice {
public:
    explicit AudioFeed(FFmpegDecoder* decoder) : m_decoder(decoder) {
        setOpenMode(ReadOnly | Unbuffered);
    }
    qint64 readData(char* data, qint64 maxlen) override {
        if (!m_decoder) return -1;
        return m_decoder->decodeAudio(data, (int)qMin<qint64>(maxlen, INT_MAX));
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    FFmpegDecoder* m_decoder = nullptr;
};

namespace {

QString fmtPreview(double t) {
    int ms = (int)std::llround(t * 1000.0);
    const int h = ms / 3600000; ms %= 3600000;
    const int m = ms / 60000;   ms %= 60000;
    const int s = ms / 1000;    ms %= 1000;
    return QString("%1:%2:%3.%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ms / 100, 2, 10, QLatin1Char('0'));
}

// Passo "redondo" para as réguas (1/2/5×10^n).
double niceStep(double raw) {
    if (raw <= 0) return 1.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double nice;
    if (norm < 1.5)      nice = 1.0;
    else if (norm < 3.0) nice = 2.0;
    else if (norm < 7.0) nice = 5.0;
    else                 nice = 10.0;
    return nice * mag;
}

QString fmtRuler(double v) {
    if (std::fabs(v - std::round(v)) < 1e-6)
        return QString::number((int)std::llround(v));
    return QString::number(v, 'f', 1);
}
}

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    m_playBtn = new QPushButton(tr("Reproduzir"), this);
    m_timeLabel = new QLabel(tr("00:00:00.00"), this);
    m_timeLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setMinimumWidth(110);

    m_topBar = new QWidget(this);
    auto* bar = new QHBoxLayout(m_topBar);
    bar->setContentsMargins(6, 6, 6, 0);
    bar->setSpacing(6);
    bar->addWidget(m_playBtn);
    bar->addWidget(m_timeLabel, 1);

    m_zoomCombo = new QComboBox(this);
    m_zoomCombo->addItem(tr("Ajustar"));
    for (int p : {25, 50, 75, 100, 150, 200})
        m_zoomCombo->addItem(tr("%1%").arg(p));
    m_zoomCombo->setCurrentIndex(0);
    m_zoomCombo->setToolTip(tr("Zoom do preview"));
    connect(m_zoomCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        static const double kZooms[] = {0.0, 0.25, 0.50, 0.75, 1.0, 1.5, 2.0};
        m_zoom = kZooms[qBound(0, idx, 6)];
        update();
    });
    bar->addWidget(m_zoomCombo);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_topBar);
    lay->addStretch(1);

    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, &PreviewWidget::tick);
    connect(m_playBtn, &QPushButton::clicked, this, &PreviewWidget::togglePlay);

    // Thread de vídeo: decodificar quadros aqui tira a decodificação (que é
    // cara em arquivos grandes/4K/MKV) do caminho da UI.
    m_frameThread = new QThread(this);
    m_frameWorker = new FrameWorker;
    m_frameWorker->moveToThread(m_frameThread);
    connect(m_frameThread, &QThread::finished, m_frameWorker, &QObject::deleteLater);
    connect(m_frameWorker, &FrameWorker::frameReady,
            this, &PreviewWidget::onFrameReady, Qt::QueuedConnection);
    m_frameThread->start();

    setMinimumSize(320, 200);
}

PreviewWidget::~PreviewWidget() {
    stopAudio();
    if (m_frameThread) {
        m_frameThread->quit();
        m_frameThread->wait(2000);
    }
}

void PreviewWidget::setProject(Project* p) {
    m_project = p;
    m_playhead = 0.0;
    stopPlayback();
    m_audioSource.clear();
    m_frame = QImage();
    m_frameFull = QImage();
    m_lastSrcT = -1.0;
    m_lastDecodeW = -1;
    m_lastFile.clear();
    m_lastCropL = m_lastCropR = m_lastCropT = m_lastCropB = -1;
    {
        QMutexLocker l(&m_frameMutex);
        m_pendingReq = FrameReq();
        m_shownPath.clear();
        m_shownT = -1.0;
        m_shownW = -1;
    }
    updateFrame();
    update();
}

void PreviewWidget::refreshView() {
    updateFrame();
    update();
}

void PreviewWidget::resizeEvent(QResizeEvent*) {
    const int top = m_topBar ? m_topBar->height() + 4 : 40;
    m_videoRect = QRect(0, top, width(), std::max(0, height() - top));
}

void PreviewWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);

    const int rulerW = 22;
    const int rulerH = 22;

    // Fundo do "console" (área ao redor do monitor).
    p.fillRect(m_videoRect, QColor(13, 13, 16));

    // Área útil dentro da qual o monitor fica, respeitando as réguas.
    const QRect work = m_videoRect.adjusted(rulerW + 10, rulerH + 10, -14, -14);
    QRect canvas;
    if (m_project && m_project->width > 0 && m_project->height > 0) {
        const double scale = m_zoom > 0.0
            ? m_zoom
            : qMin(work.width() / (double)m_project->width,
                   work.height() / (double)m_project->height);
        canvas.setSize(QSize((int)(m_project->width * scale),
                             (int)(m_project->height * scale)));
        canvas.moveCenter(work.center());
        // Não deixa o quadro ultrapassar muito a área: centraliza e recorta.
        if (canvas.left() < work.left()) canvas.moveLeft(work.left());
        if (canvas.top() < work.top()) canvas.moveTop(work.top());
    } else {
        canvas = work;
    }
    m_canvasRect = canvas;

    drawRulers(p, rulerW, rulerH, canvas);

    // Moldura do monitor (bezel) ao redor do quadro.
    const QRect bezel = canvas.adjusted(-9, -9, 9, 9).intersected(m_videoRect);
    QLinearGradient bezelGrad(bezel.topLeft(), bezel.bottomLeft());
    bezelGrad.setColorAt(0.0, QColor(62, 62, 70));
    bezelGrad.setColorAt(0.5, QColor(44, 44, 50));
    bezelGrad.setColorAt(1.0, QColor(34, 34, 40));
    p.setPen(QPen(QColor(12, 12, 15), 1));
    p.setBrush(bezelGrad);
    p.drawRoundedRect(bezel, 8, 8);

    // "Vidro" da tela, atrás do quadro.
    p.setPen(QPen(QColor(6, 6, 8), 1));
    p.setBrush(QColor(15, 15, 19));
    p.drawRect(canvas.adjusted(-1, -1, 0, 0));
    p.fillRect(canvas, QColor(0, 0, 0, 45));

    // Rótulo do monitor com a resolução do projeto (estilo fonte de programa).
    if (m_project && m_project->width > 0 && m_project->height > 0) {
        QFont f = p.font();
        f.setPointSizeF(7.5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(160, 160, 170));
        p.drawText(QRect(bezel.left() + 8, bezel.top() + 5,
                         bezel.width() - 16, 14),
                   Qt::AlignHCenter | Qt::AlignVCenter,
                   QString("%1 × %2 · %3 fps")
                       .arg(m_project->width)
                       .arg(m_project->height)
                       .arg(m_project->fps));
        f.setBold(false);
        p.setFont(f);
    }

    if (m_frame.isNull()) {
        drawEmptyMonitor(p, canvas);
        return;
    }
    const Clip* clip = clipAt(m_playhead);
    const bool hasT = clip && clip->hasTransform();
    if (!hasT) {
        const double scale = qMin(canvas.width() / (double)m_frame.width(),
                                  canvas.height() / (double)m_frame.height());
        const int w = (int)(m_frame.width() * scale);
        const int h = (int)(m_frame.height() * scale);
        const QRect dr(canvas.x() + (canvas.width() - w) / 2,
                       canvas.y() + (canvas.height() - h) / 2, w, h);
        p.drawImage(dr, m_frame);
        return;
    }
    const double S = kfValue(clip->kfScale, clip->scale, m_playhead - clip->pos);
    const double rot = kfValue(clip->kfRotation, clip->rotation, m_playhead - clip->pos);
    const double tx = kfValue(clip->kfTx, clip->tx, m_playhead - clip->pos);
    const double ty = kfValue(clip->kfTy, clip->ty, m_playhead - clip->pos);
    const double fit = qMin(canvas.width() / (double)m_frame.width(),
                            canvas.height() / (double)m_frame.height());
    const double ds = m_project ? (double)canvas.width() / m_project->width : fit;
    p.save();
    p.translate(canvas.center().x() + tx * ds, canvas.center().y() + ty * ds);
    p.rotate(rot);
    p.scale(fit * S, fit * S);
    p.translate(-m_frame.width() / 2.0, -m_frame.height() / 2.0);
    p.drawImage(0, 0, m_frame);
    p.restore();
}

// Tela vazia: placeholder com ícone e dica (como os monitores do DaVinci/Vegas).
void PreviewWidget::drawEmptyMonitor(QPainter& p, const QRect& canvas) {
    const QRect r = canvas.adjusted(4, 4, -4, -4);
    p.setPen(QPen(QColor(72, 72, 82), 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 6, 6);

    // Ícone de clipe de filme.
    const QRect ic(canvas.center().x() - 22, canvas.center().y() - 46, 44, 30);
    QPen pen(QColor(100, 100, 115), 1.6);
    p.setPen(pen);
    p.setBrush(QColor(38, 38, 46));
    p.drawRoundedRect(ic, 3, 3);
    p.setPen(QPen(QColor(100, 100, 115), 1.2));
    const int holeW = 6;
    const int holeH = 4;
    for (int i = 0; i < 4; ++i) {
        const int hx = ic.left() + 3 + i * (holeW + 2);
        p.drawRect(QRect(hx, ic.top() + 3, holeW, holeH));
        p.drawRect(QRect(hx, ic.bottom() - 3 - holeH, holeW, holeH));
    }
    p.drawRect(QRect(ic.left() + 12, ic.top() + 10, 20, 10));

    QFont f = p.font();
    f.setBold(true);
    f.setPointSizeF(9.0);
    p.setFont(f);
    p.setPen(QColor(150, 150, 162));
    p.drawText(canvas.adjusted(0, 40, 0, -20), Qt::AlignHCenter | Qt::AlignVCenter,
               tr("Nenhum clipe de vídeo aqui"));
    f.setBold(false);
    f.setPointSizeF(7.5);
    p.setFont(f);
    p.setPen(QColor(95, 95, 108));
    p.drawText(canvas.adjusted(0, 64, 0, -20), Qt::AlignHCenter | Qt::AlignVCenter,
               tr("Importe mídia e arraste para a timeline"));
}

void PreviewWidget::drawRulers(QPainter& p, int rulerW, int rulerH, const QRect& canvas) {
    const QRect vr = m_videoRect;
    p.fillRect(QRect(vr.x(), vr.y(), vr.width(), rulerH), QColor(31, 31, 35));
    p.fillRect(QRect(vr.x(), vr.y(), rulerW, vr.height()), QColor(31, 31, 35));
    p.setPen(QColor(70, 70, 78));
    p.drawLine(vr.x(), vr.y() + rulerH, vr.x() + vr.width(), vr.y() + rulerH);
    p.drawLine(vr.x() + rulerW, vr.y(), vr.x() + rulerW, vr.y() + vr.height());

    if (!m_project || m_project->width <= 0 || m_project->height <= 0) return;

    QFont f = p.font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    p.setPen(QColor(140, 140, 150));

    // régua horizontal: coordenada x em pixels do projeto
    const double stepX = niceStep(50.0 * canvas.width() / m_project->width);
    for (double v = 0; v <= m_project->width + 1e-9; v += stepX) {
        const int sx = canvas.x() + (int)(v / m_project->width * canvas.width());
        p.drawLine(sx, vr.y() + rulerH - 5, sx, vr.y() + rulerH);
        p.drawText(QRect(sx - 40, vr.y() + 1, 80, rulerH - 5),
                   Qt::AlignHCenter | Qt::AlignTop, fmtRuler(v));
    }

    // régua vertical: coordenada y em pixels do projeto
    const double stepY = niceStep(50.0 * canvas.height() / m_project->height);
    for (double v = 0; v <= m_project->height + 1e-9; v += stepY) {
        const int sy = canvas.y() + (int)(v / m_project->height * canvas.height());
        p.drawLine(vr.x() + rulerW - 5, sy, vr.x() + rulerW, sy);
        p.save();
        p.translate(vr.x() + rulerW - 6, sy);
        p.rotate(-90);
        p.drawText(QRect(-24, -8, 48, 16), Qt::AlignCenter, fmtRuler(v));
        p.restore();
    }
}

void PreviewWidget::seek(double t) {
    m_playhead = std::max(0.0, t);
    updateFrame();
    update();
    emit playheadMoved(m_playhead);
}

void PreviewWidget::togglePlay() {
    if (m_playing) {
        stopPlayback();
        return;
    }
    if (!m_project || m_project->duration() <= 0) return;
    if (m_playhead >= m_project->duration() - 1e-6) m_playhead = 0.0;
    if (m_loopOut > m_loopIn) {
        if (m_playhead < m_loopIn || m_playhead >= m_loopOut)
            m_playhead = m_loopIn;
    }
    m_playStart = m_playhead;
    m_clock.start();
    m_playing = true;
    m_timer->start();
    m_playBtn->setText(tr("Pausar"));
    startAudio(m_playhead);
    emit stateChanged(true);
}

void PreviewWidget::setLoopRange(double in, double out) {
    m_loopIn = in;
    m_loopOut = out;
}

void PreviewWidget::setZoom(double z) {
    m_zoom = z;
    if (m_zoomCombo) {
        static const double kZooms[] = {0.0, 0.25, 0.50, 0.75, 1.0, 1.5, 2.0};
        int idx = 0;
        for (int i = 0; i < 7; ++i) {
            if (std::fabs(kZooms[i] - z) < 1e-6) { idx = i; break; }
        }
        QSignalBlocker b(m_zoomCombo);
        m_zoomCombo->setCurrentIndex(idx);
    }
    update();
}

void PreviewWidget::stopPlayback() {
    m_playing = false;
    m_timer->stop();
    m_playBtn->setText(tr("Reproduzir"));
    stopAudio();
    emit stateChanged(false);
}

void PreviewWidget::tick() {
    if (!m_project) { stopPlayback(); return; }
    const double t = m_playStart + m_clock.elapsed() / 1000.0;
    const double dur = m_project->duration();
    if (m_loopOut > m_loopIn && t >= m_loopOut - 1e-9) {
        m_playStart = m_loopIn;
        m_clock.restart();
        seek(m_loopIn);
        stopAudio();
        startAudio(m_loopIn);
        return;
    }
    if (t >= dur) {
        seek(dur);
        stopPlayback();
        return;
    }
    seek(t);

    // Troca de clipe com áudio (ou saída para um intervalo vazio).
    const Clip* ac = audioClipAt(t);
    const MediaItem* m = ac ? m_project->findMedia(ac->mediaId) : nullptr;
    const QString src = (m && m->hasAudio) ? m->filePath : QString();
    if (src != m_audioSource) {
        stopAudio();
        if (!src.isEmpty()) startAudio(ac->in + (t - ac->pos));
    }
}

const Clip* PreviewWidget::clipAt(double t) const {
    if (!m_project) return nullptr;
    for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr)
        for (const Clip& c : m_project->videoTracks[tr].clips)
            if (t >= c.pos && t < c.pos + c.dur) return &c;
    return nullptr;
}

// Clip de áudio "ativo": o mais acima (vídeo ou áudio) em `t` cujo media tem áudio.
const Clip* PreviewWidget::audioClipAt(double t) const {
    if (!m_project) return nullptr;
    const Clip* found = nullptr;
    for (const Track& tr : m_project->videoTracks)
        for (const Clip& c : tr.clips)
            if (t >= c.pos && t < c.pos + c.dur) {
                const MediaItem* m = m_project->findMedia(c.mediaId);
                if (m && m->hasAudio) found = &c;
            }
    for (const Track& tr : m_project->audioTracks)
        for (const Clip& c : tr.clips)
            if (t >= c.pos && t < c.pos + c.dur) {
                const MediaItem* m = m_project->findMedia(c.mediaId);
                if (m && m->hasAudio) found = &c;
            }
    return found;
}

void PreviewWidget::startAudio(double t) {
    if (!m_project) return;
    const Clip* ac = audioClipAt(t);
    const MediaItem* m = ac ? m_project->findMedia(ac->mediaId) : nullptr;
    if (!m || !m->hasAudio) return;

    if (!m_decoder.isOpen() || m_decoder.source() != m->filePath)
        if (!m_decoder.open(m->filePath)) return;

    m_decoder.seekAudio(ac->in + (t - ac->pos));

    stopAudio();

    const int rate = m_decoder.audioSampleRate();
    const int ch = m_decoder.audioChannels();
    QAudioFormat fmt;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    fmt.setSampleFormat(QAudioFormat::Int16);
#else
    fmt.setCodec(QStringLiteral("audio/pcm"));
    fmt.setSampleSize(16);
    fmt.setSampleType(QAudioFormat::SignedInt);
    fmt.setByteOrder(QAudioFormat::LittleEndian);
#endif
    fmt.setSampleRate(rate);
    fmt.setChannelCount(ch);

    m_audioFeed = new AudioFeed(&m_decoder);
    m_audioFeed->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    m_audioSink = new QAudioSink(QMediaDevices::defaultAudioOutput(), fmt, this);
    m_audioSink->start(m_audioFeed);
#else
    m_audioOut = new QAudioOutput(QAudioDeviceInfo::defaultOutputDevice(), fmt, this);
    m_audioOut->start(m_audioFeed);
#endif
    m_audioSource = m->filePath;
}

void PreviewWidget::stopAudio() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (m_audioSink) {
        m_audioSink->stop();
        m_audioSink->deleteLater();
        m_audioSink = nullptr;
    }
#else
    if (m_audioOut) {
        m_audioOut->stop();
        m_audioOut->deleteLater();
        m_audioOut = nullptr;
    }
#endif
    if (m_audioFeed) {
        m_audioFeed->deleteLater();
        m_audioFeed = nullptr;
    }
    m_audioSource.clear();
}

void PreviewWidget::updateFrame() {
    m_timeLabel->setText(fmtPreview(m_playhead));
    if (!m_project) { m_frame = QImage(); update(); return; }

    const Clip* clip = clipAt(m_playhead);
    if (!clip) {
        m_frame = QImage();
        update();
        return;
    }

    const MediaItem* m = m_project->findMedia(clip->mediaId);
    if (!m || !m->hasVideo) { m_frame = QImage(); update(); return; }

    const double srcT = clip->in + (m_playhead - clip->pos);
    // Decodifica no tamanho de exibição: muito mais rápido que 4K/1080p.
    const int decW = qMax(320, qMin(PreviewWidget::maxDecodeWidth(), m_videoRect.width() > 0
                                    ? m_videoRect.width() : 960));
    const int cL = (int)std::lround(
        std::clamp(kfValue(clip->kfCropL, clip->cropL, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    const int cR = (int)std::lround(
        std::clamp(kfValue(clip->kfCropR, clip->cropR, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    const int cT = (int)std::lround(
        std::clamp(kfValue(clip->kfCropT, clip->cropT, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    const int cB = (int)std::lround(
        std::clamp(kfValue(clip->kfCropB, clip->cropB, m_playhead - clip->pos), 0.0, 0.9) * 1000.0);
    m_lastCropL = cL;
    m_lastCropR = cR;
    m_lastCropT = cT;
    m_lastCropB = cB;

    // O quadro do tamanho/posição atuais já está pronto? Apenas reaplica o
    // pan/crop (por exemplo quando só o corte mudou) sem decodificar de novo.
    {
        QMutexLocker l(&m_frameMutex);
        if (m_shownPath == m->filePath && std::fabs(m_shownT - srcT) < 1e-6
            && m_shownW == decW) {
            applyCrop();
            update();
            return;
        }
    }
    requestFrame(m->filePath, srcT, decW);
}

// Pedido "assíncrono": a decodificação acontece na thread do FrameWorker.
// Vários pedidos seguidos mantêm apenas o mais novo (scrub não empilha).
void PreviewWidget::requestFrame(const QString& path, double t, int maxW) {
    QMutexLocker l(&m_frameMutex);
    if (m_pendingReq.valid && m_pendingReq.path == path
        && std::fabs(m_pendingReq.t - t) < 1e-6 && m_pendingReq.maxW == maxW)
        return; // já enfileirado
    if (m_shownPath == path && std::fabs(m_shownT - t) < 1e-6 && m_shownW == maxW)
        return; // já exibido
    m_pendingReq = {path, t, maxW, true};
    kickFrameWorker();
}

// Chamado com m_frameMutex segura do.
void PreviewWidget::kickFrameWorker() {
    if (m_workerBusy || !m_pendingReq.valid || !m_frameWorker) return;
    m_workerBusy = true;
    const FrameReq r = m_pendingReq;
    m_pendingReq.valid = false;
    QMetaObject::invokeMethod(m_frameWorker, "decodeOne", Qt::QueuedConnection,
                              Q_ARG(QString, r.path), Q_ARG(double, r.t), Q_ARG(int, r.maxW));
}

void PreviewWidget::onFrameReady(const QString& path, double t, int maxW, const QImage& img) {
    {
        QMutexLocker l(&m_frameMutex);
        m_workerBusy = false;
        kickFrameWorker(); // continua com o pedido mais novo, se houver
    }
    if (img.isNull() || !m_project) return;

    const Clip* clip = clipAt(m_playhead);
    if (!clip) return;
    const MediaItem* m = m_project->findMedia(clip->mediaId);
    if (!m || m->filePath != path) return;

    // Ignora quadros decodificados para outra posição (scrub/seek rápido).
    const double wantT = clip->in + (m_playhead - clip->pos);
    if (std::fabs(wantT - t) > 0.4) return;

    {
        QMutexLocker l(&m_frameMutex);
        m_shownPath = path;
        m_shownT = t;
        m_shownW = maxW;
    }
    m_frameFull = img;
    m_lastSrcT = t;
    m_lastDecodeW = maxW;
    m_lastFile = path;
    applyCrop();
    update();
}

// Aplica pan/crop sobre o quadro cheio (m_frameFull) e guarda em m_frame.
void PreviewWidget::applyCrop() {
    m_frame = m_frameFull;
    const double w = m_frameFull.width();
    const double h = m_frameFull.height();
    if (w > 1 && h > 1 && (m_lastCropL || m_lastCropR || m_lastCropT || m_lastCropB)) {
        const int x = (int)std::lround(w * m_lastCropL / 1000.0);
        const int y = (int)std::lround(h * m_lastCropT / 1000.0);
        const int cw = (int)std::lround(w * (1.0 - (m_lastCropL + m_lastCropR) / 1000.0));
        const int ch = (int)std::lround(h * (1.0 - (m_lastCropT + m_lastCropB) / 1000.0));
        m_frame = m_frameFull.copy(
            QRect(std::clamp(x, 0, (int)w - 1), std::clamp(y, 0, (int)h - 1),
                  qMin(cw, (int)w - std::clamp(x, 0, (int)w - 1)),
                  qMin(ch, (int)h - std::clamp(y, 0, (int)h - 1))));
    }
}

#include "PreviewWidget.moc"
