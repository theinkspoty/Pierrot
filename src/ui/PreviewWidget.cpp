#include "PreviewWidget.h"

#include "ui/SettingsDialog.h"

#include <QPainter>
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

// Timecode estilo Vegas/DaVinci: HH:MM:SS:FF (frames, no fps do projeto).
QString fmtTimecode(double t, double fps) {
    const int fr = qMax(1, (int)std::llround(fps));
    int ff = (int)std::llround(t * fr);
    const int h = ff / (3600 * fr); ff %= 3600 * fr;
    const int m = ff / (60 * fr);   ff %= 60 * fr;
    const int s = ff / fr;          ff %= fr;
    return QString("%1:%2:%3:%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(ff, 2, 10, QLatin1Char('0'));
}

double projFps(const Project* p) {
    return (p && p->fps > 0.0) ? p->fps : 30.0;
}
}

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    m_playBtn = new QPushButton(tr("Reproduzir"), this);
    m_timeLabel = new QLabel(tr("00:00:00:00"), this);
    m_timeLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setMinimumWidth(120);

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

    // Fundo do console (área ao redor do monitor).
    p.fillRect(m_videoRect, QColor(13, 13, 16));

    if (!m_project || m_project->width <= 0 || m_project->height <= 0) {
        drawEmptyMonitor(p, m_videoRect);
        return;
    }

    const double pw = m_project->width;
    const double ph = m_project->height;

    // Área útil do monitor.
    const QRect work = m_videoRect.adjusted(12, 12, -12, -12);

    // Escala: zoom fixo ou "Ajustar" (o quadro inteiro cabe na área).
    double k = m_zoom > 0.0 ? m_zoom
                            : qMin(work.width() / pw, work.height() / ph);
    QRect canvas(QPoint(0, 0), QSize(qMax(1, (int)(pw * k)), qMax(1, (int)(ph * k))));
    canvas.moveCenter(work.center());
    canvas = canvas.intersected(work); // centraliza e recorta quando zoom > 1

    // Monitor: fundo preto com borda fina (como os viewers de DaVinci/Vegas).
    p.setPen(QPen(QColor(70, 70, 78), 1));
    p.setBrush(QColor(0, 0, 0));
    p.drawRect(canvas.adjusted(-1, -1, 0, 0));
    p.fillRect(canvas, QColor(8, 8, 10));

    // Rótulo de resolução/fps, discreto, no canto do monitor.
    QFont f = p.font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    p.setPen(QColor(175, 175, 185));
    p.drawText(canvas.adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::AlignTop,
                   QStringLiteral("%1 × %2 · %3 fps")
                       .arg(m_project->width)
                       .arg(m_project->height)
                       .arg(m_project->fps));

    if (m_frame.isNull()) {
        drawEmptyMonitor(p, canvas);
        return;
    }

    // Render unificado em "espaço de projeto": o canvas É o quadro do projeto
    // (k = pixels de tela por pixel do projeto). O vídeo é desenhado do mesmo
    // jeito com ou sem transform — sem transform, ele cabe inteiro no quadro;
    // com transform, pan/rot/zoom giram em torno do centro do quadro. Assim,
    // adicionar um keyframe de transform nunca muda o tamanho do vídeo.
    const Clip* clip = clipAt(m_playhead);
    const double S = clip ? kfValue(clip->kfScale, clip->scale, m_playhead - clip->pos) : 1.0;
    const double rot = clip ? kfValue(clip->kfRotation, clip->rotation, m_playhead - clip->pos) : 0.0;
    const double tx = clip ? kfValue(clip->kfTx, clip->tx, m_playhead - clip->pos) : 0.0;
    const double ty = clip ? kfValue(clip->kfTy, clip->ty, m_playhead - clip->pos) : 0.0;

    // "Fit": o vídeo inteiro (já com crop aplicado) cabe no quadro do projeto.
    const double fit = qMin(pw / m_frame.width(), ph / m_frame.height());

    p.save();
    p.setClipRect(canvas);
    p.translate(canvas.center().x() + tx * k, canvas.center().y() + ty * k);
    p.rotate(rot);
    p.scale(k * fit * S, k * fit * S);
    p.translate(-m_frame.width() / 2.0, -m_frame.height() / 2.0);
    p.drawImage(0, 0, m_frame);
    p.restore();
}

// Tela vazia: monitor escuro com mensagem discreta (como no DaVinci/Vegas).
void PreviewWidget::drawEmptyMonitor(QPainter& p, const QRect& canvas) {
    if (canvas.isEmpty()) return;
    QFont f = p.font();
    f.setPointSizeF(9.0);
    p.setFont(f);
    p.setPen(QColor(120, 120, 132));
    p.drawText(canvas.adjusted(8, 8, -8, -8), Qt::AlignCenter,
               tr("Sem clipe de vídeo aqui"));
}

void PreviewWidget::seek(double t) {
    // Snap ao frame do projeto (playhead sempre em frame cheio).
    const double fps = projFps(m_project);
    m_playhead = (fps > 0.0) ? std::round(t * fps) / fps : std::max(0.0, t);
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
    const double fps = projFps(m_project);
    const double dur = m_project->duration();
    // Avança por frames inteiros do projeto (playback determinístico, sem drift).
    double t = m_playStart + m_clock.elapsed() / 1000.0;
    t = std::floor(t * fps + 1e-6) / fps;
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
    m_timeLabel->setText(fmtTimecode(m_playhead, projFps(m_project)));
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
