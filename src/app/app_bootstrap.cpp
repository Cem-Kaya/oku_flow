#ifdef _WIN32

#include "app_internal.hpp"
#include "debug_log.hpp"

#include <QSaveFile>
#include <QThreadPool>

namespace openzoom {

OpenZoomApp::OpenZoomApp(int& argc, char** argv)
    : QObject(nullptr) {
    startupTimer_.start();
    startupWorkers_ = std::make_shared<StartupWorkerTracker>();
    microphoneCallbackTarget_ =
        std::make_shared<MicrophoneCallbackTarget>();
    microphoneCallbackTarget_->app = this;
    debug_log::InstallIfConsoleAttached();
    qtApp_ = new QApplication(argc, argv);
    imageIoPool_ = std::make_unique<QThreadPool>();
    imageIoPool_->setMaxThreadCount(2);
    imageIoPool_->setExpiryTimeout(30000);
    QCoreApplication::setOrganizationName(QStringLiteral("OpenZoom"));
    QCoreApplication::setApplicationName(QStringLiteral("OpenZoom"));
    ConfigureStartupProfiling();
}

bool OpenZoomApp::Initialize()
{
    if (initialized_) {
        return true;
    }

    const HRESULT appIdResult =
        SetCurrentProcessExplicitAppUserModelID(L"OpenZoom.OpenZoom");
    if (FAILED(appIdResult)) {
        qWarning() << "Could not set Windows AppUserModelID"
                   << QStringLiteral("0x%1").arg(static_cast<qulonglong>(appIdResult), 0, 16);
    }
    const QIcon applicationIcon(QStringLiteral(":/openzoom/icons/app.png"));
    qtApp_->setWindowIcon(applicationIcon);
    ResolveCudaBufferFormatFromOptions();
    InitializePlatform();

    presenter_ = std::make_unique<D3D12Presenter>();
    // Windows known-folder lookup ignores a child APPDATA override. Profiling
    // therefore uses an explicit sibling settings file for both load and save.
    settingsController_ = std::make_unique<SettingsController>(
        startupProfilePath_.isEmpty()
            ? QString()
            : QFileInfo(startupProfilePath_).dir().filePath(QStringLiteral("settings.json")));
    languageManager_ =
        std::make_unique<LanguageManager>(*qtApp_, this);
    auto& persistentSettings = settingsController_->MutableSettings();
    const AppLanguage initialLanguage =
        persistentSettings.language.trimmed().isEmpty()
            ? AppLanguageFromSystemLocale()
            : AppLanguageFromCode(persistentSettings.language);
    if (!languageManager_->SetLanguage(initialLanguage, false)) {
        languageManager_->SetLanguage(AppLanguage::English, false);
    }
    persistentSettings.language = languageManager_->languageCode();
    // Revalidate the persisted root on every startup, not only when the user
    // picks a folder: a saved directory can later be replaced with a
    // junction into the install directory, lose its drive, or lose write
    // access. On failure the session falls back to the default Documents
    // folder and says so, keeping the saved setting for the user to fix.
    QString userDataRootNotice;
    const QString persistedUserDataRoot =
        settingsController_->Settings().userDataRoot.trimmed();
    if (!persistedUserDataRoot.isEmpty()) {
        const UserDataValidationResult persistedRootCheck =
            UserDataPaths::ValidateRoot(
                persistedUserDataRoot,
                QCoreApplication::applicationDirPath());
        if (!persistedRootCheck.ok) {
            userDataRootNotice =
                QStringLiteral(
                    "The saved OpenZoom folder cannot be used (%1). Files "
                    "will go to the default Documents folder until a new "
                    "folder is chosen.")
                    .arg(persistedRootCheck.error);
        }
    }
    userDataPaths_ = std::make_unique<UserDataPaths>(
        userDataRootNotice.isEmpty()
            ? settingsController_->Settings().userDataRoot
            : QString(),
        QCoreApplication::applicationDirPath());
    const PhotoPairRecoveryResult photoRecovery =
        userDataPaths_->RecoverInterruptedPhotoPairs(
            // Current builds protect active writers with a per-pair lock, so
            // unmarked leftovers from older builds are safe to reconcile on
            // the first startup too.
            QDateTime::currentDateTime().addSecs(1));
    if (photoRecovery.completedPairs > 0 ||
        photoRecovery.removedFiles > 0) {
        qInfo() << "Recovered interrupted photo transactions:"
                << photoRecovery.completedPairs << "pair(s) completed,"
                << photoRecovery.removedFiles << "file(s) rolled back.";
    }
    for (const QString& path : photoRecovery.unresolvedPaths) {
        qWarning() << "Could not reconcile interrupted photo transaction:"
                   << path;
    }
    const QString photoRecoveryNotice =
        photoRecovery.unresolvedPaths.isEmpty()
            ? QString()
            : TranslateUi(QStringLiteral(
                  "The paired photos could not be saved. A partial file may "
                  "remain: %1"))
                  .arg(photoRecovery.unresolvedPaths.join(
                      QStringLiteral(", ")));
    if (debug_log::IsEnabled()) {
        QString debugDirectoryError;
        const QString debugDirectory =
            userDataPaths_->Debug(&debugDirectoryError);
        QString debugLogError;
        if (debugDirectory.isEmpty() ||
            !debug_log::SetOutputDirectory(debugDirectory, &debugLogError)) {
            qWarning().noquote()
                << (debugLogError.isEmpty()
                        ? debugDirectoryError
                        : debugLogError);
        }
    }

    mainWindow_ = std::make_unique<MainWindow>();
    mainWindow_->setWindowIcon(applicationIcon);
    mainWindow_->setApp(this);
    mainWindow_->setMaxineRuntimeInstalled(MaxineSuperRes::IsRuntimeInstalled());
    if (QComboBox* languageCombo =
            mainWindow_->applicationLanguageCombo()) {
        const QSignalBlocker blocker(languageCombo);
        const int index =
            languageCombo->findData(languageManager_->languageCode());
        languageCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    connect(mainWindow_.get(),
            &MainWindow::applicationLanguageRequested,
            this,
            [this](const QString& code) {
                if (!languageManager_ || !mainWindow_) {
                    return;
                }
                const AppLanguage requested =
                    AppLanguageFromCode(code);
                if (!languageManager_->SetLanguage(requested, false)) {
                    if (QComboBox* combo =
                            mainWindow_->applicationLanguageCombo()) {
                        const QSignalBlocker blocker(combo);
                        combo->setCurrentIndex(
                            combo->findData(languageManager_->languageCode()));
                    }
                    ShowStatusMessage(
                        TranslateUi(QStringLiteral(
                            "The selected language could not be loaded.")),
                        7000);
                    return;
                }

                settingsController_->MutableSettings().language =
                    languageManager_->languageCode();
                if (assistiveManager_) {
                    assistiveManager_->Runtime().SetResponseLanguage(
                        languageManager_->languageCode());
                }
                SavePersistentSettings();
                ShowStatusMessage(
                    TranslateUi(QStringLiteral("Language set to %1."))
                        .arg(AppLanguageNativeName(requested)),
                    5000);
            });
    uiState_ = std::make_unique<UIStateManager>(*mainWindow_, *this);
    uiState_->renderWidget_->setPresenter(presenter_.get());
    assistiveManager_ = std::make_unique<AssistiveFeatureManager>(
        *uiState_->renderWidget_,
        *this,
        [this](const QString& question) { SubmitFloatingAssistantPrompt(question); },
        [this]() { StartNewAssistantConversation(); },
        *userDataPaths_);
    mainWindow_->annotationOverlay()->SetExcludedWidget(&assistiveManager_->Overlay());
    assistiveManager_->Runtime().SetResponseLanguage(
        languageManager_->languageCode());
    connect(&assistiveManager_->Runtime(),
            &AssistiveRuntime::StatusNotice,
            this,
            [this](const QString& sourceText) {
                ShowStatusMessage(sourceText, 10000);
            });
    interactionController_ = std::make_unique<InteractionController>(*this);
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::NotesWriteFinished,
            this, [this](const QString& path, const QString& error) {
                if (!error.isEmpty()) {
                    transcriptionNoteWriteFailed_ = true;
                    transcriptionNotesCompletionPending_ = false;
                    openNotesWhenStored_ = false;
                    qWarning() << "Lecture notes storage failed:" << path << error;
                    ShowStatusMessage(QStringLiteral("Lecture notes could not be saved."), 9000);
                    return;
                }
                MaybeReportTranscriptNotesSaved();
                if (openNotesWhenStored_ && !assistiveManager_->Runtime().HasPendingNotesWrites()) {
                    openNotesWhenStored_ = false;
                    OpenNotesFile();
                }
            });
    pipelineOrchestrator_ = std::make_unique<PipelineOrchestrator>(
        *this,
        PipelineOrchestrator::Callbacks{
            .tick = [this](double elapsedSeconds) {
                return RunFrameTick(elapsedSeconds);
            },
            .hasContinuousMotion = [this]() {
                return interactionController_ &&
                       interactionController_->HasContinuousMotion();
            },
            .isMousePanActive = [this]() { return IsMousePanActive(); },
            .needsScenePresent = [this]() {
                return presenter_ && presenter_->NeedsScenePresent();
            },
            .cameraActive = [this]() { return cameraActive_; },
            .cameraFrameRate = [this]() {
                return mediaCapture_.CurrentFrameRate();
            },
            .usingCuda = [this]() { return usingCudaLastFrame_; },
            .queryDisplayRefreshRate = [this]() {
                return uiState_ && uiState_->renderWidget_
                           ? QueryDisplayRefreshHz(reinterpret_cast<HWND>(
                                 uiState_->renderWidget_->winId()))
                           : 60;
            },
            .viewportRateClamped = [this](int, int effectiveRate) {
                ShowStatusMessage(
                    QStringLiteral(
                        "Viewport motion limited to %1 FPS by this display.")
                        .arg(effectiveRate),
                    7000);
            },
        });
    // Fence state belongs to the orchestrator, so seed it only after the
    // manager exists; the presenter itself is intentionally created first.
    ResetCudaFenceState();
    // Startup is a cross-function latch: ApplyAdvancedConfig releases it
    // after the first complete UI/config synchronization.
    configTrackingSuspended_ = true;
    connect(qtApp_, &QCoreApplication::aboutToQuit, this, [this]() { SavePersistentSettings(); });
    recordingManager_ = std::make_unique<RecordingManager>(
        uiState_->recordButton_,
        [this](const QString& message, int durationMs) {
            ShowStatusMessage(message, durationMs);
        },
        [this](const SavedRecordingSegment& segment) {
            if (assistiveManager_) {
                assistiveManager_->Runtime().NoteCapturedVideoPair(
                    segment.originalPath, segment.processedPath);
            }
        },
        *userDataPaths_,
        [this](const RecordingSessionInfo& endedSession) {
            // Session-ended callbacks are queued from the recording worker
            // after the terminal state is published. If a new recording has
            // already started by the time this lands, releasing the
            // microphone now would silently strip audio from that new
            // session.
            if (transcriptionController_) {
                // Idempotent: recorder failure, watchdog, and normal stop all
                // funnel here; only the matching transcript session ends.
                transcriptionController_->FinishForSession(endedSession.id);
            }
            if (recordingManager_ && recordingManager_->IsActive()) {
                return;
            }
            StopMicrophoneCapture();
        });
    qInfo() << "Rotation combo ready with"
            << uiState_->rotationCombo_->count() << "items";

    joystickOverlay_ = new JoystickOverlay(uiState_->renderWidget_);
    connect(joystickOverlay_, &JoystickOverlay::JoystickChanged,
            this, [this](float x, float y) {
                if (interactionController_) {
                    interactionController_->SetJoystickAxes(x, y);
                }
            });
    UpdateJoystickVisibility();

    if (uiState_->zoomCenterXSlider_) {
        zoomCenterX_ = std::clamp(static_cast<float>(uiState_->zoomCenterXSlider_->value()) / static_cast<float>(kZoomFocusSliderScale), 0.0f, 1.0f);
    }
    if (uiState_->zoomCenterYSlider_) {
        zoomCenterY_ = std::clamp(static_cast<float>(uiState_->zoomCenterYSlider_->value()) / static_cast<float>(kZoomFocusSliderScale), 0.0f, 1.0f);
    }

    SetZoomCenter(zoomCenterX_, zoomCenterY_, true);

    if (uiState_->blurSigmaSlider_) {
        blurSigma_ = SliderValueToSigma(uiState_->blurSigmaSlider_->value());
    }
    if (uiState_->blurRadiusSlider_) {
        const int sliderValue = uiState_->blurRadiusSlider_->value();
        const int snapped = SnapBlurRadius(sliderValue);
        blurRadius_ = snapped;
        if (sliderValue != snapped) {
            auto block = uiState_->BlockSignals(uiState_->blurRadiusSlider_);
            uiState_->blurRadiusSlider_->setValue(snapped);
        }
    }
    if (uiState_->blurRadiusValueLabel_) {
        uiState_->blurRadiusValueLabel_->setText(QString::number(blurRadius_));
    }
    blurEnabled_ = uiState_->blurCheckbox_ ? uiState_->blurCheckbox_->isChecked() : false;
    temporalSmoothEnabled_ = uiState_->temporalSmoothCheckbox_ ? uiState_->temporalSmoothCheckbox_->isChecked() : false;
    if (uiState_->temporalSmoothSlider_) {
        temporalSmoothAlpha_ = std::clamp(static_cast<float>(uiState_->temporalSmoothSlider_->value()) / 100.0f, 0.0f, 1.0f);
    }
    spatialSharpenEnabled_ = uiState_->spatialSharpenCheckbox_ ? uiState_->spatialSharpenCheckbox_->isChecked() : false;
    if (uiState_->spatialBackendCombo_) {
        auto block = uiState_->BlockSignals(uiState_->spatialBackendCombo_);
        uiState_->spatialBackendCombo_->setCurrentIndex(static_cast<int>(SpatialUpscaler::kNis));
    }
    spatialUpscaler_ = SpatialUpscaler::kNis;
    if (uiState_->spatialSharpnessSlider_) {
        spatialSharpness_ = std::clamp(static_cast<float>(uiState_->spatialSharpnessSlider_->value()) / 100.0f, 0.0f, 1.0f);
    }
    if (uiState_->focusMarkerCheckbox_) {
        focusMarkerEnabled_ = uiState_->focusMarkerCheckbox_->isChecked();
    }
    stabilizationEnabled_ = uiState_->stabilizationCheckbox_ ? uiState_->stabilizationCheckbox_->isChecked() : false;
    keystoneEnabled_ = uiState_->keystoneCheckbox_ ? uiState_->keystoneCheckbox_->isChecked() : false;
    autoContrastEnabled_ = uiState_->autoContrastCheckbox_ ? uiState_->autoContrastCheckbox_->isChecked() : false;
    if (uiState_->autoContrastStrengthSlider_) {
        autoContrastStrength_ = std::clamp(static_cast<float>(uiState_->autoContrastStrengthSlider_->value()) / 100.0f, 0.0f, 1.0f);
    }
    if (uiState_->displayColorPicker_) {
        displayColorScheme_ = uiState_->displayColorPicker_->currentScheme();
        displayColorMode_ = std::max(0, displayColorScheme_.legacyMode);
        displayColorLut_ = color_schemes::BuildColorLut(displayColorScheme_);
    }
    if (uiState_->contrastSlider_) {
        contrast_ = std::clamp(static_cast<float>(uiState_->contrastSlider_->value()) / 100.0f, 0.25f, 4.0f);
    }
    if (uiState_->brightnessSlider_) {
        brightness_ = std::clamp(static_cast<float>(uiState_->brightnessSlider_->value()) / 100.0f, -1.0f, 1.0f);
    }

    PopulateCameraCombo();
    PopulateMicrophoneCombo();
    PopulatePresetList();

    connect(uiState_->cameraCombo_, &QComboBox::currentIndexChanged,
            this, &OpenZoomApp::OnCameraSelectionChanged);
    connect(uiState_->microphoneCombo_, &QComboBox::currentIndexChanged,
            this, &OpenZoomApp::OnMicrophoneSelectionChanged);
    if (uiState_->presetList_) {
        connect(uiState_->presetList_, &QListWidget::currentItemChanged,
                this, &OpenZoomApp::OnPresetSelectionChanged);
    }
    if (uiState_->promotePresetButton_) {
        connect(uiState_->promotePresetButton_, &QPushButton::clicked,
                this, [this]() { PromoteCurrentConfigToPreset(); });
    }
    connect(mainWindow_.get(), &MainWindow::resetCurrentProfileRequested,
            this, [this]() { ResetCurrentConfigToDefaults(); });
    connect(uiState_->bwCheckbox_, &QCheckBox::toggled,
            this, &OpenZoomApp::OnBlackWhiteToggled);
    connect(uiState_->bwSlider_, &QSlider::valueChanged,
            this, &OpenZoomApp::OnBlackWhiteThresholdChanged);
    connect(uiState_->zoomCheckbox_, &QCheckBox::toggled,
            this, &OpenZoomApp::OnZoomToggled);
    connect(uiState_->zoomSlider_, &QSlider::valueChanged,
            this, &OpenZoomApp::OnZoomAmountChanged);
    if (uiState_->debugButton_) {
        connect(uiState_->debugButton_, &QPushButton::toggled,
                this, &OpenZoomApp::OnDebugViewToggled);
    }
    if (uiState_->capturePhotoButton_) {
        connect(uiState_->capturePhotoButton_, &QPushButton::clicked, this, [this]() {
            photoCapturePending_ = true;
            ShowStatusMessage(QStringLiteral("Capturing original and processed photos..."), 2500);
        });
    }
    connect(mainWindow_.get(), &MainWindow::annotationSnapshotRequested,
            this, &OpenZoomApp::QueueAnnotationSnapshot);
    connect(mainWindow_.get(), &MainWindow::annotationPreferencesChanged,
            this,
            [this](const QString& colorName,
                   int widthPixels,
                   bool captureOnExit,
                   bool dashed,
                   const QString& shapeKind,
                   int textSizePixels) {
                auto& settings = settingsController_->MutableSettings();
                settings.annotationColor = colorName;
                settings.annotationWidthPixels = widthPixels;
                settings.annotationCaptureOnExit = captureOnExit;
                settings.annotationDashed = dashed;
                settings.annotationShapeKind = shapeKind;
                settings.annotationTextSizePixels = textSizePixels;
                SavePersistentSettings();
            });
    if (uiState_->recordButton_) {
        uiState_->recordButton_->setCheckable(true);
        connect(uiState_->recordButton_, &QPushButton::toggled, this, [this](bool checked) {
            if (recordingManager_) {
                if (checked) {
                    StartSelectedMicrophone();
                } else {
                    // Transcript input ends first, then PCM production. The
                    // transcript finalizer is bounded and asynchronous; it
                    // never delays microphone release or recorder stop.
                    if (transcriptionController_) {
                        transcriptionController_->FinishInput();
                    }
                    // Stop producing PCM first. RecordingManager::Stop
                    // deliberately clears its queued audio rather than
                    // draining a tail — finishing the sample the worker
                    // already owns is what keeps Stop fast — so retaining
                    // blocks here only avoids tearing down the device while
                    // the recorder may still reference the current buffer.
                    StopMicrophoneCapture(true);
                }
                recordingManager_->SetRequested(checked);
                if (checked && !recordingManager_->IsActive()) {
                    // Recording preflight can reject the session after the
                    // microphone has opened (for example, on low disk space).
                    // Do not retain exclusive access to the device when no
                    // recording session exists.
                    StopMicrophoneCapture();
                    if (transcriptionController_) {
                        transcriptionController_->Cancel();
                    }
                } else if (checked) {
                    StartTranscriptionForActiveRecording();
                }
            }
        });
    }
    if (uiState_->rotationCombo_) {
        connect(uiState_->rotationCombo_, &QComboBox::currentIndexChanged,
                this, &OpenZoomApp::OnRotationSelectionChanged);
    }
    if (uiState_->cameraFormatCombo_) {
        connect(uiState_->cameraFormatCombo_, &QComboBox::currentIndexChanged,
                this, &OpenZoomApp::OnCameraFormatChanged);
    }
    if (uiState_->cameraAccelerationCombo_) {
        connect(
            uiState_->cameraAccelerationCombo_,
            &QComboBox::currentIndexChanged,
            this,
            &OpenZoomApp::OnCameraAccelerationModeChanged);
    }
    if (uiState_->testCameraAccelerationButton_) {
        connect(
            uiState_->testCameraAccelerationButton_,
            &QPushButton::clicked,
            this,
            &OpenZoomApp::OnTestCameraAcceleration);
    }
    connect(uiState_->viewportRateCombo_, &QComboBox::currentIndexChanged,
            this, [this](int index) {
                const auto mode = static_cast<settings::ViewportRateMode>(
                    std::clamp(index, 0, 4));
                settingsController_->MutableSettings().viewportRateMode =
                    mode;
                pipelineOrchestrator_->SetViewportRateMode(mode);
                UpdateSectionChangedCounts();
                SavePersistentSettings();
            });
    connect(uiState_->viewportFitCombo_, &QComboBox::currentIndexChanged,
            this, [this](int index) {
                const auto mode =
                    index == 1 ? settings::ViewportFitModeSetting::Fit
                               : settings::ViewportFitModeSetting::Fill;
                settingsController_->MutableSettings().viewportFitMode =
                    mode;
                pipelineOrchestrator_->SetViewportFitMode(mode);
                UpdateSectionChangedCounts();
                SavePersistentSettings();
            });
    connect(uiState_->recordingCanvasCombo_, &QComboBox::currentIndexChanged,
            this, [this](int index) {
                const int storedValue =
                    uiState_->recordingCanvasCombo_->itemData(index).toInt();
                const int bounded = std::clamp(
                    storedValue,
                    static_cast<int>(RecordingCanvasMode::Source),
                    static_cast<int>(RecordingCanvasMode::Nhd360));
                const auto mode =
                    static_cast<RecordingCanvasMode>(bounded);
                settingsController_->MutableSettings().recordingCanvasMode =
                    mode;
                if (recordingManager_) {
                    recordingManager_->SetCanvasMode(mode);
                }
                UpdateSectionChangedCounts();
                SavePersistentSettings();
            });
    if (uiState_->transcribeMicrophoneCheckbox_) {
        connect(uiState_->transcribeMicrophoneCheckbox_, &QCheckBox::toggled,
                this, [this](bool checked) {
                    settingsController_->MutableSettings()
                        .liveTranscriptionEnabled = checked;
                    if (!checked && transcriptionController_) {
                        // Ending the opt-in mid-session ends the transcript
                        // immediately; recording continues untouched.
                        transcriptionController_->Cancel();
                        transcriptionController_->SetEnabled(false);
                    }
                    SavePersistentSettings();
                });
    }
    if (uiState_->transcriptToNotesCheckbox_) {
        connect(uiState_->transcriptToNotesCheckbox_, &QCheckBox::toggled,
                this, [this](bool checked) {
                    settingsController_->MutableSettings()
                        .appendTranscriptToNotes = checked;
                    SavePersistentSettings();
                });
    }
    if (uiState_->zoomCenterXSlider_) {
        connect(uiState_->zoomCenterXSlider_, &QSlider::valueChanged,
                this, &OpenZoomApp::OnZoomCenterXChanged);
    }
    if (uiState_->zoomCenterYSlider_) {
        connect(uiState_->zoomCenterYSlider_, &QSlider::valueChanged,
                this, &OpenZoomApp::OnZoomCenterYChanged);
    }
    if (uiState_->collapseButton_) {
        connect(uiState_->collapseButton_, &QToolButton::toggled,
                this, &OpenZoomApp::OnControlsCollapsedToggled);
        OnControlsCollapsedToggled(uiState_->collapseButton_->isChecked());
    }
    if (uiState_->joystickCheckbox_) {
        uiState_->joystickCheckbox_->setChecked(false);
        connect(uiState_->joystickCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnVirtualJoystickToggled);
    }
    if (uiState_->zoomWheelAccelerationCheckbox_) {
        connect(uiState_->zoomWheelAccelerationCheckbox_,
                &QCheckBox::toggled,
                this,
                [this](bool enabled) {
                    settingsController_->MutableSettings()
                        .zoomWheelAcceleration = enabled;
                    UpdateSectionChangedCounts();
                    SavePersistentSettings();
                });
    }
    if (mainWindow_) {
        connect(mainWindow_.get(),
                &MainWindow::sectionStatesChanged,
                this,
                &OpenZoomApp::SavePersistentSettings);
    }
    if (uiState_->blurCheckbox_) {
        auto block = uiState_->BlockSignals(uiState_->blurCheckbox_);
        uiState_->blurCheckbox_->setChecked(blurEnabled_);
        connect(uiState_->blurCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnBlurToggled);
    }
    if (uiState_->blurSigmaSlider_) {
        connect(uiState_->blurSigmaSlider_, &QSlider::valueChanged,
                this, &OpenZoomApp::OnBlurSigmaChanged);
    }
    if (uiState_->blurRadiusSlider_) {
        connect(uiState_->blurRadiusSlider_, &QSlider::valueChanged,
                this, &OpenZoomApp::OnBlurRadiusChanged);
    }
    if (uiState_->temporalSmoothCheckbox_) {
        connect(uiState_->temporalSmoothCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnTemporalSmoothToggled);
    }
    if (uiState_->temporalSmoothSlider_) {
        connect(uiState_->temporalSmoothSlider_, &QSlider::valueChanged,
                this, &OpenZoomApp::OnTemporalSmoothStrengthChanged);
    }
    if (uiState_->vlmAssistCheckbox_) {
        connect(uiState_->vlmAssistCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnVlmAssistToggled);
    }
    if (uiState_->assistiveOverlayCheckbox_) {
        connect(uiState_->assistiveOverlayCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnAssistiveOverlayToggled);
    }
    if (uiState_->spatialSharpenCheckbox_) {
        connect(uiState_->spatialSharpenCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnSpatialSharpenToggled);
    }
    if (uiState_->spatialBackendCombo_) {
        connect(uiState_->spatialBackendCombo_, &QComboBox::currentIndexChanged,
                this, &OpenZoomApp::OnSpatialUpscalerChanged);
    }
    if (uiState_->spatialSharpnessSlider_) {
        connect(uiState_->spatialSharpnessSlider_, &QSlider::valueChanged,
                this, &OpenZoomApp::OnSpatialSharpnessChanged);
    }
    if (uiState_->focusMarkerCheckbox_) {
        connect(uiState_->focusMarkerCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnFocusMarkerToggled);
    }
    if (uiState_->stabilizationCheckbox_) {
        connect(uiState_->stabilizationCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnStabilizationToggled);
    }
    if (mainWindow_->bumpHoldCheckbox()) {
        connect(mainWindow_->bumpHoldCheckbox(), &QCheckBox::toggled,
                this, &OpenZoomApp::OnBumpHoldToggled);
    }
    if (uiState_->keystoneCheckbox_) {
        connect(uiState_->keystoneCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnKeystoneToggled);
    }
    connect(mainWindow_.get(), &MainWindow::keystoneStepBackRequested,
            this, &OpenZoomApp::OnKeystoneStepBack);
    connect(mainWindow_.get(), &MainWindow::keystonePauseResumeRequested,
            this, &OpenZoomApp::OnKeystonePauseResume);
    connect(mainWindow_.get(), &MainWindow::keystoneStepForwardRequested,
            this, &OpenZoomApp::OnKeystoneStepForward);
    connect(mainWindow_.get(), &MainWindow::superResPerformanceOverrideChanged,
            this, &OpenZoomApp::SetSuperResPerformanceOverride);
    if (uiState_->autoContrastCheckbox_) {
        connect(uiState_->autoContrastCheckbox_, &QCheckBox::toggled,
                this, &OpenZoomApp::OnAutoContrastToggled);
    }
    if (uiState_->autoContrastStrengthSlider_) {
        connect(uiState_->autoContrastStrengthSlider_, &QSlider::valueChanged,
                this, &OpenZoomApp::OnAutoContrastStrengthChanged);
    }
    if (uiState_->simpleTextClarityCheckbox_) {
        connect(uiState_->simpleTextClarityCheckbox_, &QCheckBox::toggled, this, [this](bool checked) {
            if (uiState_->textClarityCheckbox_) {
                auto block = uiState_->BlockSignals(uiState_->textClarityCheckbox_);
                uiState_->textClarityCheckbox_->setChecked(checked);
            }
            OnTextClarityControlsChanged();
        });
    }
    if (uiState_->textClarityCheckbox_) {
        connect(uiState_->textClarityCheckbox_, &QCheckBox::toggled, this, [this](bool checked) {
            if (uiState_->simpleTextClarityCheckbox_) {
                auto block = uiState_->BlockSignals(uiState_->simpleTextClarityCheckbox_);
                uiState_->simpleTextClarityCheckbox_->setChecked(checked);
            }
            OnTextClarityControlsChanged();
        });
    }
    auto connectTextCheck = [this](QCheckBox* checkbox) {
        if (checkbox) connect(checkbox, &QCheckBox::toggled,
                              this, [this]() { OnTextClarityControlsChanged(); });
    };
    auto connectTextSlider = [this](QSlider* slider) {
        if (slider) connect(slider, &QSlider::valueChanged,
                           this, [this]() { OnTextClarityControlsChanged(); });
    };
    for (QCheckBox* checkbox : {uiState_->backgroundFlattenCheckbox_, uiState_->adaptiveBinarizationCheckbox_,
                                uiState_->smartSharpenCheckbox_, uiState_->claheCheckbox_, uiState_->twoColorTextCheckbox_,
                                uiState_->textHysteresisCheckbox_, uiState_->selectiveSharpenCheckbox_,
                                uiState_->focusDetectionCheckbox_, uiState_->glareSuppressionCheckbox_}) {
        connectTextCheck(checkbox);
    }
    if (uiState_->mlTextSuperResolutionCheckbox_) {
        connect(uiState_->mlTextSuperResolutionCheckbox_, &QCheckBox::toggled,
                this, [this]() { OnTextClarityControlsChanged(); });
    }
    if (uiState_->mlTextSuperResolutionUltra1440pCheckbox_) {
        connect(uiState_->mlTextSuperResolutionUltra1440pCheckbox_,
                &QCheckBox::toggled,
                this,
                [this](bool checked) {
                    if (checked && uiState_->mlTextSuperResolutionPrefer2xCheckbox_) {
                        auto block = uiState_->BlockSignals(
                            uiState_->mlTextSuperResolutionPrefer2xCheckbox_);
                        uiState_->mlTextSuperResolutionPrefer2xCheckbox_->setChecked(false);
                    }
                    OnTextClarityControlsChanged();
                });
    }
    if (uiState_->mlTextSuperResolutionPrefer2xCheckbox_) {
        connect(uiState_->mlTextSuperResolutionPrefer2xCheckbox_,
                &QCheckBox::toggled,
                this,
                [this](bool checked) {
                    if (checked && uiState_->mlTextSuperResolutionUltra1440pCheckbox_) {
                        auto block = uiState_->BlockSignals(
                            uiState_->mlTextSuperResolutionUltra1440pCheckbox_);
                        uiState_->mlTextSuperResolutionUltra1440pCheckbox_->setChecked(false);
                    }
                    OnTextClarityControlsChanged();
                });
    }
    for (QSlider* slider : {uiState_->backgroundFlattenStrengthSlider_, uiState_->sauvolaStrengthSlider_,
                            uiState_->binarizationSoftnessSlider_, uiState_->strokeWeightSlider_,
                            uiState_->smartSharpenStrengthSlider_, uiState_->claheClipLimitSlider_,
                            uiState_->textHysteresisStrengthSlider_, uiState_->focusThresholdSlider_,
                            uiState_->glareSuppressionStrengthSlider_,
                            uiState_->mlTextSuperResolutionStrengthSlider_}) {
        connectTextSlider(slider);
    }
    if (uiState_->textPolarityCombo_) {
        connect(uiState_->textPolarityCombo_, &QComboBox::currentIndexChanged,
                this, [this]() { OnTextClarityControlsChanged(); });
    }
    if (uiState_->displayColorPicker_) {
        connect(uiState_->displayColorPicker_, &ColorSchemePicker::schemeChanged,
                this, &OpenZoomApp::OnDisplayColorSchemeChanged);
    }
    if (uiState_->contrastSlider_) {
        connect(uiState_->contrastSlider_, &QSlider::valueChanged,
                this, &OpenZoomApp::OnContrastChanged);
    }
    if (uiState_->brightnessSlider_) {
        connect(uiState_->brightnessSlider_, &QSlider::valueChanged,
                this, &OpenZoomApp::OnBrightnessChanged);
    }
    // Simple/Advanced mode buttons: the MainWindow wires the page switch
    // internally; here we only track the state for persistence and expand the
    // advanced tuning panel when the advanced page is entered.
    if (QAbstractButton* simpleButton = mainWindow_->simpleModeButton()) {
        connect(simpleButton, &QAbstractButton::toggled, this, [this](bool checked) {
            if (checked) {
                simpleUiMode_ = true;
                settingsController_->MutableSettings().simpleUiMode = true;
            }
        });
    }
    if (QAbstractButton* advancedButton = mainWindow_->advancedModeButton()) {
        connect(advancedButton, &QAbstractButton::toggled, this, [this](bool checked) {
            if (checked) {
                simpleUiMode_ = false;
                settingsController_->MutableSettings().simpleUiMode = false;
                if (uiState_->collapseButton_ && !uiState_->collapseButton_->isChecked()) {
                    uiState_->collapseButton_->setChecked(true);
                }
            }
        });
    }
    if (uiState_->explainNowButton_) {
        connect(uiState_->explainNowButton_, &QPushButton::clicked,
                this, [this]() {
                    if (assistiveManager_->Runtime().IsCodexTurnActive() ||
                        pendingAssistantFramePrompt_) {
                        StopAssistantRequest();
                    } else {
                        SubmitOnDemandAnalysis(false);
                    }
                });
    }
    if (uiState_->readTextButton_) {
        connect(uiState_->readTextButton_, &QPushButton::clicked,
                this, [this]() { SubmitOnDemandAnalysis(true); });
    }
    if (uiState_->aiSettingsButton_) {
        connect(uiState_->aiSettingsButton_, &QPushButton::clicked,
                this, [this]() { OpenAiSettingsDialog(); });
    }
    if (uiState_->openNotesButton_) {
        connect(uiState_->openNotesButton_, &QPushButton::clicked,
                this, [this]() { OpenNotesFile(); });
    }
    if (uiState_->setupAssistantButton_) {
        connect(uiState_->setupAssistantButton_, &QPushButton::clicked,
                this, [this]() { OpenSetupAssistant(); });
    }
    connect(mainWindow_.get(), &MainWindow::openUserDataFolderRequested,
            this, [this]() { OpenUserDataFolder(); });
    connect(mainWindow_.get(), &MainWindow::changeUserDataFolderRequested,
            this, [this]() { ChangeUserDataFolder(); });
    if (uiState_->assistantConnectButton_) {
        connect(uiState_->assistantConnectButton_, &QPushButton::clicked,
                this, [this]() { assistiveManager_->Runtime().StartCodexLogin(); });
    }
    if (uiState_->assistantSendButton_) {
        connect(uiState_->assistantSendButton_, &QPushButton::clicked,
                this, [this]() { SubmitAssistantPrompt(); });
    }
    if (uiState_->assistantStopButton_) {
        connect(uiState_->assistantStopButton_, &QPushButton::clicked,
                this, [this]() { StopAssistantRequest(); });
    }
    if (uiState_->assistantNewButton_) {
        connect(uiState_->assistantNewButton_, &QPushButton::clicked, this, [this]() {
            if (assistiveManager_->Runtime().IsCodexTurnActive()) {
                return;
            }
            currentAssistantThreadId_.clear();
            pendingAssistantPrompt_.clear();
            assistantResponseOpen_ = false;
            assistantResponseReceivedText_ = false;
            uiState_->assistantTranscript_->clear();
            uiState_->assistantHistoryList_->clearSelection();
            uiState_->assistantPromptEdit_->setFocus();
        });
    }
    if (uiState_->assistantHistoryList_) {
        connect(uiState_->assistantHistoryList_, &QListWidget::itemDoubleClicked,
                this, [this](QListWidgetItem*) { LoadSelectedAssistantConversation(); });
        connect(uiState_->assistantHistoryList_, &QListWidget::itemActivated,
                this, [this](QListWidgetItem*) { LoadSelectedAssistantConversation(); });
    }
    if (uiState_->assistantRenameButton_) {
        connect(uiState_->assistantRenameButton_, &QPushButton::clicked, this, [this]() {
            QListWidgetItem* item = uiState_->assistantHistoryList_->currentItem();
            if (!item) {
                return;
            }
            const QString threadId = item->data(Qt::UserRole).toString();
            const QString oldName = item->data(Qt::UserRole + 1).toString();
            bool accepted = false;
            const QString name = QInputDialog::getText(mainWindow_.get(),
                                                       QStringLiteral("Rename Conversation"),
                                                       QStringLiteral("Name:"),
                                                       QLineEdit::Normal,
                                                       oldName,
                                                       &accepted).trimmed();
            if (accepted && !name.isEmpty()) {
                assistiveManager_->Runtime().RenameAssistantConversation(threadId, name);
            }
        });
    }
    if (uiState_->assistantExportButton_) {
        connect(uiState_->assistantExportButton_, &QPushButton::clicked, this, [this]() {
            if (!uiState_->assistantTranscript_ || uiState_->assistantTranscript_->toPlainText().trimmed().isEmpty()) {
                return;
            }
            QString analysisError;
            const QString analysisDirectory =
                userDataPaths_->Analysis(&analysisError);
            if (analysisDirectory.isEmpty()) {
                QMessageBox::warning(
                    mainWindow_.get(),
                    QStringLiteral("Export Conversation"),
                    analysisError.isEmpty()
                        ? QStringLiteral(
                              "The OpenZoom Analysis folder is unavailable.")
                        : analysisError);
                return;
            }
            const QString suggested = QDir(analysisDirectory)
                                          .filePath(QStringLiteral("OpenZoom_Assistant_%1.txt")
                                                        .arg(QDateTime::currentDateTime().toString(
                                                            QStringLiteral("yyyyMMdd_HHmmss"))));
            const QString path = QFileDialog::getSaveFileName(mainWindow_.get(),
                                                               QStringLiteral("Export Conversation"),
                                                               suggested,
                                                               QStringLiteral("Text Files (*.txt);;All Files (*)"));
            if (path.isEmpty()) {
                return;
            }
            QSaveFile file(path);
            const QByteArray payload =
                uiState_->assistantTranscript_->toPlainText().toUtf8();
            if (!file.open(QIODevice::WriteOnly | QIODevice::Text) ||
                file.write(payload) != payload.size() ||
                !file.commit()) {
                QMessageBox::warning(
                    mainWindow_.get(),
                    QStringLiteral("Export Conversation"),
                    QStringLiteral(
                        "The conversation could not be exported to %1.")
                        .arg(QDir::toNativeSeparators(path)));
                return;
            }
            ShowStatusMessage(
                QStringLiteral("Conversation exported to %1.")
                    .arg(QDir::toNativeSeparators(path)),
                7000);
        });
    }
    if (uiState_->assistantDeleteButton_) {
        connect(uiState_->assistantDeleteButton_, &QPushButton::clicked, this, [this]() {
            QListWidgetItem* item = uiState_->assistantHistoryList_->currentItem();
            if (!item) {
                return;
            }
            const QString threadId = item->data(Qt::UserRole).toString();
            if (QMessageBox::question(mainWindow_.get(),
                                      QStringLiteral("Delete Conversation"),
                                      QStringLiteral("Permanently delete this OpenZoom assistant conversation?"))
                == QMessageBox::Yes) {
                assistiveManager_->Runtime().DeleteAssistantConversation(threadId);
            }
        });
    }

    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexServerStateChanged,
            this, [this](bool ready, const QString& status) {
                codexReady_ = ready;
                SetLiveText(uiState_->assistantConnectionLabel_, status,
                            LivePoliteness::kPolite,
                            QStringLiteral("Codex connection status"));
                SetAssistantBusy(assistiveManager_->Runtime().IsCodexTurnActive());
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexAccountChanged,
            this, [this](bool signedIn, const QString& label, const QString& planType) {
                codexSignedIn_ = signedIn;
                const QString plan = planType.trimmed();
                SetLiveText(
                    uiState_->assistantConnectionLabel_,
                    plan.isEmpty()
                        ? label
                        : QStringLiteral("%1 (%2)").arg(label, plan),
                    LivePoliteness::kPolite,
                    QStringLiteral("Codex connection status"));
                SetLiveText(
                    uiState_->assistantConnectButton_,
                    signedIn ? QStringLiteral("Reconnect ChatGPT")
                             : QStringLiteral("Connect ChatGPT"),
                    LivePoliteness::kSilent,
                    QStringLiteral("ChatGPT connection"));
                SetAssistantBusy(assistiveManager_->Runtime().IsCodexTurnActive());
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexModelsChanged,
            this, [this](const QStringList&, const QString& selectedModel) {
                if (!selectedModel.isEmpty()) {
                    uiState_->assistantConnectionLabel_->setToolTip(
                        QStringLiteral("Vision model: %1").arg(selectedModel));
                }
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexModelCatalogChanged,
            this, [this](const QJsonArray& models, const QString& selectedModel) {
                codexModelCatalog_ = models;
                selectedCodexModel_ = selectedModel;
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexRateLimitChanged,
            this, [this](const QString& summary) {
                SetLiveText(uiState_->assistantUsageLabel_, summary,
                            LivePoliteness::kSilent,
                            QStringLiteral("Codex usage"));
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexLoginUrlReady,
            this, [](const QUrl& url) { QDesktopServices::openUrl(url); });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::PrivacyNotice,
            this, [this](const QString& summary) {
                ShowStatusMessage(summary, 12000);
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantConversationCreated,
            this, [this](const QJsonObject& thread) {
                settings::CodexConversation conversation;
                conversation.threadId = thread.value(QStringLiteral("id")).toString();
                conversation.preview = pendingAssistantPrompt_.left(160);
                conversation.title = pendingAssistantPrompt_.simplified().left(60);
                if (conversation.title.isEmpty()) {
                    conversation.title = QStringLiteral("OpenZoom Assistant");
                }
                conversation.createdAt = thread.value(QStringLiteral("createdAt")).toInteger(
                    QDateTime::currentSecsSinceEpoch());
                conversation.updatedAt = conversation.createdAt;
                const auto existing = std::find_if(
                    settingsController_->MutableSettings().codexConversations.begin(),
                    settingsController_->MutableSettings().codexConversations.end(),
                    [&conversation](const settings::CodexConversation& candidate) {
                        return candidate.threadId == conversation.threadId;
                    });
                if (existing == settingsController_->MutableSettings().codexConversations.end()) {
                    settingsController_->MutableSettings().codexConversations.push_back(conversation);
                }
                currentAssistantThreadId_ = conversation.threadId;
                PopulateAssistantHistory();
                SavePersistentSettings();
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantTranscriptLoaded,
            this, [this](const QString& threadId, const QJsonArray& messages) {
                currentAssistantThreadId_ = threadId;
                uiState_->assistantTranscript_->clear();
                for (const QJsonValue& value : messages) {
                    const QJsonObject message = value.toObject();
                    const QString speaker = message.value(QStringLiteral("role")).toString()
                                                    == QStringLiteral("user")
                                                ? QStringLiteral("You")
                                                : QStringLiteral("OpenZoom Assistant");
                    AppendAssistantMessage(speaker, message.value(QStringLiteral("text")).toString());
                }
                assistantResponseOpen_ = false;
                assistantResponseReceivedText_ = false;
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantConversationRenamed,
            this, [this](const QString& threadId, const QString& name) {
                for (settings::CodexConversation& conversation : settingsController_->MutableSettings().codexConversations) {
                    if (conversation.threadId == threadId) {
                        conversation.title = name;
                        conversation.updatedAt = QDateTime::currentSecsSinceEpoch();
                        break;
                    }
                }
                PopulateAssistantHistory();
                SavePersistentSettings();
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantConversationDeleted,
            this, [this](const QString& threadId) {
                std::erase_if(settingsController_->MutableSettings().codexConversations,
                              [&threadId](const settings::CodexConversation& conversation) {
                                  return conversation.threadId == threadId;
                              });
                if (currentAssistantThreadId_ == threadId) {
                    currentAssistantThreadId_.clear();
                    uiState_->assistantTranscript_->clear();
                }
                PopulateAssistantHistory();
                SavePersistentSettings();
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantTurnStarted,
            this, [this](const QString&, const QString&, bool persistent) {
                SetAssistantBusy(true);
                if (!persistent && uiState_->explainNowButton_) {
                    SetLiveText(uiState_->explainNowButton_,
                                QStringLiteral("Stop"),
                                LivePoliteness::kSilent,
                                QStringLiteral("Explain"));
                }
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantTextDelta,
            this, [this](const QString& threadId, const QString&, const QString& delta) {
                if (!assistantResponseOpen_ ||
                    (!currentAssistantThreadId_.isEmpty() && threadId != currentAssistantThreadId_)) {
                    return;
                }
                QTextCursor cursor = uiState_->assistantTranscript_->textCursor();
                cursor.movePosition(QTextCursor::End);
                cursor.insertText(delta);
                uiState_->assistantTranscript_->setTextCursor(cursor);
                uiState_->assistantTranscript_->ensureCursorVisible();
                assistantResponseReceivedText_ = true;
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantTurnFinished,
            this,
            [this](const QString& threadId,
                   const QString&,
                   const QString& text,
                   const QString& error,
                   bool interrupted,
                   bool persistent) {
                SetAssistantBusy(false);
                if (uiState_->explainNowButton_) {
                    SetLiveText(uiState_->explainNowButton_,
                                QStringLiteral("Explain"),
                                LivePoliteness::kSilent,
                                QStringLiteral("Explain"));
                }
                if (!persistent) {
                    return;
                }
                if (assistantResponseOpen_) {
                    QTextCursor cursor = uiState_->assistantTranscript_->textCursor();
                    cursor.movePosition(QTextCursor::End);
                    if (!assistantResponseReceivedText_ && !text.trimmed().isEmpty()) {
                        cursor.insertText(text.trimmed());
                    }
                    if (!error.trimmed().isEmpty()) {
                        cursor.insertText(QStringLiteral("\n%1").arg(error.trimmed()));
                    } else if (interrupted) {
                        cursor.insertText(
                            QStringLiteral("\n") +
                            TranslateUi(QStringLiteral("Stopped.")));
                    }
                    cursor.insertText(QStringLiteral("\n\n"));
                    uiState_->assistantTranscript_->setTextCursor(cursor);
                }
                assistantResponseOpen_ = false;
                assistantResponseReceivedText_ = false;
                for (settings::CodexConversation& conversation : settingsController_->MutableSettings().codexConversations) {
                    if (conversation.threadId == threadId) {
                        conversation.updatedAt = QDateTime::currentSecsSinceEpoch();
                        break;
                    }
                }
                PopulateAssistantHistory();
                SavePersistentSettings();
            });

    ApplyPersistentSettings(settingsController_->MutableSettings());
    PopulateAssistantHistory();

    UpdateBlurUiLabels();
    UpdateTemporalSmoothUi();
    UpdateSpatialSharpenUi();
    UpdateProcessingStatusLabel();
    UpdateRotationUi();
    UpdatePresetDescription();
    assistiveManager_->SetModes(
        vlmAssistEnabled_, assistiveOverlayEnabled_);

    int initialCameraIndex = 0;
    const int candidate = settingsController_->MutableSettings().cameraIndex;
    if (candidate >= 0 && static_cast<size_t>(candidate) < cameras_.size()) {
        initialCameraIndex = candidate;
    }
    if (!cameras_.empty()) {
        auto blocker = uiState_->BlockSignals(uiState_->cameraCombo_);
        uiState_->cameraCombo_->setCurrentIndex(initialCameraIndex);
        if (!startupSynchronousProfile_) {
            // All app services/settings now exist. Open the camera while show()
            // initializes the native presenter; completion still returns via Qt.
            StartCameraCapture(static_cast<size_t>(initialCameraIndex), true, false, true);
        }
    }

    mainWindow_->show();
    startupWindowMs_ = startupTimer_.elapsed();
    qInfo() << "Startup timing: window shown" << startupWindowMs_ << "ms";
    ApplyNativeWindowIcon(mainWindow_.get());
    pipelineOrchestrator_->Start();
    pipelineOrchestrator_->NotifyCameraFrameAvailable();

    QTimer::singleShot(
        1500, this,
        [this, userDataRootNotice, photoRecoveryNotice]() {
            const QString settingsNotice =
                settingsController_->TakeStartupNotice();
            if (!settingsNotice.isEmpty()) {
                ShowStatusMessage(settingsNotice, 12000);
            }
            if (!userDataRootNotice.isEmpty()) {
                ShowStatusMessage(userDataRootNotice, 15000);
            }
            if (!photoRecoveryNotice.isEmpty()) {
                ShowStatusMessage(photoRecoveryNotice, 15000);
            }
            if (!settingsController_->MutableSettings()
                     .setupAssistantDeclined &&
                SetupAssistantDialog::NeedsSetup(
                    settingsController_->MutableSettings()
                        .assistive.codexExecutablePath)) {
                OpenSetupAssistant();
            }
        });

    if (!cameras_.empty() && startupSynchronousProfile_) {
        // Opt-in comparison mode reproduces the former double-open,
        // synchronous startup path for the same profiling executable.
        RefreshCameraFormats(static_cast<size_t>(initialCameraIndex));
        StartCameraCapture(static_cast<size_t>(initialCameraIndex));
    }
    initialized_ = true;
    return true;
}

void OpenZoomApp::EnsureTranscriptionStack()
{
    if (transcriptionController_ || !userDataPaths_) {
        return;
    }
    realtimeTranscriptionClient_ =
        std::make_unique<CodexRealtimeTranscriptionClient>();
    // Carrier D: the native WebRTC stack has no browser runtime, profile
    // directory, or staged assets — it is always constructible.
    realtimeNativeRtcCarrier_ = std::make_unique<RealtimeNativeRtcCarrier>();
    transcriptionController_ = std::make_unique<TranscriptionSessionController>(
        realtimeTranscriptionClient_.get(), realtimeNativeRtcCarrier_.get());

    connect(transcriptionController_.get(),
            &TranscriptionSessionController::SegmentFinalized,
            this, [this](const TranscriptSegment& segment) {
                if (settingsController_ &&
                    settingsController_->Settings().appendTranscriptToNotes &&
                    assistiveManager_) {
                    if (!assistiveManager_->Runtime().NoteTranscriptSegment(segment)) {
                        transcriptionNoteWriteFailed_ = true;
                        ShowStatusMessage(
                            QCoreApplication::translate(
                                "OpenZoom",
                                "Transcript could not be saved to lecture notes."),
                            9000);
                    }
                }
                if (uiState_ && uiState_->transcriptFinalsView_) {
                    uiState_->transcriptFinalsView_->appendPlainText(segment.text);
                }
                if (mainWindow_) {
                    mainWindow_->AppendSimpleTranscriptFinal(segment.text);
                }
            });
    connect(transcriptionController_.get(),
            &TranscriptionSessionController::PartialChanged,
            this, [this](const QString&, quint64, const QString& text) {
                // Silent, coalesced presentation only — no UIA event per
                // delta.
                if (uiState_ && uiState_->transcriptPartialLabel_) {
                    uiState_->transcriptPartialLabel_->setText(text);
                }
                if (mainWindow_) {
                    mainWindow_->SetSimpleTranscriptPartial(text);
                }
            });
    connect(transcriptionController_.get(),
            &TranscriptionSessionController::QuotaChanged,
            this, [this](int remainingPercent, bool known) {
                if (!uiState_ || !uiState_->transcriptionQuotaLabel_) {
                    return;
                }
                QString text =
                    known ? QCoreApplication::translate(
                                "OpenZoom",
                                "Codex usage: %1% remaining in the current "
                                "general window.")
                                .arg(remainingPercent)
                          : QCoreApplication::translate(
                                "OpenZoom", "Codex usage is unavailable.");
                text += QLatin1Char(' ');
                text += QCoreApplication::translate(
                    "OpenZoom",
                    "Voice-specific remaining time is not exposed by this "
                    "Codex app-server.");
                uiState_->transcriptionQuotaLabel_->setText(text);
                uiState_->transcriptionQuotaLabel_->setVisible(true);
            });
    connect(transcriptionController_.get(),
            &TranscriptionSessionController::GapDetected,
            this, [this](const QString& sessionId, qint64, qint64) {
                if (settingsController_ &&
                    settingsController_->Settings().appendTranscriptToNotes &&
                    assistiveManager_) {
                    if (!assistiveManager_->Runtime().NoteTranscriptGap(sessionId)) {
                        transcriptionNoteWriteFailed_ = true;
                        ShowStatusMessage(
                            QCoreApplication::translate(
                                "OpenZoom",
                                "Transcript could not be saved to lecture notes."),
                            9000);
                    }
                }
                ShowStatusMessage(
                    QCoreApplication::translate(
                        "OpenZoom",
                        "Transcript has a gap; recording is unaffected."),
                    9000);
            });
    connect(transcriptionController_.get(),
            &TranscriptionSessionController::StateChanged,
            this, &OpenZoomApp::OnTranscriptionStateChanged);
}

void OpenZoomApp::StartTranscriptionForActiveRecording()
{
    if (!settingsController_ ||
        !settingsController_->Settings().liveTranscriptionEnabled) {
        return;
    }
    EnsureTranscriptionStack();
    if (!transcriptionController_) {
        return;
    }
    transcriptionController_->SetCodexExecutable(
        settingsController_->Settings().assistive.codexExecutablePath);
    transcriptionController_->SetEnabled(true);
    if (!audioCapture_.IsRunning()) {
        ShowStatusMessage(
            QCoreApplication::translate(
                "OpenZoom", "Select a recording microphone to transcribe."),
            9000);
        return;
    }
    const auto session =
        recordingManager_ ? recordingManager_->CurrentSessionInfo()
                          : std::nullopt;
    if (!session.has_value()) {
        return;
    }
    transcriptionNoteWriteFailed_ = false;
    transcriptionNotesCompletionPending_ = false;
    if (uiState_ && uiState_->transcriptFinalsView_) {
        uiState_->transcriptFinalsView_->clear();
    }
    if (mainWindow_) {
        mainWindow_->SetSimpleTranscriptActive(false);
    }
    transcriptionController_->StartSession(
        *session,
        languageManager_ ? languageManager_->languageCode()
                         : QStringLiteral("en"));
}

void OpenZoomApp::OnTranscriptionStateChanged(TranscriptionState state,
                                              const QString& status)
{
    // Fixed sentences translate via the catalog; a dynamic runtime reason
    // passes through unchanged rather than being assembled from fragments.
    const QString translatedStatus =
        status.isEmpty() ? QString() : TranslateUi(status);
    if (uiState_ && uiState_->transcriptionStatusLabel_) {
        uiState_->transcriptionStatusLabel_->setText(translatedStatus);
        uiState_->transcriptionStatusLabel_->setVisible(!status.isEmpty());
    }
    if (mainWindow_) {
        const bool overlayActive = state == TranscriptionState::Starting ||
                                   state == TranscriptionState::Listening ||
                                   state == TranscriptionState::Finalizing;
        mainWindow_->SetSimpleTranscriptActive(overlayActive);
    }
    if (uiState_ && uiState_->transcriptPartialLabel_ &&
        (state == TranscriptionState::Starting ||
         state == TranscriptionState::Completed ||
         state == TranscriptionState::Failed)) {
        uiState_->transcriptPartialLabel_->clear();
    }
    switch (state) {
    case TranscriptionState::Failed: {
        const QString reason =
            translatedStatus.isEmpty()
                ? QCoreApplication::translate("OpenZoom",
                                              "Live transcription failed.")
                : translatedStatus;
        // The wording always reaffirms that recording is unaffected.
        ShowStatusMessage(
            QCoreApplication::translate(
                "OpenZoom", "Transcription unavailable: %1 Recording continues.")
                .arg(reason),
            12000);
        break;
    }
    case TranscriptionState::Unavailable:
        if (!translatedStatus.isEmpty()) {
            ShowStatusMessage(translatedStatus, 9000);
        }
        break;
    case TranscriptionState::Starting:
        if (!translatedStatus.isEmpty()) {
            ShowStatusMessage(translatedStatus, 4000);
        }
        break;
    case TranscriptionState::Listening:
    case TranscriptionState::Finalizing:
        if (!translatedStatus.isEmpty()) {
            ShowStatusMessage(translatedStatus, 4000);
        }
        break;
    case TranscriptionState::Completed:
        transcriptionNotesCompletionPending_ = true;
        MaybeReportTranscriptNotesSaved();
        break;
    case TranscriptionState::Off:
    case TranscriptionState::Ready:
        break;
    }
}

void OpenZoomApp::MaybeReportTranscriptNotesSaved()
{
    if (!transcriptionNotesCompletionPending_ || !settingsController_ ||
        !settingsController_->Settings().appendTranscriptToNotes ||
        !assistiveManager_ || transcriptionNoteWriteFailed_ ||
        assistiveManager_->Runtime().HasPendingNotesWrites() ||
        assistiveManager_->Runtime().notesFilePath().isEmpty()) {
        return;
    }
    transcriptionNotesCompletionPending_ = false;
    ShowStatusMessage(QCoreApplication::translate(
        "OpenZoom", "Transcript saved to lecture notes."), 7000);
}

OpenZoomApp::~OpenZoomApp() {
    WriteStartupProfile();
    if (cameraStartupPending_) {
        StopCameraCapture(true);
    }
    if (settingsController_ && uiState_ && assistiveManager_) {
        SavePersistentSettings();
    }
    // End the transcript before microphone/recorder teardown: revokes audio
    // acceptance, stops the WebRTC carrier, and bounded-stops the dedicated
    // Codex child. Never waits on finalization.
    if (transcriptionController_) {
        transcriptionController_->Cancel();
    }
    StopMicrophoneCapture();
    if (microphoneCallbackTarget_) {
        std::lock_guard lock(microphoneCallbackTarget_->mutex);
        microphoneCallbackTarget_->accepting = false;
        ++microphoneCallbackTarget_->generation;
        microphoneCallbackTarget_->app = nullptr;
    }
    // If a detached thread may still be inside Media Foundation, or poisoned
    // recorder objects were intentionally leaked after a recovered worker,
    // process-global MF/COM teardown must be skipped.
    bool mediaFoundationTeardownUnsafe = audioCapture_.WasAbandoned();
    if (recordingManager_) {
        const bool managerSafeToDestroy =
            recordingManager_->ShutdownForProcessExit();
        // Abandonment is sticky even when the poisoned worker later recovers
        // and joins. Its VideoRecorder COM references are intentionally
        // leaked, so MFShutdown remains unsafe although manager destruction
        // is now safe.
        mediaFoundationTeardownUnsafe =
            mediaFoundationTeardownUnsafe ||
            recordingManager_->IsWorkerAbandoned();
        if (!managerSafeToDestroy) {
            // The worker is still wedged and detached. It references the
            // manager's queues and recorders, so leak the complete manager.
            (void)recordingManager_.release();
        }
    }
    recordingManager_.reset();
    if (imageIoPool_) {
        imageIoPool_->clear();
        // A photo write to a removable or network user-data folder must not
        // hang app close forever. On timeout the pool (and its writer
        // threads) is leaked for process exit; the in-flight tasks only
        // touch their own captured copies and QPointer-guard the app.
        if (!imageIoPool_->waitForDone(10000)) {
            qCritical() << "Image writes did not finish within 10 s at "
                           "shutdown; leaking the writer pool for process "
                           "exit.";
            (void)imageIoPool_.release();
        }
    }
    if (pipelineOrchestrator_) {
        pipelineOrchestrator_->Stop();
    }
    if (cameraActive_ || cameraIngress_) {
        StopCameraCapture(true);
    }
    mediaCapture_.Shutdown();
    mediaFoundationTeardownUnsafe =
        mediaFoundationTeardownUnsafe || mediaCapture_.WasAbandoned() ||
        (startupWorkers_ && (startupWorkers_->active.load() != 0 ||
                             startupWorkers_->abandoned.load()));

    // Stop service callbacks before either the runtime or its UI targets are
    // released. In particular, terminating the Codex child process can emit a
    // final server-state notification synchronously.
    if (assistiveManager_) {
        QObject::disconnect(&assistiveManager_->Runtime(), nullptr, this, nullptr);
    }
    if (mainWindow_) {
        QObject::disconnect(mainWindow_.get(), nullptr, this, nullptr);
        const auto uiObjects = mainWindow_->findChildren<QObject*>();
        for (QObject* object : uiObjects) {
            QObject::disconnect(object, nullptr, this, nullptr);
        }
    }

    interactionController_.reset();
    pipelineOrchestrator_.reset();
    assistiveManager_.reset();
    // The UI signal graph is disconnected above, so child-widget teardown
    // cannot call back through UIStateManager's non-owning widget pointers.
    mainWindow_.reset();
    uiState_.reset();
    joystickOverlay_ = nullptr;
    const bool graphicsIdle = !presenter_ || presenter_->WaitForIdle();
    const bool gpuIdle = graphicsIdle && (!cudaSurface_ || cudaSurface_->WaitForIdle());
    if (gpuIdle) {
        cudaSurface_.reset();
        cudaSharedTexture_.Reset();
        cudaSuperResTexture_.Reset();
        cudaOriginalTexture_.Reset();
    } else {
        // Unsignaled CUDA/D3D work still owns these allocations. Process exit
        // reclaims them; timeout never grants permission to destroy them.
        (void)cudaSurface_.release();
        cudaSharedTexture_.Detach();
        cudaSuperResTexture_.Detach();
        cudaOriginalTexture_.Detach();
        mediaFoundationTeardownUnsafe = true;
    }
    cudaSuperResWidth_ = 0;
    cudaSuperResHeight_ = 0;
    presenter_.reset();

    if (mfInitialized_) {
        if (mediaFoundationTeardownUnsafe) {
            qCritical() << "Skipping MFShutdown: a Media Foundation worker "
                           "is still active, was abandoned, or its objects were intentionally "
                           "leaked; process exit reclaims the runtime.";
        } else {
            MFShutdown();
        }
        mfInitialized_ = false;
    }

    if (comInitialized_) {
        if (!mediaFoundationTeardownUnsafe) {
            CoUninitialize();
        }
        comInitialized_ = false;
    }

    delete qtApp_;
    qtApp_ = nullptr;
}

int OpenZoomApp::Run() {
    return initialized_ && qtApp_ ? qtApp_->exec() : -1;
}

void OpenZoomApp::InitializePlatform() {
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(coInit) || coInit == RPC_E_CHANGED_MODE) {
        comInitialized_ = SUCCEEDED(coInit);
    } else {
        ThrowIfFailed(coInit, "Failed to initialize COM");
    }

    const HRESULT mfInit = MFStartup(MF_VERSION);
    if (FAILED(mfInit)) {
        ThrowIfFailed(mfInit, "Failed to initialize Media Foundation");
    }
    mfInitialized_ = true;

    if (!mediaCapture_.Initialize()) {
        qWarning() << "MediaCapture initialization failed";
    }
    EnumerateCameras();
    EnumerateMicrophones();
}

void OpenZoomApp::ResolveCudaBufferFormatFromOptions() {
    cudaBufferFormat_ = CudaBufferFormat::kRgba8;

    bool ok = false;
    const QByteArray envValue = qgetenv("OPENZOOM_CUDA_BUFFER_FORMAT");
    if (!envValue.isEmpty()) {
        const auto parsed = ParseCudaBufferFormatToken(QString::fromUtf8(envValue), &ok);
        if (ok) {
            cudaBufferFormat_ = parsed;
        } else {
            qWarning() << "Ignoring unknown OPENZOOM_CUDA_BUFFER_FORMAT value" << envValue;
        }
    }

    const QStringList args = qtApp_->arguments();
    for (const QString& arg : args) {
        if (arg.startsWith(QStringLiteral("--cuda-buffer-format="), Qt::CaseInsensitive)) {
            const QString value = arg.section('=', 1);
            const auto parsed = ParseCudaBufferFormatToken(value, &ok);
            if (ok) {
                cudaBufferFormat_ = parsed;
            } else {
                qWarning() << "Ignoring unknown --cuda-buffer-format option" << value;
            }
        } else if (arg.compare(QStringLiteral("--cuda-buffer-fp16"), Qt::CaseInsensitive) == 0) {
            cudaBufferFormat_ = CudaBufferFormat::kRgba16F;
        } else if (arg.compare(QStringLiteral("--cuda-buffer-rgba8"), Qt::CaseInsensitive) == 0) {
            cudaBufferFormat_ = CudaBufferFormat::kRgba8;
        }
    }

    const char* label = (cudaBufferFormat_ == CudaBufferFormat::kRgba16F) ? "RGBA16F" : "RGBA8";
    qInfo() << "CUDA staging buffer format set to" << label;
}


} // namespace openzoom

#endif // _WIN32
