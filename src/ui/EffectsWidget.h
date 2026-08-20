// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QHash>

class QLineEdit;
class QLabel;
class Project;
struct Clip;
struct OfxPluginInfo;

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

signals:
    void effectSelected(const QString& effectId);

private slots:
    void onTreeItemClicked();
    void onTreeItemDoubleClicked();
    void onSearchChanged(const QString& text);
    void onSelectionChanged();

private:
    void buildTree();
    void filterTree(const QString& text);
    void updatePreview(const QString& effectId);
    void updateCategoryPreview(QTreeWidgetItem* category);
    static QHash<QString, QString> effectDescriptions();

    Project* m_project = nullptr;
    Clip* m_currentClip = nullptr;
    EffectTree* m_tree = nullptr;
    QLineEdit* m_searchBox = nullptr;
    QVector<OfxPluginInfo> m_ofxPlugins;

    // Preview panel
    QWidget* m_previewPanel = nullptr;
    QLabel* m_previewIcon = nullptr;
    QLabel* m_previewName = nullptr;
    QLabel* m_previewDesc = nullptr;
};
