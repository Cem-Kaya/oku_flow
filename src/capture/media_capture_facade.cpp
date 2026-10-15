#ifdef _WIN32
#include "media_capture_session.hpp"
#include "okuflow/capture/phone_camera.hpp"

#include <QDebug>
#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace okuflow {
namespace {
// Intentionally no static destructor: a driver or CUDA import may still refer
// to these objects during process teardown. WasAbandoned also prevents MFShutdown.
void RetainAbandonedSession(const std::shared_ptr<MediaCaptureSession>& session)
{
    if (!session->retainedForExit_.exchange(true))
        (void)new std::shared_ptr<MediaCaptureSession>(session);
}
} // namespace

MediaCapture::MediaCapture() : session_(std::make_shared<MediaCaptureSession>()) {}
int ResolveAutomaticPhoneIndex(const std::vector<CameraDescriptor>& cameras,
                              const std::wstring& preferredSymbolicLink,
                              bool allowPhoneSelection, bool requireReady)
{
    if (!allowPhoneSelection) return -1;
    const auto eligible = [requireReady](const CameraDescriptor& camera) {
        return !camera.phoneSerial.empty() && !camera.phoneBusy &&
            camera.phoneProblem.empty() && (!requireReady || camera.phoneAppReady);
    };
    if (preferredSymbolicLink.starts_with(L"okuflow-phone:")) {
        for (size_t i = 0; i < cameras.size(); ++i) {
            if (eligible(cameras[i]) &&
                cameras[i].symbolicLink == preferredSymbolicLink)
                return static_cast<int>(i);
        }
        for (size_t i = 0; i < cameras.size(); ++i) {
            if (eligible(cameras[i]) && cameras[i].phoneCameraId.empty() &&
                preferredSymbolicLink.starts_with(cameras[i].symbolicLink + L"/"))
                return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < cameras.size(); ++i) {
        if (eligible(cameras[i]) && cameras[i].phoneAppReady)
            return static_cast<int>(i);
    }
    return -1;
}

std::vector<CameraDescriptor> MergePhoneCameraDiscovery(
    const std::vector<CameraDescriptor>& previous,
    const std::vector<CameraDescriptor>& discovered,
    const std::wstring& activeSymbolicLink)
{
    std::vector<CameraDescriptor> merged;
    std::string activeSerial;
    for (const auto& camera : previous) {
        if (camera.phoneSerial.empty()) merged.push_back(camera);
        else if (camera.symbolicLink == activeSymbolicLink) activeSerial = camera.phoneSerial;
    }
    for (const auto& camera : discovered) {
        if (!camera.phoneSerial.empty()) merged.push_back(camera);
    }
    for (const auto& old : previous) {
        if (old.phoneSerial.empty()) continue;
        const auto attached = std::find_if(discovered.begin(), discovered.end(),
            [&](const auto& current) { return current.phoneSerial == old.phoneSerial; });
        if (attached == discovered.end() && old.phoneSerial != activeSerial) continue;
        if (std::any_of(merged.begin(), merged.end(),
                [&](const auto& current) { return current.symbolicLink == old.symbolicLink; })) continue;
        auto retained = old;
        if (attached != discovered.end()) {
            retained.phoneTransportId = attached->phoneTransportId;
            retained.phoneServerEpoch = attached->phoneServerEpoch;
            retained.phoneAppReady = attached->phoneAppReady;
            retained.phoneBusy = attached->phoneBusy;
            retained.phoneProblem = attached->phoneProblem;
        }
        merged.push_back(std::move(retained));
    }
    return merged;
}

int ResolveSavedCameraIndex(const std::vector<CameraDescriptor>& cameras,
                            const std::wstring& savedSymbolicLink,
                            int legacyIndex)
{
    if (cameras.empty()) return -1;
    if (!savedSymbolicLink.empty()) {
        for (size_t index = 0; index < cameras.size(); ++index) {
            if (CompareStringOrdinal(cameras[index].symbolicLink.c_str(), -1,
                    savedSymbolicLink.c_str(), -1, TRUE) == CSTR_EQUAL) {
                return static_cast<int>(index);
            }
        }
        // A busy phone may expose only its stable default entry during discovery.
        // Preserve that phone when a previously selected lens cannot be queried.
        for (size_t index = 0; index < cameras.size(); ++index) {
            if (!cameras[index].phoneSerial.empty() && cameras[index].phoneCameraId.empty() &&
                savedSymbolicLink.starts_with(cameras[index].symbolicLink + L"/"))
                return static_cast<int>(index);
        }
        const int phone = ResolveAutomaticPhoneIndex(cameras, savedSymbolicLink,
            savedSymbolicLink.starts_with(L"okuflow-phone:"));
        return phone >= 0 ? phone : 0;
    }
    if (legacyIndex >= 0 && static_cast<size_t>(legacyIndex) < cameras.size()) return legacyIndex;
    for (size_t index = 0; index < cameras.size(); ++index) {
        if (!cameras[index].phoneSerial.empty() && cameras[index].phoneAppReady)
            return static_cast<int>(index);
    }
    return 0;
}
MediaCapture::~MediaCapture() { StopCapture(); }
void MediaCapture::Swap(MediaCapture& other) noexcept
{
    using std::swap;
    swap(session_, other.session_);
    swap(abandonedForExit_, other.abandonedForExit_);
    swap(lastStopCompleted_, other.lastStopCompleted_);
    swap(lastSymbolicLink_, other.lastSymbolicLink_);
    swap(lastError_, other.lastError_);
    swap(formatNotice_, other.formatNotice_);
    swap(negotiatedFormat_, other.negotiatedFormat_);
    swap(nativeFormats_, other.nativeFormats_);
}
bool MediaCapture::Initialize() { return true; }
void MediaCapture::Shutdown() { StopCapture(); }

std::vector<CameraDescriptor> MediaCapture::EnumerateCameras(bool includePhones)
{
    if (abandonedForExit_) {
        lastError_ = "The camera driver did not stop safely. Restart OkuFlow before opening another camera.";
        return {};
    }
    // Enumeration state is separate even if an old session is still stopping.
    auto enumerator = std::make_shared<MediaCaptureSession>();
    auto cameras = enumerator->EnumerateCameras();
    auto phones = includePhones ? PhoneCameraTransport::Enumerate()
                               : std::vector<CameraDescriptor>();
    // Physical ordering stays stable; the selection resolver honors saved cameras.
    cameras.insert(cameras.end(), std::make_move_iterator(phones.begin()), std::make_move_iterator(phones.end()));
    return cameras;
}

std::vector<VideoFormat> MediaCapture::EnumerateFormats(const CameraDescriptor& descriptor)
{
    if (abandonedForExit_) {
        lastError_ = "The camera driver did not stop safely. Restart OkuFlow before querying camera formats.";
        return {};
    }
    auto enumerator = std::make_shared<MediaCaptureSession>();
    if (!descriptor.phoneSerial.empty()) return {}; // Phone negotiates its live mode automatically.
    auto formats = enumerator->EnumerateFormats(descriptor);
    lastError_ = enumerator->LastError();
    return formats;
}

bool MediaCapture::StartCapture(const CameraDescriptor& descriptor,
                                const VideoFormat* requestedFormat,
                                FrameCallback callback, GUID preferredSubtype,
                                CaptureErrorCallback errorCallback,
                                CaptureAccelerationMode accelerationMode,
                                const std::wstring& requestedStableId,
                                const std::atomic<bool>* startupCanceled)
{
    if (session_ && session_->shutdownStarted_ &&
        PollStopCapture() != CaptureStopStatus::Completed) {
        lastError_ = "The previous camera is still stopping or requires an application restart.";
        return false;
    }
    if (!StopCapture() || abandonedForExit_) {
        lastError_ = "The camera driver did not stop safely. Restart OkuFlow before opening another camera.";
        if (session_) session_->lastFailureKind_.store(CameraFailureKind::Other);
        return false;
    }
    session_ = std::make_shared<MediaCaptureSession>();
    lastStopCompleted_ = true;
    // Retry the requested physical device even when its first open fails.
    lastSymbolicLink_ = descriptor.symbolicLink;
    const bool started = session_->StartCapture(descriptor, requestedFormat,
        std::move(callback), preferredSubtype, std::move(errorCallback), accelerationMode,
        requestedStableId, startupCanceled);
    // Session text/format fields are immutable after startup. Runtime failures
    // use callbacks and atomics, never these UI-owned diagnostic snapshots.
    lastError_ = session_->LastError();
    formatNotice_ = session_->FormatNotice();
    negotiatedFormat_ = session_->NegotiatedFormat();
    nativeFormats_ = session_->NativeFormats();
    if (!started && !StopCapture()) {
        lastError_ = "The camera could not start and its driver did not stop safely. Restart OkuFlow before trying again.";
        session_->lastFailureKind_.store(CameraFailureKind::Other);
    }
    return started;
}

void MediaCapture::BeginStopCapture()
{
    if (!session_ || session_->shutdownStarted_) return;
    const auto session = session_;
    session->stopDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    session->shutdownStarted_ = true;
    // Deny facade GPU/phone access before the worker can touch those objects.
    session->releaseNotified_ = true;
    lastStopCompleted_ = false;
    session->Cancel();
    try {
        std::thread([session] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                session->Quiesce();
                session->shutdown_->MarkQuiescent();
                if (session->shutdown_->WaitReleasePermission()) session->ReleaseResources();
            } catch (...) {
                session->shutdown_->Abandon();
                RetainAbandonedSession(session);
            }
            if (SUCCEEDED(apartment)) CoUninitialize();
            session->shutdown_->MarkFinished();
        }).detach();
    } catch (...) {
        session->shutdown_->Abandon();
        // Thread creation failure cannot destroy a joinable capture thread or
        // release driver objects on the facade thread.
        RetainAbandonedSession(session);
    }
}

CaptureStopStatus MediaCapture::PollStopCapture()
{
    if (!session_) return CaptureStopStatus::Completed;
    if (!session_->shutdownStarted_) return CaptureStopStatus::WaitingForQuiescence;
    const auto snapshot = session_->shutdown_->Poll();
    if (lastStopCompleted_) return CaptureStopStatus::Completed;
    if (!snapshot.abandoned && snapshot.finished && snapshot.quiescent &&
        session_->consumerReleaseAcknowledged_ && snapshot.finishedAt <= session_->stopDeadline_) {
        lastStopCompleted_ = true;
        return CaptureStopStatus::Completed;
    }
    if (snapshot.abandoned || std::chrono::steady_clock::now() >= session_->stopDeadline_) {
        session_->shutdown_->Abandon();
        RetainAbandonedSession(session_);
        if (!abandonedForExit_) qWarning() << "Camera asynchronous stop failed; exact session retained safely";
        abandonedForExit_ = true;
        return CaptureStopStatus::Failed;
    }
    if (!snapshot.quiescent) return CaptureStopStatus::WaitingForQuiescence;
    return session_->consumerReleaseAcknowledged_ ? CaptureStopStatus::WaitingForRelease
                                                 : CaptureStopStatus::ReadyToRelease;
}

void MediaCapture::ReleaseStoppedCapture(bool consumerSafe)
{
    if (!session_ || !session_->shutdownStarted_ || session_->consumerReleaseAcknowledged_) return;
    const auto status = PollStopCapture();
    // An early/unsafe acknowledgment must never release producer backing.
    session_->consumerReleaseAcknowledged_ = true;
    if (consumerSafe && status == CaptureStopStatus::ReadyToRelease) session_->shutdown_->AllowRelease();
    else {
        session_->shutdown_->Abandon();
        (void)PollStopCapture();
    }
}

bool MediaCapture::StopCapture(const std::function<void(bool)>& beforeAccelerationRelease)
{
    if (!session_) return true;
    const bool alreadyStarted = session_->shutdownStarted_;
    BeginStopCapture();
    auto status = PollStopCapture();
    if (status == CaptureStopStatus::Completed) return true;
    const auto session = session_;
    // A split stop already in progress owns its consumer cleanup. An implicit
    // destructor/Shutdown must not invent successful cleanup for that caller.
    if (!session->consumerReleaseAcknowledged_ && (!alreadyStarted || beforeAccelerationRelease)) {
        const bool quiesced = session->shutdown_->WaitQuiescent(session->stopDeadline_);
        bool callbackSucceeded = true;
        if (beforeAccelerationRelease) {
            try { beforeAccelerationRelease(quiesced); }
            catch (...) {
                callbackSucceeded = false;
                qWarning() << "Capture consumer interop release failed; preserving producer backing";
            }
        }
        ReleaseStoppedCapture(quiesced && callbackSucceeded);
    }
    if (!session->shutdown_->IsAbandoned()) session->shutdown_->WaitFinished(session->stopDeadline_);
    return PollStopCapture() == CaptureStopStatus::Completed;
}

CameraFailureKind MediaCapture::LastFailureKind() const
{ return session_ ? session_->LastFailureKind() : CameraFailureKind::None; }
CaptureAccelerationMode MediaCapture::AccelerationMode() const
{ return session_ ? session_->AccelerationMode() : CaptureAccelerationMode::Compatibility; }
bool MediaCapture::ConsumeAccelerationValidated()
{ return session_ && !session_->releaseNotified_ && session_->ConsumeAccelerationValidated(); }
bool MediaCapture::ConsumeAccelerationRejected()
{ return session_ && !session_->releaseNotified_ && session_->ConsumeAccelerationRejected(); }
bool MediaCapture::ConsumeDeviceLost()
{ return session_ && !session_->releaseNotified_ && session_->ConsumeDeviceLost(); }
double MediaCapture::CurrentFrameRate() const
{ return session_ && !session_->releaseNotified_ ? session_->CurrentFrameRate() : 0.0; }

GpuFramePreparationResult MediaCapture::PrepareGpuFrameForCuda(
    const MediaFrame& frame, Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTexture,
    std::shared_ptr<void>& outLease)
{
    if (!session_ || session_->releaseNotified_) {
        outTexture.Reset(); outLease.reset();
        return GpuFramePreparationResult::Unsupported;
    }
    return session_->PrepareGpuFrameForCuda(frame, outTexture, outLease);
}
GpuFrameReadbackResult MediaCapture::ReadbackGpuFrame(MediaFrame& frame)
{ return session_ && !session_->releaseNotified_ ? session_->ReadbackGpuFrame(frame) : GpuFrameReadbackResult::Failed; }
bool MediaCapture::HasPendingGpuReadback() const
{ return session_ && !session_->releaseNotified_ && session_->HasPendingGpuReadback(); }
void MediaCapture::SetPhoneZoom(float ratio)
{ if (session_ && !session_->releaseNotified_ && session_->phoneTransport_) session_->phoneTransport_->SetZoom(ratio); }
void MediaCapture::SetPhoneCamera(const std::string& id)
{ if (session_ && !session_->releaseNotified_ && session_->phoneTransport_) session_->phoneTransport_->SetCamera(id); }
void MediaCapture::SetPhoneTorch(bool enabled)
{ if (session_ && !session_->releaseNotified_ && session_->phoneTransport_) session_->phoneTransport_->SetTorch(enabled); }
bool MediaCapture::SetPhoneTorch(bool enabled, std::shared_ptr<std::atomic<bool>> canceled,
                                std::int64_t deadlineSteadyMs)
{
    const auto session = session_;
    return session && !session->releaseNotified_ && session->running_.load() && session->phoneTransport_ &&
        session->phoneTransport_->SetTorch(enabled, std::move(canceled), deadlineSteadyMs);
}
bool MediaCapture::PhoneTorchAvailable() const
{ return session_ && !session_->releaseNotified_ && session_->phoneTransport_ && session_->phoneTransport_->TorchAvailable(); }
bool MediaCapture::PhoneTorchEnabled() const
{ return session_ && !session_->releaseNotified_ && session_->phoneTransport_ && session_->phoneTransport_->TorchEnabled(); }
std::string MediaCapture::PhoneProblemCode() const
{ return session_ && !session_->releaseNotified_ && session_->phoneTransport_ ? session_->phoneTransport_->ProblemCode() : std::string(); }
} // namespace okuflow
#endif
