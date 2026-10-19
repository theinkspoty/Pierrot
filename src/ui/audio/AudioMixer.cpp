// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ui/audio/AudioMixer.h"

#include <QDebug>

namespace {
// Ganho efetivo de um clipe de áudio em `t` (tempo de timeline): volume do
// clipe × envelope do clipe × envelope/volume da faixa × fades × crossfade de
// transição com vizinho da mesma faixa, clampado 0..2. Fonte única para as
// fontes ATIVAS (buildMixSources) e para as pré-aquecidas (buildWarmSources):
// as warm precisam do MESMO ganho da costura, senão o clipe que entra num
// corte toca com o volume cru do clipe e perde o fader da faixa → "mergulho"
// curto no corte até o próximo tick recalcular.
double previewClipAudioVol(const Track& tr, const Clip& c, double t) {
    const double rel = t - c.pos;
    double vol = c.volume * kfValue(c.kfVolume, 1.0, rel)
                 * kfValue(tr.kfVolume, tr.volume, t);
    if (c.fadeIn > 1e-6) vol *= std::min(1.0, rel / c.fadeIn);
    if (c.fadeOut > 1e-6) vol *= std::min(1.0, (c.dur - rel) / c.fadeOut);
    // Crossfade de transição: sobreposição com vizinho da MESMA faixa vira
    // fade-in (da frente) / fade-out (de trás), em POTÊNCIA CONSTANTE
    // (equal-power): in = sin(π/2·p) e out = cos(π/2·p), cujos quadrados somam
    // 1 e mantêm a energia no meio da sobreposição. O linear afundava ~3 dB no
    // centro. Espelha o `afade=...:curve=qsin` do export (ProjectExporter).
    double transIn = 0.0, transOut = 0.0;
    for (const Clip& o : tr.clips) {
        if (o.id == c.id) continue;
        if (o.pos < c.pos - 1e-6 && o.pos + o.dur > c.pos + 1e-6)
            transIn = std::max(transIn, o.pos + o.dur - c.pos);
        if (o.pos > c.pos + 1e-6 && o.pos < c.pos + c.dur - 1e-6)
            transOut = std::max(transOut, c.pos + c.dur - o.pos);
    }
    // O fade-in não pode durar mais que o clipe (senão o ganho nunca chega a 1);
    // o export clampa igual (min(d, dur)).
    transIn = std::min(transIn, c.dur);
    constexpr double kHalfPi = 1.5707963267948966; // π/2
    if (transIn > 1e-6) vol *= std::sin(kHalfPi * std::clamp(rel / transIn, 0.0, 1.0));
    if (transOut > 1e-6)
        vol *= std::sin(kHalfPi * std::clamp((c.dur - rel) / transOut, 0.0, 1.0));
    return std::clamp(vol, 0.0, 2.0);
}
} // namespace

// Clip de áudio "ativo": o mais acima (vídeo ou áudio) em `t` cujo media tem áudio.
// Clipes de áudio ativos em `t`, prontos para o mixer. Dedupe de vídeo+áudio
// vinculados (mesmo groupId): a faixa de áudio vence, senão o som sobraria
// duas vezes. Respeita mute/solo das faixas, volume de faixa, volume/envelope
// do clipe e fades.
QVector<AudioMixer::SourceInfo> buildMixSources(const Project* p, double t) {
    QVector<AudioMixer::SourceInfo> out;
    if (!p) return out;
    bool anySolo = false;
    // Só faixas de ÁUDIO têm solo com efeito no mix: faixas de vídeo nunca
    // são fonte de áudio neste app (o áudio de um vídeo vive na faixa de áudio
    // pareada), então solo delas não deve silenciar o mix.
    for (const Track& tr : p->audioTracks)
        if (tr.solo) { anySolo = true; break; }

    struct Rep { const Clip* clip; double vol; double pan; int trackIdx; bool isAudio; };
    QHash<QString, Rep> reps;
    // Só faixas de ÁUDIO são fonte de áudio. O áudio de um vídeo vive na faixa
    // de áudio pareada (criada no import via groupId); o clipe de vídeo nunca
    // toca sozinho. Assim, apagar o clipe de áudio silencia o som (em vez de o
    // clipe de vídeo continuar tocando por não ser mais sobrescrito), e faixas
    // de vídeo não ganham strip/fader/VU no mixer.
    auto collect = [&](const QVector<Track>& tracks, bool isAudio) {
        for (int ti = 0; ti < tracks.size(); ++ti) {
            const Track& tr = tracks[ti];
            if (!tr.visible) continue;
            for (const Clip& c : tr.clips) {
                if (!(t >= c.pos && t < c.pos + c.dur)) continue;
                if (tr.muted || (anySolo && !tr.solo)) {
                    if (audioDbg())
                        qDebug().noquote() << QStringLiteral("[audio] MUTE skip t=%1 faixa='%2 base=%3")
                              .arg(t, 0, 'f', 3).arg(tr.name)
                              .arg(c.groupId.isEmpty() ? c.id : c.groupId);
                    continue;
                }
                const MediaItem* m = p->findMedia(c.mediaIdAt(t - c.pos));
                if (!m || !m->hasAudio) continue;
                const double vol = previewClipAudioVol(tr, c, t);
                // O STREAM diferencia as faixas de um arquivo multicanal (cada
                // clipe de áudio usa o seu stream); groupId não é mais preciso
                // aqui, pois só a faixa de áudio contribui.
                const QString key = QStringLiteral("%1|%2").arg(c.id).arg(c.audioStreamIndex);
                reps.insert(key, {&c, vol, kfValue(tr.kfPan, tr.pan, t), ti, isAudio});
            }
        }
    };
    collect(p->audioTracks, true);

    for (auto it = reps.cbegin(); it != reps.cend(); ++it) {
        const Clip* c = it.value().clip;
        const MediaItem* m = p->findMedia(c->mediaIdAt(t - c->pos));
        if (!m || !m->hasAudio) continue;
        if (audioDbg())
            qDebug().noquote() << QStringLiteral("[audio] ativo t=%1 faixa='%2' isAudio=%3 base=%4 vol=%5")
                  .arg(t, 0, 'f', 3)
                  .arg((it.value().isAudio
                            ? p->audioTracks[it.value().trackIdx].name
                            : p->videoTracks[it.value().trackIdx].name))
                  .arg(it.value().isAudio)
                  .arg(!c->groupId.isEmpty() ? c->groupId : c->id)
                  .arg(it.value().vol, 0, 'f', 3);
        AudioMixer::SourceInfo si;
        si.key = c->id;
        si.path = m->filePath;
        si.audioStream = c->audioStreamIndex;
        si.mediaPos = c->in + (t - c->pos) * c->speed;
        si.speed = c->speed;
        si.clipPos = c->pos;
        si.clipDur = c->dur;
        si.mediaStart = c->in;
        si.vol = it.value().vol;
        si.eqLow = c->eqLow;
        si.eqMid = c->eqMid;
        si.eqHigh = c->eqHigh;
        si.denoise = c->denoise;
        si.denoiseAmount = c->denoiseAmount;
        si.normalize = c->normalize;
        si.invertPhase = c->invertPhase;
        si.reverb = c->reverb;
        si.reverbMix = c->reverbMix;
        si.reverbSize = c->reverbSize;
        si.trackIndex = it.value().trackIdx;
        si.isAudioTrack = it.value().isAudio;
        si.pan = it.value().pan;
        if (it.value().isAudio) {
            const int ti = it.value().trackIdx;
            if (ti >= 0 && ti < (int)p->audioTracks.size()) {
                const Track& tr = p->audioTracks[ti];
                si.trackFxOn = tr.hasAudioFx();
                si.trackFxEqLow = tr.eqLow;
                si.trackFxEqMid = tr.eqMid;
                si.trackFxEqHigh = tr.eqHigh;
                si.trackGraphicEq = tr.graphicEq;
                si.trackParametricEq = tr.parametricEq;
                si.trackFxDenoise = tr.denoise;
                si.trackFxDenoiseAmount = tr.denoiseAmount;
                si.trackFxInvertPhase = tr.invertPhase;
                si.trackFxReverb = tr.reverb;
                si.trackFxReverbMix = tr.reverbMix;
                si.trackFxReverbSize = tr.reverbSize;
            }
        }
        out.append(si);
    }
    return out;
}

// Fontes "pré-aquecidas": clipes de áudio que começam dentro da janela de
// lookahead à frente do playhead. Entram no mix JÁ ATIVAS, mas contribuindo
// zero até o seu frame de saída de início (gating por [srcOutStart, srcOutEnd)).
// Assim, o clipe que começa num corte já está no mix na "chunk" que atravessa
// a costura → a troca esquerda→direita é amostra-exata, sem lacuna nem eco.
QVector<AudioMixer::SourceInfo> buildWarmSources(
    const Project* p, double t, const QVector<AudioMixer::SourceInfo>& active) {
    QVector<AudioMixer::SourceInfo> out;
    if (!p) return out;
    constexpr double kWarmWin = 0.8; // segundos à frente do playhead
    bool anySolo = false;
    // Só faixas de ÁUDIO têm solo com efeito (consistente com buildMixSources).
    for (const Track& tr : p->audioTracks)
        if (tr.solo) { anySolo = true; break; }

    QSet<QString> have;
    for (const AudioMixer::SourceInfo& a : active) have.insert(a.key);
    // Só faixas de ÁUDIO são fonte de áudio (o áudio de um vídeo vive na faixa
    // de áudio pareada). Consistente com buildMixSources.
    auto warmCollect = [&](const QVector<Track>& tracks, bool isAudio) {
        for (int ti = 0; ti < (int)tracks.size(); ++ti) {
            const Track& tr = tracks[ti];
            if (!tr.visible || tr.muted || (anySolo && !tr.solo)) continue;
            for (const Clip& c : tr.clips) {
                // Já em reprodução agora, ou começa depois da janela: nada a
                // aquecer.
                if (t >= c.pos && t < c.pos + c.dur) continue;
                if (c.pos <= t || c.pos >= t + kWarmWin) continue;
                const MediaItem* m = p->findMedia(c.mediaId);
                if (!m || !m->hasAudio) continue;
                if (have.contains(c.id)) continue;
                have.insert(c.id);
                AudioMixer::SourceInfo si;
                si.key = c.id; // a chave precisa ser a mesma do want futuro
                si.path = m->filePath;
                si.audioStream = c.audioStreamIndex;
                si.mediaPos = c.in; // começo do áudio do clipe
                si.speed = c.speed;
                si.clipPos = c.pos;
                si.clipDur = c.dur;
                si.mediaStart = c.in;
                // Mesmo ganho do instante da costura (início do clipe): inclui
                // fader/automação da faixa e fades. Antes usava `c.volume` cru,
                // então o clipe que entrava num corte saía baixo (sem o fader)
                // até o próximo tick — o "mergulho" no corte.
                si.vol = previewClipAudioVol(tr, c, c.pos);
                si.pan = kfValue(tr.kfPan, tr.pan, c.pos);
                si.trackIndex = ti;
                si.isAudioTrack = isAudio;
                si.eqLow = c.eqLow;
                si.eqMid = c.eqMid;
                si.eqHigh = c.eqHigh;
                si.denoise = c.denoise;
                si.denoiseAmount = c.denoiseAmount;
                si.normalize = c.normalize;
                si.invertPhase = c.invertPhase;
                si.reverb = c.reverb;
                si.reverbMix = c.reverbMix;
                si.reverbSize = c.reverbSize;
                {
                    const Track& tac = p->audioTracks[ti];
                    si.trackFxOn = tac.hasAudioFx();
                    si.trackFxEqLow = tac.eqLow;
                    si.trackFxEqMid = tac.eqMid;
                    si.trackFxEqHigh = tac.eqHigh;
                    si.trackGraphicEq = tac.graphicEq;
                    si.trackParametricEq = tac.parametricEq;
                    si.trackFxDenoise = tac.denoise;
                    si.trackFxDenoiseAmount = tac.denoiseAmount;
                    si.trackFxInvertPhase = tac.invertPhase;
                    si.trackFxReverb = tac.reverb;
                    si.trackFxReverbMix = tac.reverbMix;
                    si.trackFxReverbSize = tac.reverbSize;
                }
                if (audioDbg())
                    qDebug().noquote() << QStringLiteral("[audio] warm t=%1 faixa='%2' isAudio=%3 clip=%4 pos=%5")
                          .arg(t, 0, 'f', 3)
                          .arg(tracks[ti].name)
                          .arg(isAudio)
                          .arg(c.id)
                          .arg(c.pos, 0, 'f', 3);
                out.append(si);
            }
        }
    };
    warmCollect(p->audioTracks, true);
    return out;
}
