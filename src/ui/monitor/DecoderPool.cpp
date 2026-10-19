// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/monitor/DecoderPool.h"

#include "laartman/FFmpegDecoder.h"

DecoderPool::DecoderPool(int capacity)
    : m_capacity(qMax(2, capacity)) {
    m_clock.start();
}

DecoderPool::~DecoderPool() {
    // As threads de decode já foram paradas (PreviewWidget::~) antes deste
    // ponto; os FFmpegDecoder restantes morrem aqui, fechando seus arquivos.
    QMutexLocker l(&m_mutex);
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it)
        delete it.value();
    m_entries.clear();
}

DecoderPool::Entry::~Entry() { delete dec; }

FFmpegDecoder* DecoderPool::acquire(const QString& path) {
    QMutexLocker l(&m_mutex);
    auto it = m_entries.find(path);
    if (it != m_entries.end()) {
        if (it.value()->busy) return nullptr;
        it.value()->busy = true;
        it.value()->lastUseMs = m_clock.elapsed();
        return it.value()->dec;
    }
    // Caminho novo: despeja ociosos (LRU) até sobrar 1 slot. Se sobrar tudo
    // ocupado, o chamador libera o próprio lease e tenta de novo.
    evictIdleLocked(m_capacity - 1);
    if ((int)m_entries.size() >= m_capacity) return nullptr;
    auto e = new Entry();
    e->dec = new FFmpegDecoder();
    e->busy = true;
    e->lastUseMs = m_clock.elapsed();
    FFmpegDecoder* d = e->dec;
    m_entries.insert(path, e);
    l.unlock();
    // open() é lento (~500-800ms): fora do mutex. Falha remove a entrada.
    // (remove() no mapa de Entry* só apaga a chave; o objeto morre no delete.)
    if (!d->open(path)) {
        QMutexLocker l2(&m_mutex);
        if (m_entries.value(path) == e) m_entries.remove(path);
        delete e;
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
            if (it.value()->busy) continue;
            if (it.value()->lastUseMs < oldest) {
                oldest = it.value()->lastUseMs;
                victim = it.key();
            }
        }
        if (victim.isEmpty()) return; // todos ocupados: não há o que despejar
        delete m_entries.take(victim); // destrói o FFmpegDecoder (fecha o arquivo)
    }
}
