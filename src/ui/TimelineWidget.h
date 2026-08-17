// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

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
class QVariantAnimation;
class QTimer;

// Chave do cache de conteúdo visual dos clipes (onda/thumb + envelope + fades).
// O epoch é bumpado em mudanças estruturais; rolagem/zoom mantêm o epoch e só
// o tamanho muda, permitindo reaproveitar o blit entre repaints.
struct ClipVisKey {
    QString id;
    int w = 0;
    int h = 0;
    quint64 epoch = 0;
    bool operator==(const ClipVisKey& o) const {
        return id == o.id && w == o.w && h == o.h && epoch == o.epoch;
    }
};

inline uint qHash(const ClipVisKey& k, uint seed = 0) {
    return qHash(k.id, seed) ^ (k.w * 0x9E3779B1u) ^ (k.h * 0x85EBCA77u)
        ^ (uint(k.epoch) * 0xC2B2AE3Du);
}

class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    static inline const char* const kMimeMedia = "application/x-pierrot-media";

    explicit TimelineWidget(QWidget* parent = nullptr);

    void setProject(Project* p);
    Project* project() const { return m_project; }

    double playhead() const { return m_playhead; }
    void setPlayhead(double t);
    // Último clipe selecionado (ou vazio se não houver seleção).
    QString lastSelectedId() const {
        return m_selected.isEmpty() ? QString() : m_selected.last();
    }
    // Seleciona um clipe (usado para restaurar a seleção após undo/redo).
    void selectClip(const QString& id) { setSelection(id); }
    // Abre o menu de estilos das faixas (minimizada/normal/grande).
    void showTrackPresetMenu();

    void addTrack(bool audio);
    void updateScrollRanges();
public slots:
    void cutAtPlayhead();
    void deleteSelected();
    void deleteSelection(); // clipes selecionados, ou faixas selecionadas se não houver clipes
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
    void addMediaAtPlayhead(const QString& mediaId);
    // Cria um clipe independente de texto (animável) numa faixa de vídeo.
    void addTextClipAt(int row, double t);
    // Arrasto manual vindo da pool de mídia (não depende do DnD do
    // compositor, que falha em alguns ambientes/Wayland).
    void showDropHover(const QPoint& globalPos);
    void hideDropHover();
    void dropMediaAt(const QStringList& mediaIds, const QPoint& globalPos);
    void moveTrack(bool audio, int from, int to);
    void moveTracksTo(bool audio, const QVector<int>& idxs, int to);
    void finishTrackDrag();
    // Recria o conteúdo dos clipes (ex.: mudou o modo de exibição das
    // miniaturas nas configurações).
    void refreshSettings();
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
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    enum DragMode { None, MoveClip, TrimLeft, TrimRight, Razor, RulerLoop, ZoomSelect, Marquee, PlayheadDrag, ResizeTrack, TrackVol, ClipVol, TrackDrag };
    struct ClipOrig { double pos = 0.0, in = 0.0, dur = 0.0; };
    struct ClipboardEntry { Clip clip; int track = 0; bool audio = false; };
    struct TrackSel {
        int row = 0;
        bool audio = false;
        bool operator==(const TrackSel& o) const { return row == o.row && audio == o.audio; }
    };

    double timeToX(double t) const;
    double xToTime(int x) const;
    int trackH(int idx, bool audio) const;
    void applyTrackPreset(int preset);
    int rowY(int videoIdx, int audioIdx) const;
    bool rowFromY(int y, int& row, bool& audio) const;
    int resizeHandleAt(const QPoint& pos, int& row, bool& audio) const;
    Clip* clipAt(int row, bool audio, double t) const;
    Clip* findClipById(const QString& id);
    Track* trackOf(Clip* c);
    QVector<Clip*> groupMembers(const QString& gid);
    QStringList expandToGroups(const QStringList& ids);
    double snapTime(double t) const;
    double snapToEdges(double t, const QString& excludeId = QString()) const;
    double clampPosToTrack(Clip* c, double newPos, const QSet<QString>& moving) const;
    bool clipTrackIndex(const QString& id, int& row, bool& audio) const;
    bool moveClipToTrack(const QString& id, int row, bool audio);
    bool isSelected(const QString& id) const;
    void setSelection(const QString& id);
    void toggleSelection(const QString& id);
    void invalidateScene();
    // Redesenha sem descartar os caches de conteúdo dos clipes (rolagem,
    // zoom e follow do playhead). Só mudanças estruturais bumpam o epoch.
    void refreshView();
    void animateZoomTo(double targetPps, double anchorT);
    void renderScene(QPainter& p);
    void renderOverlays(QPainter& p);
    void ensurePlayheadVisible();
    // Autoscroll: rola a timeline continuamente enquanto o mouse segura a
    // agulha (ou clipe) perto da borda da view. Retorna a direção (px por tick).
    void startAutoScroll(QMouseEvent* e);
    void stopAutoScroll();
    void autoScrollTick();
    void drawClip(QPainter& p, const QRect& r, const Clip& c, const Track& tr, bool audio);
    void drawTextClipBody(QPainter& p, const QRect& r, const Clip& c);
    void drawAudioWaveform(QPainter& p, const QRect& r, const Clip& c, const QString& path);
    void drawVideoThumbs(QPainter& p, const QRect& r, const Clip& c, const QString& path);
    void drawFadeCorners(QPainter& p, const QRect& r, const Clip& c);
    void drawTransitionIndicator(QPainter& p, const QRect& r, const QString& type);
    void drawKeyframeDiamonds(QPainter& p, const QRect& r, const Clip& c, bool audio);
    void drawEnvelope(QPainter& p, const QRect& r, const Clip& c, bool audio);
    void finishDrop(const QStringList& mediaIds, const QPoint& dropPos);
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
    void showTextEditorDialog(Clip* c);
    void drawTrackHeader(QPainter& p, int y, int rowH, const Track& tr, bool selected);
    int headerBtnAt(const QPoint& pos, int& row, bool& audio) const;
    bool trackLocked(const Clip* c) const;
    int volLineY(int row, bool audio, const Track& tr) const;
    int volRowAt(const QPoint& pos, int& row) const;
    int clipVolLineY(int row, const Clip& c) const;
    Clip* clipVolAt(const QPoint& pos, int& row) const;
    void copySelected();
    void cutSelected();
    void pasteClips();
    void duplicateSelected();
    void nudgeSelected(int dir);
    void selectAllClips();
    bool isTrackSelected(int row, bool audio) const;
    void setTrackSel(int row, bool audio);
    void toggleTrackSel(int row, bool audio);
    void selectTrackRange(int row, bool audio);
    void selectTrackRightClick(int row, bool audio);
    void clearTrackSelection();
    int folderStripsAboveVideo(int videoIdx) const;
    int folderStripsAboveAudio(int audioIdx) const;
    QRect folderStripRect(const TrackGroup& g) const;
    QRect folderArrowRect(const TrackGroup& g) const;
    bool folderStripAt(int y, QString& gid) const;
    void drawFolderStrip(QPainter& p, const TrackGroup& g);
    bool trackVisible(int row, bool audio) const;
    void toggleGroupCollapsed(const QString& gid);
    void createTrackGroup();
    void renameTrackGroup(const QString& gid);
    void selectGroupTracks(const QString& gid);
    void toggleGroupTracks(const QString& gid);
    void pruneEmptyGroups();

    Project* m_project = nullptr;
    double m_playhead = 0.0;
    double m_pps = 80.0;
    double m_viewStart = 0.0;
    int m_viewTop = 0;
    QScrollBar* m_hbar = nullptr;
    QScrollBar* m_vbar = nullptr;
    QStringList m_selected;
    QVector<TrackSel> m_selTracks;
    TrackSel m_selAnchor;
    bool m_hasAnchor = false;
    int m_tool = 0;
    bool m_snap = true;
    bool m_showVolLines = false;
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
    QVector<ClipboardEntry> m_clipboard;
    int m_resizeRow = -1;
    bool m_resizeAudio = false;
    int m_resizeOrigH = 0;
    QVector<int> m_resizeSelOrigH; // alturas originais das faixas selecionadas
    int m_trackPreset = 1; // 0=minimizada, 1=normal, 2=grande
    // Arrasto de faixa (reordenar ou soltar em pasta).
    int m_dragTrackRow = -1;
    bool m_dragTrackAudio = false;
    bool m_trackDragActive = false;
    int m_dropRow = -1;
    bool m_dropAudio = false;
    QString m_dropGroup;
    QString m_dragGroupId; // grupo sendo arrastado pela faixa de pasta (vazio = faixa única)
    int m_dragHoverRow = -1;
    bool m_dragHoverAudio = false;
    int m_volRow = -1;
    double m_volOrig = 1.0;
    QString m_volClip;      // clipe cujo volume está sendo ajustado
    double m_volClipOrig = 1.0;
    int m_volRowOrig = -1;   // faixa de origem ao iniciar ClipVol
    bool m_volPending = false; // press em faixa de áudio; vira volume se drag vertical

    QHash<ClipVisKey, QPixmap> m_clipPix;
    qint64 m_clipBytes = 0;
    quint64 m_clipEpoch = 0;

    // Zoom com easing linear: mantém o pixel-âncora fixo durante a transição.
    // No modo "retângulo" (ferramenta de zoom) anima pps e viewStart juntos
    // para que o trecho selecionado termine preenchendo a view.
    QVariantAnimation* m_zoomAnim = nullptr;
    double m_zoomAnchorT = 0.0;
    double m_zoomAnchorPixel = 0.0;
    bool m_zoomRectMode = false;
    double m_zoomStartPps = 0.0;
    double m_zoomEndPps = 0.0;
    double m_zoomStartView = 0.0;
    double m_zoomEndView = 0.0;
    QTimer* m_autoScroll = nullptr;
    int m_autoScrollDir = 0; // -1 esquerda, +1 direita (px por tick)
    QPoint m_autoScrollMouse;
};
