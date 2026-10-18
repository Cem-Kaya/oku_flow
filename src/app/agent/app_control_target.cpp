#ifdef _WIN32
#include "okuflow/app/agent/app_control_target.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/app.hpp"
#include "okuflow/app/constants.hpp"
#include "okuflow/app/interaction_controller.hpp"
#include "okuflow/ui/main_window.hpp"
#include "okuflow/ui/color_scheme_picker.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSlider>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QPointer>
#include <QUuid>
#include <chrono>
#include <array>
#include <cmath>

namespace okuflow::agent {
namespace {
qint64 MonotonicNow() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
QJsonObject RestrictedEffects(const settings::AdvancedConfig& config) {
    return {{"debug",config.debugView},{"sr",config.mlSuperResEnabled},{"strength",config.mlSuperResStrength},
        {"two",config.mlSuperResPrefer2x},{"ultra",config.mlSuperResUltra1440p}};
}
QString SafeLabel(QString text) {
    text.replace(QRegularExpression(QStringLiteral("[\\p{Cc}\\p{Cf}]")), QStringLiteral(" "));
    return text.left(80);
}
bool PresetAllowed(const settings::AdvancedConfig& before, const settings::AdvancedConfig& after) {
    // Every unsupported field difference is denied in both directions.
    const auto* scheme = color_schemes::FindBuiltInColorScheme(after.colorScheme.id);
    return ((scheme && color_schemes::SchemesEquivalent(*scheme,after.colorScheme)) || after.colorScheme.id=="custom")
        && before.vlmAssistEnabled == after.vlmAssistEnabled
        && before.assistiveOverlayEnabled == after.assistiveOverlayEnabled;
}
}
AppControlTarget::AppControlTarget(OkuFlowApp& app) : app_(app) {
    observed_ = Fields(); restrictedEffects_ = RestrictedEffects(app_.CaptureCurrentAdvancedConfig());
}
QString AppControlTarget::CameraHandle(const QString& link) const {
    if (link.isEmpty()) return {};
    if (!cameraHandles_.contains(link) && cameraHandles_.size() >= 256) return {};
    auto [entry,inserted] = cameraHandles_.try_emplace(link);
    if (inserted) entry->second = QStringLiteral("cam-")+QUuid::createUuid().toString(QUuid::Id128);
    return entry->second;
}
std::map<Field,QJsonValue> AppControlTarget::ProfileFields(const settings::AdvancedConfig& c) {
    return {{Field::ZoomEnabled,c.zoomEnabled},{Field::ZoomAmount,c.zoomAmount},{Field::CenterX,c.zoomCenterX},{Field::CenterY,c.zoomCenterY},
        {Field::BlackWhite,c.blackWhiteEnabled},{Field::Threshold,c.blackWhiteThreshold},{Field::Blur,c.blurEnabled},{Field::BlurSigma,c.blurSigma},
        {Field::BlurRadius,c.blurRadius},{Field::Temporal,c.temporalSmoothEnabled},{Field::TemporalAlpha,c.temporalSmoothAlpha},
        {Field::Spatial,c.spatialSharpenEnabled},{Field::Upscaler,c.spatialUpscaler},{Field::Sharpness,c.spatialSharpness},
        {Field::FocusMarker,c.focusMarker},{Field::Stabilization,c.stabilizationEnabled},{Field::Keystone,c.keystoneEnabled},
        {Field::Scheme,c.colorScheme.id},{Field::Contrast,c.contrast},{Field::Brightness,c.brightness},{Field::AutoContrast,c.autoContrastEnabled},
        {Field::AutoContrastStrength,c.autoContrastStrength},{Field::TextClarity,c.autoTextClarityEnabled},{Field::Flatten,c.backgroundFlattenEnabled},
        {Field::FlattenStrength,c.backgroundFlattenStrength},{Field::Adaptive,c.adaptiveBinarizationEnabled},{Field::Sauvola,c.sauvolaStrength},
        {Field::Softness,c.binarizationSoftness},{Field::Polarity,c.textPolarityMode},{Field::Stroke,c.strokeWeight},
        {Field::SmartSharpen,c.smartSharpenEnabled},{Field::SmartStrength,c.smartSharpenStrength},{Field::Clahe,c.claheEnabled},
        {Field::ClaheLimit,c.claheClipLimit},{Field::TwoColor,c.twoColorTextEnabled},{Field::Hysteresis,c.textHysteresisEnabled},
        {Field::HysteresisStrength,c.textHysteresisStrength},{Field::SelectiveSharpen,c.selectiveSharpenEnabled},{Field::FocusDetection,c.focusDetectionEnabled},
        {Field::FocusThreshold,c.focusThreshold},{Field::Glare,c.glareSuppressionEnabled},{Field::GlareStrength,c.glareSuppressionStrength}};
}
std::map<Field,QJsonValue> AppControlTarget::Fields() const {
    auto fields = ProfileFields(app_.CaptureCurrentAdvancedConfig());
    fields[Field::Rotation] = app_.rotationQuarterTurns_ * 90;
    fields[Field::Framing] = app_.pipelineOrchestrator_->ViewportFitMode() == settings::ViewportFitModeSetting::Fit ? "fit" : "fill";
    fields[Field::Preset] = app_.settingsController_->MutableSettings().selectedPresetId;
    fields[Field::Camera] = CameraHandle(app_.cameraIntent_.Requested().identity.symbolicLink);
    const auto assistant=AssistantParameterFields();fields.insert(assistant.begin(),assistant.end());
    const auto image=ImageParameterFields();fields.insert(image.begin(),image.end());
    const auto capture=CaptureParameterFields();fields.insert(capture.begin(),capture.end());
    const auto ui=UiParameterFields();fields.insert(ui.begin(),ui.end());
    const auto recording=RecordingParameterFields();fields.insert(recording.begin(),recording.end());
    return fields;
}
QJsonObject AppControlTarget::State() const {
    const auto f = Fields();
    const bool fit = f.at(Field::Framing) == "fit";
    const bool effects = app_.usingCudaLastFrame_;
    const bool binary = f.at(Field::TextClarity).toBool() && f.at(Field::Adaptive).toBool();
    const auto effect = [effects](const QJsonValue& requested,bool shadowed = false) {
        const bool enabled = !requested.isBool() || requested.toBool();
        const bool effective = effects && !shadowed && enabled;
        return QJsonObject{{"requested",requested},{"effective",effective},
            {"inactiveReason",effective ? QString() : !effects ? QStringLiteral("raw_passthrough")
                : !enabled ? QStringLiteral("disabled") : QStringLiteral("adaptive_binarization")}};
    };
    return {{"view",QJsonObject{{"framing",f.at(Field::Framing)},{"zoomRequested",f.at(Field::ZoomAmount)},
        {"zoomEffective",fit ? QJsonValue(1.0) : (f.at(Field::ZoomEnabled).toBool() ? f.at(Field::ZoomAmount) : QJsonValue(1.0))},
        {"center",QJsonObject{{"x",f.at(Field::CenterX)},{"y",f.at(Field::CenterY)}}}}},
        {"camera",QJsonObject{{"active",app_.cameraActive_},{"orientation",f.at(Field::Rotation)},
            {"requestedHandle",f.at(Field::Camera)},
            {"activeHandle",app_.cameraIntent_.Active() ? CameraHandle(app_.cameraIntent_.Active()->symbolicLink) : QString()},
            {"firstFrame",app_.cameraIntent_.Active() && app_.cameraIntent_.Active()->firstFrame},
            {"fallbackActive",app_.cameraIntent_.FallbackActive()},
            {"transitionPending",app_.cameraStartupPending_ || app_.cameraStopPending_ || app_.pendingCameraStart_.has_value()}}},
        {"image",QJsonObject{{"schemeId",f.at(Field::Scheme)},{"textClarity",f.at(Field::TextClarity)},{"contrast",f.at(Field::Contrast)},
            {"brightness",f.at(Field::Brightness)},{"monochrome",f.at(Field::BlackWhite)},
            {"readingColors",QJsonObject{{"scheme",effect(f.at(Field::Scheme))},{"textClarity",effect(f.at(Field::TextClarity))},
                {"adaptive",effect(binary)},
                {"twoColor",effect(f.at(Field::TwoColor),!binary)},
                {"blackWhite",effect(f.at(Field::BlackWhite),binary)}}}}},
        {"ui",QJsonObject{{"modal",ModalActive()},{"userInteracting",UserInteracting()},{"hidden",app_.mainWindow_->isUiHidden()}}},
        {"pipeline",app_.usingCudaLastFrame_ ? "cuda" : "raw_passthrough"},
        {"profile",QJsonObject{{"presetId",f.at(Field::Preset)}}}};
}
ControlPlan AppControlTarget::Prepare(ToolId tool, const QJsonObject& args, const QString& owner) const {
    ControlPlan plan;
    if(tool==ToolId::UiPreferencesGet || tool==ToolId::UiPreferencesSet || tool==ToolId::AnnotationPreferencesGet || tool==ToolId::AnnotationPreferencesSet)
        return PrepareUiParameters(tool,args);
    if (tool==ToolId::ImageSettingsGet || tool==ToolId::ImageSettingsSet || tool==ToolId::ViewSettingsGet || tool==ToolId::ViewSettingsSet)
        return PrepareImageParameters(tool,args);
    if(tool==ToolId::RecordingSettingsGet || tool==ToolId::RecordingSettingsSet)
        return PrepareRecordingParameters(tool,args,owner);
    if (tool==ToolId::CameraSettingsGet || tool==ToolId::CameraSettingsSet)
        return PrepareCaptureParameters(tool,args,owner);
    if (tool==ToolId::NotesPreferencesGet || tool==ToolId::NotesPreferencesSet) {
        auto assistantArgs=args, captureArgs=args;
        if(tool==ToolId::NotesPreferencesSet) {
            assistantArgs.remove("notes_output.root"); captureArgs.remove("notes_output.lectureNotes");
        }
        auto assistant=tool==ToolId::NotesPreferencesSet && assistantArgs.isEmpty()?ControlPlan{}:PrepareAssistantParameters(tool,assistantArgs);
        auto capture=tool==ToolId::NotesPreferencesSet && captureArgs.isEmpty()?ControlPlan{}:PrepareCaptureParameters(tool,captureArgs,owner);
        if(!assistant.error.isEmpty())return assistant;
        if(!capture.error.isEmpty())return capture;
        assistant.writes.insert(assistant.writes.end(),capture.writes.begin(),capture.writes.end());
        assistant.observedFields.insert(assistant.observedFields.end(),capture.observedFields.begin(),capture.observedFields.end());
        assistant.requiresApproval|=capture.requiresApproval;
        if(capture.approvalDomain!=ApprovalDomain::None)assistant.approvalDomain=capture.approvalDomain;
        if(!capture.targetIdentity.isEmpty())assistant.targetIdentity=capture.targetIdentity;
        return assistant;
    }
    if(tool==ToolId::AssistantPreferencesGet || tool==ToolId::AssistantPreferencesSet
        || tool==ToolId::NotesPreferencesGet || tool==ToolId::NotesPreferencesSet)
        return PrepareAssistantParameters(tool,args);
    if (tool != ToolId::ControlsList && static_cast<int>(tool) >= static_cast<int>(ToolId::ControlsList)) {
        plan.error = "unavailable"; return plan; // New finite groups require their native integration before admission.
    }
    const auto fields = Fields();
    const auto profilePlan = [&](const settings::AdvancedConfig& config) {
        auto requested=ImageProfileFields(config,config.colorScheme.id=="custom"?config.colorScheme:app_.settingsController_->Settings().customColorScheme);
        QJsonObject imageArgs,viewArgs;
        for(const auto& descriptor:ParameterRegistry()) {
            const auto field=ImageParameterField(descriptor.id);
            if(!field || !requested.contains(*field))continue;
            if(config.colorScheme.id!="custom" && (*field==Field::CustomColorMode || *field==Field::CustomColorStopCount || *field==Field::CustomColorStops || *field==Field::CustomColorStepped))continue;
            auto value=requested.at(*field);
            if(*field==Field::Upscaler)value=value.toInt()==0?"fsr1":"nis";
            if(*field==Field::Polarity)value=QStringList{"auto","dark-on-light","light-on-dark"}.value(value.toInt());
            const auto id=QString::fromUtf8(descriptor.id.data(),descriptor.id.size());
            (descriptor.group==ParameterGroup::Image?imageArgs:viewArgs)[id]=value;
        }
        auto result=PrepareImageParameters(ToolId::ImageSettingsSet,imageArgs);
        if(!result.error.isEmpty())return result;
        auto desired=fields;for(const auto& write:result.writes)desired[write.field]=write.value;
        auto view=PlanImageParameterDependencies(desired,ToolId::ViewSettingsSet,viewArgs);
        if(!view.error.isEmpty())return view;
        for(const auto& write:view.writes) {
            auto existing=std::find_if(result.writes.begin(),result.writes.end(),[&](const FieldWrite& item){return item.field==write.field;});
            if(existing==result.writes.end())result.writes.push_back(write);else existing->value=write.value;
        }
        result.resetsHistory|=view.resetsHistory;
        if(config.rotationQuarterTurns!=app_.rotationQuarterTurns_) {
            const auto rotation=Prepare(ToolId::Orientation,{{"degreesClockwise",config.rotationQuarterTurns*90}},owner);
            if(!rotation.error.isEmpty())return rotation;
            result.writes.insert(result.writes.begin(),FieldWrite{Field::Rotation,config.rotationQuarterTurns*90});
            for(const auto& binding:std::array<std::pair<Field,QString>,2>{{{Field::CenterX,"view.center.x"},{Field::CenterY,"view.center.y"}}}) {
                if(std::none_of(result.writes.begin(),result.writes.end(),[&](const FieldWrite& write){return write.field==binding.first;}))
                    result.writes.push_back({binding.first,double(float(std::lround(viewArgs[binding.second].toDouble()*100))/100)});
            }
            result.resetsHistory=true;
        }
        return result;
    };
    auto write = [&](Field field, QJsonValue value) {
        // UI-owned scalar members are floats. Normalize to that same value
        // before no-op comparison so repeated hundredth setters stay no-ops.
        if (value.isDouble() && field != Field::Rotation && field != Field::Polarity && field != Field::Stroke
            && field != Field::BlurRadius && field != Field::Upscaler) value = double(float(value.toDouble()));
        plan.writes.push_back({field,std::move(value)});
    };
    auto point = [&](const QJsonObject& p) {
        const auto resolved = observations_.Resolve(p,app_.controlService_->Stamp());
        if (!resolved) { plan.error = "stale_observation"; return; }
        write(Field::CenterX,resolved->x); write(Field::CenterY,resolved->y);
    };
    switch (tool) {
    case ToolId::Zoom: {
        write(Field::ZoomEnabled,true);
        const float minimum = app_.uiState_->zoomSlider_->minimum()/100.0f;
        plan=PrepareImageParameters(ToolId::ViewSettingsSet,{{"view.zoom.enabled",true},
            {"view.zoom.amount",std::max(args["magnification"].toDouble(),double(minimum))}});
        if(!plan.error.isEmpty())break;
        if (args.contains("anchor")) point(args["anchor"].toObject());
        break;
    }
    case ToolId::Center: point(args["point"].toObject()); break;
    case ToolId::ResetView:
        plan=PrepareImageParameters(ToolId::ViewSettingsSet,{{"view.zoom.enabled",true},
            {"view.zoom.amount",std::max(1.0,app_.uiState_->zoomSlider_ ? app_.uiState_->zoomSlider_->minimum()/100.0 : 1.0)},
            {"view.center.x",0.5},{"view.center.y",0.5}});break;
    case ToolId::Contrast: write(Field::Contrast,args["contrast"]); write(Field::Brightness,args["brightness"]); break;
    case ToolId::Orientation: {
        const int quarters = args["degreesClockwise"].toInt()/90;
        const int delta = (quarters-app_.rotationQuarterTurns_+4)%4;
        double x = app_.zoomCenterX_, y = app_.zoomCenterY_;
        for (int i = 0; i < delta; ++i) { const auto old = x; x = 1-y; y = old; }
        write(Field::Rotation,args["degreesClockwise"]); write(Field::CenterX,x); write(Field::CenterY,y);
        plan.resetsHistory = delta != 0; break;
    }
    case ToolId::ReadingColors: {
        const auto intent = args["intent"].toString();
        QString scheme = args["schemeId"].toString();
        if (intent == "photographic_color" || intent == "grayscale") {
            write(Field::TextClarity,false); write(Field::Adaptive,false); write(Field::TwoColor,false); write(Field::BlackWhite,false);
            scheme = intent == "grayscale" ? "grayscale" : "normal";
        } else if (intent.startsWith("high_contrast_")) {
            write(Field::TextClarity,true); write(Field::Adaptive,true); write(Field::TwoColor,true); write(Field::Polarity,0); write(Field::BlackWhite,false);
            scheme = intent == "high_contrast_dark_on_light" ? "black-white" : "white-black";
        }
        bool found = false;
        for (const auto& builtIn : color_schemes::BuiltInColorSchemes()) found |= builtIn.id == scheme;
        if (!found) plan.error = "not_found";
        else write(Field::Scheme,scheme);
        break;
    }
    case ToolId::ModeActivate: {
        const auto config = app_.settingsController_->ResolvePreset(args["presetId"].toString());
        if (!config) { plan.error = "not_found"; break; }
        if (!PresetAllowed(app_.CaptureCurrentAdvancedConfig(),*config)) { plan.error = "denied_by_policy"; break; }
        plan=profilePlan(*config);
        if(!plan.error.isEmpty())break;
        write(Field::Preset,args["presetId"]);
        break;
    }
    case ToolId::AutoAdjust: {
        if (!operations_.HasCapacity()) { plan.error = "budget_exhausted"; break; }
        const auto metrics = observations_.Metrics(app_.controlService_->Stamp());
        if (metrics["stale"].toBool()) { plan.error = "stale_state"; break; }
        if (!metrics["held"].isBool() || !metrics["converging"].isBool()) { plan.error = "unavailable"; break; }
        if (metrics["held"].toBool() || metrics["converging"].toBool()) { plan.error = "stale_state"; break; }
        if (!app_.usingCudaLastFrame_) { plan.error = "unavailable"; break; }
        const auto goal = args["goal"].toString();
        QString preset;
        if (args["allowModeChange"].toBool()) {
            if (goal == "projected_screen") preset = "preset-projector";
            else if (goal == "board") preset = "preset-whiteboard";
            else if (goal == "low_light") preset = "preset-low-light";
            // Viewport luminance may reflect an inverted output recipe. It
            // cannot determine ink polarity or the physical source material.
            else if (goal == "read_text") preset = "preset-document";
        }
        if (!preset.isEmpty()) {
            const auto config = app_.settingsController_->ResolvePreset(preset);
            if (!config || !PresetAllowed(app_.CaptureCurrentAdvancedConfig(),*config)) { plan.error = "denied_by_policy"; break; }
            plan=profilePlan(*config);
            if(!plan.error.isEmpty())break;
            write(Field::Preset,preset); plan.resetsHistory = true;
        } else {
            write(Field::AutoContrast,true); write(Field::Polarity,0);
            write(Field::TextClarity,goal != "see_scene");
            if (goal == "see_scene") {
                write(Field::Adaptive,false); write(Field::TwoColor,false); write(Field::BlackWhite,false);
            }
            if (goal == "projected_screen") write(Field::Keystone,true);
        }
        break;
    }
    case ToolId::Observe: case ToolId::Metrics:
        if (!app_.cameraActive_ || app_.debugViewEnabled_) plan.error = "unavailable";
        break;
    case ToolId::Photo:
        if (!app_.cameraActive_) plan.error = "unavailable";
        else if (!app_.photoOperationId_.isEmpty()) plan.error = "busy";
        break;
    case ToolId::RecordingStart:
        if (!app_.cameraActive_ || app_.recordingManager_->IsWorkerAbandoned()) plan.error = "unavailable";
        else if (RecordingActive() && RecordingOwner() != owner) plan.error = "already_active";
        break;
    case ToolId::RecordingStop: {
        QString requested = args["sessionId"].toString();
        const auto own = ownedRecordingByOwner_.find(owner);
        if (requested.isEmpty() && own != ownedRecordingByOwner_.end()) requested = own->second;
        if (requested.isEmpty()) { plan.error = "not_found"; break; }
        plan.targetIdentity = requested;
        if (RecordingSession() == requested) {
            plan.ownedRecording = RecordingOwner() == owner;
            const auto session = app_.recordingManager_->CurrentSessionInfo();
            if (session) plan.startedAt = QDateTime::fromString(session->timestampToken,
                QStringLiteral("yyyyMMdd_HHmmss_zzz")).toString(QStringLiteral("HH:mm"));
        }
        else {
            const auto operation = recordingOperations_.find(requested);
            const auto record = operation != recordingOperations_.end() ? operations_.Get(operation->second) : std::nullopt;
            if (record && record->owner == owner && (record->state == OperationState::Completed || record->state == OperationState::Failed)) {
                plan.ownedRecording = true; plan.terminalRecording = true;
            } else plan.error = "stale_state";
        }
        break;
    }
    case ToolId::CameraSources: break;
    case ToolId::CameraSelect: {
        if (!operations_.HasCapacity()) { plan.error = "budget_exhausted"; break; }
        if (RecordingActive() || app_.photoCapturePending_ || app_.pendingPhotoReadbackId_
            || !app_.photoOperationId_.isEmpty() || app_.cameraStartupPending_ || app_.cameraStopPending_
            || app_.cameraStopFailed_) { plan.error = "busy"; break; }
        const auto handle = args["cameraHandle"].toString();
        const auto camera = std::find_if(app_.cameras_.begin(),app_.cameras_.end(),[&](const CameraDescriptor& item) {
            return CameraHandle(QString::fromStdWString(item.symbolicLink)) == handle;
        });
        if (camera == app_.cameras_.end()) { plan.error = "not_found"; break; }
        if (!camera->phoneSerial.empty() && (camera->phoneCameraId.empty() || !camera->phoneAppReady)) {
            plan.error = "unavailable"; break;
        }
        const auto temporaryOwner = app_.cameraIntent_.TemporaryOwner();
        if (!temporaryOwner.isEmpty() && temporaryOwner != owner) { plan.error = "busy"; break; }
        plan.targetIdentity = handle; plan.targetDisplay = SafeLabel(QString::fromStdWString(camera->name));
        write(Field::Camera,handle); break;
    }
    default: break;
    }
    if (plan.error.isEmpty() && !app_.usingCudaLastFrame_) {
        const bool imageMutation = std::any_of(plan.writes.begin(),plan.writes.end(),[&fields](const FieldWrite& field) {
            const auto current = fields.find(field.field);
            if (current != fields.end() && current->second == field.value) return false;
            switch (field.field) {
            case Field::ZoomEnabled: case Field::ZoomAmount: case Field::CenterX: case Field::CenterY:
            case Field::Rotation: case Field::Framing: case Field::Preset: case Field::Camera: return false;
            default: return true;
            }
        });
        if (imageMutation) plan.error = "unavailable";
    }
    return plan;
}
bool AppControlTarget::BeginFieldPlan(const ControlPlan& plan) {
    if (fieldPlanActive_) return false;
    fieldPlanPreviousApplying_ = applying_; applying_ = true; fieldPlanActive_ = true;
    fieldPlanPreset_ = app_.settingsController_->MutableSettings().selectedPresetId;
    fieldPlanWritesPreset_ = std::any_of(plan.writes.begin(),plan.writes.end(),[](const FieldWrite& write) { return write.field==Field::Preset; });
    if (ValidateImageParameterBatch(plan)) return true;
    fieldPlanActive_=false;applying_=fieldPlanPreviousApplying_;return false;
}
bool AppControlTarget::CommitFieldPlan(const ControlPlan& plan) { return ApplyImageParameterBatch(plan); }
void AppControlTarget::EndFieldPlan() {
    if (!fieldPlanActive_) return;
    FinalizeUctModelParameters();
    if (!fieldPlanWritesPreset_) {
        app_.settingsController_->MutableSettings().selectedPresetId = fieldPlanPreset_;
        app_.RefreshPresetSelection(true); app_.UpdatePresetDescription();
    }
    app_.mainWindow_->RefreshControlReadouts();
    fieldPlanActive_ = false; applying_ = fieldPlanPreviousApplying_; observed_ = Fields();
}
bool AppControlTarget::Apply(const FieldWrite& write) {
    const auto current = Fields();
    if (!current.contains(write.field)) return false;
    if (!write.retryUnacknowledged && ReadField(write) == write.value) return true;
    if (const auto handled = ApplyRecordingParameter(write)) return *handled;
    if (const auto handled = ApplyUiParameter(write)) return *handled;
    if (const auto handled = ApplyCaptureParameter(write)) return *handled;
    if (const auto handled = ApplyImageParameter(write)) return *handled;
    auto& ui = *app_.uiState_;
    const auto savedApplying = applying_; applying_ = true;
    if(const auto assistant=ApplyAssistantParameter(write)) {
        applying_=savedApplying;observed_=Fields();return *assistant;
    }
    const auto previousPreset = app_.settingsController_->MutableSettings().selectedPresetId;
    auto check = [&](QCheckBox* widget) { if (!widget) return false; widget->setChecked(write.value.toBool()); return true; };
    auto slider = [&](QSlider* widget, double scale = 100) { if (!widget) return false; widget->setValue(static_cast<int>(std::lround(write.value.toDouble()*scale))); return true; };
    auto combo = [&](QComboBox* widget) { if (!widget) return false; widget->setCurrentIndex(write.value.toInt()); return true; };
    bool ok = true;
    switch (write.field) {
    case Field::ZoomEnabled: ok = check(ui.zoomCheckbox_); break;
    case Field::ZoomAmount: app_.SetZoom(write.value.toDouble(),{},false); break;
    case Field::CenterX: app_.SetZoomCenter(write.value.toDouble(),app_.zoomCenterY_,true,true,false); break;
    case Field::CenterY: app_.SetZoomCenter(app_.zoomCenterX_,write.value.toDouble(),true,true,false); break;
    case Field::Rotation: app_.SetRotationQuarterTurns(write.value.toInt()/90); break;
    case Field::BlackWhite: ok = check(ui.bwCheckbox_); break;
    case Field::Threshold: ok = slider(ui.bwSlider_,255); break;
    case Field::Blur: ok = check(ui.blurCheckbox_); break;
    case Field::BlurSigma: ok = slider(ui.blurSigmaSlider_,10); break;
    case Field::BlurRadius: ok = slider(ui.blurRadiusSlider_,1); break;
    case Field::Temporal: ok = check(ui.temporalSmoothCheckbox_); break;
    case Field::TemporalAlpha: ok = slider(ui.temporalSmoothSlider_); break;
    case Field::Spatial: ok = check(ui.spatialSharpenCheckbox_); break;
    case Field::Upscaler: ok = combo(ui.spatialBackendCombo_); break;
    case Field::Sharpness: ok = slider(ui.spatialSharpnessSlider_); break;
    case Field::FocusMarker: ok = check(ui.focusMarkerCheckbox_); break;
    case Field::Stabilization: ok = check(ui.stabilizationCheckbox_); break;
    case Field::Keystone: ok = check(ui.keystoneCheckbox_); break;
    case Field::Contrast: ok = slider(ui.contrastSlider_); break;
    case Field::Brightness: ok = slider(ui.brightnessSlider_); break;
    case Field::AutoContrast: ok = check(ui.autoContrastCheckbox_); break;
    case Field::AutoContrastStrength: ok = slider(ui.autoContrastStrengthSlider_); break;
    case Field::TextClarity: ok = check(ui.textClarityCheckbox_); break;
    case Field::Flatten: ok = check(ui.backgroundFlattenCheckbox_); break;
    case Field::FlattenStrength: ok = slider(ui.backgroundFlattenStrengthSlider_); break;
    case Field::Adaptive: ok = check(ui.adaptiveBinarizationCheckbox_); break;
    case Field::Sauvola: ok = slider(ui.sauvolaStrengthSlider_); break;
    case Field::Softness: ok = slider(ui.binarizationSoftnessSlider_); break;
    case Field::Polarity: ok = combo(ui.textPolarityCombo_); break;
    case Field::Stroke: ok = slider(ui.strokeWeightSlider_,1); break;
    case Field::SmartSharpen: ok = check(ui.smartSharpenCheckbox_); break;
    case Field::SmartStrength: ok = slider(ui.smartSharpenStrengthSlider_); break;
    case Field::Clahe: ok = check(ui.claheCheckbox_); break;
    case Field::ClaheLimit: ok = slider(ui.claheClipLimitSlider_,10); break;
    case Field::TwoColor: ok = check(ui.twoColorTextCheckbox_); break;
    case Field::Hysteresis: ok = check(ui.textHysteresisCheckbox_); break;
    case Field::HysteresisStrength: ok = slider(ui.textHysteresisStrengthSlider_); break;
    case Field::SelectiveSharpen: ok = check(ui.selectiveSharpenCheckbox_); break;
    case Field::FocusDetection: ok = check(ui.focusDetectionCheckbox_); break;
    case Field::FocusThreshold: ok = slider(ui.focusThresholdSlider_,1000); break;
    case Field::Glare: ok = check(ui.glareSuppressionCheckbox_); break;
    case Field::GlareStrength: ok = slider(ui.glareSuppressionStrengthSlider_); break;
    case Field::Scheme:
        ok = app_.SetDisplayColorSchemeById(write.value.toString()); break;
    case Field::Preset:
        app_.settingsController_->MutableSettings().selectedPresetId = write.value.toString();
        app_.RefreshPresetSelection(true); app_.UpdatePresetDescription(); break;
    default: ok = false; break;
    }
    applying_ = savedApplying; observed_ = Fields();
    if (write.field == Field::ZoomEnabled || write.field == Field::ZoomAmount || write.field == Field::CenterX
        || write.field == Field::CenterY || write.field == Field::Rotation) {
        app_.settingsController_->MutableSettings().selectedPresetId = previousPreset;
        app_.RefreshPresetSelection(true); app_.UpdatePresetDescription(); observed_ = Fields();
    }
    return ok;
}
bool AppControlTarget::Save() {
    // Persistence captures the current native state, including qualified preferences.
    const auto previous = applying_; applying_ = true;
    app_.SavePersistentSettings();
    applying_ = previous;
    assistantSettingsSaved_=app_.settingsController_->Save(app_.CaptureCurrentAdvancedConfig());
    return assistantSettingsSaved_;
}
QJsonValue AppControlTarget::ReadField(const FieldWrite& write) const {
    if(write.field==Field::CameraAcceleration && !write.targetIdentity.isEmpty()) {
        const auto camera=std::find_if(cameraHandles_.begin(),cameraHandles_.end(),[&](const auto& entry){return entry.second==write.targetIdentity;});
        if(camera==cameraHandles_.end())return QJsonValue(QJsonValue::Undefined);
        return QStringList{"automatic","accelerated","compatibility"}.value(static_cast<int>(app_.settingsController_->Settings().cameraAcceleration.value(camera->first).mode));
    }
    const auto fields=Fields();const auto found=fields.find(write.field);
    return found==fields.end()?QJsonValue(QJsonValue::Undefined):found->second;
}
void AppControlTarget::FinalizeMutationReceipt() {
    FinalizeUctModelParameters();
    if(outputRootPending_) { outputRootPending_=false;assistantParametersPending_=true;assistantRuntimeSettingsPending_=true; }
    FinalizeAssistantParameters();
}
QJsonObject AppControlTarget::EffectiveValues(const ControlPlan& plan) const {
    QJsonObject values;
    constexpr ToolId getters[]{ToolId::ImageSettingsGet,ToolId::ViewSettingsGet,ToolId::CameraSettingsGet,
        ToolId::RecordingSettingsGet,ToolId::AssistantPreferencesGet,ToolId::NotesPreferencesGet,
        ToolId::UiPreferencesGet,ToolId::AnnotationPreferencesGet};
    for(const auto& write:plan.writes) {
        const auto id=ParameterId(write.field);const auto key=id.toStdString();const auto* descriptor=FindParameter(key);
        if(!descriptor)continue;
        const auto row=Query(getters[static_cast<size_t>(descriptor->group)],{{"fields",QJsonArray{id}}})["parameters"].toObject()[id].toObject();
        if(!row.isEmpty())values[id]=row.contains("effective")?row["effective"]:QJsonValue();
        if(write.field==Field::CameraAcceleration && !write.targetIdentity.isEmpty() && write.targetIdentity!=Fields().at(Field::Camera).toString())values[id]=QJsonValue();
    }
    return values;
}
QJsonObject AppControlTarget::Query(ToolId tool, const QJsonObject& args) const {
    if(tool==ToolId::UiPreferencesGet || tool==ToolId::UiPreferencesSet || tool==ToolId::AnnotationPreferencesGet || tool==ToolId::AnnotationPreferencesSet)
        return QueryUiParameters(tool,args);
    if (tool==ToolId::ImageSettingsGet || tool==ToolId::ImageSettingsSet || tool==ToolId::ViewSettingsGet || tool==ToolId::ViewSettingsSet)
        return QueryImageParameters(tool,args);
    if(tool==ToolId::RecordingSettingsGet || tool==ToolId::RecordingSettingsSet)
        return QueryRecordingParameters(tool,args);
    if(tool==ToolId::CameraSettingsGet || tool==ToolId::CameraSettingsSet)
        return QueryCaptureParameters(tool,args);
    if(tool==ToolId::NotesPreferencesGet || tool==ToolId::NotesPreferencesSet) {
        auto result=QueryAssistantParameters(tool,args);
        auto parameters=result["parameters"].toObject();
        const auto capture=QueryCaptureParameters(tool,args)["parameters"].toObject();
        for(auto it=capture.begin();it!=capture.end();++it)parameters[it.key()]=it.value();
        result["parameters"]=parameters;return result;
    }
    if(tool==ToolId::AssistantPreferencesGet || tool==ToolId::AssistantPreferencesSet
        || tool==ToolId::NotesPreferencesGet || tool==ToolId::NotesPreferencesSet)
        return QueryAssistantParameters(tool,args);
    if (tool == ToolId::ControlsList) {
        const auto metadata = ParameterMetadataPage(args["group"].toString().toStdString(),
            args["afterField"].toString().toStdString(),static_cast<size_t>(args["limit"].toInt(16)),
            app_.controlService_->Edition());
        auto result=QJsonDocument::fromJson(QByteArray::fromStdString(metadata)).object();
        auto items=result["items"].toArray();
        static constexpr ToolId getters[]{ToolId::ImageSettingsGet,ToolId::ViewSettingsGet,ToolId::CameraSettingsGet,
            ToolId::RecordingSettingsGet,ToolId::AssistantPreferencesGet,ToolId::NotesPreferencesGet,ToolId::UiPreferencesGet,ToolId::AnnotationPreferencesGet};
        for(qsizetype index=0;index<items.size();++index) {
            auto item=items[index].toObject();const auto id=item["id"].toString();
            const auto* descriptor=FindParameter(id.toStdString());if(!descriptor)continue;
            const auto parameter=Query(getters[static_cast<size_t>(descriptor->group)],{{"fields",QJsonArray{id}}})["parameters"].toObject()[id].toObject();
            if(!parameter.isEmpty()) {
                item["implementation"]="production-native";
                item["acceptance"]="not-established-by-discovery";
                item["available"]=parameter["available"];
                item["writable"]=descriptor->access==ParameterAccess::WritableWithPolicy && parameter["writable"].toBool();
                item["unsupportedReason"]=parameter["unsupportedReason"];
            }
            items[index]=item;
        }
        result["items"]=items;return result;
    }
    if (tool == ToolId::CameraSources) {
        QJsonArray sources; int omitted = 0;
        for (const auto& camera : app_.cameras_) {
            const auto handle = CameraHandle(QString::fromStdWString(camera.symbolicLink));
            if (handle.isEmpty() || sources.size() >= 32) { ++omitted; continue; }
            sources.append(QJsonObject{{"cameraHandle",handle},{"displayName",SafeLabel(QString::fromStdWString(camera.name))},
                {"kind",camera.phoneSerial.empty() ? "webcam" : "usb-phone"},
                {"agentSelectable",!app_.cameraStopFailed_ && (camera.phoneSerial.empty() ||
                    (!camera.phoneCameraId.empty() && camera.phoneAppReady))},
                {"active",app_.cameraActive_ && app_.cameraIntent_.Active()
                    && app_.cameraIntent_.Active()->symbolicLink == QString::fromStdWString(camera.symbolicLink)}});
        }
        return {{"sources",sources},{"omittedCount",omitted}};
    }
    if (tool == ToolId::Metrics) return observations_.Metrics(app_.controlService_->Stamp());
    if (tool == ToolId::Wait) return {{"ok",true},{"met",false}};
    if (tool == ToolId::Operation) {
        const auto preferences=RecordingParameterOperationStatus(args["operationId"].toString());
        if(!preferences.isEmpty())return preferences;
        const auto record = operations_.Get(args["operationId"].toString());
        if (record && record->kind == OperationKind::Recording && RecordingSession() == record->sessionId) {
            const auto state = app_.recordingManager_->State();
            if (state == RecordingState::Recording) operations_.Advance(record->id,OperationState::Recording);
            else if (state == RecordingState::Stopping || state == RecordingState::Finalizing) operations_.Advance(record->id,OperationState::Stopping);
        }
        return operations_.SafeStatus(args["operationId"].toString());
    }
    if (tool == ToolId::Modes) {
        QJsonArray modes, colors; int omitted = 0;
        auto append = [&](const settings::PresetDefinition& preset) {
            if (!QRegularExpression("^[a-z0-9-]{1,64}$").match(preset.id).hasMatch()) { ++omitted; return; }
            const auto config = app_.settingsController_->ResolvePreset(preset.id);
            const bool applicable = config && PresetAllowed(app_.CaptureCurrentAdvancedConfig(),*config);
            modes.append(QJsonObject{{"id",preset.id},{"displayName",SafeLabel(preset.name)},{"agentApplicable",applicable},
                {"reason",applicable ? "" : "denied_by_policy"}});
        };
        for (const auto& preset : settings::BuiltInPresets()) append(preset);
        for (const auto& preset : app_.settingsController_->MutableSettings().customPresets) append(preset);
        for (const auto& scheme : color_schemes::BuiltInColorSchemes()) colors.append(QJsonObject{{"id",scheme.id},{"displayName",SafeLabel(scheme.name)}});
        return {{"modes",modes},{"colorSchemes",colors},{"omittedCount",omitted}};
    }
    return {{"ok",false},{"error",QJsonObject{{"code","unavailable"},{"retryable",false}}}};
}
void AppControlTarget::Execute(ToolId tool, const QJsonObject& args, const QString& owner, ContextKind kind, ControlCompletion completion) {
    if(tool==ToolId::CameraSettingsSet) {
        ExecuteCaptureParameters(args,owner,kind,std::move(completion));return;
    }
    if(tool==ToolId::RecordingSettingsSet) {
        ExecuteRecordingParameters(tool,args,owner,kind,std::move(completion));return;
    }
    if (tool == ToolId::AutoAdjust) {
        const auto operation = operations_.Create(OperationKind::Adjustment,owner);
        if (operation.isEmpty()) { completion(ControlResult::Failure("budget_exhausted")); return; }
        operations_.Advance(operation,OperationState::Starting);
        QPointer<OkuFlowApp> app(&app_);
        observations_.Request(false,768,app_.controlService_->Stamp(),[app,operation](ControlResult metrics) {
            if (!app || !app->controlTarget_) return;
            app->controlTarget_->operations_.Advance(operation,metrics.content["ok"].toBool() ? OperationState::FirstFrame : OperationState::Failed,
                {{"metrics",metrics.content["metrics"].toObject()}});
        });
        app_.pipelineOrchestrator_->MarkViewportDirty();
        completion({{{"ok",true},{"pending",true},{"operationId",operation},{"state","starting"}},{}}); return;
    }
    if (tool == ToolId::Observe || tool == ToolId::Metrics) {
        observations_.Request(tool == ToolId::Observe,args["maxLongSide"].toInt(768),app_.controlService_->Stamp(),std::move(completion),
            tool == ToolId::Metrics && args["_requireKnownTemporal"].toBool());
        app_.pipelineOrchestrator_->MarkViewportDirty(); return;
    }
    if (tool == ToolId::Wait) {
        const auto stamp = app_.controlService_->Stamp();
        QPointer<OkuFlowApp> app(&app_);
        auto* timer = new QTimer(&observations_); timer->setInterval(25);
        const auto deadline = MonotonicNow()+args["timeoutMs"].toInt();
        const auto operationId = args["operationId"].toString();
        const auto condition = args["condition"].toString();
        const auto initial = operationId.isEmpty() ? QJsonObject{} : Query(ToolId::Operation,{{"operationId",operationId}});
        QObject::connect(timer,&QTimer::timeout,timer,[app,timer,deadline,stamp,condition,operationId,initial,completion] {
            if (!app || !app->controlTarget_) { timer->stop(); timer->deleteLater(); completion(ControlResult::Failure("canceled")); return; }
            auto& target = *app->controlTarget_;
            bool met = condition == "camera_ready" ? app->cameraActive_ && app->cameraFramePresented_
                : condition == "view_presented" ? target.presented_ && target.presentedStamp_ == stamp
                : condition == "frame_processed" ? target.processed_ && target.processedStamp_.sourceEpoch == stamp.sourceEpoch
                    && target.processedStamp_.processingVersion == stamp.processingVersion
                : condition == "operation_state" ? target.Query(ToolId::Operation,{{"operationId",operationId}}) != initial : false;
            if (met || MonotonicNow() >= deadline) {
                timer->stop(); timer->deleteLater(); completion({{{"ok",true},{"met",met}},{}});
            }
        });
        timer->start(); return;
    }
    if (tool == ToolId::Photo) {
        const auto id = app_.RequestPhotoCapture(owner);
        if (id.isEmpty()) completion(ControlResult::Failure("busy"));
        else completion({{{"ok",true},{"pending",true},{"operationId",id},{"state","queued"}},{}});
        return;
    }
    if (tool == ToolId::RecordingStart) {
        if (RecordingActive()) {
            const auto op = recordingOperations_.find(RecordingSession());
            if (RecordingOwner() == owner && op != recordingOperations_.end()) {
                auto content = operations_.SafeStatus(op->second); content["changed"] = false; content["alreadyActive"] = true;
                completion({content,{}});
            } else completion(ControlResult::Failure("already_active"));
            return;
        }
        const auto audio = args["audio"] == "none" ? RecordingAudioRequest::None : RecordingAudioRequest::SavedMicrophone;
        const auto id = operations_.Create(OperationKind::Recording,owner);
        if (id.isEmpty()) { completion(ControlResult::Failure("budget_exhausted")); return; }
        const auto session = app_.RequestRecording(audio,kind == ContextKind::ExternalLease ? RecordingOwnerKind::ExternalLease : RecordingOwnerKind::AgentSession,owner);
        if (!session) { operations_.Advance(id,OperationState::Failed); completion(ControlResult::Failure("unavailable")); return; }
        operations_.SetSession(id,session->id);
        recordingOperations_[session->id] = id; ownedRecordingByOwner_[owner] = session->id;
        QJsonObject content{{"ok",true},{"pending",true},{"operationId",id},{"sessionId",session->id},{"state","starting"},
            {"ownerKind",kind == ContextKind::ExternalLease ? "external-lease" : "agent-session"},
            {"requestedAudio",audio == RecordingAudioRequest::None ? "none" : "saved_microphone"},{"actualAudio",session->actualAudio ? "microphone" : "none"}};
        completion({content,{}}); return;
    }
    if (tool == ToolId::RecordingStop) {
        const auto plan = Prepare(tool,args,owner);
        if (!plan.error.isEmpty()) { completion(ControlResult::Failure(plan.error)); return; }
        const auto op = recordingOperations_.find(plan.targetIdentity);
        if (plan.terminalRecording && op != recordingOperations_.end()) {
            auto content = operations_.SafeStatus(op->second); content["changed"] = false;
            completion({content,{}}); return;
        }
        // Exact approval may stop a manual session; it gets its own durable
        // operation but never adopts that session into a standing grant.
        QString operation = op != recordingOperations_.end() ? op->second : operations_.Create(OperationKind::Recording,owner,plan.targetIdentity);
        if (operation.isEmpty()) { completion(ControlResult::Failure("budget_exhausted")); return; }
        recordingOperations_[plan.targetIdentity] = operation;
        if (!app_.StopRecording(plan.targetIdentity)) { operations_.Advance(operation,OperationState::Failed); completion(ControlResult::Failure("stale_state")); return; }
        operations_.Advance(operation,OperationState::Stopping);
        completion({{{"ok",true},{"pending",true},{"operationId",operation},{"sessionId",plan.targetIdentity},{"state","stopping"}},{}});
        return;
    }
    if (tool == ToolId::CameraSelect) {
        const auto plan = Prepare(tool,args,owner);
        if (!plan.error.isEmpty()) { completion(ControlResult::Failure(plan.error)); return; }
        const auto camera = std::find_if(app_.cameras_.begin(),app_.cameras_.end(),[&](const CameraDescriptor& item) {
            return CameraHandle(QString::fromStdWString(item.symbolicLink)) == plan.targetIdentity;
        });
        if (camera == app_.cameras_.end()) { completion(ControlResult::Failure("not_found")); return; }
        const auto link = QString::fromStdWString(camera->symbolicLink);
        const auto format=args["_format"].toString();
        if (!args["_reopen"].toBool() && app_.cameraIntent_.Active() && app_.cameraIntent_.Active()->symbolicLink == link && app_.cameraActive_) {
            completion({{{"ok",true},{"changed",false},{"state","first_frame"}},{}}); return;
        }
        const auto operation = operations_.Create(OperationKind::Camera,owner);
        if (operation.isEmpty()) { completion(ControlResult::Failure("budget_exhausted")); return; }
        const auto beforeSource=Fields();
        if (!app_.cameraIntent_.RequestTemporary(owner,{link,format,false},operation)) {
            operations_.Advance(operation,OperationState::Failed); completion(ControlResult::Failure("busy")); return;
        }
        const auto generation = app_.cameraIntent_.Requested().generation;
        app_.controlService_->OwnedSourceRequested(owner,beforeSource); observed_ = Fields();
        if (app_.cameraIntent_.TemporaryOwner() != owner || app_.cameraIntent_.Requested().generation != generation) {
            operations_.Advance(operation,OperationState::Failed); completion(ControlResult::Failure("canceled")); return;
        }
        const auto requestedSource=CaptureParameterFields();
        operations_.Advance(operation,OperationState::Stopping,{{"requested",QJsonObject{{"camera.selection",CameraHandle(link)},
            {"camera.format",requestedSource.at(Field::CameraFormat)}}},{"applied",args["_preferences"]},
            {"undoPolicy","temporary-source-owner"},{"savedStartupPreserved",true}});
        const bool started = app_.StartCameraCapture(static_cast<size_t>(camera-app_.cameras_.begin()),false,false,true,
            CameraSourceOrigin::AgentTemporary,format,operation);
        if (!started) { CameraTransitionFailed(); completion(ControlResult::Failure("unavailable")); return; }
        auto* timer = new QTimer(&app_); timer->setInterval(50);
        QPointer<OkuFlowApp> app(&app_); const auto deadline = MonotonicNow()+30000;
        QObject::connect(timer,&QTimer::timeout,&app_,[this,app,timer,operation,generation,owner,deadline] {
            if (!app || app->controlTarget_.get() != this) { timer->deleteLater(); return; }
            const auto record = operations_.Get(operation);
            if (!record || record->state == OperationState::Failed || record->state == OperationState::FirstFrame) {
                timer->deleteLater(); return;
            }
            if (app->cameraIntent_.Requested().generation != generation || app->cameraIntent_.TemporaryOwner() != owner) {
                operations_.Advance(operation,OperationState::Failed,{{"errorCode","canceled"}}); timer->deleteLater(); return;
            }
            if (!app->cameraStopPending_ && (app->cameraStartupPending_ || app->cameraActive_))
                operations_.Advance(operation,OperationState::Starting);
            if (app->cameraIntent_.Active() && app->cameraIntent_.Active()->firstFrame && app->cameraActive_) {
                operations_.Advance(operation,OperationState::Starting);
                const auto parameters=QueryCaptureParameters(ToolId::CameraSettingsGet,{{"fields",QJsonArray{"camera.selection","camera.format","camera.acceleration"}}})["parameters"].toObject();
                QJsonObject effective;for(auto it=parameters.begin();it!=parameters.end();++it)effective[it.key()]=it.value().toObject()["effective"];
                operations_.Advance(operation,OperationState::FirstFrame,{{"fallbackActive",app->cameraIntent_.FallbackActive()},{"effective",effective}});
                timer->deleteLater(); return;
            }
            if (app->cameraStopFailed_ || MonotonicNow() >= deadline) { CameraTransitionFailed(); timer->deleteLater(); }
        });
        timer->start();
        const auto requested=CaptureParameterFields();
        completion({{{"ok",true},{"pending",true},{"operationId",operation},{"state","stopping"},
            {"requested",QJsonObject{{"camera.selection",CameraHandle(link)},{"camera.format",requested.at(Field::CameraFormat)}}},
            {"undoPolicy","temporary-source-owner"},{"savedStartupPreserved",true}},{}}); return;
    }
    completion(ControlResult::Failure("unavailable"));
}
void AppControlTarget::CameraTransitionFailed() {
    const auto owner = app_.cameraIntent_.TemporaryOwner();
    const auto operation = app_.cameraIntent_.Requested().operationId;
    if (owner.isEmpty() || operation.isEmpty()) return;
    operations_.Advance(operation,OperationState::Failed,{{"errorCode","capture_failed"},{"fallbackActive",false},{"fallbackRequested",true}});
    app_.CancelCameraTransition(owner,true); observed_ = Fields();
}
bool AppControlTarget::ModalActive() const { return QApplication::activeModalWidget() != nullptr; }
bool AppControlTarget::UserInteracting() const {
    const auto* focus = QApplication::focusWidget();
    const auto* slider = qobject_cast<const QSlider*>(focus);
    return app_.IsMousePanActive() || (app_.interactionController_ && app_.interactionController_->HasContinuousMotion())
        || QApplication::activePopupWidget() || (app_.mainWindow_ && app_.mainWindow_->isModeGridOpen())
        || (slider && slider->isSliderDown());
}
bool AppControlTarget::RecordingActive() const { return app_.recordingManager_ && app_.recordingManager_->CurrentSessionInfo().has_value(); }
QString AppControlTarget::RecordingSession() const {
    const auto info = app_.recordingManager_ ? app_.recordingManager_->CurrentSessionInfo() : std::nullopt;
    return info ? info->id : QString();
}
QString AppControlTarget::RecordingOwner() const {
    const auto info = app_.recordingManager_ ? app_.recordingManager_->CurrentSessionInfo() : std::nullopt;
    return info && info->ownerKind != RecordingOwnerKind::Manual ? info->ownerId : QStringLiteral("manual");
}
void AppControlTarget::StopOwnedRecording(const QString& owner) {
    if (RecordingOwner() == owner) {
        const auto session = RecordingSession();
        if (app_.StopRecording(session)) {
            const auto operation = recordingOperations_.find(session);
            if (operation != recordingOperations_.end()) operations_.Advance(operation->second,OperationState::Stopping);
        }
    }
}
void AppControlTarget::CancelOwnedSource(const QString& owner) {
    if(torchOwner_==owner && torchCanceled_)torchCanceled_->store(true);
    if(!HasOwnedSource(owner))return;
    // Record genuine native edits first; the owned restore itself is not one.
    NoticeNativeChanges();
    const auto savedApplying=applying_;applying_=true;
    app_.CancelCameraTransition(owner);
    applying_=savedApplying;observed_=Fields();
}
bool AppControlTarget::HasOwnedSource(const QString& owner) const {return !owner.isEmpty() && app_.cameraIntent_.TemporaryOwner()==owner;}
QString AppControlTarget::CreatePhoto(const QString& owner) { return operations_.Create(OperationKind::Photo,owner); }
void AppControlTarget::PhotoState(const QString& id, OperationState state, QJsonObject summary) {
    if (id.isEmpty()) return;
    operations_.Advance(id,state,std::move(summary));
    if ((state == OperationState::Committed || state == OperationState::Failed) && app_.photoOperationId_ == id) app_.photoOperationId_.clear();
}
void AppControlTarget::CancelPhotoReadback() {
    const auto operation = operations_.Get(app_.photoOperationId_);
    if (operation && (operation->state == OperationState::Queued || operation->state == OperationState::Readback)) {
        PhotoState(operation->id,OperationState::Failed);
        app_.photoCapturePending_ = false; app_.pendingPhotoReadbackId_ = 0; app_.pendingPhotoOriginal_ = {};
    }
}
void AppControlTarget::RecordingEnded(const RecordingSessionInfo& session) {
    const auto operation = recordingOperations_.find(session.id);
    if (operation == recordingOperations_.end()) return;
    if (session.completion == RecordingSessionCompletion::Completed) {
        operations_.Advance(operation->second,OperationState::Stopping);
        operations_.Advance(operation->second,OperationState::Completed);
    } else operations_.Advance(operation->second,OperationState::Failed,{{"errorCode",session.completion == RecordingSessionCompletion::Abandoned ? "worker_abandoned" : "capture_failed"}});
}
void AppControlTarget::NoticeNativeChanges() {
    const auto current = Fields();
    if (!applying_ && app_.controlService_) {
        std::vector<Field> changed;
        for (const auto& [field,value] : current) if (!observed_.contains(field) || observed_.at(field) != value) changed.push_back(field);
        app_.controlService_->NativeFieldsChanged(changed);
        const auto restricted = RestrictedEffects(app_.CaptureCurrentAdvancedConfig());
        if (restricted != restrictedEffects_) app_.controlService_->InvalidateGeometry(true);
        restrictedEffects_ = restricted;
    }
    observed_ = current;
}
void AppControlTarget::NoteNativeCameraAcceleration(const QString& symbolicLink) {
    if(!applying_ && app_.controlService_)app_.controlService_->NativeCameraAccelerationChanged(CameraHandle(symbolicLink));
    const auto current=Fields();
    if(current.contains(Field::CameraAcceleration))observed_[Field::CameraAcceleration]=current.at(Field::CameraAcceleration);
}
void AppControlTarget::FrameProcessed() {
    if (!app_.controlService_) return;
    const auto stamp = app_.controlService_->Stamp();
    const auto now = MonotonicNow();
    const bool newSource = !processedInputSeen_ || processedCameraSession_ != app_.cameraSessionId_;
    if (newSource) uniqueFrameTimes_.clear();
    if (newSource || frameSequence_ != app_.currentCameraFrameSequence_) {
        processedAt_ = now; uniqueFrameTimes_.push_back(now);
        while (uniqueFrameTimes_.size() > 120 || (!uniqueFrameTimes_.empty() && now-uniqueFrameTimes_.front() > 2000))
            uniqueFrameTimes_.pop_front();
    }
    processedStamp_ = stamp;
    frameSequence_ = app_.currentCameraFrameSequence_;
    processedCameraSession_=app_.cameraSessionId_;processedInputSeen_=true;
    processed_ = true;
}
void AppControlTarget::FramePresented(const ObservationFrame& frame) { presentedStamp_ = frame.stamp; presented_ = true; }
ObservationFrame AppControlTarget::PresentedFrame(const ViewTransform& transform, QSize source, QSize viewport) {
    if (lastViewport_.isValid() && lastViewport_ != viewport && app_.controlService_) {
        app_.controlService_->InvalidateGeometry(); observations_.Cancel("stale_observation"); observationReadbackId_ = 0;
    }
    lastViewport_ = viewport;
    const auto current = app_.controlService_ ? app_.controlService_->Stamp() : ControlStamp{};
    auto stamp = processedStamp_; stamp.viewVersion = current.viewVersion; stamp.geometryEpoch = current.geometryEpoch;
    std::optional<bool> held = false, converging = false, quad = false;
    QJsonObject diagnostics{{"available",true},{"basis","cpu-output"}};
    if (app_.usingCudaLastFrame_) {
        held.reset(); converging.reset(); quad.reset();
        const auto sample = app_.cudaSurface_->ControlDiagnostics();
        const auto& identity = sample.frame;
        const auto now = MonotonicNow();
        const bool matches = identity.sourceEpoch == stamp.sourceEpoch && identity.processingVersion == stamp.processingVersion
            && identity.viewVersion == stamp.viewVersion && identity.geometryEpoch == stamp.geometryEpoch
            && identity.frameSequence == frameSequence_;
        const bool fresh = identity.submittedAtMs > 0 && now >= identity.submittedAtMs
            && now-identity.submittedAtMs <= ObservationService::MaximumFreshFrameAgeMs;
        diagnostics = {{"available",matches && fresh},{"basis","completed-gpu-diagnostic"},
            {"matchesFrame",matches},{"frameSequence",static_cast<qint64>(identity.frameSequence)},
            {"sourceEpoch",static_cast<qint64>(identity.sourceEpoch)},
            {"processingVersion",static_cast<qint64>(identity.processingVersion)},
            {"viewVersion",static_cast<qint64>(identity.viewVersion)},
            {"geometryEpoch",static_cast<qint64>(identity.geometryEpoch)},
            {"sampleAgeMs",identity.submittedAtMs > 0 ? QJsonValue(now-identity.submittedAtMs) : QJsonValue()},
            {"stabilizationModelValid",matches && fresh ? QJsonValue(sample.stabilizationModelValid) : QJsonValue()}};
        if (matches && fresh) { held = sample.held; converging = sample.converging; quad = sample.keystoneQuadValid; }
    }
    const auto elapsed = uniqueFrameTimes_.size() > 1 ? uniqueFrameTimes_.back()-uniqueFrameTimes_.front() : 0;
    const double fps = elapsed > 0 && MonotonicNow()-uniqueFrameTimes_.back() <= 1000
        ? (uniqueFrameTimes_.size()-1)*1000.0/elapsed : 0.0;
    return {stamp,transform,source,viewport,frameSequence_,processedAt_,held,converging,fps,app_.focusThreshold_,quad,diagnostics};
}
bool AppControlTarget::WantsObservationReadback(const ObservationFrame& frame) {
    if (observationReadbackId_ && MonotonicNow()-observationSubmittedAt_ > 1000) observationReadbackId_ = 0;
    return observationReadbackId_ == 0 && observations_.WantsReadback(frame);
}
void AppControlTarget::ObservationReadbackSubmitted(quint64 id, const ObservationFrame& frame) {
    if (!id) return;
    observationReadbackId_ = id; readbackFrame_ = frame; observationSubmittedAt_ = MonotonicNow();
}
void AppControlTarget::ConsumeObservationReadback(quint64 id, const QImage& pixels) {
    if (!id || id != observationReadbackId_) return;
    observationReadbackId_ = 0; observations_.Consume(pixels,readbackFrame_);
}
void AppControlTarget::CpuObservationPresented(const QImage& pixels, const ObservationFrame& frame) {
    FramePresented(frame); observations_.Consume(pixels,frame);
}
void AppControlTarget::CancelObservations() {
    if(torchCanceled_)torchCanceled_->store(true);
    observationReadbackId_ = 0; processed_ = presented_ = false; observations_.Cancel();
}
} // namespace okuflow::agent
#endif
