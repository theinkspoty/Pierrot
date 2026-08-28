// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ExpressWidget.h"
#include "EffectsWidget.h"
#include "models/Project.h"
#include "ui/Theme.h"

#include <ofxParam.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QScrollArea>
#include <QLabel>
#include <QSlider>
#include <QCheckBox>
#include <QPushButton>
#include <QFormLayout>
#include <QFrame>
#include <QComboBox>
#include <QColorDialog>
#include <QFont>
#include <QDragEnterEvent>
#include <QMimeData>
#include <cmath>
#include <functional>
#include <algorithm>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>

// ── Construtor ───────────────────────────────────────────────────────────

ExpressWidget::ExpressWidget(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_tabs = new QTabWidget(this);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { background:%1; border:none; }"
        "QTabBar { background:%2; }"
        "QTabBar::tab { background:%3; color:%4; "
        "  padding:6px 14px; border:none; border-bottom:2px solid transparent; }"
        "QTabBar::tab:selected { color:%5; border-bottom:2px solid %6; }"
        "QTabBar::tab:hover { color:%7; background:%8; }"
        "QTabBar::close-button { image: none; subcontrol-position: right; "
        "  subcontrol-origin: padding; }")
        .arg(themeColors().expressBg.name())
        .arg(themeColors().tabBg.name())
        .arg(themeColors().expressBg.name())
        .arg(themeColors().expressDescText.name())
        .arg(themeColors().text.name())
        .arg(themeColors().accent.name())
        .arg(themeColors().text.name())
        .arg(themeColors().btnHover.name()));
    lay->addWidget(m_tabs);

    m_emptyLabel = new QLabel(tr("Arraste efeitos da lista para cá\nou selecione um clipe na timeline"), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setStyleSheet(QStringLiteral("color:%1; font-size:12px; background:%2;")
        .arg(themeColors().expressDescText.name())
        .arg(themeColors().expressBg.name()));
    lay->addWidget(m_emptyLabel);
    m_emptyLabel->show();
    m_tabs->hide();

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &ExpressWidget::onTabCloseRequested);

    setAcceptDrops(true);
    setStyleSheet(QStringLiteral("background:%1;").arg(themeColors().expressBg.name()));
}

// ── Plugins OFX ──────────────────────────────────────────────────────────

void ExpressWidget::setOfxPlugins(const QVector<OfxPluginInfo>& plugins)
{
    m_ofxPlugins = plugins;
}

void ExpressWidget::setOfxParamDefs(const QString& pluginId,
                                     const QVector<QPair<QString,QPair<QString,QString>>>& params)
{
    m_ofxParamDefs[pluginId] = params;
}

// ── Seleção de clipe ─────────────────────────────────────────────────────

void ExpressWidget::setSelectedClip(Clip* clip)
{
    m_currentClip = clip;
    rebuildTabs();
}

// ── Reconstroi abas ao mudar de clipe ────────────────────────────────────

void ExpressWidget::rebuildTabs()
{
    m_tabs->clear();
    m_tabMap.clear();
    m_tabPages.clear();
    m_appliedBuiltIn.clear();

    if (!m_currentClip) {
        m_tabs->hide();
        m_emptyLabel->show();
        return;
    }

    // Abas de efeitos nativos já aplicados.
    if (m_currentClip->lainkaEnabled)     createBuiltInTab("pierrot_lainka");
    if (m_currentClip->motionEnabled)     createBuiltInTab("pierrot_motion");
    if (m_currentClip->grayscale)         createBuiltInTab("pierrot_grayscale");
    if (m_currentClip->chromaKey)         createBuiltInTab("pierrot_chromakey");
    if (m_currentClip->brightness != 0.0) createBuiltInTab("pierrot_brightness");
    if (m_currentClip->contrast != 1.0)   createBuiltInTab("pierrot_contrast");
    if (m_currentClip->saturation != 1.0) createBuiltInTab("pierrot_saturation");
    if (m_currentClip->blur != 0.0)       createBuiltInTab("pierrot_blur");

    // Efeitos de áudio (abas próprias no Express).
    if (m_currentClip->eqLow != 0.0 || m_currentClip->eqMid != 0.0
        || m_currentClip->eqHigh != 0.0)
        createBuiltInTab("pierrot_audio_eq");
    if (m_currentClip->reverb)         createBuiltInTab("pierrot_audio_reverb");

    // Abas de plugins OFX.
    for (const OfxPluginInstance& fx : m_currentClip->ofxFx)
        createOfxTab(fx.pluginId);

    if (m_tabs->count() > 0) {
        m_emptyLabel->hide();
        m_tabs->show();
    } else {
        m_tabs->hide();
        m_emptyLabel->show();
    }
}

// ── Adiciona efeito (chamado pelo drag-and-drop) ─────────────────────────

void ExpressWidget::addEffect(const QString& effectId)
{
    if (!m_currentClip) return;

    // Se já está aberto como aba, só ativa.
    if (m_tabMap.contains(effectId)) {
        m_tabs->setCurrentIndex(m_tabMap[effectId]);
        return;
    }

    // Aplica o efeito ao clipe se for nativo e ainda não aplicado.
    if (effectId == "pierrot_lainka" && !m_currentClip->lainkaEnabled) {
        m_currentClip->lainkaEnabled = true;
        m_currentClip->lainkaTargetFps = 6;
        m_currentClip->lainkaJitterPos = 10.0;
        m_currentClip->lainkaFlicker = 8.0;
        m_currentClip->lainkaWarpAmount = 8.0;
        m_currentClip->lainkaDustAmount = 0.0;
        m_currentClip->lainkaScratchAmount = 0.0;
        m_currentClip->lainkaOpacity = 100.0;
        emit modified();
    } else if (effectId == "pierrot_motion" && !m_currentClip->motionEnabled) {
        m_currentClip->motionEnabled = true;
        emit modified();
    } else if (effectId == "pierrot_grayscale" && !m_currentClip->grayscale) {
        m_currentClip->grayscale = true;
        emit modified();
    } else if (effectId == "pierrot_chromakey" && !m_currentClip->chromaKey) {
        m_currentClip->chromaKey = true;
        emit modified();
    } else if (effectId == "pierrot_brightness" && m_currentClip->brightness == 0.0) {
        m_currentClip->brightness = 0.2;
        emit modified();
    } else if (effectId == "pierrot_contrast" && m_currentClip->contrast == 1.0) {
        m_currentClip->contrast = 1.2;
        emit modified();
    } else if (effectId == "pierrot_saturation" && m_currentClip->saturation == 1.0) {
        m_currentClip->saturation = 1.2;
        emit modified();
    } else if (effectId == "pierrot_blur" && m_currentClip->blur == 0.0) {
        m_currentClip->blur = 5.0;
        emit modified();
    } else if (effectId == "pierrot_audio_eq") {
        if (std::fabs(m_currentClip->eqLow) <= 0.01
            && std::fabs(m_currentClip->eqMid) <= 0.01
            && std::fabs(m_currentClip->eqHigh) <= 0.01) {
            // EQ Express: preset inicial sutil (presença nos médios/agudos).
            m_currentClip->eqLow = 0.0;
            m_currentClip->eqMid = 1.5;
            m_currentClip->eqHigh = 1.0;
            emit modified();
        }
    } else if (effectId == "pierrot_audio_reverb" && !m_currentClip->reverb) {
        m_currentClip->reverb = true;
        m_currentClip->reverbMix = 0.35;
        m_currentClip->reverbSize = 0.5;
        emit modified();
    } else if (!effectId.startsWith("pierrot_")) {
        // OFX: adiciona ao clipe se não existe.
        bool found = false;
        for (const OfxPluginInstance& fx : m_currentClip->ofxFx)
            if (fx.pluginId == effectId) { found = true; break; }
        if (!found) {
            OfxPluginInstance fx;
            fx.pluginId = effectId;
            fx.enabled = true;
            m_currentClip->ofxFx.append(fx);
            emit modified();
        }
    }

    // Cria a aba.
    if (effectId.startsWith("pierrot_"))
        createBuiltInTab(effectId);
    else
        createOfxTab(effectId);

    m_emptyLabel->hide();
    m_tabs->show();
}

// ── Remove efeito do clipe ───────────────────────────────────────────────

void ExpressWidget::removeEffectFromClip(const QString& effectId)
{
    if (!m_currentClip) return;

    if (effectId == "pierrot_lainka")          m_currentClip->lainkaEnabled = false;
    else if (effectId == "pierrot_motion")     m_currentClip->motionEnabled = false;
    else if (effectId == "pierrot_grayscale")  m_currentClip->grayscale = false;
    else if (effectId == "pierrot_chromakey")  m_currentClip->chromaKey = false;
    else if (effectId == "pierrot_brightness") m_currentClip->brightness = 0.0;
    else if (effectId == "pierrot_contrast")   m_currentClip->contrast = 1.0;
    else if (effectId == "pierrot_saturation") m_currentClip->saturation = 1.0;
    else if (effectId == "pierrot_blur")       m_currentClip->blur = 0.0;
    else if (effectId == "pierrot_audio_eq")   m_currentClip->eqLow = m_currentClip->eqMid = m_currentClip->eqHigh = 0.0;
    else if (effectId == "pierrot_audio_reverb") m_currentClip->reverb = false;
    else {
        // OFX: remove do clipe.
        for (int i = 0; i < m_currentClip->ofxFx.size(); ++i) {
            if (m_currentClip->ofxFx[i].pluginId == effectId) {
                m_currentClip->ofxFx.removeAt(i);
                break;
            }
        }
    }
    emit modified();
}

// ── Fechar aba ───────────────────────────────────────────────────────────

void ExpressWidget::onTabCloseRequested(int index)
{
    if (index < 0 || index >= m_tabs->count()) return;

    // Encontra o effectId pelo índice no m_tabMap reverso.
    QString effectId;
    for (auto it = m_tabMap.constBegin(); it != m_tabMap.constEnd(); ++it)
        if (it.value() == index) { effectId = it.key(); break; }
    if (effectId.isEmpty()) return;

    removeEffectFromClip(effectId);
    m_tabMap.remove(effectId);
    m_tabPages.remove(effectId);
    m_appliedBuiltIn.remove(effectId);
    m_tabs->removeTab(index);

    // Reconstrói o mapa de índices.
    m_tabMap.clear();
    for (int i = 0; i < m_tabs->count(); ++i) {
        // Usa o widget da aba para encontrar o effectId.
        for (auto it = m_tabPages.constBegin(); it != m_tabPages.constEnd(); ++it) {
            if (m_tabs->widget(i) == it.value()) {
                m_tabMap.insert(it.key(), i);
                break;
            }
        }
    }

    if (m_tabs->count() == 0) {
        m_tabs->hide();
        m_emptyLabel->show();
    }
}

// ── Helpers ──────────────────────────────────────────────────────────────

static const QHash<QString, QString>& effectDisplayNames()
{
    static const QHash<QString, QString> names = {
        { QStringLiteral("pierrot_brightness"), QStringLiteral("Brilho") },
        { QStringLiteral("pierrot_contrast"),   QStringLiteral("Contraste") },
        { QStringLiteral("pierrot_saturation"), QStringLiteral("Saturação") },
        { QStringLiteral("pierrot_blur"),       QStringLiteral("Desfoque") },
        { QStringLiteral("pierrot_grayscale"),  QStringLiteral("Preto e Branco") },
        { QStringLiteral("pierrot_chromakey"),  QStringLiteral("Chroma Key") },
        { QStringLiteral("pierrot_lainka"),     QStringLiteral("LAINKA") },
        { QStringLiteral("pierrot_motion"),     QStringLiteral("MotiOn") },
        { QStringLiteral("pierrot_audio_eq"),   QStringLiteral("EQ Express") },
        { QStringLiteral("pierrot_audio_reverb"), QStringLiteral("Reverb EX") },
    };
    return names;
}

// ── Aba de efeito nativo ─────────────────────────────────────────────────

void ExpressWidget::createBuiltInTab(const QString& effectId)
{
    if (m_tabMap.contains(effectId)) {
        m_tabs->setCurrentIndex(m_tabMap[effectId]);
        return;
    }
    m_appliedBuiltIn.insert(effectId);

    Clip* c = m_currentClip;

    auto* inner = new QWidget;
    auto* form = new QFormLayout(inner);
    form->setContentsMargins(16, 12, 16, 12);
    form->setSpacing(8);

    if (effectId == "pierrot_brightness") {
        auto* sl = new QSlider(Qt::Horizontal);
        sl->setRange(-100, 100);
        sl->setValue(c ? (int)llround(c->brightness * 100.0) : 0);
        auto* lbl = new QLabel(QString::number(sl->value()));
        connect(sl, &QSlider::valueChanged, lbl, [lbl](int v) { lbl->setText(QString::number(v)); });
        connect(sl, &QSlider::valueChanged, this, [this](int v) { applyBuiltInValue("brightness", v / 100.0); });
        auto* row = new QHBoxLayout;
        row->addWidget(sl, 1);
        row->addWidget(lbl);
        form->addRow(tr("Brilho:"), row);
    }
    else if (effectId == "pierrot_contrast") {
        auto* sl = new QSlider(Qt::Horizontal);
        sl->setRange(0, 200);
        sl->setValue(c ? (int)llround(c->contrast * 100.0) : 100);
        auto* lbl = new QLabel(QString("%1%").arg(sl->value()));
        connect(sl, &QSlider::valueChanged, lbl, [lbl](int v) { lbl->setText(QString("%1%").arg(v)); });
        connect(sl, &QSlider::valueChanged, this, [this](int v) { applyBuiltInValue("contrast", v / 100.0); });
        auto* row = new QHBoxLayout;
        row->addWidget(sl, 1);
        row->addWidget(lbl);
        form->addRow(tr("Contraste:"), row);
    }
    else if (effectId == "pierrot_saturation") {
        auto* sl = new QSlider(Qt::Horizontal);
        sl->setRange(0, 200);
        sl->setValue(c ? (int)llround(c->saturation * 100.0) : 100);
        auto* lbl = new QLabel(QString("%1%").arg(sl->value()));
        connect(sl, &QSlider::valueChanged, lbl, [lbl](int v) { lbl->setText(QString("%1%").arg(v)); });
        connect(sl, &QSlider::valueChanged, this, [this](int v) { applyBuiltInValue("saturation", v / 100.0); });
        auto* row = new QHBoxLayout;
        row->addWidget(sl, 1);
        row->addWidget(lbl);
        form->addRow(tr("Saturação:"), row);
    }
    else if (effectId == "pierrot_blur") {
        auto* sl = new QSlider(Qt::Horizontal);
        sl->setRange(0, 40);
        sl->setValue(c ? (int)llround(c->blur) : 0);
        auto* lbl = new QLabel(QString::number(sl->value()));
        connect(sl, &QSlider::valueChanged, lbl, [lbl](int v) { lbl->setText(QString::number(v)); });
        connect(sl, &QSlider::valueChanged, this, [this](int v) { applyBuiltInValue("blur", v); });
        auto* row = new QHBoxLayout;
        row->addWidget(sl, 1);
        row->addWidget(lbl);
        form->addRow(tr("Desfoque:"), row);
    }
    else if (effectId == "pierrot_grayscale") {
        auto* chk = new QCheckBox(tr("Preto e branco"));
        chk->setChecked(c && c->grayscale);
        connect(chk, &QCheckBox::toggled, this, [this](bool v) { applyBuiltInBool("grayscale", v); });
        form->addRow(chk);
    }
    else if (effectId == "pierrot_chromakey") {
        auto* chk = new QCheckBox(tr("Ativar Chroma Key"));
        chk->setChecked(c && c->chromaKey);
        connect(chk, &QCheckBox::toggled, this, [this](bool v) { applyBuiltInBool("chromaKey", v); });
        form->addRow(chk);

        auto* colorBtn = new QPushButton(tr("Cor…"));
        QColor ckColor = c ? c->chromaKeyColor : Qt::green;
        connect(colorBtn, &QPushButton::clicked, this, [this, colorBtn, ckColor]() mutable {
            QColor col = QColorDialog::getColor(ckColor, this, tr("Cor do chroma key"));
            if (col.isValid() && m_currentClip) {
                m_currentClip->chromaKeyColor = col;
                emit modified();
            }
        });
        form->addRow(tr("Cor:"), colorBtn);

        auto* simSl = new QSlider(Qt::Horizontal);
        simSl->setRange(0, 100);
        simSl->setValue(c ? (int)llround(c->chromaKeySimilarity * 100.0) : 15);
        auto* simLbl = new QLabel(QString::number(simSl->value()));
        connect(simSl, &QSlider::valueChanged, simLbl, [simLbl](int v) { simLbl->setText(QString::number(v)); });
        connect(simSl, &QSlider::valueChanged, this, [this](int v) { applyBuiltInValue("chromaKeySimilarity", v / 100.0); });
        auto* simRow = new QHBoxLayout;
        simRow->addWidget(simSl, 1);
        simRow->addWidget(simLbl);
        form->addRow(tr("Similaridade:"), simRow);
    }
    else if (effectId == "pierrot_lainka") {
        auto* chk = new QCheckBox(tr("Ativar LAINKA"));
        chk->setChecked(c && c->lainkaEnabled);
        connect(chk, &QCheckBox::toggled, this, [this](bool v) { applyBuiltInBool("lainkaEnabled", v); });
        form->addRow(chk);

        auto makeSlider = [&](const QString& label, int min, int max, int val,
                              std::function<void(int)> slot, double scale = 1.0, const QString& suffix = QString()) {
            auto* sl = new QSlider(Qt::Horizontal);
            sl->setRange(min, max);
            sl->setValue(val);
            auto* lbl = new QLabel(suffix.isEmpty() ? QString::number(sl->value()) : QString("%1%").arg(sl->value()));
            connect(sl, &QSlider::valueChanged, lbl, [lbl, suffix](int v) {
                lbl->setText(suffix.isEmpty() ? QString::number(v) : QString("%1%").arg(v));
            });
            connect(sl, &QSlider::valueChanged, this, [slot](int v) { slot(v); });
            auto* row = new QHBoxLayout;
            row->addWidget(sl, 1);
            row->addWidget(lbl);
            form->addRow(label, row);
        };

        makeSlider(tr("Target FPS:"), 1, 30, c ? c->lainkaTargetFps : 8,
            [this](int v) { applyBuiltInValue("lainkaTargetFps", v); });
        makeSlider(tr("Tremida:"), 0, 100, c ? (int)llround(c->lainkaJitterPos) : 15,
            [this](int v) { applyBuiltInValue("lainkaJitterPos", v); });
        makeSlider(tr("Flicker:"), 0, 100, c ? (int)llround(c->lainkaFlicker) : 10,
            [this](int v) { applyBuiltInValue("lainkaFlicker", v); }, 1.0, "%");
        makeSlider(tr("Papel (warp):"), 0, 100, c ? (int)llround(c->lainkaWarpAmount) : 8,
            [this](int v) { applyBuiltInValue("lainkaWarpAmount", v); });
        makeSlider(tr("Opacidade:"), 0, 100, c ? (int)llround(c->lainkaOpacity) : 100,
            [this](int v) { applyBuiltInValue("lainkaOpacity", v); }, 1.0, "%");
    }
    else if (effectId == "pierrot_motion") {
        auto* chk = new QCheckBox(tr("Ativar MotiOn"));
        chk->setChecked(c && c->motionEnabled);
        connect(chk, &QCheckBox::toggled, this, [this](bool v) { applyBuiltInBool("motionEnabled", v); });
        form->addRow(chk);

        auto makeSlider = [&](const QString& label, int min, int max, int val,
                              std::function<void(int)> slot) {
            auto* sl = new QSlider(Qt::Horizontal);
            sl->setRange(min, max);
            sl->setValue(val);
            auto* lbl = new QLabel(QString::number(sl->value()));
            connect(sl, &QSlider::valueChanged, lbl, [lbl](int v) { lbl->setText(QString::number(v)); });
            connect(sl, &QSlider::valueChanged, this, [slot](int v) { slot(v); });
            auto* row = new QHBoxLayout;
            row->addWidget(sl, 1);
            row->addWidget(lbl);
            form->addRow(label, row);
        };

        makeSlider(tr("Intensidade:"), 0, 100, c ? (int)llround(c->motionAmount) : 0,
            [this](int v) { applyBuiltInValue("motionAmount", v); });
        makeSlider(tr("Ângulo:"), 0, 360, c ? (int)llround(c->motionAngle) : 0,
            [this](int v) { applyBuiltInValue("motionAngle", v); });
        makeSlider(tr("Amostras:"), 1, 32, c ? c->motionSamples : 8,
            [this](int v) { applyBuiltInValue("motionSamples", v); });
    }
    else if (effectId == "pierrot_audio_eq") {
        auto* chk = new QCheckBox(tr("Ativar EQ Express"));
        chk->setChecked(c && (std::fabs(c->eqLow) > 0.01 || std::fabs(c->eqMid) > 0.01
                              || std::fabs(c->eqHigh) > 0.01));
        connect(chk, &QCheckBox::toggled, this, [this](bool v) { applyBuiltInBool("eqExpress", v); });
        form->addRow(chk);

        auto* hint = new QLabel(tr("Iguala o som do clipe em 3 bandas. "
                                   "0 dB = sem alteração."), inner);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
            .arg(themeColors().expressDescText.name()));
        form->addRow(hint);

        auto makeDb = [&](const QString& label, double value, std::function<void(int)> slot) {
            auto* sl = new QSlider(Qt::Horizontal);
            sl->setRange(-120, 120); // décimos de dB
            sl->setValue((int)llround(value * 10.0));
            auto* lbl = new QLabel(QString::number(sl->value() / 10.0, 'f', 1) + " dB");
            connect(sl, &QSlider::valueChanged, lbl, [lbl](int v) {
                lbl->setText(QString::number(v / 10.0, 'f', 1) + " dB");
            });
            connect(sl, &QSlider::valueChanged, this, [slot](int v) { slot(v); });
            auto* row = new QHBoxLayout;
            row->addWidget(sl, 1);
            row->addWidget(lbl);
            form->addRow(label, row);
        };
        makeDb(tr("Graves:"), c ? c->eqLow : 0.0, [this](int v) { applyBuiltInValue("eqLow", v / 10.0); });
        makeDb(tr("Médios:"), c ? c->eqMid : 0.0, [this](int v) { applyBuiltInValue("eqMid", v / 10.0); });
        makeDb(tr("Agudos:"), c ? c->eqHigh : 0.0, [this](int v) { applyBuiltInValue("eqHigh", v / 10.0); });
    }
    else if (effectId == "pierrot_audio_reverb") {
        auto* chk = new QCheckBox(tr("Ativar Reverb EX"));
        chk->setChecked(c && c->reverb);
        connect(chk, &QCheckBox::toggled, this, [this](bool v) { applyBuiltInBool("reverb", v); });
        form->addRow(chk);

        auto makeSlider = [&](const QString& label, int min, int max, int val,
                              std::function<void(int)> slot) {
            auto* sl = new QSlider(Qt::Horizontal);
            sl->setRange(min, max);
            sl->setValue(val);
            auto* lbl = new QLabel(QString("%1%").arg(sl->value()));
            connect(sl, &QSlider::valueChanged, lbl, [lbl](int v) { lbl->setText(QString("%1%").arg(v)); });
            connect(sl, &QSlider::valueChanged, this, [slot](int v) { slot(v); });
            auto* row = new QHBoxLayout;
            row->addWidget(sl, 1);
            row->addWidget(lbl);
            form->addRow(label, row);
        };
        makeSlider(tr("Mix (úmido):"), 0, 100, c ? (int)llround(c->reverbMix * 100.0) : 35,
            [this](int v) { applyBuiltInValue("reverbMix", v / 100.0); });
        makeSlider(tr("Tamanho da sala:"), 0, 100, c ? (int)llround(c->reverbSize * 100.0) : 50,
            [this](int v) { applyBuiltInValue("reverbSize", v / 100.0); });
    }

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(inner);
    scroll->setStyleSheet(QStringLiteral("background:%1;").arg(themeColors().expressBg.name()));

    const auto& names = effectDisplayNames();
    const QString displayName = names.value(effectId, effectId);

    m_tabPages[effectId] = scroll;
    const int idx = m_tabs->addTab(scroll, displayName);
    m_tabMap[effectId] = idx;
    m_tabs->setCurrentIndex(idx);
}

// ── Aba de plugin OFX ────────────────────────────────────────────────────

void ExpressWidget::createOfxTab(const QString& pluginId)
{
    if (m_tabMap.contains(pluginId)) {
        m_tabs->setCurrentIndex(m_tabMap[pluginId]);
        return;
    }

    const OfxPluginInfo* info = nullptr;
    for (const OfxPluginInfo& p : m_ofxPlugins)
        if (p.id == pluginId) { info = &p; break; }

    const QString titleText = info ? (info->name.isEmpty() ? info->id : info->name) : pluginId;

    auto* page = new QWidget;
    auto* pageLay = new QVBoxLayout(page);
    pageLay->setContentsMargins(0, 0, 0, 0);
    pageLay->setSpacing(0);

    auto* title = new QLabel;
    title->setText(QStringLiteral("<b style='color:#dcddde;'>%1</b>").arg(titleText));
    title->setStyleSheet(QStringLiteral("padding:12px 16px 4px; background:%1;").arg(themeColors().expressBg.name()));
    pageLay->addWidget(title);

    if (info && !info->description.isEmpty()) {
        auto* desc = new QLabel(info->description);
        desc->setWordWrap(true);
        desc->setStyleSheet(QStringLiteral("color:%1; padding:4px 16px; font-size:11px; background:%2;")
            .arg(themeColors().expressDescText.name())
            .arg(themeColors().expressBg.name()));
        pageLay->addWidget(desc);
    }

    // Gera controles para os parâmetros descobertos.
    auto* paramsWidget = new QWidget;
    auto* paramsLayout = new QFormLayout(paramsWidget);
    paramsLayout->setContentsMargins(16, 8, 16, 8);
    paramsLayout->setSpacing(6);

    const auto& paramDefs = m_ofxParamDefs.value(pluginId);
    bool hasParams = false;

    for (const auto& pd : paramDefs) {
        const QString& paramName = pd.first;
        const QString& paramType = pd.second.first;
        const QString& paramLabel = pd.second.second.isEmpty() ? paramName : pd.second.second;

        // Encontra o índice do parâmetro no clipe (OfxPluginInstance).
        int paramIndex = -1;
        if (m_currentClip) {
            for (int i = 0; i < m_currentClip->ofxFx.size(); ++i) {
                if (m_currentClip->ofxFx[i].pluginId == pluginId) {
                    // Procura ou cria o parâmetro no instance
                    OfxPluginInstance& fx = m_currentClip->ofxFx[i];
                    bool found = false;
                    for (int j = 0; j < fx.params.size(); ++j) {
                        if (fx.params[j].key == paramName) { found = true; break; }
                    }
                    if (!found) {
                        OfxParam p;
                        p.key = paramName;
                        p.value = 0.0;
                        fx.params.append(p);
                    }
                    paramIndex = fx.params.size() - 1;
                    break;
                }
            }
        }

        if (paramType == kOfxParamTypeDouble || paramType == kOfxParamTypeInteger) {
            auto* spin = new QDoubleSpinBox;
            spin->setRange(-99999, 99999);
            spin->setDecimals(paramType == kOfxParamTypeInteger ? 0 : 3);
            spin->setValue(0);
            spin->setStyleSheet(QStringLiteral(
                "QDoubleSpinBox { background:%1; color:%2; border:1px solid %3; "
                "border-radius:3px; padding:3px 6px; }")
                .arg(themeColors().expressCardBg.name())
                .arg(themeColors().text.name())
                .arg(themeColors().inputBorder.name()));
            if (paramIndex >= 0 && m_currentClip) {
                spin->setValue(m_currentClip->ofxFx[0].params[paramIndex].value.toDouble());
            }
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, pluginId, paramName](double val) {
                    if (!m_currentClip) return;
                    for (auto& fx : m_currentClip->ofxFx) {
                        if (fx.pluginId == pluginId) {
                            for (auto& p : fx.params) {
                                if (p.key == paramName) { p.value = val; break; }
                            }
                            break;
                        }
                    }
                    emit modified();
                });
            paramsLayout->addRow(paramLabel, spin);
            hasParams = true;
        }
        else if (paramType == kOfxParamTypeBoolean) {
            auto* check = new QCheckBox;
            check->setChecked(false);
            check->setStyleSheet(QStringLiteral(
                "QCheckBox { color:%1; } QCheckBox::indicator { width:16px; height:16px; }")
                .arg(themeColors().text.name()));
            if (paramIndex >= 0 && m_currentClip) {
                check->setChecked(m_currentClip->ofxFx[0].params[paramIndex].value.toBool());
            }
            connect(check, &QCheckBox::toggled, this,
                [this, pluginId, paramName](bool val) {
                    if (!m_currentClip) return;
                    for (auto& fx : m_currentClip->ofxFx) {
                        if (fx.pluginId == pluginId) {
                            for (auto& p : fx.params) {
                                if (p.key == paramName) { p.value = val; break; }
                            }
                            break;
                        }
                    }
                    emit modified();
                });
            paramsLayout->addRow(paramLabel, check);
            hasParams = true;
        }
        else if (paramType == kOfxParamTypeChoice) {
            auto* combo = new QComboBox;
            combo->setStyleSheet(QStringLiteral(
                "QComboBox { background:%1; color:%2; border:1px solid %3; "
                "border-radius:3px; padding:3px 6px; }")
                .arg(themeColors().expressCardBg.name())
                .arg(themeColors().text.name())
                .arg(themeColors().inputBorder.name()));
            combo->addItem(QStringLiteral("0"));
            connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, pluginId, paramName](int val) {
                    if (!m_currentClip) return;
                    for (auto& fx : m_currentClip->ofxFx) {
                        if (fx.pluginId == pluginId) {
                            for (auto& p : fx.params) {
                                if (p.key == paramName) { p.value = val; break; }
                            }
                            break;
                        }
                    }
                    emit modified();
                });
            paramsLayout->addRow(paramLabel, combo);
            hasParams = true;
        }
        else if (paramType == kOfxParamTypeRGB || paramType == kOfxParamTypeRGBA) {
            auto* colorBtn = new QPushButton;
            colorBtn->setFixedSize(60, 24);
            colorBtn->setStyleSheet(QStringLiteral(
                "QPushButton { background:#00ff00; border:1px solid %1; border-radius:3px; }")
                .arg(themeColors().inputBorder.name()));
                connect(colorBtn, &QPushButton::clicked, this,
                    [this, colorBtn, pluginId, paramName, paramLabel]() {
                    QColor c = QColorDialog::getColor(Qt::green, this, paramLabel);
                    if (c.isValid()) {
                        colorBtn->setStyleSheet(QStringLiteral(
                            "QPushButton { background:%1; border:1px solid #4f545c; border-radius:3px; }")
                            .arg(c.name()));
                        if (!m_currentClip) return;
                        for (auto& fx : m_currentClip->ofxFx) {
                            if (fx.pluginId == pluginId) {
                                for (auto& p : fx.params) {
                                    if (p.key == paramName) { p.value = c; break; }
                                }
                                break;
                            }
                        }
                        emit modified();
                    }
                });
            paramsLayout->addRow(paramLabel, colorBtn);
            hasParams = true;
        }
        else if (paramType == kOfxParamTypeString) {
            auto* edit = new QLineEdit;
                    edit->setStyleSheet(QStringLiteral(
                        "QLineEdit { background:%1; color:%2; border:1px solid %3; "
                        "border-radius:3px; padding:3px 6px; }")
                        .arg(themeColors().expressCardBg.name())
                        .arg(themeColors().text.name())
                        .arg(themeColors().inputBorder.name()));
            connect(edit, &QLineEdit::textChanged, this,
                [this, pluginId, paramName](const QString& val) {
                    if (!m_currentClip) return;
                    for (auto& fx : m_currentClip->ofxFx) {
                        if (fx.pluginId == pluginId) {
                            for (auto& p : fx.params) {
                                if (p.key == paramName) { p.value = val; break; }
                            }
                            break;
                        }
                    }
                    emit modified();
                });
            paramsLayout->addRow(paramLabel, edit);
            hasParams = true;
        }
    }

    if (!hasParams) {
        auto* note = new QLabel(tr("Nenhum parâmetro descoberto.\nO plugin pode não expor parâmetros editáveis."));
        note->setAlignment(Qt::AlignCenter);
        note->setStyleSheet(QStringLiteral("color:%1; font-size:11px; padding:20px; background:%2;")
            .arg(themeColors().expressDescText.name())
            .arg(themeColors().expressBg.name()));
        pageLay->addWidget(note);
    } else {
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(paramsWidget);
        scroll->setStyleSheet(QStringLiteral("QScrollArea { background:%1; border:none; }")
            .arg(themeColors().expressBg.name()));
        pageLay->addWidget(scroll);
    }
    pageLay->addStretch(1);

    m_tabPages[pluginId] = page;
    const int idx = m_tabs->addTab(page, titleText);
    m_tabMap[pluginId] = idx;
    m_tabs->setCurrentIndex(idx);
}

// ── Aplicação de valores ─────────────────────────────────────────────────

void ExpressWidget::applyBuiltInValue(const QString& key, double value)
{
    if (!m_currentClip) return;
    if (key == "brightness") m_currentClip->brightness = value;
    else if (key == "contrast") m_currentClip->contrast = value;
    else if (key == "saturation") m_currentClip->saturation = value;
    else if (key == "blur") m_currentClip->blur = value;
    else if (key == "chromaKeySimilarity") m_currentClip->chromaKeySimilarity = value;
    else if (key == "lainkaSkip") m_currentClip->lainkaSkip = qMax(1, (int)value);
    else if (key == "lainkaJitterPos") m_currentClip->lainkaJitterPos = value;
    else if (key == "lainkaJitterRot") m_currentClip->lainkaJitterRot = value;
    else if (key == "lainkaJitterScale") m_currentClip->lainkaJitterScale = value;
    else if (key == "lainkaFlicker") m_currentClip->lainkaFlicker = value;
    else if (key == "lainkaFlickerSpeed") m_currentClip->lainkaFlickerSpeed = value;
    else if (key == "lainkaWarpAmount") m_currentClip->lainkaWarpAmount = value;
    else if (key == "lainkaWarpSpeed") m_currentClip->lainkaWarpSpeed = value;
    else if (key == "lainkaWarpGrid") m_currentClip->lainkaWarpGrid = qMax(4, (int)value);
    else if (key == "lainkaOnionSkin") m_currentClip->lainkaOnionSkin = value;
    else if (key == "lainkaDustAmount") m_currentClip->lainkaDustAmount = value;
    else if (key == "lainkaScratchAmount") m_currentClip->lainkaScratchAmount = value;
    else if (key == "lainkaTargetFps") m_currentClip->lainkaTargetFps = qMax(1, (int)value);
    else if (key == "lainkaMotionBlur") m_currentClip->lainkaMotionBlur = value;
    else if (key == "lainkaOpacity") m_currentClip->lainkaOpacity = value;
    else if (key == "lainkaAntialias") m_currentClip->lainkaAntialias = qBound(0, (int)value, 2);
    else if (key == "motionAmount") m_currentClip->motionAmount = value;
    else if (key == "motionAngle") m_currentClip->motionAngle = value;
    else if (key == "motionSamples") m_currentClip->motionSamples = qMax(1, (int)value);
    else if (key == "eqLow")     m_currentClip->eqLow = std::clamp(value, -12.0, 12.0);
    else if (key == "eqMid")     m_currentClip->eqMid = std::clamp(value, -12.0, 12.0);
    else if (key == "eqHigh")    m_currentClip->eqHigh = std::clamp(value, -12.0, 12.0);
    else if (key == "reverbMix") m_currentClip->reverbMix = std::clamp(value, 0.0, 1.0);
    else if (key == "reverbSize") m_currentClip->reverbSize = std::clamp(value, 0.0, 1.0);
    emit modified();
}

void ExpressWidget::applyBuiltInBool(const QString& key, bool value)
{
    if (!m_currentClip) return;
    if (key == "grayscale") m_currentClip->grayscale = value;
    else if (key == "chromaKey") m_currentClip->chromaKey = value;
    else if (key == "lainkaEnabled") m_currentClip->lainkaEnabled = value;
    else if (key == "motionEnabled") m_currentClip->motionEnabled = value;
    else if (key == "reverb") {
        m_currentClip->reverb = value;
        if (value && m_currentClip->reverbMix <= 0.0) m_currentClip->reverbMix = 0.35;
        if (value && m_currentClip->reverbSize <= 0.0) m_currentClip->reverbSize = 0.5;
    }
    else if (key == "eqExpress") {
        if (value) {
            if (std::fabs(m_currentClip->eqLow) <= 0.01
                && std::fabs(m_currentClip->eqMid) <= 0.01
                && std::fabs(m_currentClip->eqHigh) <= 0.01) {
                m_currentClip->eqLow = 0.0;
                m_currentClip->eqMid = 1.5;
                m_currentClip->eqHigh = 1.0;
            }
        } else {
            m_currentClip->eqLow = m_currentClip->eqMid = m_currentClip->eqHigh = 0.0;
        }
    }
    emit modified();
}

// ── Drag & Drop ──────────────────────────────────────────────────────────

void ExpressWidget::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasFormat(QLatin1String(kMimeEffect)))
        e->acceptProposedAction();
}

void ExpressWidget::dropEvent(QDropEvent* e)
{
    const QMimeData* md = e->mimeData();
    if (!md->hasFormat(QLatin1String(kMimeEffect))) { e->ignore(); return; }

    const QString effectId = QString::fromUtf8(md->data(QLatin1String(kMimeEffect)));
    if (effectId.isEmpty()) { e->ignore(); return; }

    addEffect(effectId);
    e->acceptProposedAction();
    e->accept();
}
