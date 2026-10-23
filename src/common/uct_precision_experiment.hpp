#pragma once

// Private diagnostic-only selection. Production factories never read this
// manifest or call this factory; it supplies no public/model approval.
#include "uct_runtime_precision.hpp"
#include "okuflow/common/uct_model_pins.hpp"
#include <QJsonDocument>

namespace okuflow::uct {
struct PrecisionExperimentPlan {
    ModelTier tier{ModelTier::Small};
    QString graphPath, planPath;
    QByteArray sourceSha256, graphSha256, planSha256, builderVersion, uuid, bytes;
};
inline QString PrecisionExperimentRefusal(const PrecisionExperimentPlan& plan,
    const PaddleAssets& assets, ModelTier tier, int capacity, bool variableWidth,
    bool widthPreparation, const QByteArray& uuid, int major, const QByteArray& runtime)
{
    if((tier!=ModelTier::Small && tier!=ModelTier::Tiny) || plan.tier!=tier ||
       capacity!=8 || variableWidth || widthPreparation)
        return "experimental BF16 requires the selected small/tiny recognizer, N8 fixed1024";
    if(major<8 || uuid.size()!=16 || plan.uuid!=uuid)
        return "experimental BF16 device identity/Ampere requirement differs";
    if(!tensorrt_identity::RuntimeAtLeastMinimum(runtime) || plan.builderVersion!=runtime)
        return "experimental plan requires its actual current TensorRT builder runtime";
    if(plan.sourceSha256!=assets.recognizerSha256 || !runtime_precision::Hash(plan.graphSha256) ||
       !runtime_precision::Hash(plan.planSha256) || plan.bytes.isEmpty() ||
       QCryptographicHash::hash(plan.bytes,QCryptographicHash::Sha256).toHex()!=plan.planSha256)
        return "experimental graph/source/payload identity differs";
    return {};
}
inline PrecisionExperimentPlan LoadPrecisionExperiment(const QString& manifest,
                                                       const PaddleAssets& assets,ModelTier tier)
{
    using namespace runtime_precision;
    if(!LocalAbsolute(manifest))throw ArtifactError("experiment manifest must be an explicit local absolute file");
    QFile file(manifest);
    if(!file.open(QIODevice::ReadOnly) || file.size()<1 || file.size()>64*1024)
        throw ArtifactError("experimental manifest missing/unreadable/out of bounds");
    QJsonParseError error;
    const auto doc=QJsonDocument::fromJson(file.read(64*1024+1),&error);
    if(error.error!=QJsonParseError::NoError || !doc.isObject())throw ArtifactError("invalid experimental manifest JSON");
    const auto j=doc.object();
    if(j["tier"]!=ModelTierToken(tier) || (tier!=ModelTier::Small && tier!=ModelTier::Tiny))
        throw ArtifactError("experimental manifest selected model tier differs");
    const auto* pin=FindPaddleModelPin(false,tier);
    if(!pin || j["sourceSha256"].toString().toLatin1()!=assets.recognizerSha256 ||
       j["configSha256"].toString().toLatin1()!=QByteArray(pin->configSha256.data(),qsizetype(pin->configSha256.size())))
        throw ArtifactError("experimental original source/config pin differs");
    PrecisionExperimentPlan result{tier,j["graph"].toString(),j["plan"].toString(),
        j["sourceSha256"].toString().toLatin1(),j["graphSha256"].toString().toLatin1(),
        j["planSha256"].toString().toLatin1(),j["builderTensorRtVersion"].toString().toLatin1(),
        QByteArray::fromHex(j["deviceUuid"].toString().toLatin1()),{}};
    if(!LocalAbsolute(result.graphPath) || !LocalAbsolute(result.planPath) ||
       !Hash(result.graphSha256) || !Hash(result.planSha256) || result.uuid.size()!=16 ||
       !tensorrt_identity::RuntimeAtLeastMinimum(result.builderVersion))
        throw ArtifactError("experimental graph/plan/device/builder fields invalid");
    ReadVerified(result.graphPath,result.graphSha256,kGraphLimit);
    result.bytes=ReadVerified(result.planPath,result.planSha256,kPlanLimit);
    return result; // owned immutable payload snapshot, not reopened by the worker
}
std::unique_ptr<Recognizer> CreatePrecisionExperimentRecognizer(const QString& root,const QString& sdk,
    ModelTier tier,std::shared_ptr<const PrecisionExperimentPlan> plan,
    TensorRtRetirementObserver observer={});
} // namespace okuflow::uct
