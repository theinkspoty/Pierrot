// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QWidget>
#include <QImage>

// Analisadores de vídeo para correção de cor: waveform (luma vs. posição),
// histograma (RGB) e vectorscope (croma U/V). Recebe um quadro REDUZIDO
// (ex. 160×90) do preview composto e desenha o escopo em cache próprio.
class ScopeWidget : public QWidget {
    Q_OBJECT
public:
    enum Mode { Waveform = 0, Histogram = 1, Vectorscope = 2 };
    explicit ScopeWidget(QWidget* parent = nullptr);
    void setMode(Mode m) { m_mode = m; update(); }
    Mode mode() const { return m_mode; }
    // Alimenta com o quadro composto do preview (cópia pequena).
    void refreshFrom(const QImage& small);
    bool hasData() const { return !m_src.isNull(); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void buildWaveform();
    void buildHistogram();
    void buildVectorscope();
    Mode m_mode = Waveform;
    QImage m_src;   // quadro de análise (reduzido)
    QImage m_cache; // escopo renderizado em 256×256
};