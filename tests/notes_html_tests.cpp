// Regression tests for the appended lecture-notes HTML (plan 36 redesign):
// head written once with theme toggle and viewer script, every entry a
// self-contained appended block, escaped text sections,
// collapsible note sections, duplicate-final dedupe. No network, no media
// files needed. Set OKUFLOW_NOTES_TEST_DIR to keep the generated file for
// visual inspection.

#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest>
#include <QThreadPool>
#include <QSemaphore>
#include <QScopeGuard>

#include "okuflow/common/assistive_runtime.hpp"
#include "okuflow/common/codex_app_server_client.hpp"

using namespace okuflow;

class NotesHtmlTests : public QObject {
    Q_OBJECT

    QString notesDirectory_;
    QScopedPointer<QTemporaryDir> temporaryDirectory_;

    AssistiveRuntimeConfig NotesConfig() const
    {
        AssistiveRuntimeConfig config;
        config.aiProvider = QStringLiteral("openai");
        config.lectureNotesEnabled = true;
        config.notesDirectory = notesDirectory_;
        return config;
    }

    static QString ReadAll(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }

private slots:
    void clientOpeningCancellation_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QString>("thread");
        QTest::addColumn<bool>("replacement");
        QTest::newRow("start-replacement") << QStringLiteral("vision-lifecycle-hold-start") << QString() << true;
        QTest::newRow("start-no-replacement") << QStringLiteral("vision-lifecycle-hold-start") << QString() << false;
        QTest::newRow("resume-replacement") << QStringLiteral("vision-lifecycle-hold-resume") << QStringLiteral("thr_saved") << true;
    }

    void clientOpeningCancellation()
    {
        QFETCH(QString, scenario); QFETCH(QString, thread); QFETCH(bool, replacement);
        const QByteArray previous = qgetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
        const auto restore = qScopeGuard([previous]() { if (previous.isNull()) qunsetenv("OKUFLOW_FAKE_CODEX_SCENARIO"); else qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", previous); });
        qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", scenario.toUtf8());
        CodexAppServerClient client;
        client.Configure(QCoreApplication::applicationDirPath() + QStringLiteral("/fake_codex_app_server.exe"), {}, {}, {}, false, false, {});
        QSignalSpy status(&client, &CodexAppServerClient::ServerStateChanged);
        QSignalSpy finished(&client, &CodexAppServerClient::TurnFinished);
        QSignalSpy started(&client, &CodexAppServerClient::TurnStarted);
        QString imageAPath;
        {
            QTemporaryFile imageA; QVERIFY(imageA.open());
            imageA.setAutoRemove(false);
            imageAPath = imageA.fileName();
        } // Destroy the fixture handle before handing ownership to the client.
        client.RequestVisionTurn(QStringLiteral("A"), imageAPath, thread, !thread.isEmpty());
        auto held = [&]() { for (const auto& row : status) if (row.at(1).toString() == QStringLiteral("held opening")) return true; return false; };
        QTRY_VERIFY_WITH_TIMEOUT(held(), 5000);
        client.RefreshAccount(); // Duplicate readiness cannot issue a second opening.
        client.InterruptTurn();
        QCOMPARE(finished.size(), 1);
        QVERIFY(!QFileInfo::exists(imageAPath));
        QVERIFY(!client.IsTurnActive());
        if (replacement) {
            QString imageBPath;
            {
                QTemporaryFile imageB; QVERIFY(imageB.open());
                imageB.setAutoRemove(false);
                imageBPath = imageB.fileName();
            }
            client.RequestVisionTurn(QStringLiteral("B"), imageBPath, thread, !thread.isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
            QCOMPARE(finished.last().at(2).toString(), QStringLiteral("B"));
            QCOMPARE(finished.last().at(3).toString(), QString());
            QCOMPARE(started.size(), 1);
            QVERIFY(!QFileInfo::exists(imageBPath));
        } else {
            client.RefreshAccount();
            QTest::qWait(100);
            QCOMPARE(finished.size(), 1);
            QCOMPARE(started.size(), 0);
        }
        client.InterruptTurn();
        QCOMPARE(finished.size(), replacement ? 2 : 1); // Idle Stop emits nothing.
    }

    void clientDuplicateReadinessDoesNotOpenTwice()
    {
        const QByteArray previous = qgetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
        const auto restore = qScopeGuard([previous]() { if (previous.isNull()) qunsetenv("OKUFLOW_FAKE_CODEX_SCENARIO"); else qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", previous); });
        qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", "vision-lifecycle-hold-duplicate");
        CodexAppServerClient client;
        client.Configure(QCoreApplication::applicationDirPath() + QStringLiteral("/fake_codex_app_server.exe"), {}, {}, {}, false, false, {});
        QSignalSpy status(&client, &CodexAppServerClient::ServerStateChanged);
        QSignalSpy finished(&client, &CodexAppServerClient::TurnFinished);
        client.RequestVisionTurn(QStringLiteral("one opening"), {}, {}, false);
        auto held = [&]() { for (const auto& row : status) if (row.at(1).toString() == QStringLiteral("held opening")) return true; return false; };
        QTRY_VERIFY_WITH_TIMEOUT(held(), 5000);
        client.RefreshAccount();
        client.RefreshAccount();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.first().at(2).toString(), QStringLiteral("one opening"));
        QCOMPARE(finished.first().at(3).toString(), QString());
    }

    void clientStartingCancellationWaitsForInterrupt()
    {
        const QByteArray previous = qgetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
        const auto restore = qScopeGuard([previous]() { if (previous.isNull()) qunsetenv("OKUFLOW_FAKE_CODEX_SCENARIO"); else qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", previous); });
        qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", "vision-lifecycle-hold-turn");
        CodexAppServerClient client;
        client.Configure(QCoreApplication::applicationDirPath() + QStringLiteral("/fake_codex_app_server.exe"), {}, {}, {}, false, false, {});
        QSignalSpy status(&client, &CodexAppServerClient::ServerStateChanged);
        QSignalSpy finished(&client, &CodexAppServerClient::TurnFinished);
        QSignalSpy started(&client, &CodexAppServerClient::TurnStarted);
        client.RequestVisionTurn(QStringLiteral("A"), {}, {}, true);
        auto held = [&]() { for (const auto& row : status) if (row.at(1).toString() == QStringLiteral("held turn")) return true; return false; };
        QTRY_VERIFY_WITH_TIMEOUT(held(), 5000);
        client.InterruptTurn();
        QVERIFY(client.IsTurnActive());
        QCOMPARE(finished.size(), 0);
        client.RefreshAccount(); // Release delayed turn/start, then acknowledge interrupt.
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QVERIFY(finished.first().at(4).toBool());
        QCOMPARE(started.size(), 0);
        QVERIFY(!client.IsTurnActive());
    }

    void clientControlPlaneFailureRetry_data()
    {
        QTest::addColumn<QByteArray>("scenario");
        QTest::newRow("error") << QByteArray("vision-lifecycle-init-error");
        QTest::newRow("init-timeout") << QByteArray("vision-lifecycle-init-timeout");
        QTest::newRow("turn-start-timeout") << QByteArray("vision-lifecycle-hold-turn");
    }

    void clientControlPlaneFailureRetry()
    {
        QFETCH(QByteArray, scenario);
        const QByteArray previous = qgetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
        const auto restore = qScopeGuard([previous]() { if (previous.isNull()) qunsetenv("OKUFLOW_FAKE_CODEX_SCENARIO"); else qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", previous); });
        qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", scenario);
        CodexAppServerClient client;
        client.Configure(QCoreApplication::applicationDirPath() + QStringLiteral("/fake_codex_app_server.exe"), {}, {}, {}, false, false, {});
        client.SetRequestTimeoutMs(100);
        QSignalSpy status(&client, &CodexAppServerClient::ServerStateChanged);
        QSignalSpy finished(&client, &CodexAppServerClient::TurnFinished);
        client.RequestVisionTurn(QStringLiteral("A"), {}, {}, false);
        // Transport deadlines are checked at a 5 s cadence; allow one poll
        // plus event-loop/process shutdown overhead for the 100 ms deadline.
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 7000);
        QVERIFY(!finished.first().at(3).toString().isEmpty());
        if (scenario.endsWith("hold-turn")) {
            bool reachedTurnStart = false;
            for (const auto& row : status) reachedTurnStart |= row.at(1).toString() == QStringLiteral("held turn");
            QVERIFY(reachedTurnStart);
        }
        QVERIFY(!client.IsTurnActive());
        qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", "vision-lifecycle-happy");
        client.SetRequestTimeoutMs(5000);
        client.RequestVisionTurn(QStringLiteral("B"), {}, {}, false);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
        QCOMPARE(finished.last().at(2).toString(), QStringLiteral("B"));
        QCOMPARE(finished.last().at(3).toString(), QString());
    }

    void onDemandVisionUsesLunaAndPreservesReading_data()
    {
        QTest::addColumn<bool>("readText");
        QTest::addColumn<QString>("language");
        for (const QString& language : {QStringLiteral("en"), QStringLiteral("tr"), QStringLiteral("de")}) {
            QTest::newRow(qPrintable(QStringLiteral("read-text-%1").arg(language))) << true << language;
            QTest::newRow(qPrintable(QStringLiteral("explain-scene-%1").arg(language))) << false << language;
        }
    }

    void onDemandVisionUsesLunaAndPreservesReading()
    {
        QFETCH(bool, readText);
        QFETCH(QString, language);
        const QByteArray oldScenario = qgetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
        const auto restore = qScopeGuard([oldScenario]() {
            if (oldScenario.isNull()) qunsetenv("OKUFLOW_FAKE_CODEX_SCENARIO");
            else qputenv("OKUFLOW_FAKE_CODEX_SCENARIO", oldScenario);
        });
        qputenv("OKUFLOW_FAKE_CODEX_SCENARIO",
                (QStringLiteral("vision-%1-%2").arg(readText ? QStringLiteral("read")
                                                             : QStringLiteral("explain"), language)).toUtf8());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AssistiveRuntime runtime;
        AssistiveRuntimeConfig config;
        config.codexExecutablePath = QCoreApplication::applicationDirPath() +
                                     QStringLiteral("/fake_codex_app_server.exe");
        config.notesDirectory = directory.path();
        // Advanced opt-ins must never grant tools/network to Read or Explain.
        config.codexInternetEnabled = true;
        config.codexCodingEnabled = true;
        config.codexWorkspaceDirectory = directory.path();
        runtime.SetConfig(config);
        runtime.SetResponseLanguage(language);
        runtime.SetModes(true);
        QVERIFY(!runtime.WantsAnalysis());
        QSignalSpy finished(&runtime, &AssistiveRuntime::AssistantTurnFinished);
        QSignalSpy status(&runtime, &AssistiveRuntime::StatusNotice);
        QSignalSpy overlay(&runtime, &AssistiveRuntime::OverlayUpdated);
        QImage frame(128, 96, QImage::Format_ARGB32);
        frame.fill(Qt::white);
        runtime.SubmitFrameForced(frame.constBits(), frame.width(), frame.height(), readText);
        QVERIFY(runtime.IsBusy());
        runtime.SubmitFrameForced(frame.constBits(), frame.width(), frame.height(), !readText);
        QCOMPARE(status.size(), 1); // Busy submission cannot replace the pending request.
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(finished.first().at(3).toString(), QString());
        QVERIFY(!finished.first().at(4).toBool());
        QVERIFY(!finished.first().at(5).toBool());
        QVERIFY(!runtime.IsBusy());
        QVERIFY(!overlay.isEmpty());
        const QString body = overlay.last().at(1).toString();
        QVERIFY(body.startsWith(readText ? QStringLiteral("Read Text\n") : QStringLiteral("Scene Explain\n")));
        const QString fullText = QStringLiteral("Visible line 123. ").repeated(80).trimmed();
        if (readText) QVERIFY(body.endsWith(fullText));
        else QVERIFY(body.size() < fullText.size());
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.HasPendingNotesWrites(), 5000);
        const QString notes = ReadAll(runtime.notesFilePath());
        QVERIFY(notes.contains(fullText));
        QVERIFY(notes.contains(readText ? QStringLiteral("Text on screen") : QStringLiteral("Scene explanation")));
    }

    void initTestCase()
    {
        const QByteArray keepDirectory = qgetenv("OKUFLOW_NOTES_TEST_DIR");
        if (!keepDirectory.isEmpty()) {
            notesDirectory_ = QString::fromLocal8Bit(keepDirectory);
            QDir().mkpath(notesDirectory_);
        } else {
            temporaryDirectory_.reset(new QTemporaryDir());
            QVERIFY(temporaryDirectory_->isValid());
            notesDirectory_ = temporaryDirectory_->path();
        }
    }

    void analyzedImagesEncodeOnCommitAndCanceledFramesLeaveNoOrphans()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AssistiveRuntime runtime;
        auto config = NotesConfig();
        config.notesDirectory = directory.path();
        runtime.SetConfig(config);
        QSignalSpy writes(&runtime, &AssistiveRuntime::NotesWriteFinished);
        QImage frame(2400, 1200, QImage::Format_RGB32);
        frame.fill(Qt::green);
        const QString canceled = runtime.SaveAnalyzedImageForNotes(frame);
        QVERIFY(!canceled.isEmpty());
        QVERIFY(!QFile::exists(canceled));
        runtime.RemoveNoteImage(canceled);
        QVERIFY(runtime.noteImages_.isEmpty());
        const QString image = runtime.SaveAnalyzedImageForNotes(frame);
        QVERIFY(runtime.AppendNoteSection(QStringLiteral("Image"), QStringLiteral("result"), image, true));
        QTRY_COMPARE(writes.size(), 2);
        for (const auto& write : writes) QVERIFY(write.at(1).toString().isEmpty());
        QCOMPARE(QImage(image).size(), QSize(1920, 960));
        QVERIFY(!QFile::exists(canceled));
        QVERIFY(runtime.noteImages_.isEmpty());
        // A failed append must remove the just-encoded image too.
        QVERIFY(QFile::remove(runtime.notesFilePath()));
        writes.clear();
        const QString orphan = runtime.SaveAnalyzedImageForNotes(frame);
        QVERIFY(runtime.AppendNoteSection(QStringLiteral("Fail"), QStringLiteral("result"), orphan, true));
        QTRY_COMPARE(writes.size(), 1);
        QVERIFY(!writes.front().at(1).toString().isEmpty());
        QVERIFY(!QFile::exists(orphan));
    }

    void storageIsQueuedOrderedAndCapturesItsDestination()
    {
        // Occupy the shared pool deterministically: the UI must still accept
        // notes and a destination change while no storage task can execute.
        auto* pool = QThreadPool::globalInstance();
        const int oldCount = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore started, release;
        struct Unblock {
            QSemaphore& release;
            QThreadPool* pool;
            int count;
            ~Unblock() { release.release(); pool->waitForDone(); pool->setMaxThreadCount(count); }
        } unblock{release, pool, oldCount};
        pool->start([&] { started.release(); release.acquire(); });
        started.acquire();
        AssistiveRuntime runtime;
        auto config = NotesConfig();
        config.notesDirectory += QStringLiteral("/ordered-first");
        runtime.SetConfig(config);
        QSignalSpy writes(&runtime, &AssistiveRuntime::NotesWriteFinished);
        QVERIFY(runtime.AppendNoteSection(QStringLiteral("Note"), QStringLiteral("first entry")));
        QVERIFY(runtime.HasPendingNotesWrites());
        const QString firstPath = runtime.notesFilePath();
        QVERIFY(!QFile::exists(firstPath));
        QVERIFY(runtime.AppendNoteSection(QStringLiteral("Note"), QStringLiteral("second entry")));
        config.notesDirectory += QStringLiteral("/next");
        runtime.SetConfig(config);
        QVERIFY(runtime.AppendNoteSection(QStringLiteral("Note"), QStringLiteral("new target")));
        const QString nextPath = runtime.notesFilePath();
        QVERIFY(firstPath != nextPath);
        QCOMPARE(writes.size(), 0);
        release.release();
        QTRY_COMPARE(writes.size(), 6);
        QVERIFY(!runtime.HasPendingNotesWrites());
        for (const auto& write : writes) QVERIFY(write.at(1).toString().isEmpty());
        const QString first = ReadAll(firstPath);
        QVERIFY(first.indexOf(QStringLiteral("first entry")) < first.indexOf(QStringLiteral("second entry")));
        QVERIFY(first.endsWith(QStringLiteral("</html>\n")));
        QVERIFY(!first.contains(QStringLiteral("new target")));
        QVERIFY(ReadAll(nextPath).contains(QStringLiteral("new target")));
    }

    void failedStorageReportsFailureAndAllowsRetry()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString blocker = directory.filePath(QStringLiteral("blocked"));
        QFile file(blocker);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        AssistiveRuntime runtime;
        auto config = NotesConfig();
        config.notesDirectory = blocker;
        runtime.SetConfig(config);
        QSignalSpy writes(&runtime, &AssistiveRuntime::NotesWriteFinished);
        QVERIFY(runtime.AppendNoteSection(QStringLiteral("Note"), QStringLiteral("retry this final")));
        QTRY_COMPARE(writes.size(), 2);
        for (const auto& write : writes) QVERIFY(!write.at(1).toString().isEmpty());
        QVERIFY(QFile::remove(blocker));
        writes.clear();
        QVERIFY(runtime.AppendNoteSection(QStringLiteral("Note"), QStringLiteral("retry this final")));
        const QString path = runtime.notesFilePath();
        QTRY_COMPARE(writes.size(), 2);
        for (const auto& write : writes) QVERIFY(write.at(1).toString().isEmpty());
        QCOMPARE(ReadAll(path).count(QStringLiteral("retry this final")), 1);
    }

    void queueRejectsExcessAndOwnerCanBeDestroyedBeforeStorageRuns()
    {
        auto* pool = QThreadPool::globalInstance();
        const int oldCount = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        QSemaphore started, release;
        struct Unblock {
            QSemaphore& release;
            QThreadPool* pool;
            int count;
            ~Unblock() { release.release(); pool->waitForDone(); pool->setMaxThreadCount(count); }
        } unblock{release, pool, oldCount};
        pool->start([&] { started.release(); release.acquire(); });
        started.acquire();
        QString path;
        {
            AssistiveRuntime runtime;
            auto config = NotesConfig();
            config.notesDirectory += QStringLiteral("/bounded");
            runtime.SetConfig(config);
            QSignalSpy writes(&runtime, &AssistiveRuntime::NotesWriteFinished);
            for (quint64 i = 1; i <= 127; ++i)
                QVERIFY(runtime.AppendNoteSection(QStringLiteral("Note"), QStringLiteral("accepted")));
            QVERIFY(!runtime.AppendNoteSection(QStringLiteral("Note"), QStringLiteral("rejected")));
            QCOMPARE(writes.size(), 1);
            QVERIFY(!writes.front().at(1).toString().isEmpty());
            path = runtime.notesFilePath();
            // Destruction must not wait for the blocked notes worker.
        }
        release.release();
        QTRY_COMPARE(ReadAll(path).count(QStringLiteral("<p class=\"note-text\">accepted")), 127);
        QVERIFY(!ReadAll(path).contains(QStringLiteral("rejected")));
    }

    void appendedDocumentStaysStyledEscapedAndDeduplicated()
    {
        AssistiveRuntime runtime;
        runtime.SetConfig(NotesConfig());
        QSignalSpy writes(&runtime, &AssistiveRuntime::NotesWriteFinished);

        // Media pair section (collapsible card with the paired videos).
        runtime.NoteCapturedVideoPair(
            notesDirectory_ + QStringLiteral("/VID_original.mp4"),
            notesDirectory_ + QStringLiteral("/VID_processed.mp4"));

        // A text section with an injection attempt that must stay quoted text.
        QVERIFY(runtime.AppendNoteSection(
            QStringLiteral("Read text"),
            QStringLiteral("<script>alert('x')</script> \"quotes\" & <b>tags</b>")));

        // A plain annotated-view section.
        runtime.NoteAnnotationSnapshot(
            notesDirectory_ + QStringLiteral("/IMG_annotated.png"),
            QStringLiteral("Annotated view"));

        // A second recording.
        runtime.NoteCapturedVideoPair(
            notesDirectory_ + QStringLiteral("/VID2_original.mp4"),
            notesDirectory_ + QStringLiteral("/VID2_processed.mp4"));

        QTRY_COMPARE(writes.size(), 5);
        for (const auto& write : writes) QVERIFY(write.at(1).toString().isEmpty());

        const QString path = runtime.notesFilePath();
        QVERIFY(!path.isEmpty());
        const QString content = ReadAll(path);
        QVERIFY(!content.isEmpty());

        // Head: written once — theme toggle, expand/collapse controls, the
        // transcript label for the viewer script, and no external resource.
        QCOMPARE(content.count(QStringLiteral("id=\"btn-theme\"")), 1);
        QCOMPARE(content.count(QStringLiteral("id=\"btn-expand\"")), 1);
        QCOMPARE(content.count(QStringLiteral("id=\"btn-collapse\"")), 1);
        QVERIFY(content.contains(QStringLiteral("prefers-color-scheme")));
        QVERIFY(!content.contains(QStringLiteral("http://")));
        QVERIFY(content.contains(QStringLiteral("https://okuflow.com/transcribe/")));
        QCOMPARE(content.count(QStringLiteral("data-transcribe-src=\"")), 2);
        QVERIFY(content.contains(QStringLiteral(".okuflow-transcript.json")));
        QVERIFY(!content.contains(QStringLiteral("Consolidate EVERY transcript")));

        // Sections are native collapsibles with a type icon, the heading in
        // the summary for screen-reader navigation, and the timestamp last.
        QVERIFY(content.contains(QStringLiteral("<details class=\"note")));
        QVERIFY(content.contains(
            QStringLiteral("<summary><span class=\"ic\" aria-hidden=\"true\"></span>"
                           "<h2 class=\"ttl\">")));
        QVERIFY(content.contains(QStringLiteral("<time class=\"at\">")));
        QVERIFY(content.contains(QStringLiteral("media-grid")));
        // The video pair is tagged as a media card with the video kind icon.
        QVERIFY(content.contains(QStringLiteral("class=\"note media\"")));
        QVERIFY(content.contains(QStringLiteral("data-kind=\"video\"")));
        // The head defines all type icons, the single-block consolidation,
        // the after-the-video placement, and the analyzed-image style.
        QVERIFY(content.contains(QStringLiteral("figure.ai-shot")));
        // Conversation grouping: label, icon, and the grouping query.
        QVERIFY(content.contains(QStringLiteral("data-l10n-conversation=")));
        QVERIFY(content.contains(QStringLiteral("[data-kind=\"convo\"]")));
        QVERIFY(content.contains(
            QStringLiteral("details[data-conversation]")));

        // Escaping: the injection attempt is quoted text, never markup.
        QVERIFY(!content.contains(QStringLiteral("<script>alert")));
        QVERIFY(content.contains(QStringLiteral("&lt;script&gt;alert")));
        QVERIFY(content.contains(QStringLiteral("&amp; &lt;b&gt;")));

        // Abrupt-exit readability: the file has no closing </html> while
        // open, and each appended block is complete in itself.
        QVERIFY(!content.contains(QStringLiteral("</html>")));
        QCOMPARE(content.count(QStringLiteral("<details")),
                 content.count(QStringLiteral("</details>")));

        std::printf("notes sample: %s\n", qPrintable(path));
    }
};

QTEST_MAIN(NotesHtmlTests)
#include "notes_html_tests.moc"
