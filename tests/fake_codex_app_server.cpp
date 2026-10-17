// Fake Codex app-server for deterministic vision and Assistant protocol tests.
// Speaks JSONL on stdio and plays the scenario named by the
// OKUFLOW_FAKE_CODEX_SCENARIO environment variable. Never touches the
// network. Exits when stdin closes.

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

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
int gNextThread = 0;
int gTurnCount = 0;
QStringList gKeptThreads;
bool gCodingMask{};
QString gCodingCwd;

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

// "explain-threads": a small thread store. Ephemeral threads are named tmp_N,
// stored ones keep_N; only stored threads created by this process resume.
// Answers name their thread, request kind, and whether the prompt continued
// an Explain conversation, e.g. "keep_1 explain continued".
bool HandleExplainThreads(const QJsonValue& id, const QString& method, const QJsonObject& params)
{
    const bool restrictedOpening =
        params.value(QStringLiteral("approvalPolicy")).toString() == QStringLiteral("never") &&
        params.value(QStringLiteral("sandbox")).toString() == QStringLiteral("read-only") &&
        params.value(QStringLiteral("developerInstructions")).toString().contains(
            QStringLiteral("Never execute commands or edit files."));
    if (method == QStringLiteral("thread/start")) {
        if (!restrictedOpening) {
            WriteError(id, -32602, QStringLiteral("camera thread lost the restricted vision policy"));
            return true;
        }
        const bool ephemeral = params.value(QStringLiteral("ephemeral")).toBool();
        const QString thread = QStringLiteral("%1_%2")
                                   .arg(ephemeral ? QStringLiteral("tmp") : QStringLiteral("keep"))
                                   .arg(++gNextThread);
        if (!ephemeral) gKeptThreads.append(thread);
        WriteResult(id, QJsonObject{{QStringLiteral("thread"),
                                     QJsonObject{{QStringLiteral("id"), thread},
                                                 {QStringLiteral("createdAt"), 1700000000 + gNextThread}}}});
        return true;
    }
    if (method == QStringLiteral("thread/resume")) {
        const QString thread = params.value(QStringLiteral("threadId")).toString();
        if (!restrictedOpening) {
            WriteError(id, -32602, QStringLiteral("camera thread lost the restricted vision policy"));
        } else if (!gKeptThreads.contains(thread)) {
            WriteError(id, -32600, QStringLiteral("thread not found: %1").arg(thread));
        } else {
            WriteResult(id, QJsonObject{{QStringLiteral("thread"), QJsonObject{{QStringLiteral("id"), thread}}}});
        }
        return true;
    }
    if (method == QStringLiteral("thread/delete")) {
        gKeptThreads.removeAll(params.value(QStringLiteral("threadId")).toString());
        WriteResult(id, {});
        return true;
    }
    if (method == QStringLiteral("turn/start")) {
        const QString thread = params.value(QStringLiteral("threadId")).toString();
        const QJsonArray input = params.value(QStringLiteral("input")).toArray();
        const QString prompt = input.isEmpty() ? QString() : input.first().toObject().value(QStringLiteral("text")).toString();
        const QJsonObject sandbox = params.value(QStringLiteral("sandboxPolicy")).toObject();
        if (sandbox.value(QStringLiteral("type")).toString() != QStringLiteral("readOnly") ||
            sandbox.value(QStringLiteral("networkAccess")).toBool() || input.isEmpty() ||
            (input.size() == 1 && !prompt.contains(QStringLiteral("Text-only follow-up"))) ||
            (input.size() > 1 && input.at(1).toObject().value(QStringLiteral("type")).toString() != QStringLiteral("localImage"))) {
            WriteError(id, -32602, QStringLiteral("camera turn lost its image or restricted permissions"));
            return true;
        }
        const bool reading = prompt.contains(QStringLiteral("Transcribe all readable text"));
        const bool continued = prompt.contains(QStringLiteral("ongoing Explain conversation"));
        const QString turnId = QStringLiteral("turn_%1").arg(++gTurnCount);
        const QString text = QStringLiteral("%1 %2 %3")
                                 .arg(thread,
                                      reading ? QStringLiteral("read") : QStringLiteral("explain"),
                                      continued ? QStringLiteral("continued") : QStringLiteral("first"));
        gThreadId = thread;
        WriteResult(id, QJsonObject{{QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), turnId}}}});
        WriteNotification(QStringLiteral("item/agentMessage/delta"),
                          QJsonObject{{QStringLiteral("turnId"), turnId}, {QStringLiteral("delta"), text}});
        WriteNotification(QStringLiteral("turn/completed"),
                          QJsonObject{{QStringLiteral("turn"), QJsonObject{
                              {QStringLiteral("id"), turnId}, {QStringLiteral("status"), QStringLiteral("completed")}}}});
        return true;
    }
    if (method == QStringLiteral("model/list")) {
        WriteResult(id, {});
        return true;
    }
    return false;
}
bool HandlePermissionMask(const QJsonValue& id,const QString& method,const QJsonObject& params)
{
    if (method == "thread/start") {
        gCodingMask = params.value("sandbox").toString() == "workspace-write";
        gCodingCwd = params.value("cwd").toString();
        const auto instructions = params.value("developerInstructions").toString();
        if (params.value("approvalPolicy").toString() != "never" || params.value("ephemeral").toBool() ||
            (gCodingMask ? !instructions.contains(gCodingCwd) || !QFileInfo(gCodingCwd).isDir()
                : params.value("sandbox").toString() != "read-only" || !instructions.contains("Never execute commands or edit files."))) {
            WriteError(id,-32602,"incorrect admitted permission mask"); return true;
        }
        gThreadId = QStringLiteral("%1_%2").arg(gCodingMask?"coding":"assistant").arg(++gNextThread);
        WriteResult(id,{{"thread",QJsonObject{{"id",gThreadId}}}}); return true;
    }
    if (method == "turn/start") {
        const auto policy = params.value("sandboxPolicy").toObject();
        const auto roots = policy.value("writableRoots").toArray();
        if (policy.value("networkAccess").toBool() || params.value("cwd").toString() != gCodingCwd ||
            policy.value("type").toString() != (gCodingMask?"workspaceWrite":"readOnly") ||
            (gCodingMask && (roots.size()!=1 || roots[0].toString()!=gCodingCwd))) {
            WriteError(id,-32602,"turn widened or lost its permission mask"); return true;
        }
        const auto turn = QStringLiteral("mask_%1").arg(++gTurnCount);
        WriteResult(id,{{"turn",QJsonObject{{"id",turn}}}});
        WriteNotification("item/agentMessage/delta",{{"turnId",turn},{"delta","Fixture permission mask accepted"}});
        WriteNotification("turn/completed",{{"turn",QJsonObject{{"id",turn},{"status","completed"}}}}); return true;
    }
    return false;
}

void HandleMessage(const QJsonObject& message)
{
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QString method = message.value(QStringLiteral("method")).toString();
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();

    if (gScenario == QStringLiteral("permission-mask") && HandlePermissionMask(id,method,params)) return;
    if (gScenario == QStringLiteral("explain-threads") &&
        HandleExplainThreads(id, method, params)) {
        return;
    }

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
            // Read opens a temporary thread; Explain keeps its thread.
            if (params.value(QStringLiteral("ephemeral")).toBool() !=
                    gScenario.startsWith(QStringLiteral("vision-read-")) ||
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

int main(int argc,char** argv)
{
    QStringList arguments;for(int i=1;i<argc;++i)arguments.append(QString::fromLocal8Bit(argv[i]));
    if(arguments.value(0)=="--version") {puts("codex-cli 0.160.1");return 0;}
    if(arguments.value(0)=="features") {
        QFile profile(QStringLiteral(OKUFLOW_AGENT_PROFILE_FIXTURE));if(!profile.open(QIODevice::ReadOnly))return 1;
        for(const auto& row:QJsonDocument::fromJson(profile.readAll()).object().value("featureInventory").toArray()) {
            const auto parts=row.toString().split('|');const auto line=(parts[0]+" "+parts[1]+" true\n").toUtf8();
            fwrite(line.data(),1,line.size(),stdout);
        }
        return 0;
    }
    if(arguments.value(0)=="mcp") {puts("[{\"name\":\"fixture-private\"}]");return 0;}
    const auto launchLog=qgetenv("OKUFLOW_FAKE_CODEX_LAUNCH_LOG");
    if(!launchLog.isEmpty()) {
        QFile file(QString::fromLocal8Bit(launchLog));if(!file.open(QIODevice::WriteOnly|QIODevice::Append))return 4;
        file.write(QJsonDocument(QJsonArray::fromStringList(arguments)).toJson(QJsonDocument::Compact)+"\n");
    }

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
