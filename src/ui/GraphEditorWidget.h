// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QString>
#include <QPoint>
#include <QRect>
#include <QVector>
#include <QHash>
#include <QFrame>
#include "models/Project.h"

class QLabel;
class QToolButton;
class QScrollArea;
class QSplitter;
class QVBoxLayout;

// Ferramenta ativa do gráfico (como na barra de ferramentas do Premiere).
enum CanvasTool {
    ToolSelect = 0, // selecionar/mover keyframes (padrão)
    ToolAdd,        // clicar adiciona keyframe linear
    ToolCurve       // clicar cria ponto suave; arrastar define handles bezier
};

// Propriedades animáveis exibidas no editor de curvas.
enum GraphProp {
    GPropOpacity = 0,
    GPropVolume,
    GPropScale,
    GPropRotation,
    GPropTx,
    GPropTy,
    GPropCropL,
    GPropCropR,
    GPropCropT,
    GPropCropB,
    GPropScaleX,
    GPropScaleY,
    GPropAnchorX,
    GPropAnchorY
};

// Gráfico da propriedade selecionada: curva, keyframes (com glifos por
// interpolação, como no Premiere), handles bezier, régua de tempo e playhead.
class GraphCanvas : public QWidget {
    Q_OBJECT
public:
    explicit GraphCanvas(QWidget* parent = nullptr);
    void setData(Clip* clip, GraphProp prop, double playhead, double fps);
    QVector<Keyframe>* keys() const;
    double baseValue() const;
    void valueRange(double* lo, double* hi) const;
    void commitChange();
    void fitValueRange();
    // Cria um keyframe suave (sem duplicar tempo), ajusta o zoom vertical e o
    // deixa selecionado e pronto para arrastar. Retorna o índice criado ou -1.
    int addKeyframe(double time, double value);
    void selectKeyIndex(int idx);
    void selectKeysAtTimes(const QVector<double>& ts);
    void clearSelection();
    // Move apenas no tempo um keyframe (usado pela minifaixa de keyframes).
    void dragStripTime(int idx, double t);
    // Preserva zoom e seleção ao recarregar os dados (refresh/undo).
    QVector<double> selectionTimes() const;
    double zoomT0() const { return m_t0; }
    double zoomT1() const { return m_t1; }
    void setZoom(double t0, double t1);
    void setTool(CanvasTool t) { m_tool = t; emit toolChanged(t); update(); }
    void setPlayhead(double t) { m_playhead = t; update(); }
public slots:
    void resetZoom();
    void setSnap(bool on);
    bool snapEnabled() const;
signals:
    void editStart();
    void modified();
    void statusMessage(const QString& msg);
    void snapChanged(bool on);
    void toolChanged(CanvasTool t);
    void keyframeJump(double t);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool event(QEvent*) override;
private:
    double xToT(int x) const;
    int tToX(double t) const;
    double yToV(int y) const;
    int vToY(double v) const;
    double snapTime(double t) const;
    double timeStart() const;
    double timeRange() const;
    int keyframeHit(const QPoint& p) const;
    int handleHit(const QPoint& p, int* side) const;
    void sortKeys();
    QRect plotRect() const;
    QRect rulerRect() const;
    QString fmtTime(double t) const;
    QString fmtValue(double v) const;
    void updateHover(const QPoint& p);
    void emitKeyInfo(int idx);
    void moveSelected(double dT, double dV, bool snap);
    void marqueeSelect(const QRect& r, bool add);

    Clip* m_clip = nullptr;
    GraphProp m_prop = GPropOpacity;
    double m_playhead = 0.0;
    double m_fps = 30.0;
    int m_dragKey = -1;
    int m_dragHandle = -1;
    int m_dragSide = 0; // 0 = handle de saída, 1 = handle de entrada
    int m_hoverKey = -1;
    QPoint m_lastPos;
    bool m_undoPushed = false;
    // Arraste da agulha (playhead) pela régua ou pela linha do playhead.
    bool m_playheadDrag = false;
    // Faixa natural da propriedade (fixa) e faixa visível (auto-fit).
    double m_loProp = 0.0, m_hiProp = 1.0;
    double m_lo = 0.0, m_hi = 1.0;
    // Janela de tempo visível no eixo X (m_t1 < 0 = clipe inteiro).
    double m_t0 = 0.0, m_t1 = -1.0;
    // Seleção múltipla: índices + snapshot no início do arrasto.
    QVector<int> m_selKeys;
    QVector<Keyframe> m_selOrig;
    double m_grabT = 0.0, m_grabV = 0.0;
    bool m_snap = true;
    bool m_marqueeActive = false;
    QPoint m_marqueeStart;
    QRect m_marqueeRect;
    CanvasTool m_tool = ToolSelect;
    bool m_curveNewKey = false;
};

// Minifaixa de keyframes de uma propriedade (a linha que abre com o chevron
// de cada parâmetro, como no Effect Controls do Premiere).
class KeyframeStrip : public QWidget {
    Q_OBJECT
public:
    explicit KeyframeStrip(QWidget* parent = nullptr);
    void setData(Clip* clip, GraphProp prop, double fps);
    void setPlayhead(double t);
    QSize sizeHint() const override;
signals:
    void keyClicked(int idx);
    void dragStart(int idx);
    void dragKey(int idx, double t);
    void addKey(double t);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    double xToT(int x) const;
    int tToX(double t) const;
    int hitKey(const QPoint& p) const;

    Clip* m_clip = nullptr;
    GraphProp m_prop = GPropOpacity;
    double m_playhead = 0.0;
    double m_fps = 30.0;
    int m_dragIdx = -1;
    bool m_moved = false;
    double m_dragT0 = 0.0;
};

// Linha de propriedade do painel (estilo Effect Controls do Premiere):
// chevron de expandir, nome, valor atual, stopwatch de animação e os botões
// de navegação ◀ ◆ ▶ quando a propriedade está animada.
class GraphPropRow : public QFrame {
    Q_OBJECT
public:
    explicit GraphPropRow(GraphProp p, QWidget* parent = nullptr);
    GraphProp prop() const { return m_prop; }
    void setAnimated(bool on);
    void setActive(bool on);
    void setValueText(const QString& s);
    void setStripData(Clip* clip, double fps);
    void setStripPlayhead(double t);
    void setExpanded(bool on);
    KeyframeStrip* strip() const { return m_strip; }
signals:
    void propertyClicked();
    void stopwatchClicked();
    void prevKeyframe();
    void nextKeyframe();
    void toggleKeyframe();
    void stripKeyClicked(int idx);
    void stripDragStart(int idx);
    void stripDragKey(int idx, double t);
    void stripAddKey(double t);
protected:
    void mousePressEvent(QMouseEvent*) override;
private:
    void updateExpandText();

    GraphProp m_prop;
    QLabel* m_name = nullptr;
    QLabel* m_value = nullptr;
    QToolButton* m_expand = nullptr;
    QToolButton* m_stopwatch = nullptr;
    QWidget* m_navBox = nullptr;
    QToolButton* m_prev = nullptr;
    QToolButton* m_add = nullptr;
    QToolButton* m_next = nullptr;
    KeyframeStrip* m_strip = nullptr;
};

class GraphEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit GraphEditorWidget(QWidget* parent = nullptr);

    void setProject(Project* p);
    void setClipId(const QString& id);
    void setPlayhead(double t);
    // Seleciona a curva a exibir (usado também pelo pancrop ao animar).
    void setProperty(GraphProp p);
    QSize sizeHint() const override;
public slots:
    void refresh();
signals:
    void editStart();
    void modified();
    void keyframeJump(double t);
private:
    void rebuildRows();
    void syncRowData();
    void syncValueLabels();
    Clip* activeClip() const;
    double propValueAtPlayhead(GraphProp p) const;
    void toggleAnimation(GraphProp p);
    void toggleKeyAtPlayhead(GraphProp p);
    void jumpKeyframe(GraphProp p, int dir);

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;
    GraphProp m_prop = GPropOpacity;
    GraphCanvas* m_canvas = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_noClip = nullptr;
    QScrollArea* m_scroll = nullptr;
    QVBoxLayout* m_rowsLayout = nullptr;
    QToolButton* m_toolSel = nullptr;
    QToolButton* m_toolAdd = nullptr;
    QToolButton* m_toolCurve = nullptr;
    QVector<GraphProp> m_props;
    QHash<int, GraphPropRow*> m_rows;
};
