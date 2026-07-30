#ifdef _WIN32

#include "app_internal.hpp"

namespace openzoom {

void OpenZoomApp::OpenAiSettingsDialog()
{
    if (!mainWindow_) {
        return;
    }
    AiSettingsDialog dialog(settingsController_->MutableSettings().assistive, mainWindow_.get());
    dialog.SetCodexModelCatalog(codexModelCatalog_, selectedCodexModel_);
    connect(&assistiveManager_->Runtime(),
            &AssistiveRuntime::CodexModelCatalogChanged,
            &dialog,
            &AiSettingsDialog::SetCodexModelCatalog);
    if (dialog.exec() == QDialog::Accepted) {
        settingsController_->MutableSettings().assistive = dialog.result();
        assistiveManager_->ApplySettings(settingsController_->MutableSettings().assistive);
        SavePersistentSettings();
    }
}

void OpenZoomApp::OpenNotesFile()
{
    const QString path = assistiveManager_->Runtime().notesFilePath();
    if (path.isEmpty()) {
        ShowStatusMessage(
            QStringLiteral("No lecture notes yet — notes appear once OCR or Explain produces text."));
        qInfo() << "Open notes skipped: no notes file written yet";
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void OpenZoomApp::SubmitOnDemandAnalysis(bool runOcr, bool runVlm)
{
    if (runOcr && (focusDetectionEnabled_ || autoTextClarityEnabled_) && cudaSurface_ &&
        !cudaSurface_->IsFocusAcceptable(focusThreshold_)) {
        const QString message = QStringLiteral(
            "Image out of focus. Tap the phone screen to refocus before reading text.");
        assistiveManager_->ShowFocusWarning();
        ShowStatusMessage(message);
        return;
    }

    // Prefer the processed GPU output. Queue the request with the next
    // viewport presentation so the existing CUDA/D3D fence contract orders
    // the copy without blocking the UI thread.
    if (usingCudaLastFrame_ && cudaSharedTexture_ && presenter_ &&
        processedFrameWidth_ > 0 && processedFrameHeight_ > 0) {
        pendingOnDemandRunOcr_ = pendingOnDemandRunOcr_ || runOcr;
        pendingOnDemandRunVlm_ = pendingOnDemandRunVlm_ || runVlm;
        pipelineOrchestrator_->MarkViewportDirty();
        ShowStatusMessage(QStringLiteral("Capturing the current view..."), 3000);
        return;
    }

    // Fall back to the CPU-converted presentation frame when GPU readback is
    // unavailable (passthrough or debug view).
    if (!presentationBuffer_.empty() && presentationWidth_ > 0 && presentationHeight_ > 0) {
        assistiveManager_->Runtime().SubmitFrameForced(presentationBuffer_.data(),
                                             static_cast<int>(presentationWidth_),
                                             static_cast<int>(presentationHeight_),
                                             runOcr, runVlm);
        return;
    }

    qWarning() << "On-demand analysis skipped: no frame available";
}

void OpenZoomApp::SubmitAssistantPrompt()
{
    if (!uiState_->assistantPromptEdit_ || assistiveManager_->Runtime().IsCodexTurnActive()) {
        return;
    }
    const QString prompt = uiState_->assistantPromptEdit_->toPlainText().trimmed();
    if (prompt.isEmpty()) {
        uiState_->assistantPromptEdit_->setFocus();
        return;
    }
    SubmitAssistantPromptText(prompt, true, false);
}

void OpenZoomApp::SubmitFloatingAssistantPrompt(const QString& prompt)
{
    if (prompt.trimmed().isEmpty()) {
        return;
    }
    SubmitAssistantPromptText(prompt.trimmed(), false, true);
}

void OpenZoomApp::SubmitAssistantPromptText(const QString& prompt,
                                            bool clearAdvancedEditor,
                                            bool forceAttachFrame)
{
    if (prompt.trimmed().isEmpty() ||
        assistiveManager_->Runtime().IsCodexTurnActive() ||
        pendingAssistantFramePrompt_) {
        return;
    }
    const bool attachFrame = forceAttachFrame ||
                             (uiState_->assistantAttachFrameCheckbox_ && uiState_->assistantAttachFrameCheckbox_->isChecked());

    if (attachFrame && usingCudaLastFrame_ && cudaSharedTexture_ && presenter_ &&
        processedFrameWidth_ > 0 && processedFrameHeight_ > 0) {
        pendingAssistantFramePrompt_ = PendingAssistantFramePrompt{
            prompt.trimmed(), clearAdvancedEditor};
        SetAssistantBusy(true);
        pipelineOrchestrator_->MarkViewportDirty();
        ShowStatusMessage(QStringLiteral("Attaching the current view..."), 3000);
        return;
    }

    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    if (attachFrame && !presentationBuffer_.empty() &&
        presentationWidth_ > 0 && presentationHeight_ > 0) {
        data = presentationBuffer_.data();
        width = static_cast<int>(presentationWidth_);
        height = static_cast<int>(presentationHeight_);
    }
    DispatchAssistantPrompt(prompt,
                            clearAdvancedEditor,
                            data,
                            width,
                            height,
                            attachFrame);
}

void OpenZoomApp::DispatchAssistantPrompt(const QString& prompt,
                                          bool clearAdvancedEditor,
                                          const uint8_t* bgraData,
                                          int width,
                                          int height,
                                          bool attachFrame)
{
    const QString submittedPrompt = prompt.trimmed();
    pendingAssistantPrompt_ = submittedPrompt;
    AppendAssistantMessage(QStringLiteral("You"), submittedPrompt);
    QTextCursor cursor = uiState_->assistantTranscript_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("OpenZoom Assistant\n"));
    uiState_->assistantTranscript_->setTextCursor(cursor);
    assistantResponseOpen_ = true;
    assistantResponseReceivedText_ = false;
    if (clearAdvancedEditor && uiState_->assistantPromptEdit_) {
        uiState_->assistantPromptEdit_->clear();
    }
    SetAssistantBusy(true);
    assistiveManager_->Runtime().SubmitAssistantPrompt(submittedPrompt,
                                                       currentAssistantThreadId_,
                                                       bgraData,
                                                       width,
                                                       height,
                                                       attachFrame);
}

void OpenZoomApp::StopAssistantRequest()
{
    if (pendingAssistantFramePrompt_) {
        pendingAssistantFramePrompt_.reset();
        pendingAssistantFrameReadbackId_ = 0;
        pendingAssistantFrameReadbackTimer_.invalidate();
        SetAssistantBusy(false);
        ShowStatusMessage(QStringLiteral("Assistant request stopped."), 3000);
        return;
    }
    assistiveManager_->Runtime().StopAssistant();
}

void OpenZoomApp::PopulateAssistantHistory()
{
    if (!uiState_->assistantHistoryList_) {
        return;
    }
    auto blocker = uiState_->BlockSignals(uiState_->assistantHistoryList_);
    uiState_->assistantHistoryList_->clear();
    std::vector<const settings::CodexConversation*> conversations;
    conversations.reserve(settingsController_->MutableSettings().codexConversations.size());
    for (const settings::CodexConversation& conversation : settingsController_->MutableSettings().codexConversations) {
        conversations.push_back(&conversation);
    }
    std::sort(conversations.begin(), conversations.end(),
              [](const settings::CodexConversation* lhs, const settings::CodexConversation* rhs) {
                  return lhs->updatedAt > rhs->updatedAt;
              });
    for (const settings::CodexConversation* conversation : conversations) {
        const qint64 timestamp = conversation->updatedAt > 0
                                     ? conversation->updatedAt
                                     : conversation->createdAt;
        const QString timeText = timestamp > 0
                                     ? QDateTime::fromSecsSinceEpoch(timestamp).toString(
                                           QStringLiteral("yyyy-MM-dd  HH:mm"))
                                     : QString();
        const QString title = conversation->title.trimmed().isEmpty()
                                  ? QStringLiteral("OpenZoom Assistant")
                                  : conversation->title.trimmed();
        const QString preview = conversation->preview.simplified().left(110);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n%2%3")
                .arg(title,
                     timeText,
                     preview.isEmpty() ? QString() : QStringLiteral("\n%1").arg(preview)),
            uiState_->assistantHistoryList_);
        item->setData(Qt::UserRole, conversation->threadId);
        item->setData(Qt::UserRole + 1, title);
        item->setToolTip(preview);
        if (conversation->threadId == currentAssistantThreadId_) {
            uiState_->assistantHistoryList_->setCurrentItem(item);
        }
    }
}

void OpenZoomApp::LoadSelectedAssistantConversation()
{
    if (!uiState_->assistantHistoryList_ || assistiveManager_->Runtime().IsCodexTurnActive()) {
        return;
    }
    QListWidgetItem* item = uiState_->assistantHistoryList_->currentItem();
    if (!item) {
        return;
    }
    const QString threadId = item->data(Qt::UserRole).toString();
    if (threadId.isEmpty()) {
        return;
    }
    currentAssistantThreadId_ = threadId;
    uiState_->assistantTranscript_->setPlainText(QStringLiteral("Loading conversation..."));
    assistiveManager_->Runtime().LoadAssistantConversation(threadId);
}

void OpenZoomApp::SetAssistantBusy(bool busy)
{
    assistiveManager_->Overlay().SetBusy(busy);
    if (uiState_->assistantSendButton_) {
        uiState_->assistantSendButton_->setEnabled(!busy && codexReady_ && codexSignedIn_);
    }
    if (uiState_->assistantStopButton_) {
        uiState_->assistantStopButton_->setEnabled(busy);
    }
    if (uiState_->assistantNewButton_) {
        uiState_->assistantNewButton_->setEnabled(!busy);
    }
    if (uiState_->assistantConnectButton_) {
        uiState_->assistantConnectButton_->setEnabled(!busy);
    }
    if (uiState_->assistantPromptEdit_) {
        uiState_->assistantPromptEdit_->setEnabled(!busy);
    }
    if (uiState_->assistantHistoryList_) {
        uiState_->assistantHistoryList_->setEnabled(!busy);
    }
    for (QPushButton* button : {uiState_->assistantRenameButton_, uiState_->assistantExportButton_, uiState_->assistantDeleteButton_}) {
        if (button) {
            button->setEnabled(!busy);
        }
    }
}

void OpenZoomApp::AppendAssistantMessage(const QString& speaker, const QString& text)
{
    if (!uiState_->assistantTranscript_ || text.trimmed().isEmpty()) {
        return;
    }
    QTextCursor cursor = uiState_->assistantTranscript_->textCursor();
    cursor.movePosition(QTextCursor::End);
    if (!uiState_->assistantTranscript_->document()->isEmpty()) {
        cursor.insertText(QStringLiteral("\n"));
    }
    cursor.insertText(QStringLiteral("%1\n%2\n").arg(speaker, text.trimmed()));
    uiState_->assistantTranscript_->setTextCursor(cursor);
    uiState_->assistantTranscript_->ensureCursorVisible();
}

void OpenZoomApp::OnOcrAssistToggled(bool checked)
{
    ocrAssistEnabled_ = checked;
    assistiveManager_->SetModes(
        ocrAssistEnabled_, vlmAssistEnabled_, assistiveOverlayEnabled_);
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::OnVlmAssistToggled(bool checked)
{
    vlmAssistEnabled_ = checked;
    assistiveManager_->SetModes(
        ocrAssistEnabled_, vlmAssistEnabled_, assistiveOverlayEnabled_);
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OpenZoomApp::OnAssistiveOverlayToggled(bool checked)
{
    assistiveOverlayEnabled_ = checked;
    assistiveManager_->SetModes(
        ocrAssistEnabled_, vlmAssistEnabled_, assistiveOverlayEnabled_);
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}


} // namespace openzoom

#endif // _WIN32
