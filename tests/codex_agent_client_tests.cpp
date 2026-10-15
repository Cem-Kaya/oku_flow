#include "okuflow/common/codex_agent_client.hpp"
#include <QtTest>
#include <QJsonDocument>
#include "okuflow/app/agent/operation_tracker.hpp"
#include "okuflow/app/agent/camera_source_intent.hpp"
using namespace okuflow;
using namespace okuflow::agent;
// Synthetic target only: real service/adapter/tracker ownership is exercised,
// but no production Qt transitions, media writer, GPU or camera is invoked.
class AgentTarget final : public IAppControlTarget {
public:
    double zoom=1;int effects{},saves{},observations{},photos{},starts{},stops{},metrics{};
    bool recording{},deferObservation{},deferPhoto{};
    QString session,owner,lastOperation;
    OperationTracker operations;
    CameraSourceIntent camera;
    ControlCompletion delayed;
    std::map<Field,QJsonValue> fields{{Field::ZoomAmount,1.0},{Field::ZoomEnabled,true},
        {Field::CenterX,0.5},{Field::CenterY,0.5},{Field::Rotation,0},{Field::Contrast,1.0},
        {Field::Brightness,0.0},{Field::Scheme,"normal"},{Field::TextClarity,false},
        {Field::Adaptive,false},{Field::TwoColor,false},{Field::BlackWhite,false},{Field::Polarity,0},
        {Field::AutoContrast,false},{Field::Keystone,false},{Field::Preset,"custom"},
        {Field::Camera,"cam-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},{Field::Recording,false}};
    AgentTarget() {
        camera.LoadSaved({"fixture-saved-webcam","fixture-format",0,true});
        auto generation=camera.Request({"fixture-saved-webcam","fixture-format",false},CameraSourceOrigin::Startup);
        camera.Activate(generation,{"fixture-saved-webcam","fixture-format",true});
    }
    QJsonObject State() const override {return {{"zoom",zoom},{"recording",recording},{"cameraReady",camera.Active().has_value()}};}
    std::map<Field,QJsonValue> Fields() const override {auto result=fields;result[Field::ZoomAmount]=zoom;return result;}
    ControlPlan Prepare(ToolId t,const QJsonObject& a,const QString& contextOwner) const override {
        ControlPlan p;auto write=[&](Field f,QJsonValue v){p.writes.push_back({f,v});};
        if(t==ToolId::Zoom)write(Field::ZoomAmount,a["magnification"]);
        else if(t==ToolId::Center) {write(Field::CenterX,a["point"].toObject()["x"]);write(Field::CenterY,a["point"].toObject()["y"]);}
        else if(t==ToolId::ResetView) {write(Field::ZoomAmount,1);write(Field::CenterX,0.5);write(Field::CenterY,0.5);}
        else if(t==ToolId::ModeActivate) {
            if(a["presetId"]!="preset-reading")p.error="not_found";
            else {write(Field::Preset,"preset-reading");write(Field::TextClarity,true);}
        } else if(t==ToolId::ReadingColors) {
            const auto intent=a["intent"].toString();
            const bool text=intent.startsWith("high_contrast_");
            if(intent!="scheme") {
                write(Field::TextClarity,text);write(Field::Adaptive,text);write(Field::TwoColor,text);
                write(Field::BlackWhite,false);write(Field::Polarity,0);
            }
            const QString scheme=intent=="grayscale"?QString("grayscale"):intent=="photographic_color"?QString("normal"):
                intent=="high_contrast_dark_on_light"?QString("black-white"):intent=="high_contrast_light_on_dark"?QString("white-black"):a["schemeId"].toString();
            write(Field::Scheme,scheme);
        } else if(t==ToolId::Contrast) {write(Field::Contrast,a["contrast"]);write(Field::Brightness,a["brightness"]);}
        else if(t==ToolId::AutoAdjust) {write(Field::AutoContrast,true);write(Field::TextClarity,true);}
        else if(t==ToolId::Orientation) {write(Field::Rotation,a["degreesClockwise"]);p.targetIdentity="fixture-live-source";}
        else if(t==ToolId::RecordingStop) {
            p.targetIdentity=a["sessionId"].toString();p.ownedRecording=recording && owner==contextOwner;
            if(p.targetIdentity!=session)p.error="stale_state";
        } else if(t==ToolId::CameraSelect) {
            p.targetIdentity=a["cameraHandle"].toString();write(Field::Camera,p.targetIdentity);
        }
        return p;
    }
    bool Apply(const FieldWrite& w) override {if(w.field==Field::ZoomAmount)zoom=w.value.toDouble();fields[w.field]=w.value;++effects;return true;}
    bool Save() override{++saves;return true;}
    QJsonObject Query(ToolId t,const QJsonObject& a) const override {
        if(t==ToolId::Operation)return operations.SafeStatus(a["operationId"].toString());
        if(t==ToolId::Modes)return {{"modes",QJsonArray{QJsonObject{{"presetId","preset-reading"},{"displayName","Read a Page"},{"agentApplicable",true}}}}};
        if(t==ToolId::CameraSources)return {{"sources",QJsonArray{QJsonObject{
            {"cameraHandle","cam-0123456789abcdef0123456789abcdef"},{"displayName","Camera 2"}}}}};
        return {};
    }
    ControlResult Pending(const QString& operation) const {
        auto data=operations.SafeStatus(operation);data["pending"]=true;return {data,{}};
    }
    void CompleteOperation(const QString& operation) {
        const auto current=operations.Get(operation);if(!current)return;
        if(current->kind==OperationKind::Photo)operations.Advance(operation,OperationState::Committed,
            {{"originalName","IMG_fixture_original.jpg"},{"processedName","IMG_fixture_processed.jpg"}});
        else if(current->kind==OperationKind::Recording && current->state==OperationState::Starting)
            operations.Advance(operation,OperationState::Recording);
        else if(current->kind==OperationKind::Recording && current->state==OperationState::Stopping)
            operations.Advance(operation,OperationState::Completed);
        else if(current->kind==OperationKind::Camera) {
            operations.Advance(operation,OperationState::Starting);
            camera.Activate(camera.Requested().generation,{"fixture-temporary-camera","",false});
            camera.MarkFirstFrame(camera.Requested().generation);
            operations.Advance(operation,OperationState::FirstFrame);
        }
    }
    void Execute(ToolId t,const QJsonObject& a,const QString& contextOwner,ContextKind,ControlCompletion c) override {
        if(t==ToolId::Metrics) {++metrics;c({{{"ok",true},{"lumaMean",0.4},{"stale",false}}, {}});return;}
        if(t==ToolId::Observe) {
            ++observations;if(deferObservation){delayed=std::move(c);return;}
            c({{{"ok",true},{"observationId","fixture-observation"},{"width",8},{"height",8}},QByteArray("synthetic-jpeg-marker")});return;
        }
        if(t==ToolId::Wait) {CompleteOperation(a["operationId"].toString());c({{{"ok",true},{"met",true}}, {}});return;}
        if(t==ToolId::Photo) {
            ++photos;lastOperation=operations.Create(OperationKind::Photo,contextOwner);
            operations.Advance(lastOperation,OperationState::Readback);operations.Advance(lastOperation,OperationState::Writing);
            if(deferPhoto){delayed=std::move(c);return;}c(Pending(lastOperation));return;
        }
        if(t==ToolId::RecordingStart) {
            if(recording){c(ControlResult::Failure("busy"));return;}
            ++starts;recording=true;session="fixture-owned";owner=contextOwner;
            lastOperation=operations.Create(OperationKind::Recording,owner,session);c(Pending(lastOperation));return;
        }
        if(t==ToolId::RecordingStop) {
            if(!recording || a["sessionId"]!=session){c(ControlResult::Failure("stale_state"));return;}
            ++stops;recording=false;
            if(lastOperation.isEmpty())lastOperation=operations.Create(OperationKind::Recording,contextOwner,session);
            operations.Advance(lastOperation,OperationState::Stopping);c(Pending(lastOperation));return;
        }
        if(t==ToolId::CameraSelect) {
            lastOperation=operations.Create(OperationKind::Camera,contextOwner);
            camera.RequestTemporary(contextOwner,{"fixture-temporary-camera","",false},lastOperation);
            c(Pending(lastOperation));return;
        }
        if(t==ToolId::AutoAdjust) {
            lastOperation=operations.Create(OperationKind::Adjustment,contextOwner);
            operations.Advance(lastOperation,OperationState::Starting);c(Pending(lastOperation));return;
        }
        c(ControlResult::Failure("not_supported"));
    }
    bool ModalActive() const override{return false;}
    bool UserInteracting() const override{return false;}
    bool RecordingActive() const override{return recording;}
    QString RecordingSession() const override{return session;}
    QString RecordingOwner() const override{return owner;}
    void StopOwnedRecording(const QString& contextOwner) override{if(recording && owner==contextOwner){recording=false;++stops;}}
};
class CodexAgentClientTests final : public QObject {
    Q_OBJECT
    bool Ready(CodexAgentClient& client,AppControlService& service) {
        client.SetControlService(&service);
        client.Configure(QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY),{},"low",{});
        client.TestCapability();
        return QTest::qWaitFor([&]{return client.IsReady() && !client.IsTurnActive();},10000);
    }
    bool RunScope(CodexAgentClient& client,AppControlService& service,ControlGrants grants,QJsonArray& replies) {
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"trusted-scope",1,grants});
        if(context.isEmpty())return false;
        QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        if(!client.SubmitTurn(context,"exercise finite declared tools") ||
            !QTest::qWaitFor([&]{return done.size()==1;},10000))return false;
        if(!done[0][3].toString().isEmpty())return false;
        replies=QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object()["replies"].toArray();
        return !replies.isEmpty();
    }
    static QString Error(QJsonObject data){return data["error"].toObject()["code"].toString();}
    static QJsonObject Data(const QJsonArray& replies,int row=0){return replies[row].toObject()["data"].toObject();}
private slots:
    void init(){qunsetenv("OKUFLOW_FAKE_AGENT_SCENARIO");}
    void declaredViewScope_data() {
        QTest::addColumn<QByteArray>("scenario");QTest::addColumn<int>("field");QTest::addColumn<QJsonValue>("expected");
        QTest::newRow("center")<<QByteArray("scope_center")<<int(Field::CenterX)<<QJsonValue(0.25);
        QTest::newRow("reset")<<QByteArray("scope_reset")<<int(Field::ZoomAmount)<<QJsonValue(1.0);
        QTest::newRow("mode-list-activate")<<QByteArray("scope_mode")<<int(Field::Preset)<<QJsonValue("preset-reading");
        QTest::newRow("photographic-color")<<QByteArray("scope_colors_photographic_color")<<int(Field::Scheme)<<QJsonValue("normal");
        QTest::newRow("grayscale")<<QByteArray("scope_colors_grayscale")<<int(Field::Scheme)<<QJsonValue("grayscale");
        QTest::newRow("dark-on-light")<<QByteArray("scope_colors_high_contrast_dark_on_light")<<int(Field::Scheme)<<QJsonValue("black-white");
        QTest::newRow("light-on-dark")<<QByteArray("scope_colors_high_contrast_light_on_dark")<<int(Field::Scheme)<<QJsonValue("white-black");
        QTest::newRow("named-scheme")<<QByteArray("scope_colors_scheme")<<int(Field::Scheme)<<QJsonValue("yellow-black");
        QTest::newRow("contrast-snapped")<<QByteArray("scope_contrast")<<int(Field::Contrast)<<QJsonValue(1.24);
        QTest::newRow("automatic-adjustment")<<QByteArray("scope_auto")<<int(Field::AutoContrast)<<QJsonValue(true);
    }
    void declaredViewScope() {
        QFETCH(QByteArray,scenario);QFETCH(int,field);QFETCH(QJsonValue,expected);
        qputenv("OKUFLOW_FAKE_AGENT_SCENARIO",scenario);
        AgentTarget target;
        target.zoom=3;target.fields[Field::CenterX]=0.2;target.fields[Field::CenterY]=0.1;
        target.fields[Field::Scheme]="inverted";
        AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        QVERIFY(RunScope(client,service,{},replies));
        for(const auto reply:replies) {QVERIFY(reply.toObject()["success"].toBool());QCOMPARE(reply.toObject()["images"].toInt(),0);}
        QCOMPARE(target.Fields().at(static_cast<Field>(field)),expected);QCOMPARE(target.saves,1);
        if(scenario=="scope_center")QCOMPARE(target.fields[Field::CenterY].toDouble(),0.75);
        if(scenario=="scope_reset")QCOMPARE(target.fields[Field::CenterY].toDouble(),0.5);
        if(scenario=="scope_contrast")QCOMPARE(target.fields[Field::Brightness].toDouble(),0.13);
        if(scenario.startsWith("scope_colors_high_contrast")) {
            QVERIFY(target.fields[Field::Adaptive].toBool());QCOMPARE(target.fields[Field::Polarity].toInt(),0);
        }
        if(scenario=="scope_auto") {QCOMPARE(target.metrics,1);QVERIFY(Data(replies)["pending"].toBool());
            QCOMPARE(target.operations.SafeStatus(target.lastOperation)["state"].toString(),"starting");}
        QVERIFY(service.StopAndUndo().content["ok"].toBool());
        QCOMPARE(target.zoom,3.0);QCOMPARE(target.fields[Field::Scheme].toString(),"inverted");
    }
    void observeRequiresExplicitConsent_data() {
        QTest::addColumn<bool>("grant");QTest::newRow("denied-until-ui-grant")<<false;QTest::newRow("granted-image-result")<<true;
    }
    void observeRequiresExplicitConsent() {
        QFETCH(bool,grant);qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","scope_observe");
        AgentTarget target;AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        QVERIFY(RunScope(client,service,{grant,false,false,false},replies));
        QCOMPARE(target.observations,grant?1:0);QCOMPARE(replies[0].toObject()["images"].toInt(),grant?1:0);
        if(!grant) {QCOMPARE(Error(Data(replies)),"consent_required");
            const auto approval=service.PendingApproval()["approvalId"].toString();QVERIFY(!approval.isEmpty());
            QVERIFY(service.ResolveApproval(approval,true).content["grantActivated"].toBool());
            QCOMPARE(target.observations,0); // finished turn never receives pixels
            const auto next=service.Begin({ContextKind::EmbeddedTurn,"trusted-scope",2,{}});
            QVERIFY(!next.isEmpty());QVERIFY(service.ContextStatus()["grants"].toObject()["frames"].toBool());
        } else QVERIFY(Data(replies)["ok"].toBool());
    }
    void orientationApprovalIsExactAndSingleUse_data() {
        QTest::addColumn<bool>("allow");QTest::newRow("allow")<<true;QTest::newRow("deny")<<false;
    }
    void orientationApprovalIsExactAndSingleUse() {
        QFETCH(bool,allow);qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","scope_orientation");
        AgentTarget target;AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        // Embedded source grant is ignored, even when a caller tries to supply it.
        QVERIFY(RunScope(client,service,{false,false,false,true},replies));
        QCOMPARE(Error(Data(replies)),"consent_required");QCOMPARE(target.effects,0);
        const auto approval=service.PendingApproval()["approvalId"].toString();QVERIFY(!approval.isEmpty());
        const auto result=service.ResolveApproval(approval,allow);
        QCOMPARE(result.content["ok"].toBool(),allow);QCOMPARE(target.fields[Field::Rotation].toInt(),allow?90:0);
        QCOMPARE(Error(service.ResolveApproval(approval,true).content),"not_found");
        QVERIFY(!service.ContextStatus()["grants"].toObject()["source"].toBool());
    }
    void repeatedDeniedActionDoesNotWidenConsent() {
        qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","scope_orientation_denied");
        AgentTarget target;AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        int approvals=0;
        service.SetEventSink([&](const QJsonObject& event) {if(event["type"]=="approval_requested") {
            ++approvals;const auto id=event["approvalId"].toString();
            QTimer::singleShot(0,&service,[&,id]{service.ResolveApproval(id,false);});
        }});
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        QVERIFY(RunScope(client,service,{},replies));QCOMPARE(replies.size(),2);
        QCOMPARE(Error(Data(replies,0)),"consent_required");QCOMPARE(Error(Data(replies,1)),"consent_denied");
        QCOMPARE(approvals,1);QCOMPARE(target.effects,0);
    }
    void durablePhotoAndOwnedRecordingFlow_data() {
        QTest::addColumn<QByteArray>("scenario");
        QTest::newRow("photo-pending-committed")<<QByteArray("scope_photo");
        QTest::newRow("owned-recording-start-stop")<<QByteArray("scope_recording");
    }
    void durablePhotoAndOwnedRecordingFlow() {
        QFETCH(QByteArray,scenario);qputenv("OKUFLOW_FAKE_AGENT_SCENARIO",scenario);
        AgentTarget target;AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        QVERIFY(RunScope(client,service,{false,true,true,false},replies));
        for(const auto reply:replies)QVERIFY(reply.toObject()["success"].toBool());
        QVERIFY(Data(replies)["pending"].toBool());
        const auto operation=Data(replies)["operationId"].toString();QVERIFY(!operation.isEmpty());
        QCOMPARE(target.operations.SafeStatus(operation)["state"].toString(),scenario=="scope_photo"?"committed":"completed");
        if(scenario=="scope_photo") {
            QCOMPARE(replies.size(),4);QCOMPARE(Data(replies,1)["state"].toString(),"writing");
            QCOMPARE(Data(replies,3)["state"].toString(),"committed");QCOMPARE(target.photos,1);
        } else {
            QCOMPARE(replies.size(),7);QCOMPARE(Data(replies,2)["state"].toString(),"recording");
            QCOMPARE(Data(replies,4)["state"].toString(),"stopping");QCOMPARE(Data(replies,6)["state"].toString(),"completed");
            QCOMPARE(target.starts,1);QCOMPARE(target.stops,1);QVERIFY(!target.recording);QCOMPARE(target.owner,"trusted-scope");
        }
    }
    void captureGrantMissingCreatesNoMediaEffects_data() {
        QTest::addColumn<QByteArray>("scenario");QTest::newRow("photo")<<QByteArray("scope_photo");
        QTest::newRow("recording")<<QByteArray("scope_recording");
    }
    void captureGrantMissingCreatesNoMediaEffects() {
        QFETCH(QByteArray,scenario);qputenv("OKUFLOW_FAKE_AGENT_SCENARIO",scenario);
        AgentTarget target;AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        QVERIFY(RunScope(client,service,{},replies));QCOMPARE(Error(Data(replies)),"consent_required");
        QCOMPARE(target.photos,0);QCOMPARE(target.starts,0);QCOMPARE(target.stops,0);QVERIFY(!target.recording);
        // Subsequent fake references are empty and rejected; they create no grant.
        QVERIFY(!service.ContextStatus()["grants"].toObject()["recording"].toBool());
    }
    void completedTurnRecordingIsStoppedOnlyWhenOwnerRetires() {
        qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","scope_recording_start");
        AgentTarget target;AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        QVERIFY(RunScope(client,service,{false,false,true,false},replies));
        QVERIFY(Data(replies)["pending"].toBool());QVERIFY(target.recording);QCOMPARE(target.stops,0);
        service.RetireOwner("unrelated-owner");QVERIFY(target.recording);
        service.RetireOwner("trusted-scope");QVERIFY(!target.recording);QCOMPARE(target.stops,1);
    }
    void manualRecordingCannotBeStoppedByStandingGrant() {
        qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","scope_manual_stop");
        AgentTarget target;target.recording=true;target.owner="manual";target.session="fixture-manual";
        AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        QVERIFY(RunScope(client,service,{false,false,true,false},replies));QCOMPARE(Error(Data(replies)),"consent_required");
        QVERIFY(target.recording);QCOMPARE(target.stops,0);
        const auto approval=service.PendingApproval()["approvalId"].toString();
        QVERIFY(service.ResolveApproval(approval,true).content["ok"].toBool());QCOMPARE(target.stops,1);QVERIFY(!target.recording);
    }
    void staleRecordingStopCannotStopReplacement() {
        qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","scope_stale_stop");
        AgentTarget target;target.recording=true;target.owner="trusted-scope";target.session="fixture-replacement";
        AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        QVERIFY(RunScope(client,service,{false,false,true,false},replies));
        QCOMPARE(Error(Data(replies)),"stale_state");QVERIFY(target.recording);QCOMPARE(target.stops,0);
    }
    void temporaryCameraApprovalPreservesSavedSelection_data() {
        QTest::addColumn<bool>("nativeEdit");QTest::newRow("temporary-only")<<false;QTest::newRow("native-edit-wins")<<true;
    }
    void temporaryCameraApprovalPreservesSavedSelection() {
        QFETCH(bool,nativeEdit);
        qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","scope_camera");
        AgentTarget target;AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QJsonArray replies;
        QVERIFY(RunScope(client,service,{},replies));QCOMPARE(replies.size(),2);
        QCOMPARE(Data(replies)["sources"].toArray()[0].toObject()["displayName"].toString(),"Camera 2");
        QCOMPARE(Error(Data(replies,1)),"consent_required");QVERIFY(target.lastOperation.isEmpty());
        const auto approval=service.PendingApproval()["approvalId"].toString();
        if(nativeEdit) {
            target.camera.SelectManual("fixture-new-native",1);service.NativeFieldsChanged({Field::Camera});
            QCOMPARE(Error(service.ResolveApproval(approval,true).content),"stale_state");
            QVERIFY(target.lastOperation.isEmpty());QCOMPARE(target.camera.Saved().symbolicLink,"fixture-new-native");return;
        }
        QVERIFY(service.ResolveApproval(approval,true).content["ok"].toBool());
        QCOMPARE(target.camera.Saved().symbolicLink,"fixture-saved-webcam");
        QCOMPARE(target.camera.Requested().identity.symbolicLink,"fixture-temporary-camera");
        QCOMPARE(target.camera.Active()->symbolicLink,"fixture-saved-webcam");
        target.CompleteOperation(target.lastOperation);
        QCOMPARE(target.operations.SafeStatus(target.lastOperation)["state"].toString(),"first_frame");
        QCOMPARE(target.camera.Active()->symbolicLink,"fixture-temporary-camera");
        QVERIFY(target.camera.ReleaseTemporary("trusted-scope"));
        QCOMPARE(target.camera.Saved().symbolicLink,"fixture-saved-webcam");
        QVERIFY(!QJsonDocument(QJsonObject{{"replies",replies}}).toJson().contains("fixture-saved-webcam"));
    }
    void nativeChangesWinBeforeBufferedMutationDispatch() {
        qputenv("OKUFLOW_FAKE_AGENT_SCENARIO","scope_user_wins");
        AgentTarget target;AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));
        connect(&client,&CodexAgentClient::TurnStarted,&service,[&] {
            target.fields[Field::Contrast]=3.0;service.NativeFieldsChanged({Field::Contrast});
        });
        QJsonArray replies;QVERIFY(RunScope(client,service,{},replies));
        QCOMPARE(Error(Data(replies)),"stale_state");QCOMPARE(target.fields[Field::Contrast].toDouble(),3.0);QCOMPARE(target.effects,0);
    }
    void cancelRevokesLateObservationAndPreservesAcceptedPhoto_data() {
        QTest::addColumn<bool>("photo");QTest::newRow("late-observation")<<false;QTest::newRow("accepted-photo")<<true;
    }
    void cancelRevokesLateObservationAndPreservesAcceptedPhoto() {
        QFETCH(bool,photo);qputenv("OKUFLOW_FAKE_AGENT_SCENARIO",photo?"scope_photo":"scope_observe");
        AgentTarget target;target.deferObservation=!photo;target.deferPhoto=photo;
        AppControlService service(target,CatalogEdition::DevCamera);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"trusted-scope",1,{true,true,false,false}});
        QVERIFY(client.SubmitTurn(context,"request then cancel"));QTRY_VERIFY_WITH_TIMEOUT(bool(target.delayed),10000);
        client.Cancel();QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QCOMPARE(done[0][3].toString(),"canceled");
        QVERIFY(!service.AuthorizeFrameDelivery(context,service.Stamp()));
        if(photo) {const auto operation=target.lastOperation;target.CompleteOperation(operation);
            QCOMPARE(target.operations.SafeStatus(operation)["state"].toString(),"committed");target.delayed({target.operations.SafeStatus(operation),{}});}
        else target.delayed({{{"ok",true}},QByteArray("late-private-marker")});
        QTest::qWait(30);QCOMPARE(done.size(),1);QVERIFY(!service.ContextStatus()["active"].toBool());
        QVERIFY(!done[0][2].toString().contains("late-private-marker"));
    }
    void probeAndBufferedCall_data() {
        QTest::addColumn<QByteArray>("scenario");
        QTest::newRow("call_before_ack")<<QByteArray{};
        QTest::newRow("stale_arrives_first")<<QByteArray("stale_first");
        QTest::newRow("retired_arrives_first")<<QByteArray("retired_first");
    }
    void probeAndBufferedCall() {
        QFETCH(QByteArray,scenario);qputenv("OKUFLOW_FAKE_AGENT_SCENARIO",scenario);
        AgentTarget target;AppControlService service(target);service.SetEnabled(true,false);
        CodexAgentClient client;client.SetControlService(&service);
        client.Configure(QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY),{},"low",{});
        QVERIFY(!client.IsReady());client.TestCapability();
        QTRY_VERIFY_WITH_TIMEOUT(client.IsReady() && !client.IsTurnActive(),10000);
        QCOMPARE(target.effects,0);
        auto context=service.Begin({ContextKind::EmbeddedTurn,"trusted",1,{}});QVERIFY(!context.isEmpty());
        QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        client.SubmitTurn(context,"zoom in");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);
        QCOMPARE(done[0][3].toString(),QString{});QCOMPARE(done[0][2].toString(),"done");
        QCOMPARE(target.effects,1);QCOMPARE(target.zoom,2.0);
    }
    void forbiddenFailsClosed_data() {
        QTest::addColumn<QByteArray>("scenario");
        QTest::newRow("undeclared_tool")<<QByteArray("forbidden_tool");
        QTest::newRow("forbidden_item")<<QByteArray("forbidden_item");
    }
    void forbiddenFailsClosed() {
        QFETCH(QByteArray,scenario);qputenv("OKUFLOW_FAKE_AGENT_SCENARIO",scenario);
        AgentTarget target;AppControlService service(target);service.SetEnabled(true,false);
        CodexAgentClient client;client.SetControlService(&service);
        client.Configure(QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY),{},"low",{});
        client.TestCapability();QTRY_VERIFY_WITH_TIMEOUT(client.IsReady() && !client.IsTurnActive(),10000);
        QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        client.SubmitTurn(service.Begin({ContextKind::EmbeddedTurn,"trusted",1,{}}),"zoom in");
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);
        QVERIFY(!done[0][3].toString().isEmpty());QVERIFY(!client.IsReady());
        QCOMPARE(target.effects,0);
    }
    void unavailableDoesNotExecute() {
        AgentTarget target;AppControlService service(target);CodexAgentClient client;
        client.SetControlService(&service);QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        QVERIFY(!client.SubmitTurn("fabricated","zoom"));QCOMPARE(done.size(),0);QCOMPARE(target.effects,0);
    }
    void reconfigurationInvalidatesTestedReadiness() {
        AgentTarget target;AppControlService service(target);service.SetEnabled(true,false);
        CodexAgentClient client;QVERIFY(Ready(client,service));
        QSignalSpy availability(&client,&CodexAgentClient::AvailabilityChanged);
        client.Configure(QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY),{},"low",{});
        QCOMPARE(availability.size(),0);QVERIFY(client.IsReady());
        client.Configure(QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY),"changed","low",{});
        QCOMPARE(availability.size(),1);QVERIFY(!availability[0][0].toBool());
        QVERIFY(!client.IsReady());QVERIFY(!client.IsTurnActive());
        // No implicit probe: readiness returns only through another explicit test.
        QTest::qWait(200);QCOMPARE(availability.size(),1);
        QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"trusted",1,{}});
        QVERIFY(!context.isEmpty());QVERIFY(!client.SubmitTurn(context,"zoom"));
        QCOMPARE(done.size(),0);QCOMPARE(target.effects,0);service.End(context);
        client.TestCapability();QTRY_VERIFY_WITH_TIMEOUT(client.IsReady() && !client.IsTurnActive(),10000);
    }
    void nestedConfigurationRetainsNewestModel() {
        AgentTarget target;AppControlService service(target);service.SetEnabled(true,false);
        CodexAgentClient client;client.SetControlService(&service);
        const auto executable=QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY);
        client.Configure(executable,"original","low",{});
        client.TestCapability();QTRY_VERIFY_WITH_TIMEOUT(client.IsReady() && !client.IsTurnActive(),10000);
        bool changed=false,nested=false;
        const auto firstStarted=connect(&client,&CodexAgentClient::TurnStarted,&client,[&] {
            if(!changed){changed=true;client.Configure(executable,"obsolete","low",{});}
        });
        const auto firstFinished=connect(&client,&CodexAgentClient::TurnFinished,&client,[&] {
            if(!nested){nested=true;client.Configure(executable,"newest","low",{});}
        });
        client.SubmitTurn(service.Begin({ContextKind::EmbeddedTurn,"trusted",1,{}}),"zoom");
        QTRY_VERIFY_WITH_TIMEOUT(changed && nested && !client.IsTurnActive(),10000);
        disconnect(firstStarted);disconnect(firstFinished);
        client.TestCapability();QTRY_VERIFY_WITH_TIMEOUT(client.IsReady() && !client.IsTurnActive(),10000);
        QSignalSpy conversation(&client,&CodexAgentClient::ConversationCreated);
        QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        client.SubmitTurn(service.Begin({ContextKind::EmbeddedTurn,"trusted",2,{}}),"zoom");
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);
        QCOMPARE(conversation.size(),1);
        QCOMPARE(conversation[0][0].toJsonObject()["model"].toString(),"newest");
    }
    void bridgeContextRejectsImagesToolsAndExcess() {
        AgentTarget target;AppControlService service(target);service.SetEnabled(true,false);
        CodexAgentClient client;client.SetControlService(&service);
        client.Configure(QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY),{},"low",{});
        client.TestCapability();QTRY_VERIFY_WITH_TIMEOUT(client.IsReady() && !client.IsTurnActive(),10000);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"trusted",1,{}});
        QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        for(const auto history: {QJsonArray{QJsonObject{{"role","tool"},{"content","untrusted"}}},
            QJsonArray{QJsonObject{{"role","assistant"},{"content",QJsonArray{QJsonObject{{"image_url","private"}}}}}},
            QJsonArray{QJsonObject{{"role","user"},{"content",QString(32769,'x')}}},
            QJsonArray{QJsonObject{{"role","assistant"},{"content","text"},{"threadId","foreign"}}}}) {
            QVERIFY(!client.SubmitTurn(context,"zoom",{},history));
        }
        QCOMPARE(done.size(),0);QCOMPARE(target.effects,0);
        QVERIFY(client.SubmitTurn(context,"zoom",{},QJsonArray{QJsonObject{{"role","user"},{"content","Explain the equation"}},
            QJsonObject{{"role","assistant"},{"content","K=half mv squared"}}}));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);
        QVERIFY(done[0][3].toString().isEmpty());QCOMPARE(target.effects,1);
    }
    void conversationCallbackCanReplaceTurnWithoutDuplicateStart() {
        AgentTarget target;AppControlService service(target);service.SetEnabled(true,false);
        CodexAgentClient client;client.SetControlService(&service);
        client.Configure(QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY),{},"low",{});
        client.TestCapability();QTRY_VERIFY_WITH_TIMEOUT(client.IsReady() && !client.IsTurnActive(),10000);
        QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        bool replaced=false, admitted=false;
        connect(&client,&CodexAgentClient::ConversationCreated,&client,[&] {
            if(replaced)return;
            replaced=true;client.Cancel();
            admitted=client.SubmitTurn(service.Begin({ContextKind::EmbeddedTurn,"replacement",2,{}}),"replacement");
        });
        QVERIFY(client.SubmitTurn(service.Begin({ContextKind::EmbeddedTurn,"original",1,{}}),"original"));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,10000);
        QVERIFY(replaced && admitted);
        QCOMPARE(done[0][3].toString(),"canceled");
        QVERIFY(done[1][3].toString().isEmpty());
        QCOMPARE(target.effects,1);
    }
    void capabilityCallbackCanReconfigureWithoutStaleProbe() {
        AgentTarget target;AppControlService service(target);service.SetEnabled(true,false);
        CodexAgentClient client;client.SetControlService(&service);
        const auto executable=QStringLiteral(OKUFLOW_FAKE_AGENT_BINARY);
        client.Configure(executable,"original","low",{});
        bool replaced=false;
        connect(&client,&CodexAgentClient::AvailabilityChanged,&client,[&](bool,const QString& code) {
            if(!replaced && code=="checking") {
                replaced=true;client.Configure(executable,"replacement","low",{});
            }
        });
        client.TestCapability();QVERIFY(replaced);
        QVERIFY(!client.IsReady());QVERIFY(!client.IsTurnActive());
        client.TestCapability();QTRY_VERIFY_WITH_TIMEOUT(client.IsReady() && !client.IsTurnActive(),10000);
        QSignalSpy conversation(&client,&CodexAgentClient::ConversationCreated);
        QSignalSpy done(&client,&CodexAgentClient::TurnFinished);
        QVERIFY(client.SubmitTurn(service.Begin({ContextKind::EmbeddedTurn,"trusted",1,{}}),"zoom"));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);
        QCOMPARE(conversation.size(),1);
        QCOMPARE(conversation[0][0].toJsonObject()["model"].toString(),"replacement");
        QVERIFY(done[0][3].toString().isEmpty());
    }
};
QTEST_GUILESS_MAIN(CodexAgentClientTests)
#include "codex_agent_client_tests.moc"
