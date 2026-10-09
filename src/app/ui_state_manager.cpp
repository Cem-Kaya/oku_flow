#include "okuflow/app/ui_state_manager.hpp"

#include "okuflow/app/app.hpp"
#include "okuflow/app/constants.hpp"
#include "okuflow/ui/color_scheme_picker.hpp"
#include "okuflow/ui/main_window.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTextBrowser>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <utility>

namespace okuflow {

using namespace okuflow::app_constants;

UIStateManager::UIStateManager(MainWindow& window, OkuFlowApp& app)
    : app_(app)
{
#define OKUFLOW_BIND_WIDGET(member, accessor) \
    member = window.accessor(); \
    Q_ASSERT_X(member, "UIStateManager", #accessor " returned null")

    OKUFLOW_BIND_WIDGET(renderWidget_, renderWidget);
    OKUFLOW_BIND_WIDGET(cameraCombo_, cameraCombo);
    OKUFLOW_BIND_WIDGET(microphoneCombo_, microphoneCombo);
    OKUFLOW_BIND_WIDGET(presetList_, presetList);
    OKUFLOW_BIND_WIDGET(presetDescriptionLabel_, presetDescriptionLabel);
    OKUFLOW_BIND_WIDGET(promotePresetButton_, promotePresetButton);
    OKUFLOW_BIND_WIDGET(cameraFormatCombo_, cameraFormatCombo);
    OKUFLOW_BIND_WIDGET(cameraFormatNoticeLabel_, cameraFormatNoticeLabel);
    OKUFLOW_BIND_WIDGET(bwCheckbox_, blackWhiteCheckbox);
    OKUFLOW_BIND_WIDGET(bwSlider_, blackWhiteSlider);
    OKUFLOW_BIND_WIDGET(zoomCheckbox_, zoomCheckbox);
    OKUFLOW_BIND_WIDGET(zoomSlider_, zoomSlider);
    OKUFLOW_BIND_WIDGET(debugButton_, debugButton);
    OKUFLOW_BIND_WIDGET(capturePhotoButton_, capturePhotoButton);
    OKUFLOW_BIND_WIDGET(recordButton_, recordButton);
    OKUFLOW_BIND_WIDGET(focusMarkerCheckbox_, focusMarkerCheckbox);
    OKUFLOW_BIND_WIDGET(zoomCenterXSlider_, zoomCenterXSlider);
    OKUFLOW_BIND_WIDGET(zoomCenterYSlider_, zoomCenterYSlider);
    OKUFLOW_BIND_WIDGET(rotationCombo_, rotationCombo);
    OKUFLOW_BIND_WIDGET(viewportRateCombo_, viewportRateCombo);
    OKUFLOW_BIND_WIDGET(viewportFitCombo_, viewportFitCombo);
    OKUFLOW_BIND_WIDGET(cameraAccelerationCombo_, cameraAccelerationCombo);
    OKUFLOW_BIND_WIDGET(cameraAccelerationStatusLabel_, cameraAccelerationStatusLabel);
    OKUFLOW_BIND_WIDGET(testCameraAccelerationButton_, testCameraAccelerationButton);
    OKUFLOW_BIND_WIDGET(recordingCanvasCombo_, recordingCanvasCombo);
    OKUFLOW_BIND_WIDGET(transcribeMicrophoneCheckbox_, transcribeMicrophoneCheckbox);
    OKUFLOW_BIND_WIDGET(transcriptToNotesCheckbox_, transcriptToNotesCheckbox);
    OKUFLOW_BIND_WIDGET(transcriptionStatusLabel_, transcriptionStatusLabel);
    OKUFLOW_BIND_WIDGET(transcriptionQuotaLabel_, transcriptionQuotaLabel);
    OKUFLOW_BIND_WIDGET(transcriptPartialLabel_, transcriptPartialLabel);
    OKUFLOW_BIND_WIDGET(transcriptFinalsView_, transcriptFinalsView);
    OKUFLOW_BIND_WIDGET(joystickCheckbox_, joystickCheckbox);
    OKUFLOW_BIND_WIDGET(zoomWheelAccelerationCheckbox_, zoomWheelAccelerationCheckbox);
    OKUFLOW_BIND_WIDGET(collapseButton_, controlsToggleButton);
    OKUFLOW_BIND_WIDGET(controlsContainer_, controlsContainer);
    OKUFLOW_BIND_WIDGET(blurCheckbox_, blurCheckbox);
    OKUFLOW_BIND_WIDGET(blurSigmaSlider_, blurSigmaSlider);
    OKUFLOW_BIND_WIDGET(blurRadiusSlider_, blurRadiusSlider);
    OKUFLOW_BIND_WIDGET(blurSigmaValueLabel_, blurSigmaValueLabel);
    OKUFLOW_BIND_WIDGET(blurRadiusValueLabel_, blurRadiusValueLabel);
    OKUFLOW_BIND_WIDGET(temporalSmoothCheckbox_, temporalSmoothCheckbox);
    OKUFLOW_BIND_WIDGET(temporalSmoothSlider_, temporalSmoothSlider);
    OKUFLOW_BIND_WIDGET(temporalSmoothValueLabel_, temporalSmoothValueLabel);
    OKUFLOW_BIND_WIDGET(vlmAssistCheckbox_, vlmAssistCheckbox);
    OKUFLOW_BIND_WIDGET(assistiveOverlayCheckbox_, assistiveOverlayCheckbox);
    OKUFLOW_BIND_WIDGET(spatialSharpenCheckbox_, spatialSharpenCheckbox);
    OKUFLOW_BIND_WIDGET(spatialBackendCombo_, spatialBackendCombo);
    OKUFLOW_BIND_WIDGET(spatialSharpnessSlider_, spatialSharpnessSlider);
    OKUFLOW_BIND_WIDGET(spatialSharpnessValueLabel_, spatialSharpnessValueLabel);
    OKUFLOW_BIND_WIDGET(processingStatusLabel_, processingStatusLabel);
    OKUFLOW_BIND_WIDGET(performanceDiagnosticsLabel_, performanceDiagnosticsLabel);
    OKUFLOW_BIND_WIDGET(stabilizationCheckbox_, stabilizationCheckbox);
    OKUFLOW_BIND_WIDGET(keystoneCheckbox_, keystoneCheckbox);
    OKUFLOW_BIND_WIDGET(autoContrastCheckbox_, autoContrastCheckbox);
    OKUFLOW_BIND_WIDGET(autoContrastStrengthSlider_, autoContrastStrengthSlider);
    OKUFLOW_BIND_WIDGET(simpleTextClarityCheckbox_, simpleTextClarityCheckbox);
    OKUFLOW_BIND_WIDGET(textClarityCheckbox_, textClarityCheckbox);
    OKUFLOW_BIND_WIDGET(backgroundFlattenCheckbox_, backgroundFlattenCheckbox);
    OKUFLOW_BIND_WIDGET(backgroundFlattenStrengthSlider_, backgroundFlattenStrengthSlider);
    OKUFLOW_BIND_WIDGET(adaptiveBinarizationCheckbox_, adaptiveBinarizationCheckbox);
    OKUFLOW_BIND_WIDGET(sauvolaStrengthSlider_, sauvolaStrengthSlider);
    OKUFLOW_BIND_WIDGET(binarizationSoftnessSlider_, binarizationSoftnessSlider);
    OKUFLOW_BIND_WIDGET(textPolarityCombo_, textPolarityCombo);
    OKUFLOW_BIND_WIDGET(strokeWeightSlider_, strokeWeightSlider);
    OKUFLOW_BIND_WIDGET(smartSharpenCheckbox_, smartSharpenCheckbox);
    OKUFLOW_BIND_WIDGET(smartSharpenStrengthSlider_, smartSharpenStrengthSlider);
    OKUFLOW_BIND_WIDGET(claheCheckbox_, claheCheckbox);
    OKUFLOW_BIND_WIDGET(claheClipLimitSlider_, claheClipLimitSlider);
    OKUFLOW_BIND_WIDGET(twoColorTextCheckbox_, twoColorTextCheckbox);
    OKUFLOW_BIND_WIDGET(textHysteresisCheckbox_, textHysteresisCheckbox);
    OKUFLOW_BIND_WIDGET(textHysteresisStrengthSlider_, textHysteresisStrengthSlider);
    OKUFLOW_BIND_WIDGET(selectiveSharpenCheckbox_, selectiveSharpenCheckbox);
    OKUFLOW_BIND_WIDGET(focusDetectionCheckbox_, focusDetectionCheckbox);
    OKUFLOW_BIND_WIDGET(focusThresholdSlider_, focusThresholdSlider);
    OKUFLOW_BIND_WIDGET(glareSuppressionCheckbox_, glareSuppressionCheckbox);
    OKUFLOW_BIND_WIDGET(glareSuppressionStrengthSlider_, glareSuppressionStrengthSlider);
    OKUFLOW_BIND_WIDGET(mlTextSuperResolutionCheckbox_, mlTextSuperResolutionCheckbox);
    OKUFLOW_BIND_WIDGET(mlTextSuperResolutionStrengthSlider_, mlTextSuperResolutionStrengthSlider);
    OKUFLOW_BIND_WIDGET(mlTextSuperResolutionPrefer2xCheckbox_, mlTextSuperResolutionPrefer2xCheckbox);
    OKUFLOW_BIND_WIDGET(mlTextSuperResolutionUltra1440pCheckbox_, mlTextSuperResolutionUltra1440pCheckbox);
    OKUFLOW_BIND_WIDGET(displayColorPicker_, displayColorPicker);
    OKUFLOW_BIND_WIDGET(contrastSlider_, contrastSlider);
    OKUFLOW_BIND_WIDGET(brightnessSlider_, brightnessSlider);
    OKUFLOW_BIND_WIDGET(explainNowButton_, explainNowButton);
    OKUFLOW_BIND_WIDGET(readTextButton_, readTextButton);
    OKUFLOW_BIND_WIDGET(aiSettingsButton_, aiSettingsButton);
    OKUFLOW_BIND_WIDGET(openNotesButton_, openNotesButton);
    OKUFLOW_BIND_WIDGET(setupAssistantButton_, setupAssistantButton);
    OKUFLOW_BIND_WIDGET(assistantConnectionLabel_, assistantConnectionLabel);
    OKUFLOW_BIND_WIDGET(assistantUsageLabel_, assistantUsageLabel);
    OKUFLOW_BIND_WIDGET(assistantConnectButton_, assistantConnectButton);
    OKUFLOW_BIND_WIDGET(assistantTranscript_, assistantTranscript);
    OKUFLOW_BIND_WIDGET(assistantPromptEdit_, assistantPromptEdit);
    OKUFLOW_BIND_WIDGET(assistantAttachFrameCheckbox_, assistantAttachFrameCheckbox);
    OKUFLOW_BIND_WIDGET(assistantSendButton_, assistantSendButton);
    OKUFLOW_BIND_WIDGET(assistantStopButton_, assistantStopButton);
    OKUFLOW_BIND_WIDGET(assistantNewButton_, assistantNewButton);
    OKUFLOW_BIND_WIDGET(assistantHistoryList_, assistantHistoryList);
    OKUFLOW_BIND_WIDGET(assistantRenameButton_, assistantRenameButton);
    OKUFLOW_BIND_WIDGET(assistantExportButton_, assistantExportButton);
    OKUFLOW_BIND_WIDGET(assistantDeleteButton_, assistantDeleteButton);

#undef OKUFLOW_BIND_WIDGET
}

void UIStateManager::RunWithSignalsBlocked(
    QObject* object, const std::function<void()>& operation) const
{
    Q_ASSERT(object);
    const QSignalBlocker blocker(object);
    operation();
}

QSignalBlocker UIStateManager::BlockSignals(QObject* object) const
{
    Q_ASSERT(object);
    return QSignalBlocker(object);
}

settings::AdvancedConfig UIStateManager::ReadConfigFromUI() const
{
    settings::AdvancedConfig config;
    config.blackWhiteEnabled = app_.blackWhiteEnabled_;
    config.blackWhiteThreshold = app_.blackWhiteThreshold_;
    config.zoomEnabled = app_.zoomEnabled_;
    config.zoomAmount = app_.zoomAmount_;
    config.zoomCenterX = app_.zoomCenterX_;
    config.zoomCenterY = app_.zoomCenterY_;
    config.blurEnabled = app_.blurEnabled_;
    config.blurSigma = app_.blurSigma_;
    config.blurRadius = app_.blurRadius_;
    config.temporalSmoothEnabled = app_.temporalSmoothEnabled_;
    config.temporalSmoothAlpha = app_.temporalSmoothAlpha_;
    config.spatialSharpenEnabled = app_.spatialSharpenEnabled_;
    config.spatialUpscaler = static_cast<int>(app_.spatialUpscaler_);
    config.spatialSharpness = app_.spatialSharpness_;
    config.debugView = app_.debugViewEnabled_;
    config.focusMarker = app_.focusMarkerEnabled_;
    config.vlmAssistEnabled = app_.vlmAssistEnabled_;
    config.assistiveOverlayEnabled = app_.assistiveOverlayEnabled_;
    config.stabilizationEnabled = app_.stabilizationEnabled_;
    config.displayColorMode = app_.displayColorMode_;
    config.colorScheme = app_.displayColorScheme_;
    config.contrast = app_.contrast_;
    config.brightness = app_.brightness_;
    config.keystoneEnabled = app_.keystoneEnabled_;
    config.autoContrastEnabled = app_.autoContrastEnabled_;
    config.autoContrastStrength = app_.autoContrastStrength_;
    config.autoTextClarityEnabled = app_.autoTextClarityEnabled_;
    config.backgroundFlattenEnabled = app_.backgroundFlattenEnabled_;
    config.backgroundFlattenStrength = app_.backgroundFlattenStrength_;
    config.adaptiveBinarizationEnabled = app_.adaptiveBinarizationEnabled_;
    config.sauvolaStrength = app_.sauvolaStrength_;
    config.binarizationSoftness = app_.binarizationSoftness_;
    config.textPolarityMode = app_.textPolarityMode_;
    config.strokeWeight = app_.strokeWeight_;
    config.smartSharpenEnabled = app_.smartSharpenEnabled_;
    config.smartSharpenStrength = app_.smartSharpenStrength_;
    config.claheEnabled = app_.claheEnabled_;
    config.claheClipLimit = app_.claheClipLimit_;
    config.twoColorTextEnabled = app_.twoColorTextEnabled_;
    config.textHysteresisEnabled = app_.textHysteresisEnabled_;
    config.textHysteresisStrength = app_.textHysteresisStrength_;
    config.selectiveSharpenEnabled = app_.selectiveSharpenEnabled_;
    config.focusDetectionEnabled = app_.focusDetectionEnabled_;
    config.focusThreshold = app_.focusThreshold_;
    config.glareSuppressionEnabled = app_.glareSuppressionEnabled_;
    config.glareSuppressionStrength = app_.glareSuppressionStrength_;
    config.mlSuperResEnabled = app_.mlTextSuperResolutionEnabled_;
    config.mlSuperResStrength = app_.mlTextSuperResolutionStrength_;
    config.mlSuperResPrefer2x = app_.mlTextSuperResolutionPrefer2x_;
    config.mlSuperResUltra1440p = app_.mlTextSuperResolutionUltra1440p_;
    return app_.settingsController_->DecorateLiveConfig(std::move(config));
}

void UIStateManager::ApplyConfigToUI(const settings::AdvancedConfig& config)
{
    {
    SuspendGuard suspendConfigTracking(app_.configTrackingSuspended_);

    app_.settingsController_->MutableSettings().currentConfig = config;

    if (bwCheckbox_) {
        QSignalBlocker block(bwCheckbox_);
        bwCheckbox_->setChecked(config.blackWhiteEnabled);
    }
    app_.blackWhiteEnabled_ = config.blackWhiteEnabled;
    app_.blackWhiteThreshold_ = std::clamp(config.blackWhiteThreshold, 0.0f, 1.0f);
    if (bwSlider_) {
        const int sliderValue = std::clamp(static_cast<int>(std::round(app_.blackWhiteThreshold_ * 255.0f)),
                                           bwSlider_->minimum(), bwSlider_->maximum());
        QSignalBlocker block(bwSlider_);
        bwSlider_->setValue(sliderValue);
    }
    app_.OnBlackWhiteToggled(app_.blackWhiteEnabled_);
    app_.OnBlackWhiteThresholdChanged(static_cast<int>(std::round(app_.blackWhiteThreshold_ * 255.0f)));

    if (zoomCheckbox_) {
        QSignalBlocker block(zoomCheckbox_);
        zoomCheckbox_->setChecked(config.zoomEnabled);
    }
    app_.zoomEnabled_ = config.zoomEnabled;
    app_.zoomAmount_ = std::clamp(config.zoomAmount, 1.0f, static_cast<float>(kZoomSliderMaxMultiplier));
    if (zoomSlider_) {
        const int sliderValue = std::clamp(static_cast<int>(std::round(app_.zoomAmount_ * kZoomSliderScale)),
                                           zoomSlider_->minimum(), zoomSlider_->maximum());
        QSignalBlocker block(zoomSlider_);
        zoomSlider_->setValue(sliderValue);
    }
    app_.OnZoomAmountChanged(static_cast<int>(std::round(app_.zoomAmount_ * kZoomSliderScale)));
    app_.SetZoomCenter(config.zoomCenterX, config.zoomCenterY, true);
    app_.OnZoomToggled(app_.zoomEnabled_);

    if (blurCheckbox_) {
        QSignalBlocker block(blurCheckbox_);
        blurCheckbox_->setChecked(config.blurEnabled);
    }
    app_.blurEnabled_ = config.blurEnabled;
    app_.blurSigma_ = std::clamp(config.blurSigma, kBlurSigmaStep, static_cast<float>(kBlurSigmaSliderMax) * kBlurSigmaStep);
    app_.blurRadius_ = SnapBlurRadius(config.blurRadius);
    if (blurSigmaSlider_) {
        const int sliderValue = std::clamp(static_cast<int>(std::round(app_.blurSigma_ / kBlurSigmaStep)),
                                           blurSigmaSlider_->minimum(), blurSigmaSlider_->maximum());
        QSignalBlocker block(blurSigmaSlider_);
        blurSigmaSlider_->setValue(sliderValue);
    }
    if (blurRadiusSlider_) {
        QSignalBlocker block(blurRadiusSlider_);
        blurRadiusSlider_->setValue(app_.blurRadius_);
    }
    app_.OnBlurToggled(app_.blurEnabled_);
    app_.OnBlurSigmaChanged(static_cast<int>(std::round(app_.blurSigma_ / kBlurSigmaStep)));
    app_.OnBlurRadiusChanged(app_.blurRadius_);

    if (temporalSmoothCheckbox_) {
        QSignalBlocker block(temporalSmoothCheckbox_);
        temporalSmoothCheckbox_->setChecked(config.temporalSmoothEnabled);
    }
    app_.temporalSmoothEnabled_ = config.temporalSmoothEnabled;
    app_.temporalSmoothAlpha_ = std::clamp(config.temporalSmoothAlpha, 0.0f, 1.0f);
    if (temporalSmoothSlider_) {
        const int sliderValue = std::clamp(static_cast<int>(std::round(app_.temporalSmoothAlpha_ * 100.0f)),
                                           temporalSmoothSlider_->minimum(), temporalSmoothSlider_->maximum());
        QSignalBlocker block(temporalSmoothSlider_);
        temporalSmoothSlider_->setValue(sliderValue);
    }
    app_.OnTemporalSmoothToggled(app_.temporalSmoothEnabled_);
    app_.OnTemporalSmoothStrengthChanged(static_cast<int>(std::round(app_.temporalSmoothAlpha_ * 100.0f)));

    if (stabilizationCheckbox_) {
        QSignalBlocker block(stabilizationCheckbox_);
        stabilizationCheckbox_->setChecked(config.stabilizationEnabled);
    }
    app_.stabilizationEnabled_ = config.stabilizationEnabled;
    app_.OnStabilizationToggled(app_.stabilizationEnabled_);

    if (keystoneCheckbox_) {
        QSignalBlocker block(keystoneCheckbox_);
        keystoneCheckbox_->setChecked(config.keystoneEnabled);
    }
    app_.keystoneEnabled_ = config.keystoneEnabled;
    app_.OnKeystoneToggled(app_.keystoneEnabled_);

    if (autoContrastCheckbox_) {
        QSignalBlocker block(autoContrastCheckbox_);
        autoContrastCheckbox_->setChecked(config.autoContrastEnabled);
    }
    app_.autoContrastEnabled_ = config.autoContrastEnabled;
    app_.autoContrastStrength_ = std::clamp(config.autoContrastStrength, 0.0f, 1.0f);
    if (autoContrastStrengthSlider_) {
        const int sliderValue = std::clamp(static_cast<int>(std::round(app_.autoContrastStrength_ * 100.0f)),
                                           autoContrastStrengthSlider_->minimum(), autoContrastStrengthSlider_->maximum());
        QSignalBlocker block(autoContrastStrengthSlider_);
        autoContrastStrengthSlider_->setValue(sliderValue);
    }
    app_.OnAutoContrastToggled(app_.autoContrastEnabled_);
    app_.OnAutoContrastStrengthChanged(static_cast<int>(std::round(app_.autoContrastStrength_ * 100.0f)));

    auto setChecked = [](QCheckBox* checkbox, bool checked) {
        if (!checkbox) return;
        QSignalBlocker block(checkbox);
        checkbox->setChecked(checked);
    };
    auto setSlider = [](QSlider* slider, int value) {
        if (!slider) return;
        QSignalBlocker block(slider);
        slider->setValue(std::clamp(value, slider->minimum(), slider->maximum()));
    };
    setChecked(simpleTextClarityCheckbox_, config.autoTextClarityEnabled);
    setChecked(textClarityCheckbox_, config.autoTextClarityEnabled);
    setChecked(backgroundFlattenCheckbox_, config.backgroundFlattenEnabled);
    setSlider(backgroundFlattenStrengthSlider_, static_cast<int>(std::round(config.backgroundFlattenStrength * 100.0f)));
    setChecked(adaptiveBinarizationCheckbox_, config.adaptiveBinarizationEnabled);
    setSlider(sauvolaStrengthSlider_, static_cast<int>(std::round(config.sauvolaStrength * 100.0f)));
    setSlider(binarizationSoftnessSlider_, static_cast<int>(std::round(config.binarizationSoftness * 100.0f)));
    if (textPolarityCombo_) {
        QSignalBlocker block(textPolarityCombo_);
        textPolarityCombo_->setCurrentIndex(std::clamp(config.textPolarityMode, 0, 2));
    }
    setSlider(strokeWeightSlider_, config.strokeWeight);
    setChecked(smartSharpenCheckbox_, config.smartSharpenEnabled);
    setSlider(smartSharpenStrengthSlider_, static_cast<int>(std::round(config.smartSharpenStrength * 100.0f)));
    setChecked(claheCheckbox_, config.claheEnabled);
    setSlider(claheClipLimitSlider_, static_cast<int>(std::round(config.claheClipLimit * 10.0f)));
    setChecked(twoColorTextCheckbox_, config.twoColorTextEnabled);
    setChecked(textHysteresisCheckbox_, config.textHysteresisEnabled);
    setSlider(textHysteresisStrengthSlider_, static_cast<int>(std::round(config.textHysteresisStrength * 100.0f)));
    setChecked(selectiveSharpenCheckbox_, config.selectiveSharpenEnabled);
    setChecked(focusDetectionCheckbox_, config.focusDetectionEnabled);
    setSlider(focusThresholdSlider_, static_cast<int>(std::round(config.focusThreshold * 1000.0f)));
    setChecked(glareSuppressionCheckbox_, config.glareSuppressionEnabled);
    setSlider(glareSuppressionStrengthSlider_, static_cast<int>(std::round(config.glareSuppressionStrength * 100.0f)));
    app_.UpdateControlEnabledStates();
#if OKUFLOW_ENABLE_TEXT_SR
    setChecked(mlTextSuperResolutionCheckbox_, config.mlSuperResEnabled);
    setSlider(mlTextSuperResolutionStrengthSlider_,
              static_cast<int>(std::round(config.mlSuperResStrength * 100.0f)));
    setChecked(mlTextSuperResolutionPrefer2xCheckbox_, config.mlSuperResPrefer2x);
    setChecked(mlTextSuperResolutionUltra1440pCheckbox_, config.mlSuperResUltra1440p);
#else
    setChecked(mlTextSuperResolutionCheckbox_, false);
    setChecked(mlTextSuperResolutionPrefer2xCheckbox_, false);
    setChecked(mlTextSuperResolutionUltra1440pCheckbox_, false);
#endif
    app_.OnTextClarityControlsChanged();

    app_.displayColorScheme_ = color_schemes::NormalizeColorScheme(
        config.colorScheme, config.displayColorMode);
    app_.displayColorMode_ = app_.displayColorScheme_.legacyMode >= 0
                            ? app_.displayColorScheme_.legacyMode
                            : 0;
    if (displayColorPicker_) {
        QSignalBlocker block(displayColorPicker_);
        displayColorPicker_->setCurrentScheme(app_.displayColorScheme_);
    }
    app_.contrast_ = std::clamp(config.contrast, 0.25f, 4.0f);
    if (contrastSlider_) {
        const int sliderValue = std::clamp(static_cast<int>(std::round(app_.contrast_ * 100.0f)),
                                           contrastSlider_->minimum(), contrastSlider_->maximum());
        QSignalBlocker block(contrastSlider_);
        contrastSlider_->setValue(sliderValue);
    }
    app_.brightness_ = std::clamp(config.brightness, -1.0f, 1.0f);
    if (brightnessSlider_) {
        const int sliderValue = std::clamp(static_cast<int>(std::round(app_.brightness_ * 100.0f)),
                                           brightnessSlider_->minimum(), brightnessSlider_->maximum());
        QSignalBlocker block(brightnessSlider_);
        brightnessSlider_->setValue(sliderValue);
    }
    app_.OnDisplayColorSchemeChanged();
    app_.OnContrastChanged(static_cast<int>(std::round(app_.contrast_ * 100.0f)));
    app_.OnBrightnessChanged(static_cast<int>(std::round(app_.brightness_ * 100.0f)));

    if (vlmAssistCheckbox_) {
        QSignalBlocker block(vlmAssistCheckbox_);
        vlmAssistCheckbox_->setChecked(config.vlmAssistEnabled);
    }
    app_.vlmAssistEnabled_ = config.vlmAssistEnabled;
    if (assistiveOverlayCheckbox_) {
        QSignalBlocker block(assistiveOverlayCheckbox_);
        assistiveOverlayCheckbox_->setChecked(config.assistiveOverlayEnabled);
    }
    app_.assistiveOverlayEnabled_ = config.assistiveOverlayEnabled;
    app_.assistiveManager_->SetModes(app_.vlmAssistEnabled_,
                                     app_.assistiveOverlayEnabled_);

    if (spatialSharpenCheckbox_) {
        QSignalBlocker block(spatialSharpenCheckbox_);
        spatialSharpenCheckbox_->setChecked(config.spatialSharpenEnabled);
    }
    app_.spatialSharpenEnabled_ = config.spatialSharpenEnabled;
    app_.spatialUpscaler_ = config.spatialUpscaler == 0 ? SpatialUpscaler::kFsrEasuRcas : SpatialUpscaler::kNis;
    app_.spatialSharpness_ = std::clamp(config.spatialSharpness, 0.0f, 1.0f);
    if (spatialBackendCombo_) {
        QSignalBlocker block(spatialBackendCombo_);
        spatialBackendCombo_->setCurrentIndex(static_cast<int>(app_.spatialUpscaler_));
    }
    if (spatialSharpnessSlider_) {
        const int sliderValue = std::clamp(static_cast<int>(std::round(app_.spatialSharpness_ * 100.0f)),
                                           spatialSharpnessSlider_->minimum(), spatialSharpnessSlider_->maximum());
        QSignalBlocker block(spatialSharpnessSlider_);
        spatialSharpnessSlider_->setValue(sliderValue);
    }
    app_.OnSpatialSharpenToggled(app_.spatialSharpenEnabled_);
    app_.OnSpatialUpscalerChanged(static_cast<int>(app_.spatialUpscaler_));
    app_.OnSpatialSharpnessChanged(static_cast<int>(std::round(app_.spatialSharpness_ * 100.0f)));

    if (debugButton_) {
        QSignalBlocker block(debugButton_);
        debugButton_->setChecked(config.debugView);
    }
    app_.debugViewEnabled_ = config.debugView;
    app_.OnDebugViewToggled(app_.debugViewEnabled_);

    if (focusMarkerCheckbox_) {
        QSignalBlocker block(focusMarkerCheckbox_);
        focusMarkerCheckbox_->setChecked(config.focusMarker);
    }
    app_.focusMarkerEnabled_ = config.focusMarker;
    app_.OnFocusMarkerToggled(app_.focusMarkerEnabled_);

    }
    app_.UpdateControlEnabledStates();
    app_.UpdateSectionChangedCounts();
    app_.SyncCurrentConfigToPersistence();
    app_.UpdateProcessingStatusLabel();
}


} // namespace okuflow
