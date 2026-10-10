#ifdef _WIN32

#include "app_internal.hpp"

namespace okuflow {

settings::AdvancedConfig OkuFlowApp::CaptureCurrentAdvancedConfig() const
{
    return uiState_->ReadConfigFromUI();
}

void OkuFlowApp::PopulatePresetList()
{
    if (!uiState_->presetList_) {
        return;
    }

    SuspendGuard suspendPresetSelection(presetSelectionSyncSuspended_);
    uiState_->presetList_->clear();

    auto appendPreset = [this](const settings::PresetDefinition& preset) {
        auto* item = new QListWidgetItem(preset.name);
        item->setData(kPresetIdRole, preset.id);
        item->setToolTip(preset.description);
        uiState_->presetList_->addItem(item);
    };

    for (const settings::PresetDefinition& preset : settings::BuiltInPresets()) {
        appendPreset(preset);
    }
    for (const settings::PresetDefinition& preset : settingsController_->MutableSettings().customPresets) {
        appendPreset(preset);
    }
}

void OkuFlowApp::RefreshPresetSelection(bool preserveCurrentSelection)
{
    const settings::AdvancedConfig current = CaptureCurrentAdvancedConfig();
    const QString matchedPresetId =
        settingsController_->MatchPreset(current, preserveCurrentSelection);
    if (!uiState_->presetList_) {
        return;
    }

    SuspendGuard suspendPresetSelection(presetSelectionSyncSuspended_);
    QListWidgetItem* matchedItem = nullptr;
    for (int row = 0; row < uiState_->presetList_->count(); ++row) {
        QListWidgetItem* item = uiState_->presetList_->item(row);
        if (item && item->data(kPresetIdRole).toString() == matchedPresetId) {
            matchedItem = item;
            break;
        }
    }
    if (matchedItem) {
        uiState_->presetList_->setCurrentItem(matchedItem);
    } else {
        uiState_->presetList_->clearSelection();
        uiState_->presetList_->setCurrentItem(nullptr);
    }
}

void OkuFlowApp::UpdatePresetDescription()
{
    if (!uiState_->presetDescriptionLabel_) {
        return;
    }

    QString text;
    const QString presetId = settingsController_->MutableSettings().selectedPresetId;
    if (!presetId.isEmpty()) {
        if (const settings::PresetDefinition* preset =
                settings::FindPresetById(presetId, settingsController_->MutableSettings().customPresets)) {
            text = QStringLiteral("%1\n%2").arg(preset->name, preset->description);
        }
    }

    if (text.isEmpty()) {
        text = QStringLiteral("Custom configuration from Advanced Tuning. Save it as a quick option when it feels right.");
    }

    QString assistiveText = QStringLiteral("Assistive hooks: off");
    if (vlmAssistEnabled_) {
        assistiveText =
            assistiveOverlayEnabled_
                ? QStringLiteral(
                      "Assistive hooks: Scene Explain with overlay")
                : QStringLiteral(
                      "Assistive hooks: Scene Explain");
    }

    SetLiveText(uiState_->presetDescriptionLabel_,
                text + QStringLiteral("\n") + assistiveText,
                LivePoliteness::kSilent,
                QStringLiteral("Current quick mode description"));
}

void OkuFlowApp::SyncCurrentConfigToPersistence(bool preservePresetSelection)
{
    if (configTrackingSuspended_) {
        return;
    }
    if (!preservePresetSelection) {
        RefreshPresetSelection();
    } else {
        settingsController_->MutableSettings().currentConfig =
            CaptureCurrentAdvancedConfig();
    }
    UpdatePresetDescription();
    UpdateSectionChangedCounts();
}

void OkuFlowApp::ApplyAdvancedConfig(const settings::AdvancedConfig& config)
{
    uiState_->ApplyConfigToUI(config);
}

void OkuFlowApp::ResetCurrentConfigToDefaults()
{
    if (!mainWindow_) {
        return;
    }

    const auto answer = QMessageBox::question(
        mainWindow_.get(),
        QStringLiteral("Reset Mode"),
        QStringLiteral("Reset this mode's image and assistant settings?\n\nShared settings on the Settings tab will stay unchanged."),
        QMessageBox::Reset | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (answer != QMessageBox::Reset) {
        return;
    }

    settings::AdvancedConfig defaults;
    defaults.id = QStringLiteral("current-live");
    defaults.name = QStringLiteral("Current Setup");
    defaults.description =
        QStringLiteral("Profile tuning reset to OkuFlow defaults.");

    settingsController_->MutableSettings().selectedPresetId.clear();
    ApplyAdvancedConfig(defaults);
    settingsController_->MutableSettings().selectedPresetId.clear();
    settingsController_->MutableSettings().currentConfig =
        CaptureCurrentAdvancedConfig();
    RefreshPresetSelection();
    UpdatePresetDescription();
    SavePersistentSettings();
    ShowStatusMessage(QStringLiteral("Current profile tuning reset to defaults."),
                      4000);
}

void OkuFlowApp::PromoteCurrentConfigToPreset()
{
    if (!mainWindow_) {
        return;
    }

    bool ok = false;
    const QString name = QInputDialog::getText(mainWindow_.get(),
                                               QStringLiteral("Save as Quick Mode"),
                                               QStringLiteral("Quick mode name:"),
                                               QLineEdit::Normal,
                                               settingsController_->DefaultPromotedPresetName(),
                                               &ok).trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }

    settingsController_->PromoteCurrentConfig(CaptureCurrentAdvancedConfig(), name);

    PopulatePresetList();
    RefreshPresetSelection(true);
    UpdatePresetDescription();
}

void OkuFlowApp::OnPresetSelectionChanged(QListWidgetItem* current, QListWidgetItem* /*previous*/)
{
    if (presetSelectionSyncSuspended_ || !current) {
        return;
    }

    const QString presetId = current->data(kPresetIdRole).toString();
    auto config = settingsController_->ResolvePreset(presetId);
    if (!config) {
        return;
    }

    settingsController_->MutableSettings().selectedPresetId = presetId;
    ApplyAdvancedConfig(*config);
}

void OkuFlowApp::ApplyPersistentSettings(const settings::PersistentSettings settings) {
    // Callers can pass the controller's mutable settings. Applying controls
    // invokes handlers that save back into that same object, so take a stable
    // value snapshot before any handler can overwrite fields still to restore.
    if (uiState_->displayColorPicker_ && settings.customColorScheme.stops.size() >= 2) {
        uiState_->displayColorPicker_->setCustomScheme(settings.customColorScheme);
    }
    if (uiState_->joystickCheckbox_) {
        auto block = uiState_->BlockSignals(uiState_->joystickCheckbox_);
        uiState_->joystickCheckbox_->setChecked(settings.virtualJoystick);
    }
    virtualJoystickEnabled_ = settings.virtualJoystick;
    OnVirtualJoystickToggled(virtualJoystickEnabled_);

    if (uiState_->zoomWheelAccelerationCheckbox_) {
        auto block =
            uiState_->BlockSignals(uiState_->zoomWheelAccelerationCheckbox_);
        uiState_->zoomWheelAccelerationCheckbox_->setChecked(
            settings.zoomWheelAcceleration);
    }
    if (uiState_->collapseButton_) {
        auto block = uiState_->BlockSignals(uiState_->collapseButton_);
        uiState_->collapseButton_->setChecked(true);
    }
    controlsCollapsed_ = false;
    OnControlsCollapsedToggled(true);
    simpleUiMode_ = settings.simpleUiMode;
    pipelineOrchestrator_->SetViewportRateMode(settings.viewportRateMode);
    pipelineOrchestrator_->SetViewportFitMode(settings.viewportFitMode);
    if (recordingManager_) {
        recordingManager_->SetCanvasMode(settings.recordingCanvasMode);
    }
    if (uiState_->microphoneCombo_) {
        auto block =
            uiState_->BlockSignals(uiState_->microphoneCombo_);
        const int index =
            uiState_->microphoneCombo_->findData(
                settings.microphoneEndpointId);
        uiState_->microphoneCombo_->setCurrentIndex(
            index >= 0 ? index : 0);
    }
    {
        auto block = uiState_->BlockSignals(uiState_->viewportRateCombo_);
        uiState_->viewportRateCombo_->setCurrentIndex(
            static_cast<int>(pipelineOrchestrator_->ViewportRateMode()));
    }
    {
        auto block = uiState_->BlockSignals(uiState_->viewportFitCombo_);
        uiState_->viewportFitCombo_->setCurrentIndex(
            static_cast<int>(pipelineOrchestrator_->ViewportFitMode()));
    }
    {
        auto block =
            uiState_->BlockSignals(uiState_->recordingCanvasCombo_);
        const int index = uiState_->recordingCanvasCombo_->findData(
            static_cast<int>(settings.recordingCanvasMode));
        uiState_->recordingCanvasCombo_->setCurrentIndex(
            index >= 0 ? index : 0);
    }
    if (uiState_->transcribeMicrophoneCheckbox_) {
        auto block =
            uiState_->BlockSignals(uiState_->transcribeMicrophoneCheckbox_);
        uiState_->transcribeMicrophoneCheckbox_->setChecked(
            settings.liveTranscriptionEnabled);
    }
    if (uiState_->transcriptToNotesCheckbox_) {
        auto block =
            uiState_->BlockSignals(uiState_->transcriptToNotesCheckbox_);
        uiState_->transcriptToNotesCheckbox_->setChecked(
            settings.appendTranscriptToNotes);
        uiState_->transcriptToNotesCheckbox_->setEnabled(
            settings.liveTranscriptionEnabled);
    }
    if (mainWindow_) {
        mainWindow_->setAdvancedPanelWidth(settings.advancedPanelWidth);
        mainWindow_->setSimpleMode(settings.simpleUiMode);
        mainWindow_->setSectionStates(settings.uiSectionStates);
        mainWindow_->setAnnotationPreferences(
            QColor(settings.annotationColor),
            settings.annotationWidthPixels,
            settings.annotationCaptureOnExit,
            settings.annotationDashed,
            settings.annotationShapeKind,
            settings.annotationTextSizePixels);
    }
    assistiveManager_->RestoreOverlayGeometry(settings.assistiveOverlayGeometry);
    assistiveManager_->Overlay().SetDockPosition(settings.assistiveOverlayDockPosition);
    ApplyAdvancedConfig(settings.currentConfig);
    rotationQuarterTurns_ = ((settings.rotationQuarterTurns % 4) + 4) % 4;
    UpdateRotationUi();
    settingsController_->MutableSettings().selectedPresetId = settings.selectedPresetId;
    RefreshPresetSelection(true);
    UpdatePresetDescription();
    assistiveManager_->ApplySettings(settings.assistive);
}

void OkuFlowApp::SavePersistentSettings() {
    // Startup applies several controls that can request a settings save before
    // the persisted camera has been selected. Preserve the loaded index until
    // initialization completes instead of silently replacing it with the
    // enumeration default.
    if (initialized_) {
        settingsController_->MutableSettings().cameraIndex =
            selectedCameraIndex_;
    }
    if (uiState_->microphoneCombo_) {
        settingsController_->MutableSettings().microphoneEndpointId =
            uiState_->microphoneCombo_->currentData().toString();
    }
    settingsController_->MutableSettings().rotationQuarterTurns = rotationQuarterTurns_;
    settingsController_->MutableSettings().virtualJoystick = virtualJoystickEnabled_;
    settingsController_->MutableSettings().controlsCollapsed = false;
    if (uiState_->zoomWheelAccelerationCheckbox_) {
        settingsController_->MutableSettings().zoomWheelAcceleration =
            uiState_->zoomWheelAccelerationCheckbox_->isChecked();
    }
    settingsController_->MutableSettings().simpleUiMode = simpleUiMode_;
    settingsController_->MutableSettings().viewportRateMode =
        pipelineOrchestrator_->ViewportRateMode();
    settingsController_->MutableSettings().viewportFitMode =
        pipelineOrchestrator_->ViewportFitMode();
    if (uiState_->recordingCanvasCombo_) {
        const int storedValue =
            uiState_->recordingCanvasCombo_->currentData().toInt();
        settingsController_->MutableSettings().recordingCanvasMode =
            static_cast<RecordingCanvasMode>(std::clamp(
                storedValue,
                static_cast<int>(RecordingCanvasMode::Source),
                static_cast<int>(RecordingCanvasMode::Nhd360)));
    }
    if (uiState_->transcribeMicrophoneCheckbox_) {
        settingsController_->MutableSettings().liveTranscriptionEnabled =
            uiState_->transcribeMicrophoneCheckbox_->isChecked();
    }
    if (uiState_->transcriptToNotesCheckbox_) {
        settingsController_->MutableSettings().appendTranscriptToNotes =
            uiState_->transcriptToNotesCheckbox_->isChecked();
    }
    if (mainWindow_) {
        settingsController_->MutableSettings().advancedPanelWidth = mainWindow_->advancedPanelWidth();
        settingsController_->MutableSettings().uiSectionStates =
            mainWindow_->sectionStates();
        if (AnnotationOverlay* overlay = mainWindow_->annotationOverlay()) {
            settingsController_->MutableSettings().annotationColor =
                overlay->InkColor().name(QColor::HexRgb);
            settingsController_->MutableSettings().annotationWidthPixels =
                overlay->InkWidthPixels();
            settingsController_->MutableSettings().annotationCaptureOnExit =
                overlay->CaptureOnExit();
            settingsController_->MutableSettings().annotationDashed =
                overlay->Dashed();
            settingsController_->MutableSettings().annotationShapeKind =
                overlay->ShapeKind();
            settingsController_->MutableSettings().annotationTextSizePixels =
                overlay->TextSizePixels();
        }
    }
    settingsController_->MutableSettings().assistiveOverlayGeometry =
        assistiveManager_->OverlayGeometry();
    settingsController_->MutableSettings().assistiveOverlayDockPosition =
        assistiveManager_->Overlay().DockPosition();
    if (uiState_->displayColorPicker_ && uiState_->displayColorPicker_->hasCustomScheme()) {
        settingsController_->MutableSettings().customColorScheme = uiState_->displayColorPicker_->customScheme();
    }
    if (!settingsController_->Save(CaptureCurrentAdvancedConfig())) {
        const QString error = settingsController_->LastError().isEmpty()
                                  ? QStringLiteral("OkuFlow could not save settings.")
                                  : settingsController_->LastError();
        ShowStatusMessage(error, 10000);
    }
}


} // namespace okuflow

#endif // _WIN32
