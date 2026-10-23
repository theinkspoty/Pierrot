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

void PantaloneWidget::toggleKeyframe(int prop) {
    QVector<Keyframe>* kf = keyframesFor(prop);
    if (!kf) return;
    const double rel = relPlayhead();
    if (rel < 0.0) return;
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    // Sempre informa a propriedade editada, para o editor de curvas exibir a
    // curva correspondente (inclusive as da máscara).
    emit propertyEdited(prop);
    for (int i = 0; i < kf->size(); ++i) {
        if (std::fabs((*kf)[i].time - rel) < 1e-6) {
            kf->removeAt(i);
            emitChange();
            refreshDiamonds();
            m_view->update();
            return;
        }
    }
    Keyframe k;
    k.time = rel;
    k.value = propValue(prop);
    k.interp = KfSmooth;
    kf->append(k);
    std::sort(kf->begin(), kf->end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    emitChange();
    refreshDiamonds();
    m_view->update();
}

void PantaloneWidget::writeKeyframe(int prop, double value) {
    QVector<Keyframe>* kf = keyframesFor(prop);
    if (!kf) return;
    const double rel = relPlayhead();
    if (rel < 0.0) return;
    for (int i = 0; i < kf->size(); ++i) {
        if (std::fabs((*kf)[i].time - rel) < 1e-6) {
            (*kf)[i].value = value; // atualiza o keyframe já existente
            return;
        }
    }
    Keyframe k;
    k.time = rel;
    k.value = value;
    k.interp = KfSmooth;
    kf->append(k);
    std::sort(kf->begin(), kf->end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

void PantaloneWidget::gotoKeyframe(int dir) {
    Clip* c = activeClip();
    if (!c) return;
    const QVector<int> props = activeProps();
    QVector<double> times;
    for (int prop : props) {
        QVector<Keyframe>* kf = keyframesFor(prop);
        if (!kf) continue;
        for (const Keyframe& k : *kf) times.append(c->pos + k.time);
    }
    if (times.isEmpty()) return;
    std::sort(times.begin(), times.end());
    double best = -1.0;
    if (dir > 0) {
        for (double t : times)
            if (t > m_playhead + 1e-6) { best = t; break; }
    } else {
        for (double t : times)
            if (t < m_playhead - 1e-6) best = t;
    }
    if (best >= 0.0) emit keyframeJump(best);
}

void PantaloneWidget::refreshDiamonds() {
    const double rel = relPlayhead();
    const QString on =
        QStringLiteral("QPushButton{color:%1; border:none; background:transparent; font-size:14px;}"
                       "QPushButton:hover{color:%2;}")
            .arg(themeColors().accentGold.name(),
                 themeColors().accentGold.lighter(130).name());
    const QString off =
        QStringLiteral("QPushButton{color:%1; border:none; background:transparent; font-size:14px;}"
                       "QPushButton:hover{color:%2;}")
            .arg(themeColors().iconMuted.name(),
                 themeColors().text.name());
    for (auto it = m_kfDiamonds.begin(); it != m_kfDiamonds.end(); ++it) {
        bool active = false;
        if (rel >= 0.0) {
            QVector<Keyframe>* kf = keyframesFor(it.key());
            if (kf) {
                for (const Keyframe& k : *kf) {
                    if (std::fabs(k.time - rel) < 1e-6) { active = true; break; }
                }
            }
        }
        it.value()->setStyleSheet(active ? on : off);
    }
    if (m_strip) m_strip->update();
}

void PantaloneWidget::commitSlider(int prop, double baseValue) {
    Clip* c = activeClip();
    if (!c) return;
    // Empurra o undo UMA vez por gesto; o propertyEdited é emitido sempre
    // (o editor de curvas ignora repetições da mesma propriedade).
    if (!m_undoPushed) { emit editStart(); m_undoPushed = true; }
    emit propertyEdited(prop);
    switch (prop) {
        case P_CropL: c->cropL = baseValue; break;
        case P_CropR: c->cropR = baseValue; break;
        case P_CropT: c->cropT = baseValue; break;
        case P_CropB: c->cropB = baseValue; break;
        case P_Scale: c->scale = baseValue; break;
        case P_PanX:  c->tx = baseValue; break;
        case P_PanY:  c->ty = baseValue; break;
        case P_Rotation: c->rotation = baseValue; break;
        case P_ScaleX: c->scaleX = baseValue; break;
        case P_ScaleY: c->scaleY = baseValue; break;
        case P_AnchorX: c->anchorX = baseValue; break;
        case P_AnchorY: c->anchorY = baseValue; break;
        case P_Opacity: c->opacity = baseValue; break;
    }
    if (m_kfAuto && m_kfAuto->isChecked())
        writeKeyframe(prop, baseValue);
    else if (QVector<Keyframe>* kf = keyframesFor(prop))
        kf->clear();
    // modified() DEPOIS de escrever o keyframe, senão o editor de curvas
    // atualiza com dados antigos e o keyframe novo nunca aparece.
    emit modified();
    refreshDiamonds();
    // Clipe de texto: o frame é gerado com o transform do clipe, então
    // recarrega para o texto acompanhar pan/zoom/rotação na janelinha.
    if (c->isText) loadFrame();
}

void PantaloneWidget::setCropValues(double L, double R, double T, double B) {
    Clip* c = activeClip();
    if (!c) return;
    L = std::clamp(L, 0.0, 1.0);
    R = std::clamp(R, 0.0, 1.0 - L);
    T = std::clamp(T, 0.0, 1.0);
    B = std::clamp(B, 0.0, 1.0 - T);
    QSignalBlocker b1(m_cropL), b2(m_cropR), b3(m_cropT), b4(m_cropB);
    m_cropL->setValue((int)std::lround(L * 100.0));
    m_cropR->setValue((int)std::lround(R * 100.0));
    m_cropT->setValue((int)std::lround(T * 100.0));
    m_cropB->setValue((int)std::lround(B * 100.0));
    commitSlider(P_CropL, L);
    commitSlider(P_CropR, R);
    commitSlider(P_CropT, T);
    commitSlider(P_CropB, B);
    updateValueLabels();
    m_view->update();
}

void PantaloneWidget::emitChange() {
    if (!m_undoPushed) {
        emit editStart();
        m_undoPushed = true;
    }
    emit modified();
}

void PantaloneWidget::computeView(double s, double tx, double ty, int w0, int h0,
                                QRectF* cropS, QRectF* outS) const {
    if (w0 <= 0 || h0 <= 0) return;
    const double L = std::clamp(m_cropL->value() / 100.0, 0.0, 1.0);
    const double R = std::clamp(m_cropR->value() / 100.0, 0.0, 1.0 - L);
    const double T = std::clamp(m_cropT->value() / 100.0, 0.0, 1.0);
    const double B = std::clamp(m_cropB->value() / 100.0, 0.0, 1.0 - T);
    const double kx = L * w0;
    const double ky = T * h0;
    const double kw = std::max(1.0, w0 * (1.0 - L - R));
    const double kh = std::max(1.0, h0 * (1.0 - T - B));
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    // Modelo CONTAIN (letterbox), igual ao preview/exportação: o recorte é
    // encaixado inteiro no quadro, sem cortar imagens não-16:9. Antes usava
    // "cover" (max) e a janela de saída recortava a imagem — caixa de recorte
    // "desalinhada" em relação ao resultado renderizado.
    const double k = std::min(W / kw, H / kh);
    const double ks = std::max(k * s, 1e-6);
    // Janela de saída em px do source: pode ser maior que o recorte quando há
    // barras (contain); limita ao recorte para a caixa ficar sobre a imagem.
    const double w = std::min(kw, W / ks);
    const double h = std::min(kh, H / ks);
    *cropS = QRectF(kx, ky, kw, kh);
    // Centro da janela dentro do recorte (centro do recorte - pan/escala).
    const double cx = kx + kw / 2.0 - tx / ks;
    const double cy = ky + kh / 2.0 - ty / ks;
    // As bordas vêm de `kx + kw - w`. Com w == kw a matemática diz hi == lo,
    // mas (kx + kw) - w arredonda e pode cair um ULP abaixo de kx, violando a
    // pré-condição de std::clamp (hi >= lo) e abortando o app no paint. O
    // std::max(garante hi >= lo) em qualquer estado de ponto flutuante.
    const double lx = std::clamp(cx - w / 2.0, kx, std::max(kx, kx + kw - w));
    const double ly = std::clamp(cy - h / 2.0, ky, std::max(ky, ky + kh - h));
    *outS = QRectF(lx, ly, w, h);
}

// Rotaciona um ponto da tela de volta para o espaço não-rotacionado do
// viewfinder, girando por -deg em torno do centro da área de exibição (disp).
// É o inverso do p.rotate(deg) usado no paintViewfinder, para que o hit-test
// e o arraste continuem funcionando com a imagem girada.
QPointF PantaloneWidget::rotatedViewPos(const QRect& viewRect, const QPoint& sp) const {
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return QPointF(sp);
    const QRectF disp = computeDisp(viewRect);
    const double a = m_rotation->value() * M_PI / 180.0;
    const double c = std::cos(a);
    const double s = std::sin(a);
    const QPointF ctr = disp.center();
    const double dx = sp.x() - ctr.x();
    const double dy = sp.y() - ctr.y();
    // R(-a): x' = x·cos + y·sin; y' = -x·sin + y·cos
    return QPointF(ctr.x() + dx * c + dy * s,
                   ctr.y() - dx * s + dy * c);
}

void PantaloneWidget::screenToSource(const QRect& viewRect, const QPoint& sp,
                                   double* sx, double* sy) const {
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    if (w0 <= 0 || h0 <= 0) return;
    const QRectF disp = computeDisp(viewRect);
    // Desfaz a rotação antes de mapear para o espaço do source, senão o
    // recorte/pan "descolam" do mouse quando a imagem está girada.
    const QPointF rp = rotatedViewPos(viewRect, sp);
    *sx = (rp.x() - disp.left()) / disp.width() * w0;
    *sy = (rp.y() - disp.top()) / disp.height() * h0;
}

void PantaloneWidget::applyPan(double sx, double sy) {
    Clip* c = activeClip();
    if (!c || m_frame.isNull()) return;
    const int w0 = m_frame.width();
    const int h0 = m_frame.height();
    const double L = std::clamp(m_cropL->value() / 100.0, 0.0, 1.0);
    const double R = std::clamp(m_cropR->value() / 100.0, 0.0, 1.0 - L);
    const double T = std::clamp(m_cropT->value() / 100.0, 0.0, 1.0);
    const double B = std::clamp(m_cropB->value() / 100.0, 0.0, 1.0 - T);
    const double kx = L * w0;
    const double ky = T * h0;
    const double kw = std::max(1.0, w0 * (1.0 - L - R));
    const double kh = std::max(1.0, h0 * (1.0 - T - B));
    const double W = m_project ? m_project->width : 1920.0;
    const double H = m_project ? m_project->height : 1080.0;
    // Modelo CONTAIN, igual ao computeView/preview/exportação.
    const double k = std::min(W / kw, H / kh);
    const double s = std::clamp(m_scale->value() / 100.0, kMinScale, kMaxScale);
    const double ks = std::max(k * s, 1e-6);
    const double w = std::min(kw, W / ks);
    const double h = std::min(kh, H / ks);

    const double ox = kx + kw / 2.0;
    const double oy = ky + kh / 2.0;
    double cx = sx;
    double cy = sy;
    if (w >= kw) cx = ox;
    else cx = std::clamp(cx, kx + w / 2.0, std::max(kx + w / 2.0, kx + kw - w / 2.0));
    if (h >= kh) cy = oy;
    else cy = std::clamp(cy, ky + h / 2.0, std::max(ky + h / 2.0, ky + kh - h / 2.0));

    const double tx = (ox - cx) * ks;
    const double ty = (oy - cy) * ks;

    QSignalBlocker b6(m_panX), b7(m_panY);
    m_panX->setValue((int)std::lround(std::clamp(tx / W * 100.0, -100.0, 100.0)));
    m_panY->setValue((int)std::lround(std::clamp(ty / H * 100.0, -100.0, 100.0)));
    commitSlider(P_PanX, tx);
    commitSlider(P_PanY, ty);
    m_view->update();
}

