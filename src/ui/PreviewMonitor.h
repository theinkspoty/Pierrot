// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#ifndef PIERROT_UI_PREVIEWMONITOR_H
#define PIERROT_UI_PREVIEWMONITOR_H

#include <QImage>
#include <QWidget>

// Janela dedicada de PREVIEW EXTERNO: mostra o mesmo sinal de vídeo do monitor
// principal (a composição final, sem overlays) em uma janela própria. Arraste
// para um segundo monitor e aperte F11 (ou clique duas vezes) para tela cheia.
// É alimentada por `setFrame()` vindo de um timer na MainWindow — o widget
// compara o ponteiro dos dados do QImage (implicitamente compartilhado) e só
// repinta quando de fato mudou, então o custo em idle é desprezível.
class PreviewMonitor : public QWidget {
    Q_OBJECT
public:
    explicit PreviewMonitor(QWidget* parent = nullptr);

    // Atualiza o quadro exibido se os dados realmente mudaram.
    void setFrame(const QImage& img);
    // Limpa a janela (sem quadro por enquanto — fundo preto com aviso).
    void clear();
protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
private:
    void setFullScreen(bool fs);
    QImage m_frame;
    bool m_fullScreen = false;
};

#endif // PIERROT_UI_PREVIEWMONITOR_H