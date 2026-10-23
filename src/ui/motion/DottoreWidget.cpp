// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/motion/DottoreWidget.h"
#include "ui/core/Theme.h"
#include "colombina/generators.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kHandleR = 5.0;   // raio das alças em px
constexpr double kHandleTol = 9.0; // tolerância de clique

// Normaliza um ângulo para (-180, 180] (mesmo critério do Pivot).
double normDeg(double a) {
    a = std::fmod(a, 360.0);
    if (a > 180.0) a -= 360.0;
    if (a <= -180.0) a += 360.0;
    return a;
}

// Roda um ponto em torno de um pivô (graus).
QPointF rotateAround(const QPointF& p, const QPointF& pivot, double deg) {
    const double r = deg * kPi / 180.0;
    const double ca = std::cos(r), sa = std::sin(r);
    const double dx = p.x() - pivot.x(), dy = p.y() - pivot.y();
    return QPointF(pivot.x() + dx * ca - dy * sa,
                   pivot.y() + dx * sa + dy * ca);
}
} // namespace

// ── DottoreCanvas ────────────────────────────────────────────────────────

DottoreCanvas::DottoreCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(240, 180);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

Clip* DottoreCanvas::resolveClip() const {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (const Track& t : m_project->videoTracks)
        for (const Clip& c : t.clips)
            if (c.id == m_clipId) return const_cast<Clip*>(&c);
    return nullptr;
}

void DottoreCanvas::setClipId(Project* project, const QString& clipId,
                              double playhead) {
    m_project = project;
    m_clipId = clipId;
    m_playhead = playhead;
    update();
}

void DottoreCanvas::setPlayhead(double t) {
    m_playhead = t;
    update();
}

void DottoreCanvas::commitChange() { emit changed(); }

QRect DottoreCanvas::plotRect() const {
    return QRect(16, 16, std::max(10, width() - 32), std::max(10, height() - 32));
}

double DottoreCanvas::pxToImage(double px) const {
    // O canvas mostra a imagem do clipe com "contain" dentro do plot. Para
    // clipes de forma a "imagem" é o quadro do projeto, então convertemos px
    // de widget para px de imagem pela mesma razão usada no fit.
    const Clip* c = resolveClip();
    const QRect plot = plotRect();
    const int iw = (c && !c->isText) ? std::max(1, m_project->width) : 1920;
    const int ih = (c && !c->isText) ? std::max(1, m_project->height) : 1080;
    const double fit = std::min(plot.width() / double(iw), plot.height() / double(ih));
    return fit > 1e-6 ? px / fit : px;
}

QRectF DottoreCanvas::shapeRect(const Clip& c, double* outRot) const {
    if (outRot) *outRot = 0.0;
    const MediaItem* m = m_project ? m_project->findMedia(c.mediaId) : nullptr;
    if (!m || m->generator != QLatin1String("shape")) return QRectF();
    const QRect plot = plotRect();
    const int iw = std::max(1, m_project->width);
    const int ih = std::max(1, m_project->height);
    const double fit = std::min(plot.width() / double(iw), plot.height() / double(ih));
    const double rel = std::clamp(m_playhead - c.pos, 0.0, c.dur);
    const double sc = std::max(0.01, kfValue(c.kfScale, c.scale, rel));
    const double scX = std::max(0.01, kfValue(c.kfScaleX, c.scaleX, rel));
    const double scY = std::max(0.01, kfValue(c.kfScaleY, c.scaleY, rel));
    const double rot = kfValue(c.kfRotation, c.rotation, rel);
    const double tx = kfValue(c.kfTx, c.tx, rel);
    const double ty = kfValue(c.kfTy, c.ty, rel);

    // A forma ocupa shapeW×shapeH frações do quadro; na tela isso vira
    // (imagem cheia) * shapeW * (fit*sc*scX). O centro segue o pan do clipe.
    const double bw = iw * std::clamp(m->shapeW, 0.0, 4.0) * fit * sc * scX;
    const double bh = ih * std::clamp(m->shapeH, 0.0, 4.0) * fit * sc * scY;
    const QPointF center = plot.center() + QPointF(tx * fit, ty * fit);
    if (outRot) *outRot = rot;
    return QRectF(center.x() - bw / 2.0, center.y() - bh / 2.0, bw, bh);
}

int DottoreCanvas::handleAt(const QPointF& pos, const QRectF& box,
                            double rot) const {
    const QPointF c = box.center();
    const QPointF corners[4] = { box.topLeft(), box.topRight(),
                                 box.bottomRight(), box.bottomLeft() };
    for (int i = 0; i < 4; ++i) {
        const QPointF rc = rotateAround(corners[i], c, rot);
        if (QLineF(pos, rc).length() <= kHandleTol) return i;
    }
    // Sobre o corpo (rotacionado): desfaz a rotação e testa o eixo-alinhado.
    const QPointF local = rotateAround(pos, c, -rot);
    if (box.contains(local)) return 4; // corpo
    return -1;
}

void DottoreCanvas::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    update();
}

void DottoreCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto& tc = themeColors();
    p.fillRect(rect(), tc.canvasBg);

    Clip* c = resolveClip();
    if (!c || !m_project) {
        p.setPen(tc.monitorLabel);
        p.drawText(rect(), Qt::AlignCenter, tr("Selecione um clipe de forma"));
        return;
    }
    const MediaItem* m = m_project->findMedia(c->mediaId);
    if (!m || m->generator != QLatin1String("shape")) {
        p.setPen(tc.monitorLabel);
        p.drawText(rect(), Qt::AlignCenter,
                   tr("O clipe selecionado não é uma forma (use \"Forma\" na Central de Mídias)"));
        return;
    }

    const QRect plot = plotRect();
    p.setPen(tc.canvasBorder);
    p.drawRect(plot);

    // Renderiza o quadro da forma com o MESMO gerador do Preview/export.
    const int iw = std::max(1, m_project->width);
    const int ih = std::max(1, m_project->height);
    const double fit = std::min(plot.width() / double(iw), plot.height() / double(ih));
    const QSize disp(qRound(iw * fit), qRound(ih * fit));
    const QRect target(QPoint(plot.center().x() - disp.width() / 2,
                              plot.center().y() - disp.height() / 2), disp);
    const QImage frame = generatorFrame(*m, disp.width(), disp.height());
    // Xadrez leve para denunciar transparência (a forma é um PNG alpha).
    p.fillRect(target, QColor(0, 0, 0, 40));
    p.drawImage(target.topLeft(), frame);

    // Gizmo: caixa da forma + alças de canto + alça de rotação no topo.
    double rot = 0.0;
    const QRectF box = shapeRect(*c, &rot);
    if (box.isValid()) {
        const QPointF ctr = box.center();
        p.save();
        p.translate(ctr);
        p.rotate(rot);
        p.translate(-ctr);
        p.setPen(QPen(tc.accent, 1.5, Qt::DashLine));
        p.drawRect(box);
        p.setPen(QPen(tc.accent, 1.5));
        p.setBrush(tc.accent);
        const QPointF corners[4] = { box.topLeft(), box.topRight(),
                                     box.bottomRight(), box.bottomLeft() };
        for (const QPointF& pt : corners) p.drawEllipse(pt, kHandleR, kHandleR);
        // Alça de rotação acima do topo.
        const QPointF rotPt(box.center().x(), box.top() - 22.0);
        p.setPen(QPen(tc.accent, 1.5, Qt::DashDotLine));
        p.drawLine(QPointF(box.center().x(), box.top()), rotPt);
        p.setBrush(tc.accentGold);
        p.drawEllipse(rotPt, kHandleR, kHandleR);
        p.restore();
    }
}

void DottoreCanvas::mousePressEvent(QMouseEvent* e) {
    Clip* c = resolveClip();
    if (!c || e->button() != Qt::LeftButton) return;
    double rot = 0.0;
    const QRectF box = shapeRect(*c, &rot);
    if (!box.isValid()) return;
    const QPointF ctr = box.center();
    const QPointF rotPt = rotateAround(QPointF(box.center().x(), box.top() - 22.0),
                                       ctr, rot);
    m_dragStart = e->position();
    // Estado no playhead (não no valor base): com curvas já existentes, o
    // arraste parte do valor ANIMADO, senão a forma pularia para o base.
    const double pRel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    m_origTx = kfValue(c->kfTx, c->tx, pRel);
    m_origTy = kfValue(c->kfTy, c->ty, pRel);
    m_origScaleX = kfValue(c->kfScaleX, c->scaleX, pRel);
    m_origScaleY = kfValue(c->kfScaleY, c->scaleY, pRel);
    m_dragAngle0 = std::atan2(e->position().y() - ctr.y(),
                              e->position().x() - ctr.x()) * 180.0 / kPi
                   - rot;

    if (QLineF(e->position(), rotPt).length() <= kHandleTol) {
        m_drag = DragRotate;
    } else {
        const int h = handleAt(e->position(), box, rot);
        if (h >= 0 && h < 4) {
            m_drag = DragScaleTL; // base; ajustado abaixo
            m_dragCorner = h;
            switch (h) {
                case 0: m_drag = DragScaleTL; break;
                case 1: m_drag = DragScaleTR; break;
                case 2: m_drag = DragScaleBR; break;
                default: m_drag = DragScaleBL; break;
            }
        } else if (h == 4) {
            m_drag = DragMove;
        } else {
            m_drag = DragNone;
            return;
        }
    }
    emit editStart();
    update();
}

void DottoreCanvas::mouseMoveEvent(QMouseEvent* e) {
    Clip* c = resolveClip();
    if (!c) return;
    if (m_drag == DragNone) {
        // Cursor conforme a alça sob o mouse.
        double rot = 0.0;
        const QRectF box = shapeRect(*c, &rot);
        const int h = box.isValid() ? handleAt(e->position(), box, rot) : -1;
        if (h == 4) setCursor(Qt::SizeAllCursor);
        else if (h >= 0) setCursor(Qt::SizeFDiagCursor);
        else unsetCursor();
        return;
    }
    const QPointF d = e->position() - m_dragStart;
    // Auto-key: grava keyframes de transform no playhead (relativo ao clipe);
    // senão escreve o valor base. Em ambos, o valor-alvo vem do arraste.
    const double rel = std::clamp(m_playhead - c->pos, 0.0, c->dur);
    if (m_drag == DragMove) {
        const double tx = m_origTx + pxToImage(d.x());
        const double ty = m_origTy + pxToImage(d.y());
        if (m_autoKey) {
            upsertKeyframe(c->kfTx, rel, tx, KfSmooth);
            upsertKeyframe(c->kfTy, rel, ty, KfSmooth);
        }
        c->tx = tx;
        c->ty = ty;
    } else if (m_drag == DragRotate) {
        const QRectF box = shapeRect(*c, nullptr);
        const QPointF ctr = box.center();
        const double ang = std::atan2(e->position().y() - ctr.y(),
                                      e->position().x() - ctr.x()) * 180.0 / kPi;
        const double rot = normDeg(ang - m_dragAngle0);
        if (m_autoKey) upsertKeyframe(c->kfRotation, rel, rot, KfSmooth);
        c->rotation = rot;
    } else {
        // Escala uniforme pelo arraste do canto: projeta o delta no vetor
        // diagonal do canto oposto (mantém a forma ancorada no canto parado).
        double rot = 0.0;
        const QRectF box = shapeRect(*c, &rot);
        if (!box.isValid()) return;
        const double halfW = box.width() / 2.0;
        const double halfH = box.height() / 2.0;
        if (halfW < 1e-3 || halfH < 1e-3) return;
        const double fx = m_dragCorner == 1 || m_dragCorner == 2 ? 1.0 : -1.0;
        const double fy = m_dragCorner == 2 || m_dragCorner == 3 ? 1.0 : -1.0;
        const double g = (d.x() * fx * halfW + d.y() * fy * halfH)
                         / (halfW * halfW + halfH * halfH);
        const double k = std::clamp(1.0 + g, 0.02, 20.0);
        const double sx = m_origScaleX * k;
        const double sy = m_origScaleY * k;
        if (m_autoKey) {
            upsertKeyframe(c->kfScaleX, rel, sx, KfSmooth);
            upsertKeyframe(c->kfScaleY, rel, sy, KfSmooth);
        }
        c->scaleX = sx;
        c->scaleY = sy;
    }
    update();
    emit changed();
}

void DottoreCanvas::mouseReleaseEvent(QMouseEvent* e) {
    if (m_drag != DragNone && e->button() == Qt::LeftButton) {
        m_drag = DragNone;
        m_dragCorner = -1;
        update();
    }
}

// ── DottoreWidget ────────────────────────────────────────────────────────

namespace {
QDoubleSpinBox* makeSpin(double lo, double hi, double step, int dec) {
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(dec);
    return s;
}
} // namespace

DottoreWidget::DottoreWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    m_title = new QLabel(tr("Dottore — Motion"), this);
    m_title->setStyleSheet(QStringLiteral("font-weight:600;"));
    root->addWidget(m_title);

    m_canvas = new DottoreCanvas(this);
    root->addWidget(m_canvas, 1);

    // Painel de propriedades da forma (fill/stroke/dimensões).
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(3);
    int row = 0;

    auto* typeLabel = new QLabel(tr("Tipo"), this);
    m_typeCombo = new QComboBox(this);
    m_typeCombo->addItem(tr("Retângulo"), QStringLiteral("rect"));
    m_typeCombo->addItem(tr("Elipse"), QStringLiteral("ellipse"));
    grid->addWidget(typeLabel, row, 0);
    grid->addWidget(m_typeCombo, row, 1, 1, 3);
    ++row;

    auto* fillBtn = new QPushButton(tr("Cor (fill)"), this);
    auto* strokeBtn = new QPushButton(tr("Borda (stroke)"), this);
    grid->addWidget(fillBtn, row, 0, 1, 2);
    grid->addWidget(strokeBtn, row, 2, 1, 2);
    ++row;

    auto* wLabel = new QLabel(tr("Largura"), this);
    m_wSpin = makeSpin(0.0, 4.0, 0.01, 3);
    grid->addWidget(wLabel, row, 0);
    grid->addWidget(m_wSpin, row, 1);
    auto* hLabel = new QLabel(tr("Altura"), this);
    m_hSpin = makeSpin(0.0, 4.0, 0.01, 3);
    grid->addWidget(hLabel, row, 2);
    grid->addWidget(m_hSpin, row, 3);
    ++row;

    auto* rLabel = new QLabel(tr("Cantos"), this);
    m_rSpin = makeSpin(0.0, 0.5, 0.01, 3);
    grid->addWidget(rLabel, row, 0);
    grid->addWidget(m_rSpin, row, 1);
    auto* sLabel = new QLabel(tr("Borda px"), this);
    m_sSpin = makeSpin(0.0, 200.0, 1.0, 1);
    grid->addWidget(sLabel, row, 2);
    grid->addWidget(m_sSpin, row, 3);
    ++row;

    root->addLayout(grid);

    // Auto-keyframe: mover/escalar/girar no canvas grava curvas no playhead,
    // animando o transform pelas curvas do Editor de Curvas.
    m_autoBtn = new QPushButton(tr("● Auto"), this);
    m_autoBtn->setCheckable(true);
    m_autoBtn->setChecked(true);
    m_autoBtn->setToolTip(tr("Auto-keyframe: mover/escalar/girar cria ou "
                             "atualiza keyframes de transform no playhead."));
    root->addWidget(m_autoBtn);

    m_info = new QLabel(tr("Arraste = mover · cantos = escala · alça do topo = girar"), this);
    m_info->setStyleSheet(QStringLiteral("color:#888;"));
    m_info->setWordWrap(true);
    root->addWidget(m_info);

    // ── Ligações ──────────────────────────────────────────────────────
    // Editores de parâmetro disparam editStart (undo) e modified, e refletem
    // a mudança no canvas. Guardados por m_loading para não re-entrar quando o
    // syncControls() reescreve os widgets.
    connect(m_typeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        MediaItem* m = activeShapeMedia();
        if (!m || m_loading) return;
        emit editStart();
        m->shapeType = m_typeCombo->currentData().toString();
        emit modified();
        m_canvas->update();
    });
    connect(fillBtn, &QPushButton::clicked, this, [this]() {
        MediaItem* m = activeShapeMedia();
        if (!m) return;
        const QColor col = QColorDialog::getColor(m->solidColor, this, tr("Cor da forma"));
        if (!col.isValid()) return;
        emit editStart();
        m->solidColor = col;
        emit modified();
        m_canvas->update();
    });
    connect(strokeBtn, &QPushButton::clicked, this, [this]() {
        MediaItem* m = activeShapeMedia();
        if (!m) return;
        const QColor col = QColorDialog::getColor(m->solidColor2, this, tr("Cor da borda"));
        if (!col.isValid()) return;
        emit editStart();
        m->solidColor2 = col;
        emit modified();
        m_canvas->update();
    });
    connect(m_wSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        MediaItem* m = activeShapeMedia();
        if (!m || m_loading) return;
        emit editStart();
        m->shapeW = v; emit modified(); m_canvas->update();
    });
    connect(m_hSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        MediaItem* m = activeShapeMedia();
        if (!m || m_loading) return;
        emit editStart();
        m->shapeH = v; emit modified(); m_canvas->update();
    });
    connect(m_rSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        MediaItem* m = activeShapeMedia();
        if (!m || m_loading) return;
        emit editStart();
        m->shapeRoundness = v; emit modified(); m_canvas->update();
    });
    connect(m_sSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        MediaItem* m = activeShapeMedia();
        if (!m || m_loading) return;
        emit editStart();
        m->shapeStrokeWidth = v; emit modified(); m_canvas->update();
    });

    connect(m_canvas, &DottoreCanvas::editStart, this, &DottoreWidget::editStart);
    connect(m_canvas, &DottoreCanvas::changed, this, [this]() {
        syncControls();
        emit modified();
        // O auto-key grava keyframes de transform: avisa o Editor de Curvas.
        if (m_canvas->autoKey()) emit keyframesChanged();
    });
    connect(m_canvas, &DottoreCanvas::statusMessage, this, [this](const QString& s) {
        if (m_info) m_info->setText(s);
    });
    connect(m_autoBtn, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setAutoKey(on);
    });
}

Clip* DottoreWidget::activeClip() const {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (const Track& t : m_project->videoTracks)
        for (const Clip& c : t.clips)
            if (c.id == m_clipId) return const_cast<Clip*>(&c);
    return nullptr;
}

MediaItem* DottoreWidget::activeShapeMedia() {
    Clip* c = activeClip();
    if (!c || !m_project) return nullptr;
    for (MediaItem& m : m_project->media)
        if (m.id == c->mediaId && m.generator == QLatin1String("shape")) return &m;
    return nullptr;
}

void DottoreWidget::syncControls() {
    MediaItem* m = activeShapeMedia();
    m_loading = true;
    if (m) {
        const int idx = m_typeCombo->findData(m->shapeType);
        if (idx >= 0) m_typeCombo->setCurrentIndex(idx);
        m_wSpin->setValue(m->shapeW);
        m_hSpin->setValue(m->shapeH);
        m_rSpin->setValue(m->shapeRoundness);
        m_sSpin->setValue(m->shapeStrokeWidth);
        if (m_title) m_title->setText(tr("Dottore — %1").arg(m->name));
    } else {
        if (m_title) m_title->setText(tr("Dottore — Motion"));
    }
    m_loading = false;
}

void DottoreWidget::setClipId(const QString& id) {
    m_clipId = id;
    m_canvas->setClipId(m_project, id, m_playhead);
    syncControls();
    m_canvas->update();
}

void DottoreWidget::setPlayhead(double t) {
    m_playhead = t;
    m_canvas->setPlayhead(t);
}

void DottoreWidget::refresh() {
    syncControls();
    m_canvas->update();
}
