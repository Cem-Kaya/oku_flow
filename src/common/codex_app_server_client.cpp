#ifdef _WIN32

#include "okuflow/common/codex_app_server_client.hpp"
#include "okuflow/common/codex_agent_profile.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace okuflow {

namespace {

constexpr auto kAssistantIdentity =
    "You are the assistant inside OkuFlow, an accessibility magnifier for people with low vision. "
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
    connect(&restrictedPreflight_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,
        [this](int,QProcess::ExitStatus) {
            // A retired preflight can finish after a new turn has selected a
            // different immutable child profile. Resume only current work.
            if(!restrictedPreflightActive_ && pendingTurn_.valid && !rpc_->IsRunning())
                QTimer::singleShot(0,this,[this] {if(pendingTurn_.valid)Start();});
        });
    restrictedPreflightDeadline_=new QTimer(this);restrictedPreflightDeadline_->setSingleShot(true);
    connect(restrictedPreflightDeadline_,&QTimer::timeout,this,[this]{FailRestrictedPreflight("preflight_timeout");});
    connect(&restrictedPreflight_,&QProcess::readyReadStandardError,this,[this]{restrictedPreflight_.readAllStandardError();});
    connect(&restrictedPreflight_,&QProcess::readyReadStandardOutput,this,[this] {
        const auto bytes=restrictedPreflight_.readAllStandardOutput();
        if(!restrictedPreflightActive_)return;
        if(restrictedPreflightBytes_.size()+bytes.size()>512*1024) {FailRestrictedPreflight("preflight_too_large");return;}
        restrictedPreflightBytes_+=bytes;
    });
    connect(&restrictedPreflight_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error) {
        if(error==QProcess::FailedToStart && restrictedPreflightActive_)FailRestrictedPreflight("codex_unavailable");
    });

    rpc_->SetServerRequestHandler(
        [this](const QJsonValue& id, const QString& method, const QJsonObject& params) {
            return HandleServerRequest(id, method, params);
        });
    connect(rpc_.get(), &CodexJsonRpcProcess::Started, this, [this]() {
        emit ServerStateChanged(false, QStringLiteral("Connecting to Codex..."));
        QJsonObject clientInfo{
            {QStringLiteral("name"), QStringLiteral("okuflow")},
            {QStringLiteral("title"), QStringLiteral("OkuFlow")},
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
    connect(rpc_.get(), &CodexJsonRpcProcess::StderrText, this, [this](const QString& diagnostic) {
        if(launchCoding_)qWarning().noquote() << "codex app-server:" << diagnostic;
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
        const quint64 finishedGeneration = pendingTurn_.valid
                                               ? pendingTurn_.generation : activeGeneration_;
        initialized_ = false;
        signedIn_ = false;
        rpc_->FailAllPendingReplies(QStringLiteral("Codex stopped before replying."));
        if (retiringFailedServer_) {
            // FinishInitialization reports the failure and owns the request.
            return;
        }
        if (wasInitialized || exitCode != 0) {
            emit ServerStateChanged(
                false,
                QStringLiteral("Codex stopped (exit code %1).").arg(exitCode));
        }
        const quint64 currentGeneration = pendingTurn_.valid
                                              ? pendingTurn_.generation : activeGeneration_;
        if (finishedGeneration != 0 && currentGeneration == finishedGeneration && IsTurnActive()) {
            if (interruptRequested_) {
                // Exiting while cancellation was pending completes it.
                FinishActiveTurn({}, interruptReason_, true);
            } else {
                FinishActiveTurn({}, QStringLiteral("Codex stopped before the answer completed."), false);
            }
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
    const bool oldCoding=codingEnabled_, oldInternet=internetEnabled_;
    const QString oldWorkspace=workspaceDirectory_;
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
    if (executableChanged || oldCoding!=codingEnabled_ || oldInternet!=internetEnabled_ || oldWorkspace!=workspaceDirectory_) {
        Shutdown();
        // Preserve a wider child's identity if it refuses retirement; a later
        // turn must not mistake that process for a new constrained child.
        if(!rpc_->IsRunning())launchCoding_=launchInternet_=false;
    }
}

void CodexAppServerClient::Start()
{
    if (rpc_->IsRunning() || restrictedPreflightActive_ || restrictedPreflight_.state()!=QProcess::NotRunning) {
        return;
    }

    if(!pendingTurn_.valid)launchCoding_=launchInternet_=false;
    initialized_ = false;
    accountReadInFlight_ = false;
    retiredTurnIds_.clear();
    emit ServerStateChanged(false, QStringLiteral("Starting Codex..."));
    if(!launchCoding_) {StartRestrictedPreflight(0);return;}
    // Coding is a separately requested profile, never inherited by vision.
    // Thread/turn policies retain the exact configured workspace and grants.
    rpc_->SetProgram(ResolveExecutable(),
                     {QStringLiteral("app-server"),QStringLiteral("--listen"),QStringLiteral("stdio://"),
                      QStringLiteral("-c"),QStringLiteral("approval_policy=\"never\""),
                      QStringLiteral("-c"),QStringLiteral("sandbox_mode=\"workspace-write\""),
                      QStringLiteral("-c"),QStringLiteral("sandbox_workspace_write.writable_roots=%1").arg(
                          QString::fromUtf8(QJsonDocument(QJsonArray{workspaceDirectory_}).toJson(QJsonDocument::Compact))),
                      QStringLiteral("-c"),QStringLiteral("sandbox_workspace_write.network_access=%1").arg(launchInternet_?"true":"false"),
                      QStringLiteral("-c"),launchInternet_?QStringLiteral("web_search=\"live\""):QStringLiteral("web_search=\"disabled\"")});
    auto environment=QProcessEnvironment::systemEnvironment();environment.remove("OKUFLOW_VLM_API_KEY");
    rpc_->SetProcessEnvironment(environment);
    rpc_->Start();
}

void CodexAppServerClient::Shutdown()
{
    const bool hadPreflight=restrictedPreflightActive_;
    ++restrictedPreflightEpoch_;restrictedPreflightActive_=false;
    restrictedPreflightDeadline_->stop();restrictedPreflightBytes_.clear();restrictedVersion_.clear();restrictedFeatures_.clear();
    if(restrictedPreflight_.state()!=QProcess::NotRunning)restrictedPreflight_.kill();
    if(hadPreflight && IsTurnActive())FinishActiveTurn({},QStringLiteral("canceled"),true);
    if (!rpc_->IsRunning()) {
        return;
    }
    if (IsTurnActive()) {
        InterruptTurn();
    }
    rpc_->Shutdown();
}

void CodexAppServerClient::SetRestrictedLaunchMode(bool restricted)
{
    if(restricted==restrictedLaunch_)return;
    Shutdown();restrictedLaunch_=restricted;
    if(!rpc_->IsRunning())launchCoding_=launchInternet_=false;
}

bool CodexAppServerClient::EnsureLaunchPermissions(bool coding,bool internet)
{
    coding=coding && !restrictedLaunch_;internet=internet && !restrictedLaunch_;
    if(launchCoding_==coding && launchInternet_==internet)return true;
    Shutdown();
    if(rpc_->IsRunning())return false; // Never submit through an unretired wider child.
    launchCoding_=coding;launchInternet_=internet;
    return true;
}

void CodexAppServerClient::FailRestrictedPreflight(const QString& code)
{
    if(!restrictedPreflightActive_)return;
    const auto owner=pendingTurn_.valid?pendingTurn_.generation:activeGeneration_;
    ++restrictedPreflightEpoch_;restrictedPreflightActive_=false;restrictedPreflightDeadline_->stop();
    restrictedPreflightBytes_.clear();restrictedVersion_.clear();restrictedFeatures_.clear();
    if(restrictedPreflight_.state()!=QProcess::NotRunning)restrictedPreflight_.kill();
    emit ServerStateChanged(false,code);
    const auto current=pendingTurn_.valid?pendingTurn_.generation:activeGeneration_;
    if(owner && current==owner && IsTurnActive())FinishActiveTurn({},code,false);
}

void CodexAppServerClient::StartRestrictedPreflight(int stage)
{
    restrictedPreflightActive_=true;
    const auto epoch=++restrictedPreflightEpoch_;
    disconnect(restrictedStageFinished_);
    restrictedPreflightBytes_.clear();
    restrictedStageFinished_=connect(&restrictedPreflight_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,
        [this,stage,epoch](int exit,QProcess::ExitStatus status) {
            if(epoch!=restrictedPreflightEpoch_ || !restrictedPreflightActive_)return;
            restrictedPreflightDeadline_->stop();
            restrictedPreflightBytes_+=restrictedPreflight_.readAllStandardOutput();
            if(exit!=0 || status!=QProcess::NormalExit || restrictedPreflightBytes_.size()>512*1024) {FailRestrictedPreflight("preflight_failed");return;}
            if(stage==0) {restrictedVersion_=QString::fromUtf8(restrictedPreflightBytes_);StartRestrictedPreflight(1);return;}
            if(stage==1) {restrictedFeatures_=QString::fromUtf8(restrictedPreflightBytes_);StartRestrictedPreflight(2);return;}
            QJsonParseError error;const auto inventory=QJsonDocument::fromJson(restrictedPreflightBytes_,&error);
            if(error.error!=QJsonParseError::NoError || !inventory.isArray()) {FailRestrictedPreflight("invalid_mcp_inventory");return;}
            const auto profile=CodexAgentProfile::Validate(restrictedVersion_,restrictedFeatures_,inventory.array(),launchInternet_);
            restrictedPreflightBytes_.clear();restrictedVersion_.clear();restrictedFeatures_.clear();
            if(!profile.available) {FailRestrictedPreflight(profile.errorCode);return;}
            restrictedPreflightActive_=false;
            rpc_->SetProgram(ResolveExecutable(),profile.arguments);
            auto environment=QProcessEnvironment::systemEnvironment();
            environment.remove("OKUFLOW_VLM_API_KEY");rpc_->SetProcessEnvironment(environment);
            rpc_->Start();
        });
    QStringList args;
    if(stage==0)args={"--version"};else if(stage==1)args={"features","list"};else args={"mcp","list","--json"};
    auto environment=QProcessEnvironment::systemEnvironment();environment.remove("OKUFLOW_VLM_API_KEY");
    restrictedPreflight_.setProcessEnvironment(environment);
    restrictedPreflightDeadline_->start(5000);restrictedPreflight_.start(ResolveExecutable(),args);
}

void CodexAppServerClient::SetRequestTimeoutMs(int timeoutMs)
{
    rpc_->SetRequestTimeoutMs(timeoutMs);
}

bool CodexAppServerClient::IsReady() const { return initialized_; }
bool CodexAppServerClient::IsTurnActive() const
{
    return pendingTurn_.valid || !activeThreadId_.isEmpty();
}
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
    if (accountReadInFlight_) return;
    accountReadInFlight_ = true;
    SendRequest(QStringLiteral("account/read"),
                QJsonObject{{QStringLiteral("refreshToken"), false}},
                [this](const QJsonObject& result, const QJsonObject& error) {
                    accountReadInFlight_ = false;
                    if (retiringFailedServer_) return;
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
                                             bool persistent,
                                             bool retainThread,
                                             bool permitCoding,
                                             std::function<bool()> frameAuthorization)
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
    if (persistent && permitCoding && codingEnabled_) {
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
    CancelConversationLoad();
    const bool allowCoding=persistent && permitCoding && codingEnabled_ && !restrictedLaunch_;
    const bool allowInternet=persistent && internetEnabled_ && !restrictedLaunch_;
    if(!EnsureLaunchPermissions(allowCoding,allowInternet)) {
        if(!imagePath.trimmed().isEmpty())QFile::remove(imagePath);
        emit TurnFinished(threadId,{},{},QStringLiteral("Codex did not stop before changing permissions."),false,persistent);
        return;
    }
    pendingTurn_.generation = ++lastRequestGeneration_;
    pendingTurn_.opening = false;
    turnStartedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    lastTurnActivityMs_ = turnStartedAtMs_;
    turnWatchdogTimer_->start();
    pendingTurn_.prompt = prompt.trimmed();
    pendingTurn_.imagePath = imagePath;
    pendingTurn_.frameAuthorization = std::move(frameAuthorization);
    // Read is isolated even if a caller supplies a remembered conversation.
    pendingTurn_.threadId = persistent || retainThread ? threadId.trimmed() : QString();
    pendingTurn_.persistent = persistent;
    pendingTurn_.allowCoding = allowCoding;
    pendingTurn_.allowInternet = allowInternet;
    pendingTurn_.workspace = workspaceDirectory_;
    pendingTurn_.retainThread = !persistent && retainThread;
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
    if (!IsTurnActive() || interruptRequested_) {
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
    if (activeTurnStartPending_ && activeTurnId_.isEmpty()) {
        return; // Keep ownership until start supplies a turn id or grace expires.
    }
    if (activeThreadId_.isEmpty() || activeTurnId_.isEmpty()) {
        FinishActiveTurn({}, reason, true);
        return;
    }
    SendRequest(QStringLiteral("turn/interrupt"),
                QJsonObject{{QStringLiteral("threadId"), activeThreadId_},
                            {QStringLiteral("turnId"), activeTurnId_}},
                [this, generation = activeGeneration_](const QJsonObject&, const QJsonObject& error) {
                    if (!error.isEmpty() && activeGeneration_ == generation && IsTurnActive()) {
                        emit ServerStateChanged(
                            initialized_,
                            ErrorMessage(error,
                                         QStringLiteral("Codex did not acknowledge cancellation.")));
                    }
                });
}

void CodexAppServerClient::CancelConversationLoad()
{
    ++conversationLoadGeneration_;
}

void CodexAppServerClient::LoadConversation(const QString& threadId)
{
    const quint64 generation = ++conversationLoadGeneration_;
    if (!initialized_) {
        emit ConversationTranscriptLoadFailed(threadId, QStringLiteral("Could not load conversation."));
        return;
    }
    SendRequest(QStringLiteral("thread/read"),
                QJsonObject{{QStringLiteral("threadId"), threadId},
                            {QStringLiteral("includeTurns"), true}},
                [this, threadId, generation](const QJsonObject& result, const QJsonObject& error) {
                    if (generation != conversationLoadGeneration_) return;
                    if (!error.isEmpty()) {
                        const QString message = ErrorMessage(error, QStringLiteral("Could not load conversation."));
                        emit ConversationTranscriptLoadFailed(threadId, message);
                        emit ServerStateChanged(initialized_, message);
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
    const QString environmentPath = qEnvironmentVariable("OKUFLOW_CODEX_PATH").trimmed();
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
    if (persistent && pendingTurn_.allowCoding && QFileInfo(pendingTurn_.workspace).isDir()) {
        return pendingTurn_.workspace;
    }
    QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (temp.isEmpty()) {
        temp = QCoreApplication::applicationDirPath();
    }
    QDir dir(temp);
    dir.mkpath(QStringLiteral("OkuFlow/assistant"));
    return dir.filePath(QStringLiteral("OkuFlow/assistant"));
}

QString CodexAppServerClient::DeveloperInstructions(bool persistent) const
{
    const bool allowInternet = persistent && (pendingTurn_.valid ? pendingTurn_.allowInternet : activeInternetEnabled_);
    const bool allowCoding = persistent && (pendingTurn_.valid ? pendingTurn_.allowCoding : activeCodingEnabled_);
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
                            .arg(pendingTurn_.workspace);
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
    const bool allowInternet = persistent && (pendingTurn_.valid ? pendingTurn_.allowInternet : activeInternetEnabled_);
    if (persistent && (pendingTurn_.valid ? pendingTurn_.allowCoding : activeCodingEnabled_)) {
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("workspaceWrite")},
            {QStringLiteral("networkAccess"), allowInternet},
            {QStringLiteral("writableRoots"),
             QJsonArray{QDir::toNativeSeparators(pendingTurn_.workspace)}}};
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
    const bool turnEvent = method.startsWith(QStringLiteral("item/")) ||
                           method.startsWith(QStringLiteral("turn/")) ||
                           method == QStringLiteral("error");
    if (turnEvent && !NotificationTargetsActiveTurn(params)) {
        return;
    }
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
            if (interruptRequested_) {
                SendRequest(QStringLiteral("turn/interrupt"),
                            QJsonObject{{QStringLiteral("threadId"), threadId},
                                        {QStringLiteral("turnId"), turnId}}, {});
                return;
            }
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
        activeTurnId_ = turnId;
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
                                               const QJsonObject& params)
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
    if (NotificationTargetsActiveTurn(params)) {
        InterruptTurn();
    }
    return true;
}

void CodexAppServerClient::FinishInitialization(const QJsonObject& result,
                                                const QJsonObject& error)
{
    if (retiringFailedServer_) return;
    if (!error.isEmpty()) {
        initialized_ = false;
        signedIn_ = false;
        const QString reason = ErrorMessage(error, QStringLiteral("Codex initialization failed."));
        retiringFailedServer_ = true;
        // Retire the child before publishing completion: a completion listener
        // may immediately admit a replacement and start a fresh child.
        rpc_->Shutdown();
        retiringFailedServer_ = false;
        emit ServerStateChanged(false, reason);
        FinishActiveTurn({}, reason, false);
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

bool CodexAppServerClient::PendingTurnCurrent(quint64 generation) const
{
    return !retiringFailedServer_ && pendingTurn_.valid && pendingTurn_.generation == generation;
}

bool CodexAppServerClient::NotificationTargetsActiveTurn(const QJsonObject& params) const
{
    if (retiringFailedServer_ || activeGeneration_ == 0 || activeThreadId_.isEmpty()) return false;
    const QString thread = params.value(QStringLiteral("threadId")).toString();
    QString turn = params.value(QStringLiteral("turnId")).toString();
    if (turn.isEmpty()) turn = params.value(QStringLiteral("turn")).toObject().value(QStringLiteral("id")).toString();
    if (!turn.isEmpty() && retiredTurnIds_.contains(turn)) return false;
    // Unknown/unscoped traffic is diagnostic-only; it cannot own a turn.
    return thread == activeThreadId_ && !turn.isEmpty() &&
           (activeTurnId_.isEmpty() || turn == activeTurnId_);
}

void CodexAppServerClient::RetireTurnId(const QString& turnId)
{
    if (turnId.isEmpty() || retiredTurnIds_.contains(turnId)) return;
    retiredTurnIds_.append(turnId);
    while (retiredTurnIds_.size() > kMaximumRetiredTurnIds) retiredTurnIds_.removeFirst();
}

void CodexAppServerClient::SubmitPendingTurn()
{
    if (!pendingTurn_.valid || pendingTurn_.opening || !initialized_ || !signedIn_) {
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
    pendingTurn_.opening = true;
    const quint64 generation = pendingTurn_.generation;
    const bool persistent = pendingTurn_.persistent;
    const bool allowCoding = persistent && (pendingTurn_.valid ? pendingTurn_.allowCoding : activeCodingEnabled_);
    QJsonObject params{
        {QStringLiteral("cwd"), AssistantWorkingDirectory(persistent)},
        {QStringLiteral("approvalPolicy"), QStringLiteral("never")},
        {QStringLiteral("sandbox"), allowCoding ? QStringLiteral("workspace-write")
                                                : QStringLiteral("read-only")},
        {QStringLiteral("ephemeral"), !persistent && !pendingTurn_.retainThread},
        {QStringLiteral("serviceName"), QStringLiteral("okuflow")},
        {QStringLiteral("developerInstructions"), DeveloperInstructions(persistent)}};
    if (!selectedModel_.isEmpty()) {
        params.insert(QStringLiteral("model"), selectedModel_);
    }
    SendRequest(QStringLiteral("thread/start"), params,
                [this, generation](const QJsonObject& result, const QJsonObject& error) {
                    if (!PendingTurnCurrent(generation)) return;
                    if (!error.isEmpty()) {
                        FinishActiveTurn({}, ErrorMessage(error, QStringLiteral("Could not start assistant conversation.")), false);
                        return;
                    }
                    QJsonObject thread = result.value(QStringLiteral("thread")).toObject();
                    const QString threadId = thread.value(QStringLiteral("id")).toString();
                    if (threadId.isEmpty()) {
                        FinishActiveTurn({}, QStringLiteral("Codex returned no conversation identifier."), false);
                        return;
                    }
                    pendingTurn_.threadId = threadId;
                    thread.insert(QStringLiteral("kind"),pendingTurn_.allowCoding ? QStringLiteral("coding") : QStringLiteral("assistant"));
                    if (pendingTurn_.persistent) {
                        emit ConversationCreated(thread);
                    } else if (pendingTurn_.retainThread) {
                        emit RetainedThreadStarted(thread);
                    }
                    StartTurnOnThread(threadId);
                });
}

void CodexAppServerClient::ResumeThreadForPendingTurn()
{
    pendingTurn_.opening = true;
    const quint64 generation = pendingTurn_.generation;
    const bool persistent = pendingTurn_.persistent;
    const bool allowCoding = persistent && (pendingTurn_.valid ? pendingTurn_.allowCoding : activeCodingEnabled_);
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
                [this, generation, threadId = pendingTurn_.threadId](const QJsonObject&, const QJsonObject& error) {
                    if (!PendingTurnCurrent(generation) || pendingTurn_.threadId != threadId) return;
                    if (!error.isEmpty()) {
                        if (pendingTurn_.retainThread && initialized_ && signedIn_) {
                            // A deleted, expired, or unreadable Explain thread
                            // is replaced rather than failing the request.
                            qInfo().noquote()
                                << "Codex: starting a new Explain conversation; resume failed:"
                                << ErrorMessage(error, QStringLiteral("unknown error"));
                            pendingTurn_.threadId.clear();
                            pendingTurn_.opening = false;
                            StartNewThreadForPendingTurn();
                            return;
                        }
                        FinishActiveTurn({}, ErrorMessage(error, QStringLiteral("Could not resume assistant conversation.")), false);
                        return;
                    }
                    StartTurnOnThread(threadId);
                });
}

void CodexAppServerClient::StartTurnOnThread(QString threadId)
{
    if (!pendingTurn_.valid || pendingTurn_.threadId != threadId) return;
    if (pendingTurn_.frameAuthorization && !pendingTurn_.frameAuthorization()) {
        FinishActiveTurn({},QStringLiteral("Frame permission expired."),true);
        return;
    }
    const quint64 generation = pendingTurn_.generation;
    activeGeneration_ = generation;
    activeTurnStartPending_ = true;
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
    activeInternetEnabled_ = persistent && (pendingTurn_.valid ? pendingTurn_.allowInternet : activeInternetEnabled_);
    activeCodingEnabled_ = persistent && (pendingTurn_.valid ? pendingTurn_.allowCoding : activeCodingEnabled_);
    activeImagePath_ = pendingTurn_.imagePath;
    activeText_.clear();
    activeTextTruncated_ = false;
    activeTurnId_.clear();
    lastTurnActivityMs_ = QDateTime::currentMSecsSinceEpoch();
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
                [this, threadId, persistent, generation](const QJsonObject& result, const QJsonObject& error) {
                    if (retiringFailedServer_) return;
                    const QString returnedId = result.value(QStringLiteral("turn")).toObject()
                                                   .value(QStringLiteral("id")).toString();
                    if (activeGeneration_ != generation || activeThreadId_ != threadId) {
                        if (error.isEmpty() && !returnedId.isEmpty()) {
                            RetireTurnId(returnedId);
                            SendRequest(QStringLiteral("turn/interrupt"),
                                        QJsonObject{{QStringLiteral("threadId"), threadId},
                                                    {QStringLiteral("turnId"), returnedId}}, {});
                        }
                        return;
                    }
                    activeTurnStartPending_ = false;
                    if (!error.isEmpty()) {
                        const QString reason = ErrorMessage(error, QStringLiteral("Could not start assistant request."));
                        const bool interrupted = interruptRequested_;
                        if (error.value(QStringLiteral("code")).toInt() == -32001) {
                            // A missing acknowledgement does not prove that the
                            // remote turn did not start. Retire its child before
                            // making this thread available to a replacement.
                            retiringFailedServer_ = true;
                            rpc_->Shutdown();
                            retiringFailedServer_ = false;
                        }
                        FinishActiveTurn({}, reason, interrupted);
                        return;
                    }
                    activeTurnId_ = result.value(QStringLiteral("turn")).toObject()
                                            .value(QStringLiteral("id")).toString();
                    if (activeTurnId_.isEmpty()) {
                        FinishActiveTurn({}, QStringLiteral("Codex returned no turn identifier."), false);
                        return;
                    }
                    if (interruptRequested_) {
                        SendRequest(QStringLiteral("turn/interrupt"),
                                    QJsonObject{{QStringLiteral("threadId"), threadId},
                                                {QStringLiteral("turnId"), activeTurnId_}}, {});
                        return;
                    }
                    emit TurnStarted(threadId, activeTurnId_, persistent);
                });
}

void CodexAppServerClient::FinishActiveTurn(const QString& text,
                                            const QString& error,
                                            bool interrupted)
{
    if (!IsTurnActive()) return;
    RetireTurnId(activeTurnId_);
    activeGeneration_ = 0;
    activeTurnStartPending_ = false;
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
            const QString reason = interruptReason_;
            retiringFailedServer_ = true;
            rpc_->Shutdown();
            retiringFailedServer_ = false;
            FinishActiveTurn({}, reason.isEmpty()
                                     ? QStringLiteral("Assistant cancellation timed out.")
                                     : reason,
                             true);
        }
        return;
    }

    const qint64 maximum =
        (pendingTurn_.valid ? pendingTurn_.persistent : activePersistent_)
            ? kPersistentTurnMaximumMs : kVisionTurnMaximumMs;
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
            QStringLiteral("\n\n[Answer truncated by OkuFlow for safety.]");
        if (activeText_.size() + marker.size() <=
            kMaximumAnswerCharacters + marker.size()) {
            activeText_ += marker;
            accepted += marker;
        }
        activeTextTruncated_ = true;
        emit ServerStateChanged(
            initialized_,
            QStringLiteral("Codex answer reached OkuFlow's safe display limit."));
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

} // namespace okuflow

#endif // _WIN32
