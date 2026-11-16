#pragma once
// Opt-in GPU contracts for TensorRT graph replay (src/common/uct_graph_cache.hpp).
//
// - Faults (OKUFLOW_UCT_GRAPH_FAULT_TEST=1): every failure path of the replay
//   path, driven on the GPU through the test-only fault hooks that only this
//   executable compiles (OKUFLOW_UCT_GRAPH_FAULT_HOOKS). Each read must return
//   the switch-off bytes or fail closed through State retirement; no call may
//   leave its stream capturing; the receipt counters must show the path ran.
// - Checkpoints (OKUFLOW_UCT_GRAPH_CHECKPOINT_TEST=1): cancellations at named
//   points of replay calls (planned, captured, drained, launched), two frame
//   sizes, region reconciliation on, a two-shape bound so slots are recycled.
// - Unadopted retirement (OKUFLOW_UCT_GRAPH_RETIRE_TEST=1): a reader that is
//   ready but destroyed before its first read retires its State, its
//   context-creation thread ends, and a fresh reader of the same model is
//   ready on BF16; also with the retirement landing while that thread is
//   between its completion check and its wait.
// - Configurations (OKUFLOW_UCT_GRAPH_CONFIG_TEST=1): switch-off against
//   switch-on on other model tiers, FP32 plans, recognizer batch 1 and 16, the
//   bucketed recognizer width and paused-detail reads, with timing, receipts
//   and identical output.
//
// Images: OKUFLOW_UCT_TEST_IMAGE and OKUFLOW_UCT_TEST_IMAGE_2, two frame sizes.
// Uses prepared plans; the configurations may prepare plans that are missing
// (full SDK, GPU lease).
#include "uct_resident_diagnostic.hpp"
#include "../src/common/uct_graph_cache.hpp"

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>

namespace okuflow::uct::uct_graph_faults_test {
#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT && defined(OKUFLOW_UCT_GRAPH_FAULT_HOOKS)
using graph_cache::Checkpoint;
using graph_cache::Fault;
using graph_cache::FaultPlan;
using uct_resident_test::ExactOutputBytes;
using uct_resident_test::ProfileCapture;

struct Images {
    QImage a, b;
    bool Load()
    {
        a = QImage(qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE"));
        b = QImage(qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE_2"));
        return !a.isNull() && !b.isNull() && a.size() != b.size();
    }
};

// Reads until the reader is no longer preparing (after a retirement it prepares again).
inline RecognitionOutput Read(Recognizer& recognizer, const QImage& image, const std::atomic<bool>& cancelled,
                              int timeoutMs = 1800000)
{
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        auto output = recognizer.Recognize(image, cancelled);
        if (!output.backendPreparing || timer.elapsed() > timeoutMs) return output;
        QTest::qWait(20);
    }
}
inline bool Ready(Recognizer& recognizer, int timeoutMs = 1800000)
{
    if (recognizer.Initialize().state != BackendState::kReady) return false;
    QElapsedTimer timer;
    timer.start();
    while (recognizer.Preparing() && timer.elapsed() < timeoutMs) QTest::qWait(20);
    return !recognizer.Preparing() && recognizer.Status().state == BackendState::kReady;
}
inline QJsonObject LastReceipt(const QVector<QJsonObject>& lines)
{
    for (auto it = lines.rbegin(); it != lines.rend(); ++it)
        if (it->contains("graphReplay")) return (*it)["graphReplay"].toObject();
    return {};
}
inline qint64 Count(const QJsonObject& receipt, const char* engine, const char* key)
{
    return receipt[engine].toObject()[key].toInteger();
}
inline qint64 Sum(const QJsonObject& receipt, const char* key)
{
    return Count(receipt, "detector", key) + Count(receipt, "recognizer", key);
}
// Fault plans are shared: every hook loads its own reference, so a context
// thread of a retired State that is still finishing keeps the plan it read
// alive. The MoveInput copies are read only by reads, which have all ended
// (and their streams completed: the tests wait for each retirement) when the
// plans are destroyed.
struct Plans {
    std::vector<std::shared_ptr<FaultPlan>> plans;
    FaultPlan& Next()
    {
        plans.push_back(std::make_shared<FaultPlan>());
        return *plans.back();
    }
    std::shared_ptr<FaultPlan> Last() const { return plans.back(); }
    ~Plans()
    {
        graph_cache::FaultPlanForTest() = nullptr;
        for (auto& plan : plans)
            for (auto*& scratch : plan->scratch)
                if (scratch) cudaFree(std::exchange(scratch, nullptr));
    }
};
// Context-creation threads end within a bound once their State retires.
inline bool CreatorsEnded(int timeoutMs = 30000)
{
    QElapsedTimer timer;
    timer.start();
    while (graph_cache::CreatorThreadsForTest().load() != 0 && timer.elapsed() < timeoutMs) QTest::qWait(10);
    return graph_cache::CreatorThreadsForTest().load() == 0;
}
inline bool Reference(const Images& images, QByteArray& a, QByteArray& b)
{
    uct_batch_test::Environment off("OKUFLOW_UCT_TEST_TRT_GRAPH", "0");
    uct_batch_test::RetirementReceipts retirement;
    auto recognizer = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, retirement.Observer());
    if (!Ready(*recognizer) || recognizer->Status().detail.contains("graph replay")) return false;
    std::atomic<bool> never{false};
    const auto first = Read(*recognizer, images.a, never), second = Read(*recognizer, images.b, never);
    if (!first.ok || !second.ok) return false;
    a = ExactOutputBytes(first);
    b = ExactOutputBytes(second);
    recognizer.reset();
    return retirement.Wait().Passed();
}

inline void Faults()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_GRAPH_FAULT_TEST") != "1")
        QSKIP("Explicit graph-replay fault diagnostic not selected.");
    Images images;
    QVERIFY2(images.Load(), "OKUFLOW_UCT_TEST_IMAGE and OKUFLOW_UCT_TEST_IMAGE_2 must name two images of different sizes.");
    QByteArray referenceA, referenceB;
    QVERIFY2(Reference(images, referenceA, referenceB), "Switch-off reference reads failed.");

    struct Case {
        const char* name;
        Fault fault;
        int engine, skip, count;
        const char* shapes; // OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES, or the default bound
        bool fatal;         // the read fails closed and the State retires
    };
    const Case cases[] = {
        {"capture refused by CUDA (illegal call inside the capture)", Fault::InvalidateCapture, -1, 0, 1, nullptr, false},
        {"capture put off: free memory below the headroom", Fault::LowFreeMemory, -1, 0, 3, nullptr, false},
        {"capture enqueue fails with a capture status", Fault::CaptureEnqueueFailsInvalidated, -1, 1, 1, nullptr, false},
        {"capture enqueue fails without a CUDA reason", Fault::CaptureEnqueueFails, -1, 2, 1, nullptr, true},
        {"eviction drains work still queued", Fault::QueueBeforeDrain, -1, 0, 6, "1", false},
        {"input arrives at another address", Fault::MoveInput, -1, 60, 20, nullptr, false},
        {"TensorRT rejects the shape between Plan and Bound", Fault::RejectShape, -1, 3, 1, nullptr, true},
        {"background context creation fails", Fault::ContextCreationFails, 1, 0, 1, nullptr, false},
        {"shared activation arena unavailable", Fault::ArenaUnavailable, 0, 0, 1, nullptr, false},
        {"replay output storage unavailable", Fault::OutputStorageUnavailable, 1, 0, 1, nullptr, false},
    };
    Plans plans;
    QJsonArray results;
    std::atomic<bool> never{false};
    for (const auto& item : cases) {
        auto& plan = plans.Next();
        plan.fault = item.fault;
        plan.engine = item.engine;
        plan.skip = item.skip;
        plan.count = item.count;
        graph_cache::FaultPlanForTest() = plans.Last();
        uct_batch_test::Environment graph("OKUFLOW_UCT_TEST_TRT_GRAPH", "1");
        uct_batch_test::Environment profiling("OKUFLOW_UCT_TRT_PROFILE", "1");
        std::optional<uct_batch_test::Environment> shapes;
        if (item.shapes) shapes.emplace("OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES", item.shapes);
        else qunsetenv("OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES");
        ProfileCapture capture;
        uct_batch_test::RetirementReceipts retirement;
        auto recognizer = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, retirement.Observer());
        QVERIFY2(Ready(*recognizer), qPrintable(QString(item.name) + ": " + recognizer->Status().detail));
        const auto detail = recognizer->Status().detail;
        // Two frame sizes, repeated: shapes are planned, warmed, captured,
        // replayed and (with bound 1) evicted.
        const QImage* sequence[] = {&images.a, &images.a, &images.a, &images.b, &images.b, &images.b,
                                    &images.a, &images.a, &images.b, &images.a};
        // Low free memory: each deferral holds captures off for
        // kCaptureHoldOffReads reads, so the case reads long enough for every
        // injected deferral to pass and for captures to follow the last one.
        const int reads = item.fault == Fault::LowFreeMemory
            ? int(std::size(sequence)) + (item.count + 1) * (graph_cache::kCaptureHoldOffReads + 1)
            : int(std::size(sequence));
        int failed = 0, completed = 0;
        QString failure;
        qint64 capturesAtLastDeferral = -1;
        for (int index = 0; index < reads; ++index) {
            const auto* image = sequence[std::size_t(index) % std::size(sequence)];
            const auto output = Read(*recognizer, *image, never);
            if (item.fault == Fault::LowFreeMemory && capturesAtLastDeferral < 0 &&
                plan.injected.load() >= item.count) {
                // The read that met the last injected deferral.
                capturesAtLastDeferral = Sum(LastReceipt(capture.Peek()), "captures");
            }
            QVERIFY2(!output.backendPreparing, "The reader did not become ready again.");
            if (!output.ok) {
                ++failed;
                failure = output.error;
                QVERIFY2(item.fatal, qPrintable(QString(item.name) + ": a read failed: " + output.error));
                QVERIFY2(!output.error.isEmpty(), "A read that fails closed must say why.");
                continue;
            }
            ++completed;
            QVERIFY2(ExactOutputBytes(output) == (image == &images.a ? referenceA : referenceB),
                     qPrintable(QString(item.name) + ": graph replay changed the complete output."));
        }
        const auto receipt = LastReceipt(capture.Take());
        QVERIFY2(!receipt.isEmpty(), "No graph receipt was emitted.");
        QVERIFY2(plan.injected.load() >= 1, qPrintable(QString(item.name) + ": the fault was never reached."));
        QCOMPARE(plan.callsEndedCapturing.load(), 0);
        QVERIFY(plan.replayCalls.load() > 0);
        QCOMPARE(failed, item.fatal ? 1 : 0);
        switch (item.fault) {
        case Fault::InvalidateCapture:
        case Fault::CaptureEnqueueFailsInvalidated:
            QCOMPARE(Sum(receipt, "refusals"), qint64(1));
            QVERIFY(Sum(receipt, "captures") > 0);
            QVERIFY(!receipt["detector"].toObject()["captureBlocked"].toBool());
            break;
        case Fault::LowFreeMemory:
            QCOMPARE(plan.injected.load(), item.count);
            QVERIFY(Sum(receipt, "deferrals") >= item.count);
            QCOMPARE(Sum(receipt, "refusals"), qint64(0));
            QVERIFY(Sum(receipt, "heldOff") > 0);
            // Recovers after transient low memory: the hold-off has expired
            // on both engines and captures followed the last deferral.
            QVERIFY(!receipt["detector"].toObject()["captureHeldOff"].toBool());
            QVERIFY(!receipt["recognizer"].toObject()["captureHeldOff"].toBool());
            QVERIFY(capturesAtLastDeferral >= 0);
            QVERIFY2(Sum(receipt, "captures") > capturesAtLastDeferral,
                     qPrintable(QString("captures %1 at the last deferral, %2 at the end")
                                .arg(capturesAtLastDeferral).arg(Sum(receipt, "captures"))));
            break;
        case Fault::CaptureEnqueueFails:
            QVERIFY2(failure.contains("graph capture"), qPrintable(failure));
            QCOMPARE(Sum(receipt, "refusals"), qint64(0));
            break;
        case Fault::QueueBeforeDrain:
            QVERIFY(plan.drainsAfterQueuedWork.load() >= 1);
            QCOMPARE(plan.drainsBeforeQueuedWorkDone.load(), 0);
            QVERIFY(Sum(receipt, "evictions") > 0);
            break;
        case Fault::MoveInput:
            QVERIFY(Sum(receipt, "rebinds") >= 1);
            break;
        case Fault::RejectShape:
            QVERIFY2(failure.contains("shape rejected"), qPrintable(failure));
            break;
        case Fault::ContextCreationFails:
            QCOMPARE(receipt["recognizer"].toObject()["contextsStopped"].toString(), QString("context creation failed"));
            QCOMPARE(Count(receipt, "recognizer", "contextsCreated"), qint64(0));
            QVERIFY(Count(receipt, "recognizer", "overflow") > 0);
            QVERIFY(Count(receipt, "detector", "contextsCreated") > 0);
            break;
        case Fault::ArenaUnavailable:
            QVERIFY(!receipt["detector"].toObject()["enabled"].toBool());
            QVERIFY(receipt["detector"].toObject()["reason"].toString().contains("arena"));
            QVERIFY(receipt["recognizer"].toObject()["enabled"].toBool());
            QVERIFY2(detail.contains("detector plain"), qPrintable(detail));
            break;
        case Fault::OutputStorageUnavailable:
            QVERIFY(!receipt["recognizer"].toObject()["enabled"].toBool());
            QVERIFY(receipt["recognizer"].toObject()["reason"].toString().contains("output storage"));
            // The failed larger allocation kept the plain output buffer.
            QVERIFY(receipt["recognizer"].toObject()["outputBytesKeptAfterFailure"].toInteger() > 0);
            QVERIFY(receipt["detector"].toObject()["enabled"].toBool());
            QVERIFY2(detail.contains("recognizer plain"), qPrintable(detail));
            break;
        default:
            QFAIL("Unhandled fault.");
        }
        QVERIFY2(recognizer->Status().state == BackendState::kReady, qPrintable(recognizer->Status().detail));
        recognizer.reset();
        const auto& closed = retirement.Wait();
        QVERIFY2(closed.Passed(), qPrintable(QString(item.name) + ": State retirement failed."));
        QCOMPARE(closed.statuses.size(), item.fatal ? 2 : 1);
        QVERIFY2(CreatorsEnded(), "A context-creation thread did not end after its State retired.");
        graph_cache::FaultPlanForTest() = nullptr;
        results.push_back(QJsonObject{{"case", item.name}, {"injected", plan.injected.load()},
            {"readsCompleted", completed}, {"readsFailedClosed", failed}, {"failure", failure},
            {"replayCalls", plan.replayCalls.load()}, {"callsEndedCapturing", plan.callsEndedCapturing.load()},
            {"drainsAfterQueuedWork", plan.drainsAfterQueuedWork.load()},
            {"drainsBeforeQueuedWorkDone", plan.drainsBeforeQueuedWorkDone.load()},
            {"statesRetired", int(closed.statuses.size())}, {"receipt", receipt}});
        qInfo().noquote() << "graph replay fault" << item.name << "injected" << plan.injected.load()
                          << "completed" << completed << "failed" << failed;
    }
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_GRAPH_TEST_EVIDENCE");
    if (!evidence.isEmpty()) QVERIFY(uct_batch_test::Save(evidence, QJsonObject{{"faults", results}}));
}

inline void Checkpoints()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_GRAPH_CHECKPOINT_TEST") != "1")
        QSKIP("Explicit graph-replay checkpoint diagnostic not selected.");
    Images images;
    QVERIFY2(images.Load(), "OKUFLOW_UCT_TEST_IMAGE and OKUFLOW_UCT_TEST_IMAGE_2 must name two images of different sizes.");
    uct_batch_test::Environment reconcile("OKUFLOW_UCT_TEST_REGION_RECONCILE", "1");
    QByteArray referenceA, referenceB;
    QVERIFY2(Reference(images, referenceA, referenceB), "Switch-off reference reads failed.");

    Plans plans;
    auto& plan = plans.Next();
    // Recognize runs on this thread, so the checkpoint callback does too.
    Checkpoint target{};
    int remaining = -1;
    std::atomic<bool>* armed = nullptr;
    std::map<int, int> reached;
    plan.checkpoint = [&](Checkpoint at, int) {
        ++reached[int(at)];
        if (armed && at == target && --remaining == 0) armed->store(true);
    };
    graph_cache::FaultPlanForTest() = plans.Last();
    uct_batch_test::Environment graph("OKUFLOW_UCT_TEST_TRT_GRAPH", "1");
    uct_batch_test::Environment bound("OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES", "2");
    uct_batch_test::RetirementReceipts retirement;
    auto recognizer = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, retirement.Observer());
    QVERIFY2(Ready(*recognizer), qPrintable(recognizer->Status().detail));
    std::atomic<bool> never{false};
    const auto check = [&](const RecognitionOutput& output, const QImage* image) {
        QVERIFY2(output.ok, qPrintable(output.error));
        QVERIFY2(ExactOutputBytes(output) == (image == &images.a ? referenceA : referenceB),
                 "Graph replay changed the complete output.");
    };
    for (const auto* image : {&images.a, &images.b}) {
        check(Read(*recognizer, *image, never), image);
        if (QTest::currentTestFailed()) return;
    }
    QJsonArray rows;
    for (const auto at : {Checkpoint::Planned, Checkpoint::Captured, Checkpoint::Drained, Checkpoint::Launched}) {
        int cancelled = 0, completed = 0;
        for (const int k : {1, 2, 3, 5, 8, 13, 21, 34}) {
            for (const auto* image : {&images.a, &images.b}) {
                std::atomic<bool> cancel{false};
                target = at;
                remaining = k;
                armed = &cancel;
                const auto output = recognizer->Recognize(*image, cancel);
                armed = nullptr;
                if (output.cancelled) {
                    ++cancelled;
                    QVERIFY2(!output.ok && output.error.isEmpty(), qPrintable(output.error));
                    QVERIFY2(recognizer->Status().state == BackendState::kReady, qPrintable(recognizer->Status().detail));
                } else {
                    ++completed;
                    check(output, image);
                }
                // The read after any cancellation is complete and unchanged.
                check(Read(*recognizer, *image, never), image);
                if (QTest::currentTestFailed()) return;
            }
        }
        const char* names[] = {"planned", "captured", "drained", "launched"};
        rows.push_back(QJsonObject{{"checkpoint", names[int(at)]}, {"cancelled", cancelled}, {"completed", completed}});
        qInfo() << "graph replay checkpoint" << names[int(at)] << "cancelled" << cancelled << "completed" << completed;
        QVERIFY2(cancelled > 0, "No read was cancelled at this checkpoint.");
    }
    QCOMPARE(plan.callsEndedCapturing.load(), 0);
    QVERIFY2(recognizer->Status().state == BackendState::kReady, qPrintable(recognizer->Status().detail));
    recognizer.reset();
    QVERIFY2(retirement.Wait().Passed(), "TensorRT State retirement with graph replay failed.");
    QCOMPARE(retirement.Wait().statuses.size(), 1);
    QVERIFY2(CreatorsEnded(), "A context-creation thread did not end after its State retired.");
    graph_cache::FaultPlanForTest() = nullptr;
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_GRAPH_TEST_EVIDENCE");
    if (!evidence.isEmpty()) QVERIFY(uct_batch_test::Save(evidence, QJsonObject{{"checkpoints", rows},
        {"reached", QJsonObject{{"planned", reached[0]}, {"captured", reached[1]}, {"drained", reached[2]},
                                {"launched", reached[3]}}}}));
}

inline void RetireUnadopted()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_GRAPH_RETIRE_TEST") != "1")
        QSKIP("Explicit graph-replay retirement diagnostic not selected.");
    const QImage image(qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE"));
    QVERIFY2(!image.isNull(), "OKUFLOW_UCT_TEST_IMAGE must name a readable image.");
    uct_batch_test::Environment graph("OKUFLOW_UCT_TEST_TRT_GRAPH", "1");
    qunsetenv("OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES");
    QVERIFY2(CreatorsEnded(), "A context-creation thread of an earlier test is still running.");
    // Holds the context-creation thread at its first turn, between the check
    // whether every context is made and the wait, until released. Shared, so
    // a thread that outlives a failed assertion never reads a dead local.
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        bool reached{}, released{};
    };
    const auto waitFor = [](const std::function<bool()>& condition, int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < timeoutMs) QTest::qWait(5);
        return condition();
    };
    QJsonArray rows;
    for (const bool betweenCheckAndWait : {false, true}) {
        const char* variant = betweenCheckAndWait ? "retired between the completion check and the wait"
                                                  : "retired while waiting for a wanted context";
        Plans plans;
        auto& plan = plans.Next();
        const auto gate = std::make_shared<Gate>();
        if (betweenCheckAndWait)
            plan.creatorWaiting = [gate] {
                std::unique_lock lock(gate->mutex);
                if (gate->reached) return;
                gate->reached = true;
                gate->changed.notify_all();
                gate->changed.wait(lock, [&] { return gate->released; });
            };
        graph_cache::FaultPlanForTest() = plans.Last();
        qint64 receiptsBeforeRelease = -1;
        {
            uct_batch_test::RetirementReceipts retirement;
            auto recognizer = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium,
                                                                 retirement.Observer());
            QVERIFY2(Ready(*recognizer), qPrintable(recognizer->Status().detail));
            QVERIFY2(recognizer->Status().detail.contains("graph replay"), qPrintable(recognizer->Status().detail));
            // Ready, and the State's context-creation thread is running.
            QVERIFY2(waitFor([] { return graph_cache::CreatorThreadsForTest().load() == 1; }, 30000),
                     "The context-creation thread did not start after ready.");
            if (betweenCheckAndWait)
                QVERIFY2(waitFor([gate] { std::lock_guard lock(gate->mutex); return gate->reached; }, 30000),
                         "The context-creation thread did not reach its first turn.");
            else QTest::qWait(200); // let it settle into its wait
            // Destroyed without a read: the State was published and never adopted.
            recognizer.reset();
            if (betweenCheckAndWait) {
                // The State must be retired while its thread is held: the
                // retirement may not depend on that thread getting a turn.
                receiptsBeforeRelease = retirement.Wait().Passed() ? retirement.Wait().statuses.size() : 0;
                {
                    std::lock_guard lock(gate->mutex);
                    gate->released = true;
                }
                gate->changed.notify_all();
            }
            const auto& closed = retirement.Wait();
            QVERIFY2(closed.Passed(), qPrintable(QString(variant) + ": the unadopted State was not retired."));
            QCOMPARE(closed.statuses.size(), 1);
            QVERIFY2(CreatorsEnded(), qPrintable(QString(variant) + ": the context-creation thread did not end."));
        }
        if (betweenCheckAndWait) QCOMPARE(receiptsBeforeRelease, qint64(1));
        graph_cache::FaultPlanForTest() = nullptr;
        // A fresh reader of the same model: its BF16 attempts were released.
        uct_batch_test::RetirementReceipts again;
        auto fresh = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, again.Observer());
        QVERIFY2(Ready(*fresh), qPrintable(fresh->Status().detail));
        std::atomic<bool> never{false};
        const auto output = Read(*fresh, image, never);
        QVERIFY2(output.ok, qPrintable(output.error));
        const auto status = fresh->Status();
        QVERIFY2(status.name.contains(QLatin1String("[BF16 / BF16]")), qPrintable(status.name + " / " + status.detail));
        QVERIFY2(!status.detail.contains(QLatin1String("in flight")), qPrintable(status.detail));
        fresh.reset();
        QVERIFY2(again.Wait().Passed(), "The fresh reader's State retirement failed.");
        QVERIFY2(CreatorsEnded(), "The fresh reader's context-creation thread did not end.");
        rows.push_back(QJsonObject{{"variant", variant}, {"retired", true}, {"creatorEnded", true},
                                   {"receiptsWhileHeld", receiptsBeforeRelease},
                                   {"freshReader", status.name}});
        qInfo().noquote() << "graph replay unadopted retirement:" << variant << "passed;" << status.name;
    }
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_GRAPH_TEST_EVIDENCE");
    if (!evidence.isEmpty()) QVERIFY(uct_batch_test::Save(evidence, QJsonObject{{"unadoptedRetirement", rows}}));
}

inline void Configurations()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_GRAPH_CONFIG_TEST") != "1")
        QSKIP("Explicit graph-replay configuration diagnostic not selected.");
    Images images;
    QVERIFY2(images.Load(), "OKUFLOW_UCT_TEST_IMAGE and OKUFLOW_UCT_TEST_IMAGE_2 must name two images of different sizes.");
    using Settings = std::vector<std::pair<QByteArray, QByteArray>>;
    struct Configuration {
        const char* name;
        Settings settings;
        bool fp32; // the pinned FP32 factory instead of the production (BF16) one
        ModelTier detector, recognizer;
        bool detail; // paused-detail reads instead of full reads
        bool fill{};  // full and detail reads with idle gaps: every slot of both engines in use
    };
    const std::vector<Configuration> all{
        {"medium-bf16-batch8", {}, false, ModelTier::Medium, ModelTier::Medium, false},
        {"medium-bf16-batch1", {{"OKUFLOW_UCT_TRT_BATCH", "1"}}, false, ModelTier::Medium, ModelTier::Medium, false},
        {"medium-bf16-batch16", {{"OKUFLOW_UCT_TRT_BATCH", "16"}}, false, ModelTier::Medium, ModelTier::Medium, false},
        {"medium-fp32-batch8", {}, true, ModelTier::Medium, ModelTier::Medium, false},
        {"small-bf16", {}, false, ModelTier::Small, ModelTier::Small, false},
        {"tiny-bf16", {}, false, ModelTier::Tiny, ModelTier::Tiny, false},
        {"medium-bucketed", {{"OKUFLOW_UCT_TRT_WIDTH", "bucketed"}, {"OKUFLOW_UCT_TRT_PREPARE_WIDTH", "1"}},
         false, ModelTier::Medium, ModelTier::Medium, false},
        {"medium-bf16-detail", {}, false, ModelTier::Medium, ModelTier::Medium, true},
        {"medium-bf16-fill", {}, false, ModelTier::Medium, ModelTier::Medium, true, true},
    };
    const auto selected = qEnvironmentVariable("OKUFLOW_UCT_GRAPH_CONFIGS").split(QLatin1Char(','), Qt::SkipEmptyParts);
    // Paused-detail focuses: other detector shapes and long edges.
    const auto focuses = [](const QImage& image) {
        const int w = image.width(), h = image.height();
        return std::vector<DetailFocus>{{QRect(0, 0, w / 2, h / 2), 2.0, 960, false},
                                        {QRect(w / 4, h / 4, w / 3, h / 3), 3.0, 704, false},
                                        {QRect(w / 2, h / 3, w / 2, h / 3), 1.5, 960, false},
                                        {QRect(0, h / 2, w / 4, h / 4), 4.0, 704, true}};
    };
    QJsonArray results;
    for (const auto& configuration : all) {
        if (!selected.isEmpty() && !selected.contains(QString::fromLatin1(configuration.name))) continue;
        std::vector<std::unique_ptr<uct_batch_test::Environment>> environment;
        for (const auto& [key, value] : configuration.settings)
            environment.push_back(std::make_unique<uct_batch_test::Environment>(key, value));
        // One read job: an image, and for detail reads a focus.
        struct Job { const QImage* image; std::optional<DetailFocus> focus; };
        std::vector<Job> jobs;
        if (configuration.fill) jobs = {{&images.a, {}}, {&images.b, {}}};
        if (configuration.detail) {
            for (const auto* image : {&images.a, &images.b})
                for (const auto& focus : focuses(*image)) jobs.push_back({image, focus});
            if (configuration.fill)
                for (const auto* image : {&images.a, &images.b})
                    for (const double scale : {0.6, 0.85})
                        jobs.push_back({image, DetailFocus{QRect(0, 0, int(image->width() * scale), int(image->height() * scale)),
                                                           1.0, 960, false}});
        } else jobs = {{&images.a, {}}, {&images.b, {}}};
        std::vector<QByteArray> reference(jobs.size());
        QJsonObject arms;
        for (const auto* arm : {"off", "graph", "graph-profiled"}) {
            uct_batch_test::Environment graph("OKUFLOW_UCT_TEST_TRT_GRAPH", QByteArray(arm) == "off" ? "0" : "1");
            uct_batch_test::Environment profiling("OKUFLOW_UCT_TRT_PROFILE", QByteArray(arm) == "graph-profiled" ? "1" : "0");
            qunsetenv("OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES");
            ProfileCapture capture;
            uct_batch_test::RetirementReceipts retirement;
            auto recognizer = configuration.fp32
                ? CreateTensorRtRecognizer({}, {}, configuration.detector, configuration.recognizer, retirement.Observer())
                : CreateProductionTensorRtRecognizer(configuration.detector, configuration.recognizer, retirement.Observer());
            QElapsedTimer preparation;
            preparation.start();
            QVERIFY2(recognizer && Ready(*recognizer),
                     qPrintable(QString(configuration.name) + ": " + (recognizer ? recognizer->Status().detail : QString())));
            const double preparationMs = preparation.nsecsElapsed() / 1e6;
            const auto detail = recognizer->Status().detail;
            QCOMPARE(detail.contains("graph replay"), QByteArray(arm) != "off");
            std::atomic<bool> never{false};
            // Every job: one first read, then four more; three rounds over all jobs.
            QJsonArray times;
            std::vector<QJsonObject> lastReceipts(jobs.size());
            qint64 evictionsBeforeLastRound = -1;
            for (int round = 0; round < 3; ++round) {
                if (round == 2) {
                    const auto receipt = LastReceipt(capture.Take());
                    evictionsBeforeLastRound = Sum(receipt, "evictions");
                }
                for (std::size_t index = 0; index < jobs.size(); ++index) {
                    const auto& job = jobs[index];
                    recognizer->SetDetailFocus(job.focus);
                    QJsonArray reads;
                    for (int pass = 0; pass < (round == 0 ? 5 : 2); ++pass) {
                        // Idle gaps, as the application paces its reads: contexts are made in them.
                        if (configuration.fill) QTest::qWait(300);
                        QElapsedTimer timer;
                        timer.start();
                        const auto output = Read(*recognizer, *job.image, never);
                        const double ms = timer.nsecsElapsed() / 1e6;
                        QVERIFY2(output.ok, qPrintable(QString(configuration.name) + "/" + arm + ": " + output.error));
                        const auto bytes = ExactOutputBytes(output);
                        if (reference[index].isEmpty()) reference[index] = bytes;
                        QVERIFY2(bytes == reference[index], qPrintable(QString(configuration.name) + "/" + arm +
                                 ": the complete output differs from the switch-off read."));
                        reads.push_back(ms);
                    }
                    times.push_back(QJsonObject{{"round", round}, {"job", int(index)}, {"ms", reads}});
                }
            }
            recognizer->SetDetailFocus(std::nullopt);
            // Free device memory after the same reads in every arm: the
            // difference to the switch-off arm is what replay keeps allocated.
            std::size_t freeBytes{}, totalBytes{};
            QVERIFY(cudaMemGetInfo(&freeBytes, &totalBytes) == cudaSuccess);
            QJsonObject row{{"preparationMs", preparationMs}, {"detail", detail}, {"reads", times},
                            {"freeBytesAfterReads", qint64(freeBytes)}};
            if (QByteArray(arm) == "graph-profiled") {
                const auto receipt = LastReceipt(capture.Take());
                QVERIFY2(!receipt.isEmpty(), "No graph receipt was emitted.");
                row["receipt"] = receipt;
                for (const char* engine : {"detector", "recognizer"}) {
                    const auto values = receipt[engine].toObject();
                    if (!values["enabled"].toBool()) continue;
                    QCOMPARE(values["refusals"].toInteger(), qint64(0));
                    QVERIFY(!values["captureBlocked"].toBool());
                }
                // No eviction thrash: the last round over the same jobs evicts nothing.
                if (!configuration.detail) QCOMPARE(Sum(receipt, "evictions"), evictionsBeforeLastRound);
                row["evictionsInLastRound"] = Sum(receipt, "evictions") - evictionsBeforeLastRound;
                if (QByteArray(configuration.name) == "medium-bucketed")
                    QVERIFY2(!receipt["recognizer"].toObject()["enabled"].toBool(),
                             "The bucketed recognizer width must run its recognizer plainly.");
            }
            recognizer.reset();
            QVERIFY2(retirement.Wait().Passed(), "TensorRT State retirement failed.");
            arms[arm] = row;
        }
        results.push_back(QJsonObject{{"configuration", configuration.name}, {"arms", arms},
            {"identicalOutput", true}, {"jobs", int(jobs.size())}});
        qInfo().noquote() << "graph replay configuration" << configuration.name << "identical";
    }
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_GRAPH_TEST_EVIDENCE");
    if (!evidence.isEmpty()) QVERIFY(uct_batch_test::Save(evidence, QJsonObject{{"configurations", results}}));
}
#else
inline void Skip(const char* gate)
{
    if (qEnvironmentVariable(gate) != "1") QSKIP("Explicit graph-replay diagnostic not selected.");
    QFAIL("The graph-replay diagnostics need the CUDA analysis pool, the TensorRT adapter and the fault hooks.");
}
inline void Faults() { Skip("OKUFLOW_UCT_GRAPH_FAULT_TEST"); }
inline void Checkpoints() { Skip("OKUFLOW_UCT_GRAPH_CHECKPOINT_TEST"); }
inline void RetireUnadopted() { Skip("OKUFLOW_UCT_GRAPH_RETIRE_TEST"); }
inline void Configurations() { Skip("OKUFLOW_UCT_GRAPH_CONFIG_TEST"); }
#endif
} // namespace okuflow::uct::uct_graph_faults_test
