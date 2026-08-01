#ifdef _WIN32

#include "app_internal.hpp"

namespace openzoom {

void OpenZoomApp::OnCameraSelectionChanged(int index) {
    if (index < 0 || static_cast<size_t>(index) >= cameras_.size()) {
        return;
    }

    // A manual camera pick always wins over an in-flight automatic reconnect.
    pipelineOrchestrator_->CancelCameraReconnect();
    settingsController_->MutableSettings().cameraIndex = index;
    RefreshCameraFormats(static_cast<size_t>(index));
    StartCameraCapture(static_cast<size_t>(index));
}

void OpenZoomApp::UpdateCameraAccelerationUi()
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
        auto blocker =
            uiState_->BlockSignals(
                uiState_->cameraAccelerationCombo_);
        uiState_->cameraAccelerationCombo_->setCurrentIndex(
            static_cast<int>(acceleration.mode));
    }
    if (uiState_->testCameraAccelerationButton_) {
        uiState_->testCameraAccelerationButton_->setEnabled(
            selectedCameraIndex_ >= 0 &&
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

void OpenZoomApp::OnTestCameraAcceleration()
{
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
                "The isolated camera test is missing from this OpenZoom "
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

void OpenZoomApp::OnCameraAccelerationModeChanged(int index)
{
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
    SavePersistentSettings();
    StartCameraCapture(
        static_cast<size_t>(selectedCameraIndex_));
}

void OpenZoomApp::OnCameraFormatChanged(int index) {
    if (!uiState_->cameraFormatCombo_ || index < 0) {
        return;
    }
    settingsController_->MutableSettings().cameraFormatStableId =
        uiState_->cameraFormatCombo_->itemData(index).toString();
    SavePersistentSettings();
    if (selectedCameraIndex_ >= 0 &&
        static_cast<size_t>(selectedCameraIndex_) < cameras_.size()) {
        StartCameraCapture(static_cast<size_t>(selectedCameraIndex_));
    }
}

void OpenZoomApp::OnMicrophoneSelectionChanged(int index)
{
    if (!uiState_->microphoneCombo_ || index < 0) {
        return;
    }
    const QString endpoint =
        uiState_->microphoneCombo_->itemData(index).toString();
    settingsController_->MutableSettings().microphoneEndpointId =
        endpoint;
    if (recordingManager_ && recordingManager_->IsActive()) {
        recordingManager_->Stop(
            QStringLiteral(
                "Recording stopped because the microphone selection changed."));
        StopMicrophoneCapture();
    }
    SavePersistentSettings();
}

bool OpenZoomApp::StartSelectedMicrophone()
{
    StopMicrophoneCapture();
    if (recordingManager_) {
        recordingManager_->SetAudioCaptureEnabled(false);
    }
    if (!uiState_ || !uiState_->microphoneCombo_) {
        return true;
    }
    const QString endpoint =
        uiState_->microphoneCombo_->currentData().toString();
    if (endpoint.isEmpty() ||
        endpoint == QStringLiteral("__none__")) {
        return true;
    }

    const auto microphone = std::find_if(
        microphones_.cbegin(), microphones_.cend(),
        [&endpoint](const AudioDeviceDescriptor& candidate) {
            return QString::fromStdWString(candidate.endpointId) ==
                   endpoint;
        });
    if (microphone == microphones_.cend()) {
        ShowStatusMessage(
            QStringLiteral(
                "The selected microphone is unavailable. Recording will "
                "continue without sound."),
            9000);
        return false;
    }

    const std::shared_ptr<MicrophoneCallbackTarget> callbackTarget =
        microphoneCallbackTarget_;
    std::uint64_t callbackGeneration = 0;
    if (callbackTarget) {
        std::lock_guard lock(callbackTarget->mutex);
        callbackGeneration = ++callbackTarget->generation;
        callbackTarget->accepting = callbackTarget->app == this;
    }

    const bool started = audioCapture_.Start(
        *microphone,
        [callbackTarget, callbackGeneration](AudioFrame&& frame) {
            if (!callbackTarget) {
                return;
            }
            std::lock_guard lock(callbackTarget->mutex);
            OpenZoomApp* app = callbackTarget->app;
            if (!callbackTarget->accepting || !app ||
                callbackTarget->generation != callbackGeneration) {
                return;
            }
            if (app->recordingManager_) {
                app->recordingManager_->AddAudioFrame(std::move(frame));
            }
        },
        [callbackTarget, callbackGeneration](const std::string& message) {
            if (!callbackTarget) {
                return;
            }
            std::lock_guard lock(callbackTarget->mutex);
            OpenZoomApp* app = callbackTarget->app;
            if (!callbackTarget->accepting || !app ||
                callbackTarget->generation != callbackGeneration) {
                return;
            }
            const QString detail = QString::fromStdString(message);
            QMetaObject::invokeMethod(
                app,
                [callbackTarget, callbackGeneration, detail]() {
                    OpenZoomApp* currentApp = nullptr;
                    {
                        std::lock_guard lock(callbackTarget->mutex);
                        if (!callbackTarget->accepting ||
                            callbackTarget->generation !=
                                callbackGeneration) {
                            return;
                        }
                        currentApp = callbackTarget->app;
                    }
                    if (!currentApp) {
                        return;
                    }
                    if (currentApp->recordingManager_) {
                        currentApp->recordingManager_->Stop(
                            QStringLiteral(
                                "Recording stopped because microphone "
                                "capture failed."));
                    }
                    currentApp->StopMicrophoneCapture();
                    currentApp->ShowStatusMessage(detail, 12000);
                },
                Qt::QueuedConnection);
        });
    if (!started) {
        if (callbackTarget) {
            std::lock_guard lock(callbackTarget->mutex);
            if (callbackTarget->generation == callbackGeneration) {
                callbackTarget->accepting = false;
            }
        }
        ShowStatusMessage(
            QStringLiteral(
                "The selected microphone could not start: %1. Recording "
                "will continue without sound.")
                .arg(QString::fromStdString(
                    audioCapture_.LastError())),
            12000);
        return false;
    }
    if (recordingManager_) {
        recordingManager_->SetAudioCaptureEnabled(true);
    }
    return true;
}

void OpenZoomApp::StopMicrophoneCapture(bool retainQueuedAudio)
{
    // Cancel and serialize callback delivery before waiting for ReadSample.
    // If Stop must detach a wedged thread, its retained callback can reach
    // only this independently owned, inactive target. The next Start uses a
    // different generation.
    if (microphoneCallbackTarget_) {
        std::lock_guard lock(microphoneCallbackTarget_->mutex);
        microphoneCallbackTarget_->accepting = false;
        ++microphoneCallbackTarget_->generation;
    }
    audioCapture_.Stop();
    if (recordingManager_ && !retainQueuedAudio) {
        recordingManager_->SetAudioCaptureEnabled(false);
    }
}

void OpenZoomApp::OnBlackWhiteToggled(bool checked) {
    blackWhiteEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::OnBlackWhiteThresholdChanged(int value) {
    blackWhiteThreshold_ = std::clamp(static_cast<float>(value) / 255.0f, 0.0f, 1.0f);
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::OnZoomToggled(bool checked) {
    zoomEnabled_ = checked;
    pipelineOrchestrator_->MarkViewportDirty();
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::OnZoomAmountChanged(int value) {
    zoomAmount_ = std::max(1.0f, static_cast<float>(value) / static_cast<float>(kZoomSliderScale));
    pipelineOrchestrator_->MarkViewportDirty();
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::OnDebugViewToggled(bool checked) {
    debugViewEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::OnZoomCenterXChanged(int value) {
    if (suspendControlSync_) {
        return;
    }
    const float norm = std::clamp(static_cast<float>(value) / static_cast<float>(kZoomFocusSliderScale), 0.0f, 1.0f);
    SetZoomCenter(norm, zoomCenterY_, false);
}

void OpenZoomApp::OnZoomCenterYChanged(int value) {
    if (suspendControlSync_) {
        return;
    }
    const float norm = std::clamp(static_cast<float>(value) / static_cast<float>(kZoomFocusSliderScale), 0.0f, 1.0f);
    SetZoomCenter(zoomCenterX_, norm, false);
}

void OpenZoomApp::OnRotationSelectionChanged(int index) {
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
    settingsController_->MutableSettings().rotationQuarterTurns = rotationQuarterTurns_;

    float rotatedX = zoomCenterX_;
    float rotatedY = zoomCenterY_;
    RotateNormalizedPoint(zoomCenterX_, zoomCenterY_, delta, rotatedX, rotatedY);
    SetZoomCenter(rotatedX, rotatedY, true, true);

    cpuPipeline_.ResetTemporalHistory();
    if (cudaSurface_) {
        cudaSurface_->ResetTemporalHistory();
        cudaSurface_->ResetStabilization();
        cudaSurface_->ResetKeystone();
        cudaSurface_->ResetTextClarityHistory();
    }
    UpdateKeystoneTrackingUi();
    ResetCudaFenceState();

    processedFrameWidth_ = 0;
    processedFrameHeight_ = 0;
    cpuSceneBuffer_.clear();
    cpuSceneWidth_ = 0;
    cpuSceneHeight_ = 0;
    cpuSceneReady_ = false;

    UpdateRotationUi();
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::OnControlsCollapsedToggled(bool checked) {
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

void OpenZoomApp::OnVirtualJoystickToggled(bool checked) {
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

void OpenZoomApp::OnBlurToggled(bool checked) {
    blurEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateBlurUiLabels();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnBlurSigmaChanged(int value) {
    blurSigma_ = SliderValueToSigma(value);
    UpdateBlurUiLabels();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnBlurRadiusChanged(int value) {
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

void OpenZoomApp::OnFocusMarkerToggled(bool checked) {
    focusMarkerEnabled_ = checked;
    UpdateControlEnabledStates();
    pipelineOrchestrator_->MarkViewportDirty();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnSpatialSharpenToggled(bool checked) {
    spatialSharpenEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateSpatialSharpenUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnSpatialUpscalerChanged(int index) {
    const int clamped = std::clamp(index, 0, 1);
    spatialUpscaler_ = static_cast<SpatialUpscaler>(clamped);
    UpdateSpatialSharpenUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnSpatialSharpnessChanged(int value) {
    spatialSharpness_ = std::clamp(static_cast<float>(value) / 100.0f, 0.0f, 1.0f);
    if (uiState_->spatialSharpnessValueLabel_) {
        SetLiveText(uiState_->spatialSharpnessValueLabel_,
                    QString::number(spatialSharpness_, 'f', 2),
                    LivePoliteness::kSilent,
                    QStringLiteral("Sharpness"));
    }
    UpdateSpatialSharpenUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnTemporalSmoothToggled(bool checked) {
    temporalSmoothEnabled_ = checked;
    UpdateControlEnabledStates();
    cpuPipeline_.ResetTemporalHistory();
    if (cudaSurface_) {
        cudaSurface_->ResetTemporalHistory();
    }
    UpdateTemporalSmoothUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnTemporalSmoothStrengthChanged(int value) {
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
                    QString::number(temporalSmoothAlpha_, 'f', 2),
                    LivePoliteness::kSilent,
                    QStringLiteral("Temporal blend"));
    }
    cpuPipeline_.ResetTemporalHistory();
    if (cudaSurface_) {
        cudaSurface_->ResetTemporalHistory();
    }
    UpdateTemporalSmoothUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnStabilizationToggled(bool checked) {
    stabilizationEnabled_ = checked;
    if (!checked) {
        bumpHoldEnabled_ = false;
        if (mainWindow_ && mainWindow_->bumpHoldCheckbox()) {
            const QSignalBlocker blocker(mainWindow_->bumpHoldCheckbox());
            mainWindow_->bumpHoldCheckbox()->setChecked(false);
        }
    }
    UpdateControlEnabledStates();
    if (cudaSurface_) {
        cudaSurface_->ResetStabilization();
        cudaSurface_->ResetKeystone();
        cudaSurface_->ResetTextClarityHistory();
    }
    UpdateKeystoneTrackingUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnBumpHoldToggled(bool checked) {
    bumpHoldEnabled_ =
        checked && stabilizationEnabled_;
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
}
void OpenZoomApp::OnKeystoneToggled(bool checked) {
    keystoneEnabled_ = checked;
    UpdateControlEnabledStates();
    if (cudaSurface_) {
        cudaSurface_->ResetKeystone();
        cudaSurface_->ResetTextClarityHistory();
    }
    UpdateKeystoneTrackingUi();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::OpenSetupAssistant()
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
        settingsController_->MutableSettings().assistive.tesseractPath,
        settingsController_->MutableSettings().assistive.codexExecutablePath,
        settingsController_->MutableSettings().setupAssistantDeclined,
        mainWindow_.get());
    connect(setupAssistantDialog_, &QObject::destroyed, this, [this]() {
        setupAssistantDialog_ = nullptr;
    });
    connect(setupAssistantDialog_, &SetupAssistantDialog::TesseractPathChanged,
            this, [this](const QString& path) {
                settingsController_->MutableSettings().assistive.tesseractPath = path;
                assistiveManager_->ApplySettings(
                    settingsController_->MutableSettings().assistive);
                SavePersistentSettings();
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
                if (cudaSurface_) {
                    cudaSurface_->ResetSuperRes();
                }
                mainWindow_->setMaxineRuntimeInstalled(MaxineSuperRes::IsRuntimeInstalled());
                assistiveManager_->ApplySettings(
                    settingsController_->MutableSettings().assistive);
                UpdateProcessingStatusLabel();
            });
    setupAssistantDialog_->show();
}

void OpenZoomApp::OpenUserDataFolder()
{
    if (!mainWindow_ || !userDataPaths_) {
        return;
    }
    QString error;
    const QString root = userDataPaths_->Root(&error);
    if (root.isEmpty()) {
        QMessageBox::warning(mainWindow_.get(),
                             QStringLiteral("Open OpenZoom Folder"),
                             error.isEmpty()
                                 ? QStringLiteral(
                                       "The OpenZoom folder is unavailable.")
                                 : error);
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(root))) {
        QMessageBox::warning(
            mainWindow_.get(),
            QStringLiteral("Open OpenZoom Folder"),
            QStringLiteral("Windows could not open:\n%1")
                .arg(QDir::toNativeSeparators(root)));
        return;
    }
    const QString message =
        QStringLiteral("Opened your OpenZoom folder.");
    ShowStatusMessage(message, 3500);
}

void OpenZoomApp::ChangeUserDataFolder()
{
    if (!mainWindow_ || !userDataPaths_ || !settingsController_) {
        return;
    }
    QString currentError;
    const QString currentRoot = userDataPaths_->Root(&currentError);
    const QString selected = QFileDialog::getExistingDirectory(
        mainWindow_.get(),
        QStringLiteral("Choose OpenZoom Folder"),
        currentRoot,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selected.isEmpty()) {
        return;
    }

    QString error;
    if (!userDataPaths_->SetConfiguredRoot(selected, &error)) {
        QMessageBox::warning(
            mainWindow_.get(),
            QStringLiteral("Choose OpenZoom Folder"),
            error.isEmpty()
                ? QStringLiteral("OpenZoom cannot use the selected folder.")
                : error);
        return;
    }

    settingsController_->MutableSettings().userDataRoot =
        userDataPaths_->ConfiguredRoot();
    assistiveManager_->ApplySettings(
        settingsController_->MutableSettings().assistive);
    SavePersistentSettings();

    const QString message =
        QStringLiteral("New OpenZoom files will be saved in %1.")
            .arg(QDir::toNativeSeparators(userDataPaths_->Root()));
    ShowStatusMessage(message, 7000);
}

void OpenZoomApp::OnKeystoneStepBack() {
    if (!keystoneEnabled_ || !cudaSurface_) {
        return;
    }
    if (cudaSurface_->StepKeystoneCorrection(-1)) {
        cudaSurface_->ResetTextClarityHistory();
        ShowStatusMessage(QStringLiteral("Screen correction moved back and tracking stopped."), 3500);
    }
    UpdateKeystoneTrackingUi();
}
void OpenZoomApp::OnKeystonePauseResume() {
    if (!keystoneEnabled_ || !cudaSurface_) {
        return;
    }
    const bool pause = !cudaSurface_->GetKeystoneTrackingState().paused;
    cudaSurface_->SetKeystoneTrackingPaused(pause);
    ShowStatusMessage(pause ? QStringLiteral("Automatic screen correction stopped.")
                            : QStringLiteral("Automatic screen correction continuing."),
                      3500);
    UpdateKeystoneTrackingUi();
}
void OpenZoomApp::OnKeystoneStepForward() {
    if (!keystoneEnabled_ || !cudaSurface_) {
        return;
    }
    if (cudaSurface_->StepKeystoneCorrection(1)) {
        cudaSurface_->ResetTextClarityHistory();
        const bool pending = cudaSurface_->GetKeystoneTrackingState().stepPending;
        ShowStatusMessage(pending ? QStringLiteral("Finding one new screen correction...")
                                  : QStringLiteral("Using the next screen correction."),
                          3500);
    }
    UpdateKeystoneTrackingUi();
}
void OpenZoomApp::UpdateKeystoneTrackingUi() {
    if (!mainWindow_) {
        return;
    }
    const bool available = cameraActive_ && cudaSurface_ && cudaSurface_->IsValid() &&
                           cudaPipelineAvailable_;
    const KeystoneTrackingState state = cudaSurface_
                                            ? cudaSurface_->GetKeystoneTrackingState()
                                            : KeystoneTrackingState{};
    mainWindow_->setKeystoneTrackingControls(keystoneEnabled_, available,
                                             state.paused, state.canStepBack,
                                             state.canStepForward, state.stepPending,
                                             state.position, state.count);
}
void OpenZoomApp::OnAutoContrastToggled(bool checked) {
    autoContrastEnabled_ = checked;
    UpdateControlEnabledStates();
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnAutoContrastStrengthChanged(int value) {
    autoContrastStrength_ = std::clamp(static_cast<float>(value) / 100.0f, 0.0f, 1.0f);
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnTextClarityControlsChanged() {
    const bool wasSuperResEnabled = mlTextSuperResolutionEnabled_;
    autoTextClarityEnabled_ = uiState_->textClarityCheckbox_ && uiState_->textClarityCheckbox_->isChecked();
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
#if OPENZOOM_ENABLE_TEXT_SR
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

    UpdateControlEnabledStates();
    if (cudaSurface_) {
        cudaSurface_->ResetTextClarityHistory();
        // SuperRes strength/mode are load-time SDK parameters. Recreate the
        // effect under the surface's stream-synchronization discipline after
        // profile or Advanced Text Clarity changes.
        cudaSurface_->ResetSuperRes();
        cudaSurface_->SetSuperResPerformanceOverride(superResPerformanceOverride_);
    }
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::UpdateControlEnabledStates() {
    const auto setEnabled = [](QWidget* widget, bool enabled) {
        if (widget) {
            widget->setEnabled(enabled);
        }
    };

    setEnabled(uiState_->bwSlider_, blackWhiteEnabled_);
    setEnabled(uiState_->zoomSlider_, zoomEnabled_);
    setEnabled(uiState_->blurSigmaSlider_, blurEnabled_);
    setEnabled(uiState_->blurRadiusSlider_, blurEnabled_);
    setEnabled(uiState_->temporalSmoothSlider_, temporalSmoothEnabled_);
    setEnabled(uiState_->autoContrastStrengthSlider_, autoContrastEnabled_);
    setEnabled(uiState_->focusMarkerCheckbox_, !debugViewEnabled_);
    setEnabled(mainWindow_ ? mainWindow_->bumpHoldCheckbox() : nullptr,
               stabilizationEnabled_);

    setEnabled(uiState_->backgroundFlattenStrengthSlider_,
               backgroundFlattenEnabled_ || autoTextClarityEnabled_);
    setEnabled(uiState_->sauvolaStrengthSlider_,
               adaptiveBinarizationEnabled_ || autoTextClarityEnabled_);
    setEnabled(uiState_->binarizationSoftnessSlider_,
               adaptiveBinarizationEnabled_ || autoTextClarityEnabled_);
    setEnabled(uiState_->smartSharpenStrengthSlider_,
               smartSharpenEnabled_ || autoTextClarityEnabled_);
    setEnabled(uiState_->claheClipLimitSlider_, claheEnabled_);
    setEnabled(uiState_->textHysteresisStrengthSlider_,
               textHysteresisEnabled_ || autoTextClarityEnabled_);
    setEnabled(uiState_->focusThresholdSlider_,
               focusDetectionEnabled_ || autoTextClarityEnabled_);
    setEnabled(uiState_->glareSuppressionStrengthSlider_,
               glareSuppressionEnabled_ || autoTextClarityEnabled_);

    const bool superResAvailable =
        mlTextSuperResolutionEnabled_ && mainWindow_ &&
        mainWindow_->isMaxineRuntimeInstalled();
    setEnabled(uiState_->mlTextSuperResolutionStrengthSlider_,
               superResAvailable);
    setEnabled(uiState_->mlTextSuperResolutionPrefer2xCheckbox_,
               superResAvailable);
    setEnabled(uiState_->mlTextSuperResolutionUltra1440pCheckbox_,
               superResAvailable);
    UpdateSectionChangedCounts();
}

void OpenZoomApp::UpdateSectionChangedCounts() {
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

void OpenZoomApp::SetSuperResPerformanceOverride(bool enabled) {
#if OPENZOOM_ENABLE_TEXT_SR
    if (!mlTextSuperResolutionEnabled_ || !cudaSurface_) {
        return;
    }
    superResPerformanceOverride_ = enabled;
    cudaSurface_->SetSuperResPerformanceOverride(enabled);
    ShowStatusMessage(enabled
                          ? QStringLiteral("NVIDIA Super Resolution performance limit ignored.")
                          : QStringLiteral("NVIDIA Super Resolution performance limit restored."));
#else
    Q_UNUSED(enabled);
#endif
}
void OpenZoomApp::OnDisplayColorSchemeChanged() {
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
void OpenZoomApp::OnContrastChanged(int value) {
    contrast_ = std::clamp(static_cast<float>(value) / 100.0f, 0.25f, 4.0f);
    SyncCurrentConfigToPersistence();
}
void OpenZoomApp::OnBrightnessChanged(int value) {
    brightness_ = std::clamp(static_cast<float>(value) / 100.0f, -1.0f, 1.0f);
    SyncCurrentConfigToPersistence();
}

} // namespace openzoom

#endif // _WIN32
