#pragma once

#include <QWidget>
#include <QString>
#include <QPoint>
#include <QRect>
#include <QVector>
#include "models/Project.h"

class QComboBox;
class QPushButton;
class QLabel;

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
    void setData(Clip* clip, GraphProp prop, double playhead, double fps);
    QVector<Keyframe>* keys() const;
    double baseValue() const;
    void valueRange(double* lo, double* hi) const;
    void commitChange();
    void fitValueRange();
    // Cria um keyframe suave (sem duplicar tempo), ajusta o zoom vertical e o
    // deixa selecionado e pronto para arrastar. Retorna o índice criado ou -1.
    int addKeyframe(double time, double value);
public slots:
    void resetZoom();
    void setSnap(bool on);
    bool snapEnabled() const;
signals:
    void editStart();
    void modified();
    void statusMessage(const QString& msg);
    void snapChanged(bool on);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
private:
    double xToT(int x) const;
    int tToX(double t) const;
    double yToV(int y) const;
    int vToY(double v) const;
    double snapTime(double t) const;
    double timeStart() const;
    double timeRange() const;
    int keyframeHit(const QPoint& p) const;
    int handleHit(const QPoint& p) const;
    void sortKeys();
    QRect plotRect() const;
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
    int m_hoverKey = -1;
    QPoint m_lastPos;
    bool m_undoPushed = false;
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
};

class GraphEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit GraphEditorWidget(QWidget* parent = nullptr);

    void setProject(Project* p);
    void setClipId(const QString& id);
    void setPlayhead(double t);
    QSize sizeHint() const override;
public slots:
    void refresh();
signals:
    void editStart();
    void modified();
    void keyframeJump(double t);
private:
    void rebuildProperties();
    GraphProp currentProp() const;
    Clip* activeClip() const;

    Project* m_project = nullptr;
    QString m_clipId;
    double m_playhead = 0.0;
    QComboBox* m_propCombo = nullptr;
    GraphCanvas* m_canvas = nullptr;
    QLabel* m_status = nullptr;
    QVector<GraphProp> m_props;
};
