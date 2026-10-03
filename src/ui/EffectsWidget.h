// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Painel Effects — réplica da estrutura do Adobe Premiere Pro:
// busca no topo + uma árvore única com Presets, Audio Effects,
// Audio Transitions, Video Effects (por categoria), Video Transitions
// e Text/Animation. IDs: pierrot_* | frei0r:<nome> | <ofxId> |
// trans:<tipo> | text:<ação>.

#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QHash>

class QLineEdit;
class Project;
struct Clip;
struct OfxPluginInfo;

#include "colombina/frei0r/Frei0rPluginManager.h"

static inline const char* const kMimeEffect = "application/x-pierrot-effect";

class EffectTree : public QTreeWidget {
    Q_OBJECT
public:
    using QTreeWidget::QTreeWidget;
protected:
    void startDrag(Qt::DropActions supportedActions) override;
};

class EffectsWidget : public QWidget {
    Q_OBJECT
public:
    explicit EffectsWidget(QWidget* parent = nullptr);

    void setProject(Project* p) { m_project = p; }
    void setSelectedClip(Clip* clip);
    void setOfxPlugins(const QVector<OfxPluginInfo>& plugins);
    void setFrei0rPlugins(const QVector<Frei0rPluginInfo>& plugins);

signals:
    // effectId: pierrot_* | frei0r:* | <ofxId> | trans:* | text:*
    void effectSelected(const QString& effectId);

private slots:
    void onItemActivated();
    void onSearchChanged(const QString& text);

private:
    void buildTree();
    void filterTree(const QString& text);
    static QTreeWidgetItem* addFolder(QTreeWidget* tree, QTreeWidgetItem* parent,
                                      const QString& title);
    static void addEffectItem(QTreeWidgetItem* parent, const QString& title,
                              const QString& id, const QString& tip = QString());

    Project* m_project = nullptr;
    Clip* m_currentClip = nullptr;
    EffectTree* m_tree = nullptr;
    QLineEdit* m_searchBox = nullptr;
    QVector<OfxPluginInfo> m_ofxPlugins;
    QVector<Frei0rPluginInfo> m_frei0rPlugins;
};
