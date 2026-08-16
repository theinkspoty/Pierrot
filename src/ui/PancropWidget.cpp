// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "PancropWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QGroupBox>
#include <QPushButton>
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
#include <algorithm>
#include <cmath>

namespace {
constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 4.0;
constexpr int kCropMax = 80;   // %
constexpr int kPanMax = 100;   // % da largura/altura do projeto
constexpr int kRotMax = 180;   // graus (±)
constexpr int kRotStep = 5;    // graus por tick da roda (Alt+roda)

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
// e recebe os eventos de mouse/roda. Só delega para o PancropWidget dono.
class PancropWidget::Viewport : public QWidget {
public:
    explicit Viewport(PancropWidget* owner) : QWidget(owner), m_owner(owner) {
        setMinimumSize(220, 200);
    }
protected:
    void paintEvent(QPaintEvent*) override { m_owner->paintViewfinder(this); }
    void mousePressEvent(QMouseEvent* e) override { m_owner->viewportPress(this, e); }
    void mouseMoveEvent(QMouseEvent* e) override { m_owner->viewportMove(this, e); }
    void mouseReleaseEvent(QMouseEvent* e) override { m_owner->viewportRelease(this, e); }
    void wheelEvent(QWheelEvent* e) override { m_owner->viewportWheel(this, e); }
private:
    PancropWidget* m_owner;
};

// Faixa de tempo dos keyframes do clipe: mostra a posição dos keyframes
// (relativa ao início do clipe) e permite arrastá-los para a esquerda/direita.
class PancropWidget::KeyframeStrip : public QWidget {
public:
    explicit KeyframeStrip(PancropWidget* owner) : QWidget(owner), m_owner(owner) {
        setMinimumHeight(36);
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
            // faixa de keyframes do pancrop está com foco.
            if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
                e->accept();
                return true;
            }
        }
        return QWidget::event(e);
    }
private:
    PancropWidget* m_owner;
};

PancropWidget::PancropWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(480, 360);

    auto makeSlider = [this](int min, int max) {
        auto* s = new QSlider(Qt::Horizontal, this);
        s->setRange(min, max);
        s->setMinimumWidth(110);
        return s;
    };
    auto makeVal = [this]() {
        auto* l = new QLabel(tr("0%"), this);
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        l->setFixedWidth(42);
        l->setStyleSheet(QStringLiteral(
            "color:#9cc4f0; font-weight:bold; background:#202228;"
            "border:1px solid #2c2f36; border-radius:3px; padding:1px 4px;"));
        return l;
    };
    auto makeDiamond = [this](int prop) {
        auto* b = new QPushButton(QStringLiteral("◆"), this);
        b->setFixedSize(22, 22);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tr("Alternar keyframe no playhead"));
        b->setStyleSheet(QStringLiteral(
            "QPushButton{color:#565d68; border:none; background:transparent; font-size:14px;}"
            "QPushButton:hover{color:#c9d4e2;}"));
        connect(b, &QPushButton::clicked, this, [this, prop]() { toggleKeyframe(prop); });
        m_kfDiamonds[prop] = b;
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
    m_cropLVal = makeVal(); m_cropRVal = makeVal(); m_cropTVal = makeVal();
    m_cropBVal = makeVal(); m_scaleVal = makeVal(); m_panXVal = makeVal();
    m_panYVal = makeVal(); m_rotationVal = makeVal();

    auto addValRow = [&](QGridLayout* g, int r, const QString& label,
                         QSlider* s, QLabel* v, int prop) {
        auto* lab = new QLabel(label, this);
        lab->setFixedWidth(58);
        lab->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        g->addWidget(lab, r, 0);
        g->addWidget(s, r, 1);
        g->addWidget(v, r, 2);
        g->addWidget(makeDiamond(prop), r, 3);
    };

    auto* cropBox = new QGroupBox(tr("Recorte"), this);
    auto* cropGrid = new QGridLayout(cropBox);
    cropGrid->setSpacing(3);
    cropGrid->setContentsMargins(6, 8, 6, 6);
    cropGrid->setColumnStretch(1, 1);
    addValRow(cropGrid, 0, tr("Esquerda:"), m_cropL, m_cropLVal, P_CropL);
    addValRow(cropGrid, 1, tr("Direita:"), m_cropR, m_cropRVal, P_CropR);
    addValRow(cropGrid, 2, tr("Topo:"), m_cropT, m_cropTVal, P_CropT);
    addValRow(cropGrid, 3, tr("Base:"), m_cropB, m_cropBVal, P_CropB);

    auto* moveBox = new QGroupBox(tr("Zoom e Posição"), this);
    auto* moveGrid = new QGridLayout(moveBox);
    moveGrid->setSpacing(3);
    moveGrid->setContentsMargins(6, 8, 6, 6);
    moveGrid->setColumnStretch(1, 1);
    addValRow(moveGrid, 0, tr("Zoom:"), m_scale, m_scaleVal, P_Scale);
    addValRow(moveGrid, 1, tr("Pan X:"), m_panX, m_panXVal, P_PanX);
    addValRow(moveGrid, 2, tr("Pan Y:"), m_panY, m_panYVal, P_PanY);

    auto* rotBox = new QGroupBox(tr("Rotação"), this);
    auto* rotGrid = new QGridLayout(rotBox);
    rotGrid->setSpacing(3);
    rotGrid->setContentsMargins(6, 8, 6, 6);
    rotGrid->setColumnStretch(1, 1);
    addValRow(rotGrid, 0, tr("Ângulo:"), m_rotation, m_rotationVal, P_Rotation);

    auto connectSlider = [&](QSlider* s, int prop) {
        connect(s, &QSlider::sliderReleased, this, [this]() { m_undoPushed = false; });
        connect(s, &QSlider::valueChanged, this, [this, prop]() {
            Clip* c = activeClip();
            if (!c) return;
            switch (prop) {
                case P_CropL: commitSlider(prop, m_cropL->value() / 100.0); break;
                case P_CropR: commitSlider(prop, m_cropR->value() / 100.0); break;
                case P_CropT: commitSlider(prop, m_cropT->value() / 100.0); break;
                case P_CropB: commitSlider(prop, m_cropB->value() / 100.0); break;
                case P_Scale: commitSlider(prop, m_scale->value() / 100.0); break;
                case P_PanX: {
                    const double W = m_project ? m_project->width : 1920.0;
                    commitSlider(prop, m_panX->value() / 100.0 * W);
                    break;
                }
                case P_PanY: {
                    const double H = m_project ? m_project->height : 1080.0;
                    commitSlider(prop, m_panY->value() / 100.0 * H);
                    break;
                }
                case P_Rotation:
                    commitSlider(prop, (double)m_rotation->value());
                    break;
            }
            updateValueLabels();
            m_view->update();
        });
    };
    connectSlider(m_cropL, P_CropL);
    connectSlider(m_cropR, P_CropR);
    connectSlider(m_cropT, P_CropT);
    connectSlider(m_cropB, P_CropB);
    connectSlider(m_scale, P_Scale);
    connectSlider(m_panX, P_PanX);
    connectSlider(m_panY, P_PanY);
    connectSlider(m_rotation, P_Rotation);

    auto* resetBtn = new QPushButton(tr("Redefinir"), this);
    resetBtn->setMinimumHeight(22);
    resetBtn->setToolTip(tr("Zera recorte, zoom, posição e rotação do clipe"));
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB),
                       b5(m_scale), b6(m_panX), b7(m_panY), b8(m_rotation);
        m_cropL->setValue(0); m_cropR->setValue(0);
        m_cropT->setValue(0); m_cropB->setValue(0);
        m_scale->setValue(100); m_panX->setValue(0); m_panY->setValue(0);
        m_rotation->setValue(0);
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
        }
        m_undoPushed = false;
        updateValueLabels();
        m_view->update();
        if (c) emitChange();
    });

    auto* hint = new QLabel(tr("Dica: arraste dentro da caixa branca para mover "
                               "(pan), arraste as bordas/alças para recortar, use "
                               "a roda do mouse para dar zoom e Alt+roda para "
                               "rotacionar."),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#7e8794; font-size:11px;"));

    // Navegação de keyframes (estilo DaVinci): anterior/próximo e auto-keyframe.
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

    // Viewfinder de um lado, controles de transformação do outro (lado a lado)
    // — assim a tela não fica atrás/embaixo dos sliders.
    m_view = new Viewport(this);
    m_strip = new KeyframeStrip(this);
    m_strip->setToolTip(tr("Arraste um keyframe para a esquerda/direita para "
                           "mover sua posição no tempo"));
    auto* controls = new QWidget(this);
    auto* ctlLay = new QVBoxLayout(controls);
    ctlLay->setContentsMargins(0, 0, 0, 0);
    ctlLay->setSpacing(5);
    ctlLay->addLayout(kfBar);
    ctlLay->addWidget(m_strip);
    ctlLay->addWidget(cropBox);
    ctlLay->addWidget(moveBox);
    ctlLay->addWidget(rotBox);
    ctlLay->addWidget(resetBtn);
    ctlLay->addWidget(hint);
    ctlLay->addStretch(1);

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(6);
    lay->addWidget(m_view, 1);
    lay->addWidget(controls);

    // Visual escuro, plano e minimalista (estilo Effect Controls do Premiere).
    setStyleSheet(QStringLiteral(
        "QWidget{background:#15161a;}"
        "QGroupBox{border:1px solid #2a2d34; border-radius:4px; margin-top:14px;"
        " background:#1c1e23;}"
        "QGroupBox::title{subcontrol-origin:margin; left:8px; top:2px; padding:0 4px;"
        " color:#9db2c8; font-weight:bold; font-size:11px;}"
        "QPushButton{border:1px solid #2e3138; border-radius:3px; background:#23252b;"
        " color:#c9cdd4; padding:2px 8px;}"
        "QPushButton:hover{background:#2b2e35; border-color:#3c414b;}"
        "QPushButton:checked{background:#243447; border-color:#3f6ea5; color:#9cc4f0;}"
        "QSlider::groove:horizontal{height:3px; background:#2e3138; border-radius:1px;}"
        "QSlider::sub-page:horizontal{background:#3f6ea5; border-radius:1px;}"
        "QSlider::handle:horizontal{width:11px; margin:-4px 0; border-radius:5px;"
        " background:#7aa7d4; border:1px solid #3f6ea5;}"
        "QSlider::handle:horizontal:hover{background:#93bde8;}"
        "QLabel{color:#c9cdd4;}"));

    refreshDiamonds();
    m_strip->update();
    m_view->update();
}

void PancropWidget::setProject(Project* p) {
    m_project = p;
    m_clipId.clear();
    m_frame = QImage();
    m_framePath.clear();
    m_undoPushed = false;
    m_view->update();
}

void PancropWidget::setClipId(const QString& id) {
    if (m_clipId == id) return;
    m_clipId = id;
    m_undoPushed = false;
    loadFrame();
    syncFromClip();
    m_view->update();
    m_strip->update();
}

void PancropWidget::setPlayhead(double t) {
    m_playhead = t;
    syncFromClip();
    m_view->update();
    m_strip->update();
}

void PancropWidget::refresh() {
    loadFrame();
    syncFromClip();
    m_view->update();
}

void PancropWidget::sync() {
    syncFromClip();
    m_view->update();
}

Clip* PancropWidget::activeClip() {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    return nullptr;
}

void PancropWidget::loadFrame() {
    Clip* c = activeClip();
    if (!c) { m_frame = QImage(); m_framePath.clear(); return; }
    const MediaItem* mi = m_project->findMedia(c->mediaId);
    if (!mi || !mi->hasVideo) { m_frame = QImage(); m_framePath.clear(); return; }
    if (!m_decoder.isOpen() || m_decoder.source() != mi->filePath)
        m_decoder.open(mi->filePath);
    if (!m_decoder.isOpen()) { m_frame = QImage(); m_framePath.clear(); return; }
    const double t = std::max(0.0, c->in + 0.05);
    m_frame = m_decoder.frameAt(t, 720);
    m_decoder.releaseBuffers();
    m_framePath = mi->filePath;
}

void PancropWidget::syncFromClip() {
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

    QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB);
    QSignalBlocker b5(m_scale), b6(m_panX), b7(m_panY), b8(m_rotation);
    m_cropL->setValue((int)std::lround(L * 100.0));
    m_cropR->setValue((int)std::lround(R * 100.0));
    m_cropT->setValue((int)std::lround(T * 100.0));
    m_cropB->setValue((int)std::lround(B * 100.0));
    m_scale->setValue((int)std::lround(std::clamp(s, kMinScale, kMaxScale) * 100.0));
    m_panX->setValue((int)std::lround(std::clamp(tx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(ty / H * 100.0, -100.0, 100.0)));
    m_rotation->setValue((int)std::lround(std::clamp(rot, -double(kRotMax), double(kRotMax))));
    updateValueLabels();
    refreshDiamonds();
}

void PancropWidget::updateValueLabels() {
    const int L = m_cropL->value();
    const int R = m_cropR->value();
    const int T = m_cropT->value();
    const int B = m_cropB->value();
    const int sc = m_scale->value();
    const int px = m_panX->value();
    const int py = m_panY->value();
    const int rot = m_rotation->value();
    auto fmt = [](int v) { return QStringLiteral("%1%").arg(v); };
    if (m_cropLVal) m_cropLVal->setText(fmt(L));
    if (m_cropRVal) m_cropRVal->setText(fmt(R));
    if (m_cropTVal) m_cropTVal->setText(fmt(T));
    if (m_cropBVal) m_cropBVal->setText(fmt(B));
    if (m_scaleVal) m_scaleVal->setText(fmt(sc));
    if (m_panXVal) m_panXVal->setText(fmt(px));
    if (m_panYVal) m_panYVal->setText(fmt(py));
    if (m_rotationVal) m_rotationVal->setText(QStringLiteral("%1°").arg(rot));
}

double PancropWidget::relPlayhead() {
    Clip* c = activeClip();
    if (!c) return -1.0;
    return std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur));
}

QVector<Keyframe>* PancropWidget::keyframesFor(int prop) {
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
    }
    return nullptr;
}

double PancropWidget::propValue(int prop) const {
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
    }
    return 0.0;
}

void PancropWidget::toggleKeyframe(int prop) {
    QVector<Keyframe>* kf = keyframesFor(prop);
    if (!kf) return;
    const double rel = relPlayhead();
    if (rel < 0.0) return;
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    // Sempre informa a propriedade editada, para o editor de curvas exibir a
    // curva correspondente (independente de undo pendente).
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

void PancropWidget::writeKeyframe(int prop, double value) {
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

void PancropWidget::gotoKeyframe(int dir) {
    Clip* c = activeClip();
    if (!c) return;
    const int props[] = {P_CropL, P_CropR, P_CropT, P_CropB,
                         P_Scale, P_PanX, P_PanY, P_Rotation};
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

void PancropWidget::refreshDiamonds() {
    const double rel = relPlayhead();
    const QString on =
        QStringLiteral("QPushButton{color:#ffb340; border:none; background:transparent; font-size:14px;}"
                       "QPushButton:hover{color:#ffd080;}");
    const QString off =
        QStringLiteral("QPushButton{color:#565d68; border:none; background:transparent; font-size:14px;}"
                       "QPushButton:hover{color:#c9d4e2;}");
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

void PancropWidget::commitSlider(int prop, double baseValue) {
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
    }
    if (m_kfAuto && m_kfAuto->isChecked())
        writeKeyframe(prop, baseValue);
    else if (QVector<Keyframe>* kf = keyframesFor(prop))
        kf->clear();
    // modified() DEPOIS de escrever o keyframe, senão o editor de curvas
    // atualiza com dados antigos e o keyframe novo nunca aparece.
    emit modified();
    refreshDiamonds();
}

void PancropWidget::setCropValues(double L, double R, double T, double B) {
    Clip* c = activeClip();
    if (!c) return;
    L = std::clamp(L, 0.0, 0.85);
    R = std::clamp(R, 0.0, 0.85 - L);
    T = std::clamp(T, 0.0, 0.85);
    B = std::clamp(B, 0.0, 0.85 - T);
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

void PancropWidget::emitChange() {
    if (!m_undoPushed) {
        emit editStart();
        m_undoPushed = true;
    }
    emit modified();
}

void PancropWidget::computeView(double s, double tx, double ty, int w0, int h0,
                                QRectF* cropS, QRectF* outS) const {
    if (w0 <= 0 || h0 <= 0) return;
    const double L = std::clamp(m_cropL->value() / 100.0, 0.0, 0.85);
    const double R = std::clamp(m_cropR->value() / 100.0, 0.0, 0.85 - L);
    const double T = std::clamp(m_cropT->value() / 100.0, 0.0, 0.85);
    const double B = std::clamp(m_cropB->value() / 100.0, 0.0, 0.85 - T);
    const double kx = L * w0;
    const double ky = T * h0;
    const double kw = std::max(1.0, w0 * (1.0 - L - R));
    const double kh = std::max(1.0, h0 * (1.0 - T - B));
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double k = std::max(W / kw, H / kh);
    const double ks = std::max(k * s, 1e-6);
    const double w = W / ks;
    const double h = H / ks;
    *cropS = QRectF(kx, ky, kw, kh);
    // Janela de saída: a região do source que o export mostra. k é a escala
    // "cover" do recorte no quadro do projeto (W×H); o centro sem pan é a
    // origem do cover, kx + W/(2k) (igual ao centro do recorte quando as
    // proporções coincidem). O zoom s encolhe a janela; o pan desloca o
    // centro por -tx/ks, -ty/ks.
    const double ox = kx + W / (2.0 * k);
    const double oy = ky + H / (2.0 * k);
    *outS = QRectF(ox - w / 2.0 - tx / ks,
                   oy - h / 2.0 - ty / ks,
                   w, h);
}

// Rotaciona um ponto da tela de volta para o espaço não-rotacionado do
// viewfinder, girando por -deg em torno do centro da área de exibição (disp).
// É o inverso do p.rotate(deg) usado no paintViewfinder, para que o hit-test
// e o arraste continuem funcionando com a imagem girada.
QPointF PancropWidget::rotatedViewPos(const QRect& viewRect, const QPoint& sp) const {
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return QPointF(sp);
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

void PancropWidget::screenToSource(const QRect& viewRect, const QPoint& sp,
                                   double* sx, double* sy) const {
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;
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
    // Desfaz a rotação antes de mapear para o espaço do source, senão o
    // recorte/pan "descolam" do mouse quando a imagem está girada.
    const QPointF rp = rotatedViewPos(viewRect, sp);
    *sx = (rp.x() - disp.left()) / disp.width() * w0;
    *sy = (rp.y() - disp.top()) / disp.height() * h0;
}

void PancropWidget::applyPan(double sx, double sy) {
    Clip* c = activeClip();
    if (!c || m_frame.isNull()) return;
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    const double L = std::clamp(m_cropL->value() / 100.0, 0.0, 0.85);
    const double R = std::clamp(m_cropR->value() / 100.0, 0.0, 0.85 - L);
    const double T = std::clamp(m_cropT->value() / 100.0, 0.0, 0.85);
    const double B = std::clamp(m_cropB->value() / 100.0, 0.0, 0.85 - T);
    const double kx = L * w0;
    const double ky = T * h0;
    const double kw = std::max(1.0, w0 * (1.0 - L - R));
    const double kh = std::max(1.0, h0 * (1.0 - T - B));
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double k = std::max(W / kw, H / kh);
    const double s = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
    const double ks = k * s;
    const double w = W / ks;
    const double h = H / ks;

    const double ox = kx + W / (2.0 * k);
    const double oy = ky + H / (2.0 * k);
    double cx = sx;
    double cy = sy;
    if (w >= kw) cx = ox;
    else cx = std::clamp(cx, kx + w / 2.0, kx + kw - w / 2.0);
    if (h >= kh) cy = oy;
    else cy = std::clamp(cy, ky + h / 2.0, ky + kh - h / 2.0);

    const double tx = (ox - cx) * ks;
    const double ty = (oy - cy) * ks;

    QSignalBlocker b6(m_panX), b7(m_panY);
    m_panX->setValue((int)std::lround(std::clamp(tx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(ty / H * 100.0, -100.0, 100.0)));
    commitSlider(P_PanX, tx);
    commitSlider(P_PanY, ty);
    m_view->update();
}

void PancropWidget::paintViewfinder(QWidget* view) {
    QPainter p(view);
    p.fillRect(view->rect(), QColor(14, 15, 18));
    // Borda discreta do "monitor" (como o programa monitor do Premiere).
    p.setPen(QPen(QColor(38, 41, 48), 1));
    p.drawRect(view->rect().adjusted(0, 0, -1, -1));
    if (m_frame.isNull()) {
        p.setPen(QColor(140, 140, 150));
        p.drawText(view->rect(), Qt::AlignCenter,
                   tr("Selecione um clipe de vídeo para editar o pancrop."));
        return;
    }
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;

    const int M = 12;
    const QRectF screen = QRectF(view->rect()).adjusted(M, M, -M, -M);
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
    // Gira a cena inteira (imagem + recorte + alças + janela de saída) em
    // torno do centro da área de exibição. Sem clipRect para os cantos da
    // imagem girada aparecerem sobre o fundo escuro.
    const double rot = m_rotation->value();
    p.translate(disp.center());
    p.rotate(rot);
    p.translate(-disp.center());
    p.drawImage(QRectF(disp.left(), disp.top(), disp.width(), disp.height()), m_frame);

    // Escurece fora da área recortada.
    QPainterPath shp;
    shp.addRect(disp);
    QPainterPath hole;
    hole.addRect(QRectF(toDisp(cropS.left(), cropS.top()),
                        toDisp(cropS.right(), cropS.bottom())));
    p.fillPath(shp.subtracted(hole), QColor(0, 0, 0, 150));

    // Contorno do crop (branco tracejado, como o Premiere).
    p.setPen(QPen(QColor(235, 238, 242), 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    const QRectF cropDisp(toDisp(cropS.left(), cropS.top()),
                          toDisp(cropS.right(), cropS.bottom()));
    p.drawRect(cropDisp);

    // Alças de redimensionamento do crop (quadrados brancos).
    p.setPen(QPen(QColor(18, 20, 24), 1));
    p.setBrush(QColor(235, 238, 242));
    const double hd = 6.0;
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
        p.drawRect(QRectF(h.x() - hd / 2.0, h.y() - hd / 2.0, hd, hd));

    // Janela de saída (o que o quadro final mostra).
    const QRectF outDisp(toDisp(outS.left(), outS.top()),
                         toDisp(outS.right(), outS.bottom()));
    p.setPen(QPen(QColor(255, 255, 255), 2));
    p.setBrush(QColor(255, 255, 255, 18));
    p.drawRect(outDisp);

    p.restore();
}

void PancropWidget::viewportPress(QWidget* view, QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || m_frame.isNull()) {
        e->ignore();
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

    const int M = 12;
    const QRectF screen = QRectF(view->rect()).adjusted(M, M, -M, -M);
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

    // Só inicia o pan se o clique começar dentro (ou perto) da janela de saída.
    if (outS.adjusted(-0.05 * outS.width(), -0.05 * outS.height(),
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

void PancropWidget::viewportMove(QWidget* view, QMouseEvent* e) {
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
    const double L0 = std::clamp(m_cropL->value() / 100.0, 0.0, 0.85);
    const double R0 = std::clamp(m_cropR->value() / 100.0, 0.0, 0.85);
    const double T0 = std::clamp(m_cropT->value() / 100.0, 0.0, 0.85);
    const double B0 = std::clamp(m_cropB->value() / 100.0, 0.0, 0.85);
    switch (m_dragMode) {
        case DragPan:
            applyPan(sx + m_grabOffset.x(), sy + m_grabOffset.y());
            break;
        case DragCropL:  setCropValues(sx / w0, R0, T0, B0); break;
        case DragCropR:  setCropValues(L0, (w0 - sx) / w0, T0, B0); break;
        case DragCropT:  setCropValues(L0, R0, sy / h0, B0); break;
        case DragCropB:  setCropValues(L0, R0, T0, (h0 - sy) / h0); break;
        case DragCropTL: setCropValues(sx / w0, R0, sy / h0, B0); break;
        case DragCropTR: setCropValues(L0, (w0 - sx) / w0, sy / h0, B0); break;
        case DragCropBL: setCropValues(sx / w0, R0, T0, (h0 - sy) / h0); break;
        case DragCropBR: setCropValues(L0, (w0 - sx) / w0, T0, (h0 - sy) / h0); break;
        default: break;
    }
}

void PancropWidget::viewportRelease(QWidget* view, QMouseEvent* e) {
    if (m_dragMode != DragNone) {
        m_dragMode = DragNone;
        view->setCursor(Qt::ArrowCursor);
        m_undoPushed = false;
    }
    e->accept();
}

void PancropWidget::viewportWheel(QWidget* view, QWheelEvent* e) {
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

    const double L = std::clamp(m_cropL->value() / 100.0, 0.0, 0.85);
    const double R = std::clamp(m_cropR->value() / 100.0, 0.0, 0.85 - L);
    const double T = std::clamp(m_cropT->value() / 100.0, 0.0, 0.85);
    const double B = std::clamp(m_cropB->value() / 100.0, 0.0, 0.85 - T);
    const double kx = L * w0;
    const double ky = T * h0;
    const double kw = std::max(1.0, w0 * (1.0 - L - R));
    const double kh = std::max(1.0, h0 * (1.0 - T - B));
    const double k = std::max(W / kw, H / kh);
    const double ks = k * sNew;
    const double ntx = (kx + W / (2.0 * k) - center.x()) * ks;
    const double nty = (ky + H / (2.0 * k) - center.y()) * ks;

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

// --- Faixa de tempo dos keyframes (arraste esquerda/direita) ---

// Converte posição relativa do playhead/keyframe (0..clip.dur) em pixel na strip.
static int kfStripX(double rel, const QWidget* view, double dur) {
    const int w = view->width();
    if (dur <= 0.0) return 0;
    const double frac = std::clamp(rel / dur, 0.0, 1.0);
    return 4 + (int)std::lround(frac * (w - 8));
}

void PancropWidget::paintKeyframeStrip(QWidget* view) {
    QPainter p(view);
    p.fillRect(view->rect(), QColor(19, 20, 24));
    Clip* c = activeClip();
    if (!c) return;
    const double dur = std::max(c->dur, 1e-3);
    const int w = view->width();
    const int h = view->height();

    // Linha do tempo discreta, como nas mini-tiras do editor de curvas.
    p.setPen(QColor(46, 49, 56));
    p.drawLine(0, h / 2, w, h / 2);

    // Keyframes: coleta os tempos únicos de todas as propriedades.
    const int props[] = {P_CropL, P_CropR, P_CropT, P_CropB,
                         P_Scale, P_PanX, P_PanY, P_Rotation};
    QVector<double> times;
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (const Keyframe& k : *kf) {
            if (!times.contains(k.time))
                times.append(k.time);
        }
    }
    std::sort(times.begin(), times.end());
    const double rel = relPlayhead();

    // Linha do playhead (igual à das mini-tiras do editor de curvas).
    if (rel >= 0.0) {
        const int px = kfStripX(rel, view, dur);
        p.setPen(QPen(QColor(190, 220, 255, 200), 1));
        p.drawLine(px, 2, px, h - 2);
    }

    for (double t : times) {
        const int x = kfStripX(t, view, dur);
        const bool active = rel >= 0.0 && std::fabs(t - rel) < 1e-6;
        const bool sel = m_selectedTimes.contains(t);
        QPolygon dia = QPolygon()
            << QPoint(x, h / 2 - 6) << QPoint(x + 5, h / 2)
            << QPoint(x, h / 2 + 6) << QPoint(x - 5, h / 2);
        p.setPen(QPen(QColor(10, 10, 12), 1));
        // Selecionado = verde; no playhead = laranja; senão cinza (mesmos
        // critérios de cor do editor de curvas).
        p.setBrush(sel ? QColor(120, 230, 150)
                       : active ? QColor(255, 179, 64)
                                : QColor(200, 205, 215));
        p.drawPolygon(dia);
    }
}

void PancropWidget::stripPress(QWidget* view, QMouseEvent* e) {
    m_stripDragging = false;
    m_stripPlayheadDrag = false;
    Clip* c = activeClip();
    if (e->button() != Qt::LeftButton || !c) return;
    const double dur = std::max(c->dur, 1e-3);
    const int midY = view->height() / 2;

    // Encontra o keyframe mais próximo do clique. O hit é limitado à zona do
    // losango (vertical) para não roubar o arraste da agulha quando o playhead
    // está exatamente sobre um keyframe.
    const int props[] = {P_CropL, P_CropR, P_CropT, P_CropB,
                         P_Scale, P_PanX, P_PanY, P_Rotation};
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
        if (dist <= 9 && std::abs(e->pos().y() - midY) <= 12 && dist < bestDist) {
            bestDist = dist; bestIdx = i;
        }
    }
    if (bestIdx >= 0) {
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
    m_stripPlayheadDrag = true;
    const double frac = std::clamp((double)(e->pos().x() - 4) / (view->width() - 8),
                                   0.0, 1.0);
    emit keyframeJump(c->pos + frac * dur);
    m_strip->update();
    e->accept();
}

void PancropWidget::stripMove(QWidget* view, QMouseEvent* e) {
    Clip* c = activeClip();
    if (!c) return;
    const double dur = std::max(c->dur, 1e-3);
    const double frac = std::clamp((double)(e->pos().x() - 4) / (view->width() - 8),
                                   0.0, 1.0);
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

    // Move o tempo de TODAS as propriedades que tinham keyframe na posição
    // original (preserva o agrupamento visual no playhead).
    const int props[] = {P_CropL, P_CropR, P_CropT, P_CropB,
                         P_Scale, P_PanX, P_PanY, P_Rotation};
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

void PancropWidget::stripRelease(QWidget* view, QMouseEvent* e) {
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
void PancropWidget::stripDoubleClick(QWidget* view, QMouseEvent* e) {
    Clip* c = activeClip();
    if (!c) { e->ignore(); return; }
    const double dur = std::max(c->dur, 1e-3);
    const double frac = std::clamp((double)(e->pos().x() - 4) / (view->width() - 8),
                                   0.0, 1.0);
    const double rel = frac * dur;

    const int props[] = {P_CropL, P_CropR, P_CropT, P_CropB,
                         P_Scale, P_PanX, P_PanY, P_Rotation};
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
void PancropWidget::deleteSelectedKeyframes() {
    Clip* c = activeClip();
    if (!c || m_selectedTimes.isEmpty()) return;
    const int props[] = {P_CropL, P_CropR, P_CropT, P_CropB,
                         P_Scale, P_PanX, P_PanY, P_Rotation};
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
void PancropWidget::stripContextMenu(QWidget* view, QContextMenuEvent* e) {
    Clip* c = activeClip();
    if (!c) { e->ignore(); return; }
    const double dur = std::max(c->dur, 1e-3);

    const int props[] = {P_CropL, P_CropR, P_CropT, P_CropB,
                         P_Scale, P_PanX, P_PanY, P_Rotation};
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
    QAction* lin = menu.addAction(tr("Linear (padrão)"));
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
