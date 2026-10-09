#pragma once

#include "openzoom/app/settings_store.hpp"

#include <QSignalBlocker>

#include <functional>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTextBrowser;
class QToolButton;
class QWidget;
QT_END_NAMESPACE

namespace openzoom {

class ColorSchemePicker;
class MainWindow;
class OpenZoomApp;
class RenderWidget;

class UIStateManager {
public:
    UIStateManager(MainWindow& window, OpenZoomApp& app);

    settings::AdvancedConfig ReadConfigFromUI() const;
    void ApplyConfigToUI(const settings::AdvancedConfig& config);
    void RunWithSignalsBlocked(QObject* object,
                               const std::function<void()>& operation) const;
    QSignalBlocker BlockSignals(QObject* object) const;

    RenderWidget* renderWidget_{};
    QComboBox* cameraCombo_{};
    QComboBox* microphoneCombo_{};
    QListWidget* presetList_{};
    QLabel* presetDescriptionLabel_{};
    QPushButton* promotePresetButton_{};
    QComboBox* cameraFormatCombo_{};
    QLabel* cameraFormatNoticeLabel_{};
    QCheckBox* bwCheckbox_{};
    QSlider* bwSlider_{};
    QCheckBox* zoomCheckbox_{};
    QSlider* zoomSlider_{};
    QPushButton* debugButton_{};
    QPushButton* capturePhotoButton_{};
    QPushButton* recordButton_{};
    QCheckBox* focusMarkerCheckbox_{};
    QSlider* zoomCenterXSlider_{};
    QSlider* zoomCenterYSlider_{};
    QComboBox* rotationCombo_{};
    QComboBox* viewportRateCombo_{};
    QComboBox* viewportFitCombo_{};
    QComboBox* cameraAccelerationCombo_{};
    QLabel* cameraAccelerationStatusLabel_{};
    QPushButton* testCameraAccelerationButton_{};
    QComboBox* recordingCanvasCombo_{};
    QCheckBox* transcribeMicrophoneCheckbox_{};
    QCheckBox* transcriptToNotesCheckbox_{};
    QLabel* transcriptionStatusLabel_{};
    QLabel* transcriptionQuotaLabel_{};
    QLabel* transcriptPartialLabel_{};
    QPlainTextEdit* transcriptFinalsView_{};
    QCheckBox* joystickCheckbox_{};
    QCheckBox* zoomWheelAccelerationCheckbox_{};
    QToolButton* collapseButton_{};
    QWidget* controlsContainer_{};
    QCheckBox* blurCheckbox_{};
    QSlider* blurSigmaSlider_{};
    QSlider* blurRadiusSlider_{};
    QLabel* blurSigmaValueLabel_{};
    QLabel* blurRadiusValueLabel_{};
    QCheckBox* temporalSmoothCheckbox_{};
    QSlider* temporalSmoothSlider_{};
    QLabel* temporalSmoothValueLabel_{};
    QCheckBox* vlmAssistCheckbox_{};
    QCheckBox* assistiveOverlayCheckbox_{};
    QCheckBox* spatialSharpenCheckbox_{};
    QComboBox* spatialBackendCombo_{};
    QSlider* spatialSharpnessSlider_{};
    QLabel* spatialSharpnessValueLabel_{};
    QLabel* processingStatusLabel_{};
    QLabel* performanceDiagnosticsLabel_{};
    QCheckBox* stabilizationCheckbox_{};
    QCheckBox* keystoneCheckbox_{};
    QCheckBox* autoContrastCheckbox_{};
    QSlider* autoContrastStrengthSlider_{};
    QCheckBox* simpleTextClarityCheckbox_{};
    QCheckBox* textClarityCheckbox_{};
    QCheckBox* backgroundFlattenCheckbox_{};
    QSlider* backgroundFlattenStrengthSlider_{};
    QCheckBox* adaptiveBinarizationCheckbox_{};
    QSlider* sauvolaStrengthSlider_{};
    QSlider* binarizationSoftnessSlider_{};
    QComboBox* textPolarityCombo_{};
    QSlider* strokeWeightSlider_{};
    QCheckBox* smartSharpenCheckbox_{};
    QSlider* smartSharpenStrengthSlider_{};
    QCheckBox* claheCheckbox_{};
    QSlider* claheClipLimitSlider_{};
    QCheckBox* twoColorTextCheckbox_{};
    QCheckBox* textHysteresisCheckbox_{};
    QSlider* textHysteresisStrengthSlider_{};
    QCheckBox* selectiveSharpenCheckbox_{};
    QCheckBox* focusDetectionCheckbox_{};
    QSlider* focusThresholdSlider_{};
    QCheckBox* glareSuppressionCheckbox_{};
    QSlider* glareSuppressionStrengthSlider_{};
    QCheckBox* mlTextSuperResolutionCheckbox_{};
    QSlider* mlTextSuperResolutionStrengthSlider_{};
    QCheckBox* mlTextSuperResolutionPrefer2xCheckbox_{};
    QCheckBox* mlTextSuperResolutionUltra1440pCheckbox_{};
    ColorSchemePicker* displayColorPicker_{};
    QSlider* contrastSlider_{};
    QSlider* brightnessSlider_{};
    QPushButton* explainNowButton_{};
    QPushButton* readTextButton_{};
    QPushButton* aiSettingsButton_{};
    QPushButton* openNotesButton_{};
    QPushButton* setupAssistantButton_{};
    QLabel* assistantConnectionLabel_{};
    QLabel* assistantUsageLabel_{};
    QPushButton* assistantConnectButton_{};
    QTextBrowser* assistantTranscript_{};
    QPlainTextEdit* assistantPromptEdit_{};
    QCheckBox* assistantAttachFrameCheckbox_{};
    QPushButton* assistantSendButton_{};
    QPushButton* assistantStopButton_{};
    QPushButton* assistantNewButton_{};
    QListWidget* assistantHistoryList_{};
    QPushButton* assistantRenameButton_{};
    QPushButton* assistantExportButton_{};
    QPushButton* assistantDeleteButton_{};

private:
    OpenZoomApp& app_;
};

} // namespace openzoom
