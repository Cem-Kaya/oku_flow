#pragma once

#include <QMetaType>
#include <QString>

namespace okuflow {

// Stable identity generated after recording start preflight succeeds.
// Queued recorder callbacks retain this identity for saved-media metadata.
struct RecordingSessionInfo {
    QString id;              // QUuid without braces
    QString timestampToken;  // filename timestamp (yyyyMMdd_HHmmss_zzz)
};

} // namespace okuflow

Q_DECLARE_METATYPE(okuflow::RecordingSessionInfo)
