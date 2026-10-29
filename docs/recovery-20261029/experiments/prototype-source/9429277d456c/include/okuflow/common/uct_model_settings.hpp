#pragma once
#include <QString>
#include <optional>

namespace okuflow::uct {
enum class ModelTier { Tiny, Small, Medium };
struct ModelSettings {
    ModelTier gpuDetector{ModelTier::Medium};
    ModelTier gpuRecognizer{ModelTier::Medium};
    ModelTier cpuDetector{ModelTier::Medium};
    ModelTier cpuRecognizer{ModelTier::Medium};
    bool operator==(const ModelSettings&) const = default;
};
inline constexpr int DefaultTensorRtBatchCapacity(ModelTier recognizer) {
    return recognizer == ModelTier::Tiny ? 1 : 8;
}
inline QString ModelTierToken(ModelTier tier) {
    switch(tier) {
    case ModelTier::Tiny:return QStringLiteral("tiny");
    case ModelTier::Small:return QStringLiteral("small");
    case ModelTier::Medium:return QStringLiteral("medium");
    }
    return {};
}
inline std::optional<ModelTier> ParseModelTier(const QString& token) {
    if(token==QStringLiteral("tiny"))return ModelTier::Tiny;
    if(token==QStringLiteral("small"))return ModelTier::Small;
    if(token==QStringLiteral("medium"))return ModelTier::Medium;
    return {};
}
} // namespace okuflow::uct
