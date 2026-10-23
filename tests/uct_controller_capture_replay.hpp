#pragma once

// CPU-only replay of explicitly selected private captured recognition output.
// No model, GPU, decoder, native desktop, provider or app settings are used.
#include "okuflow/common/ultra_clear_text.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace uct_controller_replay {
using namespace okuflow::uct;
struct Pair {
    QImage selectedImage, neighbourImage;
    RecognitionOutput selected, neighbour, bridgeSelected, bridgeNeighbour;
    qint64 selectedMs{}, neighbourMs{};
    QRectF focus{.1,.12,.7,.7};
    bool bridge{};
    QByteArray proofSha256;
};

inline bool Rect(const QJsonValue& value,QRectF& output) {
    const auto a=value.toArray();if(a.size()!=4)return false;
    for(const auto& n:a)if(!n.isDouble()||!std::isfinite(n.toDouble()))return false;
    output={a[0].toDouble(),a[1].toDouble(),a[2].toDouble(),a[3].toDouble()};return true;
}
inline bool Quad(const QJsonValue& value,TextQuad& output) {
    if(value.isUndefined())return true;
    const auto a=value.toArray();if(a.size()!=4)return false;
    for(int i=0;i<4;++i){const auto point=a[i].toArray();if(point.size()!=2||!point[0].isDouble()||!point[1].isDouble()||
        !std::isfinite(point[0].toDouble())||!std::isfinite(point[1].toDouble()))return false;
        output.points[i]={point[0].toDouble(),point[1].toDouble()};}
    output.valid=true;return true;
}
inline bool Output(const QJsonObject& object,RecognitionOutput& output) {
    if(!object.value("ok").toBool()||object.value("cancelled").toBool()||object.value("inputFault").toBool())return false;
    output.ok=true;output.backendPreparing=object.value("backendPreparing").toBool();
    QRectF region;if(!Rect(object.value("detailRegion"),region))return false;output.detailRegionPx=region.toAlignedRect();
    const auto lines=object.value("lines").toArray();if(lines.size()>1024)return false;
    int count=0;
    for(const auto& row:lines){RecognizedLine line;
        for(const auto& item:row.toArray()){
            if(++count>8192)return false;const auto word=item.toObject();RecognizedWord w;
            w.text=word.value("text").toString();if(w.text.size()>512)return false;
            const auto disposition=word.value("disposition").toInt(-1);
            if(disposition<0||disposition>int(TokenDisposition::kUnrecognized))return false;
            w.disposition=TokenDisposition(disposition);w.confidence=word.value("confidence").toDouble(-1);
            if(!std::isfinite(w.confidence)||w.confidence<0||w.confidence>1)return false;
            if(!Rect(word.value("bounds"),w.boundsPx)||!Rect(word.value("protectedBounds"),w.protectedBoundsPx)||
               !Rect(word.value("inkSearch"),w.inkSearchPx)||!Quad(word.value("quad"),w.quadPx)||
               !Quad(word.value("inkSearchQuad"),w.inkSearchQuadPx))return false;
            w.paddedBounds=word.value("paddedBounds").toBool();w.inkFitted=word.value("inkFitted").toBool();
            if(word.contains("foreground")||word.contains("background")){
                LineColors colors{QColor(word.value("foreground").toString()),QColor(word.value("background").toString())};
                if(!colors.foreground.isValid()||!colors.background.isValid())return false;w.colors=colors;
            }
            line.words.push_back(std::move(w));
        }
        output.lines.push_back(std::move(line));
    }
    return !output.lines.isEmpty();
}
inline bool Load(const QString& path,Pair& pair,QString& error) {
    const auto fail=[&](const QString& text){error=text;return false;};
    if(!QDir::isAbsolutePath(path))return fail("Capture proof must be an explicit absolute private path.");
    QFile proof(path);if(!proof.open(QIODevice::ReadOnly)||proof.size()>1024*1024)return fail("Capture proof unavailable or over one MiB.");
    const auto bytes=proof.readAll();QJsonParseError parse;
    const auto document=QJsonDocument::fromJson(bytes,&parse);
    if(parse.error!=QJsonParseError::NoError||!document.isObject())return fail("Malformed capture proof JSON.");
    const auto object=document.object();pair.proofSha256=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
    const auto image=[&](const char* key,const char* hash,QImage& out){
        const auto imagePath=object.value(key).toString();QFile input(imagePath);
        if(!QDir::isAbsolutePath(imagePath)||!input.open(QIODevice::ReadOnly)||input.size()>64ll*1024*1024)return false;
        const auto data=input.readAll();if(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex()!=object.value(hash).toString().toLatin1())return false;
        out=QImage::fromData(data).convertToFormat(QImage::Format_ARGB32);
        return !out.isNull()&&out.width()<=8192&&out.height()<=8192&&out.sizeInBytes()<=64ll*1024*1024;
    };
    if(!image("selectedPath","selectedSha256",pair.selectedImage)||!image("neighbourPath","neighbourSha256",pair.neighbourImage)||
       pair.selectedImage.size()!=pair.neighbourImage.size())return fail("Captured source image identity/extent mismatch.");
    pair.selectedMs=object.value("selectedMs").toVariant().toLongLong();
    pair.neighbourMs=object.value("neighbourMs").toVariant().toLongLong();
    if(pair.selectedMs<0||pair.neighbourMs<0||pair.selectedMs>INT64_MAX/10'000||pair.neighbourMs>INT64_MAX/10'000||
       pair.selectedMs==pair.neighbourMs||std::abs(pair.neighbourMs-pair.selectedMs)>250)return fail("Button replay requires the actual distinct <=250-ms source pair.");
    if(!Output(object.value("selectedOutput").toObject(),pair.selected)||
       !Output(object.value("neighbourOutput").toObject(),pair.neighbour))return fail("Incomplete captured preferred output.");
    const auto bridge=object.value("bridge").toObject();pair.bridge=!bridge.isEmpty();
    if(pair.bridge&&(!Output(bridge.value("selectedOutput").toObject(),pair.bridgeSelected)||
       !Output(bridge.value("neighbourOutput").toObject(),pair.bridgeNeighbour)))return fail("Incomplete captured bridge output.");
    const auto focus=object.value("detail").toObject().value("focusScene");
    if(!focus.isUndefined()&&!Rect(focus,pair.focus))return fail("Invalid captured focus.");
    return true;
}
} // namespace uct_controller_replay
