#include "okuflow/common/http_agent_client.hpp"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <algorithm>

namespace okuflow {
namespace {
constexpr int kResponseBytes = 1024 * 1024;
constexpr int kRequestBytes = 2 * 1024 * 1024;
constexpr int kArgumentBytes = 16 * 1024;
constexpr int kResultBytes = 32 * 1024;
constexpr int kImageBytes = 256 * 1024;
constexpr int kTurnImageBytes = 768 * 1024;
constexpr int kRounds = 8;
constexpr int kCalls = 32;
constexpr int kTurnMs = 180000;
constexpr int kProbeMs = 15000;
bool ContainsSecret(const QJsonValue& value,const QByteArray& key) {
    if (key.isEmpty()) return false;
    if (value.isString()) return value.toString().toUtf8().contains(key);
    if (value.isArray()) { for (const auto& item : value.toArray()) if (ContainsSecret(item,key)) return true; }
    if (value.isObject()) {
        const auto object=value.toObject();
        for (auto item=object.begin();item!=object.end();++item)
            if (item.key().toUtf8().contains(key) || ContainsSecret(item.value(),key)) return true;
    }
    return false;
}

QJsonObject Text(const QString& value) { return {{"type","text"},{"text",value}}; }
QJsonObject Image(const QByteArray& jpeg) {
    return {{"type","image_url"},{"image_url",QJsonObject{{"url",QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(jpeg.toBase64())}}}};
}
QJsonObject ProbeFunction() {
    return {{"name","okuflow_capability_test"},{"description","Return the supplied test value. This test does not operate OkuFlow."},
        {"parameters",QJsonObject{{"type","object"},{"properties",QJsonObject{{"value",QJsonObject{{"type","integer"},{"enum",QJsonArray{1}}}}}},
            {"required",QJsonArray{"value"}},{"additionalProperties",false}}}};
}
}

HttpAgentClient::HttpAgentClient(QObject* parent) : QObject(parent), network_(this) {
    deadline_.setSingleShot(true);
    connect(&deadline_,&QTimer::timeout,this,[this] { Finish({},QStringLiteral("timeout")); });
    contextTimer_.setInterval(100);
    connect(&contextTimer_,&QTimer::timeout,this,[this] {
        if (active_ && !probing_ && !IsCurrent(generation_)) Finish({},QStringLiteral("context_expired"));
    });
}
HttpAgentClient::~HttpAgentClient() {
    ++generation_;
    completion_ = {};
    capabilityCompletion_ = {};
    if (reply_) { reply_->disconnect(this); reply_->abort(); }
}
void HttpAgentClient::Configure(const HttpAgentConfig& config) {
    const auto epoch = ++configurationEpoch_;
    if (config_.endpoint != config.endpoint || config_.model != config.model ||
        config_.apiKey != config.apiKey || config_.toolMode != config.toolMode) {
        const bool changing = configurationChanging_;
        configurationChanging_ = true;
        Cancel();
        if (epoch != configurationEpoch_) { configurationChanging_ = changing; return; }
        // A credential change can change effective provider capability.
        if (config_.apiKey != config.apiKey) capabilities_.clear();
        config_ = config;
        configurationChanging_ = changing;
    }
}
void HttpAgentClient::SetControlService(agent::AppControlService* service) {
    const auto epoch = ++serviceEpoch_;
    if (service_ != service) {
        const bool changing = configurationChanging_;
        configurationChanging_ = true;
        Cancel();
        if (epoch != serviceEpoch_) { configurationChanging_ = changing; return; }
        service_ = service;
        configurationChanging_ = changing;
    }
}
QString HttpAgentClient::ProfileKey() const {
    return config_.endpoint.toString(QUrl::FullyEncoded) + QChar(0) + config_.model +
        QChar(0) + QString::number(static_cast<int>(config_.toolMode));
}
HttpAgentCapability HttpAgentClient::Capability() const { return capabilities_.value(ProfileKey()); }
bool HttpAgentClient::ConfigurationValid() const {
    const bool invalidHeader = std::any_of(config_.apiKey.begin(),config_.apiKey.end(),[](unsigned char value) {
        return value <= 32 || value >= 127;
    });
    return config_.endpoint.isValid() && !config_.endpoint.host().isEmpty() &&
        (config_.endpoint.scheme() == QStringLiteral("https") || config_.endpoint.scheme() == QStringLiteral("http")) &&
        config_.endpoint.userInfo().isEmpty() && config_.endpoint.fragment().isEmpty() &&
        !config_.model.trimmed().isEmpty() && config_.model.size() <= 256 &&
        !invalidHeader && config_.apiKey.size() <= 8192 &&
        config_.toolMode != HttpAgentToolMode::Off;
}
QJsonArray HttpAgentClient::Descriptors(bool legacy) const {
    QJsonArray result;
    if (probing_) {
        result.append(legacy ? ProbeFunction() : QJsonObject{{"type","function"},{"function",ProbeFunction()}});
        return result;
    }
    if (!service_) return result;
    for (const auto& tool : agent::ControlRegistry(service_->Edition())) {
        const auto schema = QJsonDocument::fromJson(QByteArray::fromStdString(tool.inputSchema)).object();
        QJsonObject function{{"name",QString::fromUtf8(tool.wireName.data(),static_cast<int>(tool.wireName.size()))},
            {"description",QString::fromUtf8(tool.description.data(),static_cast<int>(tool.description.size()))},{"parameters",schema}};
        result.append(legacy ? function : QJsonObject{{"type","function"},{"function",function}});
    }
    return result;
}
bool HttpAgentClient::Start(HttpAgentTurn turn, Completion completion) {
    if (active_ || configurationChanging_ || !service_ || !ConfigurationValid() || !Capability().available ||
        turn.contextId.isEmpty() || turn.originKey.isEmpty() || turn.originKey.size() > 100 || !completion) return false;
    const auto context = service_->ContextStatus();
    if (!context.value(QStringLiteral("active")).toBool() || context.value(QStringLiteral("contextId")).toString() != turn.contextId ||
        context.value(QStringLiteral("ownerKind")).toString() != QStringLiteral("agent-session") ||
        (!turn.jpeg.isEmpty() && !context.value(QStringLiteral("grants")).toObject().value(QStringLiteral("frames")).toBool())) return false;
    if (turn.jpeg.toBase64().size() > kImageBytes || turn.systemPrompt.size() > 65536 || turn.userPrompt.size() > 65536) return false;
    if (turn.textHistory.size() > 8 || turn.textHistory.size()%2 != 0 ||
        QJsonDocument(turn.textHistory).toJson(QJsonDocument::Compact).size() > 32*1024) return false;
    for (int index=0;index<turn.textHistory.size();++index) {
        const auto row = turn.textHistory[index].toObject();
        if (row.size()!=2 || row.value("role").toString() != (index%2 == 0 ? "user" : "assistant") || !row.value("content").isString()) return false;
        if (!config_.apiKey.isEmpty() && row.value("content").toString().toUtf8().contains(config_.apiKey)) return false;
    }
    if (!config_.apiKey.isEmpty() && (turn.systemPrompt.toUtf8().contains(config_.apiKey) || turn.userPrompt.toUtf8().contains(config_.apiKey))) return false;
    active_ = true;
    probing_ = false;
    ++generation_;
    rounds_ = callCount_ = nextCall_ = images_ = imageBytes_ = 0;
    seenIds_.clear(); calls_.clear(); observations_ = {}; messages_ = {};
    turn_ = std::move(turn); completion_ = std::move(completion);
    messages_.append(QJsonObject{{"role","system"},{"content",turn_.systemPrompt}});
    for (const auto& earlier : turn_.textHistory) messages_.append(earlier);
    QJsonArray content{Text(turn_.userPrompt)};
    if (!turn_.jpeg.isEmpty()) content.append(Image(turn_.jpeg));
    messages_.append(QJsonObject{{"role","user"},{"content",content}});
    turn_.jpeg.clear();
    elapsed_.start(); deadline_.start(kTurnMs); contextTimer_.start();
    Post();
    return true;
}
bool HttpAgentClient::TestCapability(CapabilityCompletion completion, bool testParallelToolCalls) {
    if (active_ || configurationChanging_ || !ConfigurationValid() || !completion) return false;
    active_ = probing_ = true;
    probingParallel_ = testParallelToolCalls;
    ++generation_; rounds_ = callCount_ = nextCall_ = 0;
    seenIds_.clear(); observations_ = {}; calls_.clear();
    capabilityCompletion_ = std::move(completion);
    capabilities_.remove(ProfileKey());
    messages_ = QJsonArray{QJsonObject{{"role","user"},{"content","Call okuflow_capability_test with value 1. This is a tool capability test, with no camera data."}}};
    elapsed_.start(); deadline_.start(kProbeMs);
    Post(); return true;
}
void HttpAgentClient::Cancel() { if (active_) Finish({},QStringLiteral("canceled"),true); }
bool HttpAgentClient::IsCurrent(quint64 generation) const {
    if (!active_ || generation != generation_) return false;
    if (probing_) return true;
    if (!service_) return false;
    const auto context = service_->ContextStatus();
    return context.value(QStringLiteral("active")).toBool() &&
        context.value(QStringLiteral("contextId")).toString() == turn_.contextId;
}
void HttpAgentClient::Post() {
    if (!active_) return;
    if (!probing_ && !service_) { Finish({},QStringLiteral("unavailable")); return; }
    if (!IsCurrent(generation_)) { Finish({},QStringLiteral("context_expired")); return; }
    if (!probing_ && !service_->ContextStatus().value(QStringLiteral("grants")).toObject().value(QStringLiteral("frames")).toBool()) {
        for (const auto& message : messages_)
            for (const auto& part : message.toObject().value(QStringLiteral("content")).toArray())
                if (part.toObject().value(QStringLiteral("type")).toString() == QStringLiteral("image_url")) {
                    Finish({},QStringLiteral("consent_denied")); return;
                }
    }
    if (++rounds_ > kRounds || elapsed_.elapsed() >= (probing_ ? kProbeMs : kTurnMs)) {
        Finish({},QStringLiteral("budget_exhausted")); return;
    }
    const bool legacy = config_.toolMode == HttpAgentToolMode::LegacyFunctions;
    QJsonObject body{{"model",config_.model},{"messages",messages_},{"stream",false},{"max_tokens",probing_ ? 128 : 2048}};
    body.insert(legacy ? QStringLiteral("functions") : QStringLiteral("tools"),Descriptors(legacy));
    if (probing_) {
        body.insert(legacy ? QStringLiteral("function_call") : QStringLiteral("tool_choice"),
            legacy ? QJsonObject{{"name","okuflow_capability_test"}} :
                QJsonObject{{"type","function"},{"function",QJsonObject{{"name","okuflow_capability_test"}}}});
    } else body.insert(legacy ? QStringLiteral("function_call") : QStringLiteral("tool_choice"),QStringLiteral("auto"));
    if (!legacy && ((probing_ && probingParallel_) || (!probing_ && Capability().parallelToolCallsAccepted)))
        body.insert(QStringLiteral("parallel_tool_calls"),false);
    const QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (bytes.size() > kRequestBytes) { Finish({},QStringLiteral("budget_exhausted")); return; }
    QNetworkRequest request(config_.endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader,QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(probing_ ? kProbeMs : 60000);
    if (!config_.apiKey.isEmpty()) request.setRawHeader("Authorization",QByteArrayLiteral("Bearer ") + config_.apiKey);
    response_.clear(); tooLarge_ = false;
    QNetworkReply* reply = network_.post(request,bytes);
    reply_ = reply;
    reply->setReadBufferSize(kResponseBytes + 1);
    const auto generation = generation_;
    connect(reply,&QNetworkReply::readyRead,this,[this,reply,generation] {
        if (!IsCurrent(generation) || reply_ != reply) return;
        response_.append(reply->readAll());
        if (response_.size() > kResponseBytes) { tooLarge_ = true; reply->abort(); }
    });
    connect(reply,&QNetworkReply::finished,this,[this,reply,generation] { ReplyFinished(generation,reply); });
}
bool HttpAgentClient::ParseCalls(const QJsonObject& message, std::vector<Call>& calls, bool probe) const {
    const bool legacy = config_.toolMode == HttpAgentToolMode::LegacyFunctions;
    if (!message.value(QStringLiteral("role")).isString() || message.value(QStringLiteral("role")).toString() != QStringLiteral("assistant")) return false;
    const auto content = message.value(QStringLiteral("content"));
    if (!content.isUndefined() && !content.isNull() && !content.isString()) return false;
    if (message.contains(legacy ? QStringLiteral("tool_calls") : QStringLiteral("function_call"))) return false;
    QJsonArray raw;
    if (legacy) {
        if (!message.contains(QStringLiteral("function_call"))) return !probe;
        if (!message.value(QStringLiteral("function_call")).isObject()) return false;
        raw.append(QJsonObject{{"function",message.value(QStringLiteral("function_call"))}});
    } else {
        if (!message.contains(QStringLiteral("tool_calls"))) return !probe;
        if (!message.value(QStringLiteral("tool_calls")).isArray()) return false;
        raw = message.value(QStringLiteral("tool_calls")).toArray();
        if (raw.isEmpty()) return !probe;
    }
    if (raw.size() > kCalls || (probe && raw.size() != 1)) return false;
    QSet<QString> ids;
    const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    for (const auto& entry : raw) {
        if (!entry.isObject()) return false;
        const auto object = entry.toObject();
        if (!legacy && (object.value(QStringLiteral("type")).toString() != QStringLiteral("function") ||
            !object.value(QStringLiteral("id")).isString())) return false;
        const QString id = legacy ? QStringLiteral("legacy-%1-0").arg(rounds_) : object.value(QStringLiteral("id")).toString();
        if (!idPattern.match(id).hasMatch() || ids.contains(id) || seenIds_.contains(id)) return false;
        ids.insert(id);
        if (!object.value(QStringLiteral("function")).isObject()) return false;
        const auto function = object.value(QStringLiteral("function")).toObject();
        if (!function.value(QStringLiteral("name")).isString()) return false;
        const QString name = function.value(QStringLiteral("name")).toString();
        const auto arguments = function.value(QStringLiteral("arguments"));
        QJsonObject parsed;
        if (arguments.isObject()) parsed = arguments.toObject();
        else if (arguments.isString()) {
            const auto bytes = arguments.toString().toUtf8();
            if (bytes.size() > kArgumentBytes) return false;
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(bytes,&error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
            parsed = document.object();
        } else return false;
        if (QJsonDocument(parsed).toJson(QJsonDocument::Compact).size() > kArgumentBytes) return false;
        if (ContainsSecret(parsed,config_.apiKey)) return false;
        if (probe) {
            if (name != QStringLiteral("okuflow_capability_test") || parsed.size() != 1 ||
                !parsed.value(QStringLiteral("value")).isDouble() || parsed.value(QStringLiteral("value")).toDouble() != 1) return false;
        } else if (!service_ || !service_->ValidateArguments(name,parsed)) return false;
        calls.push_back({id,name,parsed});
    }
    return true;
}
void HttpAgentClient::ReplyFinished(quint64 generation, QNetworkReply* reply) {
    reply->deleteLater();
    if (!active_ || generation != generation_ || reply_ != reply) return;
    if (!IsCurrent(generation)) { Finish({},QStringLiteral("context_expired")); return; }
    reply_ = nullptr;
    response_.append(reply->readAll());
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (tooLarge_ || response_.size() > kResponseBytes) { Finish({},QStringLiteral("response_too_large")); return; }
    if (status < 200 || status >= 300 || reply->error() != QNetworkReply::NoError) {
        Finish({}, status == 401 || status == 403 ? QStringLiteral("authentication_failed") :
            status == 429 ? QStringLiteral("rate_limited") : QStringLiteral("provider_unavailable")); return;
    }
    if (!config_.apiKey.isEmpty() && response_.contains(config_.apiKey)) {
        Finish({},QStringLiteral("invalid_response")); return;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(response_,&parseError);
    response_.clear();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) { Finish({},QStringLiteral("invalid_response")); return; }
    const auto choicesValue = document.object().value(QStringLiteral("choices"));
    if (!choicesValue.isArray() || choicesValue.toArray().size() != 1 || !choicesValue.toArray().first().isObject()) {
        Finish({},QStringLiteral("invalid_response")); return;
    }
    const auto choice = choicesValue.toArray().first().toObject();
    if (!choice.value(QStringLiteral("message")).isObject()) { Finish({},QStringLiteral("invalid_response")); return; }
    const auto message = choice.value(QStringLiteral("message")).toObject();
    if (ContainsSecret(message,config_.apiKey)) {
        Finish({},QStringLiteral("invalid_response")); return;
    }
    calls_.clear();
    if (!ParseCalls(message,calls_,probing_)) { Finish({},QStringLiteral("invalid_tool_round")); return; }
    if (probing_) {
        capabilities_.insert(ProfileKey(),{true,config_.toolMode == HttpAgentToolMode::Tools && probingParallel_});
        Finish(); return;
    }
    if (calls_.empty()) {
        if (choice.value(QStringLiteral("finish_reason")).toString() != QStringLiteral("stop")) { Finish({},QStringLiteral("incomplete_response")); return; }
        Finish(message.value(QStringLiteral("content")).toString()); return;
    }
    if (callCount_ + static_cast<int>(calls_.size()) > kCalls) { Finish({},QStringLiteral("budget_exhausted")); return; }
    const QString expected = config_.toolMode == HttpAgentToolMode::Tools ? QStringLiteral("tool_calls") : QStringLiteral("function_call");
    if (choice.value(QStringLiteral("finish_reason")).toString() != expected) { Finish({},QStringLiteral("incomplete_response")); return; }
    StripImages(); // only the next request carries newly obtained observations
    messages_.append(message); // preserve argument string/object and original assistant fields
    for (const auto& call : calls_) seenIds_.insert(call.id);
    callCount_ += static_cast<int>(calls_.size()); nextCall_ = 0; observations_ = {};
    RunNextCall(generation);
}
void HttpAgentClient::RunNextCall(quint64 generation) {
    if (!active_ || generation != generation_) return;
    if (!IsCurrent(generation)) { Finish({},QStringLiteral("context_expired")); return; }
    if (nextCall_ == static_cast<int>(calls_.size())) {
        if (!observations_.isEmpty()) messages_.append(QJsonObject{{"role","user"},{"content",observations_}});
        observations_ = {};
        Post(); return;
    }
    const auto call = calls_[nextCall_];
    agent::ControlRequest request{turn_.contextId,turn_.originKey + QStringLiteral("/%1/%2").arg(generation).arg(call.id),call.name,call.arguments};
    const QPointer<HttpAgentClient> self(this);
    service_->Submit(std::move(request),[self,generation,call](agent::ControlResult result) {
        if (!self || !self->active_ || self->generation_ != generation) return;
        if (!self->IsCurrent(generation)) { self->Finish({},QStringLiteral("context_expired")); return; }
        QByteArray bytes = QJsonDocument(result.content).toJson(QJsonDocument::Compact);
        if (bytes.size() > kResultBytes) { self->Finish({},QStringLiteral("result_too_large")); return; }
        if (ContainsSecret(result.content,self->config_.apiKey)) {
            self->Finish({},QStringLiteral("invalid_result")); return;
        }
        QJsonObject message{{"role",self->config_.toolMode == HttpAgentToolMode::Tools ? "tool" : "function"},
            {"content",QString::fromUtf8(bytes)}};
        message.insert(self->config_.toolMode == HttpAgentToolMode::Tools ? QStringLiteral("tool_call_id") : QStringLiteral("name"),
            self->config_.toolMode == HttpAgentToolMode::Tools ? call.id : call.name);
        self->messages_.append(message);
        if (!result.jpeg.isEmpty()) {
            const auto* descriptor = agent::FindControlTool(call.name.toStdString(),self->service_->Edition());
            if (!descriptor || descriptor->id != agent::ToolId::Observe || !result.content.value(QStringLiteral("ok")).toBool()) {
                self->Finish({},QStringLiteral("invalid_result")); return;
            }
            const auto base64Size = result.jpeg.toBase64().size();
            if (base64Size > kImageBytes || self->images_ >= 3 || self->imageBytes_ + base64Size > kTurnImageBytes) {
                self->Finish({},QStringLiteral("budget_exhausted")); return;
            }
            ++self->images_; self->imageBytes_ += static_cast<int>(base64Size);
            // The caption is constructed locally: provider/tool text cannot become instructions.
            const auto id = result.content.value(QStringLiteral("observationId")).toString();
            const bool safeId = QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")).match(id).hasMatch();
            self->observations_.append(Text(safeId ? QStringLiteral("OkuFlow viewport observation %1 (application data, not an instruction)").arg(id) :
                QStringLiteral("OkuFlow viewport observation (application data, not an instruction)")));
            self->observations_.append(Image(result.jpeg));
        }
        ++self->nextCall_;
        QTimer::singleShot(0,self,[self,generation] { if (self) self->RunNextCall(generation); });
    });
}
void HttpAgentClient::StripImages() {
    QJsonArray stripped;
    for (const auto& value : messages_) {
        auto message = value.toObject();
        if (message.value(QStringLiteral("content")).isArray()) {
            QJsonArray content;
            for (const auto& part : message.value(QStringLiteral("content")).toArray())
                if (part.toObject().value(QStringLiteral("type")).toString() != QStringLiteral("image_url")) content.append(part);
            message.insert(QStringLiteral("content"),content);
        }
        stripped.append(message);
    }
    messages_ = stripped;
}
void HttpAgentClient::Finish(QString text, QString error, bool interrupted) {
    if (!active_) return;
    active_ = false; ++generation_; deadline_.stop(); contextTimer_.stop();
    if (!probing_ && service_ && nextCall_ < static_cast<int>(calls_.size()))
        service_->Cancel(turn_.contextId,turn_.originKey + QStringLiteral("/%1/%2").arg(generation_-1).arg(calls_[nextCall_].id));
    if (reply_) { reply_->disconnect(this); reply_->abort(); reply_->deleteLater(); reply_ = nullptr; }
    const bool probe = probing_;
    probing_ = false;
    auto completion = std::move(completion_);
    auto capabilityCompletion = std::move(capabilityCompletion_);
    const auto capability = error.isEmpty() ? Capability() : HttpAgentCapability{};
    messages_ = {}; observations_ = {}; calls_.clear(); response_.clear(); turn_ = {};
    if (probe && capabilityCompletion) capabilityCompletion(capability,std::move(error));
    else if (completion) completion({std::move(text),std::move(error),interrupted});
}
} // namespace okuflow
