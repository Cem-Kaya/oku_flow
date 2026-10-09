#include "openzoom/capture/capture_shutdown.hpp"

#include <QtTest/QtTest>
#include <atomic>
#include <future>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

class CaptureShutdownTests : public QObject {
    Q_OBJECT
private slots:
    void blockedReaderReturnsAtDeadlineAndOwnsItsState()
    {
        struct Session {
            openzoom::CaptureShutdown shutdown;
            std::atomic<bool> resourcesReleased{false};
        };
        auto session = std::make_shared<Session>();
        std::weak_ptr<Session> weak = session;
        std::promise<void> entered, unblock, finished;
        auto unblockFuture = unblock.get_future().share();
        auto finishedFuture = finished.get_future();
        std::thread coordinator([owner = session, &entered, unblockFuture, &finished] {
            entered.set_value();
            unblockFuture.wait(); // deterministic stalled ReadSample/Flush
            owner->shutdown.MarkQuiescent();
            if (owner->shutdown.WaitReleasePermission()) owner->resourcesReleased = true;
            owner->shutdown.MarkFinished();
            finished.set_value();
        });
        entered.get_future().wait();
        session->shutdown.Cancel();
        const auto start = std::chrono::steady_clock::now();
        const bool quiesced = session->shutdown.WaitQuiescent(start + 25ms);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        // Always unblock before assertions so a failure cannot leave test code
        // or stack-owned synchronization behind on a detached thread.
        session->shutdown.Abandon();
        const bool callbackDenied = !session->shutdown.TryEnterDelivery();
        session.reset();
        const bool ownerRetained = !weak.expired();
        auto inspect = weak.lock();
        unblock.set_value();
        finishedFuture.wait();
        coordinator.join();
        QVERIFY(!quiesced);
        QVERIFY(elapsed < 500ms);
        QVERIFY(callbackDenied);
        QVERIFY(ownerRetained);
        QVERIFY(!inspect->resourcesReleased.load());
    }

    void consumerReleasePrecedesProducerTeardown()
    {
        auto control = std::make_shared<openzoom::CaptureShutdown>();
        std::atomic<bool> consumerReleased{false}, producerReleased{false}, orderingCorrect{false};
        std::thread coordinator([&] {
            control->MarkQuiescent();
            if (control->WaitReleasePermission()) {
                orderingCorrect = consumerReleased.load();
                producerReleased = true;
            }
            control->MarkFinished();
        });
        control->Cancel();
        const bool quiesced = control->WaitQuiescent(std::chrono::steady_clock::now() + 1s);
        const bool retainedBeforeCallback = !producerReleased.load();
        consumerReleased = true;
        control->AllowRelease();
        const bool completed = control->WaitFinished(std::chrono::steady_clock::now() + 1s);
        coordinator.join();
        QVERIFY(quiesced);
        QVERIFY(retainedBeforeCallback);
        QVERIFY(completed);
        QVERIFY(orderingCorrect.load());
    }

    void cancellationDoesNotWaitOnEnteredCallbackAndNewSessionIsIndependent()
    {
        auto old = std::make_shared<openzoom::CaptureShutdown>();
        std::promise<void> entered, release;
        auto releaseFuture = release.get_future().share();
        std::thread worker([&] {
            auto delivery = old->TryEnterDelivery();
            entered.set_value();
            releaseFuture.wait(); // callback has independently owned target
        });
        entered.get_future().wait();
        const auto start = std::chrono::steady_clock::now();
        old->Cancel();
        old->MarkQuiescent();
        const bool entryDenied = !old->TryEnterDelivery();
        const bool incorrectlyQuiescent = old->WaitQuiescent(start + 25ms);
        auto fresh = std::make_shared<openzoom::CaptureShutdown>();
        const bool newEntryAllowed = bool(fresh->TryEnterDelivery());
        release.set_value();
        worker.join();
        QVERIFY(entryDenied);
        QVERIFY(!incorrectlyQuiescent);
        QVERIFY(newEntryAllowed);
        QVERIFY(old->WaitQuiescent(std::chrono::steady_clock::now() + 1s));
    }

    void stalledFinalReleaseSharesTheOriginalDeadline()
    {
        openzoom::CaptureShutdown control;
        std::promise<void> finishRelease;
        auto releaseFuture = finishRelease.get_future().share();
        std::thread coordinator([&] {
            control.MarkQuiescent();
            if (control.WaitReleasePermission()) releaseFuture.wait();
            control.MarkFinished();
        });
        const auto deadline = std::chrono::steady_clock::now() + 25ms;
        control.Cancel();
        const bool quiesced = control.WaitQuiescent(deadline);
        control.AllowRelease();
        const bool completed = control.WaitFinished(deadline);
        control.Abandon();
        finishRelease.set_value();
        coordinator.join();
        QVERIFY(quiesced);
        QVERIFY(!completed);
    }
};

QTEST_APPLESS_MAIN(CaptureShutdownTests)
#include "capture_shutdown_tests.moc"
