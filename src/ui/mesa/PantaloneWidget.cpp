// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/mesa/PantaloneWidget.h"
#include "ui/core/Theme.h"
#include "ui/monitor/UiDecode.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/generators.h"
#include "colombina/render/TextRender.h"

#include <QPainter>
#include <QPainterPath>
#include <QGroupBox>
#include <QPushButton>
#include <QToolButton>
#include <QSlider>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QShortcut>
#include <QKeySequence>
#include <QScrollArea>
#include <QListWidget>
#include <QCheckBox>
#include <QComboBox>
#include <functional>
#include <algorithm>
#include <cmath>

#include "ui/mesa/PantalonePriv.h"


PantaloneWidget::PantaloneWidget(QWidget* parent) : QWidget(parent) {
    // Este decoder roda SÍNCRONO no thread da UI, um quadro por chamada. O
    // ganho do VAAPI é nulo nesse uso e o device é compartilhado por processo
    // com os workers de preview — conflito que derrubava o app. Fica em
    // software; a aceleração real acontece no preview/export.
    m_decoder.setHardwareDecodeAllowed(false);
    setMinimumSize(360, 280);
    setObjectName(QStringLiteral("pantaloneRoot"));

    auto makeSlider = [this](int min, int max) {
        auto* s = new QSlider(Qt::Horizontal, this);
        s->setRange(min, max);
        s->setMinimumWidth(70);
        return s;
    };
    auto makeValLabel = [this]() {
        auto* l = new QLabel(this);
        l->setMinimumWidth(40);
        l->setMaximumWidth(56);
        l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return l;
    };
    auto makeDiamond = [this](int prop) {
        auto* b = new QPushButton(QStringLiteral("◆"), this);
        b->setFixedSize(24, 24);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tr("Alternar keyframe no playhead"));
        b->setStyleSheet(QStringLiteral(
            "QPushButton{color:%1; border:none; background:transparent; font-size:14px;}"
            "QPushButton:hover{color:%2;}")
            .arg(themeColors().iconMuted.name(),
                 themeColors().accent.name()));
        connect(b, &QPushButton::clicked, this, [this, prop]() { toggleKeyframe(prop); });
        m_kfDiamonds[prop] = b;
        return b;
    };
    auto makeReset = [this](int prop) {
        auto* b = new QToolButton(this);
        b->setText(QStringLiteral("↺"));
        b->setFixedSize(24, 24);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tr("Redefinir esta propriedade"));
        b->setStyleSheet(QStringLiteral(
            "QToolButton{color:%1; border:none; background:transparent; font-size:13px;}"
            "QToolButton:hover{color:%2;}")
            .arg(themeColors().iconMuted.name(),
                 themeColors().accent.name()));
        m_resetBtns[prop] = b;
        return b;
    };

    m_cropL = makeSlider(0, kCropMax);
    m_cropR = makeSlider(0, kCropMax);
    m_cropT = makeSlider(0, kCropMax);
    m_cropB = makeSlider(0, kCropMax);
    m_scale = makeSlider((int)(kMinScale * 100.0), (int)(kMaxScale * 100.0));
    m_scale->setValue(100);
    m_panX = makeSlider(-kPanMax, kPanMax);
    m_panY = makeSlider(-kPanMax, kPanMax);
    m_rotation = makeSlider(-kRotMax, kRotMax);
    m_scaleX = makeSlider(kMinStretch, kMaxStretch);
    m_scaleX->setValue((int)std::lround(kStretchCenter * 100.0));
    m_scaleY = makeSlider(kMinStretch, kMaxStretch);
    m_scaleY->setValue((int)std::lround(kStretchCenter * 100.0));
    m_anchorX = makeSlider(-100, 100);
    m_anchorY = makeSlider(-100, 100);
    m_opacity = makeSlider(0, 100);
    m_opacity->setValue(100);

    m_cropLVal = makeValLabel();
    m_cropRVal = makeValLabel();
    m_cropTVal = makeValLabel();
    m_cropBVal = makeValLabel();
    m_scaleVal = makeValLabel();
    m_panXVal = makeValLabel();
    m_panYVal = makeValLabel();
    m_rotationVal = makeValLabel();
    m_scaleXVal = makeValLabel();
    m_scaleYVal = makeValLabel();
    m_anchorXVal = makeValLabel();
    m_anchorYVal = makeValLabel();
    m_opacityVal = makeValLabel();

    auto addValRow = [&](QGridLayout* g, int r, const QString& label,
                         QSlider* s, QLabel* v, int prop) {
        auto* lab = new QLabel(label, this);
        lab->setMinimumWidth(54);
        lab->setMaximumWidth(96);
        lab->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        lab->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        g->addWidget(lab, r, 0);
        g->addWidget(s, r, 1);
        g->addWidget(v, r, 2);
        g->addWidget(makeReset(prop), r, 3);
        g->addWidget(makeDiamond(prop), r, 4);
    };

    auto makeSection = [this](const QString& title) {
        CollapsibleSection sec;
        sec.container = new QWidget(this);
        auto* v = new QVBoxLayout(sec.container);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(1);
        sec.header = new QToolButton(sec.container);
        sec.header->setObjectName(QStringLiteral("sectionHeader"));
        sec.header->setText(title);
        sec.header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        sec.header->setArrowType(Qt::DownArrow);
        sec.header->setCheckable(true);
        sec.header->setChecked(true);
        sec.header->setAutoRaise(true);
        sec.header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        sec.body = new QWidget(sec.container);
        sec.grid = new QGridLayout(sec.body);
        sec.grid->setSpacing(4);
        sec.grid->setContentsMargins(14, 4, 14, 8);
        sec.grid->setColumnStretch(1, 1);
        v->addWidget(sec.header);
        v->addWidget(sec.body);
        connect(sec.header, &QToolButton::toggled, this, [sec](bool on) {
            sec.body->setVisible(on);
            sec.header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        });
        return sec;
    };

    CollapsibleSection cropSec = makeSection(tr("Recorte"));
    addValRow(cropSec.grid, 0, tr("Esquerda:"), m_cropL, m_cropLVal, P_CropL);
    addValRow(cropSec.grid, 1, tr("Direita:"), m_cropR, m_cropRVal, P_CropR);
    addValRow(cropSec.grid, 2, tr("Topo:"), m_cropT, m_cropTVal, P_CropT);
    addValRow(cropSec.grid, 3, tr("Base:"), m_cropB, m_cropBVal, P_CropB);

    CollapsibleSection moveSec = makeSection(tr("Zoom e Posição"));
    addValRow(moveSec.grid, 0, tr("Zoom:"), m_scale, m_scaleVal, P_Scale);
    addValRow(moveSec.grid, 1, tr("Pan X:"), m_panX, m_panXVal, P_PanX);
    addValRow(moveSec.grid, 2, tr("Pan Y:"), m_panY, m_panYVal, P_PanY);

    CollapsibleSection rotSec = makeSection(tr("Transformação"));
    addValRow(rotSec.grid, 0, tr("Ângulo:"), m_rotation, m_rotationVal, P_Rotation);
    addValRow(rotSec.grid, 1, tr("Flatt X:"), m_scaleX, m_scaleXVal, P_ScaleX);
    addValRow(rotSec.grid, 2, tr("Flatt Y:"), m_scaleY, m_scaleYVal, P_ScaleY);
    addValRow(rotSec.grid, 3, tr("Âncora X:"), m_anchorX, m_anchorXVal, P_AnchorX);
    addValRow(rotSec.grid, 4, tr("Âncora Y:"), m_anchorY, m_anchorYVal, P_AnchorY);
    // Opacidade do clipe (0..100%) — Clip::opacity/kfOpacity; idéia do
    // PR #10 de Gumduckk, reaplicada na árvore atual.
    addValRow(rotSec.grid, 5, tr("Opacidade:"), m_opacity, m_opacityVal, P_Opacity);

    // ── Máscaras (retângulo/elipse) ────────────────────────────────────
    // A máscara fica ancorada ao quadro (0..1) e aparece como gizmo no
    // viewfinder, ao lado do box de recorte — o mesmo modelo Mask/preview.
    CollapsibleSection maskSec = makeSection(tr("Máscaras"));
    m_maskList = new QListWidget(maskSec.body);
    m_maskList->setFixedHeight(64);
    m_maskList->setToolTip(tr("Máscaras do clipe. A selecionada vira gizmo "
                              "editável no viewfinder."));
    maskSec.grid->addWidget(m_maskList, 0, 0, 1, 5);

    auto* addRectBtn = new QPushButton(tr("+ Retângulo"), maskSec.body);
    auto* addEllBtn = new QPushButton(tr("+ Elipse"), maskSec.body);
    m_maskDelBtn = new QToolButton(maskSec.body);
    m_maskDelBtn->setText(QStringLiteral("Remover"));
    m_maskDelBtn->setToolTip(tr("Remover a máscara selecionada"));
    auto* maskBtns = new QHBoxLayout;
    maskBtns->setSpacing(3);
    maskBtns->addWidget(addRectBtn);
    maskBtns->addWidget(addEllBtn);
    maskBtns->addStretch(1);
    maskBtns->addWidget(m_maskDelBtn);
    maskSec.grid->addLayout(maskBtns, 1, 0, 1, 5);

    m_maskEnabled = new QCheckBox(tr("Habilitada"), maskSec.body);
    m_maskInvert = new QCheckBox(tr("Inverter"), maskSec.body);
    auto* maskFlags = new QHBoxLayout;
    maskFlags->setSpacing(10);
    maskFlags->addWidget(m_maskEnabled);
    maskFlags->addWidget(m_maskInvert);
    maskFlags->addStretch(1);
    maskSec.grid->addLayout(maskFlags, 2, 0, 1, 5);

    auto* feLab = new QLabel(tr("Suavidade:"), maskSec.body);
    feLab->setMinimumWidth(54);
    m_maskFeather = new QSlider(Qt::Horizontal, maskSec.body);
    m_maskFeather->setRange(0, 25); // 0..0.25 (fração do quadro)
    m_maskFeather->setMinimumWidth(70);
    m_maskFeatherVal = makeValLabel();
    auto* feRow = new QHBoxLayout;
    feRow->addWidget(feLab);
    feRow->addWidget(m_maskFeather, 1);
    feRow->addWidget(m_maskFeatherVal);
    maskSec.grid->addLayout(feRow, 3, 0, 1, 5);

    // Keyframes da máscara: um losango por propriedade (centro, tamanho,
    // rotação, suavidade), estilo AE/DaVinci. Com "● Auto" ligado, arrastar o
    // gizmo grava sozinho — o losango serve pra ligar/desligar na mão.
    auto* kfMaskRow = new QHBoxLayout;
    kfMaskRow->setSpacing(2);
    kfMaskRow->addWidget(new QLabel(tr("Keyframes:"), maskSec.body));
    auto addMaskKf = [&](int prop, const QString& tip) {
        QPushButton* b = makeDiamond(prop);
        b->setToolTip(tip);
        b->setFixedSize(20, 20);
        kfMaskRow->addWidget(b);
    };
    addMaskKf(P_MaskCx, tr("Keyframe: centro X"));
    addMaskKf(P_MaskCy, tr("Keyframe: centro Y"));
    addMaskKf(P_MaskRx, tr("Keyframe: largura"));
    addMaskKf(P_MaskRy, tr("Keyframe: altura"));
    addMaskKf(P_MaskRot, tr("Keyframe: rotação"));
    addMaskKf(P_MaskFeather, tr("Keyframe: suavidade"));
    kfMaskRow->addStretch(1);
    maskSec.grid->addLayout(kfMaskRow, 4, 0, 1, 5);

    connect(m_maskList, &QListWidget::currentRowChanged, this, [this](int row) {
        m_maskSel = row;
        syncMaskControls();
        m_view->update();
    });
    connect(addRectBtn, &QPushButton::clicked, this,
            [this]() { addMask(QStringLiteral("rect")); });
    connect(addEllBtn, &QPushButton::clicked, this,
            [this]() { addMask(QStringLiteral("ellipse")); });
    connect(m_maskDelBtn, &QToolButton::clicked, this,
            [this]() { removeSelectedMask(); });
    connect(m_maskEnabled, &QCheckBox::toggled, this, [this](bool on) {
        Clip* c = activeClip();
        if (!c || m_maskSel < 0 || m_maskSel >= c->masks.size()) return;
        emitChange();
        c->masks[m_maskSel].enabled = on;
        refreshMaskList();
        m_view->update();
    });
    connect(m_maskInvert, &QCheckBox::toggled, this, [this](bool on) {
        Clip* c = activeClip();
        if (!c || m_maskSel < 0 || m_maskSel >= c->masks.size()) return;
        emitChange();
        c->masks[m_maskSel].invert = on;
        m_view->update();
    });
    connect(m_maskFeather, &QSlider::sliderReleased, this,
            [this]() { m_undoPushed = false; });
    connect(m_maskFeather, &QSlider::valueChanged, this, [this](int v) {
        Clip* c = activeClip();
        if (!c || m_maskSel < 0 || m_maskSel >= c->masks.size()) return;
        emitChange();
        c->masks[m_maskSel].feather = v / 100.0;
        if (m_maskFeatherVal)
            m_maskFeatherVal->setText(QString::number(v / 100.0, 'f', 2));
        if (m_kfAuto && m_kfAuto->isChecked()) {
            writeKeyframe(P_MaskFeather, v / 100.0);
            refreshDiamonds();
        }
        m_view->update();
    });

    // Conecta sliders → commitSlider; os labels de valor são atualizados junto.
    auto connectSlider = [&](QSlider* s, int prop, std::function<double()> toBase) {
        connect(s, &QSlider::sliderReleased, this, [this]() { m_undoPushed = false; });
        connect(s, &QSlider::valueChanged, this, [this, s, prop, toBase]() {
            Clip* c = activeClip();
            if (!c) return;
            commitSlider(prop, toBase());
            updateValueLabels();
            m_view->update();
        });
    };

    connectSlider(m_cropL, P_CropL, [this]() { return m_cropL->value() / 100.0; });
    connectSlider(m_cropR, P_CropR, [this]() { return m_cropR->value() / 100.0; });
    connectSlider(m_cropT, P_CropT, [this]() { return m_cropT->value() / 100.0; });
    connectSlider(m_cropB, P_CropB, [this]() { return m_cropB->value() / 100.0; });
    connectSlider(m_scale, P_Scale, [this]() { return m_scale->value() / 100.0; });
    connectSlider(m_panX, P_PanX, [this]() {
        const double W = m_project ? m_project->width : 1920.0;
        return m_panX->value() / 100.0 * W;
    });
    connectSlider(m_panY, P_PanY, [this]() {
        const double H = m_project ? m_project->height : 1080.0;
        return m_panY->value() / 100.0 * H;
    });
    connectSlider(m_rotation, P_Rotation, [this]() {
        return (double)m_rotation->value();
    });
    connectSlider(m_scaleX, P_ScaleX, [this]() { return m_scaleX->value() / 100.0; });
    connectSlider(m_scaleY, P_ScaleY, [this]() { return m_scaleY->value() / 100.0; });
    connectSlider(m_anchorX, P_AnchorX, [this]() {
        return m_anchorX->value() / 100.0;
    });
    connectSlider(m_anchorY, P_AnchorY, [this]() {
        return m_anchorY->value() / 100.0;
    });
    connectSlider(m_opacity, P_Opacity, [this]() {
        return m_opacity->value() / 100.0;
    });

    // Conecta botões de reset por propriedade.
    auto connectReset = [this](int prop, QSlider* s, double defVal,
                               std::function<void(Clip*)> resetFn) {
        if (QToolButton* btn = m_resetBtns.value(prop)) {
            connect(btn, &QToolButton::clicked, this, [this, prop, s, defVal, resetFn]() {
                Clip* c = activeClip();
                if (!c) return;
                if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
                resetFn(c);
                QSignalBlocker bs(s);
                s->setValue((int)std::lround(defVal * 100.0));
                if (m_kfAuto && m_kfAuto->isChecked())
                    writeKeyframe(prop, defVal);
                emit modified();
                refreshDiamonds();
                updateValueLabels();
                m_view->update();
            });
        }
    };
    connectReset(P_CropL, m_cropL, 0.0, [](Clip* c) { c->cropL = 0.0; c->kfCropL.clear(); });
    connectReset(P_CropR, m_cropR, 0.0, [](Clip* c) { c->cropR = 0.0; c->kfCropR.clear(); });
    connectReset(P_CropT, m_cropT, 0.0, [](Clip* c) { c->cropT = 0.0; c->kfCropT.clear(); });
    connectReset(P_CropB, m_cropB, 0.0, [](Clip* c) { c->cropB = 0.0; c->kfCropB.clear(); });
    connectReset(P_Scale, m_scale, 1.0, [](Clip* c) { c->scale = 1.0; c->kfScale.clear(); });
    connectReset(P_PanX, m_panX, 0.0, [](Clip* c) { c->tx = 0.0; c->kfTx.clear(); });
    connectReset(P_PanY, m_panY, 0.0, [](Clip* c) { c->ty = 0.0; c->kfTy.clear(); });
    connectReset(P_Rotation, m_rotation, 0.0, [](Clip* c) { c->rotation = 0.0; c->kfRotation.clear(); });
    connectReset(P_ScaleX, m_scaleX, 1.0, [](Clip* c) { c->scaleX = 1.0; c->kfScaleX.clear(); });
    connectReset(P_ScaleY, m_scaleY, 1.0, [](Clip* c) { c->scaleY = 1.0; c->kfScaleY.clear(); });
    connectReset(P_AnchorX, m_anchorX, 0.0, [](Clip* c) { c->anchorX = 0.0; c->kfAnchorX.clear(); });
    connectReset(P_AnchorY, m_anchorY, 0.0, [](Clip* c) { c->anchorY = 0.0; c->kfAnchorY.clear(); });
    connectReset(P_Opacity, m_opacity, 1.0, [](Clip* c) { c->opacity = 1.0; c->kfOpacity.clear(); });

    auto* resetBtn = new QPushButton(tr("Redefinir Tudo"), this);
    resetBtn->setMinimumHeight(22);
    resetBtn->setToolTip(tr("Zera recorte, zoom, posição, rotação, achatamento, "
                            "âncora e opacidade do clipe"));
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB),
                       b5(m_scale), b6(m_panX), b7(m_panY), b8(m_rotation),
                       b9(m_scaleX), b10(m_scaleY), b11(m_anchorX), b12(m_anchorY),
                       b13(m_opacity);
        m_cropL->setValue(0); m_cropR->setValue(0);
        m_cropT->setValue(0); m_cropB->setValue(0);
        m_scale->setValue(100); m_panX->setValue(0); m_panY->setValue(0);
        m_rotation->setValue(0);
        m_scaleX->setValue((int)std::lround(kStretchCenter * 100.0));
        m_scaleY->setValue((int)std::lround(kStretchCenter * 100.0));
        m_anchorX->setValue(0); m_anchorY->setValue(0);
        m_opacity->setValue(100);
        updateValueLabels();
        Clip* c = activeClip();
        if (c) {
            c->kfCropL.clear(); c->kfCropR.clear();
            c->kfCropT.clear(); c->kfCropB.clear();
            c->cropL = c->cropR = c->cropT = c->cropB = 0.0;
            c->kfScale.clear();
            c->scale = 1.0;
            c->kfTx.clear(); c->kfTy.clear();
            c->tx = c->ty = 0.0;
            c->kfRotation.clear();
            c->rotation = 0.0;
            c->kfScaleX.clear(); c->kfScaleY.clear();
            c->scaleX = c->scaleY = 1.0;
            c->kfAnchorX.clear(); c->kfAnchorY.clear();
            c->anchorX = c->anchorY = 0.0;
            c->kfOpacity.clear();
            c->opacity = 1.0;
        }
        m_undoPushed = false;
        if (c && c->isText) loadFrame();
        m_view->update();
        if (c) emitChange();
    });

    auto* kfBar = new QHBoxLayout;
    kfBar->setSpacing(4);
    auto* prevKf = new QPushButton(tr("◀"), this);
    prevKf->setFixedSize(26, 24);
    prevKf->setToolTip(tr("Keyframe anterior"));
    connect(prevKf, &QPushButton::clicked, this, [this]() { gotoKeyframe(-1); });
    auto* nextKf = new QPushButton(tr("▶"), this);
    nextKf->setFixedSize(26, 24);
    nextKf->setToolTip(tr("Próximo keyframe"));
    connect(nextKf, &QPushButton::clicked, this, [this]() { gotoKeyframe(1); });
    m_kfAuto = new QPushButton(tr("● Auto"), this);
    m_kfAuto->setCheckable(true);
    m_kfAuto->setChecked(true);
    m_kfAuto->setToolTip(tr("Auto-keyframe: alterar um valor cria/atualiza o "
                            "keyframe no playhead"));
    kfBar->addWidget(prevKf);
    kfBar->addWidget(nextKf);
    kfBar->addStretch(1);
    kfBar->addWidget(m_kfAuto);

    m_view = new Viewport(this);
    m_strip = new KeyframeStrip(this);
    m_strip->setToolTip(tr("Arraste um keyframe para a esquerda/direita para "
                           "mover sua posição no tempo"));

    // ── Malha (resolução da grade) ─────────────────────────────────────
    CollapsibleSection meshSec = makeSection(tr("Malha"));
    auto* resLab = new QLabel(tr("Grade:"), meshSec.body);
    m_meshRes = new QComboBox(meshSec.body);
    for (const char* r : { "2×2", "3×2", "3×3", "4×4", "5×5" })
        m_meshRes->addItem(QString::fromUtf8(r));
    auto* resRow = new QHBoxLayout;
    resRow->addWidget(resLab);
    resRow->addWidget(m_meshRes, 1);
    meshSec.grid->addLayout(resRow, 0, 0, 1, 5);
    auto* meshReset = new QPushButton(tr("Redefinir malha"), meshSec.body);
    meshSec.grid->addWidget(meshReset, 1, 0, 1, 5);
    connect(m_meshRes, &QComboBox::currentTextChanged, this, [this](const QString& t) {
        Clip* c = activeClip();
        if (!c) return;
        int cols = 3, rows = 3;
        if (t == QLatin1String("2×2")) { cols = 2; rows = 2; }
        else if (t == QLatin1String("3×2")) { cols = 3; rows = 2; }
        else if (t == QLatin1String("3×3")) { cols = 3; rows = 3; }
        else if (t == QLatin1String("4×4")) { cols = 4; rows = 4; }
        else if (t == QLatin1String("5×5")) { cols = 5; rows = 5; }
        emitChange();
        c->enableDefaultMesh(cols, rows);
        m_meshDrag = -1;
        m_view->update();
    });
    connect(meshReset, &QPushButton::clicked, this, [this]() {
        Clip* c = activeClip();
        if (!c) return;
        emitChange();
        c->enableDefaultMesh(c->meshCols, c->meshRows);
        m_meshDrag = -1;
        m_view->update();
    });

    // ── Barra lateral (recortável): ajuste fino numérico ──────────────
    // Fica estreita à direita; o viewer domina. Pode ser recolhida pelo » .
    auto* sidebar = new QWidget(this);
    auto* sbLay = new QVBoxLayout(sidebar);
    sbLay->setContentsMargins(0, 0, 0, 0);
    sbLay->setSpacing(5);
    sbLay->addWidget(cropSec.container);
    sbLay->addWidget(moveSec.container);
    sbLay->addWidget(rotSec.container);
    sbLay->addWidget(meshSec.container);
    sbLay->addWidget(resetBtn);
    sbLay->addStretch(1);

    m_secCrop = cropSec.container;
    m_secMove = moveSec.container;
    m_secRot = rotSec.container;
    m_secMesh = meshSec.container;

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(sidebar);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(0);
    scroll->setMaximumWidth(250);

    auto* sbToggle = new QToolButton(this);
    sbToggle->setText(QStringLiteral("»"));
    sbToggle->setToolTip(tr("Recolher/ampliar painel de propriedades"));
    sbToggle->setCheckable(true);
    sbToggle->setAutoRaise(true);
    sbToggle->setFixedSize(16, 26);
    connect(sbToggle, &QToolButton::toggled, this, [scroll, sbToggle](bool collapsed) {
        scroll->setVisible(!collapsed);
        sbToggle->setText(collapsed ? QStringLiteral("«") : QStringLiteral("»"));
    });

    // Topo: o viewer domina; sidebar estreita à direita.
    auto* top = new QWidget(this);
    auto* topLay = new QHBoxLayout(top);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(4);
    topLay->addWidget(m_view, 1);
    topLay->addWidget(sbToggle, 0, Qt::AlignTop);
    topLay->addWidget(scroll, 0);

    // ── Faixa inferior: máscaras (esq) | keyframes/timeline (dir) ─────
    auto* bottom = new QWidget(this);
    auto* botLay = new QHBoxLayout(bottom);
    botLay->setContentsMargins(0, 0, 0, 0);
    botLay->setSpacing(10);
    maskSec.container->setMaximumWidth(230); // máscara compacta; timeline domina
    botLay->addWidget(maskSec.container, 1);
    auto* tlCol = new QVBoxLayout;
    tlCol->setContentsMargins(0, 0, 0, 0);
    tlCol->setSpacing(3);
    tlCol->addLayout(kfBar);
    tlCol->addWidget(m_strip, 1);
    botLay->addLayout(tlCol, 5);

    // ── Barra de modos do viewer ──────────────────────────────────────
    auto* modeBar = new QHBoxLayout;
    modeBar->setSpacing(2);
    auto makeMode = [&](const QString& label, ViewMode m, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(label);
        b->setCheckable(true);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setObjectName(QStringLiteral("modeBtn"));
        connect(b, &QToolButton::clicked, this, [this, m]() { setViewMode(m); });
        m_modeBtns[(int)m] = b;
        modeBar->addWidget(b);
    };
    makeMode(tr("Recortar"), ModeCrop, tr("Só as alças de recorte"));
    makeMode(tr("Transformar"), ModeTransform,
             tr("Zoom, posição, rotação e âncora"));
    makeMode(tr("Janelas"), ModeMask, tr("Máscaras/janelas do clipe"));
    makeMode(tr("Malha"), ModeMesh, tr("Malha 3×3 para espremer/esticar a mídia"));
    modeBar->addStretch(1);

    // Raiz: vertical [ modos | viewer/sidebar | máscaras/timeline ].
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(5);
    lay->addLayout(modeBar);
    lay->addWidget(top, 1);
    lay->addWidget(bottom, 0);

    // Atalhos de teclado (setas para ajustar valores).
    const auto& tc = themeColors();
    // Painel no estilo Premiere "Effect Controls": fundo neutro plano, seções
    // discretas com triângulo de recolher e divisor sutil, slider fino com
    // preenchimento de destaque e valores como label sem caixa.
    setStyleSheet(QStringLiteral(
        // Fundo só no widget raiz (objectName) e no scroll: a regra genérica
        // "QWidget" pintava #161616 por cima de TODOS os filhos — inclusive do
        // editor interno dos spinboxes — e escondia os valores no tema escuro.
        "#pantaloneRoot{background:%1;}"
        "QScrollArea{background:%1; border:none;}"
        "QScrollArea > QWidget{background:%1;}"
        "QScrollArea > QWidget > QWidget{background:%1;}"

        "QToolButton#sectionHeader{background:transparent; border:none;"
        " border-bottom:1px solid %2; color:%3; font-weight:bold;"
        " font-size:11px; padding:5px 6px; text-align:left;}"
        "QToolButton#sectionHeader:hover{color:%4; border-bottom:1px solid %5;}"
        "QToolButton#sectionHeader:checked{color:%4;}"
        "QPushButton{border:none; background:transparent; color:%6;"
        " padding:2px 8px; font-size:11px;}"
        "QPushButton:hover{background:%7; color:%4;}"
        "QPushButton:pressed{background:%8;}"
        "QPushButton:checked{color:%5;}"
        "QToolButton{border:none; background:transparent; color:%6;"
        " padding:2px 8px; font-size:11px;}"
        "QToolButton:hover{background:%7; color:%4;}"
        "QToolButton:pressed{background:%8;}"
        "QToolButton:checked{color:%5;}"
        "QToolButton#modeBtn{padding:3px 11px; border-radius:4px;}"
        "QToolButton#modeBtn:checked{background:%5; color:%4;}"
        "QSlider::groove:horizontal{height:2px; background:%2;}"
        "QSlider::sub-page:horizontal{background:%5;}"
        "QSlider::handle:horizontal{width:9px; height:9px; margin:-4px 0;"
        " border-radius:5px; background:%9; border:1px solid %5;}"
        "QSlider::handle:horizontal:hover{background:%10;}"
        "QLabel{color:%6; font-size:11px;}")
        .arg(tc.pantaloneBg.name(),       // %1 fundo do painel
             tc.trackBorder.name(),     // %2 divisor / groove
             tc.text.name(),            // %3 título da seção
             QColor(Qt::white).name(),  // %4 hover / ativo
             tc.accent.name(),          // %5 destaque / preenchimento
             tc.iconNormal.name(),      // %6 labels / botões
             tc.btnHover.name(),        // %7 hover de botão
             tc.trackLabelBg.name(),    // %8 pressed
             QColor(Qt::white).name())  // %9 alça do slider
        .arg(tc.spinText.name()));      // %10 alça hover

    refreshDiamonds();
    setViewMode(m_mode); // sincroniza o estado dos botões de modo
    m_strip->update();
    m_view->update();
}

