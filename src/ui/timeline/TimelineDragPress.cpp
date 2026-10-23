// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/timeline/TimelineWidget.h"
#include "colombina/models/Project.h"
#include "laartman/Decoder.h"
#include "ui/dialogs/SettingsDialog.h"
#include "ui/monitor/TrimmerDialog.h"
#include "colombina/util.h"

#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QApplication>
#include <QInputDialog>
#include <QDialog>
#include <QScrollBar>
#include <QTimer>
#include <QLineEdit>
#include <QMenu>
#include <QAction>
#include <QSet>
#include <QToolTip>
#include <algorithm>
#include <cmath>

namespace {


double mediaInsertDur(const MediaItem& m) {
    if (isImageFile(m.filePath) && !Decoder::isAnimatedImage(m.filePath))
        return 3.0;
    return m.duration > 0 ? m.duration : 1.0;
}
} // namespace

// ── Mouse events ────────────────────────────────────────────────────────

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
    if (!m_project) return;
    const int x = e->pos().x();
    const int y = e->pos().y();
    setFocus();

    if (e->button() != Qt::LeftButton) return;

    m_volPending = false;
    m_envPending = false;
    m_ctrlPending = false;
    m_dragTrackRec = false;
    m_ctrlClipId.clear();
    m_ctrlCanMove = false;
    m_geometryChangedDuringDrag = false; // gesto novo: zera o rastro de geometria

    if (y < kRulerH) {
        // Marca o ponteiro branco no ponto clicado da régua.
        m_cursorT = std::max(0.0, xToTime(x));
        update();
        const int zx0 = 6;
        if (x >= zx0 && x < zx0 + kZoomW) {
            const bool zoomIn = x >= zx0 + kZoomW / 2;
            zoomBy(zoomIn ? 1.6 : 1.0 / 1.6, m_playhead);
            return;
        }
        if (e->modifiers() & Qt::ControlModifier) {
            toggleMarker(xToTime(x));
            return;
        }
        // Borda da região de loop: segurar perto da "lapelinha" permite ajustar
        // só aquela borda sem criar uma região nova (como no Vegas/Premiere).
        if (m_loopOut > m_loopIn) {
            const double inPx = timeToX(m_loopIn);
            const double outPx = timeToX(m_loopOut);
            if (std::fabs(x - outPx) <= 7) {
                m_dragMode = RulerLoopEdge;
                m_loopEdge = LoopEdgeOut;
                m_loopEdgeOther = m_loopIn;
                m_dragStart = e->pos();
                setCursor(Qt::SizeHorCursor);
                update();
                return;
            }
            if (std::fabs(x - inPx) <= 7) {
                m_dragMode = RulerLoopEdge;
                m_loopEdge = LoopEdgeIn;
                m_loopEdgeOther = m_loopOut;
                m_dragStart = e->pos();
                setCursor(Qt::SizeHorCursor);
                update();
                return;
            }
        }
        // Perto da agulha: arrasta o playhead. Fora dela: clique define o
        // playhead e arrasto define a região de loop.
        const double px = timeToX(m_playhead);
        if (std::fabs(x - px) <= 6) {
            m_dragMode = PlayheadDrag;
            const double t2 = std::max(0.0, snapTime(xToTime(x)));
            setPlayhead(t2);
            emit playheadChanged(t2);
            m_dragStart = e->pos();
            update();
            return;
        }
        // Arrastar na régua define a região de loop; clique define o playhead.
        m_dragMode = RulerLoop;
        m_loopPressT = std::max(0.0, snapTime(xToTime(x)));
        m_dragStart = e->pos();
        setPlayhead(m_loopPressT);
        emit playheadChanged(m_loopPressT);
        return;
    }

    // Alça de redimensionamento da faixa (rodapé do cabeçalho).
    {
        int rrow;
        bool raudio;
        if (resizeHandleAt(e->pos(), rrow, raudio) >= 0) {
            m_dragMode = ResizeTrack;
            m_resizeRow = rrow;
            m_resizeAudio = raudio;
            m_resizeOrigH = trackH(rrow, raudio);
            m_resizeSelOrigH.clear();
            for (const TrackSel& s : m_selTracks) {
                if (s.row < 0) continue;
                m_resizeSelOrigH.append(trackH(s.row, s.audio));
            }
            m_dragStart = e->pos();
            setCursor(Qt::SizeVerCursor);
            return;
        }
    }

    // Linha de volume individual do clipe de áudio: arrastar ajusta só ele.
    if (m_showVolLines) {
        int cvrow;
        Clip* vclip = clipVolAt(e->pos(), cvrow);
        if (vclip) {
            emit editStart();
            m_dragMode = ClipVol;
            m_volClip = vclip->id;
            m_volClipOrig = vclip->volume;
            m_volRowOrig = cvrow;
            m_dragStart = e->pos();
            setCursor(Qt::SizeVerCursor);
            update();
            return;
        }
    }

    // Régua de volume das faixas de áudio (a linha é sempre visível):
    // - clicar na linha alterna um ponto do envelope (vira ajuste de volume
    //   base se o arraste for predominantemente vertical);
    // - segurar num diamante arrasta o valor do keyframe;
    // - segurar fora da linha (sem clipe) também pode virar ajuste de volume
    //   (só com a tecla V, para não sequestrar o clique normal na faixa).
    int vrow;
    if (volRowAt(e->pos(), vrow) >= 0) {
            int r2;
            bool a2;
            const bool overClip = rowFromY(y, r2, a2)
                && clipAt(r2, a2, xToTime(x)) != nullptr;
            if (!overClip) {
                // Diamante de keyframe sob o cursor: arrasta o valor.
                int erow;
                bool eaudio;
                const int kfi = trackEnvKfAt(e->pos(), erow, eaudio);
                if (kfi >= 0) {
                    emit editStart();
                    m_dragMode = TrackEnvVol;
                    m_envRow = erow;
                    m_envAudio = eaudio;
                    m_envKf = kfi;
                    m_envOrigY = e->pos().y();
                    m_dragStart = e->pos();
                    setCursor(Qt::SizeVerCursor);
                    update();
                    return;
                }
                const Track& tr = m_project->audioTracks[vrow];
                const bool onLine = std::abs(y - trackVolLineYAt(vrow,
                    kfValue(tr.kfVolume, tr.volume, xToTime(x)))) <= 6;
                if (m_showVolLines && onLine && (m_tool == ToolSelect || m_tool == ToolMove
                               || m_tool == ToolEnvelope)) {
                    m_envPending = true;
                    m_volRow = vrow;
                } else if (m_showVolLines && !onLine
                           && (m_tool == ToolSelect || m_tool == ToolMove)) {
                    // Segurou na faixa (fora da linha e sem clipe): marca como
                    // "pode virar volume". Se o arraste for predominantemente
                    // vertical vira ajuste de volume; senão segue o fluxo normal
                    // (playhead/marquee).
                    m_volPending = true;
                    m_volRow = vrow;
                    m_volOrig = m_project->audioTracks[vrow].volume;
                }
            }
    }

    // Botões de cabeçalho (ordem do Premiere, ver headerBtnAt).
    {
        int brow;
        bool baudio;
        const int b = headerBtnAt(e->pos(), brow, baudio);
        if (b >= 0) {
            Track* tr = baudio ? &m_project->audioTracks[brow]
                               : &m_project->videoTracks[brow];
            emit editStart();
            if (b == 0) {                              // recolher/expandir
                if (tr->fxTrack) {
                    // Faixa de efeitos: a seta expande o painel de efeitos
                    // (lista do clipe de ajuste) em vez de recolher a faixa.
                    setFxExpanded(tr->id, !isFxExpanded(tr->id));
                    updateScrollRanges();
                    refreshView();
                } else {
                    tr->collapsed = !tr->collapsed;
                    updateScrollRanges();
                }
            }
            else if (baudio && (b == 1 || b == 3)) {       // keyframe anterior/próximo
                const QVector<Keyframe>& kfs = tr->kfVolume;
                int best = -1;
                for (int i = 0; i < kfs.size(); ++i) {
                    const double t = kfs[i].time;
                    if (b == 1 && t < m_playhead - 1e-3
                        && (best < 0 || t > kfs[best].time)) best = i;
                    if (b == 3 && t > m_playhead + 1e-3
                        && (best < 0 || t < kfs[best].time)) best = i;
                }
                if (best >= 0) {
                    setPlayhead(kfs[best].time);
                    update();
                }
            }
            else if (baudio && b == 2) {         // adicionar/remover keyframe
                const QVector<Keyframe>& kfs = tr->kfVolume;
                int kfi = -1;
                for (int i = 0; i < kfs.size(); ++i)
                    if (std::fabs(kfs[i].time - m_playhead) < 1e-3) { kfi = i; break; }
                if (kfi >= 0) tr->kfVolume.remove(kfi);
                else upsertKeyframe(tr->kfVolume, m_playhead,
                                    kfValue(tr->kfVolume, tr->volume, m_playhead));
            }
            else if (b == 5) tr->visible = !tr->visible;   // olho/falante
            else if (b == 6) tr->muted = !tr->muted;       // M (áudio)
            else if (b == 7) tr->solo = !tr->solo;         // S (áudio)
            else if (b == 9) tr->locked = !tr->locked;     // cadeado
            // 4=sync lock e 8=record arm: sem backend no modelo (só visuais).
            // Clicar em qualquer botão também seleciona a faixa, respeitando
            // Shift/Ctrl.
            if (e->modifiers() & Qt::ShiftModifier)
                selectTrackRange(brow, baudio);
            else if (e->modifiers() & Qt::ControlModifier)
                toggleTrackSel(brow, baudio);
            else
                setTrackSel(brow, baudio);
            invalidateScene();
            emit modified();
            return;
        }
    }

    // Seleção de faixas: clicar na coluna do cabeçalho seleciona a faixa.
    // Shift = seleção em faixa (ordem exibida), Ctrl = alterna uma a uma.
    if (x < kHeaderW) {
        // Clicar na faixa de cabeçalho de uma pasta seleciona todas as suas
        // faixas; clicar na seta (à esquerda) recolhe/expande a pasta.
        QString gid;
        if (folderStripAt(y, gid)) {
            TrackGroup* g = m_project->findGroup(gid);
            const QRect ar = g ? folderArrowRect(*g) : QRect();
            if (g && x >= ar.left() && x <= ar.right()) {
                toggleGroupCollapsed(gid);
                return;
            }
            if (e->modifiers() & Qt::ControlModifier)
                toggleGroupTracks(gid);
            else
                selectGroupTracks(gid);
            refreshView();
            // Prepara o arrasto do grupo inteiro (mover a pasta acima/abaixo).
            m_dragGroupId = gid;
            m_dragTrackRow = -1;
            m_trackDragActive = false;
            m_dragTrackRec = false;
            m_dropRow = -1;
            m_dropAudio = false;
            m_dropGroup.clear();
            m_dragStart = e->pos();
            return;
        }
        int srow;
        bool saudio;
        if (rowFromY(y, srow, saudio)) {
            if (e->modifiers() & Qt::ControlModifier)
                toggleTrackSel(srow, saudio);
            else if (e->modifiers() & Qt::ShiftModifier)
                selectTrackRange(srow, saudio);
            else
                setTrackSel(srow, saudio);
            refreshView();
            // Prepara o arrasto de faixa (mover acima/abaixo ou soltar em pasta).
            m_dragTrackRow = srow;
            m_dragTrackAudio = saudio;
            m_dragTrackRec = false;
            m_trackDragActive = false;
            m_dropRow = -1;
            m_dropAudio = false;
            m_dropGroup.clear();
            m_dragStart = e->pos();
            return;
        }
        // Cabeçalho da seção de gravação: arrastar pra cima promove a faixa
        // pra uma faixa de áudio comum (ver finishTrackDrag).
        int rrow;
        if (recRowFromY(y, rrow)) {
            setRecTrackSel(rrow);
            refreshView();
            m_dragTrackRow = rrow;
            m_dragTrackAudio = true;
            m_dragTrackRec = true;
            m_trackDragActive = false;
            m_dropRow = -1;
            m_dropAudio = false;
            m_dropGroup.clear();
            m_dragStart = e->pos();
            return;
        }
    }

    // Painel de efeitos da faixa de efeitos expandida (estilo AE): o "+" grava
    // a escolha NA FAIXA (aplica aos clipes dela), o olho liga/desliga a escolha
    // (propagando aos clipes) e o clique na linha abre o efeito no dock Express.
    {
        Clip* fxClip = nullptr;
        int fpart = 0;
        QString feid;
        bool fen = false;
        if (fxPanelHit(e->pos(), fxClip, fpart, feid, fen)) {
            QString fxtrackId;
            {
                int trow = -1;
                bool taudio = false;
                if (rowFromY(e->pos().y(), trow, taudio) && !taudio
                    && trow >= 0 && trow < (int)m_project->videoTracks.size())
                    fxtrackId = m_project->videoTracks[trow].id;
            }
            if (fpart == 1) {
                QMenu menu(this);
                buildFxAddMenu(&menu);
                QAction* act = menu.exec(e->globalPosition().toPoint());
                if (act) {
                    const QString id = act->data().toString();
                    if (!id.isEmpty())
                        emit fxEffectAddRequested(fxtrackId, id);
                }
            } else if (fpart == 3) {
                emit fxEffectToggleRequested(fxtrackId, feid, !fen);
            } else if (fpart == 4) {
                emit fxEffectRemoveRequested(fxtrackId, feid);
            } else if (fpart == 2 && fxClip) {
                emit fxEffectEditRequested(fxClip->id, feid);
            }
            return;
        }
    }

    int row;
    bool audio;
    if (rowFromY(y, row, audio)) {
        const double t = xToTime(x);
        Clip* clip = clipAt(row, audio, t);
        // Clique na timeline move a agulha para onde o mouse clicou — inclusive
        // sobre clipes (estilo Vegas). Ctrl preserva o playhead (soma seleção).
        if (!(e->modifiers() & Qt::ControlModifier)) {
            setPlayhead(std::max(0.0, snapTime(t)));
            emit playheadChanged(std::max(0.0, snapTime(t)));
        }

        // Shift+clique em qualquer ponto da faixa (mesmo sobre um clipe)
        // seleciona o intervalo de faixas na ordem exibida, mantendo o que já
        // estava selecionado a partir da âncora.
        if (e->modifiers() & Qt::ShiftModifier) {
            selectTrackRange(row, audio);
            refreshView();
            return;
        }

        if (m_tool == ToolScissors) {
            m_dragMode = Razor;
            m_razorT = std::max(0.0, snapTime(t));
            m_dragStart = e->pos();
            update();
            return;
        }
        if (m_tool == ToolEnvelope) {
            // Clique alterna ponto na área vazia da faixa; sobre clipe,
            // alterna o envelope do clipe. (Diamantes de keyframe já foram
            // tratados acima, valendo para qualquer ferramenta.)
            m_envPending = false; // já tratado aqui; evita o toggle no release
            if (clip) {
                envelopePress(clip, std::max(0.0, snapTime(t)));
            } else if (row >= 0) {
                trackEnvelopePress(row, audio, std::max(0.0, snapTime(t)));
            }
            return;
        }
        if (m_tool == ToolZoom) {
            m_dragMode = ZoomSelect;
            m_zoomT0 = std::max(0.0, t);
            m_zoomT1 = m_zoomT0;
            m_dragStart = e->pos();
            update();
            return;
        }

        // Seleção de faixas também clicando no corpo da faixa: clique sem
        // clipe seleciona/alterna a faixa. O playhead continua movendo e o
        // marquee continua funcionando no arraste.
        if (!clip && (m_tool == ToolSelect || m_tool == ToolMove)) {
            if (e->modifiers() & Qt::ControlModifier)
                toggleTrackSel(row, audio);
            else
                setTrackSel(row, audio);
            refreshView();
        }

        if (clip) {
            // Ctrl no clipe: clique alterna a seleção do CLIPE (permite
            // selecionar vários); arrastar DUPLICA o clipe e move a cópia
            // (estilo Vegas). A decisão vem no movimento (>=4px) ou release.
            if (e->modifiers() & Qt::ControlModifier) {
                m_dragMode = None;
                m_ctrlPending = true;
                m_ctrlClipId = clip->id;
                m_ctrlCanMove = !trackLocked(clip);
                m_dragStart = e->pos();
                return;
            }
            if (!isSelected(clip->id))
                setSelection(clip->id);
            m_dragMode = None;
            m_rippleMove = false;
            const int cx = (int)timeToX(clip->pos);
            const int cw = (int)(clip->dur * m_pps);
            const int dx = x - cx;
            // Alça de fade (estilo Vegas): no CANTINHO SUPERIOR do clipe.
            // Arrastar o canto esquerdo/certo ajusta fade-in/fade-out, sem
            // conflitar com o trim (que fica na borda lateral). Vale nas
            // ferramentas Selecionar (0) e Mover (M).
            const int topY = audio ? rowY(-1, row) : rowY(row, -1);
            const bool nearTop = (y - topY) <= 14;
            if (nearTop && dx <= 10) {
                m_dragMode = FadeIn;
                m_dragOrigFade = clip->fadeIn;
            } else if (nearTop && cw - dx <= 10) {
                m_dragMode = FadeOut;
                m_dragOrigFade = clip->fadeOut;
            } else if (m_tool == ToolMove) {
                m_dragMode = MoveClip;
            } else if (m_tool == ToolRipple) {
                // Ripple: nas BORDAS, trim com ripple; no MEIO do clipe, mover o
                // clipe levando junto os clipes à frente (estilo Vegas).
                if (cw > 24 && dx > 10 && cw - dx > 10) {
                    m_dragMode = MoveClip;
                    m_rippleMove = true;
                } else if (dx <= cw / 2) {
                    m_dragMode = TrimLeft;  // TrimLeft com ripple
                } else {
                    m_dragMode = TrimRight; // TrimRight com ripple
                }
                if (m_dragMode != MoveClip) {
                    // rippleTrimLeft/Right medem o delta contra o estado CORRENTE
                    // do clipe e deslocam os vizinhos, então um arraste ao vivo
                    // precisa voltar ao estado original a cada mousemove. O
                    // ripple tem sync AV: desloca em TODAS as faixas e ajusta o
                    // par vinculado, então guarda o clipe, o grupo e todos os
                    // clipes à frente (candidatos a deslocamento) em qualquer
                    // faixa — não só a faixa arrastada.
                    m_rippleOrig.clear();
                    auto snapRipple = [&](QVector<Track>& tracks) {
                        for (Track& tr : tracks) {
                            for (const Clip& other : tr.clips) {
                                const bool grupo = !clip->groupId.isEmpty()
                                    && other.groupId == clip->groupId;
                                if (other.id == clip->id || grupo
                                    || other.pos >= clip->pos - 1e-6)
                                    m_rippleOrig.insert(other.id,
                                                        { other.pos, other.in, other.dur });
                            }
                        }
                    };
                    snapRipple(m_project->videoTracks);
                    snapRipple(m_project->audioTracks);
                }
            } else if (m_tool == ToolRolling) {
                // Rolling Edit: ajustar fronteira entre 2 clipes
                auto [clipA, clipB] = adjacentClips(clip);
                if (clipA && clipB) {
                    m_dragMode = RollingEdit;
                    m_rollClipA = clipA->id;
                    m_rollClipB = clipB->id;
                    m_rollOrigDurA = clipA->dur;
                    m_rollOrigDurB = clipB->dur;
                } else {
                    m_dragMode = MoveClip; // Fallback
                }
            } else if (m_tool == ToolSlip) {
                // Slip Edit: mudar in/out sem mudar posição
                m_dragMode = SlipEdit;
                m_slipOrigIn = clip->in;
                m_slipOrigOut = clip->in + clip->dur;
            } else if (m_tool == ToolSlide) {
                // Slide Edit: mover clipe e adjacentes ajustam
                m_dragMode = SlideEdit;
                auto [prev, next] = adjacentClips(clip);
                m_slidePrevId = prev ? prev->id : QString();
                m_slideNextId = next ? next->id : QString();
                m_slideOrigPosA = prev ? prev->pos : 0;
                m_slideOrigDurA = prev ? prev->dur : 0;
                m_slideOrigPosB = clip->pos;
                m_slideOrigDurB = clip->dur;
                m_slideOrigPosC = next ? next->pos : 0;
                m_slideOrigDurC = next ? next->dur : 0;
            } else if (m_tool == ToolRateStretch) {
                // Rate Stretch: mudar velocidade para preencher espaço
                m_dragMode = RateStretch;
                m_rateOrigSpeed = clip->speed;
                m_rateOrigDur = clip->dur;
            } else if (dx <= 8) {
                m_dragMode = TrimLeft;
            } else if (cw - dx <= 8) {
                m_dragMode = TrimRight;
            } else {
                m_dragMode = MoveClip;
            }
            if (trackLocked(clip)) m_dragMode = None; // faixa travada: só seleciona
            m_dragClip = clip->id;
            m_dragUndoPushed = false;
            m_dragStart = e->pos();
            m_dragOrigPos = clip->pos;
            m_dragOrigIn = clip->in;
            m_dragOrigDur = clip->dur;
            m_dragOrig.clear();
            m_rippleMoveOrig.clear();
            // Alt = arrastar apenas o clipe, sem arrastar o grupo vinculado.
            const bool solo = (e->modifiers() & Qt::AltModifier) != 0;
            auto addOrig = [this, solo](Clip* c) {
                if (!c) return;
                if (!solo && !c->groupId.isEmpty()) {
                    for (Clip* m : groupMembers(c->groupId))
                        m_dragOrig.insert(m->id, {m->pos, m->in, m->dur});
                } else {
                    m_dragOrig.insert(c->id, {c->pos, c->in, c->dur});
                }
            };
            for (const QString& sid : m_selected) {
                Clip* sc = findClipById(sid);
                addOrig(sc);
            }
            addOrig(clip);

            // Ripple MOVE: registra os clipes que estão à frente (por faixa) para
            // deslocá-los junto com o arraste.
            if (m_rippleMove) {
                auto snapshotDownstream = [this](QVector<Track>& tracks) {
                    for (Track& tr : tracks) {
                        // Borda direita original do(s) clipe(s) arrastado(s) nesta faixa.
                        double rightEdge = -1.0;
                        for (const Clip& c : tr.clips) {
                            auto it = m_dragOrig.constFind(c.id);
                            if (it != m_dragOrig.constEnd())
                                rightEdge = std::max(rightEdge, it.value().pos + it.value().dur);
                        }
                        if (rightEdge < 0.0) continue;
                        for (const Clip& c : tr.clips) {
                            if (m_dragOrig.contains(c.id)) continue;
                            if (c.pos >= rightEdge - 1e-6)
                                m_rippleMoveOrig.insert(c.id, c.pos);
                        }
                    }
                };
                snapshotDownstream(m_project->videoTracks);
                snapshotDownstream(m_project->audioTracks);
            }
        } else if (m_tool == ToolSelect || m_tool == ToolMove) {
            // Espaço vazio com a ferramenta de seleção: caixa de seleção
            // (marquee). Com Ctrl, soma aos já selecionados.
            m_dragMode = Marquee;
            m_dragStart = e->pos();
            m_marqueeRect = QRect(e->pos(), QSize(0, 0));
            update();
        } else {
            m_selected.clear();
            m_secondarySelected.clear();
            refreshView();
            emit selectionChanged(QString());
        }
    } else {
        m_selected.clear();
        m_secondarySelected.clear();
        clearTrackSelection();
        refreshView();
        emit selectionChanged(QString());
        update();
    }
}


