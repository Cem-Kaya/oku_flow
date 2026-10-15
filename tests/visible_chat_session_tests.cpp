#include "okuflow/common/visible_chat_session.hpp"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>
using namespace okuflow;
class VisibleChatSessionTests : public QObject {
    Q_OBJECT
private slots:
    void framePreviewsAreBoundedEphemeralAndTicketScoped() {
        VisibleChatSession chat;
        const auto jpeg=QByteArray::fromHex("ffd800010203ffd9");
        const auto first=chat.Admit("Explain",ChatBackendKind::Explain);QVERIFY(first);
        QVERIFY(chat.SetImagePreview(*first,jpeg));
        QCOMPARE(QByteArray::fromBase64(chat.Snapshot()["messages"].toArray()[0].toObject()["imagePreviewJpeg"].toString().toLatin1()),jpeg);
        QVERIFY(!chat.SetImagePreview(*first,QByteArray(65537,'x')));
        QVERIFY(!chat.SetImagePreview(*first,"not a JPEG"));
        QVERIFY(chat.Complete(*first,"answer"));
        QVERIFY(!chat.SetImagePreview(*first,jpeg));
        QVERIFY(!QJsonDocument(chat.TextContext()).toJson().contains("imagePreviewJpeg"));
        QVERIFY(!QJsonDocument(chat.BridgeContext(ChatBackendKind::Explain)).toJson().contains("imagePreviewJpeg"));
        for (int i=0;i<10;++i) {auto turn=chat.Admit("question",ChatBackendKind::Explain);QVERIFY(turn);QVERIFY(chat.SetImagePreview(*turn,jpeg));QVERIFY(chat.Complete(*turn,"answer"));}
        int previews=0;for(const auto value:chat.Snapshot()["messages"].toArray())previews+=value.toObject().contains("imagePreviewJpeg");
        QCOMPARE(previews,8);
        VisibleChatSession restored;restored.RestoreLogical(chat.ConversationId(),chat.Snapshot()["messages"].toArray(),{});
        QVERIFY(!QJsonDocument(restored.Snapshot()).toJson().contains("imagePreviewJpeg"));
        chat.New();QVERIFY(!chat.SetImagePreview(*first,jpeg));
    }
    void nativeNewPolicyKeepsControlRestrictedAndOrdinaryKindsImmutable() {
        VisibleChatSession chat;
        chat.New(VisibleChatSession::NewConversationKind(true,false,true));
        QCOMPARE(chat.Snapshot()["kind"].toString(),"coding");
        auto ticket=chat.Admit("question",ChatBackendKind::Coding); QVERIFY(ticket); chat.Complete(*ticket,"answer");
        // A settings toggle cannot transform an admitted logical conversation.
        QCOMPARE(VisibleChatSession::NewConversationKind(true,false,false),ChatBackendKind::Assistant);
        QCOMPARE(chat.Snapshot()["kind"].toString(),"coding");
        QCOMPARE(VisibleChatSession::NewConversationKind(true,true,true),ChatBackendKind::Explain);
        QCOMPARE(VisibleChatSession::NewConversationKind(false,false,true),ChatBackendKind::Explain);
        chat.New(ChatBackendKind::Explain); QCOMPARE(chat.Snapshot()["kind"].toString(),"explain");
    }
    void singleAdmissionAndAuthoritativeFinish() {
        VisibleChatSession chat;
        chat.SetDraft("Why is the board dark?"); chat.SetAttachCurrentView(true);
        auto ticket = chat.Admit(chat.Draft(),ChatBackendKind::Explain);
        QVERIFY(ticket); QVERIFY(chat.Busy()); QCOMPARE(chat.Draft(),QString()); QVERIFY(chat.AttachCurrentView());
        QVERIFY(!chat.Admit("duplicate",ChatBackendKind::Explain));
        QVERIFY(chat.Bind(*ticket,"explain-thread","turn-1"));
        QVERIFY(!chat.Bind(*ticket,"explain-thread","other-turn"));
        QVERIFY(chat.AppendDelta(*ticket,"partial")); QVERIFY(chat.AppendDelta(*ticket," stream"));
        QVERIFY(chat.AppendActionReceipt("Zoom 2"));
        QVERIFY(chat.Complete(*ticket,"Authoritative full answer"));
        QVERIFY(!chat.Complete(*ticket,"duplicate final")); QVERIFY(!chat.AppendDelta(*ticket,"late"));
        const auto rows = chat.Snapshot()["messages"].toArray(); QCOMPARE(rows.size(),2);
        QCOMPARE(rows[1].toObject()["text"].toString(),"Authoritative full answer");
        QCOMPARE(rows[1].toObject()["actions"].toArray().size(),1);
        QVERIFY(!QJsonDocument(chat.TextContext()).toJson().contains("Zoom 2"));
        QVERIFY(!chat.AppendActionReceipt("late receipt"));
        QVERIFY(!chat.Busy()); QVERIFY(!chat.AttachCurrentView()); QCOMPARE(chat.BackendThread(ChatBackendKind::Explain),"explain-thread");
    }
    void twoViewsShareDraftAndBusy() {
        VisibleChatSession chat;
        QJsonObject advanced = chat.Snapshot(), floating = advanced;
        connect(&chat,&VisibleChatSession::SnapshotChanged,this,[&](const auto& value) {advanced=value;floating=value;});
        chat.SetDraft("shared question"); chat.SetAttachCurrentView(true);
        QCOMPARE(advanced,floating); QCOMPARE(floating["draft"].toString(),"shared question");
        auto ticket=chat.Admit(chat.Draft(),ChatBackendKind::Agent); QVERIFY(ticket);
        QCOMPARE(advanced,floating); QVERIFY(floating["busy"].toBool());
        chat.Complete(*ticket,"done"); QCOMPARE(advanced,floating); QVERIFY(!advanced["busy"].toBool());
    }
    void backendSegmentsNeverChangeVisibleKindOrLeakIds() {
        VisibleChatSession chat;
        auto explain=chat.Admit("Explain",ChatBackendKind::Explain,ChatRequestOrigin::ExplainButton); QVERIFY(explain);
        chat.Bind(*explain,"ordinary-explain-id","explain-turn"); chat.Complete(*explain,"A dark board.");
        const auto id=chat.ConversationId();
        auto agent=chat.Admit("Make it readable",ChatBackendKind::Agent); QVERIFY(agent);
        chat.Bind(*agent,"restricted-agent-id","agent-turn"); chat.Complete(*agent,"Raised contrast.");
        QCOMPARE(chat.ConversationId(),id); QCOMPARE(chat.Snapshot()["kind"].toString(),"explain");
        QCOMPARE(chat.BackendThread(ChatBackendKind::Explain),"ordinary-explain-id");
        QCOMPARE(chat.BackendThread(ChatBackendKind::Agent),"restricted-agent-id");
        QVERIFY(chat.BridgeContext(ChatBackendKind::Agent).isEmpty());
        QVERIFY(!chat.BridgeContext(ChatBackendKind::Explain).isEmpty());
        const auto bytes=QJsonDocument(chat.TextContext()).toJson();
        QVERIFY(!bytes.contains("ordinary-explain-id")); QVERIFY(!bytes.contains("restricted-agent-id")); QVERIFY(!bytes.contains("image"));
    }
    void newAndConfigurationRetireLateTickets() {
        VisibleChatSession chat;
        auto old=chat.Admit("old",ChatBackendKind::Explain); QVERIFY(old);
        chat.New(); auto next=chat.Admit("next",ChatBackendKind::Explain); QVERIFY(next);
        QVERIFY(!chat.AppendDelta(*old,"stale")); QVERIFY(!chat.Complete(*old,"stale"));
        chat.Complete(*next,"current"); QVERIFY(!chat.TextContext().isEmpty());
        chat.RetireBackends(); QVERIFY(chat.TextContext().isEmpty());
        QCOMPARE(chat.Snapshot()["messages"].toArray().size(),2);
        auto pending=chat.Admit("pending",ChatBackendKind::Agent); QVERIFY(pending);
        chat.RetireBackends(); QVERIFY(!chat.Complete(*pending,"late")); QVERIFY(!chat.Busy());
    }
    void boundedContextRejectsPartialAndFailedAnswers() {
        VisibleChatSession chat;
        for(int i=0;i<8;++i) {auto t=chat.Admit(QStringLiteral("question %1").arg(i),ChatBackendKind::Explain); QVERIFY(t);chat.Complete(*t,QString(5000,'x'));}
        auto failed=chat.Admit("failed question",ChatBackendKind::Agent); QVERIFY(failed); chat.AppendDelta(*failed,"partial secret"); chat.Complete(*failed,{},"provider raw diagnostic");
        const auto context=chat.TextContext(); QCOMPARE(context.size(),8);
        QVERIFY(QJsonDocument(context).toJson(QJsonDocument::Compact).size()<=32*1024);
        const auto bytes=QJsonDocument(context).toJson(); QVERIFY(!bytes.contains("partial secret")); QVERIFY(!bytes.contains("failed question"));
        QVERIFY(!QJsonDocument(chat.Snapshot()).toJson().contains("provider raw diagnostic"));
    }
    void restoredHistoryKeepsKindAndDropsForeignPayloadFields() {
        VisibleChatSession chat;
        chat.Restore("saved-id",ChatBackendKind::Coding,QJsonArray{QJsonObject{{"role","user"},{"text","question"},{"image","private"}},QJsonObject{{"role","assistant"},{"text","answer"}}});
        QCOMPARE(chat.Snapshot()["kind"].toString(),"coding");
        QVERIFY(!QJsonDocument(chat.Snapshot()).toJson().contains("private"));
        QCOMPARE(chat.TextContext().size(),2);
    }
    void delayedReceiptsStayBoundToOriginalRowAndEpoch() {
        VisibleChatSession chat;
        const auto first = chat.Admit("capture",ChatBackendKind::Agent); QVERIFY(first);
        chat.Complete(*first,"Review the pending photo permission.");
        const auto second = chat.Admit("next question",ChatBackendKind::Explain); QVERIFY(second);
        QVERIFY(chat.AppendActionReceipt(*first,"Photo captured"));
        auto rows = chat.Snapshot()["messages"].toArray();
        QCOMPARE(rows[1].toObject()["actions"].toArray().size(),1);
        QVERIFY(!rows[3].toObject().contains("actions"));
        QVERIFY(!QJsonDocument(chat.TextContext()).toJson().contains("Photo captured"));
        chat.New(); QVERIFY(!chat.AppendActionReceipt(*first,"late old photo"));
        const auto third = chat.Admit("current",ChatBackendKind::Agent); QVERIFY(third);
        chat.Complete(*third,"done"); chat.RetireBackends();
        QVERIFY(!chat.AppendActionReceipt(*third,"configuration-stale receipt"));
    }
    void restoredFailedAnswersRemainOutsideCompletedContext() {
        VisibleChatSession chat;
        chat.Restore("saved",ChatBackendKind::Agent,{QJsonObject{{"role","user"},{"text","question"}},
            QJsonObject{{"role","assistant"},{"text","partial"},{"status","failed"},
                {"errorCode","raw provider diagnostic"},{"actions",QJsonArray{"Photo captured",QJsonObject{{"grant",true}}}}}});
        QVERIFY(chat.TextContext().isEmpty());
        const auto row=chat.Snapshot()["messages"].toArray()[1].toObject();
        QCOMPARE(row["status"].toString(),"failed"); QVERIFY(!row["complete"].toBool());
        QCOMPARE(row["actions"].toArray().size(),1);
        QVERIFY(!QJsonDocument(chat.Snapshot()).toJson().contains("raw provider diagnostic"));
    }
};
QTEST_GUILESS_MAIN(VisibleChatSessionTests)
#include "visible_chat_session_tests.moc"
