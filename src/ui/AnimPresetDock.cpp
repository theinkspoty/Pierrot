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
// existentes; base = primeiro valor (mesmo critério do Lazzi).
struct K { double t; double v; int i; };

void setKf(QVector<Keyframe>& kf, double& base, const QVector<K>& keys) {
    kf.clear();
    for (const K& k : keys) upsertKeyframe(kf, k.t, k.v, k.i);
    if (!keys.isEmpty()) base = keys.first().v;
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

const Preset kElasticos[] = {
    {"Slam Elástico", pSlam},
    {"Estica e Some", pEsticaSome},
    {"Mola (laço)", pMola},
};

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
    v->addWidget(makeGroup(tr("Idle (laço)"), kIdle,
                           int(sizeof(kIdle) / sizeof(kIdle[0])),
                           this, QStringLiteral("#1565C0")));
    v->addWidget(makeGroup(tr("Efeitos"), kEfeitos,
                           int(sizeof(kEfeitos) / sizeof(kEfeitos[0])),
                           this, QStringLiteral("#6A1B9A")));
    v->addWidget(makeGroup(tr("Elásticos"), kElasticos,
                           int(sizeof(kElasticos) / sizeof(kElasticos[0])),
                           this, QStringLiteral("#E65100")));
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
        for (int i = 0; i < n; ++i)
            if (name == QLatin1String(list[i].name)) { fn = list[i].fn; return true; }
        return false;
    };
    if (!findIn(kEntradas, int(sizeof(kEntradas) / sizeof(kEntradas[0])))
        && !findIn(kSaidas, int(sizeof(kSaidas) / sizeof(kSaidas[0])))
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
