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

#include "openzoom/app/assistive_feature_manager.hpp"
#include "openzoom/app/settings_store.hpp"
#include "openzoom/app/recording_manager.hpp"
#include "openzoom/app/pipeline_orchestrator.hpp"
#include "openzoom/app/settings_controller.hpp"
#include "openzoom/app/suspend_guard.hpp"
#include "openzoom/app/ui_state_manager.hpp"
#include "openzoom/app/user_data_paths.hpp"
#include "openzoom/capture/media_capture.hpp"
#include "openzoom/capture/audio_capture.hpp"
#include "openzoom/common/frame_pipeline.hpp"
#include "openzoom/common/annotation_model.hpp"
#include "openzoom/common/view_transform.hpp"
#include "openzoom/cuda/cuda_interop.hpp"
#include "openzoom/ui/live_status_text.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

namespace openzoom {

class RenderWidget;
class JoystickOverlay;
class MainWindow;
class InteractionController;
class UIStateManager;
class AssistiveFeatureManager;
class PipelineOrchestrator;
class SetupAssistantDialog;
class ColorSchemePicker;

class D3D12Presenter;
class CudaInteropSurface;

class OpenZoomApp : public QObject {
    Q_OBJECT
    friend class MainWindow;
    friend class InteractionController;
    friend class UIStateManager;
public:
    OpenZoomApp(int& argc, char** argv);
    ~OpenZoomApp() override;

    bool Initialize();
    int Run();

private slots:
    void OnPresetSelectionChanged(QListWidgetItem* current, QListWidgetItem* previous);
    void OnCameraSelectionChanged(int index);
    void OnCameraFormatChanged(int index);
    void OnMicrophoneSelectionChanged(int index);
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
    void OnOcrAssistToggled(bool checked);
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
    settings::AdvancedConfig CaptureCurrentAdvancedConfig() const;
    void ApplyAdvancedConfig(const settings::AdvancedConfig& config);
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
    void OfferLegacyOutputMigration();
    void OpenNotesFile();
    void SubmitOnDemandAnalysis(bool runOcr, bool runVlm);
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
    void SubmitFloatingAssistantPrompt(const QString& prompt);
    void PopulateAssistantHistory();
    void LoadSelectedAssistantConversation();
    void SetAssistantBusy(bool busy);
    void AppendAssistantMessage(const QString& speaker, const QString& text);
    void InitializePlatform();
    void EnumerateCameras();
    void PopulateCameraCombo();
    void EnumerateMicrophones();
    void PopulateMicrophoneCombo();
    bool StartSelectedMicrophone();
    void StopMicrophoneCapture(bool retainQueuedAudio = false);
    void RefreshCameraFormats(size_t index);
    bool StartCameraCapture(size_t index,
                            bool interactive = true,
                            bool forceCompatibility = false);
    void StopCameraCapture(bool atProcessExit = false);
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
                                    bool* outGpuCompletionPending = nullptr);
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
    void PresentLatestCudaScene(bool newCameraFrame,
                                CapturedFrame* originalFrame);
    void ResetCudaFenceState();
    void HandleCudaProcessingFailure();
    void ResolveCudaBufferFormatFromOptions();
    void HandleCameraStartFailure(const QString& message);
    void HandleCameraRuntimeFailure(uint64_t captureSession, const QString& message);
    void UpdateRotationUi();
    static void RotateNormalizedPoint(float inX, float inY, int quarterTurns, float& outX, float& outY);
    void ApplyPersistentSettings(const settings::PersistentSettings& settings);
    void SavePersistentSettings();

    QApplication* qtApp_{};
    bool initialized_{false};
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<UIStateManager> uiState_;
    std::unique_ptr<PipelineOrchestrator> pipelineOrchestrator_;
    QString currentAssistantThreadId_;
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

    std::vector<CameraDescriptor> cameras_;
    std::vector<VideoFormat> cameraFormats_;
    MediaCapture mediaCapture_;
    std::vector<AudioDeviceDescriptor> microphones_;
    AudioCapture audioCapture_;
    std::mutex cameraMutex_;
    // Preview is latest-wins, while an active recording retains a short burst
    // so scheduler jitter does not discard a camera frame before processing.
    static constexpr std::size_t kMaxRecordingCameraFrames = 6;
    std::deque<MediaFrame> pendingCameraFrames_;
    std::optional<MediaFrame> deferredGpuCameraFrame_;
    std::optional<std::uint64_t> deferredGpuSequence_;
    QElapsedTimer deferredGpuWaitTimer_;
    bool cameraActive_{};
    bool currentCaptureAccelerated_{};
    bool currentCaptureZeroCopyActive_{};
    bool captureZeroCopyAvailable_{true};
    bool captureZeroCopyStatusPersisted_{};
    QString captureZeroCopyFailureReason_;
    QString currentCameraAccelerationKey_;
    uint64_t cameraSessionId_{};
    int selectedCameraIndex_{-1};
    UINT processedFrameWidth_{};
    UINT processedFrameHeight_{};

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
    bool ocrAssistEnabled_{};
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
    std::vector<uint8_t> assistiveBuffer_;
    std::vector<uint8_t> asyncReadbackBuffer_;
    bool pendingOnDemandRunOcr_{};
    bool pendingOnDemandRunVlm_{};
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

    std::unique_ptr<AssistiveFeatureManager> assistiveManager_;
    JoystickOverlay* joystickOverlay_{};
    SetupAssistantDialog* setupAssistantDialog_{};
    std::unique_ptr<InteractionController> interactionController_;

    Microsoft::WRL::ComPtr<ID3D12Resource> cudaSharedTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> cudaSuperResTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> cudaOriginalTexture_;
    std::unique_ptr<CudaInteropSurface> cudaSurface_;
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
};

} // namespace openzoom

#endif // _WIN32
