// Pierrot — editor de vídeo estilo Vegas Pro
//
// Copyright (C) 2026 Pierrot contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "Project.h"

#include <QJsonArray>
#include <QJsonValue>

static QJsonObject mediaToJson(const MediaItem& m) {
    QJsonObject o;
    o["id"] = m.id;
    o["filePath"] = m.filePath;
    o["name"] = m.name;
    o["duration"] = m.duration;
    o["width"] = m.width;
    o["height"] = m.height;
    o["hasVideo"] = m.hasVideo;
    o["hasAudio"] = m.hasAudio;
    return o;
}

static MediaItem mediaFromJson(const QJsonObject& o) {
    MediaItem m;
    m.id = o["id"].toString();
    m.filePath = o["filePath"].toString();
    m.name = o["name"].toString();
    m.duration = o["duration"].toDouble();
    m.width = o["width"].toInt();
    m.height = o["height"].toInt();
    m.hasVideo = o["hasVideo"].toBool();
    m.hasAudio = o["hasAudio"].toBool();
    return m;
}

static QJsonArray kfToJson(const QVector<Keyframe>& v) {
    QJsonArray a;
    for (const Keyframe& k : v) {
        QJsonObject o;
        o["t"] = k.time;
        o["v"] = k.value;
        if (k.interp != KfLinear || k.hx != 0.0 || k.hy != 0.0) {
            o["i"] = k.interp;
            o["hx"] = k.hx;
            o["hy"] = k.hy;
        }
        a.append(o);
    }
    return a;
}

static QVector<Keyframe> kfFromJson(const QJsonValue& val) {
    QVector<Keyframe> v;
    const QJsonArray a = val.toArray();
    for (const QJsonValue& j : a) {
        const QJsonObject o = j.toObject();
        Keyframe k;
        k.time = o["t"].toDouble();
        k.value = o["v"].toDouble();
        k.interp = o["i"].toInt(KfLinear);
        k.hx = o["hx"].toDouble(0.0);
        k.hy = o["hy"].toDouble(0.0);
        v.append(k);
    }
    std::sort(v.begin(), v.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    return v;
}

static QJsonObject clipToJson(const Clip& c) {
    QJsonObject o;
    o["id"] = c.id;
    o["mediaId"] = c.mediaId;
    o["groupId"] = c.groupId;
    o["pos"] = c.pos;
    o["in"] = c.in;
    o["dur"] = c.dur;
    o["name"] = c.name;
    o["volume"] = c.volume;
    o["opacity"] = c.opacity;
    o["fadeIn"] = c.fadeIn;
    o["fadeOut"] = c.fadeOut;
    o["speed"] = c.speed;
    o["text"] = c.text;
    o["brightness"] = c.brightness;
    o["contrast"] = c.contrast;
    o["saturation"] = c.saturation;
    o["blur"] = c.blur;
    o["grayscale"] = c.grayscale;
    o["chromaKey"] = c.chromaKey;
    o["chromaKeyColor"] = c.chromaKeyColor.name();
    o["chromaKeySimilarity"] = c.chromaKeySimilarity;
    o["tx"] = c.tx;
    o["ty"] = c.ty;
    o["scale"] = c.scale;
    o["rotation"] = c.rotation;
    o["cropL"] = c.cropL;
    o["cropR"] = c.cropR;
    o["cropT"] = c.cropT;
    o["cropB"] = c.cropB;
    o["eqLow"] = c.eqLow;
    o["eqMid"] = c.eqMid;
    o["eqHigh"] = c.eqHigh;
    o["denoise"] = c.denoise;
    o["denoiseAmount"] = c.denoiseAmount;
    o["normalize"] = c.normalize;
    o["invertPhase"] = c.invertPhase;
    o["kfOpacity"] = kfToJson(c.kfOpacity);
    o["kfVolume"] = kfToJson(c.kfVolume);
    o["kfTx"] = kfToJson(c.kfTx);
    o["kfTy"] = kfToJson(c.kfTy);
    o["kfScale"] = kfToJson(c.kfScale);
    o["kfRotation"] = kfToJson(c.kfRotation);
    o["kfCropL"] = kfToJson(c.kfCropL);
    o["kfCropR"] = kfToJson(c.kfCropR);
    o["kfCropT"] = kfToJson(c.kfCropT);
    o["kfCropB"] = kfToJson(c.kfCropB);
    return o;
}

static Clip clipFromJson(const QJsonObject& o) {
    Clip c;
    c.id = o["id"].toString();
    c.mediaId = o["mediaId"].toString();
    c.groupId = o["groupId"].toString();
    c.pos = o["pos"].toDouble();
    c.in = o["in"].toDouble();
    c.dur = o["dur"].toDouble();
    c.name = o["name"].toString();
    c.volume = o["volume"].toDouble(1.0);
    c.opacity = o["opacity"].toDouble(1.0);
    c.fadeIn = o["fadeIn"].toDouble(0.0);
    c.fadeOut = o["fadeOut"].toDouble(0.0);
    c.speed = o["speed"].toDouble(1.0);
    c.text = o["text"].toString();
    c.brightness = o["brightness"].toDouble(0.0);
    c.contrast = o["contrast"].toDouble(1.0);
    c.saturation = o["saturation"].toDouble(1.0);
    c.blur = o["blur"].toDouble(0.0);
    c.grayscale = o["grayscale"].toBool();
    c.chromaKey = o["chromaKey"].toBool();
    const QString ckc = o["chromaKeyColor"].toString();
    if (QColor::isValidColorName(ckc)) c.chromaKeyColor = QColor(ckc);
    c.chromaKeySimilarity = o["chromaKeySimilarity"].toDouble(0.15);
    c.tx = o["tx"].toDouble(0.0);
    c.ty = o["ty"].toDouble(0.0);
    c.scale = o["scale"].toDouble(1.0);
    c.rotation = o["rotation"].toDouble(0.0);
    c.cropL = o["cropL"].toDouble(0.0);
    c.cropR = o["cropR"].toDouble(0.0);
    c.cropT = o["cropT"].toDouble(0.0);
    c.cropB = o["cropB"].toDouble(0.0);
    c.eqLow = o["eqLow"].toDouble(0.0);
    c.eqMid = o["eqMid"].toDouble(0.0);
    c.eqHigh = o["eqHigh"].toDouble(0.0);
    c.denoise = o["denoise"].toBool();
    c.denoiseAmount = o["denoiseAmount"].toDouble(12.0);
    c.normalize = o["normalize"].toBool();
    c.invertPhase = o["invertPhase"].toBool();
    c.kfOpacity = kfFromJson(o["kfOpacity"]);
    c.kfVolume = kfFromJson(o["kfVolume"]);
    c.kfTx = kfFromJson(o["kfTx"]);
    c.kfTy = kfFromJson(o["kfTy"]);
    c.kfScale = kfFromJson(o["kfScale"]);
    c.kfRotation = kfFromJson(o["kfRotation"]);
    c.kfCropL = kfFromJson(o["kfCropL"]);
    c.kfCropR = kfFromJson(o["kfCropR"]);
    c.kfCropT = kfFromJson(o["kfCropT"]);
    c.kfCropB = kfFromJson(o["kfCropB"]);
    return c;
}

static QJsonObject markerToJson(const Marker& m) {
    QJsonObject o;
    o["id"] = m.id;
    o["time"] = m.time;
    o["name"] = m.name;
    o["color"] = m.color.name();
    return o;
}

static Marker markerFromJson(const QJsonObject& o) {
    Marker m;
    m.id = o["id"].toString();
    m.time = o["time"].toDouble();
    m.name = o["name"].toString();
    const QString col = o["color"].toString();
    if (QColor::isValidColorName(col)) m.color = QColor(col);
    return m;
}

static QJsonObject trackToJson(const Track& t) {
    QJsonObject o;
    o["name"] = t.name;
    o["audio"] = t.audio;
    o["blendMode"] = t.blendMode;
    o["volume"] = t.volume;
    o["muted"] = t.muted;
    o["solo"] = t.solo;
    o["locked"] = t.locked;
    o["height"] = t.height;
    QJsonArray clips;
    for (const Clip& c : t.clips) clips.append(clipToJson(c));
    o["clips"] = clips;
    return o;
}

static Track trackFromJson(const QJsonObject& o, bool audio) {
    Track t;
    t.name = o["name"].toString(audio ? QStringLiteral("Faixa de Áudio")
                                      : QStringLiteral("Faixa de Vídeo"));
    t.audio = audio;
    t.blendMode = o["blendMode"].toString(QStringLiteral("normal"));
    t.volume = o["volume"].toDouble(1.0);
    t.muted = o["muted"].toBool();
    t.solo = o["solo"].toBool();
    t.locked = o["locked"].toBool();
    t.height = o["height"].toInt(0);
    const QJsonArray clips = o["clips"].toArray();
    for (const QJsonValue& v : clips)
        t.clips.append(clipFromJson(v.toObject()));
    return t;
}

QJsonObject Project::toJson() const {
    QJsonObject o;
    o["name"] = name;
    o["width"] = width;
    o["height"] = height;
    o["fps"] = fps;
    o["audioRate"] = audioRate;

    QJsonArray mediaArr;
    for (const MediaItem& m : media) mediaArr.append(mediaToJson(m));
    o["media"] = mediaArr;

    QJsonArray vt;
    for (const Track& t : videoTracks) vt.append(trackToJson(t));
    o["videoTracks"] = vt;

    QJsonArray at;
    for (const Track& t : audioTracks) at.append(trackToJson(t));
    o["audioTracks"] = at;

    QJsonArray mks;
    for (const Marker& m : markers) mks.append(markerToJson(m));
    o["markers"] = mks;

    return o;
}

void Project::fromJson(const QJsonObject& o) {
    name = o["name"].toString();
    width = o["width"].toInt(1920);
    height = o["height"].toInt(1080);
    fps = o["fps"].toInt(30);
    audioRate = o["audioRate"].toDouble(48000.0);

    media.clear();
    videoTracks.clear();
    audioTracks.clear();

    const QJsonArray ma = o["media"].toArray();
    for (const QJsonValue& v : ma) media.append(mediaFromJson(v.toObject()));

    const QJsonArray vta = o["videoTracks"].toArray();
    for (const QJsonValue& v : vta) videoTracks.append(trackFromJson(v.toObject(), false));

    const QJsonArray ata = o["audioTracks"].toArray();
    for (const QJsonValue& v : ata) audioTracks.append(trackFromJson(v.toObject(), true));

    markers.clear();
    const QJsonArray mka = o["markers"].toArray();
    for (const QJsonValue& v : mka) markers.append(markerFromJson(v.toObject()));
}
