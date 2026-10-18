#include "okuflow/common/visible_chat_session_store.hpp"
#include "okuflow/common/visible_chat_session.hpp"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QtTest>
#ifdef _WIN32
#include <Windows.h>
#endif
using namespace okuflow;
namespace {
QByteArray Bytes(const QString& path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
QJsonObject Mixed(VisibleChatSession& chat) {
    auto explain=chat.Admit("Explain",ChatBackendKind::Explain,ChatRequestOrigin::ExplainButton);chat.Bind(*explain,"private-explain","turn-one");chat.Complete(*explain,"Dark text on the page.");
    auto agent=chat.Admit("Increase contrast",ChatBackendKind::Agent);chat.Bind(*agent,"private-agent","turn-two");
    chat.AppendActionReceipt("Contrast 2");chat.Complete(*agent,"Contrast increased.");
    chat.SetDraft("Now describe it");chat.SetAttachCurrentView(true);return chat.Snapshot();
}
}
class VisibleChatSessionStoreTests final : public QObject {
    Q_OBJECT
private slots:
    void framePreviewBytesNeverPersist() {
        QTemporaryDir directory;QVERIFY(directory.isValid());VisibleChatSession chat;
        auto turn=chat.Admit("Explain",ChatBackendKind::Explain);QVERIFY(turn);
        QVERIFY(chat.SetImagePreview(*turn,QByteArray::fromHex("ffd800010203ffd9")));QVERIFY(chat.Complete(*turn,"answer"));
        VisibleChatSessionStore store(directory.path());QVERIFY(store.Save(chat.Snapshot()));
        const auto bytes=Bytes(directory.filePath(chat.ConversationId()+".json"));
        QVERIFY(!bytes.contains("imagePreviewJpeg"));QVERIFY(!bytes.contains("/9gAAQID/9k="));
    }
    void restartRestoresMixedTextWithoutAuthorityOrBackendIds() {
        QTemporaryDir directory;QVERIFY(directory.isValid());VisibleChatSession chat;const auto snapshot=Mixed(chat);
        VisibleChatSessionStore first(directory.path());QVERIFY(first.Save(snapshot,"Lecture"));
        const auto file=directory.filePath(chat.ConversationId()+".json");const auto bytes=Bytes(file);
        QVERIFY(!bytes.contains("private-explain"));QVERIFY(!bytes.contains("private-agent"));
        QVERIFY(!bytes.contains("attachCurrentView"));QVERIFY(!bytes.contains("grants"));QVERIFY(!bytes.contains("image"));
        VisibleChatSessionStore restarted(directory.path());const auto record=restarted.Load(chat.ConversationId());QVERIFY(record);
        QCOMPARE(restarted.List().size(),1);QCOMPARE(record->messages.size(),4);QCOMPARE(record->title,"Lecture");
        VisibleChatSession restored;restored.RestoreLogical(record->id,record->messages,record->draft);
        QVERIFY(restored.IsLocalLogical());QCOMPARE(restored.Snapshot()["kind"].toString(),"explain");
        QCOMPARE(restored.Snapshot()["messages"].toArray().size(),4);QCOMPARE(restored.Draft(),"Now describe it");
        QCOMPARE(restored.Snapshot()["messages"].toArray()[0].toObject()["origin"].toString(),"explain");
        QCOMPARE(restored.Snapshot()["messages"].toArray()[1].toObject()["origin"].toString(),"explain");
        QVERIFY(!restored.Snapshot()["messages"].toArray()[2].toObject().contains("origin"));
        QVERIFY(!restored.AttachCurrentView());QVERIFY(!restored.Busy());
        QVERIFY(restored.BackendThread(ChatBackendKind::Agent).isEmpty());QVERIFY(restored.BackendThread(ChatBackendKind::Explain).isEmpty());
        QCOMPARE(restored.BridgeContext(ChatBackendKind::Agent).size(),4);
        QVERIFY(!QJsonDocument(restored.TextContext()).toJson().contains("Contrast 2"));
    }
    void malformedRecordsAndNonOpaqueIdsAreRejected() {
        QTemporaryDir directory;QVERIFY(directory.isValid());VisibleChatSession chat;auto snapshot=Mixed(chat);
        VisibleChatSessionStore store(directory.path());QVERIFY(store.Save(snapshot));
        auto file=directory.filePath(chat.ConversationId()+".json");auto data=QJsonDocument::fromJson(Bytes(file)).object();
        data["grants"]=QJsonObject{{"frames",true}};
        QFile invalid(file);QVERIFY(invalid.open(QIODevice::WriteOnly));invalid.write(QJsonDocument(data).toJson());invalid.close();
        VisibleChatSessionStore reloaded(directory.path());QVERIFY(reloaded.List().isEmpty());
        snapshot["conversationId"]="../foreign";QVERIFY(!reloaded.Save(snapshot));
        snapshot["conversationId"]="private-agent-thread";QVERIFY(!reloaded.Save(snapshot));
        QVERIFY(!reloaded.Rename("../foreign","name"));QVERIFY(!reloaded.Delete("../foreign"));
        snapshot["conversationId"]=chat.ConversationId();snapshot["kind"]="coding";QVERIFY(!reloaded.Save(snapshot));
    }
    void knownSecretsAndPendingToolPayloadsNeverPersist() {
        QTemporaryDir directory;QVERIFY(directory.isValid());VisibleChatSessionStore store(directory.path());
        store.SetSecretFilters({"fixture-secret"});VisibleChatSession chat;
        chat.SetDraft("fixture-secret");QVERIFY(!store.Save(chat.Snapshot()));QVERIFY(store.List().isEmpty());
        chat.SetDraft("safe draft");QVERIFY(store.Save(chat.Snapshot()));
        auto pending=chat.Admit("new question",ChatBackendKind::Agent);QVERIFY(pending);chat.AppendDelta(*pending,"partial reply");
        auto snapshot=chat.Snapshot();snapshot["grants"]=QJsonObject{{"frames",true}};snapshot["tool_calls"]=QJsonArray{"private operation"};
        snapshot["images"]=QJsonArray{"private image"};snapshot["apiKey"]="fixture-secret";
        QVERIFY(store.Save(snapshot));auto record=store.Load(chat.ConversationId());QVERIFY(record);QVERIFY(record->messages.isEmpty());
        const auto bytes=Bytes(directory.filePath(chat.ConversationId()+".json"));
        QVERIFY(!bytes.contains("private operation"));QVERIFY(!bytes.contains("partial reply"));QVERIFY(!bytes.contains("fixture-secret"));
        chat.Complete(*pending,{},"raw provider error");QVERIFY(store.Save(chat.Snapshot()));
        VisibleChatSessionStore restarted(directory.path());record=restarted.Load(chat.ConversationId());QVERIFY(record);
        VisibleChatSession restored;restored.RestoreLogical(record->id,record->messages,record->draft);
        QVERIFY(restored.TextContext().isEmpty());QCOMPARE(restored.Snapshot()["messages"].toArray()[1].toObject()["status"].toString(),"failed");
    }
    void renameDeleteAndFailedAtomicReplacementPreservePreviousBytes() {
        QTemporaryDir directory;QVERIFY(directory.isValid());VisibleChatSession chat;auto snapshot=Mixed(chat);
        VisibleChatSessionStore store(directory.path());QVERIFY(store.Save(snapshot,"Original"));
        const auto path=directory.filePath(chat.ConversationId()+".json");const auto original=Bytes(path);
#ifdef _WIN32
        const auto native=QDir::toNativeSeparators(path);
        HANDLE lock=CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        QVERIFY(lock!=INVALID_HANDLE_VALUE);
        const bool renamed=store.Rename(chat.ConversationId(),"Replacement");CloseHandle(lock);
        QVERIFY(!renamed);QCOMPARE(Bytes(path),original);QCOMPARE(store.Load(chat.ConversationId())->title,"Original");
#endif
        QVERIFY(store.Rename(chat.ConversationId(),"Renamed"));
        VisibleChatSessionStore restarted(directory.path());QCOMPARE(restarted.Load(chat.ConversationId())->title,"Renamed");
        QVERIFY(restarted.Delete(chat.ConversationId()));QVERIFY(!QFile::exists(path));QVERIFY(restarted.List().isEmpty());
    }
    void ownedRetentionAndIndividualRecordRemainBounded() {
        QTemporaryDir directory;QVERIFY(directory.isValid());VisibleChatSessionStore store(directory.path());
        for (int index=0;index<70;++index) {VisibleChatSession chat;chat.SetDraft(QStringLiteral("draft %1").arg(index));QVERIFY(store.Save(chat.Snapshot()));}
        QVERIFY(store.List().size()<=64);
        VisibleChatSession large;
        for (int index=0;index<70;++index) {auto turn=large.Admit(QString(10000,'q'),ChatBackendKind::Explain);QVERIFY(turn);large.Complete(*turn,QString(100000,'a'));}
        QVERIFY(store.Save(large.Snapshot()));
        const auto record=store.Load(large.ConversationId());QVERIFY(record);QVERIFY(record->messages.size()<=128);
        QVERIFY(Bytes(directory.filePath(large.ConversationId()+".json")).size()<=1024*1024);
    }
    void failedNewCommitAtRetentionLimitPreservesEveryPreviousConversation() {
#ifdef _WIN32
        QTemporaryDir directory;QVERIFY(directory.isValid());VisibleChatSessionStore store(directory.path());
        QMap<QString,QByteArray> previous;
        for (int index=0;index<64;++index) {
            VisibleChatSession chat;chat.SetDraft(QStringLiteral("saved draft %1").arg(index));
            QVERIFY(store.Save(chat.Snapshot()));
            previous[chat.ConversationId()]=Bytes(directory.filePath(chat.ConversationId()+".json"));
        }
        VisibleChatSession next;next.SetDraft("new draft");
        const auto path=directory.filePath(next.ConversationId()+".json");
        QFile placeholder(path);QVERIFY(placeholder.open(QIODevice::WriteOnly));
        const QByteArray marker="uncommitted target";QCOMPARE(placeholder.write(marker),marker.size());placeholder.close();
        const auto native=QDir::toNativeSeparators(path);
        HANDLE lock=CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),GENERIC_READ,FILE_SHARE_READ,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        QVERIFY(lock!=INVALID_HANDLE_VALUE);
        const auto revision=store.Revision();
        const bool saved=store.Save(next.Snapshot());CloseHandle(lock);
        QVERIFY(!saved);QCOMPARE(store.Revision(),revision);QCOMPARE(store.List().size(),64);
        QCOMPARE(Bytes(path),marker);
        for (auto item=previous.begin();item!=previous.end();++item)
            QCOMPARE(Bytes(directory.filePath(item.key()+".json")),item.value());
        VisibleChatSessionStore restarted(directory.path());QCOMPARE(restarted.List().size(),64);
        QVERIFY(restarted.Save(next.Snapshot()));QCOMPARE(restarted.List().size(),64);
        QVERIFY(restarted.Load(next.ConversationId()));
#endif
    }
    void blockedPruningKeepsOneCommittedOverflowAndRefusesFurtherGrowth() {
#ifdef _WIN32
        QTemporaryDir directory;QVERIFY(directory.isValid());VisibleChatSessionStore store(directory.path());
        QList<HANDLE> locks;
        const auto release=qScopeGuard([&] { for (const auto handle : locks) CloseHandle(handle); });
        for (int index=0;index<64;++index) {
            VisibleChatSession chat;chat.SetDraft(QStringLiteral("protected draft %1").arg(index));
            QVERIFY(store.Save(chat.Snapshot()));
            const auto native=QDir::toNativeSeparators(directory.filePath(chat.ConversationId()+".json"));
            const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),GENERIC_READ,FILE_SHARE_READ,
                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            QVERIFY(handle!=INVALID_HANDLE_VALUE);locks.append(handle);
        }
        VisibleChatSession committed;committed.SetDraft("durable before retention cleanup");
        QVERIFY(store.Save(committed.Snapshot()));QCOMPARE(store.List().size(),65);
        QCOMPARE(store.ErrorCode(),QStringLiteral("retention_pending"));
        VisibleChatSession rejected;rejected.SetDraft("must not grow the overflow");
        QVERIFY(!store.Save(rejected.Snapshot()));QCOMPARE(store.List().size(),65);
        QVERIFY(!QFile::exists(directory.filePath(rejected.ConversationId()+".json")));
        VisibleChatSessionStore restarted(directory.path());QVERIFY(restarted.Load(committed.ConversationId()));
        for (const auto handle : locks) CloseHandle(handle);locks.clear();
        QVERIFY(restarted.Save(rejected.Snapshot()));QCOMPARE(restarted.List().size(),64);
        QVERIFY(restarted.Load(rejected.ConversationId()));
#endif
    }
    void legacyRestoreRemainsOutsideLocalStoreAdmission() {
        VisibleChatSession legacy;
        legacy.Restore(QString(32,'a'),ChatBackendKind::Explain,{});QVERIFY(!legacy.IsLocalLogical());
        legacy.New(ChatBackendKind::Coding);QVERIFY(!legacy.IsLocalLogical());
        legacy.New(ChatBackendKind::Explain);QVERIFY(legacy.IsLocalLogical());
    }
};
QTEST_GUILESS_MAIN(VisibleChatSessionStoreTests)
#include "visible_chat_session_store_tests.moc"
