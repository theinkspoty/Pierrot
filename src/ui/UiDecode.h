// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QImage>
#include <QString>

class FFmpegDecoder;
class QWidget;

// Ponto único de decodificação síncrona no thread da UI.
//
// Toda decodificação pontual feita na thread da UI passa por aqui. Duas regras
// que antes cada painel re-inventava (e o PivotCanvas não tinha):
//
//  - NÃO decodifica com o widget oculto. Um dock escondido não desenha nada,
//    então abrir decoder era custo puro no thread da UI — e abrir/decodificar
//    aqui já derrubou o app (VAAPI disputando device com os workers do
//    preview, SIGSEGV em avcodec_send_packet; ver PantaloneWidget::loadFrame).
//    Quando o gate fica visível, o chamador decodifica no showEvent.
//
//  - Reaproveita o decoder do widget: reabre só quando o arquivo muda e chama
//    releaseBuffers() depois do frame (devolve a RAM da resolução cheia; o
//    decoder volta a funcionar porque cada frameAt re-seek/flush).
//
// `gate` = widget dono (nullptr = sempre decodifica).
// `dec`  = decoder persistente do widget — o chamador decide política de
//          hardware (setHardwareDecodeAllowed) antes de chamar.
namespace uidecode {

QImage decode(const QWidget* gate, FFmpegDecoder& dec,
              const QString& path, double srcT, int maxWidth);

} // namespace uidecode
