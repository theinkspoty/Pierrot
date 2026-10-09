// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "EffectsWidget.h"
#include "colombina/models/Project.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "ui/Theme.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QFont>
#include <QDrag>
#include <QMimeData>
#include <QPixmap>
#include <QPainter>
#include <QIcon>
#include <QLineEdit>
#include <QTreeWidgetItem>
#include <QSet>

// ── EffectTree: arrasto Premiere ─────────────────────────────────────────

void EffectTree::startDrag(Qt::DropActions) {
    auto* item = currentItem();
    if (!item) return;
    const QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return;
    const QString effectId = v.toString();

    auto* drag = new QDrag(this);
    auto* md = new QMimeData;
    md->setData(QLatin1String(kMimeEffect), effectId.toUtf8());
    drag->setMimeData(md);

    QPixmap pix(140, 28);
    pix.fill(QColor(40, 42, 48, 200));
    QPainter p(&pix);
    p.setPen(QColor(220, 221, 222));
    p.setFont(QFont(QStringLiteral("sans-serif"), 9));
    p.drawText(pix.rect(), Qt::AlignCenter,
               item->text(0).isEmpty() ? effectId : item->text(0));
    p.end();
    drag->setPixmap(pix);
    drag->exec(Qt::CopyAction);
}

// ── Helpers de árvore ────────────────────────────────────────────────────

QTreeWidgetItem* EffectsWidget::addFolder(QTreeWidget* tree, QTreeWidgetItem* parent,
                                          const QString& title) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setText(0, title);
    item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    QFont bold;
    bold.setBold(true);
    item->setFont(0, bold);
    item->setExpanded(true);
    return item;
}

void EffectsWidget::addEffectItem(QTreeWidgetItem* parent, const QString& title,
                                  const QString& id, const QString& tip) {
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, title);
    item->setData(0, Qt::UserRole, id);
    if (!tip.isEmpty()) item->setToolTip(0, tip);
}

// ── Construtor — layout Premiere (busca + árvore única) ──────────────────

EffectsWidget::EffectsWidget(QWidget* parent) : QWidget(parent) {
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);

    // Busca no topo (como o Effects do Premiere).
    m_searchBox = new QLineEdit(this);
    m_searchBox->setPlaceholderText(tr("Buscar efeitos…"));
    m_searchBox->setClearButtonEnabled(true);
    m_searchBox->setStyleSheet(QStringLiteral(
        "QLineEdit { background:%1; color:%2; border:none; "
        "padding:6px 8px; font-size:12px; }"
        "QLineEdit:focus { border-bottom:1px solid rgba(70,130,210,0.5); }")
        .arg(themeColors().effectsSearchBg.name())
        .arg(themeColors().text.name()));
    rootLay->addWidget(m_searchBox);

    // Uma árvore única (não abas) — estrutura do Premiere.
    m_tree = new EffectTree(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(14);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setStyleSheet(QStringLiteral(
        "QTreeWidget { background:%1; border:none; font-size:12px; }"
        "QTreeWidget::item { padding:3px 4px; }"
        "QTreeWidget::item:selected { background:rgba(70,130,210,0.28); color:%2; }"
        "QTreeWidget::item:hover { background:rgba(255,255,255,0.06); }")
        .arg(themeColors().base.name())
        .arg(themeColors().text.name()));
    rootLay->addWidget(m_tree, 1);

    connect(m_searchBox, &QLineEdit::textChanged,
            this, &EffectsWidget::onSearchChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem*) { onItemActivated(); });
    connect(m_tree, &QTreeWidget::itemActivated,
            this, [this](QTreeWidgetItem*, int) { onItemActivated(); });

    buildTree();
}

// ── Árvore estilo Premiere ───────────────────────────────────────────────

void EffectsWidget::buildTree() {
    m_tree->clear();

    // ── Presets ──────────────────────────────────────────────────────────
    auto* presets = addFolder(m_tree, nullptr, tr("Presets"));
    addEffectItem(presets, tr("Fade In"), QStringLiteral("text:fadeIn"),
                  tr("Aplica fade-in no clipe selecionado"));
    addEffectItem(presets, tr("Fade Out"), QStringLiteral("text:fadeOut"),
                  tr("Aplica fade-out no clipe selecionado"));

    // ── Audio Effects ────────────────────────────────────────────────────
    auto* audioFx = addFolder(m_tree, nullptr, tr("Audio Effects"));
    addEffectItem(audioFx, tr("EQ Express"), QStringLiteral("pierrot_audio_eq"),
                  tr("Equalizador de 3 bandas (graves/médios/agudos)"));
    addEffectItem(audioFx, tr("Reverb EX"), QStringLiteral("pierrot_audio_reverb"),
                  tr("Reverb simples (Schroeder) no áudio do clipe"));

    // ── Audio Transitions ────────────────────────────────────────────────
    auto* audioTr = addFolder(m_tree, nullptr, tr("Audio Transitions"));
    addEffectItem(audioTr, tr("Constant Power"), QStringLiteral("trans:constantpower"),
                  tr("Crossfade de áudio quando há sobreposição na mesma faixa"));

    // ── Video Effects ────────────────────────────────────────────────────
    auto* videoFx = addFolder(m_tree, nullptr, tr("Video Effects"));

    auto* adjust = addFolder(m_tree, videoFx, tr("Adjust"));
    addEffectItem(adjust, tr("Brightness & Contrast"), QStringLiteral("pierrot_brightness"),
                  tr("Ajusta luminosidade (brilho) da imagem"));
    addEffectItem(adjust, tr("Contrast"), QStringLiteral("pierrot_contrast"),
                  tr("Controla diferença entre claros e escuros"));
    addEffectItem(adjust, tr("Saturation"), QStringLiteral("pierrot_saturation"),
                  tr("Intensidade das cores (0 = preto e branco)"));
    addEffectItem(adjust, tr("Color Correction (Lumetri)"), QStringLiteral("pierrot_lumetri"),
                  tr("Abre a correção de cor estilo Lumetri no clipe"));

    auto* blur = addFolder(m_tree, videoFx, tr("Blur & Sharpen"));
    addEffectItem(blur, tr("Gaussian Blur"), QStringLiteral("pierrot_blur"),
                  tr("Desfoque gaussiano (box blur no preview/export)"));

    auto* channel = addFolder(m_tree, videoFx, tr("Channel"));
    addEffectItem(channel, tr("Black & White"), QStringLiteral("pierrot_grayscale"),
                  tr("Converte para escala de cinza"));

    auto* key = addFolder(m_tree, videoFx, tr("Key"));
    addEffectItem(key, tr("Chroma Key"), QStringLiteral("pierrot_chromakey"),
                  tr("Remove cor (verde por padrão) para transparência"));

    auto* stylize = addFolder(m_tree, videoFx, tr("Stylize"));
    addEffectItem(stylize, tr("PSX"), QStringLiteral("pierrot_psx"),
                  tr("Granulado retrô: quantização de cor + dithering estilo PlayStation 1"));
    addEffectItem(stylize, tr("LAINKA (stop motion)"), QStringLiteral("pierrot_lainka"),
                  tr("Stop motion: jitter, flicker, warp, dust, scratch"));
    addEffectItem(stylize, tr("MotiOn (motion blur)"), QStringLiteral("pierrot_motion"),
                  tr("Motion blur por amostragem da imagem"));

    // Third-party
    auto* third = addFolder(m_tree, videoFx, tr("Third-Party"));
    if (m_ofxPlugins.isEmpty()) {
        auto* none = new QTreeWidgetItem(third);
        none->setText(0, tr("(nenhum plugin OFX)"));
        none->setFlags(none->flags() & ~Qt::ItemIsSelectable);
        none->setForeground(0, QColor(120, 120, 120));
    } else {
        for (const OfxPluginInfo& p : m_ofxPlugins) {
            addEffectItem(third, p.name.isEmpty() ? p.id : p.name, p.id,
                          p.description.isEmpty() ? p.id : p.description);
        }
    }

    auto* f0r = addFolder(m_tree, videoFx, tr("frei0r"));
    if (m_frei0rPlugins.isEmpty()) {
        auto* none = new QTreeWidgetItem(f0r);
        none->setText(0, tr("(instale frei0r-plugins)"));
        none->setFlags(none->flags() & ~Qt::ItemIsSelectable);
        none->setForeground(0, QColor(120, 120, 120));
        none->setToolTip(0, tr("Debian/Ubuntu: pacote frei0r-plugins; "
                               "ou PIERROT_FREI0R_PATH=/pasta/com/.so"));
    } else {
        for (const Frei0rPluginInfo& p : m_frei0rPlugins) {
            if (!p.isFilter) continue;
            addEffectItem(f0r, p.label.isEmpty() ? p.name : p.label,
                          QStringLiteral("frei0r:") + p.name,
                          p.description.isEmpty() ? p.name : p.description);
        }
    }

    // ── Video Transitions (mapeia para Clip::transitionType) ────────────
    auto* videoTr = addFolder(m_tree, nullptr, tr("Video Transitions"));

    auto* dissolve = addFolder(m_tree, videoTr, tr("Dissolve"));
    addEffectItem(dissolve, tr("Dissolve"), QStringLiteral("trans:dissolve"),
                  tr("Crossfade quando o próximo clipe se sobrepõe"));

    auto* wipe = addFolder(m_tree, videoTr, tr("Wipe"));
    addEffectItem(wipe, tr("Wipe Left"), QStringLiteral("trans:wipeleft"));
    addEffectItem(wipe, tr("Wipe Right"), QStringLiteral("trans:wiperight"));
    addEffectItem(wipe, tr("Wipe Up"), QStringLiteral("trans:wipeup"));
    addEffectItem(wipe, tr("Wipe Down"), QStringLiteral("trans:wipedown"));
    addEffectItem(wipe, tr("Wipe Top-Left"), QStringLiteral("trans:wipetl"));
    addEffectItem(wipe, tr("Wipe Top-Right"), QStringLiteral("trans:wipetr"));
    addEffectItem(wipe, tr("Wipe Bottom-Right"), QStringLiteral("trans:wipebr"));
    addEffectItem(wipe, tr("Wipe Bottom-Left"), QStringLiteral("trans:wipebl"));

    // ── Text / Animation ─────────────────────────────────────────────────
    auto* text = addFolder(m_tree, nullptr, tr("Text / Animation"));
    addEffectItem(text, tr("Criar clipe de texto"), QStringLiteral("text:create"),
                  tr("Insere um clipe de texto no playhead"));
    addEffectItem(text, tr("Abrir editor de texto"), QStringLiteral("text:edit"),
                  tr("Abre o editor do clipe de texto selecionado"));
    addEffectItem(text, tr("Fade In (texto)"), QStringLiteral("text:fadeIn"),
                  tr("fadeIn no clipe selecionado"));
    addEffectItem(text, tr("Fade Out (texto)"), QStringLiteral("text:fadeOut"),
                  tr("fadeOut no clipe selecionado"));
}

// ── Ativação (duplo clique / Enter) ──────────────────────────────────────

void EffectsWidget::onItemActivated() {
    auto* item = m_tree->currentItem();
    if (!item) return;
    const QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return;
    emit effectSelected(v.toString());
}

// ── Busca (filtra folhas; pastas ficam se algum filho casa) ──────────────

void EffectsWidget::onSearchChanged(const QString& text) {
    filterTree(text.trimmed());
}

void EffectsWidget::filterTree(const QString& text) {
    QSet<QTreeWidgetItem*> keep;
    if (!text.isEmpty()) {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = m_tree->topLevelItem(i);
            for (int j = 0; j < top->childCount(); ++j) {
                QTreeWidgetItem* ch = top->child(j);
                // Pasta de 2º nível
                for (int k = 0; k < ch->childCount(); ++k) {
                    QTreeWidgetItem* leaf = ch->child(k);
                    if (leaf->text(0).contains(text, Qt::CaseInsensitive)) {
                        keep.insert(top);
                        keep.insert(ch);
                        keep.insert(leaf);
                    }
                }
                if (ch->text(0).contains(text, Qt::CaseInsensitive)) {
                    keep.insert(top);
                    keep.insert(ch);
                }
            }
            if (top->text(0).contains(text, Qt::CaseInsensitive))
                keep.insert(top);
        }
    }
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_tree->topLevelItem(i);
        if (text.isEmpty()) {
            top->setHidden(false);
            for (int j = 0; j < top->childCount(); ++j) {
                QTreeWidgetItem* ch = top->child(j);
                ch->setHidden(false);
                for (int k = 0; k < ch->childCount(); ++k)
                    ch->child(k)->setHidden(false);
            }
            continue;
        }
        const bool topIn = keep.contains(top);
        top->setHidden(!topIn);
        if (!topIn) continue;
        for (int j = 0; j < top->childCount(); ++j) {
            QTreeWidgetItem* ch = top->child(j);
            const bool chIn = keep.contains(ch);
            ch->setHidden(!chIn);
            if (!chIn) continue;
            for (int k = 0; k < ch->childCount(); ++k) {
                QTreeWidgetItem* leaf = ch->child(k);
                leaf->setHidden(!keep.contains(leaf));
            }
        }
    }
    if (!text.isEmpty()) m_tree->expandAll();
}

// ── Plugins / seleção ────────────────────────────────────────────────────

void EffectsWidget::setOfxPlugins(const QVector<OfxPluginInfo>& plugins) {
    m_ofxPlugins = plugins;
    buildTree();
}

void EffectsWidget::setFrei0rPlugins(const QVector<Frei0rPluginInfo>& plugins) {
    m_frei0rPlugins = plugins;
    buildTree();
}

void EffectsWidget::setSelectedClip(Clip* clip) {
    m_currentClip = clip;
}
