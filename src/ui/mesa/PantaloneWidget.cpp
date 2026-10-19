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

namespace {
constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 4.0;
constexpr int kMinStretch = 20;   // % (0.2×) do achatamento/esticamento não-uniforme
constexpr int kMaxStretch = 300;  // % (3.0×)
constexpr int kCropMax = 100;  // %
constexpr int kPanMax = 100;   // % da largura/altura do projeto
constexpr int kRotMax = 180;   // graus (±)
constexpr double kStretchCenter = 1.0; // neutro (1.0×)

// Renderiza um TextStyle sobre um frame transparente (usado no pantalone de
// clipes de texto, que não têm quadro decodificado). Aplica o transform do
// clipe (pan/zoom/rotação) para o texto aparecer ONDE o preview mostra —
// antes ficava centralizado no pantalone mesmo com o clipe deslocado.
void renderTextStyleFrame(QImage& img, const TextStyle& st,
                          double tx, double ty, double rotDeg, double s,
                          double sX = 1.0, double sY = 1.0) {
    const double W = img.width();
    const double H = img.height();
    const textrender::Layout L = textrender::layout(st, W, H);
    const QRectF box = textrender::centeredBox(st, L, W, H);

    QPainter p(&img);
    p.translate(W / 2.0 + tx, H / 2.0 + ty);
    p.rotate(rotDeg);
    p.scale(s * sX, s * sY);
    textrender::paint(p, st, L, box, H);
}
constexpr int kRotStep = 5;    // graus por tick da roda (Alt+roda)

// Pontos de controle da máscara em px do quadro, já rotacionados, a partir dos
// valores RESOLVIDOS (normalizados 0..1 + rotação em graus):
// [0]=centro, [1]=alça de rotação, [2..]=bordas (cantos p/ retângulo,
// cardeais p/ elipse). O mesmo layout do previewMaskCtrl, para o hit-test e o
// desenho do Pantalone batem com o overlay do preview.
QVector<QPointF> maskCtrlPoints(const QString& type, double cxN, double cyN,
                                double rxN, double ryN, double rotDeg,
                                int w0, int h0) {
    const double cx = cxN * w0, cy = cyN * h0;
    const double rx = std::max(0.001, rxN) * w0;
    const double ry = std::max(0.001, ryN) * h0;
    const double rad = rotDeg * M_PI / 180.0;
    QVector<QPointF> base;
    if (type == QLatin1String("rect"))
        base = { QPointF(cx - rx, cy - ry), QPointF(cx + rx, cy - ry),
                 QPointF(cx + rx, cy + ry), QPointF(cx - rx, cy + ry) };
    else
        base = { QPointF(cx - rx, cy), QPointF(cx, cy - ry),
                 QPointF(cx + rx, cy), QPointF(cx, cy + ry) };
    if (rad != 0.0) {
        const double cs = std::cos(rad), sn = std::sin(rad);
        for (QPointF& b : base) {
            const double dx = b.x() - cx, dy = b.y() - cy;
            b = QPointF(cx + dx * cs - dy * sn, cy + dx * sn + dy * cs);
        }
    }
    const double arm = std::max(1.0, std::min(rx, ry));
    QVector<QPointF> pts;
    pts << QPointF(cx, cy)
        << QPointF(cx - arm * std::sin(rad), cy - arm * std::cos(rad));
    for (const QPointF& b : base) pts << b;
    return pts;
}

// Seção recolhível estilo "Effect Controls" do Premiere: um cabeçalho com seta
// de expansão que esconde/mostra o corpo (grade de sliders).
struct CollapsibleSection {
    QWidget* container = nullptr;
    QWidget* body = nullptr;
    QToolButton* header = nullptr;
    QGridLayout* grid = nullptr;
};

// Presets de suavização "estilo Vegas" aplicados ao segmento que sai do
// keyframe (ou, no último, ao segmento que chega).
void applyEasePreset(QVector<Keyframe>& kf, int i, int preset) {
    Keyframe* k = &kf[i];
    if (i + 1 < kf.size()) {
        const double span = kf[i + 1].time - k->time;
        const double delta = kf[i + 1].value - k->value;
        switch (preset) {
            case 1: // Suave
                k->interp = KfSmooth; k->ox = k->oy = k->ix = k->iy = 0.0; break;
            case 2: // Rápido (começa rápido, desacelera)
                k->interp = KfBezier; k->ox = span / 3.0; k->oy = 0.85 * delta;
                k->ix = 0.0; k->iy = 0.0; break;
            case 3: // Lento (começa devagar, acelera)
                k->interp = KfBezier; k->ox = span / 3.0; k->oy = 0.15 * delta;
                k->ix = 0.0; k->iy = 0.0; break;
            case 4: // Acentuada (S acentuado no meio)
                k->interp = KfBezier; k->ox = span / 3.0; k->oy = 0.55 * delta;
                k->ix = 0.0; k->iy = 0.0; break;
            default: // Linear
                k->interp = KfLinear; break;
        }
    } else if (i > 0) {
        Keyframe* p = &kf[i - 1];
        const double span = k->time - p->time;
        const double delta = k->value - p->value;
        switch (preset) {
            case 1:
                p->interp = KfSmooth; p->ox = p->oy = p->ix = p->iy = 0.0; break;
            case 2:
                p->interp = KfBezier; p->ox = span / 3.0; p->oy = 0.85 * delta;
                p->ix = 0.0; p->iy = 0.0; break;
            case 3:
                p->interp = KfBezier; p->ox = span / 3.0; p->oy = 0.15 * delta;
                p->ix = 0.0; p->iy = 0.0; break;
            case 4:
                p->interp = KfBezier; p->ox = span / 3.0; p->oy = 0.55 * delta;
                p->ix = 0.0; p->iy = 0.0; break;
            default:
                p->interp = KfLinear; break;
        }
    }
}
}

// Viewfinder: widget que exibe o frame com a caixa de recorte/janela de saída
// e recebe os eventos de mouse/roda. Só delega para o PantaloneWidget dono.
class PantaloneWidget::Viewport : public QWidget {
public:
    explicit Viewport(PantaloneWidget* owner) : QWidget(owner), m_owner(owner) {
        setMinimumSize(180, 150);
    }
protected:
    void paintEvent(QPaintEvent*) override { m_owner->paintViewfinder(this); }
    void mousePressEvent(QMouseEvent* e) override { m_owner->viewportPress(this, e); }
    void mouseMoveEvent(QMouseEvent* e) override { m_owner->viewportMove(this, e); }
    void mouseReleaseEvent(QMouseEvent* e) override { m_owner->viewportRelease(this, e); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { m_owner->viewportDoubleClick(this, e); }
    void wheelEvent(QWheelEvent* e) override { m_owner->viewportWheel(this, e); }
    void contextMenuEvent(QContextMenuEvent* e) override { m_owner->viewportContextMenu(this, e); }
    void leaveEvent(QEvent*) override {
        if (m_owner->m_hoverMode != DragNone) {
            m_owner->m_hoverMode = DragNone;
            update();
        }
        setCursor(Qt::ArrowCursor);
    }
private:
    PantaloneWidget* m_owner;
};

// Faixa de tempo dos keyframes do clipe: mostra a posição dos keyframes
// (relativa ao início do clipe) e permite arrastá-los para a esquerda/direita.
class PantaloneWidget::KeyframeStrip : public QWidget {
public:
    explicit KeyframeStrip(PantaloneWidget* owner) : QWidget(owner), m_owner(owner) {
        setMinimumHeight(72); // 3 pistas (Recorte · Máscara · Resto)
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::ClickFocus);
    }
protected:
    void paintEvent(QPaintEvent*) override { m_owner->paintKeyframeStrip(this); }
    void mousePressEvent(QMouseEvent* e) override { m_owner->stripPress(this, e); }
    void mouseMoveEvent(QMouseEvent* e) override { m_owner->stripMove(this, e); }
    void mouseReleaseEvent(QMouseEvent* e) override { m_owner->stripRelease(this, e); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { m_owner->stripDoubleClick(this, e); }
    void contextMenuEvent(QContextMenuEvent* e) override { m_owner->stripContextMenu(this, e); }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
            m_owner->deleteSelectedKeyframes();
            e->accept();
        } else {
            QWidget::keyPressEvent(e);
        }
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ShortcutOverride) {
            const int key = static_cast<QKeyEvent*>(e)->key();
            // Impede o atalho global do MainWindow de apagar o clipe quando a
            // faixa de keyframes do pantalone está com foco.
            if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
                e->accept();
                return true;
            }
        }
        return QWidget::event(e);
    }
private:
    PantaloneWidget* m_owner;
};

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

    auto* resetBtn = new QPushButton(tr("Redefinir Tudo"), this);
    resetBtn->setMinimumHeight(22);
    resetBtn->setToolTip(tr("Zera recorte, zoom, posição, rotação, achatamento "
                            "e âncora do clipe"));
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB),
                       b5(m_scale), b6(m_panX), b7(m_panY), b8(m_rotation),
                       b9(m_scaleX), b10(m_scaleY), b11(m_anchorX), b12(m_anchorY);
        m_cropL->setValue(0); m_cropR->setValue(0);
        m_cropT->setValue(0); m_cropB->setValue(0);
        m_scale->setValue(100); m_panX->setValue(0); m_panY->setValue(0);
        m_rotation->setValue(0);
        m_scaleX->setValue((int)std::lround(kStretchCenter * 100.0));
        m_scaleY->setValue((int)std::lround(kStretchCenter * 100.0));
        m_anchorX->setValue(0); m_anchorY->setValue(0);
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

void PantaloneWidget::setProject(Project* p) {
    m_project = p;
    m_clipId.clear();
    m_frame = QImage();
    m_framePath.clear();
    m_undoPushed = false;
    m_maskSel = -1;
    m_maskDragHandle = -1;
    refreshMaskList();
    syncMaskControls();
    m_view->update();
}

void PantaloneWidget::setClipId(const QString& id) {
    if (m_clipId == id) return;
    m_clipId = id;
    m_undoPushed = false;
    m_maskDragHandle = -1;
    loadFrame();
    syncFromClip();
    refreshMaskList();
    syncMaskControls();
    m_view->update();
    m_strip->update();
}

void PantaloneWidget::setPlayhead(double t) {
    // Só recarrega o frame se a mudança de tempo for significativa (>50ms)
    // para evitar decodificar frame a frame durante a reprodução.
    const bool needReload = std::fabs(t - m_playhead) > 0.05 || m_frame.isNull();
    m_playhead = t;
    if (needReload) loadFrame();
    syncFromClip();
    m_view->update();
    m_strip->update();
}

void PantaloneWidget::refresh() {
    loadFrame();
    syncFromClip();
    m_view->update();
}

void PantaloneWidget::sync() {
    syncFromClip();
    m_view->update();
}

void PantaloneWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // MainWindow chama setClipId() ANTES de m_pantaloneDock->show(), então o
    // loadFrame() daquele momento foi descartado pela guarda de visibilidade.
    loadFrame();
    syncFromClip();
    m_view->update();
}

Clip* PantaloneWidget::activeClip() {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    return nullptr;
}

void PantaloneWidget::loadFrame() {
    // O dock do pantalone nasce escondido (MainWindow), mas continua ligado ao
    // playhead e à seleção da timeline. Sem esta guarda, TODO arasto do
    // playhead disparava uma decodificação VAAPI SÍNCRONA no thread da UI,
    // concorrente com os workers de preview — e derrubava o app com SIGSEGV
    // dentro de avcodec_send_packet. Escondido, o painel não desenha nada,
    // então não há nada a decodificar.
    if (!isVisible()) {
        m_frame = QImage();
        m_framePath.clear();
        return;
    }
    Clip* c = activeClip();
    if (!c) { m_frame = QImage(); m_framePath.clear(); return; }
    if (c->isText) {
        const TextStyle& st = *m_project->textStyleFor(*c);
        const int W = m_project ? m_project->width : 1920;
        const int H = m_project ? m_project->height : 1080;
        QImage img(W, H, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        const double rel = std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur));
        const double tx = kfValue(c->kfTx, c->tx, rel);
        const double ty = kfValue(c->kfTy, c->ty, rel);
        const double rot = kfValue(c->kfRotation, c->rotation, rel);
        const double s = kfValue(c->kfScale, c->scale, rel);
        const double sX = kfValue(c->kfScaleX, c->scaleX, rel);
        const double sY = kfValue(c->kfScaleY, c->scaleY, rel);
        renderTextStyleFrame(img, st, tx, ty, rot, s, sX, sY);
        m_frame = img;
        m_framePath.clear();
        return;
    }
    const MediaItem* mi = m_project->findMedia(c->mediaId);
    if (!mi || !mi->hasVideo) { m_frame = QImage(); m_framePath.clear(); return; }
    if (mi->isSolid) {
        // Mídia gerada (gerador estilo Vegas): gera o quadro sem decoder.
        const int w = qMax(1, mi->width > 0 ? mi->width : m_project->width);
        const int h = qMax(1, mi->height > 0 ? mi->height : m_project->height);
        m_frame = generatorFrame(*mi, w, h);
        m_framePath.clear();
        m_lastDecodedClipId = c->id;
        return;
    }
    const QString vpath = ProxyManager::instance().resolveVideo(mi->filePath);
    // Força reset do decoder ao trocar de clipe — mesmo que o arquivo fonte seja
    // o mesmo, o estado interno (m_lastFrame/m_nextFrame/m_lastPtsSec) fica do
    // clipe anterior e pode devolver o frame errado.
    if (m_lastDecodedClipId != c->id) {
        if (m_decoder.isOpen()) m_decoder.close();
        m_lastDecodedClipId = c->id;
    }
    // Decodifica o frame na posição atual do playhead para preview em tempo real.
    const double t = std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur - 0.05));
    // Ponto único de decode na UI (uidecode::decode): gate de visibilidade,
    // reabertura só na troca de arquivo e releaseBuffers() pós-frame — antes
    // essas regras moravam duplicadas aqui e no PivotCanvas.
    m_frame = uidecode::decode(this, m_decoder, vpath, t, 720);
    if (m_frame.isNull()) m_framePath.clear();
    else m_framePath = mi->filePath;
}

void PantaloneWidget::syncFromClip() {
    Clip* c = activeClip();
    const double rel = c ? std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur)) : 0.0;
    const double L = c ? std::clamp(kfValue(c->kfCropL, c->cropL, rel), 0.0, 0.9) : 0.0;
    const double R = c ? std::clamp(kfValue(c->kfCropR, c->cropR, rel), 0.0, 0.9) : 0.0;
    const double T = c ? std::clamp(kfValue(c->kfCropT, c->cropT, rel), 0.0, 0.9) : 0.0;
    const double B = c ? std::clamp(kfValue(c->kfCropB, c->cropB, rel), 0.0, 0.9) : 0.0;
    const double s = c ? kfValue(c->kfScale, c->scale, rel) : 1.0;
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double tx = c ? kfValue(c->kfTx, c->tx, rel) : 0.0;
    const double ty = c ? kfValue(c->kfTy, c->ty, rel) : 0.0;
    const double rot = c ? kfValue(c->kfRotation, c->rotation, rel) : 0.0;
    const double sx = c ? kfValue(c->kfScaleX, c->scaleX, rel) : 1.0;
    const double sy = c ? kfValue(c->kfScaleY, c->scaleY, rel) : 1.0;
    const double ax = c ? kfValue(c->kfAnchorX, c->anchorX, rel) : 0.0;
    const double ay = c ? kfValue(c->kfAnchorY, c->anchorY, rel) : 0.0;

    QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB);
    QSignalBlocker b5(m_scale), b6(m_panX), b7(m_panY), b8(m_rotation);
    QSignalBlocker b9(m_scaleX), b10(m_scaleY), b11(m_anchorX), b12(m_anchorY);
    m_cropL->setValue((int)std::lround(L * 100.0));
    m_cropR->setValue((int)std::lround(R * 100.0));
    m_cropT->setValue((int)std::lround(T * 100.0));
    m_cropB->setValue((int)std::lround(B * 100.0));
    m_scale->setValue((int)std::lround(std::clamp(s, kMinScale, kMaxScale) * 100.0));
    m_panX->setValue((int)std::lround(std::clamp(tx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(ty / H * 100.0, -100.0, 100.0)));
    m_rotation->setValue((int)std::lround(std::clamp(rot, -double(kRotMax), double(kRotMax))));
    m_scaleX->setValue((int)std::lround(std::clamp(sx, kMinStretch / 100.0, kMaxStretch / 100.0) * 100.0));
    m_scaleY->setValue((int)std::lround(std::clamp(sy, kMinStretch / 100.0, kMaxStretch / 100.0) * 100.0));
    m_anchorX->setValue((int)std::lround(std::clamp(ax * 100.0, -100.0, 100.0)));
    m_anchorY->setValue((int)std::lround(std::clamp(ay * 100.0, -100.0, 100.0)));
    updateValueLabels();
    refreshDiamonds();
}

void PantaloneWidget::updateValueLabels() {
    const int L = m_cropL->value();
    const int R = m_cropR->value();
    const int T = m_cropT->value();
    const int B = m_cropB->value();
    const int sc = m_scale->value();
    const int px = m_panX->value();
    const int py = m_panY->value();
    const int rot = m_rotation->value();
    const int sx = m_scaleX->value();
    const int sy = m_scaleY->value();
    const int ax = m_anchorX->value();
    const int ay = m_anchorY->value();
    auto pct = [](int v) { return QStringLiteral("%1%").arg(v); };
    if (m_cropLVal) m_cropLVal->setText(pct(L));
    if (m_cropRVal) m_cropRVal->setText(pct(R));
    if (m_cropTVal) m_cropTVal->setText(pct(T));
    if (m_cropBVal) m_cropBVal->setText(pct(B));
    if (m_scaleVal) m_scaleVal->setText(pct(sc));
    if (m_panXVal) m_panXVal->setText(pct(px));
    if (m_panYVal) m_panYVal->setText(pct(py));
    if (m_rotationVal) m_rotationVal->setText(QStringLiteral("%1°").arg(rot));
    if (m_scaleXVal) m_scaleXVal->setText(pct(sx));
    if (m_scaleYVal) m_scaleYVal->setText(pct(sy));
    if (m_anchorXVal) m_anchorXVal->setText(pct(ax));
    if (m_anchorYVal) m_anchorYVal->setText(pct(ay));
}

double PantaloneWidget::relPlayhead() {
    Clip* c = activeClip();
    if (!c) return -1.0;
    return std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur));
}

QVector<Keyframe>* PantaloneWidget::keyframesFor(int prop) {
    Clip* c = activeClip();
    if (!c) return nullptr;
    switch (prop) {
        case P_CropL: return &c->kfCropL;
        case P_CropR: return &c->kfCropR;
        case P_CropT: return &c->kfCropT;
        case P_CropB: return &c->kfCropB;
        case P_Scale: return &c->kfScale;
        case P_PanX:  return &c->kfTx;
        case P_PanY:  return &c->kfTy;
        case P_Rotation: return &c->kfRotation;
        case P_ScaleX: return &c->kfScaleX;
        case P_ScaleY: return &c->kfScaleY;
        case P_AnchorX: return &c->kfAnchorX;
        case P_AnchorY: return &c->kfAnchorY;
        // Propriedades da máscara ATIVA (nullptr se não há máscara selecionada).
        case P_MaskCx:      { Mask* m = activeMask(); return m ? &m->kfCx : nullptr; }
        case P_MaskCy:      { Mask* m = activeMask(); return m ? &m->kfCy : nullptr; }
        case P_MaskRx:      { Mask* m = activeMask(); return m ? &m->kfRx : nullptr; }
        case P_MaskRy:      { Mask* m = activeMask(); return m ? &m->kfRy : nullptr; }
        case P_MaskRot:     { Mask* m = activeMask(); return m ? &m->kfRotation : nullptr; }
        case P_MaskFeather: { Mask* m = activeMask(); return m ? &m->kfFeather : nullptr; }
    }
    return nullptr;
}

// Props que a faixa de keyframes / navegação / editor de curvas enxergam: as
// 12 do clipe e, quando há máscara selecionada, as 6 dela (motion da máscara).
QVector<int> PantaloneWidget::activeProps() {
    QVector<int> props;
    props << P_CropL << P_CropR << P_CropT << P_CropB
          << P_Scale << P_PanX << P_PanY << P_Rotation
          << P_ScaleX << P_ScaleY << P_AnchorX << P_AnchorY;
    if (activeMask())
        props << P_MaskCx << P_MaskCy << P_MaskRx << P_MaskRy
              << P_MaskRot << P_MaskFeather;
    return props;
}

void PantaloneWidget::setViewMode(ViewMode m) {
    m_mode = m;
    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it)
        it.value()->setChecked((ViewMode)it.key() == m);
    // A sidebar segue o modo: mostra só as propriedades relevantes.
    // Em "Janelas" a máscara se edita no viewer + painel inferior, então a
    // sidebar fica enxuta (só "Redefinir").
    if (m_secCrop) m_secCrop->setVisible(m == ModeCrop);
    if (m_secMove) m_secMove->setVisible(m == ModeTransform);
    if (m_secRot)  m_secRot->setVisible(m == ModeTransform);
    if (m_secMesh) {
        m_secMesh->setVisible(m == ModeMesh);
        if (m == ModeMesh && m_meshRes) {
            Clip* c = activeClip();
            if (c && c->hasMesh()) {
                const QSignalBlocker b(m_meshRes);
                m_meshRes->setCurrentText(
                    QStringLiteral("%1×%2").arg(c->meshCols).arg(c->meshRows));
            }
        }
    }
    if (m_view) m_view->update();
}

double PantaloneWidget::propValue(int prop) {
    switch (prop) {
        case P_CropL: return m_cropL->value() / 100.0;
        case P_CropR: return m_cropR->value() / 100.0;
        case P_CropT: return m_cropT->value() / 100.0;
        case P_CropB: return m_cropB->value() / 100.0;
        case P_Scale: return m_scale->value() / 100.0;
        case P_PanX: {
            const double W = m_project ? m_project->width : 1920.0;
            return m_panX->value() / 100.0 * W;
        }
        case P_PanY: {
            const double H = m_project ? m_project->height : 1080.0;
            return m_panY->value() / 100.0 * H;
        }
        case P_Rotation:
            return m_rotation->value();
        case P_ScaleX:
            return m_scaleX->value() / 100.0;
        case P_ScaleY:
            return m_scaleY->value() / 100.0;
        case P_AnchorX:
            return m_anchorX->value() / 100.0;
        case P_AnchorY:
            return m_anchorY->value() / 100.0;
        case P_MaskCx:      { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->cxAt(r) : 0.0; }
        case P_MaskCy:      { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->cyAt(r) : 0.0; }
        case P_MaskRx:      { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->rxAt(r) : 0.0; }
        case P_MaskRy:      { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->ryAt(r) : 0.0; }
        case P_MaskRot:     { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->rotAt(r) : 0.0; }
        case P_MaskFeather: { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->featherAt(r) : 0.0; }
    }
    return 0.0;
}

void PantaloneWidget::toggleKeyframe(int prop) {
    QVector<Keyframe>* kf = keyframesFor(prop);
    if (!kf) return;
    const double rel = relPlayhead();
    if (rel < 0.0) return;
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    // Sempre informa a propriedade editada, para o editor de curvas exibir a
    // curva correspondente (inclusive as da máscara).
    emit propertyEdited(prop);
    for (int i = 0; i < kf->size(); ++i) {
        if (std::fabs((*kf)[i].time - rel) < 1e-6) {
            kf->removeAt(i);
            emitChange();
            refreshDiamonds();
            m_view->update();
            return;
        }
    }
    Keyframe k;
    k.time = rel;
    k.value = propValue(prop);
    k.interp = KfSmooth;
    kf->append(k);
    std::sort(kf->begin(), kf->end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    emitChange();
    refreshDiamonds();
    m_view->update();
}

void PantaloneWidget::writeKeyframe(int prop, double value) {
    QVector<Keyframe>* kf = keyframesFor(prop);
    if (!kf) return;
    const double rel = relPlayhead();
    if (rel < 0.0) return;
    for (int i = 0; i < kf->size(); ++i) {
        if (std::fabs((*kf)[i].time - rel) < 1e-6) {
            (*kf)[i].value = value; // atualiza o keyframe já existente
            return;
        }
    }
    Keyframe k;
    k.time = rel;
    k.value = value;
    k.interp = KfSmooth;
    kf->append(k);
    std::sort(kf->begin(), kf->end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

void PantaloneWidget::gotoKeyframe(int dir) {
    Clip* c = activeClip();
    if (!c) return;
    const QVector<int> props = activeProps();
    QVector<double> times;
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (const Keyframe& k : *kf) times.append(c->pos + k.time);
    }
    if (times.isEmpty()) return;
    std::sort(times.begin(), times.end());
    double best = -1.0;
    if (dir > 0) {
        for (double t : times)
            if (t > m_playhead + 1e-6) { best = t; break; }
    } else {
        for (double t : times)
            if (t < m_playhead - 1e-6) best = t;
    }
    if (best >= 0.0) emit keyframeJump(best);
}

void PantaloneWidget::refreshDiamonds() {
    const double rel = relPlayhead();
    const QString on =
        QStringLiteral("QPushButton{color:%1; border:none; background:transparent; font-size:14px;}"
                       "QPushButton:hover{color:%2;}")
            .arg(themeColors().accentGold.name(),
                 themeColors().accentGold.lighter(130).name());
    const QString off =
        QStringLiteral("QPushButton{color:%1; border:none; background:transparent; font-size:14px;}"
                       "QPushButton:hover{color:%2;}")
            .arg(themeColors().iconMuted.name(),
                 themeColors().text.name());
    for (auto it = m_kfDiamonds.begin(); it != m_kfDiamonds.end(); ++it) {
        bool active = false;
        if (rel >= 0.0) {
            QVector<Keyframe>* kf = keyframesFor(it.key());
            if (kf) {
                for (const Keyframe& k : *kf) {
                    if (std::fabs(k.time - rel) < 1e-6) { active = true; break; }
                }
            }
        }
        it.value()->setStyleSheet(active ? on : off);
    }
    if (m_strip) m_strip->update();
}

void PantaloneWidget::commitSlider(int prop, double baseValue) {
    Clip* c = activeClip();
    if (!c) return;
    // Empurra o undo UMA vez por gesto; o propertyEdited é emitido sempre
    // (o editor de curvas ignora repetições da mesma propriedade).
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    emit propertyEdited(prop);
    switch (prop) {
        case P_CropL: c->cropL = baseValue; break;
        case P_CropR: c->cropR = baseValue; break;
        case P_CropT: c->cropT = baseValue; break;
        case P_CropB: c->cropB = baseValue; break;
        case P_Scale: c->scale = baseValue; break;
        case P_PanX:  c->tx = baseValue; break;
        case P_PanY:  c->ty = baseValue; break;
        case P_Rotation: c->rotation = baseValue; break;
        case P_ScaleX: c->scaleX = baseValue; break;
        case P_ScaleY: c->scaleY = baseValue; break;
        case P_AnchorX: c->anchorX = baseValue; break;
        case P_AnchorY: c->anchorY = baseValue; break;
    }
    if (m_kfAuto && m_kfAuto->isChecked())
        writeKeyframe(prop, baseValue);
    else if (QVector<Keyframe>* kf = keyframesFor(prop))
        kf->clear();
    // modified() DEPOIS de escrever o keyframe, senão o editor de curvas
    // atualiza com dados antigos e o keyframe novo nunca aparece.
    emit modified();
    refreshDiamonds();
    // Clipe de texto: o frame é gerado com o transform do clipe, então
    // recarrega para o texto acompanhar pan/zoom/rotação na janelinha.
    if (c->isText) loadFrame();
}

void PantaloneWidget::setCropValues(double L, double R, double T, double B) {
    Clip* c = activeClip();
    if (!c) return;
    L = std::clamp(L, 0.0, 1.0);
    R = std::clamp(R, 0.0, 1.0 - L);
    T = std::clamp(T, 0.0, 1.0);
    B = std::clamp(B, 0.0, 1.0 - T);
    QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB);
    m_cropL->setValue((int)std::lround(L * 100.0));
    m_cropR->setValue((int)std::lround(R * 100.0));
    m_cropT->setValue((int)std::lround(T * 100.0));
    m_cropB->setValue((int)std::lround(B * 100.0));
    commitSlider(P_CropL, L);
    commitSlider(P_CropR, R);
    commitSlider(P_CropT, T);
    commitSlider(P_CropB, B);
    updateValueLabels();
    m_view->update();
}

void PantaloneWidget::emitChange() {
    if (!m_undoPushed) {
        emit editStart();
        m_undoPushed = true;
    }
    emit modified();
}

void PantaloneWidget::computeView(double s, double tx, double ty, int w0, int h0,
                                QRectF* cropS, QRectF* outS) const {
    if (w0 <= 0 || h0 <= 0) return;
    const double L = std::clamp(m_cropL->value() / 100.0, 0.0, 1.0);
    const double R = std::clamp(m_cropR->value() / 100.0, 0.0, 1.0 - L);
    const double T = std::clamp(m_cropT->value() / 100.0, 0.0, 1.0);
    const double B = std::clamp(m_cropB->value() / 100.0, 0.0, 1.0 - T);
    const double kx = L * w0;
    const double ky = T * h0;
    const double kw = std::max(1.0, w0 * (1.0 - L - R));
    const double kh = std::max(1.0, h0 * (1.0 - T - B));
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    // Modelo CONTAIN (letterbox), igual ao preview/exportação: o recorte é
    // encaixado inteiro no quadro, sem cortar imagens não-16:9. Antes usava
    // "cover" (max) e a janela de saída recortava a imagem — caixa de recorte
    // "desalinhada" em relação ao resultado renderizado.
    const double k = std::min(W / kw, H / kh);
    const double ks = std::max(k * s, 1e-6);
    // Janela de saída em px do source: pode ser maior que o recorte quando há
    // barras (contain); limita ao recorte para a caixa ficar sobre a imagem.
    const double w = std::min(kw, W / ks);
    const double h = std::min(kh, H / ks);
    *cropS = QRectF(kx, ky, kw, kh);
    // Centro da janela dentro do recorte (centro do recorte - pan/escala).
    const double cx = kx + kw / 2.0 - tx / ks;
    const double cy = ky + kh / 2.0 - ty / ks;
    // As bordas vêm de `kx + kw - w`. Com w == kw a matemática diz hi == lo,
    // mas (kx + kw) - w arredonda e pode cair um ULP abaixo de kx, violando a
    // pré-condição de std::clamp (hi >= lo) e abortando o app no paint. O
    // std::max(garante hi >= lo) em qualquer estado de ponto flutuante.
    const double lx = std::clamp(cx - w / 2.0, kx, std::max(kx, kx + kw - w));
    const double ly = std::clamp(cy - h / 2.0, ky, std::max(ky, ky + kh - h));
    *outS = QRectF(lx, ly, w, h);
}

// Rotaciona um ponto da tela de volta para o espaço não-rotacionado do
// viewfinder, girando por -deg em torno do centro da área de exibição (disp).
// É o inverso do p.rotate(deg) usado no paintViewfinder, para que o hit-test
// e o arraste continuem funcionando com a imagem girada.
QPointF PantaloneWidget::rotatedViewPos(const QRect& viewRect, const QPoint& sp) const {
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return QPointF(sp);
    const QRectF disp = computeDisp(viewRect);
    const double a = m_rotation->value() * M_PI / 180.0;
    const double c = std::cos(a);
    const double s = std::sin(a);
    const QPointF ctr = disp.center();
    const double dx = sp.x() - ctr.x();
    const double dy = sp.y() - ctr.y();
    // R(-a): x' = x·cos + y·sin; y' = -x·sin + y·cos
    return QPointF(ctr.x() + dx * c + dy * s,
                   ctr.y() - dx * s + dy * c);
}

void PantaloneWidget::screenToSource(const QRect& viewRect, const QPoint& sp,
                                   double* sx, double* sy) const {
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;
    const QRectF disp = computeDisp(viewRect);
    // Desfaz a rotação antes de mapear para o espaço do source, senão o
    // recorte/pan "descolam" do mouse quando a imagem está girada.
    const QPointF rp = rotatedViewPos(viewRect, sp);
    *sx = (rp.x() - disp.left()) / disp.width() * w0;
    *sy = (rp.y() - disp.top()) / disp.height() * h0;
}

void PantaloneWidget::applyPan(double sx, double sy) {
    Clip* c = activeClip();
    if (!c || m_frame.isNull()) return;
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    const double L = std::clamp(m_cropL->value() / 100.0, 0.0, 1.0);
    const double R = std::clamp(m_cropR->value() / 100.0, 0.0, 1.0 - L);
    const double T = std::clamp(m_cropT->value() / 100.0, 0.0, 1.0);
    const double B = std::clamp(m_cropB->value() / 100.0, 0.0, 1.0 - T);
    const double kx = L * w0;
    const double ky = T * h0;
    const double kw = std::max(1.0, w0 * (1.0 - L - R));
    const double kh = std::max(1.0, h0 * (1.0 - T - B));
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    // Modelo CONTAIN, igual ao computeView/preview/exportação.
    const double k = std::min(W / kw, H / kh);
    const double s = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
    const double ks = std::max(k * s, 1e-6);
    const double w = std::min(kw, W / ks);
    const double h = std::min(kh, H / ks);

    const double ox = kx + kw / 2.0;
    const double oy = ky + kh / 2.0;
    double cx = sx;
    double cy = sy;
    if (w >= kw) cx = ox;
    else cx = std::clamp(cx, kx + w / 2.0, std::max(kx + w / 2.0, kx + kw - w / 2.0));
    if (h >= kh) cy = oy;
    else cy = std::clamp(cy, ky + h / 2.0, std::max(ky + h / 2.0, ky + kh - h / 2.0));

    const double tx = (ox - cx) * ks;
    const double ty = (oy - cy) * ks;

    QSignalBlocker b6(m_panX), b7(m_panY);
    m_panX->setValue((int)std::lround(std::clamp(tx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(ty / H * 100.0, -100.0, 100.0)));
    commitSlider(P_PanX, tx);
    commitSlider(P_PanY, ty);
    m_view->update();
}

// ── Máscaras (retângulo/elipse) ────────────────────────────────────────

Mask* PantaloneWidget::activeMask() {
    Clip* c = activeClip();
    if (!c || m_maskSel < 0 || m_maskSel >= c->masks.size()) return nullptr;
    return &c->masks[m_maskSel];
}

Mask* PantaloneWidget::currentMask() {
    return activeMask();
}

void PantaloneWidget::refreshMaskList() {
    if (!m_maskList) return;
    Clip* c = activeClip();
    const QSignalBlocker b(m_maskList);
    m_maskList->clear();
    if (c) {
        for (int i = 0; i < c->masks.size(); ++i) {
            const Mask& m = c->masks[i];
            const QString kind = m.type == QLatin1String("ellipse")
                                     ? tr("elipse")
                                     : tr("retângulo");
            m_maskList->addItem(
                tr("Máscara %1 · %2%3")
                    .arg(i + 1)
                    .arg(kind)
                    .arg(m.enabled ? QString() : tr(" (deslig.)")));
        }
    }
    const int count = m_maskList->count();
    if (m_maskSel < 0 && count > 0) m_maskSel = 0;
    if (m_maskSel >= count) m_maskSel = count - 1;
    m_maskList->setCurrentRow(m_maskSel);
}

void PantaloneWidget::syncMaskControls() {
    Mask* m = activeMask();
    const bool has = m != nullptr;
    if (m_maskEnabled) {
        const QSignalBlocker b(m_maskEnabled);
        m_maskEnabled->setChecked(m && m->enabled);
        m_maskEnabled->setEnabled(has);
    }
    if (m_maskInvert) {
        const QSignalBlocker b(m_maskInvert);
        m_maskInvert->setChecked(m && m->invert);
        m_maskInvert->setEnabled(has);
    }
    if (m_maskFeather) {
        const QSignalBlocker b(m_maskFeather);
        m_maskFeather->setValue(m ? (int)std::lround(m->feather * 100.0) : 0);
        m_maskFeather->setEnabled(has);
    }
    if (m_maskFeatherVal)
        m_maskFeatherVal->setText(
            m ? QString::number(m->feather, 'f', 2) : QStringLiteral("—"));
    if (m_maskDelBtn) m_maskDelBtn->setEnabled(has);
}

void PantaloneWidget::applyMaskEdit() {
    emitChange();
    m_view->update();
}

void PantaloneWidget::addMask(const QString& type) {
    Clip* c = activeClip();
    if (!c) return;
    emitChange();
    Mask d;
    d.type = type;
    d.enabled = true;
    d.cx = 0.5; d.cy = 0.5; d.rx = 0.35; d.ry = 0.35;
    d.rotation = 0.0; d.feather = 0.0; d.invert = false;
    c->masks.append(d);
    m_maskSel = c->masks.size() - 1;
    m_undoPushed = false; // nova ação => novo undo
    refreshMaskList();
    syncMaskControls();
    m_view->update();
}

void PantaloneWidget::removeSelectedMask() {
    Clip* c = activeClip();
    if (!c || m_maskSel < 0 || m_maskSel >= c->masks.size()) return;
    emitChange();
    c->masks.remove(m_maskSel);
    m_undoPushed = false;
    refreshMaskList();
    syncMaskControls();
    m_view->update();
}

bool PantaloneWidget::pickMaskHandle(QWidget* view, const QPoint& pos) {
    Mask* mp = activeMask();
    if (!mp || !mp->hasMask() || m_frame.isNull()) return false;
    const int w0 = m_frame.width(), h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return false;
    const QRectF disp = computeDisp(view->rect());
    auto toDisp = [&](double sx, double sy) {
        return QPointF(disp.left() + sx / w0 * disp.width(),
                       disp.top() + sy / h0 * disp.height());
    };
    // Usa os valores interpolados no playhead (igual ao preview/exportação),
    // para as alças baterem com o gizmo animado.
    Clip* c = activeClip();
    const double rel = c ? std::clamp(m_playhead - c->pos, 0.0,
                                      std::max(0.0, c->dur))
                         : 0.0;
    const QVector<QPointF> pts = maskCtrlPoints(mp->type, mp->cxAt(rel),
                                                mp->cyAt(rel), mp->rxAt(rel),
                                                mp->ryAt(rel), mp->rotAt(rel),
                                                w0, h0);
    const QPointF mouse = rotatedViewPos(view->rect(), pos);
    const double tol = 9.0;
    int best = -1;
    double bestD = tol * tol;
    for (int i = 0; i < pts.size(); ++i) {
        const QPointF sp = toDisp(pts[i].x(), pts[i].y());
        const double dx = sp.x() - mouse.x(), dy = sp.y() - mouse.y();
        const double d2 = dx * dx + dy * dy;
        if (d2 < bestD) { bestD = d2; best = i; }
    }
    if (best < 0) return false;
    m_maskDragHandle = best;
    m_maskDragRot = mp->rotAt(rel);
    m_maskPress = toDisp(pts[0].x(), pts[0].y());
    m_maskDragAng = std::atan2(mouse.y() - m_maskPress.y(),
                               mouse.x() - m_maskPress.x());
    return true;
}

void PantaloneWidget::applyMaskDrag(QWidget* view, const QPoint& pos) {
    Mask* mp = activeMask();
    if (!mp || m_maskDragHandle < 0 || m_frame.isNull()) return;
    const int w0 = m_frame.width(), h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;
    // Posição do mouse no espaço do quadro (px), já desrotacionada.
    double sx, sy;
    screenToSource(view->rect(), pos, &sx, &sy);
    // Centro/rotação de referência interpolados no playhead (igual ao gizmo).
    Clip* c = activeClip();
    const double rel = c ? std::clamp(m_playhead - c->pos, 0.0,
                                      std::max(0.0, c->dur))
                         : 0.0;
    const double cx = mp->cxAt(rel) * w0, cy = mp->cyAt(rel) * h0;
    if (m_maskDragHandle == 0) {           // mover: centro acompanha o cursor
        mp->cx = std::clamp(sx / w0, 0.0, 1.0);
        mp->cy = std::clamp(sy / h0, 0.0, 1.0);
    } else if (m_maskDragHandle == 1) {    // rotação: ângulo ao redor do centro
        const double ang = std::atan2(sy - cy, sx - cx);
        double d = ang - m_maskDragAng;
        while (d > M_PI) d -= 2.0 * M_PI;
        while (d < -M_PI) d += 2.0 * M_PI;
        double rot = m_maskDragRot + d * 180.0 / M_PI;
        while (rot > 180.0) rot -= 360.0;
        while (rot < -180.0) rot += 360.0;
        mp->rotation = rot;
    } else {                              // borda: raio projetado nos eixos
        const double dx = sx - cx, dy = sy - cy;
        const double rad = mp->rotAt(rel) * M_PI / 180.0;
        const double lx =  dx * std::cos(rad) + dy * std::sin(rad);
        const double ly = -dx * std::sin(rad) + dy * std::cos(rad);
        if (std::abs(lx) > 1.0) mp->rx = qBound(0.001, std::abs(lx) / w0, 1.5);
        if (std::abs(ly) > 1.0) mp->ry = qBound(0.001, std::abs(ly) / h0, 1.5);
    }
    // Auto-keyframe (padrão AE/DaVinci): com "● Auto" ligado, arrastar o gizmo
    // grava keyframe das propriedades alteradas no playhead => motion.
    if (m_kfAuto && m_kfAuto->isChecked()) {
        if (m_maskDragHandle == 0) {
            writeKeyframe(P_MaskCx, mp->cx);
            writeKeyframe(P_MaskCy, mp->cy);
        } else if (m_maskDragHandle == 1) {
            writeKeyframe(P_MaskRot, mp->rotation);
        } else {
            writeKeyframe(P_MaskRx, mp->rx);
            writeKeyframe(P_MaskRy, mp->ry);
        }
        refreshDiamonds();
    }
    // Informa a propriedade editada: o editor de curvas passa a mostrar a
    // curva da máscara em tempo real enquanto você arrasta o gizmo.
    if (m_maskDragHandle == 0) emit propertyEdited(P_MaskCx);
    else if (m_maskDragHandle == 1) emit propertyEdited(P_MaskRot);
    else emit propertyEdited(P_MaskRx);
    applyMaskEdit();
}

void PantaloneWidget::drawMeshGizmo(QPainter& p, const QRectF& disp, int w0, int h0) {
    Clip* c = activeClip();
    if (!c || !c->hasMesh() || w0 <= 0 || h0 <= 0) return;
    auto toDisp = [&](double nx, double ny) {
        return QPointF(disp.left() + nx * disp.width(),
                       disp.top() + ny * disp.height());
    };
    const QVector<QPointF>& mesh = c->mesh;
    const int cols = c->meshCols, rows = c->meshRows;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    // Grade: linhas horizontais e verticais conectando os pontos.
    p.setPen(QPen(QColor(130, 215, 255, 140), 1));
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols - 1; ++i)
            p.drawLine(toDisp(mesh[cols * j + i].x(), mesh[cols * j + i].y()),
                       toDisp(mesh[cols * j + i + 1].x(), mesh[cols * j + i + 1].y()));
    for (int i = 0; i < cols; ++i)
        for (int j = 0; j < rows - 1; ++j)
            p.drawLine(toDisp(mesh[cols * j + i].x(), mesh[cols * j + i].y()),
                       toDisp(mesh[cols * (j + 1) + i].x(), mesh[cols * (j + 1) + i].y()));
    // Pontos de controle (alças).
    p.setPen(QPen(QColor(12, 14, 18), 1));
    for (int k = 0; k < mesh.size(); ++k) {
        const QPointF d = toDisp(mesh[k].x(), mesh[k].y());
        p.setBrush(k == m_meshDrag ? QColor(255, 186, 60) : QColor(255, 255, 255));
        p.drawEllipse(d, 4.5, 4.5);
    }
    p.restore();
}

void PantaloneWidget::drawMaskGizmos(QPainter& p, const QRectF& disp, int w0, int h0) {
    Clip* c = activeClip();
    if (!c || c->masks.isEmpty() || w0 <= 0 || h0 <= 0) return;
    auto toDisp = [&](double sx, double sy) {
        return QPointF(disp.left() + sx / w0 * disp.width(),
                       disp.top() + sy / h0 * disp.height());
    };
    const QColor band(130, 215, 255);
    const QColor accent = themeColors().accent;
    const double rel = std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur));
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < c->masks.size(); ++i) {
        const Mask& m = c->masks[i];
        if (m.type.isEmpty()) continue;
        const bool active = (i == m_maskSel);
        // Valores interpolados no playhead (igual ao preview/exportação), para
        // o gizmo seguir a animação ao dar scrub.
        const double cx = m.cxAt(rel) * w0, cy = m.cyAt(rel) * h0;
        const double rx = std::max(0.001, m.rxAt(rel)) * w0;
        const double ry = std::max(0.001, m.ryAt(rel)) * h0;
        const double rotDeg = m.rotAt(rel);
        const double feath = m.featherAt(rel);
        const QPointF ctr = toDisp(cx, cy);
        // Forma (rotacionada) desenhada em torno do centro, no espaço de disp.
        p.save();
        p.translate(ctr);
        p.rotate(rotDeg);
        const double hw = rx / w0 * disp.width();
        const double hh = ry / h0 * disp.height();
        p.setBrush(Qt::NoBrush);
        if (feath > 0.0) {
            const double fx = feath * disp.width();
            const double fy = feath * disp.height();
            p.setPen(QPen(QColor(130, 215, 255, 90), 1, Qt::DashLine));
            if (m.type == QLatin1String("rect"))
                p.drawRect(QRectF(-hw - fx, -hh - fy, 2 * (hw + fx), 2 * (hh + fy)));
            else
                p.drawEllipse(QPointF(0, 0), hw + fx, hh + fy);
        }
        p.setPen(QPen(active ? QColor(255, 255, 255, 235)
                             : QColor(band.red(), band.green(), band.blue(), 130),
                      active ? 2 : 1));
        if (m.type == QLatin1String("rect"))
            p.drawRect(QRectF(-hw, -hh, 2 * hw, 2 * hh));
        else
            p.drawEllipse(QPointF(0, 0), hw, hh);
        p.restore();
        if (!active) continue;
        // Alças da máscara ativa (centro, rotação e bordas), já rotacionadas.
        const QVector<QPointF> pts = maskCtrlPoints(m.type, m.cxAt(rel), m.cyAt(rel),
                                                    m.rxAt(rel), m.ryAt(rel),
                                                    rotDeg, w0, h0);
        p.setPen(QPen(accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawLine(ctr, toDisp(pts[1].x(), pts[1].y()));
        p.setBrush(Qt::white);
        p.setPen(QPen(accent, 1.4));
        for (int k = 2; k < pts.size(); ++k) {
            const QPointF h = toDisp(pts[k].x(), pts[k].y());
            p.drawRect(QRectF(h.x() - 4.5, h.y() - 4.5, 9.0, 9.0));
        }
        p.drawEllipse(ctr, 4.5, 4.5);
        p.setBrush(QColor(255, 186, 60));
        p.drawEllipse(toDisp(pts[1].x(), pts[1].y()), 5.0, 5.0);
    }
    p.restore();
}

// Área (em px do widget) onde o frame do source é desenhado no viewfinder
// (modelo "contain", centralizado). Compartilhada pelo painter e pelos
// hit-tests, para a geometria dos gizmos nunca descolar do desenho.
QRectF PantaloneWidget::computeDisp(const QRect& viewRect) const {
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return QRectF();
    const int M = 12;
    const QRectF screen = QRectF(viewRect).adjusted(M, M, -M, -M);
    const double fr = (double)h0 / w0;
    const double sr = screen.height() / screen.width();
    QRectF disp;
    if (fr < sr) {
        disp.setWidth(screen.width());
        disp.setHeight(screen.width() * fr);
        disp.moveLeft(screen.left());
        disp.moveTop(screen.top() + (screen.height() - disp.height()) / 2.0);
    } else {
        disp.setHeight(screen.height());
        disp.setWidth(screen.height() / fr);
        disp.moveTop(screen.top());
        disp.moveLeft(screen.left() + (screen.width() - disp.width()) / 2.0);
    }
    return disp;
}

// Aplica zoom (escala) mantendo o centro da janela de saída NO MESMO ponto da
// imagem — mesmo comportamento do zoom com a roda, que redefine o pan para a
// janela não "andar" enquanto o usuário dá zoom (usado por roda e gizmo).
void PantaloneWidget::applyZoomTo(double sNew, const QPointF& centerSrc) {
    if (m_frame.isNull()) return;
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double L = std::clamp(m_cropL->value() / 100.0, 0.0, 1.0);
    const double R = std::clamp(m_cropR->value() / 100.0, 0.0, 1.0 - L);
    const double T = std::clamp(m_cropT->value() / 100.0, 0.0, 1.0);
    const double B = std::clamp(m_cropB->value() / 100.0, 0.0, 1.0 - T);
    const double kx = L * w0;
    const double ky = T * h0;
    const double kw = std::max(1.0, w0 * (1.0 - L - R));
    const double kh = std::max(1.0, h0 * (1.0 - T - B));
    const double k = std::min(W / kw, H / kh);
    const double ks = std::max(k * sNew, 1e-6);
    const double ntx = (kx + kw / 2.0 - centerSrc.x()) * ks;
    const double nty = (ky + kh / 2.0 - centerSrc.y()) * ks;
    QSignalBlocker b5(m_scale), b6(m_panX), b7(m_panY);
    m_scale->setValue((int)std::lround(sNew * 100.0));
    m_panX->setValue((int)std::lround(std::clamp(ntx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(nty / H * 100.0, -100.0, 100.0)));
    commitSlider(P_Scale, sNew);
    commitSlider(P_PanX, ntx);
    commitSlider(P_PanY, nty);
    updateValueLabels();
    m_view->update();
}

void PantaloneWidget::paintViewfinder(QWidget* view) {
    QPainter p(view);
    // Fundo quadriculado sutil (estilo DaVinci/After Effects) atrás do frame.
    const QColor tileA(42, 44, 50);
    const QColor tileB(33, 35, 41);
    const int ts = 13;
    p.fillRect(view->rect(), tileB);
    p.setPen(Qt::NoPen);
    p.setBrush(tileA);
    for (int y = 0; y < view->height(); y += ts) {
        const int off = ((y / ts) & 1) * ts;
        for (int x = off; x < view->width(); x += 2 * ts)
            p.drawRect(x, y, ts, ts);
    }
    // Borda sutil do monitor.
    p.setPen(QPen(QColor(51, 51, 51), 1));
    p.drawRect(view->rect().adjusted(0, 0, -1, -1));
    if (m_frame.isNull()) {
        p.setPen(QColor(140, 140, 150));
        p.drawText(view->rect(), Qt::AlignCenter,
                   tr("Selecione um clipe de vídeo para editar o pantalone."));
        return;
    }
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;

    const QRectF disp = computeDisp(view->rect());

    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double s = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
    const double tx = m_panX->value() / 100.0 * W;
    const double ty = m_panY->value() / 100.0 * H;

    QRectF cropS, outS;
    computeView(s, tx, ty, w0, h0, &cropS, &outS);

    auto toDisp = [&](double sx, double sy) {
        return QPointF(disp.left() + sx / w0 * disp.width(),
                       disp.top() + sy / h0 * disp.height());
    };

    p.save();
    // Gira a cena inteira em torno do centro da área de exibição.
    const double rot = m_rotation->value();
    p.translate(disp.center());
    p.rotate(rot);
    p.translate(-disp.center());
    p.drawImage(QRectF(disp.left(), disp.top(), disp.width(), disp.height()), m_frame);

    // Janela de saída (borda azul-ciano de destaque) e caixas/gizmos.
    const QRectF cropDisp(toDisp(cropS.left(), cropS.top()),
                          toDisp(cropS.right(), cropS.bottom()));
    const QRectF outDisp(toDisp(outS.left(), outS.top()),
                         toDisp(outS.right(), outS.bottom()));

    // Máscara escura (estilo Premiere): véu do que está fora do recorte no
    // modo Recortar; do que está fora da janela de saída no modo Transformar.
    if (m_mode == ModeCrop) {
        QPainterPath outsideCrop;
        outsideCrop.addRect(disp);
        QPainterPath cropHole;
        cropHole.addRect(cropDisp.adjusted(-3, -3, 3, 3));
        p.fillPath(outsideCrop.subtracted(cropHole), QColor(0, 0, 0, 110));
    } else if (m_mode == ModeTransform && cropDisp != outDisp) {
        QPainterPath outsideOut;
        outsideOut.addRect(cropDisp);
        QPainterPath outHole;
        outHole.addRect(outDisp);
        p.fillPath(outsideOut.subtracted(outHole), QColor(0, 0, 0, 130));
    }

    // Grade de terços + cruz central (modo Transformar, de enquadramento).
    if (m_mode == ModeTransform && m_showGrid) {
        p.setPen(QPen(QColor(255, 255, 255, 45), 1));
        const QPointF c = outDisp.center();
        p.drawLine(QPointF(outDisp.left(), c.y()), QPointF(outDisp.right(), c.y()));
        p.drawLine(QPointF(c.x(), outDisp.top()), QPointF(c.x(), outDisp.bottom()));
        for (int i = 1; i <= 2; ++i) {
            const double fx = i / 3.0;
            const double fy = i / 3.0;
            p.drawLine(QPointF(outDisp.left() + outDisp.width() * fx, outDisp.top()),
                       QPointF(outDisp.left() + outDisp.width() * fx, outDisp.bottom()));
            p.drawLine(QPointF(outDisp.left(), outDisp.top() + outDisp.height() * fy),
                       QPointF(outDisp.right(), outDisp.top() + outDisp.height() * fy));
        }
    }

    // Safe margins + caixa de saída (modo Transformar) — cinza discreto.
    if (m_mode == ModeTransform) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 32), 1));
        p.drawRect(outDisp.adjusted(outDisp.width() * 0.05, outDisp.height() * 0.05,
                                    -outDisp.width() * 0.05, -outDisp.height() * 0.05));
        p.setPen(QColor(255, 255, 255, 20));
        p.drawRect(outDisp.adjusted(outDisp.width() * 0.10, outDisp.height() * 0.10,
                                    -outDisp.width() * 0.10, -outDisp.height() * 0.10));
        // Caixa de ZOOM/saída (borda azul-ciano em destaque).
        p.setPen(QPen(QColor(0, 163, 224), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(outDisp);
    }
    if (m_mode == ModeCrop) {
        // Caixa de RECORTE (borda branca tracejada sutil).
        p.setPen(QPen(QColor(255, 255, 255, 150), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cropDisp);
    }

    const bool hasHover = (m_hoverMode != DragNone && m_dragMode == DragNone);
    auto drawHandle = [&](const QPointF& h, double sz, int fr, int fg, int fb,
                          int fa) {
        p.setBrush(hasHover ? QColor(fr, fg, fb, 255)
                            : QColor(fr, fg, fb, fa));
        p.drawRoundedRect(QRectF(h.x() - sz / 2.0, h.y() - sz / 2.0, sz, sz), 2.0, 2.0);
    };

    // Alças de recorte (modo Recortar): quadrados brancos no perímetro do crop.
    if (m_mode == ModeCrop) {
        const double hd = hasHover ? 10.0 : 8.0;
        p.setPen(QPen(QColor(12, 14, 18), 1));
        const QPointF tl(cropDisp.left(), cropDisp.top());
        const QPointF tr(cropDisp.right(), cropDisp.top());
        const QPointF bl(cropDisp.left(), cropDisp.bottom());
        const QPointF br(cropDisp.right(), cropDisp.bottom());
        const QPointF mt(cropDisp.center().x(), cropDisp.top());
        const QPointF mb(cropDisp.center().x(), cropDisp.bottom());
        const QPointF ml(cropDisp.left(), cropDisp.center().y());
        const QPointF mr(cropDisp.right(), cropDisp.center().y());
        const QPointF handles[8] = { tl, tr, bl, br, mt, mb, ml, mr };
        for (const QPointF& h : handles)
            drawHandle(h, hd, 220, 225, 235, 210);
    }

    // Modo Transformar: alças de zoom (douradas), rotação e âncora.
    if (m_mode == ModeTransform) {
        const double zd = hasHover ? 12.0 : 9.0;
        p.setPen(QPen(QColor(10, 12, 16), 1));
        const QPointF outCorners[4] = { outDisp.topLeft(), outDisp.topRight(),
                                        outDisp.bottomLeft(), outDisp.bottomRight() };
        for (const QPointF& cp : outCorners) {
            const QPointF d = cp - outDisp.center();
            const double len = std::hypot(d.x(), d.y());
            const QPointF pos = len > 1e-6 ? cp + d / len * 9.0 : cp;
            drawHandle(pos, zd, 255, 186, 60, 235);
        }
        // Alça de ROTAÇÃO (estilo Premiere).
        const QPointF rotHdl(outDisp.center().x(), outDisp.top() - 26.0);
        p.setPen(QPen(QColor(255, 186, 60, 220), 1));
        p.setBrush(Qt::NoBrush);
        p.drawLine(outDisp.center().x(), outDisp.top(), rotHdl.x(), rotHdl.y());
        p.setPen(QPen(QColor(10, 12, 16), 1));
        p.setBrush(hasHover ? QColor(255, 216, 120) : QColor(255, 186, 60, 235));
        p.drawEllipse(rotHdl, 5.5, 5.5);
        // Gizmo de ponto de ancoragem (anel branco com cruz).
        const double axPct = m_anchorX->value() / 100.0;
        const double ayPct = m_anchorY->value() / 100.0;
        const double anchorSrcX = w0 * 0.5 + axPct * w0 * 0.5;
        const double anchorSrcY = h0 * 0.5 + ayPct * h0 * 0.5;
        const QPointF anchorDisp = toDisp(anchorSrcX, anchorSrcY);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(anchorDisp, 7.0, 7.0);
        p.drawLine(QPointF(anchorDisp.x() - 4.0, anchorDisp.y()),
                   QPointF(anchorDisp.x() + 4.0, anchorDisp.y()));
        p.drawLine(QPointF(anchorDisp.x(), anchorDisp.y() - 4.0),
                   QPointF(anchorDisp.x(), anchorDisp.y() + 4.0));
    }

    // Gizmos de máscara (modo Janelas) — no espaço rotacionado, com a imagem.
    if (m_mode == ModeMask)
        drawMaskGizmos(p, disp, w0, h0);

    // Malha (modo Malha) — grade 3×3 de espremer/esticar a mídia.
    if (m_mode == ModeMesh)
        drawMeshGizmo(p, disp, w0, h0);

    // ── Indicador de resolução do projeto (estilo Vegas) ─────────────
    p.restore();  // restaura antes de desenhar o overlay sem rotação
    p.save();
    const QString resLabel = QString("%1×%2").arg((int)W).arg((int)H);
    QFont f = view->font();
    f.setPointSize(9);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(0, 0, 0, 160));
    const QRect r = view->rect();
    p.drawText(r.adjusted(8, 0, -8, -8), Qt::AlignBottom | Qt::AlignRight,
               resLabel);
    p.setPen(QColor(255, 255, 255, 200));
    p.drawText(r.adjusted(7, -1, -9, -9), Qt::AlignBottom | Qt::AlignRight,
               resLabel);
    p.restore();
}

void PantaloneWidget::viewportPress(QWidget* view, QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || m_frame.isNull()) {
        e->ignore();
        return;
    }

    // Alt+clique: move o ponto de âncora direto para o clique (sem trocar de
    // modo). Liberdade estilo AE — reposiciona o pivot de escala/rotação.
    if ((e->modifiers() & Qt::AltModifier) && m_mode == ModeTransform) {
        const double w0 = m_frame.width();
        const double h0 = m_frame.height();
        double sx, sy;
        screenToSource(view->rect(), e->pos(), &sx, &sy);
        const double ax = std::clamp((sx / w0 - 0.5) / 0.5, -1.0, 1.0);
        const double ay = std::clamp((sy / h0 - 0.5) / 0.5, -1.0, 1.0);
        QSignalBlocker bax(m_anchorX), bay(m_anchorY);
        m_anchorX->setValue((int)std::lround(ax * 100.0));
        m_anchorY->setValue((int)std::lround(ay * 100.0));
        updateValueLabels();
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        commitSlider(P_AnchorX, ax);
        commitSlider(P_AnchorY, ay);
        m_view->update();
        e->accept();
        return;
    }
    double sx, sy;
    screenToSource(view->rect(), e->pos(), &sx, &sy);
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double s = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
    const double tx = m_panX->value() / 100.0 * W;
    const double ty = m_panY->value() / 100.0 * H;
    QRectF cropS, outS;
    computeView(s, tx, ty, w0, h0, &cropS, &outS);

    const QRectF disp = computeDisp(view->rect());
    auto toDisp = [&](double cx, double cy) {
        return QPointF(disp.left() + cx / w0 * disp.width(),
                       disp.top() + cy / h0 * disp.height());
    };
    const QPointF tl = toDisp(cropS.left(), cropS.top());
    const QPointF br = toDisp(cropS.right(), cropS.bottom());
    const QPointF tr = toDisp(cropS.right(), cropS.top());
    const QPointF bl = toDisp(cropS.left(), cropS.bottom());
    const QPointF mt = toDisp(cropS.center().x(), cropS.top());
    const QPointF mb = toDisp(cropS.center().x(), cropS.bottom());
    const QPointF ml = toDisp(cropS.left(), cropS.center().y());
    const QPointF mr = toDisp(cropS.right(), cropS.center().y());
    const QPointF pos = rotatedViewPos(view->rect(), e->pos());
    const double tol = 8.0;

    auto near = [&](const QPointF& a, const QPointF& b) {
        return std::hypot(a.x() - b.x(), a.y() - b.y()) <= tol;
    };

    m_dragMode = DragNone;
    m_lastDrag = e->pos();

    // Gizmos de máscara (modo Janelas) têm precedência.
    if (m_mode == ModeMask && pickMaskHandle(view, e->pos())) {
        view->setCursor(Qt::ClosedHandCursor);
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        e->accept();
        return;
    }

    // Malha 3×3 (modo Malha): ativa ao mexer e arrasta um ponto da grade.
    if (m_mode == ModeMesh) {
        Clip* c = activeClip();
        if (c) {
            if (!c->hasMesh()) c->enableDefaultMesh();
            const QRectF mdisp = computeDisp(view->rect());
            auto mToDisp = [&](double nx, double ny) {
                return QPointF(mdisp.left() + nx * mdisp.width(),
                               mdisp.top() + ny * mdisp.height());
            };
            const QPointF mpos = rotatedViewPos(view->rect(), e->pos());
            int best = -1;
            double bestD = 81.0; // 9² px de tolerância
            for (int k = 0; k < c->mesh.size(); ++k) {
                const QPointF d = mToDisp(c->mesh[k].x(), c->mesh[k].y());
                const double dx = d.x() - mpos.x(), dy = d.y() - mpos.y();
                const double d2 = dx * dx + dy * dy;
                if (d2 < bestD) { bestD = d2; best = k; }
            }
            if (best >= 0) {
                m_meshDrag = best;
                if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
                view->setCursor(Qt::ClosedHandCursor);
                e->accept();
                return;
            }
        }
    }

    // Hit-test nas alças de ZOOM (cantos da janela de saída, deslocados para
    // fora) e na alça de ROTAÇÃO (círculo acima do topo): ficam mais para fora
    // que recorte/pan, então têm precedência.
    const QPointF oTL = toDisp(outS.left(), outS.top());
    const QPointF oTR = toDisp(outS.right(), outS.top());
    const QPointF oBL = toDisp(outS.left(), outS.bottom());
    const QPointF oBR = toDisp(outS.right(), outS.bottom());
    const QPointF oC((oTL.x() + oBR.x()) / 2.0, (oTL.y() + oBR.y()) / 2.0);
    const QPointF rotHdl(oC.x(), oTL.y() - 26.0);
    QPointF zHandles[4];
    const QPointF zooms[4] = { oTL, oTR, oBL, oBR };
    for (int i = 0; i < 4; ++i) {
        const QPointF d = zooms[i] - oC;
        const double len = std::hypot(d.x(), d.y());
        zHandles[i] = len > 1e-6 ? zooms[i] + d / len * 9.0 : zooms[i];
    }

    if (std::hypot(pos.x() - rotHdl.x(), pos.y() - rotHdl.y()) <= 12.0) {
        const QPointF C = disp.center();
        m_grabOffset = QPointF(std::atan2(pos.y() - C.y(), pos.x() - C.x()),
                               (double)m_rotation->value());
        m_dragMode = DragRotate;
        view->setCursor(Qt::CrossCursor);
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        emit propertyEdited(P_Rotation);
        e->accept();
        return;
    }

    int zoomIdx = -1;
    for (int i = 0; i < 4; ++i) {
        if (std::hypot(pos.x() - zHandles[i].x(), pos.y() - zHandles[i].y()) <= 10.0) {
            zoomIdx = i;
            break;
        }
    }
    if (m_mode == ModeTransform && zoomIdx >= 0) {
        const double sOld = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
        m_grabOffset = QPointF(std::hypot(zHandles[zoomIdx].x() - oC.x(),
                                          zHandles[zoomIdx].y() - oC.y()),
                               sOld);
        m_zoomPivot = outS.center();
        m_zoomCenterDisp = oC;
        switch (zoomIdx) {
            case 0: m_dragMode = DragZoomTL; view->setCursor(Qt::SizeFDiagCursor); break;
            case 1: m_dragMode = DragZoomTR; view->setCursor(Qt::SizeBDiagCursor); break;
            case 2: m_dragMode = DragZoomBL; view->setCursor(Qt::SizeBDiagCursor); break;
            default: m_dragMode = DragZoomBR; view->setCursor(Qt::SizeFDiagCursor); break;
        }
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        emit propertyEdited(P_Scale);
        e->accept();
        return;
    }

    // Hit-test no gizmo de ancoragem (círculo branco).
    const double axPct = m_anchorX->value() / 100.0;
    const double ayPct = m_anchorY->value() / 100.0;
    const QPointF anchorDisp = toDisp(w0 * 0.5 + axPct * w0 * 0.5,
                                      h0 * 0.5 + ayPct * h0 * 0.5);
    if (m_mode == ModeTransform && near(pos, anchorDisp)) {
        m_dragMode = DragAnchor;
        m_grabOffset = QPointF(axPct, ayPct);
        view->setCursor(Qt::CrossCursor);
        if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
        emit propertyEdited(P_AnchorX);
        e->accept();
        return;
    }

    if (m_mode == ModeCrop) {
        if (near(pos, tl)) m_dragMode = DragCropTL;
        else if (near(pos, tr)) m_dragMode = DragCropTR;
        else if (near(pos, bl)) m_dragMode = DragCropBL;
        else if (near(pos, br)) m_dragMode = DragCropBR;
        else if (pos.x() >= ml.x() - tol && pos.x() <= ml.x() + tol &&
                 pos.y() >= mt.y() && pos.y() <= mb.y())
            m_dragMode = DragCropL;
        else if (pos.x() >= mr.x() - tol && pos.x() <= mr.x() + tol &&
                 pos.y() >= mt.y() && pos.y() <= mb.y())
            m_dragMode = DragCropR;
        else if (pos.y() >= mt.y() - tol && pos.y() <= mt.y() + tol &&
                 pos.x() >= ml.x() && pos.x() <= mr.x())
            m_dragMode = DragCropT;
        else if (pos.y() >= mb.y() - tol && pos.y() <= mb.y() + tol &&
                 pos.x() >= ml.x() && pos.x() <= mr.x())
            m_dragMode = DragCropB;
    }

    if (m_dragMode != DragNone) {
        switch (m_dragMode) {
            case DragCropTL: case DragCropBR: view->setCursor(Qt::SizeFDiagCursor); break;
            case DragCropTR: case DragCropBL: view->setCursor(Qt::SizeBDiagCursor); break;
            case DragCropL:  case DragCropR:  view->setCursor(Qt::SizeHorCursor); break;
            case DragCropT:  case DragCropB:  view->setCursor(Qt::SizeVerCursor); break;
            default: break;
        }
        e->accept();
        return;
    }

    // Só inicia o pan (modo Transformar) se o clique começar dentro (ou perto)
    // da janela de saída.
    if (m_mode == ModeTransform &&
        outS.adjusted(-0.05 * outS.width(), -0.05 * outS.height(),
                      0.05 * outS.width(), 0.05 * outS.height()).contains(QPointF(sx, sy))) {
        m_dragMode = DragPan;
        // Mantém a "pegada": guarda o offset entre o cursor e o centro da
        // janela para o movimento acompanhar o mouse 1:1 (sem pular ao centro).
        m_grabOffset = QPointF(outS.center().x() - sx, outS.center().y() - sy);
        view->setCursor(Qt::ClosedHandCursor);
        m_undoPushed = true;
        emit editStart();
        emit propertyEdited(P_PanX);
        applyPan(sx + m_grabOffset.x(), sy + m_grabOffset.y());
        e->accept();
    } else {
        e->ignore();
    }
}

void PantaloneWidget::viewportMove(QWidget* view, QMouseEvent* e) {
    // Arraste da malha: move o ponto selecionado (warp).
    if (m_meshDrag >= 0) {
        Clip* c = activeClip();
        if (c && c->hasMesh() && m_meshDrag < c->mesh.size() && !m_frame.isNull()) {
            const int w0 = m_frame.width(), h0 = m_frame.height();
            if (w0 > 0 && h0 > 0) {
                double sx, sy;
                screenToSource(view->rect(), e->pos(), &sx, &sy);
                c->mesh[m_meshDrag] = QPointF(std::clamp(sx / w0, -0.5, 1.5),
                                              std::clamp(sy / h0, -0.5, 1.5));
                emitChange();
                m_view->update();
            }
        }
        return;
    }
    // Arraste de máscara ativo: continua enquanto m_maskDragHandle >= 0.
    if (m_maskDragHandle >= 0) {
        applyMaskDrag(view, e->pos());
        return;
    }
    // Hover tracking: atualiza m_hoverMode quando não está arrastando.
    if (m_dragMode == DragNone && !m_frame.isNull()) {
        double sx, sy;
        screenToSource(view->rect(), e->pos(), &sx, &sy);
        const int w0 = m_frame.width();
        const int h0 = m_frame.height();
        const double W = m_project ? m_project->width : 1920.0;
        const double H = m_project ? m_project->height : 1080.0;
        const double s = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
        const double tx = m_panX->value() / 100.0 * W;
        const double ty = m_panY->value() / 100.0 * H;
        QRectF cropS, outS;
        computeView(s, tx, ty, w0, h0, &cropS, &outS);
        const QRectF disp = computeDisp(view->rect());
        auto toDisp = [&](double cx, double cy) {
            return QPointF(disp.left() + cx / w0 * disp.width(),
                           disp.top() + cy / h0 * disp.height());
        };
        const QPointF tl = toDisp(cropS.left(), cropS.top());
        const QPointF tr = toDisp(cropS.right(), cropS.top());
        const QPointF bl = toDisp(cropS.left(), cropS.bottom());
        const QPointF br = toDisp(cropS.right(), cropS.bottom());
        const QPointF mt = toDisp(cropS.center().x(), cropS.top());
        const QPointF mb = toDisp(cropS.center().x(), cropS.bottom());
        const QPointF ml = toDisp(cropS.left(), cropS.center().y());
        const QPointF mr = toDisp(cropS.right(), cropS.center().y());
        const QPointF pos = rotatedViewPos(view->rect(), e->pos());
        const double tol = 8.0;
        auto near = [&](const QPointF& a, const QPointF& b) {
            return std::hypot(a.x() - b.x(), a.y() - b.y()) <= tol;
        };
        DragMode newHover = DragNone;
        // Alças de zoom (cantos da janela de saída) e rotação (círculo acima):
        // precedência sobre recorte/pan por ficarem mais para fora.
        const QPointF oTL = toDisp(outS.left(), outS.top());
        const QPointF oTR = toDisp(outS.right(), outS.top());
        const QPointF oBL = toDisp(outS.left(), outS.bottom());
        const QPointF oBR = toDisp(outS.right(), outS.bottom());
        const QPointF oC((oTL.x() + oBR.x()) / 2.0, (oTL.y() + oBR.y()) / 2.0);
        const QPointF rotHdl(oC.x(), oTL.y() - 26.0);
        bool hitZoom = false;
        const QPointF zooms[4] = { oTL, oTR, oBL, oBR };
        for (int i = 0; i < 4 && !hitZoom; ++i) {
            const QPointF d = zooms[i] - oC;
            const double len = std::hypot(d.x(), d.y());
            const QPointF hp = len > 1e-6 ? zooms[i] + d / len * 9.0 : zooms[i];
            hitZoom = std::hypot(pos.x() - hp.x(), pos.y() - hp.y()) <= 10.0;
        }
    if (m_mode == ModeTransform &&
        std::hypot(pos.x() - rotHdl.x(), pos.y() - rotHdl.y()) <= 12.0) {
            newHover = DragRotate;
        } else if (hitZoom) {
            newHover = DragZoomBR;
        } else if (near(pos, tl)) newHover = DragCropTL;
        else if (near(pos, tr)) newHover = DragCropTR;
        else if (near(pos, bl)) newHover = DragCropBL;
        else if (near(pos, br)) newHover = DragCropBR;
        else if (pos.x() >= ml.x() - tol && pos.x() <= ml.x() + tol &&
                 pos.y() >= mt.y() && pos.y() <= mb.y())
            newHover = DragCropL;
        else if (pos.x() >= mr.x() - tol && pos.x() <= mr.x() + tol &&
                 pos.y() >= mt.y() && pos.y() <= mb.y())
            newHover = DragCropR;
        else if (pos.y() >= mt.y() - tol && pos.y() <= mt.y() + tol &&
                 pos.x() >= ml.x() && pos.x() <= mr.x())
            newHover = DragCropT;
        else if (pos.y() >= mb.y() - tol && pos.y() <= mb.y() + tol &&
                 pos.x() >= ml.x() && pos.x() <= mr.x())
            newHover = DragCropB;

        if (newHover != m_hoverMode) {
            m_hoverMode = newHover;
            view->update();
        }
        // Cursor contextual.
        switch (newHover) {
            case DragRotate: view->setCursor(Qt::CrossCursor); break;
            case DragZoomTL: case DragZoomBR: view->setCursor(Qt::SizeFDiagCursor); break;
            case DragZoomTR: case DragZoomBL: view->setCursor(Qt::SizeBDiagCursor); break;
            case DragCropTL: case DragCropBR: view->setCursor(Qt::SizeFDiagCursor); break;
            case DragCropTR: case DragCropBL: view->setCursor(Qt::SizeBDiagCursor); break;
            case DragCropL:  case DragCropR:  view->setCursor(Qt::SizeHorCursor); break;
            case DragCropT:  case DragCropB:  view->setCursor(Qt::SizeVerCursor); break;
            default: view->setCursor(Qt::ArrowCursor); break;
        }
        e->ignore();
        return;
    }
    if (m_dragMode == DragNone) {
        e->ignore();
        return;
    }
    e->accept();
    m_lastDrag = e->pos();
    double sx, sy;
    screenToSource(view->rect(), e->pos(), &sx, &sy);
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;
    const double L0 = std::clamp(m_cropL->value() / 100.0, 0.0, 1.0);
    const double R0 = std::clamp(m_cropR->value() / 100.0, 0.0, 1.0);
    const double T0 = std::clamp(m_cropT->value() / 100.0, 0.0, 1.0);
    const double B0 = std::clamp(m_cropB->value() / 100.0, 0.0, 1.0);
    switch (m_dragMode) {
        case DragPan:
            applyPan(sx + m_grabOffset.x(), sy + m_grabOffset.y());
            break;
        case DragRotate: {
            // Gira a imagem mantendo o clique: a rotação nova é a atual mais o
            // quanto o ponteiro "andou" em ângulo em torno do centro do frame.
            const QPointF C = computeDisp(view->rect()).center();
            const QPointF p = rotatedViewPos(view->rect(), e->pos());
            const double theta = std::atan2(p.y() - C.y(), p.x() - C.x());
            const double nv = std::clamp(
                m_grabOffset.y() + (theta - m_grabOffset.x()) * 180.0 / M_PI,
                (double)-kRotMax, (double)kRotMax);
            QSignalBlocker sb8(m_rotation);
            m_rotation->setValue((int)std::lround(nv));
            updateValueLabels();
            commitSlider(P_Rotation, nv);
            m_view->update();
            break;
        }
        case DragZoomTL: case DragZoomTR:
        case DragZoomBL: case DragZoomBR: {
            // Zoom "distancia-se" do centro da janela de saída: arrastar o
            // canto dourado para fora aumenta, para dentro diminui.
            const QPointF p = rotatedViewPos(view->rect(), e->pos());
            const double d = std::hypot(p.x() - m_zoomCenterDisp.x(),
                                        p.y() - m_zoomCenterDisp.y());
            const double ratio = std::clamp(d / m_grabOffset.x(), 0.1, 10.0);
            const double sNew = std::clamp(m_grabOffset.y() / ratio,
                                           kMinScale, kMaxScale);
            applyZoomTo(sNew, m_zoomPivot);
            break;
        }
        case DragAnchor: {
            // Move o ponto de ancoragem relativo ao centro do source.
            const double newAx = std::clamp((sx / w0 - 0.5) / 0.5, -1.0, 1.0);
            const double newAy = std::clamp((sy / h0 - 0.5) / 0.5, -1.0, 1.0);
            QSignalBlocker bax(m_anchorX), bay(m_anchorY);
            m_anchorX->setValue((int)std::lround(newAx * 100.0));
            m_anchorY->setValue((int)std::lround(newAy * 100.0));
            updateValueLabels();
            commitSlider(P_AnchorX, newAx);
            commitSlider(P_AnchorY, newAy);
            m_view->update();
            break;
        }
        case DragCropL: case DragCropR:
        case DragCropT: case DragCropB:
        case DragCropTL: case DragCropTR:
        case DragCropBL: case DragCropBR: {
            // Crop interativo. Shift = uniforme:
            //  • borda: recorta os DOIS lados igualmente (centro fixo);
            //  • canto: mantém a proporção da área recortada (aspect do source).
            const bool shift = (e->modifiers() & Qt::ShiftModifier);
            double nL = L0, nR = R0, nT = T0, nB = B0;
            const double aspect = (h0 > 0) ? (double)w0 / (double)h0 : 16.0 / 9.0;
            const bool isEdge = (m_dragMode == DragCropL || m_dragMode == DragCropR
                                 || m_dragMode == DragCropT || m_dragMode == DragCropB);
            if (shift && isEdge) {
                // Borda: recorta os DOIS lados pelo mesmo delta (centro fixo).
                if (m_dragMode == DragCropL) {
                    const double d = sx / w0 - L0; // negativo = corta mais p/ esq
                    nL = std::clamp(L0 + d, 0.0, 1.0 - R0 - 0.01);
                    nR = std::clamp(R0 - d, 0.0, 1.0 - nL - 0.01);
                } else if (m_dragMode == DragCropR) {
                    const double newR = (w0 - sx) / w0;
                    const double d = newR - R0;
                    nR = std::clamp(R0 + d, 0.0, 1.0 - L0 - 0.01);
                    nL = std::clamp(L0 - d, 0.0, 1.0 - nR - 0.01);
                } else if (m_dragMode == DragCropT) {
                    const double d = sy / h0 - T0;
                    nT = std::clamp(T0 + d, 0.0, 1.0 - B0 - 0.01);
                    nB = std::clamp(B0 - d, 0.0, 1.0 - nT - 0.01);
                } else if (m_dragMode == DragCropB) {
                    const double newB = (h0 - sy) / h0;
                    const double d = newB - B0;
                    nB = std::clamp(B0 + d, 0.0, 1.0 - T0 - 0.01);
                    nT = std::clamp(T0 - d, 0.0, 1.0 - nB - 0.01);
                }
            } else if (shift && !isEdge) {
                // Canto: mantém a proporção da área visível.
                // A dimensão arrastada manda; a outra é derivada pelo aspect.
                const double maxW = 1.0 - L0 - R0;
                const double maxH = 1.0 - T0 - B0;
                if (m_dragMode == DragCropTL || m_dragMode == DragCropTR) {
                    const double newW = (m_dragMode == DragCropTL)
                        ? std::clamp(sx / w0 - L0, 0.01, maxW)
                        : std::clamp((w0 - sx) / w0 - R0, 0.01, maxW);
                    const double newH = std::clamp(newW / aspect, 0.01, maxH);
                    // Ancora no topo: T e o lado arrastado mudam; B/L/R fixos.
                    nT = T0 + maxH - newH; // empurra de cima p/ baixo
                    if (m_dragMode == DragCropTL) nL = L0 + maxW - newW;
                    else                          nR = R0 + maxW - newW;
                } else {
                    const double newH = (m_dragMode == DragCropBL)
                        ? std::clamp(sy / h0 - T0, 0.01, maxH)
                        : std::clamp((h0 - sy) / h0 - B0, 0.01, maxH);
                    const double newW = std::clamp(newH * aspect, 0.01, maxW);
                    // Ancora embaixo: B e o lado arrastado mudam; T/L/R fixos.
                    nB = B0 + maxH - newH;
                    if (m_dragMode == DragCropBL) nL = L0 + maxW - newW;
                    else                          nR = R0 + maxW - newW;
                }
            } else {
                // Livre (sem Shift).
                switch (m_dragMode) {
                case DragCropL:  nL = sx / w0; break;
                case DragCropR:  nR = (w0 - sx) / w0; break;
                case DragCropT:  nT = sy / h0; break;
                case DragCropB:  nB = (h0 - sy) / h0; break;
                case DragCropTL: nL = sx / w0; nT = sy / h0; break;
                case DragCropTR: nR = (w0 - sx) / w0; nT = sy / h0; break;
                case DragCropBL: nL = sx / w0; nB = (h0 - sy) / h0; break;
                case DragCropBR: nR = (w0 - sx) / w0; nB = (h0 - sy) / h0; break;
                default: break;
                }
            }
            setCropValues(nL, nR, nT, nB);
            break;
        }
        default: break;
    }
}

// Duplo-clique no viewport, modo Transform: se acertar o gizmo da âncora,
// reseta ao centro do source (liberdade estilo AE).
void PantaloneWidget::viewportDoubleClick(QWidget* view, QMouseEvent* e) {
    if (m_mode != ModeTransform || m_frame.isNull()) return;
    const double w0 = m_frame.width();
    const double h0 = m_frame.height();
    const double axPct = m_anchorX->value() / 100.0;
    const double ayPct = m_anchorY->value() / 100.0;
    const QRectF disp = computeDisp(view->rect());
    const QPointF anchorDisp(
        disp.left() + (w0 * 0.5 + axPct * w0 * 0.5) / w0 * disp.width(),
        disp.top() + (h0 * 0.5 + ayPct * h0 * 0.5) / h0 * disp.height());
    if (std::hypot(e->pos().x() - anchorDisp.x(),
                   e->pos().y() - anchorDisp.y()) > 8.0)
        return;
    QSignalBlocker bax(m_anchorX), bay(m_anchorY);
    m_anchorX->setValue(0);
    m_anchorY->setValue(0);
    updateValueLabels();
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    commitSlider(P_AnchorX, 0.0);
    commitSlider(P_AnchorY, 0.0);
    m_view->update();
    e->accept();
}

void PantaloneWidget::viewportRelease(QWidget* view, QMouseEvent* e) {
    if (m_meshDrag >= 0) {
        m_meshDrag = -1;
        m_undoPushed = false;
        view->setCursor(Qt::ArrowCursor);
    }
    if (m_maskDragHandle >= 0) {
        m_maskDragHandle = -1;
        m_undoPushed = false;
        view->setCursor(Qt::ArrowCursor);
    }
    if (m_dragMode != DragNone) {
        m_dragMode = DragNone;
        view->setCursor(Qt::ArrowCursor);
        m_undoPushed = false;
    }
    e->accept();
}

void PantaloneWidget::viewportWheel(QWidget* view, QWheelEvent* e) {
    if (m_frame.isNull() || !activeClip()) {
        e->ignore();
        return;
    }
    // Alt+roda rotaciona a imagem em passos de kRotStep.
    if (e->modifiers() & Qt::AltModifier) {
        const int nv = std::clamp(m_rotation->value()
                                      + (e->angleDelta().y() > 0 ? kRotStep : -kRotStep),
                                  -kRotMax, kRotMax);
        QSignalBlocker b8(m_rotation);
        m_rotation->setValue(nv);
        commitSlider(P_Rotation, (double)nv);
        updateValueLabels();
        m_view->update();
        e->accept();
        return;
    }
    const double factor = e->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1;
    const double sOld = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
    const double sNew = std::clamp(sOld * factor, kMinScale, kMaxScale);
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double tx = m_panX->value() / 100.0 * W;
    const double ty = m_panY->value() / 100.0 * H;

    QRectF cropS, outS;
    computeView(sOld, tx, ty, w0, h0, &cropS, &outS);
    const QPointF center = outS.center();

    applyZoomTo(sNew, center);
}

void PantaloneWidget::viewportContextMenu(QWidget* view, QContextMenuEvent* e) {
    Clip* c = activeClip();
    if (!c) { e->ignore(); return; }
    QMenu menu(view);

    // Flip
    QAction* flipH = menu.addAction(tr("Espelhar Horizontal"));
    flipH->setCheckable(true);
    flipH->setChecked(c->scaleX < 0.0);
    QAction* flipV = menu.addAction(tr("Espelhar Vertical"));
    flipV->setCheckable(true);
    flipV->setChecked(c->scaleY < 0.0);

    menu.addSeparator();

    // Grid
    QAction* gridAct = menu.addAction(tr("Grade de Terços"));
    gridAct->setCheckable(true);
    gridAct->setChecked(m_showGrid);

    QAction* act = menu.exec(e->globalPos());
    if (!act) return;

    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }

    if (act == flipH) {
        c->scaleX = flipH->isChecked() ? -std::fabs(c->scaleX) : std::fabs(c->scaleX);
        if (m_kfAuto && m_kfAuto->isChecked())
            writeKeyframe(P_ScaleX, c->scaleX);
        emit modified();
        syncFromClip();
        m_view->update();
    } else if (act == flipV) {
        c->scaleY = flipV->isChecked() ? -std::fabs(c->scaleY) : std::fabs(c->scaleY);
        if (m_kfAuto && m_kfAuto->isChecked())
            writeKeyframe(P_ScaleY, c->scaleY);
        emit modified();
        syncFromClip();
        m_view->update();
    } else if (act == gridAct) {
        m_showGrid = gridAct->isChecked();
        m_view->update();
    }
}

// --- Faixa de tempo dos keyframes (arraste esquerda/direita) ---

// Converte posição relativa do playhead/keyframe (0..clip.dur) em pixel na strip.
// Calha à esquerda da faixa para os rótulos das pistas (Recorte/Máscara/Resto).
static constexpr int kKfLabelGutter = 54;

static int kfStripX(double rel, const QWidget* view, double dur) {
    const int usable = view->width() - kKfLabelGutter - 4;
    if (dur <= 0.0 || usable <= 0) return kKfLabelGutter;
    const double frac = std::clamp(rel / dur, 0.0, 1.0);
    return kKfLabelGutter + (int)std::lround(frac * usable);
}

// Fração 0..1 a partir da posição x do mouse (desconta a calha e a margem).
static double kfStripFrac(int x, const QWidget* view) {
    const int usable = view->width() - kKfLabelGutter - 8;
    if (usable <= 0) return 0.0;
    return std::clamp((double)(x - kKfLabelGutter - 4) / usable, 0.0, 1.0);
}

// As 3 pistas da faixa: Recorte, Máscara e Resto (transformação agrupada).
static void kfLaneProps(QVector<int> lanes[3]) {
    using P = PantaloneWidget;
    lanes[0] = {P::P_CropL, P::P_CropR, P::P_CropT, P::P_CropB};
    lanes[1] = {P::P_MaskCx, P::P_MaskCy, P::P_MaskRx, P::P_MaskRy,
                P::P_MaskRot, P::P_MaskFeather};
    lanes[2] = {P::P_Scale, P::P_PanX, P::P_PanY, P::P_Rotation,
                P::P_ScaleX, P::P_ScaleY, P::P_AnchorX, P::P_AnchorY};
}

void PantaloneWidget::paintKeyframeStrip(QWidget* view) {
    QPainter p(view);
    p.fillRect(view->rect(), QColor(19, 20, 24));
    Clip* c = activeClip();
    if (!c) return;
    const double dur = std::max(c->dur, 1e-3);
    const int w = view->width();
    const int h = view->height();

    QVector<int> lanes[3];
    kfLaneProps(lanes);
    const QString labels[3] = { tr("Recorte"), tr("Máscara"), tr("Resto") };
    const int laneH = std::max(1, h / 3);

    // Fundo, divisores e rótulos das pistas.
    QFont lf = view->font();
    lf.setPointSize(8);
    p.setFont(lf);
    for (int i = 0; i < 3; ++i) {
        const int top = i * laneH;
        const int bot = (i == 2) ? h : top + laneH;
        if (i > 0) {
            p.setPen(QColor(40, 42, 48));
            p.drawLine(0, top, w, top);
        }
        p.setPen(QColor(140, 145, 155));
        p.drawText(QRect(2, top, kKfLabelGutter - 4, bot - top),
                   Qt::AlignVCenter | Qt::AlignLeft, labels[i]);
    }

    const double rel = relPlayhead();
    // Playhead cruza todas as pistas.
    if (rel >= 0.0) {
        const int px = kfStripX(rel, view, dur);
        p.setPen(QPen(QColor(190, 220, 255, 200), 1));
        p.drawLine(px, 2, px, h - 2);
    }

    // Keyframes, uma pista de cada vez.
    for (int i = 0; i < 3; ++i) {
        const int top = i * laneH;
        const int bot = (i == 2) ? h : top + laneH;
        const int cy = (top + bot) / 2;
        QVector<double> times;
        for (int prop : lanes[i]) {
            QVector<Keyframe>* kf = keyframesFor(prop);
            if (!kf) continue;
            for (const Keyframe& k : *kf)
                if (!times.contains(k.time)) times.append(k.time);
        }
        std::sort(times.begin(), times.end());
        for (double t : times) {
            const int x = kfStripX(t, view, dur);
            const bool active = rel >= 0.0 && std::fabs(t - rel) < 1e-6;
            const bool sel = m_selectedTimes.contains(t);
            QPolygon dia = QPolygon()
                << QPoint(x, cy - 5) << QPoint(x + 4, cy)
                << QPoint(x, cy + 5) << QPoint(x - 4, cy);
            p.setPen(QPen(QColor(10, 10, 12), 1));
            // Selecionado = verde; no playhead = laranja; senão cinza.
            p.setBrush(sel ? QColor(120, 230, 150)
                           : active ? QColor(255, 179, 64)
                                    : QColor(200, 205, 215));
            p.drawPolygon(dia);
        }
    }
}

void PantaloneWidget::stripPress(QWidget* view, QMouseEvent* e) {
    m_stripDragging = false;
    m_stripPlayheadDrag = false;
    Clip* c = activeClip();
    if (e->button() != Qt::LeftButton || !c) return;
    const double dur = std::max(c->dur, 1e-3);
    const int h = view->height();
    const int laneH = std::max(1, h / 3);
    const int lane = std::clamp(e->pos().y() / laneH, 0, 2);
    const int laneTop = lane * laneH;
    const int laneBot = (lane == 2) ? h : laneTop + laneH;
    const int laneCy = (laneTop + laneBot) / 2;

    // Encontra o keyframe mais próximo do clique DENTRO da pista clicada.
    // O hit é limitado à zona do losango (vertical) para não roubar o arraste
    // da agulha quando o playhead está exatamente sobre um keyframe.
    QVector<int> lanes[3];
    kfLaneProps(lanes);
    QVector<double> times;
    for (int prop : lanes[lane]) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (const Keyframe& k : *kf)
            if (!times.contains(k.time)) times.append(k.time);
    }
    int bestIdx = -1;
    double bestDist = 1e9;
    for (int i = 0; i < times.size(); ++i) {
        const int x = kfStripX(times[i], view, dur);
        const int dist = std::abs(e->pos().x() - x);
        if (dist <= 9 && std::abs(e->pos().y() - laneCy) <= laneH / 2 + 2 && dist < bestDist) {
            bestDist = dist; bestIdx = i;
        }
    }
    if (bestIdx >= 0) {
        m_stripLane = lane; // mover/apagar só a pista de onde veio o keyframe
        // Shift+clique alterna a seleção sem iniciar arraste.
        if (e->modifiers() & Qt::ShiftModifier) {
            if (m_selectedTimes.contains(times[bestIdx]))
                m_selectedTimes.removeAll(times[bestIdx]);
            else
                m_selectedTimes.append(times[bestIdx]);
            std::sort(m_selectedTimes.begin(), m_selectedTimes.end());
            view->setFocus();
            m_strip->update();
            e->accept();
            return;
        }
        // Ctrl+clique alterna a seleção E permite arrastar (multi-arraste).
        if (e->modifiers() & Qt::ControlModifier) {
            if (m_selectedTimes.contains(times[bestIdx]))
                m_selectedTimes.removeAll(times[bestIdx]);
            else
                m_selectedTimes.append(times[bestIdx]);
            std::sort(m_selectedTimes.begin(), m_selectedTimes.end());
            view->setFocus();
            m_stripDragging = true;
            m_stripDragFrom = times[bestIdx];
            m_stripDragTo = times[bestIdx];
            setCursor(Qt::ClosedHandCursor);
            emit editStart();
            m_undoPushed = true;
            emit keyframeJump(c->pos + times[bestIdx]);
            m_strip->update();
            e->accept();
            return;
        }
        // Clique simples seleciona só este keyframe e permite arrastar.
        m_selectedTimes = { times[bestIdx] };
        view->setFocus();
        m_stripDragging = true;
        m_stripDragFrom = times[bestIdx];
        m_stripDragTo = times[bestIdx];
        setCursor(Qt::ClosedHandCursor);
        emit editStart();
        m_undoPushed = true;
        emit keyframeJump(c->pos + times[bestIdx]);
        m_strip->update();
        e->accept();
        return;
    }

    // Clique vazio / na agulha: limpa a seleção e pula/arrasta o playhead.
    m_selectedTimes.clear();
    m_stripLane = -1;
    m_stripPlayheadDrag = true;
    const double frac = kfStripFrac(e->pos().x(), view);
    emit keyframeJump(c->pos + frac * dur);
    m_strip->update();
    e->accept();
}

void PantaloneWidget::stripMove(QWidget* view, QMouseEvent* e) {
    Clip* c = activeClip();
    if (!c) return;
    const double dur = std::max(c->dur, 1e-3);
    const double frac = kfStripFrac(e->pos().x(), view);
    const double newRel = frac * dur;

    // Arraste da agulha: move o playhead (scrub).
    if (m_stripPlayheadDrag) {
        if (std::fabs(newRel - relPlayhead()) < 1e-6) return;
        emit keyframeJump(c->pos + newRel);
        m_strip->update();
        e->accept();
        return;
    }

    if (!m_stripDragging || m_stripDragTo < 0.0) return;
    if (std::fabs(newRel - m_stripDragTo) < 1e-6) return;
    // Usa a posição ATUAL do keyframe como origem (não a original), senão o
    // keyframe "descola" do cursor já no segundo evento de mouse.
    const double from = m_stripDragTo;
    m_stripDragTo = newRel;

    // Move o keyframe SÓ na pista de onde ele veio (Recorte, Máscara ou
    // Resto) — não sincroniza as outras pistas.
    QVector<int> lanes[3];
    kfLaneProps(lanes);
    const QVector<int> props = (m_stripLane >= 0 && m_stripLane < 3)
                                   ? lanes[m_stripLane]
                                   : activeProps();
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (int i = 0; i < kf->size(); ++i) {
            if (std::fabs((*kf)[i].time - from) < 1e-6) {
                (*kf)[i].time = newRel;
                break;
            }
        }
        std::sort(kf->begin(), kf->end(),
                  [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }
    refreshDiamonds();
    m_view->update();
    m_strip->update();
    // A seleção acompanha o keyframe arrastado.
    if (m_selectedTimes.contains(from)) {
        m_selectedTimes.removeAll(from);
        m_selectedTimes.append(newRel);
        std::sort(m_selectedTimes.begin(), m_selectedTimes.end());
    }
    emit modified();
    e->accept();
}

void PantaloneWidget::stripRelease(QWidget* view, QMouseEvent* e) {
    if (m_stripDragging || m_stripPlayheadDrag) {
        m_stripDragging = false;
        m_stripPlayheadDrag = false;
        m_stripDragFrom = -1.0;
        m_stripDragTo = -1.0;
        setCursor(Qt::ArrowCursor);
        m_strip->update();
    }
    e->accept();
}

// Duplo clique na faixa: cria keyframes em todas as propriedades de
// transformação naquele instante, fixando os valores interpolados.
void PantaloneWidget::stripDoubleClick(QWidget* view, QMouseEvent* e) {
    Clip* c = activeClip();
    if (!c) { e->ignore(); return; }
    const double dur = std::max(c->dur, 1e-3);
    const double frac = kfStripFrac(e->pos().x(), view);
    const double rel = frac * dur;

    const QVector<int> props = activeProps();
    // Já existe keyframe nesse instante: nada a fazer.
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (const Keyframe& k : *kf)
            if (std::fabs(k.time - rel) < 1e-6) { e->accept(); return; }
    }

    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    emit propertyEdited(P_Scale);
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        if (prop >= P_MaskCx) continue; // duplo clique = só props do clipe
        Keyframe k;
        k.time = rel;
        k.interp = KfSmooth;
        switch (prop) {
            case P_CropL: k.value = kfValue(c->kfCropL, c->cropL, rel); break;
            case P_CropR: k.value = kfValue(c->kfCropR, c->cropR, rel); break;
            case P_CropT: k.value = kfValue(c->kfCropT, c->cropT, rel); break;
            case P_CropB: k.value = kfValue(c->kfCropB, c->cropB, rel); break;
            case P_Scale: k.value = kfValue(c->kfScale, c->scale, rel); break;
            case P_PanX:  k.value = kfValue(c->kfTx, c->tx, rel); break;
            case P_PanY:  k.value = kfValue(c->kfTy, c->ty, rel); break;
            case P_Rotation: k.value = kfValue(c->kfRotation, c->rotation, rel); break;
            case P_ScaleX: k.value = kfValue(c->kfScaleX, c->scaleX, rel); break;
            case P_ScaleY: k.value = kfValue(c->kfScaleY, c->scaleY, rel); break;
            case P_AnchorX: k.value = kfValue(c->kfAnchorX, c->anchorX, rel); break;
            case P_AnchorY: k.value = kfValue(c->kfAnchorY, c->anchorY, rel); break;
        }
        kf->append(k);
        std::sort(kf->begin(), kf->end(),
                  [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }
    emit modified();
    refreshDiamonds();
    m_strip->update();
    e->accept();
}

// Apaga os keyframes selecionados na faixa (em todas as propriedades de
// transformação no mesmo instante). Ligado ao Delete/Backspace da faixa.
void PantaloneWidget::deleteSelectedKeyframes() {
    Clip* c = activeClip();
    if (!c || m_selectedTimes.isEmpty()) return;
    QVector<int> lanes[3];
    kfLaneProps(lanes);
    const QVector<int> props = (m_stripLane >= 0 && m_stripLane < 3)
                                   ? lanes[m_stripLane]
                                   : activeProps();
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    for (double t : m_selectedTimes) {
        for (int prop : props) {
            QVector<Keyframe>* kf = keyframesFor(prop);
            if (!kf) continue;
            for (int i = kf->size() - 1; i >= 0; --i) {
                if (std::fabs((*kf)[i].time - t) < 1e-6) kf->removeAt(i);
            }
        }
    }
    m_selectedTimes.clear();
    emit modified();
    refreshDiamonds();
    m_strip->update();
}

// Clique direito num keyframe da faixa: predefinições de suavização estilo
// Vegas (linear, suave, rápido, lento, acentuada).
void PantaloneWidget::stripContextMenu(QWidget* view, QContextMenuEvent* e) {
    Clip* c = activeClip();
    if (!c) { e->ignore(); return; }
    const double dur = std::max(c->dur, 1e-3);

    const QVector<int> props = activeProps();
    QVector<double> times;
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (const Keyframe& k : *kf)
            if (!times.contains(k.time)) times.append(k.time);
    }
    int bestIdx = -1;
    double bestDist = 1e9;
    for (int i = 0; i < times.size(); ++i) {
        const int x = kfStripX(times[i], view, dur);
        const int dist = std::abs(e->pos().x() - x);
        if (dist <= 9 && dist < bestDist) { bestDist = dist; bestIdx = i; }
    }
    if (bestIdx < 0) { e->accept(); return; }
    const double t = times[bestIdx];

    QMenu menu(view);
    menu.addAction(tr("Linear (padrão)"));
    QAction* suave = menu.addAction(tr("Suave"));
    QAction* rapido = menu.addAction(tr("Rápido"));
    QAction* lento = menu.addAction(tr("Lento"));
    QAction* acent = menu.addAction(tr("Acentuada"));
    QAction* act = menu.exec(e->globalPos());
    if (!act) return;

    int preset = 0;
    if (act == suave) preset = 1;
    else if (act == rapido) preset = 2;
    else if (act == lento) preset = 3;
    else if (act == acent) preset = 4;

    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (int i = 0; i < kf->size(); ++i) {
            if (std::fabs((*kf)[i].time - t) < 1e-6) {
                applyEasePreset(*kf, i, preset);
                break;
            }
        }
    }
    emit modified();
    refreshDiamonds();
    m_strip->update();
}
