#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QObject>
#include <QWidget>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QHash>

#include "okuflow/app/assistive_feature_manager.hpp"
#include "okuflow/app/cuda_surface_retry.hpp"
#include "okuflow/app/capture_handoff_policy.hpp"
#include "okuflow/app/settings_store.hpp"
#include "okuflow/app/agent/camera_source_intent.hpp"
#include "okuflow/app/recording_manager.hpp"
#include "okuflow/app/pipeline_orchestrator.hpp"
#include "okuflow/app/settings_controller.hpp"
#include "okuflow/app/ultra_clear_text_controller.hpp"
#include "okuflow/app/suspend_guard.hpp"
#include "okuflow/app/ui_state_manager.hpp"
#include "okuflow/app/user_data_paths.hpp"
#include "okuflow/capture/media_capture.hpp"
#include "okuflow/capture/phone_retry_policy.hpp"
#include "okuflow/capture/audio_capture.hpp"
#include "okuflow/common/frame_pipeline.hpp"
#include "okuflow/common/annotation_model.hpp"
#include "okuflow/common/view_transform.hpp"
#include "okuflow/cuda/cuda_interop.hpp"
#include "okuflow/ui/live_status_text.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

QT_BEGIN_NAMESPACE
class QApplication;
class QTimer;
class QThreadPool;
class QComboBox;
class QCheckBox;
class QSlider;
class QPushButton;
class QToolButton;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QEvent;
class QShowEvent;
class QPaintEvent;
class QMouseEvent;
class QPlainTextEdit;
class QResizeEvent;
class QTextBrowser;
class QWheelEvent;
QT_END_NAMESPACE

struct ID3D12Resource;

namespace okuflow {
namespace agent { class AppControlTarget; class AppControlService; class NativeControlFixture; }
enum class PhoneDiscoveryStatus;

class RenderWidget;
class JoystickOverlay;
class MainWindow;
class LanguageManager;
class InteractionController;
class UIStateManager;
class AssistiveFeatureManager;
class PipelineOrchestrator;
class CodexAgentClient;
class HttpAgentClient;
class VisibleChatSessionStore;
namespace mcp { class AppIpcEndpoint; }
class SetupAssistantDialog;
class ColorSchemePicker;
struct MicrophoneCallbackTarget;
struct CameraIngress;
struct StartupWorkerTracker;

class D3D12Presenter;
class CudaInteropSurface;

class OkuFlowApp : public QObject {
    Q_OBJECT
    friend class MainWindow;
    friend class InteractionController;
    friend class UIStateManager;
    friend class agent::AppControlTarget;
    friend class agent::NativeControlFixture;
public:
    OkuFlowApp(int& argc, char** argv);
    ~OkuFlowApp() override;

    bool Initialize();
    int Run();
    agent::AppControlService* ControlService() const { return controlService_.get(); }

private slots:
    void OnPresetSelectionChanged(QListWidgetItem* current, QListWidgetItem* previous);
    void OnCameraSelectionChanged(int index);
    void OnCameraFormatChanged(int index);
    void OnMicrophoneSelectionChanged(int index);
    void SetRecordingCanvasMode(RecordingCanvasMode mode);
    bool SetPhoneTorch(bool enabled, std::shared_ptr<std::atomic<bool>> canceled = {}, qint64 deadlineSteadyMs = 0);
    void OnBlackWhiteToggled(bool checked);
    void OnBlackWhiteThresholdChanged(int value);
    void OnZoomToggled(bool checked);
    void OnZoomAmountChanged(int value);
    void OnDebugViewToggled(bool checked);
    void OnZoomCenterXChanged(int value);
    void OnZoomCenterYChanged(int value);
    void OnRotationSelectionChanged(int index);
    void OnControlsCollapsedToggled(bool checked);
    void OnVirtualJoystickToggled(bool checked);
    void OnBlurToggled(bool checked);
    void OnBlurSigmaChanged(int value);
    void OnBlurRadiusChanged(int value);
    void OnFocusMarkerToggled(bool checked);
    void OnSpatialSharpenToggled(bool checked);
    void OnSpatialUpscalerChanged(int index);
    void OnSpatialSharpnessChanged(int value);
    void OnTemporalSmoothToggled(bool checked);
    void OnTemporalSmoothStrengthChanged(int value);
    void OnVlmAssistToggled(bool checked);
    void OnAssistiveOverlayToggled(bool checked);
    void OnStabilizationToggled(bool checked);
    void OnBumpHoldToggled(bool checked);
    void OnKeystoneToggled(bool checked);
    void OnKeystoneStepBack();
    void OnKeystonePauseResume();
    void OnKeystoneStepForward();
    void OnAutoContrastToggled(bool checked);
    void OnAutoContrastStrengthChanged(int value);
    void OnDisplayColorSchemeChanged();
    void OnContrastChanged(int value);
    void OnBrightnessChanged(int value);
    void OnTextClarityControlsChanged();
    void SetSuperResPerformanceOverride(bool enabled);

private:
    std::unique_ptr<agent::AppControlTarget> controlTarget_;
    std::unique_ptr<agent::AppControlService> controlService_;
    std::unique_ptr<CodexAgentClient> agentCodex_;
    std::unique_ptr<HttpAgentClient> agentHttp_;
    std::unique_ptr<mcp::AppIpcEndpoint> agentIpc_;
    QString agentOwner_, agentContext_;
    QString agentReceiptContext_;
    QString agentSceneContext_;
    quint64 agentSceneGeneration_{}, agentSceneSourceEpoch_{};
    QTimer* agentSceneTimer_{};
    bool agentSceneRequested_{}, agentSceneEffective_{}, agentSceneCapturePending_{};
    std::optional<ChatTurnTicket> agentReceiptTicket_;
    QHash<QString,QString> agentTranscripts_;
    std::unique_ptr<VisibleChatSessionStore> visibleChatStore_;
    QTimer* visibleChatSaveTimer_{};
    QString agentConversationId_;
    QByteArray agentConfigDigest_;
    quint64 agentGeneration_{};
    bool agentConversationSelected_{}, sendWithoutControlOnce_{};
    bool agentFrames_{}, agentPhotos_{}, agentRecording_{};
    void InitializeAgentControl();
    void RefreshAgentControlSettings();
    void TestAgentCapability();
    void StartAgentConversation();
    void SubmitAgentPrompt(const QString& prompt);
    void CancelAgentTurn(bool undo = false);
    bool SetAgentSceneEnabled(bool enabled);
    QJsonObject AgentSceneState() const;
    void CancelAgentScene();
    void TickAgentScene();
    bool AgentReady() const;
    QString photoOperationId_;
    QString RequestPhotoCapture(const QString& owner = QStringLiteral("manual"));
    std::optional<RecordingSessionInfo> RequestRecording(RecordingAudioRequest audio,
        RecordingOwnerKind ownerKind, const QString& ownerId);
    bool StopRecording(const QString& expectedSessionId);
    settings::AdvancedConfig CaptureCurrentAdvancedConfig() const;
    // preservePresetSelection keeps the caller-selected preset id authoritative
    // instead of re-deriving it from the (possibly apply-mutated) live config.
    void ApplyAdvancedConfig(const settings::AdvancedConfig& config,
                             bool preservePresetSelection = false);
    void PopulatePresetList();
    void RefreshPresetSelection(bool preserveCurrentSelection = false);
    void UpdatePresetDescription();
    void SyncCurrentConfigToPersistence(bool preservePresetSelection = false);
    void ResetCurrentConfigToDefaults();
    void PromoteCurrentConfigToPreset();
    void OpenAiSettingsDialog();
    void OpenSetupAssistant();
    void OpenUserDataFolder();
    void ChangeUserDataFolder();
    bool SetUserDataRoot(const QString& path, QString* error = nullptr);
    void OpenNotesFile();
    void SubmitOnDemandAnalysis(bool readText);
    void SubmitAssistantPrompt();
    void SubmitAssistantPromptText(const QString& prompt,
                                   bool clearAdvancedEditor,
                                   bool forceAttachFrame);
    void DispatchAssistantPrompt(const QString& prompt,
                                 bool clearAdvancedEditor,
                                 const uint8_t* bgraData,
                                 int width,
                                 int height,
                                 bool attachFrame);
    void StopAssistantRequest();
    void CancelPendingAssistantCaptures();
    void ArmAssistantCaptureDeadline();
    void SubmitFloatingAssistantPrompt(const QString& prompt);
    // Starts a fresh Advanced Assistant conversation (from the floating
    // overlay's New chat button); the next answer opens a new conversation
    // section in the lecture notes.
    void StartNewAssistantConversation();
    void StartNewAssistantConversationAs(ChatBackendKind kind);
    void PopulateAssistantHistory();
    void LoadSelectedAssistantConversation();
    void LoadAssistantConversationById(const QString& id);
    void PersistVisibleChat();
    void RenameStoredConversation(const QString& id, const QString& name);
    void DeleteStoredConversation(const QString& id);
    void CancelAssistantHistoryLoad();
    void SetAssistantBusy(bool busy);
    void AppendAssistantMessage(const QString& speaker, const QString& text);
    void InitializePlatform();
    void EnumerateCameras();
    void PollPhoneCameraDiscovery();
    void StartFallbackCamera();
    void PopulateCameraCombo();
    void EnumerateMicrophones();
    void PopulateMicrophoneCombo();
    bool StartSelectedMicrophone();
    void StopMicrophoneCapture(bool retainQueuedAudio = false);
    void RefreshCameraFormats(size_t index);
    void UpdatePhoneLightUi();
    bool StartCameraCapture(size_t index,
                            bool interactive = true,
                            bool forceCompatibility = false,
                            bool backgroundStartup = false,
                            agent::CameraSourceOrigin origin = agent::CameraSourceOrigin::Startup,
                            std::optional<QString> formatOverride = {}, const QString& operationId = {});
    bool CompleteCameraCaptureStart(bool started, bool interactive, const QString& startupError = {});
    void QueueInitialCameraStart(const CameraDescriptor& descriptor,
                                 const QString& requestedStableId,
                                 FrameCallback callback, CaptureErrorCallback errorCallback,
                                 bool requestAcceleration, bool interactive, uint64_t captureSession);
    void RecordStartupFirstPresent();
    void ConfigureStartupProfiling();
    void WriteStartupProfile();
    void StopCameraCapture(bool atProcessExit = false);
    void StopCameraCaptureAtExit();
    void DriveCameraRetirement();
    void ResumePendingCameraStart();
    void CancelCameraTransition(const QString& owner, bool failedFallback = false);
    void SetZoom(float amount, std::optional<QPointF> sceneAnchor = {}, bool enable = true, bool recenterAnchor = false);
    void SetRotationQuarterTurns(int quarters);
    bool SetDisplayColorSchemeById(const QString& id);
    void SetViewportFitMode(settings::ViewportFitModeSetting mode, bool save = true);
    void UpdateCameraAccelerationUi();
    void OnCameraAccelerationModeChanged(int index);
    void OnTestCameraAcceleration();
    void BeginCameraReconnect();
    void DriveCameraReconnect();
    void BuildCompositeAndPresent(UINT width,
                                  UINT height,
                                  CapturedFrame* originalFrame);
    void PresentFitted(const uint8_t* data,
                       UINT srcWidth,
                       UINT srcHeight,
                       bool cropToFill,
                       float centerXNorm,
                       float centerYNorm,
                       const CapturedFrame* originalFrame);
    void SetZoomCenter(float normX, float normY, bool syncUi,
                       bool preservePresetSelection = false,
                       bool persist = true);
    bool ApplyInputForces(double elapsedSeconds);
    void UpdateJoystickVisibility();
    bool HandlePanKey(int key, bool pressed);
    bool HandlePanScroll(const QWheelEvent* wheelEvent);
    void HandleZoomWheel(const QWheelEvent* wheelEvent);
    void HandleKeyboardZoom(float notches);
    void UpdateBlurUiLabels();
    void UpdateProcessingStatusLabel();
    void UpdatePerformanceDiagnosticsLabel();
    void UpdateCameraPlaceholder();
    void UpdateKeystoneTrackingUi();
    void UpdateSpatialSharpenUi();
    void UpdateTemporalSmoothUi();
    void UpdateControlEnabledStates();
    void UpdateSectionChangedCounts();
    void BeginMousePan(const QPointF& pos, const QSize& widgetSize);
    bool UpdateMousePan(const QPointF& pos);
    void EndMousePan();
    bool IsMousePanActive() const;
    bool MapViewToSource(const QPointF& pos, float& outX, float& outY) const;
    bool EnsureCudaSurface(UINT width, UINT height);
    bool ProcessFrameWithCuda(UINT width, UINT height);
    bool TryProcessRawFrameWithCuda(MediaFrame& frame,
                                    CapturedFrame* originalFrame,
                                    CaptureGpuPending* outGpuCompletionPending = nullptr);
    bool RunCudaPipeline(const ProcessingInput& input, UINT presentWidth, UINT presentHeight);
    void DrainCompletedGpuReadbacks();
    bool PrepareOriginalFrame(const MediaFrame& source, CapturedFrame& destination);
    bool PopulateOriginalFrameMetadata(const MediaFrame& source,
                                       CapturedFrame& destination) const;
    void CapturePendingPhoto(const CapturedFrame& originalFrame);
    void SaveCapturedPhotoPair(const uint8_t* processedData,
                               UINT processedWidth,
                               UINT processedHeight,
                               const CapturedFrame& originalFrame);
    void QueueAnnotationSnapshot(int reason);
    void SaveAnnotationSnapshot(const uint8_t* processedData,
                                UINT processedWidth,
                                UINT processedHeight,
                                const QVector<AnnotationStroke>& strokes,
                                const ViewTransform& transform,
                                const QString& heading);
    void ShowStatusMessage(
        const QString& message,
        int durationMs = 10000,
        LivePoliteness politeness = LivePoliteness::kPolite);
    bool RunFrameTick(double elapsedSeconds);
    void PresentPendingScene();
    void PresentLatestCudaScene(bool newCameraFrame,
                                CapturedFrame* originalFrame);
    void HandleCudaProcessingFailure();
    bool HandlePresenterFault();
    void ResolveCudaBufferFormatFromOptions();
    void HandleCameraStartFailure(const QString& message);
    void HandleCameraRuntimeFailure(uint64_t captureSession, const QString& message);
    void UpdateRotationUi();
    static void RotateNormalizedPoint(float inX, float inY, int quarterTurns, float& outX, float& outY);
    void ApplyPersistentSettings(settings::PersistentSettings settings);
    void SavePersistentSettings();
    // Ultra Clear Text: presentation-clock context and production wiring.
    UctFrameContext UltraClearTextContext(const ViewTransform& transform,
                                          UINT sceneWidth, UINT sceneHeight,
                                          UINT viewportWidth, UINT viewportHeight) const;
    void InitializeUltraClearText();

    QApplication* qtApp_{};
    bool initialized_{false};
    std::unique_ptr<LanguageManager> languageManager_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<UIStateManager> uiState_;
    std::unique_ptr<PipelineOrchestrator> pipelineOrchestrator_;
    QString currentAssistantThreadId_;
    QString pendingAssistantHistoryThreadId_;
    QString pendingAssistantPrompt_;
    struct PendingAssistantFramePrompt {
        QString prompt;
        bool clearAdvancedEditor{};
    };
    std::optional<PendingAssistantFramePrompt> pendingAssistantFramePrompt_;
    UINT64 pendingAssistantFrameReadbackId_{};
    QElapsedTimer pendingAssistantFrameReadbackTimer_;
    bool assistantResponseOpen_{false};
    bool assistantResponseReceivedText_{false};
    bool codexReady_{false};
    bool codexSignedIn_{false};
    QJsonArray codexModelCatalog_;
    QString selectedCodexModel_;

    std::unique_ptr<D3D12Presenter> presenter_;
    bool presenterFaultReported_{};

    std::vector<CameraDescriptor> cameras_;
    std::future<std::pair<PhoneDiscoveryStatus, std::vector<CameraDescriptor>>> phoneDiscovery_;
    PhoneDiscoveryStatus phoneDiscoveryStatus_{};
    std::atomic<bool> phoneDiscoveryCanceled_{false};
    QElapsedTimer phoneDiscoveryTimer_;
    QString preferredPhoneCamera_;
    bool automaticPhoneSelectionAllowed_{};
    std::shared_ptr<std::atomic<bool>> pendingCameraStartupCancel_;
    std::unordered_map<std::string, PhoneRetryPolicy> phoneRetries_;
    std::vector<VideoFormat> cameraFormats_;
    MediaCapture mediaCapture_;
    std::vector<AudioDeviceDescriptor> microphones_;
    AudioCapture audioCapture_;
    // Capture threads hold this target independently of the app. Every
    // delivery is serialized with Stop/destruction and tagged with a
    // generation, so a detached old reader cannot reach this object or a
    // later recording session after cancellation.
    std::shared_ptr<MicrophoneCallbackTarget> microphoneCallbackTarget_;
    std::shared_ptr<CameraIngress> cameraIngress_;
    struct PendingCameraStart {
        size_t index;
        bool interactive, forceCompatibility, backgroundStartup;
        std::wstring symbolicLink;
        agent::CameraSourceOrigin origin;
        std::optional<QString> formatOverride;
        QString operationId;
        quint64 generation;
    };
    std::optional<PendingCameraStart> pendingCameraStart_;
    std::shared_ptr<CameraIngress> retiredCameraIngress_;
    std::shared_ptr<std::deque<MediaFrame>> retiredCameraFrames_;
    std::shared_ptr<AsyncCleanup> cameraConsumerCleanup_;
    QElapsedTimer cameraStopTimer_;
    agent::CameraSourceIntent cameraIntent_;
    quint64 startingCameraIntentGeneration_{};
    bool cameraStopPending_{}, cameraStopFailed_{}, cameraStopReleaseSent_{}, cameraStopStartupWasPending_{};
    // Preview is latest-wins, while an active recording retains a short burst
    // so scheduler jitter does not discard a camera frame before processing.
    static constexpr std::size_t kMaxRecordingCameraFrames = 6;
    std::optional<MediaFrame> deferredGpuCameraFrame_;
    std::optional<std::uint64_t> deferredGpuSequence_;
    QElapsedTimer deferredGpuWaitTimer_;
    bool cameraActive_{};
    // True once a camera frame has been presented since the camera started;
    // drives the "Starting camera" placeholder over the view.
    bool cameraFramePresented_{};
    bool cameraStartupPending_{};
    std::shared_ptr<StartupWorkerTracker> startupWorkers_;
    QElapsedTimer startupTimer_;
    bool startupFirstFrameLogged_{};
    QString pendingCameraStartupError_;
    QString startupProfilePath_;
    QElapsedTimer startupPulseTimer_;
    std::vector<float> startupPulseDelaysMs_;
    qint64 startupWindowMs_{-1};
    qint64 startupCameraReadyMs_{-1};
    qint64 startupFirstPresentMs_{-1};
    std::uint64_t startupProcessedScenes_{};
    std::uint64_t startupPresentedFrames_{};
    std::uint64_t startupLeaseRetryTicks_{};
    std::uint64_t startupQueryRetryTicks_{};
    std::uint64_t startupRetryExpired_{};
    qint64 startupRetryMaxMs_{};
    bool startupSynchronousProfile_{};
    bool currentCaptureAccelerated_{};
    bool currentCaptureZeroCopyActive_{};
    bool captureZeroCopyAvailable_{true};
    CaptureHandoffPolicy captureHandoffPolicy_;
    bool captureZeroCopyStatusPersisted_{};
    QString captureZeroCopyFailureReason_;
    QString currentCameraAccelerationKey_;
    uint64_t cameraSessionId_{};
    int selectedCameraIndex_{-1};
    UINT processedFrameWidth_{};
    UINT processedFrameHeight_{};
    // Per-attempt queue pressure; the previous scene remains presentable.
    bool cudaSubmissionBusy_{};

    bool comInitialized_{};
    bool mfInitialized_{};

    bool blackWhiteEnabled_{};
    float blackWhiteThreshold_{0.5f};
    bool zoomEnabled_{};
    float zoomAmount_{1.0f};
    bool debugViewEnabled_{};
    bool focusMarkerEnabled_{};
    float zoomCenterX_{0.5f};
    float zoomCenterY_{0.5f};
    bool controlsCollapsed_{};
    bool virtualJoystickEnabled_{};
    bool suspendControlSync_{};
    bool blurEnabled_{};
    float blurSigma_{1.0f};
    int blurRadius_{3};
    bool temporalSmoothEnabled_{};
    float temporalSmoothAlpha_{0.25f};
    bool vlmAssistEnabled_{};
    bool assistiveOverlayEnabled_{true};
    bool spatialSharpenEnabled_{};
    SpatialUpscaler spatialUpscaler_{SpatialUpscaler::kNis};
    float spatialSharpness_{0.25f};
    int rotationQuarterTurns_{0};
    bool stabilizationEnabled_{};
    bool bumpHoldEnabled_{};
    int displayColorMode_{0};
    color_schemes::ColorScheme displayColorScheme_{};
    color_schemes::ColorLut displayColorLut_{};
    std::uint64_t displayColorLutGeneration_{1};
    float contrast_{1.0f};
    float brightness_{0.0f};
    bool keystoneEnabled_{};
    bool autoContrastEnabled_{};
    float autoContrastStrength_{0.7f};
    bool autoTextClarityEnabled_{};
    bool backgroundFlattenEnabled_{};
    float backgroundFlattenStrength_{0.8f};
    bool adaptiveBinarizationEnabled_{};
    float sauvolaStrength_{0.28f};
    float binarizationSoftness_{0.06f};
    int textPolarityMode_{};
    int strokeWeight_{};
    bool smartSharpenEnabled_{};
    float smartSharpenStrength_{0.45f};
    bool claheEnabled_{};
    float claheClipLimit_{2.0f};
    bool twoColorTextEnabled_{};
    bool textHysteresisEnabled_{};
    float textHysteresisStrength_{0.08f};
    bool selectiveSharpenEnabled_{};
    bool focusDetectionEnabled_{};
    float focusThreshold_{0.012f};
    bool glareSuppressionEnabled_{};
    float glareSuppressionStrength_{0.5f};
    bool mlTextSuperResolutionEnabled_{};
    float mlTextSuperResolutionStrength_{0.65f};
    bool mlTextSuperResolutionPrefer2x_{};
    bool mlTextSuperResolutionUltra1440p_{};
    bool superResPerformanceOverride_{};
    bool simpleUiMode_{true};
    std::vector<uint8_t> presentationBuffer_;
    std::vector<uint8_t> cpuSceneBuffer_;
    UINT cpuSceneWidth_{};
    UINT cpuSceneHeight_{};
    bool cpuSceneReady_{false};
    std::int64_t currentCameraCaptureClock100ns_{-1};
    std::uint64_t currentCameraFrameSequence_{};
    // Replaced with each processed camera generation, consumed only by its
    // first successful presentation (including a later viewport retry).
    std::optional<std::int64_t> pendingSceneCaptureClock100ns_;
    std::vector<uint8_t> assistiveBuffer_;
    std::vector<uint8_t> asyncReadbackBuffer_;
    bool pendingOnDemandAnalysis_{};
    std::uint64_t assistantCaptureGeneration_{};
    bool pendingOnDemandReadText_{};
    UINT64 pendingOnDemandReadbackId_{};
    QElapsedTimer pendingOnDemandReadbackTimer_;
    processing::CpuFramePipeline cpuPipeline_;
    processing::CpuFramePipeline capturePipeline_;
    std::unique_ptr<RecordingManager> recordingManager_;
    std::unique_ptr<QThreadPool> imageIoPool_;
    bool photoCapturePending_{};
    UINT64 pendingPhotoReadbackId_{};
    CapturedFrame pendingPhotoOriginal_;
    QElapsedTimer pendingPhotoReadbackTimer_;
    struct PendingAnnotationCapture {
        QVector<AnnotationStroke> strokes;
        ViewTransform transform;
        QString heading;
    };
    std::deque<PendingAnnotationCapture> annotationCaptureQueue_;
    std::optional<PendingAnnotationCapture> activeAnnotationCapture_;
    UINT64 pendingAnnotationReadbackId_{};
    QElapsedTimer pendingAnnotationReadbackTimer_;

    bool openNotesWhenStored_{false};
    std::unique_ptr<AssistiveFeatureManager> assistiveManager_;
    JoystickOverlay* joystickOverlay_{};
    SetupAssistantDialog* setupAssistantDialog_{};
    std::unique_ptr<InteractionController> interactionController_;

    Microsoft::WRL::ComPtr<ID3D12Resource> cudaSharedTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> cudaSuperResTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> cudaOriginalTexture_;
    std::unique_ptr<CudaInteropSurface> cudaSurface_;
    CudaSurfaceRetry cudaSurfaceRetry_;
    UINT cudaSurfaceWidth_{};
    UINT cudaSurfaceHeight_{};
    UINT cudaSuperResWidth_{};
    UINT cudaSuperResHeight_{};
    bool cudaPipelineAvailable_{};
    bool usingCudaLastFrame_{};
    bool superResPresentedLastFrame_{};
    CudaBufferFormat cudaBufferFormat_{CudaBufferFormat::kRgba8};
    bool rawCudaPathWarned_{false};
    QString transientStatusMessage_;
    qint64 transientStatusUntilMs_{0};
    LivePoliteness transientStatusPoliteness_{LivePoliteness::kPolite};
    QString lastSuperResLogState_;
    QString lastCameraError_;
    std::unique_ptr<SettingsController> settingsController_;
    std::unique_ptr<UserDataPaths> userDataPaths_;
    bool presetSelectionSyncSuspended_{false};
    bool configTrackingSuspended_{false};
    UINT presentationWidth_{0};
    UINT presentationHeight_{0};

    bool cudaSceneReady_{false};

    // Declared last so it is destroyed first: its worker callbacks retire
    // before the windows it draws into and the presenter go away.
    std::unique_ptr<UltraClearTextController> ultraClearText_;
};

} // namespace okuflow

#endif // _WIN32
