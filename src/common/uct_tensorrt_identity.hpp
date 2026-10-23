#pragma once

// Pure cache/version policy. No CUDA, DLL loading, filesystem or engine API.
#include "uct_recognizer_width.hpp"
#include <QByteArray>
#include <QCryptographicHash>
#include <QList>
#include <array>
#include <stdexcept>

namespace okuflow::uct::tensorrt_identity {
using Version = std::array<int, 4>;
inline constexpr Version kMinimumRuntime{11,4,0,106};
inline constexpr char kBuilderVersion[] = "11.4.0.106";
inline bool ParseVersion(const QByteArray& bytes, Version& result) {
    const auto parts = bytes.split('.');
    if (parts.size() != 4) return false;
    Version parsed{};
    for (int i=0;i<4;++i) {
        const auto& part=parts[i];
        if (part.isEmpty() || part.size()>6 || (part.size()>1 && part[0]=='0')) return false;
        for (const auto c : part) if (c<'0' || c>'9') return false;
        bool ok=false;
        parsed[i]=part.toInt(&ok);
        if (!ok) return false;
    }
    if (parsed[0]<1) return false;
    result=parsed; // malformed input never changes the caller's value
    return true;
}
inline QByteArray VersionString(const Version& version) {
    return QByteArray::number(version[0])+'.'+QByteArray::number(version[1])+'.'+
           QByteArray::number(version[2])+'.'+QByteArray::number(version[3]);
}
inline bool RuntimeAtLeastMinimum(const Version& version) {
    for (const int part : version) if (part<0 || part>999999) return false;
    // The application is compiled for ABI11. A numeric floor cannot discover
    // another major's DLL/factory/parser API.
    return version[0]==11 && version >= kMinimumRuntime;
}
inline bool RuntimeAtLeastMinimum(const QByteArray& bytes) {
    Version version{};
    return ParseVersion(bytes,version) && RuntimeAtLeastMinimum(version);
}
inline QByteArray Fp32ProfileTag(bool detection, int maximumBatch, bool variableWidth) {
    if (variableWidth && !detection && (maximumBatch==1 || maximumBatch==8 || maximumBatch==16))
        return recognizer_width::kCacheTag+QByteArray::number(maximumBatch);
    if (!variableWidth && !detection && (maximumBatch==8 || maximumBatch==16))
        return "-rec-min1x3x48x1024-opt"+QByteArray::number(maximumBatch)+
               "x3x48x1024-max"+QByteArray::number(maximumBatch)+"x3x48x1024-v2";
    if (!variableWidth && maximumBatch==1) return {};
    throw std::invalid_argument("unsupported TensorRT FP32 profile identity");
}
inline void CheckKeyInputs(const QByteArray& rawGraphSha256, const QByteArray& rawUuid) {
    if (rawGraphSha256.size()!=32 || rawUuid.size()!=16)
        throw std::invalid_argument("invalid TensorRT graph/UUID identity");
}
inline QByteArray Fp32PlanKey(const QByteArray& rawGraphSha256, const QByteArray& rawUuid,
                             bool detection, int maximumBatch, bool variableWidth,
                             const QByteArray& builderVersion = kBuilderVersion) {
    CheckKeyInputs(rawGraphSha256,rawUuid);
    if (!RuntimeAtLeastMinimum(builderVersion)) throw std::invalid_argument("invalid TensorRT builder version");
    const auto bytes=QByteArray("uct-fp32-v2\n")+rawGraphSha256.toHex()+'\n'+rawUuid.toHex()+
        "\ntrt"+builderVersion+"-fp32-tf32-default-opt2-workspace256MiB-aux0\n"+
        (detection ? "det-32-544x960-960x960" : "rec-48x1024")+'\n'+
        QByteArray::number(maximumBatch)+Fp32ProfileTag(detection,maximumBatch,variableWidth);
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
}
inline QByteArray LegacyFp32PlanKey(const QByteArray& rawGraphSha256, const QByteArray& rawUuid,
                                   bool detection, int maximumBatch, bool variableWidth, int historicalDriver) {
    // Lookup only: exact old bytes. Never use this key as a new write destination.
    CheckKeyInputs(rawGraphSha256,rawUuid);
    if (historicalDriver<1) throw std::invalid_argument("invalid legacy driver provenance");
    const auto bytes=rawGraphSha256+rawUuid+QByteArray::number(historicalDriver)+
        "-trt11.4.0.106-fp32-v1"+Fp32ProfileTag(detection,maximumBatch,variableWidth);
    return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
}
} // namespace okuflow::uct::tensorrt_identity
