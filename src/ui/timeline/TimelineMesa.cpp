// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/timeline/TimelineWidget.h"
#include "ui/timeline/TimelineCommands.h"
#include "colombina/util.h"
#include "colombina/models/ClipAttrs.h"

#include "colombina/ffmpeg/MediaCache.h"
#include "laartman/Decoder.h"
#include "colombina/mesh/BlenderBridge.h"
#include "colombina/render/MesaRenderer.h"
#include "ui/dialogs/TransformDialog.h"
#include "ui/audio/AudioEffectsDialog.h"
#include "ui/audio/TrackAudioFxDialog.h"
#include "ui/dialogs/TextEditorDialog.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/timeline/TlLog.h"
#include "ui/core/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QCheckBox>
#include <QCursor>
#include <QList>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QSettings>
#include <QKeySequence>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QScrollBar>
#include <QMenu>
#include <QKeyEvent>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPolygon>
#include <QDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QColorDialog>
#include <QIcon>
#include <QPixmap>
#include <QTabWidget>
#include <QGroupBox>
#include <QListWidget>
#include <QFileDialog>
#include <QActionGroup>
#include <QPair>
#include <QVariantAnimation>
#include <QStyle>
#include <QStyleOptionRubberBand>
#include <QRubberBand>
#include <algorithm>
#include <cmath>
#include <utility>

void TimelineWidget::criarMesa() {
    if (!m_project) return;

    emit editStart();

    // Cria a composição 2D
    MesaComposition mc;
    mc.id = newId();
    mc.name = tr("Mesa %1").arg(m_project->mesas.size() + 1);
    mc.canvasW = m_project->width;
    mc.canvasH = m_project->height;

    // Cria uma nova track para a Mesa
    m_project->addTrack(false);
    Track& newTrack = m_project->videoTracks.last();
    newTrack.name = mc.name;
    // Posição padrão no centro da composição (estilo AE: pos da âncora).
    newTrack.mesaX = mc.canvasW / 2.0;
    newTrack.mesaY = mc.canvasH / 2.0;

    // Cria o TrackGroup (pasta) vinculado à Mesa
    TrackGroup grp;
    grp.id = mc.id;
    grp.name = mc.name;
    grp.mesaId = mc.id;
    m_project->trackGroups.append(grp);
    newTrack.groupId = grp.id;

    mc.trackIds.append(newTrack.id);
    // Câmera padrão centrada na composição (mesma convenção das layers).
    mc.camX = mc.canvasW / 2.0;
    mc.camY = mc.canvasH / 2.0;
    m_project->mesas.append(mc);

    emit modified();
    emit mesaOpenRequested(mc.id);
    invalidateScene();
    update();
}

void TimelineWidget::addTrackToMesa(const QString& mesaId) {
    if (!m_project || mesaId.isEmpty()) return;
    MesaComposition* mc = m_project->findMesa(mesaId);
    if (!mc) return;

    emit editStart();

    // Cria nova track de vídeo
    m_project->addTrack(false);
    Track& newTrack = m_project->videoTracks.last();
    newTrack.name = tr("Mesa %1 · Track %2").arg(mc->name).arg(mc->trackIds.size() + 1);

    // Vincula à mesma pasta/grupo da Mesa
    TrackGroup* grp = m_project->findGroup(mesaId);
    if (grp) newTrack.groupId = grp->id;

    // Adiciona à lista de tracks da Mesa
    mc->trackIds.append(newTrack.id);
    // Posição padrão no centro da composição (estilo AE).
    newTrack.mesaX = mc->canvasW / 2.0;
    newTrack.mesaY = mc->canvasH / 2.0;

    emit modified();
    invalidateScene();
    update();
}

void TimelineWidget::sendTrackToMesa(const QString& trackId, const QString& mesaId) {
    if (!m_project || mesaId.isEmpty()) return;
    MesaComposition* mc = m_project->findMesa(mesaId);
    if (!mc) return;

    // Só faixas de vídeo viram camadas de Mesa.
    int trackIdx = -1;
    for (int i = 0; i < m_project->videoTracks.size(); ++i)
        if (m_project->videoTracks[i].id == trackId) { trackIdx = i; break; }
    if (trackIdx < 0) return;

    Track& t = m_project->videoTracks[trackIdx];

    // Já é membro desta Mesa: nada a fazer.
    if (mc->trackIds.contains(t.id)) return;

    emit editStart();

    // Exclusividade: a track sai de qualquer outra Mesa.
    for (MesaComposition& m : m_project->mesas)
        if (m.trackIds.contains(t.id))
            m.trackIds.removeAll(t.id);

    // Entra na composição alvo e no grupo (pasta) da Mesa.
    mc->trackIds.append(t.id);
    TrackGroup* grp = m_project->findGroup(mesaId);
    t.groupId = grp ? grp->id : t.groupId;

    emit modified();
    emit mesaChanged(mesaId);
    invalidateScene();
    updateScrollRanges();
    update();
}

void TimelineWidget::addSolidToMesa(const QString& mesaId, const QString& generator,
                                    const QColor& c1, const QColor& c2) {
    if (!m_project || mesaId.isEmpty()) return;
    MesaComposition* mc = m_project->findMesa(mesaId);
    if (!mc) return;

    emit editStart();

    // Mídia virtual gerada sob demanda (mesmo formato da pool de mídia).
    MediaItem m;
    m.id = newId();
    m.isSolid = true;
    m.hasVideo = true;
    m.width = m_project->width;
    m.height = m_project->height;
    m.duration = m_project->duration() > 0 ? m_project->duration() : 5.0;
    m.generator = generator;
    m.solidColor = c1;
    m.solidColor2 = c2;
    if (generator.isEmpty()) {
        const QString hex = m.solidColor.name();
        m.name = tr("Cor %1").arg(hex.startsWith(QLatin1Char('#')) ? hex.mid(1) : hex);
    } else if (generator == QStringLiteral("gradient")) {
        m.name = tr("Gradiente");
    } else {
        m.name = tr("Camada %1").arg(generator);
    }
    m_project->media.append(m);

    // Nova track de vídeo vinculada ao grupo (pasta) da Mesa.
    m_project->addTrack(false);
    Track& nt = m_project->videoTracks.last();
    nt.name = tr("Mesa %1 · %2").arg(mc->name).arg(m.name);
    TrackGroup* grp = m_project->findGroup(mesaId);
    if (grp) nt.groupId = grp->id;
    mc->trackIds.append(nt.id);
    nt.mesaX = mc->canvasW / 2.0;
    nt.mesaY = mc->canvasH / 2.0;

    // Clipe cobrindo toda a duração da mídia virtual.
    Clip c;
    c.id = newId();
    c.mediaId = m.id;
    c.pos = 0.0;
    c.in = 0.0;
    c.dur = m.duration;
    c.speed = 1.0;
    c.name = m.name;
    nt.clips.append(c);

    emit modified();
    emit mesaChanged(mesaId);
    invalidateScene();
    updateScrollRanges();
    update();
}

void TimelineWidget::addMeshToMesa(const QString& mesaId, const QString& objPath) {
    if (!m_project || mesaId.isEmpty() || objPath.isEmpty()) return;
    MesaComposition* mc = m_project->findMesa(mesaId);
    if (!mc) return;

    emit editStart();

    const QString base = QFileInfo(objPath).completeBaseName();

    // Mídia 3D: vira item na pool + clipe na timeline (tamanho comum, ~5s).
    MediaItem m;
    m.id = newId();
    m.filePath = objPath;
    m.name = base;
    m.isMesh = true;
    m.hasVideo = true;
    m.width = 512;
    m.height = 512;
    // Duração de clipe comum (não a timeline inteira).
    m.duration = 5.0;
    m_project->media.append(m);

    // Faixa da Mesa com a malha, no TOPO do empilhamento.
    //
    // A faixa de índice 0 é a de cima (é o que clipAt e o paintEvent
    // assumem), e addTrack() ANEXA — criaria a malha no rodapé, embaixo de
    // qualquer vídeo. Aí um clipe opícito na faixa de cima cobre a malha
    // inteira e o Preview mostra só ele. Por isso insere na frente.
    //
    // m_selTracks guarda o índice da LINHA (não o id da faixa), então toda
    // linha de vídeo selecionada precisa subir junto — senão a seleção
    // passaria a apontar para a faixa errada. Os demais membros com índice
    // de linha (m_volRow, m_dragTrackRow...) são estado transiente de gesto
    // e estão inativos durante um import.
    for (TrackSel& s : m_selTracks)
        if (!s.audio && s.row >= 0) ++s.row;
    Track freshTrack;
    freshTrack.id = newId();
    freshTrack.audio = false;
    m_project->videoTracks.prepend(freshTrack);
    Track& nt = m_project->videoTracks.first();
    nt.name = tr("Mesa %1 · 3D %2").arg(mc->name.isEmpty() ? tr("Mesa") : mc->name)
                                    .arg(base);
    TrackGroup* grp = m_project->findGroup(mesaId);
    if (grp) nt.groupId = grp->id;
    mc->trackIds.append(nt.id);
    nt.meshPath = objPath;
    nt.mesaX = mc->canvasW / 2.0;
    nt.mesaY = mc->canvasH / 2.0;
    nt.mesaScaleX = 1.0;
    nt.mesaScaleY = 1.0;
    // Liga o modo 3D da composição se ainda estiver off.
    if (!mc->mesa3d) mc->mesa3d = true;

    // A projeção 3D é `dist = camZ - z`, com corte em `dist < 1.0`. Como
    // `camZ` e `mesaZ` nascem em 0, a distância daria 0 e TODOS os vértices
    // seriam descartados — a malha não apareceria nem no Preview nem na Mesa.
    // Por isso a câmera é posicionada ao criar a faixa, e a escala escolhida
    // para a malha normalizada (~200 un) ocupar ~metade da altura da composição.
    if (mc->camZ <= 0.0) {
        const double camZ = mc->canvasH * 1.2;
        mc->camZ = camZ;
        // Mesma fórmula do renderer (focalForHeight). A conta antiga assumia
        // que 25 era MEIO-ângulo e usava tan(25°), enquanto o renderer trata
        // camFov como ângulo cheio (tan(fov/2)) — a escala nascia calibrada
        // contra um focal que ninguém usava.
        const double focal = MesaRenderer::focalForHeight(mc->canvasH, mc->camFov);
        const double sc = (mc->canvasH * 0.5) * camZ / (200.0 * focal);
        nt.mesaScaleX = sc;
        nt.mesaScaleY = sc;
    }

    // Clipe na timeline apontando para a mídia 3D (duração cheia).
    Clip c;
    c.id = newId();
    c.mediaId = m.id;
    c.pos = 0.0;
    c.in = 0.0;
    c.dur = m.duration;
    c.speed = 1.0;
    c.name = tr("3D %1").arg(base);
    nt.clips.append(c);

    emit modified();
    emit mesaChanged(mesaId);
    invalidateScene();
    updateScrollRanges();
    update();
}

QString TimelineWidget::importBlendAsMesh(const QString& mesaId, const QString& blendPath) {
    if (!m_project || mesaId.isEmpty() || blendPath.isEmpty())
        return tr("Nada para importar.");
    // Sem outDir: a ponte grava o .obj derivado ao lado do .blend de origem
    // (com fallback pro AppData). Project não guarda o próprio path, e usar
    // /tmp — como era antes — sumia com o mesh no próximo reboot.
    QString err;
    const QString objPath = mesh::blendToObj(blendPath, &err);
    if (objPath.isEmpty()) {
        qWarning() << "[BLEND]" << err;
        return err;
    }
    addMeshToMesa(mesaId, objPath);
    return QString();
}

void TimelineWidget::duplicateMesaTrack(const QString& mesaId, const QString& trackId) {
    if (!m_project || mesaId.isEmpty()) return;
    MesaComposition* mc = m_project->findMesa(mesaId);
    if (!mc) return;

    int srcIdx = -1;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        if (m_project->videoTracks[i].id == trackId) { srcIdx = i; break; }
    if (srcIdx < 0) return;

    emit editStart();

    // Cópia profunda: ids novos em clips (as mídias são compartilhadas,
    // como todo clipe duplicado do projeto).
    Track copy = m_project->videoTracks[srcIdx];
    copy.id = newId();
    copy.name = tr("%1 (cópia)").arg(copy.name);
    for (int i = 0; i < copy.clips.size(); ++i) {
        copy.clips[i].id = newId();
        copy.clips[i].groupId = QString();  // grupos entra-e-sai não valem para a cópia
    }

    // Deslocada +20/+20 no canvas (base e keyframes de posição), para sair da
    // sombra da original e fácil de pegar.
    copy.mesaX += 20.0;
    copy.mesaY += 20.0;
    for (Keyframe& k : copy.kfMesaX) k.value += 20.0;
    for (Keyframe& k : copy.kfMesaY) k.value += 20.0;

    m_project->videoTracks.append(copy);
    mc->trackIds.append(copy.id);

    emit modified();
    emit mesaChanged(mesaId);
    invalidateScene();
    updateScrollRanges();
    update();
}

