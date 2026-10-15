#ifdef _WIN32
#include "app_internal.hpp"
#include "okuflow/app/agent/app_control_target.hpp"

namespace okuflow {

void OkuFlowApp::StopCameraCapture(bool atProcessExit) {
    if (atProcessExit) {
        pendingCameraStart_.reset();
        if (retiredCameraIngress_) retiredCameraIngress_->retainedAfterTimeout = retiredCameraIngress_;
        if (retiredCameraFrames_) {
            cameraConsumerCleanup_ = AsyncCleanup::Start([frames = std::move(retiredCameraFrames_)] { return false; });
        }
        StopCameraCaptureAtExit();
        return;
    }
    if (cameraStopPending_) return;
    if (!cameraActive_ && !cameraStartupPending_ && !cameraIngress_) return;
    cameraStopPending_ = true; cameraStopReleaseSent_ = false;
    cameraStopStartupWasPending_ = cameraStartupPending_;
    cameraStopTimer_.start();
    if (controlTarget_) { controlTarget_->CancelObservations(); controlTarget_->CancelPhotoReadback(); }
    if (controlService_) controlService_->SourceChanged();
    CancelPendingAssistantCaptures();
    if (pendingCameraStartupCancel_) pendingCameraStartupCancel_->store(true);
    pendingCameraStartupCancel_.reset(); cameraStartupPending_ = false;
    pendingCameraStartupError_.clear(); ++cameraSessionId_;
    retiredCameraIngress_ = std::move(cameraIngress_);
    if (retiredCameraIngress_) {
        std::scoped_lock lock(retiredCameraIngress_->mutex);
        retiredCameraIngress_->accepting = false; retiredCameraIngress_->receiver = nullptr;
        if (deferredGpuCameraFrame_) retiredCameraIngress_->frames.push_back(std::move(*deferredGpuCameraFrame_));
    } else if (deferredGpuCameraFrame_) {
        retiredCameraFrames_ = std::make_shared<std::deque<MediaFrame>>();
        retiredCameraFrames_->push_back(std::move(*deferredGpuCameraFrame_));
    }
    deferredGpuCameraFrame_.reset(); deferredGpuSequence_.reset(); deferredGpuWaitTimer_.invalidate();
    cameraActive_ = cameraFramePresented_ = false;
    cameraIntent_.Deactivate();
    currentCameraCaptureClock100ns_ = -1; currentCameraFrameSequence_ = 0;
    pendingSceneCaptureClock100ns_.reset();
    cudaSceneReady_ = cpuSceneReady_ = false;
    mediaCapture_.BeginStopCapture();
    UpdateProcessingStatusLabel();
    QTimer::singleShot(0,this,&OkuFlowApp::DriveCameraRetirement);
}

void OkuFlowApp::DriveCameraRetirement() {
    if (!cameraStopPending_) return;
    auto status = mediaCapture_.PollStopCapture();
    if (status == CaptureStopStatus::ReadyToRelease && !cameraStopReleaseSent_) {
        const bool producerSafe = !presenter_ || !presenter_->IsFaulted();
        if (!producerSafe || (cudaSurface_ && cudaSurface_->IsFaulted())) {
            mediaCapture_.ReleaseStoppedCapture(false); status = CaptureStopStatus::Failed;
        } else if (!cudaSurface_ || cudaSurface_->PollIdle()) {
            if (!cameraConsumerCleanup_) {
                if (!retiredCameraFrames_) retiredCameraFrames_ = std::make_shared<std::deque<MediaFrame>>();
                if (retiredCameraIngress_) {
                    std::scoped_lock lock(retiredCameraIngress_->mutex);
                    retiredCameraFrames_->insert(retiredCameraFrames_->end(),
                        std::make_move_iterator(retiredCameraIngress_->frames.begin()),
                        std::make_move_iterator(retiredCameraIngress_->frames.end()));
                    retiredCameraIngress_->frames.clear();
                }
                auto frames = retiredCameraFrames_;
                cameraConsumerCleanup_ = cudaSurface_ ? cudaSurface_->BeginCaptureInteropCleanup(frames)
                    : AsyncCleanup::Start([frames] { return true; });
                if (cameraConsumerCleanup_) retiredCameraFrames_.reset();
            }
            if (cameraConsumerCleanup_) {
                const auto cleanup = cameraConsumerCleanup_->Poll();
                if (cleanup != AsyncCleanup::Status::Running) {
                    cameraStopReleaseSent_ = true;
                    mediaCapture_.ReleaseStoppedCapture(cleanup == AsyncCleanup::Status::Completed);
                    if (cleanup == AsyncCleanup::Status::Failed) status = CaptureStopStatus::Failed;
                }
            }
        }
    }
    if (status == CaptureStopStatus::Completed && startupWorkers_ && startupWorkers_->active.load() != 0) {
        if (cameraStopTimer_.elapsed() >= 1500) status = CaptureStopStatus::Failed;
        else status = CaptureStopStatus::WaitingForQuiescence;
    }
    if (status == CaptureStopStatus::Failed) {
        cameraStopPending_ = false; cameraStopFailed_ = true; pendingCameraStart_.reset();
        if (cudaSurface_) cudaSurface_->AbandonCaptureInterop();
        if (retiredCameraIngress_) retiredCameraIngress_->retainedAfterTimeout = retiredCameraIngress_;
        if (retiredCameraFrames_) cameraConsumerCleanup_ = AsyncCleanup::Start([frames = std::move(retiredCameraFrames_)] { return false; });
        ShowStatusMessage(QStringLiteral("Camera did not stop in time. Restart OkuFlow before changing cameras."),15000);
        UpdateCameraAccelerationUi(); return;
    }
    if (status != CaptureStopStatus::Completed) {
        QTimer::singleShot(20,this,&OkuFlowApp::DriveCameraRetirement); return;
    }
    cameraStopPending_ = false; retiredCameraIngress_.reset(); cameraConsumerCleanup_.reset();
    currentCaptureAccelerated_ = currentCaptureZeroCopyActive_ = false;
    captureZeroCopyAvailable_ = true; captureHandoffPolicy_.Reset();
    captureZeroCopyStatusPersisted_ = false; captureZeroCopyFailureReason_.clear();
    presentationBuffer_.clear(); presentationWidth_ = presentationHeight_ = 0;
    cpuPipeline_.ResetTemporalHistory(); processedFrameWidth_ = processedFrameHeight_ = 0;
    cpuSceneBuffer_.clear(); cpuSceneWidth_ = cpuSceneHeight_ = 0;
    if (!cameraStopStartupWasPending_ && settingsController_ &&
        settingsController_->MutableSettings().cameraAccelerationAttempt == currentCameraAccelerationKey_) {
        settingsController_->MutableSettings().cameraAccelerationAttempt.clear();
        if (uiState_ && assistiveManager_) SavePersistentSettings();
    }
    UpdateCameraAccelerationUi();
    ResumePendingCameraStart();
}

void OkuFlowApp::ResumePendingCameraStart() {
    if (cameraStopPending_ || !pendingCameraStart_) return;
    if (pendingCameraStart_->generation != cameraIntent_.Requested().generation || cameraStopFailed_) {
        pendingCameraStart_.reset(); return;
    }
    if (recordingManager_ && recordingManager_->CurrentSessionInfo()) {
        QTimer::singleShot(25,this,&OkuFlowApp::ResumePendingCameraStart); return;
    }
    {
        const auto request = *pendingCameraStart_; pendingCameraStart_.reset();
        const auto camera = std::find_if(cameras_.begin(),cameras_.end(),[&request](const CameraDescriptor& candidate) {
            return candidate.symbolicLink == request.symbolicLink;
        });
        if (camera != cameras_.end()) StartCameraCapture(static_cast<size_t>(camera-cameras_.begin()),
            request.interactive,request.forceCompatibility,request.backgroundStartup,request.origin,request.formatOverride,request.operationId);
    }
}

void OkuFlowApp::CancelCameraTransition(const QString& owner, bool failedFallback) {
    if (cameraIntent_.TemporaryOwner() != owner || owner.isEmpty()) return;
    QStringList enumeration;
    for (const auto& camera : cameras_) enumeration.append(QString::fromStdWString(camera.symbolicLink));
    const auto restore = cameraIntent_.RestoreChoice(enumeration);
    cameraIntent_.ReleaseTemporary(owner); pendingCameraStart_.reset();
    if (pendingCameraStartupCancel_) pendingCameraStartupCancel_->store(true);
    if (restore && !cameraStopFailed_) {
        const auto index = enumeration.indexOf(restore->identity.symbolicLink);
        if (index >= 0) {
            const auto origin = failedFallback ? agent::CameraSourceOrigin::Fallback : agent::CameraSourceOrigin::Reconnect;
            const auto generation = cameraIntent_.Request(restore->identity,origin);
            pendingCameraStart_ = PendingCameraStart{static_cast<size_t>(index),false,false,true,
                restore->identity.symbolicLink.toStdWString(),origin,
                restore->identity.formatStableId,{},generation};
        }
    }
    if (recordingManager_ && recordingManager_->CurrentSessionInfo()) {
        // Retire authority immediately, but preserve the producer until the
        // exact recording has finished draining its owned frames.
        auto* timer = new QTimer(this); timer->setInterval(25);
        const auto generation = cameraIntent_.Requested().generation;
        connect(timer,&QTimer::timeout,this,[this,timer,generation] {
            if (cameraIntent_.Requested().generation != generation) { timer->deleteLater(); return; }
            if (recordingManager_ && recordingManager_->CurrentSessionInfo()) return;
            timer->deleteLater(); StopCameraCapture(); ResumePendingCameraStart();
        });
        timer->start(); return;
    }
    StopCameraCapture(); ResumePendingCameraStart();
}
} // namespace okuflow
#endif
