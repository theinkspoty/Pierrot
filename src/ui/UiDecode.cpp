// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "UiDecode.h"

#include "laartman/FFmpegDecoder.h"

#include <QWidget>

namespace uidecode {

QImage decode(const QWidget* gate, FFmpegDecoder& dec,
              const QString& path, double srcT, int maxWidth) {
    // Dock/panel oculto não desenha nada: sem decode. O chamador recarrega
    // no showEvent (mesmo contrato de PantaloneWidget::loadFrame).
    if (gate && !gate->isVisible()) return QImage();
    if (path.isEmpty()) return QImage();

    // open() já troca de arquivo em cascata (freeAllLocked + reopen) e tem
    // atalho de "mesmo path já aberto" — então é seguro chamar sempre.
    if (!dec.open(path)) return QImage();

    const QImage img = dec.frameAt(srcT, maxWidth);
    dec.releaseBuffers();
    return img;
}

} // namespace uidecode
