// Selected-media controller contract. Qt offscreen only: no camera, desktop
// input, decoder, native OCR runtime or downloaded sample is required.

#include "okuflow/app/ultra_clear_text_controller.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include "ultra_clear_text_test_support.hpp"

#include <QAccessible>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPlainTextEdit>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QUuid>
#include <QtTest>
#include <QCheckBox>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>

using namespace okuflow;
using namespace okuflow::uct;
using namespace uct_test;

namespace okuflow {
// Narrow fixture access: real scheduler barriers and controller receipts,
// without adding a production enqueue/result-injection API.
struct UctControllerTestAccess {
    static void Barrier(UltraClearTextController& c, std::function<void()> barrier) {
        c.scheduler_->SetBeforeDequeueForTest(std::move(barrier));
    }
    static void SuppressRegions(UltraClearTextController& c, bool suppress) { c.regionalSuppressed_ = suppress; }
    static std::uint64_t RegionalJob(const UltraClearTextController& c) { return c.regionalJob_; }
    static std::uint64_t ProbeFrame(const UltraClearTextController& c) { return c.latestProbe_ ? c.latestProbe_->stamp.frameSeq : 0; }
    static SourceStamp Stamp(const UltraClearTextController& c) {
        return c.CurrentScene();
    }
    static std::uint64_t Authority(const UltraClearTextController& c) { return c.scheduler_->Authority(); }
    static void Deliver(UltraClearTextController& c, RecognitionResult result, bool armRegion = false) {
        if (armRegion) { c.regionalJob_ = result.jobId; c.regionalSubmittedAtMs_ = c.clock_(); }
        c.HandleResult(std::move(result));
    }
    static void Validate(UltraClearTextController& c, const uct::ProbeFrame& probe) {
        c.model_.Validate(probe,c.clock_()); c.latestProbe_ = std::make_shared<uct::ProbeFrame>(probe);
    }
};
}

namespace {

struct DequeueBarrier {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> entered{};
    bool released{};
    void Wait() {
        std::unique_lock lock(mutex); entered = true;
        changed.wait(lock,[&] { return released; });
    }
    void Release() {
        { std::lock_guard lock(mutex); released = true; }
        changed.notify_all();
    }
};

struct RegionalFlag {
    bool existed{qEnvironmentVariableIsSet("OKUFLOW_UCT_REGIONAL_OCR")};
    QByteArray previous{qgetenv("OKUFLOW_UCT_REGIONAL_OCR")};
    explicit RegionalFlag(bool enabled) { qputenv("OKUFLOW_UCT_REGIONAL_OCR",enabled ? "1" : "0"); }
    ~RegionalFlag() { if(existed) qputenv("OKUFLOW_UCT_REGIONAL_OCR",previous); else qunsetenv("OKUFLOW_UCT_REGIONAL_OCR"); }
};

struct AccessibilityActivation {
    bool previous{QAccessible::isActive()};
    AccessibilityActivation() { QAccessible::setActive(true); }
    ~AccessibilityActivation() { QAccessible::setActive(previous); }
};

struct ImmutableScript {
    std::mutex mutex;
    std::condition_variable changed;
    bool holdInitialization{};
    bool holdRecognition{};
    int holdCall{};
    int permittedCalls{-1}; // -1: unlimited; otherwise hold later calls deterministically
    QVector<PageWord> words;
    QHash<QByteArray, QVector<PageWord>> pixelWords;
    QHash<quint64, QVector<PageWord>> frameWords;
    QHash<int, QVector<PageWord>> callWords;
    std::atomic<int> initialized{};
    std::atomic<int> calls{};
    std::atomic<int> deviceCalls{};
    std::atomic<int> transientFailures{};
    int transientKind{};
    std::atomic<int> completeBridgeResults{};
    std::atomic<int> unavailableAtCall{};
    std::atomic<bool> wantsDevice{};
    std::atomic<int> wantsDeviceAfterCalls{}; // actual CPU bridge -> preferred device handoff
    std::atomic<bool> separateWordLines{};
    // Visible-detail support: the backend advertises region detection, and
    // `detailWords` are found only when a region is examined (small print the
    // full-scene detector missed). Every recognition records its focus.
    std::atomic<bool> detailFocus{};
    std::atomic<bool> ignoreDetail{};
    QVector<PageWord> detailWords;
    struct Call { QByteArray digest; quint64 frame{}; std::optional<DetailFocus> focus; int marker{}; };
    QVector<Call> recorded;

    void Release()
    {
        {
            std::lock_guard lock(mutex);
            holdInitialization = false;
            holdRecognition = false;
            holdCall = 0;
            permittedCalls = -1;
        }
        changed.notify_all();
    }
};

class ImmutableRecognizer final : public Recognizer {
public:
    explicit ImmutableRecognizer(std::shared_ptr<ImmutableScript> script) : script_(std::move(script)) {}

    BackendStatus Initialize() override
    {
        std::unique_lock lock(script_->mutex);
        ++script_->initialized;
        script_->changed.wait(lock, [&] { return !script_->holdInitialization; });
        BackendStatus status;
        status.state = BackendState::kReady;
        status.name = QStringLiteral("immutable fixture");
        status.detailFocus = script_->detailFocus.load();
        return status;
    }

    void SetDetailFocus(const std::optional<DetailFocus>& focus) override { focus_ = focus; }

    bool WantsDeviceInput() const override
    {
        const auto after = script_->wantsDeviceAfterCalls.load();
        return script_->wantsDevice.load() || (after > 0 &&
            script_->calls.load() + script_->deviceCalls.load() >= after);
    }

    BackendStatus Status() const override
    {
        if (script_->unavailableAtCall.load() > 0 &&
            script_->calls.load() + script_->deviceCalls.load() >= script_->unavailableAtCall.load()) {
            BackendStatus status;
            status.state = BackendState::kUnavailable;
            status.name = QStringLiteral("immutable fixture");
            status.detail = QStringLiteral("fixture backend chain exhausted");
            return status;
        }
        return {};
    }

    RecognitionOutput Recognize(const QImage& image, const std::atomic<bool>& cancelled) override
    {
        ++script_->calls;
        return Produce(image.width(), image.height(), cancelled, 0, SourcePixelsDigest(image),
                       image.pixelColor(image.width()-1,image.height()-1).blue());
    }

    RecognitionOutput RecognizeDevice(const DeviceImage& image, const std::atomic<bool>& cancelled) override
    {
        ++script_->deviceCalls;
        return Produce(image.width, image.height, cancelled, image.frameSequence);
    }

private:
    RecognitionOutput Produce(int width, int height, const std::atomic<bool>& cancelled,
                              quint64 frame = 0, const QByteArray& digest = {}, int marker = 0)
    {
        QVector<PageWord> words;
        QRect detail;
        {
            std::unique_lock lock(script_->mutex);
            words = script_->words;
            if (script_->pixelWords.contains(digest)) words = script_->pixelWords[digest];
            if (script_->frameWords.contains(frame)) words = script_->frameWords[frame];
            const int call = script_->calls.load() + script_->deviceCalls.load();
            if (script_->callWords.contains(call)) words = script_->callWords[call];
            script_->recorded.push_back({digest, frame, focus_, marker});
            if (focus_ && !script_->ignoreDetail) {
                // One region of the same image: only words inside it, plus
                // the small print a closer detector look reveals.
                detail = DetailRegion(QSize(width, height), *focus_);
                words += script_->detailWords;
                QVector<PageWord> inside;
                for (const PageWord& word : words)
                    if (QRectF(detail).contains(QRectF(word.sceneRect.x() * width, word.sceneRect.y() * height,
                                                       word.sceneRect.width() * width, word.sceneRect.height() * height)))
                        inside.push_back(word);
                words = inside;
            }
            script_->changed.wait(lock, [&] {
                return script_->permittedCalls < 0 || call <= script_->permittedCalls;
            });
            if (script_->holdCall && script_->calls.load() == script_->holdCall)
                script_->changed.wait(lock, [&] { return script_->holdCall == 0; });
            script_->changed.wait(lock, [&] { return !script_->holdRecognition; });
        }
        RecognitionOutput output;
        if (script_->transientFailures.load() > 0 && script_->transientFailures.fetch_sub(1) > 0) {
            output.inputFault = script_->transientKind == 0;
            output.backendPreparing = script_->transientKind == 1;
            output.cancelled = script_->transientKind == 2;
            output.error = QStringLiteral("fixture recognition failure");
            return output;
        }
        output.cancelled = cancelled.load();
        output.ok = !output.cancelled;
        if (!output.ok) return output;
        output.detailRegionPx = detail;
        if(script_->completeBridgeResults.load()>0 && script_->completeBridgeResults.fetch_sub(1)>0)
            output.backendPreparing=true;
        RecognizedLine line;
        for (const PageWord& word : words) {
            RecognizedWord recognized{word.text,
                QRectF(word.sceneRect.x() * width, word.sceneRect.y() * height,
                       word.sceneRect.width() * width, word.sceneRect.height() * height)};
            if (script_->separateWordLines.load()) {
                RecognizedLine separate;
                separate.words.push_back(std::move(recognized));
                output.lines.push_back(std::move(separate));
            } else line.words.push_back(std::move(recognized));
        }
        if (!line.words.isEmpty()) output.lines.push_back(std::move(line));
        return output;
    }

    std::shared_ptr<ImmutableScript> script_;
    std::optional<DetailFocus> focus_; // worker-confined, like production backends
};

SharedPixels OwnedPixels(const QImage& image)
{
    auto pixels = std::make_shared<PixelBuffer>();
    pixels->width = image.width();
    pixels->height = image.height();
    pixels->strideBytes = static_cast<int>(image.bytesPerLine());
    pixels->bytes.resize(static_cast<std::size_t>(image.sizeInBytes()));
    std::memcpy(pixels->bytes.data(), image.constBits(), pixels->bytes.size());
    return pixels;
}

// Independently owned fake GPU scene. Readback/ReadProbe stand in for worker
// copies and record any accidental UI-thread pixel access.
struct FakeDeviceState {
    std::thread::id uiThread{std::this_thread::get_id()};
    std::atomic<int> readbacks{};
    std::atomic<int> probes{};
    std::atomic<bool> readOnUiThread{};
    std::atomic<bool> probeFails{};
    std::atomic<bool> resident{};
    std::atomic<int> residentPreparations{};
    std::atomic<bool> failWordReferences{};
};

// CPU oracle for the resident controller protocol, not a GPU implementation.
// Separate CUDA fixtures verify production kernels and no-image-egress behavior.
class ResidentOracle final : public ResidentProbe {
public:
    ProbeFrame cpu;std::shared_ptr<FakeDeviceState> state;
    int Width() const noexcept override{return cpu.Width();}
    int Height() const noexcept override{return cpu.Height();}
    void Note() const {if(std::this_thread::get_id()==state->uiThread)state->readOnUiThread=true;}
    ReferencePatch Reference(const SourceStamp& s,const QRectF& r) const override {
        Note();auto p=cpu;p.stamp=s;return BuildReferencePatch(p,r);
    }
    QVector<ReferencePatch> References(const SourceStamp& s,const QVector<QRectF>& rects) const override {
        Note();
        if(state->failWordReferences && rects.size()>1)return {};
        return ResidentProbe::References(s,rects);
    }
    PatchMatch Match(const SourceStamp& s,const ReferencePatch& r,const MatchTuning& t,std::optional<QPointF> seed) const override {
        Note();auto p=cpu;p.stamp=s;return MatchPatch(r,p,t,seed);
    }
    std::optional<double> Similarity(const SourceStamp& a,const ResidentProbe& other,const SourceStamp& b) const override {
        Note();auto* oracle=dynamic_cast<const ResidentOracle*>(&other);if(!oracle)return {};
        auto x=cpu,y=oracle->cpu;x.stamp=a;y.stamp=b;return SceneSimilarity(x,y);
    }
};

class FakeDeviceImage final : public DeviceImage {
public:
    FakeDeviceImage(std::shared_ptr<FakeDeviceState> state, QImage page,
                    std::uint64_t epoch, std::uint64_t frame, std::int64_t ptsMs,
                    std::optional<HoldProvenance> hold = {})
        : state_(std::move(state)), page_(std::move(page)), hold_(hold)
    {
        width = page_.width();
        height = page_.height();
        pitch = static_cast<std::size_t>(page_.bytesPerLine());
        sourceEpoch = epoch;
        frameSequence = frame;
        timestampMs = ptsMs;
    }
    const void* Data() const override { return this; }
    bool WaitReady(void*, QString&) const override { return true; }
    bool HasHoldProvenance() const override { return hold_.has_value(); }
    HoldProvenance ReadHoldProvenance(const std::atomic<bool>&, QString&) const override {
        NoteThread(); return hold_.value_or(HoldProvenance{HoldDisplay::Unknown, 0, 0.0f});
    }
    bool HasResidentAnalysis() const override {return state_->resident.load();}
    bool PrepareResidentAnalysis(RecognitionResult& result,int decimation,bool probeOnly,
                                  const std::atomic<bool>& cancel,QString&) const override {
        NoteThread();if(cancel.load())return false;
        auto p=std::make_shared<ResidentOracle>();p->state=state_;
        p->cpu=MakeProbeFrame(page_.constBits(),width,height,page_.bytesPerLine(),std::max(1,decimation),result.stamp);
        result.reference.stamp=result.stamp;result.reference.resident=p;
        if(!probeOnly) {
            ++state_->residentPreparations;
            if(!result.stamp.liveCameraCapture) {
                // A protocol oracle: stable owned-image bytes model the
                // canonical resident digest across CPU/GPU OCR phases.
                // Separate CUDA fixtures validate the actual tree algorithm.
                result.sourcePixelDigest=SourcePixelsDigest(page_);
                result.sourcePixelDigestKind=RecognitionResult::DigestKind::GpuTreeSha256V1;
            }
        }
        return true;
    }
    QImage Readback(const std::atomic<bool>&, QString&) const override
    {
        ++state_->readbacks;
        NoteThread();
        return page_;
    }
    QImage ReadProbe(const std::atomic<bool>&, QString& error) const override
    {
        ++state_->probes;
        NoteThread();
        if (state_->probeFails.load()) {
            error = QStringLiteral("fixture probe failure");
            return {};
        }
        return page_.scaled(width / 2, height / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

private:
    void NoteThread() const
    {
        if (std::this_thread::get_id() == state_->uiThread) state_->readOnUiThread = true;
    }
    std::shared_ptr<FakeDeviceState> state_;
    QImage page_;
    const std::optional<HoldProvenance> hold_;
};

struct ImmutableHarness {
    std::shared_ptr<ImmutableScript> script = std::make_shared<ImmutableScript>();
    std::shared_ptr<FakeDeviceState> devices = std::make_shared<FakeDeviceState>();
    std::int64_t now{3600000};
    QWidget owner;
    QWidget target{&owner};
    UltraClearTextOverlay overlay{&target, &owner};
    UltraClearTextPanel panel{&owner};
    SyntheticPage page = MakePage({QStringLiteral("Reading helps")}, {}, 960, 540, 48);
    std::unique_ptr<UltraClearTextController> controller;

    explicit ImmutableHarness(UctTiming timing = {})
    {
        target.resize(960, 540);
        script->words = page.words;
        auto shared = script;
        controller = std::make_unique<UltraClearTextController>(
            &overlay, &panel, [shared] { return std::make_unique<ImmutableRecognizer>(shared); },
            nullptr, [this] { return now; }, timing);
    }

    ~ImmutableHarness()
    {
        script->Release();
        controller.reset();
    }

    UctImmutableScene Scene(std::uint64_t frame = 10) const
    {
        UctImmutableScene scene;
        scene.geometry.settings.sourceEpoch = 41;
        scene.geometry.sceneWidth = page.image.width();
        scene.geometry.sceneHeight = page.image.height();
        scene.sourceAuthority = 3;
        scene.selected = {OwnedPixels(page.image), frame, 1}; // genuinely hour-old source
        scene.transform.valid = true;
        scene.viewportWidth = 960;
        scene.viewportHeight = 540;
        return scene;
    }

    void Enable()
    {
        controller->SetEnabled(true);
    }

    UctAdvancingMediaScene PlayFrame(std::uint64_t frame, std::uint64_t authority = 3)
    {
        UctAdvancingMediaScene scene;
        scene.geometry.settings.sourceEpoch = 41;
        scene.geometry.sceneWidth = page.image.width();
        scene.geometry.sceneHeight = page.image.height();
        scene.sourceAuthority = authority;
        scene.current = {OwnedPixels(IndependentFramePixels(page.image, frame)), frame,
                         10000 + static_cast<std::int64_t>(frame) * 20};
        scene.transform.valid = true;
        scene.viewportWidth = 960;
        scene.viewportHeight = 540;
        {
            std::lock_guard lock(script->mutex);
            script->words = page.words;
        }
        now += 20; // wall/job time advances independently from the real media PTS
        controller->OnAdvancingMediaPresented(scene);
        QTest::qWait(3);
        return scene;
    }

    SharedDeviceImage Device(const QImage& image, std::uint64_t frame, std::int64_t ptsMs,
                             std::uint64_t epoch = 41) const
    {
        return std::make_shared<FakeDeviceImage>(devices, image, epoch, frame, ptsMs);
    }

    // Device-only selected still: no CPU pixels are retained at all.
    UctImmutableScene DeviceScene(std::uint64_t frame = 10) const
    {
        auto scene = Scene(frame);
        scene.selected = {{}, frame, 1, Device(page.image, frame, 1)};
        return scene;
    }

    UctAdvancingMediaScene PlayDeviceFrame(std::uint64_t frame, std::uint64_t authority = 3)
    {
        UctAdvancingMediaScene scene;
        scene.geometry.settings.sourceEpoch = 41;
        scene.geometry.sceneWidth = page.image.width();
        scene.geometry.sceneHeight = page.image.height();
        scene.sourceAuthority = authority;
        const std::int64_t pts = 10000 + static_cast<std::int64_t>(frame) * 20;
        scene.current = {{}, frame, pts,
                         Device(IndependentFramePixels(page.image, frame), frame, pts)};
        scene.transform.valid = true;
        scene.viewportWidth = 960;
        scene.viewportHeight = 540;
        {
            std::lock_guard lock(script->mutex);
            script->words = page.words;
        }
        now += 20;
        controller->OnAdvancingMediaPresented(scene);
        QTest::qWait(5);
        return scene;
    }

    // No-analysis display receipt for `frame`: real media PTS, no pixels.
    UctAdvancingMediaPresentation GapReceipt(std::uint64_t frame, std::uint64_t authority = 3) const
    {
        UctAdvancingMediaPresentation receipt;
        receipt.geometry.settings.sourceEpoch = 41;
        receipt.geometry.sceneWidth = page.image.width();
        receipt.geometry.sceneHeight = page.image.height();
        receipt.sourceAuthority = authority;
        receipt.frameSeq = frame;
        receipt.timestampMs = 10000 + static_cast<std::int64_t>(frame) * 20;
        receipt.transform.valid = true;
        receipt.viewportWidth = 960;
        receipt.viewportHeight = 540;
        return receipt;
    }

    bool PresentGap(std::uint64_t frame, std::uint64_t authority = 3, std::int64_t wallAdvanceMs = 20)
    {
        now += wallAdvanceMs;
        return controller->OnAdvancingMediaPresentedWithoutAnalysis(GapReceipt(frame, authority));
    }

    // Plays real device frames from `first` until glyphs are drawn, then a
    // few more so validation is current. Returns the last played frame.
    std::uint64_t EstablishDeviceGlyphs(std::uint64_t first = 1, std::uint64_t authority = 3)
    {
        std::uint64_t frame = first;
        for (; frame < first + 240 && overlay.WordCount() == 0; ++frame)
            PlayDeviceFrame(frame, authority);
        for (int i = 0; i < 8; ++i) PlayDeviceFrame(frame++, authority);
        QTest::qWait(10);
        return frame - 1;
    }

    QString Reading() const { return panel.readingPane()->toPlainText(); }
};

} // namespace

class UltraClearTextControllerTests final : public QObject {
    Q_OBJECT
private slots:
    void activeRegionalFullDeadlinesUnderContinuousFocus();
    void activeRegionalFailureKeepsBackendAndRequestsFull();
    void activeIgnoredRegionalFallbackDoesNotForceExtraFull();
    void activeRegionalResetsFenceLateCompletion_data();
    void activeRegionalResetsFenceLateCompletion();
    void activeRegionalPreservesPausedDetailPair();
    void activeQueuedRegionalIsSupersededByFull();
    void activeRegionalWithdrawalDoesNotDemandExtraFull();
    void timingNonimmutableMutationKeepsCausalTickets();
    void timingTraceKeepsWorkerAndAdmissionTickets();
    void timingPaneOnlyChangePreservesPositionedTicket();
    void textClarityMasterPreservesUctPreference();
    void liveCameraIdenticalPixelsAcquireText_data();
    void liveCameraIdenticalPixelsAcquireText();
    void residentSelectedNeighbourUsesWorkerRegistration();
    void pausedScrollingTextUsesSelectedFrameCoordinates_data();
    void pausedScrollingTextUsesSelectedFrameCoordinates();
    void pausedWordRegistrationFaultCannotPublishPartialProof();
    void immutableOldFrameAndIdleDoNoPeriodicWork();
    void backendReadySubmitsWaitingMediaOnce();
    void latePausedNeighbourReusesSelectedEvidence_data();
    void latePausedNeighbourReusesSelectedEvidence();
    void latePausedNeighbourPreservesSelectedDeadline();
    void coveredZoomReprojectsTheSameArtifact();
    void identicalPixelsNeverCorroborate();
    void changingAuthorityAndClearingFenceLateResults();
    void queuedCompletionCannotCrossSourceAuthority();
    void modelReplacementRevokesSelectedEvidence();
    void immutableDeadlineIsIndependentOfSourceAge();
    void invalidSelectedMediaDoesNotResumeHiddenLiveInput();
    void advancingMediaPacesOcrAndValidatesLatestFrames();
    void partialProbeInvalidationRequestsPromptReplacement_data();
    void partialProbeInvalidationRequestsPromptReplacement();
    void advancingMediaValidatesWhileOcrHeldAndRejectsChangedPixels();
    void advancingMediaAuthorityRevokesAndSuppressesLiveInput();
    void advancingMediaPauseReturnsToOneShotAnalysis();
    void pausedMediaRecoversTransientRecognition_data();
    void pausedMediaRecoversTransientRecognition();
    void pausedMediaRetryCannotReviveReplacedScene();
    void pausedBridgeRefinesAfterCorroboration();
    void pausedRefinementCannotDropProvenWords_data();
    void pausedRefinementCannotDropProvenWords();
    void pausedRefinementContradictionCannotRestorePriorProof_data();
    void pausedRefinementContradictionCannotRestorePriorProof();
    void pausedBridgeHorizonKeepsHealthyFallback();
    void pausedFailureRetriesAreBounded();
    void pausedRefinementFailureKeepsOnlyProvenArtifact_data();
    void pausedRefinementFailureKeepsOnlyProvenArtifact();
    void pausedReadingOnlyCandidateStillFailsClosed();
    void failedPausedRefinementStillFencesSceneChanges_data();
    void failedPausedRefinementStillFencesSceneChanges();
    void advancingMediaDeadlineUsesWallClock();
    void mediaPresentationFailureRevokesCachedArtifact_data();
    void mediaPresentationFailureRevokesCachedArtifact();
    void immutablePresentationFailureBlocksBackendReadiness();
    void advancingPresentationFailureBlocksLateResult();
    void deviceImmutableRecognizesWithoutCpuPixels();
    void deviceImmutableDistinctCorroborationDrawsGlyphs();
    void deviceImmutableIdenticalPixelsNeverCorroborate();
    void deviceImmutableRejectsMismatchedImageAndFallsBackToPixels();
    void deviceAdvancingMediaPacesAndValidates();
    void deviceAdvancingSeekAndBackwardFrameRevoke();
    void deviceAdvancingSlowJobUsesWallDeadline();
    void deviceAdvancingProbeFailureRecovers();
    void deviceAdvancingReleasesConsumedAnalysisLease();
    void gapReceiptRetainsBrieflyThenMediaExpiryHidesAndCurrentInputRecovers();
    void gapReceiptWallExpiryHidesDespiteFreshMediaPts();
    void gapReceiptDiscontinuitiesRevokeImmediately();
    void gapReceiptLateOldRecognitionCannotReviveAfterExpiry();
    void gapReceiptSameFrameReprojectsRepeatedlyWithoutWork();
    void processingCorrespondenceGateRevokesTextAndUiaWithoutChangingPreference();
    void pausedDetailSettledWhileBusyUsesLatestView();
    void pausedDetailFailureIsFiniteAndOtherRegionCanProceed();
    void pausedDetailSettledDuringPresentationFailureRunsAfterRecovery();
    void pausedDetailHeldCopiesNeverCorroborate();
    void pausedDetailAuthorityRevokesHeldWork_data();
    void pausedDetailAuthorityRevokesHeldWork();
    void pausedZoomRequestsOneBoundedVisibleDetailPair();
    void pausedVisibleDetailNeedsSupportNeighbourAndMagnification_data();
    void pausedVisibleDetailNeedsSupportNeighbourAndMagnification();
};

void UltraClearTextControllerTests::activeRegionalFullDeadlinesUnderContinuousFocus()
{
    QVector<int> baseline, active;
    for (const bool enabled : {false,true}) {
        RegionalFlag flag(enabled); ImmutableHarness h;
        h.script->detailFocus=true; h.script->separateWordLines=true;
        h.page=MakeLayoutPage({{"Reading",{70,70},36},{"Stable",{700,70},36}},960,540);
        h.Enable(); QTRY_COMPARE(h.controller->BackendStatusSnapshot().state,BackendState::kReady);
        for (std::uint64_t frame=1;frame<=64;++frame) {
            h.now+=60; h.PlayFrame(frame); // 80 ms wall / 20 ms genuine media PTS
            h.controller->UpdateImmutableView(ComputeViewTransform(960,540,960,540,2,
                frame%2 ? .2f : .8f,.3f,ViewportFitMode::kFill),960,540);
            QTRY_VERIFY(h.controller->ProbeStatistics().completed>=frame);
            QTRY_COMPARE(h.controller->SchedulerStatistics().completed,h.controller->SchedulerStatistics().submitted);
            QTest::qWait(3); // consume queued worker/UI callback before moving the fixture wall clock
        }
        QVector<int> full, regions;
        { std::lock_guard lock(h.script->mutex);
          for (const auto& call:h.script->recorded) {
              if(call.focus) {
                  regions.push_back(call.marker);
                  QCOMPARE(call.focus->detectorLongEdge,704);
                  QCOMPARE(call.focus->maximumMagnification,
                      std::nextafter(704.0/480.0,std::numeric_limits<double>::infinity()));
                  QVERIFY(call.focus->supplemental);
                  const auto region=DetailRegion(h.page.image.size(),*call.focus);
                  QVERIFY(region.width()<=480 && region.height()<=480);
                  const auto input=DetectorInputSize(region.size(),DetailMagnification(*call.focus),call.focus->detectorLongEdge);
                  QVERIFY(input.width()<=704 && input.height()<=704);
              } else full.push_back(call.marker);
          }
        }
        QVERIFY(full.size()>=4);
        for(int i=2;i<full.size();++i) QVERIFY((full[i]-full[i-1])*80<=1580);
        if(enabled) {
            active=full; QVERIFY(!regions.isEmpty());
            QCOMPARE(h.controller->SchedulerStatistics().regionalCalls,std::uint64_t(regions.size()));
            for(int i=0;i+1<full.size();++i)
                QVERIFY(std::count_if(regions.begin(),regions.end(),[&](int frame){return frame>full[i] && frame<full[i+1];})<=1);
        } else { baseline=full; QVERIFY(regions.isEmpty()); }
        QCOMPARE(h.controller->SchedulerStatistics().coalesced,std::uint64_t(0));
        QCOMPARE(h.controller->PixelAllocations(),std::uint64_t(0));
    }
    QCOMPARE(active.size(),baseline.size());
    for(int i=0;i<active.size();++i) QVERIFY(std::abs(active[i]-baseline[i])<=1);
}

void UltraClearTextControllerTests::activeRegionalFailureKeepsBackendAndRequestsFull()
{
    RegionalFlag flag(true); ImmutableHarness h; h.script->detailFocus=true; h.Enable();
    std::uint64_t frame=1;
    for(;frame<100 && h.overlay.WordCount()==0;++frame) { h.now+=60; h.PlayFrame(frame); QTest::qWait(5); }
    QVERIFY(h.overlay.WordCount()>0);
    const auto full=h.controller->SchedulerStatistics().fullCalls;
    h.script->transientKind=3; h.script->transientFailures=1;
    for(;frame<130 && h.controller->SchedulerStatistics().regionalFailures==0;++frame) {
        h.now+=60; h.PlayFrame(frame); QTest::qWait(5);
    }
    QCOMPARE(h.controller->SchedulerStatistics().regionalFailures,std::uint64_t(1));
    QCOMPARE(h.controller->BackendStatusSnapshot().state,BackendState::kReady);
    QVERIFY(h.overlay.WordCount()>0);
    for(int i=0;i<24;++i) {h.now+=60;h.PlayFrame(frame++);QTest::qWait(5);}
    QVERIFY(h.controller->SchedulerStatistics().fullCalls>full);
    QCOMPARE(h.controller->SchedulerStatistics().regionalAttempts,std::uint64_t(1)); // failed optional work latches off
}

void UltraClearTextControllerTests::activeIgnoredRegionalFallbackDoesNotForceExtraFull()
{
    RegionalFlag flag(true); ImmutableHarness h; h.script->detailFocus=true;h.script->ignoreDetail=true;h.Enable();
    std::uint64_t frame=1;
    for(;frame<100 && h.controller->SchedulerStatistics().regionalAttempts==0;++frame) {
        h.now+=60;h.PlayFrame(frame);QTest::qWait(5);
    }
    QTRY_COMPARE(h.controller->SchedulerStatistics().regionalAttempts,std::uint64_t(1));
    QTRY_COMPARE(h.controller->SchedulerStatistics().completed,h.controller->SchedulerStatistics().submitted);
    QTest::qWait(5);
    const auto full=h.controller->SchedulerStatistics().fullCalls;
    QCOMPARE(h.controller->SchedulerStatistics().regionalCalls,std::uint64_t(0));
    QCOMPARE(h.controller->SchedulerStatistics().regionalFallbacks,std::uint64_t(1));
    for(int i=0;i<5;++i) {h.now+=60;h.PlayFrame(frame++);QTest::qWait(5);}
    QCOMPARE(h.controller->SchedulerStatistics().fullCalls,full);
}

void UltraClearTextControllerTests::activeRegionalResetsFenceLateCompletion_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("authority")<<0; QTest::newRow("backward-seek")<<1;
    QTest::newRow("source")<<2; QTest::newRow("processing")<<3;
}
void UltraClearTextControllerTests::activeRegionalResetsFenceLateCompletion()
{
    QFETCH(int,change); RegionalFlag flag(true); ImmutableHarness h; h.script->detailFocus=true; h.Enable();
    std::uint64_t frame=1;
    for(;frame<100 && h.overlay.WordCount()==0;++frame) { h.now+=60; h.PlayFrame(frame); QTest::qWait(5); }
    QVERIFY(h.overlay.WordCount()>0);
    {std::lock_guard lock(h.script->mutex); h.script->permittedCalls=h.script->calls.load();}
    for(;frame<130 && h.controller->SchedulerStatistics().regionalAttempts==0;++frame) {h.now+=60;h.PlayFrame(frame);}
    QCOMPARE(h.controller->SchedulerStatistics().regionalAttempts,std::uint64_t(1));
    auto changed=h.Scene(frame+1);
    changed.selected.timestampMs=12000;
    if(change==0) ++changed.sourceAuthority;
    if(change==1) {changed.selected.frameSeq=1;changed.selected.timestampMs=1;}
    if(change==2) ++changed.geometry.settings.sourceEpoch;
    if(change==3) changed.geometry.settings.rotationQuarterTurns=1;
    // Enter a real paused selection after the source/seek/processing change;
    // its mandatory first selected full read may queue, never an old ROI.
    QVERIFY(h.controller->SetImmutableScene(changed));
    QCOMPARE(h.overlay.WordCount(),0);
    const auto before=h.script->calls.load();
    h.script->Release(); QTRY_VERIFY(h.script->calls.load()>before);
    QTRY_COMPARE(h.controller->SchedulerStatistics().completed,h.controller->SchedulerStatistics().submitted-1);
    QTest::qWait(10);
    QCOMPARE(h.overlay.WordCount(),0); // selected without real neighbour is reading-only
    std::lock_guard lock(h.script->mutex);
    QVERIFY(!h.script->recorded.last().focus);
    QCOMPARE(h.controller->BackendStatusSnapshot().state,BackendState::kReady);
}

void UltraClearTextControllerTests::activeRegionalPreservesPausedDetailPair()
{
    RegionalFlag flag(true);
    pausedZoomRequestsOneBoundedVisibleDetailPair(); // production selected + real-neighbour pair assertions
}

void UltraClearTextControllerTests::activeQueuedRegionalIsSupersededByFull()
{
    RegionalFlag flag(true); ImmutableHarness h;
    h.script->detailFocus = true; h.script->separateWordLines = true;
    h.page = MakeLayoutPage({{"Reading",{70,70},36},{"Outside",{700,300},36}},960,540);
    h.Enable(); QTRY_COMPARE(h.controller->BackendStatusSnapshot().state,BackendState::kReady);
    UctControllerTestAccess::SuppressRegions(*h.controller,true);
    QSignalSpy admitted(h.controller.get(),&UltraClearTextController::recognitionAdmitted);
    // Fixed, individually completed acquisitions establish the ordinary two
    // full observations; no polling loop resubmits an input until it passes.
    for (std::uint64_t frame = 1; frame <= 8; ++frame) {
        h.now += 60; h.PlayFrame(frame);
        QTRY_COMPARE(UctControllerTestAccess::ProbeFrame(*h.controller),frame);
        QTRY_COMPARE(admitted.count(),int(h.controller->SchedulerStatistics().submitted));
    }
    QVERIFY(h.overlay.WordCount() > 0);
    auto first = std::make_shared<DequeueBarrier>();
    auto second = std::make_shared<DequeueBarrier>();
    const auto release = qScopeGuard([&] { first->Release(); second->Release(); });
    UctControllerTestAccess::Barrier(*h.controller,[first] { first->Wait(); });
    UctControllerTestAccess::SuppressRegions(*h.controller,false);
    const auto before = h.controller->SchedulerStatistics();
    const int admissions = admitted.count();
    h.now += 60; h.PlayFrame(9);
    QTRY_VERIFY(first->entered.load());
    QTRY_COMPARE(UctControllerTestAccess::ProbeFrame(*h.controller),std::uint64_t(9));
    const auto oldRegion = UctControllerTestAccess::RegionalJob(*h.controller);
    QVERIFY(oldRegion);
    QCOMPARE(h.controller->SchedulerStatistics().started,before.started); // R really is still queued
    h.now += 1500; h.PlayFrame(10); // unchanged full refresh deadline outranks queued R
    QTRY_COMPARE(UctControllerTestAccess::ProbeFrame(*h.controller),std::uint64_t(10));
    QCOMPARE(h.controller->SchedulerStatistics().coalesced,before.coalesced + 1);
    QCOMPARE(UctControllerTestAccess::RegionalJob(*h.controller),std::uint64_t(0));
    first->Release();
    QTRY_COMPARE(admitted.count(),admissions + 1); // F completes; there is deliberately no R completion
    QCOMPARE(h.controller->SchedulerStatistics().fullCalls,before.fullCalls + 1);
    UctControllerTestAccess::Barrier(*h.controller,[second] { second->Wait(); });
    h.now += 60; h.PlayFrame(11);
    QTRY_VERIFY(second->entered.load());
    const auto newRegion = UctControllerTestAccess::RegionalJob(*h.controller);
    QVERIFY(newRegion > oldRegion);
    RecognitionResult stale;
    stale.jobId = oldRegion; stale.authority = UctControllerTestAccess::Authority(*h.controller);
    stale.stamp = UctControllerTestAccess::Stamp(*h.controller); stale.regionalRequested = true;
    const auto rejected = h.controller->RejectedResults();
    UctControllerTestAccess::Deliver(*h.controller,std::move(stale));
    QCOMPARE(h.controller->RejectedResults(),rejected + 1);
    QCOMPARE(UctControllerTestAccess::RegionalJob(*h.controller),newRegion);
    second->Release();
    QTRY_COMPARE(UctControllerTestAccess::RegionalJob(*h.controller),std::uint64_t(0));
    QCOMPARE(h.controller->SchedulerStatistics().regionalCalls,before.regionalCalls + 1);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,before.submitted + 3);
}

void UltraClearTextControllerTests::activeRegionalWithdrawalDoesNotDemandExtraFull()
{
    RegionalFlag flag(true); ImmutableHarness h;
    h.script->detailFocus = true; h.script->separateWordLines = true;
    h.page = MakeLayoutPage({{"Reading",{70,70},36},{"Outside",{700,300},36}},960,540);
    h.Enable(); QTRY_COMPARE(h.controller->BackendStatusSnapshot().state,BackendState::kReady);
    UctControllerTestAccess::SuppressRegions(*h.controller,true);
    QSignalSpy admitted(h.controller.get(),&UltraClearTextController::recognitionAdmitted);
    for (std::uint64_t frame = 1; frame <= 8; ++frame) {
        h.now += 60; h.PlayFrame(frame);
        QTRY_COMPARE(UctControllerTestAccess::ProbeFrame(*h.controller),frame);
        QTRY_COMPARE(admitted.count(),int(h.controller->SchedulerStatistics().submitted));
    }
    QVERIFY(h.overlay.WordCount() > 0);
    const auto old = h.controller->Model().Lines();
    const auto full = h.controller->SchedulerStatistics().fullCalls;
    {
        std::lock_guard lock(h.script->mutex);
        auto mismatch = h.page.words; mismatch.first().text = QStringLiteral("Readin");
        h.script->callWords[h.script->calls.load() + 1] = mismatch;
    }
    UctControllerTestAccess::SuppressRegions(*h.controller,false);
    h.now += 60; h.PlayFrame(9);
    QTRY_COMPARE(h.controller->SchedulerStatistics().regionalCalls,std::uint64_t(1));
    QTRY_COMPARE(admitted.count(),int(h.controller->SchedulerStatistics().submitted));
    QCOMPARE(admitted.last().first().toInt(),int(AdmitVerdict::kEmpty));
    QCOMPARE(h.controller->Model().PendingWordCount(),0);
    QCOMPARE(h.controller->Model().Lines().size(),old.size());
    for (int i = 0; i < old.size(); ++i) QCOMPARE(h.controller->Model().Lines()[i].id,old[i].id);
    for (std::uint64_t frame = 10; frame <= 14; ++frame) {
        h.now += 60; h.PlayFrame(frame);
        QTRY_COMPARE(UctControllerTestAccess::ProbeFrame(*h.controller),frame);
    }
    QCOMPARE(h.controller->SchedulerStatistics().fullCalls,full);
    QCOMPARE(h.controller->SchedulerStatistics().regionalAttempts,std::uint64_t(1));
    QCOMPARE(h.controller->Model().PendingWordCount(),0);
}

void UltraClearTextControllerTests::timingNonimmutableMutationKeepsCausalTickets()
{
    if (!timing::Enabled()) return; // also invoked by the existing trace-enabled pane selector below
    RegionalFlag flag(true); ImmutableHarness h;
    h.page = MakeLayoutPage({{"itself",{40,40},36},{"Outside",{700,300},36}},960,540);
    h.Enable(); QTRY_COMPARE(h.controller->BackendStatusSnapshot().state,BackendState::kReady);
    UctFrameContext context;
    context.geometry = h.Scene().geometry; context.requested = context.geometry.settings;
    context.transform.valid = true; context.cameraActive = true;
    context.viewportWidth = 960; context.viewportHeight = 540;
    std::uint64_t sequence = 0;
    const auto result = [&](std::uint64_t job, bool regional) {
        h.now += 20; context.frameSeq = ++sequence;
        h.controller->OnPresented(context); h.controller->OnPresentationSucceeded();
        RecognitionResult r;
        r.jobId = job; r.authority = UctControllerTestAccess::Authority(*h.controller);
        r.stamp = UctControllerTestAccess::Stamp(*h.controller);
        r.stamp.frameSeq = sequence; r.stamp.timestampMs = h.now;
        r.stamp.liveCameraCapture = true; r.stamp.viewTransform.valid = true;
        r.stamp.viewportWidth = 960; r.stamp.viewportHeight = 540;
        const auto pixels = IndependentFramePixels(h.page.image,sequence);
        r.reference = MakeProbeFrame(pixels.constBits(),960,540,int(pixels.bytesPerLine()),2,r.stamp);
        r.output.ok = true;
        for (const auto& word : h.page.words) r.output.lines.push_back({{{word.text,
            {word.sceneRect.x()*960,word.sceneRect.y()*540,word.sceneRect.width()*960,word.sceneRect.height()*540}}}});
        r.regionalRequested = regional;
        r.jobKind = regional ? RecognitionJobKind::Regional : RecognitionJobKind::Full;
        if (regional) r.output.detailRegionPx = {0,0,400,200};
        return r;
    };
    const auto deliver = [&](RecognitionResult r) {
        const bool regional = r.regionalRequested;
        UctControllerTestAccess::Deliver(*h.controller,std::move(r),regional);
    };
    deliver(result(101,false)); deliver(result(102,false));
    QCOMPARE(h.overlay.WordCount(),2);
    const auto initial = h.overlay.TimingTicket(); QVERIFY(initial.revision);
    auto refused = result(103,true); refused.output.lines.resize(1);
    refused.output.lines.first().words.first().text = QStringLiteral("itelf");
    deliver(std::move(refused));
    QCOMPARE(h.overlay.TimingTicket().revision,initial.revision);
    QCOMPARE(h.overlay.TimingTicket().admission.jobId,initial.admission.jobId);
    QCOMPARE(h.panel.TimingTicket().admission.jobId,initial.admission.jobId);
    // Empty-region uncertainty is a real model mutation, with zero new glyphs.
    for (bool pending : {true,false}) {
        auto uncertain = result(pending ? 104 : 105,true);
        uncertain.output.detailRegionPx = {200,100,400,200};
        RecognizedWord word{QStringLiteral("Novel"),pending ? QRectF(300,150,70,25) : QRectF(310,160,70,25)};
        if (!pending) word.confidence = .1;
        uncertain.output.lines = {RecognizedLine{{word}}};
        deliver(std::move(uncertain));
        QCOMPARE(h.overlay.WordCount(),2);
        const auto ticket = h.overlay.TimingTicket();
        QCOMPARE(ticket.admission.jobId,std::uint64_t(pending ? 104 : 105));
        QVERIFY(ticket.flags & timing::RetainedArtifact);
        QCOMPARE(h.controller->ScreenTextAccessibility()->TimingTicket().admission.jobId,ticket.admission.jobId);
        QCOMPARE(h.panel.TimingTicket().admission.jobId,std::uint64_t(104)); // unchanged omission text keeps its pane ticket
        QCOMPARE(h.controller->Model().PendingWordCount(),pending ? 1 : 0);
    }
    auto successful = result(106,true); successful.output.lines.resize(1); deliver(std::move(successful));
    QCOMPARE(h.overlay.TimingTicket().admission.jobId,std::uint64_t(106));
    QVERIFY(h.overlay.TimingTicket().flags & timing::RetainedArtifact);
    // The same pending/empty mutation class also exists for ordinary full
    // reads: current-proved detector omissions keep the other pane's proof.
    for (bool pending : {true,false}) {
        auto full = result(pending ? 107 : 108,false); full.output.lines.resize(1);
        full.output.lines.first().words.first().text = pending ? QStringLiteral("Changed") : QStringLiteral("Uncertain");
        if (!pending) {
            full.output.lines.first().words.first().confidence = .1;
            full.output.lines.first().words.first().boundsPx.translate(3,1); // changed protection, unchanged pane text
        }
        UctControllerTestAccess::Validate(*h.controller,full.reference);
        deliver(std::move(full));
        QCOMPARE(h.overlay.WordCount(),1);
        const auto ticket = h.overlay.TimingTicket();
        QCOMPARE(ticket.admission.jobId,std::uint64_t(pending ? 107 : 108));
        QVERIFY(ticket.flags & timing::RetainedArtifact);
        QCOMPARE(h.panel.TimingTicket().admission.jobId,std::uint64_t(107));
    }
    const auto positioned = h.overlay.TimingTicket(); const auto pane = h.panel.TimingTicket();
    context.transform.sourceX = .02f; context.transform.sourceWidth = .98f;
    h.controller->OnPresented(context); h.controller->OnPresentationSucceeded();
    QCOMPARE(h.overlay.TimingTicket().revision,positioned.revision);
    QCOMPARE(h.overlay.TimingTicket().admission.jobId,positioned.admission.jobId);
    QCOMPARE(h.panel.TimingTicket().revision,pane.revision);
}

void UltraClearTextControllerTests::timingTraceKeepsWorkerAndAdmissionTickets()
{
    // The full ordinary suite validates the cached disabled path. The CTest
    // selector runs this slot alone with tracing enabled before process start.
    if (!timing::Enabled()) {
        const auto snapshot = timing::CloseForDiagnostics();
        QVERIFY(snapshot && !snapshot->enabled && !snapshot->count);
        return;
    }
    ImmutableHarness h;
    h.Enable();
    auto scene = h.Scene();
    scene.corroboration = UctImmutableFrame{
        OwnedPixels(IndependentFramePixels(h.page.image, 11)), 11, 20};
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QTRY_COMPARE(h.controller->SchedulerStatistics().completed, std::uint64_t(2));
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
    QCOMPARE(h.script->calls.load(), 2);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));

    const auto snapshot = timing::CloseForDiagnostics();
    QVERIFY(snapshot && snapshot->enabled && snapshot->count);
    struct Observed { unsigned phases{}; std::int64_t lastNs{}; };
    std::map<std::uint64_t, Observed> jobs;
    bool sawAccepted{}, sawCorroboration{}, sawCpuDone{};
    std::uint64_t producer{}, authority{}, geometry{};
    for (std::size_t i = 0; i < snapshot->count; ++i) {
        const auto& r = snapshot->records[i];
        if (!r.key.jobId || r.key.channel != timing::Channel::Recognition) continue;
        if (!producer) { producer = r.key.producer; authority = r.key.authority; geometry = r.key.geometryGeneration; }
        QCOMPARE(r.key.producer, producer);
        QCOMPARE(r.key.authority, authority);
        QCOMPARE(r.key.geometryGeneration, geometry);
        QCOMPARE(r.key.sourceEpoch, std::uint64_t(41));
        QVERIFY(r.key.frameSeq == 10 || r.key.frameSeq == 11);
        QCOMPARE(r.key.sourceTimestampMs, r.key.frameSeq == 10 ? std::int64_t(1) : std::int64_t(20));
        QCOMPARE(r.key.acquisition, r.key.frameSeq);
        auto& job = jobs[r.key.jobId];
        QVERIFY(r.steadyNs >= job.lastNs);
        job.lastNs = r.steadyNs;
        switch (r.event) {
        case timing::Event::Submit: job.phases |= 1; break;
        case timing::Event::Dequeue: job.phases |= 2; break;
        case timing::Event::Done:
            job.phases |= 4;
            QVERIFY(r.values.flags & timing::InferenceAttempted);
            QVERIFY(r.values.flags & timing::BackendKnown);
            QVERIFY(!(r.values.flags & timing::GpuBackend));
            sawCpuDone = true;
            break;
        case timing::Event::UiReceipt: job.phases |= 8; break;
        case timing::Event::Admit:
            QCOMPARE(r.key.frameSeq, std::uint64_t(10)); // neighbor never becomes selected evidence
            if (r.values.code == static_cast<std::uint32_t>(AdmitVerdict::kAccepted)) {
                job.phases |= 16;
                QVERIFY(r.values.relatedJobId && r.values.relatedJobId != r.key.jobId);
                QCOMPARE(r.values.count, static_cast<std::uint32_t>(h.overlay.WordCount()));
                sawAccepted = true;
            }
            break;
        default: break;
        }
        if (r.key.frameSeq == 11 && (r.values.flags & timing::Corroboration)) {
            QVERIFY(r.values.relatedJobId && r.values.relatedJobId != r.key.jobId);
            sawCorroboration = true;
        }
    }
    const auto summary = timing::Summarize(*snapshot);
    QCOMPARE(summary.ticketMismatches, std::uint64_t(0));
    if (!snapshot->capacityDrops && !snapshot->contentionDrops && !snapshot->unavailableDueToContention) {
        QCOMPARE(jobs.size(), std::size_t(2));
        QVERIFY(sawAccepted && sawCorroboration && sawCpuDone);
        QCOMPARE(summary.fullyObserved, std::uint64_t(2));
        QCOMPARE(summary.incomplete, std::uint64_t(0));
        QVERIFY(!summary.coverageIncomplete);
    } else {
        // Best-effort production hooks must drop rather than wait. Any loss
        // is valid only with explicit incomplete coverage in the same output.
        QVERIFY(summary.coverageIncomplete);
        std::ostringstream output;
        timing::WriteJsonLines(*snapshot, output);
        QVERIFY(output.str().find("\"eligible_for_complete_chain_latency\":false") != std::string::npos);
    }
}

void UltraClearTextControllerTests::timingPaneOnlyChangePreservesPositionedTicket()
{
    if (!timing::Enabled()) return; // enabled CTest selector owns one trace window
    AccessibilityActivation accessibility;
    ImmutableHarness h;
    h.owner.resize(1000, 600);
    h.owner.show(); // Qt offscreen; no interactive desktop fixture
    h.page = MakePage({QStringLiteral("Reading"), QStringLiteral("HintAlpha")}, {}, 960, 540, 48);
    const QVector<PageWord> glyphWords{h.page.words[0]};
    h.script->words = glyphWords;
    h.script->separateWordLines = true;
    h.Enable();
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    UctFrameContext context;
    context.geometry = h.Scene().geometry;
    context.geometry.settings.stabilization = true;
    context.requested = context.geometry.settings;
    context.transform.valid = true;
    context.cameraActive = true;
    context.heldFrames = true;
    context.compositeFrames = true;
    context.viewportWidth = 960; context.viewportHeight = 540;
    std::uint64_t sequence{};
    constexpr std::int64_t stepMs = 80;
    const auto await = [](const auto& ready) {
        QElapsedTimer deadline;
        deadline.start();
        while (!ready() && deadline.elapsed() < 5000) QTest::qWait(1);
        return ready();
    };
    const auto stage = [&](HoldDisplay display, std::uint64_t acquisition) {
        const auto probes = h.controller->ProbeStatistics().submitted;
        h.now += stepMs;
        context.frameSeq = ++sequence;
        // Readiness includes consumption of the previous device/probe UI
        // receipts. Recheck this one staged presentation at a fixed clock;
        // only one independently owned image is submitted for its frame.
        if (!await([&] { return h.controller->OnPresented(context); })) return false;
        h.controller->OnPresentationSucceeded();
        h.controller->OnDeviceScene(context, std::make_shared<FakeDeviceImage>(
            h.devices, h.page.image, 41, sequence, h.now,
            HoldProvenance{display, display == HoldDisplay::Live ? sequence : acquisition, 0.0f}));
        if (!await([&] { return h.controller->ProbeStatistics().submitted == probes + 1; }) ||
            !await([&] { return h.controller->ProbeStatistics().completed == probes + 1; })) return false;
        if (!h.controller->Model().Lines().isEmpty()) {
            // completed counts worker completion, not UI receipt. The exact
            // frame in the model proves that ApplyChecks has run on the UI.
            if (!await([&] {
                    const auto& lines = h.controller->Model().Lines();
                    return !lines.isEmpty() && std::all_of(lines.begin(), lines.end(), [&](const AcceptedLine& line) {
                        return line.valid && line.validatedFrameSeq == sequence;
                    });
                })) return false;
        }
        const auto submitted = h.controller->SchedulerStatistics().submitted;
        return await([&] {
            return h.controller->SchedulerStatistics().completed == submitted &&
                static_cast<std::uint64_t>(admitted.count()) == submitted;
        }); // freeze fake time until any job's real admission callback ran
    };
    // The first probe establishes similarity; the next two live acquisitions
    // independently recognize and prove the one positioned word. Do not age
    // that proof while two queued worker/UI chains are still in flight.
    QVERIFY(stage(HoldDisplay::Live, 0));
    QVERIFY(stage(HoldDisplay::Live, 0));
    QTRY_COMPARE(admitted.count(), 1);
    QVERIFY(stage(HoldDisplay::Live, 0));
    QTRY_COMPARE(admitted.count(), 2);
    QCOMPARE(h.overlay.WordCount(), glyphWords.size());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
    {
        std::lock_guard lock(h.script->mutex);
        h.script->words = h.page.words;
    }
    // Each new held acquisition is genuinely older than its presentation.
    // Reading it still obeys the ordinary 1500-ms refresh gate. Revalidating
    // every completed 80-ms stage keeps the glyph's proof inside 350 ms.
    const auto alphaAcquisition = ++sequence;
    const auto refreshSteps = (UctTiming{}.refreshIntervalMs + stepMs - 1) / stepMs;
    for (std::int64_t i = 0; i < refreshSteps; ++i) QVERIFY(stage(HoldDisplay::Held, alphaAcquisition));
    QVERIFY(h.Reading().contains(QStringLiteral("HintAlpha")));
    QCOMPARE(admitted.count(), 3);
    QCOMPARE(h.overlay.WordCount(), glyphWords.size()); // held token stays reading-only
    const auto positioned = h.overlay.Words();
    QVERIFY(!positioned.first().protectedSceneRects.isEmpty());
    const auto screenBefore = h.controller->ScreenTextSnapshot();
    QVERIFY(!screenBefore.lines.isEmpty());
    for (const auto& line : screenBefore.lines) QVERIFY(!line.text.contains(QStringLiteral("HintAlpha")));
    // Settle any legitimate setup addition through the documented bounded
    // semantic cadence, keeping each real validation current. This is a
    // fixed interval, not a loop that drives time until a precondition passes.
    const auto semanticMs = std::max(UctScreenTextTiming{}.cadenceMs, UctScreenTextTiming{}.readingHoldMs);
    const auto semanticSteps = (semanticMs + stepMs - 1) / stepMs;
    for (std::int64_t i = 0; i < semanticSteps; ++i) {
        QVERIFY(stage(HoldDisplay::Held, alphaAcquisition));
        h.controller->ScreenTextAccessibility()->ProcessDueWork();
        QCOMPARE(h.overlay.WordCount(), glyphWords.size());
        QCOMPARE(admitted.count(), 3); // repeating alpha cannot corroborate or reread it
    }
    const auto drivePaint = [&] {
        const auto before = h.overlay.PaintedGeneration();
        h.overlay.repaint();
        return await([&] { return h.overlay.PaintedGeneration() > before; });
    };
    QVERIFY(drivePaint());
    QVERIFY(!h.controller->ScreenTextAccessibility()->HasPendingAdditions());
    QCOMPARE(h.controller->ScreenTextAccessibility()->ExposedLineCount(), screenBefore.lines.size());
    const auto publications = h.controller->ScreenTextAccessibility()->Publications();
    const auto positionedTicket = h.overlay.TimingTicket();
    const auto paintedRevision = h.overlay.PaintedTimingRevision();
    const auto sameTicket = [](const timing::PublicationTicket& a,const timing::PublicationTicket& b) {
        const auto key = [](const timing::Key& value) {
            return std::tuple{value.producer,value.sourceEpoch,value.geometryGeneration,value.frameSeq,
                value.acquisition,value.jobId,value.authority,value.sourceTimestampMs,value.channel};
        };
        return a.revision == b.revision && a.flags == b.flags && key(a.admission) == key(b.admission);
    };
    QVERIFY(positionedTicket.revision && positionedTicket.admission.jobId);
    QCOMPARE(paintedRevision,positionedTicket.revision);
    QVERIFY(sameTicket(h.controller->ScreenTextAccessibility()->TimingTicket(),positionedTicket));
    const auto paneOnlyStartedNs = timing::SteadyNanoseconds();
    {
        std::lock_guard lock(h.script->mutex);
        h.script->words.last().text = QStringLiteral("HintBeta"); // same protected rect/owned pixels
    }
    const auto betaAcquisition = ++sequence;
    QVERIFY(stage(HoldDisplay::Held, betaAcquisition));
    QVERIFY(h.Reading().contains(QStringLiteral("HintBeta")));
    QVERIFY(!h.Reading().contains(QStringLiteral("HintAlpha")));
    QCOMPARE(admitted.count(), 4);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(4));
    QCOMPARE(h.overlay.Words().size(), positioned.size());
    QVERIFY(sameTicket(h.overlay.TimingTicket(),positionedTicket));
    QVERIFY(sameTicket(h.controller->ScreenTextAccessibility()->TimingTicket(),positionedTicket));
    QCOMPARE(h.overlay.PaintedTimingRevision(),paintedRevision);
    const auto paneTicket = h.panel.TimingTicket();
    QVERIFY(paneTicket.revision > positionedTicket.revision);
    QVERIFY(paneTicket.admission.jobId != positionedTicket.admission.jobId);
    QCOMPARE(paneTicket.admission.acquisition,betaAcquisition);
    for (int i = 0; i < positioned.size(); ++i) {
        const auto& after = h.overlay.Words()[i]; const auto& before = positioned[i];
        QCOMPARE(after.text, before.text); QCOMPARE(after.lineId, before.lineId);
        QCOMPARE(after.wordIndex, before.wordIndex); QCOMPARE(after.sceneRect, before.sceneRect);
        QCOMPARE(after.protectedSceneRects, before.protectedSceneRects);
        QCOMPARE(after.carvedSceneRects, before.carvedSceneRects);
        QCOMPARE(after.colors.foreground, before.colors.foreground);
        QCOMPARE(after.colors.background, before.colors.background);
    }
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    context.transform.sourceX = 0.02f; // actual controller reprojection after pane-only admission
    context.transform.sourceWidth = 0.98f;
    h.controller->OnPresented(context);
    h.controller->OnPresentationSucceeded();
    QVERIFY(drivePaint());
    h.controller->ScreenTextAccessibility()->ProcessDueWork();
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.controller->ScreenTextAccessibility()->Publications(), publications);
    QVERIFY(sameTicket(h.overlay.TimingTicket(),positionedTicket));
    QVERIFY(sameTicket(h.controller->ScreenTextAccessibility()->TimingTicket(),positionedTicket));
    QCOMPARE(h.overlay.PaintedTimingRevision(),paintedRevision);
    QVERIFY(!h.devices->readOnUiThread.load());

    const auto snapshot = timing::CloseForDiagnostics();
    QVERIFY(snapshot);
    timingNonimmutableMutationKeepsCausalTickets(); // direct cached tickets also work after the trace window closes
    const bool lossless = !snapshot->unavailableDueToContention && !snapshot->capacityDrops &&
        !snapshot->contentionDrops && !snapshot->closeOverlappedAttempts &&
        snapshot->acceptedAtSnapshot == snapshot->count;
    if (!lossless) {
        // This nonblocking recorder is permitted to lose concurrent events.
        // Exact consumer tickets above still prove the publication contract;
        // a lossy trace cannot prove absence or complete-chain latency.
        QVERIFY(timing::Summarize(*snapshot).coverageIncomplete);
        std::ostringstream output;
        timing::WriteJsonLines(*snapshot,output);
        QVERIFY(output.str().find("\"eligible_for_complete_chain_latency\":false") != std::string::npos);
        qInfo() << "Pane publication identity verified directly; trace coverage incomplete, capacity/contention drops="
                << snapshot->capacityDrops << snapshot->contentionDrops;
        return;
    }
    QCOMPARE(snapshot->acceptedAtSnapshot,static_cast<std::uint64_t>(snapshot->count));
    const timing::Record* lastPositionedAssignment{};
    const timing::Record* paneChange{};
    int oldContentPaints{}, changedPositionedEvents{}, paneAssignments{};
    for (std::size_t i = 0; i < snapshot->count; ++i) {
        const auto& r = snapshot->records[i];
        if (r.steadyNs < paneOnlyStartedNs) {
            if (r.event == timing::Event::OverlayAssigned) lastPositionedAssignment = &r;
            if (r.event == timing::Event::OverlayPainted) ++oldContentPaints;
        } else {
            if (r.event == timing::Event::PaneAssigned) { paneChange = &r; ++paneAssignments; }
            if (r.event == timing::Event::OverlayAssigned || r.event == timing::Event::OverlayPainted ||
                r.event == timing::Event::AccessibilityScheduled || r.event == timing::Event::AccessibilityPublished)
                ++changedPositionedEvents;
        }
    }
    QVERIFY(lastPositionedAssignment && oldContentPaints > 0 && paneChange);
    QCOMPARE(paneAssignments, 1);
    QVERIFY(paneChange->values.publicationRevision > lastPositionedAssignment->values.publicationRevision);
    QVERIFY(paneChange->key.jobId != lastPositionedAssignment->key.jobId);
    QCOMPARE(paneChange->key.acquisition, betaAcquisition);
    QCOMPARE(changedPositionedEvents, 0); // baseline retickets and falsely logs a fresh content paint on pan
}

void UltraClearTextControllerTests::textClarityMasterPreservesUctPreference()
{
    ImmutableHarness h;
    h.controller->SetTextClarityEnabled(false);
    h.controller->SetEnabled(true);
    QVERIFY(h.controller->IsEnabledByUser());
    QVERIFY(!h.controller->IsEnabled());
    QVERIFY(h.panel.enableCheckbox()->isChecked());
    QVERIFY(!h.panel.enableCheckbox()->isEnabled());
    QVERIFY(h.controller->SetImmutableScene(h.Scene()));
    QTest::qWait(60);
    QCOMPARE(h.script->calls.load(), 0);
    h.controller->SetTextClarityEnabled(true);
    QVERIFY(h.controller->IsEnabled());
    QTRY_COMPARE(h.script->calls.load(), 1);
    QTRY_VERIFY(!h.Reading().isEmpty());
    h.controller->SetTextClarityEnabled(false);
    QVERIFY(h.controller->IsEnabledByUser());
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.overlay.Words().isEmpty());
    h.controller->SetTextClarityEnabled(true);
    QTRY_COMPARE(h.script->calls.load(), 2);
    h.controller->SetEnabled(false);
    h.controller->SetTextClarityEnabled(false);
    h.controller->SetTextClarityEnabled(true);
    QVERIFY(!h.controller->IsEnabled());
    QVERIFY(!h.controller->IsEnabledByUser());
    QVERIFY(!h.panel.enableCheckbox()->isChecked());
}

void UltraClearTextControllerTests::liveCameraIdenticalPixelsAcquireText_data()
{
    QTest::addColumn<bool>("device");
    QTest::addColumn<bool>("afterMedia");
    QTest::addColumn<bool>("resident");
    QTest::newRow("cpu-camera") << false << false << false;
    QTest::newRow("device-camera") << true << false << false;
    QTest::newRow("device-camera-after-video") << true << true << false;
    QTest::newRow("resident-camera") << true << false << true;
    QTest::newRow("resident-camera-after-video") << true << true << true;
}

void UltraClearTextControllerTests::liveCameraIdenticalPixelsAcquireText()
{
    QFETCH(bool,device);QFETCH(bool,afterMedia);QFETCH(bool,resident);
    ImmutableHarness h;
    h.script->wantsDevice=device;h.devices->resident=resident;
    h.Enable();
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state,BackendState::kReady);
    if(afterMedia) {
        QVERIFY(h.controller->SetImmutableScene(h.Scene()));
        h.controller->ClearImmutableScene();
    }
    UctFrameContext context;
    context.geometry=h.Scene().geometry;
    context.requested=context.geometry.settings;
    context.transform.valid=true;
    context.cameraActive=true;
    context.viewportWidth=h.page.image.width();context.viewportHeight=h.page.image.height();
    const auto present=[&](std::uint64_t sequence) {
        h.now+=100;context.frameSeq=sequence;
        const bool wanted=h.controller->OnPresented(context);
        h.controller->OnPresentationSucceeded();
        if(wanted) {
            if(device)h.controller->OnDeviceScene(context,h.Device(h.page.image,sequence,h.now));
            else h.controller->OnCpuViewport(context,h.page.image.constBits(),h.page.image.width(),
                h.page.image.height(),h.page.image.bytesPerLine());
        }
        QTest::qWait(12);
    };
    // Repainting a held camera sample is not another acquisition. The fixture
    // intentionally keeps ALL pixel bytes identical in both phases.
    for(int repeat=0;repeat<12;++repeat)present(1);
    QVERIFY(h.script->calls.load()+h.script->deviceCalls.load()>=2);
    QCOMPARE(h.overlay.WordCount(),0);
    for(std::uint64_t sequence=2;sequence<=55 && h.overlay.WordCount()==0;++sequence)present(sequence);
    QVERIFY2(h.overlay.WordCount()>0,"Fresh live-camera acquisitions with identical pixels stayed pending forever");
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(!h.controller->HasImmutableScene());
    QVERIFY(!h.devices->readOnUiThread.load());
    if(resident){QCOMPARE(h.devices->readbacks.load(),0);QCOMPARE(h.devices->probes.load(),0);}
}

void UltraClearTextControllerTests::residentSelectedNeighbourUsesWorkerRegistration()
{
    ImmutableHarness h;h.script->wantsDevice=true;h.devices->resident=true;h.Enable();
    auto scene=h.DeviceScene();
    const auto neighbour=IndependentFramePixels(h.page.image,11);
    scene.corroboration=UctImmutableFrame{{},11,2,h.Device(neighbour,11,2)};
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(h.script->deviceCalls.load(),2);
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.devices->readbacks.load(),0);QCOMPARE(h.devices->probes.load(),0);
    QVERIFY(!h.devices->readOnUiThread.load());
    h.controller->ClearImmutableScene();QCOMPARE(h.overlay.WordCount(),0);
}

void UltraClearTextControllerTests::pausedScrollingTextUsesSelectedFrameCoordinates_data()
{
    QTest::addColumn<bool>("resident");
    QTest::addColumn<bool>("changedInk");
    QTest::newRow("cpu") << false << false;
    QTest::newRow("resident") << true << false;
    QTest::newRow("cpu-changed-ink") << false << true;
    QTest::newRow("resident-changed-ink") << true << true;
}

void UltraClearTextControllerTests::pausedScrollingTextUsesSelectedFrameCoordinates()
{
    QFETCH(bool, resident);
    QFETCH(bool, changedInk);
    ImmutableHarness h;
    h.page=MakeLayoutPage({{"Fixed",{90,70},32},{"Millwright",{360,170},32},
                           {"Motion",{360,230},32}},960,540);
    auto neighbor=MakeLayoutPage({{"Fixed",{90,70},32},{"Millwright",{360,250},32},
                                       {"Motion",{360,310},32}},960,540);
    if(changedInk)neighbor.image=MakeLayoutPage({{"Fixed",{90,70},32},{"Different",{360,250},32},
                                                {"Motion",{360,310},32}},960,540).image;
    h.script->words=h.page.words;
    h.script->pixelWords.insert(SourcePixelsDigest(h.page.image),h.page.words);
    h.script->pixelWords.insert(SourcePixelsDigest(neighbor.image),neighbor.words);
    h.script->frameWords.insert(10,h.page.words);
    h.script->frameWords.insert(11,neighbor.words);
    h.script->wantsDevice=resident;h.devices->resident=resident;
    auto scene=h.Scene(10);
    if(resident) {
        scene.selected={{},10,1,h.Device(h.page.image,10,1)};
        scene.corroboration=UctImmutableFrame{{},11,34,h.Device(neighbor.image,11,34)};
    } else scene.corroboration=UctImmutableFrame{OwnedPixels(neighbor.image),11,34};
    h.Enable();QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(h.script->calls.load()+h.script->deviceCalls.load(),2);
    QTRY_COMPARE(h.controller->Model().SceneWords(h.now,10).size(),h.page.words.size()-(changedInk?1:0));
    const auto shown=h.controller->Model().SceneWords(h.now,10);
    for(const auto& expected:h.page.words) {
        const auto found=std::find_if(shown.begin(),shown.end(),[&](const auto& word){return word.text==expected.text;});
        if(changedInk && expected.text=="Millwright"){QVERIFY(found==shown.end());continue;}
        QVERIFY(found!=shown.end());
        QCOMPARE(found->sceneRect,expected.sceneRect); // never adopt the neighbour's scrolled position
    }
    h.controller->UpdateImmutableView(scene.transform,800,450,0);
    QTest::qWait(80);
    QCOMPARE(h.script->calls.load()+h.script->deviceCalls.load(),2); // remains bounded, one selected pair
    QVERIFY(!h.devices->readOnUiThread.load());
    if(resident){QCOMPARE(h.devices->readbacks.load(),0);QCOMPARE(h.devices->probes.load(),0);}
}

void UltraClearTextControllerTests::pausedWordRegistrationFaultCannotPublishPartialProof()
{
    ImmutableHarness h;
    h.page=MakeLayoutPage({{"Fixed",{90,70},32},{"Millwright",{360,170},32},
                           {"Motion",{360,230},32}},960,540);
    const auto neighbor=MakeLayoutPage({{"Fixed",{90,70},32},{"Millwright",{360,250},32},
                                       {"Motion",{360,310},32}},960,540);
    h.script->words=h.page.words;
    h.script->frameWords.insert(10,h.page.words);h.script->frameWords.insert(11,neighbor.words);
    h.script->wantsDevice=true;h.devices->resident=true;h.devices->failWordReferences=true;
    auto scene=h.DeviceScene();
    scene.corroboration=UctImmutableFrame{{},11,34,h.Device(neighbor.image,11,34)};
    h.Enable();QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.script->deviceCalls.load()>=2);
    QTest::qWait(700); // bounded failure/retry path, no completed partial receipt
    QCOMPARE(h.overlay.WordCount(),0);
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QVERIFY(h.script->deviceCalls.load()<=5);
    QVERIFY(!h.devices->readOnUiThread.load());
    QCOMPARE(h.devices->readbacks.load(),0);
}

void UltraClearTextControllerTests::processingCorrespondenceGateRevokesTextAndUiaWithoutChangingPreference()
{
    AccessibilityActivation accessibility;
    ImmutableHarness h;
    h.Enable();
    auto scene = h.Scene();
    auto independent = h.page.image.copy();
    independent.setPixelColor(950, 530, Qt::red);
    scene.corroboration = UctImmutableFrame{OwnedPixels(independent), 11, 2};
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QVERIFY(!h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextAccessibility()->ExposedLineCount() > 0);
    h.controller->SetMediaOverlaySuppressed(true);
    QVERIFY(h.controller->IsEnabled()); // user preference remains on
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QCOMPARE(h.controller->ScreenTextAccessibility()->ExposedLineCount(), 0);
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    QVERIFY(h.controller->SetImmutableScene(scene)); // cannot bypass processing correspondence
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    h.controller->OnMediaPresentationSucceeded();
    QTest::qWait(50);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.controller->SetMediaOverlaySuppressed(false);
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QVERIFY(!h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextAccessibility()->ExposedLineCount() > 0);
    h.controller->SetMediaOverlaySuppressed(true);
    h.controller->ClearImmutableScene(); // leaving media must clear the gate even without an owned scene
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount() > 0);
}

void UltraClearTextControllerTests::immutableOldFrameAndIdleDoNoPeriodicWork()
{
    ImmutableHarness h;
    auto scene = h.Scene();
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(admitted.size(), 1);
    QVERIFY(h.Reading().startsWith(QChar(0x2026)));
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.overlay.WordCount(), 0); // one still is reading evidence, not temporal proof
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    QCOMPARE(h.controller->Model().Lines().first().provenance.timestampMs, std::int64_t(1));
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, scene.selected.frameSeq);

    const QString reading = h.Reading();
    h.now += 3600000;
    UctFrameContext live;
    live.geometry.settings.sourceEpoch = 900;
    live.cameraActive = true;
    live.frameSeq = 900;
    live.transform.valid = true;
    live.viewportWidth = 960;
    live.viewportHeight = 540;
    for (int i = 0; i < 20; ++i) {
        QVERIFY(!h.controller->OnPresented(live));
        h.controller->OnCpuViewport(live, h.page.image.constBits(), h.page.image.width(),
                                    h.page.image.height(), static_cast<int>(h.page.image.bytesPerLine()));
        h.controller->OnDeviceScene(live, {});
        h.controller->OnReadbackRequested(17, live);
        h.controller->OnPresentationSucceeded();
        h.controller->OnPresentationFailed();
        h.controller->OnSourceRetired();
    }
    QTest::qWait(250); // past the ordinary watchdog tick; no media watchdog renewal
    QCOMPARE(h.Reading(), reading);
    QCOMPARE(h.script->calls.load(), 1);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
    QVERIFY(!h.controller->HasPendingReadback());
    QVERIFY(h.controller->SetImmutableScene(scene)); // exact repeated selection is projection only
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
}

void UltraClearTextControllerTests::backendReadySubmitsWaitingMediaOnce()
{
    ImmutableHarness h;
    h.script->holdInitialization = true;
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(h.Scene()));
    QTRY_COMPARE(h.script->initialized.load(), 1);
    QTest::qWait(250);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
    h.script->Release();
    QTRY_VERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QTest::qWait(250);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::latePausedNeighbourReusesSelectedEvidence_data()
{
    QTest::addColumn<bool>("resident");
    QTest::addColumn<bool>("selectedInFlight");
    QTest::newRow("cpu-selected-in-flight") << false << true;
    QTest::newRow("cpu-selected-complete") << false << false;
    QTest::newRow("resident-selected-in-flight") << true << true;
    QTest::newRow("resident-selected-complete") << true << false;
}

void UltraClearTextControllerTests::latePausedNeighbourReusesSelectedEvidence()
{
    QFETCH(bool, resident);
    QFETCH(bool, selectedInFlight);
    ImmutableHarness h;
    h.script->wantsDevice = resident;
    h.devices->resident = resident;
    h.script->holdRecognition = selectedInFlight;
    auto scene = resident ? h.DeviceScene() : h.Scene();
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(h.script->calls.load() + h.script->deviceCalls.load(), 1);
    if (!selectedInFlight) QTRY_COMPARE(admitted.size(), 1);
    const auto generation = h.controller->GeometryGeneration();
    const QString reading = h.Reading();

    // An invalid optional descriptor cannot consume the later real neighbour.
    auto invalid = scene;
    invalid.corroboration = scene.selected; // the selected acquisition is not independent
    QVERIFY(h.controller->SetImmutableScene(invalid));
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    QCOMPARE(h.Reading(), reading);

    const auto neighbour = IndependentFramePixels(h.page.image, 11);
    scene.corroboration = resident
        ? UctImmutableFrame{{}, 11, 20, h.Device(neighbour, 11, 20)}
        : UctImmutableFrame{OwnedPixels(neighbour), 11, 20};
    QVERIFY(h.controller->SetImmutableScene(scene));
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.Reading(), reading); // attaching evidence never clears selected text
    if (selectedInFlight) {
        QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
        h.script->Release();
    }
    QTRY_COMPARE(h.overlay.WordCount(), h.page.words.size());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
    QCOMPARE(h.script->calls.load() + h.script->deviceCalls.load(), 2);
    const auto proven = h.controller->Model().SceneWords(h.now, scene.selected.frameSeq);
    const QString completeReading = h.Reading();

    // Ordinary view receipts may omit ancillary neighbour data; a different
    // neighbour must not create another observation of this selected frame.
    auto repeated = scene;
    repeated.corroboration.reset();
    QVERIFY(h.controller->SetImmutableScene(repeated));
    repeated.corroboration = resident
        ? UctImmutableFrame{{}, 12, 30, h.Device(IndependentFramePixels(h.page.image, 12), 12, 30)}
        : UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image, 12)), 12, 30};
    QVERIFY(h.controller->SetImmutableScene(repeated));
    QTest::qWait(100);
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
    QCOMPARE(h.Reading(), completeReading);
    QCOMPARE(h.controller->Model().SceneWords(h.now, scene.selected.frameSeq).size(), proven.size());
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QVERIFY(!h.devices->readOnUiThread.load());
    if (resident) {
        QCOMPARE(h.devices->readbacks.load(), 0);
        QCOMPARE(h.devices->probes.load(), 0);
    }
}

void UltraClearTextControllerTests::latePausedNeighbourPreservesSelectedDeadline()
{
    UctTiming timing;
    timing.immutableJobTimeoutMs = 100;
    ImmutableHarness h(timing);
    h.script->holdRecognition = true;
    auto scene = h.Scene();
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(h.script->calls.load(), 1);
    const auto generation = h.controller->GeometryGeneration();
    h.now += 101; // selected job is already overdue before the neighbour arrives
    scene.corroboration = UctImmutableFrame{
        OwnedPixels(IndependentFramePixels(h.page.image, 11)), 11, 20};
    QVERIFY(h.controller->SetImmutableScene(scene));
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    h.script->Release();
    QTRY_COMPARE(h.controller->RejectedResults(), std::uint64_t(1));
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    // A scheduled timeout retry still belongs to its exact selected authority.
    h.controller->ClearImmutableScene();
    QTest::qWait(150);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
}

void UltraClearTextControllerTests::coveredZoomReprojectsTheSameArtifact()
{
    ImmutableHarness h;
    auto scene = h.Scene();
    auto neighbour = h.page.image.copy();
    neighbour.setPixelColor(950, 530, Qt::red); // distinct source pixels outside the text patch
    scene.corroboration = UctImmutableFrame{OwnedPixels(neighbour), 11, 2};
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QCOMPARE(h.script->calls.load(), 2);
    const auto originalWords = h.overlay.Words();
    const auto originalRects = h.overlay.ProjectedWordRects();
    const auto generation = h.controller->GeometryGeneration();
    const auto stamp = h.controller->Model().Lines().first().provenance;
    // Fit intentionally preserves the complete scene and ignores zoom/pan.
    // Fill is the production cropped-view contract for a magnified scene.
    const auto zoom = ComputeViewTransform(960, 540, 960, 540, 2.0f, 0.3f, 0.3f,
                                           ViewportFitMode::kFill);
    QVERIFY(zoom.valid);
    QCOMPARE(zoom.sourceWidth, 0.5f);
    QCOMPARE(zoom.sourceHeight, 0.5f);
    h.now += 120000;
    h.controller->UpdateImmutableView(zoom, 960, 540, 2);
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.overlay.CurrentViewTransform().sourceX, zoom.sourceX);
    QCOMPARE(h.overlay.CurrentViewTransform().sourceY, zoom.sourceY);
    QCOMPARE(h.overlay.CurrentViewTransform().sourceWidth, zoom.sourceWidth);
    QCOMPARE(h.overlay.CurrentViewTransform().sourceHeight, zoom.sourceHeight);
    QCOMPARE(h.overlay.Words().first().sceneRect, originalWords.first().sceneRect);
    QCOMPARE(h.overlay.ProjectedWordRects().first(),
             SceneRectToView(zoom, h.overlay.width(), h.overlay.height(), originalWords.first().sceneRect));
    QVERIFY(h.overlay.ProjectedWordRects().first() != originalRects.first());
    QVERIFY(std::abs(h.overlay.ProjectedWordRects().first().width() -
                     2.0 * originalRects.first().width()) < 0.001);
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, stamp.frameSeq);
    QCOMPARE(h.controller->Model().Lines().first().provenance.timestampMs, stamp.timestampMs);
    h.controller->SetShowOriginal(true);
    QCOMPARE(h.overlay.WordCount(), 0);
    h.controller->SetShowOriginal(false);
    QVERIFY(h.overlay.WordCount() > 0);
    QTest::qWait(250);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::identicalPixelsNeverCorroborate()
{
    ImmutableHarness h;
    auto scene = h.Scene();
    scene.corroboration = UctImmutableFrame{OwnedPixels(h.page.image), 11, 2};
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(admitted.size(), 2);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().startsWith(QChar(0x2026)));
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    h.now += 120000;
    QTest::qWait(250);
    QCOMPARE(h.script->calls.load(), 2); // bounded failed corroboration, no retry loop
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::changingAuthorityAndClearingFenceLateResults()
{
    ImmutableHarness h;
    h.script->holdRecognition = true;
    auto first = h.Scene();
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(first));
    QTRY_COMPARE(h.script->calls.load(), 1);
    auto next = h.Scene(20);
    ++next.sourceAuthority;
    const auto generation = h.controller->GeometryGeneration();
    QVERIFY(h.controller->SetImmutableScene(next));
    QVERIFY(h.controller->GeometryGeneration() > generation);
    QVERIFY(h.Reading().isEmpty());
    h.script->Release();
    QTRY_VERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, std::uint64_t(20));
    QVERIFY(h.controller->SchedulerStatistics().revoked >= 1);
    QCOMPARE(h.controller->SchedulerStatistics().started, std::uint64_t(2));

    {
        std::lock_guard lock(h.script->mutex);
        h.script->holdRecognition = true;
    }
    auto pending = h.Scene(30);
    QVERIFY(h.controller->SetImmutableScene(pending));
    QTRY_COMPARE(h.script->calls.load(), 3);
    h.controller->ClearImmutableScene();
    QVERIFY(!h.controller->HasImmutableScene());
    QVERIFY(h.Reading().isEmpty());
    h.script->Release();
    QTRY_VERIFY(h.controller->SchedulerStatistics().revoked >= 2);
    QVERIFY(h.controller->Model().Lines().isEmpty());
    UctFrameContext live;
    live.geometry.settings.sourceEpoch = 91;
    live.geometry.sceneWidth = 960;
    live.geometry.sceneHeight = 540;
    live.requested = live.geometry.settings;
    live.cameraActive = true;
    live.transform.valid = true;
    live.viewportWidth = 960;
    live.viewportHeight = 540;
    QVERIFY(h.controller->OnPresented(live)); // live can resume only after explicit clear
}

void UltraClearTextControllerTests::modelReplacementRevokesSelectedEvidence()
{
    ImmutableHarness h;
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(h.Scene()));
    QTRY_VERIFY(!h.Reading().isEmpty());
    const auto generation = h.controller->GeometryGeneration();
    auto shared = h.script;
    h.controller->ReplaceRecognizerFactory([shared] { return std::make_unique<ImmutableRecognizer>(shared); });
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->GeometryGeneration() > generation);
    QTRY_COMPARE(h.script->initialized.load(), 2);
    QTRY_VERIFY(!h.Reading().isEmpty());
    QCOMPARE(h.script->calls.load(), 2);
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::queuedCompletionCannotCrossSourceAuthority()
{
    ImmutableHarness h;
    h.Enable();
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
    QVERIFY(h.controller->SetImmutableScene(h.Scene()));
    // Let the native worker finish while the UI callback stays queued. This
    // exercises the controller fence as well as the worker's cancellation.
    QElapsedTimer deadline;
    deadline.start();
    while (h.controller->SchedulerStatistics().completed < 1 && deadline.elapsed() < 3000)
        QTest::qSleep(1);
    QCOMPARE(h.controller->SchedulerStatistics().completed, std::uint64_t(1));
    auto replacement = h.Scene(20);
    ++replacement.sourceAuthority;
    QVERIFY(h.controller->SetImmutableScene(replacement));
    QVERIFY(h.Reading().isEmpty());
    QTRY_VERIFY(!h.Reading().isEmpty());
    QVERIFY(h.controller->RejectedResults() >= 1);
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, std::uint64_t(20));
}

void UltraClearTextControllerTests::immutableDeadlineIsIndependentOfSourceAge()
{
    UctTiming timing;
    timing.immutableJobTimeoutMs = 100;
    ImmutableHarness h(timing);
    h.script->holdRecognition = true;
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(h.Scene()));
    QTRY_COMPARE(h.script->calls.load(), 1);
    h.now += 101; // current analysis deadline, not the hour-old source clock
    h.script->Release();
    QTRY_COMPARE(h.controller->RejectedResults(), std::uint64_t(1));
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->Model().Lines().isEmpty());
    // The expired completion stays rejected. A fresh bounded attempt can read
    // the same held pixels without confusing media age with job age.
    QTRY_VERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
    QCOMPARE(h.controller->RejectedResults(), std::uint64_t(1));
    QTest::qWait(250);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
}

void UltraClearTextControllerTests::invalidSelectedMediaDoesNotResumeHiddenLiveInput()
{
    ImmutableHarness h;
    auto scene = h.Scene();
    scene.selected.pixels.reset();
    QVERIFY(!h.controller->SetImmutableScene(scene));
    h.Enable();
    QVERIFY(h.controller->HasImmutableScene());
    UctFrameContext live;
    live.cameraActive = true;
    QVERIFY(!h.controller->OnPresented(live));
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
    QVERIFY(h.Reading().isEmpty());
}

void UltraClearTextControllerTests::advancingMediaPacesOcrAndValidatesLatestFrames()
{
    ImmutableHarness h;
    h.Enable();
    UctAdvancingMediaScene last;
    for(std::uint64_t frame=1;frame<=100;++frame) last=h.PlayFrame(frame);
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QVERIFY(h.controller->HasAdvancingMediaScene());
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    const auto jobs=h.controller->SchedulerStatistics().submitted;
    QVERIFY2(jobs>=2 && jobs<=7,"Two seconds of 50fps media must not submit OCR per frame");
    const auto probes=h.controller->ProbeStatistics();
    QVERIFY(probes.submitted>=10 && probes.submitted<=26);
    QVERIFY(probes.completed>=10);
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    const auto& line=h.controller->Model().Lines().first();
    QVERIFY(line.validatedFrameSeq>=90);
    QVERIFY(line.provenance.timestampMs>=10000 && line.provenance.timestampMs<=12000);
    QVERIFY(line.validatedAtMs>=11800 && line.validatedAtMs<=12000);
    QCOMPARE(h.controller->Model().SceneWords(last.current.timestampMs,last.current.frameSeq).size(),
             h.overlay.WordCount());
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->PixelAllocations(),std::uint64_t(0)); // shared scene leases, no viewport copy
    const auto generation=h.controller->GeometryGeneration();
    const auto submitted=h.controller->ProbeStatistics().submitted;
    last.transform=ComputeViewTransform(960,540,960,540,2,.3f,.3f,ViewportFitMode::kFill);
    QVERIFY(h.controller->OnAdvancingMediaPresented(last)); // retained frame/view receipt only
    QCOMPARE(h.controller->GeometryGeneration(),generation);
    QCOMPARE(h.controller->ProbeStatistics().submitted,submitted);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,jobs);
}

void UltraClearTextControllerTests::partialProbeInvalidationRequestsPromptReplacement_data()
{
    QTest::addColumn<bool>("advancing");
    QTest::newRow("live-camera") << false;
    QTest::newRow("advancing-saved-video") << true;
}

void UltraClearTextControllerTests::partialProbeInvalidationRequestsPromptReplacement()
{
    QFETCH(bool, advancing);
    UctTiming timing;
    ImmutableHarness h(timing);
    QVector<LayoutWord> layout;
    for (int row = 0; row < 6; ++row)
        for (int column = 0; column < 4; ++column)
            layout.push_back({QStringLiteral("Reference"), QPointF(25 + column * 225, 25 + row * 60), 24});
    layout.push_back({QStringLiteral("Before"), QPointF(30, 445), 24});
    const auto original = MakeLayoutPage(layout, 960, 540);
    layout.last().text = QStringLiteral("Afterward");
    const auto changed = MakeLayoutPage(layout, 960, 540);
    h.page = original;
    h.script->words = original.words;
    h.script->separateWordLines = true;
    SourceStamp stamp;
    stamp.viewTransform.valid = true;
    stamp.viewportWidth = 960;
    stamp.viewportHeight = 540;
    const auto beforeProbe = MakeProbeFrame(original.image.constBits(), 960, 540,
        original.image.bytesPerLine(), timing.probeDecimation, stamp);
    const auto afterProbe = MakeProbeFrame(changed.image.constBits(), 960, 540,
        changed.image.bytesPerLine(), timing.probeDecimation, stamp);
    const auto similarity = SceneSimilarity(beforeProbe, afterProbe);
    QVERIFY(similarity.has_value());
    QVERIFY2(*similarity >= timing.stableSimilarity,
             "The fixture must be globally stable while one real line changes");
    const auto geometry = h.Scene().geometry;
    std::uint64_t sequence = 0;
    std::uint64_t observedSubmissions = 0;
    auto lastSubmitWall = h.now;
    bool omitStaticWords = false;
    const auto present = [&] {
        ++sequence;
        h.now += timing.probeIntervalMs;
        {
            std::lock_guard lock(h.script->mutex);
            h.script->words = omitStaticWords ? QVector<PageWord>{h.page.words.last()} : h.page.words;
        }
        if (advancing) {
            UctAdvancingMediaScene scene;
            scene.geometry = geometry;
            scene.sourceAuthority = 3;
            scene.current = {OwnedPixels(IndependentFramePixels(h.page.image, sequence)), sequence,
                             10000 + static_cast<std::int64_t>(sequence) * timing.probeIntervalMs};
            scene.transform.valid = true;
            scene.viewportWidth = 960;
            scene.viewportHeight = 540;
            h.controller->OnAdvancingMediaPresented(std::move(scene));
        } else {
            UctFrameContext context;
            context.geometry = geometry;
            context.requested = geometry.settings;
            context.frameSeq = sequence;
            context.cameraActive = true;
            context.transform.valid = true;
            context.viewportWidth = 960;
            context.viewportHeight = 540;
            const bool wanted = h.controller->OnPresented(context);
            h.controller->OnPresentationSucceeded();
            if (wanted) h.controller->OnCpuViewport(context, h.page.image.constBits(), 960, 540,
                                                   h.page.image.bytesPerLine());
        }
        QTest::qWait(12);
        const auto submitted = h.controller->SchedulerStatistics().submitted;
        if (submitted != observedSubmissions) {
            observedSubmissions = submitted;
            lastSubmitWall = h.now;
        }
    };
    h.Enable();
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
    for (int tick = 0; tick < 80 && h.overlay.WordCount() != original.words.size(); ++tick) present();
    QCOMPARE(h.overlay.WordCount(), original.words.size());
    QVERIFY(h.Reading().contains(QStringLiteral("Before")));
    QTRY_COMPARE(h.controller->SchedulerStatistics().completed, h.controller->SchedulerStatistics().submitted);
    QTest::qWait(20);
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    const auto generation = h.controller->GeometryGeneration();
    const auto proofWall = h.now;
    const auto priorSubmitWall = lastSubmitWall;
    {
        std::lock_guard lock(h.script->mutex);
        h.script->holdRecognition = true;
    }
    h.page = changed;
    present();
    QTRY_VERIFY(!h.Reading().contains(QStringLiteral("Before")));
    QCOMPARE(h.overlay.WordCount(), original.words.size() - 1);
    QVERIFY(h.Reading().contains(QStringLiteral("Reference"))); // surviving accepted lines keep coverage
    QVERIFY(!h.Reading().contains(QStringLiteral("Afterward"))); // no new proof from a probe
    for (int tick = 1; tick < 5 && h.controller->SchedulerStatistics().submitted == jobs; ++tick) present();
    QVERIFY2(h.controller->SchedulerStatistics().submitted > jobs,
             "Real partial invalidation waited for the 1500-ms periodic refresh despite remaining accepted coverage");
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs + 1);
    const auto firstReplacementDelay = h.now - proofWall;
    QVERIFY(firstReplacementDelay <= 5 * timing.probeIntervalMs);
    QVERIFY(h.now - priorSubmitWall < timing.refreshIntervalMs);
    if (advancing) QVERIFY(h.now - priorSubmitWall >= timing.mediaRecognitionIntervalMs);
    for (int tick = 0; tick < 3; ++tick) present();
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs + 1); // held inference gets no replacement backlog
    QCOMPARE(h.overlay.WordCount(), original.words.size() - 1);
    h.script->Release();
    QTest::qWait(20);
    while (h.now - proofWall < 1200 && !h.Reading().contains(QStringLiteral("Afterward"))) present();
    QVERIFY(h.Reading().contains(QStringLiteral("Afterward")));
    QVERIFY(!h.Reading().contains(QStringLiteral("Before")));
    QCOMPARE(h.overlay.WordCount(), changed.words.size());
    QVERIFY(h.now - proofWall < timing.refreshIntervalMs);
    QCOMPARE(h.controller->GeometryGeneration(), generation);

    // A later full-view detection miss must not discard independently checked
    // static regions. Exercise the real probe -> scheduler -> admission path.
    QTRY_COMPARE(h.controller->SchedulerStatistics().completed, h.controller->SchedulerStatistics().submitted);
    const auto staticWords = h.controller->Model().SceneWords(
        advancing ? 10000 + static_cast<std::int64_t>(sequence) * timing.probeIntervalMs : h.now, sequence);
    QCOMPARE(staticWords.size(), changed.words.size());
    const auto priorCompleted = h.controller->SchedulerStatistics().completed;
    omitStaticWords = true;
    for (int tick = 0; tick < 30 && h.controller->SchedulerStatistics().completed == priorCompleted; ++tick) {
        present();
        QCOMPARE(h.overlay.WordCount(), changed.words.size());
    }
    QTRY_VERIFY(h.controller->SchedulerStatistics().completed > priorCompleted);
    QTest::qWait(20); // consume the completion's queued admission
    QCOMPARE(h.overlay.WordCount(), changed.words.size());
    const auto refreshed = h.controller->Model().SceneWords(
        advancing ? 10000 + static_cast<std::int64_t>(sequence) * timing.probeIntervalMs : h.now, sequence);
    for (const auto& word : staticWords) {
        if (word.text != QStringLiteral("Reference")) continue;
        QVERIFY(std::any_of(refreshed.begin(), refreshed.end(), [&](const auto& kept) {
            return kept.text == word.text && kept.lineId == word.lineId && kept.sceneRect == word.sceneRect;
        }));
    }
}

void UltraClearTextControllerTests::advancingMediaValidatesWhileOcrHeldAndRejectsChangedPixels()
{
    ImmutableHarness h;
    h.Enable();
    for(std::uint64_t frame=1;frame<=60;++frame) h.PlayFrame(frame);
    QTRY_VERIFY(h.overlay.WordCount()>0);
    const auto validated=h.controller->Model().Lines().first().validatedFrameSeq;
    const int calls=h.script->calls.load();
    {
        std::lock_guard lock(h.script->mutex);
        h.script->holdRecognition=true;
    }
    std::uint64_t frame=61;
    for(;frame<=180 && h.script->calls.load()==calls;++frame) h.PlayFrame(frame);
    QTRY_VERIFY(h.script->calls.load()>calls);
    const auto jobs=h.controller->SchedulerStatistics().submitted;
    for(int i=0;i<25;++i) h.PlayFrame(frame++);
    QVERIFY(h.overlay.WordCount()>0);
    QVERIFY(h.controller->Model().Lines().first().validatedFrameSeq>validated);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.controller->SchedulerStatistics().submitted,jobs); // held inference gets no replacement queue

    h.page=MakePage({QStringLiteral("Completely changed content")},QPointF(190,90),960,540,48);
    for(int i=0;i<12;++i) h.PlayFrame(frame++);
    QTRY_COMPARE(h.overlay.WordCount(),0);
    QVERIFY(!h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.script->Release();
    QTest::qWait(20);
    QVERIFY(!h.Reading().contains(QStringLiteral("Reading helps")));
}

void UltraClearTextControllerTests::advancingMediaAuthorityRevokesAndSuppressesLiveInput()
{
    ImmutableHarness h;
    h.script->holdRecognition=true;
    h.Enable();
    std::uint64_t frame=1;
    for(;frame<40 && h.script->calls.load()==0;++frame) h.PlayFrame(frame);
    QTRY_COMPARE(h.script->calls.load(),1);
    const auto oldGeneration=h.controller->GeometryGeneration();
    h.PlayFrame(frame++,9);
    QVERIFY(h.controller->GeometryGeneration()>oldGeneration);
    const auto generation=h.controller->GeometryGeneration();
    QVERIFY(h.Reading().isEmpty());
    h.script->Release();
    for(int i=0;i<70;++i) h.PlayFrame(frame++,9);
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QVERIFY(h.controller->SchedulerStatistics().revoked>=1);
    QCOMPARE(h.controller->Model().Lines().first().provenance.geometryGeneration,generation);
    UctFrameContext camera;
    camera.cameraActive=true;camera.frameSeq=999;camera.geometry.settings.sourceEpoch=999;
    const auto probes=h.controller->ProbeStatistics().submitted;
    const auto jobs=h.controller->SchedulerStatistics().submitted;
    QVERIFY(!h.controller->OnPresented(camera));
    h.controller->OnCpuViewport(camera,h.page.image.constBits(),960,540,h.page.image.bytesPerLine());
    h.controller->OnDeviceScene(camera,{});
    h.controller->OnSourceRetired();h.controller->OnPresentationFailed();
    QCOMPARE(h.controller->GeometryGeneration(),generation);
    QCOMPARE(h.controller->ProbeStatistics().submitted,probes);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,jobs);
    QVERIFY(h.overlay.WordCount()>0);
    h.controller->ClearImmutableScene();
    QVERIFY(!h.controller->HasImmutableScene());
    QVERIFY(h.Reading().isEmpty());
}

void UltraClearTextControllerTests::advancingMediaPauseReturnsToOneShotAnalysis()
{
    ImmutableHarness h;
    h.Enable();
    UctAdvancingMediaScene last;
    for(std::uint64_t frame=1;frame<=60;++frame) last=h.PlayFrame(frame);
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QTest::qWait(20);
    const auto before=h.controller->SchedulerStatistics().submitted;
    UctImmutableScene paused;
    paused.geometry=last.geometry;paused.sourceAuthority=last.sourceAuthority;
    paused.selected=last.current;paused.transform=last.transform;
    paused.viewportWidth=last.viewportWidth;paused.viewportHeight=last.viewportHeight;
    paused.corroboration=UctImmutableFrame{
        OwnedPixels(IndependentFramePixels(h.page.image,last.current.frameSeq+1)),
        last.current.frameSeq+1,last.current.timestampMs+20};
    QVERIFY(h.controller->SetImmutableScene(paused));
    QVERIFY(!h.controller->HasAdvancingMediaScene());
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,before+2);
    const auto probes=h.controller->ProbeStatistics().submitted;
    h.now+=120000;
    QTest::qWait(250);
    QVERIFY(h.overlay.WordCount()>0);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,before+2);
    QCOMPARE(h.controller->ProbeStatistics().submitted,probes);
    QVERIFY(h.controller->SetImmutableScene(paused));
    QCOMPARE(h.controller->SchedulerStatistics().submitted,before+2);
}

void UltraClearTextControllerTests::pausedMediaRecoversTransientRecognition_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("input-fault") << 0;
    QTest::newRow("backend-preparing") << 1;
    QTest::newRow("cancelled-current-job") << 2;
}

void UltraClearTextControllerTests::pausedMediaRecoversTransientRecognition()
{
    QFETCH(int, kind);
    ImmutableHarness h;h.Enable();
    UctAdvancingMediaScene last;
    for(std::uint64_t frame=1;frame<=60;++frame) last=h.PlayFrame(frame);
    QTRY_VERIFY(h.overlay.WordCount()>0);QTest::qWait(20);
    const auto before=h.controller->SchedulerStatistics().submitted;
    h.script->transientKind=kind;h.script->transientFailures=1;
    auto paused=h.Scene(last.current.frameSeq);
    paused.selected=last.current;
    paused.corroboration=UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image,last.current.frameSeq+1)),
        last.current.frameSeq+1,last.current.timestampMs+20};
    QVERIFY(h.controller->SetImmutableScene(paused));
    QTRY_VERIFY_WITH_TIMEOUT(h.overlay.WordCount()>0,5000);
    QCOMPARE(h.script->transientFailures.load(),0);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,before+3); // failed + selected + neighbour
    const auto complete=h.controller->SchedulerStatistics().submitted;
    h.now+=15000;QTest::qWait(700);
    QVERIFY(h.controller->SetImmutableScene(paused));
    QCOMPARE(h.controller->SchedulerStatistics().submitted,complete);
    QVERIFY(h.overlay.WordCount()>0); // a successful held read stays one-shot
}

void UltraClearTextControllerTests::pausedMediaRetryCannotReviveReplacedScene()
{
    ImmutableHarness h;h.script->transientKind=1;h.script->transientFailures=1;
    h.Enable();QVERIFY(h.controller->SetImmutableScene(h.Scene(10)));
    QTRY_COMPARE(h.script->transientFailures.load(),0);
    QTest::qWait(20); // failed completion schedules the delayed preparation retry
    auto newer=h.Scene(20);
    newer.corroboration=UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image,21)),21,20};
    QVERIFY(h.controller->SetImmutableScene(newer));
    QTRY_VERIFY(h.overlay.WordCount()>0);
    const auto submitted=h.controller->SchedulerStatistics().submitted;
    QTest::qWait(700);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,submitted);
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq,std::uint64_t(20));
}

void UltraClearTextControllerTests::pausedBridgeRefinesAfterCorroboration()
{
    ImmutableHarness h;h.script->completeBridgeResults=1;h.script->holdCall=4;h.Enable();
    auto scene=h.Scene(10);
    scene.corroboration=UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image,11)),11,20};
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount()>0); // complete fallback and neighbour may show first
    QTRY_COMPARE_WITH_TIMEOUT(h.script->calls.load(),4,5000);
    // The complete old pair stays visible/accessibly readable while the new
    // selected result waits for its held neighbour. No selected-only flicker.
    QVERIFY(h.overlay.WordCount()>0);
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    h.script->Release();
    QTRY_VERIFY(h.overlay.WordCount()>0);
    QTest::qWait(700);
    QCOMPARE(h.controller->SchedulerStatistics().submitted,std::uint64_t(4));
}

void UltraClearTextControllerTests::pausedRefinementCannotDropProvenWords_data()
{
    QTest::addColumn<bool>("selectedOmitsWords");
    QTest::addColumn<bool>("residentBridge");
    QTest::newRow("selected-result-omits-proven-words") << true << false;
    QTest::newRow("neighbour-result-omits-proven-words") << false << false;
    QTest::newRow("resident-cpu-bridge-selected-omits-proven-words") << true << true;
    QTest::newRow("resident-cpu-bridge-neighbour-omits-proven-words") << false << true;
}

void UltraClearTextControllerTests::pausedRefinementCannotDropProvenWords()
{
    QFETCH(bool, selectedOmitsWords);
    QFETCH(bool, residentBridge);
    AccessibilityActivation accessibility;
    ImmutableHarness h;
    h.page = MakePage({QStringLiteral("Reading helps"), QStringLiteral("Steady words")}, {},
                      960, 540, 48);
    h.script->words = h.page.words;
    const QVector<PageWord> partial{h.page.words.first()};
    if (selectedOmitsWords) h.script->callWords.insert(3, partial);
    h.script->callWords.insert(4, partial);
    h.script->completeBridgeResults = 1;
    h.script->permittedCalls = 3;
    h.devices->resident = residentBridge;
    if(residentBridge)h.script->wantsDeviceAfterCalls = 2;
    auto scene = residentBridge ? h.DeviceScene(10) : h.Scene(10);
    scene.corroboration = residentBridge
        ? UctImmutableFrame{{}, 11, 20, h.Device(IndependentFramePixels(h.page.image, 11), 11, 20)}
        : UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image, 11)), 11, 20};
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(h.overlay.WordCount(), h.page.words.size());
    QTRY_COMPARE_WITH_TIMEOUT(h.script->calls.load() + h.script->deviceCalls.load(), 4, 5000);
    const auto proven = h.controller->Model().SceneWords(h.now, scene.selected.frameSeq);
    const QString reading = h.Reading();
    QVERIFY(reading.contains(QStringLiteral("Steady words")));
    QVERIFY(h.controller->ScreenTextAccessibility()->ExposedLineCount() > 0);

    h.script->Release();
    QTRY_COMPARE(admitted.size(), 3); // selected + fallback neighbour + preferred neighbour
    if(residentBridge) {
        QCOMPARE(h.script->calls.load(), 2);
        QCOMPARE(h.script->deviceCalls.load(), 2);
        QCOMPARE(h.devices->residentPreparations.load(), 4);
        QCOMPARE(h.devices->readbacks.load(), 2); // explicit CPU bridge OCR only
        QCOMPARE(h.devices->probes.load(), 0);
        QVERIFY(!h.devices->readOnUiThread.load());
    }
    QCOMPARE(h.overlay.WordCount(), h.page.words.size());
    QCOMPARE(h.Reading(), reading);
    const auto refined = h.controller->Model().SceneWords(h.now, scene.selected.frameSeq);
    for (const auto& before : proven) {
        QVERIFY(std::any_of(refined.begin(), refined.end(), [&](const auto& after) {
            return after.text == before.text && after.sceneRect == before.sceneRect;
        }));
    }
    QVERIFY(h.controller->ScreenTextAccessibility()->ExposedLineCount() > 0);
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    h.now += 3600000;
    h.controller->UpdateImmutableView(scene.transform, 960, 540);
    QTest::qWait(700);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.overlay.WordCount(), h.page.words.size());
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());

    // An actual seek/source ticket still revokes the proven artifact at once.
    h.script->holdRecognition = true;
    auto newer = scene;
    ++newer.sourceAuthority;
    newer.selected.frameSeq = 30;
    if(residentBridge)newer.selected.device = h.Device(h.page.image, 30, newer.selected.timestampMs);
    QVERIFY(h.controller->SetImmutableScene(newer));
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
}

void UltraClearTextControllerTests::pausedRefinementContradictionCannotRestorePriorProof_data()
{
    QTest::addColumn<bool>("residentBridge");
    QTest::addColumn<bool>("mixedConflict");
    QTest::newRow("cpu-all-words-conflict") << false << false;
    QTest::newRow("resident-bridge-all-words-conflict") << true << false;
    QTest::newRow("cpu-conflict-preserves-safe-sibling") << false << true;
    QTest::newRow("resident-bridge-conflict-preserves-safe-sibling") << true << true;
}

void UltraClearTextControllerTests::pausedRefinementContradictionCannotRestorePriorProof()
{
    QFETCH(bool, residentBridge);
    QFETCH(bool, mixedConflict);
    ImmutableHarness h;
    auto conflicting = h.page.words;
    conflicting.first().text = QStringLiteral("Writing");
    if(!mixedConflict)conflicting.last().text = QStringLiteral("hinders");
    h.script->callWords.insert(3, conflicting);
    h.script->callWords.insert(4, conflicting);
    h.script->completeBridgeResults = 1;
    h.script->permittedCalls = 3;
    h.devices->resident = residentBridge;
    if(residentBridge)h.script->wantsDeviceAfterCalls = 2;
    auto scene = residentBridge ? h.DeviceScene() : h.Scene();
    scene.corroboration = residentBridge
        ? UctImmutableFrame{{}, 11, 20, h.Device(IndependentFramePixels(h.page.image, 11), 11, 20)}
        : UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image, 11)), 11, 20};
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(h.overlay.WordCount(), h.page.words.size());
    QTRY_COMPARE_WITH_TIMEOUT(h.script->calls.load() + h.script->deviceCalls.load(), 4, 5000);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    const auto proven = h.controller->Model().SceneWords(h.now, scene.selected.frameSeq);
    QVERIFY(proven.size()==2);
    h.script->Release();
    QTRY_COMPARE(admitted.size(), 3);
    if(residentBridge) {
        QCOMPARE(h.script->calls.load(), 2);
        QCOMPARE(h.script->deviceCalls.load(), 2);
        QCOMPARE(h.devices->residentPreparations.load(), 4);
        QCOMPARE(h.devices->readbacks.load(), 2);
        QCOMPARE(h.devices->probes.load(), 0);
        QVERIFY(!h.devices->readOnUiThread.load());
    }
    QCOMPARE(admitted.last().first().toInt(), static_cast<int>(mixedConflict
        ? AdmitVerdict::kAccepted : AdmitVerdict::kContentMoved));
    if(mixedConflict) {
        QCOMPARE(h.overlay.WordCount(), 1);
        const auto retained = h.controller->Model().SceneWords(h.now, scene.selected.frameSeq);
        QCOMPARE(retained.size(), 1);
        QCOMPARE(retained.first().text, QStringLiteral("helps"));
        QCOMPARE(retained.first().lineId, proven.last().lineId);
        QCOMPARE(retained.first().wordIndex, proven.last().wordIndex);
        QCOMPARE(retained.first().sceneRect, proven.last().sceneRect);
        QVERIFY(h.Reading().contains(QStringLiteral("helps")));
        QVERIFY(!h.Reading().contains(QStringLiteral("Reading")));
        QVERIFY(!h.Reading().contains(QStringLiteral("Writing")));
        const auto screen = h.controller->ScreenTextSnapshot();
        QVERIFY(!screen.lines.isEmpty());
        QVERIFY(!screen.lines.first().text.contains(QStringLiteral("Reading")));
        QVERIFY(!screen.lines.first().text.contains(QStringLiteral("Writing")));
    } else {
        QCOMPARE(h.overlay.WordCount(), 0);
        QVERIFY(h.Reading().isEmpty());
        QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    }
    h.controller->UpdateImmutableView(scene.transform, 960, 540);
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    QCOMPARE(h.overlay.WordCount(), mixedConflict ? 1 : 0); // view changes cannot revive revoked words
    QVERIFY(!h.Reading().contains(QStringLiteral("Reading")));
    QVERIFY(!h.Reading().contains(QStringLiteral("Writing")));
}

void UltraClearTextControllerTests::pausedBridgeHorizonKeepsHealthyFallback()
{
    UctTiming timing;timing.immutableJobTimeoutMs=1000;
    ImmutableHarness h(timing);h.script->completeBridgeResults=100;h.Enable();
    auto scene=h.Scene(10);
    scene.corroboration=UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image,11)),11,20};
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount()>0);
    h.now+=1001; // only preferred preparation has expired, fallback proof is valid
    QTRY_COMPARE_WITH_TIMEOUT(h.script->calls.load(),4,5000);
    QTest::qWait(700);
    QCOMPARE(h.script->calls.load(),4);
    QVERIFY(h.overlay.WordCount()>0);
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("Showing sharp text")));
}

void UltraClearTextControllerTests::pausedFailureRetriesAreBounded()
{
    ImmutableHarness h;h.script->transientKind=0;h.script->transientFailures=10;h.Enable();
    QVERIFY(h.controller->SetImmutableScene(h.Scene(10)));
    QTRY_COMPARE(h.script->calls.load(),3);
    QTest::qWait(700);QCOMPARE(h.script->calls.load(),3);
    QVERIFY(h.overlay.WordCount()==0);
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("stopped working")));
    h.script->transientFailures=0;
    auto newer=h.Scene(20);
    newer.corroboration=UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image,21)),21,20};
    QVERIFY(h.controller->SetImmutableScene(newer));
    QTRY_VERIFY(h.overlay.WordCount()>0); // a new source ticket can recover
}

void UltraClearTextControllerTests::pausedRefinementFailureKeepsOnlyProvenArtifact_data()
{
    QTest::addColumn<bool>("priorProof");
    QTest::addColumn<int>("failure");
    for (bool priorProof : {false, true}) {
        const QByteArray prefix = priorProof ? "proven-" : "unproven-";
        QTest::newRow((prefix + "backend-error").constData()) << priorProof << 0;
        QTest::newRow((prefix + "input-fault").constData()) << priorProof << 1;
        QTest::newRow((prefix + "cancelled-current-job").constData()) << priorProof << 2;
        QTest::newRow((prefix + "wall-deadline").constData()) << priorProof << 3;
        QTest::newRow((prefix + "terminal-unavailable").constData()) << priorProof << 4;
    }
}

void UltraClearTextControllerTests::pausedRefinementFailureKeepsOnlyProvenArtifact()
{
    QFETCH(bool, priorProof);
    QFETCH(int, failure);
    AccessibilityActivation accessibility;
    UctTiming timing;
    timing.immutableJobTimeoutMs = 100;
    ImmutableHarness h(timing);
    const int firstFailure = priorProof ? 3 : 1;
    h.script->permittedCalls = firstFailure - 1;
    h.script->completeBridgeResults = priorProof ? 1 : 0;
    auto scene = h.Scene();
    scene.corroboration = UctImmutableFrame{
        OwnedPixels(IndependentFramePixels(h.page.image, 11)), 11, 20};
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE_WITH_TIMEOUT(h.script->calls.load(), firstFailure, 5000);
    const auto proven = h.controller->Model().Lines();
    const QString reading = h.Reading();
    const auto accepted = h.controller->AcceptedResults();
    const auto generation = h.controller->GeometryGeneration();
    if (priorProof) {
        QCOMPARE(h.overlay.WordCount(), h.page.words.size());
        QVERIFY(!proven.isEmpty());
        QVERIFY(!reading.isEmpty());
        QVERIFY(h.controller->ScreenTextAccessibility()->ExposedLineCount() > 0);
    } else {
        QVERIFY(proven.isEmpty());
        QVERIFY(reading.isEmpty());
        QCOMPARE(h.overlay.WordCount(), 0);
    }

    const int lastFailure = firstFailure + ((failure == 2 || failure == 3) ? 3 :
                                            failure == 4 ? 0 : 2);
    if (failure == 3) {
        // Every retry completes with an otherwise healthy output after its
        // own wall deadline. Media PTS and selected pixels never change.
        for (int call = firstFailure; call <= lastFailure; ++call) {
            QTRY_COMPARE(h.script->calls.load(), call);
            const auto rejected = h.controller->RejectedResults();
            h.now += timing.immutableJobTimeoutMs + 1;
            {
                std::lock_guard lock(h.script->mutex);
                h.script->permittedCalls = call;
            }
            h.script->changed.notify_all();
            QTRY_COMPARE(h.controller->RejectedResults(), rejected + 1);
        }
    } else {
        h.script->transientKind = failure == 1 ? 0 : failure == 2 ? 2 : 3;
        if (failure == 4) {
            // The terminal status must fence even an otherwise successful
            // in-flight output, before it can publish any new evidence.
            h.script->unavailableAtCall = firstFailure;
        } else h.script->transientFailures = 10;
        h.script->Release();
        QTRY_COMPARE(h.script->calls.load(), lastFailure);
    }
    QTest::qWait(700); // drain retries; exhausted optional work must stop
    QCOMPARE(h.script->calls.load(), lastFailure);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(lastFailure));
    QCOMPARE(h.controller->AcceptedResults(), accepted); // failures never admit new proof
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    if (failure == 0) {
        QCOMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kFailed);
        QCOMPARE(h.controller->BackendStatusSnapshot().detail, QStringLiteral("fixture recognition failure"));
    } else if (failure == 4) {
        QCOMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kUnavailable);
        QCOMPARE(h.controller->BackendStatusSnapshot().detail, QStringLiteral("fixture backend chain exhausted"));
    } else QCOMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);

    h.controller->UpdateImmutableView(scene.transform, 960, 540);
    if (!priorProof) {
        QVERIFY(h.controller->Model().Lines().isEmpty());
        QCOMPARE(h.overlay.WordCount(), 0);
        QVERIFY(h.Reading().isEmpty());
        QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
        QCOMPARE(h.controller->ScreenTextAccessibility()->ExposedLineCount(), 0);
        QVERIFY(!h.controller->StatusSource().contains(QStringLiteral("Showing sharp text")));
        return; // existing no-prior-proof terminal behavior remains fail-closed
    }

    const auto& retained = h.controller->Model().Lines();
    QCOMPARE(retained.size(), proven.size());
    for (int i = 0; i < proven.size(); ++i) {
        const auto& before = proven[i];
        const auto& after = retained[i];
        QCOMPARE(after.id, before.id);
        QCOMPARE(after.sceneRect, before.sceneRect);
        QCOMPARE(after.sceneOffset, before.sceneOffset);
        QCOMPARE(after.readingTokens, before.readingTokens);
        QCOMPARE(after.readingWordIndices, before.readingWordIndices);
        QCOMPARE(after.protectedSceneRects, before.protectedSceneRects);
        QCOMPARE(after.carvedSceneRects, before.carvedSceneRects);
        QCOMPARE(after.validatedAtMs, before.validatedAtMs);
        QCOMPARE(after.validatedFrameSeq, before.validatedFrameSeq);
        QCOMPARE(after.provenance.sourceEpoch, before.provenance.sourceEpoch);
        QCOMPARE(after.provenance.geometryGeneration, before.provenance.geometryGeneration);
        QCOMPARE(after.provenance.frameSeq, before.provenance.frameSeq);
        QCOMPARE(after.provenance.timestampMs, before.provenance.timestampMs);
        QCOMPARE(after.reference.sceneRect, before.reference.sceneRect);
        QCOMPARE(after.reference.values, before.reference.values);
        QCOMPARE(after.reference.resident, before.reference.resident);
        QCOMPARE(after.words.size(), before.words.size());
        for (int word = 0; word < before.words.size(); ++word) {
            QCOMPARE(after.words[word].text, before.words[word].text);
            QCOMPARE(after.words[word].sceneRect, before.words[word].sceneRect);
            QCOMPARE(after.words[word].glyphEligible, before.words[word].glyphEligible);
        }
    }
    QCOMPARE(h.overlay.WordCount(), h.page.words.size());
    QCOMPARE(h.Reading(), reading);
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QVERIFY(h.controller->ScreenTextAccessibility()->ExposedLineCount() > 0);
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("Showing sharp text")));
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    h.now += 3600000;
    h.script->transientFailures = 0;
    h.script->unavailableAtCall = 0;
    QVERIFY(h.controller->SetImmutableScene(scene));
    h.controller->UpdateImmutableView(scene.transform, 960, 540);
    QTest::qWait(250);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.overlay.WordCount(), h.page.words.size());
    QCOMPARE(h.Reading(), reading);

    h.controller->SetShowOriginal(true);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.Reading(), reading); // existing Show original pane contract
    h.controller->SetShowOriginal(false);
    QCOMPARE(h.overlay.WordCount(), h.page.words.size());
    h.controller->OnMediaPresentationFailed();
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QCOMPARE(h.controller->ScreenTextAccessibility()->ExposedLineCount(), 0);
    h.controller->UpdateImmutableView(scene.transform, 960, 540);
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    QCOMPARE(h.overlay.WordCount(), 0); // a view/toggle cannot clear the presentation latch
    QVERIFY(h.Reading().isEmpty());
    h.controller->OnMediaPresentationSucceeded();
    QCOMPARE(h.overlay.WordCount(), h.page.words.size());
    QCOMPARE(h.Reading(), reading);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    h.controller->SetEnabled(false);
    QVERIFY(h.controller->Model().Lines().isEmpty());
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
}

void UltraClearTextControllerTests::pausedReadingOnlyCandidateStillFailsClosed()
{
    ImmutableHarness h;
    h.script->completeBridgeResults = 1;
    h.script->permittedCalls = 1;
    auto scene = h.Scene(); // no independent neighbour: reading-only evidence
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE_WITH_TIMEOUT(h.script->calls.load(), 2, 5000);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(!h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.script->transientKind = 0;
    h.script->transientFailures = 10;
    h.script->Release();
    QTRY_COMPARE(h.script->calls.load(), 4);
    QTest::qWait(250);
    h.controller->UpdateImmutableView(scene.transform, 960, 540);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QVERIFY(h.controller->StatusSource().contains(QStringLiteral("stopped working")));
}

void UltraClearTextControllerTests::failedPausedRefinementStillFencesSceneChanges_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("source-epoch") << 0;
    QTest::newRow("source-authority") << 1;
    QTest::newRow("processed-geometry") << 2;
    QTest::newRow("selected-frame") << 3;
    QTest::newRow("selected-pts") << 4;
    QTest::newRow("different-owned-lease-same-bytes") << 5;
    QTest::newRow("different-selected-pixels") << 6;
    QTest::newRow("clear-scene") << 7;
    QTest::newRow("processing-suppressed") << 8;
}

void UltraClearTextControllerTests::failedPausedRefinementStillFencesSceneChanges()
{
    QFETCH(int, change);
    ImmutableHarness h;
    h.script->completeBridgeResults = 1;
    h.script->permittedCalls = 2;
    auto scene = h.Scene();
    scene.corroboration = UctImmutableFrame{
        OwnedPixels(IndependentFramePixels(h.page.image, 11)), 11, 20};
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE_WITH_TIMEOUT(h.script->calls.load(), 3, 5000);
    QCOMPARE(h.overlay.WordCount(), h.page.words.size());
    h.script->transientKind = 3;
    h.script->transientFailures = 10;
    h.script->Release();
    QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kFailed);
    QCOMPARE(h.overlay.WordCount(), h.page.words.size());
    const auto generation = h.controller->GeometryGeneration();
    if (change == 7) h.controller->ClearImmutableScene();
    else if (change == 8) h.controller->SetMediaOverlaySuppressed(true);
    else {
        if (change == 0) ++scene.geometry.settings.sourceEpoch;
        if (change == 1) ++scene.sourceAuthority;
        if (change == 2) ++scene.geometry.settings.colorScheme;
        if (change == 3) ++scene.selected.frameSeq;
        if (change == 4) ++scene.selected.timestampMs;
        if (change == 5) scene.selected.pixels = OwnedPixels(h.page.image);
        if (change == 6) scene.selected.pixels = OwnedPixels(IndependentFramePixels(h.page.image, 12));
        QVERIFY(h.controller->SetImmutableScene(scene));
    }
    QVERIFY(h.controller->GeometryGeneration() > generation);
    QVERIFY(h.controller->Model().Lines().isEmpty());
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.controller->OnMediaPresentationSucceeded(); // a receipt cannot revive revoked source proof
    h.controller->UpdateImmutableView(scene.transform, 960, 540);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
}

void UltraClearTextControllerTests::advancingMediaDeadlineUsesWallClock()
{
    UctTiming timing;timing.immutableJobTimeoutMs=100;
    ImmutableHarness h(timing);
    h.script->holdRecognition=true;
    h.Enable();
    for(std::uint64_t frame=1;frame<40 && h.script->calls.load()==0;++frame) h.PlayFrame(frame);
    QTRY_COMPARE(h.script->calls.load(),1);
    h.now+=101; // media PTS stays fixed; only the current wall job deadline expires
    h.script->Release();
    QTRY_COMPARE(h.controller->RejectedResults(),std::uint64_t(1));
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->Model().Lines().isEmpty());
    QCOMPARE(h.controller->SchedulerStatistics().submitted,std::uint64_t(1));
}

void UltraClearTextControllerTests::mediaPresentationFailureRevokesCachedArtifact_data()
{
    QTest::addColumn<bool>("advancing");
    QTest::newRow("immutable") << false;
    QTest::newRow("advancing") << true;
}

void UltraClearTextControllerTests::mediaPresentationFailureRevokesCachedArtifact()
{
    QFETCH(bool, advancing);
    AccessibilityActivation accessibility;
    ImmutableHarness h;
    h.Enable();
    auto scene = h.Scene();
    if (advancing) {
        for (std::uint64_t frame = 1; frame <= 60; ++frame) h.PlayFrame(frame);
    } else {
        auto neighbour = h.page.image.copy();
        neighbour.setPixelColor(950, 530, Qt::red);
        scene.corroboration = UctImmutableFrame{OwnedPixels(neighbour), 11, 2};
        QVERIFY(h.controller->SetImmutableScene(scene));
    }
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QTest::qWait(20); // drain queued admission before inspecting the retained artifact
    const auto originalWords = h.overlay.Words();
    const auto originalReading = h.Reading();
    QVERIFY(!originalReading.isEmpty());
    const auto originalSnapshot = h.controller->ScreenTextSnapshot();
    QVERIFY(!originalSnapshot.lines.isEmpty());
    auto* screenText = h.controller->ScreenTextAccessibility();
    QVERIFY(screenText);
    QVERIFY(screenText->IsActive());
    QVERIFY(screenText->ExposedLineCount() > 0);
    const auto generation = h.controller->GeometryGeneration();
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    const auto probes = h.controller->ProbeStatistics().submitted;

    h.controller->OnMediaPresentationFailed();
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(screenText->ExposedLineCount(), 0);
    QVERIFY(!h.controller->Model().Lines().isEmpty()); // visible revocation retains valid evidence
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    const auto zoom = ComputeViewTransform(960, 540, 960, 540, 2, .3f, .3f,
                                           ViewportFitMode::kFill);
    h.controller->UpdateImmutableView(zoom, 960, 540);
    h.controller->OnPresentationSucceeded(); // a hidden camera receipt has no media authority
    QTest::qWait(250);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(screenText->ExposedLineCount(), 0);

    h.controller->OnMediaPresentationSucceeded(); // exact retained-frame/native-view receipt
    QCOMPARE(h.overlay.WordCount(), originalWords.size());
    QCOMPARE(h.overlay.Words().first().sceneRect, originalWords.first().sceneRect);
    QCOMPARE(h.Reading(), originalReading);
    const auto restoredSnapshot = h.controller->ScreenTextSnapshot();
    QVERIFY(!restoredSnapshot.lines.isEmpty());
    QCOMPARE(restoredSnapshot.lines.first().text, originalSnapshot.lines.first().text);
    QVERIFY(screenText->ExposedLineCount() > 0);
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.controller->ProbeStatistics().submitted, probes);

    if (!advancing) {
        h.controller->OnMediaPresentationFailed();
        QVERIFY(h.controller->SetImmutableScene(scene)); // exact scene API is itself a real receipt
        QCOMPARE(h.overlay.WordCount(), originalWords.size());
        QCOMPARE(h.Reading(), originalReading);
        QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    }
    h.controller->ClearImmutableScene();
    h.controller->OnMediaPresentationSucceeded(); // a late media receipt cannot restore live text
    QVERIFY(h.Reading().isEmpty());
    QCOMPARE(h.overlay.WordCount(), 0);
}

void UltraClearTextControllerTests::immutablePresentationFailureBlocksBackendReadiness()
{
    ImmutableHarness h;
    h.script->holdInitialization = true;
    auto scene = h.Scene();
    auto neighbour = h.page.image.copy();
    neighbour.setPixelColor(950, 530, Qt::red);
    scene.corroboration = UctImmutableFrame{OwnedPixels(neighbour), 11, 2};
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(h.script->initialized.load(), 1);
    h.controller->OnMediaPresentationFailed();
    h.script->Release(); // readiness and both OCR completions arrive after the presenter fault
    QTRY_COMPARE(h.controller->AcceptedResults(), std::uint64_t(1));
    QCOMPARE(h.script->calls.load(), 2);
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    h.controller->UpdateImmutableView(scene.transform, scene.viewportWidth, scene.viewportHeight);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.controller->OnMediaPresentationSucceeded();
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::advancingPresentationFailureBlocksLateResult()
{
    ImmutableHarness h;
    h.Enable();
    UctAdvancingMediaScene last;
    for (std::uint64_t frame = 1; frame <= 60; ++frame) last = h.PlayFrame(frame);
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    const int calls = h.script->calls.load();
    {
        std::lock_guard lock(h.script->mutex);
        h.script->holdRecognition = true;
    }
    for (std::uint64_t frame = 61; frame <= 180 && h.script->calls.load() == calls; ++frame)
        last = h.PlayFrame(frame);
    QTRY_VERIFY(h.script->calls.load() > calls);
    QVERIFY(h.overlay.WordCount() > 0);
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.controller->OnMediaPresentationFailed();
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.script->Release();
    QTRY_COMPARE(admitted.size(), 1); // an otherwise current late result must not reopen presentation
    h.controller->SetShowOriginal(true);
    h.controller->SetShowOriginal(false);
    h.controller->UpdateImmutableView(last.transform, last.viewportWidth, last.viewportHeight);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    h.controller->OnMediaPresentationSucceeded();
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
}

void UltraClearTextControllerTests::deviceImmutableRecognizesWithoutCpuPixels()
{
    ImmutableHarness h;
    auto scene = h.DeviceScene();
    QVERIFY(!scene.selected.pixels);
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(admitted.size(), 1);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.overlay.WordCount(), 0); // one still is reading evidence only
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    const auto& provenance = h.controller->Model().Lines().first().provenance;
    QCOMPARE(provenance.frameSeq, scene.selected.frameSeq);
    QCOMPARE(provenance.timestampMs, std::int64_t(1)); // real source PTS, never refreshed
    QCOMPARE(provenance.viewportWidth, std::uint32_t(960));
    QCOMPARE(provenance.viewportHeight, std::uint32_t(540));
    // The native CPU recognizer fell back through a worker readback.
    QCOMPARE(h.script->calls.load(), 1);
    QCOMPARE(h.script->deviceCalls.load(), 0);
    QVERIFY(h.devices->readbacks.load() >= 1);
    QVERIFY(!h.devices->readOnUiThread.load());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
    QCOMPARE(h.controller->PixelAllocations(), std::uint64_t(0));
    // Exact device lease repeated: projection only, no second analysis.
    QVERIFY(h.controller->SetImmutableScene(scene));
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
    // Same frame identity with a new device lease is a new selection.
    auto reselected = scene;
    reselected.selected.device = h.Device(h.page.image, 10, 1);
    const auto generation = h.controller->GeometryGeneration();
    QVERIFY(h.controller->SetImmutableScene(reselected));
    QVERIFY(h.controller->GeometryGeneration() > generation);
    QTRY_COMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(2));
}

void UltraClearTextControllerTests::deviceImmutableDistinctCorroborationDrawsGlyphs()
{
    ImmutableHarness h;
    h.script->wantsDevice = true;
    auto scene = h.DeviceScene();
    auto neighbour = h.page.image.copy();
    neighbour.setPixelColor(950, 530, Qt::red); // genuinely distinct pixels
    scene.corroboration = UctImmutableFrame{{}, 11, 2, h.Device(neighbour, 11, 2)};
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QCOMPARE(h.script->deviceCalls.load(), 2);
    QCOMPARE(h.script->calls.load(), 0); // GPU-preferred path, no CPU recognition
    QVERIFY(!h.devices->readOnUiThread.load());
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));
}

void UltraClearTextControllerTests::deviceImmutableIdenticalPixelsNeverCorroborate()
{
    ImmutableHarness h;
    auto scene = h.DeviceScene();
    // Different lease and frame identity, byte-identical scene pixels.
    scene.corroboration = UctImmutableFrame{{}, 11, 2, h.Device(h.page.image, 11, 2)};
    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.Enable();
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(admitted.size(), 2);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QTest::qWait(250);
    QCOMPARE(h.script->calls.load(), 2); // bounded attempt, no retry loop
}

void UltraClearTextControllerTests::deviceImmutableRejectsMismatchedImageAndFallsBackToPixels()
{
    { // dimensions do not cover the declared scene
        ImmutableHarness h;
        auto scene = h.DeviceScene();
        const QImage small(64, 64, QImage::Format_ARGB32);
        scene.selected.device = h.Device(small, 10, 1);
        QVERIFY(!h.controller->SetImmutableScene(scene));
        h.Enable();
        QVERIFY(h.controller->HasImmutableScene()); // live input stays suspended
        QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
        QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
        QVERIFY(h.Reading().isEmpty());
    }
    { // device frame identity disagrees with the frame's authoritative stamp
        ImmutableHarness h;
        auto scene = h.DeviceScene();
        scene.selected.device = h.Device(h.page.image, 99, 1);
        QVERIFY(!h.controller->SetImmutableScene(scene));
        h.Enable();
        QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
        QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
    }
    { // device source epoch disagrees with the scene geometry
        ImmutableHarness h;
        auto scene = h.DeviceScene();
        scene.selected.device = h.Device(h.page.image, 10, 1, 7);
        QVERIFY(!h.controller->SetImmutableScene(scene));
        h.Enable();
        QTRY_COMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
        QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(0));
    }
    { // a broken device lease must not lose UCT when CPU pixels are owned too
        ImmutableHarness h;
        auto scene = h.Scene();
        const QImage small(64, 64, QImage::Format_ARGB32);
        scene.selected.device = h.Device(small, 10, 1);
        h.Enable();
        QVERIFY(h.controller->SetImmutableScene(scene));
        QTRY_VERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
        QCOMPARE(h.script->calls.load(), 1);
    }
}

void UltraClearTextControllerTests::deviceAdvancingMediaPacesAndValidates()
{
    ImmutableHarness h;
    h.Enable();
    UctAdvancingMediaScene last;
    for (std::uint64_t frame = 1; frame <= 120 && h.overlay.WordCount() == 0; ++frame)
        last = h.PlayDeviceFrame(frame);
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.controller->HasAdvancingMediaScene());
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    const std::uint64_t lastFrame = last.current.frameSeq;
    for (std::uint64_t frame = lastFrame + 1; frame <= lastFrame + 40; ++frame)
        last = h.PlayDeviceFrame(frame);
    QVERIFY(h.overlay.WordCount() > 0);
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    QVERIFY2(jobs >= 2 && jobs <= 12, "Advancing device media must pace OCR, not submit per frame");
    QVERIFY(h.devices->probes.load() >= 5);
    QVERIFY(h.controller->ProbeStatistics().completed >= 5);
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    const auto& line = h.controller->Model().Lines().first();
    QVERIFY(line.validatedFrameSeq > line.provenance.frameSeq); // independent probes keep validating
    QVERIFY(line.provenance.timestampMs >= 10000 && line.provenance.timestampMs <= 14000);
    QVERIFY(line.validatedAtMs >= line.provenance.timestampMs);
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->PixelAllocations(), std::uint64_t(0)); // no UI viewport copy
    QVERIFY(!h.devices->readOnUiThread.load());
    QVERIFY(h.script->calls.load() + h.script->deviceCalls.load() >= 2);
}

void UltraClearTextControllerTests::deviceAdvancingSeekAndBackwardFrameRevoke()
{
    ImmutableHarness h;
    h.script->holdRecognition = true;
    h.Enable();
    std::uint64_t frame = 1;
    for (; frame < 60 && h.script->calls.load() == 0; ++frame) h.PlayDeviceFrame(frame);
    QTRY_COMPARE(h.script->calls.load(), 1); // recognition held in flight
    const auto beforeSeek = h.controller->GeometryGeneration();
    h.PlayDeviceFrame(frame++, 9); // seek: new source authority
    QVERIFY(h.controller->GeometryGeneration() > beforeSeek);
    QVERIFY(h.Reading().isEmpty());
    const auto seekGeneration = h.controller->GeometryGeneration();
    h.script->Release();
    for (int i = 0; i < 120 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++, 9);
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.controller->SchedulerStatistics().revoked >= 1);
    QCOMPARE(h.controller->Model().Lines().first().provenance.geometryGeneration, seekGeneration);
    // A backward frame under the same authority is a discontinuity too.
    const auto beforeBackward = h.controller->GeometryGeneration();
    h.PlayDeviceFrame(2, 9);
    QVERIFY(h.controller->GeometryGeneration() > beforeBackward);
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
}

void UltraClearTextControllerTests::deviceAdvancingSlowJobUsesWallDeadline()
{
    UctTiming timing;
    timing.immutableJobTimeoutMs = 100;
    ImmutableHarness h(timing);
    h.script->holdRecognition = true;
    h.Enable();
    for (std::uint64_t frame = 1; frame < 60 && h.script->calls.load() == 0; ++frame)
        h.PlayDeviceFrame(frame);
    QTRY_COMPARE(h.script->calls.load(), 1);
    h.now += 101; // media PTS unchanged; only the wall job deadline expires
    h.script->Release();
    QTRY_COMPARE(h.controller->RejectedResults(), std::uint64_t(1));
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->Model().Lines().isEmpty());
    QCOMPARE(h.controller->SchedulerStatistics().submitted, std::uint64_t(1));
}

void UltraClearTextControllerTests::deviceAdvancingProbeFailureRecovers()
{
    ImmutableHarness h;
    h.devices->probeFails = true;
    h.Enable();
    std::uint64_t frame = 1;
    for (; frame <= 80 && !h.controller->StatusSource().contains(QStringLiteral("stopped working")); ++frame)
        h.PlayDeviceFrame(frame);
    QVERIFY2(h.controller->StatusSource().contains(QStringLiteral("stopped working")),
             qPrintable(h.controller->StatusSource()));
    QCOMPARE(h.controller->BackendStatusSnapshot().state, BackendState::kReady);
    QVERIFY(h.Reading().isEmpty());
    QCOMPARE(h.overlay.WordCount(), 0);
    h.devices->probeFails = false; // copies recover: UCT must not stay lost
    for (int i = 0; i < 160 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++);
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(!h.controller->StatusSource().contains(QStringLiteral("stopped working")));
}

void UltraClearTextControllerTests::deviceAdvancingReleasesConsumedAnalysisLease()
{
    ImmutableHarness h;
    h.Enable();
    std::map<std::uint64_t, std::weak_ptr<const DeviceImage>> leases;
    for (std::uint64_t frame = 1; frame <= 160 && h.overlay.WordCount() == 0; ++frame) {
        const auto scene = h.PlayDeviceFrame(frame);
        leases[frame] = scene.current.device; // the returned scene copy drops here
    }
    QVERIFY(h.overlay.WordCount() > 0);
    const auto provenFrame = h.controller->Model().Lines().first().provenance.frameSeq;
    const auto lease = leases.find(provenFrame);
    QVERIFY(lease != leases.end());
    // Validation and recognition consumed this exact device image. Once they
    // finish, the retained advancing scene must not keep its analysis-pool
    // lease alive merely as receipt metadata...
    QTRY_VERIFY_WITH_TIMEOUT(lease->second.expired(), 5000);
    // ...while the displayed scene remains presentable and its text drawn.
    QVERIFY(h.controller->HasAdvancingMediaScene());
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
}

void UltraClearTextControllerTests::gapReceiptRetainsBrieflyThenMediaExpiryHidesAndCurrentInputRecovers()
{
    ImmutableHarness h;
    h.Enable();
    const std::uint64_t last = h.EstablishDeviceGlyphs();
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(!h.controller->Model().Lines().isEmpty());
    const auto line = h.controller->Model().Lines().first();
    const auto generation = h.controller->GeometryGeneration();
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    const auto probes = h.controller->ProbeStatistics().submitted;
    const int deviceProbes = h.devices->probes.load();
    const int deviceReadbacks = h.devices->readbacks.load();

    // Ordinary forward gap at 4x media speed (20 ms PTS per 5 ms wall):
    // retained while the previous real validation is media-fresh.
    std::uint64_t frame = last + 1;
    while (10000 + static_cast<std::int64_t>(frame) * 20 <= line.validatedAtMs + 200) {
        QVERIFY(h.PresentGap(frame++, 3, 5));
    }
    QVERIFY(frame > last + 1); // at least one gap frame was actually presented
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QVERIFY(!h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->AdvancingDisplayFrameSeq(), frame - 1);
    QCOMPARE(h.controller->AdvancingDisplayTimestampMs(),
             10000 + static_cast<std::int64_t>(frame - 1) * 20);
    // No synthetic work and no restamping of the old analysis frame.
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.controller->ProbeStatistics().submitted, probes);
    QCOMPARE(h.devices->probes.load(), deviceProbes);
    QCOMPARE(h.devices->readbacks.load(), deviceReadbacks);
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, line.provenance.frameSeq);
    QCOMPARE(h.controller->Model().Lines().first().provenance.timestampMs, line.provenance.timestampMs);
    QCOMPARE(h.controller->Model().Lines().first().validatedAtMs, line.validatedAtMs);
    QCOMPARE(h.controller->Model().Lines().first().validatedFrameSeq, line.validatedFrameSeq);
    QVERIFY(!h.devices->readOnUiThread.load());

    // Media expiry: displayed PTS passes the 350 ms validation bound while
    // the wall advance stays well inside it.
    while (10000 + static_cast<std::int64_t>(frame) * 20 <= line.validatedAtMs + 350) {
        QVERIFY(h.PresentGap(frame++, 3, 5));
    }
    QVERIFY(h.PresentGap(frame++, 3, 5)); // first receipt beyond the bound
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QVERIFY(h.controller->HasAdvancingMediaScene()); // expiry is not a discontinuity
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);

    // The next real device frames continue the same chain and recover text.
    for (int i = 0; i < 240 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++);
    QVERIFY(h.overlay.WordCount() > 0);
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QVERIFY(h.controller->Model().Lines().first().validatedFrameSeq > line.validatedFrameSeq);
}

void UltraClearTextControllerTests::gapReceiptWallExpiryHidesDespiteFreshMediaPts()
{
    ImmutableHarness h;
    h.Enable();
    const std::uint64_t last = h.EstablishDeviceGlyphs();
    QVERIFY(h.overlay.WordCount() > 0);
    const auto generation = h.controller->GeometryGeneration();
    const auto jobs = h.controller->SchedulerStatistics().submitted;

    // One forward frame, only +20 ms of media PTS, but the wall clock has
    // run far past the validation bound (slow motion / stalled presenter).
    QVERIFY(h.PresentGap(last + 1, 3, 5000));
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());
    QCOMPARE(h.controller->GeometryGeneration(), generation); // expiry, not revocation
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);

    std::uint64_t frame = last + 2;
    for (int i = 0; i < 240 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++);
    QVERIFY(h.overlay.WordCount() > 0);
    QCOMPARE(h.controller->GeometryGeneration(), generation);
}

void UltraClearTextControllerTests::gapReceiptDiscontinuitiesRevokeImmediately()
{
    { // seek: new source authority, even with a forward frame
        ImmutableHarness h;
        h.Enable();
        const std::uint64_t last = h.EstablishDeviceGlyphs();
        QVERIFY(h.overlay.WordCount() > 0);
        const auto generation = h.controller->GeometryGeneration();
        QVERIFY(!h.PresentGap(last + 1, 9));
        QVERIFY(h.controller->GeometryGeneration() > generation);
        QCOMPARE(h.overlay.WordCount(), 0);
        QVERIFY(h.Reading().isEmpty());
        QVERIFY(h.controller->Model().Lines().isEmpty());
        QVERIFY(h.controller->HasAdvancingMediaScene()); // display-only chain suspends live
        // Real frames under the new authority establish a new chain.
        std::uint64_t frame = last + 2;
        for (int i = 0; i < 240 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++, 9);
        QVERIFY(h.overlay.WordCount() > 0);
    }
    { // backward frame under the same authority
        ImmutableHarness h;
        h.Enable();
        const std::uint64_t last = h.EstablishDeviceGlyphs();
        QVERIFY(h.overlay.WordCount() > 0);
        const auto generation = h.controller->GeometryGeneration();
        QVERIFY(!h.PresentGap(last - 2));
        QVERIFY(h.controller->GeometryGeneration() > generation);
        QCOMPARE(h.overlay.WordCount(), 0);
        QVERIFY(h.Reading().isEmpty());
        QVERIFY(h.controller->Model().Lines().isEmpty());
    }
    { // geometry change carried by the display receipt
        ImmutableHarness h;
        h.Enable();
        const std::uint64_t last = h.EstablishDeviceGlyphs();
        QVERIFY(h.overlay.WordCount() > 0);
        const auto generation = h.controller->GeometryGeneration();
        auto receipt = h.GapReceipt(last + 1);
        receipt.geometry.sceneWidth = 640;
        receipt.geometry.sceneHeight = 360;
        h.now += 20;
        QVERIFY(!h.controller->OnAdvancingMediaPresentedWithoutAnalysis(receipt));
        QVERIFY(h.controller->GeometryGeneration() > generation);
        QCOMPARE(h.overlay.WordCount(), 0);
        QVERIFY(h.Reading().isEmpty());
    }
    { // forward sequence with backward PTS, and same sequence with new PTS
        ImmutableHarness h;
        h.Enable();
        const std::uint64_t last = h.EstablishDeviceGlyphs();
        QVERIFY(h.overlay.WordCount() > 0);
        const auto generation = h.controller->GeometryGeneration();
        auto receipt = h.GapReceipt(last + 1);
        receipt.timestampMs -= 40; // PTS moved backward
        h.now += 20;
        QVERIFY(!h.controller->OnAdvancingMediaPresentedWithoutAnalysis(receipt));
        QVERIFY(h.controller->GeometryGeneration() > generation);
        QCOMPARE(h.overlay.WordCount(), 0);

        auto sameFrame = receipt; // the receipt above is the display now
        sameFrame.timestampMs += 20; // same sequence, inconsistent PTS
        const auto afterFirst = h.controller->GeometryGeneration();
        h.now += 20;
        QVERIFY(!h.controller->OnAdvancingMediaPresentedWithoutAnalysis(sameFrame));
        QVERIFY(h.controller->GeometryGeneration() > afterFirst);
    }
}

void UltraClearTextControllerTests::gapReceiptLateOldRecognitionCannotReviveAfterExpiry()
{
    ImmutableHarness h;
    h.Enable();
    auto frame = h.EstablishDeviceGlyphs() + 1;
    QVERIFY(h.overlay.WordCount() > 0);
    const int calls = h.script->calls.load() + h.script->deviceCalls.load();
    {
        std::lock_guard lock(h.script->mutex);
        h.script->holdRecognition = true;
    }
    for (; frame < 400 && h.script->calls.load() + h.script->deviceCalls.load() == calls; ++frame)
        h.PlayDeviceFrame(frame);
    QVERIFY(h.script->calls.load() + h.script->deviceCalls.load() > calls); // refresh held in flight
    QVERIFY(h.overlay.WordCount() > 0);

    // No-analysis gap runs both freshness bounds out; text hides on expiry.
    for (int i = 0; i < 30; ++i) QVERIFY(h.PresentGap(frame++, 3, 20));
    QCOMPARE(h.overlay.WordCount(), 0);
    QVERIFY(h.Reading().isEmpty());

    QSignalSpy admitted(h.controller.get(), &UltraClearTextController::recognitionAdmitted);
    h.script->Release(); // the held, now-old recognition completes late
    QTRY_COMPARE(admitted.size(), 1);
    QTest::qWait(20);
    QCOMPARE(h.overlay.WordCount(), 0); // it cannot revive expired text
    QVERIFY(h.Reading().isEmpty());
    QVERIFY(h.controller->ScreenTextSnapshot().lines.isEmpty());

    for (int i = 0; i < 240 && h.overlay.WordCount() == 0; ++i) h.PlayDeviceFrame(frame++);
    QVERIFY(h.overlay.WordCount() > 0); // later current input recovers
    QVERIFY(h.Reading().contains(QStringLiteral("Reading helps")));
}

void UltraClearTextControllerTests::gapReceiptSameFrameReprojectsRepeatedlyWithoutWork()
{
    ImmutableHarness h;
    h.Enable();
    const std::uint64_t last = h.EstablishDeviceGlyphs();
    QVERIFY(h.overlay.WordCount() > 0);
    QTest::qWait(20); // drain in-flight probes so counters below are stable
    const auto words = h.overlay.Words();
    const auto rects = h.overlay.ProjectedWordRects();
    const auto generation = h.controller->GeometryGeneration();
    const auto jobs = h.controller->SchedulerStatistics().submitted;
    const auto probes = h.controller->ProbeStatistics().submitted;
    const int deviceProbes = h.devices->probes.load();
    const auto provenance = h.controller->Model().Lines().first().provenance;

    // The same displayed frame presented again under two different views:
    // pure reprojection, repeated, with no probe/OCR or restamping.
    auto receipt = h.GapReceipt(last);
    receipt.transform = ComputeViewTransform(960, 540, 960, 540, 2.0f, 0.3f, 0.3f,
                                             ViewportFitMode::kFill);
    QVERIFY(receipt.transform.valid);
    h.now += 20;
    QVERIFY(h.controller->OnAdvancingMediaPresentedWithoutAnalysis(receipt));
    QCOMPARE(h.overlay.WordCount(), words.size());
    QCOMPARE(h.overlay.Words().first().sceneRect, words.first().sceneRect);
    QCOMPARE(h.overlay.ProjectedWordRects().first(),
             SceneRectToView(receipt.transform, h.overlay.width(), h.overlay.height(),
                             words.first().sceneRect));
    QVERIFY(h.overlay.ProjectedWordRects().first() != rects.first());

    receipt.transform = ComputeViewTransform(960, 540, 960, 540, 3.0f, 0.6f, 0.4f,
                                             ViewportFitMode::kFill);
    h.now += 20;
    QVERIFY(h.controller->OnAdvancingMediaPresentedWithoutAnalysis(receipt));
    QCOMPARE(h.overlay.WordCount(), words.size());
    QCOMPARE(h.overlay.ProjectedWordRects().first(),
             SceneRectToView(receipt.transform, h.overlay.width(), h.overlay.height(),
                             words.first().sceneRect));
    QCOMPARE(h.controller->GeometryGeneration(), generation);
    QCOMPARE(h.controller->SchedulerStatistics().submitted, jobs);
    QCOMPARE(h.controller->ProbeStatistics().submitted, probes);
    QCOMPARE(h.devices->probes.load(), deviceProbes);
    QCOMPARE(h.controller->Model().Lines().first().provenance.frameSeq, provenance.frameSeq);
    QCOMPARE(h.controller->Model().Lines().first().provenance.timestampMs, provenance.timestampMs);
    QCOMPARE(h.controller->AdvancingDisplayFrameSeq(), last);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // Preserve an explicit file logger and replay its bytes through the
    // executable's stdout. The implicit QtTest logger left the first tracked
    // invocation with a failure code and no captured diagnostics.
    const QString root = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(
        QStringLiteral("OkuFlow/test-evidence/immutable-uct"));
    if (!QDir().mkpath(root)) {
        std::fprintf(stderr, "Cannot create immutable UCT evidence directory.\n");
        return 1;
    }
    const QString report = QDir(root).filePath(
        QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".txt"));
    std::fprintf(stdout, "Immutable UCT QtTest report: %s\n", qPrintable(report));
    std::fflush(stdout);
    QStringList arguments = QCoreApplication::arguments();
    arguments << QStringLiteral("-o") << report + QStringLiteral(",txt");
    UltraClearTextControllerTests tests;
    const int result = QTest::qExec(&tests, arguments);
    QFile saved(report);
    if (saved.open(QIODevice::ReadOnly)) {
        const auto bytes = saved.readAll();
        std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
    } else {
        std::fprintf(stderr, "Cannot read immutable UCT QtTest report.\n");
    }
    std::fprintf(stdout, "Immutable UCT QtTest exit code: %d\n", result);
    std::fflush(stdout);
    return result;
}

#include "ultra_clear_text_controller_tests.moc"

namespace {
// Paused view zoomed onto `target`: a quarter of the scene in each axis.
ViewTransform ZoomOnto(const QRectF& target, double span = 0.25)
{
    const double x = std::clamp(target.center().x() - span / 2, 0.0, 1.0 - span);
    const double y = std::clamp(target.center().y() - span / 2, 0.0, 1.0 - span);
    return {float(x), float(y), float(span), float(span), 0, 0, 1, 1, true};
}
bool OverlayShows(const UltraClearTextOverlay& overlay, const QString& text)
{
    return std::any_of(overlay.Words().begin(), overlay.Words().end(),
                       [&](const auto& word) { return word.text == text; });
}
} // namespace

void UltraClearTextControllerTests::pausedZoomRequestsOneBoundedVisibleDetailPair()
{
    // Reproduces the owner's paused zoom: readable small print the full-scene
    // detector never found stayed original forever, because view changes were
    // projection-only. A settled zoom now re-detects exactly the visible
    // region of the SAME selected image and its real neighbour, once.
    UctTiming timing;
    timing.immutableDetailSettleMs = 0;
    ImmutableHarness h(timing);
    h.page = MakePage({QStringLiteral("Reading helps"), QStringLiteral("every small word")}, {}, 960, 540, 48);
    const PageWord hidden = h.page.words.last();
    {
        std::lock_guard lock(h.script->mutex);
        h.script->words = h.page.words.mid(0, h.page.words.size() - 1);
        h.script->detailWords = {hidden};
        h.script->detailFocus = true;
    }
    const QImage neighbourImage = IndependentFramePixels(h.page.image, 11);
    const QByteArray selectedDigest = SourcePixelsDigest(h.page.image);
    const QByteArray neighbourDigest = SourcePixelsDigest(neighbourImage);
    h.Enable();
    auto scene = h.Scene();
    scene.corroboration = UctImmutableFrame{OwnedPixels(neighbourImage), 11, 20};
    QVERIFY(h.controller->SetImmutableScene(scene));
    QTRY_COMPARE(h.script->calls.load(), 2);
    QTRY_VERIFY(h.overlay.WordCount() > 0);
    QTest::qWait(30);
    QCOMPARE(h.script->calls.load(), 2); // the whole scene needs no detail pass
    QVERIFY(!OverlayShows(h.overlay, hidden.text));

    // Hold the neighbour half: the held image alone can never confirm what
    // its own region pass finds.
    {
        std::lock_guard lock(h.script->mutex);
        h.script->permittedCalls = 3;
    }
    const ViewTransform zoom = ZoomOnto(hidden.sceneRect);
    h.controller->UpdateImmutableView(zoom, 960, 540);
    QTRY_COMPARE(h.script->calls.load(), 4);
    QTest::qWait(30);
    QVERIFY(!OverlayShows(h.overlay, hidden.text));
    h.script->Release();
    QTRY_VERIFY(OverlayShows(h.overlay, hidden.text));
    QVERIFY(h.Reading().contains(hidden.text));
    QCOMPARE(h.controller->ImmutableDetailPasses(), std::uint64_t(1));
    QVERIFY(!h.controller->ActiveImmutableDetail());

    // Both halves name the visible source region in the retained full-scene
    // pixels, at the 6x bound, on the two original acquisitions only.
    std::vector<ImmutableScript::Call> calls;
    {
        std::lock_guard lock(h.script->mutex);
        calls.assign(h.script->recorded.begin(), h.script->recorded.end());
    }
    QCOMPARE(calls.size(), std::size_t(4));
    QVERIFY(!calls[0].focus && !calls[1].focus);
    const QRect expected = QRectF(zoom.sourceX * 960.0, zoom.sourceY * 540.0, zoom.sourceWidth * 960.0,
                                  zoom.sourceHeight * 540.0).toAlignedRect();
    for (int i : {2, 3}) {
        QVERIFY(calls[i].focus);
        QCOMPARE(calls[i].focus->regionPx, expected);
        QCOMPARE(calls[i].focus->maximumMagnification, kMaximumDetailMagnification);
    }
    QCOMPARE(calls[2].digest, selectedDigest);
    QCOMPARE(calls[3].digest, neighbourDigest);
    QCOMPARE(h.controller->ProbeStatistics().submitted, std::uint64_t(0));

    // The same settled view and a small pan inside the covered region add no work.
    h.controller->UpdateImmutableView(zoom, 960, 540);
    ViewTransform nudged = zoom;
    nudged.sourceX += 0.01f;
    nudged.sourceWidth -= 0.02f;
    nudged.sourceHeight -= 0.02f;
    h.controller->UpdateImmutableView(nudged, 960, 540);
    QTest::qWait(40);
    QCOMPARE(h.script->calls.load(), 4);
    // Returning to the whole view keeps the improved artifact without work.
    ViewTransform fit;
    fit.valid = true;
    h.controller->UpdateImmutableView(fit, 960, 540);
    QTest::qWait(40);
    QCOMPARE(h.script->calls.load(), 4);
    QVERIFY(OverlayShows(h.overlay, hidden.text));
    // Panning to an uncovered region requests exactly one more bounded pair.
    h.controller->UpdateImmutableView(ZoomOnto(h.page.words.first().sceneRect), 960, 540);
    QTRY_COMPARE(h.script->calls.load(), 6);
    QTest::qWait(40);
    QCOMPARE(h.script->calls.load(), 6);
    QCOMPARE(h.controller->ImmutableDetailPasses(), std::uint64_t(2));
    QVERIFY(OverlayShows(h.overlay, hidden.text)); // earlier proof survives a later region
}

void UltraClearTextControllerTests::pausedVisibleDetailNeedsSupportNeighbourAndMagnification_data()
{
    QTest::addColumn<bool>("supported");
    QTest::addColumn<bool>("neighbour");
    QTest::addColumn<double>("span");
    QTest::newRow("backend-without-region-detection") << false << true << 0.25;
    QTest::newRow("no-independent-neighbour") << true << false << 0.25;
    QTest::newRow("zoom-gains-too-little-detector-detail") << true << true << 0.95;
}

void UltraClearTextControllerTests::pausedVisibleDetailNeedsSupportNeighbourAndMagnification()
{
    QFETCH(bool, supported);
    QFETCH(bool, neighbour);
    QFETCH(double, span);
    UctTiming timing;
    timing.immutableDetailSettleMs = 0;
    ImmutableHarness h(timing);
    h.page = MakePage({QStringLiteral("Reading helps"), QStringLiteral("every small word")}, {}, 960, 540, 48);
    {
        std::lock_guard lock(h.script->mutex);
        h.script->words = h.page.words.mid(0, h.page.words.size() - 1);
        h.script->detailWords = {h.page.words.last()};
        h.script->detailFocus = supported;
    }
    h.Enable();
    auto scene = h.Scene();
    if (neighbour) scene.corroboration = UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image, 11)), 11, 20};
    QVERIFY(h.controller->SetImmutableScene(scene));
    const int baseline = neighbour ? 2 : 1;
    QTRY_COMPARE(h.script->calls.load(), baseline);
    h.controller->UpdateImmutableView(ZoomOnto(h.page.words.last().sceneRect, span), 960, 540);
    QTest::qWait(60);
    QCOMPARE(h.script->calls.load(), baseline);
    QCOMPARE(h.controller->ImmutableDetailPasses(), std::uint64_t(0));
    QVERIFY(!OverlayShows(h.overlay, h.page.words.last().text));
}

void UltraClearTextControllerTests::pausedDetailSettledWhileBusyUsesLatestView()
{
    UctTiming timing; timing.immutableDetailSettleMs=0;
    ImmutableHarness h(timing); h.script->detailFocus=true; h.Enable();
    auto scene=h.Scene(); scene.corroboration=UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image,11)),11,20};
    QVERIFY(h.controller->SetImmutableScene(scene)); QTRY_VERIFY(h.overlay.WordCount()>0);
    const auto old=h.controller->Model().Lines();
    {std::lock_guard lock(h.script->mutex);h.script->permittedCalls=3;}
    h.controller->UpdateImmutableView(ZoomOnto({.12,.15,.1,.1}),960,540);
    QTRY_COMPARE(h.script->calls.load(),4);
    const auto latest=ZoomOnto({.72,.7,.1,.1});
    h.controller->UpdateImmutableView(latest,960,540);
    QTest::qWait(30); QCOMPARE(h.script->calls.load(),4);
    h.script->Release(); QTRY_COMPARE(h.script->calls.load(),6);
    QTRY_VERIFY(!h.controller->ActiveImmutableDetail()); QTest::qWait(30);
    QCOMPARE(h.script->calls.load(),6);
    {std::lock_guard lock(h.script->mutex);const auto last=h.script->recorded.last();
     QVERIFY(last.focus); QCOMPARE(last.focus->regionPx.x(),int(std::floor(latest.sourceX*960.0)));}
    for(const auto& line:old) {
        const auto& current=h.controller->Model().Lines();
        const auto found=std::find_if(current.begin(),current.end(),[&](const auto& x){return x.id==line.id;});
        QVERIFY(found!=current.end()); QCOMPARE(found->validatedAtMs,line.validatedAtMs);
        QCOMPARE(found->validatedFrameSeq,line.validatedFrameSeq);
    }
}
void UltraClearTextControllerTests::pausedDetailFailureIsFiniteAndOtherRegionCanProceed()
{
    UctTiming timing;timing.immutableDetailSettleMs=0;
    ImmutableHarness h(timing);h.script->detailFocus=true;h.Enable();
    auto scene=h.Scene();scene.corroboration=UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image,11)),11,20};
    QVERIFY(h.controller->SetImmutableScene(scene));QTRY_VERIFY(h.overlay.WordCount()>0);
    h.script->transientFailures=1;h.script->transientKind=0;
    const auto failed=ZoomOnto({.12,.15,.1,.1});
    h.controller->UpdateImmutableView(failed,960,540);QTRY_COMPARE(h.script->calls.load(),3);
    QTRY_VERIFY(!h.controller->ActiveImmutableDetail());
    for(int i=0;i<12;++i)h.controller->UpdateImmutableView(failed,960,540);
    QTest::qWait(160);QCOMPARE(h.script->calls.load(),3);QVERIFY(h.overlay.WordCount()>0);
    h.controller->UpdateImmutableView(ZoomOnto({.72,.7,.1,.1}),960,540);
    QTRY_COMPARE(h.script->calls.load(),5);QTest::qWait(30);QCOMPARE(h.script->calls.load(),5);
}
void UltraClearTextControllerTests::pausedDetailSettledDuringPresentationFailureRunsAfterRecovery()
{
    // The settle deadline can expire while presentation is latched failed.
    // Recovery is an event, not a view change: it must ask once more for the
    // already-settled view instead of leaving its small print unexamined
    // until the owner happens to pan or zoom again.
    UctTiming timing;timing.immutableDetailSettleMs=25;
    ImmutableHarness h(timing);h.script->detailFocus=true;h.Enable();
    auto scene=h.Scene();scene.corroboration=UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image,11)),11,20};
    QVERIFY(h.controller->SetImmutableScene(scene));QTRY_VERIFY(h.overlay.WordCount()>0);
    QTRY_COMPARE(h.script->calls.load(),2);
    h.controller->OnMediaPresentationFailed();
    h.controller->UpdateImmutableView(ZoomOnto({.12,.15,.1,.1}),960,540);
    QTest::qWait(90); // the settle timer fired behind the presentation latch
    QCOMPARE(h.script->calls.load(),2);
    QCOMPARE(h.controller->ImmutableDetailPasses(),std::uint64_t(0));
    h.controller->OnMediaPresentationSucceeded();
    QTRY_COMPARE(h.script->calls.load(),4);
    QTRY_COMPARE(h.controller->ImmutableDetailPasses(),std::uint64_t(1));
    QTRY_VERIFY(!h.controller->ActiveImmutableDetail());
    QVERIFY(h.overlay.WordCount()>0);
    QTest::qWait(60);QCOMPARE(h.script->calls.load(),4); // recovered once, never a polling loop
}
void UltraClearTextControllerTests::pausedDetailHeldCopiesNeverCorroborate()
{
    UctTiming timing;timing.immutableDetailSettleMs=0;
    ImmutableHarness h(timing);h.script->detailFocus=true;h.Enable();
    auto scene=h.Scene();scene.corroboration=UctImmutableFrame{OwnedPixels(h.page.image.copy()),11,20};
    QVERIFY(h.controller->SetImmutableScene(scene));QTRY_COMPARE(h.script->calls.load(),2);
    h.controller->UpdateImmutableView(ZoomOnto(h.page.words.first().sceneRect),960,540);
    QTRY_COMPARE(h.script->calls.load(),4);QTRY_VERIFY(!h.controller->ActiveImmutableDetail());
    for(int i=0;i<12;++i)h.controller->UpdateImmutableView(ZoomOnto(h.page.words.first().sceneRect),960,540);
    QTest::qWait(60);QCOMPARE(h.script->calls.load(),4);QCOMPARE(h.overlay.WordCount(),0);
    QVERIFY(h.controller->Model().SceneWords(h.now,10).isEmpty());
}
void UltraClearTextControllerTests::pausedDetailAuthorityRevokesHeldWork_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("source")<<0;QTest::newRow("seek")<<1;QTest::newRow("processing")<<2;
}
void UltraClearTextControllerTests::pausedDetailAuthorityRevokesHeldWork()
{
    QFETCH(int,change);UctTiming timing;timing.immutableDetailSettleMs=0;
    ImmutableHarness h(timing);h.script->detailFocus=true;h.Enable();
    auto scene=h.Scene();scene.corroboration=UctImmutableFrame{OwnedPixels(IndependentFramePixels(h.page.image,11)),11,20};
    QVERIFY(h.controller->SetImmutableScene(scene));QTRY_VERIFY(h.overlay.WordCount()>0);
    {std::lock_guard lock(h.script->mutex);h.script->holdCall=3;}
    h.controller->UpdateImmutableView(ZoomOnto(h.page.words.first().sceneRect),960,540);
    QTRY_COMPARE(h.script->calls.load(),3);
    const auto old=h.controller->GeometryGeneration();
    auto next=h.Scene(100);next.selected.pixels=OwnedPixels(IndependentFramePixels(h.page.image,100));
    if(change==0)++next.sourceAuthority;
    if(change==2)++next.geometry.settings.warpRevision;
    QVERIFY(h.controller->SetImmutableScene(next));
    QVERIFY(h.controller->GeometryGeneration()>old);QVERIFY(!h.controller->ActiveImmutableDetail());
    h.script->Release();QTRY_VERIFY(h.controller->SchedulerStatistics().revoked>0);
    QTRY_VERIFY(h.script->calls.load()>=4);QTest::qWait(40);
    QCOMPARE(h.overlay.WordCount(),0); // new selection has no independent half
    QCOMPARE(h.controller->ImmutableDetailPasses(),std::uint64_t(1));
}
