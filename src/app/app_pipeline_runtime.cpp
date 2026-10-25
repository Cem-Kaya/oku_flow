#ifdef _WIN32

#include "app_internal.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/capture/capture_buffer.hpp"
#include "okuflow/capture/phone_camera.hpp"
#include "okuflow/common/video_playback.hpp"

#include <QAccessible>
#include <QLockFile>
#include <QMetaObject>
#include <QPointer>
#include <QScopeGuard>
#include <QThreadPool>

#include <set>
#include <bit>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>

namespace okuflow {

namespace {

void RetainOriginalForPresentation(PresentedOriginalCache& cache,
                                  const CapturedFrame* original,
                                  const QString& retentionId,
                                  std::uint64_t cameraEpoch, int rotation,
                                  const PresentedOriginalCache::ViewRecipe& recipe)
{
    if (!original || retentionId.isEmpty()) return;
    try {
        cache.Stage(*original, retentionId, cameraEpoch, rotation, recipe);
    } catch (const std::bad_alloc&) {
        // Pause retention cannot revoke the independently admitted recording.
        // A later successful unretained receipt invalidates the old hold.
    }
}

// Playback owns these pixels independently of camera textures and encoder
// leases. Preparing appearance here keeps pan/zoom on the viewport clock.
uct::SharedPixels PrepareMediaScene(const uint8_t* data, UINT width, UINT height,
                                   UINT stride, int turns, bool blackWhite,
                                   float threshold, int colorTransform,
                                   const color_schemes::ColorLut& lut,
                                   float contrast, float brightness)
{
    if (!data || !VideoPlaybackLayoutFits(width, height, stride)) return {};
    auto result = std::make_shared<uct::PixelBuffer>();
    turns = ((turns % 4) + 4) % 4;
    result->width = (turns & 1) ? height : width;
    result->height = (turns & 1) ? width : height;
    result->strideBytes = result->width * 4;
    result->bytes.resize(static_cast<std::size_t>(result->strideBytes) * result->height);
    const auto grade = [contrast, brightness](uint8_t value) {
        return std::clamp((static_cast<float>(value) / 255.0f - 0.5f) * contrast +
                              0.5f + brightness,
                          0.0f, 1.0f);
    };
    const auto byte = [](float value) {
        return static_cast<uint8_t>(std::clamp(std::lround(value * 255.0f), 0l, 255l));
    };
    for (UINT y = 0; y < static_cast<UINT>(result->height); ++y) {
        for (UINT x = 0; x < static_cast<UINT>(result->width); ++x) {
            const UINT sx = turns == 1 ? y : turns == 2 ? width - 1 - x :
                            turns == 3 ? width - 1 - y : x;
            const UINT sy = turns == 1 ? height - 1 - x : turns == 2 ? height - 1 - y :
                            turns == 3 ? x : y;
            const auto* source = data + static_cast<std::size_t>(sy) * stride + sx * 4;
            auto* target = result->bytes.data() +
                static_cast<std::size_t>(y) * result->strideBytes + x * 4;
            uint8_t b = source[0], g = source[1], r = source[2];
            if (blackWhite) {
                const auto luma = byte(0.114f * (b / 255.0f) + 0.587f * (g / 255.0f) +
                                       0.299f * (r / 255.0f));
                b = g = r = (static_cast<float>(luma) / 255.0f) >=
                                    std::clamp(threshold, 0.0f, 1.0f) ? 255 : 0;
            }
            float gb = grade(b), gg = grade(g), gr = grade(r);
            if (colorTransform == 1) {
                gb = 1.0f - gb; gg = 1.0f - gg; gr = 1.0f - gr;
            }
            if (colorTransform == 2) {
                const auto mapped = lut[byte(0.114f * gb + 0.587f * gg + 0.299f * gr)];
                target[0] = static_cast<uint8_t>(mapped);
                target[1] = static_cast<uint8_t>(mapped >> 8);
                target[2] = static_cast<uint8_t>(mapped >> 16);
            } else {
                target[0] = byte(gb); target[1] = byte(gg); target[2] = byte(gr);
            }
            target[3] = source[3];
        }
    }
    return result;
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

QSize OkuFlowApp::ProcessingCacheExtent(UINT width, UINT height) const {
    const auto extent = ComputeSuperResCacheExtent(width, height, mlTextSuperResolutionUltra1440p_);
    const QSize ceiling = width >= height ? QSize(2560, 1440) : QSize(1440, 2560);
    const QSize requested(extent.width, extent.height);
    return requested.width() <= ceiling.width() && requested.height() <= ceiling.height()
        ? requested : requested.scaled(ceiling, Qt::KeepAspectRatio);
}

void OkuFlowApp::EnumerateCameras() {
    cameras_ = mediaCapture_.EnumerateCameras(false);
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
    if (!uiState_ || !uiState_->cameraCombo_) {
        return;
    }

    QStringList names;
    for (const auto& camera : cameras_) {
        QString name = QString::fromStdWString(camera.name);
        if (!camera.phoneSerial.empty()) {
            QString label;
            if (camera.phoneCameraId.empty() || camera.phoneLensRole == "auto") label = TranslateUi(QStringLiteral("Automatic (recommended)"));
            else if (camera.phoneLensRole == "ultrawide") label = TranslateUi(QStringLiteral("Ultrawide camera"));
            else if (camera.phoneLensRole == "main") label = TranslateUi(QStringLiteral("Main camera"));
            else if (camera.phoneLensRole == "telephoto") label = TranslateUi(QStringLiteral("Telephoto camera"));
            if (!label.isEmpty()) name += QStringLiteral(" · ") + label;
        }
        names.append(name);
    }
    mainWindow_->setCameraDevices(names, selectedCameraIndex_);
}

void OkuFlowApp::PollPhoneCameraDiscovery()
{
    if (!initialized_ || phoneDiscoveryCanceled_.load() || cameraStartupPending_ ||
        (startupWorkers_ && startupWorkers_->active.load() != 0)) return;

    if (phoneDiscovery_.valid()) {
        if (phoneDiscovery_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
        try {
            auto [discoveryStatus, discovered] = phoneDiscovery_.get();
            phoneDiscoveryStatus_ = discoveryStatus;
            if (discoveryStatus == PhoneDiscoveryStatus::Listed) {
                const std::wstring selectedLink = selectedCameraIndex_ >= 0 &&
                        static_cast<size_t>(selectedCameraIndex_) < cameras_.size()
                    ? cameras_[static_cast<size_t>(selectedCameraIndex_)].symbolicLink : std::wstring();
                auto merged = MergePhoneCameraDiscovery(cameras_, discovered,
                    cameraActive_ ? selectedLink : std::wstring());
                const bool changed = merged.size() != cameras_.size() ||
                    !std::equal(merged.begin(), merged.end(), cameras_.begin(),
                        [](const auto& a, const auto& b) {
                            return a.symbolicLink == b.symbolicLink && a.name == b.name && a.phoneLensRole == b.phoneLensRole;
                        });
                cameras_ = std::move(merged);
                selectedCameraIndex_ = -1;
                for (size_t i = 0; i < cameras_.size(); ++i) {
                    if (cameras_[i].symbolicLink == selectedLink) selectedCameraIndex_ = static_cast<int>(i);
                }
                // Keep an open picker and screen-reader focus stable on unchanged snapshots.
                if (changed && uiState_->cameraCombo_) {
                    auto blocker = uiState_->BlockSignals(uiState_->cameraCombo_);
                    PopulateCameraCombo();
                    if (!mainWindow_->videoSourceSelected()) uiState_->cameraCombo_->setCurrentIndex(selectedCameraIndex_);
                }
                UpdatePhoneLightUi();
                for (const auto& phone : discovered) {
                    if (!phone.phoneSerial.empty() && phone.phoneCameraId.empty())
                        phoneRetries_[phone.phoneSerial].Observe(phone.phoneTransportId,
                            phone.phoneAppReady, phone.phoneProblem, phone.phoneServerEpoch);
                }
                const bool streamingPhone = cameraActive_ && selectedCameraIndex_ >= 0 &&
                    !cameras_[static_cast<size_t>(selectedCameraIndex_)].phoneSerial.empty();
                const int target = cameraIntent_.TemporaryOwner().isEmpty()
                    ? ResolveAutomaticPhoneIndex(cameras_, preferredPhoneCamera_.toStdWString(),
                        automaticPhoneSelectionAllowed_, cameraActive_) : -1;
                const bool recording = recordingManager_ && recordingManager_->IsActive();
                const bool sameRecordedSource = target >= 0 &&
                    cameras_[static_cast<size_t>(target)].symbolicLink == mediaCapture_.LastSymbolicLink();
                if (!streamingPhone && target >= 0 && !mediaCapture_.WasAbandoned() &&
                    (!recording || sameRecordedSource) &&
                    phoneRetries_[cameras_[static_cast<size_t>(target)].phoneSerial]
                        .CanAttempt(static_cast<std::int64_t>(GetTickCount64()))) {
                    pipelineOrchestrator_->CancelCameraReconnect();
                    if (uiState_->cameraCombo_) {
                        auto blocker = uiState_->BlockSignals(uiState_->cameraCombo_);
                        if (!mainWindow_->videoSourceSelected()) uiState_->cameraCombo_->setCurrentIndex(target);
                    }
                    if (preferredPhoneCamera_.isEmpty() && cameraIntent_.FillStartupIdentity(
                            QString::fromStdWString(cameras_[static_cast<size_t>(target)].symbolicLink),target)) {
                        auto& saved = settingsController_->MutableSettings();
                        saved.cameraSymbolicLink = QString::fromStdWString(
                            cameras_[static_cast<size_t>(target)].symbolicLink);
                        preferredPhoneCamera_ = saved.cameraSymbolicLink;
                    }
                    qInfo() << "Automatically connecting to USB phone camera";
                    StartCameraCapture(static_cast<size_t>(target), false, false, true);
                } else if (!cameraActive_) {
                    StartFallbackCamera();
                    UpdateCameraPlaceholder();
                }
            }
            UpdateCameraPlaceholder();
        } catch (const std::exception& error) {
            qWarning() << "Background phone discovery failed:" << error.what();
        }
    }
    if (cameraStartupPending_ || (phoneDiscoveryTimer_.isValid() &&
                                  phoneDiscoveryTimer_.elapsed() < 5000)) return;
    std::string connectedSerial;
    if (cameraActive_ && selectedCameraIndex_ >= 0 &&
        static_cast<size_t>(selectedCameraIndex_) < cameras_.size())
        connectedSerial = cameras_[static_cast<size_t>(selectedCameraIndex_)].phoneSerial;
    std::string recoverySerial;
    const QString phonePrefix = QStringLiteral("okuflow-phone:");
    const auto temporaryPhone = cameraIntent_.TemporaryPhoneIntent();
    const bool temporarySource = !cameraIntent_.TemporaryOwner().isEmpty();
    const auto phoneIntent = temporarySource ? temporaryPhone : preferredPhoneCamera_;
    const bool permitPhone = temporarySource ? !temporaryPhone.isEmpty() : automaticPhoneSelectionAllowed_;
    if (permitPhone && connectedSerial.empty() && phoneIntent.startsWith(phonePrefix))
        recoverySerial = phoneIntent.mid(phonePrefix.size()).section('/', 0, 0).toStdString();
    phoneDiscoveryTimer_.start();
    try {
        phoneDiscovery_ = std::async(std::launch::async, [connectedSerial, recoverySerial,
            serverIntent = permitPhone ? PhoneServerIntent::PhoneUse
                                                          : PhoneServerIntent::ReleaseForWebcam,
            canceled = &phoneDiscoveryCanceled_]() {
            PhoneDiscoveryStatus status{};
            auto phones = PhoneCameraTransport::Enumerate(connectedSerial, canceled, nullptr,
                                                         recoverySerial, &status, serverIntent);
            return std::make_pair(status, std::move(phones));
        });
    } catch (const std::exception& error) {
        qWarning() << "Cannot dispatch phone discovery:" << error.what();
    }
}

void OkuFlowApp::StartFallbackCamera()
{
    if (!cameraIntent_.TemporaryOwner().isEmpty() || !automaticPhoneSelectionAllowed_ || cameraActive_ || cameraStartupPending_ ||
        mediaCapture_.WasAbandoned() || (startupWorkers_ && startupWorkers_->active.load() != 0) ||
        (recordingManager_ && recordingManager_->IsActive())) return;
    // A fallback is temporary; only an explicit camera pick changes the saved intent.
    for (size_t i = 0; i < cameras_.size(); ++i) {
        if (!cameras_[i].phoneSerial.empty()) continue;
        if (uiState_->cameraCombo_) {
            auto blocker = uiState_->BlockSignals(uiState_->cameraCombo_);
            if (!mainWindow_->videoSourceSelected()) uiState_->cameraCombo_->setCurrentIndex(static_cast<int>(i));
        }
        StartCameraCapture(i, false, false, true,agent::CameraSourceOrigin::Fallback);
        return;
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

// Failed submissions cancel only their reservation. Repeated failures drain
// both APIs without resetting the presenter-owned live timeline.
void OkuFlowApp::HandleCudaProcessingFailure() {
    if (pipelineOrchestrator_->RecordCudaFailure() == 3) {
        if (presenter_) {
            if (!presenter_->WaitForIdle()) {
                HandlePresenterFault();
                return;
            }
        }
        if (cudaSurface_ && !cudaSurface_->WaitForIdle()) {
            HandlePresenterFault();
            return;
        }
        qWarning() << "CUDA processing failed 3 times in a row; GPU queues drained";
        ShowStatusMessage(QStringLiteral(
            "GPU processing keeps failing - showing unprocessed video."));
    }
}

bool OkuFlowApp::HandlePresenterFault() {
    if (presenter_) presenter_->PollGpuProgress();
    if ((!presenter_ || !presenter_->IsFaulted()) &&
        (!cudaSurface_ || !cudaSurface_->IsFaulted())) return false;
    if (ultraClearText_) ultraClearText_->OnMediaPresentationFailed();
    CancelPendingAssistantCaptures();
    usingCudaLastFrame_ = false;
    cudaSceneReady_ = false;
    cpuSceneReady_ = false;
    if (!presenterFaultReported_) {
        presenterFaultReported_ = true;
        if (cudaSurface_ && cudaSurface_->IsFaulted())
            qWarning() << "CUDA pipeline stopped:" << QString::fromStdString(cudaSurface_->LastError());
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
    pipelineOrchestrator_->SetFenceInteropEnabled(false);
    cudaSurface_.reset();
    cudaSharedTexture_.Reset();
    cudaSuperResTexture_.Reset();
    cudaOriginalTexture_.Reset();
    cudaSceneReady_ = false;
    cudaSurfaceWidth_ = 0;
    cudaSurfaceHeight_ = 0;
    cudaSuperResWidth_ = 0;
    cudaSuperResHeight_ = 0;

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
        qWarning() << "CUDA surface creation exception triggered fallback";
        cudaSurfaceRetry_.RecordFailure(CudaSurfaceRetry::Clock::now());
        return false;
    }
}

void OkuFlowApp::AdmitOriginalFrame(CapturedFrame& originalFrame, bool cudaOriginal) {
    const bool recording=recordingManager_ && recordingManager_->IsActive();
    if ((!recording && !LiveRecapActive()) ||
        originalFrame.width == 0 || originalFrame.height == 0) return;
    if(!recording && !LiveRecapWantsFrame(originalFrame.width,originalFrame.height,originalFrame.identity.captureClock100ns))return;

    QElapsedTimer cloneTimer;
    cloneTimer.start();
    bool poolExhausted = false;
    // A photo may already have materialized Original. Do not clone a second
    // GPU image in that case. Nothing in this producer samples viewing effects.
    if (!originalFrame.HasCpuPixels() && cudaOriginal && cudaOriginalTexture_ && presenter_ &&
        (recording || pipelineOrchestrator_->FenceInteropEnabled())) {
        RecordingViewTransform fullSource;
        fullSource.valid = true;
        originalFrame.gpuScene = presenter_->RequestRecordingFrame(
            cudaOriginalTexture_.Get(), originalFrame.width, originalFrame.height,
            fullSource, originalFrame.width, originalFrame.height, nullptr, 0,
            &poolExhausted,
            pipelineOrchestrator_->FenceInteropEnabled() ? presenter_->LastExternalSignal() : 0);
        if (!pipelineOrchestrator_->FenceInteropEnabled() && !presenter_->WaitForIdle()) return;
    }
    if (originalFrame.IsValid()) {
        // The manager retains an owned CPU/GPU lease; the caller may still
        // need its clean CPU pixels for the independent paired-photo action.
        if(recording)recordingManager_->AddOriginalFrame(originalFrame);
        SubmitLiveRecapOriginal(originalFrame);
        // The raw CUDA path has already processed this exact source. CPU
        // ingress is admitted before processing, so it stages only after a
        // successful scene build below; busy processing cannot replace the
        // pending source of a previously deferred scene.
        if (cudaOriginal && !MediaViewSelected())
            RetainOriginalForPresentation(presentedOriginalCache_, &originalFrame,
                LiveOriginalRetentionId(), cameraSessionId_, CameraOriginalRotationQuarterTurns(), cudaSceneViewRecipe_);
    } else if (poolExhausted && recording) {
        recordingManager_->NotifyRecordingPoolSkipped();
    } else if(recording) {
        recordingManager_->NotifyReadbackSkipped();
    }
    pipelineOrchestrator_->RecordStageSample(FrameTimingStage::RecordingClone,
        static_cast<float>(cloneTimer.nsecsElapsed()) * 1e-6f);
}

bool OkuFlowApp::ProcessFrameWithCuda(UINT width, UINT height) {
    cudaSubmissionBusy_ = false;
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
        LiveRecapActive() || (recordingManager_ && recordingManager_->IsActive());
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
    cudaSubmissionBusy_ = false;
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
    const int turns = ((CameraOriginalRotationQuarterTurns() % 4) + 4) % 4;
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
                LiveRecapActive() || (recordingManager_ && recordingManager_->IsActive());
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
                if (originalFrame) AdmitOriginalFrame(*originalFrame, true);
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
        const auto result = mediaCapture_.ReadbackGpuFrame(frame);
        if (result != GpuFrameReadbackResult::Ready) {
            if (result == GpuFrameReadbackResult::Pending && outGpuCompletionPending)
                *outGpuCompletionPending = CaptureGpuPending::Readback;
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
        LiveRecapActive() || (recordingManager_ && recordingManager_->IsActive());

    if (!RunCudaPipeline(hostInput, outWidth, outHeight)) {
        if (cudaSubmissionBusy_) return false;
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
    if (originalFrame) AdmitOriginalFrame(*originalFrame, true);
    PresentLatestCudaScene(true, originalFrame);
    return true;
}

ProcessingSettings OkuFlowApp::BuildProcessingSettings(UINT width, UINT height, UINT cacheWidth,
                                                         UINT cacheHeight, std::uint64_t frameSequence) const {
    ProcessingSettings settings{};
    if (controlService_) {
        const auto stamp = controlService_->Stamp();
        settings.controlFrame = {stamp.sourceEpoch,stamp.processingVersion,stamp.viewVersion,
            stamp.geometryEpoch,frameSequence,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()};
    }
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
    settings.spatialViewportWidth = presenter_ ? presenter_->ViewportWidth() : width;
    settings.spatialViewportHeight = presenter_ ? presenter_->ViewportHeight() : height;
    settings.spatialViewTransform = ComputeViewTransform(
        width, height,
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
    settings.deferMlSuperResForInteraction =
        pipelineOrchestrator_->IsViewportInteractionActive();
    settings.mlSuperResStrength = mlTextSuperResolutionStrength_;
    settings.mlSuperResUltra1440p = mlTextSuperResolutionUltra1440p_;
    if (settings.enableMlSuperRes && !settings.mlSuperResUltra1440p &&
        settings.zoomAmount >= 1.33f) {
        settings.spatialViewTransform = CanonicalizeSuperResViewTransform(
            settings.spatialViewTransform, width, height,
            cacheWidth, cacheHeight, settings.zoomAmount);
    }
    settings.ApplyTextClarityMaster();
    return settings;
}

// Shared tail of the CUDA path: builds ProcessingSettings from the live UI
// state, runs ProcessFrame with the fence dance, and presents the result.
// The interop surface must already exist at presentWidth x presentHeight.
bool OkuFlowApp::RunCudaPipeline(const ProcessingInput& input, UINT presentWidth, UINT presentHeight) {
    cudaSubmissionBusy_ = false;
    if (!cudaSurface_) {
        qWarning() << "CUDA pipeline disabled: surface not available";
        usingCudaLastFrame_ = false;
        return false;
    }

    ProcessingSettings settings = BuildProcessingSettings(presentWidth, presentHeight,
        cudaSuperResWidth_, cudaSuperResHeight_, currentCameraFrameSequence_);
    const bool hiddenLive = MediaViewSelected();
    if (hiddenLive) {
        // The selected file/held scene has its own effects cache. A camera
        // consumer here needs only conversion/orientation and Original output.
        settings = ProcessingSettings{};
        settings.stagingFormat = cudaBufferFormat_;
    }

    // The presenter owns reservations and queue dependencies for both APIs.
    FenceSyncParams cudaSyncParams{};
    if (pipelineOrchestrator_->FenceInteropEnabled()) {
        const auto waitValue = presenter_->LastGraphicsSignal();
        const auto signalValue = presenter_->ReserveExternalSignal();
        if (!signalValue) { HandlePresenterFault(); return false; }
        cudaSyncParams.enable = true;
        // Async readbacks copy from the shared texture on the graphics queue;
        // CUDA must not write the next frame until both the present and the
        // newest readback copy have retired (GPU-side wait only).
        cudaSyncParams.waitValue = waitValue;
        cudaSyncParams.signalValue = signalValue;
    }

    QElapsedTimer cudaSubmissionTimer;
    cudaSubmissionTimer.start();
    ProcessingInput stampedInput = input;
    stampedInput.cameraEpoch = cameraSessionId_;
    stampedInput.frameSequence = currentCameraFrameSequence_;
    stampedInput.settingsGeneration = cudaProcessingGeneration_;
#if OKUFLOW_ENABLE_CUDA
    cudaSurface_->EnableUctAnalysis(!hiddenLive && ultraClearText_ && ultraClearText_->IsEnabled());
    cudaSurface_->SetUctCompletionCaptureTicket(!hiddenLive && ultraClearText_
        ? ultraClearText_->CompletionProbeTicket(stampedInput.cameraEpoch, stampedInput.frameSequence) : 0);
#endif
    if (!cudaSurface_->ProcessFrame(stampedInput, settings, cudaSyncParams)) {
        pipelineOrchestrator_->RecordStageSample(
            FrameTimingStage::CudaSubmission,
            static_cast<float>(cudaSubmissionTimer.nsecsElapsed()) * 1e-6f);
        if (cudaSurface_->LastFailureWasBusy()) {
            cudaSubmissionBusy_ = true;
            // Admission queued nothing, so only the unused fence reservation
            // is canceled. Keep the last scene and all failure policy intact.
            if (cudaSyncParams.enable &&
                !presenter_->CancelExternalSignal(cudaSyncParams.signalValue)) {
                cudaSubmissionBusy_ = false;
                HandlePresenterFault();
            }
            return false;
        }
        if (cudaSurface_->IsFaulted()) {
            HandlePresenterFault();
            return false;
        }
        // A failed enqueue can leave earlier stream work in flight without
        // its promised signal. Retire it before cancelling the reservation;
        // unknown completion leaves the reservation and backing quarantined.
        if (!cudaSurface_->WaitForIdle()) {
            HandlePresenterFault();
            return false;
        }
        if (cudaSyncParams.enable) {
            if (!presenter_->CancelExternalSignal(cudaSyncParams.signalValue)) {
                HandlePresenterFault();
                return false;
            }
        }
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
        if (!presenter_->CommitExternalSignal(cudaSyncParams.signalValue)) {
            HandlePresenterFault(); return false;
        }
    }

    asyncSuperResPresentPending_ = false; // a new exact base scene supersedes enhancement
    // Stamp only the successfully admitted scene. A newer requested recipe,
    // failed upload or deferred presentation cannot rewrite displayed authority.
    const auto textStages = settings.TextClarityStages();
    cudaSceneViewRecipe_ = {
        !settings.enableBlur && !settings.enableSpatialSharpen &&
            !settings.enableTemporalSmoothing && !settings.enableStabilization &&
            !settings.enableBumpHold && !settings.enableKeystone && !settings.enableAutoContrast &&
            !textStages.background && !textStages.adaptive && !textStages.smartSharpen &&
            !textStages.clahe && !textStages.twoColor && !textStages.hysteresis &&
            !textStages.selectiveSharpen && !textStages.glare,
        settings.enableBlackWhite,
        settings.enableBlackWhite ? settings.blackWhiteThreshold : 0.5f,
        static_cast<int>(settings.displayColorTransform),
        settings.displayColorTransform == DisplayColorTransform::kLumaLut ? settings.displayColorLutGeneration : 0,
        settings.contrast, settings.brightness};
    cudaPipelineAvailable_ = true;
    UpdateKeystoneTrackingUi();
    usingCudaLastFrame_ = true;
    cudaSceneReady_ = true;
    // Ultra Clear Text provenance: exactly what this admitted scene was built
    // with. Busy/failed submissions above keep the previous snapshot, so
    // re-presents of the cached texture never borrow a newer frame/setting.
    if (!hiddenLive) {
        UctSceneSettings applied = UltraClearTextRequest();
        applied.keystone = settings.enableKeystone;
        applied.stabilization = settings.enableStabilization;
        ++annotationSceneRevision_;
        // Extra Stable remains a displayed composite for annotations, but UCT
        // supports it through each capture's hold record (heldFrames).
        uctProvenance_.OnCudaSceneProcessed(
            applied, presentWidth, presentHeight, currentCameraFrameSequence_,
            settings.enableTemporalSmoothing || (settings.enableStabilization && settings.enableBumpHold),
            false, settings.enableStabilization && settings.enableBumpHold);
    }
    if (!hiddenLive && controlTarget_) controlTarget_->FrameProcessed();
    // CPU pixels belong to the last CPU presentation, not this GPU scene.
    if (!hiddenLive) {
        presentationBuffer_.clear();
        presentationWidth_ = 0;
        presentationHeight_ = 0;
    }
    if (!hiddenLive) pipelineOrchestrator_->MarkSceneDirty();
    return true;
}

void OkuFlowApp::PresentLatestCudaScene(bool newCameraFrame,
                                         CapturedFrame* originalFrame) {
    if (MediaViewSelected()) {
        PresentMediaView();
        return;
    }
    if (!cudaSceneReady_ || !cudaSharedTexture_ || !presenter_ ||
        !presenter_->IsInitialized() ||
        processedFrameWidth_ == 0 || processedFrameHeight_ == 0) {
        return;
    }

    // Without external semaphore support, asynchronous cache publication
    // retires via a nonblocking stream poll before any graphics read begins.
    if (asyncSuperResPresentPending_) {
        if (!cudaSurface_ || !cudaSurface_->PollIdle()) {
            pipelineOrchestrator_->MarkSceneDirty();
            return;
        }
        asyncSuperResPresentPending_ = false;
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
    if (mlTextSuperResolutionEnabled_ && !mlTextSuperResolutionUltra1440p_ &&
        zoomEnabled_ && zoomAmount_ >= 1.33f &&
        pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fill) {
        transform = CanonicalizeSuperResViewTransform(
            transform, processedFrameWidth_, processedFrameHeight_,
            cudaSuperResWidth_, cudaSuperResHeight_, zoomAmount_);
    }
    const ViewTransform annotationTransform = transform;
    if (mainWindow_) {
        mainWindow_->setAnnotationViewTransform(annotationTransform);
    }
    // Ultra Clear Text reprojects cached words on this presentation clock and
    // at most asks for one more asynchronous copy of the presented viewport.
    UctFrameContext uctContext = UltraClearTextContext(
        true, annotationTransform, viewportWidth, viewportHeight);

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
    // An asynchronous enhancement changes the displayed image even when the
    // camera frame/geometry did not change. Revoke older OCR/readback tickets.
    uctContext.geometry.neuralEnhancement = presentingSuperRes;
    auto uctAnalysisContext=uctContext;
    uctAnalysisContext.geometry.neuralEnhancement=false; // analysis precedes neural appearance
#if OKUFLOW_ENABLE_CUDA
    if (ultraClearText_) ultraClearText_->OnAnalysisHealth(cudaSurface_->UctAnalysisFailed());
#endif
    const bool uctWanted = ultraClearText_ && ultraClearText_->OnPresented(uctAnalysisContext);
    presentationFocusX = std::clamp(presentationFocusX, 0.0f, 1.0f);
    presentationFocusY = std::clamp(presentationFocusY, 0.0f, 1.0f);

    if (pendingPhotoReadbackId_ != 0 &&
        pendingPhotoReadbackTimer_.isValid() &&
        pendingPhotoReadbackTimer_.elapsed() > 1000) {
        // Resize drops old-dimension presenter readbacks by contract. Retry a
        // photo whose request never returned instead of leaving capture stuck.
        pendingPhotoReadbackId_ = 0;
        pendingPhotoOriginal_ = {};
        pendingPhotoOverlays_ = {};
        pendingPhotoReadbackTimer_.invalidate();
        photoCapturePending_ = true;
    }
    ExpireAnnotationReadback();
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
    const bool assistiveWanted =
        newCameraFrame && assistiveManager_->WantsPeriodicReadback(debugViewEnabled_);
    const bool photoWanted =
        newCameraFrame && photoCapturePending_ &&
        originalFrame && originalFrame->HasCpuPixels();
    const AnnotationGpuPresentation annotationPresentation{presentationTexture,
        presentationSourceWidth, presentationSourceHeight, transform,
        presentingSuperRes, focusMarkerEnabled_, presentationFocusX, presentationFocusY};
    if (!annotationCaptureQueue_.empty() &&
        (!AnnotationFrameMatches(uctContext) || !AnnotationGpuPresentationMatches(annotationPresentation))) {
        annotationCaptureQueue_.clear();
        ShowStatusMessage(QStringLiteral("Annotated view not saved: the viewport was unavailable."));
    }
    const bool annotationWanted = pendingAnnotationReadbackId_ == 0 &&
        !annotationCaptureQueue_.empty();
    const bool onDemandAssistiveWanted =
        pendingOnDemandReadbackId_ == 0 &&
        pendingOnDemandAnalysis_;
    const bool assistantFrameWanted =
        pendingAssistantFrameReadbackId_ == 0 &&
        pendingAssistantFramePrompt_.has_value();
    auto observationFrame = controlTarget_ ? controlTarget_->PresentedFrame(annotationTransform,
        QSize(processedFrameWidth_,processedFrameHeight_),QSize(viewportWidth,viewportHeight)) : agent::ObservationFrame{};
    observationFrame.diagnostics["enhancement"] = QJsonObject{
        {"kind",presentingSuperRes ? "maxine" : presentationTexture == cudaSuperResTexture_.Get() ? "spatial" : "base"},
        {"cacheGeneration",static_cast<qint64>(superResRoi.generation)},
        {"cameraEpoch",static_cast<qint64>(cameraSessionId_)},
        {"frameSequence",static_cast<qint64>(uctContext.frameSeq)}};
    const bool observationWanted = controlTarget_ && controlTarget_->WantsObservationReadback(observationFrame);

    FenceSyncParams presentSync{};
    if (pipelineOrchestrator_->FenceInteropEnabled()) {
        presentSync.enable = true;
        presentSync.waitValue =
            presenter_->LastExternalSignal();
    }
    ViewportPresentationOptions presentationOptions{};
    presentationOptions.drawFocusMarker = focusMarkerEnabled_;
    presentationOptions.focusX = presentationFocusX;
    presentationOptions.focusY = presentationFocusY;
    presentationOptions.requestReadback =
        assistiveWanted || photoWanted || annotationWanted ||
        onDemandAssistiveWanted || assistantFrameWanted || observationWanted;
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
        if (ultraClearText_) {
            if (!presenter_->IsFaulted() && presenter_->NeedsScenePresent())
                ultraClearText_->OnPresentationDeferred();
            else
                ultraClearText_->OnPresentationFailed();
        }
        if (presenter_->IsFaulted()) {
            HandlePresenterFault();
            return;
        }
    }
    if (presented) {
        ApplyUltraClearTextDisplayColors(cudaSceneViewRecipe_.colorTransform,
                                        cudaSceneViewRecipe_.colorLutGeneration);
        if (ultraClearText_) {
            ultraClearText_->OnPresentationSucceeded();
        }
        lastAnnotationPresentation_ = uctContext;
        presentedOriginalCache_.Presented(cameraSessionId_, uctContext.frameSeq, annotationTransform,
            presentationTexture == cudaSharedTexture_.Get() &&
            !focusMarkerEnabled_,
            pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fit
                ? ViewportFitMode::kFit : ViewportFitMode::kFill,
            zoomEnabled_ && zoomAmount_ <= kWholeSourceZoom);
        lastAnnotationGpuPresentation_ = annotationPresentation;
        lastAnnotationPresentation_->cameraActive = true;
        lastAnnotationSceneRevision_ = annotationSceneRevision_;
        RetainPresentedCameraScene(uctContext.geometry.settings.sourceEpoch,uctContext.frameSeq,pendingSceneCaptureClock100ns_,
            presentationTexture,presentationSourceWidth,presentationSourceHeight,transform,nullptr,superResRoi.generation,
            presentationOptions.drawFocusMarker,presentationOptions.focusX,presentationOptions.focusY);
        annotationPresentationCuda_ = true;
        cameraIntent_.MarkFirstFrame(startingCameraIntentGeneration_);
        if (controlTarget_) controlTarget_->FramePresented(observationFrame);
        cameraFramePresented_ = true;
        RecordStartupFirstPresent();
        if (pendingSceneCaptureClock100ns_) {
            pipelineOrchestrator_->RecordCaptureToPresentSample(
                MeasureCaptureToPresentLatency(*pendingSceneCaptureClock100ns_));
            pendingSceneCaptureClock100ns_.reset();
        }
    }
    if (uctWanted && presented) {
#if OKUFLOW_ENABLE_CUDA
        auto analysis=cudaSurface_->TakeUctAnalysis();
        if(analysis && analysis->processingGeneration==cudaProcessingGeneration_ &&
            analysis->frameSequence==uctContext.frameSeq)
            ultraClearText_->OnDeviceScene(uctAnalysisContext,std::move(analysis));
#endif
    }
    if (readbackRequestId != 0) {
        if (observationWanted) controlTarget_->ObservationReadbackSubmitted(readbackRequestId,observationFrame);
        if (photoWanted) {
            if (controlTarget_) controlTarget_->PhotoState(photoOperationId_,agent::OperationState::Readback);
            pendingPhotoReadbackId_ = readbackRequestId;
            pendingPhotoOriginal_ = *originalFrame;
            pendingPhotoOverlays_ = CaptureViewOverlays(uctContext.transform);
            pendingPhotoReadbackTimer_.restart();
            photoCapturePending_ = false;
        }
        if (annotationWanted && presented) {
            activeAnnotationCapture_ =
                std::move(annotationCaptureQueue_.front());
            annotationCaptureQueue_.pop_front();
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
    } else if (presentationOptions.requestReadback) {
        // The four-slot readback ring was temporarily full. Keep the viewport
        // dirty so user-triggered requests retry instead of blocking.
        pipelineOrchestrator_->MarkViewportDirty();
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
// analysis. Original GPU admission owns a fenced full-source clone and does
// not consume this CPU readback ring.
void OkuFlowApp::RetirePendingMediaHoldReadback() {
    if (mediaHoldReadbackId_ != 0) {
        // The presenter admits at most one owned Original copy outstanding,
        // including canceled copies. Retain its exact id until it drains.
        if (retiredMediaHoldReadbackId_ == 0 || retiredMediaHoldReadbackId_ == mediaHoldReadbackId_) {
            retiredMediaHoldReadbackId_ = mediaHoldReadbackId_;
            mediaHoldReadbackId_ = 0;
            mediaHoldReadbackRetired_ = false;
        } else {
            // A resize may drop an older completed ring receipt before the
            // discard is observed. Keep a second bounded id in the active
            // slot, discard-only, rather than forgetting either provenance.
            // It blocks another hold until its own completion.
            mediaHoldReadbackRetired_ = true;
        }
    }
    mediaHoldReadbackTimer_.invalidate();
}

bool OkuFlowApp::DiscardRetiredMediaHoldReadback(UINT64 requestId) {
    if (requestId == 0) return false;
    if (requestId == retiredMediaHoldReadbackId_) {
        retiredMediaHoldReadbackId_ = 0;
        return true;
    }
    if (mediaHoldReadbackRetired_ && requestId == mediaHoldReadbackId_) {
        mediaHoldReadbackId_ = 0;
        mediaHoldReadbackRetired_ = false;
        return true;
    }
    return false;
}

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
        if (ConsumeVideoGpuReadback(requestId,asyncReadbackBuffer_,readbackWidth,readbackHeight)) continue;
        if (DiscardRetiredMediaHoldReadback(requestId)) continue;
        if (requestId == mediaHoldReadbackId_) {
            mediaHoldReadbackId_ = 0;
            mediaHoldReadbackRetired_ = false;
            mediaHoldReadbackTimer_.invalidate();
            AcceptLiveMediaHold(std::move(asyncReadbackBuffer_), readbackWidth,
                readbackHeight, mediaHoldFrameSeq_, mediaHoldCaptureClock100ns_);
            // This was a full clean source copy, not a presented viewport or
            // an assistive/observation request. Never route it to those consumers.
            continue;
        }
        if (controlTarget_) controlTarget_->ConsumeObservationReadback(requestId,QImage(asyncReadbackBuffer_.data(),
            readbackWidth,readbackHeight,readbackWidth*4,QImage::Format_ARGB32));
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
                                   activeAnnotationCapture_->text,
                                   activeAnnotationCapture_->heading);
            pendingAnnotationReadbackId_ = 0;
            pendingAnnotationReadbackTimer_.invalidate();
            activeAnnotationCapture_.reset();
            if (!annotationCaptureQueue_.empty()) {
                pipelineOrchestrator_->MarkViewportDirty();
            }
        }
        if (pendingOnDemandAnalysis_ && pendingOnDemandReadbackId_ != 0 &&
            requestId == pendingOnDemandReadbackId_) {
            const bool readText = pendingOnDemandReadText_;
            pendingOnDemandAnalysis_ = false;
            ++assistantCaptureGeneration_;
            pendingOnDemandReadText_ = false;
            pendingOnDemandReadbackId_ = 0;
            pendingOnDemandReadbackTimer_.invalidate();
            assistiveManager_->Runtime().SubmitFrameForced(
                asyncReadbackBuffer_.data(),
                static_cast<int>(readbackWidth),
                static_cast<int>(readbackHeight),
                readText);
            if (mainWindow_) {
                mainWindow_->setExplainBusy(assistiveManager_->Runtime().IsBusy() &&
                                            !assistantResponseOpen_ &&
                                            !pendingAssistantFramePrompt_);
            }
        }
        if (requestId == pendingAssistantFrameReadbackId_ &&
            pendingAssistantFramePrompt_) {
            const PendingAssistantFramePrompt prompt =
                std::move(*pendingAssistantFramePrompt_);
            pendingAssistantFramePrompt_.reset();
            ++assistantCaptureGeneration_;
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
        const bool focusGateEnabled = autoTextClarityEnabled_;
        const bool focusAcceptable =
            !focusGateEnabled || !cudaSurface_ ||
            cudaSurface_->IsFocusAcceptable(focusThreshold_);
        if (!MediaViewSelected()) {
            assistiveManager_->MaybeRequestAnalysis(asyncReadbackBuffer_.data(),
                                                     readbackWidth,
                                                     readbackHeight,
                                                     debugViewEnabled_,
                                                     focusGateEnabled,
                                                     focusAcceptable);
        }
        // Last consumer: Ultra Clear Text takes the pixels by swapping the
        // vector with a recycled one, so no 4K copy happens on this thread.
        if (ultraClearText_ && ultraClearText_->OwnsReadback(requestId)) {
            ultraClearText_->OnReadbackCompleted(
                requestId, asyncReadbackBuffer_, static_cast<int>(readbackWidth),
                static_cast<int>(readbackHeight), static_cast<int>(readbackWidth * 4));
        }
    }
    PollSelectedVideoSave();
    if (mediaHoldReadbackId_ != 0 && mediaHoldReadbackTimer_.isValid() &&
        mediaHoldReadbackTimer_.elapsed() > 1000) {
        RetirePendingMediaHoldReadback();
        AcceptLiveMediaHold({}, 0, 0, mediaHoldFrameSeq_, mediaHoldCaptureClock100ns_);
        ShowStatusMessage(QStringLiteral("Pause unavailable: the displayed frame could not be retained."));
    }
}

bool OkuFlowApp::CaptureLiveForMediaHold() {
    if (MediaViewSelected() || mediaHoldReadbackId_ != 0 || !cameraFramePresented_) return false;
    if(presentedCameraScene_ && presentedCameraScene_->epoch==cameraSessionId_) {
        const auto& scene=*presentedCameraScene_;
        if(scene.captureClock100ns<0)return false;
        mediaHoldFrameSeq_=scene.sequence;mediaHoldCaptureClock100ns_=scene.captureClock100ns;
        mediaHoldViewTransform_=scene.transform;mediaHoldCameraRotation_=CameraOriginalRotationQuarterTurns();
        // Intent belongs to these successfully presented pixels, not to a
        // newer requested Fit/zoom change whose presentation was deferred.
        mediaHoldFitMode_=scene.fitMode;mediaHoldWholeSource_=scene.wholeSource;
        mediaHoldDisplayRecipe_=scene.recipe;
        mediaHoldBakedScene_=true;
        if(scene.gpu.IsValid()) {
            if(!presenter_ || presenter_->IsFaulted() || !presenter_->RequestReadback(scene.gpu,&mediaHoldReadbackId_))return false;
            mediaHoldReadbackTimer_.start();return true;
        }
        if(scene.pixels)return AcceptLiveMediaHold(*scene.pixels,scene.width,scene.height,scene.sequence,scene.captureClock100ns);
    }
    mediaHoldBakedScene_=false;
    const auto retained = presentedOriginalCache_.Displayed();
    const auto retentionId=LiveOriginalRetentionId();
    if (!retained || retained->cameraEpoch != cameraSessionId_ ||
        retentionId.isEmpty() || retained->recordingSessionId != retentionId ||
        !retained->transform.valid) return false;
    const auto& original = *retained->original;
    PresentedOriginalCache::ViewRecipe selectedRecipe;
    selectedRecipe.blackWhite = blackWhiteEnabled_;
    selectedRecipe.blackWhiteThreshold = blackWhiteEnabled_ ? blackWhiteThreshold_ : 0.5f;
    selectedRecipe.colorTransform = displayColorScheme_.id == QStringLiteral("normal") ? 0 :
        displayColorScheme_.id == QStringLiteral("inverted") ? 1 : 2;
    selectedRecipe.colorLutGeneration = selectedRecipe.colorTransform == 2 ? displayColorLutGeneration_ : 0;
    selectedRecipe.contrast = contrast_; selectedRecipe.brightness = brightness_;
    if (!retained->canReproduceCleanView || retained->recipe != selectedRecipe) return false;
    mediaHoldFrameSeq_ = original.identity.sequenceNumber;
    mediaHoldCaptureClock100ns_ = original.identity.captureClock100ns;
    mediaHoldViewTransform_ = retained->transform;
    mediaHoldCameraRotation_ = retained->rotationQuarterTurns;
    mediaHoldFitMode_ = retained->fitMode == ViewportFitMode::kFit
        ? settings::ViewportFitModeSetting::Fit : settings::ViewportFitModeSetting::Fill;
    mediaHoldWholeSource_ = retained->wholeSource;
    if (original.HasGpuScene()) {
        if (!presenter_ || presenter_->IsFaulted() ||
            !presenter_->RequestReadback(original.gpuScene, &mediaHoldReadbackId_)) return false;
        mediaHoldReadbackTimer_.start();
        return true;
    }
    if (!original.HasCpuPixels()) return false;
    return AcceptLiveMediaHold(original.pixels, original.width, original.height,
                               mediaHoldFrameSeq_, mediaHoldCaptureClock100ns_);
}

void OkuFlowApp::RetainPresentedCameraScene(std::uint64_t sceneEpoch,std::uint64_t sceneSequence,
    std::optional<std::int64_t> committedCaptureClock100ns,const void* texture,UINT width,UINT height,
    const ViewTransform& transform,const std::uint8_t* pixels,std::uint64_t contentGeneration,
    bool focusMarker,float focusX,float focusY) {
    if(!LiveRecapActive() || MediaViewSelected())return;
    if(!lastAnnotationPresentation_ || sceneEpoch!=cameraSessionId_ ||
        sceneSequence!=lastAnnotationPresentation_->frameSeq ||
        sceneEpoch!=lastAnnotationPresentation_->geometry.settings.sourceEpoch) {
        presentedCameraCapture_.reset();presentedCameraScene_.reset();return;
    }
    // A successful receipt binds the processed clock before latency accounting
    // consumes it. Keep this one stamp independently of pixel-pool admission,
    // and never retime the same acquisition from newer camera ingress.
    if(!presentedCameraCapture_ || presentedCameraCapture_->epoch!=sceneEpoch ||
        presentedCameraCapture_->sequence!=sceneSequence)
        presentedCameraCapture_=PresentedCameraCaptureStamp{sceneEpoch,sceneSequence,-1};
    if(presentedCameraCapture_->clock100ns<0 && committedCaptureClock100ns && *committedCaptureClock100ns>=0)
        presentedCameraCapture_->clock100ns=*committedCaptureClock100ns;
    const auto captureClock=presentedCameraCapture_->clock100ns;
    if(!transform.valid || !width || !height ||
        std::uint64_t(width)*height*4>48u*1024*1024) {
        presentedCameraScene_.reset();return;
    }
    if(presentedCameraScene_ && presentedCameraScene_->epoch==cameraSessionId_ &&
        presentedCameraScene_->sequence==lastAnnotationPresentation_->frameSeq &&
        presentedCameraScene_->sceneRevision==annotationSceneRevision_ &&
        presentedCameraScene_->contentGeneration==contentGeneration && presentedCameraScene_->sourceTexture==texture) {
        presentedCameraScene_->transform=transform;
        presentedCameraScene_->fitMode=pipelineOrchestrator_->ViewportFitMode();
        presentedCameraScene_->wholeSource=zoomEnabled_ && zoomAmount_<=kWholeSourceZoom;
        presentedCameraScene_->captureClock100ns=captureClock;
        presentedCameraScene_->recipe={0,texture!=nullptr,focusMarker,focusX,focusY};return;
    }
    FrozenCameraScene scene;scene.epoch=sceneEpoch;scene.sequence=sceneSequence;
    scene.sceneRevision=annotationSceneRevision_;scene.captureClock100ns=captureClock;
    scene.contentGeneration=contentGeneration;scene.sourceTexture=texture;
    scene.width=width;scene.height=height;scene.transform=transform;
    scene.fitMode=pipelineOrchestrator_->ViewportFitMode();
    scene.wholeSource=zoomEnabled_ && zoomAmount_<=kWholeSourceZoom;
    scene.recipe={0,texture!=nullptr,focusMarker,focusX,focusY};
    if(texture && presenter_ && pipelineOrchestrator_->FenceInteropEnabled()) {
        RecordingViewTransform full;full.valid=true;
        scene.gpu=presenter_->RequestRecordingFrame(static_cast<ID3D12Resource*>(const_cast<void*>(texture)),
            width,height,full,width,height,nullptr,0,nullptr,presenter_->LastExternalSignal());
    } else if(pixels)try{scene.pixels=std::make_shared<const std::vector<std::uint8_t>>(pixels,pixels+std::size_t(width)*height*4);}
        catch(const std::bad_alloc&){presentedCameraScene_.reset();return;}
    // A successful unretained newer display revokes the old one. Pool pressure
    // must never make Freeze silently pick a previous camera acquisition.
    if(scene.gpu.IsValid() || scene.pixels)presentedCameraScene_=std::move(scene);
    else presentedCameraScene_.reset();
}

bool OkuFlowApp::StartCameraCapture(size_t index,
                                     bool interactive,
                                     bool forceCompatibility,
                                     bool backgroundStartup, agent::CameraSourceOrigin origin,
                                     std::optional<QString> formatOverride, const QString& operationId) {
    if (index >= cameras_.size() || cameraStopFailed_) {
        return false;
    }
    const auto link = QString::fromStdWString(cameras_[index].symbolicLink);
    const auto& saved = settingsController_->Settings();
    const bool restoreWorkingSet = origin != agent::CameraSourceOrigin::AgentTemporary &&
        cameraIntent_.TemporaryOwner().isEmpty() && link != currentCameraAccelerationKey_ && saved.cameraWorkingSets.contains(link);
    const auto fallbackFormat = !currentCameraAccelerationKey_.isEmpty() && link != currentCameraAccelerationKey_ &&
        origin != agent::CameraSourceOrigin::AgentTemporary ? QString() : saved.cameraFormatStableId;
    const auto format = formatOverride.value_or(restoreWorkingSet ? saved.cameraWorkingSets.value(link).formatStableId : fallbackFormat);
    if (cameraIntent_.Requested().identity.symbolicLink != link || cameraIntent_.Requested().identity.formatStableId != format || cameraIntent_.Requested().operationId != operationId
        || cameraIntent_.Requested().origin != origin)
        cameraIntent_.Request({link,format,false},origin,operationId);

    if (recordingManager_ && recordingManager_->CurrentSessionInfo()) {
        if (origin != agent::CameraSourceOrigin::Manual) return false;
        pendingCameraStart_ = PendingCameraStart{index,interactive,forceCompatibility,backgroundStartup,cameras_[index].symbolicLink,origin,formatOverride,operationId,cameraIntent_.Requested().generation};
        QTimer::singleShot(25,this,&OkuFlowApp::ResumePendingCameraStart);
        return true;
    }

    if (cameraActive_ || cameraStartupPending_ || cameraStopPending_) {
        pendingCameraStart_ = PendingCameraStart{index,interactive,forceCompatibility,backgroundStartup,cameras_[index].symbolicLink,origin,formatOverride,operationId,cameraIntent_.Requested().generation};
        StopCameraCapture();
        return !cameraStopFailed_;
    }
    if (startupWorkers_ && startupWorkers_->active.load() != 0) return false;

    selectedCameraIndex_ = static_cast<int>(index);
    startingCameraIntentGeneration_ = cameraIntent_.Requested().generation;
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
        ++uctWarpRevision_;
        cudaSurface_->ResetTextClarityHistory();
    }
    UpdateKeystoneTrackingUi();

    const CameraDescriptor& descriptor = cameras_[index];
    // Only the explicit legacy profiling comparison may open a physical
    // driver on this thread. Every ordinary reopen/reconnect uses one owner.
    backgroundStartup = backgroundStartup || !startupSynchronousProfile_ || !descriptor.phoneSerial.empty();
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
    const QString requestedStableId = format;
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
            const auto dropped = EnqueueCaptureFrame(
                ingress->frames, std::move(frame), ingress->retainBurst,
                kMaxRecordingCameraFrames, retired);
            ingress->dropped += dropped;
            ingress->profileDropped += dropped;
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
                        app->recordingManager_->NotifyCaptureFrames(received, dropped);
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
        const std::atomic<bool> notCanceled{false};
        if (cameraStartForTest_) return cameraStartForTest_(notCanceled);
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
        if (!cameraIntent_.TemporaryOwner().isEmpty() && controlTarget_) {
            controlTarget_->CameraTransitionFailed(); return false;
        }
        const std::string detail = startupError.isEmpty()
                                      ? mediaCapture_.LastError()
                                      : startupError.toStdString();
        const CameraFailureKind kind = mediaCapture_.LastFailureKind();
        const bool phone = selectedCameraIndex_ >= 0 &&
            static_cast<size_t>(selectedCameraIndex_) < cameras_.size() &&
            !cameras_[static_cast<size_t>(selectedCameraIndex_)].phoneSerial.empty();
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
        if (phone) {
            const auto& descriptor = cameras_[static_cast<size_t>(selectedCameraIndex_)];
            phoneRetries_[descriptor.phoneSerial].Failed(
                static_cast<std::int64_t>(GetTickCount64()), kind == CameraFailureKind::DeviceBusy ||
                    mediaCapture_.PhoneProblemCode() == "busy" || mediaCapture_.PhoneProblemCode() == "sessionBusy" ||
                    mediaCapture_.PhoneProblemCode() == "hot");
            qWarning() << "Phone camera start delayed:" << message;
            lastCameraError_ = message;
            StopCameraCapture();
            UpdateProcessingStatusLabel();
            if (interactive) ShowStatusMessage(message, 12000, LivePoliteness::kAssertive);
            QTimer::singleShot(100, this, &OkuFlowApp::StartFallbackCamera);
        } else if (cameraIntent_.Requested().origin == agent::CameraSourceOrigin::Reconnect &&
                   pipelineOrchestrator_->IsCameraReconnectPending()) {
            qWarning() << "Camera reconnect attempt failed:" << message;
            lastCameraError_ = message;
            StopCameraCapture();
            pipelineOrchestrator_->ScheduleCameraReconnectRetry(QDateTime::currentMSecsSinceEpoch());
            UpdateProcessingStatusLabel();
        } else if (interactive &&
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
    if (!cameraIntent_.Activate(startingCameraIntentGeneration_,
        {currentCameraAccelerationKey_,QString::fromStdWString(mediaCapture_.NegotiatedFormat().stableId),false},
        cameraIntent_.Requested().origin == agent::CameraSourceOrigin::Fallback)) {
        StopCameraCapture();
        return false;
    }
    if (controlService_) controlService_->SourceChanged();
    if (selectedCameraIndex_ >= 0 && static_cast<size_t>(selectedCameraIndex_) < cameras_.size() &&
        !cameras_[static_cast<size_t>(selectedCameraIndex_)].phoneSerial.empty()) {
        phoneRetries_[cameras_[static_cast<size_t>(selectedCameraIndex_)].phoneSerial].Succeeded();
        const auto [minimum, maximum] = mediaCapture_.PhoneZoomRange();
        const auto key = QString::fromStdWString(cameras_[size_t(selectedCameraIndex_)].symbolicLink);
        mediaCapture_.SetPhoneZoom(std::clamp(settingsController_->Settings().phoneCameraZoom.value(key, 1.0f), minimum, maximum));
        pipelineOrchestrator_->CancelCameraReconnect();
    }
    if (cameraIntent_.FillStartupIdentity(currentCameraAccelerationKey_,selectedCameraIndex_))
        settingsController_->InitializeLegacyCameraIdentity(selectedCameraIndex_,currentCameraAccelerationKey_);
    if (cameraIntent_.TemporaryOwner().isEmpty()) {
        auto& saved = settingsController_->MutableSettings();
        saved.cameraFormatStableId = cameraIntent_.Requested().identity.formatStableId;
        cameraIntent_.RememberAdoptedFormat(startingCameraIntentGeneration_,saved.cameraFormatStableId);
        const auto working = saved.cameraWorkingSets.constFind(currentCameraAccelerationKey_);
        if (working != saved.cameraWorkingSets.cend()) SetRotationQuarterTurns(working->rotationQuarterTurns);
        else if (saved.cameraWorkingSets.size() < 64)
            saved.cameraWorkingSets.insert(currentCameraAccelerationKey_, {saved.cameraFormatStableId, CameraOriginalRotationQuarterTurns()});
    }
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

void OkuFlowApp::StopCameraCaptureAtExit() {
    const bool atProcessExit = true;
    if (controlTarget_) controlTarget_->CancelObservations();
    if (controlTarget_) controlTarget_->CancelPhotoReadback();
    if (controlService_) controlService_->SourceChanged();
    CancelPendingAssistantCaptures();
    pendingSceneCaptureClock100ns_.reset();
    currentCameraCaptureClock100ns_ = -1;
    const bool startupWasPending = cameraStartupPending_;
    if (pendingCameraStartupCancel_) pendingCameraStartupCancel_->store(true);
    pendingCameraStartupCancel_.reset();
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
    presentationBuffer_.clear();
    presentationWidth_ = 0;
    presentationHeight_ = 0;
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
        ++uctWarpRevision_;
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
        // A reopened reader failed before presenting video. Keep the original
        // deadline and retire this attempt; opening alone is not reconnection.
        if (cameraActive_) {
            StopCameraCapture();
            pipelineOrchestrator_->ScheduleCameraReconnectRetry(now);
            UpdateProcessingStatusLabel();
        }
        return;
    }
    qWarning() << "Camera connection lost; reconnecting automatically";
    const bool phone = selectedCameraIndex_ >= 0 &&
        static_cast<size_t>(selectedCameraIndex_) < cameras_.size() &&
        !cameras_[static_cast<size_t>(selectedCameraIndex_)].phoneSerial.empty();
    if (phone) {
        phoneRetries_[cameras_[static_cast<size_t>(selectedCameraIndex_)].phoneSerial]
            .Failed(static_cast<std::int64_t>(GetTickCount64()));
    }
    lastCameraError_ = QStringLiteral("Reconnecting to camera…");
    StopCameraCapture();
    UpdateProcessingStatusLabel();
    if (phone) QTimer::singleShot(100, this, &OkuFlowApp::StartFallbackCamera);
}

void OkuFlowApp::DriveCameraReconnect() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!pipelineOrchestrator_->IsCameraReconnectPending()) return;
    const std::wstring targetLink = cameraIntent_.Requested().identity.symbolicLink.toStdWString();
    if (targetLink.starts_with(L"okuflow-phone:")) return; // phone has separate retry ownership
    // Expiry also applies while opening or waiting for the first video frame;
    // a successful reader activation must not renew this recovery window.
    if (pipelineOrchestrator_->CameraReconnectExpired(now)) {
        pipelineOrchestrator_->CancelCameraReconnect();
        if (pendingCameraStart_ && pendingCameraStart_->origin == agent::CameraSourceOrigin::Reconnect)
            pendingCameraStart_.reset();
        if (cameraIntent_.Requested().origin == agent::CameraSourceOrigin::Reconnect &&
            (cameraActive_ || cameraStartupPending_)) StopCameraCapture();
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

    if (!pipelineOrchestrator_->CameraReconnectDue(now) || cameraActive_ ||
        cameraStartupPending_ || cameraStopPending_ ||
        (startupWorkers_ && startupWorkers_->active.load()!=0)) return;
    // Wait for video from an adopted reader; never reopen it on every idle tick.
    // Physical activation stays on the startup worker.
    int matchIndex = -1;
    if (!targetLink.empty()) {
        for (size_t i = 0; i < cameras_.size(); ++i) {
            if (cameras_[i].symbolicLink == targetLink) {
                matchIndex = static_cast<int>(i);
                break;
            }
        }
    }

    if (matchIndex >= 0 && StartCameraCapture(static_cast<size_t>(matchIndex), false,false,false,agent::CameraSourceOrigin::Reconnect)) {
        // Accepted is not ready. Completion owns retry/cancel and updates
        // the mode list only after the worker's reader has been adopted.
        return;
    }

    pipelineOrchestrator_->ScheduleCameraReconnectRetry(now);

    // The orchestrator schedules 2s/4s/8s retry backoff.
}



void OkuFlowApp::RefreshAsyncSuperRes() {
    if (MediaViewSelected() || !cameraActive_ || !usingCudaLastFrame_ || !cudaSceneReady_ || !cudaSurface_ ||
        !presenter_ || !presenter_->IsInitialized() || !cudaSurface_->HasSuperResWork() ||
        pipelineOrchestrator_->IsViewportInteractionActive()) return;
    FenceSyncParams sync{};
    if (pipelineOrchestrator_->FenceInteropEnabled()) {
        sync.enable = true;
        sync.waitValue = presenter_->LastGraphicsSignal();
        sync.signalValue = presenter_->ReserveExternalSignal();
        if (!sync.signalValue) { HandlePresenterFault(); return; }
    } else {
        // A missing semaphore requires both APIs to be idle before a shared
        // write. Poll once; never wait for graphics or inference on this clock.
        auto* fence = presenter_->GetFence();
        if (!fence || fence->GetCompletedValue() < presenter_->LastGraphicsSignal()) return;
    }
    const auto result = cudaSurface_->RefreshSuperRes(sync);
    if (result == SuperResRefreshResult::Failed) {
        HandlePresenterFault(); // uncertain shared work retains its reservation
        return;
    }
    if (sync.enable) {
        const bool accepted = result == SuperResRefreshResult::NoWork
            ? presenter_->CancelExternalSignal(sync.signalValue)
            : presenter_->CommitExternalSignal(sync.signalValue);
        if (!accepted) { HandlePresenterFault(); return; }
    }
    if (result == SuperResRefreshResult::Published) {
        ++annotationSceneRevision_;
        asyncSuperResPresentPending_ = !sync.enable;
        pipelineOrchestrator_->MarkSceneDirty();
    }
}

void OkuFlowApp::PresentPendingScene() {
    RefreshZoomRange();
    if (MediaViewSelected()) {
        if (presenter_ && !HandlePresenterFault()) PresentMediaView();
        return;
    }
    RefreshAsyncSuperRes();
    if (!presenter_ || HandlePresenterFault() || !cameraActive_ ||
        (!pipelineOrchestrator_->IsViewportDirty() &&
         !presenter_->NeedsScenePresent())) {
        return;
    }
    if (usingCudaLastFrame_ && cudaSceneReady_) {
        PresentLatestCudaScene(false, nullptr);
    } else if (!usingCudaLastFrame_ && cpuSceneReady_) {
        PresentFitted(
            cpuSceneBuffer_.data(), cpuSceneWidth_, cpuSceneHeight_,
            pipelineOrchestrator_->ViewportFitMode() ==
                settings::ViewportFitModeSetting::Fill,
            zoomCenterX_, zoomCenterY_, nullptr);
    }
}

bool OkuFlowApp::RunFrameTick(double elapsedSeconds) {
    UpdatePhoneLightUi();
    PollSelectedVideoSave();
    if (HandlePresenterFault()) return false;
    // A completed capture is independent of another camera arrival or present.
    // Poll on idle ticks too, before any camera/readiness early return.
    DrainCompletedGpuReadbacks();
    if (!MediaViewSelected()) {
        mediaPresentedScenePixels_.reset();
        mediaViewportAdmission_.displayed.reset();
    }
    if (MediaViewSelected() || (!LiveRecapActive() &&
        (recordingManager_ && !recordingManager_->CurrentSessionInfo() && mediaHoldReadbackId_ == 0))
        )
        presentedOriginalCache_.Reset();
    // Camera loss / reconnect state machine. ConsumeDeviceLost() is polled
    // here in addition to the capture error callback so the reconnect starts
    // no matter which side notices the loss first.
    if (mediaCapture_.ConsumeDeviceLost()) {
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

    if (ApplyInputForces(elapsedSeconds)) {
        pipelineOrchestrator_->MarkViewportDirty();
    }
    if (!cameraActive_ || !cameraIngress_) {
        return false;
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
        return false;
    }

    const auto retainReadbackFrame = [&]() {
        deferredGpuCameraFrame_ = std::move(frame);
        pipelineOrchestrator_->NotifyCameraFrameAvailable(1);
    };
    // Finish the single retained copy before trying another direct GPU route.
    // This also applies when a photo was canceled while its copy was pending.
    if (mediaCapture_.HasPendingGpuReadback()) {
        const auto result = mediaCapture_.ReadbackGpuFrame(frame);
        if (result == GpuFrameReadbackResult::Pending) retainReadbackFrame();
        if (result != GpuFrameReadbackResult::Ready) return false;
    }

    currentCameraCaptureClock100ns_ = frame.captureClock100ns;
    currentCameraFrameSequence_ = frame.sequenceNumber;
    ObserveLiveRecapSource(frame);
    CapturedFrame originalFrame;
    bool recordingActive = recordingManager_ && recordingManager_->IsActive();
    bool cpuOriginalRequired = photoCapturePending_ || (LiveRecapActive() && !pipelineOrchestrator_->FenceInteropEnabled());
    if (cpuOriginalRequired && frame.data.empty() && frame.IsGpuResident()) {
        const auto result = mediaCapture_.ReadbackGpuFrame(frame);
        if (result == GpuFrameReadbackResult::Pending) {
            retainReadbackFrame();
            return false;
        }
        if (result == GpuFrameReadbackResult::Failed && photoCapturePending_) {
            if (controlTarget_) controlTarget_->PhotoState(photoOperationId_,agent::OperationState::Failed);
            ShowStatusMessage(
                QStringLiteral(
                    "Photo not saved: the original camera frame could not be read back."),
                8000);
            photoCapturePending_ = false;
            cpuOriginalRequired = false;
            return false;
        }
    }
    if (cpuOriginalRequired) {
        if (!PrepareOriginalFrame(frame, originalFrame) && recordingActive) {
            if (const auto session = recordingManager_->CurrentSessionInfo()) OnVideoRecordingStopping(*session);
            recordingManager_->Stop(QStringLiteral(
                "Recording stopped: the original camera frame could not be converted."));
            recordingActive = false;
        }
    } else if (recordingActive || LiveRecapActive()) {
        PopulateOriginalFrameMetadata(frame, originalFrame);
    }

    // With no Original/photo consumer, a hidden camera frame needs no pixel
    // processing. Ingress and device-loss handling continue independently.
    if (MediaViewSelected() && !recordingActive && !LiveRecapActive() && !photoCapturePending_) return true;

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
        if (cudaSubmissionBusy_) {
            // Drop this camera arrival rather than retrying or converting it;
            // the independent viewport callback presents the cached scene.
            if (recordingManager_) recordingManager_->NotifyCaptureGpuUploadBusy();
            return false;
        }
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
                gpuCompletionPending == CaptureGpuPending::Readback ||
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
    if (frame.data.empty() && frame.IsGpuResident()) {
        const auto result = mediaCapture_.ReadbackGpuFrame(frame);
        if (result == GpuFrameReadbackResult::Pending) retainReadbackFrame();
        if (result != GpuFrameReadbackResult::Ready) return false;
    }
    if ((recordingActive || LiveRecapActive()) && !originalFrame.HasCpuPixels() &&
        !PrepareOriginalFrame(frame, originalFrame)) {
        if(recordingActive) {
        if (const auto session = recordingManager_->CurrentSessionInfo()) OnVideoRecordingStopping(*session);
        recordingManager_->Stop(QStringLiteral(
            "Recording stopped: the original camera frame could not be converted."));
        }
        recordingActive = false;
    }
    if (recordingActive || LiveRecapActive()) AdmitOriginalFrame(originalFrame, false);
    if (MediaViewSelected()) return true;
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
    BuildCompositeAndPresent(width, height, &originalFrame);
    if (cudaSubmissionBusy_) {
        if (recordingManager_) recordingManager_->NotifyCaptureGpuUploadBusy();
        return false;
    }
    if (photoCapturePending_ && !usingCudaLastFrame_) {
        CapturePendingPhoto(originalFrame);
    }
    return true;
}
void OkuFlowApp::BuildCompositeAndPresent(UINT width,
                                           UINT height,
                                           CapturedFrame* originalFrame) {
    cudaSubmissionBusy_ = false;
    if (!debugViewEnabled_ && ProcessFrameWithCuda(width, height)) {
        processedFrameWidth_ = width;
        processedFrameHeight_ = height;
        usingCudaLastFrame_ = true;
        // Original admission already happened on the camera producer path.
        // Periodic assistive grabs use the asynchronous viewport readback ring.
        pendingSceneCaptureClock100ns_ = currentCameraCaptureClock100ns_;
        ++startupProcessedScenes_;
        RetainOriginalForPresentation(presentedOriginalCache_, originalFrame,
            LiveOriginalRetentionId(),
            cameraSessionId_, CameraOriginalRotationQuarterTurns(), cudaSceneViewRecipe_);
        PresentLatestCudaScene(true, originalFrame);
        return;
    }

    if (cudaSubmissionBusy_) {
        return;
    }
    processedFrameWidth_ = width;
    processedFrameHeight_ = height;
    usingCudaLastFrame_ = false;

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
        // CPU fallback applies rotation only: no keystone, stabilization,
        // color scheme or temporal composite.
        ++annotationSceneRevision_;
        uctProvenance_.OnCpuSceneProcessed(cameraSessionId_, rotationQuarterTurns_, width, height,
                                           currentCameraFrameSequence_, false);
        if (controlTarget_) controlTarget_->FrameProcessed();
        pendingSceneCaptureClock100ns_ = currentCameraCaptureClock100ns_;
        ++startupProcessedScenes_;
        RetainOriginalForPresentation(presentedOriginalCache_, originalFrame,
            LiveOriginalRetentionId(),
            cameraSessionId_, CameraOriginalRotationQuarterTurns(), {});
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
    presentedOriginalCache_.Reset(); // diagnostic panels have no single clean-source view
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
    ++annotationSceneRevision_;
    uctProvenance_.OnCpuSceneProcessed(cameraSessionId_, rotationQuarterTurns_, output.width,
                                       output.height, currentCameraFrameSequence_,
                                       config.enableTemporalSmooth);
    if (controlTarget_) controlTarget_->FrameProcessed();
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

void OkuFlowApp::ExpireAnnotationReadback() {
    if (pendingAnnotationReadbackId_ != 0 &&
        ((pendingAnnotationReadbackTimer_.isValid() && pendingAnnotationReadbackTimer_.elapsed() > 1000) ||
         (presenter_ && !presenter_->HasReadbackRequest(pendingAnnotationReadbackId_)))) {
        pendingAnnotationReadbackId_ = 0;
        pendingAnnotationReadbackTimer_.invalidate();
        // This intent belongs to the copied frame. A fresh frame cannot
        // replace an expired readback, even if it has the same dimensions.
        activeAnnotationCapture_.reset();
        ShowStatusMessage(QStringLiteral("Annotated view not saved: the viewport was unavailable."));
    }
}

bool OkuFlowApp::PresentSelectedMediaPixels(const uint8_t* data, UINT width, UINT height,
                                           UINT stride, std::uint64_t sourceEpoch,
                                           std::uint64_t frameSeq,
                                           const VideoPlaybackFrame* /*neighbor*/,
                                           std::int64_t pts100ns,
                                           bool continuousPlayback,
                                           uct::ImmutableCorroborationPolicy /*corroborationPolicy*/) {
    if (!data || !VideoPlaybackLayoutFits(width, height, stride) || !presenter_ ||
        !uiState_->renderWidget_ || !uiState_->renderWidget_->isPresenterReady()) return false;
    if (HandlePresenterFault()) return false;
    const int turns = ((rotationQuarterTurns_ % 4) + 4) % 4;
    const int colorTransform = displayColorScheme_.id == QStringLiteral("normal") ? 0 :
        displayColorScheme_.id == QStringLiteral("inverted") ? 1 : 2;
    const std::array<std::uint64_t, 12> recipe{
        sourceEpoch, frameSeq, width, height, stride, static_cast<std::uint64_t>(turns),
        displayColorLutGeneration_, std::bit_cast<std::uint32_t>(contrast_),
        std::bit_cast<std::uint32_t>(brightness_), static_cast<std::uint64_t>(blackWhiteEnabled_),
        std::bit_cast<std::uint32_t>(blackWhiteThreshold_), static_cast<std::uint64_t>(colorTransform)};
    const bool changed = !mediaSceneCacheValid_ || recipe != mediaSceneRecipe_;
    const std::uint64_t neighborId = 0; // standalone held-frame analysis
    const uct::ImmutableCorroborationPolicy corroborationPolicy{};
    const bool wasContinuousPlayback = mediaUctContinuousPlayback_;
    std::optional<UctImmutableScene> pausedScene;
    ViewTransform transform;
    MediaViewportAdmission::Stamp viewportStamp;
    bool needsPresentation = true;
    try {
        if (changed) {
            // A deferred decoder-ahead candidate must not change the owned
            // source lease of the actual displayed frame selected by Save.
            auto prepared = mediaViewportAdmission_.displayed &&
                    mediaViewportAdmission_.displayed->recipe == recipe && mediaPresentedScenePixels_
                ? mediaPresentedScenePixels_
                : PrepareMediaScene(data, width, height, stride, turns,
                    mediaHoldBakedScene_ ? false : blackWhiteEnabled_,blackWhiteThreshold_,
                    mediaHoldBakedScene_ ? 0 : colorTransform,displayColorLut_,
                    mediaHoldBakedScene_ ? 1.0f : contrast_,mediaHoldBakedScene_ ? 0.0f : brightness_);
            if (!prepared) return false;
            if (!mediaSceneCacheValid_ || !std::equal(recipe.begin()+6, recipe.end(), mediaSceneRecipe_.begin()+6))
                ++mediaSceneAppearanceRevision_;
            mediaScenePixels_ = std::move(prepared);
            mediaSceneWidth_ = mediaScenePixels_->width;
            mediaSceneHeight_ = mediaScenePixels_->height;
            mediaSceneRecipe_ = recipe;
            mediaSceneCacheValid_ = true;
            ++annotationSceneRevision_;
            if (controlTarget_) controlTarget_->FrameProcessed();
        }
        transform = ComputeViewTransform(mediaSceneWidth_, mediaSceneHeight_,
            presenter_->ViewportWidth(), presenter_->ViewportHeight(),
            zoomEnabled_ ? zoomAmount_ : 1.0f, zoomCenterX_, zoomCenterY_,
            pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fit
                ? ViewportFitMode::kFit : ViewportFitMode::kFill);
        if (!transform.valid) return false;
        const bool drawMarker = focusMarkerEnabled_ &&
            pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fill;
        viewportStamp = {recipe, transform, presenter_->ViewportWidth(), presenter_->ViewportHeight(),
            drawMarker, drawMarker ? zoomCenterX_ : 0.0f, drawMarker ? zoomCenterY_ : 0.0f};
        needsPresentation = mediaViewportAdmission_.NeedsPresentation(viewportStamp,
            pipelineOrchestrator_->IsViewportDirty(), presenter_->NeedsScenePresent()) ||
            !lastAnnotationPresentation_ || presentationBuffer_.empty() ||
            presentationWidth_ != viewportStamp.width || presentationHeight_ != viewportStamp.height;
        if (ultraClearText_ && !continuousPlayback && !mediaHoldBakedScene_) {
            if (changed || wasContinuousPlayback || ultraClearText_->HasAdvancingMediaScene() ||
                !ultraClearText_->HasImmutableScene() ||
                corroborationPolicy != mediaUctCorroborationPolicy_ ||
                neighborId != mediaUctNeighborSeq_) {
                UctImmutableScene scene;
                scene.admission = uct::ImmutableAdmission::Standalone;
                scene.geometry.settings.sourceEpoch = sourceEpoch;
                scene.geometry.settings.rotationQuarterTurns = turns;
                scene.geometry.settings.colorScheme = mediaSceneAppearanceRevision_;
                scene.geometry.sceneWidth = mediaSceneWidth_;
                scene.geometry.sceneHeight = mediaSceneHeight_;
                scene.sourceAuthority = sourceEpoch;
                scene.selected = {mediaScenePixels_, frameSeq, pts100ns / 10'000};
                scene.corroborationPolicy = corroborationPolicy;
                scene.transform = transform;
                scene.viewportWidth = presenter_->ViewportWidth();
                scene.viewportHeight = presenter_->ViewportHeight();
                scene.strokeWeight = 0; // CUDA stroke modification is not applied here.
                pausedScene = std::move(scene);
            }
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    const auto advancingReceipt = [&] {
        if (!ultraClearText_) return;
        ApplyUltraClearTextDisplayColors(0); // CPU analysis already contains its display colors.
        ultraClearText_->SetMediaOverlaySuppressed(false);
        UctAdvancingMediaScene scene;
        scene.geometry.settings.sourceEpoch = sourceEpoch;
        scene.geometry.settings.rotationQuarterTurns = turns;
        scene.geometry.settings.colorScheme = mediaSceneAppearanceRevision_;
        scene.geometry.sceneWidth = mediaSceneWidth_;
        scene.geometry.sceneHeight = mediaSceneHeight_;
        scene.sourceAuthority = sourceEpoch;
        scene.current = {mediaScenePixels_, frameSeq, pts100ns / 10'000};
        scene.transform = transform;
        scene.viewportWidth = viewportStamp.width;
        scene.viewportHeight = viewportStamp.height;
        scene.strokeWeight = 0;
        ultraClearText_->OnAdvancingMediaPresented(std::move(scene));
    };
    const auto pausedReceipt = [&] {
        if (!ultraClearText_) return;
        ApplyUltraClearTextDisplayColors(0);
        if(mediaHoldBakedScene_){ultraClearText_->SetMediaOverlaySuppressed(true);return;}
        ultraClearText_->SetMediaOverlaySuppressed(false);
        if (pausedScene) {
            ultraClearText_->SetImmutableScene(std::move(*pausedScene));
            mediaUctNeighborSeq_ = neighborId;
            mediaUctCorroborationPolicy_ = corroborationPolicy;
        } else if (needsPresentation) {
            ultraClearText_->UpdateImmutableView(transform, viewportStamp.width, viewportStamp.height, 0);
        }
    };
    if (!needsPresentation) {
        // This exact frame/recipe/view still has its successful native receipt
        // and retained viewport. Polling and explicit Save may reuse it without
        // clearing, resampling or uploading the same pixels again.
        if (continuousPlayback && (!wasContinuousPlayback || (ultraClearText_ &&
            ultraClearText_->AdvancingMediaRetryRequested(sourceEpoch, frameSeq, pts100ns / 10'000)))) advancingReceipt();
        else if (!continuousPlayback) pausedReceipt();
        mediaUctContinuousPlayback_ = continuousPlayback;
        return true;
    }
    lastAnnotationPresentation_.reset();
    PresentFitted(mediaScenePixels_->bytes.data(), mediaSceneWidth_, mediaSceneHeight_,
        pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fill,
        zoomCenterX_, zoomCenterY_, nullptr, true);
    const bool presented = lastAnnotationPresentation_.has_value() && !presenter_->IsFaulted() &&
        !presenter_->NeedsScenePresent();
    if (presented) {
        mediaViewportAdmission_.Presented(viewportStamp);
        mediaPresentedScenePixels_ = mediaScenePixels_;
        mediaPresentedWithCuda_ = mediaSuperResPresented_ = false;
        if (controlTarget_) {
            auto observation = controlTarget_->PresentedMediaFrame(transform,
                QSize(mediaSceneWidth_, mediaSceneHeight_), QSize(viewportStamp.width, viewportStamp.height),
                frameSeq, SelectedMediaDocumentPts100ns(pts100ns));
            observation.diagnostics["media"] = QJsonObject{
                {"sourceEpoch", qint64(sourceEpoch)}, {"frameSequence", qint64(frameSeq)},
                {"backend", "cpu-output"}};
            controlTarget_->CpuObservationPresented(QImage(presentationBuffer_.data(), viewportStamp.width,
                viewportStamp.height, viewportStamp.width * 4, QImage::Format_ARGB32), observation);
        }
        if (continuousPlayback) advancingReceipt();
        else pausedReceipt();
        mediaUctContinuousPlayback_ = continuousPlayback;
    }
    return presented;
}

void OkuFlowApp::PresentFitted(const uint8_t* data,
                                UINT srcWidth,
                                UINT srcHeight,
                                bool cropToFill,
                                float centerXNorm,
                                float centerYNorm,
                                const CapturedFrame* originalFrame,
                                bool selectedMedia) {
    if (!selectedMedia && MediaViewSelected()) {
        PresentMediaView();
        return;
    }
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

    // The mutable CPU viewport no longer represents the last successful
    // present until the new submission below succeeds.
    lastAnnotationPresentation_.reset();
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

    // CPU presentation already owns system-memory viewport pixels; copy them
    // for Ultra Clear Text before the focus marker is drawn into them.
    if (ultraClearText_ && !selectedMedia) {
        const UctFrameContext uctContext = UltraClearTextContext(
            false, transform, mapping.targetWidth, mapping.targetHeight);
        if (ultraClearText_->OnPresented(uctContext)) {
            auto analysisContext=uctContext;
            analysisContext.transform={};analysisContext.transform.valid=true;
            analysisContext.viewportWidth=srcWidth;analysisContext.viewportHeight=srcHeight;
            ultraClearText_->OnCpuViewport(
                analysisContext, data, static_cast<int>(srcWidth),
                static_cast<int>(srcHeight), static_cast<int>(srcStride));
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

    const bool focusGateEnabled = autoTextClarityEnabled_;
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
    presenter_->Present(presentationBuffer_.data(), mapping.targetWidth, mapping.targetHeight);
    if (presenter_->IsFaulted() || presenter_->NeedsScenePresent()) {
        if (ultraClearText_) {
            if (presenter_->IsFaulted()) {
                if (selectedMedia) ultraClearText_->OnMediaPresentationFailed();
                else ultraClearText_->OnPresentationFailed();
            } else ultraClearText_->OnPresentationDeferred();
        }
    }
    if (presenter_->IsFaulted()) {
        lastAnnotationPresentation_.reset();
        lastAnnotationGpuPresentation_.reset();
        return;
    }
    if (presenter_->NeedsScenePresent()) {
        lastAnnotationPresentation_.reset();
        lastAnnotationGpuPresentation_.reset();
        pipelineOrchestrator_->MarkViewportDirty();
        return;
    }
    if (ultraClearText_) {
        if (selectedMedia) ultraClearText_->OnMediaPresentationSucceeded();
        else ultraClearText_->OnPresentationSucceeded();
    }
    ApplyUltraClearTextDisplayColors(0); // Raw CPU camera / pre-graded CPU media.
    if (!selectedMedia) cameraFramePresented_ = true;
    if (!selectedMedia) {
        lastAnnotationPresentation_ = UltraClearTextContext(false, transform,
            mapping.targetWidth, mapping.targetHeight);
        presentedOriginalCache_.Presented(cameraSessionId_, lastAnnotationPresentation_->frameSeq,
            transform, !lastAnnotationPresentation_->compositeFrames && !debugViewEnabled_ &&
            !focusMarkerEnabled_, cropToFill ? ViewportFitMode::kFill : ViewportFitMode::kFit,
            zoomEnabled_ && zoomAmount_ <= kWholeSourceZoom);
        RetainPresentedCameraScene(lastAnnotationPresentation_->geometry.settings.sourceEpoch,
            lastAnnotationPresentation_->frameSeq,pendingSceneCaptureClock100ns_,
            nullptr,srcWidth,srcHeight,transform,data,0,focusMarkerEnabled_ && cropToFill,centerXNorm,centerYNorm);
    } else {
        UctFrameContext context;
        context.geometry.settings.sourceEpoch = mediaSceneRecipe_[0];
        context.geometry.settings.rotationQuarterTurns = static_cast<int>(mediaSceneRecipe_[5]);
        context.geometry.settings.colorScheme = mediaSceneAppearanceRevision_;
        context.geometry.sceneWidth = srcWidth;
        context.geometry.sceneHeight = srcHeight;
        context.requested = context.geometry.settings;
        context.frameSeq = mediaSceneRecipe_[1];
        context.transform = transform;
        context.viewportWidth = mapping.targetWidth;
        context.viewportHeight = mapping.targetHeight;
        lastAnnotationPresentation_ = context;
    }
    lastAnnotationSceneRevision_ = annotationSceneRevision_;
    lastAnnotationGpuPresentation_.reset();
    annotationPresentationCuda_ = false;
    if (!selectedMedia) {
        cameraIntent_.MarkFirstFrame(startingCameraIntentGeneration_);
        RecordStartupFirstPresent();
    }
    if (!selectedMedia && pendingSceneCaptureClock100ns_) {
        pipelineOrchestrator_->RecordCaptureToPresentSample(
            MeasureCaptureToPresentLatency(*pendingSceneCaptureClock100ns_));
        pendingSceneCaptureClock100ns_.reset();
    }
    pipelineOrchestrator_->MarkViewportPresented();
    if (controlTarget_ && !selectedMedia) {
        auto frame = controlTarget_->PresentedFrame(transform,QSize(srcWidth,srcHeight),QSize(mapping.targetWidth,mapping.targetHeight));
        controlTarget_->CpuObservationPresented(QImage(presentationBuffer_.data(),mapping.targetWidth,mapping.targetHeight,mapping.targetWidth*4,QImage::Format_ARGB32),frame);
    }
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
    capturePipeline_.RotateRawBuffer(CameraOriginalRotationQuarterTurns(), width, height);
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
    const int turns = ((CameraOriginalRotationQuarterTurns() % 4) + 4) % 4;
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
        if (controlTarget_) controlTarget_->PhotoState(photoOperationId_,agent::OperationState::Failed);
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
        if (controlTarget_) controlTarget_->PhotoState(photoOperationId_,agent::OperationState::Failed);
        ShowStatusMessage(QStringLiteral("Photo not saved: no processed frame was available."));
        return;
    }
    pendingPhotoOverlays_ = lastAnnotationPresentation_ &&
        lastAnnotationPresentation_->frameSeq == originalFrame.identity.sequenceNumber &&
        lastAnnotationPresentation_->viewportWidth == presentationWidth_ &&
        lastAnnotationPresentation_->viewportHeight == presentationHeight_
        ? CaptureViewOverlays(lastAnnotationPresentation_->transform) : ViewOverlaySnapshot{};
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
    auto overlays = std::exchange(pendingPhotoOverlays_, {});
    const QString operationId = photoOperationId_;
    if (!processedData || processedWidth == 0 || processedHeight == 0 ||
        !originalFrame.HasCpuPixels()) {
        if (controlTarget_) controlTarget_->PhotoState(operationId,agent::OperationState::Failed);
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
        if (controlTarget_) controlTarget_->PhotoState(operationId,agent::OperationState::Failed);
        ShowStatusMessage(
            outputError.isEmpty()
                ? QStringLiteral(
                      "Photo not saved: the OkuFlow Photos folder is unavailable.")
                : outputError);
        return;
    }
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString preferredStem = QStringLiteral("IMG_%1").arg(timestamp);

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
        if (controlTarget_) controlTarget_->PhotoState(operationId,agent::OperationState::Failed);
        ShowStatusMessage(
            QStringLiteral("Photo not saved: the image could not be queued."));
        return;
    }

    ShowStatusMessage(QStringLiteral("Saving original and processed photos..."),
                      2500);
    QPointer<OkuFlowApp> owner(this);
    if (controlTarget_) controlTarget_->PhotoState(operationId,agent::OperationState::Writing);
    const bool queued = imageIoPool_ && imageIoPool_->tryStart(
        [owner,
         operationId,
         processedImage = std::move(processedImage),
         originalImage = std::move(originalImage),
         overlays = std::move(overlays),
         dirPath,
         preferredStem]() mutable {
            {
                QPainter painter(&processedImage);
                overlays.Paint(painter, processedImage.size());
            }
            const PhotoPairWriteResult saved = UserDataPaths::WritePhotoPair(
                dirPath, preferredStem,
                [&originalImage](QIODevice* file) {
                    return originalImage.save(file, "JPG", 90);
                },
                [&processedImage](QIODevice* file) {
                    return processedImage.save(file, "JPG", 90);
                });
            const bool committed = saved.committed;
            const QString originalPath = saved.originalPath;
            const QString processedPath = saved.processedPath;
            QStringList leftoverPaths;
            for (const QString& path : saved.leftoverPaths) {
                leftoverPaths.append(QDir::toNativeSeparators(path));
            }
            if (!owner) {
                return;
            }
            QMetaObject::invokeMethod(
                owner,
                [owner,
                 operationId,
                 committed,
                 leftoverPaths,
                 processedPath,
                 originalPath]() {
                    if (!owner) {
                        return;
                    }
                    if (owner->controlTarget_) owner->controlTarget_->PhotoState(operationId,
                        committed ? agent::OperationState::Committed : agent::OperationState::Failed,
                        committed ? QJsonObject{{"originalName",QFileInfo(originalPath).fileName()},{"processedName",QFileInfo(processedPath).fileName()}} : QJsonObject{});
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
        if (controlTarget_) controlTarget_->PhotoState(operationId,agent::OperationState::Failed);
        ShowStatusMessage(
            QStringLiteral(
                "Photo not saved: the image writer is busy. Try again."));
    }
}

bool OkuFlowApp::AnnotationFrameMatches(const UctFrameContext& candidate) const
{
    if (annotationCaptureQueue_.empty()) return false;
    const auto& capture = annotationCaptureQueue_.front();
    const auto& frame = capture.frame;
    const auto& a = frame.transform;
    const auto& b = candidate.transform;
    return frame.cameraActive == candidate.cameraActive &&
        (frame.cameraActive || MediaViewSelected()) &&
        capture.sceneRevision == annotationSceneRevision_ &&
        frame.geometry == candidate.geometry && frame.requested == candidate.requested &&
        frame.frameSeq == candidate.frameSeq &&
        frame.viewportWidth == candidate.viewportWidth && frame.viewportHeight == candidate.viewportHeight &&
        frame.compositeFrames == candidate.compositeFrames &&
        a.valid && b.valid && a.sourceX == b.sourceX && a.sourceY == b.sourceY &&
        a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight &&
        a.destinationX == b.destinationX && a.destinationY == b.destinationY &&
        a.destinationWidth == b.destinationWidth && a.destinationHeight == b.destinationHeight;
}

bool OkuFlowApp::AnnotationGpuPresentationMatches(const AnnotationGpuPresentation& candidate) const
{
    if (annotationCaptureQueue_.empty()) return false;
    const auto& before = annotationCaptureQueue_.front().presentation;
    const auto& a = before.transform;
    const auto& b = candidate.transform;
    return before.texture == candidate.texture && before.width == candidate.width &&
        before.height == candidate.height && before.superRes == candidate.superRes &&
        before.focusMarker == candidate.focusMarker &&
        (!before.focusMarker || (before.focusX == candidate.focusX && before.focusY == candidate.focusY)) &&
        a.valid == b.valid && a.sourceX == b.sourceX && a.sourceY == b.sourceY &&
        a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight &&
        a.destinationX == b.destinationX && a.destinationY == b.destinationY &&
        a.destinationWidth == b.destinationWidth && a.destinationHeight == b.destinationHeight;
}

void OkuFlowApp::CancelAnnotationCaptures()
{
    annotationCaptureQueue_.clear();
    activeAnnotationCapture_.reset();
    pendingAnnotationReadbackId_ = 0;
    pendingAnnotationReadbackTimer_.invalidate();
    lastAnnotationPresentation_.reset();
    lastAnnotationGpuPresentation_.reset();
    ++annotationSceneRevision_;
}

void OkuFlowApp::QueueAnnotationSnapshot(int reason)
{
    if (!mainWindow_ || !mainWindow_->annotationOverlay() || !mainWindow_->annotationOverlay()->IsActive() ||
        !mainWindow_->annotationOverlay()->HasInk()) {
        return;
    }
    // A decoder-ahead candidate can have replaced the reusable scene while
    // the swapchain still shows doc.presented. Regenerate that exact selected
    // frame before reading its authority, rather than checking a revoked
    // receipt and giving up before the recovery path can run.
    if (MediaViewSelected() && pendingAnnotationReadbackId_ == 0 && annotationCaptureQueue_.empty() &&
        !PresentSelectedMediaFrameForCapture()) {
        ShowStatusMessage(QStringLiteral("Annotated view not saved: the viewport was unavailable."));
        return;
    }
    if (!lastAnnotationPresentation_ || (!lastAnnotationPresentation_->cameraActive && !MediaViewSelected()) ||
        pendingAnnotationReadbackId_ != 0 || !annotationCaptureQueue_.empty() ||
        (annotationPresentationCuda_ && !lastAnnotationGpuPresentation_)) {
        ShowStatusMessage(QStringLiteral("Annotated view not saved: the viewport was unavailable."));
        return;
    }
    PendingAnnotationCapture capture;
    capture.frame = *lastAnnotationPresentation_;
    capture.sceneRevision = lastAnnotationSceneRevision_;
    if (lastAnnotationGpuPresentation_) capture.presentation = *lastAnnotationGpuPresentation_;
    capture.transform = capture.frame.transform;
    capture.text = CaptureViewOverlays(capture.transform).text;
    capture.strokes = mainWindow_->annotationOverlay()->Strokes();
    capture.heading =
        reason == 1
            ? QCoreApplication::translate("OkuFlow",
                                          "Annotations cleared - snapshot")
            : reason == 2
                  ? QCoreApplication::translate(
                        "OkuFlow", "Annotation session ended - snapshot")
                  : QCoreApplication::translate("OkuFlow", "Annotated view");
    if (!annotationPresentationCuda_) {
        SaveAnnotationSnapshot(presentationBuffer_.data(), capture.frame.viewportWidth,
            capture.frame.viewportHeight, capture.strokes, capture.transform, capture.text, capture.heading);
        return;
    }
    annotationCaptureQueue_.push_back(std::move(capture));
    // Request the cached, already displayed scene in this UI turn, before a
    // later camera tick can replace it. Never defer the intent to fresh pixels.
    if (MediaViewSelected()) PresentSelectedMediaFrameForCapture();
    else PresentLatestCudaScene(false, nullptr);
    if (!annotationCaptureQueue_.empty()) {
        annotationCaptureQueue_.clear(); // Ring full or unavailable scene: no retarget/retry.
        ShowStatusMessage(QStringLiteral("Annotated view not saved: the viewport was unavailable."));
    } else if (activeAnnotationCapture_) {
        ShowStatusMessage(QStringLiteral("Saving annotated view to lecture notes..."), 2500);
    }
}

void OkuFlowApp::SaveAnnotationSnapshot(
    const uint8_t* processedData,
    UINT processedWidth,
    UINT processedHeight,
    const QVector<AnnotationStroke>& strokes,
    const ViewTransform& transform,
    const UctOverlaySnapshot& text,
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
    const QString preferredStem = QStringLiteral("ANNOTATION_%1").arg(timestamp);
    QPointer<OkuFlowApp> owner(this);
    const bool queued = imageIoPool_ && imageIoPool_->tryStart(
        [owner,
         annotated = std::move(annotated),
         strokes,
         transform,
         text,
         dirPath,
         preferredStem,
         heading]() mutable {
            {
                QPainter painter(&annotated);
                ViewOverlaySnapshot{text, strokes, transform}.Paint(painter, annotated.size());
            }
            const auto result = UserDataPaths::WriteAnnotationImage(dirPath, preferredStem,
                [&annotated](QIODevice* file) { return annotated.save(file, "PNG"); });
            const bool saved = result.committed;
            const QString path = result.path;
            if (!saved) {
                qWarning() << "Failed to publish annotation snapshot; owned temporary leftovers:"
                           << result.leftoverPaths;
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
