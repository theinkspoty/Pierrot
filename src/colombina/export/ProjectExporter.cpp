// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "ProjectExporter.h"
#include "colombina/util.h"
#include "colombina/generators.h"
#include "colombina/render/MesaRenderer.h"
#include "colombina/render/TextRender.h"
#include "laartman/Decoder.h"
#include "colombina/frei0r/Frei0rPluginManager.h"
#include "colombina/fx/ColorGrade.h"

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
#include <QProcess>
#include <QSettings>

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <limits>

#include "ProjectExporterPriv.h"

QStringList ProjectExporter::buildCommand(const Project& project,
                                          const ExportSettings& s, QString* error,
                                          const Progress& progress,
                                          const Frei0rPluginManager* frei0rManager) {
    QStringList args;
    const auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return QStringList();
    };

    if (s.outputPath.isEmpty())
        return fail(QStringLiteral("Caminho de saída vazio."));
    if (project.duration() <= 0)
        return fail(QStringLiteral("Timeline vazia — nada para exportar."));

    // Limpa PNGs temporários (texto, geradores, sequências Mesa e de
    // velocidade) de exportações anteriores.
    const QDir tmp(QDir::tempPath());
    for (const QFileInfo& fi : tmp.entryInfoList(
             {QStringLiteral("pierrot-text-*.png"), QStringLiteral("pierrot-gen-*.png"),
              QStringLiteral("pierrot-mesa-*.png"), QStringLiteral("pierrot-vel-*.png")},
             QDir::Files))
        QFile::remove(fi.absoluteFilePath());

    const double total = std::max(project.duration(), 0.5);
    const int W = s.width;
    const int H = s.height;
    const int FPS = s.fps;
    // Só áudio (WAV/MP3): pula o graph de vídeo e os inputs de mídia de
    // vídeo; o mix de áudio (efeitos, fades, master) roda igual.
    const bool audioOnly = s.audioOnly();
    // GIF com fundo transparente: o que não tiver clipe por cima fica alfa 0.
    const bool transparent =
        (s.format == ExportSettings::GIF) && s.transparentBackground;

    // Video clips bottom -> top, so the topmost track is composited last (on top).
    // Tracks que pertencem a uma Mesa NÃO viram camadas normais do empilhamento:
    // o clipe do grupo é a composição inteira (banda pré-renderizada), inserida
    // no Z do topo do grupo. Convenção de Z igual ao PreviewWidget: faixa 0 é o
    // TOPO (maior índice = por baixo).
    QSet<QString> mesaTrackIds;
    for (const MesaComposition& m : project.mesas)
        for (const QString& tid : m.trackIds) mesaTrackIds.insert(tid);

    // Banda Mesa: intervalo da timeline onde o grupo tem conteúdo (união dos
    // clips das tracks membro) + Z = índice da track de topo com conteúdo.
    // Pré-renderiza a sequência ANTES de montar o graph, para o padrão image2
    // já existir quando o ffmpeg rodar (a resolução de saída da composição é
    // a do projeto; o ffmpeg escala para W×H com letterbox transparente).
    struct MesaBandPrep {
        const MesaComposition* mc = nullptr;
        int z = INT_MAX;
        double start = 0.0, end = 0.0;
        int frames = 1;
        int k = -1;
        QString pattern;
    };
    // Passo 1: mede as bandas (Z, intervalo e nº de frames) sem renderizar.
    QVector<MesaBandPrep> mesaPreps;
    if (!audioOnly) {
        int k = 0;
        for (const MesaComposition& m : project.mesas) {
            if (project.width <= 0 || project.height <= 0) continue;
            double mn = std::numeric_limits<double>::max();
            double mx = -std::numeric_limits<double>::max();
            int z = INT_MAX;
            bool hasClip = false;
            bool hasVisible = false;
            for (int tr = 0; tr < (int)project.videoTracks.size(); ++tr) {
                if (!m.trackIds.contains(project.videoTracks[tr].id)) continue;
                bool trackClips = false;
                for (const Clip& c : project.videoTracks[tr].clips) {
                    trackClips = true;
                    hasClip = true;
                    if (!project.videoTracks[tr].mesaHidden) hasVisible = true;
                    if (c.pos < mn) mn = c.pos;
                    if (c.pos + c.dur > mx) mx = c.pos + c.dur;
                }
                if (trackClips && tr < z) z = tr;
            }
            if (!hasClip || !hasVisible || z == INT_MAX) continue;
            const int frames = std::max(1, (int)std::ceil((mx - mn) * FPS));
            mesaPreps.push_back({&m, z, mn, mx, frames, k++, QString()});
        }
    }
    // Passo 2: renderiza as sequências com progresso cumulativo (cancelável).
    if (!mesaPreps.isEmpty()) {
        long long totalFrames = 0;
        for (const MesaBandPrep& p : mesaPreps) totalFrames += p.frames;
        MesaRenderer renderer;
        long long done = 0;
        bool aborted = false;
        QVector<MesaBandPrep> ok;
        for (MesaBandPrep& p : mesaPreps) {
            p.pattern = renderMesaSequence(renderer, project, *p.mc, p.start, p.end,
                                           FPS, p.k, done, totalFrames, progress, &aborted);
            if (aborted) return fail(QStringLiteral("Cancelado pelo usuário."));
            if (p.pattern.isEmpty()) continue;
            ok.push_back(p);
        }
        mesaPreps = ok;
    }

    QVector<VideoClipRef> vclips;
    // Segmentos expandidos de clipes multicam. std::deque: push_back não
    // invalida ponteiros para elementos já inseridos (VideoClipRef guarda
    // const Clip*).
    std::deque<Clip> mcOwned;
    // Caminho temporário do PNG gerado para cada clipe de texto (índice = mesmo
    // de vclips); vazio para clipes de mídia. Preenchido no loop de inputs.
    QVector<QString> textPngs;
    // Banda Mesa por entrada de vclips (padrão da sequência; vazio p/ normal).
    QVector<MesaBandRef> mesaBands;
    // Padrão da sequência pré-renderizada de clipes com velocity envelope
    // (paralelo a vclips; vazio = clipe com velocidade constante).
    QVector<QString> velocityPatterns;
    for (int tr = (int)project.videoTracks.size() - 1; tr >= 0; --tr) {
        const Track& track = project.videoTracks[tr];
        if (!track.visible) continue;   // faixa oculta (olho) não é exportada
        if (!mesaTrackIds.contains(track.id)) {
            const double trOp = std::clamp(track.opacity, 0.0, 1.0);
            for (const Clip& c : track.clips) {
                if (c.isText) {
                    // Clipe independente de texto: vira uma camada gerada.
                    vclips.push_back({&c, nullptr, track.blendMode, trOp});
                    textPngs.push_back(QString());
                    mesaBands.push_back(MesaBandRef());
                    velocityPatterns.push_back(QString());
                    continue;
                }
                if (c.hasMulticam()) {
                    // Multicam: expande em segmentos por ângulo; cada um vira
                    // um input ffmpeg com o arquivo do ângulo ativo.
                    for (const Clip& sc : expandMulticamClip(c)) {
                        mcOwned.push_back(sc);
                        const MediaItem* sm = project.findMedia(mcOwned.back().mediaId);
                        if (sm && sm->hasVideo) {
                            vclips.push_back({&mcOwned.back(), sm,
                                              track.blendMode, trOp});
                            textPngs.push_back(QString());
                            mesaBands.push_back(MesaBandRef());
                            velocityPatterns.push_back(QString());
                        }
                    }
                    continue;
                }
                const MediaItem* m = project.findMedia(c.mediaId);
                if (m && m->hasVideo) {
                    vclips.push_back({&c, m, track.blendMode, trOp});
                    textPngs.push_back(QString());
                    mesaBands.push_back(MesaBandRef());
                    velocityPatterns.push_back(QString());
                }
            }
        }
        // A banda Mesa entra no Z do topo do grupo: quando tr == z (menor índice
        // das tracks membro), ela já vem depois das tracks de baixo (índice
        // maior) e antes das de cima — na ordem de empilhamento, como no preview.
        for (const MesaBandPrep& bp : mesaPreps) {
            if (bp.z != tr) continue;
            const Track& zt = project.videoTracks[bp.z];
            VideoClipRef v;
            v.c = nullptr;
            v.m = nullptr;
            v.blend = zt.blendMode;
            v.trackOpacity = std::clamp(zt.opacity, 0.0, 1.0);
            v.bandPos = bp.start;
            v.bandStart = bp.start;
            v.bandEnd = bp.end;
            vclips.push_back(v);
            textPngs.push_back(QString());
            mesaBands.push_back(MesaBandRef{bp.mc, 0, bp.pattern, bp.start, bp.end});
            velocityPatterns.push_back(QString());
        }
    }
    std::stable_sort(vclips.begin(), vclips.end(),
                     [](const VideoClipRef& a, const VideoClipRef& b) {
                         return vclipPos(a) < vclipPos(b);
                     });
    // Mantém textPngs, mesaBands e velocityPatterns alinhados a vclips reordenado.
    {
        QVector<QString> reordered(textPngs.size());
        QVector<MesaBandRef> reorderedBands(mesaBands.size());
        QVector<QString> reorderedVelocity(velocityPatterns.size());
        QVector<int> order(vclips.size());
        for (int i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return vclipPos(vclips[a]) < vclipPos(vclips[b]); });
        for (int i = 0; i < (int)vclips.size(); ++i) {
            reordered[i] = textPngs[order[i]];
            reorderedBands[i] = mesaBands[order[i]];
            reorderedVelocity[i] = velocityPatterns[order[i]];
        }
        textPngs = reordered;
        mesaBands = reorderedBands;
        velocityPatterns = reorderedVelocity;
    }

    // Pré-renderiza clipes com velocity envelope (rampa de velocidade) numa
    // sequência PNG — a velocidade variável não é expressável num setpts simples
    // e a pré-renderização garante paridade exata com o preview (mesma
    // clipSrcTime). Clipes sem envelope seguem o caminho normal (setpts /speed).
    // Só áudio: sem vídeo, sem sequência.
    if (!audioOnly) {
        int vk = 0;
        for (int i = 0; i < (int)vclips.size(); ++i) {
            const VideoClipRef& v = vclips[i];
            if (!v.c || !v.m || v.m->isSolid || isImageFile(v.m->filePath))
                continue;
            if (!hasVelocityEnvelope(*v.c))
                continue;
            velocityPatterns[i] =
                renderVelocitySequence(*v.c, v.m->filePath, FPS, vk++, W);
        }
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
            // A sobreposição vira a duração da transição, mas nunca pode passar
            // da duração de nenhum dos dois clipes (senão o `fade` do ffmpeg
            // estoura o material do clipe). Mesmo clamp do áudio acima.
            double d = prev->pos + prev->dur - cur->pos;
            d = std::min(d, std::min(prev->dur, cur->dur));
            if (d > 1e-6) {
                const QString type = isTransition(prev->transitionType)
                                         ? prev->transitionType
                                         : QStringLiteral("dissolve");
                clipTrans.insert(cur->id, qMakePair(type, d));
            }
        }
    }

    bool anySolo = false;
    // Só faixas de ÁUDIO têm solo com efeito no mix (consistente com o preview:
    // faixas de vídeo nunca são fonte de áudio).
    for (const Track& t : project.audioTracks)
        if (t.solo) { anySolo = true; break; }

    QVector<AudioClipRef> aclips;
    for (const Track& t : project.audioTracks) {
        if (!t.visible || t.muted || (anySolo && !t.solo)) continue;
        for (const Clip& c : t.clips) {
            const MediaItem* m = project.findMedia(c.mediaId);
            if (m && m->hasAudio) aclips.push_back({&c, t.volume, t.pan, &t});
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

    // GPU para encode por hardware (Configurações → Encode acelerado).
    // VAAPI: -vaapi_device é opção de INPUT e precisa vir antes do primeiro -i.
    // NVENC: -gpu vai junto do encoder (mais abaixo). Só áudio: sem GPU de vídeo.
    if (!audioOnly) {
        QSettings hwSet;
        const bool hwOn = hwSet.value("exportHwEncode", false).toBool()
                          && s.format != ExportSettings::WEBM;
        const QString hwDev = hwSet.value("exportHwDevice").toString();
        if (hwOn && hwDev.startsWith(QLatin1String("vaapi:")))
            args << QStringLiteral("-vaapi_device") << hwDev.mid(6);
    }

    // Input 0: background color. Then video clips, then audio clips.
    // No modo transparente a cor de fundo precisa nascer já em rgba (com alfa 0)
    // — o `format=rgba` precisa estar DENTRO da entrada lavfi, senão a fonte
    // negocia para yuv420p e o alfa se perde.
    if (!audioOnly)
        args << "-f" << "lavfi"
             << "-i" << (transparent
                             ? QString("color=c=black@0.0:s=%1x%2:r=%3:d=%4,format=rgba")
                                   .arg(W).arg(H).arg(FPS).arg(num(total))
                             : QString("color=c=black:s=%1x%2:r=%3:d=%4")
                                   .arg(W).arg(H).arg(FPS).arg(num(total)));

    for (int i = 0; i < (int)vclips.size(); ++i) {
        if (audioOnly) break; // sem inputs de vídeo no export de só áudio
        const VideoClipRef& v = vclips[i];
        const MesaBandRef& mb = i < mesaBands.size() ? mesaBands[i] : MesaBandRef();
        if (mb.mc != nullptr) {
            // Banda Mesa: sequência de PNGs pré-renderizada (image2). O input
            // começa no PTS 0 = bandStart; o graph reposiciona com setpts.
            args << "-framerate" << num(FPS) << "-start_number" << "1"
                 << "-t" << num(mb.end - mb.start) << "-i" << mb.pattern;
            continue;
        }
        if (!velocityPatterns[i].isEmpty()) {
            // Clipe com velocity envelope: sequência pré-renderizada (image2),
            // cadência já correta — o graph só reposiciona (setpts com speed 1).
            args << "-framerate" << num(FPS) << "-start_number" << "1"
                 << "-t" << num(v.c->dur) << "-i" << velocityPatterns[i];
            continue;
        }
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
            const int gw = v.m->width > 0 ? v.m->width : W;
            const int gh = v.m->height > 0 ? v.m->height : H;
            if (v.m->generator.isEmpty()) {
                // Cor sólida (gerador estilo Vegas): input lavfi com a cor.
                args << "-f" << "lavfi"
                     << "-i" << QString("color=c=%1:s=%2x%3:r=%4:d=%5")
                                    .arg(hexColor(v.m->solidColor))
                                    .arg(gw).arg(gh).arg(FPS)
                                    .arg(num(v.c->dur * v.c->speed));
            } else {
                // Gradiente/checkerboard/ruído: o preview é estático e no
                // tamanho próprio do gerador, então exporta um PNG estático
                // com `generatorFrame` e faz loop — consistente com o preview.
                QImage img = generatorFrame(*v.m, gw, gh);
                QTemporaryFile tmp;
                tmp.setAutoRemove(false);
                tmp.setFileTemplate(QDir::tempPath() + "/pierrot-gen-XXXXXX.png");
                if (tmp.open()) {
                    img.save(tmp.fileName(), "PNG");
                    textPngs[i] = tmp.fileName();
                    args << "-loop" << "1" << "-framerate" << num(FPS)
                         << "-t" << num(v.c->dur * v.c->speed) << "-i" << textPngs[i];
                } else {
                    // Fallback: cor sólida da base.
                    args << "-f" << "lavfi"
                         << "-i" << QString("color=c=%1:s=%2x%3:r=%4:d=%5")
                                        .arg(hexColor(v.m->solidColor))
                                        .arg(gw).arg(gh).arg(FPS)
                                        .arg(num(v.c->dur * v.c->speed));
                }
            }
        } else if (isImageFile(v.m->filePath)
                   && !Decoder::isAnimatedImage(v.m->filePath)) {
            // Imagem estática (PNG/JPEG/BMP/WebP estático, GIF de um quadro):
            // loop contínuo na taxa do projeto, limitado à duração do clipe.
            args << "-loop" << "1" << "-framerate" << num(FPS)
                 << "-t" << num(v.c->dur * v.c->speed) << "-i" << v.m->filePath;
        } else if (isImageFile(v.m->filePath)) {
            // GIF animado (ou WebP animado): o demuxer respeita a extensão
            // NETSCAPE com -ignore_loop 0 e repete os pacotes enquanto o
            // clipe durar (paridade com o preview, que embrulha o tempo com
            // fmod). Sem -ignore_loop o ffmpeg toca uma vez só e congela.
            args << "-ignore_loop" << "0"
                 << "-ss" << num(std::max(0.0, v.c->in)) << "-t" << num(v.c->dur * v.c->speed)
                 << "-i" << v.m->filePath;
        } else {
            args << "-ss" << num(std::max(0.0, v.c->in)) << "-t" << num(v.c->dur * v.c->speed)
                 << "-i" << v.m->filePath;
        }
    }

    for (const AudioClipRef& ar : aclips) {
        const Clip* c = ar.c;
        const MediaItem* m = project.findMedia(c->mediaId);
        args << "-ss" << num(std::max(0.0, c->in)) << "-t" << num(c->dur * c->speed) << "-i" << m->filePath;
    }

    QStringList fc;

    // ---- Video graph ----
    QString vout = QStringLiteral("[0:v]");
    if (audioOnly) {
        // Sem graph de vídeo: aout (se houver) é o único mapeamento da saída.
        vout.clear();
    } else if (vclips.isEmpty()) {
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
        for (int i = 0; i < (int)vclips.size(); ++i) {
            const VideoClipRef& v = vclips[i];
            ++n;
            const int inIdx = n;
            const QString lbl = QStringLiteral("v%1").arg(n);
            const QString outLbl =
                (n == (int)vclips.size()) ? QStringLiteral("vout") : QStringLiteral("m%1").arg(n);
            const MesaBandRef& mb = mesaBands[i];

            // ── Banda Mesa ────────────────────────────────────────────────
            // A composição já sai pronta (canvas + câmera + blends internos)
            // do MesaRenderer, no tamanho do projeto e com fundo transparente.
            // Reposiciona na timeline (image2 começa em PTS 0 = bandStart),
            // pré-multiplica o alpha por segurança de interpolação e overlay
            // no Z do topo do grupo. Nada de transform/fade próprios: tudo já
            // foi embutido nos frames renderizados.
            if (mb.mc != nullptr) {
                const double bStart = mb.start;
                const double bEnd = mb.end;
                const double bDur = mb.end - mb.start;
                QString chain =
                    QStringLiteral("[%1:v]fps=%2,settb=AVTB,setpts=(PTS-STARTPTS)+%3/TB,"
                                   "trim=duration=%4,format=rgba")
                        .arg(inIdx).arg(FPS).arg(num(bStart)).arg(num(bDur));
                chain += QStringLiteral(",premultiply=inplace=1");
                if (W != project.width || H != project.height) {
                    // Resolução diferente da do projeto: contém + letterbox
                    // transparente (igual a PNG/WebP com alpha), para o que
                    // está por baixo continuar aparecendo.
                    chain += QStringLiteral(
                                 ",scale=%1:%2:force_original_aspect_ratio=decrease:"
                                 "force_divisible_by=2,"
                                 "pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=black@0,"
                                 "fps=%3,format=rgba")
                                 .arg(W).arg(H).arg(FPS);
                }
                chain += QStringLiteral(",unpremultiply=inplace=1,format=rgba");
                if (v.trackOpacity < 1.0)
                    chain += QStringLiteral(",colorchannelmixer=aa=%1").arg(num(v.trackOpacity));
                fc << chain + QStringLiteral("[%1]").arg(lbl);
                if (v.blend == QStringLiteral("normal")) {
                    fc << QStringLiteral("%1[%2]overlay=0:0:eof_action=pass:enable='between(t,%3,%4)':eval=frame[%5]")
                               .arg(vout, lbl).arg(num(bStart)).arg(num(bEnd)).arg(outLbl);
                } else {
                    const QString rs = QStringLiteral("rs%1").arg(n);
                    const QString cs = QStringLiteral("cs%1").arg(n);
                    const QString bs = QStringLiteral("bs%1").arg(n);
                    const QString pl = QStringLiteral("pl%1").arg(n);
                    fc << QStringLiteral("[%1]trim=start=%2:duration=%3,setpts=PTS-STARTPTS,format=rgba[%4]")
                               .arg(vout.mid(1, vout.size() - 2), num(bStart), num(bDur), rs);
                    fc << QStringLiteral("[%1]setpts=PTS-STARTPTS[%2]").arg(lbl, cs);
                    fc << QStringLiteral("[%1][%2]blend=c0_mode=%3:c0_opacity=1:c1_mode=%3:c1_opacity=1"
                                         ":c2_mode=%3:c2_opacity=1:c3_mode=normal:c3_opacity=1[%4]")
                               .arg(rs, cs, v.blend, bs);
                    fc << QStringLiteral("[%1]setpts=PTS-STARTPTS+%2/TB[%3]")
                               .arg(bs).arg(num(bStart)).arg(pl);
                    fc << QStringLiteral("%1[%2]overlay=0:0:eof_action=pass:enable='between(t,%3,%4)':eval=frame[%5]")
                               .arg(vout, pl).arg(num(bStart)).arg(num(bEnd)).arg(outLbl);
                }
                vout = QStringLiteral("[%1]").arg(outLbl);
                continue;
            }

            const double pos = v.c->pos;
            const double end = v.c->pos + v.c->dur;
            const double dur = v.c->dur;
            // Clipe com velocity envelope: a cadência já vem correta da
            // sequência pré-renderizada; o setpts só reposiciona (speed 1).
            const bool velocity = !velocityPatterns[i].isEmpty();
            const double speed = velocity ? 1.0 : std::max(0.1, v.c->speed);
            const double fadeIn = std::min(std::max(v.c->fadeIn, 0.0), dur);
            const double fadeOut = std::min(std::max(v.c->fadeOut, 0.0), dur - fadeIn);
            const bool hasT = v.c->hasTransform();
            // Mídia transparente (texto gerado, PNG/WebP com alpha, ou
            // pré-render LAINKA/OFX em ProRes 4444 — issue #7). Todo
            // redimensionamento do ffmpeg (o enquadramento contain, o flatt
            // X/Y e a rotação) interpola RGB com alpha "freio" (straight), e o
            // RGB preto sob os pixels transparentes vaza para as bordas —
            // franja escura em volta da transparência (pior ainda ao reduzir e
            // depois esticar). Para essa mídia, pré-multiplicamos o alpha logo
            // no início da cadeia e desfazemos depois de toda a geometria.
            const bool alphaMedia = (v.m == nullptr)
                || (v.m && isImageFile(v.m->filePath))
                || (v.m && (v.m->filePath.contains(QLatin1String("/lainka/lainka_"))
                            || v.m->filePath.contains(QLatin1String("/ofx/ofx_"))));
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
                if (velocity) {
                    // Cadência e timebase da sequência pré-renderizada são
                    // normalizadas antes de reposicionar (igual ao clipe de texto
                    // e à banda Mesa), evitando desvio do image2.
                    chain = QStringLiteral("[%1:v]fps=%2,settb=AVTB,setpts=(PTS-STARTPTS)+%3/TB,"
                                           "trim=duration=%4")
                                .arg(inIdx).arg(FPS).arg(num(pos)).arg(num(dur));
                } else {
                    chain = QStringLiteral("[%1:v]setpts=(PTS-STARTPTS)/%2+%3/TB,trim=duration=%4")
                                .arg(inIdx).arg(num(speed)).arg(num(pos)).arg(num(dur));
                }
                if (alphaMedia) {
                    // Pré-multiplica ANTES do enquadramento: o downscale do
                    // contain é o maior gerador da franja escura.
                    chain += QStringLiteral(",format=rgba,premultiply=inplace=1");
                    premultiplied = true;
                }
                // Mídia sem fundo (PNG/WebP com alpha OU pré-render LAINKA/OFX
                // em ProRes): o letterbox usa TRANSPARENTE para não cobrir o
                // que está por baixo. Vídeos/imagens opacas usam preto.
                const QString padColor = (alphaMedia || transparent)
                                             ? QStringLiteral("black@0")
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
            // Máscaras: aplicadas ANTES dos efeitos de cor/chroma (paridade com
            // o preview, onde applyMasks roda no início de applyBasicEffectsOn).
            {
                const QString msk = masksFilter(*v.c, pos);
                if (!msk.isEmpty())
                    fc.last().append(QLatin1Char(',') + msk);
            }
            if (v.c->chromaKey) {
                // `blend` do chromakey do ffmpeg é a suavidade da borda — o
                // equivalente exportável de chromaKeySoftness. Estava fixo em
                // 0.1, então o controle de suavidade não tinha efeito algum no
                // render final. Mapeamento escolhido para reproduzir o 0.1 no
                // default (softness = 0.10) e cobrir toda a faixa 0→1.
                const double blend = std::clamp(v.c->chromaKeySoftness, 0.01, 0.9);
                fc.last().append(QStringLiteral(",chromakey=color=%1:similarity=%2:blend=%3")
                                     .arg(hexColor(v.c->chromaKeyColor))
                                     .arg(num(std::clamp(v.c->chromaKeySimilarity, 0.0, 1.0)))
                                     .arg(num(blend)));
                // Supressão de spill logo depois (ordem do preview: alpha é
                // calculado antes, a correção do canal da chave não muda o
                // alpha). Sem o filtro despill no ffmpeg, sai sem despill.
                const QString desp = despillFilter(*v.c);
                if (!desp.isEmpty())
                    fc.last().append(QLatin1Char(',') + desp);
            }
            if (v.c->brightness != 0.0 || v.c->contrast != 1.0 || v.c->saturation != 1.0)
                fc.last().append(QStringLiteral(",eq=brightness=%1:contrast=%2:saturation=%3")
                                     .arg(num(std::clamp(v.c->brightness, -1.0, 1.0)))
                                     .arg(num(std::clamp(v.c->contrast, 0.0, 2.0)))
                                     .arg(num(std::clamp(v.c->saturation, 0.0, 2.0))));
            // ── Color grade Lumetri (LGG + exposure/curves/LUT/vignette…) ──
            // Espelha colorgrade::applyToImage do preview. Inclui o
            // Lift/Gamma/Gain clássico (compat com projetos antigos).
            if (v.c->hasColorGrade()) {
                for (const QString& f : colorgrade::ffmpegFilters(*v.c))
                    fc.last().append(QLatin1Char(',') + f);
            }
            if (v.c->grayscale)
                fc.last().append(QStringLiteral(
                    ",colorchannelmixer=rr=0.299:rg=0.587:rb=0.114"
                    ":gr=0.299:gg=0.587:gb=0.114"
                    ":br=0.299:bg=0.587:bb=0.114"));
            // ── frei0r (padrão Kdenlive/Shotcut) ──────────────────────────
            // Mesma pilha do preview; cada filtro vira `frei0r=nome:p=v`.
            // O CLI ffmpeg usa o loader padrão do sistema (.so em
            // /usr/lib/frei0r-1 etc.); o manager local só fornece metadados
            // (nomes de parâmetros) para montar a string.
            if (!v.c->frei0rFx.isEmpty() && frei0rManager) {
                for (const Frei0rEffect& f0r : v.c->frei0rFx) {
                    const QString f =
                        Frei0rPluginManager::ffmpegFilterFor(f0r, frei0rManager);
                    if (!f.isEmpty())
                        fc.last().append(QLatin1Char(',') + f);
                }
            }
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
                const int len = std::clamp((int)std::lround(v.c->motionAmount * 0.4), 1, 40);
                const double angle = std::fmod(v.c->motionAngle, 360.0);
                const int samples = std::clamp(v.c->motionSamples, 1, 32);
                if (len > 0) {
                    fc.last().append(QStringLiteral(",motionblur=length=%1:angle=%2:samples=%3")
                                         .arg(num(len)).arg(num(angle)).arg(num(samples)));
                }
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
            fc.last().append(psxFilter(*v.c));
            fc.last().append(filmGrainFilter(*v.c));
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
                else if (transType == QStringLiteral("wipetl")) { slideX = QString("+main_w*%1").arg(s); slideY = QString("+main_h*%1").arg(s); }
                else if (transType == QStringLiteral("wipetr")) { slideX = QString("-main_w*%1").arg(s); slideY = QString("+main_h*%1").arg(s); }
                else if (transType == QStringLiteral("wipebr")) { slideX = QString("-main_w*%1").arg(s); slideY = QString("-main_h*%1").arg(s); }
                else if (transType == QStringLiteral("wipebl")) { slideX = QString("+main_w*%1").arg(s); slideY = QString("-main_h*%1").arg(s); }
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
        // Os rótulos por clipe vão por faixa em tAlbl, para o FX por faixa.
        QHash<const Track*, QStringList> tAlbl;
        int n = 0;
        for (const AudioClipRef& ar : aclips) {
            ++n;
            const Clip* c = ar.c;
            // Só áudio: sem lavfi/fundos nem inputs de vídeo → índice 0-based.
            const int inIdx = audioOnly
                ? (n - 1)
                : (1 + (int)vclips.size() + (n - 1));
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
            // `curve=qsin` = potência constante (in = sin(π/2·p), out =
            // cos(π/2·p)); o linear (default) afundava ~3 dB no centro.
            const auto afIt = audioFades.find(c->id);
            if (afIt != audioFades.end()) {
                if (afIt->first > 0.0)
                    fc.last().append(QStringLiteral(",afade=t=in:st=0:d=%1:curve=qsin")
                                         .arg(num(afIt->first)));
                if (afIt->second > 0.0)
                    fc.last().append(QStringLiteral(",afade=t=out:st=%1:d=%2:curve=qsin")
                                         .arg(num(dur - afIt->second))
                                         .arg(num(afIt->second)));
            }
            fc.last().append(QStringLiteral(",adelay=%1:all=1").arg(delayMs));
            // Lógica Vegas: volume efetivo = envelope do clipe (relativo, base 1.0)
            // × volume do clipe × envelope da faixa (tempo absoluto da timeline),
            // limitado a 200% como no preview. O preview aplica o mesmo produto
            // (PreviewWidget::buildMixSources: c->volume × kfValue × trackVol).
            const bool trackEnv = ar.tr && !ar.tr->kfVolume.isEmpty();
            const bool clipEnv = !c->kfVolume.isEmpty();
            if (clipEnv || trackEnv
                || std::fabs(c->volume * ar.trackVol - 1.0) > 1e-4) {
                //Sempre inclui c->volume (multiplicador base do clipe) — o preview
                //multiplica clipVol × envelope × trackVol; sem clipVol o export
                //toca mais alto quando volume < 1.0.
                const QString clipGain =
                    clipEnv
                        ? QStringLiteral("%1*%2")
                              .arg(num(c->volume))
                              .arg(kfExpr(c->kfVolume, 1.0, c->pos))
                        : num(c->volume);
                const QString gain =
                    QStringLiteral("clip(%1*%2,0,2)")
                        .arg(clipGain)
                        .arg(trackEnv ? kfExpr(ar.tr->kfVolume, ar.tr->volume, 0.0)
                                      : num(ar.trackVol));
                fc.last().append(QStringLiteral(",volume='%1'").arg(gain));
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
            // Pan estéreo: pan da faixa (equal-power); com envelope, avalia em `t`
            // (tempo absoluto da timeline, pós-adelay do clipe).
            const bool trackPanEnv = ar.tr && !ar.tr->kfPan.isEmpty();
            if (trackPanEnv) {
                const QString pExpr =
                    QStringLiteral("clip(%1,-1,1)").arg(kfExpr(ar.tr->kfPan, ar.tr->pan, 0.0));
                fc.last().append(
                    QStringLiteral(",aeval='val(0)*sqrt((1-(%1))*0.5)|val(1)*sqrt((1+(%1))*0.5)'")
                        .arg(pExpr));
            } else if (std::fabs(ar.trackPan) > 0.01) {
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
            // Reverb EX: mistura do sinal seco com um eco multi-tap que simula
            // reverb (filtro aecho do ffmpeg, com dry/wet via asplit+amix).
            if (c->reverb && c->reverbMix > 0.01) {
                const QString d = QStringLiteral("%1_d").arg(lbl);
                const QString w = QStringLiteral("%1_w").arg(lbl);
                const QString dm = QStringLiteral("%1_dm").arg(lbl);
                const QString rv = QStringLiteral("%1_rv").arg(lbl);
                const QString mx = QStringLiteral("%1_mx").arg(lbl);
                const double sc = 0.6 + 0.4 * std::clamp(c->reverbSize, 0.0, 1.0);
                const double mix = std::clamp(c->reverbMix, 0.0, 1.0);
                fc.last().append(QStringLiteral(",asplit=2[%1][%2]").arg(d).arg(w));
                fc << QStringLiteral("[%1]volume=%2[%3]")
                        .arg(d, num(1.0 - mix), dm);
                fc << QStringLiteral("[%1]aecho=0.9:0.9:50|100|180:%2|%3|%4,volume=%5[%6]")
                        .arg(w)
                        .arg(num(0.30 * sc), num(0.22 * sc), num(0.14 * sc))
                        .arg(num(mix), rv);
                fc << QStringLiteral("[%1][%2]amix=inputs=2:duration=first:"
                                     "normalize=0:dropout_transition=0[%3]")
                        .arg(dm, rv, mx);
                tAlbl[ar.tr] << mx;
            } else {
                fc.last().append(QStringLiteral("[%1]").arg(lbl));
                tAlbl[ar.tr] << lbl;
            }
        }
        // FX por faixa (estilo Vegas): cada faixa de áudio é resumida num
        // barramento, onde recebe os efeitos de faixa (denoise → EQ → inversão
        // → Reverb EX) antes da mistura final. Espelha a ordem do preview.
        QStringList tlbl;
        int tIdx = 0;
        for (const Track& tr : project.audioTracks) {
            auto it = tAlbl.constFind(&tr);
            if (it == tAlbl.cend() || it->isEmpty()) continue;
            const QStringList& labels = it.value();
            QString bus;
            if (labels.size() == 1) {
                bus = labels[0];
            } else {
                const QString tsrc = QStringLiteral("ta%1").arg(tIdx);
                QString bIn;
                for (const QString& l : labels) bIn += QStringLiteral("[%1]").arg(l);
                fc << bIn
                    + QStringLiteral(
                          "amix=inputs=%1:duration=longest:dropout_transition=0[%2]")
                          .arg(labels.size()).arg(tsrc);
                bus = tsrc;
            }
            QStringList fx;
            if (tr.denoise)
                fx << QStringLiteral("afftdn=nr=%1")
                          .arg(num(std::clamp(tr.denoiseAmount, 1.0, 50.0)));
            if (std::fabs(tr.eqLow) > 0.01)
                fx << QStringLiteral("equalizer=f=120:width_type=q:width=1:g=%1")
                          .arg(num(std::clamp(tr.eqLow, -12.0, 12.0)));
            if (std::fabs(tr.eqMid) > 0.01)
                fx << QStringLiteral("equalizer=f=1000:width_type=q:width=1:g=%1")
                          .arg(num(std::clamp(tr.eqMid, -12.0, 12.0)));
            if (std::fabs(tr.eqHigh) > 0.01)
                fx << QStringLiteral("equalizer=f=6000:width_type=q:width=1:g=%1")
                          .arg(num(std::clamp(tr.eqHigh, -12.0, 12.0)));
            const QString trGe = graphicEqFilters(tr.graphicEq);
            if (!trGe.isEmpty()) fx << trGe;
            const QString trPe = parametricEqFilters(tr.parametricEq);
            if (!trPe.isEmpty()) fx << trPe;
            if (tr.invertPhase)
                fx << QStringLiteral("aeval=-val(0)|-val(1)");
            QString preLbl = bus;
            if (!fx.isEmpty()) {
                preLbl = QStringLiteral("tbpre%1").arg(tIdx);
                fc << QStringLiteral("[%1],%2[%3]")
                        .arg(bus, fx.join(QLatin1Char(',')), preLbl);
            }
            if (tr.reverb && tr.reverbMix > 0.01) {
                const QString d = QStringLiteral("tb%1d").arg(tIdx);
                const QString w = QStringLiteral("tb%1w").arg(tIdx);
                const QString dm = QStringLiteral("tb%1dm").arg(tIdx);
                const QString rv = QStringLiteral("tb%1rv").arg(tIdx);
                const QString mx = QStringLiteral("tb%1mx").arg(tIdx);
                const double sc = 0.6 + 0.4 * std::clamp(tr.reverbSize, 0.0, 1.0);
                const double mix = std::clamp(tr.reverbMix, 0.0, 1.0);
                fc << QStringLiteral("[%1]asplit=2[%2][%3]").arg(preLbl, d, w);
                fc << QStringLiteral("[%1]volume=%2[%3]").arg(d, num(1.0 - mix), dm);
                fc << QStringLiteral("[%1]aecho=0.9:0.9:50|100|180:%2|%3|%4,volume=%5[%6]")
                        .arg(w)
                        .arg(num(0.30 * sc), num(0.22 * sc), num(0.14 * sc))
                        .arg(num(mix), rv);
                fc << QStringLiteral("[%1][%2]amix=inputs=2:duration=first:"
                                     "normalize=0:dropout_transition=0[%3]")
                        .arg(dm, rv, mx);
                tlbl << mx;
            } else {
                tlbl << preLbl;
            }
            ++tIdx;
        }
        if (tlbl.size() == 1) {
            aout = QStringLiteral("[%1]").arg(tlbl[0]);
        } else {
            // Cada rótulo precisa dos colchetes ([t1][t2]...) senão o ffmpeg
            // interpreta o nome inteiro como um filtro inexistente.
            QString aIn;
            for (const QString& l : tlbl) aIn += QStringLiteral("[%1]").arg(l);
            fc << aIn
                + QStringLiteral(
                      "amix=inputs=%1:duration=longest:dropout_transition=0[aout]")
                      .arg(tlbl.size());
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
        // EQ gráfico do master no sinal final (após o volume master).
        const QString mGe = graphicEqFilters(project.masterGraphicEq);
        if (!mGe.isEmpty()) {
            const QString geLbl = QStringLiteral("amge");
            fc << aout + mGe + QStringLiteral("[%1]").arg(geLbl);
            aout = QStringLiteral("[%1]").arg(geLbl);
        }
        // EQ paramétrico do master.
        const QString mPe = parametricEqFilters(project.masterParametricEq);
        if (!mPe.isEmpty()) {
            const QString peLbl = QStringLiteral("ampe");
            fc << aout + mPe + QStringLiteral("[%1]").arg(peLbl);
            aout = QStringLiteral("[%1]").arg(peLbl);
        }
    }

    // GIF: imagem animada — sem áudio e sem codec/bitrate. A última cadeia do
    // graph gera a paleta (amostrada de todos os quadros) e indexa a saída com
    // dither bayer, o único jeito de obter GIF decente num único ffmpeg. O
    // [vout] da composição já chega opaco (fundo preto composto por baixo).
    const bool isGif = (s.format == ExportSettings::GIF);
    if (isGif) {
        if (transparent) {
            // Fundo transparente: reserva o índice de transparência na paleta e
            // usa o limiar de alfa no paletteuse para manter o alfa 0.
            fc << QStringLiteral(
                "[vout]format=rgba,split=2[gbase1][gbase2];"
                "[gbase1]palettegen=reserve_transparent=1:stats_mode=diff[gpal];"
                "[gbase2][gpal]paletteuse=dither=bayer:bayer_scale=5:alpha_threshold=128[gvout]");
        } else {
            fc << QStringLiteral(
                "[vout]split=2[gbase1][gbase2];"
                "[gbase1]palettegen=stats_mode=diff[gpal];"
                "[gbase2][gpal]paletteuse=dither=bayer:bayer_scale=5[gvout]");
        }
    }
    if (!fc.isEmpty())
        args << "-filter_complex" << fc.join(QLatin1Char(';'));

    if (audioOnly) {
        if (aout.isEmpty())
            return fail(QStringLiteral("Projeto sem faixas de áudio para exportar."));
        args << "-map" << aout << "-vn";
        args << "-c:a" << codecFor(s.format, false);
        const int abr = std::clamp(s.audioBitrateKbps, 32, 640);
        if (s.format == ExportSettings::MP3)
            args << "-b:a" << QStringLiteral("%1k").arg(abr);
        // WAV é PCM: bitrate irrelevante (qualidade cheia).
        args << s.outputPath;
        return args;
    }

    args << "-map" << (isGif ? QStringLiteral("[gvout]") : QStringLiteral("[vout]"));
    if (!isGif && !aout.isEmpty()) args << "-map" << aout;
    else args << "-an";

    if (!isGif) {
        // Codificação por hardware (opt-in nas configurações): usa h264_nvenc ou
        // h264_vaapi para MP4/MKV quando o ffmpeg do sistema tiver o encoder; se
        // não houver, cai no libx264/VP9 normal (comportamento anterior).
        // GPU selecionável em Configurações (exportHwDevice: "nvenc:N" ou
        // "vaapi:/dev/dri/renderDNN"); vazio = automática.
        QSettings hwSet;
        const QString hwDevice = hwSet.value("exportHwDevice").toString();
        const QString hwCodec = (s.format != ExportSettings::WEBM
                                 && hwSet.value("exportHwEncode", false).toBool())
                                    ? detectHwEncoder() : QString();
        args << "-c:v" << (hwCodec.isEmpty() ? codecFor(s.format, true) : hwCodec);
        // NVENC: seleciona a placa (Configurações → GPU). "-gpu N" é opção do
        // h264_nvenc; com "Automática" (vazio) o ffmpeg escolhe.
        if (hwCodec == QStringLiteral("h264_nvenc") && hwDevice.startsWith(QLatin1String("nvenc:")))
            args << QStringLiteral("-gpu") << hwDevice.mid(6);
        if (s.videoBitrateKbps > 0) {
            // Bitrate fixo
            args << "-b:v" << QStringLiteral("%1k").arg(s.videoBitrateKbps);
        } else {
            // CRF (qualidade variável)
            const int crf = std::clamp(s.crf, 0, 51);
            if (s.format == ExportSettings::WEBM) {
                args << "-crf" << QString::number(crf) << "-b:v" << "0" << "-cpu-used" << "4";
            } else if (hwCodec == QStringLiteral("h264_nvenc")) {
                // NVENC: -cq == qualidade VBR (0–51, mesmo espírito do CRF).
                args << "-preset" << "p4" << "-cq" << QString::number(crf);
            } else if (hwCodec == QStringLiteral("h264_vaapi")) {
                // VAAPI: -qp em modo CQP (qualidade constante).
                args << "-qp" << QString::number(crf);
            } else {
                args << "-preset" << "medium" << "-crf" << QString::number(crf);
            }
        }
        args << "-pix_fmt" << "yuv420p";

        if (!aout.isEmpty()) {
            args << "-c:a" << codecFor(s.format, false);
            const int abr = std::clamp(s.audioBitrateKbps, 32, 640);
            args << "-b:a" << QStringLiteral("%1k").arg(abr);
        }
    } else {
        args << "-f" << "gif";
    }

    args << "-max_muxing_queue_size" << "1024";
    args << "-r" << QString::number(FPS);
    if (s.format == ExportSettings::MP4) args << "-movflags" << "+faststart";
    args << s.outputPath;

    if (error) error->clear();
    return args;
}
