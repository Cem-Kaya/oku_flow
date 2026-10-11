#pragma once

#include "okuflow/app/settings_store.hpp"

#include <optional>
#include <functional>
#include <QStringList>
#include "okuflow/app/protected_secret_store.hpp"

namespace okuflow {

class SettingsController {
public:
    struct StorageOperations {
        std::function<ProtectedSecretResult(const QString&)> readSecret;
        std::function<bool(const QString&, const QString&, QString*)> writeSecret;
        std::function<bool(const QString&, QString*)> removeSecret;
        std::function<bool(const QString&, const settings::PersistentSettings&)> saveSettings;
    };
    explicit SettingsController(QString settingsPath = {});
    SettingsController(QString settingsPath, StorageOperations storage);

    const settings::PersistentSettings& Settings() const noexcept;
    settings::PersistentSettings& MutableSettings() noexcept;

    // Successful automatic starts initialize index-only legacy profiles once.
    // An existing preferred device survives a temporary fallback.
    bool InitializeLegacyCameraIdentity(int openedIndex, const QString& openedLink);

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
    bool PrepareProtectedSecrets();

    StorageOperations storage_;
    QStringList pendingCredentialRemovals_;
    QString settingsPath_;
    settings::PersistentSettings settings_;
    QString startupNotice_;
    QString lastError_;
    bool protectedSecretReadFailed_{false};
};

} // namespace okuflow
