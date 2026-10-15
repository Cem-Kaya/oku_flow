#pragma once

#include <QMetaType>
#include <QString>

namespace okuflow {
enum class RecordingOwnerKind { Manual, AgentSession, ExternalLease };
enum class RecordingAudioRequest { SavedMicrophone, None };
enum class RecordingSessionCompletion { Pending, Completed, Failed, Abandoned };

// Stable identity generated after recording start preflight succeeds.
// Queued recorder callbacks retain this identity for saved-media metadata.
struct RecordingSessionInfo {
    QString id;              // QUuid without braces
    QString timestampToken;  // filename timestamp (yyyyMMdd_HHmmss_zzz)
    RecordingOwnerKind ownerKind{RecordingOwnerKind::Manual};
    QString ownerId;
    RecordingAudioRequest requestedAudio{RecordingAudioRequest::SavedMicrophone};
    bool actualAudio{};
    RecordingSessionCompletion completion{RecordingSessionCompletion::Pending};
};

} // namespace okuflow

Q_DECLARE_METATYPE(okuflow::RecordingSessionInfo)
