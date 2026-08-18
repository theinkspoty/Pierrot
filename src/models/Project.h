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
    // Canais por stream de áudio (índice = stream; usado p/ exibir e validar).
    QVector<int> audioChannels;
    // Mídia gerada (sem arquivo): cor sólida (gerador estilo Vegas). Quando
    // true, filePath fica vazio e o clipe é renderizado/exportado como um
    // quadro preenchido com solidColorValue.
    bool isSolid = false;
    QColor solidColor{Qt::black};
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
    KfSmooth = 1, // Auto Bezier (curva suave automática, Catmull-Rom)
    KfStep   = 2, // segura o valor até o próximo keyframe
    KfBezier = 3  // Bezier manual (handles de entrada e saída independentes)
};

struct Keyframe {
    double time = 0.0;
    double value = 0.0;
    int interp = KfLinear; // interpolação do segmento que SAI deste keyframe
    // Bezier manual, estilo Premiere: cada keyframe tem handle de SAÍDA
    // (afeta o segmento à direita) e de ENTRADA (afeta o segmento à esquerda).
    //   Handle de saída:   ponto = (time + ox, value + oy)
    //   Handle de entrada: ponto = (time - ix, value + iy)
    double ox = 0.0;
    double oy = 0.0;
    double ix = 0.0;
    double iy = 0.0;
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
    int lo = 0, hi = (int)keys.size() - 1;
    while (lo + 1 < hi) {
        const int mid = (lo + hi) / 2;
        if (t < keys[mid].time) hi = mid;
        else lo = mid;
    }
    const Keyframe& a = keys[lo];
    const Keyframe& b = keys[lo + 1];
    const double span = b.time - a.time;
    if (span <= 1e-9) return b.value;
    const double f = (t - a.time) / span;
    switch (a.interp) {
        case KfStep:
            return a.value;
        case KfSmooth: {
            const Keyframe& p0 = (lo > 0) ? keys[lo - 1] : a;
            const Keyframe& p3 = (lo + 2 < (int)keys.size()) ? keys[lo + 2] : b;
            const double dt0 = (lo > 0) ? (b.time - p0.time) : span;
            const double m0 = (dt0 > 1e-9) ? (b.value - p0.value) / dt0 : 0.0;
            const double dt3 = (lo + 2 < (int)keys.size()) ? (p3.time - a.time) : span;
            const double m3 = (dt3 > 1e-9) ? (p3.value - a.value) / dt3 : 0.0;
            return cubicBezier(a.value, a.value + m0 * span,
                               b.value - m3 * span, b.value, f);
        }
        case KfBezier: {
            // Bezier paramétrico real (como o Premiere): os handles têm
            // posição própria no tempo (ox, ix) e no valor (oy, iy).
            // Resolve Bx(u)=t por Newton e avalia By(u).
            const double p1x = a.time + a.ox;
            const double p2x = b.time - b.ix;
            const double p1y = a.value + a.oy;
            const double p2y = b.value + b.iy;
            double u = f;
            for (int it = 0; it < 10; ++it) {
                const double w0 = (1.0 - u) * (1.0 - u) * (1.0 - u);
                const double w1 = 3.0 * (1.0 - u) * (1.0 - u) * u;
                const double w2 = 3.0 * (1.0 - u) * u * u;
                const double w3 = u * u * u;
                const double bx = w0 * a.time + w1 * p1x + w2 * p2x + w3 * b.time;
                const double dxdu = 3.0 * (1.0 - u) * (1.0 - u) * (p1x - a.time)
                                  + 6.0 * (1.0 - u) * u * (p2x - p1x)
                                  + 3.0 * u * u * (b.time - p2x);
                const double err = bx - t;
                if (std::fabs(err) < 1e-10) break;
                if (std::fabs(dxdu) < 1e-12) break;
                u -= err / dxdu;
                u = std::clamp(u, 0.0, 1.0);
            }
            const double w0 = (1.0 - u) * (1.0 - u) * (1.0 - u);
            const double w1 = 3.0 * (1.0 - u) * (1.0 - u) * u;
            const double w2 = 3.0 * (1.0 - u) * u * u;
            const double w3 = u * u * u;
            return w0 * a.value + w1 * p1y + w2 * p2y + w3 * b.value;
        }
        case KfLinear:
        default:
            return a.value + (b.value - a.value) * f;
    }
}

// Estilo do texto/título sobreposto de um clipe (usado no preview e na
// exportação). Também é o conteúdo de um TextResource compartilhado.
struct TextStyle {
    QString text;
    QString fontFamily;            // vazio = fonte padrão (DejaVu Sans)
    double textSize = 0.0;         // fração da altura do quadro (0 = padrão ~ h/18)
    bool textBold = true;
    QColor textColor{255, 255, 255};
    double textOutline = 0.0;      // largura do contorno (fração da altura; 0 = sem)
    QColor textOutlineColor{0, 0, 0};
    bool textBackground = false;   // caixa sob o texto (default: sem fundo)
    QColor textBackgroundColor{0, 0, 0, 150};
    double textX = 0.5;            // posição do texto (0..1 no quadro; 0.5 = centro)
    double textY = 0.5;
    int textAlign = 0;             // 0 = centro, 1 = esquerda, 2 = direita
    bool isEmpty() const { return text.trimmed().isEmpty(); }
};

// Recurso de texto compartilhado entre clipes (cópias "unificadas" do Vegas):
// clipes que referenciam o mesmo recurso mostram/exportam o mesmo texto e
// estilo; editar um atualiza todos.
struct TextResource {
    QString id;
    TextStyle text;
};

// Tipos de transição de saída de um clipe (aplicada quando ele se sobrepõe ao
// próximo clipe da mesma faixa de vídeo; a duração é o tamanho da sobreposição).
//  ""/"dissolve" = crossfade; "wipeleft"/"wiperight"/"wipeup"/"wipedown" = o
//  próximo clipe desliza de um dos lados sobre o atual.
inline bool isTransition(const QString& type) {
    return !type.isEmpty();
}

struct Clip {
    QString id;
    QString mediaId;
    QString groupId; // mesmo grupo => vídeo e áudio vinculados
    double pos = 0.0;
    double in = 0.0;
    double dur = 0.0;
    QString name;
    QString transitionType; // transição de saída (vazio = dissolve ao sobrepor)
    // Stream de áudio que este clipe usa (0 = primeiro). Só importa para
    // clipes de áudio de arquivos com múltiplos streams (ex.: OBS/câmera).
    int audioStreamIndex = 0;
    // Clipe independente de texto (sem mídia): desenhado/exportado como texto
    // sobre o quadro. Animável pelas propriedades normais do clipe
    // (transform, opacidade, fades, keyframes).
    bool isText = false;
    // Texto/título. textResourceId vazio => usa `text` (próprio); preenchido =>
    // usa o TextResource compartilhado (cópia unificada).
    TextStyle text;
    QString textResourceId;
    double volume = 1.0;
    double opacity = 1.0;
    double fadeIn = 0.0;
    double fadeOut = 0.0;
    double speed = 1.0;
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
    // Achatamento/esticamento não-uniforme (escala X e Y independentes; 1.0 =
    // neutro, stica/encolhe em cada eixo sem afetar o outro).
    double scaleX = 1.0;
    double scaleY = 1.0;
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
    QVector<Keyframe> kfScaleX;
    QVector<Keyframe> kfScaleY;
    QVector<Keyframe> kfCropL;
    QVector<Keyframe> kfCropR;
    QVector<Keyframe> kfCropT;
    QVector<Keyframe> kfCropB;

    // True se o clipe possui qualquer transformação ativa.
    bool hasTransform() const {
        return tx != 0.0 || ty != 0.0 || scale != 1.0 || rotation != 0.0
            || scaleX != 1.0 || scaleY != 1.0
            || !kfTx.isEmpty() || !kfTy.isEmpty()
            || !kfScale.isEmpty() || !kfRotation.isEmpty()
            || !kfScaleX.isEmpty() || !kfScaleY.isEmpty();
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

struct TrackGroup {
    QString id;
    QString name;
    bool collapsed = false; // faixas da pasta ocultas na timeline
};

struct Track {
    QString name;
    bool audio = false;
    QString blendMode = QStringLiteral("normal");
    double volume = 1.0;
    bool muted = false;
    bool solo = false;
    bool locked = false;
    QString groupId; // pasta (TrackGroup) a que a faixa pertence; vazio = nenhuma
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
    QVector<TrackGroup> trackGroups;
    // Recursos de texto compartilhados (cópias unificadas de texto).
    QVector<TextResource> textResources;

    // Estilo de texto efetivo de um clipe: recurso compartilhado se referenciado,
    // senão o próprio. Nunca retorna nulo.
    const TextStyle* textStyleFor(const Clip& c) const {
        if (!c.textResourceId.isEmpty())
            for (const TextResource& r : textResources)
                if (r.id == c.textResourceId) return &r.text;
        return &c.text;
    }
    TextStyle* textStyleFor(Clip& c) {
        if (!c.textResourceId.isEmpty())
            for (TextResource& r : textResources)
                if (r.id == c.textResourceId) return &r.text;
        return &c.text;
    }

    // Cria (ou reutiliza) um recurso de texto para o clipe e o vincula.
    void bindTextResource(Clip& c) {
        if (!c.textResourceId.isEmpty()) return;
        TextResource r;
        r.id = newId();
        r.text = c.text;
        textResources.append(r);
        c.textResourceId = r.id;
        c.text = TextStyle();
    }

    TrackGroup* findGroup(const QString& id) {
        for (auto& g : trackGroups)
            if (g.id == id) return &g;
        return nullptr;
    }

    void addMarker(const Marker& m) {
        markers.append(m);
        std::sort(markers.begin(), markers.end(),
                  [](const Marker& a, const Marker& b) { return a.time < b.time; });
    }

    void addTrack(bool audio) {
        Track t;
        t.audio = audio;
        t.name = audio
            ? QString("Audio %1").arg(audioTracks.size() + 1)
            : QString("Video %1").arg(videoTracks.size() + 1);
        (audio ? audioTracks : videoTracks).append(t);
    }

    void removeTrack(bool audio, int index) {
        if (audio) {
            if (index >= 0 && index < audioTracks.size()) audioTracks.remove(index);
        } else {
            if (index >= 0 && index < videoTracks.size()) videoTracks.remove(index);
        }
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
