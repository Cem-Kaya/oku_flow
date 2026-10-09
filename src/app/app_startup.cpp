#ifdef _WIN32

#include "app_internal.hpp"
#include <QSaveFile>

namespace openzoom {
namespace {

// A startup job never owns an app pointer. Its delivery uses the same revocable
// ingress as camera frames. A hung driver retains only this independently owned
// capture, and the tracker prevents process-global MF teardown underneath it.
struct CameraStartupJob final {
    explicit CameraStartupJob(std::shared_ptr<StartupWorkerTracker> tracker)
        : tracker(std::move(tracker)), capture(std::make_unique<MediaCapture>()) {
        this->tracker->active.fetch_add(1);
    }
    ~CameraStartupJob() {
        capture->Shutdown();
        if (capture->WasAbandoned()) tracker->abandoned.store(true);
        capture.reset();
        // The original callbacks also own ingress. Release every job-owned
        // sample reference before permitting process-global MF teardown.
        callback = {};
        errorCallback = {};
        ingress.reset();
        tracker->active.fetch_sub(1);
    }

    std::shared_ptr<StartupWorkerTracker> tracker;
    std::unique_ptr<MediaCapture> capture;
    std::shared_ptr<CameraIngress> ingress;
    FrameCallback callback;
    CaptureErrorCallback errorCallback;
    bool started{};
    bool acceleratedFailed{};
    bool transientAcceleratedFailure{};
    QString acceleratedError;
    QString startupError;
};

} // namespace

void OpenZoomApp::ConfigureStartupProfiling() {
    int durationMs = 15000;
    for (const QString& argument : qtApp_->arguments()) {
        if (argument.startsWith(QStringLiteral("--startup-profile=")))
            startupProfilePath_ = argument.mid(18);
        else if (argument.startsWith(QStringLiteral("--startup-profile-ms="))) {
            bool valid = false;
            const int requested = argument.mid(21).toInt(&valid);
            if (valid) durationMs = std::clamp(requested, 1000, 60000);
        } else if (argument == QStringLiteral("--startup-profile-legacy-camera"))
            startupSynchronousProfile_ = true;
    }
    if (startupProfilePath_.isEmpty()) {
        startupSynchronousProfile_ = false;
        return;
    }
    startupPulseDelaysMs_.reserve(3000);
    startupPulseTimer_.start();
    auto* pulse = new QTimer(this);
    pulse->setTimerType(Qt::PreciseTimer);
    pulse->setInterval(20);
    connect(pulse, &QTimer::timeout, this, [this]() {
        const float elapsed = static_cast<float>(startupPulseTimer_.nsecsElapsed()) * 1e-6f;
        startupPulseTimer_.restart();
        if (startupPulseDelaysMs_.size() < 3000)
            startupPulseDelaysMs_.push_back(std::max(0.0f, elapsed - 20.0f));
    });
    pulse->start();
    QTimer::singleShot(durationMs, this, [this]() {
        WriteStartupProfile();
        qtApp_->quit();
    });
}

void OpenZoomApp::RecordStartupFirstPresent() {
    ++startupPresentedFrames_;
    if (startupFirstFrameLogged_) return;
    startupFirstFrameLogged_ = true;
    startupFirstPresentMs_ = startupTimer_.elapsed();
    qInfo() << "Startup timing: first camera frame presented" << startupFirstPresentMs_ << "ms";
}

void OpenZoomApp::WriteStartupProfile() {
    if (startupProfilePath_.isEmpty()) return;
    auto sorted = startupPulseDelaysMs_;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double fraction) {
        return sorted.empty() ? 0.0 : static_cast<double>(sorted[std::min(sorted.size() - 1,
            static_cast<size_t>(std::ceil(fraction * sorted.size()) - 1))]);
    };
    const auto capture = pipelineOrchestrator_ ? pipelineOrchestrator_->CaptureToPresentPercentiles()
                                                : TimingPercentiles{};
    const auto tick = pipelineOrchestrator_ ? pipelineOrchestrator_->FrameTickPercentiles()
                                             : TimingPercentiles{};
    const VideoFormat& format = mediaCapture_.NegotiatedFormat();
    std::uint64_t received = 0, dropped = 0;
    std::uint64_t shortIntervals = 0, longIntervals = 0, maxIntervalMs = 0;
    double arrivalFps = 0.0;
    if (cameraIngress_) {
        std::scoped_lock lock(cameraIngress_->mutex);
        received = cameraIngress_->profileReceived;
        dropped = cameraIngress_->profileDropped;
        shortIntervals = cameraIngress_->shortArrivalIntervals;
        longIntervals = cameraIngress_->longArrivalIntervals;
        maxIntervalMs = cameraIngress_->maxArrivalIntervalMs;
        const auto elapsed = cameraIngress_->lastArrivalMs - cameraIngress_->firstArrivalMs;
        if (received > 1 && elapsed > 0)
            arrivalFps = (received - 1) * 1000.0 / elapsed;
    }
    QJsonObject stages;
    if (pipelineOrchestrator_) {
        const std::pair<const char*, FrameTimingStage> names[] = {
            {"capture_handoff", FrameTimingStage::CaptureHandoff},
            {"cpu_preparation", FrameTimingStage::CpuPreparation},
            {"cuda_submission", FrameTimingStage::CudaSubmission},
            {"presentation", FrameTimingStage::Presentation},
            {"recording_clone", FrameTimingStage::RecordingClone}};
        for (const auto& [name, stage] : names)
            stages.insert(QString::fromLatin1(name), pipelineOrchestrator_->StagePercentiles(stage).p95Ms);
    }
    const QJsonObject result{
        {QStringLiteral("gpu_lease_retry_ticks"), static_cast<qint64>(startupLeaseRetryTicks_)},
        {QStringLiteral("gpu_query_retry_ticks"), static_cast<qint64>(startupQueryRetryTicks_)},
        {QStringLiteral("gpu_retry_expired_frames"), static_cast<qint64>(startupRetryExpired_)},
        {QStringLiteral("gpu_retry_max_ms"), startupRetryMaxMs_},
        {QStringLiteral("camera_received_frames"), static_cast<qint64>(received)},
        {QStringLiteral("camera_arrival_intervals_under_8ms"), static_cast<qint64>(shortIntervals)},
        {QStringLiteral("camera_arrival_intervals_over_45ms"), static_cast<qint64>(longIntervals)},
        {QStringLiteral("camera_arrival_max_interval_ms"), static_cast<qint64>(maxIntervalMs)},
        {QStringLiteral("camera_ingress_dropped_frames"), static_cast<qint64>(dropped)},
        {QStringLiteral("camera_arrival_fps"), arrivalFps},
        {QStringLiteral("processed_scenes"), static_cast<qint64>(startupProcessedScenes_)},
        {QStringLiteral("presented_frames"), static_cast<qint64>(startupPresentedFrames_)},
        {QStringLiteral("missed_present_attempts"), static_cast<qint64>(presenter_ ? presenter_->MissedPresentCount() : 0)},
        {QStringLiteral("stage_p95_ms"), stages},
        {QStringLiteral("timing_origin"), QStringLiteral("application constructor; excludes executable loader")},
        {QStringLiteral("legacy_camera_start"), startupSynchronousProfile_},
        {QStringLiteral("window_shown_ms"), startupWindowMs_},
        {QStringLiteral("camera_ready_ms"), startupCameraReadyMs_},
        {QStringLiteral("first_present_ms"), startupFirstPresentMs_},
        {QStringLiteral("observed_ms"), startupTimer_.elapsed()},
        {QStringLiteral("ui_pulse_samples"), static_cast<int>(sorted.size())},
        {QStringLiteral("ui_delay_p95_ms"), percentile(0.95)},
        {QStringLiteral("ui_delay_max_ms"), percentile(1.0)},
        {QStringLiteral("capture_to_present_p95_ms"), capture.p95Ms},
        {QStringLiteral("capture_to_present_samples"), static_cast<int>(capture.sampleCount)},
        {QStringLiteral("camera_tick_p95_ms"), tick.p95Ms},
        {QStringLiteral("viewport_fps"), pipelineOrchestrator_ ? pipelineOrchestrator_->MeasuredViewportRate() : 0.0f},
        {QStringLiteral("camera_active"), cameraActive_},
        {QStringLiteral("camera_width"), static_cast<int>(format.width)},
        {QStringLiteral("camera_height"), static_cast<int>(format.height)},
        {QStringLiteral("camera_fps"), mediaCapture_.CurrentFrameRate()},
        {QStringLiteral("accelerated"), currentCaptureAccelerated_},
        {QStringLiteral("direct_gpu"), currentCaptureZeroCopyActive_},
        {QStringLiteral("last_camera_error"), lastCameraError_}};
    QSaveFile file(startupProfilePath_);
    const QByteArray bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) {
        qInfo() << "Startup profile saved:" << startupProfilePath_;
        startupProfilePath_.clear();
    } else {
        qWarning() << "Could not write startup profile:" << file.errorString();
    }
}

void OpenZoomApp::QueueInitialCameraStart(
    const CameraDescriptor& descriptor, const QString& requestedStableId,
    FrameCallback callback, CaptureErrorCallback errorCallback,
    bool requestAcceleration, bool interactive, uint64_t captureSession) {
    auto job = std::make_shared<CameraStartupJob>(startupWorkers_);
    job->ingress = cameraIngress_;
    job->callback = std::move(callback);
    job->errorCallback = std::move(errorCallback);
    // Recreate the activation on its owning worker apartment from the opaque
    // symbolic link; never pass the UI apartment's IMFActivate into this thread.
    const std::wstring name = descriptor.name;
    const std::wstring symbolicLink = descriptor.symbolicLink;
    cameraStartupPending_ = true;
    UpdateCameraAccelerationUi();
    if (uiState_->cameraCombo_) uiState_->cameraCombo_->setEnabled(false);
    if (uiState_->cameraFormatCombo_) uiState_->cameraFormatCombo_->setEnabled(false);
    qInfo() << "Startup timing: camera worker dispatched" << startupTimer_.elapsed() << "ms";
    try {
        std::thread([job, name, symbolicLink, requestedStableId,
                     requestAcceleration, interactive, captureSession]() mutable {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                ThrowIfFailed(apartment, "Initialize camera startup COM apartment");
                CameraDescriptor localDescriptor;
                localDescriptor.name = name;
                localDescriptor.symbolicLink = symbolicLink;
                Microsoft::WRL::ComPtr<IMFAttributes> attributes;
                ThrowIfFailed(MFCreateAttributes(&attributes, 2), "Create camera attributes");
                ThrowIfFailed(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
                              "Set video source type");
                ThrowIfFailed(attributes->SetString(
                                  MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                  symbolicLink.c_str()), "Set camera symbolic link");
                ThrowIfFailed(MFCreateDeviceSourceActivate(attributes.Get(),
                                                           &localDescriptor.activation),
                              "Create camera activation");
                const auto start = [&](CaptureAccelerationMode mode) {
                    return job->capture->StartCapture(localDescriptor, nullptr, job->callback,
                        mode == CaptureAccelerationMode::Accelerated ? MFVideoFormat_ARGB32
                                                                    : MFVideoFormat_NV12,
                        job->errorCallback, mode, requestedStableId.toStdWString());
                };
                job->started = start(requestAcceleration ? CaptureAccelerationMode::Accelerated
                                                         : CaptureAccelerationMode::Compatibility);
                if (!job->started && requestAcceleration) {
                    job->acceleratedFailed = true;
                    job->acceleratedError = QString::fromStdString(job->capture->LastError());
                    const auto failure = job->capture->LastFailureKind();
                    job->transientAcceleratedFailure = failure == CameraFailureKind::DeviceBusy ||
                                                       failure == CameraFailureKind::DeviceMissing;
                    if (!job->transientAcceleratedFailure && !job->capture->WasAbandoned()) {
                        job->started = start(CaptureAccelerationMode::Compatibility);
                    }
                }
            } catch (const std::exception& error) {
                job->startupError = QString::fromUtf8(error.what());
            } catch (...) {
                job->startupError = QStringLiteral("Camera startup failed unexpectedly.");
            }

            {
                const auto& ingress = job->ingress;
                std::scoped_lock lock(ingress->mutex);
                if (ingress->accepting && ingress->receiver) {
                    auto* app = ingress->receiver;
                    QMetaObject::invokeMethod(app, [app, job, interactive, captureSession]() {
                        if (app->cameraSessionId_ != captureSession || app->cameraIngress_ != job->ingress) return;
                        app->mediaCapture_.Swap(*job->capture);
                        app->cameraStartupPending_ = false;
                        app->UpdateCameraAccelerationUi();
                        if (app->uiState_->cameraCombo_) app->uiState_->cameraCombo_->setEnabled(true);
                        if (app->uiState_->cameraFormatCombo_) app->uiState_->cameraFormatCombo_->setEnabled(true);
                        if (job->acceleratedFailed) {
                            auto& persistent = app->settingsController_->MutableSettings();
                            persistent.cameraAccelerationAttempt.clear();
                            auto& acceleration = persistent.cameraAcceleration[app->currentCameraAccelerationKey_];
                            if (!job->transientAcceleratedFailure) {
                                acceleration.reason = job->acceleratedError.isEmpty()
                                    ? QStringLiteral("GPU camera acceleration could not start; using compatibility mode.")
                                    : QStringLiteral("GPU camera acceleration could not start: %1").arg(job->acceleratedError);
                                acceleration.decidedOn = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                                if (acceleration.mode == settings::CameraAccelerationMode::Automatic)
                                    acceleration.automaticFallback = true;
                            }
                            app->SavePersistentSettings();
                        }
                        app->CompleteCameraCaptureStart(job->started, interactive, job->startupError);
                        if (job->started && !app->pendingCameraStartupError_.isEmpty()) {
                            const QString error = std::exchange(app->pendingCameraStartupError_, QString());
                            QTimer::singleShot(0, app, [app, captureSession, error]() {
                                app->HandleCameraRuntimeFailure(captureSession, error);
                            });
                        }
                        if (job->started && app->pipelineOrchestrator_)
                            app->pipelineOrchestrator_->NotifyCameraFrameAvailable();
                    }, Qt::QueuedConnection);
                }
            }
            // Neither closure separately owns ingress or the callbacks: on
            // cancellation their sample references release with this job while
            // COM and the tracker remain active. A queued completion instead
            // retains the entire job, including those teardown guards.
            job.reset();
            if (SUCCEEDED(apartment)) CoUninitialize();
        }).detach();
    } catch (const std::exception& error) {
        cameraStartupPending_ = false;
        if (uiState_->cameraCombo_) uiState_->cameraCombo_->setEnabled(true);
        if (uiState_->cameraFormatCombo_) uiState_->cameraFormatCombo_->setEnabled(true);
        CompleteCameraCaptureStart(false, interactive, QString::fromUtf8(error.what()));
    }
}

} // namespace openzoom
#endif
