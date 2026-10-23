// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.
//
// Helpers das fases do exportador (filters de vídeo/áudio, pré-renders,
// codecs). Viviam como anon-namespace/static no .cpp de buildCommand —
// viraram um bloco próprio para o .cpp principal ficar só com o método.

#pragma once

#include "ProjectExporter.h"
#include "colombina/util.h"
#include "colombina/generators.h"
#include "colombina/render/MesaRenderer.h"
#include "colombina/render/TextRender.h"
#include "laartman/Decoder.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "colombina/fx/ColorGrade.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QTextStream>

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <limits>

namespace {
// Regex compartilhada para normalização de nomes de fonte.
static const QRegularExpression kFontNameKeep(QStringLiteral("[^a-z0-9]"));

// Imagens estáticas precisam de "-loop 1" para virarem um fluxo contínuo de
// quadros (senão o ffmpeg lê um único frame e a imagem "pisca" ou quebra o
// gráfico de filtros).
struct VideoClipRef {
    const Clip* c;
    const MediaItem* m;
    QString blend;
    double trackOpacity = 1.0; // opacidade da faixa (0..1, estilo Vegas/FCE)

    // Banda Mesa (c == nullptr): a composição inteira pré-renderizada por frame
    // pelo MesaRenderer, inserida no Z do topo do grupo. bandPos = pos do clipe
    // sintético usado no sort por pos (igual aos clipes reais).
    double bandPos = -1.0;
    double bandStart = 0.0;
    double bandEnd = 0.0;
};

// Banda Mesa paralela a VideoClipRef: identifica o grupo pré-renderizado e o
// padrão ffmpeg da sequência de PNGs gerada (vazio para clipes normais).
struct MesaBandRef {
    const MesaComposition* mc = nullptr;
    int k = -1;
    QString pattern;
    double start = 0.0;
    double end = 0.0;
};

// Posição do clipe (ou banda Mesa) na timeline, para o sort por pos.
static double vclipPos(const VideoClipRef& v) {
    return v.c ? v.c->pos : v.bandPos;
}

// Expande um clipe multicam em segmentos por ângulo (cada segmento vira um
// clipe normal com mediaId/in do ângulo ativo). Cortes vêm de kfAngle
// (KfStep) + defaultAngle. O kfSpeed é deslocado para o tempo relativo do
// segmento, para o caminho de velocity da exportação continuar válido.
static QVector<Clip> expandMulticamClip(const Clip& c) {
    QVector<Clip> out;
    if (!c.hasMulticam()) return out;

    struct Seg { double t0 = 0.0, t1 = 0.0; int angle = 0; };
    QVector<Seg> segs;
    int ang = c.angleAt(0.0);
    double t0 = 0.0;
    for (const Keyframe& k : c.kfAngle) {
        if (k.time <= 1e-9) continue;
        if (k.time >= c.dur - 1e-9) break;
        const int na = (int)std::lround(k.value);
        if (na != ang) {
            segs.push_back({t0, k.time, ang});
            t0 = k.time;
            ang = na;
        }
    }
    segs.push_back({t0, c.dur, ang});

    for (const Seg& s : segs) {
        if (s.t1 - s.t0 <= 1e-6) continue;
        Clip sc = c; // mantém efeitos/transform/fades do clipe pai
        sc.isMulticam = false;
        sc.multicamSources.clear();
        sc.multicamIns.clear();
        sc.kfAngle.clear();
        sc.defaultAngle = 0;
        sc.mediaId = c.multicamSources.value(s.angle, c.mediaId);
        const double angIn = (s.angle < c.multicamIns.size())
                                 ? c.multicamIns[s.angle] : c.in;
        if (c.kfSpeed.isEmpty()) {
            sc.in = angIn + s.t0 * std::max(0.01, c.speed);
        } else {
            sc.in = angIn;
            sc.kfSpeed.clear();
            for (const Keyframe& k : c.kfSpeed) {
                if (k.time >= s.t0 - 1e-9 && k.time <= s.t1 + 1e-9) {
                    Keyframe nk = k;
                    nk.time = k.time - s.t0;
                    sc.kfSpeed.append(nk);
                }
            }
            // Garante que a velocidade em rel=0 do segmento seja a de t0.
            if (sc.kfSpeed.isEmpty() || sc.kfSpeed.first().time > 1e-9) {
                Keyframe head;
                head.time = 0.0;
                head.value = clipSpeedAt(c, s.t0);
                head.interp = KfStep;
                sc.kfSpeed.prepend(head);
            }
        }
        sc.pos = c.pos + s.t0;
        sc.dur = s.t1 - s.t0;
        sc.fadeIn = (s.t0 < 1e-9) ? c.fadeIn : 0.0;
        sc.fadeOut = (s.t1 >= c.dur - 1e-9) ? c.fadeOut : 0.0;
        out.push_back(sc);
    }
    return out;
}

// Pré-renderiza a composição Mesa de `start` a `end` (segundos absolutos da
// timeline), um PNG RGBA por frame na taxa FPS, no diretório temporário.
// Retorna o padrão image2 para o ffmpeg (ex.: /tmp/pierrot-mesa-0-%05d.png)
// ou vazio se nada for gerado. Caro em CPU: o empilhamento QPainter do canvas
// + câmera roda por frame — a única forma WYSIWYG de levar a Mesa para a
// exportação (o ffmpeg sozinho não reproduz os blends entre layers + câmera).
// `done`/`total` acumulam o progresso da composição toda (em frames); se o
// callback de progresso retornar false, aborta e marca `*aborted`.
static QString renderMesaSequence(MesaRenderer& renderer, const Project& project,
                                  const MesaComposition& mc, double start, double end,
                                  int fps, int k, long long& done, long long total,
                                  const ProjectExporter::Progress& progress,
                                  bool* aborted) {
    if (end <= start || fps <= 0) return QString();
    const int frameCount = std::max(1, (int)std::ceil((end - start) * fps));
    const QString dir = QDir::tempPath();
    const QString pattern =
        QStringLiteral("%1/pierrot-mesa-%2-%05d.png").arg(dir).arg(k);
    int written = 0;
    for (int i = 0; i < frameCount; ++i) {
        // t no início do intervalo de cada frame: o quadro i fica em
        // start + i/fps, batendo com o PTS do image2 (-framerate fps).
        const double t = start + double(i) / fps;
        if (t >= end - 1e-9) break;
        ++done;
        if (progress) {
            const int pct = total > 0 ? (int)(done * 100LL / total) : 0;
            if (!progress(pct)) {
                *aborted = true;
                return QString();
            }
        }
        const QImage frame = renderer.render(mc, project, t);
        if (frame.isNull()) continue;
        const QString name =
            QStringLiteral("%1/pierrot-mesa-%2-%3.png").arg(dir).arg(k)
                .arg(i + 1, 5, 10, QLatin1Char('0'));
        if (frame.save(name, "PNG")) {
            ++written;
        } else {
            qWarning() << "[export] falha ao salvar frame Mesa" << name;
        }
    }
    if (written == 0) return QString();
    return pattern;
}

// Pré-renderiza um clipe com velocity envelope (rampa de velocidade) numa
// sequência PNG, um quadro por frame na taxa FPS. Cada frame é decodificado na
// posição de fonte clipSrcTime(c, rel) — a MESMA usada pelo preview, garantindo
// paridade. O image2 resultante é alimentado como um input normal (cadência já
// correta), então o setpts da cadeia usa velocidade 1.0 (só reposiciona).
// Decodifica na largura `maxW` (largura do projeto): reduz 4K sem perda notável
// e evita PNGs gigantes. Retorna o padrão image2 ou vazio se nada foi gerado.
static QString renderVelocitySequence(const Clip& c, const QString& filePath,
                                      int fps, int k, int maxW) {
    const double dur = c.dur;
    if (dur <= 0.0 || fps <= 0) return QString();
    const int frameCount = std::max(1, (int)std::ceil(dur * fps));
    const QString dir = QDir::tempPath();
    const QString pattern =
        QStringLiteral("%1/pierrot-vel-%2-%05d.png").arg(dir).arg(k);

    Decoder dec;
    if (!dec.open(filePath)) return QString();

    int written = 0;
    for (int i = 0; i < frameCount; ++i) {
        const double rel = double(i) / fps;
        if (rel >= dur - 1e-9) break;
        const double srcT = clipSrcTime(c, rel);
        QImage frame = dec.frame(srcT, maxW);
        if (frame.isNull()) continue;
        const QString name =
            QStringLiteral("%1/pierrot-vel-%2-%3.png").arg(dir).arg(k)
                .arg(i + 1, 5, 10, QLatin1Char('0'));
        if (frame.save(name, "PNG")) {
            ++written;
        } else {
            qWarning() << "[export] falha ao salvar frame de velocidade" << name;
        }
    }
    if (written == 0) return QString();
    return pattern;
}

// Renderiza o texto estilizado num PNG transparente (fundo = transparência),
// aplicando a posição textX/textY do estilo SEM o transform do clipe — o
// transform (pan/zoom/rotação) é aplicado adiante pelo filter_complex na
// camada de imagem, exatamente como nas imagens estáticas. Isso substitui o
// drawtext, eliminando caixa preta/alfa incorreto e ficando consistente com o
// preview.
QImage renderTextImage(const TextStyle& st, int W, int H) {
    QImage img(W, H, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    const textrender::Layout L = textrender::layout(st, W, H);
    const QRectF box = textrender::absoluteBox(st, L, W, H);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    textrender::paint(p, st, L, box, H);
    return img;
}

struct AudioClipRef {
    const Clip* c;
    double trackVol = 1.0;
    double trackPan = 0.0; // -1..+1
    const Track* tr = nullptr;
};

QString hexColor(const QColor& col) {
    // O ffmpeg exige o prefixo "0x" (minúsculo) + dígitos hexadecimais. O
    // .toUpper() anterior deixava "0XFFFFFF", que o parser de cor rejeita
    // ("Cannot find color '0XFFFFFF'").
    return QStringLiteral("0x%1%2%3")
        .arg(col.red(), 2, 16, QLatin1Char('0'))
        .arg(col.green(), 2, 16, QLatin1Char('0'))
        .arg(col.blue(), 2, 16, QLatin1Char('0'));
}

QString codecFor(ExportSettings::Format f, bool video) {
    switch (f) {
    case ExportSettings::MP4:
    case ExportSettings::MKV:
        return video ? QStringLiteral("libx264") : QStringLiteral("aac");
    case ExportSettings::WEBM:
        return video ? QStringLiteral("libvpx-vp9") : QStringLiteral("libopus");
    case ExportSettings::WAV:
        return QStringLiteral("pcm_s16le");
    case ExportSettings::MP3:
        return QStringLiteral("libmp3lame");
    }
    return QStringLiteral("libx264");
}

// Detector (com cache por sessão) dos encoders H.264 por hardware do ffmpeg do
// sistema. Prioridade: NVENC (NVIDIA → qualquer SO) → VAAPI (Linux/Intel/AMD).
// Devolve "" quando nenhum existe (aí o export usa o libx264 normalmente).
static QString detectHwEncoder() {
    static QString cached = [] {
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), QStringList() << QStringLiteral("-encoders"));
        if (!p.waitForFinished(8000)) return QString();
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        if (out.contains(QStringLiteral("h264_nvenc")))
            return QStringLiteral("h264_nvenc");
        if (out.contains(QStringLiteral("h264_vaapi")))
            return QStringLiteral("h264_vaapi");
        return QString();
    }();
    return cached;
}

QString num(double v) {
    return QString::number(v, 'f', 3);
}

// Detector (com cache por sessão) do filtro `despill` no ffmpeg do sistema —
// existe desde o ffmpeg 4.3/2020. Sem ele, a supressão de spill fica fora do
// render: a paridade com o preview cai, mas o export continua funcionando.
static bool despillAvailable() {
    static const bool ok = [] {
        QProcess p;
        p.start(QStringLiteral("ffmpeg"), QStringList() << QStringLiteral("-filters"));
        if (!p.waitForFinished(8000)) return false;
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        return out.contains(QStringLiteral(" despill "));
    }();
    return ok;
}

// Supressão de spill no export (paridade com o preview — B-06 do
// Relatorio-Possiveis-Bugs). O preview puxa o canal da chave em direção à
// média dos outros dois, com força `spillAmt = spill * (0.35 + 0.65 *
// sino(alpha))`; no export não há alpha por pixel, então usa-se o ponto
// médio do sino (0.35 + 0.65*0.5 = 0.675) — aproximação, recalibrar se a
// diferença visual pesar.
//
// `despill` só tem type green/blue e opera por limiar: com mix=0.5 o limiar
// vira a média dos outros dois canais (= o `replacement` do preview), e a
// escala negativa do canal da chave reproduz `corrigido = ch - amt*(ch-med)`.
// Chave vermelha não tem equivalente e ties (ciano/magenta/amarelo/cinza)
// ficam de fora — mesmo critério de exclusão do preview (keyDominates).
QString despillFilter(const Clip& c) {
    const double spill = std::clamp(c.chromaKeySpillSuppress, 0.0, 1.0);
    if (spill <= 0.0) return QString();
    if (!despillAvailable()) {
        // Degradação visível: o controle existe na UI, mas o render sai sem
        // ele — avisa uma vez por sessão em vez de sumir com o efeito.
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning().noquote()
                << QStringLiteral("[export] ffmpeg sem filtro `despill`"
                                  " (>= 4.3): supressão de spill ignorada no"
                                  " render — preview e export divergem");
        }
        return QString();
    }

    // Mesmo derivador de canal do preview (PreviewWidget, bloco chroma key).
    const QColor key = c.chromaKeyColor;
    const int kv[3] = { key.red(), key.green(), key.blue() };
    const int keyCh = (kv[1] >= kv[0] && kv[1] >= kv[2]) ? 1
                    : (kv[2] >= kv[0] ? 2 : 0);
    if (!(kv[keyCh] > kv[(keyCh + 1) % 3] && kv[keyCh] > kv[(keyCh + 2) % 3]))
        return QString(); // tie: sem canal dominante, igual ao preview
    if (keyCh == 0)
        return QString(); // chave vermelha: despill não tem type=red

    const double amt = -0.675 * spill;
    if (keyCh == 1)
        return QStringLiteral("despill=type=green:mix=0.5:green=%1")
            .arg(num(amt));
    return QStringLiteral("despill=type=blue:mix=0.5:green=0:blue=%1")
        .arg(num(amt));
}

// Cadeia de filtros `equalizer` (um por banda com ganho não nulo) que
// reproduz o EQ gráfico de 10 bandas do modelo. Bandas de 1 oitava
// (width_type=o), casando com os biquads peaking do preview (Q≈1,414).
QString graphicEqFilters(const QVector<double>& gains) {
    if (gains.isEmpty()) return QString();
    QStringList out;
    const double* f = graphicEqFrequencies();
    for (int b = 0; b < kGraphicEqBands; ++b) {
        const double g =
            b < gains.size() ? std::clamp(gains[b], -12.0, 12.0) : 0.0;
        if (std::fabs(g) <= 0.01) continue;
        out << QStringLiteral("equalizer=f=%1:width_type=o:width=1:g=%2")
                   .arg(num(f[b]))
                   .arg(num(g));
    }
    return out.join(QLatin1Char(','));
}

// Cadeia de filtros do EQ paramétrico: `equalizer` (Bell), `lowshelf` /
// `highshelf` (Shelf, slope 1). Espelha os biquads do preview (EqBiquad.h).
QString parametricEqFilters(const QVector<ParametricBand>& bands) {
    if (bands.isEmpty()) return QString();
    QStringList out;
    for (const ParametricBand& b : bands) {
        if (!b.enabled || std::fabs(b.gain) <= 0.01) continue;
        const double f = std::clamp(b.freq, kParamEqMinFreq, kParamEqMaxFreq);
        const double g = std::clamp(b.gain, -kParamEqMaxGain, kParamEqMaxGain);
        if (b.type == EqLowShelf)
            out << QStringLiteral("lowshelf=f=%1:width_type=s:width=1:g=%2")
                       .arg(num(f))
                       .arg(num(g));
        else if (b.type == EqHighShelf)
            out << QStringLiteral("highshelf=f=%1:width_type=s:width=1:g=%2")
                       .arg(num(f))
                       .arg(num(g));
        else
            out << QStringLiteral("equalizer=f=%1:width_type=q:width=%2:g=%3")
                       .arg(num(f))
                       .arg(num(std::clamp(b.q, 0.1, 30.0)))
                       .arg(num(g));
    }
    return out.join(QLatin1Char(','));
}

constexpr double kPi = 3.14159265358979323846;

// Expressão ffmpeg em `t` (tempo absoluto da timeline) que reproduz a
// interpolação (linear, suave, hold ou bezier) dos keyframes a partir de
// `offset`. Suave = Catmull-Rom; bezier = handles manuais (mesmo cálculo do
// preview, em Project.h::kfValue).
QString kfExpr(const QVector<Keyframe>& keys, double base, double offset) {
    if (keys.isEmpty()) return num(base);
    // Ordena por tempo: keyframes fora de ordem geram uma expressão malformada
    // (o ffmpeg falha no setup do filtro com erro "filter graph").
    QVector<Keyframe> sorted = keys;
    std::sort(sorted.begin(), sorted.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    const QVector<Keyframe>& ks = sorted;
    QString e = QString("if(lte(t,%1),%2")
                    .arg(num(offset + ks[0].time)).arg(num(ks[0].value));
    double pt = offset + ks[0].time;
    double pv = ks[0].value;
    for (int i = 1; i < ks.size(); ++i) {
        const double t = offset + ks[i].time;
        const double v = ks[i].value;
        const double span = t - pt;
        QString seg;
        if (span <= 1e-9) {
            seg = num(v);
        } else {
            const int mode = ks[i - 1].interp;
            const QString f = QString("(t-%1)/%2").arg(num(pt)).arg(num(span));
            if (mode == KfStep) {
                seg = num(pv);
            } else if (mode == KfSmooth) {
                const Keyframe& a = ks[i - 1];
                const Keyframe& b = ks[i];
                const Keyframe& p0 = (i > 1) ? ks[i - 2] : a;
                const Keyframe& p3 = (i + 1 < ks.size()) ? ks[i + 1] : b;
                const double dt0 = (i > 1) ? (b.time - p0.time) : span;
                const double m0 = (dt0 > 1e-9) ? (b.value - p0.value) / dt0 : 0.0;
                const double dt3 = (i + 1 < ks.size()) ? (p3.time - a.time) : span;
                const double m3 = (dt3 > 1e-9) ? (p3.value - a.value) / dt3 : 0.0;
                // Mesmo ajuste do kfValue (Project.h): controle em span/3 e
                // secante/2 em par isolado — o ffmpeg reproduz o preview.
                const double iso =
                    ((i == 1) && (i + 1 >= ks.size())) ? 0.5 : 1.0;
                const double c1 = a.value + m0 * span * iso / 3.0;
                const double c2 = v - m3 * span * iso / 3.0;
                seg = QString("(1-(%1))^3*%2 + 3*(1-(%1))^2*(%1)*%3"
                              " + 3*(1-(%1))*(%1)^2*%4 + (%1)^3*%5")
                          .arg(f).arg(num(pv)).arg(num(c1)).arg(num(c2)).arg(num(v));
            } else if (mode == KfBezier || mode == KfElastic || mode == KfBounce
                       || mode == KfBack) {
                // Bezier paramétrico e easings (elastic/bounce/back) não têm
                // expressão ffmpeg fechada. Amostra a curva exata (kfValue)
                // em N sub-segmentos lineares, reproduzindo o preview.
                constexpr int N = 12;
                QVector<double> kts(N + 1), kvs(N + 1);
                for (int s = 0; s <= N; ++s) {
                    kts[s] = ks[i - 1].time + span * s / N;
                    kvs[s] = kfValue(sorted, base, kts[s]);
                }
                QString sub = QString("%1 + (%2-%1)*(t-%3)/%4")
                                  .arg(num(kvs[N - 1])).arg(num(kvs[N]))
                                  .arg(num(offset + kts[N - 1])).arg(num(kts[N] - kts[N - 1]));
                for (int s = N - 1; s >= 1; --s) {
                    const QString lin = QString("%1 + (%2-%1)*(t-%3)/%4")
                                            .arg(num(kvs[s - 1])).arg(num(kvs[s]))
                                            .arg(num(offset + kts[s - 1]))
                                            .arg(num(kts[s] - kts[s - 1]));
                    sub = QString("if(lte(t,%1),%2,%3)")
                              .arg(num(offset + kts[s])).arg(lin).arg(sub);
                }
                seg = sub;
            } else {
                const double m = (v - pv) / span;
                seg = QString("%1+%2*(t-%3)").arg(num(pv)).arg(num(m)).arg(num(pt));
            }
        }
        e += QString(",if(lte(t,%1),%2").arg(num(t)).arg(seg);
        pt = t;
        pv = v;
    }
    e += QString(",%1").arg(num(pv));
    e += QString(ks.size(), QLatin1Char(')'));
    return e;
}

double maxAbsKf(const QVector<Keyframe>& keys, double base) {
    double m = std::fabs(base);
    for (const Keyframe& k : keys) m = std::max(m, std::fabs(k.value));
    return m;
}

// Converte a curva de opacidade em fades de canal alfa. O ffmpeg só anima
// alpha via fade (rampas 0<->1), então a curva é aproximada por segmentos
// monotônicos; o pico vira multiplicador estático de colorchannelmixer.
QStringList opacityFades(const QVector<Keyframe>& keys, double base, double pos,
                         double* baseMul) {
    QStringList out;
    double mul = 1.0;
    if (keys.isEmpty()) { *baseMul = std::clamp(base, 0.0, 1.0); return out; }

    QVector<Keyframe> sorted = keys;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });

    // Verifica se todos os keyframes são lineares.
    bool allLinear = true;
    for (const Keyframe& k : sorted) {
        if (k.interp != KfLinear) { allLinear = false; break; }
    }

    if (allLinear) {
        // Abordagem original: fade=t=in/out para cada segmento linear.
        double prevT = pos;
        double prevV = std::clamp(base, 0.0, 1.0);
        for (const Keyframe& k : sorted) {
            const double t = pos + k.time;
            const double v = std::clamp(k.value, 0.0, 1.0);
            mul = std::max(mul, v);
            const double span = t - prevT;
            if (span > 0.001 && std::fabs(v - prevV) > 0.001) {
                if (v > prevV)
                    out << QStringLiteral("fade=t=in:st=%1:d=%2:alpha=1")
                              .arg(num(prevT)).arg(num(span));
                else
                    out << QStringLiteral("fade=t=out:st=%1:d=%2:alpha=1")
                              .arg(num(prevT)).arg(num(span));
            }
            prevT = t;
            prevV = v;
        }
    } else {
        // Interpolação não-linear: amostra a curva usando kfValue e gera
        // uma expressão ffmpeg if-chain piecewise.
        // Adiciona valor base antes do primeiro keyframe e depois do último.
        const double tStart = pos;
        const double tEnd = pos + sorted.last().time;

        // Amostra a cada ~2 frames para suavidade (mínimo 8 amostras).
        const double fps = 30.0;
        const int samples = qMax(8, (int)std::lround((tEnd - tStart) * fps / 2.0));
        const double dt = (samples > 1) ? (tEnd - tStart) / (samples - 1) : 0.0;

        // Constroi expressão if-chain: if(lt(t,T1),V1, if(lt(t,T2),V2, ... DEFAULT))
        QStringList parts;
        for (int i = 0; i < samples; ++i) {
            const double t = tStart + i * dt;
            const double rel = t - pos; // tempo relativo ao clipe
            const double v = std::clamp(kfValue(sorted, base, rel), 0.0, 1.0);
            mul = std::max(mul, v);
            if (i < samples - 1) {
                const double nextT = tStart + (i + 1) * dt;
                parts << QStringLiteral("if(lt(t,%1),%2").arg(num(nextT)).arg(num(v));
            } else {
                parts << num(v);
            }
        }
        // Fecha os parênteses e usa como expressão de eq=brightness.
        QString expr = parts.join(QLatin1Char(','));
        for (int i = 0; i < samples - 1; ++i) expr += QLatin1Char(')');
        // Converte opacidade (0..1) para eq brightness (-1..1): b = v - 1.
        // SEM vírgula inicial: o chamador (opacityFades loop) prepende a vírgula.
        // Com a vírgula aqui o filter_complex virava `,,eq=` → "No such filter: ''".
        const QString bExpr = QStringLiteral("(%1)-1").arg(expr);
        out << QStringLiteral("eq=brightness='%1'").arg(bExpr);
    }

    *baseMul = mul;
    return out;
}

// Cadeia de filtros atempo cujo produto é `speed`, cada um dentro de [0.5, 2].
QString atempoChain(double speed) {
    QStringList out;
    double remaining = std::clamp(speed, 0.1, 8.0);
    while (std::fabs(remaining - 1.0) > 1e-4) {
        const double f = remaining < 1.0 ? std::max(0.5, remaining)
                                         : std::min(2.0, remaining);
        out << QStringLiteral("atempo=%1").arg(num(f));
        remaining /= f;
    }
    return out.join(QLatin1Char(','));
}

// Escapa o texto para uso dentro do filtro drawtext.
QString escText(const QString& t) {
    QString s = t;
    s.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    s.replace(QLatin1Char(':'), QLatin1String("\\:"));
    s.replace(QLatin1Char('\''), QLatin1String("\\'"));
    return s;
}

// Filtro crop aplicando pan/crop (fração de cada borda), estático ou animado.
QString cropFilter(const Clip& c, double pos) {
    const QString l = kfExpr(c.kfCropL, c.cropL, pos);
    const QString r = kfExpr(c.kfCropR, c.cropR, pos);
    const QString t = kfExpr(c.kfCropT, c.cropT, pos);
    const QString b = kfExpr(c.kfCropB, c.cropB, pos);
    if (!c.kfCropL.isEmpty() || !c.kfCropR.isEmpty()
        || !c.kfCropT.isEmpty() || !c.kfCropB.isEmpty())
        // O filtro crop não tem a opção "eval" (x/y já são avaliados por
        // frame); adicionar eval=frame faz o ffmpeg falhar no setup.
        return QStringLiteral(
                   "crop=w='iw*(1-(%1)-(%2))':h='ih*(1-(%3)-(%4))':"
                   "x='iw*(%1)':y='ih*(%3)'")
                   .arg(l, r, t, b);
    return QStringLiteral("crop=w='iw*(1-(%1)-(%2))':h='ih*(1-(%3)-(%4))':"
                          "x='iw*(%1)':y='ih*(%3)'")
                .arg(num(c.cropL), num(c.cropR), num(c.cropT), num(c.cropB));
}

// Expressão de cobertura (0..1) da máscara `m` num instante da timeline,
// usando variáveis do geq (X, Y, W, H, t). Paridade com PreviewWidget::applyMasks:
//   • centro/tamanho/rotação/feather normalizados → px via W/H
//   • rotação aplicada em frame local (mesma fórmula do preview)
//   • feather por distância assinada: cov = clamp(0.5 - d/FP, 0, 1)
static QString maskCoverageExpr(const Mask& m, double pos) {
    const QString cx = kfExpr(m.kfCx, m.cx, pos);
    const QString cy = kfExpr(m.kfCy, m.cy, pos);
    const QString rx = kfExpr(m.kfRx, m.rx, pos); // normalizado → *W
    const QString ry = kfExpr(m.kfRy, m.ry, pos); // normalizado → *H
    const QString rot = kfExpr(m.kfRotation, m.rotation, pos); // graus
    const QString fe = kfExpr(m.kfFeather, m.feather, pos);    // normalizado

    const QString CX = QString("((%1)*W)").arg(cx);
    const QString CY = QString("((%1)*H)").arg(cy);
    const QString RX = QString("((%1)*W)").arg(rx);
    const QString RY = QString("((%1)*H)").arg(ry);
    const QString ra = QString("((%1)*0.0174532925)").arg(rot); // graus → rad
    const QString u = QString("(X-%1)").arg(CX);
    const QString v = QString("(Y-%1)").arg(CY);
    const QString lx = QString("((%1)*cos(%2)+(%3)*sin(%2))").arg(u).arg(ra).arg(v);
    const QString ly = QString("(-(%1)*sin(%2)+(%3)*cos(%2))").arg(u).arg(ra).arg(v);

    QString d;
    if (m.type == QLatin1String("ellipse")) {
        const QString n = QString("(pow((%1)/(%2),2)+pow((%3)/(%4),2))")
                              .arg(lx, RX, ly, RY);
        d = QString("((sqrt(max(%1,0))-1)*min(%2,%3))").arg(n, RX, RY);
    } else {
        d = QString("max(abs(%1)-%2,abs(%3)-%4)").arg(lx, RX, ly, RY);
    }
    const QString FP = QString("max((%1)*max(W,H),1)").arg(fe);
    const QString cov = QString("clamp(0.5-(%1)/(%2),0,1)").arg(d, FP);
    if (m.invert)
        return QString("(1-%1)").arg(cov);
    return cov;
}

// Cadeia geq que multiplica o ALPHA do clipe pela união (max) das máscaras.
// Vazio se o clipe não tem máscara ativa. Deve ser aplicado após format=rgba.
static QString masksFilter(const Clip& c, double pos) {
    if (!c.hasMask()) return QString();
    QString maxExpr;
    for (const Mask& m : c.masks) {
        if (!m.hasMask()) continue;
        if (m.type != QLatin1String("rect") && m.type != QLatin1String("ellipse"))
            continue; // poly: v1 não exportado — mantém paridade com o preview.
        const QString e = maskCoverageExpr(m, pos);
        if (maxExpr.isEmpty()) maxExpr = e;
        else maxExpr = QString("max(%1,%2)").arg(maxExpr, e);
    }
    if (maxExpr.isEmpty()) return QString();
    // SEM vírgula inicial: o chamador prepende a vírgula (mesma convenção de
    // despillFilter/opacityFades; a vírgula aqui gerava `,,geq=`).
    return QStringLiteral("geq=A='p(3)*clamp(%1,0,1)'").arg(maxExpr);
}

// Filtro ffmpeg para o efeito PSX (paridade com o preview). O geq avalia por
// pixel: quantiza cada canal para `bits` e soma o deslocamento do padrão de
// dithering Bayer 4x4 (o "granulado" do PlayStation 1). `format=rgba` antes
// garante canais r/g/b disponíveis independente do formato atual da cadeia.
static QString psxFilter(const Clip& c) {
    if (!c.psxEnabled) return QString();
    const int bits = std::clamp(c.psxBits, 3, 8);
    const double step = 255.0 / (1 << bits);
    const double dith = std::clamp(c.psxDither, 0.0, 1.0) * step;
    static const int bayer[16] = {
        0, 8, 2, 10,
        12, 4, 14, 6,
        3, 11, 1, 9,
        15, 7, 13, 5
    };
    const QString MX = QStringLiteral("mod(X,4)");
    const QString MY = QStringLiteral("mod(Y,4)");
    // Lookup da matriz em função de (X%4, Y%4) via `if` aninhado.
    QString lookup = QStringLiteral("if(eq(%1,3)&&eq(%2,3),%3,0)").arg(MX, MY).arg(bayer[15]);
    for (int i = 14; i >= 0; --i) {
        const int mx = i % 4, my = i / 4;
        lookup = QStringLiteral("if(eq(%1,%2)&&eq(%3,%4),%5,%6)")
                     .arg(MX).arg(mx).arg(MY).arg(my).arg(bayer[i]).arg(lookup);
    }
    const QString stepS = num(step);
    const QString off = QStringLiteral("(%1-7.5)/15.0*%2").arg(lookup).arg(num(dith));
    const QString expr = QStringLiteral("floor(clip(p(X,Y)+%1,0,255)/%2)*%3")
                             .arg(off, stepS, stepS);
    return QStringLiteral(",format=rgba,geq=r='%1':g='%2':b='%3'")
        .arg(expr, expr, expr);
}

// Filtro ffmpeg para o grão de filme (paridade com o preview). O ruído é
// monocromático (mesma força em todos os componentes) e temporal (`t`: o
// padrão muda a cada quadro) e uniforme (`u`). `alls` é a intensidade 0..100.
static QString filmGrainFilter(const Clip& c) {
    if (!c.filmGrain || c.filmGrainAmount <= 0.0) return QString();
    const double amount = std::clamp(c.filmGrainAmount, 0.0, 1.0);
    const double strength = amount * 100.0;
    // `noise=alls=<str>:allf=t+u` aplica ruído temporal uniforme em todos os
    // componentes com a mesma força (grão de luminância, não chroma).
    return QStringLiteral(",noise=alls=%1:allf=t+u").arg(num(strength));
}

// Fonte TTF disponível no sistema para o drawtext (com fallback fontconfig).
// Busca em diretórios padrão + ~/.local/share/fonts + XDG_DATA_HOME/fonts.
QString fontFilePath() {
    const QStringList candidates = {
        QStringLiteral("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/noto/NotoSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/google-noto/NotoSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttf"),
    };
    for (const QString& f : candidates)
        if (QFile::exists(f)) return f;

    // Busca genérica em diretórios de fontes conhecidos (NixOS, Arch, Void, etc.)
    const QStringList searchDirs = {
        QStringLiteral("/usr/share/fonts"),
        QStringLiteral("/usr/local/share/fonts"),
        QString::fromLocal8Bit(qgetenv("HOME")) + QStringLiteral("/.fonts"),
        QString::fromLocal8Bit(qgetenv("HOME")) + QStringLiteral("/.local/share/fonts"),
        QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME")) + QStringLiteral("/fonts"),
    };
    for (const QString& dir : searchDirs) {
        QDir d(dir);
        if (!d.exists()) continue;
        const QFileInfoList entries = d.entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot,
            QDir::Name);
        for (const QFileInfo& e : entries) {
            const QString suf = e.suffix().toLower();
            if (suf == QLatin1String("ttf") || suf == QLatin1String("otf")
                || suf == QLatin1String("ttc")) {
                const QString fname = e.completeBaseName().toLower();
                if (fname.contains(QStringLiteral("dejavusans")))
                    return e.absoluteFilePath();
            }
        }
    }
    // Último recurso: qualquer .ttf encontrado
    for (const QString& dir : searchDirs) {
        QDir d(dir);
        if (!d.exists()) continue;
        const QFileInfoList entries = d.entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot,
            QDir::Name);
        for (const QFileInfo& e : entries) {
            const QString suf = e.suffix().toLower();
            if (suf == QLatin1String("ttf") || suf == QLatin1String("otf"))
                return e.absoluteFilePath();
        }
    }
    return QString();
}

// Resolve o caminho de um arquivo de fonte para a família pedida (busca
// recursiva em /usr/share/fonts e nas pastas de fontes do usuário). Retorna
// a fonte padrão se a família não for encontrada.
QString fontFileForFamily(const QString& family) {
    if (family.isEmpty()) return fontFilePath();
    const QString safe = family.toLower().remove(kFontNameKeep);
    if (safe.isEmpty()) return fontFilePath();
    const QStringList dirs = {
        QStringLiteral("/usr/share/fonts"),
        QStringLiteral("/usr/local/share/fonts"),
        QString::fromLocal8Bit(qgetenv("HOME")) + QStringLiteral("/.fonts"),
        QString::fromLocal8Bit(qgetenv("HOME")) + QStringLiteral("/.local/share/fonts"),
        QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME")) + QStringLiteral("/fonts"),
    };
    QStringList stack;
    for (const QString& d : dirs) stack.append(d);
    QSet<QString> seen;
    while (!stack.isEmpty()) {
        const QString dir = stack.takeLast();
        if (seen.contains(dir)) continue;
        seen.insert(dir);
        QDir d(dir);
        if (!d.exists()) continue;
        const QFileInfoList entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& e : entries) {
            if (e.isDir()) {
                stack.append(e.absoluteFilePath());
            } else {
                const QString suf = e.suffix().toLower();
                if (suf == QLatin1String("ttf") || suf == QLatin1String("otf")
                    || suf == QLatin1String("ttc")) {
                    const QString fname = e.completeBaseName().toLower().remove(kFontNameKeep);
                    if (fname.contains(safe)) return e.absoluteFilePath();
                }
            }
        }
    }
    return fontFilePath();
}

// Cor no formato do drawtext: 0xRRGGBB com @alpha quando houver transparência.
QString drawColor(const QColor& c) {
    QString s = hexColor(c);
    if (c.alpha() < 255)
        s += QStringLiteral("@%1").arg(num(c.alpha() / 255.0));
    return s;
}
}
