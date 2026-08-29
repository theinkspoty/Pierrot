// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "Bench.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QTextStream>
#include <algorithm>
#include <cmath>

#include "models/Project.h"
#include "ffmpeg/FFmpegDecoder.h"

namespace {

// Estatística do vetor de duração (segundos).
struct TimeStats {
    double sum = 0.0;
    int n = 0;
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

TimeStats timeStats(QVector<double> v) {
    std::sort(v.begin(), v.end());
    TimeStats s;
    s.n = v.size();
    if (v.isEmpty()) return s;
    s.mean = 0.0;
    for (double d : v) s.mean += d;
    s.mean /= v.size();
    const auto p = [&](double q) {
        const int i = std::min<int>(v.size() - 1, int(q * v.size()));
        return v[i];
    };
    s.p50 = p(0.50);
    s.p95 = p(0.95);
    s.p99 = p(0.99);
    s.max = v.last();
    return s;
}

QString ms(double s) {
    return QString::number(s * 1000.0, 'f', 1).rightJustified(6, QLatin1Char(' ')) + "ms";
}

// VmHWM do processo (em KB) via /proc/self/status (Linux).
qint64 processVmHwm() {
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return -1;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine();
        if (line.startsWith("VmHWM:")) {
            const QList<QByteArray> parts = line.split(' ');
            for (int i = 1; i < parts.size(); ++i) {
                bool ok = false;
                const qint64 kb = parts[i].trimmed().toLongLong(&ok);
                if (ok) return kb;
            }
            break;
        }
    }
    return -1;
}

void benchReport(QTextStream& out, Project& project) {
    out << "Projeto: " << project.width << "x" << project.height << " @ "
        << project.fps << " fps, duração " << QString::number(project.duration(), 'f', 2)
        << "s, faixas de vídeo: " << project.videoTracks.size() << "\n";

    // ── Custo do CORTE (novo clipe): abrir + decodificar o 1º quadro ────────
    // Cobre o pior caso do pipeline (quando o prefetch falha). Usa contextos
    // novos a cada clipe — é exatamente o que acontece na transição de cortes.
    QVector<double> cutNs, frameNs;
    int cuts = 0;
    for (const Track& tr : project.videoTracks) {
        for (const Clip& c : tr.clips) {
            if (c.isText) continue;
            const MediaItem* m = project.findMedia(c.mediaId);
            if (!m || !m->hasVideo || m->isSolid || m->filePath.isEmpty()) continue;
            const double t0 = c.in;
            FFmpegDecoder dec;
            QElapsedTimer sw;
            sw.start();
            if (!dec.open(m->filePath)) {
                out << "  (aviso: falhou abrir " << QFileInfo(m->filePath).fileName() << ")\n";
                continue;
            }
            ++cuts;
            QImage img = dec.frameAt(t0, project.width);
            cutNs.append(sw.nsecsElapsed() / 1e9);

            // Streaming contínuo dentro do próprio clipe (mesmo context,
            // sem re-seek) — o custo "sustentado" de um quadro em playback.
            const double step = 1.0 / 60.0;
            const double end = std::min(c.in + c.dur * c.speed, m->duration - 1e-6);
            int samples = 0;
            for (double t = t0 + step; t < end && samples < 12; t += step) {
                sw.restart();
                img = dec.frameAt(t, project.width);
                frameNs.append(sw.nsecsElapsed() / 1e9);
                ++samples;
            }
            dec.close();
            if (cuts >= 400) break; // limite de tempo do bench
        }
        if (cuts >= 400) break;
    }
    const TimeStats cut = timeStats(cutNs);
    const TimeStats frame = timeStats(frameNs);

    out << "[1] Cortes — abrir + 1º quadro de cada clipe (custo do corte)\n";
    out << "  clips testados: " << cuts << "\n";
    out << "  1º quadro:   média " << ms(cut.mean) << "  p50 " << ms(cut.p50)
        << "  p95 " << ms(cut.p95) << "  p99 " << ms(cut.p99)
        << "  pior " << ms(cut.max) << "\n";

    out << "[2] Quadros em streaming contínuo (mesmo decoder, sem re-seek)\n";
    out << "  quadros: " << frame.n << "\n";
    {
        FFmpegDecoder check;
        const QString state = [&] {
            for (const Track& tr : project.videoTracks)
                for (const Clip& c : tr.clips) {
                    if (c.isText) continue;
                    const MediaItem* m = project.findMedia(c.mediaId);
                    if (!m || !m->hasVideo || m->isSolid || m->filePath.isEmpty()) continue;
                    if (!check.open(m->filePath)) return QStringLiteral("n/d");
                    return check.usesHardware() ? QStringLiteral("hardware (VAAPI)")
                                                : QStringLiteral("software (CPU)");
                }
            return QStringLiteral("n/d");
        }();
        out << "  decode: " << state << "\n";
    }
    out << "  por quadro: média " << ms(frame.mean) << "  p50 " << ms(frame.p50)
        << "  p95 " << ms(frame.p95) << "  p99 " << ms(frame.p99)
        << "  pior " << ms(frame.max) << "\n";
    {
        const double ftime = frame.n > 0 ? frame.sum : 0.0;
        const double fps = ftime > 0.0 ? frame.n / ftime : 0.0;
        out << "  taxa: " << QString::number(fps, 'f', 2) << " quadros/s\n";
        out << "  vs. timeline " << project.fps << "fps: ";
        if (fps >= project.fps) out << "OK (decode mais rápido que o tempo real)\n";
        else out << "INSUFICIENTE (é "
                 << QString::number(fps / project.fps * 100.0, 'f', 0)
                 << "% da cadência)\n";
    }

    // ── Seek preciso (custo de pular com a agulha) ──────────────────────────
    QVector<double> seekNs;
    const Clip* big = nullptr;
    for (const Track& tr : project.videoTracks)
        for (const Clip& c : tr.clips)
            if (!c.isText && c.dur > 2.0 && (!big || c.dur > big->dur)) big = &c;
    if (big) {
        const MediaItem* mm = project.findMedia(big->mediaId);
        if (mm && !mm->isSolid && !mm->filePath.isEmpty()) {
            FFmpegDecoder dec;
            if (dec.open(mm->filePath)) {
                const double span = std::min(big->dur * big->speed, 60.0);
                const int k = 10;
                for (int i = 0; i < k; ++i) {
                    double tgt = std::fmod(i * 13.37, span) + big->in;
                    tgt = std::min(tgt, std::max(0.0, mm->duration - 1e-6));
                    QElapsedTimer sw;
                    sw.start();
                    dec.frameAt(tgt, project.width);
                    seekNs.append(sw.nsecsElapsed() / 1e9);
                }
                dec.close();
            }
        }
    }
    const TimeStats seek = timeStats(seekNs);
    out << "[3] Seek (agulha pulando dentro do maior clipe de vídeo)\n";
    out << "  seeks: " << seek.n << "\n";
    out << "  por seek: média " << ms(seek.mean) << "  p50 " << ms(seek.p50)
        << "  p95 " << ms(seek.p95) << "  p99 " << ms(seek.p99)
        << "  pior " << ms(seek.max) << "\n";

    // ── Áudio por corte: abrir + seek + primeiro buffer ─────────────────────
    QVector<double> audNs;
    for (const Track& tr : project.videoTracks) {
        for (const Clip& c : tr.clips) {
            if (c.isText) continue;
            const MediaItem* m = project.findMedia(c.mediaId);
            if (!m || !m->hasAudio || m->filePath.isEmpty()) continue;
            FFmpegDecoder dec;
            QElapsedTimer sw;
            sw.start();
            if (dec.open(m->filePath) && dec.hasAudio()) {
                dec.seekAudio(c.in);
                QByteArray buf(65536, 0);
                dec.decodeAudio(buf.data(), buf.size());
                audNs.append(sw.nsecsElapsed() / 1e9);
            }
            dec.close();
            if (audNs.size() >= 200) break;
        }
        if (audNs.size() >= 200) break;
    }
    const TimeStats aud = timeStats(audNs);
    out << "[4] Áudio por corte (abrir + seek + 1º buffer de 64 KB)\n";
    out << "  cortes: " << aud.n << "\n";
    out << "  por corte: média " << ms(aud.mean) << "  p95 " << ms(aud.p95)
        << "  pior " << ms(aud.max) << "\n";

    // ── Memória ─────────────────────────────────────────────────────────────
    const qint64 hwmKb = processVmHwm();
    out << "[5] Memória\n";
    out << "  pico (VmHWM): " << (hwmKb < 0 ? QStringLiteral("n/d")
                                            : QString::number(hwmKb / 1024.0, 'f', 0) + " MB")
        << "\n";

    // ── Veredito simples ────────────────────────────────────────────────────
    out << "──────────────────────────────────────────────\n";
    out << "Veredito: ";
    if (cut.n == 0) out << "nenhum clipe de vídeo decodificável no projeto.\n";
    else {
        const bool cutOk = cut.p95 < 0.15;
        const bool strOk = frame.n > 0 && (frame.sum / frame.n) * project.fps < 1.0;
        const bool seekOk = seek.p95 < 0.25;
        if (cutOk && strOk && seekOk) out << "motor aguenta (cortes ~rápidos, streaming em tempo real).\n";
        else {
            out << "motor tem gargalos:\n";
            if (!cutOk) out << "  - corte (p95) acima de 150ms → abrir/seek do clipe é o gargalo.\n";
            if (!strOk) out << "  - streaming não acompanha a timeline → decode de CPU é o gargalo.\n";
            if (!seekOk) out << "  - seek (p95) acima de 250ms → navegação vai travar a agulha.\n";
        }
    }
    out << "(bench sintético: um decoder por clipe = pior caso. Cache/prefetch reais "
           "costumam ser melhores.)\n";
}

} // namespace

int runBench(QApplication& app, const QString& projectPath) {
    QTextStream out(stdout);
    out << "Pierrot — harness de stress\n";
    out << "Projeto: " << projectPath << "\n";
    out << "──────────────────────────────────────────────\n";

    QFile f(projectPath);
    if (!f.open(QIODevice::ReadOnly)) {
        out << "ERRO: não consegui abrir o arquivo: " << projectPath << "\n";
        return 1;
    }
    QJsonParseError parseErr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        out << "ERRO: arquivo de projeto inválido (" << parseErr.errorString() << ")\n";
        return 1;
    }

    Project project;
    project.fromJson(doc.object());
    benchReport(out, project);

    Q_UNUSED(app);
    return 0;
}

// Gera um projeto de teste com 200 cortes usando os arquivos de mídia dados e
// já roda o bench em cima dele (uma passada para validar 200 cortes/4K).
int runStress(QApplication& app, const QString& outputPath, const QStringList& mediaFiles) {
    QTextStream out(stdout);
    out << "Pierrot — gerando projeto de stress (200 cortes)\n";
    out << "Saída: " << outputPath << "\n";
    out << "Mídia: " << mediaFiles.join(QStringLiteral(", ")) << "\n";
    out << "──────────────────────────────────────────────\n";

    constexpr int kCut = 200;
    struct Src {
        QString path;
        FFmpegMediaInfo info;
        double piece = 2.0;
    };
    QVector<Src> srcs;
    for (const QString& p : mediaFiles) {
        Src s;
        s.path = p;
        s.info = FFmpegDecoder::probe(p);
        if (!s.info.hasVideo || s.info.duration <= 0.0) {
            out << "  (ignorando sem vídeo: " << QFileInfo(p).fileName()
                << " — dur " << QString::number(s.info.duration, 'f', 1) << "s)\n";
            continue;
        }
        s.piece = std::min(2.0, s.info.duration * 0.8);
        srcs.append(s);
    }
    if (srcs.isEmpty()) {
        out << "ERRO: nenhum arquivo com vídeo para gerar os cortes.\n";
        return 1;
    }

    Project project;
    project.name = QStringLiteral("Stress — 200 cortes");
    project.width = srcs.first().info.width > 0 ? srcs.first().info.width : 1920;
    project.height = srcs.first().info.height > 0 ? srcs.first().info.height : 1080;
    {
        const int f = int(srcs.first().info.fps + 0.5);
        project.fps = std::clamp(f >= 15 ? f : 30, 15, 240);
    }

    for (int i = 0; i < srcs.size(); ++i) {
        MediaItem m;
        m.id = QStringLiteral("mid%1").arg(i);
        m.filePath = srcs[i].path;
        m.name = QFileInfo(srcs[i].path).fileName();
        m.duration = srcs[i].info.duration;
        m.width = srcs[i].info.width;
        m.height = srcs[i].info.height;
        m.hasVideo = srcs[i].info.hasVideo;
        m.hasAudio = srcs[i].info.hasAudio;
        m.audioStreams = srcs[i].info.audioStreams;
        m.audioChannels = srcs[i].info.audioChannels;
        project.media.append(m);
    }

    Track track;
    track.id = newId();
    track.name = QStringLiteral("Vídeo 1");
    for (int i = 0; i < kCut; ++i) {
        const Src& s = srcs[i % srcs.size()];
        Clip c;
        c.id = newId();
        c.mediaId = QStringLiteral("mid%1").arg(i % srcs.size());
        c.pos = i * s.piece;
        c.in = std::fmod(i * s.piece, std::max(s.info.duration * 0.8, s.piece));
        c.dur = s.piece;
        c.speed = 1.0;
        c.name = QFileInfo(s.path).fileName();
        track.clips.append(c);
    }
    project.videoTracks.append(track);

    QFile outf(outputPath);
    if (!outf.open(QIODevice::WriteOnly)) {
        out << "ERRO: não consegui gravar " << outputPath << "\n";
        return 1;
    }
    const QJsonDocument doc(project.toJson());
    outf.write(doc.toJson(QJsonDocument::Indented));
    outf.close();
    out << "Gravado. Cortes: " << int(track.clips.size()) << "\n";
    out << "──────────────────────────────────────────────\n";

    benchReport(out, project);

    Q_UNUSED(app);
    return 0;
}