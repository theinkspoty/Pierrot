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

// ── Estruturas de plugins OFX ────────────────────────────────────────────

// Metadados de um efeito OFX (preenchidos durante o scan do plugin).
struct OfxPluginInfo {
    QString id;                 // identificador único (ex.: "org.openfx.invert")
    QString name;               // nome legível ("Invert")
    QString grouping;           // categoria/grouping do efeito
    QString description;        // curta descrição
    QString pluginPath;         // caminho para o bundle .ofx no disco
    QString iconPath;           // caminho ABSOLUTO do ícone (kOfxPropIcon do plugin)
    int versionMajor = 1;
    int versionMinor = 0;
};

// Parâmetro serializado de um efeito OFX (chave → valor).
struct OfxParam {
    QString key;
    QVariant value;
};

// Uma instância de efeito OFX aplicada a um clipe.
struct OfxPluginInstance {
    QString pluginId;           // referência a OfxPluginInfo::id
    bool enabled = true;
    QVector<OfxParam> params;

    double paramDouble(const QString& key, double fallback = 0.0) const {
        for (const OfxParam& p : params)
            if (p.key == key && p.value.canConvert<double>())
                return p.value.toDouble();
        return fallback;
    }
    bool paramBool(const QString& key, bool fallback = false) const {
        for (const OfxParam& p : params)
            if (p.key == key && p.value.canConvert<bool>())
                return p.value.toBool();
        return fallback;
    }
    QString paramString(const QString& key, const QString& fallback = {}) const {
        for (const OfxParam& p : params)
            if (p.key == key && p.value.metaType().id() == QMetaType::QString)
                return p.value.toString();
        return fallback;
    }
    QColor paramColor(const QString& key, const QColor& fallback = Qt::black) const {
        for (const OfxParam& p : params)
            if (p.key == key && p.value.metaType().id() == QMetaType::QColor)
                return p.value.value<QColor>();
        return fallback;
    }
};

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
    // Mídia gerada (sem arquivo): gerador estilo Vegas. Quando true,
    // filePath fica vazio e o clipe é renderizado/exportado como um quadro
    // gerado por `generatorFrame()`.
    bool isSolid = false;
    QColor solidColor{Qt::black};
    // Tipo do gerador quando isSolid: "" = cor sólida, "gradient" (gradiente
    // linear vertical sólida→solidColor2), "checkerboard" (tabuleiro com
    // genCells células por lado) ou "noise" (grão aleatório entre as cores).
    QString generator;
    QColor solidColor2{Qt::white};
    int genCells = 8;
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
    if (t < keys.first().time) return base;
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

// Insere (ou atualiza) um keyframe no instante `time`, mantendo o vetor
// ORDENADO por tempo — kfValue usa busca binária e exige a ordem. Inserir sem
// ordenar (append) deixava a animação errada até reabrir o projeto.
inline void upsertKeyframe(QVector<Keyframe>& keys, double time, double value,
                           int interp = KfLinear) {
    for (Keyframe& k : keys)
        if (qFuzzyIsNull(k.time - time)) { k.value = value; return; }
    Keyframe k;
    k.time = time;
    k.value = value;
    k.interp = interp;
    auto it = std::lower_bound(keys.begin(), keys.end(), time,
        [](const Keyframe& kf, double t) { return kf.time < t; });
    keys.insert(it, k);
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

struct MesaComposition {
    QString id;
    QString name;
    int canvasW = 1920;        // largura do canvas
    int canvasH = 1080;        // altura do canvas

    // Tracks que compõem esta Mesa (ordem = ordem de renderização, fundo → topo).
    // Cada track referencia suas props de canvas (mesaX/Y etc.) na própria Track.
    QVector<QString> trackIds;

    // Câmera (estilo After Effects: define o enquadramento pro output).
    // A posição (camX, camY) é ABSOLUTA em px da composição (origem topo-
    // esquerda); padrão = centro da comp (canvasW/2, canvasH/2).
    double camX = 0.0;         // posição X da câmera (px absolutos no canvas)
    double camY = 0.0;         // posição Y da câmera
    double camZoom = 1.0;      // zoom (1.0 = comp inteira no output)
    double camRotation = 0.0;  // rotação da câmera em graus
    QVector<Keyframe> kfCamX;
    QVector<Keyframe> kfCamY;
    QVector<Keyframe> kfCamZoom;
    QVector<Keyframe> kfCamRotation;

    // ── Motion blur por amostragem temporal (estilo AE) ─────────────────
    // Quando ligado, o quadro é integrado por `motionBlurSamples`
    // sub-passadas distribuídas em volta do tempo atual, cobrindo
    // `motionBlurShutter` fração de quadro (1.0 = "obturador de 360°").
    // No canvas cada camada rastra o próprio transform; no preview/export a
    // câmera também rastra (redefine o enquadramento por amostra).
    bool motionBlur = false;
    int motionBlurSamples = 8;
    double motionBlurShutter = 0.5;
};

// Tipos de transição de saída de um clipe (aplicada quando ele se sobrepõe ao
// próximo clipe da mesma faixa de vídeo; a duração é o tamanho da sobreposição).
//  ""/"dissolve" = crossfade; "wipeleft"/"wiperight"/"wipeup"/"wipedown" = o
//  próximo clipe desliza de um dos lados sobre o atual; "wipetl"/"wipetr"/
//  "wipebr"/"wipebl" = variações diagonais (canto de onde o próximo entra).
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

    // ── Correção de cor (estilo vegas: Lift/Gamma/Gain) ─────────────────
    // Lift: afeta os PRETOS (escala com 255 - pixel). Gamma: curva de poder
    // nos meios (1.0 = neutro). Gain: afeta os BRANCOS (escala com o pixel).
    // Faixas típicas: lift/gain em [-1, 1], gamma em [0.1, 4].
    double liftR = 0.0;
    double liftG = 0.0;
    double liftB = 0.0;
    double gammaR = 1.0;
    double gammaG = 1.0;
    double gammaB = 1.0;
    double gainR = 0.0;
    double gainG = 0.0;
    double gainB = 0.0;
    bool hasColorGrade() const {
        return liftR != 0.0 || liftG != 0.0 || liftB != 0.0
            || gammaR != 1.0 || gammaG != 1.0 || gammaB != 1.0
            || gainR != 0.0 || gainG != 0.0 || gainB != 0.0;
    }

    // ── Efeito Pierrot: LAINKA (stop motion) ─────────────────────────────
    bool lainkaEnabled = false;
    int lainkaSkip = 2;         // frame step (pulo de frame)
    double lainkaJitterPos = 0.0;  // tremida de posição (0..100)
    double lainkaJitterRot = 0.0;  // tremida de rotação (0..100)
    double lainkaJitterScale = 0.0;// tremida de escala (0..100)
    double lainkaFlicker = 0.0;    // variação de brilho (0..100)
    double lainkaFlickerSpeed = 50.0; // velocidade do flicker (0..100)
    double lainkaWarpAmount = 0.0; // distorção por grid (0..100)
    double lainkaWarpSpeed = 50.0; // velocidade do warp (0..100)
    int lainkaWarpGrid = 8;        // resolução do grid warp (4..64)
    double lainkaOnionSkin = 0.0;  // ghosting/overlay (0..100)
    double lainkaDustAmount = 0.0; // pó e sujeira (0..100)
    double lainkaScratchAmount = 0.0; // arranhões (0..100)
    int lainkaTargetFps = 8;       // FPS alvo do stop motion
    double lainkaMotionBlur = 0.0; // motion blur (0..100)
    double lainkaOpacity = 100.0;  // opacidade global (0..100)
    int lainkaAntialias = 1;       // 0=off, 1=on, 2=high quality

    // ── Efeito Pierrot: MotiOn (motion blur) ──────────────────────────
    bool motionEnabled = false;
    double motionAmount = 0.0;     // intensidade do blur (0..100)
    double motionAngle = 0.0;      // direção em graus (0..360)
    int motionSamples = 8;         // amostras de qualidade (1..32)

    // Transform (em pixels no quadro do projeto, graus, escala multiplicativa).
    double tx = 0.0;
    double ty = 0.0;
    double scale = 1.0;
    // Achatamento/esticamento não-uniforme (escala X e Y independentes; 1.0 =
    // neutro, stica/encolhe em cada eixo sem afetar o outro).
    double scaleX = 1.0;
    double scaleY = 1.0;
    double rotation = 0.0;
    // Ponto de ancoragem (relativo ao centro: 0,0 = centro; -1,-1 = canto superior esquerdo).
    double anchorX = 0.0;
    double anchorY = 0.0;

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
    // Reverb EX: mistura úmida (0..1) e tamanho de sala (0..1).
    bool reverb = false;
    double reverbMix = 0.35;
    double reverbSize = 0.5;

    // ── Efeitos OFX (plugins de terceiros) ──────────────────────────────
    QVector<OfxPluginInstance> ofxFx;  // stack de efeitos OFX (ordem = ordem de aplicação)

    // Keyframes animados (tempos em segundos da timeline).
    QVector<Keyframe> kfOpacity;
    QVector<Keyframe> kfVolume;
    QVector<Keyframe> kfTx;
    QVector<Keyframe> kfTy;
    QVector<Keyframe> kfScale;
    QVector<Keyframe> kfRotation;
    QVector<Keyframe> kfScaleX;
    QVector<Keyframe> kfScaleY;
    QVector<Keyframe> kfAnchorX;
    QVector<Keyframe> kfAnchorY;
    QVector<Keyframe> kfCropL;
    QVector<Keyframe> kfCropR;
    QVector<Keyframe> kfCropT;
    QVector<Keyframe> kfCropB;

    // True se o clipe possui qualquer transformação ativa.
    bool hasTransform() const {
        return tx != 0.0 || ty != 0.0 || scale != 1.0 || rotation != 0.0
            || scaleX != 1.0 || scaleY != 1.0
            || anchorX != 0.0 || anchorY != 0.0
            || !kfTx.isEmpty() || !kfTy.isEmpty()
            || !kfScale.isEmpty() || !kfRotation.isEmpty()
            || !kfScaleX.isEmpty() || !kfScaleY.isEmpty()
            || !kfAnchorX.isEmpty() || !kfAnchorY.isEmpty();
    }

    // True se algum pan/crop (estático ou animado) está ativo.
    bool hasCrop() const {
        return cropL > 1e-6 || cropR > 1e-6 || cropT > 1e-6 || cropB > 1e-6
            || !kfCropL.isEmpty() || !kfCropR.isEmpty()
            || !kfCropT.isEmpty() || !kfCropB.isEmpty();
    }

    bool hasAudioFx() const {
        return std::fabs(eqLow) > 0.01 || std::fabs(eqMid) > 0.01
            || std::fabs(eqHigh) > 0.01 || denoise || normalize || invertPhase
            || reverb;
    }
};

struct TrackGroup {
    QString id;
    QString name;
    bool collapsed = false; // faixas da pasta ocultas na timeline
    QString mesaId;         // vazio = pasta normal; preenchido = grupo Mesa
};

struct Track {
    QString id;                // id único da track (para referência em MesaComposition)
    QString name;
    bool audio = false;
    QString blendMode = QStringLiteral("normal");
    double volume = 1.0;
    double pan = 0.0; // -1.0 (esquerda) a +1.0 (direita), 0.0 = centro
    QColor color;     // cor da faixa (inválida = paleta automática por índice)
    // FX de áudio por faixa (estilo Vegas: aplicado ao barramento da faixa,
    // depois da soma dos clipes dela e antes da mistura final).
    double eqLow = 0.0;
    double eqMid = 0.0;
    double eqHigh = 0.0;
    bool denoise = false;
    double denoiseAmount = 12.0;
    bool invertPhase = false;
    bool reverb = false;
    double reverbMix = 0.35;
    double reverbSize = 0.5;
    bool hasAudioFx() const {
        return std::fabs(eqLow) > 0.01 || std::fabs(eqMid) > 0.01
            || std::fabs(eqHigh) > 0.01 || denoise || invertPhase || reverb;
    }
    // Envelopes de áudio por faixa (tempos em segundos da timeline, absolutos).
    // kfVolume: ganho linear 0..2; kfPan: -1..+1 (equal-power).
    QVector<Keyframe> kfVolume;
    QVector<Keyframe> kfPan;
    // Opacidade da faixa de vídeo (0..1, estilo Vegas/FCE): a faixa inteira
    // composta com transparência sobre as de baixo.
    double opacity = 1.0;
    bool muted = false;
    bool solo = false;
    bool locked = false;
    QString groupId; // pasta (TrackGroup) a que a faixa pertence; vazio = nenhuma
    int height = 0; // altura da faixa em pixels na timeline; 0 = padrão
    QVector<Clip> clips;

    // ── Propriedades de canvas (Mesa: composição 2D, estilo After Effects) ──
    // Estas propriedades são usadas quando a track pertence a um grupo Mesa.
    // Posição, escala, rotação e opacidade da track no canvas da composição.
    // O espaço da composição tem a ORIGEM no CANTO SUPERIOR ESQUERDO.
    // mesaX/mesaY = posição ABSOLUTA do ponto de âncora em px (padrão = centro
    // da comp). A cadeia de transform é AE: T(pos)·R·S·T(-âncora), onde o
    // âncora é um OFFSET do CENTRO NATURAL da layer em px da própria layer.
    double mesaX = 0.0;          // posição X da âncora no canvas (px absolutos)
    double mesaY = 0.0;          // posição Y da âncora no canvas
    double mesaScaleX = 1.0;     // escala horizontal no canvas
    double mesaScaleY = 1.0;     // escala vertical no canvas
    double mesaRotation = 0.0;   // rotação no canvas (graus)
    double mesaOpacity = 1.0;    // opacidade no canvas (0..1)
    double mesaAnchorX = 0.0;    // anchor X: offset do centro natural (px da layer)
    double mesaAnchorY = 0.0;    // anchor Y: offset do centro natural (px da layer)

    // Estado da camada na Mesa (independente do resto da timeline):
    // mesaHidden = não desenhada (olho desligado); mesaLocked = não selecionável
    // nem transformável (cadeado).
    bool mesaHidden = false;
    bool mesaLocked = false;
    // Motion blur desta camada (Vegas: "Allow motion blur" por clipe). O blur
    // global da composição (Ctrl+Shift+B) só borra camadas com este flag ON.
    bool mesaMotionBlur = true;

    // Keyframes das propriedades de canvas (tempos em segundos da timeline).
    QVector<Keyframe> kfMesaX;
    QVector<Keyframe> kfMesaY;
    QVector<Keyframe> kfMesaScaleX;
    QVector<Keyframe> kfMesaScaleY;
    QVector<Keyframe> kfMesaRotation;
    QVector<Keyframe> kfMesaOpacity;
    QVector<Keyframe> kfMesaAnchorX;
    QVector<Keyframe> kfMesaAnchorY;

    // True se a track tem propriedades de canvas ativas (diferentes do padrão).
    bool hasMesaTransform() const {
        return mesaX != 0.0 || mesaY != 0.0
            || mesaScaleX != 1.0 || mesaScaleY != 1.0
            || mesaRotation != 0.0
            || mesaAnchorX != 0.0 || mesaAnchorY != 0.0
            || !kfMesaX.isEmpty() || !kfMesaY.isEmpty()
            || !kfMesaScaleX.isEmpty() || !kfMesaScaleY.isEmpty()
            || !kfMesaRotation.isEmpty();
    }
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
    double masterVolume = 1.0; // Volume geral do mixer

    QVector<MediaItem> media;
    QVector<Track> videoTracks;
    QVector<Track> audioTracks;
    QVector<Marker> markers;
    QVector<TrackGroup> trackGroups;
    // Recursos de texto compartilhados (cópias unificadas de texto).
    QVector<TextResource> textResources;
    // Composições 2D (Mesas).
    QVector<MesaComposition> mesas;

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

    MesaComposition* findMesa(const QString& id) {
        for (auto& m : mesas)
            if (m.id == id) return &m;
        return nullptr;
    }
    const MesaComposition* findMesa(const QString& id) const {
        for (const auto& m : mesas)
            if (m.id == id) return &m;
        return nullptr;
    }

    // Mesa que contém a track `trackId` (via mesa.trackIds, a fonte da
    // verdade). Retorna nulo se a track não pertence a nenhuma Mesa. Não usa
    // o vínculo track.groupId→group.mesaId, que pode se perder se a pasta for
    // excluída/desagrupada.
    const MesaComposition* findMesaForTrack(const QString& trackId) const {
        for (const auto& m : mesas)
            if (m.trackIds.contains(trackId)) return &m;
        return nullptr;
    }

    void addMarker(const Marker& m) {
        markers.append(m);
        std::sort(markers.begin(), markers.end(),
                  [](const Marker& a, const Marker& b) { return a.time < b.time; });
    }

    void addTrack(bool audio) {
        Track t;
        t.id = newId();
        t.audio = audio;
        t.name = audio
            ? QString("Audio %1").arg(audioTracks.size() + 1)
            : QString("Video %1").arg(videoTracks.size() + 1);
        (audio ? audioTracks : videoTracks).append(t);
    }

    void removeTrack(bool audio, int index) {
        const QVector<Track>& src = audio ? audioTracks : videoTracks;
        if (index < 0 || index >= src.size()) return;
        const QString tid = src[index].id;
        if (audio) {
            audioTracks.remove(index);
        } else {
            videoTracks.remove(index);
        }
        // Remove a referência da track em qualquer Mesa (evita cantos vazios
        // e "conteúdo" que some ao reabrir o projeto).
        if (!tid.isEmpty())
            for (MesaComposition& m : mesas)
                m.trackIds.removeAll(tid);
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
