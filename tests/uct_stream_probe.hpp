#pragma once
// Opt-in GPU measurement for round 8, step 1 (docs/uct_okuread_port.md):
// does a second inference of the same engine on its own execution context and
// stream finish sooner beside the first than after it?
//
// Uses the already prepared production plans; it never prepares an engine.
// Reads one blank frame so the reader's State is adopted, then hands the
// engines to TensorRtStreamProbeForTest (src/common/uct_stream_probe.hpp),
// which makes its own contexts, buffers and streams. The recognizer runs at
// batch eight on the 1024 canvas; the detector at each distinct shape of a
// 1920x1080 read. Each configuration (1 to 3 contexts and streams, plain
// enqueue and graph launch) is timed in alternating order, and every output
// is compared byte for byte with the single-stream output of the same input.
// Not in any CTest list.
#include "uct_resident_diagnostic.hpp"
#include "../src/common/uct_stream_probe.hpp"

namespace okuflow::uct::uct_stream_probe_test {
#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT
inline void StreamConcurrency()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_STREAM_PROBE") != "1")
        QSKIP("Explicit stream-concurrency measurement not selected.");
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_STREAM_PROBE_EVIDENCE");
    QVERIFY2(QDir::isAbsolutePath(evidence) && !QFileInfo::exists(evidence),
             "OKUFLOW_UCT_STREAM_PROBE_EVIDENCE must name a new absolute file.");
    StreamProbeRequest request;
    if (qEnvironmentVariableIsSet("OKUFLOW_UCT_STREAM_PROBE_REPEATS")) {
        bool ok = false;
        request.repeats = qEnvironmentVariable("OKUFLOW_UCT_STREAM_PROBE_REPEATS").toInt(&ok);
        QVERIFY2(ok && request.repeats >= 5 && request.repeats <= 50, "OKUFLOW_UCT_STREAM_PROBE_REPEATS must be 5-50.");
    }
    // The probe needs the plain path: replay contexts share one arena.
    uct_batch_test::Environment graph("OKUFLOW_UCT_TEST_TRT_GRAPH", "0");
    QVERIFY(cudaSetDevice(0) == cudaSuccess);
    cudaDeviceProp properties{};
    QVERIFY(cudaGetDeviceProperties(&properties, 0) == cudaSuccess);
    int driver{}, runtime{};
    QVERIFY(cudaDriverGetVersion(&driver) == cudaSuccess && cudaRuntimeGetVersion(&runtime) == cudaSuccess);

    uct_batch_test::RetirementReceipts retirement;
    auto recognizer = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, retirement.Observer());
    const auto initialized = recognizer->Initialize();
    QVERIFY2(initialized.state == BackendState::kReady, qPrintable(initialized.detail));
    QTRY_VERIFY_WITH_TIMEOUT(!recognizer->Preparing(), 180000);
    QImage blank(1920, 1080, QImage::Format_ARGB32);
    blank.fill(Qt::white);
    std::atomic<bool> never{false};
    const auto read = recognizer->Recognize(blank, never);
    QVERIFY2(read.ok, qPrintable(read.error));
    const auto status = recognizer->Status();
    // A cold cache would have built engines above; measure only cached plans.
    QVERIFY2(status.detail.contains("detectorPlanCache=loaded") && status.detail.contains("recognizerPlanCache=loaded"),
             qPrintable("The stream probe measures cached plans only: " + status.detail));

    QJsonObject result;
    try {
        result = TensorRtStreamProbeForTest(*recognizer, request);
    } catch (const std::exception& error) {
        QFAIL(error.what());
    }
    result["device"] = QString::fromLatin1(properties.name);
    result["computeCapability"] = QStringLiteral("%1.%2").arg(properties.major).arg(properties.minor);
    result["cudaDriver"] = driver;
    result["cudaRuntime"] = runtime;
    result["backend"] = status.name + QStringLiteral(" | ") + status.detail;
    // The reader still reads after the probe.
    const auto after = recognizer->Recognize(blank, never);
    QVERIFY2(after.ok, qPrintable(after.error));
    recognizer.reset();
    QVERIFY2(retirement.Wait().Passed(), "TensorRT State retirement after the stream probe failed.");

    QFile file(evidence);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), qPrintable(evidence));
    file.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    file.close();

    // Summary, and the identity verdict this slot asserts.
    const auto summarize = [](const QString& name, const QJsonObject& engine) {
        int mismatches = 0;
        for (const auto mode : {"plain", "graph"})
            for (const auto& row : engine["modes"].toObject()[mode].toArray()) {
                const auto r = row.toObject();
                mismatches += r["mismatches"].toInt();
                qInfo().noquote() << name << mode << "streams" << r["streams"].toInt() << "median ms"
                                  << r["medianMs"].toDouble() << "speedup" << r["speedup"].toDouble()
                                  << "verified" << r["verifiedOutputs"].toInt() << "mismatches" << r["mismatches"].toInt();
            }
        return mismatches;
    };
    int mismatches = summarize(QStringLiteral("recognizer"), result["recognizer"].toObject());
    for (const auto& row : result["detector"].toArray())
        mismatches += summarize(QStringLiteral("detector"), row.toObject());
    QVERIFY2(mismatches == 0, "An inference on several streams differed from the single-stream output.");
}
#else
inline void StreamConcurrency()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_STREAM_PROBE") != "1")
        QSKIP("Explicit stream-concurrency measurement not selected.");
    QFAIL("The stream-concurrency measurement needs the CUDA analysis pool and TensorRT adapter.");
}
#endif
} // namespace okuflow::uct::uct_stream_probe_test
