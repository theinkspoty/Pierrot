// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QDialog>
#include <QStringList>
#include <QMap>

class QCheckBox;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QListWidget;
class QStackedWidget;
class QTreeWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // Configurações compartilhadas (lidas de qualquer lugar do app).
    static bool mkvWarningEnabled();
    static int  maxDecodeWidth();
    // Modo de exibição das miniaturas nos clipes da timeline:
    // 0 = todas (fatias contínuas), 1 = início e fim, 2 = nenhuma.
    static int  thumbMode();
    // Ao excluir, fecha o vão automaticamente (ripple). Desativável.
    static bool rippleDeleteEnabled();
    // Sensibilidade do arraste vertical no editor de curvas (1.0 = padrão).
    static double graphSensitivity();
    // Caminhos extras de plugins OFX configurados pelo usuário.
    static QStringList ofxSearchPaths();

    // Valores escolhidos no diálogo (aplicar depois do OK).
    bool autoSaveEnabled() const;
    int  autoSaveMinutes() const;
    bool mkvWarning() const;
    int  decodeWidth() const;

    // Avisa (uma vez por sessão) quando importa arquivos MKV experimentais.
    static void warnMkvIfNeeded(QWidget* parent, const QStringList& files);
protected:
    void accept() override;
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    void buildShortcutsPage();
    void refreshShortcutRow();
    QCheckBox* m_mkvWarn = nullptr;
    QCheckBox* m_autoSave = nullptr;
    QSpinBox*  m_autoInterval = nullptr;
    QComboBox* m_decodeWidth = nullptr;
    QComboBox* m_thumbMode = nullptr;
    QCheckBox* m_rippleDelete = nullptr;
    QDoubleSpinBox* m_graphSens = nullptr;
    // Navegação por categorias (sidebar estilo DaVinci).
    QListWidget*   m_catList = nullptr;
    QStackedWidget* m_stack = nullptr;
    // Editor de atalhos do teclado.
    QTreeWidget* m_shortcuts = nullptr;
    QMap<QString, QString> m_shortcutMap;
    QString m_recordId; // atalho sendo regravado
    // Configuração de caminhos OFX.
    QListWidget* m_ofxPaths = nullptr;
};
