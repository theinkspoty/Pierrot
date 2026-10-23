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

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!m_project) return;

    m_mousePos = e->pos();
    m_mouseOnRuler = (e->pos().y() < kRulerH);
    // A guia de alinhamento é recalculada só nos ramos de arraste que a usam
    // (MoveClip/Trim). Limpando aqui, ela não sobrevive a um mousemove que
    // sai das faixas — antes ficava congelada no último ponto de snap.
    m_snapLineX = -1.0;
    m_trimEdgeX = -1.0;
    m_trimEdgeRow = -1;

    // Ctrl+press num clipe: superou o limite de movimento => duplica e
    // passa a arrastar a cópia (estilo Vegas). Ficou no clique => o release
    // alterna a seleção.
    if (m_ctrlPending) {
        if (m_ctrlCanMove && (e->pos() - m_dragStart).manhattanLength() >= 4) {
            m_ctrlPending = false;
            Clip* src = findClipById(m_ctrlClipId);
            if (!src) { update(); return; }
            duplicateClip(src); // emite editStart e seleciona a cópia
            Clip* copy = m_selected.isEmpty()
                ? nullptr : findClipById(m_selected.first());
            if (!copy) { update(); return; }
            m_dragOrig.clear();
            auto addOrig = [this](Clip* c) {
                if (!c) return;
                if (!c->groupId.isEmpty()) {
                    for (Clip* m : groupMembers(c->groupId))
                        m_dragOrig.insert(m->id, {m->pos, m->in, m->dur});
                } else {
                    m_dragOrig.insert(c->id, {c->pos, c->in, c->dur});
                }
            };
            addOrig(copy);
            // A cópia acompanha o mouse: o shift de duração aplicado por
            // duplicateClip é descontado e o grupo duplicado anda com o cursor.
            const double offSec = xToTime(e->pos().x()) - xToTime(m_dragStart.x());
            for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                Clip* sc = findClipById(it.key());
                if (sc) sc->pos = snapTime(std::max(0.0, sc->pos + offSec));
            }
            // Re-registra as posições já deslocadas e continua o arraste daqui.
            m_dragOrig.clear();
            addOrig(copy);
            m_dragMode = MoveClip;
            m_dragClip = copy->id;
            m_dragUndoPushed = true; // duplicateClip já abriu o undo
            m_dragStart = e->pos();
            m_dragOrigPos = copy->pos;
            m_dragOrigIn = copy->in;
            m_dragOrigDur = copy->dur;
        }
        update();
        return;
    }

    // Destaque das alças (opacidade no centro, fades nos cantos) ao passar o
    // mouse sobre o topo de um clipe de vídeo. Repinta só quando o alvo muda.
    // Ignorado durante arrastos. Throttle: só recalcula a cada 4px de movimento.
    QString newCorner;
    int newSide = 0;
    if (m_dragMode == None && !(e->buttons() & Qt::LeftButton)
        && e->pos().y() >= kRulerH) {
        const int hoverDx = e->pos().x() - m_lastHoverPos.x();
        const int hoverDy = e->pos().y() - m_lastHoverPos.y();
        if (std::abs(hoverDx) >= 4 || std::abs(hoverDy) >= 4
            || m_hoverCornerClip.isEmpty() != m_lastHoverClipId.isEmpty()) {
            m_lastHoverPos = e->pos();
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
                    }
                }
            }
            if (newCorner != m_hoverCornerClip
                || newSide != m_hoverCornerSide) {
                m_hoverCornerClip = newCorner;
                m_hoverCornerSide = newSide;
                update();
            }
        }
    }

    // Tooltip dB: sobre a linha de volume/envelope de uma faixa de áudio,
    // mostra o valor em dB sob o cursor. Throttle: só recalcula se o mouse
    // mudou de faixa ou moveu >= 4px. Como a linha da faixa é sempre
    // Tooltip dB: só aparece quando a linha está visível (Shift+V) e o
    // cursor está sobre ela.
    if (m_dragMode == None && !(e->buttons() & Qt::LeftButton) && m_showVolLines) {
        const QPoint& at = e->pos();
        int vrow;
        bool overVol = at.x() >= kHeaderW && at.y() >= kRulerH
            && volRowAt(at, vrow) >= 0;
        if (overVol) {
            int r2;
            bool a2;
            const bool overClip = rowFromY(at.y(), r2, a2)
                && clipAt(r2, a2, xToTime(at.x())) != nullptr;
            if (!overClip) {
                const Track& ctr = m_project->audioTracks[vrow];
                overVol = std::abs(at.y() - trackVolLineYAt(vrow,
                                  kfValue(ctr.kfVolume, ctr.volume,
                                          xToTime(at.x())))) <= 6;
            }
        }
        const int tipDx = at.x() - m_lastHoverPos.x();
        const int tipDy = at.y() - m_lastHoverPos.y();
        if (overVol && (std::abs(tipDx) >= 4 || std::abs(tipDy) >= 4
                        || vrow != m_lastHoverRow)) {
            m_lastHoverRow = vrow;
            QString tip;
            int r2;
            bool a2;
            const bool overClip = rowFromY(at.y(), r2, a2)
                && clipAt(r2, a2, xToTime(at.x())) != nullptr;
            if (!overClip) {
                const Track& tr = m_project->audioTracks[vrow];
                double val = kfValue(tr.kfVolume, tr.volume, xToTime(at.x()));
                int erow;
                bool eaudio;
                const int kfi = trackEnvKfAt(at, erow, eaudio);
                if (kfi >= 0 && eaudio) {
                    const QVector<Keyframe>& keys =
                        m_project->audioTracks[erow].kfVolume;
                    if (kfi < keys.size()) val = keys[kfi].value;
                }
                const double db = 20.0 * std::log10(std::max(val, 1e-4));
                tip = QStringLiteral("Volume: %1 dB").arg(db, 0, 'f', 1);
            }
            if (tip != m_lastVolTip) {
                m_lastVolTip = tip;
                if (tip.isEmpty())
                    QToolTip::hideText();
                else
                    QToolTip::showText(mapToGlobal(at), tip, this);
            }
        } else if (!overVol && !m_lastVolTip.isEmpty()) {
            m_lastVolTip.clear();
            QToolTip::hideText();
        }
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
                    m_geometryChangedDuringDrag = true;
                    emit clipGeometryChanged();
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
                    m_geometryChangedDuringDrag = true;
                    emit clipGeometryChanged();
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
                    m_geometryChangedDuringDrag = true;
                    emit clipGeometryChanged();
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
                m_geometryChangedDuringDrag = true;
                emit clipGeometryChanged();
            }
            return;
        }
        if (m_dragMode == TrackEnvVol) {
            if (m_envRow < 0 || m_envKf < 0 || !m_envAudio) return;
            Track& t = m_project->audioTracks[m_envRow];
            if (m_envKf >= t.kfVolume.size()) return;
            const int rowH = trackH(m_envRow, true);
            const int y = rowY(-1, m_envRow);
            const int pad = 6;
            const double frac = 1.0 - std::clamp(
                (double)(e->pos().y() - (y + pad)) / (rowH - pad * 2.0), 0.0, 1.0);
            const double v = std::clamp(frac * 2.0, 0.0, 2.0);
            if (std::fabs(t.kfVolume[m_envKf].value - v) > 1e-4) {
                t.kfVolume[m_envKf].value = v;
                refreshView();
                update();
                m_geometryChangedDuringDrag = true;
                emit clipGeometryChanged();
            }
            return;
        }
        if (m_dragMode == PlayheadDrag) {
            const double t2 = std::max(0.0, snapTime(xToTime(e->pos().x())));
            setPlayhead(t2);
            // Throttle: a agulha segue o mouse de imediato; o aviso que manda o
            // preview decodificar é coalescido (~30 Hz) para o mouseMove não
            // ficar preso no updateFrame() da UI thread.
            emitPlayheadThrottled(t2);
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
            // Press na faixa de áudio (na linha do envelope ou no vazio):
            // vira ajuste de volume se o arraste for predominantemente
            // vertical (para cima/baixo).
            if (m_volPending || m_envPending) {
                const int dy = e->pos().y() - m_dragStart.y();
                const int dx = e->pos().x() - m_dragStart.x();
                if (std::abs(dy) > std::abs(dx) + 4 && std::abs(dy) >= 5) {
                    if (m_volRow >= 0 && m_volRow < (int)m_project->audioTracks.size()) {
                        emit editStart();
                        m_dragMode = TrackVol;
                        m_volPending = false;
                        m_envPending = false;
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
                        m_geometryChangedDuringDrag = true;
                        emit clipGeometryChanged();
                        return;
                    }
                }
            }
            // Recorta o marquee à área de conteúdo (fora do header e da
            // régua). Sem isso, o selectInMarquee usava o rect bruto e
            // selecionava clipes com t0 < m_viewStart (xToTime no header)
            // quando o arraste "subia" para o cabeçalho — pior com a
            // timeline scrolled para o meio (header logo abaixo da régua).
            m_marqueeRect = QRect(m_dragStart, e->pos()).normalized()
                                .intersected(QRect(kHeaderW, kRulerH,
                                                   qMax(1, width() - kHeaderW),
                                                   qMax(1, height() - kRulerH)));
            update();
            return;
        }
        if (m_dragMode != None && !m_dragClip.isEmpty()) {
            Clip* clip = findClipById(m_dragClip);
            if (clip) {
                const double dt = (e->pos().x() - m_dragStart.x()) / m_pps;
                if (!m_dragUndoPushed && std::fabs(dt) > 1e-9) {
                    emit editStart();
                    m_dragUndoPushed = true;
                }
                if (m_dragMode == MoveClip) {
                    // Troca de faixa durante o arraste: cada clipe segue a
                    // linha sob o mouse quando o tipo bate (vídeo só em faixas
                    // de vídeo, áudio só em faixas de áudio). O membro do
                    // grupo vinculado NÃO acompanha na vertical: ele mantém a
                    // faixa em que está (só a posição no tempo anda junto).
                    int row;
                    bool audio;
                    // Nota: os hit-tests de volume (clipVolAt/volRowAt) ficavam
                    // aqui dentro, sob `if (m_dragMode == MoveClip)`, e o clique
                    // de volume era ignorado por causa disso — o `return` deles
                    // congelava o arraste quando o cursor cruzava uma linha de
                    // volume. O bloco foi removido: durante um arraste ativo o
                    // gesto já foi capturado, então nenhum hit-test deve
                    // interromper o mouseMove. As funções são const, logo não
                    // tinham efeito colateral além do return.
                    if (!m_rippleMove && rowFromY(e->pos().y(), row, audio)) {
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
                    // Conjunto de clipes que se movem neste arraste (grupo +
                    // vizinhos do ripple). Excluído do snap: um alvo que se move
                    // junto faz o encaixe oscilar (o clipe "gruda" e treme).
                    QSet<QString> moving;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it)
                        moving.insert(it.key());
                    if (m_rippleMove)
                        for (auto it = m_rippleMoveOrig.constBegin();
                             it != m_rippleMoveOrig.constEnd(); ++it)
                            moving.insert(it.key());
                    const double raw = m_dragOrigPos + dt;
                    const double grid = snapTime(raw);
                    const double snapped = snapToEdges(grid, moving);
                    // Guia de alinhamento: só marca snap de BORDA. Comparar com
                    // `grid` e não com `raw` é o que importa — `snapTime()`
                    // quantiza para a grade de frames quase sempre, então
                    // comparar com `raw` acendia a guia em quase todo mousemove.
                    if (std::fabs(snapped - grid) > 1e-6)
                        m_snapLineX = timeToX(snapped);
                    else
                        m_snapLineX = -1.0;
                    const double delta = snapped - m_dragOrigPos;
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc)
                            sc->pos = clampPosToTrack(sc,
                                                      std::max(0.0, it.value().pos + delta),
                                                      moving);
                    }
                    // Ripple MOVE: os clipes à frente acompanham o arraste,
                    // mantendo a distância relativa ao clipe arrastado. Usa o
                    // delta REALMENTE aplicado (o clipe sob o mouse pode ter
                    // sido limitado por um clipe anterior), senão os seguintes
                    // andariam mais que ele e se sobreporiam.
                    if (m_rippleMove) {
                        double appliedDelta = delta;
                        if (Clip* dc0 = findClipById(m_dragClip))
                            appliedDelta = dc0->pos - m_dragOrigPos;
                        for (auto it = m_rippleMoveOrig.constBegin();
                             it != m_rippleMoveOrig.constEnd(); ++it) {
                            Clip* sc = findClipById(it.key());
                            if (sc) sc->pos = std::max(0.0, it.value() + appliedDelta);
                        }
                    }
                } else if (m_dragMode == TrimLeft && m_tool == ToolRipple) {
                    // Ripple Edit: trim esquerdo com ripple (desloca subsequentes).
                    // Precisa vir ANTES do TrimLeft genérico: o ripple também
                    // usa TrimLeft/TrimRight como m_dragMode, então depois do
                    // ramo genérico este teste nunca era alcançado.
                    // Restaura a faixa antes de aplicar — rippleTrimLeft mede
                    // o delta contra o estado corrente, então sem restaurar o
                    // arraste congelaria no primeiro mousemove.
                    for (auto it = m_rippleOrig.begin(); it != m_rippleOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc) {
                            sc->pos = it.value().pos;
                            sc->in = it.value().in;
                            sc->dur = it.value().dur;
                        }
                    }
                    // Snap ignora a faixa inteira do ripple (o próprio clipe e os
                    // vizinhos que serão deslocados): alvo móvel faz oscilar.
                    QSet<QString> ripIds;
                    for (auto it = m_rippleOrig.constBegin(); it != m_rippleOrig.constEnd(); ++it)
                        ripIds.insert(it.key());
                    const double minEnd = std::max(0.0, m_dragOrigPos + m_dragOrigDur - kMinDur);
                    const double np = std::clamp(snapToEdges(snapTime(m_dragOrigPos + dt), ripIds),
                                                 0.0, minEnd);
                    const double delta = np - m_dragOrigPos;
                    m_snapLineX = -1.0;
                    markTrimEdge(np, /*ripple=*/true);
                    rippleTrimLeft(clip, m_dragOrigIn + delta);
                } else if (m_dragMode == TrimRight && m_tool == ToolRipple) {
                    // Ripple Edit: trim direito com ripple (desloca subsequentes)
                    for (auto it = m_rippleOrig.begin(); it != m_rippleOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc) {
                            sc->pos = it.value().pos;
                            sc->in = it.value().in;
                            sc->dur = it.value().dur;
                        }
                    }
                    QSet<QString> ripIds;
                    for (auto it = m_rippleOrig.constBegin(); it != m_rippleOrig.constEnd(); ++it)
                        ripIds.insert(it.key());
                    const double end = snapToEdges(snapTime(m_dragOrigPos + m_dragOrigDur + dt), ripIds);
                    const double newDur = std::max(kMinDur, end - clip->pos);
                    m_snapLineX = -1.0;
                    markTrimEdge(end, /*ripple=*/true);
                    rippleTrimRight(clip, newDur);
                } else if (m_dragMode == TrimLeft) {
                    const double minEnd = std::max(0.0, m_dragOrigPos + m_dragOrigDur - kMinDur);
                    const double rawLeft = m_dragOrigPos + dt;
                    const double gridLeft = snapTime(rawLeft);
                    const double snappedLeft = snapToEdges(gridLeft);
                    const double np = std::clamp(snappedLeft, 0.0, minEnd);
                    // Compara com gridLeft (pós-quantização, pré-clamp): usar o
                    // np já clampado acendia a guia no limite de duração,
                    // onde não houve snap nenhum.
                    if (std::fabs(snappedLeft - gridLeft) > 1e-6)
                        m_snapLineX = timeToX(snappedLeft);
                    else
                        m_snapLineX = -1.0;
                    const double delta = np - m_dragOrigPos;
                    markTrimEdge(np, /*ripple=*/false);
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (!sc) continue;
                        const ClipOrig& o = it.value();
                        sc->pos = std::max(0.0, o.pos + delta);
                        sc->in = o.in + delta;
                        sc->dur = o.dur - delta;
                    }
                } else if (m_dragMode == TrimRight) {
                    const double rawEnd = m_dragOrigPos + m_dragOrigDur + dt;
                    const double gridEnd = snapTime(rawEnd);
                    const double end = snapToEdges(gridEnd);
                    if (std::fabs(end - gridEnd) > 1e-6)
                        m_snapLineX = timeToX(end);
                    else
                        m_snapLineX = -1.0;
                    const double newDur = std::max(kMinDur, end - clip->pos);
                    const double deltaDur = newDur - m_dragOrigDur;
                    markTrimEdge(end, /*ripple=*/false);
                    for (auto it = m_dragOrig.begin(); it != m_dragOrig.end(); ++it) {
                        Clip* sc = findClipById(it.key());
                        if (sc) sc->dur = std::max(kMinDur, it.value().dur + deltaDur);
                    }
                } else if (m_dragMode == RollingEdit) {
                    // Rolling Edit: ajustar fronteira entre 2 clipes
                    const double delta = (e->pos().x() - m_dragStart.x()) / m_pps;
                    rollingEdit(delta);
                    // No roll as duas bordas andam juntas, então o indicador
                    // fica na emenda — Premiere marca essa emenda em vermelho.
                    // rollingEdit() já reposicionou clipB: a emenda é a borda
                    // direita de clipA.
                    if (Clip* clipA = findClipById(m_rollClipA))
                        markTrimEdge(clipA->pos + clipA->dur, /*ripple=*/false);
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
                        invalidateSceneContent();
                    }
                }
                // A duração do projeto MUDA ao mover/trimar (é o máx de pos+dur),
                // então a barra horizontal precisa acompanhar o mouse. Só a
                // alarga — não recalcula o range vertical a cada mousemove.
                ensureScrollRangeReaches(m_project->duration());
                update();
                m_geometryChangedDuringDrag = true;
                emit clipGeometryChanged();
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


