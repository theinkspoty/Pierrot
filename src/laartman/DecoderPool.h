// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QString>
#include <memory>

class FFmpegDecoder;

// Pool GLOBAL de decodificadores "quentes" por caminho de arquivo (F1).
//
// Um único pool para o app inteiro (antes: DecoderPool do preview com lease
// EXCLUSIVO + mapa ilimitado de shared_ptr do MesaRenderer, sem coordenação).
// O entry é o mesmo; o que muda é a VISÃO de acesso:
//
//  - acquire()/release() — lease EXCLUSIVO (preview): um dono por vez, para o
//    protocolo prefetch/worker (busy flag; open() roda fora do mutex, pago
//    uma vez por fonte).
//  - acquireShared() — acesso COMPARTILHADO (Mesa/composição): N threads
//    podem segurar o mesmo decoder; a serialização interna do próprio
//    FFmpegDecoder (m_mutex) garante a segurança. O ponteiro shared_ptr
//    mantém o entry vivo mesmo que o pool o despeje.
//
// Evicção LRU: entradas ociosas (busy=false e use_count==1) são despejadas
// quando o pool passa do teto. Entradas em uso nunca são despejadas — o pool
// pode passar do teto temporariamente em vez de derrubar decoder em uso.
//
// Segurança: um decoder NUNCA é usado por duas threads sem a serialização
// interna dele — no lease, acquire() marca busy e o chamador é dono
// exclusivo; no shared, o mutex do decoder cobre. O mutex do pool só protege
// o MAPA, nunca o decoder.
class DecoderPool {
public:
    // Instância global do processo. Capacidade: PIERROT_DECODER_POOL=<n>
    // (mín. 2), padrão 10 — cobre o pool do preview (6) e o da composição
    // (8) com folga, sem deixar os dois crescerem por conta própria.
    static DecoderPool& instance();

    explicit DecoderPool(int capacity);
    ~DecoderPool();

    // Lease exclusivo: devolve um decoder aberto em `path` (já quente, ou
    // abre agora). nullptr se o caminho estiver ocupado por outro lease ou o
    // pool não tiver slot livre (todos ocupados).
    FFmpegDecoder* acquire(const QString& path);

    // Devolve a entrada (fica quente, pronta para o próximo acquire).
    void release(const QString& path);

    // Acesso compartilhado: entry (com decoder) para N leitores. Abre o
    // arquivo se preciso; devolve nullptr só se o open falhar. Thread-safe
    // para uso concorrente do MESMO decoder (mutex interno do FFmpegDecoder).
    std::shared_ptr<FFmpegDecoder> acquireShared(const QString& path);

    // Nº de arquivos atualmente abertos (para diagnóstico/overlay).
    int count() const;

    // Já existe entrada aberta para este caminho (quente)?
    bool contains(const QString& path) const;

private:
    struct Entry {
        std::shared_ptr<FFmpegDecoder> dec; // criado no .cpp
        bool busy = false;
        qint64 lastUseMs = 0; // último acquire/release/shared (LRU)
    };

    void evictIdleLocked(int keep);

    int m_capacity;
    mutable QMutex m_mutex;
    QHash<QString, std::shared_ptr<Entry>> m_entries;
    QElapsedTimer m_clock;
};
