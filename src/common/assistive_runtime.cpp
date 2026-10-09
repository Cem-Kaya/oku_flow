#ifdef _WIN32

#include "openzoom/common/assistive_runtime.hpp"
#include "openzoom/common/response_language.hpp"
#include "openzoom/common/codex_app_server_client.hpp"

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

#if OPENZOOM_HAS_TTS
#include <QTextToSpeech>
#include <QVoice>
#endif

namespace openzoom {

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
                          "or via OPENZOOM_VLM_API_URL and OPENZOOM_VLM_MODEL. An API key is optional for local servers.");
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
                QStringLiteral("openzoom_%1_%2_XXXXXX.%3")
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
        QStringLiteral(R"(^openzoom_codex_(\d+)_.*\.(?:png|jpg)$)"));
    const QDateTime legacyCutoff =
        QDateTime::currentDateTimeUtc().addSecs(-60 * 60);
    int removed = 0;
    for (const QString& pattern :
         {QStringLiteral("openzoom_codex_*.jpg")}) {
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
         {QStringLiteral("openzoom_codex_%1_*.jpg").arg(processId)}) {
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
                               int maximumTokens)
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
    connect(codexClient_.get(), &CodexAppServerClient::ConversationRenamed,
            this, &AssistiveRuntime::AssistantConversationRenamed);
    connect(codexClient_.get(), &CodexAppServerClient::ConversationDeleted,
            this, &AssistiveRuntime::AssistantConversationDeleted);
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
    if (speechConfigChanged) {
        StopSpeech();
#if OPENZOOM_HAS_TTS
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
#if OPENZOOM_HAS_TTS
    if (tts_) {
        SelectVoiceForResponseLanguage(true);
    }
#endif
}

QString AssistiveRuntime::AppendResponseLanguageDirective(
    const QString& prompt,
    const QString& languageCode)
{
    return openzoom::AppendResponseLanguageDirective(prompt, languageCode);
}

void AssistiveRuntime::SetModes(bool vlmEnabled)
{
    const bool vlmTurningOff = vlmEnabled_ && !vlmEnabled;

    if (vlmTurningOff && activeReply_) {
        activeReply_->abort();
    }
    if (vlmTurningOff && vlmPreparationPending_) {
        ++vlmPreparationGeneration_;
        vlmPreparationPending_ = false;
        vlmPreparationPersistent_ = false;
        vlmPreparationThreadId_.clear();
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
    StartVlm(bgraData, width, height, readText);
}

void AssistiveRuntime::NoteCapturedPhotoPair(const QString& originalPath,
                                             const QString& processedPath)
{
    AppendNoteMediaPair(QCoreApplication::translate("OpenZoom", "Photo captured"),
                        originalPath,
                        processedPath,
                        false);
}

void AssistiveRuntime::NoteCapturedVideoPair(const QString& originalPath,
                                             const QString& processedPath)
{
    AppendNoteMediaPair(QCoreApplication::translate("OpenZoom", "Video recorded"),
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

QString FormatClockDuration(qint64 duration100ns)
{
    const qint64 totalSeconds = duration100ns / 10000000LL;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

// Compact chip form: M:SS below one hour, H:MM:SS beyond.
QString FormatShortClockDuration(qint64 duration100ns)
{
    const qint64 totalSeconds = duration100ns / 10000000LL;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

} // namespace

bool AssistiveRuntime::NoteTranscriptSegment(const TranscriptSegment& segment)
{
    if (!config_.lectureNotesEnabled ||
        config_.notesDirectory.trimmed().isEmpty()) {
        return false;
    }
    const QString text = segment.text.trimmed();
    if (text.isEmpty() ||
        text.size() > transcript_limits::kMaximumSegmentCharacters ||
        !IsSafeAttributeToken(segment.recordingSessionId, QStringLiteral("-")) ||
        segment.sequence == 0) {
        return false;
    }
    if (!EnsureNotesFile()) {
        return false;
    }
    const QString identity = notesFilePath_ + QLatin1Char('|') +
        segment.recordingSessionId + QLatin1Char('|') + QString::number(segment.sequence);
    if (acceptedTranscriptIdentities_.contains(identity)) {
        return true;
    }

    // One compact feed line per finalized phrase — no per-phrase heading.
    // The load-time script in the document head groups consecutive lines of
    // one recording into a single collapsible transcript block; without
    // script the lines still render as a clean feed.
    QString timeChip;
    if (segment.approximateOffset100ns >= 0) {
        const qint64 offsetSeconds =
            segment.approximateOffset100ns / 10000000LL;
        timeChip = QStringLiteral(
                       " <time class=\"tr-at\" datetime=\"PT%1S\" title=\"%2\">%3</time>")
                       .arg(QString::number(offsetSeconds),
                            QCoreApplication::translate("OpenZoom",
                                                        "about %1 into recording")
                                .arg(FormatClockDuration(segment.approximateOffset100ns))
                                .toHtmlEscaped(),
                            FormatShortClockDuration(segment.approximateOffset100ns)
                                .toHtmlEscaped());
    }
    const QString language = segment.languageCode.trimmed().left(12);
    const QString languageAttribute =
        IsSafeAttributeToken(language, QStringLiteral("-"))
            ? QStringLiteral(" lang=\"%1\"").arg(language.toHtmlEscaped())
            : QString();
    const QString line =
        QStringLiteral(
            "  <p class=\"tr\" id=\"transcript-%1-%2\" data-recording-session=\"%3\""
            " data-sequence=\"%2\" data-time-precision=\"approximate\"%4>"
            "<span class=\"tr-text\">%5%6</span>%7</p>\n")
            .arg(segment.recordingSessionId.left(8).toHtmlEscaped(),
                 QString::number(segment.sequence),
                 segment.recordingSessionId.toHtmlEscaped(),
                 languageAttribute,
                 text.toHtmlEscaped(),
                 segment.truncated
                     ? QStringLiteral(" %1").arg(
                           QCoreApplication::translate("OpenZoom", "(truncated)")
                               .toHtmlEscaped())
                     : QString(),
                 timeChip);
    if (!AppendNoteBlock(line, identity)) {
        return false;
    }
    acceptedTranscriptIdentities_.insert(identity);
    return true;
}

bool AssistiveRuntime::NoteTranscriptGap(const QString& recordingSessionId)
{
    if (!config_.lectureNotesEnabled ||
        config_.notesDirectory.trimmed().isEmpty() ||
        !IsSafeAttributeToken(recordingSessionId, QStringLiteral("-")) ||
        !EnsureNotesFile()) {
        return false;
    }
    // A gap renders inside the transcript feed as a visually distinct line,
    // so later text never reads as continuous across dropped audio.
    return AppendNoteBlock(
        QStringLiteral(
            "  <p class=\"tr tr-gap\" data-recording-session=\"%1\">"
            "<span class=\"tr-text\">%2</span></p>\n")
            .arg(recordingSessionId.toHtmlEscaped(),
                 QCoreApplication::translate(
                     "OpenZoom",
                     "Transcript has a gap; recording is unaffected.")
                     .toHtmlEscaped()));
}

void AssistiveRuntime::NoteAnnotationSnapshot(const QString& filePath,
                                              const QString& heading)
{
    if (filePath.trimmed().isEmpty()) {
        return;
    }
    AppendNoteSection(
        heading.trimmed().isEmpty()
            ? QCoreApplication::translate("OpenZoom", "Annotated view")
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

void AssistiveRuntime::StopAssistant()
{
    if (vlmPreparationPending_) {
        const QString threadId = vlmPreparationThreadId_;
        const bool persistent = vlmPreparationPersistent_;
        ++vlmPreparationGeneration_;
        vlmPreparationPending_ = false;
        vlmPreparationPersistent_ = false;
        vlmPreparationThreadId_.clear();
        vlmText_.clear();
        vlmStatus_ = QStringLiteral("Assistant stopped.");
        RefreshOverlay();
        if (persistent) {
            emit AssistantTurnFinished(threadId,
                                       {},
                                       {},
                                       QStringLiteral("Assistant stopped."),
                                       true,
                                       true);
        }
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
                    if (!owner ||
                        generation != owner->vlmPreparationGeneration_ ||
                        !owner->vlmPreparationPending_) {
                        QFile::remove(imagePath);
                        if (owner) {
                            owner->ClearPendingAssistantNote();
                        }
                        return;
                    }
                    owner->vlmPreparationPending_ = false;
                    owner->vlmPreparationPersistent_ = false;
                    owner->vlmPreparationThreadId_.clear();
                    if (!saved) {
                        const QString error =
                            QStringLiteral(
                                "Could not prepare the current camera view.");
                        owner->ClearPendingAssistantNote();
                        owner->FinishVlmError(error);
                        emit owner->AssistantTurnFinished(
                            threadId, {}, {}, error, false, true);
                        return;
                    }
                    owner->vlmStatus_ = QStringLiteral("Thinking...");
                    owner->RefreshOverlay();
                    owner->codexClient_->RequestVisionTurn(
                        prompt, imagePath, threadId, true);
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
    return !ResolvedSetting(config_.vlmApiUrl, "OPENZOOM_VLM_API_URL").isEmpty() &&
           !ResolvedSetting(config_.vlmModel, "OPENZOOM_VLM_MODEL").isEmpty();
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

void AssistiveRuntime::StartVlm(const uint8_t* bgraData, int width, int height, bool readText)
{
    readingText_ = readText;
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
        StartCodexVlm(
            bgraData,
            width,
            height,
            readText ? prompt : AppendResponseLanguageDirective(prompt, responseLanguageCode_),
            {},
            false);
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

    const QString apiUrl = ResolvedSetting(config_.vlmApiUrl, "OPENZOOM_VLM_API_URL");
    const QString apiKey = ResolvedSetting(config_.vlmApiKey, "OPENZOOM_VLM_API_KEY");
    const QString model = ResolvedSetting(config_.vlmModel, "OPENZOOM_VLM_MODEL");
    QString prompt = readText ? readingPrompt : ResolvedSetting(config_.vlmPrompt, "OPENZOOM_VLM_PROMPT");
    if (prompt.isEmpty()) {
        prompt = QStringLiteral("Describe the visible scene briefly for a low-vision user. Focus on readable text, UI elements, and major objects.");
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
    EmitPrivacyNoticeIfChanged(
        QStringLiteral("Sending the current camera frame and prompt to %1 "
                       "(%2 endpoint). Conversation: temporary. Coding tools: unavailable.")
            .arg(endpointName,
                 IsLocalEndpoint(endpoint)
                     ? QStringLiteral("local")
                     : QStringLiteral("remote")));

    vlmPreparationPending_ = true;
    vlmPreparationPersistent_ = false;
    vlmPreparationThreadId_.clear();
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
                                        readText ? 4096 : 180);
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
                QStringLiteral("VLM response exceeded OpenZoom's safe size limit."));
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
                                     bool persistent)
{
    if (!codexClient_ || vlmPreparationPending_) {
        FinishVlmError(CodexNotAvailableMessage());
        return;
    }
    EmitPrivacyNoticeIfChanged(
        QStringLiteral("Sending the current camera frame and prompt to Codex. "
                       "Conversation: temporary. Tool internet: blocked. "
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
                        prompt, imagePath, threadId, persistent);
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
    const QString fullText = SanitizeText(text);
    vlmText_ = readingText_ ? fullText.left(kMaximumDisplayedAssistantCharacters)
                            : TruncateText(fullText, 700);
    bool noted = false;
    if (vlmText_.isEmpty()) {
        vlmStatus_ = QStringLiteral("VLM returned an empty description.");
    } else {
        vlmStatus_.clear();
        noted = AppendNoteSection(
            QCoreApplication::translate("OpenZoom", readingText_ ? "Text on screen" : "Scene explanation"),
            fullText, analyzedImage, true);
    }
    if (!noted && !analyzedImage.isEmpty()) {
        RemoveNoteImage(analyzedImage);
    }
    RefreshOverlay();
}

void AssistiveRuntime::FinishVlmError(const QString& errorText)
{
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
                                        "OpenZoom", "View the assistant analyzed")
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
                    ? QCoreApplication::translate("OpenZoom", "Assistant")
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
        QCoreApplication::translate("OpenZoom", "OpenZoom Lecture Notes");
    const QString startedLine =
        QCoreApplication::translate("OpenZoom", "Started %1")
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
    QString document = QStringLiteral(R"HTML(<!doctype html>
<html lang="@@LANG@@" data-l10n-transcript="@@TRLABEL@@" data-l10n-conversation="@@CONVOLABEL@@">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>@@TITLE@@ - @@TIME@@</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f4f2ee; --panel: #ffffff; --panel-2: #faf9f6; --ink: #1a1b1d; --muted: #5b6470;
      --line: #e3ded4; --line-soft: #ece8e0; --accent: #2563eb; --accent-soft: #e8f0fe;
      --chip: #efece5; --user-bubble: #eef2fb; --radius: 12px;
      --shadow: 0 1px 2px rgba(20, 18, 12, .06), 0 1px 8px rgba(20, 18, 12, .04);
    }
    :root[data-theme="dark"] {
      color-scheme: dark;
      --bg: #0e1013; --panel: #181b1f; --panel-2: #1e2226; --ink: #e9ebee; --muted: #9aa2ab;
      --line: #2a2f36; --line-soft: #23272d; --accent: #7aa7ff; --accent-soft: #1b2740;
      --chip: #23282e; --user-bubble: #1a2237;
      --shadow: 0 1px 2px rgba(0, 0, 0, .4), 0 2px 12px rgba(0, 0, 0, .35);
    }
    @media (prefers-color-scheme: dark) {
      :root:not([data-theme="light"]) {
        color-scheme: dark;
        --bg: #0e1013; --panel: #181b1f; --panel-2: #1e2226; --ink: #e9ebee; --muted: #9aa2ab;
        --line: #2a2f36; --line-soft: #23272d; --accent: #7aa7ff; --accent-soft: #1b2740;
        --chip: #23282e; --user-bubble: #1a2237;
        --shadow: 0 1px 2px rgba(0, 0, 0, .4), 0 2px 12px rgba(0, 0, 0, .35);
      }
    }
    * { box-sizing: border-box; }
    html { font: 16px/1.6 system-ui, "Segoe UI", sans-serif; }
    body { margin: 0; background: var(--bg); color: var(--ink); }
    .topbar { position: sticky; top: 0; z-index: 10; background: color-mix(in srgb, var(--bg) 88%, transparent); backdrop-filter: blur(8px); border-bottom: 1px solid var(--line); }
    .topbar-inner { width: min(52rem, calc(100vw - 2rem)); margin: 0 auto; display: flex; align-items: center; justify-content: space-between; gap: 1rem; padding: .5rem 0; flex-wrap: wrap; }
    .brand { margin: 0; font-weight: 700; letter-spacing: .01em; font-size: 1rem; display: flex; align-items: center; gap: .5rem; }
    .brand::before { content: ""; width: .7rem; height: .7rem; border-radius: 3px; background: var(--accent); }
    .tools { display: flex; gap: .4rem; flex-wrap: wrap; }
    .tools button { font: inherit; font-size: .82rem; color: var(--ink); background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: .35rem .8rem; cursor: pointer; }
    .tools button:hover { border-color: var(--accent); color: var(--accent); }
    main { width: min(52rem, calc(100vw - 2rem)); margin: 0 auto; padding: 1.75rem 0 5rem; }
    h1 { font-size: clamp(1.5rem, 4vw, 2rem); line-height: 1.15; margin: 0; }
    .created { color: var(--muted); margin: .35rem 0 1.75rem; font-size: .9rem; }
    /* Every entry is a compact chat-like card. */
    details.note { background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); margin: .55rem 0; box-shadow: var(--shadow); overflow: hidden; }
    summary { cursor: pointer; padding: .55rem .85rem; list-style: none; display: flex; align-items: center; gap: .5rem; }
    summary::-webkit-details-marker { display: none; }
    summary::before { content: "\25B8"; flex: none; color: var(--muted); font-size: .72rem; transition: transform .15s ease; }
    details[open] > summary::before { transform: rotate(90deg); }
    /* Type icon shows the entry kind at a glance while collapsed. */
    .ic { flex: none; font-size: 1rem; line-height: 1; }
    .ic::before { content: "\2022 "; color: var(--muted); }
    [data-kind="ai"]    > summary .ic::before { content: "\1F916 "; }
    [data-kind="video"] > summary .ic::before { content: "\1F3AC "; }
    [data-kind="photo"] > summary .ic::before { content: "\1F4F7 "; }
    [data-kind="draw"]  > summary .ic::before { content: "\270F "; }
    [data-kind="talk"]  > summary .ic::before { content: "\1F4AC "; }
    [data-kind="convo"] > summary .ic::before { content: "\1F5E8 "; }
    /* A multi-turn conversation wraps its turn cards; nested cards are flush. */
    details.convo > .note-body { padding: .4rem .6rem .6rem; }
    details.convo details.note { margin: .4rem 0; box-shadow: none; }
    details.convo details.note:first-child { margin-top: 0; }
    details.convo details.note:last-child { margin-bottom: 0; }
    h2.ttl { flex: none; font-size: .92rem; line-height: 1.3; margin: 0; font-weight: 600; color: var(--ink); }
    .count { flex: none; background: var(--chip); color: var(--muted); border-radius: 999px; padding: 0 .5rem; font-size: .74rem; font-weight: 500; }
    /* One-line preview fills the middle; hidden (but space kept) when open. */
    .preview { flex: 1 1 auto; min-width: 0; color: var(--muted); font-size: .85rem; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    details[open] > summary .preview { visibility: hidden; }
    /* Timestamp: least prominent, pinned to the far right. */
    time.at { flex: none; color: var(--muted); font-size: .76rem; font-variant-numeric: tabular-nums; }
    .note-body { padding: .1rem 1rem .85rem 2.35rem; }
    time { font-variant-numeric: tabular-nums; }
    .note-text { white-space: pre-wrap; overflow-wrap: anywhere; margin: 0; line-height: 1.55; }
    /* The question you asked the assistant, above the analyzed frame + reply. */
    .note-ask { margin: 0 0 .55rem; padding: .45rem .7rem; background: var(--user-bubble); border-radius: 9px; font-weight: 550; overflow-wrap: anywhere; }
    details.ai .note-body { border-left: 2px solid var(--accent-soft); padding-left: .9rem; margin-left: 1.4rem; }
    /* The exact frame the assistant analyzed, above its answer. */
    figure.ai-shot { margin: 0 0 .6rem; max-width: 22rem; }
    figure.ai-shot img { border-radius: 8px; }
    figure { margin: 0; }
    .media-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 18rem), 1fr)); gap: .7rem; }
    img, video { display: block; width: 100%; height: auto; border: 1px solid var(--line); border-radius: 8px; background: #000; }
    figcaption { margin-top: .35rem; color: var(--muted); font-size: .84rem; }
    figcaption a, a { color: var(--accent); }
    /* Single consolidated transcript block: tight chat rows, session breaks. */
    .trbody { padding: .3rem .5rem .5rem; }
    p.tr { display: flex; justify-content: space-between; align-items: baseline; gap: .8rem; margin: 0; padding: .32rem .65rem; }
    .trbody p.tr { border-radius: 9px; }
    .trbody p.tr:nth-child(odd) { background: var(--panel-2); }
    .trbody p.tr.tr-newsession { margin-top: .5rem; padding-top: .5rem; border-top: 1px dashed var(--line); }
    /* Standalone transcript lines (no script) still read as a card. */
    main > p.tr { background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); box-shadow: var(--shadow); margin: .3rem 0; padding: .5rem .85rem; }
    .tr-text { overflow-wrap: anywhere; }
    .tr-at { flex: none; color: var(--muted); font-size: .76rem; white-space: nowrap; font-variant-numeric: tabular-nums; }
    p.tr.tr-gap { color: var(--muted); font-style: italic; }
    p.tr.tr-gap .tr-text::before { content: "\2504 "; }
    button:focus-visible, summary:focus-visible, a:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; border-radius: 6px; }
    @media (prefers-reduced-motion: reduce) { summary::before { transition: none; } }
    @media print { .topbar { display: none; } main { width: 100%; } details.note { break-inside: avoid; box-shadow: none; } details:not([open]) > summary ~ * { display: revert; } }
  </style>
  <script>
  (function () {
    "use strict";
    var storageKey = "openzoom-notes-theme";
    function applyTheme(value) {
      if (value === "light" || value === "dark") {
        document.documentElement.setAttribute("data-theme", value);
      }
    }
    try { applyTheme(localStorage.getItem(storageKey)); } catch (e) { /* blocked storage */ }
    function isDark() {
      var explicit = document.documentElement.getAttribute("data-theme");
      if (explicit) { return explicit === "dark"; }
      return !!(window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches);
    }
    document.addEventListener("DOMContentLoaded", function () {
      var themeButton = document.getElementById("btn-theme");
      if (themeButton) {
        var sync = function () {
          themeButton.setAttribute("aria-pressed", isDark() ? "true" : "false");
        };
        sync();
        themeButton.addEventListener("click", function () {
          var next = isDark() ? "light" : "dark";
          applyTheme(next);
          try { localStorage.setItem(storageKey, next); } catch (e) { /* blocked */ }
          sync();
        });
      }
      var setAll = function (open) {
        var all = document.querySelectorAll("details");
        for (var i = 0; i < all.length; i++) { all[i].open = open; }
      };
      var expand = document.getElementById("btn-expand");
      var collapse = document.getElementById("btn-collapse");
      if (expand) { expand.addEventListener("click", function () { setAll(true); }); }
      if (collapse) { collapse.addEventListener("click", function () { setAll(false); }); }
      // Consolidate EVERY transcript line in the document into one single
      // collapsible block, regardless of how many times recording was
      // started/stopped or what media sits between them. The block is placed
      // right after the last recorded-video card (the transcript belongs to
      // the recording, so it reads after it); with no video it stays at the
      // first transcript line. A dashed rule marks where one recording ends
      // and the next begins. Progressive enhancement: without script each
      // line still renders as a readable card.
      var label = document.documentElement.getAttribute("data-l10n-transcript") || "Transcript";
      var main = document.querySelector("main");
      if (!main) { return; }
      var lines = Array.prototype.slice.call(main.children).filter(function (node) {
        return node.nodeType === 1 && node.matches("p.tr");
      });
      if (lines.length > 0) {
      var first = lines[0];
      var wrap = document.createElement("details");
      wrap.open = true;
      wrap.className = "note trwrap";
      wrap.setAttribute("data-kind", "talk");
      var summary = document.createElement("summary");
      var ic = document.createElement("span");
      ic.className = "ic";
      ic.setAttribute("aria-hidden", "true");
      summary.appendChild(ic);
      var heading = document.createElement("h2");
      heading.className = "ttl";
      heading.textContent = label;
      summary.appendChild(heading);
      var count = document.createElement("span");
      count.className = "count";
      count.textContent = String(lines.length);
      summary.appendChild(count);
      var preview = document.createElement("span");
      preview.className = "preview";
      var firstText = first.querySelector(".tr-text");
      preview.textContent = firstText ? firstText.textContent : "";
      summary.appendChild(preview);
      var atTime = document.createElement("time");
      atTime.className = "at";
      var firstChip = first.querySelector(".tr-at");
      if (firstChip) { atTime.textContent = firstChip.textContent; }
      summary.appendChild(atTime);
      wrap.appendChild(summary);
      var body = document.createElement("div");
      body.className = "trbody";
      wrap.appendChild(body);
      var videos = main.querySelectorAll(":scope > details[data-kind='video']");
      if (videos.length) {
        var lastVideo = videos[videos.length - 1];
        if (lastVideo.nextSibling) {
          main.insertBefore(wrap, lastVideo.nextSibling);
        } else {
          main.appendChild(wrap);
        }
      } else {
        main.insertBefore(wrap, first);
      }
      var prevSession = null;
      for (var i = 0; i < lines.length; i++) {
        var line = lines[i];
        var session = line.getAttribute("data-recording-session");
        if (prevSession !== null && session !== prevSession) {
          line.classList.add("tr-newsession");
        }
        prevSession = session;
        body.appendChild(line);
      }
      } // end transcript consolidation

      // Group Advanced Assistant turns of one conversation into a single
      // collapsible "Conversation" block, each turn still individually
      // collapsible inside it. A single-turn conversation stays a plain card.
      var convoLabel =
        document.documentElement.getAttribute("data-l10n-conversation") ||
        "Conversation";
      var order = [];
      var byConversation = {};
      var turns = main.querySelectorAll(":scope > details[data-conversation]");
      for (var t = 0; t < turns.length; t++) {
        var id = turns[t].getAttribute("data-conversation");
        if (!byConversation[id]) { byConversation[id] = []; order.push(id); }
        byConversation[id].push(turns[t]);
      }
      for (var c = 0; c < order.length; c++) {
        var group = byConversation[order[c]];
        if (group.length < 2) { continue; }
        var cWrap = document.createElement("details");
        cWrap.open = true;
        cWrap.className = "note convo";
        cWrap.setAttribute("data-kind", "convo");
        var cSummary = document.createElement("summary");
        var cIc = document.createElement("span");
        cIc.className = "ic";
        cIc.setAttribute("aria-hidden", "true");
        cSummary.appendChild(cIc);
        var cH2 = document.createElement("h2");
        cH2.className = "ttl";
        cH2.textContent = convoLabel;
        cSummary.appendChild(cH2);
        var cCount = document.createElement("span");
        cCount.className = "count";
        cCount.textContent = String(group.length);
        cSummary.appendChild(cCount);
        var cPrev = document.createElement("span");
        cPrev.className = "preview";
        cPrev.textContent = group[0].querySelector("h2.ttl")
          ? group[0].querySelector("h2.ttl").textContent : "";
        cSummary.appendChild(cPrev);
        cWrap.appendChild(cSummary);
        var cBody = document.createElement("div");
        cBody.className = "note-body";
        cWrap.appendChild(cBody);
        main.insertBefore(cWrap, group[0]);
        for (var g = 0; g < group.length; g++) { cBody.appendChild(group[g]); }
      }
    });
  })();
  </script>
</head>
<body>
<header class="topbar">
  <div class="topbar-inner">
    <p class="brand">OpenZoom</p>
    <div class="tools">
      <button type="button" id="btn-expand">@@EXPAND@@</button>
      <button type="button" id="btn-collapse">@@COLLAPSE@@</button>
      <button type="button" id="btn-theme" aria-pressed="false">&#9681; @@THEMELABEL@@</button>
    </div>
  </div>
</header>
<main>
  <h1>@@TITLE@@</h1>
  <p class="created">@@CREATED@@</p>
)HTML");
    document.replace(QStringLiteral("@@LANG@@"),
                     documentLanguage.toHtmlEscaped());
    document.replace(QStringLiteral("@@TRLABEL@@"),
                     QCoreApplication::translate("OpenZoom", "Lecture transcript")
                         .toHtmlEscaped());
    document.replace(QStringLiteral("@@CONVOLABEL@@"),
                     QCoreApplication::translate("OpenZoom", "Conversation")
                         .toHtmlEscaped());
    document.replace(QStringLiteral("@@TITLE@@"), notesTitle.toHtmlEscaped());
    document.replace(QStringLiteral("@@TIME@@"), displayTime.toHtmlEscaped());
    document.replace(QStringLiteral("@@CREATED@@"), startedLine);
    document.replace(QStringLiteral("@@EXPAND@@"),
                     QCoreApplication::translate("OpenZoom", "Expand all")
                         .toHtmlEscaped());
    document.replace(QStringLiteral("@@COLLAPSE@@"),
                     QCoreApplication::translate("OpenZoom", "Collapse all")
                         .toHtmlEscaped());
    document.replace(QStringLiteral("@@THEMELABEL@@"),
                     QCoreApplication::translate("OpenZoom", "Light or dark colors")
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
                                    "OpenZoom", "View the assistant analyzed")
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
                               "OpenZoom", "Captured processed camera view")
                               .toHtmlEscaped(),
                           QCoreApplication::translate(
                               "OpenZoom", "Processed camera view")
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
                                     "OpenZoom", "Open original video")
                                     .toHtmlEscaped());
        processedMedia = QStringLiteral(
                             "        <video controls preload=\"metadata\">\n"
                             "          <source src=\"%1\" type=\"video/mp4\">\n"
                             "          <a href=\"%1\">%2</a>\n"
                             "        </video>\n")
                             .arg(processedUrl,
                                  QCoreApplication::translate(
                                      "OpenZoom", "Open processed video")
                                      .toHtmlEscaped());
    } else {
        originalMedia = QStringLiteral(
                            "        <a href=\"%1\"><img src=\"%1\" alt=\"%2\" loading=\"lazy\"></a>\n")
                            .arg(originalUrl,
                                 QCoreApplication::translate(
                                     "OpenZoom", "Original camera view")
                                     .toHtmlEscaped());
        processedMedia = QStringLiteral(
                             "        <a href=\"%1\"><img src=\"%1\" alt=\"%2\" loading=\"lazy\"></a>\n")
                             .arg(processedUrl,
                                  QCoreApplication::translate(
                                      "OpenZoom", "Processed camera view")
                                      .toHtmlEscaped());
    }

    // Full sentences per language — never compose "Original camera" + type,
    // word order differs in Turkish and German.
    const QString originalCaption =
        (video ? QCoreApplication::translate("OpenZoom", "Original camera video")
               : QCoreApplication::translate("OpenZoom", "Original camera photo"))
            .toHtmlEscaped();
    const QString processedCaption =
        (video ? QCoreApplication::translate("OpenZoom", "Processed camera video")
               : QCoreApplication::translate("OpenZoom", "Processed camera photo"))
            .toHtmlEscaped();
    const QString content =
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
    NoteSectionMetadata metadata;
    metadata.cssClass = QStringLiteral("media");
    metadata.kind = video ? QStringLiteral("video") : QStringLiteral("photo");
    metadata.previewText =
        video ? QCoreApplication::translate("OpenZoom",
                                            "Original and processed video")
              : QCoreApplication::translate("OpenZoom",
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
                            receiver->acceptedTranscriptIdentities_.remove(identity);
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
#if OPENZOOM_HAS_TTS
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
#if OPENZOOM_HAS_TTS
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
                "OpenZoom",
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
#if OPENZOOM_HAS_TTS
    if (tts_) {
        tts_->stop();
    }
#endif
}

} // namespace openzoom

#endif // _WIN32
