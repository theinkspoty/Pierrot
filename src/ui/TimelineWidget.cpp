// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TimelineWidget.h"
#include "TimelineCommands.h"
#include "util.h"
#include "clipattrs.h"

#include "ffmpeg/MediaCache.h"
#include "ffmpeg/FFmpegDecoder.h"
#include "ui/TransformDialog.h"
#include "ui/AudioEffectsDialog.h"
#include "ui/TrackAudioFxDialog.h"
#include "ui/TextEditorDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/TlLog.h"

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
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QInputDialog>
#include <QSettings>
#include <QMessageBox>
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
constexpr int kMinDragH = 40; // piso ao arrastar a borda (não deixa a faixa minúscula)
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
    ToolSelect = 0, ToolMove = 1, ToolScissors = 2, ToolEnvelope = 3, ToolZoom = 4,
    ToolRipple = 5, ToolRolling = 6, ToolSlip = 7, ToolSlide = 8, ToolRateStretch = 9
};



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
    rebuildClipIndex();
    update();
}

void TimelineWidget::refreshView() {
    m_staticDirty = true;
    update();
}

void TimelineWidget::setProject(Project* p) {
    m_project = p;
    m_selected.clear();
    m_secondarySelected.clear();
    clearTrackSelection();
    m_clipPix.clear();
    m_clipBytes = 0;
    m_cursorT = -1.0;
    m_lastMax = -1;
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

void TimelineWidget::criarMesa() {
    if (!m_project) return;

    emit editStart();

    // Cria a composição 2D
    MesaComposition mc;
    mc.id = newId();
    mc.name = tr("Mesa %1").arg(m_project->mesas.size() + 1);
    mc.canvasW = m_project->width;
    mc.canvasH = m_project->height;

    // Cria uma nova track para a Mesa
    m_project->addTrack(false);
    Track& newTrack = m_project->videoTracks.last();
    newTrack.name = mc.name;
    // Posição padrão no centro da composição (estilo AE: pos da âncora).
    newTrack.mesaX = mc.canvasW / 2.0;
    newTrack.mesaY = mc.canvasH / 2.0;

    // Cria o TrackGroup (pasta) vinculado à Mesa
    TrackGroup grp;
    grp.id = mc.id;
    grp.name = mc.name;
    grp.mesaId = mc.id;
    m_project->trackGroups.append(grp);
    newTrack.groupId = grp.id;

    mc.trackIds.append(newTrack.id);
    // Câmera padrão centrada na composição (mesma convenção das layers).
    mc.camX = mc.canvasW / 2.0;
    mc.camY = mc.canvasH / 2.0;
    m_project->mesas.append(mc);

    emit modified();
    emit mesaOpenRequested(mc.id);
    invalidateScene();
    update();
}

void TimelineWidget::addTrackToMesa(const QString& mesaId) {
    if (!m_project || mesaId.isEmpty()) return;
    MesaComposition* mc = m_project->findMesa(mesaId);
    if (!mc) return;

    emit editStart();

    // Cria nova track de vídeo
    m_project->addTrack(false);
    Track& newTrack = m_project->videoTracks.last();
    newTrack.name = tr("Mesa %1 · Track %2").arg(mc->name).arg(mc->trackIds.size() + 1);

    // Vincula à mesma pasta/grupo da Mesa
    TrackGroup* grp = m_project->findGroup(mesaId);
    if (grp) newTrack.groupId = grp->id;

    // Adiciona à lista de tracks da Mesa
    mc->trackIds.append(newTrack.id);
    // Posição padrão no centro da composição (estilo AE).
    newTrack.mesaX = mc->canvasW / 2.0;
    newTrack.mesaY = mc->canvasH / 2.0;

    emit modified();
    invalidateScene();
    update();
}

void TimelineWidget::sendTrackToMesa(const QString& trackId, const QString& mesaId) {
    if (!m_project || mesaId.isEmpty()) return;
    MesaComposition* mc = m_project->findMesa(mesaId);
    if (!mc) return;

    // Só faixas de vídeo viram camadas de Mesa.
    int trackIdx = -1;
    for (int i = 0; i < m_project->videoTracks.size(); ++i)
        if (m_project->videoTracks[i].id == trackId) { trackIdx = i; break; }
    if (trackIdx < 0) return;

    Track& t = m_project->videoTracks[trackIdx];

    // Já é membro desta Mesa: nada a fazer.
    if (mc->trackIds.contains(t.id)) return;

    emit editStart();

    // Exclusividade: a track sai de qualquer outra Mesa.
    for (MesaComposition& m : m_project->mesas)
        if (m.trackIds.contains(t.id))
            m.trackIds.removeAll(t.id);

    // Entra na composição alvo e no grupo (pasta) da Mesa.
    mc->trackIds.append(t.id);
    TrackGroup* grp = m_project->findGroup(mesaId);
    t.groupId = grp ? grp->id : t.groupId;

    emit modified();
    emit mesaChanged(mesaId);
    invalidateScene();
    updateScrollRanges();
    update();
}

void TimelineWidget::addSolidToMesa(const QString& mesaId, const QString& generator,
                                    const QColor& c1, const QColor& c2) {
    if (!m_project || mesaId.isEmpty()) return;
    MesaComposition* mc = m_project->findMesa(mesaId);
    if (!mc) return;

    emit editStart();

    // Mídia virtual gerada sob demanda (mesmo formato da pool de mídia).
    MediaItem m;
    m.id = newId();
    m.isSolid = true;
    m.hasVideo = true;
    m.width = m_project->width;
    m.height = m_project->height;
    m.duration = m_project->duration() > 0 ? m_project->duration() : 5.0;
    m.generator = generator;
    m.solidColor = c1;
    m.solidColor2 = c2;
    if (generator.isEmpty()) {
        const QString hex = m.solidColor.name();
        m.name = tr("Cor %1").arg(hex.startsWith(QLatin1Char('#')) ? hex.mid(1) : hex);
    } else if (generator == QStringLiteral("gradient")) {
        m.name = tr("Gradiente");
    } else {
        m.name = tr("Camada %1").arg(generator);
    }
    m_project->media.append(m);

    // Nova track de vídeo vinculada ao grupo (pasta) da Mesa.
    m_project->addTrack(false);
    Track& nt = m_project->videoTracks.last();
    nt.name = tr("Mesa %1 · %2").arg(mc->name).arg(m.name);
    TrackGroup* grp = m_project->findGroup(mesaId);
    if (grp) nt.groupId = grp->id;
    mc->trackIds.append(nt.id);
    nt.mesaX = mc->canvasW / 2.0;
    nt.mesaY = mc->canvasH / 2.0;

    // Clipe cobrindo toda a duração da mídia virtual.
    Clip c;
    c.id = newId();
    c.mediaId = m.id;
    c.pos = 0.0;
    c.in = 0.0;
    c.dur = m.duration;
    c.speed = 1.0;
    c.name = m.name;
    nt.clips.append(c);

    emit modified();
    emit mesaChanged(mesaId);
    invalidateScene();
    updateScrollRanges();
    update();
}

void TimelineWidget::duplicateMesaTrack(const QString& mesaId, const QString& trackId) {
    if (!m_project || mesaId.isEmpty()) return;
    MesaComposition* mc = m_project->findMesa(mesaId);
    if (!mc) return;

    int srcIdx = -1;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (m_project->videoTracks[i].id == trackId) { srcIdx = i; break; }
    if (srcIdx < 0) return;

    emit editStart();

    // Cópia profunda: ids novos em clips (as mídias são compartilhadas,
    // como todo clipe duplicado do projeto).
    Track copy = m_project->videoTracks[srcIdx];
    copy.id = newId();
    copy.name = tr("%1 (cópia)").arg(copy.name);
    for (int i = 0; i < copy.clips.size(); ++i) {
        copy.clips[i].id = newId();
        copy.clips[i].groupId = QString();  // grupos entra-e-sai não valem para a cópia
    }

    // Deslocada +20/+20 no canvas (base e keyframes de posição), para sair da
    // sombra da original e fácil de pegar.
    copy.mesaX += 20.0;
    copy.mesaY += 20.0;
    for (Keyframe& k : copy.kfMesaX) k.value += 20.0;
    for (Keyframe& k : copy.kfMesaY) k.value += 20.0;

    m_project->videoTracks.append(copy);
    mc->trackIds.append(copy.id);

    emit modified();
    emit mesaChanged(mesaId);
    invalidateScene();
    updateScrollRanges();
    update();
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
    if (it != m_clipIndex.end())
        return *it;
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

void TimelineWidget::ensurePlayheadVisible() {
    if (!m_project) return;
    const double px = timeToX(m_playhead);
    const int viewW = width() - m_vbar->sizeHint().width();
    // Follow contínuo (estilo Vegas): durante a reprodução a rolagem acompanha
    // a agulha mantendo-a perto de 2/3 da largura, em vez de esperar ela sair
    // da tela e "pular". Assim a view avança suavemente a cada frame.
    const double rightEdge = kHeaderW + (viewW - kHeaderW) * (2.0 / 3.0);
    if (px < kHeaderW || px > rightEdge) {
        // Fração da área útil onde a agulha deve pousar depois de rolar
        // (20% quando ela vinha pela esquerda, 2/3 quando vem pela direita).
        const double frac = px < kHeaderW ? 0.2 : (2.0 / 3.0);
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
    emit undoLabel(tr("Recortar"));
    copySelected();
    deleteSelected();
}

void TimelineWidget::pasteClips() {
    if (m_clipboard.isEmpty() || !m_project) return;
    emit undoLabel(tr("Colar"));
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

void TimelineWidget::saveClipPreset() {
    if (m_selected.isEmpty() || !m_project) return;
    // Captura os atributos a partir do primeiro clipe selecionado.
    Clip* src = findClipById(expandToGroups(m_selected).first());
    if (!src || trackLocked(src)) return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Salvar Preset"),
                                               tr("Nome do preset:"), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    const QJsonDocument doc(clipattrs::toJson(*src));
    QSettings s;
    s.beginGroup(QStringLiteral("clipPresets"));
    s.setValue(name.trimmed(), QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    s.endGroup();
}

void TimelineWidget::applyClipPreset() {
    if (m_selected.isEmpty() || !m_project) return;

    QSettings s;
    s.beginGroup(QStringLiteral("clipPresets"));
    const QStringList names = s.childKeys();
    s.endGroup();
    if (names.isEmpty()) {
        QMessageBox::information(this, tr("Aplicar Preset"),
                                 tr("Nenhum preset salvo ainda. Use\n"
                                    "“Salvar Preset…” em um clipe para criar um."));
        return;
    }

    bool ok = false;
    const QString sel = QInputDialog::getItem(this, tr("Aplicar Preset"),
                                              tr("Preset:"), names, 0, false, &ok);
    if (!ok || sel.isEmpty()) return;

    QSettings s2;
    s2.beginGroup(QStringLiteral("clipPresets"));
    const QJsonObject obj = QJsonDocument::fromJson(
        s2.value(sel).toString().toUtf8()).object();
    s2.endGroup();
    if (obj.isEmpty()) return;

    emit editStart();
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* dst = findClipById(id);
        if (!dst || trackLocked(dst)) continue;
        clipattrs::applyJson(*dst, obj);
    }
    invalidateScene();
    update();
    emit modified();
}

void TimelineWidget::pasteAttributes() {
    if (m_clipboard.isEmpty() || m_selected.isEmpty() || !m_project) return;
    emit undoLabel(tr("Colar atributos"));
    const Clip& src = m_clipboard.first().clip;
    // Diálogo de seleção de atributos (estilo Vegas: Colar Propriedades).
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Colar Atributos"));
    auto* lay = new QVBoxLayout(&dlg);

    auto* chkVideoFx = new QCheckBox(tr("Efeitos de vídeo (brilho, contraste, saturação, blur, grayscale, chroma key)"));
    auto* chkOfxFx = new QCheckBox(tr("Efeitos OFX (plugins de terceiros)"));
    auto* chkTransform = new QCheckBox(tr("Transformar (posição, escala, rotação)"));
    auto* chkCrop = new QCheckBox(tr("Pan/Crop"));
    auto* chkAudioFx = new QCheckBox(tr("Efeitos de áudio (EQ, denoise, normalizar, fase)"));
    auto* chkProperties = new QCheckBox(tr("Propriedades (volume, opacidade, velocidade, fades)"));
    auto* chkKeyframes = new QCheckBox(tr("Keyframes"));
    chkVideoFx->setChecked(true);
    chkOfxFx->setChecked(true);
    chkTransform->setChecked(true);
    chkCrop->setChecked(true);
    chkAudioFx->setChecked(true);
    chkProperties->setChecked(true);
    chkKeyframes->setChecked(true);
    lay->addWidget(chkVideoFx);
    lay->addWidget(chkOfxFx);
    lay->addWidget(chkTransform);
    lay->addWidget(chkCrop);
    lay->addWidget(chkAudioFx);
    lay->addWidget(chkProperties);
    lay->addWidget(chkKeyframes);
    auto* btnAll = new QCheckBox(tr("Selecionar tudo"));
    btnAll->setChecked(true);
    lay->addWidget(btnAll);
    connect(btnAll, &QCheckBox::toggled, [&](bool on) {
        chkVideoFx->setChecked(on); chkOfxFx->setChecked(on);
        chkTransform->setChecked(on); chkCrop->setChecked(on);
        chkAudioFx->setChecked(on); chkProperties->setChecked(on);
        chkKeyframes->setChecked(on);
    });
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(btns);
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    emit editStart();

    // Copia atributos para cada clipe selecionado.
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* dst = findClipById(id);
        if (!dst || trackLocked(dst)) continue;

        if (chkVideoFx->isChecked()) {
            dst->brightness = src.brightness;
            dst->contrast = src.contrast;
            dst->saturation = src.saturation;
            dst->blur = src.blur;
            dst->grayscale = src.grayscale;
            dst->chromaKey = src.chromaKey;
            dst->chromaKeyColor = src.chromaKeyColor;
            dst->chromaKeySimilarity = src.chromaKeySimilarity;
        }
        if (chkOfxFx->isChecked()) {
            dst->ofxFx = src.ofxFx;
        }
        if (chkTransform->isChecked()) {
            dst->tx = src.tx; dst->ty = src.ty;
            dst->scale = src.scale;
            dst->scaleX = src.scaleX; dst->scaleY = src.scaleY;
            dst->rotation = src.rotation;
            dst->anchorX = src.anchorX; dst->anchorY = src.anchorY;
        }
        if (chkCrop->isChecked()) {
            dst->cropL = src.cropL; dst->cropR = src.cropR;
            dst->cropT = src.cropT; dst->cropB = src.cropB;
        }
        if (chkAudioFx->isChecked()) {
            dst->eqLow = src.eqLow; dst->eqMid = src.eqMid; dst->eqHigh = src.eqHigh;
            dst->denoise = src.denoise; dst->denoiseAmount = src.denoiseAmount;
            dst->normalize = src.normalize;
            dst->invertPhase = src.invertPhase;
        }
        if (chkProperties->isChecked()) {
            dst->volume = src.volume;
            dst->opacity = src.opacity;
            dst->speed = src.speed;
            dst->kfSpeed = src.kfSpeed;
            dst->fadeIn = src.fadeIn; dst->fadeOut = src.fadeOut;
        }
        if (chkKeyframes->isChecked()) {
            dst->kfOpacity = src.kfOpacity;
            dst->kfVolume = src.kfVolume;
            dst->kfTx = src.kfTx; dst->kfTy = src.kfTy;
            dst->kfScale = src.kfScale; dst->kfRotation = src.kfRotation;
            dst->kfScaleX = src.kfScaleX; dst->kfScaleY = src.kfScaleY;
            dst->kfAnchorX = src.kfAnchorX; dst->kfAnchorY = src.kfAnchorY;
            dst->kfCropL = src.kfCropL; dst->kfCropR = src.kfCropR;
            dst->kfCropT = src.kfCropT; dst->kfCropB = src.kfCropB;
        }
    }
    invalidateScene();
    update();
    emit modified();
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
    emit undoLabel(tr("Duplicar clipe"));
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

// selectAllClips — implementado em TimelineSelection.cpp

// Vegas: "U" separa os clipes selecionados do seu grupo. Quebra o grupo
// INTEIRO (todos os membros ficam sem groupId), então dá para excluir só um
// dos clipes que estavam unidos (vídeo sem arrastar o áudio, e vice-versa).
void TimelineWidget::ungroupSelected() {
    if (m_selected.isEmpty() || !m_project) return;
    QSet<QString> gids;
    for (const QString& id : m_selected) {
        Clip* c = findClipById(id);
        if (c && !c->groupId.isEmpty()) gids.insert(c->groupId);
    }
    if (gids.isEmpty()) return;
    emit undoLabel(tr("Desagrupar"));
    emit editStart();
    for (const QString& gid : gids)
        for (Clip* m : groupMembers(gid))
            m->groupId.clear();
    updateScrollRanges();
    update();
    emit modified();
}

// Vegas: "G" agrupa os clipes selecionados num grupo único (movem/apagam
// juntos). Se todos já compartilham o mesmo grupo, não faz nada.
void TimelineWidget::groupSelected() {
    if (m_selected.size() < 2 || !m_project) return;
    QString gid;
    bool allSame = true;
    for (const QString& id : m_selected) {
        Clip* c = findClipById(id);
        if (!c) { allSame = false; break; }
        if (gid.isEmpty()) gid = c->groupId;
        else if (gid != c->groupId) allSame = false;
    }
    if (allSame && !gid.isEmpty()) return; // já agrupados
    emit undoLabel(tr("Agrupar"));
    emit editStart();
    const QString ng = newId();
    for (const QString& id : m_selected) {
        Clip* c = findClipById(id);
        if (c) c->groupId = ng;
    }
    updateScrollRanges();
    update();
    emit modified();
}

// Ao sair do widget, limpa os destaques de alças.
void TimelineWidget::leaveEvent(QEvent*) {
    if (!m_hoverGripClip.isEmpty() || !m_hoverCornerClip.isEmpty() || m_hoverCornerSide != 0) {
        m_hoverGripClip.clear();
        m_hoverCornerClip.clear();
        m_hoverCornerSide = 0;
        m_mousePos = QPoint(-1, -1);
        m_mouseOnRuler = false;
        update();
    }
}

// mousePressEvent — implementado em TimelineDrag.cpp
// mouseMoveEvent — implementado em TimelineDrag.cpp
// mouseReleaseEvent — implementado em TimelineDrag.cpp
// mouseDoubleClickEvent — implementado em TimelineDrag.cpp
void TimelineWidget::wheelEvent(QWheelEvent* e) {
    const QPoint delta = e->angleDelta();
    if (e->modifiers() & Qt::ControlModifier) {
        if (delta.y() == 0) return;
        const double factor = std::pow(1.1, delta.y() / 120.0);
        const double anchorT = m_viewStart + (e->position().x() - kHeaderW) / m_pps;
        TlLog::note(QStringLiteral("roda zoom x%1 ancora=%2 (vs=%3 pps=%4)")
                        .arg(factor, 0, 'f', 2).arg(anchorT, 0, 'f', 1)
                        .arg(m_viewStart, 0, 'f', 1).arg(m_pps, 0, 'f', 1));
        animateZoomTo(m_pps * factor, anchorT);
    } else if (delta.y() != 0) {
        m_vbar->setValue(m_vbar->value() - delta.y());
    } else if (delta.x() != 0) {
        m_hbar->setValue(m_hbar->value() - delta.x());
    }
    e->accept();
}

// Tecla configurada para a Tesoura (atalho "tool2", default R), usada no modo
// hold-to-use (segurar ativa a tesoura, soltar restaura a ferramenta).
static Qt::Key razorHoldKey() {
    const QString v = QSettings().value("shortcuts/tool2").toString();
    if (!v.isEmpty()) {
        const QKeySequence ks(v, QKeySequence::PortableText);
        if (ks.count() == 1) {
            const int k = ks[0].key();
            if (k > 0 && k < 0x1000000) return Qt::Key(k);
        }
    }
    return Qt::Key_R;
}

void TimelineWidget::keyPressEvent(QKeyEvent* e) {
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    switch (e->key()) {
    case Qt::Key_Space:
        emit playPauseRequested();
        e->accept();
        break;
    case Qt::Key_Enter:
    case Qt::Key_Return:
        // Enter: pausa e move a agulha vermelha para o ponteiro branco (cursor).
        {
            if (m_playing) {
                emit playPauseRequested();
            }
            const double t = std::clamp(
                m_cursorT >= 0.0 ? m_cursorT : m_playhead, 0.0,
                m_project ? m_project->duration() : 0.0);
            setPlayhead(t);
            emit playheadChanged(t);
        }
        e->accept();
        break;
    case Qt::Key_S:
        cutAtPlayhead();
        e->accept();
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        // Vegas: com uma região de loop (seleção de tempo) ativa, Delete remove
        // o tempo dentro da região (ripple por padrão; Shift+Delete deixa gap).
        if (m_loopOut > m_loopIn) {
            if (shift)
                deleteLoopLeaveGap();
            else
                deleteLoopRipple();
        } else {
            // Mescla seleção secundária na primária para deletar tudo junto.
            if (!m_secondarySelected.isEmpty()) {
                for (const QString& id : m_secondarySelected)
                    if (!m_selected.contains(id)) m_selected.append(id);
                m_secondarySelected.clear();
            }
            if (shift) {
                deleteSelectedLeaveGap();
            } else {
                // "Fechar o vão automaticamente (ripple)" é configurável: com a
                // opção desativada, excluir deixa o espaço vazio.
                if (SettingsDialog::rippleDeleteEnabled())
                    deleteSelection();
                else
                    deleteSelectedLeaveGap();
            }
        }
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
        if (ctrl && shift) { pasteAttributes(); e->accept(); break; }
        if (ctrl) { pasteClips(); e->accept(); break; }
        m_showVolLines = !m_showVolLines; // V: oculta/mostra as linhas de volume
        invalidateScene();
        e->accept();
        break;
    case Qt::Key_D:
        if (ctrl) { duplicateSelected(); e->accept(); break; }
        cutAndDelete();
        e->accept();
        break;
    case Qt::Key_A:
        if (ctrl) { selectAllClips(); e->accept(); break; }
        QWidget::keyPressEvent(e);
        break;
    case Qt::Key_G:
        // Vegas: agrupa os clipes selecionados num grupo único.
        groupSelected();
        e->accept();
        break;
    case Qt::Key_B:
        // Ripple Edit (Premiere/Kdenlive): trim com ripple
        setTool(ToolRipple);
        e->accept();
        break;
    case Qt::Key_N:
        // Rolamento: ajustar fronteira entre 2 clipes
        setTool(ToolRolling);
        e->accept();
        break;
    case Qt::Key_Y:
        // Deslizar: mudar in/out sem mudar posição
        setTool(ToolSlip);
        e->accept();
        break;
    case Qt::Key_U:
        // Ctrl+U: Escorregar: mover clipe e adjacentes ajustam
        // U: Vegas: desagrupa os clipes selecionados
        if (ctrl) {
            setTool(ToolSlide);
            e->accept();
        } else {
            ungroupSelected();
            e->accept();
        }
        break;
    case Qt::Key_W:
        // Esticar Velocidade: mudar velocidade para preencher espaço
        setTool(ToolRateStretch);
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
    case Qt::Key_Q:
        // Vegas: "Q" liga/desliga o loop de reprodução da região de loop.
        m_loopEnabled = !m_loopEnabled;
        emit loopEnabledChanged(m_loopEnabled);
        update();
        e->accept();
        break;
    default:
        QWidget::keyPressEvent(e);
    }
}

// Captura a tecla da Tesoura (default R) globalmente, inclusive quando o foco
// do teclado está fora da timeline (preview, toolbar etc.). Não interfere na
// digitação em campos de texto.
bool TimelineWidget::eventFilter(QObject* o, QEvent* ev) {
    if (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key() == razorHoldKey()) {
            if (qobject_cast<QLineEdit*>(o) || qobject_cast<QTextEdit*>(o)
                || qobject_cast<QPlainTextEdit*>(o) || qobject_cast<QSpinBox*>(o)
                || qobject_cast<QDoubleSpinBox*>(o) || qobject_cast<QComboBox*>(o)
                || qobject_cast<QKeySequenceEdit*>(o))
                return false;
            if (ev->type() == QEvent::KeyPress) {
                if (m_tool != ToolScissors && m_tempToolStore < 0) {
                    m_tempToolStore = m_tool;
                    setTool(ToolScissors);
                }
            } else if (m_tempToolStore >= 0) {
                // Se o usuário trocou de ferramenta na toolbar durante o
                // tecla segurada, respeita a escolha; senão restaura.
                if (m_tool == ToolScissors) setTool(m_tempToolStore);
                m_tempToolStore = -1;
            }
            return true; // consome: não vaza p/ outros widgets/atalhos
        }
    }
    return QWidget::eventFilter(o, ev);
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
            QAction* openMesa = nullptr;
            QAction* delMesa = nullptr;
            QAction* mbMesa = nullptr;
            if (!g->mesaId.isEmpty()) {
                openMesa = menu.addAction(tr("Abrir Mesa"));
                mbMesa = menu.addAction(tr("Motion blur (Ctrl+Shift+B)"));
                delMesa = menu.addAction(tr("Excluir Mesa"));
                MesaComposition* m = m_project->findMesa(g->mesaId);
                if (m) mbMesa->setCheckable(true);
                if (m) mbMesa->setChecked(m->motionBlur);
            }
            QAction* act = menu.exec(e->globalPos());
            if (act == selAll) {
                selectGroupTracks(gid);
                refreshView();
            } else if (act == ren) {
                renameTrackGroup(gid);
            } else if (act == col) {
                toggleGroupCollapsed(gid);
            } else if (act == openMesa) {
                emit mesaOpenRequested(g->mesaId);
            } else if (act == mbMesa) {
                MesaComposition* m = m_project->findMesa(g->mesaId);
                if (m) {
                    emit editStart();
                    m->motionBlur = !m->motionBlur;
                    emit modified();
                    emit mesaChanged(g->mesaId);
                    refreshView();
                    qDebug() << "[TIMELINE] mesa motion blur"
                             << (m->motionBlur ? "ON" : "OFF");
                }
            } else if (act == delMesa) {
                emit editStart();
                const QString mesaId = g->mesaId;
                for (int i = (int)m_project->mesas.size() - 1; i >= 0; --i)
                    if (m_project->mesas[i].id == mesaId)
                        m_project->mesas.removeAt(i);
                g->mesaId.clear();
                updateScrollRanges();
                invalidateScene();
                emit modified();
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
        QAction* pasteAttr = menu.addAction(tr("Colar Atributos…"));
        pasteAttr->setEnabled(!m_clipboard.isEmpty());
        menu.addSeparator();
        QAction* savePreset = menu.addAction(tr("Salvar Preset…"));
        QAction* applyPreset = menu.addAction(tr("Aplicar Preset…"));
        menu.addSeparator();
        QAction* props = menu.addAction(tr("Propriedades…"));
        QAction* speedAct = menu.addAction(tr("Velocidade…"));
        QAction* unlink = nullptr;
        if (!clip->groupId.isEmpty())
            unlink = menu.addAction(tr("Desvincular grupo"));
        QAction* fx = nullptr;
        QAction* grade = nullptr;
        QAction* audioFx = nullptr;
        QAction* transform = nullptr;
        QAction* textAction = nullptr;
        QAction* panCrop = nullptr;
        QAction* maskAction = nullptr;
        if (!audio) {
            fx = menu.addAction(tr("Efeitos de vídeo…"));
            grade = menu.addAction(tr("Correção de cor…"));
            transform = menu.addAction(tr("Transformar…"));
            textAction = menu.addAction(tr("Texto…"));
            panCrop = menu.addAction(tr("Pancrop…"));
            maskAction = menu.addAction(tr("Máscara…"));
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
        QAction* transWipeTL = nullptr;
        QAction* transWipeTR = nullptr;
        QAction* transWipeBL = nullptr;
        QAction* transWipeBR = nullptr;
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
            transMenu->addSeparator();
            QMenu* diagMenu = transMenu->addMenu(tr("Wipes diagonais"));
            transWipeTL = diagMenu->addAction(tr("↖ Wipe (próximo vem de baixo-direita)"));
            transWipeTL->setCheckable(true);
            transWipeTL->setChecked(cur == QStringLiteral("wipetl"));
            transGrp->addAction(transWipeTL);
            transWipeTR = diagMenu->addAction(tr("↗ Wipe (próximo vem de baixo-esquerda)"));
            transWipeTR->setCheckable(true);
            transWipeTR->setChecked(cur == QStringLiteral("wipetr"));
            transGrp->addAction(transWipeTR);
            transWipeBR = diagMenu->addAction(tr("↘ Wipe (próximo vem de cima-esquerda)"));
            transWipeBR->setCheckable(true);
            transWipeBR->setChecked(cur == QStringLiteral("wipebr"));
            transGrp->addAction(transWipeBR);
            transWipeBL = diagMenu->addAction(tr("↙ Wipe (próximo vem de cima-direita)"));
            transWipeBL->setCheckable(true);
            transWipeBL->setChecked(cur == QStringLiteral("wipebl"));
            transGrp->addAction(transWipeBL);
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
        else if (act == pasteAttr) pasteAttributes();
        else if (act == savePreset) saveClipPreset();
        else if (act == applyPreset) applyClipPreset();
        else if (act == props) emit propertiesRequested(clip->id);
        else if (act == speedAct) showSpeedDialog(clip);
        else if (act == unlink) {
            emit editStart();
            for (Clip* m : groupMembers(clip->groupId))
                m->groupId.clear();
            update();
            emit modified();
        }
        else if (act == fx) showEffectsDialog(clip);
        else if (act == grade) showGradingDialog(clip);
        else if (act == audioFx) showAudioEffectsDialog(clip);
        else if (act == transform) showTransformDialog(clip);
        else if (act == textAction) showTextEditorDialog(clip);
        else if (act == panCrop) emit pancropRequested(clip->id);
        else if (act == maskAction) emit maskRequested(clip->id);
        else if (act == transDissolve || act == transWipeL || act == transWipeR
                 || act == transWipeU || act == transWipeD
                 || act == transWipeTL || act == transWipeTR
                 || act == transWipeBL || act == transWipeBR) {
            emit editStart();
            if (act == transDissolve) clip->transitionType = QStringLiteral("dissolve");
            else if (act == transWipeL) clip->transitionType = QStringLiteral("wipeleft");
            else if (act == transWipeR) clip->transitionType = QStringLiteral("wiperight");
            else if (act == transWipeU) clip->transitionType = QStringLiteral("wipeup");
            else if (act == transWipeD) clip->transitionType = QStringLiteral("wipedown");
            else if (act == transWipeTL) clip->transitionType = QStringLiteral("wipetl");
            else if (act == transWipeTR) clip->transitionType = QStringLiteral("wipetr");
            else if (act == transWipeBL) clip->transitionType = QStringLiteral("wipebl");
            else if (act == transWipeBR) clip->transitionType = QStringLiteral("wipebr");
            invalidateScene();
            emit modified();
        }
    } else if (e->pos().y() < kRulerH) {
        const double t = std::max(0.0, snapTime(xToTime(e->pos().x())));
        QAction* addM = menu.addAction(tr("Adicionar marcador"));
        QAction* addMN = menu.addAction(tr("Adicionar marcador nomeado…"));
        QAction* clearM = menu.addAction(tr("Limpar todos os marcadores"));
        QAction* delLoopRip = nullptr;
        QAction* delLoopGap = nullptr;
        if (m_loopOut > m_loopIn) {
            menu.addSeparator();
            delLoopRip = menu.addAction(tr("Excluir tempo da região de loop (ripple)"));
            delLoopGap = menu.addAction(tr("Excluir tempo da região de loop (deixa espaço)"));
        }
        QAction* act = menu.exec(e->globalPos());
        if (act == delLoopRip) deleteLoopRipple();
        else if (act == delLoopGap) deleteLoopLeaveGap();
        else if (act == addM) {
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
        QAction* newMesa = menu.addAction(tr("Nova Mesa…"));
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
        QAction* trackOp = nullptr;
        if (track && !track->audio)
            trackOp = menu.addAction(tr("Opacidade da faixa: %1%")
                                         .arg((int)llround(track->opacity * 100.0)));
        QAction* groupTracks = nullptr;
        QAction* renameGroup = nullptr;
        QAction* ungroupTracks = nullptr;
        QAction* createMesa = nullptr;
        QMenu* sendMesaMenu = nullptr;
        if (track) {
            menu.addSeparator();
            if (m_selTracks.size() >= 2)
                groupTracks = menu.addAction(tr("Agrupar faixas em pasta…"));
            if (!track->groupId.isEmpty()) {
                renameGroup = menu.addAction(tr("Renomear pasta…"));
                ungroupTracks = menu.addAction(tr("Desagrupar faixas"));
            }
            createMesa = menu.addAction(tr("Criar Mesa"));
            if (!track->audio && !m_project->mesas.isEmpty()) {
                sendMesaMenu = menu.addMenu(tr("Enviar para Mesa"));
                for (const MesaComposition& mesa : m_project->mesas) {
                    QAction* a = sendMesaMenu->addAction(
                        mesa.name.isEmpty() ? mesa.id : mesa.name);
                    a->setData(mesa.id);
                }
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
        else if (act == newMesa) {
            criarMesa();
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
        else if (act == trackOp) {
            bool ok = false;
            const double v = QInputDialog::getDouble(
                this, tr("Opacidade da faixa"), tr("Opacidade (0–100%):"),
                track->opacity * 100.0, 0.0, 100.0, 0, &ok);
            if (ok) {
                emit editStart();
                track->opacity = v / 100.0;
                emit modified();
                update();
            }
        }
        else if (act == delTrack) {
            emit editStart();
            m_project->removeTrack(audio, vrow);
            pruneEmptyGroups();
            m_selected.clear();
            m_secondarySelected.clear();
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
        else if (track && act == createMesa) {
            criarMesa();
        }
        else if (track && act && sendMesaMenu && act->parent() == sendMesaMenu) {
            sendTrackToMesa(track->id, act->data().toString());
        }
        else if (track && act && blendMenu && act->parent() == blendMenu) {
            emit editStart();
            track->blendMode = act->text();
            emit modified();
            update();
        }
    }
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
    TimelineCommands::cutAndDelete(this);
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

void TimelineWidget::showGradingDialog(Clip* c) {
    if (!c) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Correção de cor (Lift / Gamma / Gain)"));

    struct Row { QSlider* r = nullptr; QSlider* g = nullptr; QSlider* b = nullptr; };
    QVector<Row> rows(3);
    const struct { int min; int max; int def; } cfg[3] = {
        { -100, 100, 0 },    // Lift (neutro 0, exibido em -100..100)
        {   10, 400, 100 },  // Gamma (neutro 100%)
        { -100, 100, 0 },    // Gain (neutro 0)
    };
    const QString names[3] = { tr("Lift (sombras)"), tr("Gamma (meios)"), tr("Gain (realces)") };

    auto* grid = new QGridLayout;
    grid->addWidget(new QLabel(tr("R"), &dlg), 0, 1, Qt::AlignCenter);
    grid->addWidget(new QLabel(tr("G"), &dlg), 0, 2, Qt::AlignCenter);
    grid->addWidget(new QLabel(tr("B"), &dlg), 0, 3, Qt::AlignCenter);
    for (int i = 0; i < 3; ++i) {
        Row& row = rows[i];
        row.r = new QSlider(Qt::Horizontal, &dlg);
        row.g = new QSlider(Qt::Horizontal, &dlg);
        row.b = new QSlider(Qt::Horizontal, &dlg);
        for (QSlider* s : { row.r, row.g, row.b }) {
            s->setRange(cfg[i].min, cfg[i].max);
            s->setValue(cfg[i].def);
        }
        grid->addWidget(new QLabel(names[i], &dlg), i + 1, 0);
        grid->addWidget(row.r, i + 1, 1);
        grid->addWidget(row.g, i + 1, 2);
        grid->addWidget(row.b, i + 1, 3);
    }

    // Valores atuais do clipe (gamma em %, lift/gain em -100..100).
    rows[0].r->setValue((int)llround(c->liftR * 100.0));
    rows[0].g->setValue((int)llround(c->liftG * 100.0));
    rows[0].b->setValue((int)llround(c->liftB * 100.0));
    rows[1].r->setValue((int)llround(c->gammaR * 100.0));
    rows[1].g->setValue((int)llround(c->gammaG * 100.0));
    rows[1].b->setValue((int)llround(c->gammaB * 100.0));
    rows[2].r->setValue((int)llround(c->gainR * 100.0));
    rows[2].g->setValue((int)llround(c->gainG * 100.0));
    rows[2].b->setValue((int)llround(c->gainB * 100.0));

    auto* reset = new QPushButton(tr("Resetar"), &dlg);
    connect(reset, &QPushButton::clicked, &dlg, [&rows]() {
        for (int i = 0; i < 3; ++i)
            for (QSlider* s : { rows[i].r, rows[i].g, rows[i].b })
                s->setValue((i == 1) ? 100 : 0);
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* lay = new QVBoxLayout(&dlg);
    lay->addLayout(grid);
    lay->addWidget(reset);
    lay->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted) return;

    emit editStart();
    c->liftR = rows[0].r->value() / 100.0;
    c->liftG = rows[0].g->value() / 100.0;
    c->liftB = rows[0].b->value() / 100.0;
    c->gammaR = rows[1].r->value() / 100.0;
    c->gammaG = rows[1].g->value() / 100.0;
    c->gammaB = rows[1].b->value() / 100.0;
    c->gainR = rows[2].r->value() / 100.0;
    c->gainG = rows[2].g->value() / 100.0;
    c->gainB = rows[2].b->value() / 100.0;
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
void TimelineWidget::deleteSelection() {
    deleteSelected();
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

// Menu dropdown do chip FX no cabeçalho de uma faixa de áudio (estilo Vegas):
// liga/desliga efeitos rapidamente ou abre o diálogo de ajustes.
void TimelineWidget::trackFxMenu(Track* track, const QPoint& at) {
    if (!track || !m_project) return;
    QMenu menu(this);
    QAction* denoise = menu.addAction(tr("Redução de ruído"));
    denoise->setCheckable(true);
    denoise->setChecked(track->denoise);
    QAction* reverb = menu.addAction(tr("Reverb EX"));
    reverb->setCheckable(true);
    reverb->setChecked(track->reverb);
    QAction* invert = menu.addAction(tr("Inverter fase"));
    invert->setCheckable(true);
    invert->setChecked(track->invertPhase);
    menu.addSeparator();
    QAction* adjust = menu.addAction(tr("Ajustes de EQ / Reverb…"));
    QAction* clear = menu.addAction(tr("Limpar efeitos"));

    const QAction* chosen = menu.exec(at);
    if (!chosen) return;
    if (chosen == adjust) {
        TrackAudioFxDialog dlg(track, this);
        // O diálogo só escreve na faixa em accept(); emitenos o undo depois.
        if (dlg.exec() != QDialog::Accepted) return;
        emit editStart();
        update();
        emit modified();
        return;
    }
    emit editStart();
    if (chosen == denoise) {
        track->denoise = !track->denoise;
        if (track->denoise && track->denoiseAmount < 1.0) track->denoiseAmount = 12.0;
    } else if (chosen == reverb) {
        track->reverb = !track->reverb;
    } else if (chosen == invert) {
        track->invertPhase = !track->invertPhase;
    } else if (chosen == clear) {
        track->eqLow = track->eqMid = track->eqHigh = 0.0;
        track->denoise = false;
        track->invertPhase = false;
        track->reverb = false;
        track->reverbMix = 0.35;
        track->reverbSize = 0.5;
    }
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

Clip* TimelineWidget::clipAtTime(int row, bool audio, double t) const {
    if (!m_project || row < 0) return nullptr;
    const Track& tr = audio ? m_project->audioTracks[row] : m_project->videoTracks[row];
    for (const Clip& c : tr.clips) {
        if (t >= c.pos && t < c.pos + c.dur)
            return const_cast<Clip*>(&c);
    }
    return nullptr;
}

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
