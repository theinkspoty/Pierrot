// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/panels/EffectsWidget.h"
#include "colombina/models/Project.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "ui/core/Theme.h"

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
#include <QTabWidget>

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
                                  const QString& id, const QString& tip,
                                  const QString& source) {
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, title);
    item->setData(0, Qt::UserRole, id);
    if (!tip.isEmpty()) item->setToolTip(0, tip);

    // Badge de origem: deixa óbvio o que é nativo x plugin (frei0r/OFX), o
    // ponto mais confuso do painel. Nativo fica sem marca; plugin ganha um
    // quadradinho colorido + tom no texto.
    QColor badge;
    QString tag;
    if (source == QLatin1String("frei0r")) {
        badge = QColor(124, 196, 116);
        tag = tr("frei0r");
    } else if (source == QLatin1String("ofx")) {
        badge = QColor(186, 150, 232);
        tag = tr("OFX");
    }
    if (!badge.isValid()) return;

    QPixmap pm(10, 10);
    pm.fill(Qt::transparent);
    {
        QPainter bp(&pm);
        bp.setRenderHint(QPainter::Antialiasing, true);
        bp.setPen(Qt::NoPen);
        bp.setBrush(badge);
        bp.drawEllipse(1, 1, 8, 8);
    }
    item->setIcon(0, QIcon(pm));
    item->setForeground(0, badge.lighter(115));
    const QString hint = tip.isEmpty() ? tag : (tag + QStringLiteral(" — ") + tip);
    item->setToolTip(0, hint);
}

// ── Construtor — layout Premiere (busca + abas por tipo) ─────────────────

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

    // Abas por tipo de efeito (Vídeo | Áudio | Texto | Plugins); cada aba
    // tem sua própria árvore.
    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setStyleSheet(QStringLiteral("QTabWidget::pane { border:none; }"));
    rootLay->addWidget(m_tabs, 1);

    connect(m_searchBox, &QLineEdit::textChanged,
            this, &EffectsWidget::onSearchChanged);

    buildTree();
}

// Cria uma árvore de efeitos padrão (opções + arrasto) para uma aba.
EffectTree* EffectsWidget::makeTree() {
    auto* tree = new EffectTree(this);
    tree->setHeaderHidden(true);
    tree->setRootIsDecorated(true);
    tree->setIndentation(14);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setDragEnabled(true);
    tree->setDragDropMode(QAbstractItemView::DragOnly);
    tree->setStyleSheet(QStringLiteral(
        "QTreeWidget { background:%1; border:none; font-size:12px; }"
        "QTreeWidget::item { padding:3px 4px; }"
        "QTreeWidget::item:selected { background:rgba(70,130,210,0.28); color:%2; }"
        "QTreeWidget::item:hover { background:rgba(255,255,255,0.06); }")
        .arg(themeColors().base.name())
        .arg(themeColors().text.name()));
    connect(tree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem*) { onItemActivated(); });
    connect(tree, &QTreeWidget::itemActivated,
            this, [this](QTreeWidgetItem*, int) { onItemActivated(); });
    return tree;
}

// ── Árvore por abas (Vídeo | Áudio | Texto | Plugins) ────────────────────

void EffectsWidget::buildTree() {
    const int wasTab = m_tabs->currentIndex();
    // Remove as árvores das abas antigas (recriadas a seguir).
    while (m_tabs->count() > 0) {
        QWidget* w = m_tabs->widget(0);
        m_tabs->removeTab(0);
        w->deleteLater();
    }

    // ── Aba Vídeo: Video Effects (por categoria) + Transitions + Presets ─
    EffectTree* videoTree = makeTree();

    auto* videoFx = addFolder(videoTree, nullptr, tr("Video Effects (do Pierrot)"));
    auto* adjust = addFolder(videoTree, videoFx, tr("Adjust"));
    addEffectItem(adjust, tr("Brightness & Contrast"), QStringLiteral("pierrot_brightness"),
                  tr("Ajusta luminosidade (brilho) da imagem"));
    addEffectItem(adjust, tr("Contrast"), QStringLiteral("pierrot_contrast"),
                  tr("Controla diferença entre claros e escuros"));
    addEffectItem(adjust, tr("Saturation"), QStringLiteral("pierrot_saturation"),
                  tr("Intensidade das cores (0 = preto e branco)"));
    addEffectItem(adjust, tr("Color Correction (Lumetri)"), QStringLiteral("pierrot_lumetri"),
                  tr("Abre a correção de cor estilo Lumetri no clipe"));

    auto* blur = addFolder(videoTree, videoFx, tr("Blur & Sharpen"));
    addEffectItem(blur, tr("Gaussian Blur"), QStringLiteral("pierrot_blur"),
                  tr("Desfoque gaussiano (box blur no preview/export)"));

    auto* channel = addFolder(videoTree, videoFx, tr("Channel"));
    addEffectItem(channel, tr("Black & White"), QStringLiteral("pierrot_grayscale"),
                  tr("Converte para escala de cinza"));

    auto* key = addFolder(videoTree, videoFx, tr("Key"));
    addEffectItem(key, tr("Chroma Key"), QStringLiteral("pierrot_chromakey"),
                  tr("Remove cor (verde por padrão) para transparência"));

    auto* stylize = addFolder(videoTree, videoFx, tr("Stylize"));
    addEffectItem(stylize, tr("Film Grain"), QStringLiteral("pierrot_filmgrain"),
                  tr("Grão de filme: ruído monocromático animado por quadro (estética de película)"));
    addEffectItem(stylize, tr("PSX"), QStringLiteral("pierrot_psx"),
                  tr("Granulado retrô: quantização de cor + dithering estilo PlayStation 1"));
    addEffectItem(stylize, tr("LAINKA (stop motion)"), QStringLiteral("pierrot_lainka"),
                  tr("Stop motion: jitter, flicker, warp, dust, scratch"));
    addEffectItem(stylize, tr("MotiOn (motion blur)"), QStringLiteral("pierrot_motion"),
                  tr("Motion blur por amostragem da imagem"));

    auto* videoTr = addFolder(videoTree, nullptr, tr("Video Transitions"));
    auto* dissolve = addFolder(videoTree, videoTr, tr("Dissolve"));
    addEffectItem(dissolve, tr("Dissolve"), QStringLiteral("trans:dissolve"),
                  tr("Crossfade quando o próximo clipe se sobrepõe"));
    auto* wipe = addFolder(videoTree, videoTr, tr("Wipe"));
    addEffectItem(wipe, tr("Wipe Left"), QStringLiteral("trans:wipeleft"));
    addEffectItem(wipe, tr("Wipe Right"), QStringLiteral("trans:wiperight"));
    addEffectItem(wipe, tr("Wipe Up"), QStringLiteral("trans:wipeup"));
    addEffectItem(wipe, tr("Wipe Down"), QStringLiteral("trans:wipedown"));
    addEffectItem(wipe, tr("Wipe Top-Left"), QStringLiteral("trans:wipetl"));
    addEffectItem(wipe, tr("Wipe Top-Right"), QStringLiteral("trans:wipetr"));
    addEffectItem(wipe, tr("Wipe Bottom-Right"), QStringLiteral("trans:wipebr"));
    addEffectItem(wipe, tr("Wipe Bottom-Left"), QStringLiteral("trans:wipebl"));

    auto* presets = addFolder(videoTree, nullptr, tr("Presets"));
    addEffectItem(presets, tr("Fade In"), QStringLiteral("text:fadeIn"),
                  tr("Aplica fade-in no clipe selecionado"));
    addEffectItem(presets, tr("Fade Out"), QStringLiteral("text:fadeOut"),
                  tr("Aplica fade-out no clipe selecionado"));

    m_tabs->addTab(videoTree, tr("Vídeo"));

    // ── Aba Áudio: Audio Effects + Audio Transitions ────────────────────
    EffectTree* audioTree = makeTree();

    auto* audioFx = addFolder(audioTree, nullptr, tr("Audio Effects"));
    addEffectItem(audioFx, tr("EQ Express"), QStringLiteral("pierrot_audio_eq"),
                  tr("Equalizador de 3 bandas (graves/médios/agudos)"));
    addEffectItem(audioFx, tr("Reverb EX"), QStringLiteral("pierrot_audio_reverb"),
                  tr("Reverb simples (Schroeder) no áudio do clipe"));

    auto* audioTr = addFolder(audioTree, nullptr, tr("Audio Transitions"));
    addEffectItem(audioTr, tr("Constant Power"), QStringLiteral("trans:constantpower"),
                  tr("Crossfade de áudio quando há sobreposição na mesma faixa"));

    m_tabs->addTab(audioTree, tr("Áudio"));

    // ── Aba Texto: Text / Animation ─────────────────────────────────────
    EffectTree* textTree = makeTree();
    auto* text = addFolder(textTree, nullptr, tr("Text / Animation"));
    addEffectItem(text, tr("Criar clipe de texto"), QStringLiteral("text:create"),
                  tr("Insere um clipe de texto no playhead"));
    addEffectItem(text, tr("Abrir editor de texto"), QStringLiteral("text:edit"),
                  tr("Abre o editor do clipe de texto selecionado"));
    addEffectItem(text, tr("Fade In (texto)"), QStringLiteral("text:fadeIn"),
                  tr("fadeIn no clipe selecionado"));
    addEffectItem(text, tr("Fade Out (texto)"), QStringLiteral("text:fadeOut"),
                  tr("fadeOut no clipe selecionado"));
    m_tabs->addTab(textTree, tr("Texto"));

    // ── Aba Plugins: frei0r + OFX ───────────────────────────────────────
    EffectTree* plugTree = makeTree();

    auto* f0r = addFolder(plugTree, nullptr,
                          m_frei0rPlugins.isEmpty()
                              ? tr("frei0r (nenhum instalado)")
                              : tr("frei0r — plugins (%1)").arg(m_frei0rPlugins.size()));
    f0r->setToolTip(0, tr("Plugins frei0r (Kdenlive/Shotcut/MLT). Instale com o "
                          "pacote frei0r-plugins ou defina PIERROT_FREI0R_PATH."));
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
                          p.description.isEmpty() ? p.name : p.description,
                          QStringLiteral("frei0r"));
        }
    }

    auto* third = addFolder(plugTree, nullptr,
                            m_ofxPlugins.isEmpty()
                                ? tr("OFX (nenhum instalado)")
                                : tr("OFX — plugins de terceiros (%1)").arg(m_ofxPlugins.size()));
    third->setToolTip(0, tr("Plugins OFX (ex.: DaVinci/Resolve, Sapphire, "
                            "BorisFX) encontrados no sistema."));
    if (m_ofxPlugins.isEmpty()) {
        auto* none = new QTreeWidgetItem(third);
        none->setText(0, tr("(nenhum plugin OFX)"));
        none->setFlags(none->flags() & ~Qt::ItemIsSelectable);
        none->setForeground(0, QColor(120, 120, 120));
    } else {
        for (const OfxPluginInfo& p : m_ofxPlugins) {
            addEffectItem(third, p.name.isEmpty() ? p.id : p.name, p.id,
                          p.description.isEmpty() ? p.id : p.description,
                          QStringLiteral("ofx"));
        }
    }

    m_tabs->addTab(plugTree, tr("Plugins"));

    m_tabs->setCurrentIndex(wasTab >= 0 && wasTab < m_tabs->count() ? wasTab : 0);

    // Reaplica o filtro ativo (o rebuild recriou as árvores sem filtro).
    const QString q = m_searchBox->text().trimmed();
    if (!q.isEmpty()) filterTree(q);
}

// ── Ativação (duplo clique / Enter) ──────────────────────────────────────

void EffectsWidget::onItemActivated() {
    auto* tree = qobject_cast<EffectTree*>(m_tabs->currentWidget());
    if (!tree) return;
    auto* item = tree->currentItem();
    if (!item) return;
    const QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return;
    emit effectSelected(v.toString());
}

// ── Busca (filtra todas as abas; pastas ficam se algum filho casa) ───────
//
// Mostra um item se ele casa, se um descendente casa, ou se um ancestral
// casa (ex.: buscar "Blur" mostra a pasta "Blur & Sharpen" inteira).

namespace {
bool applyEffectFilter(QTreeWidgetItem* item, const QString& text, bool ancestorMatch) {
    const bool selfMatch = item->text(0).contains(text, Qt::CaseInsensitive);
    const bool passDown = ancestorMatch || selfMatch;
    bool childMatch = false;
    for (int i = 0; i < item->childCount(); ++i)
        childMatch = applyEffectFilter(item->child(i), text, passDown) || childMatch;
    item->setHidden(!(selfMatch || childMatch || ancestorMatch));
    return selfMatch || childMatch;
}

void setItemVisibleDeep(QTreeWidgetItem* item, bool visible) {
    item->setHidden(!visible);
    for (int i = 0; i < item->childCount(); ++i)
        setItemVisibleDeep(item->child(i), visible);
}
} // namespace

void EffectsWidget::onSearchChanged(const QString& text) {
    filterTree(text.trimmed());
}

void EffectsWidget::filterTree(const QString& text) {
    int firstMatchTab = -1;
    for (int t = 0; t < m_tabs->count(); ++t) {
        auto* tree = qobject_cast<EffectTree*>(m_tabs->widget(t));
        if (!tree) continue;
        bool anyVisible = false;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = tree->topLevelItem(i);
            if (text.isEmpty()) {
                setItemVisibleDeep(top, true);
                anyVisible = true;
            } else if (applyEffectFilter(top, text, false)) {
                anyVisible = true;
            }
        }
        if (!text.isEmpty() && anyVisible) {
            tree->expandAll();
            if (firstMatchTab < 0) firstMatchTab = t;
        }
    }
    if (!text.isEmpty() && firstMatchTab >= 0)
        m_tabs->setCurrentIndex(firstMatchTab);
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
