// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QString>

class FFmpegDecoder;

// Pool de decodificadores "quentes" por caminho de arquivo. Em vez de 2+1
// decoders fixos (main + prefetch + bg), mantém N abertos no arquivo certo:
// o uso vira um lease (acquire/release) por caminho, e o prefetch em background
// só precisa "aquecer" — abre o próximo arquivo e deixa a entrada ociosa no
// pool, sem transferir posse. Assim o open() de 500-800 ms é pago uma vez por
// fonte (ou quando uma entra/sai por LRU), não todo corte.
//
// Segurança: um decoder NUNCA é usado por duas threads ao mesmo tempo —
// acquire() marca a entrada busy, e tudo que é lento (open/frameAt) roda FORA
// do mutex do pool, com o caller como dono exclusivo até release(). O mutex só
// protege o mapa (QLock + QMutex), nunca o decoder.
class DecoderPool {
public:
    explicit DecoderPool(int capacity);
    ~DecoderPool();

    // Lease: devolve um decoder aberto em `path` (já quente, ou abre agora).
    // nullptr se o caminho estiver ocupado por outro lease (ex.: prefetch ainda
    // decodificando) ou o pool não tiver slot livre (todos ocupados).
    FFmpegDecoder* acquire(const QString& path);

    // Devolve a entrada (fica quente, pronta para o próximo acquire).
    void release(const QString& path);

    // Nº de arquivos atualmente abertos (para diagnóstico/overlay).
    int count() const;

    // Já existe entrada aberta para este caminho (quente)? Usado pelo
    // warm-ahead para não reaquecer o que já está no pool.
    bool contains(const QString& path) const;

private:
    struct Entry {
        FFmpegDecoder* dec = nullptr; // criado no .cpp (include completo lá)
        bool busy = false;
        qint64 lastUseMs = 0; // último acquire/release (LRU)
        ~Entry();
    };

    void evictIdleLocked(int keep);

    int m_capacity;
    mutable QMutex m_mutex;
    QHash<QString, Entry*> m_entries;
    QElapsedTimer m_clock;
};
