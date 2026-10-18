// Desktop-free authority/catalog fixtures. The target below is deliberately
// synthetic; native decode, media presentation and provider acceptance are separate.
#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/app/agent/control_schema.hpp"
#include "okuflow/app/agent/operation_tracker.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/agent/recording_parameter_policy.hpp"
#include <QEventLoop>
#include <QJsonDocument>
#include <QtTest>
using namespace okuflow::agent;
namespace {
const QString kDocument=QStringLiteral("media-0123456789abcdef0123456789abcdef");
struct SyntheticMediaTarget final:IAppControlTarget {
    int executions{}, applied{}, stopped{};
    bool observationCurrent{true};
    QString ownedSource;
    quint64 generation{1};
    QJsonObject State()const override{return {{"video",Query(ToolId::VideoState,{})}};}
    std::map<Field,QJsonValue> Fields()const override {
        return {{Field::ZoomAmount,1.0},{Field::VideoSelection,QJsonObject{{"documentId",kDocument},{"selectionGeneration",qint64(generation)}}}};
    }
    ControlPlan Prepare(ToolId tool,const QJsonObject& args,const QString&)const override {
        ControlPlan p;
        if(tool==ToolId::VideoState)p.observedFields={Field::VideoSelection};
        else if(IsVideoControlTool(tool)) {
            if(args["documentId"]!=kDocument || args["selectionGeneration"].toInteger()!=generation) {p.error="stale_state";return p;}
            p.requiredFields={Field::VideoSelection};p.asynchronous=true;p.requiresPhotos=tool==ToolId::VideoSaveFrame;
        } else if(tool==ToolId::Zoom)p.writes.push_back({Field::ZoomAmount,args["magnification"]});
        return p;
    }
    bool Apply(const FieldWrite&)override{++applied;return true;}
    bool Save()override{return true;}
    QJsonObject Query(ToolId,const QJsonObject&)const override {
        return {{"ok",true},{"documentId",kDocument},{"selectionGeneration",qint64(generation)},
                {"selectedPts100ns",10050000},{"kind","file"},{"state","paused"}};
    }
    void Execute(ToolId,const QJsonObject&,const QString&,ContextKind,ControlCompletion completion)override {
        ++executions;
        completion({{{"ok",true},{"pending",true},{"operationId","native-operation"},{"state","starting"}},{}});
    }
    bool ModalActive()const override{return false;}
    bool UserInteracting()const override{return false;}
    bool RecordingActive()const override{return true;}
    QString RecordingSession()const override{return "another-session";}
    QString RecordingOwner()const override{return "another-owner";}
    void StopOwnedRecording(const QString& owner)override{if(owner==RecordingOwner())++stopped;}
    bool HasOwnedSource(const QString& owner)const override{return !owner.isEmpty() && ownedSource==owner;}
    void CancelOwnedSource(const QString& owner)override{if(HasOwnedSource(owner))ownedSource.clear();}
    bool ObservationStillCurrent(const AuthorizedObservation&)const override{return observationCurrent;}
};
ControlResult Call(AppControlService& service,const QString& context,const QString& key,const QString& tool,QJsonObject args={}) {
    ControlResult result;
    QEventLoop loop;
    QTimer deadline;deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    deadline.start(1500);
    service.Submit({context,key,tool,args},[&](ControlResult r){result=std::move(r);loop.quit();});
    loop.exec();
    return result;
}
QString Code(const ControlResult& result){return result.content["error"].toObject()["code"].toString();}
QJsonObject Ticket(quint64 generation=1){return {{"documentId",kDocument},{"selectionGeneration",qint64(generation)}};}
}
class VideoAgentControlTests final:public QObject {
    Q_OBJECT
private slots:
    void finiteCatalogHasNoHostPathOpen() {
        QCOMPARE(ControlRegistry(CatalogEdition::DevFull).size(),std::size_t(43));
        QCOMPARE(ControlRegistry(CatalogEdition::V1).size(),std::size_t(43));
        for(const auto edition:{CatalogEdition::DevCore,CatalogEdition::DevCapture,CatalogEdition::DevCamera})
            QVERIFY(!FindControlTool("video.state.get",edition));
        for(const auto edition:{CatalogEdition::DevCore,CatalogEdition::DevCapture,CatalogEdition::DevCamera,CatalogEdition::V1,CatalogEdition::DevFull})
            QVERIFY(!FindControlTool("video.open",edition));
        int mutations{};
        for(const auto& entry:ControlRegistry(CatalogEdition::DevFull)) {
            if(!IsVideoControlTool(entry.id) || entry.id==ToolId::VideoState)continue;
            ++mutations;
            const auto schema=QJsonDocument::fromJson(QByteArray::fromStdString(entry.inputSchema)).object();
            auto valid=Ticket();if(entry.id==ToolId::VideoSeek)valid["positionMs"]=1000;
            QVERIFY(MatchesControlSchema(valid,schema));
            valid["path"]="C:/private/file.mp4";QVERIFY(!MatchesControlSchema(valid,schema));
            valid.remove("path");valid["documentId"]="C:/private/file.mp4";QVERIFY(!MatchesControlSchema(valid,schema));
        }
        QCOMPARE(mutations,6);
        bool found=false;
        for(const auto& action:ControlActionRegistry())if(action.id=="video.open") {found=true;QCOMPARE(action.status,std::string_view("native-ui-only"));}
        QVERIFY(found);
    }
    void nativeSelectionRequiresFreshReadWithoutStoppingRecording() {
        SyntheticMediaTarget target;AppControlService service(target,CatalogEdition::DevFull);
        service.SetEnabled(true,false);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"agent",1,{}});QVERIFY(!context.isEmpty());
        target.generation=2;service.SourceChanged();
        auto stale=Call(service,context,"old","video.pause",Ticket(2));
        QCOMPARE(Code(stale),QStringLiteral("stale_state"));QCOMPARE(target.executions,0);
        auto staleZoom=Call(service,context,"old-zoom","view.zoom.set",{{"magnification",2}});
        QCOMPARE(Code(staleZoom),QStringLiteral("stale_state"));QCOMPARE(target.applied,0);
        QVERIFY(Call(service,context,"read","video.state.get").content["ok"].toBool());
        auto paused=Call(service,context,"pause","video.pause",Ticket(2));
        QVERIFY(paused.content["ok"].toBool());QVERIFY(paused.content["pending"].toBool());
        QCOMPARE(target.executions,1);QCOMPARE(target.stopped,0);
    }
    void saveHasPhotoAuthorityAndExactTicket() {
        SyntheticMediaTarget target;AppControlService service(target,CatalogEdition::DevFull);
        service.SetEnabled(false,true);
        const auto context=service.Begin({ContextKind::ExternalLease,"lease",1,{}});QVERIFY(!context.isEmpty());
        QCOMPARE(Code(Call(service,context,"save","video.save_frame",Ticket())),QStringLiteral("denied_by_policy"));
        QCOMPARE(target.executions,0);QCOMPARE(target.stopped,0);
        service.End(context);
        const auto granted=service.Begin({ContextKind::ExternalLease,"lease",2,{false,true,false,false}});QVERIFY(!granted.isEmpty());
        QCOMPARE(Code(Call(service,granted,"stale","video.save_frame",Ticket(2))),QStringLiteral("stale_state"));
        QVERIFY(Call(service,granted,"save","video.save_frame",Ticket()).content["pending"].toBool());
        QCOMPARE(target.executions,1);QCOMPARE(target.stopped,0);
    }
    void canvasRetirementIsAtomicBeforeMicrophoneEffects() {
        const QString microphone="mic-0123456789abcdef0123456789abcdef";
        const std::map<Field,QJsonValue> fields{{Field::RecordingMicrophone,"none"},{Field::RecordingCanvas,"source"}};
        for(const auto* canvas:{"source","360p","480p","720p","1080p","1440p","2160p"}) {
            const auto plan=PrepareRecordingPreferencePatch(fields,{microphone},
                {{"recording.canvas",canvas},{"recording.microphone",microphone}},{"foreign-session","12:00",true});
            QCOMPARE(plan.error,QStringLiteral("unavailable"));QVERIFY(plan.writes.empty());
            QVERIFY(!plan.requiresApproval && !plan.requiresRecording && !plan.asynchronous);
        }
        const auto* descriptor=FindParameter("recording.canvas");QVERIFY(descriptor);
        QCOMPARE(descriptor->access,ParameterAccess::UiOnly);
        const auto schema=QJsonDocument::fromJson(QByteArray::fromStdString(ParameterSetSchema(ParameterGroup::Recording))).object();
        QVERIFY(!MatchesControlSchema(QJsonObject{{"recording.canvas","source"}},schema));
        const auto microphoneOnly=PrepareRecordingPreferencePatch(fields,{microphone},{{"recording.microphone",microphone}});
        QVERIFY(microphoneOnly.error.isEmpty());QCOMPARE(microphoneOnly.writes.size(),std::size_t(1));
    }
    void durableReceiptDistinguishesRequestedFromActualTime() {
        OperationTracker tracker;const auto id=tracker.Create(OperationKind::Video,"owner");QVERIFY(!id.isEmpty());
        QVERIFY(tracker.Advance(id,OperationState::Starting));
        QVERIFY(tracker.Advance(id,OperationState::Completed,{{"documentId",kDocument},{"selectionGeneration",3},
            {"requestGeneration",8},{"completedRequestGeneration",8},{"requestedPositionMs",1000},{"selectedPts100ns",10050000},
            {"kind","file"},{"mode","file_end"},{"path","C:/private/file.mp4"},{"recordingActive",true}}));
        const auto receipt=tracker.SafeStatus(id);
        QCOMPARE(receipt["state"].toString(),QStringLiteral("completed"));
        QCOMPARE(receipt["requestedPositionMs"].toInt(),1000);QCOMPARE(receipt["selectedPts100ns"].toInteger(),10050000);
        QCOMPARE(receipt["documentId"].toString(),kDocument);QVERIFY(!receipt.contains("path"));
        QVERIFY(receipt["recordingActive"].toBool());
    }
    void admittedAuthoritySurvivesNormalCompletionAndRetainsRevocation() {
        SyntheticMediaTarget target;AppControlService service(target,CatalogEdition::DevFull);
        service.SetEnabled(true,false);
        const auto first=service.Begin({ContextKind::EmbeddedTurn,"owner",1,{false,true,false,false}});
        const auto firstAuthority=service.CaptureOperationAuthority(first);QVERIFY(firstAuthority);
        service.End(first,true);QVERIFY(!firstAuthority->canceled);
        const auto second=service.Begin({ContextKind::EmbeddedTurn,"owner",2,{false,true,false,false}});
        const auto secondAuthority=service.CaptureOperationAuthority(second);QVERIFY(secondAuthority);
        service.SetGrants(second,{});QVERIFY(secondAuthority->photosRevoked);QVERIFY(!secondAuthority->canceled);
        service.RetireOwner("owner");QVERIFY(firstAuthority->canceled);QVERIFY(secondAuthority->canceled);
    }
    void immutableDeliveryChecksCurrentNativeFrameAsWellAsGrant() {
        SyntheticMediaTarget target;AppControlService service(target,CatalogEdition::DevFull);
        service.SetEnabled(true,false);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"owner",1,{true,false,false,false}});
        AuthorizedObservation observation{context,service.Stamp(),0,1,0};
        observation.immutableSource=true;observation.documentId=kDocument;
        observation.documentGeneration=observation.selectionGeneration=1;observation.mediaPts100ns=10050000;
        QVERIFY(service.AuthorizeFrameDelivery(observation));
        target.observationCurrent=false;
        QVERIFY(!service.AuthorizeFrameDelivery(observation));
    }
    void nativeSourceChangeDefeatsPriorViewUndo() {
        SyntheticMediaTarget target;AppControlService service(target,CatalogEdition::DevFull);
        service.SetEnabled(true,false);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"owner",1,{}});
        QVERIFY(Call(service,context,"zoom","view.zoom.set",{{"magnification",2}}).content["ok"].toBool());
        QCOMPARE(target.applied,1);
        target.generation=2;service.SourceChanged();
        const auto undo=service.StopAndUndo();
        QVERIFY(undo.content["skipped"].toArray().contains("zoomAmount"));
        QCOMPARE(target.applied,1);
    }
    void ownedCameraRestoreKeepsUndoOnlyForTheSameMediaSelection_data() {
        QTest::addColumn<bool>("nativeSelection");
        QTest::newRow("owned-camera-only")<<false;
        QTest::newRow("native-video-selection")<<true;
    }
    void ownedCameraRestoreKeepsUndoOnlyForTheSameMediaSelection() {
        QFETCH(bool,nativeSelection);
        SyntheticMediaTarget target;AppControlService service(target,CatalogEdition::DevFull);
        service.SetEnabled(true,false);
        const auto context=service.Begin({ContextKind::EmbeddedTurn,"owner",1,{}});
        QVERIFY(Call(service,context,"zoom","view.zoom.set",{{"magnification",2}}).content["ok"].toBool());
        target.ownedSource="owner";
        service.OwnedSourceRequested("owner",{{Field::Camera,"camera-original"},{Field::CameraFormat,"automatic"}});
        if(nativeSelection) {++target.generation;service.SourceChanged();}
        const auto undo=service.StopAndUndo().content;
        QVERIFY(undo["sourceRestoreRequested"].toBool());QVERIFY(target.ownedSource.isEmpty());
        QCOMPARE(undo["restored"].toArray().contains("zoomAmount"),!nativeSelection);
        QCOMPARE(undo["skipped"].toArray().contains("zoomAmount"),nativeSelection);
        QCOMPARE(target.applied,nativeSelection?1:2);
    }
};
QTEST_GUILESS_MAIN(VideoAgentControlTests)
#include "video_agent_control_tests.moc"
