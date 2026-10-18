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

double Area(const QRectF& rect)
{
    return rect.isEmpty() ? 0.0 : rect.width() * rect.height();
}

bool SameColors(const uct::LineColors& a, const uct::LineColors& b)
{
    return a.foreground == b.foreground && a.background == b.background;
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
        if (enabled_ && !HasImmutableScene() && presentationAvailable_ && !PresentationAvailable(clock_())) {
            presentationAvailable_ = false;
            RefreshOverlay(clock_());
            UpdateReadingText();
            UpdateStatus();
        }
    });
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
                    if(!self||!self->enabled_||self->HasImmutableScene()||self->settling_||self->compositePaused_||
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
        timing_.probeDecimation, uct::ModelTuning{}.minimumWordHeightPx);
}

void UltraClearTextController::SetEnabled(bool enabled)
{
    if (panel_ && panel_->enableCheckbox()->isChecked() != enabled) {
        const QSignalBlocker blocker(panel_->enableCheckbox());
        panel_->enableCheckbox()->setChecked(enabled);
    }
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
        if (!HasImmutableScene()) watchdog_->start();
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
            const auto& pixels = immutableScene_->selected.pixels;
            analysisPoolFailed_ = !pixels || !pixels->IsValid() ||
                pixels->width != static_cast<int>(geometry_.sceneWidth) ||
                pixels->height != static_cast<int>(geometry_.sceneHeight);
            SubmitImmutableIfNeeded();
            RefreshOverlay(clock_());
        } else if (advancingMediaScene_) {
            ++geometryGeneration_;
            geometry_ = advancingMediaScene_->geometry;
            haveGeometry_ = true;
            currentFrameSeq_ = advancingMediaScene_->current.frameSeq;
            const auto& pixels = advancingMediaScene_->current.pixels;
            analysisPoolFailed_ = !pixels || !pixels->IsValid() ||
                pixels->width != static_cast<int>(geometry_.sceneWidth) ||
                pixels->height != static_cast<int>(geometry_.sceneHeight);
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

bool UltraClearTextController::SetImmutableScene(UctImmutableScene scene)
{
    const auto sameFrame = [](const UctImmutableFrame& a, const UctImmutableFrame& b) {
        return a.pixels == b.pixels && a.frameSeq == b.frameSeq && a.timestampMs == b.timestampMs;
    };
    const bool sameNeighbour = immutableScene_ &&
        immutableScene_->corroboration.has_value() == scene.corroboration.has_value() &&
        (!scene.corroboration || sameFrame(*immutableScene_->corroboration, *scene.corroboration));
    if (immutableScene_ && immutableScene_->geometry == scene.geometry &&
        immutableScene_->sourceAuthority == scene.sourceAuthority &&
        sameFrame(immutableScene_->selected, scene.selected) && sameNeighbour) {
        UpdateImmutableView(scene.transform, scene.viewportWidth, scene.viewportHeight, scene.strokeWeight);
        OnMediaPresentationSucceeded(); // this API carries a successful selected-frame receipt
        return scene.selected.pixels && scene.selected.pixels->IsValid() &&
            scene.selected.pixels->width == static_cast<int>(scene.geometry.sceneWidth) &&
            scene.selected.pixels->height == static_cast<int>(scene.geometry.sceneHeight);
    }

    ++geometryGeneration_;
    Invalidate(QStringLiteral("scene"));
    advancingMediaScene_.reset();
    immutableScene_ = std::move(scene);
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
    const auto& pixels = immutableScene_->selected.pixels;
    const bool valid = pixels && pixels->IsValid() &&
        pixels->width == static_cast<int>(geometry_.sceneWidth) &&
        pixels->height == static_cast<int>(geometry_.sceneHeight);
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
    const bool discontinuity = !advancingMediaScene_ ||
        advancingMediaScene_->geometry != scene.geometry ||
        advancingMediaScene_->sourceAuthority != scene.sourceAuthority ||
        scene.current.frameSeq < advancingMediaScene_->current.frameSeq ||
        scene.current.timestampMs < advancingMediaScene_->current.timestampMs ||
        (scene.current.frameSeq == advancingMediaScene_->current.frameSeq &&
         (scene.current.timestampMs != advancingMediaScene_->current.timestampMs ||
          scene.current.pixels != advancingMediaScene_->current.pixels));
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
    geometry_ = advancingMediaScene_->geometry;
    haveGeometry_ = true;
    cameraActive_ = false;
    compositePaused_ = false;
    settling_ = false;
    presentationAvailable_ = false;
    presentationFailed_ = false;
    currentFrameSeq_ = advancingMediaScene_->current.frameSeq;
    const auto& pixels = advancingMediaScene_->current.pixels;
    const bool valid = pixels && pixels->IsValid() &&
        pixels->width == static_cast<int>(geometry_.sceneWidth) &&
        pixels->height == static_cast<int>(geometry_.sceneHeight);
    analysisPoolFailed_ = !valid;
    watchdog_->stop(); // a media receipt is not a live camera heartbeat
    UpdateImmutableView(advancingMediaScene_->transform, advancingMediaScene_->viewportWidth,
                        advancingMediaScene_->viewportHeight, advancingMediaScene_->strokeWeight);
    if (enabled_) {
        EnsureScheduler();
        SubmitAdvancingMediaProbe();
    }
    return valid;
}

void UltraClearTextController::SubmitAdvancingMediaProbe()
{
    if (!enabled_ || !advancingMediaScene_ || !probeWorker_ || !AnalysisHealthy() ||
        !BackendUsable(backend_) || probeWorker_->HasPending()) return;
    const auto now = clock_();
    const auto& current = advancingMediaScene_->current;
    if ((advancingMediaHaveProbe_ && advancingMediaProbeFrame_ == current.frameSeq) ||
        now - lastProbeRequestMs_ < timing_.probeIntervalMs) return;
    lastProbeRequestMs_ = now;
    advancingMediaProbeFrame_ = current.frameSeq;
    advancingMediaHaveProbe_ = true;
    SubmitProbe(ImmutableStamp(current), current.pixels);
}

std::int64_t UltraClearTextController::ModelTime(std::int64_t wallNowMs) const
{
    return advancingMediaScene_ ? advancingMediaScene_->current.timestampMs : wallNowMs;
}

void UltraClearTextController::UpdateImmutableView(const ViewTransform& transform,
                                                   std::uint32_t viewportWidth,
                                                   std::uint32_t viewportHeight, int strokeWeight)
{
    if (!HasImmutableScene()) return;
    if (immutableScene_) {
        immutableScene_->transform = transform;
        immutableScene_->viewportWidth = viewportWidth;
        immutableScene_->viewportHeight = viewportHeight;
        immutableScene_->strokeWeight = strokeWeight;
    } else {
        advancingMediaScene_->transform = transform;
        advancingMediaScene_->viewportWidth = viewportWidth;
        advancingMediaScene_->viewportHeight = viewportHeight;
        advancingMediaScene_->strokeWeight = strokeWeight;
    }
    if (overlay_) {
        overlay_->SetViewTransform(transform);
        overlay_->SetStrokeWeight(strokeWeight);
    }
    // The artifact covers the complete scene. View changes are projection
    // only, and never renew source timestamps or enqueue OCR/probes.
    RefreshOverlay(clock_());
    UpdateReadingText();
    UpdateStatus();
}

void UltraClearTextController::ClearImmutableScene()
{
    if (!HasImmutableScene()) return;
    ++geometryGeneration_;
    Invalidate(QStringLiteral("scene"));
    immutableScene_.reset();
    advancingMediaScene_.reset();
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
    presentationFailed_ = false;
    RefreshOverlay(clock_());
    UpdateReadingText();
    UpdateStatus();
}

void UltraClearTextController::OnMediaPresentationFailed()
{
    if (!HasImmutableScene()) return;
    // Retain the scene/model so a successful exact-frame receipt can restore
    // it, while immediately revoking every currently visible text surface.
    // Keep the latch even while disabled or awaiting backend readiness.
    presentationFailed_ = true;
    if (overlay_) overlay_->ClearWords();
    if (screenText_) screenText_->RevokeAll();
    overlayWords_.clear();
    shownWords_ = 0;
    UpdateReadingText();
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
    if (context.compositeFrames != compositePaused_) {
        compositePaused_ = context.compositeFrames;
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
    if (settling_ || compositePaused_ || !cameraActive_ || pendingReadbackId_ != 0 ||
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
    if (advancingMediaScene_) {
        return advancingMediaScene_->current.pixels && advancingMediaScene_->current.pixels->IsValid() &&
            advancingMediaScene_->transform.valid && advancingMediaScene_->viewportWidth > 0 &&
            advancingMediaScene_->viewportHeight > 0;
    }
    if (immutableScene_) {
        return immutableScene_->selected.pixels && immutableScene_->selected.pixels->IsValid() &&
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
    if (overlay_) {
        overlay_->ClearWords();
    }
    if (screenText_) screenText_->RevokeAll();
    overlayWords_.clear();
    shownWords_ = 0;
    UpdateReadingText();
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
    if (!enabled_ || settling_ || compositePaused_ || !scheduler_ || !device) return;
    const auto now=clock_();
    if (device->sourceEpoch!=context.geometry.settings.sourceEpoch || device->timestampMs>now ||
        now-device->timestampMs>1000 || device->frameSequence!=context.frameSeq) return;
    auto stamp=MakeStamp(context,device->timestampMs);
    stamp.frameSeq=device->frameSequence;
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
    if (scheduler_) {
        scheduler_->Cancel(); // in-flight input stays owned by the worker
    }
    if (probeWorker_) {
        probeWorker_->Cancel();
    }
    if(deviceProbeReader_)deviceProbeReader_->Cancel();
    model_.Reset();
    immutableSelectedResult_.reset();
    immutableSelectedJob_ = 0;
    immutableCorroborationJob_ = 0;
    immutableSelectedAttempted_ = false;
    immutableCorroborationAttempted_ = false;
    advancingMediaJob_ = 0;
    advancingMediaLastSubmitMs_ = std::numeric_limits<std::int64_t>::min() / 2;
    advancingMediaProbeFrame_ = 0;
    advancingMediaHaveProbe_ = false;
    latestProbe_.reset();
    deviceCandidate_.reset();
    stableProbes_ = 0;
    changedSinceSubmit_ = true;
    lastJobCoverage_ = {};
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
    if (immutableScene_ || !pixels || !probeWorker_ || settling_ || compositePaused_ || !stamp.SameScene(CurrentScene())) {
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
    QElapsedTimer handling;
    handling.start();
    if (!enabled_ || immutableScene_ || !probeWorker_ || !probeWorker_->IsCurrent(outcome) || settling_ || compositePaused_ ||
        !outcome.stamp.SameScene(CurrentScene()) || !outcome.probe ||
        (latestProbe_ && outcome.stamp.frameSeq<latestProbe_->stamp.frameSeq)) {
        return;
    }
    const std::int64_t now = clock_();
    const uct::ValidationSummary summary = model_.ApplyChecks(outcome.checks, outcome.stamp);
    if (summary.invalid > 0 && summary.valid == 0) {
        lastResultMoved_ = true;
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
        const bool needed = model_.PendingWordCount() > 0 || !model_.HasAcceptedText() || !covered ||
                            now - (advancingMediaScene_ ? advancingMediaLastSubmitMs_ : lastSubmitMs_) >=
                                timing_.refreshIntervalMs;
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
    if (immutableScene_) {
        HandleImmutableResult(std::move(result));
        return;
    }
    QElapsedTimer handling;
    handling.start();
    if (!enabled_ || !scheduler_ || !scheduler_->IsCurrent(result) || settling_ || compositePaused_ ||
        !result.stamp.SameScene(CurrentScene())) {
        ++rejectedResults_;
        return;
    }
    const std::int64_t now = clock_();
    if (advancingMediaScene_) {
        if (result.jobId != advancingMediaJob_ || advancingMediaJob_ == 0 ||
            now - advancingMediaSubmittedAtMs_ > timing_.immutableJobTimeoutMs) {
            if (result.jobId == advancingMediaJob_) advancingMediaJob_ = 0;
            ++rejectedResults_;
            return;
        }
        advancingMediaJob_ = 0;
    }
    // Admit compares recognized pixels with the newest independent scene probe.
    // The admission probe for this exact device frame is not a motion test.
    if (result.output.inputFault && !result.output.cancelled) ++consecutiveInputFailures_;
    else if (result.output.ok) consecutiveInputFailures_ = 0;
    if (!result.output.ok && !result.output.cancelled && !result.output.inputFault && !result.output.backendPreparing) {
        if (++consecutiveFailures_ >= kMaximumConsecutiveFailures) {
            backend_.state = uct::BackendState::kFailed;
            backend_.detail = result.output.error;
            Invalidate(QStringLiteral("failed"));
        }
    } else if (result.output.ok) {
        consecutiveFailures_ = 0;
    }
    const uct::AdmitVerdict verdict = advancingMediaScene_
        ? model_.AdmitAdvancingMedia(result, CurrentScene(), latestProbe_.get(), ModelTime(now))
        : model_.Admit(result, CurrentScene(), latestProbe_.get(), now);
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
    RefreshOverlay(now);
    UpdateReadingText();
    UpdateStatus();
    lastResultMs_ = handling.nsecsElapsed() / 1e6;
    emit recognitionAdmitted(static_cast<int>(verdict));
}

void UltraClearTextController::HandleBackendStatus(uct::BackendStatus status)
{
    backend_ = std::move(status);
    if (!BackendUsable(backend_)) {
        Invalidate(QStringLiteral("unavailable"));
    }
    if (screenText_) screenText_->ContentChanged(); // recognizer language may differ
    // Backend readiness is an event, not a reason to poll every presentation
    // or timer tick. A waiting selected frame is submitted exactly once.
    if (immutableScene_ && backend_.state == uct::BackendState::kReady) {
        SubmitImmutableIfNeeded();
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
    if (frame.pixels) {
        stamp.viewportWidth = static_cast<std::uint32_t>(frame.pixels->width);
        stamp.viewportHeight = static_cast<std::uint32_t>(frame.pixels->height);
    }
    return stamp;
}

void UltraClearTextController::SubmitImmutableIfNeeded()
{
    if (!enabled_ || !immutableScene_ || !scheduler_ || backend_.state != uct::BackendState::kReady ||
        !AnalysisHealthy()) return;
    if (!immutableSelectedAttempted_) {
        immutableSelectedAttempted_ = true;
        immutableSubmittedAtMs_ = clock_();
        immutableSelectedJob_ = scheduler_->Submit(ImmutableStamp(immutableScene_->selected),
                                                   immutableScene_->selected.pixels);
        return;
    }
    if (!immutableSelectedResult_ || !immutableSelectedResult_->output.ok ||
        immutableCorroborationAttempted_) return;
    // Bound corroboration to one actual neighbouring image. A separate
    // buffer with identical pixels still fails the worker digest comparison
    // in AdmitImmutable; a reused lease is rejected before scheduling.
    immutableCorroborationAttempted_ = true;
    if (!immutableScene_->corroboration) return;
    const auto& neighbour = *immutableScene_->corroboration;
    if (!neighbour.pixels || !neighbour.pixels->IsValid() ||
        neighbour.frameSeq == immutableScene_->selected.frameSeq ||
        neighbour.pixels == immutableScene_->selected.pixels ||
        neighbour.pixels->width != static_cast<int>(geometry_.sceneWidth) ||
        neighbour.pixels->height != static_cast<int>(geometry_.sceneHeight)) return;
    immutableSubmittedAtMs_ = clock_();
    immutableCorroborationJob_ = scheduler_->Submit(ImmutableStamp(neighbour), neighbour.pixels);
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
        if (selected) immutableSelectedJob_ = 0;
        if (neighbour) immutableCorroborationJob_ = 0;
        ++rejectedResults_;
        UpdateStatus();
        return;
    }
    if (selected) immutableSelectedJob_ = 0;
    else immutableCorroborationJob_ = 0;
    if (result.output.inputFault && !result.output.cancelled) ++consecutiveInputFailures_;
    else if (result.output.ok) consecutiveInputFailures_ = 0;
    if (!result.output.ok && !result.output.cancelled && !result.output.inputFault &&
        !result.output.backendPreparing) {
        if (++consecutiveFailures_ >= kMaximumConsecutiveFailures) {
            backend_.state = uct::BackendState::kFailed;
            backend_.detail = result.output.error;
            Invalidate(QStringLiteral("failed"));
            UpdateStatus();
            return;
        }
    } else if (result.output.ok) {
        consecutiveFailures_ = 0;
    }

    uct::AdmitVerdict verdict;
    if (selected) {
        // Preserve this selected result's job/authority/frame ticket. A
        // neighbour may corroborate it but never becomes selected evidence.
        immutableSelectedResult_ = std::move(result);
        verdict = model_.AdmitImmutable(*immutableSelectedResult_,
                                        ImmutableStamp(immutableScene_->selected), now);
    } else {
        if (!immutableSelectedResult_) { ++rejectedResults_; return; }
        verdict = model_.AdmitImmutable(*immutableSelectedResult_,
                                        ImmutableStamp(immutableScene_->selected), now, &result);
    }
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
    default:
        ++rejectedResults_;
        break;
    }
    if (selected) SubmitImmutableIfNeeded();
    RefreshOverlay(now);
    UpdateReadingText();
    UpdateStatus();
    lastResultMs_ = handling.nsecsElapsed() / 1e6;
    emit recognitionAdmitted(static_cast<int>(verdict));
}

void UltraClearTextController::RefreshOverlay(std::int64_t nowMs)
{
    if (!overlay_) {
        return;
    }
    if (!enabled_ || !AnalysisHealthy() || showOriginal_ || compositePaused_ || settling_ || !PresentationAvailable(nowMs) ||
        !BackendUsable(backend_)) {
        overlay_->ClearWords();
        overlayWords_.clear();
        // Nothing is drawn, so nothing is exposed: an empty root. Trivial
        // when already empty, as this branch runs on every presentation.
        if (screenText_) screenText_->RevokeAll();
        shownWords_ = 0;
        UpdateReadingText();
        return;
    }
    const QVector<uct::SceneWord> words = model_.SceneWords(ModelTime(nowMs), currentFrameSeq_);
    QVector<UltraClearTextOverlay::SceneWord> next;
    next.reserve(words.size());
    for (const uct::SceneWord& word : words) {
        next.push_back({word.text, word.sceneRect, word.colors, word.lineId, word.protectedSceneRects,
                        word.wordIndex});
    }
    // Text, position and colors all decide what is drawn; a re-recognized
    // word in a new color scheme must not keep its old plate.
    bool same = next.size() == overlayWords_.size();
    for (int i = 0; same && i < next.size(); ++i) {
        same = next[i].text == overlayWords_[i].text &&
               next[i].sceneRect == overlayWords_[i].sceneRect &&
               next[i].lineId == overlayWords_[i].lineId &&
               next[i].wordIndex == overlayWords_[i].wordIndex &&
               next[i].protectedSceneRects == overlayWords_[i].protectedSceneRects &&
               SameColors(next[i].colors, overlayWords_[i].colors);
    }
    if (!same) {
        overlayWords_ = next;
        if (next.isEmpty()) {
            overlay_->ClearWords();
        } else {
            overlay_->SetWords(std::move(next));
        }
        // Removals apply at once; additions wait for the semantic cadence.
        // Pan/zoom never reaches here: geometry is answered from paint.
        if (screenText_) screenText_->ContentChanged();
        UpdateReadingText();
    }
    shownWords_ = overlayWords_.size();
}

UctScreenTextSnapshot UltraClearTextController::ScreenTextSnapshot() const
{
    UctScreenTextSnapshot snapshot;
    snapshot.enabled = enabled_;
    // overlayWords_ is non-empty only while every RefreshOverlay gate holds
    // (no Show original, settling, composite, watchdog, failure or
    // unusable backend): exactly when glyphs are drawn.
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

void UltraClearTextController::UpdateReadingText()
{
    // The pane offers exactly the lines verified on screen now (also while
    // Show original hides the glyph plates).
    QString text;
    if (enabled_ && AnalysisHealthy() && PresentationAvailable(clock_()) && !compositePaused_ && !settling_ && BackendUsable(backend_)) {
        text = model_.ReadingText(ModelTime(clock_()), currentFrameSeq_);
    }
    if (text == readingText_) {
        return;
    }
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
    if (!enabled_) {
        SetStatus(QStringLiteral("Ultra Clear Text is off."));
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
    if (!AnalysisHealthy()) {
        SetStatus(QStringLiteral("Text recognition stopped working. The original camera image is shown."), true);
        return;
    }
    if (compositePaused_) {
        SetStatus(QStringLiteral("Paused while Temporal Smooth or Extra Stable is on. "
                                 "The original camera image is shown."),
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
