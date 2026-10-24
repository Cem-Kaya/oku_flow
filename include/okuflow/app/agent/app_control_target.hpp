#pragma once
#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/app/settings_store.hpp"
#include "okuflow/app/agent/operation_tracker.hpp"
#include "okuflow/app/recording_session_info.hpp"
#include "okuflow/app/agent/observation_service.hpp"
#include <QJsonDocument>
#include <QStringList>
#include <atomic>
#include <memory>

QT_BEGIN_NAMESPACE
class QTextToSpeech;
QT_END_NAMESPACE

namespace okuflow { class OkuFlowApp; }
namespace okuflow::agent {

// Original production slots own pipeline resets and dependent-widget updates.
// This adapter prepares an allowlisted delta and invokes explicit transitions.
class AppControlTarget final : public IAppControlTarget {
    friend class NativeControlFixture;
public:
    explicit AppControlTarget(OkuFlowApp& app);
    QJsonObject State() const override;
    std::map<Field,QJsonValue> Fields() const override;
    ControlPlan Prepare(ToolId tool, const QJsonObject& args, const QString& owner) const override;
    bool Apply(const FieldWrite& write) override;
    QJsonValue ReadField(const FieldWrite& write) const override;
    bool BeginFieldPlan(const ControlPlan& plan) override;
    bool CommitFieldPlan(const ControlPlan& plan) override;
    void SynchronizeNativeState() override { NoticeNativeChanges(); }
    void EndFieldPlan() override;
    void FinalizeMutationReceipt() override;
    bool Save() override;
    bool IsApplyingControlledTransition() const { return applying_; }
    QJsonObject Query(ToolId tool, const QJsonObject& args) const override;
    QJsonObject EffectiveValues(const ControlPlan& plan) const override;
    void Execute(ToolId tool, const QJsonObject& args, const QString& owner, ContextKind kind,
                 ControlCompletion completion) override;
    bool ModalActive() const override;
    bool UserInteracting() const override;
    bool RecordingActive() const override;
    QString RecordingSession() const override;
    QString RecordingOwner() const override;
    void StopOwnedRecording(const QString& owner) override;
    void CancelOwnedSource(const QString& owner) override;
    bool HasOwnedSource(const QString& owner) const override;
    bool ObservationStillCurrent(const AuthorizedObservation& expected) const override;
    void NoticeNativeChanges();
    void NoteNativeCameraAcceleration(const QString& symbolicLink);
    QString CreatePhoto(const QString& owner);
    void PhotoState(const QString& id, OperationState state, QJsonObject summary = {});
    void CancelPhotoReadback();
    void RecordingEnded(const RecordingSessionInfo& session);
    void FrameProcessed();
    void FramePresented(const ObservationFrame& frame);
    ObservationFrame PresentedFrame(const ViewTransform& transform, QSize source, QSize viewport);
    // Construct selected-media authority from the exact candidate that was
    // submitted, before the app commits doc.presented to that candidate.
    ObservationFrame PresentedMediaFrame(const ViewTransform& transform, QSize source, QSize viewport,
                                         quint64 sequence, qint64 pts100ns, bool gpuProcessed = false);
    bool WantsObservationReadback(const ObservationFrame& frame);
    void ObservationReadbackSubmitted(quint64 id, const ObservationFrame& frame);
    void ConsumeObservationReadback(quint64 id, const QImage& pixels);
    void CpuObservationPresented(const QImage& pixels, const ObservationFrame& frame);
    void CancelObservations();
    void CameraTransitionFailed();
    static std::map<Field,QJsonValue> ProfileFields(const settings::AdvancedConfig& config);
    ControlPlan PrepareImageParameters(ToolId tool, const QJsonObject& args) const;
    QJsonObject QueryImageParameters(ToolId tool, const QJsonObject& args) const;
    std::optional<bool> ApplyImageParameter(const FieldWrite& write);
    bool ApplyImageParameterBatch(const ControlPlan& plan);
    bool ValidateImageParameterBatch(const ControlPlan& plan) const;
    void FinalizeUctModelParameters();
    static bool ImageParameterEffectiveIntent(Field field, const std::map<Field,QJsonValue>& current);
    // Selected-media effect policy; GPU capability is supplied by the actual
    // selected backend, independently of whether a scene has been presented.
    static bool SelectedMediaImageFieldAvailable(Field field, bool gpuAvailable);
    static std::optional<Field> ImageParameterField(std::string_view id);
    std::map<Field,QJsonValue> ImageParameterFields() const;
    static std::map<Field,QJsonValue> ImageProfileFields(const settings::AdvancedConfig& config,
                                                       const color_schemes::ColorScheme& custom);
    static ControlPlan PlanImageParameterDependencies(const std::map<Field,QJsonValue>& current,
                                                      ToolId tool, const QJsonObject& args);
    ControlPlan PrepareAssistantParameters(ToolId tool, const QJsonObject& args) const;
    QJsonObject QueryAssistantParameters(ToolId tool, const QJsonObject& args) const;
    std::optional<bool> ApplyAssistantParameter(const FieldWrite& write);
    std::map<Field,QJsonValue> AssistantParameterFields() const;
    static std::optional<Field> AssistantParameterField(std::string_view id);
    void FinalizeAssistantParameters();
    ControlPlan PrepareCaptureParameters(ToolId tool, const QJsonObject& args, const QString& owner) const;
    QJsonObject QueryCaptureParameters(ToolId tool, const QJsonObject& args) const;
    std::map<Field,QJsonValue> CaptureParameterFields() const;
    std::optional<bool> ApplyCaptureParameter(const FieldWrite& write);
    void ExecuteCaptureParameters(const QJsonObject& args, const QString& owner, ContextKind kind, ControlCompletion completion);
    void RememberOutputRoot(const QString& path);
    void NoteTorchRequest(bool enabled);
    ControlPlan PrepareRecordingParameters(ToolId tool, const QJsonObject& args, const QString& owner) const;
    QJsonObject QueryRecordingParameters(ToolId tool, const QJsonObject& args) const;
    std::map<Field,QJsonValue> RecordingParameterFields() const;
    std::optional<bool> ApplyRecordingParameter(const FieldWrite& write);
    void ExecuteRecordingParameters(ToolId tool, const QJsonObject& args, const QString& owner,
                                    ContextKind kind, ControlCompletion completion);
    QJsonObject RecordingParameterOperationStatus(const QString& operationId) const;
    ControlPlan PrepareUiParameters(ToolId tool, const QJsonObject& args) const;
    QJsonObject QueryUiParameters(ToolId tool, const QJsonObject& args) const;
    std::map<Field,QJsonValue> UiParameterFields() const;
    std::optional<bool> ApplyUiParameter(const FieldWrite& write);
    static std::optional<Field> UiParameterField(std::string_view id);
    static ControlPlan PlanUiParameterDependencies(const std::map<Field,QJsonValue>& current,
                                                   ToolId tool, const QJsonObject& args);
    ControlPlan PrepareVideoControl(ToolId tool, const QJsonObject& args) const;
    QJsonObject QueryVideoState() const;
    void ExecuteVideoControl(ToolId tool, const QJsonObject& args, const QString& owner,
                             ControlCompletion completion);
private:
    QString CameraHandle(const QString& symbolicLink) const;
    mutable std::map<QString,QString> cameraHandles_;
    mutable std::map<QString,QString> formatHandles_, microphoneHandles_, directoryHandles_;
    QStringList approvedOutputRoots_;
    bool outputRootPending_{};
    std::optional<bool> torchRequested_;
    quint64 torchSourceEpoch_{};
    QString torchOwner_, torchOperation_;
    std::shared_ptr<std::atomic<bool>> torchCanceled_;
    qint64 torchDeadline_{};
    OkuFlowApp& app_;
    bool applying_{};
    bool fieldPlanActive_{}, fieldPlanPreviousApplying_{};
    QString fieldPlanPreset_;
    bool fieldPlanWritesPreset_{};
    bool uctModelsPending_{};
    std::optional<bool> uctEnabledPending_;
    std::map<Field,QJsonValue> observed_;
    mutable OperationTracker operations_;
    std::map<QString,QString> recordingOperations_;
    std::map<QString,QString> ownedRecordingByOwner_;
    ObservationService observations_;
    ControlStamp processedStamp_;
    ControlStamp presentedStamp_;
    bool processed_{}, presented_{};
    bool processedInputSeen_{};
    quint64 processedCameraSession_{};
    quint64 frameSequence_{}, observationReadbackId_{};
    qint64 processedAt_{}, observationSubmittedAt_{};
    std::deque<qint64> uniqueFrameTimes_;
    ObservationFrame readbackFrame_;
    QSize lastViewport_;
    QJsonObject restrictedEffects_;
    mutable std::map<QString,QStringList> assistantVoiceHandles_;
    mutable QTextToSpeech* assistantSpeechInventory_{};
    mutable QString assistantVoiceInventoryEngine_;
    bool assistantParametersPending_{}, assistantParametersQueued_{}, assistantSettingsSaved_{true};
    bool assistantRuntimeSettingsPending_{};
};
} // namespace okuflow::agent
