#include "okuflow/app/ultra_clear_text_controller.hpp"

#if defined(_WIN32)

#include "okuflow/ui/uct_screen_text_accessible.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include "okuflow/common/async_cleanup.hpp"

#include <QCheckBox>
#include <QElapsedTimer>
#include <QHash>
#include <QMetaObject>
#include <QSignalBlocker>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <utility>

namespace okuflow {
namespace {

std::int64_t SteadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool BackendUsable(const uct::BackendStatus& status)
{
    return status.state != uct::BackendState::kUnavailable &&
           status.state != uct::BackendState::kFailed;
}

void TraceRecognition(uct::timing::Event event, const uct::RecognitionScheduler* scheduler,
                      const uct::RecognitionResult& result, uct::timing::Values values = {},
                      uct::timing::Channel channel = uct::timing::Channel::Recognition)
{
    if (!uct::timing::Enabled() || !scheduler) return;
    if (result.deviceInput) values.flags |= uct::timing::DeviceInput;
    if (result.output.cancelled) values.flags |= uct::timing::Cancelled;
    if (result.output.inputFault) values.flags |= uct::timing::InputFault;
    if (result.output.backendPreparing) values.flags |= uct::timing::Preparing;
    if (result.output.compositeInput) values.flags |= uct::timing::Composite;
    if (result.corroboratedJobId) {
        values.flags |= uct::timing::Corroboration;
        if (!values.relatedJobId) values.relatedJobId = result.corroboratedJobId;
    }
    uct::TraceTiming(event, scheduler->TimingProducerId(), channel, result.stamp,
                     result.jobId, result.authority, values);
}

double Area(const QRectF& rect)
{
    return rect.isEmpty() ? 0.0 : rect.width() * rect.height();
}

bool SameColors(const uct::LineColors& a, const uct::LineColors& b)
{
    return a.foreground == b.foreground && a.background == b.background;
}

// A media device image is usable only when it is the complete pre-zoom scene
// of exactly this frame, mirroring the scheduler's SubmitDevice contract.
// This inspects identity metadata only; it never reads pixels back.
bool DeviceMatchesFrame(const UctImmutableFrame& frame, const UctGeometryKey& geometry)
{
    const auto& device = frame.device;
    const bool paired = device && device->kind == uct::AnalysisKind::TemporallySmoothed;
    return device && device->Data() && device->ValidationData() &&
           device->ValidationPitch() >= static_cast<std::size_t>(device->width) * 4 &&
           (!paired || (device->blendFrameCount >= 1 &&
                        device->seedFrameSequence <= device->frameSequence &&
                        device->historyEpoch != 0 && device->temporalAlpha >= 0.0f &&
                        device->temporalAlpha <= 1.0f)) &&
           device->width == static_cast<int>(geometry.sceneWidth) &&
           device->height == static_cast<int>(geometry.sceneHeight) &&
           device->width > 0 && device->height > 0 && device->width <= 8192 && device->height <= 8192 &&
           static_cast<std::size_t>(device->width) * static_cast<std::size_t>(device->height) * 4 *
                   (paired ? 2 : 1) <=
               128ull * 1024 * 1024 &&
           device->pitch >= static_cast<std::size_t>(device->width) * 4 &&
           device->sourceEpoch == geometry.settings.sourceEpoch &&
           device->frameSequence == frame.frameSeq;
}

bool PixelsMatchFrame(const UctImmutableFrame& frame, const UctGeometryKey& geometry)
{
    return frame.pixels && frame.pixels->IsValid() &&
           frame.pixels->width == static_cast<int>(geometry.sceneWidth) &&
           frame.pixels->height == static_cast<int>(geometry.sceneHeight);
}

// Either owned CPU pixels or a matching device image make a frame usable.
bool FrameInputValid(const UctImmutableFrame& frame, const UctGeometryKey& geometry)
{
    return PixelsMatchFrame(frame, geometry) || DeviceMatchesFrame(frame, geometry);
}

// The preferred (device) input of a frame, or null when CPU pixels apply.
uct::SharedDeviceImage EffectiveDevice(const UctImmutableFrame& frame, const UctGeometryKey& geometry)
{
    return DeviceMatchesFrame(frame, geometry) ? frame.device : uct::SharedDeviceImage{};
}

// A neighbour is ancillary evidence for the retained selected acquisition,
// never part of its identity. Reject unusable/repeated descriptors before
// claiming its one bounded corroboration attempt; pixel independence is
// checked separately by the recognition worker's complete-source digest.
bool CorroborationInputValid(const UctImmutableScene& scene, const UctImmutableFrame& neighbour)
{
    if (!FrameInputValid(neighbour, scene.geometry) ||
        neighbour.frameSeq == scene.selected.frameSeq) return false;
    const auto neighbourDevice = EffectiveDevice(neighbour, scene.geometry);
    const auto selectedDevice = EffectiveDevice(scene.selected, scene.geometry);
    return !((neighbourDevice && selectedDevice && neighbourDevice == selectedDevice) ||
             (!neighbourDevice && !selectedDevice && neighbour.pixels == scene.selected.pixels));
}

constexpr int kMaximumConsecutiveFailures = 3;
constexpr std::int64_t kPresentationWatchdogMs = 600;
// Probes are decimated to at most about this width, bounding worker cost
// independently of the viewport resolution.
constexpr int kMaximumProbeWidth = 960;
// Readback buffers alive at once: probe in flight + pending, OCR in flight +
// pending, and one spare for the presenter.
constexpr int kPixelPoolCapacity = 5;

} // namespace

UltraClearTextController::UltraClearTextController(UltraClearTextOverlay* overlay,
                                                   UltraClearTextPanel* panel,
                                                   RecognizerFactory factory,
                                                   QObject* parent,
                                                   Clock clock,
                                                   UctTiming timing)
    : QObject(parent),
      overlay_(overlay),
      panel_(panel),
      factory_(std::move(factory)),
      clock_(clock ? std::move(clock) : Clock(&SteadyNowMs)),
      timing_(timing),
      pool_(uct::PixelPool::Create(kPixelPoolCapacity))
{
    // Presentation watchdog: with no presentations (camera stopped, source
    // switching, placeholder shown) cached words must not linger.
    watchdog_ = new QTimer(this);
    watchdog_->setInterval(200);
    connect(watchdog_, &QTimer::timeout, this, [this] {
        if (!enabled_) return;
        if (!HasImmutableScene() && presentationAvailable_ && !PresentationAvailable(clock_())) {
            presentationAvailable_ = false;
            RefreshOverlay(clock_());
            UpdateReadingText();
            UpdateStatus();
        } else if (advancingDisplay_ && shownWords_ > 0 && !AdvancingValidationFresh(clock_())) {
            // No receipt is needed for wall expiry: a stalled or slow-motion
            // display must not keep text past its real validation bound.
            RefreshOverlay(clock_());
            UpdateReadingText();
            UpdateStatus();
        }
    });
    // A paused view's visible region is re-detected only after it settles.
    immutableDetailTimer_ = new QTimer(this);
    immutableDetailTimer_->setSingleShot(true);
    connect(immutableDetailTimer_, &QTimer::timeout, this, &UltraClearTextController::SubmitImmutableDetailIfUseful);
    // Screen readers reach the drawn text through the viewport itself. The
    // object is a QObject child of the render target (the host interface
    // finds it there) and does no work while accessibility is inactive.
    if (overlay_ && overlay_->RenderTarget()) {
        screenText_ = new UctScreenTextAccessibility(
            overlay_, overlay_->RenderTarget(), [this] { return ScreenTextSnapshot(); }, clock_);
    }
    if (panel_) {
        connect(panel_, &UltraClearTextPanel::enabledChanged, this, &UltraClearTextController::SetEnabled);
        connect(panel_, &UltraClearTextPanel::showOriginalChanged, this,
                &UltraClearTextController::SetShowOriginal);
    }
    UpdateStatus();
}

UltraClearTextController::~UltraClearTextController()
{
    // Revoke every accessibility element on the UI thread before the
    // snapshot provider (this) disappears.
    delete screenText_.data();
    // Disables worker callbacks before this object's members disappear.
    probeWorker_.reset();
    deviceProbeReader_.reset();
    scheduler_.reset();
    uct::timing::RecordEvent(uct::timing::Event::WindowClosed, {});
    uct::timing::FlushAtNormalTeardown(); // opt-in only; never waits for detached workers
}

void UltraClearTextController::EnsureScheduler()
{
    QPointer<UltraClearTextController> self(this);
    if (!probeWorker_) {
        probeWorker_ = std::make_unique<uct::ProbeWorker>([self](uct::ProbeOutcome outcome) {
            // Worker thread: hand the owned outcome to the UI thread only.
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, outcome = std::move(outcome)]() mutable {
                    if (self) {
                        self->HandleProbeOutcome(std::move(outcome));
                    }
                },
                Qt::QueuedConnection);
        });
    }
    if (scheduler_) {
        return;
    }
    if(!deviceProbeReader_) {
        deviceProbeReader_=std::make_unique<uct::RecognitionScheduler>(nullptr,
            [self](uct::RecognitionResult result){
                if(!self)return;
                QMetaObject::invokeMethod(self.data(),[self,result=std::move(result)]() mutable {
                    if(self) TraceRecognition(uct::timing::Event::UiReceipt, self->deviceProbeReader_.get(),
                        result, {}, uct::timing::Channel::DeviceValidation);
                    // Advancing media reuses this independent validation chain;
                    // a selected immutable still (and hidden live camera work
                    // under any media scene) does not. Media probes must match
                    // the retained device candidate exactly: a seek, geometry
                    // or source change cancels the reader and clears it first.
                    const bool advancing=self&&self->advancingMediaScene_.has_value();
                    const bool exactCandidate=self&&self->deviceCandidate_&&
                        self->deviceCandidate_->frameSequence==result.stamp.frameSeq&&
                        self->deviceCandidate_->sourceEpoch==result.stamp.sourceEpoch&&
                        static_cast<std::uint32_t>(self->deviceCandidate_->width)==result.stamp.viewportWidth&&
                        static_cast<std::uint32_t>(self->deviceCandidate_->height)==result.stamp.viewportHeight;
                    if(!self||!self->enabled_||self->immutableScene_.has_value()||self->settling_||self->compositePaused_||self->mediaOverlaySuppressed_||
                        (advancing&&!exactCandidate)||
                        !self->deviceProbeReader_->IsCurrent(result)||!result.stamp.SameScene(self->CurrentScene())||
                        !result.output.ok||!result.reference.IsValid()){
                        if(self && self->deviceCandidate_ && self->deviceCandidate_->frameSequence==result.stamp.frameSeq) {
                            self->deviceCandidate_.reset();
                            if (!result.output.cancelled) ++self->consecutiveProbeFailures_;
                            self->RefreshOverlay(self->clock_());
                            self->UpdateStatus();
                        }
                        return;
                    }
                    self->consecutiveProbeFailures_ = 0;
                    uct::ProbeRequest request;request.stamp=result.stamp;
                    request.preparedProbe=std::make_shared<uct::ProbeFrame>(std::move(result.reference));
                    request.previous=self->latestProbe_;request.lines=self->model_.LineReferences(self->CurrentScene());
                    request.tuning=uct::ModelTuning{}.match;request.budgetMs=self->timing_.probeCheckBudgetMs;
                    self->probeWorker_->Submit(std::move(request));
                },Qt::QueuedConnection);
            },uct::RecognitionScheduler::StatusChanged{},1,uct::ModelTuning{}.minimumWordHeightPx,true);
    }
    backend_ = {};
    const std::uint64_t generation = ++schedulerGeneration_;
    scheduler_ = std::make_unique<uct::RecognitionScheduler>(
        factory_ ? factory_() : std::unique_ptr<uct::Recognizer>{},
        [self, generation](uct::RecognitionResult result) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, generation, result = std::move(result)]() mutable {
                    if (self && self->schedulerGeneration_ == generation) {
                        self->HandleResult(std::move(result));
                    }
                },
                Qt::QueuedConnection);
        },
        [self, generation](uct::BackendStatus status) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, generation, status = std::move(status)]() mutable {
                    if (self && self->schedulerGeneration_ == generation) {
                        self->HandleBackendStatus(std::move(status));
                    }
                },
                Qt::QueuedConnection);
        },
        timing_.probeDecimation, uct::ModelTuning{}.minimumWordHeightPx, false,
        qEnvironmentVariable("OKUFLOW_UCT_SHADOW_PLANNER") == "1");
}

void UltraClearTextController::SetEnabled(bool enabled)
{
    enabledByUser_ = enabled;
    if (panel_) panel_->SetEnabledByUser(enabled);
    ApplyEnabled(enabledByUser_ && textClarityEnabled_);
}

void UltraClearTextController::SetTextClarityEnabled(bool enabled)
{
    textClarityEnabled_ = enabled;
    if (panel_) panel_->SetTextClarityEnabled(enabled);
    ApplyEnabled(enabledByUser_ && textClarityEnabled_);
}

void UltraClearTextController::ApplyEnabled(bool enabled)
{
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (enabled_) {
        // Initialization failure is terminal for that worker. An explicit
        // off/on retries with a fresh recognizer (for example after adding an
        // OCR language), fencing already-queued callbacks from its predecessor.
        if (scheduler_ && !BackendUsable(scheduler_->Status())) {
            ++schedulerGeneration_;
            scheduler_.reset();
        }
        EnsureScheduler();
        watchdog_->start(); // live heartbeat, plus advancing wall expiry above
        // A user re-enable retries a backend that only failed at run time.
        if (backend_.state == uct::BackendState::kFailed) {
            backend_ = scheduler_->Status();
        }
        haveGeometry_ = false; // the next presentation starts a fresh generation
        settling_ = false;
        consecutiveFailures_ = 0;
        consecutiveInputFailures_ = 0;
        consecutiveProbeFailures_ = 0;
        analysisPoolFailed_ = false;
        if (immutableScene_) {
            ++geometryGeneration_;
            geometry_ = immutableScene_->geometry;
            haveGeometry_ = true;
            currentFrameSeq_ = immutableScene_->selected.frameSeq;
            analysisPoolFailed_ = !FrameInputValid(immutableScene_->selected, geometry_);
            SubmitImmutableIfNeeded();
            RefreshOverlay(clock_());
        } else if (advancingMediaScene_ || advancingDisplay_) {
            ++geometryGeneration_;
            geometry_ = advancingDisplay_ ? advancingDisplay_->geometry
                                          : advancingMediaScene_->geometry;
            haveGeometry_ = true;
            currentFrameSeq_ = advancingDisplay_ ? advancingDisplay_->frameSeq
                                                 : advancingMediaScene_->current.frameSeq;
            // Awaiting a first/next analysis lease is not an analysis failure.
            analysisPoolFailed_ = advancingMediaScene_ ? !advancingReceiptValid_ : false;
            lastProbeRequestMs_ = std::numeric_limits<std::int64_t>::min() / 2;
            SubmitAdvancingMediaProbe();
            RefreshOverlay(clock_());
        }
    } else {
        presentationAvailable_ = false;
        watchdog_->stop();
        Invalidate({});
        // End the backend session without joining a native call on the UI.
        // Destruction cancels GPU preparation cooperatively; a new enable gets
        // a fresh backend chain, including candidates that previously failed.
        ++schedulerGeneration_;
        auto retired=std::shared_ptr<uct::RecognitionScheduler>(std::move(scheduler_));
        AsyncCleanup::Start([retired]{return true;});
        backend_={};
    }
    if (screenText_) screenText_->SetEnabled(enabled_);
    UpdateStatus();
}

void UltraClearTextController::SetShowOriginal(bool showOriginal)
{
    if (panel_ && panel_->showOriginalCheckbox()->isChecked() != showOriginal) {
        const QSignalBlocker blocker(panel_->showOriginalCheckbox());
        panel_->showOriginalCheckbox()->setChecked(showOriginal);
    }
    showOriginal_ = showOriginal;
    RefreshOverlay(clock_());
    UpdateStatus();
}

void UltraClearTextController::SetMediaOverlaySuppressed(bool suppressed, bool extraStable)
{
    if (mediaOverlaySuppressed_ == suppressed) {
        // Only the named reason changed: no second revocation, status only.
        if (mediaSuppressionExtraStable_ != extraStable) {
            mediaSuppressionExtraStable_ = extraStable;
            UpdateStatus();
        }
        return;
    }
    mediaSuppressionExtraStable_ = extraStable;
    if (suppressed) {
        // Drop the previous source input as well as semantic authority. A
        // delayed recognition callback cannot revive it behind this gate.
        if (HasImmutableScene()) ClearImmutableScene();
        ++geometryGeneration_;
        Invalidate(QStringLiteral("scene"));
    }
    mediaOverlaySuppressed_ = suppressed;
    if (!suppressed && enabled_) {
        EnsureScheduler();
        SubmitImmutableIfNeeded();
        SubmitAdvancingMediaProbe();
    }
    RefreshOverlay(clock_());
    UpdateReadingText();
    UpdateStatus();
}

bool UltraClearTextController::SetImmutableScene(UctImmutableScene scene)
{
    const auto sameFrame = [](const UctImmutableFrame& a, const UctImmutableFrame& b) {
        return a.pixels == b.pixels && a.device == b.device && a.frameSeq == b.frameSeq &&
               a.timestampMs == b.timestampMs;
    };
    if (immutableScene_ && immutableScene_->geometry == scene.geometry &&
        immutableScene_->sourceAuthority == scene.sourceAuthority &&
        sameFrame(immutableScene_->selected, scene.selected)) {
        // Decoder/preroll may supply the real neighbour after this selected
        // frame has already been read. Attach it without cancelling that job,
        // clearing its reading text, or renewing its deadline/authority.
        // Omitted/changed optional descriptors on later view receipts cannot
        // replace the first usable neighbour or reread the selected image.
        if (!immutableScene_->corroboration && scene.corroboration &&
            CorroborationInputValid(*immutableScene_, *scene.corroboration)) {
            immutableScene_->corroboration = std::move(scene.corroboration);
            immutableCorroborationAttempted_ = false;
            SubmitImmutableIfNeeded();
        }
        UpdateImmutableView(scene.transform, scene.viewportWidth, scene.viewportHeight, scene.strokeWeight);
        OnMediaPresentationSucceeded(); // this API carries a successful selected-frame receipt
        return FrameInputValid(scene.selected, scene.geometry);
    }

    ++geometryGeneration_;
    Invalidate(QStringLiteral("scene"));
    advancingMediaScene_.reset();
    advancingDisplay_.reset();
    advancingDeviceIdentity_.reset();
    advancingReceiptValid_ = false;
    immutableScene_ = std::move(scene);
    if (immutableScene_->corroboration &&
        !CorroborationInputValid(*immutableScene_, *immutableScene_->corroboration))
        immutableScene_->corroboration.reset();
    geometry_ = immutableScene_->geometry;
    haveGeometry_ = true;
    cameraActive_ = false;
    compositePaused_ = false;
    settling_ = false;
    presentationAvailable_ = false;
    presentationFailed_ = false;
    currentFrameSeq_ = immutableScene_->selected.frameSeq;
    consecutiveFailures_ = 0;
    consecutiveInputFailures_ = 0;
    consecutiveProbeFailures_ = 0;
    const bool valid = FrameInputValid(immutableScene_->selected, geometry_);
    analysisPoolFailed_ = !valid;
    watchdog_->stop();
    UpdateImmutableView(immutableScene_->transform, immutableScene_->viewportWidth,
                        immutableScene_->viewportHeight, immutableScene_->strokeWeight);
    if (enabled_) {
        EnsureScheduler();
        SubmitImmutableIfNeeded();
    }
    UpdateStatus();
    return valid;
}

bool UltraClearTextController::OnAdvancingMediaPresented(UctAdvancingMediaScene scene)
{
    ++presentCount_;
    // The previous scene's device lease may already be released (see
    // SubmitAdvancingMediaProbe); compare control-block identity instead of
    // the released strong reference.
    const std::weak_ptr<const uct::DeviceImage> newDeviceIdentity = scene.current.device;
    const bool sameDeviceIdentity =
        !newDeviceIdentity.owner_before(advancingDeviceIdentity_) &&
        !advancingDeviceIdentity_.owner_before(newDeviceIdentity);
    // Continuity is judged against the last DISPLAYED frame as well as the
    // last analysis input: an analysis receipt is also a display receipt,
    // and without-analysis receipts may have advanced the display since the
    // last analysis frame. A real frame at/behind that display clock in the
    // same chain (including a lease obtained for the currently displayed
    // frame after a gap) continues it; anything backward is a seek.
    const bool displayDiscontinuity = advancingDisplay_ &&
        (advancingDisplay_->geometry != scene.geometry ||
         advancingDisplay_->sourceAuthority != scene.sourceAuthority ||
         scene.current.frameSeq < advancingDisplay_->frameSeq ||
         scene.current.timestampMs < advancingDisplay_->timestampMs ||
         (scene.current.frameSeq == advancingDisplay_->frameSeq &&
          scene.current.timestampMs != advancingDisplay_->timestampMs));
    const bool discontinuity = immutableScene_.has_value() ||
        (!advancingMediaScene_ && !advancingDisplay_) || displayDiscontinuity ||
        (advancingMediaScene_ &&
         (advancingMediaScene_->geometry != scene.geometry ||
          advancingMediaScene_->sourceAuthority != scene.sourceAuthority ||
          scene.current.frameSeq < advancingMediaScene_->current.frameSeq ||
          scene.current.timestampMs < advancingMediaScene_->current.timestampMs ||
          (scene.current.frameSeq == advancingMediaScene_->current.frameSeq &&
           (scene.current.timestampMs != advancingMediaScene_->current.timestampMs ||
            scene.current.pixels != advancingMediaScene_->current.pixels ||
            !sameDeviceIdentity))));
    if (discontinuity) {
        ++geometryGeneration_;
        Invalidate(QStringLiteral("scene"));
        immutableScene_.reset();
        consecutiveFailures_ = 0;
        consecutiveInputFailures_ = 0;
        consecutiveProbeFailures_ = 0;
        lastProbeRequestMs_ = std::numeric_limits<std::int64_t>::min() / 2;
    }
    advancingMediaScene_ = std::move(scene); // one newest lease; workers retain their own inputs
    advancingDeviceIdentity_ = advancingMediaScene_->current.device;
    advancingDisplay_ = UctAdvancingMediaPresentation{
        advancingMediaScene_->geometry, advancingMediaScene_->sourceAuthority,
        advancingMediaScene_->current.frameSeq, advancingMediaScene_->current.timestampMs,
        advancingMediaScene_->transform, advancingMediaScene_->viewportWidth,
        advancingMediaScene_->viewportHeight, advancingMediaScene_->strokeWeight};
    geometry_ = advancingMediaScene_->geometry;
    haveGeometry_ = true;
    cameraActive_ = false;
    compositePaused_ = false;
    settling_ = false;
    presentationAvailable_ = false;
    presentationFailed_ = false;
    currentFrameSeq_ = advancingMediaScene_->current.frameSeq;
    const bool valid = FrameInputValid(advancingMediaScene_->current, geometry_);
    advancingReceiptValid_ = valid;
    analysisPoolFailed_ = !valid;
    // The watchdog is not a live heartbeat here; it only enforces advancing
    // wall-clock validation expiry between receipts.
    if (enabled_) watchdog_->start();
    UpdateImmutableView(advancingMediaScene_->transform, advancingMediaScene_->viewportWidth,
                        advancingMediaScene_->viewportHeight, advancingMediaScene_->strokeWeight);
    if (enabled_) {
        EnsureScheduler();
        SubmitAdvancingMediaProbe();
    }
    return valid;
}

bool UltraClearTextController::OnAdvancingMediaPresentedWithoutAnalysis(
    UctAdvancingMediaPresentation receipt)
{
    ++presentCount_;
    if (!receipt.transform.valid || receipt.viewportWidth == 0 || receipt.viewportHeight == 0) {
        // No truthful display state to advance to: hide like a fault and
        // keep the previous clocks untouched.
        if (HasImmutableScene()) OnMediaPresentationFailed();
        return false;
    }
    const bool discontinuity = immutableScene_.has_value() || !advancingDisplay_ ||
        advancingDisplay_->geometry != receipt.geometry ||
        advancingDisplay_->sourceAuthority != receipt.sourceAuthority ||
        receipt.frameSeq < advancingDisplay_->frameSeq ||
        receipt.timestampMs < advancingDisplay_->timestampMs ||
        (receipt.frameSeq == advancingDisplay_->frameSeq &&
         receipt.timestampMs != advancingDisplay_->timestampMs);
    if (discontinuity) {
        ++geometryGeneration_;
        Invalidate(QStringLiteral("scene"));
        immutableScene_.reset();
        advancingMediaScene_.reset(); // old analysis belongs to the old chain
        advancingDeviceIdentity_.reset();
        advancingReceiptValid_ = false;
        consecutiveFailures_ = 0;
        consecutiveInputFailures_ = 0;
        consecutiveProbeFailures_ = 0;
        lastProbeRequestMs_ = std::numeric_limits<std::int64_t>::min() / 2;
        // Awaiting a real analysis frame is not itself an analysis failure.
        analysisPoolFailed_ = false;
    }
    // Continuous receipts deliberately touch nothing else: the last analysis
    // input, its stamps, probe/OCR pacing and analysis health all stay as
    // the last real frame left them, so a pool/throttle gap cannot flicker
    // text off and no older image is restamped as this displayed frame.
    advancingDisplay_ = receipt;
    geometry_ = receipt.geometry;
    haveGeometry_ = true;
    cameraActive_ = false;
    compositePaused_ = false;
    settling_ = false;
    presentationAvailable_ = false;
    presentationFailed_ = false;
    currentFrameSeq_ = receipt.frameSeq;
    if (enabled_) watchdog_->start();
    // Projection + freshness only. RefreshOverlay (inside) evaluates the
    // displayed media PTS and wall clock against the last real validation;
    // no probe, OCR submission or validation renewal happens here.
    UpdateImmutableView(receipt.transform, receipt.viewportWidth, receipt.viewportHeight,
                        receipt.strokeWeight);
    if (enabled_) EnsureScheduler();
    return !discontinuity;
}

void UltraClearTextController::SubmitAdvancingMediaProbe()
{
    if (mediaOverlaySuppressed_ || !enabled_ || !advancingMediaScene_ || !probeWorker_ ||
        !BackendUsable(backend_) || probeWorker_->HasPending()) return;
    const auto now = clock_();
    const auto& current = advancingMediaScene_->current;
    // Media has no OnPresented clock to expire a stuck device candidate, so
    // the receipt path retires it with the same deadline as live input.
    if (deviceCandidate_ && now - deviceCandidateSinceMs_ > timing_.readbackTimeoutMs) {
        deviceCandidate_.reset();
        if (deviceProbeReader_) deviceProbeReader_->Cancel();
        ++consecutiveProbeFailures_;
    }
    if ((advancingMediaHaveProbe_ && advancingMediaProbeFrame_ == current.frameSeq) ||
        now - lastProbeRequestMs_ < timing_.probeIntervalMs) return;
    if (auto device = EffectiveDevice(current, advancingMediaScene_->geometry)) {
        // Device frames validate through the independent probe reader, never
        // a UI-thread readback. Probe-health alone must not gate submission:
        // a successful probe is the only way that health recovers.
        if (analysisPoolFailed_ || consecutiveInputFailures_ >= kMaximumConsecutiveFailures ||
            !deviceProbeReader_ || deviceCandidate_ || deviceProbeReader_->HasWork()) return;
        lastProbeRequestMs_ = now;
        advancingMediaProbeFrame_ = current.frameSeq;
        advancingMediaHaveProbe_ = true;
        deviceCandidate_ = device;
        deviceCandidateSinceMs_ = now;
        if (!deviceProbeReader_->SubmitDevice(ImmutableStamp(current), std::move(device))) {
            deviceCandidate_.reset();
            if (PixelsMatchFrame(current, advancingMediaScene_->geometry))
                SubmitProbe(ImmutableStamp(current), current.pixels);
        } else {
            // The probe reader and candidate own the analysis lease now.
            // Release the scene's strong reference so its pool slot frees
            // when validation/inference finish, even if this scene stays
            // displayed; identity and receipt validity above keep the
            // scene's provenance exact without holding pixels.
            advancingMediaScene_->current.device.reset();
        }
        return;
    }
    if (!AnalysisHealthy()) return;
    // No usable input for this frame (e.g. its device lease was already
    // consumed and released): leave the probe unmarked so a later receipt
    // carrying a fresh analysis image can still submit one.
    if (!PixelsMatchFrame(current, advancingMediaScene_->geometry)) return;
    lastProbeRequestMs_ = now;
    advancingMediaProbeFrame_ = current.frameSeq;
    advancingMediaHaveProbe_ = true;
    SubmitProbe(ImmutableStamp(current), current.pixels);
}

std::int64_t UltraClearTextController::ModelTime(std::int64_t wallNowMs) const
{
    if (advancingDisplay_) return advancingDisplay_->timestampMs;
    return advancingMediaScene_ ? advancingMediaScene_->current.timestampMs : wallNowMs;
}

bool UltraClearTextController::AdvancingValidationFresh(std::int64_t wallNowMs) const
{
    if (!advancingDisplay_ || immutableScene_) return true;
    if (!haveAdvancingValidation_) return false; // nothing real validated yet
    if (advancingDisplay_->frameSeq == advancingValidatedFrameSeq_) return true;
    return wallNowMs - advancingValidatedWallMs_ <= timing_.mediaValidationFreshnessMs;
}

void UltraClearTextController::NoteAdvancingValidation(std::int64_t wallNowMs)
{
    if (!advancingDisplay_ || immutableScene_) return;
    // Record only lines whose own media validation is still fresh for the
    // displayed frame, so a late result for an old frame cannot renew the
    // wall bound and revive text after a no-analysis gap expired it.
    const std::int64_t displayPts = advancingDisplay_->timestampMs;
    bool found = false;
    std::uint64_t newestFrame = 0;
    for (const auto& line : model_.Lines()) {
        if (!line.valid) continue;
        if (line.validatedFrameSeq != advancingDisplay_->frameSeq &&
            displayPts - line.validatedAtMs > timing_.mediaValidationFreshnessMs) continue;
        found = true;
        newestFrame = std::max(newestFrame, line.validatedFrameSeq);
    }
    if (!found) return;
    haveAdvancingValidation_ = true;
    advancingValidatedWallMs_ = wallNowMs;
    advancingValidatedFrameSeq_ = newestFrame;
}

void UltraClearTextController::UpdateImmutableView(const ViewTransform& transform,
                                                   std::uint32_t viewportWidth,
                                                   std::uint32_t viewportHeight, int strokeWeight)
{
    if (!HasImmutableScene()) return;
    if (scheduler_) scheduler_->SetShadowFocus(uct::ViewRectToScene(transform, 1, 1, {0, 0, 1, 1}));
    const auto previousView = immutableScene_ ? immutableScene_->transform : ViewTransform{};
    const bool detailViewChanged = immutableScene_ &&
        (previousView.valid != transform.valid || previousView.sourceX != transform.sourceX ||
         previousView.sourceY != transform.sourceY || previousView.sourceWidth != transform.sourceWidth ||
         previousView.sourceHeight != transform.sourceHeight);
    if (immutableScene_) {
        immutableScene_->transform = transform;
        immutableScene_->viewportWidth = viewportWidth;
        immutableScene_->viewportHeight = viewportHeight;
        immutableScene_->strokeWeight = strokeWeight;
    } else {
        // View is display state: keep the display receipt and (when present)
        // the retained analysis scene's projection in sync.
        if (advancingDisplay_) {
            advancingDisplay_->transform = transform;
            advancingDisplay_->viewportWidth = viewportWidth;
            advancingDisplay_->viewportHeight = viewportHeight;
            advancingDisplay_->strokeWeight = strokeWeight;
        }
        if (advancingMediaScene_) {
            advancingMediaScene_->transform = transform;
            advancingMediaScene_->viewportWidth = viewportWidth;
            advancingMediaScene_->viewportHeight = viewportHeight;
            advancingMediaScene_->strokeWeight = strokeWeight;
        }
    }
    if (overlay_) {
        overlay_->SetViewTransform(transform);
        overlay_->SetStrokeWeight(strokeWeight);
    }
    // The artifact covers the complete scene. View changes are projection
    // only, and never renew source timestamps or enqueue OCR/probes. A
    // settled paused view may later re-detect its visible region once.
    RefreshOverlay(clock_());
    UpdateReadingText();
    UpdateStatus();
    if (detailViewChanged) ScheduleImmutableDetail();
}

void UltraClearTextController::ClearImmutableScene()
{
    mediaOverlaySuppressed_ = false;
    if (!HasImmutableScene()) { UpdateStatus(); return; }
    ++geometryGeneration_;
    Invalidate(QStringLiteral("scene"));
    immutableScene_.reset();
    advancingMediaScene_.reset();
    advancingDisplay_.reset();
    advancingDeviceIdentity_.reset();
    advancingReceiptValid_ = false;
    haveGeometry_ = false;
    cameraActive_ = false;
    presentationAvailable_ = false;
    presentationFailed_ = false;
    settling_ = false;
    compositePaused_ = false;
    analysisPoolFailed_ = false;
    if (enabled_) watchdog_->start();
    UpdateStatus();
}

void UltraClearTextController::OnMediaPresentationSucceeded()
{
    if (!HasImmutableScene()) return;
    // This receipt changes presentation availability only. In particular it
    // neither renews media PTS nor resubmits a completed immutable artifact.
    const bool recovered = presentationFailed_;
    presentationFailed_ = false;
    RefreshOverlay(clock_());
    UpdateReadingText();
    UpdateStatus();
    // Only an actual recovery re-asks for visible detail: the settle deadline
    // may have expired behind the failure latch, and no view change or OCR
    // result will follow to ask again. Ordinary success receipts (which can
    // arrive every present) must not keep restarting the settle timer.
    if (recovered) ScheduleImmutableDetail();
}

void UltraClearTextController::OnMediaPresentationFailed()
{
    if (!HasImmutableScene()) return;
    // Retain the scene/model so a successful exact-frame receipt can restore
    // it, while immediately revoking every currently visible text surface.
    // Keep the latch even while disabled or awaiting backend readiness.
    presentationFailed_ = true;
    const bool hadWords = !overlayWords_.isEmpty();
    UpdatePositionedTimingPublication(hadWords, false, true);
    if (overlay_) overlay_->ClearWords();
    if (screenText_) screenText_->RevokeAll();
    overlayWords_.clear();
    shownWords_ = 0;
    UpdateReadingText(hadWords);
    UpdateStatus();
}

bool UltraClearTextController::OnPresented(const UctFrameContext& context)
{
    if (HasImmutableScene()) return false;
    ++presentCount_;
    if (!enabled_) {
        return false;
    }
    const std::int64_t now = clock_();

    cameraActive_ = context.cameraActive;
    attemptedFrameSeq_ = context.frameSeq;
    attemptedTransform_ = context.transform;
    if (scheduler_) scheduler_->SetShadowFocus(uct::ViewRectToScene(context.transform, 1, 1, {0, 0, 1, 1}));
    attemptedViewport_ = QSize(context.viewportWidth, context.viewportHeight);
    if (overlay_) {
        overlay_->SetStrokeWeight(context.strokeWeight);
        overlay_->SetViewTransform(context.transform);
    }
    // A requested change revokes text at once: the presented scene is still
    // built with the old settings, so nothing recognized before the change
    // may stay up, and nothing recognized until the matching scene arrives
    // belongs to the new request.
    const bool settling = context.cameraActive && !(context.requested == context.geometry.settings);
    if (settling && !settling_) {
        ++geometryGeneration_;
        Invalidate(QStringLiteral("settling"));
    }
    settling_ = settling;
    if (!haveGeometry_ || !(context.geometry == geometry_)) {
        const bool hadGeometry = haveGeometry_;
        geometry_ = context.geometry;
        haveGeometry_ = true;
        ++geometryGeneration_;
        Invalidate(hadGeometry ? QStringLiteral("scene") : QString());
    }
    // Name the pause truthfully: an unsupported composite from a stabilized
    // scene is Extra Stable's held frames; the CPU debug composite (applied
    // stabilization is always false there) keeps its own reason.
    compositeExtraStable_ = context.geometry.settings.stabilization || context.heldFrames;
    if (context.unsupportedCompositeFrames != compositePaused_) {
        compositePaused_ = context.unsupportedCompositeFrames;
        if (compositePaused_) {
            Invalidate(QStringLiteral("composite"));
        }
    }
    if (pendingReadbackId_ != 0 && now - pendingSinceMs_ > timing_.readbackTimeoutMs) {
        // The presenter drops copies across resizes; never wait forever.
        pendingReadbackId_ = 0;
    }
    if (deviceCandidate_ && now - deviceCandidateSinceMs_ > timing_.readbackTimeoutMs) {
        deviceCandidate_.reset();
        deviceProbeReader_->Cancel();
        ++consecutiveProbeFailures_;
    }
    // Freshness is enforced on the presentation clock, not the OCR clock.
    RefreshOverlay(now);
    UpdateStatus();
    if (settling_ || compositePaused_ || mediaOverlaySuppressed_ || !cameraActive_ || pendingReadbackId_ != 0 ||
        (scheduler_ && !BackendUsable(backend_))) {
        return false;
    }
    // Latest-only end to end: no new viewport copy while the probe worker
    // still has one waiting to start.
    if (deviceCandidate_ || (deviceProbeReader_ && deviceProbeReader_->HasWork()) ||
        (probeWorker_ && probeWorker_->HasPending())) {
        return false;
    }
    return now - lastProbeRequestMs_ >= timing_.probeIntervalMs;
}

bool UltraClearTextController::PresentationAvailable(std::int64_t nowMs) const
{
    if (HasImmutableScene() && presentationFailed_) return false;
    if (advancingDisplay_ || advancingMediaScene_) {
        // Display-receipt validity, not the (possibly released or withheld)
        // analysis lease: the displayed scene stays presentable after its
        // analysis image has been consumed, and during a no-analysis gap.
        // Whether any text may show is decided separately by real
        // validation freshness (media PTS in the model, wall time in
        // AdvancingValidationFresh) and analysis health.
        const ViewTransform& transform = advancingDisplay_ ? advancingDisplay_->transform
                                                           : advancingMediaScene_->transform;
        const std::uint32_t width = advancingDisplay_ ? advancingDisplay_->viewportWidth
                                                      : advancingMediaScene_->viewportWidth;
        const std::uint32_t height = advancingDisplay_ ? advancingDisplay_->viewportHeight
                                                       : advancingMediaScene_->viewportHeight;
        return transform.valid && width > 0 && height > 0;
    }
    if (immutableScene_) {
        return FrameInputValid(immutableScene_->selected, immutableScene_->geometry) &&
            immutableScene_->transform.valid && immutableScene_->viewportWidth > 0 &&
            immutableScene_->viewportHeight > 0;
    }
    return presentationAvailable_ && !presentationFailed_ &&
           nowMs - lastPresentMs_ <= kPresentationWatchdogMs;
}

void UltraClearTextController::OnPresentationSucceeded()
{
    if (!enabled_ || HasImmutableScene()) {
        return;
    }
    const std::int64_t now = clock_();
    const bool restoring = !PresentationAvailable(now);
    lastPresentMs_ = now;
    currentFrameSeq_ = attemptedFrameSeq_;
    presentedTransform_ = attemptedTransform_;
    presentedViewport_ = attemptedViewport_;
    presentedGeometryGeneration_ = geometryGeneration_;
    presentationAvailable_ = cameraActive_;
    presentationFailed_ = false;
    RefreshOverlay(now);
    if (restoring) {
        UpdateReadingText();
        UpdateStatus();
    }
}

void UltraClearTextController::ReplaceRecognizerFactory(RecognizerFactory factory)
{
    const bool enabled=enabled_;
    if(enabled)SetEnabled(false);
    factory_=std::move(factory);
    if(enabled)SetEnabled(true);
}

void UltraClearTextController::OnPresentationDeferred()
{
    if (!enabled_ || HasImmutableScene()) return;
    if (presentedGeometryGeneration_ != geometryGeneration_ ||
        presentedViewport_ != attemptedViewport_ || attemptedViewport_.isEmpty() ||
        !attemptedTransform_.valid) {
        OnPresentationFailed();
        return;
    }
    // OnPresented stages the next transform before the nonblocking presenter
    // knows whether its frame slot is free. Revert that projection when no draw
    // was submitted, instead of hiding and recreating the overlay on each pan.
    if (overlay_ && presentedGeometryGeneration_ == geometryGeneration_) {
        overlay_->SetViewTransform(presentedTransform_);
    }
    RefreshOverlay(clock_());
    UpdateReadingText();
    UpdateStatus();
}

void UltraClearTextController::OnSourceRetired()
{
    if (HasImmutableScene()) return; // camera retirement cannot retire selected media
    cameraActive_ = false;
    presentationAvailable_ = false;
    haveGeometry_ = false;
    ++geometryGeneration_;
    Invalidate(QStringLiteral("scene"));
    UpdateStatus();
}

void UltraClearTextController::OnPresentationFailed()
{
    if (!enabled_ || HasImmutableScene()) {
        return;
    }
    presentationFailed_ = true;
    presentationAvailable_ = false;
    const bool hadWords = !overlayWords_.isEmpty();
    UpdatePositionedTimingPublication(hadWords, false, true);
    if (overlay_) {
        overlay_->ClearWords();
    }
    if (screenText_) screenText_->RevokeAll();
    overlayWords_.clear();
    shownWords_ = 0;
    UpdateReadingText(hadWords);
    UpdateStatus();
}

void UltraClearTextController::OnReadbackRequested(std::uint64_t requestId,
                                                   const UctFrameContext& context)
{
    if (!enabled_ || HasImmutableScene() || requestId == 0 || settling_) {
        return; // ring full: the next presentation retries
    }
    const std::int64_t now = clock_();
    lastProbeRequestMs_ = now;
    pendingReadbackId_ = requestId;
    pendingSinceMs_ = now;
    pendingStamp_ = MakeStamp(context, now);
}

bool UltraClearTextController::OwnsReadback(std::uint64_t requestId) const noexcept
{
    return requestId != 0 && requestId == pendingReadbackId_;
}

void UltraClearTextController::OnReadbackCompleted(std::uint64_t requestId, std::vector<std::uint8_t>& bgra,
                                                   int width, int height, int strideBytes)
{
    if (!OwnsReadback(requestId)) {
        return;
    }
    pendingReadbackId_ = 0;
    if (!enabled_) {
        return;
    }
    uct::SourceStamp stamp = pendingStamp_;
    stamp.viewportWidth = static_cast<std::uint32_t>(std::max(0, width));
    stamp.viewportHeight = static_cast<std::uint32_t>(std::max(0, height));
    SubmitProbe(stamp, pool_->Adopt(bgra, width, height, strideBytes));
}

void UltraClearTextController::OnReadbackCompleted(std::uint64_t requestId, const std::uint8_t* bgra,
                                                   int width, int height, int strideBytes)
{
    if (!OwnsReadback(requestId)) {
        return;
    }
    pendingReadbackId_ = 0;
    if (!enabled_) {
        return;
    }
    uct::SourceStamp stamp = pendingStamp_;
    stamp.viewportWidth = static_cast<std::uint32_t>(std::max(0, width));
    stamp.viewportHeight = static_cast<std::uint32_t>(std::max(0, height));
    SubmitProbe(stamp, pool_->Copy(bgra, width, height, strideBytes));
}

void UltraClearTextController::OnCpuViewport(const UctFrameContext& context, const std::uint8_t* bgra,
                                             int width, int height, int strideBytes)
{
    if (!enabled_ || HasImmutableScene() || settling_) {
        return;
    }
    const std::int64_t now = clock_();
    lastProbeRequestMs_ = now;
    uct::SourceStamp stamp = MakeStamp(context, now);
    stamp.viewportWidth = static_cast<std::uint32_t>(std::max(0, width));
    stamp.viewportHeight = static_cast<std::uint32_t>(std::max(0, height));
    auto pixels = pool_->Copy(bgra, width, height, strideBytes);
    // This path no longer depends on the CUDA snapshot pool. Only an actual
    // valid current CPU image may clear its previous resource failure.
    if (pixels && !compositePaused_ && stamp.SameScene(CurrentScene())) OnAnalysisHealth(false);
    SubmitProbe(stamp, std::move(pixels));
}

uct::SchedulerStats UltraClearTextController::SchedulerStatistics() const
{
    return scheduler_ ? scheduler_->Stats() : uct::SchedulerStats{};
}

void UltraClearTextController::OnDeviceScene(const UctFrameContext& context, uct::SharedDeviceImage device)
{
    if (HasImmutableScene()) return;
    if (deviceCandidate_ || (deviceProbeReader_ && deviceProbeReader_->HasWork())) return;
    if (!enabled_ || settling_ || compositePaused_ || mediaOverlaySuppressed_ || !scheduler_ || !device) return;
    const auto now=clock_();
    if (device->sourceEpoch!=context.geometry.settings.sourceEpoch || device->timestampMs>now ||
        now-device->timestampMs>1000 || device->frameSequence!=context.frameSeq) return;
    if (device->kind == uct::AnalysisKind::TemporallySmoothed &&
        (device->blendFrameCount == 0 || device->seedFrameSequence > device->frameSequence ||
         device->historyEpoch == 0 || !(device->temporalAlpha >= 0.0f) ||
         device->temporalAlpha > 1.0f || !device->ValidationData())) return;
    // Extra Stable can show an earlier held acquisition or a crossfade under
    // this frame's sequence. Such a capture must carry the frame's own device
    // record; an unnamed image is skipped rather than trusted as live.
    const bool holdRecorded = device->HasHoldProvenance();
    if (context.heldFrames && !holdRecorded) return;
    auto stamp=MakeStamp(context,device->timestampMs);
    // Unresolved until the probe worker reads the record after the copy.
    if (holdRecorded) stamp.holdDisplay = uct::HoldDisplay::Unknown;
    stamp.frameSeq=device->frameSequence;
    stamp.temporallySmoothed = device->kind == uct::AnalysisKind::TemporallySmoothed;
    stamp.historyEpoch = device->historyEpoch;
    stamp.seedFrameSeq = device->seedFrameSequence;
    stamp.blendFrameCount = device->blendFrameCount;
    stamp.temporalAlpha = device->temporalAlpha;
    stamp.viewTransform={}; stamp.viewTransform.valid=true; // independent complete scene, no display crop
    stamp.viewportWidth=device->width;stamp.viewportHeight=device->height;
    lastProbeRequestMs_=now;
    deviceCandidate_=device;
    deviceCandidateSinceMs_=now;
    if (!deviceProbeReader_->SubmitDevice(stamp,std::move(device))) deviceCandidate_.reset();
}

void UltraClearTextController::OnAnalysisHealth(bool failed)
{
    if (HasImmutableScene()) return; // live readback health is unrelated to owned media pixels
    if (analysisPoolFailed_ == failed) return;
    analysisPoolFailed_ = failed;
    RefreshOverlay(clock_());
    UpdateReadingText();
    UpdateStatus();
}

uct::ProbeWorkerStats UltraClearTextController::ProbeStatistics() const
{
    return probeWorker_ ? probeWorker_->Stats() : uct::ProbeWorkerStats{};
}

uct::SourceStamp UltraClearTextController::MakeStamp(const UctFrameContext& context,
                                                     std::int64_t nowMs) const
{
    uct::SourceStamp stamp;
    stamp.sourceEpoch = context.geometry.settings.sourceEpoch;
    stamp.geometryGeneration = geometryGeneration_;
    stamp.frameSeq = context.frameSeq;
    stamp.liveCameraCapture = context.cameraActive;
    stamp.timestampMs = nowMs;
    stamp.viewTransform = context.transform;
    stamp.viewportWidth = context.viewportWidth;
    stamp.viewportHeight = context.viewportHeight;
    return stamp;
}

uct::SourceStamp UltraClearTextController::CurrentScene() const
{
    uct::SourceStamp stamp;
    stamp.sourceEpoch = geometry_.settings.sourceEpoch;
    stamp.geometryGeneration = geometryGeneration_;
    return stamp;
}

void UltraClearTextController::Invalidate(const QString& reason)
{
    if (uct::timing::Enabled()) {
        uct::timing::Key key;
        key.producer = scheduler_ ? scheduler_->TimingProducerId() : 0;
        key.sourceEpoch = geometry_.settings.sourceEpoch;
        key.geometryGeneration = geometryGeneration_;
        key.authority = scheduler_ ? scheduler_->Authority() : 0;
        uct::timing::RecordEvent(uct::timing::Event::Reset, key);
    }
    if (scheduler_) {
        scheduler_->Cancel(); // in-flight input stays owned by the worker
    }
    if (probeWorker_) {
        probeWorker_->Cancel();
    }
    if(deviceProbeReader_)deviceProbeReader_->Cancel();
    model_.Reset();
    timingAdmission_.reset();
    timingPositionedPublication_ = {};
    UpdatePositionedTimingPublication(false, false);
    UpdatePaneTimingPublication(false, true);
    immutableSelectedResult_.reset();
    immutableSelectedJob_ = 0;
    immutableCorroborationJob_ = 0;
    immutableSelectedAttempted_ = false;
    immutableRetryCount_ = 0;
    immutableRetryScheduled_ = immutableRetryExhausted_ = false;
    immutableRetryStartedAtMs_.reset();
    immutableCorroborationAttempted_ = false;
    immutableDetailFocus_.reset();
    immutableDetailDone_.clear();
    if (immutableDetailTimer_) immutableDetailTimer_->stop();
    advancingMediaJob_ = 0;
    haveAdvancingValidation_ = false;
    advancingValidatedFrameSeq_ = 0;
    advancingMediaLastSubmitMs_ = std::numeric_limits<std::int64_t>::min() / 2;
    advancingMediaProbeFrame_ = 0;
    advancingMediaHaveProbe_ = false;
    latestProbe_.reset();
    deviceCandidate_.reset();
    stableProbes_ = 0;
    changedSinceSubmit_ = true;
    probeInvalidatedSinceSubmit_ = false;
    lastJobCoverage_ = {};
    haveLastOcr_ = false;
    pendingReadbackId_ = 0;
    lastResultMoved_ = false;
    lastResultEmpty_ = false;
    shownWords_ = -1;
    overlayWords_.clear();
    if (overlay_) {
        overlay_->ClearWords();
    }
    if (screenText_) screenText_->RevokeAll(); // synchronous, before any timer
    readingText_.clear();
    if (panel_) {
        panel_->SetReadingText({});
    }
    Q_UNUSED(reason);
}

void UltraClearTextController::SubmitProbe(const uct::SourceStamp& stamp, uct::SharedPixels pixels)
{
    if (immutableScene_ || !pixels || !probeWorker_ || settling_ || compositePaused_ || mediaOverlaySuppressed_ || !stamp.SameScene(CurrentScene())) {
        return; // the scene changed while this copy was in flight
    }
    uct::ProbeRequest request;
    request.stamp = stamp;
    request.decimation = std::max(timing_.probeDecimation,
                                  (pixels->width + kMaximumProbeWidth - 1) / kMaximumProbeWidth);
    request.pixels = std::move(pixels);
    request.previous = latestProbe_;
    request.lines = model_.LineReferences(CurrentScene());
    request.tuning = uct::ModelTuning{}.match;
    request.budgetMs = timing_.probeCheckBudgetMs;
    probeWorker_->Submit(std::move(request));
}

void UltraClearTextController::HandleProbeOutcome(uct::ProbeOutcome outcome)
{
    if (probeWorker_) uct::TraceTiming(uct::timing::Event::UiReceipt, probeWorker_->TimingProducerId(),
        uct::timing::Channel::Probe, outcome.stamp, outcome.jobId, outcome.authority);
    QElapsedTimer handling;
    handling.start();
    if (!enabled_ || immutableScene_ || !probeWorker_ || !probeWorker_->IsCurrent(outcome) || settling_ || compositePaused_ || mediaOverlaySuppressed_ ||
        !outcome.stamp.SameScene(CurrentScene()) || !outcome.probe ||
        (latestProbe_ && outcome.stamp.frameSeq<latestProbe_->stamp.frameSeq)) {
        if (probeWorker_) uct::TraceTiming(uct::timing::Event::Fault, probeWorker_->TimingProducerId(),
            uct::timing::Channel::Probe, outcome.stamp, outcome.jobId, outcome.authority,
            {1, 0, uct::timing::Stale});
        return;
    }
    const std::int64_t now = clock_();
    // An Extra Stable crossfade (or unreadable record) composites two
    // acquisitions. It may hide or remove text, never renew its proof.
    const bool composite = !uct::EvidenceAcquisition(outcome.stamp);
    const uct::ValidationSummary summary = model_.ApplyChecks(outcome.checks, outcome.stamp, !composite);
    if (summary.valid > 0) NoteAdvancingValidation(now); // real pixels only
    if (!composite && summary.invalid > 0) {
        // A changed portion can disappear while unchanged accepted lines
        // remain. Request its replacement without waiting for the periodic
        // refresh; retain demand until the ordinary worker/pacing gates admit
        // a job, since the failed line is removed by this probe.
        probeInvalidatedSinceSubmit_ = true;
        changedSinceSubmit_ = true;
    }
    if (summary.invalid > 0 && summary.valid == 0) {
        lastResultMoved_ = true;
    }
    if (composite) {
        // Retain its identity as an admission barrier for late OCR results,
        // never as independent proof or OCR input.
        latestProbe_ = outcome.probe;
        stableProbes_ = 0;
        changedSinceSubmit_ = true;
        if (deviceCandidate_ && deviceCandidate_->frameSequence == outcome.stamp.frameSeq)
            deviceCandidate_.reset();
        RefreshOverlay(now);
        UpdateReadingText();
        UpdateStatus();
        lastOutcomeMs_ = handling.nsecsElapsed() / 1e6;
        return;
    }
    const bool stable = outcome.similarity && *outcome.similarity >= timing_.stableSimilarity;
    stableProbes_ = stable ? stableProbes_ + 1 : 0;
    if (!stable) {
        changedSinceSubmit_ = true;
    }
    latestProbe_ = outcome.probe;

    auto candidate=deviceCandidate_ && deviceCandidate_->frameSequence==outcome.stamp.frameSeq
        ? std::exchange(deviceCandidate_,{}) : uct::SharedDeviceImage{};
    if (scheduler_ && BackendUsable(backend_) && stableProbes_ >= 1 && (outcome.pixels||candidate)) {
        const int width = candidate?candidate->width:outcome.pixels->width;
        const int height = candidate?candidate->height:outcome.pixels->height;
        const QRectF viewScene =
            uct::ViewRectToScene(outcome.stamp.viewTransform, width, height, QRectF(0, 0, width, height))
                .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        const bool covered = Area(viewScene) > 0.0 &&
                             Area(lastJobCoverage_.intersected(viewScene)) >= 0.8 * Area(viewScene);
        const std::int64_t sinceSubmit =
            now - (advancingMediaScene_ ? advancingMediaLastSubmitMs_ : lastSubmitMs_);
        // A held image repeats an acquisition already read: it cannot confirm
        // pending words, so it is re-read only to recover lost text geometry
        // (for example after keystone drift) at the ordinary refresh cadence.
        const std::uint64_t acquisition = uct::EvidenceAcquisition(outcome.stamp).value_or(0);
        const bool repeatedHold = outcome.stamp.holdDisplay == uct::HoldDisplay::Held && haveLastOcr_ &&
            lastOcrGeneration_ == geometryGeneration_ && lastOcrAcquisition_ == acquisition;
        const bool needed = repeatedHold
            ? !model_.HasAcceptedText() && sinceSubmit >= timing_.refreshIntervalMs
            : probeInvalidatedSinceSubmit_ || model_.PendingWordCount() > 0 || !model_.HasAcceptedText() || !covered ||
                  sinceSubmit >= timing_.refreshIntervalMs;
        // Latest-only: a busy worker only receives a replacement after the
        // scene changed, so slow recognition cannot accumulate work. The
        // pixels move on without a copy.
        // Device snapshots share a two-slot pool with validation. Never park
        // a pending OCR lease while inference holds the other slot.
        const bool canSubmit = advancingMediaScene_
            ? advancingMediaJob_ == 0 && !scheduler_->HasWork() &&
                now - advancingMediaLastSubmitMs_ >= timing_.mediaRecognitionIntervalMs
            : !scheduler_->HasWork() || (!candidate && changedSinceSubmit_);
        if (needed && canSubmit) {
            const auto job=candidate?scheduler_->SubmitDevice(outcome.stamp,std::move(candidate))
                                    :scheduler_->Submit(outcome.stamp, std::move(outcome.pixels));
            if (job != 0) {
                lastSubmitMs_ = now;
                lastJobCoverage_ = viewScene;
                changedSinceSubmit_ = false;
                probeInvalidatedSinceSubmit_ = false;
                lastOcrAcquisition_ = acquisition;
                lastOcrGeneration_ = geometryGeneration_;
                lastOcrJob_ = job;
                haveLastOcr_ = true;
                if (advancingMediaScene_) {
                    advancingMediaJob_ = job;
                    advancingMediaSubmittedAtMs_ = now;
                    advancingMediaLastSubmitMs_ = now;
                }
            }
        }
    }
    RefreshOverlay(now);
    UpdateReadingText();
    UpdateStatus();
    lastOutcomeMs_ = handling.nsecsElapsed() / 1e6;
}

void UltraClearTextController::HandleResult(uct::RecognitionResult result)
{
    TraceRecognition(uct::timing::Event::UiReceipt, scheduler_.get(), result);
    if (immutableScene_) {
        HandleImmutableResult(std::move(result));
        return;
    }
    QElapsedTimer handling;
    handling.start();
    if (!enabled_ || !scheduler_ || !scheduler_->IsCurrent(result) || settling_ || compositePaused_ || mediaOverlaySuppressed_ ||
        !result.stamp.SameScene(CurrentScene())) {
        TraceRecognition(uct::timing::Event::Fault, scheduler_.get(), result, {1, 0, uct::timing::Stale});
        ++rejectedResults_;
        return;
    }
    const std::int64_t now = clock_();
    const auto retryUnreadAcquisition = [&] {
        if (result.jobId == lastOcrJob_) {
            haveLastOcr_ = false;
            changedSinceSubmit_ = true;
        }
    };
    if (advancingMediaScene_) {
        if (result.jobId != advancingMediaJob_ || advancingMediaJob_ == 0 ||
            now - advancingMediaSubmittedAtMs_ > timing_.immutableJobTimeoutMs) {
            if (result.jobId == advancingMediaJob_) {
                advancingMediaJob_ = 0;
                retryUnreadAcquisition();
            }
            ++rejectedResults_;
            return;
        }
        advancingMediaJob_ = 0;
    }
    if (result.output.compositeInput) {
        // The worker found Extra Stable crossfade pixels: no recognition ran.
        // Neither evidence nor a failure; a later single acquisition is read.
        changedSinceSubmit_ = true;
        retryUnreadAcquisition();
        ++rejectedResults_;
        UpdateStatus();
        return;
    }
    // Admit compares recognized pixels with the newest independent scene probe.
    // The admission probe for this exact device frame is not a motion test.
    if (result.output.inputFault && !result.output.cancelled) ++consecutiveInputFailures_;
    else if (result.output.ok) consecutiveInputFailures_ = 0;
    if (!result.output.ok && !result.output.cancelled && !result.output.inputFault && !result.output.backendPreparing) {
        if (++consecutiveFailures_ >= kMaximumConsecutiveFailures) {
            backend_.state = uct::BackendState::kFailed;
            backend_.detail = result.output.error;
            TraceRecognition(uct::timing::Event::BackendState, scheduler_.get(), result,
                {static_cast<std::uint32_t>(backend_.state)});
            Invalidate(QStringLiteral("failed"));
        }
    } else if (result.output.ok) {
        consecutiveFailures_ = 0;
    }
    const uct::AdmitVerdict verdict = advancingMediaScene_
        ? model_.AdmitAdvancingMedia(result, CurrentScene(), latestProbe_.get(), ModelTime(now))
        : model_.AdmitLiveCamera(result, CurrentScene(), latestProbe_.get(), now);
    NoteTimingAdmission(result, verdict);
    if (uct::timing::Enabled()) TraceRecognition(uct::timing::Event::Admit, scheduler_.get(), result,
        {static_cast<std::uint32_t>(verdict), static_cast<std::uint32_t>(model_.AcceptedWordCount())});
    if (verdict != uct::AdmitVerdict::kAccepted && verdict != uct::AdmitVerdict::kPending &&
        verdict != uct::AdmitVerdict::kEmpty)
        retryUnreadAcquisition();
    switch (verdict) {
    case uct::AdmitVerdict::kAccepted:
        ++acceptedResults_;
        lastResultMoved_ = false;
        lastResultEmpty_ = false;
        break;
    case uct::AdmitVerdict::kPending:
        lastResultMoved_ = false;
        lastResultEmpty_ = false;
        break;
    case uct::AdmitVerdict::kContentMoved:
        lastResultMoved_ = true;
        changedSinceSubmit_ = true;
        break;
    case uct::AdmitVerdict::kEmpty:
        lastResultEmpty_ = true;
        lastResultMoved_ = false;
        break;
    default:
        ++rejectedResults_;
        break;
    }
    if (verdict == uct::AdmitVerdict::kAccepted || verdict == uct::AdmitVerdict::kPending)
        NoteAdvancingValidation(now); // stale admissions are filtered inside
    RefreshOverlay(now);
    UpdateReadingText();
    UpdateStatus();
    lastResultMs_ = handling.nsecsElapsed() / 1e6;
    emit recognitionAdmitted(static_cast<int>(verdict));
}

void UltraClearTextController::HandleBackendStatus(uct::BackendStatus status)
{
    if (uct::timing::Enabled()) {
        uct::timing::Key key;
        key.producer = scheduler_ ? scheduler_->TimingProducerId() : 0;
        uct::timing::RecordEvent(uct::timing::Event::BackendState, key,
            {static_cast<std::uint32_t>(status.state), 0, status.usesGpu ? uct::timing::DeviceInput : 0u});
    }
    const uct::BackendState previousBackendState = backend_.state;
    backend_ = std::move(status);
    if (!BackendUsable(backend_)) {
        if (HasProvenImmutableArtifact()) {
            StopImmutableRefinement();
            RefreshOverlay(clock_());
            UpdateReadingText();
        } else Invalidate(QStringLiteral("unavailable"));
    }
    if (screenText_) screenText_->ContentChanged(); // recognizer language may differ
    // Backend readiness is an event, not a reason to poll every presentation
    // or timer tick. A waiting selected frame is submitted exactly once.
    if (immutableScene_ && backend_.state == uct::BackendState::kReady) {
        SubmitImmutableIfNeeded();
        // A settled paused view whose detail deadline expired while the
        // backend was still preparing gets one fresh ask on the transition
        // into ready; if a job was just submitted above, its result (not
        // this timer) remains the path that schedules detail afterwards.
        if (previousBackendState != uct::BackendState::kReady) ScheduleImmutableDetail();
    } else if (advancingMediaScene_ && backend_.state == uct::BackendState::kReady) {
        SubmitAdvancingMediaProbe();
    }
    UpdateStatus();
}

uct::SourceStamp UltraClearTextController::ImmutableStamp(const UctImmutableFrame& frame) const
{
    uct::SourceStamp stamp = CurrentScene();
    stamp.frameSeq = frame.frameSeq;
    stamp.timestampMs = frame.timestampMs;
    stamp.viewTransform.valid = true; // full-scene pixels, independent of current view
    if (const auto device = EffectiveDevice(frame, geometry_)) {
        stamp.viewportWidth = static_cast<std::uint32_t>(device->width);
        stamp.viewportHeight = static_cast<std::uint32_t>(device->height);
        stamp.temporallySmoothed = device->kind == uct::AnalysisKind::TemporallySmoothed;
        stamp.historyEpoch = device->historyEpoch;
        stamp.seedFrameSeq = device->seedFrameSequence;
        stamp.blendFrameCount = device->blendFrameCount;
        stamp.temporalAlpha = device->temporalAlpha;
        // Extra Stable media pixels are named only by the worker-read record.
        if (device->HasHoldProvenance()) stamp.holdDisplay = uct::HoldDisplay::Unknown;
    } else if (frame.pixels) {
        stamp.viewportWidth = static_cast<std::uint32_t>(frame.pixels->width);
        stamp.viewportHeight = static_cast<std::uint32_t>(frame.pixels->height);
    }
    return stamp;
}

void UltraClearTextController::SubmitImmutableIfNeeded()
{
    if (immutableRetryScheduled_ || immutableRetryExhausted_) return;
    if (mediaOverlaySuppressed_ || !enabled_ || !immutableScene_ || !scheduler_ || backend_.state != uct::BackendState::kReady ||
        !AnalysisHealthy()) return;
    // Preferred device input first; owned CPU pixels remain the fallback of
    // the same bounded attempt when the device hand-off is refused.
    const auto submitFrame = [this](const UctImmutableFrame& frame) {
        const auto stamp = ImmutableStamp(frame);
        std::uint64_t job = 0;
        uct::CorroborationRequest corroboration;
        corroboration.tuning = uct::ModelTuning{}.match;
        // A visible-detail selected job is the same acquisition; registering
        // the previous selected lines against identical pixels is no evidence.
        const bool detailSelected = immutableDetailFocus_ && frame.frameSeq == immutableScene_->selected.frameSeq;
        if (immutableSelectedResult_ && !detailSelected) {
            corroboration.selectedJobId = immutableSelectedResult_->jobId;
            corroboration.selectedReference = immutableSelectedResult_->reference;
            corroboration.selectedLines = immutableSelectedResult_->prepared;
            for (const auto& line : immutableSelectedResult_->prepared)
                corroboration.references.push_back(line.reference);
        }
        if (const auto device = EffectiveDevice(frame, geometry_)) {
            job = scheduler_->SubmitDevice(stamp, device, corroboration, immutableDetailFocus_);
        }
        if (job == 0 && PixelsMatchFrame(frame, geometry_))
            job = scheduler_->Submit(stamp, frame.pixels, std::move(corroboration), immutableDetailFocus_);
        return job;
    };
    if (!immutableSelectedAttempted_) {
        if (!immutableRetryStartedAtMs_) immutableRetryStartedAtMs_ = clock_();
        immutableSelectedAttempted_ = true;
        immutableSubmittedAtMs_ = clock_();
        immutableSelectedJob_ = submitFrame(immutableScene_->selected);
        if (!immutableSelectedJob_) ScheduleImmutableRetry(true);
        return;
    }
    if (!immutableSelectedResult_ || !immutableSelectedResult_->output.ok ||
        immutableCorroborationAttempted_) return;
    // Bound corroboration to one actual neighbouring image. A separate
    // buffer with identical pixels still fails the worker digest comparison
    // in AdmitImmutable; a reused lease is rejected before scheduling.
    immutableCorroborationAttempted_ = true;
    const bool usableNeighbour = immutableScene_->corroboration &&
                                 CorroborationInputValid(*immutableScene_, *immutableScene_->corroboration);
    if (!usableNeighbour) {
        FinishImmutableDetail(); // a region without its independent half proves nothing new
        return;
    }
    const auto& neighbour = *immutableScene_->corroboration;
    immutableSubmittedAtMs_ = clock_();
    immutableCorroborationJob_ = submitFrame(neighbour);
    if (!immutableCorroborationJob_) ScheduleImmutableRetry(false);
}

bool UltraClearTextController::HasProvenImmutableArtifact() const
{
    if (!immutableScene_ || !haveGeometry_ || geometry_ != immutableScene_->geometry ||
        currentFrameSeq_ != immutableScene_->selected.frameSeq ||
        !FrameInputValid(immutableScene_->selected, geometry_)) return false;
    // SetImmutableScene invalidates on ANY selected lease/frame/PTS, source
    // authority or geometry change. Its generation therefore binds these
    // stamps to the exact retained CPU/GPU image; neighbour/view updates do
    // not change that ticket. Reading-only candidates do not qualify.
    const auto selected = ImmutableStamp(immutableScene_->selected);
    const auto now = clock_();
    return std::any_of(model_.Lines().begin(), model_.Lines().end(), [&](const auto& line) {
        return line.provenance.SameScene(selected) &&
               line.provenance.frameSeq == selected.frameSeq &&
               line.provenance.timestampMs == selected.timestampMs &&
               line.validatedFrameSeq == selected.frameSeq &&
               model_.IsShown(line, now, selected.frameSeq);
    });
}

void UltraClearTextController::StopImmutableRefinement()
{
    if (uct::timing::Enabled() && immutableScene_ && scheduler_)
        uct::TraceTiming(uct::timing::Event::Cancel, scheduler_->TimingProducerId(), uct::timing::Channel::Recognition,
            ImmutableStamp(immutableScene_->selected), 0, scheduler_->Authority(),
            {3, 0, uct::timing::Cancelled | uct::timing::RetainedArtifact});
    // Retire optional work, not the accepted immutable model. This does not
    // renew any source/validation clock or clear backend/input failures.
    immutableRetryExhausted_ = true;
    immutableRetryScheduled_ = false;
    immutableSelectedAttempted_ = immutableCorroborationAttempted_ = true;
    immutableSelectedJob_ = immutableCorroborationJob_ = 0;
    immutableSelectedResult_.reset();
    immutableDetailFocus_.reset();
    if (scheduler_) scheduler_->Cancel(); // fence a queued/in-flight refinement
    if (immutableDetailTimer_) immutableDetailTimer_->stop();
}

void UltraClearTextController::ScheduleImmutableRetry(bool selected, bool preparing)
{
    if (immutableDetailFocus_) {
        FinishImmutableDetail();
        return;
    }
    if (!immutableScene_ || !enabled_ || immutableRetryScheduled_) return;
    if (preparing && immutableRetryStartedAtMs_ &&
        clock_() - *immutableRetryStartedAtMs_ > timing_.immutableJobTimeoutMs &&
        ((immutableSelectedResult_ && immutableSelectedResult_->output.ok) ||
         model_.AcceptedWordCount() > 0)) {
        if (HasProvenImmutableArtifact()) StopImmutableRefinement();
        return; // a healthy CPU fallback does not fail because GPU preparation is slow
    }
    if ((!preparing && ++immutableRetryCount_ > 3) ||
        (preparing && immutableRetryStartedAtMs_ &&
         clock_() - *immutableRetryStartedAtMs_ > timing_.immutableJobTimeoutMs)) {
        if (HasProvenImmutableArtifact()) StopImmutableRefinement();
        else {
            immutableRetryExhausted_ = true;
            model_.Reset();
        }
        RefreshOverlay(clock_());
        UpdateReadingText();
        return;
    }
    if (selected) {
        immutableSelectedAttempted_ = false;
        immutableSelectedResult_.reset();
        immutableCorroborationAttempted_ = false;
    } else immutableCorroborationAttempted_ = false;
    immutableRetryScheduled_ = true;
    const auto generation = geometryGeneration_;
    QTimer::singleShot(preparing ? 500 : 100, this, [this, generation] {
        // The old timer cannot alter a newer source, seek or settings ticket.
        if (generation != geometryGeneration_) return;
        immutableRetryScheduled_ = false;
        SubmitImmutableIfNeeded();
        UpdateStatus();
    });
}

void UltraClearTextController::HandleImmutableResult(uct::RecognitionResult result)
{
    QElapsedTimer handling;
    handling.start();
    const bool selected = immutableSelectedJob_ != 0 && result.jobId == immutableSelectedJob_;
    const bool neighbour = immutableCorroborationJob_ != 0 && result.jobId == immutableCorroborationJob_;
    const auto now = clock_();
    if (!enabled_ || !immutableScene_ || !scheduler_ || !scheduler_->IsCurrent(result) ||
        !result.stamp.SameScene(CurrentScene()) || (!selected && !neighbour) ||
        now - immutableSubmittedAtMs_ > timing_.immutableJobTimeoutMs ||
        (selected && result.stamp.frameSeq != immutableScene_->selected.frameSeq) ||
        (neighbour && (!immutableScene_->corroboration ||
                      result.stamp.frameSeq != immutableScene_->corroboration->frameSeq))) {
        TraceRecognition(uct::timing::Event::Fault, scheduler_.get(), result, {2, 0, uct::timing::Stale});
        if (selected) immutableSelectedJob_ = 0;
        if (neighbour) immutableCorroborationJob_ = 0;
        if ((selected || neighbour) && immutableScene_ && scheduler_ && scheduler_->IsCurrent(result) &&
            result.stamp.SameScene(CurrentScene())) ScheduleImmutableRetry(selected);
        ++rejectedResults_;
        UpdateStatus();
        return;
    }
    if (selected) immutableSelectedJob_ = 0;
    else immutableCorroborationJob_ = 0;
    if (result.output.inputFault && !result.output.cancelled) ++consecutiveInputFailures_;
    else if (result.output.ok) consecutiveInputFailures_ = 0;
    // A paused Extra Stable crossfade is not recognized; it is not a failure.
    if (!result.output.ok && !result.output.cancelled && !result.output.inputFault &&
        !result.output.backendPreparing && !result.output.compositeInput) {
        if (++consecutiveFailures_ >= kMaximumConsecutiveFailures) {
            backend_.state = uct::BackendState::kFailed;
            backend_.detail = result.output.error;
            TraceRecognition(uct::timing::Event::BackendState, scheduler_.get(), result,
                {static_cast<std::uint32_t>(backend_.state)});
            if (HasProvenImmutableArtifact()) StopImmutableRefinement();
            else Invalidate(QStringLiteral("failed"));
            RefreshOverlay(now);
            UpdateReadingText();
            UpdateStatus();
            return;
        }
    } else if (result.output.ok) {
        consecutiveFailures_ = 0;
    }

    if (!result.output.ok || result.output.cancelled) {
        if (!AnalysisHealthy() && HasProvenImmutableArtifact()) {
            StopImmutableRefinement();
            RefreshOverlay(now);
            UpdateReadingText();
        } else ScheduleImmutableRetry(selected, result.output.backendPreparing);
        UpdateStatus();
        return;
    }

    const bool retainProvenModel = !model_.SceneWords(now, currentFrameSeq_).isEmpty();
    const auto previousModel = retainProvenModel ? std::optional<uct::OverlayModel>(model_) : std::nullopt;
    uct::AdmitVerdict verdict;
    if (selected) {
        // Preserve this selected result's job/authority/frame ticket. A
        // neighbour may corroborate it but never becomes selected evidence.
        immutableSelectedResult_ = std::move(result);
        if (retainProvenModel) {
            // Keep the proven same-frame artifact until a complete refinement
            // pair is available. A selected-only candidate cannot erase it.
            SubmitImmutableIfNeeded();
            if (!immutableCorroborationJob_ && immutableSelectedResult_->output.backendPreparing)
                ScheduleImmutableRetry(true, true);
            UpdateStatus();
            return;
        }
        verdict = model_.AdmitImmutable(*immutableSelectedResult_,
                                        ImmutableStamp(immutableScene_->selected), now);
    } else {
        if (!immutableSelectedResult_) { ++rejectedResults_; return; }
        verdict = model_.AdmitImmutable(*immutableSelectedResult_,
                                        ImmutableStamp(immutableScene_->selected), now, &result);
    }
    if (previousModel && verdict != uct::AdmitVerdict::kAccepted &&
        verdict != uct::AdmitVerdict::kContentMoved)
        model_ = *previousModel; // a refused refinement cannot erase prior proof
    if (immutableSelectedResult_) NoteTimingAdmission(*immutableSelectedResult_, verdict,
        previousModel ? uct::timing::RetainedArtifact : 0u);
    if (uct::timing::Enabled() && immutableSelectedResult_) TraceRecognition(uct::timing::Event::Admit, scheduler_.get(),
        *immutableSelectedResult_, {static_cast<std::uint32_t>(verdict),
            static_cast<std::uint32_t>(model_.AcceptedWordCount()),
            previousModel ? uct::timing::RetainedArtifact : 0u, selected ? 0 : result.jobId});
    switch (verdict) {
    case uct::AdmitVerdict::kAccepted:
        ++acceptedResults_;
        lastResultMoved_ = false;
        lastResultEmpty_ = false;
        break;
    case uct::AdmitVerdict::kPending:
        lastResultMoved_ = false;
        lastResultEmpty_ = false;
        break;
    case uct::AdmitVerdict::kEmpty:
        lastResultEmpty_ = true;
        lastResultMoved_ = false;
        break;
    case uct::AdmitVerdict::kContentMoved:
        // A complete independent pair contradicted existing selected-frame
        // text. The model has revoked the affected proof; never restore it.
        lastResultMoved_ = true;
        lastResultEmpty_ = false;
        break;
    default:
        ++rejectedResults_;
        break;
    }
    if (selected) SubmitImmutableIfNeeded();
    if (neighbour && immutableDetailFocus_) FinishImmutableDetail();
    if (immutableSelectedResult_ && immutableSelectedResult_->output.backendPreparing &&
        immutableCorroborationJob_ == 0)
        ScheduleImmutableRetry(true, true); // refine the completed fallback only after its neighbour
    RefreshOverlay(now);
    UpdateReadingText();
    UpdateStatus();
    lastResultMs_ = handling.nsecsElapsed() / 1e6;
    emit recognitionAdmitted(static_cast<int>(verdict));
    if (!immutableSelectedJob_ && !immutableCorroborationJob_) ScheduleImmutableDetail();
}

void UltraClearTextController::ScheduleImmutableDetail()
{
    if (!immutableScene_ || !enabled_ || !immutableDetailTimer_) return;
    // Restarted by every view change: only a settled view is re-detected.
    immutableDetailTimer_->start(static_cast<int>(std::clamp<std::int64_t>(
        timing_.immutableDetailSettleMs, 0, 60000)));
}

std::optional<uct::DetailFocus> UltraClearTextController::ImmutableDetailFocus() const
{
    if (!immutableScene_) return std::nullopt;
    const auto& transform = immutableScene_->transform;
    const QSize scene(static_cast<int>(geometry_.sceneWidth), static_cast<int>(geometry_.sceneHeight));
    if (!transform.valid || scene.isEmpty()) return std::nullopt;
    // The current view's source region, in the exact pixels of the retained
    // full-scene analysis image (the same coordinates its OCR boxes use).
    const QRectF visible = uct::ViewRectToScene(transform, 1.0, 1.0, QRectF(0.0, 0.0, 1.0, 1.0))
                               .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (visible.isEmpty()) return std::nullopt;
    const QRect region = QRectF(visible.x() * scene.width(), visible.y() * scene.height(),
                                visible.width() * scene.width(), visible.height() * scene.height())
                             .toAlignedRect().intersected(QRect(QPoint(), scene));
    const uct::DetailFocus focus{region, timing_.immutableDetailMagnification};
    const QRect detail = uct::DetailRegion(scene, focus);
    if (detail.isEmpty()) return std::nullopt;
    // Re-detection is worth one bounded pass only when it gives the detector
    // clearly more pixels per glyph than the established full-scene passes.
    const double detailScale = double(uct::DetectorInputSize(detail.size(), uct::DetailMagnification(focus)).width()) /
                               detail.width();
    if (detailScale < 1.25 * uct::EstablishedDetectorScale(scene, QRectF(detail).center())) return std::nullopt;
    return focus;
}

void UltraClearTextController::SubmitImmutableDetailIfUseful()
{
    if (!enabled_ || !immutableScene_ || !scheduler_ || mediaOverlaySuppressed_ || presentationFailed_ ||
        backend_.state != uct::BackendState::kReady || !backend_.detailFocus || !AnalysisHealthy() ||
        immutableRetryScheduled_ || immutableRetryExhausted_ || immutableDetailFocus_ ||
        immutableSelectedJob_ || immutableCorroborationJob_ ||
        !immutableSelectedAttempted_ || !immutableCorroborationAttempted_ ||
        !immutableSelectedResult_ || !immutableSelectedResult_->output.ok ||
        immutableSelectedResult_->output.backendPreparing || immutableDetailDone_.size() >= 32)
        return;
    // New glyphs need a genuinely distinct real neighbour. Re-detecting the
    // held image alone cannot confirm anything it finds.
    if (!immutableScene_->corroboration || !CorroborationInputValid(*immutableScene_, *immutableScene_->corroboration))
        return;
    const auto focus = ImmutableDetailFocus();
    if (!focus) return;
    const QSize scene(static_cast<int>(geometry_.sceneWidth), static_cast<int>(geometry_.sceneHeight));
    const QRect region = uct::DetailRegion(scene, *focus);
    // A region already examined at (almost) this detector scale is covered;
    // zooming further in may earn one closer look, never beyond the bound.
    const double magnification = double(uct::DetectorInputSize(region.size(), uct::DetailMagnification(*focus)).width()) /
                                 region.width();
    for (const auto& done : immutableDetailDone_)
        if (done.region.contains(region) && done.magnification >= magnification * 0.9) return;
    immutableDetailFocus_ = focus;
    immutableSelectedAttempted_ = immutableCorroborationAttempted_ = false;
    ++immutableDetailPasses_;
    SubmitImmutableIfNeeded();
    UpdateStatus();
}

void UltraClearTextController::FinishImmutableDetail()
{
    if (!immutableDetailFocus_) return;
    const QSize scene(static_cast<int>(geometry_.sceneWidth), static_cast<int>(geometry_.sceneHeight));
    // Record completed and abandoned regions alike: a failing region is not
    // retried in a loop, and the record is bounded for this selection only.
    const QRect region = uct::DetailRegion(scene, *immutableDetailFocus_);
    immutableDetailDone_.push_back({region, region.isEmpty() ? 0.0 :
        double(uct::DetectorInputSize(region.size(), uct::DetailMagnification(*immutableDetailFocus_)).width()) /
            region.width()});
    // Never evict coverage and turn navigation into repeated work. At most
    // 32 pairs are attempted for this selection; a new selection resets it.
    immutableDetailFocus_.reset();
    immutableSelectedAttempted_ = immutableCorroborationAttempted_ = true;
    immutableSelectedJob_ = immutableCorroborationJob_ = 0;
    immutableRetryScheduled_ = false;
    // Navigation can settle while this pair is running. Reconsider only the
    // latest view after completion/failure, including the early error paths.
    ScheduleImmutableDetail();
}

void UltraClearTextController::NoteTimingAdmission(const uct::RecognitionResult& result,
                                                   uct::AdmitVerdict verdict, std::uint32_t flags)
{
    if (!uct::timing::Enabled()) return;
    if (!model_.HasAcceptedText()) {
        timingAdmission_.reset();
        return;
    }
    if (verdict != uct::AdmitVerdict::kAccepted && verdict != uct::AdmitVerdict::kContentMoved &&
        !(immutableScene_ && verdict == uct::AdmitVerdict::kPending)) return;
    timingAdmission_ = uct::TimingKey(scheduler_->TimingProducerId(), uct::timing::Channel::Recognition,
                                    result.stamp, result.jobId, result.authority);
    timingAdmissionFlags_ = flags | (result.deviceInput ? uct::timing::DeviceInput : 0u) |
        (result.stamp.liveCameraCapture ? uct::timing::LiveCamera : 0u) |
        (uct::EvidenceAcquisition(result.stamp) ? uct::timing::AcquisitionKnown : 0u) |
        (verdict == uct::AdmitVerdict::kContentMoved ? uct::timing::RetainedArtifact : 0u);
}

void UltraClearTextController::UpdatePositionedTimingPublication(bool changed, bool positioned, bool clear)
{
    if (!uct::timing::Enabled()) return;
    if (changed) {
        ++timingPublicationRevision_;
        timingPositionedPublication_ = timingAdmission_ && !clear
            ? uct::timing::PublicationTicket{*timingAdmission_, timingPublicationRevision_, timingAdmissionFlags_}
            : uct::timing::PublicationTicket{};
    }
    if (clear) timingPositionedPublication_ = {};
    if (!positioned) timingPositionedPublication_ = {};
    // Only a positioned snapshot change/revocation reaches these consumers.
    // A later pane-only update must leave their ticket and painted revision.
    if (overlay_) overlay_->SetTimingTicket(timingPositionedPublication_);
    if (screenText_) screenText_->SetTimingTicket(timingPositionedPublication_);
}

void UltraClearTextController::UpdatePaneTimingPublication(bool sharePositionedRevision, bool clear)
{
    if (!uct::timing::Enabled()) return;
    if (clear) timingPanePublication_ = {};
    else {
        const auto revision = sharePositionedRevision ? timingPublicationRevision_ : ++timingPublicationRevision_;
        timingPanePublication_ = timingAdmission_
            ? uct::timing::PublicationTicket{*timingAdmission_, revision, timingAdmissionFlags_}
            : uct::timing::PublicationTicket{};
    }
    if (panel_) panel_->SetTimingTicket(timingPanePublication_);
}

void UltraClearTextController::RefreshOverlay(std::int64_t nowMs)
{
    if (!overlay_) {
        return;
    }
    const bool provenImmutable = HasProvenImmutableArtifact();
    if (!enabled_ || (!provenImmutable && (!AnalysisHealthy() || !BackendUsable(backend_))) ||
        showOriginal_ || compositePaused_ || mediaOverlaySuppressed_ || settling_ || !PresentationAvailable(nowMs) ||
        !AdvancingValidationFresh(nowMs)) {
        const bool hadWords = !overlayWords_.isEmpty();
        UpdatePositionedTimingPublication(hadWords, false);
        overlay_->ClearWords();
        overlayWords_.clear();
        // Nothing is drawn, so nothing is exposed: an empty root. Trivial
        // when already empty, as this branch runs on every presentation.
        if (screenText_) screenText_->RevokeAll();
        shownWords_ = 0;
        UpdateReadingText(hadWords);
        return;
    }
    const QVector<uct::SceneWord> words = model_.SceneWords(ModelTime(nowMs), currentFrameSeq_);
    QVector<UltraClearTextOverlay::SceneWord> next;
    next.reserve(words.size());
    for (const uct::SceneWord& word : words) {
        next.push_back({word.text, word.sceneRect, word.colors, word.lineId, word.protectedSceneRects,
                        word.wordIndex, word.sceneQuad, word.layoutGroupId});
    }
    // Text, position and colors all decide what is drawn; a re-recognized
    // word in a new color scheme must not keep its old plate.
    bool same = next.size() == overlayWords_.size();
    for (int i = 0; same && i < next.size(); ++i) {
        same = next[i].text == overlayWords_[i].text &&
               next[i].sceneRect == overlayWords_[i].sceneRect &&
               next[i].lineId == overlayWords_[i].lineId &&
               next[i].layoutGroupId == overlayWords_[i].layoutGroupId &&
               next[i].wordIndex == overlayWords_[i].wordIndex &&
               next[i].protectedSceneRects == overlayWords_[i].protectedSceneRects &&
               next[i].sceneQuad == overlayWords_[i].sceneQuad &&
               SameColors(next[i].colors, overlayWords_[i].colors);
    }
    if (!same) {
        overlayWords_ = next;
        UpdatePositionedTimingPublication(true, !next.isEmpty());
        if (next.isEmpty()) {
            overlay_->ClearWords();
        } else {
            overlay_->SetWords(std::move(next));
        }
        // Removals apply at once; additions wait for the semantic cadence.
        // Pan/zoom never reaches here: geometry is answered from paint.
        if (screenText_) screenText_->ContentChanged();
        UpdateReadingText(true);
    }
    shownWords_ = overlayWords_.size();
}

UctScreenTextSnapshot UltraClearTextController::ScreenTextSnapshot() const
{
    UctScreenTextSnapshot snapshot;
    snapshot.enabled = enabled_;
    // overlayWords_ is non-empty only while every RefreshOverlay gate holds
    // (including presentation/source revocation). A failed optional
    // refinement may leave exact paused proof drawn, never new evidence.
    if (!enabled_ || overlayWords_.isEmpty()) {
        return snapshot;
    }
    QHash<std::pair<std::uint64_t, int>, QString> drawn;
    drawn.reserve(overlayWords_.size());
    for (const UltraClearTextOverlay::SceneWord& word : overlayWords_) {
        drawn.insert({word.lineId, word.wordIndex}, word.text);
    }
    for (uct::ScreenLine& line : model_.ScreenLines(ModelTime(clock_()), currentFrameSeq_)) {
        const bool allDrawn = std::all_of(line.tokens.begin(), line.tokens.end(), [&](const uct::ScreenToken& token) {
            if (token.wordIndex < 0) return true;
            const auto it = drawn.constFind({line.lineId, token.wordIndex});
            return it != drawn.constEnd() && it.value() == line.text.mid(token.start, token.length);
        });
        if (allDrawn) snapshot.lines.push_back(std::move(line));
    }
    snapshot.languageTag = backend_.languageTag;
    return snapshot;
}

void UltraClearTextController::UpdateReadingText(bool shareTimingRevision)
{
    // The pane offers exactly the lines verified on screen now (also while
    // Show original hides the glyph plates).
    QString text;
    if (enabled_ && (HasProvenImmutableArtifact() || (AnalysisHealthy() && BackendUsable(backend_))) &&
        PresentationAvailable(clock_()) && AdvancingValidationFresh(clock_()) &&
        !compositePaused_ && !mediaOverlaySuppressed_ && !settling_) {
        text = model_.ReadingText(ModelTime(clock_()), currentFrameSeq_);
    }
    if (text == readingText_) {
        return;
    }
    UpdatePaneTimingPublication(shareTimingRevision, text.isEmpty());
    readingText_ = text;
    if (panel_) {
        panel_->SetReadingText(readingText_);
    }
}

void UltraClearTextController::SetStatus(const QString& source, bool important)
{
    statusSource_ = source;
    if (panel_) {
        panel_->SetStatus(source, important);
    }
}

bool UltraClearTextController::AnalysisHealthy() const
{
    return !analysisPoolFailed_ && consecutiveInputFailures_ < kMaximumConsecutiveFailures &&
           consecutiveProbeFailures_ < kMaximumConsecutiveFailures;
}

void UltraClearTextController::UpdateStatus()
{
    if (panel_) panel_->SetBackendStatus(backend_);
    if (!enabled_) {
        SetStatus(QStringLiteral("Ultra Clear Text is off."));
        return;
    }
    // The backend panel above still reports the actual terminal status.
    // Avoid claiming the original image is shown while accepted paused
    // glyphs remain drawn; reuse the ordinary visible-word count.
    if (shownWords_ > 0 && HasProvenImmutableArtifact()) {
        SetStatus(QStringLiteral("Showing sharp text for %1 words.").arg(shownWords_));
        return;
    }
    switch (backend_.state) {
    case uct::BackendState::kNotStarted:
        SetStatus(QStringLiteral("Starting text recognition…"));
        return;
    case uct::BackendState::kUnavailable:
        SetStatus(backend_.detail.contains(QStringLiteral("language"))
                      ? QStringLiteral("Text recognition is unavailable: Windows has no OCR language installed. "
                                       "The original camera image is shown.")
                      : QStringLiteral("Text recognition is unavailable on this computer. "
                                       "The original camera image is shown."),
                  true);
        return;
    case uct::BackendState::kFailed:
        SetStatus(QStringLiteral("Text recognition stopped working. The original camera image is shown."),
                  true);
        return;
    case uct::BackendState::kReady:
        break;
    }
    if (mediaOverlaySuppressed_) {
        if (mediaSuppressionExtraStable_) {
            SetStatus(QStringLiteral("Paused while Extra Stable is on. The original camera image is shown."),
                      true);
        } else {
            SetStatus(QStringLiteral("Original camera text shown."));
        }
        return;
    }
    if (!AnalysisHealthy()) {
        SetStatus(QStringLiteral("Text recognition stopped working. The original camera image is shown."), true);
        return;
    }
    if (immutableRetryExhausted_) {
        SetStatus(QStringLiteral("Text recognition stopped working. The original camera image is shown."), true);
        return;
    }
    if (compositePaused_) {
        SetStatus(compositeExtraStable_
                      ? QStringLiteral("Paused while Extra Stable is on. The original camera image is shown.")
                      : QStringLiteral("Text recognition is paused in the debug composite view. The original camera image is shown."),
                  true);
    } else if (!cameraActive_ && !HasImmutableScene()) {
        SetStatus(QStringLiteral("Waiting for the camera."));
    } else if (settling_) {
        SetStatus(QStringLiteral("Image settings changed. Showing the camera image until the new image is read."));
    } else if (showOriginal_) {
        SetStatus(QStringLiteral("Original camera text shown."));
    } else if (shownWords_ > 0) {
        SetStatus(QStringLiteral("Showing sharp text for %1 words.").arg(shownWords_));
    } else if (lastResultMoved_) {
        SetStatus(QStringLiteral("Text moved. Showing the original until it is steady again."));
    } else if (scheduler_ && scheduler_->InFlight()) {
        SetStatus(QStringLiteral("Reading text…"));
    } else if (lastResultEmpty_) {
        SetStatus(QStringLiteral("No steady printed text recognized yet."));
    } else {
        SetStatus(QStringLiteral("Waiting for steady text…"));
    }
}

} // namespace okuflow

#endif // _WIN32
