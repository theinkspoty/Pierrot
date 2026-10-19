// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/motion/AnimPresetDock.h"
#include "ui/timeline/TimelineWidget.h"
#include "colombina/models/Project.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QLinearGradient>
#include <QColor>
#include <QPolygonF>
#include <QTimer>
#include <QEvent>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEnterEvent>
#endif
#include <algorithm>

namespace {

// Helper: grava keyframes ABSOLUTOS (segundos) numa prop, substituindo os
// existentes; base = valor do primeiro key efetivo.
struct K { double t; double v; int i; };

void setKf(QVector<Keyframe>& kf, double& base, const QVector<K>& keys) {
    kf.clear();
    for (const K& k : keys) {
        // Mesmo instante: substitui a key INTEIRA. O upsertKeyframe global só
        // troca o valor e preserva a interpolação do primeiro — os presets de
        // laço semeiam t=0 com Linear e o key real (Sm/Bo/Bk) do1º ciclo era
        // descartado (Wiggle começava retinho, Flicker em vez de corte seco
        // fazia fade, Respiração perdia o quique inicial).
        for (int i = kf.size() - 1; i >= 0; --i)
            if (qAbs(kf[i].time - k.t) < 1e-9) kf.removeAt(i);
        upsertKeyframe(kf, k.t, k.v, k.i);
    }
    if (!kf.isEmpty()) base = kf.first().value;
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

// Queda com quique real (bounceOut assenta sozinho no "chão") e squash
// desenhado no instante do primeiro impacto — antecipação de peso.
void pBounceCima(Clip& c, double, double H) {
    const double d = qBound(0.7, c.dur, 2.5);
    const double tQ = d * 0.55;  // fim da sequência de quiques
    const double ti = tQ * 0.36; // 1º impacto (bounceOut chega a 1 em 1/2.75)
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.1, d * 0.08), 1, KfLinear}});
    setKf(c.kfTy, c.ty, {{0, -0.45 * H, Bo}, {tQ, 0, KfLinear}});
    setKf(c.kfScaleX, c.scaleX, {{0, 1, KfLinear}, {ti, 1.16, Bk},
                                 {ti + d * 0.12, 0.96, Sm}, {ti + d * 0.30, 1, KfLinear}});
    setKf(c.kfScaleY, c.scaleY, {{0, 1, KfLinear}, {ti, 0.84, Bk},
                                 {ti + d * 0.12, 1.05, Sm}, {ti + d * 0.30, 1, KfLinear}});
}

// Entra deslizando de baixo — espelho vertical dos slides laterais.
void pSlideBaixo(Clip& c, double, double H) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.12, d * 0.1), 1, KfLinear}});
    setKf(c.kfTy, c.ty, {{0, 0.42 * H, Bk}, {d * 0.32, 0, KfLinear}});
}

// Entra girando: quase de lado (−90°) e miúdo, assenta com back-out.
void pGiraEntra(Clip& c, double, double) {
    const double d = qBound(0.7, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.14, d * 0.12), 1, KfLinear}});
    setKf(c.kfRotation, c.rotation, {{0, -90, Bk}, {d * 0.45, 0, KfLinear}});
    setKf(c.kfScale, c.scale, {{0, 0.35, El}, {d * 0.5, 1, KfLinear}});
}

// Cortina: abre na vertical como persiana, com leve estica na horizontal.
void pCortina(Clip& c, double, double) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {qMin(0.12, d * 0.1), 1, KfLinear}});
    setKf(c.kfScaleY, c.scaleY, {{0, 0.06, Bk}, {d * 0.4, 1, KfLinear}});
    setKf(c.kfScaleX, c.scaleX, {{0, 1.08, KfLinear}, {d * 0.4, 1, KfLinear}});
}

// Fade cinematográfico: dissolve lento (meio da janela) com recuo sutil de
// escala — para B-roll, entrevistas e transições de cena.
void pFadeCine(Clip& c, double, double) {
    const double d = qBound(0.8, c.dur, 3.0);
    setKf(c.kfOpacity, c.opacity, {{0, 0, Sm}, {d * 0.45, 1, KfLinear}});
    setKf(c.kfScale, c.scale, {{0, 1.06, Sm}, {d * 0.55, 1, KfLinear}});
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

void pSairEsq(Clip& c, double W, double) {
    const double t0 = qMax(0.0, c.dur - qBound(0.5, c.dur * 0.25, 1.0));
    setKf(c.kfTx, c.tx, {{0, 0, KfLinear}, {t0, 0, Bk}, {c.dur, -0.45 * W, KfLinear}});
    setKf(c.kfOpacity, c.opacity, {{0, 1, KfLinear}, {t0, 1, Sm}, {c.dur, 0, KfLinear}});
}

void pSairBaixo(Clip& c, double, double H) {
    const double t0 = qMax(0.0, c.dur - qBound(0.5, c.dur * 0.25, 1.0));
    setKf(c.kfTy, c.ty, {{0, 0, KfLinear}, {t0, 0, Bk}, {c.dur, 0.40 * H, KfLinear}});
    setKf(c.kfOpacity, c.opacity, {{0, 1, KfLinear}, {t0, 1, Sm}, {c.dur, 0, KfLinear}});
}

// Recolhe no lugar (back-out = não passa do alvo) e ainda gira no caminho.
void pSairEncolhe(Clip& c, double, double) {
    const double t0 = qMax(0.0, c.dur - qBound(0.5, c.dur * 0.3, 1.1));
    setKf(c.kfScale, c.scale, {{0, 1, KfLinear}, {t0, 1, Bk}, {c.dur, 0.14, KfLinear}});
    setKf(c.kfRotation, c.rotation, {{0, 0, KfLinear}, {t0, 0, Sm}, {c.dur, 20, KfLinear}});
    setKf(c.kfOpacity, c.opacity, {{0, 1, KfLinear}, {t0, 1, KfLinear},
                                   {t0 + (c.dur - t0) * 0.7, 0.5, Sm},
                                   {c.dur, 0, KfLinear}});
}

// Sai girando para fora de quadro (meia volta suave + encolhimento).
void pSairGira(Clip& c, double, double) {
    const double t0 = qMax(0.0, c.dur - qBound(0.5, c.dur * 0.3, 1.1));
    setKf(c.kfRotation, c.rotation, {{0, 0, KfLinear}, {t0, 0, Sm},
                                     {c.dur, -150, KfLinear}});
    setKf(c.kfScale, c.scale, {{0, 1, KfLinear}, {t0, 1, Sm},
                               {c.dur, 0.35, KfLinear}});
    setKf(c.kfOpacity, c.opacity, {{0, 1, KfLinear}, {t0, 1, KfLinear},
                                   {c.dur, 0, KfLinear}});
}

// Dissolve lento na metade final — transição de cena dentro do próprio clipe.
void pSairFade(Clip& c, double, double) {
    const double t0 = qMax(0.0, c.dur - qBound(0.6, c.dur * 0.45, 2.0));
    setKf(c.kfOpacity, c.opacity, {{0, 1, KfLinear}, {t0, 1, Sm}, {c.dur, 0, KfLinear}});
    setKf(c.kfScale, c.scale, {{0, 1, KfLinear}, {t0, 1, Sm}, {c.dur, 1.05, KfLinear}});
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

// Pêndulo: balanço de rotação passando por0 (quadrantes em Suave ≈ seno).
void pPendulo(Clip& c, double, double) {
    const double passo = 1.6;
    QVector<K> kf{{0, 0, KfLinear}};
    bool up = true;
    for (double t = 0; t + passo < c.dur; t += passo) {
        const double dir = up ? 1.0 : -1.0;
        kf << K{t + passo * 0.25, dir * 7, Sm}
           << K{t + passo * 0.75, -dir * 7, Sm}
           << K{t + passo, 0, KfLinear};
        up = !up;
    }
    setKf(c.kfRotation, c.rotation, kf);
}

// Órbita: círculo lento — tx/ty em quadratura (90° fora de fase).
void pOrbita(Clip& c, double, double H) {
    const double passo = 1.6;
    const double a = 0.03 * H;
    QVector<K> x{{0, a, KfLinear}}, y{{0, 0, KfLinear}};
    for (double t = 0; t + passo < c.dur; t += passo) {
        const double q = passo * 0.25;
        x << K{t + q, 0, Sm} << K{t + 2 * q, -a, Sm}
          << K{t + 3 * q, 0, Sm} << K{t + passo, a, KfLinear};
        y << K{t + q, a, Sm} << K{t + 2 * q, 0, Sm}
          << K{t + 3 * q, -a, Sm} << K{t + passo, 0, KfLinear};
    }
    setKf(c.kfTx, c.tx, x);
    setKf(c.kfTy, c.ty, y);
}

// Pulso (latida): dois batidos curtos de escala, tipo "coração".
void pPulso(Clip& c, double, double) {
    const double passo = 0.8;
    QVector<K> kf{{0, 1, KfLinear}};
    for (double t = 0; t + passo < c.dur; t += passo) {
        kf << K{t + 0.10, 1.07, Bk} << K{t + 0.28, 1.0, KfLinear}
           << K{t + 0.40, 1.045, Bk} << K{t + passo, 1, KfLinear};
    }
    setKf(c.kfScale, c.scale, kf);
}

// Deriva lenta: pan de um lado ao outro (Ken Burns) preenchendo o clipe.
void pDeriva(Clip& c, double W, double) {
    const double passo = qBound(0.8, c.dur * 0.5, 2.5);
    QVector<K> kf{{0, -0.02 * W, KfLinear}};
    bool pos = true;
    for (double t = 0; t + passo < c.dur; t += passo) {
        kf << K{t + passo, pos ? 0.02 * W : -0.02 * W, Sm};
        pos = !pos;
    }
    // Fecha voltando ao outro lado (e cobre clipe curto, onde o laço não roda).
    const double volta = -kf.last().v;
    if (c.dur - kf.last().t > 0.15) kf << K{c.dur, volta, Sm};
    setKf(c.kfTx, c.tx, kf);
}

// Tremida leve: vibração rápida de posição (câmera na mão / corte energético).
void pTremida(Clip& c, double W, double) {
    const double passo = 0.16;
    const double amp = 0.006 * W;
    QVector<K> x{{0, 0, KfLinear}}, y{{0, 0, KfLinear}};
    bool up = true;
    for (double t = 0; t + passo < c.dur; t += passo) {
        x << K{t + passo * 0.5, up ? amp : -amp, KfSmooth};
        y << K{t + passo * 0.5, up ? -amp * 0.8 : amp * 0.8, KfSmooth};
        up = !up;
    }
    x << K{c.dur, 0, KfLinear};
    y << K{c.dur, 0, KfLinear};
    setKf(c.kfTx, c.tx, x);
    setKf(c.kfTy, c.ty, y);
}

// ─── EFEITOS ──────────────────────────────────────────────────────────────
void pGiro(Clip& c, double, double) {
    const double d = qBound(0.8, c.dur, 3.0);
    // A interpolação mora no1º key do segmento (Sm = S no par isolado); a que
    // estava no último era inerte e o giro saía reto (linear cru).
    setKf(c.kfRotation, c.rotation, {{0, 0, Sm}, {d * 0.75, 360, KfLinear}});
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

// Meia volta com assentamento — reveal de verso (títulos, cartões).
void pGiro180(Clip& c, double, double) {
    const double d = qBound(0.7, c.dur, 2.5);
    setKf(c.kfRotation, c.rotation, {{0, 0, Sm}, {d * 0.6, 180, KfLinear}});
}

// Impacto: soco de escala uma única vez (hit/trailer) + tranco de rotação.
void pImpacto(Clip& c, double, double) {
    const double d = qBound(0.5, c.dur, 2.0);
    setKf(c.kfScale, c.scale, {{0, 1, Bk}, {d * 0.12, 1.16, Sm},
                               {d * 0.26, 0.97, Sm}, {d * 0.42, 1, KfLinear}});
    setKf(c.kfRotation, c.rotation, {{0, 0, Bk}, {d * 0.14, -3, Sm},
                                     {d * 0.42, 0, KfLinear}});
}

// Flip: vira como cartinha — passa por scaleX 0 (canto de vista) e volta.
void pFlip(Clip& c, double, double) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, KfLinear},
                                   {qMin(0.06, d * 0.05), 1, KfLinear}});
    setKf(c.kfScaleX, c.scaleX, {{0, 1, Sm}, {d * 0.45, -1, Bk},
                                 {d * 0.9, 1, KfLinear}});
    setKf(c.kfRotation, c.rotation, {{0, -6, Sm}, {d * 0.45, 6, Sm},
                                     {d * 0.9, 0, KfLinear}});
}

// Zoom por cortes: saltos secos de escala estilo vlog/TikTok (Degrau).
void pJumpZoom(Clip& c, double, double) {
    const double passo = 0.7;
    QVector<K> kf{{0, 1.0, KfStep}};
    int n = 0;
    for (double t = 0; t + passo < c.dur; t += passo)
        kf << K{t + passo, (++n % 2) ? 1.09 : 1.0, KfStep};
    setKf(c.kfScale, c.scale, kf);
}

// Estroboscópio: corte seco de opacidade a cada ~0,16 s (acende/apaga).
void pStrobe(Clip& c, double, double) {
    const double passo = 0.16;
    QVector<K> kf{{0, 1, KfStep}};
    bool on = true;
    for (double t = 0; t + passo < c.dur; t += passo) {
        kf << K{t, on ? 1.0 : 0.0, KfStep};
        on = !on;
    }
    kf << K{qMax(0.1, c.dur - 0.08), 1, KfLinear};
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

// ─── ELÁSTICOS (novos) ────────────────────────────────────────────────────
// Trio profissional com overshoot/oscilação amortecida forte (KfElastic),
// squash & stretch antifase e follow-through de rotação.

// Entrada: cai de cima e "esmaga" no impacto com deformação elástica.
void pSlam(Clip& c, double, double H) {
    const double d = qBound(0.8, c.dur, 3.0);
    const double ti = d * 0.30; // instante do impacto
    setKf(c.kfOpacity, c.opacity, {{0, 0, KfLinear}, {qMin(0.07, d * 0.05), 1, KfLinear}});
    // queda com overshoot elástico (passa do chão e quica)
    setKf(c.kfTy, c.ty, {{0, -0.62 * H, El}, {ti, 0.03 * H, Bk},
                         {ti + d * 0.12, -0.012 * H, Sm}, {ti + d * 0.24, 0, KfLinear}});
    // squash & stretch: achata no impacto, estica na recuperação (antifase)
    setKf(c.kfScaleY, c.scaleY, {{0, 1.30, El}, {ti, 0.70, Bk},
                                 {ti + d * 0.14, 1.14, El}, {ti + d * 0.30, 1, KfLinear}});
    setKf(c.kfScaleX, c.scaleX, {{0, 0.82, El}, {ti, 1.34, Bk},
                                 {ti + d * 0.14, 0.92, El}, {ti + d * 0.30, 1, KfLinear}});
    // giro amortecido acompanhando o impacto
    setKf(c.kfRotation, c.rotation, {{0, -8, El}, {ti, 4, El},
                                     {ti + d * 0.26, 0, KfLinear}});
}

// Saída: comprime (anticipation), lança com elastic gigante e desaparece.
void pEsticaSome(Clip& c, double W, double) {
    const double t0 = qMax(0.0, c.dur - qBound(0.7, c.dur * 0.35, 1.4));
    // recuo sutil antes de disparar; o segmento de saída usa back-out
    // (interpolação do key em t0) e estica até além do alvo
    setKf(c.kfTx, c.tx, {{0, 0, KfLinear}, {t0, -0.05 * W, Bk},
                         {c.dur, 0.55 * W, KfLinear}});
    // squash antifase com overshoot elástico na fuga
    setKf(c.kfScaleX, c.scaleX, {{0, 1, KfLinear}, {t0, 0.85, KfLinear},
                                 {c.dur, 1.60, El}});
    setKf(c.kfScaleY, c.scaleY, {{0, 1, KfLinear}, {t0, 1.18, KfLinear},
                                 {c.dur, 0.55, El}});
    setKf(c.kfRotation, c.rotation, {{0, 0, KfLinear}, {t0, -5, KfLinear},
                                     {c.dur, 20, El}});
    setKf(c.kfOpacity, c.opacity, {{0, 1, KfLinear}, {t0, 1, KfLinear},
                                   {t0 + (c.dur - t0) * 0.6, 0.85, Sm},
                                   {c.dur, 0, KfLinear}});
}

// Idle: loop de mola — bob vertical + squash/twist antifase, tudo elástico.
void pMola(Clip& c, double, double H) {
    const double passo = 1.1;
    QVector<K> ty{{0, 0, El}}, sx{{0, 1, El}}, sy{{0, 1, El}},
               rot{{0, 0, El}}, sc{{0, 1, El}};
    bool up = true;
    for (double t = 0; t + passo < c.dur; t += passo) {
        const double dir = up ? 1.0 : -1.0;
        const double m = t + passo * 0.5;
        ty   << K{m, dir * 0.055 * H, El} << K{t + passo, 0, El};
        sx   << K{m, 1.0 + dir * 0.11, El} << K{t + passo, 1, El};
        sy   << K{m, 1.0 - dir * 0.09, El} << K{t + passo, 1, El};
        rot  << K{m, dir * 5, El}           << K{t + passo, 0, El};
        sc   << K{m, 1.0 + dir * 0.04, El} << K{t + passo, 1, El};
        up = !up;
    }
    setKf(c.kfTy, c.ty, ty);
    setKf(c.kfScaleX, c.scaleX, sx);
    setKf(c.kfScaleY, c.scaleY, sy);
    setKf(c.kfRotation, c.rotation, rot);
    setKf(c.kfScale, c.scale, sc);
}

// Recoil: recuo tipo arma/câmera — soco seco para trás, volta com contragolpe
// elástico e micro-ring na posição.
void pRecoil(Clip& c, double W, double) {
    const double d = qBound(0.5, c.dur, 2.0);
    const double ti = d * 0.14; // pico do recuo
    setKf(c.kfTx, c.tx, {{0, 0, KfLinear}, {ti, -0.05 * W, Bk},
                         {ti + d * 0.20, 0.012 * W, El},
                         {ti + d * 0.45, 0, KfLinear}});
    setKf(c.kfRotation, c.rotation, {{0, 0, KfLinear}, {ti, -7, Bk},
                                     {ti + d * 0.26, 1.5, El},
                                     {ti + d * 0.50, 0, KfLinear}});
    setKf(c.kfScale, c.scale, {{0, 1, KfLinear}, {ti, 1.10, Bk},
                               {ti + d * 0.24, 0.98, El},
                               {ti + d * 0.50, 1, KfLinear}});
    // squash antifase acompanhando o tranco
    setKf(c.kfScaleX, c.scaleX, {{0, 1, KfLinear}, {ti, 1.14, Bk},
                                 {ti + d * 0.24, 0.95, Sm},
                                 {ti + d * 0.50, 1, KfLinear}});
    setKf(c.kfScaleY, c.scaleY, {{0, 1, KfLinear}, {ti, 0.86, Bk},
                                 {ti + d * 0.24, 1.06, Sm},
                                 {ti + d * 0.50, 1, KfLinear}});
}

// Salto: antecipação (esmaga no chão) + voo com elástico no topo +
// aterrissagem quicando, com squash na recepção.
void pSalto(Clip& c, double, double H) {
    const double d = qBound(0.8, c.dur, 3.0);
    const double t1 = d * 0.18; // fim da antecipação
    const double t2 = d * 0.55; // topo do salto
    const double t3 = d * 0.78; // aterrissagem
    setKf(c.kfOpacity, c.opacity, {{0, 0, KfLinear},
                                   {qMin(0.08, d * 0.06), 1, KfLinear}});
    setKf(c.kfTy, c.ty, {{0, 0, KfLinear}, {t1, 0.06 * H, Sm},
                         {t2, -0.30 * H, Bo}, {t3, 0, KfLinear}});
    setKf(c.kfScaleY, c.scaleY, {{0, 1, KfLinear}, {t1, 0.80, Bk},
                                 {t1 + d * 0.10, 1.12, KfLinear},
                                 {t2, 1.0, KfLinear}, {t3, 0.86, Bo},
                                 {d, 1, KfLinear}});
    setKf(c.kfScaleX, c.scaleX, {{0, 1, KfLinear}, {t1, 1.18, Bk},
                                 {t1 + d * 0.10, 0.92, KfLinear},
                                 {t2, 1.0, KfLinear}, {t3, 1.12, Bo},
                                 {d, 1, KfLinear}});
}

// Estalo (snap): entra miúdo e elástico, com giro e squash que assentam.
void pSnap(Clip& c, double, double) {
    const double d = qBound(0.6, c.dur, 2.5);
    setKf(c.kfOpacity, c.opacity, {{0, 0, KfLinear},
                                   {qMin(0.05, d * 0.04), 1, KfLinear}});
    setKf(c.kfScale, c.scale, {{0, 0.55, El}, {d * 0.30, 1, KfLinear}});
    setKf(c.kfRotation, c.rotation, {{0, 14, El}, {d * 0.32, 0, KfLinear}});
    setKf(c.kfScaleX, c.scaleX, {{0, 1.28, El}, {d * 0.30, 1, KfLinear}});
    setKf(c.kfScaleY, c.scaleY, {{0, 0.78, El}, {d * 0.30, 1, KfLinear}});
}

// ─── CÂMERA (Ken Burns — movimento lento na duração inteira) ─────────────
// Par de keys com Suave: S macia ponta a ponta (sem tranco nas bordas).

void pPushIn(Clip& c, double, double) {
    const double d = qMax(0.3, c.dur);
    setKf(c.kfScale, c.scale, {{0, 1.0, Sm}, {d, 1.12, KfLinear}});
}

void pPullOut(Clip& c, double, double) {
    const double d = qMax(0.3, c.dur);
    setKf(c.kfScale, c.scale, {{0, 1.14, Sm}, {d, 1.0, KfLinear}});
}

void pPanDir(Clip& c, double W, double) {
    const double d = qMax(0.3, c.dur);
    setKf(c.kfTx, c.tx, {{0, -0.055 * W, Sm}, {d, 0.055 * W, KfLinear}});
}

void pPanEsq(Clip& c, double W, double) {
    const double d = qMax(0.3, c.dur);
    setKf(c.kfTx, c.tx, {{0, 0.055 * W, Sm}, {d, -0.055 * W, KfLinear}});
}

void pSobe(Clip& c, double, double H) { // subida tipo drone
    const double d = qMax(0.3, c.dur);
    setKf(c.kfTy, c.ty, {{0, 0.07 * H, Sm}, {d, -0.07 * H, KfLinear}});
}

void pDutch(Clip& c, double, double) { // giro holandês sutil + leve zoom
    const double d = qMax(0.3, c.dur);
    setKf(c.kfRotation, c.rotation, {{0, -2.5, Sm}, {d, 2.5, KfLinear}});
    setKf(c.kfScale, c.scale, {{0, 1.0, Sm}, {d, 1.07, KfLinear}});
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
    {"Slide Baixo", pSlideBaixo},
    {"Bounce de Cima", pBounceCima},
    {"Girar Entrando", pGiraEntra},
    {"Cortina Vertical", pCortina},
    {"Zoom Back", pZoomBack},
    {"Fade Up", pFadeUp},
    {"Fade Cinemático", pFadeCine},
    {"Semeia (AE)", pSemear},
};

const Preset kSaidas[] = {
    {"Sair Pop", pSairPop},
    {"Sair p/ Direita", pSairDir},
    {"Sair p/ Esquerda", pSairEsq},
    {"Sair Subindo", pSairSobe},
    {"Sair Descendo", pSairBaixo},
    {"Sair Encolhendo", pSairEncolhe},
    {"Sair Girando", pSairGira},
    {"Saída em Fade", pSairFade},
};

const Preset kCamera[] = {
    {"Push In", pPushIn},
    {"Pull Out", pPullOut},
    {"Pan Direita", pPanDir},
    {"Pan Esquerda", pPanEsq},
    {"Subida Suave", pSobe},
    {"Dr. Holandês", pDutch},
};

const Preset kIdle[] = {
    {"Respiração", pRespira},
    {"Wiggle", pWiggle},
    {"Flutuação", pFlutua},
    {"Pêndulo", pPendulo},
    {"Órbita", pOrbita},
    {"Pulso (latida)", pPulso},
    {"Deriva Lenta", pDeriva},
    {"Tremida Leve", pTremida},
};

const Preset kEfeitos[] = {
    {"Giro 360", pGiro},
    {"Giro 180", pGiro180},
    {"Impacto", pImpacto},
    {"Flip Horizontal", pFlip},
    {"Squash & Stretch", pSquash},
    {"Zoom por Cortes", pJumpZoom},
    {"Flicker", pFlicker},
    {"Stroboscópio", pStrobe},
};

const Preset kElasticos[] = {
    {"Slam Elástico", pSlam},
    {"Salto Elástico", pSalto},
    {"Recoil", pRecoil},
    {"Estalo", pSnap},
    {"Estica e Some", pEsticaSome},
    {"Mola (laço)", pMola},
};

// ── Transições (grupo separado: seta transitionType, não keyframes) ─────
struct TransDef { const char* name; const char* type; };
const TransDef kTransicoes[] = {
    {"Dissolve",     "dissolve"},
    {"Wipe Esquerda","wipeleft"},
    {"Wipe Direita", "wiperight"},
    {"Wipe Cima",    "wipeup"},
    {"Wipe Baixo",   "wipedown"},
    {"Canto TL",     "wipetl"},
    {"Canto TR",     "wipetr"},
    {"Canto BR",     "wipebr"},
    {"Canto BL",     "wipebl"},
};

// ── Card de transição: miniatura animada de dois quadros ────────────────
// Pinta dois retângulos coloridos (A = fundo, B = topo) e anima a
// transição entre eles — dissolve (alpha) ou wipe (deslize/recorte).
class TransitionCard : public QWidget {
public:
    TransitionCard(const QString& type, const QString& name,
                   AnimPresetDock* dock, QWidget* parent = nullptr)
        : QWidget(parent), m_type(type), m_name(name), m_dock(dock) {
        setObjectName(QStringLiteral("transBtn_") + name);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setToolTip(name);
        setMinimumSize(sizeHint());
        if (QTimer* t = dock->thumbTimer())
            connect(t, &QTimer::timeout, this, [this]() { tick(); });
    }

    QString transitionType() const { return m_type; }
    QString displayName() const { return m_name; }

    QSize sizeHint() const override { return {114, 88}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(m_hover ? QColor("#5a6478") : QColor("#343b48"), 1));
        p.setBrush(m_hover ? QColor("#282e39") : QColor("#1e232c"));
        p.drawRoundedRect(r, 5, 5);

        const QRectF th = r.adjusted(4, 4, -4, -19);
        p.save();
        p.setClipRect(th);
        p.fillRect(th, QColor("#0e1116"));

        const double k = qMin(th.width() / 1920.0, th.height() / 1080.0);
        const QPointF ctr = th.center();
        QRectF frame(-960 * k, -540 * k, 1920 * k, 1080 * k);
        frame.moveCenter(ctr);
        p.setPen(QPen(QColor(255, 255, 255, 36), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(frame);

        // Quadro A (fundo): azul escuro. Quadro B (topo): laranja.
        const QColor colA("#1a3a5c");
        const QColor colB("#c86a20");
        const double prog = m_t; // 0..1

        // Fundo A sempre visível
        p.fillRect(frame, colA);

        if (m_type == QLatin1String("dissolve")) {
            // B aparece com alpha crescente
            p.setOpacity(prog);
            p.fillRect(frame, colB);
            p.setOpacity(1.0);
        } else {
            // Wipes: B desliza/recorta sobre A
            QRectF visB = frame;
            if (m_type == QLatin1String("wipeleft"))
                visB.setLeft(frame.left() + frame.width() * prog);
            else if (m_type == QLatin1String("wiperight"))
                visB.setRight(frame.right() - frame.width() * prog);
            else if (m_type == QLatin1String("wipeup"))
                visB.setTop(frame.top() + frame.height() * prog);
            else if (m_type == QLatin1String("wipedown"))
                visB.setBottom(frame.bottom() - frame.height() * prog);
            else if (m_type == QLatin1String("wipetl")) {
                visB.setLeft(frame.left() + frame.width() * prog);
                visB.setTop(frame.top() + frame.height() * prog);
            } else if (m_type == QLatin1String("wipetr")) {
                visB.setRight(frame.right() - frame.width() * prog);
                visB.setTop(frame.top() + frame.height() * prog);
            } else if (m_type == QLatin1String("wipebr")) {
                visB.setRight(frame.right() - frame.width() * prog);
                visB.setBottom(frame.bottom() - frame.height() * prog);
            } else if (m_type == QLatin1String("wipebl")) {
                visB.setLeft(frame.left() + frame.width() * prog);
                visB.setBottom(frame.bottom() - frame.height() * prog);
            }
            if (visB.isValid() && !visB.isEmpty())
                p.fillRect(visB, colB);
        }

        // Ícone play central (só quando em repouso)
        if (prog < 0.01) {
            const double ph = frame.height() * 0.18;
            QPolygonF tri;
            tri << QPointF(ctr.x() - ph * 0.4, ctr.y() - ph * 0.6)
                << QPointF(ctr.x() + ph * 0.7, ctr.y())
                << QPointF(ctr.x() - ph * 0.4, ctr.y() + ph * 0.6);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 120));
            p.drawPolygon(tri);
        }
        p.restore();

        // rótulo
        p.setPen(QPen(m_hover ? QColor("#e8ecf4") : QColor("#9aa3b5"), 1));
        QFont f = font();
        f.setPointSizeF(7.5);
        p.setFont(f);
        const QRectF labelR = r.adjusted(4, th.height() + 5, -4, -3);
        p.drawText(labelR, Qt::AlignCenter | Qt::TextSingleLine,
                   p.fontMetrics().elidedText(m_name, Qt::ElideRight,
                                              int(labelR.width())));
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent*) override {
        m_hover = true;
        m_t = 0.0;
        m_running = true;
        update();
    }
#else
    void enterEvent(QEvent*) override {
        m_hover = true;
        m_t = 0.0;
        m_running = true;
        update();
    }
#endif
    void leaveEvent(QEvent*) override {
        m_hover = false;
        m_running = false;
        m_t = 0.0;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && m_dock) {
            e->accept();
            m_dock->applyTransition(m_type);
        }
    }

private:
    void tick() {
        if (!m_running || !isVisible()) return;
        m_t += 0.035; // ~35 ms por passo a 33 ms de timer → ~1.1 s por loop
        if (m_t > 1.15) m_t = 0.0; // pausa breve no fim antes de reiniciar
        update();
    }

    QString m_type;
    QString m_name;
    AnimPresetDock* m_dock = nullptr;
    double m_t = 0.0;
    bool m_hover = false;
    bool m_running = false;
};

QGroupBox* makeTransitionGroup(AnimPresetDock* dock) {
    auto* box = new QGroupBox(QObject::tr("Transições"), dock);
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(8, 14, 8, 8);
    grid->setSpacing(6);
    const int n = int(sizeof(kTransicoes) / sizeof(kTransicoes[0]));
    const int cols = 3;
    for (int i = 0; i < n; ++i) {
        auto* card = new TransitionCard(
            QString::fromUtf8(kTransicoes[i].type),
            QString::fromUtf8(kTransicoes[i].name), dock, box);
        grid->addWidget(card, i / cols, i % cols);
    }
    return box;
}

// ── Miniatura animada (estilo Vegas) ──────────────────────────────────────
// Simula o preset num clipe dummy (1920×1080, 2 s). Em repouso mostra um
// quadro estático representativo; ao passar o mouse por cima, o loop (~30 fps)
// roda do início — sai de novo ao retirar o mouse.
class PresetCard : public QWidget {
public:
    PresetCard(PresetFn fn, const QString& name, const QString& tint,
               AnimPresetDock* dock, QWidget* parent = nullptr)
        : QWidget(parent), m_name(name), m_tint(tint), m_dock(dock) {
        setObjectName(QStringLiteral("presetBtn_") + name);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setToolTip(name);
        // nunca encolhe: com 3 colunas o scroll aparece se faltar espaço
        setMinimumSize(sizeHint());
        m_clip.dur = 2.0;
        fn(m_clip, 1920.0, 1080.0);
        m_t = restT(); // quadro estático inicial (visível em todos os presets)
        if (QTimer* t = dock->thumbTimer()) {
            connect(t, &QTimer::timeout, this, [this]() { tick(); });
        }
    }

    QSize sizeHint() const override { return {114, 88}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(m_hover ? QColor("#5a6478") : QColor("#343b48"), 1));
        p.setBrush(m_hover ? QColor("#282e39") : QColor("#1e232c"));
        p.drawRoundedRect(r, 5, 5);

        // ── área do thumb ──
        const QRectF th = r.adjusted(4, 4, -4, -19);
        p.save();
        p.setClipRect(th);
        p.fillRect(th, QColor("#0e1116"));

        const double k = qMin(th.width() / 1920.0, th.height() / 1080.0);
        const QPointF ctr = th.center();

        // moldura do "projeto" (referência de quadro)
        QRectF frame(-960 * k, -540 * k, 1920 * k, 1080 * k);
        frame.moveCenter(ctr);
        p.setPen(QPen(QColor(255, 255, 255, 36), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(frame);

        // fantasma em repouso (evidencia deslocamento/escala/rotação)
        const QRectF corpo(-frame.width() * 0.34, -frame.height() * 0.34,
                           frame.width() * 0.68, frame.height() * 0.68);
        p.setPen(QPen(QColor(255, 255, 255, 26), 1));
        p.drawRoundedRect(QRectF(corpo).translated(ctr), 3, 3);

        // avalia o preset simulado no instante do loop
        const double rel = m_t;
        const double tx = kfValue(m_clip.kfTx, m_clip.tx, rel);
        const double ty = kfValue(m_clip.kfTy, m_clip.ty, rel);
        const double sc = kfValue(m_clip.kfScale, m_clip.scale, rel);
        const double sx = kfValue(m_clip.kfScaleX, m_clip.scaleX, rel);
        const double sy = kfValue(m_clip.kfScaleY, m_clip.scaleY, rel);
        const double rot = kfValue(m_clip.kfRotation, m_clip.rotation, rel);
        const double op =
            std::clamp(kfValue(m_clip.kfOpacity, m_clip.opacity, rel), 0.0, 1.0);

        p.setOpacity(op);
        p.translate(ctr.x() + tx * k, ctr.y() + ty * k);
        p.rotate(rot);
        p.scale(sc * sx, sc * sy);
        QLinearGradient g(corpo.topLeft(), corpo.bottomRight());
        g.setColorAt(0, m_tint.lighter(130));
        g.setColorAt(1, m_tint.darker(170));
        p.setBrush(g);
        p.setPen(QPen(QColor(255, 255, 255, 150), 1.2));
        p.drawRoundedRect(corpo, 4, 4);
        // ícone play — orienta rotação e direção da deformação
        const double ph = corpo.height() * 0.34;
        QPolygonF tri;
        tri << QPointF(-ph * 0.45, -ph * 0.6)
            << QPointF(ph * 0.7, 0)
            << QPointF(-ph * 0.45, ph * 0.6);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 210));
        p.drawPolygon(tri);
        p.restore();

        // ── nome ──
        p.setPen(m_hover ? QColor("#ffffff") : QColor("#c5cdd9"));
        QFont f = font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        const QRectF lr = r.adjusted(6, r.height() - 17, -6, -3);
        p.drawText(lr, Qt::AlignCenter,
                   QFontMetrics(f).elidedText(m_name, Qt::ElideRight,
                                              int(lr.width())));
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && m_dock) {
            e->accept();
            m_dock->applyPreset(m_name);
        }
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent*) override { m_hover = true; m_t = 0.0; update(); }
#else
    void enterEvent(QEvent*) override { m_hover = true; m_t = 0.0; update(); }
#endif
    void leaveEvent(QEvent*) override {
        m_hover = false;
        m_t = restT();
        update();
    }

private:
    // Quadro estático de repouso: meio da animação — visível tanto nos
    // presets de entrada (assentou) quanto de saída (antes de ir embora).
    double restT() const { return m_clip.dur * 0.5; }

    void tick() {
        if (!isVisible() || !m_hover) return; // só toca com o mouse em cima
        m_t += 1.0 / 30.0;
        // loop: animação (2 s) + pausa no estado final antes de repetir
        if (m_t > m_clip.dur + 0.9) m_t = 0.0;
        update();
    }

    QString m_name;
    QColor m_tint;
    AnimPresetDock* m_dock = nullptr;
    Clip m_clip;   // clipe dummy com os keyframes do preset aplicados
    double m_t = 0.0;
    bool m_hover = false;
};

QGroupBox* makeGroup(const QString& title, const Preset* list, int n,
                     AnimPresetDock* dock, const QString& tint) {
    auto* box = new QGroupBox(title, dock);
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(8, 14, 8, 8);
    grid->setSpacing(6);
    const int cols = 3; // fileiras de 3, sem espremer (minimum = sizeHint)
    for (int i = 0; i < n; ++i) {
        auto* card = new PresetCard(list[i].fn, QString::fromUtf8(list[i].name),
                                    tint, dock, box);
        grid->addWidget(card, i / cols, i % cols);
    }
    return box;
}

} // namespace

AnimPresetDock::AnimPresetDock(QWidget* parent) : QWidget(parent) {
    // Timer único da miniatura animada: cards se conectam ao sinal e só
    // redesenham quando visíveis; o dock para/retoma no hide/show.
    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    if (isVisible()) m_timer->start(); // senão o showEvent inicia

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(6);

    m_helpLabel = new QLabel(tr("Selecione clipe(s) na timeline e clique num "
                                "preset para aplicar. Passe o mouse sobre a "
                                "miniatura para ver a animação."),
                             this);
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
    v->addWidget(makeGroup(tr("Câmera (Ken Burns)"), kCamera,
                           int(sizeof(kCamera) / sizeof(kCamera[0])),
                           this, QStringLiteral("#00838F")));
    v->addWidget(makeGroup(tr("Idle (laço)"), kIdle,
                           int(sizeof(kIdle) / sizeof(kIdle[0])),
                           this, QStringLiteral("#1565C0")));
    v->addWidget(makeGroup(tr("Efeitos"), kEfeitos,
                           int(sizeof(kEfeitos) / sizeof(kEfeitos[0])),
                           this, QStringLiteral("#6A1B9A")));
    v->addWidget(makeGroup(tr("Elásticos"), kElasticos,
                           int(sizeof(kElasticos) / sizeof(kElasticos[0])),
                           this, QStringLiteral("#E65100")));
    v->addWidget(makeTransitionGroup(this));
    v->addStretch(1);
    scroll->setWidget(inner);
    lay->addWidget(scroll, 1);

    // Largura mínima para 3 colunas de 114 px sem espremer + barra de
    // rolagem vertical da área de scroll (≈15 px); se ficar menor, aparece
    // rolagem horizontal em vez de encolher os cards.
    setMinimumSize(405, 360);
}

AnimPresetDock::~AnimPresetDock() = default;

QTimer* AnimPresetDock::thumbTimer() const {
    return m_timer;
}

void AnimPresetDock::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (m_timer) m_timer->start();
}

void AnimPresetDock::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (m_timer) m_timer->stop();
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
        // fromUtf8 nos DOIS lados: QString == QLatin1String lê os bytes como
        // Latin-1, e os nomes vêm do UTF-8 da fonte — "Pop Elástico" nunca
        // casava e o clique era silenciosamente ignorado.
        const QString alvo = name;
        for (int i = 0; i < n; ++i)
            if (alvo == QString::fromUtf8(list[i].name)) {
                fn = list[i].fn;
                return true;
            }
        return false;
    };
    if (!findIn(kEntradas, int(sizeof(kEntradas) / sizeof(kEntradas[0])))
        && !findIn(kSaidas, int(sizeof(kSaidas) / sizeof(kSaidas[0])))
        && !findIn(kCamera, int(sizeof(kCamera) / sizeof(kCamera[0])))
        && !findIn(kIdle, int(sizeof(kIdle) / sizeof(kIdle[0])))
        && !findIn(kEfeitos, int(sizeof(kEfeitos) / sizeof(kEfeitos[0])))
        && !findIn(kElasticos, int(sizeof(kElasticos) / sizeof(kElasticos[0]))))
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

void AnimPresetDock::applyTransition(const QString& type) {
    if (!m_timeline) return;
    Project* p = m_timeline->project();
    if (!p) return;

    const QStringList ids = m_timeline->selectedIds();
    if (ids.isEmpty()) {
        m_helpLabel->setText(tr("Selecione pelo menos um clipe na timeline "
                                "antes de aplicar uma transição."));
        return;
    }

    emit m_timeline->editStart();
    int applied = 0;
    int noOverlap = 0;
    for (Track& tr : p->videoTracks) {
        for (Clip& c : tr.clips) {
            if (!ids.contains(c.id)) continue;
            c.transitionType = type;
            ++applied;
            // Verifica se há sobreposição com o próximo clipe da faixa.
            bool hasOverlap = false;
            for (const Clip& other : tr.clips) {
                if (other.id == c.id) continue;
                if (other.pos < c.pos + c.dur && other.pos > c.pos) {
                    hasOverlap = true;
                    break;
                }
            }
            if (!hasOverlap) ++noOverlap;
        }
    }
    m_timeline->invalidateScene();
    m_timeline->update();
    emit m_timeline->modified();

    if (noOverlap > 0) {
        m_helpLabel->setText(
            tr("Transição \"%1\" aplicada em %2 clipe(s). %3 sem sobreposição "
               "— arraste um clipe para sobrepor o anterior.")
                .arg(type).arg(applied).arg(noOverlap));
    } else {
        m_helpLabel->setText(tr("Transição \"%1\" aplicada em %2 clipe(s).")
                                 .arg(type).arg(applied));
    }
    emit presetsApplied(applied);
}
