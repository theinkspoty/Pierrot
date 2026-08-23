// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "TimelineCommands.h"
#include "TimelineWidget.h"
#include <algorithm>
#include <cmath>

// Constantes auxiliares
static constexpr double kMinDur = 0.04;

// ── Funções auxiliares internas ────────────────────────────────────────

// Verifica se a faixa está travada.
static bool trackLocked(const TimelineWidget* tl, const Clip* c) {
    // Delega para o método público (ou acessa via friend)
    // Por simplicidade, acessamos o projeto diretamente.
    if (!tl || !c || !tl->project()) return true;
    const Project* p = tl->project();
    for (const Track& t : p->videoTracks)
        for (const Clip& x : t.clips)
            if (&x == c) return t.locked;
    for (const Track& t : p->audioTracks)
        for (const Clip& x : t.clips)
            if (&x == c) return t.locked;
    return true;
}

// Encontra clipe por ID.
static Clip* findClipById(TimelineWidget* tl, const QString& id) {
    if (!tl || !tl->project()) return nullptr;
    Project* p = tl->project();
    for (Track& t : p->videoTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    for (Track& t : p->audioTracks)
        for (Clip& c : t.clips)
            if (c.id == id) return &c;
    return nullptr;
}

// Retorna a faixa que contém o clipe.
static Track* trackOf(TimelineWidget* tl, Clip* c) {
    if (!tl || !tl->project() || !c) return nullptr;
    Project* p = tl->project();
    for (Track& t : p->videoTracks)
        for (Clip& x : t.clips)
            if (&x == c) return &t;
    for (Track& t : p->audioTracks)
        for (Clip& x : t.clips)
            if (&x == c) return &t;
    return nullptr;
}

// Retorna todos os membros do mesmo grupo.
static QVector<Clip*> groupMembers(TimelineWidget* tl, const QString& gid) {
    QVector<Clip*> out;
    if (gid.isEmpty() || !tl || !tl->project()) return out;
    Project* p = tl->project();
    for (Track& t : p->videoTracks)
        for (Clip& c : t.clips)
            if (c.groupId == gid) out.append(&c);
    for (Track& t : p->audioTracks)
        for (Clip& c : t.clips)
            if (c.groupId == gid) out.append(&c);
    return out;
}

// Expande IDs para incluir todos os membros dos grupos.
static QStringList expandToGroups(TimelineWidget* tl, const QStringList& ids) {
    QStringList out = ids;
    for (const QString& id : ids) {
        Clip* c = findClipById(tl, id);
        if (!c || c->groupId.isEmpty()) continue;
        for (Clip* m : groupMembers(tl, c->groupId))
            if (!out.contains(m->id)) out.append(m->id);
    }
    return out;
}

// Remove clipes por IDs (sem ripple).
static void doRemoveClipsByIds(TimelineWidget* tl, const QStringList& ids) {
    if (!tl || !tl->project()) return;
    Project* p = tl->project();
    auto remove = [&](QVector<Clip>& clips) {
        clips.erase(
            std::remove_if(clips.begin(), clips.end(),
                           [&ids](const Clip& c) { return ids.contains(c.id); }),
            clips.end());
    };
    for (Track& t : p->videoTracks) remove(t.clips);
    for (Track& t : p->audioTracks) remove(t.clips);
}

// Ripple delete em uma faixa.
static void rippleDeleteInTrack(Track& t, const QStringList& sel) {
    QVector<Clip> keep;
    QVector<QPair<double, double>> removed;
    for (Clip& c : t.clips) {
        if (sel.contains(c.id))
            removed.append(qMakePair(c.pos, c.pos + c.dur));
        else
            keep.append(c);
    }
    if (removed.isEmpty()) {
        t.clips = keep;
        return;
    }
    std::sort(removed.begin(), removed.end());
    QVector<QPair<double, double>> merged;
    for (const auto& iv : removed) {
        if (merged.isEmpty() || iv.first > merged.last().second + 1e-6)
            merged.append(iv);
        else
            merged.last().second = std::max(merged.last().second, iv.second);
    }
    for (Clip& c : keep) {
        double shift = 0.0;
        for (const auto& iv : merged)
            if (c.pos >= iv.second - 1e-6)
                shift += iv.second - iv.first;
        c.pos = std::max(0.0, c.pos - shift);
    }
    t.clips = keep;
}

// ── Corte ──────────────────────────────────────────────────────────────

void TimelineCommands::splitClipAt(TimelineWidget* tl, Clip* c, double t) {
    if (!c || !tl || !tl->project()) return;

    // Coleta IDs dos membros do grupo (evita invalidação de ponteiros).
    QStringList ids;
    if (!c->groupId.isEmpty()) {
        for (Clip* m : groupMembers(tl, c->groupId))
            ids.append(m->id);
    } else {
        ids.append(c->id);
    }

    const bool grouped = ids.size() > 1;
    const QString fg = grouped ? newId() : QString();
    const QString bg = grouped ? newId() : QString();

    for (const QString& id : ids) {
        Clip* cc = findClipById(tl, id);
        if (!cc) continue;
        if (t <= cc->pos + 1e-6 || t >= cc->pos + cc->dur - 1e-6) {
            if (grouped) cc->groupId = newId();
            continue;
        }

        // Cópia do clipe (metade da frente).
        Clip b = *cc;
        b.id = newId();
        b.pos = t;
        b.in = cc->in + (t - cc->pos);
        b.dur = cc->dur - (t - cc->pos);

        // Ajusta o original.
        cc->dur = t - cc->pos;
        cc->groupId = fg;
        b.groupId = bg;

        Track* tr = trackOf(tl, cc);
        if (tr) {
            int idx = -1;
            for (int i = 0; i < tr->clips.size(); ++i)
                if (tr->clips[i].id == cc->id) { idx = i; break; }
            if (idx >= 0) tr->clips.insert(idx + 1, b);
        }
    }
}

void TimelineCommands::cutAtPlayhead(TimelineWidget* tl) {
    if (!tl || !tl->project()) return;
    emit tl->editStart();

    const QStringList sel = tl->selectedIds();
    const bool hasSel = !sel.isEmpty();

    QHash<QString, QString> units;
    auto consider = [&](const Clip& c) {
        if (trackLocked(tl, &c)) return;
        // Se há seleção, corta apenas clipes selecionados.
        if (hasSel && !sel.contains(c.id)) return;
        if (tl->playhead() > c.pos + 1e-6 && tl->playhead() < c.pos + c.dur - 1e-6) {
            const QString key = c.groupId.isEmpty() ? c.id : c.groupId;
            if (!units.contains(key)) units.insert(key, c.id);
        }
    };
    for (const Track& t : tl->project()->videoTracks)
        for (const Clip& c : t.clips) consider(c);
    for (const Track& t : tl->project()->audioTracks)
        for (const Clip& c : t.clips) consider(c);

    for (const QString& rep : units.values()) {
        Clip* cc = findClipById(tl, rep);
        if (cc) splitClipAt(tl, cc, tl->playhead());
    }
    tl->updateScrollRanges();
    tl->update();
    emit tl->modified();
}

void TimelineCommands::razorSplitAt(TimelineWidget* tl, double t) {
    if (!tl || !tl->project()) return;
    emit tl->editStart();

    QStringList toSplit;
    auto consider = [&](const Clip& c) {
        if (t > c.pos + 1e-6 && t < c.pos + c.dur - 1e-6)
            toSplit.append(c.id);
    };
    for (Track& tr : tl->project()->videoTracks)
        for (const Clip& c : tr.clips) consider(c);
    for (Track& tr : tl->project()->audioTracks)
        for (const Clip& c : tr.clips) consider(c);

    QStringList handled;
    for (const QString& id : toSplit) {
        Clip* cc = findClipById(tl, id);
        if (!cc) continue;
        const QString key = cc->groupId.isEmpty() ? cc->id : cc->groupId;
        if (handled.contains(key)) continue;
        handled.append(key);
        splitClipAt(tl, cc, t);
    }
    tl->updateScrollRanges();
    tl->update();
    emit tl->modified();
}

// ── Exclusão ───────────────────────────────────────────────────────────

void TimelineCommands::deleteSelected(TimelineWidget* tl) {
    if (!tl || !tl->project() || tl->selectedIds().isEmpty()) return;
    emit tl->editStart();

    QStringList sel;
    for (const QString& id : expandToGroups(tl, tl->selectedIds())) {
        Clip* c = findClipById(tl, id);
        if (c && !trackLocked(tl, c)) sel.append(id);
    }
    if (sel.isEmpty()) return;

    for (Track& t : tl->project()->videoTracks) rippleDeleteInTrack(t, sel);
    for (Track& t : tl->project()->audioTracks) rippleDeleteInTrack(t, sel);

    tl->clearSelection();
    tl->invalidateScene();
    tl->updateScrollRanges();
    emit tl->modified();
}

void TimelineCommands::deleteSelectedLeaveGap(TimelineWidget* tl) {
    if (!tl || !tl->project() || tl->selectedIds().isEmpty()) return;
    emit tl->editStart();

    QStringList sel;
    for (const QString& id : expandToGroups(tl, tl->selectedIds())) {
        Clip* c = findClipById(tl, id);
        if (c && !trackLocked(tl, c)) sel.append(id);
    }
    if (sel.isEmpty()) return;

    doRemoveClipsByIds(tl, sel);
    tl->clearSelection();
    tl->updateScrollRanges();
    tl->update();
    emit tl->modified();
}

void TimelineCommands::deleteLoopRipple(TimelineWidget* tl) {
    if (!tl || !tl->project()) return;
    const double loopIn = tl->loopIn();
    const double loopOut = tl->loopOut();
    if (loopOut <= loopIn) return;

    emit tl->editStart();
    const double width = loopOut - loopIn;

    auto editTrack = [&](QVector<Clip>& clips) {
        QVector<Clip> result;
        for (Clip& c : clips) {
            if (trackLocked(tl, &c)) { result.append(c); continue; }
            const double cEnd = c.pos + c.dur;
            if (loopIn <= c.pos && cEnd <= loopOut) continue;
            if (c.pos < loopIn && cEnd > loopOut) {
                c.dur -= width;
                result.append(c);
            } else if (c.pos < loopIn && cEnd > loopIn) {
                c.dur = loopIn - c.pos;
                result.append(c);
            } else if (c.pos < loopOut && cEnd > loopOut) {
                const double shift = loopOut - c.pos;
                c.pos = loopOut;
                c.in += shift;
                c.dur = cEnd - loopOut;
                result.append(c);
            } else if (c.pos >= loopOut) {
                c.pos = std::max(0.0, c.pos - width);
                result.append(c);
            } else {
                result.append(c);
            }
        }
        clips = result;
    };

    for (Track& t : tl->project()->videoTracks) editTrack(t.clips);
    for (Track& t : tl->project()->audioTracks) editTrack(t.clips);

    tl->clearSelection();
    tl->updateScrollRanges();
    tl->update();
    emit tl->modified();
}

void TimelineCommands::deleteLoopLeaveGap(TimelineWidget* tl) {
    if (!tl || !tl->project()) return;
    const double loopIn = tl->loopIn();
    const double loopOut = tl->loopOut();
    if (loopOut <= loopIn) return;

    emit tl->editStart();

    auto editTrack = [&](QVector<Clip>& clips) {
        QVector<Clip> result;
        for (Clip& c : clips) {
            if (trackLocked(tl, &c)) { result.append(c); continue; }
            const double cEnd = c.pos + c.dur;
            if (loopIn <= c.pos && cEnd <= loopOut) continue;
            if (c.pos < loopIn && cEnd > loopOut) c.dur -= (loopOut - loopIn);
            else if (c.pos < loopIn && cEnd > loopIn) c.dur = loopIn - c.pos;
            else if (c.pos < loopOut && cEnd > loopOut) {
                const double shift = loopOut - c.pos;
                c.pos = loopOut;
                c.in += shift;
                c.dur = cEnd - loopOut;
            }
            result.append(c);
        }
        clips = result;
    };

    for (Track& t : tl->project()->videoTracks) editTrack(t.clips);
    for (Track& t : tl->project()->audioTracks) editTrack(t.clips);

    tl->clearSelection();
    tl->updateScrollRanges();
    tl->update();
    emit tl->modified();
}

void TimelineCommands::deleteClipBeforePlayhead(TimelineWidget* tl) {
    if (!tl || !tl->project()) return;
    emit tl->editStart();
    const double t = tl->playhead();
    QStringList toRemove;
    auto consider = [&](Track& tr) {
        for (Clip& c : tr.clips) {
            if (trackLocked(tl, &c)) continue;
            if (c.pos < t && c.pos + c.dur > t) {
                // Clipe cruza o playhead: encurta até o playhead.
                c.dur = t - c.pos;
            } else if (c.pos + c.dur <= t + 1e-6 && c.pos + c.dur > t - 1e-6) {
                // Termina exatamente no playhead: ignora.
            } else if (c.pos + c.dur <= t - 1e-6) {
                // Termina antes do playhead: remove.
                toRemove.append(c.id);
            }
        }
    };
    for (Track& t : tl->project()->videoTracks) consider(t);
    for (Track& t : tl->project()->audioTracks) consider(t);
    if (!toRemove.isEmpty()) doRemoveClipsByIds(tl, toRemove);
    tl->updateScrollRanges();
    tl->update();
    emit tl->modified();
}

void TimelineCommands::deleteClipAfterPlayhead(TimelineWidget* tl) {
    if (!tl || !tl->project()) return;
    emit tl->editStart();
    const double t = tl->playhead();
    QStringList toRemove;
    auto consider = [&](Track& tr) {
        for (Clip& c : tr.clips) {
            if (trackLocked(tl, &c)) continue;
            if (c.pos < t && c.pos + c.dur > t) {
                // Clipe cruza o playhead: ajusta início.
                const double shift = t - c.pos;
                c.pos = t;
                c.in += shift;
                c.dur -= shift;
            } else if (c.pos >= t - 1e-6) {
                // Começa depois do playhead: remove.
                toRemove.append(c.id);
            }
        }
    };
    for (Track& t : tl->project()->videoTracks) consider(t);
    for (Track& t : tl->project()->audioTracks) consider(t);
    if (!toRemove.isEmpty()) doRemoveClipsByIds(tl, toRemove);
    tl->updateScrollRanges();
    tl->update();
    emit tl->modified();
}

// ── Ripple Edit ────────────────────────────────────────────────────────

void TimelineCommands::rippleTrimLeft(TimelineWidget* tl, Clip* c, double newIn) {
    if (!c || !tl || !tl->project()) return;

    const double delta = newIn - c->in;
    if (std::fabs(delta) < 1e-6) return;

    c->in = std::max(0.0, newIn);
    c->dur -= delta;
    c->pos += delta;

    Track* tr = trackOf(tl, c);
    if (!tr) return;

    for (Clip& other : tr->clips) {
        if (other.id == c->id) continue;
        if (other.pos >= c->pos + c->dur - 1e-6) {
            other.pos -= delta;
        }
    }
}

void TimelineCommands::rippleTrimRight(TimelineWidget* tl, Clip* c, double newDur) {
    if (!c || !tl || !tl->project()) return;

    const double delta = newDur - c->dur;
    if (std::fabs(delta) < 1e-6) return;

    c->dur = std::max(kMinDur, newDur);

    Track* tr = trackOf(tl, c);
    if (!tr) return;

    const double cEnd = c->pos + c->dur;
    for (Clip& other : tr->clips) {
        if (other.id == c->id) continue;
        if (other.pos >= cEnd - delta - 1e-6) {
            other.pos += delta;
        }
    }
}

// ── Rolling Edit ───────────────────────────────────────────────────────

void TimelineCommands::rollingEdit(TimelineWidget* tl, Clip* clipA, Clip* clipB, double delta) {
    if (!clipA || !clipB) return;

    const double newDurA = std::max(kMinDur, clipA->dur + delta);
    const double newDurB = std::max(kMinDur, clipB->dur - delta);

    clipA->dur = newDurA;
    clipB->in += (clipB->dur - newDurB);
    clipB->dur = newDurB;
    clipB->pos = clipA->pos + clipA->dur;
}

// ── Slip Edit ──────────────────────────────────────────────────────────

void TimelineCommands::slipEdit(TimelineWidget* tl, Clip* c, double newIn) {
    if (!c || !tl || !tl->project()) return;

    const MediaItem* m = tl->project()->findMedia(c->mediaId);
    const double maxDur = m ? m->duration : (c->in + c->dur);
    const double maxIn = std::max(0.0, maxDur - c->dur);
    c->in = std::min(std::max(0.0, newIn), maxIn);
}

// ── Slide Edit ─────────────────────────────────────────────────────────

void TimelineCommands::slideEdit(TimelineWidget* tl, Clip* c, double origPos,
                                  double origDurPrev, double origDurNext,
                                  double origPosNext, double delta) {
    if (!c) return;

    c->pos = std::max(0.0, origPos + delta);

    // Ajusta clipe anterior (estender/encurtar).
    // Nota: caller deve passar info do clipe anterior se necessário.

    // Ajusta clipe seguinte.
    if (origDurNext > 0) {
        const double nextNewPos = c->pos + c->dur;
        const double shift = nextNewPos - origPosNext;
        // Caller ajusta o próximo clipe.
    }
}

// ── Rate Stretch ───────────────────────────────────────────────────────

void TimelineCommands::rateStretch(TimelineWidget* tl, Clip* c, double origDur, double origSpeed, double newSpeed) {
    if (!c) return;
    c->speed = std::max(0.1, newSpeed);
    c->dur = origDur * (origSpeed / c->speed);
}

// ── Utilitários ────────────────────────────────────────────────────────

bool TimelineCommands::moveClipToTrack(TimelineWidget* tl, const QString& id, int row, bool audio) {
    if (!tl || !tl->project()) return false;
    Project* p = tl->project();

    Track& dst = audio ? p->audioTracks[row] : p->videoTracks[row];
    if (dst.locked) return false;

    Clip* c = findClipById(tl, id);
    if (!c) return false;

    // Remove da faixa atual.
    auto removeFrom = [&](Track& t) {
        for (auto it = t.clips.begin(); it != t.clips.end(); ++it)
            if (it->id == id) { t.clips.erase(it); return true; }
        return false;
    };

    bool removed = removeFrom(dst);
    if (!removed) {
        for (Track& t : p->videoTracks)
            if (removeFrom(t)) { removed = true; break; }
        if (!removed)
            for (Track& t : p->audioTracks)
                if (removeFrom(t)) { removed = true; break; }
        if (!removed) return false;
    }

    // Insere na nova faixa.
    auto it = dst.clips.begin();
    while (it != dst.clips.end() && it->pos <= c->pos) ++it;
    dst.clips.insert(it, *c);
    return true;
}

void TimelineCommands::removeClipsByIds(TimelineWidget* tl, const QStringList& ids) {
    doRemoveClipsByIds(tl, ids);
}

void TimelineCommands::duplicateClip(TimelineWidget* tl, Clip* c) {
    if (!c || !tl || !tl->project()) return;
    // TODO: implementar duplicação completa (estava no TimelineWidget)
}

void TimelineCommands::nudgeSelected(TimelineWidget* tl, int dir) {
    if (!tl || !tl->project()) return;
    const double step = dir * (1.0 / tl->project()->fps);
    for (const QString& id : tl->selectedIds()) {
        Clip* c = findClipById(tl, id);
        if (c && !trackLocked(tl, c))
            c->pos = std::max(0.0, c->pos + step);
    }
    tl->update();
    emit tl->modified();
}
