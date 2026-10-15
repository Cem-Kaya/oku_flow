#pragma once
#include <QJsonObject>
#include <QString>
#include <map>
#include <optional>
#include <functional>

namespace okuflow::agent {
enum class OperationKind { Photo, Recording, Camera, Adjustment };
enum class OperationState { Queued, Readback, Writing, Committed, Starting, Recording, Stopping, Completed, Requested, FirstFrame, Failed };
struct OperationRecord {
    QString id, owner, sessionId;
    OperationKind kind;
    OperationState state;
    qint64 terminalAt{};
    QJsonObject summary;
};
QString OperationStateName(OperationState state);
class OperationTracker {
public:
    explicit OperationTracker(std::function<qint64()> clock = {});
    QString Create(OperationKind kind, const QString& owner, const QString& sessionId = {});
    bool HasCapacity();
    bool Advance(const QString& id, OperationState state, QJsonObject summary = {});
    bool SetSession(const QString& id, const QString& sessionId);
    std::optional<OperationRecord> Get(const QString& id) const;
    QJsonObject SafeStatus(const QString& id) const;
    void Prune(qint64 now);
private:
    qint64 Now() const;
    std::function<qint64()> clock_;
    std::map<QString,OperationRecord> operations_;
};
} // namespace okuflow::agent
