#pragma once

// Explicit finite GPU execution proof. Uses real extracted adjacent inputs,
// cached engines and the production scheduler; no native window or settings.
#include "uct_resident_diagnostic.hpp"
#include "okuflow/common/uct_region_planner.hpp"

namespace uct_active_regional_test {
inline void Run()
{
    using namespace okuflow::uct;
    if (qEnvironmentVariable("OKUFLOW_UCT_ACTIVE_DIAGNOSTIC") != "1")
        QSKIP("Explicit active regional GPU diagnostic not selected.");
#if !(OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT)
    QFAIL("The selected diagnostic requires CUDA/TensorRT.");
#else
    using namespace uct_resident_test;
    const auto firstPath = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_SELECTED");
    const auto nextPath = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_NEIGHBOUR");
    const auto sdk = qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT");
    const auto outputRoot = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_OUTPUT_ROOT");
    bool firstOk{}, nextOk{};
    const auto firstPts = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_SELECTED_MS").toLongLong(&firstOk);
    const auto nextPts = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_NEIGHBOUR_MS").toLongLong(&nextOk);
    QVERIFY(firstOk && nextOk && firstPts >= 0 && nextPts > firstPts && nextPts - firstPts <= 250);
    QVERIFY(nextPts <= std::numeric_limits<qint64>::max() / 60);
    QVERIFY(QDir::isAbsolutePath(firstPath) && QDir::isAbsolutePath(nextPath));
    QVERIFY(QDir::isAbsolutePath(outputRoot));
    const QDir bin(QDir(sdk).filePath("bin"));
    QVERIFY(QFile::exists(bin.filePath("nvinfer_11.dll")));
    QVERIFY(!QFile::exists(bin.filePath("nvonnxparser_11.dll")) &&
            bin.entryList({"nvinfer_builder_resource*.dll"}, QDir::Files).isEmpty());
    const QImage firstImage = QImage(firstPath).convertToFormat(QImage::Format_ARGB32);
    const QImage nextImage = QImage(nextPath).convertToFormat(QImage::Format_ARGB32);
    QVERIFY(!firstImage.isNull() && firstImage.size() == nextImage.size());
    QVERIFY(firstImage.width() <= 8192 && firstImage.height() <= 8192 &&
            firstImage.sizeInBytes() <= 64ll * 1024 * 1024);
    QVERIFY(FileSha256(firstPath) != FileSha256(nextPath));

    int devices{}, driver{};
    QVERIFY(cudaGetDeviceCount(&devices) == cudaSuccess && devices > 0);
    QVERIFY(cudaSetDevice(0) == cudaSuccess);
    cudaDeviceProp properties{};
    QVERIFY(cudaGetDeviceProperties(&properties, 0) == cudaSuccess);
    QVERIFY(cudaDriverGetVersion(&driver) == cudaSuccess);
    const QByteArray uuid(reinterpret_cast<const char*>(&properties.uuid), sizeof(properties.uuid));
    const auto assets = LoadPaddleAssets(DefaultPaddleRoot(), ModelTier::Medium, ModelTier::Medium);
    QVERIFY2(assets.IsValid(), qPrintable(assets.error));
    const auto cache = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/ocr-engines");
    const auto detector = InspectPlan(cache, "detector", ModelTier::Medium, 1,
                                     PlanKey(assets.detectorSha256, uuid, driver, true, 1));
    const auto recognizerPlan = InspectPlan(cache, "recognizer", ModelTier::Medium, 8,
                                           PlanKey(assets.recognizerSha256, uuid, driver, false, 8));
    QVERIFY2(detector.valid, qPrintable(detector.error));
    QVERIFY2(recognizerPlan.valid, qPrintable(recognizerPlan.error));
    const auto cacheBefore = CacheInventory(cache);
    const auto evidence = QDir(outputRoot).filePath("uct-active-proof-" +
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + "-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(QDir().mkpath(evidence));
    qInfo().noquote() << "Active regional GPU proof evidence=" << evidence;

    DeviceStream stream;
    QVERIFY(stream.value);
    DeviceUploads uploads;
    uploads.stream = stream.value;
    AnalysisPool pool;
    const auto upload = [&](const QImage& image, qint64 pts) -> std::shared_ptr<MeteredImage> {
        auto* pixels = uploads.Allocate(std::size_t(image.sizeInBytes()));
        if (!pixels || cudaMemcpyAsync(pixels, image.constBits(), std::size_t(image.sizeInBytes()),
                                      cudaMemcpyHostToDevice, stream.value) != cudaSuccess) return {};
        // These fixtures are extracted from 60 Hz recordings. Reusing the
        // second lease below retains this exact acquisition and pixel digest.
        auto owned = pool.Capture(pixels, std::size_t(image.bytesPerLine()), image.width(), image.height(),
                                  stream.value, 1, std::uint64_t(pts * 60 / 1000 + 1), 1);
        if (!owned || cudaStreamSynchronize(stream.value) != cudaSuccess) return {};
        return std::make_shared<MeteredImage>(std::move(owned));
    };
    const auto firstInput = upload(firstImage, firstPts);
    const auto nextInput = upload(nextImage, nextPts);
    QVERIFY(firstInput && nextInput && firstInput->frameSequence < nextInput->frameSequence);
    const auto stamp = [](const DeviceImage& image, qint64 pts) {
        SourceStamp value;
        value.sourceEpoch = 1; value.geometryGeneration = 1;
        value.frameSeq = image.frameSequence; value.timestampMs = pts;
        value.viewportWidth = image.width; value.viewportHeight = image.height;
        value.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
        return value;
    };
    const auto firstStamp = stamp(*firstInput, firstPts);
    const auto nextStamp = stamp(*nextInput, nextPts);
    uct_batch_test::Environment batch("OKUFLOW_UCT_TRT_BATCH", "8");
    uct_batch_test::Environment profiling("OKUFLOW_UCT_TRT_PROFILE", "1");
    ProfileCapture capture;
    uct_batch_test::RetirementReceipts retirement;
    const auto box = std::make_shared<Mailbox>();
    auto recognizer = CreateTensorRtRecognizer(assets.root, sdk, ModelTier::Medium,
                                               ModelTier::Medium, retirement.Observer());
    auto scheduler = std::make_unique<RecognitionScheduler>(std::move(recognizer),
        [box](RecognitionResult result) {
            { std::lock_guard lock(box->mutex); box->completed = Completion{std::move(result), box->backend, Clock::now()}; }
            box->ready.notify_all();
        }, [box](BackendStatus status) { std::lock_guard lock(box->mutex); box->backend = std::move(status); },
        2, 7.0, false, false, true);
    scheduler->SetShadowFocus(QRectF(.55, .28, .14, .12));
    std::optional<Completion> full;
    Clock::time_point fullSubmitted;
    const auto startupDeadline = Clock::now() + std::chrono::seconds(60);
    while (!full && Clock::now() < startupDeadline) {
        fullSubmitted = Clock::now();
        QVERIFY(scheduler->SubmitDevice(firstStamp, firstInput));
        auto completion = Await(*box, startupDeadline);
        QVERIFY(completion);
        if (completion->result.output.backendPreparing && !completion->result.output.ok)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        else full = std::move(completion);
    }
    QVERIFY(full && full->backend.usesGpu);
    QVERIFY2(full->result.output.ok, qPrintable(full->result.output.error));
    QVERIFY(full->result.reference.resident && full->result.sourcePixelDigest.size() == 32);
    QCOMPARE(full->result.jobKind, RecognitionJobKind::Full);
    QVERIFY(full->result.shadowPlan && full->result.shadowPlan->status == RegionPlanStatus::kAccepted);
    const auto baselineStats = scheduler->Stats();
    const auto fullProfile = capture.Take();
    const auto fullDeadline = fullSubmitted + std::chrono::milliseconds(1500);
    const auto slackMs = std::chrono::duration<double, std::milli>(fullDeadline - Clock::now()).count();
    QVERIFY2(slackMs > 830.0, "No safe 750 ms estimate plus 80 ms reserve remains; do not extend the full deadline.");
    const auto regionalJob = scheduler->SubmitDevice(nextStamp, nextInput, {}, std::nullopt,
        RegionalOcrRequest{full->result.shadowPlan, fullDeadline, 750.0});
    QVERIFY(regionalJob);
    auto regional = Await(*box, startupDeadline);
    QVERIFY(regional && regional->backend.usesGpu);
    QVERIFY2(regional->result.output.ok, qPrintable(regional->result.output.error));
    QCOMPARE(regional->result.jobKind, RecognitionJobKind::Regional);
    QVERIFY(regional->result.regionalRequested && !regional->result.regionalFallback &&
            !regional->result.regionalFailed && !regional->result.regionalDeadlineOverrun);
    const auto roi = regional->result.output.detailRegionPx;
    QVERIFY(!roi.isEmpty() && roi.width() <= 480 && roi.height() <= 480);
    QVERIFY(QRect(QPoint{}, nextImage.size()).contains(roi));
    QVERIFY(!regional->result.shadowPlan && regional->result.reference.resident);
    const auto regionalProfile = capture.Take();
    const auto regionalStats = scheduler->Stats();
    QCOMPARE(regionalStats.regionalCalls, baselineStats.regionalCalls + 1);
    QCOMPARE(regionalStats.fullCalls, baselineStats.fullCalls);

    // Submit an ordinary full read of the SAME second acquisition. This is
    // not a third temporal observation, and this scheduler-only diagnostic
    // does not admit any text or claim independent corroboration/UIA timing.
    QVERIFY(scheduler->SubmitDevice(nextStamp, nextInput));
    auto nextFull = Await(*box, startupDeadline);
    QVERIFY(nextFull && nextFull->backend.usesGpu);
    QVERIFY2(nextFull->result.output.ok, qPrintable(nextFull->result.output.error));
    QCOMPARE(nextFull->result.jobKind, RecognitionJobKind::Full);
    QVERIFY(nextFull->result.output.detailRegionPx.isEmpty());
    QCOMPARE(nextFull->result.stamp.frameSeq, regional->result.stamp.frameSeq);
    QCOMPARE(nextFull->result.sourcePixelDigest, regional->result.sourcePixelDigest);
    QVERIFY(nextFull->result.shadowPlan && nextFull->result.shadowPlan->status == RegionPlanStatus::kAccepted);
    QCOMPARE(nextFull->result.shadowPlan->coverage.acceptedObservations, std::uint64_t{2});
    const auto nextFullProfile = capture.Take();
    const auto finalStats = scheduler->Stats();
    QCOMPARE(finalStats.fullCalls, baselineStats.fullCalls + 1);
    QCOMPARE(finalStats.regionalCalls, baselineStats.regionalCalls + 1);
    const auto profiles = [](const QVector<QJsonObject>& values) {
        QJsonArray result; for (const auto& value : values) result.push_back(value); return result;
    };
    const QJsonObject report{{"scope", "Actual cached GPU full/region/full calls on two extracted acquisitions; no controller cadence, text admission, screen-reader or display latency claim."},
        {"firstPath", firstPath}, {"nextPath", nextPath},
        {"firstSha256", FileSha256(firstPath)}, {"nextSha256", FileSha256(nextPath)},
        {"firstPtsMs", firstPts}, {"nextPtsMs", nextPts}, {"slackBeforeRegionMs", slackMs},
        {"firstWorkerMs", full->result.durationMs}, {"regionalWorkerMs", regional->result.durationMs},
        {"nextFullWorkerMs", nextFull->result.durationMs}, {"regionalJob", qint64(regionalJob)},
        {"roi", QJsonArray{roi.x(), roi.y(), roi.width(), roi.height()}},
        {"firstOutput", uct_batch_test::Output(full->result.output)},
        {"regionalOutput", uct_batch_test::Output(regional->result.output)},
        {"nextFullOutput", uct_batch_test::Output(nextFull->result.output)},
        {"firstProfile", profiles(fullProfile)}, {"regionalProfile", profiles(regionalProfile)},
        {"nextFullProfile", profiles(nextFullProfile)}, {"profileMalformed", capture.Malformed()},
        {"firstCandidateWords", CandidateWords(full->result.output)},
        {"regionalCandidateWords", CandidateWords(regional->result.output)},
        {"nextFullCandidateWords", CandidateWords(nextFull->result.output)},
        {"fullCallsIncludingPreparing", qint64(finalStats.fullCalls)},
        {"regionalCalls", qint64(finalStats.regionalCalls)},
        {"regionalFallbacks", qint64(finalStats.regionalFallbacks)},
        {"regionalFailures", qint64(finalStats.regionalFailures)},
        {"regionalDeadlineOverruns", qint64(finalStats.regionalDeadlineOverruns)},
        {"imageEgress", firstInput->Read().egress + nextInput->Read().egress}};
    QVERIFY(uct_batch_test::Save(QDir(evidence).filePath("proof.json"), report));
    qInfo() << "Active regional worker ms" << full->result.durationMs << regional->result.durationMs
            << nextFull->result.durationMs << "ROI" << roi;
    QCOMPARE(capture.Malformed(), 0);
    QCOMPARE(firstInput->Read().egress + nextInput->Read().egress, 0);
    scheduler->Cancel();
    scheduler.reset();
    QVERIFY2(retirement.Wait().Passed(), "Exact diagnostic TensorRT State did not close.");
    QVERIFY(CacheInventory(cache) == cacheBefore);
#endif
}
} // namespace uct_active_regional_test
