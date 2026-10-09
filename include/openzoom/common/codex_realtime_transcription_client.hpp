#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QJsonObject>
#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <memory>

#include "openzoom/common/codex_json_rpc_process.hpp"
#include "openzoom/common/realtime_transcription_interfaces.hpp"

QT_BEGIN_NAMESPACE
class QTimer;
class QProcess;
class QTemporaryDir;
QT_END_NAMESPACE

namespace openzoom {

// Dedicated Codex app-server child for live transcription. Subscription-only:
// the child runs with API-key environment variables removed, requires a
// ChatGPT-signed-in account, disables every effective MCP server in a
// fail-closed preflight, and starts an ephemeral no-approval/read-only thread
// in a fresh empty working directory. Microphone text is explicitly marked
// as untrusted data, never as commands. The
// realtime session is carried over WebRTC (the only transport that accepts
// subscription auth — verified 2026-08-07, plan 36).
//
// Isolated from the Assistant's CodexAppServerClient on purpose: a realtime
// crash cannot cancel an explanation, and a long-lived realtime thread cannot
// hold IsTurnActive() true. Only role=="user" transcript events are exposed;
// assistant transcripts and outputAudio deltas are discarded undecoded.
//
// Every signal carries the generation passed to BeginSession, so a stale
// session can never update a newer recording's transcript.
class CodexRealtimeTranscriptionClient : public RealtimeTranscriptionService {
    Q_OBJECT
public:
    explicit CodexRealtimeTranscriptionClient(QObject* parent = nullptr);
    ~CodexRealtimeTranscriptionClient() override;

    void SetExecutablePath(const QString& configuredExecutable) override;

    bool IsSessionActive() const override;

    // Start process -> initialize (experimentalApi) -> account gate ->
    // ephemeral read-only thread. Emits SessionReady or SessionFailed.
    void BeginSession(quint64 generation) override;

    // Send thread/realtime/start with the WebRTC offer. AnswerSdp arrives via
    // notification; RealtimeStarted only after the started event. A start
    // reply is never treated as "connected".
    void StartRealtime(quint64 generation, const QString& offerSdp) override;

    // Request a graceful realtime stop; matching transcript finals are still
    // accepted until RealtimeClosed or the bounded stop deadline.
    void StopRealtime(quint64 generation) override;

    // Bounded teardown of realtime and the child process. Safe to repeat;
    // never blocks beyond the transport's bounded shutdown.
    void EndSession(quint64 generation) override;

    void RefreshRateLimits() override;

private:
    enum class Phase {
        Idle,
        DiscoveringTools,
        StartingProcess,
        Initializing,
        CheckingAccount,
        StartingThread,
        SessionReady,
        RealtimeStarting,
        RealtimeActive,
        Stopping,
    };

    void HandleNotification(const QString& method, const QJsonObject& params);
    void StartToolDiscovery();
    void HandleToolDiscoveryFinished(int exitCode);
    void StartAppServer(const QStringList& enabledServerNames);
    void StopToolDiscovery();
    void FailSession(const QString& message);
    void ArmPhaseDeadline(int timeoutMs);
    void DisarmPhaseDeadline();
    void ShutdownProcess();
    bool GenerationCurrent(quint64 generation) const;

    static constexpr int kStartupDeadlineMs = 20000;
    static constexpr int kRealtimeStartDeadlineMs = 30000;
    static constexpr int kStopDeadlineMs = 5000;
    static constexpr qsizetype kMaximumToolDiscoveryBytes = 256 * 1024;

    std::unique_ptr<CodexJsonRpcProcess> rpc_;
    std::unique_ptr<QProcess> toolDiscoveryProcess_;
    std::unique_ptr<QTemporaryDir> isolatedWorkingDirectory_;
    QTimer* phaseDeadlineTimer_{nullptr};
    QByteArray toolDiscoveryOutput_;
    bool toolDiscoveryOverflow_{false};
    QString configuredExecutable_;
    QString resolvedExecutable_;
    QString threadId_;
    Phase phase_{Phase::Idle};
    quint64 generation_{0};
};

} // namespace openzoom

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
