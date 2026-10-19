// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "laartman/DecoderPool.h"

#include "laartman/FFmpegDecoder.h"

DecoderPool& DecoderPool::instance() {
    static DecoderPool pool([] {
        bool ok = false;
        const int n = qEnvironmentVariableIntValue("PIERROT_DECODER_POOL", &ok);
        return (ok && n >= 2) ? n : 10;
    }());
    return pool;
}

DecoderPool::DecoderPool(int capacity)
    : m_capacity(qMax(2, capacity)) {
    m_clock.start();
}

DecoderPool::~DecoderPool() {
    // As threads de decode já foram paradas (PreviewWidget::~) antes deste
    // ponto; os FFmpegDecoder restantes morrem aqui, fechando seus arquivos.
    QMutexLocker l(&m_mutex);
    m_entries.clear();
}

FFmpegDecoder* DecoderPool::acquire(const QString& path) {
    QMutexLocker l(&m_mutex);
    auto it = m_entries.find(path);
    if (it != m_entries.end()) {
        if (it.value()->busy) return nullptr;
        it.value()->busy = true;
        it.value()->lastUseMs = m_clock.elapsed();
        return it.value()->dec.get();
    }
    // Caminho novo: despeja ociosos (LRU) até sobrar 1 slot. Se sobrar tudo
    // ocupado, o chamador libera o próprio lease e tenta de novo.
    evictIdleLocked(m_capacity - 1);
    if ((int)m_entries.size() >= m_capacity) return nullptr;
    auto e = std::make_shared<Entry>();
    e->dec = std::make_shared<FFmpegDecoder>();
    e->busy = true;
    e->lastUseMs = m_clock.elapsed();
    FFmpegDecoder* d = e->dec.get();
    m_entries.insert(path, e);
    l.unlock();
    // open() é lento (~500-800ms): fora do mutex. Falha remove a entrada.
    if (!d->open(path)) {
        QMutexLocker l2(&m_mutex);
        if (m_entries.value(path) == e) m_entries.remove(path);
        return nullptr;
    }
    return d;
}

void DecoderPool::release(const QString& path) {
    QMutexLocker l(&m_mutex);
    auto it = m_entries.find(path);
    if (it != m_entries.end() && it.value()->busy) {
        it.value()->busy = false;
        it.value()->lastUseMs = m_clock.elapsed();
    }
}

std::shared_ptr<FFmpegDecoder> DecoderPool::acquireShared(const QString& path) {
    std::shared_ptr<Entry> e;
    {
        QMutexLocker l(&m_mutex);
        auto it = m_entries.find(path);
        if (it != m_entries.end()) {
            it.value()->lastUseMs = m_clock.elapsed();
            e = it.value();
        } else {
            // Só despeja ociosas; se tudo estiver em uso, deixa passar do teto
            // (derrubar decoder em uso é pior que memória temporária).
            evictIdleLocked(m_capacity - 1);
            e = std::make_shared<Entry>();
            e->dec = std::make_shared<FFmpegDecoder>();
            e->lastUseMs = m_clock.elapsed();
            m_entries.insert(path, e);
        }
    }
    // open() fora do mutex do pool; o FFmpegDecoder serializa aberturas
    // concorrentes do MESMO decoder (checagem idempotente sob o lock dele).
    if (!e->dec->isOpen() && !e->dec->open(path)) {
        QMutexLocker l(&m_mutex);
        if (m_entries.value(path) == e) m_entries.remove(path);
        return nullptr;
    }
    return e->dec;
}

int DecoderPool::count() const {
    QMutexLocker l(&m_mutex);
    return m_entries.size();
}

bool DecoderPool::contains(const QString& path) const {
    QMutexLocker l(&m_mutex);
    return m_entries.contains(path);
}

void DecoderPool::evictIdleLocked(int keep) {
    while ((int)m_entries.size() > keep) {
        QString victim;
        qint64 oldest = m_clock.elapsed() + 1; // sentinela
        for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
            const std::shared_ptr<Entry>& e = it.value();
            // Ocioso = sem lease ativo e sem ninguém de fora segurando o
            // shared_ptr (use_count==1: só o próprio mapa).
            if (e->busy || e->dec.use_count() > 1) continue;
            if (e->lastUseMs < oldest) {
                oldest = e->lastUseMs;
                victim = it.key();
            }
        }
        if (victim.isEmpty()) return; // nada ocioso: não há o que despejar
        m_entries.remove(victim); // shared_ptr do entry destrói o decoder
    }
}
