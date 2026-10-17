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
    // Last known valid persisted state, separate from live staging. Missing on
    // first run until a commit succeeds. Runtime plaintext secrets are omitted.
    const std::optional<settings::PersistentSettings>& LastCommittedSettings() const noexcept;

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
    std::optional<settings::PresetDefinition> PromoteCurrentConfig(
        settings::AdvancedConfig config, const QString& name);

    bool Save(const settings::AdvancedConfig& current);
    QString TakeStartupNotice();
    QString LastError() const;

private:
    struct CredentialBinding {
        QString id;
        QString provider;
        QString endpoint;
        QString secret;
    };
    void InitializeDefaults();
    void LoadProtectedSecrets();
    bool PrepareProtectedSecrets();
    void RememberCommittedSettings(const settings::PersistentSettings& value);

    StorageOperations storage_;
    QStringList pendingCredentialRemovals_;
    QString settingsPath_;
    settings::PersistentSettings settings_;
    std::optional<settings::PersistentSettings> lastCommittedSettings_;
    // Runtime-only identity of the loaded/staged immutable secret generation.
    // A failed JSON commit may retry it, but cannot rewrite an older key id.
    std::optional<CredentialBinding> preparedCredential_;
    QString startupNotice_;
    QString lastError_;
    bool protectedSecretReadFailed_{false};
};

} // namespace okuflow
