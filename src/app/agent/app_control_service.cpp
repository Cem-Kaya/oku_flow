#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/app/agent/control_schema.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include <QCryptographicHash>
#include <chrono>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace okuflow::agent {
namespace {
QString Id() { return QUuid::createUuid().toString(QUuid::Id128); }
QByteArray Digest(ToolId tool, const QJsonObject& args) {
    return QCryptographicHash::hash(QByteArray::number(static_cast<int>(tool)) + ':'
        + QJsonDocument(args).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
bool Matches(const QJsonValue& value, const QJsonObject& schema) {
    return MatchesControlSchema(value,schema);
}
bool IsView(Field field) { return field == Field::ZoomEnabled || field == Field::ZoomAmount
    || field == Field::CenterX || field == Field::CenterY || field == Field::Framing; }
QString ResultFieldName(ToolId tool, Field field) {
    if (static_cast<int>(tool)<static_cast<int>(ToolId::ControlsList)) return FieldName(field);
    static constexpr const char* ids[]{"view.zoom.enabled","view.zoom.amount","view.center.x","view.center.y","view.fit","camera.orientation","camera.selection",
        "image.blackWhite.enabled","image.blackWhite.threshold","image.blur.enabled","image.blur.sigma","image.blur.radius",
        "image.temporal.enabled","image.temporal.alpha","image.spatial.enabled","image.spatial.backend","image.spatial.sharpness",
        "view.focusMarker","image.stabilization.enabled","image.keystone.enabled","image.colors.schemeId","image.contrast","image.brightness",
        "image.autoContrast.enabled","image.autoContrast.strength","image.textClarity.enabled","image.textClarity.flatten.enabled","image.textClarity.flatten.strength",
        "image.textClarity.adaptive.enabled","image.textClarity.adaptive.sauvolaStrength","image.textClarity.adaptive.softness","image.textClarity.polarity",
        "image.textClarity.strokeWeight","image.textClarity.smartSharpen.enabled","image.textClarity.smartSharpen.strength","image.textClarity.clahe.enabled",
        "image.textClarity.clahe.clipLimit","image.textClarity.twoColor","image.textClarity.hysteresis.enabled","image.textClarity.hysteresis.strength",
        "image.textClarity.selectiveSharpen","image.textClarity.focusDetection.enabled","image.textClarity.focusDetection.threshold","image.textClarity.glare.enabled",
        "image.textClarity.glare.strength"};
    const auto index=static_cast<size_t>(field);
    if(index<std::size(ids))return QString::fromLatin1(ids[index]);
    if(field==Field::CustomColorRecipe)return {};
    if(field==Field::DebugView)return "image.debugView";
    if(field==Field::SuperResEnabled)return "image.superResolution.enabled";
    if(field==Field::SuperResStrength)return "image.superResolution.strength";
    if(field==Field::SuperResPrefer2x)return "image.superResolution.prefer2x";
    if(field==Field::SuperResUltra1440p)return "image.superResolution.ultra1440p";
    if(field==Field::SuperResPerformanceOverride)return "image.superResolution.performanceOverride";
    return FieldName(field);
}
QJsonValue ResultFieldValue(ToolId tool, Field field, QJsonValue value) {
    if (static_cast<int>(tool)<static_cast<int>(ToolId::ControlsList)) return value;
    if (field==Field::Upscaler) return value.toInt()==0 ? QJsonValue("fsr1") : QJsonValue("nis");
    if (field==Field::Polarity) return QStringList{"auto","dark-on-light","light-on-dark"}.value(value.toInt());
    return value;
}
bool IsFlash(Field field) { return field == Field::Scheme || field == Field::Contrast || field == Field::Brightness; }
void DeriveParameterPolicy(ControlPlan& plan) {
    for(const auto& write:plan.writes) {
        const auto id=ResultFieldName(ToolId::ControlsList,write.field).toStdString();
        const auto* descriptor=FindParameter(id);if(!descriptor)continue;
        if(descriptor->requiresUiApproval) {
            plan.requiresApproval=true;
            if(descriptor->permission!=ParameterPermission::Source)plan.approvalDomain=ApprovalDomain::Configuration;
        }
        if(descriptor->permission==ParameterPermission::FrameEgress && write.value.toBool())plan.requiresFrames=true;
        if(descriptor->permission==ParameterPermission::PhotoEgress && write.value.toBool())plan.requiresPhotos=true;
        if(descriptor->permission==ParameterPermission::CaptureConfiguration && plan.terminalRecording)plan.requiresRecording=true;
    }
}
bool IsAsync(ToolId tool) { return tool == ToolId::Observe || tool == ToolId::Metrics || tool == ToolId::Wait || tool == ToolId::Photo
    || tool == ToolId::RecordingStart || tool == ToolId::RecordingStop || tool == ToolId::CameraSelect; }
}
QString FieldName(Field field) {
    static constexpr const char* names[]{"zoomEnabled","zoomAmount","centerX","centerY","framing","rotation","camera",
        "blackWhite","threshold","blur","blurSigma","blurRadius","temporal","temporalAlpha","spatial","upscaler","sharpness",
        "focusMarker","stabilization","keystone","scheme","contrast","brightness","autoContrast","autoContrastStrength","textClarity",
        "flatten","flattenStrength","adaptive","sauvola","softness","polarity","stroke","smartSharpen","smartStrength","clahe",
        "claheLimit","twoColor","hysteresis","hysteresisStrength","selectiveSharpen","focusDetection","focusThreshold","glare",
        "glareStrength","preset","presetList","recording",
        "assistant_preferences.sceneExplain","assistant_preferences.overlayEnabled","assistant_preferences.provider",
        "assistant_preferences.codexModel","assistant_preferences.codexReasoning","assistant_preferences.httpModel",
        "assistant_preferences.instructions","assistant_preferences.explainPrompt","assistant_preferences.ttsVoice",
        "assistant_preferences.ttsRate","notes_output.lectureNotes",
        "image.debug","image.superRes.enabled","image.superRes.strength","image.superRes.prefer2x",
        "image.superRes.ultra1440p","image.superRes.performanceOverride","image.stabilization.bumpHold",
        "image.keystone.paused","image.colors.custom.mode","image.colors.custom.stopCount",
        "image.colors.custom.stops","image.colors.custom.stepped","customColorRecipe",
        "view.presentationRate","view.virtualJoystick","view.zoomWheelAcceleration",
        "ui.mode","ui.inspectorWidth","ui.language","ui.hidden","ui.sectionExpanded","ui.assistiveDock","ui.assistiveGeometry",
        "ui.setupPromptDeclined","ui.activeTab","annotation.color","annotation.width","annotation.textSize","annotation.dashed",
        "annotation.shape","annotation.captureOnExit","annotation.tool","annotation.enabled",
        "camera.format","camera.acceleration","camera.phoneTorch","recording.microphone","recording.canvas","notes_output.root"};
    const auto index = static_cast<std::size_t>(field);
    return index < std::size(names) ? QString::fromLatin1(names[index]) : QString();
}
QString ParameterId(Field field) { return ResultFieldName(ToolId::ControlsList,field); }
ControlResult ControlResult::Failure(const QString& code, bool retryable) {
    static const QStringList allowed{"invalid_argument","out_of_range","unavailable","busy","user_interacting","modal_active",
        "consent_required","consent_denied","denied_by_policy","not_found","already_active","stale_state","stale_observation",
        "budget_exhausted","context_expired","canceled","timeout","internal"};
    return {{{"ok", false}, {"error", QJsonObject{{"code",allowed.contains(code) ? code : QStringLiteral("internal")},{"retryable",retryable}}}}, {}};
}
qint64 AppControlService::Now() const {
    return clock_ ? clock_() : std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
AppControlService::AppControlService(IAppControlTarget& target, CatalogEdition edition, QObject* parent, std::function<qint64()> clock)
    : QObject(parent), target_(target), edition_(edition), clock_(std::move(clock)) {
    connect(&expiryTimer_, &QTimer::timeout, this, [this] { Expire(); });
    expiryTimer_.start(250);
}
void AppControlService::SetEnabled(bool embedded, bool external) {
    Q_ASSERT(QThread::currentThread() == thread());
    embeddedEnabled_ = embedded; externalEnabled_ = external;
    if (!embedded) nextFrameOwner_.clear();
    if (context_ && ((context_->spec.kind == ContextKind::EmbeddedTurn && !embedded)
        || (context_->spec.kind == ContextKind::ExternalLease && !external))) RetireOwner(context_->spec.owner);
}
QString AppControlService::Begin(const ContextSpec& spec) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (spec.owner.isEmpty() || !spec.epoch || (context_ && context_->active)
        || (spec.kind == ContextKind::EmbeddedTurn ? !embeddedEnabled_ : !externalEnabled_)) return {};
    approval_.reset();
    Context context;
    context.id = Id(); context.spec = spec;
    if (spec.kind == ContextKind::EmbeddedTurn && nextFrameOwner_ == spec.owner
        && nextFrameSource_ == sourceEpoch_ && Now() < nextFrameExpires_) context.spec.grants.frames = true;
    nextFrameOwner_.clear();
    // Embedded turns have no standing source-change grant.
    if (spec.kind == ContextKind::EmbeddedTurn) context.spec.grants.source = false;
    context.started = context.lastCall = context.refill = Now();
    for (const auto& [field, value] : target_.Fields()) context.baseline[field] = revisions_[field].value;
    const auto initial=target_.Fields();const auto camera=initial.find(Field::Camera);
    if(camera!=initial.end())context.cameraBaseline[camera->second.toString()]=cameraRevisions_[camera->second.toString()].value;
    context_ = std::move(context);
    return context_->id;
}
void AppControlService::End(const QString& id, bool normal) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!context_ || context_->id != id || !context_->active) return;
    context_->active = false; context_->ended = Now();
    if (approval_ && approval_->contextId == id) {
        if (normal) approval_->expires = std::min(approval_->expires, context_->ended + 30000);
        else approval_.reset();
    }
    for (auto& [key, call] : context_->calls) {
        if (!call.completed) {
            call.result = ControlResult::Failure("canceled"); call.completed = true;
            auto listeners = std::move(call.listeners);
            const auto result = call.result;
            for (auto& callback : listeners) QMetaObject::invokeMethod(this,[callback = std::move(callback),result] { callback(result); },Qt::QueuedConnection);
        }
    }
    context_->calls.clear();
    context_->canceledKeys.clear();
    context_->readsInFlight = context_->mutationsInFlight = 0;
    if (!normal || context_->spec.kind == ContextKind::ExternalLease) {
        target_.StopOwnedRecording(context_->spec.owner); target_.CancelOwnedSource(context_->spec.owner);
    }
    Event(*context_, normal ? "context_completed" : "context_canceled");
}
void AppControlService::RetireOwner(const QString& owner) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (owner.isEmpty()) return;
    if (nextFrameOwner_ == owner) nextFrameOwner_.clear();
    bool stopped = false;
    if (context_ && context_->spec.owner == owner) {
        if (approval_ && approval_->contextId == context_->id) approval_.reset();
        if (context_->active) { End(context_->id); stopped = true; }
        context_->spec.grants = {};
    }
    if (!stopped) { target_.StopOwnedRecording(owner); target_.CancelOwnedSource(owner); }
}
void AppControlService::SetGrants(const QString& id, const ControlGrants& grants) {
    if (!context_ || context_->id != id || !context_->active) return;
    if (context_->spec.kind == ContextKind::ExternalLease) return; // immutable lease bundle
    const bool recordingRevoked = context_->spec.grants.recording && !grants.recording;
    context_->spec.grants = grants; context_->spec.grants.source = false;
    if (recordingRevoked) target_.StopOwnedRecording(context_->spec.owner);
    Event(*context_, "grant_changed");
}
void AppControlService::SourceChanged() {
    ++sourceEpoch_; ++geometryEpoch_; ++stateVersion_;
    approval_.reset();
    nextFrameOwner_.clear();
    if (context_) { context_->spec.grants.frames = false; Event(*context_, "source_changed"); }
}
void AppControlService::RevokeFrameGrant(const QString& owner) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (owner.isEmpty()) return;
    if (nextFrameOwner_ == owner) nextFrameOwner_.clear();
    if (context_ && context_->spec.owner == owner && context_->spec.kind == ContextKind::EmbeddedTurn) {
        context_->spec.grants.frames = false;
        Event(*context_,"grant_changed");
    }
}
void AppControlService::OwnedSourceRequested(const QString& owner,const std::map<Field,QJsonValue>& previous) {
    SourceChanged(); // Retire image/geometry authority even when there was no active producer to stop.
    for(const auto field:{Field::Camera,Field::CameraFormat}) {
        auto& revision=revisions_[field];++revision.value;revision.user=false;
        if(context_ && context_->spec.owner==owner) {
            context_->baseline[field]=revision.value;
            if(previous.contains(field)) {
                auto [inverse,inserted]=context_->sourceJournal.try_emplace(field,Inverse{previous.at(field),revision.value,{}});
                inverse->second.lastWrite=revision.value;
            }
        }
    }
    ++stateVersion_;
    if (context_ && context_->spec.owner == owner) {
        Event(*context_,"source_requested");
    }
}
void AppControlService::InvalidateGeometry(bool processing) {
    ++geometryEpoch_; ++viewVersion_; ++stateVersion_;
    if (processing) ++processingVersion_;
    if (context_) Event(*context_,"geometry_changed");
}
void AppControlService::NativeFieldsChanged(const std::vector<Field>& fields) {
    bool view = false, processing = false;
    for (const auto field : fields) {
        auto& revision = revisions_[field]; ++revision.value; revision.user = true;
        if(field==Field::CameraAcceleration) {
            const auto values=target_.Fields();const auto camera=values.find(Field::Camera);
            if(camera!=values.end() && !camera->second.toString().isEmpty()) {
                auto& scoped=cameraRevisions_[camera->second.toString()];++scoped.value;scoped.user=true;
            }
        }
        view |= IsView(field); processing |= !IsView(field);
        if (field == Field::Rotation) { view = true; ++geometryEpoch_; }
    }
    if (!fields.empty()) ++stateVersion_;
    if (view) ++viewVersion_;
    if (processing) ++processingVersion_;
    if (context_) Event(*context_, "user_changed");
}
void AppControlService::NativeCameraAccelerationChanged(const QString& cameraHandle) {
    Q_ASSERT(QThread::currentThread()==thread());
    if(cameraHandle.isEmpty())return;
    auto& revision=revisions_[Field::CameraAcceleration];++revision.value;revision.user=true;
    auto& scoped=cameraRevisions_[cameraHandle];++scoped.value;scoped.user=true;
    ++stateVersion_;++processingVersion_;
    if(context_)Event(*context_,"user_changed");
}
void AppControlService::Submit(ControlRequest request, ControlCompletion completion) {
    // Atomically admitting cross-thread traffic is deliberately left to the IPC
    // adapter; this entry is queued from the provider's owning GUI QObject.
    Q_ASSERT(QThread::currentThread() == thread());
    if (queued_ >= 8) { completion(ControlResult::Failure("busy", true)); return; }
    ++queued_;
    if (context_ && context_->id == request.contextId) context_->queuedKeys.insert(request.callKey);
    QMetaObject::invokeMethod(this, [this, request = std::move(request), completion = std::move(completion)]() mutable {
        --queued_; Dispatch(request, std::move(completion));
    }, Qt::QueuedConnection);
}
void AppControlService::Cancel(const QString& contextId, const QString& callKey) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!context_ || context_->id != contextId) return;
    const auto call = context_->calls.find(callKey);
    if (call != context_->calls.end()) {
        if (!call->second.completed) Finish(contextId,callKey,ControlResult::Failure("canceled"));
        return;
    }
    if (context_->queuedKeys.contains(callKey)) context_->canceledKeys.insert(callKey);
}
bool AppControlService::ValidateArguments(const QString& name, const QJsonObject& arguments) const {
    const auto* tool = FindControlTool(name.toStdString(),edition_);
    return tool && Normalize(*tool,arguments).has_value();
}
bool AppControlService::IsContextActive(const QString& id) {
    Q_ASSERT(QThread::currentThread()==thread());
    return context_ && context_->id==id && context_->active && ValidContext(*context_);
}
quint64 AppControlService::FieldRevision(Field field) const {
    Q_ASSERT(QThread::currentThread()==thread());
    const auto found=revisions_.find(field);return found==revisions_.end()?0:found->second.value;
}
ControlResult AppControlService::ApplyDeferredFields(const QString& id,ToolId tool,const QJsonObject& args,ControlPlan plan) {
    target_.SynchronizeNativeState();
    if(!IsContextActive(id))return ControlResult::Failure("canceled");
    plan.asynchronous=false;
    auto result=Run(*context_,tool,args,plan,true,true);
    target_.FinalizeMutationReceipt();
    return result;
}
QJsonObject AppControlService::ContextStatus() const {
    if (!context_) return {{"active",false}};
    const auto& context = *context_;
    const bool external = context.spec.kind == ContextKind::ExternalLease;
    const auto now = Now();
    return {{"active",context.active},{"contextId",context.id},{"ownerKind",external ? "external-lease" : "agent-session"},
        {"nextTurnFrames",!external && nextFrameOwner_ == context.spec.owner && nextFrameSource_ == sourceEpoch_ && now < nextFrameExpires_},
        {"remainingAbsoluteMs",external && context.active ? std::max<qint64>(0,300000-(now-context.started)) : 0},
        {"remainingIdleMs",external && context.active ? std::max<qint64>(0,60000-(now-context.lastCall)) : 0},
        {"grants",QJsonObject{{"frames",context.spec.grants.frames},{"photos",context.spec.grants.photos},
            {"recording",context.spec.grants.recording},{"source",context.spec.grants.source}}},
        {"remainingMutations",std::max(0,(external ? 60 : 20)-context.mutations)},
        {"remainingObservations",std::max(0,(external ? 6 : 3)-context.observations)}};
}
bool AppControlService::AuthorizeFrameDelivery(const QString& contextId, ControlStamp expected) {
    Q_ASSERT(QThread::currentThread() == thread());
    return context_ && context_->id == contextId && ValidContext(*context_)
        && context_->spec.grants.frames && !target_.ModalActive() && Stamp() == expected;
}
bool AppControlService::ValidContext(Context& context) {
    if (!context.active) return false;
    if (context.spec.kind == ContextKind::EmbeddedTurn) return embeddedEnabled_;
    const auto now = Now();
    if (!externalEnabled_ || now - context.started >= 300000 || now - context.lastCall >= 60000) {
        End(context.id); return false;
    }
    return true;
}
std::optional<QJsonObject> AppControlService::Normalize(const ToolDescriptor& tool, const QJsonObject& args) const {
    if (QJsonDocument(args).toJson(QJsonDocument::Compact).size() > 16384) return {};
    const auto schema = QJsonDocument::fromJson(QByteArray::fromStdString(tool.inputSchema)).object();
    if (!Matches(args, schema)) return {};
    auto result = args;
    if (tool.id == ToolId::Zoom) result["magnification"] = std::round(args["magnification"].toDouble()*100)/100;
    if (tool.id == ToolId::Contrast) for (const auto key : {"contrast","brightness"}) result[key] = std::round(args[key].toDouble()*100)/100;
    if (tool.id == ToolId::Observe && !result.contains("maxLongSide")) result["maxLongSide"] = 768;
    if (tool.id == ToolId::Wait && result["condition"] == "operation_state" && !result.contains("operationId")) return {};
    if (tool.id == ToolId::ReadingColors) {
        if ((result["intent"] == "scheme") != result.contains("schemeId")) return {};
    }
    return result;
}
bool AppControlService::Authorized(const Context& context, ToolId tool, const ControlPlan& plan) const {
    return !plan.requiresApproval && plan.approvalDomain == ApprovalDomain::None && HasDomainGrant(context,tool,plan);
}
bool AppControlService::HasDomainGrant(const Context& context, ToolId tool, const ControlPlan& plan) const {
    if (plan.requiresFrames && !context.spec.grants.frames) return false;
    if (plan.requiresPhotos && !context.spec.grants.photos) return false;
    if (plan.requiresRecording && !context.spec.grants.recording) return false;
    const auto external = context.spec.kind == ContextKind::ExternalLease;
    for (const auto& write : plan.writes) {
        const auto id=ParameterId(write.field).toStdString();const auto* parameter=FindParameter(id);
        if(parameter && parameter->permission==ParameterPermission::Source && (!external || !context.spec.grants.source))return false;
        if (write.field==Field::AssistantSceneExplain && write.value.toBool() && !context.spec.grants.frames) return false;
        if (write.field==Field::AnnotationCaptureOnExit && write.value.toBool() && !context.spec.grants.photos) return false;
        if (write.field==Field::Rotation || write.field==Field::Camera || write.field==Field::CameraFormat
            || write.field==Field::CameraAcceleration || write.field==Field::CameraTorch)
            if (!external || !context.spec.grants.source) return false;
    }
    switch (tool) {
    case ToolId::Observe: return context.spec.grants.frames;
    case ToolId::Orientation: case ToolId::CameraSelect: case ToolId::CameraSettingsSet:
        return external && context.spec.grants.source;
    case ToolId::Photo: return context.spec.grants.photos;
    case ToolId::RecordingStart: return context.spec.grants.recording;
    case ToolId::RecordingStop:
        return context.spec.grants.recording && plan.ownedRecording;
    default: return true;
    }
}
bool AppControlService::Stale(const Context& context, const ControlPlan& plan) const {
    for (const auto& write : plan.writes) {
        if(write.field==Field::CameraAcceleration && !write.targetIdentity.isEmpty()) {
            const auto revision=cameraRevisions_.find(write.targetIdentity);
            const auto baseline=context.cameraBaseline.find(write.targetIdentity);
            if(revision!=cameraRevisions_.end() && revision->second.user
                && (baseline==context.cameraBaseline.end() || revision->second.value>baseline->second))return true;
            continue;
        }
        const auto revision = revisions_.find(write.field);
        const auto baseline = context.baseline.find(write.field);
        if (revision != revisions_.end() && revision->second.user
            && (baseline == context.baseline.end() || revision->second.value > baseline->second)) return true;
    }
    return false;
}
void AppControlService::Dispatch(const ControlRequest& request, ControlCompletion completion) {
    target_.SynchronizeNativeState();
    if (!context_ || context_->id != request.contextId || !ValidContext(*context_)) {
        completion(ControlResult::Failure("context_expired")); return;
    }
    auto& context = *context_;
    context.queuedKeys.erase(request.callKey);
    if (context.canceledKeys.contains(request.callKey)) { completion(ControlResult::Failure("canceled")); return; }
    if (request.callKey.isEmpty() || request.callKey.size() > 256) { completion(ControlResult::Failure("invalid_argument")); return; }
    const auto* descriptor = FindControlTool(request.tool.toStdString(), edition_);
    if (!descriptor) { completion(ControlResult::Failure("denied_by_policy")); return; }
    const auto args = Normalize(*descriptor, request.arguments);
    if (!args) { completion(ControlResult::Failure("invalid_argument")); return; }
    const auto digest = Digest(descriptor->id, *args);
    if (auto found = context.calls.find(request.callKey); found != context.calls.end()) {
        if (found->second.digest != digest) completion(ControlResult::Failure("invalid_argument"));
        else if (found->second.completed) completion(Delivery(context,found->second));
        else found->second.listeners.push_back(std::move(completion));
        return;
    }
    if (context.calls.size() >= 128) { completion(ControlResult::Failure("budget_exhausted")); return; }
    context.calls.emplace(request.callKey, Reservation{digest, {}, false, {std::move(completion)}});
    context.lastCall = Now();
    const auto tool = descriptor->id;
    auto normalized = request; normalized.arguments = *args;
    if (tool == ToolId::AutoAdjust) {
        if (target_.ModalActive() || target_.UserInteracting() || context.mutationsInFlight >= 1) {
            Finish(context.id,request.callKey,ControlResult::Failure("busy",true)); return;
        }
        auto& call = context.calls.at(request.callKey); call.counted = true; call.mutation = true; call.deadline = Now()+5000;
        ++context.mutationsInFlight;
        const auto expected = Stamp();
        QPointer<AppControlService> self(this);
        target_.Execute(ToolId::Metrics,{{"_requireKnownTemporal",true}},context.spec.owner,context.spec.kind,[self,normalized,expected](ControlResult metrics) {
            if (!self) return;
            QMetaObject::invokeMethod(self,[self,normalized,expected,metrics] {
                if (!self) return;
                if (!metrics.content["ok"].toBool()) self->Finish(normalized.contextId,normalized.callKey,metrics);
                else if (self->Stamp() != expected) self->Finish(normalized.contextId,normalized.callKey,ControlResult::Failure("stale_state"));
                else self->Continue(normalized,ToolId::AutoAdjust);
            },Qt::QueuedConnection);
        });
        return;
    }
    Continue(normalized,tool);
}
void AppControlService::Continue(const ControlRequest& request, ToolId tool) {
    if (!context_ || context_->id != request.contextId || !ValidContext(*context_)) {
        Finish(request.contextId,request.callKey,ControlResult::Failure("context_expired")); return;
    }
    auto& context = *context_;
    const auto found = context.calls.find(request.callKey);
    if (found == context.calls.end() || found->second.completed) return;
    const auto* descriptor = FindControlTool(request.tool.toStdString(),edition_);
    if (!descriptor) { Finish(context.id,request.callKey,ControlResult::Failure("denied_by_policy")); return; }
    const auto args = std::optional<QJsonObject>(request.arguments);
    const auto digest = found->second.digest;
    if (descriptor->tier != Tier::Read && descriptor->tier != Tier::Egress
        && context.mutationsInFlight >= 1 && !found->second.counted) {
        Finish(context.id,request.callKey,ControlResult::Failure("busy",true)); return;
    }
    // Plan 52 §14.3: a lease without the matching bundle toggle is refused by
    // policy before any availability check, so an ungranted external client
    // cannot learn camera, frame or recording availability.
    if (context.spec.kind == ContextKind::ExternalLease) {
        const auto& grants = context.spec.grants;
        const bool toggleMissing =
            (tool == ToolId::Observe && !grants.frames)
            || ((tool == ToolId::Orientation || tool == ToolId::CameraSelect || tool == ToolId::CameraSettingsSet) && !grants.source)
            || (tool == ToolId::Photo && !grants.photos)
            || ((tool == ToolId::RecordingStart || tool == ToolId::RecordingStop) && !grants.recording);
        if (toggleMissing) { Finish(context.id, request.callKey, ControlResult::Failure("denied_by_policy")); return; }
    }
    auto plan = target_.Prepare(tool, *args, context.spec.owner);
    DeriveParameterPolicy(plan);
    if (!plan.error.isEmpty()) { Finish(context.id, request.callKey, ControlResult::Failure(plan.error)); return; }
    if (descriptor->tier != Tier::Read && target_.ModalActive()) { Finish(context.id, request.callKey, ControlResult::Failure("modal_active", true)); return; }
    if (descriptor->tier == Tier::Source && target_.RecordingActive()) { Finish(context.id, request.callKey, ControlResult::Failure("busy")); return; }
    if (descriptor->tier != Tier::Read && descriptor->tier != Tier::Egress && target_.UserInteracting()) { Finish(context.id, request.callKey, ControlResult::Failure("user_interacting", true)); return; }
    if (Stale(context, plan)) { Finish(context.id, request.callKey, ControlResult::Failure("stale_state")); return; }
    if (!Authorized(context, tool, plan)) {
        Finish(context.id, request.callKey, context.spec.kind == ContextKind::ExternalLease
            && !(plan.approvalDomain == ApprovalDomain::Configuration && HasDomainGrant(context,tool,plan))
            ? ControlResult::Failure("denied_by_policy") : Ask(context, tool, *args, plan, digest)); return;
    }
    if (tool == ToolId::RecordingStart && context.spec.kind == ContextKind::ExternalLease && Now() - context.started > 270000) {
        Finish(context.id, request.callKey, ControlResult::Failure("context_expired")); return;
    }
    if (IsAsync(tool) || plan.asynchronous) {
        const bool mutation = descriptor->tier != Tier::Read && descriptor->tier != Tier::Egress;
        if ((mutation && context.mutationsInFlight >= 1) || (!mutation && context.readsInFlight >= 4)) {
            Finish(context.id,request.callKey,ControlResult::Failure("busy",true)); return;
        }
        // Run performs admission/rate guards without letting an async target
        // bypass revisions. Frame bytes are checked again by Finish.
        const auto guard = Run(context, tool, *args, plan, false);
        if (!guard.content["ok"].toBool()) { Finish(context.id, request.callKey, guard); return; }
        auto& reservation = context.calls.at(request.callKey);
        reservation.counted = true; reservation.mutation = mutation;
        reservation.deadline = Now() + (tool == ToolId::Observe ? 3000 : tool == ToolId::Wait ? args->value("timeoutMs").toInt()+250 : 5000);
        if (mutation) ++context.mutationsInFlight; else ++context.readsInFlight;
        const QString id = context.id, key = request.callKey, owner = context.spec.owner;
        QPointer<AppControlService> self(this);
        const auto expected = Stamp();
        if (tool == ToolId::Wait && args->value("condition") == "grant_changed") {
            const auto cursor = context.eventCursor;
            const auto deadline = Now()+args->value("timeoutMs").toInt();
            auto* timer = new QTimer(this);
            timer->setInterval(25);
            const auto poll = [self,timer,id,key,cursor,deadline] {
                if (!self || !self->context_ || self->context_->id != id || !self->context_->active ||
                    !self->context_->calls.contains(key) || self->context_->calls.at(key).completed) {
                    timer->deleteLater(); return;
                }
                const auto& events = self->context_->events;
                const bool met = std::any_of(events.begin(),events.end(),[cursor](const QJsonObject& event) {
                    return event.value("sequence").toInteger() > static_cast<qint64>(cursor) &&
                        event.value("type") == "grant_changed";
                });
                if (met || self->Now() >= deadline) {
                    timer->stop(); timer->deleteLater();
                    self->Finish(id,key,{{{"ok",true},{"met",met}},{}});
                }
            };
            connect(timer,&QTimer::timeout,this,poll);
            timer->start(); poll(); return;
        }
        target_.Execute(tool, *args, owner, context.spec.kind, [self, id, key, expected, tool](ControlResult result) mutable {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, id, key, expected, tool, result = std::move(result)]() mutable {
                if (!self) return;
                if ((tool == ToolId::Observe || tool == ToolId::Metrics) && expected != self->Stamp()) result = ControlResult::Failure("stale_observation");
                self->Finish(id, key, std::move(result));
            }, Qt::QueuedConnection);
        });
        return;
    }
    auto applied = Run(context, tool, *args, plan, false);
    if (tool == ToolId::AutoAdjust && applied.content["ok"].toBool()) {
        QPointer<AppControlService> self(this);
        target_.Execute(tool,*args,context.spec.owner,context.spec.kind,[self,request,applied](ControlResult pending) mutable {
            if (!self) return;
            for (const auto key : {"changed","applied","effective","stateVersion"}) if (applied.content.contains(key)) pending.content[key] = applied.content[key];
            if (!pending.content["ok"].toBool() && applied.content["changed"].toBool()) pending.content["partial"] = true;
            self->Finish(request.contextId,request.callKey,std::move(pending));
        });
        return;
    }
    Finish(context.id, request.callKey, std::move(applied));
}
ControlResult AppControlService::Run(Context& context, ToolId tool, const QJsonObject& args,
                                   const ControlPlan& plan, bool approved, bool alreadyAdmitted) {
    const auto* descriptor = FindControlTool([&]() { for (const auto& t : ControlRegistry(edition_)) if (t.id == tool) return t.canonicalId; return std::string_view(); }(), edition_);
    if (!descriptor) return ControlResult::Failure("denied_by_policy");
    if (descriptor->tier != Tier::Read && target_.ModalActive()) return ControlResult::Failure("modal_active", true);
    if (descriptor->tier == Tier::Source && target_.RecordingActive()) return ControlResult::Failure("busy");
    if(target_.RecordingActive() && std::any_of(plan.writes.begin(),plan.writes.end(),[](const FieldWrite& write) {
        return write.field==Field::Rotation || write.field==Field::Camera || write.field==Field::CameraFormat
            || write.field==Field::CameraAcceleration || write.field==Field::CameraTorch;
    }))return ControlResult::Failure("busy");
    if (plan.requiresFrames && !context.spec.grants.frames) return ControlResult::Failure("consent_denied");
    if (plan.requiresPhotos && !context.spec.grants.photos) return ControlResult::Failure("consent_denied");
    if (plan.requiresRecording && !context.spec.grants.recording) return ControlResult::Failure("consent_denied");
    for (const auto& write : plan.writes) {
        if (write.field==Field::AssistantSceneExplain && write.value.toBool() && !context.spec.grants.frames)
            return ControlResult::Failure("consent_denied");
        if (write.field==Field::AnnotationCaptureOnExit && write.value.toBool() && !context.spec.grants.photos)
            return ControlResult::Failure("consent_denied");
    }
    if (approved && context.spec.kind == ContextKind::ExternalLease && !HasDomainGrant(context,tool,plan))
        return ControlResult::Failure("denied_by_policy");
    if (!approved && !Authorized(context, tool, plan)) return ControlResult::Failure("consent_denied");
    if (Stale(context, plan)) return ControlResult::Failure("stale_state");
    if (descriptor->tier == Tier::Read) {
        auto result = tool == ToolId::State ? target_.State() : target_.Query(tool, args);
        if (result.contains("ok") && !result["ok"].toBool()) return {result,{}};
        if (tool == ToolId::State) {
            QJsonObject revisionDto;
            for (const auto& [field, value] : target_.Fields()) {
                context.baseline[field] = revisions_[field].value;
                if(field==Field::CameraAcceleration) {
                    const auto values=target_.Fields();const auto camera=values.find(Field::Camera);
                    if(camera!=values.end())context.cameraBaseline[camera->second.toString()]=cameraRevisions_[camera->second.toString()].value;
                }
                revisionDto[FieldName(field)] = static_cast<qint64>(revisions_[field].value);
            }
            result["fieldRevisions"] = revisionDto;
            result["assistant"] = ContextStatus();
            result["versions"] = QJsonObject{{"state",static_cast<qint64>(stateVersion_)},{"view",static_cast<qint64>(viewVersion_)},
                {"processing",static_cast<qint64>(processingVersion_)},{"sourceEpoch",static_cast<qint64>(sourceEpoch_)},{"geometryEpoch",static_cast<qint64>(geometryEpoch_)}};
            if (args.contains("sinceEvent")) {
                const auto since = static_cast<quint64>(args["sinceEvent"].toDouble());
                QJsonArray events;
                for (const auto& event : context.events) if (event["sequence"].toDouble() > since && events.size() < 32) events.append(event);
                result["eventsAfter"] = events;
                result["eventsLost"] = !context.events.empty() && since + 1 < context.events.front()["sequence"].toDouble();
                result["nextEventCursor"] = events.isEmpty() ? QJsonValue(static_cast<qint64>(since)) : QJsonValue(events.last().toObject()["sequence"]);
            }
        } else if (tool == ToolId::Modes) context.baseline[Field::PresetList] = revisions_[Field::PresetList].value;
        else if (tool == ToolId::Operation) context.baseline[Field::Recording] = revisions_[Field::Recording].value;
        else if (!plan.observedFields.empty()) {
            QJsonObject observedRevisions;
            for (const auto field : plan.observedFields) {
                context.baseline[field] = revisions_[field].value;
                if(field==Field::CameraAcceleration) {
                    const auto values=target_.Fields();const auto camera=values.find(Field::Camera);
                    if(camera!=values.end())context.cameraBaseline[camera->second.toString()]=cameraRevisions_[camera->second.toString()].value;
                }
                observedRevisions[ResultFieldName(tool,field)] = static_cast<qint64>(revisions_[field].value);
            }
            result["fieldRevisions"] = observedRevisions;
        }
        result["ok"] = true;
        return {result,{}};
    }
    if (descriptor->tier == Tier::Egress) {
        const int limit = context.spec.kind == ContextKind::EmbeddedTurn ? 3 : 6;
        if (context.observations >= limit) return ControlResult::Failure("budget_exhausted");
        ++context.observations;
        return {{{"ok",true}},{}};
    }
    const auto now = Now();
    context.tokens = std::min(4.0, context.tokens + (now - context.refill)/500.0); context.refill = now;
    const int maximum = context.spec.kind == ContextKind::EmbeddedTurn ? 20 : 60;
    if (!alreadyAdmitted && context.mutations >= maximum) return ControlResult::Failure("budget_exhausted");
    if (!alreadyAdmitted && (context.tokens < 1 || (descriptor->composite && now - context.compositeAt < 2000))) return ControlResult::Failure("busy", true);
    auto fields = target_.Fields();
    for(const auto& write:plan.writes)fields[write.field]=target_.ReadField(write);
    for (const auto& write : plan.writes) if (IsFlash(write.field)) {
        const auto previous = context.flashAt.find(write.field);
        if (previous != context.flashAt.end() && now - previous->second < 250) return ControlResult::Failure("busy", true);
    }
    if(!alreadyAdmitted) { ++context.mutations; --context.tokens; if (descriptor->composite) context.compositeAt = now; }
    if (IsAsync(tool) || plan.asynchronous) return {{{"ok",true}},{}};
    QJsonObject anticipated;
    for (const auto& write : plan.writes) {
        if (!fields.contains(write.field)) return ControlResult::Failure("unavailable");
        const auto name=ResultFieldName(tool,write.field);
        if(!name.isEmpty())anticipated[name] = ResultFieldValue(tool,write.field,write.value);
    }
    if (QJsonDocument(QJsonObject{{"applied",anticipated},{"effective",anticipated}}).toJson(QJsonDocument::Compact).size() > 30000)
        return ControlResult::Failure("budget_exhausted");
    if (plan.approvalDomain == ApprovalDomain::Configuration) {
        const bool providerChanged = std::any_of(plan.writes.begin(),plan.writes.end(),[&](const FieldWrite& write) {
            const bool sensitive = write.field == Field::AssistantProvider || write.field == Field::AssistantCodexModel
                || write.field == Field::AssistantHttpModel || write.field == Field::AssistantReasoning
                || write.field == Field::AssistantInstructions || write.field == Field::AssistantExplainPrompt;
            return sensitive && fields.at(write.field) != write.value;
        });
        if (providerChanged) {
            context.spec.grants.frames = false;
            if (nextFrameOwner_ == context.spec.owner) { nextFrameOwner_.clear(); nextFrameExpires_ = 0; }
            Event(context,"grant_changed");
        }
    }
    if (!target_.BeginFieldPlan(plan)) return ControlResult::Failure("unavailable");
    const auto priorRevisions = revisions_;
    const auto priorBaseline = context.baseline;
    const auto priorJournal = context.journal;
    const auto priorCameraJournal=context.cameraJournal;
    const auto priorCameraBaseline=context.cameraBaseline;
    const auto priorCameraRevisions=cameraRevisions_;
    const auto priorFlash = context.flashAt;
    bool fieldPlanOpen = true;
    auto fieldPlanGuard = qScopeGuard([&] { if (fieldPlanOpen) target_.EndFieldPlan(); });
    QJsonObject applied;
    QJsonObject effective;
    bool partial = !target_.CommitFieldPlan(plan), view = false, processing = false;
    for (const auto& write : plan.writes) {
        if(partial)break;
        const auto existing = fields.find(write.field);
        if (existing == fields.end()) { partial = true; break; }
        const auto live=target_.ReadField(write);
        if (!write.retryUnacknowledged && existing->second == write.value && live == write.value) {
            effective[FieldName(write.field)] = existing->second;
            context.baseline[write.field] = revisions_[write.field].value;
            continue;
        }
        if (!target_.Apply(write)) { partial = true; break; }
        auto& revision = revisions_[write.field]; ++revision.value; revision.user = false;
        if(write.field==Field::CameraAcceleration && !write.targetIdentity.isEmpty()) {
            auto& scoped=cameraRevisions_[write.targetIdentity];++scoped.value;scoped.user=false;
            auto [inverse,inserted]=context.cameraJournal.try_emplace(write.targetIdentity,Inverse{existing->second,scoped.value,write.targetIdentity});
            inverse->second.lastWrite=scoped.value;context.cameraBaseline[write.targetIdentity]=scoped.value;
        } else {
            auto [inverse, inserted] = context.journal.try_emplace(write.field, Inverse{existing->second, revision.value,write.targetIdentity});
            inverse->second.lastWrite = revision.value;
        }
        context.baseline[write.field] = revision.value;
        if (IsFlash(write.field)) context.flashAt[write.field] = now;
        applied[FieldName(write.field)] = target_.ReadField(write);
        effective[FieldName(write.field)] = applied[FieldName(write.field)];
        view |= IsView(write.field); processing |= !IsView(write.field);
        if (write.field == Field::Rotation) { view = true; ++geometryEpoch_; }
    }
    target_.EndFieldPlan(); fieldPlanOpen = false;
    if (partial) {
        ControlPlan rollback;
        for (const auto& write : plan.writes)
            if (fields.contains(write.field)) rollback.writes.push_back({write.field,fields.at(write.field),write.targetIdentity});
        const bool rollbackBatch = target_.BeginFieldPlan(rollback);
        bool restored = rollbackBatch;
        if (rollbackBatch) {
            restored=target_.CommitFieldPlan(rollback);
            for (const auto& write : rollback.writes) restored = target_.Apply(write) && restored;
            target_.EndFieldPlan();
        }
        revisions_ = priorRevisions; context.baseline = priorBaseline;
        context.journal = priorJournal; context.flashAt = priorFlash;
        context.cameraJournal=priorCameraJournal;context.cameraBaseline=priorCameraBaseline;cameraRevisions_=priorCameraRevisions;
        // Restoring preferences cannot restore consumed processing history.
        // Retire cached frame provenance even when every inverse succeeded.
        ++processingVersion_; ++geometryEpoch_;
        if (!restored) {
            // A failed native inverse cannot retain the old revision or undo authority.
            for (const auto& write : plan.writes) {
                auto& revision=revisions_[write.field]; ++revision.value; revision.user=true;
                context.baseline.erase(write.field); context.journal.erase(write.field);
                if(write.field==Field::CameraAcceleration && !write.targetIdentity.isEmpty()) {
                    auto& scoped=cameraRevisions_[write.targetIdentity]; ++scoped.value; scoped.user=true;
                    context.cameraBaseline.erase(write.targetIdentity);context.cameraJournal.erase(write.targetIdentity);
                }
            }
            ++stateVersion_; ++processingVersion_; ++viewVersion_;
        }
        auto failure = ControlResult::Failure("internal");
        failure.content["rolledBack"] = restored;
        Event(context,"mutation_failed",{{"rolledBack",restored}});
        return failure;
    }
    for (const auto& write : plan.writes) {
        const auto finalValue=target_.ReadField(write);
        if(!finalValue.isUndefined())effective[FieldName(write.field)]=finalValue;
        if(applied.contains(FieldName(write.field)) && !finalValue.isUndefined())applied[FieldName(write.field)]=finalValue;
    }
    const bool changed = !applied.isEmpty();
    if(static_cast<int>(tool)>=static_cast<int>(ToolId::ControlsList)) {
        QJsonObject canonicalApplied,canonicalEffective;
        for(const auto& write:plan.writes) {
            const auto name=ResultFieldName(tool,write.field);if(name.isEmpty())continue;
            if(applied.contains(FieldName(write.field)))canonicalApplied[name]=ResultFieldValue(tool,write.field,applied[FieldName(write.field)]);
            if(effective.contains(FieldName(write.field)))canonicalEffective[name]=ResultFieldValue(tool,write.field,effective[FieldName(write.field)]);
        }
        applied=canonicalApplied;effective=canonicalEffective;
        const auto nativeEffective=target_.EffectiveValues(plan);
        for(auto it=nativeEffective.begin();it!=nativeEffective.end();++it)if(effective.contains(it.key()))effective[it.key()]=it.value();
    }
    if (changed) {
        ++stateVersion_; if (view) ++viewVersion_; if (processing) ++processingVersion_;
        if (!target_.Save()) Event(context,"settings_not_saved");
        Event(context,"applied",{{"applied",applied}});
    }
    QJsonObject result{{"ok",!partial},{"changed",changed},{"applied",applied},{"stateVersion",static_cast<qint64>(stateVersion_)}};
    if (!effective.isEmpty()) result["effective"] = effective;
    if (partial) { result["partial"] = true; result["error"] = QJsonObject{{"code","internal"},{"retryable",false}}; }
    return {result,{}};
}
ControlResult AppControlService::Ask(Context& context, ToolId tool, const QJsonObject& args,
                                    const ControlPlan& plan, const QByteArray& digest) {
    if (std::find(context.denied.begin(),context.denied.end(),digest) != context.denied.end()) return ControlResult::Failure("consent_denied");
    if (approval_ && approval_->pending) return ControlResult::Failure("busy",true);
    if (context.approvals >= 3) return ControlResult::Failure("budget_exhausted");
    Approval approval;
    approval.id = Id(); approval.contextId = context.id; approval.epoch = context.spec.epoch;
    approval.sourceEpoch = sourceEpoch_; approval.tool = tool; approval.args = args;
    approval.digest = digest; approval.plan = plan; approval.expires = Now()+300000;
    approval.frame = tool == ToolId::Observe || (plan.requiresFrames && !context.spec.grants.frames);
    for (const auto& write : plan.writes) approval.revisions[write.field] = revisions_[write.field].value;
    approval_ = std::move(approval); ++context.approvals;
    Event(context,"approval_requested",{{"approvalId",approval_->id}});
    auto result = ControlResult::Failure("consent_required"); result.content["approvalId"] = approval_->id;
    return result;
}
QJsonObject AppControlService::PendingApproval() const {
    if (!approval_ || !approval_->pending) return {};
    QString tool;
    for (const auto& descriptor : ControlRegistry(edition_)) if (descriptor.id == approval_->tool) tool = QString::fromUtf8(descriptor.canonicalId.data(), descriptor.canonicalId.size());
    QString domain = approval_->frame ? "frame" : "configuration";
    QJsonArray sideEffects;
    QJsonObject normalizedChanges;
    for (const auto& write : approval_->plan.writes) {
        const auto name = ResultFieldName(ToolId::ControlsList,write.field);
        if (name.isEmpty()) continue;
        normalizedChanges[name] = ResultFieldValue(ToolId::ControlsList,write.field,write.value);
    }
    switch (approval_->tool) {
    case ToolId::Orientation: case ToolId::CameraSelect: case ToolId::CameraSettingsSet:
        domain = "source"; sideEffects.append("source-change"); break;
    case ToolId::RecordingStop: case ToolId::RecordingSettingsSet: domain = "recording"; break;
    case ToolId::AssistantPreferencesSet: domain = "provider"; break;
    case ToolId::NotesPreferencesSet: domain = "output"; break;
    case ToolId::AnnotationPreferencesSet: domain = "drawing"; break;
    case ToolId::UiPreferencesSet: domain = "ui"; break;
    default: break;
    }
    if (approval_->frame) { domain = "frame"; sideEffects = {}; }
    if(approval_->tool==ToolId::RecordingSettingsSet && approval_->plan.terminalRecording)sideEffects.append("recording-stop");
    return {{"approvalId",approval_->id},{"tool",tool},{"arguments",approval_->args},{"expiresAt",approval_->expires},
        {"approvalDomain",domain},{"sideEffects",sideEffects},{"normalizedChanges",normalizedChanges},
        {"expiresInMs",std::max<qint64>(0,approval_->expires-Now())},{"kind",approval_->frame ? "frame-grant" : "action"},
        {"targetIdentity",approval_->plan.targetIdentity},{"targetDisplay",approval_->plan.targetDisplay},
        {"startedAt",approval_->plan.startedAt}};
}
ControlResult AppControlService::ResolveApproval(const QString& id, bool allow) {
    target_.SynchronizeNativeState();
    Q_ASSERT(QThread::currentThread() == thread());
    if (!approval_ || !approval_->pending || approval_->id != id || !context_
        || context_->id != approval_->contextId || context_->spec.epoch != approval_->epoch) return ControlResult::Failure("not_found");
    auto approval = *approval_; approval_->pending = false; // consume before effect
    auto& context = *context_;
    if (context.spec.kind == ContextKind::ExternalLease && (!context.active || !ValidContext(context)))
        return ControlResult::Failure("context_expired");
    if (!allow) {
        context.denied.push_back(approval.digest); Event(context,"grant_changed",{{"outcome","denied"}});
        return ControlResult::Failure("consent_denied");
    }
    if (Now() >= approval.expires || approval.sourceEpoch != sourceEpoch_
        || (!context.active && Now()-context.ended >= 30000)
        || (context.spec.kind == ContextKind::EmbeddedTurn ? !embeddedEnabled_ : !externalEnabled_)) return ControlResult::Failure("context_expired");
    for (const auto& [field, revision] : approval.revisions) if (revisions_[field].value != revision) return ControlResult::Failure("stale_state");
    auto currentPlan = target_.Prepare(approval.tool,approval.args,context.spec.owner);
    DeriveParameterPolicy(currentPlan);
    const auto& bound = approval.plan;
    bool sameWrites = currentPlan.writes.size() == bound.writes.size();
    for (size_t index = 0; sameWrites && index < bound.writes.size(); ++index)
        sameWrites = currentPlan.writes[index].field == bound.writes[index].field
            && currentPlan.writes[index].value == bound.writes[index].value
            && currentPlan.writes[index].targetIdentity == bound.writes[index].targetIdentity
            && currentPlan.writes[index].retryUnacknowledged == bound.writes[index].retryUnacknowledged;
    if (!currentPlan.error.isEmpty() || currentPlan.targetIdentity != bound.targetIdentity || !sameWrites
        || currentPlan.approvalDomain != bound.approvalDomain || currentPlan.requiresFrames != bound.requiresFrames
        || currentPlan.requiresPhotos != bound.requiresPhotos
        || currentPlan.requiresRecording != bound.requiresRecording || currentPlan.asynchronous != bound.asynchronous
        || currentPlan.requiresApproval != bound.requiresApproval || currentPlan.resetsHistory != bound.resetsHistory
        || currentPlan.ownedRecording != bound.ownedRecording || currentPlan.terminalRecording != bound.terminalRecording)
        return ControlResult::Failure("stale_state");
    if (approval.frame) {
        if (!context.active) {
            // A finished turn cannot receive a frame. The UI starts the next
            // turn with this narrowly scoped grant; no pixels are captured here.
            nextFrameOwner_ = context.spec.owner; nextFrameSource_ = sourceEpoch_; nextFrameExpires_ = Now()+300000;
            Event(context,"frame_grant_next_turn",{{"sourceEpoch",static_cast<qint64>(sourceEpoch_)},{"expiresAt",nextFrameExpires_}});
        } else { context.spec.grants.frames = true; Event(context,"grant_changed"); }
        return {{{"ok",true},{"changed",false},{"grantActivated",true}},{}};
    }
    auto result = Run(context,approval.tool,approval.args,currentPlan,true);
    if (result.content["ok"].toBool() && (IsAsync(approval.tool) || currentPlan.asynchronous)) {
        QPointer<AppControlService> self(this);
        const auto contextId = context.id;
        target_.Execute(approval.tool,approval.args,context.spec.owner,context.spec.kind,[self,contextId,id](ControlResult completed) {
            if (!self) return;
            QMetaObject::invokeMethod(self,[self,contextId,id,completed] {
                if (self && self->context_ && self->context_->id == contextId)
                    self->Event(*self->context_,"operation_receipt",{{"approvalId",id},{"outcome",completed.content}});
            },Qt::QueuedConnection);
        });
    }
    Event(context,"grant_changed",{{"approvalId",id},{"outcome",result.content}});
    target_.FinalizeMutationReceipt();
    return result;
}
void AppControlService::Finish(const QString& id, const QString& key, ControlResult result) {
    if (!context_ || context_->id != id) return;
    auto found = context_->calls.find(key);
    if (found == context_->calls.end() || found->second.completed) return;
    if (!ValidContext(*context_)) {
        if (context_ && context_->id == id && context_->active) End(id);
        return;
    }
    if (QJsonDocument(result.content).toJson(QJsonDocument::Compact).size() > 32*1024) result = ControlResult::Failure("internal");
    found->second.result = result; found->second.completed = true;
    found->second.imageStamp = Stamp();
    if (found->second.counted) {
        if (found->second.mutation) context_->mutationsInFlight = std::max(0,context_->mutationsInFlight-1);
        else context_->readsInFlight = std::max(0,context_->readsInFlight-1);
    }
    auto callbacks = std::move(found->second.listeners);
    const auto cached = found->second;
    QPointer<AppControlService> self(this);
    for (auto& callback : callbacks) QMetaObject::invokeMethod(this,[self,id,cached,callback = std::move(callback)]() mutable {
        if (!self) return;
        if (!self->context_ || self->context_->id != id) callback(ControlResult::Failure("canceled"));
        else callback(self->Delivery(*self->context_,cached));
    },Qt::QueuedConnection);
    target_.FinalizeMutationReceipt();
}
ControlResult AppControlService::Delivery(Context& context, const Reservation& reservation) {
    auto result = reservation.result;
    if (result.jpeg.isEmpty()) return result;
    const auto now = Now();
    if (context.spec.kind == ContextKind::ExternalLease &&
        (now-context.started >= 300000 || now-context.lastCall >= 60000))
        return ControlResult::Failure("context_expired");
    if (!context.active || !context.spec.grants.frames || target_.ModalActive()) return ControlResult::Failure("consent_denied");
    if (reservation.imageStamp != Stamp()) return ControlResult::Failure("stale_observation");
    const auto bytes = result.jpeg.toBase64().size();
    const bool external = context.spec.kind == ContextKind::ExternalLease;
    if (bytes > 256*1024 || context.imageBytes+bytes > (external ? 1536 : 768)*1024
        || context.imagesDelivered >= (external ? 6 : 3)) return ControlResult::Failure("budget_exhausted");
    context.imageBytes += static_cast<int>(bytes); ++context.imagesDelivered;
    return result;
}
void AppControlService::Event(Context& context, const QString& type, QJsonObject data) {
    data["contextId"] = context.id;
    data["type"] = type; data["sequence"] = static_cast<qint64>(++context.eventCursor);
    context.events.push_back(data); if (context.events.size() > 256) context.events.pop_front();
    if (eventSink_) {
        QPointer<AppControlService> self(this);
        const auto id = context.id; const auto sinkGeneration = eventSinkGeneration_;
        QMetaObject::invokeMethod(this,[self,id,sinkGeneration,data = std::move(data)] {
            if (!self || !self->context_ || self->context_->id != id
                || self->eventSinkGeneration_ != sinkGeneration || !self->eventSink_) return;
            // Copy the callable before invoking it: a trusted listener may
            // replace itself or begin another context during this notification.
            const auto sink = self->eventSink_; sink(data);
        },Qt::QueuedConnection);
    }
}
void AppControlService::SetEventSink(std::function<void(const QJsonObject&)> sink) {
    ++eventSinkGeneration_; eventSink_ = std::move(sink);
}
void AppControlService::Expire() {
    if (context_ && context_->active) ValidContext(*context_);
    if (approval_ && Now() >= approval_->expires) approval_.reset();
    if (context_ && context_->active) {
        std::vector<QString> expired;
        for (const auto& [key,call] : context_->calls) if (!call.completed && call.deadline && Now() >= call.deadline) expired.push_back(key);
        for (const auto& key : expired) Finish(context_->id,key,ControlResult::Failure("timeout"));
    }
}
ControlResult AppControlService::StopAndUndo() {
    target_.SynchronizeNativeState();
    if (!context_) return {{{"ok",true},{"changed",false}},{}};
    const bool sourceRestoreRequested=target_.HasOwnedSource(context_->spec.owner);
    if (context_->active) End(context_->id);
    else { target_.StopOwnedRecording(context_->spec.owner); target_.CancelOwnedSource(context_->spec.owner); }
    approval_.reset();
    QJsonArray restored, skipped;
    QJsonArray sourceRestoreFields;
    for(const auto& [field,inverse]:context_->sourceJournal) {
        if(sourceRestoreRequested && revisions_[field].value==inverse.lastWrite)sourceRestoreFields.append(ResultFieldName(ToolId::ControlsList,field));
        else skipped.append(ResultFieldName(ToolId::ControlsList,field));
    }
    context_->sourceJournal.clear();
    std::vector<Field> order;
    for (const auto field : {Field::Rotation,Field::SuperResEnabled,Field::SuperResPrefer2x,Field::SuperResUltra1440p,
                            Field::SuperResStrength,Field::Stabilization,Field::CustomColorRecipe})
        if (context_->journal.contains(field)) order.push_back(field);
    for (const auto& [field,inverse] : context_->journal)
        if (std::find(order.begin(),order.end(),field)==order.end()) order.push_back(field);
    const auto custom = [](Field field) { return field==Field::CustomColorRecipe || field==Field::CustomColorMode
        || field==Field::CustomColorStopCount || field==Field::CustomColorStops || field==Field::CustomColorStepped || field==Field::Scheme; };
    bool customStale = false;
    if (context_->journal.contains(Field::CustomColorRecipe))
        for (const auto& [field,inverse] : context_->journal)
            if (custom(field) && revisions_[field].value != inverse.lastWrite) customStale = true;
    if (context_->journal.contains(Field::CustomColorRecipe))
        for (const auto field : {Field::CustomColorMode,Field::CustomColorStopCount,Field::CustomColorStops,Field::CustomColorStepped,Field::Scheme})
            if (revisions_[field].user && revisions_[field].value > context_->baseline[field]) customStale = true;
    ControlPlan inversePlan;
    for (const auto field : order) {
        const auto inverse = context_->journal.at(field);
        if (customStale && custom(field)) { skipped.append(FieldName(field)); continue; }
        if (field == Field::Rotation && ((revisions_[Field::CenterX].user && revisions_[Field::CenterX].value > context_->baseline[Field::CenterX])
            || (revisions_[Field::CenterY].user && revisions_[Field::CenterY].value > context_->baseline[Field::CenterY]))) {
            skipped.append(FieldName(field)); continue;
        }
        if (revisions_[field].value != inverse.lastWrite) { skipped.append(FieldName(field)); continue; }
        inversePlan.writes.push_back({field,inverse.previous,inverse.targetIdentity});
    }
    const auto beforeUndo=target_.Fields();
    const bool batch = target_.BeginFieldPlan(inversePlan);
    const bool committed=batch && target_.CommitFieldPlan(inversePlan);
    if(batch && !committed) {
        target_.EndFieldPlan();
        ControlPlan rollback;
        for(const auto& write:inversePlan.writes)if(beforeUndo.contains(write.field))rollback.writes.push_back({write.field,beforeUndo.at(write.field),write.targetIdentity});
        bool restoredRollback=target_.BeginFieldPlan(rollback);
        if(restoredRollback) {
            restoredRollback=target_.CommitFieldPlan(rollback);
            for(const auto& write:rollback.writes)restoredRollback=target_.Apply(write) && restoredRollback;
            target_.EndFieldPlan();
        }
        if(!restoredRollback) {
            std::vector<Field> uncertain;for(const auto& write:inversePlan.writes)uncertain.push_back(write.field);
            NativeFieldsChanged(uncertain);
        }
        auto result=ControlResult::Failure("internal");result.content["rolledBack"]=restoredRollback;
        return result;
    }
    for (const auto& write : inversePlan.writes) {
        const auto field = write.field;
        const auto inverse = context_->journal.at(field);
        if (!committed) { skipped.append(FieldName(field)); continue; }
        if (!target_.Apply({field,inverse.previous,inverse.targetIdentity})) { skipped.append(FieldName(field)); continue; }
        auto& revision = revisions_[field]; ++revision.value; revision.user = false;
        context_->baseline[field] = revision.value; restored.append(FieldName(field));
    }
    if (batch) target_.EndFieldPlan();
    for(const auto& [identity,inverse]:context_->cameraJournal) {
        if(cameraRevisions_[identity].value!=inverse.lastWrite || !target_.Apply({Field::CameraAcceleration,inverse.previous,identity})) {
            skipped.append("camera.acceleration");continue;
        }
        auto& revision=cameraRevisions_[identity];++revision.value;revision.user=false;
        context_->cameraBaseline[identity]=revision.value;
        ++revisions_[Field::CameraAcceleration].value;revisions_[Field::CameraAcceleration].user=false;
        restored.append("camera.acceleration");
    }
    context_->cameraJournal.clear();
    context_->journal.clear();
    if (!restored.isEmpty()) {
        ++stateVersion_; ++viewVersion_; ++processingVersion_;
        if (!target_.Save()) Event(*context_,"settings_not_saved");
    }
    Event(*context_,"undo",{{"restored",restored},{"skipped",skipped}});
    target_.FinalizeMutationReceipt();
    return {{{"ok",true},{"changed",!restored.isEmpty() || sourceRestoreRequested},{"restored",restored},{"skipped",skipped},
        {"sourceRestoreRequested",sourceRestoreRequested},{"sourceRestoreFields",sourceRestoreFields}},{}};
}
} // namespace okuflow::agent
