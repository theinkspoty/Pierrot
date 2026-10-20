// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "colombina/ffmpeg/SourcePool.h"

#include "laartman/core/FFmpegDecoder.h"

SourcePool& SourcePool::instance() {
    static SourcePool pool([] {
        bool ok = false;
        const int n = qEnvironmentVariableIntValue("PIERROT_DECODER_POOL", &ok);
        return (ok && n >= 2) ? n : 10;
    }());
    return pool;
}

SourcePool::SourcePool(int capacity)
    : m_capacity(qMax(2, capacity)) {
    m_clock.start();
}

SourcePool::~SourcePool() {
    // As threads de decode já foram paradas (PreviewWidget::~) antes deste
    // ponto; os FFmpegDecoder restantes morrem aqui, fechando seus arquivos.
    QMutexLocker l(&m_mutex);
    m_entries.clear();
}

std::shared_ptr<FFmpegDecoder> SourcePool::acquire(const QString& path) {
    std::shared_ptr<Entry> e;
    {
        QMutexLocker l(&m_mutex);
        auto it = m_entries.find(path);
        if (it != m_entries.end()) {
            it.value()->lastUseMs = m_clock.elapsed();
            e = it.value();
        } else {
            // Caminho novo: despeja ociosos (LRU) até sobrar 1 slot. Se nada
            // for ocioso, segue além do teto (derrubar decoder em uso é pior
            // que memória temporária) — nunca devolvemos nullptr por "cheio".
            evictIdleLocked(m_capacity - 1);
            e = std::make_shared<Entry>();
            e->dec = std::make_shared<FFmpegDecoder>();
            e->lastUseMs = m_clock.elapsed();
            m_entries.insert(path, e);
        }
    }
    // open() é lento (~500-800ms): fora do mutex do pool. O FFmpegDecoder
    // serializa aberturas concorrentes do MESMO decoder (checagem idempotente
    // sob o lock dele). Falha de open remove a entrada.
    if (!e->dec->isOpen() && !e->dec->open(path)) {
        QMutexLocker l(&m_mutex);
        if (m_entries.value(path) == e) m_entries.remove(path);
        return nullptr;
    }
    return e->dec;
}

void SourcePool::release(const QString& path) {
    // Não há lease a devolver: o acesso é compartilhado. Só atualiza o LRU para
    // que a evicção reflita a última utilização real.
    QMutexLocker l(&m_mutex);
    auto it = m_entries.find(path);
    if (it != m_entries.end()) it.value()->lastUseMs = m_clock.elapsed();
}

int SourcePool::count() const {
    QMutexLocker l(&m_mutex);
    return m_entries.size();
}

bool SourcePool::contains(const QString& path) const {
    QMutexLocker l(&m_mutex);
    return m_entries.contains(path);
}

void SourcePool::evictIdleLocked(int keep) {
    while ((int)m_entries.size() > keep) {
        QString victim;
        qint64 oldest = m_clock.elapsed() + 1; // sentinela
        for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
            const std::shared_ptr<Entry>& e = it.value();
            // Ocioso = sem ninguém de fora segurando o shared_ptr
            // (use_count==1: só o próprio mapa).
            if (e->dec.use_count() > 1) continue;
            if (e->lastUseMs < oldest) {
                oldest = e->lastUseMs;
                victim = it.key();
            }
        }
        if (victim.isEmpty()) return; // tudo em uso: não há o que despejar
        m_entries.remove(victim); // shared_ptr do entry destrói o decoder
    }
}
