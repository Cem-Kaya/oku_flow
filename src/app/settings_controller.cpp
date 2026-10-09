#include "okuflow/app/settings_controller.hpp"
#include "okuflow/app/protected_secret_store.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <utility>

namespace okuflow {

namespace {

QString MakeCustomEntityId(const QString& prefix)
{
    return QStringLiteral("%1-%2")
        .arg(prefix)
        .arg(QDateTime::currentMSecsSinceEpoch());
}

} // namespace

SettingsController::SettingsController(QString settingsPath)
    : settingsPath_(settingsPath.trimmed().isEmpty()
                        ? settings::ResolveSettingsPath()
                        : std::move(settingsPath))
{
    bool restoreBackupAfterSecretLoad = false;
    bool rewriteMigratedSettings = false;
    const settings::LoadResult primary = settings::LoadDetailed(settingsPath_);
    if (primary.status == settings::LoadStatus::Loaded && primary.settings) {
        settings_ = *primary.settings;
        rewriteMigratedSettings = primary.migrationApplied;
    } else if (primary.status == settings::LoadStatus::Missing) {
        InitializeDefaults();
    } else {
        const QString suffix =
            primary.status == settings::LoadStatus::UnsupportedVersion
                ? QStringLiteral(".unsupported-")
                : QStringLiteral(".corrupt-");
        const QString preservedPath =
            settingsPath_ + suffix +
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
        bool preserved = QFile::rename(settingsPath_, preservedPath);
        if (!preserved) {
            preserved = QFile::copy(settingsPath_, preservedPath);
        }

        const QString backupPath = settingsPath_ + QStringLiteral(".backup");
        const settings::LoadResult backup = settings::LoadDetailed(backupPath);
        if (backup.status == settings::LoadStatus::Loaded && backup.settings) {
            settings_ = *backup.settings;
            restoreBackupAfterSecretLoad = true;
            rewriteMigratedSettings = backup.migrationApplied;
            startupNotice_ =
                QStringLiteral("%1 Recovered the last valid settings backup%2.")
                    .arg(primary.error,
                         preserved
                             ? QStringLiteral("; the original file was preserved")
                             : QString());
        } else {
            InitializeDefaults();
            startupNotice_ =
                QStringLiteral("%1 OkuFlow started with safe defaults%2.")
                    .arg(primary.error,
                         preserved
                             ? QStringLiteral("; the original file was preserved")
                             : QString());
        }
    }

    LoadProtectedSecrets();
    if ((restoreBackupAfterSecretLoad || rewriteMigratedSettings) &&
        !settings::Save(settingsPath_, settings_)) {
        startupNotice_ +=
            (startupNotice_.isEmpty() ? QString() : QStringLiteral(" ")) +
            QStringLiteral("The recovered or migrated settings could not be written back to disk.");
    }
}

void SettingsController::InitializeDefaults()
{
    settings_.selectedPresetId = settings::DefaultPresetId();
    if (auto defaultConfig = ResolvePreset(settings_.selectedPresetId)) {
        settings_.currentConfig = std::move(*defaultConfig);
    } else if (!settings::BuiltInConfigs().empty()) {
        settings_.currentConfig = settings::BuiltInConfigs().front();
    }
}

void SettingsController::LoadProtectedSecrets()
{
    auto& assistive = settings_.assistive;
    if (assistive.vlmCredentialId.trimmed().isEmpty()) {
        return;
    }
    const ProtectedSecretResult secret =
        ProtectedSecretStore::Read(assistive.vlmCredentialId);
    switch (secret.status) {
    case ProtectedSecretResult::Status::Found:
        assistive.vlmApiKey = secret.value;
        break;
    case ProtectedSecretResult::Status::NotFound:
        startupNotice_ +=
            (startupNotice_.isEmpty() ? QString() : QStringLiteral(" ")) +
            QStringLiteral("The saved VLM credential was not found. Enter the API key again.");
        break;
    case ProtectedSecretResult::Status::Error:
        protectedSecretReadFailed_ = true;
        startupNotice_ +=
            (startupNotice_.isEmpty() ? QString() : QStringLiteral(" ")) +
            secret.error;
        break;
    }
}

const settings::PersistentSettings& SettingsController::Settings() const noexcept
{
    return settings_;
}

settings::PersistentSettings& SettingsController::MutableSettings() noexcept
{
    return settings_;
}

settings::AdvancedConfig SettingsController::DecorateLiveConfig(
    settings::AdvancedConfig config) const
{
    const settings::AdvancedConfig& stored = settings_.currentConfig;
    config.id = stored.id.isEmpty() ? QStringLiteral("current-live") : stored.id;
    config.name = stored.name.isEmpty() ? QStringLiteral("Current Setup") : stored.name;
    config.description = stored.description.isEmpty()
        ? QStringLiteral("Live configuration derived from quick mode and advanced tuning.")
        : stored.description;

    if (settings_.selectedPresetId.isEmpty()) {
        config.id = QStringLiteral("current-live");
        config.name = QStringLiteral("Current Setup");
        config.description =
            QStringLiteral("Live configuration derived from quick mode and advanced tuning.");
    } else if (auto selected = ResolvePreset(settings_.selectedPresetId)) {
        config.id = selected->id;
        config.name = selected->name;
        config.description = selected->description;
    }
    return config;
}

QString SettingsController::MatchPreset(const settings::AdvancedConfig& current,
                                        bool preserveCurrentSelection)
{
    QString matchedPresetId;
    if (preserveCurrentSelection && ResolvePreset(settings_.selectedPresetId)) {
        matchedPresetId = settings_.selectedPresetId;
    }

    const auto matches = [this, &current](const settings::PresetDefinition& preset) {
        const auto config = ResolvePreset(preset.id);
        return config && settings::AreConfigsEquivalent(current, *config);
    };

    if (matchedPresetId.isEmpty()) {
        for (const settings::PresetDefinition& preset : settings::BuiltInPresets()) {
            if (matches(preset)) {
                matchedPresetId = preset.id;
                break;
            }
        }
    }
    if (matchedPresetId.isEmpty()) {
        for (const settings::PresetDefinition& preset : settings_.customPresets) {
            if (matches(preset)) {
                matchedPresetId = preset.id;
                break;
            }
        }
    }

    settings_.selectedPresetId = matchedPresetId;
    settings_.currentConfig = DecorateLiveConfig(current);
    return matchedPresetId;
}

std::optional<settings::AdvancedConfig> SettingsController::ResolvePreset(
    const QString& presetId) const
{
    return settings::ResolveConfigForPreset(
        presetId, settings_.customConfigs, settings_.customPresets);
}

QString SettingsController::DefaultPromotedPresetName() const
{
    if (const settings::PresetDefinition* preset =
            settings::FindPresetById(settings_.selectedPresetId,
                                     settings_.customPresets)) {
        return preset->name + QStringLiteral(" Copy");
    }
    return QStringLiteral("Custom Quick Option");
}

settings::PresetDefinition SettingsController::PromoteCurrentConfig(
    settings::AdvancedConfig config, const QString& name)
{
    config.id = MakeCustomEntityId(QStringLiteral("custom-config"));
    config.name = name;
    config.description =
        QStringLiteral("Custom quick option created from Advanced Tuning.");

    settings::PresetDefinition preset;
    preset.id = MakeCustomEntityId(QStringLiteral("custom-preset"));
    preset.name = name;
    preset.description = config.description;
    preset.configId = config.id;
    preset.isBuiltIn = false;

    settings_.customConfigs.push_back(config);
    settings_.customPresets.push_back(preset);
    settings_.currentConfig = config;
    settings_.selectedPresetId = preset.id;
    return preset;
}

bool SettingsController::Save(const settings::AdvancedConfig& current)
{
    lastError_.clear();
    settings_.currentConfig = DecorateLiveConfig(current);
    QString credentialToRemove;
    if (!PrepareProtectedSecrets(&credentialToRemove)) {
        return false;
    }
    if (!settings::Save(settingsPath_, settings_)) {
        lastError_ =
            QStringLiteral("OkuFlow could not save settings. The previous settings file "
                           "and its backup were left intact.");
        return false;
    }
    if (!credentialToRemove.isEmpty()) {
        QString removeError;
        if (!ProtectedSecretStore::Remove(credentialToRemove, &removeError)) {
            lastError_ = removeError;
            return false;
        }
    }
    return true;
}

bool SettingsController::PrepareProtectedSecrets(QString* credentialToRemove)
{
    auto& assistive = settings_.assistive;
    if (protectedSecretReadFailed_ && assistive.vlmApiKey.isEmpty()) {
        lastError_ =
            QStringLiteral("Settings were not saved because Windows protected credential "
                           "storage could not be read. This prevents accidental credential loss.");
        return false;
    }

    if (!assistive.vlmApiKey.isEmpty()) {
        if (assistive.vlmCredentialId.trimmed().isEmpty()) {
            assistive.vlmCredentialId =
                ProtectedSecretStore::DefaultVlmCredentialId();
        }
        QString error;
        if (!ProtectedSecretStore::Write(assistive.vlmCredentialId,
                                         assistive.vlmApiKey,
                                         &error)) {
            lastError_ = error;
            return false;
        }
        protectedSecretReadFailed_ = false;
        return true;
    }

    if (!assistive.vlmCredentialId.trimmed().isEmpty()) {
        if (credentialToRemove) {
            *credentialToRemove = assistive.vlmCredentialId.trimmed();
        }
        assistive.vlmCredentialId.clear();
    }
    return true;
}

QString SettingsController::TakeStartupNotice()
{
    return std::exchange(startupNotice_, {});
}

QString SettingsController::LastError() const
{
    return lastError_;
}

} // namespace okuflow
