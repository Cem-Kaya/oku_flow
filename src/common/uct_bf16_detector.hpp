#pragma once
#include "uct_bf16_runtime.hpp"
#include <cmath>

namespace okuflow::uct::bf16_detector {
inline const char* GraphHash(ModelTier tier) {
    switch(tier) {
    case ModelTier::Medium:return "5699b5d662f046d7fcad339b351263f154a13c67a11ee797f45074f8876db10c";
    case ModelTier::Small:return "05f643581c6662b1e04f201be302aeb4d3fd5f0b03bf65d483e420d977ab06e6";
    case ModelTier::Tiny:return "cfb0aadcc7efb452cb830b30540d5d2468a79b2b59ffba0ec0e986dceaad5833";
    }
    throw runtime_precision::ArtifactError("unknown BF16 detector tier");
}
inline QString RelativePath(ModelTier tier) {return "uct/det/"+ModelTierToken(tier)+"/bf16-linear/inference.onnx";}
inline QString ReadGraphPath(const QString& root,const PaddleAssets& assets,ModelTier tier) {
    using namespace runtime_precision;
    const auto* pin=FindPaddleModelPin(true,tier);
    if(!pin || !assets.IsValid() || assets.detectorSha256!=pin->modelSha256.data())
        throw ArtifactError("BF16 detector original model differs from selected tier");
    ReadVerified(assets.detector,assets.detectorSha256,kGraphLimit);
    ReadVerified(QDir(assets.root).filePath(QString::fromUtf8(pin->directory.data(),qsizetype(pin->directory.size()))+"/inference.yml"),
        QByteArray(pin->configSha256.data(),qsizetype(pin->configSha256.size())),8*1024*1024+1);
    if(!LocalAbsolute(root))throw ArtifactError("BF16 detector model root must be local and absolute");
    const auto path=QDir(root).filePath(RelativePath(tier));
    CanonicalWithin(root,path);ReadVerified(path,GraphHash(tier),kGraphLimit);
    return path;
}
inline QByteArray PlanKey(const QByteArray& graph,const QByteArray& uuid,
                          const QByteArray& builder=tensorrt_identity::kBuilderVersion) {
    return bf16_runtime::PlanKey(graph,uuid,true,1,false,builder);
}
struct MapSummary {float maximum{};double mean{};};
inline MapSummary CheckMap(std::span<const float> map) {
    if(map.empty() || map.size()>960ull*960)
        throw runtime_precision::ArtifactError("BF16 detector smoke output map exceeds its shape bounds");
    MapSummary summary;
    for(const auto value:map) {
        if(!std::isfinite(value) || value<0 || value>1.0001f)
            throw runtime_precision::ArtifactError("BF16 detector smoke output contains invalid probabilities");
        summary.maximum=std::max(summary.maximum,value);summary.mean+=value;
    }
    summary.mean/=map.size();return summary;
}
inline void CheckMapSize(int width,int height,int mapWidth,int mapHeight) {
    if(width!=mapWidth || height!=mapHeight)
        throw runtime_precision::ArtifactError("BF16 detector probability map differs from input dimensions");
}
} // namespace okuflow::uct::bf16_detector
