#ifdef _WIN32

#include "app_internal.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/common/codex_agent_client.hpp"
#include "okuflow/common/http_agent_client.hpp"
#include "okuflow/common/visible_chat_session.hpp"
#include "okuflow/common/visible_chat_session_store.hpp"
#include "okuflow/ui/assistant_chat_view.hpp"
#include "okuflow/ui/agent_control_panel.hpp"
#include "okuflow/mcp/app_ipc_endpoint.hpp"
#include <QLocale>
#include "debug_log.hpp"
#include "okuflow/capture/phone_camera.hpp"

#include <QSaveFile>
#include <QStandardPaths>
#include <QScrollBar>
#include <QThreadPool>

namespace okuflow {

OkuFlowApp::OkuFlowApp(int& argc, char** argv)
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
    QCoreApplication::setOrganizationName(QStringLiteral("OkuFlow"));
    QCoreApplication::setApplicationName(QStringLiteral("OkuFlow"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("okuflow.com"));
    ConfigureStartupProfiling();
}

bool OkuFlowApp::Initialize()
{
    if (initialized_) {
        return true;
    }

    const HRESULT appIdResult =
        SetCurrentProcessExplicitAppUserModelID(L"CemKaya.OkuFlow");
    if (FAILED(appIdResult)) {
        qWarning() << "Could not set Windows AppUserModelID"
                   << QStringLiteral("0x%1").arg(static_cast<qulonglong>(appIdResult), 0, 16);
    }
    const QIcon applicationIcon(QStringLiteral(":/okuflow/icons/app.png"));
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
    cameraIntent_.LoadSaved({persistentSettings.cameraSymbolicLink,persistentSettings.cameraFormatStableId,
        persistentSettings.cameraIndex,persistentSettings.cameraSelectionExplicit});
    // An explicit saved language wins; "system" (also a missing or unknown
    // value) resolves the Windows display-language list on every start.
    persistentSettings.language =
        NormalizeLanguagePreference(persistentSettings.language);
    if (persistentSettings.language.isEmpty()) {
        persistentSettings.language =
            QString::fromLatin1(kSystemLanguagePreference);
    }
    const AppLanguage initialLanguage = ResolveLanguagePreference(
        persistentSettings.language, QLocale::system().uiLanguages());
    // A missing or unloadable catalog leaves the UI in English. The saved
    // preference is kept; the selector and a status notice show English.
    const bool startupLanguageFailed =
        !languageManager_->SetLanguage(initialLanguage, false);
    if (startupLanguageFailed) {
        languageManager_->SetLanguage(AppLanguage::English, false);
    }
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
                    "The saved OkuFlow folder cannot be used (%1). Files "
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
        const int index = languageCombo->findData(
            startupLanguageFailed
                ? AppLanguageCode(AppLanguage::English)
                : settingsController_->Settings().language);
        languageCombo->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (startupLanguageFailed) {
        ShowStatusMessage(
            TranslateUi(QStringLiteral(
                "The selected language could not be loaded.")),
            7000);
    }
    connect(mainWindow_.get(),
            &MainWindow::applicationLanguageRequested,
            this,
            [this](const QString& code) {
                if (!languageManager_ || !mainWindow_) {
                    return;
                }
                QString preference = NormalizeLanguagePreference(code);
                if (preference.isEmpty()) {
                    preference = QString::fromLatin1(kSystemLanguagePreference);
                }
                const AppLanguage requested = ResolveLanguagePreference(
                    preference, QLocale::system().uiLanguages());
                if (!languageManager_->SetLanguage(requested, false)) {
                    if (QComboBox* combo =
                            mainWindow_->applicationLanguageCombo()) {
                        const QSignalBlocker blocker(combo);
                        const QString& saved = settingsController_->Settings().language;
                        const QString visible = ResolveLanguagePreference(
                            saved, QLocale::system().uiLanguages()) == languageManager_->language()
                            ? saved : languageManager_->languageCode();
                        const int previous = combo->findData(
                            visible);
                        combo->setCurrentIndex(previous >= 0 ? previous : 0);
                    }
                    ShowStatusMessage(
                        TranslateUi(QStringLiteral(
                            "The selected language could not be loaded.")),
                        7000);
                    return;
                }

                settingsController_->MutableSettings().language = preference;
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
    auto* mainChat = mainWindow_->assistantChatView();
    auto* floatingChat = assistiveManager_->Overlay().chatView();
    visibleChatStore_ = std::make_unique<VisibleChatSessionStore>(
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("visible-chat")));
    visibleChatStore_->SetSecretFilters({settingsController_->MutableSettings().assistive.vlmApiKey.trimmed(),
        qEnvironmentVariable("OKUFLOW_VLM_API_KEY").trimmed()});
    visibleChatSaveTimer_ = new QTimer(this); visibleChatSaveTimer_->setSingleShot(true);
    visibleChatSaveTimer_->setInterval(350);
    connect(visibleChatSaveTimer_,&QTimer::timeout,this,&OkuFlowApp::PersistVisibleChat);
    connect(&assistiveManager_->Runtime().ChatSession(),&VisibleChatSession::SnapshotChanged,this,[this](const QJsonObject&) {
        visibleChatSaveTimer_->start();
    });
    auto renderChat = [this](const QJsonObject& snapshot) {
        auto presentation = snapshot;
        const bool busy = snapshot.value(QStringLiteral("busy")).toBool() || assistiveManager_->Runtime().IsForegroundBusy();
        presentation[QStringLiteral("busy")] = busy;
        mainWindow_->assistantChatView()->SetChatSnapshot(presentation);
        assistiveManager_->Overlay().SetChatSnapshot(presentation);
        SetAssistantBusy(busy);
        const auto messages = snapshot.value(QStringLiteral("messages")).toArray();
        bool explainBusy = false;
        if (snapshot.value(QStringLiteral("busy")).toBool())
            for (auto row = messages.size()-1; row >= 0; --row) {
                const auto message = messages[row].toObject();
                if (message.value(QStringLiteral("role")) == QStringLiteral("user")) {
                    explainBusy = message.value(QStringLiteral("origin")) == QStringLiteral("explain"); break;
                }
            }
        mainWindow_->setExplainBusy(pendingOnDemandAnalysis_ || explainBusy);
        if (!snapshot.value(QStringLiteral("busy")).toBool()) assistantResponseOpen_ = false;
    };
    connect(&assistiveManager_->Runtime().ChatSession(),&VisibleChatSession::SnapshotChanged,this,renderChat);
    renderChat(assistiveManager_->Runtime().ChatSession().Snapshot());
    for (auto* chat : {mainChat,floatingChat}) {
        connect(chat,&AssistantChatView::ChatDraftEdited,this,[this](const QString& draft) {
            assistiveManager_->Runtime().ChatSession().SetDraft(draft);
        });
        connect(chat,&AssistantChatView::ChatAttachmentChanged,this,[this](bool attached) {
            auto& runtime = assistiveManager_->Runtime();
            if (attached && runtime.IsForegroundBusy()) {
                runtime.ChatSession().Refresh(); return; // A running turn cannot acquire a broader grant.
            }
            if (!attached) {
                agentFrames_ = false;
                if (controlService_) controlService_->RevokeFrameGrant(agentOwner_);
                if (controlService_ && !agentContext_.isEmpty())
                    controlService_->SetGrants(agentContext_,{false,agentPhotos_,agentRecording_,false});
                for (auto* panel : {mainWindow_->agentControlPanel(),assistiveManager_->Overlay().agentControlPanel()})
                    if (panel) panel->SetSessionGrants(false,agentPhotos_,agentRecording_);
            }
            runtime.ChatSession().SetAttachCurrentView(attached);
        });
        connect(chat,&AssistantChatView::ChatSelected,this,&OkuFlowApp::LoadAssistantConversationById);
        connect(chat,&AssistantChatView::ChatReadAloudRequested,this,[this](const QString& messageId,const QString&) {
            const auto messages = assistiveManager_->Runtime().ChatSession().Snapshot().value(QStringLiteral("messages")).toArray();
            for (const auto& value : messages) {
                const auto message = value.toObject();
                if (message.value(QStringLiteral("id")).toString() == messageId &&
                    message.value(QStringLiteral("role")) == QStringLiteral("assistant") &&
                    message.value(QStringLiteral("complete")).toBool()) {
                    assistiveManager_->Runtime().ReadAloud(message.value(QStringLiteral("text")).toString());
                    break;
                }
            }
        });
        connect(chat,&AssistantChatView::StopRequested,this,&OkuFlowApp::StopAssistantRequest);
    }
    connect(mainChat,&AssistantChatView::ChatSubmitted,this,[this](const QString& prompt) {
        SubmitAssistantPromptText(prompt,true,false);
    });
    connect(mainChat,&AssistantChatView::ChatNewRequested,this,&OkuFlowApp::StartNewAssistantConversation);
    connect(floatingChat,&AssistantChatView::AISettingsRequested,this,&OkuFlowApp::OpenAiSettingsDialog);
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
                    openNotesWhenStored_ = false;
                    qWarning() << "Lecture notes storage failed:" << path << error;
                    ShowStatusMessage(QStringLiteral("Lecture notes could not be saved."), 9000);
                    return;
                }
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
            .presentPendingScene = [this]() { PresentPendingScene(); },
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
        [this](const RecordingSessionInfo& session) {
            if (controlTarget_) controlTarget_->RecordingEnded(session);
            // Session-ended callbacks are queued from the recording worker
            // after the terminal state is published. If a new recording has
            // already started by the time this lands, releasing the
            // microphone now would silently strip audio from that new
            // session.
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
        uiState_->blurRadiusValueLabel_->setText(QLocale().toString(blurRadius_));
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
            this, &OkuFlowApp::OnCameraSelectionChanged);
    connect(uiState_->phoneLightButton_, &QPushButton::clicked, this, [this]() {
        if (cameraActive_ && mediaCapture_.PhoneTorchAvailable())
            SetPhoneTorch(!mediaCapture_.PhoneTorchEnabled());
    });
    connect(uiState_->microphoneCombo_, &QComboBox::currentIndexChanged,
            this, &OkuFlowApp::OnMicrophoneSelectionChanged);
    if (uiState_->presetList_) {
        connect(mainWindow_.get(), &MainWindow::quickModeActivated,
                this, [this](int row) {
                    OnPresetSelectionChanged(uiState_->presetList_->item(row), nullptr);
                });
    }
    if (uiState_->promotePresetButton_) {
        connect(uiState_->promotePresetButton_, &QPushButton::clicked,
                this, [this]() { PromoteCurrentConfigToPreset(); });
    }
    connect(mainWindow_.get(), &MainWindow::resetCurrentProfileRequested,
            this, [this]() { ResetCurrentConfigToDefaults(); });
    connect(uiState_->bwCheckbox_, &QCheckBox::toggled,
            this, &OkuFlowApp::OnBlackWhiteToggled);
    connect(uiState_->bwSlider_, &QSlider::valueChanged,
            this, &OkuFlowApp::OnBlackWhiteThresholdChanged);
    connect(uiState_->zoomCheckbox_, &QCheckBox::toggled,
            this, &OkuFlowApp::OnZoomToggled);
    connect(uiState_->zoomSlider_, &QSlider::valueChanged,
            this, &OkuFlowApp::OnZoomAmountChanged);
    if (uiState_->debugButton_) {
        connect(uiState_->debugButton_, &QPushButton::toggled,
                this, &OkuFlowApp::OnDebugViewToggled);
    }
    if (uiState_->capturePhotoButton_) {
        connect(uiState_->capturePhotoButton_, &QPushButton::clicked, this, [this]() {
            RequestPhotoCapture();
        });
    }
    connect(mainWindow_.get(), &MainWindow::annotationSnapshotRequested,
            this, &OkuFlowApp::QueueAnnotationSnapshot);
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
                    RequestRecording(RecordingAudioRequest::SavedMicrophone,RecordingOwnerKind::Manual,{});
                } else {
                    // Stop producing PCM first. RecordingManager::Stop
                    // deliberately clears its queued audio rather than
                    // draining a tail — finishing the sample the worker
                    // already owns is what keeps Stop fast — so retaining
                    // blocks here only avoids tearing down the device while
                    // the recorder may still reference the current buffer.
                    const auto session = recordingManager_->CurrentSessionInfo();
                    if (session) StopRecording(session->id);
                }
                if (checked && !recordingManager_->IsActive()) {
                    // Recording preflight can reject the session after the
                    // microphone has opened (for example, on low disk space).
                    // Do not retain exclusive access to the device when no
                    // recording session exists.
                    StopMicrophoneCapture();
                }
            }
        });
    }
    if (uiState_->rotationCombo_) {
        connect(uiState_->rotationCombo_, &QComboBox::currentIndexChanged,
                this, &OkuFlowApp::OnRotationSelectionChanged);
    }
    if (uiState_->cameraFormatCombo_) {
        connect(uiState_->cameraFormatCombo_, &QComboBox::currentIndexChanged,
                this, &OkuFlowApp::OnCameraFormatChanged);
    }
    if (uiState_->cameraAccelerationCombo_) {
        connect(
            uiState_->cameraAccelerationCombo_,
            &QComboBox::currentIndexChanged,
            this,
            &OkuFlowApp::OnCameraAccelerationModeChanged);
    }
    if (uiState_->testCameraAccelerationButton_) {
        connect(
            uiState_->testCameraAccelerationButton_,
            &QPushButton::clicked,
            this,
            &OkuFlowApp::OnTestCameraAcceleration);
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
                SetViewportFitMode(mode);
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
                SetRecordingCanvasMode(mode);
            });
    if (uiState_->zoomCenterXSlider_) {
        connect(uiState_->zoomCenterXSlider_, &QSlider::valueChanged,
                this, &OkuFlowApp::OnZoomCenterXChanged);
    }
    if (uiState_->zoomCenterYSlider_) {
        connect(uiState_->zoomCenterYSlider_, &QSlider::valueChanged,
                this, &OkuFlowApp::OnZoomCenterYChanged);
    }
    if (uiState_->collapseButton_) {
        connect(uiState_->collapseButton_, &QToolButton::toggled,
                this, &OkuFlowApp::OnControlsCollapsedToggled);
        OnControlsCollapsedToggled(uiState_->collapseButton_->isChecked());
    }
    if (uiState_->joystickCheckbox_) {
        uiState_->joystickCheckbox_->setChecked(false);
        connect(uiState_->joystickCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnVirtualJoystickToggled);
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
                &OkuFlowApp::SavePersistentSettings);
    }
    if (uiState_->blurCheckbox_) {
        auto block = uiState_->BlockSignals(uiState_->blurCheckbox_);
        uiState_->blurCheckbox_->setChecked(blurEnabled_);
        connect(uiState_->blurCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnBlurToggled);
    }
    if (uiState_->blurSigmaSlider_) {
        connect(uiState_->blurSigmaSlider_, &QSlider::valueChanged,
                this, &OkuFlowApp::OnBlurSigmaChanged);
    }
    if (uiState_->blurRadiusSlider_) {
        connect(uiState_->blurRadiusSlider_, &QSlider::valueChanged,
                this, &OkuFlowApp::OnBlurRadiusChanged);
    }
    if (uiState_->temporalSmoothCheckbox_) {
        connect(uiState_->temporalSmoothCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnTemporalSmoothToggled);
    }
    if (uiState_->temporalSmoothSlider_) {
        connect(uiState_->temporalSmoothSlider_, &QSlider::valueChanged,
                this, &OkuFlowApp::OnTemporalSmoothStrengthChanged);
    }
    if (uiState_->vlmAssistCheckbox_) {
        connect(uiState_->vlmAssistCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnVlmAssistToggled);
    }
    if (uiState_->assistiveOverlayCheckbox_) {
        connect(uiState_->assistiveOverlayCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnAssistiveOverlayToggled);
    }
    if (uiState_->spatialSharpenCheckbox_) {
        connect(uiState_->spatialSharpenCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnSpatialSharpenToggled);
    }
    if (uiState_->spatialBackendCombo_) {
        connect(uiState_->spatialBackendCombo_, &QComboBox::currentIndexChanged,
                this, &OkuFlowApp::OnSpatialUpscalerChanged);
    }
    if (uiState_->spatialSharpnessSlider_) {
        connect(uiState_->spatialSharpnessSlider_, &QSlider::valueChanged,
                this, &OkuFlowApp::OnSpatialSharpnessChanged);
    }
    if (uiState_->focusMarkerCheckbox_) {
        connect(uiState_->focusMarkerCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnFocusMarkerToggled);
    }
    if (uiState_->stabilizationCheckbox_) {
        connect(uiState_->stabilizationCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnStabilizationToggled);
    }
    if (mainWindow_->bumpHoldCheckbox()) {
        connect(mainWindow_->bumpHoldCheckbox(), &QCheckBox::toggled,
                this, &OkuFlowApp::OnBumpHoldToggled);
    }
    if (uiState_->keystoneCheckbox_) {
        connect(uiState_->keystoneCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnKeystoneToggled);
    }
    connect(mainWindow_.get(), &MainWindow::keystoneStepBackRequested,
            this, &OkuFlowApp::OnKeystoneStepBack);
    connect(mainWindow_.get(), &MainWindow::keystonePauseResumeRequested,
            this, &OkuFlowApp::OnKeystonePauseResume);
    connect(mainWindow_.get(), &MainWindow::keystoneStepForwardRequested,
            this, &OkuFlowApp::OnKeystoneStepForward);
    connect(mainWindow_.get(), &MainWindow::superResPerformanceOverrideChanged,
            this, &OkuFlowApp::SetSuperResPerformanceOverride);
    if (uiState_->autoContrastCheckbox_) {
        connect(uiState_->autoContrastCheckbox_, &QCheckBox::toggled,
                this, &OkuFlowApp::OnAutoContrastToggled);
    }
    if (uiState_->autoContrastStrengthSlider_) {
        connect(uiState_->autoContrastStrengthSlider_, &QSlider::valueChanged,
                this, &OkuFlowApp::OnAutoContrastStrengthChanged);
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
                this, &OkuFlowApp::OnDisplayColorSchemeChanged);
    }
    if (uiState_->contrastSlider_) {
        connect(uiState_->contrastSlider_, &QSlider::valueChanged,
                this, &OkuFlowApp::OnContrastChanged);
    }
    if (uiState_->brightnessSlider_) {
        connect(uiState_->brightnessSlider_, &QSlider::valueChanged,
                this, &OkuFlowApp::OnBrightnessChanged);
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
    // Explain shows Stop only while a temporary Explain/Read request is in
    // flight and cancellable (frame preparation, VLM HTTP reply, or Codex
    // turn). Advanced Assistant chat work (open response or queued frame
    // prompt) is a different action and never flips this button.
    const auto explainIsCancellable = [this]() {
        return pendingOnDemandAnalysis_ ||
               (assistiveManager_->Runtime().IsBusy() &&
                !assistantResponseOpen_ &&
                !pendingAssistantFramePrompt_);
    };
    const auto syncExplainBusy = [this, explainIsCancellable]() {
        if (mainWindow_) {
            mainWindow_->setExplainBusy(explainIsCancellable());
        }
    };
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::OverlayUpdated,
            this, [syncExplainBusy](const QString&, const QString&, bool) {
                // Overlay text can precede the associated busy-state write.
                // Observe the completed transition on the next event turn.
                syncExplainBusy();
            }, Qt::QueuedConnection);
    if (uiState_->explainNowButton_) {
        connect(uiState_->explainNowButton_, &QPushButton::clicked,
                this, [this, explainIsCancellable, syncExplainBusy]() {
                    if (explainIsCancellable()) {
                        StopAssistantRequest();
                    } else {
                        SubmitOnDemandAnalysis(false);
                    }
                    syncExplainBusy();
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
    for (QPushButton* button : {uiState_->assistantConnectButton_, floatingChat->connectButton()}) {
        if (button) connect(button, &QPushButton::clicked,
                            this, [this]() { assistiveManager_->Runtime().StartCodexLogin(); });
    }
    const auto syncFloatingHistory = [this,floatingChat] {
        const auto* selected = floatingChat->historyList()->currentItem();
        if (!selected || !uiState_->assistantHistoryList_) return;
        const auto id = selected->data(Qt::UserRole).toString();
        for (int row = 0; row < uiState_->assistantHistoryList_->count(); ++row)
            if (uiState_->assistantHistoryList_->item(row)->data(Qt::UserRole).toString() == id) {
                uiState_->assistantHistoryList_->setCurrentRow(row); break;
            }
    };
    const auto renameConversation = [this]() {
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
                if (threadId.startsWith(QStringLiteral("logical-chat-"))) {
                    RenameStoredConversation(threadId,name); return;
                }
                auto& conversations = settingsController_->MutableSettings().codexConversations;
                const auto saved = std::find_if(conversations.begin(),conversations.end(),[&threadId](const auto& item) {
                    return item.threadId == threadId;
                });
                if (saved != conversations.end() && settings::EffectiveConversationKind(*saved) == QStringLiteral("agent")) {
                    saved->title = name; saved->updatedAt = QDateTime::currentSecsSinceEpoch();
                    PopulateAssistantHistory(); SavePersistentSettings();
                } else assistiveManager_->Runtime().RenameAssistantConversation(threadId, name);
            }
        };
    for (auto* button : {uiState_->assistantRenameButton_,floatingChat->renameButton()})
        if (button) connect(button,&QPushButton::clicked,this,[button,floatingChat,syncFloatingHistory,renameConversation] {
            if (button == floatingChat->renameButton()) syncFloatingHistory();
            renameConversation();
        });
    const auto exportConversation = [this]() {
            QString transcript = uiState_->assistantTranscript_ ? uiState_->assistantTranscript_->toPlainText() : QString();
            const auto* selected = uiState_->assistantHistoryList_ ? uiState_->assistantHistoryList_->currentItem() : nullptr;
            const auto selectedId = selected ? selected->data(Qt::UserRole).toString() : QString();
            if (selectedId.startsWith(QStringLiteral("logical-chat-"))) {
                PersistVisibleChat();
                const auto stored = visibleChatStore_ ? visibleChatStore_->Load(selectedId.mid(13)) : std::nullopt;
                if (!stored) return;
                transcript.clear();
                for (const auto& value : stored->messages) {
                    const auto message = value.toObject();
                    const auto role = message.value(QStringLiteral("role")).toString();
                    if (role != QStringLiteral("user") && role != QStringLiteral("assistant")) continue;
                    transcript += role + QStringLiteral(": ") + message.value(QStringLiteral("text")).toString();
                    for (const auto& action : message.value(QStringLiteral("actions")).toArray())
                        if (action.isString()) transcript += QStringLiteral("\n") + action.toString();
                    const auto status = message.value(QStringLiteral("status")).toString();
                    if (status == QStringLiteral("failed") || status == QStringLiteral("interrupted"))
                        transcript += QStringLiteral("\n[") + status + QStringLiteral("]");
                    transcript += QStringLiteral("\n\n");
                }
            }
            if (transcript.trimmed().isEmpty()) {
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
                              "The OkuFlow Analysis folder is unavailable.")
                        : analysisError);
                return;
            }
            const QString suggested = QDir(analysisDirectory)
                                          .filePath(QStringLiteral("OkuFlow_Assistant_%1.txt")
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
                transcript.toUtf8();
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
        };
    for (auto* button : {uiState_->assistantExportButton_,floatingChat->exportButton()})
        if (button) connect(button,&QPushButton::clicked,this,[button,floatingChat,syncFloatingHistory,exportConversation] {
            if (button == floatingChat->exportButton()) syncFloatingHistory();
            exportConversation();
        });
    const auto deleteConversation = [this]() {
            QListWidgetItem* item = uiState_->assistantHistoryList_->currentItem();
            if (!item) {
                return;
            }
            const QString threadId = item->data(Qt::UserRole).toString();
            if (QMessageBox::question(mainWindow_.get(),
                                      QStringLiteral("Delete Conversation"),
                                      QStringLiteral("Permanently delete this OkuFlow assistant conversation?"))
                == QMessageBox::Yes) {
                if (threadId.startsWith(QStringLiteral("logical-chat-"))) {
                    DeleteStoredConversation(threadId); return;
                }
                if (currentAssistantThreadId_ == threadId) {
                    CancelAssistantHistoryLoad();
                }
                auto& conversations = settingsController_->MutableSettings().codexConversations;
                const auto saved = std::find_if(conversations.begin(),conversations.end(),[&threadId](const auto& item) {
                    return item.threadId == threadId;
                });
                if (saved != conversations.end() && settings::EffectiveConversationKind(*saved) == QStringLiteral("agent")) {
                    conversations.erase(saved); agentTranscripts_.remove(threadId);
                    if (agentConversationId_ == threadId || currentAssistantThreadId_ == threadId) {
                        assistiveManager_->Runtime().ResetVisibleChat(); currentAssistantThreadId_.clear(); agentConversationId_.clear();
                    }
                    PopulateAssistantHistory(); SavePersistentSettings();
                } else assistiveManager_->Runtime().DeleteAssistantConversation(threadId);
            }
        };
    for (auto* button : {uiState_->assistantDeleteButton_,floatingChat->deleteButton()})
        if (button) connect(button,&QPushButton::clicked,this,[button,floatingChat,syncFloatingHistory,deleteConversation] {
            if (button == floatingChat->deleteButton()) syncFloatingHistory();
            deleteConversation();
        });

    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexServerStateChanged,
            this, [this,mainChat,floatingChat](bool ready, const QString& status) {
                codexReady_ = ready;
                SetLiveText(uiState_->assistantConnectionLabel_, status,
                            LivePoliteness::kPolite,
                            QStringLiteral("Codex connection status"));
                // The floating header mirrors the same state; only Advanced announces it.
                SetLiveText(floatingChat->connectionLabel(), status, LivePoliteness::kSilent,
                            QStringLiteral("Codex connection status"));
                for (auto* chat : {mainChat,floatingChat}) chat->SetConnectionState(codexReady_ && codexSignedIn_);
                SetAssistantBusy(assistiveManager_->Runtime().ChatSession().Busy() || assistiveManager_->Runtime().IsForegroundBusy());
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexAccountChanged,
            this, [this,mainChat,floatingChat](bool signedIn, const QString& label, const QString& planType) {
                codexSignedIn_ = signedIn;
                const QString plan = planType.trimmed();
                const QString account = plan.isEmpty()
                    ? label
                    : QStringLiteral("%1 (%2)").arg(label, plan);
                SetLiveText(
                    uiState_->assistantConnectionLabel_,
                    account,
                    LivePoliteness::kPolite,
                    QStringLiteral("Codex connection status"));
                SetLiveText(floatingChat->connectionLabel(), account, LivePoliteness::kSilent,
                            QStringLiteral("Codex connection status"));
                for (QPushButton* button : {uiState_->assistantConnectButton_, floatingChat->connectButton()})
                    SetLiveText(
                        button,
                        signedIn ? QStringLiteral("Reconnect ChatGPT")
                                 : QStringLiteral("Connect ChatGPT"),
                        LivePoliteness::kSilent,
                        QStringLiteral("ChatGPT connection"));
                for (auto* chat : {mainChat,floatingChat}) chat->SetConnectionState(codexReady_ && codexSignedIn_);
                SetAssistantBusy(assistiveManager_->Runtime().ChatSession().Busy() || assistiveManager_->Runtime().IsForegroundBusy());
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexModelsChanged,
            this, [this,floatingChat](const QStringList&, const QString& selectedModel) {
                if (!selectedModel.isEmpty()) {
                    for (QLabel* label : {uiState_->assistantConnectionLabel_, floatingChat->connectionLabel()})
                        label->setToolTip(QStringLiteral("Vision model: %1").arg(selectedModel));
                }
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexModelCatalogChanged,
            this, [this](const QJsonArray& models, const QString& selectedModel) {
                codexModelCatalog_ = models;
                selectedCodexModel_ = selectedModel;
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::CodexRateLimitChanged,
            this, [this,floatingChat](const QString& summary) {
                for (QLabel* label : {uiState_->assistantUsageLabel_, floatingChat->usageLabel()})
                    SetLiveText(label, summary,
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
                if (assistiveManager_->Runtime().HasChatTurn() && assistiveManager_->Runtime().ChatSession().IsLocalLogical()) return;
                settings::CodexConversation conversation;
                conversation.threadId = thread.value(QStringLiteral("id")).toString();
                conversation.kind = thread.value(QStringLiteral("kind")).toString(
                    settingsController_->MutableSettings().assistive.codexCodingEnabled
                        ? QStringLiteral("coding") : QStringLiteral("assistant"));
                conversation.toolsetVersion = thread.value(QStringLiteral("toolsetVersion")).toString();
                conversation.preview = pendingAssistantPrompt_.left(160);
                conversation.title = pendingAssistantPrompt_.simplified().left(60);
                if (conversation.title.isEmpty()) {
                    conversation.title = QStringLiteral("OkuFlow Assistant");
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
    // Legacy Explain anchors are restored only by an explicit history choice;
    // a new logical chat must not inherit a previous private provider thread.
    if (!assistiveManager_->Runtime().ChatSession().IsLocalLogical())
        assistiveManager_->Runtime().SetExplainConversationId(settingsController_->MutableSettings().explainThreadId);
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::ExplainConversationCreated,
            this, [this](const QJsonObject& thread) {
                if (assistiveManager_->Runtime().HasChatTurn() && assistiveManager_->Runtime().ChatSession().IsLocalLogical()) return;
                settings::CodexConversation conversation;
                conversation.threadId = thread.value(QStringLiteral("id")).toString();
                conversation.explain = true;
                conversation.kind = QStringLiteral("explain");
                conversation.createdAt = thread.value(QStringLiteral("createdAt")).toInteger(
                    QDateTime::currentSecsSinceEpoch());
                conversation.updatedAt = conversation.createdAt;
                auto& conversations = settingsController_->MutableSettings().codexConversations;
                if (std::none_of(conversations.begin(), conversations.end(),
                                 [&conversation](const settings::CodexConversation& candidate) {
                                     return candidate.threadId == conversation.threadId;
                                 })) {
                    conversations.push_back(conversation);
                }
                PopulateAssistantHistory();
                SavePersistentSettings();
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::ExplainConversationChanged,
            this, [this](const QString& threadId) {
                if (assistiveManager_->Runtime().ChatSession().IsLocalLogical()) return;
                settingsController_->MutableSettings().explainThreadId = threadId;
                SavePersistentSettings();
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantTranscriptLoaded,
            this, [this](const QString& threadId, const QJsonArray& messages) {
                if (threadId != pendingAssistantHistoryThreadId_ || assistantResponseOpen_ ||
                    pendingAssistantFramePrompt_ || assistiveManager_->Runtime().IsCodexTurnActive()) {
                    return;
                }
                pendingAssistantHistoryThreadId_.clear();
                currentAssistantThreadId_ = threadId;
                auto kind = ChatBackendKind::Assistant;
                for (const auto& conversation : settingsController_->MutableSettings().codexConversations)
                    if (conversation.threadId == threadId) {
                        const auto token = settings::EffectiveConversationKind(conversation);
                        if (token == QStringLiteral("explain")) kind = ChatBackendKind::Explain;
                        else if (token == QStringLiteral("coding")) kind = ChatBackendKind::Coding;
                        break;
                    }
                if (kind == ChatBackendKind::Explain) assistiveManager_->Runtime().SetExplainConversationId(threadId);
                assistiveManager_->Runtime().ChatSession().Restore(threadId,kind,messages);
                assistantResponseOpen_ = false;
                assistantResponseReceivedText_ = false;
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantTranscriptLoadFailed,
            this, [this](const QString& threadId, const QString& error) {
                if (threadId != pendingAssistantHistoryThreadId_ || assistantResponseOpen_ ||
                    pendingAssistantFramePrompt_ || assistiveManager_->Runtime().IsCodexTurnActive()) {
                    return;
                }
                pendingAssistantHistoryThreadId_.clear();
                const QString failure = QStringLiteral("Could not load conversation.");
                QString text = TranslateUi(failure);
                if (!error.trimmed().isEmpty() && error.trimmed() != failure) {
                    text += QStringLiteral("\n") + TranslateUi(error.trimmed());
                }
                ShowStatusMessage(text,7000);
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
                const auto& chat = assistiveManager_->Runtime().ChatSession();
                if (currentAssistantThreadId_ == threadId || chat.BackendThread(ChatBackendKind::Explain) == threadId
                    || chat.BackendThread(ChatBackendKind::Assistant) == threadId || chat.BackendThread(ChatBackendKind::Coding) == threadId) {
                    CancelAssistantHistoryLoad();
                    currentAssistantThreadId_.clear();
                    assistiveManager_->Runtime().ResetVisibleChat();
                }
                PopulateAssistantHistory();
                SavePersistentSettings();
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantTurnStarted,
            this, [this, syncExplainBusy](const QString&, const QString&, bool) {
                if (assistiveManager_->Runtime().HasChatTurn()) return;
                SetAssistantBusy(true);
                syncExplainBusy();
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantTextDelta,
            this, [this](const QString& threadId, const QString&, const QString& delta) {
                if (assistiveManager_->Runtime().HasChatTurn()) return;
                if (!assistantResponseOpen_ ||
                    (!currentAssistantThreadId_.isEmpty() && threadId != currentAssistantThreadId_)) {
                    return;
                }
                QScrollBar* scroll = uiState_->assistantTranscript_->verticalScrollBar();
                const int scrollPosition = scroll->value();
                const bool followTail = !uiState_->assistantTranscript_->textCursor().hasSelection() &&
                                        scrollPosition >= scroll->maximum() - 2;
                QTextCursor cursor(uiState_->assistantTranscript_->document());
                cursor.movePosition(QTextCursor::End);
                cursor.insertText(delta);
                scroll->setValue(followTail ? scroll->maximum() : scrollPosition);
                assistantResponseReceivedText_ = true;
            });
    connect(&assistiveManager_->Runtime(), &AssistiveRuntime::AssistantTurnFinished,
            this,
            [this, syncExplainBusy](const QString& threadId,
                   const QString&,
                   const QString& text,
                   const QString& error,
                   bool interrupted,
                   bool persistent) {
                if (assistiveManager_->Runtime().HasChatTurn()) {
                    if (error.trimmed().isEmpty() && !interrupted && !threadId.isEmpty())
                        for (auto& conversation : settingsController_->MutableSettings().codexConversations)
                            if (conversation.threadId == threadId) {
                                conversation.updatedAt = QDateTime::currentSecsSinceEpoch();
                                conversation.preview = text.simplified().left(160);
                                PopulateAssistantHistory(); SavePersistentSettings(); break;
                            }
                    return;
                }
                SetAssistantBusy(false);
                // The runtime has already cleared its turn state; resync so a
                // finished temporary turn releases Stop while an unrelated
                // persistent chat turn leaves the button alone.
                syncExplainBusy();
                if (!persistent) {
                    // A completed Explain moves its listed conversation to the
                    // top and previews the latest explanation.
                    if (!threadId.isEmpty() &&
                        threadId == assistiveManager_->Runtime().ExplainConversationId() &&
                        error.trimmed().isEmpty() && !interrupted) {
                        for (settings::CodexConversation& conversation :
                             settingsController_->MutableSettings().codexConversations) {
                            if (conversation.threadId == threadId) {
                                conversation.updatedAt = QDateTime::currentSecsSinceEpoch();
                                conversation.preview = text.simplified().left(160);
                                PopulateAssistantHistory();
                                SavePersistentSettings();
                                break;
                            }
                        }
                    }
                    return;
                }
                if (assistantResponseOpen_) {
                    QScrollBar* scroll = uiState_->assistantTranscript_->verticalScrollBar();
                    const int scrollPosition = scroll->value();
                    const bool followTail = !uiState_->assistantTranscript_->textCursor().hasSelection() &&
                                            scrollPosition >= scroll->maximum() - 2;
                    QTextCursor cursor(uiState_->assistantTranscript_->document());
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
                    scroll->setValue(followTail ? scroll->maximum() : scrollPosition);
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

    const auto& savedCamera = settingsController_->MutableSettings();
    automaticPhoneSelectionAllowed_ =
        savedCamera.cameraSymbolicLink.startsWith(QStringLiteral("okuflow-phone:")) ||
        !savedCamera.cameraSelectionExplicit;
    preferredPhoneCamera_ = savedCamera.cameraSymbolicLink.startsWith(QStringLiteral("okuflow-phone:"))
        ? savedCamera.cameraSymbolicLink : QString();
    const int initialCameraIndex = ResolveSavedCameraIndex(cameras_,
        savedCamera.cameraSymbolicLink.toStdWString(), savedCamera.cameraIndex);
    if (!cameras_.empty()) {
        auto blocker = uiState_->BlockSignals(uiState_->cameraCombo_);
        uiState_->cameraCombo_->setCurrentIndex(initialCameraIndex);
        if (!startupSynchronousProfile_) {
            // All app services/settings now exist. Open the camera while show()
            // initializes the native presenter; completion still returns via Qt.
            StartCameraCapture(static_cast<size_t>(initialCameraIndex),
                cameras_[static_cast<size_t>(initialCameraIndex)].phoneSerial.empty(), false, true);
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
    controlTarget_ = std::make_unique<agent::AppControlTarget>(*this);
    controlService_ = std::make_unique<agent::AppControlService>(*controlTarget_,agent::BuiltControlCatalogEdition());
    InitializeAgentControl();
    auto* phoneDiscoveryPulse = new QTimer(this);
    phoneDiscoveryPulse->setInterval(100);
    connect(phoneDiscoveryPulse, &QTimer::timeout, this, &OkuFlowApp::PollPhoneCameraDiscovery);
    phoneDiscoveryPulse->start();
    return true;
}

OkuFlowApp::~OkuFlowApp() {
    CancelAgentTurn();
    if (visibleChatSaveTimer_) visibleChatSaveTimer_->stop();
    PersistVisibleChat();
    agentIpc_.reset();
    agentHttp_.reset();
    agentCodex_.reset();
    controlService_.reset();
    controlTarget_.reset();
    phoneDiscoveryCanceled_.store(true);
    if (phoneDiscovery_.valid()) phoneDiscovery_.wait();
    WriteStartupProfile();
    if (cameraStartupPending_) {
        StopCameraCapture(true);
    }
    if (settingsController_ && uiState_ && assistiveManager_) {
        SavePersistentSettings();
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
    // Discovery is canceled above and capture has stopped: terminate only
    // the private adb server this process owns. A borrowed server (already
    // running on the default endpoint or explicitly overridden) is left
    // exactly as found.
    ShutdownOwnedAdbServer();
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

int OkuFlowApp::Run() {
    return initialized_ && qtApp_ ? qtApp_->exec() : -1;
}

void OkuFlowApp::InitializePlatform() {
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

void OkuFlowApp::ResolveCudaBufferFormatFromOptions() {
    cudaBufferFormat_ = CudaBufferFormat::kRgba8;

    bool ok = false;
    const QByteArray envValue = qgetenv("OKUFLOW_CUDA_BUFFER_FORMAT");
    if (!envValue.isEmpty()) {
        const auto parsed = ParseCudaBufferFormatToken(QString::fromUtf8(envValue), &ok);
        if (ok) {
            cudaBufferFormat_ = parsed;
        } else {
            qWarning() << "Ignoring unknown OKUFLOW_CUDA_BUFFER_FORMAT value" << envValue;
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


} // namespace okuflow

#endif // _WIN32
