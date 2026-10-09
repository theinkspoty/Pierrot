// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QIcon>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

class QToolButton;
class QPushButton;
class QComboBox;
class QTimer;
class QPainter;
class QPaintEvent;
class QResizeEvent;
class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QEnterEvent;
class QEvent;

// Janela dedicada de PREVIEW EXTERNO: mostra o mesmo sinal de vídeo do monitor
// principal (a composição final, sem overlays) em uma janela própria — arraste
// para um segundo monitor e use F11 para tela cheia.
//
// No espírito do Program Monitor do FilmCraft, além do quadro ela traz os
// controles do programa: transporte (quadro anterior/seguinte, play/pausa e
// loop), margens de segurança (Action 90% + Title 80% + cruz central), zoom
// ("Ajustar" ou percentual, com pan arrastando) e timecode. A janela é
// alimentada por `setFrame()` vindo de um timer na MainWindow — o widget
// compara o ponteiro dos dados do QImage (implicitamente compartilhado) e só
// repinta quando de fato mudou, então o custo em idle é desprezível. Os
// comandos de transporte são emitidos como sinais e a MainWindow os religa ao
// PreviewWidget; o tempo/estado voltam por setTimecode()/setPlaying().
class PreviewMonitor : public QWidget {
    Q_OBJECT
public:
    explicit PreviewMonitor(QWidget* parent = nullptr);

    // Atualiza o quadro exibido se os dados realmente mudaram.
    void setFrame(const QImage& img);
    // Limpa a janela (sem quadro por enquanto — fundo com aviso).
    void clear();

    // Estado vindo do monitor principal (rótulo de timecode e play/pausa).
    void setTimecode(double seconds);
    void setFps(double fps);
    void setPlaying(bool playing);
    void setLoopEnabled(bool enabled);

    bool isFullScreen() const { return m_fullScreen; }

signals:
    void togglePlayRequested();
    void stepRequested(int dir);      // -1 = quadro anterior, +1 = seguinte
    void loopToggled(bool enabled);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    void buildControls();
    void applyBarStyle();
    void setFullScreen(bool fs);
    void revealControls();
    void scheduleAutoHide();
    void updateCursorMode();
    void refreshPlayIcon();
    void syncZoomCombo();
    QRect viewportRect() const;      // área do vídeo (exclui a barra)
    QRect pictureRect();             // onde o quadro é desenhado no viewport (clampa o pan)
    void drawSafeMargins(QPainter& p, const QRect& picture);
    void drawTimecode(QPainter& p);
    QString timecodeText() const;

    QImage m_frame;
    bool m_fullScreen = false;
    bool m_controlsRevealed = true;

    // Zoom/pan: zoom 0 = "Ajustar"; senão fator (1.0 = 100% dos pixels).
    double m_zoom = 0.0;
    QPointF m_pan;
    bool m_panning = false;
    QPoint m_panLast;

    // Estado espelhado do transporte principal.
    double m_time = 0.0;
    double m_fps = 30.0;
    bool m_playing = false;

    bool m_showSafe = false;

    QWidget* m_bar = nullptr;
    QToolButton* m_stepBackBtn = nullptr;
    QPushButton* m_playBtn = nullptr;
    QToolButton* m_stepFwdBtn = nullptr;
    QToolButton* m_loopBtn = nullptr;
    QToolButton* m_safeBtn = nullptr;
    QComboBox* m_zoomCombo = nullptr;
    QToolButton* m_fullBtn = nullptr;

    QIcon m_playIcon;
    QIcon m_pauseIcon;
    QTimer* m_hideTimer = nullptr;
};
