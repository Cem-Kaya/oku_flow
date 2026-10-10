#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QAbstractNativeEventFilter>
#include <QMainWindow>
#include <QWidget>
#include <QPointF>
#include <QMap>
#include <QRectF>

#include <array>
#include <functional>
#include <vector>

#include "okuflow/ui/render_widget.hpp"
#include "okuflow/ui/assistive_overlay.hpp"
#include "okuflow/ui/annotation_overlay.hpp"
#include "okuflow/ui/joystick_overlay.hpp"

QT_BEGIN_NAMESPACE
class QAbstractButton;
class QBoxLayout;
class QTabWidget;
class QComboBox;
class QCheckBox;
class QListWidgetItem;
class QParallelAnimationGroup;
class QSlider;
class QSplitter;
class QPushButton;
class QToolButton;
class QLabel;
class QLineEdit;
class QListWidget;
class QTimer;
class QEvent;
class QShowEvent;
class QResizeEvent;
class QScrollArea;
class QPaintEvent;
class QMouseEvent;
class QPlainTextEdit;
class QWheelEvent;
class QKeyEvent;
class QTextBrowser;
QT_END_NAMESPACE

namespace okuflow {

class OkuFlowApp;
class ColorSchemePicker;
class CollapsibleSection;
namespace settings { struct AdvancedConfig; }



class MainWindow : public QMainWindow, public QAbstractNativeEventFilter {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    void setApp(OkuFlowApp* app);

    RenderWidget* renderWidget() const;
    QComboBox* cameraCombo() const;
    QComboBox* microphoneCombo() const;
    QListWidget* presetList() const;
    QLabel* presetDescriptionLabel() const;
    QPushButton* promotePresetButton() const;
    QCheckBox* blackWhiteCheckbox() const;
    QSlider* blackWhiteSlider() const;
    QCheckBox* zoomCheckbox() const;
    QSlider* zoomSlider() const;
    QPushButton* debugButton() const;
    QCheckBox* focusMarkerCheckbox() const;
    QSlider* zoomCenterXSlider() const;
    QSlider* zoomCenterYSlider() const;
    QCheckBox* joystickCheckbox() const;
    QToolButton* controlsToggleButton() const;
    QWidget* controlsContainer() const;
    QCheckBox* blurCheckbox() const;
    QSlider* blurSigmaSlider() const;
    QSlider* blurRadiusSlider() const;
    QLabel* blurSigmaValueLabel() const;
    QLabel* blurRadiusValueLabel() const;
    QComboBox* cameraFormatCombo() const;
    QLabel* cameraFormatNoticeLabel() const;
    QCheckBox* zoomWheelAccelerationCheckbox() const;
    QPushButton* capturePhotoButton() const;
    QPushButton* recordButton() const;
    QPushButton* annotationButton() const;
    AnnotationOverlay* annotationOverlay() const;
    QCheckBox* temporalSmoothCheckbox() const;
    QSlider* temporalSmoothSlider() const;
    QLabel* temporalSmoothValueLabel() const;
    QCheckBox* vlmAssistCheckbox() const;
    QCheckBox* assistiveOverlayCheckbox() const;
    QCheckBox* spatialSharpenCheckbox() const;
    QComboBox* spatialBackendCombo() const;
    QSlider* spatialSharpnessSlider() const;
    QLabel* spatialSharpnessValueLabel() const;
    QLabel* processingStatusLabel() const;
    QLabel* performanceDiagnosticsLabel() const;
    QComboBox* rotationCombo() const;
    QComboBox* viewportRateCombo() const;
    QComboBox* viewportFitCombo() const;
    QComboBox* cameraAccelerationCombo() const;
    QLabel* cameraAccelerationStatusLabel() const;
    QPushButton* testCameraAccelerationButton() const;
    QComboBox* recordingCanvasCombo() const;
    QComboBox* applicationLanguageCombo() const;
    QCheckBox* transcribeMicrophoneCheckbox() const;
    QCheckBox* transcriptToNotesCheckbox() const;
    QLabel* transcriptionStatusLabel() const;
    QLabel* transcriptionQuotaLabel() const;
    QLabel* transcriptPartialLabel() const;
    QPlainTextEdit* transcriptFinalsView() const;

    // Simple-mode live transcript overlay: non-activating and
    // mouse-transparent, shown above the bottom action panel only while a
    // transcription session is active. Text updates are silent (no
    // accessibility announcements per delta).
    void SetSimpleTranscriptActive(bool active);
    void SetSimpleTranscriptPartial(const QString& text);
    void AppendSimpleTranscriptFinal(const QString& text);

    // Two-speed UI: Simple overlays compact corner controls on the full render
    // surface; Advanced adds a right-side inspector.
    void setSimpleMode(bool simple);
    bool isSimpleMode() const;
    int advancedPanelWidth() const;
    void setAdvancedPanelWidth(int width);
    QAbstractButton* simpleModeButton() const;
    QAbstractButton* advancedModeButton() const;
    QPushButton* explainNowButton() const;
    QPushButton* readTextButton() const;
    QCheckBox* stabilizationCheckbox() const;
    QCheckBox* bumpHoldCheckbox() const;
    QCheckBox* keystoneCheckbox() const;
    QCheckBox* autoContrastCheckbox() const;
    QSlider* autoContrastStrengthSlider() const;
    QCheckBox* simpleTextClarityCheckbox() const;
    QCheckBox* textClarityCheckbox() const;
    QCheckBox* backgroundFlattenCheckbox() const;
    QSlider* backgroundFlattenStrengthSlider() const;
    QCheckBox* adaptiveBinarizationCheckbox() const;
    QSlider* sauvolaStrengthSlider() const;
    QSlider* binarizationSoftnessSlider() const;
    QComboBox* textPolarityCombo() const;
    QSlider* strokeWeightSlider() const;
    QCheckBox* smartSharpenCheckbox() const;
    QSlider* smartSharpenStrengthSlider() const;
    QCheckBox* claheCheckbox() const;
    QSlider* claheClipLimitSlider() const;
    QCheckBox* twoColorTextCheckbox() const;
    QCheckBox* textHysteresisCheckbox() const;
    QSlider* textHysteresisStrengthSlider() const;
    QCheckBox* selectiveSharpenCheckbox() const;
    QCheckBox* focusDetectionCheckbox() const;
    QSlider* focusThresholdSlider() const;
    QCheckBox* glareSuppressionCheckbox() const;
    QSlider* glareSuppressionStrengthSlider() const;
    QCheckBox* mlTextSuperResolutionCheckbox() const;
    QSlider* mlTextSuperResolutionStrengthSlider() const;
    QCheckBox* mlTextSuperResolutionPrefer2xCheckbox() const;
    QCheckBox* mlTextSuperResolutionUltra1440pCheckbox() const;
    ColorSchemePicker* displayColorPicker() const;
    QSlider* contrastSlider() const;
    QSlider* brightnessSlider() const;
    QPushButton* aiSettingsButton() const;
    QPushButton* openNotesButton() const;
    QPushButton* setupAssistantButton() const;
    QLabel* assistantConnectionLabel() const;
    QLabel* assistantUsageLabel() const;
    QPushButton* assistantConnectButton() const;
    QTextBrowser* assistantTranscript() const;
    QPlainTextEdit* assistantPromptEdit() const;
    QCheckBox* assistantAttachFrameCheckbox() const;
    QPushButton* assistantSendButton() const;
    QPushButton* assistantStopButton() const;
    QPushButton* assistantNewButton() const;
    QListWidget* assistantHistoryList() const;
    QPushButton* assistantRenameButton() const;
    QPushButton* assistantExportButton() const;
    QPushButton* assistantDeleteButton() const;
    bool isMaxineRuntimeInstalled() const;
    void setMaxineRuntimeInstalled(bool installed);
    void setSuperResStatus(const QString& status,
                           bool active,
                           bool performanceLimited = false);
    void setSuperResPerformanceOverrideChecked(bool checked);
    void setKeystoneTrackingControls(bool active,
                                     bool available,
                                     bool paused,
                                     bool canStepBack,
                                     bool canStepForward,
                                     bool stepPending,
                                     int position,
                                     int count);
    QMap<QString, bool> sectionStates() const;
    void setSectionStates(const QMap<QString, bool>& states);
    void updateSectionChangedCounts(const settings::AdvancedConfig& current,
                                    const settings::AdvancedConfig& defaults);
    void setAnnotationPreferences(const QColor& color,
                                  int widthPixels,
                                  bool captureOnExit,
                                  bool dashed,
                                  const QString& shapeKind,
                                  int textSizePixels);
    void setAnnotationViewTransform(const ViewTransform& transform);
    void setAnnotationMode(bool enabled);
    // Centered camera-state message shown over the view while no camera
    // picture is presented (starting, offline, reconnecting). An empty title
    // hides it. Visual only: the pipeline status label owns announcements.
    void setCameraPlaceholder(const QString& title, const QString& detail);
    void setExplainBusy(bool busy);
    void refreshSliderReadouts();
    void refreshTextClarityUi();
    void setUiHidden(bool hidden);
    bool isUiHidden() const { return uiHidden_; }
    QToolButton* uiVisibilityButton() const { return uiVisibilityButton_; }

signals:
    void quickModeActivated(int row);
    void openUserDataFolderRequested();
    void changeUserDataFolderRequested();
    void keystoneStepBackRequested();
    void keystonePauseResumeRequested();
    void keystoneStepForwardRequested();
    void resetCurrentProfileRequested();
    void superResPerformanceOverrideChanged(bool enabled);
    void sectionStatesChanged();
    // reason: 0 explicit snapshot, 1 clear, 2 exit.
    void annotationSnapshotRequested(int reason);
    void annotationPreferencesChanged(const QString& colorName,
                                      int widthPixels,
                                      bool captureOnExit,
                                      bool dashed,
                                      const QString& shapeKind,
                                      int textSizePixels);
    void applicationLanguageRequested(const QString& code);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void ActivatePresetRow(int row);
    void ActivateRelativePreset(int offset);
    void ToggleModeGrid();
    void UpdateCurrentPresetUi(QListWidgetItem* current, QListWidgetItem* previous);
    void ShowModeAnnouncement(const QString& label, const QString& profileName);
    bool updatingChromeGeometry_{false};
    void UpdateSimpleChromeGeometry();
    void UpdateCameraPlaceholderVisibility();
    void RevealSimpleChrome();
    void FadeSimpleChrome();
    void SetChromeOpacity(qreal opacity, int durationMs, bool hideWhenFinished);
    bool SimpleChromeHasFocus() const;
    void RaiseChromeAboveCanvas();
    void RaiseDialogsAboveChrome();
    void ApplyAdvancedPanelWidth();
    void ShowHelpDialog();
    enum class SettingsScope { kImage, kShared };
    void FilterSettingsTab(SettingsScope scope, const QString& query);
    void ActivateSearchResult(SettingsScope scope);
    std::vector<QWidget*> InspectorFocusOrder(QWidget* page) const;
    void UpdateAdvancedTabToolTips();
    void UpdateProfileButtonLayout();
    void ApplyExplainBusyUi();
    QString ScopedDescriptionText(const QString& scope, const QString& description) const;
    void FocusRegion(bool backwards);
    void CloseModeGrid(bool restoreSelection);
    void UpdateUiVisibilityControl();
    std::vector<CollapsibleSection*> SettingsSections(SettingsScope scope) const;
    void UpdateDirectionalUi();

    RenderWidget* renderWidget_{};
    QComboBox* cameraCombo_{};
    QComboBox* microphoneCombo_{};
    QListWidget* presetList_{};
    QLabel* presetDescriptionLabel_{};
    QPushButton* promotePresetButton_{};
    QCheckBox* bwCheckbox_{};
    QSlider* bwSlider_{};
    QCheckBox* zoomCheckbox_{};
    QSlider* zoomSlider_{};
    QCheckBox* blurCheckbox_{};
    QSlider* blurSigmaSlider_{};
    QSlider* blurRadiusSlider_{};
    QLabel* blurSigmaValueLabel_{};
    QLabel* blurRadiusValueLabel_{};
    QComboBox* cameraFormatCombo_{};
    QLabel* cameraFormatNoticeLabel_{};
    QCheckBox* zoomWheelAccelerationCheckbox_{};
    QWidget* imageTabPage_{};
    QWidget* settingsTabPage_{};
    QTabWidget* advancedTabs_{};
    QLineEdit* imageSearchEdit_{};
    QLineEdit* sharedSearchEdit_{};
    QLabel* imageSearchStatusLabel_{};
    QLabel* sharedSearchStatusLabel_{};
    QWidget* sharedSettingsContent_{};
    QScrollArea* sharedSettingsScroll_{};
    QLabel* currentModeSummaryLabel_{};
    QBoxLayout* profileButtonsLayout_{};
    QWidget* firstImageSearchMatch_{};
    QWidget* firstSharedSearchMatch_{};
    bool explainBusy_{};
    int gridOriginalRow_{-1};
    bool gridBrowsing_{};
    struct ScopedDescription { QWidget* widget; QString scope; QString description; };
    std::vector<ScopedDescription> scopedDescriptions_;
    QPushButton* capturePhotoButton_{};
    QPushButton* recordButton_{};
    QPushButton* annotationButton_{};
    AnnotationOverlay* annotationOverlay_{};
    bool changingAnnotationMode_{};
    QCheckBox* temporalSmoothCheckbox_{};
    QSlider* temporalSmoothSlider_{};
    QLabel* temporalSmoothValueLabel_{};
    QCheckBox* vlmAssistCheckbox_{};
    QCheckBox* assistiveOverlayCheckbox_{};
    QCheckBox* annotationCaptureOnExitCheckbox_{};
    QCheckBox* spatialSharpenCheckbox_{};
    QComboBox* spatialBackendCombo_{};
    QSlider* spatialSharpnessSlider_{};
    QLabel* spatialSharpnessValueLabel_{};
    QPushButton* debugButton_{};
    QCheckBox* focusMarkerCheckbox_{};
    QSlider* zoomCenterXSlider_{};
    QSlider* zoomCenterYSlider_{};
    QCheckBox* joystickCheckbox_{};
    QToolButton* helpButton_{};
    QToolButton* previousAdvancedTabButton_{};
    QToolButton* nextAdvancedTabButton_{};
    QPushButton* resetProfileButton_{};
    QToolButton* controlsToggleButton_{};
    QWidget* controlsContainer_{};
    QLabel* processingStatusLabel_{};
    QLabel* performanceDiagnosticsLabel_{};
    QLabel* maxineAttribution_{};
    bool maxineRuntimeInstalled_{};
    // Re-render slider value readouts after a locale change (decimal marks).
    std::vector<std::function<void()>> sliderReadoutRefreshers_;
    OkuFlowApp* app_{};
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
    QWidget* simpleTranscriptPanel_{};
    QLabel* simpleTranscriptLabel_{};
    QString simpleTranscriptPartial_;
    QStringList simpleTranscriptFinals_;
    bool simpleTranscriptActive_{false};
    void RefreshSimpleTranscriptText();
    QComboBox* applicationLanguageCombo_{};
    CollapsibleSection* applicationSection_{};
    CollapsibleSection* deviceSection_{};
    CollapsibleSection* deviceMoreSection_{};
    CollapsibleSection* recordingSection_{};
    CollapsibleSection* magnificationSection_{};
    CollapsibleSection* readabilitySection_{};
    CollapsibleSection* stabilitySection_{};
    CollapsibleSection* screenFixSection_{};
    CollapsibleSection* textClaritySection_{};
    CollapsibleSection* textClarityFineSection_{};
    CollapsibleSection* notesFilesSection_{};
    CollapsibleSection* aiDownloadsSection_{};
    CollapsibleSection* troubleshootingSection_{};
    CollapsibleSection* assistantSection_{};
    CollapsibleSection* sharpeningSection_{};
    CollapsibleSection* diagnosticsSection_{};
    QWidget* advancedPanel_{};
    QScrollArea* advancedScroll_{};
    QSplitter* contentSplitter_{};
    int advancedPanelPreferredWidth_{520};
    QWidget* topLeftPanel_{};
    QWidget* bottomLeftPanel_{};
    QWidget* keystoneTrackingPanel_{};
    QWidget* bottomRightPanel_{};
    QWidget* modeGridPopup_{};
    QWidget* modeToast_{};
    QLabel* modeToastTitle_{};
    QLabel* modeToastSubtitle_{};
    QWidget* cameraPlaceholder_{};
    QLabel* cameraPlaceholderTitle_{};
    QLabel* cameraPlaceholderDetail_{};
    QToolButton* modeGridButton_{};
    QPushButton* previousModeButton_{};
    QPushButton* currentModeButton_{};
    QPushButton* nextModeButton_{};
    QPushButton* simpleKeystoneBackButton_{};
    QPushButton* simpleKeystonePauseButton_{};
    QPushButton* simpleKeystoneNextButton_{};
    QTimer* simpleChromeIdleTimer_{};
    QTimer* modeToastTimer_{};
    QParallelAnimationGroup* chromeAnimation_{};
    bool simpleChromeVisible_{true};
    bool chromePinned_{};
    bool chromeKeyboardFocus_{};
    QPointF chromeMouseGlobalPosition_{};
    QPoint chromeNativeMousePosition_{};
    bool chromeMousePositionKnown_{};
    bool chromeNativeMousePositionKnown_{};
    bool uiHidden_{};
    bool modeBeforeUiHiddenSimple_{true};
    QWidget* uiVisibilityPanel_{};
    QToolButton* uiVisibilityButton_{};
    bool keystoneTrackingActive_{};
    QPushButton* simpleModeButton_{};
    QPushButton* advancedModeButton_{};
    QPushButton* explainNowButton_{};
    QPushButton* readTextButton_{};
    QCheckBox* stabilizationCheckbox_{};
    QCheckBox* bumpHoldCheckbox_{};
    QCheckBox* keystoneCheckbox_{};
    QWidget* advancedKeystoneTrackingRow_{};
    QPushButton* advancedKeystoneBackButton_{};
    QPushButton* advancedKeystonePauseButton_{};
    QPushButton* advancedKeystoneNextButton_{};
    QCheckBox* autoContrastCheckbox_{};
    QSlider* autoContrastStrengthSlider_{};
    QCheckBox* simpleTextClarityCheckbox_{};
    QCheckBox* textClarityCheckbox_{};
    QLabel* textClarityHelp_{};
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
    QLabel* mlTextSuperResolutionStatusLabel_{};
    QCheckBox* mlTextSuperResolutionOverrideCheckbox_{};
    ColorSchemePicker* displayColorPicker_{};
    QSlider* contrastSlider_{};
    QSlider* brightnessSlider_{};
    QPushButton* aiSettingsButton_{};
    QPushButton* openNotesButton_{};
    QPushButton* openUserDataFolderButton_{};
    QPushButton* changeUserDataFolderButton_{};
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
};

} // namespace okuflow

#endif // _WIN32
