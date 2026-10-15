#include "okuflow/common/visible_chat_session.hpp"
#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <vector>

namespace okuflow {
namespace {
QString Id() { return QUuid::createUuid().toString(QUuid::Id128); }
constexpr int kTextCharacters = 100000;
}
QString VisibleChatSession::KindName(ChatBackendKind kind) {
    switch (kind) {
    case ChatBackendKind::Explain:return QStringLiteral("explain");
    case ChatBackendKind::Agent:return QStringLiteral("agent");
    case ChatBackendKind::Assistant:return QStringLiteral("assistant");
    case ChatBackendKind::Coding:return QStringLiteral("coding");
    }
    return QStringLiteral("explain");
}
ChatBackendKind VisibleChatSession::NewConversationKind(bool subscriptionProvider,bool embeddedControl,bool codingEnabled) {
    if (!subscriptionProvider || embeddedControl) return ChatBackendKind::Explain;
    return codingEnabled ? ChatBackendKind::Coding : ChatBackendKind::Assistant;
}
VisibleChatSession::VisibleChatSession(QObject* parent) : QObject(parent) { New(); }
QJsonObject VisibleChatSession::Snapshot() const {
    return {{"conversationId",conversationId_},{"kind",KindName(visibleKind_)},{"title",QString()},
        {"messages",messages_},{"draft",draft_},{"busy",Busy()},{"attachCurrentView",attach_}};
}
void VisibleChatSession::Changed() { emit SnapshotChanged(Snapshot()); }
void VisibleChatSession::Prune() {
    for (;;) {
        qsizetype characters = 0;
        for (const auto& row : messages_) characters += row.toObject().value("text").toString().size();
        if (messages_.size() <= 2 || (messages_.size() <= 128 && characters <= 256*1024)) break;
        messages_.removeFirst(); messages_.removeFirst();
        contextStartRow_ = std::max(0,contextStartRow_-2);
        if (active_) active_->assistantRow -= 2;
    }
    for (auto it = receiptRows_.begin();it != receiptRows_.end();) {
        bool present = false;
        for (const auto& value : messages_) present |= value.toObject().value("id").toString() == it->second;
        if (!present) it = receiptRows_.erase(it); else ++it;
    }
}
void VisibleChatSession::SetDraft(const QString& draft) {
    const auto bounded = draft.left(65536);
    if (bounded != draft_) { draft_ = bounded; Changed(); }
}
void VisibleChatSession::SetAttachCurrentView(bool attach) {
    if (attach_ != attach) { attach_ = attach; Changed(); }
}
void VisibleChatSession::New(ChatBackendKind kind) {
    ++epoch_; ++generation_; conversationId_ = Id(); visibleKind_ = kind;
    localLogical_ = true;
    active_.reset(); threads_.clear(); receiptRows_.clear(); messages_ = {}; draft_.clear(); attach_ = false;
    contextStartRow_ = 0; lastCompletedBackend_.reset(); Changed();
}
void VisibleChatSession::Restore(const QString& id,ChatBackendKind kind,const QJsonArray& messages) {
    New(kind); if (!id.isEmpty()) conversationId_ = id;
    localLogical_ = false;
    for (int index = std::max(0,static_cast<int>(messages.size())-128);index<messages.size();++index) {
        const auto& value = messages[index];
        const auto source = value.toObject(); const auto role = source.value("role").toString();
        if (role != "user" && role != "assistant") continue;
        const auto text = source.value("text").toString().left(kTextCharacters);
        if (text.isEmpty()) continue;
        const auto storedStatus = source.value("status").toString();
        const auto status = storedStatus == "failed" || storedStatus == "interrupted" ? storedStatus : QStringLiteral("completed");
        QJsonObject row{{"id",Id()},{"role",role},{"text",text},{"status",status},
            {"complete",status == "completed"},{"updatedAt",std::max<qint64>(0,source.value("updatedAt").toInteger(QDateTime::currentMSecsSinceEpoch()))}};
        if (role == "assistant") {
            QJsonArray actions;
            for (const auto& action : source.value("actions").toArray()) {
                if (action.isString()) actions.append(action.toString().left(2048));
                if (actions.size() >= 32) break;
            }
            if (!actions.isEmpty()) row["actions"] = actions;
            if (status != "completed") row["errorCode"] = status == "interrupted" ? "canceled" : "request_failed";
        }
        if (source.value("origin").toString()=="explain") row["origin"]="explain";
        messages_.append(row);
        if (messages_.size() >= 128) break;
    }
    Prune(); Changed();
}
void VisibleChatSession::RestoreLogical(const QString& id,const QJsonArray& messages,const QString& draft) {
    Restore(id,ChatBackendKind::Explain,messages);localLogical_=true;SetDraft(draft);Changed();
}
std::optional<ChatTurnTicket> VisibleChatSession::Admit(const QString& text,ChatBackendKind backend,ChatRequestOrigin origin) {
    if (active_ || text.trimmed().isEmpty() || text.size() > 65536) return {};
    ChatTurnTicket ticket{conversationId_,epoch_,++generation_};
    QJsonObject user{{"id",Id()},{"role","user"},{"text",text.trimmed()},
        {"status","completed"},{"complete",true},{"updatedAt",QDateTime::currentMSecsSinceEpoch()}};
    QJsonObject assistant{{"id",Id()},{"role","assistant"},{"text",QString()},
        {"status","pending"},{"complete",false},{"updatedAt",QDateTime::currentMSecsSinceEpoch()}};
    if (origin==ChatRequestOrigin::ExplainButton) {user["origin"]="explain";assistant["origin"]="explain";}
    messages_.append(user);messages_.append(assistant);
    active_ = Active{ticket,backend,static_cast<int>(messages_.size())-1,{}};
    receiptRows_[ticket.requestGeneration] = messages_.last().toObject().value("id").toString();
    draft_.clear(); Prune(); Changed(); return ticket;
}
bool VisibleChatSession::Current(const ChatTurnTicket& ticket) const { return active_ && active_->ticket == ticket; }
bool VisibleChatSession::OwnsTicket(const ChatTurnTicket& ticket) const {
    return ticket.conversationId == conversationId_ && ticket.epoch == epoch_ && receiptRows_.contains(ticket.requestGeneration);
}
std::optional<ChatTurnTicket> VisibleChatSession::ActiveTicket() const {
    return active_ ? std::optional<ChatTurnTicket>(active_->ticket) : std::nullopt;
}
bool VisibleChatSession::Bind(const ChatTurnTicket& ticket,const QString& thread,const QString& turn) {
    if (!Current(ticket) || thread.isEmpty() || turn.isEmpty()) return false;
    if (!active_->turnId.isEmpty() && active_->turnId != turn) return false;
    active_->turnId = turn; threads_[active_->backend] = thread; return true;
}
bool VisibleChatSession::AppendDelta(const ChatTurnTicket& ticket,const QString& delta) {
    if (!Current(ticket)) return false;
    auto row = messages_[active_->assistantRow].toObject();
    row["text"] = (row.value("text").toString()+delta).left(kTextCharacters);
    row["status"] = "streaming"; messages_[active_->assistantRow] = row; Prune(); Changed(); return true;
}
bool VisibleChatSession::AppendActionReceipt(const QString& text) {
    return active_ && AppendActionReceipt(active_->ticket,text);
}
bool VisibleChatSession::AppendActionReceipt(const ChatTurnTicket& ticket,const QString& text) {
    if (!OwnsTicket(ticket) || text.trimmed().isEmpty()) return false;
    const auto found = receiptRows_.find(ticket.requestGeneration);
    if (found == receiptRows_.end()) return false;
    int index = -1;
    for (int i = 0;i < messages_.size();++i)
        if (messages_[i].toObject().value("id").toString() == found->second) { index = i; break; }
    if (index < 0) return false;
    auto row = messages_[index].toObject();
    auto actions = row.value("actions").toArray();
    if (actions.size() >= 32) actions.removeFirst();
    actions.append(text.left(2048)); row["actions"] = actions;
    messages_[index] = row; Changed(); return true;
}
bool VisibleChatSession::Complete(const ChatTurnTicket& ticket,const QString& text,const QString& error,bool interrupted) {
    if (!Current(ticket)) return false;
    auto row = messages_[active_->assistantRow].toObject();
    if (!text.isEmpty()) row["text"] = text.left(kTextCharacters);
    row["status"] = interrupted ? "interrupted" : error.isEmpty() ? "completed" : "failed";
    row["complete"] = error.isEmpty() && !interrupted;
    // Fixed application text only. Never expose provider/internal diagnostics.
    if (!error.isEmpty()) row["errorCode"] = interrupted ? "canceled" : "request_failed";
    if (error.isEmpty() && !interrupted) lastCompletedBackend_ = active_->backend;
    messages_[active_->assistantRow] = row; active_.reset(); attach_ = false; Prune(); Changed(); return true;
}
QString VisibleChatSession::BackendThread(ChatBackendKind kind) const {
    const auto found = threads_.find(kind); return found == threads_.end() ? QString() : found->second;
}
void VisibleChatSession::RetireBackends() {
    ++epoch_; ++generation_; threads_.clear(); receiptRows_.clear();
    if (active_) { const auto old = active_->ticket; Complete(old,{},"canceled",true); }
    contextStartRow_ = static_cast<int>(messages_.size()); lastCompletedBackend_.reset(); Changed();
}
QJsonArray VisibleChatSession::TextContext() const {
    std::vector<QJsonArray> pairs;
    for (int index = contextStartRow_; index+1 < messages_.size(); ++index) {
        const auto user = messages_[index].toObject(), answer = messages_[index+1].toObject();
        if (user.value("role") != "user" || answer.value("role") != "assistant" || !answer.value("complete").toBool()) continue;
        pairs.push_back({QJsonObject{{"role","user"},{"content",user.value("text").toString().left(4096)}},
            QJsonObject{{"role","assistant"},{"content",answer.value("text").toString().left(1200)}}});
        ++index;
    }
    QJsonArray context;
    const int first = std::max(0,static_cast<int>(pairs.size())-4);
    for (int i=first;i<static_cast<int>(pairs.size());++i) for (const auto& item : pairs[i]) context.append(item);
    while (QJsonDocument(context).toJson(QJsonDocument::Compact).size() > 32*1024 && context.size() >= 2) {
        context.removeFirst(); context.removeFirst();
    }
    return context;
}
QJsonArray VisibleChatSession::BridgeContext(ChatBackendKind backend) const {
    return lastCompletedBackend_ == backend && !BackendThread(backend).isEmpty() ? QJsonArray{} : TextContext();
}
} // namespace okuflow
