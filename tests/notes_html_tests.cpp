// Regression tests for the appended lecture-notes HTML (plan 36 redesign):
// head written once with theme toggle and viewer script, every entry a
// self-contained appended block, transcript as escaped line-by-line feed,
// collapsible note sections, duplicate-final dedupe. No network, no media
// files needed. Set OPENZOOM_NOTES_TEST_DIR to keep the generated file for
// visual inspection.

#include <QTemporaryDir>
#include <QtTest>
#include <QThreadPool>
#include <QSemaphore>
#include <QScopeGuard>

#include "openzoom/common/assistive_runtime.hpp"
#include "openzoom/common/transcript.hpp"

using namespace openzoom;

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

    static TranscriptSegment MakeSegment(quint64 sequence, const QString& text,
                                         qint64 offsetSeconds)
    {
        TranscriptSegment segment;
        segment.recordingSessionId = QStringLiteral("11112222-3333-4444");
        segment.sequence = sequence;
        segment.languageCode = QStringLiteral("en");
        segment.text = text;
        segment.observedCaptureClock100ns = offsetSeconds * 10000000LL;
        segment.approximateOffset100ns = offsetSeconds * 10000000LL;
        return segment;
    }

    static QString ReadAll(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }

    static bool content_hasBothSessions(const QString& path)
    {
        const QString content = ReadAll(path);
        return content.contains(
                   QStringLiteral("data-recording-session=\"11112222-3333-4444\"")) &&
               content.contains(
                   QStringLiteral("data-recording-session=\"99998888-7777-6666\""));
    }

private slots:
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
        const QByteArray oldScenario = qgetenv("OPENZOOM_FAKE_CODEX_SCENARIO");
        const auto restore = qScopeGuard([oldScenario]() {
            if (oldScenario.isNull()) qunsetenv("OPENZOOM_FAKE_CODEX_SCENARIO");
            else qputenv("OPENZOOM_FAKE_CODEX_SCENARIO", oldScenario);
        });
        qputenv("OPENZOOM_FAKE_CODEX_SCENARIO",
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
        const QByteArray keepDirectory = qgetenv("OPENZOOM_NOTES_TEST_DIR");
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
        QVERIFY(runtime.NoteTranscriptSegment(MakeSegment(1, QStringLiteral("first entry"), 1)));
        QVERIFY(runtime.HasPendingNotesWrites());
        const QString firstPath = runtime.notesFilePath();
        QVERIFY(!QFile::exists(firstPath));
        QVERIFY(runtime.NoteTranscriptSegment(MakeSegment(2, QStringLiteral("second entry"), 2)));
        config.notesDirectory += QStringLiteral("/next");
        runtime.SetConfig(config);
        QVERIFY(runtime.NoteTranscriptSegment(MakeSegment(1, QStringLiteral("new target"), 1)));
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
        const auto segment = MakeSegment(1, QStringLiteral("retry this final"), 1);
        QVERIFY(runtime.NoteTranscriptSegment(segment));
        QTRY_COMPARE(writes.size(), 2);
        for (const auto& write : writes) QVERIFY(!write.at(1).toString().isEmpty());
        QVERIFY(QFile::remove(blocker));
        writes.clear();
        QVERIFY(runtime.NoteTranscriptSegment(segment));
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
                QVERIFY(runtime.NoteTranscriptSegment(MakeSegment(i, QStringLiteral("accepted"), i)));
            QVERIFY(!runtime.NoteTranscriptSegment(MakeSegment(128, QStringLiteral("rejected"), 128)));
            QCOMPARE(writes.size(), 1);
            QVERIFY(!writes.front().at(1).toString().isEmpty());
            path = runtime.notesFilePath();
            // Destruction must not wait for the blocked notes worker.
        }
        release.release();
        QTRY_COMPARE(ReadAll(path).count(QStringLiteral("<span class=\"tr-text\">accepted")), 127);
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

        // Transcript lines, including an injection attempt that must stay
        // quoted text, a truncated final, a duplicate sequence, and a gap.
        QVERIFY(runtime.NoteTranscriptSegment(MakeSegment(
            1, QStringLiteral("Hello class, welcome back."), 5)));
        QVERIFY(runtime.NoteTranscriptSegment(MakeSegment(
            2,
            QStringLiteral("<script>alert('x')</script> \"quotes\" & <b>tags</b>"),
            12)));
        QVERIFY(runtime.NoteTranscriptSegment(MakeSegment(2,
            QStringLiteral("duplicate must not append"), 12)));
        TranscriptSegment truncated =
            MakeSegment(3, QStringLiteral("cut off mid"), 65);
        truncated.truncated = true;
        QVERIFY(runtime.NoteTranscriptSegment(truncated));
        QVERIFY(runtime.NoteTranscriptGap(QStringLiteral("11112222-3333-4444")));

        // A plain annotated-view section.
        runtime.NoteAnnotationSnapshot(
            notesDirectory_ + QStringLiteral("/IMG_annotated.png"),
            QStringLiteral("Annotated view"));

        // A SECOND recording session (different id) with media between: its
        // transcript lines are appended far from the first session's. The
        // viewer script must still gather every line into one block.
        TranscriptSegment second =
            MakeSegment(1, QStringLiteral("Back after a pause, second clip."), 3);
        second.recordingSessionId = QStringLiteral("99998888-7777-6666");
        QVERIFY(runtime.NoteTranscriptSegment(second));

        // Media from the second session, appended after its transcript.
        runtime.NoteCapturedVideoPair(
            notesDirectory_ + QStringLiteral("/VID2_original.mp4"),
            notesDirectory_ + QStringLiteral("/VID2_processed.mp4"));

        // Two recording sessions produced transcript lines that are
        // physically separated in the appended file; the head script keys
        // them by data-recording-session so they can be unified at load.
        QTRY_COMPARE(writes.size(), 9);
        for (const auto& write : writes) QVERIFY(write.at(1).toString().isEmpty());
        QVERIFY(content_hasBothSessions(runtime.notesFilePath()));

        const QString path = runtime.notesFilePath();
        QVERIFY(!path.isEmpty());
        const QString content = ReadAll(path);
        QVERIFY(!content.isEmpty());

        // Head: written once — theme toggle, expand/collapse controls, the
        // transcript label for the viewer script, and no external resource.
        QCOMPARE(content.count(QStringLiteral("id=\"btn-theme\"")), 1);
        QCOMPARE(content.count(QStringLiteral("id=\"btn-expand\"")), 1);
        QCOMPARE(content.count(QStringLiteral("id=\"btn-collapse\"")), 1);
        QVERIFY(content.contains(QStringLiteral("data-l10n-transcript=")));
        QVERIFY(content.contains(QStringLiteral("prefers-color-scheme")));
        QVERIFY(!content.contains(QStringLiteral("http://")));
        QVERIFY(!content.contains(QStringLiteral("https://")));

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
        QVERIFY(content.contains(QStringLiteral("[data-kind=\"talk\"]")));
        QVERIFY(content.contains(QStringLiteral("insertBefore(wrap, first)")));
        QVERIFY(content.contains(QStringLiteral("details[data-kind='video']")));
        QVERIFY(content.contains(QStringLiteral("figure.ai-shot")));
        // Conversation grouping: label, icon, and the grouping query.
        QVERIFY(content.contains(QStringLiteral("data-l10n-conversation=")));
        QVERIFY(content.contains(QStringLiteral("[data-kind=\"convo\"]")));
        QVERIFY(content.contains(
            QStringLiteral("details[data-conversation]")));

        // Transcript feed: compact lines, ids, session key, short time chip
        // plus the approximate long form in the tooltip.
        QVERIFY(content.contains(QStringLiteral(
            "id=\"transcript-11112222-1\"")));
        QVERIFY(content.contains(
            QStringLiteral("data-recording-session=\"11112222-3333-4444\"")));
        QVERIFY(content.contains(QStringLiteral(">0:05</time>")));
        QVERIFY(content.contains(QStringLiteral(">1:05</time>")));
        QVERIFY(content.contains(QStringLiteral("about 00:00:05")));

        // Escaping: the injection attempt is quoted text, never markup.
        QVERIFY(!content.contains(QStringLiteral("<script>alert")));
        QVERIFY(content.contains(QStringLiteral("&lt;script&gt;alert")));
        QVERIFY(content.contains(QStringLiteral("&amp; &lt;b&gt;")));

        // Duplicate sequence appended exactly once.
        QCOMPARE(content.count(QStringLiteral("id=\"transcript-11112222-2\"")), 1);
        QVERIFY(!content.contains(QStringLiteral("duplicate must not append")));

        // Truncation marker and gap line.
        QVERIFY(content.contains(QStringLiteral("(truncated)")));
        QVERIFY(content.contains(QStringLiteral("tr tr-gap")));

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
