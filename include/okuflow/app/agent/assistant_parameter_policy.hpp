#pragma once
#include "okuflow/app/agent/app_control_service.hpp"
#include "okuflow/app/settings_store.hpp"
#include <QJsonArray>
#include <QSet>

namespace okuflow::agent {
// Pure whole-patch admission. Runtime inventory supplies only opaque installed
// voice handles; this function never probes devices or changes a preference.
ControlPlan PrepareAssistantPreferencePatch(const settings::AssistiveSettings& config,
    const QJsonArray& models,const QSet<QString>& installedVoices,const QJsonObject& patch,
    bool notesOnly,const QString& environmentSecret = {},bool nativeScene = false,
    bool overlayEnabled = true);
// Full values are never truncated. Projections and typed sameAs references
// keep finite Unicode preferences within the service's fixed result budget.
QJsonObject BoundAssistantPreferenceRead(QJsonObject parameters,const QString& valueKind = "all");
bool AssistantPreferenceValueInContract(Field field,const QJsonValue& value);
} // namespace okuflow::agent
