// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

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
    o["audioStreams"] = m.audioStreams;
    QJsonArray ch;
    for (int c : m.audioChannels) ch.append(c);
    o["audioChannels"] = ch;
    o["isSolid"] = m.isSolid;
    o["solidColor"] = m.solidColor.name(QColor::HexArgb);
    o["generator"] = m.generator;
    o["solidColor2"] = m.solidColor2.name(QColor::HexArgb);
    o["genCells"] = m.genCells;
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
    m.audioStreams = o["audioStreams"].toInt();
    const QJsonArray ch = o["audioChannels"].toArray();
    for (const QJsonValue& v : ch) m.audioChannels.append(v.toInt());
    m.isSolid = o["isSolid"].toBool();
    const QString sc = o["solidColor"].toString();
    if (QColor::isValidColorName(sc)) m.solidColor = QColor(sc);
    m.generator = o["generator"].toString();
    const QString sc2 = o["solidColor2"].toString();
    if (QColor::isValidColorName(sc2)) m.solidColor2 = QColor(sc2);
    m.genCells = o["genCells"].toInt(8);
    return m;
}

static QJsonArray kfToJson(const QVector<Keyframe>& v) {
    QJsonArray a;
    for (const Keyframe& k : v) {
        QJsonObject o;
        o["t"] = k.time;
        o["v"] = k.value;
        if (k.interp != KfLinear || k.ox != 0.0 || k.oy != 0.0
            || k.ix != 0.0 || k.iy != 0.0) {
            o["i"] = k.interp;
            o["ox"] = k.ox;
            o["oy"] = k.oy;
            o["ix"] = k.ix;
            o["iy"] = k.iy;
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
        if (o.contains("hx") || o.contains("hy")) {
            // Projeto antigo (handle único): preserva a curva. A posição
            // horizontal do controle (u = 1/3) é resolvida depois, com os
            // vizinhos; marca com -1.
            const double hy = o["hy"].toDouble(0.0);
            k.oy = hy;
            k.iy = -hy;
            k.ox = (k.interp == KfBezier) ? -1.0 : 0.0;
            k.ix = k.ox;
        } else {
            k.ox = o["ox"].toDouble(0.0);
            k.oy = o["oy"].toDouble(0.0);
            k.ix = o["ix"].toDouble(0.0);
            k.iy = o["iy"].toDouble(0.0);
        }
        v.append(k);
    }
    std::sort(v.begin(), v.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    // Projetos antigos: posiciona o controle real (u = 1/3) do bezier.
    for (int i = 0; i < v.size(); ++i) {
        Keyframe& k = v[i];
        if (k.interp != KfBezier || k.ox >= 0.0) continue;
        const double spanR = (i + 1 < v.size()) ? v[i + 1].time - k.time : 0.0;
        const double spanL = (i > 0) ? k.time - v[i - 1].time : 0.0;
        const double span = (spanR > 1e-9) ? spanR : spanL;
        k.ox = (span > 1e-9) ? span / 3.0 : 0.0;
        k.ix = k.ox;
    }
    return v;
}

static QJsonObject textStyleToJson(const TextStyle& t) {
    QJsonObject o;
    o["text"] = t.text;
    o["fontFamily"] = t.fontFamily;
    o["textSize"] = t.textSize;
    o["textBold"] = t.textBold;
    o["textColor"] = t.textColor.name(QColor::HexArgb);
    o["textOutline"] = t.textOutline;
    o["textOutlineColor"] = t.textOutlineColor.name(QColor::HexArgb);
    o["textBackground"] = t.textBackground;
    o["textBackgroundColor"] = t.textBackgroundColor.name(QColor::HexArgb);
    o["textX"] = t.textX;
    o["textY"] = t.textY;
    o["textAlign"] = t.textAlign;
    return o;
}

static TextStyle textStyleFromJson(const QJsonObject& o) {
    TextStyle t;
    t.text = o["text"].toString();
    t.fontFamily = o["fontFamily"].toString();
    t.textSize = o["textSize"].toDouble(0.0);
    t.textBold = o["textBold"].toBool(true);
    const QString tc = o["textColor"].toString();
    if (QColor::isValidColorName(tc)) t.textColor = QColor(tc);
    t.textOutline = o["textOutline"].toDouble(0.0);
    const QString toc = o["textOutlineColor"].toString();
    if (QColor::isValidColorName(toc)) t.textOutlineColor = QColor(toc);
    t.textBackground = o["textBackground"].toBool(false);
    const QString tbc = o["textBackgroundColor"].toString();
    if (QColor::isValidColorName(tbc)) t.textBackgroundColor = QColor(tbc);
    t.textX = o["textX"].toDouble(0.5);
    t.textY = o["textY"].toDouble(0.5);
    t.textAlign = o["textAlign"].toInt(0);
    return t;
}

// ── Mesa (composição 2D) ────────────────────────────────────────────────

static QJsonObject mesaToJson(const MesaComposition& m) {
    QJsonObject o;
    o["id"] = m.id;
    o["name"] = m.name;
    o["canvasW"] = m.canvasW;
    o["canvasH"] = m.canvasH;
    QJsonArray tids;
    for (const QString& tid : m.trackIds) tids.append(tid);
    o["trackIds"] = tids;
    o["camX"] = m.camX;
    o["camY"] = m.camY;
    o["camZoom"] = m.camZoom;
    o["camRotation"] = m.camRotation;
    o["kfCamX"] = kfToJson(m.kfCamX);
    o["kfCamY"] = kfToJson(m.kfCamY);
    o["kfCamZoom"] = kfToJson(m.kfCamZoom);
    o["kfCamRotation"] = kfToJson(m.kfCamRotation);
    o["motionBlur"] = m.motionBlur;
    o["motionBlurSamples"] = m.motionBlurSamples;
    o["motionBlurShutter"] = m.motionBlurShutter;
    return o;
}

static MesaComposition mesaFromJson(const QJsonObject& o) {
    MesaComposition m;
    m.id = o["id"].toString();
    m.name = o["name"].toString();
    m.canvasW = o["canvasW"].toInt(1920);
    m.canvasH = o["canvasH"].toInt(1080);
    const QJsonArray tids = o["trackIds"].toArray();
    for (const QJsonValue& v : tids) m.trackIds.append(v.toString());
    m.camX = o["camX"].toDouble(0.0);
    m.camY = o["camY"].toDouble(0.0);
    m.camZoom = o["camZoom"].toDouble(1.0);
    m.camRotation = o["camRotation"].toDouble(0.0);
    m.kfCamX = kfFromJson(o["kfCamX"]);
    m.kfCamY = kfFromJson(o["kfCamY"]);
    m.kfCamZoom = kfFromJson(o["kfCamZoom"]);
    m.kfCamRotation = kfFromJson(o["kfCamRotation"]);
    m.motionBlur = o["motionBlur"].toBool(false);
    m.motionBlurSamples = o["motionBlurSamples"].toInt(8);
    m.motionBlurShutter = o["motionBlurShutter"].toDouble(0.5);
    return m;
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
    o["transitionType"] = c.transitionType;
    o["audioStreamIndex"] = c.audioStreamIndex;
    o["volume"] = c.volume;
    o["opacity"] = c.opacity;
    o["fadeIn"] = c.fadeIn;
    o["fadeOut"] = c.fadeOut;
    o["speed"] = c.speed;
    o["isText"] = c.isText;
    o["textResourceId"] = c.textResourceId;
    o["textStyle"] = textStyleToJson(c.text);
    o["brightness"] = c.brightness;
    o["contrast"] = c.contrast;
    o["saturation"] = c.saturation;
    o["blur"] = c.blur;
    o["grayscale"] = c.grayscale;
    o["chromaKey"] = c.chromaKey;
    o["chromaKeyColor"] = c.chromaKeyColor.name();
    o["chromaKeySimilarity"] = c.chromaKeySimilarity;
    o["liftR"] = c.liftR; o["liftG"] = c.liftG; o["liftB"] = c.liftB;
    o["gammaR"] = c.gammaR; o["gammaG"] = c.gammaG; o["gammaB"] = c.gammaB;
    o["gainR"] = c.gainR; o["gainG"] = c.gainG; o["gainB"] = c.gainB;
    o["lainkaEnabled"] = c.lainkaEnabled;
    o["lainkaSkip"] = c.lainkaSkip;
    o["lainkaJitterPos"] = c.lainkaJitterPos;
    o["lainkaJitterRot"] = c.lainkaJitterRot;
    o["lainkaJitterScale"] = c.lainkaJitterScale;
    o["lainkaFlicker"] = c.lainkaFlicker;
    o["lainkaFlickerSpeed"] = c.lainkaFlickerSpeed;
    o["lainkaWarpAmount"] = c.lainkaWarpAmount;
    o["lainkaWarpSpeed"] = c.lainkaWarpSpeed;
    o["lainkaWarpGrid"] = c.lainkaWarpGrid;
    o["lainkaOnionSkin"] = c.lainkaOnionSkin;
    o["lainkaDustAmount"] = c.lainkaDustAmount;
    o["lainkaScratchAmount"] = c.lainkaScratchAmount;
    o["lainkaTargetFps"] = c.lainkaTargetFps;
    o["lainkaMotionBlur"] = c.lainkaMotionBlur;
    o["lainkaOpacity"] = c.lainkaOpacity;
    o["lainkaAntialias"] = c.lainkaAntialias;
    o["motionEnabled"] = c.motionEnabled;
    o["motionAmount"] = c.motionAmount;
    o["motionAngle"] = c.motionAngle;
    o["motionSamples"] = c.motionSamples;
    o["tx"] = c.tx;
    o["ty"] = c.ty;
    o["scale"] = c.scale;
    o["scaleX"] = c.scaleX;
    o["scaleY"] = c.scaleY;
    o["rotation"] = c.rotation;
    o["anchorX"] = c.anchorX;
    o["anchorY"] = c.anchorY;
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
    o["reverb"] = c.reverb;
    o["reverbMix"] = c.reverbMix;
    o["reverbSize"] = c.reverbSize;
    o["kfOpacity"] = kfToJson(c.kfOpacity);
    o["kfVolume"] = kfToJson(c.kfVolume);
    o["kfTx"] = kfToJson(c.kfTx);
    o["kfTy"] = kfToJson(c.kfTy);
    o["kfScale"] = kfToJson(c.kfScale);
    o["kfScaleX"] = kfToJson(c.kfScaleX);
    o["kfScaleY"] = kfToJson(c.kfScaleY);
    o["kfRotation"] = kfToJson(c.kfRotation);
    o["kfAnchorX"] = kfToJson(c.kfAnchorX);
    o["kfAnchorY"] = kfToJson(c.kfAnchorY);
    o["kfCropL"] = kfToJson(c.kfCropL);
    o["kfCropR"] = kfToJson(c.kfCropR);
    o["kfCropT"] = kfToJson(c.kfCropT);
    o["kfCropB"] = kfToJson(c.kfCropB);
    o["kfSpeed"] = kfToJson(c.kfSpeed);
    // ── Efeitos OFX ─────────────────────────────────────────────────────
    QJsonArray ofxArr;
    for (const OfxPluginInstance& fx : c.ofxFx) {
        QJsonObject fxo;
        fxo["pluginId"] = fx.pluginId;
        fxo["enabled"] = fx.enabled;
        QJsonArray pArr;
        for (const OfxParam& p : fx.params) {
            QJsonObject po;
            po["key"] = p.key;
                if (p.value.metaType().id() == QMetaType::QColor)
                po["value"] = p.value.value<QColor>().name(QColor::HexArgb);
            else
                po["value"] = QJsonValue::fromVariant(p.value);
            pArr.append(po);
        }
        fxo["params"] = pArr;
        ofxArr.append(fxo);
    }
    o["ofxFx"] = ofxArr;
    // ── Máscaras ─────────────────────────────────────────────────────────
    QJsonArray maskArr;
    for (const Mask& m : c.masks) {
        QJsonObject mo;
        mo["type"] = m.type;
        mo["enabled"] = m.enabled;
        mo["cx"] = m.cx;
        mo["cy"] = m.cy;
        mo["rx"] = m.rx;
        mo["ry"] = m.ry;
        mo["rotation"] = m.rotation;
        mo["feather"] = m.feather;
        mo["invert"] = m.invert;
        QJsonArray pa;
        for (const QPointF& pt : m.poly) {
            QJsonArray qa;
            qa.append(pt.x());
            qa.append(pt.y());
            pa.append(qa);
        }
        mo["poly"] = pa;
        mo["kfCx"] = kfToJson(m.kfCx);
        mo["kfCy"] = kfToJson(m.kfCy);
        mo["kfRx"] = kfToJson(m.kfRx);
        mo["kfRy"] = kfToJson(m.kfRy);
        mo["kfRotation"] = kfToJson(m.kfRotation);
        mo["kfFeather"] = kfToJson(m.kfFeather);
        maskArr.append(mo);
    }
    o["masks"] = maskArr;
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
    c.transitionType = o["transitionType"].toString();
    c.audioStreamIndex = o["audioStreamIndex"].toInt(0);
    c.volume = o["volume"].toDouble(1.0);
    c.opacity = o["opacity"].toDouble(1.0);
    c.fadeIn = o["fadeIn"].toDouble(0.0);
    c.fadeOut = o["fadeOut"].toDouble(0.0);
    c.speed = o["speed"].toDouble(1.0);
    c.isText = o["isText"].toBool(false);
    c.textResourceId = o["textResourceId"].toString();
    const QJsonObject ts = o["textStyle"].toObject();
    if (!ts.isEmpty()) {
        c.text = textStyleFromJson(ts);
    } else {
        // Projeto antigo: texto simples no campo único "text".
        c.text.text = o["text"].toString();
    }
    c.brightness = o["brightness"].toDouble(0.0);
    c.contrast = o["contrast"].toDouble(1.0);
    c.saturation = o["saturation"].toDouble(1.0);
    c.blur = o["blur"].toDouble(0.0);
    c.grayscale = o["grayscale"].toBool();
    c.chromaKey = o["chromaKey"].toBool();
    const QString ckc = o["chromaKeyColor"].toString();
    if (QColor::isValidColorName(ckc)) c.chromaKeyColor = QColor(ckc);
    c.chromaKeySimilarity = o["chromaKeySimilarity"].toDouble(0.15);
    c.liftR = o["liftR"].toDouble(0.0);
    c.liftG = o["liftG"].toDouble(0.0);
    c.liftB = o["liftB"].toDouble(0.0);
    c.gammaR = o["gammaR"].toDouble(1.0);
    c.gammaG = o["gammaG"].toDouble(1.0);
    c.gammaB = o["gammaB"].toDouble(1.0);
    c.gainR = o["gainR"].toDouble(0.0);
    c.gainG = o["gainG"].toDouble(0.0);
    c.gainB = o["gainB"].toDouble(0.0);
    c.lainkaEnabled = o["lainkaEnabled"].toBool(false);
    c.lainkaSkip = o["lainkaSkip"].toInt(2);
    c.lainkaJitterPos = o["lainkaJitterPos"].toDouble(0.0);
    c.lainkaJitterRot = o["lainkaJitterRot"].toDouble(0.0);
    c.lainkaJitterScale = o["lainkaJitterScale"].toDouble(0.0);
    c.lainkaFlicker = o["lainkaFlicker"].toDouble(0.0);
    c.lainkaFlickerSpeed = o["lainkaFlickerSpeed"].toDouble(50.0);
    c.lainkaWarpAmount = o["lainkaWarpAmount"].toDouble(0.0);
    c.lainkaWarpSpeed = o["lainkaWarpSpeed"].toDouble(50.0);
    c.lainkaWarpGrid = o["lainkaWarpGrid"].toInt(8);
    c.lainkaOnionSkin = o["lainkaOnionSkin"].toDouble(0.0);
    c.lainkaDustAmount = o["lainkaDustAmount"].toDouble(0.0);
    c.lainkaScratchAmount = o["lainkaScratchAmount"].toDouble(0.0);
    c.lainkaTargetFps = o["lainkaTargetFps"].toInt(8);
    c.lainkaMotionBlur = o["lainkaMotionBlur"].toDouble(0.0);
    c.lainkaOpacity = o["lainkaOpacity"].toDouble(100.0);
    c.lainkaAntialias = o["lainkaAntialias"].toInt(1);
    c.motionEnabled = o["motionEnabled"].toBool(false);
    c.motionAmount = o["motionAmount"].toDouble(0.0);
    c.motionAngle = o["motionAngle"].toDouble(0.0);
    c.motionSamples = o["motionSamples"].toInt(8);
    c.tx = o["tx"].toDouble(0.0);
    c.ty = o["ty"].toDouble(0.0);
    c.scale = o["scale"].toDouble(1.0);
    c.scaleX = o["scaleX"].toDouble(1.0);
    c.scaleY = o["scaleY"].toDouble(1.0);
    c.rotation = o["rotation"].toDouble(0.0);
    c.anchorX = o["anchorX"].toDouble(0.0);
    c.anchorY = o["anchorY"].toDouble(0.0);
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
    c.reverb = o["reverb"].toBool();
    c.reverbMix = o["reverbMix"].toDouble(0.35);
    c.reverbSize = o["reverbSize"].toDouble(0.5);
    c.kfOpacity = kfFromJson(o["kfOpacity"]);
    c.kfVolume = kfFromJson(o["kfVolume"]);
    c.kfTx = kfFromJson(o["kfTx"]);
    c.kfTy = kfFromJson(o["kfTy"]);
    c.kfScale = kfFromJson(o["kfScale"]);
    c.kfScaleX = kfFromJson(o["kfScaleX"]);
    c.kfScaleY = kfFromJson(o["kfScaleY"]);
    c.kfRotation = kfFromJson(o["kfRotation"]);
    c.kfAnchorX = kfFromJson(o["kfAnchorX"]);
    c.kfAnchorY = kfFromJson(o["kfAnchorY"]);
    c.kfCropL = kfFromJson(o["kfCropL"]);
    c.kfCropR = kfFromJson(o["kfCropR"]);
    c.kfCropT = kfFromJson(o["kfCropT"]);
    c.kfCropB = kfFromJson(o["kfCropB"]);
    c.kfSpeed = kfFromJson(o["kfSpeed"]);
    // ── Efeitos OFX ─────────────────────────────────────────────────────
    const QJsonArray ofxArr = o["ofxFx"].toArray();
    for (const QJsonValue& v : ofxArr) {
        const QJsonObject fxo = v.toObject();
        OfxPluginInstance fx;
        fx.pluginId = fxo["pluginId"].toString();
        fx.enabled = fxo["enabled"].toBool(true);
        const QJsonArray pArr = fxo["params"].toArray();
        for (const QJsonValue& pv : pArr) {
            const QJsonObject po = pv.toObject();
            OfxParam p;
            p.key = po["key"].toString();
            const QJsonValue val = po["value"];
            if (val.isString()) {
                const QString s = val.toString();
                if (QColor::isValidColorName(s) && s.startsWith('#') && s.length() == 9)
                    p.value = QColor(s);  // HexArgb
                else
                    p.value = s;
            } else if (val.isDouble()) {
                p.value = val.toDouble();
            } else if (val.isBool()) {
                p.value = val.toBool();
            }
            fx.params.append(p);
        }
        c.ofxFx.append(fx);
    }
    // ── Máscaras ─────────────────────────────────────────────────────────
    const QJsonArray maskArr = o["masks"].toArray();
    for (const QJsonValue& v : maskArr) {
        const QJsonObject mo = v.toObject();
        Mask m;
        m.type = mo["type"].toString();
        m.enabled = mo["enabled"].toBool(true);
        m.cx = mo["cx"].toDouble(0.5);
        m.cy = mo["cy"].toDouble(0.5);
        m.rx = mo["rx"].toDouble(0.4);
        m.ry = mo["ry"].toDouble(0.4);
        m.rotation = mo["rotation"].toDouble(0.0);
        m.feather = mo["feather"].toDouble(0.0);
        m.invert = mo["invert"].toBool(false);
        const QJsonArray pa = mo["poly"].toArray();
        for (const QJsonValue& pv : pa) {
            const QJsonArray pta = pv.toArray();
            if (pta.size() >= 2)
                m.poly.append(QPointF(pta.at(0).toDouble(), pta.at(1).toDouble()));
        }
        m.kfCx = kfFromJson(mo["kfCx"]);
        m.kfCy = kfFromJson(mo["kfCy"]);
        m.kfRx = kfFromJson(mo["kfRx"]);
        m.kfRy = kfFromJson(mo["kfRy"]);
        m.kfRotation = kfFromJson(mo["kfRotation"]);
        m.kfFeather = kfFromJson(mo["kfFeather"]);
        if (!m.type.isEmpty()) c.masks.append(m);
    }
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
    o["id"] = t.id;
    o["name"] = t.name;
    o["audio"] = t.audio;
    o["blendMode"] = t.blendMode;
    o["volume"] = t.volume;
    o["pan"] = t.pan;
    o["color"] = t.color.isValid() ? t.color.name() : QString();
    o["eqLow"] = t.eqLow;
    o["eqMid"] = t.eqMid;
    o["eqHigh"] = t.eqHigh;
    o["denoise"] = t.denoise;
    o["denoiseAmount"] = t.denoiseAmount;
    o["invertPhase"] = t.invertPhase;
    o["reverb"] = t.reverb;
    o["reverbMix"] = t.reverbMix;
    o["reverbSize"] = t.reverbSize;
    o["opacity"] = t.opacity;
    o["muted"] = t.muted;
    o["solo"] = t.solo;
    o["locked"] = t.locked;
    o["height"] = t.height;
    o["groupId"] = t.groupId;
    QJsonArray clips;
    for (const Clip& c : t.clips) clips.append(clipToJson(c));
    o["clips"] = clips;
    // Props de canvas (Mesa)
    o["mesaX"] = t.mesaX;
    o["mesaY"] = t.mesaY;
    o["mesaScaleX"] = t.mesaScaleX;
    o["mesaScaleY"] = t.mesaScaleY;
    o["mesaRotation"] = t.mesaRotation;
    o["mesaOpacity"] = t.mesaOpacity;
    o["mesaAnchorX"] = t.mesaAnchorX;
    o["mesaAnchorY"] = t.mesaAnchorY;
    o["mesaHidden"] = t.mesaHidden;
    o["mesaLocked"] = t.mesaLocked;
    o["mesaMotionBlur"] = t.mesaMotionBlur;
    o["kfMesaX"] = kfToJson(t.kfMesaX);
    o["kfMesaY"] = kfToJson(t.kfMesaY);
    o["kfMesaScaleX"] = kfToJson(t.kfMesaScaleX);
    o["kfMesaScaleY"] = kfToJson(t.kfMesaScaleY);
    o["kfMesaRotation"] = kfToJson(t.kfMesaRotation);
    o["kfMesaOpacity"] = kfToJson(t.kfMesaOpacity);
    o["kfMesaAnchorX"] = kfToJson(t.kfMesaAnchorX);
    o["kfMesaAnchorY"] = kfToJson(t.kfMesaAnchorY);
    o["kfVolume"] = kfToJson(t.kfVolume);
    o["kfPan"] = kfToJson(t.kfPan);
    return o;
}

static Track trackFromJson(const QJsonObject& o, bool audio) {
    Track t;
    t.id = o["id"].toString();
    if (t.id.isEmpty()) t.id = newId();
    t.name = o["name"].toString(audio ? QStringLiteral("Audio")
                                      : QStringLiteral("Video"));
    t.audio = audio;
    t.blendMode = o["blendMode"].toString(QStringLiteral("normal"));
    t.volume = o["volume"].toDouble(1.0);
    t.pan = o["pan"].toDouble(0.0);
    const QString col = o["color"].toString();
    if (!col.isEmpty() && QColor::isValidColorName(col)) t.color = QColor(col);
    t.eqLow = o["eqLow"].toDouble(0.0);
    t.eqMid = o["eqMid"].toDouble(0.0);
    t.eqHigh = o["eqHigh"].toDouble(0.0);
    t.denoise = o["denoise"].toBool();
    t.denoiseAmount = o["denoiseAmount"].toDouble(12.0);
    t.invertPhase = o["invertPhase"].toBool();
    t.reverb = o["reverb"].toBool();
    t.reverbMix = o["reverbMix"].toDouble(0.35);
    t.reverbSize = o["reverbSize"].toDouble(0.5);
    t.opacity = o["opacity"].toDouble(1.0);
    t.muted = o["muted"].toBool();
    t.solo = o["solo"].toBool();
    t.locked = o["locked"].toBool();
    t.height = o["height"].toInt(0);
    t.groupId = o["groupId"].toString();
    const QJsonArray clips = o["clips"].toArray();
    for (const QJsonValue& v : clips)
        t.clips.append(clipFromJson(v.toObject()));
    // Props de canvas (Mesa)
    t.mesaX = o["mesaX"].toDouble(0.0);
    t.mesaY = o["mesaY"].toDouble(0.0);
    t.mesaScaleX = o["mesaScaleX"].toDouble(1.0);
    t.mesaScaleY = o["mesaScaleY"].toDouble(1.0);
    t.mesaRotation = o["mesaRotation"].toDouble(0.0);
    t.mesaOpacity = o["mesaOpacity"].toDouble(1.0);
    t.mesaAnchorX = o["mesaAnchorX"].toDouble(0.0);
    t.mesaAnchorY = o["mesaAnchorY"].toDouble(0.0);
    t.mesaHidden = o["mesaHidden"].toBool(false);
    t.mesaLocked = o["mesaLocked"].toBool(false);
    t.mesaMotionBlur = o["mesaMotionBlur"].toBool(true);
    t.kfMesaX = kfFromJson(o["kfMesaX"]);
    t.kfMesaY = kfFromJson(o["kfMesaY"]);
    t.kfMesaScaleX = kfFromJson(o["kfMesaScaleX"]);
    t.kfMesaScaleY = kfFromJson(o["kfMesaScaleY"]);
    t.kfMesaRotation = kfFromJson(o["kfMesaRotation"]);
    t.kfMesaOpacity = kfFromJson(o["kfMesaOpacity"]);
    t.kfMesaAnchorX = kfFromJson(o["kfMesaAnchorX"]);
    t.kfMesaAnchorY = kfFromJson(o["kfMesaAnchorY"]);
    t.kfVolume = kfFromJson(o["kfVolume"]);
    t.kfPan = kfFromJson(o["kfPan"]);
    return t;
}

QJsonObject Project::toJson() const {
    QJsonObject o;
    o["name"] = name;
    o["width"] = width;
    o["height"] = height;
    o["fps"] = fps;
    o["audioRate"] = audioRate;
    o["masterVolume"] = masterVolume;

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

    QJsonArray tgs;
    for (const TrackGroup& g : trackGroups) {
        QJsonObject go;
        go["id"] = g.id;
        go["name"] = g.name;
        go["collapsed"] = g.collapsed;
        go["mesaId"] = g.mesaId;
        tgs.append(go);
    }
    o["trackGroups"] = tgs;

    QJsonArray trs;
    for (const TextResource& r : textResources) {
        QJsonObject ro;
        ro["id"] = r.id;
        ro["textStyle"] = textStyleToJson(r.text);
        trs.append(ro);
    }
    o["textResources"] = trs;

    QJsonArray mesaArr;
    for (const MesaComposition& m : mesas) mesaArr.append(mesaToJson(m));
    o["mesas"] = mesaArr;

    // Marker de migração: versão 2 das posições Mesa (px absolutos, origem
    // topo-esquerda). Arquivos sem este campo são da v1 (offset do centro)
    // e recebem +canvasW/2, +canvasH/2 no load — ver fromJson.
    o["mesaPosAbs"] = true;

    return o;
}

void Project::fromJson(const QJsonObject& o) {
    name = o["name"].toString();
    width = o["width"].toInt(1920);
    height = o["height"].toInt(1080);
    fps = o["fps"].toInt(30);
    audioRate = o["audioRate"].toDouble(48000.0);
    masterVolume = o["masterVolume"].toDouble(1.0);

    media.clear();
    videoTracks.clear();
    audioTracks.clear();
    trackGroups.clear();
    textResources.clear();
    mesas.clear();

    const QJsonArray ma = o["media"].toArray();
    for (const QJsonValue& v : ma) media.append(mediaFromJson(v.toObject()));

    const QJsonArray vta = o["videoTracks"].toArray();
    for (const QJsonValue& v : vta) videoTracks.append(trackFromJson(v.toObject(), false));

    const QJsonArray ata = o["audioTracks"].toArray();
    for (const QJsonValue& v : ata) audioTracks.append(trackFromJson(v.toObject(), true));

    markers.clear();
    const QJsonArray mka = o["markers"].toArray();
    for (const QJsonValue& v : mka) markers.append(markerFromJson(v.toObject()));

    const QJsonArray tga = o["trackGroups"].toArray();
    for (const QJsonValue& v : tga) {
        const QJsonObject go = v.toObject();
        TrackGroup g;
        g.id = go["id"].toString();
        g.name = go["name"].toString();
        g.collapsed = go["collapsed"].toBool();
        g.mesaId = go["mesaId"].toString();
        if (!g.id.isEmpty()) trackGroups.append(g);
    }

    const QJsonArray tra = o["textResources"].toArray();
    for (const QJsonValue& v : tra) {
        const QJsonObject ro = v.toObject();
        TextResource r;
        r.id = ro["id"].toString();
        r.text = textStyleFromJson(ro["textStyle"].toObject());
        if (!r.id.isEmpty()) textResources.append(r);
    }

    const QJsonArray mesaArr = o["mesas"].toArray();
    for (const QJsonValue& v : mesaArr) {
        MesaComposition m = mesaFromJson(v.toObject());
        if (!m.id.isEmpty()) mesas.append(m);
    }

    // Migração v1 → v2: antes, mesaX/mesaY e camX/camY eram OFFSETS do centro
    // da comp (default 0 = centro). Agora são px ABSOLUTOS com origem no canto
    // superior esquerdo (default = centro). Desloca valores estáticos E os
    // keyframes, preservando a aparência dos projetos antigos.
    // Cada carregamento é uma nova revisão: invalida qualquer cache de
    // composição que ainda referencie o conteúdo anterior.
    ++revision;

    if (!o.contains("mesaPosAbs") || !o["mesaPosAbs"].toBool()) {
        for (MesaComposition& m : mesas) {
            const double dX = m.canvasW / 2.0;
            const double dY = m.canvasH / 2.0;
            auto shift = [dX, dY](Track& t) {
                t.mesaX += dX;
                t.mesaY += dY;
                for (Keyframe& k : t.kfMesaX) k.value += dX;
                for (Keyframe& k : t.kfMesaY) k.value += dY;
            };
            for (const QString& tid : m.trackIds) {
                for (Track& t : videoTracks) if (t.id == tid) shift(t);
                for (Track& t : audioTracks) if (t.id == tid) shift(t);
            }
            m.camX += dX;
            m.camY += dY;
            for (Keyframe& k : m.kfCamX) k.value += dX;
            for (Keyframe& k : m.kfCamY) k.value += dY;
        }
    }
}
