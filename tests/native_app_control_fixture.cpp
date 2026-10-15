#include "okuflow/app/app.hpp"
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
        storage.saveSettings=settings::Save;
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
    static AppControlService& Service(OkuFlowApp& app) {return *app.controlService_;}
    static AppControlTarget& Target(OkuFlowApp& app) {return *app.controlTarget_;}
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
