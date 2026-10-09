#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace openzoom {

// Shared JSONL/JSON-RPC transport for local Codex app-server children. Owns
// process lifetime, request ids and reply deadlines, newline framing with hard
// message/buffer bounds, one payload-free stderr indication per child, and
// default denial of unexpected
// server-to-client requests. Feature clients (Assistant, realtime
// transcription) own only their method and notification state.
//
// Diagnostics never include raw payload bytes: malformed or oversized traffic
// is reported by size and parse offset only, because protocol lines can carry
// speech transcripts, SDP, or account details.
class CodexJsonRpcProcess : public QObject {
    Q_OBJECT
public:
    using ReplyHandler = std::function<void(const QJsonObject& result, const QJsonObject& error)>;
    // Return true when the request was answered via SendResult/SendError;
    // returning false makes the transport send the standard capability denial.
    using ServerRequestHandler = std::function<bool(const QJsonValue& id,
                                                    const QString& method,
                                                    const QJsonObject& params)>;

    explicit CodexJsonRpcProcess(QObject* parent = nullptr);
    ~CodexJsonRpcProcess() override;

    void SetProgram(const QString& program, const QStringList& arguments);
    void SetProcessEnvironment(const QProcessEnvironment& environment);
    void SetRequestTimeoutMs(int timeoutMs);
    void SetServerRequestHandler(ServerRequestHandler handler);

    // True while the child is starting or running.
    bool IsRunning() const;

    // Clears framing state, fails leftover replies, resets request ids, and
    // starts the configured program.
    void Start();

    // Bounded stop: close stdin, terminate, then kill. Safe to repeat.
    void Shutdown();

    // Callers must fail pending replies themselves when the process finishes;
    // the transport cannot know which user-facing message each client needs.
    qint64 SendRequest(const QString& method,
                       const QJsonObject& params,
                       ReplyHandler handler = {},
                       int timeoutMs = -1);
    void SendNotification(const QString& method, const QJsonObject& params = {});
    void SendResult(const QJsonValue& id, const QJsonObject& result);
    void SendError(const QJsonValue& id, int code, const QString& message);
    void FailAllPendingReplies(const QString& message);

signals:
    void Started();
    void FailedToStart();
    void Finished(int exitCode);
    void NotificationReceived(const QString& method, const QJsonObject& params);
    void StderrText(const QString& truncatedDiagnostic);
    // Framing or size-bound violation. After the connected slots return, the
    // transport fails pending replies and kills the child.
    void ProtocolFailed(const QString& reason);

private:
    struct PendingReply {
        ReplyHandler handler;
        qint64 deadlineMs{0};
    };

    void SendObject(const QJsonObject& object);
    void ConsumeStdout();
    void HandleMessage(const QJsonObject& message);
    void FailProtocol(const QString& reason);
    void ExpireTimedOutReplies();

    static constexpr int kDefaultRequestTimeoutMs = 60000;
    static constexpr int kShutdownGraceMs = 250;
    static constexpr qsizetype kMaximumProtocolBufferBytes = 4 * 1024 * 1024;
    static constexpr qsizetype kMaximumProtocolMessageBytes = 2 * 1024 * 1024;

    std::unique_ptr<QProcess> process_;
    QByteArray stdoutBuffer_;
    bool stderrObserved_{false};
    QHash<qint64, PendingReply> pendingReplies_;
    QTimer* replyTimeoutTimer_{nullptr};
    qint64 nextRequestId_{1};
    int requestTimeoutMs_{kDefaultRequestTimeoutMs};
    ServerRequestHandler serverRequestHandler_;
};

} // namespace openzoom

#endif // defined(_WIN32) || defined(Q_MOC_RUN)
