#pragma once

#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/common/async_cleanup.hpp"
#include "okuflow/common/uct_model_pins.hpp"
#include "ultra_clear_text_test_support.hpp"
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <utility>

namespace uct_batch_test {
using namespace okuflow::uct;

class Environment {
public:
    Environment(QByteArray key, QByteArray value)
        : key_(std::move(key)), present_(qEnvironmentVariableIsSet(key_.constData())),
          old_(qgetenv(key_.constData())) { qputenv(key_.constData(), value); }
    ~Environment() {
        if (present_) qputenv(key_.constData(), old_);
        else qunsetenv(key_.constData());
    }
private:
    QByteArray key_;
    bool present_{};
    QByteArray old_;
};

inline QJsonArray Rect(const QRectF& r) { return {r.x(), r.y(), r.width(), r.height()}; }

inline QJsonObject Output(const RecognitionOutput& output)
{
    QJsonArray lines;
    for (const auto& line : output.lines) {
        QJsonArray words;
        for (const auto& word : line.words)
            words.push_back(QJsonObject{{"text", word.text}, {"disposition", int(word.disposition)},
                {"confidence", word.confidence}, {"bounds", Rect(word.boundsPx)},
                {"protectedBounds", Rect(word.protectedBoundsPx)}, {"inkSearch", Rect(word.inkSearchPx)},
                {"paddedBounds", word.paddedBounds}});
        lines.push_back(words);
    }
    return {{"ok", output.ok}, {"cancelled", output.cancelled}, {"inputFault", output.inputFault},
            {"backendPreparing", output.backendPreparing}, {"error", output.error}, {"lines", lines}};
}

inline bool Save(const QString& path, const QJsonObject& value)
{
    QSaveFile file(path);
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

// Declare before the scheduler: even an early Qt assertion destroys the worker
// owner first, then performs this bounded diagnostic-only receipt wait.
class RetirementReceipts {
public:
    struct Result {
        bool observed{}, allCompleted{}, cleanupFailed{};
        QJsonArray statuses;
        bool Passed() const { return observed && allCompleted && !cleanupFailed; }
        QJsonObject Json() const {
            return {{"receiptStatuses", statuses}, {"receiptObserved", observed},
                {"allObservedStateClosuresCompleted", observed && allCompleted},
                {"cleanupFailed", cleanupFailed},
                {"receiptScope", "Exact TensorRT State closure only; nested AnalysisPool storage and vendor threads excluded."}};
        }
    };
    TensorRtRetirementObserver Observer() const {
        return [state = state_](std::shared_ptr<okuflow::AsyncCleanup> receipt) {
            std::lock_guard lock(state->mutex);
            state->receipts.push_back(std::move(receipt));
        };
    }
    const Result& Wait() {
        if (waited_) return result_;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            result_.statuses = {};
            result_.allCompleted = true;
            {
                std::lock_guard lock(state_->mutex);
                result_.observed = !state_->receipts.empty();
                for (const auto& receipt : state_->receipts) {
                    const auto status = receipt->Poll();
                    result_.statuses.push_back(int(status));
                    result_.allCompleted &= status == okuflow::AsyncCleanup::Status::Completed;
                    result_.cleanupFailed |= status == okuflow::AsyncCleanup::Status::Failed;
                }
            }
            if ((result_.observed && result_.allCompleted) || result_.cleanupFailed) break;
            QTest::qWait(5);
        } while (std::chrono::steady_clock::now() < deadline);
        waited_ = true;
        qInfo().noquote() << "TensorRT exact State retirement receiptCount=" << result_.statuses.size()
                         << "allCompleted=" << result_.Passed() << "cleanupFailed=" << result_.cleanupFailed;
        return result_;
    }
    ~RetirementReceipts() {
        // Observer/diagnostic failure must not propagate during unwinding.
        if (!waited_) {
            try { Wait(); }
            catch (...) { qWarning("TensorRT receipt wait failed during diagnostic unwinding."); }
        }
    }
private:
    struct State {
        std::mutex mutex;
        std::vector<std::shared_ptr<okuflow::AsyncCleanup>> receipts;
    };
    std::shared_ptr<State> state_{std::make_shared<State>()};
    Result result_;
    bool waited_{};
};

struct Agreement {
    bool structure{true};
    double geometryDelta{}, confidenceDelta{};
};

inline Agreement Compare(const RecognitionOutput& reference, const RecognitionOutput& actual)
{
    Agreement agreement;
    agreement.structure = reference.lines.size() == actual.lines.size();
    const auto rect = [&](const QRectF& a, const QRectF& b) {
        const std::array<double, 8> values{a.x(), a.y(), a.width(), a.height(),
                                           b.x(), b.y(), b.width(), b.height()};
        agreement.structure &= std::all_of(values.begin(), values.end(),
                                           [](double value) { return std::isfinite(value); });
        agreement.geometryDelta = std::max({agreement.geometryDelta,
            std::abs(a.x()-b.x()), std::abs(a.y()-b.y()),
            std::abs(a.width()-b.width()), std::abs(a.height()-b.height())});
    };
    for (int line = 0; line < std::min(reference.lines.size(), actual.lines.size()); ++line) {
        const auto& before = reference.lines[line].words;
        const auto& after = actual.lines[line].words;
        agreement.structure &= before.size() == after.size();
        for (int word = 0; word < std::min(before.size(), after.size()); ++word) {
            agreement.structure &= before[word].text == after[word].text &&
                before[word].disposition == after[word].disposition &&
                before[word].paddedBounds == after[word].paddedBounds;
            agreement.structure &= std::isfinite(before[word].confidence) &&
                                   std::isfinite(after[word].confidence);
            agreement.confidenceDelta = std::max(agreement.confidenceDelta,
                std::abs(before[word].confidence-after[word].confidence));
            rect(before[word].boundsPx, after[word].boundsPx);
            rect(before[word].protectedBoundsPx, after[word].protectedBoundsPx);
            rect(before[word].inkSearchPx, after[word].inkSearchPx);
        }
    }
    return agreement;
}

// One explicitly authorized model-pair experiment: at most one preparation per
// missing detector/recognizer profile, one warmup and three timed passes per
// variant. Existing plans are preserved; wider batches compare to their pair's N1.
// This diagnostic is intentionally not registered with CTest.
inline void CompareTensorRtBatches()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_BATCH_DIAGNOSTIC") != "1")
        QSKIP("Explicit TensorRT microbatch comparison not selected.");
    auto tierToken = qEnvironmentVariable("OKUFLOW_UCT_BATCH_TEST_TIER");
    if (tierToken.isEmpty()) tierToken = "medium";
    QVERIFY2(tierToken == "medium" || tierToken == "small",
             "OKUFLOW_UCT_BATCH_TEST_TIER must be medium or small.");
    auto detectorToken = qEnvironmentVariable("OKUFLOW_UCT_BATCH_TEST_DETECTOR_TIER");
    auto recognizerToken = qEnvironmentVariable("OKUFLOW_UCT_BATCH_TEST_RECOGNIZER_TIER");
    if (detectorToken.isEmpty()) detectorToken = tierToken;
    if (recognizerToken.isEmpty()) recognizerToken = tierToken;
    const auto detectorTier = ParseModelTier(detectorToken);
    const auto recognizerTier = ParseModelTier(recognizerToken);
    QVERIFY2(detectorTier.has_value(), "Detector tier must be tiny, small or medium.");
    QVERIFY2(recognizerTier.has_value(), "Recognizer tier must be tiny, small or medium.");
    const auto assets = LoadPaddleAssets(DefaultPaddleRoot(), *detectorTier, *recognizerTier);
    QVERIFY2(assets.IsValid(), qPrintable(assets.error));
    const auto pinJson = [detectorTier, recognizerTier](bool detection) {
        const auto tier = detection ? *detectorTier : *recognizerTier;
        const auto& pin = *FindPaddleModelPin(detection, tier);
        const auto text = [](std::string_view value) {
            return QString::fromUtf8(value.data(), qsizetype(value.size()));
        };
        return QJsonObject{{"tier", ModelTierToken(tier)}, {"directory", text(pin.directory)},
            {"modelSha256", text(pin.modelSha256)}, {"configSha256", text(pin.configSha256)},
            {"repository", text(pin.repository)}, {"revision", text(pin.revision)}};
    };
    const QJsonObject models{{"detector", pinJson(true)}, {"recognizer", pinJson(false)}};
    const auto source = qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE");
    QVERIFY2(!source.isEmpty(), "OKUFLOW_UCT_TEST_IMAGE must select a private source image.");
    const auto image = QImage(source).convertToFormat(QImage::Format_ARGB32);
    QVERIFY2(!image.isNull(), qPrintable(source));
    const auto evidence = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(
        "OkuFlow/uct-batch-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") +
        "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(QDir().mkpath(evidence));
    qInfo().noquote() << "TensorRT batch comparison detector=" << detectorToken
                     << "recognizer=" << recognizerToken << "evidence=" << evidence;
    QVERIFY(Save(QDir(evidence).filePath("source.json"),
        {{"width", image.width()}, {"height", image.height()},
         {"models", models},
         {"activeRowSha256", QString::fromLatin1(uct_test::SourcePixelsDigest(image).toHex())},
         {"timedPassesPerVariant", 3}, {"warmupsPerVariant", 1}, {"capacities", QJsonArray{1, 8, 16}}}));
    Environment profiling("OKUFLOW_UCT_TRT_PROFILE", "1");
    std::array<RecognitionOutput, 3> reference;
    std::array<double, 3> averageMs{};
    const std::array<int, 3> capacities{1, 8, 16};
    bool allAgree = true;
    for (int variant = 0; variant < 3; ++variant) {
        const int batch = capacities[variant];
        Environment selectedBatch("OKUFLOW_UCT_TRT_BATCH", QByteArray::number(batch));
        const auto start = std::chrono::steady_clock::now();
        const auto elapsed = [start] {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now()-start).count();
        };
        struct Completion { RecognitionResult result; BackendStatus backend; double elapsedMs{}; };
        struct State {
            std::mutex mutex;
            BackendStatus backend;
            std::optional<Completion> completed;
        };
        const auto state = std::make_shared<State>();
        RetirementReceipts retirement;
        auto scheduler = std::make_unique<RecognitionScheduler>(
            CreateTensorRtRecognizer(assets.root, {}, *detectorTier, *recognizerTier, retirement.Observer()),
            [state, elapsed](RecognitionResult result) {
                std::lock_guard lock(state->mutex);
                state->completed = Completion{std::move(result), state->backend, elapsed()};
            },
            [state](BackendStatus backend) {
                std::lock_guard lock(state->mutex);
                state->backend = std::move(backend);
            });
        SourceStamp stamp;
        stamp.sourceEpoch = 1; stamp.geometryGeneration = 1; stamp.frameSeq = 1;
        stamp.viewportWidth = image.width(); stamp.viewportHeight = image.height();
        stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
        const auto wait = [&](double deadline) -> std::optional<Completion> {
            while (elapsed() < deadline) {
                {
                    std::lock_guard lock(state->mutex);
                    if (state->completed) return std::exchange(state->completed, {});
                }
                QTest::qWait(5);
            }
            return {};
        };
        std::optional<Completion> warmup;
        while (elapsed() < 180000) {
            QVERIFY2(scheduler->Submit(stamp, image) != 0, qPrintable(scheduler->Status().detail));
            auto completion = wait(180000);
            QVERIFY2(completion.has_value(), "TensorRT preparation/warmup exceeded the existing 180-second horizon.");
            if (completion->result.output.backendPreparing && !completion->result.output.ok) {
                QTest::qWait(500);
                continue;
            }
            warmup = std::move(completion);
            break;
        }
        QVERIFY2(warmup.has_value(), "TensorRT preparation produced no warmup result.");
        QVERIFY(Save(QDir(evidence).filePath(QString("batch%1-warmup.json").arg(batch)),
            {{"elapsedIncludingPreparationMs", warmup->elapsedMs},
             {"models", models},
             {"workerMs", warmup->result.durationMs}, {"backend", warmup->backend.name},
             {"detail", warmup->backend.detail}, {"output", Output(warmup->result.output)}}));
        qInfo().noquote() << "TensorRT batch warmup capacity=" << batch
            << "elapsedIncludingPreparationMs=" << warmup->elapsedMs
            << "workerMs=" << warmup->result.durationMs << "detail=" << warmup->backend.detail;
        QVERIFY2(warmup->result.output.ok, qPrintable(warmup->result.output.error));
        QVERIFY(warmup->backend.usesGpu);
        QVERIFY2(warmup->backend.detail.contains(QString("recognizer batch=%1.").arg(batch)),
                 qPrintable(warmup->backend.detail)); // do not benchmark a fallback as the requested capacity
        for (int pass = 0; pass < 3; ++pass) {
            const double deadline = elapsed() + 30000;
            QVERIFY(scheduler->Submit(stamp, image) != 0);
            auto completion = wait(deadline);
            QVERIFY2(completion.has_value(), "Timed TensorRT pass exceeded 30 seconds.");
            const auto& result = completion->result;
            const auto agreement = variant == 0 ? Agreement{} : Compare(reference[pass], result.output);
            int candidateWords = 0;
            for (const auto& line : result.output.lines) for (const auto& word : line.words)
                candidateWords += word.disposition == TokenDisposition::kCandidate;
            QVERIFY(Save(QDir(evidence).filePath(QString("batch%1-pass%2.json").arg(batch).arg(pass+1)),
                {{"workerMs", result.durationMs}, {"backend", completion->backend.name},
                 {"models", models},
                 {"detail", completion->backend.detail}, {"output", Output(result.output)},
                 {"structureMatchesBatchOne", agreement.structure},
                 {"candidateWords", candidateWords},
                 {"maximumGeometryDeltaPx", agreement.geometryDelta},
                 {"maximumConfidenceDelta", agreement.confidenceDelta}}));
            qInfo().noquote() << "TensorRT batch timed capacity=" << batch << "pass=" << pass+1
                << "workerMs=" << result.durationMs << "structureMatches=" << agreement.structure
                << "candidateWords=" << candidateWords
                << "geometryDeltaPx=" << agreement.geometryDelta
                << "confidenceDelta=" << agreement.confidenceDelta;
            QVERIFY2(result.output.ok, qPrintable(result.output.error));
            QVERIFY(completion->backend.usesGpu);
            QVERIFY(result.reference.IsValid());
            QVERIFY(!result.output.lines.isEmpty());
            QVERIFY(candidateWords > 0);
            if (variant == 0) reference[pass] = result.output;
            else allAgree &= agreement.structure && agreement.geometryDelta <= 1e-3 &&
                             agreement.confidenceDelta <= 1e-3;
            averageMs[variant] += result.durationMs / 3;
        }
        scheduler->Cancel();
        scheduler.reset();
        const auto& retired = retirement.Wait();
        auto retirementReport = retired.Json();
        retirementReport.insert("models", models);
        retirementReport.insert("requestedBatch", batch);
        QVERIFY(Save(QDir(evidence).filePath(QString("batch%1-retirement.json").arg(batch)), retirementReport));
        QVERIFY2(retired.Passed(), "Exact TensorRT State retirement failed or exceeded 10 seconds; do not advance variants.");
    }
    qInfo().noquote() << "TensorRT comparison detector=" << detectorToken << "recognizer=" << recognizerToken
        << "mean scheduler-worker completionMs batch1=" << averageMs[0]
        << "batch8=" << averageMs[1] << "batch16=" << averageMs[2]
        << "allTimedQualityComparisonsPass=" << allAgree;
    qInfo("Worker completion includes source upload and CPU result preparation; stream stage events are separate. No display/FPS claim.");
    QVERIFY2(allAgree, "A wider batch changed token order/text/disposition, source geometry >0.001px, or confidence >0.001. All outputs preserved.");
}

// A separate explicitly selected cached teardown regression, not another
// throughput comparison. No parser/builder resources may be present, so a
// missing/incompatible cached plan fails rather than starting a new build.
inline void TensorRtRetirementDiagnostic()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_RETIREMENT_DIAGNOSTIC") != "1")
        QSKIP("Explicit cached TensorRT retirement diagnostic not selected.");
    const auto source = qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE");
    QVERIFY2(!source.isEmpty(), "OKUFLOW_UCT_TEST_IMAGE must select a private source image.");
    const auto image = QImage(source).convertToFormat(QImage::Format_ARGB32);
    QVERIFY2(!image.isNull(), qPrintable(source));
    const auto sdk = qEnvironmentVariable("OKUFLOW_UCT_TENSORRT_ROOT");
    const QDir bin(QDir(sdk).filePath("bin"));
    QVERIFY2(!sdk.isEmpty() && QFile::exists(bin.filePath("nvinfer_11.dll")),
             "Select the existing inference-only SDK explicitly.");
    QVERIFY2(!QFile::exists(bin.filePath("nvonnxparser_11.dll")) &&
             bin.entryList({"nvinfer_builder_resource*.dll"}, QDir::Files).isEmpty(),
             "This cached-only diagnostic refuses SDK parser/builder resources.");
    const auto evidence = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(
        "OkuFlow/uct-retirement-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") +
        "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(QDir().mkpath(evidence));
    qInfo().noquote() << "TensorRT retirement evidence=" << evidence;
    qInfo("One cached N16 inference; profiling enabled to exercise State-owned events; retirement wait <=10000ms.");
    Environment selectedBatch("OKUFLOW_UCT_TRT_BATCH", "16");
    Environment profiling("OKUFLOW_UCT_TRT_PROFILE", "1");
    struct Completion { RecognitionResult result; BackendStatus backend; };
    struct State {
        std::mutex mutex;
        BackendStatus backend;
        std::optional<Completion> completed;
    };
    const auto state = std::make_shared<State>();
    std::optional<Completion> final;
    QString failure;
    RetirementReceipts retirement;
    {
        RecognitionScheduler scheduler(CreateTensorRtRecognizer({}, sdk, ModelTier::Medium,
            ModelTier::Medium, retirement.Observer()),
            [state](RecognitionResult result) {
                std::lock_guard lock(state->mutex);
                state->completed = Completion{std::move(result), state->backend};
            },
            [state](BackendStatus backend) {
                std::lock_guard lock(state->mutex);
                state->backend = std::move(backend);
            });
        SourceStamp stamp;
        stamp.sourceEpoch = 1; stamp.geometryGeneration = 1; stamp.frameSeq = 1;
        stamp.viewportWidth = image.width(); stamp.viewportHeight = image.height();
        stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        auto nextSubmission = std::chrono::steady_clock::now();
        bool pending = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!pending && std::chrono::steady_clock::now() >= nextSubmission) {
                if (!scheduler.Submit(stamp, image)) {
                    failure = "Submission refused: " + scheduler.Status().detail;
                    break;
                }
                pending = true;
            }
            std::optional<Completion> completion;
            {
                std::lock_guard lock(state->mutex);
                completion = std::exchange(state->completed, {});
            }
            if (completion) {
                pending = false;
                if (completion->result.output.backendPreparing && !completion->result.output.ok)
                    nextSubmission = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                else {
                    final = std::move(completion);
                    break; // exactly one completed inference, including on failure
                }
            }
            QTest::qWait(5);
        }
        if (!final && failure.isEmpty()) failure = "Cached preparation/inference exceeded 30 seconds.";
        scheduler.Cancel();
    } // join the completed scheduler worker; its recognizer publishes the exact receipt

    const auto& retired = retirement.Wait();
    int candidates = 0;
    if (final) for (const auto& line : final->result.output.lines) for (const auto& word : line.words)
        candidates += word.disposition == TokenDisposition::kCandidate;
    auto report = retired.Json();
    report.insert("requestedBatch", 16);
    report.insert("profilingEnabled", true);
    report.insert("sourceActiveRowSha256", QString::fromLatin1(uct_test::SourcePixelsDigest(image).toHex()));
    report.insert("failure", failure);
    report.insert("candidateWords", candidates);
    if (final) {
        report.insert("backend", final->backend.name);
        report.insert("usesGpu", final->backend.usesGpu);
        report.insert("detail", final->backend.detail);
        report.insert("output", Output(final->result.output));
    }
    QVERIFY(Save(QDir(evidence).filePath("retirement.json"), report));
    qInfo("Receipt completion excludes nested AnalysisPool storage/vendor threads. Normal process exit must be checked separately; no crash-fix or glyph-admission claim.");
    QVERIFY2(retired.Passed(),
             "Exact TensorRT State retirement receipt missing, failed/quarantined, or still running at the bounded deadline.");
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(final.has_value());
    QVERIFY2(final->result.output.ok, qPrintable(final->result.output.error));
    QVERIFY(!final->result.output.cancelled && !final->result.output.backendPreparing);
    QVERIFY(final->backend.usesGpu);
    QVERIFY2(final->backend.detail.contains("recognizer batch=16."), qPrintable(final->backend.detail));
    QVERIFY(final->result.reference.IsValid());
    QVERIFY(candidates > 0);
}

} // namespace uct_batch_test
