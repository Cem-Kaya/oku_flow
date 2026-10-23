#pragma once

// Offline experiment boundary, deliberately separate from the schema-1 local
// approval. No environment, CUDA/TRT API, builder, cache write or receipt I/O.
#include "uct_runtime_precision.hpp"
#include "okuflow/common/uct_model_pins.hpp"
#include <QJsonArray>
#include <array>
#include <cstdint>
#include <iterator>
#include <span>

namespace okuflow::uct::precision_artifacts {
using runtime_precision::ArtifactError;
using Shape = std::array<int, 4>;
enum class Role { Detector, Recognizer };
enum class Width { Fixed1024, OrderedBuckets };
enum class OutputAbi { DetectorMapV1, CtcProbabilitiesV1 };
struct Profile { Shape minimum{}, optimum{}, maximum{}; };
struct Runtime {
    QByteArray uuid; // native CUDA UUID bytes, never an adapter ordinal
    int driver{}, major{}, minor{};
    QByteArray tensorRtVersion, tensorRtSha256; // loaded inference DLL provenance
    QByteArray driverBuild, driverSha256; // provenance only, never approval/key data
};
struct Identity {
    // Exact canonical metadata is compared with a separately reviewed catalog.
    // A selector is not an approval authority, even with internally valid hashes.
    QJsonObject metadata;
    Role role{};
    Width width{Width::Fixed1024};
    OutputAbi abi{};
    Profile profile;
    int capacity{}, classes{};
};
struct Entry { Identity identity; QString graphPath, planPath; };
struct Selection { QString root; Entry detector, recognizer; };
struct VerifiedPlan { Identity identity; QByteArray payload; };
struct RecognizerPolicy {
    Width width{Width::Fixed1024};
    int capacity{8};
    OutputAbi abi{OutputAbi::CtcProbabilitiesV1};
};
struct VerifiedSelection {
    // Commit this complete value only after BOTH networks have been constructed
    // and checked. Never copy the engine pointer without its effective policy.
    VerifiedPlan detector, recognizer;
    RecognizerPolicy policy;
    Runtime runtime;
    QJsonObject Receipt() const {
        return {{"schema", 2}, {"detector", detector.identity.metadata},
                {"recognizer", recognizer.identity.metadata},
                {"runtimeProvenance", QJsonObject{{"deviceUuid",QString::fromLatin1(runtime.uuid.toHex())},
                    {"driver",runtime.driver},{"driverBuild",QString::fromLatin1(runtime.driverBuild)},
                    {"driverSha256",QString::fromLatin1(runtime.driverSha256)},
                    {"tensorRtVersion",QString::fromLatin1(runtime.tensorRtVersion)},
                    {"tensorRtSha256",QString::fromLatin1(runtime.tensorRtSha256)}}}};
    }
};

inline QString RoleName(Role role) { return role == Role::Detector ? "detector" : "recognizer"; }
inline QString WidthName(Width width) { return width == Width::OrderedBuckets ? "bucketed-ordered-v1" : "fixed1024"; }
inline QString AbiName(OutputAbi abi) { return abi == OutputAbi::DetectorMapV1 ? "detector-map-v1" : "ctc-probabilities-v1"; }
inline QByteArray Digest(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
inline QByteArray DictionaryHash(const QStringList& dictionary) {
    // Length-prefix UTF-8 entries, including blank and appended space; no Unicode
    // normalization, locale formatting or delimiter ambiguity.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray("uct-dictionary-v1\n"));
    for (const auto& word : dictionary) {
        const auto utf8 = word.toUtf8();
        hash.addData(QByteArray::number(utf8.size()) + ':' + utf8);
    }
    return hash.result().toHex();
}
inline int Integer(const QJsonObject& json, const char* key, int low, int high) {
    const auto value = json.value(key);
    const double number = value.toDouble(-1);
    if (!value.isDouble() || number < low || number > high || number != int(number))
        throw ArtifactError("precision artifact has an invalid integer field");
    return int(number);
}
inline Shape ParseShape(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() != 4)
        throw ArtifactError("precision artifact needs an exact rank-four profile");
    Shape shape{};
    const auto array = value.toArray();
    for (int i = 0; i < 4; ++i) {
        const auto number = array[i].toDouble(-1);
        if (!array[i].isDouble() || number < 1 || number > 1024 || number != int(number))
            throw ArtifactError("precision artifact profile dimension is invalid");
        shape[i] = int(number);
    }
    return shape;
}
inline Identity ParseIdentity(const QJsonObject& json) {
    // Reject misspelled/extra fields so no approval appears to bind data which
    // the loader actually ignores. Strings below are technical, not UI copy.
    constexpr const char* strings[]{"id", "role", "tier", "precision", "sourceSha256", "graphSha256",
        "configSha256", "dictionarySha256", "conversionRecipeSha256", "graphAuditSha256",
        "planFileSha256", "deviceUuid", "tensorRtVersion", "inputName", "outputName",
        "inputType", "outputType", "outputAbi", "widthPolicy", "tf32",
        "inputLocation", "outputLocation", "inputFormat", "outputFormat", "graphRelativePath", "planRelativePath"};
    constexpr const char* numbers[]{"computeMajor", "computeMinor", "capacity", "classes",
        "optimizationLevel", "workspaceMiB", "auxStreams"};
    if (json.size() != int(std::size(strings) + std::size(numbers) + 3))
        throw ArtifactError("precision artifact identity fields do not match schema 2");
    for (const auto* key : strings)
        if (!json[key].isString() || json[key].toString().isEmpty() || json[key].toString().size() > 256)
            throw ArtifactError("precision artifact string field is missing/invalid");
    for (const auto* key : {"graphRelativePath", "planRelativePath"}) {
        const auto path = json[key].toString();
        if (QDir::isAbsolutePath(path) || path.contains('\\') || path.contains(':') || path.contains(QChar::Null) ||
            path.startsWith('/') || path.split('/').contains("..") || QDir::cleanPath(path) != path)
            throw ArtifactError("precision artifact locator must stay within its approved root");
    }
    for (const auto* key : numbers) Integer(json, key, 0, 1000000);
    for (const auto* key : {"sourceSha256", "graphSha256", "configSha256", "dictionarySha256",
                           "conversionRecipeSha256", "graphAuditSha256", "planFileSha256"})
        if (!runtime_precision::Hash(json[key].toString().toLatin1()))
            throw ArtifactError("precision artifact identity hash is invalid");
    const auto uuid = json["deviceUuid"].toString().toLatin1();
    if (uuid.size() != 32 || QByteArray::fromHex(uuid).toHex() != uuid ||
        !tensorrt_identity::RuntimeAtLeastMinimum(json["tensorRtVersion"].toString().toLatin1()) ||
        json["inputType"] != "FLOAT" || json["outputType"] != "FLOAT" ||
        json["tier"] != "medium" || json["tf32"] != "default" ||
        json["inputLocation"] != "DEVICE" || json["outputLocation"] != "DEVICE" ||
        json["inputFormat"] != "LINEAR" || json["outputFormat"] != "LINEAR" ||
        Integer(json,"optimizationLevel",2,2) != 2 || Integer(json,"workspaceMiB",256,256) != 256 ||
        Integer(json,"auxStreams",0,0) != 0)
        throw ArtifactError("precision artifact runtime/type/build policy is unsupported");
    Identity result;
    result.metadata = json;
    result.profile = {ParseShape(json["minimum"]), ParseShape(json["optimum"]), ParseShape(json["maximum"])};
    result.capacity = Integer(json,"capacity",1,8);
    result.classes = Integer(json,"classes",1,100000);
    Integer(json,"computeMajor",1,99);
    Integer(json,"computeMinor",0,99);
    if (json["role"] == "detector") {
        result.role = Role::Detector;
        result.abi = OutputAbi::DetectorMapV1;
        if ((json["precision"] != "fp32" && json["precision"] != "fp16-linear") ||
            json["outputAbi"] != AbiName(result.abi) || json["widthPolicy"] != "detector-dynamic-v1" ||
            result.capacity != 1 || result.classes != 1 ||
            result.profile.minimum != Shape{1,3,32,32} || result.profile.optimum != Shape{1,3,544,960} ||
            result.profile.maximum != Shape{1,3,960,960})
            throw ArtifactError("precision detector role/profile/ABI is unsupported");
    } else if (json["role"] == "recognizer") {
        result.role = Role::Recognizer;
        result.abi = OutputAbi::CtcProbabilitiesV1;
        if (json["widthPolicy"] == "bucketed-ordered-v1") result.width = Width::OrderedBuckets;
        else if (json["widthPolicy"] != "fixed1024") throw ArtifactError("precision recognizer width policy is unsupported");
        const bool variable = result.width == Width::OrderedBuckets;
        if ((json["precision"] != "bf16-linear" && json["precision"] != "fp32") ||
            (json["precision"] == "fp32" && variable) || json["outputAbi"] != AbiName(result.abi) ||
            result.capacity != 8 || result.classes != 18710 ||
            result.profile.minimum != Shape{1,3,48,variable ? 128 : 1024} ||
            result.profile.optimum != Shape{8,3,48,variable ? 512 : 1024} ||
            result.profile.maximum != Shape{8,3,48,1024})
            throw ArtifactError("precision recognizer role/profile/ABI is unsupported");
    } else throw ArtifactError("precision artifact role is unsupported");
    const auto* pin = FindPaddleModelPin(result.role == Role::Detector, ModelTier::Medium);
    if (json["sourceSha256"].toString().toLatin1() != QByteArray(pin->modelSha256.data(),qsizetype(pin->modelSha256.size())) ||
        json["configSha256"].toString().toLatin1() != QByteArray(pin->configSha256.data(),qsizetype(pin->configSha256.size())) ||
        (json["precision"] == "fp32" && json["sourceSha256"] != json["graphSha256"]))
        throw ArtifactError("precision artifact source/configuration is not the pinned medium model");
    return result;
}
inline QByteArray IdentityKey(const Identity& identity) {
    // Receipt identity only. Never a request to rename/copy an existing plan.
    // Metadata binds builder TRT, never loaded runtime/driver provenance.
    return Digest(QByteArray("uct-role-artifact-v2\n") + QJsonDocument(identity.metadata).toJson(QJsonDocument::Compact));
}
inline Identity ApprovedIdentity(const QJsonObject& json, std::span<const QJsonObject> approved) {
    auto result = ParseIdentity(json);
    int matchingId = 0, exact = 0;
    for (const auto& record : approved) {
        if (record["id"] == json["id"]) {
            ++matchingId;
            if (record == json) ++exact;
        }
    }
    if (matchingId != 1 || exact != 1)
        throw ArtifactError("precision artifact has no unique independently approved identity");
    return result;
}
inline Selection ParseManifest(const QByteArray& bytes, std::span<const QJsonObject> approved) {
    if (bytes.isEmpty() || bytes.size() > runtime_precision::kManifestLimit)
        throw ArtifactError("precision family manifest exceeds its bounds");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes,&error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw ArtifactError("precision family manifest JSON is invalid");
    const auto json = document.object();
    if (json.size() != 5 || !json["schema"].isDouble() || json["schema"].toDouble() != 2 ||
        !json["prepare"].isBool() || json["prepare"].toBool())
        throw ArtifactError("precision family requires schema 2 and explicit prepare=false");
    Selection result;
    result.root = json["artifactRoot"].toString();
    if (!runtime_precision::LocalAbsolute(result.root)) throw ArtifactError("precision family root must be local and absolute");
    const auto entry = [&](Role role) {
        const auto value = json.value(RoleName(role));
        if (!value.isObject()) throw ArtifactError("precision family role is missing");
        const auto object = value.toObject();
        if (object.size() != 3 || !object["identity"].isObject()) throw ArtifactError("precision family entry is invalid");
        Entry e{ApprovedIdentity(object["identity"].toObject(),approved),object["graphPath"].toString(),object["planPath"].toString()};
        if (e.identity.role != role || !runtime_precision::LocalAbsolute(e.graphPath) || !runtime_precision::LocalAbsolute(e.planPath) ||
            !runtime_precision::SamePath(e.graphPath,QDir(result.root).filePath(e.identity.metadata["graphRelativePath"].toString())) ||
            !runtime_precision::SamePath(e.planPath,QDir(result.root).filePath(e.identity.metadata["planRelativePath"].toString())))
            throw ArtifactError("precision family role/path does not match its identity");
        return e;
    };
    result.detector = entry(Role::Detector);
    result.recognizer = entry(Role::Recognizer);
    return result;
}
inline Selection LoadManifest(const QString& path, std::span<const QJsonObject> approved) {
    if (!runtime_precision::LocalAbsolute(path)) throw ArtifactError("precision family manifest must be local and absolute");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() > runtime_precision::kManifestLimit)
        throw ArtifactError("precision family manifest is missing/unreadable/out of bounds");
    const auto bytes=file.read(runtime_precision::kManifestLimit + 1);
    if (file.error()!=QFileDevice::NoError || !file.atEnd()) throw ArtifactError("precision family manifest read did not complete");
    return ParseManifest(bytes,approved);
}
inline void CheckRuntime(const Identity& identity, const Runtime& runtime) {
    const auto& json = identity.metadata;
    if (runtime.uuid.size() != 16 || json["deviceUuid"].toString().toLatin1() != runtime.uuid.toHex() ||
        json["computeMajor"].toInt() != runtime.major || json["computeMinor"].toInt() != runtime.minor ||
        !tensorrt_identity::RuntimeAtLeastMinimum(runtime.tensorRtVersion) ||
        (json["precision"] == "bf16-linear" && runtime.major < 8))
        throw ArtifactError("precision artifact device mismatch or TensorRT runtime below minimum/invalid ABI11");
}
inline VerifiedPlan ReadPlan(const QString& root, const Entry& entry, const Runtime& runtime) {
    CheckRuntime(entry.identity,runtime);
    if (!runtime_precision::SamePath(entry.graphPath,QDir(root).filePath(entry.identity.metadata["graphRelativePath"].toString())) ||
        !runtime_precision::SamePath(entry.planPath,QDir(root).filePath(entry.identity.metadata["planRelativePath"].toString())))
        throw ArtifactError("precision artifact locator changed after selection");
    runtime_precision::CanonicalWithin(root,entry.graphPath);
    runtime_precision::ReadVerified(entry.graphPath,entry.identity.metadata["graphSha256"].toString().toLatin1(),runtime_precision::kGraphLimit);
    runtime_precision::CanonicalWithin(root,entry.planPath);
    const auto bytes = runtime_precision::ReadVerified(entry.planPath,entry.identity.metadata["planFileSha256"].toString().toLatin1(),runtime_precision::kPlanLimit);
    if (bytes.size() <= 32 || QCryptographicHash::hash(bytes.sliced(32),QCryptographicHash::Sha256) != bytes.first(32))
        throw ArtifactError("precision artifact cached-plan payload digest mismatch");
    return {entry.identity,bytes.sliced(32)};
}
inline VerifiedSelection ReadSelection(const Selection& selection, const Runtime& runtime, const PaddleAssets& assets) {
    if (!assets.IsValid() || assets.detectorSha256 != selection.detector.identity.metadata["sourceSha256"].toString().toLatin1() ||
        assets.recognizerSha256 != selection.recognizer.identity.metadata["sourceSha256"].toString().toLatin1() ||
        assets.dictionary.size() != selection.recognizer.identity.classes)
        throw ArtifactError("precision family does not match the selected pinned models/dictionary");
    const auto dictionary = DictionaryHash(assets.dictionary);
    for (const auto* e : {&selection.detector,&selection.recognizer}) {
        if (e->identity.metadata["dictionarySha256"].toString().toLatin1() != dictionary)
            throw ArtifactError("precision family dictionary identity mismatch");
        const auto* pin = FindPaddleModelPin(e->identity.role == Role::Detector,ModelTier::Medium);
        const auto directory = QString::fromUtf8(pin->directory.data(),qsizetype(pin->directory.size()));
        // Revalidate original source and config bytes immediately before plan
        // acceptance; detached paths/metadata cannot replace selected assets.
        const auto source = e->identity.role == Role::Detector ? assets.detector : assets.recognizer;
        runtime_precision::ReadVerified(source,e->identity.metadata["sourceSha256"].toString().toLatin1(),runtime_precision::kGraphLimit);
        runtime_precision::ReadVerified(QDir(assets.root).filePath(directory + "/inference.yml"),
            e->identity.metadata["configSha256"].toString().toLatin1(),8 * 1024 * 1024 + 1);
    }
    VerifiedSelection result{ReadPlan(selection.root,selection.detector,runtime),ReadPlan(selection.root,selection.recognizer,runtime),
        {selection.recognizer.identity.width,selection.recognizer.identity.capacity,selection.recognizer.identity.abi},runtime};
    return result;
}
template<class Extent>
inline void CheckEngineProfile(const Identity& identity, bool detection, int profiles,
                               const char* input, const char* output, bool floatInput, bool floatOutput,
                               bool deviceLinearInput, bool deviceLinearOutput,
                               const Extent* minimum, int minRank, const Extent* optimum, int optRank,
                               const Extent* maximum, int maxRank) {
    const auto matches = [](const Extent* actual, int rank, const Shape& expected) {
        if (!actual || rank != 4) return false;
        for (int i=0;i<4;++i) if (actual[i] != expected[i]) return false;
        return true;
    };
    if (detection != (identity.role == Role::Detector) || profiles != 1 || !input || !output ||
        identity.metadata["inputName"].toString() != QString::fromUtf8(input) ||
        identity.metadata["outputName"].toString() != QString::fromUtf8(output) || !floatInput || !floatOutput ||
        !deviceLinearInput || !deviceLinearOutput ||
        !matches(minimum,minRank,identity.profile.minimum) || !matches(optimum,optRank,identity.profile.optimum) ||
        !matches(maximum,maxRank,identity.profile.maximum))
        throw ArtifactError("precision engine role/type/name/profile does not match approved artifact");
}
inline void CheckOutput(const Identity& identity, std::span<const std::int64_t> shape,
                        int batch, int height, int width) {
    if (identity.role == Role::Detector) {
        if (shape.size()!=4 || batch!=1 || shape[0]!=1 || shape[1]!=1 || shape[2]!=height || shape[3]!=width)
            throw std::runtime_error("precision detector output violates FLOAT map ABI");
    } else if (shape.size()!=3 || shape[0]!=batch || batch<1 || batch>identity.capacity || shape[1]<1 || shape[2]!=identity.classes)
        throw std::runtime_error("precision recognizer output violates CTC probability ABI");
    // Runtime output failure is not an ArtifactError: no engine retry after work.
}
inline QString ActualStatus(const VerifiedSelection& selection) {
    const auto one = [](const VerifiedPlan& plan) {
        const auto& id = plan.identity;
        return RoleName(id.role) + "=" + id.metadata["precision"].toString() + ", artifact=" + id.metadata["id"].toString() +
            ", plan=" + id.metadata["planFileSha256"].toString() + ", profile=" +
            QString::fromLatin1(IdentityKey(id)) + ", ABI=" + AbiName(id.abi) + ", FLOAT I/O";
    };
    return one(selection.detector) + "; " + one(selection.recognizer) + "; width=" + WidthName(selection.policy.width) +
        ", N=" + QString::number(selection.policy.capacity) + ", loaded TRT=" +
        QString::fromLatin1(selection.runtime.tensorRtVersion) + ". Mixed graph arithmetic; TF32 allowed. ";
}
} // namespace okuflow::uct::precision_artifacts
