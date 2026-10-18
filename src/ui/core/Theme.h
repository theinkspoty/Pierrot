// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QColor>
#include <QPalette>
#include <QString>
#include <QStringList>

// ── Tema global ─────────────────────────────────────────────────
// Paleta compartilhada para todo o app. Estilo Final Cut / Apple:
// tons de cinza limpos, sem saturação, contraste suave.

// Os valores sao o indice no combo de Ajustes>Interface e o valor gravado
// em QSettings. Adicionar um tema = acrescentar aqui E no combo, nessa ordem.
enum class AppTheme { Dark = 0, Light = 1, RidgeRacer = 2, Japan95 = 3 };

struct ThemeColors {
    // ── Janela / fundo ──────────────────────────────────────────
    QColor window;            // fundo principal de janelas/dialogs
    QColor windowText;        // texto sobre window
    QColor base;              // fundo de listas, edits, views
    QColor alternateBase;     // linhas alternadas em listas
    QColor text;              // texto sobre base
    QColor button;            // fundo de botões
    QColor buttonText;        // texto sobre button
    QColor brightText;        // texto de destaque (vermelho)
    QColor link;              // links
    QColor highlight;         // seleção
    QColor highlightedText;   // texto sobre seleção
    QColor toolTipBase;       // fundo tooltip
    QColor toolTipText;       // texto tooltip
    QColor placeholderText;   // texto placeholder
    QColor disabledText;      // texto desabilitado
    QColor disabledWindowText;// texto desabilitado em janelas

    // ── Monitor / Preview ───────────────────────────────────────
    QColor monitorBg;         // fundo do monitor (ao redor do canvas)
    QColor canvasBg;          // fundo do canvas (onde o vídeo é desenhado)
    QColor canvasBorder;      // borda do canvas
    QColor monitorLabel;      // label de resolução/fps

    // ── Timeline ────────────────────────────────────────────────
    QColor timelineBg;        // fundo da timeline
    QColor timelineGrid;      // linhas de grade verticais
    QColor rulerBg;           // fundo da régua
    QColor rulerText;         // texto da régua
    QColor rulerTick;         // ticks menores da régua
    QColor rulerTickMajor;    // ticks maiores da régua
    QColor trackBg;           // fundo da faixa
    QColor trackBgAlt;        // fundo da faixa alternada
    QColor trackBorder;       // borda entre faixas
    QColor trackLabelBg;      // fundo do label da faixa
    QColor trackLabelText;    // texto do label da faixa
    QColor clipBg;            // fundo do clipe
    QColor clipBorder;        // borda do clipe
    QColor clipBorderSelect;  // borda do clipe selecionado (primário)
    QColor clipBorderSecondary; // borda do clipe selecionado (secundário)
    QColor clipText;          // texto do clipe
    QColor clipThumbBorder;   // borda entre thumbnails
    QColor playhead;          // linha do playhead
    QColor playheadHandle;    // cabeça do playhead
    QColor selectionRect;     // retângulo de seleção marquee
    QColor selectionFill;     // preenchimento do marquee

    // ── Transport bar ───────────────────────────────────────────
    QColor transportBg;       // fundo da barra de transporte
    QColor transportBorder;   // borda superior da barra

    // ── Dock widgets ────────────────────────────────────────────
    QColor dockTitleBg;       // fundo do título do dock
    QColor dockTitleBgHover;  // hover no título
    QColor dockTitleText;     // texto do título do dock
    QColor dockBorder;        // borda do dock
    QColor dockCloseHover;    // hover do botão fechar

    // ── Inputs / Controles ──────────────────────────────────────
    QColor inputBg;           // fundo de edits, spins, combos
    QColor inputBorder;       // borda de inputs
    QColor inputFocus;        // borda focada
    QColor inputFocusNeutral; // foco SEM cor (estilo Premiere, sem ciano).
                             // Era hardcoded por ternario Light/Dark, o que
                             // deixaria um 3o tema cair no ramo errado.
    QColor ctrlHover;         // hover de botao no stylesheet global. Slots
                             // DEDICADOS de proposito: btnHover/btnActive
                             // nao servem (o btnActive do dark e o azul
                             // Adobe, nao o cinza de "pressionado").
    QColor ctrlPress;         // estado pressionado
    QColor toolBtnBg;         // fundo do botao de ferramenta no toolbar
    QColor toolBtnHover;      // hover do QToolBar vertical (ferramentas da
                             // timeline). Eram derivados de canvasBorder+alpha
                             // no MainWindow, o que impedia o tema de controlar.
    QColor toolBtnChecked;    // ferramenta ativa/checked no toolbar
    QColor spinText;          // texto dos spins

    // ── Botões ──────────────────────────────────────────────────
    QColor btnPrimary;        // botão primário (ação principal)
    QColor btnPrimaryText;    // texto do primário
    QColor btnHover;          // hover de botões
    QColor btnActive;         // estado ativo/pressed

    // ── Ícones / Accent ─────────────────────────────────────────
    QColor accent;            // cor de destaque (links, bordas ativas)
    QColor accentGold;        // dourado (clipes ativos, LAINKA)
    QColor iconNormal;        // cor padrão de ícones
    QColor iconMuted;         // cor de ícones secundários

    // ── Tab bar ─────────────────────────────────────────────────
    QColor tabBg;             // fundo das tabs
    QColor tabSelected;       // tab selecionada
    QColor tabSelectedText;   // texto da aba selecionada (era #FFFFFF fixo,
                              // ilegível em temas claros)
    QColor tabBorder;         // borda inferior da tab ativa

    // ── Scrollbar ───────────────────────────────────────────────
    QColor scrollbarBg;       // fundo da scrollbar
    QColor scrollbarHandle;   // alça da scrollbar
    QColor scrollbarHover;    // hover na alça

    // ── Welcome / Splash ────────────────────────────────────────
    QColor welcomeBgTop;      // gradiente superior
    QColor welcomeBgBottom;   // gradiente inferior
    QColor welcomeBtnGradStart; // início do gradiente do botão
    QColor welcomeBtnGradEnd;   // fim do gradiente do botão

    // ── Express / Effects ───────────────────────────────────────
    QColor expressBg;         // fundo do painel express
    QColor expressCardBg;     // fundo de cards de efeitos
    QColor expressDescText;   // texto de descrição
    QColor effectsSearchBg;   // fundo da busca de efeitos
    QColor effectsTreeBg;     // fundo da árvore de efeitos
    QColor effectsPreviewBg;  // fundo do preview de efeitos

    // ── Graph Editor ────────────────────────────────────────────
    QColor graphBg;           // fundo do editor de curvas
    QColor graphGrid;         // grade do editor
    QColor graphLine;         // linhas de curva
    QColor graphKeyframe;     // keyframes
    QColor graphLabel;        // labels de valor
    QColor graphRuler;        // régua de tempo do editor
    QColor graphAxis;         // linha dos eixos e ticks da régua
    QColor graphRulerText;    // texto da régua e do label do playhead
    QColor graphHandle;       // alças bezier, marquee e linha do playhead

    // ── Pantalone ─────────────────────────────────────────────────
    QColor pantaloneBg;         // fundo do pan/crop
    QColor pantaloneRegion;     // região selecionada
    QColor pantaloneHandle;     // alças de resize

    // ── Divider between video/audio sections ──────────────────────
    QColor sectionDivider;    // barra divisória vídeo/áudio

    // ── Scrollbar (global) ──────────────────────────────────────
    QString scrollbarStyle;   // stylesheet completo da scrollbar

    // ── Tipografia ──────────────────────────────────────────────
    QString uiFont;           // pilha de famílias da UI (separada por vírgula);
                              // vazio = fonte do sistema. Usado pelo main().
};

// Acessa a paleta do tema atual.
const ThemeColors& themeColors();

// Acessa o tema atual (índice/enum, para stylesheets condicionais).
AppTheme currentTheme();

// Aplica a paleta QPalette ao QApplication.
void applyAppPalette(QApplication* app, AppTheme theme);

// Retorna o AppTheme salvo nas configurações.
AppTheme savedTheme();

// Salva o tema nas configurações.
void saveTheme(AppTheme theme);

// ── Pasta de temas (~/.config/Pierrot/themes) ───────────────────
// Guarda os arquivos .pierrot-theme do usuário (tema por paleta).
QString externalThemesDir();

// ── Tema por paleta (.pierrot-theme) ────────────────────────────
// O arquivo traz as cores semânticas de ThemeColors (chave = #RRGGBB, ~96
// linhas). Como toda a UI lê de ThemeColors — inclusive o que é DESENHADO por
// código (timeline, playhead "agulha", monitor, gráficos, docks, botões) — este
// formato alcança o app inteiro. Chaves ausentes herdam do tema embutido
// escolhido em "Tema:". Requer reinício.
//
// A pasta é a mesma dos temas (~/.config/Pierrot/themes).

// Nomes dos temas por paleta disponíveis (*.pierrot-theme, ordenado).
QStringList listExternalPalettes();

// Nome do tema por paleta ativo ("" = nenhum, usa só o tema embutido).
QString savedExternalPalette();

// Salva o tema por paleta ativo (nome do arquivo; "" desliga).
void saveExternalPalette(const QString& name);

// Serializa uma paleta para o formato texto do .pierrot-theme.
QString serializeThemeColors(const ThemeColors& c, const QString& title = QString());

// Sobrescreve os campos de 'c' com os pares "chave = #RRGGBB" de 'text'.
// Linhas vazias, comentários (# ;) e chaves desconhecidas são ignorados.
// Retorna false se alguma linha era inválida (chave/cor desconhecida).
bool applyThemeColorText(ThemeColors& c, const QString& text);

// Texto da paleta do tema ATUAL (para "Exportar tema atual…").
QString exportCurrentPalette();

// Idioma da UI salvo: "pt" (0), "en" (1), "ja" (2) — índices do combo de
// Ajustes ▸ Interface ▸ Idioma.
QString savedLanguage();

// Pilha de fontes gothic japonesas (usada pelo Japan95 e pela UI em japonês).
QString japaneseUiFontStack();

// Retorna um stylesheet global para a aplicação (dock titles, etc.).
QString globalStyleSheet(AppTheme theme);

// Retorna o stylesheet flat estilo Premiere para os controles padrão
// (botões, campos, combos, menus, tabs, scrollbars...) aplicado ao app.
QString flatControlStyleSheet(AppTheme theme);

// Folha de estilo padrão das caixas de diálogo do editor (group boxes,
// entradas, combos e botões OK/Cancelar), preenchida com as cores do tema.
// Reutilizada pelos dialogs com a cara "casa" (Configurações do projeto,
// Exportação…).
QString dialogStyleSheet();
