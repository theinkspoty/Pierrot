// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "Theme.h"
#include <QApplication>
#include <QSettings>

static ThemeColors s_current;

static ThemeColors makeDarkPalette() {
    // Fidelidade Adobe Premiere Pro (tema escuro padrão), medida de
    // screenshots do app: chrome 100% neutro (#232323 painéis, #121212 área
    // de trilhos), texto #E1E1E1, e UM único acento — o azul Adobe #2680EB —
    // reservado para seleção, foco, playhead e valores numéricos. A cor vive
    // no conteúdo (clipes, waveforms, labels), nunca no chrome.
    ThemeColors c;
    c.window            = QColor(0x23, 0x23, 0x23);
    c.windowText        = QColor(0xE1, 0xE1, 0xE1);
    c.base              = QColor(0x1B, 0x1B, 0x1B);
    c.alternateBase     = QColor(0x20, 0x20, 0x20);
    c.text              = QColor(0xE1, 0xE1, 0xE1);
    c.button            = QColor(0x38, 0x38, 0x38);
    c.buttonText        = QColor(0xE1, 0xE1, 0xE1);
    c.brightText        = Qt::red;
    c.link              = QColor(0x26, 0x80, 0xEB);
    c.highlight         = QColor(0x26, 0x80, 0xEB);
    c.highlightedText   = Qt::white;
    c.toolTipBase       = QColor(0x2A, 0x2A, 0x2A);
    c.toolTipText       = QColor(0xE1, 0xE1, 0xE1);
    c.placeholderText   = QColor(0x7A, 0x7A, 0x7A);
    c.disabledText      = QColor(0x5C, 0x5C, 0x5C);
    c.disabledWindowText= QColor(0x5C, 0x5C, 0x5C);

    c.monitorBg         = QColor(0x0E, 0x0E, 0x0E); // nível 1: letterbox do monitor
    c.canvasBg          = QColor(0x00, 0x00, 0x00); // nível 2: o vídeo em si
    c.canvasBorder      = QColor(0x0A, 0x0A, 0x0A);
    c.monitorLabel      = QColor(0x9A, 0x9A, 0x9A);

    c.timelineBg        = QColor(0x18, 0x18, 0x18); // nível 3: timeline (entre monitor e painéis)
    c.timelineGrid      = QColor(0x23, 0x23, 0x23);
    c.rulerBg           = QColor(0x1A, 0x1A, 0x1A);
    c.rulerText         = QColor(0x9A, 0x9A, 0x9A);
    c.rulerTick         = QColor(0x3A, 0x3A, 0x3A);
    c.rulerTickMajor    = QColor(0x55, 0x55, 0x55);
    c.trackBg           = QColor(0x16, 0x16, 0x16);
    c.trackBgAlt        = QColor(0x13, 0x13, 0x13);
    c.trackBorder       = QColor(0x0A, 0x0A, 0x0A);
    c.trackLabelBg      = QColor(0x1E, 0x1E, 0x1E);
    c.trackLabelText    = QColor(0xD5, 0xD5, 0xD5);
    c.clipBg            = QColor(0x4A, 0x68, 0x99);   // azul-aço do clipe de vídeo
    c.clipBorder        = QColor(0x2E, 0x45, 0x68);
    c.clipBorderSelect  = QColor(0xF5, 0xF5, 0xF5);   // seleção = borda branca
    c.clipBorderSecondary = QColor(0xF5, 0xF5, 0xF5, 120);
    c.clipText          = QColor(0xEA, 0xEA, 0xEA);
    c.clipThumbBorder   = QColor(0x14, 0x14, 0x14);
    c.playhead          = QColor(0x26, 0x80, 0xEB);   // CTI azul Adobe
    c.playheadHandle    = QColor(0x26, 0x80, 0xEB);
    c.selectionRect     = QColor(0x26, 0x80, 0xEB, 60);
    c.selectionFill     = QColor(0x26, 0x80, 0xEB, 28);

    c.transportBg       = QColor(0x1E, 0x1E, 0x1E);
    c.transportBorder   = QColor(0x0F, 0x0F, 0x0F);

    c.dockTitleBg       = QColor(0x1E, 0x1E, 0x1E);
    c.dockTitleBgHover  = QColor(0x26, 0x26, 0x26);
    c.dockTitleText     = QColor(0xC8, 0xC8, 0xC8);
    c.dockBorder        = QColor(0x0C, 0x0C, 0x0C);
    c.dockCloseHover    = QColor(0xE8, 0x11, 0x23);   // vermelho estilo Adobe/SO

    c.inputBg           = QColor(0x1B, 0x1B, 0x1B);
    c.inputBorder       = QColor(0x3A, 0x3A, 0x3A);
    c.inputFocus        = QColor(0x26, 0x80, 0xEB);
    c.spinText          = QColor(0x4D, 0xA3, 0xFF);   // valores numéricos azuis

    c.btnPrimary        = QColor(0x26, 0x80, 0xEB);
    c.btnPrimaryText    = QColor(0xFF, 0xFF, 0xFF);
    c.btnHover          = QColor(0x3E, 0x3E, 0x3E);
    c.btnActive         = QColor(0x26, 0x80, 0xEB);

    c.accent            = QColor(0x26, 0x80, 0xEB);
    c.accentGold        = QColor(0xFF, 0xB3, 0x40);
    c.iconNormal        = QColor(0xB8, 0xB8, 0xB8);
    c.iconMuted         = QColor(0x6E, 0x6E, 0x6E);

    c.tabBg             = QColor(0x1A, 0x1A, 0x1A);
    c.tabSelected       = QColor(0x26, 0x26, 0x26);
    c.tabBorder         = QColor(0x26, 0x80, 0xEB);

    c.scrollbarBg       = QColor(0x12, 0x12, 0x12);
    c.scrollbarHandle   = QColor(0x4A, 0x4A, 0x4A);
    c.scrollbarHover    = QColor(0x60, 0x60, 0x60);

    c.welcomeBgTop      = QColor(0x1E, 0x1E, 0x20);
    c.welcomeBgBottom   = QColor(0x12, 0x12, 0x14);
    c.welcomeBtnGradStart = QColor(0x14, 0x73, 0xE6);
    c.welcomeBtnGradEnd   = QColor(0x26, 0x8C, 0xF5);

    c.expressBg         = QColor(0x23, 0x23, 0x23);
    c.expressCardBg     = QColor(0x2A, 0x2A, 0x2A);
    c.expressDescText   = QColor(0x9A, 0x9A, 0x9A);
    c.effectsSearchBg   = QColor(0x1B, 0x1B, 0x1B);
    c.effectsTreeBg     = QColor(0x1E, 0x1E, 0x1E);
    c.effectsPreviewBg  = QColor(0x12, 0x12, 0x12);

    c.graphBg           = QColor(0x16, 0x16, 0x16);
    c.graphGrid         = QColor(0x26, 0x26, 0x26);
    c.graphLine         = QColor(0x26, 0x80, 0xEB);
    c.graphKeyframe     = QColor(0xC4, 0xC8, 0xD2);
    c.graphLabel        = QColor(0x9A, 0x9A, 0x9A);
    c.graphRuler        = QColor(0x1A, 0x1A, 0x1A);
    c.graphAxis         = QColor(0x3A, 0x3A, 0x3A);
    c.graphRulerText    = QColor(0x96, 0x99, 0xA0);
    c.graphHandle       = QColor(0x26, 0x80, 0xEB);

    c.pancropBg         = QColor(0x16, 0x16, 0x16);
    c.pancropRegion     = QColor(0x26, 0x80, 0xEB, 60);
    c.pancropHandle     = QColor(0x26, 0x80, 0xEB);

    c.sectionDivider    = QColor(0x0A, 0x0A, 0x0A);   // divisória quase invisível

    return c;
}

static ThemeColors makeLightPalette() {
    ThemeColors c;
    c.window            = QColor(195, 195, 198);
    c.windowText        = QColor(30, 30, 30);
    c.base              = QColor(225, 225, 228);
    c.alternateBase     = QColor(215, 215, 218);
    c.text              = QColor(30, 30, 30);
    c.button            = QColor(185, 185, 190);
    c.buttonText        = QColor(30, 30, 30);
    c.brightText        = QColor(220, 50, 50);
    c.link              = QColor(0, 110, 235);
    c.highlight         = QColor(0, 110, 235);
    c.highlightedText   = Qt::white;
    c.toolTipBase       = QColor(215, 215, 218);
    c.toolTipText       = QColor(30, 30, 30);
    c.placeholderText   = QColor(125, 125, 130);
    c.disabledText      = QColor(140, 140, 145);
    c.disabledWindowText= QColor(140, 140, 145);

    c.monitorBg         = QColor(42, 42, 44);
    c.canvasBg          = QColor(0, 0, 0);
    c.canvasBorder      = QColor(65, 65, 68);
    c.monitorLabel      = QColor(150, 150, 155);

    c.timelineBg        = QColor(180, 180, 184);
    c.timelineGrid      = QColor(160, 160, 165);
    c.rulerBg           = QColor(172, 172, 176);
    c.rulerText         = QColor(75, 75, 80);
    c.rulerTick         = QColor(152, 152, 157);
    c.rulerTickMajor    = QColor(125, 125, 130);
    c.trackBg           = QColor(192, 192, 196);
    c.trackBgAlt        = QColor(186, 186, 190);
    c.trackBorder       = QColor(162, 162, 167);
    c.trackLabelBg      = QColor(172, 172, 176);
    c.trackLabelText    = QColor(60, 60, 65);
    c.clipBg            = QColor(130, 168, 210);
    c.clipBorder        = QColor(112, 150, 192);
    c.clipBorderSelect  = QColor(0, 100, 225);
    c.clipBorderSecondary = QColor(0, 100, 225, 120);
    c.clipText          = QColor(10, 10, 10);
    c.clipThumbBorder   = QColor(158, 158, 163);
    c.playhead          = QColor(230, 158, 24);
    c.playheadHandle    = QColor(235, 176, 54);
    c.selectionRect     = QColor(0, 100, 225, 40);
    c.selectionFill     = QColor(0, 100, 225, 18);

    c.transportBg       = QColor(180, 180, 184);
    c.transportBorder   = QColor(160, 160, 165);

    c.dockTitleBg       = QColor(180, 180, 184);
    c.dockTitleBgHover  = QColor(170, 170, 175);
    c.dockTitleText     = QColor(35, 35, 40);
    c.dockBorder        = QColor(160, 160, 165);
    c.dockCloseHover    = QColor(200, 70, 70);

    c.inputBg           = QColor(225, 225, 228);
    c.inputBorder       = QColor(152, 152, 157);
    c.inputFocus        = QColor(0, 100, 225);
    c.spinText          = QColor(0, 82, 185);

    c.btnPrimary        = QColor(0, 100, 225);
    c.btnPrimaryText    = Qt::white;
    c.btnHover          = QColor(185, 185, 195);
    c.btnActive         = QColor(0, 82, 200);

    c.accent            = QColor(0, 100, 225);
    c.accentGold        = QColor(230, 142, 10);
    c.iconNormal        = QColor(60, 60, 65);
    c.iconMuted         = QColor(130, 130, 135);

    c.tabBg             = QColor(188, 188, 192);
    c.tabSelected       = QColor(225, 225, 228);
    c.tabBorder         = QColor(0, 100, 225);

    c.scrollbarBg       = QColor(205, 205, 208);
    c.scrollbarHandle   = QColor(150, 150, 155);
    c.scrollbarHover    = QColor(125, 125, 130);

    c.welcomeBgTop      = QColor(195, 195, 200);
    c.welcomeBgBottom   = QColor(165, 165, 170);
    c.welcomeBtnGradStart = QColor(0, 100, 225);
    c.welcomeBtnGradEnd   = QColor(25, 130, 245);

    c.expressBg         = QColor(188, 188, 192);
    c.expressCardBg     = QColor(215, 215, 218);
    c.expressDescText   = QColor(92, 92, 97);
    c.effectsSearchBg   = QColor(225, 225, 228);
    c.effectsTreeBg     = QColor(210, 210, 214);
    c.effectsPreviewBg  = QColor(165, 165, 170);

    c.graphBg           = QColor(210, 210, 214);
    c.graphGrid         = QColor(178, 178, 183);
    c.graphLine         = QColor(0, 110, 225);
    c.graphKeyframe     = QColor(125, 125, 130);
    c.graphLabel        = QColor(75, 75, 80);
    c.graphRuler        = QColor(176, 176, 181);
    c.graphAxis         = QColor(120, 120, 126);
    c.graphRulerText    = QColor(90, 90, 96);
    c.graphHandle       = QColor(0, 110, 225);

    c.pancropBg         = QColor(165, 165, 170);
    c.pancropRegion     = QColor(0, 100, 225, 50);
    c.pancropHandle     = QColor(0, 82, 185);

    c.sectionDivider    = QColor(0, 100, 225);

    return c;
}

const ThemeColors& themeColors() {
    return s_current;
}

void applyAppPalette(QApplication* app, AppTheme theme) {
    s_current = (theme == AppTheme::Light) ? makeLightPalette() : makeDarkPalette();
    const auto& c = s_current;

    QPalette pal;
    pal.setColor(QPalette::Window, c.window);
    pal.setColor(QPalette::WindowText, c.windowText);
    pal.setColor(QPalette::Base, c.base);
    pal.setColor(QPalette::AlternateBase, c.alternateBase);
    pal.setColor(QPalette::Text, c.text);
    pal.setColor(QPalette::Button, c.button);
    pal.setColor(QPalette::ButtonText, c.buttonText);
    pal.setColor(QPalette::BrightText, c.brightText);
    pal.setColor(QPalette::Link, c.link);
    pal.setColor(QPalette::Highlight, c.highlight);
    pal.setColor(QPalette::HighlightedText, c.highlightedText);
    pal.setColor(QPalette::ToolTipBase, c.toolTipBase);
    pal.setColor(QPalette::ToolTipText, c.toolTipText);
    pal.setColor(QPalette::PlaceholderText, c.placeholderText);
    pal.setColor(QPalette::Disabled, QPalette::Text, c.disabledText);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, c.disabledWindowText);
    app->setPalette(pal);
}

AppTheme savedTheme() {
    QSettings s;
    return s.value("theme", 0).toInt() == 1 ? AppTheme::Light : AppTheme::Dark;
}

void saveTheme(AppTheme theme) {
    QSettings s;
    s.setValue("theme", theme == AppTheme::Light ? 1 : 0);
}

QString globalStyleSheet(AppTheme theme) {
    const auto& c = (theme == AppTheme::Light) ? makeLightPalette() : makeDarkPalette();
    return QStringLiteral(R"(
        QDockWidget {
            color: %1;
        }
        QDockWidget::title {
            background-color: %2;
            border-bottom: 1px solid %3;
            padding: 2px 4px 2px 8px;
            spacing: 4px;
        }
        QDockWidget::title:hover {
            background-color: %5;
        }
        QDockWidget::float-button,
        QDockWidget::close-button {
            background: transparent;
            border: none;
            padding: 1px;
            icon-size: 12px;
            subcontrol-position: top right;
        }
        QDockWidget::float-button:hover,
        QDockWidget::close-button:hover {
            background-color: %6;
            border-radius: 3px;
        }
    )").arg(c.dockTitleText.name(),
            c.dockTitleBg.name(),
            c.dockBorder.name(),
            c.dockTitleBg.name(),
            c.dockTitleBgHover.name(),
            theme == AppTheme::Light ? QStringLiteral("#e0b0b0") : c.dockCloseHover.name());
}

QString flatControlStyleSheet(AppTheme theme) {
    const auto& c = (theme == AppTheme::Light) ? makeLightPalette() : makeDarkPalette();
    const QString win      = c.window.name();
    const QString base     = c.base.name();
    const QString alt      = c.alternateBase.name();
    const QString txt      = c.windowText.name();
    const QString text     = c.text.name();
    const QString btn      = c.button.name();
    const QString btntxt   = c.buttonText.name();
    const QString hover    = (theme == AppTheme::Light) ? QColor(200, 200, 205).name() : QColor(58, 62, 72).name();
    const QString press    = (theme == AppTheme::Light) ? QColor(170, 172, 178).name() : QColor(49, 50, 54).name();
    const QString brd      = c.trackBorder.name();
    const QString input    = c.inputBg.name();
    const QString inputbrd = c.inputBorder.name();
    const QString focus    = c.inputFocus.name();
    const QString accent   = c.accent.name();
    const QString primary  = c.btnPrimary.name();
    const QString primaryTxt = c.btnPrimaryText.name();
    const QString hl       = c.highlight.name();
    const QString hltxt    = c.highlightedText.name();
    const QString disabled = c.disabledText.name();
    const QString tooltip  = c.toolTipBase.name();
    const QString tiptext  = c.toolTipText.name();
    const QString titleTxt = c.trackLabelText.name();
    const QString scBg     = c.scrollbarBg.name();
    const QString scH      = c.scrollbarHandle.name();
    const QString scHo     = c.scrollbarHover.name();
    const QString tabSel   = c.tabSelected.name();
    const QString btnPrimaryTxt = c.btnPrimaryText.name();
    // Foco neutro (Premiere): sem borda ciano em hover/focus de inputs.
    const QString fneut = (theme == AppTheme::Light) ? QColor(180, 182, 188).name()
                                                     : QColor(92, 96, 104).name();
    // Divisória entre painéis: quase invisível, como no Premiere.
    const QString div   = c.dockBorder.name();

    return QStringLiteral(R"(
        QMenuBar {
            background: %1; color: %2;
            border-bottom: 1px solid %4;
            padding: 1px;
        }
        QMenuBar::item { background: transparent; padding: 4px 10px; border-radius: 2px; }
        QMenuBar::item:selected { background: %5; border-radius: 2px; }

        QMenu {
            background: %3; color: %2;
            border: 1px solid %6; padding: 4px;
        }
        QMenu::item {
            background: transparent; padding: 5px 22px 5px 12px;
            border-radius: 2px;
        }
        QMenu::item:selected { background: %11; color: %12; }
        QMenu::item:disabled { color: %13; }
        QMenu::separator { height: 1px; background: %6; margin: 4px 6px; }
        QMenu::icon { padding-left: 6px; }

        QToolBar { background: %1; border: none; spacing: 3px; padding: 2px; }

        QStatusBar {
            background: %1; color: %2;
            border-top: 1px solid %6;
        }

        QToolTip {
            background: %17; color: %18;
            border: 1px solid %6; padding: 3px 6px;
        }

        QPushButton {
            background: %7; color: %8;
            border: 1px solid %10; border-radius: 2px;
            padding: 3px 10px;
        }
        QPushButton:hover { background: %5; }
        QPushButton:pressed { background: %14; }
        QPushButton:checked { background: %14; border-color: %27; }
        QPushButton:disabled { color: %13; background: %3; }
        QPushButton[primary="true"],
        QPushButton#primary {
            background: %15; color: %19;
            border: 1px solid %15;
            font-weight: 600;
        }
        QPushButton[primary="true"]:hover,
        QPushButton#primary:hover { background: %20; border-color: %20; }

        QToolButton {
            background: transparent; color: %2;
            border: 1px solid transparent; border-radius: 2px;
            padding: 2px;
        }
        QToolButton:hover { background: %5; border-color: %6; }
        QToolButton:pressed,
        QToolButton:checked {
            background: %9; border-color: %27;
        }
        QToolButton:disabled { color: %13; }

        QComboBox {
            background: %16; color: %2;
            border: 1px solid %10; border-radius: 2px;
            padding: 3px 8px;
        }
        QComboBox:hover, QComboBox:focus { border-color: %27; }
        QComboBox:disabled { color: %13; background: %3; }
        QComboBox::drop-down {
            subcontrol-origin: padding; subcontrol-position: center right;
            width: 20px; border: none; border-left: 1px solid %10;
        }
        QComboBox::down-arrow {
            image: url(:/icons/arrow-down.png); width: 12px; height: 12px;
        }
        QComboBox QAbstractItemView {
            background: %3; color: %2;
            border: 1px solid %6;
            selection-background-color: %11; selection-color: %12;
            outline: 0;
        }

        QLineEdit, QSpinBox, QDoubleSpinBox, QDateTimeEdit, QDateEdit, QTimeEdit {
            background: %16; color: %2;
            border: 1px solid %10; border-radius: 2px;
            padding: 3px 6px;
            selection-background-color: %11; selection-color: %12;
        }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus,
        QDateTimeEdit:focus, QDateEdit:focus, QTimeEdit:focus {
            border-color: %27;
        }
        QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
            color: %13; background: %3;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            width: 16px; border: none; background: transparent;
        }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
            image: url(:/icons/arrow-up.png); width: 11px; height: 11px;
        }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
            image: url(:/icons/arrow-down.png); width: 11px; height: 11px;
        }

        QCheckBox, QRadioButton {
            background: transparent; color: %2; spacing: 6px;
        }
        QCheckBox::indicator, QRadioButton::indicator {
            width: 14px; height: 14px;
            background: %16; border: 1px solid %10; border-radius: 2px;
        }
        QRadioButton::indicator { border-radius: 7px; }
        QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: %27; }
        QCheckBox::indicator:checked {
            background: %21; border-color: %21;
            image: url(:/icons/check.png);
        }
        QRadioButton::indicator:checked {
            background: %21; border-color: %21;
            image: url(:/icons/radio.png);
        }
        QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {
            background: %3; border-color: %6;
        }

        QGroupBox { border: 1px solid %6; border-radius: 3px; margin-top: 8px; }
        QGroupBox::title {
            subcontrol-origin: margin; left: 8px; padding: 0 4px;
            color: %22; background: transparent;
        }

        QTabWidget::pane { border: 1px solid %6; top: -1px; }
        QTabBar::tab {
            background: transparent; color: %2;
            border: 1px solid transparent;
            padding: 5px 12px;
        }
        QTabBar::tab:selected {
            background: %29; color: #FFFFFF;
        }
        QTabBar::tab:hover:!selected { background: %5; }

        QHeaderView::section {
            background: %1; color: %22;
            border: none; border-right: 1px solid %6; border-bottom: 1px solid %6;
            padding: 4px 8px;
        }

        QTreeView, QListView, QTableView, QTableWidget, QListWidget {
            background: %3; color: %2;
            alternate-background-color: %23;
            border: 1px solid %6; outline: 0;
        }
        QTreeView::item:hover, QListView::item:hover,
        QTableView::item:hover, QListWidget::item:hover { background: %5; }
        QTreeView::item:selected, QListView::item:selected,
        QTableView::item:selected, QListWidget::item:selected {
            background: %11; color: %12;
        }

        QScrollBar:vertical { background: %24; width: 10px; margin: 0; }
        QScrollBar::handle:vertical {
            background: %25; min-height: 24px; border-radius: 2px; margin: 1px;
        }
        QScrollBar::handle:vertical:hover { background: %26; }
        QScrollBar:horizontal { background: %24; height: 10px; margin: 0; }
        QScrollBar::handle:horizontal {
            background: %25; min-width: 24px; border-radius: 2px; margin: 1px;
        }
        QScrollBar::handle:horizontal:hover { background: %26; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

        QSplitter::handle { background: %28; }
        QSplitter::handle:horizontal { width: 3px; }
        QSplitter::handle:vertical { height: 3px; }

        QProgressBar {
            border: 1px solid %6; border-radius: 2px;
            background: %3; color: %2; text-align: center;
        }
        QProgressBar::chunk { background: %15; }

        QDialog, QMessageBox { background: %1; }

        QTextEdit, QPlainTextEdit {
            background: %16; color: %2;
            border: 1px solid %10; border-radius: 2px;
            selection-background-color: %11; selection-color: %12;
        }
    )")
.arg(win, txt, base, brd, hover, brd,
     btn, btntxt, press, inputbrd,
     hl, hltxt, disabled, press, primary,
     input, tooltip, tiptext, btnPrimaryTxt, focus,
     accent, titleTxt, alt, scBg, scH, scHo,
     fneut, div, tabSel);
}
