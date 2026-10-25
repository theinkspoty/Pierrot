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
using namespace LainkaFx;

void PreviewWidget::updateFrame() {
    updateTimecodeLabel();
    if (!m_project) { m_frame = QImage(); m_transAlpha = -1.0; m_underFrame = QImage(); m_underRequested = false; m_underPending = false; m_clipMasks.clear(); update(); return; }

    const Clip* clip = clipAt(m_playhead);
    if (!clip) {
        m_frame = QImage();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
        m_underPending = false;
        m_clipLainkaEnabled = false;
        m_clipOfxFx.clear(); m_clipMotionEnabled = false; m_clipFrei0rFx.clear();
    m_clipMasks.clear();
        update();
        return;
    }

    // Clipe pertence a um grupo Mesa → renderiza a composição inteira
    // (todas as camadas), sem a transform de câmera.
    if (tryRenderMesa(clip)) {
        // Mesmo com Mesa no topo, tracks inferiores precisam ser decodificadas
        // para que paintEvent possa compô-las por baixo (transparência, blend).
        const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
        const int decW = qMax(160, qMax(m_previewQuality, widgetW));
        requestLowerLayers(decW);
        m_underFrame = QImage();
        m_underRequested = false;
        m_underPending = false;
        m_transAlpha = -1.0;
        m_clipLainkaEnabled = false;
        m_clipOfxFx.clear(); m_clipMotionEnabled = false; m_clipFrei0rFx.clear();
    m_clipMasks.clear();
        update();
        return;
    }

    const MediaItem* m = m_project->findMedia(clip->mediaIdAt(m_playhead - clip->pos));
    if (!m || !m->hasVideo) {
        m_frame = QImage();
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
        m_underPending = false;
        m_clipLainkaEnabled = false;
        m_clipOfxFx.clear(); m_clipMotionEnabled = false; m_clipFrei0rFx.clear();
    m_clipMasks.clear();
        update();
        return;
    }

    // Captura estado do LAINKA para uso em applyCrop().
    m_clipLainkaEnabled = clip->lainkaEnabled;
    m_clipLainkaSkip = clip->lainkaSkip;
    m_clipLainkaJitterPos = clip->lainkaJitterPos;
    m_clipLainkaJitterRot = clip->lainkaJitterRot;
    m_clipLainkaJitterScale = clip->lainkaJitterScale;
    m_clipLainkaFlicker = clip->lainkaFlicker;
    m_clipLainkaFlickerSpeed = clip->lainkaFlickerSpeed;
    m_clipLainkaWarpAmount = clip->lainkaWarpAmount;
    m_clipLainkaWarpSpeed = clip->lainkaWarpSpeed;
    m_clipLainkaWarpGrid = clip->lainkaWarpGrid;
    m_clipLainkaOnionSkin = clip->lainkaOnionSkin;
    m_clipLainkaDustAmount = clip->lainkaDustAmount;
    m_clipLainkaScratchAmount = clip->lainkaScratchAmount;
    m_clipLainkaTargetFps = clip->lainkaTargetFps;
    m_clipLainkaMotionBlur = clip->lainkaMotionBlur;
    m_clipLainkaOpacity = clip->lainkaOpacity;
    m_clipLainkaAntialias = clip->lainkaAntialias;
    m_clipLainkaHold = clip->lainkaHold;
    m_clipLainkaStutter = clip->lainkaStutter;
    m_clipLainkaPosterize = clip->lainkaPosterize;
    m_clipLainkaColorShift = clip->lainkaColorShift;
    m_clipLainkaGate = clip->lainkaGate;
    m_clipLainkaId = clip->id;
    m_clipMotionEnabled = clip->motionEnabled;
    m_clipMotionAmount = clip->motionAmount;
    m_clipMotionAngle = clip->motionAngle;
    m_clipMotionSamples = clip->motionSamples;
    m_clipBrightness = clip->brightness;
    m_clipContrast = clip->contrast;
    m_clipSaturation = clip->saturation;
    m_clipBlur = clip->blur;
    m_clipGrayscale = clip->grayscale;
    m_clipChromaKey = clip->chromaKey;
    m_clipChromaKeyColor = clip->chromaKeyColor;
    m_clipChromaKeySimilarity = clip->chromaKeySimilarity;
    m_clipChromaKeySoftness = clip->chromaKeySoftness;
    m_clipChromaKeySpillSuppress = clip->chromaKeySpillSuppress;
    m_clipPsxEnabled = clip->psxEnabled;
    m_clipPsxDither = clip->psxDither;
    m_clipPsxBits = clip->psxBits;
    m_clipFilmGrain = clip->filmGrain;
    m_clipFilmGrainAmount = clip->filmGrainAmount;
    m_clipFilmGrainSize = clip->filmGrainSize;
    m_clipOfxFx = clip->ofxFx;
    m_clipFrei0rFx = clip->frei0rFx;
    m_clipMasks = clip->masks;

    double srcT = clipSrcTime(*clip, m_playhead - clip->pos);

    // LAINKA: quantiza o tempo para simular stop motion.
    // Usa targetFps para calcular o skip (projectFps / targetFps).
    m_lainkaQuantizedTime = -1.0;
    if (clip->lainkaEnabled) {
        const double pfps = projFps(m_project);
        const int effSkip = (clip->lainkaTargetFps > 0 && pfps > 1.0)
            ? std::max(1, (int)std::lround(pfps / clip->lainkaTargetFps))
            : clip->lainkaSkip;
        if (effSkip > 1) {
            srcT = lainkaQuantizeTime(srcT, effSkip, pfps);
            m_lainkaQuantizedTime = srcT;
        }
    }
    // Decodifica na maior entre a qualidade escolhida e a largura exibida:
    // nunca abaixo do tamanho de tela (evita pixelização ao esticar), e nem
    // além do que o monitor precisa, exceto quando a qualidade pede mais.
    const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
    const int decW = qMax(160, qMax(m_previewQuality, widgetW));

    // Camadas inferiores (empilhamento multi-faixa): pede os quadros dos
    // clipes de vídeo que estão por baixo do topo, para compor transparência
    // e blend no preview (antes só o clipe do topo aparecia).
    requestLowerLayers(decW);

    // Mídia gerada (gerador estilo Vegas): gera o quadro (cor, gradiente,
    // checkerboard ou ruído), sem passar pelo decoder (não há arquivo).
    if (m->isSolid) {
        const int w = qMax(1, m->width > 0 ? m->width
                          : (m_project ? m_project->width : 1920));
        const int h = qMax(1, m->height > 0 ? m->height
                          : (m_project ? m_project->height : 1080));
        QImage img = generatedFrameCached(*m, w, h);
        {
            QMutexLocker l(&m_frameMutex);
            m_frameFull = img;
            m_shownPath = QStringLiteral("solid:") + m->id;
            // Posse: mídia gerada não passa pelo worker (handleFrameReady) —
            // sem este carimbo, a guarda de posse do fast-path derrubava o
            // quadro do gerador (carência de 250 ms e depois PRETO fixo).
            m_shownClipId = clip->id;
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
        m_underPending = false;
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
    // A transição não pode durar mais que o clipe da frente (paridade com o
    // clamp do export): se a sobreposição passar da duração dele, a transição
    // completa no fim do clipe em vez de nunca chegar a 100%.
    if (under)
        overlap = std::min(under->pos + under->dur - clip->pos, clip->dur);
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
            const QString uvpath = resolvePreviewVideo(um->filePath);
            const double uSrcT = clipSrcTime(*under, m_playhead - under->pos);
            QMutexLocker l(&m_frameMutex);
            // Mesmo alvo (clipe/ largura / tempo) que o quadro atual.
            const bool sameTarget = m_underRequested && m_underPath == uvpath
                                    && m_underW == decW
                                    && std::fabs(m_underT - uSrcT)
                                       <= 0.5 / projFps(m_project);
            if (m_underPending) {
                // Já há um decode em voo: não emite outro. O resultado em
                // onPrefetchReady limpa o flag; se o playhead andou muito
                // (sameTarget falso), o próximo tick re-mira o alvo.
            } else if (sameTarget && !m_underFrame.isNull()) {
                // Quadro atual serve (até ~0.5 frame de atraso): nada a fazer.
            } else {
                // Numa transição longa o playhead avança a cada frame, então o
                // alvo (uSrcT) muda continuamente. Zerar o quadro aqui fazia o
                // clipe de trás desaparecer até o decode chegar → "piscada" no
                // meio do dissolve. Mantém o quadro anterior na tela enquanto o
                // novo é decodificado; só descarta quando o alvo muda de clipe
                // (path ou largura), aí o quadro antigo não serve mais.
                if (m_underPath != uvpath || m_underW != decW)
                    m_underFrame = QImage();
                m_underPath = uvpath;
                m_underT = uSrcT;
                m_underW = decW;
                m_underCropL = m_underCropR = m_underCropT = m_underCropB = 0; // recortado no onFrame
                m_underRequested = true;
                m_underPending = true;
                QMetaObject::invokeMethod(m_frameWorker, "decodePrefetch", Qt::QueuedConnection,
                                          Q_ARG(QString, uvpath),
                                          Q_ARG(double, uSrcT),
                                          Q_ARG(int, decW),
                                          Q_ARG(double, 1.0 / projFps(m_project)));
            }
        }
    } else {
        m_transAlpha = -1.0;
        m_underFrame = QImage();
        m_underRequested = false;
        m_underPending = false;
    }

    // Snapshot do caminho rápido: crop + efeitos básicos + máscaras + MotiOn/
    // OFX/frei0r vão para a thread de vídeo (FrameWorker), tirando a cadeia
    // inteira per-pixel do tiquinho da UI. Só LAINKA desliga o fast path
    // (onion skin/motion blur dependem do quadro anterior da UI).
    // O snapshot usa os MESMOS valores que applyCrop() usaria (crop avaliado
    // em m_playhead e máscaras em srcT).
    FrameFx topFx;
    const bool lainkaNeedsPrev = (m_clipLainkaEnabled && (m_clipLainkaOnionSkin > 1e-6 || m_clipLainkaMotionBlur > 1e-6));
    topFx.active = !m_clipLainkaEnabled;
    if (m_clipLainkaEnabled) topFx.active = false;
    topFx.cropL = m_lastCropL;
    topFx.cropR = m_lastCropR;
    topFx.cropT = m_lastCropT;
    topFx.cropB = m_lastCropB;
    topFx.rel = srcT;
    topFx.absTime = m_playhead;
    topFx.clip = *clip;
    topFx.lainkaEnabled = m_clipLainkaEnabled;
    topFx.lainkaId = m_clipLainkaId;
    topFx.lainkaFxT = (m_lainkaQuantizedTime >= 0.0) ? m_lainkaQuantizedTime : srcT;
    topFx.lainkaSkip = m_clipLainkaSkip;
    topFx.lainkaJitterPos = m_clipLainkaJitterPos;
    topFx.lainkaJitterRot = m_clipLainkaJitterRot;
    topFx.lainkaJitterScale = m_clipLainkaJitterScale;
    topFx.lainkaFlicker = m_clipLainkaFlicker;
    topFx.lainkaFlickerSpeed = m_clipLainkaFlickerSpeed;
    topFx.lainkaWarpAmount = m_clipLainkaWarpAmount;
    topFx.lainkaWarpSpeed = m_clipLainkaWarpSpeed;
    topFx.lainkaWarpGrid = m_clipLainkaWarpGrid;
    topFx.lainkaOnionSkin = m_clipLainkaOnionSkin;
    topFx.lainkaDustAmount = m_clipLainkaDustAmount;
    topFx.lainkaScratchAmount = m_clipLainkaScratchAmount;
    topFx.lainkaMotionBlur = m_clipLainkaMotionBlur;
    topFx.lainkaOpacity = m_clipLainkaOpacity;
    topFx.lainkaTargetFps = m_clipLainkaTargetFps;
    topFx.lainkaAntialias = m_clipLainkaAntialias;
    topFx.lainkaHold = m_clipLainkaHold;
    topFx.lainkaStutter = m_clipLainkaStutter;
    topFx.lainkaPosterize = m_clipLainkaPosterize;
    topFx.lainkaColorShift = m_clipLainkaColorShift;
    topFx.lainkaGate = m_clipLainkaGate;
    topFx.lainkaNeedsPrev = lainkaNeedsPrev;

    // Path efetivo de vídeo do clipe do topo: proxy se houver (consistente com
    // requestFrame/onFrameReady e o cache).
    const QString vpath = resolvePreviewVideo(m->filePath);
    if (PreviewProfiler::active()) {
        // O que o A/B proxy-vs-original precisa saber: de onde saiu o quadro
        // que vai aparecer na tela (o clipe do topo), não quantas camadas
        // inferiores resolveram para proxy.
        profFrame().proxy = (vpath != m->filePath);
        profFrame().cut = (m_profTopClipId != m->id);
        m_profTopClipId = m->id;
    }

    // Se temos um quadro pré-carregado que bate com a posição de entrada do novo clipe, exibe imediatamente.
    bool usedPrefetch = false;
    if (m_transAlpha < 0.0) {
        QMutexLocker l(&m_frameMutex);
        const double frameDur = 1.0 / projFps(m_project);
        if (m_prefetch.valid && m_prefetch.path == vpath
            && std::fabs(m_prefetch.t - srcT) <= frameDur * 0.5
            && m_prefetch.maxW == decW) {
            m_frameFull = m_prefetch.img;
            m_shownPath = vpath;
            m_shownT = srcT;
            m_shownW = decW;
            m_shownFx = false; // quadro cru do prefetch; crop é aplicado na UI
            m_lastSrcT = srcT;
            m_lastDecodeW = decW;
            m_lastFile = vpath;
            m_prefetch.valid = false;
            m_prefetch.requested = false;
            usedPrefetch = true;
            applyCrop();
            update();
        }
    }
    // Diagnóstico do engasgo no corte: o clipe do topo mudou e o frame0 pront o
    // do prefetch NÃO foi usado — o corte vai pagar decode (idealmente quente no
    // pool, mas frio se o prefetch foi descartado atrás). Só em PIERROT_PLAY_DEBUG.
    if (playDbg() && !usedPrefetch && clip && clip->id != m_dbgLastTopId) {
        m_dbgLastTopId = clip->id;
        bool req = false, valid = false, invoked = false;
        {
            QMutexLocker l(&m_frameMutex);
            req = m_prefetch.requested;
            valid = m_prefetch.valid;
            invoked = m_prefetch.invoked;
        }
        qDebug().noquote()
            << QStringLiteral("[play] CORTE sem prefetch pronto (requested=%1 valid=%2 invoked=%3) t=%4")
                   .arg(req).arg(valid).arg(invoked)
                   .arg(m_playhead, 0, 'f', 3);
    }

    // O quadro do tamanho/posição atuais já está pronto? Apenas reaplica o
    // pan/crop (por exemplo quando só o corte mudou) sem decodificar de novo.
    bool sameShown = false;
    bool shownFx = false;
    {
        QMutexLocker l(&m_frameMutex);
        sameShown = (!usedPrefetch && m_shownPath == vpath && std::fabs(m_shownT - srcT) < 1e-6
                     && m_shownW == decW);
        shownFx = m_shownFx;
    }
    if (sameShown) {
        if (shownFx) {
            // Veio pronto da thread do worker (já cortado/efeituado): aplicar
            // applyCrop() por cima duplicaria o corte. Re-pede o quadro com o
            // snapshot atual — o worker reaplica a cadeia.
            requestFrame(clip->id, vpath, srcT, decW, topFx);
        } else {
            applyCrop();
            update();
        }
        return;
    }
    if (usedPrefetch) {
        if (PreviewProfiler::active()) profFrame().prefetchHit = true;
        // Já no tick do corte, dispara o próximo frame: o decoder trocado está
        // posicionado e decodifica adiante, evitando "segurar" o frame do corte.
        requestFrame(clip->id, vpath, srcT + 1.0 / projFps(m_project), decW, topFx);
        return;
    }
    if (PreviewProfiler::active()) profFrame().prefetchHit = false;
    requestFrame(clip->id, vpath, srcT, decW, topFx);
}

// Pedido "assíncrono": a decodificação acontece na thread do FrameWorker.
// Vários pedidos seguidos entram numa fila; apenas um roda por vez
// (scrub não empilha — a fila só guarda pedidos ainda não enviados).
void PreviewWidget::requestFrame(const QString& clipId, const QString& path, double t, int maxW,
                                 const FrameFx& fx) {
    QMutexLocker l(&m_frameMutex);
    // Fase 3: se um decode já está em andamento e este pedido do TOPO o supera
    // (cortou para outro clipe, ou scrub/seek de mais de 3 frames parado),
    // cancela o decode obsoleto — o worker larga o alvo antigo e pega o novo já.
    // Durante reprodução no MESMO clipe NÃO cancela: o avanço de 1 frame por
    // tick completaria mesmo; cancelar a cada tick travaria o preview.
    if (m_hasInflightReq && m_frameWorker && m_project) {
        // PIERROT_NO_CANCEL=1 desliga (A/B: medir o efeito do cancelamento).
        static const bool kCancelOn = [] {
            return !qEnvironmentVariableIsSet("PIERROT_NO_CANCEL");
        }();
        const Clip* top = clipAt(m_playhead);
        if (kCancelOn && top && top->id == clipId) {
            bool cancel = (m_inflightReq.clipId != clipId);
            if (!cancel && !m_playing) {
                const double fd = projFps(m_project) > 0.0 ? 1.0 / projFps(m_project) : 1.0 / 30.0;
                cancel = std::fabs(m_inflightReq.t - t) > fd * 3.0;
            }
            if (cancel) m_frameWorker->requestCancel();
        }
    }
    // Coalesce: durante o scrub (e na reprodução) chegam vários alvos para o
    // mesmo clipe; só o mais recente interessa — o worker é serial e decodificar
    // posições intermediárias só atrasa a chegada ao alvo final.
    for (FrameReq& r : m_reqQueue)
        if (r.clipId == clipId && r.path == path && r.maxW == maxW) {
            r.fx = fx;
            if (std::fabs(r.t - t) < 1e-6) return; // já enfileirado igual
            r.t = t;
            r.dt = 1.0 / projFps(m_project);
            return;
        }
    m_reqQueue.append({clipId, path, t, 1.0 / projFps(m_project), maxW, fx});
    kickFrameWorker();
}


// Pedidos os quadros dos clipes de vídeo ativos no playhead que ficam POR
// BAIXO do clipe do topo (empilhamento de faixas). Quadros já em cache
// (mesmo arquivo/tempo/tamanho) são pulados.
void PreviewWidget::requestLowerLayers(int decW) {
    if (!m_project) return;
    const Clip* top = clipAt(m_playhead);
    // Quantos decodes de camada inferior são pedidos por frame — candidato nº1
    // a roubar slots do worker que deveria estar decodificando o clipe do topo
    // (vídeo que engasga com áudio perfeito = o quadro do topo chega tarde).
    int dbgLayerDecodes = 0;
    // Camadas que deixaram de estar ativas no playhead saem do cache (evita
    // crescimento sem limite conforme o usuário navega pelo projeto).
    {
        QList<QString> active;
        for (const Track& tr : m_project->videoTracks) {
            if (!tr.visible) continue;
            for (const Clip& cl : tr.clips)
                if (!cl.isText && m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur)
                    active.append(cl.id);
        }
        QMutexLocker l(&m_frameMutex);
        for (auto it = m_layerCache.begin(); it != m_layerCache.end();) {
            if (!active.contains(it.key())) it = m_layerCache.erase(it);
            else ++it;
        }
    }
    for (const Track& tr : m_project->videoTracks) {
        if (!tr.visible) continue;   // faixa oculta (olho)
        const Clip* c = nullptr;
        for (const Clip& cl : tr.clips) {
            if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                const MediaItem* mm = m_project->findMedia(cl.mediaIdAt(m_playhead - cl.pos));
                if (mm && mm->hasVideo && (!c || cl.pos > c->pos)) c = &cl;
            }
        }
        if (!c || (top && c->id == top->id)) continue;
        const MediaItem* m = m_project->findMedia(c->mediaIdAt(m_playhead - c->pos));
        if (!m || !m->hasVideo) continue;
        // Cor sólida não tem arquivo: é gerada na pintura, não pede decode.
        if (m->isSolid) continue;
        // Malha 3D também não é mídia decodificável: um .obj não vai pelo
        // FFmpeg. A camada é rasterizada por `MesaRenderer::prepareLayer`,
        // no loop de tracks Mesa logo abaixo.
        if (m->isMesh) continue;
        const double srcT = clipSrcTime(*c, m_playhead - c->pos);
        const QString vpath = resolvePreviewVideo(m->filePath);
        bool pending = false;
        {
            QMutexLocker l(&m_frameMutex);
            const auto it = m_layerCache.constFind(c->id);
            if (it != m_layerCache.constEnd() && it->path == vpath
                && std::fabs(it->t - srcT) < 3.0 / projFps(m_project)
                && it->maxW == decW && !it->img.isNull())
                continue; // já em cache (3 frames: evita re-decode de background a cada frame)
            // Já em voo (worker ou fila)? Passa adiante — senão a camada de
            // cima, que fica "expirada" até o decode terminar, monopolizava o
            // loop e as camadas 3+ nunca eram pedidas (frame velho/ausente).
            if ((m_hasInflightReq && m_inflightReq.clipId == c->id)
                || (m_hasLayerInflight && m_layerInflight.clipId == c->id)) {
                pending = true; // em decode: não dá pra re-mirar, só esperar
            } else {
                for (const FrameReq& r : m_reqQueue)
                    if (r.clipId == c->id) { pending = true; break; }
            }
        }
        if (pending) continue; // em voo/fila: não conta nem segura as demais
        requestFrame(c->id, vpath, srcT, decW, [&] {
            // Camada inferior sem LAINKA: crop + efeitos básicos + MotiOn/OFX/
            // frei0r vão para a thread do worker (era o H6: per-pixel na UI a
            // cada quadro; MotiOn/OFX/frei0r das inferiores nem sequer eram
            // aplicados — lacuna de paridade com a exportação).
            FrameFx fx;
            fx.active = !c->lainkaEnabled;
            if (fx.active) {
                const double rel = m_playhead - c->pos;
                clipCrop(*c, rel, fx.cropL, fx.cropR, fx.cropT, fx.cropB);
                fx.rel = rel;
                fx.absTime = m_playhead;
                fx.clip = *c;
            }
            return fx;
        }());
        ++dbgLayerDecodes;
        // Teto de decodes NOVAS por tick: prioriza o clipe do topo sem afogar
        // o worker, mas cobre 2 camadas novas — com 1 por tick e 3+ faixas
        // empilhadas, o pedido em voo re-expirava a cada tick e as camadas
        // fundas nunca alcançavam.
        if (dbgLayerDecodes >= 2) break;
    }

    // Tracks Mesa: decodifica o frame individual de cada track via MesaRenderer
    // e armazena no layerCache, para que paintEvent possa compô-las por baixo
    // da composição Mesa do topo (transparência, blend entre faixas).
    for (const Track& tr : m_project->videoTracks) {
        if (!tr.visible) continue;   // faixa oculta (olho)
        const MesaComposition* mc = m_project->findMesaForTrack(tr.id);
        if (!mc) continue;
        const Clip* c = nullptr;
        for (const Clip& cl : tr.clips) {
            if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                if (!c || cl.pos > c->pos) c = &cl;
            }
        }
        if (!c || (top && c->id == top->id)) continue;
        // Cache hit: já renderizado nesta frame.
        {
            QMutexLocker l(&m_frameMutex);
            if (m_layerCache.contains(c->id)) continue;
        }
        MesaRenderer::LayerPrep prep;
        if (m_mesaRenderer.prepareLayer(prep, *mc, *m_project, m_playhead, tr) && prep.valid) {
            QMutexLocker l(&m_frameMutex);
            m_layerCache[c->id] = {prep.frame,
                                   QStringLiteral("mesa:") + tr.id,
                                   m_playhead, decW};
        }
    }
    // Log de diagnóstico: quantos decodes inferiores dispararam este frame — se
    // houver, o worker está dividindo atenção entre o clipe do topo e N camadas
    // novas ao mesmo tempo (candidato nº1 ao engasgo pós-20s em projetos com
    // múltiplas faixas).
    if (playDbg() && dbgLayerDecodes > 0)
        qDebug().noquote() << QStringLiteral("[play] layers decode=%1 t=%2 top=%3 queue=%4")
                  .arg(dbgLayerDecodes)
                  .arg(m_playhead, 0, 'f', 3)
                  .arg(top ? top->id.mid(0,8) : QString())
                  .arg(m_reqQueue.size());
    if (PreviewProfiler::active()) {
        profFrame().lowerReq = dbgLayerDecodes;
        profFrame().queue = m_reqQueue.size();
    }
}

// Chamado com m_frameMutex segurado. Despacha para DOIS workers (H3): o do
// topo (prioridade absoluta — é o quadro exibido) e o dedicado de camadas
// inferiores (roda em paralelo; antes tudo serializava num worker só).
void PreviewWidget::kickFrameWorker() {
    if (m_reqQueue.isEmpty() || !m_frameWorker) return;

    // Podas pedidos órfãos: na virada de um corte, o pedido residual do clipe
    // antigo não é mais "vivo" — decodificá-lo gastava um slot inteiro e
    // atrasava o 1º frame do clipe novo (o "tiquinho" de travada). Na fila só
    // interessam clipes que contêm o playhead agora (topo ou camadas ativas).
    if (m_project) {
        for (int i = m_reqQueue.size() - 1; i >= 0; --i) {
            const QString& cid = m_reqQueue[i].clipId;
            bool live = false;
            for (const Track& tr : m_project->videoTracks)
                for (const Clip& c : tr.clips)
                    if (c.id == cid && !c.isText
                        && m_playhead >= c.pos - 1e-6 && m_playhead < c.pos + c.dur + 1e-6) {
                        live = true;
                        break;
                    }
            if (!live) m_reqQueue.removeAt(i);
        }
        if (m_reqQueue.isEmpty()) return;
    }

    const Clip* top = clipAt(m_playhead);

    // 1) TOPO → worker principal (prioridade absoluta: é o quadro exibido).
    if (!m_workerBusy && top) {
        for (int i = 0; i < m_reqQueue.size(); ++i) {
            if (m_reqQueue[i].clipId != top->id) continue;
            const FrameReq r = m_reqQueue.takeAt(i);
            m_workerBusy = true;
            m_inflightReq = r;
            m_hasInflightReq = true;
            if (m_perfT.isValid()) m_perfWorkerStartNs = m_perfT.nsecsElapsed();
            QMetaObject::invokeMethod(m_frameWorker, "decodeOne", Qt::QueuedConnection,
                                      Q_ARG(QString, r.clipId), Q_ARG(QString, r.path),
                                      Q_ARG(double, r.t), Q_ARG(int, r.maxW),
                                      Q_ARG(double, r.dt), Q_ARG(FrameFx, r.fx));
            break;
        }
    }

    // 2) Camadas inferiores → worker DEDICADO (H3), em paralelo com o topo.
    if (!m_layerBusy && m_layerWorker && !m_reqQueue.isEmpty()) {
        // Só pedidos de CAMADA: o primeiro da fila pode ser um pedido do TOPO
        // (o worker principal estava ocupado e o pedido ficou na fila) —
        // mandá-lo para o worker de camadas gasta o slot do dedicado num frame
        // que já tem dono e ATRASA a entrega da camada de verdade: com o topo
        // lento (animação/efeitos/cena pesada) os clipes de baixo ficavam sem
        // quadro — "não reproduzidos" enquanto o topo estava animado.
        int idx = -1;
        for (int i = 0; i < m_reqQueue.size(); ++i) {
            if (!top || m_reqQueue[i].clipId != top->id) { idx = i; break; }
        }
        if (idx >= 0) {
            const FrameReq r = m_reqQueue.takeAt(idx);
            m_layerBusy = true;
            m_layerInflight = r;
            m_hasLayerInflight = true;
            QMetaObject::invokeMethod(m_layerWorker, "decodeOne", Qt::QueuedConnection,
                                      Q_ARG(QString, r.clipId), Q_ARG(QString, r.path),
                                      Q_ARG(double, r.t), Q_ARG(int, r.maxW),
                                      Q_ARG(double, r.dt), Q_ARG(FrameFx, r.fx));
        }
    }
}

