#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/agent/control_schema.hpp"
#include <QJsonArray>
#include <array>
#include <algorithm>
#include <iterator>

#ifndef OKUFLOW_UI_PARAMETER_PLANNER_ONLY
#include "../app_internal.hpp"
#endif

namespace okuflow::agent {
namespace {
struct Binding { std::string_view id; Field field; };
constexpr Binding kBindings[]{
    {"ui.mode",Field::UiMode},{"ui.inspectorWidth",Field::UiInspectorWidth},
    {"ui.language",Field::UiLanguage},{"ui.hidden",Field::UiHidden},
    {"ui.sectionExpanded",Field::UiSectionExpanded},{"ui.assistiveDock",Field::UiAssistiveDock},
    {"ui.assistiveGeometry",Field::UiAssistiveGeometry},{"ui.setupPromptDeclined",Field::UiSetupPromptDeclined},
    {"ui.activeTab",Field::UiActiveTab},{"annotation.color",Field::AnnotationColor},
    {"annotation.width",Field::AnnotationWidth},{"annotation.textSize",Field::AnnotationTextSize},
    {"annotation.dashed",Field::AnnotationDashed},{"annotation.shape",Field::AnnotationShape},
    {"annotation.captureOnExit",Field::AnnotationCaptureOnExit},{"annotation.enabled",Field::AnnotationEnabled},
    {"annotation.tool",Field::AnnotationTool}};
QString Key(std::string_view id) { return QString::fromUtf8(id.data(),qsizetype(id.size())); }
QJsonObject Schema(const std::string& text) { return QJsonDocument::fromJson(QByteArray::fromStdString(text)).object(); }
bool UiGroup(ToolId tool,ParameterGroup& group,bool& setter) {
    return ParameterGroupForTool(tool,group,setter) &&
           (group==ParameterGroup::Ui || group==ParameterGroup::Annotation);
}
}

std::optional<Field> AppControlTarget::UiParameterField(std::string_view id) {
    for(const auto& binding:kBindings)if(binding.id==id)return binding.field;
    return {};
}

ControlPlan AppControlTarget::PlanUiParameterDependencies(const std::map<Field,QJsonValue>& current,
                                                        ToolId tool,const QJsonObject& args) {
    ControlPlan plan;
    ParameterGroup group; bool setter{};
    if(!UiGroup(tool,group,setter) || !MatchesControlSchema(args,
        Schema(setter?ParameterSetSchema(group):ParameterGetSchema(group)))) {
        plan.error="invalid_argument";return plan;
    }
    const auto selected=args.value("fields").toArray();
    if(!setter) {
        for(const auto& binding:kBindings) {
            const auto* descriptor=FindParameter(binding.id);
            const auto found=current.find(binding.field);
            if(descriptor->group==group && (selected.isEmpty() || selected.contains(Key(binding.id))) &&
                found!=current.end() && !found->second.isNull())plan.observedFields.push_back(binding.field);
        }
        return plan;
    }
    const auto& patch=args;
    for(const auto& binding:kBindings) {
        const auto key=Key(binding.id);
        if(!patch.contains(key))continue;
        const auto found=current.find(binding.field);
        if(found==current.end()) {plan.error="unavailable";plan.writes.clear();return plan;}
        auto value=patch.value(key);
        if(binding.field==Field::AnnotationColor)value=value.toString().toLower();
        if(binding.field==Field::UiSectionExpanded) {
            auto merged=found->second.toObject();
            const auto delta=value.toObject();
            for(auto it=delta.begin();it!=delta.end();++it)merged.insert(it.key(),it.value());
            value=merged;
        }
        if(value!=found->second) {
            plan.writes.push_back({binding.field,value});
            if(binding.field==Field::AnnotationEnabled) {
                plan.requiresApproval=true;plan.approvalDomain=ApprovalDomain::Configuration;
            }
            if(binding.field==Field::AnnotationCaptureOnExit && value.toBool())plan.requiresPhotos=true;
        }
    }
    // Controlled activation retains the selected tool; apply an explicit tool
    // after activation so the same patch never depends on native defaults.
    const auto enabled=std::find_if(plan.writes.begin(),plan.writes.end(),[](const auto& write) {
        return write.field==Field::AnnotationEnabled;
    });
    if(enabled!=plan.writes.end() && enabled->value.toBool())
        std::rotate(plan.writes.begin(),enabled,std::next(enabled));
    return plan;
}

#ifndef OKUFLOW_UI_PARAMETER_PLANNER_ONLY
namespace {
QJsonObject Sections(const QMap<QString,bool>& states) {
    QJsonObject result;
    const auto properties=Schema(std::string(FindParameter("ui.sectionExpanded")->schema))["properties"].toObject();
    for(auto it=states.begin();it!=states.end();++it)if(properties.contains(it.key()))result.insert(it.key(),it.value());
    return result;
}
QJsonValue Geometry(const QRect& rect) {
    if(!rect.isValid())return QJsonValue(QJsonValue::Null);
    return QJsonObject{{"x",rect.x()},{"y",rect.y()},{"width",rect.width()},{"height",rect.height()}};
}
QString ToolName(AnnotationTool tool) {
    switch(tool) {
    case AnnotationTool::kPointer:return "pointer";
    case AnnotationTool::kPen:return "pen";
    case AnnotationTool::kLine:return "line";
    case AnnotationTool::kShape:return "shape";
    case AnnotationTool::kText:return "text";
    case AnnotationTool::kEraser:return "eraser";
    }
    return {};
}
std::optional<AnnotationTool> ToolValue(const QString& name) {
    for(const auto tool:{AnnotationTool::kPointer,AnnotationTool::kPen,AnnotationTool::kLine,
                        AnnotationTool::kShape,AnnotationTool::kText,AnnotationTool::kEraser})
        if(ToolName(tool)==name)return tool;
    return {};
}
QString TabName(int index) {
    constexpr const char* tabs[]{"image","assistant","settings"};
    return index>=0 && index<3?QString::fromLatin1(tabs[index]):QString();
}
}

std::map<Field,QJsonValue> AppControlTarget::UiParameterFields() const {
    std::map<Field,QJsonValue> fields;
    if(!app_.mainWindow_ || !app_.settingsController_)return fields;
    const auto& settings=app_.settingsController_->Settings();
    const auto& window=*app_.mainWindow_;
    fields={{Field::UiMode,window.isSimpleMode()?"simple":"advanced"},
        {Field::UiInspectorWidth,window.preferredAdvancedPanelWidth()},
        {Field::UiLanguage,settings.language},{Field::UiHidden,window.isUiHidden()},
        {Field::UiSectionExpanded,Sections(window.sectionStates())},
        {Field::UiSetupPromptDeclined,settings.setupAssistantDeclined},
        {Field::UiActiveTab,TabName(window.advancedActiveTab())}};
    if(app_.assistiveManager_) {
        fields[Field::UiAssistiveDock]=app_.assistiveManager_->Overlay().DockPosition();
        fields[Field::UiAssistiveGeometry]=Geometry(app_.assistiveManager_->OverlayGeometry());
    }
    if(const auto* overlay=window.annotationOverlay()) {
        fields[Field::AnnotationColor]=overlay->InkColor().name(QColor::HexRgb);
        fields[Field::AnnotationWidth]=overlay->InkWidthPixels();
        fields[Field::AnnotationTextSize]=overlay->TextSizePixels();
        fields[Field::AnnotationDashed]=overlay->Dashed();
        fields[Field::AnnotationShape]=overlay->ShapeKind();
        fields[Field::AnnotationCaptureOnExit]=overlay->CaptureOnExit();
        fields[Field::AnnotationTool]=ToolName(overlay->SelectedTool());
        fields[Field::AnnotationEnabled]=overlay->IsActive();
    }
    return fields;
}

ControlPlan AppControlTarget::PrepareUiParameters(ToolId tool,const QJsonObject& args) const {
    return PlanUiParameterDependencies(UiParameterFields(),tool,args);
}

QJsonObject AppControlTarget::QueryUiParameters(ToolId tool,const QJsonObject& args) const {
    ParameterGroup group;bool setter{};
    if(!UiGroup(tool,group,setter) || setter || !MatchesControlSchema(args,Schema(ParameterGetSchema(group))))
        return ControlResult::Failure("invalid_argument").content;
    const auto fields=UiParameterFields();
    const auto filter=args["fields"].toArray();
    const auto& committed=app_.settingsController_->LastCommittedSettings();
    auto saved=[&](Field field)->QJsonValue {
        if(!committed)return QJsonValue(QJsonValue::Null);
        const auto& value=*committed;
        switch(field) {
        case Field::UiMode:return value.simpleUiMode?"simple":"advanced";
        case Field::UiInspectorWidth:return value.advancedPanelWidth;
        case Field::UiLanguage:return value.language;
        case Field::UiSectionExpanded:return Sections(value.uiSectionStates);
        case Field::UiAssistiveDock:return value.assistiveOverlayDockPosition;
        case Field::UiAssistiveGeometry:return Geometry(value.assistiveOverlayGeometry);
        case Field::UiSetupPromptDeclined:return value.setupAssistantDeclined;
        case Field::AnnotationColor:return value.annotationColor;
        case Field::AnnotationWidth:return value.annotationWidthPixels;
        case Field::AnnotationTextSize:return value.annotationTextSizePixels;
        case Field::AnnotationDashed:return value.annotationDashed;
        case Field::AnnotationShape:return value.annotationShapeKind;
        case Field::AnnotationCaptureOnExit:return value.annotationCaptureOnExit;
        default:return QJsonValue(QJsonValue::Null);
        }
    };
    QJsonObject parameters;
    for(const auto& binding:kBindings) {
        const auto* descriptor=FindParameter(binding.id);
        const auto key=Key(binding.id);
        if(descriptor->group!=group || (!filter.isEmpty() && !filter.contains(key)))continue;
        const auto found=fields.find(binding.field);
        const bool available=found!=fields.end();
        const auto value=available?found->second:QJsonValue(QJsonValue::Null);
        QJsonObject item{{"requested",value},{"effective",value},{"saved",saved(binding.field)},
            {"available",available},{"writable",available},{"requiresUi",descriptor->requiresUiApproval},
            {"unsupportedReason",available?QString():QStringLiteral("control_unavailable")},
            {"persistence",Key(descriptor->persistence)}};
        if(available && binding.field==Field::UiInspectorWidth)
            item["effective"]=app_.mainWindow_->advancedPanelWidth();
        if(available && binding.field==Field::UiLanguage && app_.languageManager_)
            item["effective"]=app_.languageManager_->languageCode();
        if(available && (binding.field==Field::UiMode || binding.field==Field::UiActiveTab) &&
            app_.mainWindow_->isUiHidden()) {item["effective"]=QJsonValue(QJsonValue::Null);item["inactiveReason"]="ui_hidden";}
        if(available && binding.field==Field::UiActiveTab && app_.mainWindow_->isSimpleMode()) {
            item["effective"]=QJsonValue(QJsonValue::Null);item["inactiveReason"]="simple_mode";
        }
        if(available && binding.field==Field::AnnotationTool && !fields.at(Field::AnnotationEnabled).toBool()) {
            item["effective"]=QJsonValue(QJsonValue::Null);item["inactiveReason"]="annotation_disabled";
        }
        if(available && binding.field==Field::UiAssistiveGeometry && fields.at(Field::UiAssistiveDock)!="floating") {
            item["effective"]=QJsonValue(QJsonValue::Null);item["inactiveReason"]="docked";
        }
        parameters.insert(key,item);
    }
    return {{"ok",true},{"parameters",parameters}};
}

std::optional<bool> AppControlTarget::ApplyUiParameter(const FieldWrite& write) {
    if(!app_.mainWindow_ || !app_.settingsController_)return false;
    auto& window=*app_.mainWindow_;
    auto& settings=app_.settingsController_->MutableSettings();
    const auto value=write.value;
    switch(write.field) {
    case Field::UiMode:
        window.setSimpleMode(value=="simple");app_.simpleUiMode_=value=="simple";
        settings.simpleUiMode=app_.simpleUiMode_;return true;
    case Field::UiInspectorWidth:window.setAdvancedPanelWidth(value.toInt());return true;
    case Field::UiLanguage: {
        auto* combo=window.applicationLanguageCombo();
        const int index=combo?combo->findData(value.toString()):-1;
        if(index<0)return false;
        // Emit the existing native language transaction even if a failed load
        // left the combo on this index; its handler owns rollback and speech.
        {const QSignalBlocker blocked(combo);combo->setCurrentIndex(index);}
        emit window.applicationLanguageRequested(value.toString());
        return settings.language==value.toString();
    }
    case Field::UiHidden:window.setUiHidden(value.toBool());return true;
    case Field::UiSectionExpanded: {
        QMap<QString,bool> states;const auto object=value.toObject();
        for(auto it=object.begin();it!=object.end();++it)states.insert(it.key(),it.value().toBool());
        window.setSectionStates(states);return true;
    }
    case Field::UiAssistiveDock:
        if(!app_.assistiveManager_)return false;
        app_.assistiveManager_->Overlay().SetDockPosition(value.toString());
        return app_.assistiveManager_->Overlay().DockPosition()==value.toString();
    case Field::UiAssistiveGeometry: {
        if(!app_.assistiveManager_)return false;
        const auto rect=value.toObject();
        app_.assistiveManager_->RestoreOverlayGeometry(QRect(rect["x"].toInt(),rect["y"].toInt(),
            rect["width"].toInt(),rect["height"].toInt()));return true;
    }
    case Field::UiSetupPromptDeclined:
        settings.setupAssistantDeclined=value.toBool();
        if(app_.setupAssistantDialog_)app_.setupAssistantDialog_->SetDeclinedPreference(value.toBool());
        return true;
    case Field::UiActiveTab: {
        const auto name=value.toString();
        return window.setAdvancedActiveTab(name=="image"?0:name=="assistant"?1:2);
    }
    case Field::AnnotationEnabled:
        if(!window.annotationOverlay())return false;
        window.setAnnotationMode(value.toBool(),window.annotationOverlay()->SelectedTool(),true);return true;
    case Field::AnnotationTool: {
        auto* overlay=window.annotationOverlay();const auto tool=ToolValue(value.toString());
        if(!overlay || !tool)return false;overlay->SetTool(*tool,true);return true;
    }
    case Field::AnnotationColor:case Field::AnnotationWidth:case Field::AnnotationTextSize:
    case Field::AnnotationDashed:case Field::AnnotationShape:case Field::AnnotationCaptureOnExit: {
        auto* overlay=window.annotationOverlay();if(!overlay)return false;
        window.setAnnotationPreferences(
            write.field==Field::AnnotationColor?QColor(value.toString()):overlay->InkColor(),
            write.field==Field::AnnotationWidth?value.toInt():overlay->InkWidthPixels(),
            write.field==Field::AnnotationCaptureOnExit?value.toBool():overlay->CaptureOnExit(),
            write.field==Field::AnnotationDashed?value.toBool():overlay->Dashed(),
            write.field==Field::AnnotationShape?value.toString():overlay->ShapeKind(),
            write.field==Field::AnnotationTextSize?value.toInt():overlay->TextSizePixels());
        return true;
    }
    default:return {};
    }
}
#endif
} // namespace okuflow::agent
