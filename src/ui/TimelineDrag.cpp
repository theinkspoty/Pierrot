// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TimelineWidget.h"
#include "models/Project.h"
#include "ffmpeg/FFmpegDecoder.h"
#include "ui/SettingsDialog.h"
#include "ui/TrimmerDialog.h"
#include "util.h"

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
#include <QSet>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kHeaderW = 130;
constexpr int kRulerH = 26;
constexpr int kZoomW = 64;
constexpr int kMinDragH = 40;
constexpr int kMaxRowH = 400;
constexpr int kResizeHandleH = 5;
constexpr double kMinDur = 0.04;

enum Tool {
    ToolSelect = 0, ToolMove = 1, ToolScissors = 2, ToolEnvelope = 3, ToolZoom = 4,
    ToolRipple = 5, ToolRolling = 6, ToolSlip = 7, ToolSlide = 8, ToolRateStretch = 9
};

double mediaInsertDur(const MediaItem& m) {
    if (isImageFile(m.filePath)) return 3.0;
    return m.duration > 0 ? m.duration : 1.0;
}
} // namespace

// ── Autoscroll ──────────────────────────────────────────────────────────

// Liga o autoscroll quando o mouse (durante PlayheadDrag, RulerLoop ou o
// arraste de um clipe) está perto da borda esquerda/direita da view. A rolagem
// contínua faz a agulha/clipe acompanhar o cursor sem que ele saia da janela.
void TimelineWidget::startAutoScroll(QMouseEvent* e) {
    if (!m_project) return;
    m_autoScrollMouse = e->pos();
    const int edge = 32;
    const int vw = width() - m_vbar->sizeHint().width();
    const int mx = m_autoScrollMouse.x();
    int dir = 0;
    if (mx >= kHeaderW && mx <= kHeaderW + edge)
        dir = -1;
    else if (mx >= vw - edge)
        dir = +1;
    if (dir != 0) {
        m_autoScrollDir = dir;
        if (!m_autoScroll->isActive()) m_autoScroll->start();
    } else {
        stopAutoScroll();
    }
}

void TimelineWidget::stopAutoScroll() {
    m_autoScroll->stop();
    m_autoScrollDir = 0;
}

// Rola a timeline e, quando aplicável, reposiciona a agulha/loop/arrasto sob o
// cursor para que a operação continue na nova área visível.
void TimelineWidget::autoScrollTick() {
    if (m_dragMode != PlayheadDrag && m_dragMode != RulerLoop
        && m_dragMode != RulerLoopEdge
        && m_dragMode != MoveClip && m_dragMode != TrimLeft
        && m_dragMode != TrimRight) {
        stopAutoScroll();
        return;
    }
    const int viewW = width() - m_vbar->sizeHint().width();
    const int step = qMax(8, viewW / 24) * m_autoScrollDir;
    int v = m_hbar->value() + step;
    if (v < m_hbar->minimum()) v = m_hbar->minimum();
    if (v > m_hbar->maximum()) v = m_hbar->maximum();
    m_hbar->setValue(v);

    // Recalcula a operação na nova posição da view.
    const double autoT = std::max(0.0, snapTime(xToTime(m_autoScrollMouse.x())));
    if (m_dragMode == RulerLoopEdge) {
        if (m_loopEdge == LoopEdgeIn)
            m_loopIn = std::min(autoT, m_loopEdgeOther - 0.02);
        else
            m_loopOut = std::max(autoT, m_loopEdgeOther + 0.02);
        emit loopChanged(m_loopIn, m_loopOut);
        update();
        return;
    }
    if (m_dragMode == PlayheadDrag || m_dragMode == RulerLoop) {
        const double t2 = std::max(0.0, snapTime(xToTime(m_autoScrollMouse.x())));
        setPlayhead(t2);
        emit playheadChanged(t2);
        update();
        return;
    }
    // MoveClip/TrimLeft/TrimRight: reposiciona a operação sob o cursor.
    QMouseEvent ev(QEvent::MouseMove, m_autoScrollMouse,
                   mapToGlobal(m_autoScrollMouse), Qt::LeftButton,
                   Qt::LeftButton, Qt::NoModifier);
    mouseMoveEvent(&ev);
}

// ── Hit-test de cabeçalho e faixas ──────────────────────────────────────

// Retorna o índice da alça de redimensionamento na qual o cursor está (0) ou
// -1 se fora. Só vale na coluna de cabeçalho (x < kHeaderW).
int TimelineWidget::resizeHandleAt(const QPoint& pos, int& row, bool& audio) const {
    if (pos.x() >= kHeaderW || pos.y() < kRulerH || !m_project) return -1;
    if (!rowFromY(pos.y(), row, audio)) return -1;
    const int y = audio ? rowY(-1, row) : rowY(row, -1);
    const int rowH = trackH(row, audio);
    if (pos.y() >= y + rowH - kResizeHandleH && pos.y() < y + rowH)
        return 0;
    return -1;
}

// Se o ponto está sobre a linha de volume de um clipe de áudio, retorna o
// clipe (dentro de uma tolerância vertical e do intervalo horizontal dele).
Clip* TimelineWidget::clipVolAt(const QPoint& pos, int& row) const {
    if (!m_project || pos.x() < kHeaderW || pos.y() < kRulerH) return nullptr;
    int r = -1;
    bool audio = false;
    if (!rowFromY(pos.y(), r, audio) || !audio) return nullptr;
    const double t = xToTime(pos.x());
    Clip* c = clipAt(r, true, t);
    if (!c) return nullptr;
    const int ly = clipVolLineY(r, *c);
    if (std::abs(pos.y() - ly) <= 4) { row = r; return c; }
    return nullptr;
}

// Se o ponto está sobre uma faixa de áudio (na área de conteúdo, fora de um
// clipe), retorna o índice da faixa para o ajuste de volume. Toda a altura da
// faixa é aceita: segurar sobre a faixa e arrastar para cima/baixo ajusta o
// volume (estilo Vegas), sem precisar acertar a linha. -1 se fora.
int TimelineWidget::volRowAt(const QPoint& pos, int& row) const {
    if (!m_project || pos.x() < kHeaderW || pos.y() < kRulerH) return -1;
    int r = -1;
    bool audio = false;
    if (!rowFromY(pos.y(), r, audio) || !audio) return -1;
    if (pos.y() < rowY(-1, r) || pos.y() >= rowY(-1, r) + trackH(r, true)) return -1;
    row = r;
    return 0;
}

// 0=M, 1=S, 2=L, -1=nenhum.
int TimelineWidget::headerBtnAt(const QPoint& pos, int& row, bool& audio) const {
    if (pos.x() >= kHeaderW || pos.y() < kRulerH || !m_project) return -1;
    if (!rowFromY(pos.y(), row, audio)) return -1;
    const int rowH = trackH(row, audio);
    const int y = audio ? rowY(-1, row) : rowY(row, -1);
    const int btnY = y + rowH - 22;
    if (pos.y() < btnY || pos.y() >= btnY + 18) return -1;
    const int dx = pos.x() - 6;
    const int size = 18, gap = 3;
    for (int i = 0; i < 3; ++i) {
        const int bx = i * (size + gap);
        if (dx >= bx && dx < bx + size) return i;
    }
    return -1;
}

// ── Mouse events ────────────────────────────────────────────────────────

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
    if (!m_project) return;
    const int x = e->pos().x();
    const int y = e->pos().y();
    setFocus();

    if (e->button() != Qt::LeftButton) return;

    m_volPending = false;

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

    // Régua de volume das faixas de áudio: arrastar a linha ajusta o volume.
    // Só quando não há clipe sob o cursor (o clique no clipe continua
    // selecionando/arrastando normalmente, mesmo sobre a linha).
    if (m_showVolLines) {
        int vrow;
        const bool onLine = volRowAt(e->pos(), vrow) >= 0
            && std::abs(e->pos().y() - volLineY(vrow, true, m_project->audioTracks[vrow])) <= 6;
        if (volRowAt(e->pos(), vrow) >= 0) {
            int r2;
            bool a2;
            bool overClip = false;
            if (rowFromY(y, r2, a2) && clipAt(r2, a2, xToTime(x)) != nullptr)
                overClip = true;
            if (!overClip && onLine) {
                emit editStart();
                m_dragMode = TrackVol;
                m_volRow = vrow;
                m_volOrig = m_project->audioTracks[vrow].volume;
                m_dragStart = e->pos();
                setCursor(Qt::SizeVerCursor);
                update();
                return;
            }
            if (!overClip && !onLine && (m_tool == ToolSelect || m_tool == ToolMove)) {
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

    // Barra de opacidade da faixa de vídeo no cabeçalho: arrastar ajusta opacidade.
    if (x < kHeaderW) {
        int orow;
        bool oaudio;
        if (rowFromY(y, orow, oaudio) && !oaudio && orow >= 0) {
            const int oy = rowY(orow, -1);
            const int barY = oy + 34;
            const int barH = 4;
            if (y >= barY && y < barY + barH && x >= 6 && x < kHeaderW - 6) {
                emit editStart();
                m_dragMode = TrackOp;
                m_volRow = orow;
                m_volOrig = m_project->videoTracks[orow].opacity;
                m_dragStart = e->pos();
                setCursor(Qt::SizeHorCursor);
                update();
                return;
            }
        }
    }

    // Botões de cabeçalho (M/S/L) das faixas.
    {
        int brow;
        bool baudio;
        const int b = headerBtnAt(e->pos(), brow, baudio);
        if (b >= 0) {
            Track* tr = baudio ? &m_project->audioTracks[brow]
                               : &m_project->videoTracks[brow];
            emit editStart();
            if (b == 0) tr->muted = !tr->muted;
            else if (b == 1) tr->solo = !tr->solo;
            else tr->locked = !tr->locked;
            // Clicar no botão também seleciona a faixa (qualquer lugar do
            // cabeçalho seleciona), respeitando Shift/Ctrl.
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
            m_trackDragActive = false;
            m_dropRow = -1;
            m_dropAudio = false;
            m_dropGroup.clear();
            m_dragStart = e->pos();
            return;
        }
    }

    int row;
    bool audio;
    if (rowFromY(y, row, audio)) {
        const double t = xToTime(x);
        Clip* clip = clipAt(row, audio, t);
        // Clique na timeline move a agulha para onde o mouse clicou — mas
        // SOMENTE em espaço vazio. Se há um clipe sob o cursor, o usuário
        // provavelmente quer movê-lo/redimensionar, e a agulha não deve pular.
        if (!clip && !(e->modifiers() & Qt::ControlModifier)) {
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
            if (clip) envelopePress(clip, std::max(0.0, snapTime(t)));
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
            // Ctrl+clique sobre um clipe alterna a seleção do CLIPE (permite
            // selecionar vários clipes). Seleção de faixas fica no cabeçalho e
            // no corpo vazio da faixa (que continuam com Ctrl = alternar faixa).
            if (e->modifiers() & Qt::ControlModifier) {
                toggleSelection(clip->id);
                return;
            }
            if (!isSelected(clip->id))
                setSelection(clip->id);
            m_dragMode = None;
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
            } else if (nearTop && !audio) {
                // Vegas: segurar no TOPO do clipe de vídeo (centro) e arrastar
                // para baixo reduz a opacidade do clipe; para cima aumenta.
                m_dragMode = ClipOpacity;
                m_dragOrigOpacity = clip->opacity;
            } else if (m_tool == ToolMove) {
                m_dragMode = MoveClip;
            } else if (m_tool == ToolRipple) {
                // Ripple Edit: trim com ripple (desloca subsequentes)
                if (dx <= cw / 2) {
                    m_dragMode = TrimLeft;  // TrimLeft com ripple
                } else {
                    m_dragMode = TrimRight; // TrimRight com ripple
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

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!m_project) return;

    m_mousePos = e->pos();
    m_mouseOnRuler = (e->pos().y() < kRulerH);

    // Destaque das alças (opacidade no centro, fades nos cantos) ao passar o
    // mouse sobre o topo de um clipe de vídeo. Repinta só quando o alvo muda.
    // Ignorado durante arrastos.
    QString newGrip, newCorner;
    int newSide = 0;
    if (m_dragMode == None && !(e->buttons() & Qt::LeftButton)
        && e->pos().y() >= kRulerH) {
        bool hAudio = false;
        int hrow = -1;
        if (rowFromY(e->pos().y(), hrow, hAudio) && hrow >= 0) {
            Clip* hc = clipAt(hrow, hAudio, std::max(0.0, xToTime(e->pos().x())));
            if (hc) {
                const int topY = hAudio ? rowY(-1, hrow) : rowY(hrow, -1);
                if (e->pos().y() - topY <= 14) {
                    const int hcx = (int)timeToX(hc->pos);
                    const int hcw = (int)(hc->dur * m_pps);
                    const int hdx = e->pos().x() - hcx;
                    if (hdx <= 12) { newCorner = hc->id; newSide = -1; }
                    else if (hcw - hdx <= 12) { newCorner = hc->id; newSide = 1; }
                    else if (!hAudio) newGrip = hc->id;
                }
            }
        }
    }
    if (newGrip != m_hoverGripClip || newCorner != m_hoverCornerClip
        || newSide != m_hoverCornerSide) {
        m_hoverGripClip = newGrip;
        m_hoverCornerClip = newCorner;
        m_hoverCornerSide = newSide;
        update();
    }

    // Agulha "ponteiro" branca: segue o cursor enquanto ele está sobre a régua
    // e fica parada na última posição (independente da agulha de reprodução).
    if (e->pos().y() < kRulerH) {
        const double nt = std::max(0.0, xToTime(e->pos().x()));
        if (std::fabs(nt - m_cursorT) > 1e-9) {
            m_cursorT = nt;
            update();
        }
    }

    if (e->buttons() & Qt::LeftButton) {
        // Arrasto de faixa ou grupo: cruza o limiar e passa a mover/reordenar.
        if ((m_dragTrackRow >= 0 || !m_dragGroupId.isEmpty()) && !m_trackDragActive
            && (e->pos() - m_dragStart).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragMode = TrackDrag;
            m_trackDragActive = true;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_dragMode == TrackDrag) {
            const QPoint p = e->pos();
            QString gid;
            if (p.x() < kHeaderW && folderStripAt(p.y(), gid)) {
                // Sobre uma faixa de pasta: destaca o grupo e calcula o alvo
                // como a primeira faixa abaixo da pasta (para reordenar).
                m_dropGroup = gid;
                const TrackGroup* g = m_project->findGroup(gid);
                if (g) {
                    const QRect fr = folderStripRect(*g);
                    int trow;
                    bool taudio;
                    if (rowFromY(fr.bottom() + 1, trow, taudio)) {
                        m_dropRow = trow;
                        m_dropAudio = taudio;
                    } else {
                        m_dropRow = -1;
                    }
                } else {
                    m_dropRow = -1;
                }
            } else {
                m_dropGroup.clear();
                int trow;
                bool taudio;
                if (rowFromY(p.y(), trow, taudio)) {
                    m_dropRow = trow;
                    m_dropAudio = taudio;
                } else {
                    m_dropRow = -1;
                }
            }
            update();
            e->accept();
            return;
        }
        if (m_dragMode == ResizeTrack) {
            const int dy = e->pos().y() - m_dragStart.y();
            // Se a faixa arrastada está selecionada, redimensiona todas as
            // faixas selecionadas juntas (mesmo delta).
            if (isTrackSelected(m_resizeRow, m_resizeAudio)) {
                bool any = false;
                int j = 0;
                for (const TrackSel& s : m_selTracks) {
                    if (s.row < 0) continue;
                    const int origH = (j < m_resizeSelOrigH.size())
                        ? m_resizeSelOrigH[j] : trackH(s.row, s.audio);
                    ++j;
                    Track* tt = s.audio
                        ? (s.row < (int)m_project->audioTracks.size() ? &m_project->audioTracks[s.row] : nullptr)
                        : (s.row < (int)m_project->videoTracks.size() ? &m_project->videoTracks[s.row] : nullptr);
                    if (!tt) continue;
                    // Usa a altura ORIGINAL capturada no início do arraste (e
                    // não a atual), senão o delta total acumula a cada movimento.
                    const int nh = std::clamp(origH + dy, kMinDragH, kMaxRowH);
                    if (tt->height != nh) {
                        tt->height = nh;
                        any = true;
                    }
                }
                if (any) {
                    updateScrollRanges();
                    invalidateScene();
                    update();
                    emit modified();
                }
            } else {
                Track& t = m_resizeAudio ? m_project->audioTracks[m_resizeRow]
                                         : m_project->videoTracks[m_resizeRow];
                const int newH = std::clamp(m_resizeOrigH + dy, kMinDragH, kMaxRowH);
                if (t.height != newH) {
                    t.height = newH;
                    updateScrollRanges();
                    invalidateScene();
                    update();
                    emit modified();
                }
            }
            return;
        }
        if (m_dragMode == ClipVol) {
            Clip* clip = findClipById(m_volClip);
            if (clip && m_volRowOrig >= 0 && m_volRowOrig < (int)m_project->audioTracks.size()) {
                const int rowH = trackH(m_volRowOrig, true);
                const int y = rowY(-1, m_volRowOrig);
                const int pad = 6;
                const double frac = 1.0 - std::clamp(
                    (double)(e->pos().y() - (y + pad)) / (rowH - pad * 2.0), 0.0, 1.0);
                const double v = frac * 2.0;
                if (std::fabs(clip->volume - v) > 1e-4) {
                    clip->volume = v;
                    refreshView();
                    update();
                    emit modified();
                }
            }
            return;
        }
        if (m_dragMode == TrackVol) {
            if (m_volRow < 0 || m_volRow >= (int)m_project->audioTracks.size()) return;
            Track& t = m_project->audioTracks[m_volRow];
            const int rowH = trackH(m_volRow, true);
            const int y = rowY(-1, m_volRow);
            const int pad = 6;
            const double frac = 1.0 - std::clamp(
                (double)(e->pos().y() - (y + pad)) / (rowH - pad * 2.0), 0.0, 1.0);
            const double v = frac * 2.0;
            if (std::fabs(t.volume - v) > 1e-4) {
                t.volume = v;
                refreshView();
                update();
                emit modified();
            }
            return;
        }
        if (m_dragMode == TrackOp) {
            if (m_volRow < 0 || m_volRow >= (int)m_project->videoTracks.size()) return;
            Track& t = m_project->videoTracks[m_volRow];
            // Arrasto horizontal: esquerda = 0%, direita = 100%.
            const int barX0 = 6;
            const int barW = kHeaderW - 12;
            const double frac = std::clamp(
                (double)(e->pos().x() - barX0) / barW, 0.0, 1.0);
            if (std::fabs(t.opacity - frac) > 1e-4) {
                t.opacity = frac;
                update();
                emit modified();
            }
            return;
        }
        if (m_dragMode == PlayheadDrag) {
            const double t2 = std::max(0.0, snapTime(xToTime(e->pos().x())));
            setPlayhead(t2);
            emit playheadChanged(t2);
            startAutoScroll(e);
            update();
            return;
        }
        if (m_dragMode == RulerLoop) {
            const double cur = std::max(0.0, snapTime(xToTime(e->pos().x())));
            const double a = std::min(m_loopPressT, cur);
            const double b = std::max(m_loopPressT, cur);
            if (b - a > 0.02) {
                m_loopIn = a;
                m_loopOut = b;
                emit loopChanged(m_loopIn, m_loopOut);
            }
            startAutoScroll(e);
            update();
            return;
        }
        if (m_dragMode == RulerLoopEdge) {
            const double cur = std::max(0.0, snapTime(xToTime(e->pos().x())));
            if (m_loopEdge == LoopEdgeIn) {
                m_loopIn = std::min(cur, m_loopEdgeOther - 0.02);
            } else {
                m_loopOut = std::max(cur, m_loopEdgeOther + 0.02);
            }
            emit loopChanged(m_loopIn, m_loopOut);
            startAutoScroll(e);
            update();
            return;
        }
        if (m_dragMode == Razor) {
            m_razorT = std::max(0.0, snapTime(xToTime(e->pos().x())));
            update();
            return;
        }
        if (m_dragMode == ZoomSelect) {
            m_zoomT1 = std::max(0.0, xToTime(e->pos().x()));
            update();
            return;
        }
        if (m_dragMode == Marquee) {
            // Press na faixa de áudio vazia: vira ajuste de volume se o
            // arraste for predominantemente vertical (para cima/baixo).
            if (m_volPending) {
                const int dy = e->pos().y() - m_dragStart.y();
                const int dx = e->pos().x() - m_dragStart.x();
                if (std::abs(dy) > std::abs(dx) + 4 && std::abs(dy) >= 5) {
                    if (m_volRow >= 0 && m_volRow < (int)m_project->audioTracks.size()) {
                        emit editStart();
                        m_dragMode = TrackVol;
                        m_volPending = false;
                        m_volOrig = m_project->audioTracks[m_volRow].volume;
                        setCursor(Qt::SizeVerCursor);
                        // aplica o volume já neste movimento
                        if (m_dragMode == TrackVol) {
                            Track& t = m_project->audioTracks[m_volRow];
                            const int rowH = trackH(m_volRow, true);
                            const int y = rowY(-1, m_volRow);
                            const int pad = 6;
                            const double frac = 1.0 - std::clamp(
                                (double)(e->pos().y() - (y + pad)) / (rowH - pad * 2.0), 0.0, 1.0);
                            t.volume = frac * 2.0;
                            refreshView();
                        }
                        update();
                        emit modified();
                        return;
                    }
                }
            }
            m_marqueeRect = QRect(m_dragStart, e->pos()).normalized();
            update();
            return;
        }
        if (m_dragMode != None && !m_dragClip.isEmpty()) {
            Clip* clip = findClipById(m_dragClip);
            if (clip) {
                const double dt = (e->pos().x() - m_dragStart.x()) / m_pps;
                if (!m_dragUndoPushed) {
                    // Arrasto de opacidade é vertical: usa o dy (não o dt que
                    // depende do eixo do tempo) para disparar o undo.
                    const double dy = e->pos().y() - m_dragStart.y();
                    if ((m_dragMode == ClipOpacity && std::fabs(dy) > 0.5)
                        || std::fabs(dt) > 1e-9) {
                        emit editStart();
                        m_dragUndoPushed = true;
                    }
                }
                if (m_dragMode == MoveClip) {
                    // Troca de faixa durante o arraste: cada clipe segue a
                    // linha sob o mouse quando o tipo bate (vídeo só em faixas
                    // de vídeo, áudio só em faixas de áudio). O membro do
                    // grupo vinculado NÃO acompanha na vertical: ele mantém a
                    // faixa em que está (só a posição no tempo anda junto).
    int row;
    bool audio;
    int vrow;
    int cvrow;
    if (m_showVolLines && clipVolAt(e->pos(), cvrow) != nullptr) {
        setCursor(Qt::SizeVerCursor);
        return;
    }
    if (m_showVolLines && volRowAt(e->pos(), vrow) >= 0) {
        int r2;
        bool a2;
        bool overClip = false;
        if (rowFromY(e->pos().y(), r2, a2) && clipAt(r2, a2, xToTime(e->pos().x())) != nullptr)
            overClip = true;
        if (!overClip) {
            setCursor(Qt::SizeVerCursor);
            return;
        }
    }
    if (rowFromY(e->pos().y(), row, audio)) {
                        // Só o clipe sob o mouse acompanha na vertical (muda de
                        // faixa se o tipo bater). Os demais membros do grupo
                        // mantêm suas faixas — só a posição no tempo anda junto.
                        // Antes movíamos TODOS os membros do tipo para a linha do
                        // mouse: as várias faixas de áudio de um arquivo multicanal
                        // (mesmo grupo) empilhavam todas na mesma faixa e
                        // sumiam umas sob as outras.
                        Clip* dc = findClipById(m_dragClip);
                        if (dc) {
                            int curRow;
                            bool curAudio;
                            if (clipTrackIndex(m_dragClip, curRow, curAudio)
                                && curAudio == audio && curRow != row) {
                                Track& dst = curAudio ? m_project->audioTracks[row]
                                                      : m_project->videoTracks[row];
                                if (!dst.locked)
                                    moveClipToTrack(m_dragClip, row, curAudio);
                            }
                        }
                    }
                    const double raw = m_dragOrigPos + dt;
                    const double snapped = snapToEdges(snapTime(raw), m_dragClip);
                    const double delta = snapped - m_dragOrigPos;
                    QSet<QString> moving;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it)
                        moving.insert(it.key());
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc)
                            sc->pos = clampPosToTrack(sc,
                                                      std::max(0.0, it.value().pos + delta),
                                                      moving);
                    }
                } else if (m_dragMode == TrimLeft) {
                    const double minEnd = std::max(0.0, m_dragOrigPos + m_dragOrigDur - kMinDur);
                    const double np = std::clamp(snapToEdges(snapTime(m_dragOrigPos + dt)),
                                                 0.0, minEnd);
                    const double delta = np - m_dragOrigPos;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (!sc) continue;
                        const ClipOrig& o = it.value();
                        sc->pos = std::max(0.0, o.pos + delta);
                        sc->in = o.in + delta;
                        sc->dur = o.dur - delta;
                    }
                } else if (m_dragMode == TrimRight) {
                    const double end = snapToEdges(snapTime(m_dragOrigPos + m_dragOrigDur + dt));
                    const double newDur = std::max(kMinDur, end - clip->pos);
                    const double deltaDur = newDur - m_dragOrigDur;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc) sc->dur = std::max(kMinDur, it.value().dur + deltaDur);
                    }
                } else if (m_dragMode == TrimLeft && m_tool == ToolRipple) {
                    // Ripple Edit: trim esquerdo com ripple (desloca subsequentes)
                    const double minEnd = std::max(0.0, m_dragOrigPos + m_dragOrigDur - kMinDur);
                    const double np = std::clamp(snapToEdges(snapTime(m_dragOrigPos + dt)),
                                                 0.0, minEnd);
                    const double delta = np - m_dragOrigPos;
                    rippleTrimLeft(clip, m_dragOrigIn + delta);
                } else if (m_dragMode == TrimRight && m_tool == ToolRipple) {
                    // Ripple Edit: trim direito com ripple (desloca subsequentes)
                    const double end = snapToEdges(snapTime(m_dragOrigPos + m_dragOrigDur + dt));
                    const double newDur = std::max(kMinDur, end - clip->pos);
                    rippleTrimRight(clip, newDur);
                } else if (m_dragMode == RollingEdit) {
                    // Rolling Edit: ajustar fronteira entre 2 clipes
                    const double delta = (e->pos().x() - m_dragStart.x()) / m_pps;
                    rollingEdit(delta);
                } else if (m_dragMode == SlipEdit) {
                    // Slip Edit: mudar in/out sem mudar posição
                    const double delta = (e->pos().x() - m_dragStart.x()) / m_pps;
                    slipEdit(clip, delta);
                } else if (m_dragMode == SlideEdit) {
                    // Slide Edit: mover clipe e adjacentes ajustam
                    const double delta = (e->pos().x() - m_dragStart.x()) / m_pps;
                    slideEdit(clip, delta);
                } else if (m_dragMode == RateStretch) {
                    // Rate Stretch: mudar velocidade para preencher espaço
                    const double newDur = std::max(0.1, m_rateOrigDur + dt);
                    const double newSpeed = m_rateOrigSpeed * (m_rateOrigDur / newDur);
                    rateStretch(clip, std::max(0.1, newSpeed));
                } else if (m_dragMode == FadeIn || m_dragMode == FadeOut) {
                    // Alça de fade no canto (estilo Vegas): arrastar horizontal
                    // define a duração do fade em segundos. Arrastar o canto
                    // PARA DENTRO do clipe estende o fade — por isso o sinal
                    // do canto direito é invertido (dt fica negativo ao ir p/ esq).
                    Clip* sc = findClipById(m_dragClip);
                    if (sc) {
                        const double dur = sc->dur;
                        const double d = m_dragMode == FadeIn ? dt : -dt;
                        const double nf = std::clamp(m_dragOrigFade + d, 0.0,
                                                     std::max(0.0, dur - 0.1));
                        if (m_dragMode == FadeIn) sc->fadeIn = nf;
                        else sc->fadeOut = nf;
                        invalidateScene();
                    }
                } else if (m_dragMode == ClipOpacity) {
                    // Vegas: arrastar o topo do clipe de vídeo para baixo reduz
                    // a opacidade (0–100%), para cima aumenta.
                    Clip* sc = findClipById(m_dragClip);
                    int crow;
                    bool caudio;
                    if (sc && clipTrackIndex(sc->id, crow, caudio) && !caudio) {
                        const int rowH = trackH(crow, false);
                        if (rowH > 0) {
                            const double dy = (double)(e->pos().y() - m_dragStart.y())
                                              / (double)rowH;
                            sc->opacity = std::clamp(m_dragOrigOpacity - dy, 0.0, 1.0);
                            invalidateScene();
                        }
                    }
                }
                updateScrollRanges();
                update();
                emit modified();
            }
        }
        startAutoScroll(e);
        return;
    }

    int hrow;
    bool haudio;
    if (resizeHandleAt(e->pos(), hrow, haudio) >= 0) {
        setCursor(Qt::SizeVerCursor);
        return;
    }

    if (m_tool == ToolScissors) {
        setCursor(Qt::SplitVCursor);
        return;
    }
    if (m_tool == ToolEnvelope || m_tool == ToolZoom) {
        setCursor(Qt::CrossCursor);
        return;
    }

    int row;
    bool audio;
    if (rowFromY(e->pos().y(), row, audio)) {
        Clip* clip = clipAt(row, audio, xToTime(e->pos().x()));
        if (clip) {
            const int cx = (int)timeToX(clip->pos);
            const int cw = (int)(clip->dur * m_pps);
            const int dx = e->pos().x() - cx;
            const bool nearTop = (e->pos().y() - rowY(row, audio)) <= 12;
            if (nearTop && (dx <= 10 || cw - dx <= 10)) {
                setCursor(Qt::SizeVerCursor); // alças de fade (cantos superiores)
            } else if (nearTop && !audio) {
                setCursor(Qt::SizeVerCursor); // alça de opacidade (topo do vídeo)
            } else if (m_tool == ToolMove) {
                setCursor(Qt::OpenHandCursor);
            } else if (dx <= 8 || cw - dx <= 8) {
                setCursor(Qt::SizeHorCursor);
            } else {
                setCursor(Qt::OpenHandCursor);
            }
        } else {
            setCursor(Qt::ArrowCursor);
        }
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e) {
    switch (m_dragMode) {
    case RulerLoop:
    case RulerLoopEdge:
        m_loopEdge = LoopEdgeNone;
        setCursor(Qt::ArrowCursor);
        break;
    case Razor:
        razorSplitAt(m_razorT);
        break;
    case Marquee:
        if (m_marqueeRect.width() < 4 && m_marqueeRect.height() < 4) {
            // Clique simples no vazio: desfaz a seleção (Ctrl mantém).
            if (!(e->modifiers() & Qt::ControlModifier)) {
                m_selected.clear();
                m_secondarySelected.clear();
                refreshView();
                emit selectionChanged(QString());
            }
        } else {
            selectInMarquee(e->modifiers() & Qt::ControlModifier);
        }
        m_marqueeRect = QRect();
        break;
    case ZoomSelect:
        if (std::fabs(m_zoomT1 - m_zoomT0) < 0.02)
            zoomBy(1.6, m_zoomT0);
        else
            applyZoomRect(m_zoomT0, m_zoomT1);
        break;
    case TrackDrag:
        finishTrackDrag();
        break;
    case TrackOp:
        setCursor(Qt::ArrowCursor);
        break;
    default:
        break;
    }
    m_dragMode = None;
    m_dragClip.clear();
    m_dragUndoPushed = false;
    m_dragOrig.clear();
    m_volRow = -1;
    m_volClip.clear();
    m_volPending = false;
    m_trackDragActive = false;
    m_dragTrackRow = -1;
    m_dragGroupId.clear();
    m_dropRow = -1;
    m_dropGroup.clear();
    stopAutoScroll();
    setCursor(Qt::ArrowCursor);
    update();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!m_project) return;
    // Duplo clique na faixa de uma pasta (fora da seta) renomeia a pasta.
    if (e->pos().x() < kHeaderW) {
        QString gid;
        if (folderStripAt(e->pos().y(), gid)) {
            TrackGroup* g = m_project->findGroup(gid);
            const QRect ar = g ? folderArrowRect(*g) : QRect();
            if (g && !(e->pos().x() >= ar.left() && e->pos().x() <= ar.right())) {
                renameTrackGroup(gid);
                return;
            }
        }
    }
    // Duplo clique na agulha, ou na região do loop (na régua), limpa o loop.
    if (m_loopOut > m_loopIn) {
        const double px = timeToX(m_playhead);
        const double t = xToTime(e->pos().x());
        if (std::fabs(e->pos().x() - px) <= 6
            || (e->pos().y() < kRulerH && t >= m_loopIn && t <= m_loopOut)) {
            clearLoop();
            return;
        }
    }
    int row;
    bool audio;
    // Duplo clique no nome da faixa (topo do cabeçalho): renomeia a faixa.
    if (e->pos().x() < kHeaderW && e->pos().y() >= kRulerH
        && rowFromY(e->pos().y(), row, audio)) {
        const int y = audio ? rowY(-1, row) : rowY(row, -1);
        if (e->pos().y() >= y + 1 && e->pos().y() <= y + 20) {
            Track* trk = audio ? &m_project->audioTracks[row]
                               : &m_project->videoTracks[row];
            bool ok = false;
            const QString name = QInputDialog::getText(
                this, tr("Renomear faixa"), tr("Nome da faixa:"),
                QLineEdit::Normal, trk->name, &ok);
            if (ok && !name.trimmed().isEmpty()) {
                emit editStart();
                trk->name = name.trimmed();
                emit modified();
                update();
            }
            return;
        }
    }
    if (rowFromY(e->pos().y(), row, audio)) {
        Clip* clip = clipAt(row, audio, xToTime(e->pos().x()));
        if (clip) {
            // Duplo clique num clipe com texto (ou num clipe de texto) abre a
            // mesma janela usada para criar — vira a janela de edição do clipe.
            if (!audio && (clip->isText || !m_project->textStyleFor(*clip)->isEmpty())) {
                showTextEditorDialog(clip);
                return;
            }
            toggleSelection(clip->id);
            update();
        }
    }
}

// ── Drag & Drop (eventos Qt) ───────────────────────────────────────────

void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QLatin1String(kMimeMedia))
        || md->hasFormat(QLatin1String(kMimeEffect))
        || md->hasUrls()) {
        e->acceptProposedAction();
        e->accept();
    }
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* e) {
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QLatin1String(kMimeMedia))
        || md->hasFormat(QLatin1String(kMimeEffect))
        || md->hasUrls()) {
        int row = -1;
        bool audio = false;
        QString gid;
        if (folderStripAt(e->position().toPoint().y(), gid)) {
            m_dragHoverRow = -1;
        } else if (rowFromY(e->position().toPoint().y(), row, audio)) {
            m_dragHoverRow = row;
            m_dragHoverAudio = audio;
        } else {
            m_dragHoverRow = -1;
        }
        // Prévia (estilo Premiere): duração representativa da mídia arrastada
        // para desenhar a "fantasma" do clipe no tamanho certo.
        m_dragHoverT = snapTime(std::max(0.0, xToTime(e->position().toPoint().x())));
        m_dragHoverDur = 0.0;
        m_dragHoverName.clear();
        if (m_project && md->hasFormat(QLatin1String(kMimeMedia))) {
            const QByteArray data = md->data(QLatin1String(kMimeMedia));
            for (QByteArray line : data.split('\n')) {
                line = line.trimmed();
                if (line.isEmpty()) continue;
                const MediaItem* m = m_project->findMedia(QString::fromLatin1(line));
                if (m && (m->hasVideo || m->hasAudio)) {
                    m_dragHoverDur = mediaInsertDur(*m);
                    m_dragHoverName = m->name;
                    break;
                }
            }
        } else if (md->hasUrls()) {
            for (const QUrl& u : md->urls()) {
                if (!u.isLocalFile()) continue;
                const FFmpegMediaInfo info = FFmpegDecoder::probe(u.toLocalFile());
                if (info.hasVideo || info.hasAudio) {
                    m_dragHoverDur = info.duration > 0 ? info.duration : 1.0;
                    m_dragHoverName = QFileInfo(u.toLocalFile()).completeBaseName();
                    break;
                }
            }
        }
        if (m_dragHoverDur <= 0.0) m_dragHoverDur = 1.0;
        update();
        e->acceptProposedAction();
        e->accept();
    }
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) {
    m_dragHoverRow = -1;
    m_dragHoverDur = 0.0;
    m_dragHoverName.clear();
    update();
}

void TimelineWidget::dropEvent(QDropEvent* e) {
    if (!m_project) return;
    const QMimeData* md = e->mimeData();

    // ── Arrasto de efeito (do painel de efeitos) ─────────────────────────
    if (md->hasFormat(QLatin1String(kMimeEffect))) {
        const QByteArray effectData = md->data(QLatin1String(kMimeEffect));
        const QString effectId = QString::fromUtf8(effectData);
        if (effectId.isEmpty()) { e->ignore(); return; }

        // Encontra o clipe sob o cursor.
        int row = -1;
        bool audio = false;
        if (!rowFromY(e->position().toPoint().y(), row, audio) || audio) {
            e->ignore();
            return;
        }
        const double t = xToTime(e->position().toPoint().x());
        Clip* target = clipAt(row, false, t);
        if (!target) { e->ignore(); return; }

        emit editStart();

        if (effectId == "pierrot_lainka") {
            target->lainkaEnabled = true;
            target->lainkaTargetFps = 6;
            target->lainkaJitterPos = 10.0;
            target->lainkaFlicker = 8.0;
            target->lainkaWarpAmount = 8.0;
            target->lainkaDustAmount = 0.0;
            target->lainkaScratchAmount = 0.0;
            target->lainkaOpacity = 100.0;
        } else if (effectId == "pierrot_motion") {
            target->motionEnabled = true;
            target->motionAmount = 25.0;
        } else if (effectId == "pierrot_brightness") {
            target->brightness = 0.2; // valor inicial ao arrastar
        } else if (effectId == "pierrot_contrast") {
            target->contrast = 1.2;
        } else if (effectId == "pierrot_saturation") {
            target->saturation = 1.2;
        } else if (effectId == "pierrot_blur") {
            target->blur = 5.0;
        } else if (effectId == "pierrot_grayscale") {
            target->grayscale = true;
        } else if (effectId == "pierrot_chromakey") {
            target->chromaKey = true;
        } else if (effectId == "pierrot_audio_eq") {
            // EQ Express: só aplica o preset inicial se o EQ ainda estiver neutro.
            if (std::fabs(target->eqLow) <= 0.01 && std::fabs(target->eqMid) <= 0.01
                && std::fabs(target->eqHigh) <= 0.01) {
                target->eqLow = 0.0;
                target->eqMid = 1.5;
                target->eqHigh = 1.0;
            }
        } else if (effectId == "pierrot_audio_reverb") {
            target->reverb = true;
            target->reverbMix = 0.35;
            target->reverbSize = 0.5;
        } else {
            // Efeito OFX: adiciona ao stack ofxFx do clipe.
            bool already = false;
            for (const OfxPluginInstance& fx : target->ofxFx)
                if (fx.pluginId == effectId) { already = true; break; }
            if (!already) {
                OfxPluginInstance fx;
                fx.pluginId = effectId;
                fx.enabled = true;
                target->ofxFx.append(fx);
            }
        }

        invalidateScene();
        update();
        emit modified();
        e->acceptProposedAction();
        e->accept();
        return;
    }

    // ── Arrasto de mídia (existente) ─────────────────────────────────────
    QStringList mediaIds;
    if (md->hasFormat(QLatin1String(kMimeMedia))) {
        // Arrasto vindo da lista de mídia do aplicativo (caminho clássico de
        // DnD; a pool também chama finishDrop() direto no arrasto manual).
        const QByteArray data = md->data(QLatin1String(kMimeMedia));
        for (QByteArray line : data.split('\n')) {
            line = line.trimmed();
            if (!line.isEmpty()) mediaIds.append(QString::fromUtf8(line));
        }
    } else if (md->hasUrls()) {
        // Arrasto de arquivos vindos do gerenciador de arquivos: importa para
        // a mídia do projeto e já coloca clipes no ponto solto.
        int added = 0;
        QStringList imported;
        for (const QUrl& u : md->urls()) {
            if (!u.isLocalFile()) continue;
            const QString path = u.toLocalFile();
            const FFmpegMediaInfo info = FFmpegDecoder::probe(path);
            if (!info.hasVideo && !info.hasAudio) continue;
            MediaItem m;
            m.id = newId();
            m.filePath = path;
            m.name = QFileInfo(path).completeBaseName();
            m.duration = info.duration;
            m.width = info.width;
            m.height = info.height;
            m.hasVideo = info.hasVideo;
            m.hasAudio = info.hasAudio;
            m.audioStreams = info.audioStreams;
            m.audioChannels = info.audioChannels;
            m_project->media.append(m);
            mediaIds.append(m.id);
            imported.append(path);
            ++added;
        }
        if (added == 0) return;
        SettingsDialog::warnMkvIfNeeded(this, imported);
        emit mediaImported(); // atualiza a lista de mídia e marca o projeto
    } else {
        return;
    }
    if (mediaIds.isEmpty()) return;

    finishDrop(mediaIds, e->position().toPoint());
}

// ── Drag & Drop (arrasto manual da pool) ────────────────────────────────

void TimelineWidget::showDropHover(const QPoint& globalPos) {
    const QPoint pos = mapFromGlobal(globalPos);
    int row = -1;
    bool audio = false;
    m_dragHoverRow = rowFromY(pos.y(), row, audio) ? row : -1;
    m_dragHoverAudio = audio;
    // Arrasto manual da pool (sem dragMoveEvent): atualiza o instante e usa a
    // duração já conhecida (ou um padrão) para a prévia do clipe.
    m_dragHoverT = snapTime(std::max(0.0, xToTime(pos.x())));
    if (m_dragHoverDur <= 0.0) m_dragHoverDur = 1.0;
    update();
}

void TimelineWidget::hideDropHover() {
    m_dragHoverRow = -1;
    m_dragHoverDur = 0.0;
    m_dragHoverName.clear();
    update();
}

void TimelineWidget::dropMediaAt(const QStringList& mediaIds, const QPoint& globalPos) {
    finishDrop(mediaIds, mapFromGlobal(globalPos));
}

void TimelineWidget::finishDrop(const QStringList& mediaIds, const QPoint& dropPos) {
    if (!m_project || mediaIds.isEmpty()) return;

    // Trimmer (estilo Vegas): ao soltar UMA mídia de arquivo com vídeo, oferece
    // definir in/out antes de inserir. Cancelar aborta a soltura; multi-seleção
    // insere direto (sem diálogo). Ativável nas configurações (padrão: sem trimmer).
    double trimIn = 0.0;
    double trimDur = -1.0;
    if (mediaIds.size() == 1 && SettingsDialog::trimmerEnabled()) {
        const MediaItem* m = m_project->findMedia(mediaIds.first());
        if (m && m->hasVideo && !m->filePath.isEmpty() && m->duration > 0.5) {
            TrimmerDialog dlg(*m, this);
            if (dlg.exec() != QDialog::Accepted) return;
            trimIn = dlg.trimIn();
            const double out = dlg.trimOut();
            trimDur = out - trimIn;
            if (trimDur <= 0.0) return;
        }
    }

    int row = -1;
    bool audio = false;
    if (!rowFromY(dropPos.y(), row, audio)) {
        // Solta fora de uma faixa (régua, rodapé ou coluna de cabeçalho):
        // usa a primeira faixa desbloqueada compatível com a mídia.
        bool anyVideo = false;
        for (const QString& mid : mediaIds) {
            const MediaItem* m = m_project->findMedia(mid);
            if (m && m->hasVideo) { anyVideo = true; break; }
        }
        audio = !anyVideo;
        row = 0;
    }
    m_dragHoverRow = -1;
    m_dragHoverDur = 0.0;
    m_dragHoverName.clear();

    emit editStart();
    double t = snapTime(std::max(0.0, xToTime(dropPos.x())));
    QString lastPlaced;

    // Acha a faixa (do tipo `audio`) que esteja livre para receber um clipe de
    // duração `dur` em `t`. Prefere `preferRow` (a faixa sob o mouse); se
    // ocupada, procura a primeira faixa desbloqueada que não sobreponha; se
    // nenhuma existir, cria uma nova faixa vazia.
    auto findFreeTrack = [this](bool audio, double t, double dur, int preferRow) {
        auto overlap = [t, dur](const QVector<Clip>& clips) {
            for (const Clip& o : clips)
                if (o.pos < t + dur - 1e-9 && o.pos + o.dur > t + 1e-9) return true;
            return false;
        };
        auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
        if (preferRow >= 0 && preferRow < (int)list.size()
            && !list[preferRow].locked && !overlap(list[preferRow].clips))
            return preferRow;
        for (int i = 0; i < (int)list.size(); ++i)
            if (!list[i].locked && !overlap(list[i].clips)) return i;
        m_project->addTrack(audio);
        return (int)list.size() - 1;
    };

    for (const QString& mid : mediaIds) {
        const MediaItem* m = m_project->findMedia(mid);
        if (!m) continue;
        const bool both = m->hasVideo && m->hasAudio;
        // Arquivos multicanal (OBS/câmera): um clipe de áudio POR stream, cada
        // um na sua faixa. O número de streams pode ser 0 mesmo com hasAudio.
        const int aStreams = m->hasAudio ? qMax(1, m->audioStreams) : 0;
        const double dur = trimDur > 0.0 ? trimDur : mediaInsertDur(*m);

        // Vídeo primeiro: decide a faixa de vídeo e a posição.
        int vRow = -1;
        double vDur = 0.0;
        if (m->hasVideo) {
            vRow = findFreeTrack(false, t, dur, audio ? -1 : row);
            vDur = dur;
        }

        if (vRow < 0 && aStreams == 0) continue;

        // Grupo: vídeo+áudio vinculados OU áudio multicanal (várias faixas do
        // mesmo arquivo) — movem e são excluídos juntos.
        const QString gid = (both || aStreams > 1) ? newId() : QString();
        if (vRow >= 0) {
            Clip c;
            c.id = newId();
            c.groupId = gid;
            c.mediaId = mid;
            c.pos = t;
            c.in = trimIn;
            c.dur = vDur;
            c.name = m->name;
            auto& clips = m_project->videoTracks[vRow].clips;
            auto it = clips.begin();
            while (it != clips.end() && it->pos <= c.pos) ++it;
            clips.insert(it, c);
            lastPlaced = c.id;
        }
        for (int k = 0; k < aStreams; ++k) {
            const int aRow = findFreeTrack(true, t, dur, (k == 0 && audio) ? row : -1);
            if (aRow < 0) continue;
            Clip c;
            c.id = newId();
            c.groupId = gid;
            c.mediaId = mid;
            c.audioStreamIndex = k;
            c.pos = t;
            c.in = trimIn;
            c.dur = dur;
            c.name = aStreams > 1 ? QString("%1 (faixa %2)").arg(m->name).arg(k + 1)
                                  : m->name;
            auto& clips = m_project->audioTracks[aRow].clips;
            auto it = clips.begin();
            while (it != clips.end() && it->pos <= c.pos) ++it;
            clips.insert(it, c);
            if (lastPlaced.isEmpty()) lastPlaced = c.id;
        }
        t += dur;
    }
    if (!lastPlaced.isEmpty()) setSelection(lastPlaced);
    updateScrollRanges();
    update();
    emit modified();
}

// ── Reordenação de faixas (arrasto de header) ──────────────────────────

void TimelineWidget::finishTrackDrag() {
    if (!m_project) return;
    // Arrasto de GRUPO (pela faixa de pasta): move todas as faixas do grupo.
    if (!m_dragGroupId.isEmpty()) {
        TrackGroup* g = m_project->findGroup(m_dragGroupId);
        if (g && m_dropRow >= 0) {
            QVector<int> vidx, aidx;
            for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
                if (m_project->videoTracks[i].groupId == g->id) vidx.append(i);
            for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
                if (m_project->audioTracks[i].groupId == g->id) aidx.append(i);
            emit editStart();
            if (m_dropAudio) moveTracksTo(true, aidx, m_dropRow);
            else moveTracksTo(false, vidx, m_dropRow);
            pruneEmptyGroups();
            updateScrollRanges();
            invalidateScene();
            emit modified();
        }
        m_dragGroupId.clear();
        m_dragTrackRow = -1;
        m_dropRow = -1;
        m_dropGroup.clear();
        return;
    }

    if (m_dragTrackRow < 0) return;
    const int from = m_dragTrackRow;
    const bool audio = m_dragTrackAudio;
    if (!m_dropGroup.isEmpty()) {
        // Soltou sobre uma pasta: entra no grupo.
        if (m_project->findGroup(m_dropGroup)) {
            emit editStart();
            auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
            if (from >= 0 && from < (int)list.size())
                list[from].groupId = m_dropGroup;
            pruneEmptyGroups();
            updateScrollRanges();
            invalidateScene();
            emit modified();
        }
        m_dropGroup.clear();
        m_dragTrackRow = -1;
        return;
    }
    // Soltou sobre uma faixa do mesmo tipo: reordena. Uma faixa de grupo, ao
    // ser movida para outro lugar, sai do grupo (fica por cima/abaixo da
    // hierarquia de pastas).
    if (m_dropRow >= 0 && m_dropAudio == audio && m_dropRow != from) {
        emit editStart();
        auto& list = audio ? m_project->audioTracks : m_project->videoTracks;
        if (from >= 0 && from < (int)list.size())
            list[from].groupId.clear();
        moveTrack(audio, from, m_dropRow);
        pruneEmptyGroups();
        updateScrollRanges();
        invalidateScene();
        emit modified();
    }
    m_dragTrackRow = -1;
}

// ── Snap e posicionamento ───────────────────────────────────────────────

double TimelineWidget::snapToEdges(double t, const QString& excludeId) const {
    if (!m_snap) return t;
    const double thresh = 8.0 / m_pps;
    double best = t;
    double bestD = thresh;
    auto consider = [&](double x) {
        const double d = std::fabs(x - t);
        if (d < bestD) { bestD = d; best = x; }
    };
    consider(0.0);
    consider(m_playhead);
    auto considerClip = [&](const Clip& c) {
        if (c.id == excludeId) return;
        consider(c.pos);
        consider(c.pos + c.dur);
    };
    if (m_project) {
        for (const Track& tr : m_project->videoTracks)
            for (const Clip& c : tr.clips)
                considerClip(c);
        for (const Track& tr : m_project->audioTracks)
            for (const Clip& c : tr.clips)
                considerClip(c);
    }
    return best;
}

double TimelineWidget::clampPosToTrack(Clip* c, double newPos,
                                       const QSet<QString>& moving) const {
    if (!m_project || !c) return newPos;
    Q_UNUSED(moving)
    // Clipes podem se sobrepor na mesma faixa: a sobreposição vira uma
    // transição (estilo Vegas). Só garante que não vá para o negativo.
    return std::max(0.0, newPos);
}

bool TimelineWidget::clipTrackIndex(const QString& id, int& row, bool& audio) const {
    if (!m_project) return false;
    for (int i = 0; i < (int)m_project->videoTracks.size(); ++i)
        for (const Clip& c : m_project->videoTracks[i].clips)
            if (c.id == id) { row = i; audio = false; return true; }
    for (int i = 0; i < (int)m_project->audioTracks.size(); ++i)
        for (const Clip& c : m_project->audioTracks[i].clips)
            if (c.id == id) { row = i; audio = true; return true; }
    return false;
}
