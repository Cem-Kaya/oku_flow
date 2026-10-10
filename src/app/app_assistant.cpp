#ifdef _WIN32

#include "app_internal.hpp"

namespace okuflow {

void OkuFlowApp::OpenAiSettingsDialog()
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

void OkuFlowApp::OpenNotesFile()
{
    if (assistiveManager_->Runtime().HasPendingNotesWrites()) {
        openNotesWhenStored_ = true;
        ShowStatusMessage(QStringLiteral("Saving lecture notes..."), 3000);
        return;
    }
    const QString path = assistiveManager_->Runtime().notesFilePath();
    if (path.isEmpty()) {
        ShowStatusMessage(
            QStringLiteral("No lecture notes yet — use Read, Explain, or Assistant to add text."));
        qInfo() << "Open notes skipped: no notes file written yet";
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void OkuFlowApp::SubmitOnDemandAnalysis(bool readText)
{
    if (pendingOnDemandAnalysis_ || pendingAssistantFramePrompt_ ||
        assistantResponseOpen_ || assistiveManager_->Runtime().IsBusy()) {
        ShowStatusMessage(QStringLiteral("Scene explanation is busy with a previous request. Try again in a moment."));
        return;
    }
    if (readText && autoTextClarityEnabled_ && cudaSurface_ &&
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
        pendingOnDemandAnalysis_ = true;
        pendingOnDemandReadText_ = readText;
        ArmAssistantCaptureDeadline();
        if (mainWindow_) {
            mainWindow_->setExplainBusy(true);
        }
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
                                             readText);
        return;
    }

    qWarning() << "On-demand analysis skipped: no frame available";
}

void OkuFlowApp::SubmitAssistantPrompt()
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

void OkuFlowApp::SubmitFloatingAssistantPrompt(const QString& prompt)
{
    if (prompt.trimmed().isEmpty()) {
        return;
    }
    SubmitAssistantPromptText(prompt.trimmed(), false, true);
}

void OkuFlowApp::StartNewAssistantConversation()
{
    if (assistiveManager_->Runtime().IsCodexTurnActive() ||
        pendingAssistantFramePrompt_) {
        ShowStatusMessage(
            QCoreApplication::translate(
                "OkuFlow",
                "Finish the current answer before starting a new chat."),
            5000);
        return;
    }
    // Dropping the thread id makes the next turn open a fresh Codex thread,
    // which the notes writer records as a new Conversation section.
    currentAssistantThreadId_.clear();
    pendingAssistantPrompt_.clear();
    assistantResponseOpen_ = false;
    assistantResponseReceivedText_ = false;
    if (uiState_->assistantTranscript_) {
        uiState_->assistantTranscript_->clear();
    }
    if (uiState_->assistantHistoryList_) {
        uiState_->assistantHistoryList_->clearSelection();
    }
    ShowStatusMessage(
        QCoreApplication::translate("OkuFlow", "Started a new conversation."),
        4000);
}

void OkuFlowApp::SubmitAssistantPromptText(const QString& prompt,
                                            bool clearAdvancedEditor,
                                            bool forceAttachFrame)
{
    if (prompt.trimmed().isEmpty() ||
        pendingOnDemandAnalysis_ || assistantResponseOpen_ ||
        assistiveManager_->Runtime().IsBusy() ||
        pendingAssistantFramePrompt_) {
        return;
    }
    const bool attachFrame = forceAttachFrame ||
                             (uiState_->assistantAttachFrameCheckbox_ && uiState_->assistantAttachFrameCheckbox_->isChecked());

    if (attachFrame && usingCudaLastFrame_ && cudaSharedTexture_ && presenter_ &&
        processedFrameWidth_ > 0 && processedFrameHeight_ > 0) {
        pendingAssistantFramePrompt_ = PendingAssistantFramePrompt{
            prompt.trimmed(), clearAdvancedEditor};
        ArmAssistantCaptureDeadline();
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

void OkuFlowApp::DispatchAssistantPrompt(const QString& prompt,
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
    cursor.insertText(QStringLiteral("OkuFlow Assistant\n"));
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

void OkuFlowApp::StopAssistantRequest()
{
    if (pendingOnDemandAnalysis_ || pendingAssistantFramePrompt_) {
        CancelPendingAssistantCaptures();
        ShowStatusMessage(QStringLiteral("Assistant request stopped."), 3000);
        return;
    }
    assistiveManager_->Runtime().StopAssistant();
}

void OkuFlowApp::CancelPendingAssistantCaptures()
{
    ++assistantCaptureGeneration_;
    const bool hadAssistantFrame = pendingAssistantFramePrompt_.has_value();
    pendingOnDemandAnalysis_ = false;
    pendingOnDemandReadText_ = false;
    pendingOnDemandReadbackId_ = 0;
    pendingOnDemandReadbackTimer_.invalidate();
    pendingAssistantFramePrompt_.reset();
    pendingAssistantFrameReadbackId_ = 0;
    pendingAssistantFrameReadbackTimer_.invalidate();
    if (!assistiveManager_) {
        return;
    }
    // Cancel queued camera captures without interrupting a dispatched chat.
    if (hadAssistantFrame && !assistiveManager_->Runtime().IsBusy()) {
        SetAssistantBusy(false);
    }
    if (mainWindow_) {
        mainWindow_->setExplainBusy(assistiveManager_->Runtime().IsBusy() &&
                                    !assistantResponseOpen_);
    }
}

void OkuFlowApp::ArmAssistantCaptureDeadline()
{
    const std::uint64_t generation = ++assistantCaptureGeneration_;
    QTimer::singleShot(5000, this, [this, generation]() {
        if (generation != assistantCaptureGeneration_ ||
            (!pendingOnDemandAnalysis_ && !pendingAssistantFramePrompt_)) {
            return;
        }
        CancelPendingAssistantCaptures();
        ShowStatusMessage(QStringLiteral("No camera frame is available to attach."), 5000);
    });
}

void OkuFlowApp::PopulateAssistantHistory()
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
                                  ? QStringLiteral("OkuFlow Assistant")
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

void OkuFlowApp::LoadSelectedAssistantConversation()
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

void OkuFlowApp::SetAssistantBusy(bool busy)
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

void OkuFlowApp::AppendAssistantMessage(const QString& speaker, const QString& text)
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

void OkuFlowApp::OnVlmAssistToggled(bool checked)
{
    vlmAssistEnabled_ = checked;
    assistiveManager_->SetModes(
        vlmAssistEnabled_, assistiveOverlayEnabled_);
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}

void OkuFlowApp::OnAssistiveOverlayToggled(bool checked)
{
    assistiveOverlayEnabled_ = checked;
    assistiveManager_->SetModes(
        vlmAssistEnabled_, assistiveOverlayEnabled_);
    UpdateProcessingStatusLabel();
    SyncCurrentConfigToPersistence();
}


} // namespace okuflow

#endif // _WIN32
