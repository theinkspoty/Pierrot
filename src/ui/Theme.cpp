// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "Theme.h"
#include <QApplication>
#include <QSettings>

static ThemeColors s_current;

static ThemeColors makeDarkPalette() {
    ThemeColors c;
    c.window            = QColor(33, 33, 35);
    c.windowText        = QColor(220, 220, 220);
    c.base              = QColor(38, 38, 41);
    c.alternateBase     = QColor(43, 43, 46);
    c.text              = QColor(220, 220, 220);
    c.button            = QColor(58, 58, 61);
    c.buttonText        = QColor(230, 230, 230);
    c.brightText        = Qt::red;
    c.link              = QColor(80, 160, 255);
    c.highlight         = QColor(0, 120, 215);
    c.highlightedText   = Qt::white;
    c.toolTipBase       = QColor(60, 60, 62);
    c.toolTipText       = QColor(230, 230, 230);
    c.placeholderText   = QColor(120, 120, 125);
    c.disabledText      = QColor(100, 100, 105);
    c.disabledWindowText= QColor(100, 100, 105);

    c.monitorBg         = QColor(13, 13, 16);
    c.canvasBg          = QColor(8, 8, 10);
    c.canvasBorder      = QColor(70, 70, 78);
    c.monitorLabel      = QColor(175, 175, 185);

    c.timelineBg        = QColor(24, 24, 27);
    c.timelineGrid      = QColor(55, 55, 60);
    c.rulerBg           = QColor(28, 28, 31);
    c.rulerText         = QColor(160, 160, 170);
    c.rulerTick         = QColor(55, 55, 60);
    c.rulerTickMajor    = QColor(80, 80, 88);
    c.trackBg           = QColor(41, 41, 45);
    c.trackBgAlt        = QColor(33, 33, 37);
    c.trackBorder       = QColor(55, 55, 60);
    c.trackLabelBg      = QColor(30, 30, 33);
    c.trackLabelText    = QColor(160, 160, 170);
    c.clipBg            = QColor(55, 80, 110);
    c.clipBorder        = QColor(70, 95, 125);
    c.clipBorderSelect  = QColor(147, 195, 255);
    c.clipBorderSecondary = QColor(147, 195, 255, 110);
    c.clipText          = QColor(230, 230, 235);
    c.clipThumbBorder   = QColor(40, 40, 45);
    c.playhead          = QColor(255, 166, 38);
    c.playheadHandle    = QColor(255, 187, 90);
    c.selectionRect     = QColor(0, 120, 215, 60);
    c.selectionFill     = QColor(0, 120, 215, 30);

    c.transportBg       = QColor(26, 28, 34);
    c.transportBorder   = QColor(42, 45, 52);

    c.dockTitleBg       = QColor(45, 47, 52);
    c.dockTitleBgHover  = QColor(51, 54, 60);
    c.dockTitleText     = QColor(212, 216, 222);
    c.dockBorder        = QColor(21, 22, 25);
    c.dockCloseHover    = QColor(90, 28, 28);

    c.inputBg           = QColor(31, 31, 33);
    c.inputBorder       = QColor(62, 62, 66);
    c.inputFocus        = QColor(0, 120, 215);
    c.spinText          = QColor(156, 196, 240);

    c.btnPrimary        = QColor(47, 111, 179);
    c.btnPrimaryText    = QColor(230, 230, 230);
    c.btnHover          = QColor(55, 60, 68);
    c.btnActive         = QColor(0, 100, 190);

    c.accent            = QColor(80, 160, 255);
    c.accentGold        = QColor(255, 179, 64);
    c.iconNormal        = QColor(160, 160, 170);
    c.iconMuted         = QColor(95, 103, 114);

    c.tabBg             = QColor(44, 46, 51);
    c.tabSelected       = QColor(38, 40, 44);
    c.tabBorder         = QColor(59, 89, 152);

    c.scrollbarBg       = QColor(30, 30, 33);
    c.scrollbarHandle   = QColor(70, 70, 75);
    c.scrollbarHover    = QColor(90, 90, 95);

    c.welcomeBgTop      = QColor(38, 40, 46);
    c.welcomeBgBottom   = QColor(21, 22, 26);
    c.welcomeBtnGradStart = QColor(47, 111, 179);
    c.welcomeBtnGradEnd   = QColor(61, 143, 212);

    c.expressBg         = QColor(43, 45, 49);
    c.expressCardBg     = QColor(54, 57, 63);
    c.expressDescText   = QColor(95, 103, 114);
    c.effectsSearchBg   = QColor(30, 31, 34);
    c.effectsTreeBg     = QColor(43, 45, 49);
    c.effectsPreviewBg  = QColor(22, 24, 28);

    c.graphBg           = QColor(25, 27, 31);
    c.graphGrid         = QColor(45, 47, 52);
    c.graphLine         = QColor(80, 160, 255);
    c.graphKeyframe     = QColor(255, 179, 64);
    c.graphLabel        = QColor(138, 154, 170);

    c.pancropBg         = QColor(21, 22, 26);
    c.pancropRegion     = QColor(0, 120, 215, 80);
    c.pancropHandle     = QColor(156, 196, 240);

    c.sectionDivider    = QColor(80, 120, 180);

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
    c.graphLine         = QColor(0, 100, 225);
    c.graphKeyframe     = QColor(230, 142, 10);
    c.graphLabel        = QColor(75, 75, 80);

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
            border-top: 1px solid %4;
            padding: 4px 5px 4px 10px;
            spacing: 4px;
        }
        QDockWidget::title:hover {
            background-color: %5;
        }
        QDockWidget::float-button,
        QDockWidget::close-button {
            background: transparent;
            border: none;
            padding: 2px;
            icon-size: 14px;
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
