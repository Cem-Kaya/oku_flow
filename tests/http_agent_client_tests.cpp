#include "okuflow/common/http_agent_client.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <deque>
#include <optional>
#include <utility>

using namespace okuflow;
using namespace okuflow::agent;
namespace {
QJsonObject Call(QString id, QString name, QJsonValue arguments = QJsonObject{}) {
    return {{"id",id},{"type","function"},{"function",QJsonObject{{"name",name},{"arguments",arguments}}}};
}
QByteArray Answer(QJsonObject message, QString reason = QStringLiteral("tool_calls")) {
    return QJsonDocument(QJsonObject{{"choices",QJsonArray{QJsonObject{{"message",message},{"finish_reason",reason}}}}}).toJson(QJsonDocument::Compact);
}
QByteArray Tools(QJsonArray calls) { return Answer({{"role","assistant"},{"content",QJsonValue::Null},{"tool_calls",calls}}); }
QByteArray Final(QString text = QStringLiteral("Done")) { return Answer({{"role","assistant"},{"content",text}},"stop"); }
QByteArray Probe(bool legacy = false) {
    if (legacy) return Answer({{"role","assistant"},{"function_call",QJsonObject{{"name","okuflow_capability_test"},{"arguments","{\"value\":1}"}}}},"function_call");
    return Tools({Call("probe","okuflow_capability_test",QJsonObject{{"value",1}})});
}
class Server {
public:
    struct Response { QByteArray bytes; int status{200}; QByteArray extraHeaders; };
    QTcpServer server;
    std::deque<Response> responses;
    std::vector<QJsonObject> requests;
    std::vector<QByteArray> headers;
    Server() {
        if (!server.listen(QHostAddress::LocalHost,0)) qFatal("Private HTTP fixture bind failed");
        QObject::connect(&server,&QTcpServer::newConnection,&server,[this] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket,&QTcpSocket::readyRead,socket,[this,socket,buffer] {
                    buffer->append(socket->readAll());
                    const auto boundary = buffer->indexOf("\r\n\r\n");
                    if (boundary < 0 || socket->property("received").toBool()) return;
                    const auto header = buffer->left(boundary);
                    int length = 0;
                    for (const auto line : header.split('\n')) {
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    }
                    if (buffer->size() < boundary + 4 + length) return;
                    socket->setProperty("received",true);
                    headers.push_back(header);
                    requests.push_back(QJsonDocument::fromJson(buffer->mid(boundary+4,length)).object());
                    if (responses.empty()) return; // deliberately held request for cancellation fixtures
                    const auto response = responses.front(); responses.pop_front();
                    socket->write(QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(response.status) + " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(response.bytes.size()) + "\r\n" + response.extraHeaders + "\r\n" + response.bytes);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            }
        });
    }
    QUrl Url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort())); }
    void Push(QByteArray bytes, int status = 200, QByteArray extraHeaders = {}) { responses.push_back({bytes,status,extraHeaders}); }
};
class Target final : public IAppControlTarget {
public:
    std::map<Field,QJsonValue> fields{{Field::ZoomAmount,1.0}};
    int effects{}, observations{};
    bool delay{}, modal{};
    mutable std::function<void()> beforeState;
    ControlCompletion delayed;
    QJsonObject State() const override { return {{"zoom",fields.at(Field::ZoomAmount)}}; }
    std::map<Field,QJsonValue> Fields() const override { return fields; }
    ControlPlan Prepare(ToolId tool,const QJsonObject& args,const QString&) const override {
        if (tool == ToolId::State && beforeState) std::exchange(beforeState, {})();
        ControlPlan plan;
        if (tool == ToolId::Zoom) plan.writes = {{Field::ZoomAmount,args["magnification"]}};
        return plan;
    }
    bool Apply(const FieldWrite& write) override { ++effects; fields[write.field] = write.value; return true; }
    bool Save() override { return true; }
    QJsonObject Query(ToolId,const QJsonObject&) const override { return {}; }
    void Execute(ToolId tool,const QJsonObject&,const QString&,ContextKind,ControlCompletion completion) override {
        if (tool == ToolId::Observe) {
            ++observations;
            if (delay) delayed = std::move(completion);
            else completion({{{"ok",true},{"observationId","obs-fixture"}},QByteArray("fixture-jpeg")});
        } else completion({{{"ok",true}}, {}});
    }
    bool ModalActive() const override { return modal; }
    bool UserInteracting() const override { return false; }
    bool RecordingActive() const override { return false; }
    QString RecordingSession() const override { return {}; }
    QString RecordingOwner() const override { return {}; }
    void StopOwnedRecording(const QString&) override {}
};
HttpAgentConfig Config(const Server& server, HttpAgentToolMode mode = HttpAgentToolMode::Tools) {
    return {server.Url(),"fixture-model","fixture-not-real-secret-9f82e4",mode};
}
HttpAgentTurn Turn(AppControlService& service, QByteArray jpeg = {}) {
    service.SetEnabled(true,false);
    const auto context = service.Begin({ContextKind::EmbeddedTurn,"app-issued-owner",5,{true,false,false,false}});
    // Synthetic pixels and authority are created together, before any mutation.
    return {context, "app-issued-http-epoch-5","Application control fixture.","Make the text readable.",jpeg,
        {}, AuthorizedObservation{context, service.Stamp(), 0}};
}
bool Test(HttpAgentClient& client) {
    std::optional<bool> result;
    if (!client.TestCapability([&](HttpAgentCapability capability,QString error) { result = capability.available && error.isEmpty(); },true)) return false;
    QElapsedTimer timer; timer.start();
    while (!result && timer.elapsed() < 5000) QTest::qWait(5);
    return result.value_or(false);
}
int Images(const QJsonArray& messages) {
    int result = 0;
    for (const auto& message : messages)
        for (const auto& part : message.toObject()["content"].toArray())
            if (part.toObject()["type"].toString() == "image_url") ++result;
    return result;
}
}
class HttpAgentTests final : public QObject {
    Q_OBJECT
private slots:
    void initialImageCannotAcquireNewAuthorityAfterPreparation() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service);
        server.Push(Probe()); QVERIFY(Test(client));
        auto turn = Turn(service, "fixture-old-pixels");
        service.NativeFieldsChanged({Field::ZoomAmount});
        std::optional<HttpAgentOutcome> outcome;
        QVERIFY(client.Start(turn, [&](auto value) { outcome = value; }));
        QTRY_VERIFY(outcome.has_value());
        QVERIFY(!outcome->errorCode.isEmpty());
        QCOMPARE(server.requests.size(), std::size_t(1));
    }
    void imageEgressRechecksOriginalAuthority_data() {
        QTest::addColumn<QString>("change");
        for (const auto* change : {"view", "processing", "source", "grant-cycle", "modal"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void imageEgressRechecksOriginalAuthority() {
        QFETCH(QString, change);
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service);
        server.Push(Probe()); QVERIFY(Test(client));
        auto turn = Turn(service);
        target.beforeState = [&] {
            if (change == "view") service.NativeFieldsChanged({Field::ZoomAmount});
            else if (change == "processing") service.NativeFieldsChanged({Field::Contrast});
            else if (change == "source") {
                service.SourceChanged();
                service.SetGrants(turn.contextId, {true,false,false,false});
            } else if (change == "grant-cycle") {
                service.SetGrants(turn.contextId, {});
                service.SetGrants(turn.contextId, {true,false,false,false});
            } else target.modal = true;
        };
        server.Push(Tools({Call("observe", "view_observe"), Call("state", "app_state_get")}));
        server.Push(Final());
        std::optional<HttpAgentOutcome> outcome;
        QVERIFY(client.Start(turn, [&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 5000);
        QVERIFY(!outcome->errorCode.isEmpty());
        QCOMPARE(target.observations, 1);
        QCOMPARE(server.requests.size(), std::size_t(2)); // probe + initial text, no image POST
        QCOMPARE(Images(server.requests.back()["messages"].toArray()), 0);
    }
    void initTestCase() { QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy)); }
    void completedTextContextIsBoundedAndPixelFree() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service);
        server.Push(Probe()); QVERIFY(Test(client));
        auto turn = Turn(service);
        const QJsonArray context{QJsonObject{{"role","user"},{"content","earlier question"}},
            QJsonObject{{"role","assistant"},{"content","earlier answer"}}};
        turn.textHistory = context;
        server.Push(Final()); std::optional<HttpAgentOutcome> outcome;
        QVERIFY(client.Start(turn,[&](auto value) { outcome=value; }));
        QTRY_VERIFY(outcome.has_value()); QVERIFY(outcome->errorCode.isEmpty());
        const auto messages = server.requests.back()["messages"].toArray();
        QCOMPARE(messages.size(),4); QCOMPARE(messages[1],context[0]); QCOMPARE(messages[2],context[1]);
        QCOMPARE(Images(messages),0);
        auto rejected = turn;
        rejected.textHistory[0] = QJsonObject{{"role","user"},{"content","earlier"},{"image_url","private"}};
        QVERIFY(!client.Start(rejected,[](auto) {}));
        rejected.textHistory = QJsonArray{context[1],context[0]}; QVERIFY(!client.Start(rejected,[](auto) {}));
        rejected.textHistory = QJsonArray{context[0]}; QVERIFY(!client.Start(rejected,[](auto) {}));
        rejected.textHistory = QJsonArray{QJsonObject{{"role","user"},{"content",QString(33000,'a')}},context[1]};
        QVERIFY(!client.Start(rejected,[](auto) {}));
        QCOMPARE(server.requests.size(),std::size_t(2)); QCOMPARE(target.effects,0);
    }
    void explicitCapabilityAndProfileIsolation() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        auto config = Config(server); client.Configure(config); client.SetControlService(&service);
        QVERIFY(!client.Capability().available);
        const auto turn = Turn(service);
        QVERIFY(!client.Start(turn,[](HttpAgentOutcome) {}));
        QTest::qWait(20); QVERIFY(server.requests.empty());
        server.Push(Probe()); QVERIFY(Test(client)); QVERIFY(client.Capability().available);
        QCOMPARE(server.requests.size(),std::size_t(1));
        QCOMPARE(Images(server.requests[0]["messages"].toArray()),0);
        QVERIFY(!QJsonDocument(server.requests[0]).toJson().contains(config.apiKey));
        QVERIFY(server.headers[0].toLower().contains("authorization: bearer " + config.apiKey));
        config.model = "other-model"; client.Configure(config); QVERIFY(!client.Capability().available);
        QCOMPARE(server.requests.size(),std::size_t(1));
        config.model = "fixture-model"; client.Configure(config); QVERIFY(client.Capability().available);
        config.apiKey = "new-fixture-secret"; client.Configure(config); QVERIFY(!client.Capability().available);
    }
    void baseProbeOmitsOptionalFlagAndRefusesPlainTextVerdict() {
        Server server; HttpAgentClient client; client.Configure(Config(server)); server.Push(Probe());
        std::optional<HttpAgentCapability> capability; QString error;
        QVERIFY(client.TestCapability([&](auto value,auto code) { capability = value; error = code; }));
        QTRY_VERIFY_WITH_TIMEOUT(capability.has_value(),5000); QVERIFY(capability->available);
        QVERIFY(!capability->parallelToolCallsAccepted); QVERIFY(error.isEmpty());
        QVERIFY(!server.requests[0].contains("parallel_tool_calls"));
        server.Push(Final("Tool support is available.")); capability.reset();
        QVERIFY(client.TestCapability([&](auto value,auto code) { capability = value; error = code; }));
        QTRY_VERIFY_WITH_TIMEOUT(capability.has_value(),5000); QVERIFY(!capability->available);
        QCOMPARE(error,"invalid_tool_round"); QVERIFY(!client.Capability().available);
    }
    void orderedToolsAndImagesAreNotResent() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service);
        server.Push(Probe()); QVERIFY(Test(client));
        const QJsonObject assistant{{"role","assistant"},{"content",QJsonValue::Null},{"tool_calls",QJsonArray{
            Call("zoom","view_zoom_set",QJsonObject{{"magnification",2.0}}),Call("observe","view_observe",QStringLiteral(" { } "))}}};
        server.Push(Answer(assistant));
        server.Push(Tools({Call("state","app_state_get")})); server.Push(Final());
        std::optional<HttpAgentOutcome> outcome;
        QVERIFY(client.Start(Turn(service,"initial-jpeg"),[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000);
        QVERIFY(outcome->errorCode.isEmpty()); QCOMPARE(outcome->text,"Done");
        QCOMPARE(target.effects,1); QCOMPARE(target.observations,1); QCOMPARE(server.requests.size(),std::size_t(4));
        const auto first = server.requests[1]["messages"].toArray(); QCOMPARE(Images(first),1);
        const auto second = server.requests[2]["messages"].toArray(); QCOMPARE(Images(second),1);
        QCOMPARE(second[2].toObject(),assistant);
        QCOMPARE(second[3].toObject()["role"].toString(),"tool"); QCOMPARE(second[3].toObject()["tool_call_id"].toString(),"zoom");
        QCOMPARE(second[4].toObject()["role"].toString(),"tool"); QCOMPARE(second[4].toObject()["tool_call_id"].toString(),"observe");
        QCOMPARE(second[5].toObject()["role"].toString(),"user");
        QCOMPARE(Images(server.requests[3]["messages"].toArray()),0);
        QCOMPARE(server.requests[1]["parallel_tool_calls"].toBool(),false);
    }
    void malformedRoundHasNoPartialMutation_data() {
        QTest::addColumn<QJsonArray>("calls");
        const auto valid = Call("first","view_zoom_set",QJsonObject{{"magnification",2.0}});
        QTest::newRow("duplicate") << QJsonArray{valid,Call("first","app_state_get")};
        auto missing = Call("second","app_state_get"); missing.remove("id");
        QTest::newRow("missing-id") << QJsonArray{valid,missing};
        QTest::newRow("bad-id") << QJsonArray{valid,Call("bad/id","app_state_get")};
        QTest::newRow("array-args") << QJsonArray{valid,Call("second","app_state_get",QJsonArray{})};
        QTest::newRow("string-array-args") << QJsonArray{valid,Call("second","app_state_get",QStringLiteral("[]"))};
        QTest::newRow("malformed-json") << QJsonArray{valid,Call("second","app_state_get",QStringLiteral("{"))};
        QTest::newRow("unknown-tool") << QJsonArray{valid,Call("second","shell_execute")};
        QTest::newRow("oversize-args") << QJsonArray{valid,Call("second","app_state_get",QString(17000,'x'))};
        QTest::newRow("schema-invalid") << QJsonArray{valid,Call("second","view_zoom_set",QJsonObject{{"magnification",100}})};
    }
    void malformedRoundHasNoPartialMutation() {
        QFETCH(QJsonArray,calls);
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        server.Push(Tools(calls)); std::optional<HttpAgentOutcome> outcome;
        QVERIFY(client.Start(Turn(service),[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000);
        QCOMPARE(outcome->errorCode,"invalid_tool_round"); QCOMPARE(target.effects,0); QCOMPARE(server.requests.size(),std::size_t(2));
    }
    void legacyLoopUsesFunctionRoleAndPreservesArguments() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server,HttpAgentToolMode::LegacyFunctions)); client.SetControlService(&service);
        server.Push(Probe(true)); QVERIFY(Test(client));
        const QJsonObject assistant{{"role","assistant"},{"content",QJsonValue::Null},
            {"function_call",QJsonObject{{"name","view_zoom_set"},{"arguments"," { \"magnification\" : 3 } "}}}};
        server.Push(Answer(assistant,"function_call")); server.Push(Final());
        std::optional<HttpAgentOutcome> outcome;
        QVERIFY(client.Start(Turn(service),[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000); QVERIFY(outcome->errorCode.isEmpty());
        const auto request = server.requests[2]; QVERIFY(request.contains("functions")); QVERIFY(!request.contains("tools"));
        QVERIFY(!request.contains("parallel_tool_calls"));
        const auto messages = request["messages"].toArray(); QCOMPARE(messages[2].toObject(),assistant);
        QCOMPARE(messages[3].toObject()["role"].toString(),"function"); QCOMPARE(messages[3].toObject()["name"].toString(),"view_zoom_set");
        QVERIFY(!messages[3].toObject().contains("tool_call_id")); QCOMPARE(target.effects,1);
    }
    void cancelStopsRemainingCallsAndIgnoresLateCompletion() {
        Server server; Target target; target.delay = true; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        server.Push(Tools({Call("observe","view_observe"),Call("zoom","view_zoom_set",QJsonObject{{"magnification",2}})}));
        std::optional<HttpAgentOutcome> outcome; int completions = 0;
        auto turn = Turn(service);
        QVERIFY(client.Start(turn,[&](auto value) { outcome = value; ++completions; }));
        QTRY_VERIFY_WITH_TIMEOUT(bool(target.delayed),5000);
        client.Cancel(); QVERIFY(outcome); QVERIFY(outcome->interrupted); QCOMPARE(outcome->errorCode,"canceled");
        target.delayed({{{"ok",true}},"late-jpeg"}); QTest::qWait(30);
        QCOMPARE(completions,1); QCOMPARE(target.effects,0); QCOMPARE(server.requests.size(),std::size_t(2));
        service.End(turn.contextId);
    }
    void configurationCancelsHeldNetworkReply() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        auto config = Config(server); client.Configure(config); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        std::optional<HttpAgentOutcome> outcome; int completions = 0; bool restarted = false;
        const auto turn = Turn(service);
        QVERIFY(client.Start(turn,[&](auto value) {
            outcome = value; ++completions;
            if (value.interrupted) {
                restarted = client.Start(turn,[](auto) {});
                auto newer = config; newer.model = "newer-model";
                client.Configure(newer);
            }
        }));
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(),std::size_t(2),5000);
        config.model = "replacement-model"; client.Configure(config);
        QVERIFY(outcome); QVERIFY(outcome->interrupted); QCOMPARE(completions,1); QVERIFY(!client.Busy()); QVERIFY(!restarted);
        QTest::qWait(20); QCOMPARE(completions,1); QVERIFY(!client.Capability().available);
        server.Push(Probe()); QVERIFY(Test(client));
        QCOMPARE(server.requests.back()["model"].toString(),"newer-model");
    }
    void appContextEndRetiresHeldRequest() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        std::optional<HttpAgentOutcome> outcome; auto turn = Turn(service);
        QVERIFY(client.Start(turn,[&](auto value) { outcome = value; }));
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(),std::size_t(2),5000);
        service.End(turn.contextId);
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000); QCOMPARE(outcome->errorCode,"context_expired"); QVERIFY(!client.Busy());
    }
    void rejectsUnissuedContextsAndUnauthorizedInitialImage() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        auto turn = Turn(service,"fixture-image"); const auto issued = turn.contextId;
        turn.contextId = "unissued"; QVERIFY(!client.Start(turn,[](auto) {}));
        turn.contextId = issued; service.SetGrants(issued,{}); QVERIFY(!client.Start(turn,[](auto) {}));
        QCOMPARE(server.requests.size(),std::size_t(1)); QCOMPARE(target.effects,0);
    }
    void providerCannotEchoAuthorizationIntoTranscript() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        const auto config = Config(server); client.Configure(config); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        server.Push(Final(QString::fromUtf8(config.apiKey)));
        std::optional<HttpAgentOutcome> outcome;
        QVERIFY(client.Start(Turn(service),[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000); QCOMPARE(outcome->errorCode,"invalid_response"); QVERIFY(outcome->text.isEmpty());
    }
    void escapedAuthorizationEchoNeverEntersAnswer() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        auto config=Config(server); config.apiKey="fixture-\"secret";
        client.Configure(config); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        server.Push(Final(QString::fromUtf8(config.apiKey))); std::optional<HttpAgentOutcome> outcome;
        QVERIFY(client.Start(Turn(service),[&](auto value) {outcome=value;}));
        QTRY_VERIFY(outcome.has_value()); QCOMPARE(outcome->errorCode,"invalid_response"); QVERIFY(outcome->text.isEmpty());
        QCOMPARE(target.effects,0);
    }
    void errorsAreFixedCodesAndRedirectsNeverFollow() {
        Server server, redirected; Target target; AppControlService service(target); HttpAgentClient client;
        const auto config = Config(server); client.Configure(config); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        server.Push(QByteArray("private body ") + config.apiKey + " C:/private/notes",403);
        std::optional<HttpAgentOutcome> outcome;
        auto turn = Turn(service);
        QVERIFY(client.Start(turn,[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000); QCOMPARE(outcome->errorCode,"authentication_failed"); QVERIFY(outcome->text.isEmpty());
        service.End(turn.contextId,true); outcome.reset(); turn = Turn(service);
        server.Push({},302,"Location: " + redirected.Url().toEncoded() + "\r\n");
        QVERIFY(client.Start(turn,[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000); QCOMPARE(outcome->errorCode,"provider_unavailable");
        QVERIFY(redirected.requests.empty());
    }
    void repeatedIdsRejectNewRoundAndRoundLimitIsBounded() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        server.Push(Tools({Call("same","app_state_get")})); server.Push(Tools({Call("same","app_state_get")}));
        std::optional<HttpAgentOutcome> outcome; auto turn = Turn(service);
        QVERIFY(client.Start(turn,[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000); QCOMPARE(outcome->errorCode,"invalid_tool_round");
        service.End(turn.contextId,true); outcome.reset(); turn = Turn(service);
        for (int i=0;i<8;++i) server.Push(Tools({Call(QStringLiteral("round-%1").arg(i),"app_state_get")}));
        QVERIFY(client.Start(turn,[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000); QCOMPARE(outcome->errorCode,"budget_exhausted");
        QCOMPARE(server.requests.size(),std::size_t(11));
    }
    void oversizedAndIncompleteResponsesHaveNoEffects() {
        Server server; Target target; AppControlService service(target); HttpAgentClient client;
        client.Configure(Config(server)); client.SetControlService(&service); server.Push(Probe()); QVERIFY(Test(client));
        server.Push(QByteArray(1024*1024+1,'x'));
        std::optional<HttpAgentOutcome> outcome; auto turn = Turn(service);
        QVERIFY(client.Start(turn,[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000); QCOMPARE(outcome->errorCode,"response_too_large");
        service.End(turn.contextId,true); outcome.reset(); turn = Turn(service);
        server.Push(Answer({{"role","assistant"},{"tool_calls",QJsonArray{Call("zoom","view_zoom_set",QJsonObject{{"magnification",2}})}}},"length"));
        QVERIFY(client.Start(turn,[&](auto value) { outcome = value; }));
        QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(),5000); QCOMPARE(outcome->errorCode,"incomplete_response"); QCOMPARE(target.effects,0);
    }
};
QTEST_GUILESS_MAIN(HttpAgentTests)
#include "http_agent_client_tests.moc"
