#pragma once

#include "okuflow/app/agent/control_registry.hpp"
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <vector>
#include <set>

namespace okuflow::agent {

enum class Field {
    ZoomEnabled, ZoomAmount, CenterX, CenterY, Framing, Rotation, Camera,
    BlackWhite, Threshold, Blur, BlurSigma, BlurRadius, Temporal, TemporalAlpha,
    Spatial, Upscaler, Sharpness, FocusMarker, Stabilization, Keystone,
    Scheme, Contrast, Brightness, AutoContrast, AutoContrastStrength, TextClarity,
    Flatten, FlattenStrength, Adaptive, Sauvola, Softness, Polarity, Stroke,
    SmartSharpen, SmartStrength, Clahe, ClaheLimit, TwoColor, Hysteresis,
    HysteresisStrength, SelectiveSharpen, FocusDetection, FocusThreshold,
    Glare, GlareStrength, Preset, PresetList, Recording,
    AssistantSceneExplain, AssistantOverlay, AssistantProvider, AssistantCodexModel, AssistantReasoning,
    AssistantHttpModel, AssistantInstructions, AssistantExplainPrompt, AssistantVoice, AssistantTtsRate, LectureNotes,
    DebugView, SuperResEnabled, SuperResStrength, SuperResPrefer2x, SuperResUltra1440p,
    SuperResPerformanceOverride, BumpHold, KeystonePaused, CustomColorMode, CustomColorStopCount,
    CustomColorStops, CustomColorStepped, CustomColorRecipe, PresentationRate, VirtualJoystick, ZoomWheelAcceleration,
    UiMode, UiInspectorWidth, UiLanguage, UiHidden, UiSectionExpanded, UiAssistiveDock, UiAssistiveGeometry,
    UiSetupPromptDeclined, UiActiveTab, AnnotationColor, AnnotationWidth, AnnotationTextSize, AnnotationDashed,
    AnnotationShape, AnnotationCaptureOnExit, AnnotationTool, AnnotationEnabled,
    CameraFormat, CameraAcceleration, CameraTorch, RecordingMicrophone, RecordingCanvas, NotesRoot
};
enum class ContextKind { EmbeddedTurn, ExternalLease };
enum class ApprovalDomain { None, Configuration };
QString FieldName(Field field);
QString ParameterId(Field field);
struct FieldWrite { Field field; QJsonValue value; QString targetIdentity; bool retryUnacknowledged{}; };
struct ControlStamp {
    quint64 sourceEpoch{1}, processingVersion{}, viewVersion{}, geometryEpoch{1};
    bool operator==(const ControlStamp&) const = default;
};
struct ControlPlan {
    std::vector<FieldWrite> writes;
    std::vector<Field> observedFields;
    QString targetIdentity;
    QString error;
    bool resetsHistory{};
    bool ownedRecording{}, terminalRecording{};
    bool requiresApproval{};
    bool requiresFrames{};
    bool requiresPhotos{};
    bool requiresRecording{}, asynchronous{};
    ApprovalDomain approvalDomain{ApprovalDomain::None};
    QString targetDisplay, startedAt;
};
struct ControlResult {
    QJsonObject content;
    QByteArray jpeg;
    static ControlResult Failure(const QString& code, bool retryable = false);
};
using ControlCompletion = std::function<void(ControlResult)>;

// args is normalized by the service. Targets accept only the finite ToolId/Field
// contracts, never arbitrary settings keys. They must return safe DTOs only.
class IAppControlTarget {
public:
    virtual ~IAppControlTarget() = default;
    virtual QJsonObject State() const = 0;
    virtual std::map<Field, QJsonValue> Fields() const = 0;
    virtual ControlPlan Prepare(ToolId tool, const QJsonObject& args, const QString& owner) const = 0;
    virtual QJsonValue ReadField(const FieldWrite& write) const {
        const auto fields=Fields();const auto found=fields.find(write.field);
        return found==fields.end()?QJsonValue(QJsonValue::Undefined):found->second;
    }
    virtual bool Apply(const FieldWrite& write) = 0;
    virtual bool BeginFieldPlan(const ControlPlan&) { return true; }
    virtual bool CommitFieldPlan(const ControlPlan&) { return true; }
    virtual void SynchronizeNativeState() {}
    virtual void EndFieldPlan() {}
    virtual void FinalizeMutationReceipt() {}
    virtual bool Save() = 0;
    virtual QJsonObject Query(ToolId tool, const QJsonObject& args) const = 0;
    virtual QJsonObject EffectiveValues(const ControlPlan&) const { return {}; }
    virtual void Execute(ToolId tool, const QJsonObject& args, const QString& owner, ContextKind kind,
                         ControlCompletion completion) = 0;
    virtual bool ModalActive() const = 0;
    virtual bool UserInteracting() const = 0;
    virtual bool RecordingActive() const = 0;
    virtual QString RecordingSession() const = 0;
    virtual QString RecordingOwner() const = 0;
    virtual void StopOwnedRecording(const QString& owner) = 0;
    virtual void CancelOwnedSource(const QString&) {}
    virtual bool HasOwnedSource(const QString&) const { return false; }
};

struct ControlGrants {
    bool frames{};
    bool photos{};
    bool recording{};
    bool source{}; // external bundle only; embedded source always exact approval
};
struct ContextSpec {
    ContextKind kind{ContextKind::EmbeddedTurn};
    QString owner; // app-created control-session/lease identity, never model text
    quint64 epoch{};
    ControlGrants grants;
};
struct ControlRequest {
    QString contextId;
    QString callKey; // scoped provider/child/thread/generation/call or bridge key
    QString tool;
    QJsonObject arguments;
};

// All public calls are GUI-thread-only. Submit queues execution onto this
// QObject. IPC workers must first invoke Submit on the GUI thread. Only the
// app's UI may call SetEnabled/Begin/ResolveApproval/SetGrants.
class AppControlService final : public QObject {
public:
    explicit AppControlService(IAppControlTarget& target,
        CatalogEdition edition = CatalogEdition::DevCore, QObject* parent = nullptr,
        std::function<qint64()> clock = {});
    void SetEnabled(bool embedded, bool external);
    QString Begin(const ContextSpec& spec); // empty means disabled/busy
    void End(const QString& contextId, bool normalCompletion = false);
    void RetireOwner(const QString& owner);
    void SetGrants(const QString& contextId, const ControlGrants& grants);
    void RevokeFrameGrant(const QString& owner);
    void SourceChanged();
    void OwnedSourceRequested(const QString& owner, const std::map<Field,QJsonValue>& previous = {});
    void NativeFieldsChanged(const std::vector<Field>& fields);
    void NativeCameraAccelerationChanged(const QString& cameraHandle);
    void Submit(ControlRequest request, ControlCompletion completion);
    void Cancel(const QString& contextId, const QString& callKey);
    bool ValidateArguments(const QString& tool, const QJsonObject& arguments) const;
    QJsonObject ContextStatus() const;
    bool AuthorizeFrameDelivery(const QString& contextId, ControlStamp expected);
    bool IsContextActive(const QString& contextId);
    quint64 FieldRevision(Field field) const;
    ControlResult ApplyDeferredFields(const QString& contextId, ToolId tool, const QJsonObject& args, ControlPlan plan);
    ControlResult ResolveApproval(const QString& approvalId, bool allow);
    QJsonObject PendingApproval() const;
    ControlResult StopAndUndo();
    void SetEventSink(std::function<void(const QJsonObject&)> sink);
    CatalogEdition Edition() const { return edition_; }
    ControlStamp Stamp() const { return {sourceEpoch_,processingVersion_,viewVersion_,geometryEpoch_}; }
    void InvalidateGeometry(bool processing = false);
private:
    struct Revision { quint64 value{}; bool user{}; };
    struct Inverse { QJsonValue previous; quint64 lastWrite{}; QString targetIdentity; };
    struct Reservation {
        QByteArray digest; ControlResult result; bool completed{};
        std::vector<ControlCompletion> listeners;
        qint64 deadline{};
        bool counted{}, mutation{};
        ControlStamp imageStamp;
    };
    struct Context {
        QString id;
        ContextSpec spec;
        bool active{true};
        qint64 started{}, lastCall{}, ended{};
        int mutations{}, approvals{}, observations{};
        int imageBytes{}, imagesDelivered{};
        int readsInFlight{}, mutationsInFlight{};
        double tokens{4};
        qint64 refill{}, compositeAt{-2000};
        std::map<Field, quint64> baseline;
        std::map<Field, Inverse> journal;
        std::map<QString, Inverse> cameraJournal;
        std::map<Field, Inverse> sourceJournal;
        std::map<QString, quint64> cameraBaseline;
        std::map<QString, Reservation> calls;
        std::set<QString> queuedKeys, canceledKeys;
        std::map<Field, qint64> flashAt;
        std::deque<QJsonObject> events;
        quint64 eventCursor{};
        std::vector<QByteArray> denied;
    };
    struct Approval {
        QString id, contextId;
        quint64 epoch{}, sourceEpoch{};
        ToolId tool{};
        QJsonObject args;
        QByteArray digest;
        ControlPlan plan;
        std::map<Field, quint64> revisions;
        qint64 expires{};
        bool frame{}, pending{true};
    };
    void Dispatch(const ControlRequest& request, ControlCompletion completion);
    void Continue(const ControlRequest& request, ToolId tool);
    ControlResult Run(Context& context, ToolId tool, const QJsonObject& args,
                      const ControlPlan& plan, bool approved, bool alreadyAdmitted = false);
    std::optional<QJsonObject> Normalize(const ToolDescriptor& tool,
                                        const QJsonObject& arguments) const;
    bool ValidContext(Context& context);
    bool Authorized(const Context& context, ToolId tool, const ControlPlan& plan) const;
    bool HasDomainGrant(const Context& context, ToolId tool, const ControlPlan& plan) const;
    bool Stale(const Context& context, const ControlPlan& plan) const;
    ControlResult Ask(Context& context, ToolId tool, const QJsonObject& args,
                      const ControlPlan& plan, const QByteArray& digest);
    void Event(Context& context, const QString& type, QJsonObject data = {});
    void Finish(const QString& contextId, const QString& key, ControlResult result);
    ControlResult Delivery(Context& context, const Reservation& reservation);
    void Expire();
    qint64 Now() const;
    IAppControlTarget& target_;
    CatalogEdition edition_;
    bool embeddedEnabled_{}, externalEnabled_{};
    std::optional<Context> context_;
    std::optional<Approval> approval_;
    std::map<Field, Revision> revisions_;
    std::map<QString, Revision> cameraRevisions_;
    quint64 stateVersion_{}, viewVersion_{}, processingVersion_{}, sourceEpoch_{1}, geometryEpoch_{1};
    int queued_{};
    QTimer expiryTimer_;
    std::function<void(const QJsonObject&)> eventSink_;
    quint64 eventSinkGeneration_{};
    QString nextFrameOwner_;
    quint64 nextFrameSource_{};
    qint64 nextFrameExpires_{};
    std::function<qint64()> clock_;
};
} // namespace okuflow::agent
