// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

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
#include <QPainterPath>
#include <QFontMetrics>
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif
#include <algorithm>
#include <QDebug>
#include <cmath>
#include <climits>
#include <cstring>
#include <memory>
#include <QHash>
#include <QMutex>
#include <QDebug>

static int s_maxDecodeWidth = -1;

// Diagnóstico do caminho de áudio do preview: ligue com PIERROT_AUDIO_DEBUG=1.
static bool audioDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_AUDIO_DEBUG");
    return on;
}

// Diagnóstico da composição multi-faixa do preview: ligue com PIERROT_COMPOSE_DEBUG=1.
static bool composeDbg() {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_COMPOSE_DEBUG");
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

// Decodifica quadros de vídeo na própria thread (com dois FFmpegDecoder dedicados:
// um para o clipe principal e outro para pré-carregamento especulativo).
// O PreviewWidget pede o "último" quadro desejado e descarta intermediários.
class FrameWorker : public QObject {
    Q_OBJECT
public:
    explicit FrameWorker(QObject* parent = nullptr)
        : QObject(parent),
          m_mainDecoder(std::make_unique<FFmpegDecoder>()),
          m_prefetchDecoder(std::make_unique<FFmpegDecoder>()) {}

public slots:
    void decodeOne(const QString& clipId, const QString& path, double t, int maxW, double dt) {
        if (!m_mainDecoder->isOpen() || m_mainDecoder->source() != path) {
            if (m_prefetchDecoder->isOpen() && m_prefetchDecoder->source() == path) {
                // O decodificador de prefetch já abriu e aqueceu este arquivo: swap instantâneo!
                std::swap(m_mainDecoder, m_prefetchDecoder);
            } else {
                if (!m_mainDecoder->open(path)) {
                    emit frameReady(clipId, path, t, maxW, QImage());
                    return;
                }
            }
        }

        // Pipeline de 1 frame à frente: quando o próximo frame já foi
        // decodificado na folga (m_ready*), devolve instantâneo e decodifica o
        // seguinte. Esconde a latência do decode quando ele passa de 33ms
        // (fontes pesadas, ex. 4K) — o preview mantém a cadência mesmo que um
        // frame individual demore. Em seek/atraso cai para decode normal.
        // O passo usa a cadência do PROJETO (dt = 1/fps do projeto), não a do
        // arquivo: a UI pede quadros no ritmo do projeto, e prefetchar com o
        // fps do arquivo (fd) deixava o m_ready fora de fase quando os dois
        // diferem, entregando frame repetido ou atrasado.
        const double step = (dt > 0.0) ? dt
                            : ((m_mainDecoder->fps() > 0.0) ? 1.0 / m_mainDecoder->fps() : 1.0 / 30.0);
        QImage img;
        if (m_readyValid && m_readyPath == path && m_readyMaxW == maxW
            && std::fabs(m_readyT - t) <= step * 0.5) {
            img = m_readyImg;
            m_readyValid = false;
        } else {
            img = m_mainDecoder->frameAt(t, maxW);
        }
        emit frameReady(clipId, path, t, maxW, img);

        // Decodifica o próximo frame na folga para o próximo pedido (apenas se
        // o decoder ainda estiver no mesmo arquivo).
        if (!img.isNull() && m_mainDecoder->isOpen() && m_mainDecoder->source() == path) {
            m_readyImg = m_mainDecoder->frameAt(t + step, maxW);
            m_readyT = t + step;
            m_readyPath = path;
            m_readyMaxW = maxW;
            m_readyValid = !m_readyImg.isNull();
        } else {
            m_readyValid = false;
        }
    }

    void decodePrefetch(const QString& path, double t, int maxW) {
        if (!m_prefetchDecoder->isOpen() || m_prefetchDecoder->source() != path) {
            if (!m_prefetchDecoder->open(path)) {
                emit prefetchReady(path, t, maxW, QImage());
                return;
            }
        }
        emit prefetchReady(path, t, maxW, m_prefetchDecoder->frameAt(t, maxW));
    }

signals:
    void frameReady(const QString& clipId, const QString& path, double t, int maxW, const QImage& img);
    void prefetchReady(const QString& path, double t, int maxW, const QImage& img);

private:
    std::unique_ptr<FFmpegDecoder> m_mainDecoder;
    std::unique_ptr<FFmpegDecoder> m_prefetchDecoder;

    // Frame decodificado adiante (pipeline de 1 frame à frente).
    QImage m_readyImg;
    QString m_readyPath;
    double m_readyT = -1.0;
    int m_readyMaxW = 0;
    bool m_readyValid = false;
};

// Mixer de áudio: soma o PCM de todos os clipes ativos em `t` (clipe de vídeo
// + faixas de áudio), cada um com volume próprio (clipe, envelope e faixa).
// Todos os FFmpegDecoder resampleiam para S16/48 kHz/estéreo, então misturar
// é alinhar amostra a amostra. A thread do QAudioSink chama readData(); a UI
// chama updateSources() conforme o playhead avança.

// DSP dos efeitos de áudio do preview — réplica em tempo real do que a
// exportação aplica via ffmpeg:
//   equalizer f=120/1000/6000 Q=1  -> biquad peaking por banda
//   aeval (inverter fase)          -> multiplica por -1
//   afftdn (denoise)               -> aproximação: noise gate suave
//   loudnorm (normalizar -14 LUFS) -> aproximação: AGC lento + soft clip
class AudioFx {
public:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        void peaking(double freq, double q, double gainDb, double fs) {
            const double A = std::pow(10.0, gainDb / 40.0);
            const double w0 = 2.0 * M_PI * freq / fs;
            const double alpha = std::sin(w0) / (2.0 * q);
            const double a0 = 1.0 + alpha / A;
            b0 = (1.0 + alpha * A) / a0;
            b1 = (-2.0 * std::cos(w0)) / a0;
            b2 = (1.0 - alpha * A) / a0;
            a1 = (-2.0 * std::cos(w0)) / a0;
            a2 = (1.0 - alpha / A) / a0;
        }
        inline double tick(double x) {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
        void reset() { x1 = x2 = y1 = y2 = 0.0; }
    };

    void configure(double eqLow, double eqMid, double eqHigh, bool denoise,
                   double denoiseAmt, bool invertPhase, bool normalize) {
        const double fs = 48000.0;
        const int key = (int)std::llround(eqLow * 10) * 1000000
                      + (int)std::llround(eqMid * 10) * 1000
                      + (int)std::llround(eqHigh * 10)
                      + (denoise ? 100 : 0)
                      + (invertPhase ? 200 : 0)
                      + (normalize ? 400 : 0)
                      + (int)std::llround(denoiseAmt) * 10000;
        if (key == m_key) return; // parâmetros inalterados: mantém o estado
        m_key = key;
        for (int ch = 0; ch < 2; ++ch) {
            low[ch].peaking(120.0, 1.0, std::clamp(eqLow, -12.0, 12.0), fs);
            mid[ch].peaking(1000.0, 1.0, std::clamp(eqMid, -12.0, 12.0), fs);
            high[ch].peaking(6000.0, 1.0, std::clamp(eqHigh, -12.0, 12.0), fs);
        }
        invert = invertPhase;
        gateEnabled = denoise;
        gateAmount = std::clamp(denoiseAmt, 1.0, 50.0);
        agcEnabled = normalize;
        resetState();
    }

    void resetState() {
        for (int ch = 0; ch < 2; ++ch) {
            low[ch].reset(); mid[ch].reset(); high[ch].reset();
        }
        gateEnv = 0.0;
        agcLevel = 0.0;
        agcGain = 1.0;
    }

    // Processa `frames` amostras estéreo interleaved S16 no próprio buffer.
    void process(int16_t* buf, int frames) {
        for (int f = 0; f < frames; ++f) {
            double l = buf[2 * f] / 32768.0;
            double r = buf[2 * f + 1] / 32768.0;
            l = high[0].tick(mid[0].tick(low[0].tick(l)));
            r = high[1].tick(mid[1].tick(low[1].tick(r)));
            if (invert) { l = -l; r = -r; }
            if (gateEnabled) {
                const double peak = qMax(std::fabs(l), std::fabs(r));
                gateEnv = peak > gateEnv ? peak : gateEnv * 0.999;
                const double db = 20.0 * std::log10(gateEnv + 1e-9);
                const double floorDb = -50.0;
                double g = 1.0;
                if (db < floorDb) {
                    const double depth = 1.0 - (db - floorDb) / (0.0 - floorDb);
                    g = std::pow(10.0, -(gateAmount * depth) / 20.0);
                }
                l *= g; r *= g;
            }
            if (agcEnabled) {
                const double lvl = 0.5 * (l * l + r * r);
                agcLevel = agcLevel * 0.999 + lvl * 0.001;
                const double db = 20.0 * std::log10(std::sqrt(agcLevel) + 1e-9);
                const double want = -14.0 - db; // ganho (dB) rumo a -14
                const double g = std::pow(10.0, std::clamp(want, -12.0, 12.0) / 20.0);
                agcGain = agcGain * 0.95 + g * 0.05;
                l *= agcGain; r *= agcGain;
                const double pk = qMax(std::fabs(l), std::fabs(r));
                if (pk > 0.95) { const double s = 0.95 / pk; l *= s; r *= s; }
            }
            buf[2 * f] = (int16_t)std::lround(std::clamp(l, -1.0, 1.0) * 32768.0);
            buf[2 * f + 1] = (int16_t)std::lround(std::clamp(r, -1.0, 1.0) * 32768.0);
        }
    }

private:
    int m_key = -1;
    Biquad low[2], mid[2], high[2];
    bool invert = false;
    bool gateEnabled = false;
    double gateAmount = 12.0;
    double gateEnv = 0.0;
    bool agcEnabled = false;
    double agcLevel = 0.0;
    double agcGain = 1.0;
};

class AudioMixer : public QIODevice {
public:
    struct SourceInfo {
        QString key;      // id do clipe (estável durante a reprodução)
        QString path;     // arquivo de mídia
        int audioStream = 0; // stream de áudio usado por este clipe
        double mediaPos;  // posição no arquivo de mídia (em segundos)
        double vol = 1.0;
        double eqLow = 0.0;
        double eqMid = 0.0;
        double eqHigh = 0.0;
        bool denoise = false;
        double denoiseAmount = 12.0;
        bool normalize = false;
        bool invertPhase = false;
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
                if (!s->dec.open(w.path, w.audioStream)) {
                    if (audioDbg()) qDebug() << "[audio] updateSources: FALHOU ao abrir" << w.path
                                             << "stream=" << w.audioStream;
                    delete s; continue;
                }
                s->key = w.key;
                s->dec.seekAudio(w.mediaPos);
                m_sources.append(s);
                if (audioDbg()) qDebug() << "[audio] updateSources: +fonte" << w.path
                                         << "stream=" << w.audioStream << "mediaPos=" << w.mediaPos;
            }
            s->vol = w.vol;
            s->fx.configure(w.eqLow, w.eqMid, w.eqHigh, w.denoise,
                            w.denoiseAmount, w.invertPhase, w.normalize);
            s->active = true;
            if (reseek) {
                s->dec.seekAudio(w.mediaPos);
                s->fx.resetState();
            }
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
            if (n > 0) {
                s->fx.process(tmp.data(), n);
                const int16_t* src = tmp.constData();
                for (int i = 0; i < n * ch; ++i) {
                    const float sum = out[i] / 32768.0f + src[i] / 32768.0f * (float)s->vol;
                    const float cl = qBound(-1.0f, sum, 1.0f);
                    out[i] = (int16_t)std::lround(cl * 32768.0f);
                }
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
        AudioFx fx;
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

// Mapeia o modo de composição da faixa (os mesmos 12 modos da exportação)
// para o composition mode do QPainter. "subtract" não existe no Qt: cai em
// SourceOver (aproximação razoável para o preview).
QPainter::CompositionMode blendModeToQt(const QString& mode) {
    if (mode == QStringLiteral("screen"))    return QPainter::CompositionMode_Screen;
    if (mode == QStringLiteral("multiply"))  return QPainter::CompositionMode_Multiply;
    if (mode == QStringLiteral("overlay"))   return QPainter::CompositionMode_Overlay;
    if (mode == QStringLiteral("darken"))    return QPainter::CompositionMode_Darken;
    if (mode == QStringLiteral("lighten"))   return QPainter::CompositionMode_Lighten;
    if (mode == QStringLiteral("softlight")) return QPainter::CompositionMode_SoftLight;
    if (mode == QStringLiteral("hardlight")) return QPainter::CompositionMode_HardLight;
    if (mode == QStringLiteral("difference"))return QPainter::CompositionMode_Difference;
    if (mode == QStringLiteral("addition"))  return QPainter::CompositionMode_Plus;
    if (mode == QStringLiteral("exclusion")) return QPainter::CompositionMode_Exclusion;
    return QPainter::CompositionMode_SourceOver;
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
    m_timer->setTimerType(Qt::PreciseTimer);
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
    connect(m_frameWorker, &FrameWorker::prefetchReady,
            this, &PreviewWidget::onPrefetchReady, Qt::QueuedConnection);
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
    m_underFrame = QImage();
    m_underPath.clear();
    m_underT = -1.0;
    m_underW = -1;
    m_underRequested = false;
    m_transAlpha = -1.0;
    m_transType.clear();
    {
        QMutexLocker l(&m_frameMutex);
        m_reqQueue.clear();
        m_layerCache.clear();
        m_prefetch = PrefetchFrame();
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

    // Texto independente ativo no playhead (desenhado mesmo sem quadro de vídeo).
    bool anyText = false;
    if (m_project) {
        for (const Track& tr : m_project->videoTracks)
            for (const Clip& c : tr.clips)
                if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur) {
                    anyText = true;
                    break;
                }
    }

    // Render unificado em "espaço de projeto": o canvas É o quadro do projeto
    // (k = pixels de tela por pixel do projeto). O vídeo é desenhado do mesmo
    // jeito com ou sem transform — sem transform, ele cabe inteiro no quadro;
    // com transform, pan/rot/zoom giram em torno do centro do quadro. Assim,
    // adicionar um keyframe de transform nunca muda o tamanho do vídeo.
    const Clip* clip = clipAt(m_playhead);
    const double S = clip ? kfValue(clip->kfScale, clip->scale, m_playhead - clip->pos) : 1.0;
    const double SX = clip ? kfValue(clip->kfScaleX, clip->scaleX, m_playhead - clip->pos) : 1.0;
    const double SY = clip ? kfValue(clip->kfScaleY, clip->scaleY, m_playhead - clip->pos) : 1.0;
    const double rot = clip ? kfValue(clip->kfRotation, clip->rotation, m_playhead - clip->pos) : 0.0;
    const double tx = clip ? kfValue(clip->kfTx, clip->tx, m_playhead - clip->pos) : 0.0;
    const double ty = clip ? kfValue(clip->kfTy, clip->ty, m_playhead - clip->pos) : 0.0;

    // Desenha uma camada com transform (escala, rotação, pan), centrada no
    // ponto (cx, cy) e na escala kk (qualquer buffer/painter). `clipRect` evita
    // que o conteúdo vaze do monitor (rotação/zoom) para a área ao redor.
    const auto drawLayer = [&](QPainter& qp, const QImage& img, double s, double r,
                               double x, double y, double alpha,
                               double sX, double sY, double kk, double cx, double cy,
                               const QRect& clipRect,
                               double ox = 0.0, double oy = 0.0) {
        const double fit = qMin(pw / img.width(), ph / img.height());
        qp.save();
        qp.setClipRect(clipRect);
        qp.translate(cx + x * kk + ox * kk, cy + y * kk + oy * kk);
        qp.rotate(r);
        qp.scale(kk * fit * s * sX, kk * fit * s * sY);
        qp.translate(-img.width() / 2.0, -img.height() / 2.0);
        if (alpha < 1.0) {
            QImage img2 = img;
            QPainter ip(&img2);
            ip.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            ip.fillRect(img2.rect(), QColor(0, 0, 0, (int)(alpha * 255)));
            ip.end();
            qp.drawImage(0, 0, img2);
        } else {
            qp.drawImage(0, 0, img);
        }
        qp.restore();
    };

    // Uma camada de vídeo pronta para desenhar (na ordem de baixo para cima).
    struct Layer {
        QImage frame;
        double s = 1.0, sX = 1.0, sY = 1.0, rot = 0.0, x = 0.0, y = 0.0;
        double alpha = 1.0, ox = 0.0, oy = 0.0;
        QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;
    };
    QVector<Layer> layers;

    // Monta o empilhamento: todas as faixas de vídeo (a faixa 0 é o TOPO), de
    // baixo para cima. Cada faixa contribui com o clipe ativo no playhead.
    // Clipes de texto independentes entram depois (desenhados por cima).
    {
        const Clip* topClip = clip;
        for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
            const Track& t = m_project->videoTracks[tr];
            const Clip* c = nullptr;
            for (const Clip& cl : t.clips) {
                if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                    const MediaItem* mm = m_project->findMedia(cl.mediaId);
                    if (mm && mm->hasVideo && (!c || cl.pos > c->pos)) c = &cl;
                }
            }
            if (!c) continue;
            const QPainter::CompositionMode mode = blendModeToQt(t.blendMode);
            if (topClip && c->id == topClip->id && !m_frame.isNull()) {
                // Transição (mesma faixa): o clipe de trás por baixo, o da
                // frente por cima com fade (dissolve) ou deslizando (wipe).
                if (m_transAlpha >= 0.0 && !m_underFrame.isNull()) {
                    Layer u;
                    u.frame = m_underFrame;
                    layers.append(u);
                }
                Layer L;
                L.frame = m_frame;
                L.s = S; L.sX = SX; L.sY = SY; L.rot = rot; L.x = tx; L.y = ty;
                const double rel = m_playhead - topClip->pos;
                double alpha = std::clamp(kfValue(topClip->kfOpacity, topClip->opacity, rel), 0.0, 1.0);
                if (topClip->fadeIn > 1e-6) alpha *= std::min(1.0, rel / topClip->fadeIn);
                if (topClip->fadeOut > 1e-6) alpha *= std::min(1.0, (topClip->dur - rel) / topClip->fadeOut);
                if (m_transAlpha >= 0.0) {
                    if (m_transType == QStringLiteral("dissolve")) alpha *= m_transAlpha;
                    else if (m_transType == QStringLiteral("wipeleft")) L.ox = pw * (1.0 - m_transAlpha);
                    else if (m_transType == QStringLiteral("wiperight")) L.ox = -pw * (1.0 - m_transAlpha);
                    else if (m_transType == QStringLiteral("wipeup")) L.oy = ph * (1.0 - m_transAlpha);
                    else if (m_transType == QStringLiteral("wipedown")) L.oy = -ph * (1.0 - m_transAlpha);
                }
                L.alpha = alpha;
                // Opacidade da faixa (estilo Vegas/FCE): a faixa inteira é
                // composta com transparência sobre as de baixo.
                L.alpha *= std::clamp(t.opacity, 0.0, 1.0);
                L.mode = mode;
                layers.append(L);
            } else {
                const MediaItem* mm = m_project->findMedia(c->mediaId);
                if (!mm || !mm->hasVideo) continue;
                QImage f;
                if (mm->isSolid) {
                    // Mídia gerada (cor sólida estilo Vegas): sem arquivo — o
                    // quadro é preenchido na hora, na proporção do projeto (o
                    // "fit" do drawLayer enche o quadro; cor uniforme não
                    // perde qualidade ao ser ampliada).
                    const int w = qMax(1, mm->width > 0 ? mm->width : m_project->width);
                    const int h = qMax(1, mm->height > 0 ? mm->height : m_project->height);
                    const int sw = 64;
                    const int sh = qMax(1, sw * h / w);
                    f = QImage(sw, sh, QImage::Format_ARGB32);
                    f.fill(mm->solidColor);
                } else {
                    {
                        QMutexLocker l(&m_frameMutex);
                        f = m_layerCache.value(c->id).img;
                    }
                    if (f.isNull()) continue; // quadro ainda não chegou
                }
                const double rel = m_playhead - c->pos;
                Layer L;
                L.frame = f;
                L.s = kfValue(c->kfScale, c->scale, rel);
                L.sX = kfValue(c->kfScaleX, c->scaleX, rel);
                L.sY = kfValue(c->kfScaleY, c->scaleY, rel);
                L.rot = kfValue(c->kfRotation, c->rotation, rel);
                L.x = kfValue(c->kfTx, c->tx, rel);
                L.y = kfValue(c->kfTy, c->ty, rel);
                double alpha = std::clamp(kfValue(c->kfOpacity, c->opacity, rel), 0.0, 1.0);
                if (c->fadeIn > 1e-6) alpha *= std::min(1.0, rel / c->fadeIn);
                if (c->fadeOut > 1e-6) alpha *= std::min(1.0, (c->dur - rel) / c->fadeOut);
                L.alpha = alpha;
                // Opacidade da faixa (estilo Vegas/FCE): a faixa inteira é
                // composta com transparência sobre as de baixo.
                L.alpha *= std::clamp(t.opacity, 0.0, 1.0);
                L.mode = mode;
                layers.append(L);
            }
            if (composeDbg())
                qDebug() << "[compose] tr=" << tr
                         << "clip=" << (c->name.isEmpty() ? c->id : c->name)
                         << "top=" << (topClip && c->id == topClip->id)
                         << "media=" << (m_project->findMedia(c->mediaId)
                                            ? m_project->findMedia(c->mediaId)->name : QString());
        }
    }

    if (composeDbg()) {
        for (const Layer& L : layers) {
            int cornerA = -1;
            if (!L.frame.isNull() && L.frame.width() > 2 && L.frame.height() > 2)
                cornerA = L.frame.pixelColor(1, 1).alpha();
            qDebug() << "  [compose] layer " << L.frame.width() << "x" << L.frame.height()
                     << "alpha=" << L.alpha << "cornerA=" << cornerA << "mode=" << (int)L.mode;
        }
        qDebug() << "  [compose] total=" << layers.size() << "m_frameNull=" << m_frame.isNull();
    }

    if (m_frame.isNull() && layers.isEmpty() && !anyText) {
        drawEmptyMonitor(p, canvas);
        return;
    }

    // Empilhamento multi-faixa: compõe num buffer do tamanho do canvas, faixa
    // a faixa com o modo de composição da faixa (igual ao blend da exportação),
    // de baixo para cima — é o que permite ver transparência (PNG/WebP com
    // alpha) e as camadas que ficam POR BAIXO do clipe do topo.
    // O fundo é PRETO opaco (como o ffmpeg): os modos de blend (multiply,
    // screen…) produzem o mesmo resultado da exportação, e não dependem do
    // fundo do monitor.
    if (layers.size() >= 2 || (m_frame.isNull() && !layers.isEmpty())) {
        QImage acc(canvas.size(), QImage::Format_ARGB32);
        acc.fill(Qt::black);
        QPainter ap(&acc);
        ap.setClipRect(QRect(0, 0, acc.width(), acc.height()));
        const double cx = acc.width() / 2.0;
        const double cy = acc.height() / 2.0;
        for (const Layer& L : layers) {
            if (L.frame.isNull()) continue;
            ap.setCompositionMode(L.mode);
            drawLayer(ap, L.frame, L.s, L.rot, L.x, L.y, L.alpha, L.sX, L.sY, k, cx, cy,
                      QRect(0, 0, acc.width(), acc.height()), L.ox, L.oy);
        }
        // Texto sempre em SourceOver (o modo de composição da última camada
        // não pode vazar para o texto).
        ap.setCompositionMode(QPainter::CompositionMode_SourceOver);
        // Texto (independente e anexado) por cima das camadas.
        if (m_project) {
            for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
                const Clip* tclip = nullptr;
                for (const Clip& c : m_project->videoTracks[tr].clips)
                    if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur)
                        if (!tclip || c.pos > tclip->pos) tclip = &c;
                if (tclip) drawClipText(ap, acc.rect(), tclip, k);
            }
        }
        if (clip && !clip->isText)
            drawClipText(ap, acc.rect(), clip, k);
        ap.end();
        p.drawImage(canvas.topLeft(), acc);
        return;
    }

    // Caminho tradicional (um clipe só, com ou sem transição): desenha direto.
    // Opacidade do clipe + fades de entrada/saída + opacidade da faixa
    // aplicados no alpha (como na exportação), para o fade aparecer mesmo com
    // um clipe sozinho.
    const double clipRel = clip ? (m_playhead - clip->pos) : 0.0;
    double clipAlpha = 1.0;
    double clipTrackOpacity = 1.0;
    if (clip && m_project) {
        clipAlpha = std::clamp(kfValue(clip->kfOpacity, clip->opacity, clipRel), 0.0, 1.0);
        if (clip->fadeIn > 1e-6) clipAlpha *= std::min(1.0, clipRel / clip->fadeIn);
        if (clip->fadeOut > 1e-6) clipAlpha *= std::min(1.0, (clip->dur - clipRel) / clip->fadeOut);
        for (const Track& tr : m_project->videoTracks)
            for (const Clip& c : tr.clips)
                if (c.id == clip->id) { clipTrackOpacity = std::clamp(tr.opacity, 0.0, 1.0); break; }
    }
    clipAlpha *= clipTrackOpacity;
    const bool transActive = m_transAlpha >= 0.0
                             && !m_frame.isNull() && !m_underFrame.isNull();
    if (!m_frame.isNull()) {
        if (transActive) {
            drawLayer(p, m_underFrame, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, k,
                      canvas.center().x(), canvas.center().y(), canvas);
            double ox = 0.0, oy = 0.0;
            if (m_transType == QStringLiteral("wipeleft")) ox = pw * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wiperight")) ox = -pw * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipeup")) oy = ph * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipedown")) oy = -ph * (1.0 - m_transAlpha);
            double a = clipAlpha;
            if (m_transType == QStringLiteral("dissolve")) a *= m_transAlpha;
            drawLayer(p, m_frame, S, rot, tx, ty, a, SX, SY, k,
                      canvas.center().x(), canvas.center().y(), canvas, ox, oy);
        } else {
            drawLayer(p, m_frame, S, rot, tx, ty, clipAlpha, SX, SY, k,
                      canvas.center().x(), canvas.center().y(), canvas);
        }
    }

    // Clipes de texto independentes: desenha todos os ativos no playhead, da
    // faixa de baixo para a de cima (a faixa 0 é o topo e fica por cima).
    if (m_project) {
        for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
            const Clip* tclip = nullptr;
            for (const Clip& c : m_project->videoTracks[tr].clips)
                if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur)
                    if (!tclip || c.pos > tclip->pos) tclip = &c;
            if (tclip) drawClipText(p, canvas, tclip, k);
        }
    }
    // Texto anexado a um clipe de vídeo (comportamento antigo) ainda vale.
    if (clip && !clip->isText)
        drawClipText(p, canvas, clip, k);
}

// Desenha o texto/título estilizado do clipe sobre o monitor (mesmo resultado
// visual do drawtext da exportação). O desenho acontece em espaço de PROJETO
// mapeado para o canvas (escala k), aplicando o transform animável do clipe
// (escala, rotação, pan) e a opacidade (fades/keyframes).
void PreviewWidget::drawClipText(QPainter& p, const QRect& canvas, const Clip* clip, double k) {
    if (!clip || !m_project) return;
    const TextStyle& st = *m_project->textStyleFor(*clip);
    if (st.isEmpty()) return;
    const double W = m_project->width;
    const double H = m_project->height;
    const double rel = m_playhead - clip->pos;
    const double alpha = std::clamp(kfValue(clip->kfOpacity, clip->opacity, rel), 0.0, 1.0);
    if (alpha <= 0.0) return;

    const double sizeFrac = st.textSize > 0.0 ? st.textSize : (1.0 / 18.0);
    const int pxSize = qMax(4, (int)qRound(sizeFrac * H)); // px de projeto
    QFont font;
    if (!st.fontFamily.isEmpty()) font.setFamily(st.fontFamily);
    font.setPixelSize(pxSize);
    font.setBold(st.textBold);
    const QFontMetricsF fm(font);

    // Quebra em linhas dentro de 90% da largura do projeto.
    const double maxW = W * 0.9;
    QStringList wrapped;
    for (const QString& raw : st.text.split(QLatin1Char('\n'))) {
        if (raw.isEmpty()) { wrapped << QString(); continue; }
        QString cur;
        const QStringList words = raw.split(QLatin1Char(' '));
        for (const QString& w : words) {
            const QString trial = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
            if (fm.horizontalAdvance(trial) <= maxW || cur.isEmpty())
                cur = trial;
            else { wrapped << cur; cur = w; }
        }
        wrapped << cur;
    }

    double tw = 0.0;
    for (const QString& l : wrapped) tw = qMax(tw, fm.horizontalAdvance(l));
    const double th = wrapped.size() * fm.height();
    const double pad = pxSize * 0.25;
    const double bw = tw + 2.0 * pad;
    const double bh = th + 2.0 * pad;

    // Posição do texto RELATIVA ao centro do quadro (a origem do transform é
    // o centro + pan). textX=0.5 → x=0 → texto centralizado. Antes desenhávamos
    // em coordenadas absolutas (0.5W) E transladávamos por W/2: o texto caía
    // em 0.5W+0.5W = W (canto inferior direito, fora da tela).
    double x;
    if (st.textAlign == 1) x = st.textX * W - W / 2.0;
    else if (st.textAlign == 2) x = st.textX * W - bw - W / 2.0;
    else x = st.textX * W - bw / 2.0 - W / 2.0;
    const double y = st.textY * H - bh / 2.0 - H / 2.0;
    const QRectF box(x, y, bw, bh);

    p.save();
    p.setClipRect(canvas);
    // Mapeia espaço de projeto (0..W x 0..H) para o canvas.
    p.translate(canvas.topLeft());
    p.scale(k, k);
    // Transform animável do clipe, em torno do centro do quadro.
    p.translate(W / 2.0 + kfValue(clip->kfTx, clip->tx, rel),
                H / 2.0 + kfValue(clip->kfTy, clip->ty, rel));
    p.rotate(kfValue(clip->kfRotation, clip->rotation, rel));
    p.scale(kfValue(clip->kfScale, clip->scale, rel)
                * kfValue(clip->kfScaleX, clip->scaleX, rel),
            kfValue(clip->kfScale, clip->scale, rel)
                * kfValue(clip->kfScaleY, clip->scaleY, rel));

    if (st.textBackground) {
        QColor bc = st.textBackgroundColor;
        bc.setAlpha((int)(bc.alpha() * alpha));
        p.fillRect(box, bc);
    }

    QPainterPath path;
    const double baseline = box.top() + pad + fm.ascent();
    for (int i = 0; i < wrapped.size(); ++i)
        path.addText(QPointF(box.left() + pad, baseline + i * fm.height()), font, wrapped[i]);

    if (st.textOutline > 0.0) {
        const double ow = qMax(1.0, st.textOutline * H);
        QColor oc = st.textOutlineColor;
        oc.setAlpha((int)(oc.alpha() * alpha));
        QPen pen(oc, ow, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(path, pen);
    }
    QColor fc = st.textColor;
    fc.setAlpha((int)(fc.alpha() * alpha));
    p.fillPath(path, fc);

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
    if (!m_playing) {
        m_currentFrameIndex = std::llround(m_playhead * fps);
    }
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
    const double fps = projFps(m_project);
    m_currentFrameIndex = std::llround(m_playhead * fps);
    m_playStart = m_playhead;
    m_clock.start();
    m_playing = true;
    // Dispara em metade do período do frame: o tick calcula o frame alvo pelo
    // clock de alta precisão e avança exatamente 1 frame por vez. Com timer
    // grosseiro (1x/frame), o QTimer atrasado pela UI fazia o llround pular frames.
    m_timer->setInterval(fps > 0.0 ? qBound(8, (int)std::lround(1000.0 / fps / 2.0), 40) : 33);
    m_timer->start();
    m_playBtn->setText(tr("Pausar"));
    startAudio(m_playhead);
    emit stateChanged(true);
}

void PreviewWidget::playFrom(double t) {
    // Enter (estilo Vegas): busca para a posição e começa a reproduzir dali,
    // mesmo se já estivesse tocando.
    if (!m_project || m_project->duration() <= 0) return;
    seek(std::clamp(t, 0.0, m_project->duration()));
    const double fps = projFps(m_project);
    m_currentFrameIndex = std::llround(m_playhead * fps);
    m_playStart = m_playhead;
    m_clock.start();
    m_playing = true;
    m_timer->setInterval(fps > 0.0 ? qBound(8, (int)std::lround(1000.0 / fps / 2.0), 40) : 33);
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
    m_currentFrameIndex = -1;
    stopAudio();
    {
        QMutexLocker l(&m_frameMutex);
        m_prefetch = PrefetchFrame();
    }
    emit stateChanged(false);
}

void PreviewWidget::tick() {
    if (!m_project) { stopPlayback(); return; }
    const double fps = projFps(m_project);
    const double dur = m_project->duration();

    // Determina o índice de frame com base no clock de alta precisão
    const double elapsed = m_clock.elapsed() / 1000.0;
    const qint64 targetFrame = std::llround((m_playStart + elapsed) * fps);

    // Se o timer acordou ligeiramente antes de 1 frame inteiro passar, não repete nem duplica o frame
    if (targetFrame <= m_currentFrameIndex) {
        return;
    }

    m_currentFrameIndex = targetFrame;
    const double t = (fps > 0.0) ? (targetFrame / fps) : (m_playStart + elapsed);

    if (m_loopOut > m_loopIn && t >= m_loopOut - 1e-9) {
        m_playStart = m_loopIn;
        m_currentFrameIndex = std::llround(m_loopIn * fps);
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
    updatePrefetch();
    // Mixer acompanha o playhead: volumes/fades e troca de clipes acontecem
    // aqui, sem reiniciar o sink a cada transição.
    updateMixAudio(t, false);
}

// Clipe de vídeo (com mídia) no topo em `t`. Clipes de texto independentes são
// ignorados aqui (não têm quadro para decodificar); o texto é desenhado por
// cima do vídeo no paintEvent.
const Clip* PreviewWidget::clipAt(double t) const {
    if (!m_project) return nullptr;
    for (int tr = 0; tr < (int)m_project->videoTracks.size(); ++tr) {
        const Clip* best = nullptr;
        for (const Clip& c : m_project->videoTracks[tr].clips) {
            if (t >= c.pos && t < c.pos + c.dur && !c.isText) {
                const MediaItem* m = m_project->findMedia(c.mediaId);
                if (m && m->hasVideo && (!best || c.pos > best->pos)) best = &c;
            }
        }
        if (best) return best;
    }
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
                // Crossfade de transição: sobreposição com vizinho da MESMA
                // faixa vira fade-in (da frente) / fade-out (de trás).
                double transIn = 0.0, transOut = 0.0;
                for (const Clip& o : tr.clips) {
                    if (o.id == c.id) continue;
                    if (o.pos < c.pos - 1e-6 && o.pos + o.dur > c.pos + 1e-6)
                        transIn = std::max(transIn, o.pos + o.dur - c.pos);
                    if (o.pos > c.pos + 1e-6 && o.pos < c.pos + c.dur - 1e-6)
                        transOut = std::max(transOut, c.pos + c.dur - o.pos);
                }
                if (transIn > 1e-6) vol *= std::clamp((t - c.pos) / transIn, 0.0, 1.0);
                if (transOut > 1e-6) vol *= std::clamp((c.pos + c.dur - t) / transOut, 0.0, 1.0);
                vol = std::clamp(vol, 0.0, 2.0);
                // Mesmo grupo (vídeo+áudio vinculados) compartilha a chave para
                // não dobrar o som; o STREAM diferencia as faixas de um arquivo
                // multicanal (cada clipe de áudio usa o seu stream). O clipe da
                // faixa de áudio vence o da faixa de vídeo (inserido depois).
                const QString base = c.groupId.isEmpty() ? c.id : c.groupId;
                const QString key = QStringLiteral("%1|%2").arg(base).arg(c.audioStreamIndex);
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
        si.audioStream = c->audioStreamIndex;
        si.mediaPos = c->in + (t - c->pos) * c->speed;
        si.vol = it.value().vol;
        si.eqLow = c->eqLow;
        si.eqMid = c->eqMid;
        si.eqHigh = c->eqHigh;
        si.denoise = c->denoise;
        si.denoiseAmount = c->denoiseAmount;
        si.normalize = c->normalize;
        si.invertPhase = c->invertPhase;
        out.append(si);
    }
    return out;
}

void PreviewWidget::startAudio(double t) {
    if (!m_project) return;
    stopAudio();
    // Cria o mixer/SEMPRE, mesmo se não houver fonte de áudio em `t`: dar play
    // num vão de silêncio (ex.: clipe de áudio movido para longe) não pode
    // deixar o preview mudo — as fontes são adicionadas pelo updateMixAudio
    // quando a reprodução alcançar o clipe. Antes retornávamos aqui sem criar
    // o mixer e o áudio nunca ligava ao alcançar o trecho com som.
    const QVector<AudioMixer::SourceInfo> sources = buildMixSources(m_project, t);
    if (sources.isEmpty() && audioDbg())
        qDebug() << "[audio] startAudio em t=" << t << "- nenhuma fonte (mixer será criado mudo)";
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
    const QAudioDevice def = QMediaDevices::defaultAudioOutput();
    if (def.isNull()) {
        if (audioDbg()) qDebug() << "[audio] defaultAudioOutput nulo — sem saída de áudio";
        return;
    }
    m_audioSink = new QAudioSink(def, fmt, this);
    if (audioDbg()) {
        connect(m_audioSink, &QAudioSink::stateChanged, this, [this](QAudio::State s) {
            qDebug() << "[audio] sink state =" << s;
            if (s == QAudio::StoppedState && m_audioSink)
                qDebug() << "[audio] sink error =" << m_audioSink->error();
        });
    }
    m_audioSink->start(m_audioFeed);
#else
    const QAudioDeviceInfo def = QAudioDeviceInfo::defaultOutputDevice();
    if (def.isNull()) {
        if (audioDbg()) qDebug() << "[audio] defaultOutputDevice nulo — sem saída de áudio";
        return;
    }
    m_audioOut = new QAudioOutput(def, fmt, this);
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
}

void PreviewWidget::updateFrame() {
    m_timeLabel->setText(fmtTimecode(m_playhead, projFps(m_project)));
    if (!m_project) { m_frame = QImage(); m_transAlpha = -1.0; m_underFrame = QImage(); update(); return; }

    const Clip* clip = clipAt(m_playhead);
    if (!clip) {
        m_frame = QImage();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        update();
        return;
    }

    const MediaItem* m = m_project->findMedia(clip->mediaId);
    if (!m || !m->hasVideo) {
        m_frame = QImage();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        update();
        return;
    }

    const double srcT = clip->in + (m_playhead - clip->pos) * clip->speed;
    // Decodifica no tamanho de exibição: muito mais rápido que 4K/1080p.
    const int decW = qMax(320, qMin(PreviewWidget::maxDecodeWidth(), m_videoRect.width() > 0
                                    ? m_videoRect.width() : 960));

    // Camadas inferiores (empilhamento multi-faixa): pede os quadros dos
    // clipes de vídeo que estão por baixo do topo, para compor transparência
    // e blend no preview (antes só o clipe do topo aparecia).
    requestLowerLayers(decW);

    // Cor sólida (gerador estilo Vegas): gera o quadro preenchido com a cor,
    // sem passar pelo decoder (não há arquivo).
    if (m->isSolid) {
        const int w = qMax(1, m->width > 0 ? m->width
                          : (m_project ? m_project->width : 1920));
        const int h = qMax(1, m->height > 0 ? m->height
                          : (m_project ? m_project->height : 1080));
        QImage img(w, h, QImage::Format_ARGB32);
        img.fill(m->solidColor);
        {
            QMutexLocker l(&m_frameMutex);
            m_frameFull = img;
            m_shownPath = QStringLiteral("solid:") + m->id;
            m_shownT = srcT;
            m_shownW = decW;
            m_lastSrcT = srcT;
            m_lastDecodeW = decW;
            m_lastFile = QString();
            m_prefetch.valid = false;
            m_prefetch.requested = false;
        }
        applyCrop();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
        update();
        return;
    }

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

    // Transição: procura o clipe anterior da MESMA faixa que se sobrepõe a
    // este (estilo Vegas). O quadro de trás é decodificado pelo canal de
    // prefetch e o paintEvent compõe dissolve/wipe durante a sobreposição.
    const Clip* under = nullptr;
    double overlap = 0.0;
    for (const Track& tr : m_project->videoTracks) {
        bool inTrack = false;
        for (const Clip& c : tr.clips)
            if (c.id == clip->id) { inTrack = true; break; }
        if (!inTrack) continue;
        for (const Clip& o : tr.clips)
            if (o.id != clip->id && o.pos < clip->pos - 1e-6
                && o.pos + o.dur > clip->pos + 1e-6
                && (!under || o.pos > under->pos))
                under = &o;
        break;
    }
    if (under) overlap = under->pos + under->dur - clip->pos;
    if (under && overlap > 1e-6 && m_playhead < clip->pos + overlap - 1e-9) {
        m_transAlpha = std::clamp((m_playhead - clip->pos) / overlap, 0.0, 1.0);
        m_transType = isTransition(under->transitionType)
                          ? under->transitionType
                          : QStringLiteral("dissolve");
        const double rel = m_playhead - under->pos;
        m_underCropL = (int)std::lround(
            std::clamp(kfValue(under->kfCropL, under->cropL, rel), 0.0, 0.9) * 1000.0);
        m_underCropR = (int)std::lround(
            std::clamp(kfValue(under->kfCropR, under->cropR, rel), 0.0, 0.9) * 1000.0);
        m_underCropT = (int)std::lround(
            std::clamp(kfValue(under->kfCropT, under->cropT, rel), 0.0, 0.9) * 1000.0);
        m_underCropB = (int)std::lround(
            std::clamp(kfValue(under->kfCropB, under->cropB, rel), 0.0, 0.9) * 1000.0);
        const MediaItem* um = m_project->findMedia(under->mediaId);
        if (um && um->hasVideo && m_frameWorker) {
            const double uSrcT = under->in + (m_playhead - under->pos) * under->speed;
            QMutexLocker l(&m_frameMutex);
            const bool already = m_underRequested && m_underPath == um->filePath
                                 && m_underW == decW
                                 && std::fabs(m_underT - uSrcT) <= 0.5 / projFps(m_project)
                                 && !m_underFrame.isNull();
            if (!already) {
                m_underPath = um->filePath;
                m_underT = uSrcT;
                m_underW = decW;
                m_underRequested = true;
                m_underFrame = QImage();
                QMetaObject::invokeMethod(m_frameWorker, "decodePrefetch", Qt::QueuedConnection,
                                          Q_ARG(QString, um->filePath),
                                          Q_ARG(double, uSrcT),
                                          Q_ARG(int, decW));
            }
        }
    } else {
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
    }

    // Se temos um quadro pré-carregado que bate com a posição de entrada do novo clipe, exibe imediatamente.
    bool usedPrefetch = false;
    if (m_transAlpha < 0.0) {
        QMutexLocker l(&m_frameMutex);
        const double frameDur = 1.0 / projFps(m_project);
        if (m_prefetch.valid && m_prefetch.path == m->filePath
            && std::fabs(m_prefetch.t - srcT) <= frameDur * 0.5
            && m_prefetch.maxW == decW) {
            m_frameFull = m_prefetch.img;
            m_shownPath = m->filePath;
            m_shownT = srcT;
            m_shownW = decW;
            m_lastSrcT = srcT;
            m_lastDecodeW = decW;
            m_lastFile = m->filePath;
            m_prefetch.valid = false;
            m_prefetch.requested = false;
            usedPrefetch = true;
            applyCrop();
            update();
        }
    }

    // O quadro do tamanho/posição atuais já está pronto? Apenas reaplica o
    // pan/crop (por exemplo quando só o corte mudou) sem decodificar de novo.
    {
        QMutexLocker l(&m_frameMutex);
        if (!usedPrefetch && m_shownPath == m->filePath && std::fabs(m_shownT - srcT) < 1e-6
            && m_shownW == decW) {
            applyCrop();
            update();
            return;
        }
    }
    if (usedPrefetch) {
        // Já no tick do corte, dispara o próximo frame: o decoder trocado está
        // posicionado e decodifica adiante, evitando "segurar" o frame do corte.
        requestFrame(clip->id, m->filePath, srcT + 1.0 / projFps(m_project), decW);
        return;
    }
    requestFrame(clip->id, m->filePath, srcT, decW);
}

// Pedido "assíncrono": a decodificação acontece na thread do FrameWorker.
// Vários pedidos seguidos entram numa fila; apenas um roda por vez
// (scrub não empilha — a fila só guarda pedidos ainda não enviados).
void PreviewWidget::requestFrame(const QString& clipId, const QString& path, double t, int maxW) {
    QMutexLocker l(&m_frameMutex);
    for (const FrameReq& r : m_reqQueue)
        if (r.clipId == clipId && r.path == path
            && std::fabs(r.t - t) < 1e-6 && r.maxW == maxW)
            return; // já enfileirado
    m_reqQueue.append({clipId, path, t, 1.0 / projFps(m_project), maxW});
    kickFrameWorker();
}

// Pedidos os quadros dos clipes de vídeo ativos no playhead que ficam POR
// BAIXO do clipe do topo (empilhamento de faixas). Quadros já em cache
// (mesmo arquivo/tempo/tamanho) são pulados.
void PreviewWidget::requestLowerLayers(int decW) {
    if (!m_project) return;
    const Clip* top = clipAt(m_playhead);
    // Camadas que deixaram de estar ativas no playhead saem do cache (evita
    // crescimento sem limite conforme o usuário navega pelo projeto).
    {
        QList<QString> active;
        for (const Track& tr : m_project->videoTracks)
            for (const Clip& cl : tr.clips)
                if (!cl.isText && m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur)
                    active.append(cl.id);
        QMutexLocker l(&m_frameMutex);
        for (auto it = m_layerCache.begin(); it != m_layerCache.end();) {
            if (!active.contains(it.key())) it = m_layerCache.erase(it);
            else ++it;
        }
    }
    for (const Track& tr : m_project->videoTracks) {
        const Clip* c = nullptr;
        for (const Clip& cl : tr.clips) {
            if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                const MediaItem* mm = m_project->findMedia(cl.mediaId);
                if (mm && mm->hasVideo && (!c || cl.pos > c->pos)) c = &cl;
            }
        }
        if (!c || (top && c->id == top->id)) continue;
        const MediaItem* m = m_project->findMedia(c->mediaId);
        if (!m || !m->hasVideo) continue;
        // Cor sólida não tem arquivo: é gerada na pintura, não pede decode.
        if (m->isSolid) continue;
        const double srcT = c->in + (m_playhead - c->pos) * c->speed;
        {
            QMutexLocker l(&m_frameMutex);
            const auto it = m_layerCache.constFind(c->id);
            if (it != m_layerCache.constEnd() && it->path == m->filePath
                && std::fabs(it->t - srcT) < 1e-6 && it->maxW == decW && !it->img.isNull())
                continue; // já em cache
        }
        requestFrame(c->id, m->filePath, srcT, decW);
    }
}

// Chamado com m_frameMutex segurado.
void PreviewWidget::kickFrameWorker() {
    if (m_workerBusy || m_reqQueue.isEmpty() || !m_frameWorker) return;
    m_workerBusy = true;
    const FrameReq r = m_reqQueue.takeFirst();
    QMetaObject::invokeMethod(m_frameWorker, "decodeOne", Qt::QueuedConnection,
                              Q_ARG(QString, r.clipId), Q_ARG(QString, r.path),
                              Q_ARG(double, r.t), Q_ARG(int, r.maxW), Q_ARG(double, r.dt));
}

// Crop (pan/crop) do clipe no instante `rel` da timeline.
static void clipCrop(const Clip& c, double rel, int& cL, int& cR, int& cT, int& cB) {
    cL = (int)std::lround(std::clamp(kfValue(c.kfCropL, c.cropL, rel), 0.0, 0.9) * 1000.0);
    cR = (int)std::lround(std::clamp(kfValue(c.kfCropR, c.cropR, rel), 0.0, 0.9) * 1000.0);
    cT = (int)std::lround(std::clamp(kfValue(c.kfCropT, c.cropT, rel), 0.0, 0.9) * 1000.0);
    cB = (int)std::lround(std::clamp(kfValue(c.kfCropB, c.cropB, rel), 0.0, 0.9) * 1000.0);
}

void PreviewWidget::onFrameReady(const QString& clipId, const QString& path, double t, int maxW, const QImage& img) {
    {
        QMutexLocker l(&m_frameMutex);
        m_workerBusy = false;
        kickFrameWorker(); // continua com o próximo pedido, se houver
    }
    if (img.isNull() || !m_project) return;

    const Clip* clip = nullptr;
    for (const Track& tr : m_project->videoTracks)
        for (const Clip& c : tr.clips)
            if (c.id == clipId) { clip = &c; break; }
    if (!clip) return;
    const MediaItem* m = m_project->findMedia(clip->mediaId);
    if (!m || m->filePath != path) return;

    // Ignora quadros decodificados para outra posição (scrub/seek rápido muito distante).
    const double wantT = clip->in + (m_playhead - clip->pos) * clip->speed;
    if (std::fabs(wantT - t) > 1.5) return;

    // Quadro do clipe do TOPO: caminho atual do preview (com prefetch/pan-crop).
    const Clip* top = clipAt(m_playhead);
    if (composeDbg())
        qDebug() << "[compose] frameReady clipId=" << clipId
                 << "path=" << path << "t=" << t
                 << "top=" << (top ? top->id : QString())
                 << "isTop=" << (top && top->id == clipId);
    if (top && top->id == clipId) {
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
        return;
    }

    // Quadro de uma camada INFERIOR: guarda no cache de camadas (já cortado).
    const double rel = m_playhead - clip->pos;
    int cL, cR, cT, cB;
    clipCrop(*clip, rel, cL, cR, cT, cB);
    const QImage cropped = applyCropTo(img, cL, cR, cT, cB);
    {
        QMutexLocker l(&m_frameMutex);
        m_layerCache[clipId] = {cropped, path, t, maxW};
    }
    update();
}

void PreviewWidget::onPrefetchReady(const QString& path, double t, int maxW, const QImage& img) {
    QMutexLocker l(&m_frameMutex);
    // Quadro do clipe de trás (transição ativa).
    if (m_underRequested && m_underPath == path
        && std::fabs(m_underT - t) < 1e-4 && m_underW == maxW) {
        m_underFrame = img.isNull() ? QImage() : applyCropTo(img, m_underCropL, m_underCropR,
                                                             m_underCropT, m_underCropB);
    }
    if (m_prefetch.requested && m_prefetch.path == path
        && std::fabs(m_prefetch.t - t) < 1e-4 && m_prefetch.maxW == maxW) {
        m_prefetch.img = img;
        m_prefetch.valid = !img.isNull();
    }
}

void PreviewWidget::updatePrefetch() {
    if (!m_project || !m_frameWorker || !m_playing) return;
    // Durante uma transição o canal de prefetch decodifica o clipe de trás.
    if (m_transAlpha >= 0.0) return;
    const Clip* clip = clipAt(m_playhead);
    if (!clip) {
        QMutexLocker l(&m_frameMutex);
        m_prefetch.valid = false;
        m_prefetch.requested = false;
        return;
    }
    const double remain = (clip->pos + clip->dur) - m_playhead;
    if (remain > 1.0 || remain <= 0.0) return;

    // Procura o próximo clipe que será exibido no fim do clipe atual
    const Clip* nextClip = nullptr;
    double nextPos = 1e9;
    for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
        for (const Clip& c : m_project->videoTracks[tr].clips) {
            if (c.pos >= clip->pos + clip->dur - 1e-4 && c.pos < nextPos) {
                nextPos = c.pos;
                nextClip = &c;
            }
        }
    }
    if (!nextClip) return;

    const MediaItem* nextMedia = m_project->findMedia(nextClip->mediaId);
    if (!nextMedia || !nextMedia->hasVideo) return;

    const double srcT = nextClip->in;
    const int decW = qMax(320, qMin(PreviewWidget::maxDecodeWidth(), m_videoRect.width() > 0
                                    ? m_videoRect.width() : 960));

    {
        QMutexLocker l(&m_frameMutex);
        if (m_prefetch.requested && m_prefetch.path == nextMedia->filePath
            && std::fabs(m_prefetch.t - srcT) < 1e-4 && m_prefetch.maxW == decW) {
            return; // já solicitado ou já pronto
        }
        m_prefetch.path = nextMedia->filePath;
        m_prefetch.t = srcT;
        m_prefetch.maxW = decW;
        m_prefetch.img = QImage();
        m_prefetch.valid = false;
        m_prefetch.requested = true;
    }

    QMetaObject::invokeMethod(m_frameWorker, "decodePrefetch", Qt::QueuedConnection,
                              Q_ARG(QString, nextMedia->filePath),
                              Q_ARG(double, srcT),
                              Q_ARG(int, decW));
}

// Aplica pan/crop sobre um quadro e devolve o recorte.
QImage PreviewWidget::applyCropTo(const QImage& img, int cL, int cR, int cT, int cB) {
    const double w = img.width();
    const double h = img.height();
    if (w <= 1 || h <= 1 || (!cL && !cR && !cT && !cB)) return img;
    const int x = (int)std::lround(w * cL / 1000.0);
    const int y = (int)std::lround(h * cT / 1000.0);
    const int cw = (int)std::lround(w * (1.0 - (cL + cR) / 1000.0));
    const int ch = (int)std::lround(h * (1.0 - (cT + cB) / 1000.0));
    return img.copy(QRect(std::clamp(x, 0, (int)w - 1), std::clamp(y, 0, (int)h - 1),
                          qMin(cw, (int)w - std::clamp(x, 0, (int)w - 1)),
                          qMin(ch, (int)h - std::clamp(y, 0, (int)h - 1))));
}

// Aplica pan/crop sobre o quadro cheio (m_frameFull) e guarda em m_frame.
void PreviewWidget::applyCrop() {
    m_frame = applyCropTo(m_frameFull, m_lastCropL, m_lastCropR, m_lastCropT, m_lastCropB);
}

#include "PreviewWidget.moc"
