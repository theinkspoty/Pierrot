// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "NleInterchange.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QHash>
#include <QSet>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace NleInterchange {

// ── Helpers internos ─────────────────────────────────────────────────────

namespace {

// HH:MM:SS:FF → segundos.
double timecodeToSeconds(const QString& tc, int fps) {
    const QStringList p = tc.split(QLatin1Char(':'));
    if (p.size() != 4) return 0.0;
    const double h = p[0].toDouble();
    const double m = p[1].toDouble();
    const double s = p[2].toDouble();
    const double f = p[3].toDouble();
    const double fr = fps > 0 ? fps : 1.0;
    return h * 3600.0 + m * 60.0 + s + f / fr;
}

// Nome de faixa a partir do canal EDL (número, 0-based).
QString trackNameFor(int channel, bool audio) {
    return audio ? QStringLiteral("Audio %1").arg(channel + 1)
                 : QStringLiteral("Video %1").arg(channel + 1);
}

// Um reel só é "resolvido" (vira filePath) se já for um caminho absoluto
// existente em disco. Do contrário fica como nome, para o usuário resolver.
bool reelIsFile(const QString& reel) {
    return QDir::isAbsolutePath(reel) && QFileInfo::exists(reel);
}

} // namespace

// ── EDL: times e linhas ─────────────────────────────────────────────────

QString edlTimecode(double seconds, int fps) {
    seconds = std::max(0.0, seconds);
    const int fr = fps > 0 ? fps : 30;
    const int totalFrames = (int)std::llround(seconds * fr);
    const int f = totalFrames % fr;
    const int totalSec = totalFrames / fr;
    const int s = totalSec % 60;
    const int totalMin = totalSec / 60;
    const int m = totalMin % 60;
    const int h = totalMin / 60;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(f, 2, 10, QLatin1Char('0'));
}

QString edlEditLine(int eventNumber, const QString& reel, const QString& trackId,
                    bool isAudio, const QString& transition, double srcIn,
                    double srcOut, double recIn, double recOut, int fps) {
    // CMX3600: "001  REEL  V  D  C  00:00:00:00 00:00:00:00 00:00:00:00 00:00:00:00"
    Q_UNUSED(trackId); // o tipo de canal não entra na linha (V/AA são fixos).
    const QString chan = isAudio ? QStringLiteral("AA") : QStringLiteral("V");
    const QString reelName = reel.isEmpty() ? QStringLiteral("REEL") : reel.left(8);
    const QString trans = transition.isEmpty() ? QStringLiteral("C")
                                               : transition.left(1);
    return QStringLiteral("%1  %2  %3  %4  %5 %6 %7 %8 %9")
        .arg(eventNumber, 3, 10, QLatin1Char('0'))
        .arg(reelName.leftJustified(8, QLatin1Char(' ')))
        .arg(chan)
        .arg(trans)
        .arg(QStringLiteral("C"), -1)  // coluna "Cut/Des" — mantemos C fixo p/ simplicidade
        .arg(edlTimecode(srcIn, fps))
        .arg(edlTimecode(srcOut, fps))
        .arg(edlTimecode(recIn, fps))
        .arg(edlTimecode(recOut, fps));
}

QString buildEdl(const Project& project) {
    const int fps = project.fps > 0 ? project.fps : 30;
    QString out;
    out += QStringLiteral("TITLE: %1\n").arg(project.name.isEmpty()
                                           ? QStringLiteral("Pierrot Project")
                                           : project.name);
    out += QStringLiteral("FCM: NON-DROP FRAME\n\n");

    int ev = 1;
    auto emitTrack = [&](const Track& tr, bool audio) {
        QVector<const Clip*> ordered;
        for (const Clip& c : tr.clips) ordered.push_back(&c);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Clip* a, const Clip* b) { return a->pos < b->pos; });
        for (const Clip* c : ordered) {
            const MediaItem* m = project.findMedia(c->mediaId);
            if (!m) continue;
            if (audio && !m->hasAudio) continue;
            if (!audio && !m->hasVideo && !m->isSolid) continue;
            const QString reel = m->isSolid ? QStringLiteral("GENERATED")
                                            : QFileInfo(m->filePath).baseName();
            out += edlEditLine(ev++, reel, tr.name, audio, QStringLiteral("C"),
                               c->in, c->in + c->dur, c->pos, c->pos + c->dur, fps);
            out += QLatin1Char('\n');
        }
    };

    for (const Track& tr : project.videoTracks)
        emitTrack(tr, false);
    for (const Track& tr : project.audioTracks)
        emitTrack(tr, true);

    out += QLatin1Char('\n');
    out += QStringLiteral(
        "// Gerado por Pierrot. EDL CMX3600 representa apenas cortes;\n"
        "// efeitos, transformação, texto e blend não são preservados.\n");
    return out;
}

bool exportEdl(const Project& project, const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("Não foi possível gravar: %1").arg(path);
        return false;
    }
    QTextStream ts(&f);
    ts << buildEdl(project);
    f.close();
    return true;
}

// ── Importação EDL ─────────────────────────────────────────────────────

bool importEdl(const QString& path, EdlImportResult& result) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.warnings << QStringLiteral("Não foi possível abrir: %1").arg(path);
        return false;
    }
    QTextStream ts(&f);
    const QString content = ts.readAll();
    f.close();

    result.project.name = QFileInfo(path).completeBaseName();
    result.project.width = 1920;
    result.project.height = 1080;
    result.project.fps = 30; // EDL não carrega fps; default 30 (ajustável depois).

    const int fps = result.project.fps;

    // Formato: "NNN  REEL  CANAL  TRANS  (…)  srcIn srcOut recIn recOut"
    // Campos separados por 1+ espaços. Mantemos o parse por regex ancorado.
    QRegularExpression editRe(
        QStringLiteral("^(\\d{3})\\s+(\\S+)\\s+(\\S+)\\s+(\\S+)\\s+(\\S+)\\s+"
                       "(\\S+)\\s+(\\S+)\\s+(\\S+)\\s+(\\S+)"));

    struct ParsedEdit {
        QString reel;
        bool audio;
        int channel;
        double srcIn, srcOut, recIn, recOut;
    };
    QVector<ParsedEdit> edits;
    QSet<QString> unresolved;

    const QStringList lines = content.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))
            || line.startsWith(QLatin1String("//")))
            continue;
        if (line.startsWith(QLatin1String("TITLE:"))
            || line.startsWith(QLatin1String("FCM:")))
            continue;

        const QRegularExpressionMatch m = editRe.match(line);
        if (!m.hasMatch()) {
            result.warnings << QStringLiteral("Linha ignorada: %1").arg(line);
            continue;
        }
        const QString chan = m.captured(3);
        const QString trans = m.captured(4);
        // Canal "V" é vídeo; "AA/A/A2…" é áudio.
        const bool audio = (chan.compare(QLatin1String("V"), Qt::CaseInsensitive) != 0);
        // Número do canal a partir de dígitos em "A2"/"AA"/"V".
        int channelNum = 0;
        {
            QString digits;
            for (const QChar ch : chan) if (ch.isDigit()) digits.append(ch);
            if (!digits.isEmpty()) channelNum = digits.toInt();
        }
        // Ignora events não-Cut (dissolve W, etc.) no import v1.
        if (!trans.isEmpty() && trans.left(1) != QLatin1Char('C')) {
            result.warnings << QStringLiteral("Transição não-Cut ignorada: %1").arg(line);
            continue;
        }

        ParsedEdit pe;
        pe.reel = m.captured(2);
        pe.audio = audio;
        pe.channel = channelNum;
        pe.srcIn  = timecodeToSeconds(m.captured(6), fps);
        pe.srcOut = timecodeToSeconds(m.captured(7), fps);
        pe.recIn  = timecodeToSeconds(m.captured(8), fps);
        pe.recOut = timecodeToSeconds(m.captured(9), fps);
        if (pe.recOut <= pe.recIn || pe.srcOut <= pe.srcIn) {
            result.warnings << QStringLiteral("Edit vazio ignorado: %1").arg(line);
            continue;
        }
        edits.append(pe);
        if (!reelIsFile(pe.reel))
            unresolved.insert(pe.reel);
    }

    if (edits.isEmpty()) {
        result.warnings << QStringLiteral("Nenhum edit reconhecido no EDL.");
        return true;
    }

    // Reels → MediaItem (deduplicado por nome).
    QHash<QString, QString> reelToMediaId;
    for (const ParsedEdit& e : edits) {
        if (reelToMediaId.contains(e.reel)) continue;
        MediaItem mi;
        mi.id = newId();
        mi.name = e.reel;
        mi.filePath = reelIsFile(e.reel) ? e.reel : QString();
        mi.hasVideo = true;
        mi.hasAudio = true;
        result.project.media.append(mi);
        reelToMediaId.insert(e.reel, mi.id);
    }

    // Canal → índice de faixa (vídeo e áudio separados).
    QHash<QPair<bool,int>, int> channelToTrack;
    auto ensureTrack = [&](bool audio, int channel) -> Track* {
        QVector<Track>& tracks = audio ? result.project.audioTracks
                                       : result.project.videoTracks;
        const auto key = qMakePair(audio, channel);
        if (channelToTrack.contains(key))
            return &tracks[channelToTrack.value(key)];
        Track t;
        t.id = newId();
        t.audio = audio;
        t.name = trackNameFor(channel, audio);
        tracks.append(t);
        channelToTrack.insert(key, tracks.size() - 1);
        return &tracks.last();
    };

    for (const ParsedEdit& e : edits) {
        Track* tr = ensureTrack(e.audio, e.channel);
        Clip c;
        c.id = newId();
        c.mediaId = reelToMediaId.value(e.reel);
        c.pos = e.recIn;
        c.in = e.srcIn;
        c.dur = e.recOut - e.recIn;
        c.name = e.reel;
        tr->clips.append(c);
    }

    result.unresolvedReels = unresolved.values();
    result.unresolvedReels.sort();
    return true;
}

} // namespace NleInterchange
