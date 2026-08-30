// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QHash>
#include <QSet>

class QTabWidget;
class QScrollArea;
class QLabel;
class QDragEnterEvent;
class QDropEvent;
class Project;
struct Clip;
struct OfxPluginInfo;

#include "ofx/OfxPluginManager.h"

// Janela docável "Express" — editor de parâmetros de efeitos do clipe.
// Cada efeito aplicado ao clipe vira uma aba nesta janela.
class ExpressWidget : public QWidget {
    Q_OBJECT
public:
    explicit ExpressWidget(QWidget* parent = nullptr);

    void setProject(Project* p) { m_project = p; }
    void setOfxPlugins(const QVector<OfxPluginInfo>& plugins);

    // Registra parâmetros descobertos pelo describe (chamado pelo manager).
    void setOfxParamDefs(const QString& pluginId,
                         const QVector<OfxParamDefInfo>& params);

    // Chamado quando a seleção de clipes muda na timeline.
    void setSelectedClip(Clip* clip);

    // Adiciona um efeito ao clipe atual e cria/ativa a aba.
    void addEffect(const QString& effectId);

signals:
    void modified();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private slots:
    void onTabCloseRequested(int index);

private:
    void rebuildTabs();
    void createBuiltInTab(const QString& effectId);
    void createOfxTab(const QString& pluginId);
    void removeEffectFromClip(const QString& effectId);

    void applyBuiltInValue(const QString& key, double value);
    void applyBuiltInBool(const QString& key, bool value);

    Project* m_project = nullptr;
    Clip* m_currentClip = nullptr;
    QTabWidget* m_tabs = nullptr;
    QLabel* m_emptyLabel = nullptr;

    QVector<OfxPluginInfo> m_ofxPlugins;

    // Efeitos atualmente abertos como abas (effectId -> tab index).
    QHash<QString, int> m_tabMap;
    // Cache de widgets de abas (para não recriar).
    QHash<QString, QWidget*> m_tabPages;
    // Efeitos nativos que já foram aplicados ao clipe (para não duplicar).
    QSet<QString> m_appliedBuiltIn;

    // Parâmetros OFX descobertos (pluginId -> lista de definições completas).
    QHash<QString, QVector<OfxParamDefInfo>> m_ofxParamDefs;

    // Widgets de parâmetros OFX (para leitura dos valores).
    QHash<QString, QWidget*> m_ofxParamWidgets;
};
