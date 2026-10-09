// Protocol tests for CodexRealtimeTranscriptionClient against the fake
// app-server fixture (fake_codex_app_server.cpp). No network, no real Codex.

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QtTest>

#include "openzoom/common/codex_realtime_transcription_client.hpp"

using openzoom::CodexRealtimeTranscriptionClient;

namespace {

QString FakeServerPath()
{
    return QCoreApplication::applicationDirPath() +
           QStringLiteral("/fake_codex_app_server.exe");
}

} // namespace

class CodexRealtimeClientTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(QFileInfo::exists(FakeServerPath()),
                 qPrintable(QStringLiteral("missing fixture: %1").arg(FakeServerPath())));
        // The fake server refuses to run when these leak through; the client
        // must scrub them from the child environment without reading them.
        qputenv("OPENAI_API_KEY", "leak-canary");
        qputenv("CODEX_API_KEY", "leak-canary");
    }

    void cleanupTestCase()
    {
        qunsetenv("OPENAI_API_KEY");
        qunsetenv("CODEX_API_KEY");
    }

    void happyPathFiltersRolesAndAcceptsStopTail()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "happy");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        QSignalSpy answer(&client, &CodexRealtimeTranscriptionClient::AnswerSdp);
        QSignalSpy started(&client, &CodexRealtimeTranscriptionClient::RealtimeStarted);
        QSignalSpy deltas(&client, &CodexRealtimeTranscriptionClient::UserTranscriptDelta);
        QSignalSpy dones(&client, &CodexRealtimeTranscriptionClient::UserTranscriptDone);
        QSignalSpy closed(&client, &CodexRealtimeTranscriptionClient::RealtimeClosed);
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        QSignalSpy limits(&client, &CodexRealtimeTranscriptionClient::RateLimitsChanged);

        client.BeginSession(7);
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(ready.at(0).at(0).toULongLong(), quint64(7));
        QTRY_COMPARE(limits.count(), 1);
        QCOMPARE(limits.at(0).at(0).toInt(), 61);
        QCOMPARE(limits.at(0).at(1).toBool(), true);

        client.StartRealtime(7, QStringLiteral("v=0\r\nfake offer"));
        QTRY_COMPARE(started.count(), 1);
        QCOMPARE(answer.count(), 1);
        QVERIFY(answer.at(0).at(1).toString().startsWith(QStringLiteral("v=0")));
        QTRY_COMPARE(deltas.count(), 2);
        QCOMPARE(deltas.at(0).at(1).toString(), QStringLiteral("hello "));
        QCOMPARE(deltas.at(1).at(1).toString(), QStringLiteral("world"));
        QTRY_COMPARE(dones.count(), 1);
        QCOMPARE(dones.at(0).at(1).toString(), QStringLiteral("hello world"));
        QCOMPARE(dones.at(0).at(2).toBool(), false);

        client.StopRealtime(7);
        QTRY_COMPARE(closed.count(), 1);
        // The final that arrived during the bounded stop window is published.
        QCOMPARE(dones.count(), 2);
        QCOMPARE(dones.at(1).at(1).toString(), QStringLiteral("final during stop"));
        // Assistant transcripts and blank finals never surfaced.
        QCOMPARE(deltas.count(), 2);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!client.IsSessionActive());
    }

    void nonChatGptAccountFails()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "notchatgpt");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        client.BeginSession(1);
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("ChatGPT")));
        QCOMPARE(ready.count(), 0);
        QVERIFY(!client.IsSessionActive());
    }

    void missingRealtimeSupportFails()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "norealtime");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        client.BeginSession(2);
        QTRY_COMPARE(ready.count(), 1);
        client.StartRealtime(2, QStringLiteral("v=0\r\nfake offer"));
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("does not support")));
    }

    void asyncApiKeyAuthErrorFails()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "apikey");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        QSignalSpy started(&client, &CodexRealtimeTranscriptionClient::RealtimeStarted);
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        client.BeginSession(3);
        QTRY_COMPARE(ready.count(), 1);
        client.StartRealtime(3, QStringLiteral("v=0\r\nfake offer"));
        // The successful start reply must not count as started; the async
        // error decides.
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(started.count(), 0);
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("ChatGPT")));
    }

    void malformedProtocolLineIsIgnored()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "malformed");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        client.BeginSession(4);
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
    }

    void oversizedProtocolLineFailsSession()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "oversized");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        client.BeginSession(5);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 0);
        QVERIFY(!client.IsSessionActive());
    }

    void oversizedOfferIsRejectedLocally()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "happy");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        client.BeginSession(6);
        QTRY_COMPARE(ready.count(), 1);
        client.StartRealtime(6, QString(70000, QLatin1Char('a')));
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("offer")));
    }

    void staleGenerationCallsAreIgnored()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "happy");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        client.BeginSession(10);
        QTRY_COMPARE(ready.count(), 1);
        // Wrong-generation control calls must be no-ops.
        client.StartRealtime(9, QStringLiteral("v=0\r\nfake offer"));
        client.StopRealtime(9);
        client.EndSession(9);
        QVERIFY(client.IsSessionActive());
        client.EndSession(10);
        QVERIFY(!client.IsSessionActive());
        QCOMPARE(failed.count(), 0);
    }

    void toolDiscoveryFailureFailsClosed()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "tool_discovery_failure");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        client.BeginSession(11);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 0);
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("isolation")));
    }

    void unexpectedMcpStartupFailsClosed()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "unexpected_mcp");
        CodexRealtimeTranscriptionClient client;
        client.SetExecutablePath(FakeServerPath());
        QSignalSpy ready(&client, &CodexRealtimeTranscriptionClient::SessionReady);
        QSignalSpy failed(&client, &CodexRealtimeTranscriptionClient::SessionFailed);
        client.BeginSession(12);
        QTRY_COMPARE(ready.count(), 1);
        client.StartRealtime(12, QStringLiteral("v=0\r\nfake offer"));
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("isolation")));
    }

    void stderrPayloadIsNeverForwarded()
    {
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", "stderr_payload");
        openzoom::CodexJsonRpcProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("OPENAI_API_KEY"));
        environment.remove(QStringLiteral("CODEX_API_KEY"));
        process.SetProcessEnvironment(environment);
        process.SetProgram(
            FakeServerPath(),
            {QStringLiteral("app-server"),
             QStringLiteral("--disable"),
             QStringLiteral("apps"),
             QStringLiteral("--disable"),
             QStringLiteral("plugins"),
             QStringLiteral("-c"),
             QStringLiteral("mcp_servers.fake_tools.enabled=false")});
        QSignalSpy diagnostics(&process, &openzoom::CodexJsonRpcProcess::StderrText);
        process.Start();
        QTRY_COMPARE(diagnostics.count(), 1);
        const QString diagnostic = diagnostics.at(0).at(0).toString();
        QVERIFY(diagnostic.contains(QStringLiteral("suppressed")));
        QVERIFY(!diagnostic.contains(QStringLiteral("SENSITIVE")));
        process.Shutdown();
    }
};

QTEST_MAIN(CodexRealtimeClientTests)
#include "codex_realtime_client_tests.moc"
