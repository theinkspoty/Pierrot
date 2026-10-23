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

void PreviewWidget::prewarmAudioSink() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (m_audioSink) {
        const QAudioDevice def = QMediaDevices::defaultAudioOutput();
        if (def.isNull() || m_audioSinkDevice == QString::fromUtf8(def.id())) return;
    }
    const QAudioDevice def = QMediaDevices::defaultAudioOutput();
    if (def.isNull()) return;
    const QString devId = QString::fromUtf8(def.id());
    if (m_audioSink) {
        m_audioSink->stop();
        m_audioSink->deleteLater();
        m_audioSink = nullptr;
    }
    // Primeira criação = inicializa o backend de áudio do Qt (~1s). Paga aqui,
    // ocioso, em vez de travar o 1º play.
    m_audioSink = new QAudioSink(def, previewAudioFormat(), this);
    m_audioSinkDevice = devId;
#else
    if (m_audioOut) {
        const QAudioDeviceInfo def = QAudioDeviceInfo::defaultOutputDevice();
        if (def.isNull() || m_audioOutDevice == def.deviceName()) return;
    }
    const QAudioDeviceInfo def = QAudioDeviceInfo::defaultOutputDevice();
    if (def.isNull()) return;
    const QString devId = def.deviceName();
    if (m_audioOut) {
        m_audioOut->stop();
        m_audioOut->deleteLater();
        m_audioOut = nullptr;
    }
    m_audioOut = new QAudioOutput(def, previewAudioFormat(), this);
    m_audioOutDevice = devId;
#endif
}

void PreviewWidget::startAudio(double t) {
    if (!m_project) return;
    QElapsedTimer tStage;
    tStage.start();
    stopAudio();
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

    const QAudioFormat fmt = previewAudioFormat();

    m_audioFeed = new AudioMixer(this);
    m_audioFeed->setMasterVolume(m_project ? m_project->masterVolume : 1.0);
    if (m_project) {
        m_audioFeed->setMasterGraphicEq(m_project->masterGraphicEq);
        m_audioFeed->setMasterParametricEq(m_project->masterParametricEq);
    }
    m_audioFeed->open(QIODevice::ReadOnly | QIODevice::Unbuffered);

    // O mixer lê SÓ do conform (PCM já decodificado em background) → está
    // sempre "pronto": não há decoders a abrir, então o sink já começa. Faltas
    // de trecho são silêncio momentâneo até o worker terminar a janela pedida.
    // Nada de waitReadyBeforeSink() aqui: ele segurava a UI por até 150ms no
    // play (C1 do relatório). `updateSources` já dispara a conform em
    // background; aceitamos 1-2 frames de silêncio no warm-up frio.
    // Inclui o lookahead (mesmo se o play começa colado num corte): o sink
    // enche o buffer à frente do playhead e um clipe curto que começa logo
    // precisa já estar no mix para não ser pulado.
    const QVector<AudioMixer::SourceInfo> warm =
        buildWarmSources(m_project, t, sources);
    m_audioFeed->updateSources(sources, /*reseek=*/true, warm, -1, t);
    const qint64 afterSourcesMs = tStage.elapsed();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QAudioDevice def = QMediaDevices::defaultAudioOutput();
    if (def.isNull()) {
        if (audioDbg()) qDebug() << "[audio] defaultAudioOutput nulo — sem saída de áudio";
        return;
    }
    // Reusa o sink entre play/stop (C4): new/destroy custa 10-50ms por play.
    // Recria só se a saída padrão mudou.
    const QString devId = QString::fromUtf8(def.id());
    if (!m_audioSink || m_audioSinkDevice != devId) {
        if (m_audioSink) {
            m_audioSink->stop();
            m_audioSink->deleteLater();
            m_audioSink = nullptr;
        }
        m_audioSink = new QAudioSink(def, fmt, this);
        m_audioSinkDevice = devId;
    }
    m_audioSink->start(m_audioFeed);
#else
    const QAudioDeviceInfo def = QAudioDeviceInfo::defaultOutputDevice();
    if (def.isNull()) {
        if (audioDbg()) qDebug() << "[audio] defaultOutputDevice nulo — sem saída de áudio";
        return;
    }
    const QString devId = def.deviceName();
    if (!m_audioOut || m_audioOutDevice != devId) {
        if (m_audioOut) {
            m_audioOut->stop();
            m_audioOut->deleteLater();
            m_audioOut = nullptr;
        }
        m_audioOut = new QAudioOutput(def, fmt, this);
        m_audioOutDevice = devId;
    }
    m_audioOut->start(m_audioFeed);
#endif
    if (playDbg())
        qDebug().noquote()
            << QStringLiteral("[play] startAudio: updateSources=%1ms sink=%2ms")
                   .arg(afterSourcesMs)
                   .arg(tStage.elapsed() - afterSourcesMs);
}

void PreviewWidget::stopAudio() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (m_audioSink) {
        // Não destrói: o sink é reutilizado no próximo play (C4). `start()`
        // troca a fonte para o novo m_audioFeed. O `deleteLater` da antiga
        // fonte é seguro porque o sink já foi parado e largou a referência.
        m_audioSink->stop();
    }
#else
    if (m_audioOut) {
        m_audioOut->stop();
    }
#endif
    if (m_audioFeed) {
        // shutdown() síncrono: bloqueia novos updates e libera as fontes antes
        // do mixer ser descartado. As caches do conform continuam no registro.
        m_audioFeed->shutdown();
        m_audioFeed->deleteLater();
        m_audioFeed = nullptr;
    }
}

// Recalcula as fontes de áudio ativas no instante `t` e empurra ao mixer.
// Com o conform, tudo aqui é barato e síncrono (sem open/seekAudio no caminho):
// updateSources só troca ponteiros de leitura e emite pedidos de conform para
// o worker — que decodifica em background, em região por região.
void PreviewWidget::updateMixAudio(double t, bool reseek) {
    if (!m_audioFeed) return;
    m_audioFeed->setMasterVolume(m_project ? m_project->masterVolume : 1.0);
    if (m_project) {
        m_audioFeed->setMasterGraphicEq(m_project->masterGraphicEq);
        m_audioFeed->setMasterParametricEq(m_project->masterParametricEq);
    }
    const QVector<AudioMixer::SourceInfo> sources = buildMixSources(m_project, t);
    const QVector<AudioMixer::SourceInfo> warm =
        buildWarmSources(m_project, t, sources);
    m_audioFeed->updateSources(sources, reseek, warm, -1, t);
}

