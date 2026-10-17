#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QProcess>

#include <functional>
#include <memory>

#include "okuflow/common/codex_json_rpc_process.hpp"

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace okuflow {

// Native client for the local Codex app-server. Internet and coding permissions
// are explicit opt-ins and apply only to persistent Advanced Assistant turns;
// Read and Explain camera turns always retain the restricted vision policy.
class CodexAppServerClient : public QObject {
    Q_OBJECT
public:
    explicit CodexAppServerClient(QObject* parent = nullptr);
    ~CodexAppServerClient() override;

    void Configure(const QString& executablePath,
                   const QString& preferredModel,
                   const QString& reasoningEffort,
                   const QString& assistantInstructions,
                   bool internetEnabled,
                   bool codingEnabled,
                   const QString& workspaceDirectory);
    void Start();
    void Shutdown();
    // Per-request JSON-RPC reply deadline (default 60 s). Tests shorten it to
    // exercise initialization timeouts without waiting for the default.
    void SetRequestTimeoutMs(int timeoutMs);
    // Forces the same constrained offline profile used for all Read/Explain
    // turns, even when an ordinary persistent caller enables coding/internet.
    void SetRestrictedLaunchMode(bool restricted);

    bool IsReady() const;
    bool IsTurnActive() const;
    static QString BuiltInAssistantInstructions();
    // Shared Codex CLI discovery (configured path, OKUFLOW_CODEX_PATH, PATH,
    // then known install locations).
    static QString ResolveExecutablePath(const QString& configuredExecutable);

    void RefreshAccount();
    void StartChatGptLogin();
    // persistent selects an Advanced Assistant turn. A non-persistent turn
    // with retainThread is an Explain turn: its thread is stored (not
    // ephemeral) so later Explains can continue it, but it keeps the
    // restricted vision policy. If resuming its threadId fails while Codex is
    // running, a replacement thread is opened transparently and reported by
    // RetainedThreadStarted. Without retainThread a non-persistent turn runs
    // in a fresh ephemeral thread (Read).
    void RequestVisionTurn(const QString& prompt,
                           const QString& imagePath,
                           const QString& threadId,
                           bool persistent,
                           bool retainThread = false,
                           bool permitCoding = true,
                           std::function<bool()> frameAuthorization = {});
    void InterruptTurn();
    void LoadConversation(const QString& threadId);
    void CancelConversationLoad();
    void RenameConversation(const QString& threadId, const QString& name);
    void DeleteConversation(const QString& threadId);

signals:
    void ServerStateChanged(bool ready, const QString& status);
    void AccountChanged(bool signedIn, const QString& label, const QString& planType);
    void ModelsChanged(const QStringList& modelIds, const QString& selectedModel);
    void ModelCatalogChanged(const QJsonArray& models, const QString& selectedModel);
    void RateLimitChanged(const QString& summary);
    void LoginUrlReady(const QUrl& url);

    void ConversationCreated(const QJsonObject& thread);
    // A retained (Explain) thread was opened for a new or replaced conversation.
    void RetainedThreadStarted(const QJsonObject& thread);
    void ConversationTranscriptLoaded(const QString& threadId, const QJsonArray& messages);
    // Only the current load generation may publish either terminal signal.
    void ConversationTranscriptLoadFailed(const QString& threadId, const QString& error);
    void ConversationRenamed(const QString& threadId, const QString& name);
    void ConversationDeleted(const QString& threadId);

    void TurnStarted(const QString& threadId, const QString& turnId, bool persistent);
    void TurnTextDelta(const QString& threadId, const QString& turnId, const QString& delta);
    void TurnFinished(const QString& threadId,
                      const QString& turnId,
                      const QString& text,
                      const QString& error,
                      bool interrupted,
                      bool persistent);

private:
    using ReplyHandler = CodexJsonRpcProcess::ReplyHandler;

    // A request owns one generation from admission to its single TurnFinished.
    // Reply handlers capture it, so a late reply for a finished request can
    // neither resurrect it nor consume a replacement request.
    struct PendingTurn {
        QString prompt;
        QString imagePath;
        QString threadId;
        quint64 generation{0};
        bool persistent{false};
        bool retainThread{false};
        bool valid{false};
        bool opening{false}; // thread/start or thread/resume already sent
        bool allowCoding{}, allowInternet{};
        QString workspace;
        std::function<bool()> frameAuthorization;
    };

    QString ResolveExecutable() const;
    QString AssistantWorkingDirectory(bool persistent) const;
    QString DeveloperInstructions(bool persistent) const;
    QJsonObject SandboxPolicy(bool persistent) const;
    void SendNotification(const QString& method, const QJsonObject& params = {});
    qint64 SendRequest(const QString& method,
                       const QJsonObject& params,
                       ReplyHandler handler = {});
    void HandleNotification(const QString& method, const QJsonObject& params);
    bool HandleServerRequest(const QJsonValue& id,
                             const QString& method,
                             const QJsonObject& params);
    void FinishInitialization(const QJsonObject& result,
                              const QJsonObject& error);
    bool PendingTurnCurrent(quint64 generation) const;
    bool NotificationTargetsActiveTurn(const QJsonObject& params) const;
    void RetireTurnId(const QString& turnId);
    void SubmitPendingTurn();
    void StartNewThreadForPendingTurn();
    void ResumeThreadForPendingTurn();
    void StartTurnOnThread(QString threadId);
    void FinishActiveTurn(const QString& text,
                          const QString& error,
                          bool interrupted);
    void RejectForbiddenAgentItem(const QJsonObject& item);
    bool EnsureLaunchPermissions(bool coding, bool internet);
    void StartRestrictedPreflight(int stage);
    void FailRestrictedPreflight(const QString& code);
    void TouchTurnWatchdog();
    void CheckTurnWatchdog();
    void RequestInterrupt(const QString& reason);
    QString AppendActiveText(const QString& delta);
    static QJsonArray TranscriptFromThread(const QJsonObject& thread);
    static QString FinalAgentText(const QJsonObject& turn);

    static constexpr int kTurnIdleTimeoutMs = 90000;
    static constexpr int kVisionTurnMaximumMs = 180000;
    static constexpr int kPersistentTurnMaximumMs = 1800000;
    static constexpr int kInterruptGraceMs = 5000;
    static constexpr qsizetype kMaximumAnswerCharacters = 256 * 1024;
    static constexpr qsizetype kMaximumTranscriptMessageCharacters = 32 * 1024;
    static constexpr qsizetype kMaximumTranscriptMessages = 200;
    static constexpr qsizetype kMaximumRetiredTurnIds = 16;

    std::unique_ptr<CodexJsonRpcProcess> rpc_;
    QTimer* turnWatchdogTimer_{nullptr};
    QProcess restrictedPreflight_;
    QMetaObject::Connection restrictedStageFinished_;
    QTimer* restrictedPreflightDeadline_{};
    QByteArray restrictedPreflightBytes_;
    QString restrictedVersion_, restrictedFeatures_;
    quint64 restrictedPreflightEpoch_{};
    bool restrictedLaunch_{}, restrictedPreflightActive_{};
    // Immutable for each child. Idle/login children start with neither grant.
    bool launchCoding_{}, launchInternet_{};

    QString configuredExecutable_;
    QString preferredModel_;
    QString selectedModel_;
    QString appServerIdentity_;
    QString reasoningEffort_{QStringLiteral("low")};
    QString assistantInstructions_;
    QString workspaceDirectory_;
    bool initialized_{false};
    bool signedIn_{false};
    bool loginWhenReady_{false};
    bool internetEnabled_{false};
    bool codingEnabled_{false};
    bool retiringFailedServer_{false};
    bool accountReadInFlight_{false};

    quint64 conversationLoadGeneration_{0};
    quint64 lastRequestGeneration_{0};
    PendingTurn pendingTurn_;
    quint64 activeGeneration_{0};
    bool activeTurnStartPending_{false};
    // Finished, canceled, or orphaned turns whose late notifications must not
    // touch a newer turn (bounded FIFO).
    QStringList retiredTurnIds_;
    QString activeThreadId_;
    QString activeTurnId_;
    QString activeText_;
    QString activeImagePath_;
    bool activePersistent_{false};
    bool activeInternetEnabled_{false};
    bool activeCodingEnabled_{false};
    bool interruptingForbiddenItem_{false};
    bool activeTextTruncated_{false};
    bool interruptRequested_{false};
    qint64 turnStartedAtMs_{0};
    qint64 lastTurnActivityMs_{0};
    qint64 interruptDeadlineMs_{0};
    QString interruptReason_;
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
