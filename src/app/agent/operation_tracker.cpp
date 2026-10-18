#include "okuflow/app/agent/operation_tracker.hpp"
#include <chrono>
#include <cmath>
#include <QUuid>
#include <QRegularExpression>

namespace okuflow::agent {
namespace {
bool Terminal(OperationState state) { return state == OperationState::Committed || state == OperationState::Completed
    || state == OperationState::FirstFrame || state == OperationState::Failed; }
bool Allowed(OperationKind kind, OperationState from, OperationState to) {
    if (Terminal(from)) return false;
    if (to == OperationState::Failed) return true;
    if (kind == OperationKind::Photo) return (from == OperationState::Queued && to == OperationState::Readback)
        || ((from == OperationState::Queued || from == OperationState::Readback) && to == OperationState::Writing)
        || (from == OperationState::Writing && to == OperationState::Committed);
    if (kind == OperationKind::Recording) return (from == OperationState::Starting && (to == OperationState::Recording || to == OperationState::Stopping))
        || (from == OperationState::Recording && to == OperationState::Stopping)
        || (from == OperationState::Stopping && to == OperationState::Completed);
    if(kind==OperationKind::Video)return (from==OperationState::Requested && to==OperationState::Starting)
        || (from==OperationState::Starting && to==OperationState::Completed);
    return (from == OperationState::Requested && to == OperationState::Stopping)
        || ((from == OperationState::Requested || from == OperationState::Stopping) && to == OperationState::Starting)
        || (from == OperationState::Starting && to == OperationState::FirstFrame);
}
}
QString OperationStateName(OperationState state) {
    switch (state) {
    case OperationState::Queued:return "queued"; case OperationState::Readback:return "readback";
    case OperationState::Writing:return "writing"; case OperationState::Committed:return "committed";
    case OperationState::Starting:return "starting"; case OperationState::Recording:return "recording";
    case OperationState::Stopping:return "stopping"; case OperationState::Completed:return "completed";
    case OperationState::Requested:return "requested"; case OperationState::FirstFrame:return "first_frame";
    case OperationState::Failed:return "failed";
    }
    return "failed";
}
OperationTracker::OperationTracker(std::function<qint64()> clock) : clock_(std::move(clock)) {}
qint64 OperationTracker::Now() const {
    return clock_ ? clock_() : std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
QString OperationTracker::Create(OperationKind kind, const QString& owner, const QString& sessionId) {
    if (!HasCapacity()) return {};
    // Bound live/retained state too: a full tracker denies admission rather
    // than evicting an outcome that remains inside its retention period.
    const QString id = QUuid::createUuid().toString(QUuid::Id128);
    const auto state = kind == OperationKind::Photo ? OperationState::Queued
        : kind == OperationKind::Recording ? OperationState::Starting : OperationState::Requested;
    operations_.emplace(id,OperationRecord{id,owner,sessionId,kind,state,0,{}});
    return id;
}
bool OperationTracker::HasCapacity() { Prune(Now()); return operations_.size() < 256; }
bool OperationTracker::Advance(const QString& id, OperationState state, QJsonObject summary) {
    auto record = operations_.find(id);
    if (record == operations_.end()) return false;
    if (record->second.state == state) return true;
    if (!Allowed(record->second.kind,record->second.state,state)) return false;
    record->second.state = state;
    for (auto it = summary.begin(); it != summary.end(); ++it) record->second.summary[it.key()] = it.value();
    if (Terminal(state)) record->second.terminalAt = Now();
    return true;
}
bool OperationTracker::SetSession(const QString& id, const QString& sessionId) {
    const auto record = operations_.find(id);
    if (record == operations_.end() || !record->second.sessionId.isEmpty() || Terminal(record->second.state) || sessionId.isEmpty()) return false;
    record->second.sessionId = sessionId; return true;
}
std::optional<OperationRecord> OperationTracker::Get(const QString& id) const {
    const auto record = operations_.find(id);
    if (record == operations_.end()) return {};
    return record->second;
}
QJsonObject OperationTracker::SafeStatus(const QString& id) const {
    const auto record = Get(id);
    if (!record) return {{"ok",false},{"error",QJsonObject{{"code","not_found"},{"retryable",false}}}};
    QJsonObject dto{{"ok",true},{"operationId",record->id},{"state",OperationStateName(record->state)}};
    if (record->kind == OperationKind::Camera && record->summary["fallbackActive"].isBool())
        dto["fallbackActive"] = record->summary["fallbackActive"];
    if (record->kind == OperationKind::Camera && record->summary["fallbackRequested"].isBool())
        dto["fallbackRequested"] = record->summary["fallbackRequested"];
    if(record->kind==OperationKind::Camera) {
        for(const auto property:{"requested","applied","effective"}) {
            QJsonObject values;const auto fields=record->summary[property].toObject();
            for(const auto key:{"camera.selection","camera.format","camera.acceleration"}) {
                const auto value=fields[key];const auto text=value.toString();
                const bool valid=value.isNull() || (QString::fromLatin1(key)=="camera.selection"
                    ? QRegularExpression("^cam-[0-9a-f]{32}$").match(text).hasMatch()
                    : QString::fromLatin1(key)=="camera.format"
                        ? text=="automatic" || QRegularExpression("^fmt-[0-9a-f]{32}$").match(text).hasMatch()
                        : QStringList{"automatic","accelerated","compatibility"}.contains(text));
                if(fields.contains(key) && valid)values[key]=value;
            }
            if(!values.isEmpty())dto[property]=values;
        }
        if(record->summary["undoPolicy"]=="temporary-source-owner")dto["undoPolicy"]="temporary-source-owner";
        if(record->summary["savedStartupPreserved"]==true)dto["savedStartupPreserved"]=true;
    }
    if (!record->sessionId.isEmpty()) dto["sessionId"] = record->sessionId;
    if(record->kind==OperationKind::Video) {
        // These DTO fields are native-generated media identity, never paths or
        // worker diagnostic text. A completion retains the admitted Save ticket.
        for(const auto* key:{"documentId","resultDocumentId"}) {
            const auto value=record->summary[key].toString();
            if(QRegularExpression("^media-[a-f0-9]{32}$").match(value).hasMatch() || value.isEmpty())dto[key]=value;
        }
        for(const auto* key:{"selectionGeneration","requestGeneration","completedRequestGeneration","saveRevision",
                             "selectedPts100ns","frameSequence","provisionalCaptureClock100ns","requestedPositionMs","resultSelectionGeneration"}) {
            const auto value=record->summary[key];
            if(value.isNull() || (value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble()>=0 && value.toDouble()<=9007199254740991.0))dto[key]=value;
        }
        for(const auto* key:{"kind","mode","waitingReason","saveState"}) {
            const auto value=record->summary[key].toString();
            if(QStringList{"camera","recording","file","live","behind_live","paused","playing","holding","held_unrecorded","video","end","opening","seeking",
                           "waiting","finishing","unavailable","idle","writing","committed","failed","latest_live","file_end"}.contains(value))dto[key]=value;
        }
        for(const auto* key:{"playing","recordingActive","hasFrame","heldUnrecorded"})
            if(record->summary[key].isBool())dto[key]=record->summary[key];
    }
    if(record->kind==OperationKind::Adjustment && record->summary["deviceAcknowledged"].isBool())dto["deviceAcknowledged"]=record->summary["deviceAcknowledged"];
    // Strict allowlist; a producer cannot forward arbitrary worker error/path
    // strings through the durable operation query.
    for (const auto key : {"audio","requestedAudio","actualAudio","ownerKind","errorCode"}) {
        const auto text = record->summary[key].toString();
        const QStringList safe{"none","saved_microphone","microphone","manual","agent-session","external-lease","worker_abandoned","capture_failed"};
        if (safe.contains(text) || (QString::fromLatin1(key)=="errorCode" && QStringList{"canceled","stale_state","timeout","unavailable","internal"}.contains(text))) dto[key] = text;
    }
    for (const auto key : {"originalName","processedName"}) {
        const auto text = record->summary[key].toString();
        if (QRegularExpression("^(?:IMG|VID)_[A-Za-z0-9_-]+\\.(?:jpg|mp4)$").match(text).hasMatch()) dto[key] = text;
    }
    if (record->kind == OperationKind::Adjustment && record->state == OperationState::FirstFrame) {
        const auto metrics = record->summary["metrics"].toObject();
        QJsonObject safeMetrics;
        for (const auto key : {"lumaMean","lumaP10","lumaP90","contrastSpread","focusScore","frameAgeMs"})
            if (metrics[key].isDouble() && std::isfinite(metrics[key].toDouble())) safeMetrics[key] = metrics[key];
        for (const auto key : {"focusAcceptable","keystoneQuad"}) if (metrics[key].isBool()) safeMetrics[key] = metrics[key];
        for (const auto key : {"sceneClass","sceneClassBasis","focusBasis"}) {
            const QStringList allowed{"paper","board","mixed","viewport_luma","viewport_laplacian"};
            if (allowed.contains(metrics[key].toString())) safeMetrics[key] = metrics[key];
        }
        dto["metrics"] = safeMetrics;
    }
    return dto;
}
void OperationTracker::Prune(qint64 now) {
    for (auto it = operations_.begin(); it != operations_.end();) {
        if (it->second.terminalAt && now-it->second.terminalAt >= 900000) it = operations_.erase(it);
        else ++it;
    }
}
} // namespace okuflow::agent
