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

namespace {
// Paleta escolhível de marcador: vermelho, azul, amarelo, roxo. O amarelo é
// o default (é o mais próximo do âmbar antigo).
const QColor* markerPalette() {
    static const QColor c[4] = {
        QColor(QStringLiteral("#E53935")), // vermelho
        QColor(QStringLiteral("#1E88E5")), // azul
        QColor(QStringLiteral("#FDD835")), // amarelo (default)
        QColor(QStringLiteral("#8E24AA")), // roxo
    };
    return c;
}
constexpr int kMarkerPaletteN = 4;
const char* const kMarkerColorNames[kMarkerPaletteN] = {
    "Vermelho", "Azul", "Amarelo", "Roxo",
};

// "Amostra" colorida (quadrado arredondado) para o menu.
QIcon swatchIcon(const QColor& c) {
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter pt(&pm);
    pt.setRenderHint(QPainter::Antialiasing);
    pt.setPen(Qt::NoPen);
    pt.setBrush(c);
    pt.drawRoundedRect(QRectF(2, 2, 12, 12), 3, 3);
    pt.setPen(QPen(c.darker(160), 1));
    pt.setBrush(Qt::NoBrush);
    pt.drawRoundedRect(QRectF(2.5, 2.5, 11, 11), 3, 3);
    return QIcon(pm);
}

// Nome automático estável: "Marcador N" com o menor N livre. Renomear um
// marcador para "Marcador 5" manualmente conta como uso do5.
QString autoMarkerName(const QVector<Marker>& ms) {
    QSet<int> used;
    for (const Marker& m : ms) {
        bool ok = false;
        const int n = m.name.startsWith(QLatin1String("Marcador "))
                          ? m.name.mid(9).toInt(&ok)
                          : -1;
        if (ok) used.insert(n);
    }
    int n = 1;
    while (used.contains(n)) ++n;
    return QStringLiteral("Marcador %1").arg(n);
}
} // namespace

void TimelineWidget::toggleMarker(double t, const QColor& color) {
    if (!m_project) return;
    emit editStart();
    const double tt = snapTime(std::max(0.0, t));
    for (int i = 0; i < m_project->markers.size(); ++i) {
        if (std::fabs(m_project->markers[i].time - tt) < 1e-6) {
            m_project->markers.removeAt(i);
            emit modified();
            update();
            return;
        }
    }
    Marker m;
    m.id = newId();
    m.time = tt;
    m.name = autoMarkerName(m_project->markers);
    m.color = color.isValid() ? color : markerPalette()[2]; // amarelo
    m_project->addMarker(m);
    emit modified();
    update();
}

int TimelineWidget::markerAt(int x) const {
    if (!m_project) return -1;
    for (int i = 0; i < m_project->markers.size(); ++i) {
        const int mx = (int)timeToX(m_project->markers[i].time);
        if (std::abs(x - mx) <= 7) return i;
    }
    return -1;
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* e) {
    if (!m_project) return;
    QMenu menu(this);
    Clip* clip = nullptr;
    QString gid;
    if (folderStripAt(e->pos().y(), gid)) {
        TrackGroup* g = m_project->findGroup(gid);
        if (g) {
            QAction* selAll = menu.addAction(tr("Selecionar faixas da pasta"));
            QAction* ren = menu.addAction(tr("Renomear pasta…"));
            QAction* col = menu.addAction(g->collapsed ? tr("Expandir pasta")
                                                       : tr("Recolher pasta"));
            QAction* del = menu.addAction(tr("Desagrupar faixas"));
            QAction* openMesa = nullptr;
            QAction* delMesa = nullptr;
            QAction* mbMesa = nullptr;
            if (!g->mesaId.isEmpty()) {
                openMesa = menu.addAction(tr("Abrir Mesa"));
                mbMesa = menu.addAction(tr("Motion blur (Ctrl+Shift+B)"));
                delMesa = menu.addAction(tr("Excluir Mesa"));
                MesaComposition* m = m_project->findMesa(g->mesaId);
                if (m) mbMesa->setCheckable(true);
                if (m) mbMesa->setChecked(m->motionBlur);
            }
            QAction* act = menu.exec(e->globalPos());
            if (act == selAll) {
                selectGroupTracks(gid);
                refreshView();
            } else if (act == ren) {
                renameTrackGroup(gid);
            } else if (act == col) {
                toggleGroupCollapsed(gid);
            } else if (act == openMesa) {
                emit mesaOpenRequested(g->mesaId);
            } else if (act == mbMesa) {
                MesaComposition* m = m_project->findMesa(g->mesaId);
                if (m) {
                    emit editStart();
                    m->motionBlur = !m->motionBlur;
                    emit modified();
                    emit mesaChanged(g->mesaId);
                    refreshView();
                    qDebug() << "[TIMELINE] mesa motion blur"
                             << (m->motionBlur ? "ON" : "OFF");
                }
            } else if (act == delMesa) {
                emit editStart();
                const QString mesaId = g->mesaId;
                for (int i = (int)m_project->mesas.size() - 1; i >= 0; --i)
                    if (m_project->mesas[i].id == mesaId)
                        m_project->mesas.removeAt(i);
                g->mesaId.clear();
                updateScrollRanges();
                invalidateScene();
                emit modified();
            } else if (act == del) {
                emit editStart();
                for (int i = (int)m_project->trackGroups.size() - 1; i >= 0; --i)
                    if (m_project->trackGroups[i].id == gid)
                        m_project->trackGroups.removeAt(i);
                for (Track& t : m_project->videoTracks)
                    if (t.groupId == gid) t.groupId.clear();
                for (Track& t : m_project->audioTracks)
                    if (t.groupId == gid) t.groupId.clear();
                updateScrollRanges();
                invalidateScene();
                emit modified();
            }
        }
        return;
    }

    int row;
    bool audio;
    if (rowFromY(e->pos().y(), row, audio))
        clip = clipAt(row, audio, xToTime(e->pos().x()));

    if (clip) {
        if (!isSelected(clip->id))
            setSelection(clip->id);
        update();
        QAction* cut = menu.addAction(tr("Dividir no playhead"));
        QAction* del = menu.addAction(tr("Excluir (junta os espaços)"));
        QAction* gapDel = menu.addAction(tr("Excluir (deixa espaço)"));
        QAction* dup = menu.addAction(tr("Duplicar"));
        menu.addSeparator();
        QAction* copy = menu.addAction(tr("Copiar"));
        QAction* ccut = menu.addAction(tr("Recortar"));
        QAction* paste = menu.addAction(tr("Colar"));
        QAction* pasteAttr = menu.addAction(tr("Colar Atributos…"));
        pasteAttr->setEnabled(!m_clipboard.isEmpty());
        menu.addSeparator();
        QAction* savePreset = menu.addAction(tr("Salvar Preset…"));
        QAction* applyPreset = menu.addAction(tr("Aplicar Preset…"));
        menu.addSeparator();
        QAction* props = menu.addAction(tr("Propriedades…"));
        QAction* velAct = nullptr;
        QAction* pivotAct = nullptr;
        if (!audio) {
            velAct = menu.addAction(tr("Editor de velocidade"));
            velAct->setToolTip(tr("Envelope de velocidade (time remapping) no dock Velocidade"));
            pivotAct = menu.addAction(tr("Pivot (3D do clipe)"));
            pivotAct->setToolTip(tr("Gizmo 3D + keyframes de transform no dock Pivot"));
        }
        QAction* unlink = nullptr;
        if (!clip->groupId.isEmpty())
            unlink = menu.addAction(tr("Desvincular grupo"));
        QAction* mcAct = nullptr;
        {
            int videoSel = 0;
            for (const QString& id : m_selected) {
                Clip* cc = findClipById(id);
                if (!cc || cc->isText || cc->hasMulticam()) continue;
                const MediaItem* mi = m_project ? m_project->findMedia(cc->mediaId) : nullptr;
                if (mi && mi->hasVideo) ++videoSel;
            }
            if (videoSel >= 2) {
                mcAct = menu.addAction(tr("Criar multicam (%1 ângulos)…")
                                           .arg(videoSel));
                mcAct->setToolTip(tr("Junta os clipes de vídeo selecionados num "
                                     "clipe multicam; teclas 1..N cortam o ângulo"));
            } else if (clip->hasMulticam()) {
                QAction* info = menu.addAction(
                    tr("Multicam: %1 ângulos (teclas 1..N)").arg(clip->multicamSources.size()));
                info->setEnabled(false);
            }
        }
        QAction* fx = nullptr;
        QAction* grade = nullptr;
        QAction* audioFx = nullptr;
        QAction* transform = nullptr;
        QAction* textAction = nullptr;
        QAction* pantalone = nullptr;
        QAction* maskAction = nullptr;
        if (!audio) {
            fx = menu.addAction(tr("Efeitos de vídeo…"));
            grade = menu.addAction(tr("Correção de cor…"));
            transform = menu.addAction(tr("Transformar…"));
            textAction = menu.addAction(tr("Texto…"));
            pantalone = menu.addAction(tr("Pantalone…"));
            maskAction = menu.addAction(tr("Máscara…"));
        } else {
            audioFx = menu.addAction(tr("Efeitos de áudio…"));
        }
        // Tipo da transição de saída (aplicada quando este clipe se sobrepõe
        // ao próximo da mesma faixa; a duração é o tamanho da sobreposição).
        QMenu* transMenu = nullptr;
        QActionGroup* transGrp = nullptr;
        QAction* transDissolve = nullptr;
        QAction* transWipeL = nullptr;
        QAction* transWipeR = nullptr;
        QAction* transWipeU = nullptr;
        QAction* transWipeD = nullptr;
        QAction* transWipeTL = nullptr;
        QAction* transWipeTR = nullptr;
        QAction* transWipeBL = nullptr;
        QAction* transWipeBR = nullptr;
        if (!audio) {
            menu.addSeparator();
            transMenu = menu.addMenu(tr("Transição de saída"));
            transGrp = new QActionGroup(transMenu);
            const QString cur = isTransition(clip->transitionType)
                                    ? clip->transitionType
                                    : QStringLiteral("dissolve");
            transDissolve = transMenu->addAction(tr("Dissolver"));
            transDissolve->setCheckable(true);
            transDissolve->setChecked(cur == QStringLiteral("dissolve"));
            transGrp->addAction(transDissolve);
            transMenu->addSeparator();
            transWipeL = transMenu->addAction(tr("Wipe ← (próximo vem da direita)"));
            transWipeL->setCheckable(true);
            transWipeL->setChecked(cur == QStringLiteral("wipeleft"));
            transGrp->addAction(transWipeL);
            transWipeR = transMenu->addAction(tr("Wipe → (próximo vem da esquerda)"));
            transWipeR->setCheckable(true);
            transWipeR->setChecked(cur == QStringLiteral("wiperight"));
            transGrp->addAction(transWipeR);
            transWipeU = transMenu->addAction(tr("Wipe ↑ (próximo vem de baixo)"));
            transWipeU->setCheckable(true);
            transWipeU->setChecked(cur == QStringLiteral("wipeup"));
            transGrp->addAction(transWipeU);
            transWipeD = transMenu->addAction(tr("Wipe ↓ (próximo vem de cima)"));
            transWipeD->setCheckable(true);
            transWipeD->setChecked(cur == QStringLiteral("wipedown"));
            transGrp->addAction(transWipeD);
            transMenu->addSeparator();
            QMenu* diagMenu = transMenu->addMenu(tr("Wipes diagonais"));
            transWipeTL = diagMenu->addAction(tr("↖ Wipe (próximo vem de baixo-direita)"));
            transWipeTL->setCheckable(true);
            transWipeTL->setChecked(cur == QStringLiteral("wipetl"));
            transGrp->addAction(transWipeTL);
            transWipeTR = diagMenu->addAction(tr("↗ Wipe (próximo vem de baixo-esquerda)"));
            transWipeTR->setCheckable(true);
            transWipeTR->setChecked(cur == QStringLiteral("wipetr"));
            transGrp->addAction(transWipeTR);
            transWipeBR = diagMenu->addAction(tr("↘ Wipe (próximo vem de cima-esquerda)"));
            transWipeBR->setCheckable(true);
            transWipeBR->setChecked(cur == QStringLiteral("wipebr"));
            transGrp->addAction(transWipeBR);
            transWipeBL = diagMenu->addAction(tr("↙ Wipe (próximo vem de cima-direita)"));
            transWipeBL->setCheckable(true);
            transWipeBL->setChecked(cur == QStringLiteral("wipebl"));
            transGrp->addAction(transWipeBL);
            transMenu->setEnabled([&]() {
                // Só faz sentido se o clipe seguinte da faixa se sobrepõe a ele.
                const Track* tr = trackOf(clip);
                if (!tr || tr->audio) return false;
                const double end = clip->pos + clip->dur;
                for (const Clip& o : tr->clips)
                    if (o.id != clip->id && o.pos < end - 1e-6 && o.pos + o.dur > clip->pos + 1e-6
                        && o.pos >= clip->pos - 1e-6)
                        return true;
                return false;
            }());
        }
        QAction* act = menu.exec(e->globalPos());
        if (act == cut) cutAtPlayhead();
        else if (act == del) deleteSelected();
        else if (act == gapDel) deleteSelectedLeaveGap();
        else if (act == dup) { emit editStart(); duplicateClip(clip); }
        else if (act == copy) copySelected();
        else if (act == ccut) cutSelected();
        else if (act == paste) pasteClips();
        else if (act == pasteAttr) pasteAttributes();
        else if (act == savePreset) saveClipPreset();
        else if (act == applyPreset) applyClipPreset();
        else if (act == props) emit propertiesRequested(clip->id);
        else if (act == velAct) emit velocityRequested(clip->id);
        else if (act == pivotAct) emit pivotRequested(clip->id);
        else if (act == unlink) {
            emit editStart();
            for (Clip* m : groupMembers(clip->groupId))
                m->groupId.clear();
            update();
            emit modified();
        }
        else if (act == mcAct) createMulticamFromSelection();
        else if (act == fx) showEffectsDialog(clip);
        else if (act == grade) showGradingDialog(clip);
        else if (act == audioFx) showAudioEffectsDialog(clip);
        else if (act == transform) showTransformDialog(clip);
        else if (act == textAction) showTextEditorDialog(clip);
        else if (act == pantalone) emit pantaloneRequested(clip->id);
        else if (act == maskAction) emit maskRequested(clip->id);
        else if (act == transDissolve || act == transWipeL || act == transWipeR
                 || act == transWipeU || act == transWipeD
                 || act == transWipeTL || act == transWipeTR
                 || act == transWipeBL || act == transWipeBR) {
            emit editStart();
            if (act == transDissolve) clip->transitionType = QStringLiteral("dissolve");
            else if (act == transWipeL) clip->transitionType = QStringLiteral("wipeleft");
            else if (act == transWipeR) clip->transitionType = QStringLiteral("wiperight");
            else if (act == transWipeU) clip->transitionType = QStringLiteral("wipeup");
            else if (act == transWipeD) clip->transitionType = QStringLiteral("wipedown");
            else if (act == transWipeTL) clip->transitionType = QStringLiteral("wipetl");
            else if (act == transWipeTR) clip->transitionType = QStringLiteral("wipetr");
            else if (act == transWipeBL) clip->transitionType = QStringLiteral("wipebl");
            else if (act == transWipeBR) clip->transitionType = QStringLiteral("wipebr");
            invalidateScene();
            emit modified();
        }
    } else if (e->pos().y() < kRulerH) {
        // Clique-direito sobre um marcador existente: menu dedicado —
        // renomear, trocar a cor (paleta de4) ou excluir.
        const int mi = markerAt(e->pos().x());
        if (mi >= 0) {
            Marker& mk = m_project->markers[mi];
            QMenu mmenu;
            QAction* ren = mmenu.addAction(tr("Renomear…"));
            mmenu.addSeparator();
            QAction* cols[kMarkerPaletteN];
            for (int i = 0; i < kMarkerPaletteN; ++i) {
                cols[i] = mmenu.addAction(swatchIcon(markerPalette()[i]),
                                          tr(kMarkerColorNames[i]));
                cols[i]->setCheckable(true);
                cols[i]->setChecked(mk.color == markerPalette()[i]);
            }
            mmenu.addSeparator();
            QAction* del = mmenu.addAction(tr("Excluir marcador"));
            QAction* mact = mmenu.exec(e->globalPos());
            if (mact == del) {
                emit editStart();
                m_project->markers.removeAt(mi);
                emit modified();
                update();
            } else if (mact == ren) {
                bool ok = false;
                const QString name = QInputDialog::getText(
                    this, tr("Marcador"), tr("Nome do marcador:"),
                    QLineEdit::Normal, mk.name, &ok);
                if (ok && !name.isEmpty()) {
                    emit editStart();
                    mk.name = name;
                    emit modified();
                    update();
                }
            } else {
                for (int i = 0; i < kMarkerPaletteN; ++i) {
                    if (mact != cols[i]) continue;
                    emit editStart();
                    mk.color = markerPalette()[i];
                    emit modified();
                    update();
                    break;
                }
            }
            return;
        }
        const double t = std::max(0.0, snapTime(xToTime(e->pos().x())));
        QMenu* addMenu = menu.addMenu(tr("Adicionar marcador"));
        QAction* addCols[kMarkerPaletteN];
        for (int i = 0; i < kMarkerPaletteN; ++i)
            addCols[i] = addMenu->addAction(swatchIcon(markerPalette()[i]),
                                            tr(kMarkerColorNames[i]));
        QAction* addMN = menu.addAction(tr("Adicionar marcador nomeado…"));
        QAction* clearM = menu.addAction(tr("Limpar todos os marcadores"));
        QAction* delLoopRip = nullptr;
        QAction* delLoopGap = nullptr;
        if (m_loopOut > m_loopIn) {
            menu.addSeparator();
            delLoopRip = menu.addAction(tr("Excluir tempo da região de loop (ripple)"));
            delLoopGap = menu.addAction(tr("Excluir tempo da região de loop (deixa espaço)"));
        }
        QAction* act = menu.exec(e->globalPos());
        if (act == delLoopRip) deleteLoopRipple();
        else if (act == delLoopGap) deleteLoopLeaveGap();
        else {
            bool isAdd = false;
            for (int i = 0; i < kMarkerPaletteN; ++i) {
                if (act != addCols[i]) continue;
                toggleMarker(t, markerPalette()[i]);
                isAdd = true;
                break;
            }
            if (!isAdd && act == addMN) {
                bool ok = false;
                const QString name =
                    QInputDialog::getText(this, tr("Marcador"), tr("Nome do marcador:"),
                                          QLineEdit::Normal, QString(), &ok);
                if (ok && !name.isEmpty()) {
                    emit editStart();
                    Marker m;
                    m.id = newId();
                    m.time = t;
                    m.name = name;
                    m_project->addMarker(m);
                    emit modified();
                    update();
                }
            } else if (!isAdd && act == clearM) {
                emit editStart();
                m_project->markers.clear();
                emit modified();
                update();
            }
        }
    } else {
        QAction* addV = menu.addAction(tr("Adicionar faixa de vídeo"));
        QAction* addA = menu.addAction(tr("Adicionar faixa de áudio"));
        QAction* addR = menu.addAction(tr("Adicionar faixa de gravação"));
        QAction* addFx = menu.addAction(tr("Adicionar faixa de efeitos"));
        QAction* newText = menu.addAction(tr("Novo texto…"));
        QAction* newMesa = menu.addAction(tr("Nova Mesa…"));
        QAction* paste = nullptr;
        if (!m_clipboard.isEmpty()) paste = menu.addAction(tr("Colar"));
        QAction* act = nullptr;
        Track* track = nullptr;
        QMenu* blendMenu = nullptr;
        int vrow = -1;
        if (rowFromY(e->pos().y(), vrow, audio)) {
            if (!audio && vrow >= 0 && vrow < m_project->videoTracks.size())
                track = &m_project->videoTracks[vrow];
            else if (audio && vrow >= 0 && vrow < m_project->audioTracks.size())
                track = &m_project->audioTracks[vrow];
        }
        // Faixas de gravação (seção vermelha no rodapé) não entram no
        // rowFromY: detecta aqui para oferecer a exclusão no menu.
        int recRow = -1;
        if (!track) recRowFromY(e->pos().y(), recRow);
        QAction* delRec = nullptr;
        if (recRow >= 0 && recRow < (int)m_project->recordingTracks.size()) {
            menu.addSeparator();
            delRec = menu.addAction(tr("Excluir faixa de gravação"));
        }
        if (track && !track->audio) {
            menu.addSeparator();
            blendMenu = menu.addMenu(tr("Modo de composição"));
            const QStringList modes = {"normal", "screen", "multiply", "overlay",
                                       "darken", "lighten", "softlight", "hardlight",
                                       "difference", "addition", "subtract", "exclusion"};
            QActionGroup* grp = new QActionGroup(blendMenu);
            for (const QString& m : modes) {
                QAction* a = blendMenu->addAction(m);
                a->setCheckable(true);
                a->setChecked(track->blendMode == m);
                grp->addAction(a);
            }
        }
        // Clique com o direito na faixa: garante que ela esteja selecionada
        // (sem desfazer a seleção múltipla quando o clique já está nela).
        if (track) {
            selectTrackRightClick(vrow, audio);
            refreshView();
        }
        QAction* trackVol = nullptr;
        if (track) trackVol = menu.addAction(tr("Volume da faixa: %1%").arg((int)llround(track->volume * 100.0)));
        QAction* trackOp = nullptr;
        if (track && !track->audio)
            trackOp = menu.addAction(tr("Opacidade da faixa: %1%")
                                         .arg((int)llround(track->opacity * 100.0)));
        QAction* groupTracks = nullptr;
        QAction* renameGroup = nullptr;
        QAction* ungroupTracks = nullptr;
        QAction* createMesa = nullptr;
        QMenu* sendMesaMenu = nullptr;
        if (track) {
            menu.addSeparator();
            if (m_selTracks.size() >= 2)
                groupTracks = menu.addAction(tr("Agrupar faixas em pasta…"));
            if (!track->groupId.isEmpty()) {
                renameGroup = menu.addAction(tr("Renomear pasta…"));
                ungroupTracks = menu.addAction(tr("Desagrupar faixas"));
            }
            createMesa = menu.addAction(tr("Criar Mesa"));
            if (!track->audio && !m_project->mesas.isEmpty()) {
                sendMesaMenu = menu.addMenu(tr("Enviar para Mesa"));
                for (const MesaComposition& mesa : m_project->mesas) {
                    QAction* a = sendMesaMenu->addAction(
                        mesa.name.isEmpty() ? mesa.id : mesa.name);
                    a->setData(mesa.id);
                }
            }
        }
        QAction* delTrack = nullptr;
        if (track) {
            menu.addSeparator();
            delTrack = menu.addAction(tr("Excluir faixa"));
        }
        act = menu.exec(e->globalPos());
        if (act == addV) addTrack(false);
        else if (act == addA) addTrack(true);
        else if (act == addR) addRecordingTrack();
        else if (act == addFx) addFxTrack();
        else if (act == delRec) {
            emit editStart();
            m_project->removeRecordingTrack(recRow);
            m_selected.clear();
            m_secondarySelected.clear();
            clearTrackSelection();
            invalidateScene();
            updateScrollRanges();
            emit modified();
        }
        else if (act == newText) {
            const double tt = std::max(0.0, snapTime(xToTime(e->pos().x())));
            addTextClipAt(vrow, tt);
        }
        else if (act == newMesa) {
            criarMesa();
        }
        else if (act == paste) pasteClips();
        else if (act == trackVol) {
            bool ok = false;
            const double v = QInputDialog::getDouble(
                this, tr("Volume da faixa"), tr("Volume (0–200%):"),
                track->volume * 100.0, 0.0, 200.0, 0, &ok);
            if (ok) {
                emit editStart();
                track->volume = v / 100.0;
                emit modified();
                update();
            }
        }
        else if (act == trackOp) {
            bool ok = false;
            const double v = QInputDialog::getDouble(
                this, tr("Opacidade da faixa"), tr("Opacidade (0–100%):"),
                track->opacity * 100.0, 0.0, 100.0, 0, &ok);
            if (ok) {
                emit editStart();
                track->opacity = v / 100.0;
                emit modified();
                update();
            }
        }
        else if (act == delTrack) {
            emit editStart();
            m_project->removeTrack(audio, vrow);
            pruneEmptyGroups();
            m_selected.clear();
            m_secondarySelected.clear();
            clearTrackSelection();
            invalidateScene();
            updateScrollRanges();
            emit modified();
        }
        else if (act == groupTracks) {
            createTrackGroup();
        }
        else if (act == renameGroup) {
            renameTrackGroup(track->groupId);
        }
        else if (act == ungroupTracks) {
            emit editStart();
            const QString gid = track->groupId;
            for (int i = (int)m_project->trackGroups.size() - 1; i >= 0; --i)
                if (m_project->trackGroups[i].id == gid)
                    m_project->trackGroups.removeAt(i);
            for (Track& t : m_project->videoTracks)
                if (t.groupId == gid) t.groupId.clear();
            for (Track& t : m_project->audioTracks)
                if (t.groupId == gid) t.groupId.clear();
            updateScrollRanges();
            invalidateScene();
            emit modified();
        }
        else if (track && act == createMesa) {
            criarMesa();
        }
        else if (track && act && sendMesaMenu && act->parent() == sendMesaMenu) {
            sendTrackToMesa(track->id, act->data().toString());
        }
        else if (track && act && blendMenu && act->parent() == blendMenu) {
            emit editStart();
            track->blendMode = act->text();
            emit modified();
            update();
        }
    }
}

