#include "okuflow/common/uct_region_planner.hpp"
#include <QtTest>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

using namespace okuflow::uct;
namespace {
class FixedRecognizer final : public Recognizer {
public:
    BackendStatus Initialize() override { return {BackendState::kReady, "fixture", {}, {}}; }
    RecognitionOutput Recognize(const QImage&, const std::atomic<bool>&) override {
        RecognitionOutput result;
        result.ok = true;
        result.lines = {RecognizedLine{{RecognizedWord{QStringLiteral("Read"), {10, 10, 30, 12}}}}};
        return result;
    }
};
struct Mailbox {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<RecognitionResult> result;
    void Receive(RecognitionResult value) {
        std::lock_guard lock(mutex);
        result = std::move(value);
        ready.notify_one();
    }
    std::optional<RecognitionResult> Take() {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(5), [&]{return result.has_value();})) return {};
        return std::exchange(result, {});
    }
};
SourceStamp Stamp(std::uint64_t frame) {
    SourceStamp value;
    value.sourceEpoch = 1;
    value.geometryGeneration = 1;
    value.frameSeq = frame;
    value.timestampMs = qint64(frame)*100;
    value.liveCameraCapture = true;
    value.viewportWidth = 160;
    value.viewportHeight = 80;
    value.viewTransform = {0,0,1,1,0,0,1,1,true};
    return value;
}
QImage Pixels() {
    QImage result(160,80,QImage::Format_ARGB32);
    result.fill(Qt::white);
    return result;
}
// CPU fallback fixture implementing the capture-owned metadata protocol;
// no CUDA pointer or native device is used by FixedRecognizer.
class OwnedHeldInput final : public DeviceImage {
public:
    OwnedHeldInput() : pixels_(Pixels()) {
        width=160; height=80; pitch=640;
        sourceEpoch=1; frameSequence=2; processingGeneration=1;
    }
    const void* Data() const override { return pixels_.constBits(); }
    bool WaitReady(void*, QString&) const override { return true; }
    QImage Readback(const std::atomic<bool>&, QString&) const override { return pixels_; }
    bool HasHoldProvenance() const override { return true; }
    HoldProvenance ReadHoldProvenance(const std::atomic<bool>&, QString&) const override {
        ++recordReads;
        return {HoldDisplay::Held, 1, 0.0f};
    }
    mutable std::atomic<int> recordReads{};
private:
    const QImage pixels_;
};
}
class UctRegionSchedulerTests : public QObject {
    Q_OBJECT
private slots:
    void disabledObserverHasNoPlanOrOverhead()
    {
        Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<FixedRecognizer>(),
            [&](RecognitionResult value){box.Receive(std::move(value));}, {});
        QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
        scheduler.SetShadowFocus(QRectF(0,0,.5,.5));
        QVERIFY(scheduler.Submit(Stamp(1),Pixels()));
        auto value = box.Take();
        QVERIFY(value);
        QVERIFY(value->output.ok);
        QVERIFY(!value->shadowPlan);
        QCOMPARE(value->shadowPlannerMs, 0.0);
        QVERIFY(value->shadowPlannerError.isEmpty());
    }
    void enabledObserverPreservesFullOutputAndFocus()
    {
        Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<FixedRecognizer>(),
            [&](RecognitionResult value){box.Receive(std::move(value));}, {}, 2, 7, false, true);
        QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
        scheduler.SetShadowFocus(QRectF(0,0,.5,.5));
        QVERIFY(scheduler.Submit(Stamp(1),Pixels()));
        auto value = box.Take();
        QVERIFY(value && value->shadowPlan);
        QVERIFY(value->output.ok);
        QCOMPARE(value->output.lines.size(), 1);
        QCOMPARE(value->output.lines[0].words[0].text, QStringLiteral("Read"));
        QCOMPARE(value->output.lines[0].words[0].boundsPx, QRectF(10,10,30,12));
        QCOMPARE(value->shadowPlan->status, RegionPlanStatus::kAccepted);
        QCOMPARE(value->shadowPlan->authoritativeAction, RegionAuthoritativeAction::kRunFullOcr);
        QCOMPARE(value->shadowPlan->jobId, value->jobId);
        QCOMPARE(value->shadowPlan->authority, value->authority);
        QVERIFY(value->shadowPlan->focusApplied);
        QVERIFY(value->shadowPlannerMs >= 0);
        QVERIFY(value->shadowPlannerError.isEmpty());
        scheduler.SetShadowFocus({}); // priority update is not another job
        QCOMPARE(scheduler.Stats().submitted, std::uint64_t{1});
    }
    void probeOnlyWorkerCannotEnableObservation()
    {
        Mailbox box;
        RecognitionScheduler scheduler(nullptr,
            [&](RecognitionResult value){box.Receive(std::move(value));}, {}, 2, 7, true, true);
        QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
        QVERIFY(scheduler.Submit(Stamp(1),Pixels()));
        auto value = box.Take();
        QVERIFY(value && value->output.ok);
        QVERIFY(value->reference.IsValid());
        QVERIFY(!value->shadowPlan);
        QCOMPARE(value->shadowPlannerMs, 0.0);
    }
    void heldCopyDoesNotRenewShadowEvidence()
    {
        Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<FixedRecognizer>(),
            [&](RecognitionResult value){box.Receive(std::move(value));}, {}, 2, 7, false, true);
        QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
        QVERIFY(scheduler.Submit(Stamp(1),Pixels()));
        auto first = box.Take();
        QVERIFY(first && first->shadowPlan);
        auto held = Stamp(2);
        held.holdDisplay = HoldDisplay::Unknown; // only the owner's record can resolve it
        const auto owned = std::make_shared<OwnedHeldInput>();
        QVERIFY(scheduler.SubmitDevice(held,owned));
        auto second = box.Take();
        QVERIFY(second && second->shadowPlan);
        QVERIFY(second->output.ok);
        QCOMPARE(owned->recordReads.load(), 1);
        QCOMPARE(second->stamp.holdDisplay, HoldDisplay::Held);
        QCOMPARE(second->stamp.holdAcquisition, std::uint64_t{1});
        QCOMPARE(second->shadowPlan->rejectReason, RegionRejectReason::kDuplicateAcquisition);
        QCOMPARE(second->shadowPlan->coverage.acceptedObservations,
                 first->shadowPlan->coverage.acceptedObservations);
    }
    void unownedHeldClaimCannotAdvanceShadowEvidence()
    {
        Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<FixedRecognizer>(),
            [&](RecognitionResult value){box.Receive(std::move(value));}, {}, 2, 7, false, true);
        QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
        QVERIFY(scheduler.Submit(Stamp(1),Pixels()));
        auto first=box.Take();
        QVERIFY(first && first->shadowPlan);
        auto unowned=Stamp(2);
        unowned.holdDisplay=HoldDisplay::Held;
        unowned.holdAcquisition=1;
        QVERIFY(scheduler.Submit(unowned,Pixels()));
        auto rejected=box.Take();
        QVERIFY(rejected);
        QVERIFY(rejected->output.compositeInput);
        QVERIFY(!rejected->output.ok);
        QVERIFY(!rejected->shadowPlan);
        QCOMPARE(rejected->stamp.holdDisplay, HoldDisplay::Unknown);
        QVERIFY(scheduler.Submit(Stamp(3),Pixels()));
        auto next=box.Take();
        QVERIFY(next && next->shadowPlan);
        QCOMPARE(next->shadowPlan->status, RegionPlanStatus::kAccepted);
        QCOMPARE(next->shadowPlan->coverage.acceptedObservations, std::uint64_t{2});
    }
    void cancellationResetsTicketsForBackwardSeek()
    {
        Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<FixedRecognizer>(),
            [&](RecognitionResult value){box.Receive(std::move(value));}, {}, 2, 7, false, true);
        QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
        auto late = Stamp(200);
        late.sourceEpoch = 9;
        QVERIFY(scheduler.Submit(late,Pixels()));
        auto first = box.Take();
        QVERIFY(first && first->shadowPlan);
        scheduler.Cancel();
        QVERIFY(scheduler.Submit(Stamp(1),Pixels()));
        auto second = box.Take();
        QVERIFY(second && second->shadowPlan);
        QCOMPARE(second->shadowPlan->status, RegionPlanStatus::kAccepted);
        QVERIFY(second->shadowPlan->plannerEpoch > first->shadowPlan->plannerEpoch);
        QCOMPARE(second->shadowPlan->coverage.acceptedObservations, std::uint64_t{1});
        QCOMPARE(second->shadowPlan->authority, scheduler.Authority());
    }
    void observerFailureDoesNotFailOrChangeFullOcr()
    {
        Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<FixedRecognizer>(),
            [&](RecognitionResult value){box.Receive(std::move(value));}, {}, 2, 7, false, true);
        QTRY_COMPARE(scheduler.Status().state, BackendState::kReady);
        scheduler.FailNextShadowObservationForTest();
        QVERIFY(scheduler.Submit(Stamp(1),Pixels()));
        auto failedObserver = box.Take();
        QVERIFY(failedObserver);
        QVERIFY(failedObserver->output.ok);
        QVERIFY(!failedObserver->output.inputFault);
        QCOMPARE(failedObserver->output.lines[0].words[0].text, QStringLiteral("Read"));
        QVERIFY(!failedObserver->shadowPlan);
        QVERIFY(!failedObserver->shadowPlannerError.isEmpty());
        QVERIFY(scheduler.Submit(Stamp(2),Pixels()));
        auto next = box.Take();
        QVERIFY(next && next->output.ok);
        QCOMPARE(next->output.lines[0].words[0].text, QStringLiteral("Read"));
        QVERIFY(!next->shadowPlan); // failed observer retires; full OCR continues
        QVERIFY(next->shadowPlannerError.isEmpty());
    }
};
QTEST_GUILESS_MAIN(UctRegionSchedulerTests)
#include "uct_region_scheduler_tests.moc"
