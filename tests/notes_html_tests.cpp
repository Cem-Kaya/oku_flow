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
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>

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

    struct MediaCardDom {
        QMap<QString, QString> attributes;
        int videos{}, figures{}, mediaGrids{};
        QStringList videoSources, imageSources, transcribeSources;
        QList<QJsonObject> recordingIdentities;
    };
    struct MediaCardsDom {
        QList<MediaCardDom> cards;
        QString error;
    };

    static MediaCardsDom ParseMediaCards(const QString& html)
    {
        MediaCardsDom result;
        // Parse the app-owned appended DOM, excluding header scripts/styles.
        // The document deliberately stays open while notes are being written.
        const int mainTag = html.indexOf(QStringLiteral("<main id=\"notes\">"));
        if (mainTag < 0) { result.error = QStringLiteral("Missing notes main element"); return result; }
        const int begin = html.indexOf(QLatin1Char('>'), mainTag) + 1;
        const int end = html.indexOf(QStringLiteral("</main>"), begin);
        const QString fragment = html.mid(begin, end < 0 ? -1 : end - begin);
        // QXmlStreamReader gives structural elements/decoded attributes after
        // normalizing only HTML boolean attributes and void source/img tags.
        // No visible text or attribute values are rewritten.
        const QRegularExpression tags(QStringLiteral("<[A-Za-z][^>]*>"));
        const QRegularExpression booleanAttribute(QStringLiteral("\\s(controls|open|hidden)(?=\\s|>)"));
        const QRegularExpression voidTag(QStringLiteral("^<(?:source|img)\\b"));
        QString xml = QStringLiteral("<main>");
        qsizetype offset = 0;
        auto matches = tags.globalMatch(fragment);
        while (matches.hasNext()) {
            const auto match = matches.next();
            xml += fragment.mid(offset, match.capturedStart() - offset);
            QString tag = match.captured();
            tag.replace(booleanAttribute, QStringLiteral(" \\1=\"\""));
            if (voidTag.match(tag).hasMatch() && !tag.endsWith(QStringLiteral("/>"))) {
                tag.chop(1); tag += QStringLiteral("/>");
            }
            xml += tag; offset = match.capturedEnd();
        }
        xml += fragment.mid(offset) + QStringLiteral("</main>");
        QXmlStreamReader reader(xml);
        QStringList stack;
        qsizetype cardDepth = -1;
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement()) {
                const QString name = reader.name().toString();
                const QString parent = stack.isEmpty() ? QString() : stack.last();
                stack.append(name);
                const auto attributes = reader.attributes();
                if (name == QStringLiteral("details")) {
                    if (cardDepth >= 0) { result.error = QStringLiteral("Nested media card"); return result; }
                    result.cards.append(MediaCardDom{}); cardDepth = stack.size();
                    for (const auto& attribute : attributes)
                        result.cards.last().attributes.insert(attribute.name().toString(), attribute.value().toString());
                }
                if (cardDepth < 0) continue;
                auto& card = result.cards.last();
                if (name == QStringLiteral("figure")) ++card.figures;
                if (name == QStringLiteral("video")) ++card.videos;
                if (name == QStringLiteral("source")) {
                    if (parent != QStringLiteral("video")) { result.error = QStringLiteral("Video source outside video"); return result; }
                    card.videoSources.append(attributes.value(QStringLiteral("src")).toString());
                }
                if (name == QStringLiteral("img")) card.imageSources.append(attributes.value(QStringLiteral("src")).toString());
                const auto classes = attributes.value(QStringLiteral("class")).toString().split(QLatin1Char(' '));
                if (name == QStringLiteral("div") && classes.contains(QStringLiteral("media-grid"))) ++card.mediaGrids;
                if (name == QStringLiteral("section") && classes.contains(QStringLiteral("transcribe-tools"))) {
                    card.transcribeSources.append(attributes.value(QStringLiteral("data-transcribe-src")).toString());
                    card.recordingIdentities.append(QJsonDocument::fromJson(
                        attributes.value(QStringLiteral("data-recording-identity")).toString().toUtf8()).object());
                }
            } else if (reader.isEndElement()) {
                if (stack.size() == cardDepth) cardDepth = -1;
                if (!stack.isEmpty()) stack.removeLast();
            }
        }
        if (reader.hasError()) result.error = reader.errorString();
        return result;
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
        const auto chatBefore = runtime.ChatSession().Snapshot();
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
        const QString fullText = QStringLiteral("Visible line 123. ").repeated(80).trimmed();
        if (readText) {
            QVERIFY(!overlay.isEmpty());
            const QString body = overlay.last().at(1).toString();
            QVERIFY(body.startsWith(QStringLiteral("Read Text\n"))); QVERIFY(body.endsWith(fullText));
            QCOMPARE(runtime.ChatSession().Snapshot(),chatBefore);
        } else {
            // Explain owns the shared full-answer chat, never the Read channel.
            QVERIFY(overlay.isEmpty());
            const auto rows = runtime.ChatSession().Snapshot().value("messages").toArray();
            QCOMPARE(rows.size(),2); QCOMPARE(rows[0].toObject().value("origin").toString(),"explain");
            QCOMPARE(rows[1].toObject().value("text").toString(),fullText);
            QVERIFY(rows[1].toObject().value("complete").toBool());
        }
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.HasPendingNotesWrites(), 5000);
        const QString notes = ReadAll(runtime.notesFilePath());
        QVERIFY(notes.contains(fullText));
        QVERIFY(notes.contains(readText ? QStringLiteral("Text on screen") : QStringLiteral("Scene explanation")));
        QCOMPARE(notes.count(QStringLiteral("<div class=\"note-body\">")),1);
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

    void originalOnlyVideoAppearsOnceAndPreservesTranscriptionIdentity()
    {
        QTemporaryDir fixture; QVERIFY(fixture.isValid());
        const QString safeOriginal = fixture.filePath(QStringLiteral("original & lecture.mp4"));
        QFile media(safeOriginal); QVERIFY(media.open(QIODevice::WriteOnly));
        QCOMPARE(media.write("sole original recording"), 23); media.close();
        AssistiveRuntime runtime;
        auto config = NotesConfig(); config.notesDirectory = fixture.path(); runtime.SetConfig(config);
        QSignalSpy writes(&runtime, &AssistiveRuntime::NotesWriteFinished);
        runtime.NoteCapturedVideo(safeOriginal, QStringLiteral("recording-session-a"), 2, 1234567);
        QTRY_COMPARE(writes.size(), 2);
        QVERIFY(writes.last().at(1).toString().isEmpty());
        const auto dom = ParseMediaCards(ReadAll(runtime.notesFilePath()));
        QVERIFY2(dom.error.isEmpty(), qPrintable(dom.error));
        QCOMPARE(dom.cards.size(), 1);
        const auto& card = dom.cards.first();
        QCOMPARE(card.attributes.value(QStringLiteral("data-kind")), QStringLiteral("video"));
        QCOMPARE(card.videos, 1); QCOMPARE(card.figures, 1); QCOMPARE(card.mediaGrids, 0);
        QVERIFY(card.imageSources.isEmpty());
        const QString expectedUrl = QString::fromUtf8(QUrl(QFileInfo(safeOriginal).fileName()).toEncoded(QUrl::FullyEncoded));
        QCOMPARE(card.videoSources, QStringList{expectedUrl});
        QCOMPARE(card.transcribeSources, card.videoSources);
        QCOMPARE(card.recordingIdentities.size(), 1);
        QCOMPARE(card.attributes.value(QStringLiteral("data-recording-session")), QStringLiteral("recording-session-a"));
        QCOMPARE(card.attributes.value(QStringLiteral("data-recording-part")), QStringLiteral("2"));
        QCOMPARE(card.attributes.value(QStringLiteral("data-source-clock-origin-100ns")), QStringLiteral("1234567"));
        const auto identity = QJsonDocument::fromJson(
            ReadAll(safeOriginal + QStringLiteral(".okuflow-media.json")).toUtf8()).object();
        QCOMPARE(identity.value(QStringLiteral("name")).toString(), QFileInfo(safeOriginal).fileName());
        QCOMPARE(identity.value(QStringLiteral("size")).toInt(), 23);
        QVERIFY(!identity.value(QStringLiteral("mediaId")).toString().isEmpty());
        QCOMPARE(card.recordingIdentities.first(), identity);
        QVERIFY(!QFileInfo::exists(fixture.filePath(QStringLiteral("processed.mp4.okuflow-media.json"))));
    }

    void legacyVideoAndPhotoPairsKeepBothViews()
    {
        QTemporaryDir fixture; QVERIFY(fixture.isValid());
        const QString original = fixture.filePath(QStringLiteral("legacy_original.mp4"));
        QFile media(original); QVERIFY(media.open(QIODevice::WriteOnly));
        QCOMPARE(media.write("legacy recording"), 16); media.close();
        AssistiveRuntime runtime;
        auto config = NotesConfig(); config.notesDirectory = fixture.path(); runtime.SetConfig(config);
        QSignalSpy writes(&runtime, &AssistiveRuntime::NotesWriteFinished);
        runtime.NoteCapturedVideoPair(original, fixture.filePath(QStringLiteral("legacy_processed.mp4")));
        runtime.NoteCapturedPhotoPair(fixture.filePath(QStringLiteral("photo_original.jpg")),
                                      fixture.filePath(QStringLiteral("photo_processed.jpg")));
        QTRY_COMPARE(writes.size(), 3);
        for (const auto& write : writes) QVERIFY(write.at(1).toString().isEmpty());
        const auto dom = ParseMediaCards(ReadAll(runtime.notesFilePath()));
        QVERIFY2(dom.error.isEmpty(), qPrintable(dom.error));
        QCOMPARE(dom.cards.size(), 2);
        const auto& video = dom.cards[0];
        QCOMPARE(video.attributes.value(QStringLiteral("data-kind")), QStringLiteral("video"));
        QCOMPARE(video.videos, 2); QCOMPARE(video.figures, 2); QCOMPARE(video.mediaGrids, 1);
        QCOMPARE(video.videoSources, (QStringList{QStringLiteral("legacy_original.mp4"), QStringLiteral("legacy_processed.mp4")}));
        QCOMPARE(video.transcribeSources, QStringList{QStringLiteral("legacy_original.mp4")});
        QCOMPARE(video.recordingIdentities.size(), 1);
        QVERIFY(!video.recordingIdentities.first().isEmpty());
        QVERIFY(video.imageSources.isEmpty());
        const auto& photo = dom.cards[1];
        QCOMPARE(photo.attributes.value(QStringLiteral("data-kind")), QStringLiteral("photo"));
        QCOMPARE(photo.videos, 0); QCOMPARE(photo.figures, 2); QCOMPARE(photo.mediaGrids, 1);
        QCOMPARE(photo.imageSources, (QStringList{QStringLiteral("photo_original.jpg"), QStringLiteral("photo_processed.jpg")}));
        QVERIFY(photo.videoSources.isEmpty()); QVERIFY(photo.transcribeSources.isEmpty());
        QVERIFY(photo.recordingIdentities.isEmpty());
    }

    void recordingIdentityIsStableAndRefusesReplacedBytes()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QString original = fixture.filePath(QStringLiteral("video.mp4"));
        const QString marker = original + QStringLiteral(".okuflow-media.json");
        auto writeMedia = [&](const QByteArray& bytes) {
            QFile media(original);
            return media.open(QIODevice::WriteOnly) && media.write(bytes) == bytes.size();
        };
        QVERIFY(writeMedia("fixture recording"));
        AssistiveRuntime runtime;
        auto config = NotesConfig();
        config.notesDirectory = fixture.path();
        runtime.SetConfig(config);
        QSignalSpy writes(&runtime, &AssistiveRuntime::NotesWriteFinished);
        runtime.NoteCapturedVideo(original);
        QTRY_COMPARE(writes.size(), 2);
        QVERIFY(writes.last().at(1).toString().isEmpty());
        const QString first = ReadAll(marker);
        QVERIFY(!first.isEmpty());
        runtime.NoteCapturedVideo(original);
        QTRY_COMPARE(writes.size(), 3);
        QVERIFY(writes.last().at(1).toString().isEmpty());
        QCOMPARE(ReadAll(marker), first);
        QVERIFY(writeMedia("changed recording")); // Identical length, different bytes.
        runtime.NoteCapturedVideo(original);
        QTRY_COMPARE(writes.size(), 4);
        QVERIFY(!writes.last().at(1).toString().isEmpty());
        QCOMPARE(ReadAll(marker), first); // Never replace an immutable identity marker.
    }

    void appendedDocumentStaysStyledEscapedAndDeduplicated()
    {
        // Finalized media identity is computed by the storage worker, so the
        // fixture must provide actual bytes rather than filename-only media.
        for (const auto* name : {"VID_original.mp4", "VID2_original.mp4"}) {
            QFile recording(notesDirectory_ + QLatin1Char('/') + QLatin1String(name));
            QVERIFY(recording.open(QIODevice::WriteOnly));
            QCOMPARE(recording.write("fixture recording"), 17);
        }
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
        QCOMPARE(content.count(QStringLiteral("data-recording-identity=\"")), 2);
        QVERIFY(!content.contains(QStringLiteral("__OKUFLOW_MEDIA_IDENTITY__")));
        const auto firstIdentity = QJsonDocument::fromJson(ReadAll(notesDirectory_ +
            QStringLiteral("/VID_original.mp4.okuflow-media.json")).toUtf8()).object();
        const auto secondIdentity = QJsonDocument::fromJson(ReadAll(notesDirectory_ +
            QStringLiteral("/VID2_original.mp4.okuflow-media.json")).toUtf8()).object();
        QCOMPARE(firstIdentity.value(QStringLiteral("size")).toInt(), 17);
        QCOMPARE(firstIdentity.value(QStringLiteral("fingerprint")).toString(),
            QStringLiteral("2e0fa70301bb1ee4a5875308ff4df614d95578a0d19cf1788dce6a0c11aa64ba"));
        QCOMPARE(firstIdentity.value(QStringLiteral("fingerprint")), secondIdentity.value(QStringLiteral("fingerprint")));
        QVERIFY(firstIdentity.value(QStringLiteral("mediaId")) != secondIdentity.value(QStringLiteral("mediaId")));
        QVERIFY(content.contains(QStringLiteral(".okuflow-transcript-")));
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
