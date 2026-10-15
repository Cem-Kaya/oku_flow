#ifdef _WIN32

#include "app_internal.hpp"
#include "okuflow/ui/agent_control_panel.hpp"
#include "okuflow/common/codex_agent_client.hpp"
#include "okuflow/common/visible_chat_session_store.hpp"
#include <QScrollBar>
#include <QSignalBlocker>
#include <array>
#include <QUuid>
#include "okuflow/ui/assistant_chat_view.hpp"

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
        CancelPendingAssistantCaptures();
        CancelAssistantHistoryLoad();
        settingsController_->MutableSettings().assistive = dialog.result();
        if (visibleChatStore_) {
            visibleChatStore_->SetSecretFilters({dialog.result().vlmApiKey.trimmed(),qEnvironmentVariable("OKUFLOW_VLM_API_KEY").trimmed()});
            PopulateAssistantHistory();
        }
        RefreshAgentControlSettings();
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
    assistiveManager_->Runtime().CancelPeriodicForUserTurn();
    if (pendingOnDemandAnalysis_ || pendingAssistantFramePrompt_ ||
        assistiveManager_->Runtime().ChatSession().Busy() || assistiveManager_->Runtime().IsBusy()) {
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
    if (!readText) {
        CancelAssistantHistoryLoad();
        const bool frameAvailable = (usingCudaLastFrame_ && cudaSharedTexture_ && presenter_ &&
            processedFrameWidth_ > 0 && processedFrameHeight_ > 0) ||
            (!presentationBuffer_.empty() && presentationWidth_ > 0 && presentationHeight_ > 0);
        if (frameAvailable && assistiveManager_->Runtime().ChatSession().Snapshot().value("kind").toString() != "explain")
            StartNewAssistantConversationAs(ChatBackendKind::Explain);
    }

    // Prefer the processed GPU output. Queue the request with the next
    // viewport presentation so the existing CUDA/D3D fence contract orders
    // the copy without blocking the UI thread.
    if (usingCudaLastFrame_ && cudaSharedTexture_ && presenter_ &&
        processedFrameWidth_ > 0 && processedFrameHeight_ > 0) {
        if (!readText && !assistiveManager_->Runtime().PrepareChatTurn(
            QCoreApplication::translate("OkuFlow","Explain"),ChatBackendKind::Explain,ChatRequestOrigin::ExplainButton)) return;
        if (!readText) assistiveManager_->Overlay().ShowChat(true);
        if (readText) assistiveManager_->Runtime().ReserveForegroundCapture();
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
        if (!readText) {
            if (!assistiveManager_->Runtime().PrepareChatTurn(QCoreApplication::translate("OkuFlow","Explain"),
                ChatBackendKind::Explain,ChatRequestOrigin::ExplainButton)) return;
            assistiveManager_->Overlay().ShowChat(true);
        }
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
    const QString prompt = assistiveManager_->Runtime().ChatSession().Draft().trimmed();
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
    const auto& settings = settingsController_->MutableSettings().assistive;
    const bool subscriptionProvider = settings.aiProvider.trimmed().compare(QStringLiteral("codex"),Qt::CaseInsensitive) == 0;
    StartNewAssistantConversationAs(VisibleChatSession::NewConversationKind(subscriptionProvider,
        settings.agentControlEnabled,settings.codexCodingEnabled));
}

void OkuFlowApp::StartNewAssistantConversationAs(ChatBackendKind kind)
{
    PersistVisibleChat();
    CancelPendingAssistantCaptures();
    CancelAgentTurn();
    if (controlService_) controlService_->RetireOwner(agentOwner_);
    agentOwner_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    CancelAssistantHistoryLoad();
    auto& runtime = assistiveManager_->Runtime();
    runtime.ResetVisibleChat(kind);
    currentAssistantThreadId_.clear(); agentConversationId_.clear();
    agentConversationSelected_ = false; sendWithoutControlOnce_ = false;
    if (agentCodex_) agentCodex_->ResetConversation();
    pendingAssistantPrompt_.clear(); assistantResponseOpen_ = false;
    assistantResponseReceivedText_ = false;
    for (auto* list : {uiState_->assistantHistoryList_,assistiveManager_->Overlay().chatView()->historyList()})
        if (list) {QSignalBlocker block(list);list->setCurrentRow(-1);}
}

void OkuFlowApp::SubmitAssistantPromptText(const QString& prompt,
                                            bool clearAdvancedEditor,
                                            bool forceAttachFrame)
{
    auto& runtime = assistiveManager_->Runtime();
    runtime.CancelPeriodicForUserTurn();
    if (prompt.trimmed().isEmpty() || pendingOnDemandAnalysis_ || runtime.IsBusy() ||
        runtime.ChatSession().Busy() || pendingAssistantFramePrompt_) return;
    CancelAssistantHistoryLoad();
    runtime.ChatSession().SetDraft(prompt);
    const auto kind = runtime.ChatSession().Snapshot().value("kind").toString();
    const bool attachFrame = runtime.ChatSession().AttachCurrentView();
    const bool embedded = settingsController_->MutableSettings().assistive.agentControlEnabled && !sendWithoutControlOnce_;
    const bool legacy = kind == "assistant" || kind == "coding";
    if (kind == "agent" || (legacy && embedded)) {
        mainWindow_->agentControlPanel()->SetRoutingNotice(true,AgentReady());
        assistiveManager_->Overlay().SetControlRoutingNotice(true,AgentReady());
        return;
    }
    if (embedded && !AgentReady()) {
        mainWindow_->agentControlPanel()->SetRoutingNotice(true,false);
        assistiveManager_->Overlay().SetControlRoutingNotice(true,false);
        return;
    }
    const auto backend = embedded ? ChatBackendKind::Agent : kind == "coding" ? ChatBackendKind::Coding :
        kind == "assistant" ? ChatBackendKind::Assistant : ChatBackendKind::Explain;
    if (!runtime.PrepareChatTurn(prompt,backend)) return;
    sendWithoutControlOnce_ = false;
    agentConversationSelected_ = embedded;
    if (embedded) {
        agentFrames_ = attachFrame; // explicit composer action, source-scoped by Begin
        DispatchAssistantPrompt(prompt,clearAdvancedEditor,nullptr,0,0,false);
        return;
    }
    if (attachFrame && usingCudaLastFrame_ && cudaSharedTexture_ && presenter_ &&
        processedFrameWidth_ > 0 && processedFrameHeight_ > 0) {
        pendingAssistantFramePrompt_ = PendingAssistantFramePrompt{prompt.trimmed(),clearAdvancedEditor};
        ArmAssistantCaptureDeadline(); SetAssistantBusy(true);
        pipelineOrchestrator_->MarkViewportDirty(); return;
    }
    const uint8_t* data = nullptr; int width = 0, height = 0;
    if (attachFrame && !presentationBuffer_.empty() && presentationWidth_ > 0 && presentationHeight_ > 0) {
        data = presentationBuffer_.data(); width = static_cast<int>(presentationWidth_); height = static_cast<int>(presentationHeight_);
    }
    DispatchAssistantPrompt(prompt,clearAdvancedEditor,data,width,height,attachFrame);
    (void)forceAttachFrame; // Floating and Advanced share the same explicit checkbox.
}

void OkuFlowApp::DispatchAssistantPrompt(const QString& prompt,
                                          bool clearAdvancedEditor,
                                          const uint8_t* bgraData,
                                          int width,
                                          int height,
                                          bool attachFrame)
{
    const QString submittedPrompt = prompt.trimmed();
    if (!assistiveManager_->Runtime().HasChatTurn() || !assistiveManager_->Runtime().ChatSession().Busy()) {
        SetAssistantBusy(assistiveManager_->Runtime().IsBusy()); return;
    }
    pendingAssistantPrompt_ = submittedPrompt;
    assistantResponseOpen_ = true; assistantResponseReceivedText_ = false;
    SetAssistantBusy(true);
    if (agentConversationSelected_) { SubmitAgentPrompt(submittedPrompt); return; }
    auto& runtime = assistiveManager_->Runtime();
    const auto kind = runtime.ChatSession().Snapshot().value("kind").toString();
    if (kind == "coding" || kind == "assistant")
        runtime.SubmitAssistantPrompt(submittedPrompt,currentAssistantThreadId_,bgraData,width,height,attachFrame,kind == "coding");
    else runtime.SubmitExplainFollowup(submittedPrompt,bgraData,width,height,attachFrame);
    (void)clearAdvancedEditor; // Admission clears the shared model draft once.
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
    const bool hadExplainFrame = pendingOnDemandAnalysis_ && !pendingOnDemandReadText_;
    pendingOnDemandAnalysis_ = false;
    pendingOnDemandReadText_ = false;
    pendingOnDemandReadbackId_ = 0;
    pendingOnDemandReadbackTimer_.invalidate();
    pendingAssistantFramePrompt_.reset();
    pendingAssistantFrameReadbackId_ = 0;
    pendingAssistantFrameReadbackTimer_.invalidate();
    if (assistiveManager_) assistiveManager_->Runtime().ReleaseForegroundCapture();
    if (!assistiveManager_) {
        return;
    }
    if (hadAssistantFrame || hadExplainFrame) assistiveManager_->Runtime().FailPreparedChat("canceled");
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
    if (!mainWindow_ || !assistiveManager_ || !uiState_->assistantHistoryList_) {
        return;
    }
    const std::array<QListWidget*,2> lists{uiState_->assistantHistoryList_,assistiveManager_->Overlay().chatView()->historyList()};
    std::array<QString,2> selected;
    std::array<int,2> scroll;
    for (int index=0;index<2;++index) {
        selected[index]=lists[index]->currentItem()?lists[index]->currentItem()->data(Qt::UserRole).toString():QString();
        scroll[index]=lists[index]->verticalScrollBar()->value();
    }
    QSignalBlocker first(lists[0]), second(lists[1]);
    for (auto* list : lists) list->clear();
    struct Row { QString id,title,preview; qint64 updatedAt{}; };
    std::vector<Row> conversations;
    for (const settings::CodexConversation& conversation : settingsController_->MutableSettings().codexConversations) {
        conversations.push_back({conversation.threadId,
            !conversation.title.trimmed().isEmpty() ? conversation.title.trimmed() : conversation.explain
                ? QCoreApplication::translate("OkuFlow","Explain conversation") : QStringLiteral("OkuFlow Assistant"),
            conversation.preview.simplified().left(110),
            (conversation.updatedAt>0?conversation.updatedAt:conversation.createdAt)*1000});
    }
    if (visibleChatStore_) for (const auto& entry : visibleChatStore_->List()) {
        QString preview=entry.draft.simplified().left(110);
        if (preview.isEmpty() && !entry.messages.isEmpty()) preview=entry.messages.last().toObject().value("text").toString().simplified().left(110);
        conversations.push_back({QStringLiteral("logical-chat-")+entry.id,
            entry.title.isEmpty()?QCoreApplication::translate("OkuFlow","Explain conversation"):entry.title,
            preview,entry.updatedAt});
    }
    std::sort(conversations.begin(), conversations.end(),
              [](const Row& lhs, const Row& rhs) {
                  return lhs.updatedAt > rhs.updatedAt;
              });
    for (const auto& conversation : conversations) {
        const qint64 timestamp = conversation.updatedAt;
        const QString timeText = timestamp > 0
                                     ? QDateTime::fromMSecsSinceEpoch(timestamp).toString(
                                           QStringLiteral("yyyy-MM-dd  HH:mm"))
                                     : QString();
        const QString title=conversation.title,preview=conversation.preview;
        for (int index=0;index<2;++index) {
        auto* list=lists[index];
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n%2%3")
                .arg(title,
                     timeText,
                     preview.isEmpty() ? QString() : QStringLiteral("\n%1").arg(preview)),
            list);
        item->setData(Qt::UserRole, conversation.id);
        item->setData(Qt::UserRole + 1, title);
        item->setToolTip(preview);
        const auto active=assistiveManager_->Runtime().ChatSession().IsLocalLogical()
            ? QStringLiteral("logical-chat-")+assistiveManager_->Runtime().ChatSession().ConversationId():currentAssistantThreadId_;
        if (conversation.id == (selected[index].isEmpty()?active:selected[index])) {
            list->setCurrentItem(item);
        }
        }
    }
    for (int index=0;index<2;++index) lists[index]->verticalScrollBar()->setValue(scroll[index]);
}

void OkuFlowApp::LoadSelectedAssistantConversation()
{
    if (!uiState_->assistantHistoryList_ || !uiState_->assistantHistoryList_->currentItem()) return;
    LoadAssistantConversationById(uiState_->assistantHistoryList_->currentItem()->data(Qt::UserRole).toString());
}
void OkuFlowApp::LoadAssistantConversationById(const QString& threadId)
{
    auto& runtime = assistiveManager_->Runtime();
    if (threadId.isEmpty() || runtime.IsBusy() || runtime.ChatSession().Busy() || pendingAssistantFramePrompt_) return;
    if (threadId.startsWith(QStringLiteral("logical-chat-"))) {
        PersistVisibleChat();
        const auto entry=visibleChatStore_?visibleChatStore_->Load(threadId.mid(13)):std::nullopt;
        if (!entry) {ShowStatusMessage(QStringLiteral("Could not load conversation."),7000);return;}
        CancelAssistantHistoryLoad();runtime.StopReadAloud();
        if (controlService_) controlService_->RetireOwner(agentOwner_);
        agentOwner_=QUuid::createUuid().toString(QUuid::WithoutBraces);
        agentConversationSelected_=false;agentConversationId_.clear();sendWithoutControlOnce_=false;
        if (agentCodex_) agentCodex_->ResetConversation();
        runtime.ResetVisibleChat();runtime.ChatSession().RestoreLogical(entry->id,entry->messages,entry->draft);
        currentAssistantThreadId_=threadId;
        mainWindow_->agentControlPanel()->SetRoutingNotice(false);assistiveManager_->Overlay().SetControlRoutingNotice(false);
        PopulateAssistantHistory();return;
    }
    const auto& conversations = settingsController_->MutableSettings().codexConversations;
    const auto entry = std::find_if(conversations.begin(),conversations.end(),[&](const auto& value) {return value.threadId == threadId;});
    if (entry == conversations.end()) return; // A native index selection, never model text.
    PersistVisibleChat();CancelAssistantHistoryLoad(); runtime.StopReadAloud();
    if (controlService_) controlService_->RetireOwner(agentOwner_);
    agentOwner_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto kind = settings::EffectiveConversationKind(*entry);
    if (kind == "agent") {
        currentAssistantThreadId_ = threadId;
        const auto messages = QJsonDocument::fromJson(agentTranscripts_.value(threadId).toUtf8()).array();
        runtime.ChatSession().Restore(threadId,ChatBackendKind::Agent,messages);
        agentConversationSelected_ = false;
        mainWindow_->agentControlPanel()->SetRoutingNotice(true,AgentReady());
        assistiveManager_->Overlay().SetControlRoutingNotice(true,AgentReady()); return;
    }
    pendingAssistantHistoryThreadId_ = threadId;
    runtime.LoadAssistantConversation(threadId);
}

void OkuFlowApp::PersistVisibleChat()
{
    if (!visibleChatStore_ || !assistiveManager_ || !assistiveManager_->Runtime().ChatSession().IsLocalLogical()) return;
    const auto& config=settingsController_->MutableSettings().assistive;
    visibleChatStore_->SetSecretFilters({config.vlmApiKey.trimmed(),qEnvironmentVariable("OKUFLOW_VLM_API_KEY").trimmed()});
    const auto revision=visibleChatStore_->Revision();
    if (!visibleChatStore_->Save(assistiveManager_->Runtime().ChatSession().Snapshot())) {
        if (visibleChatStore_->Revision()!=revision) PopulateAssistantHistory();
        ShowStatusMessage(QStringLiteral("Conversation history could not be updated."),7000);return;
    }
    if (visibleChatStore_->Revision()!=revision) PopulateAssistantHistory();
}
void OkuFlowApp::RenameStoredConversation(const QString& id,const QString& title)
{
    if (!visibleChatStore_ || !id.startsWith(QStringLiteral("logical-chat-"))) return;
    if (!visibleChatStore_->Rename(id.mid(13),title)) {ShowStatusMessage(QStringLiteral("Conversation history could not be updated."),7000);return;}
    PopulateAssistantHistory();
}
void OkuFlowApp::DeleteStoredConversation(const QString& id)
{
    if (!visibleChatStore_ || !id.startsWith(QStringLiteral("logical-chat-"))) return;
    const auto logicalId=id.mid(13);
    if (!visibleChatStore_->Delete(logicalId)) {ShowStatusMessage(QStringLiteral("Conversation history could not be updated."),7000);return;}
    if (assistiveManager_->Runtime().ChatSession().ConversationId()==logicalId) {
        // Reset after deletion without writing the just-deleted session again.
        CancelPendingAssistantCaptures();CancelAgentTurn();CancelAssistantHistoryLoad();
        if (controlService_) controlService_->RetireOwner(agentOwner_);
        agentOwner_=QUuid::createUuid().toString(QUuid::WithoutBraces);
        assistiveManager_->Runtime().ResetVisibleChat();currentAssistantThreadId_.clear();
        agentConversationId_.clear();agentConversationSelected_=false;
        if (agentCodex_) agentCodex_->ResetConversation();
    }
    PopulateAssistantHistory();
}

void OkuFlowApp::CancelAssistantHistoryLoad()
{
    pendingAssistantHistoryThreadId_.clear();
    assistiveManager_->Runtime().CancelAssistantConversationLoad();
}

void OkuFlowApp::SetAssistantBusy(bool busy)
{
    assistiveManager_->Overlay().SetBusy(busy);
    if (uiState_->assistantSendButton_) {
        uiState_->assistantSendButton_->setEnabled(!busy);
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
        uiState_->assistantPromptEdit_->setEnabled(true);
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
    QScrollBar* scroll = uiState_->assistantTranscript_->verticalScrollBar();
    const int scrollPosition = scroll->value();
    const bool followTail = !uiState_->assistantTranscript_->textCursor().hasSelection() &&
                            scrollPosition >= scroll->maximum() - 2;
    QTextCursor cursor(uiState_->assistantTranscript_->document());
    cursor.movePosition(QTextCursor::End);
    if (!uiState_->assistantTranscript_->document()->isEmpty()) {
        cursor.insertText(QStringLiteral("\n"));
    }
    cursor.insertText(QStringLiteral("%1\n%2\n").arg(speaker, text.trimmed()));
    scroll->setValue(followTail ? scroll->maximum() : scrollPosition);
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
