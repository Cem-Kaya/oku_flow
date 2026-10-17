#include "okuflow/app/ultra_clear_text_controller.hpp"

#if defined(_WIN32)

#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"

#include <QCheckBox>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QSignalBlocker>
#include <QTimer>

#include <chrono>

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
        if (enabled_ && overlay_ && overlay_->WordCount() > 0 &&
            clock_() - lastPresentMs_ > kPresentationWatchdogMs) {
            overlay_->ClearWords();
            overlayWords_.clear();
            shownWords_ = 0;
            UpdateStatus();
        }
    });
    if (panel_) {
        connect(panel_, &UltraClearTextPanel::enabledChanged, this, &UltraClearTextController::SetEnabled);
        connect(panel_, &UltraClearTextPanel::showOriginalChanged, this,
                &UltraClearTextController::SetShowOriginal);
    }
    UpdateStatus();
}

UltraClearTextController::~UltraClearTextController()
{
    // Disables worker callbacks before this object's members disappear.
    probeWorker_.reset();
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
    backend_ = {};
    scheduler_ = std::make_unique<uct::RecognitionScheduler>(
        factory_ ? factory_() : std::unique_ptr<uct::Recognizer>{},
        [self](uct::RecognitionResult result) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, result = std::move(result)]() mutable {
                    if (self) {
                        self->HandleResult(std::move(result));
                    }
                },
                Qt::QueuedConnection);
        },
        [self](uct::BackendStatus status) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, status = std::move(status)]() mutable {
                    if (self) {
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
        EnsureScheduler();
        watchdog_->start();
        // A user re-enable retries a backend that only failed at run time.
        if (backend_.state == uct::BackendState::kFailed) {
            backend_ = scheduler_->Status();
        }
        haveGeometry_ = false; // the next presentation starts a fresh generation
        settling_ = false;
        consecutiveFailures_ = 0;
    } else {
        watchdog_->stop();
        Invalidate({});
    }
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

bool UltraClearTextController::OnPresented(const UctFrameContext& context)
{
    ++presentCount_;
    if (!enabled_) {
        return false;
    }
    const std::int64_t now = clock_();
    lastPresentMs_ = now;
    presentationFailed_ = false;
    cameraActive_ = context.cameraActive;
    currentFrameSeq_ = context.frameSeq;
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
    // Freshness is enforced on the presentation clock, not the OCR clock.
    RefreshOverlay(now);
    UpdateStatus();
    if (settling_ || compositePaused_ || !cameraActive_ || pendingReadbackId_ != 0 ||
        (scheduler_ && !BackendUsable(backend_))) {
        return false;
    }
    // Latest-only end to end: no new viewport copy while the probe worker
    // still has one waiting to start.
    if (probeWorker_ && probeWorker_->HasPending()) {
        return false;
    }
    return now - lastProbeRequestMs_ >= timing_.probeIntervalMs;
}

void UltraClearTextController::OnSourceRetired()
{
    cameraActive_ = false;
    haveGeometry_ = false;
    ++geometryGeneration_;
    Invalidate(QStringLiteral("scene"));
    UpdateStatus();
}

void UltraClearTextController::OnPresentationFailed()
{
    if (!enabled_) {
        return;
    }
    presentationFailed_ = true;
    if (overlay_) {
        overlay_->ClearWords();
    }
    overlayWords_.clear();
    shownWords_ = 0;
}

void UltraClearTextController::OnReadbackRequested(std::uint64_t requestId,
                                                   const UctFrameContext& context)
{
    if (!enabled_ || requestId == 0 || settling_) {
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
    if (!enabled_ || settling_) {
        return;
    }
    const std::int64_t now = clock_();
    lastProbeRequestMs_ = now;
    uct::SourceStamp stamp = MakeStamp(context, now);
    stamp.viewportWidth = static_cast<std::uint32_t>(std::max(0, width));
    stamp.viewportHeight = static_cast<std::uint32_t>(std::max(0, height));
    SubmitProbe(stamp, pool_->Copy(bgra, width, height, strideBytes));
}

uct::SchedulerStats UltraClearTextController::SchedulerStatistics() const
{
    return scheduler_ ? scheduler_->Stats() : uct::SchedulerStats{};
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
    model_.Reset();
    latestProbe_.reset();
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
    readingText_.clear();
    if (panel_) {
        panel_->SetReadingText({});
    }
    Q_UNUSED(reason);
}

void UltraClearTextController::SubmitProbe(const uct::SourceStamp& stamp, uct::SharedPixels pixels)
{
    if (!pixels || !probeWorker_ || settling_ || compositePaused_ || !stamp.SameScene(CurrentScene())) {
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
    if (!enabled_ || !probeWorker_ || !probeWorker_->IsCurrent(outcome) || settling_ || compositePaused_ ||
        !outcome.stamp.SameScene(CurrentScene()) || !outcome.probe) {
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

    if (scheduler_ && BackendUsable(backend_) && stableProbes_ >= 1 && outcome.pixels) {
        const int width = outcome.pixels->width;
        const int height = outcome.pixels->height;
        const QRectF viewScene =
            uct::ViewRectToScene(outcome.stamp.viewTransform, width, height, QRectF(0, 0, width, height))
                .intersected(QRectF(0.0, 0.0, 1.0, 1.0));
        const bool covered = Area(viewScene) > 0.0 &&
                             Area(lastJobCoverage_.intersected(viewScene)) >= 0.8 * Area(viewScene);
        const bool needed = model_.PendingWordCount() > 0 || !model_.HasAcceptedText() || !covered ||
                            now - lastSubmitMs_ >= timing_.refreshIntervalMs;
        // Latest-only: a busy worker only receives a replacement after the
        // scene changed, so slow recognition cannot accumulate work. The
        // pixels move on without a copy.
        if (needed && (!scheduler_->HasWork() || changedSinceSubmit_)) {
            if (scheduler_->Submit(outcome.stamp, std::move(outcome.pixels)) != 0) {
                lastSubmitMs_ = now;
                lastJobCoverage_ = viewScene;
                changedSinceSubmit_ = false;
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
    QElapsedTimer handling;
    handling.start();
    if (!enabled_ || !scheduler_ || !scheduler_->IsCurrent(result) || settling_) {
        ++rejectedResults_;
        return;
    }
    const std::int64_t now = clock_();
    if (!result.output.ok && !result.output.cancelled) {
        if (++consecutiveFailures_ >= kMaximumConsecutiveFailures) {
            backend_.state = uct::BackendState::kFailed;
            backend_.detail = result.output.error;
            Invalidate(QStringLiteral("failed"));
        }
    } else if (result.output.ok) {
        consecutiveFailures_ = 0;
    }
    const uct::AdmitVerdict verdict = model_.Admit(result, CurrentScene(), latestProbe_.get(), now);
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
    UpdateStatus();
}

void UltraClearTextController::RefreshOverlay(std::int64_t nowMs)
{
    if (!overlay_) {
        return;
    }
    if (!enabled_ || showOriginal_ || compositePaused_ || settling_ || presentationFailed_ ||
        !BackendUsable(backend_)) {
        overlay_->ClearWords();
        overlayWords_.clear();
        shownWords_ = 0;
        UpdateReadingText();
        return;
    }
    const QVector<uct::SceneWord> words = model_.SceneWords(nowMs, currentFrameSeq_);
    QVector<UltraClearTextOverlay::SceneWord> next;
    next.reserve(words.size());
    for (const uct::SceneWord& word : words) {
        next.push_back({word.text, word.sceneRect, word.colors, word.lineId});
    }
    // Text, position and colors all decide what is drawn; a re-recognized
    // word in a new color scheme must not keep its old plate.
    bool same = next.size() == overlayWords_.size();
    for (int i = 0; same && i < next.size(); ++i) {
        same = next[i].text == overlayWords_[i].text &&
               next[i].sceneRect == overlayWords_[i].sceneRect &&
               next[i].lineId == overlayWords_[i].lineId &&
               SameColors(next[i].colors, overlayWords_[i].colors);
    }
    if (!same) {
        overlayWords_ = next;
        if (next.isEmpty()) {
            overlay_->ClearWords();
        } else {
            overlay_->SetWords(std::move(next));
        }
        UpdateReadingText();
    }
    shownWords_ = overlayWords_.size();
}

void UltraClearTextController::UpdateReadingText()
{
    // The pane offers exactly the lines verified on screen now (also while
    // Show original hides the glyph plates).
    QString text;
    if (enabled_ && !compositePaused_ && !settling_ && BackendUsable(backend_)) {
        text = model_.ReadingText(clock_(), currentFrameSeq_);
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
    if (compositePaused_) {
        SetStatus(QStringLiteral("Paused while Temporal Smooth or Extra Stable is on. "
                                 "The original camera image is shown."),
                  true);
    } else if (!cameraActive_) {
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
