// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QString>
#include <list>
#include <memory>

class FFmpegDecoder;

// Pool GLOBAL de decodificadores "quentes" por caminho de arquivo.
//
// Diferente do antigo DecoderPool, NÃO há lease exclusivo nem flag `busy`. Todo
// acesso é compartilhado: várias threads podem segurar o mesmo decoder, e a
// serialização interna do próprio FFmpegDecoder (m_mutex) garante a segurança.
//
// A consequência de desenho é a importante: get()/acquire() NUNCA devolvem
// nullptr por "ocupado". Se o caminho já existe, ele é reusado (ref-counted);
// se não existe e o pool passou do teto, o mais frio é despejado (LRU); se não
// houver o que despejar, o pool abre além do teto por um instante. Assim não
// existe caminho que recuse acesso a uma fonte — e a classe de falha "sem slot
// / ocupado" (quadro vazio por bloqueio) deixa de existir por construção.
//
// Uso:
//   SourcePool& pool = SourcePool::instance();
//   std::shared_ptr<FFmpegDecoder> dec = pool.acquire(path);
//   if (dec) { ... dec->frameAt(t); }   // dec sobrevive ao despejo do pool
//
// O ponteiro shared_ptr mantém a entrada viva mesmo que o pool a despeje, então
// quem já está decodificando nunca perde o decoder no meio do quadro.
//
// release() é mantido apenas por compatibilidade de chamada e é no-op: não há
// posse a devolver. O lastUseMs é atualizado no acquire e no release, para a
// evicção LRU refletir a última utilização real.
class SourcePool {
public:
    // Instância global do processo. Capacidade: PIERROT_DECODER_POOL=<n>
    // (mín. 2), padrão 10.
    static SourcePool& instance();

    explicit SourcePool(int capacity);
    ~SourcePool();

    // Devolve um decoder aberto em `path` (reusa o quente ou abre agora).
    // NUNCA devolve nullptr por "ocupado" — só se o open() falhar de fato.
    std::shared_ptr<FFmpegDecoder> acquire(const QString& path);

    // Compatibilidade: não há lease a devolver. Atualiza o LRU e retorna.
    void release(const QString& path);

    // Alias histórico de acquire() (visão compartilhada). Mantido para os
    // chamadores existentes.
    std::shared_ptr<FFmpegDecoder> acquireShared(const QString& path) {
        return acquire(path);
    }

    // Nº de arquivos atualmente abertos (para diagnóstico/overlay).
    int count() const;

    // Já existe entrada aberta para este caminho (quente)?
    bool contains(const QString& path) const;

private:
    struct Entry {
        std::shared_ptr<FFmpegDecoder> dec; // criado no .cpp
        qint64 lastUseMs = 0;               // último acquire/release (LRU)
    };

    // Despeja entradas ociosas (use_count==1: só o próprio mapa segura) até o
    // mapa ter `keep` entradas. Nunca despeja uma entrada em uso por alguém de
    // fora (o shared_ptr mantém vivo); se nada for ocioso, retorna sem despejar
    // e o pool ultrapassa o teto temporariamente.
    void evictIdleLocked(int keep);

    int m_capacity;
    mutable QMutex m_mutex;
    QHash<QString, std::shared_ptr<Entry>> m_entries;
    QElapsedTimer m_clock;
};
