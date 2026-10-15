#include "mcp_jpeg_fixture.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/mcp/protocol.hpp"
#include "okuflow/mcp/windows_pipe.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTest>
#include <set>
#include <thread>

using namespace okuflow::mcp;
class FakeChannel final : public AppChannel {
  public:
    std::string name, key, tool;
    Json args;
    Done done;
    int calls{}, cancels{}, closes{};
    void SetClientName(std::string n) override { name = std::move(n); }
    void Call(std::string k, std::string t, Json a, Done d) override {
        key = std::move(k);
        tool = std::move(t);
        args = std::move(a);
        done = std::move(d);
        ++calls;
    }
    void Cancel(std::string_view) override { ++cancels; }
    void Close() override { ++closes; }
};
const char *Initialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2099-01-01","capabilities":{},"clientInfo":{"name":"client <bad>\n","version":"1.0"}}})";
const char *Initialized = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
Json Read(QProcess &p, int timeout = 8000) {
    QElapsedTimer clock;
    clock.start();
    while (!p.canReadLine()) {
        if (clock.elapsed() > timeout)
            return {};
        p.waitForReadyRead(100);
        if (p.state() == QProcess::NotRunning && !p.canReadLine())
            return {};
    }
    auto bytes = p.readLine().trimmed();
    try {
        return Json::Parse(std::string_view(bytes.constData(), bytes.size()));
    } catch (...) {
        return {};
    }
}
void Write(QProcess &p, std::string_view bytes) {
    p.write(bytes.data(), static_cast<qint64>(bytes.size()));
    p.write("\n");
    p.waitForBytesWritten(2000);
}
Json WaitType(QProcess &p, std::string_view type) {
    for (int i = 0; i < 8; ++i) {
        auto j = Read(p);
        auto t = j.Find("type");
        if (t && t->IsString() && t->String() == type)
            return j;
        if (j.IsNull())
            break;
    }
    return {};
}
void HideFixture(QProcess &process) {
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
}
class McpTests final : public QObject {
    Q_OBJECT
  private slots:
#ifdef Q_MOC_RUN
    // Keep Qt's moc parser away from the schema fixture's nested raw literals;
    // the real compiler builds the complete assertions below.
    void expandedParameterRegistry();
    void expandedMetadataPages();
    void expandedProtocolAdmission();
    void assistantProjectionProtocolAdmission();
    void schemaParityBounds();
#else
    void expandedParameterRegistry() {
        using namespace okuflow::agent;
        QCOMPARE(ControlRegistry(CatalogEdition::DevCore).size(), std::size_t(14));
        QCOMPARE(ControlRegistry(CatalogEdition::DevCapture).size(), std::size_t(17));
        QCOMPARE(ControlRegistry(CatalogEdition::DevCamera).size(), std::size_t(19));
        QCOMPARE(ControlRegistry(CatalogEdition::DevFull).size(), std::size_t(36));
        QCOMPARE(ControlRegistry(CatalogEdition::V1).size(), std::size_t(36));
        QCOMPARE(static_cast<int>(CatalogEdition::V1), 3);
        QCOMPARE(static_cast<int>(CatalogEdition::DevFull), 4);
        QVERIFY(!FindControlTool("image.settings.set", CatalogEdition::DevCamera));
        QVERIFY(FindControlTool("image_settings_set", CatalogEdition::DevFull));
        QCOMPARE(ParameterRegistry().size(), std::size_t(94));
        QFile inventory(QFINDTESTDATA("data/app_control_inventory.json"));
        QVERIFY(inventory.open(QIODevice::ReadOnly));
        auto source = QJsonDocument::fromJson(inventory.readAll()).object().value("controls").toArray();
        QCOMPARE(source.size(), qsizetype(94));
        std::set<std::string_view> ids;
        for (const auto &field : ParameterRegistry()) {
            QVERIFY(ids.insert(field.id).second);
            auto schema = Json::Parse(field.schema);
            QVERIFY(schema.Find("type"));
            QVERIFY(!MatchesSchema(Json(), schema));
            QVERIFY(!field.nativeSource.empty());
            QVERIFY(!field.nativeHandler.empty());
            bool found = false;
            for (const auto &record : source)
                if (record.toObject()["fieldId"].toString().toStdString() == field.id) {
                    found = true;
                    QCOMPARE(record.toObject()["native"].toObject()["source"].toString().toStdString(),
                             std::string(field.nativeSource));
                }
            QVERIFY(found);
        }
        for (int i = 0; i < 8; ++i) {
            const auto group = static_cast<ParameterGroup>(i);
            const auto setSchema = Json::Parse(ParameterSetSchema(group));
            const auto getSchema = Json::Parse(ParameterGetSchema(group));
            QVERIFY(!MatchesSchema(Json::Object{}, setSchema));
            QVERIFY(!MatchesSchema(Json::Object{{"secret", "never"}}, setSchema));
            QVERIFY(MatchesSchema(Json::Object{}, getSchema));
            auto field = std::find_if(ParameterRegistry().begin(), ParameterRegistry().end(),
                                      [group](const auto &p) { return p.group == group; });
            QVERIFY(field != ParameterRegistry().end());
            QVERIFY(MatchesSchema(Json::Object{{"fields", Json::Array{std::string(field->id)}}}, getSchema));
            QVERIFY(!MatchesSchema(
                Json::Object{{"fields", Json::Array{std::string(field->id), std::string(field->id)}}},
                getSchema));
            QVERIFY(!MatchesSchema(Json::Object{{"fields", Json::Array{}}}, getSchema));
            ParameterGroup mapped{};
            bool setter{};
            const auto tool = static_cast<ToolId>(static_cast<int>(ToolId::ImageSettingsGet) + i * 2);
            QVERIFY(ParameterGroupForTool(tool, mapped, setter));
            QCOMPARE(mapped, group);
            QVERIFY(!setter);
            QVERIFY(ParameterGroupForTool(static_cast<ToolId>(static_cast<int>(tool) + 1), mapped, setter));
            QVERIFY(setter);
        }
        auto image = Json::Parse(ParameterSetSchema(ParameterGroup::Image));
        QVERIFY(MatchesSchema(
            Json::Object{{"image.textClarity.adaptive.sauvolaStrength", 0.28}, {"image.blur.radius", 15}},
            image));
        QVERIFY(!MatchesSchema(Json::Object{{"image.blur.radius", 12}}, image));
        QVERIFY(!MatchesSchema(Json::Object{{"image.textClarity.adaptive.sauvolaStrength", 0.9}}, image));
        QVERIFY(MatchesSchema(Json::Object{{"image.colors.custom.stops", Json::Array{"#000000", "#ffffff"}}},
                              image));
        QVERIFY(!MatchesSchema(Json::Object{{"image.colors.custom.stops", Json::Array{"#000000"}}}, image));
        QVERIFY(!MatchesSchema(
            Json::Object{{"image.colors.custom.stops", Json::Array{"#000000", "file:///private"}}}, image));
        auto ui = Json::Parse(ParameterSetSchema(ParameterGroup::Ui));
        QVERIFY(!MatchesSchema(Json::Object{{"ui.sectionExpanded", Json::Object{{"unknown", true}}}}, ui));
        QVERIFY(!MatchesSchema(Json::Object{{"ui.sectionExpanded", Json::Object{{"magnification", "true"}}}},
                               ui));
        auto notes = Json::Parse(ParameterSetSchema(ParameterGroup::Notes));
        QVERIFY(MatchesSchema(Json::Object{{"notes_output.root", "dir-0123456789abcdef0123456789abcdef"}},
                              notes));
        QVERIFY(!MatchesSchema(Json::Object{{"notes_output.root", "C:/private"}}, notes));
        auto assistant = Json::Parse(ParameterSetSchema(ParameterGroup::Assistant));
        for (auto denied : {"assistant.apiKey", "assistant.credentialId", "assistant.controlEnabled",
                            "assistant.codingEnabled", "assistant.externalLease.frames"})
            QVERIFY(!MatchesSchema(Json::Object{{denied, true}}, assistant));
        QVERIFY(!MatchesSchema(Json::Object{{"assistant_preferences.instructions", std::string(4097, 'x')}},
                               assistant));
        const auto assistantGet = Json::Parse(ParameterGetSchema(ParameterGroup::Assistant));
        QCOMPARE(assistantGet.Find("properties")->Find("valueKind")->Find("default")->String(),
                 std::string("all"));
        for (const auto kind : {"all", "requested", "saved"})
            QVERIFY(MatchesSchema(Json::Object{{"valueKind", kind},
                                               {"fields", Json::Array{"assistant_preferences.instructions"}}},
                                  assistantGet));
        for (const auto invalid : {Json("effective"), Json("ALL"), Json(false), Json(), Json(1)})
            QVERIFY(!MatchesSchema(Json::Object{{"valueKind", invalid}}, assistantGet));
        for (int i = 0; i < 8; ++i)
            if (static_cast<ParameterGroup>(i) != ParameterGroup::Assistant)
                QVERIFY(!MatchesSchema(Json::Object{{"valueKind", "saved"}},
                                       Json::Parse(ParameterGetSchema(static_cast<ParameterGroup>(i)))));
    }
    void expandedMetadataPages() {
        using namespace okuflow::agent;
        std::string after;
        std::set<std::string> ids;
        int parameters = 0, protectedEntries = 0, actions = 0;
        for (int page = 0; page < 20; ++page) {
            const auto bytes = ParameterMetadataPage({}, after, 16, CatalogEdition::DevFull);
            QVERIFY(bytes.size() <= MaxStructuredResult);
            auto metadata = Json::Parse(bytes);
            QVERIFY(metadata.Find("ok")->Boolean());
            QCOMPARE(metadata.Find("catalogEdition")->String(), std::string("dev-full"));
            const auto &items = metadata.Find("items")->Elements();
            QVERIFY(items.size() <= 16);
            for (const auto &item : items) {
                auto id = item.Find("id")->String();
                QVERIFY(ids.insert(id).second);
                const auto kind = item.Find("kind")->String();
                QVERIFY(!item.Find("value"));
                QVERIFY(!item.Find("current"));
                if (kind == "parameter") {
                    ++parameters;
                    QCOMPARE(item.Find("implementation")->String(),
                             std::string("target-integration-required"));
                } else if (kind == "protected") {
                    ++protectedEntries;
                    QVERIFY(item.Find("requiresUi")->Boolean());
                    QVERIFY(!item.Find("writable")->Boolean());
                    QVERIFY(!item.Find("schema"));
                } else {
                    ++actions;
                    QVERIFY(item.Find("status"));
                }
            }
            if (!metadata.Find("hasMore")->Boolean())
                break;
            auto next = metadata.Find("afterField");
            QVERIFY(next);
            QVERIFY(next->String() != after);
            after = next->String();
        }
        QCOMPARE(parameters, 94);
        QCOMPARE(protectedEntries, 17);
        QCOMPARE(actions, 39);
        QVERIFY(!Json::Parse(ParameterMetadataPage("image", "not-a-field", 16)).Find("ok")->Boolean());
        QVERIFY(!Json::Parse(ParameterMetadataPage("wrong-group")).Find("ok")->Boolean());
        QVERIFY(!Json::Parse(ParameterMetadataPage({}, {}, 17)).Find("ok")->Boolean());
    }
    void expandedProtocolAdmission() {
        using namespace okuflow::agent;
        FakeChannel channel;
        std::vector<Json> responses;
        Protocol protocol(
            channel, [&](std::string value) { responses.push_back(Json::Parse(value)); },
            CatalogEdition::DevFull);
        protocol.Line(Initialize);
        protocol.Line(Initialized);
        protocol.Line(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
        QCOMPARE(responses.back().Find("result")->Find("tools")->Elements().size(), std::size_t(36));
        for (auto args :
             {R"({})", R"({"image.blur.enabled":"true"})", R"({"image.colors.custom.stops":["#000000"]})",
              R"({"assistant.apiKey":"never"})"}) {
            Json request = Json::Object{
                {"jsonrpc", "2.0"},
                {"id", 3},
                {"method", "tools/call"},
                {"params", Json::Object{{"name", "image.settings.set"}, {"arguments", Json::Parse(args)}}}};
            protocol.Line(request.Dump());
            QVERIFY(responses.back().Find("result")->Find("isError")->Boolean());
        }
        QCOMPARE(channel.calls, 0);
        protocol.Line(
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"image.settings.set","arguments":{"image.blur.enabled":true}}})");
        QCOMPARE(channel.calls, 1);
        channel.done({Json::Object{{"ok", false},
                                   {"error", Json::Object{{"code", "unavailable"}, {"retryable", false}}}},
                      {}});
        QVERIFY(responses.back().Find("result")->Find("isError")->Boolean());
        QVERIFY(!responses.back().Find("result")->Find("structuredContent"));
    }
    void assistantProjectionProtocolAdmission() {
        using namespace okuflow::agent;
        FakeChannel channel;
        std::vector<Json> responses;
        Protocol protocol(
            channel, [&](std::string value) { responses.push_back(Json::Parse(value)); },
            CatalogEdition::DevFull);
        protocol.Line(Initialize);
        protocol.Line(Initialized);
        protocol.Line(
            R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"assistant.preferences.get","arguments":{"valueKind":"effective"}}})");
        QVERIFY(responses.back().Find("result")->Find("isError")->Boolean());
        QCOMPARE(channel.calls, 0);
        protocol.Line(
            R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"assistant.preferences.get","arguments":{"valueKind":"saved","fields":["assistant_preferences.instructions"]}}})");
        QCOMPARE(channel.calls, 1);
        QCOMPARE(channel.args.Find("valueKind")->String(), std::string("saved"));
        channel.done({Json::Object{{"ok", true},
                                   {"parameters", Json::Object{{"assistant_preferences.instructions",
                                                                Json::Object{{"saved", "synthetic"}}}}}},
                      {}});
        QVERIFY(!responses.back().Find("result")->Find("isError")->Boolean());
    }
    void schemaParityBounds() {
        auto array = Json::Parse(
            R"({"type":"array","items":{"type":"integer","minimum":1},"minItems":1,"maxItems":2,"uniqueItems":true})");
        QVERIFY(MatchesSchema(Json::Array{1, 2}, array));
        QVERIFY(!MatchesSchema(Json::Array{1, 1}, array));
        QVERIFY(!MatchesSchema(Json::Array{1, "2"}, array));
        QVERIFY(!MatchesSchema(Json::Array{}, array));
        QVERIFY(!MatchesSchema(Json::Array{1, 2, 3}, array));
        auto object = Json::Parse(
            R"({"type":"object","minProperties":1,"maxProperties":1,"additionalProperties":{"type":"boolean"}})");
        QVERIFY(MatchesSchema(Json::Object{{"x", true}}, object));
        QVERIFY(!MatchesSchema(Json::Object{{"x", 1}}, object));
        QVERIFY(!MatchesSchema(Json::Object{}, object));
        QVERIFY(!MatchesSchema(Json::Object{{"x", true}, {"y", false}}, object));
        QVERIFY(MatchesSchema(Json(), Json::Parse(R"({"type":"null"})")));
        QVERIFY(!MatchesSchema(Json(), Json::Parse(R"({"type":"object"})")));
        QVERIFY(!MatchesSchema(Json::Object{}, Json::Parse(R"({"type":"unknown"})")));
        QVERIFY(MatchesSchema(Json::Parse(R"("\u00e9")"), Json::Parse(R"({"type":"string","maxLength":1})")));
        QVERIFY(!MatchesSchema(Json::Parse(R"("\u00e9\u00e9")"),
                               Json::Parse(R"({"type":"string","maxLength":1})")));
        QVERIFY(!MatchesSchema(Json("x"), Json::Parse(R"({"type":"string","pattern":"["})")));
        QVERIFY(MatchesSchema(Json::Object{}, Json(true)));
        QVERIFY(!MatchesSchema(Json::Object{}, Json(false)));
    }
#endif
    void buildInfoDiagnostic() {
        QProcess process;
        HideFixture(process);
        process.setProgram(QString::fromUtf8(OKUFLOW_MCP_BRIDGE_FIXTURE));
        process.setArguments({"--build-info"});
        process.start();
        QVERIFY(process.waitForFinished(3000));
        QCOMPARE(process.exitCode(), 0);
        auto bytes = process.readAllStandardOutput();
        QCOMPARE(bytes.count('\n'), 1);
        auto info = Json::Parse(std::string_view(bytes.constData(), bytes.size()));
        QCOMPARE(info.Members().size(), std::size_t(5));
        QCOMPARE(info.Find("protocol")->String(), std::string(ProtocolVersion));
        QCOMPARE(info.Find("catalogEdition")->String(), std::string(okuflow::agent::CatalogEditionName(
                                                            okuflow::agent::BuiltControlCatalogEdition())));
        QCOMPARE(info.Find("deployedImagePolicy")->Boolean(),
                 QString::fromUtf8(OKUFLOW_MCP_APP_FIXTURE).endsWith("OkuFlowApp.exe"));
        QVERIFY(process.readAllStandardError().isEmpty());
        process.setArguments({"--build-info", "unexpected"});
        process.start();
        QVERIFY(process.waitForFinished(3000));
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(process.readAllStandardOutput().isEmpty());
        QCOMPARE(process.readAllStandardError(), QByteArray("configuration_invalid\r\n"));
    }
    void devFullProcessSubset() {
        using namespace okuflow::agent;
        if (BuiltControlCatalogEdition() != CatalogEdition::DevFull)
            QSKIP("Explicit dev-full bridge/fixture build required; default remains dev-camera");
        QProcess app;
        HideFixture(app);
        app.setProgram(QString::fromUtf8(OKUFLOW_MCP_APP_FIXTURE));
        app.start();
        QVERIFY(app.waitForStarted());
        const auto ready = WaitType(app, "ready");
        QVERIFY(ready.Find("selector"));
        QProcess bridge;
        HideFixture(bridge);
        bridge.setProgram(QString::fromUtf8(OKUFLOW_MCP_BRIDGE_FIXTURE));
        bridge.setArguments({"--instance", QString::fromStdString(ready.Find("selector")->String())});
        bridge.start();
        QVERIFY(bridge.waitForStarted());
        Write(bridge, Initialize);
        QVERIFY(Read(bridge).Find("result"));
        Write(bridge, Initialized);
        const auto pending = WaitType(app, "pending");
        QVERIFY(pending.Find("peerId"));
        Write(app, "approve:" + pending.Find("peerId")->String());
        QVERIFY(WaitType(app, "approved").Find("ok")->Boolean());
        Write(bridge, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
        QCOMPARE(Read(bridge).Find("result")->Find("tools")->Elements().size(), std::size_t(36));
        Write(
            bridge,
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"app.controls.list","arguments":{"group":"image","limit":16}}})");
        auto page = Read(bridge);
        auto content = page.Find("result")->Find("structuredContent");
        QVERIFY(content);
        QCOMPARE(content->Find("catalogEdition")->String(), std::string("dev-full"));
        QCOMPARE(content->Find("items")->Elements().size(), std::size_t(16));
        Write(
            bridge,
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"view.settings.get","arguments":{"fields":["view.zoom.amount"]}}})");
        auto read = Read(bridge);
        auto subset = read.Find("result")->Find("structuredContent");
        QVERIFY(subset);
        QCOMPARE(subset->Find("support")->String(), std::string("synthetic-read-subset"));
        QCOMPARE(subset->Find("parameters")->Find("view.zoom.amount")->Number(), 1.0);
        Write(
            bridge,
            R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"image.settings.set","arguments":{"image.blur.enabled":true}}})");
        auto denied = Read(bridge);
        QVERIFY(denied.Find("result")->Find("isError")->Boolean());
        QCOMPARE(Json::Parse(denied.Find("result")->Find("content")->Elements()[0].Find("text")->String())
                     .Find("error")
                     ->Find("code")
                     ->String(),
                 std::string("unavailable"));
        Write(app, "fixture-status");
        QCOMPARE(WaitType(app, "fixture-status").Find("writes")->Number(), 0.0);
        bridge.closeWriteChannel();
        QVERIFY(bridge.waitForFinished(8000));
        QCOMPARE(bridge.exitCode(), 0);
        Write(app, "exit");
        app.closeWriteChannel();
        QVERIFY(app.waitForFinished(8000));
        QCOMPARE(app.exitCode(), 0);
    }
    void acceptedCaptureCancellation() {
        if (okuflow::agent::BuiltControlCatalogEdition() == okuflow::agent::CatalogEdition::DevCore)
            QSKIP("Capture tools absent in immutable dev-core catalog");
        QProcess app;
        HideFixture(app);
        app.setProgram(QString::fromUtf8(OKUFLOW_MCP_APP_FIXTURE));
        app.start();
        QVERIFY(app.waitForStarted());
        const auto ready = WaitType(app, "ready");
        QVERIFY(ready.Find("selector"));
        QProcess bridge;
        HideFixture(bridge);
        bridge.setProgram(QString::fromUtf8(OKUFLOW_MCP_BRIDGE_FIXTURE));
        bridge.setArguments({"--instance", QString::fromStdString(ready.Find("selector")->String())});
        bridge.start();
        QVERIFY(bridge.waitForStarted());
        Write(bridge, Initialize);
        QVERIFY(Read(bridge).Find("result"));
        Write(bridge, Initialized);
        const auto pending = WaitType(app, "pending");
        QVERIFY(pending.Find("peerId"));
        Write(app, "approve-all:" + pending.Find("peerId")->String());
        QVERIFY(WaitType(app, "approved").Find("ok")->Boolean());
        Write(app, "hold-photo");
        QVERIFY(WaitType(app, "photo-held").IsObject());
        Write(bridge,
              R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"capture.photo.take"}})");
        const auto accepted = WaitType(app, "photo-accepted");
        QVERIFY(accepted.Find("operationId"));
        Write(bridge, R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":2}})");
        Write(bridge, R"({"jsonrpc":"2.0","id":8,"method":"ping"})");
        QCOMPARE(Read(bridge).Find("id")->Number(), 8.0);
        Write(app, "release-photo");
        QVERIFY(WaitType(app, "photo-released").IsObject());
        Write(app, "settle");
        QVERIFY(WaitType(app, "settled").IsObject());
        Json request(Json::Object{
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "tools/call"},
            {"params", Json::Object{{"name", "app.operation.get"},
                                    {"arguments", Json::Object{{"operationId",
                                                                accepted.Find("operationId")->String()}}}}}});
        Write(bridge, request.Dump());
        auto terminal = Read(bridge);
        QCOMPARE(terminal.Find("id")->Number(), 3.0);
        QCOMPARE(terminal.Find("result")->Find("structuredContent")->Find("state")->String(),
                 std::string("committed"));
        Write(app, "revoke");
        QVERIFY(WaitType(app, "revoked").IsObject());
        Write(app, "approve-all:" + pending.Find("peerId")->String());
        QVERIFY(WaitType(app, "approved").Find("ok")->Boolean());
        Write(bridge, request.Dump());
        QCOMPARE(Read(bridge).Find("result")->Find("structuredContent")->Find("state")->String(),
                 std::string("committed"));
        bridge.closeWriteChannel();
        QVERIFY(bridge.waitForFinished(8000));
        QVERIFY(bridge.readAllStandardOutput().isEmpty());
        QCOMPARE(bridge.exitCode(), 0);
        Write(app, "exit");
        app.closeWriteChannel();
        QVERIFY(app.waitForFinished(8000));
        QCOMPARE(app.exitCode(), 0);
    }
    void parser() {
        auto j = Json::Parse(R"({"a":"\ud83d\ude00","b":[true,1.5,null]})");
        QVERIFY(j.IsObject());
        QCOMPARE(Json::Parse(j.Dump()).Dump(), j.Dump());
        for (auto s : {"{\"a\":1,\"a\":2}", "01", "1e999", "\"\\ud800\"", "[1,]"}) {
            bool rejected = false;
            try {
                Json::Parse(s);
            } catch (...) {
                rejected = true;
            }
            QVERIFY(rejected);
        }
        QCOMPARE(Base64("a"), std::string("YQ=="));
    }
    void lifecycle() {
        FakeChannel c;
        std::vector<Json> out;
        Protocol p(c, [&](std::string s) { out.push_back(Json::Parse(s)); });
        p.Line(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
        QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32002.0);
        p.Line(Initialize);
        QCOMPARE(out.back().Find("result")->Find("protocolVersion")->String(), std::string(ProtocolVersion));
        QVERIFY(c.name.find('<') == std::string::npos);
        p.Line(R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
        QVERIFY(out.back().Find("result")->IsObject());
        p.Line(Initialize);
        QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32600.0);
        p.Line(R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":[]})");
        QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32600.0);
        p.Line(Initialized);
        p.Line(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{"progressToken":"a"}}})");
        auto tools = out.back().Find("result")->Find("tools");
        QCOMPARE(tools->Elements().size(),
                 okuflow::agent::ControlRegistry(okuflow::agent::BuiltControlCatalogEdition()).size());
        for (const auto &t : tools->Elements()) {
            QVERIFY(t.Find("inputSchema")->Find("$schema"));
            QVERIFY(t.Find("outputSchema")->Find("oneOf"));
            const auto *descriptor = okuflow::agent::FindControlTool(
                t.Find("name")->String(), okuflow::agent::BuiltControlCatalogEdition());
            QVERIFY(descriptor);
            QCOMPARE(t.Find("annotations")->Find("destructiveHint")->Boolean(),
                     okuflow::agent::ControlToolDestructiveHint(descriptor->id));
        }
        using okuflow::agent::ControlToolDestructiveHint;
        using okuflow::agent::ToolId;
        QVERIFY(ControlToolDestructiveHint(ToolId::RecordingStop));
        QVERIFY(ControlToolDestructiveHint(ToolId::CameraSelect));
        QVERIFY(ControlToolDestructiveHint(ToolId::RecordingSettingsSet));
        QVERIFY(ControlToolDestructiveHint(ToolId::AnnotationPreferencesSet));
        QVERIFY(!ControlToolDestructiveHint(ToolId::Photo));
        QVERIFY(!ControlToolDestructiveHint(ToolId::RecordingStart));
        QVERIFY(!ControlToolDestructiveHint(ToolId::Observe));
        for (auto cursor : {"null", "\"\""}) {
            p.Line(std::string(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"cursor":)") +
                   cursor + "}}");
            QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32602.0);
        }
        p.Line(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"missing"}})");
        QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32602.0);
    }
    void envelopes() {
        FakeChannel c;
        std::vector<Json> out;
        Protocol p(c, [&](std::string s) { out.push_back(Json::Parse(s)); });
        p.Line(Initialize);
        p.Line(Initialized);
        p.Line(
            R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"view.observe","arguments":{},"_meta":{"progressToken":null}}})");
        QCOMPARE(c.calls, 1);
        const auto jpeg = McpFixtureJpeg();
        QVERIFY(!jpeg.isEmpty());
        c.done({Json::Object{{"ok", true}, {"observationId", "fixture"}}, jpeg.toStdString()});
        const auto &result = *out.back().Find("result");
        QVERIFY(result.Find("structuredContent"));
        QCOMPARE(result.Find("content")->Elements().size(), size_t(2));
        QVERIFY(!result.Find("structuredContent")->Find("jpeg"));
        p.Line(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"app.state.get"}})");
        c.done({Failure("denied_by_policy"), "do not emit"});
        QVERIFY(out.back().Find("result")->Find("isError")->Boolean());
        QVERIFY(!out.back().Find("result")->Find("structuredContent"));
        QCOMPARE(out.back().Find("result")->Find("content")->Elements().size(), size_t(1));
        p.Line(
            R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"app.state.get","task":{}}})");
        QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32602.0);
    }
    void cancelAndReuse() {
        FakeChannel c;
        std::vector<Json> out;
        Protocol p(c, [&](std::string s) { out.push_back(Json::Parse(s)); });
        p.Line(Initialize);
        p.Line(Initialized);
        std::string call =
            R"({"jsonrpc":"2.0","id":"same","method":"tools/call","params":{"name":"app.state.get"}})";
        p.Line(call);
        auto original = c.done;
        p.Line(call);
        QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32600.0);
        p.Line(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"same"}})");
        QCOMPARE(c.cancels, 1);
        auto count = out.size();
        original({Json::Object{{"ok", true}}, {}});
        QCOMPARE(out.size(), count);
        p.Line(call);
        original({Json::Object{{"ok", true}}, {}});
        QCOMPARE(out.size(), count);
        c.done({Json::Object{{"ok", true}}, {}});
        QCOMPARE(out.size(), count + 1);
    }
    void malformedBounds() {
        FakeChannel c;
        std::vector<Json> out;
        Protocol p(c, [&](std::string s) { out.push_back(Json::Parse(s)); });
        p.Line("[]");
        QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32600.0);
        p.Line(R"({"jsonrpc":"2.0","id":null,"method":"ping"})");
        QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32600.0);
        QVERIFY(p.Line("{"));
        QVERIFY(p.OversizedLine());
        QVERIFY(!p.Line("invalid"));
        QCOMPARE(out.back().Find("error")->Find("code")->Number(), -32700.0);
    }
    void catalogSchemas() {
        for (auto edition :
             {okuflow::agent::CatalogEdition::DevCore, okuflow::agent::CatalogEdition::DevCapture,
              okuflow::agent::CatalogEdition::DevCamera}) {
            for (const auto &tool : okuflow::agent::ControlRegistry(edition)) {
                const auto output = Json::Parse(tool.outputSchema);
                QVERIFY(output.Find("type"));
                QCOMPARE(output.Find("type")->String(), std::string("object"));
                QVERIFY(MatchesSchema(Json::Object{{"ok", true}}, output));
                QVERIFY(!MatchesSchema(Failure("unavailable"), output));
                const Json pending = Json::Object{
                    {"ok", true}, {"pending", true}, {"operationId", "opaque"}, {"state", "queued"}};
                QCOMPARE(MatchesSchema(pending, output), tool.pending);
                QVERIFY(!MatchesSchema(Json::Object{{"ok", true}, {"pending", true}}, output));
            }
        }
        const auto *zoom =
            okuflow::agent::FindControlTool("view.zoom.set", okuflow::agent::CatalogEdition::DevCore);
        const auto schema = Json::Parse(zoom->inputSchema);
        QVERIFY(MatchesSchema(Json::Object{{"magnification", 2}}, schema));
        QVERIFY(!MatchesSchema(Json::Object{{"magnification", 20}}, schema));
        QVERIFY(!MatchesSchema(Json::Object{{"magnification", 2}, {"shell", "never"}}, schema));
        const auto stateSchema = Json::Parse(
            okuflow::agent::FindControlTool("app.state.get", okuflow::agent::CatalogEdition::DevCore)
                ->inputSchema);
        QVERIFY(MatchesSchema(Json::Object{{"sinceEvent", 9007199254740991.0}}, stateSchema));
        QVERIFY(!MatchesSchema(Json::Object{{"sinceEvent", 9007199254740992.0}}, stateSchema));
        QVERIFY(!MatchesSchema(Json::Object{{"sinceEvent", 1e100}}, stateSchema));
        for (const auto name : {"view.center.set", "view.zoom.set"}) {
            const auto pointSchema = Json::Parse(
                okuflow::agent::FindControlTool(name, okuflow::agent::CatalogEdition::DevCore)->inputSchema);
            const auto argument = std::string(name) == "view.center.set" ? "point" : "anchor";
            Json::Object args;
            if (argument == std::string("anchor"))
                args["magnification"] = 2;
            args[argument] = Json::Object{
                {"space", "observation"}, {"observationId", "fixture"}, {"px", 1024}, {"py", 1024}};
            QVERIFY(MatchesSchema(args, pointSchema));
            args[argument].Members()["px"] = 1024.01;
            QVERIFY(!MatchesSchema(args, pointSchema));
            args[argument].Members()["px"] = 0;
            args[argument].Members()["py"] = 1e100;
            QVERIFY(!MatchesSchema(args, pointSchema));
        }
    }
    void queueAndArguments() {
        FakeChannel c;
        std::vector<Json> out;
        Protocol p(c, [&](std::string s) { out.push_back(Json::Parse(s)); });
        p.Line(Initialize);
        p.Line(Initialized);
        p.Line(
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"view.zoom.set","arguments":{"magnification":99}}})");
        QCOMPARE(c.calls, 0);
        QVERIFY(out.back().Find("result")->Find("isError")->Boolean());
        for (int i = 0; i < 9; ++i)
            p.Line(Json(Json::Object{{"jsonrpc", "2.0"},
                                     {"id", 10 + i},
                                     {"method", "tools/call"},
                                     {"params", Json::Object{{"name", "app.state.get"}}}})
                       .Dump());
        QCOMPARE(c.calls, 8);
        QVERIFY(out.back().Find("result")->Find("isError")->Boolean());
        c.done({Json::Object{{"ok", true}, {"payload", std::string(MaxStructuredResult, 'a')}}, {}});
        QVERIFY(out.back().Find("result")->Find("isError")->Boolean());
        QVERIFY(!out.back().Find("result")->Find("structuredContent"));
    }
    void absentAppAndSplitUtf8() {
        QProcess bridge;
        HideFixture(bridge);
        bridge.setProgram(QString::fromUtf8(OKUFLOW_MCP_BRIDGE_FIXTURE));
        bridge.setArguments({"--instance", QString::fromStdString(RandomHex())});
        bridge.start();
        QVERIFY(bridge.waitForStarted());
        const std::string init = Initialize;
        bridge.write(init.data(), 17);
        bridge.waitForBytesWritten();
        Write(bridge, std::string_view(init).substr(17));
        QVERIFY(Read(bridge).Find("result"));
        Write(bridge, Initialized);
        Write(bridge, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
        QVERIFY(Read(bridge).Find("result")->Find("tools"));
        const std::string ping = "{\"jsonrpc\":\"2.0\",\"id\":\"\xc3\xa7\",\"method\":\"ping\"}";
        auto split = ping.find(char(0xc3)) + 1;
        bridge.write(ping.data(), static_cast<qint64>(split));
        bridge.waitForBytesWritten();
        Write(bridge, std::string_view(ping).substr(split));
        QCOMPARE(Read(bridge).Find("id")->String(), std::string("\xc3\xa7"));
        Write(bridge, R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"app.state.get"}})");
        QVERIFY(Read(bridge).Find("result")->Find("isError")->Boolean());
        bridge.closeWriteChannel();
        QVERIFY(bridge.waitForFinished(8000));
        QCOMPARE(bridge.exitCode(), 0);
        QVERIFY(bridge.readAllStandardError().isEmpty());
    }
    void slowStdoutConsumer() {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE readRaw{}, writeRaw{};
        QVERIFY(CreatePipe(&readRaw, &writeRaw, &security, 512));
        Handle outputRead(readRaw), outputWrite(writeRaw);
        QVERIFY(SetHandleInformation(outputRead.value, HANDLE_FLAG_INHERIT, 0));
        QVERIFY(CreatePipe(&readRaw, &writeRaw, &security, 512));
        Handle inputRead(readRaw), inputWrite(writeRaw);
        QVERIFY(SetHandleInformation(inputWrite.value, HANDLE_FLAG_INHERIT, 0));
        SIZE_T attributeBytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<BYTE> attributes(attributeBytes);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = inputRead.value;
        startup.StartupInfo.hStdOutput = outputWrite.value;
        startup.StartupInfo.hStdError = outputWrite.value;
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        QVERIFY(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes));
        HANDLE inherited[]{inputRead.value, outputWrite.value};
        const bool updated =
            UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
        if (!updated) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            QFAIL("private handle-list fixture failed");
        }
        const std::filesystem::path image(OKUFLOW_MCP_BRIDGE_FIXTURE);
        const auto selector = RandomHex();
        std::wstring command =
            L"\"" + image.wstring() + L"\" --instance " + std::wstring(selector.begin(), selector.end());
        PROCESS_INFORMATION process{};
        const bool created = CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, TRUE,
                                            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                                            &startup.StartupInfo, &process) != FALSE;
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        QVERIFY(created);
        Handle child(process.hProcess), thread(process.hThread);
        inputRead = {};
        outputWrite = {};
        // This exact child is a private bridge fixture. Closing its input is the
        // ordinary teardown even on assertion failure; no user app is targeted.
        std::string traffic = std::string(Initialize) + "\n" + Initialized +
                              "\n{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n";
        DWORD written{};
        QVERIFY(WriteFile(inputWrite.value, traffic.data(), static_cast<DWORD>(traffic.size()), &written,
                          nullptr));
        QCOMPARE(written, static_cast<DWORD>(traffic.size()));
        const auto exited = WaitForSingleObject(child.value, 5000);
        inputWrite = {};
        if (exited != WAIT_OBJECT_0) {
            WaitForSingleObject(child.value,
                                4000); // EOF normal cleanup for the owned fixture
            QFAIL("slow consumer did not close within output deadline");
        }
        DWORD code{};
        QVERIFY(GetExitCodeProcess(child.value, &code));
        QCOMPARE(code, DWORD(1));
    }
    void shutdownWithHeldFrameFence() {
        QProcess app;
        HideFixture(app);
        app.setProgram(QString::fromUtf8(OKUFLOW_MCP_APP_FIXTURE));
        app.start();
        QVERIFY(app.waitForStarted());
        auto ready = WaitType(app, "ready");
        QVERIFY(ready.Find("selector"));
        QProcess bridge;
        HideFixture(bridge);
        bridge.setProgram(QString::fromUtf8(OKUFLOW_MCP_BRIDGE_FIXTURE));
        bridge.setArguments({"--instance", QString::fromStdString(ready.Find("selector")->String())});
        bridge.start();
        QVERIFY(bridge.waitForStarted());
        Write(bridge, Initialize);
        QVERIFY(Read(bridge).Find("result"));
        Write(bridge, Initialized);
        auto pending = WaitType(app, "pending");
        QVERIFY(pending.Find("peerId"));
        Write(app, "approve-frames:" + pending.Find("peerId")->String());
        QVERIFY(WaitType(app, "approved").Find("ok")->Boolean());
        Write(app, "hold-fence");
        QVERIFY(WaitType(app, "fence-armed").IsObject());
        Write(
            bridge,
            R"({"jsonrpc":"2.0","id":42,"method":"tools/call","params":{"name":"view.observe","arguments":{"maxLongSide":384}}})");
        QVERIFY(WaitType(app, "fence-posted").IsObject());
        Write(app, "off");
        auto off = WaitType(app, "off");
        QVERIFY(off.Find("elapsedMs"));
        qInfo("Held image-fence shutdown: %.0f ms", off.Find("elapsedMs")->Number());
        QVERIFY2(off.Find("elapsedMs")->Number() < 250,
                 "GUI shutdown waited for its queued image authorization");
        auto reply = Read(bridge);
        QVERIFY(reply.Find("result"));
        QVERIFY(reply.Find("result")->Find("isError")->Boolean());
        QCOMPARE(reply.Find("result")->Find("content")->Elements().size(), size_t(1));
        bridge.closeWriteChannel();
        QVERIFY(bridge.waitForFinished(8000));
        Write(app, "exit");
        app.closeWriteChannel();
        QVERIFY(app.waitForFinished(8000));
        QCOMPARE(app.exitCode(), 0);
    }
    void catalogMismatch() {
        QProcess app;
        HideFixture(app);
        app.setProgram(QString::fromUtf8(OKUFLOW_MCP_APP_FIXTURE));
        app.setArguments({"--catalog-mismatch"});
        app.start();
        QVERIFY(app.waitForStarted());
        auto ready = WaitType(app, "ready");
        QVERIFY(ready.Find("selector"));
        QProcess bridge;
        HideFixture(bridge);
        bridge.setProgram(QString::fromUtf8(OKUFLOW_MCP_BRIDGE_FIXTURE));
        bridge.setArguments({"--instance", QString::fromStdString(ready.Find("selector")->String())});
        bridge.start();
        QVERIFY(bridge.waitForStarted());
        Write(bridge, Initialize);
        QVERIFY(Read(bridge).Find("result"));
        Write(bridge, Initialized);
        auto pending = WaitType(app, "pending");
        QVERIFY(pending.Find("peerId"));
        Write(app, "approve:" + pending.Find("peerId")->String());
        QVERIFY(WaitType(app, "approved").Find("ok")->Boolean());
        Write(bridge, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
        QCOMPARE(Read(bridge).Find("result")->Find("tools")->Elements().size(),
                 okuflow::agent::ControlRegistry(okuflow::agent::BuiltControlCatalogEdition()).size());
        Write(bridge, R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"app.state.get"}})");
        auto reply = Read(bridge);
        QVERIFY(reply.Find("result")->Find("isError")->Boolean());
        const auto error =
            Json::Parse(reply.Find("result")->Find("content")->Elements()[0].Find("text")->String());
        QCOMPARE(error.Find("error")->Find("reason")->String(), std::string("catalog_mismatch"));
        bridge.closeWriteChannel();
        QVERIFY(bridge.waitForFinished(8000));
        Write(app, "exit");
        app.closeWriteChannel();
        QVERIFY(app.waitForFinished(8000));
        QCOMPARE(app.exitCode(), 0);
    }
    void privatePipeIdentity() {
        InstanceDescriptor d{RandomHex(), RandomHex()};
        auto server = CreateSecurePipe(d);
        QVERIFY(server);
        std::atomic_bool stop{};
        bool accepted = false;
        std::shared_ptr<PeerIdentity> identity;
        std::thread peer([&] {
            accepted = AcceptPipe(server.value, stop);
            if (accepted)
                identity = PeerIdentity::Verify(server.value, true, CurrentImage());
        });
        auto client = ConnectPipe(d);
        QVERIFY(client);
        peer.join();
        QVERIFY(accepted);
        QVERIFY(identity);
        QVERIFY(identity->Alive());
        QVERIFY(PeerIdentity::Verify(client.value, false, CurrentImage()));
        QVERIFY(!PeerIdentity::Verify(client.value, false, CurrentImage().parent_path() / L"foreign.exe"));
        auto duplicate = CreateSecurePipe(d);
        QVERIFY(!duplicate);
        std::string frame = "private fixture";
        std::thread writer([&] { WriteFrame(client.value, frame, stop); });
        std::string received;
        QVERIFY(ReadFrame(server.value, received, stop));
        writer.join();
        QCOMPARE(received, frame);
    }
    void processBoundary() {
        QProcess app;
        HideFixture(app);
        app.setProgram(QString::fromUtf8(OKUFLOW_MCP_APP_FIXTURE));
        app.start();
        QVERIFY(app.waitForStarted());
        auto ready = WaitType(app, "ready");
        QVERIFY(ready.Find("selector"));
        auto selector = ready.Find("selector")->String();
        { // A wrong-image peer is rejected without losing the owned listener.
            auto instance = InstanceDescriptor::Read(selector);
            auto foreign = ConnectPipe(instance);
            QVERIFY(foreign);
            std::atomic_bool stop{};
            std::string bytes;
            QVERIFY(!ReadFrame(foreign.value, bytes, stop, 1000));
        }
        QProcess bridge;
        HideFixture(bridge);
        bridge.setProgram(QString::fromUtf8(OKUFLOW_MCP_BRIDGE_FIXTURE));
        bridge.setArguments({"--instance", QString::fromStdString(selector)});
        bridge.start();
        QVERIFY(bridge.waitForStarted());
        Write(bridge, Initialize);
        auto initialized = Read(bridge);
        QVERIFY(initialized.Find("result"));
        Write(bridge, Initialized);
        auto pending = WaitType(app, "pending");
        QVERIFY(pending.Find("peerId"));
        auto peerId = pending.Find("peerId")->String();
        Write(bridge, R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"app.state.get"}})");
        auto denied = Read(bridge);
        QVERIFY(denied.Find("result"));
        QVERIFY(denied.Find("result")->Find("isError")->Boolean());
        Write(app, "approve:" + peerId + "0");
        QVERIFY(!WaitType(app, "approved").Find("ok")->Boolean());
        Write(app, "approve:" + peerId);
        auto approval = WaitType(app, "approved");
        QVERIFY(approval.Find("ok"));
        QVERIFY(approval.Find("ok")->Boolean());
        Write(bridge, R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"app.state.get"}})");
        auto state = Read(bridge);
        QVERIFY(state.Find("result"));
        QVERIFY(state.Find("result")->Find("structuredContent"));
        Write(app, "revoke");
        QVERIFY(WaitType(app, "revoked").IsObject());
        Write(bridge, R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"app.state.get"}})");
        auto revoked = Read(bridge);
        QVERIFY(revoked.Find("result")->Find("isError")->Boolean());
        bridge.closeWriteChannel();
        QVERIFY(bridge.waitForFinished(8000));
        QCOMPARE(bridge.exitCode(), 0);
        QVERIFY(bridge.readAllStandardOutput().isEmpty());
        QVERIFY(bridge.readAllStandardError().isEmpty());
        QProcess nextBridge;
        HideFixture(nextBridge);
        nextBridge.setProgram(QString::fromUtf8(OKUFLOW_MCP_BRIDGE_FIXTURE));
        nextBridge.setArguments({"--instance", QString::fromStdString(selector)});
        nextBridge.start();
        QVERIFY(nextBridge.waitForStarted());
        Write(nextBridge, Initialize);
        QVERIFY(Read(nextBridge).Find("result"));
        Write(nextBridge, Initialized);
        auto nextPending = WaitType(app, "pending");
        QVERIFY(nextPending.Find("peerId"));
        const auto nextPeerId = nextPending.Find("peerId")->String();
        QVERIFY(nextPeerId != peerId);
        Write(app, "approve:" + peerId);
        QVERIFY(!WaitType(app, "approved").Find("ok")->Boolean());
        Write(nextBridge,
              R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"app.state.get"}})");
        QVERIFY(Read(nextBridge).Find("result")->Find("isError")->Boolean());
        Write(app, "approve:" + nextPeerId);
        QVERIFY(WaitType(app, "approved").Find("ok")->Boolean());
        Write(nextBridge,
              R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"app.state.get"}})");
        QVERIFY(Read(nextBridge).Find("result")->Find("structuredContent"));
        nextBridge.closeWriteChannel();
        QVERIFY(nextBridge.waitForFinished(8000));
        QCOMPARE(nextBridge.exitCode(), 0);
        Write(app, "exit");
        app.closeWriteChannel();
        QVERIFY(app.waitForFinished(8000));
        QCOMPARE(app.exitCode(), 0);
        QVERIFY(InstanceDescriptor::Read(selector).selector.empty());
    }
};
QTEST_GUILESS_MAIN(McpTests)
#include "mcp_tests.moc"
