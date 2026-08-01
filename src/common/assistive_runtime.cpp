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
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <limits>

#include <windows.h>

#if OPENZOOM_HAS_TTS
#include <QTextToSpeech>
#include <QVoice>
#endif

namespace openzoom {

namespace {

constexpr int kMinFrameEdge = 64;
constexpr int kMaxVlmFrameEdge = 2048;
constexpr int kOcrWatchdogMs = 10000;
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
        QStringLiteral(R"(^openzoom_(?:ocr|codex)_(\d+)_.*\.(?:png|jpg)$)"));
    const QDateTime legacyCutoff =
        QDateTime::currentDateTimeUtc().addSecs(-60 * 60);
    int removed = 0;
    for (const QString& pattern :
         {QStringLiteral("openzoom_ocr_*.png"),
          QStringLiteral("openzoom_codex_*.jpg")}) {
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
         {QStringLiteral("openzoom_ocr_%1_*.png").arg(processId),
          QStringLiteral("openzoom_codex_%1_*.jpg").arg(processId)}) {
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
                               const QString& responseLanguageCode)
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
        {QStringLiteral("max_tokens"), 180}};
    return QJsonDocument(requestBody).toJson(QJsonDocument::Compact);
}

} // namespace

AssistiveRuntime::AssistiveRuntime(QObject* parent)
    : QObject(parent)
{
    SweepAssistiveTemporaryFrames();
    imagePreparationPool_ = std::make_unique<QThreadPool>();
    imagePreparationPool_->setMaxThreadCount(2);
    imagePreparationPool_->setExpiryTimeout(30000);
    networkManager_ = new QNetworkAccessManager(this);
    ocrProcess_ = std::make_unique<QProcess>(this);
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
                    vlmText_.clear();
                    vlmStatus_ = error.trimmed().isEmpty()
                                     ? QStringLiteral("Assistant stopped.")
                                     : SanitizeText(error);
                    RefreshOverlay();
                } else if (!error.isEmpty()) {
                    FinishVlmError(error);
                } else {
                    FinishVlmSuccess(text);
                }
                emit AssistantTurnFinished(threadId, turnId, text, error, interrupted, persistent);
            });

    ocrWatchdogTimer_ = new QTimer(this);
    ocrWatchdogTimer_->setSingleShot(true);
    ocrWatchdogTimer_->setInterval(kOcrWatchdogMs);
    connect(ocrWatchdogTimer_, &QTimer::timeout, this, [this]() {
        if (!ocrProcess_ || ocrProcess_->state() == QProcess::NotRunning) {
            return;
        }
        ocrTimedOut_ = true;
        FinishOcrError(QStringLiteral("OCR timed out."));
        // The finished/errorOccurred handlers remove the temp image once the
        // process is gone and the file is no longer locked.
        ocrProcess_->kill();
    });

    connect(ocrProcess_.get(),
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                ocrWatchdogTimer_->stop();
                const QString stdoutText = SanitizeText(QString::fromUtf8(ocrProcess_->readAllStandardOutput()));
                const QString stderrText = SanitizeText(QString::fromUtf8(ocrProcess_->readAllStandardError()));
                if (!pendingOcrImagePath_.isEmpty()) {
                    QFile::remove(pendingOcrImagePath_);
                    pendingOcrImagePath_.clear();
                }
                if (ocrTimedOut_) {
                    // The watchdog already reported the timeout.
                    ocrTimedOut_ = false;
                    ocrRunForced_ = false;
                    return;
                }
                if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                    FinishOcrSuccess(stdoutText);
                    return;
                }
                ocrRunForced_ = false;
                QString errorText = stderrText;
                if (errorText.isEmpty()) {
                    errorText = QStringLiteral("tesseract exited with code %1").arg(exitCode);
                }
                FinishOcrError(errorText);
            });

    connect(ocrProcess_.get(),
            &QProcess::errorOccurred,
            this,
            [this](QProcess::ProcessError error) {
                ocrWatchdogTimer_->stop();
                if (!pendingOcrImagePath_.isEmpty()) {
                    QFile::remove(pendingOcrImagePath_);
                    pendingOcrImagePath_.clear();
                }
                if (ocrTimedOut_) {
                    // Kill after timeout surfaces as Crashed; already reported.
                    return;
                }
                ocrRunForced_ = false;
                QString errorText;
                switch (error) {
                case QProcess::FailedToStart:
                    errorText = QStringLiteral("tesseract not found. Install it or set its path in the "
                                               "assistive settings or via OPENZOOM_TESSERACT_PATH.");
                    break;
                case QProcess::Crashed:
                    errorText = QStringLiteral("tesseract crashed during OCR.");
                    break;
                default:
                    errorText = QStringLiteral("tesseract OCR process failed.");
                    break;
                }
                FinishOcrError(errorText);
            });

    RefreshOverlay();
}

AssistiveRuntime::~AssistiveRuntime()
{
    ++ocrPreparationGeneration_;
    ++vlmPreparationGeneration_;
    ocrPreparationPending_ = false;
    vlmPreparationPending_ = false;
    if (imagePreparationPool_) {
        imagePreparationPool_->clear();
        imagePreparationPool_->waitForDone();
    }
    if (activeReply_) {
        activeReply_->abort();
    }
    if (ocrProcess_ && ocrProcess_->state() != QProcess::NotRunning) {
        ocrProcess_->kill();
        if (!ocrProcess_->waitForFinished(250)) {
            qWarning() << "Tesseract did not exit within the bounded shutdown "
                          "window";
        }
    }
    if (!pendingOcrImagePath_.isEmpty()) {
        QFile::remove(pendingOcrImagePath_);
    }
    RemoveCurrentProcessAssistiveFrames();
    FinalizeNotesFile();
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

    // New credentials or a new tesseract path may fix a previous hard failure.
    ocrHardUnavailable_ = false;
    vlmHardUnavailable_ = false;

    if (notesTargetChanged) {
        FinalizeNotesFile();
        notesFilePath_.clear();
        lastNotedOcrText_.clear();
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

void AssistiveRuntime::SetModes(bool ocrEnabled, bool vlmEnabled)
{
    const bool ocrTurningOff = ocrEnabled_ && !ocrEnabled;
    const bool vlmTurningOff = vlmEnabled_ && !vlmEnabled;

    if (ocrTurningOff && ocrProcess_ && ocrProcess_->state() != QProcess::NotRunning) {
        ocrWatchdogTimer_->stop();
        ocrProcess_->kill();
        ocrProcess_->waitForFinished(250);
    }
    if (ocrTurningOff && ocrPreparationPending_) {
        ++ocrPreparationGeneration_;
        ocrPreparationPending_ = false;
    }
    if (vlmTurningOff && activeReply_) {
        activeReply_->abort();
    }
    if (vlmTurningOff && vlmPreparationPending_) {
        ++vlmPreparationGeneration_;
        vlmPreparationPending_ = false;
        vlmPreparationPersistent_ = false;
        vlmPreparationThreadId_.clear();
    }

    ocrEnabled_ = ocrEnabled;
    vlmEnabled_ = vlmEnabled;
    if (ocrEnabled_) {
        ocrHardUnavailable_ = false;
    }
    if (vlmEnabled_) {
        vlmHardUnavailable_ = false;
    }
    if (ocrTurningOff) {
        ocrForcedVisible_ = false;
        ocrText_.clear();
        ocrStatus_.clear();
    } else if (ocrEnabled_ && ocrText_.isEmpty() && ocrStatus_.isEmpty()) {
        ocrStatus_ = QStringLiteral("OCR ready. Install tesseract or set its path in the assistive "
                                    "settings or via OPENZOOM_TESSERACT_PATH if detection fails.");
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
    return (ocrEnabled_ && !ocrHardUnavailable_) || automaticVlm;
}

bool AssistiveRuntime::IsBusy() const
{
    const bool ocrBusy = ocrProcess_ && ocrProcess_->state() != QProcess::NotRunning;
    return ocrPreparationPending_ || vlmPreparationPending_ ||
           ocrBusy || activeReply_ != nullptr ||
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

    if (ocrEnabled_ && !ocrPreparationPending_ &&
        ocrProcess_ && ocrProcess_->state() == QProcess::NotRunning) {
        StartOcr(bgraData, width, height, false);
    }
    if (vlmEnabled_ && !UsesCodexProvider() &&
        !vlmPreparationPending_ && activeReply_ == nullptr) {
        StartVlm(bgraData, width, height);
    }
}

void AssistiveRuntime::SubmitFrameForced(const uint8_t* bgraData, int width, int height, bool runOcr, bool runVlm)
{
    if ((!runOcr && !runVlm) || !ValidateFrame(bgraData, width, height)) {
        return;
    }
    overlayDismissed_ = false;

    if (runOcr && !runVlm && !vlmEnabled_) {
        vlmForcedVisible_ = false;
        vlmText_.clear();
        vlmStatus_.clear();
    } else if (runVlm && !runOcr && !ocrEnabled_) {
        ocrForcedVisible_ = false;
        ocrText_.clear();
        ocrStatus_.clear();
    }

    if (runOcr) {
        ocrForcedVisible_ = true;
        if (ocrPreparationPending_ ||
            (ocrProcess_ && ocrProcess_->state() != QProcess::NotRunning)) {
            FinishOcrError(QStringLiteral("OCR is busy with a previous capture. Try again in a moment."));
        } else {
            StartOcr(bgraData, width, height, true);
        }
    }

    if (runVlm) {
        vlmForcedVisible_ = true;
        if (vlmPreparationPending_ || activeReply_ != nullptr ||
            (codexClient_ && codexClient_->IsTurnActive())) {
            FinishVlmError(QStringLiteral("Scene explanation is busy with a previous request. Try again in a moment."));
        } else if (!VlmConfigured()) {
            FinishVlmError(VlmNotConfiguredMessage());
        } else {
            vlmHardUnavailable_ = false;
            StartVlm(bgraData, width, height);
        }
    }
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
    vlmText_.clear();
    vlmStatus_ = attachFrame
                     ? QStringLiteral("Preparing the current view...")
                     : QStringLiteral("Thinking...");
    RefreshOverlay();

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
    const QString imagePath =
        CreateAssistiveTemporaryFramePath(QStringLiteral("codex"),
                                          QStringLiteral("jpg"));
    if (copy.isNull() || imagePath.isEmpty()) {
        QFile::remove(imagePath);
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
                        return;
                    }
                    owner->vlmPreparationPending_ = false;
                    owner->vlmPreparationPersistent_ = false;
                    owner->vlmPreparationThreadId_.clear();
                    if (!saved) {
                        const QString error =
                            QStringLiteral(
                                "Could not prepare the current camera view.");
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
    if (ocrEnabled_ || ocrForcedVisible_) {
        QString text = ocrText_.isEmpty() ? ocrStatus_ : ocrText_;
        if (!text.isEmpty()) {
            sections.push_back(QStringLiteral("OCR\n%1").arg(text));
        }
    }
    if (vlmEnabled_ || vlmForcedVisible_) {
        QString text = vlmText_.isEmpty() ? vlmStatus_ : vlmText_;
        if (!text.isEmpty()) {
            sections.push_back(QStringLiteral("Scene Explain\n%1").arg(text));
        }
    }

    const QString body = sections.join(QStringLiteral("\n\n"));
    emit OverlayUpdated(QStringLiteral("Assistive View"),
                        body,
                        !overlayDismissed_ && !body.isEmpty());
}

QString AssistiveRuntime::TesseractProgram() const
{
    const QString resolved = ResolvedSetting(config_.tesseractPath, "OPENZOOM_TESSERACT_PATH");
    if (!resolved.isEmpty()) {
        const QFileInfo configured(resolved);
        if (configured.isDir()) {
            return QDir(configured.absoluteFilePath()).filePath(QStringLiteral("tesseract.exe"));
        }
        return resolved;
    }

    const QString appDirectory = QCoreApplication::applicationDirPath();
    QStringList candidates{
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
            .filePath(QStringLiteral("OpenZoom/tools/tesseract/tesseract.exe")),
        QDir(appDirectory).filePath(QStringLiteral("tools/tesseract/tesseract.exe")),
        QDir(appDirectory).filePath(QStringLiteral("tesseract/tesseract.exe")),
        QDir(appDirectory).filePath(QStringLiteral("tesseract.exe"))};

    const QString pathExecutable = QStandardPaths::findExecutable(QStringLiteral("tesseract.exe"));
    if (!pathExecutable.isEmpty()) {
        candidates.push_back(pathExecutable);
    }

    const QString programFiles = qEnvironmentVariable("ProgramFiles").trimmed();
    if (!programFiles.isEmpty()) {
        candidates.push_back(QDir(programFiles).filePath(QStringLiteral("Tesseract-OCR/tesseract.exe")));
    }
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA").trimmed();
    if (!localAppData.isEmpty()) {
        candidates.push_back(QDir(localAppData).filePath(
            QStringLiteral("Programs/Tesseract-OCR/tesseract.exe")));
    }

    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile()) {
            return info.absoluteFilePath();
        }
    }
    return QStringLiteral("tesseract");
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

void AssistiveRuntime::StartOcr(const uint8_t* bgraData, int width, int height, bool forced)
{
    if ((ocrHardUnavailable_ && !forced) || ocrPreparationPending_) {
        return;
    }
    QImage frameImage(bgraData, width, height, width * 4, QImage::Format_ARGB32);
    QImage copy = frameImage.copy();
    const QString imagePath =
        CreateAssistiveTemporaryFramePath(QStringLiteral("ocr"),
                                          QStringLiteral("png"));
    if (copy.isNull() || imagePath.isEmpty()) {
        QFile::remove(imagePath);
        FinishOcrError(QStringLiteral("Failed to create temporary image for OCR."));
        return;
    }

    ocrRunForced_ = forced;
    ocrTimedOut_ = false;
    ocrPreparationPending_ = true;
    ocrStatus_ = QStringLiteral("Preparing OCR...");
    RefreshOverlay();
    const QString language = config_.ocrLanguage.trimmed();
    const QString program = TesseractProgram();
    const std::uint64_t generation = ++ocrPreparationGeneration_;
    QPointer<AssistiveRuntime> owner(this);
    const bool queued = imagePreparationPool_ &&
        imagePreparationPool_->tryStart(
        [owner,
         copy = std::move(copy),
         imagePath,
         language,
         program,
         generation]() mutable {
            const bool saved = copy.save(imagePath, "PNG");
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
                 language,
                 program,
                 generation]() {
                    if (!owner ||
                        generation != owner->ocrPreparationGeneration_ ||
                        !owner->ocrPreparationPending_) {
                        QFile::remove(imagePath);
                        return;
                    }
                    owner->ocrPreparationPending_ = false;
                    if (!saved) {
                        owner->ocrRunForced_ = false;
                        owner->FinishOcrError(
                            QStringLiteral(
                                "Failed to save OCR input image."));
                        return;
                    }

                    owner->pendingOcrImagePath_ = imagePath;
                    owner->ocrStatus_ = QStringLiteral("Running OCR...");
                    owner->RefreshOverlay();
                    QStringList arguments{
                        imagePath,
                        QStringLiteral("stdout"),
                        QStringLiteral("--psm"),
                        QStringLiteral("6")};
                    if (!language.isEmpty()) {
                        arguments << QStringLiteral("-l") << language;
                    }

                    owner->ocrProcess_->setProgram(program);
                    owner->ocrProcess_->setArguments(arguments);
                    QProcessEnvironment environment =
                        QProcessEnvironment::systemEnvironment();
                    const QFileInfo programInfo(program);
                    if (programInfo.isFile()) {
                        const QString tessdataPath =
                            QDir(programInfo.absolutePath())
                                .filePath(QStringLiteral("tessdata"));
                        if (QDir(tessdataPath).exists()) {
                            environment.insert(
                                QStringLiteral("TESSDATA_PREFIX"),
                                QDir::toNativeSeparators(tessdataPath));
                        }
                        owner->ocrProcess_->setWorkingDirectory(
                            programInfo.absolutePath());
                    } else {
                        owner->ocrProcess_->setWorkingDirectory(QString());
                    }
                    owner->ocrProcess_->setProcessEnvironment(environment);
                    owner->ocrProcess_->start();
                    owner->ocrWatchdogTimer_->start();
                },
                Qt::QueuedConnection);
        });
    if (!queued) {
        ocrPreparationPending_ = false;
        ocrRunForced_ = false;
        QFile::remove(imagePath);
        FinishOcrError(
            QStringLiteral(
                "OCR image preparation is busy. Try again in a moment."));
    }
}

void AssistiveRuntime::StartVlm(const uint8_t* bgraData, int width, int height)
{
    if (UsesCodexProvider()) {
        QString prompt = config_.vlmPrompt.trimmed();
        if (prompt.isEmpty()) {
            prompt = QStringLiteral("Describe the visible scene briefly for a low-vision user. "
                                    "Focus on readable text, controls, and major objects.");
        }
        StartCodexVlm(
            bgraData,
            width,
            height,
            AppendResponseLanguageDirective(prompt, responseLanguageCode_),
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
    QString prompt = ResolvedSetting(config_.vlmPrompt, "OPENZOOM_VLM_PROMPT");
    if (prompt.isEmpty()) {
        prompt = QStringLiteral("Describe the visible scene briefly for a low-vision user. Focus on readable text, UI elements, and major objects.");
    }
    const QString assistantInstructions =
        config_.assistantInstructions.trimmed();
    const QString responseLanguageCode = responseLanguageCode_;
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
                                        responseLanguageCode);
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

void AssistiveRuntime::FinishOcrSuccess(const QString& text)
{
    ocrRunForced_ = false;
    const QString fullText = SanitizeText(text);
    ocrText_ = TruncateText(fullText, 700);
    if (ocrText_.isEmpty()) {
        ocrStatus_ = QStringLiteral("OCR found no readable text.");
    } else {
        ocrStatus_.clear();
        if (fullText != lastNotedOcrText_) {
            lastNotedOcrText_ = fullText;
            AppendNoteSection(
                QCoreApplication::translate("OpenZoom", "Text on screen"),
                fullText);
        }
    }
    RefreshOverlay();
}

void AssistiveRuntime::FinishOcrError(const QString& errorText)
{
    ocrText_.clear();
    ocrStatus_ = SanitizeText(errorText);
    if (ocrStatus_.contains(QStringLiteral("not found"), Qt::CaseInsensitive)) {
        ocrHardUnavailable_ = true;
    }
    RefreshOverlay();
}

void AssistiveRuntime::FinishVlmSuccess(const QString& text)
{
    const QString fullText = SanitizeText(text);
    vlmText_ = TruncateText(fullText, 700);
    if (vlmText_.isEmpty()) {
        vlmStatus_ = QStringLiteral("VLM returned an empty description.");
    } else {
        vlmStatus_.clear();
        AppendNoteSection(
            QCoreApplication::translate("OpenZoom", "Scene explanation"),
            fullText);
    }
    RefreshOverlay();
}

void AssistiveRuntime::FinishVlmError(const QString& errorText)
{
    vlmText_.clear();
    vlmStatus_ = SanitizeText(errorText);
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
    if (!QDir().mkpath(directory)) {
        qWarning("AssistiveRuntime: failed to create notes directory %s", qPrintable(directory));
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    const QString fileName = QStringLiteral("NOTES_%1.html")
                                 .arg(now.toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QDir(directory).filePath(fileName);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning("AssistiveRuntime: failed to create notes file %s", qPrintable(path));
        return false;
    }
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
    const QString document = QStringLiteral(
        "<!doctype html>\n"
        "<html lang=\"%2\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  <title>%3 - %1</title>\n"
        "  <style>\n"
        "    :root { color-scheme: light dark; font: 18px/1.6 system-ui, sans-serif; }\n"
        "    body { margin: 0; background: Canvas; color: CanvasText; }\n"
        "    main { width: min(72rem, calc(100% - 2rem)); margin: 0 auto; padding: 2rem 0 4rem; }\n"
        "    h1 { font-size: clamp(1.8rem, 5vw, 3rem); line-height: 1.15; margin: 0; }\n"
        "    .created { color: GrayText; margin: .5rem 0 2rem; }\n"
        "    section { border-top: 2px solid GrayText; padding: 1.5rem 0; }\n"
        "    h2 { font-size: 1.25rem; line-height: 1.3; margin: 0 0 1rem; }\n"
        "    time { font-variant-numeric: tabular-nums; }\n"
        "    .note-text { white-space: pre-wrap; overflow-wrap: anywhere; }\n"
        "    figure { margin: 0; }\n"
        "    .media-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(100%, 24rem), 1fr)); gap: 1rem; }\n"
        "    img, video { display: block; width: 100%; height: auto; border: 2px solid GrayText; background: #000; }\n"
        "    figcaption { margin-top: .5rem; color: GrayText; }\n"
        "    a:focus-visible { outline: 4px solid Highlight; outline-offset: 4px; }\n"
        "    @media print { main { width: 100%; } section { break-inside: avoid; } }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "<main>\n"
        "  <h1>%3</h1>\n"
        "  <p class=\"created\">%4</p>\n")
                                 .arg(displayTime.toHtmlEscaped(),
                                      documentLanguage.toHtmlEscaped(),
                                      notesTitle.toHtmlEscaped(),
                                      startedLine);
    const QByteArray documentBytes = document.toUtf8();
    if (file.write(documentBytes) != documentBytes.size() || !file.commit()) {
        qWarning("AssistiveRuntime: failed to finalize notes file %s", qPrintable(path));
        return false;
    }

    notesFilePath_ = path;
    notesDocumentOpen_ = true;
    return true;
}

void AssistiveRuntime::FinalizeNotesFile()
{
    if (!notesDocumentOpen_ || notesFilePath_.isEmpty()) {
        return;
    }
    QFile file(notesFilePath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        qWarning("AssistiveRuntime: failed to finalize notes file %s",
                 qPrintable(notesFilePath_));
        return;
    }
    static constexpr auto closingDocument =
        "</main>\n"
        "</body>\n"
        "</html>\n";
    const QByteArray closingBytes(closingDocument);
    if (file.write(closingBytes) != closingBytes.size() || !file.flush()) {
        qWarning("AssistiveRuntime: failed to finalize notes file %s",
                 qPrintable(notesFilePath_));
        return;
    }
    notesDocumentOpen_ = false;
}

void AssistiveRuntime::AppendNoteSection(const QString& heading,
                                         const QString& bodyText,
                                         const QString& imagePath)
{
    if (!config_.lectureNotesEnabled || config_.notesDirectory.trimmed().isEmpty()) {
        return;
    }
    if (bodyText.trimmed().isEmpty() && imagePath.trimmed().isEmpty()) {
        return;
    }
    if (!EnsureNotesFile()) {
        return;
    }

    QString content;
    if (!imagePath.trimmed().isEmpty()) {
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
    } else {
        content = QStringLiteral("    <div class=\"note-text\">%1</div>\n")
                      .arg(bodyText.toHtmlEscaped());
    }
    AppendNoteHtmlSection(heading, content);
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
    AppendNoteHtmlSection(heading, content);
}

void AssistiveRuntime::AppendNoteHtmlSection(const QString& heading,
                                              const QString& contentHtml)
{
    if (contentHtml.trimmed().isEmpty() || notesFilePath_.isEmpty()) {
        return;
    }
    const QString timestamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    // Sections restate the language they were written in; the document-level
    // lang only reflects the language at file creation and the user can
    // switch mid-session.
    const QString sectionLanguage =
        responseLanguageCode_.isEmpty() ? QStringLiteral("en")
                                        : responseLanguageCode_;
    const QString section = QStringLiteral(
                                "  <section lang=\"%4\">\n"
                                "    <h2><time>[%1]</time> %2</h2>\n"
                                "%3"
                                "  </section>\n")
                                .arg(timestamp.toHtmlEscaped(),
                                     heading.toHtmlEscaped(),
                                     contentHtml,
                                     sectionLanguage.toHtmlEscaped());
    const QByteArray sectionBytes = section.toUtf8();
    QFile outputFile(notesFilePath_);
    if (!notesDocumentOpen_ ||
        !outputFile.open(QIODevice::WriteOnly | QIODevice::Append) ||
        outputFile.write(sectionBytes) != sectionBytes.size() ||
        !outputFile.flush()) {
        qWarning("AssistiveRuntime: failed to append notes file %s",
                 qPrintable(notesFilePath_));
    }
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
