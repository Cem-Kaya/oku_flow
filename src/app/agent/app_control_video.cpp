#ifdef _WIN32
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/control_schema.hpp"
#include "okuflow/app/app.hpp"
#include <QDateTime>
#include <QJsonArray>
#include <QPointer>
#include <QRegularExpression>
#include <chrono>
#include <cmath>

namespace okuflow::agent {
namespace {
qint64 VideoNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool MediaId(const QString& text) {
    return QRegularExpression(QStringLiteral("^media-[a-f0-9]{32}$")).match(text).hasMatch();
}
QJsonObject VideoSummary(const QJsonObject& state) {
    QJsonObject summary;
    for(const auto* key:{"documentId","selectionGeneration","requestGeneration","completedRequestGeneration",
                        "selectedPts100ns","provisionalCaptureClock100ns","kind","playing","hasFrame","heldUnrecorded"})
        if(state.contains(key))summary[key]=state[key];
    summary["mode"]=state["state"];
    return summary;
}
}

bool AppControlTarget::ObservationStillCurrent(const AuthorizedObservation& expected) const {
    if(!expected.immutableSource)return !app_.MediaViewSelected();
    if(!app_.MediaViewSelected())return false;
    const auto state=app_.SelectedVideoState();
    const qint64 pts=state["selectedPts100ns"].isDouble()?state["selectedPts100ns"].toInteger():-1;
    return state["hasFrame"].toBool() && state["documentId"]==expected.documentId &&
        state["selectionGeneration"].toInteger()==expected.documentGeneration &&
        state["selectionGeneration"].toInteger()==expected.selectionGeneration &&
        static_cast<quint64>(state["frameSequence"].toInteger())==expected.frameSequence && pts==expected.mediaPts100ns;
}

QJsonObject AppControlTarget::QueryVideoState() const {
    const auto native=app_.SelectedVideoState();
    QJsonObject state;
    const QStringList kinds{"camera","recording","file"};
    const QStringList states{"camera","live","behind_live","paused","playing","end","opening","seeking",
                             "waiting","finishing","unavailable","holding","held_unrecorded"};
    state["kind"]=kinds.contains(native["kind"].toString())?native["kind"]:QJsonValue("camera");
    state["state"]=states.contains(native["state"].toString())?native["state"]:QJsonValue("unavailable");
    for(const auto* key:{"documentId","saveDocumentId"}) {
        const auto text=native[key].toString();
        state[key]=MediaId(text)?text:QString();
    }
    for(const auto* key:{"selectionGeneration","requestGeneration","completedRequestGeneration","saveRevision",
                        "selectedPts100ns","provisionalCaptureClock100ns","seekableEnd100ns",
                        "saveFrameSequence","savePts100ns","saveSelectionGeneration"}) {
        const auto value=native[key];
        state[key]=value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble()>=0 &&
            value.toDouble()<=9007199254740991.0?value:QJsonValue(QJsonValue::Null);
    }
    for(const auto* key:{"playing","hasFrame","canSaveFrame","pendingHold","heldUnrecorded"})state[key]=native[key].toBool();
    const auto save=native["saveState"].toString();
    state["saveState"]=QStringList{"idle","writing","committed","failed"}.contains(save)?save:QStringLiteral("idle");
    // User-selected filenames are untrusted content. Discovery exposes only an
    // opaque process-local identity, never file paths or filename instructions.
    state["recordingActive"]=RecordingActive();
    state["pipeline"]=app_.MediaViewSelected()
        ? (app_.MediaGpuViewSelected()?QStringLiteral("gpu_selected_media"):QStringLiteral("cpu_selected_media"))
        : app_.usingCudaLastFrame_?QStringLiteral("cuda"):QStringLiteral("raw_passthrough");
    state["advancedGpuEffectsAvailable"]=app_.MediaViewSelected()
        ? app_.SelectedMediaGpuProcessingAvailable() : app_.usingCudaLastFrame_;
    state["openPolicy"]="native_ui_only";
    return state;
}

ControlPlan AppControlTarget::PrepareVideoControl(ToolId tool,const QJsonObject& args) const {
    ControlPlan plan;
    const ToolDescriptor* descriptor=nullptr;
    for(const auto& candidate:ControlRegistry(CatalogEdition::DevFull))if(candidate.id==tool) {descriptor=&candidate;break;}
    if(!descriptor || !MatchesControlSchema(args,QJsonDocument::fromJson(QByteArray::fromStdString(descriptor->inputSchema)).object())) {
        plan.error="invalid_argument";return plan;
    }
    if(tool==ToolId::VideoState) {plan.observedFields.push_back(Field::VideoSelection);return plan;}
    const auto state=QueryVideoState();
    if(state["kind"]=="camera" || state["documentId"].toString().isEmpty()) {plan.error="unavailable";return plan;}
    if(args["documentId"]!=state["documentId"] || args["selectionGeneration"]!=state["selectionGeneration"]) {
        plan.error="stale_state";return plan;
    }
    if(!operations_.HasCapacity()) {plan.error="budget_exhausted";return plan;}
    if(state["state"]=="opening" || state["state"]=="finishing") {plan.error="busy";return plan;}
    if(tool==ToolId::VideoSaveFrame && !state["canSaveFrame"].toBool()) {plan.error="unavailable";return plan;}
    if(tool==ToolId::VideoSeek && state["seekableEnd100ns"].isNull()) {plan.error="unavailable";return plan;}
    plan.targetIdentity=state["documentId"].toString();
    plan.requiredFields.push_back(Field::VideoSelection);
    plan.requiresPhotos=tool==ToolId::VideoSaveFrame;
    plan.asynchronous=true;
    return plan;
}

void AppControlTarget::ExecuteVideoControl(ToolId tool,const QJsonObject& args,const QString& owner,
                                         ControlCompletion completion) {
    const auto plan=PrepareVideoControl(tool,args);
    if(!plan.error.isEmpty()) {completion(ControlResult::Failure(plan.error));return;}
    if(tool==ToolId::VideoState) {completion({QueryVideoState(),{}});return;}
    const auto context=app_.controlService_->ContextStatus();
    const auto contextId=context["contextId"].toString();
    const bool standingPhotoGrant=context["grants"].toObject()["photos"].toBool();
    const auto authority=app_.controlService_->CaptureOperationAuthority(contextId);
    if(!authority || authority->canceled) {completion(ControlResult::Failure("canceled"));return;}
    const auto before=QueryVideoState();
    const auto operation=operations_.Create(OperationKind::Video,owner);
    if(operation.isEmpty()) {completion(ControlResult::Failure("budget_exhausted"));return;}
    auto summary=VideoSummary(before);summary["recordingActive"]=RecordingActive();
    if(tool==ToolId::VideoSeek)summary["requestedPositionMs"]=args["positionMs"];
    bool admitted=true;
    switch(tool) {
    case ToolId::VideoPause:admitted=app_.SetSelectedVideoPlaying(false);break;
    case ToolId::VideoResume:admitted=app_.SetSelectedVideoPlaying(true);break;
    case ToolId::VideoSeek:app_.SeekSelectedVideo(args["positionMs"].toInteger());break;
    case ToolId::VideoJumpLatestEnd:app_.JumpSelectedVideoToEnd();break;
    case ToolId::VideoReturnCamera:app_.ReturnVideoToCamera();break;
    case ToolId::VideoSaveFrame:app_.SaveSelectedVideoFrame();break;
    default:admitted=false;break;
    }
    if(!admitted) {
        operations_.Advance(operation,OperationState::Failed,{{"errorCode","unavailable"}});
        completion(ControlResult::Failure("unavailable"));return;
    }
    const auto expected=QueryVideoState();
    if(tool==ToolId::VideoSaveFrame &&
       (expected["saveRevision"].isNull() ||
        expected["saveRevision"].toInteger()<=before["saveRevision"].toInteger())) {
        operations_.Advance(operation,OperationState::Failed,{{"errorCode","unavailable"}});
        completion(ControlResult::Failure("unavailable"));return;
    }
    const auto expectedSave=expected["saveRevision"];
    const auto expectedSelection=expected["selectionGeneration"];
    const auto expectedDocument=expected["documentId"];
    const auto expectedRequest=expected["requestGeneration"];
    summary["requestGeneration"]=expectedRequest;
    if(tool==ToolId::VideoSaveFrame) {
        summary["saveRevision"]=expectedSave;
        summary["documentId"]=expected["saveDocumentId"];
        summary["selectionGeneration"]=expected["saveSelectionGeneration"];
        summary["selectedPts100ns"]=expected["savePts100ns"];
        summary["frameSequence"]=expected["saveFrameSequence"];
    }
    operations_.Advance(operation,OperationState::Starting,summary);
    auto* timer=new QTimer(&observations_);timer->setInterval(25);
    QPointer<OkuFlowApp> app(&app_);
    const auto deadline=VideoNow()+30000;
    QObject::connect(timer,&QTimer::timeout,timer,[app,timer,operation,tool,expectedSave,
        expectedSelection,expectedDocument,summary,deadline,standingPhotoGrant,authority] () mutable {
        if(!app || !app->controlTarget_ || !app->controlService_) {timer->stop();timer->deleteLater();return;}
        auto& target=*app->controlTarget_;
        auto state=target.QueryVideoState();
        QString error;
        if(authority->canceled)error="canceled";
        else if(tool==ToolId::VideoSaveFrame) {
            if(state["saveState"]=="failed")error="unavailable";
            else if(state["saveRevision"]!=expectedSave || state["saveDocumentId"]!=summary["documentId"])error="stale_state";
            else if(standingPhotoGrant && authority->photosRevoked)error="canceled";
        } else if(state["documentId"]!=expectedDocument || state["selectionGeneration"]!=expectedSelection)error="stale_state";
        if(error.isEmpty() && tool!=ToolId::VideoSaveFrame && state["state"]=="unavailable")error="unavailable";
        if(error.isEmpty() && VideoNow()>=deadline)error="timeout";
        bool done=false;
        if(error.isEmpty()) {
            const auto mode=state["state"].toString();
            if(tool==ToolId::VideoSaveFrame)done=state["saveState"]=="committed";
            else if(tool==ToolId::VideoReturnCamera)done=state["kind"]=="camera" ||
                (state["kind"]=="recording" && mode=="live" && state["hasFrame"].toBool());
            else if(tool==ToolId::VideoPause)done=!state["playing"].toBool() && !state["pendingHold"].toBool() && state["hasFrame"].toBool();
            else if(tool==ToolId::VideoResume)done=state["playing"].toBool() && state["hasFrame"].toBool() && (mode=="playing" || mode=="live");
            else if(tool==ToolId::VideoJumpLatestEnd && state["kind"]=="recording" && mode=="live")done=state["hasFrame"].toBool();
            else done=state["hasFrame"].toBool() && state["completedRequestGeneration"]==state["requestGeneration"] && mode!="seeking" && mode!="opening" && mode!="waiting";
        }
        if(error.isEmpty() && !done)return;
        timer->stop();timer->deleteLater();
        if(!error.isEmpty()) {summary["errorCode"]=error;target.operations_.Advance(operation,OperationState::Failed,summary);return;}
        if(tool==ToolId::VideoSaveFrame)summary["saveState"]="committed";
        else {
            const auto result=VideoSummary(state);
            for(auto it=result.begin();it!=result.end();++it)
                if(it.key()!="documentId" && it.key()!="selectionGeneration")summary[it.key()]=it.value();
            summary["resultDocumentId"]=state["documentId"];
            summary["resultSelectionGeneration"]=state["selectionGeneration"];
            if(tool==ToolId::VideoJumpLatestEnd)summary["mode"]=state["kind"]=="recording"?"latest_live":"file_end";
        }
        summary["recordingActive"]=target.RecordingActive();
        target.operations_.Advance(operation,OperationState::Completed,summary);
    });
    timer->start();
    QJsonObject receipt{{"ok",true},{"pending",true},{"operationId",operation},{"state","starting"},
        {"documentId",summary["documentId"]},{"selectionGeneration",tool==ToolId::VideoSaveFrame?summary["selectionGeneration"]:expectedSelection},
        {"requestGeneration",expectedRequest},{"selectedPts100ns",summary["selectedPts100ns"]}};
    if(tool==ToolId::VideoSeek)receipt["requestedPositionMs"]=args["positionMs"];
    completion({receipt,{}});
}
} // namespace okuflow::agent
#endif
