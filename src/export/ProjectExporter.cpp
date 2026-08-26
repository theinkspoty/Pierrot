// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ProjectExporter.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QDir>
#include <QSet>
#include <QRegularExpression>
#include <QDebug>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QTextStream>
#include <QTemporaryFile>

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
    double trackOpacity = 1.0; // opacidade da faixa (0..1, estilo Vegas/FCE)
};

// Renderiza o texto estilizado num PNG transparente (fundo = transparência),
// aplicando a posição textX/textY do estilo SEM o transform do clipe — o
// transform (pan/zoom/rotação) é aplicado adiante pelo filter_complex na
// camada de imagem, exatamente como nas imagens estáticas. Isso substitui o
// drawtext, eliminando caixa preta/alfa incorreto e ficando consistente com o
// preview.
QImage renderTextImage(const TextStyle& st, int W, int H) {
    QImage img(W, H, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    if (st.text.trimmed().isEmpty()) return img;
    const double sizeFrac = st.textSize > 0.0 ? st.textSize : (1.0 / 18.0);
    const int pxSize = qMax(4, (int)qRound(sizeFrac * H));
    QFont font;
    if (!st.fontFamily.isEmpty()) font.setFamily(st.fontFamily);
    font.setPixelSize(pxSize);
    font.setBold(st.textBold);
    const QFontMetricsF fm(font);

    // Quebra em linhas dentro de 90% da largura.
    const double maxW = W * 0.9;
    QStringList wrapped;
    for (const QString& raw : st.text.split(QLatin1Char('\n'))) {
        if (raw.isEmpty()) { wrapped << QString(); continue; }
        QString cur;
        const QStringList words = raw.split(QLatin1Char(' '));
        for (const QString& w : words) {
            const QString trial = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
            if (fm.horizontalAdvance(trial) <= maxW || cur.isEmpty())
                cur = trial;
            else { wrapped << cur; cur = w; }
        }
        wrapped << cur;
    }
    double tw = 0.0;
    for (const QString& l : wrapped) tw = qMax(tw, fm.horizontalAdvance(l));
    const double th = wrapped.size() * fm.height();
    const double pad = pxSize * 0.25;
    const double bw = tw + 2.0 * pad;
    const double bh = th + 2.0 * pad;
    double x;
    if (st.textAlign == 1) x = st.textX * W;
    else if (st.textAlign == 2) x = st.textX * W - bw;
    else x = st.textX * W - bw / 2.0;
    const double y = st.textY * H - bh / 2.0;
    const QRectF box(x, y, bw, bh);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    if (st.textBackground) p.fillRect(box, st.textBackgroundColor);
    QPainterPath path;
    const double baseline = box.top() + pad + fm.ascent();
    for (int i = 0; i < wrapped.size(); ++i)
        path.addText(QPointF(box.left() + pad, baseline + i * fm.height()), font, wrapped[i]);
    if (st.textOutline > 0.0) {
        QPen pen(st.textOutlineColor, qMax(1.0, st.textOutline * H),
                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(path, pen);
    }
    p.fillPath(path, st.textColor);
    return img;
}

struct AudioClipRef {
    const Clip* c;
    double trackVol = 1.0;
    double trackPan = 0.0; // -1..+1
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
        const double baseV = std::clamp(base, 0.0, 1.0);

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
        const QString bExpr = QStringLiteral("(%1)-1").arg(expr);
        out << QStringLiteral(",eq=brightness='%1'").arg(bExpr);
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

// Resolve o caminho de um arquivo de fonte para a família pedida (busca
// recursiva em /usr/share/fonts e nas pastas de fontes do usuário). Retorna
// a fonte padrão se a família não for encontrada.
QString fontFileForFamily(const QString& family) {
    if (family.isEmpty()) return fontFilePath();
    static const QRegularExpression keep(QStringLiteral("[^a-z0-9]"));
    const QString safe = family.toLower().remove(keep);
    if (safe.isEmpty()) return fontFilePath();
    const QStringList dirs = {
        QStringLiteral("/usr/share/fonts"),
        QStringLiteral("/usr/local/share/fonts"),
        QString::fromLocal8Bit(qgetenv("HOME")) + QStringLiteral("/.fonts"),
        QString::fromLocal8Bit(qgetenv("HOME")) + QStringLiteral("/.local/share/fonts"),
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
                    const QString fname = e.completeBaseName().toLower().remove(keep);
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

    // Limpa PNGs de texto temporários de exportações anteriores.
    const QDir tmp(QDir::tempPath());
    for (const QFileInfo& fi : tmp.entryInfoList(QStringList{QStringLiteral("pierrot-text-*.png")},
                                                 QDir::Files))
        QFile::remove(fi.absoluteFilePath());

    const double total = std::max(project.duration(), 0.5);
    const int W = s.width;
    const int H = s.height;
    const int FPS = s.fps;

    // Video clips bottom -> top, so the topmost track is composited last (on top).
    QVector<VideoClipRef> vclips;
    // Caminho temporário do PNG gerado para cada clipe de texto (índice = mesmo
    // de vclips); vazio para clipes de mídia. Preenchido no loop de inputs.
    QVector<QString> textPngs;
    for (int tr = (int)project.videoTracks.size() - 1; tr >= 0; --tr) {
        const double trOp = std::clamp(project.videoTracks[tr].opacity, 0.0, 1.0);
        for (const Clip& c : project.videoTracks[tr].clips) {
            if (c.isText) {
                // Clipe independente de texto: vira uma camada gerada.
                vclips.push_back({&c, nullptr, project.videoTracks[tr].blendMode, trOp});
                textPngs.push_back(QString());
                continue;
            }
            const MediaItem* m = project.findMedia(c.mediaId);
            if (m && m->hasVideo) {
                vclips.push_back({&c, m, project.videoTracks[tr].blendMode, trOp});
                textPngs.push_back(QString());
            }
        }
    }
    std::stable_sort(vclips.begin(), vclips.end(),
                     [](const VideoClipRef& a, const VideoClipRef& b) { return a.c->pos < b.c->pos; });
    // Mantém textPngs alinhado a vclips reordenado.
    {
        QVector<QString> reordered(textPngs.size());
        QVector<int> order(vclips.size());
        for (int i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return vclips[a].c->pos < vclips[b].c->pos; });
        for (int i = 0; i < (int)vclips.size(); ++i) reordered[i] = textPngs[order[i]];
        textPngs = reordered;
    }

    // Transições: para cada clipe de vídeo, verifica se ele se sobrepõe ao
    // clipe anterior da MESMA faixa. A duração é o tamanho da sobreposição e o
    // tipo vem do clipe de trás (transição de saída; vazio = dissolve).
    // clipTrans[id do clipe seguinte] = {tipo, duração}.
    QHash<QString, QPair<QString, double>> clipTrans;
    for (const Track& tr : project.videoTracks) {
        QVector<const Clip*> sorted;
        for (const Clip& c : tr.clips) sorted.push_back(&c);
        std::sort(sorted.begin(), sorted.end(),
                  [](const Clip* a, const Clip* b) { return a->pos < b->pos; });
        for (int i = 1; i < (int)sorted.size(); ++i) {
            const Clip* prev = sorted[i - 1];
            const Clip* cur = sorted[i];
            const double d = prev->pos + prev->dur - cur->pos;
            if (d > 1e-6) {
                const QString type = isTransition(prev->transitionType)
                                         ? prev->transitionType
                                         : QStringLiteral("dissolve");
                clipTrans.insert(cur->id, qMakePair(type, d));
            }
        }
    }

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
            if (m && m->hasAudio) aclips.push_back({&c, t.volume, t.pan});
        }
    }
    std::sort(aclips.begin(), aclips.end(),
              [](const AudioClipRef& a, const AudioClipRef& b) { return a.c->pos < b.c->pos; });

    // Crossfade de áudio: clipes de áudio que se sobrepõem na MESMA faixa
    // ganham fade-in (o da frente) e fade-out (o de trás) sobre a sobreposição,
    // acompanhando a transição de vídeo. audioFades[id] = {fadeIn, fadeOut}.
    QHash<QString, QPair<double, double>> audioFades;
    for (const Track& tr : project.audioTracks) {
        QVector<const Clip*> sorted;
        for (const Clip& c : tr.clips) sorted.push_back(&c);
        std::sort(sorted.begin(), sorted.end(),
                  [](const Clip* a, const Clip* b) { return a->pos < b->pos; });
        for (int i = 1; i < (int)sorted.size(); ++i) {
            const double d = sorted[i - 1]->pos + sorted[i - 1]->dur - sorted[i]->pos;
            if (d > 1e-6) {
                auto& fi = audioFades[sorted[i]->id];
                fi.first = std::max(fi.first, std::min(d, sorted[i]->dur));
                auto& fo = audioFades[sorted[i - 1]->id];
                fo.second = std::max(fo.second, std::min(d, sorted[i - 1]->dur));
            }
        }
    }

    args << "-y" << "-hide_banner" << "-loglevel" << "info";

    // Input 0: background color. Then video clips, then audio clips.
    args << "-f" << "lavfi"
         << "-i" << QString("color=c=black:s=%1x%2:r=%3:d=%4").arg(W).arg(H).arg(FPS).arg(num(total));

    for (int i = 0; i < (int)vclips.size(); ++i) {
        const VideoClipRef& v = vclips[i];
        if (v.m == nullptr) {
            // Clipe de texto: gera um PNG transparente com o texto estilizado
            // (como mídia), eliminando o drawtext — fonte/caixa/alfa ficam
            // consistentes com o preview e com as imagens estáticas (que não
            // têm o problema de fundo).
            const TextStyle& st = *project.textStyleFor(*v.c);
            QImage img = renderTextImage(st, W, H);
            QTemporaryFile tmp;
            tmp.setAutoRemove(false);
            tmp.setFileTemplate(QDir::tempPath() + "/pierrot-text-XXXXXX.png");
            if (tmp.open()) {
                img.save(tmp.fileName(), "PNG");
                textPngs[i] = tmp.fileName();
                args << "-loop" << "1" << "-framerate" << num(FPS)
                     << "-t" << num(v.c->dur * v.c->speed) << "-i" << textPngs[i];
            } else {
                // Fallback: fonte transparente vazia (nunca deve ocorrer).
                args << "-f" << "lavfi"
                     << "-i" << QString("color=c=black@0.0:s=%1x%2:r=%3:d=%4")
                                    .arg(W).arg(H).arg(FPS).arg(num(v.c->dur * v.c->speed));
            }
        } else if (v.m->isSolid) {
            // Cor sólida (gerador estilo Vegas): input lavfi com a cor sólida.
            args << "-f" << "lavfi"
                 << "-i" << QString("color=c=%1:s=%2x%3:r=%4:d=%5")
                                .arg(hexColor(v.m->solidColor))
                                .arg(v.m->width > 0 ? v.m->width : W)
                                .arg(v.m->height > 0 ? v.m->height : H)
                                .arg(FPS)
                                .arg(num(v.c->dur * v.c->speed));
        } else if (isImageFile(v.m->filePath)) {
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
            // Mídia transparente (texto gerado ou PNG/WebP com alpha). Todo
            // redimensionamento do ffmpeg (o enquadramento contain, o flatt
            // X/Y e a rotação) interpola RGB com alpha "freio" (straight), e o
            // RGB preto sob os pixels transparentes vaza para as bordas —
            // franja escura em volta da transparência (pior ainda ao reduzir e
            // depois esticar). Para essa mídia, pré-multiplicamos o alpha logo
            // no início da cadeia e desfazemos depois de toda a geometria.
            const bool alphaMedia = (v.m == nullptr)
                || (v.m && isImageFile(v.m->filePath));
            bool premultiplied = false;
            const QString scaleExpr = kfExpr(v.c->kfScale, v.c->scale, pos);
            const QString rotExpr = kfExpr(v.c->kfRotation, v.c->rotation, pos);
            const QString txExpr = kfExpr(v.c->kfTx, v.c->tx, pos);
            const QString tyExpr = kfExpr(v.c->kfTy, v.c->ty, pos);
            const auto trIt = clipTrans.find(v.c->id);
            const bool hasTrans = trIt != clipTrans.end();
            const QString transType = hasTrans ? trIt->first : QString();
            const double transDur = hasTrans ? trIt->second : 0.0;
            QString chain;
            if (v.m == nullptr) {
                // Clipe de texto (PNG transparente gerado): força cadência fixa
                // e timebase antes de reposicionar. O PNG via -loop gerava PTS
                // com passo > 1/30, fazendo o `t` avançar rápido e a animação
                // ficar ~N× mais rápida no render (so texto, onde há keyframes).
                chain = QStringLiteral("[%1:v]fps=%2,settb=AVTB,setpts=(PTS-STARTPTS)/%3+%4/TB,"
                                       "trim=duration=%5,format=rgba")
                            .arg(inIdx).arg(FPS).arg(num(speed)).arg(num(pos)).arg(num(dur));
                if (alphaMedia) {
                    chain += QStringLiteral(",premultiply=inplace=1");
                    premultiplied = true;
                }
            } else {
                chain = QStringLiteral("[%1:v]setpts=(PTS-STARTPTS)/%2+%3/TB,trim=duration=%4")
                            .arg(inIdx).arg(num(speed)).arg(num(pos)).arg(num(dur));
                if (alphaMedia) {
                    // Pré-multiplica ANTES do enquadramento: o downscale do
                    // contain é o maior gerador da franja escura.
                    chain += QStringLiteral(",format=rgba,premultiply=inplace=1");
                    premultiplied = true;
                }
                // Mídia sem fundo (PNG/WebP com alpha): o letterbox usa
                // TRANSPARENTE para não cobrir o que está por baixo. Vídeos/
                // imagens opacas usam preto (letterbox clássico).
                const bool alphaMedia = v.m && isImageFile(v.m->filePath);
                const QString padColor = alphaMedia ? QStringLiteral("black@0")
                                                    : QStringLiteral("black");
                if (v.c->hasCrop()) {
                    // Com crop, o conteúdo recortado é encaixado no quadro com
                    // CONTAIN (letterbox), igual ao preview. Antes usávamos
                    // cover (increase + crop), que RECORTAVA imagens não-16:9
                    // (quadradas/retrato) — o render cortava o que o preview
                    // mostrava inteiro.
                    chain += QLatin1Char(',') + cropFilter(*v.c, pos);
                    chain += QStringLiteral(
                                 ",scale=%1:%2:force_original_aspect_ratio=decrease:"
                                 "force_divisible_by=2,"
                                 "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=%3,fps=%4,format=rgba")
                                 .arg(W).arg(H).arg(padColor).arg(FPS);
                } else {
                    chain += QStringLiteral(
                                 ",scale=%1:%2:force_original_aspect_ratio=decrease:"
                                 "force_divisible_by=2,"
                                 "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=%3,fps=%4,format=rgba")
                                 .arg(W).arg(H).arg(padColor).arg(FPS);
                }
            }
            fc << chain;
            // Texto (do clipe de texto ou anexado ao vídeo): desenhado AQUI,
            // antes de scale/rotate/fade/opacity, para o texto acompanhar a
            // transformação e os fades do clipe.
            // Apenas clipes de MÍDIA com texto anexado (não clipes de texto
            // independentes — esses já vêm renderizados no PNG gerado acima).
            if (v.m != nullptr && !project.textStyleFor(*v.c)->isEmpty()) {
                const TextStyle& st = *project.textStyleFor(*v.c);
                const double sizeFrac = st.textSize > 0.0 ? st.textSize : (1.0 / 18.0);
                const QString font = fontFileForFamily(st.fontFamily);
                // O fontfile precisa ser a PRIMEIRA opção dentro do filtro
                // drawtext (drawtext=fontfile=...:text=...): colocar fora,
                // como "fontfile=...:drawtext=...", quebra o parser do ffmpeg.
                QString dt = QStringLiteral("drawtext=");
                if (!font.isEmpty()) {
                    // Escapa ':' e ',' no caminho (separadores do formato).
                    QString fpath = font;
                    fpath.replace(QLatin1Char(':'), QLatin1String("\\:"));
                    fpath.replace(QLatin1Char(','), QLatin1String("\\,"));
                    dt += QStringLiteral("fontfile=%1:").arg(fpath);
                }
                dt += QStringLiteral("text='%1':fontsize=h*%2")
                          .arg(escText(st.text))
                          .arg(num(sizeFrac));
                dt += QStringLiteral(":fontcolor=%1").arg(drawColor(st.textColor));
                if (st.textOutline > 0.0)
                    dt += QStringLiteral(":borderw=h*%1:bordercolor=%2")
                              .arg(num(st.textOutline))
                              .arg(drawColor(st.textOutlineColor));
                if (st.textBackground)
                    dt += QStringLiteral(":box=1:boxcolor=%1:boxborderw=%2")
                              .arg(drawColor(st.textBackgroundColor))
                              .arg(qMax(4, (int)llround(W * 0.008)));
                QString xExpr;
                if (st.textAlign == 1) xExpr = QStringLiteral("w*%1").arg(num(st.textX));
                else if (st.textAlign == 2) xExpr = QStringLiteral("w*%1-text_w").arg(num(st.textX));
                else xExpr = QStringLiteral("w*%1-text_w/2").arg(num(st.textX));
                const QString yExpr = QStringLiteral("h*%1-text_h/2").arg(num(st.textY));
                dt += QStringLiteral(":x='%1':y='%2'").arg(xExpr, yExpr);
                fc.last().append(QLatin1Char(',') + dt);
            }
            // Transformação (zoom, movimento, rotação) — antes dos efeitos.
            // Escala efetiva em cada eixo = scale (uniforme) × scaleX/scaleY
            // (achatamento não-uniforme). Sempre que houver escala não uniforme
            // aplicamos X e Y separadamente.
            const bool xStretch = v.c->scaleX != 1.0 || !v.c->kfScaleX.isEmpty();
            const bool yStretch = v.c->scaleY != 1.0 || !v.c->kfScaleY.isEmpty();
            if (v.c->scale != 1.0 || !v.c->kfScale.isEmpty() || xStretch || yStretch) {
                const QString sxExpr =
                    (v.c->kfScale.isEmpty()
                         ? QStringLiteral("%1").arg(num(v.c->scale))
                         : QStringLiteral("(%1)").arg(scaleExpr))
                    + (v.c->kfScaleX.isEmpty()
                           ? QStringLiteral("*%1").arg(num(v.c->scaleX))
                           : QStringLiteral("*(%1)").arg(kfExpr(v.c->kfScaleX, v.c->scaleX, pos)));
                const QString syExpr =
                    (v.c->kfScale.isEmpty()
                         ? QStringLiteral("%1").arg(num(v.c->scale))
                         : QStringLiteral("(%1)").arg(scaleExpr))
                    + (v.c->kfScaleY.isEmpty()
                           ? QStringLiteral("*%1").arg(num(v.c->scaleY))
                           : QStringLiteral("*(%1)").arg(kfExpr(v.c->kfScaleY, v.c->scaleY, pos)));
                const bool hasAnim = !v.c->kfScale.isEmpty()
                    || xStretch || yStretch;
                if (hasAnim)
                    fc.last().append(QStringLiteral(",scale=w='iw*(%1)':h='ih*(%2)':eval=frame")
                                         .arg(sxExpr, syExpr));
                else
                    fc.last().append(QStringLiteral(",scale=w=iw*%1:h=ih*%2")
                                         .arg(num(v.c->scale * v.c->scaleX),
                                              num(v.c->scale * v.c->scaleY)));
            }
            if (v.c->rotation != 0.0 || !v.c->kfRotation.isEmpty()) {
                // Texto: a imagem já é W×H e o preview RECORTA a rotação ao
                // quadro do projeto (clipe). Usar ow=W:oh=H mantém o texto
                // centrado e rotacionando em torno do centro, igual ao preview.
                // Para vídeo/imagem, o canvas ampliado evita cortar as quinas.
                const int rotW = (v.m == nullptr) ? W : [&]() {
                    const double maxX = std::max(1.0, maxAbsKf(v.c->kfScale, v.c->scale)
                                                      * maxAbsKf(v.c->kfScaleX, v.c->scaleX));
                    const double maxY = std::max(1.0, maxAbsKf(v.c->kfScale, v.c->scale)
                                                      * maxAbsKf(v.c->kfScaleY, v.c->scaleY));
                    const double aRad = maxAbsKf(v.c->kfRotation, v.c->rotation) * kPi / 180.0;
                    const double cAng = std::fabs(std::cos(aRad));
                    const double sAng = std::fabs(std::sin(aRad));
                    return std::max(2, (int)std::ceil(W * maxX * cAng + H * maxY * sAng));
                }();
                const int rotH = (v.m == nullptr) ? H : [&]() {
                    const double maxX = std::max(1.0, maxAbsKf(v.c->kfScale, v.c->scale)
                                                      * maxAbsKf(v.c->kfScaleX, v.c->scaleX));
                    const double maxY = std::max(1.0, maxAbsKf(v.c->kfScale, v.c->scale)
                                                      * maxAbsKf(v.c->kfScaleY, v.c->scaleY));
                    const double aRad = maxAbsKf(v.c->kfRotation, v.c->rotation) * kPi / 180.0;
                    const double cAng = std::fabs(std::cos(aRad));
                    const double sAng = std::fabs(std::sin(aRad));
                    return std::max(2, (int)std::ceil(W * maxX * sAng + H * maxY * cAng));
                }();
                // Mídia transparente: rotação preenche com TRANSPARENTE, não preto (senão
                // as quinas/canvas viram uma máscara preta sobre a camada de baixo).
                const QString rotFill = alphaMedia ? QStringLiteral("black@0")
                                                   : QStringLiteral("black");
                if (v.c->kfRotation.isEmpty())
                    fc.last().append(QStringLiteral(",rotate=a=%1:ow=%2:oh=%3:c=%4")
                                         .arg(num(v.c->rotation * kPi / 180.0))
                                         .arg(rotW).arg(rotH).arg(rotFill));
                else
                    // rotExpr está em GRAUS; o ffmpeg espera radianos no
                    // rotate. Sem essa conversão a rotação animada gira ~14×
                    // (90° vira 90 rad) e fica "maluca".
                    fc.last().append(QStringLiteral(",rotate=a='(%1)*%2':ow=%3:oh=%4:c=%5")
                                         .arg(rotExpr)
                                         .arg(num(kPi / 180.0))
                                         .arg(rotW).arg(rotH).arg(rotFill));
            }
            // Desfaz a pré-multiplicação do alpha após todos os redimensiona-
            // mentos, voltando ao alpha "freio" antes dos fades/efeitos.
            // O premultiply/unpremultiply trabalham em gbrap (planar); força
            // rgba empacotado de volta para os filtros seguintes negociarem
            // formato com alpha garantido (sem isso a camada podia virar uma
            // máscara opaca sobre o que está por baixo).
            if (premultiplied)
                fc.last().append(QLatin1Char(',') + QStringLiteral("unpremultiply=inplace=1,format=rgba"));
            // Texto: fade por ALPHA (não para preto), senão a camada transparente
            // viraria um quadrado preto durante o fade. Imagens (PNG/WebP) também
            // podem ter alpha — aplicar :alpha=1 para preservar transparência.
            const QString fadeAlpha = alphaMedia ? QStringLiteral(":alpha=1") : QString();
            if (fadeIn > 0)
                fc.last().append(QStringLiteral(",fade=t=in:st=%1:d=%2%3")
                                     .arg(num(pos)).arg(num(fadeIn)).arg(fadeAlpha));
            if (fadeOut > 0)
                fc.last().append(QStringLiteral(",fade=t=out:st=%1:d=%2%3")
                                     .arg(num(end - fadeOut)).arg(num(fadeOut)).arg(fadeAlpha));
            // Transição dissolve: o clipe da frente entra com alpha 0→1 sobre
            // o anterior (que segue por baixo, cobrindo a sobreposição).
            if (hasTrans && transType == QStringLiteral("dissolve"))
                fc.last().append(QStringLiteral(",fade=t=in:st=%1:d=%2:alpha=1")
                                     .arg(num(pos)).arg(num(transDur)));
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
            // ── LAINKA: stop motion na exportação ──────────────────────────
            if (v.c->lainkaEnabled) {
                const double pfps = project.fps > 0.0 ? project.fps : 30.0;
                const double effectiveFps = (v.c->lainkaTargetFps > 0)
                    ? std::clamp((double)v.c->lainkaTargetFps, 1.0, pfps)
                    : pfps / std::max(1, v.c->lainkaSkip);
                if (effectiveFps < pfps - 0.5)
                    fc.last().append(QStringLiteral(",fps=fps=%1").arg(num(effectiveFps)));

                if (v.c->lainkaJitterPos > 1e-6) {
                    const int amp = std::max(1, (int)std::lround(v.c->lainkaJitterPos * 0.08));
                    fc.last().append(QStringLiteral(
                        ",crop=iw-%1:ih-%2:random(1)*%1:random(2)*%2,scale=iw+%1:ih+%2")
                        .arg(num(amp * 2)).arg(num(amp * 2)));
                }

                if (v.c->lainkaJitterRot > 1e-6) {
                    const double maxAngle = v.c->lainkaJitterRot * 0.015;
                    fc.last().append(QStringLiteral(
                        ",rotate=eval=frame:ow=rotw(%1):oh=roth(%1):c=none:"
                        "angle=(random(3)-0.5)*%1*2*PI/180")
                        .arg(num(maxAngle)));
                }

                if (v.c->lainkaJitterScale > 1e-6) {
                    const double maxScale = v.c->lainkaJitterScale * 0.001;
                    fc.last().append(QStringLiteral(
                        ",scale=trunc((iw*(1+(random(4)-0.5)*%1*2))/2)*2:"
                        "trunc((ih*(1+(random(5)-0.5)*%1*2))/2)*2")
                        .arg(num(maxScale)));
                }

                if (v.c->lainkaFlicker > 1e-6) {
                    const double flickAmp = std::min(v.c->lainkaFlicker / 100.0, 0.15);
                    fc.last().append(QStringLiteral(",eq=brightness='random(1)*%1-%2'")
                                         .arg(num(flickAmp * 2.0))
                                         .arg(num(flickAmp)));
                }

                if (v.c->lainkaMotionBlur > 1e-6) {
                    const int r = std::max(1, (int)std::lround(v.c->lainkaMotionBlur * 0.05));
                    fc.last().append(QStringLiteral(",boxblur=luma_radius=%1:chroma_radius=%1")
                                         .arg(num(r)));
                }

                if (v.c->lainkaOpacity < 99.9) {
                    fc.last().append(QStringLiteral(",colorchannelmixer=aa=%1")
                                         .arg(num(std::clamp(v.c->lainkaOpacity / 100.0, 0.0, 1.0))));
                }
            }
            if (v.c->motionEnabled && v.c->motionAmount > 0.0) {
                const int r = std::clamp((int)std::lround(v.c->motionAmount * 0.4), 1, 40);
                const int p = std::clamp(v.c->motionSamples / 4, 1, 8);
                fc.last().append(QStringLiteral(",boxblur=luma_radius=%1:luma_power=%2")
                                     .arg(num(r)).arg(num(p)));
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
            // Opacidade da faixa (estilo Vegas/FCE): multiplica o alfa da faixa
            // inteira sobre as de baixo. Aplicada DEPOIS do fade/opacidade do
            // clipe (que usa rampas de fade) para não duplicar a rampa.
            if (v.trackOpacity < 1.0)
                fc.last().append(QStringLiteral(",colorchannelmixer=aa=%1").arg(num(v.trackOpacity)));
            fc.last().append(QStringLiteral("[%1]").arg(lbl));
            // Wipe: o clipe da frente desliza de um dos lados sobre o anterior
            // durante a sobreposição (progresso = min(1,(t-pos)/dur)).
            const bool isWipe = hasTrans && transType != QStringLiteral("dissolve");
            QString slideX, slideY;
            if (isWipe) {
                const QString s = QString("(1-min(1,(t-%1)/%2))").arg(num(pos)).arg(num(transDur));
                if (transType == QStringLiteral("wipeleft")) slideX = QString("+main_w*%1").arg(s);
                else if (transType == QStringLiteral("wiperight")) slideX = QString("-main_w*%1").arg(s);
                else if (transType == QStringLiteral("wipeup")) slideY = QString("+main_h*%1").arg(s);
                else if (transType == QStringLiteral("wipedown")) slideY = QString("-main_h*%1").arg(s);
            }
            if (hasT) {
                QString x = QString("main_w/2-overlay_w/2+%1").arg(txExpr);
                QString y = QString("main_h/2-overlay_h/2+%1").arg(tyExpr);
                if (!slideX.isEmpty()) x += slideX;
                if (!slideY.isEmpty()) y += slideY;
                fc << QStringLiteral("%1[%2]overlay=x='%3':y='%4':eof_action=pass:enable='between(t,%5,%6)':eval=frame[%7]")
                           .arg(vout, lbl, x, y).arg(num(pos)).arg(num(end)).arg(outLbl);
            } else if (v.blend == QStringLiteral("normal")) {
                if (isWipe) {
                    fc << QStringLiteral("%1[%2]overlay=x='%3':y='%4':eof_action=pass:enable='between(t,%5,%6)':eval=frame[%7]")
                               .arg(vout, lbl,
                                    slideX.isEmpty() ? QStringLiteral("0") : slideX,
                                    slideY.isEmpty() ? QStringLiteral("0") : slideY)
                               .arg(num(pos)).arg(num(end)).arg(outLbl);
                } else {
                    fc << QStringLiteral("%1[%2]overlay=0:0:eof_action=pass:enable='between(t,%3,%4)':eval=frame[%5]")
                               .arg(vout, lbl).arg(num(pos)).arg(num(end)).arg(outLbl);
                }
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
                if (isWipe) {
                    fc << QStringLiteral("%1[%2]overlay=x='%3':y='%4':eof_action=pass:enable='between(t,%5,%6)':eval=frame[%7]")
                               .arg(vout, pl,
                                    slideX.isEmpty() ? QStringLiteral("0") : slideX,
                                    slideY.isEmpty() ? QStringLiteral("0") : slideY)
                               .arg(num(pos)).arg(num(end)).arg(outLbl);
                } else {
                    fc << QStringLiteral("%1[%2]overlay=0:0:eof_action=pass:enable='between(t,%3,%4)':eval=frame[%5]")
                               .arg(vout, pl).arg(num(pos)).arg(num(end)).arg(outLbl);
                }
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

            QString src;
            // Cada clipe de áudio usa UM stream específico do arquivo
            // (ex.: OBS/câmera com várias faixas). Antes misturávamos todas as
            // faixas; com 1 clipe por stream na timeline, cada um escolhe a dele.
            int stream = 0;
            if (m && m->audioStreams > 0)
                stream = qMin(qMax(0, c->audioStreamIndex), m->audioStreams - 1);
            src = QStringLiteral("[%1:a:%2]").arg(inIdx).arg(stream);

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
            // Crossfade de transição: fade-in no clipe da frente e fade-out no
            // de trás, sobre a sobreposição com o vizinho da mesma faixa.
            const auto afIt = audioFades.find(c->id);
            if (afIt != audioFades.end()) {
                if (afIt->first > 0.0)
                    fc.last().append(QStringLiteral(",afade=t=in:st=0:d=%1")
                                         .arg(num(afIt->first)));
                if (afIt->second > 0.0)
                    fc.last().append(QStringLiteral(",afade=t=out:st=%1:d=%2")
                                         .arg(num(dur - afIt->second))
                                         .arg(num(afIt->second)));
            }
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
            // Pan estéreo: aplica pan da faixa (equal-power).
            if (std::fabs(ar.trackPan) > 0.01) {
                const double gL = std::sqrt(std::max(0.0, (1.0 - ar.trackPan) * 0.5));
                const double gR = std::sqrt(std::max(0.0, (1.0 + ar.trackPan) * 0.5));
                fc.last().append(QStringLiteral(",aeval='val(0)*%1|val(1)*%2'")
                                     .arg(num(gL)).arg(num(gR)));
            }
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
        // Volume master: aplica o masterVolume do projeto no sinal de áudio final.
        if (std::fabs(project.masterVolume - 1.0) > 1e-4) {
            const QString masterLbl = QStringLiteral("amstr");
            fc << aout + QStringLiteral("volume=%1[%2]")
                    .arg(num(std::clamp(project.masterVolume, 0.0, 2.0)))
                    .arg(masterLbl);
            aout = QStringLiteral("[%1]").arg(masterLbl);
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
