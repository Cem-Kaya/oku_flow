#ifdef _WIN32

#include "okuflow/common/assistive_runtime.hpp"
#include "okuflow/common/notes_document_template.hpp"
#include "okuflow/common/notes_transcribe_bridge.hpp"
#include "okuflow/common/response_language.hpp"
#include "okuflow/common/codex_app_server_client.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QUuid>
#include <mutex>
#include <deque>

#include <algorithm>
#include <limits>

#include <windows.h>

#if OKUFLOW_HAS_TTS
#include <QTextToSpeech>
#include <QVoice>
#endif

namespace okuflow {

// A serial, byte-bounded queue. Worker closures own only immutable values;
// receiver access is guarded during dispatch and disconnected at destruction.
struct NotesWorkState {
    struct Job {
        QString path;
        QString identity;
        qint64 bytes;
        std::function<bool()> work;
    };
    std::mutex mutex;
    std::deque<Job> jobs;
    qint64 bytes{};
    size_t count{};
    bool running{};
    AssistiveRuntime* receiver{};
};


namespace {

constexpr int kMinFrameEdge = 64;
constexpr int kMaxVlmFrameEdge = 2048;
constexpr int kVlmTransferTimeoutMs = 30000;
constexpr qint64 kMaximumVlmResponseBytes = 2 * 1024 * 1024;
constexpr qsizetype kMaximumDisplayedAssistantCharacters = 256 * 1024;
// Continuity for OpenAI-compatible Explain requests: earlier answers only,
// never earlier images, so each request stays small and bounded.
constexpr qsizetype kMaximumExplainHistoryAnswers = 4;
constexpr qsizetype kMaximumExplainHistoryAnswerCharacters = 1200;

QString ExplainContinuationPrompt(const QString& prompt)
{
    return prompt + QStringLiteral(
        "\n\nThis is a new camera view in an ongoing Explain conversation. "
        "Describe this view; use earlier views only as context, for example "
        "to point out what changed.");
}

QString SanitizeText(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QStringLiteral("\r"), QStringLiteral("\n"));
    return text.trimmed();
}

QString TruncateText(const QString& text, int maxChars)
{
    if (text.size() <= maxChars) {
        return text;
    }
    return text.left(maxChars).trimmed() + QStringLiteral("...");
}

QString NotesMediaUrl(const QString& notesFilePath, const QString& mediaPath)
{
    const QFileInfo mediaInfo(mediaPath);
    const QDir notesDirectory(QFileInfo(notesFilePath).absolutePath());
    QString relativePath =
        notesDirectory.relativeFilePath(mediaInfo.absoluteFilePath());
    relativePath = QDir::fromNativeSeparators(relativePath);

    const QUrl url = QDir::isAbsolutePath(relativePath)
                         ? QUrl::fromLocalFile(mediaInfo.absoluteFilePath())
                         : QUrl(relativePath);
    return QString::fromUtf8(url.toEncoded(QUrl::FullyEncoded));
}

// Non-empty configured values take precedence over the environment variable.
QString ResolvedSetting(const QString& configured, const char* envName)
{
    const QString fromConfig = configured.trimmed();
    if (!fromConfig.isEmpty()) {
        return fromConfig;
    }
    return qEnvironmentVariable(envName).trimmed();
}

QString VlmNotConfiguredMessage()
{
    return QStringLiteral("VLM not configured. Set the server URL and model in AI Settings "
                          "or via OKUFLOW_VLM_API_URL and OKUFLOW_VLM_MODEL. An API key is optional for local servers.");
}

QString CodexNotAvailableMessage()
{
    return QStringLiteral("Codex is not ready. Open Advanced > Assistant to connect a ChatGPT account, "
                          "or choose an OpenAI-compatible provider in AI Settings.");
}

bool IsLocalEndpoint(const QUrl& url)
{
    const QString host = url.host().trimmed().toLower();
    return host == QStringLiteral("localhost") ||
           host == QStringLiteral("127.0.0.1") ||
           host == QStringLiteral("::1") ||
           host.endsWith(QStringLiteral(".localhost"));
}

QString CreateAssistiveTemporaryFramePath(const QString& purpose,
                                          const QString& suffix)
{
    QTemporaryFile tempFile(
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(
                QStringLiteral("okuflow_%1_%2_XXXXXX.%3")
                    .arg(purpose)
                    .arg(GetCurrentProcessId())
                    .arg(suffix)));
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        return {};
    }
    const QString path = tempFile.fileName();
    tempFile.close();
    return path;
}

bool ProcessIsRunning(quint32 processId)
{
    if (processId == 0) {
        return false;
    }
    const HANDLE process = OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(processId));
    if (!process) {
        // Access-denied and protected-process failures are not proof that the
        // process is gone. Only the OS "invalid parameter" result is.
        return GetLastError() != ERROR_INVALID_PARAMETER;
    }
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

void SweepAssistiveTemporaryFrames()
{
    QDir temporaryDirectory(
        QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    const QRegularExpression ownedFilePattern(
        QStringLiteral(R"(^okuflow_codex_(\d+)_.*\.(?:png|jpg)$)"));
    const QDateTime legacyCutoff =
        QDateTime::currentDateTimeUtc().addSecs(-60 * 60);
    int removed = 0;
    for (const QString& pattern :
         {QStringLiteral("okuflow_codex_*.jpg")}) {
        const QFileInfoList files =
            temporaryDirectory.entryInfoList({pattern}, QDir::Files);
        for (const QFileInfo& file : files) {
            const QRegularExpressionMatch match =
                ownedFilePattern.match(file.fileName());
            if (match.hasMatch()) {
                bool pidOk = false;
                const quint32 processId =
                    match.captured(1).toUInt(&pidOk);
                if (pidOk && ProcessIsRunning(processId)) {
                    continue;
                }
            } else if (file.lastModified().toUTC() > legacyCutoff) {
                // Older builds did not include a process id. Give any
                // concurrently running legacy request a conservative grace
                // period instead of deleting its live input.
                continue;
            }
            if (QFile::remove(file.absoluteFilePath())) {
                ++removed;
            }
        }
    }
    if (removed > 0) {
        qInfo() << "Removed" << removed
                << "temporary assistive camera frame(s) left by an earlier session.";
    }
}

void RemoveCurrentProcessAssistiveFrames()
{
    QDir temporaryDirectory(
        QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    const QString processId = QString::number(GetCurrentProcessId());
    for (const QString& pattern :
         {QStringLiteral("okuflow_codex_%1_*.jpg").arg(processId)}) {
        for (const QString& fileName :
             temporaryDirectory.entryList({pattern}, QDir::Files)) {
            QFile::remove(temporaryDirectory.filePath(fileName));
        }
    }
}

QString ParseVlmResponseText(const QByteArray& payload)
{
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }

    const QJsonObject root = doc.object();

    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (!choices.isEmpty()) {
        const QJsonObject firstChoice = choices.first().toObject();
        const QJsonObject message = firstChoice.value(QStringLiteral("message")).toObject();
        const QJsonValue content = message.value(QStringLiteral("content"));
        if (content.isString()) {
            return SanitizeText(content.toString());
        }
        if (content.isArray()) {
            QStringList parts;
            for (const QJsonValue& itemValue : content.toArray()) {
                const QJsonObject item = itemValue.toObject();
                if (item.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                    parts.push_back(item.value(QStringLiteral("text")).toString());
                }
            }
            return SanitizeText(parts.join(QStringLiteral("\n")));
        }
    }

    const QJsonArray output = root.value(QStringLiteral("output")).toArray();
    QStringList responseParts;
    for (const QJsonValue& blockValue : output) {
        const QJsonObject block = blockValue.toObject();
        const QJsonArray content = block.value(QStringLiteral("content")).toArray();
        for (const QJsonValue& itemValue : content) {
            const QJsonObject item = itemValue.toObject();
            if (item.value(QStringLiteral("type")).toString() == QStringLiteral("output_text")) {
                responseParts.push_back(item.value(QStringLiteral("text")).toString());
            }
        }
    }
    return SanitizeText(responseParts.join(QStringLiteral("\n")));
}

QByteArray BuildVlmRequestBody(const QByteArray& jpegBytes,
                               const QString& model,
                               const QString& prompt,
                               const QString& assistantInstructions,
                               const QString& responseLanguageCode,
                               int maximumTokens,
                               const QStringList& explainHistory = {})
{
    const QString dataUri =
        QStringLiteral("data:image/jpeg;base64,%1")
            .arg(QString::fromLatin1(jpegBytes.toBase64()));

    const QJsonObject textPart{
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"), prompt}};
    const QJsonObject imagePart{
        {QStringLiteral("type"), QStringLiteral("image_url")},
        {QStringLiteral("image_url"),
         QJsonObject{{QStringLiteral("url"), dataUri}}}};
    const QJsonObject message{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), QJsonArray{textPart, imagePart}}};

    QJsonArray messages;
    const QString languageDirective =
        AssistiveRuntime::AppendResponseLanguageDirective(
            QString(), responseLanguageCode);
    if (!languageDirective.isEmpty()) {
        messages.append(
            QJsonObject{
                {QStringLiteral("role"), QStringLiteral("system")},
                {QStringLiteral("content"), languageDirective}});
    }
    if (!assistantInstructions.isEmpty()) {
        messages.append(
            QJsonObject{
                {QStringLiteral("role"), QStringLiteral("system")},
                {QStringLiteral("content"), assistantInstructions}});
    }
    for (const QString& earlierAnswer : explainHistory) {
        messages.append(
            QJsonObject{
                {QStringLiteral("role"), QStringLiteral("user")},
                {QStringLiteral("content"),
                 QStringLiteral("Earlier Explain request (its camera image is not resent).")}});
        messages.append(
            QJsonObject{
                {QStringLiteral("role"), QStringLiteral("assistant")},
                {QStringLiteral("content"), earlierAnswer}});
    }
    messages.append(message);

    const QJsonObject requestBody{
        {QStringLiteral("model"), model},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("max_tokens"), maximumTokens}};
    return QJsonDocument(requestBody).toJson(QJsonDocument::Compact);
}

} // namespace

AssistiveRuntime::AssistiveRuntime(QObject* parent)
    : QObject(parent)
{
    SweepAssistiveTemporaryFrames();
    notesWork_ = std::make_shared<NotesWorkState>();
    notesWork_->receiver = this;
    imagePreparationPool_ = std::make_unique<QThreadPool>();
    imagePreparationPool_->setMaxThreadCount(2);
    imagePreparationPool_->setExpiryTimeout(30000);
    networkManager_ = new QNetworkAccessManager(this);
    codexClient_ = std::make_unique<CodexAppServerClient>(this);

    connect(codexClient_.get(), &CodexAppServerClient::ServerStateChanged,
            this, &AssistiveRuntime::CodexServerStateChanged);
    connect(codexClient_.get(), &CodexAppServerClient::AccountChanged,
            this, &AssistiveRuntime::CodexAccountChanged);
    connect(codexClient_.get(), &CodexAppServerClient::ModelsChanged,
            this, &AssistiveRuntime::CodexModelsChanged);
    connect(codexClient_.get(), &CodexAppServerClient::ModelCatalogChanged,
            this, &AssistiveRuntime::CodexModelCatalogChanged);
    connect(codexClient_.get(), &CodexAppServerClient::RateLimitChanged,
            this, &AssistiveRuntime::CodexRateLimitChanged);
    connect(codexClient_.get(), &CodexAppServerClient::LoginUrlReady,
            this, &AssistiveRuntime::CodexLoginUrlReady);
    connect(codexClient_.get(), &CodexAppServerClient::ConversationCreated,
            this, &AssistiveRuntime::AssistantConversationCreated);
    connect(codexClient_.get(), &CodexAppServerClient::ConversationTranscriptLoaded,
            this, &AssistiveRuntime::AssistantTranscriptLoaded);
    connect(codexClient_.get(), &CodexAppServerClient::ConversationTranscriptLoadFailed,
            this, &AssistiveRuntime::AssistantTranscriptLoadFailed);
    connect(codexClient_.get(), &CodexAppServerClient::ConversationRenamed,
            this, &AssistiveRuntime::AssistantConversationRenamed);
    connect(codexClient_.get(), &CodexAppServerClient::ConversationDeleted,
            this, [this](const QString& threadId) {
                // Deleting the Explain conversation makes the next Explain
                // start a new one.
                if (!threadId.isEmpty() && threadId == explainThreadId_) {
                    explainThreadId_.clear();
                    emit ExplainConversationChanged({});
                }
                emit AssistantConversationDeleted(threadId);
            });
    connect(codexClient_.get(), &CodexAppServerClient::RetainedThreadStarted,
            this, [this](const QJsonObject& thread) {
                const QString threadId =
                    thread.value(QStringLiteral("id")).toString();
                if (threadId.isEmpty()) {
                    return;
                }
                explainThreadId_ = threadId;
                emit ExplainConversationCreated(thread);
                emit ExplainConversationChanged(threadId);
            });
    connect(codexClient_.get(), &CodexAppServerClient::TurnStarted,
            this, &AssistiveRuntime::AssistantTurnStarted);
    connect(codexClient_.get(), &CodexAppServerClient::TurnTextDelta,
            this,
            [this](const QString& threadId, const QString& turnId, const QString& delta) {
                const qsizetype available =
                    std::max<qsizetype>(
                        0,
                        kMaximumDisplayedAssistantCharacters - vlmText_.size());
                vlmText_ += delta.left(available);
                vlmStatus_.clear();
                RefreshOverlay();
                emit AssistantTextDelta(threadId, turnId, delta);
            });
    connect(codexClient_.get(), &CodexAppServerClient::TurnFinished,
            this,
            [this](const QString& threadId,
                   const QString& turnId,
                   const QString& text,
                   const QString& error,
                   bool interrupted,
                   bool persistent) {
                if (interrupted) {
                    if (persistent) {
                        ClearPendingAssistantNote();
                    }
                    vlmText_.clear();
                    vlmStatus_ = error.trimmed().isEmpty()
                                     ? QStringLiteral("Assistant stopped.")
                                     : SanitizeText(error);
                    RefreshOverlay();
                } else if (!error.isEmpty()) {
                    if (persistent) {
                        ClearPendingAssistantNote();
                    }
                    FinishVlmError(error);
                } else if (persistent) {
                    // The Advanced Assistant records the full exchange —
                    // question, analyzed frame, and answer — keyed to its
                    // conversation so multiple turns group together.
                    FinishAssistantTurnSuccess(threadId, text);
                } else {
                    FinishVlmSuccess(text);
                }
                emit AssistantTurnFinished(threadId, turnId, text, error, interrupted, persistent);
            });

    RefreshOverlay();
}

AssistiveRuntime::~AssistiveRuntime()
{
    ++vlmPreparationGeneration_;
    vlmPreparationPending_ = false;
    if (imagePreparationPool_) {
        imagePreparationPool_->clear();
        imagePreparationPool_->waitForDone();
    }
    if (activeReply_) {
        activeReply_->abort();
    }
    if (!pendingVlmNoteImagePath_.isEmpty()) {
        RemoveNoteImage(pendingVlmNoteImagePath_);
    }
    if (!pendingAssistantNoteImagePath_.isEmpty()) {
        RemoveNoteImage(pendingAssistantNoteImagePath_);
    }
    RemoveCurrentProcessAssistiveFrames();
    FinalizeNotesFile();
    // Accepted writes finish on the global pool without retaining this QObject.
    // Qt waits for that pool at application shutdown; runtime teardown does not
    // wait on a slow notes destination.
    std::lock_guard lock(notesWork_->mutex);
    notesWork_->receiver = nullptr;
}

void AssistiveRuntime::SetConfig(const AssistiveRuntimeConfig& config)
{
    const bool notesTargetChanged = config.notesDirectory.trimmed() != config_.notesDirectory.trimmed();
    const bool speechConfigChanged =
        config.ttsEngine != config_.ttsEngine ||
        config.ttsVoiceName != config_.ttsVoiceName ||
        config.ttsVoiceLocale != config_.ttsVoiceLocale ||
        config.ttsRate != config_.ttsRate;
    // Earlier answers are only resent to the endpoint, model and credential
    // that wrote them. Retire the active request's history admission too: its
    // eventual answer still belongs to the old configuration.
    if (config.aiProvider != config_.aiProvider ||
        config.vlmApiUrl != config_.vlmApiUrl ||
        config.vlmModel != config_.vlmModel ||
        config.vlmApiKey != config_.vlmApiKey) {
        explainHistory_.clear();
        vlmRequestRecordsExplainHistory_ = false;
    }
    if (speechConfigChanged) {
        StopSpeech();
#if OKUFLOW_HAS_TTS
        delete tts_;
        tts_ = nullptr;
#endif
    }
    config_ = config;
    if (codexClient_) {
        codexClient_->Configure(config_.codexExecutablePath,
                                config_.codexModel,
                                config_.codexReasoningEffort,
                                config_.assistantInstructions,
                                config_.codexInternetEnabled,
                                config_.codexCodingEnabled,
                                config_.codexWorkspaceDirectory);
        if (UsesCodexProvider()) {
            codexClient_->Start();
        }
    }

    // New credentials may fix a previous hard failure.
    vlmHardUnavailable_ = false;

    if (notesTargetChanged) {
        FinalizeNotesFile();
        notesFilePath_.clear();
    }

    if (vlmEnabled_ && vlmText_.isEmpty()) {
        vlmStatus_ = VlmConfigured() ? QStringLiteral("VLM ready.") : VlmNotConfiguredMessage();
        RefreshOverlay();
    }
}

void AssistiveRuntime::SetResponseLanguage(const QString& languageCode)
{
    const QString normalized = languageCode.trimmed().toLower();
    const QString next =
        normalized == QStringLiteral("tr") ||
                normalized == QStringLiteral("de")
            ? normalized
            : QStringLiteral("en");
    if (responseLanguageCode_ == next) {
        return;
    }
    responseLanguageCode_ = next;
    warnedMissingVoiceLanguage_.clear();
#if OKUFLOW_HAS_TTS
    if (tts_) {
        SelectVoiceForResponseLanguage(true);
    }
#endif
}

QString AssistiveRuntime::AppendResponseLanguageDirective(
    const QString& prompt,
    const QString& languageCode)
{
    return okuflow::AppendResponseLanguageDirective(prompt, languageCode);
}

void AssistiveRuntime::SetModes(bool vlmEnabled)
{
    const bool vlmTurningOff = vlmEnabled_ && !vlmEnabled;

    if (vlmTurningOff && activeReply_) {
        activeReply_->abort();
    }
    if (vlmTurningOff) {
        CancelVlmPreparation();
    }

    vlmEnabled_ = vlmEnabled;
    if (vlmEnabled_) {
        vlmHardUnavailable_ = false;
    }
    if (vlmTurningOff) {
        vlmForcedVisible_ = false;
        vlmText_.clear();
        vlmStatus_.clear();
    } else if (vlmEnabled_ && vlmText_.isEmpty() && vlmStatus_.isEmpty()) {
        if (VlmConfigured()) {
            vlmStatus_ = QStringLiteral("VLM ready.");
        } else {
            vlmStatus_ = VlmNotConfiguredMessage();
        }
    }

    RefreshOverlay();
}

void AssistiveRuntime::ReadAloud(const QString& text)
{
    SpeakText(text);
}

void AssistiveRuntime::DismissOverlay()
{
    overlayDismissed_ = true;
    RefreshOverlay();
}

bool AssistiveRuntime::WantsAnalysis() const
{
    // Subscription-backed Codex explanations are user initiated. This avoids
    // spending a user's Codex allowance every 1.6 seconds in an assistive mode.
    const bool automaticVlm = !UsesCodexProvider() && vlmEnabled_ && !vlmHardUnavailable_;
    return automaticVlm;
}

bool AssistiveRuntime::IsBusy() const
{
    return vlmPreparationPending_ || activeReply_ != nullptr ||
           (codexClient_ && codexClient_->IsTurnActive());
}

bool AssistiveRuntime::IsCodexTurnActive() const
{
    return (vlmPreparationPending_ && vlmPreparationPersistent_) ||
           (codexClient_ && codexClient_->IsTurnActive());
}

void AssistiveRuntime::SubmitFrame(const uint8_t* bgraData, int width, int height)
{
    if (!WantsAnalysis() || !ValidateFrame(bgraData, width, height)) {
        return;
    }

    if (vlmEnabled_ && !UsesCodexProvider() &&
        !vlmPreparationPending_ && activeReply_ == nullptr) {
        StartVlm(bgraData, width, height);
    }
}

void AssistiveRuntime::SubmitFrameForced(const uint8_t* bgraData, int width, int height, bool readText)
{
    if (!ValidateFrame(bgraData, width, height)) {
        return;
    }
    // Do not overwrite the answer or note-image ownership of an active turn.
    if (IsBusy()) {
        emit StatusNotice(QStringLiteral("Scene explanation is busy with a previous request. Try again in a moment."));
        return;
    }
    overlayDismissed_ = false;
    vlmForcedVisible_ = true;
    if (!VlmConfigured()) {
        FinishVlmError(VlmNotConfiguredMessage());
        return;
    }
    vlmHardUnavailable_ = false;
    StartVlm(bgraData, width, height, readText, !readText);
}

void AssistiveRuntime::NoteCapturedPhotoPair(const QString& originalPath,
                                             const QString& processedPath)
{
    AppendNoteMediaPair(QCoreApplication::translate("OkuFlow", "Photo captured"),
                        originalPath,
                        processedPath,
                        false);
}

void AssistiveRuntime::NoteCapturedVideoPair(const QString& originalPath,
                                             const QString& processedPath)
{
    AppendNoteMediaPair(QCoreApplication::translate("OkuFlow", "Video recorded"),
                        originalPath,
                        processedPath,
                        true);
}

namespace {

bool IsSafeAttributeToken(const QString& value, const QString& extraCharacters)
{
    if (value.isEmpty()) {
        return false;
    }
    for (const QChar character : value) {
        if (!character.isLetterOrNumber() &&
            !extraCharacters.contains(character)) {
            return false;
        }
    }
    return true;
}

} // namespace

void AssistiveRuntime::NoteAnnotationSnapshot(const QString& filePath,
                                              const QString& heading)
{
    if (filePath.trimmed().isEmpty()) {
        return;
    }
    AppendNoteSection(
        heading.trimmed().isEmpty()
            ? QCoreApplication::translate("OkuFlow", "Annotated view")
            : heading,
        {},
        filePath);
}

void AssistiveRuntime::StartCodexLogin()
{
    if (codexClient_) {
        codexClient_->Start();
        codexClient_->StartChatGptLogin();
    }
}

bool AssistiveRuntime::CancelVlmPreparation()
{
    if (!vlmPreparationPending_) {
        return false;
    }
    const QString threadId = vlmPreparationThreadId_;
    const bool persistent = vlmPreparationPersistent_;
    // Retire ownership before publishing the terminal event: a receiver may
    // immediately submit the next request from its completion handler.
    ++vlmPreparationGeneration_;
    vlmPreparationPending_ = false;
    vlmPreparationPersistent_ = false;
    vlmPreparationThreadId_.clear();
    vlmRequestRecordsExplainHistory_ = false;
    if (persistent) {
        ClearPendingAssistantNote();
    } else if (!pendingVlmNoteImagePath_.isEmpty()) {
        RemoveNoteImage(pendingVlmNoteImagePath_);
        pendingVlmNoteImagePath_.clear();
    }
    vlmText_.clear();
    vlmStatus_ = QStringLiteral("Assistant stopped.");
    RefreshOverlay();
    if (persistent) {
        emit AssistantTurnFinished(threadId, {}, {},
                                   QStringLiteral("Assistant stopped."), true, true);
    }
    return true;
}

void AssistiveRuntime::StopAssistant()
{
    if (CancelVlmPreparation()) {
        return;
    }
    if (activeReply_) {
        activeReply_->abort();
        return;
    }
    if (codexClient_) {
        codexClient_->InterruptTurn();
    }
}

void AssistiveRuntime::SubmitAssistantPrompt(const QString& prompt,
                                             const QString& threadId,
                                             const uint8_t* bgraData,
                                             int width,
                                             int height,
                                             bool attachFrame)
{
    const QString responsePrompt =
        AppendResponseLanguageDirective(prompt, responseLanguageCode_);
    if (!UsesCodexProvider()) {
        emit AssistantTurnFinished(threadId, {}, {},
                                   QStringLiteral("Persistent Assistant conversations require the Codex subscription provider."),
                                   false, true);
        return;
    }
    if (!codexClient_ || IsCodexTurnActive()) {
        emit AssistantTurnFinished(threadId, {}, {},
                                   QStringLiteral("Another assistant request is already running."),
                                   false, true);
        return;
    }

    if (attachFrame && !ValidateFrame(bgraData, width, height)) {
        emit AssistantTurnFinished(threadId, {}, {},
                                   QStringLiteral("No camera frame is available to attach."),
                                   false, true);
        return;
    }
    EmitPrivacyNoticeIfChanged(
        QStringLiteral("%1 to Codex. Conversation: persistent. "
                       "Tool internet: %2. Coding and file changes: %3.")
            .arg(attachFrame
                     ? QStringLiteral("Sending the current camera frame and prompt")
                     : QStringLiteral("Sending the prompt only"),
                 config_.codexInternetEnabled
                     ? QStringLiteral("enabled")
                     : QStringLiteral("blocked"),
                 config_.codexCodingEnabled
                     ? QStringLiteral("enabled")
                     : QStringLiteral("blocked")));
    overlayDismissed_ = false;
    vlmForcedVisible_ = true;
    readingText_ = false;
    vlmText_.clear();
    vlmStatus_ = attachFrame
                     ? QStringLiteral("Preparing the current view...")
                     : QStringLiteral("Thinking...");
    RefreshOverlay();

    CancelAssistantConversationLoad();
    // Record the raw question (not the language-directive-appended form) so
    // the completed answer can be noted as a full exchange.
    ClearPendingAssistantNote();
    pendingAssistantNotePrompt_ = prompt;

    if (!attachFrame) {
        codexClient_->RequestVisionTurn(responsePrompt, {}, threadId, true);
        return;
    }

    QImage frameImage(bgraData,
                      width,
                      height,
                      width * 4,
                      QImage::Format_ARGB32);
    QImage copy = frameImage.copy();
    // Retain a shared notes frame before moving the request copy into its
    // encode worker; notes JPEG encoding waits for a completed answer.
    pendingAssistantNoteImagePath_ = SaveAnalyzedImageForNotes(copy);
    const QString imagePath =
        CreateAssistiveTemporaryFramePath(QStringLiteral("codex"),
                                          QStringLiteral("jpg"));
    if (copy.isNull() || imagePath.isEmpty()) {
        QFile::remove(imagePath);
        ClearPendingAssistantNote();
        FinishVlmError(
            QStringLiteral("Could not prepare the current camera view."));
        emit AssistantTurnFinished(
            threadId,
            {},
            {},
            QStringLiteral("Could not prepare the current camera view."),
            false,
            true);
        return;
    }

    vlmPreparationPending_ = true;
    vlmPreparationPersistent_ = true;
    vlmPreparationThreadId_ = threadId;
    const std::uint64_t generation = ++vlmPreparationGeneration_;
    QPointer<AssistiveRuntime> owner(this);
    const bool queued = imagePreparationPool_ &&
        imagePreparationPool_->tryStart(
        [owner,
         copy = std::move(copy),
         imagePath,
         prompt = responsePrompt,
         threadId,
         generation]() mutable {
            if (copy.width() > kMaxVlmFrameEdge ||
                copy.height() > kMaxVlmFrameEdge) {
                copy = copy.scaled(kMaxVlmFrameEdge,
                                   kMaxVlmFrameEdge,
                                   Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
            }
            const bool saved = copy.save(imagePath, "JPG", 85);
            if (!saved) {
                QFile::remove(imagePath);
            }
            if (!owner) {
                if (saved) {
                    QFile::remove(imagePath);
                }
                return;
            }
            QMetaObject::invokeMethod(
                owner,
                [owner,
                 saved,
                 imagePath,
                 prompt,
                 threadId,
                 generation]() {
                    if (!owner) {
                        QFile::remove(imagePath);
                        return;
                    }
                    owner->CompleteAssistantPreparation(
                        generation, saved, imagePath, prompt, threadId);
                },
                Qt::QueuedConnection);
        });
    if (!queued) {
        vlmPreparationPending_ = false;
        vlmPreparationPersistent_ = false;
        vlmPreparationThreadId_.clear();
        QFile::remove(imagePath);
        ClearPendingAssistantNote();
        const QString error =
            QStringLiteral(
                "Could not prepare the current camera view because the image "
                "worker is busy.");
        FinishVlmError(error);
        emit AssistantTurnFinished(
            threadId, {}, {}, error, false, true);
    }
}

void AssistiveRuntime::CompleteAssistantPreparation(
    std::uint64_t generation, bool saved, const QString& imagePath,
    const QString& prompt, const QString& threadId)
{
    if (generation != vlmPreparationGeneration_ || !vlmPreparationPending_) {
        // Cancellation already released this request's notes payload. A late
        // worker owns only its temporary file, never a newer request's notes.
        QFile::remove(imagePath);
        return;
    }
    vlmPreparationPending_ = false;
    vlmPreparationPersistent_ = false;
    vlmPreparationThreadId_.clear();
    if (!saved) {
        const QString error = QStringLiteral("Could not prepare the current camera view.");
        ClearPendingAssistantNote();
        FinishVlmError(error);
        emit AssistantTurnFinished(threadId, {}, {}, error, false, true);
        return;
    }
    vlmStatus_ = QStringLiteral("Thinking...");
    RefreshOverlay();
    codexClient_->RequestVisionTurn(prompt, imagePath, threadId, true);
}

void AssistiveRuntime::CancelAssistantConversationLoad()
{
    if (codexClient_) codexClient_->CancelConversationLoad();
}

void AssistiveRuntime::LoadAssistantConversation(const QString& threadId)
{
    if (codexClient_) {
        codexClient_->LoadConversation(threadId);
    }
}

void AssistiveRuntime::RenameAssistantConversation(const QString& threadId, const QString& name)
{
    if (codexClient_) {
        codexClient_->RenameConversation(threadId, name);
    }
}

void AssistiveRuntime::DeleteAssistantConversation(const QString& threadId)
{
    if (codexClient_) {
        codexClient_->DeleteConversation(threadId);
    }
}

void AssistiveRuntime::SetExplainConversationId(const QString& threadId)
{
    explainThreadId_ = threadId.trimmed();
}

void AssistiveRuntime::StartNewExplainConversation()
{
    explainHistory_.clear();
    // An in-flight HTTP answer must not repopulate the reset conversation.
    vlmRequestRecordsExplainHistory_ = false;
    if (explainThreadId_.isEmpty()) {
        return;
    }
    explainThreadId_.clear();
    emit ExplainConversationChanged({});
}

QString AssistiveRuntime::notesFilePath() const
{
    return notesFilePath_;
}

void AssistiveRuntime::RefreshOverlay()
{
    QStringList sections;
    if (vlmEnabled_ || vlmForcedVisible_) {
        QString text = vlmText_.isEmpty() ? vlmStatus_ : vlmText_;
        if (!text.isEmpty()) {
            sections.push_back((readingText_ ? QStringLiteral("Read Text\n%1")
                                             : QStringLiteral("Scene Explain\n%1")).arg(text));
        }
    }

    const QString body = sections.join(QStringLiteral("\n\n"));
    emit OverlayUpdated(QStringLiteral("Assistive View"),
                        body,
                        !overlayDismissed_ && !body.isEmpty());
}

bool AssistiveRuntime::VlmConfigured() const
{
    if (UsesCodexProvider()) {
        return codexClient_ != nullptr;
    }
    return !ResolvedSetting(config_.vlmApiUrl, "OKUFLOW_VLM_API_URL").isEmpty() &&
           !ResolvedSetting(config_.vlmModel, "OKUFLOW_VLM_MODEL").isEmpty();
}

bool AssistiveRuntime::UsesCodexProvider() const
{
    return config_.aiProvider.trimmed().compare(QStringLiteral("codex"), Qt::CaseInsensitive) == 0;
}

bool AssistiveRuntime::ValidateFrame(const uint8_t* bgraData, int width, int height)
{
    if (!bgraData) {
        return false;
    }
    if (width < kMinFrameEdge || height < kMinFrameEdge) {
        if (!warnedDegenerateFrame_) {
            warnedDegenerateFrame_ = true;
            qWarning("AssistiveRuntime: ignoring degenerate %dx%d frame (minimum edge is %d px).",
                     width, height, kMinFrameEdge);
        }
        return false;
    }
    // BGRA stride math (width * 4 * height) must not overflow int; camera
    // dimensions are untrusted driver input.
    if (width > std::numeric_limits<int>::max() / 4 ||
        height > std::numeric_limits<int>::max() / (width * 4)) {
        qWarning("AssistiveRuntime: rejecting oversized %dx%d frame.", width, height);
        return false;
    }
    return true;
}

void AssistiveRuntime::StartVlm(const uint8_t* bgraData, int width, int height,
                                bool readText, bool continueExplain)
{
    readingText_ = readText;
    continueExplain = continueExplain && !readText;
    const QString readingPrompt = QStringLiteral(
        "Transcribe all readable text in the attached camera view, in reading order. "
        "Preserve the original wording, numbers, paragraph breaks, and source language. "
        "Do not translate or summarize the text and do not guess unreadable words. "
        "Use [unreadable] for unclear portions. Return the transcription only; "
        "if there is no readable text, say so briefly.");
    if (UsesCodexProvider()) {
        QString prompt = readText ? readingPrompt : config_.vlmPrompt.trimmed();
        if (prompt.isEmpty()) {
            prompt = QStringLiteral("Describe the visible scene briefly for a low-vision user. "
                                    "Focus on readable text, controls, and major objects.");
        }
        // Read always opens a new temporary thread. Explain continues the
        // remembered Explain thread, or opens one that later Explains reuse.
        const bool continuing = continueExplain && !explainThreadId_.isEmpty();
        if (continuing) {
            prompt = ExplainContinuationPrompt(prompt);
        }
        StartCodexVlm(
            bgraData,
            width,
            height,
            readText ? prompt : AppendResponseLanguageDirective(prompt, responseLanguageCode_),
            continueExplain ? explainThreadId_ : QString(),
            false,
            continueExplain);
        return;
    }
    if (vlmHardUnavailable_ || vlmPreparationPending_) {
        return;
    }
    if (!VlmConfigured()) {
        vlmHardUnavailable_ = true;
        FinishVlmError(VlmNotConfiguredMessage());
        return;
    }

    QImage frameImage(bgraData, width, height, width * 4, QImage::Format_ARGB32);
    QImage copy = frameImage.copy();
    if (copy.isNull()) {
        FinishVlmError(QStringLiteral("Failed to encode frame for VLM request."));
        return;
    }

    const QString apiUrl = ResolvedSetting(config_.vlmApiUrl, "OKUFLOW_VLM_API_URL");
    const QString apiKey = ResolvedSetting(config_.vlmApiKey, "OKUFLOW_VLM_API_KEY");
    const QString model = ResolvedSetting(config_.vlmModel, "OKUFLOW_VLM_MODEL");
    QString prompt = readText ? readingPrompt : ResolvedSetting(config_.vlmPrompt, "OKUFLOW_VLM_PROMPT");
    if (prompt.isEmpty()) {
        prompt = QStringLiteral("Describe the visible scene briefly for a low-vision user. Focus on readable text, UI elements, and major objects.");
    }
    // Only a user-requested Explain sends and extends the Explain history;
    // Read and periodic analyses remain independent single requests.
    const QStringList explainHistory =
        continueExplain ? explainHistory_ : QStringList();
    if (!explainHistory.isEmpty()) {
        prompt = ExplainContinuationPrompt(prompt);
    }
    // Reading copies source text verbatim, regardless of conversational
    // language, tone, or summary preferences.
    const QString assistantInstructions =
        readText ? QString() : config_.assistantInstructions.trimmed();
    const QString responseLanguageCode = readText ? QString() : responseLanguageCode_;
    const QUrl endpoint(apiUrl);
    const QString endpointName =
        endpoint.host().trimmed().isEmpty()
            ? apiUrl
            : endpoint.host().trimmed();
    const bool localEndpoint = IsLocalEndpoint(endpoint);
    if (!continueExplain) {
        EmitPrivacyNoticeIfChanged(
            QStringLiteral("Sending the current camera frame and prompt to %1 "
                           "(%2 endpoint). Conversation: temporary. Coding tools: unavailable.")
                .arg(endpointName,
                     localEndpoint
                         ? QStringLiteral("local")
                         : QStringLiteral("remote")));
    } else {
        EmitPrivacyNoticeIfChanged(
            (localEndpoint
                 ? QStringLiteral(
                       "Sending the current camera frame, prompt, and recent Explain answers "
                       "to %1 (local endpoint). Conversation: recent Explain answers, kept in "
                       "memory only. Coding tools: unavailable.")
                 : QStringLiteral(
                       "Sending the current camera frame, prompt, and recent Explain answers "
                       "to %1 (remote endpoint). Conversation: recent Explain answers, kept in "
                       "memory only. Coding tools: unavailable."))
                .arg(endpointName));
    }

    vlmPreparationPending_ = true;
    vlmPreparationPersistent_ = false;
    vlmPreparationThreadId_.clear();
    vlmRequestRecordsExplainHistory_ = continueExplain;
    if (!pendingVlmNoteImagePath_.isEmpty()) {
        RemoveNoteImage(pendingVlmNoteImagePath_);
    }
    pendingVlmNoteImagePath_ = SaveAnalyzedImageForNotes(copy);
    vlmStatus_ = QStringLiteral("Preparing the current view...");
    RefreshOverlay();
    const std::uint64_t generation = ++vlmPreparationGeneration_;
    QPointer<AssistiveRuntime> owner(this);
    const bool queued = imagePreparationPool_ &&
        imagePreparationPool_->tryStart(
        [owner,
         copy = std::move(copy),
         apiUrl,
         apiKey,
         model,
         prompt,
         assistantInstructions,
         responseLanguageCode,
         readText,
         explainHistory,
         generation]() mutable {
            if (copy.width() > kMaxVlmFrameEdge ||
                copy.height() > kMaxVlmFrameEdge) {
                copy = copy.scaled(kMaxVlmFrameEdge,
                                   kMaxVlmFrameEdge,
                                   Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
            }
            QByteArray jpegBytes;
            QBuffer buffer(&jpegBytes);
            buffer.open(QIODevice::WriteOnly);
            const bool encoded = copy.save(&buffer, "JPG", 82);
            QByteArray requestBody;
            if (encoded) {
                requestBody =
                    BuildVlmRequestBody(jpegBytes,
                                        model,
                                        prompt,
                                        assistantInstructions,
                                        responseLanguageCode,
                                        readText ? 4096 : 180,
                                        explainHistory);
            }
            if (!owner) {
                return;
            }
            QMetaObject::invokeMethod(
                owner,
                [owner,
                 encoded,
                 requestBody = std::move(requestBody),
                 apiUrl,
                 apiKey,
                 generation]() mutable {
                    if (!owner ||
                        generation != owner->vlmPreparationGeneration_ ||
                        !owner->vlmPreparationPending_) {
                        return;
                    }
                    owner->vlmPreparationPending_ = false;
                    if (!encoded) {
                        owner->FinishVlmError(
                            QStringLiteral(
                                "Failed to encode frame for VLM request."));
                        return;
                    }
                    owner->PostVlmRequest(requestBody, apiUrl, apiKey);
                },
                Qt::QueuedConnection);
        });
    if (!queued) {
        vlmPreparationPending_ = false;
        vlmRequestRecordsExplainHistory_ = false;
        FinishVlmError(
            QStringLiteral(
                "VLM image preparation is busy. Try again in a moment."));
    }
}

void AssistiveRuntime::PostVlmRequest(
    const QByteArray& requestBody,
    const QString& apiUrl,
    const QString& apiKey)
{
    QNetworkRequest request{QUrl(apiUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!apiKey.isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());
    }
    request.setTransferTimeout(kVlmTransferTimeoutMs);

    vlmStatus_ = QStringLiteral("Querying VLM...");
    RefreshOverlay();

    vlmResponseTooLarge_ = false;
    activeReply_ = networkManager_->post(request, requestBody);
    activeReply_->setReadBufferSize(kMaximumVlmResponseBytes + 1);
    connect(activeReply_, &QNetworkReply::readyRead, this, [this]() {
        if (activeReply_ &&
            activeReply_->bytesAvailable() > kMaximumVlmResponseBytes) {
            vlmResponseTooLarge_ = true;
            activeReply_->abort();
        }
    });
    connect(activeReply_, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* reply = activeReply_;
        activeReply_ = nullptr;
        if (!reply) {
            return;
        }

        const QByteArray payload =
            reply->read(kMaximumVlmResponseBytes + 1);
        if (vlmResponseTooLarge_ ||
            payload.size() > kMaximumVlmResponseBytes) {
            vlmResponseTooLarge_ = false;
            FinishVlmError(
                QStringLiteral("VLM response exceeded OkuFlow's safe size limit."));
            reply->deleteLater();
            return;
        }
        vlmResponseTooLarge_ = false;
        if (reply->error() != QNetworkReply::NoError) {
            QString detail = reply->errorString();
            const QVariant statusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            if (statusAttr.isValid()) {
                detail += QStringLiteral(" (HTTP %1)").arg(statusAttr.toInt());
            }
            const QString excerpt = TruncateText(SanitizeText(QString::fromUtf8(payload)), 160);
            if (!excerpt.isEmpty()) {
                detail += QStringLiteral(" - %1").arg(excerpt);
            }
            FinishVlmError(QStringLiteral("VLM request failed: %1").arg(detail));
            reply->deleteLater();
            return;
        }

        const QString text = ParseVlmResponseText(payload);
        if (text.isEmpty()) {
            FinishVlmError(QStringLiteral("VLM response did not contain readable text."));
        } else {
            FinishVlmSuccess(text);
        }
        reply->deleteLater();
    });
}

void AssistiveRuntime::StartCodexVlm(const uint8_t* bgraData,
                                     int width,
                                     int height,
                                     const QString& prompt,
                                     const QString& threadId,
                                     bool persistent,
                                     bool retainThread)
{
    if (!codexClient_ || vlmPreparationPending_) {
        FinishVlmError(CodexNotAvailableMessage());
        return;
    }
    EmitPrivacyNoticeIfChanged(
        retainThread
            ? QStringLiteral("Sending the current camera frame and prompt to Codex. "
                             "Conversation: saved Explain conversation, continued by each Explain. "
                             "Tool internet: blocked. Coding and file changes: blocked.")
            : QStringLiteral("Sending the current camera frame and prompt to Codex. "
                             "Conversation: temporary, new for each Read. Tool internet: blocked. "
                             "Coding and file changes: blocked."));
    QImage frameImage(bgraData,
                      width,
                      height,
                      width * 4,
                      QImage::Format_ARGB32);
    QImage copy = frameImage.copy();
    const QString imagePath =
        CreateAssistiveTemporaryFramePath(QStringLiteral("codex"),
                                          QStringLiteral("jpg"));
    if (copy.isNull() || imagePath.isEmpty()) {
        QFile::remove(imagePath);
        FinishVlmError(QStringLiteral("Failed to prepare the current view for Codex."));
        return;
    }

    vlmText_.clear();
    vlmStatus_ = QStringLiteral("Preparing the current view...");
    RefreshOverlay();
    vlmPreparationPending_ = true;
    vlmPreparationPersistent_ = persistent;
    vlmPreparationThreadId_ = threadId;
    // Persistent turns are the Advanced Assistant chat, which does not write
    // to lecture notes; only the non-persistent scene-explanation records the
    // analyzed frame.
    if (!persistent) {
        if (!pendingVlmNoteImagePath_.isEmpty()) {
            RemoveNoteImage(pendingVlmNoteImagePath_);
        }
        pendingVlmNoteImagePath_ = SaveAnalyzedImageForNotes(copy);
    }
    const std::uint64_t generation = ++vlmPreparationGeneration_;
    QPointer<AssistiveRuntime> owner(this);
    const bool queued = imagePreparationPool_ &&
        imagePreparationPool_->tryStart(
        [owner,
         copy = std::move(copy),
         imagePath,
         prompt,
         threadId,
         persistent,
         retainThread,
         generation]() mutable {
            if (copy.width() > kMaxVlmFrameEdge ||
                copy.height() > kMaxVlmFrameEdge) {
                copy = copy.scaled(kMaxVlmFrameEdge,
                                   kMaxVlmFrameEdge,
                                   Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
            }
            const bool saved = copy.save(imagePath, "JPG", 85);
            if (!saved) {
                QFile::remove(imagePath);
            }
            if (!owner) {
                if (saved) {
                    QFile::remove(imagePath);
                }
                return;
            }
            QMetaObject::invokeMethod(
                owner,
                [owner,
                 saved,
                 imagePath,
                 prompt,
                 threadId,
                 persistent,
                 retainThread,
                 generation]() {
                    if (!owner ||
                        generation != owner->vlmPreparationGeneration_ ||
                        !owner->vlmPreparationPending_) {
                        QFile::remove(imagePath);
                        return;
                    }
                    owner->vlmPreparationPending_ = false;
                    owner->vlmPreparationPersistent_ = false;
                    owner->vlmPreparationThreadId_.clear();
                    if (!saved) {
                        owner->FinishVlmError(
                            QStringLiteral(
                                "Failed to prepare the current view for Codex."));
                        return;
                    }
                    owner->vlmStatus_ = QStringLiteral("Thinking...");
                    owner->RefreshOverlay();
                    owner->codexClient_->RequestVisionTurn(
                        prompt, imagePath, threadId, persistent, retainThread);
                },
                Qt::QueuedConnection);
        });
    if (!queued) {
        vlmPreparationPending_ = false;
        vlmPreparationPersistent_ = false;
        vlmPreparationThreadId_.clear();
        QFile::remove(imagePath);
        FinishVlmError(
            QStringLiteral(
                "Codex image preparation is busy. Try again in a moment."));
    }
}

void AssistiveRuntime::FinishVlmSuccess(const QString& text)
{
    const QString analyzedImage = pendingVlmNoteImagePath_;
    pendingVlmNoteImagePath_.clear();
    const bool recordExplainHistory = vlmRequestRecordsExplainHistory_;
    vlmRequestRecordsExplainHistory_ = false;
    const QString fullText = SanitizeText(text);
    vlmText_ = readingText_ ? fullText.left(kMaximumDisplayedAssistantCharacters)
                            : TruncateText(fullText, 700);
    bool noted = false;
    if (vlmText_.isEmpty()) {
        vlmStatus_ = QStringLiteral("VLM returned an empty description.");
    } else {
        vlmStatus_.clear();
        if (recordExplainHistory && !readingText_) {
            explainHistory_.append(
                fullText.left(kMaximumExplainHistoryAnswerCharacters));
            while (explainHistory_.size() > kMaximumExplainHistoryAnswers) {
                explainHistory_.removeFirst();
            }
        }
        noted = AppendNoteSection(
            QCoreApplication::translate("OkuFlow", readingText_ ? "Text on screen" : "Scene explanation"),
            fullText, analyzedImage, true);
    }
    if (!noted && !analyzedImage.isEmpty()) {
        RemoveNoteImage(analyzedImage);
    }
    RefreshOverlay();
}

void AssistiveRuntime::FinishVlmError(const QString& errorText)
{
    vlmRequestRecordsExplainHistory_ = false;
    if (!pendingVlmNoteImagePath_.isEmpty()) {
        RemoveNoteImage(pendingVlmNoteImagePath_);
        pendingVlmNoteImagePath_.clear();
    }
    vlmText_.clear();
    vlmStatus_ = SanitizeText(errorText);
    RefreshOverlay();
}

void AssistiveRuntime::ClearPendingAssistantNote()
{
    if (!pendingAssistantNoteImagePath_.isEmpty()) {
        RemoveNoteImage(pendingAssistantNoteImagePath_);
        pendingAssistantNoteImagePath_.clear();
    }
    pendingAssistantNotePrompt_.clear();
}

void AssistiveRuntime::FinishAssistantTurnSuccess(const QString& threadId,
                                                  const QString& text)
{
    readingText_ = false;
    const QString question = pendingAssistantNotePrompt_.trimmed();
    const QString analyzedImage = pendingAssistantNoteImagePath_;
    pendingAssistantNotePrompt_.clear();
    pendingAssistantNoteImagePath_.clear();

    const QString fullText = SanitizeText(text);
    vlmText_ = TruncateText(fullText, 700);
    bool noted = false;
    if (fullText.isEmpty()) {
        vlmStatus_ = QStringLiteral("Assistant returned an empty answer.");
    } else {
        vlmStatus_.clear();
        if (config_.lectureNotesEnabled &&
            !config_.notesDirectory.trimmed().isEmpty() && EnsureNotesFile()) {
            QString content;
            if (!question.isEmpty()) {
                content += QStringLiteral(
                               "    <p class=\"note-ask\">%1</p>\n")
                               .arg(question.toHtmlEscaped());
            }
            if (!analyzedImage.isEmpty()) {
                const QString imageUrl =
                    NotesMediaUrl(notesFilePath_, analyzedImage).toHtmlEscaped();
                content += QStringLiteral(
                               "    <figure class=\"ai-shot\">\n"
                               "      <a href=\"%1\"><img src=\"%1\" alt=\"%2\" loading=\"lazy\"></a>\n"
                               "    </figure>\n")
                               .arg(imageUrl,
                                    QCoreApplication::translate(
                                        "OkuFlow", "View the assistant analyzed")
                                        .toHtmlEscaped());
            }
            content += QStringLiteral("    <p class=\"note-text\">%1</p>\n")
                           .arg(fullText.toHtmlEscaped());
            NoteSectionMetadata metadata;
            metadata.cssClass = QStringLiteral("ai");
            metadata.kind = QStringLiteral("ai");
            metadata.startCollapsed = true;
            // The turn's title is the question so a collapsed turn is
            // identifiable; the preview shows the answer.
            metadata.previewText = fullText.simplified().left(160);
            // A sanitized thread id groups all turns of one conversation.
            QString conversation = threadId;
            conversation.replace(QLatin1Char('_'), QLatin1Char('-'));
            conversation.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9-]")));
            metadata.conversationId = conversation.left(64);
            const QString heading =
                question.isEmpty()
                    ? QCoreApplication::translate("OkuFlow", "Assistant")
                    : question.simplified().left(80);
            noted = AppendNoteHtmlSection(heading, content, metadata);
        }
    }
    if (!noted && !analyzedImage.isEmpty()) {
        RemoveNoteImage(analyzedImage);
    }
    RefreshOverlay();
}

void AssistiveRuntime::EmitPrivacyNoticeIfChanged(const QString& summary)
{
    const QString clean = summary.simplified();
    if (clean.isEmpty() || clean == lastPrivacyNotice_) {
        return;
    }
    lastPrivacyNotice_ = clean;
    emit PrivacyNotice(clean);
}

bool AssistiveRuntime::EnsureNotesFile()
{
    if (!notesFilePath_.isEmpty()) {
        return true;
    }
    const QString directory = config_.notesDirectory.trimmed();
    if (directory.isEmpty()) {
        return false;
    }
    const QDateTime now = QDateTime::currentDateTime();
    const QString fileName = QStringLiteral("NOTES_%1_%2.html")
                                 .arg(now.toString(QStringLiteral("yyyyMMdd_HHmmss")),
                                      QUuid::createUuid().toString(QUuid::Id128));
    const QString path = QDir(directory).filePath(fileName);

    const QString displayTime = now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const QString machineTime = now.toString(Qt::ISODate);
    // The document language matches the response/UI language at creation
    // time; every appended section restates its own language so a mid-session
    // switch cannot make a screen reader misread later sections.
    const QString documentLanguage =
        responseLanguageCode_.isEmpty() ? QStringLiteral("en")
                                        : responseLanguageCode_;
    const QString notesTitle =
        QCoreApplication::translate("OkuFlow", "OkuFlow Lecture Notes");
    const QString startedLine =
        QCoreApplication::translate("OkuFlow", "Started %1")
            .toHtmlEscaped()
            .arg(QStringLiteral("<time datetime=\"%1\">%2</time>")
                     .arg(machineTime.toHtmlEscaped(),
                          displayTime.toHtmlEscaped()));
    // The head is written exactly once; every later entry is one appended,
    // flushed, self-contained block, so a crash mid-lecture still leaves a
    // fully styled, fully readable page. Styling and the small viewer
    // script are inline: the file must work offline with no external
    // resources. Placeholders are token-replaced (never QString::arg) so
    // literal % signs in CSS/JS stay inert.
    QString document = QString::fromUtf8(kNotesDocumentTemplate);
    document.replace(QStringLiteral("@@TRANSCRIBE_SCRIPT@@"),
                     QString::fromUtf8(kNotesTranscribeBridge));
    document.replace(QStringLiteral("@@LANG@@"),
                     documentLanguage.toHtmlEscaped());
    document.replace(QStringLiteral("@@CONVOLABEL@@"),
                     QCoreApplication::translate("OkuFlow", "Conversation")
                         .toHtmlEscaped());
    document.replace(QStringLiteral("@@TITLE@@"), notesTitle.toHtmlEscaped());
    document.replace(QStringLiteral("@@TIME@@"), displayTime.toHtmlEscaped());
    document.replace(QStringLiteral("@@CREATED@@"), startedLine);
    document.replace(QStringLiteral("@@EXPAND@@"),
                     QCoreApplication::translate("OkuFlow", "Expand all")
                         .toHtmlEscaped());
    document.replace(QStringLiteral("@@COLLAPSE@@"),
                     QCoreApplication::translate("OkuFlow", "Collapse all")
                         .toHtmlEscaped());
    document.replace(QStringLiteral("@@THEMELABEL@@"),
                     QCoreApplication::translate("OkuFlow", "Light or dark colors")
                         .toHtmlEscaped());
    if (!QueueNotesWork(path, document.size() * 2,
                        [path, directory, document] {
        if (!QDir().mkpath(directory)) return false;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        const QByteArray bytes = document.toUtf8();
        return file.write(bytes) == bytes.size() && file.commit();
    })) return false;

    notesFilePath_ = path;
    notesDocumentOpen_ = true;
    return true;
}

void AssistiveRuntime::FinalizeNotesFile()
{
    if (!notesDocumentOpen_ || notesFilePath_.isEmpty()) {
        return;
    }
    AppendNoteBlock(QStringLiteral("</main>\n</body>\n</html>\n"));
    notesDocumentOpen_ = false;
}

bool AssistiveRuntime::AppendNoteSection(const QString& heading,
                                         const QString& bodyText,
                                         const QString& imagePath,
                                         bool aiText)
{
    if (!config_.lectureNotesEnabled || config_.notesDirectory.trimmed().isEmpty()) {
        RemoveNoteImage(imagePath);
        return false;
    }
    if (bodyText.trimmed().isEmpty() && imagePath.trimmed().isEmpty()) {
        RemoveNoteImage(imagePath);
        return false;
    }
    if (!EnsureNotesFile()) {
        RemoveNoteImage(imagePath);
        return false;
    }

    QString content;
    NoteSectionMetadata metadata;
    if (aiText) {
        // An AI answer: embed the exact frame the AI analyzed above its text
        // so the note records what was seen, then the answer. Long answers
        // start collapsed with a one-line preview to keep the page compact.
        if (!imagePath.trimmed().isEmpty()) {
            const QString imageUrl =
                NotesMediaUrl(notesFilePath_, imagePath).toHtmlEscaped();
            content += QStringLiteral(
                           "    <figure class=\"ai-shot\">\n"
                           "      <a href=\"%1\"><img src=\"%1\" alt=\"%2\" loading=\"lazy\"></a>\n"
                           "    </figure>\n")
                           .arg(imageUrl,
                                QCoreApplication::translate(
                                    "OkuFlow", "View the assistant analyzed")
                                    .toHtmlEscaped());
        }
        content += QStringLiteral("    <p class=\"note-text\">%1</p>\n")
                       .arg(bodyText.toHtmlEscaped());
        metadata.cssClass = QStringLiteral("ai");
        metadata.kind = QStringLiteral("ai");
        metadata.startCollapsed = true;
        metadata.previewText = bodyText.simplified().left(160);
    } else if (!imagePath.trimmed().isEmpty()) {
        const QString imageUrl =
            NotesMediaUrl(notesFilePath_, imagePath).toHtmlEscaped();
        content = QStringLiteral(
                      "    <figure>\n"
                      "      <a href=\"%1\"><img src=\"%1\" alt=\"%2\" loading=\"lazy\"></a>\n"
                      "      <figcaption>%3</figcaption>\n"
                      "    </figure>\n")
                      .arg(imageUrl,
                           QCoreApplication::translate(
                               "OkuFlow", "Captured processed camera view")
                               .toHtmlEscaped(),
                           QCoreApplication::translate(
                               "OkuFlow", "Processed camera view")
                               .toHtmlEscaped());
        // A single processed image reaches here only for annotation
        // snapshots; the marked-drawing icon fits better than a camera.
        metadata.cssClass = QStringLiteral("media");
        metadata.kind = QStringLiteral("draw");
    } else {
        content = QStringLiteral("    <p class=\"note-text\">%1</p>\n")
                      .arg(bodyText.toHtmlEscaped());
    }
    const bool accepted = AppendNoteHtmlSection(heading, content, metadata);
    if (!accepted) RemoveNoteImage(imagePath);
    return accepted;
}

QString AssistiveRuntime::SaveAnalyzedImageForNotes(const QImage& image)
{
    if (!config_.lectureNotesEnabled ||
        config_.notesDirectory.trimmed().isEmpty() || image.isNull()) {
        return {};
    }
    qint64 retainedBytes = image.sizeInBytes();
    for (const auto& retained : noteImages_) retainedBytes += retained.sizeInBytes();
    if (noteImages_.size() >= 3 || retainedBytes > 192LL * 1024 * 1024) {
        emit NotesWriteFinished(config_.notesDirectory, QStringLiteral("Notes image queue is full."));
        return {};
    }
    const QString path = QDir(config_.notesDirectory).filePath(
        QStringLiteral("images/AI_%1.jpg").arg(QUuid::createUuid().toString(QUuid::Id128)));
    noteImages_.insert(path, image);
    return path;
}

void AssistiveRuntime::RemoveNoteImage(const QString& path)
{
    // Unsubmitted images exist only in memory. Submitted images belong to the
    // writer, which removes them if the associated HTML block fails.
    noteImages_.remove(path);
}

void AssistiveRuntime::AppendNoteMediaPair(const QString& heading,
                                           const QString& originalPath,
                                           const QString& processedPath,
                                           bool video)
{
    if (!config_.lectureNotesEnabled ||
        config_.notesDirectory.trimmed().isEmpty() ||
        originalPath.trimmed().isEmpty() ||
        processedPath.trimmed().isEmpty() ||
        !EnsureNotesFile()) {
        return;
    }

    const QString originalUrl =
        NotesMediaUrl(notesFilePath_, originalPath).toHtmlEscaped();
    const QString processedUrl =
        NotesMediaUrl(notesFilePath_, processedPath).toHtmlEscaped();
    QString originalMedia;
    QString processedMedia;
    if (video) {
        originalMedia = QStringLiteral(
                            "        <video controls preload=\"metadata\">\n"
                            "          <source src=\"%1\" type=\"video/mp4\">\n"
                            "          <a href=\"%1\">%2</a>\n"
                            "        </video>\n")
                            .arg(originalUrl,
                                 QCoreApplication::translate(
                                     "OkuFlow", "Open original video")
                                     .toHtmlEscaped());
        processedMedia = QStringLiteral(
                             "        <video controls preload=\"metadata\">\n"
                             "          <source src=\"%1\" type=\"video/mp4\">\n"
                             "          <a href=\"%1\">%2</a>\n"
                             "        </video>\n")
                             .arg(processedUrl,
                                  QCoreApplication::translate(
                                      "OkuFlow", "Open processed video")
                                      .toHtmlEscaped());
    } else {
        originalMedia = QStringLiteral(
                            "        <a href=\"%1\"><img src=\"%1\" alt=\"%2\" loading=\"lazy\"></a>\n")
                            .arg(originalUrl,
                                 QCoreApplication::translate(
                                     "OkuFlow", "Original camera view")
                                     .toHtmlEscaped());
        processedMedia = QStringLiteral(
                             "        <a href=\"%1\"><img src=\"%1\" alt=\"%2\" loading=\"lazy\"></a>\n")
                             .arg(processedUrl,
                                  QCoreApplication::translate(
                                      "OkuFlow", "Processed camera view")
                                      .toHtmlEscaped());
    }

    // Full sentences per language — never compose "Original camera" + type,
    // word order differs in Turkish and German.
    const QString originalCaption =
        (video ? QCoreApplication::translate("OkuFlow", "Original camera video")
               : QCoreApplication::translate("OkuFlow", "Original camera photo"))
            .toHtmlEscaped();
    const QString processedCaption =
        (video ? QCoreApplication::translate("OkuFlow", "Processed camera video")
               : QCoreApplication::translate("OkuFlow", "Processed camera photo"))
            .toHtmlEscaped();
    QString content =
        QStringLiteral(
            "    <div class=\"media-grid\">\n"
            "      <figure>\n"
            "%1"
            "        <figcaption><a href=\"%2\">%3</a></figcaption>\n"
            "      </figure>\n"
            "      <figure>\n"
            "%4"
            "        <figcaption><a href=\"%5\">%6</a></figcaption>\n"
            "      </figure>\n"
            "    </div>\n")
            .arg(originalMedia,
                 originalUrl,
                 originalCaption,
                 processedMedia,
                 processedUrl,
                 processedCaption);
    if (video) {
        content += QStringLiteral(
            "    <section class=\"transcribe-tools\" data-transcribe-src=\"%1\">\n"
            "      <div class=\"transcribe-actions\"><button type=\"button\" class=\"transcribe-start\">Transcribe</button>"
            "<button type=\"button\" class=\"transcribe-stop\" hidden>Stop</button>"
            "<button type=\"button\" class=\"transcribe-export\" hidden>Save as text</button></div>\n"
            "      <p class=\"transcribe-status\" role=\"status\"></p>\n"
            "      <div class=\"transcribe-result\" hidden><h3 class=\"transcribe-heading\">Transcript</h3>"
            "<div class=\"transcribe-lines\"></div></div>\n"
            "    </section>\n").arg(originalUrl);
    }
    NoteSectionMetadata metadata;
    metadata.cssClass = QStringLiteral("media");
    metadata.kind = video ? QStringLiteral("video") : QStringLiteral("photo");
    metadata.previewText =
        video ? QCoreApplication::translate("OkuFlow",
                                            "Original and processed video")
              : QCoreApplication::translate("OkuFlow",
                                            "Original and processed photo");
    AppendNoteHtmlSection(heading, content, metadata);
}

bool AssistiveRuntime::QueueNotesWork(const QString& path, qint64 bytes,
                                      std::function<bool()> work,
                                      const QString& identity)
{
    const auto state = notesWork_;
    std::unique_lock lock(state->mutex);
    if (state->count >= 128 || bytes > 256LL * 1024 * 1024 - state->bytes) {
        lock.unlock();
        emit NotesWriteFinished(path, QStringLiteral("Notes storage queue is full."));
        return false;
    }
    state->jobs.push_back({path, identity, bytes, std::move(work)});
    ++pendingNotesWrites_;
    state->bytes += bytes;
    ++state->count;
    if (state->running) return true;
    state->running = true;
    QThreadPool::globalInstance()->start([state] {
        for (;;) {
            NotesWorkState::Job job;
            {
                std::lock_guard guard(state->mutex);
                if (state->jobs.empty()) {
                    state->running = false;
                    return;
                }
                job = std::move(state->jobs.front());
                state->jobs.pop_front();
            }
            bool success = false;
            try { success = job.work(); } catch (...) { success = false; }
            std::lock_guard guard(state->mutex);
            state->bytes -= job.bytes;
            --state->count;
            if (state->receiver) {
                auto* receiver = state->receiver;
                QMetaObject::invokeMethod(receiver,
                    [receiver, path = job.path, identity = job.identity, success] {
                        --receiver->pendingNotesWrites_;
                        if (!success) {
                            if (receiver->notesFilePath_ == path) {
                                receiver->notesFilePath_.clear();
                                receiver->notesDocumentOpen_ = false;
                            }
                        }
                        emit receiver->NotesWriteFinished(path, success ? QString() :
                            QStringLiteral("Failed to save lecture notes."));
                    }, Qt::QueuedConnection);
            }
        }
    });
    return true;
}

bool AssistiveRuntime::AppendNoteBlock(const QString& blockHtml, const QString& identity)
{
    if (!notesDocumentOpen_ || notesFilePath_.isEmpty()) return false;
    const QString path = notesFilePath_;
    QHash<QString, QImage> images;
    qint64 bytes = blockHtml.size() * 2;
    for (auto it = noteImages_.begin(); it != noteImages_.end();) {
        if (blockHtml.contains(NotesMediaUrl(path, it.key()).toHtmlEscaped())) {
            bytes += it.value().sizeInBytes();
            images.insert(it.key(), it.value());
            it = noteImages_.erase(it);
        } else ++it;
    }
    return QueueNotesWork(path, bytes, [path, blockHtml, images] {
        QStringList created;
        auto cleanup = [&] { for (const auto& image : created) QFile::remove(image); };
        for (auto it = images.cbegin(); it != images.cend(); ++it) {
            QImage image = it.value();
            if (std::max(image.width(), image.height()) > 1920)
                image = image.scaled(1920, 1920, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            created.append(it.key());
            if (!QDir().mkpath(QFileInfo(it.key()).absolutePath()) ||
                !image.save(it.key(), "JPG", 88)) { cleanup(); return false; }
        }
        const QByteArray blockBytes = blockHtml.toUtf8();
        QFile outputFile(path);
        // Never create a headerless file after a failed document creation.
        if (!outputFile.exists() || !outputFile.open(QIODevice::ReadWrite | QIODevice::Append)) {
            cleanup(); return false;
        }
        const qint64 originalSize = outputFile.size();
        if (outputFile.write(blockBytes) != blockBytes.size() || !outputFile.flush()) {
            if (!outputFile.resize(originalSize) || !outputFile.flush())
                qWarning("AssistiveRuntime: failed to roll back notes file %s", qPrintable(path));
            cleanup(); return false;
        }
        return true;
    }, identity);
}

bool AssistiveRuntime::AppendNoteHtmlSection(const QString& heading,
                                             const QString& contentHtml,
                                             const NoteSectionMetadata& metadata)
{
    if (contentHtml.trimmed().isEmpty() || notesFilePath_.isEmpty()) {
        return false;
    }
    const QString timestamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    // Sections restate the language they were written in; the document-level
    // lang only reflects the language at file creation and the user can
    // switch mid-session.
    const QString sectionLanguage =
        !metadata.languageOverride.isEmpty()
            ? metadata.languageOverride
            : (responseLanguageCode_.isEmpty() ? QStringLiteral("en")
                                               : responseLanguageCode_);
    // Attribute values are typed and validated by the callers; they are
    // still escaped here so no caller mistake can break out of a quote.
    QString attributes;
    if (IsSafeAttributeToken(metadata.id, QStringLiteral("_-"))) {
        attributes += QStringLiteral(" id=\"%1\"").arg(metadata.id.toHtmlEscaped());
    }
    QString classes = QStringLiteral("note");
    if (IsSafeAttributeToken(metadata.cssClass, QStringLiteral(" _-"))) {
        classes += QLatin1Char(' ') + metadata.cssClass;
    }
    attributes += QStringLiteral(" class=\"%1\"").arg(classes.toHtmlEscaped());
    if (IsSafeAttributeToken(metadata.dataRecordingSession, QStringLiteral("-"))) {
        attributes += QStringLiteral(" data-recording-session=\"%1\"")
                          .arg(metadata.dataRecordingSession.toHtmlEscaped());
    }
    if (metadata.dataSequence >= 0) {
        attributes += QStringLiteral(" data-sequence=\"%1\"")
                          .arg(metadata.dataSequence);
    }
    if (metadata.approximateTime) {
        attributes += QStringLiteral(" data-time-precision=\"approximate\"");
    }
    if (IsSafeAttributeToken(metadata.kind, QString())) {
        attributes += QStringLiteral(" data-kind=\"%1\"").arg(metadata.kind);
    }
    if (IsSafeAttributeToken(metadata.conversationId, QStringLiteral("_-"))) {
        attributes += QStringLiteral(" data-conversation=\"%1\"")
                          .arg(metadata.conversationId.toHtmlEscaped());
    }
    // Native <details>/<summary> gives every note keyboard- and
    // screen-reader-accessible collapsing with zero script dependency; the
    // heading inside the summary keeps heading navigation working. Summary
    // order: disclosure, type icon, title, preview (fills the middle),
    // timestamp pinned far right (least prominent).
    const QString preview =
        QStringLiteral("<span class=\"preview\">%1</span>")
            .arg(metadata.previewText.trimmed().isEmpty()
                     ? QString()
                     : metadata.previewText.toHtmlEscaped());
    const QString section = QStringLiteral(
                                "  <details%5 lang=\"%4\"%6>\n"
                                "    <summary><span class=\"ic\" aria-hidden=\"true\"></span>"
                                "<h2 class=\"ttl\">%2</h2>%7"
                                "<time class=\"at\">%1</time></summary>\n"
                                "    <div class=\"note-body\">\n"
                                "%3"
                                "    </div>\n"
                                "  </details>\n")
                                .arg(timestamp.toHtmlEscaped(),
                                     heading.toHtmlEscaped(),
                                     contentHtml,
                                     sectionLanguage.toHtmlEscaped(),
                                     attributes,
                                     metadata.startCollapsed ? QString()
                                                             : QStringLiteral(" open"),
                                     preview);
    return AppendNoteBlock(section);
}

void AssistiveRuntime::SpeakText(const QString& text)
{
#if OKUFLOW_HAS_TTS
    if (text.trimmed().isEmpty()) {
        return;
    }
    if (!tts_) {
        const QStringList engines = QTextToSpeech::availableEngines();
        auto availableEngine = [&engines](const QString& requested) {
            for (const QString& engine : engines) {
                if (engine.compare(requested, Qt::CaseInsensitive) == 0) {
                    return engine;
                }
            }
            return QString();
        };

        QString engine = availableEngine(config_.ttsEngine.trimmed());
        if (engine.isEmpty()) {
            engine = availableEngine(QStringLiteral("winrt"));
        }
        tts_ = engine.isEmpty() ? new QTextToSpeech(this)
                                : new QTextToSpeech(engine, this);
        if (tts_->state() == QTextToSpeech::Error &&
            tts_->engine().compare(QStringLiteral("winrt"), Qt::CaseInsensitive) == 0) {
            const QString fallbackEngine = availableEngine(QStringLiteral("sapi"));
            if (!fallbackEngine.isEmpty()) {
                tts_->setEngine(fallbackEngine);
            }
        }
    }

    tts_->setRate(std::clamp(config_.ttsRate, -1.0, 1.0));
    if (!config_.ttsVoiceName.trimmed().isEmpty()) {
        const QList<QVoice> voices = tts_->findVoices();
        for (const QVoice& voice : voices) {
            const bool nameMatches = voice.name() == config_.ttsVoiceName;
            const bool localeMatches = config_.ttsVoiceLocale.trimmed().isEmpty() ||
                                       voice.locale().name() == config_.ttsVoiceLocale;
            if (nameMatches && localeMatches) {
                tts_->setVoice(voice);
                break;
            }
        }
    }
    SelectVoiceForResponseLanguage(true);
    tts_->stop();
    tts_->say(text);
#else
    Q_UNUSED(text);
#endif
}

bool AssistiveRuntime::SelectVoiceForResponseLanguage(bool notifyMissing)
{
#if OKUFLOW_HAS_TTS
    if (!tts_) {
        return false;
    }

    QLocale targetLocale;
    if (responseLanguageCode_ == QStringLiteral("tr")) {
        targetLocale = QLocale(QLocale::Turkish, QLocale::Turkey);
    } else if (responseLanguageCode_ == QStringLiteral("de")) {
        targetLocale = QLocale(QLocale::German, QLocale::Germany);
    } else {
        targetLocale = QLocale(QLocale::English, QLocale::UnitedStates);
    }

    const QVoice current = tts_->voice();
    if (current.locale().language() == targetLocale.language()) {
        return true;
    }

    const QList<QVoice> voices = tts_->findVoices();
    const QVoice* best = nullptr;
    for (const QVoice& voice : voices) {
        if (voice.locale().language() != targetLocale.language()) {
            continue;
        }
        if (!best) {
            best = &voice;
        }
        if (voice.locale().territory() == targetLocale.territory()) {
            best = &voice;
            break;
        }
    }
    if (best) {
        tts_->setVoice(*best);
        warnedMissingVoiceLanguage_.clear();
        return true;
    }

    if (notifyMissing &&
        warnedMissingVoiceLanguage_ != responseLanguageCode_) {
        warnedMissingVoiceLanguage_ = responseLanguageCode_;
        const QString languageName =
            responseLanguageCode_ == QStringLiteral("tr")
                ? QStringLiteral("Türkçe")
                : responseLanguageCode_ == QStringLiteral("de")
                      ? QStringLiteral("Deutsch")
                      : QStringLiteral("English");
        emit StatusNotice(
            QCoreApplication::translate(
                "OkuFlow",
                "No %1 voice is installed — using the current voice. "
                "Install one under Windows Settings → Time & Language → Speech.")
                .arg(languageName));
    }
    return false;
#else
    Q_UNUSED(notifyMissing);
    return false;
#endif
}

void AssistiveRuntime::StopSpeech()
{
#if OKUFLOW_HAS_TTS
    if (tts_) {
        tts_->stop();
    }
#endif
}

} // namespace okuflow

#endif // _WIN32
