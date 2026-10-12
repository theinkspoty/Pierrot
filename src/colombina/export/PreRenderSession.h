// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QImage>
#include <QProcess>
#include <QString>

namespace prerender {

// Pipe ffmpeg rawvideo RGBA -> H.264/mp4, compartilhado pelos
// pré-renderizadores LAINKA e OFX. Encapsula a montagem dos argumentos, o
// bombeamento de frames e a validação do exit code, que antes eram
// duplicados nos dois renderizadores.
class PreRenderSession {
public:
    PreRenderSession(int width, int height, int fps, const QString& tmpPath);
    ~PreRenderSession();

    PreRenderSession(const PreRenderSession&) = delete;
    PreRenderSession& operator=(const PreRenderSession&) = delete;

    // Inicia o ffmpeg. Em falha preenche error (rotulado com label) e retorna false.
    bool start(QString* error, const QString& label);

    // Converte o frame para RGBA, escreve no pipe e, em falha, aborta o processo.
    bool writeFrame(const QImage& frame, QString* error, const QString& label);

    // Fecha o pipe, aguarda o ffmpeg e valida o exit code.
    bool finish(QString* error, const QString& label);

    // Encerra imediatamente o processo ffmpeg, se ainda estiver rodando.
    void abort();

    const QString& tmpPath() const { return m_tmpPath; }

private:
    QProcess m_proc;
    QString m_tmpPath;
    int m_w = 0;
    int m_h = 0;
    int m_fps = 0;
};

// Move tmpPath para o cache permanente (CacheLocation/<dirSuffix>/<fileName>).
// Retorna o caminho final, ou vazio em falha (preenche error se fornecido).
QString publishToCache(const QString& tmpPath, const QString& dirSuffix,
                       const QString& fileName, QString* error, const QString& label);

} // namespace prerender
