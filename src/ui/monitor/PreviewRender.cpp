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

namespace {
// Mapeia o modo de composição da faixa (os mesmos 12 modos da exportação)
// para o composition mode do QPainter. "subtract" usa SourceOut como
// aproximação (Dst × (1 - Src)), que é o mais próximo do subtract no Qt.
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
    if (mode == QStringLiteral("subtract")) return QPainter::CompositionMode_SourceOut;
    if (mode == QStringLiteral("exclusion")) return QPainter::CompositionMode_Exclusion;
    return QPainter::CompositionMode_SourceOver;
}
}

// ── LAINKA: usa funções compartilhadas de LainkaFx.h ────────────────
using namespace LainkaFx;

void PreviewWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    PreviewProfiler::Scope paintScope(PreviewProfiler::active() ? &profFrame().paintNs : nullptr);
    renderFrame(p);
    // Os indicadores vão SEMPRE por cima, em qualquer caminho de render. Antes
    // eles ficavam no fim de renderFrame(), que tem vários `return` antecipado
    // (projeto vazio, cache de composição, caminho de clip único) — ou seja,
    // em projetos com 2+ camadas o overlay de perf nunca aparecia, que é
    // justamente o cenário que se quer diagnosticar. A margem de segurança
    // tinha o mesmo problema (sumia no caminho do composto).
    drawGrid(p, m_lastCanvas);
    drawQualityChip(p);
    drawPlaybackBadges(p);
}

// Chip "720p"/"4K" no canto superior direito do vídeo: lembra que a imagem
// pode não estar na resolução cheia (estilo Premiere). Sutil: fundo preto
// translúcido, texto claro 8pt.
void PreviewWidget::drawQualityChip(QPainter& p) {
    if (m_videoRect.isEmpty()) return;
    QString code;
    for (const PreviewQOpt& o : kPreviewQualities)
        if (o.width == m_previewQuality) {
            code = QString::fromUtf8(o.code);
            break;
        }
    if (code.isEmpty()) return;
    p.save();
    p.setClipRect(m_videoRect);
    QFont f = p.font();
    f.setPointSizeF(8);
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    const QRect chip(m_videoRect.right() - fm.horizontalAdvance(code) - 18,
                     m_videoRect.top() + 10, fm.horizontalAdvance(code) + 12, 16);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 150));
    p.drawRoundedRect(chip, 3, 3);
    p.setPen(QColor(255, 255, 255, 220));
    p.drawText(chip, Qt::AlignCenter, code);
    p.restore();
}

void PreviewWidget::drawPlaybackBadges(QPainter& p) {
    // Indicador de frames perdidos (visível quando há drops, sem env var).
    if (m_playing && m_perf.droppedTotal > 0) {
        p.save();
        p.resetTransform();
        QFont df = p.font();
        df.setPointSizeF(8);
        df.setBold(true);
        p.setFont(df);
        const QString dropTxt = QStringLiteral("Dropped: %1").arg(m_perf.droppedTotal);
        const QRect dropR(width() - 130, 8, 122, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(180, 50, 50, 180));
        QPainterPath dropBg;
        dropBg.addRoundedRect(QRectF(dropR), 3.0, 3.0);
        p.drawPath(dropBg);
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(dropR, Qt::AlignCenter, dropTxt);
        p.restore();
    }

    // Indicador de qualidade adaptativa (mostra quando auto-baixou).
    if (m_adaptiveActive) {
        p.save();
        p.resetTransform();
        QFont af = p.font();
        af.setPointSizeF(8);
        af.setBold(true);
        p.setFont(af);
        const QString adpTxt = QStringLiteral("Auto: %1p").arg(m_previewQuality);
        const QRect adpR(width() - 130, m_playing && m_perf.droppedTotal > 0 ? 30 : 8, 122, 18);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(50, 120, 180, 180));
        QPainterPath adpBg;
        adpBg.addRoundedRect(QRectF(adpR), 3.0, 3.0);
        p.drawPath(adpBg);
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(adpR, Qt::AlignCenter, adpTxt);
        p.restore();
    }

    // Diagnóstico de performance (PIERROT_PERF_DEBUG=1).
    drawPerfOverlay(p);
}

void PreviewWidget::renderFrame(QPainter& p) {

    // Fundo do console (área ao redor do monitor).
    p.fillRect(m_videoRect, themeColors().monitorBg);

    if (!m_project || m_project->width <= 0 || m_project->height <= 0) {
        m_lastCanvas = QRect(); // sem canvas válido: sem margens de segurança
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
    m_lastCanvas = canvas; // margens de segurança desenhadas no paintEvent

    // Monitor: o quadro assenta direto no fundo do painel, com uma sombra
    // suave atrás (3 anéis pretos translúcidos — custo ~zero, sem blur) para
    // separar o "programa" do fundo do console.
    for (int i = 3; i >= 1; --i) {
        QColor sh(0, 0, 0, 12 / i);
        p.setPen(Qt::NoPen);
        p.setBrush(sh);
        p.drawRect(canvas.adjusted(-i, -i, i, i));
    }
    p.fillRect(canvas, themeColors().canvasBg);

    // Texto independente ativo no playhead (desenhado mesmo sem quadro de vídeo).
    bool anyText = false;
    if (m_project) {
        for (const Track& tr : m_project->videoTracks) {
            if (!tr.visible) continue;   // faixa oculta (olho) não mostra texto
            for (const Clip& c : tr.clips)
                if (c.isText && m_playhead >= c.pos && m_playhead < c.pos + c.dur) {
                    anyText = true;
                    break;
                }
        }
    }

    // Render unificado em "espaço de projeto": o canvas É o quadro do projeto
    // (k = pixels de tela por pixel do projeto). O vídeo é desenhado do mesmo
    // jeito com ou sem transform — sem transform, ele cabe inteiro no quadro;
    // com transform, pan/rot/zoom giram em torno do centro do quadro. Assim,
    // adicionar um keyframe de transform nunca muda o tamanho do vídeo.
    const Clip* clip = clipAt(m_playhead);

    // Carência do corte: a janela reinicia quando o clipe do topo muda.
    if (clip) {
        if (clip->id != m_graceTopId) {
            m_graceTopId = clip->id;
            m_cutGrace.start();
        }
    } else {
        m_graceTopId.clear();
    }
    // Carência do corte: é uma CONDIÇÃO DE ESTADO, não um prazo. Enquanto o
    // topo é um clipe novo e o worker ainda não entregou o primeiro quadro dele
    // (m_shownClipId != clip->id), seguramos o último quadro do clipe anterior
    // com transform neutro — o corte "fecha" quando o frame chega, não quando o
    // relógio anda. O teto kCutGraceCeilingMs é só rede de segurança: se o
    // decode falhar de vez (arquivo corrompido, disco morto), não deixamos o
    // quadro velho preso para sempre — depois do teto, fundo honesto.
    constexpr int kCutGraceCeilingMs = 700;
    const bool cutGrace = clip && !clip->isText && !m_shownClipId.isEmpty()
                          && m_shownClipId != clip->id
                          && m_cutGrace.isValid() && m_cutGrace.elapsed() < kCutGraceCeilingMs;

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
        // Opacidade via QPainter: antes multiplicava o alpha num QImage copiado
        // (DestinationIn) — a cópia forçava o COW a desprender o quadro inteiro
        // (8MB 1080p / 33MB 4K) a CADA paint em que alpha < 1. Como o fade da
        // camada de cima deixa alpha < 1 durante toda a transição, isso alocava
        // e copiava o quadro por frame e engasgava o vídeo. setOpacity aplica a
        // mesma mistura sem copiar.
        qp.setOpacity(alpha);
        qp.drawImage(0, 0, img);
        qp.restore();
    };

    // Uma camada pronta para desenhar (na ordem de baixo para cima). `text`
    // marca camadas de TEXTO: ficam na posição da faixa na pilha (um texto
    // numa faixa baixa fica POR BAIXO do vídeo das faixas de cima) e são
    // desenhadas sempre em SourceOver.
    struct Layer {
        QImage frame;
        double s = 1.0, sX = 1.0, sY = 1.0, rot = 0.0, x = 0.0, y = 0.0;
        double alpha = 1.0, ox = 0.0, oy = 0.0;
        QPainter::CompositionMode mode = QPainter::CompositionMode_SourceOver;
        const Clip* text = nullptr;
    };
    QVector<Layer> layers;

    // Monta o empilhamento: todas as faixas de vídeo (a faixa 0 é o TOPO), de
    // baixo para cima. Cada faixa contribui com o clipe de mídia ativo e com o
    // clipe de TEXTO ativo, NESTA ordem dentro da faixa — o texto respeita a
    // hierarquia das faixas (antes todos os textos eram desenhados por cima de
    // TUDO, no fim, ignorando a ordem das faixas).
    {
        const Clip* topClip = clip;
        for (int tr = (int)m_project->videoTracks.size() - 1; tr >= 0; --tr) {
            const Track& t = m_project->videoTracks[tr];
            if (!t.visible) continue;   // faixa oculta (olho) não participa da composição
            const bool isMesaTrack = m_project->findMesaForTrack(t.id) != nullptr;
            const Clip* c = nullptr;
            for (const Clip& cl : t.clips) {
                if (m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur && !cl.isText) {
                    const MediaItem* mm = m_project->findMedia(cl.mediaId);
                    if ((isMesaTrack || (mm && mm->hasVideo)) && (!c || cl.pos > c->pos))
                        c = &cl;
                }
            }
            const QPainter::CompositionMode mode = blendModeToQt(t.blendMode);

            // ── Camada de mídia da faixa (lambda: os `continue` viram
            // `return` — o texto da faixa é empilhado mesmo sem mídia) ──
            const auto addMediaLayer = [&]() {
            // Quadro deste clipe: vídeo normal
            QImage clipFrame;
            if (topClip && c->id == topClip->id) {
                // Posse + carência: só usa m_frame se o topo é o dono ou a
                // carência do corte ainda segura o último quadro do clipe
                // anterior (sem isto, a camada do topo sumia no corte e o
                // composite mostrava preto até o decode chegar).
                const bool owned = (m_shownClipId == c->id);
                if (owned || cutGrace) {
                    QMutexLocker l(&m_frameMutex);
                    if (!m_frame.isNull()) {
                        clipFrame = m_frame;
                    }
                }
            } else if (isMesaTrack) {
                // Track Mesa em layers inferiores: frame já renderizado pelo
                // MesaRenderer e armazenado no layerCache por requestLowerLayers.
                {
                    QMutexLocker l(&m_frameMutex);
                    clipFrame = m_layerCache.value(c->id).img;
                }
                if (clipFrame.isNull()) return;
            } else {
                const MediaItem* mm = m_project->findMedia(c->mediaId);
                if (!mm || !mm->hasVideo) return;
                if (mm->isSolid) {
                    // Mídia gerada: gera no tamanho natural, sem passar de uma
                    // vez pela resolução exibida (não borra e não estoura 4K
                    // a cada paint). Preserva o aspecto da mídia.
                    const int mw = mm->width > 0 ? mm->width : (int)pw;
                    const int mh = mm->height > 0 ? mm->height : (int)ph;
                    const double sc = qMin(1.0, qMin((double)canvas.width() / mw,
                                                     (double)canvas.height() / mh));
                    const int w = qMax(1, (int)std::lround(mw * sc));
                    const int h = qMax(1, (int)std::lround(mh * sc));
                    clipFrame = generatedFrameCached(*mm, w, h);
                    if (c->lainkaEnabled) {
                        const double srcT = clipSrcTime(*c, m_playhead - c->pos);
                        clipFrame = lainkaApplyFx(clipFrame, c->id, srcT,
                                                  c->lainkaSkip, c->lainkaJitterPos,
                                                  c->lainkaJitterRot, c->lainkaJitterScale,
                                                  c->lainkaFlicker, c->lainkaFlickerSpeed,
                                                  c->lainkaWarpAmount, c->lainkaWarpSpeed,
                                                  c->lainkaWarpGrid, c->lainkaOnionSkin,
                                                  c->lainkaDustAmount, c->lainkaScratchAmount,
                                                  c->lainkaMotionBlur, c->lainkaOpacity,
                                                  c->lainkaTargetFps, c->lainkaAntialias,
                                                  QImage());
                    }
                    applyBasicEffectsOn(clipFrame, *c, m_playhead - c->pos);
                } else {
                    {
                        QMutexLocker l(&m_frameMutex);
                        clipFrame = m_layerCache.value(c->id).img;
                    }
                    if (clipFrame.isNull()) return;
                }
            }

            if (clipFrame.isNull()) return;

            // Transição (mesma faixa): o clipe de trás por baixo
            if (topClip && c->id == topClip->id && m_transAlpha >= 0.0 && !m_underFrame.isNull()) {
                Layer u;
                u.frame = m_underFrame;
                layers.append(u);
            }

            Layer L;
            L.frame = clipFrame;
            L.s = kfValue(c->kfScale, c->scale, m_playhead - c->pos);
            L.sX = kfValue(c->kfScaleX, c->scaleX, m_playhead - c->pos);
            L.sY = kfValue(c->kfScaleY, c->scaleY, m_playhead - c->pos);
            L.rot = kfValue(c->kfRotation, c->rotation, m_playhead - c->pos);
            L.x = kfValue(c->kfTx, c->tx, m_playhead - c->pos);
            L.y = kfValue(c->kfTy, c->ty, m_playhead - c->pos);
            const double rel = m_playhead - c->pos;
            double alpha = std::clamp(kfValue(c->kfOpacity, c->opacity, rel), 0.0, 1.0);
            if (c->fadeIn > 1e-6) alpha *= std::min(1.0, rel / c->fadeIn);
            if (c->fadeOut > 1e-6) alpha *= std::min(1.0, (c->dur - rel) / c->fadeOut);
            if (topClip && c->id == topClip->id && m_transAlpha >= 0.0) {
                if (m_transType == QStringLiteral("dissolve")) alpha *= m_transAlpha;
                else if (m_transType == QStringLiteral("wipeleft")) L.ox = pw * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wiperight")) L.ox = -pw * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wipeup")) L.oy = ph * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wipedown")) L.oy = -ph * (1.0 - m_transAlpha);
                else if (m_transType == QStringLiteral("wipetl")) { L.ox = pw * (1.0 - m_transAlpha); L.oy = ph * (1.0 - m_transAlpha); }
                else if (m_transType == QStringLiteral("wipetr")) { L.ox = -pw * (1.0 - m_transAlpha); L.oy = ph * (1.0 - m_transAlpha); }
                else if (m_transType == QStringLiteral("wipebr")) { L.ox = -pw * (1.0 - m_transAlpha); L.oy = -ph * (1.0 - m_transAlpha); }
                else if (m_transType == QStringLiteral("wipebl")) { L.ox = pw * (1.0 - m_transAlpha); L.oy = -ph * (1.0 - m_transAlpha); }
            }
            L.alpha = alpha;
            L.alpha *= std::clamp(t.opacity, 0.0, 1.0);
            L.mode = mode;
            layers.append(L);
            if (composeDbg())
                qDebug() << "[compose] t=" << m_playhead << "tr=" << tr
                         << "clip=" << (c->name.isEmpty() ? c->id : c->name)
                         << "top=" << (topClip && c->id == topClip->id);

            // Texto ANEXADO a este clipe de vídeo: acima do próprio clipe,
            // abaixo das faixas de cima (antes era desenhado no fim, por
            // cima de tudo, ignorando a hierarquia).
            const TextStyle* ats = m_project->textStyleFor(*c);
            if (ats && !ats->isEmpty()) {
                Layer T;
                T.text = c;
                layers.append(T);
            }
            };
            if (c) addMediaLayer();

            // ── Texto INDEPENDENTE da faixa (clipe isText ativo) ──
            // O de pos mais tarde da faixa (mesmo critério do passo antigo).
            const Clip* tclip = nullptr;
            for (const Clip& cl : t.clips)
                if (cl.isText && m_playhead >= cl.pos && m_playhead < cl.pos + cl.dur)
                    if (!tclip || cl.pos > tclip->pos) tclip = &cl;
            if (tclip) {
                Layer T;
                T.text = tclip;
                layers.append(T);
                if (composeDbg())
                    qDebug() << "[compose] t=" << m_playhead << "tr=" << tr
                             << "text=" << (tclip->name.isEmpty() ? tclip->id : tclip->name);
            }
        }
    }

    if (composeDbg()) {
        for (const Layer& L : layers) {
            if (L.text) {
                qDebug() << "  [compose] t=" << m_playhead << "layer TEXT"
                         << (L.text->name.isEmpty() ? L.text->id : L.text->name);
                continue;
            }
            int cornerA = -1;
            if (!L.frame.isNull() && L.frame.width() > 2 && L.frame.height() > 2)
                cornerA = L.frame.pixelColor(1, 1).alpha();
            qDebug() << "  [compose] t=" << m_playhead << "layer " << L.frame.width() << "x" << L.frame.height()
                     << "alpha=" << L.alpha << "cornerA=" << cornerA << "mode=" << (int)L.mode;
        }
        qDebug() << "  [compose] t=" << m_playhead << "total=" << layers.size()
                 << "m_frameNull=" << m_frame.isNull();
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
    // Composite com texto: qualquer texto ativo força o caminho do composto
    // (os textos agora são camadas da pilha, na ordem das faixas — o caminho
    // rápido não tem hierarquia de faixas).
    if (layers.size() >= 2 || anyText
        || (m_frame.isNull() && !layers.isEmpty())) {
        // Cache: reutiliza se playhead EXATO, índice, camadas e canvas não
        // mudaram. O playhead exato é chave obrigatória: kfValue (opacidade,
        // transform, texto) é avaliado no playhead de cada paint — sem isso,
        // dois paints no mesmo quantum de frame desenhavam a fase velha da
        // animação e depois pulavam um quantum inteiro (tremida), ou ficavam
        // presos na fase velha até o índice virar (congelamento visível com
        // vídeo/imagem/texto animados).
        if (!m_compositedCache.isNull() && m_compositedEpoch == m_currentFrameIndex
            && m_compositedPlayhead == m_playhead
            && m_compositedCache.size() == canvas.size()
            && m_compositedLayerCount == layers.size()) {
            if (PreviewProfiler::active()) {
                profFrame().compositeEvaluated = true;
                profFrame().compositeHit = true;
            }
            p.drawImage(canvas.topLeft(), m_compositedCache);
            drawMaskOverlay(p, canvas, k);
            return;
        }
        if (PreviewProfiler::active()) {
            profFrame().compositeEvaluated = true;
            profFrame().compositeHit = false;
            profFrame().layers = layers.size();
        }
        PreviewProfiler::Scope compScope(PreviewProfiler::active() ? &profFrame().compositeNs : nullptr);
        // Buffer reutilizado entre frames (é o próprio m_compositedCache): alocar
        // um ARGB32 novo do canvas a cada quadro (~8 MB @1080p, 33 MB @4K) era
        // megas de allocator por frame à toa. fill() no mesmo buffer mantém a
        // capacidade; o epoch garante que repaints do mesmo frame só redesenham.
        if (m_compositedCache.size() != canvas.size()
            || m_compositedCache.format() != QImage::Format_ARGB32) {
            m_compositedCache = QImage(canvas.size(), QImage::Format_ARGB32);
        }
        QImage& acc = m_compositedCache;
        acc.fill(Qt::black);
        const double cx = acc.width() / 2.0;
        const double cy = acc.height() / 2.0;
        for (const Layer& L : layers) {
            QPainter ap(&acc);
            ap.setClipRect(QRect(0, 0, acc.width(), acc.height()));
            if (L.text) {
                // Texto na posição da hierarquia da faixa — sempre SourceOver
                // (o modo de composição não se aplica a texto).
                ap.setCompositionMode(QPainter::CompositionMode_SourceOver);
                drawClipText(ap, acc.rect(), L.text, k);
                continue;
            }
            if (L.frame.isNull()) continue;
            ap.setCompositionMode(L.mode);
            drawLayer(ap, L.frame, L.s, L.rot, L.x, L.y, L.alpha, L.sX, L.sY, k, cx, cy,
                      QRect(0, 0, acc.width(), acc.height()), L.ox, L.oy);
        }
        m_compositedEpoch = m_currentFrameIndex;
        m_compositedPlayhead = m_playhead;
        m_compositedLayerCount = layers.size();
        p.drawImage(canvas.topLeft(), acc);
        drawMaskOverlay(p, canvas, k);
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
    // Posse do quadro: m_frame só é desenhado quando o clipe do topo é o DONO
    // (mesmo clipId que o worker entregou). Na virada de corte, até o decode
    // do clipe novo chegar, m_frame ainda é o frame do clipe ANTIGO — sem a
    // carência abaixo, o monitor ficava preto; com ela, o último quadro é
    // segurado por ~250 ms com transform neutro.
    const QImage& singleFrame = m_frame;
    const bool frameOwned = clip && !clip->isText && m_shownClipId == clip->id;
    // Carência do corte: segura o último quadro do clipe anterior (transform
    // neutro) enquanto o frame0 do novo decodifica — evita o PRETO em toda
    // virada de corte com decode não instantâneo.
    if (!singleFrame.isNull() && !frameOwned && cutGrace) {
        drawLayer(p, singleFrame, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, k,
                  canvas.center().x(), canvas.center().y(), canvas);
    }
    const bool transActive = m_transAlpha >= 0.0
                             && !singleFrame.isNull() && !m_underFrame.isNull();
    if (!singleFrame.isNull() && frameOwned) {
        if (transActive) {
            drawLayer(p, m_underFrame, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, k,
                      canvas.center().x(), canvas.center().y(), canvas);
            double ox = 0.0, oy = 0.0;
            if (m_transType == QStringLiteral("wipeleft")) ox = pw * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wiperight")) ox = -pw * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipeup")) oy = ph * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipedown")) oy = -ph * (1.0 - m_transAlpha);
            else if (m_transType == QStringLiteral("wipetl")) { ox = pw * (1.0 - m_transAlpha); oy = ph * (1.0 - m_transAlpha); }
            else if (m_transType == QStringLiteral("wipetr")) { ox = -pw * (1.0 - m_transAlpha); oy = ph * (1.0 - m_transAlpha); }
            else if (m_transType == QStringLiteral("wipebr")) { ox = -pw * (1.0 - m_transAlpha); oy = -ph * (1.0 - m_transAlpha); }
            else if (m_transType == QStringLiteral("wipebl")) { ox = pw * (1.0 - m_transAlpha); oy = -ph * (1.0 - m_transAlpha); }
            double a = clipAlpha;
            if (m_transType == QStringLiteral("dissolve")) a *= m_transAlpha;
            drawLayer(p, singleFrame, S, rot, tx, ty, a, SX, SY, k,
                      canvas.center().x(), canvas.center().y(), canvas, ox, oy);
        } else {
            drawLayer(p, singleFrame, S, rot, tx, ty, clipAlpha, SX, SY, k,
                      canvas.center().x(), canvas.center().y(), canvas);
        }
    }
    // Texto: qualquer texto ativo força o caminho do composto (acima) — lá os
    // textos são camadas na ordem das faixas. O caminho rápido só roda sem
    // texto, então não há bloco de texto aqui.

    // Overlay de edição de máscara (formas + alças do clipe sob edição).
    // (A margem de segurança saiu daqui: os caminhos do composto retornam
    // antes, e ela sumia em projetos com 2+ camadas — ver paintEvent.)
    drawMaskOverlay(p, canvas, k);
}

void PreviewWidget::drawPerfOverlay(QPainter& p) {
    static const bool on = qEnvironmentVariableIsSet("PIERROT_PERF_DEBUG");
    if (!on) return;
    // Estágios do tick corrente (ms). O registro vive no PlaybackEngine e é
    // preenchido pelos dois lados — o widget escreve as fases assíncronas
    // (worker, prefetch, paint) no mesmo registro em que o tick está.
    const PreviewProfiler::Frame& fr = profFrame();
    const PreviewProfiler& prof = PreviewProfiler::instance();
    const auto ms = [](double ns) { return ns / 1e6; };

    p.save();
    p.resetTransform();
    const QRect box(8, 8, 392, 190);
    p.fillRect(box, QColor(0, 0, 0, 190));
    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255));
    const QString txt = QStringLiteral(
        "fonte: %1   quadros: %2   p95 tick %3 ms  p95 decode %4 ms\n"
        "tick %5 ms = clock %6 + seek %7 + prefetch %8 + mix %9\n"
        "decode(worker->frame) %10 ms   prefetch->ready %11 ms\n"
        "paint %12 ms (comp %13)   resolve proxy %14 ms\n"
        "prefetch %15   fila %16   camadas %17   cortes %18\n"
        "dropped %19   skip %20   adaptive %21\n"
        "abre %22 fonte(s)   open total %23 ms   ultima %24 ms   pool %25 quentes")
        .arg(prof.wroteProxy() ? QStringLiteral("PROXY")
                               : QStringLiteral("original"))
        .arg(prof.frameCount())
        .arg(ms(prof.stats(QStringLiteral("tickNs")).p95), 0, 'f', 1)
        .arg(ms(prof.stats(QStringLiteral("workerNs")).p95), 0, 'f', 1)
        .arg(ms(fr.tickNs), 0, 'f', 1)
        .arg(ms(fr.clockNs), 0, 'f', 1)
        .arg(ms(fr.seekNs), 0, 'f', 1)
        .arg(ms(fr.prefetchNs), 0, 'f', 1)
        .arg(ms(fr.mixNs), 0, 'f', 1)
        .arg(ms(fr.workerNs), 0, 'f', 1)
        .arg(ms(fr.prefetchLatNs), 0, 'f', 1)
        .arg(ms(fr.paintNs), 0, 'f', 1)
        .arg(ms(fr.compositeNs), 0, 'f', 1)
        .arg(ms(fr.resolveNs), 0, 'f', 1)
        .arg(fr.prefetchHit ? QStringLiteral("HIT") : QStringLiteral("miss"))
        .arg(fr.queue)
        .arg(fr.layers)
        .arg(prof.cuts())
        .arg(m_perf.droppedTotal)
        .arg(fr.skipped)
        .arg(m_adaptiveActive
             ? QStringLiteral("ON (%1p)").arg(m_previewQuality)
             : QStringLiteral("OFF"))
        .arg(DecoderMetrics::instance().openCount())
        .arg(ms(DecoderMetrics::instance().openTotalNs()), 0, 'f', 1)
        .arg(ms(DecoderMetrics::instance().lastOpenNs()), 0, 'f', 1)
        .arg(m_decPool ? m_decPool->count() : 0);
    p.drawText(box.adjusted(10, 8, -6, -6), Qt::AlignLeft | Qt::AlignTop, txt);
    p.restore();
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
    double alpha = std::clamp(kfValue(clip->kfOpacity, clip->opacity, rel), 0.0, 1.0);
    if (clip->fadeIn > 1e-6) alpha *= std::min(1.0, rel / clip->fadeIn);
    if (clip->fadeOut > 1e-6) alpha *= std::min(1.0, (clip->dur - rel) / clip->fadeOut);
    if (alpha <= 0.0) return;

    // LAINKA no texto: jitter de posição, rotação, escala e flicker.
    double jtX = 0.0, jtY = 0.0, jtRot = 0.0, jtScale = 1.0;
    if (clip->lainkaEnabled) {
        const uint h = lainkaHash(clip->id, m_playhead);
        if (clip->lainkaJitterPos > 1e-6) {
            const double nx = ((h & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            const double ny = (((h >> 16) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            const double maxPx = clip->lainkaJitterPos * 0.5; // escala para texto
            jtX = nx * maxPx;
            jtY = ny * maxPx;
        }
        if (clip->lainkaJitterRot > 1e-6) {
            const double n = ((lainkaHashN(clip->id, m_playhead, 11) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            jtRot = n * clip->lainkaJitterRot * 0.3;
        }
        if (clip->lainkaJitterScale > 1e-6) {
            const double n = ((lainkaHashN(clip->id, m_playhead, 12) & 0xFFFF) / 65535.0) * 2.0 - 1.0;
            jtScale = 1.0 + n * clip->lainkaJitterScale * 0.01;
        }
        if (clip->lainkaFlicker > 1e-6) {
            const double n = ((lainkaHashN(clip->id, m_playhead * (clip->lainkaFlickerSpeed / 50.0), 13) & 0xFF) / 255.0);
            alpha *= (1.0 + (n * 2.0 - 1.0) * (clip->lainkaFlicker / 100.0));
            alpha = std::clamp(alpha, 0.0, 1.0);
        }
    }

    const textrender::Layout L = textrender::layout(st, W, H);
    const QRectF box = textrender::centeredBox(st, L, W, H);

    p.save();
    p.setClipRect(canvas);
    p.setOpacity(alpha);
    // Mapeia espaço de projeto (0..W x 0..H) para o canvas.
    p.translate(canvas.topLeft());
    p.scale(k, k);
    // Transform animável do clipe, em torno do centro do quadro.
    p.translate(W / 2.0 + kfValue(clip->kfTx, clip->tx, rel) + jtX,
                H / 2.0 + kfValue(clip->kfTy, clip->ty, rel) + jtY);
    p.rotate(kfValue(clip->kfRotation, clip->rotation, rel) + jtRot);
    p.scale(kfValue(clip->kfScale, clip->scale, rel) * jtScale
                * kfValue(clip->kfScaleX, clip->scaleX, rel),
            kfValue(clip->kfScale, clip->scale, rel) * jtScale
                * kfValue(clip->kfScaleY, clip->scaleY, rel));

    textrender::paint(p, st, L, box, H);

    p.restore();
}

// Tela vazia: ícone de play grande + título + dica de como começar, no tom
// monitorLabel com alfa (respeita o tema claro/escuro).
void PreviewWidget::drawEmptyMonitor(QPainter&, const QRect&) {
    // Tela preta limpa quando não há clipe — sem ícone nem texto.
}

