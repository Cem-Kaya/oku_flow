#include <QtTest>
#include "openzoom/d3d12/fence_wait.hpp"
#include "openzoom/d3d12/frame_readiness.hpp"

using namespace openzoom;

class FenceWaitTests : public QObject {
    Q_OBJECT
private slots:
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
