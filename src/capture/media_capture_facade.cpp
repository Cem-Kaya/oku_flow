#ifdef _WIN32
#include "media_capture_session.hpp"

#include <QDebug>
#include <chrono>
#include <thread>
#include <utility>

namespace openzoom {
namespace {
// Intentionally no static destructor: a driver or CUDA import may still refer
// to these objects during process teardown. WasAbandoned also prevents MFShutdown.
void RetainAbandonedSession(const std::shared_ptr<MediaCaptureSession>& session)
{
    (void)new std::shared_ptr<MediaCaptureSession>(session);
}
} // namespace

MediaCapture::MediaCapture() : session_(std::make_shared<MediaCaptureSession>()) {}
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

std::vector<CameraDescriptor> MediaCapture::EnumerateCameras()
{
    if (abandonedForExit_) {
        lastError_ = "The camera driver did not stop safely. Restart OpenZoom before opening another camera.";
        return {};
    }
    // Enumeration state is separate even if an old session is still stopping.
    auto enumerator = std::make_shared<MediaCaptureSession>();
    return enumerator->EnumerateCameras();
}

std::vector<VideoFormat> MediaCapture::EnumerateFormats(const CameraDescriptor& descriptor)
{
    if (abandonedForExit_) {
        lastError_ = "The camera driver did not stop safely. Restart OpenZoom before querying camera formats.";
        return {};
    }
    auto enumerator = std::make_shared<MediaCaptureSession>();
    auto formats = enumerator->EnumerateFormats(descriptor);
    lastError_ = enumerator->LastError();
    return formats;
}

bool MediaCapture::StartCapture(const CameraDescriptor& descriptor,
                                const VideoFormat* requestedFormat,
                                FrameCallback callback, GUID preferredSubtype,
                                CaptureErrorCallback errorCallback,
                                CaptureAccelerationMode accelerationMode,
                                const std::wstring& requestedStableId)
{
    if (!StopCapture() || abandonedForExit_) {
        lastError_ = "The camera driver did not stop safely. Restart OpenZoom before opening another camera.";
        if (session_) session_->lastFailureKind_.store(CameraFailureKind::Other);
        return false;
    }
    session_ = std::make_shared<MediaCaptureSession>();
    lastStopCompleted_ = true;
    const bool started = session_->StartCapture(descriptor, requestedFormat,
        std::move(callback), preferredSubtype, std::move(errorCallback), accelerationMode,
        requestedStableId);
    // Session text/format fields are immutable after startup. Runtime failures
    // use callbacks and atomics, never these UI-owned diagnostic snapshots.
    lastError_ = session_->LastError();
    formatNotice_ = session_->FormatNotice();
    negotiatedFormat_ = session_->NegotiatedFormat();
    nativeFormats_ = session_->NativeFormats();
    if (started) lastSymbolicLink_ = session_->LastSymbolicLink();
    else if (!StopCapture()) {
        lastError_ = "The camera could not start and its driver did not stop safely. Restart OpenZoom before trying again.";
        session_->lastFailureKind_.store(CameraFailureKind::Other);
    }
    return started;
}

bool MediaCapture::StopCapture(const std::function<void(bool)>& beforeAccelerationRelease)
{
    if (!session_) return true;
    const auto session = session_;
    if (session->releaseNotified_) return lastStopCompleted_;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    session->Cancel();
    if (!session->shutdownStarted_) {
        session->shutdownStarted_ = true;
        try {
            std::thread([session] {
                const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                try {
                    session->Quiesce();
                    session->shutdown_->MarkQuiescent();
                    if (session->shutdown_->WaitReleasePermission()) {
                        session->ReleaseResources();
                    }
                } catch (...) {
                    session->shutdown_->Abandon();
                    RetainAbandonedSession(session);
                }
                if (SUCCEEDED(apartment)) CoUninitialize();
                session->shutdown_->MarkFinished();
            }).detach();
        } catch (...) {
            session->shutdown_->Abandon();
            // In particular, thread creation failure cannot destroy a still
            // joinable capture thread or its driver objects on the caller.
            RetainAbandonedSession(session);
        }
    }
    const bool quiesced = session->shutdown_->WaitQuiescent(deadline);
    bool callbackSucceeded = true;
    session->releaseNotified_ = true;
    if (beforeAccelerationRelease) {
        try {
            beforeAccelerationRelease(quiesced);
        } catch (...) {
            callbackSucceeded = false;
            qWarning() << "Capture consumer interop release failed; preserving producer backing";
        }
    }
    if (quiesced && callbackSucceeded) session->shutdown_->AllowRelease();
    else session->shutdown_->Abandon();
    lastStopCompleted_ = quiesced && callbackSucceeded &&
        session->shutdown_->WaitFinished(deadline) && !session->shutdown_->IsAbandoned();
    if (!lastStopCompleted_) {
        session->shutdown_->Abandon();
        RetainAbandonedSession(session);
        abandonedForExit_ = true;
        qWarning() << "Camera stop exceeded its deadline; canceled session retained safely";
    }
    return lastStopCompleted_;
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
bool MediaCapture::ReadbackGpuFrame(MediaFrame& frame)
{ return session_ && !session_->releaseNotified_ && session_->ReadbackGpuFrame(frame); }
} // namespace openzoom
#endif
