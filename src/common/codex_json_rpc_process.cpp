#ifdef _WIN32

#include "openzoom/common/codex_json_rpc_process.hpp"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSignalBlocker>
#include <QTimer>

namespace openzoom {

CodexJsonRpcProcess::CodexJsonRpcProcess(QObject* parent)
    : QObject(parent), process_(std::make_unique<QProcess>(this))
{
    replyTimeoutTimer_ = new QTimer(this);
    replyTimeoutTimer_->setInterval(5000);
    connect(replyTimeoutTimer_, &QTimer::timeout,
            this, &CodexJsonRpcProcess::ExpireTimedOutReplies);

    process_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process_.get(), &QProcess::started, this, &CodexJsonRpcProcess::Started);
    connect(process_.get(), &QProcess::readyReadStandardOutput,
            this, &CodexJsonRpcProcess::ConsumeStdout);
    connect(process_.get(), &QProcess::readyReadStandardError, this, [this]() {
        // stderr can contain protocol payload fragments, file paths, URLs,
        // configuration values, or server diagnostics. Never forward its
        // contents; emit at most one payload-free indication per child.
        const QByteArray diagnostic = process_->readAllStandardError();
        if (!diagnostic.isEmpty() && !stderrObserved_) {
            stderrObserved_ = true;
            emit StderrText(
                QStringLiteral("diagnostic output present (content suppressed)"));
        }
    });
    connect(process_.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            emit FailedToStart();
        }
    });
    connect(process_.get(),
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus) { emit Finished(exitCode); });
}

CodexJsonRpcProcess::~CodexJsonRpcProcess()
{
    // QProcess::waitForFinished can synchronously deliver finished/error
    // callbacks; the owner is already being destroyed, so nothing may be
    // forwarded from here.
    const QSignalBlocker block(this);
    Shutdown();
}

void CodexJsonRpcProcess::SetProgram(const QString& program, const QStringList& arguments)
{
    process_->setProgram(program);
    process_->setArguments(arguments);
}

void CodexJsonRpcProcess::SetProcessEnvironment(const QProcessEnvironment& environment)
{
    process_->setProcessEnvironment(environment);
}

void CodexJsonRpcProcess::SetRequestTimeoutMs(int timeoutMs)
{
    requestTimeoutMs_ = timeoutMs > 0 ? timeoutMs : kDefaultRequestTimeoutMs;
}

void CodexJsonRpcProcess::SetServerRequestHandler(ServerRequestHandler handler)
{
    serverRequestHandler_ = std::move(handler);
}

bool CodexJsonRpcProcess::IsRunning() const
{
    return process_->state() != QProcess::NotRunning;
}

void CodexJsonRpcProcess::Start()
{
    if (IsRunning()) {
        return;
    }
    stdoutBuffer_.clear();
    stderrObserved_ = false;
    FailAllPendingReplies(QStringLiteral("Codex is restarting."));
    nextRequestId_ = 1;
    process_->start();
}

void CodexJsonRpcProcess::Shutdown()
{
    if (!IsRunning()) {
        return;
    }
    process_->closeWriteChannel();
    process_->terminate();
    if (!process_->waitForFinished(kShutdownGraceMs)) {
        process_->kill();
        if (!process_->waitForFinished(kShutdownGraceMs)) {
            qWarning() << "Codex process did not exit within the bounded "
                          "shutdown window";
        }
    }
}

qint64 CodexJsonRpcProcess::SendRequest(const QString& method,
                                        const QJsonObject& params,
                                        ReplyHandler handler,
                                        int timeoutMs)
{
    const qint64 id = nextRequestId_++;
    if (handler) {
        const int effectiveTimeout = timeoutMs > 0 ? timeoutMs : requestTimeoutMs_;
        pendingReplies_.insert(id,
                               PendingReply{std::move(handler),
                                            QDateTime::currentMSecsSinceEpoch() + effectiveTimeout});
        if (!replyTimeoutTimer_->isActive()) {
            replyTimeoutTimer_->start();
        }
    }
    SendObject(QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("method"), method},
                           {QStringLiteral("params"), params}});
    return id;
}

void CodexJsonRpcProcess::SendNotification(const QString& method, const QJsonObject& params)
{
    SendObject(QJsonObject{{QStringLiteral("method"), method},
                           {QStringLiteral("params"), params}});
}

void CodexJsonRpcProcess::SendResult(const QJsonValue& id, const QJsonObject& result)
{
    SendObject(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("result"), result}});
}

void CodexJsonRpcProcess::SendError(const QJsonValue& id, int code, const QString& message)
{
    SendObject(QJsonObject{{QStringLiteral("id"), id},
                           {QStringLiteral("error"),
                            QJsonObject{{QStringLiteral("code"), code},
                                        {QStringLiteral("message"), message}}}});
}

void CodexJsonRpcProcess::FailAllPendingReplies(const QString& message)
{
    if (pendingReplies_.isEmpty()) {
        replyTimeoutTimer_->stop();
        return;
    }
    QList<ReplyHandler> handlers;
    handlers.reserve(pendingReplies_.size());
    for (auto& pending : pendingReplies_) {
        handlers.push_back(std::move(pending.handler));
    }
    pendingReplies_.clear();
    replyTimeoutTimer_->stop();
    const QJsonObject error{{QStringLiteral("code"), -32000}, {QStringLiteral("message"), message}};
    for (ReplyHandler& handler : handlers) {
        handler({}, error);
    }
}

void CodexJsonRpcProcess::SendObject(const QJsonObject& object)
{
    if (!IsRunning()) {
        return;
    }
    QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    line.append('\n');
    process_->write(line);
}

void CodexJsonRpcProcess::ConsumeStdout()
{
    stdoutBuffer_.append(process_->readAllStandardOutput());
    qsizetype newline = -1;
    while ((newline = stdoutBuffer_.indexOf('\n')) >= 0) {
        if (newline > kMaximumProtocolMessageBytes) {
            FailProtocol(QStringLiteral("Codex sent an oversized protocol message."));
            return;
        }
        const QByteArray line = stdoutBuffer_.left(newline).trimmed();
        stdoutBuffer_.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            qWarning().noquote()
                << QStringLiteral("Ignoring malformed codex app-server message "
                                  "(%1 bytes, parse offset %2, %3)")
                       .arg(line.size())
                       .arg(parseError.offset)
                       .arg(parseError.error == QJsonParseError::NoError
                                ? QStringLiteral("not a JSON object")
                                : parseError.errorString());
            continue;
        }
        HandleMessage(document.object());
    }
    if (stdoutBuffer_.size() > kMaximumProtocolBufferBytes) {
        FailProtocol(QStringLiteral("Codex protocol buffering exceeded the safe limit."));
    }
}

void CodexJsonRpcProcess::HandleMessage(const QJsonObject& message)
{
    if (message.contains(QStringLiteral("id")) && message.contains(QStringLiteral("method"))) {
        const QJsonValue id = message.value(QStringLiteral("id"));
        const QString method = message.value(QStringLiteral("method")).toString();
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        if (serverRequestHandler_ && serverRequestHandler_(id, method, params)) {
            return;
        }
        SendError(id, -32601,
                  QStringLiteral("OpenZoom does not expose this Codex capability."));
        return;
    }
    if (message.contains(QStringLiteral("id"))) {
        const qint64 id = message.value(QStringLiteral("id")).toVariant().toLongLong();
        auto it = pendingReplies_.find(id);
        if (it != pendingReplies_.end()) {
            ReplyHandler handler = std::move(it.value().handler);
            pendingReplies_.erase(it);
            if (pendingReplies_.isEmpty()) {
                replyTimeoutTimer_->stop();
            }
            handler(message.value(QStringLiteral("result")).toObject(),
                    message.value(QStringLiteral("error")).toObject());
        }
        return;
    }
    emit NotificationReceived(message.value(QStringLiteral("method")).toString(),
                              message.value(QStringLiteral("params")).toObject());
}

void CodexJsonRpcProcess::FailProtocol(const QString& reason)
{
    stdoutBuffer_.clear();
    emit ProtocolFailed(reason);
    FailAllPendingReplies(reason);
    process_->kill();
}

void CodexJsonRpcProcess::ExpireTimedOutReplies()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<ReplyHandler> expired;
    for (auto it = pendingReplies_.begin(); it != pendingReplies_.end();) {
        if (it.value().deadlineMs <= now) {
            expired.push_back(std::move(it.value().handler));
            it = pendingReplies_.erase(it);
        } else {
            ++it;
        }
    }
    if (pendingReplies_.isEmpty()) {
        replyTimeoutTimer_->stop();
    }
    const QJsonObject error{{QStringLiteral("code"), -32001},
                            {QStringLiteral("message"), QStringLiteral("Codex request timed out.")}};
    for (ReplyHandler& handler : expired) {
        handler({}, error);
    }
}

} // namespace openzoom

#endif // _WIN32
