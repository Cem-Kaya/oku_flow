#ifdef _WIN32
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/recording_parameter_policy.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/agent/control_schema.hpp"
#include "okuflow/app/ui_state_manager.hpp"
#include "../app_internal.hpp"
#include <chrono>

namespace okuflow::agent {
namespace {
QString MicHandle(std::map<QString,QString>& handles,const QString& endpoint) {
    if(endpoint.isEmpty() || endpoint=="__none__")return "none";
    if(!handles.contains(endpoint) && handles.size()>=128)return {};
    auto [it,inserted]=handles.try_emplace(endpoint);
    if(inserted)it->second="mic-"+QUuid::createUuid().toString(QUuid::Id128);
    return it->second;
}
QString Canvas(RecordingCanvasMode mode) {
    static const QStringList values{"source","1080p","1440p","2160p","720p","480p","360p"};
    return values.value(static_cast<int>(mode));
}
qint64 Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

std::map<Field,QJsonValue> AppControlTarget::RecordingParameterFields() const {
    if(!app_.settingsController_)return {};
    const auto& settings=app_.settingsController_->Settings();
    return {{Field::RecordingMicrophone,MicHandle(microphoneHandles_,settings.microphoneEndpointId)},
            {Field::RecordingCanvas,Canvas(settings.recordingCanvasMode)}};
}

ControlPlan AppControlTarget::PrepareRecordingParameters(ToolId tool,const QJsonObject& args,const QString& owner) const {
    Q_UNUSED(owner);
    ControlPlan plan;
    const auto fields=RecordingParameterFields();
    if(tool==ToolId::RecordingSettingsGet) {
        const auto schema=QJsonDocument::fromJson(QByteArray::fromStdString(ParameterGetSchema(ParameterGroup::Recording))).object();
        if(!MatchesControlSchema(args,schema)) {plan.error="invalid_argument";return plan;}
        const auto filter=args["fields"].toArray();
        for(const auto& [id,field]:{std::pair{"recording.microphone",Field::RecordingMicrophone},
                                  std::pair{"recording.canvas",Field::RecordingCanvas}})
            if((filter.isEmpty() || filter.contains(QString::fromLatin1(id))) && fields.contains(field))plan.observedFields.push_back(field);
        return plan;
    }
    if(tool!=ToolId::RecordingSettingsSet) {plan.error="invalid_argument";return plan;}
    QSet<QString> choices;
    for(const auto& mic:app_.microphones_) {
        const auto handle=MicHandle(microphoneHandles_,QString::fromStdWString(mic.endpointId));
        if(!handle.isEmpty())choices.insert(handle);
    }
    RecordingPreferenceSession session;
    if(app_.recordingManager_ && app_.recordingManager_->IsActive()) {
        const auto live=app_.recordingManager_->CurrentSessionInfo();
        if(!live) {plan.error="busy";return plan;}
        const auto state=app_.recordingManager_->State();
        const auto started=QDateTime::fromString(live->timestampToken,QStringLiteral("yyyyMMdd_HHmmss_zzz"));
        session={live->id,started.isValid()?started.time().toString(QStringLiteral("HH:mm")):QString(),
                 state==RecordingState::Starting || state==RecordingState::Recording};
    }
    plan=PrepareRecordingPreferencePatch(fields,choices,args,session,
        !app_.recordingManager_ || app_.recordingManager_->IsWorkerAbandoned());
    if(!plan.error.isEmpty())return plan;
    for(const auto& write:plan.writes) {
        auto* combo=write.field==Field::RecordingMicrophone?app_.uiState_->microphoneCombo_:app_.uiState_->recordingCanvasCombo_;
        if(!combo) {plan=ControlPlan{};plan.error="unavailable";return plan;}
        QVariant nativeValue;
        if(write.field==Field::RecordingMicrophone) {
            QString endpoint="__none__";
            if(write.value!="none") {
                const auto found=std::find_if(microphoneHandles_.begin(),microphoneHandles_.end(),
                    [&](const auto& item){return item.second==write.value.toString();});
                if(found==microphoneHandles_.end()) {plan=ControlPlan{};plan.error="not_found";return plan;}
                endpoint=found->first;
            }
            nativeValue=endpoint;
        } else {
            static const QStringList modes{"source","1080p","1440p","2160p","720p","480p","360p"};
            const int mode=modes.indexOf(write.value.toString());
            if(mode<0) {plan=ControlPlan{};plan.error="invalid_argument";return plan;}
            nativeValue=mode;
        }
        // Admission must prove native availability before any irreversible stop.
        const int nativeIndex=combo->findData(nativeValue);
        if(nativeIndex<0) {plan=ControlPlan{};plan.error="unavailable";return plan;}
        if(write.field==Field::RecordingMicrophone) plan.targetDisplay=combo->itemText(nativeIndex).left(256);
    }
    return plan;
}

QJsonObject AppControlTarget::QueryRecordingParameters(ToolId tool,const QJsonObject& args) const {
    if(tool!=ToolId::RecordingSettingsGet || !PrepareRecordingParameters(tool,args,{}).error.isEmpty())
        return ControlResult::Failure("invalid_argument").content;
    const auto fields=RecordingParameterFields();
    const auto filter=args["fields"].toArray();
    const auto& committed=app_.settingsController_->LastCommittedSettings();
    QJsonObject parameters;
    QSet<QString> installed;
    QJsonArray microphones{QJsonObject{{"value","none"},{"label","none"}}};int omitted=0;
    for(const auto& mic:app_.microphones_) {
        const auto handle=MicHandle(microphoneHandles_,QString::fromStdWString(mic.endpointId));
        if(handle.isEmpty()) {++omitted;continue;}
        installed.insert(handle);
        if(microphones.size()<32)microphones.append(QJsonObject{{"value",handle},
            {"label",QStringLiteral("Microphone %1").arg(microphones.size())}});
        else ++omitted;
    }
    for(const auto& [id,field]:{std::pair{"recording.microphone",Field::RecordingMicrophone},
                              std::pair{"recording.canvas",Field::RecordingCanvas}}) {
        const auto name=QString::fromLatin1(id);
        if(!filter.isEmpty() && !filter.contains(name))continue;
        const auto found=fields.find(field);
        const bool available=found!=fields.end() && app_.recordingManager_ && !app_.recordingManager_->IsWorkerAbandoned();
        const auto value=found!=fields.end()?found->second:QJsonValue(QJsonValue::Null);
        const bool missingMic=field==Field::RecordingMicrophone && value!="none" && !installed.contains(value.toString());
        const auto saved=!committed?QJsonValue(QJsonValue::Null):field==Field::RecordingCanvas?
            QJsonValue(Canvas(committed->recordingCanvasMode)):QJsonValue(MicHandle(microphoneHandles_,committed->microphoneEndpointId));
        QJsonObject row{{"requested",value},{"effective",available && !missingMic?value:QJsonValue(QJsonValue::Null)},
            {"saved",saved},{"available",available && !missingMic},{"writable",available},
            {"unsupportedReason",!available?"recorder_unavailable":missingMic?"microphone_unavailable":""},
            {"persistence","shared_saved"},{"requiresUi",field==Field::RecordingMicrophone},{"appliesTo","next_recording"}};
        if(field==Field::RecordingMicrophone) {row["choices"]=microphones;if(omitted)row["omittedChoices"]=omitted;}
        if(RecordingActive()) {
            row["effective"]=QJsonValue(QJsonValue::Null);row["inactiveReason"]="active_session_configuration_retained";
            row["requiresRecordingGrantForChange"]=field==Field::RecordingMicrophone;
        }
        parameters.insert(name,row);
    }
    return {{"ok",true},{"parameters",parameters}};
}

std::optional<bool> AppControlTarget::ApplyRecordingParameter(const FieldWrite& write) {
    if(write.field!=Field::RecordingMicrophone && write.field!=Field::RecordingCanvas)return {};
    if(write.field==Field::RecordingMicrophone) {
        if(RecordingActive())return false;
        QString endpoint="__none__";
        if(write.value!="none") {
            const auto found=std::find_if(microphoneHandles_.begin(),microphoneHandles_.end(),[&](const auto& item){return item.second==write.value.toString();});
            if(found==microphoneHandles_.end())return false;endpoint=found->first;
        }
        auto* combo=app_.uiState_->microphoneCombo_;const int index=combo?combo->findData(endpoint):-1;
        if(index<0)return false;
        {const QSignalBlocker block(combo);combo->setCurrentIndex(index);}
        app_.OnMicrophoneSelectionChanged(index);return true;
    }
    static const QStringList modes{"source","1080p","1440p","2160p","720p","480p","360p"};
    const int mode=modes.indexOf(write.value.toString());
    auto* combo=app_.uiState_->recordingCanvasCombo_;const int index=combo?combo->findData(mode):-1;
    if(mode<0 || index<0)return false;
    {const QSignalBlocker block(combo);combo->setCurrentIndex(index);}
    app_.SetRecordingCanvasMode(static_cast<RecordingCanvasMode>(mode));return true;
}

void AppControlTarget::ExecuteRecordingParameters(ToolId tool,const QJsonObject& args,const QString& owner,
    ContextKind kind,ControlCompletion completion) {
    Q_UNUSED(kind);
    const auto plan=PrepareRecordingParameters(tool,args,owner);
    if(!plan.error.isEmpty()) {completion(ControlResult::Failure(plan.error));return;}
    if(!plan.asynchronous || !plan.requiresRecording || plan.targetIdentity.isEmpty()) {
        completion(ControlResult::Failure("invalid_argument"));return;
    }
    auto& service=*app_.controlService_;
    const auto context=service.ContextStatus();const auto id=context["contextId"].toString();
    if(!service.IsContextActive(id) || !context["grants"].toObject()["recording"].toBool()) {
        completion(ControlResult::Failure("canceled"));return;
    }
    const RecordingPreferenceFence fence{service.Stamp().sourceEpoch,service.FieldRevision(Field::RecordingMicrophone),
        service.FieldRevision(Field::RecordingCanvas),plan.targetIdentity};
    const auto baselineFields=RecordingParameterFields();
    const auto operation=operations_.Create(OperationKind::Recording,owner,plan.targetIdentity);
    if(operation.isEmpty()) {completion(ControlResult::Failure("budget_exhausted"));return;}
    if(!app_.StopRecording(plan.targetIdentity)) {
        operations_.Advance(operation,OperationState::Failed,{{"errorCode","stale_state"}});
        completion(ControlResult::Failure("stale_state"));return;
    }
    operations_.Advance(operation,OperationState::Stopping,{{"recordingPreferences",true}});
    auto* timer=new QTimer(&observations_);timer->setInterval(25);
    const qint64 deadline=Now()+30000;
    QPointer<OkuFlowApp> app(&app_);
    QObject::connect(timer,&QTimer::timeout,timer,[app,timer,id,operation,tool,args,plan,fence,deadline,baselineFields] {
        if(!app || !app->controlTarget_ || !app->controlService_) {timer->stop();timer->deleteLater();return;}
        auto& target=*app->controlTarget_;auto& service=*app->controlService_;
        const auto active=service.IsContextActive(id);
        const auto grants=service.ContextStatus()["grants"].toObject();
        const auto live=app->recordingManager_->CurrentSessionInfo();
        const auto status=RecordingPreferenceFenceStatus(fence,active,grants["recording"].toBool(),service.Stamp().sourceEpoch,
            service.FieldRevision(Field::RecordingMicrophone),service.FieldRevision(Field::RecordingCanvas),
            live?live->id:QString(),app->recordingManager_->IsActive(),app->recordingManager_->IsWorkerAbandoned(),Now()>=deadline);
        if(status=="waiting")return;
        timer->stop();timer->deleteLater();
        if(status!="ready") {target.operations_.Advance(operation,OperationState::Failed,{{"errorCode",status}});return;}
        if(target.RecordingParameterFields()!=baselineFields) {
            target.operations_.Advance(operation,OperationState::Failed,{{"errorCode","stale_state"}});return;
        }
        auto revalidated=target.PrepareRecordingParameters(tool,args,{});
        if(!revalidated.error.isEmpty() || revalidated.writes.size()!=plan.writes.size()) {
            target.operations_.Advance(operation,OperationState::Failed,{{"errorCode","stale_state"}});return;
        }
        for(std::size_t i=0;i<plan.writes.size();++i)if(revalidated.writes[i].field!=plan.writes[i].field ||
            revalidated.writes[i].value!=plan.writes[i].value) {
            target.operations_.Advance(operation,OperationState::Failed,{{"errorCode","stale_state"}});return;
        }
        const auto result=service.ApplyDeferredFields(id,tool,args,plan);
        target.operations_.Advance(operation,result.content["ok"].toBool()?OperationState::Completed:OperationState::Failed,
            {{"recordingPreferences",true},{"result",result.content}});
    });
    timer->start();
    completion({{{"ok",true},{"pending",true},{"operationId",operation},{"state","stopping"},
        {"recordingStopRequested",true},{"preferencesPending",true}},{}});
}

QJsonObject AppControlTarget::RecordingParameterOperationStatus(const QString& operationId) const {
    const auto record=operations_.Get(operationId);
    if(!record || !record->summary["recordingPreferences"].toBool())return {};
    auto status=operations_.SafeStatus(operationId);
    status["recordingStopRequested"]=true;
    status["preferencesPending"]=record->state!=OperationState::Completed && record->state!=OperationState::Failed;
    const auto result=record->summary["result"].toObject();
    for(const auto* property:{"applied","effective"}) {
        QJsonObject safe;
        const auto fields=result[property].toObject();
        for(const auto* key:{"recording.microphone","recording.canvas"}) {
            if(!fields.contains(key))continue;
            const auto* descriptor=FindParameter(key);
            const auto schema=QJsonDocument::fromJson(QByteArray::fromStdString(std::string(descriptor->schema))).object();
            if(MatchesControlSchema(fields[key],schema))safe[key]=fields[key];
        }
        if(!safe.isEmpty())status[property]=safe;
    }
    const auto error=result["error"].toObject()["code"].toString(record->summary["errorCode"].toString());
    const QStringList known{"canceled","stale_state","timeout","unavailable","internal","consent_denied","modal_active","user_interacting"};
    if(known.contains(error))status["errorCode"]=error;
    return status;
}
} // namespace okuflow::agent
#endif
