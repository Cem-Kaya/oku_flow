#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>

#include <QSet>
#include <QHash>
#include <QImage>
#include <functional>

#include <cstdint>
#include <memory>
#include "okuflow/common/visible_chat_session.hpp"


QT_BEGIN_NAMESPACE
class QImage;
class QNetworkAccessManager;
class QNetworkReply;
class QThreadPool;
class QTimer;
#if OKUFLOW_HAS_TTS
class QTextToSpeech;
#endif
QT_END_NAMESPACE

class NotesHtmlTests;
class AssistivePreparationTests;

namespace okuflow {

class CodexAppServerClient;
struct NotesWorkState;

// Runtime configuration for the assistive features. Each string field, when
// non-empty, takes precedence over the corresponding OKUFLOW_* environment
// variable; empty fields fall back to the environment variable.
struct AssistiveRuntimeConfig {
    QString aiProvider{QStringLiteral("codex")};
    QString codexExecutablePath;
    QString codexModel{QStringLiteral("gpt-6-luna")};
    QString codexReasoningEffort{QStringLiteral("low")};
    bool codexInternetEnabled{false};
    bool codexCodingEnabled{false};
    QString codexWorkspaceDirectory;
    QString assistantInstructions;
    QString vlmApiUrl;
    QString vlmApiKey;
    QString vlmModel;
    QString vlmPrompt;
    QString ttsEngine;
    QString ttsVoiceName;
    QString ttsVoiceLocale;
    double ttsRate{0.0};
    bool lectureNotesEnabled{true};
    QString notesDirectory;   // absolute dir for notes files; empty = notes disabled
};

class AssistiveRuntime : public QObject {
    Q_OBJECT
public:
    explicit AssistiveRuntime(QObject* parent = nullptr);
    ~AssistiveRuntime() override;

    void SetConfig(const AssistiveRuntimeConfig& config);
    // Changes the language used by subsequent AI requests and Read Aloud
    // voice selection. In-flight requests keep the language they started
    // with.
    void SetResponseLanguage(const QString& languageCode);
    static QString AppendResponseLanguageDirective(
        const QString& prompt,
        const QString& languageCode);
    void SetModes(bool vlmEnabled);
    // Profile/visibility changes own only periodic analysis. Explicit user
    // Read/Explain/Assistant preparations and replies keep their lifetime.
    void SetPeriodicAnalysisEnabled(bool enabled);
    bool WantsAnalysis() const;
    bool IsBusy() const;
    bool IsForegroundBusy() const { return IsBusy() && !visionRequestPeriodic_; }
    void CancelPeriodicForUserTurn();
    void ReserveForegroundCapture() { CancelPeriodicForUserTurn(); foregroundCaptureReserved_ = true; }
    void ReleaseForegroundCapture() { foregroundCaptureReserved_ = false; }
    bool IsCodexTurnActive() const;

    void SubmitFrame(const uint8_t* bgraData, int width, int height);
    // Reads text verbatim or explains the scene through the configured vision
    // provider, regardless of the periodic mode. Busy requests report status
    // without changing the active answer or its retained notes image.
    void SubmitFrameForced(const uint8_t* bgraData, int width, int height, bool readText = false);
    // Speaks result text only after an explicit user request.
    void ReadAloud(const QString& text);
    // Hides the current result without disabling future forced results.
    void DismissOverlay();
    void StartCodexLogin();
    void StopAssistant();
    VisibleChatSession& ChatSession() { return *chatSession_; }
    const VisibleChatSession& ChatSession() const { return *chatSession_; }
    bool HasChatTurn() const { return chatTicket_.has_value() || publishingChatEvent_; }
    bool PrepareChatTurn(const QString& text, ChatBackendKind backend,
                         ChatRequestOrigin origin = ChatRequestOrigin::Followup);
    void SubmitExplainFollowup(const QString& prompt, const uint8_t* data = nullptr,
                              int width = 0, int height = 0, bool attach = false);
    void ResetVisibleChat(ChatBackendKind kind = ChatBackendKind::Explain);
    void FailPreparedChat(const QString& error);
    void StopReadAloud() { StopSpeech(); }
    // Independent tool-free scene lane: app-issued guard is checked at actual
    // pixel submission and completion. No chat rows or automatic speech.
    bool SubmitScopedScene(const QByteArray& jpeg, std::function<bool()> guard,
                           std::function<void(bool)> completion);
    void CancelScopedScene();
    // Dedicated agent adapters share answer/notes presentation, not tool
    // authority or the ordinary Codex child. All calls are runtime-thread only.
    bool BeginControlAnswer(const QString& prompt, std::function<void()> stop);
    void ControlAnswerStarted(const QString& threadId, const QString& turnId);
    void ControlAnswerDelta(const QString& threadId, const QString& turnId, const QString& delta);
    void CompleteControlAnswer(const QString& threadId, const QString& turnId,
                              const QString& text, const QString& error, bool interrupted);
    void SubmitAssistantPrompt(const QString& prompt,
                               const QString& threadId,
                               const uint8_t* bgraData,
                               int width,
                               int height,
                               bool attachFrame, bool permitCoding = false);
    void LoadAssistantConversation(const QString& threadId);
    void CancelAssistantConversationLoad();
    void RenameAssistantConversation(const QString& threadId, const QString& name);
    void DeleteAssistantConversation(const QString& threadId);
    // Codex thread continued by every Explain (Read always opens a new
    // temporary thread). The owner restores the persisted id at startup;
    // ExplainConversationChanged reports every later change.
    void SetExplainConversationId(const QString& threadId);
    QString ExplainConversationId() const { return explainThreadId_; }
    // Makes the next Explain open a new conversation: forgets the Codex
    // thread id and the in-memory OpenAI-compatible Explain history.
    void StartNewExplainConversation();

    // Appends synchronized original and processed media to the HTML lecture
    // notes (no-op when notes are disabled).
    void NoteCapturedPhotoPair(const QString& originalPath,
                               const QString& processedPath);
    void NoteCapturedVideoPair(const QString& originalPath,
                               const QString& processedPath);
    // Appends an annotation snapshot section with the marked viewport image.
    void NoteAnnotationSnapshot(const QString& filePath,
                                const QString& heading);
    // Absolute path of the current lecture notes file; empty if nothing has
    // been scheduled yet; NotesWriteFinished confirms durable creation.
    QString notesFilePath() const;
    // Includes completions still queued for delivery on the runtime thread.
    bool HasPendingNotesWrites() const { return pendingNotesWrites_ != 0; }

signals:
    void OverlayUpdated(const QString& title, const QString& body, bool visible);
    void CodexServerStateChanged(bool ready, const QString& status);
    void CodexAccountChanged(bool signedIn, const QString& label, const QString& planType);
    void CodexModelsChanged(const QStringList& modelIds, const QString& selectedModel);
    void CodexModelCatalogChanged(const QJsonArray& models, const QString& selectedModel);
    void CodexRateLimitChanged(const QString& summary);
    void CodexLoginUrlReady(const QUrl& url);
    // Emitted only when the effective outbound-data state changes, so the UI
    // can show and announce it without flooding repeated periodic analyses.
    void PrivacyNotice(const QString& summary);
    void AssistantConversationCreated(const QJsonObject& thread);
    void AssistantTranscriptLoaded(const QString& threadId, const QJsonArray& messages);
    void AssistantTranscriptLoadFailed(const QString& threadId, const QString& error);
    void AssistantConversationRenamed(const QString& threadId, const QString& name);
    void AssistantConversationDeleted(const QString& threadId);
    // A new Codex Explain conversation was opened (first Explain, after a
    // reset, or replacing a thread that could not be resumed).
    void ExplainConversationCreated(const QJsonObject& thread);
    // The Explain thread id changed; empty means the next Explain starts fresh.
    void ExplainConversationChanged(const QString& threadId);
    void AssistantTurnStarted(const QString& threadId, const QString& turnId, bool persistent);
    void AssistantTextDelta(const QString& threadId, const QString& turnId, const QString& delta);
    void AssistantTurnFinished(const QString& threadId,
                               const QString& turnId,
                               const QString& text,
                               const QString& error,
                               bool interrupted,
                               bool persistent);
    void StatusNotice(const QString& sourceText);
    // One completion per accepted document/block write. Empty error = durable
    // success. Also emitted for queue rejection; paths identify captured targets.
    void NotesWriteFinished(const QString& path, const QString& error);

private:
    friend class ::NotesHtmlTests;
    friend class ::AssistivePreparationTests;
    void RefreshOverlay();
    // continueExplain marks a user-requested Explain that continues the
    // Explain conversation; periodic analyses and Read stay independent.
    void StartVlm(const uint8_t* bgraData, int width, int height,
                  bool readText = false, bool continueExplain = false,
                  bool periodic = false);
    void StartCodexVlm(const uint8_t* bgraData,
                       int width,
                       int height,
                       const QString& prompt,
                       const QString& threadId,
                       bool persistent,
                       bool retainThread = false);
    void PostVlmRequest(const QByteArray& requestBody,
                        const QString& apiUrl,
                        const QString& apiKey);
    void FinishVlmSuccess(const QString& text);
    void FinishVlmError(const QString& errorText);
    // Records a completed Advanced Assistant turn (question + analyzed image +
    // answer) as one lecture-note entry keyed to its conversation, and
    // updates the overlay text.
    void FinishAssistantTurnSuccess(const QString& threadId, const QString& text);
    void ClearPendingAssistantNote();
    bool CancelVlmPreparation();
    void CompleteAssistantPreparation(std::uint64_t generation, bool saved,
                                      const QString& imagePath, const QString& prompt,
                                      const QString& threadId, const QByteArray& preview = {});
    bool VlmConfigured() const;
    bool UsesCodexProvider() const;
    bool ValidateFrame(const uint8_t* bgraData, int width, int height);
    bool EnsureNotesFile();
    void FinalizeNotesFile();
    bool AppendNoteSection(const QString& heading,
                           const QString& bodyText,
                           const QString& imagePath = {},
                           bool aiText = false);
    // Reserves an image path and retains a bounded, implicitly shared frame.
    // Encoding happens only in the storage worker when a completed note uses it.
    QString SaveAnalyzedImageForNotes(const QImage& image);
    void AppendNoteMediaPair(const QString& heading,
                             const QString& originalPath,
                             const QString& processedPath,
                             bool video);
    // Typed, validated metadata for a notes section; every value is escaped
    // or charset-checked before it becomes an HTML attribute.
    struct NoteSectionMetadata {
        QString id;                    // [A-Za-z0-9_-]+ or dropped
        QString cssClass;              // [A-Za-z0-9 _-]+ or dropped
        QString dataRecordingSession;  // [A-Za-z0-9-]+ or dropped
        qint64 dataSequence{-1};       // >= 0 to emit
        bool approximateTime{false};
        QString languageOverride;      // section lang; empty = current
        // Long AI text starts collapsed to keep the page compact; the
        // summary then shows a one-line ellipsized preview.
        bool startCollapsed{false};
        QString previewText;           // plain text; escaped at emit
        // Type marker driving the collapsed-view icon: ai | video | photo |
        // draw | talk. Bare token or dropped.
        QString kind;
        // Advanced Assistant conversation id (sanitized thread id): turns
        // sharing it are grouped under one collapsible Conversation block.
        QString conversationId;
    };
    // Queues checked, flushed, rollback-on-short-write storage of one HTML
    // block and its retained images. All emitters share the same ordered queue.
    bool AppendNoteBlock(const QString& blockHtml, const QString& identity = {});
    bool QueueNotesWork(const QString& path, qint64 bytes,
                        std::function<bool()> work, const QString& identity = {});
    void RemoveNoteImage(const QString& path);
    bool AppendNoteHtmlSection(const QString& heading,
                               const QString& contentHtml,
                               const NoteSectionMetadata& metadata = {});
    void SpeakText(const QString& text);
    void StopSpeech();
    bool SelectVoiceForResponseLanguage(bool notifyMissing);
    void EmitPrivacyNoticeIfChanged(const QString& summary);
    void FinishChat(const QString& text, const QString& error = {}, bool interrupted = false);
    void PublishChatFinished(const QString& thread, const QString& turn, const QString& text,
                             const QString& error, bool interrupted, bool persistent);

    AssistiveRuntimeConfig config_;
    QString responseLanguageCode_{QStringLiteral("en")};
    QString warnedMissingVoiceLanguage_;

    bool vlmEnabled_{false};
    bool readingText_{false};
    bool vlmHardUnavailable_{false};
    bool vlmForcedVisible_{false};
    bool vlmPreparationPending_{false};
    bool vlmPreparationPersistent_{false};
    bool visionRequestPeriodic_{false};
    bool warnedDegenerateFrame_{false};
    bool overlayDismissed_{false};
    std::uint64_t vlmPreparationGeneration_{};

    QString vlmText_;
    QString vlmStatus_;

    QString notesFilePath_;
    bool notesDocumentOpen_{false};
    // The analyzed frame is retained at request time and encoded by the notes
    // worker when an answer arrives; cancellation drops the retained frame.
    QString pendingVlmNoteImagePath_;
    // Advanced Assistant exchange in flight: the question and the attached
    // frame (if any) are held so the completed answer is recorded as one
    // note with the question, the analyzed image, and the reply.
    QString pendingAssistantNotePrompt_;
    QString pendingAssistantNoteImagePath_;
    QHash<QString, QImage> noteImages_;
    std::shared_ptr<NotesWorkState> notesWork_;
    std::size_t pendingNotesWrites_{};

    std::unique_ptr<QThreadPool> imagePreparationPool_;
    QNetworkAccessManager* networkManager_{};
    QNetworkReply* activeReply_{};
    QNetworkReply* scopedSceneReply_{};
    std::unique_ptr<CodexAppServerClient> scopedSceneCodex_;
    quint64 scopedSceneGeneration_{};
    bool scopedSceneBusy_{};
    std::function<bool()> scopedSceneGuard_;
    std::function<void(bool)> scopedSceneCompletion_;
    void FinishScopedScene(quint64 generation,const QString& text);
    bool vlmResponseTooLarge_{false};
    QString vlmPreparationThreadId_;
    QString explainThreadId_;
    // OpenAI-compatible endpoints have no server-side thread. A bounded,
    // text-only, in-memory record of recent Explain answers gives later
    // Explains the same continuity. Cleared on provider/endpoint/model/key
    // change or explicit reset, including admission of an in-flight answer.
    QStringList explainHistory_;
    bool vlmRequestRecordsExplainHistory_{false};
    QString lastPrivacyNotice_;
    std::unique_ptr<CodexAppServerClient> codexClient_;
    std::unique_ptr<VisibleChatSession> chatSession_;
    std::optional<ChatTurnTicket> chatTicket_;
    ChatBackendKind chatBackend_{ChatBackendKind::Explain};
    ChatRequestOrigin chatOrigin_{ChatRequestOrigin::Followup};
    bool assistantPermitCoding_{};
    bool publishingChatEvent_{};
    bool foregroundCaptureReserved_{};
    bool controlAnswerPending_{};
    std::function<void()> controlAnswerStop_;
#if OKUFLOW_HAS_TTS
    QTextToSpeech* tts_{};
#endif
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
