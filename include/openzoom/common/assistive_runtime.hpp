#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>

#include <cstdint>
#include <memory>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QThreadPool;
class QTimer;
#if OPENZOOM_HAS_TTS
class QTextToSpeech;
#endif
QT_END_NAMESPACE

namespace openzoom {

class CodexAppServerClient;

// Runtime configuration for the assistive features. Each string field, when
// non-empty, takes precedence over the corresponding OPENZOOM_* environment
// variable; empty fields fall back to the environment variable.
struct AssistiveRuntimeConfig {
    QString aiProvider{QStringLiteral("codex")};
    QString codexExecutablePath;
    QString codexModel{QStringLiteral("gpt-5.6-tera")};
    QString codexReasoningEffort{QStringLiteral("low")};
    bool codexInternetEnabled{false};
    bool codexCodingEnabled{false};
    QString codexWorkspaceDirectory;
    QString assistantInstructions;
    QString vlmApiUrl;
    QString vlmApiKey;
    QString vlmModel;
    QString vlmPrompt;
    QString tesseractPath;
    QString ocrLanguage;      // e.g. "eng"; passed to tesseract as "-l <language>" when set
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
    QString responseLanguage() const { return responseLanguageCode_; }
    static QString AppendResponseLanguageDirective(
        const QString& prompt,
        const QString& languageCode);
    void SetModes(bool ocrEnabled, bool vlmEnabled);
    bool WantsAnalysis() const;
    bool IsBusy() const;
    bool IsCodexTurnActive() const;

    void SubmitFrame(const uint8_t* bgraData, int width, int height);
    // Runs the requested analyses immediately, regardless of the enabled
    // modes. Configuration availability and per-engine busy state are still
    // respected; a busy engine reports through the normal error/status path.
    void SubmitFrameForced(const uint8_t* bgraData, int width, int height, bool runOcr, bool runVlm);
    // Speaks result text only after an explicit user request.
    void ReadAloud(const QString& text);
    // Hides the current result without disabling future forced results.
    void DismissOverlay();
    void StartCodexLogin();
    void StopAssistant();
    void SubmitAssistantPrompt(const QString& prompt,
                               const QString& threadId,
                               const uint8_t* bgraData,
                               int width,
                               int height,
                               bool attachFrame);
    void LoadAssistantConversation(const QString& threadId);
    void RenameAssistantConversation(const QString& threadId, const QString& name);
    void DeleteAssistantConversation(const QString& threadId);

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
    // been written yet.
    QString notesFilePath() const;

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
    void AssistantConversationRenamed(const QString& threadId, const QString& name);
    void AssistantConversationDeleted(const QString& threadId);
    void AssistantTurnStarted(const QString& threadId, const QString& turnId, bool persistent);
    void AssistantTextDelta(const QString& threadId, const QString& turnId, const QString& delta);
    void AssistantTurnFinished(const QString& threadId,
                               const QString& turnId,
                               const QString& text,
                               const QString& error,
                               bool interrupted,
                               bool persistent);
    void StatusNotice(const QString& sourceText);

private:
    void RefreshOverlay();
    void StartOcr(const uint8_t* bgraData, int width, int height, bool forced);
    void StartVlm(const uint8_t* bgraData, int width, int height);
    void StartCodexVlm(const uint8_t* bgraData,
                       int width,
                       int height,
                       const QString& prompt,
                       const QString& threadId,
                       bool persistent);
    void PostVlmRequest(const QByteArray& requestBody,
                        const QString& apiUrl,
                        const QString& apiKey);
    void FinishOcrSuccess(const QString& text);
    void FinishOcrError(const QString& errorText);
    void FinishVlmSuccess(const QString& text);
    void FinishVlmError(const QString& errorText);
    QString TesseractProgram() const;
    bool VlmConfigured() const;
    bool UsesCodexProvider() const;
    bool ValidateFrame(const uint8_t* bgraData, int width, int height);
    bool EnsureNotesFile();
    void FinalizeNotesFile();
    void AppendNoteSection(const QString& heading,
                           const QString& bodyText,
                           const QString& imagePath = {});
    void AppendNoteMediaPair(const QString& heading,
                             const QString& originalPath,
                             const QString& processedPath,
                             bool video);
    void AppendNoteHtmlSection(const QString& heading,
                               const QString& contentHtml);
    void SpeakText(const QString& text);
    void StopSpeech();
    bool SelectVoiceForResponseLanguage(bool notifyMissing);
    void EmitPrivacyNoticeIfChanged(const QString& summary);

    AssistiveRuntimeConfig config_;
    QString responseLanguageCode_{QStringLiteral("en")};
    QString warnedMissingVoiceLanguage_;

    bool ocrEnabled_{false};
    bool vlmEnabled_{false};
    bool ocrHardUnavailable_{false};
    bool vlmHardUnavailable_{false};
    bool ocrForcedVisible_{false};
    bool vlmForcedVisible_{false};
    bool ocrRunForced_{false};
    bool ocrTimedOut_{false};
    bool ocrPreparationPending_{false};
    bool vlmPreparationPending_{false};
    bool vlmPreparationPersistent_{false};
    bool warnedDegenerateFrame_{false};
    bool overlayDismissed_{false};
    std::uint64_t ocrPreparationGeneration_{};
    std::uint64_t vlmPreparationGeneration_{};

    QString ocrText_;
    QString vlmText_;
    QString ocrStatus_;
    QString vlmStatus_;

    QString notesFilePath_;
    bool notesDocumentOpen_{false};
    QString lastNotedOcrText_;

    std::unique_ptr<QProcess> ocrProcess_;
    std::unique_ptr<QThreadPool> imagePreparationPool_;
    QTimer* ocrWatchdogTimer_{};
    QString pendingOcrImagePath_;
    QNetworkAccessManager* networkManager_{};
    QNetworkReply* activeReply_{};
    bool vlmResponseTooLarge_{false};
    QString vlmPreparationThreadId_;
    QString lastPrivacyNotice_;
    std::unique_ptr<CodexAppServerClient> codexClient_;
#if OPENZOOM_HAS_TTS
    QTextToSpeech* tts_{};
#endif
};

} // namespace openzoom

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
