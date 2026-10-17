// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AnimPresetDock.h"
#include "TimelineWidget.h"
#include "../colombina/models/Project.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QFont>

namespace {

// Helper: grava keyframes ABSOLUTOS (segundos) numa prop, substituindo os
// existentes; base = primeiro valor (mesmo critério do Lazzi).
struct K { double t; double v; int i; };

void setKf(QVector<Keyframe>& kf, double& base, const QVector<K>& keys) {
    kf.clear();
    for (const K& k : keys) upsertKeyframe(kf, k.t, k.v, k.i);
    if (!keys.isEmpty()) base = keys.first().v;
}

void setKf(QVector<Keyframe>& kf, const QVector<K>& keys) {
    double dummy = 0.0;
    setKf(kf, dummy, keys);
}

using PresetFn = void (*)(Clip&, double W, double H);

// S ≈ 1 (0.95) — atalho de leitura dos easings.
constexpr int Sm = KfSmooth, El = KfElastic, Bo = KfBounce, Bk = KfBack;

// ─── ENTRADAS ─────────────────────────────────────────────────────────────
void pPop(Clip& c, double, double H) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.12, d * 0.1), 1, KfLinear}});
    setKf(c.kfScale, c.scale, {{0, 0, El}, {d * 0.35, 1, KfLinear}});
    setKf(c.kfTy, c.ty, {{0, -0.16 * H, El}, {d * 0.35, 0, KfLinear}});
}

void pSlideEsq(Clip& c, double W, double) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.12, d * 0.1), 1, KfLinear}});
    setKf(c.kfTx, c.tx, {{0, -0.40 * W, Bk}, {d * 0.32, 0, KfLinear}});
}

void pSlideDir(Clip& c, double W, double) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.12, d * 0.1), 1, KfLinear}});
    setKf(c.kfTx, c.tx, {{0, 0.40 * W, Bk}, {d * 0.32, 0, KfLinear}});
}

void pCai(Clip& c, double, double H) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.1, d * 0.08), 1, KfLinear}});
    setKf(c.kfTy, c.ty, {{0, -0.34 * H, El}, {d * 0.32, 0, KfLinear}});
    setKf(c.kfScaleX, c.scaleX, {{d * 0.18, 1.25, Bk}, {d * 0.32, 1, KfLinear}});
    setKf(c.kfScaleY, c.scaleY, {{d * 0.18, 0.75, Bk}, {d * 0.32, 1, KfLinear}});
}

void pZoomBack(Clip& c, double, double) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.15, d * 0.12), 1, KfLinear}});
    setKf(c.kfScale, c.scale, {{0, 1.7, Bk}, {d * 0.35, 1, KfLinear}});
}

void pFadeUp(Clip& c, double, double H) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {d * 0.22, 1, KfLinear}});
    setKf(c.kfTy, c.ty, {{0, 0.10 * H, Sm}, {d * 0.28, 0, KfLinear}});
}

// ─── SAÍDAS ───────────────────────────────────────────────────────────────
void pSairPop(Clip& c, double, double) {
    const double t0 = qMax(0.0, c.dur - qBound(0.5, c.dur * 0.25, 1.0));
    setKf(c.kfScale, c.scale, {{0, 1, KfLinear}, {t0, 1, El}, {c.dur, 1.55, KfLinear}});
    setKf(c.kfOpacity, c.opacity, {{0, 1, KfLinear}, {t0, 1, Sm}, {c.dur, 0, KfLinear}});
}

void pSairDir(Clip& c, double W, double) {
    const double t0 = qMax(0.0, c.dur - qBound(0.5, c.dur * 0.25, 1.0));
    setKf(c.kfTx, c.tx, {{0, 0, KfLinear}, {t0, 0, Bk}, {c.dur, 0.45 * W, KfLinear}});
    setKf(c.kfOpacity, c.opacity, {{0, 1, KfLinear}, {t0, 1, Sm}, {c.dur, 0, KfLinear}});
}

void pSairSobe(Clip& c, double, double H) {
    const double t0 = qMax(0.0, c.dur - qBound(0.5, c.dur * 0.25, 1.0));
    setKf(c.kfTy, c.ty, {{0, 0, KfLinear}, {t0, 0, El}, {c.dur, -0.35 * H, KfLinear}});
    setKf(c.kfOpacity, c.opacity, {{0, 1, KfLinear}, {t0, 1, Sm}, {c.dur, 0, KfLinear}});
}

// ─── IDLE (laço até o fim do clipe) ───────────────────────────────────────
void pRespira(Clip& c, double, double) {
    const double passo = 1.4;
    QVector<K> kf{{0, 1, KfLinear}};
    for (double t = 0; t + passo < c.dur; t += passo) {
        kf << K{t, 1, Bo} << K{t + passo * 0.45, 1.06, Sm} << K{t + passo, 1, KfLinear};
    }
    setKf(c.kfScale, c.scale, kf);
}

void pWiggle(Clip& c, double, double) {
    const double passo = 1.0;
    QVector<K> kf{{0, 0, KfLinear}};
    bool up = true;
    for (double t = 0; t + passo < c.dur; t += passo) {
        kf << K{t, up ? -3.5 : 3.5, Sm} << K{t + passo, up ? 3.5 : -3.5, KfLinear};
        up = !up;
    }
    kf << K{qMax(0.0, c.dur - 0.2), 0, Sm};
    setKf(c.kfRotation, c.rotation, kf);
}

void pFlutua(Clip& c, double, double H) {
    const double amp = 0.03 * H;
    const double passo = 2.0;
    QVector<K> kfY{{0, 0, KfLinear}}, kfX{{0, 0, KfLinear}};
    bool up = true;
    for (double t = 0; t + passo < c.dur; t += passo) {
        const double dir = up ? 1.0 : -1.0;
        kfY << K{t, dir * amp, Sm} << K{t + passo, -dir * amp, KfLinear};
        kfX << K{t, dir * amp * 0.5, Sm} << K{t + passo, -dir * amp * 0.5, KfLinear};
        up = !up;
    }
    setKf(c.kfTy, c.ty, kfY);
    setKf(c.kfTx, c.tx, kfX);
}

// ─── EFEITOS ──────────────────────────────────────────────────────────────
void pGiro(Clip& c, double, double) {
    const double d = qBound(0.8, c.dur, 3.0);
    setKf(c.kfRotation, c.rotation, {{0, 0, KfLinear}, {d * 0.75, 360, Sm}});
    setKf(c.kfScale, c.scale, {{0, 0.4, El}, {d * 0.4, 1, KfLinear}});
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.15, d * 0.1), 1, KfLinear}});
}

void pSquash(Clip& c, double, double) {
    const double passo = 1.2;
    QVector<K> sx{{0, 1, KfLinear}}, sy{{0, 1, KfLinear}};
    for (double t = 0; t + passo < c.dur; t += passo) {
        sx << K{t, 1, Bk} << K{t + passo * 0.3, 1.18, Sm} << K{t + passo * 0.65, 0.92, Sm}
           << K{t + passo, 1, KfLinear};
        sy << K{t, 1, Bk} << K{t + passo * 0.3, 0.85, Sm} << K{t + passo * 0.65, 1.08, Sm}
           << K{t + passo, 1, KfLinear};
    }
    setKf(c.kfScaleX, c.scaleX, sx);
    setKf(c.kfScaleY, c.scaleY, sy);
}

void pFlicker(Clip& c, double, double) {
    const double passo = 0.5;
    QVector<K> kf{{0, 1, KfLinear}};
    bool on = true;
    for (double t = 0; t + passo < c.dur; t += passo) {
        kf << K{t, on ? 1.0 : 0.25, KfStep};
        on = !on;
    }
    kf << K{qMax(0.0, c.dur - 0.15), 1, KfLinear};
    setKf(c.kfOpacity, c.opacity, kf);
}

void pSemear(Clip& c, double W, double) {
    // Stagger lateral: entra pela esquerda com overshoot e assenta.
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.12, d * 0.1), 1, KfLinear}});
    setKf(c.kfTx, c.tx, {{0, -0.55 * W, El}, {d * 0.30, 0.03 * W, Sm},
                         {d * 0.42, -0.01 * W, Sm}, {d * 0.52, 0, KfLinear}});
    setKf(c.kfRotation, c.rotation, {{0, -12, El}, {d * 0.40, 0, KfLinear}});
}

struct Preset {
    const char* name;
    PresetFn fn;
};

const Preset kEntradas[] = {
    {"Pop Elástico", pPop},
    {"Slide Esquerda", pSlideEsq},
    {"Slide Direita", pSlideDir},
    {"Cai de Cima", pCai},
    {"Zoom Back", pZoomBack},
    {"Fade Up", pFadeUp},
    {"Semeia (AE)", pSemear},
};

const Preset kSaidas[] = {
    {"Sair Pop", pSairPop},
    {"Sair p/ Direita", pSairDir},
    {"Sair Subindo", pSairSobe},
};

const Preset kIdle[] = {
    {"Respiração", pRespira},
    {"Wiggle", pWiggle},
    {"Flutuação", pFlutua},
};

const Preset kEfeitos[] = {
    {"Giro 360", pGiro},
    {"Squash & Stretch", pSquash},
    {"Flicker", pFlicker},
};

QPushButton* makePresetBtn(const QString& name, AnimPresetDock* dock) {
    auto* b = new QPushButton(name, dock);
    b->setObjectName(QStringLiteral("presetBtn_") + name);
    b->setFocusPolicy(Qt::NoFocus);
    QObject::connect(b, &QPushButton::clicked, dock, [dock, name]() {
        dock->applyPreset(name);
    });
    return b;
}

QGroupBox* makeGroup(const QString& title, const Preset* list, int n,
                     AnimPresetDock* dock, const QString& tint) {
    auto* box = new QGroupBox(title, dock);
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(8, 14, 8, 8);
    grid->setSpacing(4);
    const int cols = 2;
    for (int i = 0; i < n; ++i) {
        auto* b = makePresetBtn(QString::fromUtf8(list[i].name), dock);
        b->setStyleSheet(QStringLiteral(
            "QPushButton{background:%1; color:#fff; border:none;"
            " border-radius:4px; padding:6px 8px; font-weight:bold;"
            " text-align:left;}"
            "QPushButton:hover{background:%2;}").arg(tint, QStringLiteral("#ffffff")));
        grid->addWidget(b, i / cols, i % cols);
    }
    return box;
}

} // namespace

AnimPresetDock::AnimPresetDock(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    m_helpLabel = new QLabel(tr("Presets de Animação — selecione clipe(s) na "
                                "timeline e clique num preset. Os keyframes "
                                "entram dentro da duração do clipe."), this);
    m_helpLabel->setWordWrap(true);
    lay->addWidget(m_helpLabel);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    auto* v = new QVBoxLayout(inner);
    v->setContentsMargins(0, 0, 6, 0);
    v->setSpacing(8);
    v->addWidget(makeGroup(tr("Entradas"), kEntradas,
                           int(sizeof(kEntradas) / sizeof(kEntradas[0])),
                           this, QStringLiteral("#2E7D32")));
    v->addWidget(makeGroup(tr("Saídas"), kSaidas,
                           int(sizeof(kSaidas) / sizeof(kSaidas[0])),
                           this, QStringLiteral("#C62828")));
    v->addWidget(makeGroup(tr("Idle (laço)"), kIdle,
                           int(sizeof(kIdle) / sizeof(kIdle[0])),
                           this, QStringLiteral("#1565C0")));
    v->addWidget(makeGroup(tr("Efeitos"), kEfeitos,
                           int(sizeof(kEfeitos) / sizeof(kEfeitos[0])),
                           this, QStringLiteral("#6A1B9A")));
    v->addStretch(1);
    scroll->setWidget(inner);
    lay->addWidget(scroll, 1);

    setMinimumSize(240, 320);
}

void AnimPresetDock::setTimeline(TimelineWidget* tl) {
    m_timeline = tl;
}

void AnimPresetDock::applyPreset(const QString& name) {
    if (!m_timeline) return;
    Project* p = m_timeline->project();
    if (!p) return;

    const QStringList ids = m_timeline->selectedIds();
    if (ids.isEmpty()) {
        m_helpLabel->setText(tr("Selecione pelo menos um clipe na timeline "
                                "antes de aplicar um preset."));
        return;
    }

    PresetFn fn = nullptr;
    auto findIn = [&](const Preset* list, int n) {
        for (int i = 0; i < n; ++i)
            if (name == QLatin1String(list[i].name)) { fn = list[i].fn; return true; }
        return false;
    };
    if (!findIn(kEntradas, int(sizeof(kEntradas) / sizeof(kEntradas[0])))
        && !findIn(kSaidas, int(sizeof(kSaidas) / sizeof(kSaidas[0])))
        && !findIn(kIdle, int(sizeof(kIdle) / sizeof(kIdle[0])))
        && !findIn(kEfeitos, int(sizeof(kEfeitos) / sizeof(kEfeitos[0]))))
        return;

    const double W = p->width > 0 ? p->width : 1920.0;
    const double H = p->height > 0 ? p->height : 1080.0;

    emit m_timeline->editStart();
    int applied = 0;
    for (Track& tr : p->videoTracks) {
        for (Clip& c : tr.clips) {
            if (!ids.contains(c.id)) continue;
            // O preset substitui as keyframes das props que ele toca.
            fn(c, W, H);
            ++applied;
        }
    }
    m_timeline->invalidateScene();
    m_timeline->update();
    emit m_timeline->modified();

    m_helpLabel->setText(tr("Preset \"%1\" aplicado em %2 clipe(s).")
                             .arg(name).arg(applied));
    emit presetsApplied(applied);
}
