// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "EffectsWidget.h"
#include "models/Project.h"
#include "ui/Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFont>
#include <QDrag>
#include <QMimeData>
#include <QPixmap>
#include <QPainter>
#include <QIcon>
#include <QLineEdit>
#include <QTreeWidgetItem>
#include <QScrollArea>
#include <QFrame>

// ── EffectTree: arrasto de efeitos ───────────────────────────────────────

void EffectTree::startDrag(Qt::DropActions /*supportedActions*/)
{
    auto* item = currentItem();
    if (!item) return;
    const QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return;

    const QString effectId = v.toString();

    auto* drag = new QDrag(this);
    auto* md = new QMimeData;
    md->setData(QLatin1String(kMimeEffect), effectId.toUtf8());
    drag->setMimeData(md);

    static const QHash<QString, QString> fxIcons = {
        { QStringLiteral("pierrot_lainka"), QStringLiteral(":/fx/lainka.ico") },
        { QStringLiteral("pierrot_motion"), QStringLiteral(":/fx/motion.ico") },
    };
    const QString iconPath = fxIcons.value(effectId);
    QPixmap pix(120, 40);
    pix.fill(QColor(40, 42, 48, 200));
    QPainter p(&pix);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (!iconPath.isEmpty()) {
        p.drawPixmap(4, 4, 32, 32, QPixmap(iconPath).scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    p.setPen(QColor(220, 221, 222));
    p.setFont(QFont(QStringLiteral("sans-serif"), 10));
    p.drawText(iconPath.isEmpty() ? 8 : 42, 0, pix.width(), pix.height(),
               Qt::AlignLeft | Qt::AlignVCenter, item->text(0).isEmpty() ? effectId : item->text(0));
    p.end();
    drag->setPixmap(pix);

    drag->exec(Qt::CopyAction);
}

// ── Descrições dos efeitos ──────────────────────────────────────────────

QHash<QString, QString> EffectsWidget::effectDescriptions()
{
    return {
        { QStringLiteral("pierrot_brightness"),
          tr("Ajusta a luminosidade da imagem. Valores positivos clareiam, negativos escurecem.") },
        { QStringLiteral("pierrot_contrast"),
          tr("Controla a diferença entre áreas claras e escuras. Valores altos aumentam o contraste.") },
        { QStringLiteral("pierrot_saturation"),
          tr("Ajusta a intensidade das cores. Zero resulta em preto e branco.") },
        { QStringLiteral("pierrot_blur"),
          tr("Aplica desfoque gaussiano para suavizar detalhes e reduzir nitidez.") },
        { QStringLiteral("pierrot_grayscale"),
          tr("Converte a imagem para escala de cinza, removendo toda saturação de cor.") },
        { QStringLiteral("pierrot_chromakey"),
          tr("Remove uma cor específica (verde por padrão) para criar transparência. Útil para chroma key.") },
        { QStringLiteral("pierrot_lainka"),
          tr("Cria deformações que simulam efeito stop motion na imagem. Ideal para animações com aparência quadro a quadro.") },
        { QStringLiteral("pierrot_motion"),
          tr("Efeito de movimento e câmera dinâmica. Adiciona deslocamento e rotação à imagem.") },
    };
}

// ── Construtor ───────────────────────────────────────────────────────────

EffectsWidget::EffectsWidget(QWidget* parent) : QWidget(parent)
{
    auto* rootLay = new QVBoxLayout(this);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);

    // Barra de busca
    m_searchBox = new QLineEdit(this);
    m_searchBox->setPlaceholderText(tr("Buscar efeitos..."));
    m_searchBox->setClearButtonEnabled(true);
    m_searchBox->setStyleSheet(QStringLiteral(
        "QLineEdit { background:%1; color:%2; border:none; "
        "padding:6px 8px; font-size:12px; }"
        "QLineEdit:focus { border-bottom:1px solid rgba(70,130,210,0.5); }")
        .arg(themeColors().effectsSearchBg.name())
        .arg(themeColors().text.name()));
    rootLay->addWidget(m_searchBox);

    // Layout horizontal: árvore (esquerda) + preview (direita)
    auto* hLay = new QHBoxLayout;
    hLay->setContentsMargins(0, 0, 0, 0);
    hLay->setSpacing(0);

    // Árvore de efeitos
    m_tree = new EffectTree(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(16);
    m_tree->setMinimumWidth(160);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setStyleSheet(QStringLiteral(
        "QTreeWidget { background:%1; color:%2; border:none; }"
        "QTreeWidget::item { padding:2px 4px; }"
        "QTreeWidget::item:selected { background:rgba(70,130,210,0.32); }"
        "QTreeWidget::item:hover { background:rgba(255,255,255,0.06); }"
        "QTreeWidget::branch { background:%3; }"
        "QScrollBar:vertical { background:%4; width:8px; }"
        "QScrollBar::handle:vertical { background:%5; border-radius:4px; min-height:24px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }")
        .arg(themeColors().effectsTreeBg.name())
        .arg(themeColors().text.name())
        .arg(themeColors().effectsTreeBg.name())
        .arg(themeColors().scrollbarBg.name())
        .arg(themeColors().scrollbarHandle.name()));
    hLay->addWidget(m_tree, 1);

    // Separador vertical
    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::VLine);
    separator->setStyleSheet(QStringLiteral("color:%1;").arg(themeColors().dockBorder.name()));
    hLay->addWidget(separator);

    // Painel de preview
    m_previewPanel = new QWidget(this);
    m_previewPanel->setMinimumWidth(180);
    m_previewPanel->setStyleSheet(QStringLiteral("background:%1;").arg(themeColors().effectsPreviewBg.name()));
    auto* previewLay = new QVBoxLayout(m_previewPanel);
    previewLay->setContentsMargins(16, 16, 16, 16);
    previewLay->setSpacing(12);

    m_previewIcon = new QLabel(m_previewPanel);
    m_previewIcon->setAlignment(Qt::AlignCenter);
    m_previewIcon->setFixedHeight(64);
    previewLay->addWidget(m_previewIcon);

    m_previewName = new QLabel(m_previewPanel);
    m_previewName->setAlignment(Qt::AlignCenter);
    QFont nameFont;
    nameFont.setBold(true);
    nameFont.setPointSize(13);
    m_previewName->setFont(nameFont);
    m_previewName->setStyleSheet(QStringLiteral("color:%1;").arg(themeColors().text.name()));
    previewLay->addWidget(m_previewName);

    m_previewDesc = new QLabel(m_previewPanel);
    m_previewDesc->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_previewDesc->setWordWrap(true);
    m_previewDesc->setStyleSheet(QStringLiteral("color:%1; font-size:12px; line-height:1.4;").arg(themeColors().expressDescText.name()));
    previewLay->addWidget(m_previewDesc);

    hLay->addWidget(m_previewPanel, 1);

    rootLay->addLayout(hLay, 1);

    buildTree();

    connect(m_tree, &QTreeWidget::itemClicked,
            this, &EffectsWidget::onTreeItemClicked);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &EffectsWidget::onTreeItemDoubleClicked);
    connect(m_searchBox, &QLineEdit::textChanged,
            this, &EffectsWidget::onSearchChanged);
    connect(m_tree, &QTreeWidget::itemSelectionChanged,
            this, &EffectsWidget::onSelectionChanged);
}

// ── Construção da árvore ─────────────────────────────────────────────────

void EffectsWidget::buildTree()
{
    m_tree->clear();

    QFont bold;
    bold.setBold(true);

    // ── Genérico ──────────────────────────────────────────────────────
    auto* pierrotCat = new QTreeWidgetItem(m_tree);
    pierrotCat->setText(0, tr("Genérico"));
    pierrotCat->setFlags(pierrotCat->flags() & ~Qt::ItemIsSelectable);
    pierrotCat->setFont(0, bold);

    const QStringList builtInNames = {
        tr("Brilho"),
        tr("Contraste"),
        tr("Saturação"),
        tr("Desfoque"),
        tr("Preto e Branco"),
        tr("Chroma Key"),
    };
    const QStringList builtInIds = {
        QStringLiteral("pierrot_brightness"),
        QStringLiteral("pierrot_contrast"),
        QStringLiteral("pierrot_saturation"),
        QStringLiteral("pierrot_blur"),
        QStringLiteral("pierrot_grayscale"),
        QStringLiteral("pierrot_chromakey"),
    };

    for (int i = 0; i < builtInNames.size(); ++i) {
        auto* item = new QTreeWidgetItem(pierrotCat);
        item->setText(0, builtInNames[i]);
        item->setData(0, Qt::UserRole, builtInIds[i]);
    }
    pierrotCat->setExpanded(true);

    // ── Pierrot (efeitos nativos com ícones) ──────────────────────────
    auto* pierrotNativeCat = new QTreeWidgetItem(m_tree);
    pierrotNativeCat->setText(0, tr("Pierrot"));
    pierrotNativeCat->setFlags(pierrotNativeCat->flags() & ~Qt::ItemIsSelectable);
    pierrotNativeCat->setFont(0, bold);

    struct NativeEffect { QString icon; QString name; QString id; };
    const QVector<NativeEffect> nativeEffects = {
        { QStringLiteral(":/fx/lainka.ico"), tr("LAINKA"), QStringLiteral("pierrot_lainka") },
        { QStringLiteral(":/fx/motion.ico"), tr("MotiOn"), QStringLiteral("pierrot_motion") },
    };
    for (const auto& fx : nativeEffects) {
        auto* item = new QTreeWidgetItem(pierrotNativeCat);
        item->setText(0, fx.name);
        item->setData(0, Qt::UserRole, fx.id);
        item->setIcon(0, QIcon(fx.icon));
    }
    pierrotNativeCat->setExpanded(true);

    // ── OFX (Terceiros) ──────────────────────────────────────────────
    auto* ofxCat = new QTreeWidgetItem(m_tree);
    ofxCat->setText(0, tr("OFX (Terceiros)"));
    ofxCat->setFlags(ofxCat->flags() & ~Qt::ItemIsSelectable);
    ofxCat->setFont(0, bold);

    if (m_ofxPlugins.isEmpty()) {
        auto* noPlugins = new QTreeWidgetItem(ofxCat);
        noPlugins->setText(0, tr("(nenhum plugin encontrado)"));
        noPlugins->setFlags(noPlugins->flags() & ~Qt::ItemIsSelectable);
        noPlugins->setForeground(0, QColor(120, 120, 120));
    } else {
        for (const OfxPluginInfo& p : m_ofxPlugins) {
            auto* item = new QTreeWidgetItem(ofxCat);
            item->setText(0, p.name.isEmpty() ? p.id : p.name);
            item->setData(0, Qt::UserRole, p.id);
            item->setToolTip(0, p.description.isEmpty() ? p.id : p.description);
        }
    }
    ofxCat->setExpanded(true);
}

// ── Preview ──────────────────────────────────────────────────────────────

void EffectsWidget::updatePreview(const QString& effectId)
{
    m_previewIcon->show();
    m_previewName->show();

    if (effectId.isEmpty()) {
        m_previewIcon->clear();
        m_previewName->clear();
        m_previewDesc->setText(tr("Selecione um efeito para ver os detalhes."));
        return;
    }

    static const QHash<QString, QString> fxIcons = {
        { QStringLiteral("pierrot_lainka"), QStringLiteral(":/fx/lainka.ico") },
        { QStringLiteral("pierrot_motion"), QStringLiteral(":/fx/motion.ico") },
    };

    const QString iconPath = fxIcons.value(effectId);
    if (!iconPath.isEmpty()) {
        m_previewIcon->setPixmap(QIcon(iconPath).pixmap(64, 64));
    } else {
        m_previewIcon->setPixmap(QIcon::fromTheme(QStringLiteral("video-x-generic")).pixmap(64, 64));
    }

    static const QHash<QString, QString> displayNames = {
        { QStringLiteral("pierrot_brightness"), tr("Brilho") },
        { QStringLiteral("pierrot_contrast"), tr("Contraste") },
        { QStringLiteral("pierrot_saturation"), tr("Saturação") },
        { QStringLiteral("pierrot_blur"), tr("Desfoque") },
        { QStringLiteral("pierrot_grayscale"), tr("Preto e Branco") },
        { QStringLiteral("pierrot_chromakey"), tr("Chroma Key") },
        { QStringLiteral("pierrot_lainka"), tr("LAINKA") },
        { QStringLiteral("pierrot_motion"), tr("MotiOn") },
    };
    m_previewName->setText(displayNames.value(effectId, effectId));

    const auto descs = effectDescriptions();
    if (descs.contains(effectId)) {
        m_previewDesc->setText(descs.value(effectId));
    } else {
        for (const OfxPluginInfo& p : m_ofxPlugins) {
            if (p.id == effectId) {
                m_previewDesc->setText(p.description.isEmpty() ? p.id : p.description);
                return;
            }
        }
        m_previewDesc->setText(QString());
    }
}

void EffectsWidget::updateCategoryPreview(QTreeWidgetItem* category)
{
    if (!category) return;

    m_previewIcon->hide();
    m_previewName->hide();

    static const QHash<QString, QString> fxIcons = {
        { QStringLiteral("pierrot_lainka"), QStringLiteral(":/fx/lainka.ico") },
        { QStringLiteral("pierrot_motion"), QStringLiteral(":/fx/motion.ico") },
    };

    QString html;
    html += QStringLiteral("<style>"
        ".name { color:#dcddde; font-size:11px; text-align:center; }"
        ".name-gray { color:#8e9297; font-size:11px; text-align:center; }"
        "</style>"
        "<table cellpadding='4' cellspacing='0'>");

    int col = 0;
    for (int i = 0; i < category->childCount(); ++i) {
        auto* child = category->child(i);
        if (child->isHidden()) continue;

        const QString name = child->text(0);
        const QVariant v = child->data(0, Qt::UserRole);

        if (col == 0) html += QStringLiteral("<tr>");

        html += QStringLiteral("<td valign='top' align='center'>");

        if (v.isValid()) {
            const QString iconPath = fxIcons.value(v.toString());
            if (!iconPath.isEmpty()) {
                html += QStringLiteral("<img src='%1' width='56' height='56'><br><span class='name'>%2</span>")
                    .arg(iconPath, name);
            } else {
                html += QStringLiteral("<div style='width:56px;height:56px;background:#3a3d44;border-radius:6px;margin:0 auto;'></div><br><span class='name'>%1</span>")
                    .arg(name);
            }
        } else {
            html += QStringLiteral("<div style='width:56px;height:56px;background:#3a3d44;border-radius:6px;margin:0 auto;'></div><br><span class='name-gray'>%1</span>")
                .arg(name);
        }

        html += QStringLiteral("</td>");
        col++;
        if (col >= 3) {
            html += QStringLiteral("</tr>");
            col = 0;
        }
    }
    if (col > 0) html += QStringLiteral("</tr>");

    html += QStringLiteral("</table>");
    m_previewDesc->setText(html);
}

void EffectsWidget::onSelectionChanged()
{
    auto* item = m_tree->currentItem();
    if (!item) {
        m_previewIcon->show();
        m_previewName->show();
        m_previewIcon->clear();
        m_previewName->clear();
        m_previewDesc->setText(tr("Selecione um efeito para ver os detalhes."));
        return;
    }

    const QVariant v = item->data(0, Qt::UserRole);
    if (v.isValid()) {
        updatePreview(v.toString());
    } else {
        updateCategoryPreview(item);
    }
}

// ── Filtragem ────────────────────────────────────────────────────────────

void EffectsWidget::onSearchChanged(const QString& text)
{
    filterTree(text);
}

void EffectsWidget::filterTree(const QString& text)
{
    const QString query = text.trimmed().toLower();

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* cat = m_tree->topLevelItem(i);
        bool anyChildVisible = false;

        for (int j = 0; j < cat->childCount(); ++j) {
            auto* child = cat->child(j);
            const bool match = query.isEmpty() || child->text(0).toLower().contains(query);
            child->setHidden(!match);
            if (match) anyChildVisible = true;
        }

        const bool catMatch = query.isEmpty() || cat->text(0).toLower().contains(query);
        cat->setHidden(!catMatch && !anyChildVisible);
        if (catMatch || anyChildVisible) {
            cat->setExpanded(!query.isEmpty());
        }
    }
}

// ── Popula plugins OFX ───────────────────────────────────────────────────

void EffectsWidget::setOfxPlugins(const QVector<OfxPluginInfo>& plugins)
{
    m_ofxPlugins = plugins;
    buildTree();
}

// ── Clipe selecionado mudou ──────────────────────────────────────────────

void EffectsWidget::setSelectedClip(Clip* clip)
{
    m_currentClip = clip;
    m_tree->clearSelection();
    updatePreview(QString());
}

// ── Click na árvore ──────────────────────────────────────────────────────

void EffectsWidget::onTreeItemClicked()
{
    auto* item = m_tree->currentItem();
    if (!item) return;
    const QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return;

    emit effectSelected(v.toString());
}

void EffectsWidget::onTreeItemDoubleClicked()
{
    auto* item = m_tree->currentItem();
    if (!item || !m_currentClip) return;
    const QVariant v = item->data(0, Qt::UserRole);
    if (!v.isValid()) return;

    emit effectSelected(v.toString());
}
