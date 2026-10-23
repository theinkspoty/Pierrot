// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/timeline/TimelineWidget.h"
#include "ui/timeline/TimelineCommands.h"
#include "colombina/util.h"
#include "colombina/models/ClipAttrs.h"

#include "colombina/ffmpeg/MediaCache.h"
#include "laartman/Decoder.h"
#include "colombina/mesh/BlenderBridge.h"
#include "colombina/render/MesaRenderer.h"
#include "ui/dialogs/TransformDialog.h"
#include "ui/audio/AudioEffectsDialog.h"
#include "ui/audio/TrackAudioFxDialog.h"
#include "ui/dialogs/TextEditorDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/timeline/TlLog.h"
#include "ui/core/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QCheckBox>
#include <QCursor>
#include <QList>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QSettings>
#include <QKeySequence>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QScrollBar>
#include <QMenu>
#include <QKeyEvent>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPolygon>
#include <QDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QColorDialog>
#include <QIcon>
#include <QPixmap>
#include <QTabWidget>
#include <QGroupBox>
#include <QListWidget>
#include <QFileDialog>
#include <QActionGroup>
#include <QPair>
#include <QVariantAnimation>
#include <QStyle>
#include <QStyleOptionRubberBand>
#include <QRubberBand>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr int kMarginR = 60;
// Tamanhos dos presets de estilo das faixas (minimizada / grande).
constexpr int kPresetMinV = 34, kPresetMinA = 28;
constexpr int kPresetMaxV = 100, kPresetMaxA = 78;

}

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    qApp->installEventFilter(this);
    setAcceptDrops(true);
    setMinimumHeight(180);
    // A cena é totalmente opaca: evitar a limpeza de fundo da backing store
    // poupa uma passada inteira por repaint.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);

    m_hbar = new QScrollBar(Qt::Horizontal, this);
    m_vbar = new QScrollBar(Qt::Vertical, this);
    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) {
        m_viewStart = v / m_pps;
        refreshView();
    });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) {
        m_viewTop = v;
        refreshView();
    });
    connect(&MediaCache::instance(), &MediaCache::waveformReady, this,
            [this](const QString&, int) { invalidateScene(); });
    connect(&MediaCache::instance(), &MediaCache::thumbnailReady, this,
            [this](const QString&, double) { invalidateScene(); });
    // Qualquer alteração estrutural do projeto invalida a cena estática
    // (clipes, marcadores, etc.). Durante um arraste contínuo (mover/aparar
    // clipe, redimensionar faixa) `modified` é emitido por frame: nesse caso
    // só redesenhamos a cena reaproveitando o cache de conteúdo dos clipes —
    // a posição muda, mas a onda/thumb do conteúdo é idêntica.
    connect(this, &TimelineWidget::modified, this, [this]() {
        if (m_dragMode == MoveClip || m_dragMode == TrimLeft
            || m_dragMode == TrimRight || m_dragMode == ResizeTrack)
            refreshView();
        else if (m_dragMode == FadeIn || m_dragMode == FadeOut)
            // Fade/opacidade não mudam o conteúdo cacheado dos clipes (onda,
            // thumb) nem o conjunto de clipes — só o alpha de composição.
            // invalidateScene() aqui refazia o rebuildClipIndex() por mousemove
            // e anulava a versão leve usada no arraste.
            invalidateSceneContent();
        else
            invalidateScene();
    });
    // Gêmeo leve do modified acima: durante o arraste a timeline emite
    // clipGeometryChanged (só repinta a cena reaproveitando o cache de conteúdo
    // — a posição muda, onda/thumb não) e GUARDA o modified completo para o fim
    // do gesto. Sem ESTA conexão, nada invalida a cena durante o arraste e o
    // clipe parece parado (só "teletransporta" ao soltar, quando o modified
    // final chega). O MainWindow escuta o mesmo sinal só para o preview.
    connect(this, &TimelineWidget::clipGeometryChanged, this, [this]() {
        refreshView();
    });

    // Zoom com transição linear: o pixel-âncora fica parado sob o cursor
    // enquanto o pps interpola de forma constante (sem easing de curva).
    m_zoomAnim = new QVariantAnimation(this);
    m_zoomAnim->setDuration(140);
    m_zoomAnim->setEasingCurve(QEasingCurve::Linear);
    connect(m_zoomAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        double pps;
        double newStart;
        if (m_zoomRectMode) {
            const double k = v.toDouble();
            pps = m_zoomStartPps + (m_zoomEndPps - m_zoomStartPps) * k;
            newStart = m_zoomStartView + (m_zoomEndView - m_zoomStartView) * k;
        } else {
            pps = v.toDouble();
            newStart = m_zoomAnchorT - m_zoomAnchorPixel / pps;
        }
        m_pps = pps;
        updateScrollRanges();
        m_hbar->setValue(qRound(newStart * pps));
        m_viewStart = m_hbar->value() / pps;
        refreshView();
    });

    // Autoscroll: enquanto o mouse segura a agulha/clipe encostado na borda da
    // view, a timeline rola sozinha para acompanhar o arrasto.
    m_autoScroll = new QTimer(this);
    m_autoScroll->setInterval(16);
    m_autoScroll->setTimerType(Qt::PreciseTimer);
    connect(m_autoScroll, &QTimer::timeout, this, &TimelineWidget::autoScrollTick);

    // Throttle do playheadChanged durante o arraste da agulha: o preview
    // decodifica a cada pedido, e o trabalho de updateFrame() na UI thread
    // segura o mouseMove (a agulha passa a atrasar o cursor em movimentos
    // rápidos). 33 ms (~30 Hz) é imperceptível no preview; a AGULHA continua
    // imediata.
    m_playheadThrottle = new QTimer(this);
    m_playheadThrottle->setInterval(33);
    m_playheadThrottle->setSingleShot(false);
    m_playheadThrottle->setTimerType(Qt::PreciseTimer);
    connect(m_playheadThrottle, &QTimer::timeout, this, &TimelineWidget::flushPlayheadThrottled);
}

void TimelineWidget::invalidateScene() {
    // Mudança estrutural: descarta o cache de conteúdo dos clipes para que
    // onda/thumb/envelope sejam re-renderizados na próxima passada.
    ++m_clipEpoch;
    m_staticDirty = true;
    rebuildClipIndex();
    update();
}

void TimelineWidget::invalidateSceneContent() {
    // Versão leve: invalida só o cache de conteúdo (fade/opacity), sem
    // reconstruir o índice de clipes — chamada durante arraste contínuo.
    ++m_clipEpoch;
    m_staticDirty = true;
    update();
}

void TimelineWidget::refreshView() {
    m_staticDirty = true;
    update();
}

void TimelineWidget::setProject(Project* p) {
    m_project = p;
    hideTrackRename();
    m_selected.clear();
    m_secondarySelected.clear();
    clearTrackSelection();
    m_clipPix.clear();
    m_clipBytes = 0;
    m_cursorT = -1.0;
    m_lastMax = -1;
    m_lastHoverPos = QPoint(-999, -999);
    m_lastHoverClipId.clear();
    m_lastHoverRow = -1;
    m_lastHoverAudio = false;
    emit selectionChanged(QString());
    rebuildClipIndex();
    invalidateScene();
    updateScrollRanges();
}

void TimelineWidget::addTrack(bool audio) {
    if (!m_project) return;
    emit editStart();
    m_project->addTrack(audio);
    invalidateScene();
    updateScrollRanges();
    emit modified();
}

// Cria uma faixa de efeitos (azul) no topo da pilha de vídeo e a seleciona.
// A faixa nasce VAZIA: os clipes que forem movidos para ela recebem os efeitos
// escolhidos no painel da faixa (Track::fxEffects).
void TimelineWidget::addFxTrack() {
    if (!m_project) return;
    emit editStart();
    m_project->addFxTrack(true);
    invalidateScene();
    updateScrollRanges();
    setTrackSel(0, false);   // a nova faixa entrou no índice 0 (topo)
    refreshView();
    emit modified();
}

// Cria uma faixa na seção de gravação (vermelha) e a seleciona.
void TimelineWidget::addRecordingTrack() {
    if (!m_project) return;
    emit editStart();
    m_project->addRecordingTrack();
    setRecTrackSel((int)m_project->recordingTracks.size() - 1);
    invalidateScene();
    updateScrollRanges();
    refreshView();
    emit modified();
}


void TimelineWidget::openGradingForClip(const QString& clipId) {
    Clip* c = findClipById(clipId);
    if (!c) return;
    showGradingDialog(c);
}

void TimelineWidget::openTextEditorForClip(const QString& clipId) {
    Clip* c = findClipById(clipId);
    if (!c) return;
    showTextEditorDialog(c);
}

// ── Multicâmera ──────────────────────────────────────────────────────────

void TimelineWidget::createMulticamFromSelection() {
    if (!m_project || m_selected.size() < 2) return;

    struct Src {
        QString clipId;
        QString mediaId;
        double in = 0.0;
        double pos = 0.0;
        double dur = 0.0;
        int track = -1;
    };
    QVector<Src> srcs;
    for (const QString& id : m_selected) {
        Clip* c = findClipById(id);
        if (!c || c->isText || c->hasMulticam()) continue;
        const MediaItem* m = m_project->findMedia(c->mediaId);
        if (!m || !m->hasVideo) continue;
        int row = -1;
        for (int i = 0; i < (int)m_project->videoTracks.size(); ++i) {
            for (const Clip& cl : m_project->videoTracks[i].clips) {
                if (cl.id == id) { row = i; break; }
            }
            if (row >= 0) break;
        }
        if (row < 0) continue; // vídeo-only no MVP
        srcs.push_back({id, c->mediaId, c->in, c->pos, c->dur, row});
    }
    if (srcs.size() < 2) return;

    std::sort(srcs.begin(), srcs.end(),
              [](const Src& a, const Src& b) { return a.pos < b.pos; });

    QStringList sources;
    QVector<double> ins;
    for (const Src& s : srcs) {
        if (sources.contains(s.mediaId)) continue;
        sources.append(s.mediaId);
        ins.append(s.in);
    }
    if (sources.size() < 2) return;

    const double minPos = srcs.first().pos;
    double dur = srcs.first().pos + srcs.first().dur - minPos;
    for (const Src& s : srcs)
        dur = std::min(dur, (s.pos + s.dur) - minPos);
    if (dur < kMinDur) return;

    emit undoLabel(tr("Criar multicam"));
    emit editStart();

    QStringList toRemove;
    for (const Src& s : srcs) toRemove.append(s.clipId);
    for (Track& tr : m_project->videoTracks) {
        QVector<Clip> keep;
        for (Clip& cl : tr.clips) {
            if (toRemove.contains(cl.id)) continue;
            keep.append(cl);
        }
        tr.clips = keep;
    }

    Clip mc;
    mc.id = newId();
    mc.isMulticam = true;
    mc.multicamSources = sources;
    mc.multicamIns = ins;
    mc.defaultAngle = 0;
    mc.mediaId = sources.first();
    mc.pos = minPos;
    mc.in = ins.first();
    mc.dur = dur;
    mc.name = tr("Multicam (%1 ângulos)").arg(sources.size());

    const int vRow = srcs.first().track;
    auto& clips = m_project->videoTracks[vRow].clips;
    auto it = clips.begin();
    while (it != clips.end() && it->pos <= mc.pos) ++it;
    clips.insert(it, mc);

    setSelection(mc.id);
    ensurePlayheadVisible();
    updateScrollRanges();
    invalidateScene();
    update();
    emit modified();
}

void TimelineWidget::setMulticamAngleAtPlayhead(int angle) {
    if (!m_project) return;

    Clip* c = nullptr;
    for (const QString& id : m_selected) {
        Clip* cc = findClipById(id);
        if (cc && cc->hasMulticam()) { c = cc; break; }
    }
    if (!c) {
        for (Track& tr : m_project->videoTracks) {
            for (Clip& cl : tr.clips) {
                if (cl.hasMulticam() && m_playhead >= cl.pos - 1e-9
                    && m_playhead < cl.pos + cl.dur - 1e-9) {
                    c = &cl;
                    break;
                }
            }
            if (c) break;
        }
    }
    if (!c || !c->hasMulticam()) return;

    angle = std::clamp(angle, 0, int(c->multicamSources.size()) - 1);
    emit undoLabel(tr("Ângulo multicam %1").arg(angle + 1));
    emit editStart();
    const double rel = std::max(0.0, m_playhead - c->pos);
    upsertKeyframe(c->kfAngle, rel, (double)angle, KfStep);
    if (c->kfAngle.size() == 1) c->defaultAngle = angle;
    invalidateScene();
    update();
    emit modified();
}

// Cria um clipe independente de texto na faixa de vídeo `row` (ou numa livre)
// no instante `t`, com duração padrão, e abre o editor de texto.
void TimelineWidget::addTextClipAt(int row, double t) {
    if (!m_project) return;
    constexpr double kTextDur = 3.0;
    auto overlap = [t](const QVector<Clip>& clips) {
        for (const Clip& o : clips)
            if (o.pos < t + kTextDur - 1e-9 && o.pos + o.dur > t + 1e-9) return true;
        return false;
    };
    int vRow = row;
    if (vRow < 0 || vRow >= (int)m_project->videoTracks.size()
        || m_project->videoTracks[vRow].locked || overlap(m_project->videoTracks[vRow].clips)) {
        vRow = -1;
        for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
            if (!m_project->videoTracks[i].locked && !overlap(m_project->videoTracks[i].clips)) {
                vRow = i;
                break;
            }
    }
    if (vRow < 0) {
        m_project->addTrack(false);
        vRow = (int)m_project->videoTracks.size() - 1;
    }

    emit editStart();
    Clip c;
    c.id = newId();
    c.isText = true;
    c.pos = t;
    c.in = 0.0;
    c.dur = kTextDur;
    c.name = tr("Texto");
    c.text.text = tr("Texto");
    // Centralizado por padrão (0.5/0.5 no quadro, alinhamento centro).
    c.text.textX = 0.5;
    c.text.textY = 0.5;
    c.text.textAlign = 0;
    auto& clips = m_project->videoTracks[vRow].clips;
    auto it = clips.begin();
    while (it != clips.end() && it->pos <= c.pos) ++it;
    clips.insert(it, c);
    setSelection(c.id);
    invalidateScene();
    update();
    emit modified();

    Clip* created = findClipById(c.id);
    if (created) showTextEditorDialog(created);
}


void TimelineWidget::resizeEvent(QResizeEvent*) {
    const int bar = m_vbar->sizeHint().width();
    const int hbarH = m_hbar->sizeHint().height();
    m_hbar->setGeometry(0, height() - hbarH, width() - bar, hbarH);
    m_vbar->setGeometry(width() - bar, 0, bar, height() - hbarH);
    invalidateScene();
    updateScrollRanges();
}

void TimelineWidget::updateScrollRanges() {
    if (!m_project) return;
    const int bar = m_vbar->sizeHint().width();
    const int hbarH = m_hbar->sizeHint().height();
    const int viewW = width() - bar;
    const int viewH = height() - hbarH - kRulerH;

    const double total = m_project->duration();
    int rowsH = 0;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        rowsH += trackVisible(i, false) ? trackH(i, false) : 0;
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        rowsH += trackVisible(i, true) ? trackH(i, true) : 0;
    for (int i = 0; i < (int)m_project->recordingTracks.size(); ++i)
        rowsH += recTrackVisible(i) ? recTrackH(i) : 0;
    int foldersH = folderStripsAboveVideo(-1) * kFolderH;
    foldersH += folderStripsAboveAudio((int)m_project->audioTracks.size() - 1) * kFolderH;
    const int totalH = kRulerH + rowsH + foldersH + 20;
    const int totalW = kHeaderW + (int)(total * m_pps) + kMarginR;

    // Se o range novo é menor que a posição atual, a QScrollBar vai GRUDAR o
    // valor para dentro do range — a view inteira pula de lugar ("timeline
    // resetando"). Registra sempre; colapso grande dispara dump automático.
    const int newMax = std::max(0, totalW - viewW);
    if (newMax < m_hbar->value()) {
        TlLog::note(QStringLiteral("range encolheu: scroll %1 -> %2 (dur=%3 pps=%4)")
                        .arg(m_hbar->value()).arg(newMax)
                        .arg(total, 0, 'f', 2).arg(m_pps, 0, 'f', 2));
        if (newMax < m_hbar->value() * 0.5)
            TlLog::dump(QStringLiteral("range encolheu %1 -> %2")
                            .arg(m_hbar->value()).arg(newMax));
    }
    m_hbar->setRange(0, newMax);
    m_hbar->setPageStep(std::max(1, viewW));
    m_vbar->setRange(0, std::max(0, totalH - viewH));
    m_vbar->setPageStep(std::max(1, viewH));

    // Mudança grande no alcance horizontal (edição, zoom, resize) entra no
    // registro — ajuda a correlacionar com cliques que "voltam pro início".
    if (m_lastMax >= 0 && std::abs(newMax - m_lastMax) > 2000)
        TlLog::note(QStringLiteral("range: max %1 -> %2 (dur=%3 pps=%4)")
                        .arg(m_lastMax).arg(newMax)
                        .arg(total, 0, 'f', 2).arg(m_pps, 0, 'f', 2));
    m_lastMax = newMax;

    m_viewStart = m_hbar->value() / m_pps;
    m_viewTop = m_vbar->value();
}

// Versão barata de updateScrollRanges() para uso durante o arraste: só garante
// que a barra horizontal alcance `tEnd`. Mover/trimar clipes MUDA a duração do
// projeto (Project::duration() é o máx de pos+dur), então a premissa de que o
// range horizontal é constante durante o arraste é falsa — sem isto, esticar o
// último clipe faz a borda de trim travar no limite da barra. Não recalcula o
// range vertical (alturas de faixa/pastas não mudam no arraste) e nunca
// encolhe, evitando que a view pule de lugar a cada mousemove.
void TimelineWidget::ensureScrollRangeReaches(double tEnd) {
    if (!m_project) return;
    const int viewW = std::max(1, width() - m_vbar->sizeHint().width());
    const int wantMax = std::max(0, kHeaderW + (int)(tEnd * m_pps) + kMarginR - viewW);
    if (wantMax > m_hbar->maximum()) {
        m_hbar->setMaximum(wantMax);
        m_lastMax = wantMax;
    }
    m_viewStart = m_hbar->value() / m_pps;
}

// timeToX — implementado em TimelinePaint.cpp

// xToTime, trackH, rowY, rowFromY, clipAt, volLineY, clipVolLineY — implementados em TimelinePaint.cpp

// Abre o menu de estilos das faixas ao lado do cursor (acionado pelo botão
// "Estilo" da barra de ferramentas da timeline).
void TimelineWidget::showTrackPresetMenu() {
    QMenu menu(this);
    QAction* minA = menu.addAction(tr("Minimizada"));
    QAction* norA = menu.addAction(tr("Normal (padrão)"));
    QAction* grdA = menu.addAction(tr("Grande"));
    minA->setCheckable(true);
    norA->setCheckable(true);
    grdA->setCheckable(true);
    minA->setChecked(m_trackPreset == 0);
    norA->setChecked(m_trackPreset == 1);
    grdA->setChecked(m_trackPreset == 2);
    QAction* act = menu.exec(QCursor::pos());
    if (act == minA) applyTrackPreset(0);
    else if (act == norA) applyTrackPreset(1);
    else if (act == grdA) applyTrackPreset(2);
}

// Aplica um preset de estilo às faixas: 0 = minimizada, 1 = normal, 2 = grande.
// Recurso experimental: avisa o usuário na primeira vez.
void TimelineWidget::applyTrackPreset(int preset) {
    if (!m_project) return;
    int vh, ah;
    if (preset == 0) { vh = kPresetMinV; ah = kPresetMinA; }
    else if (preset == 2) { vh = kPresetMaxV; ah = kPresetMaxA; }
    else { vh = 0; ah = 0; } // normal = altura padrão
    for (Track& t : m_project->videoTracks) t.height = vh;
    for (Track& t : m_project->audioTracks) t.height = ah;
    m_trackPreset = preset;
    updateScrollRanges();
    invalidateScene();
    update();
    emit modified();
}

// rowY, rowFromY, clipAt — implementados em TimelinePaint.cpp
// (clips giữ ở đây vì clipAt(row, audio, t) được dùng ở nhiều nơi)

Clip* TimelineWidget::findClipById(const QString& id) {
    if (!m_project) return nullptr;
    auto it = m_clipIndex.find(id);
    if (it != m_clipIndex.end() && (*it) && (*it)->id == id)
        return *it;
    // O índice pode guardar um ponteiro obsoleto se um vetor de clipes foi
    // mutado sem rebuild (ex.: moveClipToTrack/duplicateClip durante um
    // arrasto). Cai para a varredura para nunca devolver memória pendurada.
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    return nullptr;
}

void TimelineWidget::rebuildClipIndex() {
    m_clipIndex.clear();
    if (!m_project) return;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            m_clipIndex.insert(c.id, &c);
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            m_clipIndex.insert(c.id, &c);
}

Track* TimelineWidget::trackOf(Clip* c) {
    if (!m_project || !c) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& x : t.clips)
            if (&x == c) return &t;
    for (Track& t : m_project->audioTracks)
        for (Clip& x : t.clips)
            if (&x == c) return &t;
    return nullptr;
}

double TimelineWidget::snapTime(double t) const {
    if (!m_snap) return t;
    const double fps = m_project ? (double)m_project->fps : 30.0;
    return std::round(t * fps) / fps;
}

void TimelineWidget::setPlayhead(double t) {
    QElapsedTimer dbg; dbg.start();
    const double prev = m_playhead;
    m_playhead = std::max(0.0, t);
    // Registro sempre-ligado: só transições relevantes (seeks/saltos), não
    // cada frame da reprodução. Salto grande para trás dispara o dump
    // automático do histórico — evidência do "volta pro começo".
    if (std::fabs(m_playhead - prev) > 0.25)
        TlLog::note(QStringLiteral("setPlayhead %1 -> %2 (playing=%3 vs=%4 pps=%5 sc=%6/%7)")
                        .arg(prev, 0, 'f', 3).arg(m_playhead, 0, 'f', 3)
                        .arg(m_playing ? 1 : 0)
                        .arg(m_viewStart, 0, 'f', 1).arg(m_pps, 0, 'f', 1)
                        .arg(m_hbar->value()).arg(m_hbar->maximum()));
    if (prev > 5.0 && m_playhead < prev - std::max(5.0, prev * 0.25)) {
        TlLog::note(QStringLiteral("!! SALTO PARA TRÁS detectado"));
        TlLog::dump(QStringLiteral("agulha saltou %1 -> %2")
                        .arg(prev, 0, 'f', 2).arg(m_playhead, 0, 'f', 2));
    }
    // Simbiose das agulhas: durante a reprodução a agulha branca acompanha a
    // vermelha, a não ser que o mouse esteja sobre a régua (aí ela vira prévia
    // da posição do cursor). Assim elas nunca ficam "descoladas" à toa.
    if (m_playing && !m_mouseOnRuler)
        m_cursorT = m_playhead;
    ensurePlayheadVisible();
    m_perfPlayheadMs = dbg.elapsed();
    update();
}

void TimelineWidget::setPlaying(bool p) {
    m_playing = p;
    if (p) m_cursorT = m_playhead; // ao começar, alinha a branca na vermelha
    update();
}

// Enfileira a posição para o preview. A agulha JÁ foi movida (setPlayhead,
// imediato); aqui só se adia o aviso que faz o preview decodificar, para não
// segurar o mouseMove. O primeiro pedido arma o timer; os seguintes só
// atualizam o valor alvo (coalesce) — o preview sempre recebe a posição mais
// recente, nunca uma intermediária.
void TimelineWidget::emitPlayheadThrottled(double t) {
    m_pendingPlayhead = t;
    m_playheadThrottleDirty = true;
    if (!m_playheadThrottleArmed) {
        m_playheadThrottleArmed = true;
        m_playheadThrottle->start();
    }
}

// Tick do timer: emite a posição pendente (a mais recente). Se nada mudou
// desde o último tick, não emite nada — o preview não gasta decode à toa.
void TimelineWidget::flushPlayheadThrottled() {
    if (!m_playheadThrottleDirty) return;
    m_playheadThrottleDirty = false;
    emit playheadChanged(m_pendingPlayhead);
}

// Fim do gesto: para o timer e emite a posição EXATA, para o preview nunca
// ficar um quadro atrás do cursor (o throttle pode ter engolido o último).
void TimelineWidget::flushPlayheadNow() {
    if (m_playheadThrottleArmed) {
        m_playheadThrottle->stop();
        m_playheadThrottleArmed = false;
    }
    if (m_playheadThrottleDirty) {
        m_playheadThrottleDirty = false;
        emit playheadChanged(m_pendingPlayhead);
    }
}

void TimelineWidget::ensurePlayheadVisible() {
    if (!m_project) return;
    const double px = timeToX(m_playhead);
    const int viewW = width() - m_vbar->sizeHint().width();
    // Follow contínuo (estilo Vegas): rola só quando a agulha chega PERTO DA
    // BORDA direita (95%). Antes o gatilho era 75% e a área de rolagem parecia
    // "começar no meio" — a view rolava cedo demais. Ao rolar, a agulha é
    // recolocada a ~90% (à direita), não no centro: menos teleporte, mais
    // conteúdo já reproduzido à vista.
    const double rightEdge = kHeaderW + (viewW - kHeaderW) * 0.95;
    if (px < kHeaderW || px > rightEdge) {
        const double frac = px < kHeaderW ? 0.3 : 0.9;
        const int before = m_hbar->value();
        // scroll alvo = tempo×pps − fração×largura_útil  (não misturar
        // segundos com pixels: era isso que teleportava a view pro zero).
        const int target = qBound(0,
                                  (int)std::llround(m_playhead * m_pps
                                                    - (viewW - kHeaderW) * frac),
                                  std::max(0, m_hbar->maximum()));
        m_hbar->setValue(target);
        TlLog::note(QStringLiteral("follow: px=%1 alvo=%2 scroll %3 -> %4 (max=%5)")
                        .arg(px, 0, 'f', 0).arg(target)
                        .arg(before).arg(m_hbar->value()).arg(m_hbar->maximum()));
    }
    m_viewStart = m_hbar->value() / m_pps;
}

// startAutoScroll — implementado em TimelineDrag.cpp
// stopAutoScroll — implementado em TimelineDrag.cpp
// autoScrollTick — implementado em TimelineDrag.cpp
// paintEvent, renderScene, renderOverlays, drawClip, drawTextClipBody,
// drawAudioWaveform, drawVideoThumbs, drawEnvelope, drawFadeCorners,
// drawOpacityHandle, drawTransitionIndicator, drawKeyframeDiamonds,
// drawTrackHeader, drawFolderStrip — implementados em TimelinePaint.cpp

void TimelineWidget::zoomBy(double factor, double centerT) {
    if (!m_project) return;
    const double target = std::clamp(m_pps * factor, kMinPps, kMaxPps);
    animateZoomTo(target, centerT);
}

// Transição linear de zoom mantendo o instante `anchorT` no mesmo pixel da
// view. A cada tick o pps interpola e o scroll é reancorado — o resultado é
// um zoom contínuo e sem saltos (ponto forte do app).
void TimelineWidget::animateZoomTo(double targetPps, double anchorT) {
    if (!m_project) return;
    targetPps = std::clamp(targetPps, kMinPps, kMaxPps);
    if (std::fabs(targetPps - m_pps) < 1e-9) return;
    const double anchorPixel = timeToX(anchorT) - kHeaderW;
    m_zoomAnchorT = anchorT;
    m_zoomAnchorPixel = anchorPixel;
    m_zoomRectMode = false;
    if (m_zoomAnim->state() != QVariantAnimation::Stopped)
        m_zoomAnim->stop();
    m_zoomAnim->setStartValue(m_pps);
    m_zoomAnim->setEndValue(targetPps);
    m_zoomAnim->start();
}



// resizeHandleAt — implementado em TimelineDrag.cpp
// clipVolAt — implementado em TimelineDrag.cpp
// volRowAt — implementado em TimelineDrag.cpp
// headerBtnAt — implementado em TimelineDrag.cpp
bool TimelineWidget::trackLocked(const Clip* c) const {
    if (!m_project || !c) return false;
    for (const Track& t : m_project->videoTracks)
        for (const Clip& x : t.clips)
            if (&x == c) return t.locked;
    for (const Track& t : m_project->audioTracks)
        for (const Clip& x : t.clips)
            if (&x == c) return t.locked;
    return false;
}



// dragEnterEvent — implementado em TimelineDrag.cpp
// dragMoveEvent — implementado em TimelineDrag.cpp
// dragLeaveEvent — implementado em TimelineDrag.cpp
// dropEvent — implementado em TimelineDrag.cpp
// showDropHover — implementado em TimelineDrag.cpp
// hideDropHover — implementado em TimelineDrag.cpp
// dropMediaAt — implementado em TimelineDrag.cpp

void TimelineWidget::refreshSettings() {
    invalidateScene();
}

// finishDrop — implementado em TimelineDrag.cpp

void TimelineWidget::cutAtPlayhead() {
    emit undoLabel(tr("Dividir no playhead"));
    TimelineCommands::cutAtPlayhead(this);
}

void TimelineWidget::cutAndDelete() {
    emit undoLabel(tr("Dividir e excluir"));
    // Sem ímã, não junta: remove o trecho da direita deixando o vão.
    TimelineCommands::cutAndDelete(this, rippleCloseEnabled());
}

void TimelineWidget::splitClipAt(Clip* c, double t, QStringList* newIds) {
    if (!c || !m_project) return;
    QStringList ids;
    if (!c->groupId.isEmpty()) {
        for (Clip* m : groupMembers(c->groupId)) ids.append(m->id);
    } else {
        ids.append(c->id);
    }

    const bool grouped = ids.size() > 1;
    const QString fg = grouped ? newId() : QString();
    const QString bg = grouped ? newId() : QString();
    for (const QString& id : ids) {
        Clip* cc = findClipById(id);
        if (!cc) continue;
        if (t <= cc->pos + 1e-6 || t >= cc->pos + cc->dur - 1e-6) {
            if (grouped) cc->groupId = newId();
            continue;
        }
        Clip b = *cc;
        b.id = newId();
        b.pos = t;
        b.in = cc->in + (t - cc->pos);
        b.dur = cc->dur - (t - cc->pos);
        cc->dur = t - cc->pos;
        cc->groupId = fg;
        b.groupId = bg;
        if (newIds) newIds->append(b.id);
        Track* tr = trackOf(cc);
        if (tr) {
            int idx = -1;
            for (int i = 0; i < tr->clips.size(); ++i)
                if (tr->clips[i].id == cc->id) { idx = i; break; }
            if (idx >= 0) tr->clips.insert(idx + 1, b);
        }
    }
}

void TimelineWidget::duplicateClip(Clip* c) {
    if (!c) return;
    emit undoLabel(tr("Duplicar clipe"));
    const double shift = c->dur;
    // Usa ids (não ponteiros): push_back pode realocar o vetor e invalidar
    // os ponteiros ainda não processados.
    QStringList ids = c->groupId.isEmpty() ? QStringList{c->id} : QStringList();
    if (ids.isEmpty())
        for (Clip* m : groupMembers(c->groupId)) ids.append(m->id);

    // Cópia unificada (estilo Vegas): se o clipe tem texto, pergunta se a
    // cópia é independente ou compartilha o texto com o original.
    bool hasText = false;
    for (const QString& id : ids) {
        Clip* s = findClipById(id);
        if (s && !m_project->textStyleFor(*s)->isEmpty()) { hasText = true; break; }
    }
    enum CopyKind { Independent, Unified };
    CopyKind kind = Independent;
    if (hasText) {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Duplicar clipe"));
        auto* rbInd = new QRadioButton(tr("Cópia independente"), &dlg);
        rbInd->setToolTip(tr("Cada clipe tem o próprio texto e estilo."));
        auto* rbUni = new QRadioButton(tr("Cópia unificada"), &dlg);
        rbUni->setToolTip(tr("Os clipes compartilham o mesmo texto e estilo: "
                             "editar um atualiza os outros."));
        rbUni->setChecked(true);
        auto* lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel(tr("O clipe tem texto. Como criar a cópia?"), &dlg));
        lay->addWidget(rbInd);
        lay->addWidget(rbUni);
        auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        lay->addWidget(btns);
        if (dlg.exec() != QDialog::Accepted) return;
        kind = rbUni->isChecked() ? Unified : Independent;
    }

    // Cópia unificada: converte o texto do original num recurso compartilhado
    // (se ainda não for) e a cópia passa a referenciar o mesmo recurso.
    emit editStart();
    QString sharedRes;
    if (kind == Unified) {
        for (const QString& id : ids) {
            Clip* s = findClipById(id);
            if (s && !m_project->textStyleFor(*s)->isEmpty()) {
                if (s->textResourceId.isEmpty())
                    m_project->bindTextResource(*s);
                sharedRes = s->textResourceId;
                break;
            }
        }
    }

    const QString gid = newId();
    QString firstDup;
    for (const QString& id : ids) {
        Clip* src = findClipById(id);
        if (!src) continue;
        Clip b = *src;
        b.id = newId();
        b.groupId = gid;
        b.pos = snapTime(src->pos + shift);
        if (kind == Unified && !sharedRes.isEmpty()
            && !m_project->textStyleFor(*src)->isEmpty()) {
            b.textResourceId = sharedRes;
            b.text = TextStyle(); // o texto vem do recurso compartilhado
        }
        Track* tr = trackOf(src);
        if (tr) {
            // Insere em ordem cronológica (push_back quebrava a ordem por pos
            // e o clipAt, que assumia lista ordenada, não achava o duplicado).
            auto it = tr->clips.begin();
            while (it != tr->clips.end() && it->pos <= b.pos) ++it;
            tr->clips.insert(it, b);
            if (id == c->id) firstDup = b.id;
        }
    }
    // O insert pode realocar o vetor: reconstrói o índice para que o
    // deslocamento da cópia durante o Ctrl-arrasto (findClipById logo após)
    // encontre os clipes duplicados e o par não fique separado do mouse.
    rebuildClipIndex();
    if (!firstDup.isEmpty()) setSelection(firstDup);
    updateScrollRanges();
    update();
    emit modified();
}

// Diálogo de velocidade: digita o fator (0.1x-4x). Aplica ao clipe e, se
// vinculado, aos membros do grupo (vídeo + faixas de áudio) mantendo a posição
// da borda DIREITA (a duração do clipe é retida no timeline).


void TimelineWidget::removeClipsByIds(const QStringList& ids) {
    if (!m_project) return;
    auto eraseIn = [&ids](QVector<Clip>& clips) {
        clips.erase(std::remove_if(clips.begin(), clips.end(),
                                   [&ids](const Clip& c) { return ids.contains(c.id); }),
                    clips.end());
    };
    for (Track& t : m_project->videoTracks)
        eraseIn(t.clips);
    for (Track& t : m_project->audioTracks)
        eraseIn(t.clips);
}

void TimelineWidget::deleteSelected() {
    emit undoLabel(tr("Excluir clipes"));
    TimelineCommands::deleteSelected(this);
}

void TimelineWidget::deleteSelectedLeaveGap() {
    emit undoLabel(tr("Excluir clipes (deixando espaço)"));
    TimelineCommands::deleteSelectedLeaveGap(this);
}

void TimelineWidget::deleteLoopRipple() {
    emit undoLabel(tr("Excluir região de loop (ripple)"));
    TimelineCommands::deleteLoopRipple(this);
}

void TimelineWidget::deleteLoopLeaveGap() {
    emit undoLabel(tr("Excluir região de loop"));
    TimelineCommands::deleteLoopLeaveGap(this);
}

// Delete: apaga somente os clipes selecionados. A seleção de faixa é apenas
// informativa (mostra em qual faixa você está) e NÃO apaga faixa nenhuma.
// Respeita o ímã: desligado, deixa o vão (sem ripple) como o Shift+Delete.
void TimelineWidget::deleteSelection() {
    if (rippleCloseEnabled())
        deleteSelected();
    else
        deleteSelectedLeaveGap();
}

void TimelineWidget::deleteClipBeforePlayhead() {
    TimelineCommands::deleteClipBeforePlayhead(this);
}

void TimelineWidget::deleteClipAfterPlayhead() {
    TimelineCommands::deleteClipAfterPlayhead(this);
}

// isSelected, setSelection, toggleSelection, isTrackSelected, setTrackSel,
// toggleTrackSel, selectTrackRange, selectTrackRightClick, clearTrackSelection,
// selectAllClips, selectInMarquee — implementados em TimelineSelection.cpp

// clearTrackSelection — implementado em TimelineSelection.cpp

// Nº de pastas de vídeo cujo membro mais alto (menor índice) está em um
// índice <= videoIdx. Com V1 no topo, cada pasta ocupa kFolderH acima do
// folderStripsAboveVideo, folderStripsAboveAudio, folderStripRect,
// folderStripAt, folderArrowRect, trackVisible, drawFolderStrip,
// volLineY, clipVolLineY — implementados em TimelinePaint.cpp

void TimelineWidget::toggleGroupCollapsed(const QString& gid) {
    TrackGroup* g = m_project ? m_project->findGroup(gid) : nullptr;
    if (!g) return;
    emit editStart();
    g->collapsed = !g->collapsed;
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        m_secondarySelected.clear();
        emit selectionChanged(QString());
    }
    clearTrackSelection();
    updateScrollRanges();
    invalidateScene();
    emit modified();
}

void TimelineWidget::createTrackGroup() {
    if (!m_project || m_selTracks.size() < 2) return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Agrupar faixas"), tr("Nome da pasta:"), QLineEdit::Normal,
        tr("Pasta %1").arg(m_project->trackGroups.size() + 1), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    emit editStart();
    TrackGroup g;
    g.id = newId();
    g.name = name.trimmed();
    m_project->trackGroups.append(g);
    for (const TrackSel& s : m_selTracks) {
        if (s.row < 0) continue;
        Track* t = s.audio
            ? (s.row < (int)m_project->audioTracks.size() ? &m_project->audioTracks[s.row] : nullptr)
            : (s.row < (int)m_project->videoTracks.size() ? &m_project->videoTracks[s.row] : nullptr);
        if (t) t->groupId = g.id;
    }
    updateScrollRanges();
    invalidateScene();
    emit modified();
}

// ── Renomeio inline da faixa (duplo clique no nome, estilo Premiere) ──────
// Um QLineEdit sem bordas cobre a área do nome; Enter/perda de foco confirma,
// Esc cancela. O texto só muda de verdade se for diferente do original.
void TimelineWidget::beginTrackRename(int row, bool audio) {
    if (!m_project) return;
    Track* trk = audio ? &m_project->audioTracks[row] : &m_project->videoTracks[row];
    if (m_trackRename && m_trackRename->isVisible()
        && m_renameRow == row && m_renameAudio == audio)
        return;
    m_renameRow = row;
    m_renameAudio = audio;
    m_renameOriginal = trk->name;
    const int y = audio ? rowY(-1, row) : rowY(row, -1);
    if (!m_trackRename) {
        m_trackRename = new QLineEdit(this);
        m_trackRename->setObjectName(QStringLiteral("trackRename"));
        m_trackRename->setFrame(false);
        m_trackRename->installEventFilter(this);
        connect(m_trackRename, &QLineEdit::editingFinished,
                this, &TimelineWidget::commitTrackRename);
        const QColor& bg = themeColors().trackLabelBg;
        const QColor& fg = themeColors().trackLabelText;
        const QColor& bd = themeColors().accent;
        m_trackRename->setStyleSheet(QStringLiteral(
            "QLineEdit{background:%1;color:%2;border:1px solid %3;"
            "padding:0;font-size:9pt;font-weight:bold;}")
            .arg(bg.name(), fg.name(), bd.name()));
    }
    m_trackRename->setText(trk->name);
    // Editor posicionado sobre a faixa do NOME: linha inferior no layout
    // normal, única linha na faixa recolhida/baixa — mesmo retângulo do desenho.
    const int rowH = trackH(row, audio);
    const bool mini = trk->collapsed || rowH - kResizeHandleH < 22;
    m_trackRename->setGeometry(mini ? headerMiniNameRect(y) : headerNameRect(y));
    m_trackRename->show();
    m_trackRename->raise();
    m_trackRename->setFocus(Qt::OtherFocusReason);
    m_trackRename->selectAll();
}

void TimelineWidget::commitTrackRename() {
    if (!m_project || m_renameRow < 0) return;
    Track* trk = m_renameAudio
        ? (m_renameRow < m_project->audioTracks.size()
               ? &m_project->audioTracks[m_renameRow] : nullptr)
        : (m_renameRow < m_project->videoTracks.size()
               ? &m_project->videoTracks[m_renameRow] : nullptr);
    if (trk) {
        const QString t = m_trackRename->text().trimmed();
        if (!t.isEmpty() && t != m_renameOriginal) {
            emit editStart();
            trk->name = t;
            emit modified();
            invalidateScene();
        }
    }
    hideTrackRename();
}

void TimelineWidget::hideTrackRename() {
    if (m_trackRename) m_trackRename->hide();
    m_renameRow = -1;
    m_renameAudio = false;
    m_renameOriginal.clear();
    setFocus();
    update();
}

void TimelineWidget::renameTrackGroup(const QString& gid) {
    TrackGroup* g = m_project ? m_project->findGroup(gid) : nullptr;
    if (!g) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Renomear pasta"),
                                               tr("Nome da pasta:"), QLineEdit::Normal,
                                               g->name, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    emit editStart();
    g->name = name.trimmed();
    invalidateScene();
    emit modified();
}

void TimelineWidget::selectGroupTracks(const QString& gid) {
    m_selTracks.clear();
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (m_project->videoTracks[i].groupId == gid)
            m_selTracks.append(TrackSel{i, false});
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        if (m_project->audioTracks[i].groupId == gid)
            m_selTracks.append(TrackSel{i, true});
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        m_secondarySelected.clear();
        emit selectionChanged(QString());
    }
    m_hasAnchor = false;
}

// Ctrl+clique na pasta: alterna as faixas dela dentro da seleção atual, para
// acumular pastas sem perder o que já estava selecionado.
void TimelineWidget::toggleGroupTracks(const QString& gid) {
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        m_secondarySelected.clear();
        emit selectionChanged(QString());
    }
    QVector<TrackSel> group;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (m_project->videoTracks[i].groupId == gid)
            group.append(TrackSel{i, false});
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        if (m_project->audioTracks[i].groupId == gid)
            group.append(TrackSel{i, true});
    bool allIn = true;
    for (const TrackSel& ts : group)
        if (!m_selTracks.contains(ts)) { allIn = false; break; }
    if (allIn) {
        for (const TrackSel& ts : group)
            m_selTracks.removeAll(ts);
    } else {
        for (const TrackSel& ts : group)
            if (!m_selTracks.contains(ts)) m_selTracks.append(ts);
    }
    m_hasAnchor = false;
}

void TimelineWidget::pruneEmptyGroups() {
    if (!m_project) return;
    for (int i = (int)m_project->trackGroups.size() - 1; i >= 0; --i) {
        bool has = false;
        for (const Track& t : m_project->videoTracks)
            if (t.groupId == m_project->trackGroups[i].id) { has = true; break; }
        if (!has)
            for (const Track& t : m_project->audioTracks)
                if (t.groupId == m_project->trackGroups[i].id) { has = true; break; }
        if (!has) m_project->trackGroups.removeAt(i);
    }
}

// Move uma faixa de `from` para o índice `to` (ordem top-down de exibição).
void TimelineWidget::moveTrack(bool audio, int from, int to) {
    if (!m_project) return;
    auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
    if (from < 0 || from >= (int)list.size()) return;
    to = std::clamp(to, 0, (int)list.size() - 1);
    if (to == from) return;
    Track t = list.takeAt(from);
    list.insert(to, t);
}

// Move um conjunto de faixas (índices) para que a primeira caia em `to`.
void TimelineWidget::moveTracksTo(bool audio, const QVector<int>& idxs, int to) {
    if (!m_project) return;
    auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
    if (idxs.isEmpty()) return;
    QVector<int> sorted = idxs;
    std::sort(sorted.begin(), sorted.end());
    QVector<Track> picked;
    for (int i = sorted.size() - 1; i >= 0; --i) {
        if (sorted[i] < 0 || sorted[i] >= (int)list.size()) continue;
        picked.prepend(list.takeAt(sorted[i]));
    }
    if (picked.isEmpty()) return;
    to = std::clamp(to, 0, (int)list.size());
    for (int i = 0; i < picked.size(); ++i)
        list.insert(to + i, picked[i]);
}

// finishTrackDrag — implementado em TimelineDrag.cpp
// snapToEdges — implementado em TimelineDrag.cpp
// clampPosToTrack — implementado em TimelineDrag.cpp
// clipTrackIndex — implementado em TimelineDrag.cpp

// Move um clipe para a faixa (row/audio) preservando pos/in/dur. Mantém a
// lista ordenada por pos. Recusa faixa inválida, travada ou de tipo diferente.
bool TimelineWidget::moveClipToTrack(const QString& id, int row, bool audio) {
    if (!m_project) return false;
    if (audio) {
        if (row < 0 || row >= (int)m_project->audioTracks.size()) return false;
    } else {
        if (row < 0 || row >= (int)m_project->videoTracks.size()) return false;
    }
    Track& dst = audio ? m_project->audioTracks[row] : m_project->videoTracks[row];
    if (dst.locked) return false;
    if (!trackVisible(row, audio)) return false;

    Clip* c = findClipById(id);
    if (!c) return false;
    int curRow;
    bool curAudio;
    if (!clipTrackIndex(id, curRow, curAudio) || curAudio != audio)
        return false;
    if (curRow == row) return true;

    const Clip copy = *c;
    auto removeFrom = [&id](Track& t) {
        for (auto it = t.clips.begin(); it != t.clips.end(); ++it)
            if (it->id == id) { t.clips.erase(it); return true; }
        return false;
    };
    bool removed = removeFrom(dst);
    if (!removed) {
        for (Track& t : m_project->videoTracks)
            if (removeFrom(t)) { removed = true; break; }
        if (!removed)
            for (Track& t : m_project->audioTracks)
                if (removeFrom(t)) { removed = true; break; }
        if (!removed) return false;
    }

    auto it = dst.clips.begin();
    while (it != dst.clips.end() && it->pos <= copy.pos) ++it;
    dst.clips.insert(it, copy);
    // O erase+insert mexeu nos vetores: reconstrói o índice para os ponteiros
    // não ficarem pendurados durante o arrasto (MoveClip usa findClipById
    // logo depois e leria memória inválida, "separando" o par e travando).
    rebuildClipIndex();
    // Clipe que saiu de uma faixa de efeitos: o efeito é da faixa, então o
    // clipe "sai como entrou" (MainWindow remove os efeitos copiados da faixa).
    Track& src = m_project->videoTracks[curRow];
    if (src.fxTrack && !dst.fxTrack && !copy.isText)
        emit fxTrackClipRemoved(src.id, copy.id);
    // Clipe que entrou numa faixa de efeitos: aplica os efeitos habilitados da
    // faixa (MainWindow copies Track::fxEffects para o clipe).
    if (dst.fxTrack && !copy.isText)
        emit fxTrackClipAdded(dst.id, copy.id);
    return true;
}


void TimelineWidget::setTool(int tool) {
    if (tool == m_tool) return;
    m_tool = tool;
    invalidateScene();
    emit toolChanged(tool);
}

void TimelineWidget::setSnap(bool on) {
    m_snap = on;
    update();
}

// O ímã é o bloqueio mestre do auto-juntar: desligá-lo desativa o ripple
// (fechar o vão) mesmo que o botão Ripple esteja ligado.
bool TimelineWidget::rippleCloseEnabled() const {
    return m_snap && SettingsDialog::rippleDeleteEnabled();
}

void TimelineWidget::setGridVisible(bool on) {
    m_showGrid = on;
    update();
}

void TimelineWidget::setRulerVisible(bool on) {
    m_showRuler = on;
    update();
}

void TimelineWidget::setLoopInAtPlayhead() {
    if (!m_project) return;
    m_loopIn = std::max(0.0, snapTime(m_playhead));
    if (m_loopOut > 0 && m_loopOut <= m_loopIn)
        std::swap(m_loopIn, m_loopOut);
    emit loopChanged(m_loopIn, m_loopOut);
    update();
}

void TimelineWidget::setLoopOutAtPlayhead() {
    if (!m_project) return;
    m_loopOut = std::max(0.0, snapTime(m_playhead));
    if (m_loopIn < 0.0) m_loopIn = 0.0;
    if (m_loopOut <= m_loopIn)
        std::swap(m_loopIn, m_loopOut);
    emit loopChanged(m_loopIn, m_loopOut);
    update();
}

void TimelineWidget::clearLoop() {
    m_loopIn = m_loopOut = -1.0;
    emit loopChanged(-1.0, -1.0);
    update();
}

QVector<Clip*> TimelineWidget::groupMembers(const QString& gid) {
    QVector<Clip*> out;
    if (gid.isEmpty() || !m_project) return out;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.groupId == gid) out.append(&c);
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.groupId == gid) out.append(&c);
    return out;
}

QStringList TimelineWidget::expandToGroups(const QStringList& ids) {
    QStringList out = ids;
    for (const QString& id : ids) {
        Clip* c = findClipById(id);
        if (!c || c->groupId.isEmpty()) continue;
        for (Clip* m : groupMembers(c->groupId))
            if (!out.contains(m->id)) out.append(m->id);
    }
    return out;
}

void TimelineWidget::razorSplitAt(double t) {
    if (!m_project) return;
    emit undoLabel(tr("Dividir clipes"));
    emit editStart();
    QStringList toSplit;
    auto consider = [&](const Clip& c) {
        if (t > c.pos + 1e-6 && t < c.pos + c.dur - 1e-6)
            toSplit.append(c.id);
    };
    for (Track& tr : m_project->videoTracks)
        for (const Clip& c : tr.clips) consider(c);
    for (Track& tr : m_project->audioTracks)
        for (const Clip& c : tr.clips) consider(c);
    // Coleta os alvos primeiro; depois divide. Não pode iterar o vetor de
    // clipes enquanto o split insere nele (iterator inválido).
    QStringList handled; // dedupe por grupo (vídeo+áudio vinculados)
    for (const QString& id : toSplit) {
        Clip* cc = findClipById(id);
        if (!cc) continue;
        const QString key = cc->groupId.isEmpty() ? cc->id : cc->groupId;
        if (handled.contains(key)) continue;
        handled.append(key);
        splitClipAt(cc, t);
    }
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::envelopePress(Clip* c, double t) {
    if (!c) return;
    emit editStart();
    const bool audio = trackOf(c) ? trackOf(c)->audio : false;
    QVector<Keyframe>& keys = audio ? c->kfVolume : c->kfOpacity;
    double& base = audio ? c->volume : c->opacity;
    // t é absoluto na timeline; keyframes são relativos ao clipe.
    const double rel = t - c->pos;
    for (int i = 0; i < keys.size(); ++i) {
        if (std::fabs(keys[i].time - rel) < 1e-3) {
            keys.removeAt(i);
            update();
            emit modified();
            return;
        }
    }
    Keyframe k;
    k.time = rel;
    k.value = kfValue(keys, base, rel);
    keys.append(k);
    std::sort(keys.begin(), keys.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    update();
    emit modified();
}

void TimelineWidget::trackEnvelopePress(int row, bool audio, double t) {
    // Envelope de volume por faixa (ToolEnvelope em área vazia): alterna um
    // keyframe no tempo absoluto `t` da timeline (segue o padrão do clipe,
    // mas com tempos absolutos e base = volume estático da faixa).
    if (!m_project || row < 0) return;
    Track& tr = audio ? m_project->audioTracks[row] : m_project->videoTracks[row];
    const double tt = std::max(0.0, t);
    QVector<Keyframe>& keys = tr.kfVolume;
    const double base = tr.volume;
    for (int i = 0; i < keys.size(); ++i) {
        if (std::fabs(keys[i].time - tt) < 1e-3) {
            emit editStart();
            keys.removeAt(i);
            update();
            emit modified();
            return;
        }
    }
    emit editStart();
    Keyframe k;
    k.time = tt;
    k.value = kfValue(keys, base, tt);
    keys.append(k);
    std::sort(keys.begin(), keys.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    update();
    emit modified();
}

void TimelineWidget::applyZoomRect(double t0, double t1) {
    if (t1 < t0) std::swap(t0, t1);
    const double span = std::max(0.1, t1 - t0);
    const int usable = width() - m_vbar->sizeHint().width() - kHeaderW;
    const double pps = usable > 0 ? (double)usable / span : m_pps;
    const double endPps = std::clamp(pps, kMinPps, kMaxPps);
    if (std::fabs(endPps - m_pps) < 1e-9) return;
    m_zoomRectMode = true;
    m_zoomStartPps = m_pps;
    m_zoomEndPps = endPps;
    m_zoomStartView = m_viewStart;
    m_zoomEndView = std::max(0.0, t0);
    if (m_zoomAnim->state() != QVariantAnimation::Stopped)
        m_zoomAnim->stop();
    m_zoomAnim->setStartValue(0.0);
    m_zoomAnim->setEndValue(1.0);
    m_zoomAnim->start();
}

// ---- Novas ferramentas de edição (Kdenlive + Premiere Pro) ----

QPair<Clip*, Clip*> TimelineWidget::adjacentClips(Clip* c) {
    if (!c || !m_project) return {nullptr, nullptr};
    Track* tr = trackOf(c);
    if (!tr) return {nullptr, nullptr};
    
    Clip* prev = nullptr;
    Clip* next = nullptr;
    
    for (Clip& other : tr->clips) {
        if (other.id == c->id) continue;
        // Clipe anterior: termina onde o atual começa (ou perto)
        if (other.pos + other.dur <= c->pos + 1e-6) {
            if (!prev || other.pos > prev->pos) prev = &other;
        }
        // Clipe seguinte: começa onde o atual termina (ou perto)
        if (other.pos >= c->pos + c->dur - 1e-6) {
            if (!next || other.pos < next->pos) next = &other;
        }
    }
    
    return {prev, next};
}

void TimelineWidget::rippleTrimLeft(Clip* c, double newIn) {
    if (!c || !m_project) return;
    
    const double delta = newIn - c->in;
    if (std::fabs(delta) < 1e-6) return;
    
    // Ajustar in e dur do clipe
    c->in = std::max(0.0, newIn);
    c->dur -= delta;
    c->pos += delta;
    
    // Deslocar todos os clipes posteriores na mesma faixa
    Track* tr = trackOf(c);
    if (!tr) return;
    
    for (Clip& other : tr->clips) {
        if (other.id == c->id) continue;
        if (other.pos >= c->pos + c->dur - 1e-6) {
            other.pos -= delta;
        }
    }
}

void TimelineWidget::rippleTrimRight(Clip* c, double newDur) {
    if (!c || !m_project) return;
    
    const double delta = newDur - c->dur;
    if (std::fabs(delta) < 1e-6) return;
    
    // Ajustar dur do clipe
    c->dur = std::max(kMinDur, newDur);
    
    // Deslocar todos os clipes posteriores na mesma faixa
    Track* tr = trackOf(c);
    if (!tr) return;
    
    const double cEnd = c->pos + c->dur;
    for (Clip& other : tr->clips) {
        if (other.id == c->id) continue;
        // Clipe começa após o final atual do clipe editado
        if (other.pos >= cEnd - delta - 1e-6) {
            other.pos += delta;
        }
    }
}

void TimelineWidget::rollingEdit(double delta) {
    Clip* clipA = findClipById(m_rollClipA);
    Clip* clipB = findClipById(m_rollClipB);
    if (!clipA || !clipB) return;
    
    // Ajustar durações opostas
    const double newDurA = std::max(kMinDur, m_rollOrigDurA + delta);
    const double newDurB = std::max(kMinDur, m_rollOrigDurB - delta);
    
    clipA->dur = newDurA;
    clipB->in = clipB->in + (m_rollOrigDurB - newDurB);
    clipB->dur = newDurB;
    clipB->pos = clipA->pos + clipA->dur;
}

void TimelineWidget::slipEdit(Clip* c, double delta) {
    if (!c) return;
    
    // Mudar in/out sem mudar posição
    const double newIn = std::max(0.0, m_slipOrigIn + delta);
    // Limitar ao tamanho da mídia
    const MediaItem* m = m_project->findMedia(c->mediaId);
    const double maxDur = m ? m->duration : (c->in + c->dur);
    const double maxIn = std::max(0.0, maxDur - c->dur);
    c->in = std::min(newIn, maxIn);
    // dur e pos permanecem iguais
}

void TimelineWidget::slideEdit(Clip* c, double delta) {
    if (!c || !m_project) return;
    
    Track* tr = trackOf(c);
    if (!tr) return;
    
    // Encontrar clipes adjacentes
    Clip* prev = nullptr;
    Clip* next = nullptr;
    for (Clip& other : tr->clips) {
        if (other.id == c->id) continue;
        if (other.pos + other.dur <= c->pos + 1e-6) {
            if (!prev || other.pos > prev->pos) prev = &other;
        }
        if (other.pos >= c->pos + c->dur - 1e-6) {
            if (!next || other.pos < next->pos) next = &other;
        }
    }
    
    // Mover clipe atual
    c->pos = std::max(0.0, m_slideOrigPosB + delta);
    
    // Ajustar clipe anterior (estender/encurtar)
    if (prev && m_slideOrigDurA > 0) {
        prev->dur = c->pos - prev->pos;
    }
    
    // Ajustar clipe seguinte (mover in e dur)
    if (next && m_slideOrigDurC > 0) {
        const double nextNewPos = c->pos + c->dur;
        const double shift = nextNewPos - next->pos;
        next->pos = nextNewPos;
        next->in = std::max(0.0, next->in - shift / next->speed);
    }
}

void TimelineWidget::rateStretch(Clip* c, double newSpeed) {
    if (!c) return;
    
    // Mudar velocidade e ajustar duração
    c->speed = std::max(0.1, newSpeed);
    c->dur = m_rateOrigDur * (m_rateOrigSpeed / c->speed);
}
