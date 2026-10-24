#include "okuflow/common/codex_agent_profile.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QTest>

using namespace okuflow;

class CodexAgentProfileTests final : public QObject {
    Q_OBJECT
private:
    QJsonObject profile_;
    QString Features() const {
        QStringList rows;
        for (const auto& value : profile_.value("featureInventory").toArray()) {
            auto row = value.toString();
            row.replace(QLatin1Char('|'), QStringLiteral("  "));
            rows << row + QStringLiteral("  true");
        }
        return rows.join(QLatin1Char('\n'));
    }
private slots:
    void initTestCase() {
        QFile file(QStringLiteral(OKUFLOW_AGENT_PROFILE_FIXTURE));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonParseError error{};
        profile_ = QJsonDocument::fromJson(file.readAll(), &error).object();
        QCOMPARE(error.error, QJsonParseError::NoError);
        QCOMPARE(profile_.value("cliVersion").toString(), QStringLiteral("0.160.1"));
    }
    void knownProfileDisablesEveryProvider() {
        QJsonArray mcp{QJsonObject{{"name", "approved-server"}, {"private", "secret"}}};
        const auto result = CodexAgentProfile::Validate("codex-cli 0.160.1", Features(), mcp);
        QVERIFY(result.available);
        QCOMPARE(result.arguments.mid(0, 3), QStringList({"app-server", "--listen", "stdio://"}));
        for (const auto& name : CodexAgentProfile::DisabledFeatures()) {
            const auto index = result.arguments.indexOf(name);
            QVERIFY(index > 0);
            QCOMPARE(result.arguments[index - 1], QStringLiteral("--disable"));
        }
        QVERIFY(result.arguments.contains("mcp_servers={}"));
        QVERIFY(result.arguments.contains("mcp_servers.approved-server={enabled=false,command=\"codex.exe\"}"));
        QVERIFY(!result.arguments.join(' ').contains("secret"));
    }
    void codeModeOnlyModelsRetainRestrictedToolTransport() {
        const auto result = CodexAgentProfile::Validate("codex-cli 0.160.1", Features(), {});
        QVERIFY(result.available);
        QVERIFY(!CodexAgentProfile::DisabledFeatures().contains("code_mode_host"));
        const auto host = result.arguments.indexOf("code_mode_host");
        QVERIFY(host > 0);
        QCOMPARE(result.arguments[host - 1], QStringLiteral("--enable"));
        QCOMPARE(result.arguments.count("code_mode_host"), 1);
        for (const auto* feature : {"shell_tool", "computer_use", "browser_use", "multi_agent", "apps", "plugins"}) {
            const auto index = result.arguments.indexOf(QString::fromLatin1(feature));
            QVERIFY(index > 0);
            QCOMPARE(result.arguments[index - 1], QStringLiteral("--disable"));
        }
        QVERIFY(result.arguments.contains("sandbox_mode=\"read-only\""));
        QVERIFY(result.arguments.contains("approval_policy=\"never\""));
        QVERIFY(result.arguments.contains("mcp_servers={}"));
    }
    void everyConfiguredServerGetsAnExplicitChildDisable() {
        // Empty table overrides are merged by the real CLI. Retain the exact
        // offline inventory and disable each server in the child, not on disk.
        const QJsonArray malformed{QJsonObject{{"name", "code-review"},
            {"enabled", false}, {"transport", "invalid"}}};
        const auto result = CodexAgentProfile::Validate("codex-cli 0.160.1", Features(), malformed);
        QVERIFY(result.available);
        const QString disabled = "mcp_servers.code-review={enabled=false,command=\"codex.exe\"}";
        QVERIFY(result.arguments.contains(disabled));
        QVERIFY(result.arguments.indexOf("mcp_servers={}") <
                result.arguments.indexOf(disabled));
        QCOMPARE(result.arguments.count("mcp_servers={}"), 1);
        QCOMPARE(result.arguments.count(disabled),1);
    }
    void futureVersionFailsClosed() {
        const auto result = CodexAgentProfile::Validate("codex-cli 0.161.0", Features(), {});
        QVERIFY(!result.available);
        QCOMPARE(result.errorCode, QStringLiteral("unsupported_codex_version"));
        QVERIFY(result.arguments.isEmpty());
    }
    void changedFeatureInventoryFailsClosed() {
        auto features = Features();
        features.replace("shell_tool  stable", "shell_tool  experimental");
        QVERIFY(!CodexAgentProfile::Validate("codex-cli 0.160.1", features, {}).available);
        features = Features();
        features += "\nunknown_tool  stable  true";
        QVERIFY(CodexAgentProfile::FeatureInventoryDigest(features).isEmpty());
    }
    void duplicateAndMalformedRowsRejected() {
        QVERIFY(CodexAgentProfile::FeatureInventoryDigest(Features() + "\napps  stable  true").isEmpty());
        QVERIFY(CodexAgentProfile::FeatureInventoryDigest("unparseable diagnostic").isEmpty());
    }
    void userEnabledValuesCannotWidenProfile() {
        const auto enabled = CodexAgentProfile::Validate("codex-cli 0.160.1", Features(), {});
        auto disabledText = Features();
        disabledText.replace("  true", "  false");
        const auto disabled = CodexAgentProfile::Validate("codex-cli 0.160.1", disabledText, {});
        QVERIFY(enabled.available && disabled.available);
        QCOMPARE(enabled.arguments, disabled.arguments);
        QCOMPARE(CodexAgentProfile::FeatureInventoryDigest(Features()),
                 profile_.value("featureInventorySha256").toString());
    }
    void unsafeServerName_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("quoted") << QStringLiteral("server\"name");
        QTest::newRow("nested") << QStringLiteral("server.name");
        QTest::newRow("newline") << QStringLiteral("server\nname");
        QTest::newRow("shell") << QStringLiteral("server;command");
        QTest::newRow("empty") << QString();
    }
    void unsafeServerName() {
        QFETCH(QString, name);
        const auto result = CodexAgentProfile::Validate("codex-cli 0.160.1", Features(),
                                                       QJsonArray{QJsonObject{{"name", name}}});
        QVERIFY(!result.available);
        QVERIFY(result.arguments.isEmpty());
    }
    void malformedMcpInventoryRejected() {
        QVERIFY(!CodexAgentProfile::Validate("codex-cli 0.160.1", Features(), QJsonArray{"server"}).available);
        QVERIFY(!CodexAgentProfile::Validate("codex-cli 0.160.1", Features(), QJsonArray{QJsonObject{{"name", 3}}}).available);
        const QJsonObject server{{"name", "duplicate"}};
        QVERIFY(!CodexAgentProfile::Validate("codex-cli 0.160.1", Features(), QJsonArray{server, server}).available);
    }
    void experimentalInitializationExplicit() {
        const auto initialization = CodexAgentProfile::Initialization();
        QVERIFY(initialization.value("capabilities").toObject().value("experimentalApi").toBool());
        const auto facts = profile_.value("schemaFacts").toObject();
        QVERIFY(!facts.value("stableThreadStartDynamicTools").toBool());
        QVERIFY(facts.value("experimentalThreadStartDynamicTools").toBool());
        QVERIFY(facts.value("experimentalInitializationRequired").toBool());
        QCOMPARE(facts.value("callMethod").toString(), QStringLiteral("item/tool/call"));
    }
};

QTEST_GUILESS_MAIN(CodexAgentProfileTests)
#include "codex_agent_profile_tests.moc"
