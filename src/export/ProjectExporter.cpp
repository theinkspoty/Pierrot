// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ProjectExporter.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace {

// Imagens estáticas precisam de "-loop 1" para virarem um fluxo contínuo de
// quadros (senão o ffmpeg lê um único frame e a imagem "pisca" ou quebra o
// gráfico de filtros).
bool isImageFile(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    static const QStringList exts = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("svg")
    };
    return exts.contains(ext);
}

struct VideoClipRef {
    const Clip* c;
    const MediaItem* m;
    QString blend;
};

struct AudioClipRef {
    const Clip* c;
    double trackVol = 1.0;
};

QString hexColor(const QColor& col) {
    return QStringLiteral("0x%1%2%3")
        .arg(col.red(), 2, 16, QLatin1Char('0'))
        .arg(col.green(), 2, 16, QLatin1Char('0'))
        .arg(col.blue(), 2, 16, QLatin1Char('0'))
        .toUpper();
}

QString codecFor(ExportSettings::Format f, bool video) {
    switch (f) {
    case ExportSettings::MP4:
    case ExportSettings::MKV:
        return video ? QStringLiteral("libx264") : QStringLiteral("aac");
    case ExportSettings::WEBM:
        return video ? QStringLiteral("libvpx-vp9") : QStringLiteral("libopus");
    }
    return QStringLiteral("libx264");
}

QString num(double v) {
    return QString::number(v, 'f', 3);
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
                const double c1 = a.value + m0 * span;
                const double c2 = v - m3 * span;
                seg = QString("(1-(%1))^3*%2 + 3*(1-(%1))^2*(%1)*%3"
                              " + 3*(1-(%1))*(%1)^2*%4 + (%1)^3*%5")
                          .arg(f).arg(num(pv)).arg(num(c1)).arg(num(c2)).arg(num(v));
            } else if (mode == KfBezier) {
                // O ffmpeg não resolve o u do bezier paramétrico (posição
                // real dos handles no tempo). Amostra a curva exata (kfValue)
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
    double prevT = pos;
    double prevV = std::clamp(base, 0.0, 1.0);
    QVector<Keyframe> sorted = keys;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
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

// Fonte TTF disponível no sistema para o drawtext (com fallback fontconfig).
QString fontFilePath() {
    const QStringList candidates = {
        QStringLiteral("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/noto/NotoSans-Bold.ttf"),
        QStringLiteral("/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"),
    };
    for (const QString& f : candidates)
        if (QFile::exists(f)) return f;
    return QString();
}

}

QStringList ProjectExporter::buildCommand(const Project& project,
                                          const ExportSettings& s, QString* error) {
    QStringList args;
    const auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return QStringList();
    };

    if (s.outputPath.isEmpty())
        return fail(QStringLiteral("Caminho de saída vazio."));
    if (project.duration() <= 0)
        return fail(QStringLiteral("Timeline vazia — nada para exportar."));

    const double total = std::max(project.duration(), 0.5);
    const int W = s.width;
    const int H = s.height;
    const int FPS = s.fps;

    // Video clips bottom -> top, so the topmost track is composited last (on top).
    QVector<VideoClipRef> vclips;
    for (int tr = (int)project.videoTracks.size() - 1; tr >= 0; --tr)
        for (const Clip& c : project.videoTracks[tr].clips) {
            const MediaItem* m = project.findMedia(c.mediaId);
            if (m && m->hasVideo)
                vclips.push_back({&c, m, project.videoTracks[tr].blendMode});
        }
    std::stable_sort(vclips.begin(), vclips.end(),
                     [](const VideoClipRef& a, const VideoClipRef& b) { return a.c->pos < b.c->pos; });

    bool anySolo = false;
    for (const Track& t : project.videoTracks)
        if (t.solo) { anySolo = true; break; }
    if (!anySolo)
        for (const Track& t : project.audioTracks)
            if (t.solo) { anySolo = true; break; }

    QVector<AudioClipRef> aclips;
    for (const Track& t : project.audioTracks) {
        if (t.muted || (anySolo && !t.solo)) continue;
        for (const Clip& c : t.clips) {
            const MediaItem* m = project.findMedia(c.mediaId);
            if (m && m->hasAudio) aclips.push_back({&c, t.volume});
        }
    }
    std::sort(aclips.begin(), aclips.end(),
              [](const AudioClipRef& a, const AudioClipRef& b) { return a.c->pos < b.c->pos; });

    args << "-y" << "-hide_banner" << "-loglevel" << "info";

    // Input 0: background color. Then video clips, then audio clips.
    args << "-f" << "lavfi"
         << "-i" << QString("color=c=black:s=%1x%2:r=%3:d=%4").arg(W).arg(H).arg(FPS).arg(num(total));

    for (const VideoClipRef& v : vclips) {
        if (isImageFile(v.m->filePath)) {
            // Imagem estática: loop contínuo na taxa do projeto, limitado à
            // duração do clipe.
            args << "-loop" << "1" << "-framerate" << num(FPS)
                 << "-t" << num(v.c->dur * v.c->speed) << "-i" << v.m->filePath;
        } else {
            args << "-ss" << num(v.c->in) << "-t" << num(v.c->dur * v.c->speed)
                 << "-i" << v.m->filePath;
        }
    }

    for (const AudioClipRef& ar : aclips) {
        const Clip* c = ar.c;
        const MediaItem* m = project.findMedia(c->mediaId);
        args << "-ss" << num(c->in) << "-t" << num(c->dur * c->speed) << "-i" << m->filePath;
    }

    QStringList fc;

    // ---- Video graph ----
    QString vout = QStringLiteral("[0:v]");
    if (vclips.isEmpty()) {
        fc << QStringLiteral("[0:v]null[vout]");
    } else {
        bool anyBlend = false;
        for (const VideoClipRef& v : vclips)
            if (v.blend != QStringLiteral("normal")) { anyBlend = true; break; }
        if (anyBlend) {
            fc << QStringLiteral("[0:v]format=rgba[bg0]");
            vout = QStringLiteral("[bg0]");
        }
        int n = 0;
        for (const VideoClipRef& v : vclips) {
            ++n;
            const int inIdx = n;
            const double pos = v.c->pos;
            const double end = v.c->pos + v.c->dur;
            const double dur = v.c->dur;
            const double speed = std::max(0.1, v.c->speed);
            const double fadeIn = std::min(std::max(v.c->fadeIn, 0.0), dur);
            const double fadeOut = std::min(std::max(v.c->fadeOut, 0.0), dur - fadeIn);
            const QString lbl = QStringLiteral("v%1").arg(n);
            const QString outLbl =
                (n == (int)vclips.size()) ? QStringLiteral("vout") : QStringLiteral("m%1").arg(n);
            const bool hasT = v.c->hasTransform();
            const QString scaleExpr = kfExpr(v.c->kfScale, v.c->scale, pos);
            const QString rotExpr = kfExpr(v.c->kfRotation, v.c->rotation, pos);
            const QString txExpr = kfExpr(v.c->kfTx, v.c->tx, pos);
            const QString tyExpr = kfExpr(v.c->kfTy, v.c->ty, pos);
            QString chain = QStringLiteral("[%1:v]setpts=(PTS-STARTPTS)/%2+%3/TB,trim=duration=%4")
                                .arg(inIdx).arg(num(speed)).arg(num(pos)).arg(num(dur));
            if (v.c->hasCrop()) {
                // Com crop, o conteúdo recortado preenche o quadro (cover).
                chain += QLatin1Char(',') + cropFilter(*v.c, pos);
                chain += QStringLiteral(
                             ",scale=%1:%2:force_original_aspect_ratio=increase:"
                             "force_divisible_by=2,crop=%1:%2,fps=%3,format=rgba")
                             .arg(W).arg(H).arg(FPS);
            } else {
                chain += QStringLiteral(
                             ",scale=%1:%2:force_original_aspect_ratio=decrease:"
                             "force_divisible_by=2,"
                             "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=black,fps=%3,format=rgba")
                             .arg(W).arg(H).arg(FPS);
            }
            fc << chain;
            // Transformação (zoom, movimento, rotação) — antes dos efeitos.
            if (v.c->scale != 1.0 || !v.c->kfScale.isEmpty()) {
                if (v.c->kfScale.isEmpty())
                    fc.last().append(QStringLiteral(",scale=w=iw*%1:h=ih*%1")
                                         .arg(num(v.c->scale)));
                else
                    fc.last().append(QStringLiteral(",scale=w='iw*(%1)':h='ih*(%1)':eval=frame")
                                         .arg(scaleExpr));
            }
            if (v.c->rotation != 0.0 || !v.c->kfRotation.isEmpty()) {
                const double maxS = std::max(1.0, maxAbsKf(v.c->kfScale, v.c->scale));
                const double aRad = maxAbsKf(v.c->kfRotation, v.c->rotation) * kPi / 180.0;
                const double cAng = std::cos(aRad);
                const double sAng = std::sin(aRad);
                const int rotW = std::max(2, (int)std::ceil(W * maxS * cAng + H * maxS * sAng));
                const int rotH = std::max(2, (int)std::ceil(W * maxS * sAng + H * maxS * cAng));
                if (v.c->kfRotation.isEmpty())
                    fc.last().append(QStringLiteral(",rotate=a=%1:ow=%2:oh=%3:c=black")
                                         .arg(num(v.c->rotation * kPi / 180.0)).arg(rotW).arg(rotH));
                else
                    fc.last().append(QStringLiteral(",rotate=a='%1':ow=%2:oh=%3:c=black")
                                         .arg(rotExpr).arg(rotW).arg(rotH));
            }
            if (fadeIn > 0)
                fc.last().append(QStringLiteral(",fade=t=in:st=%1:d=%2")
                                     .arg(num(pos)).arg(num(fadeIn)));
            if (fadeOut > 0)
                fc.last().append(QStringLiteral(",fade=t=out:st=%1:d=%2")
                                     .arg(num(end - fadeOut)).arg(num(fadeOut)));
            if (v.c->chromaKey)
                fc.last().append(QStringLiteral(",chromakey=color=%1:similarity=%2:blend=0.1")
                                     .arg(hexColor(v.c->chromaKeyColor))
                                     .arg(num(std::clamp(v.c->chromaKeySimilarity, 0.0, 1.0))));
            if (v.c->brightness != 0.0 || v.c->contrast != 1.0 || v.c->saturation != 1.0)
                fc.last().append(QStringLiteral(",eq=brightness=%1:contrast=%2:saturation=%3")
                                     .arg(num(std::clamp(v.c->brightness, -1.0, 1.0)))
                                     .arg(num(std::clamp(v.c->contrast, 0.0, 2.0)))
                                     .arg(num(std::clamp(v.c->saturation, 0.0, 2.0))));
            if (v.c->grayscale)
                fc.last().append(QStringLiteral(
                    ",colorchannelmixer=rr=0.299:rg=0.587:rb=0.114"
                    ":gr=0.299:gg=0.587:gb=0.114"
                    ":br=0.299:bg=0.587:bb=0.114"));
            if (v.c->blur > 0.0)
                fc.last().append(QStringLiteral(",boxblur=luma_radius=%1:luma_power=2")
                                     .arg(num(std::clamp(v.c->blur, 0.0, 40.0))));
            if (!v.c->text.isEmpty()) {
                const QString font = fontFilePath();
                QString dt = QStringLiteral("drawtext=text='%1':fontsize=h/18:fontcolor=white"
                                            ":box=1:boxcolor=black@0.6:boxborderw=10"
                                            ":x=(w-text_w)/2:y=(h-text_h)/2")
                                 .arg(escText(v.c->text));
                if (!font.isEmpty())
                    dt.prepend(QStringLiteral("fontfile=%1:").arg(font));
                fc.last().append(QLatin1Char(',') + dt);
            }
            if (v.c->kfOpacity.isEmpty()) {
                if (v.c->opacity < 1.0)
                    fc.last().append(QStringLiteral(",colorchannelmixer=aa=%1")
                                         .arg(num(std::clamp(v.c->opacity, 0.0, 1.0))));
            } else {
                double mul = 1.0;
                const QStringList fades = opacityFades(v.c->kfOpacity, v.c->opacity, pos, &mul);
                for (const QString& f : fades)
                    fc.last().append(QLatin1Char(',') + f);
                if (mul < 1.0)
                    fc.last().append(QStringLiteral(",colorchannelmixer=aa=%1").arg(num(mul)));
            }
            fc.last().append(QStringLiteral("[%1]").arg(lbl));
            if (hasT) {
                fc << QStringLiteral("%1[%2]overlay=x='main_w/2-overlay_w/2+%3':y='main_h/2-overlay_h/2+%4':eof_action=pass:enable='between(t,%5,%6)':eval=frame[%7]")
                           .arg(vout, lbl).arg(txExpr).arg(tyExpr).arg(num(pos)).arg(num(end)).arg(outLbl);
            } else if (v.blend == QStringLiteral("normal")) {
                fc << QStringLiteral("%1[%2]overlay=0:0:eof_action=pass:enable='between(t,%3,%4)':eval=frame[%5]")
                           .arg(vout, lbl).arg(num(pos)).arg(num(end)).arg(outLbl);
            } else {
                const QString rs = QStringLiteral("rs%1").arg(n);
                const QString cs = QStringLiteral("cs%1").arg(n);
                const QString bs = QStringLiteral("bs%1").arg(n);
                const QString pl = QStringLiteral("pl%1").arg(n);
                fc << QStringLiteral("[%1]trim=start=%2:duration=%3,setpts=PTS-STARTPTS,format=rgba[%4]")
                           .arg(vout.mid(1, vout.size() - 2), num(pos), num(dur), rs);
                fc << QStringLiteral("[%1]setpts=PTS-STARTPTS[%2]").arg(lbl, cs);
                fc << QStringLiteral("[%1][%2]blend=c0_mode=%3:c0_opacity=1:c1_mode=%3:c1_opacity=1"
                                     ":c2_mode=%3:c2_opacity=1:c3_mode=normal:c3_opacity=1[%4]")
                           .arg(rs, cs, v.blend, bs);
                fc << QStringLiteral("[%1]setpts=PTS-STARTPTS+%2/TB[%3]")
                           .arg(bs).arg(num(pos)).arg(pl);
                fc << QStringLiteral("%1[%2]overlay=0:0:eof_action=pass:enable='between(t,%3,%4)':eval=frame[%5]")
                           .arg(vout, pl).arg(num(pos)).arg(num(end)).arg(outLbl);
            }
            vout = QStringLiteral("[%1]").arg(outLbl);
        }
    }

    // ---- Audio graph ----
    QString aout;
    if (!aclips.isEmpty()) {
        QStringList albl;
        int n = 0;
        for (const AudioClipRef& ar : aclips) {
            ++n;
            const Clip* c = ar.c;
            const int inIdx = 1 + (int)vclips.size() + (n - 1);
            const MediaItem* m = project.findMedia(c->mediaId);
            const QString lbl = QStringLiteral("a%1").arg(n);
            const double dur = c->dur;
            const double speed = std::max(0.1, c->speed);
            const double fadeIn = std::min(std::max(c->fadeIn, 0.0), dur);
            const double fadeOut = std::min(std::max(c->fadeOut, 0.0), dur - fadeIn);
            const qint64 delayMs = (qint64)llround(c->pos * 1000.0);

            QString src = QStringLiteral("[%1:a]").arg(inIdx);
            if (m && m->audioStreams > 1) {
                // Mixa todas as faixas de áudio do arquivo (mkv/mp4/mov multifaixa).
                QStringList mixIn;
                const QString mixLbl = QStringLiteral("amix%1").arg(n);
                for (int k = 0; k < m->audioStreams; ++k) {
                    const QString s = QStringLiteral("s%1_%2").arg(n).arg(k);
                    fc << QStringLiteral("[%1:a:%2]aformat=sample_fmts=fltp:"
                                        "channel_layouts=stereo,aresample=%3[%4]")
                           .arg(inIdx).arg(k).arg((qint64)project.audioRate).arg(s);
                    mixIn << QStringLiteral("[%1]").arg(s);
                }
                fc << mixIn.join(QString())
                    + QStringLiteral("amix=inputs=%1:duration=first:dropout_transition=0[%2]")
                          .arg(mixIn.size()).arg(mixLbl);
                src = QStringLiteral("[%1]").arg(mixLbl);
            }

            // O input já vem com -ss in -t dur (frames começando em 0 relativo);
            // usar atrim=start=in descartaria todo o áudio dos cortes (in > 0).
            // atrim=duration basta para garantir a duração exata.
            fc << src + QStringLiteral("atrim=duration=%1,asetpts=PTS-STARTPTS")
                             .arg(num(dur * speed));
            if (std::fabs(speed - 1.0) > 1e-4)
                fc.last().append(QLatin1Char(',') + atempoChain(speed));
            if (fadeIn > 0)
                fc.last().append(QStringLiteral(",afade=t=in:st=0:d=%1").arg(num(fadeIn)));
            if (fadeOut > 0)
                fc.last().append(QStringLiteral(",afade=t=out:st=%1:d=%2")
                                     .arg(num(dur - fadeOut)).arg(num(fadeOut)));
            fc.last().append(QStringLiteral(",adelay=%1:all=1").arg(delayMs));
            // Lógica Vegas: volume efetivo = envelope do clipe (relativo, base 1.0)
            // × volume do clipe × volume da faixa, limitado a 200% como no preview.
            // O preview aplica o mesmo produto (PreviewWidget::buildMixSources).
            const double staticGain =
                std::clamp(c->volume * ar.trackVol, 0.0, 2.0);
            if (!c->kfVolume.isEmpty()) {
                fc.last().append(QStringLiteral(",volume='clip(%1*%2,0,2)'")
                                     .arg(kfExpr(c->kfVolume, 1.0, c->pos))
                                     .arg(num(staticGain)));
            } else if (std::fabs(staticGain - 1.0) > 1e-4) {
                fc.last().append(QStringLiteral(",volume=%1").arg(num(staticGain)));
            }
            if (c->denoise)
                fc.last().append(QStringLiteral(",afftdn=nr=%1")
                                     .arg(num(std::clamp(c->denoiseAmount, 1.0, 50.0))));
            if (std::fabs(c->eqLow) > 0.01)
                fc.last().append(QStringLiteral(",equalizer=f=120:width_type=q:width=1:g=%1")
                                     .arg(num(std::clamp(c->eqLow, -12.0, 12.0))));
            if (std::fabs(c->eqMid) > 0.01)
                fc.last().append(QStringLiteral(",equalizer=f=1000:width_type=q:width=1:g=%1")
                                     .arg(num(std::clamp(c->eqMid, -12.0, 12.0))));
            if (std::fabs(c->eqHigh) > 0.01)
                fc.last().append(QStringLiteral(",equalizer=f=6000:width_type=q:width=1:g=%1")
                                     .arg(num(std::clamp(c->eqHigh, -12.0, 12.0))));
            fc.last().append(QStringLiteral(",aformat=sample_fmts=fltp:channel_layouts=stereo,"
                                            "aresample=%1")
                                 .arg((qint64)project.audioRate));
            if (c->normalize)
                fc.last().append(QStringLiteral(",loudnorm=I=-14:TP=-1.5:LRA=11"));
            if (c->invertPhase)
                fc.last().append(QStringLiteral(",aeval=-val(0)|-val(1)"));
            fc.last().append(QStringLiteral("[%1]").arg(lbl));
            albl << lbl;
        }
        if (albl.size() == 1) {
            aout = QStringLiteral("[%1]").arg(albl[0]);
        } else {
            // Cada rótulo precisa dos colchetes ([a1][a2]...) senão o ffmpeg
            // interpreta o nome inteiro como um filtro inexistente.
            QString aIn;
            for (const QString& l : albl) aIn += QStringLiteral("[%1]").arg(l);
            fc << aIn
                + QStringLiteral(
                      "amix=inputs=%1:duration=longest:dropout_transition=0[aout]")
                      .arg(albl.size());
            aout = QStringLiteral("[aout]");
        }
    }

    args << "-filter_complex" << fc.join(QLatin1Char(';'));

    args << "-map" << QStringLiteral("[vout]");
    if (!aout.isEmpty()) args << "-map" << aout;
    else args << "-an";

    args << "-c:v" << codecFor(s.format, true);
    if (s.format == ExportSettings::WEBM) {
        args << "-crf" << "32" << "-b:v" << "0" << "-cpu-used" << "4";
    } else {
        args << "-preset" << "medium" << "-crf" << "18";
    }
    args << "-pix_fmt" << "yuv420p";

    if (!aout.isEmpty()) {
        args << "-c:a" << codecFor(s.format, false);
        args << "-b:a" << (s.format == ExportSettings::WEBM ? "128k" : "192k");
    }

    args << "-max_muxing_queue_size" << "1024";
    args << "-r" << QString::number(FPS);
    if (s.format == ExportSettings::MP4) args << "-movflags" << "+faststart";
    args << s.outputPath;

    if (error) error->clear();
    return args;
}
