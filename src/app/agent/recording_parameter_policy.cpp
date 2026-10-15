#include "okuflow/app/agent/recording_parameter_policy.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"
#include "okuflow/app/agent/control_schema.hpp"
#include <QJsonDocument>

namespace okuflow::agent {
ControlPlan PrepareRecordingPreferencePatch(const std::map<Field,QJsonValue>& current,
    const QSet<QString>& microphones,const QJsonObject& patch,
    const RecordingPreferenceSession& session,bool recorderUnavailable) {
    ControlPlan plan;
    const auto schema=QJsonDocument::fromJson(QByteArray::fromStdString(ParameterSetSchema(ParameterGroup::Recording))).object();
    if(!MatchesControlSchema(patch,schema)) {plan.error="invalid_argument";return plan;}
    auto fail=[&](const char* code) {plan=ControlPlan{};plan.error=QString::fromLatin1(code);return plan;};
    for(auto it=patch.begin();it!=patch.end();++it) {
        const auto field=it.key()=="recording.microphone"?Field::RecordingMicrophone:Field::RecordingCanvas;
        const auto existing=current.find(field);
        if(existing==current.end())return fail("unavailable");
        if(existing->second==it.value())continue;
        if(recorderUnavailable)return fail("unavailable");
        if(field==Field::RecordingMicrophone) {
            if(it.value()!="none" && !microphones.contains(it.value().toString()))return fail("not_found");
            plan.requiresApproval=true;plan.approvalDomain=ApprovalDomain::Configuration;
            if(!session.id.isEmpty()) {
                if(!session.stoppable)return fail("busy");
                plan.targetIdentity=session.id;plan.startedAt=session.startedAt;
                plan.requiresRecording=true;plan.asynchronous=true;plan.terminalRecording=true;
            }
        }
        plan.writes.push_back({field,it.value()});
    }
    return plan;
}

QString RecordingPreferenceFenceStatus(const RecordingPreferenceFence& expected,
    bool contextActive,bool recordingGrant,quint64 sourceEpoch,
    quint64 microphoneRevision,quint64 canvasRevision,const QString& liveSession,
    bool recordingActive,bool workerAbandoned,bool expired) {
    if(!contextActive || !recordingGrant)return "canceled";
    if(sourceEpoch!=expected.sourceEpoch || microphoneRevision!=expected.microphoneRevision ||
       canvasRevision!=expected.canvasRevision || (!liveSession.isEmpty() && liveSession!=expected.session))return "stale_state";
    if(workerAbandoned)return "unavailable";
    if(expired)return "timeout";
    return recordingActive?QStringLiteral("waiting"):QStringLiteral("ready");
}
} // namespace okuflow::agent
