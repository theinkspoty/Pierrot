// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QVector>

// Um ponto/nó da curva de EQ. O mesmo widget serve ao EQ gráfico (frequências
// fixas — só arrasta na vertical) e ao paramétrico (arrasta freq + ganho).
struct EqNode {
    double freq = 1000.0;   // Hz
    double gain = 0.0;      // dB
    double q = 1.0;         // Bell
    int type = 0;           // EqBandType (0 Bell, 1 LowShelf, 2 HighShelf)
    bool enabled = true;
    bool movableFreq = true; // false = só o ganho é editável (EQ gráfico)
};

// Gráfico de resposta de frequência: eixo X logarítmico (20 Hz..20 kHz) e eixo
// Y em dB (-maxDb..+maxDb). Desenha a curva somando a magnitude dos biquads
// (EqBiquad) e permite arrastar os nós. Estilo Audition.
class EqCurveView : public QWidget {
    Q_OBJECT
public:
    explicit EqCurveView(QWidget* parent = nullptr);

    void setNodes(const QVector<EqNode>& nodes);
    const QVector<EqNode>& nodes() const { return m_nodes; }
    void setGainRange(double maxDb); // simétrico
    void setInteractive(bool on);
    void setSelected(int index);
    int selected() const { return m_selected; }

    QSize sizeHint() const override { return QSize(560, 260); }
    QSize minimumSizeHint() const override { return QSize(320, 170); }

signals:
    void editStart(); // início de um arraste (snapshot de undo)
    void nodeMoved(int index, double freq, double gain);
    void nodeSelected(int index);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    QRect plotRect() const;
    double xForFreq(double f) const;
    double freqForX(double x) const;
    double yForGain(double g) const;
    double gainForY(double y) const;
    int nodeAt(const QPoint& p) const;

    QVector<EqNode> m_nodes;
    double m_padL = 36.0, m_padR = 12.0, m_padT = 12.0, m_padB = 22.0;
    double m_maxDb = 24.0;
    int m_selected = -1;
    int m_drag = -1;
    bool m_interactive = true;
};
