#pragma once
#include <QObject>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <map>
#include <optional>

namespace okuflow {
enum class ChatBackendKind { Explain, Agent, Assistant, Coding };
enum class ChatRequestOrigin { ExplainButton, Followup };
struct ChatTurnTicket {
    QString conversationId;
    quint64 epoch{}, requestGeneration{};
    bool operator==(const ChatTurnTicket&) const = default;
};
// Runtime-thread presentation store. Small frame previews are memory-only;
// it owns no credential, control grant, file path or widget.
// Backend kind never changes when control toggles.
class VisibleChatSession final : public QObject {
    Q_OBJECT
public:
    explicit VisibleChatSession(QObject* parent = nullptr);
    QJsonObject Snapshot() const;
    void Refresh() { Changed(); }
    QString ConversationId() const { return conversationId_; }
    bool Busy() const { return active_.has_value(); }
    void SetDraft(const QString& draft);
    QString Draft() const { return draft_; }
    void SetAttachCurrentView(bool attach);
    bool AttachCurrentView() const { return attach_; }
    void New(ChatBackendKind visibleKind = ChatBackendKind::Explain);
    void Restore(const QString& id, ChatBackendKind visibleKind, const QJsonArray& messages);
    void RestoreLogical(const QString& id, const QJsonArray& messages, const QString& draft);
    bool IsLocalLogical() const { return localLogical_ && visibleKind_ == ChatBackendKind::Explain; }
    std::optional<ChatTurnTicket> Admit(const QString& text, ChatBackendKind backend,
                                      ChatRequestOrigin origin = ChatRequestOrigin::Followup);
    bool Current(const ChatTurnTicket& ticket) const;
    bool OwnsTicket(const ChatTurnTicket& ticket) const;
    std::optional<ChatTurnTicket> ActiveTicket() const;
    bool Bind(const ChatTurnTicket& ticket, const QString& threadId, const QString& turnId);
    // At most eight 64 KiB JPEG thumbnails; never restored or sent as context.
    bool SetImagePreview(const ChatTurnTicket& ticket, const QByteArray& jpeg);
    bool AppendDelta(const ChatTurnTicket& ticket, const QString& delta);
    // First-party presentation receipts only; never included in model context.
    bool AppendActionReceipt(const QString& text);
    bool AppendActionReceipt(const ChatTurnTicket& ticket, const QString& text);
    bool Complete(const ChatTurnTicket& ticket, const QString& fullText,
                  const QString& error = {}, bool interrupted = false);
    QString BackendThread(ChatBackendKind kind) const;
    void RetireBackends();
    // Only completed role pairs, ≤4 exchanges /32 KiB UTF-8. No images,
    // tools, IDs, action records, errors or incomplete answers enter context.
    QJsonArray TextContext() const;
    QJsonArray BridgeContext(ChatBackendKind backend) const;
    static QString KindName(ChatBackendKind kind);
    static ChatBackendKind NewConversationKind(bool subscriptionProvider, bool embeddedControl, bool codingEnabled);
signals:
    void SnapshotChanged(const QJsonObject& snapshot);
private:
    struct Active { ChatTurnTicket ticket; ChatBackendKind backend; int assistantRow; QString turnId; };
    void Changed();
    void Prune();
    QString conversationId_, draft_;
    ChatBackendKind visibleKind_{ChatBackendKind::Explain};
    quint64 epoch_{}, generation_{};
    QJsonArray messages_;
    std::optional<Active> active_;
    std::map<ChatBackendKind,QString> threads_;
    std::map<quint64,QString> receiptRows_;
    bool attach_{};
    bool localLogical_{true};
    int contextStartRow_{};
    std::optional<ChatBackendKind> lastCompletedBackend_;
};
} // namespace okuflow
Q_DECLARE_METATYPE(okuflow::ChatTurnTicket)
