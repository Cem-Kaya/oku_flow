#pragma once

#include "okuflow/app/settings_store.hpp"

#include <optional>

namespace okuflow {

class SettingsController {
public:
    explicit SettingsController(QString settingsPath = {});

    const settings::PersistentSettings& Settings() const noexcept;
    settings::PersistentSettings& MutableSettings() noexcept;

    settings::AdvancedConfig DecorateLiveConfig(
        settings::AdvancedConfig config) const;
    QString MatchPreset(const settings::AdvancedConfig& current,
                        bool preserveCurrentSelection);
    std::optional<settings::AdvancedConfig> ResolvePreset(
        const QString& presetId) const;
    QString DefaultPromotedPresetName() const;
    settings::PresetDefinition PromoteCurrentConfig(
        settings::AdvancedConfig config, const QString& name);

    bool Save(const settings::AdvancedConfig& current);
    QString TakeStartupNotice();
    QString LastError() const;

private:
    void InitializeDefaults();
    void LoadProtectedSecrets();
    bool PrepareProtectedSecrets(QString* credentialToRemove);

    QString settingsPath_;
    settings::PersistentSettings settings_;
    QString startupNotice_;
    QString lastError_;
    bool protectedSecretReadFailed_{false};
};

} // namespace okuflow
