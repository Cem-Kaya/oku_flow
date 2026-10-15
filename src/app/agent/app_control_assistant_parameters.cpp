#ifdef _WIN32
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/app.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/agent/assistant_parameter_policy.hpp"
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalBlocker>
#include <QTextToSpeech>
#include <QTimer>
#include <QUuid>
#include <QVoice>
#include <cmath>

namespace okuflow::agent {
namespace {
constexpr std::pair<std::string_view,Field> kFields[]{
    {"assistant_preferences.sceneExplain",Field::AssistantSceneExplain},
    {"assistant_preferences.overlayEnabled",Field::AssistantOverlay},
    {"assistant_preferences.provider",Field::AssistantProvider},
    {"assistant_preferences.codexModel",Field::AssistantCodexModel},
    {"assistant_preferences.codexReasoning",Field::AssistantReasoning},
    {"assistant_preferences.httpModel",Field::AssistantHttpModel},
    {"assistant_preferences.instructions",Field::AssistantInstructions},
    {"assistant_preferences.explainPrompt",Field::AssistantExplainPrompt},
    {"assistant_preferences.ttsVoice",Field::AssistantVoice},
    {"assistant_preferences.ttsRate",Field::AssistantTtsRate},
    {"notes_output.lectureNotes",Field::LectureNotes}};
bool Configuration(Field f) {
    return f==Field::AssistantProvider || f==Field::AssistantCodexModel || f==Field::AssistantReasoning
        || f==Field::AssistantHttpModel || f==Field::AssistantInstructions || f==Field::AssistantExplainPrompt;
}
bool Secret(const QString& value,const settings::AssistiveSettings& cfg) {
    const auto configured=cfg.vlmApiKey.trimmed(), environment=qEnvironmentVariable("OKUFLOW_VLM_API_KEY").trimmed();
    return (!configured.isEmpty() && value.contains(configured)) || (!environment.isEmpty() && value.contains(environment));
}
QStringList Reasoning(const QJsonArray& catalog,const QString& model) {
    QStringList out;
    for(const auto& row:catalog)if(row.toObject().value("id").toString()==model)
        for(const auto& effort:row.toObject().value("supportedReasoningEfforts").toArray()) {
            const auto id=(effort.isString()?effort.toString():effort.toObject().value("reasoningEffort").toString()).trimmed().toLower();
            if(!id.isEmpty() && id.size()<=256 && !out.contains(id))out.append(id);
        }
    return out;
}
}

std::optional<Field> AppControlTarget::AssistantParameterField(std::string_view id) {
    for(const auto& [name,field]:kFields)if(name==id)return field;
    return {};
}

std::map<Field,QJsonValue> AppControlTarget::AssistantParameterFields() const {
    const auto& cfg=app_.settingsController_->MutableSettings().assistive;
    QString voice="system";
    if(!cfg.ttsVoiceName.isEmpty()) {
        for(const auto& [handle,identity]:assistantVoiceHandles_)
            if(identity.size()>=3 && identity[0]==cfg.ttsEngine
                && identity[1]==cfg.ttsVoiceName && identity[2]==cfg.ttsVoiceLocale)voice=handle;
        if(voice=="system") {
            if(assistantVoiceHandles_.size()>=256)voice.clear();
            else {voice=QStringLiteral("voice-")+QUuid::createUuid().toString(QUuid::Id128);
                assistantVoiceHandles_[voice]={cfg.ttsEngine,cfg.ttsVoiceName,cfg.ttsVoiceLocale,"missing"};}
        }
    }
    return {{Field::AssistantSceneExplain,app_.AgentSceneState().value("requested")},{Field::AssistantOverlay,app_.assistiveOverlayEnabled_},
        {Field::AssistantProvider,cfg.aiProvider.trimmed().compare("codex",Qt::CaseInsensitive)==0?"codex":"openai-compatible"},
        {Field::AssistantCodexModel,cfg.codexModel},{Field::AssistantReasoning,cfg.codexReasoningEffort},
        {Field::AssistantHttpModel,cfg.vlmModel},{Field::AssistantInstructions,cfg.assistantInstructions},
        {Field::AssistantExplainPrompt,cfg.vlmPrompt},{Field::AssistantVoice,voice.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(voice)},{Field::AssistantTtsRate,cfg.ttsRate},
        {Field::LectureNotes,cfg.lectureNotesEnabled}};
}

QJsonObject AppControlTarget::QueryAssistantParameters(ToolId tool,const QJsonObject& args) const {
    const bool notes=tool==ToolId::NotesPreferencesGet || tool==ToolId::NotesPreferencesSet;
    const auto& cfg=app_.settingsController_->MutableSettings().assistive;
    const auto requestedFields=args.value("fields").toArray();
    auto selected=[&](std::string_view id){return requestedFields.isEmpty() || requestedFields.contains(QString::fromUtf8(id.data(),qsizetype(id.size())));};
    const bool needVoice=!notes && selected("assistant_preferences.ttsVoice");
    if(needVoice) {
        AssistantParameterFields(); // Retain exact native preference identity for inverse restoration.
        const auto engines=QTextToSpeech::availableEngines();
        QString engine;
        for(const auto& name:engines)if(name.compare(cfg.ttsEngine,Qt::CaseInsensitive)==0)engine=name;
        if(engine.isEmpty())for(const auto& preferred:{QStringLiteral("winrt"),QStringLiteral("sapi")})
            for(const auto& name:engines)if(engine.isEmpty() && name.compare(preferred,Qt::CaseInsensitive)==0)engine=name;
        if(assistantSpeechInventory_ && engine.compare(assistantVoiceInventoryEngine_,Qt::CaseInsensitive)!=0) {
            delete assistantSpeechInventory_;assistantSpeechInventory_=nullptr;
        }
        if(!assistantSpeechInventory_ && !engine.isEmpty()) {
            assistantSpeechInventory_=new QTextToSpeech(engine,&app_);
            assistantVoiceInventoryEngine_=engine;
        }
    }
    QJsonArray voices;
    int omittedVoices=0;
    voices.append(QJsonObject{{"id","system"},{"label","System default"}});
    if(needVoice)for(auto& [handle,identity]:assistantVoiceHandles_) {
        Q_UNUSED(handle);
        if(identity.size()>=4)identity[3]="missing";
    }
    if(needVoice && assistantSpeechInventory_) {
        const auto installed=assistantSpeechInventory_->findVoices();
        int n=0;
        for(const auto& item:installed) {
            if(++n>256) {omittedVoices+=installed.size()-n+1;break;}
            QString handle;
            for(auto& [id,identity]:assistantVoiceHandles_)if(identity.size()>=4 && identity[1]==item.name()
                && (identity[2]==item.locale().name() || (identity[2].isEmpty() && identity[3]=="missing"))
                && (identity[0].isEmpty() || identity[0].compare(assistantVoiceInventoryEngine_,Qt::CaseInsensitive)==0)) {
                    handle=id;identity[3]="installed";break;
                }
            if(handle.isEmpty()) {if(assistantVoiceHandles_.size()>=256) {++omittedVoices;continue;}
                handle="voice-"+QUuid::createUuid().toString(QUuid::Id128);assistantVoiceHandles_[handle]={assistantVoiceInventoryEngine_,item.name(),item.locale().name(),"installed"};}
            const auto locale=Secret(item.locale().name(),cfg)?QString():item.locale().name();
            if(voices.size()<33)voices.append(QJsonObject{{"id",handle},{"label",QStringLiteral("Voice %1 (%2)").arg(n).arg(locale)}});
            else ++omittedVoices;
        }
    }
    const auto fields=AssistantParameterFields();
    const auto& committed=app_.settingsController_->LastCommittedSettings();
    auto savedValue=[&](Field field)->QJsonValue {
        if(!committed)return QJsonValue(QJsonValue::Null);
        const auto& stored=committed->assistive;
        switch(field) {
        case Field::AssistantSceneExplain:return QJsonValue(QJsonValue::Null);
        case Field::AssistantOverlay:return committed->currentConfig.assistiveOverlayEnabled;
        case Field::AssistantProvider:return stored.aiProvider.trimmed().compare("codex",Qt::CaseInsensitive)==0?"codex":"openai-compatible";
        case Field::AssistantCodexModel:return stored.codexModel;
        case Field::AssistantReasoning:return stored.codexReasoningEffort;
        case Field::AssistantHttpModel:return stored.vlmModel;
        case Field::AssistantInstructions:return stored.assistantInstructions;
        case Field::AssistantExplainPrompt:return stored.vlmPrompt;
        case Field::AssistantTtsRate:return stored.ttsRate;
        case Field::LectureNotes:return stored.lectureNotesEnabled;
        case Field::AssistantVoice:
            if(stored.ttsVoiceName.isEmpty())return "system";
            for(const auto& [handle,identity]:assistantVoiceHandles_)if(identity.size()>=3 && identity[1]==stored.ttsVoiceName && identity[2]==stored.ttsVoiceLocale
                && identity[0]==stored.ttsEngine)return handle;
            if(assistantVoiceHandles_.size()<256) {
                const auto handle="voice-"+QUuid::createUuid().toString(QUuid::Id128);
                assistantVoiceHandles_[handle]={stored.ttsEngine,stored.ttsVoiceName,stored.ttsVoiceLocale,"missing"};return handle;
            }
            return QJsonValue(QJsonValue::Null);
        default:return QJsonValue(QJsonValue::Null);
        }
    };
    QJsonObject parameters;
    for(const auto& [id,field]:kFields) {
        if((field==Field::LectureNotes)!=notes || !selected(id))continue;
        const auto name=QString::fromUtf8(id.data(),qsizetype(id.size()));
        const auto value=fields.at(field);
        QString reason;
        if(!value.isString() && !AssistantPreferenceValueInContract(field,value))reason="value_out_of_range";
        if(field==Field::AssistantSceneExplain && (!app_.cameraActive_ || app_.debugViewEnabled_))reason="no_frame";
        if(value.isString() && Secret(value.toString(),cfg))reason="protected_value";
        if(field==Field::AssistantVoice && value!="system") {
            const auto found=assistantVoiceHandles_.find(value.toString());
            if(found==assistantVoiceHandles_.end() || found->second.value(3)!="installed")reason="voice_unavailable";
        }
        if(field==Field::AssistantVoice && (!assistantSpeechInventory_ || assistantSpeechInventory_->state()==QTextToSpeech::Error))reason="tts_unavailable";
        const bool available=reason.isEmpty();
        const auto visible=reason=="protected_value" || reason=="value_out_of_range"?QJsonValue(QJsonValue::Null):value;
        auto saved=savedValue(field);
        const bool legacySaved=!saved.isNull() && !saved.isString() && !AssistantPreferenceValueInContract(field,saved);
        if(legacySaved)saved=QJsonValue(QJsonValue::Null);
        if(saved.isString() && Secret(saved.toString(),cfg))saved=QJsonValue(QJsonValue::Null);
        QJsonObject item{{"requested",visible},{"effective",available && !assistantParametersPending_?visible:QJsonValue(QJsonValue::Null)},
            {"saved",saved},{"available",available},
            {"unsupportedReason",reason},{"persistence",field==Field::AssistantOverlay || field==Field::AssistantSceneExplain?"live_profile":"shared_saved"},
            {"writable",available},{"requiresUi",Configuration(field)}};
        if(assistantParametersPending_)item.insert("pendingApplication",true);
        if(reason=="value_out_of_range" || legacySaved) {
            item.insert("requiresUi",true);item.insert("valueOutOfContract",true);
            if(legacySaved)item.insert("savedUnavailableReason","value_out_of_range");
        }
        if(app_.vlmAssistEnabled_ && field!=Field::AssistantSceneExplain && (field!=Field::AssistantOverlay || !value.toBool())) {
            item.insert("writable",false);item.insert("policyReason","native_scene_active");
        }
        const bool codex=cfg.aiProvider.trimmed().compare("codex",Qt::CaseInsensitive)==0;
        if((!codex && (field==Field::AssistantCodexModel || field==Field::AssistantReasoning)) || (codex && field==Field::AssistantHttpModel)) {
            item.insert("effective",QJsonValue(QJsonValue::Null));item.insert("inactiveReason","provider_not_selected");
        }
        if(field==Field::AssistantSceneExplain) {
            const auto state=app_.AgentSceneState();
            item.insert("effective",state.value("effective"));
            item.insert("saved",QJsonValue(QJsonValue::Null));
            item.insert("persistence","context_only");
            item.insert("nativeRequested",state.value("nativeRequested"));
            item.insert("requiresFrameGrant",true);
        }
        if(field==Field::AssistantVoice) {item.insert("options",voices);if(omittedVoices)item.insert("omittedOptions",omittedVoices);}
        if(field==Field::AssistantCodexModel) {
            QJsonArray options;int omitted=0;
            for(const auto& candidate:app_.codexModelCatalog_) {
                const auto model=candidate.toObject().value("id").toString();
                if(model.isEmpty() || model.toUcs4().size()>256 || Secret(model,cfg))continue;
                if(options.size()<16)options.append(model);else ++omitted;
            }
            item.insert("options",options);if(omitted)item.insert("omittedOptions",omitted);
            if(codex && !assistantParametersPending_ && !app_.selectedCodexModel_.isEmpty())
                item.insert("effective",Secret(app_.selectedCodexModel_,cfg)?QJsonValue(QJsonValue::Null):QJsonValue(app_.selectedCodexModel_));
        }
        if(field==Field::AssistantReasoning) {
            QJsonArray options;
            int omitted=0;
            for(const auto& effort:Reasoning(app_.codexModelCatalog_,cfg.codexModel))if(!Secret(effort,cfg)) {
                if(options.size()<16)options.append(effort);else ++omitted;
            }
            item.insert("options",options);
            if(omitted)item.insert("omittedOptions",omitted);
        }
        parameters.insert(name,item);
    }
    return BoundAssistantPreferenceRead(parameters,notes?QStringLiteral("all"):args.value("valueKind").toString("all"));
}

ControlPlan AppControlTarget::PrepareAssistantParameters(ToolId tool,const QJsonObject& args) const {
    ControlPlan plan;
    const bool notes=tool==ToolId::NotesPreferencesGet || tool==ToolId::NotesPreferencesSet;
    const bool setter=tool==ToolId::AssistantPreferencesSet || tool==ToolId::NotesPreferencesSet;
    const auto& cfg=app_.settingsController_->MutableSettings().assistive;
    if(!setter) {
        const auto reply=QueryAssistantParameters(tool,args).value("parameters").toObject();
        for(auto it=reply.begin();it!=reply.end();++it)if(it.value().toObject().contains("requested") && !it.value().toObject().value("requested").isNull())
            if(auto f=AssistantParameterField(it.key().toStdString()))plan.observedFields.push_back(*f);
        return plan;
    }
    QSet<QString> installed;
    if(args.value("assistant_preferences.sceneExplain").toBool() && (!app_.cameraActive_ || app_.debugViewEnabled_)) {
        plan.error="unavailable";return plan;
    }
    if(args.contains("assistant_preferences.ttsVoice")) {
        QueryAssistantParameters(ToolId::AssistantPreferencesGet,{{"fields",QJsonArray{"assistant_preferences.ttsVoice"}}});
        for(const auto& [handle,identity]:assistantVoiceHandles_)if(identity.value(3)=="installed")installed.insert(handle);
    }
    return PrepareAssistantPreferencePatch(cfg,app_.codexModelCatalog_,installed,args,notes,qEnvironmentVariable("OKUFLOW_VLM_API_KEY"),
        app_.vlmAssistEnabled_,app_.assistiveOverlayEnabled_);
}

std::optional<bool> AppControlTarget::ApplyAssistantParameter(const FieldWrite& write) {
    auto& cfg=app_.settingsController_->MutableSettings().assistive;
    switch(write.field) {
    case Field::AssistantSceneExplain:return app_.SetAgentSceneEnabled(write.value.toBool());
    case Field::AssistantOverlay:
        app_.assistiveOverlayEnabled_=write.value.toBool();
        if(app_.uiState_->assistiveOverlayCheckbox_) {QSignalBlocker blocker(app_.uiState_->assistiveOverlayCheckbox_);app_.uiState_->assistiveOverlayCheckbox_->setChecked(app_.assistiveOverlayEnabled_);}break;
    case Field::AssistantProvider:cfg.aiProvider=write.value=="codex"?"codex":"openai";break;
    case Field::AssistantCodexModel:cfg.codexModel=write.value.toString();break;
    case Field::AssistantReasoning:cfg.codexReasoningEffort=write.value.toString();break;
    case Field::AssistantHttpModel:cfg.vlmModel=write.value.toString();break;
    case Field::AssistantInstructions:cfg.assistantInstructions=write.value.toString();break;
    case Field::AssistantExplainPrompt:cfg.vlmPrompt=write.value.toString();break;
    case Field::AssistantVoice:
        if(write.value=="system") {cfg.ttsVoiceName.clear();cfg.ttsVoiceLocale.clear();}
        else {const auto it=assistantVoiceHandles_.find(write.value.toString());if(it==assistantVoiceHandles_.end() || it->second.size()<3)return false;
            cfg.ttsEngine=it->second[0];cfg.ttsVoiceName=it->second[1];cfg.ttsVoiceLocale=it->second[2];}break;
    case Field::AssistantTtsRate:cfg.ttsRate=write.value.toDouble();break;
    case Field::LectureNotes:cfg.lectureNotesEnabled=write.value.toBool();break;
    default:return {};
    }
    assistantParametersPending_=true;
    assistantRuntimeSettingsPending_|=write.field!=Field::AssistantOverlay;
    return true;
}

void AppControlTarget::FinalizeAssistantParameters() {
    if(!assistantParametersPending_ || assistantParametersQueued_)return;
    assistantParametersQueued_=true;
    QPointer<OkuFlowApp> app=&app_;
    QMetaObject::invokeMethod(&app_,[app,target=this] {
        if(!app || app->controlTarget_.get()!=target)return;
        target->assistantParametersQueued_=false;
        if(!target->assistantParametersPending_)return;
        target->assistantParametersPending_=false;
        const bool runtime=target->assistantRuntimeSettingsPending_;target->assistantRuntimeSettingsPending_=false;
        if(runtime) {
            app->CancelPendingAssistantCaptures();
            app->RefreshAgentControlSettings();
            app->assistiveManager_->ApplySettings(app->settingsController_->MutableSettings().assistive);
        }
        app->assistiveManager_->SetOverlayEnabled(app->assistiveOverlayEnabled_);
        app->UpdateProcessingStatusLabel();
    },Qt::QueuedConnection);
}
} // namespace okuflow::agent
#endif
