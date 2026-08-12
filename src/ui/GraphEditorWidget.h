#pragma once

#include <QWidget>
#include <QString>
#include <QPoint>
#include <QRect>
#include <QVector>
#include "models/Project.h"

class QComboBox;
class QPushButton;

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
    GPropCropB
};

class GraphCanvas : public QWidget {
    Q_OBJECT
public:
    explicit GraphCanvas(QWidget* parent = nullptr);
    void setData(Clip* clip, GraphProp prop, double playhead);
    QVector<Keyframe>* keys() const;
    double baseValue() const;
    void valueRange(double* lo, double* hi) const;
    void commitChange();
signals:
    void editStart();
    void modified();
    void statusMessage(const QString& msg);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
private:
    double xToT(int x) const;
    int tToX(double t) const;
    double yToV(int y) const;
    int vToY(double v) const;
    int keyframeHit(const QPoint& p) const;
    int handleHit(const QPoint& p) const;
    void sortKeys();
    QRect plotRect() const;

    Clip* m_clip = nullptr;
    GraphProp m_prop = GPropOpacity;
    double m_playhead = 0.0;
    int m_dragKey = -1;
    int m_dragHandle = -1;
    QPoint m_lastPos;
    bool m_undoPushed = false;
    double m_lo = 0.0, m_hi = 1.0;
};

class GraphEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit GraphEditorWidget(QWidget* parent = nullptr);

    void setProject(Project* p);
    void setClipId(const QString& id);
    void setPlayhead(double t);
public slots:
    void refresh();
signals:
    void editStart();
    void modified();
private:
    void rebuildProperties();
    GraphProp currentProp() const;
    Clip* activeClip() const;

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;
    QComboBox* m_propCombo = nullptr;
    GraphCanvas* m_canvas = nullptr;
    QVector<GraphProp> m_props;
};
