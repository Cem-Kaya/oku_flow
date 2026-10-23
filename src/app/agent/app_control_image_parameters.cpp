#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/agent/control_schema.hpp"
#include "okuflow/app/constants.hpp"
#include "okuflow/common/uct_model_settings.hpp"
#include <QJsonArray>
#include <QScopedValueRollback>
#include <algorithm>
#include <cmath>
#include <set>
#include <iterator>

#ifndef OKUFLOW_IMAGE_PARAMETER_PLANNER_ONLY
#include "../app_internal.hpp"
#include "okuflow/app/ui_state_manager.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#endif

namespace okuflow::agent {
bool AppControlTarget::SelectedMediaImageFieldAvailable(Field field, bool gpuAvailable) {
    // Debug composite is a camera diagnostic, not a media shader effect.
    if(field==Field::DebugView)return false;
    if(gpuAvailable)return true;
    switch(field) {
    case Field::BlackWhite:case Field::Threshold:case Field::Contrast:case Field::Brightness:
    case Field::Scheme:case Field::CustomColorMode:case Field::CustomColorStopCount:
    case Field::CustomColorStops:case Field::CustomColorStepped:case Field::CustomColorRecipe:
    case Field::UctGpuDetector:case Field::UctGpuRecognizer:case Field::UctCpuDetector:case Field::UctCpuRecognizer:
        return true;
    default:return false;
    }
}
namespace {
struct ParameterField { std::string_view id; Field field; };
constexpr ParameterField kImageViewFields[] = {
    {"image.uct.gpu.detector",Field::UctGpuDetector},
    {"image.uct.gpu.recognizer",Field::UctGpuRecognizer},
    {"image.uct.cpu.detector",Field::UctCpuDetector},
    {"image.uct.cpu.recognizer",Field::UctCpuRecognizer},
    {"view.zoom.enabled", Field::ZoomEnabled},
    {"view.zoom.amount", Field::ZoomAmount},
    {"view.center.x", Field::CenterX},
    {"view.center.y", Field::CenterY},
    {"view.focusMarker", Field::FocusMarker},
    {"view.fit", Field::Framing},
    {"view.presentationRate", Field::PresentationRate},
    {"view.virtualJoystick", Field::VirtualJoystick},
    {"view.zoomWheelAcceleration", Field::ZoomWheelAcceleration},
    {"image.blackWhite.enabled", Field::BlackWhite},
    {"image.blackWhite.threshold", Field::Threshold},
    {"image.contrast", Field::Contrast},
    {"image.brightness", Field::Brightness},
    {"image.autoContrast.enabled", Field::AutoContrast},
    {"image.autoContrast.strength", Field::AutoContrastStrength},
    {"image.blur.enabled", Field::Blur},
    {"image.blur.sigma", Field::BlurSigma},
    {"image.blur.radius", Field::BlurRadius},
    {"image.temporal.enabled", Field::Temporal},
    {"image.temporal.alpha", Field::TemporalAlpha},
    {"image.spatial.enabled", Field::Spatial},
    {"image.spatial.backend", Field::Upscaler},
    {"image.spatial.sharpness", Field::Sharpness},
    {"image.stabilization.enabled", Field::Stabilization},
    {"image.keystone.enabled", Field::Keystone},
    {"image.debugView", Field::DebugView},
    {"image.textClarity.enabled", Field::TextClarity},
    {"image.textClarity.flatten.enabled", Field::Flatten},
    {"image.textClarity.flatten.strength", Field::FlattenStrength},
    {"image.textClarity.adaptive.enabled", Field::Adaptive},
    {"image.textClarity.adaptive.sauvolaStrength", Field::Sauvola},
    {"image.textClarity.adaptive.softness", Field::Softness},
    {"image.textClarity.polarity", Field::Polarity},
    {"image.textClarity.strokeWeight", Field::Stroke},
    {"image.textClarity.smartSharpen.enabled", Field::SmartSharpen},
    {"image.textClarity.smartSharpen.strength", Field::SmartStrength},
    {"image.textClarity.clahe.enabled", Field::Clahe},
    {"image.textClarity.clahe.clipLimit", Field::ClaheLimit},
    {"image.textClarity.twoColor", Field::TwoColor},
    {"image.textClarity.hysteresis.enabled", Field::Hysteresis},
    {"image.textClarity.hysteresis.strength", Field::HysteresisStrength},
    {"image.textClarity.selectiveSharpen", Field::SelectiveSharpen},
    {"image.textClarity.focusDetection.enabled", Field::FocusDetection},
    {"image.textClarity.focusDetection.threshold", Field::FocusThreshold},
    {"image.textClarity.glare.enabled", Field::Glare},
    {"image.textClarity.glare.strength", Field::GlareStrength},
    {"image.superResolution.enabled", Field::SuperResEnabled},
    {"image.superResolution.strength", Field::SuperResStrength},
    {"image.superResolution.prefer2x", Field::SuperResPrefer2x},
    {"image.superResolution.ultra1440p", Field::SuperResUltra1440p},
    {"image.superResolution.performanceOverride", Field::SuperResPerformanceOverride},
    {"image.stabilization.bumpHold", Field::BumpHold},
    {"image.keystone.paused", Field::KeystonePaused},
    {"image.colors.schemeId", Field::Scheme},
    {"image.colors.custom.mode", Field::CustomColorMode},
    {"image.colors.custom.stopCount", Field::CustomColorStopCount},
    {"image.colors.custom.stops", Field::CustomColorStops},
    {"image.colors.custom.stepped", Field::CustomColorStepped},
};
QString Token(std::string_view value) { return QString::fromUtf8(value.data(),qsizetype(value.size())); }
bool IsCustomComponent(Field f) {
    return f==Field::CustomColorMode || f==Field::CustomColorStopCount || f==Field::CustomColorStops || f==Field::CustomColorStepped;
}
bool IsSuperRes(Field f) {
    return f==Field::SuperResEnabled || f==Field::SuperResStrength || f==Field::SuperResPrefer2x ||
        f==Field::SuperResUltra1440p || f==Field::SuperResPerformanceOverride;
}
bool TextHandler(Field f) {
    switch(f) {
    case Field::TextClarity:case Field::Flatten:case Field::FlattenStrength:case Field::Adaptive:case Field::Sauvola:
    case Field::Softness:case Field::Polarity:case Field::Stroke:case Field::SmartSharpen:case Field::SmartStrength:
    case Field::Clahe:case Field::ClaheLimit:case Field::TwoColor:case Field::Hysteresis:case Field::HysteresisStrength:
    case Field::SelectiveSharpen:case Field::FocusDetection:case Field::FocusThreshold:case Field::Glare:case Field::GlareStrength:
        return true;
    default:return IsSuperRes(f) && f!=Field::SuperResPerformanceOverride;
    }
}
bool IsUctModel(Field f) {
    return f==Field::UctGpuDetector || f==Field::UctGpuRecognizer ||
           f==Field::UctCpuDetector || f==Field::UctCpuRecognizer;
}
QJsonValue Value(const std::map<Field,QJsonValue>& fields,Field f) {
    const auto found=fields.find(f);return found==fields.end()?QJsonValue(QJsonValue::Null):found->second;
}
QJsonValue WireValue(Field f,QJsonValue value) {
    if(f==Field::Upscaler && value.isDouble())return value.toInt()==0?QJsonValue("fsr1"):QJsonValue("nis");
    if(f==Field::Polarity && value.isDouble()) {
        const QStringList tokens{"auto","dark-on-light","light-on-dark"};return tokens.value(value.toInt(),"auto");
    }
    return value;
}
double Scale(Field f) {
    switch(f) {
    case Field::Threshold:return 255;
    case Field::BlurSigma:case Field::ClaheLimit:return 10;
    case Field::FocusThreshold:return 1000;
    case Field::ZoomAmount:case Field::CenterX:case Field::CenterY:case Field::Contrast:case Field::Brightness:
    case Field::AutoContrastStrength:case Field::TemporalAlpha:case Field::Sharpness:case Field::FlattenStrength:
    case Field::Sauvola:case Field::Softness:case Field::SmartStrength:case Field::HysteresisStrength:
    case Field::GlareStrength:case Field::SuperResStrength:return 100;
    default:return 1;
    }
}
QJsonObject Recipe(const std::map<Field,QJsonValue>& fields,const QString& selected) {
    return {{"mode",Value(fields,Field::CustomColorMode)},{"stopCount",Value(fields,Field::CustomColorStopCount)},
        {"stops",Value(fields,Field::CustomColorStops)},{"stepped",Value(fields,Field::CustomColorStepped)},{"schemeId",selected},
        {"name","Custom colors"},{"accessibleName","Custom display colors"},{"textColorAtHighLuma",true},{"effect",false}};
}
}

std::optional<Field> AppControlTarget::ImageParameterField(std::string_view id) {
    for(const auto& entry:kImageViewFields)if(entry.id==id)return entry.field;
    return {};
}

// Mirrors the normalized settings and mask consumers in the CUDA pipeline.
// Refinement preferences remain stored while the master is off; they are dormant.
bool AppControlTarget::ImageParameterEffectiveIntent(Field field,const std::map<Field,QJsonValue>& fields) {
    const auto enabled=[&](Field f){return Value(fields,f).toBool();};
    const bool master=enabled(Field::TextClarity), adaptive=master && enabled(Field::Adaptive);
    const bool smart=master && enabled(Field::SmartSharpen) && !adaptive;
    const bool selective=smart && enabled(Field::SelectiveSharpen);
    const bool consumedMask=adaptive || selective;
    Field prerequisite=field;
    switch(field) {
    case Field::Threshold:return enabled(Field::BlackWhite) && !adaptive;
    case Field::BlackWhite:return enabled(Field::BlackWhite) && !adaptive;
    case Field::BlurSigma:case Field::BlurRadius:prerequisite=Field::Blur;break;
    case Field::TemporalAlpha:prerequisite=Field::Temporal;break;
    case Field::Upscaler:case Field::Sharpness:prerequisite=Field::Spatial;break;
    case Field::AutoContrastStrength:prerequisite=Field::AutoContrast;break;
    case Field::FlattenStrength:prerequisite=Field::Flatten;break;
    case Field::Sauvola:case Field::Softness:case Field::Polarity:case Field::Stroke:return consumedMask;
    case Field::SmartStrength:prerequisite=Field::SmartSharpen;break;
    case Field::ClaheLimit:prerequisite=Field::Clahe;break;
    case Field::Hysteresis:case Field::HysteresisStrength:return consumedMask && enabled(Field::Hysteresis);
    case Field::SelectiveSharpen:return selective;
    case Field::FocusThreshold:prerequisite=Field::FocusDetection;break;
    case Field::GlareStrength:prerequisite=Field::Glare;break;
    case Field::BumpHold:return enabled(Field::BumpHold) && enabled(Field::Stabilization);
    case Field::KeystonePaused:return enabled(Field::KeystonePaused) && enabled(Field::Keystone);
    case Field::TwoColor:return master && adaptive && enabled(Field::TwoColor);
    case Field::Scheme:return !(adaptive && enabled(Field::TwoColor));
    default:break;
    }
    if(IsCustomComponent(field))return Value(fields,Field::Scheme)=="custom" && !(adaptive && enabled(Field::TwoColor));
    const bool intent=Value(fields,prerequisite).isBool()?enabled(prerequisite):true;
    switch(prerequisite) {
    case Field::Flatten:case Field::Adaptive:case Field::Clahe:
    case Field::FocusDetection:case Field::Glare:return master && intent;
    case Field::SmartSharpen:return master && intent && !adaptive;
    default:return intent;
    }
}

// No app/widget/driver access: validation and every implicit delta finish before
// the first FieldWrite is published. This is also the headless fixture seam.
ControlPlan AppControlTarget::PlanImageParameterDependencies(const std::map<Field,QJsonValue>& current,
    ToolId tool,const QJsonObject& args) {
    ControlPlan plan;ParameterGroup group;bool setter=false;
    if(!ParameterGroupForTool(tool,group,setter) || (group!=ParameterGroup::Image && group!=ParameterGroup::View)) {
        plan.error="invalid_argument";return plan;
    }
    auto reject=[&](const char* code){plan.error=code;plan.writes.clear();plan.observedFields.clear();return plan;};
    const auto schema=setter?ParameterSetSchema(group):ParameterGetSchema(group);
    if(!MatchesControlSchema(args,QJsonDocument::fromJson(QByteArray::fromStdString(schema)).object()))return reject("invalid_argument");
    if(!setter) {
        if(args.size()>1 || (!args.isEmpty() && !args.contains("fields")))return reject("invalid_argument");
        const auto requested=args["fields"].toArray();
        if(args.contains("fields") && (!args["fields"].isArray() || requested.isEmpty() || requested.size()>16))return reject("invalid_argument");
        std::set<Field> seen;
        for(const auto& entry:kImageViewFields) {
            const auto* descriptor=FindParameter(entry.id);if(!descriptor || descriptor->group!=group)continue;
            if(requested.isEmpty() || requested.contains(Token(entry.id))) {plan.observedFields.push_back(entry.field);seen.insert(entry.field);}
        }
        if(!requested.isEmpty() && qsizetype(seen.size())!=requested.size())return reject("invalid_argument");
        return plan;
    }
    if(args.isEmpty())return reject("invalid_argument");
    auto desired=current;std::set<Field> touched;bool custom=false,missingDependency=false;
    for(auto it=args.begin();it!=args.end();++it) {
        const auto id=it.key().toStdString();const auto* descriptor=FindParameter(id);const auto field=ImageParameterField(id);
        if(!descriptor || descriptor->group!=group || !field || !current.contains(*field) ||
            !MatchesControlSchema(it.value(),QJsonDocument::fromJson(QByteArray::fromStdString(std::string(descriptor->schema))).object()))return reject("invalid_argument");
        QJsonValue value=it.value();
        if(*field==Field::Upscaler)value=value=="fsr1"?0:1;
        else if(*field==Field::Polarity)value=QStringList{"auto","dark-on-light","light-on-dark"}.indexOf(value.toString());
        else if(value.isDouble()) {
            const auto steps=std::lround(value.toDouble()*Scale(*field));
            if(*field==Field::BlurRadius || *field==Field::Stroke || *field==Field::CustomColorStopCount)value=int(steps);
            else if(*field==Field::BlurSigma)value=double(app_constants::SliderValueToSigma(int(steps)));
            else value=double(float(steps)/float(Scale(*field)));
        }
        if(*field==Field::BlurRadius)value=app_constants::SnapBlurRadius(value.toInt());
        desired[*field]=value;touched.insert(*field);custom|=IsCustomComponent(*field);
    }
    const auto changed=[&](Field field){return touched.contains(field) && Value(desired,field)!=Value(current,field);};
    const bool text=std::any_of(touched.begin(),touched.end(),[&](Field field){return TextHandler(field) && changed(field);});
    auto couple=[&](Field f,QJsonValue value){if(!current.contains(f)){missingDependency=true;return false;}desired[f]=value;touched.insert(f);return true;};
    if(custom) {
        for(const auto key:{"image.colors.custom.mode","image.colors.custom.stopCount","image.colors.custom.stops","image.colors.custom.stepped"})
            if(!args.contains(key))return reject("invalid_argument");
        auto stops=Value(desired,Field::CustomColorStops).toArray();
        if(stops.size()!=Value(desired,Field::CustomColorStopCount).toInt())return reject("invalid_argument");
        if(Value(desired,Field::CustomColorMode)=="duotone" && stops.size()!=2)return reject("invalid_argument");
        for(int i=0;i<stops.size();++i)stops[i]=stops[i].toString().toLower();
        desired[Field::CustomColorStops]=stops;
        if(Value(desired,Field::CustomColorMode)=="posterize")desired[Field::CustomColorStepped]=true;
        if(!args.contains("image.colors.schemeId") && !couple(Field::Scheme,"custom"))return reject("unavailable");
        if(!couple(Field::CustomColorRecipe,Recipe(desired,Value(desired,Field::Scheme).toString())))return reject("unavailable");
    } else if(touched.contains(Field::Scheme) && Value(desired,Field::Scheme)=="custom") {
        if(Value(current,Field::CustomColorStops).toArray().size()<2)return reject("invalid_argument");
    }
    if(args.value("image.superResolution.prefer2x").toBool() && args.value("image.superResolution.ultra1440p").toBool())return reject("invalid_argument");
    if(changed(Field::SuperResPrefer2x) || changed(Field::SuperResUltra1440p)) {
        if(args.value("image.superResolution.prefer2x").toBool())couple(Field::SuperResUltra1440p,false);
        else if(args.value("image.superResolution.ultra1440p").toBool())couple(Field::SuperResPrefer2x,false);
    }
    // OnTextClarityControlsChanged reads the full refinement/SR widget group.
    // Include its real implicit zoom/strength/quality changes in the journal.
    if(text) {
        if(Value(desired,Field::SuperResUltra1440p).toBool())couple(Field::SuperResPrefer2x,false);
        if(Value(desired,Field::SuperResEnabled).toBool()) {
            if(Value(desired,Field::SuperResStrength).toDouble()<=0)couple(Field::SuperResStrength,double(0.65f));
        } else if(Value(current,Field::SuperResEnabled).toBool())couple(Field::SuperResPerformanceOverride,false);
    }
    const bool nonUltraSuperRes=Value(desired,Field::SuperResEnabled).toBool() && !Value(desired,Field::SuperResUltra1440p).toBool();
    const double minimumZoom=Value(desired,Field::SuperResPrefer2x).toBool()?2.0:double(1.33f);
    // A contradictory explicit view request is refused as one batch. Never
    // silently invert a requested boolean or raise an explicitly requested zoom.
    if(nonUltraSuperRes && ((args.contains("view.zoom.enabled") && !args["view.zoom.enabled"].toBool()) ||
        (args.contains("view.zoom.amount") && Value(desired,Field::ZoomAmount).toDouble()<minimumZoom)))return reject("invalid_argument");
    // Only an actually changed refinement invokes the native group handler,
    // whose SR activation can enable zoom and raise its minimum implicitly.
    if(text && nonUltraSuperRes) {
        couple(Field::ZoomEnabled,true);
        couple(Field::ZoomAmount,std::max(Value(desired,Field::ZoomAmount).toDouble(),minimumZoom));
    }
    if(changed(Field::Stabilization)) {
        couple(Field::KeystonePaused,false); // native stabilization resets keystone tracking
        if(!Value(desired,Field::Stabilization).toBool())couple(Field::BumpHold,false);
    }
    if(changed(Field::Keystone))couple(Field::KeystonePaused,false);
    if(args.value("image.stabilization.bumpHold").toBool() && !Value(desired,Field::Stabilization).toBool())return reject("invalid_argument");
    if(args.value("image.keystone.paused").toBool() && !Value(desired,Field::Keystone).toBool())return reject("invalid_argument");
    if(touched.contains(Field::KeystonePaused) && args.contains("image.keystone.paused"))desired[Field::KeystonePaused]=args["image.keystone.paused"];
    if(args.value("image.superResolution.performanceOverride").toBool() && !Value(desired,Field::SuperResEnabled).toBool())return reject("invalid_argument");
    if(missingDependency)return reject("unavailable");
    for(const auto f:touched)if(IsSuperRes(f) && Value(desired,f)!=Value(current,f) &&
        (Value(desired,Field::SuperResEnabled).toBool() || (f==Field::SuperResPerformanceOverride && Value(desired,f).toBool()))) {
        plan.requiresApproval=true;plan.approvalDomain=ApprovalDomain::Configuration;
    }
    std::vector<Field> order(touched.begin(),touched.end());
    auto rank=[](Field f) {
        if(f==Field::CustomColorRecipe)return 0;
        if(f==Field::SuperResUltra1440p || f==Field::SuperResPrefer2x)return 1;
        if(f==Field::SuperResStrength)return 2;
        if(f==Field::SuperResEnabled)return 3;
        if(f==Field::Stabilization || f==Field::Keystone)return 4;
        if(TextHandler(f))return 5;
        if(f==Field::ZoomEnabled)return 6;
        if(f==Field::ZoomAmount)return 7;
        if(f==Field::BumpHold || f==Field::KeystonePaused || f==Field::SuperResPerformanceOverride)return 9;
        return 8;
    };
    std::stable_sort(order.begin(),order.end(),[&](Field a,Field b){return rank(a)<rank(b);});
    for(const auto f:order) {
        if(!current.contains(f))return reject("unavailable");
        plan.writes.push_back({f,Value(desired,f)});
        plan.resetsHistory|=TextHandler(f) || f==Field::Temporal || f==Field::TemporalAlpha || f==Field::Stabilization || f==Field::Keystone;
    }
    return plan;
}

#ifndef OKUFLOW_IMAGE_PARAMETER_PLANNER_ONLY
namespace {
QObject* ParameterWidget(Field field,UIStateManager& ui,MainWindow* window) {
    if(IsUctModel(field)) {
        auto* panel=window?window->ultraClearTextPanel():nullptr;if(!panel)return nullptr;
        if(field==Field::UctGpuDetector)return panel->gpuDetectorCombo();
        if(field==Field::UctGpuRecognizer)return panel->gpuRecognizerCombo();
        if(field==Field::UctCpuDetector)return panel->cpuDetectorCombo();
        return panel->cpuRecognizerCombo();
    }
    switch(field) {
    case Field::ZoomEnabled:return ui.zoomCheckbox_;
    case Field::ZoomAmount:return ui.zoomSlider_;
    case Field::CenterX:return ui.zoomCenterXSlider_;
    case Field::CenterY:return ui.zoomCenterYSlider_;
    case Field::FocusMarker:return ui.focusMarkerCheckbox_;
    case Field::Framing:return ui.viewportFitCombo_;
    case Field::PresentationRate:return ui.viewportRateCombo_;
    case Field::VirtualJoystick:return ui.joystickCheckbox_;
    case Field::ZoomWheelAcceleration:return ui.zoomWheelAccelerationCheckbox_;
    case Field::BlackWhite:return ui.bwCheckbox_;
    case Field::Threshold:return ui.bwSlider_;
    case Field::Contrast:return ui.contrastSlider_;
    case Field::Brightness:return ui.brightnessSlider_;
    case Field::AutoContrast:return ui.autoContrastCheckbox_;
    case Field::AutoContrastStrength:return ui.autoContrastStrengthSlider_;
    case Field::Blur:return ui.blurCheckbox_;
    case Field::BlurSigma:return ui.blurSigmaSlider_;
    case Field::BlurRadius:return ui.blurRadiusSlider_;
    case Field::Temporal:return ui.temporalSmoothCheckbox_;
    case Field::TemporalAlpha:return ui.temporalSmoothSlider_;
    case Field::Spatial:return ui.spatialSharpenCheckbox_;
    case Field::Upscaler:return ui.spatialBackendCombo_;
    case Field::Sharpness:return ui.spatialSharpnessSlider_;
    case Field::Stabilization:return ui.stabilizationCheckbox_;
    case Field::Keystone:return ui.keystoneCheckbox_;
    case Field::DebugView:return ui.debugButton_;
    case Field::TextClarity:return ui.textClarityCheckbox_;
    case Field::Flatten:return ui.backgroundFlattenCheckbox_;
    case Field::FlattenStrength:return ui.backgroundFlattenStrengthSlider_;
    case Field::Adaptive:return ui.adaptiveBinarizationCheckbox_;
    case Field::Sauvola:return ui.sauvolaStrengthSlider_;
    case Field::Softness:return ui.binarizationSoftnessSlider_;
    case Field::Polarity:return ui.textPolarityCombo_;
    case Field::Stroke:return ui.strokeWeightSlider_;
    case Field::SmartSharpen:return ui.smartSharpenCheckbox_;
    case Field::SmartStrength:return ui.smartSharpenStrengthSlider_;
    case Field::Clahe:return ui.claheCheckbox_;
    case Field::ClaheLimit:return ui.claheClipLimitSlider_;
    case Field::TwoColor:return ui.twoColorTextCheckbox_;
    case Field::Hysteresis:return ui.textHysteresisCheckbox_;
    case Field::HysteresisStrength:return ui.textHysteresisStrengthSlider_;
    case Field::SelectiveSharpen:return ui.selectiveSharpenCheckbox_;
    case Field::FocusDetection:return ui.focusDetectionCheckbox_;
    case Field::FocusThreshold:return ui.focusThresholdSlider_;
    case Field::Glare:return ui.glareSuppressionCheckbox_;
    case Field::GlareStrength:return ui.glareSuppressionStrengthSlider_;
    case Field::SuperResEnabled:return ui.mlTextSuperResolutionCheckbox_;
    case Field::SuperResStrength:return ui.mlTextSuperResolutionStrengthSlider_;
    case Field::SuperResPrefer2x:return ui.mlTextSuperResolutionPrefer2xCheckbox_;
    case Field::SuperResUltra1440p:return ui.mlTextSuperResolutionUltra1440pCheckbox_;
    case Field::Scheme:return ui.displayColorPicker_;
    case Field::CustomColorMode:return ui.displayColorPicker_;
    case Field::CustomColorStopCount:return ui.displayColorPicker_;
    case Field::CustomColorStops:return ui.displayColorPicker_;
    case Field::CustomColorStepped:return ui.displayColorPicker_;
    case Field::CustomColorRecipe:return ui.displayColorPicker_;
    case Field::BumpHold:return window?window->bumpHoldCheckbox():nullptr;
    default:return nullptr;
    }
}
int ComboIndex(const FieldWrite& write) {
    if(IsUctModel(write.field))return QStringList{"tiny","small","medium"}.indexOf(write.value.toString());
    if(write.field==Field::Framing)return write.value=="fit"?1:0;
    if(write.field==Field::PresentationRate)return QStringList{"auto-up-to-120","60","90","120","match-display"}.indexOf(write.value.toString());
    return write.value.toInt();
}
bool CanSetParameterWidget(QObject* widget,const FieldWrite& write) {
    if(!widget)return false;
    if(qobject_cast<QAbstractButton*>(widget))return write.value.isBool();
    if(auto* slider=qobject_cast<QSlider*>(widget)) {
        if(!write.value.isDouble() || !std::isfinite(write.value.toDouble()))return false;
        // Persisted zoom has a whole-source token below the aspect-dependent
        // display slider minimum. SetZoom applies it without a slider gesture.
        if(write.field==Field::ZoomAmount)return write.value.toDouble()>=double(kWholeSourceZoom) &&
            write.value.toDouble()<=app_constants::kZoomSliderMaxMultiplier;
        const auto steps=std::lround(write.value.toDouble()*Scale(write.field));
        return steps>=slider->minimum() && steps<=slider->maximum();
    }
    if(auto* combo=qobject_cast<QComboBox*>(widget)) {
        if(write.field==Field::Framing && (!write.value.isString() || (write.value!="fit" && write.value!="fill")))return false;
        if(write.field==Field::PresentationRate && !write.value.isString())return false;
        if(IsUctModel(write.field) && (!write.value.isString() || !uct::ParseModelTier(write.value.toString())))return false;
        if(write.field!=Field::Framing && write.field!=Field::PresentationRate && !IsUctModel(write.field) &&
            (!write.value.isDouble() || !std::isfinite(write.value.toDouble()) || std::floor(write.value.toDouble())!=write.value.toDouble()))return false;
        const int index=ComboIndex(write);return index>=0 && index<combo->count();
    }
    return false;
}
bool ValidRecipe(const QJsonValue& value) {
    if(!value.isObject())return false;
    const auto recipe=value.toObject();
    if(!recipe["stops"].isArray() || !recipe["schemeId"].isString() || !recipe["stopCount"].isDouble() ||
        !recipe["stepped"].isBool())return false;
    const auto stops=recipe["stops"].toArray();
    const auto selected=recipe["schemeId"].toString();
    if(selected!="custom" && !color_schemes::FindBuiltInColorScheme(selected))return false;
    const auto mode=recipe["mode"].toString();
    if(mode!="duotone" && mode!="posterize" && mode!="gradient")return false;
    if(stops.isEmpty())return selected!="custom" && recipe["stopCount"].toDouble()==0 && mode=="duotone" && !recipe["stepped"].toBool();
    if(stops.size()<2 || stops.size()>8 || recipe["stopCount"].toDouble()!=stops.size())return false;
    if(mode=="duotone" && stops.size()!=2)return false;
    if(mode=="posterize" && !recipe["stepped"].toBool())return false;
    for(const auto stop:stops)if(!stop.isString() || !QColor(stop.toString()).isValid())return false;
    return true;
}
bool SetParameterWidget(QObject* widget,const FieldWrite& write) {
    if(!CanSetParameterWidget(widget,write))return false;
    if(!widget)return false;
    const QSignalBlocker block(widget);
    if(auto* button=qobject_cast<QAbstractButton*>(widget)) {button->setChecked(write.value.toBool());return true;}
    if(auto* slider=qobject_cast<QSlider*>(widget)) {
        const int steps=int(std::lround(write.value.toDouble()*Scale(write.field)));
        if(steps<slider->minimum() || steps>slider->maximum())return false;
        slider->setValue(steps);return true;
    }
    if(auto* combo=qobject_cast<QComboBox*>(widget)) {
        const int index=ComboIndex(write);
        if(index<0 || index>=combo->count())return false;
        combo->setCurrentIndex(index);return true;
    }
    return false;
}
QString RateToken(settings::ViewportRateMode mode) {
    return QStringList{"auto-up-to-120","60","90","120","match-display"}.value(int(mode),"auto-up-to-120");
}
QJsonArray ColorStops(const color_schemes::ColorScheme& scheme) {
    QJsonArray result;for(const auto& color:scheme.stops)result.append(color.name(QColor::HexRgb));return result;
}
void AddRecipeFields(std::map<Field,QJsonValue>& fields,const color_schemes::ColorScheme& custom,const QString& selected) {
    fields[Field::CustomColorMode]=color_schemes::ModeToken(custom.mode);
    fields[Field::CustomColorStopCount]=int(custom.stops.size());
    fields[Field::CustomColorStops]=ColorStops(custom);
    fields[Field::CustomColorStepped]=custom.stepped;
    auto recipe=Recipe(fields,selected);
    recipe["name"]=custom.name;recipe["accessibleName"]=custom.accessibleName;
    recipe["textColorAtHighLuma"]=custom.textColorAtHighLuma;recipe["effect"]=custom.effect;
    fields[Field::CustomColorRecipe]=recipe;
}
}

std::map<Field,QJsonValue> AppControlTarget::ImageProfileFields(const settings::AdvancedConfig& config,
    const color_schemes::ColorScheme& custom) {
    auto fields=ProfileFields(config);
    fields[Field::DebugView]=config.debugView;
    fields[Field::SuperResEnabled]=config.mlSuperResEnabled;
    fields[Field::SuperResStrength]=double(config.mlSuperResStrength);
    fields[Field::SuperResPrefer2x]=config.mlSuperResPrefer2x;
    fields[Field::SuperResUltra1440p]=config.mlSuperResUltra1440p;
    AddRecipeFields(fields,custom,config.colorScheme.id);
    return fields;
}

std::map<Field,QJsonValue> AppControlTarget::ImageParameterFields() const {
    const auto& custom=app_.uiState_ && app_.uiState_->displayColorPicker_
        ? app_.uiState_->displayColorPicker_->customScheme():app_.settingsController_->Settings().customColorScheme;
    auto fields=ImageProfileFields(app_.CaptureCurrentAdvancedConfig(),custom);
    const auto models=app_.mainWindow_ && app_.mainWindow_->ultraClearTextPanel()
        ?app_.mainWindow_->ultraClearTextPanel()->ModelSettings():app_.settingsController_->Settings().ultraClearTextModels;
    fields[Field::UctGpuDetector]=uct::ModelTierToken(models.gpuDetector);
    fields[Field::UctGpuRecognizer]=uct::ModelTierToken(models.gpuRecognizer);
    fields[Field::UctCpuDetector]=uct::ModelTierToken(models.cpuDetector);
    fields[Field::UctCpuRecognizer]=uct::ModelTierToken(models.cpuRecognizer);
    fields[Field::Framing]=app_.pipelineOrchestrator_->ViewportFitMode()==settings::ViewportFitModeSetting::Fit?"fit":"fill";
    fields[Field::PresentationRate]=RateToken(app_.pipelineOrchestrator_->ViewportRateMode());
    fields[Field::VirtualJoystick]=app_.virtualJoystickEnabled_;
    fields[Field::ZoomWheelAcceleration]=app_.settingsController_->Settings().zoomWheelAcceleration;
    fields[Field::DebugView]=app_.debugViewEnabled_;
    fields[Field::SuperResEnabled]=app_.mlTextSuperResolutionEnabled_;
    fields[Field::SuperResStrength]=double(app_.mlTextSuperResolutionStrength_);
    fields[Field::SuperResPrefer2x]=app_.mlTextSuperResolutionPrefer2x_;
    fields[Field::SuperResUltra1440p]=app_.mlTextSuperResolutionUltra1440p_;
    fields[Field::SuperResPerformanceOverride]=app_.superResPerformanceOverride_;
    fields[Field::BumpHold]=app_.bumpHoldEnabled_;
    fields[Field::KeystonePaused]=app_.MediaViewSelected()
        ? app_.mediaGpuScene_ && app_.mediaGpuScene_->GetKeystoneTrackingState().paused
        : app_.cudaSurface_ && app_.cudaSurface_->GetKeystoneTrackingState().paused;
    return fields;
}

QJsonObject AppControlTarget::QueryImageParameters(ToolId tool,const QJsonObject& args) const {
    ParameterGroup group;bool setter=false;
    if(!ParameterGroupForTool(tool,group,setter) || (group!=ParameterGroup::Image && group!=ParameterGroup::View))
        return ControlResult::Failure("invalid_argument").content;
    const auto fields=ImageParameterFields();const auto filter=args["fields"].toArray();
    const auto& committed=app_.settingsController_->LastCommittedSettings();
    std::map<Field,QJsonValue> saved;
    if(committed) {
        saved=ImageProfileFields(committed->currentConfig,committed->customColorScheme);
        saved[Field::UctGpuDetector]=uct::ModelTierToken(committed->ultraClearTextModels.gpuDetector);
        saved[Field::UctGpuRecognizer]=uct::ModelTierToken(committed->ultraClearTextModels.gpuRecognizer);
        saved[Field::UctCpuDetector]=uct::ModelTierToken(committed->ultraClearTextModels.cpuDetector);
        saved[Field::UctCpuRecognizer]=uct::ModelTierToken(committed->ultraClearTextModels.cpuRecognizer);
        saved[Field::Framing]=committed->viewportFitMode==settings::ViewportFitModeSetting::Fit?"fit":"fill";
        saved[Field::PresentationRate]=RateToken(committed->viewportRateMode);
        saved[Field::VirtualJoystick]=committed->virtualJoystick;
        saved[Field::ZoomWheelAcceleration]=committed->zoomWheelAcceleration;
    }
    const bool media=app_.MediaViewSelected();
    const bool mediaGpu=media && app_.SelectedMediaGpuProcessingAvailable();
    const bool displayedGpu=media && app_.MediaGpuViewSelected();
    const bool cuda=media?mediaGpu:app_.cudaSurface_ && app_.cudaSurface_->IsValid() && app_.cudaPipelineAvailable_;
    const bool live=media?displayedGpu:app_.usingCudaLastFrame_ && app_.cameraActive_;
    const bool hasFrame=media?app_.SelectedVideoState()["hasFrame"].toBool():app_.cameraActive_;
    const bool master=(!media || displayedGpu) && Value(fields,Field::TextClarity).toBool();
    const bool adaptive=master && Value(fields,Field::Adaptive).toBool();
    const bool twoColor=adaptive && Value(fields,Field::TwoColor).toBool();
    auto effectiveFields=fields;
    // Dormant GPU text preferences cannot mask the CPU threshold/color recipe.
    if(media && !displayedGpu)effectiveFields[Field::TextClarity]=false;
    const auto enabled=[&](Field f){return Value(fields,f).toBool();};
    QJsonObject parameters;
    for(const auto& entry:kImageViewFields) {
        const auto* descriptor=FindParameter(entry.id);if(!descriptor || descriptor->group!=group)continue;
        const auto name=Token(entry.id);if(!filter.isEmpty() && !filter.contains(name))continue;
        const auto field=entry.field;const auto requested=WireValue(field,Value(fields,field));
        bool available=app_.uiState_ && app_.mainWindow_ &&
            (ParameterWidget(field,*app_.uiState_,app_.mainWindow_.get()) || field==Field::KeystonePaused || field==Field::SuperResPerformanceOverride);
        QString unsupported=available?QString():QString("native_control_unavailable");
        if(group==ParameterGroup::Image && media && !SelectedMediaImageFieldAvailable(field,mediaGpu)) {available=false;unsupported="selected_media_unsupported_effect";}
        else if(group==ParameterGroup::Image && !media && field!=Field::DebugView && !IsUctModel(field) && !cuda) {available=false;unsupported="cuda_unavailable";}
        if(field==Field::KeystonePaused && mediaGpu && (!app_.mediaGpuScene_ || !app_.mediaGpuScene_->SceneReady())) {
            available=false;unsupported="no_processed_frame";
        }
        if(IsSuperRes(field)) {
#if OKUFLOW_ENABLE_TEXT_SR
            if(!app_.mainWindow_ || !app_.mainWindow_->isMaxineRuntimeInstalled()) {available=false;unsupported="maxine_unavailable";}
#else
            available=false;unsupported="super_resolution_not_built";
#endif
        }
        bool active=live || (media && hasFrame && SelectedMediaImageFieldAvailable(field,false));
        QString inactive=active?QString():QString("no_processed_frame");
        bool intent=ImageParameterEffectiveIntent(field,effectiveFields);
        if(TextHandler(field) && !IsSuperRes(field) && field!=Field::TextClarity && !master)inactive="text_clarity_disabled";
        if(IsSuperRes(field)) {
            active=live && (media ? app_.mediaGpuScene_ && app_.mediaGpuScene_->IsSuperResActive()
                                  : app_.cudaSurface_ && app_.cudaSurface_->IsSuperResActive());
            intent=enabled(Field::SuperResEnabled);
            if(field==Field::SuperResPrefer2x || field==Field::SuperResUltra1440p || field==Field::SuperResPerformanceOverride)intent&=enabled(field);
            if(!active)inactive=(media ? app_.mediaGpuScene_ && app_.mediaGpuScene_->IsSuperResPerformanceLimited()
                                       : app_.cudaSurface_ && app_.cudaSurface_->IsSuperResPerformanceLimited())
                ?"performance_limit":"super_resolution_inactive";
        }
        if((field==Field::BlackWhite || field==Field::Threshold) && adaptive) {intent=false;inactive="adaptive_binarization";}
        if((field==Field::SmartSharpen || field==Field::SmartStrength) && adaptive) {intent=false;inactive="adaptive_binarization";}
        if(field==Field::SelectiveSharpen && master && !ImageParameterEffectiveIntent(Field::SmartSharpen,fields))
            inactive=adaptive?"adaptive_binarization":"smart_sharpen_disabled";
        if(master && !adaptive && !ImageParameterEffectiveIntent(Field::SelectiveSharpen,fields) &&
            (field==Field::Hysteresis || field==Field::HysteresisStrength || field==Field::Sauvola ||
             field==Field::Softness || field==Field::Polarity || field==Field::Stroke))inactive="no_consumed_mask";
        if(IsCustomComponent(field) && Value(fields,Field::Scheme)!="custom") {intent=false;inactive="custom_scheme_inactive";}
        if((field==Field::Scheme || IsCustomComponent(field)) && twoColor) {intent=false;inactive="two_color_text";}
        if(group==ParameterGroup::View || field==Field::DebugView) {
            active=app_.cameraActive_ || media;intent=requested.isBool()?requested.toBool():true;inactive=active?QString():QString("no_frame");
            if(field==Field::PresentationRate || field==Field::Framing || field==Field::VirtualJoystick || field==Field::ZoomWheelAcceleration)active=true;
            if(field==Field::FocusMarker && ((!media && app_.debugViewEnabled_) || Value(fields,Field::Framing)=="fit")) {intent=false;inactive="view_layout";}
        }
        if(IsUctModel(field)) {
            const auto* controller=app_.ultraClearText_.get();
            const auto backend=controller?controller->BackendStatusSnapshot():uct::BackendStatus{};
            const bool gpuField=field==Field::UctGpuDetector || field==Field::UctGpuRecognizer;
            active=controller && controller->IsEnabled() && backend.state==uct::BackendState::kReady &&
                backend.name.contains(gpuField?"TensorRT":"ONNX CPU");
            intent=true;inactive=active?QString():QString("uct_backend_inactive");
        }
        QJsonValue effective=requested.isBool()?QJsonValue(active && intent):active && intent?requested:QJsonValue(QJsonValue::Null);
        if(field==Field::ZoomEnabled && Value(fields,Field::Framing)=="fit") {effective=false;inactive="view_layout";}
        if((field==Field::CenterX || field==Field::CenterY) && Value(fields,Field::Framing)=="fit") {effective=0.5;inactive="view_layout";}
        if(field==Field::ZoomAmount)effective=app_.cameraActive_ || media?QJsonValue(app_.EffectiveZoomAmount()):QJsonValue(QJsonValue::Null);
        if(field==Field::PresentationRate)effective=app_.pipelineOrchestrator_->EffectiveViewportRate();
        if(media && group==ParameterGroup::Image && !SelectedMediaImageFieldAvailable(field,displayedGpu)) {
            effective=QJsonValue(QJsonValue::Null);inactive="selected_media_unsupported_effect";
            if(SelectedMediaImageFieldAvailable(field,mediaGpu))inactive="no_processed_frame";
        }
        if(!intent && inactive.isEmpty())inactive="disabled";
        const bool sessionOnly=descriptor->persistence=="session_only";
        QJsonObject item{{"requested",requested},{"effective",effective},{"saved",sessionOnly?QJsonValue(QJsonValue::Null):WireValue(field,Value(saved,field))},
            {"savedKnown",!sessionOnly && committed.has_value()},{"available",available},{"writable",available},
            {"unsupportedReason",unsupported},{"effectiveReason",inactive},{"persistence",Token(descriptor->persistence)},
            {"requiresUi",IsSuperRes(field)}};
        if(IsCustomComponent(field))item["requiresCompleteRecipe"]=true;
        parameters[name]=item;
    }
    return {{"parameters",parameters}};
}

ControlPlan AppControlTarget::PrepareImageParameters(ToolId tool,const QJsonObject& args) const {
    auto plan=PlanImageParameterDependencies(ImageParameterFields(),tool,args);
    if(!plan.error.isEmpty())return plan;
    ParameterGroup group;bool setter=false;ParameterGroupForTool(tool,group,setter);
    if(!setter) {
        const auto reply=QueryImageParameters(tool,args)["parameters"].toObject();
        plan.observedFields.clear();
        for(auto it=reply.begin();it!=reply.end();++it)if(const auto f=ImageParameterField(it.key().toStdString()))plan.observedFields.push_back(*f);
        return plan;
    }
    const auto fields=ImageParameterFields();const auto rows=QueryImageParameters(tool,{})["parameters"].toObject();
    for(auto it=args.begin();it!=args.end();++it) {
        const auto f=ImageParameterField(it.key().toStdString());
        if(app_.MediaViewSelected() && group==ParameterGroup::Image && f &&
           !SelectedMediaImageFieldAvailable(*f,app_.SelectedMediaGpuProcessingAvailable())) {
            plan.error="unavailable";plan.writes.clear();return plan;
        }
        const auto planned=f?std::find_if(plan.writes.begin(),plan.writes.end(),[&](const FieldWrite& w){return w.field==*f;}):plan.writes.end();
        const bool nativeWidget=f && app_.uiState_ && (ParameterWidget(*f,*app_.uiState_,app_.mainWindow_.get()) ||
            *f==Field::KeystonePaused || *f==Field::SuperResPerformanceOverride);
        if(planned!=plan.writes.end() && Value(fields,*f)!=planned->value &&
            (!rows[it.key()].toObject()["writable"].toBool() || !nativeWidget)) {
            plan.error="unavailable";plan.writes.clear();return plan;
        }
    }
    return plan;
}

bool AppControlTarget::ValidateImageParameterBatch(const ControlPlan& plan) const {
    if(!app_.uiState_ || !app_.mainWindow_)return false;
    const auto current=ImageParameterFields();auto desired=current;
    for(const auto& write:plan.writes)desired[write.field]=write.value;
    const auto imageRows=QueryImageParameters(ToolId::ImageSettingsGet,{})["parameters"].toObject();
    const auto viewRows=QueryImageParameters(ToolId::ViewSettingsGet,{})["parameters"].toObject();
    const auto recipe=std::find_if(plan.writes.begin(),plan.writes.end(),[](const FieldWrite& write){return write.field==Field::CustomColorRecipe;});
    if(recipe!=plan.writes.end()) {
        if(!ValidRecipe(recipe->value))return false;
        const auto value=recipe->value.toObject();
        for(const auto& write:plan.writes) {
            const char* key=nullptr;
            switch(write.field) {
            case Field::Scheme:key="schemeId";break;
            case Field::CustomColorMode:key="mode";break;
            case Field::CustomColorStopCount:key="stopCount";break;
            case Field::CustomColorStops:key="stops";break;
            case Field::CustomColorStepped:key="stepped";break;
            default:break;
            }
            // Even a currently unchanged component must agree with the recipe
            // commit, or staging would turn that explicit no-op into an effect.
            if(key && value[key]!=write.value)return false;
        }
    }
    // Read-only admission: every widget/type/range/device and recipe prerequisite
    // is checked before CommitFieldPlan can stage or call a native handler.
    for(const auto& write:plan.writes) {
        const bool imageOwned=write.field==Field::CustomColorRecipe || std::any_of(std::begin(kImageViewFields),std::end(kImageViewFields),
            [&](const ParameterField& f){return f.field==write.field;});
        if(imageOwned && Value(current,write.field)!=write.value &&
            !ParameterWidget(write.field,*app_.uiState_,app_.mainWindow_.get()) &&
            write.field!=Field::KeystonePaused && write.field!=Field::SuperResPerformanceOverride)return false;
        if(!imageOwned || Value(current,write.field)==write.value)continue;
        const auto descriptorField=std::find_if(std::begin(kImageViewFields),std::end(kImageViewFields),
            [&](const ParameterField& field){return field.field==write.field;});
        const auto id=descriptorField==std::end(kImageViewFields)?QString("image.colors.custom.mode"):Token(descriptorField->id);
        const auto row=imageRows.contains(id)?imageRows[id].toObject():viewRows[id].toObject();
        if(!row["writable"].toBool())return false;
        if(write.field==Field::CustomColorRecipe) {
            if(!app_.uiState_->displayColorPicker_ || !ValidRecipe(write.value))return false;
        } else if(write.field==Field::Scheme) {
            if(write.value!="custom" && !color_schemes::FindBuiltInColorScheme(write.value.toString()))return false;
            if(recipe!=plan.writes.end() && recipe->value.toObject()["schemeId"]!=write.value)return false;
            if(write.value=="custom" && recipe==plan.writes.end() && !app_.uiState_->displayColorPicker_->hasCustomScheme())return false;
        } else if(IsCustomComponent(write.field)) {
            if(recipe==plan.writes.end() || !ValidRecipe(recipe->value))return false;
            const auto value=recipe->value.toObject();
            const char* key=write.field==Field::CustomColorMode?"mode":write.field==Field::CustomColorStopCount?"stopCount":
                write.field==Field::CustomColorStops?"stops":"stepped";
            if(value[key]!=write.value)return false;
        } else if(write.field==Field::KeystonePaused || write.field==Field::SuperResPerformanceOverride) {
            if(!write.value.isBool() || (app_.MediaViewSelected()
                ? !app_.SelectedMediaGpuProcessingAvailable()
                : !app_.cudaSurface_ || !app_.cudaSurface_->IsValid()))return false;
            if(write.field==Field::KeystonePaused && write.value.toBool() && !Value(desired,Field::Keystone).toBool())return false;
            if(write.field==Field::SuperResPerformanceOverride && write.value.toBool() && !Value(desired,Field::SuperResEnabled).toBool())return false;
        } else if(!IsCustomComponent(write.field) && write.field!=Field::KeystonePaused && write.field!=Field::SuperResPerformanceOverride &&
            !CanSetParameterWidget(ParameterWidget(write.field,*app_.uiState_,app_.mainWindow_.get()),write))return false;
        if(write.field==Field::BumpHold && write.value.toBool() && !Value(desired,Field::Stabilization).toBool())return false;
    }
    return true;
}

bool AppControlTarget::ApplyImageParameterBatch(const ControlPlan& plan) {
    if(!ValidateImageParameterBatch(plan))return false;
    const auto current=ImageParameterFields();
    const bool textChanged=std::any_of(plan.writes.begin(),plan.writes.end(),[&](const FieldWrite& write){
        return TextHandler(write.field) && Value(current,write.field)!=write.value;
    });
    const QScopedValueRollback<bool> applying(applying_,true);
    if(textChanged) {
        for(const auto& write:plan.writes)if(TextHandler(write.field) && Value(current,write.field)!=write.value) {
            if(!SetParameterWidget(ParameterWidget(write.field,*app_.uiState_,app_.mainWindow_.get()),write))return false;
            if(write.field==Field::TextClarity && app_.uiState_->simpleTextClarityCheckbox_) {
                const QSignalBlocker block(app_.uiState_->simpleTextClarityCheckbox_);
                app_.uiState_->simpleTextClarityCheckbox_->setChecked(write.value.toBool());
            }
        }
        app_.OnTextClarityControlsChanged();
    }
    for(const auto& write:plan.writes)if(write.field==Field::CustomColorRecipe && Value(current,write.field)!=write.value) {
        const auto result=ApplyImageParameter(write);if(!result || !*result)return false;
    }
    return true;
}

std::optional<bool> AppControlTarget::ApplyImageParameter(const FieldWrite& write) {
    const bool owned=write.field==Field::CustomColorRecipe || std::any_of(std::begin(kImageViewFields),std::end(kImageViewFields),
        [&](const ParameterField& f){return f.field==write.field;});
    if(!owned)return {};
    if(!app_.uiState_ || !app_.mainWindow_)return false;
    const auto current=ImageParameterFields();
    if(Value(current,write.field)==write.value)return true;
    // The complete recipe/text group was applied by CommitFieldPlan. Component
    // entries remain independent revision/journal records, never partial edits.
    if(IsCustomComponent(write.field) || (fieldPlanActive_ && TextHandler(write.field)))return false;
    const QScopedValueRollback<bool> applying(applying_,true);
    if(write.field==Field::CustomColorRecipe) {
        const auto recipe=write.value.toObject();const auto stops=recipe["stops"].toArray();
        auto* picker=app_.uiState_->displayColorPicker_;if(!picker || !ValidRecipe(write.value))return false;
        const auto selected=recipe["schemeId"].toString();
        const auto* builtIn=color_schemes::FindBuiltInColorScheme(selected);
        if(selected!="custom" && !builtIn)return false;
        if(stops.isEmpty()) {
            if(selected=="custom")return false;
            picker->clearCustomScheme();app_.settingsController_->MutableSettings().customColorScheme={};
        } else {
            if(stops.size()<2 || stops.size()>8 || recipe["stopCount"].toInt()!=stops.size())return false;
            const auto mode=recipe["mode"].toString();if(mode!="duotone" && mode!="posterize" && mode!="gradient")return false;
            if(mode=="duotone" && stops.size()!=2)return false;
            color_schemes::ColorScheme scheme;
            scheme.id="custom";scheme.name=recipe["name"].toString("Custom colors");
            scheme.accessibleName=recipe["accessibleName"].toString("Custom display colors");
            scheme.mode=color_schemes::ModeFromToken(mode);scheme.stepped=recipe["stepped"].toBool();
            scheme.textColorAtHighLuma=recipe["textColorAtHighLuma"].toBool(true);scheme.effect=recipe["effect"].toBool();
            for(const auto stop:stops) {const QColor color(stop.toString());if(!color.isValid())return false;scheme.stops.push_back(color);}
            picker->setCustomScheme(scheme);
        }
        const QSignalBlocker block(picker);
        picker->setCurrentScheme(selected=="custom"?picker->customScheme():*builtIn);
        app_.OnDisplayColorSchemeChanged();return true;
    }
    if(write.field==Field::Scheme) {
        auto* picker=app_.uiState_->displayColorPicker_;if(!picker)return false;
        const QSignalBlocker block(picker);
        if(write.value=="custom") {
            if(!picker->hasCustomScheme())return false;picker->setCurrentScheme(picker->customScheme());
            app_.OnDisplayColorSchemeChanged();return true;
        }
        return app_.SetDisplayColorSchemeById(write.value.toString());
    }
    if(write.field==Field::Framing) {app_.SetViewportFitMode(write.value=="fit"?settings::ViewportFitModeSetting::Fit:settings::ViewportFitModeSetting::Fill,false);return true;}
    if(write.field==Field::ZoomAmount) {app_.SetZoom(float(write.value.toDouble()),{},false);return true;}
    if(write.field==Field::CenterX) {app_.SetZoomCenter(float(write.value.toDouble()),app_.zoomCenterY_,true,true,false);return true;}
    if(write.field==Field::CenterY) {app_.SetZoomCenter(app_.zoomCenterX_,float(write.value.toDouble()),true,true,false);return true;}
    if(write.field==Field::KeystonePaused) {
        if(app_.MediaViewSelected()) {
            if(!app_.SelectedMediaGpuProcessingAvailable() || !app_.mediaGpuScene_ || !app_.mediaGpuScene_->SceneReady())return false;
            app_.mediaGpuScene_->SetKeystoneTrackingPaused(write.value.toBool());
        } else {
            if(!app_.cudaSurface_)return false;
            app_.cudaSurface_->SetKeystoneTrackingPaused(write.value.toBool());
        }
        app_.UpdateKeystoneTrackingUi();return true;
    }
    if(write.field==Field::SuperResPerformanceOverride) {
        if(!app_.mlTextSuperResolutionEnabled_ || (app_.MediaViewSelected()
            ? !app_.SelectedMediaGpuProcessingAvailable() : !app_.cudaSurface_))return false;
        app_.mainWindow_->setSuperResPerformanceOverrideChecked(write.value.toBool());
        app_.SetSuperResPerformanceOverride(write.value.toBool());return true;
    }
    if(!SetParameterWidget(ParameterWidget(write.field,*app_.uiState_,app_.mainWindow_.get()),write))return false;
    if(IsUctModel(write.field)) {
        uctModelsPending_=true;
        if(!fieldPlanActive_)FinalizeUctModelParameters();
        return true;
    }
    const int steps=int(std::lround(write.value.toDouble()*Scale(write.field)));
    if(TextHandler(write.field)) {
        if(write.field==Field::TextClarity && app_.uiState_->simpleTextClarityCheckbox_) {
            const QSignalBlocker block(app_.uiState_->simpleTextClarityCheckbox_);
            app_.uiState_->simpleTextClarityCheckbox_->setChecked(write.value.toBool());
        }
        app_.OnTextClarityControlsChanged();return true;
    }
    switch(write.field) {
    case Field::ZoomEnabled:app_.OnZoomToggled(write.value.toBool());break;
    case Field::FocusMarker:app_.OnFocusMarkerToggled(write.value.toBool());break;
    case Field::BlackWhite:app_.OnBlackWhiteToggled(write.value.toBool());break;
    case Field::Threshold:app_.OnBlackWhiteThresholdChanged(steps);break;
    case Field::Contrast:app_.OnContrastChanged(steps);break;
    case Field::Brightness:app_.OnBrightnessChanged(steps);break;
    case Field::AutoContrast:app_.OnAutoContrastToggled(write.value.toBool());break;
    case Field::AutoContrastStrength:app_.OnAutoContrastStrengthChanged(steps);break;
    case Field::Blur:app_.OnBlurToggled(write.value.toBool());break;
    case Field::BlurSigma:app_.OnBlurSigmaChanged(steps);break;
    case Field::BlurRadius:app_.OnBlurRadiusChanged(steps);break;
    case Field::Temporal:app_.OnTemporalSmoothToggled(write.value.toBool());break;
    case Field::TemporalAlpha:app_.OnTemporalSmoothStrengthChanged(steps);break;
    case Field::Spatial:app_.OnSpatialSharpenToggled(write.value.toBool());break;
    case Field::Upscaler:app_.OnSpatialUpscalerChanged(write.value.toInt());break;
    case Field::Sharpness:app_.OnSpatialSharpnessChanged(steps);break;
    case Field::Stabilization:app_.OnStabilizationToggled(write.value.toBool());break;
    case Field::Keystone:app_.OnKeystoneToggled(write.value.toBool());break;
    case Field::BumpHold:app_.OnBumpHoldToggled(write.value.toBool());break;
    case Field::DebugView:app_.OnDebugViewToggled(write.value.toBool());break;
    case Field::VirtualJoystick:app_.OnVirtualJoystickToggled(write.value.toBool());break;
    case Field::ZoomWheelAcceleration:
        app_.settingsController_->MutableSettings().zoomWheelAcceleration=write.value.toBool();app_.UpdateSectionChangedCounts();break;
    case Field::PresentationRate: {
        const auto mode=static_cast<settings::ViewportRateMode>(app_.uiState_->viewportRateCombo_->currentIndex());
        app_.settingsController_->MutableSettings().viewportRateMode=mode;app_.pipelineOrchestrator_->SetViewportRateMode(mode);
        app_.UpdateSectionChangedCounts();break;
    }
    default:return false;
    }
    return true;
}
void AppControlTarget::FinalizeUctModelParameters() {
    if(!uctModelsPending_)return;
    uctModelsPending_=false;
    app_.OnUltraClearTextModelsChanged();
}
#endif // OKUFLOW_IMAGE_PARAMETER_PLANNER_ONLY
} // namespace okuflow::agent
