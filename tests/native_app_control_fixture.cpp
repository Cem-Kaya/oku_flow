#include "../src/app/app_internal.hpp"
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
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
#include "okuflow/ui/video_transport.hpp"
#include "okuflow/common/uct_device_image.hpp"
#include "okuflow/common/video_playback.hpp"
#include "okuflow/mcp/app_ipc_endpoint.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLayout>
#include <QGridLayout>
#include <QMap>
#include <QPushButton>
#include <QScopeGuard>
#include <QSlider>
#include <QTemporaryDir>
#include <QUuid>
#include <QElapsedTimer>
#include <QtTest>
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
        app.controlService_=std::make_unique<AppControlService>(*app.controlTarget_,CatalogEdition::DevFull);
        app.controlService_->SetEnabled(true,true);
#if defined(OKUFLOW_VIDEO_DOCUMENT_TESTS)
        app.InitializeVideoPlayback();
#endif
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
    static void VideoSaved(OkuFlowApp& app,const SavedRecordingSegment& part) {app.OnVideoSegmentSaved(part);}
    static bool VideoTimerActive(const OkuFlowApp& app) {return app.videoPlaybackTimer_ && app.videoPlaybackTimer_->isActive();}
    static VideoTransport::Snapshot VideoUi(const OkuFlowApp& app) {return app.mainWindow_->videoTransport()->snapshot();}
    static MainWindow& Window(OkuFlowApp& app) {return *app.mainWindow_;}
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
                                                  bool unsupportedApplied=false,bool pendingAppearance=false) {
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
        CapturedFrame first;first.width=32;first.height=16;first.pixels.assign(32*16*4,31);
        first.identity.sequenceNumber=71;first.identity.captureClock100ns=710'000;
        const auto view=ComputeViewTransform(32,16,viewportWidth,viewportHeight,2.0f,0.25f,0.75f,ViewportFitMode::kFill);
        if(!app.presentedOriginalCache_.Stage(first,session.id,app.cameraSessionId_,0,applied))return 0;
        app.presentedOriginalCache_.Presented(app.cameraSessionId_,71,view);
        auto next=first;next.pixels.assign(next.pixels.size(),191);
        next.identity.sequenceNumber=72;next.identity.captureClock100ns=720'000;
        auto nextRecipe=applied;nextRecipe.canReproduceCleanView=true;
        if(pendingAppearance) {app.OnContrastChanged(140);nextRecipe.contrast=app.contrast_;}
        if(!app.presentedOriginalCache_.Stage(next,session.id,app.cameraSessionId_,0,nextRecipe))return 0;
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
        for(std::int64_t now=1000;now<3000;now+=16)
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
        // Two seconds of 60-Hz view replays whose capture throttle yields no
        // fresh lease: the valid same-scene lease survives every replay and
        // its untouched retry budget never demands another whole-frame pass.
        for(std::int64_t now=1000;now<3000;now+=16) {
            app.SelectedMediaGpuSceneProcessed(true,{},now);
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
    void cancelledVideoPickerRestoresOnlyPhoneLight() {
        NativeControlFixture::ResetVideo(app_);
        QVERIFY(NativeControlFixture::CancelVideoPickerRestoresPhoneLight(app_,true));
        QVERIFY(NativeControlFixture::CancelVideoPickerRestoresPhoneLight(app_,false));
    }
    void translatedActionRowsHaveUniformCellMinimums() {
        auto* application=qobject_cast<QApplication*>(QCoreApplication::instance());QVERIFY(application);
        LanguageManager translations(*application);QVERIFY(translations.SetLanguage(AppLanguage::German,false));
        const auto cleanup=qScopeGuard([&]{translations.SetLanguage(AppLanguage::English,false);});
        MainWindow window;window.resize(1100,760);window.show();window.advancedModeButton()->click();window.setAdvancedPanelWidth(440);
        QCoreApplication::processEvents();
        auto* panel=window.findChild<QWidget*>("bottomRightPanel");QVERIFY(panel);
        auto* grid=qobject_cast<QGridLayout*>(panel->layout());QVERIFY(grid);
        const int minimum=grid->columnMinimumWidth(0);QVERIFY(minimum>0);
        for(int i=0;i<grid->columnCount();++i)if(grid->columnStretch(i)>0)QCOMPARE(grid->columnMinimumWidth(i),minimum);
        QMap<int,QVector<int>> widths;
        for(auto* button:{window.capturePhotoButton(),window.recordButton(),window.explainNowButton(),window.readTextButton(),window.annotationButton()})widths[button->y()].push_back(button->width());
        for(const auto& row:widths){const auto range=std::minmax_element(row.begin(),row.end());QVERIFY(*range.second-*range.first<=4);}
    }
    void selectedMediaGpuPolicyIncludesAllImageEffects() {
        // Exercise the policy seam without creating a CUDA surface or claiming
        // that this offscreen fixture has executed any shader.
        for(const auto field:{Field::Blur,Field::BlurSigma,Field::BlurRadius,
            Field::Spatial,Field::Upscaler,Field::Sharpness,Field::TextClarity,
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
            Field::Scheme,Field::CustomColorRecipe,Field::UctCpuDetector,Field::UctGpuDetector})
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
        for(const auto* id:{"image.blur.enabled","image.spatial.enabled","image.textClarity.enabled",
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
    void pauseRejectsUnreproducibleDisplayedAppearance_data() {
        QTest::addColumn<bool>("unsupportedApplied");QTest::addColumn<bool>("pendingAppearance");
        QTest::newRow("unsupported-actual-neutral-request")<<true<<false;
        QTest::newRow("neutral-actual-newer-pending-contrast")<<false<<true;
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
        QCOMPARE(ParameterRegistry().size(),size_t(98));
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
        const auto rows=Call(context,"image-availability","image_settings_get").content["parameters"].toObject();
        QCOMPARE(rows.size(),53);int denied=0;
        for(const auto& descriptor:ParameterRegistry()) {
            if(descriptor.group!=ParameterGroup::Image)continue;
            const auto id=QString::fromUtf8(descriptor.id.data(),descriptor.id.size());
            const auto row=rows[id].toObject();
            if(row["available"].toBool())continue; // Debug-view preference is backend-independent.
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
        QVERIFY(denied>=48);service.End(context);
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
    if(!stdio) {NativeAcceptance tests(app);int testArgc=1;return QTest::qExec(&tests,testArgc,argv);}
    mcp::AppIpcEndpoint endpoint(NativeControlFixture::Service(app));
    if(!endpoint.SetEnabled(true))return 4;
    Output({{"type","ready"},{"selector",endpoint.InstanceSelector()},{"catalogEdition","dev-full"},{"realTarget",true}});
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
