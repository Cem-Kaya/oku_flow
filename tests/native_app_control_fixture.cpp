#include "../src/app/app_internal.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/agent/receipt_labels.hpp"
#include "okuflow/app/settings_controller.hpp"
#include "okuflow/app/ui_state_manager.hpp"
#include "okuflow/app/user_data_paths.hpp"
#include "okuflow/app/assistive_feature_manager.hpp"
#include "okuflow/app/interaction_controller.hpp"
#include "okuflow/app/pipeline_orchestrator.hpp"
#include "okuflow/app/recording_manager.hpp"
#include "okuflow/app/language_manager.hpp"
#include "okuflow/ui/main_window.hpp"
#include "okuflow/ui/render_widget.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "okuflow/ui/video_transport.hpp"
#include "okuflow/ui/agent_control_panel.hpp"
#include "okuflow/common/codex_agent_client.hpp"
#include "okuflow/common/uct_device_image.hpp"
#include "okuflow/common/video_playback.hpp"
#include "okuflow/common/video_recap.hpp"
#include "okuflow/mcp/app_ipc_endpoint.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLayout>
#include <QGridLayout>
#include <QMap>
#include <QPushButton>
#include <QScopeGuard>
#include <QSlider>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <QElapsedTimer>
#include <QtTest>
#include "ultra_clear_text_test_support.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>

#if defined(OKUFLOW_VIDEO_DOCUMENT_TESTS)
namespace okuflow {
// An idle worker stays asleep: these fixtures model only an already pending
// session-finalization receipt, never a codec/camera/adb operation.
struct RecordingManagerTestAccess {
    static void FailEncoderStarts(RecordingManager& manager,
                                  const std::shared_ptr<std::atomic<int>>& attempts) {
        manager.encoderStartOverride_ = [attempts](const QString&, VideoRecorder::Codec) {
            attempts->fetch_add(1);return false;
        };
    }
    static void ClearEncoderOverride(RecordingManager& manager) {manager.encoderStartOverride_={};}
    static void PublishAdmission(RecordingManager& manager) {
        RecordingFrameIdentity identity;
        identity.sequenceNumber=91;identity.captureClock100ns=910'000;
        // This models the exact post-success admission seam, not a codec run.
        manager.PublishFrameAdmission(identity,0);
    }
    static void PendingFinalization(RecordingManager& manager, const RecordingSessionInfo& session) {
        std::lock_guard lock(manager.queueMutex_);
        manager.sessionInfo_ = session;
        manager.state_.store(RecordingState::Finalizing);
    }
    static void ClearPendingFinalization(RecordingManager& manager) {
        std::lock_guard lock(manager.queueMutex_);
        manager.sessionInfo_ = {};
        manager.state_.store(RecordingState::Idle);
    }
};
}
#endif

namespace okuflow::agent {
// This harness intentionally bypasses platform/device/provider bootstrap.
// All controls and transitions below are the real production implementations.
#if defined(OKUFLOW_VIDEO_DOCUMENT_TESTS)
// Metadata-only analysis lease: identity fields only, never read back and
// never a CUDA allocation, so retention policy can run without hardware.
class MetadataOnlyDeviceImage final : public uct::DeviceImage {
public:
    const void* Data() const override {return nullptr;}
    bool WaitReady(void*,QString&) const override {return true;}
    QImage Readback(const std::atomic<bool>&,QString&) const override {return {};}
};
#endif
class NativeControlFixture {
public:
    static void MoveViewportPeers(MainWindow& window,const QPoint& origin) {
        window.MoveSimpleChrome(origin);
    }
    static void Initialize(OkuFlowApp& app,const QString& root) {
        SettingsController::StorageOperations storage;
        storage.readSecret=[](const QString&) {return ProtectedSecretResult{};};
        storage.writeSecret=[](const QString&,const QString&,QString*) {return false;};
        storage.removeSecret=[](const QString&,QString*) {return true;};
        storage.saveSettings=settings::SaveDetailed;
        app.settingsController_=std::make_unique<SettingsController>(root+"/settings.json",std::move(storage));
        auto& settings=app.settingsController_->MutableSettings();
        settings.userDataRoot=root+"/output";
        settings.assistive.vlmApiKey.clear();
        settings.currentConfig.vlmAssistEnabled=false;
        settings.currentConfig.assistiveOverlayEnabled=false;
        app.userDataPaths_=std::make_unique<UserDataPaths>(settings.userDataRoot,root+"/install");
        app.languageManager_=std::make_unique<LanguageManager>(*app.qtApp_,&app);
        app.mainWindow_=std::make_unique<MainWindow>();
        app.uiState_=std::make_unique<UIStateManager>(*app.mainWindow_,app);
        app.pipelineOrchestrator_=std::make_unique<PipelineOrchestrator>(app,PipelineOrchestrator::Callbacks{});
        app.interactionController_=std::make_unique<InteractionController>(app);
        app.assistiveManager_=std::make_unique<AssistiveFeatureManager>(*app.uiState_->renderWidget_,app,
            [](const QString&) {},[] {},*app.userDataPaths_);
        app.recordingManager_=std::make_unique<RecordingManager>(app.uiState_->recordButton_,
            [](const QString&,int) {},[&app](const SavedRecordingSegment& part) {app.OnVideoSegmentSaved(part);},
            *app.userDataPaths_,[&app](const RecordingSessionInfo& session) {app.OnVideoRecordingEnded(session);},
            [&app](const RecordingSessionInfo& session) {app.OnVideoRecordingStarted(session);});
        app.uiState_->microphoneCombo_->addItem("None","__none__");
        app.cameraIntent_.LoadSaved({});
        app.PopulatePresetList();
        app.uiState_->ApplyConfigToUI(settings.currentConfig,true);
        app.InitializeUltraClearText();
        app.controlTarget_=std::make_unique<AppControlTarget>(app);
        app.controlService_=std::make_unique<AppControlService>(*app.controlTarget_,BuiltControlCatalogEdition());
        app.controlService_->SetEnabled(true,true);
#if defined(OKUFLOW_VIDEO_DOCUMENT_TESTS)
        app.InitializeVideoPlayback();
#endif
    }
    static QString PhotoDirectory(OkuFlowApp& app) {
        return app.userDataPaths_->PhotosForDate(QDate::currentDate());
    }
    static void UctPalette(OkuFlowApp& app, const QString& scheme, std::uint64_t generation) {
        app.displayColorLut_ = color_schemes::BuildColorLut(*color_schemes::FindBuiltInColorScheme(scheme));
        app.displayColorLutGeneration_ = generation;
    }
    static auto UctPaletteState(const OkuFlowApp& app) {
        return std::pair{app.displayColorLut_, app.displayColorLutGeneration_};
    }
    static void RestoreUctPalette(OkuFlowApp& app, const std::pair<color_schemes::ColorLut, std::uint64_t>& state) {
        app.displayColorLut_ = state.first; app.displayColorLutGeneration_ = state.second;
    }
    static void UctPaletteReceipt(OkuFlowApp& app, int transform, std::uint64_t generation = 0) {
        app.ApplyUltraClearTextDisplayColors(transform, generation);
    }
    static void SavePhotoFixture(OkuFlowApp& app, const QImage& pixels,
                                 ViewOverlaySnapshot overlays) {
        CapturedFrame original;
        original.width=static_cast<UINT>(pixels.width());
        original.height=static_cast<UINT>(pixels.height());
        original.pixels.resize(static_cast<std::size_t>(pixels.sizeInBytes()));
        std::memcpy(original.pixels.data(),pixels.constBits(),original.pixels.size());
        app.pendingPhotoOverlays_=std::move(overlays);
        app.SaveCapturedPhotoPair(pixels.constBits(),original.width,original.height,original);
    }
    static void SaveDrawingFixture(OkuFlowApp& app, const QImage& pixels,
                                   const ViewOverlaySnapshot& overlays) {
        app.SaveAnnotationSnapshot(pixels.constBits(),pixels.width(),pixels.height(),
            overlays.strokes,overlays.transform,overlays.text,QStringLiteral("Fixture drawing"));
    }
    static UctFrameContext PrimeAnnotation(OkuFlowApp& app) {
        app.CancelAnnotationCaptures();
        UctFrameContext frame;
        frame.cameraActive=true;frame.geometry={{},640,480};
        frame.geometry.settings.sourceEpoch=app.cameraSessionId_;
        frame.requested=frame.geometry.settings;frame.frameSeq=41;
        frame.transform=ComputeViewTransform(640,480,800,600,1,0.5f,0.5f,ViewportFitMode::kFit);
        frame.viewportWidth=800;frame.viewportHeight=600;
        OkuFlowApp::PendingAnnotationCapture capture;
        capture.frame=frame;capture.sceneRevision=app.annotationSceneRevision_;
        capture.transform=frame.transform;
        AnnotationStroke stroke;stroke.points={{0.25,0.25},{0.5,0.5}};capture.strokes={stroke};
        app.annotationCaptureQueue_.push_back(capture);
        app.activeAnnotationCapture_=capture;app.pendingAnnotationReadbackId_=91;
        app.lastAnnotationPresentation_=frame;
        return frame;
    }
    static bool AnnotationGpuSignatureRejectsChangedOutput(OkuFlowApp& app) {
        PrimeAnnotation(app);
        OkuFlowApp::AnnotationGpuPresentation displayed;
        displayed.texture=&app;displayed.width=1280;displayed.height=720;
        displayed.transform=app.annotationCaptureQueue_.front().transform;displayed.superRes=true;
        app.annotationCaptureQueue_.front().presentation=displayed;
        if(!app.AnnotationGpuPresentationMatches(displayed))return false;
        auto candidate=displayed;candidate.superRes=false;
        if(app.AnnotationGpuPresentationMatches(candidate))return false;
        candidate=displayed;candidate.texture=nullptr;
        if(app.AnnotationGpuPresentationMatches(candidate))return false;
        candidate=displayed;candidate.width=640;
        if(app.AnnotationGpuPresentationMatches(candidate))return false;
        candidate=displayed;candidate.transform.sourceX=0.25f;
        if(app.AnnotationGpuPresentationMatches(candidate))return false;
        candidate=displayed;candidate.focusMarker=true;
        return !app.AnnotationGpuPresentationMatches(candidate);
    }
    static bool AnnotationMatches(OkuFlowApp& app,const UctFrameContext& frame) {return app.AnnotationFrameMatches(frame);}
    static void AnnotationSceneReprocessed(OkuFlowApp& app) {++app.annotationSceneRevision_;}
    static bool AnnotationEmpty(OkuFlowApp& app) {
        return app.annotationCaptureQueue_.empty() && !app.activeAnnotationCapture_ &&
            app.pendingAnnotationReadbackId_==0 && !app.lastAnnotationPresentation_;
    }
    // Production submission moves the queued intent into the active slot
    // and starts its one-second receipt deadline. PrimeAnnotation instead
    // leaves the intent in both places with the timer stopped.
    static void SubmitPrimedAnnotationReadback(OkuFlowApp& app) {
        if(!app.annotationCaptureQueue_.empty())app.annotationCaptureQueue_.pop_front();
        app.pendingAnnotationReadbackTimer_.restart();
    }
    static bool AnnotationReadbackPending(OkuFlowApp& app) {
        return app.activeAnnotationCapture_.has_value() && app.pendingAnnotationReadbackId_!=0;
    }
    static bool AnnotationReadbackExpired(OkuFlowApp& app) {
        return !app.activeAnnotationCapture_ && app.pendingAnnotationReadbackId_==0 &&
            app.annotationCaptureQueue_.empty();
    }
    static void ClearAnnotation(OkuFlowApp& app) {app.CancelAnnotationCaptures();}
    static AppControlService& Service(OkuFlowApp& app) {return *app.controlService_;}
    static AppControlTarget& Target(OkuFlowApp& app) {return *app.controlTarget_;}
    static uct::ModelSettings UctModels(OkuFlowApp& app) {return app.mainWindow_->ultraClearTextPanel()->ModelSettings();}
    static void NativeUctModels(OkuFlowApp& app,const uct::ModelSettings& models) {
        app.mainWindow_->ultraClearTextPanel()->SetModelSettings(models);app.OnUltraClearTextModelsChanged();
    }
    static QString DeferredPreferenceOperation(OkuFlowApp& app) {
        auto& operations=app.controlTarget_->operations_;
        const auto id=operations.Create(OperationKind::Recording,"wait-fixture");
        operations.Advance(id,OperationState::Stopping,{{"recordingPreferences",true}});
        return id;
    }
    static void CompletePreferenceOperation(OkuFlowApp& app,const QString& id) {
        app.controlTarget_->operations_.Advance(id,OperationState::Completed,{{"recordingPreferences",true}});
    }
    static QJsonObject NativeState(OkuFlowApp& app) {
        const auto& saved=app.settingsController_->LastCommittedSettings();
        return {{"realTarget",true},{"contrastSlider",app.uiState_->contrastSlider_->value()},
            {"savedContrast",saved?QJsonValue(saved->currentConfig.contrast):QJsonValue()},
            {"savedZoom",saved?QJsonValue(saved->currentConfig.zoomAmount):QJsonValue()},
            {"zoomSlider",app.uiState_->zoomSlider_->value()},
            {"cameraActive",app.cameraActive_},{"cameraCount",int(app.cameras_.size())},
            {"platformInitialized",app.comInitialized_ || app.mfInitialized_},
            {"recording",app.recordingManager_->IsActive()}};
    }
    static settings::AdvancedConfig CurrentImageConfig(OkuFlowApp& app) {return app.CaptureCurrentAdvancedConfig();}
    static void RestoreImageConfig(OkuFlowApp& app,const settings::AdvancedConfig& config) {
        app.ApplyAdvancedConfig(config,true);app.SavePersistentSettings();
    }
    static std::uint64_t ProcessingGeneration(const OkuFlowApp& app) {return app.cudaProcessingGeneration_;}
    static void NativeContrastWithoutAgent(OkuFlowApp& app,int value) {
        auto target=std::move(app.controlTarget_);
        const auto restore=qScopeGuard([&] {app.controlTarget_=std::move(target);});
        app.OnContrastChanged(value);
    }
    static void NativeContrast(OkuFlowApp& app,int value) {
        app.uiState_->contrastSlider_->setValue(value);app.OnContrastChanged(value);
        app.SavePersistentSettings(); // Same trusted native settings commit; no service notice.
    }
    static void NativeZoom(OkuFlowApp& app) {app.SetZoom(1.75,{},false);app.SavePersistentSettings();}
    static void ViewSourceSize(OkuFlowApp& app, const QSize& size) {
        app.processedFrameWidth_=size.width();app.processedFrameHeight_=size.height();app.RefreshZoomRange();
    }
    static void SliderZoom(OkuFlowApp& app,int value) {app.OnZoomAmountChanged(value);}
    static void WheelZoom(OkuFlowApp& app,float steps) {app.HandleKeyboardZoom(steps);}
    static void DisableZoom(OkuFlowApp& app) {app.OnZoomToggled(false);}
    // Mirrors the ownership bookkeeping of camera_select without enumerating,
    // opening or starting any device. The empty plan is the target's own
    // controlled-transition window, so the request is not a user edit.
    static bool RequestTemporarySource(OkuFlowApp& app,const QString& owner) {
        auto& target=*app.controlTarget_;
        const auto before=target.Fields();
        if(!target.BeginFieldPlan({}))return false;
        const bool requested=app.cameraIntent_.RequestTemporary(owner,{"fixture-temporary-source","",false},"fixture-operation");
        if(requested)app.controlService_->OwnedSourceRequested(owner,before);
        target.EndFieldPlan();
        return requested;
    }
    static void CancelAgentTurn(OkuFlowApp& app,const QString& context,bool undo) {app.agentContext_=context;app.CancelAgentTurn(undo);}
    static QString AgentContext(OkuFlowApp& app) {return app.agentContext_;}
    static void PrepareAssistant(OkuFlowApp& app,const QString& model) {
        auto& config=app.settingsController_->MutableSettings().assistive;
        config.aiProvider="codex";config.codexExecutablePath=QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY);
        config.codexModel=model;config.codexReasoningEffort="low";
        config.agentControlEnabled=true;config.externalMcpEnabled=false;
        if(!app.agentCodex_)app.InitializeAgentControl();
        app.StartNewAssistantConversationAs(ChatBackendKind::Explain);
        app.RefreshAgentControlSettings();
    }
    static void SubmitAssistant(OkuFlowApp& app,const QString& prompt=QStringLiteral("Set zoom to 2x")) {
        app.SubmitAssistantPromptText(prompt,false,false);
    }
    static void ReconfigureAssistant(OkuFlowApp& app,const QString& model) {
        app.settingsController_->MutableSettings().assistive.codexModel=model;
        app.RefreshAgentControlSettings();
    }
    static void PrepareHttpAssistant(OkuFlowApp& app,const QString& endpoint) {
        auto& config=app.settingsController_->MutableSettings().assistive;
        config.aiProvider="openai";config.vlmApiUrl=endpoint;
        config.vlmModel="private-http-fixture";config.vlmApiKey.clear();
        config.agentControlEnabled=true;config.externalMcpEnabled=false;
        if(!app.agentCodex_)app.InitializeAgentControl();
        app.StartNewAssistantConversationAs(ChatBackendKind::Explain);
        app.RefreshAgentControlSettings();
    }
    static void RestoreAssistantHistory(OkuFlowApp& app,ChatBackendKind kind) {
        app.assistiveManager_->Runtime().ChatSession().Restore("restored-provider-thread",kind,
            {QJsonObject{{"role","user"},{"text","Old provider question"}},
             QJsonObject{{"role","assistant"},{"text","Old provider answer"}}});
        app.currentAssistantThreadId_="restored-provider-thread";
    }
    static QString AssistantProviderThread(const OkuFlowApp& app) {return app.currentAssistantThreadId_;}
    static void TestAssistantTools(OkuFlowApp& app) {app.TestAgentCapability();}
    static void StopAssistant(OkuFlowApp& app) {app.StopAssistantRequest();}
    static bool AssistantPending(const OkuFlowApp& app) {return app.pendingAgentSubmission_.has_value();}
    static CodexAgentClient& AgentClient(OkuFlowApp& app) {return *app.agentCodex_;}
    static AssistiveRuntime& Runtime(OkuFlowApp& app) {return app.assistiveManager_->Runtime();}
    static bool UctRequested(const OkuFlowApp& app) {return app.ultraClearText_->IsEnabledByUser();}
    static bool UctReady(const OkuFlowApp& app) {
        return app.ultraClearText_->BackendStatusSnapshot().state==uct::BackendState::kReady;
    }
    static void UseFixtureUctRecognizer(OkuFlowApp& app) {
        class FixtureRecognizer final:public uct::Recognizer {
        public:
            uct::BackendStatus Initialize() override {
                uct::BackendStatus status;status.state=uct::BackendState::kReady;
                status.name="private UCT fixture";return status;
            }
            uct::RecognitionOutput Recognize(const QImage&,const std::atomic<bool>&) override {
                uct::RecognitionOutput output;output.ok=true;return output;
            }
        };
        app.ultraClearText_->SetEnabled(false);
        app.ultraClearText_->ReplaceRecognizerFactory([] {return std::make_unique<FixtureRecognizer>();});
    }
    static void RestoreUctRecognizer(OkuFlowApp& app) {
        app.ultraClearText_->SetEnabled(false);
        const auto models=app.settingsController_->Settings().ultraClearTextModels;
        app.ultraClearText_->ReplaceRecognizerFactory([models] {return uct::CreateDefaultRecognizer(models);});
        app.settingsController_->MutableSettings().ultraClearTextEnabled=false;
        app.SavePersistentSettings();
    }
    static bool StartBlockedCamera(OkuFlowApp& app,CameraSourceOrigin origin,
        std::function<bool(const std::atomic<bool>&)> start) {
        CameraDescriptor camera;camera.name=L"Private startup fixture";camera.symbolicLink=L"fixture-physical-camera";
        app.cameras_.push_back(std::move(camera));app.cameraStartForTest_=std::move(start);
        return app.StartCameraCapture(app.cameras_.size()-1,false,false,false,origin);
    }
    static void StopCamera(OkuFlowApp& app) {app.StopCameraCapture();}
    static void BeginReconnect(OkuFlowApp& app) {app.pipelineOrchestrator_->BeginCameraReconnect(QDateTime::currentMSecsSinceEpoch()-3000);}
    static bool ReconnectDue(OkuFlowApp& app) {return app.pipelineOrchestrator_->CameraReconnectDue(QDateTime::currentMSecsSinceEpoch());}
    static void LoseActiveReconnect(OkuFlowApp& app) {app.BeginCameraReconnect();}
    static bool CameraActive(OkuFlowApp& app) {return app.cameraActive_;}
    static void DriveReconnect(OkuFlowApp& app) {app.DriveCameraReconnect();}
    static void ExpireReconnect(OkuFlowApp& app) {
        app.pipelineOrchestrator_->CancelCameraReconnect();
        app.pipelineOrchestrator_->BeginCameraReconnect(QDateTime::currentMSecsSinceEpoch()-30001);
    }
    static void PresentedCameraFrame(OkuFlowApp& app) {
        app.cameraFramePresented_=true;
        app.cameraIntent_.MarkFirstFrame(app.startingCameraIntentGeneration_);
        app.RecordStartupFirstPresent();
    }
    static bool ReconnectPending(OkuFlowApp& app) {return app.pipelineOrchestrator_->IsCameraReconnectPending();}
    static int ReconnectAttempts(OkuFlowApp& app) {return app.pipelineOrchestrator_->CameraReconnectAttempt();}
    static bool CameraPending(OkuFlowApp& app) {
        return app.cameraStartupPending_ || app.cameraStopPending_ || app.startupWorkers_->active.load()!=0;
    }
    static void ResetCameraFixture(OkuFlowApp& app) {
        app.pipelineOrchestrator_->CancelCameraReconnect();
        app.cameraStartForTest_={};app.cameras_.clear();app.selectedCameraIndex_=-1;
        app.currentCameraAccelerationKey_.clear();app.cameraIntent_.LoadSaved({});
    }
    static std::optional<RecordingSessionInfo> RecordWithMicrophone(OkuFlowApp& app,bool selected,
        std::function<bool(const std::atomic<bool>&)> start={},std::function<void()> stop={}) {
        app.cameraActive_=true;app.microphoneStartForTest_=std::move(start);app.microphoneStopForTest_=std::move(stop);
        if(selected) {
            AudioDeviceDescriptor microphone;microphone.name=L"Private microphone fixture";
            microphone.endpointId=L"fixture-microphone";app.microphones_.push_back(std::move(microphone));
            app.uiState_->microphoneCombo_->addItem("Private microphone fixture","fixture-microphone");
            app.uiState_->microphoneCombo_->setCurrentIndex(app.uiState_->microphoneCombo_->count()-1);
        } else app.uiState_->microphoneCombo_->setCurrentIndex(0);
        return app.RequestRecording(RecordingAudioRequest::SavedMicrophone,RecordingOwnerKind::Manual,{});
    }
    static void StopMicrophoneRecording(OkuFlowApp& app) {
        if(const auto session=app.recordingManager_->CurrentSessionInfo())app.StopRecording(session->id);
        else app.StopMicrophoneCapture();
    }
    static bool RecordingRetired(OkuFlowApp& app) {return !app.recordingManager_->CurrentSessionInfo().has_value();}
    static bool AudioOpening(OkuFlowApp& app) {return !app.pendingMicrophoneRecording_.isEmpty();}
    static bool MicrophoneUnsafe(OkuFlowApp& app) {return app.MicrophoneTeardownUnsafe();}
    static void ShortMicrophoneDeadline(OkuFlowApp& app) {app.microphoneDeadlineMs_=30;}
    static std::optional<RecordingSessionInfo> Recording(OkuFlowApp& app) {return app.recordingManager_->CurrentSessionInfo();}
    static bool MicrophoneRetired(OkuFlowApp& app) {return !app.microphoneWorker_ || app.microphoneWorker_->done.load();}
    static bool MicrophoneStartupFinished(OkuFlowApp& app) {return app.microphoneWorker_ && app.microphoneWorker_->startupFinished.load();}
    static void ResetMicrophoneFixture(OkuFlowApp& app) {
        app.microphoneStartForTest_={};app.microphoneStopForTest_={};app.microphones_.clear();app.microphoneDeadlineMs_=3000;
        app.uiState_->microphoneCombo_->setCurrentIndex(0);while(app.uiState_->microphoneCombo_->count()>1)app.uiState_->microphoneCombo_->removeItem(1);
        app.cameraActive_=false;
    }
    static int Exec(OkuFlowApp& app) {return app.qtApp_->exec();}
#if defined(OKUFLOW_VIDEO_DOCUMENT_TESTS)
    static std::optional<RecordingSessionInfo> StartSilentVideoRecording(OkuFlowApp& app) {
        app.cameraActive_=true;
        return app.RequestRecording(RecordingAudioRequest::None,RecordingOwnerKind::Manual,{});
    }
    static void ForceEncoderStartFailure(OkuFlowApp& app,const std::shared_ptr<std::atomic<int>>& attempts) {
        RecordingManagerTestAccess::FailEncoderStarts(*app.recordingManager_,attempts);
    }
    static void SubmitVideoOriginal(OkuFlowApp& app) {
        CapturedFrame frame;frame.width=frame.height=64;frame.pixels.assign(64*64*4,71);
        frame.identity.sequenceNumber=91;frame.identity.captureClock100ns=910'000;
        app.recordingManager_->AddOriginalFrame(std::move(frame));
    }
    static void PublishVideoAdmission(OkuFlowApp& app) {
        RecordingManagerTestAccess::PublishAdmission(*app.recordingManager_);
    }
    static std::optional<ActiveRecordingTimeline> VideoTimeline(const OkuFlowApp& app) {
        return app.recordingManager_->CurrentTimeline();
    }
    static void StopVideoRecording(OkuFlowApp& app,const QString& session) {app.StopRecording(session);}
    static void CleanupVideoRecording(OkuFlowApp& app) {
        if(const auto session=app.recordingManager_->CurrentSessionInfo())app.StopRecording(session->id);
        QElapsedTimer timer;timer.start();
        while(app.recordingManager_->CurrentSessionInfo() && timer.elapsed()<3000) {
            QCoreApplication::processEvents();QTest::qWait(1);
        }
        if(!app.recordingManager_->CurrentSessionInfo())
            RecordingManagerTestAccess::ClearEncoderOverride(*app.recordingManager_);
        app.cameraActive_=false;
    }
    static void SeedCommittedVideoSave(OkuFlowApp& app) {app.SeedCommittedVideoSaveForTest();}
    static void SaveVideo(OkuFlowApp& app) {app.SaveSelectedVideoFrame();}
    static QJsonObject VideoState(const OkuFlowApp& app) {return app.SelectedVideoState();}
    static void OpenVideo(OkuFlowApp& app,const QString& path) {app.OpenVideoFile(path);}
    static QString PendingVideoOpen(const OkuFlowApp& app) {return app.PendingVideoOpenForTest();}
    static void PendingVideoFinalization(OkuFlowApp& app,const RecordingSessionInfo& session) {
        app.OnVideoRecordingStarted(session);
        RecordingManagerTestAccess::PendingFinalization(*app.recordingManager_,session);
    }
    static void VideoEnded(OkuFlowApp& app,const RecordingSessionInfo& session) {app.OnVideoRecordingEnded(session);}
    static void ClearVideoFinalization(OkuFlowApp& app) {RecordingManagerTestAccess::ClearPendingFinalization(*app.recordingManager_);}
    static void SeedVideo(OkuFlowApp& app,const RecordingSessionInfo& session,
                          const std::shared_ptr<const VideoPlaybackFrame>& frame,bool recording,bool playing) {
        app.SeedVideoDocumentForTest(session,frame,recording,playing);
    }
    static void VideoStopping(OkuFlowApp& app,const RecordingSessionInfo& session) {app.OnVideoRecordingStopping(session);}
    static bool RefusePendingLiveHold(OkuFlowApp& app, bool finishing, bool cameraActive) {
        app.SeedPendingLiveHoldForTest(finishing);
        app.cameraActive_=cameraActive;
        app.cameraFramePresented_=cameraActive; // Explicit successful display receipt for this synthetic live/lost row.
        return app.AcceptLiveMediaHold({},0,0,1,10'000);
    }
    static void VideoSaved(OkuFlowApp& app,const SavedRecordingSegment& part) {app.OnVideoSegmentSaved(part);}
    static bool VideoTimerActive(const OkuFlowApp& app) {return app.videoPlaybackTimer_ && app.videoPlaybackTimer_->isActive();}
    static VideoTransport::Snapshot VideoUi(const OkuFlowApp& app) {return app.mainWindow_->videoTransport()->snapshot();}
    // Metadata-only polling seam; no synthetic segment is opened or published.
    static void ApplyRecapSnapshot(OkuFlowApp& app,const VideoRecapSnapshot& snapshot) {app.PollLiveRecapForTest(snapshot);}
    static MainWindow& Window(OkuFlowApp& app) {return *app.mainWindow_;}
    static bool RememberDistinctCameraSettings(OkuFlowApp& app) {
        const auto previous = app.settingsController_->Settings();
        const auto key = app.currentCameraAccelerationKey_;
        const auto turns = app.rotationQuarterTurns_;
        const auto restore = qScopeGuard([&] {
            app.settingsController_->MutableSettings() = previous;
            app.currentCameraAccelerationKey_ = key; app.rotationQuarterTurns_ = turns;
        });
        auto& saved = app.settingsController_->MutableSettings();
        app.currentCameraAccelerationKey_ = "private-camera-A";
        saved.cameraFormatStableId = "A-mode"; app.rotationQuarterTurns_ = 1;
        app.RememberCameraWorkingSet();
        app.currentCameraAccelerationKey_ = "private-camera-B";
        saved.cameraFormatStableId = "B-mode"; app.rotationQuarterTurns_ = 3;
        app.RememberCameraWorkingSet();
        const auto a = saved.cameraWorkingSets.value("private-camera-A");
        const auto b = saved.cameraWorkingSets.value("private-camera-B");
        return a.formatStableId == "A-mode" && a.rotationQuarterTurns == 1 &&
            b.formatStableId == "B-mode" && b.rotationQuarterTurns == 3;
    }
    static bool CancelVideoPickerRestoresPhoneLight(OkuFlowApp& app,bool phone) {
        const auto previous=app.cameras_;const auto selected=app.selectedCameraIndex_;
        const auto cleanup=qScopeGuard([&]{app.cameras_=previous;app.selectedCameraIndex_=selected;app.UpdatePhoneLightUi();});
        CameraDescriptor camera;camera.name=L"Fixture source";if(phone)camera.phoneSerial="fixture-only";
        app.cameras_={camera};app.selectedCameraIndex_=0;
        app.mainWindow_->setCameraDevices({"Fixture source"},0);app.PollVideoPlayback();app.UpdatePhoneLightUi();
        if(app.mainWindow_->phoneLightButton()->isHidden()==phone)return false;
        app.mainWindow_->setVideoSourceSelected(true,0);
        if(!app.mainWindow_->phoneLightButton()->isHidden())return false;
        app.PollVideoPlayback(); // same cancellation/failure reconciliation, no dialog/device
        return !app.mainWindow_->videoSourceSelected() && app.mainWindow_->phoneLightButton()->isHidden()!=phone;
    }
    static void VideoOrientation(OkuFlowApp& app,int turns) {
        app.uiState_->rotationCombo_->setCurrentIndex(turns);
        app.OnRotationSelectionChanged(turns);
    }
    static int VideoRecipeOrientation(const OkuFlowApp& app) {
        return static_cast<int>(app.SelectedMediaSceneRecipe(*app.SelectedVideoFrameForTest())[OkuFlowApp::kMediaRecipeRotation]);
    }
    static int CameraOrientation(const OkuFlowApp& app) {return app.CameraOriginalRotationQuarterTurns();}
    static int SavedOrientation(const OkuFlowApp& app) {return app.settingsController_->Settings().rotationQuarterTurns;}
    static void ReturnVideo(OkuFlowApp& app) {app.ReturnVideoToCamera();}
    static bool VideoStartAllowed(const OkuFlowApp& app) {return app.BeforeRequestRecording();}
    static void SetCameraActive(OkuFlowApp& app,bool active) {app.cameraActive_=active;}
    static std::shared_ptr<const VideoPlaybackFrame> HeldVideoFrame(const OkuFlowApp& app) {
        return app.HeldVideoFrameForTest();
    }
    static const std::optional<PresentedOriginalCache::Frame>& PresentedOriginal(const OkuFlowApp& app) {
        return app.presentedOriginalCache_.Displayed();
    }
    static settings::ViewportFitModeSetting VideoFitMode(const OkuFlowApp& app) {
        return app.pipelineOrchestrator_->ViewportFitMode();
    }
    static void RestoreVideoFitMode(OkuFlowApp& app,settings::ViewportFitModeSetting mode) {
        app.SetViewportFitMode(mode,false);
    }
    static bool MediaViewportAdmissionAvoidsRepeatedUploads() {
        OkuFlowApp::MediaViewportAdmission admission;
        OkuFlowApp::MediaViewportAdmission::Stamp stamp;
        stamp.recipe[0]=17;stamp.recipe[1]=71;stamp.width=1280;stamp.height=720;
        stamp.transform=ComputeViewTransform(32,16,stamp.width,stamp.height,2,0.25f,0.75f,ViewportFitMode::kFill);
        int uploads=0;
        const auto poll=[&](bool dirty=false,bool presenterNeeds=false) {
            if(admission.NeedsPresentation(stamp,dirty,presenterNeeds)) {
                ++uploads;admission.Presented(stamp);
            }
        };
        for(int i=0;i<120;++i)poll();
        if(uploads!=1)return false; // two seconds of unchanged 60-Hz polling
        ++stamp.recipe[1];poll();if(uploads!=2)return false; // selected frame
        ++stamp.recipe[7];poll();if(uploads!=3)return false; // appearance
        stamp.transform.sourceX+=0.01f;poll();if(uploads!=4)return false; // pan
        ++stamp.width;poll();if(uploads!=5)return false; // native resize
        stamp.focusMarker=true;stamp.focusX=0.3f;poll();if(uploads!=6)return false;
        poll(true);poll(false,true);if(uploads!=8)return false; // explicit dirty/presenter demand
        const auto displayed=stamp;
        ++stamp.recipe[1]; // a failed/deferred draw must not publish a receipt
        return admission.NeedsPresentation(stamp,false,false) &&
            admission.NeedsPresentation(stamp,false,false) &&
            !admission.NeedsPresentation(displayed,false,false);
    }
    static bool RetiredMediaHoldCopiesStayOutOfViewportRouting(OkuFlowApp& app) {
        const auto active=app.mediaHoldReadbackId_;
        const auto retired=app.retiredMediaHoldReadbackId_;
        const auto activeRetired=app.mediaHoldReadbackRetired_;
        const auto timer=app.mediaHoldReadbackTimer_;
        const auto restore=qScopeGuard([&] {
            app.mediaHoldReadbackId_=active;app.retiredMediaHoldReadbackId_=retired;
            app.mediaHoldReadbackRetired_=activeRetired;
            app.mediaHoldReadbackTimer_=timer;
        });
        app.mediaHoldReadbackId_=101;app.retiredMediaHoldReadbackId_=0;app.mediaHoldReadbackRetired_=false;
        app.RetirePendingMediaHoldReadback(); // cancel/timeout has identical retirement policy
        if(app.mediaHoldReadbackId_!=0 || app.DiscardRetiredMediaHoldReadback(77) ||
            !app.DiscardRetiredMediaHoldReadback(101) || app.DiscardRetiredMediaHoldReadback(101))return false;
        app.mediaHoldReadbackId_=202;app.RetirePendingMediaHoldReadback();
        // Model an invariant violation without dropping either exact id: the
        // fallback active slot is discard-only and prevents another hold.
        app.mediaHoldReadbackId_=303;app.RetirePendingMediaHoldReadback();
        return app.mediaHoldReadbackId_==303 && app.mediaHoldReadbackRetired_ &&
            app.DiscardRetiredMediaHoldReadback(202) &&
            app.DiscardRetiredMediaHoldReadback(303) &&
            app.mediaHoldReadbackId_==0 && app.retiredMediaHoldReadbackId_==0 &&
            !app.mediaHoldReadbackRetired_ && !app.DiscardRetiredMediaHoldReadback(404);
    }
    static std::uint64_t StageDeferredLiveOriginal(OkuFlowApp& app,const RecordingSessionInfo& session,
                                                  bool unsupportedApplied=false,bool pendingAppearance=false,
                                                  int matchingAspectIntent=0) {
        PendingVideoFinalization(app,session);
        auto neutral=app.CaptureCurrentAdvancedConfig();
        neutral.blackWhiteEnabled=false;neutral.blackWhiteThreshold=0.5f;
        neutral.blurEnabled=neutral.temporalSmoothEnabled=neutral.spatialSharpenEnabled=false;
        neutral.debugView=neutral.focusMarker=neutral.stabilizationEnabled=neutral.keystoneEnabled=false;
        neutral.autoContrastEnabled=neutral.autoTextClarityEnabled=neutral.mlSuperResEnabled=false;
        neutral.rotationQuarterTurns=0;neutral.displayColorMode=0;
        const auto* normal=color_schemes::FindBuiltInColorScheme(QStringLiteral("normal"));
        if(!normal)return 0;
        neutral.colorScheme=*normal;
        neutral.contrast=1.0f;neutral.brightness=0.0f;
        app.ApplyAdvancedConfig(neutral,true);
        app.cameraActive_=true;app.cameraFramePresented_=true;
        // Use the same native viewport authority checked by Accept. Offscreen
        // fixtures have no initialized presenter, so derive its current render
        // extent without creating a native window or changing desktop geometry.
        const auto* render=app.uiState_->renderWidget_;
        if(!render)return 0;
        const bool presenterViewport=app.presenter_ && app.presenter_->ViewportWidth() && app.presenter_->ViewportHeight();
        const UINT viewportWidth=presenterViewport
            ? app.presenter_->ViewportWidth() : static_cast<UINT>(std::max(1,qRound(render->width()*render->devicePixelRatioF())));
        const UINT viewportHeight=presenterViewport
            ? app.presenter_->ViewportHeight() : static_cast<UINT>(std::max(1,qRound(render->height()*render->devicePixelRatioF())));
        PresentedOriginalCache::ViewRecipe applied;
        applied.canReproduceCleanView=!unsupportedApplied;
        applied.blackWhite=app.blackWhiteEnabled_;
        applied.blackWhiteThreshold=app.blackWhiteEnabled_?app.blackWhiteThreshold_:0.5f;
        applied.colorTransform=app.displayColorScheme_.id==QStringLiteral("normal")?0:
            app.displayColorScheme_.id==QStringLiteral("inverted")?1:2;
        applied.colorLutGeneration=applied.colorTransform==2?app.displayColorLutGeneration_:0;
        applied.contrast=app.contrast_;applied.brightness=app.brightness_;
        CapturedFrame first;
        first.width=matchingAspectIntent ? viewportWidth : 32;
        first.height=matchingAspectIntent ? viewportHeight : 16;
        first.pixels.assign(std::size_t(first.width)*first.height*4,31);
        first.identity.sequenceNumber=71;first.identity.captureClock100ns=710'000;
        const auto committedMode=matchingAspectIntent==1 ? ViewportFitMode::kFit : ViewportFitMode::kFill;
        const auto view=ComputeViewTransform(first.width,first.height,viewportWidth,viewportHeight,
            matchingAspectIntent ? 1.0f : 2.0f,0.25f,0.75f,committedMode);
        if(!app.presentedOriginalCache_.Stage(first,session.id,app.cameraSessionId_,0,applied))return 0;
        app.presentedOriginalCache_.Presented(app.cameraSessionId_,71,view,true,committedMode,matchingAspectIntent==2);
        auto next=first;next.pixels.assign(next.pixels.size(),191);
        next.identity.sequenceNumber=72;next.identity.captureClock100ns=720'000;
        auto nextRecipe=applied;nextRecipe.canReproduceCleanView=true;
        if(pendingAppearance) {app.OnContrastChanged(140);nextRecipe.contrast=app.contrast_;}
        if(!app.presentedOriginalCache_.Stage(next,session.id,app.cameraSessionId_,0,nextRecipe))return 0;
        if(matchingAspectIntent) {
            app.SetViewportFitMode(committedMode==ViewportFitMode::kFit
                ? settings::ViewportFitModeSetting::Fill : settings::ViewportFitModeSetting::Fit,false);
            app.zoomEnabled_=true;app.zoomAmount_=2.0f;
        }
        app.presentedCameraScene_.reset(); // force the retained clean Original fallback
        // This processed successor received no successful presentation. The
        // mutable pipeline buffers and capture clock have already advanced.
        app.currentCameraFrameSequence_=72;app.currentCameraCaptureClock100ns_=720'000;
        app.cpuSceneBuffer_=next.pixels;app.cpuSceneWidth_=32;app.cpuSceneHeight_=16;
        app.cpuSceneReady_=true;app.usingCudaLastFrame_=false;
        app.uctProvenance_.OnCpuSceneProcessed(app.cameraSessionId_,0,32,16,72,false);
        app.mediaHoldFrameSeq_=0;
        app.ToggleSelectedVideoPlayback();
        return app.mediaHoldFrameSeq_;
    }
    static std::optional<RecordingSessionInfo> DeniedRecording(OkuFlowApp& app) {
        app.cameraActive_=false;
        return app.RequestRecording(RecordingAudioRequest::None,RecordingOwnerKind::Manual,{});
    }
    static void ResetVideo(OkuFlowApp& app) {
        ClearVideoFinalization(app);
        app.presentedOriginalCache_.Reset();
        app.SeedVideoDocumentForTest({},nullptr,false,false);
        app.ReturnVideoToCamera();
    }
    static void SeedRecapSceneWithoutRecording(OkuFlowApp& app,std::vector<std::uint8_t>& pixels,bool unsupported=false,bool deferReceipt=false) {
        app.cameraActive_=true;app.cameraFramePresented_=true;
        app.currentCameraFrameSequence_=91;app.currentCameraCaptureClock100ns_=9'100'000;
        MediaFrame source;source.width=unsupported?8192:32;source.height=unsupported?4320:16;source.subtype=MFVideoFormat_ARGB32;
        source.frameRateNumerator=30;source.frameRateDenominator=1;
        app.ObserveLiveRecapSource(source);
        UctFrameContext context;context.frameSeq=91;context.geometry.settings.sourceEpoch=app.cameraSessionId_;app.lastAnnotationPresentation_=context;
        app.pendingSceneCaptureClock100ns_=9'100'000;
        const auto transform=ComputeViewTransform(32,16,1280,720,2,0.4f,0.6f,ViewportFitMode::kFill);
        if(!deferReceipt) {
            app.RetainPresentedCameraScene(app.cameraSessionId_,91,app.pendingSceneCaptureClock100ns_,nullptr,32,16,transform,pixels.data());
            app.pendingSceneCaptureClock100ns_.reset();
        }
        app.PollVideoPlayback();
    }
    static void ResetRecap(OkuFlowApp& app){app.ResetLiveRecap();}
    static void OversizedPresentedScene(OkuFlowApp& app) {
        ++app.lastAnnotationPresentation_->frameSeq;
        const auto transform=ComputeViewTransform(8192,4320,1280,720,1,0.5f,0.5f,ViewportFitMode::kFill);
        app.RetainPresentedCameraScene(app.cameraSessionId_,app.lastAnnotationPresentation_->frameSeq,
            std::nullopt,nullptr,8192,4320,transform,nullptr);
        app.PollVideoPlayback();
    }
    static void AdvanceRecapIngress(OkuFlowApp& app) {
        app.currentCameraFrameSequence_=92;app.currentCameraCaptureClock100ns_=9'200'000;
    }
    static void CommitRecapReceipt(OkuFlowApp& app,const std::uint8_t* pixels,std::uint64_t generation) {
        const auto transform=ComputeViewTransform(32,16,1280,720,2,0.4f,0.6f,ViewportFitMode::kFill);
        app.RetainPresentedCameraScene(app.cameraSessionId_,app.lastAnnotationPresentation_->frameSeq,
            app.pendingSceneCaptureClock100ns_,nullptr,32,16,transform,pixels,generation);
        app.pendingSceneCaptureClock100ns_.reset(); // production consumes latency only after success
        app.PollVideoPlayback();
    }
    static QJsonObject RecapCaptureStamp(const OkuFlowApp& app) {
        const auto& stamp=app.presentedCameraCapture_;
        return {{"sequence",static_cast<qint64>(stamp?stamp->sequence:0)},
            {"clock",static_cast<qint64>(stamp?stamp->clock100ns:-1)},
            {"hasPixels",app.presentedCameraScene_.has_value()}};
    }
    static void ForgetPendingRecapClock(OkuFlowApp& app){app.pendingSceneCaptureClock100ns_.reset();}
    static void SetRecapMinutes(OkuFlowApp& app,int minutes) {
        auto* combo=app.mainWindow_->liveRecapMinutesCombo();combo->setCurrentIndex(combo->findData(minutes));
    }
    static QJsonObject RecapPreferences(const OkuFlowApp& app) {
        const auto& s=app.settingsController_->Settings();
        return {{"minutes",s.liveRecapMinutes},{"zoom",s.currentConfig.zoomAmount},{"center",s.currentConfig.zoomCenterX},
            {"camera",s.cameraSymbolicLink},{"format",s.cameraFormatStableId},{"microphone",s.microphoneEndpointId}};
    }
    static void SeekVideo(OkuFlowApp& app,qint64 positionMs) {app.SeekSelectedVideo(positionMs);}
    static void ToggleVideo(OkuFlowApp& app) {app.ToggleSelectedVideoPlayback();}
    static void JumpVideo(OkuFlowApp& app) {app.JumpSelectedVideoToEnd();}
    static std::shared_ptr<const VideoPlaybackFrame> SelectedVideoFrame(const OkuFlowApp& app) {
        return app.SelectedVideoFrameForTest();
    }
    static std::shared_ptr<const VideoPlaybackFrame> HoldPresentedVideoFrame(OkuFlowApp& app,
        const std::shared_ptr<const VideoPlaybackFrame>& presented) {return app.HoldPresentedVideoFrameForTest(presented);}
    static VideoPlaybackSnapshot DecoderSnapshot(const OkuFlowApp& app) {return app.VideoPlaybackSnapshotForTest();}
    // Same entry used by an actual app-side media CUDA scene failure.
    static void MediaSceneFailed(OkuFlowApp& app,const std::string& reason) {app.RequestMediaCpuFallback(reason);}
    static bool HasMediaGpuScene(const OkuFlowApp& app) {return static_cast<bool>(app.mediaGpuScene_);}
    // Owned lease metadata only; no CUDA call ever touches this image.
    static uct::SharedDeviceImage MetadataAnalysis(const OkuFlowApp& app,std::uint64_t frameSequence,
                                                   std::uint64_t appearance) {
        auto image=std::make_shared<MetadataOnlyDeviceImage>();
        image->width=32;image->height=16;image->pitch=128;
        image->sourceEpoch=app.mediaDocumentEpoch_;image->frameSequence=frameSequence;
        image->processingGeneration=appearance;
        return image;
    }
    // Window-free selected-media GPU admission. MediaGpuScene stamps a processed
    // scene with exactly this epoch/frame/appearance identity.
    static bool SameExtentRotationReprocessesWithUctOff(OkuFlowApp& app) {
        const auto rotation=app.rotationQuarterTurns_;
        const auto restore=qScopeGuard([&] {
            app.rotationQuarterTurns_=rotation;app.InvalidateMediaSelection();
            app.SelectedMediaGpuSceneProcessed(false,{},0);
        });
        app.InvalidateMediaSelection();
        app.rotationQuarterTurns_=0;
        VideoPlaybackFrame frame;frame.width=32;frame.height=16;frame.strideBytes=128;frame.sampleId=7;
        if(!app.AdmitSelectedMediaSceneRecipe(app.SelectedMediaSceneRecipe(frame)))return false;
        app.SelectedMediaGpuSceneProcessed(false,{},1000);
        const OkuFlowApp::MediaGpuSceneStamp upright{true,app.mediaDocumentEpoch_,7,app.mediaSceneAppearanceRevision_,false};
        // Two seconds of unchanged 60-Hz paused polling reuse the processed scene.
        for(std::int64_t now=1000;now<3000;now+=8)
            if(app.AdmitSelectedMediaSceneRecipe(app.SelectedMediaSceneRecipe(frame)) ||
               app.SelectedMediaGpuSceneNeedsProcess(upright,7,false,now))return false;
        app.rotationQuarterTurns_=2; // 180 degrees keeps the 32x16 extent
        const auto turned=app.SelectedMediaSceneRecipe(frame);
        if(turned[OkuFlowApp::kMediaRecipeRotation]!=2 || !app.AdmitSelectedMediaSceneRecipe(turned))return false;
        if(app.mediaSceneWidth_!=32 || app.mediaSceneHeight_!=16)return false;
        if(!app.SelectedMediaGpuSceneNeedsProcess(upright,7,false,3000))return false;
        app.SelectedMediaGpuSceneProcessed(false,{},3000);
        const OkuFlowApp::MediaGpuSceneStamp rotated{true,app.mediaDocumentEpoch_,7,app.mediaSceneAppearanceRevision_,false};
        if(app.SelectedMediaGpuSceneNeedsProcess(rotated,7,false,3016))return false;
        app.rotationQuarterTurns_=1; // a quarter turn swaps the processed extent
        if(!app.AdmitSelectedMediaSceneRecipe(app.SelectedMediaSceneRecipe(frame)))return false;
        return app.mediaSceneWidth_==16 && app.mediaSceneHeight_==32 &&
            app.SelectedMediaGpuSceneNeedsProcess(rotated,7,false,3032);
    }
    static bool ValidUctLeaseSurvivesViewReplays(OkuFlowApp& app) {
        const auto restore=qScopeGuard([&] {app.InvalidateMediaSelection();app.SelectedMediaGpuSceneProcessed(false,{},0);});
        app.InvalidateMediaSelection();
        VideoPlaybackFrame frame;frame.width=32;frame.height=16;frame.strideBytes=128;frame.sampleId=21;
        if(!app.AdmitSelectedMediaSceneRecipe(app.SelectedMediaSceneRecipe(frame)))return false;
        const auto lease=MetadataAnalysis(app,21,app.mediaSceneAppearanceRevision_);
        app.SelectedMediaGpuSceneProcessed(true,lease,1000);
        if(app.mediaGpuAnalysis_!=lease || app.mediaGpuAnalysisAttempts_!=0)return false;
        OkuFlowApp::MediaGpuSceneStamp stamp{true,app.mediaDocumentEpoch_,21,app.mediaSceneAppearanceRevision_,false};
        // Two seconds of 125-Hz view replays whose capture throttle yields no
        // fresh lease: the valid same-scene lease survives every replay and
        // its untouched retry budget never demands another whole-frame pass.
        for(std::int64_t now=1000;now<3000;now+=8) {
            // ROI sharpening/SR can replay the same selected sample and
            // return a new allocation. Allocation identity is not a new
            // selected image: keep the lease already owned by UCT.
            const auto replay=MetadataAnalysis(app,21,app.mediaSceneAppearanceRevision_);
            app.SelectedMediaGpuSceneProcessed(true,replay,now);
            if(app.mediaGpuAnalysis_!=lease || app.mediaGpuAnalysisAttempts_!=0 ||
               app.SelectedMediaGpuSceneNeedsProcess(stamp,21,true,now))return false;
        }
        // An incoming lease from another appearance cannot replace the valid
        // current lease, even transiently. A real appearance change invalidates
        // the old one immediately, before the next overlay admission.
        const auto stale=MetadataAnalysis(app,21,app.mediaSceneAppearanceRevision_+1);
        app.SelectedMediaGpuSceneProcessed(true,stale,2999);
        if(app.mediaGpuAnalysis_!=lease || app.mediaGpuAnalysisAttempts_!=0)return false;
        ++app.mediaSceneAppearanceRevision_;
        stamp.appearance=app.mediaSceneAppearanceRevision_;
        app.SelectedMediaGpuSceneProcessed(true,lease,3000);
        if(app.mediaGpuAnalysis_ || app.mediaGpuAnalysisAttempts_!=1)return false;
        // Replays before the 80-ms retry deadline spend no further attempts.
        for(std::int64_t now=3001;now<3080;now+=16) {
            app.SelectedMediaGpuSceneProcessed(true,{},now);
            if(app.mediaGpuAnalysis_ || app.mediaGpuAnalysisAttempts_!=1 ||
               app.SelectedMediaGpuSceneNeedsProcess(stamp,21,true,now))return false;
        }
        if(!app.SelectedMediaGpuSceneNeedsProcess(stamp,21,true,3080))return false;
        app.SelectedMediaGpuSceneProcessed(true,{},3080);
        if(app.mediaGpuAnalysisAttempts_!=2)return false;
        // The second backoff is 160 ms: still quiet halfway, due at its end.
        if(app.SelectedMediaGpuSceneNeedsProcess(stamp,21,true,3160))return false;
        if(!app.SelectedMediaGpuSceneNeedsProcess(stamp,21,true,3240))return false;
        // A fresh valid lease for this exact scene restores the retry budget.
        const auto restored=MetadataAnalysis(app,21,app.mediaSceneAppearanceRevision_);
        app.SelectedMediaGpuSceneProcessed(true,restored,3240);
        return app.mediaGpuAnalysis_==restored && app.mediaGpuAnalysisAttempts_==0 &&
            !app.SelectedMediaGpuSceneNeedsProcess(stamp,21,true,3240);
    }
    static bool PausedMediaAnalysisRetryIsBoundedPerFrame(OkuFlowApp& app) {
        const auto restore=qScopeGuard([&] {app.InvalidateMediaSelection();app.SelectedMediaGpuSceneProcessed(false,{},0);});
        app.InvalidateMediaSelection();
        VideoPlaybackFrame frame;frame.width=32;frame.height=16;frame.strideBytes=128;frame.sampleId=9;
        if(!app.AdmitSelectedMediaSceneRecipe(app.SelectedMediaSceneRecipe(frame)))return false;
        OkuFlowApp::MediaGpuSceneStamp scene{true,app.mediaDocumentEpoch_,9,app.mediaSceneAppearanceRevision_,false};
        // UCT on, and no pass yields an analysis lease for this paused frame.
        int passes=0;
        for(std::int64_t now=0;now<60'000;now+=16)
            if(app.SelectedMediaGpuSceneNeedsProcess(scene,9,true,now)) {app.SelectedMediaGpuSceneProcessed(true,{},now);++passes;}
        if(passes!=OkuFlowApp::kMediaAnalysisAttempts || !app.SelectedMediaAnalysisGivenUp(false))return false;
        // The next selected frame receives a fresh bounded budget.
        frame.sampleId=10;
        if(!app.AdmitSelectedMediaSceneRecipe(app.SelectedMediaSceneRecipe(frame)) || app.SelectedMediaAnalysisGivenUp(false))return false;
        scene.frameSequence=10;scene.appearance=app.mediaSceneAppearanceRevision_;
        if(!app.SelectedMediaGpuSceneNeedsProcess(scene,10,true,60'000))return false;
        app.SelectedMediaGpuSceneProcessed(true,{},60'000);
        // A failed analysis pool stops whole-frame retries at once.
        scene.analysisFailed=true;
        for(std::int64_t now=60'000;now<70'000;now+=16)
            if(app.SelectedMediaGpuSceneNeedsProcess(scene,10,true,now))return false;
        return app.SelectedMediaAnalysisGivenUp(true) && !app.SelectedMediaAnalysisGivenUp(false);
    }
#endif
};
}
using namespace okuflow;
using namespace okuflow::agent;
namespace {
#if defined(OKUFLOW_VIDEO_DOCUMENT_TESTS)
void AviU32(QByteArray& bytes,std::uint32_t value) {
    for(int shift=0;shift<32;shift+=8)bytes.append(static_cast<char>((value>>shift)&255));
}
QByteArray AviChunk(const char* kind,QByteArray data) {
    QByteArray result(kind,4);AviU32(result,static_cast<std::uint32_t>(data.size()));
    result+=data;if(data.size()%2)result.append('\0');return result;
}
QByteArray AviList(const char* kind,const QByteArray& data) {return AviChunk("LIST",QByteArray(kind,4)+data);}
// Three first-party numbered-colour raw frames. No encoder, camera, downloaded
// video, native UI, owner data or external model participates in this fixture.
bool WritePrivateVideoFixture(const QString& path) {
    constexpr std::uint32_t width=32,height=16,frameBytes=width*height*4,count=3;
    QByteArray main;
    for(auto value:{100'000u,frameBytes*10,0u,0x10u,count,0u,1u,frameBytes,width,height,0u,0u,0u,0u})AviU32(main,value);
    QByteArray stream("vidsDIB ",8);AviU32(stream,0);AviU32(stream,0);
    for(auto value:{0u,1u,10u,0u,count,frameBytes,0xffffffffu,0u})AviU32(stream,value);
    stream.append("\0\0\0\0",4);stream.append(char(width));stream.append('\0');stream.append(char(height));stream.append('\0');
    QByteArray format;for(auto value:{40u,width,height})AviU32(format,value);
    format.append("\1\0\40\0",4);for(auto value:{0u,frameBytes,0u,0u,0u,0u})AviU32(format,value);
    const auto header=AviList("hdrl",AviChunk("avih",main)+AviList("strl",AviChunk("strh",stream)+AviChunk("strf",format)));
    QByteArray frames,index;
    for(std::uint32_t frame=0;frame<count;++frame) {
        QByteArray pixels(static_cast<qsizetype>(frameBytes),'\0');
        for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x) {
            const auto offset=static_cast<qsizetype>((y*width+x)*4);
            pixels[offset]=char(30+frame*60);pixels[offset+1]=char(x*7);pixels[offset+2]=char(y*13);pixels[offset+3]=char(255);
        }
        index.append("00db",4);AviU32(index,0x10);AviU32(index,static_cast<std::uint32_t>(frames.size())+4);AviU32(index,frameBytes);
        frames+=AviChunk("00db",pixels);
    }
    const auto riff=AviChunk("RIFF",QByteArray("AVI ",4)+header+AviList("movi",frames)+AviChunk("idx1",index));
    QFile output(path);return output.open(QIODevice::WriteOnly|QIODevice::NewOnly)&&output.write(riff)==riff.size();
}
std::shared_ptr<const VideoPlaybackFrame> OwnedVideoPolicyFrame() {
    auto frame=std::make_shared<VideoPlaybackFrame>();frame->width=32;frame->height=16;frame->strideBytes=128;
    frame->bgra=std::make_shared<const std::vector<std::uint8_t>>(32*16*4,127);frame->sampleId=41;frame->pts100ns=55'000'000;return frame;
}
#endif
// Fault injection changes only the commit outcome; every state read and write
// still reaches the same real production target and native handler.
class FailingNativeCommit final : public IAppControlTarget {
    AppControlTarget& target_;
public:
    explicit FailingNativeCommit(AppControlTarget& target):target_(target) {}
    QJsonObject State() const override {return target_.State();}
    std::map<Field,QJsonValue> Fields() const override {return target_.Fields();}
    ControlPlan Prepare(ToolId tool,const QJsonObject& args,const QString& owner) const override {return target_.Prepare(tool,args,owner);}
    QJsonValue ReadField(const FieldWrite& write) const override {return target_.ReadField(write);}
    bool Apply(const FieldWrite& write) override {return target_.Apply(write);}
    bool BeginFieldPlan(const ControlPlan& plan) override {return target_.BeginFieldPlan(plan);}
    bool CommitFieldPlan(const ControlPlan& plan) override {
        if(!target_.CommitFieldPlan(plan))return false;
        for(const auto& write:plan.writes)if(write.field==Field::ZoomAmount && write.value==2.0) {
            target_.Apply(write);return false;
        }
        return true;
    }
    void SynchronizeNativeState() override {target_.SynchronizeNativeState();}
    void EndFieldPlan() override {target_.EndFieldPlan();}
    bool Save() override {return target_.Save();}
    QJsonObject Query(ToolId tool,const QJsonObject& args) const override {return target_.Query(tool,args);}
    QJsonObject EffectiveValues(const ControlPlan& plan) const override {return target_.EffectiveValues(plan);}
    void Execute(ToolId tool,const QJsonObject& args,const QString& owner,ContextKind kind,ControlCompletion completion) override {
        target_.Execute(tool,args,owner,kind,std::move(completion));
    }
    bool ModalActive() const override {return target_.ModalActive();}
    bool UserInteracting() const override {return target_.UserInteracting();}
    bool RecordingActive() const override {return target_.RecordingActive();}
    QString RecordingSession() const override {return target_.RecordingSession();}
    QString RecordingOwner() const override {return target_.RecordingOwner();}
    void StopOwnedRecording(const QString& owner) override {target_.StopOwnedRecording(owner);}
};
void Output(const QJsonObject& object) {
    const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(),1,bytes.size(),stdout);std::fputc('\n',stdout);std::fflush(stdout);
}
class NativeAcceptance final : public QObject {
    Q_OBJECT
    OkuFlowApp& app_;
    ControlResult Call(const QString& context,const QString& key,const QString& tool,QJsonObject args={}) {
        std::optional<ControlResult> result;
        NativeControlFixture::Service(app_).Submit({context,key,tool,args},[&](ControlResult value){result=std::move(value);});
        QElapsedTimer clock;clock.start();
        while(!result && clock.elapsed()<2000)QCoreApplication::processEvents();
        return result.value_or(ControlResult::Failure("timeout"));
    }
public:
    explicit NativeAcceptance(OkuFlowApp& app):app_(app) {}
private slots:
    void phoneZoomHasNativeNameBoundsAndIndependentValue() {
        auto& window = NativeControlFixture::Window(app_);
        const auto restore = qScopeGuard([&] { window.setPhoneZoomControls(false, false, 1, 1, 1); });
        const auto viewportZoom = window.zoomSlider()->value();
        window.setPhoneZoomControls(true, true, .5f, 20, 3.25f);
        auto* zoom = window.phoneZoomControl();
        QCOMPARE(zoom->minimum(), .5); QCOMPARE(zoom->maximum(), 20.0);
        QCOMPARE(zoom->value(), 3.25); QVERIFY(zoom->isEnabled());
        QCOMPARE(window.zoomSlider()->value(), viewportZoom);
        QWheelEvent wheel(QPointF(5,5), QPointF(5,5), QPoint(), QPoint(0,120), Qt::NoButton,
            Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(zoom, &wheel);
        QCOMPARE(zoom->value(), 3.25);
        QVERIFY(!zoom->accessibleName().isEmpty());
        window.setPhoneZoomControls(true, false, 1, 1, 1);
        QVERIFY(!zoom->isEnabled()); QCOMPARE(zoom->value(), 1.0);
    }
    void cameraSettingsRememberFormatAndOrientationByIdentity() {
        QVERIFY(NativeControlFixture::RememberDistinctCameraSettings(app_));
    }
    void processedPhotoAndDrawingIncludeUctAndInkWithCleanOriginal() {
        uct_test::RegisterFixtureFonts();
        QWidget owner;owner.resize(640,360);
        QWidget viewport(&owner);viewport.setGeometry(owner.rect());
        UltraClearTextOverlay overlay(&viewport,&owner);
        ViewTransform transform;transform.valid=true;
        overlay.SetViewTransform(transform);
        owner.show();viewport.show();
        overlay.SetWords({{QStringLiteral("Clear"),QRectF(.15,.25,.35,.20),
            {QColor(Qt::white),QColor(Qt::black)},1,{},0}});
        QCoreApplication::processEvents();
        AnnotationStroke ink;ink.color=Qt::yellow;ink.sceneWidth=.025;
        ink.points={QPointF(.15,.35),QPointF(.5,.35)};
        ViewOverlaySnapshot overlays{overlay.CaptureSnapshot(transform,owner.size()),{ink},transform};
        QVERIFY(!overlays.text.IsEmpty());
        const QDir folder(NativeControlFixture::PhotoDirectory(app_));
        const auto before=folder.entryList({QStringLiteral("IMG_*.jpg")},QDir::Files);
        QImage base(owner.size(),QImage::Format_ARGB32);base.fill(QColor(82,94,108));
        const auto beforeDrawings=folder.entryList({QStringLiteral("ANNOTATION_*.png")},QDir::Files);
        NativeControlFixture::SavePhotoFixture(app_,base,overlays);
        overlay.ClearWords(); // writer must not consult the current overlay
        QTRY_COMPARE_WITH_TIMEOUT(folder.entryList({QStringLiteral("IMG_*.jpg")},QDir::Files).size(),before.size()+2,5000);
        const auto files=folder.entryList({QStringLiteral("IMG_*.jpg")},QDir::Files);
        QImage original,processed;
        for(const auto& name:files)if(!before.contains(name)) {
            if(name.endsWith(QStringLiteral("_original.jpg")))original.load(folder.filePath(name));
            if(name.endsWith(QStringLiteral("_processed.jpg")))processed.load(folder.filePath(name));
        }
        QVERIFY(!original.isNull());QVERIFY(!processed.isNull());
        QCOMPARE(original.size(),base.size());QCOMPARE(processed.size(),base.size());
        const auto clean=original.pixelColor(200,126);
        QVERIFY(std::abs(clean.red()-82)<4 && std::abs(clean.green()-94)<4 && std::abs(clean.blue()-108)<4);
        bool allOriginalPixelsClean=true;
        for(int y=0;y<original.height();++y)for(int x=0;x<original.width();++x) {
            const auto pixel=original.pixelColor(x,y);
            if(std::abs(pixel.red()-82)>=4 || std::abs(pixel.green()-94)>=4 ||
                std::abs(pixel.blue()-108)>=4)allOriginalPixelsClean=false;
        }
        QVERIFY(allOriginalPixelsClean);
        const auto textPlate=processed.pixelColor(90,100);
        QVERIFY(textPlate.red()<25 && textPlate.green()<25 && textPlate.blue()<25);
        const auto drawn=processed.pixelColor(200,126);
        QVERIFY(drawn.red()>220 && drawn.green()>220 && drawn.blue()<40);
        NativeControlFixture::SaveDrawingFixture(app_,base,overlays);
        QTRY_COMPARE_WITH_TIMEOUT(folder.entryList({QStringLiteral("ANNOTATION_*.png")},QDir::Files).size(),beforeDrawings.size()+1,5000);
        QImage drawing;
        for(const auto& name:folder.entryList({QStringLiteral("ANNOTATION_*.png")},QDir::Files))
            if(!beforeDrawings.contains(name))drawing.load(folder.filePath(name));
        QVERIFY(!drawing.isNull());QCOMPARE(drawing.size(),base.size());
        QCOMPARE(drawing.pixelColor(90,100),QColor(Qt::black));
        QCOMPARE(drawing.pixelColor(200,126),QColor(Qt::yellow));
        QCOMPARE(drawing.pixelColor(10,10),base.pixelColor(10,10));
        int whiteGlyphPixels=0;
        for(int y=85;y<170;++y)for(int x=90;x<330;++x) {
            const auto pixel=drawing.pixelColor(x,y);
            if(pixel.red()>230 && pixel.green()>230 && pixel.blue()>230)++whiteGlyphPixels;
        }
        QVERIFY(whiteGlyphPixels>100);
        QCOMPARE(base.pixelColor(200,126),QColor(82,94,108));
    }
#if defined(OKUFLOW_VIDEO_DOCUMENT_TESTS)
    void videoSourceControlsBelongToCameraSettings() {
        MainWindow window;
        window.setCameraDevices({"Fixture webcam","Fixture phone"},0);
        auto* combo=window.cameraCombo();auto* select=window.openVideoButton();
        QVERIFY(select->parentWidget()==combo->parentWidget());
        QVERIFY(select->parentWidget()!=window.annotationButton()->parentWidget());
        QCOMPARE(window.annotationButton()->parentWidget()->layout()->indexOf(select),-1);
        QVERIFY(select->isHidden());QVERIFY(!window.cameraFormatCombo()->parentWidget()->isHidden());
        QCOMPARE(combo->count(),3);QCOMPARE(combo->itemText(2),QStringLiteral("View video"));
        QSignalSpy changes(combo,&QComboBox::currentIndexChanged);
        QSignalSpy opens(&window,&MainWindow::openVideoRequested);
        combo->setCurrentIndex(2);
        QVERIFY(window.videoSourceSelected());QVERIFY(!select->isHidden());
        QCOMPARE(select->text(),QStringLiteral("Select video"));
        QVERIFY(window.cameraFormatCombo()->parentWidget()->isHidden());
        QVERIFY(!window.rotationCombo()->isHidden());QVERIFY(window.rotationCombo()->isEnabled());
        QCOMPARE(combo->nextInFocusChain(),static_cast<QWidget*>(select));
        select->click();QCOMPARE(opens.count(),1);
        window.setCameraDevices({"Fixture webcam","Fixture phone","New phone"},1);
        QVERIFY(window.videoSourceSelected());QCOMPARE(combo->currentIndex(),3);
        QCOMPARE(changes.count(),1);QCOMPARE(opens.count(),1); // refresh cannot open a dialog
        window.setVideoSourceSelected(false,1);
        QVERIFY(!window.videoSourceSelected());QCOMPARE(combo->currentIndex(),1);
        QVERIFY(select->isHidden());QVERIFY(!window.cameraFormatCombo()->parentWidget()->isHidden());
        QCOMPARE(changes.count(),1);
    }
    void zoomOutRangeAndEndpointUseActualAspectWithoutPersistence() {
        auto& window=NativeControlFixture::Window(app_);
        const auto original=NativeControlFixture::CurrentImageConfig(app_);
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ViewSourceSize(app_,QSize(0,0));
            NativeControlFixture::RestoreImageConfig(app_,original);});
        NativeControlFixture::ResetVideo(app_);
        window.resize(1200,780);window.show();QCoreApplication::processEvents();
        auto* slider=window.zoomSlider();QSignalSpy edits(slider,&QSlider::valueChanged);
        const auto savedBefore=NativeControlFixture::CurrentImageConfig(app_).zoomAmount;
        NativeControlFixture::ViewSourceSize(app_,QSize(1080,1920));
        QCOMPARE(edits.count(),0);
        QCOMPARE(NativeControlFixture::CurrentImageConfig(app_).zoomAmount,savedBefore);
        QVERIFY(slider->minimum()<100);QCOMPARE(slider->maximum(),1600);
        NativeControlFixture::SliderZoom(app_,slider->minimum());
        QCOMPARE(NativeControlFixture::CurrentImageConfig(app_).zoomAmount,kWholeSourceZoom);
        const int before=slider->value();
        NativeControlFixture::WheelZoom(app_,1);
        QVERIFY(slider->value()>before); // first step leaves the whole-picture endpoint
        NativeControlFixture::SliderZoom(app_,99);
        QCOMPARE(NativeControlFixture::CurrentImageConfig(app_).zoomAmount,1.0f);
        QCOMPARE(slider->value(),100);
        auto& target=NativeControlFixture::Target(app_);
        ControlPlan whole;whole.writes.push_back({Field::ZoomAmount,double(kWholeSourceZoom)});
        QVERIFY(target.ValidateImageParameterBatch(whole));
        QVERIFY(target.ApplyImageParameter(whole.writes.front()).value_or(false));
        QCOMPARE(NativeControlFixture::CurrentImageConfig(app_).zoomAmount,kWholeSourceZoom);
        // The disabled view displays 1x, independently of the saved endpoint.
        NativeControlFixture::DisableZoom(app_);
        NativeControlFixture::WheelZoom(app_,1);
        QCOMPARE(NativeControlFixture::CurrentImageConfig(app_).zoomAmount,1.1f);
        NativeControlFixture::SliderZoom(app_,99);
        edits.clear();
        NativeControlFixture::ViewSourceSize(app_,QSize(2560,1080));
        QCOMPARE(edits.count(),0);
        QCOMPARE(NativeControlFixture::CurrentImageConfig(app_).zoomAmount,1.0f);
    }
    void freezeKeepsCommittedFillBeforeDeferredFitRequest() {
        const auto original=NativeControlFixture::CurrentImageConfig(app_);
        const auto originalFit=NativeControlFixture::VideoFitMode(app_);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::SetCameraActive(app_,false);
            NativeControlFixture::ResetRecap(app_);NativeControlFixture::ResetVideo(app_);
            NativeControlFixture::RestoreImageConfig(app_,original);
            NativeControlFixture::RestoreVideoFitMode(app_,originalFit);
        });
        NativeControlFixture::ResetVideo(app_);
        auto fill=original;fill.zoomEnabled=true;fill.zoomAmount=2.0f;
        NativeControlFixture::RestoreImageConfig(app_,fill);
        NativeControlFixture::RestoreVideoFitMode(app_,settings::ViewportFitModeSetting::Fill);
        std::vector<std::uint8_t> pixels(32*16*4,47);
        NativeControlFixture::SeedRecapSceneWithoutRecording(app_,pixels);
        // No successful camera receipt follows this requested mode change.
        NativeControlFixture::RestoreVideoFitMode(app_,settings::ViewportFitModeSetting::Fit);
        NativeControlFixture::ToggleVideo(app_);
        const auto held=NativeControlFixture::HeldVideoFrame(app_);
        QVERIFY(held && held->bgra);QCOMPARE(*held->bgra,pixels);
        const auto config=NativeControlFixture::CurrentImageConfig(app_);
        QVERIFY(config.zoomEnabled);QCOMPARE(config.zoomAmount,2.0f);
        QCOMPARE(NativeControlFixture::VideoFitMode(app_),settings::ViewportFitModeSetting::Fill);
    }
    void videoChevronLeadsNeutralAndActionsContainOnlyFourButtons() {
        MainWindow window;window.resize(1400,800);window.show();
        auto* transport=window.videoTransport();
        VideoTransport::Snapshot snapshot;snapshot.source=VideoTransport::Source::Video;
        snapshot.sourceGeneration=51;snapshot.durationMs=10000;snapshot.seekable=true;
        transport->SetSnapshot(snapshot);
        for (int width : {1400,600}) {
            window.resize(width,800);QCoreApplication::processEvents();
            auto* panel=window.findChild<QWidget*>("bottomLeftPanel");
            auto* layout=qobject_cast<QGridLayout*>(panel->layout());QVERIFY(layout);
            int row,column,rowSpan,columnSpan;
            layout->getItemPosition(layout->indexOf(transport->collapseButton()),&row,&column,&rowSpan,&columnSpan);
            QCOMPARE(column,0);
            transport->SetCollapsed(true);QCoreApplication::processEvents();
            QVERIFY(!transport->isVisible());QVERIFY(transport->collapseButton()->isVisible());
            const auto ink=transport->collapseButton()->palette().color(QPalette::Button);
            const auto image=transport->collapseButton()->grab().toImage();
            QCOMPARE(image.pixelColor(5,image.height()/2),ink);
            transport->SetCollapsed(false);
        }
        auto* actions=window.findChild<QWidget*>("bottomRightPanel");QVERIFY(actions);
        const auto buttons=actions->findChildren<QPushButton*>();QCOMPARE(buttons.size(),4);
        for(auto* button:buttons)QVERIFY(!button->text().contains("Read"));
    }
    void cancelledVideoPickerRestoresOnlyPhoneLight() {
        NativeControlFixture::ResetVideo(app_);
        QVERIFY(NativeControlFixture::CancelVideoPickerRestoresPhoneLight(app_,true));
        QVERIFY(NativeControlFixture::CancelVideoPickerRestoresPhoneLight(app_,false));
    }
    void translatedActionRowsHaveUniformCellMinimums_data() {
        QTest::addColumn<int>("language");
        QTest::newRow("english") << int(AppLanguage::English);
        QTest::newRow("turkish") << int(AppLanguage::Turkish);
        QTest::newRow("german") << int(AppLanguage::German);
    }
    void translatedActionRowsHaveUniformCellMinimums() {
        QFETCH(int,language);
        auto* application=qobject_cast<QApplication*>(QCoreApplication::instance());QVERIFY(application);
        QVERIFY(QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"))>=0);
        LanguageManager translations(*application);QVERIFY(translations.SetLanguage(static_cast<AppLanguage>(language),false));
        const auto cleanup=qScopeGuard([&]{translations.SetLanguage(AppLanguage::English,false);});
        MainWindow window;window.setFont(QFont(QStringLiteral("Segoe UI")));
        window.resize(language==int(AppLanguage::German)?1600:1280,800);window.show();
        QCoreApplication::processEvents();
        window.resize(1100,760);window.advancedModeButton()->click();window.setAdvancedPanelWidth(440);
        auto* panel=window.findChild<QWidget*>("bottomRightPanel");QVERIFY(panel);
        auto* grid=qobject_cast<QGridLayout*>(panel->layout());QVERIFY(grid);
        const auto problem=[&](int expectedRows) -> QString {
            QMap<int,QVector<QRect>> rows;
            for(auto* button:{window.capturePhotoButton(),window.recordButton(),window.explainNowButton(),window.annotationButton()})rows[button->y()].push_back(button->geometry());
            if(rows.size()<1||rows.size()>2||(expectedRows&&rows.size()!=expectedRows))return QStringLiteral("Expected %1 action rows (0 = content fit), got %2").arg(expectedRows).arg(rows.size());
            if(rows.size()==2&&(rows.first().size()!=2||rows.last().size()!=2))return QStringLiteral("Wrapped actions must form two above two");
            int left=-1,right=-1;
            for(const auto& row:rows){
                int minWidth=INT_MAX,maxWidth=0,rowLeft=INT_MAX,rowRight=0;
                for(const auto& rect:row){minWidth=std::min(minWidth,rect.width());maxWidth=std::max(maxWidth,rect.width());rowLeft=std::min(rowLeft,rect.left());rowRight=std::max(rowRight,rect.right());}
                if(maxWidth-minWidth>4)return QStringLiteral("Action row widths %1..%2").arg(minWidth).arg(maxWidth);
                if(left>=0&&(std::abs(rowLeft-left)>1||std::abs(rowRight-right)>1))return QStringLiteral("Action row edges differ");
                left=rowLeft;right=rowRight;
            }
            return {};
        };
        QCoreApplication::processEvents();
        QVERIFY2(problem(0).isEmpty(),qPrintable(problem(0)));
        auto* modes=window.findChild<QWidget*>("bottomLeftPanel");QVERIFY(modes);
        QCOMPARE(modes->property("modeRows").toInt(),2);
        QVERIFY(modes->width()<window.renderWidget()->width());
        const auto bounds=[](QWidget* widget){return QRect(widget->mapToGlobal(QPoint()),widget->size());};
        QVERIFY(!bounds(modes).intersects(bounds(panel)));
        QCOMPARE(bounds(panel).bottom(),bounds(window.renderWidget()).bottom());
        window.resize(960,760);QCoreApplication::processEvents();
        QVERIFY2(problem(2).isEmpty(),qPrintable(problem(2)));
        QVERIFY(bounds(window.renderWidget()).contains(bounds(panel)));
        QVERIFY(!bounds(modes).intersects(bounds(panel)));
        window.resize(720,720);window.setAdvancedPanelWidth(360);QCoreApplication::processEvents();
        QVERIFY2(problem(2).isEmpty(),qPrintable(problem(2)));
        QVERIFY(bounds(window.renderWidget()).contains(bounds(panel)));
        QVERIFY(bounds(window.renderWidget()).contains(bounds(modes)));
        QVERIFY(!bounds(modes).intersects(bounds(panel)));
        for(auto* button:{window.capturePhotoButton(),window.recordButton(),window.explainNowButton(),window.annotationButton()}){
            QVERIFY(!button->text().isEmpty());QVERIFY(!button->toolTip().isEmpty());
            QVERIFY(!button->accessibleName().isEmpty());
            QVERIFY(button->width()>=48);QVERIFY(button->height()>=58);
        }
        window.simpleModeButton()->click();window.resize(language==int(AppLanguage::German)?1600:1280,800);QCoreApplication::processEvents();
        QVERIFY2(problem(1).isEmpty(),qPrintable(problem(1)));
    }
    void uctOverlayFollowsImmediateViewportMovesWithoutQueuedEvents() {
        MainWindow window;window.resize(1100,760);window.show();
        QCoreApplication::processEvents();
        auto* target=window.renderWidget();auto* overlay=window.ultraClearTextOverlay();
        QVERIFY(target&&overlay);
        const auto transform=ComputeViewTransform(1600,900,target->width(),target->height(),1.0f,0.5f,0.5f,ViewportFitMode::kFit);
        overlay->SetViewTransform(transform);
        overlay->SetWords({{QStringLiteral("Reading"),QRectF(.28,.35,.23,.07),{Qt::black,Qt::white},1}});
        QCoreApplication::processEvents();QVERIFY(overlay->isVisible());
        const QPoint origin=target->mapToGlobal(QPoint());
        const auto paints=overlay->PaintCount();
        // Feed the production fast path the native position while Qt's move
        // queue is withheld. No OCR, timer tick or repaint may be required.
        for(int step=1;step<=120;++step){
            const QPoint next=origin+QPoint(step*2,step%7);
            NativeControlFixture::MoveViewportPeers(window,next);
            QCOMPARE(overlay->geometry(),QRect(next,target->size()));
            QCOMPARE(overlay->WordCount(),1);
            QCOMPARE(overlay->PaintCount(),paints);
            QCOMPARE(overlay->CurrentViewTransform().sourceX,transform.sourceX);
        }
    }
    void uctPaletteUsesPresentedGenerationAndCpuDoesNotReapply() {
        auto& window = NativeControlFixture::Window(app_);
        auto* target = window.renderWidget(); auto* overlay = window.ultraClearTextOverlay();
        const auto palette = NativeControlFixture::UctPaletteState(app_);
        const auto cleanup = qScopeGuard([&] {
            overlay->ClearWords(); NativeControlFixture::UctPaletteReceipt(app_, 0); window.hide();
            NativeControlFixture::RestoreUctPalette(app_, palette);
        });
        window.resize(1200, 800); window.show(); QCoreApplication::processEvents();
        overlay->SetViewTransform(ComputeViewTransform(target->width(), target->height(),
            target->width(), target->height(), 1.f, .5f, .5f, ViewportFitMode::kFit));
        overlay->SetWords({{QStringLiteral("Control"), QRectF(.25,.25,.45,.14), {Qt::black,Qt::green},1}});
        const auto count = [&](QColor color) {
            QImage image(target->size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            overlay->render(&image);
            int pixels = 0;
            for (int y=0;y<image.height();++y) for(int x=0;x<image.width();++x)
                pixels += image.pixelColor(x,y) == color;
            return pixels;
        };
        NativeControlFixture::UctPalette(app_, QStringLiteral("yellow-black"), 100);
        NativeControlFixture::UctPaletteReceipt(app_, 2, 99); // old receipt cannot borrow new LUT
        QVERIFY(count(Qt::green)>100);
        NativeControlFixture::UctPaletteReceipt(app_, 2, 100);
        QVERIFY(count(QColor(149,149,0))>100);
        QCOMPARE(count(Qt::green),0);
        NativeControlFixture::UctPalette(app_, QStringLiteral("cyan-black"), 101);
        NativeControlFixture::UctPaletteReceipt(app_, 2, 100); // keep cached scene's exact old mapping
        QVERIFY(count(QColor(149,149,0))>100);
        NativeControlFixture::UctPaletteReceipt(app_, 2, 101);
        QVERIFY(count(QColor(0,149,149))>100);
        NativeControlFixture::UctPaletteReceipt(app_, 0); // CPU analysis has already been processed
        QVERIFY(count(Qt::green)>100);
    }
    void selectedMediaGpuPolicyIncludesAllImageEffects() {
        // Exercise the policy seam without creating a CUDA surface or claiming
        // that this offscreen fixture has executed any shader.
        for(const auto field:{Field::Blur,Field::BlurSigma,Field::BlurRadius,
            Field::Spatial,Field::Upscaler,Field::Sharpness,
            Field::Flatten,Field::Adaptive,Field::SmartSharpen,Field::Clahe,
            Field::Glare,Field::FocusDetection,Field::Hysteresis,Field::TwoColor,
            Field::AutoContrast,Field::AutoContrastStrength,Field::Stabilization,
            Field::BumpHold,Field::Keystone,Field::KeystonePaused,Field::Temporal,
            Field::TemporalAlpha,Field::SuperResEnabled,Field::SuperResStrength,
            Field::SuperResPrefer2x,Field::SuperResUltra1440p,Field::SuperResPerformanceOverride}) {
            QVERIFY(AppControlTarget::SelectedMediaImageFieldAvailable(field,true));
            QVERIFY(!AppControlTarget::SelectedMediaImageFieldAvailable(field,false));
        }
        for(const auto field:{Field::BlackWhite,Field::Threshold,Field::Contrast,Field::Brightness,
            Field::Scheme,Field::CustomColorRecipe,Field::TextClarity,Field::UctCpuDetector,Field::UctGpuDetector})
            QVERIFY(AppControlTarget::SelectedMediaImageFieldAvailable(field,false));
        QVERIFY(!AppControlTarget::SelectedMediaImageFieldAvailable(Field::DebugView,true));
    }
    void selectedCpuMediaRejectsGpuEffectMutations() {
        const auto original=NativeControlFixture::CurrentImageConfig(app_);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::ResetVideo(app_);
            NativeControlFixture::RestoreImageConfig(app_,original);
        });
        auto dormant=original;
        dormant.autoTextClarityEnabled=true;dormant.adaptiveBinarizationEnabled=true;
        dormant.twoColorTextEnabled=true;dormant.blackWhiteEnabled=true;
        NativeControlFixture::RestoreImageConfig(app_,dormant);
        NativeControlFixture::SeedVideo(app_,{},OwnedVideoPolicyFrame(),false,false);
        auto& target=NativeControlFixture::Target(app_);
        const auto rows=target.QueryImageParameters(ToolId::ImageSettingsGet,{})["parameters"].toObject();
        const auto before=target.ImageParameterFields();
        for(const auto* id:{"image.blur.enabled","image.spatial.enabled",
            "image.autoContrast.enabled","image.stabilization.enabled","image.keystone.enabled",
            "image.temporal.enabled","image.superResolution.enabled"}) {
            const auto row=rows[id].toObject();
            QVERIFY(!row["available"].toBool());QVERIFY(!row["writable"].toBool());
            QVERIFY(row["effective"].isNull());
            QCOMPARE(row["effectiveReason"].toString(),QStringLiteral("selected_media_unsupported_effect"));
            const auto field=AppControlTarget::ImageParameterField(id);QVERIFY(field);
            const bool next=!before.at(*field).toBool();
            const auto plan=target.PrepareImageParameters(ToolId::ImageSettingsSet,{{id,next}});
            QCOMPARE(plan.error,QStringLiteral("unavailable"));QVERIFY(plan.writes.empty());
            ControlPlan bypass;bypass.writes.push_back({*field,next});
            QVERIFY(!target.ValidateImageParameterBatch(bypass));
        }
        QVERIFY(rows["image.contrast"].toObject()["writable"].toBool());
        QVERIFY(rows["image.blackWhite.enabled"].toObject()["writable"].toBool());
        const auto master=rows["image.textClarity.enabled"].toObject();
        QVERIFY(master["writable"].toBool());QVERIFY(master["requested"].toBool());
        QVERIFY(!master["effective"].toBool());
        QCOMPARE(master["effectiveReason"].toString(),"cuda_unavailable");
        const auto masterPlan=target.PrepareImageParameters(ToolId::ImageSettingsSet,{{"image.textClarity.enabled",false}});
        QVERIFY(masterPlan.error.isEmpty());QVERIFY(!masterPlan.writes.empty());
        QVERIFY(target.ValidateImageParameterBatch(masterPlan));
        // Unsupported requested adaptive text must not shadow the actual CPU
        // threshold/color recipe, even though those GPU preferences are kept.
        QVERIFY(rows["image.blackWhite.enabled"].toObject()["effective"].toBool());
        QVERIFY(rows["image.blackWhite.threshold"].toObject()["effective"].isDouble());
        QCOMPARE(rows["image.colors.schemeId"].toObject()["effective"],rows["image.colors.schemeId"].toObject()["requested"]);
        QCOMPARE(target.ImageParameterFields(),before);
        const auto state=target.QueryVideoState();
        QCOMPARE(state["pipeline"].toString(),QStringLiteral("cpu_selected_media"));
        QVERIFY(!state["advancedGpuEffectsAvailable"].toBool());
    }
    void selectedMediaObservationUsesPresentationCandidateIdentity() {
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::SeedVideo(app_,{},OwnedVideoPolicyFrame(),false,false);
        auto& target=NativeControlFixture::Target(app_);
        const auto old=NativeControlFixture::VideoState(app_);
        const auto transform=ComputeViewTransform(32,16,640,480,2,0.25f,0.75f,ViewportFitMode::kFill);
        constexpr quint64 candidateSequence=741;
        constexpr qint64 candidatePts=59'000'000;
        QVERIFY(old["frameSequence"].toInteger()!=candidateSequence);
        QVERIFY(old["selectedPts100ns"].toInteger()!=candidatePts);
        const auto frame=target.PresentedMediaFrame(transform,QSize(32,16),QSize(640,480),candidateSequence,candidatePts);
        QVERIFY(frame.immutableSource);
        QCOMPARE(frame.sequence,candidateSequence);QCOMPARE(frame.mediaPts100ns,candidatePts);
        QCOMPARE(frame.documentId,old["documentId"].toString());
        QCOMPARE(frame.selectionGeneration,old["selectionGeneration"].toInteger());
        QCOMPARE(frame.documentGeneration,old["selectionGeneration"].toInteger());
        // The paused CPU candidate carries known still-frame diagnostics.
        QVERIFY(frame.held && *frame.held);
        QVERIFY(frame.converging && !*frame.converging);
        QVERIFY(frame.keystoneQuad && !*frame.keystoneQuad);
        QVERIFY(frame.diagnostics.value("available").toBool());
        // Constructing candidate authority must not publish it as the displayed
        // document; the successful presenter submission owns that transition.
        const auto after=NativeControlFixture::VideoState(app_);
        QCOMPARE(after["frameSequence"],old["frameSequence"]);
        QCOMPARE(after["selectedPts100ns"],old["selectedPts100ns"]);
        // The same candidate submitted through the GPU scene is explicitly
        // unknown without a completed media helper diagnostic: no held,
        // converging or keystone certainty is inherited from this CPU
        // display, even for the first GPU frame. This fixture creates no
        // CUDA scene, so nothing here claims GPU execution.
        QVERIFY(!NativeControlFixture::HasMediaGpuScene(app_));
        const auto gpuFrame=target.PresentedMediaFrame(transform,QSize(32,16),QSize(640,480),
            candidateSequence,candidatePts,true);
        QVERIFY(gpuFrame.immutableSource);
        QCOMPARE(gpuFrame.sequence,candidateSequence);QCOMPARE(gpuFrame.mediaPts100ns,candidatePts);
        QCOMPARE(gpuFrame.documentId,old["documentId"].toString());
        QVERIFY(!gpuFrame.held);QVERIFY(!gpuFrame.converging);QVERIFY(!gpuFrame.keystoneQuad);
        QVERIFY(!gpuFrame.diagnostics.value("available").toBool());
        QCOMPARE(gpuFrame.diagnostics.value("basis").toString(),QStringLiteral("pending-media-gpu-diagnostic"));
        const auto afterGpu=NativeControlFixture::VideoState(app_);
        QCOMPARE(afterGpu["frameSequence"],old["frameSequence"]);
        QCOMPARE(afterGpu["selectedPts100ns"],old["selectedPts100ns"]);
    }
    void unchangedPausedMediaKeepsOneViewportUpload() {
        QVERIFY(NativeControlFixture::MediaViewportAdmissionAvoidsRepeatedUploads());
    }
    void canceledAndTimedOutOriginalReadbacksKeepExactDiscardIdentity() {
        QVERIFY(NativeControlFixture::RetiredMediaHoldCopiesStayOutOfViewportRouting(app_));
    }
    void pauseRetainsPresentedOriginalBeforeDeferredSuccessor() {
        const auto original=NativeControlFixture::CurrentImageConfig(app_);
        const auto originalFit=NativeControlFixture::VideoFitMode(app_);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::ResetVideo(app_);
            NativeControlFixture::SetCameraActive(app_,false);
            NativeControlFixture::RestoreImageConfig(app_,original);
            NativeControlFixture::RestoreVideoFitMode(app_,originalFit);
        });
        RecordingSessionInfo session;session.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(NativeControlFixture::StageDeferredLiveOriginal(app_,session),std::uint64_t(71));
        const auto state=NativeControlFixture::VideoState(app_);
        QVERIFY(!state["pendingHold"].toBool());
        QCOMPARE(state["provisionalCaptureClock100ns"].toInteger(),qint64(710'000));
        const auto held=NativeControlFixture::HeldVideoFrame(app_);
        QVERIFY(held);
        QCOMPARE(held->sampleId,std::uint64_t(71));
        QVERIFY(held->bgra && std::all_of(held->bgra->begin(),held->bgra->end(),[](auto pixel) {return pixel==31;}));
        const auto heldConfig=NativeControlFixture::CurrentImageConfig(app_);
        QVERIFY(heldConfig.zoomEnabled);
        QVERIFY(std::abs(heldConfig.zoomAmount-2.0f)<0.00001f);
        QCOMPARE(NativeControlFixture::VideoFitMode(app_),settings::ViewportFitModeSetting::Fill);
    }
    void freezeOriginalFallbackPreservesCommittedIntent_data() {
        QTest::addColumn<int>("intent");
        QTest::newRow("explicit-fit-before-pending-fill")<<1;
        QTest::newRow("whole-fill-before-pending-fit")<<2;
        QTest::newRow("legacy-fill-before-pending-fit")<<3;
    }
    void freezeOriginalFallbackPreservesCommittedIntent() {
        QFETCH(int,intent);
        const auto original=NativeControlFixture::CurrentImageConfig(app_);
        const auto originalFit=NativeControlFixture::VideoFitMode(app_);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::ResetVideo(app_);
            NativeControlFixture::SetCameraActive(app_,false);
            NativeControlFixture::RestoreImageConfig(app_,original);
            NativeControlFixture::RestoreVideoFitMode(app_,originalFit);
        });
        RecordingSessionInfo session;session.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(NativeControlFixture::StageDeferredLiveOriginal(app_,session,false,false,intent),std::uint64_t(71));
        const auto held=NativeControlFixture::HeldVideoFrame(app_);
        QVERIFY(held && held->bgra);
        QVERIFY(std::all_of(held->bgra->begin(),held->bgra->end(),[](auto pixel) {return pixel==31;}));
        const auto config=NativeControlFixture::CurrentImageConfig(app_);
        const bool fitted=intent==1,whole=intent==2;
        QCOMPARE(NativeControlFixture::VideoFitMode(app_),fitted
            ? settings::ViewportFitModeSetting::Fit : settings::ViewportFitModeSetting::Fill);
        QCOMPARE(config.zoomEnabled,whole);
        QCOMPARE(config.zoomAmount,whole ? kWholeSourceZoom : 1.0f);
        // All three committed transforms were identical at matching aspects.
        // A later aspect change must preserve their different requested intent.
        const auto reframed=ComputeViewTransform(held->width,held->height,held->width*2,held->height,
            config.zoomEnabled ? config.zoomAmount : 1.0f,0.5f,0.5f,
            fitted ? ViewportFitMode::kFit : ViewportFitMode::kFill);
        QVERIFY(reframed.valid);
        QCOMPARE(reframed.sourceWidth,1.0f);
        if(fitted || whole)QCOMPARE(reframed.sourceHeight,1.0f);
        else QVERIFY(reframed.sourceHeight<1.0f);
    }
    void pauseRejectsUnreproducibleDisplayedAppearance_data() {
        QTest::addColumn<bool>("unsupportedApplied");QTest::addColumn<bool>("pendingAppearance");
        QTest::newRow("unsupported-actual-neutral-request")<<true<<false;
        QTest::newRow("neutral-actual-newer-pending-contrast")<<false<<true;
    }
    void asynchronousPauseRefusalKeepsLiveOrReportsUnavailable_data() {
        QTest::addColumn<bool>("finishing");QTest::addColumn<bool>("cameraActive");
        QTest::newRow("recording-camera-live")<<false<<true;
        QTest::newRow("recording-camera-lost")<<false<<false;
        QTest::newRow("finishing-camera-live")<<true<<true;
        QTest::newRow("finishing-camera-lost")<<true<<false;
    }
    void asynchronousPauseRefusalKeepsLiveOrReportsUnavailable() {
        QFETCH(bool,finishing);QFETCH(bool,cameraActive);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::ResetVideo(app_);
            NativeControlFixture::SetCameraActive(app_,false);
        });
        QVERIFY(!NativeControlFixture::RefusePendingLiveHold(app_,finishing,cameraActive));
        const auto state=NativeControlFixture::VideoState(app_);
        QVERIFY(!state["pendingHold"].toBool());
        QCOMPARE(state["heldUnrecorded"].toBool(),finishing);
        QCOMPARE(state["playing"].toBool(),!finishing); // active recording retains live intent
        QCOMPARE(state["state"].toString(),finishing?QStringLiteral("finishing")
            :cameraActive?QStringLiteral("live"):QStringLiteral("waiting"));
    }
    void pauseRejectsUnreproducibleDisplayedAppearance() {
        QFETCH(bool,unsupportedApplied);QFETCH(bool,pendingAppearance);
        const auto original=NativeControlFixture::CurrentImageConfig(app_);
        const auto originalFit=NativeControlFixture::VideoFitMode(app_);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::ResetVideo(app_);
            NativeControlFixture::SetCameraActive(app_,false);
            NativeControlFixture::RestoreImageConfig(app_,original);
            NativeControlFixture::RestoreVideoFitMode(app_,originalFit);
        });
        RecordingSessionInfo session;session.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(NativeControlFixture::StageDeferredLiveOriginal(app_,session,unsupportedApplied,pendingAppearance),std::uint64_t(0));
        const auto& shown=NativeControlFixture::PresentedOriginal(app_);
        QVERIFY(shown);
        QCOMPARE(shown->original->identity.sequenceNumber,std::uint64_t(71));
        QCOMPARE(shown->canReproduceCleanView,!unsupportedApplied);
        QCOMPARE(shown->recipe.contrast,1.0f);
        QVERIFY(!NativeControlFixture::HeldVideoFrame(app_));
        QVERIFY(!NativeControlFixture::VideoState(app_)["pendingHold"].toBool());
        // Refusing an unreproducible hold leaves the camera selected instead
        // of entering an empty selected-media document and freezing preview.
        QCOMPARE(NativeControlFixture::VideoState(app_)["state"].toString(),QStringLiteral("live"));
    }
    void presentedOriginalCacheBoundsLeasesAndRevokesUnretainedSuccess() {
        PresentedOriginalCache cache;
        CapturedFrame frame;frame.width=3840;frame.height=2160;
        frame.gpuScene.textureSharedHandle=reinterpret_cast<HANDLE>(1);
        frame.gpuScene.fenceSharedHandle=reinterpret_cast<HANDLE>(2);
        frame.gpuScene.readyFenceValue=1;frame.gpuScene.width=frame.width;frame.gpuScene.height=frame.height;
        frame.gpuScene.lifetime=std::make_shared<int>(7);
        frame.identity.sequenceNumber=1;
        const auto view=ComputeViewTransform(frame.width,frame.height,800,600,1,0.5f,0.5f,ViewportFitMode::kFit);
        QVERIFY(cache.Stage(frame,"recording",11,0));cache.Presented(11,1,view);
        const std::weak_ptr<void> originalLease=frame.gpuScene.lifetime;
        frame.gpuScene.lifetime=std::make_shared<int>(8);frame.identity.sequenceNumber=2;
        QVERIFY(cache.Stage(frame,"recording",11,0));
        QVERIFY(!originalLease.expired());
        QCOMPARE(cache.Displayed()->original->identity.sequenceNumber,std::uint64_t(1));
        QVERIFY(cache.RetainedBytes()<=PresentedOriginalCache::kByteLimit);
        cache.Presented(11,2,view);QVERIFY(originalLease.expired());
        frame.width=16384;frame.height=16384;frame.gpuScene.width=frame.width;frame.gpuScene.height=frame.height;
        frame.identity.sequenceNumber=3;
        QVERIFY(!cache.Stage(frame,"recording",11,0));
        cache.Presented(11,3,view);QVERIFY(!cache.Displayed());
        cache.Reset();QCOMPARE(cache.RetainedBytes(),std::size_t(0));
    }
    void encoderStartupFailureKeepsSelectedFile_data() {
        QTest::addColumn<bool>("playing");
        QTest::newRow("paused")<<false;QTest::newRow("playing")<<true;
    }
    void encoderStartupFailureKeepsSelectedFile() {
        QFETCH(bool,playing);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::CleanupVideoRecording(app_);NativeControlFixture::ResetVideo(app_);
        });
        NativeControlFixture::SeedVideo(app_,{},OwnedVideoPolicyFrame(),false,playing);
        const auto before=NativeControlFixture::VideoState(app_);
        auto attempts=std::make_shared<std::atomic<int>>(0);
        NativeControlFixture::ForceEncoderStartFailure(app_,attempts);
        const auto session=NativeControlFixture::StartSilentVideoRecording(app_);
        QVERIFY(session);
        QCOMPARE(NativeControlFixture::VideoState(app_)["documentId"],before["documentId"]);
        QVERIFY(!NativeControlFixture::VideoTimeline(app_));
        NativeControlFixture::SubmitVideoOriginal(app_);
        QTRY_VERIFY_WITH_TIMEOUT(NativeControlFixture::RecordingRetired(app_),5000);
        QVERIFY(attempts->load()>0);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
        const auto after=NativeControlFixture::VideoState(app_);
        QCOMPARE(after["kind"].toString(),QStringLiteral("file"));
        for(const auto* key:{"documentId","selectionGeneration","selectedPts100ns","frameSequence","playing"})
            QCOMPARE(after[key],before[key]);
        QVERIFY(!NativeControlFixture::VideoTimeline(app_));
    }
    void firstAdmissionSwitchesOnlyAfterCurrentSessionAcknowledgement_data() {
        QTest::addColumn<bool>("stopBeforeDelivery");
        QTest::newRow("first-success")<<false;
        QTest::newRow("stopped-before-queued-acknowledgement")<<true;
    }
    void firstAdmissionSwitchesOnlyAfterCurrentSessionAcknowledgement() {
        QFETCH(bool,stopBeforeDelivery);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::CleanupVideoRecording(app_);NativeControlFixture::ResetVideo(app_);
        });
        NativeControlFixture::SeedVideo(app_,{},OwnedVideoPolicyFrame(),false,false);
        const auto before=NativeControlFixture::VideoState(app_);
        const auto session=NativeControlFixture::StartSilentVideoRecording(app_);
        QVERIFY(session);
        QCOMPARE(NativeControlFixture::VideoState(app_)["documentId"],before["documentId"]);
        NativeControlFixture::PublishVideoAdmission(app_);
        const auto timeline=NativeControlFixture::VideoTimeline(app_);
        QVERIFY(timeline);QCOMPARE(timeline->session.id,session->id);
        QVERIFY(timeline->FindAdmission(91,910'000));
        // Admission publication queues the UI acknowledgement; it does not
        // mutate selection from the recording worker's publication boundary.
        QCOMPARE(NativeControlFixture::VideoState(app_)["documentId"],before["documentId"]);
        if(stopBeforeDelivery) {
            NativeControlFixture::StopVideoRecording(app_,session->id);
            QTRY_VERIFY_WITH_TIMEOUT(NativeControlFixture::RecordingRetired(app_),5000);
            QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
            const auto after=NativeControlFixture::VideoState(app_);
            QCOMPARE(after["kind"].toString(),QStringLiteral("file"));
            for(const auto* key:{"documentId","selectionGeneration","selectedPts100ns","frameSequence","playing"})
                QCOMPARE(after[key],before[key]);
        } else {
            QTRY_COMPARE(NativeControlFixture::VideoState(app_)["kind"].toString(),QStringLiteral("recording"));
            const auto selected=NativeControlFixture::VideoState(app_);
            QVERIFY(selected["documentId"]!=before["documentId"]);
            NativeControlFixture::PublishVideoAdmission(app_);
            QCoreApplication::sendPostedEvents(nullptr,QEvent::MetaCall);
            QCOMPARE(NativeControlFixture::VideoState(app_)["documentId"],selected["documentId"]);
            QCOMPARE(NativeControlFixture::VideoState(app_)["selectionGeneration"],selected["selectionGeneration"]);
        }
    }
    void unavailableSaveKeepsPreviousCommittedReceipt_data() {
        QTest::addColumn<bool>("cameraView");
        QTest::newRow("selected-scene-without-native-presentation")<<false;
        QTest::newRow("camera-has-no-selected-media")<<true;
    }
    void unavailableSaveKeepsPreviousCommittedReceipt() {
        QFETCH(bool,cameraView);
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::SeedVideo(app_,{},OwnedVideoPolicyFrame(),false,false);
        NativeControlFixture::SeedCommittedVideoSave(app_);
        if(cameraView)NativeControlFixture::ResetVideo(app_);
        const auto before=NativeControlFixture::VideoState(app_);
        NativeControlFixture::SaveVideo(app_);
        const auto after=NativeControlFixture::VideoState(app_);
        for(const auto* key:{"saveRevision","saveState","saveDocumentId","saveFrameSequence","saveSelectionGeneration","savePts100ns"})
            QCOMPARE(after[key],before[key]);
        QCOMPARE(after["saveState"].toString(),QStringLiteral("committed"));
    }
    void agentCannotReuseCommittedSaveWhenNewPayloadWasNotAdmitted() {
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::SeedVideo(app_,{},OwnedVideoPolicyFrame(),false,false);
        NativeControlFixture::SeedCommittedVideoSave(app_);
        const auto before=NativeControlFixture::VideoState(app_);
        auto& service=NativeControlFixture::Service(app_);
        ControlGrants grants;grants.photos=true;
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"video-save-not-admitted",1,grants});
        const auto end=qScopeGuard([&] {service.End(context);});
        (void)Call(context,"save-source","video.state.get");
        const auto result=Call(context,"save-attempt","video.save_frame",
            {{"documentId",before["documentId"]},{"selectionGeneration",before["selectionGeneration"]}});
        QCOMPARE(result.content["error"].toObject()["code"].toString(),QStringLiteral("unavailable"));
        QVERIFY(!result.content["pending"].toBool());
        const auto after=NativeControlFixture::VideoState(app_);
        QCOMPARE(after["saveRevision"],before["saveRevision"]);
        QCOMPARE(after["saveState"].toString(),QStringLiteral("committed"));
    }
    void ordinaryCameraOwnsNoVideoTimelineOrPolling() {
        NativeControlFixture::ResetVideo(app_);
        const auto state=NativeControlFixture::VideoState(app_);
        QCOMPARE(state["kind"].toString(),QStringLiteral("camera"));
        QVERIFY(!state["hasFrame"].toBool());
        QCOMPARE(NativeControlFixture::VideoUi(app_).source,VideoTransport::Source::None);
        QVERIFY(!NativeControlFixture::VideoTimerActive(app_));
    }
    void recapFreezeOwnsExactSceneDuringEncoderWarmupWithoutRecording_data() {
        QTest::addColumn<bool>("unsupported");QTest::newRow("encoder-warmup")<<false;QTest::newRow("unsupported-codec-size")<<true;
    }
    void recapFreezeOwnsExactSceneDuringEncoderWarmupWithoutRecording() {
        QFETCH(bool,unsupported);
        const auto config=NativeControlFixture::CurrentImageConfig(app_);
        const auto cleanup=qScopeGuard([&]{NativeControlFixture::SetCameraActive(app_,false);
            NativeControlFixture::ResetRecap(app_);NativeControlFixture::ResetVideo(app_);
            NativeControlFixture::RestoreImageConfig(app_,config);});
        NativeControlFixture::ResetVideo(app_);std::vector<std::uint8_t> pixels(32*16*4,47);
        NativeControlFixture::SeedRecapSceneWithoutRecording(app_,pixels,unsupported);
        QCOMPARE(NativeControlFixture::VideoState(app_)["kind"].toString(),QStringLiteral("recap"));
        QCOMPARE(NativeControlFixture::VideoUi(app_).source,VideoTransport::Source::Recap);
        QVERIFY(!NativeControlFixture::VideoUi(app_).seekable);
        QVERIFY(NativeControlFixture::VideoUi(app_).canPauseLive);
        std::fill(pixels.begin(),pixels.end(),211);
        NativeControlFixture::ToggleVideo(app_);
        const auto held=NativeControlFixture::HeldVideoFrame(app_);
        QVERIFY(held && held->bgra);QCOMPARE(held->sampleId,std::uint64_t(91));
        QVERIFY(std::all_of(held->bgra->begin(),held->bgra->end(),[](auto b){return b==47;}));
        QVERIFY(NativeControlFixture::VideoState(app_)["selectedPts100ns"].isNull());
        const auto preferences=NativeControlFixture::RecapPreferences(app_);
        NativeControlFixture::SetRecapMinutes(app_,3);
        auto expected=preferences;expected["minutes"]=3;
        QCOMPARE(NativeControlFixture::RecapPreferences(app_),expected);
        NativeControlFixture::SetRecapMinutes(app_,preferences["minutes"].toInt());
        NativeControlFixture::JumpVideo(app_);
        QCOMPARE(NativeControlFixture::VideoState(app_)["kind"].toString(),QStringLiteral("recap"));
        QVERIFY(!NativeControlFixture::HeldVideoFrame(app_));
        NativeControlFixture::ResetRecap(app_);
        QVERIFY(!NativeControlFixture::HeldVideoFrame(app_));
        QVERIFY(!NativeControlFixture::VideoUi(app_).seekable);
    }
    void collapsedRecapViewportSpaceKeepsFreezeResumeAndFocus() {
        auto& window=NativeControlFixture::Window(app_);
        auto* transport=window.videoTransport();
        const auto cleanup=qScopeGuard([&]{transport->SetCollapsed(false);window.hide();
            NativeControlFixture::SetCameraActive(app_,false);NativeControlFixture::ResetRecap(app_);NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::ResetVideo(app_);std::vector<std::uint8_t> pixels(32*16*4,47);
        NativeControlFixture::SeedRecapSceneWithoutRecording(app_,pixels);
        window.show();window.activateWindow();
        auto* viewport=window.renderWidget();viewport->setFocus();
        QTRY_COMPARE(QApplication::focusWidget(),viewport);
        transport->SetCollapsed(true);
        QVERIFY(transport->isHidden());
        QSignalSpy toggle(transport,&VideoTransport::PlayPauseRequested);
        QTest::keyClick(viewport,Qt::Key_Space);
        QCOMPARE(toggle.size(),1);
        const auto held=NativeControlFixture::HeldVideoFrame(app_);
        QVERIFY(held && held->bgra);
        QCOMPARE(QApplication::focusWidget(),viewport);
        // There is no native presenter in this fixture. Model only its
        // successful held-frame receipt through the existing production
        // bookkeeping seam before asking the real keyboard route to Resume.
        QCOMPARE(NativeControlFixture::HoldPresentedVideoFrame(app_,held),held);
        NativeControlFixture::ApplyRecapSnapshot(app_,VideoRecapSnapshot{});
        QTest::keyClick(viewport,Qt::Key_Space);
        QCOMPARE(toggle.size(),2);
        QVERIFY(NativeControlFixture::VideoState(app_)["playing"].toBool());
        QCOMPARE(NativeControlFixture::HeldVideoFrame(app_),held);
        QCOMPARE(QApplication::focusWidget(),viewport);
        QVERIFY(transport->isCollapsed());
        QVERIFY(!transport->IsActionReady(transport->jumpButton()));
        QVERIFY(transport->IsCommandReady(transport->jumpButton()));
    }
    void hiddenVideoBarLeavesOnlyCornerToggleAndReclaimsViewport() {
        QVERIFY(QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"))>=0);
        MainWindow window;
        auto* transport=window.videoTransport(); auto* toggle=transport->collapseButton();
        auto* corner=window.findChild<QWidget*>(QStringLiteral("bottomLeftPanel"));
        QVERIFY(corner); QCOMPARE(toggle->parentWidget(),corner);
        const auto capture = [&](const QString& name) {
            const auto folder=qEnvironmentVariable("OKUFLOW_VIDEO_TRANSPORT_PREVIEWS");
            if(folder.isEmpty())return;
            QVERIFY(QDir().mkpath(folder));
            QVERIFY(window.grab().save(QDir(folder).filePath(name+"-window.png")));
            QVERIFY(corner->grab().save(QDir(folder).filePath(name+"-corner.png")));
        };
        for (int width : {1600,800}) {
            window.resize(width,800); window.show(); QCoreApplication::processEvents();
            VideoTransport::Snapshot snapshot; snapshot.source=VideoTransport::Source::Video;
            snapshot.sourceName=QStringLiteral("Fixture video");snapshot.durationMs=60000;
            snapshot.seekable=true;snapshot.canSaveFrame=true;
            transport->SetSnapshot(snapshot); QCoreApplication::processEvents();
            QVERIFY(transport->isVisible()); QVERIFY(toggle->isVisible());
            QVERIFY(toggle->width()>=44 && toggle->height()>=44);
            QVERIFY(corner->rect().contains(toggle->geometry()));
            const auto height=window.renderWidget()->height();
            capture(QString::number(width)+"-expanded");
            QSignalSpy play(transport,&VideoTransport::PlayPauseRequested);
            QSignalSpy seek(transport,&VideoTransport::SeekRequested);
            QSignalSpy jump(transport,&VideoTransport::JumpToEndRequested);
            toggle->click();QCoreApplication::processEvents();
            QVERIFY(transport->isHidden()); QVERIFY(toggle->isVisible());
            QVERIFY(window.renderWidget()->height()>height);
            QVERIFY(transport->FocusTargets().empty());
            QVERIFY(corner->rect().contains(toggle->geometry()));
            capture(QString::number(width)+"-hidden");
            snapshot.positionMs=12000;transport->SetSnapshot(snapshot);QCoreApplication::processEvents();
            QVERIFY(transport->isHidden()); // metadata polling cannot reopen the bar
            window.setUiHidden(true);window.setUiHidden(false);QCoreApplication::processEvents();
            QVERIFY(transport->isHidden());QVERIFY(toggle->isVisible());
            toggle->setFocus();QTest::keyClick(toggle,Qt::Key_Space);QCoreApplication::processEvents();
            QVERIFY(transport->isVisible());QCOMPARE(transport->timeline()->value(),12000);
            QCOMPARE(play.size(),0);QCOMPARE(seek.size(),0);QCOMPARE(jump.size(),0);
            transport->SetSnapshot({});QCoreApplication::processEvents();
            QVERIFY(transport->isHidden());QVERIFY(toggle->isHidden());
        }
    }
    void terminalRecapResumeRetainsHeldScene_data() {
        QTest::addColumn<bool>("unsupported");QTest::addColumn<bool>("oldHistory");
        QTest::newRow("failed-cold-buffer")<<false<<false;
        QTest::newRow("failed-after-published-tail")<<false<<true;
        QTest::newRow("unsupported-source")<<true<<false;
    }
    void terminalRecapResumeRetainsHeldScene() {
        QFETCH(bool,unsupported);QFETCH(bool,oldHistory);
        const auto cleanup=qScopeGuard([&]{NativeControlFixture::SetCameraActive(app_,false);
            NativeControlFixture::ResetRecap(app_);NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::ResetVideo(app_);std::vector<std::uint8_t> pixels(32*16*4,47);
        NativeControlFixture::SeedRecapSceneWithoutRecording(app_,pixels,unsupported);
        NativeControlFixture::ToggleVideo(app_);
        const auto held=NativeControlFixture::HeldVideoFrame(app_);
        const auto stamp=NativeControlFixture::RecapCaptureStamp(app_);
        QVERIFY(held && held->bgra);
        NativeControlFixture::ToggleVideo(app_);
        VideoRecapSnapshot snapshot;snapshot.captureOrigin100ns=0;
        snapshot.available=oldHistory;snapshot.first100ns=0;snapshot.end100ns=100'000;
        // This old published tail is before the exact held capture, so no
        // decoder or fake admission is needed for this app-state regression.
        snapshot.failed=!unsupported;
        if(!unsupported)snapshot.error="The recording's final fragment could not be completed (hr=0x80030070). The RAM segment cannot be sealed.";
        NativeControlFixture::ApplyRecapSnapshot(app_,snapshot);
        const auto ui=NativeControlFixture::VideoUi(app_);
        QCOMPARE(ui.state,VideoTransport::State::Unavailable);
        QVERIFY(!ui.playing);
        QVERIFY(!NativeControlFixture::VideoState(app_)["playing"].toBool());
        QCOMPARE(NativeControlFixture::HeldVideoFrame(app_),held);
        QCOMPARE(NativeControlFixture::RecapCaptureStamp(app_),stamp);
        QVERIFY(std::all_of(held->bgra->begin(),held->bgra->end(),[](auto b){return b==47;}));
        QVERIFY(NativeControlFixture::Window(app_).videoTransport()->IsActionReady(
            NativeControlFixture::Window(app_).videoTransport()->jumpButton()));
        if(!unsupported)QCOMPARE(ui.diagnosticDetail,QString::fromStdString(snapshot.error));
    }
    void warmingRecapResumeStillWaitsForGenuineHistory() {
        const auto cleanup=qScopeGuard([&]{NativeControlFixture::SetCameraActive(app_,false);
            NativeControlFixture::ResetRecap(app_);NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::ResetVideo(app_);std::vector<std::uint8_t> pixels(32*16*4,47);
        NativeControlFixture::SeedRecapSceneWithoutRecording(app_,pixels);
        NativeControlFixture::ToggleVideo(app_);const auto held=NativeControlFixture::HeldVideoFrame(app_);
        NativeControlFixture::ToggleVideo(app_);
        VideoRecapSnapshot snapshot;snapshot.captureOrigin100ns=0;snapshot.error="Live recap native encoder is still busy; capture continues.";
        NativeControlFixture::ApplyRecapSnapshot(app_,snapshot);
        QCOMPARE(NativeControlFixture::VideoUi(app_).state,VideoTransport::State::Buffering);
        QVERIFY(NativeControlFixture::VideoState(app_)["playing"].toBool());
        QCOMPARE(NativeControlFixture::HeldVideoFrame(app_),held);
    }
    void deferredRecapReceiptKeepsCommittedClockAcrossIngressAndRedraw_data() {
        QTest::addColumn<bool>("cloneUnavailable");
        QTest::newRow("deferred-first-receipt")<<false;
        QTest::newRow("clone-pressure-then-retry")<<true;
    }
    void deferredRecapReceiptKeepsCommittedClockAcrossIngressAndRedraw() {
        QFETCH(bool,cloneUnavailable);
        const auto config=NativeControlFixture::CurrentImageConfig(app_);
        const auto cleanup=qScopeGuard([&]{NativeControlFixture::SetCameraActive(app_,false);
            NativeControlFixture::ResetRecap(app_);NativeControlFixture::ResetVideo(app_);
            NativeControlFixture::RestoreImageConfig(app_,config);});
        NativeControlFixture::ResetVideo(app_);std::vector<std::uint8_t> pixels(32*16*4,47);
        NativeControlFixture::SeedRecapSceneWithoutRecording(app_,pixels,false,true);
        NativeControlFixture::AdvanceRecapIngress(app_);
        NativeControlFixture::CommitRecapReceipt(app_,cloneUnavailable?nullptr:pixels.data(),1);
        auto receipt=NativeControlFixture::RecapCaptureStamp(app_);
        QCOMPARE(receipt["sequence"].toInteger(),qint64(91));QCOMPARE(receipt["clock"].toInteger(),qint64(9'100'000));
        QCOMPARE(receipt["hasPixels"].toBool(),!cloneUnavailable);
        // Pending latency metadata is gone. Retry/reallocate the same actual
        // capture with a new scene allocation while newer ingress remains 92.
        std::fill(pixels.begin(),pixels.end(),95);
        NativeControlFixture::CommitRecapReceipt(app_,pixels.data(),2);
        receipt=NativeControlFixture::RecapCaptureStamp(app_);
        QCOMPARE(receipt["sequence"].toInteger(),qint64(91));QCOMPARE(receipt["clock"].toInteger(),qint64(9'100'000));
        NativeControlFixture::CommitRecapReceipt(app_,pixels.data(),2); // unchanged same-scene redraw
        QCOMPARE(NativeControlFixture::RecapCaptureStamp(app_),receipt);
        NativeControlFixture::ToggleVideo(app_);
        const auto held=NativeControlFixture::HeldVideoFrame(app_);QVERIFY(held && held->bgra);
        QCOMPARE(held->sampleId,std::uint64_t(91));
        QVERIFY(std::all_of(held->bgra->begin(),held->bgra->end(),[](auto byte){return byte==95;}));
        QCOMPARE(NativeControlFixture::VideoState(app_)["provisionalCaptureClock100ns"].toInteger(),qint64(9'100'000));
        // A source reset removes clock authority even if its sequence happens
        // to match. No pending proof means explicit Pause refusal, no waiter.
        NativeControlFixture::ResetRecap(app_);
        NativeControlFixture::SeedRecapSceneWithoutRecording(app_,pixels,false,true);
        NativeControlFixture::ForgetPendingRecapClock(app_);
        NativeControlFixture::AdvanceRecapIngress(app_);
        NativeControlFixture::CommitRecapReceipt(app_,pixels.data(),3);
        receipt=NativeControlFixture::RecapCaptureStamp(app_);
        QCOMPARE(receipt["sequence"].toInteger(),qint64(91));QCOMPARE(receipt["clock"].toInteger(),qint64(-1));
        QVERIFY(!NativeControlFixture::VideoUi(app_).canPauseLive);
        NativeControlFixture::ToggleVideo(app_);
        QVERIFY(!NativeControlFixture::HeldVideoFrame(app_));
        QVERIFY(!NativeControlFixture::VideoState(app_)["pendingHold"].toBool());
        QCOMPARE(NativeControlFixture::VideoState(app_)["state"].toString(),QStringLiteral("live"));
        QVERIFY(NativeControlFixture::Window(app_).videoTransport()->IsActionReady(
            NativeControlFixture::Window(app_).videoTransport()->jumpButton()));
    }
    void oversizedNewPresentationRevokesPreviousExactFreeze() {
        const auto cleanup=qScopeGuard([&]{NativeControlFixture::SetCameraActive(app_,false);
            NativeControlFixture::ResetRecap(app_);NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::ResetVideo(app_);std::vector<std::uint8_t> pixels(32*16*4,47);
        NativeControlFixture::SeedRecapSceneWithoutRecording(app_,pixels);
        QVERIFY(NativeControlFixture::VideoUi(app_).canPauseLive);
        NativeControlFixture::OversizedPresentedScene(app_);
        QVERIFY(!NativeControlFixture::VideoUi(app_).canPauseLive);
        NativeControlFixture::ToggleVideo(app_);
        QVERIFY(!NativeControlFixture::HeldVideoFrame(app_));
    }
    void stopAndFinalizationKeepPresentedRecordingCursor_data() {
        QTest::addColumn<bool>("playing");
        QTest::newRow("paused")<<false;QTest::newRow("playing")<<true;
    }
    void stopAndFinalizationKeepPresentedRecordingCursor() {
        QFETCH(bool,playing);
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        RecordingSessionInfo session;session.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        NativeControlFixture::SeedVideo(app_,session,OwnedVideoPolicyFrame(),true,playing);
        const auto before=NativeControlFixture::VideoState(app_);
        NativeControlFixture::VideoStopping(app_,session);
        QCOMPARE(NativeControlFixture::VideoState(app_)["selectedPts100ns"],before["selectedPts100ns"]);
        QCOMPARE(NativeControlFixture::VideoState(app_)["playing"].toBool(),playing);
        SavedRecordingSegment part;part.session=session;part.originalPath="C:/private-fixture/saved-part.mp4";
        NativeControlFixture::VideoSaved(app_,part);
        session.completion=RecordingSessionCompletion::Completed;
        NativeControlFixture::VideoEnded(app_,session);
        const auto after=NativeControlFixture::VideoState(app_);
        QCOMPARE(after["documentId"],before["documentId"]);
        QCOMPARE(after["selectedPts100ns"],before["selectedPts100ns"]);
        QCOMPARE(after["frameSequence"],before["frameSequence"]);
        QCOMPARE(after["playing"].toBool(),playing);
        QCOMPARE(after["selectionGeneration"],before["selectionGeneration"]);
    }
    void staleRecordingCallbackDoesNotStealSelectedFile() {
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        RecordingSessionInfo old;old.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        NativeControlFixture::SeedVideo(app_,old,OwnedVideoPolicyFrame(),false,false);
        const auto before=NativeControlFixture::VideoState(app_);
        SavedRecordingSegment part;part.session=old;part.originalPath="C:/private-fixture/late.mp4";
        NativeControlFixture::VideoSaved(app_,part);NativeControlFixture::VideoEnded(app_,old);
        const auto after=NativeControlFixture::VideoState(app_);
        QCOMPARE(after["kind"].toString(),QStringLiteral("file"));
        QCOMPARE(after["documentId"],before["documentId"]);
        QCOMPARE(after["selectedPts100ns"],before["selectedPts100ns"]);
        QVERIFY(!NativeControlFixture::DeniedRecording(app_));
        QCOMPARE(NativeControlFixture::VideoState(app_)["documentId"],before["documentId"]);
        QCOMPARE(NativeControlFixture::VideoState(app_)["kind"].toString(),QStringLiteral("file"));
    }
    void cameraOfflineFileUsesNativeDecoderAndStartsPaused() {
        QTemporaryDir folder;QVERIFY(folder.isValid());
        const QString path=folder.filePath(QStringLiteral("private lecture ü.avi"));
        QVERIFY(WritePrivateVideoFixture(path));
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::ResetVideo(app_);
        QVERIFY(!NativeControlFixture::CameraActive(app_));
        bool pulse=false;QTimer::singleShot(0,&app_,[&] {pulse=true;});
        NativeControlFixture::OpenVideo(app_,path);
        QTRY_VERIFY(pulse);
        QTRY_COMPARE_WITH_TIMEOUT(NativeControlFixture::VideoState(app_)["kind"].toString(),QStringLiteral("file"),5000);
        const auto state=NativeControlFixture::VideoState(app_);
        QCOMPARE(state["sourceName"].toString(),QFileInfo(path).fileName());
        QCOMPARE(state["state"].toString(),QStringLiteral("paused"));
        QVERIFY(!state["playing"].toBool());
        QVERIFY(!NativeControlFixture::CameraActive(app_));
        // Offscreen has no D3D presenter: decode is not a presentation claim.
        QVERIFY(!state["hasFrame"].toBool());
        NativeControlFixture::ResetVideo(app_);
        bool removed=false;QTRY_VERIFY_WITH_TIMEOUT(removed || (removed=QFile::remove(path)),3000);
    }
    void selectedVideoOrientationPreservesCameraPreferenceAndIdentity() {
        QTemporaryDir folder;QVERIFY(folder.isValid());
        const auto path=folder.filePath(QStringLiteral("orientation.avi"));
        QVERIFY(WritePrivateVideoFixture(path));
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ReturnVideo(app_);NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::ResetVideo(app_);
        const int cameraTurns=NativeControlFixture::CameraOrientation(app_);
        NativeControlFixture::OpenVideo(app_,path);
        QTRY_COMPARE_WITH_TIMEOUT(NativeControlFixture::VideoState(app_)["kind"].toString(),QStringLiteral("file"),5000);
        auto& window=NativeControlFixture::Window(app_);
        QVERIFY(window.videoSourceSelected());QVERIFY(!window.openVideoButton()->isHidden());
        const auto identity=NativeControlFixture::VideoState(app_)["documentId"];
        const auto pts=NativeControlFixture::SelectedVideoFrame(app_)->pts100ns;
        for(int turns:{1,2,3}) {
            NativeControlFixture::VideoOrientation(app_,turns);
            QCOMPARE(window.rotationCombo()->currentIndex(),turns);
            QCOMPARE(NativeControlFixture::VideoRecipeOrientation(app_),turns);
            QCOMPARE(NativeControlFixture::CameraOrientation(app_),cameraTurns);
            QCOMPARE(NativeControlFixture::SavedOrientation(app_),cameraTurns);
            QCOMPARE(NativeControlFixture::VideoState(app_)["documentId"],identity);
            QCOMPARE(NativeControlFixture::SelectedVideoFrame(app_)->pts100ns,pts);
        }
        NativeControlFixture::ReturnVideo(app_);
        QVERIFY(!window.videoSourceSelected());QVERIFY(window.openVideoButton()->isHidden());
        QCOMPARE(window.rotationCombo()->currentIndex(),cameraTurns);
    }
    void pendingRecordingOpenWaitsForMatchingSessionAndNewestFileWins() {
        QTemporaryDir folder;QVERIFY(folder.isValid());
        const auto newest=folder.filePath(QStringLiteral("newest.avi"));
        const auto obsolete=folder.filePath(QStringLiteral("obsolete-missing.mp4"));
        QVERIFY(WritePrivateVideoFixture(newest));
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        RecordingSessionInfo session;session.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        NativeControlFixture::PendingVideoFinalization(app_,session);
        NativeControlFixture::OpenVideo(app_,obsolete);
        const QJsonValue request=NativeControlFixture::VideoState(app_)["requestGeneration"];
        NativeControlFixture::OpenVideo(app_,newest);
        QCOMPARE(NativeControlFixture::PendingVideoOpen(app_),QDir::cleanPath(newest));
        QVERIFY(!NativeControlFixture::VideoStartAllowed(app_));
        QCOMPARE(NativeControlFixture::VideoState(app_)["requestGeneration"],request);
        auto unrelated=session;unrelated.id="different-session";
        NativeControlFixture::VideoEnded(app_,unrelated);
        QCOMPARE(NativeControlFixture::VideoState(app_)["requestGeneration"],request);
        QCOMPARE(NativeControlFixture::PendingVideoOpen(app_),QDir::cleanPath(newest));
        NativeControlFixture::ClearVideoFinalization(app_);
        session.completion=RecordingSessionCompletion::Completed;
        NativeControlFixture::VideoEnded(app_,session);
        QVERIFY(NativeControlFixture::PendingVideoOpen(app_).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(NativeControlFixture::VideoState(app_)["kind"].toString(),QStringLiteral("file"),5000);
        QCOMPARE(NativeControlFixture::VideoState(app_)["sourceName"].toString(),QFileInfo(newest).fileName());
        NativeControlFixture::ResetVideo(app_);
        bool removed=false;QTRY_VERIFY_WITH_TIMEOUT(removed || (removed=QFile::remove(newest)),3000);
    }
    void sameExtentRotationReprocessesPausedGpuSceneWithUctOff() {
        QVERIFY(NativeControlFixture::SameExtentRotationReprocessesWithUctOff(app_));
    }
    void validUctLeaseSurvivesViewReplaysAndRetryStaysBounded() {
        QVERIFY(NativeControlFixture::ValidUctLeaseSurvivesViewReplays(app_));
    }
    void pausedMediaAnalysisRetryIsBoundedAndStopsOnPoolFailure() {
        QVERIFY(NativeControlFixture::PausedMediaAnalysisRetryIsBoundedPerFrame(app_));
    }
    void openingDifferentVideoCannotInheritPreviousDisplayReceipt() {
        QTemporaryDir folder;QVERIFY(folder.isValid());
        const auto path=folder.filePath(QStringLiteral("new-document.avi"));
        QVERIFY(WritePrivateVideoFixture(path));
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::SeedVideo(app_,{},OwnedVideoPolicyFrame(),false,false);
        const auto previous=NativeControlFixture::VideoState(app_);
        QVERIFY(previous["hasFrame"].toBool());
        NativeControlFixture::OpenVideo(app_,path);
        QTRY_VERIFY_WITH_TIMEOUT(NativeControlFixture::VideoState(app_)["documentId"]!=previous["documentId"],5000);
        QTRY_VERIFY_WITH_TIMEOUT(NativeControlFixture::SelectedVideoFrame(app_),5000);
        const auto selected=NativeControlFixture::VideoState(app_);
        // This fixture decodes, but never presents a native frame. A new file
        // must await its own display receipt even when the old file had one.
        QVERIFY(!selected["hasFrame"].toBool());
        QVERIFY(!selected["canSaveFrame"].toBool());
        QCOMPARE(selected["frameSequence"].toInteger(),qint64(0));
        QCOMPARE(selected["completedRequestGeneration"].toInteger(),qint64(0));
        QVERIFY(!NativeControlFixture::VideoUi(app_).canSaveFrame);
        NativeControlFixture::ResetVideo(app_);
        bool removed=false;QTRY_VERIFY_WITH_TIMEOUT(removed || (removed=QFile::remove(path)),3000);
    }
    void transportGenerationIsPerDocumentAndShowsPendingSeekTarget() {
        QTemporaryDir folder;QVERIFY(folder.isValid());
        const auto path=folder.filePath(QStringLiteral("transport.avi"));
        QVERIFY(WritePrivateVideoFixture(path));
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::ResetVideo(app_);
        const auto camera=NativeControlFixture::VideoUi(app_).sourceGeneration;
        NativeControlFixture::OpenVideo(app_,path);
        QTRY_COMPARE_WITH_TIMEOUT(NativeControlFixture::VideoState(app_)["kind"].toString(),QStringLiteral("file"),5000);
        QTRY_COMPARE_WITH_TIMEOUT(NativeControlFixture::VideoState(app_)["state"].toString(),QStringLiteral("paused"),5000);
        const auto opened=NativeControlFixture::VideoUi(app_);
        QCOMPARE(opened.source,VideoTransport::Source::Video);
        QVERIFY(opened.sourceGeneration!=camera);
        const QJsonValue documentId=NativeControlFixture::VideoState(app_)["documentId"];
        const QJsonValue selection=NativeControlFixture::VideoState(app_)["selectionGeneration"];
        NativeControlFixture::SeekVideo(app_,250);
        // Selection authority advances; the transport document does not.
        QVERIFY(NativeControlFixture::VideoState(app_)["selectionGeneration"]!=selection);
        QCOMPARE(NativeControlFixture::VideoUi(app_).sourceGeneration,opened.sourceGeneration);
        QCOMPARE(NativeControlFixture::VideoUi(app_).positionMs,qint64(250));
        // A second relative seek before the first is shown accumulates from it.
        NativeControlFixture::SeekVideo(app_,NativeControlFixture::VideoUi(app_).positionMs-100);
        QCOMPARE(NativeControlFixture::VideoUi(app_).positionMs,qint64(150));
        QTRY_COMPARE_WITH_TIMEOUT(NativeControlFixture::VideoState(app_)["state"].toString(),QStringLiteral("paused"),5000);
        // Offscreen decode is not a presentation, so the target remains shown.
        QCOMPARE(NativeControlFixture::VideoUi(app_).positionMs,qint64(150));
        NativeControlFixture::ToggleVideo(app_);NativeControlFixture::ToggleVideo(app_);
        NativeControlFixture::JumpVideo(app_);
        QCOMPARE(NativeControlFixture::VideoUi(app_).sourceGeneration,opened.sourceGeneration);
        QCOMPARE(NativeControlFixture::VideoState(app_)["documentId"],documentId);
        NativeControlFixture::ResetVideo(app_); // Return to camera is a document change
        QVERIFY(NativeControlFixture::VideoUi(app_).sourceGeneration!=opened.sourceGeneration);
        bool removed=false;QTRY_VERIFY_WITH_TIMEOUT(removed || (removed=QFile::remove(path)),3000);
    }
    void selectedMediaPollingExpiresPendingAnnotationReadbackWithoutRetargeting() {
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::ClearAnnotation(app_);NativeControlFixture::ResetVideo(app_);
        });
        NativeControlFixture::SeedVideo(app_,{},OwnedVideoPolicyFrame(),false,false);
        const auto original=NativeControlFixture::PrimeAnnotation(app_);
        NativeControlFixture::SubmitPrimedAnnotationReadback(app_);
        QVERIFY(NativeControlFixture::AnnotationReadbackPending(app_));
        QElapsedTimer clock;clock.start();
        // The seeded document's real 16-ms playback timer drives
        // PollVideoPlayback, whose first act is the shared one-second
        // readback expiry. Offscreen never produces a presenter receipt.
        QTest::qWait(120);
        QVERIFY(NativeControlFixture::AnnotationReadbackPending(app_)); // deadline not yet reached
        QTRY_VERIFY_WITH_TIMEOUT(NativeControlFixture::AnnotationReadbackExpired(app_),5000);
        QVERIFY(clock.elapsed()>=900); // the full one-second intent lifetime elapsed
        // The expired intent stays dead: later polling never retargets it
        // onto a subsequent frame with the same geometry.
        QVERIFY(!NativeControlFixture::AnnotationMatches(app_,original));
        QTest::qWait(120);
        QVERIFY(NativeControlFixture::AnnotationReadbackExpired(app_));
        QVERIFY(!NativeControlFixture::AnnotationMatches(app_,original));
    }
    void pausedMediaFallbackAdoptsCpuFrameAtDisplayedPoint() {
        QTemporaryDir folder;QVERIFY(folder.isValid());
        const auto path=folder.filePath(QStringLiteral("fallback.avi"));
        QVERIFY(WritePrivateVideoFixture(path));
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetVideo(app_);});
        NativeControlFixture::ResetVideo(app_);
        NativeControlFixture::OpenVideo(app_,path);
        QTRY_COMPARE_WITH_TIMEOUT(NativeControlFixture::VideoState(app_)["kind"].toString(),QStringLiteral("file"),5000);
        QTRY_VERIFY_WITH_TIMEOUT(NativeControlFixture::SelectedVideoFrame(app_),5000);
        const auto displayed=NativeControlFixture::SelectedVideoFrame(app_);
        QCOMPARE(displayed->pts100ns,std::int64_t(0));
        // The decoder advances beyond the frame that is modelled as displayed.
        NativeControlFixture::SeekVideo(app_,200);
        QTRY_VERIFY_WITH_TIMEOUT(NativeControlFixture::SelectedVideoFrame(app_) &&
            NativeControlFixture::SelectedVideoFrame(app_)->pts100ns==2'000'000,5000);
        QVERIFY(NativeControlFixture::HoldPresentedVideoFrame(app_,displayed)==displayed);
        const auto held=NativeControlFixture::VideoState(app_);
        const auto ahead=NativeControlFixture::DecoderSnapshot(app_).frame;
        QVERIFY(ahead);QCOMPARE(ahead->pts100ns,std::int64_t(2'000'000));
        const std::string reason="fixture: selected media CUDA scene failed";
        NativeControlFixture::MediaSceneFailed(app_,reason);
        const auto request=NativeControlFixture::VideoState(app_)["requestGeneration"].toInteger();
        QVERIFY(request>held["requestGeneration"].toInteger());
        // The paused hold no longer blocks adoption of the CPU result, which
        // re-decodes the displayed point rather than the decoder-ahead frame.
        QTRY_VERIFY_WITH_TIMEOUT(NativeControlFixture::SelectedVideoFrame(app_) &&
            qint64(NativeControlFixture::SelectedVideoFrame(app_)->requestGeneration)==request,5000);
        const auto adopted=NativeControlFixture::SelectedVideoFrame(app_);
        QCOMPARE(adopted->pts100ns,displayed->pts100ns);
        QVERIFY(adopted!=displayed);
        QVERIFY(adopted->bgra && !adopted->gpu);
        const auto decoder=NativeControlFixture::DecoderSnapshot(app_);
        QVERIFY(!decoder.gpuResident);
        QCOMPARE(QString::fromStdString(decoder.backendReason),QString::fromStdString(reason));
        const auto state=NativeControlFixture::VideoState(app_);
        QCOMPARE(state["state"].toString(),QStringLiteral("paused"));
        QVERIFY(!state["playing"].toBool());
        QCOMPARE(state["documentId"],held["documentId"]);
        // The modeled old display survives fallback. Offscreen cannot present
        // the replacement, so its new request must not receive a display receipt.
        QVERIFY(state["hasFrame"].toBool());
        QCOMPARE(state["frameSequence"],held["frameSequence"]);
        QCOMPARE(state["completedRequestGeneration"],held["completedRequestGeneration"]);
        QVERIFY(state["completedRequestGeneration"].toInteger()!=request);
        NativeControlFixture::ResetVideo(app_);
        bool removed=false;QTRY_VERIFY_WITH_TIMEOUT(removed || (removed=QFile::remove(path)),3000);
    }
#endif
    void nativeImageEditsAdvanceProcessingAuthorityWithoutAgent() {
        const auto original=NativeControlFixture::CurrentImageConfig(app_);
        const auto restore=qScopeGuard([&] {NativeControlFixture::RestoreImageConfig(app_,original);});
        const auto before=NativeControlFixture::ProcessingGeneration(app_);
        NativeControlFixture::NativeContrastWithoutAgent(app_,140);
        QVERIFY(NativeControlFixture::ProcessingGeneration(app_)>before);
        const auto changed=NativeControlFixture::ProcessingGeneration(app_);
        NativeControlFixture::NativeZoom(app_);
        QVERIFY(NativeControlFixture::ProcessingGeneration(app_)>changed);
    }
    void annotationReadbackNeverRetargetsChangedSceneOrTransform() {
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ClearAnnotation(app_);});
        const auto original=NativeControlFixture::PrimeAnnotation(app_);
        QVERIFY(NativeControlFixture::AnnotationMatches(app_,original));
        auto later=original;++later.frameSeq;
        QVERIFY(!NativeControlFixture::AnnotationMatches(app_,later));
        later=original;later.transform.sourceWidth=0.5f;
        QVERIFY(!NativeControlFixture::AnnotationMatches(app_,later));
        later=original;++later.geometry.settings.sourceEpoch;
        QVERIFY(!NativeControlFixture::AnnotationMatches(app_,later));
        later=original;later.viewportWidth=1200;
        QVERIFY(!NativeControlFixture::AnnotationMatches(app_,later));
        later=original;++later.requested.rotationQuarterTurns;
        QVERIFY(!NativeControlFixture::AnnotationMatches(app_,later));
        // Reprocessing the same camera sequence with different effects is
        // different pixels even when the spatial OCR key remains identical.
        NativeControlFixture::AnnotationSceneReprocessed(app_);
        QVERIFY(!NativeControlFixture::AnnotationMatches(app_,original));
    }
    void annotationReadbackRejectsChangedSrTextureAndPresentationOptions() {
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ClearAnnotation(app_);});
        QVERIFY(NativeControlFixture::AnnotationGpuSignatureRejectsChangedOutput(app_));
    }
    void sourceRetirementRevokesPendingAnnotationReadback() {
        NativeControlFixture::PrimeAnnotation(app_);
        // Exercise the production retirement entry, including its no-camera
        // early return. No device or native presenter is created by this fixture.
        NativeControlFixture::StopCamera(app_);
        QVERIFY(NativeControlFixture::AnnotationEmpty(app_));
    }
    void deferredPreferenceWaitUsesTheSameStatusProjection() {
        auto& target=NativeControlFixture::Target(app_);
        const auto id=NativeControlFixture::DeferredPreferenceOperation(app_);
        QVERIFY(target.Query(ToolId::Operation,{{"operationId",id}})["preferencesPending"].toBool());
        std::optional<ControlResult> result;
        const auto wait=[&] {
            result.reset();
            target.Execute(ToolId::Wait,{{"condition","operation_state"},{"operationId",id},{"timeoutMs",75}},
                "wait-fixture",ContextKind::EmbeddedTurn,[&](ControlResult value){result=std::move(value);});
        };
        wait();QTRY_VERIFY(result.has_value());
        QVERIFY(result->content["ok"].toBool());
        QVERIFY(!result->content["met"].toBool());
        wait();
        QTimer::singleShot(30,&app_,[&]{NativeControlFixture::CompletePreferenceOperation(app_,id);});
        QTRY_VERIFY(result.has_value());
        QVERIFY(result->content["met"].toBool());
        QVERIFY(!target.Query(ToolId::Operation,{{"operationId",id}})["preferencesPending"].toBool());
    }
    void physicalCameraOpenStaysOffUiAndStopRetiresItsOwnWorker_data() {
        QTest::addColumn<int>("origin");
        QTest::newRow("manual-reopen")<<int(CameraSourceOrigin::Manual);
        QTest::newRow("reconnect")<<int(CameraSourceOrigin::Reconnect);
        QTest::newRow("agent-format")<<int(CameraSourceOrigin::AgentTemporary);
    }
    void physicalCameraOpenStaysOffUiAndStopRetiresItsOwnWorker() {
        QFETCH(int,origin);
        struct Gate {std::atomic<bool> entered{},release{},onUi{},sawCancel{};};
        auto gate=std::make_shared<Gate>();const auto ui=std::this_thread::get_id();
        const auto cleanup=qScopeGuard([&] {
            gate->release=true;NativeControlFixture::StopCamera(app_);
            QElapsedTimer clock;clock.start();
            while(NativeControlFixture::CameraPending(app_) && clock.elapsed()<3000) {
                QCoreApplication::processEvents();QTest::qWait(1);
            }
            NativeControlFixture::ResetCameraFixture(app_);
        });
        const bool accepted=NativeControlFixture::StartBlockedCamera(app_,CameraSourceOrigin(origin),
            [gate,ui](const std::atomic<bool>& canceled) {
                gate->onUi=std::this_thread::get_id()==ui;gate->entered=true;
                const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(!gate->release && std::chrono::steady_clock::now()<until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                gate->sawCancel=canceled.load();return false;
            });
        QVERIFY(accepted);QTRY_VERIFY(gate->entered.load());QVERIFY(!gate->onUi);
        bool pulse=false;QTimer::singleShot(0,&app_,[&] {pulse=true;});QTRY_VERIFY(pulse);
        NativeControlFixture::StopCamera(app_);
        QVERIFY(NativeControlFixture::CameraPending(app_));
        pulse=false;QTimer::singleShot(0,&app_,[&] {pulse=true;});QTRY_VERIFY(pulse);
        gate->release=true;QTRY_VERIFY(!NativeControlFixture::CameraPending(app_));
        QVERIFY(gate->sawCancel);
    }
    void asyncReconnectFailureSchedulesRetryRatherThanAnnouncingSuccess() {
        const auto cleanup=qScopeGuard([&] {NativeControlFixture::ResetCameraFixture(app_);});
        NativeControlFixture::BeginReconnect(app_);
        QVERIFY(NativeControlFixture::StartBlockedCamera(app_,CameraSourceOrigin::Reconnect,
            [](const std::atomic<bool>&) {return false;}));
        QVERIFY(NativeControlFixture::ReconnectPending(app_));
        QTRY_VERIFY(!NativeControlFixture::CameraPending(app_));
        QVERIFY(NativeControlFixture::ReconnectPending(app_));
        QCOMPARE(NativeControlFixture::ReconnectAttempts(app_),1);
    }
    void reconnectRequiresFrameAndSilentAdoptionExpiresWithoutReopenChurn() {
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::StopCamera(app_); QElapsedTimer deadline;deadline.start();
            while(NativeControlFixture::CameraPending(app_) && deadline.elapsed()<2000) {
                QCoreApplication::processEvents();QTest::qWait(1);
            }
            NativeControlFixture::ResetCameraFixture(app_);
        });
        const auto opens=std::make_shared<std::atomic<int>>(0);
        NativeControlFixture::BeginReconnect(app_);
        QVERIFY(NativeControlFixture::StartBlockedCamera(app_,CameraSourceOrigin::Reconnect,
            [opens](const std::atomic<bool>&) {++*opens;return true;}));
        QTRY_VERIFY(!NativeControlFixture::CameraPending(app_));
        QVERIFY(NativeControlFixture::CameraActive(app_));
        QVERIFY(NativeControlFixture::ReconnectPending(app_)); // adoption is not video
        QVERIFY(NativeControlFixture::ReconnectDue(app_)); // exercise the active-reader guard, not the backoff guard
        for(int tick=0;tick<20;++tick)NativeControlFixture::DriveReconnect(app_);
        QCOMPARE(opens->load(),1);
        QVERIFY(NativeControlFixture::CameraActive(app_));
        QVERIFY(!NativeControlFixture::CameraPending(app_)); // no hidden stop/reopen queued
        NativeControlFixture::PresentedCameraFrame(app_);
        QVERIFY(!NativeControlFixture::ReconnectPending(app_));
        NativeControlFixture::PresentedCameraFrame(app_); // completion stays idempotent
        QVERIFY(!NativeControlFixture::ReconnectPending(app_));
        NativeControlFixture::StopCamera(app_);
        QTRY_VERIFY(!NativeControlFixture::CameraPending(app_));
        NativeControlFixture::BeginReconnect(app_);
        QVERIFY(NativeControlFixture::StartBlockedCamera(app_,CameraSourceOrigin::Reconnect,
            [opens](const std::atomic<bool>&) {++*opens;return true;}));
        QTRY_VERIFY(!NativeControlFixture::CameraPending(app_));
        QVERIFY(NativeControlFixture::CameraActive(app_));
        QVERIFY(NativeControlFixture::ReconnectPending(app_));
        // A failed active attempt is retired without renewing the recovery owner.
        NativeControlFixture::LoseActiveReconnect(app_);
        QVERIFY(!NativeControlFixture::CameraActive(app_));
        QVERIFY(NativeControlFixture::ReconnectPending(app_));
        QCOMPARE(NativeControlFixture::ReconnectAttempts(app_),1);
        QTRY_VERIFY(!NativeControlFixture::CameraPending(app_));
        QVERIFY(NativeControlFixture::StartBlockedCamera(app_,CameraSourceOrigin::Reconnect,
            [opens](const std::atomic<bool>&) {++*opens;return true;}));
        QTRY_VERIFY(!NativeControlFixture::CameraPending(app_));
        QVERIFY(NativeControlFixture::CameraActive(app_));
        // This successful adoption never delivers a frame and must expire.
        NativeControlFixture::ExpireReconnect(app_);
        NativeControlFixture::DriveReconnect(app_);
        QVERIFY(!NativeControlFixture::ReconnectPending(app_));
        QVERIFY(!NativeControlFixture::CameraActive(app_));
        for(int tick=0;tick<20;++tick)NativeControlFixture::DriveReconnect(app_);
        QCOMPARE(opens->load(),3);
        QTRY_VERIFY(!NativeControlFixture::CameraPending(app_));
    }
    void nonReconnectVideoRetiresOldRecovery_data() {
        QTest::addColumn<int>("origin");
        QTest::newRow("fallback")<<int(CameraSourceOrigin::Fallback);
        QTest::newRow("manual-format")<<int(CameraSourceOrigin::Manual);
    }
    void nonReconnectVideoRetiresOldRecovery() {
        QFETCH(int,origin);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::StopCamera(app_);QElapsedTimer deadline;deadline.start();
            while(NativeControlFixture::CameraPending(app_) && deadline.elapsed()<2000) {
                QCoreApplication::processEvents();QTest::qWait(1);
            }
            NativeControlFixture::ResetCameraFixture(app_);
        });
        NativeControlFixture::BeginReconnect(app_);
        QVERIFY(NativeControlFixture::StartBlockedCamera(app_,CameraSourceOrigin(origin),
            [](const std::atomic<bool>&) {return true;}));
        QTRY_VERIFY(!NativeControlFixture::CameraPending(app_));
        QVERIFY(NativeControlFixture::ReconnectPending(app_));
        NativeControlFixture::PresentedCameraFrame(app_);
        QVERIFY(!NativeControlFixture::ReconnectPending(app_));
        NativeControlFixture::DriveReconnect(app_);
        QVERIFY(NativeControlFixture::CameraActive(app_));
    }
    void noSelectedMicrophoneNeverClaimsRecordedAudio() {
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::StopMicrophoneRecording(app_);QElapsedTimer clock;clock.start();
            while(!NativeControlFixture::RecordingRetired(app_) && clock.elapsed()<3000) {QCoreApplication::processEvents();QTest::qWait(1);}
            NativeControlFixture::ResetMicrophoneFixture(app_);
        });
        const auto session=NativeControlFixture::RecordWithMicrophone(app_,false);QVERIFY(session);
        QVERIFY(!session->actualAudio);
        NativeControlFixture::StopMicrophoneRecording(app_);QTRY_VERIFY(NativeControlFixture::RecordingRetired(app_));
    }
    void microphoneDriverOpenAndFinalShutdownNeverBlockUi() {
        struct Gate {std::atomic<bool> opening{},openRelease{},stopping{},stopRelease{},onUi{};};
        auto gate=std::make_shared<Gate>();const auto ui=std::this_thread::get_id();
        const auto cleanup=qScopeGuard([&] {
            gate->openRelease=gate->stopRelease=true;NativeControlFixture::StopMicrophoneRecording(app_);
            QElapsedTimer clock;clock.start();
            while((!NativeControlFixture::MicrophoneRetired(app_) || !NativeControlFixture::RecordingRetired(app_)) && clock.elapsed()<4000) {
                QCoreApplication::processEvents();QTest::qWait(1);
            }
            NativeControlFixture::ResetMicrophoneFixture(app_);
        });
        const auto session=NativeControlFixture::RecordWithMicrophone(app_,true,
            [gate,ui](const std::atomic<bool>& canceled) {
                gate->onUi=std::this_thread::get_id()==ui;gate->opening=true;
                const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(!gate->openRelease && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return !canceled;
            },[gate,ui] {
                gate->onUi=gate->onUi || std::this_thread::get_id()==ui;gate->stopping=true;
                const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(!gate->stopRelease && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
        QVERIFY(session);QTRY_VERIFY(gate->opening.load());QVERIFY(!gate->onUi);
        QVERIFY(!session->actualAudio);
        bool pulse=false;QTimer::singleShot(0,&app_,[&] {pulse=true;});QTRY_VERIFY(pulse);
        gate->openRelease=true;QTRY_VERIFY(NativeControlFixture::MicrophoneStartupFinished(app_));
        NativeControlFixture::StopMicrophoneRecording(app_);QTRY_VERIFY(gate->stopping.load());
        QVERIFY(!NativeControlFixture::MicrophoneRetired(app_));QVERIFY(NativeControlFixture::MicrophoneUnsafe(app_));
        pulse=false;QTimer::singleShot(0,&app_,[&] {pulse=true;});QTRY_VERIFY(pulse);QVERIFY(!gate->onUi);
        gate->stopRelease=true;QTRY_VERIFY(NativeControlFixture::MicrophoneRetired(app_));
        QVERIFY(!NativeControlFixture::MicrophoneUnsafe(app_));
        QTRY_VERIFY(NativeControlFixture::RecordingRetired(app_));
    }
    void timedOutMicrophoneCannotPublishIntoANewerRecording() {
        auto release=std::make_shared<std::atomic<bool>>(false);
        auto entered=std::make_shared<std::atomic<bool>>(false);
        const auto cleanup=qScopeGuard([&] {
            *release=true;NativeControlFixture::StopMicrophoneRecording(app_);QElapsedTimer clock;clock.start();
            while((!NativeControlFixture::MicrophoneRetired(app_) || !NativeControlFixture::RecordingRetired(app_)) && clock.elapsed()<3000) {
                QCoreApplication::processEvents();QTest::qWait(1);
            }
            NativeControlFixture::ResetMicrophoneFixture(app_);
        });
        NativeControlFixture::ShortMicrophoneDeadline(app_);
        const auto first=NativeControlFixture::RecordWithMicrophone(app_,true,[release,entered](const std::atomic<bool>&) {
            *entered=true;const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(!*release && std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return true; // A late successful native return must not revive authority.
        });
        QVERIFY(first);QTRY_VERIFY(entered->load());QTRY_VERIFY(!NativeControlFixture::AudioOpening(app_));
        QVERIFY(!NativeControlFixture::Recording(app_)->actualAudio);QVERIFY(NativeControlFixture::MicrophoneUnsafe(app_));
        NativeControlFixture::StopMicrophoneRecording(app_);QTRY_VERIFY(NativeControlFixture::RecordingRetired(app_));
        const auto successor=NativeControlFixture::RecordWithMicrophone(app_,true);QVERIFY(successor);
        QVERIFY(successor->id!=first->id);QVERIFY(!successor->actualAudio);
        *release=true;QTRY_VERIFY(NativeControlFixture::MicrophoneRetired(app_));
        QCOMPARE(NativeControlFixture::Recording(app_)->id,successor->id);
        QVERIFY(!NativeControlFixture::Recording(app_)->actualAudio);
        NativeControlFixture::StopMicrophoneRecording(app_);QTRY_VERIFY(NativeControlFixture::RecordingRetired(app_));
    }
    void failedNativeCommitRestoresActualControlsWithoutPersisting() {
        auto& target=NativeControlFixture::Target(app_);
        const auto before=target.Fields();
        // A copy, not auto: operator[] on the temporary QJsonObject returns a
        // QJsonValueRef that would dangle after this statement.
        const QJsonValue savedBefore=NativeControlFixture::NativeState(app_)["savedZoom"];
        FailingNativeCommit fault(target);AppControlService service(fault,CatalogEdition::DevFull);
        service.SetEnabled(true,false);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"native-fault",1,{}});
        std::optional<ControlResult> result;
        service.Submit({context,"failed-commit","view_settings_set",{{"view.zoom.amount",2.0}}},
            [&](ControlResult value){result=std::move(value);});
        QTRY_VERIFY(result.has_value());
        QCOMPARE(result->content["error"].toObject()["code"].toString(),"internal");
        QVERIFY(result->content["rolledBack"].toBool());
        QVERIFY(target.Fields()==before);
        QCOMPARE(NativeControlFixture::NativeState(app_)["savedZoom"],savedBefore);
        QVERIFY(service.StopAndUndo().content["restored"].toArray().isEmpty());
    }
    void rejectedBeginIsValidationOnly() {
        auto& target=NativeControlFixture::Target(app_);
        const auto before=target.Fields();
        ControlPlan plan;
        plan.writes={{Field::TextClarity,!before.at(Field::TextClarity).toBool()},
            {Field::CustomColorRecipe,QJsonObject{{"schemeId","unregistered-recipe"},{"mode","duotone"},
                {"stopCount",2},{"stops",QJsonArray{"#000000","#ffffff"}},{"stepped",false}}}};
        QVERIFY(!target.BeginFieldPlan(plan));
        QVERIFY(target.Fields()==before);
    }
    void nativeGroupsAndMetadataStayBoundedWithoutHardware() {
        auto& service=NativeControlFixture::Service(app_);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"native-fixture",1,{}});
        QVERIFY(!context.isEmpty());
        for(const auto tool:{"image_settings_get","view_settings_get","camera_settings_get","recording_settings_get",
            "assistant_preferences_get","notes_preferences_get","ui_preferences_get","annotation_preferences_get"}) {
            const auto result=Call(context,tool,tool);QVERIFY2(result.content["ok"].toBool(),tool);
            QVERIFY(QJsonDocument(result.content).toJson(QJsonDocument::Compact).size()<=32768);
        }
        QCOMPARE(ParameterRegistry().size(),size_t(100));
        const auto state=NativeControlFixture::NativeState(app_);
        QVERIFY(!state["cameraActive"].toBool());QCOMPARE(state["cameraCount"].toInt(),0);
        QVERIFY(!state["platformInitialized"].toBool());QVERIFY(!state["recording"].toBool());
        service.End(context);
    }
    void actualNativeMutationPersistenceAndUserWins() {
        auto& service=NativeControlFixture::Service(app_);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"native-fixture",2,{}});
        const auto result=Call(context,"zoom","view_settings_set",{{"view.zoom.amount",2.0}});
        QVERIFY(result.content["ok"].toBool());
        QCOMPARE(NativeControlFixture::NativeState(app_)["zoomSlider"].toInt(),200);
        QCOMPARE(NativeControlFixture::NativeState(app_)["savedZoom"],QJsonValue(2.0));
        NativeControlFixture::NativeZoom(app_);
        const auto stale=Call(context,"stale","view_settings_set",{{"view.zoom.amount",2.5}});
        QCOMPARE(stale.content["error"].toObject()["code"].toString(),"stale_state");
        service.StopAndUndo();
        QCOMPARE(NativeControlFixture::NativeState(app_)["zoomSlider"].toInt(),175);
        service.End(context);
    }
    void uctModelSettingsShareNativePersistenceApiUndoAndUserWins() {
        auto& service=NativeControlFixture::Service(app_);
        const auto original=NativeControlFixture::UctModels(app_);
        const auto restore=qScopeGuard([&]{service.StopAndUndo();NativeControlFixture::NativeUctModels(app_,original);});
        NativeControlFixture::NativeUctModels(app_,{});
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"native-uct-models",1,{}});
        auto rows=Call(context,"uct-initial","image_settings_get").content["parameters"].toObject();
        QCOMPARE(rows["image.uct.gpu.detector"].toObject()["requested"],QJsonValue("medium"));
        QCOMPARE(rows["image.uct.cpu.recognizer"].toObject()["saved"],QJsonValue("small"));
        QVERIFY(rows["image.uct.gpu.detector"].toObject()["effective"].isNull());
        const QJsonObject changes{{"image.uct.gpu.detector","small"},{"image.uct.gpu.recognizer","tiny"},
            {"image.uct.cpu.detector","medium"},{"image.uct.cpu.recognizer","medium"}};
        QVERIFY(Call(context,"uct-select","image_settings_set",changes).content["ok"].toBool());
        rows=Call(context,"uct-saved","image_settings_get").content["parameters"].toObject();
        for(auto it=changes.begin();it!=changes.end();++it) {
            const auto row=rows[it.key()].toObject();QVERIFY(row["available"].toBool());
            QCOMPARE(row["requested"],it.value());QCOMPARE(row["saved"],it.value());QVERIFY(row["effective"].isNull());
        }
        QVERIFY(service.StopAndUndo().content["restored"].toArray().contains("image.uct.gpu.detector"));
        QVERIFY(NativeControlFixture::UctModels(app_)==uct::ModelSettings{});
        const auto second=service.Begin({ContextKind::EmbeddedTurn,"native-uct-user-wins",2,{}});
        (void)Call(second,"uct-baseline","image_settings_get");
        auto native=NativeControlFixture::UctModels(app_);native.cpuRecognizer=uct::ModelTier::Tiny;
        NativeControlFixture::NativeUctModels(app_,native);
        const auto stale=Call(second,"uct-stale","image_settings_set",{{"image.uct.cpu.recognizer","medium"}});
        QCOMPARE(stale.content["error"].toObject()["code"].toString(),"stale_state");
        QCOMPARE(NativeControlFixture::UctModels(app_).cpuRecognizer,uct::ModelTier::Tiny);
        service.End(second);
    }
    void unavailableCpuImageWritesHaveNoNativeEffects() {
        auto& service=NativeControlFixture::Service(app_);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"native-image-unavailable",3,{}});
        QVERIFY(!context.isEmpty());
        const auto cleanup=qScopeGuard([&] {service.End(context);});
        const auto rows=Call(context,"image-availability","image_settings_get").content["parameters"].toObject();
        QCOMPARE(rows.size(),55);int denied=0;
        const QSet<QString> cpuPreferences{"image.debugView","image.textClarity.enabled",
            "image.uct.enabled","image.uct.showOriginal","image.uct.gpu.detector",
            "image.uct.gpu.recognizer","image.uct.cpu.detector","image.uct.cpu.recognizer"};
        for(const auto& descriptor:ParameterRegistry()) {
            if(descriptor.group!=ParameterGroup::Image)continue;
            const auto id=QString::fromUtf8(descriptor.id.data(),descriptor.id.size());
            const auto row=rows[id].toObject();
            QCOMPARE(row["available"].toBool(),cpuPreferences.contains(id));
            if(row["available"].toBool())continue;
            const auto schema=QJsonDocument::fromJson(QByteArray::fromStdString(std::string(descriptor.schema))).object();
            const auto current=row["requested"];QJsonValue wanted;
            const auto type=schema["type"].toString();
            if(type=="boolean")wanted=!current.toBool();
            else if(!schema["enum"].toArray().isEmpty()) {for(const auto value:schema["enum"].toArray())if(value!=current){wanted=value;break;}}
            else if(type=="integer" || type=="number")wanted=current==schema["minimum"]?schema["maximum"]:schema["minimum"];
            else if(type=="array")wanted=QJsonArray{"#112233","#ddeeff"};
            else wanted=current=="normal"?QJsonValue("grayscale"):QJsonValue("normal");
            const auto before=NativeControlFixture::Target(app_).Fields();
            const auto result=Call(context,id,"image_settings_set",{{id,wanted}});
            QVERIFY(!result.content["ok"].toBool());
            QVERIFY(NativeControlFixture::Target(app_).Fields()==before);++denied;
        }
        QCOMPARE(denied,47); // Every other image parameter still refuses CPU camera writes.
    }
    void stopAndUndoRestoresOwnedSourceBeforeProviderCleanup() {
        auto& service=NativeControlFixture::Service(app_);
        auto& target=NativeControlFixture::Target(app_);
        const auto owner=QStringLiteral("native-cancel-owner");
        const auto context=service.Begin({ContextKind::EmbeddedTurn,owner,4,{}});
        QVERIFY(!context.isEmpty());
        const auto zoomBefore=target.Fields().at(Field::ZoomAmount);
        QVERIFY(Call(context,"cancel-zoom","view_settings_set",{{"view.zoom.amount",2.5}}).content["ok"].toBool());
        QVERIFY(NativeControlFixture::RequestTemporarySource(app_,owner));
        QVERIFY(target.HasOwnedSource(owner));
        QJsonObject undo;int userChanges=0;
        const auto resetSink=qScopeGuard([&service] {service.SetEventSink({});});
        service.SetEventSink([&](const QJsonObject& event) {
            if(event["type"]=="undo")undo=event;
            if(event["type"]=="user_changed")++userChanges;
        });
        // The production Stop and undo path with no provider, lease or device.
        NativeControlFixture::CancelAgentTurn(app_,context,true);
        QVERIFY(NativeControlFixture::AgentContext(app_).isEmpty());
        QVERIFY(!target.HasOwnedSource(owner));QVERIFY(!service.IsContextActive(context));
        QTRY_VERIFY(!undo.isEmpty());
        QCOMPARE(undo["sourceRestoreFields"].toArray(),(QJsonArray{"camera.selection","camera.format"}));
        QVERIFY(!undo["skipped"].toArray().contains("camera.selection"));
        QVERIFY(undo["restored"].toArray().contains("zoomAmount"));
        QCOMPARE(target.Fields().at(Field::ZoomAmount),zoomBefore);
        // The owned restore is not attributed to the user by the next native poll.
        const auto revision=service.FieldRevision(Field::Camera);
        target.NoticeNativeChanges();QCoreApplication::processEvents();
        QCOMPARE(service.FieldRevision(Field::Camera),revision);QCOMPARE(userChanges,0);
    }
    void assistantSendPreparesToolsAndContinuesWithoutRoutingButtons() {
        qunsetenv("OKUFLOW_FAKE_AGENT_SCENARIO");
        NativeControlFixture::PrepareAssistant(app_,"automatic-control");
        auto& runtime=NativeControlFixture::Runtime(app_);
        auto& client=NativeControlFixture::AgentClient(app_);
        QSignalSpy availability(&client,&CodexAgentClient::AvailabilityChanged);
        bool frames=false,photos=true,recording=true;
        const auto capture=connect(&client,&CodexAgentClient::TurnStarted,this,[&] {
            const auto grants=NativeControlFixture::Service(app_).ContextStatus()["grants"].toObject();
            frames=grants["frames"].toBool();photos=grants["photos"].toBool();recording=grants["recording"].toBool();
        });
        const auto cleanup=qScopeGuard([&] {disconnect(capture);});
        NativeControlFixture::SubmitAssistant(app_);
        QVERIFY(NativeControlFixture::AssistantPending(app_));QVERIFY(runtime.ChatSession().Busy());
        const auto pendingEvents=availability.size();
        NativeControlFixture::TestAssistantTools(app_);
        QCOMPARE(availability.size(),pendingEvents); // diagnostic click shares the existing preparation
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.ChatSession().Busy(),10000);
        QVERIFY(!NativeControlFixture::AssistantPending(app_));QVERIFY(client.IsReady());
        QVERIFY(frames);QVERIFY(!photos);QVERIFY(!recording);
        QVERIFY(!NativeControlFixture::Window(app_).agentControlPanel()->routingNoticeShown());
        QVERIFY(NativeControlFixture::Service(app_).PendingApproval().isEmpty());
        QCOMPARE(NativeControlFixture::Target(app_).Fields().at(Field::ZoomAmount).toDouble(),2.0);
        const auto first=runtime.ChatSession().Snapshot()["messages"].toArray();
        QCOMPARE(first.size(),2);QVERIFY(first.last().toObject()["complete"].toBool());
        const auto readinessEvents=availability.size();
        NativeControlFixture::SubmitAssistant(app_);
        QVERIFY(!NativeControlFixture::AssistantPending(app_));
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.ChatSession().Busy(),10000);
        QCOMPARE(availability.size(),readinessEvents);
        QCOMPARE(runtime.ChatSession().Snapshot()["messages"].toArray().size(),4);
    }
    void assistantPreparationCanBeStoppedOrReconfigured_data() {
        QTest::addColumn<bool>("reconfigure");QTest::newRow("Stop")<<false;QTest::newRow("new-settings")<<true;
    }
    void assistantPreparationCanBeStoppedOrReconfigured() {
        QFETCH(bool,reconfigure);qunsetenv("OKUFLOW_FAKE_AGENT_SCENARIO");
        NativeControlFixture::PrepareAssistant(app_,reconfigure?"before-change":"before-stop");
        const auto zoom=NativeControlFixture::Target(app_).Fields().at(Field::ZoomAmount);
        NativeControlFixture::SubmitAssistant(app_);QVERIFY(NativeControlFixture::AssistantPending(app_));
        if(reconfigure)NativeControlFixture::ReconfigureAssistant(app_,"after-change");
        else NativeControlFixture::StopAssistant(app_);
        QVERIFY(!NativeControlFixture::AssistantPending(app_));
        QVERIFY(!NativeControlFixture::Runtime(app_).ChatSession().Busy());
        QTest::qWait(100);
        QCOMPARE(NativeControlFixture::Target(app_).Fields().at(Field::ZoomAmount),zoom);
        QVERIFY(NativeControlFixture::AgentContext(app_).isEmpty());
    }
    void assistantSendSeparatesRestoredProviderHistory_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("assistant")<<int(ChatBackendKind::Assistant);
        QTest::newRow("coding")<<int(ChatBackendKind::Coding);
        QTest::newRow("old-agent")<<int(ChatBackendKind::Agent);
    }
    void assistantSendSeparatesRestoredProviderHistory() {
        QFETCH(int,kind);qunsetenv("OKUFLOW_FAKE_AGENT_SCENARIO");
        NativeControlFixture::PrepareAssistant(app_,"history-"+QString::number(kind));
        NativeControlFixture::RestoreAssistantHistory(app_,static_cast<ChatBackendKind>(kind));
        NativeControlFixture::SubmitAssistant(app_);
        auto& session=NativeControlFixture::Runtime(app_).ChatSession();
        QCOMPARE(session.Snapshot()["kind"].toString(),"explain");
        QVERIFY(session.Snapshot()["conversationId"].toString()!="restored-provider-thread");
        QVERIFY(NativeControlFixture::AssistantProviderThread(app_).isEmpty());
        QVERIFY(session.Draft().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!session.Busy(),10000);
        const auto messages=session.Snapshot()["messages"].toArray();
        QCOMPARE(messages.size(),2);
        QCOMPARE(messages.first().toObject()["text"].toString(),"Set zoom to 2x");
        QVERIFY(messages.last().toObject()["complete"].toBool());
    }
    void assistantFailedPreparationNeverDispatchesControl() {
        qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","probe_start_rejected");
        const auto cleanup=qScopeGuard([] {qunsetenv("OKUFLOW_FAKE_AGENT_SCENARIO");});
        NativeControlFixture::PrepareAssistant(app_,"rejected-automatic-control");
        const auto zoom=NativeControlFixture::Target(app_).Fields().at(Field::ZoomAmount);
        NativeControlFixture::SubmitAssistant(app_);
        QTRY_VERIFY_WITH_TIMEOUT(!NativeControlFixture::Runtime(app_).ChatSession().Busy(),10000);
        QVERIFY(!NativeControlFixture::AssistantPending(app_));QVERIFY(NativeControlFixture::AgentContext(app_).isEmpty());
        QCOMPARE(NativeControlFixture::Target(app_).Fields().at(Field::ZoomAmount),zoom);
        QVERIFY(!NativeControlFixture::Runtime(app_).ChatSession().Snapshot()["messages"].toArray().last().toObject()["complete"].toBool());
    }
    void assistantHttpPreparationContinuesOrFailsWithoutDispatch_data() {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("ready")<<QString("ready");
        QTest::newRow("no-tool")<<QString("no-tool");
        QTest::newRow("invalid-configuration")<<QString("invalid");
    }
    void assistantHttpPreparationContinuesOrFailsWithoutDispatch() {
        QFETCH(QString,scenario);
        QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost,0));
        int requests=0,probes=0,submissions=0;
        connect(&server,&QTcpServer::newConnection,&server,[&] {
            while(server.hasPendingConnections()) {
                auto* socket=server.nextPendingConnection();auto bytes=std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,socket,[&,socket,bytes] {
                    bytes->append(socket->readAll());
                    const auto boundary=bytes->indexOf("\r\n\r\n");
                    if(boundary<0 || socket->property("received").toBool())return;
                    int length=0;
                    for(const auto line:bytes->left(boundary).split('\n'))
                        if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                    if(bytes->size()<boundary+4+length)return;
                    socket->setProperty("received",true);++requests;
                    const auto body=QJsonDocument::fromJson(bytes->mid(boundary+4,length)).object();
                    const auto messages=body["messages"].toArray();
                    bool probe=false;
                    for(const auto tool:body["tools"].toArray())
                        probe|=tool.toObject()["function"].toObject()["name"]=="okuflow_capability_test";
                    QJsonObject message{{"role","assistant"},{"content","Done"}};
                    QString reason="stop";
                    if(probe || (!messages.isEmpty() && messages.last().toObject()["role"]=="user")) {
                        if(probe)++probes;else ++submissions;
                        if(!probe || scenario=="ready") {
                            const auto name=probe?"okuflow_capability_test":"view_zoom_set";
                            const QJsonObject args=probe?QJsonObject{{"value",1}}:QJsonObject{{"magnification",2}};
                            const QJsonObject call{{"id",QString("call-%1").arg(requests)},{"type","function"},
                                {"function",QJsonObject{{"name",name},{"arguments",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
                            message["content"]=QJsonValue::Null;message["tool_calls"]=QJsonArray{call};reason="tool_calls";
                        }
                    }
                    const auto answer=QJsonDocument(QJsonObject{{"choices",QJsonArray{QJsonObject{{"message",message},{"finish_reason",reason}}}}}).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+QByteArray::number(answer.size())+"\r\n\r\n"+answer);
                    socket->disconnectFromHost();
                });
            }
        });
        NativeControlFixture::PrepareHttpAssistant(app_,scenario=="invalid"?QString("http://"):
            QString("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort()));
        const auto zoom=NativeControlFixture::Target(app_).Fields().at(Field::ZoomAmount);
        NativeControlFixture::SubmitAssistant(app_);
        auto& session=NativeControlFixture::Runtime(app_).ChatSession();
        QTRY_VERIFY_WITH_TIMEOUT(!session.Busy(),10000);
        QVERIFY(!NativeControlFixture::AssistantPending(app_));
        QCOMPARE(submissions,scenario=="ready"?1:0);QCOMPARE(probes,scenario=="invalid"?0:1);
        QCOMPARE(session.Snapshot()["messages"].toArray().size(),2);
        const auto completed=session.Snapshot()["messages"].toArray().last().toObject();
        QVERIFY2(completed["complete"].toBool()==(scenario=="ready"),qPrintable(completed["errorCode"].toString()));
        QCOMPARE(NativeControlFixture::Target(app_).Fields().at(Field::ZoomAmount),scenario=="ready"?QJsonValue(2.0):zoom);
        QVERIFY(NativeControlFixture::Service(app_).PendingApproval().isEmpty());
    }
    void assistantUctRequestUsesDiscoveredSettingsAndNativeController() {
        qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","scope_uct");
        const auto original=NativeControlFixture::CurrentImageConfig(app_);
        NativeControlFixture::UseFixtureUctRecognizer(app_);
        const auto cleanup=qScopeGuard([&] {
            NativeControlFixture::RestoreUctRecognizer(app_);
            NativeControlFixture::RestoreImageConfig(app_,original);
            qunsetenv("OKUFLOW_FAKE_AGENT_SCENARIO");
        });
        auto disabled=original;disabled.autoTextClarityEnabled=false;
        NativeControlFixture::RestoreImageConfig(app_,disabled);
        NativeControlFixture::PrepareAssistant(app_,"uct-product-name");
        NativeControlFixture::Runtime(app_).ChatSession().SetDraft("Turn on Ultra Clear Text");
        NativeControlFixture::SubmitAssistant(app_,"Turn on Ultra Clear Text");
        auto& session=NativeControlFixture::Runtime(app_).ChatSession();
        QTRY_VERIFY_WITH_TIMEOUT(!session.Busy(),10000);
        const auto answer=session.Snapshot()["messages"].toArray().last().toObject();
        QVERIFY2(answer["complete"].toBool(),qPrintable(answer["errorCode"].toString()));
        const auto replies=QJsonDocument::fromJson(answer["text"].toString().toUtf8()).object()["replies"].toArray();
        QCOMPARE(replies.size(),3);
        for(const auto reply:replies) {
            const auto data=reply.toObject()["data"].toObject();
            QVERIFY2(data["ok"].toBool(),qPrintable(data["error"].toObject()["code"].toString()));
        }
        QTRY_VERIFY(NativeControlFixture::UctReady(app_));
        QVERIFY(NativeControlFixture::UctRequested(app_));
        auto& service=NativeControlFixture::Service(app_);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"verify-uct",1,{}});
        QVERIFY(!context.isEmpty());
        const auto rows=Call(context,"uct-ready-read","image_settings_get",{{"fields",QJsonArray{"image.uct.enabled","image.textClarity.enabled"}}}).content["parameters"].toObject();
        service.End(context);
        const auto uct=rows["image.uct.enabled"].toObject();
        QVERIFY(uct["requested"].toBool());QVERIFY(uct["effective"].toBool());QVERIFY(uct["saved"].toBool());
        QVERIFY(rows["image.textClarity.enabled"].toObject()["requested"].toBool());
        QVERIFY(!NativeControlFixture::Window(app_).agentControlPanel()->routingNoticeShown());
    }
    void uctMasterAndOriginalUseNativeControllerWithoutClaimingReadiness() {
        auto& service=NativeControlFixture::Service(app_);
        const auto* panel=NativeControlFixture::Window(app_).ultraClearTextPanel();
        QCOMPARE(QString::fromUtf8(ReceiptLabelSource("image.uct.enabled").data()),panel->enableCheckbox()->text());
        QCOMPARE(QString::fromUtf8(ReceiptLabelSource("image.uct.showOriginal").data()),panel->showOriginalCheckbox()->text());
        service.SetEnabled(true,false);
        auto config=NativeControlFixture::CurrentImageConfig(app_);config.autoTextClarityEnabled=false;
        NativeControlFixture::RestoreImageConfig(app_,config);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"uct-controls",99,{}});
        QVERIFY(!context.isEmpty());
        const auto enabled=Call(context,"uct-on","image_settings_set",{{"image.uct.enabled",true}});
        QVERIFY(enabled.content["ok"].toBool());QVERIFY(NativeControlFixture::UctRequested(app_));
        QVERIFY(NativeControlFixture::Window(app_).ultraClearTextPanel()->IsEnabledByUser());
        QVERIFY(Call(context,"uct-original","image_settings_set",{{"image.uct.showOriginal",true}}).content["ok"].toBool());
        QVERIFY(NativeControlFixture::Window(app_).ultraClearTextPanel()->IsShowingOriginal());
        const auto rows=Call(context,"uct-read","image_settings_get",{{"fields",QJsonArray{"image.uct.enabled","image.uct.showOriginal"}}}).content["parameters"].toObject();
        QVERIFY(rows["image.uct.enabled"].toObject()["requested"].toBool());
        QVERIFY(!rows["image.uct.enabled"].toObject()["effective"].toBool());
        QVERIFY(rows["image.uct.enabled"].toObject()["saved"].toBool());
        QCOMPARE(rows["image.uct.showOriginal"].toObject()["persistence"].toString(),"session_only");
        QVERIFY(Call(context,"uct-off","image_settings_set",{{"image.uct.enabled",false}}).content["ok"].toBool());
        QVERIFY(!NativeControlFixture::UctRequested(app_));service.End(context);
    }
};
}
int main(int argc,char** argv) {
    qputenv("QT_QPA_PLATFORM","offscreen");
    qunsetenv("OKUFLOW_VLM_API_KEY");
    QTemporaryDir root;
    if(!root.isValid())return 3;
    OkuFlowApp app(argc,argv);
    QCoreApplication::setApplicationName("OkuFlowNativeControlFixture-"+QUuid::createUuid().toString(QUuid::Id128));
    NativeControlFixture::Initialize(app,root.path());
    bool stdio=false;for(int i=1;i<argc;++i)stdio|=QString::fromLocal8Bit(argv[i])=="--stdio-fixture";
    if(!stdio) {NativeAcceptance tests(app);return QTest::qExec(&tests,argc,argv);}
    mcp::AppIpcEndpoint endpoint(NativeControlFixture::Service(app));
    if(!endpoint.SetEnabled(true))return 4;
    const auto edition=agent::CatalogEditionName(agent::BuiltControlCatalogEdition());
    Output({{"type","ready"},{"selector",endpoint.InstanceSelector()},
        {"catalogEdition",QString::fromUtf8(edition.data(),edition.size())},{"realTarget",true}});
    endpoint.SetPendingChanged([&] {const auto id=endpoint.PendingPeerId();if(!id.isEmpty())Output({{"type","pending"},{"peerId",id}});});
    std::thread input([&] {
        std::string line;
        while(std::getline(std::cin,line))QMetaObject::invokeMethod(&app,[&,line] {
            if(line.starts_with("approve:"))Output({{"type","approved"},{"ok",endpoint.ApprovePending(QString::fromStdString(line.substr(8)),{})}});
            else if(line=="native-state") {auto state=NativeControlFixture::NativeState(app);state["type"]="native-state";Output(state);}
            else if(line=="native-contrast") {NativeControlFixture::NativeContrast(app,175);Output({{"type","native-changed"}});}
            else if(line=="native-zoom") {NativeControlFixture::NativeZoom(app);Output({{"type","native-changed"}});}
            else if(line=="undo")Output({{"type","undo"},{"result",NativeControlFixture::Service(app).StopAndUndo().content}});
            else if(line.starts_with("resolve:"))Output({{"type","resolved"},{"result",NativeControlFixture::Service(app).ResolveApproval(QString::fromStdString(line.substr(8)),true).content}});
            else if(line=="revoke") {endpoint.Revoke();Output({{"type","revoked"}});}
            else if(line=="exit")QCoreApplication::quit();
        },Qt::QueuedConnection);
        QMetaObject::invokeMethod(&app,[] {QCoreApplication::quit();},Qt::QueuedConnection);
    });
    const int result=NativeControlFixture::Exec(app);
    if(input.joinable())input.join();
    return result;
}
#include "native_app_control_fixture.moc"
