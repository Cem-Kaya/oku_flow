#ifdef _WIN32

#include "app_internal.hpp"

#include <QAccessible>
#include <QLockFile>
#include <QMetaObject>
#include <QPointer>
#include <QScopeGuard>
#include <QThreadPool>

#include <set>
#include <tuple>
#include <utility>

namespace okuflow {

namespace {

RecordingViewTransform ToRecordingTransform(const ViewTransform& transform)
{
    return RecordingViewTransform{
        transform.sourceX,
        transform.sourceY,
        transform.sourceWidth,
        transform.sourceHeight,
        transform.destinationX,
        transform.destinationY,
        transform.destinationWidth,
        transform.destinationHeight,
        transform.valid,
    };
}

float MeasureCaptureToPresentLatency(std::int64_t captureClock100ns)
{
    if (captureClock100ns < 0) {
        return -1.0f;
    }

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    static const LONGLONG frequencyValue = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart;
    }();
    if (frequencyValue <= 0) {
        return -1.0f;
    }
    const std::int64_t now100ns =
        static_cast<std::int64_t>(
            static_cast<long double>(counter.QuadPart) *
            kMediaFoundationTicksPerSecond /
            static_cast<long double>(frequencyValue));

    static std::uint64_t samples = 0;
    static long double total100ns = 0.0;
    static std::int64_t maximum100ns = 0;
    static std::int64_t reportStart100ns = now100ns;
    const std::int64_t latency100ns =
        std::max<std::int64_t>(0, now100ns - captureClock100ns);
    const float latencyMs =
        static_cast<float>(latency100ns) / 10'000.0f;

    static const bool enabled =
        qEnvironmentVariableIsSet("OKUFLOW_CAPTURE_DIAGNOSTICS");
    if (!enabled) {
        return latencyMs;
    }
    ++samples;
    total100ns += latency100ns;
    maximum100ns = std::max(maximum100ns, latency100ns);
    if (now100ns - reportStart100ns < 5 * 10'000'000LL) {
        return latencyMs;
    }

    qInfo().nospace()
        << "Capture-to-present diagnostics: "
        << static_cast<double>(total100ns / samples / 10'000.0L)
        << " ms average | "
        << static_cast<double>(maximum100ns / 10'000.0)
        << " ms maximum | " << samples << " frames";
    samples = 0;
    total100ns = 0.0;
    maximum100ns = 0;
    reportStart100ns = now100ns;
    return latencyMs;
}

} // namespace

namespace {

struct SuperResCacheExtent {
    UINT width{};
    UINT height{};
};

SuperResCacheExtent ComputeSuperResCacheExtent(UINT sceneWidth,
                                               UINT sceneHeight,
                                               bool ultra1440p) {
    SuperResCacheExtent extent{sceneWidth, sceneHeight};
    if (!ultra1440p || sceneWidth == 0 || sceneHeight == 0) {
        return extent;
    }

    const UINT maxWidth = sceneWidth >= sceneHeight ? 2560u : 1440u;
    const UINT maxHeight = sceneWidth >= sceneHeight ? 1440u : 2560u;
    struct Scale {
        UINT numerator;
        UINT denominator;
    };
    constexpr Scale kScales[] = {
        {4u, 1u}, {3u, 1u}, {2u, 1u}, {3u, 2u}, {4u, 3u}};
    for (const Scale scale : kScales) {
        const std::uint64_t scaledWidth =
            static_cast<std::uint64_t>(sceneWidth) * scale.numerator;
        const std::uint64_t scaledHeight =
            static_cast<std::uint64_t>(sceneHeight) * scale.numerator;
        if (scaledWidth % scale.denominator != 0u ||
            scaledHeight % scale.denominator != 0u) {
            continue;
        }
        const std::uint64_t candidateWidth =
            scaledWidth / scale.denominator;
        const std::uint64_t candidateHeight =
            scaledHeight / scale.denominator;
        if (candidateWidth <= maxWidth && candidateHeight <= maxHeight) {
            extent = {static_cast<UINT>(candidateWidth),
                      static_cast<UINT>(candidateHeight)};
            break;
        }
    }
    return extent;
}

} // namespace

void OkuFlowApp::EnumerateCameras() {
    cameras_ = mediaCapture_.EnumerateCameras();
    if (cameras_.empty()) {
        selectedCameraIndex_ = -1;
        return;
    }
    const int persistedIndex =
        settingsController_
            ? settingsController_->MutableSettings().cameraIndex
            : -1;
    selectedCameraIndex_ =
        persistedIndex >= 0 &&
                static_cast<size_t>(persistedIndex) < cameras_.size()
            ? persistedIndex
            : 0;
}

void OkuFlowApp::PopulateCameraCombo() {
    if (!uiState_->cameraCombo_) {
        return;
    }

    uiState_->cameraCombo_->clear();
    for (const auto& camera : cameras_) {
        uiState_->cameraCombo_->addItem(QString::fromWCharArray(camera.name.c_str()));
    }
}

void OkuFlowApp::EnumerateMicrophones()
{
    microphones_ = audioCapture_.EnumerateDevices();
}

void OkuFlowApp::PopulateMicrophoneCombo()
{
    if (!uiState_ || !uiState_->microphoneCombo_) {
        return;
    }
    const QSignalBlocker blocker(uiState_->microphoneCombo_);
    uiState_->microphoneCombo_->clear();
    uiState_->microphoneCombo_->addItem(
        QStringLiteral("No microphone (video only)"),
        QStringLiteral("__none__"));
    const QString persisted =
        settingsController_->MutableSettings().microphoneEndpointId;
    int selectedIndex = 0;
    for (const AudioDeviceDescriptor& microphone : microphones_) {
        const QString endpoint =
            QString::fromStdWString(microphone.endpointId);
        const QString label =
            microphone.isDefault
                ? QStringLiteral("%1 (system default)")
                      .arg(QString::fromStdWString(microphone.name))
                : QString::fromStdWString(microphone.name);
        uiState_->microphoneCombo_->addItem(
            label, endpoint);
        if (!persisted.isEmpty() && endpoint == persisted) {
            selectedIndex = uiState_->microphoneCombo_->count() - 1;
        } else if (persisted.isEmpty() && microphone.isDefault) {
            selectedIndex = uiState_->microphoneCombo_->count() - 1;
        }
    }
    if (persisted == QStringLiteral("__none__")) {
        selectedIndex = 0;
    } else if (persisted.isEmpty() && selectedIndex == 0 &&
               !microphones_.empty()) {
        selectedIndex = 1;
    }
    uiState_->microphoneCombo_->setCurrentIndex(selectedIndex);
    const QString selectedEndpoint =
        uiState_->microphoneCombo_->currentData().toString();
    if (persisted.isEmpty() && selectedIndex > 0) {
        settingsController_->MutableSettings().microphoneEndpointId =
            selectedEndpoint;
    } else if (!persisted.isEmpty() &&
               persisted != QStringLiteral("__none__") &&
               selectedIndex == 0) {
        settingsController_->MutableSettings().microphoneEndpointId.clear();
        ShowStatusMessage(
            QStringLiteral(
                "The saved microphone is unavailable. Recordings will be "
                "video only until another microphone is selected."),
            9000);
    }
}

void OkuFlowApp::RefreshCameraFormats(size_t index) {
    if (!uiState_->cameraFormatCombo_) {
        return;
    }
    const QSignalBlocker blocker(uiState_->cameraFormatCombo_);
    uiState_->cameraFormatCombo_->clear();
    uiState_->cameraFormatCombo_->addItem(
        QStringLiteral("Automatic (driver's choice)"), QString());
    cameraFormats_.clear();
    if (index >= cameras_.size()) {
        return;
    }

    // Reuse the reader already opened for capture; querying modes must not
    // activate and shut down the same physical camera a second time.
    const auto formats = cameraActive_ && static_cast<int>(index) == selectedCameraIndex_
                             ? mediaCapture_.NativeFormats()
                             : mediaCapture_.EnumerateFormats(cameras_[index]);
    if (formats.empty()) {
        const std::string& detail = mediaCapture_.LastError();
        if (uiState_->cameraFormatNoticeLabel_) {
            SetLiveText(
                uiState_->cameraFormatNoticeLabel_,
                !detail.empty()
                    ? QStringLiteral("Modes unavailable: %1")
                          .arg(QString::fromStdString(detail))
                    : QStringLiteral("No camera modes were reported."),
                LivePoliteness::kPolite,
                QStringLiteral("Camera format notice"));
            uiState_->cameraFormatNoticeLabel_->show();
        }
        return;
    }

    std::vector<VideoFormat> sorted = formats;
    std::sort(sorted.begin(), sorted.end(), [](const VideoFormat& a, const VideoFormat& b) {
        if (a.height != b.height) {
            return a.height > b.height;
        }
        const double fpsA = (a.denominator == 0) ? 0.0 : static_cast<double>(a.numerator) / static_cast<double>(a.denominator);
        const double fpsB = (b.denominator == 0) ? 0.0 : static_cast<double>(b.numerator) / static_cast<double>(b.denominator);
        if (std::abs(fpsA - fpsB) > 0.01) {
            return fpsA > fpsB;
        }
        if (a.width != b.width) {
            return a.width > b.width;
        }
        return a.stableId < b.stableId;
    });

    std::set<std::tuple<UINT, UINT, UINT, UINT>> displayed;
    int selectedIndex = 0;
    const QString persistedId =
        settingsController_->MutableSettings().cameraFormatStableId;
    for (const auto& fmt : sorted) {
        const auto key =
            std::make_tuple(fmt.width, fmt.height, fmt.numerator, fmt.denominator);
        if (!displayed.insert(key).second) {
            continue;
        }
        QString fpsText;
        if (fmt.numerator == 0 || fmt.denominator == 0) {
            fpsText = QStringLiteral("?");
        } else {
            const double fps = static_cast<double>(fmt.numerator) / static_cast<double>(fmt.denominator);
            if (std::abs(fps - std::round(fps)) < 0.01) {
                fpsText = QString::number(static_cast<int>(std::round(fps)));
            } else {
                fpsText = QString::number(fps, 'f', 2);
            }
        }
        const QString line = QStringLiteral("%1 \u00d7 %2 @ %3 fps")
                                 .arg(fmt.width)
                                 .arg(fmt.height)
                                 .arg(fpsText);
        cameraFormats_.push_back(fmt);
        const QString stableId = QString::fromStdWString(fmt.stableId);
        uiState_->cameraFormatCombo_->addItem(line, stableId);
        if (!persistedId.isEmpty() && persistedId == stableId) {
            selectedIndex = uiState_->cameraFormatCombo_->count() - 1;
        }
    }
    uiState_->cameraFormatCombo_->setCurrentIndex(selectedIndex);
    if (uiState_->cameraFormatNoticeLabel_) {
        uiState_->cameraFormatNoticeLabel_->hide();
    }
}

void OkuFlowApp::ResetCudaFenceState() {
    const UINT64 baseValue = presenter_ ? presenter_->GetLastSignaledFenceValue() : 0;
    pipelineOrchestrator_->ResetFence(baseValue);
    if (recordingManager_) {
        recordingManager_->ClearPendingReadbacks();
    }
}

// S6b recovery policy: one failed ProcessFrame only rolls the fence
// reservation back (RunCudaPipeline calls CudaFailed() before this). Three
// consecutive failures trigger a full resync — drain the graphics queue and
// re-seed the fence timeline — plus a single status message, so a persistent
// CUDA failure can neither wedge the present loop nor spam the user.
void OkuFlowApp::HandleCudaProcessingFailure() {
    if (pipelineOrchestrator_->RecordCudaFailure() == 3) {
        if (presenter_) {
            if (!presenter_->WaitForIdle()) {
                HandlePresenterFault();
                return;
            }
        }
        ResetCudaFenceState();
        qWarning() << "CUDA processing failed 3 times in a row; fence state resynced";
        ShowStatusMessage(QStringLiteral(
            "GPU processing keeps failing - showing unprocessed video."));
    }
}

bool OkuFlowApp::HandlePresenterFault() {
    if ((!presenter_ || !presenter_->IsFaulted()) &&
        (!cudaSurface_ || !cudaSurface_->IsFaulted())) return false;
    usingCudaLastFrame_ = false;
    cudaSceneReady_ = false;
    cpuSceneReady_ = false;
    if (!presenterFaultReported_) {
        presenterFaultReported_ = true;
        ShowStatusMessage(QStringLiteral(
            "GPU stopped responding. Restart OkuFlow to resume video."),
            15000, LivePoliteness::kAssertive);
    }
    return true;
}

bool OkuFlowApp::EnsureCudaSurface(UINT width, UINT height) {
    if (HandlePresenterFault()) return false;
    if (!presenter_ || !uiState_->renderWidget_ || !uiState_->renderWidget_->isPresenterReady()) {
        // Readiness is transient and checked again next frame without GPU work.
        return false;
    }

    const SuperResCacheExtent superResExtent =
        ComputeSuperResCacheExtent(
            width,
            height,
            mlTextSuperResolutionEnabled_ &&
                mlTextSuperResolutionUltra1440p_);
    if (cudaSurface_ && cudaSurface_->IsValid() &&
        cudaSurfaceWidth_ == width && cudaSurfaceHeight_ == height &&
        cudaSuperResWidth_ == superResExtent.width &&
        cudaSuperResHeight_ == superResExtent.height) {
        return true;
    }

    ID3D12Device* device = presenter_->GetDevice();
    const CudaSurfaceConfiguration configuration{
        device, presenter_->GetFence(), cameraSessionId_, width, height,
        superResExtent.width, superResExtent.height};
    if (!cudaSurfaceRetry_.ShouldAttempt(
            configuration, CudaSurfaceRetry::Clock::now())) {
        return false;
    }
    if (!device) {
        cudaSurfaceRetry_.RecordFailure(CudaSurfaceRetry::Clock::now());
        qWarning() << "CUDA surface unavailable: presenter returned null device";
        return false;
    }

    // Drain the graphics queue first: pipelined presents may still be copying
    // from the shared texture we are about to release.
    if (!presenter_->WaitForIdle()) {
        HandlePresenterFault();
        return false;
    }
    if (cudaSurface_ && !cudaSurface_->WaitForIdle()) {
        HandlePresenterFault();
        return false;
    }
    cudaSurface_.reset();
    cudaSharedTexture_.Reset();
    cudaSuperResTexture_.Reset();
    cudaOriginalTexture_.Reset();
    cudaSceneReady_ = false;
    cudaSurfaceWidth_ = 0;
    cudaSurfaceHeight_ = 0;
    cudaSuperResWidth_ = 0;
    cudaSuperResHeight_ = 0;
    ResetCudaFenceState();

    try {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        ThrowIfFailed(device->CreateCommittedResource(&heapProps,
                                                      D3D12_HEAP_FLAG_SHARED,
                                                      &desc,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr,
                                                      IID_PPV_ARGS(&cudaSharedTexture_)),
                      "Failed to create CUDA shared texture");
        D3D12_RESOURCE_DESC superResDesc = desc;
        superResDesc.Width = superResExtent.width;
        superResDesc.Height = superResExtent.height;
        ThrowIfFailed(device->CreateCommittedResource(&heapProps,
                                                      D3D12_HEAP_FLAG_SHARED,
                                                      &superResDesc,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr,
                                                      IID_PPV_ARGS(&cudaSuperResTexture_)),
                      "Failed to create CUDA SuperRes cache texture");
        ThrowIfFailed(device->CreateCommittedResource(&heapProps,
                                                      D3D12_HEAP_FLAG_SHARED,
                                                      &desc,
                                                      D3D12_RESOURCE_STATE_COMMON,
                                                      nullptr,
                                                      IID_PPV_ARGS(&cudaOriginalTexture_)),
                      "Failed to create CUDA original recording texture");

        auto surface = std::make_unique<CudaInteropSurface>(
            cudaSharedTexture_.Get(),
            cudaSuperResTexture_.Get(),
            presenter_->GetFence(),
            cudaOriginalTexture_.Get());
        if (!surface || !surface->IsValid()) {
            if (surface) {
                const std::string& err = surface->LastError();
                if (!err.empty()) {
                    qWarning() << "CUDA surface detail:" << err.c_str();
                }
            }
            cudaSharedTexture_.Reset();
            cudaSuperResTexture_.Reset();
            cudaOriginalTexture_.Reset();
            qWarning() << "CUDA surface initialization failed: surface invalid"
                       << "(requested" << width << "x" << height << ")";
            cudaPipelineAvailable_ = false;
            cudaSurfaceRetry_.RecordFailure(CudaSurfaceRetry::Clock::now());
            return false;
        }

        surface->SetSuperResPerformanceOverride(superResPerformanceOverride_);
        cudaSurface_ = std::move(surface);
        cudaSurfaceWidth_ = width;
        cudaSurfaceHeight_ = height;
        cudaSuperResWidth_ = superResExtent.width;
        cudaSuperResHeight_ = superResExtent.height;
        cudaPipelineAvailable_ = true;
        UpdateKeystoneTrackingUi();
        pipelineOrchestrator_->SetFenceInteropEnabled(
            cudaSurface_->HasExternalSemaphore());
        if (pipelineOrchestrator_->FenceInteropEnabled()) {
            const UINT64 baseValue = presenter_->GetLastSignaledFenceValue();
            pipelineOrchestrator_->ResetFence(baseValue);
            pipelineOrchestrator_->SetFenceInteropEnabled(true);
            qInfo() << "CUDA fence interop enabled; base fence value"
                    << static_cast<unsigned long long>(baseValue);
        } else {
            qInfo() << "CUDA surface ready without fence interop";
        }
        qInfo() << "CUDA surface ready for" << width << "x" << height
                << "; SuperRes cache" << superResExtent.width << "x"
                << superResExtent.height;
        cudaSurfaceRetry_.RecordSuccess();
        return true;
    } catch (...) {
        cudaSurface_.reset();
        cudaSharedTexture_.Reset();
        cudaSuperResTexture_.Reset();
        cudaOriginalTexture_.Reset();
        cudaSceneReady_ = false;
        cudaSurfaceWidth_ = 0;
        cudaSurfaceHeight_ = 0;
        cudaSuperResWidth_ = 0;
        cudaSuperResHeight_ = 0;
        cudaPipelineAvailable_ = false;
        UpdateKeystoneTrackingUi();
        ResetCudaFenceState();
        qWarning() << "CUDA surface creation exception triggered fallback";
        cudaSurfaceRetry_.RecordFailure(CudaSurfaceRetry::Clock::now());
        return false;
    }
}

bool OkuFlowApp::ProcessFrameWithCuda(UINT width, UINT height) {
    const auto& stageRaw = cpuPipeline_.StageRaw();
    if (stageRaw.empty()) {
        qWarning() << "CUDA pipeline skipped: stage raw empty";
        return false;
    }

    if (!EnsureCudaSurface(width, height)) {
        // EnsureCudaSurface reports actual initialization attempts; suppressed
        // retries must remain quiet on the per-frame passthrough path.
        usingCudaLastFrame_ = false;
        return false;
    }

    ProcessingInput input{};
    input.hostPixels = stageRaw.data();
    input.hostStrideBytes = width * 4;
    input.pixelSizeBytes = static_cast<unsigned int>(sizeof(uint32_t));
    input.width = width;
    input.height = height;
    input.publishOriginalFrame =
        recordingManager_ && recordingManager_->IsActive();
    // inputFormat 0 (BGRA): the CPU already converted and rotated, so
    // rotationQuarterTurns stays 0.

    return RunCudaPipeline(input, width, height);
}

// Feeds retained D3D11 BGRA textures or raw NV12/YUY2 camera frames straight
// to the CUDA pipeline. Conversion and rotation stay on the GPU and the
// per-frame CPU convert/rotate work is skipped entirely. Returns false
// whenever anything is off so the caller can use the CPU-converted BGRA path.
bool OkuFlowApp::TryProcessRawFrameWithCuda(MediaFrame& frame,
                                             CapturedFrame* originalFrame,
                                             CaptureGpuPending* outGpuCompletionPending) {
    if (outGpuCompletionPending) {
        *outGpuCompletionPending = CaptureGpuPending::None;
    }
    const bool isNv12 = IsEqualGUID(frame.subtype, MFVideoFormat_NV12);
    const bool isYuy2 = IsEqualGUID(frame.subtype, MFVideoFormat_YUY2);
    const bool isBgra =
        IsEqualGUID(frame.subtype, MFVideoFormat_ARGB32) ||
        IsEqualGUID(frame.subtype, MFVideoFormat_RGB32);
    if (!isNv12 && !isYuy2 &&
        !(isBgra && frame.IsGpuResident())) {
        return false;
    }

    const UINT width = frame.width;
    const UINT height = frame.height;
    if (width == 0 || height == 0 ||
        (frame.data.empty() && !frame.IsGpuResident())) {
        return false;
    }

    // Bottom-up CPU buffers require normalization, but a retained GPU texture
    // uses its own surface coordinates regardless of negotiated CPU pitch.
    if (!frame.IsGpuResident() && frame.stride < 0) {
        return false;
    }
    // Rotation happens on the GPU after conversion, so the interop surface and
    // every processing stage run at the post-rotation extent.
    const int turns = ((rotationQuarterTurns_ % 4) + 4) % 4;
    const UINT outWidth = ((turns & 1) != 0) ? height : width;
    const UINT outHeight = ((turns & 1) != 0) ? width : height;

    // Release the previous converted-texture lease only after its CUDA copy
    // event completes. Until then keep this exact next MF frame for retry;
    // neither conversion nor destination resizing may overtake that copy.
    if (cudaSurface_ && !cudaSurface_->PollCaptureCopy()) {
        if (cudaSurface_->IsFaulted()) {
            cudaPipelineAvailable_ = false;
            HandlePresenterFault();
        } else {
            if (outGpuCompletionPending) *outGpuCompletionPending = CaptureGpuPending::CudaCopy;
            ++startupLeaseRetryTicks_;
            if (recordingManager_) recordingManager_->NotifyCaptureGpuRetry();
        }
        return false;
    }

    // Startup validation has already materialized these BGRA pixels. Reuse
    // them through the existing normalized/rotated host-upload path instead
    // of converting the retained texture again and waiting on D3D11. Keep
    // this after PollCaptureCopy so a prior GPU-copy lease still guards reuse
    // and resizing. Empty-data steady-state frames retain the direct GPU rung.
    if (isBgra && !frame.data.empty()) {
        currentCaptureZeroCopyActive_ = false;
        return false;
    }

    if (!EnsureCudaSurface(outWidth, outHeight)) {
        usingCudaLastFrame_ = false;
        return false;
    }

    auto persistRung = [&](const QString& rung, const QString& reason) {
        if (captureZeroCopyStatusPersisted_ || currentCameraAccelerationKey_.isEmpty()) {
            return;
        }
        auto& acceleration =
            settingsController_->MutableSettings().cameraAcceleration[
                currentCameraAccelerationKey_];
        acceleration.lastRung = rung;
        acceleration.reason = reason;
        acceleration.decidedOn =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        SavePersistentSettings();
        captureZeroCopyStatusPersisted_ = true;
        UpdateCameraAccelerationUi();
    };

    if (frame.IsGpuResident() && captureZeroCopyAvailable_) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> cudaTexture;
        std::shared_ptr<void> cudaTextureLease;
        QElapsedTimer captureHandoffTimer;
        captureHandoffTimer.start();
        const GpuFramePreparationResult preparation =
            mediaCapture_.PrepareGpuFrameForCuda(frame, cudaTexture, cudaTextureLease);
        pipelineOrchestrator_->RecordStageSample(
            FrameTimingStage::CaptureHandoff,
            static_cast<float>(captureHandoffTimer.nsecsElapsed()) * 1e-6f);
        if (preparation == GpuFramePreparationResult::Ready && cudaTexture) {
            captureHandoffPolicy_.RecordReady();
            ProcessingInput gpuInput{};
            gpuInput.width = width;
            gpuInput.height = height;
            gpuInput.inputFormat = isNv12 ? 1 : (isYuy2 ? 2 : 0);
            gpuInput.pixelSizeBytes = sizeof(std::uint32_t);
            gpuInput.rotationQuarterTurns = turns;
            gpuInput.d3d11Texture = cudaTexture.Get();
            gpuInput.d3d11TextureLease = std::move(cudaTextureLease);
            gpuInput.d3d11Subresource = 0;
            gpuInput.publishOriginalFrame =
                recordingManager_ && recordingManager_->IsActive();
            if (RunCudaPipeline(gpuInput, outWidth, outHeight)) {
                currentCaptureZeroCopyActive_ = true;
                persistRung(
                    QStringLiteral("zeroCopy"),
                    QStringLiteral(
                        "Direct GPU camera texture transfer to CUDA is active."));
                processedFrameWidth_ = outWidth;
                processedFrameHeight_ = outHeight;
                usingCudaLastFrame_ = true;
                pendingSceneCaptureClock100ns_ = currentCameraCaptureClock100ns_;
                ++startupProcessedScenes_;
                PresentLatestCudaScene(true, originalFrame);
                return true;
            }
            if (!cudaSurface_ ||
                !cudaSurface_->LastFailureWasCaptureInterop()) {
                return false;
            }
            captureZeroCopyFailureReason_ =
                QString::fromStdString(cudaSurface_->LastError());
        } else if (preparation == GpuFramePreparationResult::Retry) {
            ++startupQueryRetryTicks_;
            // Keep this exact retained MF frame alive and retry its pending
            // D3D11 query from a later event-loop turn. Using the next frame
            // here would pair the completed conversion with the wrong capture.
            if (recordingManager_) {
                recordingManager_->NotifyCaptureGpuRetry();
            }
            if (outGpuCompletionPending) {
                *outGpuCompletionPending = CaptureGpuPending::D3D11Query;
            }
            return false;
        } else {
            captureZeroCopyFailureReason_ =
                QStringLiteral(
                    "The camera GPU texture could not be converted for CUDA.");
        }

        if (preparation != GpuFramePreparationResult::Retry) {
            captureZeroCopyAvailable_ = false;
            currentCaptureZeroCopyActive_ = false;
            persistRung(
                QStringLiteral("acceleratedCopy"),
                QStringLiteral(
                    "GPU camera capture is active with the safe copy fallback. %1")
                    .arg(captureZeroCopyFailureReason_));
            qWarning() << "Zero-copy camera rung unavailable:"
                       << captureZeroCopyFailureReason_;
        }
    }

    if (frame.data.empty() && frame.IsGpuResident()) {
        if (recordingManager_) {
            recordingManager_->NotifyCaptureSafeCopyFallback();
        }
        if (!mediaCapture_.ReadbackGpuFrame(frame)) {
            return false;
        }
    }
    if (frame.data.empty()) {
        return false;
    }
    if (!isNv12 && !isYuy2) {
        // A GPU BGRA frame that reached a lower rung is already in the
        // CPU pipeline's native format after ReadbackGpuFrame().
        return false;
    }

    UINT stride = static_cast<UINT>(frame.stride);
    const uint8_t* plane2 = nullptr;
    UINT plane2Stride = 0;
    if (isNv12) {
        stride = std::max(stride, width);
        const size_t yBytes = static_cast<size_t>(stride) * height;
        const size_t uvBytes =
            static_cast<size_t>(stride) * ((height + 1) / 2);
        if (frame.dataSize < yBytes + uvBytes) {
            return false;
        }
        plane2 = frame.data.data() + yBytes;
        plane2Stride = stride;
    } else {
        stride = std::max(stride, width * 2);
        if (frame.dataSize < static_cast<size_t>(stride) * height) {
            return false;
        }
    }

    ProcessingInput hostInput{};
    hostInput.hostPixels = frame.data.data();
    hostInput.hostStrideBytes = stride;
    hostInput.pixelSizeBytes = isNv12 ? 1u : 2u;
    hostInput.width = width;
    hostInput.height = height;
    hostInput.inputFormat = isNv12 ? 1 : 2;
    hostInput.yuvColor = frame.yuvColor;
    hostInput.hostPlane2 = plane2;
    hostInput.hostPlane2StrideBytes = plane2Stride;
    hostInput.rotationQuarterTurns = turns;
    hostInput.publishOriginalFrame =
        recordingManager_ && recordingManager_->IsActive();

    if (!RunCudaPipeline(hostInput, outWidth, outHeight)) {
        if (!rawCudaPathWarned_) {
            qWarning() << "GPU raw-format path failed; falling back to CPU conversion for"
                       << (isNv12 ? "NV12" : "YUY2") << "frames";
            rawCudaPathWarned_ = true;
        }
        return false;
    }

    currentCaptureZeroCopyActive_ = false;
    if (currentCaptureAccelerated_) {
        persistRung(
            QStringLiteral("acceleratedCopy"),
            captureZeroCopyFailureReason_.isEmpty()
                ? QStringLiteral(
                      "GPU camera capture is active with the safe copy fallback.")
                : captureZeroCopyFailureReason_);
    }
    processedFrameWidth_ = outWidth;
    processedFrameHeight_ = outHeight;
    usingCudaLastFrame_ = true;
    pendingSceneCaptureClock100ns_ = currentCameraCaptureClock100ns_;
    ++startupProcessedScenes_;
    PresentLatestCudaScene(true, originalFrame);
    return true;
}

// Shared tail of the CUDA path: builds ProcessingSettings from the live UI
// state, runs ProcessFrame with the fence dance, and presents the result.
// The interop surface must already exist at presentWidth x presentHeight.
bool OkuFlowApp::RunCudaPipeline(const ProcessingInput& input, UINT presentWidth, UINT presentHeight) {
    if (!cudaSurface_) {
        qWarning() << "CUDA pipeline disabled: surface not available";
        usingCudaLastFrame_ = false;
        return false;
    }

    ProcessingSettings settings{};
    settings.enableBlackWhite = blackWhiteEnabled_;
    settings.blackWhiteThreshold = blackWhiteThreshold_;
    // Viewport zoom is a presentation transform. Stateful CUDA stages process
    // each camera frame once at full-scene geometry.
    settings.enableZoom = false;
    // Supply viewing geometry for zoom-aware stabilization and the
    // camera-clock SuperRes ROI without applying zoom to the full scene.
    settings.zoomAmount = zoomEnabled_ && pipelineOrchestrator_->ViewportFitMode() ==
        settings::ViewportFitModeSetting::Fill ? zoomAmount_ : 1.0f;
    settings.zoomCenterX = zoomCenterX_;
    settings.zoomCenterY = zoomCenterY_;
    settings.enableBlur = blurEnabled_;
    settings.blurRadius = std::max(blurRadius_, 0);
    settings.blurSigma = blurSigma_;
    settings.drawFocusMarker = false;
    settings.enableSpatialSharpen = spatialSharpenEnabled_;
    settings.spatialUpscaler = spatialUpscaler_;
    settings.spatialSharpness = spatialSharpness_;
    settings.spatialViewportWidth = presenter_->ViewportWidth();
    settings.spatialViewportHeight = presenter_->ViewportHeight();
    settings.spatialViewTransform = ComputeViewTransform(
        presentWidth, presentHeight,
        settings.spatialViewportWidth, settings.spatialViewportHeight,
        settings.zoomAmount, settings.zoomCenterX, settings.zoomCenterY,
        pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fit
            ? ViewportFitMode::kFit : ViewportFitMode::kFill);
    settings.stagingFormat = cudaBufferFormat_;
    settings.enableTemporalSmoothing = temporalSmoothEnabled_;
    settings.temporalSmoothingAlpha = temporalSmoothAlpha_;
    settings.enableStabilization = stabilizationEnabled_;
    settings.enableBumpHold = stabilizationEnabled_ && bumpHoldEnabled_;
    if (displayColorScheme_.id == QStringLiteral("normal")) {
        settings.displayColorTransform = DisplayColorTransform::kNone;
    } else if (displayColorScheme_.id == QStringLiteral("inverted")) {
        settings.displayColorTransform = DisplayColorTransform::kInvert;
    } else {
        settings.displayColorTransform = DisplayColorTransform::kLumaLut;
    }
    settings.displayColorLut = displayColorLut_.data();
    settings.displayColorLutGeneration = displayColorLutGeneration_;
    settings.textForegroundBgra = color_schemes::TextForegroundBgra(displayColorScheme_);
    settings.textBackgroundBgra = color_schemes::TextBackgroundBgra(displayColorScheme_);
    settings.contrast = contrast_;
    settings.brightness = brightness_;
    settings.enableKeystone = keystoneEnabled_;
    settings.enableAutoContrast = autoContrastEnabled_;
    settings.autoContrastStrength = autoContrastStrength_;
    settings.enableAutoTextClarity = autoTextClarityEnabled_;
    settings.enableBackgroundFlatten = backgroundFlattenEnabled_;
    settings.backgroundFlattenStrength = backgroundFlattenStrength_;
    settings.enableAdaptiveBinarization = adaptiveBinarizationEnabled_;
    settings.sauvolaStrength = sauvolaStrength_;
    settings.binarizationSoftness = binarizationSoftness_;
    settings.textPolarityMode = textPolarityMode_;
    settings.strokeWeight = strokeWeight_;
    settings.enableSmartSharpen = smartSharpenEnabled_;
    settings.smartSharpenStrength = smartSharpenStrength_;
    settings.enableClahe = claheEnabled_;
    settings.claheClipLimit = claheClipLimit_;
    settings.enableTwoColorText = twoColorTextEnabled_;
    settings.enableTextHysteresis = textHysteresisEnabled_;
    settings.textHysteresisStrength = textHysteresisStrength_;
    settings.enableSelectiveSharpen = selectiveSharpenEnabled_;
    settings.enableFocusDetection = focusDetectionEnabled_;
    settings.focusThreshold = focusThreshold_;
    settings.enableGlareSuppression = glareSuppressionEnabled_;
    settings.glareSuppressionStrength = glareSuppressionStrength_;
    settings.enableMlSuperRes = mlTextSuperResolutionEnabled_;
    settings.mlSuperResStrength = mlTextSuperResolutionStrength_;
    settings.mlSuperResUltra1440p = mlTextSuperResolutionUltra1440p_;

    // Fence choreography is owned by FenceSequencer (S6b contract in
    // pipeline_orchestrator.hpp):
    // BeginCudaFrame() re-seeds from the presenter and reserves the CUDA
    // signal value; the reservation is committed only after ProcessFrame
    // actually enqueued the signal.
    FenceSyncParams cudaSyncParams{};
    if (pipelineOrchestrator_->FenceInteropEnabled()) {
        const FenceSequencer::CudaTicket ticket =
            pipelineOrchestrator_->Fence().BeginCudaFrame(
                presenter_->GetLastSignaledFenceValue());
        cudaSyncParams.enable = true;
        // Async readbacks copy from the shared texture on the graphics queue;
        // CUDA must not write the next frame until both the present and the
        // newest readback copy have retired (GPU-side wait only).
        cudaSyncParams.waitValue = ticket.waitValue;
        cudaSyncParams.signalValue = ticket.signalValue;
    }

    QElapsedTimer cudaSubmissionTimer;
    cudaSubmissionTimer.start();
    if (!cudaSurface_->ProcessFrame(input, settings, cudaSyncParams)) {
        pipelineOrchestrator_->RecordStageSample(
            FrameTimingStage::CudaSubmission,
            static_cast<float>(cudaSubmissionTimer.nsecsElapsed()) * 1e-6f);
        if (cudaSurface_->IsFaulted()) {
            HandlePresenterFault();
            return false;
        }
        pipelineOrchestrator_->Fence().CudaFailed();
        if (input.d3d11Texture &&
            cudaSurface_->LastFailureWasCaptureInterop()) {
            // This failure belongs only to the zero-copy top rung. Keep the
            // CUDA scene/pipeline alive so the caller can read this retained
            // texture back and retry through the accelerated-copy rung.
            usingCudaLastFrame_ = false;
            return false;
        }
        cudaPipelineAvailable_ = false;
        UpdateKeystoneTrackingUi();
        qWarning() << "CUDA pipeline processing failed, falling back to CPU";
        HandleCudaProcessingFailure();
        usingCudaLastFrame_ = false;
        cudaSceneReady_ = false;
        return false;
    }
    pipelineOrchestrator_->RecordStageSample(
        FrameTimingStage::CudaSubmission,
        static_cast<float>(cudaSubmissionTimer.nsecsElapsed()) * 1e-6f);
    pipelineOrchestrator_->ResetCudaFailures();

    if (cudaSyncParams.enable) {
        pipelineOrchestrator_->Fence().CudaSignaled();
    }

    cudaPipelineAvailable_ = true;
    UpdateKeystoneTrackingUi();
    Q_UNUSED(presentWidth);
    Q_UNUSED(presentHeight);
    usingCudaLastFrame_ = true;
    cudaSceneReady_ = true;
    pipelineOrchestrator_->MarkViewportDirty();
    return true;
}

void OkuFlowApp::PresentLatestCudaScene(bool newCameraFrame,
                                         CapturedFrame* originalFrame) {
    if (!cudaSceneReady_ || !cudaSharedTexture_ || !presenter_ ||
        !presenter_->IsInitialized() ||
        processedFrameWidth_ == 0 || processedFrameHeight_ == 0) {
        return;
    }

    const UINT viewportWidth = presenter_->ViewportWidth();
    const UINT viewportHeight = presenter_->ViewportHeight();
    ViewTransform transform =
        ComputeViewTransform(processedFrameWidth_,
                             processedFrameHeight_,
                             viewportWidth,
                             viewportHeight,
                             zoomEnabled_ ? zoomAmount_ : 1.0f,
                             zoomCenterX_,
                             zoomCenterY_,
                             pipelineOrchestrator_->ViewportFitMode() ==
                                     settings::ViewportFitModeSetting::Fit
                                 ? ViewportFitMode::kFit
                                 : ViewportFitMode::kFill);
    if (!transform.valid) {
        return;
    }
    const ViewTransform annotationTransform = transform;
    if (mainWindow_) {
        mainWindow_->setAnnotationViewTransform(annotationTransform);
    }

    ID3D12Resource* presentationTexture = cudaSharedTexture_.Get();
    UINT presentationSourceWidth = processedFrameWidth_;
    UINT presentationSourceHeight = processedFrameHeight_;
    float presentationFocusX = zoomCenterX_;
    float presentationFocusY = zoomCenterY_;
    const SuperResRoiMetadata superResRoi =
        cudaSurface_ ? cudaSurface_->SuperResRoi() : SuperResRoiMetadata{};
    const NormalizedSourceRect roiRect{
        superResRoi.sourceX,
        superResRoi.sourceY,
        superResRoi.sourceWidth,
        superResRoi.sourceHeight};
    ViewTransform roiTransform;
    bool presentingSuperRes = false;
    if (superResRoi.valid && cudaSuperResTexture_ &&
        RemapViewTransformToSourceRect(transform, roiRect, roiTransform)) {
        transform = roiTransform;
        presentationFocusX =
            (zoomCenterX_ - superResRoi.sourceX) /
            superResRoi.sourceWidth;
        presentationFocusY =
            (zoomCenterY_ - superResRoi.sourceY) /
            superResRoi.sourceHeight;
        // Spatial caches can occupy a bounded subrectangle of the shared
        // allocation. Normalize sampling against that allocation's extent.
        const float cacheFractionX = static_cast<float>(superResRoi.outputWidth) /
                                     static_cast<float>(cudaSuperResWidth_);
        const float cacheFractionY = static_cast<float>(superResRoi.outputHeight) /
                                     static_cast<float>(cudaSuperResHeight_);
        transform.sourceX *= cacheFractionX;
        transform.sourceY *= cacheFractionY;
        transform.sourceWidth *= cacheFractionX;
        transform.sourceHeight *= cacheFractionY;
        presentationFocusX *= cacheFractionX;
        presentationFocusY *= cacheFractionY;
        presentationTexture = cudaSuperResTexture_.Get();
        presentationSourceWidth = cudaSuperResWidth_;
        presentationSourceHeight = cudaSuperResHeight_;
        presentingSuperRes = cudaSurface_->IsSuperResActive();
    }
    presentationFocusX = std::clamp(presentationFocusX, 0.0f, 1.0f);
    presentationFocusY = std::clamp(presentationFocusY, 0.0f, 1.0f);

    DrainCompletedGpuReadbacks();
    if (pendingPhotoReadbackId_ != 0 &&
        pendingPhotoReadbackTimer_.isValid() &&
        pendingPhotoReadbackTimer_.elapsed() > 1000) {
        // Resize drops old-dimension presenter readbacks by contract. Retry a
        // photo whose request never returned instead of leaving capture stuck.
        pendingPhotoReadbackId_ = 0;
        pendingPhotoOriginal_ = {};
        pendingPhotoReadbackTimer_.invalidate();
        photoCapturePending_ = true;
    }
    if (pendingAnnotationReadbackId_ != 0 &&
        pendingAnnotationReadbackTimer_.isValid() &&
        pendingAnnotationReadbackTimer_.elapsed() > 1000) {
        pendingAnnotationReadbackId_ = 0;
        pendingAnnotationReadbackTimer_.invalidate();
        if (activeAnnotationCapture_) {
            annotationCaptureQueue_.push_front(
                std::move(*activeAnnotationCapture_));
            activeAnnotationCapture_.reset();
        }
        pipelineOrchestrator_->MarkViewportDirty();
    }
    if (pendingOnDemandReadbackId_ != 0 &&
        pendingOnDemandReadbackTimer_.isValid() &&
        pendingOnDemandReadbackTimer_.elapsed() > 1000) {
        pendingOnDemandReadbackId_ = 0;
        pendingOnDemandReadbackTimer_.invalidate();
        pipelineOrchestrator_->MarkViewportDirty();
    }
    if (pendingAssistantFrameReadbackId_ != 0 &&
        pendingAssistantFrameReadbackTimer_.isValid() &&
        pendingAssistantFrameReadbackTimer_.elapsed() > 1000) {
        pendingAssistantFrameReadbackId_ = 0;
        pendingAssistantFrameReadbackTimer_.invalidate();
        pipelineOrchestrator_->MarkViewportDirty();
    }
    const bool recordingActive =
        newCameraFrame && recordingManager_ && recordingManager_->IsActive();
    const bool assistiveWanted =
        newCameraFrame && assistiveManager_->WantsPeriodicReadback(debugViewEnabled_);
    const bool photoWanted =
        newCameraFrame && photoCapturePending_ &&
        originalFrame && originalFrame->HasCpuPixels();
    const bool annotationWanted =
        pendingAnnotationReadbackId_ == 0 &&
        !annotationCaptureQueue_.empty();
    const bool onDemandAssistiveWanted =
        pendingOnDemandReadbackId_ == 0 &&
        pendingOnDemandAnalysis_;
    const bool assistantFrameWanted =
        pendingAssistantFrameReadbackId_ == 0 &&
        pendingAssistantFramePrompt_.has_value();

    FenceSyncParams presentSync{};
    if (pipelineOrchestrator_->FenceInteropEnabled()) {
        presentSync.enable = true;
        presentSync.waitValue =
            pipelineOrchestrator_->Fence().LastCudaSignal();
        presentSync.signalValue =
            pipelineOrchestrator_->Fence().BeginGraphicsFrame(
                presenter_->GetLastSignaledFenceValue());
    }
    ViewportPresentationOptions presentationOptions{};
    presentationOptions.drawFocusMarker = focusMarkerEnabled_;
    presentationOptions.focusX = presentationFocusX;
    presentationOptions.focusY = presentationFocusY;
    presentationOptions.requestReadback =
        assistiveWanted || photoWanted || annotationWanted ||
        onDemandAssistiveWanted || assistantFrameWanted;
    UINT64 readbackRequestId = 0;
    QElapsedTimer presentationTimer;
    presentationTimer.start();
    const bool presented = presenter_->PresentSceneTexture(
        presentationTexture,
        presentationSourceWidth,
        presentationSourceHeight,
        transform,
        presentSync.enable ? &presentSync : nullptr,
        &presentationOptions,
        &readbackRequestId);
    pipelineOrchestrator_->RecordStageSample(
        FrameTimingStage::Presentation,
        static_cast<float>(presentationTimer.nsecsElapsed()) * 1e-6f);
    if (!presented) {
        pipelineOrchestrator_->MarkViewportDirty();
        if (presenter_->IsFaulted()) return;
    }
    if (presented && presentSync.enable) {
        // PresentSceneTexture also signals its internal frame-slot value.
        // Adopt the actual newest value after every present so viewport-only
        // draws and the next CUDA frame share one strictly monotonic timeline.
        pipelineOrchestrator_->Fence().GraphicsSignaled(
            presenter_->GetLastSignaledFenceValue());
    }
    if (presented) {
        cameraFramePresented_ = true;
        RecordStartupFirstPresent();
        if (pendingSceneCaptureClock100ns_) {
            pipelineOrchestrator_->RecordCaptureToPresentSample(
                MeasureCaptureToPresentLatency(*pendingSceneCaptureClock100ns_));
            pendingSceneCaptureClock100ns_.reset();
        }
    }
    if (readbackRequestId != 0) {
        if (photoWanted) {
            pendingPhotoReadbackId_ = readbackRequestId;
            pendingPhotoOriginal_ = *originalFrame;
            pendingPhotoReadbackTimer_.restart();
            photoCapturePending_ = false;
        }
        if (annotationWanted) {
            activeAnnotationCapture_ =
                std::move(annotationCaptureQueue_.front());
            annotationCaptureQueue_.pop_front();
            activeAnnotationCapture_->transform = annotationTransform;
            pendingAnnotationReadbackId_ = readbackRequestId;
            pendingAnnotationReadbackTimer_.restart();
        }
        if (onDemandAssistiveWanted) {
            pendingOnDemandReadbackId_ = readbackRequestId;
            pendingOnDemandReadbackTimer_.restart();
        }
        if (assistantFrameWanted) {
            pendingAssistantFrameReadbackId_ = readbackRequestId;
            pendingAssistantFrameReadbackTimer_.restart();
        }
        pipelineOrchestrator_->Fence().ReadbackObserved(
            presenter_->GetLastSignaledFenceValue());
    } else if (presentationOptions.requestReadback) {
        // The four-slot readback ring was temporarily full. Keep the viewport
        // dirty so user-triggered requests retry instead of blocking.
        pipelineOrchestrator_->MarkViewportDirty();
    }

    // Recording owns fixed GPU textures independent of the swap chain.
    // Clone both the canonical processed view and the post-conversion,
    // post-rotation original frame into unique shareable allocations. The
    // worker can feed them directly to Media Foundation without retaining a
    // camera-owned texture or reading either stream through system memory.
    if (recordingActive && originalFrame &&
        originalFrame->width > 0 && originalFrame->height > 0) {
        QElapsedTimer recordingCloneTimer;
        recordingCloneTimer.start();
        const RecordingCanvasSize canvas =
            recordingManager_->ResolveCanvas(
                originalFrame->width, originalFrame->height);
        QImage annotationLayer;
        const AnnotationOverlay* annotationOverlay =
            mainWindow_->annotationOverlay();
        if (annotationOverlay && annotationOverlay->HasInk()) {
            annotationLayer = QImage(
                static_cast<int>(canvas.width),
                static_cast<int>(canvas.height),
                QImage::Format_ARGB32);
            annotationLayer.fill(Qt::transparent);
            QPainter annotationPainter(&annotationLayer);
            // Ink is stored in full-scene coordinates, even when the video
            // texture below is sampled through a remapped SuperRes crop.
            RenderAnnotationStrokes(
                annotationPainter,
                annotationOverlay->Strokes(),
                annotationTransform,
                annotationLayer.size());
        }
        bool processedPoolExhausted = false;
        GpuVideoFrame recordingFrame =
            presenter_->RequestRecordingFrame(
                presentationTexture,
                presentationSourceWidth,
                presentationSourceHeight,
                ToRecordingTransform(transform),
                canvas.width,
                canvas.height,
                annotationLayer.isNull()
                    ? nullptr
                    : annotationLayer.constBits(),
                annotationLayer.isNull()
                    ? 0
                    : static_cast<std::size_t>(
                          annotationLayer.bytesPerLine()),
                &processedPoolExhausted,
                presentSync.enable ? presentSync.waitValue : 0);
        RecordingViewTransform originalTransform;
        originalTransform.valid = true;
        GpuVideoFrame originalRecordingFrame;
        bool originalPoolExhausted = false;
        if (cudaOriginalTexture_) {
            originalRecordingFrame =
                presenter_->RequestRecordingFrame(
                    cudaOriginalTexture_.Get(),
                    originalFrame->width,
                    originalFrame->height,
                    originalTransform,
                    originalFrame->width,
                    originalFrame->height,
                    nullptr,
                    0,
                    &originalPoolExhausted,
                    presentSync.enable ? presentSync.waitValue : 0);
        }
        if (pipelineOrchestrator_->FenceInteropEnabled()) {
            pipelineOrchestrator_->Fence().GraphicsSignaled(
                presenter_->GetLastSignaledFenceValue());
        } else if (!presenter_->WaitForIdle()) {
            // Without a shared semaphore the next CUDA frame cannot wait on
            // these recording readers. Completion must precede source reuse.
            return;
        }
        if (recordingFrame.IsValid() &&
            (originalRecordingFrame.IsValid() ||
             originalFrame->HasCpuPixels())) {
            originalFrame->gpuScene = std::move(originalRecordingFrame);
            recordingManager_->AddGpuSceneFrame(
                std::move(recordingFrame),
                ToRecordingTransform(transform),
                std::move(*originalFrame));
        } else {
            if (processedPoolExhausted || originalPoolExhausted) {
                recordingManager_->NotifyRecordingPoolSkipped();
            } else {
                recordingManager_->NotifyReadbackSkipped();
            }
        }
        pipelineOrchestrator_->RecordStageSample(
            FrameTimingStage::RecordingClone,
            static_cast<float>(recordingCloneTimer.nsecsElapsed()) * 1e-6f);
    }
    if (presented) {
        pipelineOrchestrator_->MarkViewportPresented();
        superResPresentedLastFrame_ = presentingSuperRes;
        if (presentationOptions.requestReadback && readbackRequestId == 0)
            pipelineOrchestrator_->MarkViewportDirty();
    }

    if (newCameraFrame) {
        UpdateProcessingStatusLabel();
    }
}

// Drain completed asynchronous viewport copies for photos and assistive
// analysis. Processed recording now uses a fenced GPU canvas and does not
// consume this CPU readback ring.
void OkuFlowApp::DrainCompletedGpuReadbacks() {
    if (!presenter_) {
        return;
    }

    UINT readbackWidth = 0;
    UINT readbackHeight = 0;
    UINT64 requestId = 0;
    while (presenter_->TryGetCompletedReadback(asyncReadbackBuffer_,
                                               readbackWidth,
                                               readbackHeight,
                                               &requestId)) {
        if (requestId == pendingPhotoReadbackId_ &&
            pendingPhotoOriginal_.HasCpuPixels()) {
            SaveCapturedPhotoPair(asyncReadbackBuffer_.data(),
                                  readbackWidth,
                                  readbackHeight,
                                  pendingPhotoOriginal_);
            pendingPhotoReadbackId_ = 0;
            pendingPhotoOriginal_ = {};
            pendingPhotoReadbackTimer_.invalidate();
        }
        if (requestId == pendingAnnotationReadbackId_ &&
            activeAnnotationCapture_) {
            SaveAnnotationSnapshot(asyncReadbackBuffer_.data(),
                                   readbackWidth,
                                   readbackHeight,
                                   activeAnnotationCapture_->strokes,
                                   activeAnnotationCapture_->transform,
                                   activeAnnotationCapture_->heading);
            pendingAnnotationReadbackId_ = 0;
            pendingAnnotationReadbackTimer_.invalidate();
            activeAnnotationCapture_.reset();
            if (!annotationCaptureQueue_.empty()) {
                pipelineOrchestrator_->MarkViewportDirty();
            }
        }
        if (requestId == pendingOnDemandReadbackId_) {
            const bool readText = pendingOnDemandReadText_;
            pendingOnDemandAnalysis_ = false;
            pendingOnDemandReadText_ = false;
            pendingOnDemandReadbackId_ = 0;
            pendingOnDemandReadbackTimer_.invalidate();
            assistiveManager_->Runtime().SubmitFrameForced(
                asyncReadbackBuffer_.data(),
                static_cast<int>(readbackWidth),
                static_cast<int>(readbackHeight),
                readText);
        }
        if (requestId == pendingAssistantFrameReadbackId_ &&
            pendingAssistantFramePrompt_) {
            const PendingAssistantFramePrompt prompt =
                std::move(*pendingAssistantFramePrompt_);
            pendingAssistantFramePrompt_.reset();
            pendingAssistantFrameReadbackId_ = 0;
            pendingAssistantFrameReadbackTimer_.invalidate();
            DispatchAssistantPrompt(
                prompt.prompt,
                prompt.clearAdvancedEditor,
                asyncReadbackBuffer_.data(),
                static_cast<int>(readbackWidth),
                static_cast<int>(readbackHeight),
                true);
        }
        if (recordingManager_) {
            recordingManager_->HandleProcessedReadback(
                requestId,
                asyncReadbackBuffer_.data(),
                readbackWidth,
                readbackHeight);
        }
        const bool focusGateEnabled = focusDetectionEnabled_ || autoTextClarityEnabled_;
        const bool focusAcceptable =
            !focusGateEnabled || !cudaSurface_ ||
            cudaSurface_->IsFocusAcceptable(focusThreshold_);
        assistiveManager_->MaybeRequestAnalysis(asyncReadbackBuffer_.data(),
                                                 readbackWidth,
                                                 readbackHeight,
                                                 debugViewEnabled_,
                                                 focusGateEnabled,
                                                 focusAcceptable);
    }
}

bool OkuFlowApp::StartCameraCapture(size_t index,
                                     bool interactive,
                                     bool forceCompatibility,
                                     bool backgroundStartup) {
    if (index >= cameras_.size() || cameraStartupPending_ ||
        (!cameraActive_ && startupWorkers_ && startupWorkers_->active.load() != 0)) {
        return false;
    }

    selectedCameraIndex_ = static_cast<int>(index);
    StopCameraCapture();
    if (mediaCapture_.WasAbandoned()) {
        ShowStatusMessage(QStringLiteral(
            "Camera did not stop in time. Restart OkuFlow before changing cameras."), 15000);
        return false;
    }
    const uint64_t captureSession = cameraSessionId_;
    cpuPipeline_.ResetTemporalHistory();
    if (cudaSurface_ && !cudaSurface_->IsFaulted()) {
        cudaSurface_->ResetTemporalHistory();
        cudaSurface_->ResetStabilization();
        cudaSurface_->ResetKeystone();
        cudaSurface_->ResetTextClarityHistory();
    }
    UpdateKeystoneTrackingUi();

    const CameraDescriptor& descriptor = cameras_[index];
    auto& persistent = settingsController_->MutableSettings();
    currentCameraAccelerationKey_ =
        QString::fromStdWString(descriptor.symbolicLink);
    settings::CameraAccelerationSetting& acceleration =
        persistent.cameraAcceleration[currentCameraAccelerationKey_];
    if (persistent.legacyWiderCameraCompatibility) {
        acceleration.mode =
            settings::CameraAccelerationMode::ForceCompatibility;
        acceleration.automaticFallback = false;
        acceleration.reason =
            QStringLiteral(
                "Compatibility mode was migrated from the former global "
                "camera-compatibility option.");
        acceleration.decidedOn =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        persistent.legacyWiderCameraCompatibility = false;
        SavePersistentSettings();
    }
    if (persistent.cameraAccelerationAttempt ==
        currentCameraAccelerationKey_) {
        acceleration.automaticFallback = true;
        acceleration.reason =
            QStringLiteral(
                "The previous accelerated camera start did not finish. "
                "Compatibility mode was selected automatically.");
        acceleration.decidedOn =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        persistent.cameraAccelerationAttempt.clear();
        forceCompatibility = true;
    }
    const bool explicitlyCompatible =
        acceleration.mode ==
        settings::CameraAccelerationMode::ForceCompatibility;
    const bool automaticFallback =
        acceleration.mode ==
            settings::CameraAccelerationMode::Automatic &&
        acceleration.automaticFallback;
    const bool requestAcceleration =
        !forceCompatibility &&
        !explicitlyCompatible && !automaticFallback;
    const QString requestedStableId =
        settingsController_->MutableSettings().cameraFormatStableId;
    cameraIngress_ = std::make_shared<CameraIngress>();
    cameraIngress_->receiver = this;
    cameraIngress_->accepting = true;
    cameraIngress_->retainBurst = recordingManager_ && recordingManager_->IsActive();
    const auto ingress = cameraIngress_;
    FrameCallback callback = [ingress, captureSession](MediaFrame&& frame) {
        // Release dropped sample leases outside the ingress lock. A slow COM
        // release on this worker must not prevent the UI from canceling it.
        std::deque<MediaFrame> retired;
        {
            std::scoped_lock lock(ingress->mutex);
            if (!ingress->accepting) return;
            ++ingress->received;
            if (ingress->profileReceived++ == 0)
                ingress->firstArrivalMs = GetTickCount64();
            else {
                const auto interval = GetTickCount64() - ingress->lastArrivalMs;
                if (interval < 8) ++ingress->shortArrivalIntervals;
                if (interval > 45) ++ingress->longArrivalIntervals;
                ingress->maxArrivalIntervalMs = std::max(ingress->maxArrivalIntervalMs, interval);
            }
            ingress->lastArrivalMs = GetTickCount64();
            if (!ingress->retainBurst) {
                if (!ingress->frames.empty()) {
                    ++ingress->dropped;
                    ++ingress->profileDropped;
                }
                retired.swap(ingress->frames);
            } else if (ingress->frames.size() >=
                       kMaxRecordingCameraFrames) {
                retired.push_back(std::move(ingress->frames.front()));
                ingress->frames.pop_front();
                ++ingress->dropped;
                ++ingress->profileDropped;
            }
            ingress->frames.push_back(std::move(frame));
            if (!ingress->wakeQueued && ingress->receiver) {
                ingress->wakeQueued = true;
                auto* app = ingress->receiver;
                QMetaObject::invokeMethod(app, [app, ingress, captureSession] {
                    if (app->cameraSessionId_ != captureSession || app->cameraIngress_ != ingress) return;
                    std::uint64_t received = 0, dropped = 0;
                    {
                        std::scoped_lock lock(ingress->mutex);
                        ingress->wakeQueued = false;
                        received = std::exchange(ingress->received, 0);
                        dropped = std::exchange(ingress->dropped, 0);
                        ingress->retainBurst = app->recordingManager_ && app->recordingManager_->IsActive();
                    }
                    if (app->recordingManager_) {
                        for (std::uint64_t i = 0; i < received; ++i)
                            app->recordingManager_->NotifyCaptureFrame(i < dropped);
                    }
                    if (app->pipelineOrchestrator_)
                        app->pipelineOrchestrator_->NotifyCameraFrameAvailable();
                }, Qt::QueuedConnection);
            }
        }
    };
    CaptureErrorCallback errorCallback = [ingress, captureSession](const std::string& detail) {
        const QString message = QString::fromStdString(detail);
        std::scoped_lock lock(ingress->mutex);
        if (!ingress->accepting || !ingress->receiver) return;
        auto* app = ingress->receiver;
        QMetaObject::invokeMethod(app,
                                  [app, captureSession, message]() {
                                      app->HandleCameraRuntimeFailure(captureSession, message);
                                  },
                                  Qt::QueuedConnection);
    };

    if (backgroundStartup) {
        persistent.cameraAccelerationAttempt = requestAcceleration
            ? currentCameraAccelerationKey_ : QString();
        SavePersistentSettings();
        QueueInitialCameraStart(descriptor, requestedStableId, callback, errorCallback,
                                requestAcceleration, interactive, captureSession);
        return true; // Accepted; readiness is reported after worker adoption.
    }

    auto startWithMode = [&](CaptureAccelerationMode mode) {
        return mediaCapture_.StartCapture(
            descriptor,
            nullptr,
            callback,
            mode == CaptureAccelerationMode::Accelerated
                ? MFVideoFormat_ARGB32
                : MFVideoFormat_NV12,
            errorCallback,
            mode,
            requestedStableId.toStdWString());
    };

    bool started = false;
    if (requestAcceleration) {
        persistent.cameraAccelerationAttempt =
            currentCameraAccelerationKey_;
        SavePersistentSettings();
        started = startWithMode(CaptureAccelerationMode::Accelerated);
        if (!started) {
            const QString acceleratedError =
                QString::fromStdString(mediaCapture_.LastError());
            const CameraFailureKind acceleratedFailure =
                mediaCapture_.LastFailureKind();
            persistent.cameraAccelerationAttempt.clear();
            const bool transientDeviceFailure =
                acceleratedFailure == CameraFailureKind::DeviceBusy ||
                acceleratedFailure == CameraFailureKind::DeviceMissing;
            if (!transientDeviceFailure) {
                acceleration.reason =
                    acceleratedError.isEmpty()
                        ? QStringLiteral(
                              "GPU camera acceleration could not start; using "
                              "compatibility mode.")
                        : QStringLiteral(
                              "GPU camera acceleration could not start: %1")
                              .arg(acceleratedError);
                acceleration.decidedOn =
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                if (acceleration.mode ==
                    settings::CameraAccelerationMode::Automatic) {
                    acceleration.automaticFallback = true;
                }
            }
            SavePersistentSettings();
            if (!transientDeviceFailure) {
                qWarning() << acceleration.reason;
                started =
                    startWithMode(CaptureAccelerationMode::Compatibility);
            }
        }
    } else {
        persistent.cameraAccelerationAttempt.clear();
        started =
            startWithMode(CaptureAccelerationMode::Compatibility);
    }

    return CompleteCameraCaptureStart(started, interactive);
}

bool OkuFlowApp::CompleteCameraCaptureStart(bool started, bool interactive, const QString& startupError) {
    if (!started) {
        const std::string detail = startupError.isEmpty()
                                      ? mediaCapture_.LastError()
                                      : startupError.toStdString();
        const CameraFailureKind kind = mediaCapture_.LastFailureKind();
        QString message;
        if (!detail.empty() && (kind == CameraFailureKind::DeviceBusy ||
                                kind == CameraFailureKind::DeviceMissing ||
                                kind == CameraFailureKind::AccessDenied)) {
            // LastError() is already a full plain-language sentence for these
            // failure kinds; show it verbatim.
            message = QString::fromStdString(detail);
        } else {
            message = QStringLiteral("Failed to start camera capture");
            if (!detail.empty()) {
                message += QStringLiteral(" (%1)").arg(QString::fromStdString(detail));
            }
        }
        if (interactive &&
            (kind == CameraFailureKind::DeviceBusy ||
             kind == CameraFailureKind::DeviceMissing)) {
            qWarning() << "Camera start delayed:" << message;
            lastCameraError_ = message;
            StopCameraCapture();
            UpdateProcessingStatusLabel();
            ShowStatusMessage(
                QStringLiteral("%1 OkuFlow will keep trying without "
                               "blocking the controls.")
                    .arg(message),
                12000,
                LivePoliteness::kAssertive);
            BeginCameraReconnect();
        } else if (interactive) {
            HandleCameraStartFailure(message);
        } else {
            qWarning() << "Camera start failed (silent):" << message;
            lastCameraError_ = message;
            UpdateProcessingStatusLabel();
        }
        currentCaptureAccelerated_ = false;
        UpdateCameraAccelerationUi();
        return false;
    }

    processedFrameWidth_ = 0;
    processedFrameHeight_ = 0;
    cpuSceneBuffer_.clear();
    cpuSceneWidth_ = 0;
    cpuSceneHeight_ = 0;
    cpuSceneReady_ = false;
    cameraActive_ = true;
    cameraFramePresented_ = false;
    pendingSceneCaptureClock100ns_.reset();
    currentCameraCaptureClock100ns_ = -1;
    currentCaptureAccelerated_ =
        mediaCapture_.AccelerationMode() ==
        CaptureAccelerationMode::Accelerated;
    currentCaptureZeroCopyActive_ = false;
    const bool forceCopyRung =
        qEnvironmentVariableIntValue(
            "OKUFLOW_FORCE_CAPTURE_COPY_RUNG") != 0;
    captureZeroCopyAvailable_ =
        currentCaptureAccelerated_ && !forceCopyRung;
    captureHandoffPolicy_.Reset();
    captureZeroCopyStatusPersisted_ = false;
    captureZeroCopyFailureReason_ =
        forceCopyRung
            ? QStringLiteral(
                  "The direct GPU rung was disabled by the diagnostic "
                  "OKUFLOW_FORCE_CAPTURE_COPY_RUNG setting.")
            : QString{};
    if (!currentCaptureAccelerated_ &&
        !currentCameraAccelerationKey_.isEmpty()) {
        auto& compatibility =
            settingsController_->MutableSettings().cameraAcceleration[
                currentCameraAccelerationKey_];
        compatibility.lastRung = QStringLiteral("compatibility");
        compatibility.decidedOn =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        SavePersistentSettings();
    }
    RefreshCameraFormats(static_cast<size_t>(selectedCameraIndex_));
    if (uiState_->cameraFormatNoticeLabel_) {
        const QString notice =
            QString::fromStdString(mediaCapture_.FormatNotice());
        SetLiveText(uiState_->cameraFormatNoticeLabel_, notice,
                    notice.isEmpty() ? LivePoliteness::kSilent
                                     : LivePoliteness::kPolite,
                    QStringLiteral("Camera format notice"));
        uiState_->cameraFormatNoticeLabel_->setVisible(!notice.isEmpty());
    }
    lastCameraError_.clear();
    if (startupCameraReadyMs_ < 0) {
        startupCameraReadyMs_ = startupTimer_.elapsed();
        qInfo() << "Startup timing: camera ready" << startupCameraReadyMs_ << "ms";
    }
    UpdateKeystoneTrackingUi();
    UpdateCameraAccelerationUi();
    UpdateProcessingStatusLabel();
    return true;
}

void OkuFlowApp::StopCameraCapture(bool atProcessExit) {
    pendingSceneCaptureClock100ns_.reset();
    currentCameraCaptureClock100ns_ = -1;
    const bool startupWasPending = cameraStartupPending_;
    cameraStartupPending_ = false;
    pendingCameraStartupError_.clear();
    if (!atProcessExit && uiState_) {
        if (uiState_->cameraCombo_) uiState_->cameraCombo_->setEnabled(true);
        if (uiState_->cameraFormatCombo_) uiState_->cameraFormatCombo_->setEnabled(true);
        UpdateCameraAccelerationUi();
    }
    ++cameraSessionId_;
    const auto retiredIngress = std::move(cameraIngress_);
    if (retiredIngress) {
        std::scoped_lock lock(retiredIngress->mutex);
        retiredIngress->accepting = false;
        retiredIngress->receiver = nullptr;
    }
    const bool captureStopped = mediaCapture_.StopCapture([this, atProcessExit](bool quiesced) {
        if (cudaSurface_) {
            const bool producerSafe = quiesced &&
                (!presenter_ || !presenter_->IsFaulted());
            const bool consumerSafe = producerSafe && cudaSurface_->WaitForIdle();
            if (consumerSafe) cudaSurface_->ResetCaptureInterop(atProcessExit);
            else cudaSurface_->AbandonCaptureInterop();
        }
    });
    if (!captureStopped && retiredIngress) {
        // A timed-out driver may still reference any queued sample. Keep the
        // disconnected ingress alive until process exit without holding a UI
        // pointer, even if its last callback eventually returns.
        retiredIngress->retainedAfterTimeout = retiredIngress;
        if (deferredGpuCameraFrame_) {
            std::scoped_lock lock(retiredIngress->mutex);
            retiredIngress->frames.push_back(std::move(*deferredGpuCameraFrame_));
        }
    }
    if (!captureStopped && !atProcessExit) {
        ShowStatusMessage(QStringLiteral(
            "Camera did not stop in time. Restart OkuFlow before changing cameras."), 15000);
    }
    if (captureStopped && !startupWasPending && settingsController_ &&
        settingsController_->MutableSettings().cameraAccelerationAttempt ==
            currentCameraAccelerationKey_) {
        // Reaching this point means the capture thread and driver stopped
        // cleanly. A process/driver hang never reaches this clear, so the
        // persisted marker still identifies an incomplete accelerated start
        // on the next launch.
        settingsController_->MutableSettings()
            .cameraAccelerationAttempt.clear();
        if (uiState_ && assistiveManager_) {
            SavePersistentSettings();
        }
    }
    cameraActive_ = false;
    cameraFramePresented_ = false;
    currentCaptureAccelerated_ = false;
    currentCaptureZeroCopyActive_ = false;
    captureZeroCopyAvailable_ = true;
    captureHandoffPolicy_.Reset();
    captureZeroCopyStatusPersisted_ = false;
    captureZeroCopyFailureReason_.clear();

    deferredGpuCameraFrame_.reset();
    deferredGpuSequence_.reset();
    deferredGpuWaitTimer_.invalidate();

    cpuPipeline_.ResetTemporalHistory();
    if (cudaSurface_ && !cudaSurface_->IsFaulted()) {
        cudaSurface_->ResetTemporalHistory();
        cudaSurface_->ResetStabilization();
        cudaSurface_->ResetKeystone();
        cudaSurface_->ResetTextClarityHistory();
    }
    UpdateKeystoneTrackingUi();
    processedFrameWidth_ = 0;
    processedFrameHeight_ = 0;
    cpuSceneBuffer_.clear();
    cpuSceneWidth_ = 0;
    cpuSceneHeight_ = 0;
    cpuSceneReady_ = false;
    UpdateProcessingStatusLabel();
}

void OkuFlowApp::HandleCameraStartFailure(const QString& message) {
    qWarning() << "Camera start failed:" << message;
    lastCameraError_ = message;
    StopCameraCapture();
    UpdateProcessingStatusLabel();
    if (mainWindow_) {
        QMessageBox::warning(mainWindow_.get(), QStringLiteral("Camera Error"), message);
    }
}

void OkuFlowApp::HandleCameraRuntimeFailure(uint64_t captureSession, const QString& message) {
    if (captureSession == cameraSessionId_ && cameraStartupPending_) {
        pendingCameraStartupError_ = message;
        return;
    }
    // Never surface a modal dialog while the automatic reconnect is running:
    // popping one up mid-lecture would steal focus from the student.
    if (pipelineOrchestrator_->IsCameraReconnectPending()) {
        return;
    }
    if (captureSession != cameraSessionId_ || !cameraActive_) {
        return;
    }

    qWarning() << "Camera runtime failure:" << message;
    lastCameraError_ = message;

    if (currentCaptureAccelerated_ &&
        mediaCapture_.ConsumeAccelerationRejected()) {
        auto& persistent = settingsController_->MutableSettings();
        settings::CameraAccelerationSetting& acceleration =
            persistent.cameraAcceleration[currentCameraAccelerationKey_];
        acceleration.reason =
            QStringLiteral(
                "GPU camera acceleration did not produce a usable startup "
                "image, so OkuFlow switched to compatibility mode.");
        acceleration.decidedOn =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (acceleration.mode ==
            settings::CameraAccelerationMode::Automatic) {
            acceleration.automaticFallback = true;
        }
        persistent.cameraAccelerationAttempt.clear();
        const int cameraIndex = selectedCameraIndex_;
        SavePersistentSettings();
        StopCameraCapture();
        if (cameraIndex >= 0 &&
            StartCameraCapture(
                static_cast<size_t>(cameraIndex), false, true)) {
            ShowStatusMessage(
                QStringLiteral(
                    "Camera switched to compatibility mode because the "
                    "accelerated startup image was not usable."),
                10000);
            return;
        }
    }

    // Mid-stream device loss is handled by the reconnect state machine instead
    // of an error dialog; the flag is also polled from OnFrameTick in case the
    // tick sees it first.
    if (mediaCapture_.ConsumeDeviceLost()) {
        BeginCameraReconnect();
        return;
    }

    StopCameraCapture();
    UpdateProcessingStatusLabel();
    if (mainWindow_) {
        QMessageBox::warning(mainWindow_.get(), QStringLiteral("Camera Error"), message);
    }
}

// Camera reconnect state machine. Entered on mid-stream device loss; driven
// from OnFrameTick with QDateTime-based backoff (2s/4s/8s), no blocking
// sleeps, no modal dialogs. Gives up after ~30 seconds.
void OkuFlowApp::BeginCameraReconnect() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!pipelineOrchestrator_->BeginCameraReconnect(now)) {
        return;
    }
    qWarning() << "Camera connection lost; reconnecting automatically";
    lastCameraError_ = QStringLiteral("Reconnecting to camera…");
    StopCameraCapture();
    UpdateProcessingStatusLabel();
}

void OkuFlowApp::DriveCameraReconnect() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!pipelineOrchestrator_->CameraReconnectDue(now)) {
        return;
    }

    // Re-enumerate and look for the same physical device again.
    const std::wstring targetLink = mediaCapture_.LastSymbolicLink();
    cameras_ = mediaCapture_.EnumerateCameras();
    {
        auto blocker = uiState_->BlockSignals(uiState_->cameraCombo_);
        PopulateCameraCombo();
    }

    int matchIndex = -1;
    if (!targetLink.empty()) {
        for (size_t i = 0; i < cameras_.size(); ++i) {
            if (cameras_[i].symbolicLink == targetLink) {
                matchIndex = static_cast<int>(i);
                break;
            }
        }
    }

    if (matchIndex >= 0 && StartCameraCapture(static_cast<size_t>(matchIndex), false)) {
        const int attempts =
            pipelineOrchestrator_->CameraReconnectAttempt() + 1;
        pipelineOrchestrator_->CancelCameraReconnect();
        qInfo() << "Camera reconnected after" << attempts << "attempt(s)";
        if (uiState_->cameraCombo_) {
            auto blocker = uiState_->BlockSignals(uiState_->cameraCombo_);
            uiState_->cameraCombo_->setCurrentIndex(matchIndex);
        }
        RefreshCameraFormats(static_cast<size_t>(matchIndex));
        ShowStatusMessage(QStringLiteral("Camera reconnected."), 5000);
        return;
    }

    pipelineOrchestrator_->ScheduleCameraReconnectRetry(now);
    if (pipelineOrchestrator_->CameraReconnectExpired(now)) {
        pipelineOrchestrator_->CancelCameraReconnect();
        const std::string detail = mediaCapture_.LastError();
        lastCameraError_ = !detail.empty()
            ? QString::fromStdString(detail)
            : QStringLiteral("The camera did not come back. Check the connection, then pick it "
                             "again from the camera list.");
        qWarning() << "Camera reconnect gave up:" << lastCameraError_;
        ShowStatusMessage(lastCameraError_, 15000,
                          LivePoliteness::kAssertive);
        return;
    }

    // The orchestrator schedules 2s/4s/8s retry backoff.
}



bool OkuFlowApp::RunFrameTick(double elapsedSeconds) {
    if (HandlePresenterFault()) return false;
    // Camera loss / reconnect state machine. ConsumeDeviceLost() is polled
    // here in addition to the capture error callback so the reconnect starts
    // no matter which side notices the loss first.
    if (mediaCapture_.ConsumeDeviceLost() &&
        !pipelineOrchestrator_->IsCameraReconnectPending()) {
        BeginCameraReconnect();
    }
    if (pipelineOrchestrator_->IsCameraReconnectPending()) {
        DriveCameraReconnect();
    }

    if (currentCaptureAccelerated_ &&
        mediaCapture_.ConsumeAccelerationValidated()) {
        auto& persistent = settingsController_->MutableSettings();
        persistent.cameraAccelerationAttempt.clear();
        settings::CameraAccelerationSetting& acceleration =
            persistent.cameraAcceleration[currentCameraAccelerationKey_];
        acceleration.automaticFallback = false;
        acceleration.reason =
            QStringLiteral("GPU-accelerated camera capture passed startup validation.");
        acceleration.decidedOn =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        SavePersistentSettings();
        UpdateCameraAccelerationUi();
        qInfo() << "Camera acceleration validated for"
                << currentCameraAccelerationKey_;
    }

    if (!cameraActive_ || !cameraIngress_) {
        return false;
    }

    if (ApplyInputForces(elapsedSeconds)) {
        pipelineOrchestrator_->MarkViewportDirty();
    }
    MediaFrame frame;
    {
        std::scoped_lock lock(cameraIngress_->mutex);
        cameraIngress_->retainBurst = recordingManager_ && recordingManager_->IsActive();
        if (deferredGpuCameraFrame_) {
            frame = std::move(*deferredGpuCameraFrame_);
            deferredGpuCameraFrame_.reset();
        } else if (!cameraIngress_->frames.empty()) {
            frame = std::move(cameraIngress_->frames.front());
            cameraIngress_->frames.pop_front();
        }
    }
    const auto scheduleNextCameraFrame = qScopeGuard([this]() {
        bool hasPendingFrame = false;
        {
            if (!cameraIngress_) return;
            std::scoped_lock lock(cameraIngress_->mutex);
            hasPendingFrame =
                !deferredGpuCameraFrame_ &&
                !cameraIngress_->frames.empty();
        }
        if (hasPendingFrame && pipelineOrchestrator_) {
            pipelineOrchestrator_->NotifyCameraFrameAvailable();
        }
    });

    if ((frame.data.empty() && !frame.IsGpuResident()) ||
        frame.width == 0 || frame.height == 0) {
        if (usingCudaLastFrame_ && cudaSceneReady_ &&
            (pipelineOrchestrator_->IsViewportDirty() ||
             presenter_->NeedsScenePresent())) {
            PresentLatestCudaScene(false, nullptr);
        } else if (!usingCudaLastFrame_ && cpuSceneReady_ &&
                   (pipelineOrchestrator_->IsViewportDirty() ||
                    presenter_->NeedsScenePresent())) {
            PresentFitted(
                cpuSceneBuffer_.data(),
                cpuSceneWidth_,
                cpuSceneHeight_,
                pipelineOrchestrator_->ViewportFitMode() ==
                    settings::ViewportFitModeSetting::Fill,
                zoomCenterX_,
                zoomCenterY_,
                nullptr);
        }
        return false;
    }

    currentCameraCaptureClock100ns_ = frame.captureClock100ns;
    CapturedFrame originalFrame;
    bool recordingActive = recordingManager_ && recordingManager_->IsActive();
    bool cpuOriginalRequired = photoCapturePending_;
    if (cpuOriginalRequired &&
        frame.data.empty() && frame.IsGpuResident() &&
        !mediaCapture_.ReadbackGpuFrame(frame)) {
        if (photoCapturePending_) {
            ShowStatusMessage(
                QStringLiteral(
                    "Photo not saved: the original camera frame could not be read back."),
                8000);
            photoCapturePending_ = false;
            cpuOriginalRequired = false;
        }
    }
    if (cpuOriginalRequired) {
        if (!PrepareOriginalFrame(frame, originalFrame) && recordingActive) {
            recordingManager_->Stop(QStringLiteral(
                "Recording stopped: the original camera frame could not be converted."));
            recordingActive = false;
        }
    } else if (recordingActive) {
        PopulateOriginalFrameMetadata(frame, originalFrame);
    }

    // GPU fast path: NV12/YUY2 frames go straight to CUDA (conversion and
    // rotation on the GPU), skipping the per-pixel CPU work below. The CPU
    // path remains for the debug view, other subtypes, GPU-unavailable
    // passthrough, and any frame the raw path rejects.
    if (!debugViewEnabled_) {
        CaptureGpuPending gpuCompletionPending = CaptureGpuPending::None;
        if (TryProcessRawFrameWithCuda(
                frame, &originalFrame, &gpuCompletionPending)) {
            if (deferredGpuWaitTimer_.isValid())
                startupRetryMaxMs_ = std::max(startupRetryMaxMs_, deferredGpuWaitTimer_.elapsed());
            deferredGpuSequence_.reset();
            deferredGpuWaitTimer_.invalidate();
            return true;
        }
        if (HandlePresenterFault()) return false;
        if (gpuCompletionPending != CaptureGpuPending::None) {
            if (!deferredGpuSequence_ ||
                *deferredGpuSequence_ != frame.sequenceNumber) {
                deferredGpuSequence_ = frame.sequenceNumber;
                deferredGpuWaitTimer_.restart();
            }
            // A healthy conversion normally completes on the first retry.
            // Keep the wait bounded so a wedged driver can still fall back to
            // a safe copy for this frame instead of freezing preview.
            constexpr qint64 kGpuCompletionRetryBudgetMs = 25;
            startupRetryMaxMs_ = std::max(startupRetryMaxMs_, deferredGpuWaitTimer_.elapsed());
            if (gpuCompletionPending == CaptureGpuPending::CudaCopy ||
                deferredGpuWaitTimer_.elapsed() <
                kGpuCompletionRetryBudgetMs) {
                {
                    deferredGpuCameraFrame_ = std::move(frame);
                }
                pipelineOrchestrator_->NotifyCameraFrameAvailable(1);
                return false;
            }

            if (recordingManager_) {
                recordingManager_->NotifyCaptureSafeCopyFallback();
            }
            ++startupRetryExpired_;
            if (captureHandoffPolicy_.RecordDeadline(gpuCompletionPending)) {
                // Do not clear the producer query or its allocations here.
                // Safe-copy and eventual teardown keep their real GPU waits.
                captureZeroCopyAvailable_ = false;
                currentCaptureZeroCopyActive_ = false;
                captureZeroCopyFailureReason_ = QStringLiteral(
                    "GPU camera capture is active with the safe copy fallback.");
                if (!currentCameraAccelerationKey_.isEmpty()) {
                    auto& acceleration = settingsController_->MutableSettings()
                        .cameraAcceleration[currentCameraAccelerationKey_];
                    acceleration.lastRung = QStringLiteral("acceleratedCopy");
                    acceleration.reason = captureZeroCopyFailureReason_;
                    acceleration.decidedOn = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                    SavePersistentSettings();
                    captureZeroCopyStatusPersisted_ = true;
                }
                qWarning() << "Direct camera handoff disabled for this session after three consecutive D3D11 deadlines";
                UpdateCameraAccelerationUi();
            }
            deferredGpuSequence_.reset();
            deferredGpuWaitTimer_.invalidate();
        }
    }

    QElapsedTimer cpuPreparationTimer;
    cpuPreparationTimer.start();
    if (frame.data.empty() && frame.IsGpuResident() &&
        !mediaCapture_.ReadbackGpuFrame(frame)) {
        return true;
    }
    if (recordingActive && !originalFrame.HasCpuPixels() &&
        !PrepareOriginalFrame(frame, originalFrame)) {
        recordingManager_->Stop(QStringLiteral(
            "Recording stopped: the original camera frame could not be converted."));
        recordingActive = false;
    }
    if (!cpuPipeline_.ConvertFrameToBgra(frame.data,
                                         frame.subtype,
                                         frame.width,
                                         frame.height,
                                         frame.stride,
                                         frame.dataSize,
                                         frame.yuvColor)) {
        return true;
    }

    UINT width = frame.width;
    UINT height = frame.height;
    cpuPipeline_.RotateRawBuffer(rotationQuarterTurns_, width, height);
    pipelineOrchestrator_->RecordStageSample(
        FrameTimingStage::CpuPreparation,
        static_cast<float>(cpuPreparationTimer.nsecsElapsed()) * 1e-6f);
    processedFrameWidth_ = width;
    processedFrameHeight_ = height;

    BuildCompositeAndPresent(width, height, &originalFrame);
    if (photoCapturePending_ && !usingCudaLastFrame_) {
        CapturePendingPhoto(originalFrame);
    }
    return true;
}
void OkuFlowApp::BuildCompositeAndPresent(UINT width,
                                           UINT height,
                                           CapturedFrame* originalFrame) {
    processedFrameWidth_ = width;
    processedFrameHeight_ = height;
    usingCudaLastFrame_ = false;
    if (!debugViewEnabled_ && ProcessFrameWithCuda(width, height)) {
        usingCudaLastFrame_ = true;
        // Recording and the periodic assistive grab both use the async
        // readback ring; nothing on this path blocks on the GPU anymore.
        pendingSceneCaptureClock100ns_ = currentCameraCaptureClock100ns_;
        ++startupProcessedScenes_;
        PresentLatestCudaScene(true, originalFrame);
        return;
    }

    if (!debugViewEnabled_) {
        // The CPU effects pipeline is deprecated: without CUDA the app
        // presents the unprocessed converted frame and reports that the GPU
        // is required. Recording, snapshots, and assistive readback continue
        // to work from the presentation buffer inside PresentFitted.
        const std::vector<uint8_t>& raw = cpuPipeline_.StageRaw();
        if (raw.empty()) {
            UpdateProcessingStatusLabel();
            return;
        }
        cpuSceneBuffer_ = raw;
        cpuSceneWidth_ = width;
        cpuSceneHeight_ = height;
        cpuSceneReady_ = true;
        pendingSceneCaptureClock100ns_ = currentCameraCaptureClock100ns_;
        ++startupProcessedScenes_;
        PresentFitted(
            cpuSceneBuffer_.data(),
            width,
            height,
            pipelineOrchestrator_->ViewportFitMode() ==
                settings::ViewportFitModeSetting::Fill,
            zoomCenterX_,
            zoomCenterY_,
            originalFrame);
        return;
    }

    // Legacy CPU composite, kept as a diagnostic for the debug view only.
    processing::CpuPipelineConfig config{};
    config.enableBlackWhite = blackWhiteEnabled_;
    config.blackWhiteThreshold = blackWhiteThreshold_;
    // Viewport geometry owns zoom. Keeping it out of the stateful CPU stage
    // lets the cached result be re-presented smoothly without advancing
    // temporal effects or applying magnification twice.
    config.enableZoom = false;
    config.zoomAmount = 1.0f;
    config.zoomCenterX = zoomCenterX_;
    config.zoomCenterY = zoomCenterY_;
    config.enableBlur = blurEnabled_;
    config.blurRadius = std::max(0, blurRadius_);
    config.blurSigma = blurSigma_;
    config.enableTemporalSmooth = temporalSmoothEnabled_;
    config.temporalSmoothAlpha = temporalSmoothAlpha_;

    const processing::CpuPipelineOutput output =
        cpuPipeline_.BuildStages(width, height, config, debugViewEnabled_);

    if (!output.data || output.width == 0 || output.height == 0) {
        UpdateProcessingStatusLabel();
        return;
    }
    cpuSceneBuffer_.assign(
        output.data,
        output.data + static_cast<std::size_t>(output.width) *
                          output.height * 4);
    cpuSceneWidth_ = output.width;
    cpuSceneHeight_ = output.height;
    cpuSceneReady_ = true;
    pendingSceneCaptureClock100ns_ = currentCameraCaptureClock100ns_;
    ++startupProcessedScenes_;

    const bool cropToFill =
        pipelineOrchestrator_->ViewportFitMode() ==
        settings::ViewportFitModeSetting::Fill;
    const float centerX = cropToFill ? zoomCenterX_ : 0.5f;
    const float centerY = cropToFill ? zoomCenterY_ : 0.5f;
    PresentFitted(cpuSceneBuffer_.data(),
                  cpuSceneWidth_,
                  cpuSceneHeight_,
                  cropToFill,
                  centerX,
                  centerY,
                  originalFrame);
}

void OkuFlowApp::PresentFitted(const uint8_t* data,
                                UINT srcWidth,
                                UINT srcHeight,
                                bool cropToFill,
                                float centerXNorm,
                                float centerYNorm,
                                const CapturedFrame* originalFrame) {
    if (!data || srcWidth == 0 || srcHeight == 0) {
        return;
    }

    if (!uiState_->renderWidget_ || !uiState_->renderWidget_->isPresenterReady()) {
        return;
    }

    const UINT viewportWidth = presenter_->ViewportWidth();
    const UINT viewportHeight = presenter_->ViewportHeight();
    if (viewportWidth == 0 || viewportHeight == 0) {
        return;
    }
    const ViewTransform transform =
        ComputeViewTransform(
            srcWidth,
            srcHeight,
            viewportWidth,
            viewportHeight,
            zoomEnabled_ ? zoomAmount_ : 1.0f,
            centerXNorm,
            centerYNorm,
            cropToFill ? ViewportFitMode::kFill : ViewportFitMode::kFit);
    const PixelViewMapping mapping =
        ComputePixelViewMapping(
            transform, srcWidth, srcHeight, viewportWidth, viewportHeight);
    if (!mapping.valid) {
        return;
    }
    if (mainWindow_) {
        mainWindow_->setAnnotationViewTransform(transform);
    }

    presentationBuffer_.assign(static_cast<size_t>(mapping.targetWidth) * mapping.targetHeight * 4, 0);
    presentationWidth_ = mapping.targetWidth;
    presentationHeight_ = mapping.targetHeight;

    const UINT srcStride = srcWidth * 4;
    const UINT dstStride = mapping.targetWidth * 4;

    for (UINT y = 0; y < mapping.activeHeight; ++y) {
        const float sampleY = mapping.startY + static_cast<float>(y) * mapping.stepY;
        int srcYIndex = static_cast<int>(std::lroundf(sampleY));
        srcYIndex = std::clamp(srcYIndex, 0, static_cast<int>(srcHeight) - 1);
        uint8_t* dstRow = presentationBuffer_.data() +
                          (static_cast<size_t>(mapping.offsetY + y) * dstStride) +
                          mapping.offsetX * 4;
        const uint8_t* srcRow = data + static_cast<size_t>(srcYIndex) * srcStride;
        for (UINT x = 0; x < mapping.activeWidth; ++x) {
            const float sampleX = mapping.startX + static_cast<float>(x) * mapping.stepX;
            int srcXIndex = static_cast<int>(std::lroundf(sampleX));
            srcXIndex = std::clamp(srcXIndex, 0, static_cast<int>(srcWidth) - 1);
            const uint8_t* srcPixel = srcRow + srcXIndex * 4;
            uint8_t* dstPixel = dstRow + x * 4;
            dstPixel[0] = srcPixel[0];
            dstPixel[1] = srcPixel[1];
            dstPixel[2] = srcPixel[2];
            dstPixel[3] = srcPixel[3];
        }
    }

    if (focusMarkerEnabled_ && cropToFill) {
        const float focusX =
            std::clamp(centerXNorm, 0.0f, 1.0f) * static_cast<float>(srcWidth);
        const float focusY =
            std::clamp(centerYNorm, 0.0f, 1.0f) * static_cast<float>(srcHeight);
        const float localX = (focusX - mapping.startX) / mapping.stepX;
        const float localY = (focusY - mapping.startY) / mapping.stepY;
        const float markerX = static_cast<float>(mapping.offsetX) + localX;
        const float markerY = static_cast<float>(mapping.offsetY) + localY;

        auto drawFilledCircle = [&](float cx, float cy, float radius,
                                    uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
            const int minX = std::max(0, static_cast<int>(std::floor(cx - radius)));
            const int maxX = std::min(static_cast<int>(mapping.targetWidth) - 1,
                                      static_cast<int>(std::ceil(cx + radius)));
            const int minY = std::max(0, static_cast<int>(std::floor(cy - radius)));
            const int maxY = std::min(static_cast<int>(mapping.targetHeight) - 1,
                                      static_cast<int>(std::ceil(cy + radius)));
            const float radiusSq = radius * radius;
            for (int py = minY; py <= maxY; ++py) {
                const float dy = (static_cast<float>(py) + 0.5f) - cy;
                for (int px = minX; px <= maxX; ++px) {
                    const float dx = (static_cast<float>(px) + 0.5f) - cx;
                    if (dx * dx + dy * dy <= radiusSq) {
                        uint8_t* pixel = presentationBuffer_.data() +
                                         (static_cast<size_t>(py) * mapping.targetWidth + px) * 4;
                        pixel[0] = b;
                        pixel[1] = g;
                        pixel[2] = r;
                        pixel[3] = a;
                    }
                }
            }
        };

        constexpr float kMarkerRadius = 18.0f;
        drawFilledCircle(markerX, markerY, kMarkerRadius, 0, 0, 255, 255);
        constexpr float kInnerRadius = 6.0f;
        drawFilledCircle(markerX, markerY, kInnerRadius, 255, 255, 255, 255);
    }

    if (!annotationCaptureQueue_.empty()) {
        PendingAnnotationCapture capture =
            std::move(annotationCaptureQueue_.front());
        annotationCaptureQueue_.pop_front();
        capture.transform = transform;
        SaveAnnotationSnapshot(presentationBuffer_.data(),
                               mapping.targetWidth,
                               mapping.targetHeight,
                               capture.strokes,
                               capture.transform,
                               capture.heading);
    }

    const bool focusGateEnabled = focusDetectionEnabled_ || autoTextClarityEnabled_;
    const bool focusAcceptable =
        !focusGateEnabled || !cudaSurface_ ||
        cudaSurface_->IsFocusAcceptable(focusThreshold_);
    if (originalFrame) {
        assistiveManager_->MaybeRequestAnalysis(presentationBuffer_.data(),
                                                 mapping.targetWidth,
                                                 mapping.targetHeight,
                                                 debugViewEnabled_,
                                                 focusGateEnabled,
                                                 focusAcceptable);
    }
    if (originalFrame && originalFrame->IsValid()) {
        if (recordingManager_) {
            recordingManager_->AddSceneFrame(
                data,
                srcWidth,
                srcHeight,
                ToRecordingTransform(transform),
                std::move(*originalFrame));
        }
    }
    presenter_->Present(presentationBuffer_.data(), mapping.targetWidth, mapping.targetHeight);
    if (presenter_->IsFaulted()) return;
    if (presenter_->NeedsScenePresent()) {
        pipelineOrchestrator_->MarkViewportDirty();
        return;
    }
    cameraFramePresented_ = true;
    RecordStartupFirstPresent();
    if (pendingSceneCaptureClock100ns_) {
        pipelineOrchestrator_->RecordCaptureToPresentSample(
            MeasureCaptureToPresentLatency(*pendingSceneCaptureClock100ns_));
        pendingSceneCaptureClock100ns_.reset();
    }
    pipelineOrchestrator_->MarkViewportPresented();
    if (originalFrame) {
        UpdateProcessingStatusLabel();
    }
}

bool OkuFlowApp::PrepareOriginalFrame(const MediaFrame& source,
                                       CapturedFrame& destination)
{
    destination = {};
    if (!capturePipeline_.ConvertFrameToBgra(source.data,
                                             source.subtype,
                                             source.width,
                                             source.height,
                                             source.stride,
                                             source.dataSize,
                                             source.yuvColor)) {
        return false;
    }

    UINT width = source.width;
    UINT height = source.height;
    capturePipeline_.RotateRawBuffer(rotationQuarterTurns_, width, height);
    const std::vector<uint8_t>& pixels = capturePipeline_.StageRaw();
    if (pixels.empty()) {
        return false;
    }

    destination.pixels = pixels;
    destination.width = width;
    destination.height = height;
    destination.identity.captureTimestamp100ns =
        source.captureTimestamp100ns;
    destination.identity.captureClock100ns =
        source.captureClock100ns;
    destination.identity.sequenceNumber = source.sequenceNumber;
    destination.identity.frameRateNumerator =
        source.frameRateNumerator;
    destination.identity.frameRateDenominator =
        source.frameRateDenominator;
    return true;
}

bool OkuFlowApp::PopulateOriginalFrameMetadata(
    const MediaFrame& source,
    CapturedFrame& destination) const
{
    if (source.width == 0 || source.height == 0) {
        return false;
    }
    const int turns = ((rotationQuarterTurns_ % 4) + 4) % 4;
    destination = {};
    destination.width =
        ((turns & 1) != 0) ? source.height : source.width;
    destination.height =
        ((turns & 1) != 0) ? source.width : source.height;
    destination.identity.captureTimestamp100ns =
        source.captureTimestamp100ns;
    destination.identity.captureClock100ns =
        source.captureClock100ns;
    destination.identity.sequenceNumber = source.sequenceNumber;
    destination.identity.frameRateNumerator =
        source.frameRateNumerator;
    destination.identity.frameRateDenominator =
        source.frameRateDenominator;
    return true;
}

void OkuFlowApp::CapturePendingPhoto(const CapturedFrame& originalFrame)
{
    if (!photoCapturePending_) {
        return;
    }
    photoCapturePending_ = false;
    if (!originalFrame.HasCpuPixels()) {
        ShowStatusMessage(QStringLiteral(
            "Photo not saved: the original camera frame could not be converted."));
        return;
    }

    if (usingCudaLastFrame_) {
        // CUDA photos are paired with the asynchronous viewport readback in
        // PresentLatestCudaScene so the saved image matches the visible crop.
        photoCapturePending_ = true;
        return;
    }
    if (presentationBuffer_.empty() ||
        presentationWidth_ == 0 || presentationHeight_ == 0) {
        ShowStatusMessage(QStringLiteral("Photo not saved: no processed frame was available."));
        return;
    }
    SaveCapturedPhotoPair(presentationBuffer_.data(),
                          presentationWidth_,
                          presentationHeight_,
                          originalFrame);
}

void OkuFlowApp::SaveCapturedPhotoPair(const uint8_t* processedData,
                                        UINT processedWidth,
                                        UINT processedHeight,
                                        const CapturedFrame& originalFrame)
{
    if (!processedData || processedWidth == 0 || processedHeight == 0 ||
        !originalFrame.HasCpuPixels()) {
        ShowStatusMessage(QStringLiteral(
            "Photo not saved: the processed or original frame was unavailable."));
        return;
    }
    QString outputError;
    const QString dirPath =
        userDataPaths_
            ? userDataPaths_->PhotosForDate(QDate::currentDate(), &outputError)
            : QString();
    if (dirPath.isEmpty()) {
        ShowStatusMessage(
            outputError.isEmpty()
                ? QStringLiteral(
                      "Photo not saved: the OkuFlow Photos folder is unavailable.")
                : outputError);
        return;
    }
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString processedPath = QDir(dirPath).filePath(
        QStringLiteral("IMG_%1_processed.jpg").arg(timestamp));
    const QString originalPath = QDir(dirPath).filePath(
        QStringLiteral("IMG_%1_original.jpg").arg(timestamp));
    const QString transactionLockPath = QDir(dirPath).filePath(
        QStringLiteral("IMG_%1.pair.lock").arg(timestamp));

    QImage processedImage(processedData,
                          static_cast<int>(processedWidth),
                          static_cast<int>(processedHeight),
                          static_cast<int>(processedWidth) * 4,
                          QImage::Format_ARGB32);
    QImage originalImage(originalFrame.pixels.data(),
                         static_cast<int>(originalFrame.width),
                         static_cast<int>(originalFrame.height),
                         static_cast<int>(originalFrame.width) * 4,
                         QImage::Format_ARGB32);
    processedImage = processedImage.copy();
    originalImage = originalImage.copy();
    if (processedImage.isNull() || originalImage.isNull()) {
        ShowStatusMessage(
            QStringLiteral("Photo not saved: the image could not be queued."));
        return;
    }

    ShowStatusMessage(QStringLiteral("Saving original and processed photos..."),
                      2500);
    QPointer<OkuFlowApp> owner(this);
    const bool queued = imageIoPool_ && imageIoPool_->tryStart(
        [owner,
         processedImage = std::move(processedImage),
         originalImage = std::move(originalImage),
         processedPath,
         originalPath,
         transactionLockPath]() mutable {
            // Encode both files before entering the two-rename commit. The
            // runtime rolls back a failed commit; after a process crash,
            // startup completes the remaining rename from the intact temp or
            // removes the entire incomplete set.
            const QString processedTemp =
                processedPath + QStringLiteral(".writing");
            const QString originalTemp =
                originalPath + QStringLiteral(".writing");
            QLockFile transactionLock(transactionLockPath);
            // Do not age out a valid but slow network/removable-drive write.
            // Owner-process death still makes the lock stale and immediately
            // recoverable at the next startup.
            transactionLock.setStaleLockTime(0);
            const bool lockAcquired = transactionLock.tryLock(0);
            if (!lockAcquired) {
                qWarning() << "Could not lock paired snapshot transaction"
                           << transactionLockPath;
            }
            const bool processedSaved =
                lockAcquired && processedImage.save(processedTemp, "JPG", 90);
            const bool originalSaved =
                lockAcquired && originalImage.save(originalTemp, "JPG", 90);
            if (lockAcquired && !processedSaved) {
                qWarning() << "Failed to save snapshot to" << processedTemp;
            }
            if (lockAcquired && !originalSaved) {
                qWarning() << "Failed to save snapshot to" << originalTemp;
            }
            // Processed renames first. If the process dies between renames,
            // the processed final plus fully encoded original .writing file
            // form an unambiguous recovery record; startup finishes the
            // original rename. A final without its counterpart temp is
            // rolled back instead of being presented as a complete capture.
            bool committed = false;
            if (processedSaved && originalSaved) {
                if (QFile::rename(processedTemp, processedPath)) {
                    if (QFile::rename(originalTemp, originalPath)) {
                        committed = true;
                    } else {
                        qWarning() << "Failed to finalize snapshot"
                                   << originalPath;
                        if (!QFile::remove(processedPath)) {
                            qWarning() << "Rollback could not remove"
                                       << processedPath;
                        }
                    }
                } else {
                    qWarning() << "Failed to finalize snapshot"
                               << processedPath;
                }
            }
            QStringList leftoverPaths;
            if (!committed && lockAcquired) {
                // Every removal is checked; anything that survives rollback
                // is reported to the user instead of pretending the folder
                // is clean. A caller that did not acquire the pair lock must
                // not touch paths owned by another process with the same
                // millisecond timestamp.
                for (const QString& path : {processedTemp, originalTemp,
                                            processedPath, originalPath}) {
                    if (QFileInfo::exists(path) && !QFile::remove(path)) {
                        leftoverPaths.append(QDir::toNativeSeparators(path));
                    }
                }
            }
            if (!owner) {
                return;
            }
            QMetaObject::invokeMethod(
                owner,
                [owner,
                 committed,
                 leftoverPaths,
                 processedPath,
                 originalPath]() {
                    if (!owner) {
                        return;
                    }
                    if (committed) {
                        qInfo() << "Saved paired snapshots to"
                                << originalPath << "and" << processedPath;
                        owner->assistiveManager_->Runtime()
                            .NoteCapturedPhotoPair(originalPath,
                                                   processedPath);
                        owner->ShowStatusMessage(
                            QStringLiteral(
                                "Saved original and processed photos. Press "
                                "Ctrl+Shift+O to open the OkuFlow folder."),
                            7000);
                    } else if (leftoverPaths.isEmpty()) {
                        owner->ShowStatusMessage(
                            QStringLiteral(
                                "The paired photos could not be saved, so no "
                                "files were kept. Try again."));
                    } else {
                        owner->ShowStatusMessage(
                            QStringLiteral(
                                "The paired photos could not be saved. A "
                                "partial file may remain: %1")
                                .arg(leftoverPaths.join(
                                    QStringLiteral(", "))),
                            10000);
                    }
                },
                Qt::QueuedConnection);
        });
    if (!queued) {
        ShowStatusMessage(
            QStringLiteral(
                "Photo not saved: the image writer is busy. Try again."));
    }
}

void OkuFlowApp::QueueAnnotationSnapshot(int reason)
{
    if (!mainWindow_ || !mainWindow_->annotationOverlay() ||
        !mainWindow_->annotationOverlay()->HasInk()) {
        return;
    }
    PendingAnnotationCapture capture;
    capture.strokes = mainWindow_->annotationOverlay()->Strokes();
    capture.heading =
        reason == 1
            ? QCoreApplication::translate("OkuFlow",
                                          "Annotations cleared - snapshot")
            : reason == 2
                  ? QCoreApplication::translate(
                        "OkuFlow", "Annotation session ended - snapshot")
                  : QCoreApplication::translate("OkuFlow", "Annotated view");
    annotationCaptureQueue_.push_back(std::move(capture));
    pipelineOrchestrator_->MarkViewportDirty();
    ShowStatusMessage(QStringLiteral("Saving annotated view to lecture notes..."),
                      2500);
}

void OkuFlowApp::SaveAnnotationSnapshot(
    const uint8_t* processedData,
    UINT processedWidth,
    UINT processedHeight,
    const QVector<AnnotationStroke>& strokes,
    const ViewTransform& transform,
    const QString& heading)
{
    if (!processedData || processedWidth == 0 || processedHeight == 0 ||
        strokes.isEmpty() || !transform.valid) {
        ShowStatusMessage(
            QStringLiteral("Annotated view not saved: the viewport was unavailable."));
        return;
    }

    QImage image(processedData,
                 static_cast<int>(processedWidth),
                 static_cast<int>(processedHeight),
                 static_cast<int>(processedWidth) * 4,
                 QImage::Format_ARGB32);
    QImage annotated = image.copy();
    if (annotated.isNull()) {
        ShowStatusMessage(
            QStringLiteral("Annotated view not saved: the image could not be queued."));
        return;
    }

    QString outputError;
    const QString dirPath =
        userDataPaths_
            ? userDataPaths_->PhotosForDate(QDate::currentDate(), &outputError)
            : QString();
    if (dirPath.isEmpty()) {
        ShowStatusMessage(
            outputError.isEmpty()
                ? QStringLiteral(
                      "Annotated view not saved: the OkuFlow Photos folder "
                      "is unavailable.")
                : outputError);
        return;
    }
    const QString timestamp =
        QDateTime::currentDateTime().toString(
            QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString path = QDir(dirPath).filePath(
        QStringLiteral("ANNOTATION_%1.png").arg(timestamp));
    QPointer<OkuFlowApp> owner(this);
    const bool queued = imageIoPool_ && imageIoPool_->tryStart(
        [owner,
         annotated = std::move(annotated),
         strokes,
         transform,
         path,
         heading]() mutable {
            {
                QPainter painter(&annotated);
                RenderAnnotationStrokes(painter,
                                        strokes,
                                        transform,
                                        annotated.size());
            }
            const bool saved = annotated.save(path, "PNG");
            if (!saved) {
                qWarning() << "Failed to save annotation snapshot to" << path;
            }
            if (!owner) {
                return;
            }
            QMetaObject::invokeMethod(
                owner,
                [owner, saved, path, heading]() {
                    if (!owner) {
                        return;
                    }
                    if (!saved) {
                        owner->ShowStatusMessage(
                            QStringLiteral(
                                "Annotated view could not be saved."));
                        return;
                    }
                    owner->assistiveManager_->Runtime()
                        .NoteAnnotationSnapshot(path, heading);
                    owner->ShowStatusMessage(
                        QStringLiteral(
                            "Saved annotated view to lecture notes. Press "
                            "Ctrl+Shift+O to open the OkuFlow folder."),
                        7000);
                },
                Qt::QueuedConnection);
        });
    if (!queued) {
        ShowStatusMessage(
            QStringLiteral(
                "Annotated view not saved: the image writer is busy. Try again."));
    }
}

} // namespace okuflow

#endif // _WIN32
