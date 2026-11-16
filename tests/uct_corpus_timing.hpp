#pragma once
// Opt-in, unregistered corpus timing for same-binary switch comparisons.
//
// One process reads every selected image through the production GPU factory
// and RecognitionScheduler::SubmitDevice: one first read, then the requested
// warm reads, each on a fresh owned analysis capture completed before
// submission. scripts/uct/perf_measure.py runs these processes in reversed
// cohorts and compares arms. It never prepares an engine, changes a default
// or runs in ordinary CTest. See docs/uct_corpus_timing.md.
#include "uct_resident_diagnostic.hpp"

namespace okuflow::uct::uct_corpus_test {
#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT
inline void CorpusTiming()
{
    using namespace uct_resident_test;
    if (qEnvironmentVariable("OKUFLOW_UCT_CORPUS_TIMING") != "1")
        QSKIP("Explicit corpus timing not selected.");
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_EVIDENCE");
    QVERIFY2(QDir::isAbsolutePath(evidence) && !QFileInfo::exists(evidence),
             "OKUFLOW_UCT_CORPUS_EVIDENCE must name a new absolute file.");
    QStringList paths;
    for (const auto& path : qEnvironmentVariable("OKUFLOW_UCT_CORPUS_IMAGES").split(QLatin1Char(';'), Qt::SkipEmptyParts))
        paths.push_back(QFileInfo(path.trimmed()).absoluteFilePath());
    QVERIFY2(!paths.isEmpty() && paths.size() <= 64, "Select 1-64 images with OKUFLOW_UCT_CORPUS_IMAGES.");
    bool ok = true;
    const int passes = qEnvironmentVariableIsSet("OKUFLOW_UCT_CORPUS_PASSES")
        ? qEnvironmentVariable("OKUFLOW_UCT_CORPUS_PASSES").toInt(&ok) : 10;
    QVERIFY2(ok && passes >= 1 && passes <= 50, "OKUFLOW_UCT_CORPUS_PASSES must be 1-50.");
    const int gapMs = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_GAP_MS").toInt();
    QVERIFY2(gapMs >= 0 && gapMs <= 5000, "OKUFLOW_UCT_CORPUS_GAP_MS must be 0-5000.");
    const auto stampKind = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_STAMP");
    QVERIFY2(stampKind.isEmpty() || stampKind == "camera" || stampKind == "media",
             "OKUFLOW_UCT_CORPUS_STAMP must be camera or media.");
    const bool media = stampKind == "media";
    const auto tiers = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_PAIR", "medium:medium").split(QLatin1Char(':'));
    const auto detector = tiers.size() == 2 ? ParseModelTier(tiers[0]) : std::nullopt;
    const auto recognizerTier = tiers.size() == 2 ? ParseModelTier(tiers[1]) : std::nullopt;
    QVERIFY2(detector && recognizerTier, "OKUFLOW_UCT_CORPUS_PAIR must be detector:recognizer.");
    const auto cache = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_ENGINE_CACHE");
    QVERIFY2(cache.isEmpty() || QDir::isAbsolutePath(cache), "The engine cache override must be absolute.");
    const bool profile = qEnvironmentVariable("OKUFLOW_UCT_CORPUS_PROFILE") == "1";

    QVERIFY(cudaSetDevice(0) == cudaSuccess);
    cudaDeviceProp properties{};
    QVERIFY(cudaGetDeviceProperties(&properties, 0) == cudaSuccess);
    int driver{};
    QVERIFY(cudaDriverGetVersion(&driver) == cudaSuccess);

    struct Scene {
        QString path, sha256;
        QImage image;
        void* device{};
    };
    DeviceStream stream; // destroyed last
    DeviceUploads uploads;
    QVERIFY2(stream.value, "Fixture CUDA stream unavailable.");
    uploads.stream = stream.value;
    std::vector<Scene> scenes;
    for (const auto& path : paths) {
        Scene scene;
        scene.path = path;
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        const auto bytes = file.readAll();
        scene.sha256 = Hex(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        scene.image = QImage::fromData(bytes).convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(!scene.image.isNull() && scene.image.width() <= 8192 && scene.image.height() <= 8192, qPrintable(path));
        scene.device = uploads.Allocate(std::size_t(scene.image.sizeInBytes()));
        QVERIFY2(scene.device, "Source upload allocation failed.");
        QVERIFY(cudaMemcpyAsync(scene.device, scene.image.constBits(), std::size_t(scene.image.sizeInBytes()),
                                cudaMemcpyHostToDevice, stream.value) == cudaSuccess);
        scenes.push_back(std::move(scene));
    }
    QVERIFY(cudaStreamSynchronize(stream.value) == cudaSuccess);

    uct_batch_test::Environment profiling("OKUFLOW_UCT_TRT_PROFILE", profile ? "1" : "0");
    ProfileCapture capture;
    uct_batch_test::RetirementReceipts retirement;
    const auto mailbox = std::make_shared<Mailbox>();
    auto pool = std::make_unique<AnalysisPool>();
    const auto constructed = Clock::now();
    auto scheduler = std::make_unique<RecognitionScheduler>(
        CreateProductionTensorRtRecognizer(*detector, *recognizerTier, retirement.Observer(), cache),
        [mailbox](RecognitionResult result) {
            const auto at = Clock::now();
            {
                std::lock_guard lock(mailbox->mutex);
                mailbox->completed = Completion{std::move(result), mailbox->backend, at};
            }
            mailbox->ready.notify_all();
        },
        [mailbox](BackendStatus status) {
            std::lock_guard lock(mailbox->mutex);
            mailbox->backend = std::move(status);
        },
        2, ModelTuning{}.minimumWordHeightPx, false, false);

    std::uint64_t frame = 1;
    struct Read {
        std::optional<Completion> completion;
        MeteredImage::Meters meters;
        double submitToCallbackMs{};
        qint64 submittedUnixMs{}; // wall clock, to align reads with external GPU-state samples
        QVector<QJsonObject> profiles;
        QString error;
    };
    const auto read = [&](int sceneIndex) {
        Read result;
        const auto& scene = scenes[std::size_t(sceneIndex)];
        const int w = scene.image.width(), h = scene.image.height();
        SharedDeviceImage owned;
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (!owned) {
            owned = pool->Capture(scene.device, std::size_t(w) * 4, w, h, stream.value,
                                  std::uint64_t(sceneIndex) + 1, frame, 1);
            if (owned) break;
            if (pool->HasFailure() || Clock::now() >= deadline) {
                result.error = "The analysis pool could not supply an owned capture.";
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (cudaStreamSynchronize(stream.value) != cudaSuccess) {
            result.error = "The analysis copy did not complete.";
            return result;
        }
        const auto metered = std::make_shared<MeteredImage>(std::move(owned));
        SourceStamp stamp;
        stamp.sourceEpoch = metered->sourceEpoch;
        stamp.geometryGeneration = 1;
        stamp.frameSeq = frame++;
        stamp.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        stamp.viewportWidth = std::uint32_t(w);
        stamp.viewportHeight = std::uint32_t(h);
        stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
        stamp.liveCameraCapture = !media;
        capture.Take();
        result.submittedUnixMs = QDateTime::currentMSecsSinceEpoch();
        const auto submitted = Clock::now();
        if (!scheduler->SubmitDevice(stamp, metered)) {
            result.error = "Submission refused: " + scheduler->Status().detail;
            return result;
        }
        result.completion = Await(*mailbox, submitted + std::chrono::seconds(60));
        if (!result.completion) {
            result.error = "A read exceeded 60 seconds.";
            return result;
        }
        result.submitToCallbackMs = Ms(result.completion->at - submitted);
        result.meters = metered->Read();
        result.profiles = capture.Take();
        return result;
    };

    // Startup: cached plan load plus the first inference. Never a timed read.
    std::optional<Read> first;
    int preparingPolls = 0;
    while (!first && Ms(Clock::now() - constructed) < 120000) {
        auto attempt = read(0);
        QVERIFY2(attempt.completion, qPrintable(attempt.error));
        if (attempt.completion->result.output.backendPreparing && !attempt.completion->result.output.ok) {
            ++preparingPolls;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else first = std::move(attempt);
    }
    QVERIFY2(first, "Cached GPU engines were not ready within 120 seconds; prepare them first.");
    const double startupMs = Ms(first->completion->at - constructed);
    QVERIFY2(first->completion->result.output.ok, qPrintable(first->completion->result.output.error));
    const auto backend = first->completion->backend;
    QVERIFY2(backend.usesGpu, qPrintable(backend.detail));

    QJsonArray sceneRecords;
    for (int index = 0; index < int(scenes.size()); ++index) {
        QJsonArray reads;
        QJsonObject firstOutput;
        QString firstSha;
        bool identical = true;
        for (int pass = 0; pass <= passes; ++pass) { // pass 0 is this scene's first read in this process
            if (gapMs) std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
            auto attempt = read(index);
            QVERIFY2(attempt.completion, qPrintable(attempt.error));
            const auto& result = attempt.completion->result;
            QVERIFY2(result.output.ok, qPrintable(scenes[std::size_t(index)].path + ": " + result.output.error));
            QVERIFY2(result.deviceInput && attempt.meters.egress == 0 && attempt.meters.residentCalls == 1,
                     "A read left the resident GPU path.");
            const auto sha = Hex(QCryptographicHash::hash(ExactOutputBytes(result.output), QCryptographicHash::Sha256));
            if (pass == 0) {
                firstSha = sha;
                firstOutput = OutputJson(result.output);
                firstOutput.remove("exactOrderedOutputV1");
            } else identical = identical && sha == firstSha;
            int words = 0, candidates = 0;
            for (const auto& line : result.output.lines)
                for (const auto& word : line.words) {
                    ++words;
                    candidates += word.disposition == TokenDisposition::kCandidate;
                }
            QJsonObject record{{"pass", pass}, {"workerMs", result.durationMs},
                {"submitToCallbackMs", attempt.submitToCallbackMs}, {"submittedUnixMs", attempt.submittedUnixMs},
                {"residentMs", attempt.meters.residentMs},
                {"inputWaitMs", attempt.meters.waitMs}, {"outputSha256", sha}, {"words", words}, {"candidates", candidates}};
            if (!attempt.profiles.isEmpty()) {
                QJsonArray lines;
                for (const auto& line : attempt.profiles) lines.push_back(line);
                record["profile"] = lines;
            }
            reads.push_back(record);
        }
        const auto& scene = scenes[std::size_t(index)];
        sceneRecords.push_back(QJsonObject{{"path", scene.path}, {"sha256", scene.sha256},
            {"width", scene.image.width()}, {"height", scene.image.height()}, {"reads", reads},
            {"repeatedOutputsIdentical", identical}, {"firstOutputSha256", firstSha}, {"firstOutput", firstOutput}});
    }

    scheduler->Cancel();
    scheduler.reset();
    const auto closed = retirement.Wait();
    QJsonObject switches;
    const auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& key : environment.keys())
        if (key.startsWith("OKUFLOW_UCT_") && !key.startsWith("OKUFLOW_UCT_CORPUS_")) switches[key] = environment.value(key);
    QFile executable(QCoreApplication::applicationFilePath());
    QVERIFY(executable.open(QIODevice::ReadOnly));
    QCryptographicHash executableHash(QCryptographicHash::Sha256);
    QVERIFY(executableHash.addData(&executable));
    const QJsonObject report{{"schema", 1}, {"scenes", sceneRecords}, {"passes", passes}, {"gapMs", gapMs},
        {"stamp", media ? "media" : "camera"}, {"pair", tiers.join(QLatin1Char(':'))}, {"profile", profile},
        {"startupFirstResultMs", startupMs}, {"startupPreparingPolls", preparingPolls},
        {"backendDetail", backend.detail}, {"switches", switches},
        {"executableSha256", Hex(executableHash.result())},
        {"gpu", QString::fromLatin1(properties.name)}, {"cudaDriverApi", driver},
        {"retirement", closed.Json()}, {"profileLinesMalformed", capture.Malformed()},
        {"scope", "Scheduler worker time on file fixtures: no camera, decoder, presenter, paint or accessibility."}};
    QVERIFY(uct_batch_test::Save(evidence, report));
    QVERIFY(closed.Passed());
}
#else
inline void CorpusTiming()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_CORPUS_TIMING") != "1")
        QSKIP("Explicit corpus timing not selected.");
    QFAIL("Corpus timing was selected, but this build lacks the CUDA analysis pool or TensorRT adapter.");
}
#endif
} // namespace okuflow::uct::uct_corpus_test
