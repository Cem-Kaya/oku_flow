#pragma once

// Explicit private-source diagnostic. Reuses the production resident scheduler,
// exact selected/neighbour evidence and immutable admission. No CTest/model
// download, native window or app settings are involved.
#include "uct_resident_diagnostic.hpp"

namespace uct_paused_video_test {
inline void Run()
{
    using namespace okuflow::uct;
    if (qEnvironmentVariable("OKUFLOW_UCT_PAUSE_DIAGNOSTIC") != "1")
        QSKIP("Explicit paused-video evidence diagnostic not selected.");
#if !(OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT)
    QFAIL("The selected diagnostic requires the CUDA/TensorRT fixture.");
#else
    using namespace uct_resident_test;
    const auto selectedPath = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_SELECTED");
    const auto neighbourPath = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_NEIGHBOUR");
    const auto sdk = qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT");
    bool selectedPtsOk{}, neighbourPtsOk{};
    const auto selectedPts = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_SELECTED_MS").toLongLong(&selectedPtsOk);
    const auto neighbourPts = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_NEIGHBOUR_MS").toLongLong(&neighbourPtsOk);
    QVERIFY(selectedPtsOk && neighbourPtsOk && selectedPts >= 0 && neighbourPts >= 0);
    QVERIFY(std::abs(double(selectedPts) - neighbourPts) <= 250 && selectedPts != neighbourPts);
    const QDir bin(QDir(sdk).filePath("bin"));
    QVERIFY(QFile::exists(bin.filePath("nvinfer_11.dll")));
    QVERIFY(!QFile::exists(bin.filePath("nvonnxparser_11.dll")) &&
            bin.entryList({"nvinfer_builder_resource*.dll"}, QDir::Files).isEmpty());
    const QImage selectedImage = QImage(selectedPath).convertToFormat(QImage::Format_ARGB32);
    const QImage neighbourImage = QImage(neighbourPath).convertToFormat(QImage::Format_ARGB32);
    QVERIFY(!selectedImage.isNull() && selectedImage.size() == neighbourImage.size());
    QVERIFY(selectedImage.width() <= 8192 && selectedImage.height() <= 8192 &&
            selectedImage.sizeInBytes() <= 64ll * 1024 * 1024);
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
    QVERIFY2(detector.valid, qPrintable(detector.error));
    for (const int capacity : {1, 8}) {
        const auto plan = InspectPlan(cache, "recognizer", ModelTier::Medium, capacity,
                                      PlanKey(assets.recognizerSha256, uuid, driver, false, capacity));
        QVERIFY2(plan.valid, qPrintable(plan.error));
    }
    const auto cacheBefore = CacheInventory(cache);
    const auto evidence = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(
        "OkuFlow/uct-pause-proof-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") +
        "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(QDir().mkpath(evidence));
    qInfo().noquote() << "Paused resident proof evidence=" << evidence;
    DeviceStream stream;
    QVERIFY(stream.value);
    DeviceUploads uploads;
    uploads.stream = stream.value;
    AnalysisPool pool;
    const auto upload = [&](const QImage& image, std::uint64_t frame) -> std::shared_ptr<MeteredImage> {
        auto* pixels = uploads.Allocate(std::size_t(image.sizeInBytes()));
        if (!pixels || cudaMemcpyAsync(pixels, image.constBits(), std::size_t(image.sizeInBytes()),
                                      cudaMemcpyHostToDevice, stream.value) != cudaSuccess) return {};
        auto owned = pool.Capture(pixels, std::size_t(image.bytesPerLine()), image.width(), image.height(),
                                  stream.value, 1, frame, 1);
        if (!owned || cudaStreamSynchronize(stream.value) != cudaSuccess) return {};
        return std::make_shared<MeteredImage>(std::move(owned));
    };
    // Selected source is a 60 Hz recording. These identify the extracted real
    // acquisitions, and retries retain them; job IDs never become evidence.
    const auto sequence = [](qint64 pts) { return std::uint64_t(pts * 60 / 1000 + 1); };
    const auto selectedInput = upload(selectedImage, sequence(selectedPts));
    const auto neighbourInput = upload(neighbourImage, sequence(neighbourPts));
    QVERIFY(selectedInput && neighbourInput);
    const auto stamp = [&](const DeviceImage& image, qint64 pts) {
        SourceStamp s;
        s.sourceEpoch = 1; s.geometryGeneration = 1; s.frameSeq = image.frameSequence; s.timestampMs = pts;
        s.viewportWidth = image.width; s.viewportHeight = image.height;
        s.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
        return s;
    };
    const auto selectedStamp = stamp(*selectedInput, selectedPts);
    const auto neighbourStamp = stamp(*neighbourInput, neighbourPts);
    uct_batch_test::Environment batch("OKUFLOW_UCT_TRT_BATCH", "8");
    uct_batch_test::RetirementReceipts retirement;
    const auto box = std::make_shared<Mailbox>();
    auto scheduler = std::make_unique<RecognitionScheduler>(
        CreateTensorRtRecognizer(assets.root, sdk, ModelTier::Medium, ModelTier::Medium, retirement.Observer()),
        [box](RecognitionResult result) {
            { std::lock_guard lock(box->mutex); box->completed = Completion{std::move(result), box->backend, Clock::now()}; }
            box->ready.notify_all();
        }, [box](BackendStatus status) { std::lock_guard lock(box->mutex); box->backend = std::move(status); });
    std::optional<Completion> selected;
    const auto startupDeadline = Clock::now() + std::chrono::seconds(60);
    while (!selected && Clock::now() < startupDeadline) {
        QVERIFY(scheduler->SubmitDevice(selectedStamp, selectedInput));
        auto completion = Await(*box, startupDeadline);
        QVERIFY(completion);
        if (completion->result.output.backendPreparing && !completion->result.output.ok)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        else selected = std::move(completion);
    }
    QVERIFY(selected && selected->backend.usesGpu);
    QVERIFY2(selected->result.output.ok, qPrintable(selected->result.output.error));
    QVERIFY(selected->result.reference.resident && selected->result.sourcePixelDigest.size() == 32);
    OverlayModel model;
    const auto beforeVerdict = model.AdmitImmutable(selected->result, selectedStamp, 0);
    CorroborationRequest request;
    request.selectedJobId = selected->result.jobId;
    request.selectedReference = selected->result.reference;
    request.selectedLines = selected->result.prepared;
    request.tuning = ModelTuning{}.match;
    for (const auto& line : request.selectedLines) request.references.push_back(line.reference);
    QVERIFY(scheduler->SubmitDevice(neighbourStamp, neighbourInput, request));
    auto neighbour = Await(*box, Clock::now() + std::chrono::seconds(30));
    QVERIFY(neighbour && neighbour->backend.usesGpu);
    QVERIFY2(neighbour->result.output.ok, qPrintable(neighbour->result.output.error));
    const auto afterVerdict = model.AdmitImmutable(selected->result, selectedStamp, 0, &neighbour->result);
    const auto shown = model.SceneWords(0, selectedStamp.frameSeq);
    QJsonArray words;
    for (int i = 0; i < selected->result.prepared.size(); ++i) {
        const auto& line = selected->result.prepared[i];
        for (int j = 0; j < line.words.size(); ++j) {
            const auto& word = line.words[j];
            const bool lineProof = i < neighbour->result.corroborationMatches.size() &&
                neighbour->result.corroborationMatches[i];
            const bool wordProof = i < neighbour->result.corroborationWordMatches.size() &&
                j < neighbour->result.corroborationWordMatches[i].size() && neighbour->result.corroborationWordMatches[i][j];
            const bool drawn = std::any_of(shown.begin(), shown.end(), [&](const auto& s) {
                return s.text == word.text && s.sceneRect.intersects(word.sceneRect);
            });
            words.push_back(QJsonObject{{"text", word.text}, {"disposition", int(word.disposition)},
                {"line", i}, {"word", j}, {"referenceValid", line.reference.valid},
                {"lineProof", lineProof}, {"wordProof", wordProof}, {"glyphEligible", drawn}});
        }
    }
    const QJsonObject report{{"selectedPath", selectedPath}, {"neighbourPath", neighbourPath},
        {"selectedMs", selectedPts}, {"neighbourMs", neighbourPts},
        {"selectedSha256", FileSha256(selectedPath)}, {"neighbourSha256", FileSha256(neighbourPath)},
        {"beforeVerdict", int(beforeVerdict)}, {"afterVerdict", int(afterVerdict)},
        {"selectedOutput", uct_batch_test::Output(selected->result.output)},
        {"neighbourOutput", uct_batch_test::Output(neighbour->result.output)},
        {"selectedCandidates", CandidateWords(selected->result.output)}, {"shownWords", int(shown.size())},
        {"sourceDigestsDiffer", selected->result.sourcePixelDigest != neighbour->result.sourcePixelDigest},
        {"words", words}, {"readingText", model.ReadingText(0, selectedStamp.frameSeq)},
        {"imageEgress", selectedInput->Read().egress + neighbourInput->Read().egress},
        {"scope", "Actual resident recognition and immutable admission; no decoder, native display or latency benchmark."}};
    QVERIFY(uct_batch_test::Save(QDir(evidence).filePath("proof.json"), report));
    qInfo().noquote() << "Paused candidates=" << CandidateWords(selected->result.output)
                     << "shown=" << shown.size() << "egress=" << report["imageEgress"].toInt();
    QCOMPARE(selectedInput->Read().egress + neighbourInput->Read().egress, 0);
    QCOMPARE(model.SceneWords(3600000, selectedStamp.frameSeq).size(), shown.size());
    scheduler->Cancel();
    scheduler.reset();
    const auto& closed = retirement.Wait();
    QVERIFY2(closed.Passed(), "Exact TensorRT State did not close.");
    QVERIFY(CacheInventory(cache) == cacheBefore);
#endif
}
} // namespace uct_paused_video_test
