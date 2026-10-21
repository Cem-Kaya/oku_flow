#include "okuflow/common/uct_region_planner.hpp"
#include <QtTest>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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
    std::deque<RecognitionResult> results;
    void Receive(RecognitionResult value) {
        std::lock_guard lock(mutex);
        results.push_back(std::move(value));
        ready.notify_one();
    }
    std::optional<RecognitionResult> Take() {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(5), [&]{return !results.empty();})) return {};
        auto value = std::move(results.front()); results.pop_front(); return value;
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

struct ActiveCalls {
    std::atomic<int> full{}, detail{};
    std::atomic<bool> supported{true}, failDetail{}, ignoreDetail{};
    std::mutex mutex;
    std::condition_variable ready;
    bool blockInput{}, inputEntered{}, blockDetail{};
    QRgb lastPixel{};
    void Release() { std::lock_guard lock(mutex); blockInput = blockDetail = false; ready.notify_all(); }
};
class ActiveRecognizer final : public Recognizer {
public:
    explicit ActiveRecognizer(std::shared_ptr<ActiveCalls> calls) : calls_(std::move(calls)) {}
    BackendStatus Initialize() override {
        BackendStatus status; status.state=BackendState::kReady; status.detailFocus=calls_->supported; return status;
    }
    void SetDetailFocus(const std::optional<DetailFocus>& focus) override { focus_=focus; }
    RecognitionOutput Recognize(const QImage& image, const std::atomic<bool>& cancelled) override {
        RecognitionOutput output;
        if (focus_) ++calls_->detail; else ++calls_->full;
        {
            std::unique_lock lock(calls_->mutex);
            calls_->lastPixel = image.pixel(image.width()-1, image.height()-1);
            calls_->ready.wait(lock,[&]{return !focus_ || !calls_->blockDetail;});
        }
        if (focus_ && calls_->failDetail) { output.error="optional detail failure"; return output; }
        output.ok=!cancelled; output.cancelled=cancelled;
        QRectF word(20,20,50,18);
        if (focus_ && !calls_->ignoreDetail) {
            output.detailRegionPx=DetailRegion(image.size(),*focus_);
            word=QRectF(output.detailRegionPx.x()+12,output.detailRegionPx.y()+12,50,18);
        }
        output.lines={RecognizedLine{{RecognizedWord{QStringLiteral("Read"),word}}}};
        return output;
    }
private:
    std::shared_ptr<ActiveCalls> calls_;
    std::optional<DetailFocus> focus_;
};
QImage ActivePixels(std::uint64_t frame) {
    QImage image(960,540,QImage::Format_ARGB32); image.fill(Qt::white);
    image.setPixelColor(959,539,QColor(20,30,int(frame))); return image;
}
SourceStamp ActiveStamp(std::uint64_t frame) {
    auto stamp=Stamp(frame); stamp.viewportWidth=960; stamp.viewportHeight=540; return stamp;
}
RegionalOcrRequest Request(const RecognitionResult& full, int slackMs=5000) {
    return {full.shadowPlan,std::chrono::steady_clock::now()+std::chrono::milliseconds(slackMs),750};
}
class DeferredInput final : public DeviceImage {
public:
    DeferredInput(std::shared_ptr<ActiveCalls> calls, std::uint64_t frame, bool held=false)
        : calls_(std::move(calls)), pixels_(ActivePixels(frame)), held_(held) {
        width=960; height=540; pitch=width*4; sourceEpoch=1; frameSequence=frame;
    }
    const void* Data() const override { return pixels_.constBits(); }
    bool WaitReady(void*,QString&) const override { return true; }
    QImage Readback(const std::atomic<bool>&,QString&) const override { return pixels_; }
    bool HasHoldProvenance() const override { return true; }
    HoldProvenance ReadHoldProvenance(const std::atomic<bool>&,QString&) const override {
        std::unique_lock lock(calls_->mutex); calls_->inputEntered=true; calls_->ready.notify_all();
        calls_->ready.wait(lock,[&]{return !calls_->blockInput;});
        return held_ ? HoldProvenance{HoldDisplay::Held,1,0} : HoldProvenance{HoldDisplay::Live,frameSequence,0};
    }
private:
    std::shared_ptr<ActiveCalls> calls_;
    QImage pixels_;
    bool held_{};
};
struct ReleaseCalls { std::shared_ptr<ActiveCalls> calls; ~ReleaseCalls(){calls->Release();} };
}
class UctRegionSchedulerTests : public QObject {
    Q_OBJECT
private slots:
    void regionalDetectorBudgetKeepsPausedDefaults()
    {
        const QSize source(1920,1080);
        const DetailFocus paused{QRect(0,0,100,60),1};
        const DetailFocus regional{QRect(0,0,100,60),1,480,true};
        QCOMPARE(DetailRegion(source,paused).width(),960);
        QCOMPARE(DetailRegion(source,regional).width(),480);
        for (const QSize size:{QSize(480,480),QSize(480,270),QSize(479,33),QSize(1920,1080)}) {
            const auto budget=DetectorInputSize(size,1,480);
            QVERIFY(budget.width()<=480 && budget.height()<=480);
        }
        QCOMPARE(DetectorInputSize(source),QSize(960,544));
    }
    void activePlanRunsActualCurrentRegionAndDoesNotObservePartial()
    {
        auto calls=std::make_shared<ActiveCalls>(); Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<ActiveRecognizer>(calls),
            [&](auto value){box.Receive(std::move(value));},{},2,7,false,false,true);
        QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(scheduler.Submit(ActiveStamp(1),ActivePixels(1)));
        auto full=box.Take(); QVERIFY(full && full->shadowPlan);
        const auto observations=full->shadowPlan->coverage.acceptedObservations;
        QVERIFY(scheduler.Submit(ActiveStamp(2),ActivePixels(2),{}, {},Request(*full)));
        auto roi=box.Take(); QVERIFY(roi && roi->output.ok);
        QCOMPARE(roi->jobKind,RecognitionJobKind::Regional);
        QCOMPARE(calls->full.load(),1); QCOMPARE(calls->detail.load(),1);
        QCOMPARE(calls->lastPixel,ActivePixels(2).pixel(959,539)); // current owned image actually executed
        QVERIFY(!roi->shadowPlan);
        QVERIFY(roi->output.detailRegionPx.width()<=480 && roi->output.detailRegionPx.height()<=480);
        QCOMPARE(roi->stamp.frameSeq,std::uint64_t(2)); QCOMPARE(roi->reference.stamp.frameSeq,std::uint64_t(2));
        QVERIFY(scheduler.Submit(ActiveStamp(3),ActivePixels(3)));
        auto next=box.Take(); QVERIFY(next && next->shadowPlan);
        QCOMPARE(next->jobKind,RecognitionJobKind::Full);
        QCOMPARE(next->shadowPlan->coverage.acceptedObservations,observations+1);
        QCOMPARE(calls->full.load(),2); QCOMPARE(calls->detail.load(),1);
    }
    void activeOptOutAndProbeOnlyRejectRegionalDescriptors_data() {
        QTest::addColumn<bool>("probe"); QTest::newRow("opt-out")<<false; QTest::newRow("probe-only")<<true;
    }
    void activeOptOutAndProbeOnlyRejectRegionalDescriptors()
    {
        QFETCH(bool,probe); auto calls=std::make_shared<ActiveCalls>(); Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<ActiveRecognizer>(calls),
            [&](auto value){box.Receive(std::move(value));},{},2,7,probe,true,probe);
        QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(scheduler.Submit(ActiveStamp(1),ActivePixels(1)));
        auto full=box.Take(); QVERIFY(full);
        QVERIFY(!scheduler.Submit(ActiveStamp(2),ActivePixels(2),{},{},Request(*full)));
        QCOMPARE(calls->detail.load(),0); QCOMPARE(scheduler.Stats().submitted,std::uint64_t(1));
        if (probe) { QVERIFY(!full->shadowPlan); QCOMPARE(calls->full.load(),0); }
    }
    void activeInvalidTicketsAndUnsupportedFallBackToFull_data() {
        QTest::addColumn<int>("change");
        for(int value=0;value<8;++value) QTest::newRow(qPrintable(QString::number(value)))<<value;
    }
    void activeInvalidTicketsAndUnsupportedFallBackToFull()
    {
        QFETCH(int,change); auto calls=std::make_shared<ActiveCalls>(); Mailbox box;
        if(change==0) calls->supported=false;
        RecognitionScheduler scheduler(std::make_unique<ActiveRecognizer>(calls),
            [&](auto value){box.Receive(std::move(value));},{},2,7,false,false,true);
        QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(scheduler.Submit(ActiveStamp(2),ActivePixels(2))); auto full=box.Take(); QVERIFY(full && full->shadowPlan);
        auto stamp=ActiveStamp(3); auto request=Request(*full);
        if(change==1) ++stamp.sourceEpoch;
        if(change==2) ++stamp.geometryGeneration;
        if(change==3) scheduler.Cancel();
        if(change==4) stamp.frameSeq=1;
        if(change==5) stamp.timestampMs=1;
        if(change==6) request.fullDeadline=std::chrono::steady_clock::now()-std::chrono::milliseconds(1);
        if(change==7) request.plan=std::make_shared<const RegionShadowPlan>(*request.plan); // foreign/copy ticket
        QVERIFY(scheduler.Submit(stamp,ActivePixels(3),{},{},request));
        auto result=box.Take(); QVERIFY(result && result->output.ok && result->regionalFallback);
        QCOMPARE(result->jobKind,RecognitionJobKind::Full); QCOMPARE(calls->detail.load(),0); QCOMPARE(calls->full.load(),2);
    }
    void activeExecutionRechecksHeldInputAndSlack_data() {
        QTest::addColumn<bool>("held"); QTest::newRow("held-acquisition")<<true; QTest::newRow("expired-slack")<<false;
    }
    void activeExecutionRechecksHeldInputAndSlack()
    {
        QFETCH(bool,held); auto calls=std::make_shared<ActiveCalls>(); Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<ActiveRecognizer>(calls),
            [&](auto value){box.Receive(std::move(value));},{},2,7,false,false,true);
        ReleaseCalls release{calls};
        QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(scheduler.Submit(ActiveStamp(1),ActivePixels(1))); auto full=box.Take(); QVERIFY(full && full->shadowPlan);
        {std::lock_guard lock(calls->mutex); calls->blockInput=true;}
        auto request=Request(*full,1000);
        QVERIFY(scheduler.SubmitDevice(ActiveStamp(2),std::make_shared<DeferredInput>(calls,2,held),{},{},request));
        {std::unique_lock lock(calls->mutex); QVERIFY(calls->ready.wait_for(lock,std::chrono::seconds(2),[&]{return calls->inputEntered;}));}
        if(!held) QTest::qWait(200); // slack was sufficient at enqueue, not at actual execution
        calls->Release(); auto result=box.Take(); QVERIFY(result && result->output.ok && result->regionalFallback);
        QCOMPARE(calls->detail.load(),0); QCOMPARE(calls->full.load(),2);
        if(held) QCOMPARE(result->stamp.holdAcquisition,std::uint64_t(1));
    }
    void queuedFullReplacesRegionAndCannotBeOverwritten()
    {
        auto calls=std::make_shared<ActiveCalls>(); Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<ActiveRecognizer>(calls),
            [&](auto value){box.Receive(std::move(value));},{},2,7,false,false,true);
        ReleaseCalls release{calls}; QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(scheduler.Submit(ActiveStamp(1),ActivePixels(1))); auto full=box.Take(); QVERIFY(full && full->shadowPlan);
        {std::lock_guard lock(calls->mutex); calls->blockInput=true;}
        QVERIFY(scheduler.SubmitDevice(ActiveStamp(2),std::make_shared<DeferredInput>(calls,2),{},{},Request(*full)));
        {std::unique_lock lock(calls->mutex); QVERIFY(calls->ready.wait_for(lock,std::chrono::seconds(2),[&]{return calls->inputEntered;}));}
        QVERIFY(scheduler.Submit(ActiveStamp(3),ActivePixels(3),{},{},Request(*full))); // one pending ROI
        const auto dueFull=scheduler.Submit(ActiveStamp(4),ActivePixels(4)); QVERIFY(dueFull);
        for(int i=0;i<100;++i) QVERIFY(!scheduler.Submit(ActiveStamp(5+i),ActivePixels(5),{},{},Request(*full)));
        calls->Release(); auto yielded=box.Take(); auto result=box.Take();
        QVERIFY(yielded && yielded->output.cancelled && yielded->regionalFallback);
        QVERIFY(result && result->output.ok); QCOMPARE(result->jobId,dueFull);
        QCOMPARE(result->stamp.frameSeq,std::uint64_t(4)); QCOMPARE(calls->detail.load(),0);
        QCOMPARE(scheduler.Stats().started,std::uint64_t(3));
        QCOMPARE(scheduler.Stats().coalesced,std::uint64_t(1));
        QCOMPARE(scheduler.Stats().regionalPriorityRefusals,std::uint64_t(100));
    }
    void activeFailureRequestsFullWithoutFailingBackend()
    {
        auto calls=std::make_shared<ActiveCalls>(); Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<ActiveRecognizer>(calls),
            [&](auto value){box.Receive(std::move(value));},{},2,7,false,false,true);
        QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(scheduler.Submit(ActiveStamp(1),ActivePixels(1))); auto full=box.Take(); QVERIFY(full && full->shadowPlan);
        calls->failDetail=true;
        QVERIFY(scheduler.Submit(ActiveStamp(2),ActivePixels(2),{},{},Request(*full)));
        auto failed=box.Take(); QVERIFY(failed && failed->regionalFailed && !failed->output.ok);
        QCOMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(scheduler.Submit(ActiveStamp(3),ActivePixels(3))); auto fallback=box.Take(); QVERIFY(fallback && fallback->output.ok);
        QCOMPARE(calls->detail.load(),1); QCOMPARE(calls->full.load(),2);
        QCOMPARE(scheduler.Stats().regionalFailures,std::uint64_t(1));
    }
    void activeIgnoredDetailCountsActualFullFallback()
    {
        auto calls=std::make_shared<ActiveCalls>(); Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<ActiveRecognizer>(calls),
            [&](auto value){box.Receive(std::move(value));},{},2,7,false,false,true);
        QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(scheduler.Submit(ActiveStamp(1),ActivePixels(1))); auto full=box.Take(); QVERIFY(full && full->shadowPlan);
        calls->ignoreDetail=true;
        QVERIFY(scheduler.Submit(ActiveStamp(2),ActivePixels(2),{},{},Request(*full)));
        auto result=box.Take(); QVERIFY(result && result->output.ok && result->regionalFallback);
        QCOMPARE(result->jobKind,RecognitionJobKind::Full); QVERIFY(result->shadowPlan);
        const auto stats=scheduler.Stats(); QCOMPARE(stats.fullCalls,std::uint64_t(2));
        QCOMPARE(stats.regionalAttempts,std::uint64_t(1)); QCOMPARE(stats.regionalCalls,std::uint64_t(0));
    }
    void activeSlowRegionCountsDeadlineOverrun()
    {
        auto calls=std::make_shared<ActiveCalls>(); Mailbox box;
        RecognitionScheduler scheduler(std::make_unique<ActiveRecognizer>(calls),
            [&](auto value){box.Receive(std::move(value));},{},2,7,false,false,true);
        ReleaseCalls release{calls}; QTRY_COMPARE(scheduler.Status().state,BackendState::kReady);
        QVERIFY(scheduler.Submit(ActiveStamp(1),ActivePixels(1))); auto full=box.Take(); QVERIFY(full && full->shadowPlan);
        {std::lock_guard lock(calls->mutex); calls->blockDetail=true;}
        QVERIFY(scheduler.Submit(ActiveStamp(2),ActivePixels(2),{},{},Request(*full,1000)));
        QTRY_COMPARE(calls->detail.load(),1);
        QTest::qWait(1050); calls->Release();
        auto result=box.Take(); QVERIFY(result && result->regionalDeadlineOverrun);
        QCOMPARE(scheduler.Stats().regionalDeadlineOverruns,std::uint64_t(1));
        QVERIFY(scheduler.Stats().maximumRegionalMs>=1000);
    }
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
