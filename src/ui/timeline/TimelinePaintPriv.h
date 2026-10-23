// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Helpers compartilhados entre os .cpp de TimelinePaint (base, Clips, Header,
// Fx). Viviam como anon-namespace no .cpp único — com o corte atravessariam
// unidades de tradução.

#pragma once

#include "ui/timeline/TimelineWidget.h"
#include "colombina/models/Project.h"
#include "ui/core/Theme.h"

#include <QColor>
#include <QString>
#include <QVector>

namespace {


// Vermelho da seção de gravação (divisória e destaque de seleção). O
// `Track::color` das faixas de gravação já vem vermelho do modelo.
QColor recColor() { return recordingTrackColor(); }

// Qual das três seções da timeline um row/faixa pertence.
enum class Zone { Video, Audio, Rec };

QString fmtRuler(double t) {
    const int total = (int)std::floor(t);
    return QString("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

// Paleta automática de cores de faixa de VÍDEO (estilo Vegas), usada quando
// a faixa não tem cor própria definida pelo usuário.
QColor autoTrackColor(int index) {
    // Paleta em tons de azul (estilo "cool"): matizes que variam de nobre
    // profundo a ciano, mantendo distinção entre faixas vizinhas.
    static const QColor pal[] = {
        QColor(66, 108, 188),  // azul profundo
        QColor(60, 154, 190),  // ciano aço
        QColor(52, 78, 150),   // navy
        QColor(88, 162, 214),  // celeste
        QColor(96, 92, 178),   // índigo
        QColor(54, 128, 158),  // azul-petróleo
        QColor(58, 122, 220),  // azul elétrico
        QColor(64, 162, 168),  // turquesa azulado
        QColor(48, 66, 132),   // azul-marinho
        QColor(104, 140, 196), // aço claro
    };
    return pal[index % (int)(sizeof pal / sizeof pal[0])];
}

// Paleta automática de cores de faixas de ÁUDIO (labels padrão do Adobe
// Premiere), usada quando a faixa não tem cor própria definida pelo usuário.
// A1 verde, A2 verde-água, A3 azul, A4 violeta, A5 âmbar, A6 cinza.
QColor autoAudioColor(int index) {
    static const QColor pal[] = {
        QColor(80, 168, 94),   // geo
        QColor(62, 152, 168),  // verde-água
        QColor(84, 112, 190),  // azul
        QColor(132, 100, 178), // violeta
        QColor(180, 136, 68),  // âmbar
        QColor(122, 132, 142), // cinza
    };
    return pal[index % (int)(sizeof pal / sizeof pal[0])];
}

// Cor efetiva de uma faixa para desenho (própria se definida, senão paleta).
QColor trackColorAt(const Track& tr, int index) {
    if (tr.color.isValid()) return tr.color;
    return tr.audio ? autoAudioColor(index) : autoTrackColor(index);
}

// Cor sólida de uma faixa de ÁUDIO no matiz/saturação do label, com a
// luminosidade `val` fornecida (estilo Premiere: pista/clipe/header pintados
// na cor da faixa em tons sólidos, nunca em cinza escuro).
QColor audioSolidColor(const Track& tr, int index, qreal val) {
    QColor c = trackColorAt(tr, index);
    qreal hue = c.hslHueF();
    if (hue < 0.0) hue = 0.36;
    qreal sat = c.hslSaturationF();
    if (sat <= 0.0) sat = 0.65;
    return QColor::fromHslF(hue, sat, val);
}

// Texto legível sobre uma cor de fundo: claro -> letra escura, escuro ->
// letra clara. Usado na faixa de nome dos clipes (áudio e vídeo), que é a
// mesma anatomia nos dois.
QColor readableTextOn(const QColor& bg) {
    return bg.lightnessF() > 0.55 ? QColor(0x1A, 0x1A, 0x1A) : QColor(0xEA, 0xEA, 0xEA);
}
}
