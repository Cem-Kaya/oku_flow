#ifdef _WIN32
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "../app_internal.hpp"
#include <QComboBox>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QUuid>
#include <algorithm>
#include <chrono>

namespace okuflow::agent {
namespace {
qint64 SteadyNow() {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
QString Handle(std::map<QString,QString>& handles, const QString& identity, const QString& prefix) {
    if (identity.isEmpty()) return {};
    if (!handles.contains(identity) && handles.size() >= 128) return {};
    auto [entry,inserted] = handles.try_emplace(identity);
    if (inserted) entry->second=prefix+QUuid::createUuid().toString(QUuid::Id128);
    return entry->second;
}
QString Canvas(RecordingCanvasMode mode) {
    static const QStringList values{"source","1080p","1440p","2160p","720p","480p","360p"};
    const auto index=static_cast<int>(mode); return index>=0 && index<values.size()?values[index]:QString();
}
std::optional<Field> CaptureField(QString id) {
    if(id=="camera.orientation")return Field::Rotation;
    if(id=="camera.selection")return Field::Camera;
    if(id=="camera.format")return Field::CameraFormat;
    if(id=="camera.acceleration")return Field::CameraAcceleration;
    if(id=="camera.phoneTorch")return Field::CameraTorch;
    if(id=="notes_output.root")return Field::NotesRoot;
    return {};
}
}
void AppControlTarget::RememberOutputRoot(const QString& path) {
    if(path.isEmpty() || approvedOutputRoots_.contains(path))return;
    approvedOutputRoots_.append(path);
    while(approvedOutputRoots_.size()>8)approvedOutputRoots_.removeFirst();
}
void AppControlTarget::NoteTorchRequest(bool enabled) {
    if(!applying_) {
        if(torchCanceled_)torchCanceled_->store(true);
        torchOwner_.clear();
    }
    torchRequested_=enabled;torchSourceEpoch_=app_.controlService_->Stamp().sourceEpoch;
    if(!applying_)app_.controlService_->NativeFieldsChanged({Field::CameraTorch});
    observed_=Fields();
}
std::map<Field,QJsonValue> AppControlTarget::CaptureParameterFields() const {
    std::map<Field,QJsonValue> fields;
    const auto& settings=app_.settingsController_->Settings();
    QString format=app_.cameraIntent_.Requested().identity.formatStableId;
    const auto link=app_.cameraIntent_.Requested().identity.symbolicLink;
    fields[Field::CameraFormat]=format.isEmpty()?QString("automatic"):Handle(formatHandles_,link+'\n'+format,"fmt-");
    const auto acceleration=settings.cameraAcceleration.value(link).mode;
    static const QStringList modes{"automatic","accelerated","compatibility"};
    fields[Field::CameraAcceleration]=modes.value(static_cast<int>(acceleration));
    fields[Field::CameraTorch]=torchRequested_ && torchSourceEpoch_==app_.controlService_->Stamp().sourceEpoch
        ? *torchRequested_ : app_.cameraActive_ && app_.mediaCapture_.PhoneTorchEnabled();
    const auto root=app_.userDataPaths_->ConfiguredRoot().isEmpty()?UserDataPaths::DefaultRoot():app_.userDataPaths_->ConfiguredRoot();
    fields[Field::NotesRoot]=Handle(directoryHandles_,root,"dir-");
    return fields;
}
ControlPlan AppControlTarget::PrepareCaptureParameters(ToolId tool,const QJsonObject& args,const QString& owner) const {
    ControlPlan plan; ParameterGroup group;bool setter{};
    if(!ParameterGroupForTool(tool,group,setter)){plan.error="invalid_argument";return plan;}
    const auto fields=Fields();
    if(!setter) {
        const auto filter=args["fields"].toArray();
        for(const auto& descriptor:ParameterRegistry()) {
            const auto id=QString::fromUtf8(descriptor.id.data(),descriptor.id.size());
            if(descriptor.group!=group || (!filter.isEmpty() && !filter.contains(id)))continue;
            if(const auto field=CaptureField(id))plan.observedFields.push_back(*field);
        }
        return plan;
    }
    for(auto it=args.begin();it!=args.end();++it) {
        const auto field=CaptureField(it.key());
        if(!field){plan.error="invalid_argument";return plan;}
        const bool retryTorch=*field==Field::CameraTorch && app_.cameraActive_
            && app_.mediaCapture_.PhoneTorchAvailable() && app_.mediaCapture_.PhoneTorchEnabled()!=it.value().toBool();
        if(fields.at(*field)==it.value() && !retryTorch)continue;
        if(*field==Field::NotesRoot) {
            if(app_.vlmAssistEnabled_){plan.error="denied_by_policy";return plan;}
            const auto path=std::find_if(directoryHandles_.begin(),directoryHandles_.end(),[&](const auto& entry){return entry.second==it.value().toString();});
            if(path==directoryHandles_.end() || !UserDataPaths::ValidateRoot(path->first,{},false).ok){plan.error="not_found";return plan;}
            if(app_.photoCapturePending_ || app_.pendingPhotoReadbackId_ || !app_.photoOperationId_.isEmpty()){plan.error="busy";return plan;}
            plan.requiresApproval=true;plan.approvalDomain=ApprovalDomain::Configuration;
            plan.targetIdentity=it.value().toString();
            plan.targetDisplay=QFileInfo(path->first).fileName().left(80);
        } else if(*field==Field::Camera) {
            const auto source=Prepare(ToolId::CameraSelect,{{"cameraHandle",it.value()}},owner);
            if(!source.error.isEmpty())return source;
            plan.targetIdentity=source.targetIdentity;plan.targetDisplay=source.targetDisplay;plan.asynchronous=true;
        } else if(*field==Field::CameraFormat) {
            if(it.value()!="automatic") {
                if(args.contains("camera.selection") && args["camera.selection"]!=fields.at(Field::Camera)) {plan.error="invalid_argument";return plan;}
                const auto found=std::find_if(formatHandles_.begin(),formatHandles_.end(),[&](const auto& entry){return entry.second==it.value().toString();});
                const auto link=app_.cameraIntent_.Requested().identity.symbolicLink;
                if(found==formatHandles_.end() || !found->first.startsWith(link+'\n') || !app_.uiState_->cameraFormatCombo_
                    || app_.uiState_->cameraFormatCombo_->findData(found->first.mid(link.size()+1))<0){plan.error="not_found";return plan;}
            }
            plan.asynchronous=true;
        } else if(*field==Field::CameraAcceleration) {
            if(args.contains("camera.selection") && args["camera.selection"]!=fields.at(Field::Camera)){plan.error="invalid_argument";return plan;}
            const auto link=app_.cameraIntent_.Requested().identity.symbolicLink;
            const auto camera=std::find_if(app_.cameras_.begin(),app_.cameras_.end(),[&](const auto& item){return QString::fromStdWString(item.symbolicLink)==link;});
            if(camera==app_.cameras_.end() || !camera->phoneSerial.empty()){plan.error="unavailable";return plan;}
            const auto setting=app_.settingsController_->Settings().cameraAcceleration.value(link);
            if(it.value()=="accelerated" && setting.lastRung!="zeroCopy" && setting.lastRung!="acceleratedCopy") {plan.error="unavailable";return plan;}
            plan.asynchronous=true;
        } else if(*field==Field::CameraTorch) {
            if(!app_.cameraActive_ || !app_.mediaCapture_.PhoneTorchAvailable()){plan.error="unavailable";return plan;}
            plan.asynchronous=true;
        } else if(*field==Field::Rotation) {
            const auto rotation=Prepare(ToolId::Orientation,{{"degreesClockwise",it.value()}},owner);
            plan.writes.insert(plan.writes.end(),rotation.writes.begin(),rotation.writes.end());
            plan.resetsHistory=rotation.resetsHistory;continue;
        }
        const auto binding = *field==Field::CameraTorch && app_.cameraIntent_.Active()
            ? CameraHandle(app_.cameraIntent_.Active()->symbolicLink)
            : *field==Field::CameraAcceleration ? fields.at(Field::Camera).toString() : QString();
        plan.writes.push_back({*field,it.value(),binding,retryTorch});
    }
    if(group==ParameterGroup::Camera && !plan.writes.empty()) {
        if(RecordingActive() || app_.photoCapturePending_ || app_.pendingPhotoReadbackId_ || !app_.photoOperationId_.isEmpty()
            || app_.cameraStartupPending_ || app_.cameraStopPending_ || app_.cameraStopFailed_) {plan.error="busy";plan.writes.clear();return plan;}
        const bool restart=std::any_of(plan.writes.begin(),plan.writes.end(),[](const FieldWrite& write){return write.field==Field::Camera || write.field==Field::CameraFormat || write.field==Field::CameraAcceleration;});
        if((restart && std::any_of(plan.writes.begin(),plan.writes.end(),[](const FieldWrite& write){return write.field==Field::Rotation || write.field==Field::CameraTorch;}))
            || (plan.asynchronous && !restart && plan.writes.size()!=1)) {
            plan.error="invalid_argument";plan.writes.clear();return plan;
        }
        if(plan.targetIdentity.isEmpty())plan.targetIdentity=fields.at(Field::Camera).toString();
        if(restart) {
            const auto source=Prepare(ToolId::CameraSelect,{{"cameraHandle",plan.targetIdentity}},owner);
            if(!source.error.isEmpty()){plan.error=source.error;plan.writes.clear();return plan;}
            plan.targetDisplay=source.targetDisplay;
        }
    }
    return plan;
}
QJsonObject AppControlTarget::QueryCaptureParameters(ToolId tool,const QJsonObject& args) const {
    ParameterGroup group;bool setter{};ParameterGroupForTool(tool,group,setter);
    const auto fields=Fields();const auto filter=args["fields"].toArray();QJsonObject parameters;
    const auto& committed=app_.settingsController_->LastCommittedSettings();
    for(const auto& descriptor:ParameterRegistry()) {
        const auto id=QString::fromUtf8(descriptor.id.data(),descriptor.id.size());
        const auto field=CaptureField(id);
        if(!field || descriptor.group!=group || (!filter.isEmpty() && !filter.contains(id)))continue;
        QJsonValue saved;
        QJsonValue effective=fields.at(*field);
        bool available=true;
        QString reason;
        if(committed) {
            if(*field==Field::RecordingCanvas)saved=Canvas(committed->recordingCanvasMode);
            if(*field==Field::RecordingMicrophone)saved=committed->microphoneEndpointId.isEmpty() || committed->microphoneEndpointId=="__none__"?QString("none"):Handle(microphoneHandles_,committed->microphoneEndpointId,"mic-");
            if(*field==Field::NotesRoot)saved=Handle(directoryHandles_,committed->userDataRoot.isEmpty()?UserDataPaths::DefaultRoot():committed->userDataRoot,"dir-");
            if(*field==Field::Rotation)saved=committed->rotationQuarterTurns*90;
            if(*field==Field::Camera)saved=CameraHandle(committed->cameraSymbolicLink);
            if(*field==Field::CameraFormat)saved=committed->cameraFormatStableId.isEmpty()?QString("automatic"):Handle(formatHandles_,committed->cameraSymbolicLink+'\n'+committed->cameraFormatStableId,"fmt-");
            if(*field==Field::CameraAcceleration) {
                static const QStringList modes{"automatic","accelerated","compatibility"};
                saved=modes.value(static_cast<int>(committed->cameraAcceleration.value(app_.cameraIntent_.Requested().identity.symbolicLink).mode));
            }
        }
        QJsonArray choices;
        if(*field==Field::Camera) {
            const auto sources=Query(ToolId::CameraSources,{})["sources"].toArray();
            for(const auto& value:sources) {const auto source=value.toObject();choices.append(QJsonObject{{"value",source["cameraHandle"]},{"label",source["displayName"]},{"available",source["agentSelectable"]}});}
            effective=app_.cameraIntent_.Active()?QJsonValue(CameraHandle(app_.cameraIntent_.Active()->symbolicLink)):QJsonValue();
            available=!app_.cameras_.empty() && !app_.cameraStopFailed_;if(!available)reason="source_unavailable";
        }
        if(*field==Field::CameraFormat) {
            choices.append(QJsonObject{{"value","automatic"},{"label","automatic"}});
            const auto link=app_.cameraIntent_.Requested().identity.symbolicLink;
            if(app_.cameraActive_ && app_.cameraIntent_.Active() && app_.cameraIntent_.Active()->symbolicLink==link && app_.uiState_->cameraFormatCombo_) {
                auto* combo=app_.uiState_->cameraFormatCombo_;
                for(int index=0;index<combo->count() && choices.size()<32;++index) {
                    const auto id=combo->itemData(index).toString();if(id.isEmpty())continue;
                    choices.append(QJsonObject{{"value",Handle(formatHandles_,link+'\n'+id,"fmt-")},{"label",combo->itemText(index).left(80)}});
                }
                const auto id=QString::fromStdWString(app_.mediaCapture_.NegotiatedFormat().stableId);
                effective=id.isEmpty()?QJsonValue():QJsonValue(Handle(formatHandles_,link+'\n'+id,"fmt-"));
            } else {effective=QJsonValue();available=false;reason="source_not_ready";}
        }
        if(*field==Field::CameraAcceleration) {
            const auto link=app_.cameraIntent_.Requested().identity.symbolicLink;
            const auto camera=std::find_if(app_.cameras_.begin(),app_.cameras_.end(),[&](const auto& item){return QString::fromStdWString(item.symbolicLink)==link;});
            available=camera!=app_.cameras_.end() && camera->phoneSerial.empty();
            if(!available)reason="source_not_applicable";
            effective=available && app_.cameraActive_ && app_.cameraIntent_.Active() && app_.cameraIntent_.Active()->symbolicLink==link
                ? QJsonValue(app_.mediaCapture_.AccelerationMode()==CaptureAccelerationMode::Accelerated?"accelerated":"compatibility"):QJsonValue();
            const auto setting=app_.settingsController_->Settings().cameraAcceleration.value(link);
            choices.append(QJsonObject{{"value","automatic"},{"available",available}});
            choices.append(QJsonObject{{"value","compatibility"},{"available",available}});
            choices.append(QJsonObject{{"value","accelerated"},{"available",available && (setting.lastRung=="zeroCopy" || setting.lastRung=="acceleratedCopy")}});
        }
        if(*field==Field::CameraTorch) {
            available=app_.cameraActive_ && app_.mediaCapture_.PhoneTorchAvailable();
            effective=available?QJsonValue(app_.mediaCapture_.PhoneTorchEnabled()):QJsonValue();
            if(!available){reason="torch_unavailable";effective=QJsonValue();}
        }
        if(*field==Field::RecordingMicrophone) {
            choices.append(QJsonObject{{"value","none"},{"label","none"}});
            for(const auto& mic:app_.microphones_)if(choices.size()<32)
                choices.append(QJsonObject{{"value",Handle(microphoneHandles_,QString::fromStdWString(mic.endpointId),"mic-")},{"label",QString::fromStdWString(mic.name).left(80)}});
        }
        if(*field==Field::NotesRoot) {
            const auto add=[&](const QString& path,const QString& label){choices.append(QJsonObject{{"value",Handle(directoryHandles_,path,"dir-")},{"label",label}});};
            add(UserDataPaths::DefaultRoot(),"documents-default");
            const auto root=app_.userDataPaths_->ConfiguredRoot();if(!root.isEmpty() && root!=UserDataPaths::DefaultRoot())add(root,"current-output");
            int index{};for(const auto& path:approvedOutputRoots_)if(path!=root && path!=UserDataPaths::DefaultRoot())add(path,QString("remembered-output-%1").arg(++index));
        }
        parameters[id]=QJsonObject{{"requested",fields.at(*field)},{"effective",effective},{"saved",saved},
            {"savedKnown",committed.has_value() && !saved.isNull()},{"available",available},{"writable",available},
            {"unsupportedReason",reason},
            {"choices",choices},{"persistence",QString::fromUtf8(descriptor.persistence.data(),descriptor.persistence.size())}};
    }
    return {{"parameters",parameters}};
}
std::optional<bool> AppControlTarget::ApplyCaptureParameter(const FieldWrite& write) {
    const QScopedValueRollback<bool> applying(applying_,true);
    if(write.field==Field::CameraTorch) {
        if(!app_.cameraActive_ || !app_.mediaCapture_.PhoneTorchAvailable())return false;
        if(!app_.cameraIntent_.Active() || (!write.targetIdentity.isEmpty() && write.targetIdentity!=CameraHandle(app_.cameraIntent_.Active()->symbolicLink)))return false;
        if(!torchCanceled_ || torchCanceled_->load()) {torchCanceled_=std::make_shared<std::atomic<bool>>(false);torchDeadline_=SteadyNow()+3000;}
        return app_.SetPhoneTorch(write.value.toBool(),torchCanceled_,torchDeadline_);
    }
    if(write.field==Field::CameraAcceleration) {
        const auto token=write.targetIdentity.isEmpty()?CameraHandle(app_.cameraIntent_.Requested().identity.symbolicLink):write.targetIdentity;
        const auto camera=std::find_if(cameraHandles_.begin(),cameraHandles_.end(),[&](const auto& entry){return entry.second==token;});
        if(camera==cameraHandles_.end())return false;
        const auto index=QStringList{"automatic","accelerated","compatibility"}.indexOf(write.value.toString());if(index<0)return false;
        auto& preference=app_.settingsController_->MutableSettings().cameraAcceleration[camera->first];
        preference.mode=static_cast<settings::CameraAccelerationMode>(index);preference.automaticFallback=false;preference.reason.clear();
        if(app_.cameraIntent_.Requested().identity.symbolicLink==camera->first && app_.uiState_->cameraAccelerationCombo_) {
            const QSignalBlocker blocker(app_.uiState_->cameraAccelerationCombo_);app_.uiState_->cameraAccelerationCombo_->setCurrentIndex(index);
        }
        return true;
    }
    if(write.field==Field::NotesRoot) {
        const auto found=std::find_if(directoryHandles_.begin(),directoryHandles_.end(),[&](const auto& entry){return entry.second==write.value.toString();});
        if(found==directoryHandles_.end())return false;
        if(!app_.SetUserDataRoot(found->first))return false;
        outputRootPending_=true;return true;
    }
    return {};
}
void AppControlTarget::ExecuteCaptureParameters(const QJsonObject& args,const QString& owner,ContextKind kind,ControlCompletion completion) {
    auto plan=PrepareCaptureParameters(ToolId::CameraSettingsSet,args,owner);
    if(!plan.error.isEmpty()){completion(ControlResult::Failure(plan.error));return;}
    const auto context=app_.controlService_->ContextStatus()["contextId"].toString();
    if(!app_.controlService_->IsContextActive(context)){completion(ControlResult::Failure("canceled"));return;}
    if(plan.writes.size()==1 && plan.writes.front().field==Field::CameraTorch) {
        if(torchCanceled_)torchCanceled_->store(true);
        torchCanceled_=std::make_shared<std::atomic<bool>>(false);torchOwner_=owner;torchDeadline_=SteadyNow()+3000;
        const auto lease=app_.controlService_->ContextStatus();
        if(kind==ContextKind::ExternalLease)torchDeadline_=std::min(torchDeadline_,SteadyNow()+lease["remainingAbsoluteMs"].toInteger());
        const auto operation=operations_.Create(OperationKind::Adjustment,owner);
        if(operation.isEmpty()){completion(ControlResult::Failure("budget_exhausted"));return;}
        torchOperation_=operation;
        auto applied=app_.controlService_->ApplyDeferredFields(context,ToolId::CameraSettingsSet,args,plan);
        if(!applied.content["ok"].toBool()){operations_.Advance(operation,OperationState::Failed);completion(applied);return;}
        operations_.Advance(operation,OperationState::Starting);
        auto effective=applied.content["effective"].toObject();effective["camera.phoneTorch"]=app_.mediaCapture_.PhoneTorchEnabled();applied.content["effective"]=effective;
        const auto desired=plan.writes.front().value.toBool();const auto source=torchSourceEpoch_;
        const auto sequence=app_.currentCameraFrameSequence_;const auto token=torchCanceled_;const auto deadline=torchDeadline_;
        auto* timer=new QTimer(&app_);timer->setInterval(25);
        QPointer<OkuFlowApp> app(&app_);
        QObject::connect(timer,&QTimer::timeout,&app_,[this,app,timer,operation,desired,source,sequence,token,deadline] {
            if(!app || app->controlTarget_.get()!=this){token->store(true);timer->deleteLater();return;}
            const bool acknowledged=app_.mediaCapture_.PhoneTorchEnabled()==desired;
            if(acknowledged && app_.currentCameraFrameSequence_!=sequence && app_.controlService_->Stamp().sourceEpoch==source) {
                operations_.Advance(operation,OperationState::FirstFrame,{{"deviceAcknowledged",true}});timer->deleteLater();return;
            }
            if(token->load() || SteadyNow()>=deadline || app_.controlService_->Stamp().sourceEpoch!=source) {
                token->store(true);operations_.Advance(operation,OperationState::Failed,{{"deviceAcknowledged",acknowledged},{"errorCode","capture_failed"}});timer->deleteLater();
            }
        });timer->start();
        applied.content["pending"]=true;applied.content["operationId"]=operation;applied.content["state"]="starting";
        completion(std::move(applied));return;
    }
    QString format=app_.cameraIntent_.Requested().identity.formatStableId;
    if(args.contains("camera.selection") && args["camera.selection"]!=Fields().at(Field::Camera))format.clear();
    if(args.contains("camera.format")) {
        format.clear();
        if(args["camera.format"]!="automatic") {
            const auto found=std::find_if(formatHandles_.begin(),formatHandles_.end(),[&](const auto& entry){return entry.second==args["camera.format"].toString();});
            if(found==formatHandles_.end()){completion(ControlResult::Failure("not_found"));return;}
            format=found->first.mid(found->first.indexOf('\n')+1);
        }
    }
    auto synchronous=plan;synchronous.asynchronous=false;
    std::erase_if(synchronous.writes,[](const FieldWrite& write){return write.field==Field::Camera || write.field==Field::CameraFormat;});
    std::map<Field,QJsonValue> previous;
    for(const auto& write:synchronous.writes)previous[write.field]=ReadField(write);
    auto applied=app_.controlService_->ApplyDeferredFields(context,ToolId::CameraSettingsSet,args,synchronous);
    if(!applied.content["ok"].toBool()){completion(applied);return;}
    std::map<Field,quint64> committedRevisions;
    for(const auto& write:synchronous.writes)committedRevisions[write.field]=app_.controlService_->FieldRevision(write.field);
    Execute(ToolId::CameraSelect,{{"cameraHandle",plan.targetIdentity},{"_format",format},{"_reopen",true},{"_preferences",applied.content["applied"]}},owner,kind,
        [this,context,args,synchronous,previous,committedRevisions,completion=std::move(completion),applied](ControlResult result) mutable {
            if(!result.content["ok"].toBool() && !synchronous.writes.empty()) {
                NoticeNativeChanges();ControlPlan rollback;
                bool current=app_.controlService_->IsContextActive(context);
                for(const auto& write:synchronous.writes) {
                    current &= app_.controlService_->FieldRevision(write.field)==committedRevisions.at(write.field) && ReadField(write)==write.value;
                    rollback.writes.push_back({write.field,previous.at(write.field),write.targetIdentity});
                }
                const auto restored=current?app_.controlService_->ApplyDeferredFields(context,ToolId::CameraSettingsSet,args,rollback):ControlResult::Failure("stale_state");
                result.content["rolledBack"]=restored.content["ok"].toBool();
                if(result.content["rolledBack"].toBool()) {applied.content["applied"]=QJsonObject{};applied.content["effective"]=restored.content["effective"];}
            }
            for(const auto key:{"applied","effective","stateVersion"})if(applied.content.contains(key))result.content[key]=applied.content[key];
            completion(std::move(result));
        });
}
} // namespace okuflow::agent
#endif
