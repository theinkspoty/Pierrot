// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QVariant>

#include "models/Project.h"

// Serialização somente dos atributos "coláveis" de um clipe — base do sistema
// de presets (estilo Vegas: Salvar/Aplicar preset) e útil para futuras
// operações de "paste attributes". Não inclui identidade (id, mediaId, pos,
// dur, grupo, texto): esses pertencem ao clipe, não ao preset.
namespace clipattrs {

inline QJsonArray kfArr(const QVector<Keyframe>& v) {
    QJsonArray a;
    for (const Keyframe& k : v) {
        QJsonObject o;
        o["t"] = k.time;
        o["v"] = k.value;
        o["i"] = k.interp;
        o["ox"] = k.ox; o["oy"] = k.oy;
        o["ix"] = k.ix; o["iy"] = k.iy;
        a.append(o);
    }
    return a;
}

inline QVector<Keyframe> kfVec(const QJsonValue& val) {
    QVector<Keyframe> v;
    const QJsonArray a = val.toArray();
    v.reserve(a.size());
    for (const QJsonValue& jv : a) {
        const QJsonObject o = jv.toObject();
        Keyframe k;
        k.time = o["t"].toDouble();
        k.value = o["v"].toDouble();
        k.interp = o["i"].toInt();
        k.ox = o["ox"].toDouble(); k.oy = o["oy"].toDouble();
        k.ix = o["ix"].toDouble(); k.iy = o["iy"].toDouble();
        v.append(k);
    }
    return v;
}

inline QJsonObject toJson(const Clip& c) {
    QJsonObject o;
    o["volume"] = c.volume;
    o["opacity"] = c.opacity;
    o["fadeIn"] = c.fadeIn;
    o["fadeOut"] = c.fadeOut;
    o["speed"] = c.speed;
    o["transitionType"] = c.transitionType;

    o["brightness"] = c.brightness;
    o["contrast"] = c.contrast;
    o["saturation"] = c.saturation;
    o["blur"] = c.blur;
    o["grayscale"] = c.grayscale;
    o["chromaKey"] = c.chromaKey;
    o["chromaKeyColor"] = c.chromaKeyColor.name(QColor::HexArgb);
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

    o["kfOpacity"] = kfArr(c.kfOpacity);
    o["kfVolume"] = kfArr(c.kfVolume);
    o["kfTx"] = kfArr(c.kfTx);
    o["kfTy"] = kfArr(c.kfTy);
    o["kfScale"] = kfArr(c.kfScale);
    o["kfRotation"] = kfArr(c.kfRotation);
    o["kfScaleX"] = kfArr(c.kfScaleX);
    o["kfScaleY"] = kfArr(c.kfScaleY);
    o["kfAnchorX"] = kfArr(c.kfAnchorX);
    o["kfAnchorY"] = kfArr(c.kfAnchorY);
    o["kfCropL"] = kfArr(c.kfCropL);
    o["kfCropR"] = kfArr(c.kfCropR);
    o["kfCropT"] = kfArr(c.kfCropT);
    o["kfCropB"] = kfArr(c.kfCropB);
    return o;
}

// Aplica somente as chaves presentes em `o` — presets salvos por versões
// diferentes do app não quebram clipes antigos.
inline void applyJson(Clip& c, const QJsonObject& o) {
    auto key = [&o](const char* k) { return o.contains(QLatin1String(k)); };
    auto d = [&o](const char* k, double fallback) { return o[QLatin1String(k)].toDouble(fallback); };
    auto b = [&o](const char* k, bool fallback) { return o[QLatin1String(k)].toBool(fallback); };

    if (key("volume")) c.volume = d("volume", c.volume);
    if (key("opacity")) c.opacity = d("opacity", c.opacity);
    if (key("fadeIn")) c.fadeIn = d("fadeIn", c.fadeIn);
    if (key("fadeOut")) c.fadeOut = d("fadeOut", c.fadeOut);
    if (key("speed")) c.speed = d("speed", c.speed);
    if (key("transitionType")) c.transitionType = o["transitionType"].toString();

    if (key("brightness")) c.brightness = d("brightness", c.brightness);
    if (key("contrast")) c.contrast = d("contrast", c.contrast);
    if (key("saturation")) c.saturation = d("saturation", c.saturation);
    if (key("blur")) c.blur = d("blur", c.blur);
    if (key("grayscale")) c.grayscale = b("grayscale", c.grayscale);
    if (key("chromaKey")) c.chromaKey = b("chromaKey", c.chromaKey);
    if (key("chromaKeyColor")) {
        const QString sc = o["chromaKeyColor"].toString();
        if (QColor::isValidColorName(sc)) c.chromaKeyColor = QColor(sc);
    }
    if (key("chromaKeySimilarity")) c.chromaKeySimilarity = d("chromaKeySimilarity", c.chromaKeySimilarity);
    if (key("liftR")) c.liftR = d("liftR", c.liftR);
    if (key("liftG")) c.liftG = d("liftG", c.liftG);
    if (key("liftB")) c.liftB = d("liftB", c.liftB);
    if (key("gammaR")) c.gammaR = d("gammaR", c.gammaR);
    if (key("gammaG")) c.gammaG = d("gammaG", c.gammaG);
    if (key("gammaB")) c.gammaB = d("gammaB", c.gammaB);
    if (key("gainR")) c.gainR = d("gainR", c.gainR);
    if (key("gainG")) c.gainG = d("gainG", c.gainG);
    if (key("gainB")) c.gainB = d("gainB", c.gainB);

    if (key("lainkaEnabled")) c.lainkaEnabled = b("lainkaEnabled", c.lainkaEnabled);
    if (key("lainkaSkip")) c.lainkaSkip = qMax(1, o["lainkaSkip"].toInt(c.lainkaSkip));
    if (key("lainkaJitterPos")) c.lainkaJitterPos = d("lainkaJitterPos", c.lainkaJitterPos);
    if (key("lainkaJitterRot")) c.lainkaJitterRot = d("lainkaJitterRot", c.lainkaJitterRot);
    if (key("lainkaJitterScale")) c.lainkaJitterScale = d("lainkaJitterScale", c.lainkaJitterScale);
    if (key("lainkaFlicker")) c.lainkaFlicker = d("lainkaFlicker", c.lainkaFlicker);
    if (key("lainkaFlickerSpeed")) c.lainkaFlickerSpeed = d("lainkaFlickerSpeed", c.lainkaFlickerSpeed);
    if (key("lainkaWarpAmount")) c.lainkaWarpAmount = d("lainkaWarpAmount", c.lainkaWarpAmount);
    if (key("lainkaWarpSpeed")) c.lainkaWarpSpeed = d("lainkaWarpSpeed", c.lainkaWarpSpeed);
    if (key("lainkaWarpGrid")) c.lainkaWarpGrid = qBound(4, o["lainkaWarpGrid"].toInt(c.lainkaWarpGrid), 64);
    if (key("lainkaOnionSkin")) c.lainkaOnionSkin = d("lainkaOnionSkin", c.lainkaOnionSkin);
    if (key("lainkaDustAmount")) c.lainkaDustAmount = d("lainkaDustAmount", c.lainkaDustAmount);
    if (key("lainkaScratchAmount")) c.lainkaScratchAmount = d("lainkaScratchAmount", c.lainkaScratchAmount);
    if (key("lainkaTargetFps")) c.lainkaTargetFps = qMax(1, o["lainkaTargetFps"].toInt(c.lainkaTargetFps));
    if (key("lainkaMotionBlur")) c.lainkaMotionBlur = d("lainkaMotionBlur", c.lainkaMotionBlur);
    if (key("lainkaOpacity")) c.lainkaOpacity = d("lainkaOpacity", c.lainkaOpacity);
    if (key("lainkaAntialias")) c.lainkaAntialias = o["lainkaAntialias"].toInt(c.lainkaAntialias);

    if (key("motionEnabled")) c.motionEnabled = b("motionEnabled", c.motionEnabled);
    if (key("motionAmount")) c.motionAmount = d("motionAmount", c.motionAmount);
    if (key("motionAngle")) c.motionAngle = d("motionAngle", c.motionAngle);
    if (key("motionSamples")) c.motionSamples = qBound(1, o["motionSamples"].toInt(c.motionSamples), 32);

    if (key("tx")) c.tx = d("tx", c.tx);
    if (key("ty")) c.ty = d("ty", c.ty);
    if (key("scale")) c.scale = d("scale", c.scale);
    if (key("scaleX")) c.scaleX = d("scaleX", c.scaleX);
    if (key("scaleY")) c.scaleY = d("scaleY", c.scaleY);
    if (key("rotation")) c.rotation = d("rotation", c.rotation);
    if (key("anchorX")) c.anchorX = d("anchorX", c.anchorX);
    if (key("anchorY")) c.anchorY = d("anchorY", c.anchorY);

    if (key("cropL")) c.cropL = d("cropL", c.cropL);
    if (key("cropR")) c.cropR = d("cropR", c.cropR);
    if (key("cropT")) c.cropT = d("cropT", c.cropT);
    if (key("cropB")) c.cropB = d("cropB", c.cropB);

    if (key("eqLow")) c.eqLow = d("eqLow", c.eqLow);
    if (key("eqMid")) c.eqMid = d("eqMid", c.eqMid);
    if (key("eqHigh")) c.eqHigh = d("eqHigh", c.eqHigh);
    if (key("denoise")) c.denoise = b("denoise", c.denoise);
    if (key("denoiseAmount")) c.denoiseAmount = d("denoiseAmount", c.denoiseAmount);
    if (key("normalize")) c.normalize = b("normalize", c.normalize);
    if (key("invertPhase")) c.invertPhase = b("invertPhase", c.invertPhase);

    if (key("ofxFx")) {
        c.ofxFx.clear();
        const QJsonArray ofxArr = o["ofxFx"].toArray();
        for (const QJsonValue& jv : ofxArr) {
            const QJsonObject fxo = jv.toObject();
            OfxPluginInstance fx;
            fx.pluginId = fxo["pluginId"].toString();
            fx.enabled = fxo["enabled"].toBool();
            for (const QJsonValue& pv : fxo["params"].toArray()) {
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
    }

    if (key("kfOpacity")) c.kfOpacity = kfVec(o["kfOpacity"]);
    if (key("kfVolume")) c.kfVolume = kfVec(o["kfVolume"]);
    if (key("kfTx")) c.kfTx = kfVec(o["kfTx"]);
    if (key("kfTy")) c.kfTy = kfVec(o["kfTy"]);
    if (key("kfScale")) c.kfScale = kfVec(o["kfScale"]);
    if (key("kfRotation")) c.kfRotation = kfVec(o["kfRotation"]);
    if (key("kfScaleX")) c.kfScaleX = kfVec(o["kfScaleX"]);
    if (key("kfScaleY")) c.kfScaleY = kfVec(o["kfScaleY"]);
    if (key("kfAnchorX")) c.kfAnchorX = kfVec(o["kfAnchorX"]);
    if (key("kfAnchorY")) c.kfAnchorY = kfVec(o["kfAnchorY"]);
    if (key("kfCropL")) c.kfCropL = kfVec(o["kfCropL"]);
    if (key("kfCropR")) c.kfCropR = kfVec(o["kfCropR"]);
    if (key("kfCropT")) c.kfCropT = kfVec(o["kfCropT"]);
    if (key("kfCropB")) c.kfCropB = kfVec(o["kfCropB"]);
}

} // namespace clipattrs