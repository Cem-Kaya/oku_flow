#pragma once

// Explicit private-source diagnostic. Reuses the production resident scheduler,
// exact selected/neighbour evidence and immutable admission. No CTest/model
// download, native window or app settings are involved.
#include "uct_resident_diagnostic.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#if OKUFLOW_ENABLE_CUDA
#include "okuflow/cuda/uct_kernels.hpp"
#endif

namespace uct_paused_video_test {
// Explicit diagnostic bridge: complete one real Windows selected/neighbour
// pair before using the real resident recognizer, even with warm GPU plans.
class CpuBridgeThenResident final : public okuflow::uct::Recognizer {
public:
    explicit CpuBridgeThenResident(std::unique_ptr<okuflow::uct::Recognizer> preferred)
        : preferred_(std::move(preferred)), bridge_(okuflow::uct::CreateWindowsRecognizer()) {}
    okuflow::uct::BackendStatus Initialize() override {
        preferred_->Initialize();
        status_ = bridge_->Initialize();
        return status_;
    }
    okuflow::uct::BackendStatus Status() const override { return status_; }
    bool WantsDeviceInput() const override { return bridgeCalls_ >= 2; }
    okuflow::uct::RecognitionOutput Recognize(const QImage& image, const std::atomic<bool>& cancel) override {
        auto output = bridge_->Recognize(image, cancel);
        ++bridgeCalls_;
        status_ = bridge_->Status();
        output.backendPreparing = true;
        return output;
    }
    okuflow::uct::RecognitionOutput RecognizeDevice(const okuflow::uct::DeviceImage& image,
                                                   const std::atomic<bool>& cancel) override {
        auto output = preferred_->RecognizeDevice(image, cancel);
        status_ = preferred_->Status();
        return output;
    }
private:
    std::unique_ptr<okuflow::uct::Recognizer> preferred_, bridge_;
    okuflow::uct::BackendStatus status_;
    int bridgeCalls_{};
};

inline void Run()
{
    using namespace okuflow::uct;
    if (qEnvironmentVariable("OKUFLOW_UCT_PAUSE_DIAGNOSTIC") != "1")
        QSKIP("Explicit paused-video evidence diagnostic not selected.");
#if !(OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT)
    QFAIL("The selected diagnostic requires the CUDA/TensorRT fixture.");
#else
    using namespace uct_resident_test;
    const bool bridge = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_BRIDGE") == "1";
    const auto selectedPath = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_SELECTED");
    const auto neighbourPath = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_NEIGHBOUR");
    const auto sdk = qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT");
    bool selectedPtsOk{}, neighbourPtsOk{};
    const auto selectedPts = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_SELECTED_MS").toLongLong(&selectedPtsOk);
    const auto neighbourPts = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_NEIGHBOUR_MS").toLongLong(&neighbourPtsOk);
    QVERIFY(selectedPtsOk && neighbourPtsOk && selectedPts >= 0 && neighbourPts >= 0);
    QVERIFY(selectedPts <= std::numeric_limits<qint64>::max() / 60 &&
            neighbourPts <= std::numeric_limits<qint64>::max() / 60);
    QVERIFY(std::abs(selectedPts - neighbourPts) <= 250 && selectedPts != neighbourPts);
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
    const auto outputRoot=qEnvironmentVariable("OKUFLOW_UCT_PAUSE_OUTPUT_ROOT");
    QVERIFY(outputRoot.isEmpty() || QDir::isAbsolutePath(outputRoot));
    const auto evidence = QDir(outputRoot.isEmpty() ?
        QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow") : outputRoot).filePath(
        "uct-pause-proof-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") +
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
    auto recognizer = CreateTensorRtRecognizer(assets.root, sdk, ModelTier::Medium,
                                               ModelTier::Medium, retirement.Observer());
    if (bridge) recognizer = std::make_unique<CpuBridgeThenResident>(std::move(recognizer));
    auto scheduler = std::make_unique<RecognitionScheduler>(std::move(recognizer),
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
    QVERIFY(selected && (bridge ? !selected->backend.usesGpu : selected->backend.usesGpu));
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
    QVERIFY(neighbour && (bridge ? !neighbour->backend.usesGpu : neighbour->backend.usesGpu));
    QVERIFY2(neighbour->result.output.ok, qPrintable(neighbour->result.output.error));
    auto afterVerdict = model.AdmitImmutable(selected->result, selectedStamp, 0, &neighbour->result);
    QJsonObject bridgeReport;
    if (bridge) {
        const auto initial = selected->result;
        const auto oldWords = model.SceneWords(0, selectedStamp.frameSeq);
        bridgeReport = {{"selectedOutput", uct_batch_test::Output(initial.output)},
                        {"neighbourOutput", uct_batch_test::Output(neighbour->result.output)},
                        {"shownWords", int(oldWords.size())},
                        {"digestKind", int(initial.sourcePixelDigestKind)}};
        selected.reset();
        const auto preferredDeadline = Clock::now() + std::chrono::seconds(60);
        while (!selected && Clock::now() < preferredDeadline) {
            QVERIFY(scheduler->SubmitDevice(selectedStamp, selectedInput));
            auto completion = Await(*box, preferredDeadline);
            QVERIFY(completion);
            if (completion->result.output.backendPreparing && !completion->result.output.ok)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            else selected = std::move(completion);
        }
        QVERIFY(selected && selected->backend.usesGpu && selected->result.output.ok);
        QVERIFY(selected->result.reference.resident);
        QCOMPARE(selected->result.sourcePixelDigestKind, initial.sourcePixelDigestKind);
        QCOMPARE(selected->result.sourcePixelDigest, initial.sourcePixelDigest);
        request = {};
        request.selectedJobId = selected->result.jobId;
        request.selectedReference = selected->result.reference;
        request.selectedLines = selected->result.prepared;
        request.tuning = ModelTuning{}.match;
        for (const auto& line : request.selectedLines) request.references.push_back(line.reference);
        QVERIFY(scheduler->SubmitDevice(neighbourStamp, neighbourInput, request));
        neighbour = Await(*box, Clock::now() + std::chrono::seconds(30));
        QVERIFY(neighbour && neighbour->backend.usesGpu && neighbour->result.output.ok);
        afterVerdict = model.AdmitImmutable(selected->result, selectedStamp, 0, &neighbour->result);
        const auto refined = model.SceneWords(0, selectedStamp.frameSeq);
        QJsonArray retained, revoked;
        for (const auto& old : oldWords) {
            const bool kept = std::any_of(refined.begin(), refined.end(), [&](const auto& word) {
                return word.lineId == old.lineId && word.wordIndex == old.wordIndex &&
                       word.text == old.text && word.sceneRect == old.sceneRect;
            });
            QJsonObject item{{"text", old.text}, {"lineId", qint64(old.lineId)},
                             {"wordIndex", old.wordIndex},
                             {"rect", QJsonArray{old.sceneRect.x(), old.sceneRect.y(),
                                                 old.sceneRect.width(), old.sceneRect.height()}}};
            (kept ? retained : revoked).push_back(item);
        }
        bridgeReport.insert("retainedWords", retained);
        bridgeReport.insert("revokedWords", revoked);
    }
    const auto shown = model.SceneWords(0, selectedStamp.frameSeq);
    // Admission is not paint coverage. Exercise the actual glyph layout and
    // protected-ink closure too, on the desktop-free platform only.
    QVERIFY(QGuiApplication::platformName() == QStringLiteral("offscreen"));
    uct_test::RegisterFixtureFonts();
    QVERIFY(QFontDatabase::families().contains(QStringLiteral("Segoe UI")));
    QWidget viewport;
    viewport.resize(selectedImage.size());
    okuflow::UltraClearTextOverlay overlay(&viewport, &viewport);
    viewport.show();
    overlay.SetViewTransform(selectedStamp.viewTransform);
    QVector<okuflow::UltraClearTextOverlay::SceneWord> overlayWords;
    for (const auto& word : shown)
        overlayWords.push_back({word.text, word.sceneRect, word.colors, word.lineId,
                               word.protectedSceneRects, word.wordIndex, word.sceneQuad, word.layoutGroupId});
    overlay.SetWords(std::move(overlayWords));
    QTRY_VERIFY(overlay.PaintedGeometryCurrent());
    QJsonArray painted;
    int paintedCount = 0;
    for (const auto& word : shown) {
        okuflow::UltraClearTextOverlay::PaintedWordGeometry geometry;
        const bool drawn = overlay.PaintedWord(word.lineId, word.wordIndex, word.text, geometry) &&
                           geometry.drawn;
        paintedCount += drawn;
        painted.push_back(QJsonObject{{"text", word.text}, {"drawn", drawn},
            {"sceneRect", QJsonArray{word.sceneRect.x(), word.sceneRect.y(),
                                     word.sceneRect.width(), word.sceneRect.height()}}});
    }
    QImage composite = selectedImage;
    QPainter painter(&composite);
    overlay.CaptureSnapshot(selectedStamp.viewTransform, composite.size()).Paint(painter, composite.size());
    painter.end();
    QVERIFY(composite.save(QDir(evidence).filePath("overlay.png")));

    // Optional visible detail: one bounded region of the SAME two owned
    // images, as the paused controller requests after a zoom settles. The
    // selected half keeps its stamp and digest; only the real neighbour can
    // supply independent proof for anything the region newly finds.
    QJsonObject detailReport;
    const auto focusText = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_FOCUS");
    if (!focusText.isEmpty()) {
        const auto parts = focusText.split(',');
        QVERIFY2(parts.size() == 4, "OKUFLOW_UCT_PAUSE_FOCUS is x,y,w,h in normalized scene units.");
        const QRectF focusScene(parts[0].toDouble(), parts[1].toDouble(), parts[2].toDouble(), parts[3].toDouble());
        QVERIFY(!focusScene.isEmpty() && QRectF(0, 0, 1, 1).contains(focusScene));
        bool magnificationOk{};
        double magnification = qEnvironmentVariable("OKUFLOW_UCT_PAUSE_DETAIL_MAGNIFICATION").toDouble(&magnificationOk);
        if (!magnificationOk) magnification = kMaximumDetailMagnification;
        const QSize sceneSize = selectedImage.size();
        const DetailFocus focus{QRectF(focusScene.x() * sceneSize.width(), focusScene.y() * sceneSize.height(),
                                       focusScene.width() * sceneSize.width(), focusScene.height() * sceneSize.height())
                                    .toAlignedRect(), magnification};
        const okuflow::ViewTransform zoomed{float(focusScene.x()), float(focusScene.y()), float(focusScene.width()),
                                   float(focusScene.height()), 0, 0, 1, 1, true};
        const double zoomScale = 1600.0 / (focusScene.width() * sceneSize.width());
        const QSize viewSize(1600, int(std::lround(focusScene.height() * sceneSize.height() * zoomScale)));
        const auto renderZoom = [&](const OverlayModel& source, const QString& name) {
            QWidget zoomViewport;
            zoomViewport.resize(viewSize);
            okuflow::UltraClearTextOverlay zoomOverlay(&zoomViewport, &zoomViewport);
            zoomViewport.show();
            zoomOverlay.SetViewTransform(zoomed);
            QVector<okuflow::UltraClearTextOverlay::SceneWord> sceneWords;
            const auto words = source.SceneWords(0, selectedStamp.frameSeq);
            for (const auto& word : words)
                sceneWords.push_back({word.text, word.sceneRect, word.colors, word.lineId,
                                      word.protectedSceneRects, word.wordIndex, word.sceneQuad, word.layoutGroupId});
            zoomOverlay.SetWords(std::move(sceneWords));
            QJsonArray visibleWords;
            int inView = 0, drawn = 0;
            if (!words.isEmpty()) {
                [&] { QTRY_VERIFY(zoomOverlay.PaintedGeometryCurrent()); }();
                for (const auto& word : words) {
                    if (!word.sceneRect.intersects(focusScene)) continue;
                    ++inView;
                    okuflow::UltraClearTextOverlay::PaintedWordGeometry geometry;
                    const bool painted = zoomOverlay.PaintedWord(word.lineId, word.wordIndex, word.text, geometry) &&
                                         geometry.drawn;
                    drawn += painted;
                    visibleWords.push_back(QJsonObject{{"text", word.text}, {"drawn", painted},
                                                       {"rotated", word.sceneQuad.valid}});
                }
            }
            QImage view = selectedImage.copy(QRectF(focusScene.x() * sceneSize.width(), focusScene.y() * sceneSize.height(),
                                                    focusScene.width() * sceneSize.width(),
                                                    focusScene.height() * sceneSize.height()).toAlignedRect())
                              .scaled(viewSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_ARGB32);
            QPainter zoomPainter(&view);
            zoomOverlay.CaptureSnapshot(zoomed, view.size()).Paint(zoomPainter, view.size());
            zoomPainter.end();
            const bool saved = view.save(QDir(evidence).filePath(name));
            return QJsonObject{{"wordsInView", inView}, {"paintedInView", drawn}, {"words", visibleWords},
                               {"image", name}, {"saved", saved}, {"reading", source.ReadingText(0, selectedStamp.frameSeq)}};
        };
        const OverlayModel baseModel = model;
        const auto before = renderZoom(baseModel, "zoom-before.png");
        QVERIFY(scheduler->SubmitDevice(selectedStamp, selectedInput, {}, focus));
        auto detailSelected = Await(*box, Clock::now() + std::chrono::seconds(30));
        QVERIFY(detailSelected);
        QVERIFY2(detailSelected->result.output.ok, qPrintable(detailSelected->result.output.error));
        QVERIFY(!detailSelected->result.output.detailRegionPx.isEmpty());
        // Re-detecting the held image is the same acquisition, never a new one.
        QCOMPARE(detailSelected->result.stamp.frameSeq, selectedStamp.frameSeq);
        QCOMPARE(detailSelected->result.sourcePixelDigest, selected->result.sourcePixelDigest);
        const auto selectedOnly = model.SceneWords(0, selectedStamp.frameSeq).isEmpty()
            ? model.AdmitImmutable(detailSelected->result, selectedStamp, 0) : AdmitVerdict::kPending;
        CorroborationRequest detailRequest;
        detailRequest.selectedJobId = detailSelected->result.jobId;
        detailRequest.selectedReference = detailSelected->result.reference;
        detailRequest.selectedLines = detailSelected->result.prepared;
        detailRequest.tuning = ModelTuning{}.match;
        for (const auto& line : detailRequest.selectedLines) detailRequest.references.push_back(line.reference);
        QVERIFY(scheduler->SubmitDevice(neighbourStamp, neighbourInput, detailRequest, focus));
        auto detailNeighbour = Await(*box, Clock::now() + std::chrono::seconds(30));
        QVERIFY(detailNeighbour && detailNeighbour->result.output.ok);
        const auto detailVerdict = model.AdmitImmutable(detailSelected->result, selectedStamp, 0, &detailNeighbour->result);
        const auto after = renderZoom(model, "zoom-after.png");
        detailReport = {{"focusScene", QJsonArray{focusScene.x(), focusScene.y(), focusScene.width(), focusScene.height()}},
                        {"requestedRegion", QJsonArray{focus.regionPx.x(), focus.regionPx.y(), focus.regionPx.width(), focus.regionPx.height()}},
                        {"requestedMagnification", magnification},
                        {"effectiveMagnification", double(DetectorInputSize(detailSelected->result.output.detailRegionPx.size(),
                                                                             magnification).width()) /
                                                       detailSelected->result.output.detailRegionPx.width()},
                        {"establishedScale", EstablishedDetectorScale(sceneSize, QRectF(focus.regionPx).center())},
                        {"selectedOnlyVerdict", int(selectedOnly)}, {"pairVerdict", int(detailVerdict)},
                        {"selectedOutput", uct_batch_test::Output(detailSelected->result.output)},
                        {"selectedCandidates", CandidateWords(detailSelected->result.output)},
                        {"neighbourCandidates", CandidateWords(detailNeighbour->result.output)},
                        {"selectedWorkerMs", detailSelected->result.durationMs},
                        {"neighbourWorkerMs", detailNeighbour->result.durationMs},
                        {"before", before}, {"after", after}};
        qInfo().noquote() << "Detail magnification=" << magnification << "region=" << detailSelected->result.output.detailRegionPx
                         << "candidates=" << CandidateWords(detailSelected->result.output)
                         << "inView before/after=" << before["paintedInView"].toInt() << "/" << after["paintedInView"].toInt()
                         << "of" << before["wordsInView"].toInt() << "/" << after["wordsInView"].toInt()
                         << "ms=" << detailSelected->result.durationMs << "+" << detailNeighbour->result.durationMs;
    }
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
                return s.text == word.text && s.sceneRect == word.sceneRect;
            });
            words.push_back(QJsonObject{{"text", word.text}, {"disposition", int(word.disposition)},
                {"line", i}, {"word", j}, {"referenceValid", line.reference.valid},
                {"lineProof", lineProof}, {"wordProof", wordProof}, {"glyphEligible", drawn}});
        }
    }
    // Diagnostic-only replay of retained search frames/raw colors. Never
    // downloads pixels or contributes to the worker's measured duration.
    const auto fitReplay = [&](const SharedDeviceImage& input, const RecognitionOutput& output) {
        QVector<gpu::OrientedInk> frames; QVector<gpu::Colors> colors; QVector<int> indices;
        QJsonArray words;
        for (const auto& line : output.lines) for (const auto& word : line.words) {
            if (!word.paddedBounds || word.disposition != TokenDisposition::kCandidate ||
                !QuadIsRotated(word.quadPx) || !word.colors) continue;
            const auto& q = word.inkSearchQuadPx.valid ? word.inkSearchQuadPx : word.quadPx;
            const double w=QuadWidth(q),h=QuadHeight(q); const auto origin=q.points[0];
            const auto u=(q.points[1]-origin)/w,v=(q.points[3]-origin)/h;
            const auto warp=q.points[2]-q.points[1]-q.points[3]+origin;
            bool marks=word.text.contains('i',Qt::CaseInsensitive)||word.text.contains('j',Qt::CaseInsensitive);
            for(const auto c:word.text.normalized(QString::NormalizationForm_D))marks=marks||c.isMark()||QStringLiteral("!?:;").contains(c);
            frames.push_back({origin.x(),origin.y(),u.x(),u.y(),v.x(),v.y(),w,h,QuadWidth(word.quadPx),int(marks),warp.x(),warp.y()});
            const auto& fg=word.colors->foreground;const auto& bg=word.colors->background;
            colors.push_back({{fg.red(),fg.green(),fg.blue()},{bg.red(),bg.green(),bg.blue()}});
            indices.push_back(indices.size());
            words.push_back(QJsonObject{{"text",word.text},{"productionFitted",word.inkFitted}});
        }
        if(frames.isEmpty())return QJsonObject{{"words",words}};
        auto* df=static_cast<gpu::OrientedInk*>(uploads.Allocate(frames.size()*sizeof(gpu::OrientedInk)));
        auto* dc=static_cast<gpu::Colors*>(uploads.Allocate(colors.size()*sizeof(gpu::Colors)));
        auto* di=static_cast<int*>(uploads.Allocate(indices.size()*sizeof(int)));
        auto* dr=static_cast<gpu::OrientedFit*>(uploads.Allocate(frames.size()*sizeof(gpu::OrientedFit)));
        QString error;
        if(!df||!dc||!di||!dr||!input->WaitReady(stream.value,error))return QJsonObject{{"error","Fit replay input/allocation failed."}};
        if(cudaMemcpyAsync(df,frames.constData(),frames.size()*sizeof(gpu::OrientedInk),cudaMemcpyHostToDevice,stream.value)!=cudaSuccess||
           cudaMemcpyAsync(dc,colors.constData(),colors.size()*sizeof(gpu::Colors),cudaMemcpyHostToDevice,stream.value)!=cudaSuccess||
           cudaMemcpyAsync(di,indices.constData(),indices.size()*sizeof(int),cudaMemcpyHostToDevice,stream.value)!=cudaSuccess)
            return QJsonObject{{"error","Fit replay descriptor upload failed."}};
        struct Events {
            cudaEvent_t begin{},end{};
            ~Events(){if(begin)cudaEventDestroy(begin);if(end)cudaEventDestroy(end);}
        } events;
        if(cudaEventCreate(&events.begin)!=cudaSuccess||cudaEventCreate(&events.end)!=cudaSuccess)
            return QJsonObject{{"error","Fit replay event creation failed."}};
        QJsonArray kernelMs;QVector<gpu::OrientedFit> fits(frames.size());
        for(int pass=0;pass<4;++pass) {
            if(cudaEventRecord(events.begin,stream.value)!=cudaSuccess)return QJsonObject{{"error","Fit replay timing failed."}};
            gpu::OrientedInkBounds(input->Data(),input->pitch,input->width,input->height,df,di,frames.size(),dc,dr,stream.value);
            if(cudaEventRecord(events.end,stream.value)!=cudaSuccess||
               cudaMemcpyAsync(fits.data(),dr,fits.size()*sizeof(gpu::OrientedFit),cudaMemcpyDeviceToHost,stream.value)!=cudaSuccess||
               cudaStreamSynchronize(stream.value)!=cudaSuccess)return QJsonObject{{"error","Fit replay completion failed."}};
            float ms{};if(cudaEventElapsedTime(&ms,events.begin,events.end)!=cudaSuccess)return QJsonObject{{"error","Fit replay elapsed timing failed."}};
            if(pass)kernelMs.push_back(ms);
        }
        QJsonObject histogram;int complete=0,fitted=0;
        for(int i=0;i<fits.size();++i) {
            const auto& fit=fits[i];auto item=words[i].toObject();
            const auto reason=[&] {
                switch(fit.reason) {
                case gpu::OrientedFitReason::InvalidInput:return "invalid-input";
                case gpu::OrientedFitReason::Incomplete:return "propagation-cap";
                case gpu::OrientedFitReason::UnanchoredInk:return "unanchored-ink";
                case gpu::OrientedFitReason::TooFewSamples:return "too-few-samples";
                case gpu::OrientedFitReason::AmbiguousBands:return "ambiguous-bands";
                case gpu::OrientedFitReason::ShrinkGuard:return "shrink-guard";
                case gpu::OrientedFitReason::Fitted:return "fitted";
                }return "unknown";
            };
            item.insert("replayReason",QString::fromLatin1(reason()));
            item.insert("passes",fit.propagationPasses);item.insert("complete",bool(fit.propagationComplete));item.insert("replayFitted",bool(fit.fitted));words[i]=item;
            const auto key=QString::number(fit.propagationPasses);histogram[key]=histogram.value(key).toInt()+1;
            complete+=bool(fit.propagationComplete);fitted+=bool(fit.fitted);
        }
        return QJsonObject{{"words",words},{"complete",complete},{"incomplete",int(fits.size())-complete},
            {"replayFitted",fitted},{"passHistogram",histogram},{"kernelMs",kernelMs},
            {"scope","Diagnostic retained-search/color replay; one warmup, three CUDA event samples; outside actual worker duration."}};
    };
    const auto selectedFitReplay=fitReplay(selectedInput,selected->result.output);
    const auto neighbourFitReplay=fitReplay(neighbourInput,neighbour->result.output);
    QVERIFY2(!selectedFitReplay.contains("error"),qPrintable(selectedFitReplay.value("error").toString()));
    QVERIFY2(!neighbourFitReplay.contains("error"),qPrintable(neighbourFitReplay.value("error").toString()));
    const QJsonObject report{{"selectedPath", selectedPath}, {"neighbourPath", neighbourPath},
        {"selectedMs", selectedPts}, {"neighbourMs", neighbourPts},
        {"selectedSha256", FileSha256(selectedPath)}, {"neighbourSha256", FileSha256(neighbourPath)},
        {"beforeVerdict", int(beforeVerdict)}, {"afterVerdict", int(afterVerdict)},
        {"selectedOutput", uct_batch_test::Output(selected->result.output)},
        {"neighbourOutput", uct_batch_test::Output(neighbour->result.output)},
        {"selectedCandidates", CandidateWords(selected->result.output)}, {"shownWords", int(shown.size())},
        {"paintedWords", painted}, {"paintedCount", paintedCount},
        {"selectedWorkerMs", selected->result.durationMs}, {"neighbourWorkerMs", neighbour->result.durationMs},
        {"selectedFitReplay",selectedFitReplay},{"neighbourFitReplay",neighbourFitReplay},
        {"sourceDigestsDiffer", selected->result.sourcePixelDigest != neighbour->result.sourcePixelDigest},
        {"words", words}, {"readingText", model.ReadingText(0, selectedStamp.frameSeq)},
        {"imageEgress", selectedInput->Read().egress + neighbourInput->Read().egress},
        {"bridge", bridgeReport}, {"detail", detailReport},
        {"scope", "Actual resident recognition and immutable admission; no decoder, native display or latency benchmark."}};
    QVERIFY(uct_batch_test::Save(QDir(evidence).filePath("proof.json"), report));
    qInfo().noquote() << "Paused candidates=" << CandidateWords(selected->result.output)
                     << "shown=" << shown.size() << "egress=" << report["imageEgress"].toInt();
    // A real Windows bridge explicitly reads each image once for its CPU OCR.
    // GPU recognition/validation adds no image egress after that handoff.
    QCOMPARE(selectedInput->Read().egress + neighbourInput->Read().egress, bridge ? 2 : 0);
    QCOMPARE(model.SceneWords(3600000, selectedStamp.frameSeq).size(),
             model.SceneWords(0, selectedStamp.frameSeq).size());
    scheduler->Cancel();
    scheduler.reset();
    const auto& closed = retirement.Wait();
    QVERIFY2(closed.Passed(), "Exact TensorRT State did not close.");
    QVERIFY(CacheInventory(cache) == cacheBefore);
#endif
}
} // namespace uct_paused_video_test
