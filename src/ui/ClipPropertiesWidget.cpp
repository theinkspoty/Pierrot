// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ClipPropertiesWidget.h"

#include "TextEditorDialog.h"
#include "Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

namespace {

// Ícone de "cronômetro" do Effect Controls (AE): bolinha + botão em cima.
// Azul (checked) quando a propriedade tem keyframe exatamente no playhead.
QIcon stopwatchIcon(bool on) {
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor c = on ? themeColors().accent : themeColors().iconMuted;
    p.setPen(QPen(c, 1.4));
    QColor fill = themeColors().accent;
    fill.setAlpha(40);
    p.setBrush(on ? fill : Qt::NoBrush);
    p.drawEllipse(QPointF(8, 8), 5.2, 5.2);
    p.drawRect(QRectF(7, 0, 2, 2.6));
    p.drawLine(QPointF(8, 8), QPointF(8, 4.6));
    return QIcon(pm);
}

// Índice de um keyframe exatamente em `t` (tolerância de um micro), ou -1.
int keyframeAt(const QVector<Keyframe>& ks, double t) {
    for (int i = 0; i < ks.size(); ++i)
        if (std::fabs(ks[i].time - t) < 1e-6) return i;
    return -1;
}

} // namespace

ClipPropertiesWidget::ClipPropertiesWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background:%1;").arg(themeColors().window.name()));

    m_undoTimer = new QTimer(this);
    m_undoTimer->setSingleShot(true);
    m_undoTimer->setInterval(700);
    connect(m_undoTimer, &QTimer::timeout, this, [this]() { m_undoPushed = false; });

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background:%1; }").arg(themeColors().window.name()));

    m_body = new QWidget(m_scroll);
    auto* bodyLay = new QVBoxLayout(m_body);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    bodyLay->setSpacing(1);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_scroll);
    m_scroll->setWidget(m_body);

    rebuild();
}

void ClipPropertiesWidget::setProject(Project* p) {
    m_project = p;
    refresh();
}

void ClipPropertiesWidget::setPlayhead(double t) {
    if (std::fabs(m_playhead - t) < 1e-9) return;
    m_playhead = t;
    refreshValues();
    refreshStopwatches();
}

void ClipPropertiesWidget::showClip(const QString& clipId) {
    m_clipId = clipId;
    m_trackId.clear();
    m_mesaId.clear();
    rebuild();
}

void ClipPropertiesWidget::showMesaLayer(const QString& trackId) {
    m_trackId = trackId;
    m_clipId.clear();
    m_mesaId.clear();
    rebuild();
}

void ClipPropertiesWidget::showMesaCamera(const QString& mesaId) {
    m_mesaId = mesaId;
    m_clipId.clear();
    m_trackId.clear();
    rebuild();
}

void ClipPropertiesWidget::refresh() {
    rebuild();
}

// ═══════════════════════════════════════════════════════════════════════
// Resolução de alvos (seguros após undo: resolvem pelo id atual do projeto)
// ═══════════════════════════════════════════════════════════════════════

Clip* ClipPropertiesWidget::clipOf(const QString& id) const {
    if (!m_project || id.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    for (Track& t : m_project->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    return nullptr;
}

Track* ClipPropertiesWidget::trackOf(const QString& id) const {
    if (!m_project || id.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        if (t.id == id) return &t;
    for (Track& t : m_project->audioTracks)
        if (t.id == id) return &t;
    return nullptr;
}

Track* ClipPropertiesWidget::trackOfClip(Clip* c) const {
    if (!m_project || !c) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& x : t.clips)
            if (x.id == c->id) return &t;
    for (Track& t : m_project->audioTracks)
        for (Clip& x : t.clips)
            if (x.id == c->id) return &t;
    return nullptr;
}

// Camada da Mesa em edição: camada explícita selecionada no canvas, ou a
// track do clipe selecionado na timeline.
Track* ClipPropertiesWidget::ctxMesaTrack() const {
    if (Track* t = trackOf(m_trackId)) return t;
    if (Clip* c = clipOf(m_clipId)) return trackOfClip(c);
    return nullptr;
}

MesaComposition* ClipPropertiesWidget::ctxMesa() const {
    if (!m_project) return nullptr;
    if (!m_mesaId.isEmpty())
        if (MesaComposition* m = m_project->findMesa(m_mesaId)) return m;
    if (Track* t = ctxMesaTrack())
        for (MesaComposition& m : m_project->mesas)
            if (m.trackIds.contains(t->id)) return &m;
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// Undo / notificação
// ═══════════════════════════════════════════════════════════════════════

void ClipPropertiesWidget::beginEdit() {
    if (!m_undoPushed) {
        emit editStart();   // MainWindow::pushUndo (snapshot do projeto)
        m_undoPushed = true;
    }
    m_undoTimer->start();
}

void ClipPropertiesWidget::emitEdited() {
    emit modified();
}

// ═══════════════════════════════════════════════════════════════════════
// Rebuild das seções (Effect Controls)
// ═══════════════════════════════════════════════════════════════════════

void ClipPropertiesWidget::clearBody() {
    m_spinRows.clear();
    m_checkRows.clear();
    m_comboRows.clear();
    auto* lay = m_body->layout();
    if (!lay) return;
    while (QLayoutItem* item = lay->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
}

QDoubleSpinBox* ClipPropertiesWidget::addSpinRow(
        const QString& label, double lo, double hi, double step, int dec, int minW,
        double dim, std::function<double()> read, std::function<void(double)> write,
        std::function<QVector<Keyframe>*()> keys) {
    auto* spin = new QDoubleSpinBox(m_body);
    spin->setRange(lo, hi);
    spin->setSingleStep(step);
    spin->setDecimals(dec);
    spin->setMinimumWidth(minW);
    spin->setKeyboardTracking(false);
    spin->setAlignment(Qt::AlignRight);
    spin->setStyleSheet(QStringLiteral(
        "QDoubleSpinBox { background:%1; color:%2; border:1px solid %3;"
        " border-radius:3px; padding:1px 4px; font-size:11px; }"
        "QDoubleSpinBox:focus { border:1px solid %4; }")
        .arg(themeColors().inputBg.name(), themeColors().spinText.name(),
             themeColors().inputBorder.name(), themeColors().accent.name()));

    QWidget* row = new QWidget(m_body);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(8, 0, 4, 0);
    h->setSpacing(4);
    auto* name = new QLabel(label, row);
    name->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                        .arg(themeColors().text.name()));
    h->addWidget(name, 1);
    h->addWidget(spin, 0);

    QToolButton* sw = nullptr;
    if (keys) {
        sw = new QToolButton(row);
        sw->setAutoRaise(true);
        sw->setCheckable(true);
        sw->setIcon(stopwatchIcon(false));
        sw->setToolTip(tr("Alternar keyframe no playhead"));
        sw->setCursor(Qt::PointingHandCursor);
        sw->setStyleSheet(QStringLiteral("QToolButton { background:transparent;"
                                         " border:none; }"));
        h->addWidget(sw, 0);
        connect(sw, &QToolButton::clicked, this,
                [this, read, keys]() {
                    QVector<Keyframe>* ks = keys();
                    if (!ks) return;
                    beginEdit();
                    const int i = keyframeAt(*ks, m_playhead);
                    if (i >= 0)
                        ks->removeAt(i);
                    else
                        upsertKeyframe(*ks, m_playhead, read());
                    refreshValues();
                    refreshStopwatches();
                    emitEdited();
                });
    }

    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, write = std::move(write), dim](double v) {
                if (m_creating || m_syncing) return;
                beginEdit();
                write(v / dim);
                refreshStopwatches();
                emitEdited();
            });

    m_spinRows.append({ spin, sw, std::move(read), std::move(write),
                        std::move(keys), dim });
    m_body->layout()->addWidget(row);
    return spin;
}

QCheckBox* ClipPropertiesWidget::addCheckRow(
        const QString& label, const QString& tip,
        std::function<bool()> read, std::function<void(bool)> write) {
    auto* box = new QCheckBox(label, m_body);
    box->setToolTip(tip);
    box->setCursor(Qt::PointingHandCursor);
    box->setStyleSheet(QStringLiteral(
        "QCheckBox { color:%1; font-size:11px; padding:2px 8px; }")
        .arg(themeColors().text.name()));
    connect(box, &QCheckBox::toggled, this,
            [this, write = std::move(write)](bool on) {
                if (m_creating || m_syncing) return;
                beginEdit();
                write(on);
                emitEdited();
            });
    m_checkRows.append({ box, std::move(read) });
    m_body->layout()->addWidget(box);
    return box;
}

QComboBox* ClipPropertiesWidget::addComboRow(
        const QString& label, const QStringList& items, const QStringList& values,
        std::function<QString()> read, std::function<void(const QString&)> write) {
    auto* row = new QWidget(m_body);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(8, 0, 8, 0);
    auto* name = new QLabel(label, row);
    name->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                        .arg(themeColors().text.name()));
    h->addWidget(name, 1);
    auto* box = new QComboBox(row);
    box->addItems(items);
    box->setStyleSheet(QStringLiteral(
        "QComboBox { background:%1; color:%2; border:1px solid %3;"
        " border-radius:3px; padding:1px 4px; font-size:11px; }")
        .arg(themeColors().inputBg.name(), themeColors().text.name(),
             themeColors().inputBorder.name()));
    h->addWidget(box, 0);
    connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, values, write = std::move(write)](int idx) {
                if (m_creating || m_syncing) return;
                if (idx < 0 || idx >= values.size()) return;
                beginEdit();
                write(values[idx]);
                emitEdited();
            });
    m_comboRows.append({ box, std::move(read), values });
    m_body->layout()->addWidget(row);
    return box;
}

void ClipPropertiesWidget::addActionRow(
        const QString& label, const QString& text, const QString& tip,
        std::function<void()> onClick) {
    auto* row = new QWidget(m_body);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(8, 0, 8, 0);
    auto* name = new QLabel(label, row);
    name->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                        .arg(themeColors().text.name()));
    h->addWidget(name, 1);
    auto* btn = new QPushButton(text, row);
    btn->setToolTip(tip);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(QStringLiteral(
        "QPushButton { background:%1; color:%2; border:1px solid %3;"
        " border-radius:3px; padding:2px 10px; font-size:11px; }"
        "QPushButton:hover { background:%4; }")
        .arg(themeColors().inputBg.name(), themeColors().text.name(),
             themeColors().inputBorder.name(), themeColors().btnHover.name()));
    h->addWidget(btn, 0);
    connect(btn, &QPushButton::clicked, this, [this, onClick = std::move(onClick)]() {
        onClick();
    });
    m_body->layout()->addWidget(row);
}

// Título de seção estilo Effect Controls (maiúsculas, cinza, linha abaixo).
void ClipPropertiesWidget::addSectionTitle(const QString& title) {
    auto* lbl = new QLabel(title, m_body);
    lbl->setStyleSheet(QStringLiteral(
        "font-size:10px; font-weight:bold; color:%1; padding:8px 8px 2px 8px;"
        " border-bottom:1px solid %2;")
        .arg(themeColors().iconMuted.name(), themeColors().inputBorder.name()));
    m_body->layout()->addWidget(lbl);
}

void ClipPropertiesWidget::rebuild() {
    m_creating = true;
    m_undoPushed = false;
    clearBody();

    auto* lay = m_body->layout();

    Clip* clip = clipOf(m_clipId);
    Track* mTrack = ctxMesaTrack();
    MesaComposition* mc = ctxMesa();

    const bool audio = clip && mTrack && mTrack->audio;
    // Camada da Mesa em edição: track selecionada no canvas OU track do clipe
    // selecionado na timeline, desde que pertença a uma composição.
    const bool isMesaTrack = mTrack && mc && mc->trackIds.contains(mTrack->id);

    // ── Cabeçalho (nome do alvo) ──────────────────────────────────────
    m_header = new QLabel(m_body);
    m_header->setStyleSheet(QStringLiteral(
        "font-size:13px; font-weight:bold; color:%1; padding:8px 10px 6px 10px;")
        .arg(themeColors().text.name()));
    if (clip) m_header->setText(clip->name);
    else if (mTrack) m_header->setText(tr("Camada: %1").arg(mTrack->name));
    else if (mc) m_header->setText(tr("Câmera — %1").arg(mc->name));
    lay->addWidget(m_header);

    // Nada selecionado.
    if (!clip && !mTrack && !mc) {
        auto* hint = new QLabel(tr("Selecione um clipe na timeline\n"
                                   "ou uma camada no canvas da Mesa."), m_body);
        hint->setAlignment(Qt::AlignCenter);
        hint->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
                            .arg(themeColors().iconMuted.name()));
        lay->addWidget(hint);
        m_creating = false;
        return;
    }

    // ═══ 1. Clipe (normal) ═══════════════════════════════════════════
    if (clip) {
        addSectionTitle(tr("CLIPE"));
        if (audio) {
            addSpinRow(tr("Volume (%):"), 0, 400, 1, 0, 60, 100.0,
                [this, clip]() { return kfValue(clip->kfVolume, clip->volume, m_playhead); },
                [this, clip](double v) {
                    if (!clip->kfVolume.isEmpty())
                        upsertKeyframe(clip->kfVolume, m_playhead, v);
                    else clip->volume = v;
                },
                [this, clip]() { return &clip->kfVolume; });
        } else {
            addSpinRow(tr("Velocidade (×):"), 0.1, 4.0, 0.05, 2, 60, 1.0,
                [clip]() { return clip->speed; },
                [clip](double v) { clip->speed = v; });
            addSpinRow(tr("Opacidade (%):"), 0, 300, 1, 1, 60, 100.0,
                [this, clip]() { return kfValue(clip->kfOpacity, clip->opacity, m_playhead); },
                [this, clip](double v) {
                    if (!clip->kfOpacity.isEmpty())
                        upsertKeyframe(clip->kfOpacity, m_playhead, v);
                    else clip->opacity = v;
                },
                [this, clip]() { return &clip->kfOpacity; });
            addSpinRow(tr("Volume (%):"), 0, 400, 1, 0, 60, 100.0,
                [this, clip]() { return kfValue(clip->kfVolume, clip->volume, m_playhead); },
                [this, clip](double v) {
                    if (!clip->kfVolume.isEmpty())
                        upsertKeyframe(clip->kfVolume, m_playhead, v);
                    else clip->volume = v;
                },
                [this, clip]() { return &clip->kfVolume; });
            addSpinRow(tr("Fade in (s):"), 0, 10, 0.1, 2, 60, 1.0,
                [clip]() { return clip->fadeIn; },
                [clip](double v) { clip->fadeIn = v; });
            addSpinRow(tr("Fade out (s):"), 0, 10, 0.1, 2, 60, 1.0,
                [clip]() { return clip->fadeOut; },
                [clip](double v) { clip->fadeOut = v; });
            addActionRow(tr("Texto:"), tr("Editar…"),
                tr("Abrir o editor de texto do clipe"),
                [this, clip]() {
                    TextEditorDialog dlg(m_project, clip, this);
                    if (dlg.exec() == QDialog::Accepted) {
                        beginEdit();
                        emitEdited();
                    }
                });
        }
    }

    // ═══ 2. Motion (AE) — transform do clipe de vídeo ═════════════════
    if (clip && !audio) {
        addSectionTitle(tr("MOTION"));
        addSpinRow(tr("Posição X (px):"), -8000, 8000, 1, 1, 70, 1.0,
            [this, clip]() { return kfValue(clip->kfTx, clip->tx, m_playhead); },
            [this, clip](double v) {
                if (!clip->kfTx.isEmpty()) upsertKeyframe(clip->kfTx, m_playhead, v);
                else clip->tx = v;
            },
            [this, clip]() { return &clip->kfTx; });
        addSpinRow(tr("Posição Y (px):"), -8000, 8000, 1, 1, 70, 1.0,
            [this, clip]() { return kfValue(clip->kfTy, clip->ty, m_playhead); },
            [this, clip](double v) {
                if (!clip->kfTy.isEmpty()) upsertKeyframe(clip->kfTy, m_playhead, v);
                else clip->ty = v;
            },
            [this, clip]() { return &clip->kfTy; });
        addSpinRow(tr("Escala X (%):"), 0, 2000, 1, 0, 70, 100.0,
            [this, clip]() { return kfValue(clip->kfScaleX, clip->scaleX, m_playhead); },
            [this, clip](double v) {
                if (!clip->kfScaleX.isEmpty())
                    upsertKeyframe(clip->kfScaleX, m_playhead, v);
                else clip->scaleX = v;
            },
            [this, clip]() { return &clip->kfScaleX; });
        addSpinRow(tr("Escala Y (%):"), 0, 2000, 1, 0, 70, 100.0,
            [this, clip]() { return kfValue(clip->kfScaleY, clip->scaleY, m_playhead); },
            [this, clip](double v) {
                if (!clip->kfScaleY.isEmpty())
                    upsertKeyframe(clip->kfScaleY, m_playhead, v);
                else clip->scaleY = v;
            },
            [this, clip]() { return &clip->kfScaleY; });
        addSpinRow(tr("Rotação (°):"), -720, 720, 1, 1, 70, 1.0,
            [this, clip]() { return kfValue(clip->kfRotation, clip->rotation, m_playhead); },
            [this, clip](double v) {
                if (!clip->kfRotation.isEmpty())
                    upsertKeyframe(clip->kfRotation, m_playhead, v);
                else clip->rotation = v;
            },
            [this, clip]() { return &clip->kfRotation; });
        addSpinRow(tr("Âncora X:"), -4, 4, 0.01, 2, 70, 1.0,
            [this, clip]() { return kfValue(clip->kfAnchorX, clip->anchorX, m_playhead); },
            [this, clip](double v) {
                if (!clip->kfAnchorX.isEmpty())
                    upsertKeyframe(clip->kfAnchorX, m_playhead, v);
                else clip->anchorX = v;
            },
            [this, clip]() { return &clip->kfAnchorX; });
        addSpinRow(tr("Âncora Y:"), -4, 4, 0.01, 2, 70, 1.0,
            [this, clip]() { return kfValue(clip->kfAnchorY, clip->anchorY, m_playhead); },
            [this, clip](double v) {
                if (!clip->kfAnchorY.isEmpty())
                    upsertKeyframe(clip->kfAnchorY, m_playhead, v);
                else clip->anchorY = v;
            },
            [this, clip]() { return &clip->kfAnchorY; });
    }

    // ═══ 3. Motion Blur (MotiOn) do clipe ════════════════════════════
    if (clip && !audio) {
        addSectionTitle(tr("MOTION BLUR (MOTION)"));
        addCheckRow(tr("Motion blur"),
            tr("Borra o movimento do clipe por amostragem temporal (MotiOn)."),
            [clip]() { return clip->motionEnabled; },
            [this, clip](bool on) {
                beginEdit();
                clip->motionEnabled = on;
                emitEdited();
            });
        addSpinRow(tr("Intensidade (%):"), 0, 100, 1, 1, 60, 1.0,
            [clip]() { return clip->motionAmount; },
            [clip](double v) { clip->motionAmount = v; });
        addSpinRow(tr("Ângulo (°):"), 0, 360, 1, 1, 60, 1.0,
            [clip]() { return clip->motionAngle; },
            [clip](double v) { clip->motionAngle = v; });
        addSpinRow(tr("Amostras:"), 1, 32, 1, 0, 60, 1.0,
            [clip]() { return (double)clip->motionSamples; },
            [clip](double v) { clip->motionSamples = qBound(1, (int)llround(v), 32); });
    }

    // ═══ 4. Cor / Efeitos ════════════════════════════════════════════
    if (clip && !audio) {
        addSectionTitle(tr("COR / EFEITOS"));
        addSpinRow(tr("Brilho:"), -100, 100, 1, 0, 60, 100.0,
            [clip]() { return clip->brightness; },
            [clip](double v) { clip->brightness = v; });
        addSpinRow(tr("Contraste (%):"), 0, 200, 1, 0, 60, 100.0,
            [clip]() { return clip->contrast; },
            [clip](double v) { clip->contrast = v; });
        addSpinRow(tr("Saturação (%):"), 0, 200, 1, 0, 60, 100.0,
            [clip]() { return clip->saturation; },
            [clip](double v) { clip->saturation = v; });
        addSpinRow(tr("Desfoque (%):"), 0, 100, 1, 1, 60, 1.0,
            [clip]() { return clip->blur; },
            [clip](double v) { clip->blur = v; });
        addCheckRow(tr("Preto e branco"),
            tr("Converte o clipe para escala de cinza."),
            [clip]() { return clip->grayscale; },
            [this, clip](bool on) {
                beginEdit();
                clip->grayscale = on;
                emitEdited();
            });
        addCheckRow(tr("Chroma key"),
            tr("Remove o fundo da cor selecionada no preview."),
            [clip]() { return clip->chromaKey; },
            [this, clip](bool on) {
                beginEdit();
                clip->chromaKey = on;
                emitEdited();
            });
    }

    // ═══ 5. Motion / Camada da MESA ══════════════════════════════════
    if (isMesaTrack) {
        addSectionTitle(tr("MOTION (MESA)"));
        addSpinRow(tr("Posição X (px):"), -8000, 8000, 1, 1, 70, 1.0,
            [this, mTrack]() { return kfValue(mTrack->kfMesaX, mTrack->mesaX, m_playhead); },
            [this, mTrack](double v) {
                if (!mTrack->kfMesaX.isEmpty())
                    upsertKeyframe(mTrack->kfMesaX, m_playhead, v);
                else mTrack->mesaX = v;
            },
            [this, mTrack]() { return &mTrack->kfMesaX; });
        addSpinRow(tr("Posição Y (px):"), -8000, 8000, 1, 1, 70, 1.0,
            [this, mTrack]() { return kfValue(mTrack->kfMesaY, mTrack->mesaY, m_playhead); },
            [this, mTrack](double v) {
                if (!mTrack->kfMesaY.isEmpty())
                    upsertKeyframe(mTrack->kfMesaY, m_playhead, v);
                else mTrack->mesaY = v;
            },
            [this, mTrack]() { return &mTrack->kfMesaY; });
        addSpinRow(tr("Escala X (%):"), 0, 2000, 1, 0, 70, 100.0,
            [this, mTrack]() { return kfValue(mTrack->kfMesaScaleX, mTrack->mesaScaleX, m_playhead); },
            [this, mTrack](double v) {
                if (!mTrack->kfMesaScaleX.isEmpty())
                    upsertKeyframe(mTrack->kfMesaScaleX, m_playhead, v);
                else mTrack->mesaScaleX = v;
            },
            [this, mTrack]() { return &mTrack->kfMesaScaleX; });
        addSpinRow(tr("Escala Y (%):"), 0, 2000, 1, 0, 70, 100.0,
            [this, mTrack]() { return kfValue(mTrack->kfMesaScaleY, mTrack->mesaScaleY, m_playhead); },
            [this, mTrack](double v) {
                if (!mTrack->kfMesaScaleY.isEmpty())
                    upsertKeyframe(mTrack->kfMesaScaleY, m_playhead, v);
                else mTrack->mesaScaleY = v;
            },
            [this, mTrack]() { return &mTrack->kfMesaScaleY; });
        addSpinRow(tr("Rotação (°):"), -720, 720, 1, 1, 70, 1.0,
            [this, mTrack]() { return kfValue(mTrack->kfMesaRotation, mTrack->mesaRotation, m_playhead); },
            [this, mTrack](double v) {
                if (!mTrack->kfMesaRotation.isEmpty())
                    upsertKeyframe(mTrack->kfMesaRotation, m_playhead, v);
                else mTrack->mesaRotation = v;
            },
            [this, mTrack]() { return &mTrack->kfMesaRotation; });
        addSpinRow(tr("Opacidade (%):"), 0, 100, 1, 0, 70, 100.0,
            [this, mTrack]() { return kfValue(mTrack->kfMesaOpacity, mTrack->mesaOpacity, m_playhead); },
            [this, mTrack](double v) {
                if (!mTrack->kfMesaOpacity.isEmpty())
                    upsertKeyframe(mTrack->kfMesaOpacity, m_playhead, v);
                else mTrack->mesaOpacity = v;
            },
            [this, mTrack]() { return &mTrack->kfMesaOpacity; });
        addSpinRow(tr("Âncora X (px):"), -8000, 8000, 1, 1, 70, 1.0,
            [this, mTrack]() { return kfValue(mTrack->kfMesaAnchorX, mTrack->mesaAnchorX, m_playhead); },
            [this, mTrack](double v) {
                if (!mTrack->kfMesaAnchorX.isEmpty())
                    upsertKeyframe(mTrack->kfMesaAnchorX, m_playhead, v);
                else mTrack->mesaAnchorX = v;
            },
            [this, mTrack]() { return &mTrack->kfMesaAnchorX; });
        addSpinRow(tr("Âncora Y (px):"), -8000, 8000, 1, 1, 70, 1.0,
            [this, mTrack]() { return kfValue(mTrack->kfMesaAnchorY, mTrack->mesaAnchorY, m_playhead); },
            [this, mTrack](double v) {
                if (!mTrack->kfMesaAnchorY.isEmpty())
                    upsertKeyframe(mTrack->kfMesaAnchorY, m_playhead, v);
                else mTrack->mesaAnchorY = v;
            },
            [this, mTrack]() { return &mTrack->kfMesaAnchorY; });

        addSectionTitle(tr("CAMADA (MESA)"));
        const QStringList blendItems = { tr("Normal"), tr("Adição (add)"),
            tr("Multiplicar"), tr("Tela (screen)"), tr("Sobrepor (overlay)"),
            tr("Luz suave"), tr("Diferença") };
        const QStringList blendValues = { QStringLiteral("normal"),
            QStringLiteral("add"), QStringLiteral("multiply"),
            QStringLiteral("screen"), QStringLiteral("overlay"),
            QStringLiteral("softlight"), QStringLiteral("difference") };
        addComboRow(tr("Blend:"), blendItems, blendValues,
            [mTrack]() { return mTrack->blendMode; },
            [this, mTrack](const QString& v) {
                beginEdit();
                mTrack->blendMode = v;
                emitEdited();
            });
        addCheckRow(tr("Motion blur (allow)"),
            tr("Permite que esta camada borre nas sub-passadas do motion blur "
               "global (Ctrl+Shift+B). Desligue para a camada ficar fixa."),
            [mTrack]() { return mTrack->mesaMotionBlur; },
            [this, mTrack](bool on) {
                beginEdit();
                mTrack->mesaMotionBlur = on;
                emitEdited();
            });
    }

    // ═══ 6. Câmera + Composição (blur global) ════════════════════════
    if (mc) {
        addSectionTitle(tr("CÂMERA"));
        addSpinRow(tr("Posição X (px):"), -8000, 8000, 1, 1, 70, 1.0,
            [this, mc]() { return kfValue(mc->kfCamX, mc->camX, m_playhead); },
            [this, mc](double v) {
                if (!mc->kfCamX.isEmpty()) upsertKeyframe(mc->kfCamX, m_playhead, v);
                else mc->camX = v;
            },
            [this, mc]() { return &mc->kfCamX; });
        addSpinRow(tr("Posição Y (px):"), -8000, 8000, 1, 1, 70, 1.0,
            [this, mc]() { return kfValue(mc->kfCamY, mc->camY, m_playhead); },
            [this, mc](double v) {
                if (!mc->kfCamY.isEmpty()) upsertKeyframe(mc->kfCamY, m_playhead, v);
                else mc->camY = v;
            },
            [this, mc]() { return &mc->kfCamY; });
        addSpinRow(tr("Zoom:"), 0.05, 20.0, 0.05, 2, 70, 1.0,
            [this, mc]() { return kfValue(mc->kfCamZoom, mc->camZoom, m_playhead); },
            [this, mc](double v) {
                if (!mc->kfCamZoom.isEmpty()) upsertKeyframe(mc->kfCamZoom, m_playhead, v);
                else mc->camZoom = v;
            },
            [this, mc]() { return &mc->kfCamZoom; });
        addSpinRow(tr("Rotação (°):"), -720, 720, 1, 1, 70, 1.0,
            [this, mc]() { return kfValue(mc->kfCamRotation, mc->camRotation, m_playhead); },
            [this, mc](double v) {
                if (!mc->kfCamRotation.isEmpty())
                    upsertKeyframe(mc->kfCamRotation, m_playhead, v);
                else mc->camRotation = v;
            },
            [this, mc]() { return &mc->kfCamRotation; });

        addSectionTitle(tr("COMPOSIÇÃO (BLUR GLOBAL)"));
        addCheckRow(tr("Motion blur global"),
            tr("Borra o quadro inteiro por amostragem temporal. Cada camada "
               "precisa do flag 'Motion blur (allow)'."),
            [mc]() { return mc->motionBlur; },
            [this, mc](bool on) {
                beginEdit();
                mc->motionBlur = on;
                emitEdited();
            });
        addSpinRow(tr("Amostras:"), 2, 32, 1, 0, 50, 1.0,
            [mc]() { return (double)mc->motionBlurSamples; },
            [mc](double v) { mc->motionBlurSamples = qBound(2, (int)llround(v), 32); });
        addSpinRow(tr("Obturador (%):"), 5, 100, 1, 0, 50, 100.0,
            [mc]() { return mc->motionBlurShutter; },
            [mc](double v) { mc->motionBlurShutter = qBound(0.05, v, 1.0); });
    }

    m_creating = false;
    refreshValues();
    refreshStopwatches();
}

void ClipPropertiesWidget::refreshValues() {
    m_syncing = true;
    for (const SpinRow& r : m_spinRows) {
        if (!r.spin) continue;
        const QSignalBlocker b(r.spin);
        r.spin->setValue(r.read() * r.dim);
    }
    m_syncing = false;
}

void ClipPropertiesWidget::refreshStopwatches() {
    for (const SpinRow& r : m_spinRows) {
        if (!r.sw || !r.keys || !r.keys()) continue;
        const bool on = keyframeAt(*r.keys(), m_playhead) >= 0;
        const QSignalBlocker b(r.sw);
        r.sw->setChecked(on);
        r.sw->setIcon(stopwatchIcon(on));
    }
    for (const CheckRow& r : m_checkRows) {
        if (!r.box) continue;
        const QSignalBlocker b(r.box);
        r.box->setChecked(r.read());
    }
    for (const ComboRow& r : m_comboRows) {
        if (!r.box) continue;
        const int idx = r.values.indexOf(r.read());
        const QSignalBlocker b(r.box);
        r.box->setCurrentIndex(qMax(0, idx));
    }
}