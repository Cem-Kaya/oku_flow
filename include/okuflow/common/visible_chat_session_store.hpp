#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <map>
#include <optional>

namespace okuflow {
struct StoredVisibleChat {
    QString id, title, draft;
    qint64 createdAt{}, updatedAt{};
    QJsonArray messages;
};
// First-party text history only. No provider references or authority survive
// restart. All files are opaque-id children of the caller's owned app-data root.
class VisibleChatSessionStore final {
public:
    explicit VisibleChatSessionStore(QString ownedRoot);
    static bool IsLogicalId(const QString& id);
    void SetSecretFilters(QStringList secrets);
    QList<StoredVisibleChat> List() const;
    std::optional<StoredVisibleChat> Load(const QString& id) const;
    bool Save(const QJsonObject& snapshot, const QString& title = {});
    bool Rename(const QString& id, const QString& title);
    bool Delete(const QString& id);
    quint64 Revision() const { return revision_; }
    QString ErrorCode() const { return error_; }
private:
    bool SafeFile(const QString& id) const;
    QString Path(const QString& id) const;
    bool Write(const StoredVisibleChat& entry);
    bool ContainsSecret(const StoredVisibleChat& entry) const;
    qsizetype StoredBytes() const;
    void Reload();
    void Prune(const QString& keepId = {});
    QString root_, error_;
    QStringList secrets_;
    std::map<QString,StoredVisibleChat> entries_;
    quint64 revision_{};
};
} // namespace okuflow
