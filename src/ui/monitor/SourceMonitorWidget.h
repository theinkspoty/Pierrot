// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Source Monitor (dock, estilo Premiere): carrega uma mídia do pool,
// define In/Out com I/O e insere na timeline via Insert (,) ou
// Overwrite (.). O Program Monitor continua sendo o PreviewWidget central.

#pragma once

#include <QWidget>
#include <QImage>
#include <QElapsedTimer>
#include "colombina/models/Project.h"

class QLabel;
class QSlider;
class QDoubleSpinBox;
class QTimer;
class QKeyEvent;
class QToolButton;
#include "laartman/Decoder.h"
class SourceScrub;
class AspectLabel;

class SourceMonitorWidget : public QWidget {
    Q_OBJECT
public:
    explicit SourceMonitorWidget(QWidget* parent = nullptr);
    ~SourceMonitorWidget() override;

    void setProject(Project* p);
    bool hasMedia() const { return !m_mediaId.isEmpty(); }
    QString mediaId() const { return m_mediaId; }
    double sourceIn() const { return m_in; }
    double sourceOut() const { return m_out; }

public slots:
    // Abre a mídia no monitor (id do MediaItem no projeto corrente).
    void openMedia(const QString& mediaId);
    void markIn();
    void markOut();
    void togglePlay();
    void stepFrame(int dir);
    void insertRequested();
    void overwriteRequested();

signals:
    void insertSource(const QString& mediaId, double srcIn, double srcOut);
    void overwriteSource(const QString& mediaId, double srcIn, double srcOut);
    void mediaOpened(const QString& mediaId);
    void statusMessage(const QString& msg);

protected:
    void keyPressEvent(QKeyEvent* e) override;

private slots:
    void onScrubPos(double p, bool fromUser);
    void onScrubRange(double in, double out);
    void setPos(double p, bool fromUser);
    void playTick();
    void updatePreview();

private:
    void clearMonitor();
    void clampInOut();
    void refreshTimeLabels();
    double currentPos() const;
    double mediaDuration() const;
    void openDecoderFor(const MediaItem& media);

    Project* m_project = nullptr;
    QString m_mediaId;
    MediaItem m_media; // cópia: o Project pode trocar durante a sessão
    Decoder* m_decoder = nullptr;
    DecodeOptions m_openOpts; // hardwareAllowed=false: decode de UI em software

    double m_in = 0.0;
    double m_out = 0.0;
    bool m_playing = false;
    double m_pos = 0.0;              // posição autoritativa, em segundos
    QElapsedTimer m_playClock;

    QLabel* m_title = nullptr;
    AspectLabel* m_preview = nullptr;
    SourceScrub* m_scrub = nullptr;
    QLabel* m_timeLbl = nullptr;
    QLabel* m_rangeLbl = nullptr;
    QDoubleSpinBox* m_spIn = nullptr;
    QDoubleSpinBox* m_spOut = nullptr;
    QToolButton* m_playBtn = nullptr;
    QToolButton* m_insertBtn = nullptr;
    QToolButton* m_overwriteBtn = nullptr;
    QTimer* m_throttle = nullptr;
    QTimer* m_playTimer = nullptr;
};
