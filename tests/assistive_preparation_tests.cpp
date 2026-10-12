// Deterministic preparation completions; no provider, camera, or credentials.
#include <QFile>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest>
#include "okuflow/common/assistive_runtime.hpp"
#include "okuflow/common/codex_app_server_client.hpp"
#include <QScopeGuard>

namespace {

// Minimal loopback OpenAI-compatible endpoint: records each request body and
// answers "answer N". Never leaves the machine.
class FakeChatEndpoint : public QObject {
public:
    FakeChatEndpoint()
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                    if (socket->property("handled").toBool()) return;
                    buffer->append(socket->readAll());
                    const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    qsizetype length = 0;
                    for (const QByteArray& line : buffer->left(headerEnd).split('\n')) {
                        if (line.trimmed().toLower().startsWith("content-length:")) {
                            length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                        }
                    }
                    if (buffer->size() < headerEnd + 4 + length) return;
                    socket->setProperty("handled", true);
                    requests.append(QJsonDocument::fromJson(buffer->mid(headerEnd + 4, length)).object());
                    const QByteArray body = QJsonDocument(QJsonObject{
                        {QStringLiteral("choices"), QJsonArray{QJsonObject{
                            {QStringLiteral("message"), QJsonObject{
                                {QStringLiteral("content"),
                                 answerOverride.isEmpty()
                                     ? QStringLiteral("answer %1").arg(requests.size())
                                     : answerOverride}}}}}}})
                        .toJson(QJsonDocument::Compact);
                    const QByteArray response =
                        (failNextResponse ? QByteArray("HTTP/1.1 500 Fixture failure\r\n")
                                          : QByteArray("HTTP/1.1 200 OK\r\n")) +
                        "Content-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body;
                    failNextResponse = false;
                    if (holdNextResponse) {
                        holdNextResponse = false;
                        heldSocket_ = socket;
                        heldResponse_ = response;
                        return;
                    }
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        server_.listen(QHostAddress::LocalHost);
    }
    void ReleaseResponse()
    {
        if (heldSocket_ && heldSocket_->state() == QAbstractSocket::ConnectedState) {
            heldSocket_->write(heldResponse_);
            heldSocket_->disconnectFromHost();
        }
        heldSocket_.clear();
        heldResponse_.clear();
    }
    QString Url() const
    {
        return QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(server_.serverPort());
    }
    // Assistant-role messages: the earlier Explain answers that were resent.
    static QStringList History(const QJsonObject& request)
    {
        QStringList answers;
        for (const QJsonValue& value : request.value(QStringLiteral("messages")).toArray()) {
            const QJsonObject message = value.toObject();
            if (message.value(QStringLiteral("role")).toString() == QStringLiteral("assistant")) {
                answers.append(message.value(QStringLiteral("content")).toString());
            }
        }
        return answers;
    }
    QList<QJsonObject> requests;
    bool holdNextResponse{false};
    bool failNextResponse{false};
    QString answerOverride;

private:
    QTcpServer server_;
    QPointer<QTcpSocket> heldSocket_;
    QByteArray heldResponse_;
};

} // namespace

class AssistivePreparationTests : public QObject {
    Q_OBJECT
private:
    // Isolates TEMP/TMP (temporary Codex frames) and the fake scenario.
    struct CodexFixture {
        QTemporaryDir directory;
        QByteArray oldTemp{qgetenv("TEMP")};
        QByteArray oldTmp{qgetenv("TMP")};
        QByteArray oldScenario{qgetenv("OKUFLOW_FAKE_CODEX_SCENARIO")};
        explicit CodexFixture(const QByteArray& scenario)
        {
            qputenv("TEMP", directory.path().toLocal8Bit());
            qputenv("TMP", directory.path().toLocal8Bit());
            qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", scenario);
        }
        ~CodexFixture()
        {
            if (oldTemp.isNull()) qunsetenv("TEMP"); else qputenv("TEMP", oldTemp);
            if (oldTmp.isNull()) qunsetenv("TMP"); else qputenv("TMP", oldTmp);
            if (oldScenario.isNull()) qunsetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
            else qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", oldScenario);
        }
        okuflow::AssistiveRuntimeConfig Config() const
        {
            okuflow::AssistiveRuntimeConfig config;
            config.codexExecutablePath = QCoreApplication::applicationDirPath() +
                                         QStringLiteral("/fake_codex_app_server.exe");
            config.notesDirectory = directory.path();
            // Advanced opt-ins never apply to Read or Explain threads.
            config.codexInternetEnabled = true;
            config.codexCodingEnabled = true;
            config.codexWorkspaceDirectory = directory.path();
            return config;
        }
    };

    // Submits one forced Read/Explain and returns its finished turn.
    static QList<QVariant> RunCameraTurn(okuflow::AssistiveRuntime& runtime, bool readText)
    {
        QSignalSpy finished(&runtime, &okuflow::AssistiveRuntime::AssistantTurnFinished);
        QImage frame(128, 96, QImage::Format_ARGB32);
        frame.fill(Qt::white);
        runtime.SubmitFrameForced(frame.constBits(), frame.width(), frame.height(), readText);
        if (!QTest::qWaitFor([&]() { return finished.size() == 1 && !runtime.IsBusy(); }, 10000)) {
            return {};
        }
        return finished.first();
    }

    static bool RunEndpointRequest(okuflow::AssistiveRuntime& runtime,
                                   FakeChatEndpoint& endpoint,
                                   bool readText)
    {
        const qsizetype before = endpoint.requests.size();
        QImage frame(128, 96, QImage::Format_ARGB32);
        frame.fill(Qt::white);
        runtime.SubmitFrameForced(frame.constBits(), frame.width(), frame.height(), readText);
        return QTest::qWaitFor([&]() {
            return endpoint.requests.size() == before + 1 && !runtime.IsBusy();
        }, 10000);
    }

    static void Begin(okuflow::AssistiveRuntime& runtime, const QString& prompt)
    {
        ++runtime.vlmPreparationGeneration_;
        runtime.vlmPreparationPending_ = true;
        runtime.vlmPreparationPersistent_ = true;
        runtime.vlmPreparationThreadId_ = QStringLiteral("fixture-thread");
        runtime.pendingAssistantNotePrompt_ = prompt;
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::white);
        runtime.pendingAssistantNoteImagePath_ = runtime.SaveAnalyzedImageForNotes(image);
    }
private slots:
    void staleHistory_data()
    {
        QTest::addColumn<bool>("newTurn");
        QTest::addColumn<bool>("failHistory");
        QTest::newRow("A-after-B") << false << false;
        QTest::newRow("A-during-new-answer") << true << false;
        QTest::newRow("failed-A-after-B") << false << true;
        QTest::newRow("failed-A-during-new-answer") << true << true;
    }
    void staleHistory()
    {
        QFETCH(bool, newTurn);
        QFETCH(bool, failHistory);
        const QByteArray previous = qgetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
        const auto restore = qScopeGuard([previous]() {
            if (previous.isNull()) qunsetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
            else qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", previous);
        });
        qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", failHistory
                    ? "vision-lifecycle-history-error" : "vision-lifecycle-history");
        okuflow::CodexAppServerClient client;
        client.Configure(QCoreApplication::applicationDirPath() + QStringLiteral("/fake_codex_app_server.exe"),
                         {}, {}, {}, false, false, {});
        QSignalSpy account(&client, &okuflow::CodexAppServerClient::AccountChanged);
        QSignalSpy status(&client, &okuflow::CodexAppServerClient::ServerStateChanged);
        QSignalSpy loaded(&client, &okuflow::CodexAppServerClient::ConversationTranscriptLoaded);
        QSignalSpy failed(&client, &okuflow::CodexAppServerClient::ConversationTranscriptLoadFailed);
        QSignalSpy finished(&client, &okuflow::CodexAppServerClient::TurnFinished);
        client.Start();
        QTRY_VERIFY(!account.isEmpty());
        QVERIFY(account.last().at(0).toBool());
        client.LoadConversation(QStringLiteral("A"));
        auto hasStatus = [&](const QString& text) {
            for (const auto& row : status) if (row.at(1).toString() == text) return true;
            return false;
        };
        QTRY_VERIFY(hasStatus(QStringLiteral("held history")));
        if (newTurn) client.RequestVisionTurn(QStringLiteral("new answer"), {}, QStringLiteral("A"), true);
        else client.LoadConversation(QStringLiteral("B"));
        QTRY_VERIFY(hasStatus(QStringLiteral("released history")));
        QCOMPARE(failed.count(), 0);
        if (newTurn) {
            QTRY_COMPARE(finished.count(), 1);
            QCOMPARE(finished.first().at(2).toString(), QStringLiteral("new answer"));
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("A"));
            QCOMPARE(loaded.count(), 0);
        } else {
            QCOMPARE(loaded.count(), 1);
            QCOMPARE(loaded.first().at(0).toString(), QStringLiteral("B"));
            QCOMPARE(loaded.first().at(1).toJsonArray().first().toObject().value(QStringLiteral("text")).toString(),
                     QStringLiteral("B history"));
        }
    }

    void historyFailure_data()
    {
        QTest::addColumn<QString>("threadId");
        QTest::addColumn<QString>("expectedError");
        QTest::newRow("provider-error") << QStringLiteral("error")
                                         << QStringLiteral("fake history read failure");
        QTest::newRow("empty-error") << QStringLiteral("empty-error")
                                      << QStringLiteral("Could not load conversation.");
    }

    void historyFailure()
    {
        QFETCH(QString, threadId);
        QFETCH(QString, expectedError);
        const QByteArray previous = qgetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
        const auto restore = qScopeGuard([previous]() {
            if (previous.isNull()) qunsetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
            else qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", previous);
        });
        qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", "vision-lifecycle-history");
        okuflow::CodexAppServerClient client;
        client.Configure(QCoreApplication::applicationDirPath() + QStringLiteral("/fake_codex_app_server.exe"),
                         {}, {}, {}, false, false, {});
        QSignalSpy account(&client, &okuflow::CodexAppServerClient::AccountChanged);
        QSignalSpy loaded(&client, &okuflow::CodexAppServerClient::ConversationTranscriptLoaded);
        QSignalSpy failed(&client, &okuflow::CodexAppServerClient::ConversationTranscriptLoadFailed);
        client.Start();
        QTRY_VERIFY(!account.isEmpty());
        client.LoadConversation(threadId);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(), threadId);
        QCOMPARE(failed.first().at(1).toString(), expectedError);
        QCOMPARE(loaded.count(), 0);
        QVERIFY(client.IsReady());
        QVERIFY(!client.IsTurnActive());
    }

    void historyLoadBeforeReadyReportsFailure()
    {
        okuflow::CodexAppServerClient client;
        QSignalSpy loaded(&client, &okuflow::CodexAppServerClient::ConversationTranscriptLoaded);
        QSignalSpy failed(&client, &okuflow::CodexAppServerClient::ConversationTranscriptLoadFailed);
        client.LoadConversation(QStringLiteral("A"));
        QCOMPARE(loaded.count(), 0);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(0).toString(), QStringLiteral("A"));
        QCOMPARE(failed.first().at(1).toString(), QStringLiteral("Could not load conversation."));
    }

    void runtimeHistoryFailure_data()
    {
        QTest::addColumn<bool>("newPrompt");
        QTest::newRow("forward-current-error") << false;
        QTest::newRow("cancel-pending-A-before-prompt") << true;
    }

    void runtimeHistoryFailure()
    {
        QFETCH(bool, newPrompt);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto oldTemp = qgetenv("TEMP");
        const auto oldTmp = qgetenv("TMP");
        const auto previousScenario = qgetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
        const auto restore = qScopeGuard([oldTemp, oldTmp, previousScenario]() {
            if (oldTemp.isNull()) qunsetenv("TEMP"); else qputenv("TEMP", oldTemp);
            if (oldTmp.isNull()) qunsetenv("TMP"); else qputenv("TMP", oldTmp);
            if (previousScenario.isNull()) qunsetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
            else qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", previousScenario);
        });
        qputenv("TEMP", directory.path().toLocal8Bit());
        qputenv("TMP", directory.path().toLocal8Bit());
        qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", "vision-lifecycle-history-error");
        okuflow::AssistiveRuntime runtime;
        runtime.config_.lectureNotesEnabled = false;
        runtime.codexClient_->Configure(
            QCoreApplication::applicationDirPath() + QStringLiteral("/fake_codex_app_server.exe"),
            {}, {}, {}, false, false, {});
        QSignalSpy account(&runtime, &okuflow::AssistiveRuntime::CodexAccountChanged);
        QSignalSpy status(&runtime, &okuflow::AssistiveRuntime::CodexServerStateChanged);
        QSignalSpy loaded(&runtime, &okuflow::AssistiveRuntime::AssistantTranscriptLoaded);
        QSignalSpy failed(&runtime, &okuflow::AssistiveRuntime::AssistantTranscriptLoadFailed);
        QSignalSpy finished(&runtime, &okuflow::AssistiveRuntime::AssistantTurnFinished);
        runtime.codexClient_->Start();
        QTRY_VERIFY(!account.isEmpty());
        if (!newPrompt) {
            runtime.LoadAssistantConversation(QStringLiteral("error"));
            QTRY_COMPARE(failed.count(), 1);
            QCOMPARE(failed.first().at(0).toString(), QStringLiteral("error"));
            QCOMPARE(failed.first().at(1).toString(), QStringLiteral("fake history read failure"));
            QVERIFY(!runtime.IsCodexTurnActive());
            QCOMPARE(finished.count(), 0);
        } else {
            runtime.LoadAssistantConversation(QStringLiteral("A"));
            auto hasStatus = [&](const QString& text) {
                for (const auto& row : status) if (row.at(1).toString() == text) return true;
                return false;
            };
            QTRY_VERIFY(hasStatus(QStringLiteral("held history")));
            runtime.SubmitAssistantPrompt(QStringLiteral("new answer"), QStringLiteral("A"),
                                          nullptr, 0, 0, false);
            QTRY_VERIFY(hasStatus(QStringLiteral("released history")));
            QTRY_COMPARE(finished.count(), 1);
            QCOMPARE(failed.count(), 0);
            QCOMPARE(finished.first().at(0).toString(), QStringLiteral("A"));
            QCOMPARE(finished.first().at(2).toString(), QStringLiteral("new answer"));
            QVERIFY(!runtime.IsCodexTurnActive());
        }
        QCOMPARE(loaded.count(), 0);
    }
    void cancellation_data()
    {
        QTest::addColumn<bool>("modeOff");
        QTest::newRow("mode-off") << true;
        QTest::newRow("explicit-stop") << false;
    }
    void cancellation()
    {
        QFETCH(bool, modeOff);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto oldTemp = qgetenv("TEMP");
        const auto oldTmp = qgetenv("TMP");
        const auto restoreTemp = qScopeGuard([oldTemp, oldTmp] {
            if (oldTemp.isNull()) qunsetenv("TEMP"); else qputenv("TEMP", oldTemp);
            if (oldTmp.isNull()) qunsetenv("TMP"); else qputenv("TMP", oldTmp);
        });
        qputenv("TEMP", directory.path().toLocal8Bit());
        qputenv("TMP", directory.path().toLocal8Bit());
        okuflow::AssistiveRuntime runtime;
        runtime.config_.notesDirectory = directory.path();
        runtime.vlmEnabled_ = true;
        Begin(runtime, QStringLiteral("question A"));
        const auto generationA = runtime.vlmPreparationGeneration_;
        const auto imageA = runtime.pendingAssistantNoteImagePath_;
        QSignalSpy finished(&runtime, &okuflow::AssistiveRuntime::AssistantTurnFinished);
        if (modeOff) runtime.SetModes(false);
        else runtime.StopAssistant();
        QCOMPARE(finished.count(), 1);
        QVERIFY(finished.at(0).at(4).toBool());
        QVERIFY(finished.at(0).at(5).toBool());
        QVERIFY(!runtime.IsCodexTurnActive());
        QVERIFY(!runtime.noteImages_.contains(imageA));
        runtime.SetModes(false);
        QCOMPARE(finished.count(), 1);
        // B has acquired the same notes slot before A's queued completion.
        Begin(runtime, QStringLiteral("question B"));
        const auto imageB = runtime.pendingAssistantNoteImagePath_;
        const QString temporaryA = directory.filePath(QStringLiteral("late-A.jpg"));
        QFile file(temporaryA);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("fixture");
        file.close();
        runtime.CompleteAssistantPreparation(generationA, true, temporaryA,
                                              QStringLiteral("question A"), QStringLiteral("fixture-thread"));
        QVERIFY(!QFile::exists(temporaryA));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(runtime.pendingAssistantNotePrompt_, QStringLiteral("question B"));
        QCOMPARE(runtime.pendingAssistantNoteImagePath_, imageB);
        QVERIFY(runtime.noteImages_.contains(imageB));
        runtime.vlmPreparationPending_ = false;
        runtime.vlmPreparationPersistent_ = false;
        runtime.vlmPreparationThreadId_.clear();
        runtime.FinishAssistantTurnSuccess(QStringLiteral("fixture-thread"), QStringLiteral("answer B"));
        QTRY_VERIFY(!runtime.HasPendingNotesWrites());
        QFile notes(runtime.notesFilePath());
        QVERIFY(notes.open(QIODevice::ReadOnly));
        const auto html = notes.readAll();
        QVERIFY(html.contains("question B"));
        QVERIFY(html.contains("answer B"));
        QVERIFY(!html.contains("question A"));
        QVERIFY(QFile::exists(imageB));
    }

    void explainContinuesOneThreadAndReadAlwaysStartsNew()
    {
        CodexFixture fixture("explain-threads");
        QVERIFY(fixture.directory.isValid());
        okuflow::AssistiveRuntime runtime;
        runtime.SetConfig(fixture.Config());
        QSignalSpy created(&runtime, &okuflow::AssistiveRuntime::ExplainConversationCreated);
        QSignalSpy changed(&runtime, &okuflow::AssistiveRuntime::ExplainConversationChanged);
        QSignalSpy privacy(&runtime, &okuflow::AssistiveRuntime::PrivacyNotice);

        const QList<QVariant> explainA = RunCameraTurn(runtime, false);
        QVERIFY(!explainA.isEmpty());
        QCOMPARE(explainA.at(3).toString(), QString());
        QVERIFY(!explainA.at(5).toBool()); // Not an Assistant chat turn.
        const QString explainThread = explainA.at(0).toString();
        QVERIFY(explainThread.startsWith(QStringLiteral("keep_")));
        QCOMPARE(explainA.at(2).toString(), explainThread + QStringLiteral(" explain first"));
        QCOMPARE(runtime.ExplainConversationId(), explainThread);
        QCOMPARE(created.size(), 1);
        QCOMPARE(created.first().at(0).toJsonObject().value(QStringLiteral("id")).toString(), explainThread);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().at(0).toString(), explainThread);
        QVERIFY(privacy.last().at(0).toString().contains(QStringLiteral("saved Explain conversation")));

        const QList<QVariant> readA = RunCameraTurn(runtime, true);
        QVERIFY(!readA.isEmpty());
        QCOMPARE(readA.at(3).toString(), QString());
        const QString readThreadA = readA.at(0).toString();
        QVERIFY(readThreadA.startsWith(QStringLiteral("tmp_")));
        QCOMPARE(readA.at(2).toString(), readThreadA + QStringLiteral(" read first"));
        QVERIFY(privacy.last().at(0).toString().contains(QStringLiteral("temporary, new for each Read")));

        const QList<QVariant> explainB = RunCameraTurn(runtime, false);
        QVERIFY(!explainB.isEmpty());
        QCOMPARE(explainB.at(0).toString(), explainThread);
        QCOMPARE(explainB.at(2).toString(), explainThread + QStringLiteral(" explain continued"));

        const QList<QVariant> readB = RunCameraTurn(runtime, true);
        QVERIFY(!readB.isEmpty());
        const QString readThreadB = readB.at(0).toString();
        QVERIFY(readThreadB.startsWith(QStringLiteral("tmp_")));
        QVERIFY(readThreadB != readThreadA);
        QVERIFY(readThreadB != explainThread);
        QCOMPARE(readB.at(2).toString(), readThreadB + QStringLiteral(" read first"));

        QCOMPARE(created.size(), 1);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(runtime.ExplainConversationId(), explainThread);

        // Both Explains still reach the lecture notes as scene explanations.
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.HasPendingNotesWrites(), 5000);
        QFile notes(runtime.notesFilePath());
        QVERIFY(notes.open(QIODevice::ReadOnly));
        const QString html = QString::fromUtf8(notes.readAll());
        QVERIFY(html.contains(explainThread + QStringLiteral(" explain first")));
        QVERIFY(html.contains(explainThread + QStringLiteral(" explain continued")));
        QVERIFY(html.contains(QStringLiteral("Scene explanation")));
        QVERIFY(html.contains(readThreadB + QStringLiteral(" read first")));
    }

    void missingRememberedExplainThreadStartsANewOne()
    {
        CodexFixture fixture("explain-threads");
        QVERIFY(fixture.directory.isValid());
        okuflow::AssistiveRuntime runtime;
        runtime.SetConfig(fixture.Config());
        // Restored from settings, but unknown to this app-server.
        runtime.SetExplainConversationId(QStringLiteral("keep_from_previous_launch"));
        QSignalSpy created(&runtime, &okuflow::AssistiveRuntime::ExplainConversationCreated);
        QSignalSpy changed(&runtime, &okuflow::AssistiveRuntime::ExplainConversationChanged);

        const QList<QVariant> explain = RunCameraTurn(runtime, false);
        QVERIFY(!explain.isEmpty());
        QCOMPARE(explain.at(3).toString(), QString()); // Fallback is transparent.
        const QString replacement = explain.at(0).toString();
        QVERIFY(replacement.startsWith(QStringLiteral("keep_")));
        QVERIFY(replacement != QStringLiteral("keep_from_previous_launch"));
        QCOMPARE(created.size(), 1);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first().at(0).toString(), replacement);
        QCOMPARE(runtime.ExplainConversationId(), replacement);

        // The replacement is remembered and continued by the next Explain.
        const QList<QVariant> next = RunCameraTurn(runtime, false);
        QVERIFY(!next.isEmpty());
        QCOMPARE(next.at(0).toString(), replacement);
        QCOMPARE(next.at(2).toString(), replacement + QStringLiteral(" explain continued"));
        QCOMPARE(created.size(), 1);
    }

    void readDiscardsASuppliedConversationId()
    {
        CodexFixture fixture("explain-threads");
        QVERIFY(fixture.directory.isValid());
        const QString imagePath = fixture.directory.filePath(QStringLiteral("read.jpg"));
        QImage frame(128, 96, QImage::Format_ARGB32);
        frame.fill(Qt::white);
        QVERIFY(frame.save(imagePath, "JPG"));
        const auto config = fixture.Config();
        okuflow::CodexAppServerClient client;
        client.Configure(config.codexExecutablePath, {}, {}, {}, true, true,
                         fixture.directory.path());
        QSignalSpy finished(&client, &okuflow::CodexAppServerClient::TurnFinished);
        QSignalSpy created(&client, &okuflow::CodexAppServerClient::RetainedThreadStarted);
        client.RequestVisionTurn(QStringLiteral("Transcribe all readable text"), imagePath,
                                 QStringLiteral("keep_existing"), false);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.first().at(3).toString(), QString());
        QVERIFY(finished.first().at(0).toString().startsWith(QStringLiteral("tmp_")));
        QCOMPARE(created.size(), 0);
        QVERIFY(!QFile::exists(imagePath));
    }

    void explainAndReadPreserveTheAssistantConversation()
    {
        CodexFixture fixture("explain-threads");
        QVERIFY(fixture.directory.isValid());
        okuflow::AssistiveRuntime runtime;
        auto config = fixture.Config();
        config.codexInternetEnabled = false;
        config.codexCodingEnabled = false;
        runtime.SetConfig(config);
        QSignalSpy chatCreated(&runtime, &okuflow::AssistiveRuntime::AssistantConversationCreated);
        QSignalSpy explainCreated(&runtime, &okuflow::AssistiveRuntime::ExplainConversationCreated);
        auto chat = [&](const QString& threadId) -> QList<QVariant> {
            QSignalSpy finished(&runtime, &okuflow::AssistiveRuntime::AssistantTurnFinished);
            QImage frame(128, 96, QImage::Format_ARGB32);
            frame.fill(Qt::white);
            runtime.SubmitAssistantPrompt(QStringLiteral("Assistant question"), threadId,
                                          frame.constBits(), frame.width(), frame.height(), true);
            if (!QTest::qWaitFor([&]() { return finished.size() == 1 && !runtime.IsBusy(); }, 5000)) {
                return {};
            }
            return finished.first();
        };
        const auto firstChat = chat({});
        QVERIFY(!firstChat.isEmpty());
        QCOMPARE(firstChat.at(3).toString(), QString());
        QVERIFY(firstChat.at(5).toBool());
        const QString chatThread = firstChat.at(0).toString();
        QVERIFY(!chatThread.isEmpty());
        QCOMPARE(chatCreated.size(), 1);
        QCOMPARE(runtime.ExplainConversationId(), QString());
        const auto firstExplain = RunCameraTurn(runtime, false);
        QVERIFY(!firstExplain.isEmpty());
        const QString explainThread = firstExplain.at(0).toString();
        QVERIFY(explainThread != chatThread);
        QCOMPARE(explainCreated.size(), 1);
        QVERIFY(!RunCameraTurn(runtime, true).isEmpty());
        const auto secondChat = chat(chatThread);
        QVERIFY(!secondChat.isEmpty());
        QCOMPARE(secondChat.at(3).toString(), QString());
        QCOMPARE(secondChat.at(0).toString(), chatThread);
        QVERIFY(secondChat.at(5).toBool());
        QCOMPARE(chatCreated.size(), 1);
        QCOMPARE(runtime.ExplainConversationId(), explainThread);
        const auto secondExplain = RunCameraTurn(runtime, false);
        QVERIFY(!secondExplain.isEmpty());
        QCOMPARE(secondExplain.at(0).toString(), explainThread);
        QCOMPARE(explainCreated.size(), 1);
        QCOMPARE(chatCreated.size(), 1);
    }

    void deletingOrResettingExplainConversationStartsFresh()
    {
        CodexFixture fixture("explain-threads");
        QVERIFY(fixture.directory.isValid());
        okuflow::AssistiveRuntime runtime;
        runtime.SetConfig(fixture.Config());
        QSignalSpy changed(&runtime, &okuflow::AssistiveRuntime::ExplainConversationChanged);
        QSignalSpy deleted(&runtime, &okuflow::AssistiveRuntime::AssistantConversationDeleted);

        const QString first = RunCameraTurn(runtime, false).value(0).toString();
        QVERIFY(first.startsWith(QStringLiteral("keep_")));
        runtime.DeleteAssistantConversation(first);
        QTRY_COMPARE_WITH_TIMEOUT(deleted.size(), 1, 5000);
        QCOMPARE(runtime.ExplainConversationId(), QString());
        QCOMPARE(changed.last().at(0).toString(), QString());

        const QList<QVariant> second = RunCameraTurn(runtime, false);
        QVERIFY(!second.isEmpty());
        QVERIFY(second.at(0).toString() != first);
        QCOMPARE(second.at(2).toString(), second.at(0).toString() + QStringLiteral(" explain first"));

        const qsizetype changesBeforeReset = changed.size();
        runtime.StartNewExplainConversation();
        QCOMPARE(runtime.ExplainConversationId(), QString());
        QCOMPARE(changed.size(), changesBeforeReset + 1);
        const QList<QVariant> third = RunCameraTurn(runtime, false);
        QVERIFY(!third.isEmpty());
        QVERIFY(third.at(0).toString() != second.at(0).toString());
        QCOMPARE(third.at(2).toString(), third.at(0).toString() + QStringLiteral(" explain first"));
    }

    void cancelledExplainOpeningCannotPublishAThread_data()
    {
        QTest::addColumn<QByteArray>("scenario");
        QTest::addColumn<QString>("rememberedThread");
        QTest::newRow("new-thread") << QByteArray("vision-lifecycle-hold-start") << QString();
        QTest::newRow("resumed-thread") << QByteArray("vision-lifecycle-hold-resume")
                                         << QStringLiteral("thr_saved");
    }

    void cancelledExplainOpeningCannotPublishAThread()
    {
        QFETCH(QByteArray, scenario);
        QFETCH(QString, rememberedThread);
        CodexFixture fixture(scenario);
        QVERIFY(fixture.directory.isValid());
        okuflow::AssistiveRuntime runtime;
        runtime.SetConfig(fixture.Config());
        runtime.SetExplainConversationId(rememberedThread);
        QSignalSpy status(&runtime, &okuflow::AssistiveRuntime::CodexServerStateChanged);
        QSignalSpy created(&runtime, &okuflow::AssistiveRuntime::ExplainConversationCreated);
        QSignalSpy changed(&runtime, &okuflow::AssistiveRuntime::ExplainConversationChanged);
        QSignalSpy finished(&runtime, &okuflow::AssistiveRuntime::AssistantTurnFinished);
        QImage frame(128, 96, QImage::Format_ARGB32);
        frame.fill(Qt::white);
        runtime.SubmitFrameForced(frame.constBits(), frame.width(), frame.height(), false);
        auto held = [&]() {
            for (const auto& row : status) {
                if (row.at(1).toString() == QStringLiteral("held opening")) return true;
            }
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(held(), 5000);
        runtime.StopAssistant();
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.first().at(4).toBool());
        QVERIFY(!runtime.IsBusy());
        QCOMPARE(runtime.ExplainConversationId(), rememberedThread);
        // The next Read releases the stale opening reply while creating its
        // own request. Neither reply is allowed to change Explain's anchor.
        const QList<QVariant> read = RunCameraTurn(runtime, true);
        QVERIFY(!read.isEmpty());
        QCOMPARE(read.at(3).toString(), QString());
        QCOMPARE(created.size(), 0);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(runtime.ExplainConversationId(), rememberedThread);
    }

    void endpointExplainKeepsBoundedTextHistoryAndReadStaysIndependent()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        FakeChatEndpoint endpoint;
        okuflow::AssistiveRuntime runtime;
        okuflow::AssistiveRuntimeConfig config;
        config.aiProvider = QStringLiteral("openai");
        config.vlmApiUrl = endpoint.Url();
        config.vlmModel = QStringLiteral("fixture-model");
        config.notesDirectory = directory.path();
        runtime.SetConfig(config);
        // A system proxy must not intercept the loopback fixture.
        const QNetworkProxy previousProxy = QNetworkProxy::applicationProxy();
        const auto restoreProxy = qScopeGuard([previousProxy]() {
            QNetworkProxy::setApplicationProxy(previousProxy);
        });
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QSignalSpy privacy(&runtime, &okuflow::AssistiveRuntime::PrivacyNotice);

        QVERIFY(RunEndpointRequest(runtime, endpoint, false)); // answer 1
        QVERIFY(FakeChatEndpoint::History(endpoint.requests.at(0)).isEmpty());
        QVERIFY(privacy.last().at(0).toString().contains(QStringLiteral("(local endpoint)")));
        QVERIFY(RunEndpointRequest(runtime, endpoint, true)); // answer 2, Read
        QVERIFY(FakeChatEndpoint::History(endpoint.requests.at(1)).isEmpty());
        QVERIFY(privacy.last().at(0).toString().contains(QStringLiteral("Conversation: temporary.")));
        QVERIFY(RunEndpointRequest(runtime, endpoint, false)); // answer 3
        QCOMPARE(FakeChatEndpoint::History(endpoint.requests.at(2)), QStringList{QStringLiteral("answer 1")});
        const QString continuedPrompt =
            endpoint.requests.at(2).value(QStringLiteral("messages")).toArray().last().toObject()
                .value(QStringLiteral("content")).toArray().first().toObject()
                .value(QStringLiteral("text")).toString();
        QVERIFY(continuedPrompt.contains(QStringLiteral("ongoing Explain conversation")));

        for (int i = 0; i < 4; ++i) {
            QVERIFY(RunEndpointRequest(runtime, endpoint, false)); // answers 4..7
        }
        // Bounded to the four most recent Explain answers; Read never enters.
        QCOMPARE(FakeChatEndpoint::History(endpoint.requests.last()),
                 (QStringList{QStringLiteral("answer 3"), QStringLiteral("answer 4"),
                              QStringLiteral("answer 5"), QStringLiteral("answer 6")}));

        // A different model never receives answers written by another one.
        config.vlmModel = QStringLiteral("other-model");
        runtime.SetConfig(config);
        QVERIFY(RunEndpointRequest(runtime, endpoint, false));
        QVERIFY(FakeChatEndpoint::History(endpoint.requests.last()).isEmpty());
        QVERIFY(RunEndpointRequest(runtime, endpoint, false));
        QCOMPARE(FakeChatEndpoint::History(endpoint.requests.last()).size(), 1);
        runtime.StartNewExplainConversation();
        QVERIFY(RunEndpointRequest(runtime, endpoint, false));
        QVERIFY(FakeChatEndpoint::History(endpoint.requests.last()).isEmpty());
    }

    void delayedEndpointAnswerCannotRepopulateExplainHistory_data()
    {
        QTest::addColumn<QString>("change");
        for (const char* change : {"endpoint", "model", "credential", "provider", "reset", "cancel"}) {
            QTest::newRow(change) << QString::fromLatin1(change);
        }
    }

    void failedAndOversizedExplainAnswersKeepHistoryBounded()
    {
        const QNetworkProxy previousProxy = QNetworkProxy::applicationProxy();
        const auto restoreProxy = qScopeGuard([previousProxy]() {
            QNetworkProxy::setApplicationProxy(previousProxy);
        });
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        FakeChatEndpoint endpoint;
        okuflow::AssistiveRuntime runtime;
        okuflow::AssistiveRuntimeConfig config;
        config.aiProvider = QStringLiteral("openai");
        config.vlmApiUrl = endpoint.Url();
        config.vlmModel = QStringLiteral("fixture-model");
        config.notesDirectory = directory.path();
        runtime.SetConfig(config);
        QVERIFY(RunEndpointRequest(runtime, endpoint, false));
        endpoint.failNextResponse = true;
        QVERIFY(RunEndpointRequest(runtime, endpoint, false));
        QCOMPARE(runtime.explainHistory_, QStringList{QStringLiteral("answer 1")});
        endpoint.answerOverride = QString(10000, QLatin1Char('x'));
        QVERIFY(RunEndpointRequest(runtime, endpoint, false));
        QCOMPARE(FakeChatEndpoint::History(endpoint.requests.last()),
                 QStringList{QStringLiteral("answer 1")});
        QVERIFY(RunEndpointRequest(runtime, endpoint, false));
        const QStringList history = FakeChatEndpoint::History(endpoint.requests.last());
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.first(), QStringLiteral("answer 1"));
        QCOMPARE(history.last().size(), qsizetype(1200));
    }

    void delayedEndpointAnswerCannotRepopulateExplainHistory()
    {
        QFETCH(QString, change);
        const QNetworkProxy previousProxy = QNetworkProxy::applicationProxy();
        const auto restoreProxy = qScopeGuard([previousProxy]() {
            QNetworkProxy::setApplicationProxy(previousProxy);
        });
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        FakeChatEndpoint endpoint;
        FakeChatEndpoint replacement;
        QVERIFY(!endpoint.Url().contains(QStringLiteral(":0/")));
        QVERIFY(!replacement.Url().contains(QStringLiteral(":0/")));
        okuflow::AssistiveRuntime runtime;
        okuflow::AssistiveRuntimeConfig config;
        config.aiProvider = QStringLiteral("openai");
        config.vlmApiUrl = endpoint.Url();
        config.vlmModel = QStringLiteral("fixture-model");
        config.notesDirectory = directory.path();
        runtime.SetConfig(config);
        endpoint.holdNextResponse = true;
        QImage frame(128, 96, QImage::Format_ARGB32);
        frame.fill(Qt::white);
        runtime.SubmitFrameForced(frame.constBits(), frame.width(), frame.height(), false);
        QTRY_COMPARE_WITH_TIMEOUT(endpoint.requests.size(), 1, 5000);
        QVERIFY(runtime.IsBusy());
        FakeChatEndpoint* nextEndpoint = &endpoint;
        if (change == QStringLiteral("reset")) {
            runtime.StartNewExplainConversation();
        } else if (change == QStringLiteral("cancel")) {
            runtime.StopAssistant();
        } else {
            if (change == QStringLiteral("endpoint")) {
                config.vlmApiUrl = replacement.Url();
                nextEndpoint = &replacement;
            } else if (change == QStringLiteral("model")) {
                config.vlmModel = QStringLiteral("replacement-model");
            } else if (change == QStringLiteral("credential")) {
                config.vlmApiKey = QStringLiteral("fixture-key");
            } else {
                // Switch away and back without launching any external provider.
                config.aiProvider = QStringLiteral("local");
                runtime.SetConfig(config);
                config.aiProvider = QStringLiteral("openai");
            }
            runtime.SetConfig(config);
        }
        endpoint.ReleaseResponse();
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.IsBusy(), 5000);
        QVERIFY(RunEndpointRequest(runtime, *nextEndpoint, false));
        QVERIFY(FakeChatEndpoint::History(nextEndpoint->requests.last()).isEmpty());
        QVERIFY(RunEndpointRequest(runtime, *nextEndpoint, false));
        QCOMPARE(FakeChatEndpoint::History(nextEndpoint->requests.last()).size(), 1);
    }
};
QTEST_GUILESS_MAIN(AssistivePreparationTests)
#include "assistive_preparation_tests.moc"
