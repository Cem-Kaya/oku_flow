#include "okuflow/capture/capture_shutdown.hpp"
#include "okuflow/capture/media_capture.hpp"
#include "okuflow/capture/phone_camera.hpp"
#include "../src/capture/media_capture_session.hpp"

#include <QtTest/QtTest>
#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace okuflow {
struct CaptureReadbackTestAccess {
    static std::shared_ptr<MediaCaptureSession> Session(MediaCapture& capture) { return capture.session_; }
    static void AttachSyntheticPhone(MediaCapture& capture) {
        capture.session_->phoneTransport_=std::make_shared<PhoneCameraTransport>();
        capture.session_->running_.store(true);
    }
    static void SetCaptureWorker(MediaCapture& capture, std::shared_future<void> release) {
        capture.session_->captureThread_ = std::thread([release] { release.wait(); });
    }
    static void HoldConsumer(MediaCapture& capture, const std::shared_ptr<void>& lease) {
        capture.session_->activeCudaLease_ = lease;
    }
    static void ExpireStop(MediaCapture& capture) { capture.session_->stopDeadline_ -= 2s; }
    static void SetDevice(MediaCaptureSession& session, ID3D11Device* device,
                          ID3D11DeviceContext* context) {
        session.d3d11Device_ = device;
        session.d3d11Context_ = context;
    }
    static void Expire(MediaCaptureSession& session) { session.gpuReadbackStarted_ -= 2s; }
    static std::mutex& Mutex(MediaCaptureSession& session) { return session.d3d11Mutex_; }
    static bool Retained(const MediaCaptureSession& session) {
        return session.gpuReadbackFrame_.has_value() && session.gpuReadbackTexture_ &&
               session.gpuReadbackQuery_ && session.gpuReadbackSubmitted_;
    }
};
}

namespace {
// Never leave a fixture worker blocked when a Qt assertion returns early.
struct StopWorkerGate {
    std::promise<void> release;
    std::shared_future<void> future{release.get_future().share()};
    bool released{};
    void Open() { if (!released) { released = true; release.set_value(); } }
    ~StopWorkerGate() { Open(); }
};
struct ReadbackFixture {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    okuflow::MediaCaptureSession session;
    okuflow::MediaFrame frame;
    bool Initialize() {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION,
            &device, nullptr, &context))) return false;
        okuflow::CaptureReadbackTestAccess::SetDevice(session, device.Get(), context.Get());
        D3D11_TEXTURE2D_DESC description{};
        description.Width = description.Height = 4;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        const std::uint32_t pixels[16] = {
            0xffcc2211, 0xffcc2211, 0xffcc2211, 0xffcc2211,
            0xffcc2211, 0xffcc2211, 0xffcc2211, 0xffcc2211,
            0xffcc2211, 0xffcc2211, 0xffcc2211, 0xffcc2211,
            0xffcc2211, 0xffcc2211, 0xffcc2211, 0xffcc2211};
        D3D11_SUBRESOURCE_DATA initial{pixels, 16, 0};
        if (FAILED(device->CreateTexture2D(&description, &initial, &frame.gpuTexture))) return false;
        frame.width = frame.height = 4;
        frame.gpuFormat = description.Format;
        frame.sequenceNumber = 42;
        return true;
    }
    ~ReadbackFixture() {
        // WARP fixture only. Retire submitted test work before inspecting
        // teardown; production performs this on its bounded stop coordinator.
        if (context) {
            Microsoft::WRL::ComPtr<ID3D11Query> query;
            D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
            if (SUCCEEDED(device->CreateQuery(&description, &query))) {
                context->End(query.Get());
                context->Flush();
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (context->GetData(query.Get(), nullptr, 0, 0) == S_FALSE &&
                       std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            }
        }
    }
};
}

class CaptureShutdownTests : public QObject {
    Q_OBJECT
private slots:
    void boundedPhoneTorchAdmissionEndsAtExactSplitStop() {
        okuflow::MediaCapture capture;
        const auto token=std::make_shared<std::atomic<bool>>(false);
        const auto deadline=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()+1000;
        QVERIFY(!capture.SetPhoneTorch(true,token,deadline));
        okuflow::CaptureReadbackTestAccess::AttachSyntheticPhone(capture);
        QVERIFY(capture.SetPhoneTorch(true,token,deadline));QVERIFY(!capture.PhoneTorchEnabled());
        capture.BeginStopCapture();QVERIFY(token->load());
        QVERIFY(!capture.SetPhoneTorch(true,std::make_shared<std::atomic<bool>>(false),deadline));
        QTRY_COMPARE_WITH_TIMEOUT(capture.PollStopCapture(),okuflow::CaptureStopStatus::ReadyToRelease,500);
        capture.ReleaseStoppedCapture(true);
        QTRY_COMPARE_WITH_TIMEOUT(capture.PollStopCapture(),okuflow::CaptureStopStatus::Completed,500);
    }
    void legacyStopRunsConsumerCallbackOnceWhileBackingIsRetained()
    {
        okuflow::MediaCapture capture;
        const auto session = okuflow::CaptureReadbackTestAccess::Session(capture);
        int calls = 0;
        bool backingRetained = false;
        QVERIFY(capture.StopCapture([&](bool quiescent) {
            ++calls;
            const auto state = session->shutdown_->Poll();
            backingRetained = quiescent && state.quiescent && !state.finished;
        }));
        QVERIFY(backingRetained);
        QCOMPARE(calls, 1);
        QVERIFY(capture.StopCapture([&](bool) { ++calls; }));
        QCOMPARE(calls, 1);
    }

    void legacyConsumerExceptionPreservesProducerBacking()
    {
        okuflow::MediaCapture capture;
        const auto session = okuflow::CaptureReadbackTestAccess::Session(capture);
        QVERIFY(!capture.StopCapture([](bool) { throw std::runtime_error("fixture consumer failure"); }));
        QVERIFY(capture.WasAbandoned());
        QVERIFY(session->retainedForExit_.load());
        QTRY_VERIFY_WITH_TIMEOUT(session->shutdown_->Poll().finished, 1000);
        QCOMPARE(capture.PollStopCapture(), okuflow::CaptureStopStatus::Failed);
    }

    void splitStopReturnsImmediatelyAndRevokesProducerAccess()
    {
        using Status = okuflow::CaptureStopStatus;
        okuflow::MediaCapture capture;
        StopWorkerGate gate;
        okuflow::CaptureReadbackTestAccess::SetCaptureWorker(capture, gate.future);
        const auto session = okuflow::CaptureReadbackTestAccess::Session(capture);
        const auto start = std::chrono::steady_clock::now();
        capture.BeginStopCapture();
        capture.BeginStopCapture(); // same session, no additional stop worker
        QCOMPARE(capture.PollStopCapture(), Status::WaitingForQuiescence);
        QVERIFY(std::chrono::steady_clock::now() - start < 100ms);
        QVERIFY(!session->shutdown_->TryEnterDelivery());
        QCOMPARE(capture.CurrentFrameRate(), 0.0);
        okuflow::MediaFrame frame;
        QCOMPARE(capture.ReadbackGpuFrame(frame), okuflow::GpuFrameReadbackResult::Failed);
        okuflow::CameraDescriptor next;
        next.symbolicLink = L"fixture-next-camera";
        QVERIFY(!capture.StartCapture(next, nullptr, {}));
        QVERIFY(capture.LastSymbolicLink().empty());
        gate.Open();
        QTRY_COMPARE_WITH_TIMEOUT(capture.PollStopCapture(), Status::ReadyToRelease, 1000);
        capture.ReleaseStoppedCapture(true);
        QTRY_COMPARE_WITH_TIMEOUT(capture.PollStopCapture(), Status::Completed, 1000);
        QVERIFY(!capture.WasAbandoned());
    }

    void splitStopPreservesBackingThroughExactConsumerLease()
    {
        using Status = okuflow::CaptureStopStatus;
        okuflow::MediaCapture capture;
        auto lease = std::make_shared<int>(42);
        okuflow::CaptureReadbackTestAccess::HoldConsumer(capture, lease);
        const auto session = okuflow::CaptureReadbackTestAccess::Session(capture);
        capture.BeginStopCapture();
        QTRY_COMPARE_WITH_TIMEOUT(capture.PollStopCapture(), Status::ReadyToRelease, 1000);
        QVERIFY(!session->shutdown_->Poll().finished);
        capture.ReleaseStoppedCapture(true);
        QCOMPARE(capture.PollStopCapture(), Status::WaitingForRelease);
        QVERIFY(!session->shutdown_->Poll().finished);
        lease.reset();
        QTRY_COMPARE_WITH_TIMEOUT(capture.PollStopCapture(), Status::Completed, 1000);
        capture.ReleaseStoppedCapture(false); // repeated acknowledgment has no effect
        QVERIFY(capture.StopCapture());
    }

    void splitStopDeadlineRetainsBlockedSessionAndCannotRestart()
    {
        using Status = okuflow::CaptureStopStatus;
        okuflow::MediaCapture capture;
        StopWorkerGate gate;
        okuflow::CaptureReadbackTestAccess::SetCaptureWorker(capture, gate.future);
        const auto session = okuflow::CaptureReadbackTestAccess::Session(capture);
        capture.BeginStopCapture();
        okuflow::CaptureReadbackTestAccess::ExpireStop(capture);
        const auto start = std::chrono::steady_clock::now();
        QCOMPARE(capture.PollStopCapture(), Status::Failed);
        QVERIFY(std::chrono::steady_clock::now() - start < 100ms);
        QVERIFY(capture.WasAbandoned());
        QVERIFY(session->retainedForExit_.load());
        capture.ReleaseStoppedCapture(true);
        QVERIFY(session->shutdown_->IsAbandoned());
        gate.Open();
        QTRY_VERIFY_WITH_TIMEOUT(session->shutdown_->Poll().finished, 1000);
        QCOMPARE(capture.PollStopCapture(), Status::Failed);
        okuflow::CameraDescriptor next;
        next.symbolicLink = L"fixture-next-camera";
        QVERIFY(!capture.StartCapture(next, nullptr, {}));
        QCOMPARE(okuflow::CaptureReadbackTestAccess::Session(capture).get(), session.get());
        QVERIFY(!capture.StopCapture());
    }

    void splitStopEarlyConsumerAcknowledgmentFailsClosed()
    {
        using Status = okuflow::CaptureStopStatus;
        okuflow::MediaCapture capture;
        StopWorkerGate gate;
        okuflow::CaptureReadbackTestAccess::SetCaptureWorker(capture, gate.future);
        const auto session = okuflow::CaptureReadbackTestAccess::Session(capture);
        capture.BeginStopCapture();
        capture.ReleaseStoppedCapture(true);
        QCOMPARE(capture.PollStopCapture(), Status::Failed);
        gate.Open();
        QTRY_VERIFY_WITH_TIMEOUT(session->shutdown_->Poll().finished, 1000);
        QVERIFY(session->shutdown_->IsAbandoned());
    }

    void splitStopSwapTransfersPendingSessionAndDeadline()
    {
        using Status = okuflow::CaptureStopStatus;
        okuflow::MediaCapture original, adopted;
        StopWorkerGate gate;
        okuflow::CaptureReadbackTestAccess::SetCaptureWorker(original, gate.future);
        const auto session = okuflow::CaptureReadbackTestAccess::Session(original);
        original.BeginStopCapture();
        adopted.Swap(original);
        QCOMPARE(okuflow::CaptureReadbackTestAccess::Session(adopted).get(), session.get());
        QCOMPARE(adopted.PollStopCapture(), Status::WaitingForQuiescence);
        gate.Open();
        QTRY_COMPARE_WITH_TIMEOUT(adopted.PollStopCapture(), Status::ReadyToRelease, 1000);
        adopted.ReleaseStoppedCapture(true);
        QTRY_COMPARE_WITH_TIMEOUT(adopted.PollStopCapture(), Status::Completed, 1000);
        QVERIFY(original.StopCapture());
    }

    void splitStopCompletionUsesWorkerTimestampRatherThanGuiPollTime()
    {
        using Status = okuflow::CaptureStopStatus;
        okuflow::MediaCapture capture;
        const auto session = okuflow::CaptureReadbackTestAccess::Session(capture);
        capture.BeginStopCapture();
        QTRY_COMPARE_WITH_TIMEOUT(capture.PollStopCapture(), Status::ReadyToRelease, 1000);
        capture.ReleaseStoppedCapture(true);
        QTRY_VERIFY_WITH_TIMEOUT(session->shutdown_->Poll().finished, 1000);
        // Simulate an event-loop delay after worker completion without sleeping.
        // Its deadline remains later than the actual worker-finished timestamp.
        session->stopDeadline_ = session->shutdown_->Poll().finishedAt;
        QCOMPARE(capture.PollStopCapture(), Status::Completed);
        QVERIFY(!capture.WasAbandoned());
    }

    void readbackQueuesOnceAndReturnsTheRetainedPixels()
    {
        using Result = okuflow::GpuFrameReadbackResult;
        ReadbackFixture fixture;
        QVERIFY(fixture.Initialize());
        QCOMPARE(fixture.session.ReadbackGpuFrame(fixture.frame), Result::Pending);
        QVERIFY(fixture.session.HasPendingGpuReadback());
        // Change the source after its first copy. Retrying must not copy again.
        const std::uint32_t replacement[16]{};
        fixture.context->UpdateSubresource(fixture.frame.gpuTexture.Get(), 0, nullptr, replacement, 16, 0);
        Result result = Result::Pending;
        QTRY_VERIFY_WITH_TIMEOUT((result = fixture.session.ReadbackGpuFrame(fixture.frame)) != Result::Pending, 1000);
        QCOMPARE(result, Result::Ready);
        QCOMPARE(fixture.frame.data.size(), std::size_t(64));
        QCOMPARE(fixture.frame.data[0], std::uint8_t(0x11));
        QCOMPARE(fixture.frame.data[1], std::uint8_t(0x22));
        QCOMPARE(fixture.frame.data[2], std::uint8_t(0xcc));
        QVERIFY(!fixture.session.HasPendingGpuReadback());
    }

    void expiredReadbackRetainsResourcesAndRequestsReconnect()
    {
        using Result = okuflow::GpuFrameReadbackResult;
        ReadbackFixture fixture;
        QVERIFY(fixture.Initialize());
        QCOMPARE(fixture.session.ReadbackGpuFrame(fixture.frame), Result::Pending);
        okuflow::CaptureReadbackTestAccess::Expire(fixture.session);
        QCOMPARE(fixture.session.ReadbackGpuFrame(fixture.frame), Result::Failed);
        QVERIFY(okuflow::CaptureReadbackTestAccess::Retained(fixture.session));
        QVERIFY(fixture.session.ConsumeDeviceLost());
        QVERIFY(!fixture.session.ConsumeDeviceLost());
        QCOMPARE(fixture.session.ReadbackGpuFrame(fixture.frame), Result::Failed);
    }

    void readbackDoesNotBlockOnCaptureMutex()
    {
        ReadbackFixture fixture;
        QVERIFY(fixture.Initialize());
        std::promise<void> locked, release;
        auto released = release.get_future();
        std::thread owner([&] {
            std::lock_guard lock(okuflow::CaptureReadbackTestAccess::Mutex(fixture.session));
            locked.set_value();
            released.wait();
        });
        locked.get_future().wait();
        const auto start = std::chrono::steady_clock::now();
        const auto result = fixture.session.ReadbackGpuFrame(fixture.frame);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        release.set_value();
        owner.join();
        QCOMPARE(result, okuflow::GpuFrameReadbackResult::Pending);
        QVERIFY(elapsed < 100ms);
    }

    void readbackRejectsAnotherFrameWhileCopyIsPending()
    {
        ReadbackFixture fixture;
        QVERIFY(fixture.Initialize());
        QCOMPARE(fixture.session.ReadbackGpuFrame(fixture.frame), okuflow::GpuFrameReadbackResult::Pending);
        ++fixture.frame.sequenceNumber;
        QCOMPARE(fixture.session.ReadbackGpuFrame(fixture.frame), okuflow::GpuFrameReadbackResult::Failed);
        QVERIFY(okuflow::CaptureReadbackTestAccess::Retained(fixture.session));
    }

    void savedCameraSelectionFollowsPhysicalIdentity()
    {
        const auto camera = [](const wchar_t* link) {
            okuflow::CameraDescriptor descriptor;
            descriptor.name = L"Identical friendly name";
            descriptor.symbolicLink = link;
            return descriptor;
        };
        const auto a = camera(L"fixture-A");
        const auto b = camera(L"fixture-B");
        const auto c = camera(L"fixture-C");
        using okuflow::ResolveSavedCameraIndex;
        QCOMPARE(ResolveSavedCameraIndex({a, b}, L"fixture-B", 1), 1);
        QCOMPARE(ResolveSavedCameraIndex({b, a}, L"FIXTURE-b", 1), 0);
        QCOMPARE(ResolveSavedCameraIndex({c, a, b}, L"fixture-B", 1), 2);
        QCOMPARE(ResolveSavedCameraIndex({b, c}, L"fixture-B", 1), 0);
        QCOMPARE(ResolveSavedCameraIndex({a, c}, L"fixture-B", 1), 0);
        QCOMPARE(ResolveSavedCameraIndex({a, b}, L"", 1), 1);
        QCOMPARE(ResolveSavedCameraIndex({a, b}, L"", -1), 0);
        QCOMPARE(ResolveSavedCameraIndex({a, b}, L"", 2), 0);
        QCOMPARE(ResolveSavedCameraIndex({}, L"fixture-B", 1), -1);
    }

    void failedCameraSelectionRetainsRequestedRetryIdentity()
    {
        // Invalid activation is a deterministic failed open, without touching
        // a real camera. Retry identity must follow each request, not success.
        okuflow::MediaCapture capture;
        okuflow::CameraDescriptor first;
        first.symbolicLink = L"fixture-camera-A";
        QVERIFY(!capture.StartCapture(first, nullptr, {}));
        QVERIFY(capture.LastSymbolicLink() == first.symbolicLink);
        QVERIFY(capture.StopCapture());
        okuflow::CameraDescriptor second;
        second.symbolicLink = L"fixture-camera-B";
        QVERIFY(!capture.StartCapture(second, nullptr, {}));
        QVERIFY(capture.LastSymbolicLink() == second.symbolicLink);
        QVERIFY(capture.StopCapture());
        QVERIFY(capture.LastSymbolicLink() == second.symbolicLink);
    }

    void blockedReaderReturnsAtDeadlineAndOwnsItsState()
    {
        struct Session {
            okuflow::CaptureShutdown shutdown;
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
        auto control = std::make_shared<okuflow::CaptureShutdown>();
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
        auto old = std::make_shared<okuflow::CaptureShutdown>();
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
        auto fresh = std::make_shared<okuflow::CaptureShutdown>();
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
        okuflow::CaptureShutdown control;
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

    void boundedPollCompletesAfterPendingPolls()
    {
        okuflow::CaptureShutdown control;
        int calls = 0;
        const auto outcome = okuflow::PollWithDeadline(
            [&] { return ++calls >= 3; }, control,
            std::chrono::steady_clock::now() + 1s);
        QCOMPARE(outcome, okuflow::BoundedPollOutcome::Completed);
        QCOMPARE(calls, 3);
    }

    void boundedPollTimesOutInsteadOfSpinningForever()
    {
        okuflow::CaptureShutdown control;
        int calls = 0;
        const auto started = std::chrono::steady_clock::now();
        const auto outcome = okuflow::PollWithDeadline(
            [&] { ++calls; return false; }, control, started + 25ms);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        QCOMPARE(outcome, okuflow::BoundedPollOutcome::TimedOut);
        QVERIFY(calls > 1);
        QVERIFY(elapsed >= 25ms);
        QVERIFY(elapsed < 1s);
    }

    void boundedPollStopsWhenSessionIsAbandoned()
    {
        okuflow::CaptureShutdown control;
        const auto started = std::chrono::steady_clock::now();
        const auto outcome = okuflow::PollWithDeadline(
            [&] { control.Abandon(); return false; }, control, started + 60s);
        QCOMPARE(outcome, okuflow::BoundedPollOutcome::Abandoned);
        QVERIFY(std::chrono::steady_clock::now() - started < 1s);
    }
};

QTEST_APPLESS_MAIN(CaptureShutdownTests)
#include "capture_shutdown_tests.moc"
