#pragma once

// Private application boundary. Diagnostics keep the public FP32 factory.
// This policy can select an existing reviewed plan, never prepare one.
#include "okuflow/common/uct_paddle.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <stdexcept>

namespace okuflow::uct::runtime_precision {
inline constexpr char kEnvironment[] = "OKUFLOW_UCT_LOCAL_BF16_MANIFEST";
inline constexpr char kDetectorSource[] = "eb13b44b25bb36f89528b68720af8a61d9cf381176107f465db1757b65d086e1";
inline constexpr char kRecognizerSource[] = "9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba";
inline constexpr char kRecognizerGraph[] = "ecd8ffc92aa1b9f7a33b0ad39757b64887c465f15bb54853936b2136f94e5835";
inline constexpr char kPlanFileSha256[] = "155c74b5b856467e49853190c881077c9af3200203a196f9e737f7533e6f5495";
inline constexpr char kDeviceUuid[] = "230556cb9d98d5c17318d9ac52e6697d";
inline constexpr qint64 kManifestLimit = 64 * 1024;
inline constexpr qint64 kGraphLimit = 256 * 1024 * 1024;
inline constexpr qint64 kPlanLimit = 256 * 1024 * 1024;

// Only this exception permits pre-inference FP32 selection. CUDA, allocation,
// context creation and inference faults follow ordinary retirement/failover.
class ArtifactError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct Request {
    QString manifest;
    bool Requested() const { return !manifest.isEmpty(); }
};
inline Request ReadApplicationRequest() { return {qEnvironmentVariable(kEnvironment)}; }

struct Graph {
    QString precision, path;
    QByteArray sourceSha256, sha256;
};
struct Selection {
    QString artifactRoot;
    Graph detector, recognizer;
};
struct Device {
    QByteArray uuid;
    int driver{}, major{};
    QByteArray tensorRtVersion{"11.4.0.106"};
};
inline bool Hash(const QByteArray& hash)
{
    return hash.size() == 64 && QByteArray::fromHex(hash).toHex() == hash;
}
inline bool LocalAbsolute(const QString& path)
{
    // Runtime assets are private local files, not a URL, UNC or drive-relative path.
    const auto p = QDir::fromNativeSeparators(path);
    return !p.isEmpty() && p.size() <= 32767 && QDir::isAbsolutePath(p) &&
           !p.startsWith("//") && !p.contains(QChar::Null) &&
           !p.mid(2).contains(':');
}
inline bool Within(const QString& root, const QString& path)
{
    const auto relative = QDir(root).relativeFilePath(path);
    return relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
}
inline bool SamePath(const QString& a, const QString& b)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(
               QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}
inline QByteArray PrecisionPlanKey(const Graph& graph, const Device& device,
                                   bool detection, int batch, const QByteArray& width = "fixed1024")
{
    // Byte-identical to the reviewed offline experiment for this fixed profile.
    const auto identity = QByteArray("uct-precision-v1\n") + graph.precision.toUtf8() + '\n' +
        graph.sourceSha256 + '\n' + graph.sha256 + '\n' + device.uuid.toHex() + '\n' +
        QByteArray::number(device.driver) + "\ntrt" + device.tensorRtVersion +
        "-tf32-default-opt2-workspace256MiB-aux0-" + width + '\n' +
        (detection ? "det-32-544x960-960x960" : "rec-48x1024") + '\n' + QByteArray::number(batch);
    return QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
}
inline QString Unsupported(ModelTier detector, ModelTier recognizer, int batch,
                           bool variableWidth, bool prepareWidth, const Device& device)
{
    if (detector != ModelTier::Medium || recognizer != ModelTier::Medium ||
        batch != 8 || variableWidth || prepareWidth)
        return "local BF16 supports only medium/medium, N8, fixed1024 without preparation";
    if (device.major < 8) return "local BF16 requires Ampere or newer";
    if (device.uuid.toHex() != kDeviceUuid || device.driver != 13040 ||
        device.tensorRtVersion != "11.4.0.106")
        return "no approved local BF16 plan for this device/driver/TensorRT";
    return {};
}
inline Selection ParseManifest(const QByteArray& bytes, const QString& detectorPath)
{
    if (bytes.isEmpty() || bytes.size() > kManifestLimit)
        throw ArtifactError("local BF16 manifest exceeds its bounds");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw ArtifactError("invalid local BF16 manifest JSON");
    const auto json = document.object();
    if (!json["schema"].isDouble() || json["schema"].toDouble() != 1 ||
        !json["prepare"].isBool() || json["prepare"].toBool())
        throw ArtifactError("local BF16 requires schema 1 and explicit prepare=false");
    Selection result;
    result.artifactRoot = json["artifactRoot"].toString();
    if (!LocalAbsolute(result.artifactRoot))
        throw ArtifactError("local BF16 artifact root must be an absolute local directory");
    const auto graph = [&](const char* role, const char* precision, const char* source, const char* hash) {
        if (!json[role].isObject()) throw ArtifactError("missing local BF16 graph role");
        const auto value = json[role].toObject();
        Graph g{value["precision"].toString(), value["path"].toString(),
                value["sourceSha256"].toString().toLatin1(), value["sha256"].toString().toLatin1()};
        if (g.precision != precision || !Hash(g.sourceSha256) || !Hash(g.sha256) ||
            g.sourceSha256 != source || g.sha256 != hash || !LocalAbsolute(g.path))
            throw ArtifactError("local BF16 graph precision/source/hash/role is not approved");
        return g;
    };
    result.detector = graph("detector", "fp32", kDetectorSource, kDetectorSource);
    result.recognizer = graph("recognizer", "bf16-linear", kRecognizerSource, kRecognizerGraph);
    const auto expected = QDir(result.artifactRoot).filePath(
        QString("models/bf16-linear/%1/%2.onnx").arg(kRecognizerSource, kRecognizerGraph));
    if (!SamePath(result.detector.path, detectorPath) || !SamePath(result.recognizer.path, expected) ||
        !Within(result.artifactRoot, result.recognizer.path))
        throw ArtifactError("local BF16 graph paths do not match their pinned roles");
    return result;
}
inline Selection LoadManifest(const Request& request, const QString& detectorPath)
{
    if (!LocalAbsolute(request.manifest))
        throw ArtifactError("local BF16 manifest must be an absolute local file");
    QFile file(request.manifest);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() > kManifestLimit)
        throw ArtifactError("local BF16 manifest missing/unreadable/out of bounds");
    return ParseManifest(file.read(kManifestLimit + 1), detectorPath);
}
inline void CanonicalWithin(const QString& root, const QString& path)
{
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    const auto canonicalPath = QFileInfo(path).canonicalFilePath();
    if (canonicalRoot.isEmpty() || canonicalPath.isEmpty() || !Within(canonicalRoot, canonicalPath))
        throw ArtifactError("local BF16 artifact missing or escapes its canonical root");
}
inline QByteArray ReadVerified(const QString& path, const QByteArray& hash, qint64 limit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() >= limit)
        throw ArtifactError("local BF16 artifact missing/unreadable/out of bounds");
    const auto bytes = file.read(limit);
    if (file.error() != QFileDevice::NoError || !file.atEnd() ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != hash)
        throw ArtifactError("local BF16 artifact hash mismatch");
    return bytes;
}
inline QString PlanPath(const Selection& selection, const Device& device)
{
    return QDir(selection.artifactRoot).filePath("engines/bf16-linear/" +
        QString::fromLatin1(PrecisionPlanKey(selection.recognizer, device, false, 8)) + ".plan");
}
inline QByteArray ReadPlan(const Selection& selection, const Device& device)
{
    CanonicalWithin(selection.artifactRoot, selection.recognizer.path);
    // Recheck converted graph bytes immediately before accepting its cached plan.
    ReadVerified(selection.recognizer.path, selection.recognizer.sha256, kGraphLimit);
    const auto path = PlanPath(selection, device);
    CanonicalWithin(selection.artifactRoot, path);
    const auto bytes = ReadVerified(path, kPlanFileSha256, kPlanLimit);
    if (bytes.size() <= 32 || QCryptographicHash::hash(bytes.sliced(32), QCryptographicHash::Sha256) != bytes.first(32))
        throw ArtifactError("local BF16 cached plan payload digest mismatch");
    return bytes.sliced(32);
}
inline QString ActualStatus(bool bf16, const QString& fallback = {})
{
    return (bf16 ? QStringLiteral("Recognition graph=BF16-linear, mixed BF16/FP16/FP32 arithmetic, FLOAT I/O; detector=FP32 medium (TF32 allowed). ")
                 : QStringLiteral("Recognizer=FP32 (TF32 allowed), FLOAT I/O; detector=FP32 (TF32 allowed). ")) +
           (fallback.isEmpty() ? QString() : "Local BF16 fallback: " + fallback + ". ");
}
} // namespace okuflow::uct::runtime_precision

namespace okuflow::uct {
std::unique_ptr<Recognizer> CreateProductionTensorRtRecognizer(ModelTier detector, ModelTier recognizer,
    TensorRtRetirementObserver retirementObserver = {});
} // namespace okuflow::uct
