// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QPair>
#include <QVector>

#include <algorithm>
#include <cstdint>

// Álgebra pura de intervalos e janelas usada pelo AudioConformCache. Está
// separada da classe de propósito: a fusão de intervalos é a única parte do
// conform onde um off-by-one produz um bug silencioso (uma fresta de uma
// amostra entre dois "intervalos contíguos" passa a ler silêncio e o áudio
// dá um estalo), e aqui é testável sem threads, mutexes nem FFmpeg.
namespace AudioConformIntervals {

// [inicio, fim) em frames. Adjacência funde exactamente: [0,10) e [10,20)
// tornam-se um só intervalo, porque não existe amostra entre eles.
using FrameRange = QPair<qint64, qint64>;

// [inicio, fim) em segundos — as janelas que o mixer pede.
using Window = QPair<double, double>;

namespace detail {
template <typename Range>
void sortByStart(QVector<Range>& v) {
    std::sort(v.begin(), v.end(), [](const Range& x, const Range& y) {
        if (x.first != y.first) return x.first < y.first;
        return x.second < y.second;
    });
}
} // namespace detail

// Funde intervalos que se sobrepõem ou que são contíguos. Devolve ordenado e
// sem sobreposições — é o invariante de que firstUncoveredFrame depende.
inline QVector<FrameRange> mergeIntervals(QVector<FrameRange> ranges) {
    if (ranges.size() < 2) return ranges;
    detail::sortByStart(ranges);
    QVector<FrameRange> merged;
    merged.reserve(ranges.size());
    for (const auto& r : ranges) {
        if (merged.isEmpty() || r.first > merged.last().second) {
            merged.append(r);
        } else {
            merged.last().second = std::max(merged.last().second, r.second);
        }
    }
    return merged;
}

// Funde janelas que se sobrepõem OU que distam menos de eps. O eps existe
// para o caso de dois pedidos que quase se tocam: se ficassem separados,
// sobrava uma fresta de um sample entre eles e o worker teria de voltar
// atrás para a preencher. eps = 0 dá a fusão exacta, como mergeIntervals.
inline QVector<Window> coalesceWindows(QVector<Window> windows, double eps) {
    if (windows.size() < 2) return windows;
    detail::sortByStart(windows);
    QVector<Window> merged;
    merged.reserve(windows.size());
    for (const auto& w : windows) {
        if (merged.isEmpty() || w.first > merged.last().second + eps) {
            merged.append(w);
        } else {
            merged.last().second = std::max(merged.last().second, w.second);
        }
    }
    return merged;
}

// Primeiro frame de [startFrame, endFrame) que `filled` ainda não cobre;
// -1 se estiver tudo coberto.
//
// `filled` tem de vir de mergeIntervals: só com os intervalos ordenados e
// fundidos é seguro parar no primeiro acerto, porque nenhum intervalo
// seguinte pode voltar a conter o ponto de partida. Passar uma lista por
// fundir daria um -1 falso.
inline qint64 firstUncoveredFrame(const QVector<FrameRange>& filled,
                                  qint64 startFrame, qint64 endFrame) {
    qint64 a = startFrame;
    for (const auto& f : filled) {
        if (f.first <= a && a < f.second) { a = f.second; break; }
    }
    return a < endFrame ? a : -1;
}

} // namespace AudioConformIntervals
