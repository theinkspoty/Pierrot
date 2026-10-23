// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/motion/GraphEditorWidget.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/core/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QPushButton>
#include <QToolButton>
#include <QButtonGroup>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QFrame>
#include <QPixmap>
#include <QIcon>
#include <QFont>
#include <algorithm>
#include <cmath>
#include <functional>

#include "ui/motion/GraphEditorPriv.h"


// --------------------------------------------------------------------------
// GraphEditorWidget
// --------------------------------------------------------------------------

GraphEditorWidget::GraphEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* snapBtn = new QPushButton(tr("Snap"), this);
    snapBtn->setCheckable(true);
    snapBtn->setChecked(true);
    snapBtn->setToolTip(tr("Encaixar nos frames (S; Ctrl durante o arrasto = livre)"));

    auto* fitBtn = new QPushButton(tr("Ajustar"), this);
    fitBtn->setToolTip(tr("Mostrar o clipe inteiro (F; Ctrl+roda dá zoom)"));

    auto* topBar = new QHBoxLayout;
    topBar->setContentsMargins(6, 2, 6, 0);
    topBar->setSpacing(4);
    topBar->addWidget(snapBtn);
    topBar->addWidget(fitBtn);
    topBar->addSpacing(8);

    m_toolSel = new QToolButton(this);
    m_toolSel->setCheckable(true);
    m_toolSel->setChecked(true);
    m_toolSel->setText(tr("Selecionar"));
    m_toolSel->setToolTip(tr("Selecionar e mover keyframes (V)"));
    m_toolAdd = new QToolButton(this);
    m_toolAdd->setCheckable(true);
    m_toolAdd->setText(tr("Adicionar"));
    m_toolAdd->setToolTip(tr("Clicar no gráfico adiciona um keyframe (P)"));
    m_toolCurve = new QToolButton(this);
    m_toolCurve->setCheckable(true);
    m_toolCurve->setText(tr("Curva"));
    m_toolCurve->setToolTip(tr("Clicar cria um ponto suave; arrastar define a curva (B)"));

    // Varinha Easy Ease (F9): suaviza os keyframes selecionados — o
    // equivalente ao Keyframe Assistant do After Effects / Premiere.
    m_easyEaseBtn = new QToolButton(this);
    m_easyEaseBtn->setText(tr("Varinha"));
    m_easyEaseBtn->setToolTip(tr("Easy Ease nos keyframes selecionados (F9) — "
                                 "suaviza a animação com handles horizontais"));
    m_easyEaseBtn->setCursor(Qt::PointingHandCursor);
    connect(m_easyEaseBtn, &QToolButton::clicked, this, [this]() {
        m_canvas->applyEasyEase(0);
    });

    auto* tools = new QButtonGroup(this);
    tools->setExclusive(true);
    tools->addButton(m_toolSel, static_cast<int>(CanvasTool::Select));
    tools->addButton(m_toolAdd, static_cast<int>(CanvasTool::Add));
    tools->addButton(m_toolCurve, static_cast<int>(CanvasTool::Curve));
    topBar->addWidget(m_toolSel);
    topBar->addWidget(m_toolAdd);
    topBar->addWidget(m_toolCurve);
    topBar->addWidget(m_easyEaseBtn);
    topBar->addStretch(1);

    m_canvas = new GraphCanvas(this);

    auto* canvasWrap = new QWidget(this);
    auto* cw = new QVBoxLayout(canvasWrap);
    cw->setContentsMargins(0, 0, 0, 0);
    cw->setSpacing(0);
    cw->addLayout(topBar);
    cw->addWidget(m_canvas, 1);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setMinimumWidth(160);

    auto* list = new QWidget(this);
    m_rowsLayout = new QVBoxLayout(list);
    m_rowsLayout->setContentsMargins(2, 2, 2, 2);
    m_rowsLayout->setSpacing(2);
    m_noClip = new QLabel(tr("Selecione um clipe na timeline para\neditar os keyframes."), list);
    m_noClip->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_noClip->setStyleSheet(QStringLiteral("color:%1; font-size:11px; padding:8px 4px;").arg(themeColors().iconMuted.name()));
    m_rowsLayout->addWidget(m_noClip);
    m_rowsLayout->addStretch(1);
    m_scroll->setWidget(list);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_scroll);
    splitter->addWidget(canvasWrap);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(5);
    splitter->setSizes({ 240, 600 });

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color:%1; font-size:11px; padding:2px 6px;").arg(themeColors().iconNormal.name()));

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(splitter, 1);
    lay->addWidget(m_status);

    setMinimumHeight(170);

    connect(m_canvas, &GraphCanvas::editStart, this, &GraphEditorWidget::editStart);
    connect(m_canvas, &GraphCanvas::modified, this, &GraphEditorWidget::modified);
    connect(m_canvas, &GraphCanvas::statusMessage, m_status, &QLabel::setText);
    connect(m_canvas, &GraphCanvas::keyframeJump, this, &GraphEditorWidget::keyframeJump);
    connect(m_canvas, &GraphCanvas::snapChanged, snapBtn, &QPushButton::setChecked);
    connect(snapBtn, &QPushButton::toggled, m_canvas, &GraphCanvas::setSnap);
    connect(fitBtn, &QPushButton::clicked, m_canvas, &GraphCanvas::resetZoom);
    connect(tools, &QButtonGroup::idClicked, m_canvas, [this](int id) {
        m_canvas->setTool((CanvasTool)id);
    });
    connect(m_canvas, &GraphCanvas::toolChanged, this, [this, tools](CanvasTool t) {
        if (QAbstractButton* b = tools->button((int)t)) b->setChecked(true);
    });
}

// Tamanho padrão do painel: evita que o dock abra com metade da tela na
// primeira execução (sem layout salvo ainda).
QSize GraphEditorWidget::sizeHint() const {
    return QSize(520, 240);
}

void GraphEditorWidget::setProject(Project* p) {
    m_project = p;
    rebuildRows();
    if (m_camMode && m_mesaCamera)
        m_canvas->setCamData(m_mesaCamera, m_prop, m_playhead,
                             m_project ? m_project->fps : 30.0);
    else if (m_mesaMode && m_mesaTrack)
        m_canvas->setMesaData(m_mesaTrack, m_prop, m_playhead,
                              m_project ? m_project->fps : 30.0);
    else
        m_canvas->setData(activeClip(), m_prop, m_playhead,
                          m_project ? m_project->fps : 30.0);
}

void GraphEditorWidget::setClipId(const QString& id) {
    if (m_clipId == id) return;
    m_clipId = id;
    m_mesaTrack = nullptr;
    m_mesaMode = false;
    m_mesaCamera = nullptr;
    m_camMode = false;
    m_activeMaskIndex = -1; // sai do modo máscara ao trocar de clipe
    if (m_prop >= GPropMaskCx && m_prop <= GPropMaskFeather) m_prop = GPropOpacity;
    rebuildRows();
    Clip* c = activeClip();
    if (c) {
        auto* km = keysFor(c, m_prop);
        if (!km || km->isEmpty()) {
            for (GraphProp p : m_props) {
                auto* kp = keysFor(c, p);
                if (kp && !kp->isEmpty()) { m_prop = p; break; }
            }
        }
        if (!m_props.contains(m_prop)) m_prop = m_props.first();
    }
    m_canvas->setData(activeClip(), m_prop, m_playhead,
                      m_project ? m_project->fps : 30.0);
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (row) {
            row->setActive(p == m_prop);
            row->setExpanded(p == m_prop);
        }
    }
    syncValueLabels();
}

void GraphEditorWidget::setMesaTrack(Track* track) {
    if (m_mesaTrack == track) return;
    m_mesaTrack = track;
    m_mesaMode = (track != nullptr);
    m_mesaCamera = nullptr;
    m_camMode = false;
    m_clipId.clear();
    rebuildRows();
    if (track) {
        // Find first property with keyframes
        for (GraphProp p : m_props) {
            QVector<Keyframe>* ks = keysForTrack(track, p);
            if (ks && !ks->isEmpty()) { m_prop = p; break; }
        }
        if (!m_props.contains(m_prop)) m_prop = m_props.first();
    }
    m_canvas->setMesaData(track, m_prop, m_playhead,
                          m_project ? m_project->fps : 30.0);
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (row) {
            row->setActive(p == m_prop);
            row->setExpanded(p == m_prop);
        }
    }
    syncValueLabels();
}

void GraphEditorWidget::setMesaCamera(MesaComposition* mc) {
    if (m_mesaCamera == mc) return;
    m_mesaCamera = mc;
    m_camMode = (mc != nullptr);
    m_mesaTrack = nullptr;
    m_mesaMode = false;
    m_clipId.clear();
    rebuildRows();
    if (mc) {
        for (GraphProp p : m_props) {
            QVector<Keyframe>* ks = keysForCamera(mc, p);
            if (ks && !ks->isEmpty()) { m_prop = p; break; }
        }
        if (!m_props.contains(m_prop)) m_prop = m_props.first();
    }
    m_canvas->setCamData(mc, m_prop, m_playhead,
                         m_project ? m_project->fps : 30.0);
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (row) {
            row->setActive(p == m_prop);
            row->setExpanded(p == m_prop);
        }
    }
    syncValueLabels();
}

void GraphEditorWidget::setPlayhead(double t) {
    m_playhead = t;
    m_canvas->setPlayhead(t);
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (row) row->setStripPlayhead(t);
    }
    syncValueLabels();
}

void GraphEditorWidget::refresh() {
    if (m_camMode && m_mesaCamera) {
        const double zt0 = m_canvas->zoomT0();
        const double zt1 = m_canvas->zoomT1();
        const QVector<double> selTimes = m_canvas->selectionTimes();
        syncRowData();
        m_canvas->setCamData(m_mesaCamera, m_prop, m_playhead,
                             m_project ? m_project->fps : 30.0);
        if (zt1 > zt0) m_canvas->setZoom(zt0, zt1);
        if (!selTimes.isEmpty()) m_canvas->selectKeysAtTimes(selTimes);
        return;
    }
    if (m_mesaMode && m_mesaTrack) {
        // Mesa mode: just refresh row data, preserve zoom and selection
        const double zt0 = m_canvas->zoomT0();
        const double zt1 = m_canvas->zoomT1();
        const QVector<double> selTimes = m_canvas->selectionTimes();
        syncRowData();
        m_canvas->setMesaData(m_mesaTrack, m_prop, m_playhead,
                              m_project ? m_project->fps : 30.0);
        if (zt1 > zt0) m_canvas->setZoom(zt0, zt1);
        if (!selTimes.isEmpty()) m_canvas->selectKeysAtTimes(selTimes);
        return;
    }
    // Clip mode
    bool changed = false;
    {
        Clip* c = activeClip();
        bool hasVideo = false, hasAudio = false, hasMask = false;
        if (c && m_project) {
            const MediaItem* mi = m_project->findMedia(c->mediaId);
            if (mi) { hasVideo = mi->hasVideo; hasAudio = mi->hasAudio; }
            if (c->isText) hasVideo = true;
            hasMask = !c->masks.isEmpty();
        }
        bool haveVideo = false, haveAudio = false, haveMask = false;
        for (GraphProp p : m_props) {
            if (p == GPropVolume) haveAudio = true;
            else if (p >= GPropMaskCx && p <= GPropMaskFeather) haveMask = true;
            else haveVideo = true;
        }
        // Reconstrói também quando o nº de máscaras muda (criou/apagou máscara
        // no Pantalone) — é o que faz o grupo "Máscara" aparecer.
        changed = (haveVideo != hasVideo) || (haveAudio != hasAudio) ||
                  (haveMask != hasMask);
    }
    if (changed) rebuildRows();

    const double zt0 = m_canvas->zoomT0();
    const double zt1 = m_canvas->zoomT1();
    const QVector<double> selTimes = m_canvas->selectionTimes();
    syncRowData();
    const double fps = m_project ? m_project->fps : 30.0;
    // Canvas no modo certo: máscara se a prop atual é de máscara, senão clipe.
    if (m_prop >= GPropMaskCx && m_prop <= GPropMaskFeather)
        m_canvas->setMaskData(activeClip(), m_activeMaskIndex, m_prop, m_playhead, fps);
    else
        m_canvas->setData(activeClip(), m_prop, m_playhead, fps);
    if (zt1 > zt0) m_canvas->setZoom(zt0, zt1);
    if (!selTimes.isEmpty()) m_canvas->selectKeysAtTimes(selTimes);
}

Clip* GraphEditorWidget::activeClip() const {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    return nullptr;
}

// Keyframes/valor base cientes de máscara: para props GPropMask* usa a máscara
// em edição; para as demais, o clipe. Evita deref de nullptr.
QVector<Keyframe>* GraphEditorWidget::keysForActive(GraphProp p) const {
    if (p >= GPropMaskCx && p <= GPropMaskFeather)
        return keysForMask(maskAt(activeClip(), m_activeMaskIndex), p);
    return keysFor(activeClip(), p);
}

double GraphEditorWidget::baseForActive(GraphProp p) const {
    if (p >= GPropMaskCx && p <= GPropMaskFeather)
        return baseForMask(maskAt(activeClip(), m_activeMaskIndex), p);
    return baseFor(activeClip(), p);
}

// Exibe a curva da máscara ATIVA (Pantalone) — motion da janela no editor de
// curvas. Não passa por m_props (que lista só props do clipe).
void GraphEditorWidget::setActiveMask(int maskIndex, GraphProp p) {
    if (p < GPropMaskCx || p > GPropMaskFeather) return;
    m_activeMaskIndex = maskIndex;
    m_prop = p;
    for (GraphProp q : m_props) {
        GraphPropRow* row = m_rows.value((int)q);
        if (row) row->setActive(false);
    }
    m_canvas->setMaskData(activeClip(), maskIndex, p, m_playhead,
                          m_project ? m_project->fps : 30.0);
    syncValueLabels();
}

double GraphEditorWidget::propValueAtPlayhead(GraphProp p) const {
    if (m_camMode && m_mesaCamera) {
        const QVector<Keyframe>* K = keysForCamera(m_mesaCamera, p);
        return kfValue(*K, baseForCamera(m_mesaCamera, p), std::max(0.0, m_playhead));
    }
    if (m_mesaMode && m_mesaTrack) {
        const QVector<Keyframe>* K = keysForTrack(m_mesaTrack, p);
        return kfValue(*K, baseForTrack(m_mesaTrack, p), std::max(0.0, m_playhead));
    }
    Clip* c = activeClip();
    const QVector<Keyframe>* K = keysForActive(p);
    if (!K) return baseForActive(p);
    double rel = 0.0;
    if (c) rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    return kfValue(*K, baseForActive(p), rel);
}

void GraphEditorWidget::setProperty(GraphProp p) {
    if (!m_props.contains(p)) return;
    if (p == m_prop) return;
    m_activeMaskIndex = -1; // selecionar prop do clipe sai do modo máscara
    m_prop = p;
    if (m_camMode && m_mesaCamera)
        m_canvas->setCamData(m_mesaCamera, p, m_playhead,
                             m_project ? m_project->fps : 30.0);
    else if (m_mesaMode && m_mesaTrack)
        m_canvas->setMesaData(m_mesaTrack, p, m_playhead,
                              m_project ? m_project->fps : 30.0);
    else
        m_canvas->setData(activeClip(), p, m_playhead,
                          m_project ? m_project->fps : 30.0);
    for (GraphProp q : m_props) {
        GraphPropRow* row = m_rows.value((int)q);
        if (row) row->setActive(q == p);
    }
    if (GraphPropRow* row = m_rows.value((int)p)) row->setExpanded(true);
    syncValueLabels();
}

void GraphEditorWidget::syncRowData() {
    const double fps = m_project ? m_project->fps : 30.0;
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (!row) continue;
        if (m_camMode && m_mesaCamera) {
            QVector<Keyframe>* K = keysForCamera(m_mesaCamera, p);
            row->setAnimated(K && !K->isEmpty());
            row->setStripCamData(m_mesaCamera, fps);
        } else if (m_mesaMode && m_mesaTrack) {
            QVector<Keyframe>* K = keysForTrack(m_mesaTrack, p);
            row->setAnimated(K && !K->isEmpty());
            row->setStripMesaData(m_mesaTrack, fps);
        } else {
            Clip* c = activeClip();
            if (p >= GPropMaskCx && p <= GPropMaskFeather) {
                const QVector<Keyframe>* K = keysForMask(maskAt(c, m_activeMaskIndex), p);
                row->setAnimated(K && !K->isEmpty());
                row->setStripMaskData(c, m_activeMaskIndex, fps);
            } else {
                row->setStripData(c, fps);
                const QVector<Keyframe>* K = keysFor(c, p);
                row->setAnimated(K && !K->isEmpty());
            }
        }
        row->setActive(p == m_prop);
    }
    syncValueLabels();
}

void GraphEditorWidget::syncValueLabels() {
    for (GraphProp p : m_props) {
        GraphPropRow* row = m_rows.value((int)p);
        if (!row) continue;
        const double v = propValueAtPlayhead(p);
        double lo = 0.0, hi = 1.0;
        rangeFor(p, &lo, &hi);
        row->setValueText(fmtVal(v, hi - lo));
    }
}

void GraphEditorWidget::rebuildRows() {
    while (m_rowsLayout->count() > 2) {
        QLayoutItem* it = m_rowsLayout->takeAt(1);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    m_rows.clear();
    m_props.clear();

    // Propriedades separadas em GRUPOS (cabeçalho + linhas), em vez de uma
    // coluna plana — alinhado à estrutura por modo do Pantalone.
    struct G { QString title; QVector<GraphProp> props; };
    QVector<G> groups;

    if (m_camMode && m_mesaCamera) {
        groups << G{ QString(), { GPropCamX, GPropCamY, GPropCamZoom, GPropCamRotation } };
        m_noClip->setVisible(false);
    } else if (m_mesaMode && m_mesaTrack) {
        groups << G{ QString(), { GPropMesaX, GPropMesaY, GPropMesaScaleX,
                                  GPropMesaRotation, GPropMesaOpacity,
                                  GPropMesaAnchorX, GPropMesaAnchorY } };
        m_noClip->setVisible(false);
    } else {
        Clip* c = activeClip();
        if (!c) {
            m_canvas->setData(nullptr, m_prop, m_playhead,
                              m_project ? m_project->fps : 30.0);
            m_noClip->setVisible(true);
            return;
        }
        bool hasVideo = false, hasAudio = false;
        if (m_project) {
            const MediaItem* mi = m_project->findMedia(c->mediaId);
            if (mi) { hasVideo = mi->hasVideo; hasAudio = mi->hasAudio; }
            if (c->isText) hasVideo = true;
        }
        m_activeMaskIndex = -1;
        if (hasVideo) {
            groups << G{ tr("Transformação"),
                         { GPropOpacity, GPropScale, GPropScaleX, GPropScaleY,
                           GPropRotation, GPropTx, GPropTy, GPropAnchorX, GPropAnchorY } };
            groups << G{ tr("Recorte"),
                         { GPropCropL, GPropCropR, GPropCropT, GPropCropB } };
        }
        if (hasAudio) groups << G{ tr("Áudio"), { GPropVolume } };
        if (!c->masks.isEmpty()) {
            m_activeMaskIndex = 0;
            groups << G{ tr("Máscara"),
                         { GPropMaskCx, GPropMaskCy, GPropMaskRx, GPropMaskRy,
                           GPropMaskRot, GPropMaskFeather } };
        }
        if (groups.isEmpty()) groups << G{ QString(), { GPropOpacity } };
        m_noClip->setVisible(false);
    }

    // Monta m_props na ordem de exibição (grupos).
    for (const G& g : groups)
        for (GraphProp p : g.props) m_props.append(p);
    if (!m_props.contains(m_prop)) m_prop = m_props.first();

    for (const G& g : groups) {
        if (!g.title.isEmpty()) {
            auto* h = new QLabel(g.title);
            h->setObjectName(QStringLiteral("graphGroupHeader"));
            h->setStyleSheet(QStringLiteral(
                "color:%1; font-weight:bold; font-size:11px; padding:7px 4px 2px 4px;")
                .arg(themeColors().text.name()));
            m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, h);
        }
        for (GraphProp p : g.props) {
            auto* row = new GraphPropRow(p);
            m_rowsLayout->insertWidget(m_rowsLayout->count() - 1, row);
            m_rows.insert((int)p, row);

            connect(row, &GraphPropRow::propertyClicked, this, [this, p]() {
                if (p >= GPropMaskCx && p <= GPropMaskFeather) setActiveMask(m_activeMaskIndex, p);
                else setProperty(p);
            });
            connect(row, &GraphPropRow::stopwatchClicked, this, [this, p]() { toggleAnimation(p); });
            connect(row, &GraphPropRow::prevKeyframe, this, [this, p]() { jumpKeyframe(p, -1); });
            connect(row, &GraphPropRow::nextKeyframe, this, [this, p]() { jumpKeyframe(p, 1); });
            connect(row, &GraphPropRow::toggleKeyframe, this, [this, p]() { toggleKeyAtPlayhead(p); });
            connect(row, &GraphPropRow::stripKeyClicked, this, [this, p](int idx) {
                if (p >= GPropMaskCx && p <= GPropMaskFeather) setActiveMask(m_activeMaskIndex, p);
                else setProperty(p);
                m_canvas->selectKeyIndex(idx);
            });
            connect(row, &GraphPropRow::stripDragStart, this, [this](int) { emit editStart(); });
            connect(row, &GraphPropRow::stripDragKey, this, [this, p](int idx, double t) {
                m_canvas->dragStripTime(idx, t);
            });
            connect(row, &GraphPropRow::stripAddKey, this, [this, p](double t) {
                Clip* c = activeClip();
                QVector<Keyframe>* kp = keysForActive(p);
                if (!c || !kp) return;
                const double rel = std::clamp(t, 0.0, c->dur);
                QVector<Keyframe>& K = *kp;
                for (const Keyframe& k : K)
                    if (std::fabs(k.time - rel) < 1e-9) {
                        m_status->setText(tr("Já existe um keyframe nesse tempo."));
                        return;
                    }
                const double v = kfValue(K, baseForActive(p), rel);
                emit editStart();
                m_canvas->addKeyframe(rel, v);
                syncRowData();
                m_canvas->commitChange();
            });
        }
    }
    syncRowData();
    if (GraphPropRow* r = m_rows.value((int)m_prop)) r->setExpanded(true);
}

void GraphEditorWidget::toggleAnimation(GraphProp p) {
    emit editStart();
    if (m_camMode && m_mesaCamera) {
        QVector<Keyframe>& K = *keysForCamera(m_mesaCamera, p);
        if (K.isEmpty()) {
            Keyframe nk;
            nk.time = std::max(0.0, m_playhead);
            nk.value = baseForCamera(m_mesaCamera, p);
            nk.interp = KfSmooth;
            K.append(nk);
        } else {
            K.clear();
            m_canvas->clearSelection();
        }
    } else if (m_mesaMode && m_mesaTrack) {
        QVector<Keyframe>& K = *keysForTrack(m_mesaTrack, p);
        if (K.isEmpty()) {
            Keyframe nk;
            nk.time = std::max(0.0, m_playhead);
            nk.value = baseForTrack(m_mesaTrack, p);
            nk.interp = KfSmooth;
            K.append(nk);
        } else {
            K.clear();
            m_canvas->clearSelection();
        }
    } else {
        Clip* c = activeClip();
        QVector<Keyframe>* kp = keysForActive(p);
        if (!c || !kp) return;
        QVector<Keyframe>& K = *kp;
        if (K.isEmpty()) {
            const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
            Keyframe nk;
            nk.time = rel;
            nk.value = baseForActive(p);
            nk.interp = KfSmooth;
            K.append(nk);
        } else {
            K.clear();
            m_canvas->clearSelection();
        }
    }
    m_canvas->fitValueRange();
    syncRowData();
    m_canvas->commitChange();
}

void GraphEditorWidget::toggleKeyAtPlayhead(GraphProp p) {
    emit editStart();
    const double fr = 1.0 / std::max(1.0, m_project ? m_project->fps : 30.0);
    if (m_camMode && m_mesaCamera) {
        QVector<Keyframe>& K = *keysForCamera(m_mesaCamera, p);
        const double t0 = std::max(0.0, m_playhead);
        const double rel = std::round(t0 / fr) * fr;
        int at = -1;
        for (int i = 0; i < K.size(); ++i)
            if (std::fabs(K[i].time - rel) < 1e-9) { at = i; break; }
        if (at >= 0) {
            K.removeAt(at);
            m_canvas->clearSelection();
        } else {
            const double v = kfValue(K, baseForCamera(m_mesaCamera, p), rel);
            m_canvas->addKeyframe(rel, v);
        }
    } else if (m_mesaMode && m_mesaTrack) {
        QVector<Keyframe>& K = *keysForTrack(m_mesaTrack, p);
        const double t0 = std::max(0.0, m_playhead);
        const double rel = std::round(t0 / fr) * fr;
        int at = -1;
        for (int i = 0; i < K.size(); ++i)
            if (std::fabs(K[i].time - rel) < 1e-9) { at = i; break; }
        if (at >= 0) {
            K.removeAt(at);
            m_canvas->clearSelection();
        } else {
            const double v = kfValue(K, baseForTrack(m_mesaTrack, p), rel);
            m_canvas->addKeyframe(rel, v);
        }
    } else {
        Clip* c = activeClip();
        QVector<Keyframe>* kp = keysForActive(p);
        if (!c || !kp) return;
        QVector<Keyframe>& K = *kp;
        const double rel0 = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        const double rel = std::round(rel0 / fr) * fr;
        int at = -1;
        for (int i = 0; i < K.size(); ++i)
            if (std::fabs(K[i].time - rel) < 1e-9) { at = i; break; }
        if (at >= 0) {
            K.removeAt(at);
            m_canvas->clearSelection();
        } else {
            const double v = kfValue(K, baseForActive(p), rel);
            m_canvas->addKeyframe(rel, v);
        }
    }
    m_canvas->fitValueRange();
    syncRowData();
    m_canvas->commitChange();
}

void GraphEditorWidget::jumpKeyframe(GraphProp p, int dir) {
    if (m_camMode && m_mesaCamera) {
        const QVector<Keyframe>* K = keysForCamera(m_mesaCamera, p);
        if (!K || K->isEmpty()) return;
        const double t = std::max(0.0, m_playhead);
        int best = -1;
        if (dir < 0) {
            for (int i = 0; i < K->size(); ++i)
                if ((*K)[i].time < t - 1e-9) best = i;
            if (best < 0) best = K->size() - 1;
        } else {
            for (int i = K->size() - 1; i >= 0; --i)
                if ((*K)[i].time > t + 1e-9) best = i;
            if (best < 0) best = 0;
        }
        emit keyframeJump((*K)[best].time);
        return;
    }
    if (m_mesaMode && m_mesaTrack) {
        const QVector<Keyframe>* K = keysForTrack(m_mesaTrack, p);
        if (!K || K->isEmpty()) return;
        const double t = std::max(0.0, m_playhead);
        int best = -1;
        if (dir < 0) {
            for (int i = 0; i < K->size(); ++i)
                if ((*K)[i].time < t - 1e-9) best = i;
            if (best < 0) best = K->size() - 1;
        } else {
            for (int i = K->size() - 1; i >= 0; --i)
                if ((*K)[i].time > t + 1e-9) best = i;
            if (best < 0) best = 0;
        }
        emit keyframeJump((*K)[best].time);
        return;
    }
    Clip* c = activeClip();
    if (!c) return;
    const QVector<Keyframe>* K = keysFor(c, p);
    if (!K || K->isEmpty()) return;
    const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    int best = -1;
    if (dir < 0) {
        for (int i = 0; i < K->size(); ++i)
            if ((*K)[i].time < rel - 1e-9) best = i;
        if (best < 0) best = K->size() - 1;
    } else {
        for (int i = K->size() - 1; i >= 0; --i)
            if ((*K)[i].time > rel + 1e-9) best = i;
        if (best < 0) best = 0;
    }
    emit keyframeJump(c->pos + (*K)[best].time);
}
