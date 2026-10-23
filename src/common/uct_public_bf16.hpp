#pragma once

// Pure public-model attestation contract. Runtime integration is private in
// uct_tensorrt.cpp; the schema-1 owner/dev selection stays separate.
#include "uct_precision_artifacts.hpp"
#include <QSet>

namespace okuflow::uct::public_bf16 {
inline constexpr char kModelVersion[]="ppocrv6-medium-rec-bf16-linear-v1";
inline constexpr char kDictionarySha256[]="f1d7cde73a39cc887dd68d5c64b2756b4962ae28594d3a610c683b2fc47110f1";
inline constexpr char kGoldenPolicy[]="uct-bf16-exact-text-n1-n8-v1";
inline constexpr char kCropEncoding[]="gray8-upright-h48-zero-pad1024-v1";
inline constexpr char kPreprocessing[]="paddle-rectify-normalized-zero-pad-v1";
inline constexpr char kGraphRelativePath[]="uct/rec/medium/bf16-linear/inference.onnx";
inline constexpr int kGoldenSamples=8, kGoldenObservations=16;
enum class Reason { Configuration, Hardware, Runtime, Model, CorpusUnavailable, Corpus, BuildRefused, GoldenMismatch };
class Refusal : public runtime_precision::ArtifactError {
public:
    Refusal(Reason reason,const char* text):ArtifactError(text),reason_(reason){}
    Reason Code() const {return reason_;}
private:
    Reason reason_;
};
struct ModelSpec {
    ModelTier tier;
    const char* graphSha256;
    const char* dictionarySha256;
    int classes;
};
inline ModelSpec Model(ModelTier tier) {
    switch(tier) {
    case ModelTier::Medium:return {tier,runtime_precision::kRecognizerGraph,kDictionarySha256,18710};
    case ModelTier::Small:return {tier,"483b257a64da0b493e64dc58cc3827070eb11a9c7ee8ae0b1ffad7a61aeb03f1",kDictionarySha256,18710};
    case ModelTier::Tiny:return {tier,"7fce45ba34121940382e59fd6a81c78ffab85d80b3fd51d01a9b66c5372f31ad",
        "b1f7def77c3b4dcc341d9a3c6753e2d49c282f7c348896c19ce7eb6e7c7b1dce",6906};
    }
    throw Refusal(Reason::Configuration,"unknown BF16 recognizer tier");
}
inline QString ModelVersion(ModelTier tier) {return "ppocrv6-"+ModelTierToken(tier)+"-rec-bf16-linear-v1";}
inline QString GraphRelativePath(ModelTier tier) {return "uct/rec/"+ModelTierToken(tier)+"/bf16-linear/inference.onnx";}
inline QString CorpusVersion(ModelTier tier) {
    return tier==ModelTier::Medium ? "okuflow-owned-typeset-gray8-v1" : "okuflow-owned-typeset-gray8-"+ModelTierToken(tier)+"-v1";
}
inline QJsonObject ModelIdentity(ModelTier tier=ModelTier::Medium) {
    const auto spec=Model(tier);
    const auto* pin=FindPaddleModelPin(false,tier);
    return {{"version",ModelVersion(tier)},{"precision","bf16-linear"},
        {"sourceSha256",QString::fromUtf8(pin->modelSha256.data(),qsizetype(pin->modelSha256.size()))},{"graphSha256",spec.graphSha256},
        {"configSha256",QString::fromUtf8(pin->configSha256.data(),qsizetype(pin->configSha256.size()))},
        {"dictionarySha256",spec.dictionarySha256},{"classes",spec.classes},
        {"inputName","x"},{"outputName","fetch_name_0"},{"inputType","FLOAT"},{"outputType","FLOAT"},
        {"inputLocation","DEVICE"},{"outputLocation","DEVICE"},{"format","LINEAR"},
        {"outputAbi","ctc-probabilities-v1"},{"widthPolicy","fixed1024"},{"capacity",8},
        {"preprocessing",kPreprocessing},
        {"minimum",QJsonArray{1,3,48,1024}},{"optimum",QJsonArray{8,3,48,1024}},{"maximum",QJsonArray{8,3,48,1024}},
        {"tf32","default"},{"optimizationLevel",2},{"workspaceMiB",256},{"auxStreams",0}};
}
struct CorpusPin {QString version; QByteArray manifestSha256;};
inline std::span<const CorpusPin> ProductionCorpusCatalog() {
    // Owned synthetic source crops are candidate validation inputs only. Fail
    // closed until strict real runtime attestation and separate root activation.
    // Never read this
    // authority from an environment variable, selector or writable receipt.
    return {};
}
inline void CheckEligibility(ModelTier detector,ModelTier recognizer,int capacity,bool variableWidth,
                             bool prepareWidth,const precision_artifacts::Runtime& runtime,
                             std::span<const CorpusPin> catalog) {
    if(!FindPaddleModelPin(true,detector) || !FindPaddleModelPin(false,recognizer) ||
       (capacity!=1 && capacity!=8) || variableWidth || prepareWidth)
        throw Refusal(Reason::Configuration,"public BF16 preserves selected tiers; requires N1/N8 fixed1024");
    if(runtime.major<8 || runtime.uuid.size()!=16)
        throw Refusal(Reason::Hardware,"public BF16 requires Ampere or newer and actual device identity");
    if(!tensorrt_identity::RuntimeAtLeastMinimum(runtime.tensorRtVersion))
        throw Refusal(Reason::Runtime,"public BF16 requires supported TensorRT ABI11 runtime at least 11.4.0.106");
    if(catalog.empty())throw Refusal(Reason::CorpusUnavailable,"public BF16 golden corpus is not approved/available");
}
inline QByteArray ReadModel(const QString& modelAssetRoot,const PaddleAssets& assets,ModelTier tier=ModelTier::Medium) {
    using namespace runtime_precision;
    const auto spec=Model(tier);
    const auto* pin=FindPaddleModelPin(false,tier);
    const QByteArray source(pin->modelSha256.data(),qsizetype(pin->modelSha256.size()));
    if(!assets.IsValid() || assets.recognizerSha256!=source || assets.dictionary.size()!=spec.classes ||
       precision_artifacts::DictionaryHash(assets.dictionary)!=spec.dictionarySha256)
        throw Refusal(Reason::Model,"public BF16 source/dictionary does not match selected model assets");
    // The existing decoder's dictionary comes from this exact bounded config.
    ReadVerified(assets.recognizer,source,kGraphLimit);
    ReadVerified(QDir(assets.root).filePath(QString::fromUtf8(pin->directory.data(),qsizetype(pin->directory.size()))+"/inference.yml"),
        QByteArray(pin->configSha256.data(),qsizetype(pin->configSha256.size())),8*1024*1024+1);
    const auto path=QDir(modelAssetRoot).filePath(GraphRelativePath(tier));
    if(!LocalAbsolute(modelAssetRoot))throw Refusal(Reason::Model,"public BF16 assets require an explicit local model root");
    CanonicalWithin(modelAssetRoot,path);
    return ReadVerified(path,spec.graphSha256,kGraphLimit);
}
struct Crop {
    QString id,expected;
    QByteArray pixelsSha256,pixels; // exact GRAY8, row-major; no runtime font/codec
    int width{}; // height48, useful width; production pads to fixed1024
};
class Corpus {
public:
    const QString& Version() const{return version_;}
    const QByteArray& Sha256() const{return sha256_;}
    const std::vector<Crop>& Crops() const{return crops_;}
    const QJsonObject& ModelIdentity() const{return model_;}
private:
    friend Corpus ReadCorpus(const QByteArray&,std::span<const CorpusPin>,const std::function<QByteArray(const QString&)>&);
    QString version_;
    QByteArray sha256_;
    QJsonObject model_;
    std::vector<Crop> crops_;
};
inline Corpus ReadCorpus(const QByteArray& manifest,std::span<const CorpusPin> catalog,
                         const std::function<QByteArray(const QString&)>& readOwnedCrop) {
    if(manifest.isEmpty() || manifest.size()>64*1024)throw Refusal(Reason::Corpus,"golden corpus manifest exceeds bounds");
    const auto hash=precision_artifacts::Digest(manifest);
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(manifest,&error);
    if(error.error!=QJsonParseError::NoError || !document.isObject())throw Refusal(Reason::Corpus,"invalid golden corpus JSON");
    const auto json=document.object();
    const auto version=json["version"].toString();
    int approved=0;
    for(const auto& pin:catalog)if(pin.version==version && pin.manifestSha256==hash)++approved;
    if(approved!=1)throw Refusal(Reason::CorpusUnavailable,"golden corpus lacks unique compiled approval");
    bool knownModel=false;
    for(const auto tier:{ModelTier::Tiny,ModelTier::Small,ModelTier::Medium})knownModel=knownModel || json["model"]==ModelIdentity(tier);
    if(json.size()!=7 || json["schema"]!=1 || version.isEmpty() || version.size()>96 ||
       !knownModel || json["policy"]!=kGoldenPolicy || json["encoding"]!=kCropEncoding ||
       !json["provenance"].isString() || json["provenance"].toString().isEmpty() ||
       json["provenance"].toString().size()>2048 || !json["samples"].isArray() || json["samples"].toArray().size()!=kGoldenSamples)
        throw Refusal(Reason::Corpus,"golden corpus model/policy/format/provenance mismatch");
    Corpus result;result.version_=version;result.sha256_=hash;result.model_=json["model"].toObject();
    QSet<QString> ids,coverage;
    for(const auto& value:json["samples"].toArray()) {
        const auto sample=value.toObject();
        const auto id=sample["id"].toString(), expected=sample["expected"].toString();
        if(!value.isObject() || sample.size()!=5 || id.isEmpty() || id.size()>48 || expected.isEmpty() || expected.size()>256 ||
           expected.contains(QChar::Null) || ids.contains(id) || !sample["tags"].isArray() || sample["tags"].toArray().size()>8)
            throw Refusal(Reason::Corpus,"golden sample fields/identity/text invalid");
        for(const auto c:id)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))
            throw Refusal(Reason::Corpus,"golden sample ID is not an owned relative basename");
        ids.insert(id);
        for(const auto& tag:sample["tags"].toArray()) {
            if(!tag.isString() || tag.toString().isEmpty() || tag.toString().size()>48)
                throw Refusal(Reason::Corpus,"golden sensitive label invalid");
            coverage.insert(tag.toString());
        }
        const int width=precision_artifacts::Integer(sample,"width",1,1024);
        const auto pixelHash=sample["pixelsSha256"].toString().toLatin1();
        if(!runtime_precision::Hash(pixelHash))throw Refusal(Reason::Corpus,"golden pixel digest invalid");
        const auto pixels=readOwnedCrop(id+".gray8");
        if(pixels.size()!=width*48 || precision_artifacts::Digest(pixels)!=pixelHash)
            throw Refusal(Reason::Corpus,"golden owned pixel bytes do not match approved corpus");
        result.crops_.push_back({id,expected,pixelHash,pixels,width});
    }
    for(const auto* tag:{"O0","Il1","numbers","negation","accents"})if(!coverage.contains(tag))
        throw Refusal(Reason::Corpus,"golden corpus lacks required sensitive-token coverage");
    return result;
}
struct EngineContract {
    int inputs{},outputs{},profiles{};
    QString input,output;
    bool floatDeviceLinearInput{},floatDeviceLinearOutput{};
    precision_artifacts::Profile profile;
};
inline void CheckEngine(const EngineContract& actual) {
    if(actual.inputs!=1 || actual.outputs!=1 || actual.profiles!=1 || actual.input!="x" || actual.output!="fetch_name_0" ||
       !actual.floatDeviceLinearInput || !actual.floatDeviceLinearOutput ||
       actual.profile.minimum!=precision_artifacts::Shape{1,3,48,1024} ||
       actual.profile.optimum!=precision_artifacts::Shape{8,3,48,1024} ||
       actual.profile.maximum!=precision_artifacts::Shape{8,3,48,1024})
        throw Refusal(Reason::Model,"local BF16 engine does not satisfy approved FLOAT profile/ABI");
}
struct Observation {QString crop;int batch{},steps{},classes{};QByteArray pixelsSha256;QString text;};
struct LocalBuild {
    QByteArray uuid,builderVersion,planFileSha256;
    int major{},minor{};
};
enum class Completion { Confirmed, Unknown };
class GoldenResult {
public:
    bool Passed() const{return passed_;}
    const QJsonObject& Record() const{return record_;}
private:
    friend GoldenResult CheckGolden(const Corpus&,const LocalBuild&,std::span<const Observation>,Completion);
    QJsonObject record_;
    bool passed_{};
};
inline GoldenResult CheckGolden(const Corpus& corpus,const LocalBuild& tested,std::span<const Observation> observations,Completion completion) {
    // Never turn CUDA/context/enqueue/copy/event faults or uncertain completion
    // into a healthy-stream precision fallback. Caller owns all GPU checks/drain.
    if(completion!=Completion::Confirmed)throw std::runtime_error("BF16 golden completion uncertain; retire/quarantine");
    if(tested.uuid.size()!=16 || tested.major<8 || !tensorrt_identity::RuntimeAtLeastMinimum(tested.builderVersion) ||
       !runtime_precision::Hash(tested.planFileSha256))throw Refusal(Reason::Model,"golden tested-engine identity invalid");
    if(corpus.Crops().size()!=kGoldenSamples)throw Refusal(Reason::Corpus,"unverified/incomplete golden corpus");
    if(observations.size()>kGoldenObservations)throw Refusal(Reason::GoldenMismatch,"golden observation count exceeds bounded policy");
    QJsonArray rows;
    QSet<QString> seen;
    bool passed=observations.size()==kGoldenObservations;
    for(const auto& observation:observations) {
        if(observation.crop.size()>48 || observation.text.size()>8192)
            throw Refusal(Reason::GoldenMismatch,"golden output metadata exceeds bounded policy");
        const Crop* crop=nullptr;
        for(const auto& candidate:corpus.Crops())if(candidate.id==observation.crop){crop=&candidate;break;}
        const auto key=observation.crop+":"+QString::number(observation.batch);
        const bool row=crop && (observation.batch==1 || observation.batch==8) && observation.steps>0 && observation.steps<=512 &&
            observation.classes==corpus.ModelIdentity()["classes"].toInt() && observation.pixelsSha256==crop->pixelsSha256 && observation.text==crop->expected && !seen.contains(key);
        seen.insert(key);passed=passed&&row;
        rows.append(QJsonObject{{"crop",observation.crop},{"batch",observation.batch},{"T",observation.steps},
            {"classes",observation.classes},{"pixelsSha256",QString::fromLatin1(observation.pixelsSha256)},
            {"text",observation.text},{"pass",row}});
    }
    bool complete=observations.size()==kGoldenObservations;
    for(const auto& crop:corpus.Crops())for(const int batch:{1,8})if(!seen.contains(crop.id+":"+QString::number(batch))){passed=false;complete=false;}
    GoldenResult result;result.passed_=passed;
    result.record_={{"policy",kGoldenPolicy},{"corpusVersion",corpus.Version()},{"corpusSha256",QString::fromLatin1(corpus.Sha256())},
                    {"testedPlanSha256",QString::fromLatin1(tested.planFileSha256)},
                    {"testedDeviceUuid",QString::fromLatin1(tested.uuid.toHex())},
                    {"testedBuilderVersion",QString::fromLatin1(tested.builderVersion)},
                    {"testedComputeMajor",tested.major},{"testedComputeMinor",tested.minor},
                    {"completion","confirmed"},{"complete",complete},{"passed",passed},{"rows",rows}};
    return result; // failed outputs retained; no retry-until-pass
}
inline QJsonObject BuildIdentity(const LocalBuild& build,const Corpus& corpus) {
    if(build.uuid.size()!=16 || build.major<8 || build.minor<0 || build.minor>99 ||
       !tensorrt_identity::RuntimeAtLeastMinimum(build.builderVersion) || corpus.Crops().size()!=kGoldenSamples)
        throw Refusal(Reason::Hardware,"local BF16 builder/device/corpus identity invalid");
    return {{"model",corpus.ModelIdentity()},{"deviceUuid",QString::fromLatin1(build.uuid.toHex())},
        {"computeMajor",build.major},{"computeMinor",build.minor},{"builderTensorRtVersion",QString::fromLatin1(build.builderVersion)},
        {"goldenPolicy",kGoldenPolicy},{"corpusVersion",corpus.Version()},{"corpusSha256",QString::fromLatin1(corpus.Sha256())}};
}
inline QByteArray CacheKey(const LocalBuild& build,const Corpus& corpus) {
    return precision_artifacts::Digest("uct-public-bf16-cache-v2\n"+QJsonDocument(BuildIdentity(build,corpus)).toJson(QJsonDocument::Compact));
}
inline QJsonObject ReadyReceipt(const LocalBuild& build,const Corpus& corpus,const GoldenResult& golden,
                                const precision_artifacts::Runtime& runtime) {
    auto identity=BuildIdentity(build,corpus);
    if(!runtime_precision::Hash(build.planFileSha256))throw Refusal(Reason::Model,"local BF16 plan hash missing/invalid");
    if(!golden.Passed() || golden.Record()["corpusSha256"]!=QString::fromLatin1(corpus.Sha256()) ||
       golden.Record()["testedPlanSha256"]!=QString::fromLatin1(build.planFileSha256) ||
       golden.Record()["testedDeviceUuid"]!=QString::fromLatin1(build.uuid.toHex()) ||
       golden.Record()["testedBuilderVersion"]!=QString::fromLatin1(build.builderVersion))
        throw Refusal(Reason::GoldenMismatch,"local BF16 golden exact-text check failed; FP32 remains selected");
    if(runtime.uuid!=build.uuid || runtime.major!=build.major || runtime.minor!=build.minor ||
       !tensorrt_identity::RuntimeAtLeastMinimum(runtime.tensorRtVersion))
        throw Refusal(Reason::Runtime,"local BF16 runtime/device admission changed before readiness");
    identity["planFileSha256"]=QString::fromLatin1(build.planFileSha256);
    identity["golden"]=golden.Record();
    return {{"schema","uct-local-bf16-ready-v2"},{"identity",identity},
        {"identitySha256",QString::fromLatin1(precision_artifacts::Digest(QJsonDocument(identity).toJson(QJsonDocument::Compact)))},
        {"runtimeProvenance",QJsonObject{{"driver",runtime.driver},{"driverBuild",QString::fromLatin1(runtime.driverBuild)},
            {"driverSha256",QString::fromLatin1(runtime.driverSha256)},{"tensorRtVersion",QString::fromLatin1(runtime.tensorRtVersion)},
            {"tensorRtSha256",QString::fromLatin1(runtime.tensorRtSha256)}}}};
}
} // namespace okuflow::uct::public_bf16
