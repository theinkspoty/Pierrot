// Pierrot — editor de vídeo
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>
#include <QVector>
#include <QPointF>
#include "colombina/models/Project.h"

class ColorWheel;
class QSlider;
class QLabel;
class QPushButton;
class QTabWidget;
class QScrollArea;
class QGridLayout;
class QFrame;
class PreviewWidget;
class TimelineWidget;

// Dock "Cor" — correção + looks analógicos inspirados no charme do
// revelado fotográfico, adaptados ao fluxo de vídeo do Pierrot.
class ColorDock : public QWidget {
    Q_OBJECT
public:
    explicit ColorDock(QWidget* parent = nullptr);

    void setTimeline(TimelineWidget* t) { m_timeline = t; }
    void setPreview(PreviewWidget* p);
    void setClip(Clip* c);

public slots:
    void setClipById(const QString& id);
    void refreshFromClip();

private slots:
    void applyLive();

private:
    struct ColorLook {
        QString id;
        QString name;
        bool builtIn = false;
        // Copia do estado relevante do grade
        double liftR = 0, liftG = 0, liftB = 0;
        double gammaR = 1, gammaG = 1, gammaB = 1;
        double gainR = 0, gainG = 0, gainB = 0;
        double cgExposure = 0, cgContrast = 0, cgHighlights = 0, cgShadows = 0;
        double cgWhites = 0, cgBlacks = 0;
        double cgSaturation = 0, cgVibrance = 0;
        double cgTemperature = 0, cgTint = 0;
        double cgFadedFilm = 0, cgSharpen = 0;
        double cgVignette = 0, cgVignetteFeather = 50;
        double cgBlend = 1.0;
        QString cgLutPath;
        double cgLutStrength = 1.0;
        QVector<QPointF> cgMasterCurve, cgRCurve, cgGCurve, cgBCurve;
    };

    void buildUi();
    void buildTabs();
    void buildCorrectionTab(QWidget* parent);
    void buildWheelsTab(QWidget* parent);
    void buildLooksTab(QWidget* parent);
    void pushUndo();
    void writeWorkFromWheels();
    void readWheelsFromWork();
    void updatePreviewFrame();

    void initBuiltInLooks();
    void loadUserLooks();
    void saveUserLooks();
    void refreshLooksGrid();
    void applyLook(const ColorLook& look);
    Clip clipFromLook(const ColorLook& look) const;
    ColorLook lookFromClip(const Clip& c, const QString& name, bool builtIn) const;
    QString looksDir() const;

    TimelineWidget* m_timeline = nullptr;
    PreviewWidget* m_preview = nullptr;
    Clip* m_clip = nullptr;
    Clip m_work;
    bool m_updating = false;

    QTabWidget* m_tabs = nullptr;

    // Correção / Rodas
    ColorWheel* m_lift = nullptr;
    ColorWheel* m_gamma = nullptr;
    ColorWheel* m_gain = nullptr;
    QSlider* m_exposure = nullptr;
    QSlider* m_contrast = nullptr;
    QSlider* m_saturation = nullptr;
    QSlider* m_temperature = nullptr;
    QSlider* m_tint = nullptr;
    QSlider* m_blend = nullptr;
    QPushButton* m_resetBtn = nullptr;

    // Looks
    QScrollArea* m_looksScroll = nullptr;
    QWidget* m_looksGridHost = nullptr;
    QGridLayout* m_looksGrid = nullptr;
    QPushButton* m_saveLookBtn = nullptr;
    QVector<ColorLook> m_looks;
    QVector<ColorLook> m_builtInLooks;

    QLabel* m_status = nullptr;
};
