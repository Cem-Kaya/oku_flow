#ifdef _WIN32
#include "app_internal.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/ultra_clear_text_controller.hpp"
#include "okuflow/ui/annotation_overlay.hpp"
#include <bit>
#include <chrono>
#include <type_traits>

namespace okuflow {
namespace {
std::uint64_t MediaAppearanceSignature(const ProcessingSettings& s) {
    std::uint64_t signature = 14695981039346656037ull;
    const auto mix = [&](auto value) {
        using T = decltype(value);
        const auto bits = [&]() -> std::uint64_t {
            if constexpr (std::is_floating_point_v<T>) return std::bit_cast<std::uint32_t>(value);
            else return static_cast<std::uint64_t>(value);
        }();
        signature = (signature ^ bits) * 1099511628211ull;
    };
    // Explicit members, never struct padding, pointers, frame-clock diagnostics
    // or view-only geometry. Every actual effect setting participates.
    mix(s.enableBlackWhite); mix(s.blackWhiteThreshold);
    mix(s.enableBlur); mix(s.blurRadius); mix(s.blurSigma);
    mix(s.enableSpatialSharpen); mix(s.spatialUpscaler); mix(s.spatialSharpness); mix(s.stagingFormat);
    mix(s.enableTemporalSmoothing); mix(s.temporalSmoothingAlpha);
    mix(s.enableStabilization); mix(s.enableBumpHold);
    mix(s.displayColorTransform); mix(s.displayColorLutGeneration);
    mix(s.textForegroundBgra); mix(s.textBackgroundBgra); mix(s.contrast); mix(s.brightness);
    mix(s.enableKeystone); mix(s.enableAutoContrast); mix(s.autoContrastStrength);
    mix(s.enableAutoTextClarity); mix(s.enableBackgroundFlatten); mix(s.backgroundFlattenStrength);
    mix(s.enableAdaptiveBinarization); mix(s.sauvolaStrength); mix(s.binarizationSoftness);
    mix(s.textPolarityMode); mix(s.strokeWeight); mix(s.enableSmartSharpen); mix(s.smartSharpenStrength);
    mix(s.enableClahe); mix(s.claheClipLimit); mix(s.enableTwoColorText);
    mix(s.enableTextHysteresis); mix(s.textHysteresisStrength); mix(s.enableSelectiveSharpen);
    mix(s.enableFocusDetection); mix(s.focusThreshold); mix(s.enableGlareSuppression); mix(s.glareSuppressionStrength);
    mix(s.enableMlSuperRes); mix(s.mlSuperResStrength); mix(s.mlSuperResUltra1440p);
    return signature;
}
}

OkuFlowApp::MediaSceneRecipe OkuFlowApp::SelectedMediaSceneRecipe(const VideoPlaybackFrame& frame) const {
    const int turns = ((rotationQuarterTurns_ % 4) + 4) % 4;
    const int colorTransform = displayColorScheme_.id == QStringLiteral("normal")     ? 0
                               : displayColorScheme_.id == QStringLiteral("inverted") ? 1
                                                                                      : 2;
    return {mediaDocumentEpoch_,
            frame.sampleId,
            frame.width,
            frame.height,
            frame.strideBytes,
            std::uint64_t(turns),
            MediaAppearanceSignature(BuildProcessingSettings(frame.width, frame.height,
                frame.width, frame.height, frame.sampleId)),
            std::bit_cast<std::uint32_t>(contrast_),
            std::bit_cast<std::uint32_t>(brightness_),
            std::uint64_t(blackWhiteEnabled_),
            std::bit_cast<std::uint32_t>(blackWhiteThreshold_),
            std::uint64_t(colorTransform)};
}

bool OkuFlowApp::AdmitSelectedMediaSceneRecipe(const MediaSceneRecipe& recipe) {
    if (mediaSceneCacheValid_ && mediaSceneRecipe_ == recipe)
        return false;
    // Rotation is part of appearance: a 180-degree turn keeps the extent and
    // frame identity but not the pixels, so it can never reuse the scene.
    if (!mediaSceneCacheValid_ || !std::equal(recipe.begin() + kMediaRecipeRotation, recipe.end(),
                                              mediaSceneRecipe_.begin() + kMediaRecipeRotation))
        ++mediaSceneAppearanceRevision_;
    mediaGpuAnalysis_.reset();
    mediaGpuNeighborAnalysis_.reset();
    mediaGpuAnalysisRetryAtMs_ = mediaGpuNeighborRetryAtMs_ = 0;
    mediaGpuAnalysisAttempts_ = 0;
    mediaScenePixels_.reset();
    mediaPresentedScenePixels_.reset();
    mediaSceneRecipe_ = recipe;
    const bool quarter = (recipe[kMediaRecipeRotation] & 1) != 0;
    mediaSceneWidth_ = static_cast<UINT>(quarter ? recipe[3] : recipe[2]);
    mediaSceneHeight_ = static_cast<UINT>(quarter ? recipe[2] : recipe[3]);
    mediaSceneCacheValid_ = true;
    mediaGpuViewRecipeValid_ = false;
    return true;
}

bool OkuFlowApp::SelectedMediaAnalysisGivenUp(bool analysisFailed) const {
    return analysisFailed || mediaGpuAnalysisAttempts_ >= kMediaAnalysisAttempts;
}

bool OkuFlowApp::SelectedMediaGpuSceneNeedsProcess(const MediaGpuSceneStamp& scene,
                                                   std::uint64_t frameSequence, bool uctEnabled,
                                                   std::int64_t nowMs) const {
    if (!scene.ready || scene.epoch != mediaDocumentEpoch_ || scene.frameSequence != frameSequence ||
        scene.appearance != mediaSceneAppearanceRevision_ || mediaGpuAnalysisEnabled_ != uctEnabled)
        return true;
    // A throttled or busy capture may legitimately miss one pass. Retrying is
    // bounded per frame and stops once the analysis pool reports failure.
    return uctEnabled && !mediaGpuAnalysis_ && !SelectedMediaAnalysisGivenUp(scene.analysisFailed) &&
           nowMs >= mediaGpuAnalysisRetryAtMs_;
}

void OkuFlowApp::SelectedMediaGpuSceneProcessed(bool uctEnabled, uct::SharedDeviceImage analysis,
                                                std::int64_t nowMs) {
    if (mediaGpuAnalysisEnabled_ != uctEnabled)
        mediaGpuAnalysisAttempts_ = 0;
    mediaGpuAnalysisEnabled_ = uctEnabled;
    if (!uctEnabled) {
        mediaGpuAnalysis_.reset();
        mediaGpuNeighborAnalysis_.reset();
        mediaGpuAnalysisAttempts_ = 0;
        mediaGpuAnalysisRetryAtMs_ = 0;
        return;
    }
    const auto matchesCurrent = [this](const uct::SharedDeviceImage& image) {
        return image && image->sourceEpoch == mediaDocumentEpoch_ &&
            image->frameSequence == mediaSceneRecipe_[1] &&
            image->processingGeneration == mediaSceneAppearanceRevision_;
    };
    // A paused ROI replay may produce a fresh allocation for the exact same
    // sample. Keep UCT's admitted lease: replacing it would make immutable
    // admission revoke the complete artifact on every pan, despite unchanged
    // source pixels. Source/frame/appearance changes still revoke immediately.
    if (!matchesCurrent(mediaGpuAnalysis_)) {
        mediaGpuAnalysis_.reset();
        if (matchesCurrent(analysis)) mediaGpuAnalysis_ = std::move(analysis);
    }
    // A view replay may hit the capture throttle. It neither discards a valid
    // same-scene lease nor spends another attempt before the retry deadline.
    if (mediaGpuAnalysis_) {
        mediaGpuAnalysisAttempts_ = 0;
        mediaGpuAnalysisRetryAtMs_ = 0;
    } else if (nowMs >= mediaGpuAnalysisRetryAtMs_) {
        ++mediaGpuAnalysisAttempts_;
        mediaGpuAnalysisRetryAtMs_ = nowMs + (std::int64_t{80} << std::min(mediaGpuAnalysisAttempts_ - 1, 3));
    }
}

bool OkuFlowApp::PresentSelectedMediaGpuFrame(const VideoPlaybackFrame& frame,
                                              const VideoPlaybackFrame* /*neighbor*/, bool continuousPlayback) {
#if !OKUFLOW_ENABLE_CUDA
    RequestMediaCpuFallback("This application build has no CUDA media output.");
    return false;
#else
    const bool cpuPixels = frame.bgra && VideoPlaybackLayoutFits(frame.width, frame.height, frame.strideBytes) &&
        frame.bgra->size() >= std::size_t(frame.strideBytes) * (frame.height - 1) + std::size_t(frame.width) * 4;
    if ((!cpuPixels && (!frame.gpu || !frame.gpu->texture)) || !presenter_ || !presenter_->GetDevice() ||
        !uiState_->renderWidget_ || !uiState_->renderWidget_->isPresenterReady())
        return false;
    if (HandlePresenterFault())
        return false;
    if (!SelectedMediaGpuAdapter()) {
        // This frame was decoded before its adapter's media CUDA scene failed
        // this session. Never repeat CUDA setup for it.
        RequestMediaCpuFallback("Selected media CUDA output is unavailable on this adapter.");
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const auto recipe = SelectedMediaSceneRecipe(frame);
    const int turns = static_cast<int>(recipe[kMediaRecipeRotation]);
    const UINT width = (turns & 1) ? frame.height : frame.width,
               height = (turns & 1) ? frame.width : frame.height;
    AdmitSelectedMediaSceneRecipe(recipe);
    const bool uctEnabled = ultraClearText_ && ultraClearText_->IsEnabled();
    if (!mediaGpuScene_)
        mediaGpuScene_ = std::make_unique<MediaGpuScene>();
    const QSize cache = ProcessingCacheExtent(width, height);
    const auto prepared = mediaGpuScene_->Prepare(presenter_->GetDevice(), width, height,
                                                  cache.width(), cache.height());
    if (prepared == MediaGpuScene::Status::Failed) {
        RequestMediaCpuFallback(mediaGpuScene_->Error());
        return false;
    }
    if (prepared != MediaGpuScene::Status::Ready)
        return false;
    ProcessingSettings settings = BuildProcessingSettings(width, height,
        mediaGpuScene_->SuperResWidth(), mediaGpuScene_->SuperResHeight(), frame.sampleId);
    mediaGpuScene_->SetSuperResPerformanceOverride(superResPerformanceOverride_);
    const std::array<std::uint64_t, 8> viewRecipe{
        std::bit_cast<std::uint32_t>(settings.zoomAmount),
        std::bit_cast<std::uint32_t>(settings.zoomCenterX),
        std::bit_cast<std::uint32_t>(settings.zoomCenterY),
        settings.spatialViewportWidth, settings.spatialViewportHeight,
        std::uint64_t(pipelineOrchestrator_->ViewportFitMode()),
        std::uint64_t(settings.deferMlSuperResForInteraction),
        (std::uint64_t(cache.width()) << 32) | std::uint64_t(cache.height())};
    // Ordinary navigation changes only the presenter transform. Replay the
    // saved frame for ROI-dependent enhancement; stabilization learns viewing
    // scale on the next real sample, without registering a paused sample again.
    const bool viewAffectsProcessing = settings.enableSpatialSharpen || settings.enableMlSuperRes;
    const bool mustProcess = SelectedMediaGpuSceneNeedsProcess(
        {mediaGpuScene_->SceneReady(), mediaGpuScene_->SceneEpoch(), mediaGpuScene_->SceneFrameSequence(),
         mediaGpuScene_->SceneAppearance(), mediaGpuScene_->UctAnalysisFailed()},
        frame.sampleId, uctEnabled, now) ||
        (viewAffectsProcessing && (!mediaGpuViewRecipeValid_ || mediaGpuViewRecipe_ != viewRecipe));
    if (mustProcess) {
        // The previous native texture receipt is no longer a usable Save/ink
        // source after its shared scene is overwritten. Busy admission keeps it.
        const auto processed = mediaGpuScene_->Process(frame, turns, settings, mediaDocumentEpoch_,
                                                       mediaSceneAppearanceRevision_, uctEnabled);
        if (processed == MediaGpuScene::Status::Failed) {
            RequestMediaCpuFallback(mediaGpuScene_->Error());
            return false;
        }
        if (processed != MediaGpuScene::Status::Ready)
            return false;
        mediaGpuViewRecipe_ = viewRecipe;
        mediaGpuViewRecipeValid_ = true;
        SelectedMediaGpuSceneProcessed(uctEnabled,
                                       uctEnabled ? mediaGpuScene_->TakeUctAnalysis() : uct::SharedDeviceImage{},
                                       now);
        if (controlTarget_)
            controlTarget_->FrameProcessed();
    }
    const auto refresh = mediaGpuScene_->RefreshSuperRes();
    if (refresh == SuperResRefreshResult::Failed) {
        RequestMediaCpuFallback(mediaGpuScene_->Error());
        return false;
    }
    const auto generation = mediaGpuScene_->SceneGeneration();
    auto transform =
        ComputeViewTransform(width, height, presenter_->ViewportWidth(), presenter_->ViewportHeight(),
                             zoomEnabled_ ? zoomAmount_ : 1.0f, zoomCenterX_, zoomCenterY_,
                             pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fit
                                 ? ViewportFitMode::kFit
                                 : ViewportFitMode::kFill);
    if (settings.enableMlSuperRes && !settings.mlSuperResUltra1440p && settings.zoomAmount >= 1.33f)
        transform = CanonicalizeSuperResViewTransform(transform, width, height,
            mediaGpuScene_->SuperResWidth(), mediaGpuScene_->SuperResHeight(), settings.zoomAmount);
    if (!transform.valid) return false;
    auto renderTransform = transform;
    ID3D12Resource* renderTexture = mediaGpuScene_->Texture();
    UINT renderWidth = width, renderHeight = height;
    float focusX = zoomCenterX_, focusY = zoomCenterY_;
    const auto roi = mediaGpuScene_->SuperResRoi();
    const NormalizedSourceRect rect{roi.sourceX, roi.sourceY, roi.sourceWidth, roi.sourceHeight};
    ViewTransform remapped;
    bool neural = false;
    if (roi.valid && mediaGpuScene_->SuperResTexture() && RemapViewTransformToSourceRect(transform, rect, remapped)) {
        const float fractionX = float(roi.outputWidth) / mediaGpuScene_->SuperResWidth();
        const float fractionY = float(roi.outputHeight) / mediaGpuScene_->SuperResHeight();
        renderTransform = remapped;
        renderTransform.sourceX *= fractionX; renderTransform.sourceY *= fractionY;
        renderTransform.sourceWidth *= fractionX; renderTransform.sourceHeight *= fractionY;
        focusX = (zoomCenterX_ - roi.sourceX) / roi.sourceWidth * fractionX;
        focusY = (zoomCenterY_ - roi.sourceY) / roi.sourceHeight * fractionY;
        renderTexture = mediaGpuScene_->SuperResTexture();
        renderWidth = mediaGpuScene_->SuperResWidth(); renderHeight = mediaGpuScene_->SuperResHeight();
        neural = mediaGpuScene_->IsSuperResActive();
    }
    focusX = std::clamp(focusX, 0.0f, 1.0f);
    focusY = std::clamp(focusY, 0.0f, 1.0f);
    const bool marker = focusMarkerEnabled_ &&
                        pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fill;
    const MediaViewportAdmission::Stamp viewport{recipe,
                                                 transform,
                                                 presenter_->ViewportWidth(),
                                                 presenter_->ViewportHeight(),
                                                 marker,
                                                 marker ? zoomCenterX_ : 0.0f,
                                                 marker ? zoomCenterY_ : 0.0f};
    UctFrameContext context;
    context.geometry.settings.sourceEpoch = mediaDocumentEpoch_;
    context.geometry.settings.rotationQuarterTurns = turns;
    context.geometry.settings.colorScheme = mediaSceneAppearanceRevision_;
    context.geometry.sceneWidth = width;
    context.geometry.sceneHeight = height;
    context.geometry.settings.stabilization = settings.enableStabilization;
    context.geometry.settings.keystone = settings.enableKeystone;
    context.geometry.settings.warpRevision = uctWarpRevision_;
    context.geometry.neuralEnhancement = neural;
    context.requested = context.geometry.settings;
    context.compositeFrames = settings.enableTemporalSmoothing ||
        (settings.enableStabilization && settings.enableBumpHold);
    // Extra Stable media captures carry their frame's device hold record, so
    // UCT names held/crossfade pixels per capture instead of pausing.
    context.heldFrames = settings.enableStabilization && settings.enableBumpHold;
    context.strokeWeight = settings.strokeWeight;
    context.frameSeq = frame.sampleId;
    context.transform = transform;
    context.viewportWidth = viewport.width;
    context.viewportHeight = viewport.height;
    auto observation = controlTarget_ ? controlTarget_->PresentedMediaFrame(transform, QSize(width, height),
                                         QSize(viewport.width, viewport.height), frame.sampleId, SelectedMediaDocumentPts100ns(frame.pts100ns), true)
                                      : agent::ObservationFrame{};
    observation.diagnostics["media"] = QJsonObject{{"sourceEpoch", qint64(mediaDocumentEpoch_)},
                                                   {"frameSequence", qint64(frame.sampleId)},
                                                   {"backend", "gpu-texture"}};
    const bool observationWanted = controlTarget_ && controlTarget_->WantsObservationReadback(observation);
    const bool explainWanted = pendingOnDemandAnalysis_ && pendingOnDemandReadbackId_ == 0;
    const bool assistantWanted = pendingAssistantFramePrompt_ && pendingAssistantFrameReadbackId_ == 0;
    const AnnotationGpuPresentation annotationSource{
        renderTexture, renderWidth, renderHeight, renderTransform, neural, marker, focusX, focusY};
    const bool annotationWanted = pendingAnnotationReadbackId_ == 0 && AnnotationFrameMatches(context) &&
                                  AnnotationGpuPresentationMatches(annotationSource);
    const bool readbackWanted = observationWanted || explainWanted || assistantWanted || annotationWanted;
    const bool needsPresent =
        mustProcess || generation != mediaPresentedSceneGeneration_ || readbackWanted ||
        mediaViewportAdmission_.NeedsPresentation(viewport, pipelineOrchestrator_->IsViewportDirty(),
                                                  presenter_->NeedsScenePresent());
    bool presented = true;
    if (needsPresent) {
        mainWindow_->setAnnotationViewTransform(transform);
        ViewportPresentationOptions options{};
        options.drawFocusMarker = marker;
        options.focusX = focusX;
        options.focusY = focusY;
        options.requestReadback = readbackWanted;
        auto sync = mediaGpuScene_->PresentationSync();
        UINT64 readback{}, independentSignal{};
        presented = presenter_->PresentSceneTexture(renderTexture, renderWidth, renderHeight, renderTransform,
                                                    &sync, &options, &readback, &independentSignal);
        if (independentSignal)
            mediaGpuScene_->RecordPresented(independentSignal);
        if (!presented) {
            pipelineOrchestrator_->MarkViewportDirty();
            if (presenter_->IsFaulted() && ultraClearText_)
                ultraClearText_->OnMediaPresentationFailed();
            return false;
        }
        // Scene preparation can succeed while native draw admission is busy.
        // Commit annotation authority only for the pixels actually submitted.
        if (!mediaViewportAdmission_.displayed || mediaViewportAdmission_.displayed->recipe != recipe ||
            generation != mediaPresentedSceneGeneration_)
            ++annotationSceneRevision_;
        mediaPresentedSceneGeneration_ = generation;
        mediaPresentedWithCuda_ = true;
        mediaSuperResPresented_ = neural;
        mediaViewportAdmission_.Presented(viewport);
        pipelineOrchestrator_->MarkViewportPresented();
        lastAnnotationPresentation_ = context;
        lastAnnotationSceneRevision_ = annotationSceneRevision_;
        lastAnnotationGpuPresentation_ = AnnotationGpuPresentation{
            renderTexture, renderWidth, renderHeight, renderTransform, neural, marker, focusX, focusY};
        annotationPresentationCuda_ = true;
        presentationBuffer_.clear();
        presentationWidth_ = viewport.width;
        presentationHeight_ = viewport.height;
        if (controlTarget_)
            controlTarget_->FramePresented(observation);
        if (readback) {
            if (observationWanted)
                controlTarget_->ObservationReadbackSubmitted(readback, observation);
            if (explainWanted) {
                pendingOnDemandReadbackId_ = readback;
                pendingOnDemandReadbackTimer_.restart();
            }
            if (assistantWanted) {
                pendingAssistantFrameReadbackId_ = readback;
                pendingAssistantFrameReadbackTimer_.restart();
            }
            if (annotationWanted) {
                activeAnnotationCapture_ = std::move(annotationCaptureQueue_.front());
                annotationCaptureQueue_.pop_front();
                pendingAnnotationReadbackId_ = readback;
                pendingAnnotationReadbackTimer_.restart();
            }
        } else if (readbackWanted)
            pipelineOrchestrator_->MarkViewportDirty();
    }
    if (now >= mediaDiagnosticsRefreshAtMs_) {
        mediaDiagnosticsRefreshAtMs_ = now + 100;
        UpdateKeystoneTrackingUi();
        UpdateProcessingStatusLabel();
    }
    if (ultraClearText_) {
        const bool suppressed = !mediaGpuScene_->UctCoordinatesReliable();
        if (mediaGpuAnalysisSuppressed_ && !suppressed) {
            // A completed/disabled neural presentation may restore exact OCR
            // correspondence without changing the frame or appearance recipe.
            mediaGpuAnalysisAttempts_ = 0;
            mediaGpuAnalysisRetryAtMs_ = 0;
        }
        mediaGpuAnalysisSuppressed_ = suppressed;
        ultraClearText_->SetMediaOverlaySuppressed(suppressed);
        if (!mediaGpuScene_->UctCoordinatesReliable()) {
            mediaGpuAnalysis_.reset();
            mediaGpuNeighborAnalysis_.reset();
            mediaUctContinuousPlayback_ = continuousPlayback;
            return presented;
        }
        // Paused recognition consumes only the exact displayed image.
        mediaGpuNeighborAnalysis_.reset();
        if (mediaGpuAnalysis_) {
            if (continuousPlayback) {
                UctAdvancingMediaScene scene;
                scene.geometry = context.geometry;
                scene.sourceAuthority = mediaDocumentEpoch_;
                scene.current = {{}, frame.sampleId, frame.pts100ns / 10'000, mediaGpuAnalysis_};
                scene.transform = transform;
                scene.viewportWidth = viewport.width;
                scene.viewportHeight = viewport.height;
            scene.strokeWeight = settings.strokeWeight;
                ultraClearText_->OnAdvancingMediaPresented(std::move(scene));
            } else {
                UctImmutableScene scene;
                scene.admission = uct::ImmutableAdmission::Standalone;
                scene.geometry = context.geometry;
                scene.sourceAuthority = mediaDocumentEpoch_;
                scene.selected = {{}, frame.sampleId, frame.pts100ns / 10'000, mediaGpuAnalysis_};
                scene.transform = transform;
                scene.viewportWidth = viewport.width;
                scene.viewportHeight = viewport.height;
            scene.strokeWeight = settings.strokeWeight;
                ultraClearText_->SetImmutableScene(std::move(scene));
            }
        } else if (continuousPlayback) {
            UctAdvancingMediaPresentation receipt;
            receipt.geometry = context.geometry;
            receipt.sourceAuthority = mediaDocumentEpoch_;
            receipt.frameSeq = frame.sampleId;
            receipt.timestampMs = frame.pts100ns / 10'000;
            receipt.transform = transform;
            receipt.viewportWidth = viewport.width;
            receipt.viewportHeight = viewport.height;
            receipt.strokeWeight = settings.strokeWeight;
            ultraClearText_->OnAdvancingMediaPresentedWithoutAnalysis(std::move(receipt));
        } else if (uctEnabled && SelectedMediaAnalysisGivenUp(mediaGpuScene_->UctAnalysisFailed())) {
            // The selected frame was presented, but its bounded analysis
            // retries ended or the pool failed. Report the missing analysis
            // input as analysis health, never as a presentation failure.
            UctImmutableScene scene;
            scene.admission = uct::ImmutableAdmission::Standalone;
            scene.geometry = context.geometry;
            scene.sourceAuthority = mediaDocumentEpoch_;
            scene.selected = {{}, frame.sampleId, frame.pts100ns / 10'000, {}};
            scene.transform = transform;
            scene.viewportWidth = viewport.width;
            scene.viewportHeight = viewport.height;
            scene.strokeWeight = settings.strokeWeight;
            ultraClearText_->SetImmutableScene(std::move(scene));
        } else if (ultraClearText_->HasImmutableScene()) {
            // Awaiting this frame's analysis retry (or UCT off) is not a
            // failure. Words read from different pixels are revoked.
            ultraClearText_->ClearImmutableScene();
        }
        mediaUctContinuousPlayback_ = continuousPlayback;
    }
    return presented;
#endif
}
} // namespace okuflow
#endif
