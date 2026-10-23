// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/monitor/PreviewWidget.h"
#include <cstdint>

#include "ui/dialogs/SettingsDialog.h"
#include "ui/timeline/TlLog.h"
#include "colombina/CrashReporter.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/ffmpeg/AudioConformCache.h"
#include "colombina/audio/EqBiquad.h"
#include "colombina/ofx/OfxRenderer.h"
#include "colombina/ofx/OfxPluginManager.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "colombina/fx/ColorGrade.h"
#include "ui/monitor/ScopeWidget.h"
#include "colombina/export/LainkaFx.h"
#include "ui/core/Theme.h"
#include "ui/monitor/MonitorUtils.h"
#include "colombina/generators.h"
#include "colombina/render/TextRender.h"
// DSP dos efeitos de áudio (AudioFx/GraphicEq/ParametricEqDsp) e os helpers
// de diagnóstico/config do preview (playDbg/audioDbg/composeDbg/
// previewAudioFormat/desengasgaIntervalMs) — vidrados em
// headers próprios, compartilhados com FrameWorker/FramePipeline/AudioMixer.
#include "ui/audio/AudioFxDsp.h"
#include "ui/audio/AudioMixer.h"
#include "ui/monitor/PreviewDebug.h"
#include "colombina/ffmpeg/SourcePool.h"
#include "colombina/ffmpeg/FramePipeline.h"
#include "laartman/core/DecoderMetrics.h"
#include "ui/monitor/FrameWorker.h"
#include "ui/monitor/MonitorScrubBar.h"

#include <QPainter>
#include <QPixmap>
#include <QPolygon>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QPushButton>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QLabel>
#include <QComboBox>
#include <QSignalBlocker>
#include <QPointer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QShortcut>
#include <QIODevice>
#include <QAudioFormat>
#include <QPainterPath>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSettings>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioSink>
#include <QMediaDevices>
#else
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#endif
#include <algorithm>
#include <atomic>
#include <QDebug>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <utility>
#include <QHash>
#include <QMutex>


#include "ui/monitor/PreviewPriv.h"

void PreviewWidget::seek(double t) {
    PlaybackEngine::seek(t);
}

// Quadro a quadro (Premiere: os botões ◀▶ do monitor). Pausa a reprodução
// (senão o seek brigaria com o relógio do playback) e busca ±1 quadro.
void PreviewWidget::stepFrame(int dir) {
    if (!m_project || m_project->duration() <= 0) return;
    if (m_playing) togglePlay();
    const double fps = projFps(m_project);
    const double t = m_playhead + (dir > 0 ? 1.0 : -1.0) / fps;
    seek(std::clamp(t, 0.0, m_project->duration()));
}

// Slot público do passo a passo, para os botões ◀▶ da janela de preview externo.
void PreviewWidget::stepFrameBy(int dir) {
    stepFrame(dir);
}

void PreviewWidget::onSeek(double t) {
    applySeekVisual(t);
}

// Intervalo do "desengasgo" (flush periódico dos buffers do decoder): vidrado
// em `desengasgaIntervalMs()` (PreviewDebug.h) — inclui a medição que justifica
// o padrão desligado (0).
//
// Decodifica e desenha o quadro na posição `t` (o transporte/relógio é do
// PlaybackEngine; aqui só a parte visual/decodificação).
void PreviewWidget::applySeekVisual(double t) {
    Q_UNUSED(t);
    updateTimecodeLabel();
    if (m_scrubBar) m_scrubBar->update();

    // Desengasgo periódico: a cada ~10s durante playback contínuo, libera os
    // buffers internos do decoder (DPB + caches de 2 frames) que degradam o
    // frameAt em vídeos longos — a causa do vídeo engasgar com áudio perfeito.
    // O flush é barato (releaseBuffers não fecha o arquivo; o próximo pedido
    // faz re-seek) e roda no worker thread, seriado com os decodeOne.
    if (m_playing && m_frameWorker && desengasgaIntervalMs() > 0) {
        const qint64 nowMs = m_desengasgaT.isValid()
                                 ? m_desengasgaT.elapsed() : 0;
        if (!m_desengasgaT.isValid()) {
            m_desengasgaT.start();
            m_desengasgaLastMs = 0;
        } else if (nowMs - m_desengasgaLastMs >= desengasgaIntervalMs()) {
            // Medido numa fonte 1080p60 (H.264 High@4.2, 252 s, 13 Mbps):
            // cada flush custa um re-seek frio de 114-304 ms, ou seja, 4
            // travamentos de ~0,25 s em 120 s de reprodução — exatamente a cada
            // 30 s. Baixar de 10s para 30s só dilatou o custo, não o reduziu.
            // E o problema que ele existia para evitar não aparece: com o flush
            // desligado o p95 de decode ficou plano (0,35-0,37 ms) nos 4
            // minutos, sem degradação. Por isso o intervalo virou configurável
            // (PIERROT_DESENGASGA_MS=0) em vez de fixo — desligar é o padrão
            // melhor medido, mas se algum dia o engasgo reaparecer em outra
            // fonte, basta ligar de volta por env var.
            m_desengasgaLastMs = nowMs;
            QMetaObject::invokeMethod(m_frameWorker, "desengasga", Qt::QueuedConnection);
        }
    }
    updateFrame();
    // Agulha parada: aquece o clipe sob ela para o próximo play sair quente.
    scheduleIdleWarm();
    update();
}

double PreviewWidget::audioClockSec() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (m_audioSink) return m_audioSink->processedUSecs() / 1.0e6;
#else
    if (m_audioOut) return m_audioOut->processedUSecs() / 1.0e6;
#endif
    return -1.0;
}

void PreviewWidget::togglePlay() {
    PlaybackEngine::togglePlay();
}

void PreviewWidget::shuttle(int dir) {
    PlaybackEngine::shuttle(dir);
}

void PreviewWidget::playFrom(double t) {
    PlaybackEngine::playFrom(t);
}

void PreviewWidget::setLoopRange(double in, double out) {
    PlaybackEngine::setLoopRange(in, out);
    if (m_scrubBar) m_scrubBar->update(); // região de loop sombreada
}

void PreviewWidget::setLoopEnabled(bool enabled) {
    PlaybackEngine::setLoopEnabled(enabled);
    if (m_scrubBar) m_scrubBar->update();
}

// Hooks do PlaybackEngine: o transporte decide quando; aqui a PreviewWidget
// faz o áudio e o rendering.

void PreviewWidget::onStartAudio(double t) { startAudio(t); }
void PreviewWidget::onStopAudio() { stopAudio(); }
void PreviewWidget::onMixAudio(double t, bool reseek) { updateMixAudio(t, reseek); }
void PreviewWidget::onPrefetch() { updatePrefetch(); }

// Limpa buffers de frame quando o transporte para (sem duplicar o estado do
// PlaybackEngine, que já zera o relógio).
void PreviewWidget::onStopPlaybackUI() {
    QMutexLocker l(&m_frameMutex);
    m_prefetch = PrefetchFrame();
}

void PreviewWidget::onPlayheadMoved(double t) {
    updateTimecodeLabel();
    if (m_scrubBar) m_scrubBar->update();
    emit playheadMoved(t);
}

void PreviewWidget::onStateChanged(bool playing) {
    // O PlaybackEngine troca o rótulo do botão a cada transição; como ele está
    // em modo ícone (estilo Premiere), restauramos o ícone e limpamos o texto.
    if (m_playBtn) {
        m_playBtn->setIcon(playing ? m_pauseIcon : m_playIcon);
        m_playBtn->setText(QString());
    }
    updateTimecodeLabel();
    // Marco do play para medir "apertar play -> 1º quadro" (o log sai em
    // onFrameReady, com PIERROT_PLAY_DEBUG=1).
    m_playStartNs = playing ? PreviewProfiler::nowNs() : 0;
    emit stateChanged(playing);
}

void PreviewWidget::setZoom(double z) {
    m_zoom = z;
    if (m_zoomCombo) {
        int idx = 0;
        for (int i = 0; i < 7; ++i) {
            if (std::fabs(kMonitorZooms[i] - z) < 1e-6) { idx = i; break; }
        }
        QSettings().setValue(QStringLiteral("monitor/zoom"), idx);
        QSignalBlocker b(m_zoomCombo);
        m_zoomCombo->setCurrentIndex(idx);
    }
    update();
}

void PreviewWidget::setPreviewQuality(int width) {
    m_previewQuality = width;
    QSettings().setValue(QStringLiteral("monitor/quality"), width);
    // Se o usuário mudou manualmente, reseta o estado adaptativo.
    m_adaptiveSlowCount = 0;
    m_adaptiveActive = false;
    m_adaptiveDecodeMsAvg = 0.0;
    // Invalida o quadro atual e força a reconversão na nova resolução.
    {
        QMutexLocker l(&m_frameMutex);
        if (!m_frame.isNull()) m_frame = QImage();
    }
    m_shownW = -1;
    m_lastDecodeW = -1;
    for (QAction* a : m_qualityMenu->actions())
        a->setChecked(a->data().toInt() == width);
    for (const PreviewQOpt& o : kPreviewQualities)
        if (o.width == width) {
            if (m_qualityBtn) m_qualityBtn->setText(QString::fromUtf8(o.code));
            break;
        }
    update();
    updateFrame();
}

bool PreviewWidget::tryRenderMesa(const Clip* clip) {
    if (!m_project || !clip) return false;

    // Encontra a faixa do clipe ativo.
    const Track* track = nullptr;
    for (const Track& tr : m_project->videoTracks) {
        for (const Clip& c : tr.clips)
            if (c.id == clip->id) { track = &tr; break; }
    }
    if (!track) return false;

    // A Mesa dona da track é descoberta via mesa.trackIds (fonte da verdade,
    // a mesma usada pelo MesaWidget/MesaRenderer). O caminho antigo
    // track.groupId→group.mesaId quebrava quando a pasta da Mesa era
    // excluída/desagrupada: a composição sumia do preview ao reabrir.
    const MesaComposition* mc = m_project->findMesaForTrack(track->id);
    if (!mc) return false;

    // Renderiza o canvas + transform de câmera (saída no tamanho do projeto).
    const QImage out = m_mesaRenderer.render(*mc, *m_project, m_playhead);
    if (out.isNull()) return false;

    {
        QMutexLocker l(&m_frameMutex);
        m_frameFull = out;
        m_shownPath = QStringLiteral("mesa:") + mc->id;
        m_shownT = m_playhead;
        m_shownW = m_project->width;
        m_lastSrcT = m_playhead;
        m_lastDecodeW = m_project->width;
        m_lastFile.clear();
        m_prefetch.valid = false;
        m_prefetch.requested = false;
    }
    // Nenhum efeito de clipe deve ser aplicado à composição da Mesa.
    m_clipLainkaEnabled = false;
    m_clipOfxFx.clear(); m_clipMotionEnabled = false; m_clipFrei0rFx.clear();
    m_clipMasks.clear();
    applyCrop();
    return true;
}


