// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "models/Project.h"

// ── Intercâmbio com outros NLEs (EDL / FCPXML) ─────────────────────────
// Ferramentas de import/export independentes de UI e de FFmpeg — puramente
// textuais e testáveis. O EDL (CMX3600) é o denominador comum entre Davinci
// Resolve, Premiere e Vegas para troca de EDITS (cortes simples), sem efeitos.

namespace NleInterchange {

// ── EDL (CMX3600) ─────────────────────────────────────────────────────

// Converte o instante (segundos, 0 = início da mídia) no formato HH:MM:SS:FF,
// respeitando o fps nominal do EDL (reel rate).
QString edlTimecode(double seconds, int fps);

// Edits de um clipe → linha EDL "001  CLIP   V  C        TCin TCout ...".
// Retorna a linha completa (sem quebra de linha final).
QString edlEditLine(int eventNumber, const QString& reel, const QString& trackId,
                    bool isAudio, const QString& transition, double srcIn,
                    double srcOut, double recIn, double recOut, int fps);

// Gera o arquivo EDL completo do projeto (apenas cortes nas faixas de vídeo e
// áudio; efeitos/transform/texto/blend não são representados em EDL e são
// ignorados — documentado no comentário de rodapé do arquivo).
QString buildEdl(const Project& project);

// Escreve o EDL em `path`. Retorna true em sucesso.
bool exportEdl(const Project& project, const QString& path, QString* error = nullptr);

// ── Importação EDL ────────────────────────────────────────────────────

// Resultado do parse: um projeto novo com as faixas e clipes do EDL. As mídias
// são criadas como MediaItem com `filePath` = nome do REEL (o caminho real
// precisa ser resolvido pelo usuário se o reel não for um caminho absoluto).
struct EdlImportResult {
    Project project;
    QVector<QString> unresolvedReels; // reels sem caminho absoluto válido
    QStringList warnings;
};

// Lê um EDL e preenche `result`. Retorna false em erro fatal (arquivo ilegível
// ou estrutura irreconhecível); avisos vão em `result.warnings`.
bool importEdl(const QString& path, EdlImportResult& result);

} // namespace NleInterchange
