#ifdef _WIN32

#include "app_internal.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include <QLocale>
#include <initializer_list>
#include "okuflow/capture/phone_camera.hpp"

namespace okuflow {

void OkuFlowApp::OnCameraSelectionChanged(int index) {
    if (uiState_ && uiState_->cameraCombo_ &&
        uiState_->cameraCombo_->itemData(index).toString() == QStringLiteral("video")) {
        emit mainWindow_->openVideoRequested();
        return;
    }
    if (index < 0 || static_cast<size_t>(index) >= cameras_.size()) {
        return;
    }

    // A manual camera pick always wins over an in-flight automatic reconnect.
    pipelineOrchestrator_->CancelCameraReconnect();
    automaticPhoneSelectionAllowed_ = !cameras_[static_cast<size_t>(index)].phoneSerial.empty();
    preferredPhoneCamera_ = automaticPhoneSelectionAllowed_
        ? QString::fromStdWString(cameras_[static_cast<size_t>(index)].symbolicLink) : QString();
    settingsController_->MutableSettings().cameraIndex = index;
    settingsController_->MutableSettings().cameraSelectionExplicit = true;
    settingsController_->MutableSettings().cameraSymbolicLink =
        QString::fromStdWString(cameras_[static_cast<size_t>(index)].symbolicLink);
    cameraIntent_.SelectManual(settingsController_->MutableSettings().cameraSymbolicLink,index);
    if (controlService_) controlService_->SourceChanged();
    if (recordingManager_) {
        if (const auto session = recordingManager_->CurrentSessionInfo()) StopRecording(session->id);
    }
    // A manual camera pick cancels pending file opening as well as adopted
    // playback. Do this after StopRecording's same-session hold transition.
    ReturnVideoToCamera();
    if (mainWindow_) mainWindow_->setVideoSourceSelected(false, index);
    if (automaticPhoneSelectionAllowed_) {
        phoneRetries_[cameras_[static_cast<size_t>(index)].phoneSerial].Succeeded();
        PhoneCameraTransport::AllowAppLaunch(cameras_[static_cast<size_t>(index)].phoneSerial);
    }
    StartCameraCapture(static_cast<size_t>(index),true,false,false,agent::CameraSourceOrigin::Manual);
    SavePersistentSettings();
}

int OkuFlowApp::CameraPickerIndex() const {
    const auto& requested = cameraIntent_.Requested().identity.symbolicLink;
    for (size_t i = 0; i < cameras_.size(); ++i)
        if (QString::fromStdWString(cameras_[i].symbolicLink) == requested)
            return static_cast<int>(i);
    return selectedCameraIndex_;
}

void OkuFlowApp::UpdateCameraAccelerationUi()
{
    if (!uiState_) {
        return;
    }
    auto& persistent = settingsController_->MutableSettings();

    settings::CameraAccelerationSetting acceleration;
    if (!currentCameraAccelerationKey_.isEmpty()) {
        acceleration =
            persistent.cameraAcceleration.value(
                currentCameraAccelerationKey_);
    }
    if (uiState_->cameraAccelerationCombo_) {
        uiState_->cameraAccelerationCombo_->setEnabled(!cameraStartupPending_);
        auto blocker =
            uiState_->BlockSignals(
                uiState_->cameraAccelerationCombo_);
        uiState_->cameraAccelerationCombo_->setCurrentIndex(
            static_cast<int>(acceleration.mode));
    }
    if (uiState_->testCameraAccelerationButton_) {
        uiState_->testCameraAccelerationButton_->setEnabled(
            !cameraStartupPending_ && selectedCameraIndex_ >= 0 &&
            static_cast<size_t>(selectedCameraIndex_) < cameras_.size());
    }

    if (!uiState_->cameraAccelerationStatusLabel_) {
        return;
    }
    QString status;
    if (cameraActive_) {
        if (currentCaptureZeroCopyActive_) {
            status = QStringLiteral(
                "Direct GPU camera transfer is active. Camera frames stay on "
                "the GPU through Media Foundation, D3D11, and CUDA.");
        } else if (currentCaptureAccelerated_) {
            status = QStringLiteral(
                "GPU camera capture is active with the safe copy fallback.");
        } else {
            status = QStringLiteral(
                "Compatibility capture is active. This path supports more "
                "camera drivers but can add latency.");
        }
    } else {
        status =
            QStringLiteral("Camera acceleration has not started yet.");
    }
    if (!acceleration.reason.isEmpty()) {
        status += QStringLiteral("\n%1").arg(acceleration.reason);
    }
    SetLiveText(uiState_->cameraAccelerationStatusLabel_, status,
                LivePoliteness::kSilent,
                QStringLiteral("Camera acceleration status"));
    uiState_->cameraAccelerationStatusLabel_->setToolTip(status);
}

void OkuFlowApp::UpdatePhoneLightUi()
{
    if (!uiState_ || !uiState_->phoneLightButton_) return;
    const bool phoneSelected = !MediaViewSelected() &&
        (!mainWindow_ || !mainWindow_->videoSourceSelected()) && selectedCameraIndex_ >= 0 &&
        static_cast<size_t>(selectedCameraIndex_) < cameras_.size() &&
        !cameras_[static_cast<size_t>(selectedCameraIndex_)].phoneSerial.empty();
    auto* button = uiState_->phoneLightButton_;
    button->setVisible(phoneSelected);
    button->setEnabled(phoneSelected && cameraActive_ && !cameraStartupPending_ &&
                       mediaCapture_.PhoneTorchAvailable());
    SetLiveText(button, mediaCapture_.PhoneTorchEnabled()
        ? QStringLiteral("Turn off phone light") : QStringLiteral("Turn on phone light"),
        LivePoliteness::kSilent, QStringLiteral("Phone light"));
    const auto [minimum, maximum] = mediaCapture_.PhoneZoomRange();
    const auto key = phoneSelected ? QString::fromStdWString(cameras_[size_t(selectedCameraIndex_)].symbolicLink) : QString();
    const float requested = std::clamp(settingsController_->Settings().phoneCameraZoom.value(key, 1.0f), minimum, maximum);
    mainWindow_->setPhoneZoomControls(phoneSelected,
        phoneSelected && cameraActive_ && !cameraStartupPending_ && maximum > minimum, minimum, maximum, requested);
}

void OkuFlowApp::OnTestCameraAcceleration()
{
    if (cameraStartupPending_) return;
    if (selectedCameraIndex_ < 0 ||
        static_cast<size_t>(selectedCameraIndex_) >= cameras_.size()) {
        ShowStatusMessage(
            QStringLiteral("Select a camera before running the test."));
        return;
    }
    if (recordingManager_ && recordingManager_->IsActive()) {
        ShowStatusMessage(
            QStringLiteral("Stop recording before testing camera acceleration."));
        return;
    }

    const QString appDirectory = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QDir(appDirectory).filePath(QStringLiteral("mf_dxva_minimal.exe")),
        QDir(appDirectory).filePath(
            QStringLiteral("../sandbox_mf_dxva_minimal/mf_dxva_minimal.exe")),
        QDir(appDirectory).filePath(
            QStringLiteral("../sandbox_mf_dxva_minimal/Release/mf_dxva_minimal.exe")),
        QDir(appDirectory).filePath(
            QStringLiteral("../../sandbox_mf_dxva_minimal/Release/mf_dxva_minimal.exe")),
    };
    QString probePath;
    for (const QString& candidate : candidates) {
        const QFileInfo file(candidate);
        if (file.exists() && file.isFile()) {
            probePath = file.absoluteFilePath();
            break;
        }
    }
    if (probePath.isEmpty()) {
        ShowStatusMessage(
            QStringLiteral(
                "The isolated camera test is missing from this OkuFlow "
                "bundle. Rebuild or reinstall the complete bundle."),
            12000);
        return;
    }

    const int cameraIndex = selectedCameraIndex_;
    const QString cameraKey = QString::fromStdWString(
        cameras_[static_cast<size_t>(cameraIndex)].symbolicLink);
    StopCameraCapture();

    if (uiState_->testCameraAccelerationButton_) {
        uiState_->testCameraAccelerationButton_->setEnabled(false);
        SetLiveText(uiState_->testCameraAccelerationButton_,
                    QStringLiteral("Testing camera..."),
                    LivePoliteness::kSilent,
                    QStringLiteral("Test camera acceleration"));
    }
    ShowStatusMessage(
        QStringLiteral(
            "Testing GPU and compatibility capture in an isolated process..."),
        12000);

    auto* process = new QProcess(this);
    process->setProgram(probePath);
    process->setArguments(
        {QStringLiteral("--camera"),
         QString::number(cameraIndex),
         QStringLiteral("--timeout-ms"),
         QStringLiteral("12000")});
    process->setProcessChannelMode(QProcess::SeparateChannels);

    const auto restoreUiAndCamera = [this, cameraIndex]() {
        if (uiState_->testCameraAccelerationButton_) {
            SetLiveText(uiState_->testCameraAccelerationButton_,
                        QStringLiteral("Test this camera"),
                        LivePoliteness::kSilent,
                        QStringLiteral("Test camera acceleration"));
            uiState_->testCameraAccelerationButton_->setEnabled(true);
        }
        if (cameraIndex >= 0 &&
            static_cast<size_t>(cameraIndex) < cameras_.size()) {
            StartCameraCapture(static_cast<size_t>(cameraIndex), false);
        }
    };

    connect(
        process,
        &QProcess::finished,
        this,
        [this, process, cameraKey, restoreUiAndCamera](
            int exitCode, QProcess::ExitStatus exitStatus) {
            const QByteArray output =
                process->readAllStandardOutput().trimmed();
            const QString processError =
                QString::fromUtf8(process->readAllStandardError()).trimmed();
            process->deleteLater();

            QJsonParseError parseError;
            const QJsonDocument document =
                QJsonDocument::fromJson(output, &parseError);
            const QJsonArray cameras =
                document.object().value(QStringLiteral("cameras")).toArray();
            if (exitStatus != QProcess::NormalExit || exitCode != 0 ||
                parseError.error != QJsonParseError::NoError ||
                cameras.isEmpty()) {
                restoreUiAndCamera();
                const QString detail =
                    !processError.isEmpty()
                        ? processError
                        : QStringLiteral(
                              "The isolated camera test did not return a "
                              "usable verdict.");
                ShowStatusMessage(detail, 12000);
                return;
            }

            const QJsonObject camera = cameras.first().toObject();
            const QJsonObject accelerated =
                camera.value(QStringLiteral("accelerated")).toObject();
            const QJsonObject compatibility =
                camera.value(QStringLiteral("compatibility")).toObject();
            const QString acceleratedStatus =
                accelerated.value(QStringLiteral("status")).toString();
            const QString compatibilityStatus =
                compatibility.value(QStringLiteral("status")).toString();
            const double acceleratedMs =
                accelerated.value(QStringLiteral("averageReadMs")).toDouble();
            const double compatibilityMs =
                compatibility.value(QStringLiteral("averageReadMs")).toDouble();

            auto& setting =
                settingsController_->MutableSettings().cameraAcceleration[
                    cameraKey];
            setting.decidedOn =
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            QString result;
            if (acceleratedStatus == QStringLiteral("ok")) {
                setting.automaticFallback = false;
                setting.reason =
                    QStringLiteral(
                        "Isolated test passed: GPU %1 ms, compatibility %2 ms "
                        "average frame read.")
                        .arg(acceleratedMs, 0, 'f', 1)
                        .arg(compatibilityMs, 0, 'f', 1);
                result =
                    QStringLiteral(
                        "GPU camera acceleration passed. Average frame read: "
                        "%1 ms versus %2 ms in compatibility mode.")
                        .arg(acceleratedMs, 0, 'f', 1)
                        .arg(compatibilityMs, 0, 'f', 1);
            } else if (compatibilityStatus == QStringLiteral("ok")) {
                if (setting.mode ==
                    settings::CameraAccelerationMode::Automatic) {
                    setting.automaticFallback = true;
                }
                setting.reason =
                    QStringLiteral(
                        "Isolated GPU test returned %1; compatibility capture "
                        "passed.")
                        .arg(acceleratedStatus);
                result =
                    QStringLiteral(
                        "GPU capture did not pass, so this camera will use "
                        "compatibility mode automatically.");
            } else {
                setting.reason =
                    QStringLiteral(
                        "The isolated test could not verify a usable image in "
                        "either mode. Check that the camera is connected and "
                        "showing a detailed scene.");
                result = setting.reason;
            }
            settingsController_->MutableSettings()
                .cameraAccelerationAttempt.clear();
            SavePersistentSettings();
            restoreUiAndCamera();
            UpdateCameraAccelerationUi();
            ShowStatusMessage(result, 15000);
        });
    connect(
        process,
        &QProcess::errorOccurred,
        this,
        [this, process, restoreUiAndCamera](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) {
                return;
            }
            const QString detail =
                QStringLiteral("The isolated camera test could not start: %1")
                    .arg(process->errorString());
            process->deleteLater();
            restoreUiAndCamera();
            ShowStatusMessage(detail, 12000);
        });
    process->start();
}

void OkuFlowApp::OnCameraAccelerationModeChanged(int index)
{
    if (cameraStartupPending_) return;
    if (index < 0 || index > 2 ||
        selectedCameraIndex_ < 0 ||
        static_cast<size_t>(selectedCameraIndex_) >= cameras_.size()) {
        return;
    }
    const QString key = QString::fromStdWString(
        cameras_[static_cast<size_t>(selectedCameraIndex_)].symbolicLink);
    auto& acceleration =
        settingsController_->MutableSettings().cameraAcceleration[key];
    acceleration.mode =
        static_cast<settings::CameraAccelerationMode>(index);
    acceleration.automaticFallback = false;
    acceleration.reason.clear();
    acceleration.decidedOn =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    settingsController_->MutableSettings().cameraAccelerationAttempt.clear();
    if(controlTarget_)controlTarget_->NoteNativeCameraAcceleration(key);
    SavePersistentSettings();
    StartCameraCapture(
        static_cast<size_t>(selectedCameraIndex_));
}

void OkuFlowApp::RememberCameraWorkingSet()
{
    if (MediaViewSelected() || currentCameraAccelerationKey_.isEmpty() || !cameraIntent_.TemporaryOwner().isEmpty()) return;
    auto& saved = settingsController_->MutableSettings();
    if (saved.cameraWorkingSets.contains(currentCameraAccelerationKey_) || saved.cameraWorkingSets.size() < 64)
        saved.cameraWorkingSets[currentCameraAccelerationKey_] = {saved.cameraFormatStableId, CameraOriginalRotationQuarterTurns()};
}

void OkuFlowApp::OnCameraFormatChanged(int index) {
    if (cameraStartupPending_ || !uiState_->cameraFormatCombo_ || index < 0) {
        return;
    }
    settingsController_->MutableSettings().cameraFormatStableId =
        uiState_->cameraFormatCombo_->itemData(index).toString();
    RememberCameraWorkingSet();
    cameraIntent_.SetSavedFormat(settingsController_->MutableSettings().cameraFormatStableId);
    SavePersistentSettings();
    if (selectedCameraIndex_ >= 0 &&
        static_cast<size_t>(selectedCameraIndex_) < cameras_.size()) {
        StartCameraCapture(static_cast<size_t>(selectedCameraIndex_),true,false,false,agent::CameraSourceOrigin::Manual);
    }
}

void OkuFlowApp::OnMicrophoneSelectionChanged(int index)
{
    if (!uiState_->microphoneCombo_ || index < 0) {
        return;
    }
    const QString endpoint =
        uiState_->microphoneCombo_->itemData(index).toString();
    settingsController_->MutableSettings().microphoneEndpointId =
        endpoint;
    if (recordingManager_ && recordingManager_->IsActive()) {
        if (const auto session = recordingManager_->CurrentSessionInfo()) OnVideoRecordingStopping(*session);
        recordingManager_->Stop(
            QStringLiteral(
                "Recording stopped because the microphone selection changed."));
        StopMicrophoneCapture();
    }
    SavePersistentSettings();
}
void OkuFlowApp::SetRecordingCanvasMode(RecordingCanvasMode mode) {
    Q_UNUSED(mode);
    // Compatibility slot only. Original dimensions are source-owned.
    settingsController_->MutableSettings().recordingCanvasMode = RecordingCanvasMode::Source;
}
bool OkuFlowApp::SetPhoneTorch(bool enabled,std::shared_ptr<std::atomic<bool>> canceled,qint64 deadlineSteadyMs) {
    if(!cameraActive_ || !mediaCapture_.PhoneTorchAvailable())return false;
    if(canceled) {
        if(!mediaCapture_.SetPhoneTorch(enabled,std::move(canceled),deadlineSteadyMs))return false;
    } else mediaCapture_.SetPhoneTorch(enabled);
    if(controlTarget_)controlTarget_->NoteTorchRequest(enabled);
    pipelineOrchestrator_->MarkViewportDirty();return true;
}

OkuFlowApp::MicrophoneStartResult OkuFlowApp::StartSelectedMicrophone()
{
    StopMicrophoneCapture();
    if (!uiState_ || !uiState_->microphoneCombo_) return MicrophoneStartResult::Disabled;
    const QString endpoint=uiState_->microphoneCombo_->currentData().toString();
    if(endpoint.isEmpty() || endpoint==QStringLiteral("__none__"))return MicrophoneStartResult::Disabled;
    const auto microphone=std::find_if(microphones_.cbegin(),microphones_.cend(),[&](const AudioDeviceDescriptor& item) {
        return QString::fromStdWString(item.endpointId)==endpoint;
    });
    if(microphone==microphones_.cend() || (microphoneWorker_ &&
       (!microphoneWorker_->done.load() || microphoneWorker_->abandoned.load()))) {
        ShowStatusMessage(QStringLiteral("The selected microphone is unavailable. Recording will continue without sound."),9000);
        return MicrophoneStartResult::Failed;
    }
    const auto recording=recordingManager_?recordingManager_->CurrentSessionInfo():std::nullopt;
    if(!recording || !recordingManager_->IsActive())return MicrophoneStartResult::Failed;
    const QString recordingId=recording->id;
    const auto target=microphoneCallbackTarget_;
    std::uint64_t generation=0;
    {std::lock_guard lock(target->mutex);generation=++target->generation;target->accepting=target->app==this;}
    auto worker=std::make_shared<MicrophoneWorkerState>();microphoneWorker_=worker;
    pendingMicrophoneRecording_=recordingId;
    recordingManager_->SetSessionAudioPending(recordingId);
    AudioFrameCallback frameCallback=[target,generation,recordingId](AudioFrame&& frame) {
        std::lock_guard lock(target->mutex);auto* app=target->app;
        if(!target->accepting || !app || target->generation!=generation || !app->recordingManager_)return;
        const auto session=app->recordingManager_->CurrentSessionInfo();
        if(session && session->id==recordingId)app->recordingManager_->AddAudioFrame(std::move(frame));
    };
    AudioErrorCallback errorCallback=[target,generation,recordingId](const std::string& error) {
        std::lock_guard lock(target->mutex);auto* app=target->app;
        if(!target->accepting || !app || target->generation!=generation)return;
        QMetaObject::invokeMethod(app,[target,generation,recordingId,detail=QString::fromStdString(error)] {
            OkuFlowApp* current=nullptr;
            {std::lock_guard guard(target->mutex);if(target->accepting && target->generation==generation)current=target->app;}
            if(!current || !current->recordingManager_)return;
            if(!current->StopRecording(recordingId))return;
            current->ShowStatusMessage(detail,12000);
        },Qt::QueuedConnection);
    };
    const auto startForTest=microphoneStartForTest_;const auto stopForTest=microphoneStopForTest_;
    try {
        std::thread([worker,target,generation,recordingId,endpoint=endpoint.toStdWString(),
                     frameCallback=std::move(frameCallback),errorCallback=std::move(errorCallback),startForTest,stopForTest]() mutable {
            const HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            // Native state is constructed, used and destroyed on this worker.
            // A blocked Open/Flush/Shutdown retains this owner, never the app.
            std::unique_ptr<AudioCapture> capture;
            try {
                capture=std::make_unique<AudioCapture>();
                ThrowIfFailed(apartment,"Initialize microphone COM apartment");
                AudioDeviceDescriptor descriptor;descriptor.endpointId=endpoint;
                if(!startForTest && !worker->canceled.load()) {
                    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
                    ThrowIfFailed(MFCreateAttributes(&attributes,2),"Create microphone attributes");
                    ThrowIfFailed(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_GUID),"Set microphone source type");
                    ThrowIfFailed(attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_AUDCAP_ENDPOINT_ID,endpoint.c_str()),"Set microphone endpoint");
                    ThrowIfFailed(MFCreateDeviceSourceActivate(attributes.Get(),&descriptor.activation),"Create microphone activation");
                }
                if(!worker->canceled.load())worker->started=startForTest?startForTest(worker->canceled):
                    capture->Start(descriptor,std::move(frameCallback),std::move(errorCallback));
                if(!worker->started.load()) {std::lock_guard lock(worker->mutex);worker->error=capture->LastError();}
            } catch(const std::exception& error) {std::lock_guard lock(worker->mutex);worker->error=error.what();}
              catch(...) {std::lock_guard lock(worker->mutex);worker->error="Unknown error opening microphone";}
            worker->startupFinished=true;
            {
                std::lock_guard lock(target->mutex);auto* app=target->app;
                if(target->accepting && app && target->generation==generation)
                    QMetaObject::invokeMethod(app,[app,worker,recordingId,generation] {
                        app->CompleteMicrophoneStart(worker,recordingId,generation);
                    },Qt::QueuedConnection);
            }
            if(worker->started.load()) {
                std::unique_lock lock(worker->mutex);worker->changed.wait(lock,[&] {return worker->canceled.load();});
            }
            // The retirement boundary includes every native cleanup and release,
            // not only the reader-loop/flusher's earlier three-second wait.
            try {if(stopForTest)stopForTest();if(capture)capture->Stop();}
            catch(...) {worker->abandoned=true;(void)capture.release();}
            if(capture)worker->abandoned=worker->abandoned.load() || capture->WasAbandoned();
            capture.reset();frameCallback={};errorCallback={};
            if(SUCCEEDED(apartment))CoUninitialize();
            worker->done=true;worker->changed.notify_all();
        }).detach();
    } catch(const std::exception& error) {
        {std::lock_guard lock(worker->mutex);worker->error=error.what();}
        worker->startupFinished=worker->done=true;
        CompleteMicrophoneStart(worker,recordingId,generation);
        return MicrophoneStartResult::Failed;
    }
    QTimer::singleShot(microphoneDeadlineMs_,this,[this,worker,recordingId,generation] {
        if(microphoneWorker_!=worker || worker->startupFinished.load() || worker->canceled.load())return;
        {std::lock_guard lock(worker->mutex);worker->error="Microphone startup timed out";}
        StopMicrophoneCapture();
        ShowStatusMessage(QStringLiteral("The selected microphone could not start: %1. Recording will continue without sound.")
            .arg(QStringLiteral("Microphone startup timed out")),12000);
    });
    return MicrophoneStartResult::Pending;
}

void OkuFlowApp::CompleteMicrophoneStart(const std::shared_ptr<MicrophoneWorkerState>& worker,
                                        const QString& recordingId,std::uint64_t generation)
{
    if(microphoneWorker_!=worker || worker->canceled.load())return;
    {std::lock_guard lock(microphoneCallbackTarget_->mutex);
     if(!microphoneCallbackTarget_->accepting || microphoneCallbackTarget_->generation!=generation)return;}
    const auto session=recordingManager_?recordingManager_->CurrentSessionInfo():std::nullopt;
    if(!session || session->id!=recordingId || !recordingManager_->IsActive()) {StopMicrophoneCapture();return;}
    pendingMicrophoneRecording_.clear();
    recordingManager_->SetSessionAudioResult(recordingId,worker->started.load());
    if(!worker->started.load()) {
        std::string error;{std::lock_guard lock(worker->mutex);error=worker->error;}
        StopMicrophoneCapture();
        ShowStatusMessage(QStringLiteral("The selected microphone could not start: %1. Recording will continue without sound.")
            .arg(QString::fromStdString(error)),12000);
    }
}

void OkuFlowApp::StopMicrophoneCapture(bool retainQueuedAudio)
{
    if(microphoneCallbackTarget_) {
        std::lock_guard lock(microphoneCallbackTarget_->mutex);
        microphoneCallbackTarget_->accepting=false;++microphoneCallbackTarget_->generation;
    }
    const auto worker=microphoneWorker_;
    if(worker && !worker->canceled.exchange(true)) {
        worker->changed.notify_all();
        QTimer::singleShot(microphoneDeadlineMs_,this,[this,worker] {
            if(microphoneWorker_==worker && !worker->done.load())
                ShowStatusMessage(QStringLiteral("Microphone shutdown timed out"),12000);
        });
    }
    if(recordingManager_) {
        if(!pendingMicrophoneRecording_.isEmpty())recordingManager_->SetSessionAudioResult(std::exchange(pendingMicrophoneRecording_,{}),false);
        if(!retainQueuedAudio)recordingManager_->SetAudioCaptureEnabled(false);
    }
}

bool OkuFlowApp::MicrophoneTeardownUnsafe() const
{
    return microphoneWorker_ && (!microphoneWorker_->done.load() || microphoneWorker_->abandoned.load());
}

void OkuFlowApp::OnBlackWhiteToggled(bool checked) {
    blackWhiteEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OkuFlowApp::OnBlackWhiteThresholdChanged(int value) {
    blackWhiteThreshold_ = std::clamp(static_cast<float>(value) / 255.0f, 0.0f, 1.0f);
    SyncCurrentConfigToPersistence();
}

void OkuFlowApp::OnZoomToggled(bool checked) {
    if (zoomEnabled_ != checked) pipelineOrchestrator_->NotifyViewportMotion();
    zoomEnabled_ = checked;
    pipelineOrchestrator_->MarkViewportDirty();
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OkuFlowApp::OnZoomAmountChanged(int value) {
    const bool whole = uiState_->zoomSlider_ && value == uiState_->zoomSlider_->minimum();
    // A short slider magnet at Fill; wheel/key stepping uses a crossing stop.
    if (!whole && std::abs(value - kZoomSliderScale) <= 2) value = kZoomSliderScale;
    SetViewportFitMode(settings::ViewportFitModeSetting::Fill, false);
    SetZoom(whole ? kWholeSourceZoom : static_cast<float>(value)/kZoomSliderScale,{},false);
}

float OkuFlowApp::EffectiveZoomAmount() const {
    return pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fit
        ? 1.0f : EffectiveFillZoom(zoomEnabled_ ? zoomAmount_ : 1.0f, ViewportFitZoom());
}

void OkuFlowApp::RefreshZoomRange() {
    if (mainWindow_) mainWindow_->setZoomRange(ViewportFitZoom(), zoomAmount_);
}

void OkuFlowApp::SetZoom(float amount, std::optional<QPointF> anchor, bool enable, bool recenterAnchor) {
    if (!uiState_->zoomSlider_) return;
    const float before = zoomAmount_;
    const float beforeEffective = EffectiveFillZoom(zoomEnabled_ ? before : 1.0f, ViewportFitZoom());
    const auto previousFit = pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fit
        ? ViewportFitMode::kFit : ViewportFitMode::kFill;
    if (enable) SetViewportFitMode(settings::ViewportFitModeSetting::Fill, false);
    const bool enableChanged = enable && !zoomEnabled_;
    if (enableChanged) {
        if (uiState_->zoomCheckbox_) {
            const QSignalBlocker blocker(uiState_->zoomCheckbox_); uiState_->zoomCheckbox_->setChecked(true);
        }
        zoomEnabled_ = true; UpdateControlEnabledStates();
    }
    const int requested = std::clamp(static_cast<int>(std::lround(ClampZoomMultiplier(amount)*kZoomSliderScale)),
        1, kZoomSliderMaxMultiplier * kZoomSliderScale);
    const float after = float(requested)/kZoomSliderScale;
    const bool changed = after != before;
    if (changed) {
        zoomAmount_ = after;
    }
    RefreshZoomRange();
    if (anchor && (changed || recenterAnchor)) {
        float x = anchor->x(), y = anchor->y();
        const auto source = CurrentViewSourceSize();
        if (!recenterAnchor && uiState_->renderWidget_ && source.width() > 0 && source.height() > 0) {
            const auto width = static_cast<unsigned int>(std::max(1,uiState_->renderWidget_->width()));
            const auto height = static_cast<unsigned int>(std::max(1,uiState_->renderWidget_->height()));
            const auto fit = pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fit
                ? ViewportFitMode::kFit : ViewportFitMode::kFill;
            const auto previous = ComputeViewTransform(source.width(),source.height(),width,height,beforeEffective,zoomCenterX_,zoomCenterY_,previousFit);
            const auto next = ComputeViewTransform(source.width(),source.height(),width,height,after,zoomCenterX_,zoomCenterY_,fit);
            if (!ComputeZoomAnchorCenter(previous,next,float(anchor->x()),float(anchor->y()),x,y)) {
                x = zoomCenterX_; y = zoomCenterY_;
            }
        }
        SetZoomCenter(x,y,true,true);
    } else if (changed || enableChanged) SyncCurrentConfigToPersistence(true);
    if (changed || enableChanged || (anchor && recenterAnchor)) {
        pipelineOrchestrator_->NotifyViewportMotion();
        pipelineOrchestrator_->MarkViewportDirty(); UpdateProcessingStatusLabel();
    }
}

void OkuFlowApp::OnDebugViewToggled(bool checked) {
    debugViewEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OkuFlowApp::OnZoomCenterXChanged(int value) {
    if (suspendControlSync_) {
        return;
    }
    const float norm = std::clamp(static_cast<float>(value) / static_cast<float>(kZoomFocusSliderScale), 0.0f, 1.0f);
    SetZoomCenter(norm, zoomCenterY_, false);
}

void OkuFlowApp::OnZoomCenterYChanged(int value) {
    if (suspendControlSync_) {
        return;
    }
    const float norm = std::clamp(static_cast<float>(value) / static_cast<float>(kZoomFocusSliderScale), 0.0f, 1.0f);
    SetZoomCenter(zoomCenterX_, norm, false);
}

void OkuFlowApp::OnRotationSelectionChanged(int index) {
    SetRotationQuarterTurns(index);
}

void OkuFlowApp::SetRotationQuarterTurns(int index) {
    if (!uiState_->rotationCombo_) {
        return;
    }

    const int clamped = std::clamp(index, 0, 3);
    const int previous = ((rotationQuarterTurns_ % 4) + 4) % 4;
    if (clamped == previous) {
        return;
    }

    const int delta = (clamped - previous + 4) % 4;
    rotationQuarterTurns_ = clamped;
    settingsController_->MutableSettings().rotationQuarterTurns = CameraOriginalRotationQuarterTurns();
    RememberCameraWorkingSet();

    float rotatedX = zoomCenterX_;
    float rotatedY = zoomCenterY_;
    RotateNormalizedPoint(zoomCenterX_, zoomCenterY_, delta, rotatedX, rotatedY);
    SetZoomCenter(rotatedX, rotatedY, true, true);
    if (mainWindow_) mainWindow_->rotateAnnotationsClockwise(delta);

    cpuPipeline_.ResetTemporalHistory();
    if (!MediaViewSelected() && cudaSurface_) {
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

    presentationBuffer_.clear();
    presentationWidth_ = 0;
    presentationHeight_ = 0;

    UpdateRotationUi();
    SyncCurrentConfigToPersistence();
}

void OkuFlowApp::OnControlsCollapsedToggled(bool checked) {
    Q_UNUSED(checked);
    controlsCollapsed_ = false;
    if (uiState_->controlsContainer_) {
        uiState_->controlsContainer_->show();
    }
    if (uiState_->collapseButton_) {
        const QSignalBlocker blocker(uiState_->collapseButton_);
        uiState_->collapseButton_->setChecked(true);
        uiState_->collapseButton_->setArrowType(Qt::DownArrow);
        uiState_->collapseButton_->setText("Advanced Tuning");
    }
    settingsController_->MutableSettings().controlsCollapsed = false;
}

void OkuFlowApp::OnVirtualJoystickToggled(bool checked) {
    virtualJoystickEnabled_ = checked;
    if (!virtualJoystickEnabled_) {
        if (interactionController_) {
            interactionController_->ResetJoystick();
        }
        if (joystickOverlay_) {
            joystickOverlay_->ResetKnob();
        }
    } else {
        if (interactionController_) {
            interactionController_->ResetJoystick();
        }
    }
    UpdateJoystickVisibility();
    settingsController_->MutableSettings().virtualJoystick = virtualJoystickEnabled_;
    UpdateSectionChangedCounts();
    SavePersistentSettings();
}

void OkuFlowApp::OnBlurToggled(bool checked) {
    blurEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateBlurUiLabels();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnBlurSigmaChanged(int value) {
    blurSigma_ = SliderValueToSigma(value);
    UpdateBlurUiLabels();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnBlurRadiusChanged(int value) {
    const int snapped = SnapBlurRadius(value);
    if (uiState_->blurRadiusSlider_ && snapped != value) {
        auto blocker = uiState_->BlockSignals(uiState_->blurRadiusSlider_);
        uiState_->blurRadiusSlider_->setValue(snapped);
    }
    blurRadius_ = snapped;
    UpdateBlurUiLabels();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OkuFlowApp::OnFocusMarkerToggled(bool checked) {
    focusMarkerEnabled_ = checked;
    UpdateControlEnabledStates();
    pipelineOrchestrator_->MarkViewportDirty();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnSpatialSharpenToggled(bool checked) {
    spatialSharpenEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateSpatialSharpenUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnSpatialUpscalerChanged(int index) {
    const int clamped = std::clamp(index, 0, 1);
    spatialUpscaler_ = static_cast<SpatialUpscaler>(clamped);
    UpdateSpatialSharpenUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnSpatialSharpnessChanged(int value) {
    spatialSharpness_ = std::clamp(static_cast<float>(value) / 100.0f, 0.0f, 1.0f);
    if (uiState_->spatialSharpnessValueLabel_) {
        SetLiveText(uiState_->spatialSharpnessValueLabel_,
                    QLocale().toString(spatialSharpness_, 'f', 2),
                    LivePoliteness::kSilent,
                    QStringLiteral("Sharpness"));
    }
    UpdateSpatialSharpenUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnTemporalSmoothToggled(bool checked) {
    temporalSmoothEnabled_ = checked;
    UpdateControlEnabledStates();
    cpuPipeline_.ResetTemporalHistory();
    if (!MediaViewSelected() && cudaSurface_) {
        cudaSurface_->ResetTemporalHistory();
    }
    UpdateTemporalSmoothUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnTemporalSmoothStrengthChanged(int value) {
    const int sliderMin = uiState_->temporalSmoothSlider_ ? uiState_->temporalSmoothSlider_->minimum() : 1;
    const int sliderMax = uiState_->temporalSmoothSlider_ ? uiState_->temporalSmoothSlider_->maximum() : 100;
    const int clamped = std::clamp(value, sliderMin, sliderMax);
    if (uiState_->temporalSmoothSlider_ && clamped != value) {
        auto block = uiState_->BlockSignals(uiState_->temporalSmoothSlider_);
        uiState_->temporalSmoothSlider_->setValue(clamped);
    }
    temporalSmoothAlpha_ = std::clamp(static_cast<float>(clamped) / 100.0f, 0.0f, 1.0f);
    if (uiState_->temporalSmoothValueLabel_) {
        SetLiveText(uiState_->temporalSmoothValueLabel_,
                    QLocale().toString(temporalSmoothAlpha_, 'f', 2),
                    LivePoliteness::kSilent,
                    QStringLiteral("Temporal blend"));
    }
    cpuPipeline_.ResetTemporalHistory();
    if (!MediaViewSelected() && cudaSurface_) {
        cudaSurface_->ResetTemporalHistory();
    }
    UpdateTemporalSmoothUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnStabilizationToggled(bool checked) {
    stabilizationEnabled_ = checked;
    if (!checked) {
        bumpHoldEnabled_ = false;
        if (mainWindow_ && mainWindow_->bumpHoldCheckbox()) {
            const QSignalBlocker blocker(mainWindow_->bumpHoldCheckbox());
            mainWindow_->bumpHoldCheckbox()->setChecked(false);
        }
    }
    UpdateControlEnabledStates();
    if (!MediaViewSelected() && cudaSurface_) {
        cudaSurface_->ResetStabilization();
        cudaSurface_->ResetKeystone();
        ++uctWarpRevision_;
        cudaSurface_->ResetTextClarityHistory();
    }
    UpdateKeystoneTrackingUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnBumpHoldToggled(bool checked) {
    bumpHoldEnabled_ =
        checked && stabilizationEnabled_;
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
}
void OkuFlowApp::OnKeystoneToggled(bool checked) {
    keystoneEnabled_ = checked;
    UpdateControlEnabledStates();
    if (!MediaViewSelected() && cudaSurface_) {
        cudaSurface_->ResetKeystone();
        ++uctWarpRevision_;
        cudaSurface_->ResetTextClarityHistory();
    }
    UpdateKeystoneTrackingUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OkuFlowApp::OpenSetupAssistant()
{
    if (!mainWindow_) {
        return;
    }
    if (setupAssistantDialog_) {
        setupAssistantDialog_->show();
        setupAssistantDialog_->raise();
        setupAssistantDialog_->activateWindow();
        return;
    }

    setupAssistantDialog_ = new SetupAssistantDialog(
        settingsController_->MutableSettings().assistive.codexExecutablePath,
        settingsController_->MutableSettings().setupAssistantDeclined,
        mainWindow_.get());
    connect(setupAssistantDialog_, &QObject::destroyed, this, [this]() {
        setupAssistantDialog_ = nullptr;
    });
    connect(setupAssistantDialog_, &SetupAssistantDialog::CodexPathChanged,
            this, [this](const QString& path) {
                settingsController_->MutableSettings().assistive.codexExecutablePath = path;
                assistiveManager_->ApplySettings(
                    settingsController_->MutableSettings().assistive);
                SavePersistentSettings();
            });
    connect(setupAssistantDialog_, &SetupAssistantDialog::DeclinePreferenceChanged,
            this, [this](bool declined) {
                settingsController_->MutableSettings().setupAssistantDeclined = declined;
                SavePersistentSettings();
            });
    connect(setupAssistantDialog_, &SetupAssistantDialog::DependenciesChanged,
            this, [this]() {
                if (!MediaViewSelected() && cudaSurface_) {
                    cudaSurface_->ResetSuperRes();
                }
                mainWindow_->setMaxineRuntimeInstalled(MaxineSuperRes::IsRuntimeInstalled());
                assistiveManager_->ApplySettings(
                    settingsController_->MutableSettings().assistive);
                UpdateProcessingStatusLabel();
            });
    setupAssistantDialog_->show();
}

void OkuFlowApp::OpenUserDataFolder()
{
    if (!mainWindow_ || !userDataPaths_) {
        return;
    }
    QString error;
    const QString root = userDataPaths_->Root(&error);
    if (root.isEmpty()) {
        QMessageBox::warning(mainWindow_.get(),
                             QStringLiteral("Open OkuFlow Folder"),
                             error.isEmpty()
                                 ? QStringLiteral(
                                       "The OkuFlow folder is unavailable.")
                                 : error);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(root))) {
        QMessageBox::warning(
            mainWindow_.get(),
            QStringLiteral("Open OkuFlow Folder"),
            QStringLiteral("Windows could not open:\n%1")
                .arg(QDir::toNativeSeparators(root)));
        return;
    }
    const QString message =
        QStringLiteral("Opened your OkuFlow folder.");
    ShowStatusMessage(message, 3500);
}

void OkuFlowApp::ChangeUserDataFolder()
{
    if (!mainWindow_ || !userDataPaths_ || !settingsController_) {
        return;
    }
    QString currentError;
    const QString currentRoot = userDataPaths_->Root(&currentError);
    const QString selected = QFileDialog::getExistingDirectory(
        mainWindow_.get(),
        QStringLiteral("Choose OkuFlow Folder"),
        currentRoot,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) {
        return;
    }

    QString error;
    if (!SetUserDataRoot(selected, &error)) {
        QMessageBox::warning(
            mainWindow_.get(),
            QStringLiteral("Choose OkuFlow Folder"),
            error.isEmpty()
                ? QStringLiteral("OkuFlow cannot use the selected folder.")
                : error);
        return;
    }

    const QString message =
        QStringLiteral("New OkuFlow files will be saved in %1.")
            .arg(QDir::toNativeSeparators(userDataPaths_->Root()));
    ShowStatusMessage(message, 7000);
}
bool OkuFlowApp::SetUserDataRoot(const QString& path, QString* error) {
    if (!userDataPaths_ || !settingsController_ || !userDataPaths_->SetConfiguredRoot(path,error)) return false;
    settingsController_->MutableSettings().userDataRoot = userDataPaths_->ConfiguredRoot();
    if (!controlTarget_ || !controlTarget_->IsApplyingControlledTransition()) {
        if (controlTarget_) controlTarget_->RememberOutputRoot(userDataPaths_->ConfiguredRoot());
        assistiveManager_->ApplySettings(settingsController_->MutableSettings().assistive);
        SavePersistentSettings();
    }
    return true;
}

void OkuFlowApp::OnKeystoneStepBack() {
    const bool media = MediaViewSelected();
    if (!keystoneEnabled_ || (media ? !SelectedMediaGpuProcessingAvailable() || !mediaGpuScene_ : !cudaSurface_)) {
        return;
    }
    if (media ? mediaGpuScene_->StepKeystoneCorrection(-1) : cudaSurface_->StepKeystoneCorrection(-1)) {
        ++uctWarpRevision_;
        if (!media) cudaSurface_->ResetTextClarityHistory();
        else ++mediaSceneAppearanceRevision_; // admit the exact still again
        pipelineOrchestrator_->MarkViewportDirty();
        ShowStatusMessage(QStringLiteral("Screen correction moved back and tracking stopped."), 3500);
    }
    UpdateKeystoneTrackingUi();
}
void OkuFlowApp::OnKeystonePauseResume() {
    const bool media = MediaViewSelected();
    if (!keystoneEnabled_ || (media ? !SelectedMediaGpuProcessingAvailable() || !mediaGpuScene_ || !mediaGpuScene_->SceneReady() : !cudaSurface_)) {
        return;
    }
    const bool pause = !(media ? mediaGpuScene_->GetKeystoneTrackingState().paused : cudaSurface_->GetKeystoneTrackingState().paused);
    if (media) mediaGpuScene_->SetKeystoneTrackingPaused(pause);
    else cudaSurface_->SetKeystoneTrackingPaused(pause);
    ShowStatusMessage(pause ? QStringLiteral("Automatic screen correction stopped.")
                            : QStringLiteral("Automatic screen correction continuing."),
                      3500);
    UpdateKeystoneTrackingUi();
}
void OkuFlowApp::OnKeystoneStepForward() {
    const bool media = MediaViewSelected();
    if (!keystoneEnabled_ || (media ? !SelectedMediaGpuProcessingAvailable() || !mediaGpuScene_ : !cudaSurface_)) {
        return;
    }
    if (media ? mediaGpuScene_->StepKeystoneCorrection(1) : cudaSurface_->StepKeystoneCorrection(1)) {
        ++uctWarpRevision_;
        if (!media) cudaSurface_->ResetTextClarityHistory();
        else ++mediaSceneAppearanceRevision_; // admit the exact still again
        pipelineOrchestrator_->MarkViewportDirty();
        const bool pending = media ? mediaGpuScene_->GetKeystoneTrackingState().stepPending : cudaSurface_->GetKeystoneTrackingState().stepPending;
        ShowStatusMessage(pending ? QStringLiteral("Finding one new screen correction...")
                                  : QStringLiteral("Using the next screen correction."),
                          3500);
    }
    UpdateKeystoneTrackingUi();
}
void OkuFlowApp::UpdateKeystoneTrackingUi() {
    if (!mainWindow_) {
        return;
    }
    const bool media = MediaViewSelected();
    const bool available = media ? SelectedMediaGpuProcessingAvailable() && mediaGpuScene_ && mediaGpuScene_->SceneReady()
                                : cameraActive_ && cudaSurface_ && cudaSurface_->IsValid() && cudaPipelineAvailable_;
    const KeystoneTrackingState state = media
        ? (mediaGpuScene_ ? mediaGpuScene_->GetKeystoneTrackingState() : KeystoneTrackingState{})
        : (cudaSurface_ ? cudaSurface_->GetKeystoneTrackingState() : KeystoneTrackingState{});
    mainWindow_->setKeystoneTrackingControls(keystoneEnabled_ && (!media || SelectedMediaGpuProcessingAvailable()), available,
                                             state.paused, state.canStepBack,
                                             state.canStepForward, state.stepPending,
                                             state.position, state.count);
}
void OkuFlowApp::OnAutoContrastToggled(bool checked) {
    autoContrastEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnAutoContrastStrengthChanged(int value) {
    autoContrastStrength_ = std::clamp(static_cast<float>(value) / 100.0f, 0.0f, 1.0f);
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnTextClarityControlsChanged() {
    const bool wasSuperResEnabled = mlTextSuperResolutionEnabled_;
    const bool wasZoomEnabled = zoomEnabled_;
    const float previousZoom = zoomAmount_;
    autoTextClarityEnabled_ = uiState_->textClarityCheckbox_ && uiState_->textClarityCheckbox_->isChecked();
    if (ultraClearText_) ultraClearText_->SetTextClarityEnabled(autoTextClarityEnabled_);
    if (!autoTextClarityEnabled_ && assistiveManager_) assistiveManager_->ClearFocusWarning();
    backgroundFlattenEnabled_ = uiState_->backgroundFlattenCheckbox_ && uiState_->backgroundFlattenCheckbox_->isChecked();
    backgroundFlattenStrength_ = uiState_->backgroundFlattenStrengthSlider_
                                     ? std::clamp(uiState_->backgroundFlattenStrengthSlider_->value() / 100.0f, 0.0f, 1.0f) : 0.8f;
    adaptiveBinarizationEnabled_ = uiState_->adaptiveBinarizationCheckbox_ && uiState_->adaptiveBinarizationCheckbox_->isChecked();
    sauvolaStrength_ = uiState_->sauvolaStrengthSlider_
                           ? std::clamp(uiState_->sauvolaStrengthSlider_->value() / 100.0f, 0.1f, 0.5f) : 0.28f;
    binarizationSoftness_ = uiState_->binarizationSoftnessSlider_
                                ? std::clamp(uiState_->binarizationSoftnessSlider_->value() / 100.0f, 0.0f, 0.25f) : 0.06f;
    textPolarityMode_ = uiState_->textPolarityCombo_ ? std::clamp(uiState_->textPolarityCombo_->currentIndex(), 0, 2) : 0;
    strokeWeight_ = uiState_->strokeWeightSlider_ ? std::clamp(uiState_->strokeWeightSlider_->value(), -3, 3) : 0;
    smartSharpenEnabled_ = uiState_->smartSharpenCheckbox_ && uiState_->smartSharpenCheckbox_->isChecked();
    smartSharpenStrength_ = uiState_->smartSharpenStrengthSlider_
                                ? std::clamp(uiState_->smartSharpenStrengthSlider_->value() / 100.0f, 0.0f, 1.0f) : 0.45f;
    claheEnabled_ = uiState_->claheCheckbox_ && uiState_->claheCheckbox_->isChecked();
    claheClipLimit_ = uiState_->claheClipLimitSlider_
                          ? std::clamp(uiState_->claheClipLimitSlider_->value() / 10.0f, 1.0f, 8.0f) : 2.0f;
    twoColorTextEnabled_ = uiState_->twoColorTextCheckbox_ && uiState_->twoColorTextCheckbox_->isChecked();
    textHysteresisEnabled_ = uiState_->textHysteresisCheckbox_ && uiState_->textHysteresisCheckbox_->isChecked();
    textHysteresisStrength_ = uiState_->textHysteresisStrengthSlider_
                                  ? std::clamp(uiState_->textHysteresisStrengthSlider_->value() / 100.0f, 0.0f, 0.25f) : 0.08f;
    selectiveSharpenEnabled_ = uiState_->selectiveSharpenCheckbox_ && uiState_->selectiveSharpenCheckbox_->isChecked();
    focusDetectionEnabled_ = uiState_->focusDetectionCheckbox_ && uiState_->focusDetectionCheckbox_->isChecked();
    focusThreshold_ = uiState_->focusThresholdSlider_
                          ? std::clamp(uiState_->focusThresholdSlider_->value() / 1000.0f, 0.001f, 0.1f) : 0.012f;
    glareSuppressionEnabled_ = uiState_->glareSuppressionCheckbox_ && uiState_->glareSuppressionCheckbox_->isChecked();
    glareSuppressionStrength_ = uiState_->glareSuppressionStrengthSlider_
                                    ? std::clamp(uiState_->glareSuppressionStrengthSlider_->value() / 100.0f, 0.0f, 1.0f) : 0.5f;
#if OKUFLOW_ENABLE_TEXT_SR
    if (uiState_->mlTextSuperResolutionCheckbox_ && uiState_->mlTextSuperResolutionCheckbox_->isChecked()) {
        mlTextSuperResolutionUltra1440p_ =
            uiState_->mlTextSuperResolutionUltra1440pCheckbox_ &&
            uiState_->mlTextSuperResolutionUltra1440pCheckbox_->isChecked();
        if (mlTextSuperResolutionUltra1440p_ &&
            uiState_->mlTextSuperResolutionPrefer2xCheckbox_ &&
            uiState_->mlTextSuperResolutionPrefer2xCheckbox_->isChecked()) {
            auto block = uiState_->BlockSignals(
                uiState_->mlTextSuperResolutionPrefer2xCheckbox_);
            uiState_->mlTextSuperResolutionPrefer2xCheckbox_->setChecked(false);
        }
        mlTextSuperResolutionPrefer2x_ =
            !mlTextSuperResolutionUltra1440p_ &&
            uiState_->mlTextSuperResolutionPrefer2xCheckbox_ &&
            uiState_->mlTextSuperResolutionPrefer2xCheckbox_->isChecked();
        if (uiState_->mlTextSuperResolutionStrengthSlider_ &&
            uiState_->mlTextSuperResolutionStrengthSlider_->value() <= 0) {
            auto block = uiState_->BlockSignals(uiState_->mlTextSuperResolutionStrengthSlider_);
            uiState_->mlTextSuperResolutionStrengthSlider_->setValue(65);
        }
        if (!mlTextSuperResolutionUltra1440p_ &&
            uiState_->zoomCheckbox_ && !uiState_->zoomCheckbox_->isChecked()) {
            auto block = uiState_->BlockSignals(uiState_->zoomCheckbox_);
            uiState_->zoomCheckbox_->setChecked(true);
        }
        if (!mlTextSuperResolutionUltra1440p_) {
            zoomEnabled_ = true;
        }
        if (!mlTextSuperResolutionUltra1440p_ && uiState_->zoomSlider_) {
            // NVIDIA's smallest supported SuperRes ratio is 4/3. The slider
            // stores hundredths, so 1.33 is the closest user-facing value;
            // the CUDA stage itself uses the exact 4/3 ratio.
            const int minimumZoom =
                mlTextSuperResolutionPrefer2x_ ? 200 : 133;
            if (uiState_->zoomSlider_->value() < minimumZoom) {
                auto block = uiState_->BlockSignals(uiState_->zoomSlider_);
                uiState_->zoomSlider_->setValue(minimumZoom);
            }
            zoomAmount_ = std::max(
                static_cast<float>(minimumZoom) /
                    static_cast<float>(kZoomSliderScale),
                static_cast<float>(uiState_->zoomSlider_->value()) /
                    static_cast<float>(kZoomSliderScale));
        } else if (!mlTextSuperResolutionUltra1440p_) {
            zoomAmount_ = std::max(
                zoomAmount_, mlTextSuperResolutionPrefer2x_ ? 2.0f : 1.33f);
        }
    } else {
        mlTextSuperResolutionUltra1440p_ =
            uiState_->mlTextSuperResolutionUltra1440pCheckbox_ &&
            uiState_->mlTextSuperResolutionUltra1440pCheckbox_->isChecked();
        mlTextSuperResolutionPrefer2x_ =
            !mlTextSuperResolutionUltra1440p_ &&
            uiState_->mlTextSuperResolutionPrefer2xCheckbox_ &&
            uiState_->mlTextSuperResolutionPrefer2xCheckbox_->isChecked();
    }
    mlTextSuperResolutionEnabled_ = uiState_->mlTextSuperResolutionCheckbox_ && uiState_->mlTextSuperResolutionCheckbox_->isChecked();
    mlTextSuperResolutionStrength_ = uiState_->mlTextSuperResolutionStrengthSlider_
                                         ? std::clamp(uiState_->mlTextSuperResolutionStrengthSlider_->value() / 100.0f,
                                                      0.0f, 1.0f)
                                         : 0.65f;
#else
    mlTextSuperResolutionEnabled_ = false;
#endif
    if (wasSuperResEnabled && !mlTextSuperResolutionEnabled_) {
        superResPerformanceOverride_ = false;
        if (mainWindow_) {
            mainWindow_->setSuperResPerformanceOverrideChecked(false);
        }
    }

    if (wasZoomEnabled != zoomEnabled_ || previousZoom != zoomAmount_) {
        pipelineOrchestrator_->NotifyViewportMotion();
        pipelineOrchestrator_->MarkViewportDirty();
    }
    UpdateControlEnabledStates();
    if (!MediaViewSelected() && cudaSurface_) {
        cudaSurface_->ResetTextClarityHistory();
        // The CUDA surface reloads changed model parameters with proper stream
        // retirement. Unrelated text controls retain the model and timing state.
        if (wasSuperResEnabled != mlTextSuperResolutionEnabled_) {
            cudaSurface_->ResetSuperRes();
        }
        cudaSurface_->SetSuperResPerformanceOverride(superResPerformanceOverride_);
    } else if (MediaViewSelected() && mediaGpuScene_) {
        mediaGpuScene_->ResetTextClarityHistory();
        if (wasSuperResEnabled != mlTextSuperResolutionEnabled_) mediaGpuScene_->ResetSuperRes();
        mediaGpuScene_->SetSuperResPerformanceOverride(superResPerformanceOverride_);
        ++mediaSceneAppearanceRevision_;
        pipelineOrchestrator_->MarkViewportDirty();
    }
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OkuFlowApp::UpdateControlEnabledStates() {
    if (!uiState_) return;
    const auto setEnabled = [](QWidget* widget, bool enabled) {
        if (widget) {
            widget->setEnabled(enabled);
        }
    };
    const bool cameraView = !MediaViewSelected();
    const bool gpuView = cameraView || SelectedMediaGpuProcessingAvailable();

    setEnabled(uiState_->bwSlider_, blackWhiteEnabled_);
    setEnabled(uiState_->zoomSlider_, zoomEnabled_);
    setEnabled(uiState_->zoomCenterXSlider_, zoomEnabled_);
    setEnabled(uiState_->zoomCenterYSlider_, zoomEnabled_);
    setEnabled(uiState_->blurSigmaSlider_, gpuView && blurEnabled_);
    setEnabled(uiState_->blurRadiusSlider_, gpuView && blurEnabled_);
    setEnabled(uiState_->blurSigmaValueLabel_, gpuView && blurEnabled_);
    setEnabled(uiState_->blurRadiusValueLabel_, gpuView && blurEnabled_);
    setEnabled(uiState_->temporalSmoothSlider_, gpuView && temporalSmoothEnabled_);
    setEnabled(uiState_->temporalSmoothValueLabel_, gpuView && temporalSmoothEnabled_);
    setEnabled(uiState_->autoContrastStrengthSlider_, gpuView && autoContrastEnabled_);
    setEnabled(uiState_->focusMarkerCheckbox_, gpuView && (!cameraView || !debugViewEnabled_));
    setEnabled(mainWindow_ ? mainWindow_->bumpHoldCheckbox() : nullptr,
               gpuView && stabilizationEnabled_);
    setEnabled(uiState_->spatialBackendCombo_, gpuView && spatialSharpenEnabled_);
    setEnabled(uiState_->spatialSharpnessSlider_, gpuView && spatialSharpenEnabled_);
    setEnabled(uiState_->spatialSharpnessValueLabel_, gpuView && spatialSharpenEnabled_);

    setEnabled(uiState_->backgroundFlattenStrengthSlider_,
               gpuView && (backgroundFlattenEnabled_ || autoTextClarityEnabled_));
    setEnabled(uiState_->sauvolaStrengthSlider_,
               gpuView && (adaptiveBinarizationEnabled_ || autoTextClarityEnabled_));
    setEnabled(uiState_->binarizationSoftnessSlider_,
               gpuView && (adaptiveBinarizationEnabled_ || autoTextClarityEnabled_));
    setEnabled(uiState_->smartSharpenStrengthSlider_,
               gpuView && (smartSharpenEnabled_ || autoTextClarityEnabled_));
    setEnabled(uiState_->claheClipLimitSlider_, gpuView && claheEnabled_);
    setEnabled(uiState_->textHysteresisStrengthSlider_,
               gpuView && (textHysteresisEnabled_ || autoTextClarityEnabled_));
    setEnabled(uiState_->focusThresholdSlider_,
               gpuView && (focusDetectionEnabled_ || autoTextClarityEnabled_));
    setEnabled(uiState_->glareSuppressionStrengthSlider_,
               gpuView && (glareSuppressionEnabled_ || autoTextClarityEnabled_));

    const bool superResAvailable =
        gpuView && mlTextSuperResolutionEnabled_ && mainWindow_ &&
        mainWindow_->isMaxineRuntimeInstalled();
    setEnabled(uiState_->mlTextSuperResolutionStrengthSlider_,
               superResAvailable);
    setEnabled(uiState_->mlTextSuperResolutionPrefer2xCheckbox_,
               superResAvailable);
    setEnabled(uiState_->mlTextSuperResolutionUltra1440pCheckbox_,
               superResAvailable);
    if (mainWindow_) mainWindow_->refreshTextClarityUi();
    // Keep requested preferences intact while CPU compatibility limits effects.
    for (QWidget* control : std::initializer_list<QWidget*>{
        uiState_->blurCheckbox_, uiState_->temporalSmoothCheckbox_,
        uiState_->spatialSharpenCheckbox_, uiState_->stabilizationCheckbox_, uiState_->keystoneCheckbox_,
        uiState_->autoContrastCheckbox_, uiState_->simpleTextClarityCheckbox_, uiState_->textClarityCheckbox_,
        uiState_->backgroundFlattenCheckbox_, uiState_->adaptiveBinarizationCheckbox_,
        uiState_->textPolarityCombo_, uiState_->strokeWeightSlider_, uiState_->smartSharpenCheckbox_,
        uiState_->claheCheckbox_, uiState_->twoColorTextCheckbox_, uiState_->textHysteresisCheckbox_,
        uiState_->selectiveSharpenCheckbox_, uiState_->focusDetectionCheckbox_, uiState_->glareSuppressionCheckbox_}) setEnabled(control, gpuView);
    setEnabled(uiState_->debugButton_, cameraView);
    setEnabled(uiState_->vlmAssistCheckbox_, cameraView);
#if OKUFLOW_ENABLE_TEXT_SR
    setEnabled(uiState_->mlTextSuperResolutionCheckbox_, gpuView && mainWindow_ && mainWindow_->isMaxineRuntimeInstalled());
#else
    setEnabled(uiState_->mlTextSuperResolutionCheckbox_, false);
#endif
    // Config sync owns preset matching and section badges. Transient controls
    // such as Extra Stable do not affect saved profiles.
}

void OkuFlowApp::UpdateSectionChangedCounts() {
    if (!mainWindow_ || !uiState_) {
        return;
    }
    settings::AdvancedConfig profileDefault;
    const QString presetId = settingsController_->Settings().selectedPresetId;
    if (!presetId.isEmpty()) {
        if (const auto preset = settingsController_->ResolvePreset(presetId)) {
            profileDefault = *preset;
        }
    }
    mainWindow_->updateSectionChangedCounts(CaptureCurrentAdvancedConfig(),
                                            profileDefault);
}

void OkuFlowApp::SetSuperResPerformanceOverride(bool enabled) {
#if OKUFLOW_ENABLE_TEXT_SR
    const bool media = MediaViewSelected();
    if (!mlTextSuperResolutionEnabled_ || (media ? !SelectedMediaGpuProcessingAvailable() : !cudaSurface_)) {
        return;
    }
    superResPerformanceOverride_ = enabled;
    if (media) {
        if (!mediaGpuScene_) mediaGpuScene_ = std::make_unique<MediaGpuScene>();
        mediaGpuScene_->SetSuperResPerformanceOverride(enabled);
    }
    else cudaSurface_->SetSuperResPerformanceOverride(enabled);
    ShowStatusMessage(enabled
                          ? QStringLiteral("NVIDIA Super Resolution performance limit ignored.")
                          : QStringLiteral("NVIDIA Super Resolution performance limit restored."));
#else
    Q_UNUSED(enabled);
#endif
}
void OkuFlowApp::OnDisplayColorSchemeChanged() {
    if (uiState_->displayColorPicker_) {
        displayColorScheme_ = color_schemes::NormalizeColorScheme(
            uiState_->displayColorPicker_->currentScheme(), displayColorMode_);
    }
    displayColorMode_ = displayColorScheme_.legacyMode >= 0
                            ? displayColorScheme_.legacyMode
                            : 0;
    displayColorLut_ = color_schemes::BuildColorLut(displayColorScheme_);
    ++displayColorLutGeneration_;
    if (displayColorLutGeneration_ == 0) {
        displayColorLutGeneration_ = 1;
    }
    if (uiState_->displayColorPicker_ && uiState_->displayColorPicker_->hasCustomScheme()) {
        settingsController_->MutableSettings().customColorScheme = uiState_->displayColorPicker_->customScheme();
    }
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
bool OkuFlowApp::SetDisplayColorSchemeById(const QString& id) {
    const auto* scheme = color_schemes::FindBuiltInColorScheme(id);
    if (!scheme || !uiState_->displayColorPicker_) return false;
    if (color_schemes::SchemesEquivalent(displayColorScheme_,*scheme)) return true;
    uiState_->displayColorPicker_->setCurrentScheme(*scheme);
    OnDisplayColorSchemeChanged(); return true;
}
void OkuFlowApp::SetViewportFitMode(settings::ViewportFitModeSetting mode, bool save) {
    if (settingsController_->MutableSettings().viewportFitMode == mode) return;
    settingsController_->MutableSettings().viewportFitMode = mode;
    if (uiState_->viewportFitCombo_) {
        const QSignalBlocker blocker(uiState_->viewportFitCombo_);
        uiState_->viewportFitCombo_->setCurrentIndex(mode == settings::ViewportFitModeSetting::Fit ? 1 : 0);
    }
    pipelineOrchestrator_->SetViewportFitMode(mode); UpdateSectionChangedCounts();
    if (save) SavePersistentSettings();
}
void OkuFlowApp::OnContrastChanged(int value) {
    contrast_ = std::clamp(static_cast<float>(value) / 100.0f, 0.25f, 4.0f);
    SyncCurrentConfigToPersistence();
}
void OkuFlowApp::OnBrightnessChanged(int value) {
    brightness_ = std::clamp(static_cast<float>(value) / 100.0f, -1.0f, 1.0f);
    SyncCurrentConfigToPersistence();
}

} // namespace okuflow

#endif // _WIN32
