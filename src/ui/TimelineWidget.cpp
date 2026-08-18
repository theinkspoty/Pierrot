// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TimelineWidget.h"

#include "ffmpeg/MediaCache.h"
#include "ffmpeg/FFmpegDecoder.h"
#include "ui/TransformDialog.h"
#include "ui/AudioEffectsDialog.h"
#include "ui/TextEditorDialog.h"
#include "ui/SettingsDialog.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QCheckBox>
#include <QCursor>
#include <QList>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QScrollBar>
#include <QMenu>
#include <QKeyEvent>
#include <QDebug>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPolygon>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QInputDialog>
#include <QPushButton>
#include <QCheckBox>
#include <QColorDialog>
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
constexpr int kHeaderW = 130;
constexpr int kRulerH = 26;
constexpr int kMarginR = 60;
constexpr int kZoomW = 64;
// Tamanhos dos presets de estilo das faixas (minimizada / grande).
constexpr int kPresetMinV = 34, kPresetMinA = 28;
constexpr int kPresetMaxV = 100, kPresetMaxA = 78;
constexpr int kVideoRowH = 56;
constexpr int kAudioRowH = 44;
constexpr int kMinRowH = 24;
constexpr int kMaxRowH = 400;
constexpr int kResizeHandleH = 5;
constexpr int kFolderH = 22; // altura da faixa de cabeçalho de uma pasta
constexpr double kMinPps = 2.0;
constexpr double kMaxPps = 4000.0;
constexpr double kMinDur = 0.04;
// Duração padrão ao inserir uma imagem na timeline (imagens não têm duração
// própria; sem isto entrariam como um clipe de ~1s, estreito demais).
constexpr double kDefaultImageDur = 3.0;

// Modos de ferramenta (índices usados pela barra de ferramentas).
enum Tool {
    ToolSelect = 0, ToolMove = 1, ToolScissors = 2, ToolEnvelope = 3, ToolZoom = 4
};

bool isImageFile(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    static const QStringList exts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("svg")
    };
    return exts.contains(ext);
}

// Duração efetiva ao inserir uma mídia na timeline: imagens usam o padrão
// próprio; vídeo/áudio usam a duração real (ou 1s como fallback).
double mediaInsertDur(const MediaItem& m) {
    if (isImageFile(m.filePath)) return kDefaultImageDur;
    return m.duration > 0 ? m.duration : 1.0;
}

QString fmtRuler(double t) {
    const int total = (int)std::floor(t);
    return QString("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

// fps do stream de vídeo do arquivo, com cache (o probe abre o arquivo).
double sourceFps(const QString& path) {
    static QHash<QString, double> cache;
    const auto it = cache.constFind(path);
    if (it != cache.constEnd()) return it.value();
    const FFmpegMediaInfo info = FFmpegDecoder::probe(path);
    cache.insert(path, info.fps);
    return info.fps;
}

// Remove os clipes selecionados de uma faixa e desloca os que estão à
// direita da lacuna removida para a esquerda (edição ripple).
void rippleDeleteInTrack(Track& t, const QStringList& sel) {
    QVector<Clip> keep;
    QVector<QPair<double, double>> removed;
    for (Clip& c : t.clips) {
        if (sel.contains(c.id))
            removed.append(qMakePair(c.pos, c.pos + c.dur));
        else
            keep.append(c);
    }
    if (removed.isEmpty()) {
        t.clips = keep;
        return;
    }
    std::sort(removed.begin(), removed.end());
    QVector<QPair<double, double>> merged;
    for (const auto& iv : removed) {
        if (merged.isEmpty() || iv.first > merged.last().second + 1e-6)
            merged.append(iv);
        else
            merged.last().second = std::max(merged.last().second, iv.second);
    }
    for (Clip& c : keep) {
        double shift = 0.0;
        for (const auto& iv : merged)
            if (c.pos >= iv.second - 1e-6)
                shift += iv.second - iv.first;
        c.pos = std::max(0.0, c.pos - shift);
    }
    t.clips = keep;
}
}

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
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
        else
            invalidateScene();
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
}

void TimelineWidget::invalidateScene() {
    // Mudança estrutural: descarta o cache de conteúdo dos clipes para que
    // onda/thumb/envelope sejam re-renderizados na próxima passada.
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
    m_selected.clear();
    clearTrackSelection();
    m_clipPix.clear();
    m_clipBytes = 0;
    emit selectionChanged(QString());
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

// Adiciona uma mídia do pool no playhead (fallback para o arrastar/soltar):
// vídeo na primeira faixa de vídeo desbloqueada, com áudio vinculado quando a
// mídia tiver ambas as trilhas.
void TimelineWidget::addMediaAtPlayhead(const QString& mediaId) {
    if (!m_project) return;
    const MediaItem* m = m_project->findMedia(mediaId);
    if (!m) return;
    qDebug() << "[TimelineWidget::addMediaAtPlayhead]" << m->name
             << "hasVideo=" << m->hasVideo << "hasAudio=" << m->hasAudio
             << "dur=" << m->duration;
    emit editStart();
    const double t = snapTime(std::max(0.0, m_playhead));
    const double dur = mediaInsertDur(*m);

    // Usa uma faixa livre (sem sobreposição em `t`) ou cria uma nova vazia.
    auto findFreeTrack = [this](bool audio, double t, double dur, int prefer) {
        auto overlap = [t, dur](const QVector<Clip>& clips) {
            for (const Clip& o : clips)
                if (o.pos < t + dur - 1e-9 && o.pos + o.dur > t + 1e-9) return true;
            return false;
        };
        auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
        if (prefer >= 0 && prefer < (int)list.size()
            && !list[prefer].locked && !overlap(list[prefer].clips))
            return prefer;
        for (int i = 0; i < (int)list.size(); ++i)
            if (!list[i].locked && !overlap(list[i].clips)) return i;
        m_project->addTrack(audio);
        return (int)list.size() - 1;
    };

    QString lastPlaced;
    const auto push = [&](QVector<Clip>& clips, const Clip& c) {
        auto it = clips.begin();
        while (it != clips.end() && it->pos <= c.pos) ++it;
        clips.insert(it, c);
    };
    // Arquivos multicanal (OBS/câmera): um clipe de áudio POR stream, cada um
    // na sua faixa. O número de streams pode ser 0 mesmo com hasAudio.
    const int aStreams = m->hasAudio ? qMax(1, m->audioStreams) : 0;
    const bool multi = aStreams > 1;
    if (m->hasVideo) {
        const int vRow = findFreeTrack(false, t, dur, 0);
        Clip c;
        c.id = newId();
        c.groupId = aStreams > 0 ? newId() : QString();
        c.mediaId = mediaId;
        c.pos = t;
        c.in = 0.0;
        c.dur = dur;
        c.name = m->name;
        push(m_project->videoTracks[vRow].clips, c);
        lastPlaced = c.id;
        for (int k = 0; k < aStreams; ++k) {
            const int aRow = findFreeTrack(true, t, dur, 0);
            Clip ac = c;
            ac.id = newId();
            ac.audioStreamIndex = k;
            if (multi) ac.name = QString("%1 (faixa %2)").arg(m->name).arg(k + 1);
            push(m_project->audioTracks[aRow].clips, ac);
        }
    } else if (m->hasAudio) {
        const QString gid = multi ? newId() : QString(); // streams juntos no Delete/mover
        for (int k = 0; k < aStreams; ++k) {
            const int aRow = findFreeTrack(true, t, dur, 0);
            Clip c;
            c.id = newId();
            c.mediaId = mediaId;
            c.groupId = gid;
            c.audioStreamIndex = k;
            c.pos = t;
            c.in = 0.0;
            c.dur = dur;
            c.name = multi ? QString("%1 (faixa %2)").arg(m->name).arg(k + 1)
                           : m->name;
            push(m_project->audioTracks[aRow].clips, c);
            lastPlaced = c.id;
        }
    }
    if (!lastPlaced.isEmpty()) setSelection(lastPlaced);
    updateScrollRanges();
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
    int foldersH = folderStripsAboveVideo(-1) * kFolderH;
    foldersH += folderStripsAboveAudio((int)m_project->audioTracks.size() - 1) * kFolderH;
    const int totalH = kRulerH + rowsH + foldersH + 20;
    const int totalW = kHeaderW + (int)(total * m_pps) + kMarginR;

    m_hbar->setRange(0, std::max(0, totalW - viewW));
    m_hbar->setPageStep(std::max(1, viewW));
    m_vbar->setRange(0, std::max(0, totalH - viewH));
    m_vbar->setPageStep(std::max(1, viewH));

    m_viewStart = m_hbar->value() / m_pps;
    m_viewTop = m_vbar->value();
}

double TimelineWidget::timeToX(double t) const {
    return kHeaderW + (t - m_viewStart) * m_pps;
}

double TimelineWidget::xToTime(int x) const {
    return m_viewStart + (x - kHeaderW) / m_pps;
}

// Altura efetiva de uma faixa (personalizada ou padrão).
int TimelineWidget::trackH(int idx, bool audio) const {
    if (!m_project) return audio ? kAudioRowH : kVideoRowH;
    const QVector<Track>& list = audio ? m_project->audioTracks : m_project->videoTracks;
    if (idx < 0 || idx >= (int)list.size()) return audio ? kAudioRowH : kVideoRowH;
    const int h = list[idx].height;
    if (h >= kMinRowH) return std::min(h, kMaxRowH);
    return audio ? kAudioRowH : kVideoRowH;
}

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

int TimelineWidget::rowY(int videoIdx, int audioIdx) const {
    const int n = (int)m_project->videoTracks.size();
    int y = kRulerH - m_viewTop;
    if (videoIdx >= 0) {
        // Faixas de vídeo na ordem dos índices (V1 no topo, V2 abaixo, ...).
        for (int i = 0; i < videoIdx; ++i)
            if (trackVisible(i, false)) y += trackH(i, false);
        y += folderStripsAboveVideo(videoIdx) * kFolderH;
        return y;
    }
    for (int i = 0; i < n; ++i)
        if (trackVisible(i, false)) y += trackH(i, false);
    y += folderStripsAboveVideo(-1) * kFolderH;
    y += folderStripsAboveAudio(audioIdx) * kFolderH;
    for (int i = 0; i < audioIdx; ++i)
        if (trackVisible(i, true)) y += trackH(i, true);
    return y;
}

bool TimelineWidget::rowFromY(int y, int& row, bool& audio) const {
    if (!m_project) return false;
    int rem = y + m_viewTop - kRulerH;
    if (rem < 0) return false;
    const int n = (int)m_project->videoTracks.size();
    for (int i = 0; i < n; ++i) {
        if (!trackVisible(i, false)) continue;
        const int above = folderStripsAboveVideo(i);
        const int below = (i > 0) ? folderStripsAboveVideo(i - 1) : 0;
        rem -= (above - below) * kFolderH;
        const int h = trackH(i, false);
        if (rem < h) { row = i; audio = false; return true; }
        rem -= h;
    }
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
        if (!trackVisible(i, true)) continue;
        const int above = folderStripsAboveAudio(i);
        const int below = (i > 0) ? folderStripsAboveAudio(i - 1) : 0;
        rem -= (above - below) * kFolderH;
        const int h = trackH(i, true);
        if (rem < h) { row = i; audio = true; return true; }
        rem -= h;
    }
    return false;
}

Clip* TimelineWidget::clipAt(int row, bool audio, double t) const {
    if (!m_project) return nullptr;
    auto top = [t](QVector<Clip>& clips) -> Clip* {
        // Com sobreposições, o clipe visível no topo é o que começa mais tarde.
        Clip* best = nullptr;
        for (auto& c : clips)
            if (t >= c.pos && t < c.pos + c.dur)
                if (!best || c.pos > best->pos) best = &c;
        return best;
    };
    if (audio) {
        if (row < 0 || row >= (int)m_project->audioTracks.size()) return nullptr;
        return top(m_project->audioTracks[row].clips);
    } else {
        if (row < 0 || row >= (int)m_project->videoTracks.size()) return nullptr;
        return top(m_project->videoTracks[row].clips);
    }
}

Clip* TimelineWidget::findClipById(const QString& id) {
    if (!m_project) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    return nullptr;
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
    m_playhead = std::max(0.0, t);
    ensurePlayheadVisible();
    update();
}

void TimelineWidget::ensurePlayheadVisible() {
    if (!m_project) return;
    const double px = timeToX(m_playhead);
    const int viewW = width() - m_vbar->sizeHint().width();
    if (px < kHeaderW) {
        m_hbar->setValue((int)((m_playhead - (viewW - kHeaderW) * 0.2) * m_pps));
    } else if (px > viewW) {
        m_hbar->setValue((int)((m_playhead - (viewW - kHeaderW) * 0.8) * m_pps));
    }
    m_viewStart = m_hbar->value() / m_pps;
}

// Liga o autoscroll quando o mouse (durante PlayheadDrag, RulerLoop ou o
// arraste de um clipe) está perto da borda esquerda/direita da view. A rolagem
// contínua faz a agulha/clipe acompanhar o cursor sem que ele saia da janela.
void TimelineWidget::startAutoScroll(QMouseEvent* e) {
    if (!m_project) return;
    m_autoScrollMouse = e->pos();
    const int edge = 32;
    const int vw = width() - m_vbar->sizeHint().width();
    const int mx = m_autoScrollMouse.x();
    int dir = 0;
    if (mx >= kHeaderW && mx <= kHeaderW + edge)
        dir = -1;
    else if (mx >= vw - edge)
        dir = +1;
    if (dir != 0) {
        m_autoScrollDir = dir;
        if (!m_autoScroll->isActive()) m_autoScroll->start();
    } else {
        stopAutoScroll();
    }
}

void TimelineWidget::stopAutoScroll() {
    m_autoScroll->stop();
    m_autoScrollDir = 0;
}

// Rola a timeline e, quando aplicável, reposiciona a agulha/loop/arrasto sob o
// cursor para que a operação continue na nova área visível.
void TimelineWidget::autoScrollTick() {
    if (m_dragMode != PlayheadDrag && m_dragMode != RulerLoop
        && m_dragMode != MoveClip && m_dragMode != TrimLeft
        && m_dragMode != TrimRight) {
        stopAutoScroll();
        return;
    }
    const int viewW = width() - m_vbar->sizeHint().width();
    const int step = qMax(8, viewW / 24) * m_autoScrollDir;
    int v = m_hbar->value() + step;
    if (v < m_hbar->minimum()) v = m_hbar->minimum();
    if (v > m_hbar->maximum()) v = m_hbar->maximum();
    m_hbar->setValue(v);

    // Recalcula a operação na nova posição da view.
    QMouseEvent ev(QEvent::MouseMove, m_autoScrollMouse,
                   mapToGlobal(m_autoScrollMouse), Qt::LeftButton,
                   Qt::LeftButton, Qt::NoModifier);
    if (m_dragMode == PlayheadDrag || m_dragMode == RulerLoop) {
        const double t2 = std::max(0.0, snapTime(xToTime(m_autoScrollMouse.x())));
        setPlayhead(t2);
        emit playheadChanged(t2);
        update();
        return;
    }
    mouseMoveEvent(&ev);
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 24, 26));
    if (!m_project) return;

    // A cena estática (grid, faixas, clipes, ondas, thumbs) é desenhada uma
    // vez e reutilizada: mover o playhead durante a reprodução/scrub custa só
    // as camadas finas (playhead, loop, indicações de arrasto), não a cena.
    // O buffer é reaproveitado entre repaints para evitar alocar um QPixmap
    // inteiro a cada frame de rolagem.
    if (m_staticDirty || m_staticCache.size() != size()) {
        if (m_staticCache.size() != size())
            m_staticCache = QPixmap(size());
        m_staticCache.fill(Qt::transparent);
        QPainter sp(&m_staticCache);
        renderScene(sp);
        m_staticDirty = false;
    }
    p.drawPixmap(0, 0, m_staticCache);
    renderOverlays(p);
}

// Parte fixa da timeline. Desenhada em QPixmap e reutilizada entre repaints.
void TimelineWidget::renderScene(QPainter& p) {
    const int H = kHeaderW;
    const int R = kRulerH;

    p.fillRect(0, 0, width(), R, QColor(40, 40, 44));
    p.setPen(QColor(70, 70, 76));
    p.drawLine(0, R - 1, width(), R - 1);
    p.drawLine(H - 1, R, H - 1, height());

    // O grid de tempo só vai até o fim das faixas, não pelo espaço vazio abaixo.
    int rowsBottom = kRulerH - m_viewTop;
    rowsBottom += folderStripsAboveVideo(-1) * kFolderH;
    rowsBottom += folderStripsAboveAudio((int)m_project->audioTracks.size() - 1) * kFolderH;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (trackVisible(i, false)) rowsBottom += trackH(i, false);
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        if (trackVisible(i, true)) rowsBottom += trackH(i, true);
    const int gridBottom = std::min(height(), rowsBottom);
    double step = 1.0;
    while (step * m_pps < 70.0) step *= 2.0;
    const double last = m_viewStart + (width() - H) / m_pps;
    for (double t = std::ceil(m_viewStart / step) * step; t <= last; t += step) {
        const int x = (int)(H + (t - m_viewStart) * m_pps);
        p.setPen(QColor(52, 52, 58));
        p.drawLine(x, R, x, gridBottom);
        p.setPen(QColor(180, 180, 190));
        QFont f = p.font();
        f.setPointSizeF(8);
        p.setFont(f);
        p.drawText(x + 3, R - 7, fmtRuler(t));
    }

    for (const Marker& mk : m_project->markers) {
        const int mx = (int)(H + (mk.time - m_viewStart) * m_pps);
        if (mx < H || mx > width()) continue;
        p.setPen(QPen(mk.color, 1));
        p.drawLine(mx, 2, mx, R - 4);
        QPolygon flag;
        flag << QPoint(mx, 2) << QPoint(mx + 6, 2) << QPoint(mx + 6, 8) << QPoint(mx, 8);
        p.setPen(Qt::NoPen);
        p.setBrush(mk.color);
        p.drawPolygon(flag);
        if (!mk.name.isEmpty()) {
            p.setPen(mk.color);
            p.drawText(mx + 8, R - 8, mk.name);
        }
    }

    const int zx0 = 6;
    p.fillRect(zx0, 2, kZoomW, R - 4, QColor(60, 60, 66));
    p.setPen(QColor(200, 200, 205));
    QFont zf = p.font();
    zf.setPointSizeF(9);
    zf.setBold(true);
    p.setFont(zf);
    p.drawText(QRect(zx0, 2, kZoomW / 2, R - 4), Qt::AlignCenter, QStringLiteral("−"));
    p.drawText(QRect(zx0 + kZoomW / 2, 2, kZoomW / 2, R - 4), Qt::AlignCenter, QStringLiteral("+"));
    p.drawLine(zx0 + kZoomW / 2, 4, zx0 + kZoomW / 2, R - 6);

    // Fundos e cabeçalhos das faixas. Clipados abaixo da régua: com o scroll
    // vertical (m_viewTop > 0) uma faixa parcialmente oculta não pode pintar
    // por cima da régua nem da coluna de cabeçalho.
    p.save();
    p.setClipRect(QRect(0, R, width(), height() - R));
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i) {
        if (!trackVisible(i, false)) continue;
        const int y = rowY(i, -1);
        const int rowH = trackH(i, false);
        const bool sel = isTrackSelected(i, false);
        p.fillRect(0, y, width(), rowH, sel ? QColor(44, 50, 64)
                                            : ((i % 2) ? QColor(31, 31, 34) : QColor(28, 28, 31)));
        if (sel) {
            p.fillRect(0, y, 4, rowH, QColor(120, 170, 255));
            p.setPen(QPen(QColor(110, 160, 240), 1));
            p.drawRect(0, y, width() - 1, rowH - 1);
        }
        p.setPen(QColor(48, 48, 54));
        p.drawLine(0, y + rowH, width(), y + rowH);
        drawTrackHeader(p, y, rowH, m_project->videoTracks[i], sel);
    }
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
        if (!trackVisible(i, true)) continue;
        const int y = rowY(-1, i);
        const int rowH = trackH(i, true);
        const bool sel = isTrackSelected(i, true);
        p.fillRect(0, y, width(), rowH, sel ? QColor(42, 48, 62)
                                            : ((i % 2) ? QColor(29, 29, 32) : QColor(26, 26, 29)));
        if (sel) {
            p.fillRect(0, y, 4, rowH, QColor(120, 170, 255));
            p.setPen(QPen(QColor(106, 150, 224), 1));
            p.drawRect(0, y, width() - 1, rowH - 1);
        }
        p.setPen(QColor(46, 46, 52));
        p.drawLine(0, y + rowH, width(), y + rowH);
        drawTrackHeader(p, y, rowH, m_project->audioTracks[i], sel);
    }

    // Faixas de cabeçalho das pastas (grupos de faixas).
    for (const TrackGroup& g : m_project->trackGroups)
        drawFolderStrip(p, g);

    p.restore();

    // Clipes: nunca invadem a régua nem a coluna de cabeçalho, mesmo quando um
    // clipe está parcialmente à esquerda da janela (zoom out / scroll).
    // Desenha na ordem de `pos`: com sobreposições, o clipe que começa depois
    // (camada de cima) sai por último.
    p.save();
    p.setClipRect(QRect(H, R, width() - H, height() - R));
    auto drawClips = [&](const QVector<Track>& tracks, bool audio) {
        QVector<QPair<int, const Clip*>> order;
        for (int i = 0; i < (int)tracks.size(); ++i) {
            if (!trackVisible(i, audio)) continue;
            for (const Clip& c : tracks[i].clips)
                order.append(QPair<int, const Clip*>(i, &c));
        }
        std::stable_sort(order.begin(), order.end(),
                         [](const QPair<int, const Clip*>& a,
                            const QPair<int, const Clip*>& b) {
                             return a.second->pos < b.second->pos;
                         });
        for (const auto& it : order) {
            const int i = it.first;
            const Clip& c = *it.second;
            const Track& tr = tracks[i];
            const int y = audio ? rowY(-1, i) : rowY(i, -1);
            const int rowH = audio ? trackH(i, true) : trackH(i, false);
            const int cx = (int)(H + (c.pos - m_viewStart) * m_pps);
            const int cw = std::max(2, (int)(c.dur * m_pps));
            QRect r(cx + 1, y + 4, cw - 2, rowH - 8);
            if (r.width() <= 0 || r.height() <= 0) continue;
            if (r.right() < H || r.left() > width()) continue;
            drawClip(p, r, c, tr, audio);
        }
    };
    drawClips(m_project->videoTracks, false);
    drawClips(m_project->audioTracks, true);

    // Indicadores de transição: faixa diagonal com o símbolo do tipo sobre a
    // região de sobreposição entre o clipe anterior e o seguinte.
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i) {
        if (!trackVisible(i, false)) continue;
        const QVector<Clip>& clips = m_project->videoTracks[i].clips;
        const int y = rowY(i, -1);
        const int rowH = trackH(i, false);
        for (int k = 1; k < (int)clips.size(); ++k) {
            const Clip& prev = clips[k - 1];
            const Clip& cur = clips[k];
            const double end = prev.pos + prev.dur;
            if (cur.pos >= end - 1e-6) continue; // sem sobreposição
            const int x0 = (int)(H + (cur.pos - m_viewStart) * m_pps);
            const int x1 = (int)(H + (end - m_viewStart) * m_pps);
            if (x1 < H || x0 > width()) continue;
            const QRect r(x0 + 1, y + 4, std::max(2, x1 - x0 - 2), rowH - 8);
            const QString type = isTransition(prev.transitionType)
                                     ? prev.transitionType
                                     : QStringLiteral("dissolve");
            drawTransitionIndicator(p, r, type);
        }
    }
    p.restore();

    // Régua de volume das faixas de áudio (estilo Vegas): linha horizontal
    // arrastável; arrastar para cima aumenta o volume da faixa (0–200%).
    if (m_showVolLines) {
        p.save();
        p.setClipRect(QRect(H, kRulerH, width() - H, height() - kRulerH));
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
            if (!trackVisible(i, true)) continue;
            const Track& tr = m_project->audioTracks[i];
            const int ly = volLineY(i, true, tr);
            const bool active = m_dragMode == TrackVol && m_volRow == i;
            p.setPen(QPen(active ? QColor(255, 220, 90) : QColor(255, 255, 255),
                          active ? 2 : 1, Qt::SolidLine));
            p.drawLine(H, ly, width(), ly);
        }
        // Linha de volume individual de cada clipe de áudio (arrastar ajusta
        // só aquele clipe, sem mexer no resto da faixa).
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i) {
            if (!trackVisible(i, true)) continue;
            const Track& tr = m_project->audioTracks[i];
            const int y = rowY(-1, i);
            for (const Clip& c : tr.clips) {
                const int cx = (int)(H + (c.pos - m_viewStart) * m_pps);
                const int cw = std::max(2, (int)(c.dur * m_pps));
                if (cx + cw < H || cx > width()) continue;
                const int ly = clipVolLineY(i, c);
                const bool active = m_dragMode == ClipVol && m_volClip == c.id;
                p.setPen(QPen(active ? QColor(255, 220, 90) : QColor(255, 255, 255, 170),
                              active ? 2 : 1, Qt::SolidLine));
                p.drawLine(cx + 1, ly, cx + cw - 1, ly);
            }
        }
        p.restore();
    }
}

// Camadas finas desenhadas por cima da cena estática em cada repaint.
void TimelineWidget::renderOverlays(QPainter& p) {
    const int H = kHeaderW;
    const int R = kRulerH;

    // Região de loop (in/out).
    if (m_loopOut > m_loopIn) {
        const int lx0 = (int)timeToX(m_loopIn);
        const int lx1 = (int)timeToX(m_loopOut);
        p.fillRect(QRect(lx0, 0, lx1 - lx0, R), QColor(255, 200, 40, 70));
        p.fillRect(QRect(lx0, R, lx1 - lx0, height() - R), QColor(255, 200, 40, 18));
        p.setPen(QColor(255, 200, 40, 170));
        p.drawLine(lx0, R, lx0, height());
        p.drawLine(lx1, R, lx1, height());
    }

    const double px = H + (m_playhead - m_viewStart) * m_pps;
    // Só desenha a agulha sobre a área de conteúdo: quando o playhead está
    // antes da área visível (px < cabeçalho) a linha não pode cruzar a coluna
    // dos cabeçalhos das faixas.
    if (px >= H && px <= width()) {
        p.setPen(QColor(255, 70, 70));
        p.drawLine((int)px, R, (int)px, height());
        QPolygon tri;
        tri << QPoint((int)px - 6, 0) << QPoint((int)px + 6, 0) << QPoint((int)px, 9);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 70, 70));
        p.drawPolygon(tri);
    }

    if (m_dragMode == Razor) {
        const int rx = (int)timeToX(m_razorT);
        p.setPen(QPen(QColor(255, 255, 255, 190), 1, Qt::DashLine));
        p.drawLine(rx, R, rx, height());
    } else if (m_dragMode == ZoomSelect) {
        const int zx1 = (int)timeToX(m_zoomT0);
        const int zx2 = (int)timeToX(m_zoomT1);
        const QRect zr(QPoint(std::min(zx1, zx2), R), QPoint(std::max(zx1, zx2), height()));
        QStyleOptionRubberBand opt;
        opt.initFrom(this);
        opt.rect = zr;
        opt.shape = QRubberBand::Rectangle;
        opt.opaque = false;
        style()->drawControl(QStyle::CE_RubberBand, &opt, &p, this);
    } else if (m_dragMode == Marquee) {
        const QRect mr = m_marqueeRect.normalized();
        if (!mr.isEmpty()) {
            // Mesma aparência do seletor de mídias (QRubberBand da pool): o
            // próprio estilo desenha o retângulo azul semi-transparente.
            QStyleOptionRubberBand opt;
            opt.initFrom(this);
            opt.rect = mr;
            opt.shape = QRubberBand::Rectangle;
            opt.opaque = false;
            style()->drawControl(QStyle::CE_RubberBand, &opt, &p, this);
        }
    }

    // Feedback de arrasto vindo da mídia: destaca a faixa-alvo.
    if (m_dragHoverRow >= 0) {
        const int hy = m_dragHoverAudio ? rowY(-1, m_dragHoverRow)
                                        : rowY(m_dragHoverRow, -1);
        const int hh = trackH(m_dragHoverRow, m_dragHoverAudio);
        p.setPen(QPen(QColor(80, 160, 255, 220), 2));
        p.drawRect(H + 1, hy + 1, width() - H - 2, hh - 2);
        p.fillRect(H, hy, width() - H, hh, QColor(80, 160, 255, 26));
    }

    // Arrasto de faixa/grupo: indica onde vai entrar (linha ou pasta).
    if (m_dragMode == TrackDrag) {
        const bool isGroup = !m_dragGroupId.isEmpty();
        if (isGroup) {
            // Arrasto da pasta inteira: linha da posição alvo.
            if (m_dropRow >= 0) {
                const int dy = m_dropAudio ? rowY(-1, m_dropRow) : rowY(m_dropRow, -1);
                p.setPen(QPen(QColor(80, 200, 120), 2));
                p.drawLine(0, dy, width(), dy);
            }
        } else if (!m_dropGroup.isEmpty()) {
            // Faixa arrastada para dentro de uma pasta: destaca a pasta.
            const TrackGroup* g = m_project->findGroup(m_dropGroup);
            if (g) {
                const QRect fr = folderStripRect(*g);
                p.fillRect(fr, QColor(80, 200, 120, 60));
                p.setPen(QPen(QColor(80, 200, 120), 2));
                p.drawRect(fr.adjusted(0, 0, -1, -1));
            }
        } else if (m_dropRow >= 0) {
            const int dy = m_dropAudio ? rowY(-1, m_dropRow) : rowY(m_dropRow, -1);
            p.setPen(QPen(QColor(80, 200, 120), 2));
            p.drawLine(0, dy, width(), dy);
        }
    }
}

void TimelineWidget::drawClip(QPainter& p, const QRect& r, const Clip& c,
                              const Track& tr, bool audio) {
    if (r.width() <= 0 || r.height() <= 0) return;
    const bool sel = isSelected(c.id);
    QColor fill = audio ? QColor(26, 86, 66) : QColor(32, 66, 116);
    QColor border = audio ? QColor(70, 160, 120) : QColor(90, 140, 210);
    if (sel) {
        fill = audio ? QColor(40, 120, 92) : QColor(46, 96, 168);
        border = QColor(255, 170, 40);
    }
    p.setPen(QPen(border, sel ? 2 : 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, 3, 3);

    // Conteúdo do clipe (onda/thumb + envelope + fades) num pixmap cacheado.
    // A parte cara (loop pixel a pixel da onda, slices de thumb) é desenhada
    // UMA vez por (clipe, tamanho, epoch); rolagem, zoom e playhead reusam o
    // blit. Só edições estruturais bumpam o epoch (ver invalidateScene).
    const MediaItem* mi = m_project ? m_project->findMedia(c.mediaId) : nullptr;
    const QString path = mi ? mi->filePath : QString();
    const ClipVisKey key{c.id, r.width(), r.height(), m_clipEpoch};
    QPixmap content = m_clipPix.value(key);
    if (content.isNull() || content.size() != r.size()) {
        content = QPixmap(r.size());
        content.fill(Qt::transparent);
        QPainter cp(&content);
        const QRect cr(0, 0, r.width(), r.height());
        if (audio)
            drawAudioWaveform(cp, cr, c, path);
        else if (c.isText)
            drawTextClipBody(cp, cr, c);
        else
            drawVideoThumbs(cp, cr, c, path);
        drawFadeCorners(cp, cr, c);
        if (m_tool == ToolEnvelope && (m_showVolLines || !audio))
            drawEnvelope(cp, cr, c, audio);
        // Orçamento simples de memória: além de N entradas, estoura limpa.
        const qint64 bytes = (qint64)content.width() * content.height()
                             * (content.depth() / 8);
        if (m_clipBytes + bytes > 96LL * 1024 * 1024) {
            qDebug() << "[TimelineWidget::drawClip] CLIP PIXMAP CACHE CLEARED (budget exceeded:"
                     << m_clipBytes << "+" << bytes << ")";
            m_clipPix.clear();
            m_clipBytes = 0;
        }
        m_clipBytes += bytes;
        m_clipPix.insert(key, content);
    }
    p.drawPixmap(r.topLeft(), content);

    // Envelope vai no pixmap cacheado; losangos de keyframe desenhados
    // dinamicamente (baratos), preservando o comportamento original: com a
    // ferramenta de envelope os keyframes não aparecem por cima da curva.
    if (m_tool != ToolEnvelope)
        drawKeyframeDiamonds(p, r, c, audio);

    QString label = c.name.isEmpty() ? tr.name : c.name;
    if (audio) {
        label += QString("  ·  v %1%").arg((int)llround(c.volume * 100.0));
        if (c.hasAudioFx())
            label += QString("  ·  FX");
    }
    if (std::fabs(c.speed - 1.0) > 1e-4)
        label += QString("  ·  %1×").arg(c.speed, 0, 'g', 3);
    QFont f = p.font();
    f.setPointSizeF(8.5);
    p.setFont(f);
    QFontMetrics fm(f);
    label = fm.elidedText(label, Qt::ElideRight, std::max(1, r.width() - 10));
    QRect labelRect(r.left() + 3, r.top() + 2, r.width() - 6, fm.height());
    p.fillRect(labelRect, QColor(0, 0, 0, 110));
    p.setPen(QColor(235, 235, 240));
    p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, label);

    if (!audio) {
        QRect obar(r.left() + 5, r.top() + 22, std::max(1, r.width() - 10), 3);
        const int w = (int)(obar.width() * c.opacity);
        p.fillRect(obar, QColor(0, 0, 0, 110));
        p.fillRect(QRect(obar.x(), obar.y(), w, obar.height()), QColor(255, 255, 255, 190));
    }

    const QString range = fmtRuler(c.in) + " – " + fmtRuler(c.in + c.dur);
    f.setPointSizeF(7.5);
    p.setFont(f);
    QFontMetrics fm2(f);
    const QString rng = fm2.elidedText(range, Qt::ElideRight, std::max(1, r.width() - 10));
    QRect rangeRect(r.left() + 3, r.bottom() - fm2.height() - 2, r.width() - 6, fm2.height());
    p.fillRect(rangeRect, QColor(0, 0, 0, 110));
    p.setPen(QColor(190, 195, 205));
    p.drawText(rangeRect, Qt::AlignLeft | Qt::AlignVCenter, rng);
}

// Corpo de um clipe de texto independente na timeline: "T" + prévia do texto
// com a cor do estilo.
void TimelineWidget::drawTextClipBody(QPainter& p, const QRect& r, const Clip& c) {
    const TextStyle& st = *m_project->textStyleFor(c);
    QFont f = p.font();
    f.setPointSizeF(9.5);
    f.setBold(true);
    p.setFont(f);
    p.setPen(st.textColor);
    p.drawText(r.adjusted(5, 3, -5, -3), Qt::AlignLeft | Qt::AlignTop, tr("T"));
    f.setPointSizeF(8.5);
    f.setBold(false);
    p.setFont(f);
    p.setPen(QColor(220, 225, 235));
    QString txt = st.text.simplified();
    if (txt.isEmpty()) txt = tr("(texto vazio)");
    const QFontMetrics fm(f);
    txt = fm.elidedText(txt, Qt::ElideRight, std::max(10, r.width() - 16));
    p.drawText(QRect(r.left() + 4, r.top() + 18, std::max(10, r.width() - 8),
                     std::max(10, r.height() - 22)),
               Qt::AlignLeft | Qt::AlignTop, txt);
}

void TimelineWidget::drawAudioWaveform(QPainter& p, const QRect& r, const Clip& c,
                                       const QString& path) {
    if (path.isEmpty() || r.width() < 2) return;
    MediaCache& cache = MediaCache::instance();
    if (!cache.hasPeaks(path, c.audioStreamIndex)) {
        cache.requestPeaks(path, c.audioStreamIndex);
        p.setPen(QColor(255, 255, 255, 45));
        p.drawLine(r.left(), r.center().y(), r.right(), r.center().y());
        return;
    }

    const FFmpegAudioPeaks& pk = cache.peaks(path, c.audioStreamIndex);
    if (pk.min.isEmpty()) return;

    const int bps = pk.bucketsPerSecond > 0 ? pk.bucketsPerSecond : 1;
    const int x0 = r.left();
    const int x1 = r.right();
    const double dur = c.dur;
    const int midY = r.center().y();
    const double amp = r.height() / 2.0 - 2.0;

    p.setPen(QPen(QColor(110, 235, 185), 1));
    for (int x = x0; x <= x1; ++x) {
        const double t0 = c.in + (x - x0) * dur / (double)(x1 - x0);
        const double t1 = c.in + (x + 1 - x0) * dur / (double)(x1 - x0);
        int b0 = (int)std::floor(t0 * bps);
        int b1 = (int)std::floor(t1 * bps);
        if (b0 < 0) b0 = 0;
        if (b1 >= pk.min.size()) b1 = pk.min.size() - 1;
        if (b0 > b1) continue;

        float mn = 0.0f;
        float mx = 0.0f;
        for (int b = b0; b <= b1; ++b) {
            if (pk.min[b] < mn) mn = pk.min[b];
            if (pk.max[b] > mx) mx = pk.max[b];
        }
        const int y0 = (int)(midY - mx * amp);
        const int y1 = (int)(midY - mn * amp);
        if (y0 >= r.top() && y1 <= r.bottom())
            p.drawLine(x, y0, x, y1);
    }
}

void TimelineWidget::drawVideoThumbs(QPainter& p, const QRect& r, const Clip& c,
                                     const QString& path) {
    if (path.isEmpty()) return;
    const int mode = SettingsDialog::thumbMode();
    if (mode == 2) return;

    MediaCache& cache = MediaCache::instance();

    // Aplica cobertura (aspect correto, cortando o excesso) numa fatia.
    const auto drawCover = [&p](const QRect& slice, const QImage& img) {
        const double scale = std::max(slice.width() / (double)img.width(),
                                      slice.height() / (double)img.height());
        const int tw = (int)std::ceil(img.width() * scale);
        const int th = (int)std::ceil(img.height() * scale);
        p.drawImage(QRect(slice.x() + (slice.width() - tw) / 2,
                          slice.y() + (slice.height() - th) / 2, tw, th),
                    img);
    };

    QPainterPath clipPath;
    clipPath.addRoundedRect(r, 3, 3);
    p.save();
    p.setClipPath(clipPath);

    if (mode == 1) {
        // Miniatura no início e outra no fim do clipe.
        const int sw = std::min(96, std::max(1, r.width() / 3));
        const QRect slices[2] = {
            QRect(r.left(), r.top(), sw, r.height()),
            QRect(r.right() - sw + 1, r.top(), sw, r.height()),
        };
        const double ts[2] = { c.in, std::max(c.in, c.in + c.dur - 0.01) };
        QList<double> want;
        for (int i = 0; i < 2; ++i) {
            if (i == 1 && r.width() < sw * 2 + 2) break; // clipe estreito
            const double k = std::round(ts[i] * 10.0) / 10.0;
            QImage img = cache.thumb(path, k);
            if (img.isNull()) {
                want.append(k);
                p.fillRect(slices[i].adjusted(1, 1, -1, -1), QColor(0, 0, 0, 70));
                continue;
            }
            drawCover(slices[i], img);
        }
        if (!want.isEmpty()) cache.requestThumbs(path, want);
        p.restore();
        return;
    }

    // Modo padrão: fatias contínuas preenchendo o corpo do clipe.
    const int sliceW = 96;
    const int maxSlices = 20;
    const int n = std::clamp(std::max(1, r.width() / sliceW), 1, maxSlices);

    QList<double> want;
    for (int i = 0; i < n; ++i) {
        const int sliceLeft = r.left() + i * r.width() / n;
        const int sliceRight = r.left() + (i + 1) * r.width() / n;
        const int sw = sliceRight - sliceLeft;
        const QRect sliceRect(sliceLeft, r.top(), sw, r.height());

        const double t = c.in + (i + 0.5) / n * c.dur;
        const double k = std::round(t * 10.0) / 10.0;
        QImage img = cache.thumb(path, k);
        if (img.isNull()) {
            want.append(k);
            p.fillRect(sliceRect.adjusted(1, 1, -1, -1), QColor(0, 0, 0, 70));
            continue;
        }

        drawCover(sliceRect, img);
    }
    if (!want.isEmpty()) cache.requestThumbs(path, want);
    p.restore();
}

void TimelineWidget::drawEnvelope(QPainter& p, const QRect& r, const Clip& c, bool audio) {
    const double maxV = audio ? 3.0 : 1.0;
    const QVector<Keyframe>& keys = audio ? c.kfVolume : c.kfOpacity;

    QPainterPath path;
    const int steps = std::max(2, r.width() / 2);
    for (int i = 0; i <= steps; ++i) {
        // Keyframes são relativos ao início do clipe (padrão do exportador).
        const double t = (double)i / steps * c.dur;
        const double v = audio ? kfValue(c.kfVolume, c.volume, t)
                               : kfValue(c.kfOpacity, c.opacity, t);
        const double cl = std::clamp(v, 0.0, maxV) / maxV;
        const int x = r.left() + (int)std::lround((double)i / steps * r.width());
        const int y = r.bottom() - (int)std::lround(cl * (r.height() - 4.0)) - 2;
        if (i == 0) path.moveTo(x, y);
        else path.lineTo(x, y);
    }
    p.setPen(QPen(QColor(255, 220, 90), 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    for (const Keyframe& k : keys) {
        if (k.time < -1e-6 || k.time > c.dur + 1e-6) continue;
        const int x = r.left() + (int)std::lround(k.time / c.dur * r.width());
        const double cl = std::clamp(k.value, 0.0, maxV) / maxV;
        const int y = r.bottom() - (int)std::lround(cl * (r.height() - 4.0)) - 2;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 220, 90));
        p.drawEllipse(QPoint(x, y), 3, 3);
    }
}

void TimelineWidget::drawFadeCorners(QPainter& p, const QRect& r, const Clip& c) {
    if (c.fadeIn <= 0 && c.fadeOut <= 0) return;
    const double dur = std::max(c.dur, kMinDur);
    const int fi = (int)std::round(std::min(c.fadeIn, dur) / dur * r.width());
    const int fo = (int)std::round(std::min(c.fadeOut, dur) / dur * r.width());
    QPainterPath path;
    if (fi > 0) {
        path.moveTo(r.left(), r.bottom());
        path.lineTo(r.left() + fi, r.bottom());
        path.lineTo(r.left(), r.bottom() - fi);
        path.closeSubpath();
    }
    if (fo > 0) {
        path.moveTo(r.right(), r.bottom());
        path.lineTo(r.right() - fo, r.bottom());
        path.lineTo(r.right(), r.bottom() - fo);
        path.closeSubpath();
    }
    if (!path.isEmpty()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 130));
        p.drawPath(path);
    }
}

// Indicador de transição na região de sobreposição entre dois clipes.
void TimelineWidget::drawTransitionIndicator(QPainter& p, const QRect& r,
                                             const QString& type) {
    if (r.width() < 3 || r.height() < 3) return;
    p.fillRect(r, QColor(255, 170, 40, 70));
    // Hachura diagonal para o "não-lugar" da transição.
    p.setPen(QPen(QColor(255, 190, 80, 120), 1));
    const int step = 5;
    for (int x = r.left() - r.height(); x < r.right() + r.height(); x += step)
        p.drawLine(x, r.bottom(), x + r.height(), r.top());
    // Símbolo central: X para dissolve, seta para wipe.
    QFont f = p.font();
    f.setPointSizeF(8.5);
    f.setBold(true);
    p.setFont(f);
    QFontMetrics fm(f);
    QString glyph;
    if (type == QStringLiteral("wipeleft"))
        glyph = QStringLiteral("←");
    else if (type == QStringLiteral("wiperight"))
        glyph = QStringLiteral("→");
    else if (type == QStringLiteral("wipeup"))
        glyph = QStringLiteral("↑");
    else if (type == QStringLiteral("wipedown"))
        glyph = QStringLiteral("↓");
    else
        glyph = QStringLiteral("✕");
    const QRect symRect(r.left(), r.top(), r.width(), fm.height());
    p.setPen(QColor(255, 220, 140));
    p.drawText(symRect, Qt::AlignHCenter | Qt::AlignTop, glyph);
}

// Losangos de keyframe animados (estilo Vegas) marcando posição na linha do
// tempo. Uma cor por propriedade; lane central para não poluir o clipe.
void TimelineWidget::drawKeyframeDiamonds(QPainter& p, const QRect& r,
                                          const Clip& c, bool audio) {
    struct KfSet { const QVector<Keyframe>* keys; QColor color; };
    QVector<KfSet> sets;
    if (audio) {
        sets.append(KfSet{&c.kfVolume, QColor(110, 235, 185)});
    } else {
        sets.append(KfSet{&c.kfOpacity, QColor(255, 255, 255)});
        sets.append(KfSet{&c.kfScale, QColor(90, 200, 255)});
        sets.append(KfSet{&c.kfRotation, QColor(210, 150, 255)});
        sets.append(KfSet{&c.kfTx, QColor(255, 200, 90)});
        sets.append(KfSet{&c.kfTy, QColor(255, 160, 90)});
        sets.append(KfSet{&c.kfCropL, QColor(120, 255, 140)});
        sets.append(KfSet{&c.kfCropR, QColor(120, 255, 140)});
        sets.append(KfSet{&c.kfCropT, QColor(120, 255, 140)});
        sets.append(KfSet{&c.kfCropB, QColor(120, 255, 140)});
    }
    const double dur = std::max(c.dur, kMinDur);
    int lane = 0;
    for (const KfSet& s : sets) {
        if (!s.keys || s.keys->isEmpty()) continue;
        const int laneY = r.bottom() - (audio ? 20 : 22) - (lane % 2) * 10;
        for (const Keyframe& k : *s.keys) {
            const int x = r.left() + (int)std::lround(
                std::clamp(k.time, 0.0, dur) / dur * r.width());
            const QPolygonF dia = QPolygonF()
                << QPointF(x, laneY - 4) << QPointF(x + 4, laneY)
                << QPointF(x, laneY + 4) << QPointF(x - 4, laneY);
            p.setPen(QPen(QColor(20, 20, 24), 1));
            p.setBrush(s.color);
            p.drawPolygon(dia);
        }
        ++lane;
    }
}

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

void TimelineWidget::toggleMarker(double t) {
    if (!m_project) return;
    emit editStart();
    const double tt = snapTime(std::max(0.0, t));
    for (int i = 0; i < m_project->markers.size(); ++i) {
        if (std::fabs(m_project->markers[i].time - tt) < 1e-6) {
            m_project->markers.removeAt(i);
            emit modified();
            update();
            return;
        }
    }
    Marker m;
    m.id = newId();
    m.time = tt;
    m_project->addMarker(m);
    emit modified();
    update();
}

void TimelineWidget::drawTrackHeader(QPainter& p, int y, int rowH, const Track& tr, bool selected) {
    const int H = kHeaderW;
    bool anySolo = false;
    if (m_project) {
        for (const Track& t : m_project->videoTracks) if (t.solo) { anySolo = true; break; }
        if (!anySolo)
            for (const Track& t : m_project->audioTracks) if (t.solo) { anySolo = true; break; }
    }

    // Fundo achatado, na mesma família de cores das faixas (identidade do app).
    QColor base = selected
        ? (tr.audio ? QColor(42, 48, 62) : QColor(44, 50, 64))
        : (tr.audio ? QColor(34, 34, 38) : QColor(37, 37, 41));
    if (tr.locked) base = selected ? QColor(58, 46, 46) : QColor(46, 38, 38);
    p.fillRect(0, y, H, rowH, base);

    // Indicador de seleção (fino) + faixa de tipo à esquerda.
    if (selected)
        p.fillRect(0, y, 3, rowH, QColor(120, 170, 255));
    p.fillRect(0, y, 3, rowH,
               tr.locked ? QColor(200, 90, 90)
                         : (tr.audio ? QColor(70, 160, 120) : QColor(90, 140, 210)));

    QFont basef = p.font();
    // Nome da faixa.
    QFont f = basef;
    f.setBold(true);
    f.setPointSizeF(8.5);
    p.setFont(f);
    p.setPen(tr.audio ? QColor(200, 200, 208) : QColor(215, 215, 222));
    p.drawText(QRect(12, y + 2, H - 20, 16), Qt::AlignLeft | Qt::AlignVCenter, tr.name);

    // Ícone de tipo ao lado do nome: câmera (vídeo) / alto-falante (áudio),
    // nas mesmas cores dos clipes do app.
    p.setRenderHint(QPainter::Antialiasing, true);
    if (tr.audio) {
        const QColor c = tr.locked ? QColor(215, 150, 150) : QColor(70, 160, 120);
        QPainterPath sp;
        sp.moveTo(4.5, y + 8);
        sp.lineTo(9.0, y + 5);
        sp.lineTo(9.0, y + 11);
        sp.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(sp);
        p.fillRect(QRectF(9, y + 6.5, 4.5, 3), c);
        p.setPen(QPen(c, 1));
        p.drawArc(QRectF(11.5, y + 5, 4.5, 6), 0, 180 * 16);
    } else {
        const QColor c = tr.locked ? QColor(215, 150, 150) : QColor(90, 140, 210);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(3, y + 4.5, 10.5, 7), 1.6, 1.6);
        p.setBrush(QColor(22, 24, 28));
        p.drawEllipse(QPointF(8.2, y + 8), 2.3, 2.3);
    }
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setFont(basef);

    if (tr.audio) {
        // Indicador do volume da faixa no cabeçalho (0–200%), refletindo a régua.
        const QString pct = QString("%1%").arg((int)llround(tr.volume * 100.0));
        QFont vf = basef;
        vf.setPointSizeF(7.5);
        vf.setBold(true);
        p.setFont(vf);
        p.setPen(QColor(120, 190, 150));
        p.drawText(QRect(6, y + 19, H - 12, 14), Qt::AlignRight | Qt::AlignVCenter, pct);
        p.setFont(basef);
    }

    // Botões M/S/L achatados (estilo Fusion do app). Geometria igual à do
    // headerBtnAt().
    const bool audible = !tr.muted && !(anySolo && !tr.solo);
    const QColor dim(110, 110, 120);
    const int btnY = y + rowH - 22;
    const int size = 18, gap = 3, bx0 = 6;
    auto drawBtn = [&](int idx, const QString& label, bool active, const QColor& on) {
        const int bx = bx0 + idx * (size + gap);
        const QRect r(bx, btnY, size, size);
        p.setPen(QPen(active ? on.lighter(140) : QColor(48, 48, 54), 1));
        p.setBrush(active ? on : QColor(52, 52, 58));
        p.drawRect(r);
        p.setPen(active ? QColor(255, 255, 255) : dim);
        QFont bf = basef;
        bf.setBold(true);
        bf.setPointSizeF(7.5);
        p.setFont(bf);
        p.drawText(r, Qt::AlignCenter, label);
        p.setFont(basef);
    };
    drawBtn(0, QStringLiteral("M"), tr.muted || !audible, QColor(200, 90, 70));
    drawBtn(1, QStringLiteral("S"), tr.solo, QColor(215, 170, 50));
    drawBtn(2, QStringLiteral("L"), tr.locked, QColor(90, 160, 210));

    // Alça de redimensionamento no rodapé do cabeçalho da faixa.
    const int gy0 = y + rowH - kResizeHandleH;
    p.fillRect(0, gy0, H, kResizeHandleH, QColor(26, 27, 31));
    p.setPen(QColor(70, 72, 80));
    const int gx0 = (H - 26) / 2;
    for (int i = 0; i < 4; ++i)
        p.drawLine(gx0 + i * 8, gy0 + 2, gx0 + i * 8, gy0 + 3);
}

// Retorna o índice da alça de redimensionamento na qual o cursor está (0) ou
// -1 se fora. Só vale na coluna de cabeçalho (x < kHeaderW).
int TimelineWidget::resizeHandleAt(const QPoint& pos, int& row, bool& audio) const {
    if (pos.x() >= kHeaderW || pos.y() < kRulerH || !m_project) return -1;
    if (!rowFromY(pos.y(), row, audio)) return -1;
    const int y = audio ? rowY(-1, row) : rowY(row, -1);
    const int rowH = trackH(row, audio);
    if (pos.y() >= y + rowH - kResizeHandleH && pos.y() < y + rowH)
        return 0;
    return -1;
}

// Posição vertical da régua de volume da faixa de áudio: volume 0% na base da
// faixa, 200% no topo (estilo Vegas).
int TimelineWidget::volLineY(int row, bool audio, const Track& tr) const {
    const int rowH = trackH(row, audio);
    const int y = rowY(-1, row);
    const int pad = 6;
    const double frac = std::clamp(tr.volume, 0.0, 2.0) / 2.0;
    return y + rowH - pad - (int)std::lround(frac * (rowH - pad * 2.0));
}

// Linha de volume individual de um clipe de áudio (mesma escala da faixa).
int TimelineWidget::clipVolLineY(int row, const Clip& c) const {
    const int rowH = trackH(row, true);
    const int y = rowY(-1, row);
    const int pad = 6;
    const double frac = std::clamp(c.volume, 0.0, 2.0) / 2.0;
    return y + rowH - pad - (int)std::lround(frac * (rowH - pad * 2.0));
}

// Se o ponto está sobre a linha de volume de um clipe de áudio, retorna o
// clipe (dentro de uma tolerância vertical e do intervalo horizontal dele).
Clip* TimelineWidget::clipVolAt(const QPoint& pos, int& row) const {
    if (!m_project || pos.x() < kHeaderW || pos.y() < kRulerH) return nullptr;
    int r = -1;
    bool audio = false;
    if (!rowFromY(pos.y(), r, audio) || !audio) return nullptr;
    const double t = xToTime(pos.x());
    Clip* c = clipAt(r, true, t);
    if (!c) return nullptr;
    const int ly = clipVolLineY(r, *c);
    if (std::abs(pos.y() - ly) <= 4) { row = r; return c; }
    return nullptr;
}

// Se o ponto está sobre uma faixa de áudio (na área de conteúdo, fora de um
// clipe), retorna o índice da faixa para o ajuste de volume. Toda a altura da
// faixa é aceita: segurar sobre a faixa e arrastar para cima/baixo ajusta o
// volume (estilo Vegas), sem precisar acertar a linha. -1 se fora.
int TimelineWidget::volRowAt(const QPoint& pos, int& row) const {
    if (!m_project || pos.x() < kHeaderW || pos.y() < kRulerH) return -1;
    int r = -1;
    bool audio = false;
    if (!rowFromY(pos.y(), r, audio) || !audio) return -1;
    if (pos.y() < rowY(-1, r) || pos.y() >= rowY(-1, r) + trackH(r, true)) return -1;
    row = r;
    return 0;
}

// 0=M, 1=S, 2=L, -1=nenhum.
int TimelineWidget::headerBtnAt(const QPoint& pos, int& row, bool& audio) const {
    if (pos.x() >= kHeaderW || pos.y() < kRulerH || !m_project) return -1;
    if (!rowFromY(pos.y(), row, audio)) return -1;
    const int rowH = trackH(row, audio);
    const int y = audio ? rowY(-1, row) : rowY(row, -1);
    const int btnY = y + rowH - 22;
    if (pos.y() < btnY || pos.y() >= btnY + 18) return -1;
    const int dx = pos.x() - 6;
    const int size = 18, gap = 3;
    for (int i = 0; i < 3; ++i) {
        const int bx = i * (size + gap);
        if (dx >= bx && dx < bx + size) return i;
    }
    return -1;
}

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

void TimelineWidget::copySelected() {
    if (m_selected.isEmpty() || !m_project) return;
    m_clipboard.clear();
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* c = findClipById(id);
        if (!c || trackLocked(c)) continue;
        ClipboardEntry e;
        e.clip = *c;
        e.audio = false;
        e.track = 0;
        bool found = false;
        for (int i = 0; i < m_project->videoTracks.size() && !found; ++i)
            for (const Clip& x : m_project->videoTracks[i].clips)
                if (&x == c) { e.track = i; e.audio = false; found = true; break; }
        for (int i = 0; i < m_project->audioTracks.size() && !found; ++i)
            for (const Clip& x : m_project->audioTracks[i].clips)
                if (&x == c) { e.track = i; e.audio = true; found = true; break; }
        if (found) m_clipboard.append(e);
    }
}

void TimelineWidget::cutSelected() {
    if (m_selected.isEmpty()) return;
    copySelected();
    deleteSelected();
}

void TimelineWidget::pasteClips() {
    if (m_clipboard.isEmpty() || !m_project) return;
    emit editStart();
    double minPos = 1e18;
    for (const ClipboardEntry& e : m_clipboard) minPos = std::min(minPos, e.clip.pos);
    const double base = snapTime(std::max(0.0, m_playhead));

    QHash<QString, QString> groupMap; // groupId original -> novo
    QHash<Track*, QVector<Clip>> placed;
    QStringList pasted;

    auto targetTrack = [this](const ClipboardEntry& e) -> Track* {
        if (e.audio)
            return (e.track >= 0 && e.track < m_project->audioTracks.size())
                ? &m_project->audioTracks[e.track] : nullptr;
        return (e.track >= 0 && e.track < m_project->videoTracks.size())
            ? &m_project->videoTracks[e.track] : nullptr;
    };
    // Pula faixas travadas.
    auto usable = [](Track* tr) -> Track* { return (tr && tr->locked) ? nullptr : tr; };
    // Desloca para não colidir com clipes já existentes na faixa.
    auto fitPos = [](const QVector<Clip>& blockers, const Clip& c, double np) {
        np = std::max(0.0, np);
        for (const Clip& o : blockers) {
            if (np < o.pos) {
                if (np + c.dur > o.pos) np = o.pos - c.dur;
            } else if (np < o.pos + o.dur) {
                np = o.pos + o.dur;
            }
            if (np < 0.0) np = 0.0;
        }
        return std::max(0.0, np);
    };

    for (const ClipboardEntry& e : m_clipboard) {
        Clip nc = e.clip;
        nc.id = newId();
        if (!e.clip.groupId.isEmpty()) {
            if (!groupMap.contains(e.clip.groupId))
                groupMap.insert(e.clip.groupId, newId());
            nc.groupId = groupMap.value(e.clip.groupId);
        } else {
            nc.groupId.clear();
        }
        nc.pos = base + (e.clip.pos - minPos);
        Track* tr = usable(targetTrack(e));
        if (!tr) continue;
        QVector<Clip> blockers = tr->clips;
        blockers += placed.value(tr);
        nc.pos = fitPos(blockers, nc, nc.pos);
        tr->clips.push_back(nc);
        placed[tr].append(nc);
        pasted.append(nc.id);
    }
    if (pasted.isEmpty()) return;
    clearTrackSelection();
    m_selected = pasted;
    invalidateScene();
    updateScrollRanges();
    update();
    emit modified();
    emit selectionChanged(m_selected.last());
}

void TimelineWidget::duplicateSelected() {
    if (m_selected.isEmpty() || !m_project) return;
    QStringList reps;
    QSet<QString> seen;
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* c = findClipById(id);
        if (!c || trackLocked(c)) continue;
        const QString key = c->groupId.isEmpty() ? c->id : c->groupId;
        if (!seen.contains(key)) { seen.insert(key); reps.append(c->id); }
    }
    if (reps.isEmpty()) return;
    emit editStart();
    for (const QString& id : reps) {
        Clip* c = findClipById(id);
        if (c) duplicateClip(c);
    }
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::nudgeSelected(int dir) {
    if (m_selected.isEmpty() || !m_project) return;
    emit editStart();
    const double frames = (QApplication::keyboardModifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
    const double d = dir * frames / m_project->fps;
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* c = findClipById(id);
        if (!c || trackLocked(c)) continue;
        c->pos = snapTime(std::max(0.0, c->pos + d));
    }
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::selectAllClips() {
    if (!m_project) return;
    clearTrackSelection();
    m_selected.clear();
    for (const Track& t : m_project->videoTracks)
        for (const Clip& c : t.clips) m_selected.append(c.id);
    for (const Track& t : m_project->audioTracks)
        for (const Clip& c : t.clips) m_selected.append(c.id);
    refreshView();
    emit selectionChanged(m_selected.isEmpty() ? QString() : m_selected.last());
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
    if (!m_project) return;
    const int x = e->pos().x();
    const int y = e->pos().y();
    setFocus();

    if (e->button() != Qt::LeftButton) return;

    m_volPending = false;

    if (y < kRulerH) {
        const int zx0 = 6;
        if (x >= zx0 && x < zx0 + kZoomW) {
            const bool zoomIn = x >= zx0 + kZoomW / 2;
            zoomBy(zoomIn ? 1.6 : 1.0 / 1.6, m_playhead);
            return;
        }
        if (e->modifiers() & Qt::ControlModifier) {
            toggleMarker(xToTime(x));
            return;
        }
        // Perto da agulha: arrasta o playhead. Fora dela: clique define o
        // playhead e arrasto define a região de loop.
        const double px = timeToX(m_playhead);
        if (std::fabs(x - px) <= 6) {
            m_dragMode = PlayheadDrag;
            const double t2 = std::max(0.0, snapTime(xToTime(x)));
            setPlayhead(t2);
            emit playheadChanged(t2);
            m_dragStart = e->pos();
            update();
            return;
        }
        // Arrastar na régua define a região de loop; clique define o playhead.
        m_dragMode = RulerLoop;
        m_loopPressT = std::max(0.0, snapTime(xToTime(x)));
        m_dragStart = e->pos();
        setPlayhead(m_loopPressT);
        emit playheadChanged(m_loopPressT);
        return;
    }

    // Alça de redimensionamento da faixa (rodapé do cabeçalho).
    {
        int rrow;
        bool raudio;
        if (resizeHandleAt(e->pos(), rrow, raudio) >= 0) {
            m_dragMode = ResizeTrack;
            m_resizeRow = rrow;
            m_resizeAudio = raudio;
            m_resizeOrigH = trackH(rrow, raudio);
            m_resizeSelOrigH.clear();
            for (const TrackSel& s : m_selTracks) {
                if (s.row < 0) continue;
                m_resizeSelOrigH.append(trackH(s.row, s.audio));
            }
            m_dragStart = e->pos();
            setCursor(Qt::SizeVerCursor);
            return;
        }
    }

    // Linha de volume individual do clipe de áudio: arrastar ajusta só ele.
    if (m_showVolLines) {
        int cvrow;
        Clip* vclip = clipVolAt(e->pos(), cvrow);
        if (vclip) {
            emit editStart();
            m_dragMode = ClipVol;
            m_volClip = vclip->id;
            m_volClipOrig = vclip->volume;
            m_volRowOrig = cvrow;
            m_dragStart = e->pos();
            setCursor(Qt::SizeVerCursor);
            update();
            return;
        }
    }

    // Régua de volume das faixas de áudio: arrastar a linha ajusta o volume.
    // Só quando não há clipe sob o cursor (o clique no clipe continua
    // selecionando/arrastando normalmente, mesmo sobre a linha).
    if (m_showVolLines) {
        int vrow;
        const bool onLine = volRowAt(e->pos(), vrow) >= 0
            && std::abs(e->pos().y() - volLineY(vrow, true, m_project->audioTracks[vrow])) <= 6;
        if (volRowAt(e->pos(), vrow) >= 0) {
            int r2;
            bool a2;
            bool overClip = false;
            if (rowFromY(y, r2, a2) && clipAt(r2, a2, xToTime(x)) != nullptr)
                overClip = true;
            if (!overClip && onLine) {
                emit editStart();
                m_dragMode = TrackVol;
                m_volRow = vrow;
                m_volOrig = m_project->audioTracks[vrow].volume;
                m_dragStart = e->pos();
                setCursor(Qt::SizeVerCursor);
                update();
                return;
            }
            if (!overClip && !onLine && (m_tool == ToolSelect || m_tool == ToolMove)) {
                // Segurou na faixa (fora da linha e sem clipe): marca como
                // "pode virar volume". Se o arraste for predominantemente
                // vertical vira ajuste de volume; senão segue o fluxo normal
                // (playhead/marquee).
                m_volPending = true;
                m_volRow = vrow;
                m_volOrig = m_project->audioTracks[vrow].volume;
            }
        }
    }

    // Botões de cabeçalho (M/S/L) das faixas.
    {
        int brow;
        bool baudio;
        const int b = headerBtnAt(e->pos(), brow, baudio);
        if (b >= 0) {
            Track* tr = baudio ? &m_project->audioTracks[brow]
                               : &m_project->videoTracks[brow];
            emit editStart();
            if (b == 0) tr->muted = !tr->muted;
            else if (b == 1) tr->solo = !tr->solo;
            else tr->locked = !tr->locked;
            // Clicar no botão também seleciona a faixa (qualquer lugar do
            // cabeçalho seleciona), respeitando Shift/Ctrl.
            if (e->modifiers() & Qt::ShiftModifier)
                selectTrackRange(brow, baudio);
            else if (e->modifiers() & Qt::ControlModifier)
                toggleTrackSel(brow, baudio);
            else
                setTrackSel(brow, baudio);
            invalidateScene();
            emit modified();
            return;
        }
    }

    // Seleção de faixas: clicar na coluna do cabeçalho seleciona a faixa.
    // Shift = seleção em faixa (ordem exibida), Ctrl = alterna uma a uma.
    if (x < kHeaderW) {
        // Clicar na faixa de cabeçalho de uma pasta seleciona todas as suas
        // faixas; clicar na seta (à esquerda) recolhe/expande a pasta.
        QString gid;
        if (folderStripAt(y, gid)) {
            TrackGroup* g = m_project->findGroup(gid);
            const QRect ar = g ? folderArrowRect(*g) : QRect();
            if (g && x >= ar.left() && x <= ar.right()) {
                toggleGroupCollapsed(gid);
                return;
            }
            if (e->modifiers() & Qt::ControlModifier)
                toggleGroupTracks(gid);
            else
                selectGroupTracks(gid);
            refreshView();
            // Prepara o arrasto do grupo inteiro (mover a pasta acima/abaixo).
            m_dragGroupId = gid;
            m_dragTrackRow = -1;
            m_trackDragActive = false;
            m_dropRow = -1;
            m_dropAudio = false;
            m_dropGroup.clear();
            m_dragStart = e->pos();
            return;
        }
        int srow;
        bool saudio;
        if (rowFromY(y, srow, saudio)) {
            if (e->modifiers() & Qt::ControlModifier)
                toggleTrackSel(srow, saudio);
            else if (e->modifiers() & Qt::ShiftModifier)
                selectTrackRange(srow, saudio);
            else
                setTrackSel(srow, saudio);
            refreshView();
            // Prepara o arrasto de faixa (mover acima/abaixo ou soltar em pasta).
            m_dragTrackRow = srow;
            m_dragTrackAudio = saudio;
            m_trackDragActive = false;
            m_dropRow = -1;
            m_dropAudio = false;
            m_dropGroup.clear();
            m_dragStart = e->pos();
            return;
        }
    }

    int row;
    bool audio;
    if (rowFromY(y, row, audio)) {
        const double t = xToTime(x);
        // Clique na timeline move a agulha para onde o mouse clicou. Ctrl+clique
        // apenas seleciona (não mexe na agulha), para multi-seleção ficar confortável.
        if (!(e->modifiers() & Qt::ControlModifier)) {
            setPlayhead(std::max(0.0, snapTime(t)));
            emit playheadChanged(std::max(0.0, snapTime(t)));
        }
        Clip* clip = clipAt(row, audio, t);

        // Shift+clique em qualquer ponto da faixa (mesmo sobre um clipe)
        // seleciona o intervalo de faixas na ordem exibida, mantendo o que já
        // estava selecionado a partir da âncora.
        if (e->modifiers() & Qt::ShiftModifier) {
            selectTrackRange(row, audio);
            refreshView();
            return;
        }

        if (m_tool == ToolScissors) {
            m_dragMode = Razor;
            m_razorT = std::max(0.0, snapTime(t));
            m_dragStart = e->pos();
            update();
            return;
        }
        if (m_tool == ToolEnvelope) {
            if (clip) envelopePress(clip, std::max(0.0, snapTime(t)));
            return;
        }
        if (m_tool == ToolZoom) {
            m_dragMode = ZoomSelect;
            m_zoomT0 = std::max(0.0, t);
            m_zoomT1 = m_zoomT0;
            m_dragStart = e->pos();
            update();
            return;
        }

        // Seleção de faixas também clicando no corpo da faixa: clique sem
        // clipe seleciona/alterna a faixa. O playhead continua movendo e o
        // marquee continua funcionando no arraste.
        if (!clip && (m_tool == ToolSelect || m_tool == ToolMove)) {
            if (e->modifiers() & Qt::ControlModifier)
                toggleTrackSel(row, audio);
            else
                setTrackSel(row, audio);
            refreshView();
        }

        if (clip) {
            // Ctrl+clique sobre um clipe alterna a seleção do CLIPE (permite
            // selecionar vários clipes). Seleção de faixas fica no cabeçalho e
            // no corpo vazio da faixa (que continuam com Ctrl = alternar faixa).
            if (e->modifiers() & Qt::ControlModifier) {
                toggleSelection(clip->id);
                return;
            }
            if (!isSelected(clip->id))
                setSelection(clip->id);
            m_dragMode = None;
            if (m_tool == ToolMove) {
                m_dragMode = MoveClip;
            } else {
                const int cx = (int)timeToX(clip->pos);
                const int cw = (int)(clip->dur * m_pps);
                const int dx = x - cx;
                if (dx <= 8) m_dragMode = TrimLeft;
                else if (cw - dx <= 8) m_dragMode = TrimRight;
                else m_dragMode = MoveClip;
            }
            if (trackLocked(clip)) m_dragMode = None; // faixa travada: só seleciona
            m_dragClip = clip->id;
            m_dragUndoPushed = false;
            m_dragStart = e->pos();
            m_dragOrigPos = clip->pos;
            m_dragOrigIn = clip->in;
            m_dragOrigDur = clip->dur;
            m_dragOrig.clear();
            // Alt = arrastar apenas o clipe, sem arrastar o grupo vinculado.
            const bool solo = (e->modifiers() & Qt::AltModifier) != 0;
            auto addOrig = [this, solo](Clip* c) {
                if (!c) return;
                if (!solo && !c->groupId.isEmpty()) {
                    for (Clip* m : groupMembers(c->groupId))
                        m_dragOrig.insert(m->id, {m->pos, m->in, m->dur});
                } else {
                    m_dragOrig.insert(c->id, {c->pos, c->in, c->dur});
                }
            };
            for (const QString& sid : m_selected) {
                Clip* sc = findClipById(sid);
                addOrig(sc);
            }
            addOrig(clip);
        } else if (m_tool == ToolSelect || m_tool == ToolMove) {
            // Espaço vazio com a ferramenta de seleção: caixa de seleção
            // (marquee). Com Ctrl, soma aos já selecionados.
            m_dragMode = Marquee;
            m_dragStart = e->pos();
            m_marqueeRect = QRect(e->pos(), QSize(0, 0));
            update();
        } else {
            m_selected.clear();
            refreshView();
            emit selectionChanged(QString());
        }
        update();
    } else {
        m_selected.clear();
        clearTrackSelection();
        refreshView();
        emit selectionChanged(QString());
        update();
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!m_project) return;

    if (e->buttons() & Qt::LeftButton) {
        // Arrasto de faixa ou grupo: cruza o limiar e passa a mover/reordenar.
        if ((m_dragTrackRow >= 0 || !m_dragGroupId.isEmpty()) && !m_trackDragActive
            && (e->pos() - m_dragStart).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragMode = TrackDrag;
            m_trackDragActive = true;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_dragMode == TrackDrag) {
            const QPoint p = e->pos();
            QString gid;
            if (p.x() < kHeaderW && folderStripAt(p.y(), gid)) {
                // Sobre uma faixa de pasta: destaca o grupo e calcula o alvo
                // como a primeira faixa abaixo da pasta (para reordenar).
                m_dropGroup = gid;
                const TrackGroup* g = m_project->findGroup(gid);
                if (g) {
                    const QRect fr = folderStripRect(*g);
                    int trow;
                    bool taudio;
                    if (rowFromY(fr.bottom() + 1, trow, taudio)) {
                        m_dropRow = trow;
                        m_dropAudio = taudio;
                    } else {
                        m_dropRow = -1;
                    }
                } else {
                    m_dropRow = -1;
                }
            } else {
                m_dropGroup.clear();
                int trow;
                bool taudio;
                if (rowFromY(p.y(), trow, taudio)) {
                    m_dropRow = trow;
                    m_dropAudio = taudio;
                } else {
                    m_dropRow = -1;
                }
            }
            update();
            e->accept();
            return;
        }
        if (m_dragMode == ResizeTrack) {
            const int dy = e->pos().y() - m_dragStart.y();
            // Se a faixa arrastada está selecionada, redimensiona todas as
            // faixas selecionadas juntas (mesmo delta).
            if (isTrackSelected(m_resizeRow, m_resizeAudio)) {
                bool any = false;
                int j = 0;
                for (const TrackSel& s : m_selTracks) {
                    if (s.row < 0) continue;
                    const int origH = (j < m_resizeSelOrigH.size())
                        ? m_resizeSelOrigH[j] : trackH(s.row, s.audio);
                    ++j;
                    Track* tt = s.audio
                        ? (s.row < (int)m_project->audioTracks.size() ? &m_project->audioTracks[s.row] : nullptr)
                        : (s.row < (int)m_project->videoTracks.size() ? &m_project->videoTracks[s.row] : nullptr);
                    if (!tt) continue;
                    // Usa a altura ORIGINAL capturada no início do arraste (e
                    // não a atual), senão o delta total acumula a cada movimento.
                    const int nh = std::clamp(origH + dy, kMinRowH, kMaxRowH);
                    if (tt->height != nh) {
                        tt->height = nh;
                        any = true;
                    }
                }
                if (any) {
                    updateScrollRanges();
                    invalidateScene();
                    update();
                    emit modified();
                }
            } else {
                Track& t = m_resizeAudio ? m_project->audioTracks[m_resizeRow]
                                         : m_project->videoTracks[m_resizeRow];
                const int newH = std::clamp(m_resizeOrigH + dy, kMinRowH, kMaxRowH);
                if (t.height != newH) {
                    t.height = newH;
                    updateScrollRanges();
                    invalidateScene();
                    update();
                    emit modified();
                }
            }
            return;
        }
        if (m_dragMode == ClipVol) {
            Clip* clip = findClipById(m_volClip);
            if (clip && m_volRowOrig >= 0 && m_volRowOrig < (int)m_project->audioTracks.size()) {
                const int rowH = trackH(m_volRowOrig, true);
                const int y = rowY(-1, m_volRowOrig);
                const int pad = 6;
                const double frac = 1.0 - std::clamp(
                    (double)(e->pos().y() - (y + pad)) / (rowH - pad * 2.0), 0.0, 1.0);
                const double v = frac * 2.0;
                if (std::fabs(clip->volume - v) > 1e-4) {
                    clip->volume = v;
                    refreshView();
                    update();
                    emit modified();
                }
            }
            return;
        }
        if (m_dragMode == TrackVol) {
            if (m_volRow < 0 || m_volRow >= (int)m_project->audioTracks.size()) return;
            Track& t = m_project->audioTracks[m_volRow];
            const int rowH = trackH(m_volRow, true);
            const int y = rowY(-1, m_volRow);
            const int pad = 6;
            const double frac = 1.0 - std::clamp(
                (double)(e->pos().y() - (y + pad)) / (rowH - pad * 2.0), 0.0, 1.0);
            const double v = frac * 2.0;
            if (std::fabs(t.volume - v) > 1e-4) {
                t.volume = v;
                refreshView();
                update();
                emit modified();
            }
            return;
        }
        if (m_dragMode == PlayheadDrag) {
            const double t2 = std::max(0.0, snapTime(xToTime(e->pos().x())));
            setPlayhead(t2);
            emit playheadChanged(t2);
            startAutoScroll(e);
            update();
            return;
        }
        if (m_dragMode == RulerLoop) {
            const double cur = std::max(0.0, snapTime(xToTime(e->pos().x())));
            const double a = std::min(m_loopPressT, cur);
            const double b = std::max(m_loopPressT, cur);
            if (b - a > 0.02) {
                m_loopIn = a;
                m_loopOut = b;
                emit loopChanged(m_loopIn, m_loopOut);
            }
            startAutoScroll(e);
            update();
            return;
        }
        if (m_dragMode == Razor) {
            m_razorT = std::max(0.0, snapTime(xToTime(e->pos().x())));
            update();
            return;
        }
        if (m_dragMode == ZoomSelect) {
            m_zoomT1 = std::max(0.0, xToTime(e->pos().x()));
            update();
            return;
        }
        if (m_dragMode == Marquee) {
            // Press na faixa de áudio vazia: vira ajuste de volume se o
            // arraste for predominantemente vertical (para cima/baixo).
            if (m_volPending) {
                const int dy = e->pos().y() - m_dragStart.y();
                const int dx = e->pos().x() - m_dragStart.x();
                if (std::abs(dy) > std::abs(dx) + 4 && std::abs(dy) >= 5) {
                    if (m_volRow >= 0 && m_volRow < (int)m_project->audioTracks.size()) {
                        emit editStart();
                        m_dragMode = TrackVol;
                        m_volPending = false;
                        m_volOrig = m_project->audioTracks[m_volRow].volume;
                        setCursor(Qt::SizeVerCursor);
                        // aplica o volume já neste movimento
                        if (m_dragMode == TrackVol) {
                            Track& t = m_project->audioTracks[m_volRow];
                            const int rowH = trackH(m_volRow, true);
                            const int y = rowY(-1, m_volRow);
                            const int pad = 6;
                            const double frac = 1.0 - std::clamp(
                                (double)(e->pos().y() - (y + pad)) / (rowH - pad * 2.0), 0.0, 1.0);
                            t.volume = frac * 2.0;
                            refreshView();
                        }
                        update();
                        emit modified();
                        return;
                    }
                }
            }
            m_marqueeRect = QRect(m_dragStart, e->pos()).normalized();
            update();
            return;
        }
        if (m_dragMode != None && !m_dragClip.isEmpty()) {
            Clip* clip = findClipById(m_dragClip);
            if (clip) {
                const double dt = (e->pos().x() - m_dragStart.x()) / m_pps;
                if (!m_dragUndoPushed && std::fabs(dt) > 1e-9) {
                    emit editStart();
                    m_dragUndoPushed = true;
                }
                if (m_dragMode == MoveClip) {
                    // Troca de faixa durante o arraste: cada clipe segue a
                    // linha sob o mouse quando o tipo bate (vídeo só em faixas
                    // de vídeo, áudio só em faixas de áudio). O membro do
                    // grupo vinculado NÃO acompanha na vertical: ele mantém a
                    // faixa em que está (só a posição no tempo anda junto).
    int row;
    bool audio;
    int vrow;
    int cvrow;
    if (m_showVolLines && clipVolAt(e->pos(), cvrow) != nullptr) {
        setCursor(Qt::SizeVerCursor);
        return;
    }
    if (m_showVolLines && volRowAt(e->pos(), vrow) >= 0) {
        int r2;
        bool a2;
        bool overClip = false;
        if (rowFromY(e->pos().y(), r2, a2) && clipAt(r2, a2, xToTime(e->pos().x())) != nullptr)
            overClip = true;
        if (!overClip) {
            setCursor(Qt::SizeVerCursor);
            return;
        }
    }
    if (rowFromY(e->pos().y(), row, audio)) {
                        // Só o clipe sob o mouse acompanha na vertical (muda de
                        // faixa se o tipo bater). Os demais membros do grupo
                        // mantêm suas faixas — só a posição no tempo anda junto.
                        // Antes movíamos TODOS os membros do tipo para a linha do
                        // mouse: as várias faixas de áudio de um arquivo multicanal
                        // (mesmo grupo) empilhavam todas na mesma faixa e
                        // sumiam umas sob as outras.
                        Clip* dc = findClipById(m_dragClip);
                        if (dc) {
                            int curRow;
                            bool curAudio;
                            if (clipTrackIndex(m_dragClip, curRow, curAudio)
                                && curAudio == audio && curRow != row) {
                                Track& dst = curAudio ? m_project->audioTracks[row]
                                                      : m_project->videoTracks[row];
                                if (!dst.locked)
                                    moveClipToTrack(m_dragClip, row, curAudio);
                            }
                        }
                    }
                    const double raw = m_dragOrigPos + dt;
                    const double snapped = snapToEdges(snapTime(raw), m_dragClip);
                    const double delta = snapped - m_dragOrigPos;
                    QSet<QString> moving;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it)
                        moving.insert(it.key());
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc)
                            sc->pos = clampPosToTrack(sc,
                                                      std::max(0.0, it.value().pos + delta),
                                                      moving);
                    }
                } else if (m_dragMode == TrimLeft) {
                    const double minEnd = std::max(0.0, m_dragOrigPos + m_dragOrigDur - kMinDur);
                    const double np = std::clamp(snapToEdges(snapTime(m_dragOrigPos + dt)),
                                                 0.0, minEnd);
                    const double delta = np - m_dragOrigPos;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (!sc) continue;
                        const ClipOrig& o = it.value();
                        sc->pos = std::max(0.0, o.pos + delta);
                        sc->in = o.in + delta;
                        sc->dur = o.dur - delta;
                    }
                } else if (m_dragMode == TrimRight) {
                    const double end = snapToEdges(snapTime(m_dragOrigPos + m_dragOrigDur + dt));
                    const double newDur = std::max(kMinDur, end - clip->pos);
                    const double deltaDur = newDur - m_dragOrigDur;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc) sc->dur = std::max(kMinDur, it.value().dur + deltaDur);
                    }
                }
                updateScrollRanges();
                update();
                emit modified();
            }
        }
        startAutoScroll(e);
        return;
    }

    int hrow;
    bool haudio;
    if (resizeHandleAt(e->pos(), hrow, haudio) >= 0) {
        setCursor(Qt::SizeVerCursor);
        return;
    }

    if (m_tool == ToolScissors) {
        setCursor(Qt::SplitVCursor);
        return;
    }
    if (m_tool == ToolEnvelope || m_tool == ToolZoom) {
        setCursor(Qt::CrossCursor);
        return;
    }

    int row;
    bool audio;
    if (rowFromY(e->pos().y(), row, audio)) {
        Clip* clip = clipAt(row, audio, xToTime(e->pos().x()));
        if (clip) {
            const int cx = (int)timeToX(clip->pos);
            const int cw = (int)(clip->dur * m_pps);
            const int dx = e->pos().x() - cx;
            if (m_tool == ToolMove) {
                setCursor(Qt::OpenHandCursor);
            } else if (dx <= 8 || cw - dx <= 8) {
                setCursor(Qt::SizeHorCursor);
            } else {
                setCursor(Qt::OpenHandCursor);
            }
        } else {
            setCursor(Qt::ArrowCursor);
        }
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e) {
    switch (m_dragMode) {
    case RulerLoop:
        break;
    case Razor:
        razorSplitAt(m_razorT);
        break;
    case Marquee:
        if (m_marqueeRect.width() < 4 && m_marqueeRect.height() < 4) {
            // Clique simples no vazio: desfaz a seleção (Ctrl mantém).
            if (!(e->modifiers() & Qt::ControlModifier)) {
                m_selected.clear();
                refreshView();
                emit selectionChanged(QString());
            }
        } else {
            selectInMarquee(e->modifiers() & Qt::ControlModifier);
        }
        m_marqueeRect = QRect();
        break;
    case ZoomSelect:
        if (std::fabs(m_zoomT1 - m_zoomT0) < 0.02)
            zoomBy(1.6, m_zoomT0);
        else
            applyZoomRect(m_zoomT0, m_zoomT1);
        break;
    case TrackDrag:
        finishTrackDrag();
        break;
    default:
        break;
    }
    m_dragMode = None;
    m_dragClip.clear();
    m_dragUndoPushed = false;
    m_dragOrig.clear();
    m_volRow = -1;
    m_volClip.clear();
    m_volPending = false;
    m_trackDragActive = false;
    m_dragTrackRow = -1;
    m_dragGroupId.clear();
    m_dropRow = -1;
    m_dropGroup.clear();
    stopAutoScroll();
    setCursor(Qt::ArrowCursor);
    update();
}

// Caixa de seleção: seleciona todos os clipes cujo retângulo cruza a caixa.
void TimelineWidget::selectInMarquee(bool add) {
    if (!m_project) return;
    const double t0 = xToTime(m_marqueeRect.left());
    const double t1 = xToTime(m_marqueeRect.right());
    if (t1 - t0 < 1e-9) return;
    QStringList found;
    auto collect = [&](const Track& tr, bool audio, int row) {
        const int y = audio ? rowY(-1, row) : rowY(row, -1);
        const int rowH = trackH(row, audio);
        if (m_marqueeRect.bottom() < y || m_marqueeRect.top() > y + rowH) return;
        for (const Clip& c : tr.clips)
            if (c.pos + c.dur > t0 && c.pos < t1)
                found.append(c.id);
    };
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (trackVisible(i, false))
            collect(m_project->videoTracks[i], false, i);
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        if (trackVisible(i, true))
            collect(m_project->audioTracks[i], true, i);
    if (!add) m_selected.clear();
    for (const QString& id : found)
        if (!m_selected.contains(id)) m_selected.append(id);
    clearTrackSelection();
    refreshView();
    emit selectionChanged(m_selected.isEmpty() ? QString() : m_selected.last());
    update();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_project) return;
    // Duplo clique na faixa de uma pasta (fora da seta) renomeia a pasta.
    if (e->pos().x() < kHeaderW) {
        QString gid;
        if (folderStripAt(e->pos().y(), gid)) {
            TrackGroup* g = m_project->findGroup(gid);
            const QRect ar = g ? folderArrowRect(*g) : QRect();
            if (g && !(e->pos().x() >= ar.left() && e->pos().x() <= ar.right())) {
                renameTrackGroup(gid);
                return;
            }
        }
    }
    // Duplo clique na agulha, ou na região do loop (na régua), limpa o loop.
    if (m_loopOut > m_loopIn) {
        const double px = timeToX(m_playhead);
        const double t = xToTime(e->pos().x());
        if (std::fabs(e->pos().x() - px) <= 6
            || (e->pos().y() < kRulerH && t >= m_loopIn && t <= m_loopOut)) {
            clearLoop();
            return;
        }
    }
    int row;
    bool audio;
    // Duplo clique no nome da faixa (topo do cabeçalho): renomeia a faixa.
    if (e->pos().x() < kHeaderW && e->pos().y() >= kRulerH
        && rowFromY(e->pos().y(), row, audio)) {
        const int y = audio ? rowY(-1, row) : rowY(row, -1);
        if (e->pos().y() >= y + 1 && e->pos().y() <= y + 20) {
            Track* trk = audio ? &m_project->audioTracks[row]
                               : &m_project->videoTracks[row];
            bool ok = false;
            const QString name = QInputDialog::getText(
                this, tr("Renomear faixa"), tr("Nome da faixa:"),
                QLineEdit::Normal, trk->name, &ok);
            if (ok && !name.trimmed().isEmpty()) {
                emit editStart();
                trk->name = name.trimmed();
                emit modified();
                update();
            }
            return;
        }
    }
    if (rowFromY(e->pos().y(), row, audio)) {
        Clip* clip = clipAt(row, audio, xToTime(e->pos().x()));
        if (clip) {
            // Duplo clique num clipe com texto (ou num clipe de texto) abre a
            // mesma janela usada para criar — vira a janela de edição do clipe.
            if (!audio && (clip->isText || !m_project->textStyleFor(*clip)->isEmpty())) {
                showTextEditorDialog(clip);
                return;
            }
            toggleSelection(clip->id);
            update();
        }
    }
}

void TimelineWidget::wheelEvent(QWheelEvent* e) {
    const QPoint delta = e->angleDelta();
    if (e->modifiers() & Qt::ControlModifier) {
        if (delta.y() == 0) return;
        const double factor = std::pow(1.1, delta.y() / 120.0);
        const double anchorT = m_viewStart + (e->position().x() - kHeaderW) / m_pps;
        animateZoomTo(m_pps * factor, anchorT);
    } else if (delta.y() != 0) {
        m_vbar->setValue(m_vbar->value() - delta.y());
    } else if (delta.x() != 0) {
        m_hbar->setValue(m_hbar->value() - delta.x());
    }
    e->accept();
}

void TimelineWidget::keyPressEvent(QKeyEvent* e) {
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    switch (e->key()) {
    case Qt::Key_Space:
        emit playPauseRequested();
        e->accept();
        break;
    case Qt::Key_S:
        cutAtPlayhead();
        e->accept();
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (shift)
            deleteSelectedLeaveGap();
        else
            deleteSelection();
        e->accept();
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomBy(1.6, m_playhead);
        e->accept();
        break;
    case Qt::Key_Minus:
        zoomBy(1.0 / 1.6, m_playhead);
        e->accept();
        break;
    case Qt::Key_C:
        if (ctrl) { copySelected(); e->accept(); break; }
        QWidget::keyPressEvent(e);
        break;
    case Qt::Key_X:
        if (ctrl) { cutSelected(); e->accept(); break; }
        QWidget::keyPressEvent(e);
        break;
    case Qt::Key_V:
        if (ctrl) { pasteClips(); e->accept(); break; }
        m_showVolLines = !m_showVolLines; // V: oculta/mostra as linhas de volume
        invalidateScene();
        e->accept();
        break;
    case Qt::Key_D:
        if (ctrl) { duplicateSelected(); e->accept(); break; }
        QWidget::keyPressEvent(e);
        break;
    case Qt::Key_A:
        if (ctrl) { selectAllClips(); e->accept(); break; }
        QWidget::keyPressEvent(e);
        break;
    case Qt::Key_Left:
        nudgeSelected(-1);
        e->accept();
        break;
    case Qt::Key_Right:
        nudgeSelected(1);
        e->accept();
        break;
    case Qt::Key_Home:
        setPlayhead(0.0);
        emit playheadChanged(0.0);
        e->accept();
        break;
    case Qt::Key_End:
        if (m_project) {
            setPlayhead(m_project->duration());
            emit playheadChanged(m_project->duration());
        }
        e->accept();
        break;
    default:
        QWidget::keyPressEvent(e);
    }
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_project) return;
    QMenu menu(this);
    Clip* clip = nullptr;
    QString gid;
    if (folderStripAt(e->pos().y(), gid)) {
        TrackGroup* g = m_project->findGroup(gid);
        if (g) {
            QAction* selAll = menu.addAction(tr("Selecionar faixas da pasta"));
            QAction* ren = menu.addAction(tr("Renomear pasta…"));
            QAction* col = menu.addAction(g->collapsed ? tr("Expandir pasta")
                                                       : tr("Recolher pasta"));
            QAction* del = menu.addAction(tr("Desagrupar faixas"));
            QAction* act = menu.exec(e->globalPos());
            if (act == selAll) {
                selectGroupTracks(gid);
                refreshView();
            } else if (act == ren) {
                renameTrackGroup(gid);
            } else if (act == col) {
                toggleGroupCollapsed(gid);
            } else if (act == del) {
                emit editStart();
                for (int i = (int)m_project->trackGroups.size() - 1; i >= 0; --i)
                    if (m_project->trackGroups[i].id == gid)
                        m_project->trackGroups.removeAt(i);
                for (Track& t : m_project->videoTracks)
                    if (t.groupId == gid) t.groupId.clear();
                for (Track& t : m_project->audioTracks)
                    if (t.groupId == gid) t.groupId.clear();
                updateScrollRanges();
                invalidateScene();
                emit modified();
            }
        }
        return;
    }

    int row;
    bool audio;
    if (rowFromY(e->pos().y(), row, audio))
        clip = clipAt(row, audio, xToTime(e->pos().x()));

    if (clip) {
        if (!isSelected(clip->id))
            setSelection(clip->id);
        update();
        QAction* cut = menu.addAction(tr("Dividir no playhead"));
        QAction* del = menu.addAction(tr("Excluir (junta os espaços)"));
        QAction* gapDel = menu.addAction(tr("Excluir (deixa espaço)"));
        QAction* dup = menu.addAction(tr("Duplicar"));
        menu.addSeparator();
        QAction* copy = menu.addAction(tr("Copiar"));
        QAction* ccut = menu.addAction(tr("Recortar"));
        QAction* paste = menu.addAction(tr("Colar"));
        menu.addSeparator();
        QAction* props = menu.addAction(tr("Propriedades…"));
        QAction* speedAct = menu.addAction(tr("Velocidade…"));
        QAction* unlink = nullptr;
        if (!clip->groupId.isEmpty())
            unlink = menu.addAction(tr("Desvincular grupo"));
        QAction* fx = nullptr;
        QAction* audioFx = nullptr;
        QAction* transform = nullptr;
        QAction* textAction = nullptr;
        QAction* panCrop = nullptr;
        if (!audio) {
            fx = menu.addAction(tr("Efeitos de vídeo…"));
            transform = menu.addAction(tr("Transformar…"));
            textAction = menu.addAction(tr("Texto…"));
            panCrop = menu.addAction(tr("Pancrop…"));
        } else {
            audioFx = menu.addAction(tr("Efeitos de áudio…"));
        }
        // Tipo da transição de saída (aplicada quando este clipe se sobrepõe
        // ao próximo da mesma faixa; a duração é o tamanho da sobreposição).
        QMenu* transMenu = nullptr;
        QActionGroup* transGrp = nullptr;
        QAction* transDissolve = nullptr;
        QAction* transWipeL = nullptr;
        QAction* transWipeR = nullptr;
        QAction* transWipeU = nullptr;
        QAction* transWipeD = nullptr;
        if (!audio) {
            menu.addSeparator();
            transMenu = menu.addMenu(tr("Transição de saída"));
            transGrp = new QActionGroup(transMenu);
            const QString cur = isTransition(clip->transitionType)
                                    ? clip->transitionType
                                    : QStringLiteral("dissolve");
            transDissolve = transMenu->addAction(tr("Dissolver"));
            transDissolve->setCheckable(true);
            transDissolve->setChecked(cur == QStringLiteral("dissolve"));
            transGrp->addAction(transDissolve);
            transMenu->addSeparator();
            transWipeL = transMenu->addAction(tr("Wipe ← (próximo vem da direita)"));
            transWipeL->setCheckable(true);
            transWipeL->setChecked(cur == QStringLiteral("wipeleft"));
            transGrp->addAction(transWipeL);
            transWipeR = transMenu->addAction(tr("Wipe → (próximo vem da esquerda)"));
            transWipeR->setCheckable(true);
            transWipeR->setChecked(cur == QStringLiteral("wiperight"));
            transGrp->addAction(transWipeR);
            transWipeU = transMenu->addAction(tr("Wipe ↑ (próximo vem de baixo)"));
            transWipeU->setCheckable(true);
            transWipeU->setChecked(cur == QStringLiteral("wipeup"));
            transGrp->addAction(transWipeU);
            transWipeD = transMenu->addAction(tr("Wipe ↓ (próximo vem de cima)"));
            transWipeD->setCheckable(true);
            transWipeD->setChecked(cur == QStringLiteral("wipedown"));
            transGrp->addAction(transWipeD);
            transMenu->setEnabled([&]() {
                // Só faz sentido se o clipe seguinte da faixa se sobrepõe a ele.
                const Track* tr = trackOf(clip);
                if (!tr || tr->audio) return false;
                const double end = clip->pos + clip->dur;
                for (const Clip& o : tr->clips)
                    if (o.id != clip->id && o.pos < end - 1e-6 && o.pos + o.dur > clip->pos + 1e-6
                        && o.pos >= clip->pos - 1e-6)
                        return true;
                return false;
            }());
        }
        QAction* act = menu.exec(e->globalPos());
        if (act == cut) cutAtPlayhead();
        else if (act == del) deleteSelected();
        else if (act == gapDel) deleteSelectedLeaveGap();
        else if (act == dup) { emit editStart(); duplicateClip(clip); }
        else if (act == copy) copySelected();
        else if (act == ccut) cutSelected();
        else if (act == paste) pasteClips();
        else if (act == props) showProperties(clip);
        else if (act == speedAct) showSpeedDialog(clip);
        else if (act == unlink) {
            emit editStart();
            for (Clip* m : groupMembers(clip->groupId))
                m->groupId.clear();
            update();
            emit modified();
        }
        else if (act == fx) showEffectsDialog(clip);
        else if (act == audioFx) showAudioEffectsDialog(clip);
        else if (act == transform) showTransformDialog(clip);
        else if (act == textAction) showTextEditorDialog(clip);
        else if (act == panCrop) emit pancropRequested(clip->id);
        else if (act == transDissolve || act == transWipeL || act == transWipeR
                 || act == transWipeU || act == transWipeD) {
            emit editStart();
            if (act == transDissolve) clip->transitionType = QStringLiteral("dissolve");
            else if (act == transWipeL) clip->transitionType = QStringLiteral("wipeleft");
            else if (act == transWipeR) clip->transitionType = QStringLiteral("wiperight");
            else if (act == transWipeU) clip->transitionType = QStringLiteral("wipeup");
            else if (act == transWipeD) clip->transitionType = QStringLiteral("wipedown");
            invalidateScene();
            emit modified();
        }
    } else if (e->pos().y() < kRulerH) {
        const double t = std::max(0.0, snapTime(xToTime(e->pos().x())));
        QAction* addM = menu.addAction(tr("Adicionar marcador"));
        QAction* addMN = menu.addAction(tr("Adicionar marcador nomeado…"));
        QAction* clearM = menu.addAction(tr("Limpar todos os marcadores"));
        QAction* act = menu.exec(e->globalPos());
        if (act == addM) {
            toggleMarker(t);
        } else if (act == addMN) {
            bool ok = false;
            const QString name =
                QInputDialog::getText(this, tr("Marcador"), tr("Nome do marcador:"),
                                      QLineEdit::Normal, QString(), &ok);
            if (ok && !name.isEmpty()) {
                emit editStart();
                Marker m;
                m.id = newId();
                m.time = t;
                m.name = name;
                m_project->addMarker(m);
                emit modified();
                update();
            }
        } else if (act == clearM) {
            emit editStart();
            m_project->markers.clear();
            emit modified();
            update();
        }
    } else {
        QAction* addV = menu.addAction(tr("Adicionar faixa de vídeo"));
        QAction* addA = menu.addAction(tr("Adicionar faixa de áudio"));
        QAction* newText = menu.addAction(tr("Novo texto…"));
        QAction* paste = nullptr;
        if (!m_clipboard.isEmpty()) paste = menu.addAction(tr("Colar"));
        QAction* act = nullptr;
        Track* track = nullptr;
        QMenu* blendMenu = nullptr;
        int vrow = -1;
        if (rowFromY(e->pos().y(), vrow, audio)) {
            if (!audio && vrow >= 0 && vrow < m_project->videoTracks.size())
                track = &m_project->videoTracks[vrow];
            else if (audio && vrow >= 0 && vrow < m_project->audioTracks.size())
                track = &m_project->audioTracks[vrow];
        }
        if (track && !track->audio) {
            menu.addSeparator();
            blendMenu = menu.addMenu(tr("Modo de composição"));
            const QStringList modes = {"normal", "screen", "multiply", "overlay",
                                       "darken", "lighten", "softlight", "hardlight",
                                       "difference", "addition", "subtract", "exclusion"};
            QActionGroup* grp = new QActionGroup(blendMenu);
            for (const QString& m : modes) {
                QAction* a = blendMenu->addAction(m);
                a->setCheckable(true);
                a->setChecked(track->blendMode == m);
                grp->addAction(a);
            }
        }
        // Clique com o direito na faixa: garante que ela esteja selecionada
        // (sem desfazer a seleção múltipla quando o clique já está nela).
        if (track) {
            selectTrackRightClick(vrow, audio);
            refreshView();
        }
        QAction* trackVol = nullptr;
        if (track) trackVol = menu.addAction(tr("Volume da faixa: %1%").arg((int)llround(track->volume * 100.0)));
        QAction* groupTracks = nullptr;
        QAction* renameGroup = nullptr;
        QAction* ungroupTracks = nullptr;
        if (track) {
            menu.addSeparator();
            if (m_selTracks.size() >= 2)
                groupTracks = menu.addAction(tr("Agrupar faixas em pasta…"));
            if (!track->groupId.isEmpty()) {
                renameGroup = menu.addAction(tr("Renomear pasta…"));
                ungroupTracks = menu.addAction(tr("Desagrupar faixas"));
            }
        }
        QAction* delTrack = nullptr;
        if (track) {
            menu.addSeparator();
            delTrack = menu.addAction(tr("Excluir faixa"));
        }
        act = menu.exec(e->globalPos());
        if (act == addV) addTrack(false);
        else if (act == addA) addTrack(true);
        else if (act == newText) {
            const double tt = std::max(0.0, snapTime(xToTime(e->pos().x())));
            addTextClipAt(vrow, tt);
        }
        else if (act == paste) pasteClips();
        else if (act == trackVol) {
            bool ok = false;
            const double v = QInputDialog::getDouble(
                this, tr("Volume da faixa"), tr("Volume (0–200%):"),
                track->volume * 100.0, 0.0, 200.0, 0, &ok);
            if (ok) {
                emit editStart();
                track->volume = v / 100.0;
                emit modified();
                update();
            }
        }
        else if (act == delTrack) {
            emit editStart();
            m_project->removeTrack(audio, vrow);
            pruneEmptyGroups();
            m_selected.clear();
            clearTrackSelection();
            invalidateScene();
            updateScrollRanges();
            emit modified();
        }
        else if (act == groupTracks) {
            createTrackGroup();
        }
        else if (act == renameGroup) {
            renameTrackGroup(track->groupId);
        }
        else if (act == ungroupTracks) {
            emit editStart();
            const QString gid = track->groupId;
            for (int i = (int)m_project->trackGroups.size() - 1; i >= 0; --i)
                if (m_project->trackGroups[i].id == gid)
                    m_project->trackGroups.removeAt(i);
            for (Track& t : m_project->videoTracks)
                if (t.groupId == gid) t.groupId.clear();
            for (Track& t : m_project->audioTracks)
                if (t.groupId == gid) t.groupId.clear();
            updateScrollRanges();
            invalidateScene();
            emit modified();
        }
        else if (track && act && blendMenu && act->parent() == blendMenu) {
            emit editStart();
            track->blendMode = act->text();
            emit modified();
            update();
        }
    }
}
void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QLatin1String(kMimeMedia)) || md->hasUrls()) {
        e->acceptProposedAction();
        e->accept();
    }
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QLatin1String(kMimeMedia)) || md->hasUrls()) {
        int row = -1;
        bool audio = false;
        QString gid;
        if (folderStripAt(e->position().toPoint().y(), gid)) {
            m_dragHoverRow = -1;
        } else if (rowFromY(e->position().toPoint().y(), row, audio)) {
            m_dragHoverRow = row;
            m_dragHoverAudio = audio;
        } else {
            m_dragHoverRow = -1;
        }
        update();
        e->acceptProposedAction();
        e->accept();
    }
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) {
    m_dragHoverRow = -1;
    update();
}

void TimelineWidget::dropEvent(QDropEvent* e) {
    if (!m_project) return;
    const QMimeData* md = e->mimeData();

    QStringList mediaIds;
    if (md->hasFormat(QLatin1String(kMimeMedia))) {
        // Arrasto vindo da lista de mídia do aplicativo (caminho clássico de
        // DnD; a pool também chama finishDrop() direto no arrasto manual).
        const QByteArray data = md->data(QLatin1String(kMimeMedia));
        for (QByteArray line : data.split('\n')) {
            line = line.trimmed();
            if (!line.isEmpty()) mediaIds.append(QString::fromUtf8(line));
        }
    } else if (md->hasUrls()) {
        // Arrasto de arquivos vindos do gerenciador de arquivos: importa para
        // a mídia do projeto e já coloca clipes no ponto solto.
        int added = 0;
        QStringList imported;
        for (const QUrl& u : md->urls()) {
            if (!u.isLocalFile()) continue;
            const QString path = u.toLocalFile();
            const FFmpegMediaInfo info = FFmpegDecoder::probe(path);
            if (!info.hasVideo && !info.hasAudio) continue;
            MediaItem m;
            m.id = newId();
            m.filePath = path;
            m.name = QFileInfo(path).completeBaseName();
            m.duration = info.duration;
            m.width = info.width;
            m.height = info.height;
            m.hasVideo = info.hasVideo;
            m.hasAudio = info.hasAudio;
            m.audioStreams = info.audioStreams;
            m.audioChannels = info.audioChannels;
            m_project->media.append(m);
            mediaIds.append(m.id);
            imported.append(path);
            ++added;
        }
        if (added == 0) return;
        SettingsDialog::warnMkvIfNeeded(this, imported);
        emit mediaImported(); // atualiza a lista de mídia e marca o projeto
    } else {
        return;
    }
    if (mediaIds.isEmpty()) return;

    finishDrop(mediaIds, e->position().toPoint());
}

void TimelineWidget::showDropHover(const QPoint& globalPos) {
    const QPoint pos = mapFromGlobal(globalPos);
    int row = -1;
    bool audio = false;
    m_dragHoverRow = rowFromY(pos.y(), row, audio) ? row : -1;
    m_dragHoverAudio = audio;
    update();
}

void TimelineWidget::hideDropHover() {
    m_dragHoverRow = -1;
    update();
}

void TimelineWidget::dropMediaAt(const QStringList& mediaIds, const QPoint& globalPos) {
    finishDrop(mediaIds, mapFromGlobal(globalPos));
}

void TimelineWidget::refreshSettings() {
    invalidateScene();
}

void TimelineWidget::finishDrop(const QStringList& mediaIds, const QPoint& dropPos) {
    if (!m_project || mediaIds.isEmpty()) return;
    int row = -1;
    bool audio = false;
    if (!rowFromY(dropPos.y(), row, audio)) {
        // Solta fora de uma faixa (régua, rodapé ou coluna de cabeçalho):
        // usa a primeira faixa desbloqueada compatível com a mídia.
        bool anyVideo = false;
        for (const QString& mid : mediaIds) {
            const MediaItem* m = m_project->findMedia(mid);
            if (m && m->hasVideo) { anyVideo = true; break; }
        }
        audio = !anyVideo;
        row = 0;
    }
    m_dragHoverRow = -1;

    emit editStart();
    double t = snapTime(std::max(0.0, xToTime(dropPos.x())));
    QString lastPlaced;

    // Acha a faixa (do tipo `audio`) que esteja livre para receber um clipe de
    // duração `dur` em `t`. Prefere `preferRow` (a faixa sob o mouse); se
    // ocupada, procura a primeira faixa desbloqueada que não sobreponha; se
    // nenhuma existir, cria uma nova faixa vazia.
    auto findFreeTrack = [this](bool audio, double t, double dur, int preferRow) {
        auto overlap = [t, dur](const QVector<Clip>& clips) {
            for (const Clip& o : clips)
                if (o.pos < t + dur - 1e-9 && o.pos + o.dur > t + 1e-9) return true;
            return false;
        };
        auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
        if (preferRow >= 0 && preferRow < (int)list.size()
            && !list[preferRow].locked && !overlap(list[preferRow].clips))
            return preferRow;
        for (int i = 0; i < (int)list.size(); ++i)
            if (!list[i].locked && !overlap(list[i].clips)) return i;
        m_project->addTrack(audio);
        return (int)list.size() - 1;
    };

    for (const QString& mid : mediaIds) {
        const MediaItem* m = m_project->findMedia(mid);
        if (!m) continue;
        const bool both = m->hasVideo && m->hasAudio;
        // Arquivos multicanal (OBS/câmera): um clipe de áudio POR stream, cada
        // um na sua faixa. O número de streams pode ser 0 mesmo com hasAudio.
        const int aStreams = m->hasAudio ? qMax(1, m->audioStreams) : 0;
        const double dur = mediaInsertDur(*m);

        // Vídeo primeiro: decide a faixa de vídeo e a posição.
        int vRow = -1;
        double vDur = 0.0;
        if (m->hasVideo) {
            vRow = findFreeTrack(false, t, dur, audio ? -1 : row);
            vDur = dur;
        }

        if (vRow < 0 && aStreams == 0) continue;

        // Grupo: vídeo+áudio vinculados OU áudio multicanal (várias faixas do
        // mesmo arquivo) — movem e são excluídos juntos.
        const QString gid = (both || aStreams > 1) ? newId() : QString();
        if (vRow >= 0) {
            Clip c;
            c.id = newId();
            c.groupId = gid;
            c.mediaId = mid;
            c.pos = t;
            c.in = 0.0;
            c.dur = vDur;
            c.name = m->name;
            auto& clips = m_project->videoTracks[vRow].clips;
            auto it = clips.begin();
            while (it != clips.end() && it->pos <= c.pos) ++it;
            clips.insert(it, c);
            lastPlaced = c.id;
        }
        for (int k = 0; k < aStreams; ++k) {
            const int aRow = findFreeTrack(true, t, dur, (k == 0 && audio) ? row : -1);
            if (aRow < 0) continue;
            Clip c;
            c.id = newId();
            c.groupId = gid;
            c.mediaId = mid;
            c.audioStreamIndex = k;
            c.pos = t;
            c.in = 0.0;
            c.dur = dur;
            c.name = aStreams > 1 ? QString("%1 (faixa %2)").arg(m->name).arg(k + 1)
                                  : m->name;
            auto& clips = m_project->audioTracks[aRow].clips;
            auto it = clips.begin();
            while (it != clips.end() && it->pos <= c.pos) ++it;
            clips.insert(it, c);
            if (lastPlaced.isEmpty()) lastPlaced = c.id;
        }
        t += dur;
    }
    if (!lastPlaced.isEmpty()) setSelection(lastPlaced);
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::cutAtPlayhead() {
    if (!m_project) return;
    emit editStart();
    // Comportamento clássico (Vegas): "S" divide TODOS os clipes (vídeo e
    // áudio) que cruzam o playhead — independentemente da seleção. Assim o
    // corte sempre funciona: basta posicionar o playhead sobre o clipe.
    // Coleta um representante por grupo ANTES de dividir: a divisão troca os
    // groupIds, então deduplicar durante o processo faria um mesmo grupo ser
    // reprocessado (e desagrupado).
    QHash<QString, QString> units; // unidade -> id de um representante
    auto consider = [&](const Clip& c) {
        if (trackLocked(&c)) return;
        if (m_playhead > c.pos + 1e-6 && m_playhead < c.pos + c.dur - 1e-6) {
            const QString key = c.groupId.isEmpty() ? c.id : c.groupId;
            if (!units.contains(key)) units.insert(key, c.id);
        }
    };
    for (const Track& t : m_project->videoTracks)
        for (const Clip& c : t.clips) consider(c);
    for (const Track& t : m_project->audioTracks)
        for (const Clip& c : t.clips) consider(c);
    for (const QString& rep : units.values()) {
        Clip* cc = findClipById(rep);
        if (cc) splitClipAt(cc, m_playhead);
    }
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::splitClipAt(Clip* c, double t) {
    if (!c || !m_project) return;
    // Usa ids (não ponteiros): inserir clipes pode realocar o vetor e
    // invalidar ponteiros coletados antes do split.
    QStringList ids;
    if (!c->groupId.isEmpty()) {
        for (Clip* m : groupMembers(c->groupId)) ids.append(m->id);
    } else {
        ids.append(c->id);
    }

    // Precisão do corte: além da grade do projeto (snapTime), desloca o corte
    // para a grade de frames da MÍDIA de vídeo (fps do arquivo). Assim a borda
    // cai no início exato de um frame de origem e o clipe 2 começa no frame
    // seguinte — sem repetir o último frame do clipe 1 (o "pedaço do clipe que
    // fica" / frame duplicado no corte). Usa o primeiro membro do grupo com
    // vídeo como referência e aplica o MESMO t' a todos os membros, mantendo
    // vídeo+áudio sincronizados.
    double tCut = t;
    for (const QString& id : ids) {
        Clip* ref = findClipById(id);
        if (!ref) continue;
        const MediaItem* m = m_project->findMedia(ref->mediaId);
        if (!m || !m->hasVideo) continue;
        const double fps = sourceFps(m->filePath);
        if (fps > 0.0) {
            const double t0 = ref->in + (t - ref->pos);       // instante na mídia
            const double t0s = std::round(t0 * fps) / fps;    // na grade do arquivo
            tCut = std::clamp(ref->pos + (t0s - ref->in),
                              ref->pos + 1e-6, ref->pos + ref->dur - 1e-6);
        }
        break; // uma única referência (vídeo) define o corte do grupo
    }
    t = tCut;

    const bool grouped = ids.size() > 1;
    // Cada metade vira um grupo próprio (vídeo+áudio da frente ficam
    // juntos, e os de trás juntos), para que excluir um lado não apague o outro.
    const QString fg = grouped ? newId() : QString();
    const QString bg = grouped ? newId() : QString();
    for (const QString& id : ids) {
        Clip* cc = findClipById(id);
        if (!cc) continue;
        if (t <= cc->pos + 1e-6 || t >= cc->pos + cc->dur - 1e-6) {
            // Membro do grupo que não cruza o playhead: vira grupo próprio,
            // para que excluir uma das metades não arraste o clipe inteiro.
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
            tr->clips.push_back(b);
            if (id == c->id) firstDup = b.id;
        }
    }
    if (!firstDup.isEmpty()) setSelection(firstDup);
    updateScrollRanges();
    update();
    emit modified();
}

// Diálogo de velocidade: digita o fator (0.1x-4x). Aplica ao clipe e, se
// vinculado, aos membros do grupo (vídeo + faixas de áudio) mantendo a posição
// da borda DIREITA (a duração do clipe é retida no timeline).
void TimelineWidget::showSpeedDialog(Clip* c) {
    if (!c) return;
    QStringList ids = c->groupId.isEmpty() ? QStringList{c->id} : QStringList();
    if (ids.isEmpty())
        for (Clip* m : groupMembers(c->groupId)) ids.append(m->id);

    bool ok = false;
    const double v = QInputDialog::getDouble(
        this, tr("Velocidade do clipe"),
        tr("Velocidade: (0,1×–4×)\n\nO vídeo fica mais rápido (valor > 1) ou "
           "mais lento (< 1). A duração na timeline é preservada; o conteúdo "
           "da mídia que sobra é descartado ou repetido conforme a direção."),
        c->speed, 0.1, 4.0, 1, &ok);
    if (!ok) return;

    emit editStart();
    for (const QString& id : ids) {
        Clip* sc = findClipById(id);
        if (!sc) continue;
        sc->speed = v;
        // Mantém a borda DIREITA: duração no timeline preservada, mas o ponto
        // de entrada na mídia e o quanto é consumido mudam conforme a
        // velocidade, como no Vegas. `in` é ajustado para a nova velocidade.
        const double consumed = c->dur * v; // segundos de mídia consumidos
        sc->in = std::max(0.0, sc->in - (consumed - c->dur));
    }
    invalidateScene();
    update();
    emit modified();
}

void TimelineWidget::showProperties(Clip* c) {    if (!c) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Propriedades do clipe"));

    auto* vol = new QSlider(Qt::Horizontal, &dlg);
    vol->setRange(0, 200);
    vol->setValue((int)llround(c->volume * 100.0));
    auto* volLabel = new QLabel(QString("%1%").arg(vol->value()), &dlg);
    connect(vol, &QSlider::valueChanged, &dlg, [volLabel](int v) {
        volLabel->setText(QString("%1%").arg(v));
    });

    auto* op = new QSlider(Qt::Horizontal, &dlg);
    op->setRange(0, 100);
    op->setValue((int)llround(c->opacity * 100.0));
    auto* opLabel = new QLabel(QString("%1%").arg(op->value()), &dlg);
    connect(op, &QSlider::valueChanged, &dlg, [opLabel](int v) {
        opLabel->setText(QString("%1%").arg(v));
    });

    auto* volRow = new QHBoxLayout;
    volRow->addWidget(vol, 1);
    volRow->addWidget(volLabel);
    auto* opRow = new QHBoxLayout;
    opRow->addWidget(op, 1);
    opRow->addWidget(opLabel);

    auto* speed = new QSlider(Qt::Horizontal, &dlg);
    speed->setRange(10, 400);
    speed->setValue((int)llround(c->speed * 100.0));
    auto* speedLabel = new QLabel(QString("%1×").arg(speed->value() / 100.0), &dlg);
    connect(speed, &QSlider::valueChanged, &dlg, [speedLabel](int v) {
        speedLabel->setText(QString("%1×").arg(v / 100.0));
    });
    auto* speedRow = new QHBoxLayout;
    speedRow->addWidget(speed, 1);
    speedRow->addWidget(speedLabel);

    auto* fIn = new QSlider(Qt::Horizontal, &dlg);
    fIn->setRange(0, 5000);
    fIn->setValue((int)llround(c->fadeIn * 1000.0));
    auto* fInLabel = new QLabel(QString("%1 s").arg(fIn->value() / 1000.0), &dlg);
    connect(fIn, &QSlider::valueChanged, &dlg, [fInLabel](int v) {
        fInLabel->setText(QString("%1 s").arg(v / 1000.0));
    });
    auto* fInRow = new QHBoxLayout;
    fInRow->addWidget(fIn, 1);
    fInRow->addWidget(fInLabel);

    auto* fOut = new QSlider(Qt::Horizontal, &dlg);
    fOut->setRange(0, 5000);
    fOut->setValue((int)llround(c->fadeOut * 1000.0));
    auto* fOutLabel = new QLabel(QString("%1 s").arg(fOut->value() / 1000.0), &dlg);
    connect(fOut, &QSlider::valueChanged, &dlg, [fOutLabel](int v) {
        fOutLabel->setText(QString("%1 s").arg(v / 1000.0));
    });
    auto* fOutRow = new QHBoxLayout;
    fOutRow->addWidget(fOut, 1);
    fOutRow->addWidget(fOutLabel);

    const bool isAudio = trackOf(c)->audio;

    auto* form = new QFormLayout;
    form->addRow(tr("Volume:"), volRow);
    form->addRow(tr("Opacidade:"), opRow);
    form->addRow(tr("Velocidade:"), speedRow);
    form->addRow(tr("Fade in:"), fInRow);
    form->addRow(tr("Fade out:"), fOutRow);
    if (!isAudio) {
        // Texto/título tem editor dedicado (menu de contexto "Texto…").
        auto* textBtn = new QPushButton(tr("Texto do clipe…"), &dlg);
        connect(textBtn, &QPushButton::clicked, &dlg, [this, c, &dlg]() {
            TextEditorDialog td(m_project, c, &dlg);
            if (td.exec() == QDialog::Accepted) {
                emit editStart();
                emit modified();
            }
        });
        form->addRow(tr("Texto:"), textBtn);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;

    emit editStart();
    c->volume = vol->value() / 100.0;
    c->opacity = op->value() / 100.0;
    c->speed = speed->value() / 100.0;
    c->fadeIn = fIn->value() / 1000.0;
    c->fadeOut = fOut->value() / 1000.0;
    update();
    emit modified();
}

void TimelineWidget::showEffectsDialog(Clip* c) {
    if (!c) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Efeitos de vídeo"));

    auto* bright = new QSlider(Qt::Horizontal, &dlg);
    bright->setRange(-100, 100);
    bright->setValue((int)llround(c->brightness * 100.0));
    auto* brightLabel = new QLabel(QString("%1").arg(bright->value()), &dlg);
    connect(bright, &QSlider::valueChanged, &dlg, [brightLabel](int v) {
        brightLabel->setText(QString("%1").arg(v));
    });
    auto* brightRow = new QHBoxLayout;
    brightRow->addWidget(bright, 1);
    brightRow->addWidget(brightLabel);

    auto* cont = new QSlider(Qt::Horizontal, &dlg);
    cont->setRange(0, 200);
    cont->setValue((int)llround(c->contrast * 100.0));
    auto* contLabel = new QLabel(QString("%1%").arg(cont->value()), &dlg);
    connect(cont, &QSlider::valueChanged, &dlg, [contLabel](int v) {
        contLabel->setText(QString("%1%").arg(v));
    });
    auto* contRow = new QHBoxLayout;
    contRow->addWidget(cont, 1);
    contRow->addWidget(contLabel);

    auto* sat = new QSlider(Qt::Horizontal, &dlg);
    sat->setRange(0, 200);
    sat->setValue((int)llround(c->saturation * 100.0));
    auto* satLabel = new QLabel(QString("%1%").arg(sat->value()), &dlg);
    connect(sat, &QSlider::valueChanged, &dlg, [satLabel](int v) {
        satLabel->setText(QString("%1%").arg(v));
    });
    auto* satRow = new QHBoxLayout;
    satRow->addWidget(sat, 1);
    satRow->addWidget(satLabel);

    auto* blur = new QSlider(Qt::Horizontal, &dlg);
    blur->setRange(0, 40);
    blur->setValue((int)llround(c->blur));
    auto* blurLabel = new QLabel(QString("%1").arg(blur->value()), &dlg);
    connect(blur, &QSlider::valueChanged, &dlg, [blurLabel](int v) {
        blurLabel->setText(QString("%1").arg(v));
    });
    auto* blurRow = new QHBoxLayout;
    blurRow->addWidget(blur, 1);
    blurRow->addWidget(blurLabel);

    auto* gray = new QCheckBox(tr("Preto e branco"), &dlg);
    gray->setChecked(c->grayscale);

    auto* ck = new QCheckBox(tr("Chroma key"), &dlg);
    ck->setChecked(c->chromaKey);
    auto* ckColor = new QPushButton(tr("Cor…"), &dlg);
    ckColor->setEnabled(c->chromaKey);
    connect(ck, &QCheckBox::toggled, ckColor, &QPushButton::setEnabled);
    QColor ckSelected = c->chromaKeyColor;
    connect(ckColor, &QPushButton::clicked, &dlg, [&dlg, &ckSelected]() {
        const QColor col = QColorDialog::getColor(ckSelected, &dlg, tr("Cor do chroma key"));
        if (col.isValid()) ckSelected = col;
    });
    auto* ckSim = new QSlider(Qt::Horizontal, &dlg);
    ckSim->setRange(0, 100);
    ckSim->setValue((int)llround(c->chromaKeySimilarity * 100.0));
    ckSim->setEnabled(c->chromaKey);
    connect(ck, &QCheckBox::toggled, ckSim, &QSlider::setEnabled);
    auto* ckSimLabel = new QLabel(QString("%1").arg(ckSim->value()), &dlg);
    connect(ckSim, &QSlider::valueChanged, &dlg, [ckSimLabel](int v) {
        ckSimLabel->setText(QString("%1").arg(v));
    });
    auto* ckSimRow = new QHBoxLayout;
    ckSimRow->addWidget(ckSim, 1);
    ckSimRow->addWidget(ckSimLabel);

    auto* form = new QFormLayout;
    form->addRow(tr("Brilho:"), brightRow);
    form->addRow(tr("Contraste:"), contRow);
    form->addRow(tr("Saturação:"), satRow);
    form->addRow(tr("Desfoque:"), blurRow);
    form->addRow(gray);
    form->addRow(ck, ckColor);
    form->addRow(tr("Similaridade:"), ckSimRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* lay = new QVBoxLayout(&dlg);
    lay->addLayout(form);
    lay->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;

    emit editStart();
    c->brightness = bright->value() / 100.0;
    c->contrast = cont->value() / 100.0;
    c->saturation = sat->value() / 100.0;
    c->blur = blur->value();
    c->grayscale = gray->isChecked();
    c->chromaKey = ck->isChecked();
    c->chromaKeyColor = ckSelected;
    c->chromaKeySimilarity = ckSim->value() / 100.0;
    update();
    emit modified();
}

void TimelineWidget::showTransformDialog(Clip* c) {
    if (!c) return;
    TransformDialog dlg(c, this);
    if (dlg.exec() != QDialog::Accepted) return;
    emit editStart();
    update();
    emit modified();
}

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
    // Ripple: ao excluir, os clipes seguintes deslizam e fecham o espaço.
    if (!m_project || m_selected.isEmpty()) return;
    emit editStart();
    QStringList sel;
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* c = findClipById(id);
        if (c && !trackLocked(c)) sel.append(id);
    }
    if (sel.isEmpty()) return;
    for (Track& t : m_project->videoTracks)
        rippleDeleteInTrack(t, sel);
    for (Track& t : m_project->audioTracks)
        rippleDeleteInTrack(t, sel);
    m_selected.clear();
    invalidateScene();
    updateScrollRanges();
    emit modified();
}

void TimelineWidget::deleteSelectedLeaveGap() {
    if (!m_project || m_selected.isEmpty()) return;
    emit editStart();
    QStringList sel;
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* c = findClipById(id);
        if (c && !trackLocked(c)) sel.append(id);
    }
    if (sel.isEmpty()) return;
    removeClipsByIds(sel);
    m_selected.clear();
    updateScrollRanges();
    update();
    emit modified();
}

// Delete: apaga somente os clipes selecionados. A seleção de faixa é apenas
// informativa (mostra em qual faixa você está) e NÃO apaga faixa nenhuma.
void TimelineWidget::deleteSelection() {
    deleteSelected();
}

void TimelineWidget::deleteClipBeforePlayhead() {
    if (!m_project) return;
    const QStringList scope = m_selected.isEmpty() ? [this]() {
        QStringList all;
        for (const Track& t : m_project->videoTracks)
            for (const Clip& c : t.clips) all.append(c.id);
        for (const Track& t : m_project->audioTracks)
            for (const Clip& c : t.clips) all.append(c.id);
        return all;
    }() : m_selected;
    QStringList victims;
    double bestEnd = -1.0;
    for (const QString& id : scope) {
        Clip* c = findClipById(id);
        if (!c) continue;
        if (c->pos + c->dur <= m_playhead + 1e-6) {
            const double end = c->pos + c->dur;
            if (end > bestEnd + 1e-6) {
                bestEnd = end;
                victims = {id};
            } else if (std::fabs(end - bestEnd) < 1e-6) {
                victims.append(id);
            }
        }
    }
    if (victims.isEmpty()) return;
    emit editStart();
    QStringList sel;
    for (const QString& id : expandToGroups(victims)) {
        Clip* c = findClipById(id);
        if (c && !trackLocked(c)) sel.append(id);
    }
    removeClipsByIds(sel);
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::deleteClipAfterPlayhead() {
    if (!m_project) return;
    const QStringList scope = m_selected.isEmpty() ? [this]() {
        QStringList all;
        for (const Track& t : m_project->videoTracks)
            for (const Clip& c : t.clips) all.append(c.id);
        for (const Track& t : m_project->audioTracks)
            for (const Clip& c : t.clips) all.append(c.id);
        return all;
    }() : m_selected;
    QStringList victims;
    double bestPos = 1e18;
    for (const QString& id : scope) {
        Clip* c = findClipById(id);
        if (!c) continue;
        if (c->pos >= m_playhead - 1e-6) {
            if (c->pos < bestPos - 1e-6) {
                bestPos = c->pos;
                victims = {id};
            } else if (std::fabs(c->pos - bestPos) < 1e-6) {
                victims.append(id);
            }
        }
    }
    if (victims.isEmpty()) return;
    emit editStart();
    QStringList sel;
    for (const QString& id : expandToGroups(victims)) {
        Clip* c = findClipById(id);
        if (c && !trackLocked(c)) sel.append(id);
    }
    removeClipsByIds(sel);
    updateScrollRanges();
    update();
    emit modified();
}

bool TimelineWidget::isSelected(const QString& id) const {
    return m_selected.contains(id);
}

void TimelineWidget::setSelection(const QString& id) {
    clearTrackSelection();
    m_selected.clear();
    m_selected.append(id);
    refreshView();
    emit selectionChanged(id);
}

void TimelineWidget::toggleSelection(const QString& id) {
    clearTrackSelection();
    const int i = m_selected.indexOf(id);
    if (i >= 0) m_selected.removeAt(i);
    else m_selected.append(id);
    refreshView();
    emit selectionChanged(m_selected.isEmpty() ? QString() : m_selected.last());
}

bool TimelineWidget::isTrackSelected(int row, bool audio) const {
    return m_selTracks.contains(TrackSel{row, audio});
}

void TimelineWidget::setTrackSel(int row, bool audio) {
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        emit selectionChanged(QString());
    }
    m_selTracks.clear();
    m_selTracks.append(TrackSel{row, audio});
    m_selAnchor = TrackSel{row, audio};
    m_hasAnchor = true;
}

void TimelineWidget::toggleTrackSel(int row, bool audio) {
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        emit selectionChanged(QString());
    }
    const TrackSel ts{row, audio};
    const int i = m_selTracks.indexOf(ts);
    if (i >= 0) m_selTracks.removeAt(i);
    else m_selTracks.append(ts);
    m_selAnchor = ts;
    m_hasAnchor = true;
}

void TimelineWidget::selectTrackRange(int row, bool audio) {
    if (!m_hasAnchor || m_selAnchor.audio != audio) {
        setTrackSel(row, audio);
        return;
    }
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        emit selectionChanged(QString());
    }
    // Faixas de vídeo e áudio exibidas na ordem dos índices (V1 no topo);
    // o intervalo segue a ordem que o usuário vê na tela.
    int a = m_selAnchor.row;
    int b = row;
    if (a > b) std::swap(a, b);
    m_selTracks.clear();
    for (int k = a; k <= b; ++k)
        m_selTracks.append(TrackSel{k, audio});
}

void TimelineWidget::selectTrackRightClick(int row, bool audio) {
    if (isTrackSelected(row, audio)) return;
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        emit selectionChanged(QString());
    }
    m_selTracks.clear();
    m_selTracks.append(TrackSel{row, audio});
    m_selAnchor = TrackSel{row, audio};
    m_hasAnchor = true;
}

void TimelineWidget::clearTrackSelection() {
    m_selTracks.clear();
    m_hasAnchor = false;
}

// Nº de pastas de vídeo cujo membro mais alto (menor índice) está em um
// índice <= videoIdx. Com V1 no topo, cada pasta ocupa kFolderH acima do
// primeiro membro (o de menor índice). videoIdx < 0 conta todas.
int TimelineWidget::folderStripsAboveVideo(int videoIdx) const {
    int count = 0;
    for (const TrackGroup& g : m_project->trackGroups) {
        int first = -1;
        for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
            if (m_project->videoTracks[i].groupId == g.id) { first = i; break; }
        if (first < 0) continue;
        if (videoIdx < 0 || first <= videoIdx) ++count;
    }
    return count;
}

// Nº de pastas (só de áudio) cujo membro mais alto (menor índice) está em um
// índice <= audioIdx. Pastas com vídeo têm a faixa na seção de vídeo.
int TimelineWidget::folderStripsAboveAudio(int audioIdx) const {
    int count = 0;
    for (const TrackGroup& g : m_project->trackGroups) {
        bool hasVideo = false;
        for (const Track& t : m_project->videoTracks)
            if (t.groupId == g.id) { hasVideo = true; break; }
        if (hasVideo) continue;
        int top = -1;
        for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
            if (m_project->audioTracks[i].groupId == g.id) { top = i; break; }
        if (top >= 0 && top <= audioIdx) ++count;
    }
    return count;
}

QRect TimelineWidget::folderStripRect(const TrackGroup& g) const {
    if (!m_project) return QRect();
    int topVideo = -1;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (m_project->videoTracks[i].groupId == g.id) { topVideo = i; break; }
    if (topVideo >= 0) {
        const int y = rowY(topVideo, -1) - kFolderH;
        return QRect(0, y, width(), kFolderH);
    }
    int topAudio = -1;
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        if (m_project->audioTracks[i].groupId == g.id) { topAudio = i; break; }
    if (topAudio >= 0) {
        const int y = rowY(-1, topAudio) - kFolderH;
        return QRect(0, y, width(), kFolderH);
    }
    return QRect();
}

bool TimelineWidget::folderStripAt(int y, QString& gid) const {
    if (!m_project) return false;
    for (const TrackGroup& g : m_project->trackGroups) {
        const QRect r = folderStripRect(g);
        if (y >= r.top() && y < r.bottom()) { gid = g.id; return true; }
    }
    return false;
}

QRect TimelineWidget::folderArrowRect(const TrackGroup& g) const {
    const QRect r = folderStripRect(g);
    if (r.isEmpty()) return QRect();
    return QRect(r.left() + 2, r.top() + (kFolderH - 14) / 2, 18, 14);
}

// Uma faixa fica visível se não pertencer a uma pasta recolhida.
bool TimelineWidget::trackVisible(int row, bool audio) const {
    if (!m_project) return true;
    const QVector<Track>& list = audio ? m_project->audioTracks : m_project->videoTracks;
    if (row < 0 || row >= (int)list.size()) return true;
    const QString& gid = list[row].groupId;
    if (gid.isEmpty()) return true;
    const TrackGroup* g = m_project->findGroup(gid);
    return g ? !g->collapsed : true;
}

void TimelineWidget::toggleGroupCollapsed(const QString& gid) {
    TrackGroup* g = m_project ? m_project->findGroup(gid) : nullptr;
    if (!g) return;
    emit editStart();
    g->collapsed = !g->collapsed;
    if (!m_selected.isEmpty()) {
        m_selected.clear();
        emit selectionChanged(QString());
    }
    clearTrackSelection();
    updateScrollRanges();
    invalidateScene();
    emit modified();
}

void TimelineWidget::drawFolderStrip(QPainter& p, const TrackGroup& g) {
    const QRect r = folderStripRect(g);
    if (r.isEmpty()) return;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(150, 118, 60));
    p.drawRect(r);
    p.setPen(QColor(120, 92, 46));
    p.drawLine(r.left(), r.bottom(), r.right(), r.bottom());
    // Seta de recolher/expandir (▾ aberta, ▸ fechada).
    const QRect ar = folderArrowRect(g);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(QColor(235, 220, 180));
    p.setPen(Qt::NoPen);
    QPolygon tri;
    const int cx = ar.center().x();
    const int cy = ar.center().y();
    if (g.collapsed)
        tri << QPoint(cx - 4, cy - 5) << QPoint(cx + 3, cy) << QPoint(cx - 4, cy + 5);
    else
        tri << QPoint(cx - 5, cy - 4) << QPoint(cx + 5, cy - 4) << QPoint(cx, cy + 4);
    p.drawPolygon(tri);
    p.setRenderHint(QPainter::Antialiasing, false);
    // Ícone de pasta.
    p.setBrush(QColor(224, 192, 112));
    p.drawRect(QRect(r.left() + 24, r.top() + 5, 11, 5));
    p.drawRect(QRect(r.left() + 22, r.top() + 8, 17, 12));
    // Nome da pasta.
    p.setPen(QColor(245, 235, 210));
    QFont f = p.font();
    f.setBold(true);
    f.setPointSizeF(8.5);
    p.setFont(f);
    p.drawText(QRect(r.left() + 46, r.top(), r.width() - 54, kFolderH),
               Qt::AlignLeft | Qt::AlignVCenter, g.name);
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
        emit selectionChanged(QString());
    }
    m_hasAnchor = false;
}

// Ctrl+clique na pasta: alterna as faixas dela dentro da seleção atual, para
// acumular pastas sem perder o que já estava selecionado.
void TimelineWidget::toggleGroupTracks(const QString& gid) {
    if (!m_selected.isEmpty()) {
        m_selected.clear();
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

// Finaliza o arrasto de faixa: reordena a faixa/grupo ou a coloca numa pasta.
void TimelineWidget::finishTrackDrag() {
    if (!m_project) return;
    // Arrasto de GRUPO (pela faixa de pasta): move todas as faixas do grupo.
    if (!m_dragGroupId.isEmpty()) {
        TrackGroup* g = m_project->findGroup(m_dragGroupId);
        if (g && m_dropRow >= 0) {
            QVector<int> vidx, aidx;
            for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
                if (m_project->videoTracks[i].groupId == g->id) vidx.append(i);
            for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
                if (m_project->audioTracks[i].groupId == g->id) aidx.append(i);
            emit editStart();
            if (m_dropAudio) moveTracksTo(true, aidx, m_dropRow);
            else moveTracksTo(false, vidx, m_dropRow);
            pruneEmptyGroups();
            updateScrollRanges();
            invalidateScene();
            emit modified();
        }
        m_dragGroupId.clear();
        m_dragTrackRow = -1;
        m_dropRow = -1;
        m_dropGroup.clear();
        return;
    }

    if (m_dragTrackRow < 0) return;
    const int from = m_dragTrackRow;
    const bool audio = m_dragTrackAudio;
    if (!m_dropGroup.isEmpty()) {
        // Soltou sobre uma pasta: entra no grupo.
        if (m_project->findGroup(m_dropGroup)) {
            emit editStart();
            auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
            if (from >= 0 && from < (int)list.size())
                list[from].groupId = m_dropGroup;
            pruneEmptyGroups();
            updateScrollRanges();
            invalidateScene();
            emit modified();
        }
        m_dropGroup.clear();
        m_dragTrackRow = -1;
        return;
    }
    // Soltou sobre uma faixa do mesmo tipo: reordena. Uma faixa de grupo, ao
    // ser movida para outro lugar, sai do grupo (fica por cima/abaixo da
    // hierarquia de pastas).
    if (m_dropRow >= 0 && m_dropAudio == audio && m_dropRow != from) {
        emit editStart();
        auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
        if (from >= 0 && from < (int)list.size())
            list[from].groupId.clear();
        moveTrack(audio, from, m_dropRow);
        pruneEmptyGroups();
        updateScrollRanges();
        invalidateScene();
        emit modified();
    }
    m_dragTrackRow = -1;
}

double TimelineWidget::snapToEdges(double t, const QString& excludeId) const {
    if (!m_snap) return t;
    const double thresh = 8.0 / m_pps;
    double best = t;
    double bestD = thresh;
    auto consider = [&](double x) {
        const double d = std::fabs(x - t);
        if (d < bestD) { bestD = d; best = x; }
    };
    consider(0.0);
    consider(m_playhead);
    auto considerClip = [&](const Clip& c) {
        if (c.id == excludeId) return;
        consider(c.pos);
        consider(c.pos + c.dur);
    };
    if (m_project) {
        for (const Track& tr : m_project->videoTracks)
            for (const Clip& c : tr.clips)
                considerClip(c);
        for (const Track& tr : m_project->audioTracks)
            for (const Clip& c : tr.clips)
                considerClip(c);
    }
    return best;
}

double TimelineWidget::clampPosToTrack(Clip* c, double newPos,
                                       const QSet<QString>& moving) const {
    if (!m_project || !c) return newPos;
    Q_UNUSED(moving)
    // Clipes podem se sobrepor na mesma faixa: a sobreposição vira uma
    // transição (estilo Vegas). Só garante que não vá para o negativo.
    return std::max(0.0, newPos);
}

bool TimelineWidget::clipTrackIndex(const QString& id, int& row, bool& audio) const {
    if (!m_project) return false;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        for (const Clip& c : m_project->videoTracks[i].clips)
            if (c.id == id) { row = i; audio = false; return true; }
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        for (const Clip& c : m_project->audioTracks[i].clips)
            if (c.id == id) { row = i; audio = true; return true; }
    return false;
}

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
    return true;
}

void TimelineWidget::showAudioEffectsDialog(Clip* c) {
    if (!c) return;
    AudioEffectsDialog dlg(c, this);
    if (dlg.exec() != QDialog::Accepted) return;
    emit editStart();
    update();
    emit modified();
}

void TimelineWidget::showTextEditorDialog(Clip* c) {
    if (!c) return;
    TextEditorDialog dlg(m_project, c, this);
    if (dlg.exec() != QDialog::Accepted) return;
    emit editStart();
    invalidateScene();
    update();
    emit modified();
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
