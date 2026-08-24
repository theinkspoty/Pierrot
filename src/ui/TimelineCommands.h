// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QString>
#include <QStringList>
#include "models/Project.h"

class TimelineWidget;

// Operações de edição da timeline: corte, movimentação, exclusão, ripple,
// rolling, slip, slide, rate stretch. Extraídas do TimelineWidget para
// reduzir o tamanho do arquivo principal e facilitar manutenção/testes.
namespace TimelineCommands {

// ── Corte ──────────────────────────────────────────────────────────────
// Divide um clipe (e todos os membros do seu grupo) na posição t.
void splitClipAt(TimelineWidget* tl, Clip* c, double t);
// Versão com saída: retorna IDs dos clipes criados (metade direita).
void splitClipAt(TimelineWidget* tl, Clip* c, double t, QStringList* newIds);

// Divide TODOS os clipes que cruzam a posição t (estilo Vegas).
void cutAtPlayhead(TimelineWidget* tl);

// Corta e deleta os clipes da direita numa única operação undo (S+D).
void cutAndDelete(TimelineWidget* tl);

// Divide clipes na posição t (ferramenta tesoura).
void razorSplitAt(TimelineWidget* tl, double t);

// ── Exclusão ───────────────────────────────────────────────────────────
// Remove clipes selecionados e fecha lacunas (ripple).
void deleteSelected(TimelineWidget* tl);
// Remove clipes selecionados e deixa lacunas.
void deleteSelectedLeaveGap(TimelineWidget* tl);
// Remove clipes na região de loop com ripple.
void deleteLoopRipple(TimelineWidget* tl);
// Remove clipes na região de loop e deixa lacunas.
void deleteLoopLeaveGap(TimelineWidget* tl);
// Remove clipe à esquerda do playhead.
void deleteClipBeforePlayhead(TimelineWidget* tl);
// Remove clipe à direita do playhead.
void deleteClipAfterPlayhead(TimelineWidget* tl);

// ── Ripple Edit ────────────────────────────────────────────────────────
// Trim esquerdo com ripple: ajusta in/dur e desloca clipes posteriores.
void rippleTrimLeft(TimelineWidget* tl, Clip* c, double newIn);
// Trim direito com ripple: ajusta dur e desloca clipes posteriores.
void rippleTrimRight(TimelineWidget* tl, Clip* c, double newDur);

// ── Rolling Edit ───────────────────────────────────────────────────────
// Ajusta a fronteira entre dois clipes sem mudar a duração total.
void rollingEdit(TimelineWidget* tl, Clip* clipA, Clip* clipB, double delta);

// ── Slip Edit ──────────────────────────────────────────────────────────
// Mudar in/out de um clipe sem mudar sua posição na timeline.
void slipEdit(TimelineWidget* tl, Clip* c, double newIn);

// ── Slide Edit ─────────────────────────────────────────────────────────
// Mover um clipe e ajustar clipes adjacentes.
void slideEdit(TimelineWidget* tl, Clip* c, double origPos,
               double origDurPrev, double origDurNext,
               double origPosNext, double delta);

// ── Rate Stretch ───────────────────────────────────────────────────────
// Mudar velocidade para preencher espaço específico.
void rateStretch(TimelineWidget* tl, Clip* c, double origDur, double origSpeed, double newSpeed);

// ── Utilitários ────────────────────────────────────────────────────────
// Move clipe para outra faixa.
bool moveClipToTrack(TimelineWidget* tl, const QString& id, int row, bool audio);
// Remove clipes por IDs.
void removeClipsByIds(TimelineWidget* tl, const QStringList& ids);
// Duplica um clipe.
void duplicateClip(TimelineWidget* tl, Clip* c);
// Desloca clipes selecionados (nudge).
void nudgeSelected(TimelineWidget* tl, int dir);

} // namespace TimelineCommands
