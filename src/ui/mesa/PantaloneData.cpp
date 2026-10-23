// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/mesa/PantaloneWidget.h"
#include "ui/core/Theme.h"
#include "ui/monitor/UiDecode.h"
#include "colombina/ffmpeg/ProxyManager.h"
#include "colombina/generators.h"
#include "colombina/render/TextRender.h"

#include <QPainter>
#include <QPainterPath>
#include <QGroupBox>
#include <QPushButton>
#include <QToolButton>
#include <QSlider>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QShortcut>
#include <QKeySequence>
#include <QScrollArea>
#include <QListWidget>
#include <QCheckBox>
#include <QComboBox>
#include <functional>
#include <algorithm>
#include <cmath>

#include "ui/mesa/PantalonePriv.h"

void PantaloneWidget::setProject(Project* p) {
    m_project = p;
    m_clipId.clear();
    m_frame = QImage();
    m_framePath.clear();
    m_undoPushed = false;
    m_maskSel = -1;
    m_maskDragHandle = -1;
    refreshMaskList();
    syncMaskControls();
    m_view->update();
}

void PantaloneWidget::setClipId(const QString& id) {
    if (m_clipId == id) return;
    m_clipId = id;
    m_undoPushed = false;
    m_maskDragHandle = -1;
    loadFrame();
    syncFromClip();
    refreshMaskList();
    syncMaskControls();
    m_view->update();
    m_strip->update();
}

void PantaloneWidget::setPlayhead(double t) {
    // Só recarrega o frame se a mudança de tempo for significativa (>50ms)
    // para evitar decodificar frame a frame durante a reprodução.
    const bool needReload = std::fabs(t - m_playhead) > 0.05 || m_frame.isNull();
    m_playhead = t;
    if (needReload) loadFrame();
    syncFromClip();
    m_view->update();
    m_strip->update();
}

void PantaloneWidget::refresh() {
    loadFrame();
    syncFromClip();
    m_view->update();
}

void PantaloneWidget::sync() {
    syncFromClip();
    m_view->update();
}

void PantaloneWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // MainWindow chama setClipId() ANTES de m_pantaloneDock->show(), então o
    // loadFrame() daquele momento foi descartado pela guarda de visibilidade.
    loadFrame();
    syncFromClip();
    m_view->update();
}

Clip* PantaloneWidget::activeClip() {
    if (!m_project || m_clipId.isEmpty()) return nullptr;
    for (Track& t : m_project->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == m_clipId) return &c;
    return nullptr;
}

void PantaloneWidget::loadFrame() {
    // O dock do pantalone nasce escondido (MainWindow), mas continua ligado ao
    // playhead e à seleção da timeline. Sem esta guarda, TODO arasto do
    // playhead disparava uma decodificação VAAPI SÍNCRONA no thread da UI,
    // concorrente com os workers de preview — e derrubava o app com SIGSEGV
    // dentro de avcodec_send_packet. Escondido, o painel não desenha nada,
    // então não há nada a decodificar.
    if (!isVisible()) {
        m_frame = QImage();
        m_framePath.clear();
        return;
    }
    Clip* c = activeClip();
    if (!c) { m_frame = QImage(); m_framePath.clear(); return; }
    if (c->isText) {
        const TextStyle& st = *m_project->textStyleFor(*c);
        const int W = m_project ? m_project->width : 1920;
        const int H = m_project ? m_project->height : 1080;
        QImage img(W, H, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        const double rel = std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur));
        const double tx = kfValue(c->kfTx, c->tx, rel);
        const double ty = kfValue(c->kfTy, c->ty, rel);
        const double rot = kfValue(c->kfRotation, c->rotation, rel);
        const double s = kfValue(c->kfScale, c->scale, rel);
        const double sX = kfValue(c->kfScaleX, c->scaleX, rel);
        const double sY = kfValue(c->kfScaleY, c->scaleY, rel);
        renderTextStyleFrame(img, st, tx, ty, rot, s, sX, sY);
        m_frame = img;
        m_framePath.clear();
        return;
    }
    const MediaItem* mi = m_project->findMedia(c->mediaId);
    if (!mi || !mi->hasVideo) { m_frame = QImage(); m_framePath.clear(); return; }
    if (mi->isSolid) {
        // Mídia gerada (gerador estilo Vegas): gera o quadro sem decoder.
        const int w = qMax(1, mi->width > 0 ? mi->width : m_project->width);
        const int h = qMax(1, mi->height > 0 ? mi->height : m_project->height);
        m_frame = generatorFrame(*mi, w, h);
        m_framePath.clear();
        m_lastDecodedClipId = c->id;
        return;
    }
    const QString vpath = ProxyManager::instance().resolveVideo(mi->filePath);
    // Força reset do decoder ao trocar de clipe — mesmo que o arquivo fonte seja
    // o mesmo, o estado interno (m_lastFrame/m_nextFrame/m_lastPtsSec) fica do
    // clipe anterior e pode devolver o frame errado.
    if (m_lastDecodedClipId != c->id) {
        if (m_decoder.isOpen()) m_decoder.close();
        m_lastDecodedClipId = c->id;
    }
    // Decodifica o frame na posição atual do playhead para preview em tempo real.
    const double t = std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur - 0.05));
    // Ponto único de decode na UI (uidecode::decode): gate de visibilidade,
    // reabertura só na troca de arquivo e releaseBuffers() pós-frame — antes
    // essas regras moravam duplicadas aqui e no PivotCanvas.
    m_frame = uidecode::decode(this, m_decoder, vpath, t, 720);
    if (m_frame.isNull()) m_framePath.clear();
    else m_framePath = mi->filePath;
}

void PantaloneWidget::syncFromClip() {
    Clip* c = activeClip();
    const double rel = c ? std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur)) : 0.0;
    const double L = c ? std::clamp(kfValue(c->kfCropL, c->cropL, rel), 0.0, 0.9) : 0.0;
    const double R = c ? std::clamp(kfValue(c->kfCropR, c->cropR, rel), 0.0, 0.9) : 0.0;
    const double T = c ? std::clamp(kfValue(c->kfCropT, c->cropT, rel), 0.0, 0.9) : 0.0;
    const double B = c ? std::clamp(kfValue(c->kfCropB, c->cropB, rel), 0.0, 0.9) : 0.0;
    const double s = c ? kfValue(c->kfScale, c->scale, rel) : 1.0;
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    const double tx = c ? kfValue(c->kfTx, c->tx, rel) : 0.0;
    const double ty = c ? kfValue(c->kfTy, c->ty, rel) : 0.0;
    const double rot = c ? kfValue(c->kfRotation, c->rotation, rel) : 0.0;
    const double sx = c ? kfValue(c->kfScaleX, c->scaleX, rel) : 1.0;
    const double sy = c ? kfValue(c->kfScaleY, c->scaleY, rel) : 1.0;
    const double ax = c ? kfValue(c->kfAnchorX, c->anchorX, rel) : 0.0;
    const double ay = c ? kfValue(c->kfAnchorY, c->anchorY, rel) : 0.0;
    const double op = c ? kfValue(c->kfOpacity, c->opacity, rel) : 1.0;

    QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB);
    QSignalBlocker b5(m_scale), b6(m_panX), b7(m_panY), b8(m_rotation);
    QSignalBlocker b9(m_scaleX), b10(m_scaleY), b11(m_anchorX), b12(m_anchorY);
    QSignalBlocker b13(m_opacity);
    m_cropL->setValue((int)std::lround(L * 100.0));
    m_cropR->setValue((int)std::lround(R * 100.0));
    m_cropT->setValue((int)std::lround(T * 100.0));
    m_cropB->setValue((int)std::lround(B * 100.0));
    m_scale->setValue((int)std::lround(std::clamp(s, kMinScale, kMaxScale) * 100.0));
    m_panX->setValue((int)std::lround(std::clamp(tx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(ty / H * 100.0, -100.0, 100.0)));
    m_rotation->setValue((int)std::lround(std::clamp(rot, -double(kRotMax), double(kRotMax))));
    m_scaleX->setValue((int)std::lround(std::clamp(sx, kMinStretch / 100.0, kMaxStretch / 100.0) * 100.0));
    m_scaleY->setValue((int)std::lround(std::clamp(sy, kMinStretch / 100.0, kMaxStretch / 100.0) * 100.0));
    m_anchorX->setValue((int)std::lround(std::clamp(ax * 100.0, -100.0, 100.0)));
    m_anchorY->setValue((int)std::lround(std::clamp(ay * 100.0, -100.0, 100.0)));
    m_opacity->setValue((int)std::lround(std::clamp(op, 0.0, 1.0) * 100.0));
    updateValueLabels();
    refreshDiamonds();
}

void PantaloneWidget::updateValueLabels() {
    const int L = m_cropL->value();
    const int R = m_cropR->value();
    const int T = m_cropT->value();
    const int B = m_cropB->value();
    const int sc = m_scale->value();
    const int px = m_panX->value();
    const int py = m_panY->value();
    const int rot = m_rotation->value();
    const int sx = m_scaleX->value();
    const int sy = m_scaleY->value();
    const int ax = m_anchorX->value();
    const int ay = m_anchorY->value();
    const int op = m_opacity->value();
    auto pct = [](int v) { return QStringLiteral("%1%").arg(v); };
    if (m_cropLVal) m_cropLVal->setText(pct(L));
    if (m_cropRVal) m_cropRVal->setText(pct(R));
    if (m_cropTVal) m_cropTVal->setText(pct(T));
    if (m_cropBVal) m_cropBVal->setText(pct(B));
    if (m_scaleVal) m_scaleVal->setText(pct(sc));
    if (m_panXVal) m_panXVal->setText(pct(px));
    if (m_panYVal) m_panYVal->setText(pct(py));
    if (m_rotationVal) m_rotationVal->setText(QStringLiteral("%1°").arg(rot));
    if (m_scaleXVal) m_scaleXVal->setText(pct(sx));
    if (m_scaleYVal) m_scaleYVal->setText(pct(sy));
    if (m_anchorXVal) m_anchorXVal->setText(pct(ax));
    if (m_anchorYVal) m_anchorYVal->setText(pct(ay));
    if (m_opacityVal) m_opacityVal->setText(pct(op));
}

double PantaloneWidget::relPlayhead() {
    Clip* c = activeClip();
    if (!c) return -1.0;
    return std::clamp(m_playhead - c->pos, 0.0, std::max(0.0, c->dur));
}

QVector<Keyframe>* PantaloneWidget::keyframesFor(int prop) {
    Clip* c = activeClip();
    if (!c) return nullptr;
    switch (prop) {
        case P_CropL: return &c->kfCropL;
        case P_CropR: return &c->kfCropR;
        case P_CropT: return &c->kfCropT;
        case P_CropB: return &c->kfCropB;
        case P_Scale: return &c->kfScale;
        case P_PanX:  return &c->kfTx;
        case P_PanY:  return &c->kfTy;
        case P_Rotation: return &c->kfRotation;
        case P_ScaleX: return &c->kfScaleX;
        case P_ScaleY: return &c->kfScaleY;
        case P_AnchorX: return &c->kfAnchorX;
        case P_AnchorY: return &c->kfAnchorY;
        case P_Opacity: return &c->kfOpacity;
        // Propriedades da máscara ATIVA (nullptr se não há máscara selecionada).
        case P_MaskCx:      { Mask* m = activeMask(); return m ? &m->kfCx : nullptr; }
        case P_MaskCy:      { Mask* m = activeMask(); return m ? &m->kfCy : nullptr; }
        case P_MaskRx:      { Mask* m = activeMask(); return m ? &m->kfRx : nullptr; }
        case P_MaskRy:      { Mask* m = activeMask(); return m ? &m->kfRy : nullptr; }
        case P_MaskRot:     { Mask* m = activeMask(); return m ? &m->kfRotation : nullptr; }
        case P_MaskFeather: { Mask* m = activeMask(); return m ? &m->kfFeather : nullptr; }
    }
    return nullptr;
}

// Props que a faixa de keyframes / navegação / editor de curvas enxergam: as
// 12 do clipe e, quando há máscara selecionada, as 6 dela (motion da máscara).
QVector<int> PantaloneWidget::activeProps() {
    QVector<int> props;
    props << P_CropL << P_CropR << P_CropT << P_CropB
          << P_Scale << P_PanX << P_PanY << P_Rotation
          << P_ScaleX << P_ScaleY << P_AnchorX << P_AnchorY
          << P_Opacity;
    if (activeMask())
        props << P_MaskCx << P_MaskCy << P_MaskRx << P_MaskRy
              << P_MaskRot << P_MaskFeather;
    return props;
}

void PantaloneWidget::setViewMode(ViewMode m) {
    m_mode = m;
    for (auto it = m_modeBtns.begin(); it != m_modeBtns.end(); ++it)
        it.value()->setChecked((ViewMode)it.key() == m);
    // A sidebar segue o modo: mostra só as propriedades relevantes.
    // Em "Janelas" a máscara se edita no viewer + painel inferior, então a
    // sidebar fica enxuta (só "Redefinir").
    if (m_secCrop) m_secCrop->setVisible(m == ModeCrop);
    if (m_secMove) m_secMove->setVisible(m == ModeTransform);
    if (m_secRot)  m_secRot->setVisible(m == ModeTransform);
    if (m_secMesh) {
        m_secMesh->setVisible(m == ModeMesh);
        if (m == ModeMesh && m_meshRes) {
            Clip* c = activeClip();
            if (c && c->hasMesh()) {
                const QSignalBlocker b(m_meshRes);
                m_meshRes->setCurrentText(
                    QStringLiteral("%1×%2").arg(c->meshCols).arg(c->meshRows));
            }
        }
    }
    if (m_view) m_view->update();
}

double PantaloneWidget::propValue(int prop) {
    switch (prop) {
        case P_CropL: return m_cropL->value() / 100.0;
        case P_CropR: return m_cropR->value() / 100.0;
        case P_CropT: return m_cropT->value() / 100.0;
        case P_CropB: return m_cropB->value() / 100.0;
        case P_Scale: return m_scale->value() / 100.0;
        case P_PanX: {
            const double W = m_project ? m_project->width : 1920.0;
            return m_panX->value() / 100.0 * W;
        }
        case P_PanY: {
            const double H = m_project ? m_project->height : 1080.0;
            return m_panY->value() / 100.0 * H;
        }
        case P_Rotation:
            return m_rotation->value();
        case P_ScaleX:
            return m_scaleX->value() / 100.0;
        case P_ScaleY:
            return m_scaleY->value() / 100.0;
        case P_AnchorX:
            return m_anchorX->value() / 100.0;
        case P_AnchorY:
            return m_anchorY->value() / 100.0;
        case P_Opacity:
            return m_opacity->value() / 100.0;
        case P_MaskCx:      { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->cxAt(r) : 0.0; }
        case P_MaskCy:      { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->cyAt(r) : 0.0; }
        case P_MaskRx:      { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->rxAt(r) : 0.0; }
        case P_MaskRy:      { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->ryAt(r) : 0.0; }
        case P_MaskRot:     { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->rotAt(r) : 0.0; }
        case P_MaskFeather: { Mask* m = activeMask(); const double r = std::max(0.0, relPlayhead()); return m ? m->featherAt(r) : 0.0; }
    }
    return 0.0;
}


