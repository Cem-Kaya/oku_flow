#include <QtTest>
#include "okuflow/d3d12/fence_wait.hpp"
#include "okuflow/d3d12/frame_readiness.hpp"
#include "okuflow/d3d12/shared_timeline.hpp"
#include "okuflow/d3d12/recording_slot_policy.hpp"
#include "okuflow/common/gpu_read_retirement.hpp"
#include <deque>
#include <random>
#include <set>

namespace {
// Independent execution model: signals replace the fence value, including
// lower values. A missing dependency therefore produces an observable rewind.
struct RewindFence {
    struct Operation { bool wait; std::uint64_t value; };
    std::deque<Operation> graphics, cuda;
    std::uint64_t completed{};
    bool monotonic{true};
    std::set<std::uint64_t> submittedValues;
    bool unique{true};
    void Signal(std::deque<Operation>& queue, std::uint64_t value) {
        unique = submittedValues.insert(value).second && unique;
        queue.push_back({false, value});
    }
    bool Step(std::deque<Operation>& queue) {
        if (queue.empty()) return false;
        const auto op = queue.front();
        if (op.wait && completed < op.value) return false;
        queue.pop_front();
        if (!op.wait) {
            monotonic = monotonic && op.value > completed;
            completed = op.value;
        }
        return true;
    }
};
}

using namespace okuflow;

class FenceWaitTests : public QObject {
    Q_OBJECT
private slots:
    void rejectedSampleRetainsSourceUntilConversionCompletes()
    {
        for (int dropPath = 0; dropPath < 2; ++dropPath) {
            auto source = std::make_shared<int>(dropPath);
            std::weak_ptr<int> sourceLifetime = source;
            auto retirement = std::make_shared<GpuReadRetirement>(source);
            std::weak_ptr<GpuReadRetirement> readerLifetime = retirement;
            retirement->Arm(retirement); // Before conversion submission.
            source.reset(); // Sample creation failure or encoder rejection.
            QCOMPARE(retirement->PollCompletion([] { return GpuReadCompletion::Pending; }),
                     GpuReadCompletion::Pending);
            QVERIFY(!sourceLifetime.expired());
            retirement.reset(); // No accepted sample/recorder owns the source.
            QVERIFY(!readerLifetime.expired());
            auto pendingReader = readerLifetime.lock();
            QCOMPARE(pendingReader->PollCompletion([] { return GpuReadCompletion::Complete; }),
                     GpuReadCompletion::Complete);
            QVERIFY(sourceLifetime.expired());
            int redundantPolls = 0;
            QCOMPARE(pendingReader->PollCompletion([&] {
                ++redundantPolls; return GpuReadCompletion::Unknown;
            }), GpuReadCompletion::Complete);
            QCOMPARE(redundantPolls, 0);
            pendingReader.reset();
            QVERIFY(readerLifetime.expired());
        }
    }

    void unknownConversionQuarantinesSourceAndStopsPolling()
    {
        auto source = std::make_shared<int>(1);
        std::weak_ptr<int> sourceLifetime = source;
        auto retirement = std::make_shared<GpuReadRetirement>(source);
        std::weak_ptr<GpuReadRetirement> readerLifetime = retirement;
        retirement->Arm(retirement);
        source.reset();
        int polls = 0;
        QCOMPARE(retirement->PollCompletion([&] {
            ++polls; return GpuReadCompletion::Unknown;
        }), GpuReadCompletion::Unknown);
        QCOMPARE(retirement->PollCompletion([&] {
            ++polls; return GpuReadCompletion::Complete;
        }), GpuReadCompletion::Unknown);
        QCOMPARE(polls, 1);
        retirement.reset();
        QVERIFY(!readerLifetime.expired());
        QVERIFY(!sourceLifetime.expired());
        // Deliberate process-lifetime quarantine; no synthetic completion.
    }

    void unsubmittedConversionHasNoSelfRetention()
    {
        auto source = std::make_shared<int>(1);
        std::weak_ptr<int> sourceLifetime = source;
        auto retirement = std::make_shared<GpuReadRetirement>(source);
        std::weak_ptr<GpuReadRetirement> readerLifetime = retirement;
        source.reset();
        retirement.reset(); // Failure before Arm/submission needs no quarantine.
        QVERIFY(sourceLifetime.expired());
        QVERIFY(readerLifetime.expired());
    }

    void executionModelDetectsAnUnorderedLateSignal()
    {
        RewindFence fence;
        fence.completed = 10;
        fence.Signal(fence.cuda, 11);
        fence.Signal(fence.graphics, 12);
        QVERIFY(fence.Step(fence.graphics));
        QVERIFY(fence.Step(fence.cuda));
        QCOMPARE(fence.completed, std::uint64_t{11});
        QVERIFY(!fence.monotonic);
        fence.Signal(fence.graphics, 11);
        QVERIFY(!fence.unique);
    }

    void skippedPresentResizeCannotOvertakeCuda()
    {
        SharedFenceTimeline timeline;
        RewindFence fence;
        for (int i = 0; i < 10; ++i) {
            const auto value = timeline.ReserveGraphics();
            timeline.GraphicsSubmitted(value);
            fence.Signal(fence.graphics, value);
            QVERIFY(fence.Step(fence.graphics));
        }
        const auto cuda = timeline.ReserveExternal();
        QCOMPARE(cuda, std::uint64_t{11});
        fence.Signal(fence.cuda, cuda); // Deliberately leave CUDA gated.
        QVERIFY(timeline.CommitExternal(cuda));
        // Busy presentation submits nothing. Resize must still depend on CUDA.
        QVERIFY(timeline.QueueDependency([&](auto value) {
            fence.graphics.push_back({true, value}); return true;
        }));
        const auto drain = timeline.ReserveGraphics();
        timeline.GraphicsSubmitted(drain);
        fence.Signal(fence.graphics, drain);
        QVERIFY(drain > cuda);
        QVERIFY(!fence.Step(fence.graphics));
        QCOMPARE(fence.completed, std::uint64_t{10});
        QVERIFY(fence.Step(fence.cuda));
        QVERIFY(fence.Step(fence.graphics));
        QVERIFY(fence.Step(fence.graphics));
        QCOMPARE(fence.completed, drain);
        QVERIFY(fence.monotonic && fence.unique);
    }

    void twoQueuesRemainOrderedAcrossMixedSubmissions()
    {
        SharedFenceTimeline timeline;
        RewindFence fence;
        std::mt19937 random(0x0c0f10u);
        for (int step = 0; step < 10000; ++step) {
            switch (random() % 8) {
            case 0: case 1: {
                const auto wait = timeline.LastGraphics();
                const auto signal = timeline.ReserveExternal();
                QVERIFY(wait < signal);
                if (random() % 5 == 0) {
                    QVERIFY(timeline.CancelExternal(signal));
                } else {
                    fence.cuda.push_back({true, wait});
                    fence.Signal(fence.cuda, signal);
                    QVERIFY(timeline.CommitExternal(signal));
                }
                break;
            }
            case 2: break; // Busy viewport admission: no work submitted.
            default: // Present, CPU fallback, clone, readback or resize drain.
                QVERIFY(timeline.QueueDependency([&](auto value) {
                    fence.graphics.push_back({true, value}); return true;
                }));
                const auto signal = timeline.ReserveGraphics();
                QVERIFY(signal > 0);
                fence.Signal(fence.graphics, signal);
                timeline.GraphicsSubmitted(signal);
                break;
            }
            // Adversarial scheduling lets either queue get ahead when legal.
            if (random() % 2) fence.Step(fence.graphics);
            if (random() % 2) fence.Step(fence.cuda);
            QVERIFY(fence.monotonic && fence.unique);
        }
        while (fence.Step(fence.graphics) || fence.Step(fence.cuda)) {}
        QVERIFY(fence.graphics.empty() && fence.cuda.empty());
        QVERIFY(fence.monotonic && fence.unique);
    }

    void pendingAndCancelledReservationsNeverBecomeWaitTargets()
    {
        SharedFenceTimeline timeline;
        const auto cancelled = timeline.ReserveExternal();
        QCOMPARE(timeline.ReserveExternal(), std::uint64_t{0});
        QCOMPARE(timeline.ReserveGraphics(), std::uint64_t{0});
        int waits = 0;
        const auto wait = [&](auto) { ++waits; return true; };
        QVERIFY(!timeline.QueueDependency(wait));
        QVERIFY(!timeline.CommitExternal(cancelled + 1));
        QVERIFY(!timeline.CancelExternal(cancelled + 1));
        QVERIFY(timeline.CancelExternal(cancelled));
        QVERIFY(timeline.QueueDependency(wait));
        QCOMPARE(waits, 0);
        const auto committed = timeline.ReserveExternal();
        QVERIFY(committed > cancelled);
        QVERIFY(timeline.CommitExternal(committed));
        QCOMPARE(timeline.ReserveGraphics(), std::uint64_t{0});
        QVERIFY(!timeline.CommitExternal(committed));
        QVERIFY(!timeline.CancelExternal(committed));
        std::uint64_t failedWaitValue = 0;
        QVERIFY(!timeline.QueueDependency([&](auto value) {
            failedWaitValue = value; ++waits; return false;
        }));
        QCOMPARE(failedWaitValue, committed);
        QCOMPARE(timeline.ReserveGraphics(), std::uint64_t{0});
        QVERIFY(timeline.QueueDependency(wait));
        QVERIFY(timeline.QueueDependency(wait));
        QCOMPARE(waits, 2); // Failed waits must retry; accepted waits are deduplicated.
    }

    void droppedRecordingLeaseDoesNotRetireProducer()
    {
        std::uint64_t completed = 9;
        bool lost = false;
        const auto poll = [&](bool leased, std::uint64_t producer) {
            return PollRecordingSlot(leased, producer, [&] { return completed; },
                                     [&] { return lost; });
        };
        QCOMPARE(poll(true, 10), FrameReadiness::Busy);
        // Original-clone failure and queue overflow both drop the consumer
        // lease; neither permits allocator reset, descriptor writes or eviction.
        QCOMPARE(poll(false, 10), FrameReadiness::Busy);
        completed = 10;
        QCOMPARE(poll(false, 10), FrameReadiness::Ready);
        QCOMPARE(poll(true, 10), FrameReadiness::Busy);
        QCOMPARE(poll(false, 0), FrameReadiness::Ready); // Pre-submit failure.
        QCOMPARE(poll(false, kRecordingProducerPending), FrameReadiness::Busy);
        completed = kRecordingProducerPending - 1;
        QCOMPARE(poll(false, kRecordingProducerPending), FrameReadiness::Busy);
        lost = true;
        QCOMPARE(poll(true, 10), FrameReadiness::DeviceLost);
        lost = false;
        completed = kRecordingProducerPending;
        QCOMPARE(poll(false, 10), FrameReadiness::DeviceLost);
        QCOMPARE(poll(true, kRecordingProducerPending), FrameReadiness::DeviceLost);
    }

    void busyFrameDoesNotConsumeLatencyAdmission()
    {
        int polls = 0;
        std::uint64_t completed = 9;
        const auto poll = [&] {
            return PollFrameReadiness(10, [&] { return completed; },
                [] { return false; }, [&] {
                    ++polls;
                    return FenceEventResult::Signaled;
                });
        };
        QCOMPARE(poll(), FrameReadiness::Busy);
        QCOMPARE(polls, 0);
        completed = 10;
        QCOMPARE(poll(), FrameReadiness::Ready);
        QCOMPARE(polls, 1);
    }

    void frameAdmissionBusyCanRecoverWithoutFault()
    {
        auto admission = FenceEventResult::TimedOut;
        const auto poll = [&] {
            return PollFrameReadiness(0, [] { return std::uint64_t{0}; },
                [] { return false; }, [&] { return admission; });
        };
        QCOMPARE(poll(), FrameReadiness::Busy);
        admission = FenceEventResult::Signaled;
        QCOMPARE(poll(), FrameReadiness::Ready);
        admission = FenceEventResult::Failed;
        QCOMPARE(poll(), FrameReadiness::WaitFailed);
    }

    void removedFrameNeverConsumesLatencyAdmission()
    {
        int polls = 0;
        const auto latency = [&] { ++polls; return FenceEventResult::Signaled; };
        QCOMPARE(PollFrameReadiness(0,
            [] { return std::numeric_limits<std::uint64_t>::max(); },
            [] { return false; }, latency), FrameReadiness::DeviceLost);
        QCOMPARE(PollFrameReadiness(0, [] { return std::uint64_t{0}; },
            [] { return true; }, latency), FrameReadiness::DeviceLost);
        QCOMPARE(polls, 0);
    }

    void removalDuringAdmissionPollIsTerminal()
    {
        bool lost = false;
        QCOMPARE(PollFrameReadiness(0, [] { return std::uint64_t{0}; },
            [&] { return lost; }, [&] {
                lost = true;
                return FenceEventResult::TimedOut;
            }), FrameReadiness::DeviceLost);
    }

    void completedFenceDoesNotWait()
    {
        int waits = 0;
        const auto result = WaitForFenceDeadline(10, 1000,
            [] { return std::uint64_t{10}; }, [] { return false; },
            [&](auto) { ++waits; return FenceEventResult::Signaled; },
            [] { return std::uint64_t{0}; });
        QCOMPARE(result, FenceWaitResult::Completed);
        QCOMPARE(waits, 0);
    }

    void deadlineNeverInventsCompletion()
    {
        std::uint64_t now = 0;
        std::uint64_t observed = 7;
        int waits = 0;
        const auto result = WaitForFenceDeadline(10, 125,
            [&] { return observed; }, [] { return false; },
            [&](auto ms) { now += ms; ++waits; return FenceEventResult::TimedOut; },
            [&] { return now; });
        QCOMPARE(result, FenceWaitResult::TimedOut);
        QCOMPARE(now, std::uint64_t{125});
        QCOMPARE(waits, 3);
        QCOMPARE(observed, std::uint64_t{7});
    }

    void spuriousEventCannotGrantResourceReuse()
    {
        std::uint64_t now = 0;
        const auto result = WaitForFenceDeadline(10, 100,
            [] { return std::uint64_t{9}; }, [] { return false; },
            [&](auto ms) { now += ms; return FenceEventResult::Signaled; },
            [&] { return now; });
        QCOMPARE(result, FenceWaitResult::TimedOut);
    }

    void removalSentinelIsFailureEvenAboveTarget()
    {
        const auto result = WaitForFenceDeadline(10, 1000,
            [] { return std::numeric_limits<std::uint64_t>::max(); },
            [] { return false; },
            [](auto) { return FenceEventResult::Signaled; },
            [] { return std::uint64_t{0}; });
        QCOMPARE(result, FenceWaitResult::DeviceLost);
    }

    void deviceLossDuringWaitStopsAtNextProbe()
    {
        std::uint64_t now = 0;
        bool lost = false;
        const auto result = WaitForFenceDeadline(10, 1000,
            [] { return std::uint64_t{9}; }, [&] { return lost; },
            [&](auto ms) { now += ms; lost = true; return FenceEventResult::TimedOut; },
            [&] { return now; });
        QCOMPARE(result, FenceWaitResult::DeviceLost);
        QCOMPARE(now, std::uint64_t{50});
    }

    void eventFailureDoesNotGrantCompletion()
    {
        const auto result = WaitForFenceDeadline(10, 1000,
            [] { return std::uint64_t{9}; }, [] { return false; },
            [](auto) { return FenceEventResult::Failed; },
            [] { return std::uint64_t{0}; });
        QCOMPARE(result, FenceWaitResult::WaitFailed);
    }

    void completionAfterWakeIsObserved()
    {
        std::uint64_t completed = 9;
        const auto result = WaitForFenceDeadline(10, 1000,
            [&] { return completed; }, [] { return false; },
            [&](auto) { completed = 10; return FenceEventResult::Signaled; },
            [] { return std::uint64_t{0}; });
        QCOMPARE(result, FenceWaitResult::Completed);
    }
};

QTEST_APPLESS_MAIN(FenceWaitTests)
#include "fence_wait_tests.moc"
