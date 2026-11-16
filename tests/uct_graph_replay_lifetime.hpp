#pragma once
// Opt-in GPU contract for the private graph-replay experiment
// (OKUFLOW_UCT_TEST_TRT_GRAPH, src/common/uct_graph_cache.hpp).
//
// Uses the already prepared production plans; it never prepares an engine.
// Reads one image with the switch off, then with one graph per engine (every
// shape change drains, reshapes and recaptures) and with the default cache,
// cancelling reads from another thread at spread delays so cancellation lands
// before, during and after captures, replays and evictions. Every completed
// read must reproduce the switch-off output byte for byte, a cancelled read
// must leave the engines ready, and the State must retire cleanly.
#include "uct_resident_diagnostic.hpp"

namespace okuflow::uct::uct_graph_replay_test {
#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT
inline void Lifetime()
{
    using namespace uct_resident_test;
    if (qEnvironmentVariable("OKUFLOW_UCT_GRAPH_LIFETIME_TEST") != "1")
        QSKIP("Explicit graph-replay lifetime diagnostic not selected.");
    const QImage image(qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE"));
    QVERIFY2(!image.isNull(), "OKUFLOW_UCT_TEST_IMAGE must name a readable image.");

    struct Arm { const char* graph; const char* shapes; };
    QByteArray reference;
    for (const Arm arm : {Arm{"0", nullptr}, Arm{"1", "1"}, Arm{"1", nullptr}}) {
        std::optional<uct_batch_test::Environment> graph, shapes;
        graph.emplace("OKUFLOW_UCT_TEST_TRT_GRAPH", arm.graph);
        if (arm.shapes) shapes.emplace("OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES", arm.shapes);
        else qunsetenv("OKUFLOW_UCT_TEST_TRT_GRAPH_SHAPES");
        uct_batch_test::RetirementReceipts retirement;
        auto recognizer = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium,
                                                             retirement.Observer());
        const auto initialized = recognizer->Initialize();
        QVERIFY2(initialized.state == BackendState::kReady, qPrintable(initialized.detail));
        QTRY_VERIFY_WITH_TIMEOUT(!recognizer->Preparing(), 180000);
        QVERIFY2(recognizer->Status().state == BackendState::kReady, qPrintable(recognizer->Status().detail));
        const bool replay = QByteArray(arm.graph) == "1";
        QCOMPARE(recognizer->Status().detail.contains("graph replay"), replay);
        std::atomic<bool> never{false};
        const auto check = [&](const RecognitionOutput& output) {
            QVERIFY2(output.ok, qPrintable(output.error));
            const auto bytes = ExactOutputBytes(output);
            if (reference.isEmpty()) reference = bytes;
            QVERIFY2(bytes == reference, "Graph replay changed the complete output.");
        };
        // Warm: plain enqueue, capture, replay for every shape of this image.
        std::int64_t readNs = 0;
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer timer;
            timer.start();
            check(recognizer->Recognize(image, never));
            readNs = timer.nsecsElapsed();
            if (QTest::currentTestFailed()) return;
        }
        if (!replay) continue;
        int cancelledReads = 0, completedReads = 0;
        for (int i = 0; i < 40; ++i) {
            std::atomic<bool> cancelled{false};
            // From before the first enqueue to past the end of a warm read.
            const auto delay = std::chrono::nanoseconds(readNs * i / 32);
            std::thread canceller([&] {
                std::this_thread::sleep_for(delay);
                cancelled = true;
            });
            const auto output = recognizer->Recognize(image, cancelled);
            canceller.join();
            if (output.cancelled) {
                ++cancelledReads;
                QVERIFY2(!output.ok && output.error.isEmpty(), qPrintable(output.error));
                QVERIFY2(recognizer->Status().state == BackendState::kReady, qPrintable(recognizer->Status().detail));
            } else {
                ++completedReads;
                check(output);
            }
            // The next read after any cancellation is complete and unchanged.
            check(recognizer->Recognize(image, never));
            if (QTest::currentTestFailed()) return;
        }
        qInfo() << "graph replay lifetime" << arm.graph << (arm.shapes ? arm.shapes : "default")
                << "cancelled" << cancelledReads << "completed" << completedReads;
        QVERIFY2(cancelledReads > 0 && completedReads > 0,
                 "Cancellation no longer spans a whole read; the delays need adjusting.");
        recognizer.reset();
        QVERIFY2(retirement.Wait().Passed(), "TensorRT State retirement with graph replay failed.");
    }
}
#else
inline void Lifetime()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_GRAPH_LIFETIME_TEST") != "1")
        QSKIP("Explicit graph-replay lifetime diagnostic not selected.");
    QFAIL("The graph-replay diagnostic needs the CUDA analysis pool and TensorRT adapter.");
}
#endif
} // namespace okuflow::uct::uct_graph_replay_test
