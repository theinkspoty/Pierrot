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
#include <algorithm>
#include <cmath>

namespace {
constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 4.0;
constexpr int kCropMax = 80;   // %
constexpr int kPanMax = 100;   // % da largura/altura do projeto

enum Prop { P_CropL, P_CropR, P_CropT, P_CropB, P_Scale, P_PanX, P_PanY };
}

PancropWidget::PancropWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 400);

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
        l->setStyleSheet(QStringLiteral("color:#88c0ff; font-weight:bold;"));
        return l;
    };
    auto makeDiamond = [this](int prop) {
        auto* b = new QPushButton(QStringLiteral("◆"), this);
        b->setFixedSize(22, 22);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tr("Alternar keyframe no playhead"));
        b->setStyleSheet(QStringLiteral(
            "QPushButton{color:#5a5a66; border:none; background:transparent; font-size:13px;}"
            "QPushButton:hover{color:#ffffff;}"));
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
    m_cropLVal = makeVal(); m_cropRVal = makeVal(); m_cropTVal = makeVal();
    m_cropBVal = makeVal(); m_scaleVal = makeVal(); m_panXVal = makeVal();
    m_panYVal = makeVal();

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
            }
            updateValueLabels();
            update();
        });
    };
    connectSlider(m_cropL, P_CropL);
    connectSlider(m_cropR, P_CropR);
    connectSlider(m_cropT, P_CropT);
    connectSlider(m_cropB, P_CropB);
    connectSlider(m_scale, P_Scale);
    connectSlider(m_panX, P_PanX);
    connectSlider(m_panY, P_PanY);

    auto presetBtn = [this](const QString& label, const QString& tip, int idx) {
        auto* b = new QPushButton(label, this);
        b->setMinimumHeight(22);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, [this, idx]() {
            applyPreset(idx);
            update();
        });
        return b;
    };
    auto* presets = new QGridLayout;
    presets->setSpacing(4);
    presets->setContentsMargins(0, 0, 0, 0);
    presets->addWidget(presetBtn(tr("Zoom-in"), tr("Zoom-in suave"), 1), 0, 0);
    presets->addWidget(presetBtn(tr("Zoom-out"), tr("Zoom-out suave"), 2), 0, 1);
    presets->addWidget(presetBtn(tr("Pan →"), tr("Pan da esquerda para a direita"), 3), 0, 2);
    presets->addWidget(presetBtn(tr("Pan ←"), tr("Pan da direita para a esquerda"), 4), 0, 3);
    presets->addWidget(presetBtn(tr("Pan ↓"), tr("Pan de cima para baixo"), 5), 1, 0);
    presets->addWidget(presetBtn(tr("Pan ↑"), tr("Pan de baixo para cima"), 6), 1, 1);
    presets->addWidget(presetBtn(tr("Diag ↘"), tr("Diagonal para a direita/baixo"), 7), 1, 2);
    presets->addWidget(presetBtn(tr("Diag ↙"), tr("Diagonal para a esquerda/baixo"), 8), 1, 3);
    presets->addWidget(presetBtn(tr("Limpar"), tr("Sem animação (transformação fixa)"), 0), 2, 0, 1, 2);
    auto* resetBtn = new QPushButton(tr("Redefinir"), this);
    resetBtn->setMinimumHeight(22);
    resetBtn->setToolTip(tr("Zera recorte, zoom e posição do clipe"));
    connect(resetBtn, &QPushButton::clicked, this, [this]() {
        QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB),
                       b5(m_scale), b6(m_panX), b7(m_panY);
        m_cropL->setValue(0); m_cropR->setValue(0);
        m_cropT->setValue(0); m_cropB->setValue(0);
        m_scale->setValue(100); m_panX->setValue(0); m_panY->setValue(0);
        Clip* c = activeClip();
        if (c) {
            c->kfCropL.clear(); c->kfCropR.clear();
            c->kfCropT.clear(); c->kfCropB.clear();
            c->cropL = c->cropR = c->cropT = c->cropB = 0.0;
            c->kfScale.clear();
            c->scale = 1.0;
            c->kfTx.clear(); c->kfTy.clear();
            c->tx = c->ty = 0.0;
        }
        m_undoPushed = false;
        updateValueLabels();
        update();
        if (c) emitChange();
    });
    presets->addWidget(resetBtn, 2, 2, 1, 2);

    auto* hint = new QLabel(tr("Dica: arraste dentro da caixa branca para mover "
                               "(pan), arraste as bordas/alças para recortar e use "
                               "a roda do mouse para dar zoom."),
                            this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#999; font-size:11px;"));

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

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(5);
    lay->addLayout(kfBar);
    lay->addWidget(cropBox);
    lay->addWidget(moveBox);
    lay->addLayout(presets);
    lay->addWidget(hint);
    lay->addStretch(1);

    refreshDiamonds();
}

void PancropWidget::setProject(Project* p) {
    m_project = p;
    m_clipId.clear();
    m_frame = QImage();
    m_framePath.clear();
    m_undoPushed = false;
    update();
}

void PancropWidget::setClipId(const QString& id) {
    if (m_clipId == id) return;
    m_clipId = id;
    m_undoPushed = false;
    loadFrame();
    syncFromClip();
    update();
}

void PancropWidget::setPlayhead(double t) {
    m_playhead = t;
    syncFromClip();
    update();
}

void PancropWidget::refresh() {
    loadFrame();
    syncFromClip();
    update();
}

void PancropWidget::sync() {
    syncFromClip();
    update();
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

    QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB);
    QSignalBlocker b5(m_scale), b6(m_panX), b7(m_panY);
    m_cropL->setValue((int)std::lround(L * 100.0));
    m_cropR->setValue((int)std::lround(R * 100.0));
    m_cropT->setValue((int)std::lround(T * 100.0));
    m_cropB->setValue((int)std::lround(B * 100.0));
    m_scale->setValue((int)std::lround(std::clamp(s, kMinScale, kMaxScale) * 100.0));
    m_panX->setValue((int)std::lround(std::clamp(tx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(ty / H * 100.0, -100.0, 100.0)));
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
    auto fmt = [](int v) { return QStringLiteral("%1%").arg(v); };
    if (m_cropLVal) m_cropLVal->setText(fmt(L));
    if (m_cropRVal) m_cropRVal->setText(fmt(R));
    if (m_cropTVal) m_cropTVal->setText(fmt(T));
    if (m_cropBVal) m_cropBVal->setText(fmt(B));
    if (m_scaleVal) m_scaleVal->setText(fmt(sc));
    if (m_panXVal) m_panXVal->setText(fmt(px));
    if (m_panYVal) m_panYVal->setText(fmt(py));
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
    }
    return 0.0;
}

void PancropWidget::toggleKeyframe(int prop) {
    QVector<Keyframe>* kf = keyframesFor(prop);
    if (!kf) return;
    const double rel = relPlayhead();
    if (rel < 0.0) return;
    for (int i = 0; i < kf->size(); ++i) {
        if (std::fabs((*kf)[i].time - rel) < 1e-6) {
            if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
            kf->removeAt(i);
            emitChange();
            refreshDiamonds();
            update();
            return;
        }
    }
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    Keyframe k;
    k.time = rel;
    k.value = propValue(prop);
    k.interp = KfSmooth;
    kf->append(k);
    std::sort(kf->begin(), kf->end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    emitChange();
    refreshDiamonds();
    update();
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
                         P_Scale, P_PanX, P_PanY};
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
        QStringLiteral("QPushButton{color:#ffb340; border:none; background:transparent; font-size:13px;}"
                       "QPushButton:hover{color:#ffd080;}");
    const QString off =
        QStringLiteral("QPushButton{color:#5a5a66; border:none; background:transparent; font-size:13px;}"
                       "QPushButton:hover{color:#ffffff;}");
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
}

void PancropWidget::commitSlider(int prop, double baseValue) {
    Clip* c = activeClip();
    if (!c) return;
    emitChange();
    switch (prop) {
        case P_CropL: c->cropL = baseValue; break;
        case P_CropR: c->cropR = baseValue; break;
        case P_CropT: c->cropT = baseValue; break;
        case P_CropB: c->cropB = baseValue; break;
        case P_Scale: c->scale = baseValue; break;
        case P_PanX:  c->tx = baseValue; break;
        case P_PanY:  c->ty = baseValue; break;
    }
    if (m_kfAuto && m_kfAuto->isChecked())
        writeKeyframe(prop, baseValue);
    else if (QVector<Keyframe>* kf = keyframesFor(prop))
        kf->clear();
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
    update();
}

void PancropWidget::emitChange() {
    if (!m_undoPushed) {
        emit editStart();
        m_undoPushed = true;
    }
    emit modified();
}

void PancropWidget::applyPreset(int idx) {
    Clip* c = activeClip();
    if (!c) return;
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    const double T0 = 0.0;
    const double T1 = std::max(0.05, c->dur);
    const double rel = std::clamp(m_playhead - c->pos, 0.0, T1);
    const double s0 = kfValue(c->kfScale, c->scale, rel);
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;

    auto kf = [](QVector<Keyframe>& v, double t, double val) {
        Keyframe k;
        k.time = t;
        k.value = val;
        v.append(k);
    };

    QVector<Keyframe> ks, kx, ky;
    switch (idx) {
        case 0:
            c->kfScale.clear();
            c->kfTx.clear();
            c->kfTy.clear();
            break;
        case 1: // Zoom-in
            kf(ks, T0, s0);
            kf(ks, T1, s0 * 1.5);
            break;
        case 2: // Zoom-out
            kf(ks, T0, std::max(1.0, s0) * 1.4);
            kf(ks, T1, std::max(1.0, s0));
            break;
        case 3: // Pan L->R
            kf(ks, T0, std::max(1.2, s0));
            kf(ks, T1, std::max(1.2, s0));
            kf(kx, T0, -W * 0.10);
            kf(kx, T1, W * 0.10);
            break;
        case 4: // Pan R->L
            kf(ks, T0, std::max(1.2, s0));
            kf(ks, T1, std::max(1.2, s0));
            kf(kx, T0, W * 0.10);
            kf(kx, T1, -W * 0.10);
            break;
        case 5: // Pan T->B
            kf(ks, T0, std::max(1.2, s0));
            kf(ks, T1, std::max(1.2, s0));
            kf(ky, T0, -H * 0.10);
            kf(ky, T1, H * 0.10);
            break;
        case 6: // Pan B->T
            kf(ks, T0, std::max(1.2, s0));
            kf(ks, T1, std::max(1.2, s0));
            kf(ky, T0, H * 0.10);
            kf(ky, T1, -H * 0.10);
            break;
        case 7: // Diagonal ↘
            kf(ks, T0, 1.25);
            kf(ks, T1, 1.25);
            kf(kx, T0, -W * 0.12);
            kf(kx, T1, W * 0.12);
            kf(ky, T0, -H * 0.12);
            kf(ky, T1, H * 0.12);
            break;
        case 8: // Diagonal ↙
            kf(ks, T0, 1.25);
            kf(ks, T1, 1.25);
            kf(kx, T0, W * 0.12);
            kf(kx, T1, -W * 0.12);
            kf(ky, T0, -H * 0.12);
            kf(ky, T1, H * 0.12);
            break;
    }
    if (idx != 0) {
        c->kfScale = ks;
        c->kfTx = kx;
        c->kfTy = ky;
    }
    m_undoPushed = false;
    syncFromClip();
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
    const double cx = kx + kw / 2.0;
    const double cy = ky + kh / 2.0;
    const double w = W / ks;
    const double h = H / ks;
    *cropS = QRectF(kx, ky, kw, kh);
    *outS = QRectF(cx + kw / 2.0 - w / 2.0 - tx / ks,
                   cy + kh / 2.0 - h / 2.0 - ty / ks,
                   w, h);
}

void PancropWidget::screenToSource(const QPoint& sp, double* sx, double* sy) const {
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;
    const int M = 12;
    const QRectF screen = QRectF(rect()).adjusted(M, M, -M, -M);
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
    *sx = (sp.x() - disp.left()) / disp.width() * w0;
    *sy = (sp.y() - disp.top()) / disp.height() * h0;
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

    double cx = sx;
    double cy = sy;
    if (w >= kw) cx = kx + kw / 2.0;
    else cx = std::clamp(cx, kx + w / 2.0, kx + kw - w / 2.0);
    if (h >= kh) cy = ky + kh / 2.0;
    else cy = std::clamp(cy, ky + h / 2.0, ky + kh - h / 2.0);

    const double tx = (kx + kw - cx) * ks;
    const double ty = (ky + kh - cy) * ks;

    QSignalBlocker b6(m_panX), b7(m_panY);
    m_panX->setValue((int)std::lround(std::clamp(tx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(ty / H * 100.0, -100.0, 100.0)));
    commitSlider(P_PanX, tx);
    commitSlider(P_PanY, ty);
    update();
}

void PancropWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 34));
    if (m_frame.isNull()) {
        p.setPen(QColor(140, 140, 150));
        p.drawText(rect(), Qt::AlignCenter,
                   tr("Selecione um clipe de vídeo para editar o pancrop."));
        return;
    }
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;

    const int M = 12;
    const QRectF screen = QRectF(rect()).adjusted(M, M, -M, -M);
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
    p.setClipRect(disp);
    p.drawImage(QRectF(disp.left(), disp.top(), disp.width(), disp.height()), m_frame);

    // Escurece fora da área recortada.
    QPainterPath shp;
    shp.addRect(disp);
    QPainterPath hole;
    hole.addRect(QRectF(toDisp(cropS.left(), cropS.top()),
                        toDisp(cropS.right(), cropS.bottom())));
    p.fillPath(shp.subtracted(hole), QColor(0, 0, 0, 150));

    // Contorno do crop.
    p.setPen(QPen(QColor(90, 160, 255), 1.5, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    const QRectF cropDisp(toDisp(cropS.left(), cropS.top()),
                          toDisp(cropS.right(), cropS.bottom()));
    p.drawRect(cropDisp);

    // Alças de redimensionamento do crop.
    p.setPen(QPen(QColor(255, 255, 255), 1));
    p.setBrush(QColor(90, 160, 255));
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

void PancropWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || m_frame.isNull()) {
        QWidget::mousePressEvent(e);
        return;
    }
    double sx, sy;
    screenToSource(e->pos(), &sx, &sy);
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
    const QRectF screen = QRectF(rect()).adjusted(M, M, -M, -M);
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
    const QPointF pos = e->pos();
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
            case DragCropTL: case DragCropBR: setCursor(Qt::SizeFDiagCursor); break;
            case DragCropTR: case DragCropBL: setCursor(Qt::SizeBDiagCursor); break;
            case DragCropL:  case DragCropR:  setCursor(Qt::SizeHorCursor); break;
            case DragCropT:  case DragCropB:  setCursor(Qt::SizeVerCursor); break;
            default: break;
        }
        return;
    }

    // Só inicia o pan se o clique começar dentro (ou perto) da janela de saída.
    if (outS.adjusted(-0.05 * outS.width(), -0.05 * outS.height(),
                      0.05 * outS.width(), 0.05 * outS.height()).contains(QPointF(sx, sy))) {
        m_dragMode = DragPan;
        // Mantém a "pegada": guarda o offset entre o cursor e o centro da
        // janela para o movimento acompanhar o mouse 1:1 (sem pular ao centro).
        m_grabOffset = QPointF(outS.center().x() - sx, outS.center().y() - sy);
        setCursor(Qt::ClosedHandCursor);
        m_undoPushed = true;
        emit editStart();
        applyPan(sx + m_grabOffset.x(), sy + m_grabOffset.y());
    } else {
        QWidget::mousePressEvent(e);
    }
}

void PancropWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragMode == DragNone) {
        QWidget::mouseMoveEvent(e);
        return;
    }
    m_lastDrag = e->pos();
    double sx, sy;
    screenToSource(e->pos(), &sx, &sy);
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

void PancropWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragMode != DragNone) {
        m_dragMode = DragNone;
        setCursor(Qt::ArrowCursor);
        m_undoPushed = false;
    }
    QWidget::mouseReleaseEvent(e);
}

void PancropWidget::wheelEvent(QWheelEvent* e) {
    if (m_frame.isNull() || !activeClip()) {
        QWidget::wheelEvent(e);
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
    const double ntx = (kx + kw - center.x()) * ks;
    const double nty = (ky + kh - center.y()) * ks;

    QSignalBlocker b5(m_scale), b6(m_panX), b7(m_panY);
    m_scale->setValue((int)std::lround(sNew * 100.0));
    m_panX->setValue((int)std::lround(std::clamp(ntx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(nty / H * 100.0, -100.0, 100.0)));
    commitSlider(P_Scale, sNew);
    commitSlider(P_PanX, ntx);
    commitSlider(P_PanY, nty);
    updateValueLabels();
    update();
}
