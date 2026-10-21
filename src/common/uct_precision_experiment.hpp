#pragma once

// Explicit offline diagnostic API. The application/default factory never reads
// these options or an environment selector for reduced precision.
#include "okuflow/common/uct_paddle.hpp"
#include <QFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <stdexcept>

namespace okuflow::uct {
struct PrecisionGraph {
    QString precision{QStringLiteral("fp32")}, path;
    QByteArray sourceSha256, sha256;
};
struct TensorRtPrecisionExperiment {
    PrecisionGraph detector, recognizer;
    QString artifactRoot, receiptRoot;
    bool prepare{};
};
inline bool PrecisionHash(const QByteArray& hash)
{
    return hash.size() == 64 && QByteArray::fromHex(hash).toHex() == hash;
}
inline QByteArray PrecisionPlanKey(const PrecisionGraph& graph, const QByteArray& uuid,
                                  int driver, bool detection, int batch)
{
    const auto identity = QByteArray("uct-precision-v1\n") + graph.precision.toUtf8() + '\n' +
        graph.sourceSha256 + '\n' + graph.sha256 + '\n' + uuid.toHex() + '\n' + QByteArray::number(driver) +
        "\ntrt11.4.0.106-tf32-default-opt2-workspace256MiB-aux0-fixed1024\n" +
        (detection ? "det-32-544x960-960x960" : "rec-48x1024") + '\n' + QByteArray::number(batch);
    return QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
}
inline TensorRtPrecisionExperiment LoadPrecisionExperiment(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024*1024)
        throw std::runtime_error("Cannot read bounded precision experiment manifest.");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Invalid precision experiment JSON.");
    const auto json = document.object();
    TensorRtPrecisionExperiment result;
    if (json["schema"].toInt() != 1 || !json["prepare"].isBool())
        throw std::runtime_error("Precision manifest schema/prepare must be explicit.");
    result.prepare = json["prepare"].toBool();
    result.artifactRoot = json["artifactRoot"].toString();
    result.receiptRoot = json["receiptRoot"].toString();
    const auto absolute = [](const QString& p) { return !p.isEmpty() && QDir::isAbsolutePath(p); };
    if (!absolute(result.artifactRoot) || !absolute(result.receiptRoot))
        throw std::runtime_error("Precision artifact and receipt roots must be absolute.");
    const auto graph = [&](const char* key) {
        const auto value = json[key].toObject();
        PrecisionGraph g;
        g.precision = value["precision"].toString();
        if (g.precision != "fp32" && g.precision != "fp16-linear" &&
            g.precision != "bf16-linear" && g.precision != "int8-qdq" &&
            g.precision != "fp8-qdq" && g.precision != "int4-woq")
            throw std::runtime_error("Precision selection must be explicit; no fallback is allowed.");
        g.sourceSha256 = value["sourceSha256"].toString().toLatin1();
        g.sha256 = value["sha256"].toString().toLatin1();
        g.path = value["path"].toString();
        if (!PrecisionHash(g.sourceSha256) || !PrecisionHash(g.sha256) || !absolute(g.path) ||
            (g.precision == "fp32" && g.sourceSha256 != g.sha256))
            throw std::runtime_error("Invalid precision graph identity.");
        const auto relative = QDir(result.artifactRoot).relativeFilePath(g.path);
        if (g.precision != "fp32" && (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative)))
            throw std::runtime_error("Variant graphs must remain under the private artifact root.");
        return g;
    };
    result.detector = graph("detector"); result.recognizer = graph("recognizer");
    return result;
}
std::unique_ptr<Recognizer> CreateTensorRtPrecisionExperiment(
    const QString& root, const QString& sdk, std::shared_ptr<const TensorRtPrecisionExperiment> experiment,
    TensorRtRetirementObserver retirementObserver = {},
    std::shared_ptr<TensorRtCropDiagnostic> cropDiagnostic = {});
} // namespace okuflow::uct
