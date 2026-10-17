// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>
#include <QColor>
#include <QVector2D>

// Roda de cor estilo DaVinci Resolve (trackball). O puck é arrastado do
// centro; o deslocamento (dx,dy) em -1..1 codifica o tint do intervalo
// tonal (Lift=sombras, Gamma=meios, Gain=realces). Cor de fundo mostra a
// matiz/saturação da direção puxada. Roda do mouse não faz nada aqui.
class ColorWheel : public QWidget {
    Q_OBJECT
public:
    explicit ColorWheel(const QString& title, QWidget* parent = nullptr);

    // Deslocamento do puck, -1..1 em cada eixo (0 = centro / neutro).
    QVector2D offset() const { return m_offset; }
    void setOffset(const QVector2D& o);

    // Brilho/master do intervalo (-1..1), mostrado como anel externo.
    double master() const { return m_master; }
    void setMaster(double m);

    QSize sizeHint() const override { return {128, 128}; }
    QSize minimumSizeHint() const override { return {96, 96}; }

signals:
    void offsetChanged(const QVector2D& o); // arrasto (ao vivo)
    void masterChanged(double m);
    void adjusted();                        // commit (soltar / fim de arrasto)

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    QPointF puckPos() const;   // centro do puck em coords de widget
    void setFromPos(const QPointF& p);
    void reset();

    QString m_title;
    QVector2D m_offset;
    double m_master = 0.0;
    bool m_dragging = false;
};
