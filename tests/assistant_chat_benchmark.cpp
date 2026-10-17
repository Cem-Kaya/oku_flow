// Opt-in offline CPU diagnostic. This executable always uses Qt offscreen and
// never constructs an application runtime, provider, camera or native window.
#include "okuflow/common/deadline_worker.hpp"
#include "okuflow/ui/assistant_chat_view.hpp"
#include <QApplication>
#include <QBuffer>
#include <QCommandLineParser>
#include <QColor>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QSaveFile>
#include <QScrollBar>
#include <QSysInfo>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

namespace {
class DiagnosticApplication final : public QApplication {
public:
    using QApplication::QApplication;
    bool traceEvents{};
    const char* phase{"initialization"};
    QJsonArray events;
    bool notify(QObject* receiver,QEvent* event) override {
        if(!traceEvents)return QApplication::notify(receiver,event);
        QElapsedTimer clock;clock.start();
        const auto eventType=event->type();
        const QString type=QString::fromLatin1(receiver->metaObject()->className());
        const QString name=receiver->objectName();
        const bool result=QApplication::notify(receiver,event);
        const qint64 elapsed=clock.nsecsElapsed()/1000;
        if(elapsed>=500 && events.size()<32)events.append(QJsonObject{
            {"type",static_cast<int>(eventType)},{"receiver_class",type},{"receiver_name",name},
            {"phase",phase},{"inclusive_us",elapsed}});
        return result;
    }
};
QJsonObject Distribution(std::vector<qint64> values)
{
    if (values.empty()) return {{"count", 0}};
    std::sort(values.begin(), values.end());
    const auto rank = [&](double p) {
        return values[static_cast<size_t>(std::ceil(p * values.size())) - 1];
    };
    return {{"count", static_cast<qint64>(values.size())},
        {"p50_us", rank(.50)}, {"p95_us", rank(.95)},
        {"p99_us", rank(.99)}, {"max_us", values.back()}};
}

QString Preview()
{
    QImage image(320, 180, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor(x * 255 / 320, y * 255 / 180, 90));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", 75)) return {};
    return QString::fromLatin1(bytes.toBase64());
}

QJsonObject Snapshot(int count, int characters, bool previews)
{
    QJsonArray messages;
    const QString preview = previews ? Preview() : QString{};
    const QString phrase = QStringLiteral("Repeated phrase: read the board carefully. ");
    QString text;
    while (text.size() < characters) text += phrase;
    text.truncate(characters);
    for (int i = 0; i < count; ++i) {
        QJsonObject message{{"id", QStringLiteral("message-%1").arg(i)},
            {"role", i % 2 == 0 ? "user" : "assistant"}, {"text", text},
            {"status", i == count - 1 ? "streaming" : "completed"},
            {"complete", i != count - 1}, {"updatedAt", 1700000000000LL}};
        // VisibleChatSession retains at most eight thumbnails.
        if (i % 2 == 0 && i >= count - 16 && !preview.isEmpty())
            message["imagePreviewJpeg"] = preview;
        messages.append(message);
    }
    return {{"conversationId", "offline-benchmark"}, {"messages", messages},
        {"busy", true}, {"draft", ""}, {"attachCurrentView", false}};
}

void Delta(QJsonObject& snapshot)
{
    auto messages = snapshot.value("messages").toArray();
    auto last = messages.last().toObject();
    last["text"] = last.value("text").toString() + QStringLiteral(" next word");
    messages[messages.size() - 1] = last;
    snapshot["messages"] = messages;
}

QJsonArray ControlledWaits(int samples, bool& valid)
{
    QJsonArray results;
    for (const int delay : {0, 5, 20, 50}) {
        okuflow::DeadlineWorker worker;
        std::vector<qint64> waits;
        for (int i = 0; i < std::min(samples, 40); ++i) {
            QElapsedTimer clock;
            clock.start();
            const auto result = worker.Execute([delay] {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }, std::chrono::milliseconds(500));
            waits.push_back(clock.nsecsElapsed() / 1000);
            valid &= result == okuflow::DeadlineWorker::Result::Completed;
        }
        results.append(QJsonObject{{"injected_delay_ms", delay},
            {"caller_wait", Distribution(std::move(waits))}});
    }
    return results;
}
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    DiagnosticApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Offline chat render and controlled DeadlineWorker CPU diagnostic"));
    parser.addHelpOption();
    parser.addOption({"output", "JSON evidence file (required).", "path"});
    parser.addOption({"samples", "Measured deltas, excluding ten warmups (20..1000).", "count", "200"});
    parser.addOption({"messages", "Even message count (4..128).", "count", "40"});
    parser.addOption({"characters", "Initial characters per message (128..2048).", "count", "1024"});
    parser.addOption({"views", "Production views updated per delta (1..2).", "count", "2"});
    parser.addOption({"previews", "Include up to eight original synthetic JPEG previews."});
    parser.addOption({"trace-events", "Bounded slow Qt event trace; diagnostic overhead makes this a separate workload."});
    parser.process(app);
    const auto integer = [&](const char* name, int minimum, int maximum) {
        bool ok = false;
        const int value = parser.value(QString::fromLatin1(name)).toInt(&ok);
        return ok && value >= minimum && value <= maximum ? value : -1;
    };
    const int samples = integer("samples", 20, 1000);
    const int messages = integer("messages", 4, 128);
    const int characters = integer("characters", 128, 2048);
    const int viewCount = integer("views", 1, 2);
    if (parser.value("output").isEmpty() || samples < 0 || messages < 0 ||
        characters < 0 || viewCount < 0 || messages % 2 ||
        messages * characters + (samples + 10) * 10 > 256 * 1024) {
        qCritical("Invalid arguments or snapshot exceeds the production 256 Ki-character budget.");
        return 2;
    }
    const QString fontPath = qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/segoeui.ttf";
    if (QFontDatabase::addApplicationFont(fontPath) < 0) {
        qCritical("Could not register installed Segoe UI for the Windows offscreen backend.");
        return 2;
    }
    QApplication::setFont(QFont(QStringLiteral("Segoe UI"), 12));
    std::vector<std::unique_ptr<okuflow::AssistantChatView>> views;
    auto snapshot = Snapshot(messages, characters, parser.isSet("previews"));
    for (int i = 0; i < viewCount; ++i) {
        auto view = std::make_unique<okuflow::AssistantChatView>();
        view->resize(i == 0 ? QSize(900, 720) : QSize(430, 720));
        view->show(); // The platform is unconditionally offscreen.
        view->SetChatSnapshot(snapshot);
        views.push_back(std::move(view));
    }
    QApplication::processEvents();
    for (auto& view : views) {
        auto* transcript = view->transcript();
        QTextCursor selected(transcript->document());
        selected.setPosition(transcript->document()->characterCount() / 3);
        selected.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, 24);
        selected.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, 24);
        transcript->setTextCursor(selected); // Reverse selection, away from the streaming message.
        transcript->verticalScrollBar()->setValue(transcript->verticalScrollBar()->maximum() / 2);
    }
    QJsonArray rows;
    std::vector<qint64> snapshotTimes, paintTimes, queueTimes, processTimes;
    int selectionFailures = 0;
    int maximumDisplacement = 0;
    for (int sample = -10; sample < samples; ++sample) {
        app.events=QJsonArray{};app.traceEvents=parser.isSet("trace-events");
        Delta(snapshot);
        struct Anchor { int anchor, position, topPosition, topY; };
        std::vector<Anchor> anchors;
        for (auto& view : views) {
            auto* transcript = view->transcript();
            const auto cursor = transcript->textCursor();
            const auto top = transcript->cursorForPosition(QPoint(0, 0));
            anchors.push_back({cursor.anchor(), cursor.position(), top.position(), transcript->cursorRect(top).top()});
        }
        QElapsedTimer clock;
        clock.start();
        qint64 queueUs = -1;
        QMetaObject::invokeMethod(&app, [&] { queueUs = clock.nsecsElapsed() / 1000; }, Qt::QueuedConnection);
        app.phase="snapshot";
        for (auto& view : views) view->SetChatSnapshot(snapshot);
        const qint64 snapshotUs = clock.nsecsElapsed() / 1000;
        app.phase="offscreen-paint";
        for (auto& view : views) {
            QImage target(view->size(), QImage::Format_ARGB32_Premultiplied);
            target.fill(Qt::transparent);
            view->render(&target);
        }
        const qint64 paintUs = clock.nsecsElapsed() / 1000;
        app.phase="process-events";
        QApplication::processEvents();
        const qint64 processUs=clock.nsecsElapsed()/1000-paintUs;
        if (queueUs < 0) return 3;
        int displacement = 0;
        for (int i = 0; i < viewCount; ++i) {
            auto* transcript = views[i]->transcript();
            const auto cursor = transcript->textCursor();
            selectionFailures += cursor.anchor() != anchors[i].anchor || cursor.position() != anchors[i].position;
            QTextCursor top(transcript->document());
            top.setPosition(anchors[i].topPosition);
            displacement = std::max(displacement, std::abs(transcript->cursorRect(top).top() - anchors[i].topY));
        }
        maximumDisplacement = std::max(maximumDisplacement, displacement);
        if (sample >= 0) {
            snapshotTimes.push_back(snapshotUs); paintTimes.push_back(paintUs); queueTimes.push_back(queueUs);processTimes.push_back(processUs);
            rows.append(QJsonObject{{"delta", sample}, {"set_snapshot_us", snapshotUs},
                {"snapshot_and_offscreen_paint_us", paintUs}, {"queued_probe_delay_us", queueUs},
                {"process_events_us",processUs},{"slow_events",app.events},
                {"reading_anchor_displacement_px", displacement}});
        }
    }
    app.traceEvents=false;
    bool valid = selectionFailures == 0 && maximumDisplacement <= 1;
    const auto controlled = ControlledWaits(samples, valid);
    const QJsonObject report{{"schema", 1}, {"scope", "synthetic-offline-cpu; no-native-input-or-present"},
        {"qt", qVersion()}, {"architecture", QSysInfo::buildCpuArchitecture()},
        {"platform", QGuiApplication::platformName()}, {"warmup_deltas", 10},
        {"messages", messages}, {"initial_characters_per_message", characters},
        {"views", viewCount}, {"previews", parser.isSet("previews")},
        {"queue_policy", "one queued probe per delta; processEvents between deltas"},
        {"event_trace_enabled",parser.isSet("trace-events")},
        {"set_snapshot", Distribution(std::move(snapshotTimes))},
        {"snapshot_and_offscreen_paint", Distribution(std::move(paintTimes))},
        {"queued_probe_delay", Distribution(std::move(queueTimes))},
        {"process_events", Distribution(std::move(processTimes))},
        {"selection_failures", selectionFailures}, {"max_reading_anchor_displacement_px", maximumDisplacement},
        {"controlled_deadline_worker_waits_not_maxine", controlled}, {"valid", valid}, {"samples", rows}};
    QSaveFile output(parser.value("output"));
    const auto bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) return 4;
    return valid ? 0 : 1;
}
