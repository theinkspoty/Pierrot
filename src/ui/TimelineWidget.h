#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QRect>
#include <QPoint>
#include <QPixmap>
#include "models/Project.h"

class QScrollBar;
class QPainter;
class QMouseEvent;
class QKeyEvent;
class QContextMenuEvent;

class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    static inline const char* const kMimeMedia = "application/x-pierrot-media";

    explicit TimelineWidget(QWidget* parent = nullptr);

    void setProject(Project* p);
    Project* project() const { return m_project; }

    double playhead() const { return m_playhead; }
    void setPlayhead(double t);

    void addTrack(bool audio);
    void updateScrollRanges();
public slots:
    void cutAtPlayhead();
    void deleteSelected();
    void deleteSelectedLeaveGap();
    void deleteClipBeforePlayhead();
    void deleteClipAfterPlayhead();
    void zoomBy(double factor, double centerT);
    void toggleMarker(double t);
    void setTool(int tool);
    void setSnap(bool on);
    void setLoopInAtPlayhead();
    void setLoopOutAtPlayhead();
    void clearLoop();
signals:
    void playheadChanged(double t);
    void modified();
    void playPauseRequested();
    void editStart();
    void toolChanged(int tool);
    void loopChanged(double in, double out);
    void selectionChanged(const QString& id);
    void pancropRequested(const QString& id);
    void mediaImported();
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    enum DragMode { None, MoveClip, TrimLeft, TrimRight, Razor, RulerLoop, ZoomSelect, Marquee, PlayheadDrag };
    struct ClipOrig { double pos = 0.0, in = 0.0, dur = 0.0; };

    double timeToX(double t) const;
    double xToTime(int x) const;
    int rowY(int videoIdx, int audioIdx) const;
    bool rowFromY(int y, int& row, bool& audio) const;
    Clip* clipAt(int row, bool audio, double t);
    Clip* findClipById(const QString& id);
    Track* trackOf(Clip* c);
    QVector<Clip*> groupMembers(const QString& gid);
    QStringList expandToGroups(const QStringList& ids);
    double snapTime(double t) const;
    double snapToEdges(double t, const QString& excludeId = QString()) const;
    double clampPosToTrack(Clip* c, double newPos, const QSet<QString>& moving) const;
    double fitDurationInTrack(const Track& tr, double t, double dur,
                              const QString& excludeId) const;
    bool isSelected(const QString& id) const;
    void setSelection(const QString& id);
    void toggleSelection(const QString& id);
    void invalidateScene();
    void renderScene(QPainter& p);
    void renderOverlays(QPainter& p);
    void ensurePlayheadVisible();
    void drawClip(QPainter& p, const QRect& r, const Clip& c, const Track& tr, bool audio);
    void drawAudioWaveform(QPainter& p, const QRect& r, const Clip& c, const QString& path);
    void drawVideoThumbs(QPainter& p, const QRect& r, const Clip& c, const QString& path);
    void drawFadeCorners(QPainter& p, const QRect& r, const Clip& c);
    void drawKeyframeDiamonds(QPainter& p, const QRect& r, const Clip& c, bool audio);
    void drawEnvelope(QPainter& p, const QRect& r, const Clip& c, bool audio);
    void splitClipAt(Clip* c, double t);
    void duplicateClip(Clip* c);
    void selectInMarquee(bool add);
    void razorSplitAt(double t);
    void envelopePress(Clip* c, double t);
    void applyZoomRect(double t0, double t1);
    void removeClipsByIds(const QStringList& ids);
    void showProperties(Clip* c);
    void showEffectsDialog(Clip* c);
    void showTransformDialog(Clip* c);
    void showAudioEffectsDialog(Clip* c);

    Project* m_project = nullptr;
    double m_playhead = 0.0;
    double m_pps = 80.0;
    double m_viewStart = 0.0;
    int m_viewTop = 0;
    QScrollBar* m_hbar = nullptr;
    QScrollBar* m_vbar = nullptr;
    QStringList m_selected;
    int m_tool = 0;
    bool m_snap = true;
    double m_loopIn = -1.0;
    double m_loopOut = -1.0;
    DragMode m_dragMode = None;
    QString m_dragClip;
    bool m_dragUndoPushed = false;
    QPoint m_dragStart;
    double m_dragOrigPos = 0.0;
    double m_dragOrigIn = 0.0;
    double m_dragOrigDur = 0.0;
    double m_loopPressT = 0.0;
    double m_razorT = 0.0;
    double m_zoomT0 = 0.0;
    double m_zoomT1 = 0.0;
    QRect m_marqueeRect;
    QPixmap m_staticCache;
    bool m_staticDirty = true;
    QHash<QString, ClipOrig> m_dragOrig;
};
