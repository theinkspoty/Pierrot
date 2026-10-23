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

void PreviewWidget::onFrameReady(const QString& clipId, const QString& path, double t, int maxW,
                                 const QImage& img, bool processed) {
    handleFrameReady(clipId, path, t, maxW, img, processed, /*fromLayer=*/false);
}

void PreviewWidget::handleFrameReady(const QString& clipId, const QString& path, double t, int maxW,
                                     const QImage& img, bool processed, bool fromLayer) {
    // Latência do decode SÓ do worker do topo: o m_perfWorkerStartNs é o
    // dispatch do último quadro do topo — se um resultado de CAMADA chegasse
    // segundos depois, a subtração gerava latência falsa de 1 s (poluía o
    // p99 do profiler; o H3 separou os workers, separe a métrica também).
    const qint64 workerLatNs =
        (!fromLayer && PreviewProfiler::active() && m_perfT.isValid() && m_perfWorkerStartNs > 0)
            ? m_perfT.nsecsElapsed() - m_perfWorkerStartNs
            : 0;
    if (PreviewProfiler::active() && !fromLayer) {
        profFrame().workerNs = workerLatNs; // latência do decode: dispatch -> quadro pronto
        profFrame().cacheHits = FrameCache::hits();
        profFrame().cacheMisses = FrameCache::misses();
        profFrame().decSeekNs = qint64(DecoderMetrics::instance().lastSeekNs());
        profFrame().decDiscard = DecoderMetrics::instance().lastDiscard();
        profFrame().decWorkNs = DecoderMetrics::instance().lastWorkNs();
    }
    // Latência "apertar play -> 1º quadro na tela" (só diagnostica; com o
    // warmPlayhead no playhead parado deve cair de centenas de ms para dezenas).
    if (m_playStartNs > 0 && !img.isNull()) {
        const double dtMs = (PreviewProfiler::nowNs() - m_playStartNs) / 1e6;
        if (playDbg())
            qDebug().noquote()
                << QStringLiteral("[play] 1º quadro %1ms após o play").arg(dtMs, 0, 'f', 0);
        m_playStartNs = 0; // mede só o primeiro quadro de cada play
    }
    {
        QMutexLocker l(&m_frameMutex);
        if (fromLayer) {
            // Resultado do worker DEDICADO de camadas (H3): só o estado dele
            // zera; o worker principal segue com o quadro do topo.
            m_layerBusy = false;
            m_hasLayerInflight = false;
        } else {
            m_workerBusy = false;
            m_hasInflightReq = false;
            m_perfWorkerStartNs = 0;
            if (workerLatNs > 0) m_perf.workerMs = workerLatNs / 1000000;
        }
        kickFrameWorker(); // continua com o próximo pedido, se houver
    }

    // ── Adaptive quality: monitora latência do decode ──────────────
    if (m_playing && m_perf.workerMs > 0 && m_project && m_project->fps > 0.0) {
        const double frameMs = 1000.0 / m_project->fps;
        // Média móvel exponencial (suaviza spikes).
        m_adaptiveDecodeMsAvg = m_adaptiveDecodeMsAvg * 0.8 + m_perf.workerMs * 0.2;
        if (m_adaptiveDecodeMsAvg > frameMs * 1.2) {
            ++m_adaptiveSlowCount;
        } else {
            m_adaptiveSlowCount = std::max(0, m_adaptiveSlowCount - 1);
        }
        // 5 ticks consecutivos lentos → baixa resolução.
        if (m_adaptiveSlowCount >= 5 && !m_adaptiveActive) {
            m_adaptiveBaseQuality = m_previewQuality;
            const int levels[] = {3840, 1080, 720, 480, 360};
            for (int i = 0; i < 5; ++i) {
                if (m_previewQuality >= levels[i] && levels[i] < m_previewQuality) {
                    setPreviewQuality(levels[i]);
                    m_adaptiveActive = true;
                    break;
                }
            }
        }
        // Recupera quando a latência normaliza por 30 ticks.
        if (m_adaptiveActive && m_adaptiveDecodeMsAvg < frameMs * 0.7) {
            m_adaptiveSlowCount -= 2;
            if (m_adaptiveSlowCount <= 0) {
                setPreviewQuality(m_adaptiveBaseQuality);
                m_adaptiveActive = false;
                m_adaptiveSlowCount = 0;
            }
        }
        // Acumula dropped frames para o overlay.
        m_perf.droppedTotal += consumeDroppedFrames(); // só o overlay do live
    }

    if (img.isNull() || !m_project) return;

    const Clip* clip = nullptr;
    for (const Track& tr : m_project->videoTracks)
        for (const Clip& c : tr.clips)
            if (c.id == clipId) { clip = &c; break; }
    if (!clip) return;
    // Multicam: a mídia ativa é a do ângulo no playhead (mediaIdAt), não a base
    // (mediaId). Validar pela base descartava todo frame correto do ângulo ativo
    // e travava o monitor no conteúdo do clipe anterior ("mostra outro clipe").
    const MediaItem* m = m_project->findMedia(clip->mediaIdAt(m_playhead - clip->pos));
    if (!m) return;
    if (resolvePreviewVideo(m->filePath) != path) return;
    if (PreviewProfiler::active())
        profFrame().proxy = (path != m->filePath); // o quadro exibido veio de proxy?

    // Resultado de um clipe que JÁ NÃO está ativo no playhead (o corte/scrub
    // mudou enquanto o worker decodificava): o quadro é válido para o alvo
    // ANTIGO, não para o estado atual do monitor. O decode não se perde — o
    // FrameCache (L4) guarda o quadro por (path, t, maxWidth) — então
    // descartar aqui só evita lixo no cache de camadas e pinturas com
    // conteúdo de posição vencida. (ideia da branch fix/preview-stale-frame,
    // implementada sem destruir o arquivo 🙂)
    if (m_playhead < clip->pos - 1e-6 || m_playhead >= clip->pos + clip->dur + 1e-6)
        return;

    // Ignora quadros decodificados para outra posição (scrub/seek rápido muito
    // distante). Durante a REPRODUÇÃO exige proximidade de poucos quadros: antes
    // aceitava qualquer frame a até 1,5 s, o que fazia a tela exibir um trecho
    // errado do clipe (parecia "outro clipe") enquanto o áudio ia no lugar certo.
    // EXCEÇÃO (virada de corte): o PRIMEIRO frame de um clipe NOVO chega
    // atrasado por definição (decode/open no momento da virada) e rejeitá-lo
    // desperdiçava o decode + pedia de novo = preto prolongado e frames
    // perdidos no corte. Aceitamos com uma tolerância própria, maior, porém
    // LIMITADA (kFirstFrameTolS): exibir o começo do clipe certo um pouco
    // atrasado é melhor que preto, mas nunca seguramos um trecho muito distante
    // (o antigo 1,5 s mostrava conteúdo visivelmente fora de sincronia).
    const bool isNewClip = (m_shownClipId != clipId);
    constexpr double kFirstFrameTolS = 0.5;
    const double wantT = clipSrcTime(*clip, m_playhead - clip->pos);
    const double tol = isNewClip
        ? std::max(kFirstFrameTolS, 3.0 / std::max(1.0, projFps(m_project)))
        : (m_playing ? (3.0 / std::max(1.0, projFps(m_project))) : 1.5);
    if (std::fabs(wantT - t) > tol) return;

    // Quantum a que o playhead (já cruzou o frame) está à frente do quadro que
    // acabou de decodificar. >1 frame em reprodução = o vídeo está atrasado e
    // vai ENGASGAR (re-exibir) mesmo com o áudio perfeito.
    if (playDbg() && m_playing && std::fabs(t - wantT) <= tol) {
        const double aheadFrames = (wantT - t) * projFps(m_project);
        if (std::fabs(aheadFrames) > 0.5)
            qDebug().noquote() << QStringLiteral("[play] frame atraso %1f (dec %2s playhead %3s) lat %4ms q %5 %6")
                      .arg(aheadFrames, 4, 'f', 1)
                      .arg(t, 0, 'f', 3)
                      .arg(wantT, 0, 'f', 3)
                      .arg(static_cast<double>(m_perf.workerMs), 0, 'f', 0)
                      .arg(maxW)
                      .arg(path);
    }

    // Outlier de decode (P4): acima de 500 ms um frame é EVENTO, não ruído —
    // avisa sempre (não só com debug) e com o CAMINHO da mídia, para caçar a
    // fonte culpada (o outlier de 1,4 s da sessão de estresse era invisível:
    // a métrica dizia "max" mas não quem).
    if (workerLatNs > 500000000LL)
        qWarning().noquote() << QStringLiteral("[worker] decode LENTO %1 ms → %2 (t=%3, %4px, lat=%5ms)")
                                 .arg(double(workerLatNs) / 1e6, 0, 'f', 0)
                                 .arg(path)
                                 .arg(t, 0, 'f', 3)
                                 .arg(maxW)
                                 .arg(static_cast<double>(m_perf.workerMs), 0, 'f', 0);

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
            m_shownClipId = clipId; // posse: o fast-path só desenha o frame do dono
            m_shownT = t;
            m_shownW = maxW;
            m_shownFx = processed;   // m_frameFull já chegou cortado/efeituado
        }
        m_frameFull = img;
        m_lastSrcT = t;
        m_lastDecodeW = maxW;
        m_lastFile = path;
        if (processed) {
            // Camada rápida: crop + efeitos básicos/máscaras foram aplicados na
            // thread de vídeo — aplicar applyCrop() aqui duplicaria o pipeline.
            m_frame = img;
        } else {
            applyCrop();
        }
        update();
        return;
    }

    // Quadro de uma camada INFERIOR: guarda no cache de camadas (cortado + LAINKA).
    QImage cropped;
    if (processed) {
        // Já veio cortado + com efeitos básicos/máscaras da thread do worker.
        cropped = img;
    } else {
        const double rel = m_playhead - clip->pos;
        int cL, cR, cT, cB;
        clipCrop(*clip, rel, cL, cR, cT, cB);
        cropped = applyCropTo(img, cL, cR, cT, cB);
        // Aplica LAINKA também em camadas inferiores.
        if (clip->lainkaEnabled) {
            double srcT = clipSrcTime(*clip, m_playhead - clip->pos);
            const double pfps = projFps(m_project);
            const int effSkip = (clip->lainkaTargetFps > 0 && pfps > 1.0)
                ? std::max(1, (int)std::lround(pfps / clip->lainkaTargetFps))
                : clip->lainkaSkip;
            if (effSkip > 1)
                srcT = lainkaQuantizeTime(srcT, effSkip, pfps);
            cropped = lainkaApplyFx(cropped, clip->id, srcT,
                                    clip->lainkaSkip, clip->lainkaJitterPos,
                                    clip->lainkaJitterRot, clip->lainkaJitterScale,
                                    clip->lainkaFlicker, clip->lainkaFlickerSpeed,
                                    clip->lainkaWarpAmount, clip->lainkaWarpSpeed,
                                    clip->lainkaWarpGrid, clip->lainkaOnionSkin,
                                    clip->lainkaDustAmount, clip->lainkaScratchAmount,
                                    clip->lainkaMotionBlur, clip->lainkaOpacity,
                                    clip->lainkaTargetFps, clip->lainkaAntialias,
                                    QImage());
        }
        // Aplica efeitos básicos também em camadas inferiores.
        applyBasicEffectsOn(cropped, *clip, m_playhead - clip->pos);
        // MotiOn/OFX/frei0r das camadas inferiores (paridade com a exportação —
        // antes só o topo recebia; com LAINKA o worker não pôde aplicar).
        applyTemporalFxOn(cropped, *clip, m_playhead, m_playhead - clip->pos,
                          m_ofxManager, m_frei0rManager);
    }
    {
        QMutexLocker l(&m_frameMutex);
        m_layerCache[clipId] = {cropped, path, t, maxW};
    }
    update();
}

void PreviewWidget::onPrefetchReady(const QString& path, double t, int maxW, const QImage& img) {
    const qint64 latNs =
        (PreviewProfiler::active() && m_perfT.isValid() && m_perfPrefetchStartNs > 0)
            ? m_perfT.nsecsElapsed() - m_perfPrefetchStartNs : 0;
    if (PreviewProfiler::active()) profFrame().prefetchLatNs = latNs;
    QMutexLocker l(&m_frameMutex);
    if (latNs > 0) m_perf.prefetchLatMs = latNs / 1000000;
    m_perfPrefetchStartNs = 0;
    // Quadro do clipe de trás (transição ativa). Resposta do pedido em voo:
    // limpa o flag mesmo se for descartada (largura/tempo divergente), para o
    // próximo tick poder re-mirar o alvo.
    if (m_underRequested && m_underPath == path) {
        m_underPending = false;
        if (std::fabs(m_underT - t) < 1e-4 && m_underW == maxW)
            m_underFrame = img.isNull() ? QImage() : applyCropTo(img, m_underCropL,
                                                                  m_underCropR,
                                                                  m_underCropT,
                                                                  m_underCropB);
    }
    if (m_prefetch.requested && m_prefetch.path == path
        && std::fabs(m_prefetch.t - t) < 1e-4 && m_prefetch.maxW == maxW) {
        m_prefetch.img = img;
        m_prefetch.valid = !img.isNull();
        m_prefetch.invoked = false; // pronto (ou falhou): permite novo ciclo
        if (img.isNull()) m_prefetch.requested = false; // falhou: encerra o ciclo
    }
    // O prefetch liberou o caminho — retoma os decodeOne's que estavam esperando.
    m_workerBusy = false;
    kickFrameWorker();
}

void PreviewWidget::onBgPrefetchDone(const QString& path, double t, int maxW,
                                     const QImage& frame0, const QImage& frame1) {
    Q_UNUSED(frame1);
    const qint64 latNs =
        (PreviewProfiler::active() && m_perfT.isValid() && m_perfPrefetchStartNs > 0)
            ? m_perfT.nsecsElapsed() - m_perfPrefetchStartNs : 0;
    if (PreviewProfiler::active()) profFrame().prefetchLatNs = latNs;
    {
        QMutexLocker l(&m_frameMutex);
        if (latNs > 0) m_perf.prefetchLatMs = latNs / 1000000;
        m_perfPrefetchStartNs = 0;
        if (m_prefetch.requested && m_prefetch.path == path && m_prefetch.maxW == maxW) {
            m_prefetch.img = frame0;
            m_prefetch.valid = !frame0.isNull();
            m_prefetch.invoked = false;
            if (frame0.isNull()) m_prefetch.requested = false;
        }
    }
    // O decoder NÃO vem junto: o prefetch só aqueceu o pool (a entrada segue
    // quente). No corte, o FrameWorker pega o mesmo decoder já quente na
    // posição do próximo clipe — sem troca de posse, sem fila.
}

void PreviewWidget::onBgPrefetchFailed(const QString& path) {
    QMutexLocker l(&m_frameMutex);
    if (m_prefetch.requested && m_prefetch.path == path) {
        m_prefetch.valid = false;
        m_prefetch.requested = false;
    }
}

// Próximo clipe que começa depois do fim de `c` (varredura igual à do
// updatePrefetch: próximo início de clipe na timeline, preferindo faixa de cima).
static const Clip* clipAfter(const Project* p, const Clip* c) {
    if (!p || !c) return nullptr;
    const double start = c->pos + c->dur;
    const Clip* found = nullptr;
    double nextPos = 1e9;
    for (int tr = (int)p->videoTracks.size() - 1; tr >= 0; --tr) {
        for (const Clip& cc : p->videoTracks[tr].clips) {
            if (cc.pos >= start - 1e-4 && cc.pos < nextPos) {
                nextPos = cc.pos;
                found = &cc;
            }
        }
    }
    return found;
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
    // Janela dinâmica: precisa cobrir o open() FRIO do próximo arquivo
    // (~1.3-1.5s medido, até ~2s em disco lento/rede) com folga, senão o warm
    // chega depois do corte e o worker paga o open na thread do decode (foi o
    // stall de 1,3s da sessão de referência). Antes era ≤5s; agora até 12s para
    // clipes longos, mínimo 3s para clipes curtos (o corte está próximo de
    // qualquer forma).
    const double window = qMin(12.0, qMax(3.0, clip->dur * 0.9));
    if (remain > window || remain <= 0.0) return;

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
    // Prefetch no mesmo tamanho do quadro com qualidade (nunca abaixo da tela).
    const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
    const int decW = qMax(160, qMax(m_previewQuality, widgetW));
    const QString vpath = resolvePreviewVideo(nextMedia->filePath);

    // ── Frame0 do próximo clipe (crítico: o corte mais próximo) ──────────
    // Decide ANTES do warm-ahead e despacha logo, para que decode(N1) seja o
    // primeiro da fila da thread bg (nunca atrasado pelo warm do 2º clipe).
    bool needFrame0 = false;
    {
        QMutexLocker l(&m_frameMutex);
        if (m_perfT.isValid()) m_perfPrefetchStartNs = m_perfT.nsecsElapsed();
        if (m_prefetch.requested && m_prefetch.path == vpath
            && std::fabs(m_prefetch.t - srcT) < 1e-4 && m_prefetch.maxW == decW) {
            needFrame0 = false; // já solicitado ou já pronto
        } else {
            m_prefetch.path = vpath;
            m_prefetch.t = srcT;
            m_prefetch.maxW = decW;
            m_prefetch.img = QImage();
            m_prefetch.valid = false;
            m_prefetch.requested = true;
            m_prefetch.invoked = true; // despachado agora (re-despachado se o corte chegar sem terminar)
            m_prefetch.clipEnd = clip->pos + clip->dur;
            needFrame0 = true;
        }
    }
    if (needFrame0 && m_pipeline) {
        m_pipeline->prefetchFrame(vpath, srcT, decW, 1.0 / projFps(m_project));
    }

    // ── Warm-ahead (L4): 2º/3º clipe à frente, só abre+posiciona ─────
    // A UI resolve QUAIS clipes/paths (é política de modelo, L6); o
    // FramePipeline cuida da fila, da serialização e do estado in-flight.
    QVector<FramePipeline::WarmTarget> wants;
    const Clip* ahead = nextClip;
    for (int i = 0; i < 2 && ahead; ++i) {
        ahead = clipAfter(m_project, ahead);
        if (!ahead) break;
        const MediaItem* am = m_project->findMedia(ahead->mediaId);
        if (!am || !am->hasVideo) continue;
        const QString apath = resolvePreviewVideo(am->filePath);
        if (apath.isEmpty() || apath == vpath) continue; // mesmo arquivo já coberto
        if (m_decPool->contains(apath) || m_pipeline->warmInFlight(apath)) continue;
        bool dup = false;
        for (const auto& w : wants)
            if (w.path == apath) { dup = true; break; }
        if (!dup) wants.push_back({apath, ahead->in, decW});
    }
    if (m_pipeline) m_pipeline->setWarmQueue(wants);
}

// Um alvo da fila de warm terminou. A fila e o despacho do próximo vivem no
// FramePipeline; aqui só se reagenda o idle-warm se a agulha estiver parada.
void PreviewWidget::onBgWarmDone(const QString& path) {
    Q_UNUSED(path); // a fila e o próximo despacho agora vivem no FramePipeline
}

// Agenda (debounce) o aquecimento do clipe sob a agulha. Só fora da reprodução:
// durante o play o prefetch e o warm-ahead já mantêm o pool quente. O timer de
// play ativo é a fonte da verdade do transporte (m_playing pode estar defasado).
void PreviewWidget::scheduleIdleWarm() {
    if (!m_idleWarmTimer) return;
    if (m_timer && m_timer->isActive()) return;
    m_idleWarmTimer->start();
}

// Aquece o clipe sob a agulha parada: abre+posiciona o decoder (vídeo) e
// começa a conformar a janela de áudio. Assim o próximo play não paga o open()
// frio (~500-800ms) nem o "começa mudo" do conform. Best-effort: se o decoder
// já está no pool (o FrameWorker decodificou o quadro da agulha), nada a fazer.
void PreviewWidget::warmPlayhead() {
    if (!m_project) return;
    if (m_timer && m_timer->isActive()) return;
    // Sink já criado no startup; revalida caso a saída padrão tenha mudado.
    prewarmAudioSink();
    const Clip* clip = clipAt(m_playhead);
    if (!clip) return;
    const MediaItem* media = m_project->findMedia(clip->mediaId);
    if (!media) return;

    const int widgetW = m_videoRect.width() > 0 ? m_videoRect.width() : 960;
    const int decW = qMax(160, qMax(m_previewQuality, widgetW));

    // Vídeo: aquece na thread de prefetch (ociosa fora do play). O pipeline
    // ignora o pedido se já houver um warm em andamento — o quadro chega de
    // qualquer forma pelo FrameWorker.
    if (media->hasVideo && m_pipeline && m_decPool
        && !media->filePath.isEmpty()) {
        const QString vpath = resolvePreviewVideo(media->filePath);
        if (!vpath.isEmpty() && !m_decPool->contains(vpath)) {
            // Mesmo tempo-fonte que o quadro da agulha (respeita in/speed).
            const double srcT = clipSrcTime(*clip, m_playhead - clip->pos);
            m_pipeline->warmNow(vpath, srcT, decW);
        }
    }

    // Áudio: pede (async) a janela de conform do trecho sob a agulha. No play,
    // updateSources() pede a MESMA janela — já preenchida, o sink parte com o
    // conteúdo certo em vez de silêncio.
    const QVector<AudioMixer::SourceInfo> sources = buildMixSources(m_project, m_playhead);
    if (!sources.isEmpty()) {
        AudioConformCache& conform = AudioConformCache::instance();
        for (const AudioMixer::SourceInfo& w : sources) {
            if (w.path.isEmpty()) continue;
            // Só até o fim do clipe (um SFX de 0,2 s não pede 6 s de decode).
            const double remainingOut =
                qMax(0.0, w.clipPos + w.clipDur - m_playhead);
            const double reqLen =
                qMin(6.0, remainingOut * qMax(0.01, w.speed) + 0.1);
            if (reqLen <= 0.0) continue;
            const AudioConformCache::Ref c = conform.get(w.path, w.audioStream);
            conform.request(c, w.mediaPos, reqLen, m_playhead);
        }
    }
}

