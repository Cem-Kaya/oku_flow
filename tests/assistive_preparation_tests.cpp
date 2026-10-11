// Deterministic preparation completions; no provider, camera, or credentials.
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QtTest>
#include "okuflow/common/assistive_runtime.hpp"
#include "okuflow/common/codex_app_server_client.hpp"
#include <QScopeGuard>

class AssistivePreparationTests : public QObject {
    Q_OBJECT
private:
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
};
QTEST_GUILESS_MAIN(AssistivePreparationTests)
#include "assistive_preparation_tests.moc"
