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

void TimelineWidget::copySelected() {
    if (m_selected.isEmpty() || !m_project) return;
    m_clipboard.clear();
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* c = findClipById(id);
        if (!c || trackLocked(c)) continue;
        ClipboardEntry e;
        e.clip = *c;
        e.audio = false;
        e.track = 0;
        bool found = false;
        for (int i = 0; i < m_project->videoTracks.size() && !found; ++i)
            for (const Clip& x : m_project->videoTracks[i].clips)
                if (&x == c) { e.track = i; e.audio = false; found = true; break; }
        for (int i = 0; i < m_project->audioTracks.size() && !found; ++i)
            for (const Clip& x : m_project->audioTracks[i].clips)
                if (&x == c) { e.track = i; e.audio = true; found = true; break; }
        if (found) m_clipboard.append(e);
    }
}

void TimelineWidget::cutSelected() {
    if (m_selected.isEmpty()) return;
    emit undoLabel(tr("Recortar"));
    copySelected();
    // Recortar não deve juntar o vão por conta própria: só com o ímã ligado.
    deleteSelection();
}

// Faixa livre para o intervalo [pos, pos+dur): prefere `prefer` (a faixa de
// origem), depois a mais próxima acima/abaixo, e por fim cria uma faixa nova.
// É o coração do "colar não empurra no tempo": em vez de deslizar o clipe até
// não colidir, ele escolhe outra faixa — mantendo o tempo da agulha.
int TimelineWidget::findFreeTrackFor(bool audio, double pos, double dur, int prefer) {
    if (!m_project) return -1;
    auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
    auto overlap = [pos, dur](const QVector<Clip>& clips) {
        for (const Clip& o : clips)
            if (o.pos < pos + dur - 1e-9 && o.pos + o.dur > pos + 1e-9) return true;
        return false;
    };
    auto isFree = [&](int i) {
        return i >= 0 && i < (int)list.size() && !list[i].locked && !overlap(list[i].clips);
    };
    if (isFree(prefer)) return prefer;
    // Mais próxima da faixa de origem (testa para baixo e para cima).
    for (int d = 1; d <= (int)list.size(); ++d) {
        if (isFree(prefer + d)) return prefer + d;
        if (isFree(prefer - d)) return prefer - d;
    }
    // `prefer` fora do range: varre tudo antes de criar.
    for (int i = 0; i < (int)list.size(); ++i)
        if (isFree(i)) return i;
    m_project->addTrack(audio);
    return (int)list.size() - 1;
}

void TimelineWidget::pasteClips() {
    if (m_clipboard.isEmpty() || !m_project) return;
    emit undoLabel(tr("Colar"));
    emit editStart();
    double minPos = 1e18;
    for (const ClipboardEntry& e : m_clipboard) minPos = std::min(minPos, e.clip.pos);
    const double base = snapTime(std::max(0.0, m_playhead));

    QHash<QString, QString> groupMap; // groupId original -> novo
    QStringList pasted;

    // Insere mantendo a lista ordenada por pos (o clipAt/hit-test depende disso).
    const auto insertSorted = [](QVector<Clip>& clips, const Clip& c) {
        auto it = clips.begin();
        while (it != clips.end() && it->pos <= c.pos) ++it;
        clips.insert(it, c);
    };

    for (const ClipboardEntry& e : m_clipboard) {
        Clip nc = e.clip;
        nc.id = newId();
        if (!e.clip.groupId.isEmpty()) {
            if (!groupMap.contains(e.clip.groupId))
                groupMap.insert(e.clip.groupId, newId());
            nc.groupId = groupMap.value(e.clip.groupId);
        } else {
            nc.groupId.clear();
        }
        // Posição no tempo: preserva o offset relativo entre os clipes copiados.
        nc.pos = std::max(0.0, base + (e.clip.pos - minPos));
        // Onde colocar: faixa de origem se livre, senão a livre mais próxima,
        // senão uma faixa nova. Nunca desloca no tempo.
        const int row = findFreeTrackFor(e.audio, nc.pos, nc.dur, e.track);
        auto& list = e.audio ? m_project->audioTracks : m_project->videoTracks;
        if (row < 0 || row >= (int)list.size()) continue;
        insertSorted(list[row].clips, nc);
        pasted.append(nc.id);
    }
    if (pasted.isEmpty()) return;
    clearTrackSelection();
    m_selected = pasted;
    invalidateScene();
    updateScrollRanges();
    update();
    emit modified();
    emit selectionChanged(m_selected.last());
}

void TimelineWidget::saveClipPreset() {
    if (m_selected.isEmpty() || !m_project) return;
    // Captura os atributos a partir do primeiro clipe selecionado.
    Clip* src = findClipById(expandToGroups(m_selected).first());
    if (!src || trackLocked(src)) return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Salvar Preset"),
                                               tr("Nome do preset:"), QLineEdit::Normal,
                                               QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    const QJsonDocument doc(clipattrs::toJson(*src));
    QSettings s;
    s.beginGroup(QStringLiteral("clipPresets"));
    s.setValue(name.trimmed(), QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    s.endGroup();
}

void TimelineWidget::applyClipPreset() {
    if (m_selected.isEmpty() || !m_project) return;

    QSettings s;
    s.beginGroup(QStringLiteral("clipPresets"));
    const QStringList names = s.childKeys();
    s.endGroup();
    if (names.isEmpty()) {
        QMessageBox::information(this, tr("Aplicar Preset"),
                                 tr("Nenhum preset salvo ainda. Use\n"
                                    "“Salvar Preset…” em um clipe para criar um."));
        return;
    }

    bool ok = false;
    const QString sel = QInputDialog::getItem(this, tr("Aplicar Preset"),
                                              tr("Preset:"), names, 0, false, &ok);
    if (!ok || sel.isEmpty()) return;

    QSettings s2;
    s2.beginGroup(QStringLiteral("clipPresets"));
    const QJsonObject obj = QJsonDocument::fromJson(
        s2.value(sel).toString().toUtf8()).object();
    s2.endGroup();
    if (obj.isEmpty()) return;

    emit editStart();
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* dst = findClipById(id);
        if (!dst || trackLocked(dst)) continue;
        clipattrs::applyJson(*dst, obj);
    }
    invalidateScene();
    update();
    emit modified();
}

void TimelineWidget::pasteAttributes() {
    if (m_clipboard.isEmpty() || m_selected.isEmpty() || !m_project) return;
    emit undoLabel(tr("Colar atributos"));
    const Clip& src = m_clipboard.first().clip;
    // Diálogo de seleção de atributos (estilo Vegas: Colar Propriedades).
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Colar Atributos"));
    auto* lay = new QVBoxLayout(&dlg);

    auto* chkVideoFx = new QCheckBox(tr("Efeitos de vídeo (brilho, contraste, saturação, blur, grayscale, chroma key)"));
    auto* chkOfxFx = new QCheckBox(tr("Efeitos OFX (plugins de terceiros)"));
    auto* chkTransform = new QCheckBox(tr("Transformar (posição, escala, rotação)"));
    auto* chkCrop = new QCheckBox(tr("Pan/Crop"));
    auto* chkAudioFx = new QCheckBox(tr("Efeitos de áudio (EQ, denoise, normalizar, fase)"));
    auto* chkProperties = new QCheckBox(tr("Propriedades (volume, opacidade, velocidade, fades)"));
    auto* chkKeyframes = new QCheckBox(tr("Keyframes"));
    chkVideoFx->setChecked(true);
    chkOfxFx->setChecked(true);
    chkTransform->setChecked(true);
    chkCrop->setChecked(true);
    chkAudioFx->setChecked(true);
    chkProperties->setChecked(true);
    chkKeyframes->setChecked(true);
    lay->addWidget(chkVideoFx);
    lay->addWidget(chkOfxFx);
    lay->addWidget(chkTransform);
    lay->addWidget(chkCrop);
    lay->addWidget(chkAudioFx);
    lay->addWidget(chkProperties);
    lay->addWidget(chkKeyframes);
    auto* btnAll = new QCheckBox(tr("Selecionar tudo"));
    btnAll->setChecked(true);
    lay->addWidget(btnAll);
    connect(btnAll, &QCheckBox::toggled, [&](bool on) {
        chkVideoFx->setChecked(on); chkOfxFx->setChecked(on);
        chkTransform->setChecked(on); chkCrop->setChecked(on);
        chkAudioFx->setChecked(on); chkProperties->setChecked(on);
        chkKeyframes->setChecked(on);
    });
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(btns);
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    emit editStart();

    // Copia atributos para cada clipe selecionado.
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* dst = findClipById(id);
        if (!dst || trackLocked(dst)) continue;

        if (chkVideoFx->isChecked()) {
            dst->brightness = src.brightness;
            dst->contrast = src.contrast;
            dst->saturation = src.saturation;
            dst->blur = src.blur;
            dst->grayscale = src.grayscale;
            dst->chromaKey = src.chromaKey;
            dst->chromaKeyColor = src.chromaKeyColor;
            dst->chromaKeySimilarity = src.chromaKeySimilarity;
            dst->chromaKeySoftness = src.chromaKeySoftness;
            dst->chromaKeySpillSuppress = src.chromaKeySpillSuppress;
            dst->psxEnabled = src.psxEnabled;
            dst->psxDither = src.psxDither;
            dst->psxBits = src.psxBits;
            dst->filmGrain = src.filmGrain;
            dst->filmGrainAmount = src.filmGrainAmount;
            dst->filmGrainSize = src.filmGrainSize;
        }
        if (chkOfxFx->isChecked()) {
            dst->ofxFx = src.ofxFx;
        }
        if (chkTransform->isChecked()) {
            dst->tx = src.tx; dst->ty = src.ty;
            dst->scale = src.scale;
            dst->scaleX = src.scaleX; dst->scaleY = src.scaleY;
            dst->rotation = src.rotation;
            dst->anchorX = src.anchorX; dst->anchorY = src.anchorY;
        }
        if (chkCrop->isChecked()) {
            dst->cropL = src.cropL; dst->cropR = src.cropR;
            dst->cropT = src.cropT; dst->cropB = src.cropB;
        }
        if (chkAudioFx->isChecked()) {
            dst->eqLow = src.eqLow; dst->eqMid = src.eqMid; dst->eqHigh = src.eqHigh;
            dst->denoise = src.denoise; dst->denoiseAmount = src.denoiseAmount;
            dst->normalize = src.normalize;
            dst->invertPhase = src.invertPhase;
        }
        if (chkProperties->isChecked()) {
            dst->volume = src.volume;
            dst->opacity = src.opacity;
            dst->speed = src.speed;
            dst->kfSpeed = src.kfSpeed;
            dst->fadeIn = src.fadeIn; dst->fadeOut = src.fadeOut;
        }
        if (chkKeyframes->isChecked()) {
            dst->kfOpacity = src.kfOpacity;
            dst->kfVolume = src.kfVolume;
            dst->kfTx = src.kfTx; dst->kfTy = src.kfTy;
            dst->kfScale = src.kfScale; dst->kfRotation = src.kfRotation;
            dst->kfScaleX = src.kfScaleX; dst->kfScaleY = src.kfScaleY;
            dst->kfAnchorX = src.kfAnchorX; dst->kfAnchorY = src.kfAnchorY;
            dst->kfCropL = src.kfCropL; dst->kfCropR = src.kfCropR;
            dst->kfCropT = src.kfCropT; dst->kfCropB = src.kfCropB;
        }
    }
    invalidateScene();
    update();
    emit modified();
}

void TimelineWidget::duplicateSelected() {
    if (m_selected.isEmpty() || !m_project) return;
    QStringList reps;
    QSet<QString> seen;
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* c = findClipById(id);
        if (!c || trackLocked(c)) continue;
        const QString key = c->groupId.isEmpty() ? c->id : c->groupId;
        if (!seen.contains(key)) { seen.insert(key); reps.append(c->id); }
    }
    if (reps.isEmpty()) return;
    emit undoLabel(tr("Duplicar clipe"));
    emit editStart();
    for (const QString& id : reps) {
        Clip* c = findClipById(id);
        if (c) duplicateClip(c);
    }
    updateScrollRanges();
    update();
    emit modified();
}

void TimelineWidget::nudgeSelected(int dir) {
    if (m_selected.isEmpty() || !m_project) return;
    emit editStart();
    const double frames = (QApplication::keyboardModifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
    const double d = dir * frames / m_project->fps;
    for (const QString& id : expandToGroups(m_selected)) {
        Clip* c = findClipById(id);
        if (!c || trackLocked(c)) continue;
        c->pos = snapTime(std::max(0.0, c->pos + d));
    }
    updateScrollRanges();
    update();
    emit modified();
}

// selectAllClips — implementado em TimelineSelection.cpp

// Vegas: "U" separa os clipes selecionados do seu grupo. Quebra o grupo
// INTEIRO (todos os membros ficam sem groupId), então dá para excluir só um
// dos clipes que estavam unidos (vídeo sem arrastar o áudio, e vice-versa).
void TimelineWidget::ungroupSelected() {
    if (m_selected.isEmpty() || !m_project) return;
    QSet<QString> gids;
    for (const QString& id : m_selected) {
        Clip* c = findClipById(id);
        if (c && !c->groupId.isEmpty()) gids.insert(c->groupId);
    }
    if (gids.isEmpty()) return;
    emit undoLabel(tr("Desagrupar"));
    emit editStart();
    for (const QString& gid : gids)
        for (Clip* m : groupMembers(gid))
            m->groupId.clear();
    updateScrollRanges();
    update();
    emit modified();
}

// Vegas: "G" agrupa os clipes selecionados num grupo único (movem/apagam
// juntos). Se todos já compartilham o mesmo grupo, não faz nada.
void TimelineWidget::groupSelected() {
    if (m_selected.size() < 2 || !m_project) return;
    QString gid;
    bool allSame = true;
    for (const QString& id : m_selected) {
        Clip* c = findClipById(id);
        if (!c) { allSame = false; break; }
        if (gid.isEmpty()) gid = c->groupId;
        else if (gid != c->groupId) allSame = false;
    }
    if (allSame && !gid.isEmpty()) return; // já agrupados
    emit undoLabel(tr("Agrupar"));
    emit editStart();
    const QString ng = newId();
    for (const QString& id : m_selected) {
        Clip* c = findClipById(id);
        if (c) c->groupId = ng;
    }
    updateScrollRanges();
    update();
    emit modified();
}

