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
#include "okuflow/mcp/app_ipc_endpoint.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSlider>
#include <QTemporaryDir>
#include <QUuid>
#include <QElapsedTimer>
#include <QtTest>
#include <cstdio>
#include <iostream>
#include <thread>

namespace okuflow::agent {
// This harness intentionally bypasses platform/device/provider bootstrap.
// All controls and transitions below are the real production implementations.
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
            [](const QString&,int) {},[](const SavedRecordingSegment&) {},*app.userDataPaths_);
        app.uiState_->microphoneCombo_->addItem("None","__none__");
        app.cameraIntent_.LoadSaved({});
        app.PopulatePresetList();
        app.uiState_->ApplyConfigToUI(settings.currentConfig,true);
        app.controlTarget_=std::make_unique<AppControlTarget>(app);
        app.controlService_=std::make_unique<AppControlService>(*app.controlTarget_,CatalogEdition::DevFull);
        app.controlService_->SetEnabled(true,true);
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
    static void ClearAnnotation(OkuFlowApp& app) {app.CancelAnnotationCaptures();}
    static AppControlService& Service(OkuFlowApp& app) {return *app.controlService_;}
    static AppControlTarget& Target(OkuFlowApp& app) {return *app.controlTarget_;}
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
};
}
using namespace okuflow;
using namespace okuflow::agent;
namespace {
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
        QCOMPARE(ParameterRegistry().size(),size_t(94));
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
    void unavailableCpuImageWritesHaveNoNativeEffects() {
        auto& service=NativeControlFixture::Service(app_);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"native-image-unavailable",3,{}});
        const auto rows=Call(context,"image-availability","image_settings_get").content["parameters"].toObject();
        QCOMPARE(rows.size(),49);int denied=0;
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
