// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QObject>
#include <QImage>
#include <QString>

class DecoderPool;

// Thread separada para prefetch — decodifica o próximo clipe em background
// sem bloquear o FrameWorker principal (decodePrefetch original levava
// 500-800ms de open+frameAt, congelando o worker e quebrando o m_ready).
// Aqui não há posse: o prefetch só AQUECE o DecoderPool — abre o arquivo na
// posição do corte e deixa a entrada ociosa (release). No corte, o FrameWorker
// reacquire a mesma entrada já quente em µs. Como acquire/release são
// exclusivos (busy), jamais há acesso concorrente ao mesmo decoder.
class BgPrefetchWorker : public QObject {
    Q_OBJECT
public:
    explicit BgPrefetchWorker(DecoderPool* pool, QObject* parent = nullptr);

public slots:
    void decode(const QString& path, double t, int maxW, double step);

    // Aquece um clipe mais à frente (Fase 2): só abre e posiciona o decoder no
    // início do clip, sem devolver frame. No corte daquele clip o acquire do
    // FrameWorker volta quente em µs. Serializado com decode() na mesma thread.
    void warm(const QString& path, double t, int maxW);

signals:
    void done(const QString& path, double t, int maxW,
              const QImage& frame0, const QImage& frame1);
    void failed(const QString& path);
    void warmed(const QString& path);

private:
    DecoderPool* m_pool; // não dono (vida do PreviewWidget)
};
