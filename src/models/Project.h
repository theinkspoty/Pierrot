// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QString>
#include <QVector>
#include <QUuid>
#include <QJsonObject>
#include <QColor>
#include <algorithm>
#include <cmath>

struct MediaItem {
    QString id;
    QString filePath;
    QString name;
    double duration = 0.0;
    int width = 0;
    int height = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    int audioStreams = 0;
};

struct Marker {
    QString id;
    double time = 0.0;
    QString name;
    QColor color{QColor(255, 200, 40)};
};

// Modos de interpolação de keyframe.
enum KfInterp {
    KfLinear = 0, // segmento reto
    KfSmooth = 1, // Catmull-Rom (curva suave automática)
    KfStep   = 2, // segura o valor até o próximo keyframe
    KfBezier = 3  // handles manuais (hx, hy)
};

struct Keyframe {
    double time = 0.0;
    double value = 0.0;
    int interp = KfLinear; // interpolação do segmento que SAI deste keyframe
    // Handle de saída (bezier manual): ponto = (time + hx, value + hy).
    double hx = 0.0;
    double hy = 0.0;
};

// Bezier cúbico: A e B são pontos, c1/c2 handles.
inline double cubicBezier(double a, double c1, double c2, double b, double f) {
    const double u = 1.0 - f;
    return u * u * u * a + 3.0 * u * u * f * c1
         + 3.0 * u * f * f * c2 + f * f * f * b;
}

// Interpola os keyframes no instante `t` (timeline, em segundos), respeitando
// o modo de interpolação de cada segmento. Sem keyframes, retorna o valor base.
inline double kfValue(const QVector<Keyframe>& keys, double base, double t) {
    if (keys.isEmpty()) return base;
    if (t <= keys.first().time) return keys.first().value;
    if (t >= keys.last().time) return keys.last().value;
    for (int i = 0; i + 1 < keys.size(); ++i) {
        const Keyframe& a = keys[i];
        const Keyframe& b = keys[i + 1];
        if (t >= a.time && t <= b.time) {
            const double span = b.time - a.time;
            if (span <= 1e-9) return b.value;
            const double f = (t - a.time) / span;
            switch (a.interp) {
                case KfStep:
                    return a.value;
                case KfSmooth: {
                    // Tangente Catmull-Rom por diferenças finitas com os
                    // vizinhos (extrapola nos extremos) -> curva suave.
                    const Keyframe& p0 = (i > 0) ? keys[i - 1] : a;
                    const Keyframe& p3 = (i + 2 < keys.size()) ? keys[i + 2] : b;
                    const double dt0 = (i > 0) ? (b.time - p0.time) : span;
                    const double m0 = (b.value - p0.value) / dt0;
                    const double dt3 = (i + 2 < keys.size()) ? (p3.time - a.time) : span;
                    const double m3 = (p3.value - a.value) / dt3;
                    return cubicBezier(a.value, a.value + m0 * span,
                                       b.value - m3 * span, b.value, f);
                }
                case KfBezier: {
                    const double c1 = a.value + a.hy;
                    const double c2 = b.value - b.hy;
                    return cubicBezier(a.value, c1, c2, b.value, f);
                }
                case KfLinear:
                default:
                    return a.value + (b.value - a.value) * f;
            }
        }
    }
    return keys.last().value;
}

struct Clip {
    QString id;
    QString mediaId;
    QString groupId; // mesmo grupo => vídeo e áudio vinculados
    double pos = 0.0;
    double in = 0.0;
    double dur = 0.0;
    QString name;
    double volume = 1.0;
    double opacity = 1.0;
    double fadeIn = 0.0;
    double fadeOut = 0.0;
    double speed = 1.0;
    QString text;
    double brightness = 0.0;
    double contrast = 1.0;
    double saturation = 1.0;
    double blur = 0.0;
    bool grayscale = false;
    bool chromaKey = false;
    QColor chromaKeyColor{Qt::green};
    double chromaKeySimilarity = 0.15;

    // Transform (em pixels no quadro do projeto, graus, escala multiplicativa).
    double tx = 0.0;
    double ty = 0.0;
    double scale = 1.0;
    double rotation = 0.0;

    // Pan/Crop (fração de cada borda removida, 0..1 do quadro original).
    double cropL = 0.0;
    double cropR = 0.0;
    double cropT = 0.0;
    double cropB = 0.0;

    // Efeitos de áudio (ganhos em dB; 0 = neutro).
    double eqLow = 0.0;
    double eqMid = 0.0;
    double eqHigh = 0.0;
    bool denoise = false;
    double denoiseAmount = 12.0;
    bool normalize = false;
    bool invertPhase = false;

    // Keyframes animados (tempos em segundos da timeline).
    QVector<Keyframe> kfOpacity;
    QVector<Keyframe> kfVolume;
    QVector<Keyframe> kfTx;
    QVector<Keyframe> kfTy;
    QVector<Keyframe> kfScale;
    QVector<Keyframe> kfRotation;
    QVector<Keyframe> kfCropL;
    QVector<Keyframe> kfCropR;
    QVector<Keyframe> kfCropT;
    QVector<Keyframe> kfCropB;

    // True se o clipe possui qualquer transformação ativa.
    bool hasTransform() const {
        return tx != 0.0 || ty != 0.0 || scale != 1.0 || rotation != 0.0
            || !kfTx.isEmpty() || !kfTy.isEmpty()
            || !kfScale.isEmpty() || !kfRotation.isEmpty();
    }

    // True se algum pan/crop (estático ou animado) está ativo.
    bool hasCrop() const {
        return cropL > 1e-6 || cropR > 1e-6 || cropT > 1e-6 || cropB > 1e-6
            || !kfCropL.isEmpty() || !kfCropR.isEmpty()
            || !kfCropT.isEmpty() || !kfCropB.isEmpty();
    }

    bool hasAudioFx() const {
        return std::fabs(eqLow) > 0.01 || std::fabs(eqMid) > 0.01
            || std::fabs(eqHigh) > 0.01 || denoise || normalize || invertPhase;
    }
};

struct Track {
    QString name;
    bool audio = false;
    QString blendMode = QStringLiteral("normal");
    double volume = 1.0;
    bool muted = false;
    bool solo = false;
    bool locked = false;
    int height = 0; // altura da faixa em pixels na timeline; 0 = padrão
    QVector<Clip> clips;
};

inline QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

class Project {
public:
    QString name;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    double audioRate = 48000.0;

    QVector<MediaItem> media;
    QVector<Track> videoTracks;
    QVector<Track> audioTracks;
    QVector<Marker> markers;

    void addMarker(const Marker& m) {
        markers.append(m);
        std::sort(markers.begin(), markers.end(),
                  [](const Marker& a, const Marker& b) { return a.time < b.time; });
    }

    void addTrack(bool audio) {
        Track t;
        t.audio = audio;
        t.name = audio
            ? QString("Faixa de Áudio %1").arg(audioTracks.size() + 1)
            : QString("Faixa de Vídeo %1").arg(videoTracks.size() + 1);
        (audio ? audioTracks : videoTracks).append(t);
    }

    const MediaItem* findMedia(const QString& id) const {
        for (const auto& m : media)
            if (m.id == id) return &m;
        return nullptr;
    }

    double duration() const {
        double d = 0.0;
        for (const auto& t : videoTracks)
            for (const auto& c : t.clips)
                d = std::max(d, c.pos + c.dur);
        for (const auto& t : audioTracks)
            for (const auto& c : t.clips)
                d = std::max(d, c.pos + c.dur);
        return d;
    }

    QJsonObject toJson() const;
    void fromJson(const QJsonObject& obj);
};
