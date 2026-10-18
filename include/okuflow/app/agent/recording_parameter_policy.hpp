#pragma once
#include "okuflow/app/agent/app_control_service.hpp"
#include <QSet>

namespace okuflow::agent {
struct RecordingPreferenceSession {
    QString id, startedAt;
    bool stoppable{};
};
// Pure microphone planning; deprecated canvas input is atomically rejected.
// Runtime provides opaque installed microphone handles
// and the exact current recording identity. No device or file is touched here.
ControlPlan PrepareRecordingPreferencePatch(const std::map<Field,QJsonValue>& current,
    const QSet<QString>& microphones, const QJsonObject& patch,
    const RecordingPreferenceSession& session = {}, bool recorderUnavailable = false);
struct RecordingPreferenceFence {
    quint64 sourceEpoch{}, microphoneRevision{}, canvasRevision{};
    QString session;
};
// The irreversible stop has already been requested. Only the preference writes
// are deferred, and must never run after authority or native-user state changes.
QString RecordingPreferenceFenceStatus(const RecordingPreferenceFence& expected,
    bool contextActive, bool recordingGrant, quint64 sourceEpoch,
    quint64 microphoneRevision, quint64 canvasRevision, const QString& liveSession,
    bool recordingActive, bool workerAbandoned, bool expired);
} // namespace okuflow::agent
