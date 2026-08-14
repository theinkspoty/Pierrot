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
#include <cstring>
#include <QHash>
#include <QMutex>
#include <QDebug>

static int s_maxDecodeWidth = -1;

// Diagnóstico do caminho de áudio do preview: ligue com PIERROT_AUDIO_DEBUG=1.
static bool audioDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_AUDIO_DEBUG");
    return on;
}

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

// Mixer de áudio: soma o PCM de todos os clipes ativos em `t` (clipe de vídeo
// + faixas de áudio), cada um com volume próprio (clipe, envelope e faixa).
// Todos os FFmpegDecoder resampleiam para S16/48 kHz/estéreo, então misturar
// é alinhar amostra a amostra. A thread do QAudioSink chama readData(); a UI
// chama updateSources() conforme o playhead avança.
class AudioMixer : public QIODevice {
public:
    struct SourceInfo {
        QString key;      // id do clipe (estável durante a reprodução)
        QString path;     // arquivo de mídia
        double mediaPos;  // posição no arquivo de mídia (em segundos)
        double vol = 1.0;
    };

    explicit AudioMixer(QObject* parent = nullptr) : QIODevice(parent) {
        setOpenMode(ReadOnly | Unbuffered);
    }
    ~AudioMixer() override {
        QMutexLocker l(&m_mutex);
        for (Source* s : m_sources) { s->dec.close(); delete s; }
        m_sources.clear();
    }

    // atualiza o conjunto de fontes ativas para o playhead atual.
    // reseek=true: reposiciona as fontes existentes (loop, salto de playhead).
    void updateSources(const QVector<SourceInfo>& want, bool reseek) {
        QMutexLocker l(&m_mutex);
        for (const SourceInfo& w : want) {
            Source* s = findLocked(w.key);
            if (!s) {
                s = new Source;
                if (!s->dec.open(w.path)) {
                    if (audioDbg()) qDebug() << "[audio] updateSources: FALHOU ao abrir" << w.path;
                    delete s; continue;
                }
                s->key = w.key;
                s->dec.seekAudio(w.mediaPos);
                m_sources.append(s);
                if (audioDbg()) qDebug() << "[audio] updateSources: +fonte" << w.path << "mediaPos=" << w.mediaPos;
            }
            s->vol = w.vol;
            s->active = true;
            if (reseek) s->dec.seekAudio(w.mediaPos);
        }
        // Desativa (e libera) fontes que saíram do intervalo do clipe.
        for (int i = 0; i < m_sources.size(); ++i) {
            Source* s = m_sources[i];
            if (s->active && !usedLocked(s->key, want)) {
                if (audioDbg()) qDebug() << "[audio] updateSources: -fonte" << s->key;
                s->dec.close();
                s->active = false;
            }
        }
        m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(),
                                       [](Source* s) { return !s->active; }),
                        m_sources.end());
    }

    qint64 readData(char* data, qint64 maxlen) override {
        QMutexLocker l(&m_mutex);
        const int ch = kChannels;
        const int bytesPerSample = 2 * ch; // S16 interleaved
        const int maxBytes = (int)qMin<qint64>(maxlen, INT_MAX);
        const int capacity = (maxBytes / bytesPerSample) * bytesPerSample;
        memset(data, 0, capacity);

        if (m_sources.isEmpty() || capacity <= 0) {
            if (audioDbg()) qDebug() << "[audio] readData: sem fontes (silêncio)";
            return capacity;
        }

        QVector<int16_t> tmp;
        tmp.resize(capacity / 2); // um sample por amostra (ch compensado abaixo)
        int16_t* out = reinterpret_cast<int16_t*>(data);
        for (Source* s : m_sources) {
            const int got = s->dec.decodeAudio(tmp.data(), capacity);
            if (audioDbg() && got == 0)
                qDebug() << "[audio] readData: decodeAudio retornou 0 (silêncio)";
            const int n = got / bytesPerSample;
            const int16_t* src = tmp.constData();
            for (int i = 0; i < n * ch; ++i) {
                const float sum = out[i] / 32768.0f + src[i] / 32768.0f * (float)s->vol;
                const float cl = qBound(-1.0f, sum, 1.0f);
                out[i] = (int16_t)std::lround(cl * 32768.0f);
            }
        }
        return capacity;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
    qint64 bytesAvailable() const override { return 4096; }

private:
    static constexpr int kChannels = 2;
    struct Source {
        FFmpegDecoder dec;
        QString key;
        double vol = 1.0;
        bool active = false;
    };
    Source* findLocked(const QString& key) const {
        for (Source* s : m_sources) if (s->key == key) return s;
        return nullptr;
    }
    static bool usedLocked(const QString& key, const QVector<SourceInfo>& want) {
        for (const SourceInfo& w : want) if (w.key == key) return true;
        return false;
    }
    QVector<Source*> m_sources;
    QMutex m_mutex;
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
        updateMixAudio(m_loopIn, true);
        return;
    }
    if (t >= dur) {
        seek(dur);
        stopPlayback();
        return;
    }
    seek(t);
    // Mixer acompanha o playhead: volumes/fades e troca de clipes acontecem
    // aqui, sem reiniciar o sink a cada transição.
    updateMixAudio(t, false);
}

const Clip* PreviewWidget::clipAt(double t) const {
    if (!m_project) return nullptr;
    for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr)
        for (const Clip& c : m_project->videoTracks[tr].clips)
            if (t >= c.pos && t < c.pos + c.dur) return &c;
    return nullptr;
}

// Clip de áudio "ativo": o mais acima (vídeo ou áudio) em `t` cujo media tem áudio.
// Clipes de áudio ativos em `t`, prontos para o mixer. Dedupe de vídeo+áudio
// vinculados (mesmo groupId): a faixa de áudio vence, senão o som sobraria
// duas vezes. Respeita mute/solo das faixas, volume de faixa, volume/envelope
// do clipe e fades.
QVector<AudioMixer::SourceInfo> buildMixSources(const Project* p, double t) {
    QVector<AudioMixer::SourceInfo> out;
    if (!p) return out;
    bool anySolo = false;
    for (const Track& tr : p->videoTracks)
        if (tr.solo) { anySolo = true; break; }
    if (!anySolo)
        for (const Track& tr : p->audioTracks)
            if (tr.solo) { anySolo = true; break; }

    struct Rep { const Clip* clip; double vol; };
    QHash<QString, Rep> reps;
    auto collect = [&](const QVector<Track>& tracks) {
        for (const Track& tr : tracks) {
            for (const Clip& c : tr.clips) {
                if (!(t >= c.pos && t < c.pos + c.dur)) continue;
                if (tr.muted || (anySolo && !tr.solo)) continue;
                const MediaItem* m = p->findMedia(c.mediaId);
                if (!m || !m->hasAudio) continue;
                const double rel = t - c.pos;
                double vol = c.volume * kfValue(c.kfVolume, 1.0, rel) * tr.volume;
                if (c.fadeIn > 1e-6) vol *= std::min(1.0, rel / c.fadeIn);
                if (c.fadeOut > 1e-6) vol *= std::min(1.0, (c.dur - rel) / c.fadeOut);
                vol = std::clamp(vol, 0.0, 2.0);
                const QString key = c.groupId.isEmpty() ? c.id : c.groupId;
                reps.insert(key, {&c, vol});
            }
        }
    };
    collect(p->videoTracks);
    collect(p->audioTracks);

    for (auto it = reps.cbegin(); it != reps.cend(); ++it) {
        const Clip* c = it.value().clip;
        const MediaItem* m = p->findMedia(c->mediaId);
        if (!m || !m->hasAudio) continue;
        AudioMixer::SourceInfo si;
        si.key = c->id;
        si.path = m->filePath;
        si.mediaPos = c->in + (t - c->pos);
        si.vol = it.value().vol;
        out.append(si);
    }
    return out;
}

void PreviewWidget::startAudio(double t) {
    if (!m_project) return;
    stopAudio();
    const QVector<AudioMixer::SourceInfo> sources = buildMixSources(m_project, t);
    if (sources.isEmpty()) {
        if (audioDbg()) qDebug() << "[audio] startAudio: nenhuma fonte com áudio em t=" << t;
        return; // nada com áudio neste instante
    }
    if (audioDbg()) {
        qDebug() << "[audio] startAudio em t=" << t << "- fontes:" << sources.size();
        const auto outs = QMediaDevices::audioOutputs();
        for (const auto& d : outs)
            qDebug() << "[audio]   saida:" << d.description() << "default?" << d.isDefault();
        const QAudioDevice def = QMediaDevices::defaultAudioOutput();
        qDebug() << "[audio]   defaultAudioOutput() válido?" << def.isNull() << "-" << def.description();
    }

    QAudioFormat fmt;
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    fmt.setSampleFormat(QAudioFormat::Int16);
#else
    fmt.setCodec(QStringLiteral("audio/pcm"));
    fmt.setSampleSize(16);
    fmt.setSampleType(QAudioFormat::SignedInt);
    fmt.setByteOrder(QAudioFormat::LittleEndian);
#endif
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);

    m_audioFeed = new AudioMixer(this);
    m_audioFeed->open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    m_audioFeed->updateSources(sources, true);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    m_audioSink = new QAudioSink(QMediaDevices::defaultAudioOutput(), fmt, this);
    if (audioDbg()) {
        connect(m_audioSink, &QAudioSink::stateChanged, this, [this](QAudio::State s) {
            qDebug() << "[audio] sink state =" << s;
            if (s == QAudio::StoppedState && m_audioSink)
                qDebug() << "[audio] sink error =" << m_audioSink->error();
        });
    }
    m_audioSink->start(m_audioFeed);
#else
    m_audioOut = new QAudioOutput(QAudioDeviceInfo::defaultOutputDevice(), fmt, this);
    if (audioDbg()) {
        connect(m_audioOut, &QAudioOutput::stateChanged, this, [](QAudioOutput::State s) {
            qDebug() << "[audio] sink state =" << s;
        });
        connect(m_audioOut, &QAudioOutput::errorChanged, this, [](QAudioOutput::Error e) {
            qDebug() << "[audio] sink ERROR =" << e;
        });
    }
    m_audioOut->start(m_audioFeed);
#endif
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
}

// Recalcula as fontes de áudio ativas no instante `t` e empurra ao mixer.
void PreviewWidget::updateMixAudio(double t, bool reseek) {
    if (!m_audioFeed) return;
    m_audioFeed->updateSources(buildMixSources(m_project, t), reseek);
    emit m_audioFeed->readyRead();
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
