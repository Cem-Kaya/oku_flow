#ifdef _WIN32

#include "openzoom/common/codex_app_server_client.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace openzoom {

namespace {

constexpr auto kAssistantIdentity =
    "You are the assistant inside OpenZoom, an accessibility magnifier for people with low vision. "
    "Give concise, concrete answers that prioritize readable text, controls, obstacles, and spatial "
    "relationships when an image is attached. Do not use Markdown tables. ";

QString ErrorMessage(const QJsonObject& error, const QString& fallback)
{
    const QString message = error.value(QStringLiteral("message")).toString().trimmed();
    return message.isEmpty() ? fallback : message;
}

QStringList StringArray(const QJsonArray& values)
{
    QStringList result;
    result.reserve(values.size());
    for (const QJsonValue& value : values) {
        if (value.isString()) {
            result.push_back(value.toString());
        }
    }
    return result;
}

} // namespace

CodexAppServerClient::CodexAppServerClient(QObject* parent)
    : QObject(parent), rpc_(std::make_unique<CodexJsonRpcProcess>(this))
{
    turnWatchdogTimer_ = new QTimer(this);
    turnWatchdogTimer_->setInterval(1000);
    connect(turnWatchdogTimer_, &QTimer::timeout,
            this, &CodexAppServerClient::CheckTurnWatchdog);

    rpc_->SetServerRequestHandler(
        [this](const QJsonValue& id, const QString& method, const QJsonObject& params) {
            return HandleServerRequest(id, method, params);
        });
    connect(rpc_.get(), &CodexJsonRpcProcess::Started, this, [this]() {
        emit ServerStateChanged(false, QStringLiteral("Connecting to Codex..."));
        QJsonObject clientInfo{
            {QStringLiteral("name"), QStringLiteral("openzoom")},
            {QStringLiteral("title"), QStringLiteral("OpenZoom")},
            {QStringLiteral("version"), QCoreApplication::applicationVersion().isEmpty()
                                            ? QStringLiteral("0.1.0")
                                            : QCoreApplication::applicationVersion()}};
        SendRequest(QStringLiteral("initialize"),
                    QJsonObject{{QStringLiteral("clientInfo"), clientInfo}},
                    [this](const QJsonObject& result, const QJsonObject& error) {
                        FinishInitialization(result, error);
                    });
    });
    connect(rpc_.get(), &CodexJsonRpcProcess::NotificationReceived,
            this, &CodexAppServerClient::HandleNotification);
    connect(rpc_.get(), &CodexJsonRpcProcess::StderrText, this, [](const QString& diagnostic) {
        qWarning().noquote() << "codex app-server:" << diagnostic;
    });
    connect(rpc_.get(), &CodexJsonRpcProcess::ProtocolFailed, this, [this](const QString& reason) {
        emit ServerStateChanged(false, reason);
        if (IsTurnActive()) {
            FinishActiveTurn({}, reason, false);
        }
    });
    connect(rpc_.get(), &CodexJsonRpcProcess::FailedToStart, this, [this]() {
        initialized_ = false;
        emit ServerStateChanged(
            false,
            QStringLiteral("Codex CLI not found. Install Codex or set its path in AI Settings."));
        if (pendingTurn_.valid) {
            FinishActiveTurn({}, QStringLiteral("Codex CLI could not be started."), false);
        }
    });
    connect(rpc_.get(), &CodexJsonRpcProcess::Finished, this, [this](int exitCode) {
        const bool wasInitialized = initialized_;
        initialized_ = false;
        signedIn_ = false;
        rpc_->FailAllPendingReplies(QStringLiteral("Codex stopped before replying."));
        if (wasInitialized || exitCode != 0) {
            emit ServerStateChanged(
                false,
                QStringLiteral("Codex stopped (exit code %1).").arg(exitCode));
        }
        if (!activeThreadId_.isEmpty() || pendingTurn_.valid) {
            FinishActiveTurn({}, QStringLiteral("Codex stopped before the answer completed."), false);
        }
    });
}

CodexAppServerClient::~CodexAppServerClient()
{
    // QProcess::waitForFinished can synchronously deliver finished/error
    // callbacks. They must complete internal cleanup without forwarding state
    // changes from an object whose owner is already being destroyed.
    const QSignalBlocker block(this);
    Shutdown();
}

void CodexAppServerClient::Configure(const QString& executablePath,
                                     const QString& preferredModel,
                                     const QString& reasoningEffort,
                                     const QString& assistantInstructions,
                                     bool internetEnabled,
                                     bool codingEnabled,
                                     const QString& workspaceDirectory)
{
    const QString newExecutable = executablePath.trimmed();
    const bool executableChanged = configuredExecutable_ != newExecutable;
    configuredExecutable_ = newExecutable;
    preferredModel_ = preferredModel.trimmed();
    const QString normalizedEffort = reasoningEffort.trimmed().toLower();
    static const QStringList supportedEfforts{
        QStringLiteral("none"), QStringLiteral("minimal"),
        QStringLiteral("low"), QStringLiteral("medium"),
        QStringLiteral("high"), QStringLiteral("xhigh")};
    reasoningEffort_ = supportedEfforts.contains(normalizedEffort)
                           ? normalizedEffort
                           : QStringLiteral("low");
    assistantInstructions_ = assistantInstructions.trimmed();
    internetEnabled_ = internetEnabled;
    codingEnabled_ = codingEnabled;
    workspaceDirectory_ = workspaceDirectory.trimmed();
    if (!workspaceDirectory_.isEmpty()) {
        workspaceDirectory_ = QDir::toNativeSeparators(QDir::cleanPath(workspaceDirectory_));
    }
    if (!preferredModel_.isEmpty()) {
        selectedModel_ = preferredModel_;
    }
    if (executableChanged && rpc_->IsRunning()) {
        Shutdown();
    }
}

void CodexAppServerClient::Start()
{
    if (rpc_->IsRunning()) {
        return;
    }

    initialized_ = false;
    rpc_->SetProgram(ResolveExecutable(),
                     {QStringLiteral("app-server"),
                      QStringLiteral("--listen"),
                      QStringLiteral("stdio://")});
    rpc_->SetProcessEnvironment(QProcessEnvironment::systemEnvironment());
    emit ServerStateChanged(false, QStringLiteral("Starting Codex..."));
    rpc_->Start();
}

void CodexAppServerClient::Shutdown()
{
    if (!rpc_->IsRunning()) {
        return;
    }
    if (IsTurnActive()) {
        InterruptTurn();
    }
    rpc_->Shutdown();
}

bool CodexAppServerClient::IsReady() const { return initialized_; }
bool CodexAppServerClient::IsSignedIn() const { return signedIn_; }
bool CodexAppServerClient::IsTurnActive() const
{
    return pendingTurn_.valid || !activeThreadId_.isEmpty();
}
QString CodexAppServerClient::SelectedModel() const { return selectedModel_; }

QString CodexAppServerClient::BuiltInAssistantInstructions()
{
    return QString::fromLatin1(kAssistantIdentity).trimmed();
}

void CodexAppServerClient::RefreshAccount()
{
    if (!initialized_) {
        Start();
        return;
    }
    SendRequest(QStringLiteral("account/read"),
                QJsonObject{{QStringLiteral("refreshToken"), false}},
                [this](const QJsonObject& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        signedIn_ = false;
                        emit AccountChanged(false, ErrorMessage(error, QStringLiteral("Sign-in check failed.")), {});
                        if (pendingTurn_.valid) {
                            FinishActiveTurn({}, ErrorMessage(error, QStringLiteral("Codex sign-in check failed.")), false);
                        }
                        return;
                    }
                    const QJsonObject account = result.value(QStringLiteral("account")).toObject();
                    signedIn_ = !account.isEmpty();
                    QString plan = account.value(QStringLiteral("planType")).toString();
                    QString label;
                    if (account.value(QStringLiteral("type")).toString() == QStringLiteral("chatgpt")) {
                        const QString email = account.value(QStringLiteral("email")).toString();
                        label = email.isEmpty() ? QStringLiteral("Connected to ChatGPT")
                                                : QStringLiteral("Connected as %1").arg(email);
                    } else if (signedIn_) {
                        label = QStringLiteral("Connected to Codex");
                    } else {
                        label = QStringLiteral("Not connected");
                    }
                    emit AccountChanged(signedIn_, label, plan);
                    if (signedIn_) {
                        SendRequest(QStringLiteral("account/rateLimits/read"), {},
                                    [this](const QJsonObject& limits, const QJsonObject&) {
                                        const QJsonObject snapshot = limits.value(QStringLiteral("rateLimits")).toObject();
                                        const QJsonObject primary = snapshot.value(QStringLiteral("primary")).toObject();
                                        if (!primary.isEmpty()) {
                                            const int usedPercent = primary.value(
                                                QStringLiteral("usedPercent")).toInt();
                                            const int percentLeft = std::clamp(100 - usedPercent, 0, 100);
                                            emit RateLimitChanged(
                                                QStringLiteral("Codex usage: %1% left in current window")
                                                    .arg(percentLeft));
                                        }
                                    });
                        if (pendingTurn_.valid) {
                            SubmitPendingTurn();
                        }
                    } else if (pendingTurn_.valid) {
                        FinishActiveTurn({},
                                         QStringLiteral("Connect a ChatGPT account in Advanced > Assistant before using Explain."),
                                         false);
                    }
                });
}

void CodexAppServerClient::StartChatGptLogin()
{
    if (!initialized_) {
        loginWhenReady_ = true;
        Start();
        emit ServerStateChanged(false, QStringLiteral("Starting ChatGPT sign-in..."));
        return;
    }
    loginWhenReady_ = false;
    SendRequest(QStringLiteral("account/login/start"),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("chatgpt")}},
                [this](const QJsonObject& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        emit AccountChanged(false, ErrorMessage(error, QStringLiteral("Could not start ChatGPT sign-in.")), {});
                        return;
                    }
                    const QUrl url(result.value(QStringLiteral("authUrl")).toString());
                    if (url.isValid()) {
                        emit LoginUrlReady(url);
                    }
                });
}

void CodexAppServerClient::RequestVisionTurn(const QString& prompt,
                                             const QString& imagePath,
                                             const QString& threadId,
                                             bool persistent)
{
    if (IsTurnActive()) {
        if (!imagePath.trimmed().isEmpty()) {
            QFile::remove(imagePath);
        }
        emit TurnFinished(threadId, {}, {},
                          QStringLiteral("Another assistant request is already running."),
                          false, persistent);
        return;
    }
    if (persistent && codingEnabled_) {
        const QFileInfo workspace(workspaceDirectory_);
        if (!workspace.exists() || !workspace.isDir()) {
            if (!imagePath.trimmed().isEmpty()) {
                QFile::remove(imagePath);
            }
            emit TurnFinished(threadId, {}, {},
                              QStringLiteral("Choose an existing coding workspace in AI Settings."),
                              false, persistent);
            return;
        }
    }
    pendingTurn_.prompt = prompt.trimmed();
    pendingTurn_.imagePath = imagePath;
    pendingTurn_.threadId = threadId.trimmed();
    pendingTurn_.persistent = persistent;
    pendingTurn_.valid = true;
    if (pendingTurn_.prompt.isEmpty()) {
        pendingTurn_.prompt = QStringLiteral("Describe the current view for a low-vision user.");
    }
    Start();
    if (initialized_) {
        if (signedIn_) {
            SubmitPendingTurn();
        } else {
            RefreshAccount();
        }
    }
}

void CodexAppServerClient::InterruptTurn()
{
    RequestInterrupt(QStringLiteral("Assistant request stopped."));
}

void CodexAppServerClient::RequestInterrupt(const QString& reason)
{
    if (interruptRequested_) {
        return;
    }
    interruptRequested_ = true;
    interruptReason_ = reason;
    interruptDeadlineMs_ =
        QDateTime::currentMSecsSinceEpoch() + kInterruptGraceMs;
    if (!turnWatchdogTimer_->isActive()) {
        turnWatchdogTimer_->start();
    }
    if (!initialized_) {
        FinishActiveTurn({}, reason, true);
        return;
    }
    if (activeThreadId_.isEmpty() || activeTurnId_.isEmpty()) {
        FinishActiveTurn({}, reason, true);
        return;
    }
    SendRequest(QStringLiteral("turn/interrupt"),
                QJsonObject{{QStringLiteral("threadId"), activeThreadId_},
                            {QStringLiteral("turnId"), activeTurnId_}},
                [this](const QJsonObject&, const QJsonObject& error) {
                    if (!error.isEmpty() && IsTurnActive()) {
                        emit ServerStateChanged(
                            initialized_,
                            ErrorMessage(error,
                                         QStringLiteral("Codex did not acknowledge cancellation.")));
                    }
                });
}

void CodexAppServerClient::LoadConversation(const QString& threadId)
{
    if (!initialized_) {
        emit ConversationTranscriptLoaded(threadId, {});
        return;
    }
    SendRequest(QStringLiteral("thread/read"),
                QJsonObject{{QStringLiteral("threadId"), threadId},
                            {QStringLiteral("includeTurns"), true}},
                [this, threadId](const QJsonObject& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        emit ServerStateChanged(true, ErrorMessage(error, QStringLiteral("Could not load conversation.")));
                        return;
                    }
                    emit ConversationTranscriptLoaded(
                        threadId, TranscriptFromThread(result.value(QStringLiteral("thread")).toObject()));
                });
}

void CodexAppServerClient::RenameConversation(const QString& threadId, const QString& name)
{
    if (!initialized_ || name.trimmed().isEmpty()) {
        return;
    }
    const QString cleanName = name.trimmed();
    SendRequest(QStringLiteral("thread/name/set"),
                QJsonObject{{QStringLiteral("threadId"), threadId},
                            {QStringLiteral("name"), cleanName}},
                [this, threadId, cleanName](const QJsonObject&, const QJsonObject& error) {
                    if (error.isEmpty()) {
                        emit ConversationRenamed(threadId, cleanName);
                    }
                });
}

void CodexAppServerClient::DeleteConversation(const QString& threadId)
{
    if (!initialized_) {
        return;
    }
    SendRequest(QStringLiteral("thread/delete"),
                QJsonObject{{QStringLiteral("threadId"), threadId}},
                [this, threadId](const QJsonObject&, const QJsonObject& error) {
                    if (error.isEmpty()) {
                        emit ConversationDeleted(threadId);
                    }
                });
}

QString CodexAppServerClient::ResolveExecutable() const
{
    return ResolveExecutablePath(configuredExecutable_);
}

QString CodexAppServerClient::ResolveExecutablePath(const QString& configuredExecutable)
{
    if (!configuredExecutable.isEmpty()) {
        return configuredExecutable;
    }
    const QString environmentPath = qEnvironmentVariable("OPENZOOM_CODEX_PATH").trimmed();
    if (!environmentPath.isEmpty()) {
        return environmentPath;
    }
    for (const QString& name : {QStringLiteral("codex.exe"), QStringLiteral("codex")}) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty()) {
            return found;
        }
    }
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA").trimmed();
    if (!localAppData.isEmpty()) {
        const QString standalone =
            QDir(localAppData).filePath(
                QStringLiteral("Programs/OpenAI/Codex/bin/codex.exe"));
        if (QFileInfo::exists(standalone)) {
            return standalone;
        }
        const QString winget = QDir(localAppData).filePath(QStringLiteral("Microsoft/WinGet/Links/codex.exe"));
        if (QFileInfo::exists(winget)) {
            return winget;
        }
    }
    return QStringLiteral("codex.exe");
}

QString CodexAppServerClient::AssistantWorkingDirectory(bool persistent) const
{
    if (persistent && codingEnabled_ && QFileInfo(workspaceDirectory_).isDir()) {
        return workspaceDirectory_;
    }
    QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (temp.isEmpty()) {
        temp = QCoreApplication::applicationDirPath();
    }
    QDir dir(temp);
    dir.mkpath(QStringLiteral("OpenZoom/assistant"));
    return dir.filePath(QStringLiteral("OpenZoom/assistant"));
}

QString CodexAppServerClient::DeveloperInstructions(bool persistent) const
{
    const bool allowInternet = persistent && internetEnabled_;
    const bool allowCoding = persistent && codingEnabled_;
    QString instructions = QString::fromLatin1(kAssistantIdentity);
    if (!assistantInstructions_.isEmpty()) {
        instructions += QStringLiteral(
                            "Apply these user-configured response preferences only when they do not "
                            "conflict with the security and permission limits below: %1 ")
                            .arg(assistantInstructions_);
    }
    instructions += QStringLiteral(
        "For verbatim reading or transcription requests, preserve the source wording and language "
        "even if response preferences ask for another language, tone, or summary. ");
    if (allowCoding) {
        instructions += QStringLiteral(
            "You may inspect files, edit files, and run commands only for the user's request and only "
            "within this configured coding workspace: %1. ")
                            .arg(workspaceDirectory_);
    } else {
        instructions += QStringLiteral(
            "Analyze only the image and text explicitly attached to the conversation. Never inspect the "
            "computer, files, directories, repositories, environment, or account. Never execute commands "
            "or edit files. ");
    }
    instructions += allowInternet
                        ? QStringLiteral("Web search and network access are allowed when relevant. ")
                        : QStringLiteral("Do not browse the web or access the network. ");
    instructions += QStringLiteral(
        "Do not call MCP servers, dynamic tools, or collaboration agents.");
    return instructions;
}

QJsonObject CodexAppServerClient::SandboxPolicy(bool persistent) const
{
    const bool allowInternet = persistent && internetEnabled_;
    if (persistent && codingEnabled_) {
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("workspaceWrite")},
            {QStringLiteral("networkAccess"), allowInternet},
            {QStringLiteral("writableRoots"),
             QJsonArray{QDir::toNativeSeparators(workspaceDirectory_)}}};
    }
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("readOnly")},
                       {QStringLiteral("networkAccess"), allowInternet}};
}

void CodexAppServerClient::SendNotification(const QString& method, const QJsonObject& params)
{
    rpc_->SendNotification(method, params);
}

qint64 CodexAppServerClient::SendRequest(const QString& method,
                                         const QJsonObject& params,
                                         ReplyHandler handler)
{
    return rpc_->SendRequest(method, params, std::move(handler));
}

void CodexAppServerClient::HandleNotification(const QString& method, const QJsonObject& params)
{
    if (IsTurnActive() &&
        (method.startsWith(QStringLiteral("item/")) ||
         method.startsWith(QStringLiteral("turn/")))) {
        TouchTurnWatchdog();
    }
    if (method == QStringLiteral("account/login/completed")) {
        if (params.value(QStringLiteral("success")).toBool()) {
            RefreshAccount();
        } else {
            emit AccountChanged(false,
                                params.value(QStringLiteral("error")).toString(QStringLiteral("ChatGPT sign-in failed.")),
                                {});
        }
        return;
    }
    if (method == QStringLiteral("account/updated")) {
        const QString authMode = params.value(QStringLiteral("authMode")).toString();
        signedIn_ = !authMode.isEmpty();
        emit AccountChanged(signedIn_,
                            signedIn_ ? QStringLiteral("Connected to Codex") : QStringLiteral("Not connected"),
                            params.value(QStringLiteral("planType")).toString());
        return;
    }
    if (method == QStringLiteral("item/agentMessage/delta")) {
        const QString threadId = params.value(QStringLiteral("threadId")).toString();
        const QString turnId = params.value(QStringLiteral("turnId")).toString();
        if (threadId.isEmpty() || turnId.isEmpty() || !params.value(QStringLiteral("delta")).isString()) {
            qWarning() << "Codex: malformed agentMessage/delta notification ignored";
            return;
        }
        if (threadId == activeThreadId_ && (activeTurnId_.isEmpty() || turnId == activeTurnId_)) {
            activeTurnId_ = turnId;
            const QString delta = params.value(QStringLiteral("delta")).toString();
            const QString accepted = AppendActiveText(delta);
            if (!accepted.isEmpty()) {
                emit TurnTextDelta(threadId, turnId, accepted);
            }
        }
        return;
    }
    if (method == QStringLiteral("item/started")) {
        RejectForbiddenAgentItem(params.value(QStringLiteral("item")).toObject());
        return;
    }
    if (method == QStringLiteral("item/completed")) {
        const QJsonObject item = params.value(QStringLiteral("item")).toObject();
        if (item.value(QStringLiteral("type")).toString() == QStringLiteral("agentMessage") &&
            activeText_.trimmed().isEmpty()) {
            AppendActiveText(item.value(QStringLiteral("text")).toString());
        }
        return;
    }
    if (method == QStringLiteral("turn/completed")) {
        const QString threadId = params.value(QStringLiteral("threadId")).toString();
        const QJsonObject turn = params.value(QStringLiteral("turn")).toObject();
        const QString turnId = turn.value(QStringLiteral("id")).toString();
        if (threadId != activeThreadId_ || (!activeTurnId_.isEmpty() && turnId != activeTurnId_)) {
            return;
        }
        const QString status = turn.value(QStringLiteral("status")).toString();
        QString error;
        if (status == QStringLiteral("failed")) {
            error = turn.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        }
        if (activeText_.trimmed().isEmpty()) {
            AppendActiveText(FinalAgentText(turn));
        }
        const bool interrupted =
            status == QStringLiteral("interrupted") || interruptRequested_;
        if (interrupted && error.isEmpty() && !interruptReason_.isEmpty()) {
            error = interruptReason_;
        }
        FinishActiveTurn(activeText_, error, interrupted);
        return;
    }
    if (method == QStringLiteral("error")) {
        const QJsonObject error = params.value(QStringLiteral("error")).toObject();
        if (IsTurnActive()) {
            FinishActiveTurn({}, ErrorMessage(error, QStringLiteral("Codex request failed.")), false);
        }
        return;
    }
    if (method == QStringLiteral("warning")) {
        const QString warning = params.value(QStringLiteral("message")).toString();
        if (!warning.isEmpty()) {
            emit ServerStateChanged(initialized_, warning);
        }
    }
}

bool CodexAppServerClient::HandleServerRequest(const QJsonValue& id,
                                               const QString& method,
                                               const QJsonObject&)
{
    QJsonObject result;
    if (method == QStringLiteral("item/permissions/requestApproval")) {
        // Permission requests use a grant-profile response rather than the
        // decision response used by command/file approvals. An empty profile
        // grants none of the requested filesystem or network permissions.
        result.insert(QStringLiteral("permissions"), QJsonObject{});
        result.insert(QStringLiteral("scope"), QStringLiteral("turn"));
    } else if (method == QStringLiteral("item/commandExecution/requestApproval") ||
               method == QStringLiteral("item/fileChange/requestApproval")) {
        result.insert(QStringLiteral("decision"), QStringLiteral("decline"));
    } else if (method == QStringLiteral("mcpServer/elicitation/request")) {
        result.insert(QStringLiteral("action"), QStringLiteral("decline"));
        result.insert(QStringLiteral("content"), QJsonValue::Null);
    } else {
        // Unhandled: the transport sends the standard capability denial.
        return false;
    }
    rpc_->SendResult(id, result);
    if (IsTurnActive()) {
        InterruptTurn();
    }
    return true;
}

void CodexAppServerClient::FinishInitialization(const QJsonObject& result,
                                                const QJsonObject& error)
{
    if (!error.isEmpty()) {
        initialized_ = false;
        emit ServerStateChanged(false, ErrorMessage(error, QStringLiteral("Codex initialization failed.")));
        return;
    }
    initialized_ = true;
    appServerIdentity_ =
        result.value(QStringLiteral("userAgent")).toString().trimmed();
    if (appServerIdentity_.isEmpty()) {
        appServerIdentity_ = QStringLiteral("the installed Codex app-server");
    }
    SendNotification(QStringLiteral("initialized"));
    emit ServerStateChanged(true, QStringLiteral("Codex ready"));
    SendRequest(QStringLiteral("model/list"),
                QJsonObject{{QStringLiteral("limit"), 100},
                            {QStringLiteral("includeHidden"), false}},
                [this](const QJsonObject& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        const QString fallback =
                            QStringLiteral("Model discovery failed with %1. "
                                           "Update the Codex CLI if its model cache is incompatible.")
                                .arg(appServerIdentity_);
                        emit ServerStateChanged(
                            true,
                            ErrorMessage(error, fallback));
                        return;
                    }
                    QStringList modelIds;
                    QJsonArray modelCatalog;
                    QString defaultImageModel;
                    for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
                        const QJsonObject model = value.toObject();
                        const QString id = model.value(QStringLiteral("id")).toString();
                        const QStringList modalities = StringArray(model.value(QStringLiteral("inputModalities")).toArray());
                        if (id.isEmpty() || (!modalities.isEmpty() && !modalities.contains(QStringLiteral("image")))) {
                            continue;
                        }
                        modelIds.push_back(id);
                        modelCatalog.push_back(model);
                        if (model.value(QStringLiteral("isDefault")).toBool()) {
                            defaultImageModel = id;
                        }
                    }
                    if (!preferredModel_.isEmpty() && modelIds.contains(preferredModel_)) {
                        selectedModel_ = preferredModel_;
                    } else if (!defaultImageModel.isEmpty()) {
                        selectedModel_ = defaultImageModel;
                    } else if (!modelIds.isEmpty()) {
                        selectedModel_ = modelIds.front();
                    }
                    emit ModelsChanged(modelIds, selectedModel_);
                    emit ModelCatalogChanged(modelCatalog, selectedModel_);
                });
    RefreshAccount();
    if (loginWhenReady_) {
        StartChatGptLogin();
    }
}

void CodexAppServerClient::SubmitPendingTurn()
{
    if (!pendingTurn_.valid || !initialized_ || !signedIn_) {
        return;
    }
    if (pendingTurn_.threadId.isEmpty()) {
        StartNewThreadForPendingTurn();
    } else {
        ResumeThreadForPendingTurn();
    }
}

void CodexAppServerClient::StartNewThreadForPendingTurn()
{
    const bool persistent = pendingTurn_.persistent;
    const bool allowCoding = persistent && codingEnabled_;
    QJsonObject params{
        {QStringLiteral("cwd"), AssistantWorkingDirectory(persistent)},
        {QStringLiteral("approvalPolicy"), QStringLiteral("never")},
        {QStringLiteral("sandbox"), allowCoding ? QStringLiteral("workspace-write")
                                                : QStringLiteral("read-only")},
        {QStringLiteral("ephemeral"), !persistent},
        {QStringLiteral("serviceName"), QStringLiteral("openzoom")},
        {QStringLiteral("developerInstructions"), DeveloperInstructions(persistent)}};
    if (!selectedModel_.isEmpty()) {
        params.insert(QStringLiteral("model"), selectedModel_);
    }
    SendRequest(QStringLiteral("thread/start"), params,
                [this](const QJsonObject& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        FinishActiveTurn({}, ErrorMessage(error, QStringLiteral("Could not start assistant conversation.")), false);
                        return;
                    }
                    const QJsonObject thread = result.value(QStringLiteral("thread")).toObject();
                    const QString threadId = thread.value(QStringLiteral("id")).toString();
                    if (threadId.isEmpty()) {
                        FinishActiveTurn({}, QStringLiteral("Codex returned no conversation identifier."), false);
                        return;
                    }
                    pendingTurn_.threadId = threadId;
                    if (pendingTurn_.persistent) {
                        emit ConversationCreated(thread);
                    }
                    StartTurnOnThread(threadId);
                });
}

void CodexAppServerClient::ResumeThreadForPendingTurn()
{
    const bool persistent = pendingTurn_.persistent;
    const bool allowCoding = persistent && codingEnabled_;
    QJsonObject params{
        {QStringLiteral("threadId"), pendingTurn_.threadId},
        {QStringLiteral("cwd"), AssistantWorkingDirectory(persistent)},
        {QStringLiteral("approvalPolicy"), QStringLiteral("never")},
        {QStringLiteral("sandbox"), allowCoding ? QStringLiteral("workspace-write")
                                                : QStringLiteral("read-only")},
        {QStringLiteral("developerInstructions"), DeveloperInstructions(persistent)}};
    if (!selectedModel_.isEmpty()) {
        params.insert(QStringLiteral("model"), selectedModel_);
    }
    SendRequest(QStringLiteral("thread/resume"), params,
                [this](const QJsonObject&, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        FinishActiveTurn({}, ErrorMessage(error, QStringLiteral("Could not resume assistant conversation.")), false);
                        return;
                    }
                    StartTurnOnThread(pendingTurn_.threadId);
                });
}

void CodexAppServerClient::StartTurnOnThread(const QString& threadId)
{
    const bool persistent = pendingTurn_.persistent;
    const QString workingDirectory = AssistantWorkingDirectory(persistent);
    const QJsonObject sandboxPolicy = SandboxPolicy(persistent);
    QJsonArray input{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), pendingTurn_.prompt}}};
    if (!pendingTurn_.imagePath.trimmed().isEmpty()) {
        input.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("localImage")},
                                 {QStringLiteral("path"), QDir::toNativeSeparators(pendingTurn_.imagePath)}});
    }

    activeThreadId_ = threadId;
    activePersistent_ = persistent;
    activeInternetEnabled_ = persistent && internetEnabled_;
    activeCodingEnabled_ = persistent && codingEnabled_;
    activeImagePath_ = pendingTurn_.imagePath;
    activeText_.clear();
    activeTextTruncated_ = false;
    activeTurnId_.clear();
    turnStartedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    lastTurnActivityMs_ = turnStartedAtMs_;
    interruptRequested_ = false;
    interruptDeadlineMs_ = 0;
    interruptReason_.clear();
    turnWatchdogTimer_->start();
    pendingTurn_ = {};

    QJsonObject params{
        {QStringLiteral("threadId"), threadId},
        {QStringLiteral("input"), input},
        {QStringLiteral("approvalPolicy"), QStringLiteral("never")},
        {QStringLiteral("sandboxPolicy"), sandboxPolicy},
        {QStringLiteral("cwd"), workingDirectory},
        {QStringLiteral("effort"), reasoningEffort_}};
    if (!selectedModel_.isEmpty()) {
        params.insert(QStringLiteral("model"), selectedModel_);
    }
    SendRequest(QStringLiteral("turn/start"), params,
                [this, threadId, persistent](const QJsonObject& result, const QJsonObject& error) {
                    if (!error.isEmpty()) {
                        FinishActiveTurn({}, ErrorMessage(error, QStringLiteral("Could not start assistant request.")), false);
                        return;
                    }
                    activeTurnId_ = result.value(QStringLiteral("turn")).toObject()
                                            .value(QStringLiteral("id")).toString();
                    emit TurnStarted(threadId, activeTurnId_, persistent);
                });
}

void CodexAppServerClient::FinishActiveTurn(const QString& text,
                                            const QString& error,
                                            bool interrupted)
{
    // text can alias activeText_; copy it before clearing the active state.
    const QString finishedText = text.trimmed();
    const QString finishedError = error.trimmed();
    const QString threadId = activeThreadId_.isEmpty() ? pendingTurn_.threadId : activeThreadId_;
    const QString turnId = activeTurnId_;
    const bool persistent = activeThreadId_.isEmpty() ? pendingTurn_.persistent : activePersistent_;
    const QString imagePath = activeImagePath_.isEmpty() ? pendingTurn_.imagePath : activeImagePath_;
    pendingTurn_ = {};
    activeThreadId_.clear();
    activeTurnId_.clear();
    activeText_.clear();
    activeImagePath_.clear();
    activePersistent_ = false;
    activeInternetEnabled_ = false;
    activeCodingEnabled_ = false;
    interruptingForbiddenItem_ = false;
    activeTextTruncated_ = false;
    interruptRequested_ = false;
    turnStartedAtMs_ = 0;
    lastTurnActivityMs_ = 0;
    interruptDeadlineMs_ = 0;
    interruptReason_.clear();
    turnWatchdogTimer_->stop();
    if (!imagePath.isEmpty()) {
        QFile::remove(imagePath);
    }
    emit TurnFinished(threadId, turnId, finishedText, finishedError, interrupted, persistent);
}

void CodexAppServerClient::RejectForbiddenAgentItem(const QJsonObject& item)
{
    const QString type = item.value(QStringLiteral("type")).toString();
    const bool codingItem = type == QStringLiteral("commandExecution") ||
                            type == QStringLiteral("fileChange");
    const bool internetItem = type == QStringLiteral("webSearch");
    const bool alwaysForbidden = type == QStringLiteral("mcpToolCall") ||
                                 type == QStringLiteral("dynamicToolCall") ||
                                 type == QStringLiteral("collabToolCall") ||
                                 type == QStringLiteral("collabAgentToolCall");
    const bool forbidden = alwaysForbidden || (codingItem && !activeCodingEnabled_) ||
                           (internetItem && !activeInternetEnabled_);
    if (!forbidden || interruptingForbiddenItem_) {
        return;
    }
    interruptingForbiddenItem_ = true;
    emit ServerStateChanged(true,
                            QStringLiteral("Stopped an unexpected Codex capability: %1").arg(type));
    InterruptTurn();
}

void CodexAppServerClient::TouchTurnWatchdog()
{
    lastTurnActivityMs_ = QDateTime::currentMSecsSinceEpoch();
}

void CodexAppServerClient::CheckTurnWatchdog()
{
    if (!IsTurnActive()) {
        turnWatchdogTimer_->stop();
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (interruptRequested_) {
        if (interruptDeadlineMs_ > 0 && now >= interruptDeadlineMs_) {
            FinishActiveTurn({}, interruptReason_.isEmpty()
                                     ? QStringLiteral("Assistant cancellation timed out.")
                                     : interruptReason_,
                             true);
        }
        return;
    }

    const qint64 maximum =
        activePersistent_ ? kPersistentTurnMaximumMs : kVisionTurnMaximumMs;
    if (turnStartedAtMs_ > 0 && now - turnStartedAtMs_ >= maximum) {
        RequestInterrupt(QStringLiteral("Assistant request reached its maximum duration."));
        return;
    }
    if (lastTurnActivityMs_ > 0 &&
        now - lastTurnActivityMs_ >= kTurnIdleTimeoutMs) {
        RequestInterrupt(QStringLiteral("Assistant request stopped after no progress."));
    }
}

QString CodexAppServerClient::AppendActiveText(const QString& delta)
{
    if (delta.isEmpty() || activeTextTruncated_) {
        return {};
    }
    const qsizetype available =
        std::max<qsizetype>(0,
                            kMaximumAnswerCharacters - activeText_.size());
    QString accepted = delta.left(available);
    activeText_ += accepted;
    if (accepted.size() < delta.size()) {
        const QString marker =
            QStringLiteral("\n\n[Answer truncated by OpenZoom for safety.]");
        if (activeText_.size() + marker.size() <=
            kMaximumAnswerCharacters + marker.size()) {
            activeText_ += marker;
            accepted += marker;
        }
        activeTextTruncated_ = true;
        emit ServerStateChanged(
            initialized_,
            QStringLiteral("Codex answer reached OpenZoom's safe display limit."));
    }
    return accepted;
}

QJsonArray CodexAppServerClient::TranscriptFromThread(const QJsonObject& thread)
{
    QJsonArray messages;
    for (const QJsonValue& turnValue : thread.value(QStringLiteral("turns")).toArray()) {
        const QJsonObject turn = turnValue.toObject();
        for (const QJsonValue& itemValue : turn.value(QStringLiteral("items")).toArray()) {
            const QJsonObject item = itemValue.toObject();
            const QString type = item.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("userMessage")) {
                QStringList textParts;
                for (const QJsonValue& contentValue : item.value(QStringLiteral("content")).toArray()) {
                    const QJsonObject content = contentValue.toObject();
                    if (content.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                        textParts.push_back(content.value(QStringLiteral("text")).toString());
                    }
                }
                if (!textParts.isEmpty()) {
                    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                                {QStringLiteral("text"),
                                                 textParts.join(QStringLiteral("\n"))
                                                     .left(kMaximumTranscriptMessageCharacters)}});
                }
            } else if (type == QStringLiteral("agentMessage")) {
                const QString text = item.value(QStringLiteral("text")).toString().trimmed();
                if (!text.isEmpty()) {
                    messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                                {QStringLiteral("text"),
                                                 text.left(kMaximumTranscriptMessageCharacters)}});
                }
            }
        }
    }
    while (messages.size() > kMaximumTranscriptMessages) {
        messages.removeFirst();
    }
    return messages;
}

QString CodexAppServerClient::FinalAgentText(const QJsonObject& turn)
{
    QString finalText;
    for (const QJsonValue& itemValue : turn.value(QStringLiteral("items")).toArray()) {
        const QJsonObject item = itemValue.toObject();
        if (item.value(QStringLiteral("type")).toString() == QStringLiteral("agentMessage")) {
            finalText = item.value(QStringLiteral("text")).toString();
        }
    }
    return finalText.trimmed();
}

} // namespace openzoom

#endif // _WIN32
