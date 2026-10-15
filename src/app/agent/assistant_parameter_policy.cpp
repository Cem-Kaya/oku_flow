#include "okuflow/app/agent/assistant_parameter_policy.hpp"
#include <cmath>
#include <QJsonDocument>
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
bool Secret(const QString& value,const settings::AssistiveSettings& cfg,const QString& environmentSecret) {
    const auto configured=cfg.vlmApiKey.trimmed(), environment=environmentSecret.trimmed();
    return (!configured.isEmpty() && value.contains(configured)) || (!environment.isEmpty() && value.contains(environment));
}
bool SafeText(const QString& value,int limit,bool multiline,const settings::AssistiveSettings& cfg,const QString& environmentSecret) {
    const auto codepoints=value.toUcs4();
    if(codepoints.size()>limit || Secret(value,cfg,environmentSecret))return false;
    for(const auto c:codepoints) {
        const auto category=QChar::category(c);
        if(category==QChar::Other_Control && !(multiline && (c=='\n'||c=='\t'||c=='\r')))return false;
        if(category==QChar::Other_Format && !(multiline && (c==0x200e || c==0x200f)))return false;
    }
    return true;
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
bool KnownModel(const QJsonArray& catalog,const QString& model) {
    for(const auto& row:catalog)if(row.toObject().value("id").toString()==model)return true;
    return false;
}
std::optional<Field> FindField(std::string_view id) {for(const auto& [name,field]:kFields)if(name==id)return field;return {};}
}
ControlPlan PrepareAssistantPreferencePatch(const settings::AssistiveSettings& cfg,const QJsonArray& models,const QSet<QString>& installedVoices,const QJsonObject& args,bool notes,const QString& environmentSecret,bool nativeScene,bool overlayEnabled) {
    ControlPlan plan;
    if(args.isEmpty()) {plan.error="invalid_argument";return plan;}
    auto reject=[&](const QString& code){plan=ControlPlan{};plan.error=code;return plan;};
    QString model=args.value("assistant_preferences.codexModel").toString(cfg.codexModel);
    auto efforts=Reasoning(models,model);
    efforts.removeIf([&](const QString& effort){return !SafeText(effort,256,false,cfg,environmentSecret);});
    for(auto it=args.begin();it!=args.end();++it) {
        const auto field=FindField(it.key().toStdString());
        if(!field || ((*field==Field::LectureNotes)!=notes))return reject("invalid_argument");
        QJsonValue value=it.value();
        switch(*field) {
        case Field::AssistantSceneExplain:if(!value.isBool())return reject("invalid_argument");plan.requiresFrames=value.toBool();break;
        case Field::AssistantOverlay:case Field::LectureNotes:if(!value.isBool())return reject("invalid_argument");break;
        case Field::AssistantProvider:if(value!="codex" && value!="openai-compatible")return reject("invalid_argument");break;
        case Field::AssistantCodexModel:
            if(!value.isString() || !SafeText(value.toString(),256,false,cfg,environmentSecret))return reject("invalid_argument");
            if(value!=cfg.codexModel && !KnownModel(models,value.toString()))return reject("unavailable");
            if(value!=cfg.codexModel && efforts.isEmpty())return reject("unavailable");break;
        case Field::AssistantReasoning:
            if(!value.isString() || !SafeText(value.toString(),256,false,cfg,environmentSecret))return reject("invalid_argument");
            if(!efforts.contains(value.toString()) && !(model==cfg.codexModel && value==cfg.codexReasoningEffort))return reject("unavailable");break;
        case Field::AssistantHttpModel:
            if(!value.isString() || value.toString().trimmed().isEmpty() || !SafeText(value.toString(),256,false,cfg,environmentSecret))return reject("invalid_argument");break;
        case Field::AssistantInstructions:case Field::AssistantExplainPrompt:
            if(!value.isString() || !SafeText(value.toString(),4096,true,cfg,environmentSecret))return reject("invalid_argument");break;
        case Field::AssistantTtsRate:
            if(!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < -1 || value.toDouble()>1)return reject("invalid_argument");
            value=std::round(value.toDouble()*100)/100.0;break;
        case Field::AssistantVoice: {
            if(!value.isString() || (value!="system" && !installedVoices.contains(value.toString())))return reject("unavailable");break;
        }
        default:return reject("invalid_argument");
        }
        if(nativeScene && ((*field==Field::AssistantOverlay && value.toBool() && !overlayEnabled)
            || (*field==Field::AssistantTtsRate && value.toDouble()!=cfg.ttsRate)
            || (*field==Field::LectureNotes && value.toBool()!=cfg.lectureNotesEnabled)
            || (*field==Field::AssistantVoice && (value!="system" || !cfg.ttsVoiceName.isEmpty()))))return reject("denied_by_policy");
        plan.writes.push_back({*field,value});
        if(Configuration(*field)) {
            QJsonValue current;
            switch(*field) {
            case Field::AssistantProvider:current=cfg.aiProvider.trimmed().compare("codex",Qt::CaseInsensitive)==0?"codex":"openai-compatible";break;
            case Field::AssistantCodexModel:current=cfg.codexModel;break;
            case Field::AssistantReasoning:current=cfg.codexReasoningEffort;break;
            case Field::AssistantHttpModel:current=cfg.vlmModel;break;
            case Field::AssistantInstructions:current=cfg.assistantInstructions;break;
            case Field::AssistantExplainPrompt:current=cfg.vlmPrompt;break;
            default:break;
            }
            if(current!=value) {plan.requiresApproval=true;plan.approvalDomain=ApprovalDomain::Configuration;}
        }
    }
    if(model!=cfg.codexModel && !args.contains("assistant_preferences.codexReasoning") && !efforts.contains(cfg.codexReasoningEffort))
        plan.writes.push_back({Field::AssistantReasoning,efforts.contains("low")?QStringLiteral("low"):efforts.first()});
    if(plan.requiresApproval && args.value("assistant_preferences.sceneExplain").toBool())return reject("denied_by_policy");
    if(nativeScene && plan.requiresApproval)return reject("denied_by_policy");
    return plan;
}
bool AssistantPreferenceValueInContract(Field field,const QJsonValue& value) {
    if(field==Field::AssistantSceneExplain || field==Field::AssistantOverlay || field==Field::LectureNotes)return value.isBool();
    if(field==Field::AssistantTtsRate)return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble()>=-1 && value.toDouble()<=1;
    if(!value.isString())return false;
    const auto limit=field==Field::AssistantInstructions || field==Field::AssistantExplainPrompt?4096:256;
    return value.toString().toUcs4().size()<=limit;
}
QJsonObject BoundAssistantPreferenceRead(QJsonObject parameters,const QString& valueKind) {
    constexpr qsizetype budget=29*1024;
    for(auto it=parameters.begin();it!=parameters.end();++it) {
        auto row=it.value().toObject();
        if(valueKind=="requested") {row.remove("effective");row.remove("saved");}
        else if(valueKind=="saved") {row.remove("effective");row.remove("requested");}
        const auto maximum=it.key()=="assistant_preferences.instructions" || it.key()=="assistant_preferences.explainPrompt"?4096:256;
        for(const auto* key:{"requested","effective","saved"})if(row.value(key).isString() && row.value(key).toString().toUcs4().size()>maximum) {
            auto single=row;
            for(const auto* other:{"requested","effective","saved"})if(QString::fromLatin1(other)!=QString::fromLatin1(key))single.remove(other);
            if(QJsonDocument(QJsonObject{{"parameters",QJsonObject{{it.key(),single}}}}).toJson(QJsonDocument::Compact).size()>budget) {
                row.insert(key,QJsonValue(QJsonValue::Null));row.insert("requiresUi",true);row.insert("valueOutOfContract",true);
                row.insert("unsupportedReason","value_out_of_range");row.insert("filterRequired",false);row.insert("writable",false);
            }
        }
        it.value()=row;
    }
    auto size=[&]{return QJsonDocument(QJsonObject{{"parameters",parameters}}).toJson(QJsonDocument::Compact).size();};
    if(size()>budget && valueKind=="all") {
        for(auto it=parameters.begin();it!=parameters.end();++it) {
            auto row=it.value().toObject();
            if(row.value("requested").isString())for(const auto* key:{"effective","saved"})
                if(row.value(key)==row.value("requested"))row.insert(key,QJsonObject{{"sameAs","requested"}});
            it.value()=row;
        }
    }
    QSet<QString> suppressed;
    while(size()>budget) {
        QString largest;qsizetype bytes=0;
        for(auto it=parameters.begin();it!=parameters.end();++it) {
            const auto row=it.value().toObject();if(row.value("filterRequired").toBool() || suppressed.contains(it.key()))continue;
            const auto rowSize=QJsonDocument(row).toJson(QJsonDocument::Compact).size();
            if(rowSize>bytes) {bytes=rowSize;largest=it.key();}
        }
        if(largest.isEmpty())break;
        auto row=parameters.value(largest).toObject();
        for(const auto* key:{"requested","effective","saved"})if(row.contains(key))row.insert(key,QJsonValue(QJsonValue::Null));
        row.remove("options");
        if(!row.value("valueOutOfContract").toBool()) {
            row.insert("filterRequired",true);row.insert("unsupportedReason","response_budget");
        }
        suppressed.insert(largest);
        parameters.insert(largest,row);
    }
    return {{"parameters",parameters}};
}
}
