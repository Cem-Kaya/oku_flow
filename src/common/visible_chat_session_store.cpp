#include "okuflow/common/visible_chat_session_store.hpp"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace okuflow {
namespace {
constexpr qsizetype kFileBytes = 1024*1024;
constexpr qsizetype kTotalBytes = 32*1024*1024;
constexpr int kEntries = 64;
QJsonObject Encode(const StoredVisibleChat& entry) {
    return {{"version",1},{"id",entry.id},{"kind","explain"},{"title",entry.title},
        {"createdAt",entry.createdAt},{"updatedAt",entry.updatedAt},{"draft",entry.draft},{"messages",entry.messages}};
}
bool Terminal(const QString& status) { return status == "completed" || status == "failed" || status == "interrupted"; }
std::optional<StoredVisibleChat> Decode(const QJsonObject& value) {
    static const QStringList keys{"version","id","kind","title","createdAt","updatedAt","draft","messages"};
    if (value.size()!=keys.size()) return {};
    for (auto item=value.begin();item!=value.end();++item) if (!keys.contains(item.key())) return {};
    if (value.value("version").toInt()!=1 || value.value("kind").toString()!="explain" ||
        !VisibleChatSessionStore::IsLogicalId(value.value("id").toString()) ||
        !value.value("title").isString() || value.value("title").toString().size()>256 ||
        !value.value("draft").isString() || value.value("draft").toString().size()>65536 ||
        !value.value("messages").isArray() || value.value("createdAt").toInteger(-1)<0 || value.value("updatedAt").toInteger(-1)<0) return {};
    StoredVisibleChat entry{value.value("id").toString(),value.value("title").toString(),value.value("draft").toString(),
        value.value("createdAt").toInteger(),value.value("updatedAt").toInteger(),value.value("messages").toArray()};
    if (entry.messages.size()>128 || entry.messages.size()%2!=0) return {};
    for (int index=0;index<entry.messages.size();++index) {
        if (!entry.messages[index].isObject()) return {};
        const auto row=entry.messages[index].toObject();
        static const QStringList rowKeys{"role","text","status","updatedAt","actions","origin"};
        for (auto field=row.begin();field!=row.end();++field) if (!rowKeys.contains(field.key())) return {};
        if (row.value("role").toString()!=(index%2==0?"user":"assistant") || !row.value("text").isString() ||
            row.value("text").toString().size()>100000 || !Terminal(row.value("status").toString()) ||
            row.value("updatedAt").toInteger(-1)<0) return {};
        if (index%2==0 && row.value("status").toString()!="completed") return {};
        if (row.contains("origin") && row.value("origin").toString()!="explain") return {};
        if (row.contains("actions")) {
            if (index%2==0 || !row.value("actions").isArray() || row.value("actions").toArray().size()>32) return {};
            for (const auto& action : row.value("actions").toArray()) if (!action.isString() || action.toString().size()>2048) return {};
        }
    }
    return entry;
}
}
VisibleChatSessionStore::VisibleChatSessionStore(QString ownedRoot) {
    if (ownedRoot.isEmpty() || !QDir().mkpath(ownedRoot)) { error_="storage_unavailable";return; }
    root_=QDir::fromNativeSeparators(QFileInfo(ownedRoot).canonicalFilePath());
    if (root_.isEmpty()) { error_="storage_unavailable";return; }
    Reload();
}
bool VisibleChatSessionStore::IsLogicalId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(id).hasMatch();
}
QString VisibleChatSessionStore::Path(const QString& id) const { return QDir(root_).filePath(id+QStringLiteral(".json")); }
bool VisibleChatSessionStore::SafeFile(const QString& id) const {
    if (root_.isEmpty() || !IsLogicalId(id)) return false;
    const QFileInfo file(Path(id));
    if (file.isSymLink()) return false;
    if (!file.exists()) return true;
    const auto canonical=QDir::fromNativeSeparators(file.canonicalFilePath());
#ifdef _WIN32
    constexpr auto comparison=Qt::CaseInsensitive;
#else
    constexpr auto comparison=Qt::CaseSensitive;
#endif
    return file.isFile() && canonical.startsWith(root_+QStringLiteral("/"),comparison);
}
bool VisibleChatSessionStore::ContainsSecret(const StoredVisibleChat& entry) const {
    QStringList text{entry.title,entry.draft};
    for (const auto& value : entry.messages) {
        const auto row=value.toObject();text.append(row.value("text").toString());
        for (const auto& action : row.value("actions").toArray()) text.append(action.toString());
    }
    for (const auto& secret : secrets_) for (const auto& item : text) if (item.contains(secret)) return true;
    return false;
}
void VisibleChatSessionStore::SetSecretFilters(QStringList secrets) {
    secrets.removeAll(QString());secrets.removeDuplicates();if (secrets==secrets_) return;
    secrets_=std::move(secrets);Reload();
}
void VisibleChatSessionStore::Reload() {
    entries_.clear();if (root_.isEmpty()) return;
    QDirIterator files(root_,QStringList{QStringLiteral("*.json")},QDir::Files|QDir::NoSymLinks);
    int visited=0;qsizetype readBytes=0;
    while (files.hasNext() && ++visited<=256) {
        files.next();const auto file=files.fileInfo();const auto id=file.completeBaseName();
        if (!SafeFile(id) || file.size()>kFileBytes) continue;
        if (readBytes+file.size()>kTotalBytes) break;
        readBytes+=file.size();
        QFile input(file.absoluteFilePath());if (!input.open(QIODevice::ReadOnly)) continue;
        const auto bytes=input.read(kFileBytes+1);if (bytes.size()>kFileBytes) continue;
        const auto document=QJsonDocument::fromJson(bytes);if (!document.isObject()) continue;
        const auto entry=Decode(document.object());
        if (entry && entry->id==id && !ContainsSecret(*entry)) entries_[id]=*entry;
    }
    Prune();
}
QList<StoredVisibleChat> VisibleChatSessionStore::List() const {
    QList<StoredVisibleChat> result;for (const auto& [id,entry] : entries_) result.append(entry);
    std::sort(result.begin(),result.end(),[](const auto& first,const auto& second) {return first.updatedAt>second.updatedAt;});
    return result;
}
std::optional<StoredVisibleChat> VisibleChatSessionStore::Load(const QString& id) const {
    const auto found=entries_.find(id);if (found==entries_.end() || ContainsSecret(found->second)) return {};
    return found->second;
}
bool VisibleChatSessionStore::Write(const StoredVisibleChat& entry) {
    if (!SafeFile(entry.id) || ContainsSecret(entry)) {error_="invalid_record";return false;}
    const auto bytes=QJsonDocument(Encode(entry)).toJson(QJsonDocument::Compact);
    if (bytes.size()>kFileBytes) {error_="storage_limit";return false;}
    auto sorted=List();qsizetype projected=bytes.size();
    for (const auto& [id,stored] : entries_) if (id!=entry.id) projected+=QFileInfo(Path(id)).size();
    while ((entries_.size()+(entries_.contains(entry.id)?0:1)>kEntries || projected>kTotalBytes) && !sorted.isEmpty()) {
        const auto oldest=sorted.takeLast();if (oldest.id==entry.id) continue;
        const auto size=QFileInfo(Path(oldest.id)).size();
        if (!SafeFile(oldest.id) || !QFile::remove(Path(oldest.id))) {error_="storage_unavailable";return false;}
        entries_.erase(oldest.id);++revision_;projected-=size;
    }
    if (projected>kTotalBytes || entries_.size()+(entries_.contains(entry.id)?0:1)>kEntries) {error_="storage_limit";return false;}
    QSaveFile file(Path(entry.id));file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {error_="storage_unavailable";return false;}
    entries_[entry.id]=entry;++revision_;error_.clear();Prune(entry.id);return true;
}
bool VisibleChatSessionStore::Save(const QJsonObject& snapshot,const QString& title) {
    const auto id=snapshot.value("conversationId").toString();
    if (!IsLogicalId(id) || snapshot.value("kind").toString()!="explain") {error_="invalid_record";return false;}
    const auto existing=entries_.find(id);const auto now=QDateTime::currentMSecsSinceEpoch();
    StoredVisibleChat entry{id,title.left(256),snapshot.value("draft").toString().left(65536),
        existing==entries_.end()?now:existing->second.createdAt,now,{}};
    if (entry.title.isEmpty() && existing!=entries_.end()) entry.title=existing->second.title;
    const auto rows=snapshot.value("messages").toArray();
    for (int index=0;index+1<rows.size();++index) {
        const auto user=rows[index].toObject(),answer=rows[index+1].toObject();
        if (user.value("role").toString()!="user" || answer.value("role").toString()!="assistant" || !Terminal(answer.value("status").toString())) continue;
        for (const auto& source : {user,answer}) {
            QJsonObject row{{"role",source.value("role")},{"text",source.value("text").toString().left(100000)},
                {"status",source==user?QStringLiteral("completed"):source.value("status").toString()},
                {"updatedAt",source.value("updatedAt").toInteger(now)}};
            if (source.value("role").toString()=="assistant") {
                QJsonArray actions;for (const auto& action : source.value("actions").toArray()) {
                    if (action.isString()) actions.append(action.toString().left(2048));if (actions.size()>=32) break;
                }
                if (!actions.isEmpty()) row["actions"]=actions;
            }
            if (source.value("origin").toString()=="explain") row["origin"]="explain";
            entry.messages.append(row);
        }
        ++index;
    }
    while (entry.messages.size()>128) {entry.messages.removeFirst();entry.messages.removeFirst();}
    while (QJsonDocument(Encode(entry)).toJson(QJsonDocument::Compact).size()>kFileBytes && entry.messages.size()>2) {
        entry.messages.removeFirst();entry.messages.removeFirst();
    }
    if (entry.messages.isEmpty() && entry.draft.trimmed().isEmpty() && existing==entries_.end()) return true;
    if (existing!=entries_.end() && existing->second.messages==entry.messages && existing->second.draft==entry.draft && existing->second.title==entry.title) return true;
    return Write(entry);
}
bool VisibleChatSessionStore::Rename(const QString& id,const QString& title) {
    const auto entry=Load(id);if (!entry || title.trimmed().isEmpty() || title.size()>256) {error_="invalid_record";return false;}
    auto renamed=*entry;renamed.title=title.trimmed();renamed.updatedAt=QDateTime::currentMSecsSinceEpoch();return Write(renamed);
}
bool VisibleChatSessionStore::Delete(const QString& id) {
    if (!entries_.contains(id) || !SafeFile(id)) {error_="invalid_record";return false;}
    if (!QFile::remove(Path(id))) {error_="storage_unavailable";return false;}
    entries_.erase(id);++revision_;error_.clear();return true;
}
void VisibleChatSessionStore::Prune(const QString& keepId) {
    auto sorted=List();qsizetype bytes=0;for (const auto& entry : sorted) bytes+=QFileInfo(Path(entry.id)).size();
    while ((entries_.size()>kEntries || bytes>kTotalBytes) && !sorted.isEmpty()) {
        const auto oldest=sorted.takeLast();if (oldest.id==keepId) continue;
        const auto size=QFileInfo(Path(oldest.id)).size();
        if (!SafeFile(oldest.id) || !QFile::remove(Path(oldest.id))) {error_="storage_unavailable";break;}
        entries_.erase(oldest.id);bytes-=size;
    }
}
} // namespace okuflow
