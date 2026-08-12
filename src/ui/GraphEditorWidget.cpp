#include "GraphEditorWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kMarginL = 56;
constexpr int kMarginR = 16;
constexpr int kMarginT = 18;
constexpr int kMarginB = 22;

double niceStep(double raw) {
    if (raw <= 0) return 1.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double nice;
    if (norm < 1.5)      nice = 1.0;
    else if (norm < 3.0) nice = 2.0;
    else if (norm < 7.0) nice = 5.0;
    else                 nice = 10.0;
    return nice * mag;
}

QString propName(GraphProp p) {
    switch (p) {
        case GPropOpacity:   return "Opacity";
        case GPropVolume:    return "Volume";
        case GPropScale:     return "Scale";
        case GPropRotation:  return "Rotation";
        case GPropTx:        return "Position X";
        case GPropTy:        return "Position Y";
        case GPropCropL:     return "Crop Left";
        case GPropCropR:     return "Crop Right";
        case GPropCropT:     return "Crop Top";
        case GPropCropB:     return "Crop Bottom";
    }
    return QString();
}

QString interpName(int interp) {
    switch (interp) {
        case KfSmooth: return "Suave";
        case KfStep:   return "Segurar";
        case KfBezier: return "Bezier";
        default:       return "Linear";
    }
}

} // namespace

// --------------------------------------------------------------------------
// GraphCanvas
// --------------------------------------------------------------------------

GraphCanvas::GraphCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(140);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
}

void GraphCanvas::setData(Clip* clip, GraphProp prop, double playhead, double fps) {
    m_clip = clip;
    m_prop = prop;
    m_playhead = playhead;
    m_fps = (fps > 1.0) ? fps : 30.0;
    valueRange(&m_lo, &m_hi);
    m_dragKey = -1;
    m_dragHandle = -1;
    m_hoverKey = -1;
    m_undoPushed = false;
    update();
}

QVector<Keyframe>* GraphCanvas::keys() const {
    if (!m_clip) return nullptr;
    switch (m_prop) {
        case GPropOpacity:  return &m_clip->kfOpacity;
        case GPropVolume:   return &m_clip->kfVolume;
        case GPropScale:    return &m_clip->kfScale;
        case GPropRotation: return &m_clip->kfRotation;
        case GPropTx:       return &m_clip->kfTx;
        case GPropTy:       return &m_clip->kfTy;
        case GPropCropL:    return &m_clip->kfCropL;
        case GPropCropR:    return &m_clip->kfCropR;
        case GPropCropT:    return &m_clip->kfCropT;
        case GPropCropB:    return &m_clip->kfCropB;
    }
    return nullptr;
}

double GraphCanvas::baseValue() const {
    if (!m_clip) return 0.0;
    switch (m_prop) {
        case GPropOpacity:  return m_clip->opacity;
        case GPropVolume:   return m_clip->volume;
        case GPropScale:    return m_clip->scale;
        case GPropRotation: return m_clip->rotation;
        case GPropTx:       return m_clip->tx;
        case GPropTy:       return m_clip->ty;
        case GPropCropL:    return m_clip->cropL;
        case GPropCropR:    return m_clip->cropR;
        case GPropCropT:    return m_clip->cropT;
        case GPropCropB:    return m_clip->cropB;
    }
    return 0.0;
}

void GraphCanvas::valueRange(double* lo, double* hi) const {
    switch (m_prop) {
        case GPropOpacity:  *lo = 0.0; *hi = 1.0; return;
        case GPropVolume:   *lo = 0.0; *hi = 2.0; return;
        case GPropScale:    *lo = 0.0; *hi = 3.0; return;
        case GPropRotation: *lo = -360.0; *hi = 360.0; return;
        case GPropTx: *lo = -800.0; *hi = 800.0; return;
        case GPropTy: *lo = -450.0; *hi = 450.0; return;
        case GPropCropL: case GPropCropR:
        case GPropCropT: case GPropCropB:
            *lo = 0.0; *hi = 1.0; return;
    }
    *lo = 0.0; *hi = 1.0;
}

QRect GraphCanvas::plotRect() const {
    return QRect(kMarginL, kMarginT,
                 std::max(10, width() - kMarginL - kMarginR),
                 std::max(10, height() - kMarginT - kMarginB));
}

double GraphCanvas::xToT(int x) const {
    const QRect r = plotRect();
    const double dur = m_clip ? std::max(0.05, m_clip->dur) : 1.0;
    return std::clamp((x - r.left()) / (double)r.width(), 0.0, 1.0) * dur;
}

int GraphCanvas::tToX(double t) const {
    const QRect r = plotRect();
    const double dur = m_clip ? std::max(0.05, m_clip->dur) : 1.0;
    return r.left() + (int)std::lround(t / dur * r.width());
}

// Arredonda o tempo para a grade de frames (1/fps); a precisão livre fica
// disponível segurando Ctrl durante o arrasto.
double GraphCanvas::snapTime(double t) const {
    const double fr = 1.0 / std::max(1.0, m_fps);
    return std::round(t / fr) * fr;
}

QString GraphCanvas::fmtTime(double t) const {
    return QStringLiteral("%1s").arg(t, 0, 'f', 3);
}

QString GraphCanvas::fmtValue(double v) const {
    const int prec = (std::fabs(m_hi - m_lo) < 2.0) ? 4 : 5;
    return QString::number(v, 'g', prec);
}

double GraphCanvas::yToV(int y) const {
    const QRect r = plotRect();
    const double f = std::clamp((r.bottom() - y) / (double)r.height(), 0.0, 1.0);
    return m_lo + (m_hi - m_lo) * f;
}

int GraphCanvas::vToY(double v) const {
    const QRect r = plotRect();
    const double f = (std::clamp(v, m_lo, m_hi) - m_lo) / (m_hi - m_lo);
    return r.bottom() - (int)std::lround(f * r.height());
}

int GraphCanvas::keyframeHit(const QPoint& p) const {
    const QVector<Keyframe>* ks = keys();
    if (!ks) return -1;
    for (int i = 0; i < ks->size(); ++i) {
        const QPointF kp(tToX((*ks)[i].time), vToY((*ks)[i].value));
        if (std::hypot(p.x() - kp.x(), p.y() - kp.y()) <= 7.0) return i;
    }
    return -1;
}

int GraphCanvas::handleHit(const QPoint& p) const {
    const QVector<Keyframe>* ks = keys();
    if (!ks) return -1;
    for (int i = 0; i + 1 < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        if (k.interp != KfSmooth && k.interp != KfBezier) continue;
        const double span = (*ks)[i + 1].time - k.time;
        if (span <= 1e-9) continue;
        double cx = k.hx;
        if (k.interp == KfSmooth)
            cx = std::clamp(span * 0.35, 0.0, span);
        const QPointF hp(tToX(k.time + cx), vToY(k.value + k.hy));
        if (std::hypot(p.x() - hp.x(), p.y() - hp.y()) <= 8.0) return i;
    }
    return -1;
}

void GraphCanvas::sortKeys() {
    QVector<Keyframe>* ks = keys();
    if (!ks) return;
    std::sort(ks->begin(), ks->end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

void GraphCanvas::commitChange() {
    sortKeys();
    emit modified();
    update();
}

void GraphCanvas::updateHover(const QPoint& p) {
    const int hk = (m_clip && keys()) ? keyframeHit(p) : -1;
    if (hk == m_hoverKey) return;
    m_hoverKey = hk;
    if (m_hoverKey >= 0)
        emitKeyInfo(m_hoverKey);
    else
        emit statusMessage(QString());
    setCursor(m_hoverKey >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void GraphCanvas::emitKeyInfo(int idx) {
    const QVector<Keyframe>* ks = keys();
    if (!ks || idx < 0 || idx >= ks->size()) return;
    const Keyframe& k = (*ks)[idx];
    const int frame = (int)std::lround(k.time * m_fps);
    emit statusMessage(tr("Keyframe %1/%2  ·  t = %3 (frame %4)  ·  v = %5")
                           .arg(idx + 1)
                           .arg(ks->size())
                           .arg(fmtTime(k.time))
                           .arg(frame)
                           .arg(fmtValue(k.value)));
}

void GraphCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(28, 28, 32));
    const QRect r = plotRect();
    if (r.width() <= 0 || r.height() <= 0) return;

    // Grade de valores.
    p.setPen(QColor(50, 50, 58));
    const double vStep = niceStep((m_hi - m_lo) / 4.0);
    for (double v = std::ceil(m_lo / vStep) * vStep; v <= m_hi + 1e-9; v += vStep) {
        const int y = vToY(v);
        p.drawLine(r.left(), y, r.right(), y);
        p.setPen(QColor(150, 150, 160));
        p.drawText(QRect(0, y - 8, kMarginL - 6, 16),
                   Qt::AlignRight | Qt::AlignVCenter, fmtValue(v));
        p.setPen(QColor(50, 50, 58));
    }
    // Grade de tempo.
    const double dur = m_clip ? std::max(0.05, m_clip->dur) : 1.0;
    const double tStep = niceStep(dur / 6.0);
    for (double t = 0.0; t <= dur + 1e-9; t += tStep) {
        const int x = tToX(t);
        p.drawLine(x, r.top(), x, r.bottom());
        p.setPen(QColor(150, 150, 160));
        const QString tlab = (tStep < 1.0)
            ? fmtTime(t)
            : QStringLiteral("%1s").arg(QString::number(t, 'g', 3));
        p.drawText(QRect(x - 24, r.bottom() + 4, 48, 16),
                   Qt::AlignHCenter | Qt::AlignVCenter, tlab);
        p.setPen(QColor(50, 50, 58));
    }

    const QVector<Keyframe>* ks = keys();
    if (!ks) return;

    // Curva (sem keyframes: linha base pontilhada).
    QPainterPath path;
    bool havePath = false;
    const int steps = std::max(2, r.width() / 2);
    for (int i = 0; i <= steps; ++i) {
        const double t = dur * i / steps;
        const double v = kfValue(*ks, baseValue(), t);
        const QPointF pt(tToX(t), vToY(v));
        if (!havePath) { path.moveTo(pt); havePath = true; }
        else path.lineTo(pt);
    }
    if (ks->isEmpty()) {
        p.setPen(QPen(QColor(110, 110, 120), 1, Qt::DashLine));
        p.drawLine(r.left(), vToY(baseValue()), r.right(), vToY(baseValue()));
    } else {
        p.setPen(QPen(QColor(90, 180, 255), 2));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawPath(path);
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // Handles ("tracinhos") do keyframe selecionado.
    if (m_dragKey >= 0 && m_dragKey < ks->size()) {
        const Keyframe& k = (*ks)[m_dragKey];
        if (k.interp == KfSmooth || k.interp == KfBezier) {
            if (m_dragKey + 1 < ks->size()) {
                const double span = (*ks)[m_dragKey + 1].time - k.time;
                if (span > 1e-9) {
                    double cx = k.hx;
                    double cy = k.hy;
                    if (k.interp == KfSmooth) {
                        // Tangente Catmull-Rom (decorativa).
                        const Keyframe& a = k;
                        const Keyframe& b = (*ks)[m_dragKey + 1];
                        const Keyframe& p0 = (m_dragKey > 0) ? (*ks)[m_dragKey - 1] : a;
                        const double m0 = (b.value - p0.value) / (b.time - p0.time);
                        cx = span * 0.35;
                        cy = m0 * span * 0.35;
                    }
                    const QPointF base(tToX(k.time), vToY(k.value));
                    const QPointF hp(tToX(k.time + cx), vToY(k.value + cy));
                    p.setPen(QPen(QColor(255, 200, 90), 1.5, Qt::DashLine));
                    p.drawLine(base, hp);
                    p.setBrush(QColor(255, 200, 90));
                    p.setPen(Qt::NoPen);
                    p.drawEllipse(hp, 4, 4);
                }
            }
        }
    }

    // Keyframes (losangos).
    for (int i = 0; i < ks->size(); ++i) {
        const Keyframe& k = (*ks)[i];
        const QPointF kp(tToX(k.time), vToY(k.value));
        const QColor fill = (i == m_dragKey)
            ? QColor(90, 200, 120) : QColor(210, 210, 220);
        p.setBrush(fill);
        p.setPen(QPen(QColor(255, 255, 255), 1.2));
        const QPolygonF dia = QPolygonF() << QPointF(kp.x(), kp.y() - 6)
                                          << QPointF(kp.x() + 6, kp.y())
                                          << QPointF(kp.x(), kp.y() + 6)
                                          << QPointF(kp.x() - 6, kp.y());
        p.drawPolygon(dia);
    }

    // Leitura exata do keyframe sob o mouse ou sendo arrastado (tempo, frame
    // e valor), para saber com precisão onde cada keyframe está.
    const int infoKey = (m_dragKey >= 0) ? m_dragKey : m_hoverKey;
    if (infoKey >= 0 && infoKey < ks->size()) {
        const Keyframe& k = (*ks)[infoKey];
        const QPointF kp(tToX(k.time), vToY(k.value));
        const QString txt = tr("t = %1  ·  frame %2\nv = %3  ·  %4")
                                .arg(fmtTime(k.time))
                                .arg((int)std::lround(k.time * m_fps))
                                .arg(fmtValue(k.value))
                                .arg(interpName(k.interp));
        QRect box(kp.x() + 12, kp.y() - 34, 168, 36);
        if (box.right() > width() - 4)
            box.moveLeft(kp.x() - 12 - box.width());
        if (box.top() < 4)
            box.moveTop(kp.y() + 14);
        p.setPen(QPen(QColor(255, 200, 90), 1));
        p.setBrush(QColor(40, 40, 46, 232));
        p.drawRect(box);
        p.setPen(QColor(255, 235, 200));
        p.drawText(box, Qt::AlignCenter, txt);
    }

    // Linha do playhead.
    if (m_clip) {
        const double rel = std::clamp(m_playhead - m_clip->pos, 0.0, m_clip->dur);
        const int x = tToX(rel);
        p.setPen(QPen(QColor(0, 160, 255), 1));
        p.drawLine(x, r.top(), x, r.bottom());
    }
}

void GraphCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !m_clip) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_lastPos = e->pos();
    updateHover(e->pos());
    const int kh = keyframeHit(e->pos());
    if (kh >= 0) {
        m_dragKey = kh;
        m_dragHandle = -1;
        emit editStart();
        m_undoPushed = true;
        setCursor(Qt::ClosedHandCursor);
        update();
        return;
    }
    const int hh = handleHit(e->pos());
    if (hh >= 0) {
        m_dragKey = hh;
        m_dragHandle = hh;
        QVector<Keyframe>& K = *keys();
        if (K[hh].interp == KfSmooth) {
            // Pegar o handle de um keyframe suave o converte em bezier
            // (os handles passam a ser manuais, como no DaVinci Resolve).
            K[hh].interp = KfBezier;
            emit editStart();
            m_undoPushed = true;
        }
        setCursor(Qt::SizeFDiagCursor);
        update();
        return;
    }
    m_dragKey = -1;
    m_dragHandle = -1;
    update();
    QWidget::mousePressEvent(e);
}

void GraphCanvas::mouseMoveEvent(QMouseEvent* e) {
    m_lastPos = e->pos();
    if (m_dragKey < 0 || !m_clip) {
        updateHover(e->pos());
        QWidget::mouseMoveEvent(e);
        return;
    }
    const QVector<Keyframe>* ks = keys();
    if (!ks || m_dragKey >= ks->size()) return;
    QVector<Keyframe>& K = *keys();
    const double dur = std::max(0.05, m_clip->dur);

    if (m_dragHandle >= 0 && K[m_dragKey].interp == KfBezier) {
        const Keyframe& k = K[m_dragKey];
        const double maxDx = (m_dragKey + 1 < K.size())
            ? (K[m_dragKey + 1].time - k.time) * 0.5 : dur * 0.5;
        const double newDx = std::clamp(xToT(e->pos().x()) - k.time, 0.0, maxDx);
        const double newDy = std::clamp(yToV(e->pos().y()) - k.value,
                                        m_lo - k.value, m_hi - k.value);
        K[m_dragKey].hx = newDx;
        K[m_dragKey].hy = newDy;
        emitKeyInfo(m_dragKey);
        commitChange();
        return;
    }

    Keyframe& k = K[m_dragKey];
    const double tMin = (m_dragKey > 0) ? K[m_dragKey - 1].time + 0.001 : 0.0;
    const double tMax = (m_dragKey + 1 < K.size())
        ? K[m_dragKey + 1].time - 0.001 : dur;
    double t = std::clamp(xToT(e->pos().x()), tMin, tMax);
    // Snap à grade de frames; segure Ctrl para mover com precisão livre.
    if (!(e->modifiers() & Qt::ControlModifier))
        t = std::clamp(snapTime(t), tMin, tMax);
    k.time = t;
    k.value = std::clamp(yToV(e->pos().y()), m_lo, m_hi);
    emitKeyInfo(m_dragKey);
    commitChange();
}

void GraphCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragKey >= 0) {
        m_dragKey = -1;
        m_dragHandle = -1;
        m_undoPushed = false;
        updateHover(e->pos());
        update();
    }
    QWidget::mouseReleaseEvent(e);
}

void GraphCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_clip) { QWidget::mouseDoubleClickEvent(e); return; }
    if (keyframeHit(e->pos()) >= 0) { QWidget::mouseDoubleClickEvent(e); return; }
    QVector<Keyframe>& K = *keys();
    const double t = snapTime(xToT(e->pos().x()));
    const double v = kfValue(K, baseValue(), t);
    Keyframe nk;
    nk.time = t;
    nk.value = v;
    nk.interp = KfSmooth;
    K.append(nk);
    emit editStart();
    commitChange();
    m_undoPushed = true;
}

void GraphCanvas::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_clip) { QWidget::contextMenuEvent(e); return; }
    QVector<Keyframe>& K = *keys();
    const int hit = keyframeHit(e->pos());
    if (hit < 0) { QWidget::contextMenuEvent(e); return; }
    m_dragKey = hit;
    update();

    QMenu menu(this);
    QAction* lin = menu.addAction(tr("Linear"));
    QAction* smo = menu.addAction(tr("Suave"));
    QAction* ste = menu.addAction(tr("Segurar"));
    QAction* bez = menu.addAction(tr("Bezier"));
    menu.addSeparator();
    QAction* del = menu.addAction(tr("Excluir keyframe"));
    QAction* act = menu.exec(e->globalPos());
    if (!act) return;
    emit editStart();
    m_undoPushed = true;
    if (act == lin) K[hit].interp = KfLinear;
    else if (act == smo) { K[hit].interp = KfSmooth; K[hit].hx = K[hit].hy = 0.0; }
    else if (act == ste) K[hit].interp = KfStep;
    else if (act == bez) K[hit].interp = KfBezier;
    else if (act == del) K.removeAt(hit);
    commitChange();
}

// --------------------------------------------------------------------------
// GraphEditorWidget
// --------------------------------------------------------------------------

GraphEditorWidget::GraphEditorWidget(QWidget* parent) : QWidget(parent) {
    auto* lbl = new QLabel(tr("Propriedade:"), this);
    m_propCombo = new QComboBox(this);
    connect(m_propCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        m_canvas->setData(activeClip(), currentProp(), m_playhead,
                          m_project ? m_project->fps : 30.0);
    });

    auto* addBtn = new QPushButton(tr("+ no playhead"), this);
    addBtn->setToolTip(tr("Adicionar keyframe no playhead"));
    connect(addBtn, &QPushButton::clicked, this, [this]() {
        Clip* c = activeClip();
        if (!c) return;
        const double rel0 = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        const double fr = 1.0 / std::max(1.0, m_project ? m_project->fps : 30.0);
        const double rel = std::round(rel0 / fr) * fr;
        QVector<Keyframe>& K = *m_canvas->keys();
        const double v = kfValue(K, m_canvas->baseValue(), rel);
        for (const Keyframe& k : K)
            if (std::fabs(k.time - rel) < 1e-6) return;
        Keyframe nk;
        nk.time = rel;
        nk.value = v;
        nk.interp = KfSmooth;
        K.append(nk);
        emit editStart();
        m_canvas->commitChange();
    });

    auto* delBtn = new QPushButton(tr("Excluir keyframe"), this);
    delBtn->setToolTip(tr("Excluir o keyframe mais próximo do playhead"));
    connect(delBtn, &QPushButton::clicked, this, [this]() {
        Clip* c = activeClip();
        if (!c) return;
        QVector<Keyframe>& K = *m_canvas->keys();
        if (K.isEmpty()) return;
        const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
        int best = 0;
        double bestD = 1e18;
        for (int i = 0; i < K.size(); ++i) {
            const double d = std::fabs(K[i].time - rel);
            if (d < bestD) { bestD = d; best = i; }
        }
        emit editStart();
        K.removeAt(best);
        m_canvas->commitChange();
    });

    auto* top = new QHBoxLayout;
    top->setContentsMargins(6, 4, 6, 2);
    top->addWidget(lbl);
    top->addWidget(m_propCombo, 1);
    top->addWidget(addBtn);
    top->addWidget(delBtn);

    m_canvas = new GraphCanvas(this);
    connect(m_canvas, &GraphCanvas::editStart, this, &GraphEditorWidget::editStart);
    connect(m_canvas, &GraphCanvas::modified, this, &GraphEditorWidget::modified);

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color:#9aa; font-size:11px; padding:2px 6px;"));
    connect(m_canvas, &GraphCanvas::statusMessage, m_status, &QLabel::setText);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addLayout(top);
    lay->addWidget(m_canvas, 1);
    lay->addWidget(m_status);
}

void GraphEditorWidget::setProject(Project* p) {
    m_project = p;
    rebuildProperties();
    m_canvas->setData(activeClip(), currentProp(), m_playhead,
                      m_project ? m_project->fps : 30.0);
}

void GraphEditorWidget::setClipId(const QString& id) {
    if (m_clipId == id) return;
    m_clipId = id;
    rebuildProperties();
    m_canvas->setData(activeClip(), currentProp(), m_playhead,
                      m_project ? m_project->fps : 30.0);
}

void GraphEditorWidget::setPlayhead(double t) {
    m_playhead = t;
    m_canvas->update();
}

void GraphEditorWidget::refresh() {
    rebuildProperties();
    m_canvas->setData(activeClip(), currentProp(), m_playhead,
                      m_project ? m_project->fps : 30.0);
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

GraphProp GraphEditorWidget::currentProp() const {
    const int idx = m_propCombo->currentIndex();
    if (idx >= 0 && idx < m_props.size()) return m_props[idx];
    return GPropOpacity;
}

void GraphEditorWidget::rebuildProperties() {
    const QSignalBlocker b(m_propCombo);
    m_propCombo->clear();
    m_props.clear();

    Clip* c = activeClip();
    if (!c) return;
    bool hasVideo = false, hasAudio = false;
    if (m_project) {
        const MediaItem* mi = m_project->findMedia(c->mediaId);
        if (mi) { hasVideo = mi->hasVideo; hasAudio = mi->hasAudio; }
    }
    if (hasVideo) {
        const QVector<GraphProp> vid = { GPropOpacity, GPropScale, GPropRotation,
                                         GPropTx, GPropTy,
                                         GPropCropL, GPropCropR, GPropCropT, GPropCropB };
        for (GraphProp p : vid) { m_props.append(p); m_propCombo->addItem(propName(p)); }
    }
    if (hasAudio) {
        m_props.append(GPropVolume);
        m_propCombo->addItem(propName(GPropVolume));
    }
    if (m_props.isEmpty()) {
        m_props.append(GPropOpacity);
        m_propCombo->addItem(propName(GPropOpacity));
    }
}
