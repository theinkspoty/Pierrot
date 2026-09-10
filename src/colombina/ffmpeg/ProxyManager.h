// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

// ProxyManager — gera e gerencia versões leves das mídias (proxies) para o
// preview e thumbs. Vídeos de alta resolução (2K/4K/8K) são transcondificados
// em background para H.264 de baixo bitrate em metade da resolução; o preview
// decodifica o proxy (rápido) enquanto a exportação continua usando o ORIGINAL.
//
// Local: diretório de cache do sistema (QStandardPaths::CacheLocation).
// Proxies são VÍDEO-apenas (-an); o áudio do preview sempre usa o arquivo
// original (por isso resolveVideo() só é chamado nos pontos de decode de vídeo).

#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QThread>
#include <QMutex>

class ProxyWorker : public QObject {
    Q_OBJECT
public:
    void process(const QString& srcPath, const QString& proxyPath);
signals:
    void proxyReady(const QString& srcPath, const QString& proxyPath);
    void proxyFailed(const QString& srcPath);
};

class ProxyManager : public QObject {
    Q_OBJECT
public:
    static ProxyManager& instance();
    ~ProxyManager();

    // Limiar de resolução (px de largura) que dispara a geração de proxy.
    static constexpr int kThresholdWidth = 2560;

    // Path a usar para DECODE DE VÍDEO de preview/thumbs: o proxy se existir
    // (e estiver habilitado), senão o original. Determinístico (mesmo src →
    // mesmo proxy), para evitar cache keys inconsistentes.
    QString resolveVideo(const QString& srcPath) const;
    bool hasProxy(const QString& srcPath) const;

    // Programa a geração de proxy para `srcPath` se a mídia for grande e
    // ainda não houver proxy. Não bloqueia; roda em background.
    void probeAndQueue(const QString& srcPath);

    // Desliga proxies (debug/forçar qualidade cheia no preview).
    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }

    int pendingJobCount() const { return m_pending.count(); }

signals:
    void proxyReady(const QString& srcPath);
    void proxyFailed(const QString& srcPath);
    void busyChanged(bool busy);
    void startJob(const QString& srcPath, const QString& proxyPath);

public slots:
    void clear();

private slots:
    void onProxyReady(const QString& srcPath, const QString& proxyPath);
    void onProxyFailed(const QString& srcPath);

private:
    ProxyManager();
    QString proxyDir() const;
    QString proxyPathFor(const QString& srcPath) const;
    void loadState();
    void saveState() const;
    void pump();

    QThread* m_thread = nullptr;
    ProxyWorker* m_worker = nullptr;

    QSet<QString> m_pending;  // srcs na fila
    QString m_activeSrc;    // srcs atualmente em processamento
    bool m_running = false;
    bool m_enabled = true;

    // Estado persistido: src → proxy.
    QHash<QString, QString> m_map;
    QSet<QString> m_failed; // srcs que falharam (não re-tentar por ora)
    QString m_stateFile;

    // Cache de "não é candidato" (largura abaixo do limiar).
    mutable QSet<QString> m_small;

    // Protege os mapas de estado, lidos de threads de workers (MediaCache).
    mutable QMutex m_mutex;
};
