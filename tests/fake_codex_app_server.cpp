// Fake Codex app-server for deterministic vision and Assistant protocol tests.
// Speaks JSONL on stdio and plays the scenario named by the
// OKUFLOW_FAKE_CODEX_SCENARIO environment variable. Never touches the
// network. Exits when stdin closes.

#include <QByteArray>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

QString gScenario;
QString gThreadId = QStringLiteral("thr_fake");
QJsonValue gHeldId(QJsonValue::Undefined);
QString gHeldMethod;
int gLifecycleOpenings = 0;
QJsonValue gHistoryId(QJsonValue::Undefined);
QString gHistoryThread;

void WriteObject(const QJsonObject& object)
{
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void WriteResult(const QJsonValue& id, const QJsonObject& result)
{
    WriteObject(QJsonObject{{QStringLiteral("id"), id},
                            {QStringLiteral("result"), result}});
}

void WriteError(const QJsonValue& id, int code, const QString& message)
{
    WriteObject(QJsonObject{{QStringLiteral("id"), id},
                            {QStringLiteral("error"),
                             QJsonObject{{QStringLiteral("code"), code},
                                         {QStringLiteral("message"), message}}}});
}

void WriteNotification(const QString& method, QJsonObject params)
{
    params.insert(QStringLiteral("threadId"), gThreadId);
    WriteObject(QJsonObject{{QStringLiteral("method"), method},
                            {QStringLiteral("params"), params}});
}

void WriteHistory(const QJsonValue& id, const QString& thread)
{
    const QJsonObject item{{QStringLiteral("type"), QStringLiteral("agentMessage")},
                           {QStringLiteral("text"), thread + QStringLiteral(" history")}};
    const QJsonObject turn{{QStringLiteral("items"), QJsonArray{item}}};
    const QJsonObject history{{QStringLiteral("id"), thread},
                             {QStringLiteral("turns"), QJsonArray{turn}}};
    WriteResult(id, QJsonObject{{QStringLiteral("thread"), history}});
}

void ReleaseHistory()
{
    if (gHistoryId.isUndefined()) return;
    if (gScenario == QStringLiteral("vision-lifecycle-history-error")) {
        WriteError(gHistoryId, -32000, QStringLiteral("fake history read failure"));
    } else {
        WriteHistory(gHistoryId, gHistoryThread);
    }
    gHistoryId = QJsonValue(QJsonValue::Undefined);
    WriteNotification(QStringLiteral("warning"), QJsonObject{{QStringLiteral("message"), QStringLiteral("released history")}});
}

void HandleMessage(const QJsonObject& message)
{
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();

    if (gScenario.startsWith(QStringLiteral("vision-lifecycle"))) {
        if (gScenario.startsWith(QStringLiteral("vision-lifecycle-history")) && method == QStringLiteral("thread/read")) {
            const QString thread = params.value(QStringLiteral("threadId")).toString();
            if (thread == QStringLiteral("A")) {
                gHistoryId = id;
                gHistoryThread = thread;
                WriteNotification(QStringLiteral("warning"), QJsonObject{{QStringLiteral("message"), QStringLiteral("held history")}});
            } else {
                if (thread == QStringLiteral("error")) {
                    WriteError(id, -32000, QStringLiteral("fake history read failure"));
                } else if (thread == QStringLiteral("empty-error")) {
                    WriteError(id, -32000, {});
                } else {
                    WriteHistory(id, thread);
                }
                ReleaseHistory();
            }
            return;
        }
        if (method == QStringLiteral("initialize")) {
            if (gScenario.endsWith(QStringLiteral("init-timeout"))) return;
            if (gScenario.endsWith(QStringLiteral("init-error"))) {
                WriteError(id, -32000, QStringLiteral("fake initialization failure"));
                return;
            }
        }
        if (method == QStringLiteral("thread/start") || method == QStringLiteral("thread/resume")) {
            ++gLifecycleOpenings;
            const QString holdMethod = gScenario.endsWith(QStringLiteral("resume"))
                                           ? QStringLiteral("thread/resume") : QStringLiteral("thread/start");
            const bool holdOpening = gScenario == QStringLiteral("vision-lifecycle-hold-start") ||
                                     gScenario == QStringLiteral("vision-lifecycle-hold-resume") ||
                                     gScenario == QStringLiteral("vision-lifecycle-hold-duplicate");
            if (holdOpening && method == holdMethod && gHeldId.isUndefined()) {
                gHeldId = id;
                gHeldMethod = method;
                WriteNotification(QStringLiteral("warning"), QJsonObject{{QStringLiteral("message"), QStringLiteral("held opening")}});
                return;
            }
            if (!gHeldId.isUndefined() && gHeldMethod != QStringLiteral("turn/start")) {
                WriteResult(gHeldId, QJsonObject{{QStringLiteral("thread"), QJsonObject{{QStringLiteral("id"), gScenario.endsWith(QStringLiteral("duplicate")) ? QStringLiteral("thr_new") : QStringLiteral("thr_old")}}}});
            }
            gThreadId = params.value(QStringLiteral("threadId")).toString(QStringLiteral("thr_new"));
            WriteResult(id, QJsonObject{{QStringLiteral("thread"), QJsonObject{{QStringLiteral("id"), gThreadId}}}});
            return;
        }
        if (method == QStringLiteral("turn/start")) {
            if (gScenario.endsWith(QStringLiteral("hold-turn")) && gHeldId.isUndefined()) {
                gHeldId = id;
                gHeldMethod = method;
                WriteNotification(QStringLiteral("warning"), QJsonObject{{QStringLiteral("message"), QStringLiteral("held turn")}});
                return;
            }
            const QJsonArray input = params.value(QStringLiteral("input")).toArray();
            const QString prompt = input.isEmpty() ? QString() : input.first().toObject().value(QStringLiteral("text")).toString();
            const QString thread = params.value(QStringLiteral("threadId")).toString();
            if ((gScenario.endsWith(QStringLiteral("duplicate")) && gLifecycleOpenings != 1) ||
                thread == QStringLiteral("thr_old") || input.isEmpty() ||
                (input.size() > 1 && !QFileInfo(input.at(1).toObject().value(QStringLiteral("path")).toString()).isFile())) {
                WriteError(id, -32602, QStringLiteral("stale opening consumed request"));
                return;
            }
            gThreadId = thread;
            WriteResult(id, QJsonObject{{QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), QStringLiteral("turn_new")}}}});
            // Old non-delta traffic must not finish or contaminate this turn.
            WriteNotification(QStringLiteral("item/completed"), QJsonObject{{QStringLiteral("turnId"), QStringLiteral("turn_old")}, {QStringLiteral("item"), QJsonObject{{QStringLiteral("type"), QStringLiteral("agentMessage")}, {QStringLiteral("text"), QStringLiteral("wrong answer")}}}});
            WriteNotification(QStringLiteral("error"), QJsonObject{{QStringLiteral("turnId"), QStringLiteral("turn_old")}});
            WriteNotification(QStringLiteral("item/agentMessage/delta"), QJsonObject{{QStringLiteral("turnId"), QStringLiteral("turn_new")}, {QStringLiteral("delta"), prompt}});
            if (gScenario.startsWith(QStringLiteral("vision-lifecycle-history"))) ReleaseHistory();
            WriteNotification(QStringLiteral("turn/completed"), QJsonObject{{QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), QStringLiteral("turn_new")}, {QStringLiteral("status"), QStringLiteral("completed")}}}});
            return;
        }
        if (method == QStringLiteral("account/read") && !gHeldId.isUndefined()) {
            if (gHeldMethod == QStringLiteral("turn/start")) {
                WriteResult(gHeldId, QJsonObject{{QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), QStringLiteral("turn_old")}}}});
            } else {
                WriteResult(gHeldId, QJsonObject{{QStringLiteral("thread"), QJsonObject{{QStringLiteral("id"), gScenario.endsWith(QStringLiteral("duplicate")) ? QStringLiteral("thr_new") : QStringLiteral("thr_old")}}}});
            }
        }
        if (method == QStringLiteral("turn/interrupt")) {
            WriteResult(id, {});
            WriteNotification(QStringLiteral("turn/completed"), QJsonObject{{QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), params.value(QStringLiteral("turnId"))}, {QStringLiteral("status"), QStringLiteral("interrupted")}}}});
            return;
        }
        if (method == QStringLiteral("model/list")) { WriteResult(id, {}); return; }
    }
    if (gScenario.startsWith(QStringLiteral("vision-"))) {
        if (method == QStringLiteral("model/list")) {
            WriteResult(id, QJsonObject{{QStringLiteral("data"), QJsonArray{
                QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-6-luna")},
                            {QStringLiteral("inputModalities"), QJsonArray{QStringLiteral("text"), QStringLiteral("image")}},
                            {QStringLiteral("isDefault"), true}}}}});
            return;
        }
        if (method == QStringLiteral("thread/start")) {
            if (!params.value(QStringLiteral("ephemeral")).toBool() ||
                params.value(QStringLiteral("approvalPolicy")).toString() != QStringLiteral("never") ||
                  params.value(QStringLiteral("sandbox")).toString() != QStringLiteral("read-only") ||
                  !params.value(QStringLiteral("developerInstructions")).toString().contains(
                      QStringLiteral("For verbatim reading or transcription requests, preserve the source wording and language")) ||
                  params.value(QStringLiteral("model")).toString() != QStringLiteral("gpt-6-luna")) {
                WriteError(id, -32602, QStringLiteral("incorrect vision thread policy or model"));
            } else {
                WriteResult(id, QJsonObject{{QStringLiteral("thread"),
                    QJsonObject{{QStringLiteral("id"), gThreadId}}}});
            }
            return;
        }
        if (method == QStringLiteral("turn/start")) {
            const QJsonArray input = params.value(QStringLiteral("input")).toArray();
            const QString prompt = input.isEmpty() ? QString() : input.first().toObject().value(QStringLiteral("text")).toString();
            const QJsonObject image = input.size() > 1 ? input.at(1).toObject() : QJsonObject();
            const QJsonObject sandbox = params.value(QStringLiteral("sandboxPolicy")).toObject();
            const bool reading = gScenario.startsWith(QStringLiteral("vision-read-"));
            const bool turkish = !reading && gScenario.endsWith(QStringLiteral("-tr"));
            const bool german = !reading && gScenario.endsWith(QStringLiteral("-de"));
            if (prompt.contains(QStringLiteral("Transcribe all readable text")) != reading ||
                  (reading && !prompt.contains(QStringLiteral("Do not translate or summarize"))) ||
                  prompt.contains(QStringLiteral("Respond in Turkish.")) != turkish ||
                  prompt.contains(QStringLiteral("Respond in German.")) != german ||
                image.value(QStringLiteral("type")).toString() != QStringLiteral("localImage") ||
                !QFileInfo(image.value(QStringLiteral("path")).toString()).isFile() ||
                params.value(QStringLiteral("model")).toString() != QStringLiteral("gpt-6-luna") ||
                params.value(QStringLiteral("effort")).toString() != QStringLiteral("low") ||
                sandbox.value(QStringLiteral("type")).toString() != QStringLiteral("readOnly") ||
                sandbox.value(QStringLiteral("networkAccess")).toBool()) {
                WriteError(id, -32602, QStringLiteral("incorrect reading prompt, image, model, or permissions"));
                return;
            }
            const QString text = QStringLiteral("Visible line 123. ").repeated(80);
            const QString turnId = QStringLiteral("turn_vision");
            WriteResult(id, QJsonObject{{QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), turnId}}}});
            WriteNotification(QStringLiteral("item/agentMessage/delta"),
                QJsonObject{{QStringLiteral("turnId"), turnId}, {QStringLiteral("delta"), text}});
            WriteNotification(QStringLiteral("turn/completed"),
                QJsonObject{{QStringLiteral("turn"), QJsonObject{
                    {QStringLiteral("id"), turnId}, {QStringLiteral("status"), QStringLiteral("completed")}}}});
            return;
        }
    }

    if (method == QStringLiteral("initialize")) {
        WriteResult(id, QJsonObject{{QStringLiteral("userAgent"), QStringLiteral("fake-codex")}});
        return;
    }
    if (method == QStringLiteral("initialized")) {
        return;
    }
    if (method == QStringLiteral("account/read")) {
        const QString type = gScenario == QStringLiteral("notchatgpt")
                                 ? QStringLiteral("apikey")
                                 : QStringLiteral("chatgpt");
        WriteResult(id, QJsonObject{{QStringLiteral("account"),
                                     QJsonObject{{QStringLiteral("type"), type}}}});
        return;
    }
    if (method == QStringLiteral("account/rateLimits/read")) {
        WriteResult(id, QJsonObject{{QStringLiteral("rateLimits"),
                                     QJsonObject{{QStringLiteral("primary"),
                                                  QJsonObject{{QStringLiteral("usedPercent"), 39}}}}}});
        return;
    }
    if (!id.isUndefined()) {
        WriteError(id, -32601, QStringLiteral("unknown method"));
    }
}

} // namespace

int main()
{
    const char* scenario = std::getenv("OKUFLOW_FAKE_CODEX_SCENARIO");
    gScenario = scenario != nullptr ? QString::fromUtf8(scenario) : QStringLiteral("vision-happy");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        const QJsonDocument document =
            QJsonDocument::fromJson(QByteArray::fromStdString(line));
        if (!document.isObject()) {
            continue;
        }
        HandleMessage(document.object());
    }
    return 0;
}
