#include "okuflow/app/settings_controller.hpp"
#include "okuflow/app/protected_secret_store.hpp"
#include "okuflow/app/app_language.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <utility>

namespace okuflow {

namespace {

QString MakeCustomEntityId(const QString& prefix)
{
    return QStringLiteral("%1-%2")
        .arg(prefix)
        .arg(QDateTime::currentMSecsSinceEpoch());
}

// Save reports only success; the size limit is the one refusal with a
// user-actionable cause, so name it rather than a generic I/O failure.
bool ExceedsSettingsLimit(const settings::PersistentSettings& value)
{
    return settings::Serialize(value).size() > settings::kMaxSettingsFileBytes;
}

QString SettingsLimitMessage()
{
    return QStringLiteral("Settings would exceed the %1 MiB settings file limit. "
                          "Delete old Assistant conversations to reduce them.")
        .arg(settings::kMaxSettingsFileBytes / (1024 * 1024));
}

} // namespace

SettingsController::SettingsController(QString settingsPath)
    : SettingsController(std::move(settingsPath), StorageOperations{})
{
}

SettingsController::SettingsController(QString settingsPath, StorageOperations storage)
    : storage_(std::move(storage))
    , settingsPath_(settingsPath.trimmed().isEmpty()
                        ? settings::ResolveSettingsPath()
                        : std::move(settingsPath))
{
    if (!storage_.readSecret) storage_.readSecret = ProtectedSecretStore::Read;
    if (!storage_.writeSecret) storage_.writeSecret = ProtectedSecretStore::Write;
    if (!storage_.removeSecret) storage_.removeSecret = ProtectedSecretStore::Remove;
    if (!storage_.saveSettings) storage_.saveSettings = settings::Save;
    bool restoreBackupAfterSecretLoad = false;
    bool rewriteMigratedSettings = false;
    const QString backupPath = settingsPath_ + QStringLiteral(".backup");
    const settings::LoadResult primary = settings::LoadDetailed(settingsPath_);
    if (primary.status == settings::LoadStatus::Loaded && primary.settings) {
        settings_ = *primary.settings;
        RememberCommittedSettings(settings_);
        rewriteMigratedSettings = primary.migrationApplied;
    } else if (primary.status == settings::LoadStatus::Missing) {
        // First run has no backup. A backup without its primary means an
        // interrupted save or recovery write-back; keep those settings rather
        // than silently starting from defaults.
        const settings::LoadResult backup = settings::LoadDetailed(backupPath);
        if (backup.status == settings::LoadStatus::Loaded && backup.settings) {
            settings_ = *backup.settings;
            RememberCommittedSettings(settings_);
            restoreBackupAfterSecretLoad = true;
            rewriteMigratedSettings = backup.migrationApplied;
            startupNotice_ = QStringLiteral(
                "The settings file was missing. Recovered the last valid settings backup.");
        } else {
            InitializeDefaults();
        }
    } else {
        const QString suffix =
            primary.status == settings::LoadStatus::UnsupportedVersion
                ? QStringLiteral(".unsupported-")
                : primary.status == settings::LoadStatus::TooLarge
                      ? QStringLiteral(".too-large-")
                      : QStringLiteral(".corrupt-");
        const QString preservedPath =
            settingsPath_ + suffix +
            QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz"));
        bool preserved = QFile::rename(settingsPath_, preservedPath);
        if (!preserved) {
            preserved = QFile::copy(settingsPath_, preservedPath);
        }

        const settings::LoadResult backup = settings::LoadDetailed(backupPath);
        if (backup.status == settings::LoadStatus::Loaded && backup.settings) {
            settings_ = *backup.settings;
            RememberCommittedSettings(settings_);
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

    pendingCredentialRemovals_ = settings_.assistive.pendingCredentialRemovals;
    LoadProtectedSecrets();
    if (restoreBackupAfterSecretLoad || rewriteMigratedSettings) {
        if (storage_.saveSettings(settingsPath_, settings_)) {
            RememberCommittedSettings(settings_);
        } else {
            startupNotice_ +=
                (startupNotice_.isEmpty() ? QString() : QStringLiteral(" ")) +
                QStringLiteral("The recovered or migrated settings could not be written back to disk.");
            if (ExceedsSettingsLimit(settings_)) {
                startupNotice_ += QStringLiteral(" ") + SettingsLimitMessage();
            }
        }
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

bool SettingsController::InitializeLegacyCameraIdentity(
    int openedIndex, const QString& openedLink)
{
    if (!settings_.cameraSymbolicLink.isEmpty() || openedIndex < 0 || openedLink.isEmpty()) {
        return false;
    }
    settings_.cameraIndex = openedIndex;
    settings_.cameraSymbolicLink = openedLink;
    return true;
}

void SettingsController::LoadProtectedSecrets()
{
    auto& assistive = settings_.assistive;
    if (assistive.vlmCredentialId.trimmed().isEmpty()) {
        return;
    }
    const ProtectedSecretResult secret =
        storage_.readSecret(assistive.vlmCredentialId);
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

const std::optional<settings::PersistentSettings>& SettingsController::LastCommittedSettings() const noexcept
{
    return lastCommittedSettings_;
}

void SettingsController::RememberCommittedSettings(const settings::PersistentSettings& value)
{
    lastCommittedSettings_ = value;
    lastCommittedSettings_->assistive.vlmApiKey.clear();
    // Mirror the serializer's canonical language token, including its fallback.
    const auto language = NormalizeLanguagePreference(value.language);
    lastCommittedSettings_->language = language.isEmpty()
        ? QString::fromLatin1(kSystemLanguagePreference) : language;
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
    if (!PrepareProtectedSecrets()) {
        return false;
    }
    auto persisted = settings_;
    persisted.assistive.pendingCredentialRemovals = pendingCredentialRemovals_;
    if (pendingCredentialRemovals_.contains(persisted.assistive.vlmCredentialId.trimmed())) {
        persisted.assistive.vlmCredentialId.clear();
    }
    if (!storage_.saveSettings(settingsPath_, persisted)) {
        // A late commit failure can leave the backup rotated to the current
        // primary, so do not promise that both generations are unchanged.
        lastError_ = QStringLiteral("OkuFlow could not save settings.");
        if (ExceedsSettingsLimit(persisted)) {
            lastError_ += QStringLiteral(" ") + SettingsLimitMessage();
        } else {
            lastError_ += QStringLiteral(
                " Check that the settings folder is writable and try again.");
        }
        return false;
    }
    RememberCommittedSettings(persisted);
    settings_.assistive.vlmCredentialId = persisted.assistive.vlmCredentialId;
    settings_.assistive.pendingCredentialRemovals = pendingCredentialRemovals_;
    for (const QString& credentialToRemove : pendingCredentialRemovals_) {
        QString removeError;
        if (!storage_.removeSecret(credentialToRemove, &removeError)) {
            lastError_ = removeError;
            return false;
        }

    }
    if (!pendingCredentialRemovals_.isEmpty()) {
        persisted.assistive.pendingCredentialRemovals.clear();
        if (!storage_.saveSettings(settingsPath_, persisted)) {
            lastError_ = QStringLiteral("Credential deletion completed, but its settings cleanup could not be saved. It will be retried.");
            return false;
        }
        RememberCommittedSettings(persisted);
        pendingCredentialRemovals_.clear();
        settings_.assistive.pendingCredentialRemovals.clear();
    }
    return true;
}

bool SettingsController::PrepareProtectedSecrets()
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
        if (pendingCredentialRemovals_.contains(assistive.vlmCredentialId.trimmed())) {
            // A durable tombstone must never identify a newly written key:
            // settings publication may fail and a later process will still
            // execute the old deletion intent from disk.
            assistive.vlmCredentialId =
                ProtectedSecretStore::DefaultVlmCredentialId() + QLatin1Char('/') +
                QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        QString error;
        if (!storage_.writeSecret(assistive.vlmCredentialId,
                                         assistive.vlmApiKey,
                                         &error)) {
            lastError_ = error;
            return false;
        }
        protectedSecretReadFailed_ = false;
        return true;
    }

    if (!assistive.vlmCredentialId.trimmed().isEmpty()) {
        const QString credentialId = assistive.vlmCredentialId.trimmed();
        if (!pendingCredentialRemovals_.contains(credentialId)) {
            pendingCredentialRemovals_.append(credentialId);
        }
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
