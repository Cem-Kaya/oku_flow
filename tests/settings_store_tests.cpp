#include <limits>
#include <memory>
#include <array>
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "okuflow/app/color_schemes.hpp"
#include "okuflow/app/protected_secret_store.hpp"
#include "okuflow/app/settings_controller.hpp"
#include "okuflow/app/settings_store.hpp"
#include "okuflow/cuda/cuda_interop.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QUuid>
#include <QtTest>

namespace okuflow::settings {

namespace {

AdvancedConfig MakePopulatedConfig()
{
    AdvancedConfig config;
    config.id = QStringLiteral("round-trip-id");
    config.name = QStringLiteral("Round Trip");
    config.description = QStringLiteral("Every persisted field is non-default.");
    config.blackWhiteEnabled = true;
    config.blackWhiteThreshold = 0.73f;
    config.zoomEnabled = true;
    config.zoomAmount = 4.25f;
    config.zoomCenterX = 0.31f;
    config.zoomCenterY = 0.79f;
    config.blurEnabled = true;
    config.blurSigma = 2.4f;
    config.blurRadius = 15;
    config.temporalSmoothEnabled = true;
    config.temporalSmoothAlpha = 0.61f;
    config.spatialSharpenEnabled = true;
    config.spatialUpscaler = 0;
    config.spatialSharpness = 0.44f;
    config.debugView = true;
    config.focusMarker = true;
    config.rotationQuarterTurns = 3;
    config.vlmAssistEnabled = true;
    config.assistiveOverlayEnabled = false;
    config.stabilizationEnabled = true;
    config.displayColorMode = 3;
    config.colorScheme = color_schemes::ColorScheme{
        QStringLiteral("custom"),
        QStringLiteral("Test colors"),
        QStringLiteral("Test colors"),
        color_schemes::SchemeMode::Gradient,
        {QColor(QStringLiteral("#102030")),
         QColor(QStringLiteral("#8090a0")),
         QColor(QStringLiteral("#f0e0d0"))},
        false,
        true,
        -1,
        false
    };
    config.contrast = 2.25f;
    config.brightness = -0.35f;
    config.keystoneEnabled = true;
    config.autoContrastEnabled = true;
    config.autoContrastStrength = 0.82f;
    config.autoTextClarityEnabled = true;
    config.backgroundFlattenEnabled = true;
    config.backgroundFlattenStrength = 0.77f;
    config.adaptiveBinarizationEnabled = true;
    config.sauvolaStrength = 0.33f;
    config.binarizationSoftness = 0.12f;
    config.textPolarityMode = 2;
    config.strokeWeight = -2;
    config.smartSharpenEnabled = true;
    config.smartSharpenStrength = 0.58f;
    config.claheEnabled = true;
    config.claheClipLimit = 3.7f;
    config.twoColorTextEnabled = true;
    config.textHysteresisEnabled = true;
    config.textHysteresisStrength = 0.13f;
    config.selectiveSharpenEnabled = true;
    config.focusDetectionEnabled = true;
    config.focusThreshold = 0.027f;
    config.glareSuppressionEnabled = true;
    config.glareSuppressionStrength = 0.64f;
    config.mlSuperResEnabled = true;
    config.mlSuperResStrength = 0.71f;
    config.mlSuperResPrefer2x = false;
    config.mlSuperResUltra1440p = true;
    return config;
}

bool WriteJson(const QString& path, const QJsonObject& object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(QJsonDocument(object).toJson()) >= 0;
}

QByteArray ReadBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool WriteBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

// Pads an ASCII string field so Serialize() is exactly `bytes` long.
PersistentSettings SettingsOfSerializedSize(qint64 bytes, int cameraIndex = 5)
{
    PersistentSettings settings;
    settings.cameraIndex = cameraIndex;
    settings.assistive.assistantInstructions.clear();
    const qint64 base = Serialize(settings).size();
    settings.assistive.assistantInstructions =
        QString(static_cast<qsizetype>(bytes - base), QLatin1Char('a'));
    return settings;
}

int StoredCameraIndex(const QString& path)
{
    const auto loaded = Load(path);
    return loaded ? loaded->cameraIndex : std::numeric_limits<int>::min();
}

QStringList TooLargeSidecars(const QTemporaryDir& directory)
{
    return QDir(directory.path())
        .entryList({QStringLiteral("settings.json.too-large-*")}, QDir::Files);
}

// Includes QSaveFile temporaries, so a failed save must leave no trace.
QStringList FileEntries(const QTemporaryDir& directory)
{
    return QDir(directory.path()).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}

void MakeFilesWritable(const QTemporaryDir& directory)
{
    for (const QString& name : FileEntries(directory)) {
        QFile::setPermissions(directory.filePath(name),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ReadUser | QFileDevice::WriteUser);
    }
}

} // namespace

class SettingsStoreTests : public QObject {
    Q_OBJECT

private slots:
    void uctModelDefaultsMigrationAndRoundTrip();
    void controlSettingsAndConversationKinds();
    void liveRecapDurationIsBoundedAndPreservesOtherPreferences();
    void defaultsUseLunaLow();
    void automaticFallbackPreservesPreferredCamera();
    void modelDefaultsPreserveExplicitSelections();
    void migratesMisspelledTerraModel();
    void roundTripPreservesAdvancedConfig();
    void textClarityMasterPreservesDormantPreferences();
    void textClarityEffectiveSettingsGateEntireFamily();
    void textClarityStageSelectionHonorsEveryRefinement();
    void invalidLanguageFallsBackToMigrationDefault();
    void loadsRemovedGlobalCompatibilityForMigration();
    void migratesLegacyV1();
    void rejectsCorruptJson();
    void rejectsFutureSettingsVersion();
    void saveCreatesValidBackup();
    void serializedBytesAreWhatSaveWrites();
    void loadCountsRawBytesAtCap();
    void saveAtExactCapRoundTripsThroughBackup();
    void saveOverCapLeavesFilesUntouched();
    void oversizePrimaryRecoversValidBackup_data();
    void oversizePrimaryRecoversValidBackup();
    void saveSetsAsideOversizePrimaryWithoutRotation();
    void refusedOversizeReplacementKeepsOriginal();
    void refusedStagingKeepsBackupGeneration_data();
    void refusedStagingKeepsBackupGeneration();
    void lockedOversizePrimaryStaysInPlace();
    void commitFailureKeepsCurrentPrimary();
    void missingPrimaryRecoversBackup_data();
    void missingPrimaryRecoversBackup();
    void controllerReportsSizeLimitAndKeepsValidPrimary();
    void committedSnapshotSeparatesLiveAndFailedSave();
    void committedSnapshotTracksRecoveredBackup();
    void committedSnapshotTracksPartialCredentialCommit();
    void capHoldsDocumentedCapacity();
    void credentialDeletionRetries_data();
    void credentialDeletionRetries();
    void pendingDeletionNeverReusesReplacementKey_data();
    void pendingDeletionNeverReusesReplacementKey();
    void credentialRotationKeepsRecoverableEndpointBinding_data();
    void credentialRotationKeepsRecoverableEndpointBinding();
    void unchangedCredentialBindingDoesNotCreateGenerations();
    void ignoresPlaintextApiKeyField();
    void clampsOutOfRangeValues();
    void sixteenXZoomPreservesSettingsAndBoundsProfiles();
    void zoomOutPreferencesRoundTripWithoutChangingLegacyFill();
    void equivalenceUsesUiTolerances();
    void presetIdentitySurvivesApplyTimeMutations();
    void quickModeSaveIsDurableBeforeReturn();
    void failedQuickModeSavePreservesOldModesAndLiveSettings();
    void danglingPresetResolutionLeavesSelectionUnchanged();
};

void SettingsStoreTests::liveRecapDurationIsBoundedAndPreservesOtherPreferences()
{
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("settings.json");
    PersistentSettings original;original.cameraSymbolicLink="fixture-camera";original.cameraSelectionExplicit=true;
    original.currentConfig.zoomAmount=2.5f;original.currentConfig.zoomCenterX=0.3f;original.microphoneEndpointId="__none__";
    QCOMPARE(original.liveRecapMinutes,5);
    for(const auto& pair:std::array<std::pair<int,int>,4>{{{-10,1},{3,3},{5,5},{999,5}}}) {
        original.liveRecapMinutes=pair.first;QVERIFY(Save(path,original));const auto loaded=Load(path);QVERIFY(loaded);
        QCOMPARE(loaded->liveRecapMinutes,pair.second);QCOMPARE(loaded->cameraSymbolicLink,original.cameraSymbolicLink);
        QCOMPARE(loaded->cameraSelectionExplicit,true);QCOMPARE(loaded->currentConfig.zoomAmount,2.5f);
        QCOMPARE(loaded->currentConfig.zoomCenterX,0.3f);QCOMPARE(loaded->microphoneEndpointId,original.microphoneEndpointId);
    }
    auto json=QJsonDocument::fromJson(Serialize(original)).object();json.remove("liveRecapMinutes");
    QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(json).toJson());file.close();
    const auto missing=Load(path);QVERIFY(missing);QCOMPARE(missing->liveRecapMinutes,5);
}

void SettingsStoreTests::uctModelDefaultsMigrationAndRoundTrip()
{
    using uct::ModelTier;
    const PersistentSettings defaults;
    QVERIFY(!defaults.ultraClearTextEnabled);
    QCOMPARE(defaults.ultraClearTextModels.gpuDetector,ModelTier::Medium);
    QCOMPARE(defaults.ultraClearTextModels.gpuRecognizer,ModelTier::Medium);
    QCOMPARE(defaults.ultraClearTextModels.cpuDetector,ModelTier::Tiny);
    QCOMPARE(defaults.ultraClearTextModels.cpuRecognizer,ModelTier::Small);
    QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("settings.json");
    auto root=QJsonDocument::fromJson(Serialize(defaults)).object();root.remove("ultraClearTextModels");
    root.remove("ultraClearTextEnabled");
    const auto write=[&]{QFile f(path);if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate))return false;return f.write(QJsonDocument(root).toJson())>0;};
    QVERIFY(write());auto loaded=Load(path);QVERIFY(loaded);
    QVERIFY(loaded->ultraClearTextModels==defaults.ultraClearTextModels);
    QVERIFY(!loaded->ultraClearTextEnabled);
    root["ultraClearTextModels"]=QJsonObject{{"gpuDetector","untrusted.onnx"},{"gpuRecognizer",42},{"cpuDetector","small"},{"cpuRecognizer","tiny"}};
    QVERIFY(write());loaded=Load(path);QVERIFY(loaded);
    QCOMPARE(loaded->ultraClearTextModels.gpuDetector,ModelTier::Medium);
    QCOMPARE(loaded->ultraClearTextModels.gpuRecognizer,ModelTier::Medium);
    QCOMPARE(loaded->ultraClearTextModels.cpuDetector,ModelTier::Small);
    QCOMPARE(loaded->ultraClearTextModels.cpuRecognizer,ModelTier::Tiny);
    // Explicit model choices survive the batching policy; no silent migration.
    root["ultraClearTextModels"]=QJsonObject{{"gpuDetector","medium"},{"gpuRecognizer","small"}};
    QVERIFY(write());loaded=Load(path);QVERIFY(loaded);
    QCOMPARE(loaded->ultraClearTextModels.gpuRecognizer,ModelTier::Small);
    loaded->ultraClearTextModels={ModelTier::Small,ModelTier::Tiny,ModelTier::Medium,ModelTier::Medium};
    loaded->ultraClearTextEnabled=true;
    QVERIFY(Save(path,*loaded));const auto saved=Load(path);QVERIFY(saved);
    QVERIFY(saved->ultraClearTextModels==loaded->ultraClearTextModels);
    QVERIFY(saved->ultraClearTextEnabled);
    auto disabled=*saved;disabled.ultraClearTextEnabled=false;
    QVERIFY(Save(path,disabled));QVERIFY(!Load(path)->ultraClearTextEnabled);
    const auto json=QJsonDocument::fromJson(Serialize(*saved)).object()["ultraClearTextModels"].toObject();
    QCOMPARE(json["gpuDetector"].toString(),"small");QCOMPARE(json["gpuRecognizer"].toString(),"tiny");
    QVERIFY(!json.contains("enabled"));
}

void SettingsStoreTests::controlSettingsAndConversationKinds()
{
    PersistentSettings settings;
    QVERIFY(settings.assistive.agentControlEnabled);
    QVERIFY(!settings.assistive.externalMcpEnabled);
    settings.assistive.agentControlEnabled = true;
    settings.assistive.externalMcpEnabled = true;
    CodexConversation agent; agent.threadId = QStringLiteral("agent-fixture");
    agent.kind = QStringLiteral("agent"); agent.toolsetVersion = QStringLiteral("dev-core");
    settings.codexConversations.push_back(agent);
    const auto root = QJsonDocument::fromJson(Serialize(settings)).object();
    QCOMPARE(root.value(QStringLiteral("version")).toInt(),17);
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("settings.json"));
    QVERIFY(Save(path,settings));
    const auto loaded = Load(path); QVERIFY(loaded);
    QVERIFY(loaded->assistive.agentControlEnabled); QVERIFY(loaded->assistive.externalMcpEnabled);
    QCOMPARE(loaded->codexConversations.front().kind,QStringLiteral("agent"));
    QCOMPARE(loaded->codexConversations.front().toolsetVersion,QStringLiteral("dev-core"));
    // A persisted opt-out must survive the new default, including after save.
    settings.assistive.agentControlEnabled = false;
    settings.assistive.externalMcpEnabled = false;
    QVERIFY(Save(path,settings));
    const auto disabled = Load(path); QVERIFY(disabled);
    QVERIFY(!disabled->assistive.agentControlEnabled);
    QVERIFY(!disabled->assistive.externalMcpEnabled);
    // Missing keys in current files use the same defaults as a fresh install.
    auto missing = root;
    auto assistive = missing.value(QStringLiteral("assistive")).toObject();
    assistive.remove(QStringLiteral("agentControlEnabled"));
    assistive.remove(QStringLiteral("externalMcpEnabled"));
    missing[QStringLiteral("assistive")] = assistive;
    QFile missingFile(path); QVERIFY(missingFile.open(QIODevice::WriteOnly|QIODevice::Truncate));
    missingFile.write(QJsonDocument(missing).toJson()); missingFile.close();
    const auto defaults = Load(path); QVERIFY(defaults);
    QVERIFY(defaults->assistive.agentControlEnabled);
    QVERIFY(!defaults->assistive.externalMcpEnabled);
    for (const QString kind : {QString(),QStringLiteral("agent"),QStringLiteral("coding"),QStringLiteral("future-power"),QStringLiteral("AGENT")}) {
        QJsonObject legacy = root; legacy[QStringLiteral("version")] = 16;
        QJsonObject entry{{QStringLiteral("threadId"),QStringLiteral("legacy-fixture")},{QStringLiteral("explain"),true}};
        if (!kind.isNull()) entry[QStringLiteral("kind")] = kind;
        legacy[QStringLiteral("codexConversations")] = QJsonArray{entry};
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
        file.write(QJsonDocument(legacy).toJson()); file.close();
        const auto migrated = Load(path); QVERIFY(migrated);
        QVERIFY(migrated->assistive.agentControlEnabled); QVERIFY(!migrated->assistive.externalMcpEnabled);
        const QString expected = kind.isNull() ? QStringLiteral("explain")
            : (kind == QStringLiteral("agent") || kind == QStringLiteral("coding") ? kind : QStringLiteral("assistant"));
        QCOMPARE(migrated->codexConversations.front().kind,expected);
    }
}

void SettingsStoreTests::automaticFallbackPreservesPreferredCamera()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.json"));
    PersistentSettings preferred;
    preferred.cameraIndex = 1;
    preferred.cameraSymbolicLink = QStringLiteral("fixture-camera-B");
    preferred.cameraSelectionExplicit = true;
    QVERIFY(Save(path, preferred));
    SettingsController controller(path);
    QVERIFY(!controller.InitializeLegacyCameraIdentity(0, QStringLiteral("fixture-camera-A")));
    QVERIFY(controller.Save(controller.Settings().currentConfig));
    const auto reloaded = Load(path);
    QVERIFY(reloaded.has_value());
    QCOMPARE(reloaded->cameraSymbolicLink, preferred.cameraSymbolicLink);
    QCOMPARE(reloaded->cameraIndex, 1);
    QVERIFY(reloaded->cameraSelectionExplicit);

    preferred.cameraSymbolicLink.clear();
    preferred.cameraSelectionExplicit = false;
    QVERIFY(Save(path, preferred));
    SettingsController legacy(path);
    QVERIFY(legacy.InitializeLegacyCameraIdentity(1, QStringLiteral("fixture-camera-B")));
    QVERIFY(!legacy.InitializeLegacyCameraIdentity(0, QStringLiteral("fixture-camera-A")));
    QCOMPARE(legacy.Settings().cameraSymbolicLink, QStringLiteral("fixture-camera-B"));
    QCOMPARE(legacy.Settings().cameraIndex, 1);
    QVERIFY(legacy.Save(legacy.Settings().currentConfig));
    const auto migrated = Load(path);
    QVERIFY(migrated.has_value());
    QCOMPARE(migrated->cameraSymbolicLink, QStringLiteral("fixture-camera-B"));
    QVERIFY(!migrated->cameraSelectionExplicit);
}

void SettingsStoreTests::textClarityMasterPreservesDormantPreferences()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PersistentSettings expected;
    expected.currentConfig = MakePopulatedConfig();
    expected.currentConfig.autoTextClarityEnabled = false;
    expected.customConfigs.push_back(expected.currentConfig);
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    QVERIFY(Save(path, expected));
    const auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QVERIFY(!loaded->currentConfig.autoTextClarityEnabled);
    QVERIFY(AreConfigsEquivalent(loaded->currentConfig, expected.currentConfig));
    QCOMPARE(loaded->customConfigs.size(), expected.customConfigs.size());
    QVERIFY(AreConfigsEquivalent(loaded->customConfigs.front(), expected.customConfigs.front()));
    // Re-enabling the existing master never resets saved refinements.
    auto restored = loaded->currentConfig;
    restored.autoTextClarityEnabled = true;
    auto enabled = expected.currentConfig;
    enabled.autoTextClarityEnabled = true;
    QVERIFY(AreConfigsEquivalent(restored, enabled));
}

void SettingsStoreTests::textClarityEffectiveSettingsGateEntireFamily()
{
    ProcessingSettings preferences;
    preferences.enableBackgroundFlatten = true;
    preferences.enableAdaptiveBinarization = true;
    preferences.enableSmartSharpen = true;
    preferences.enableClahe = true;
    preferences.enableTwoColorText = true;
    preferences.enableTextHysteresis = true;
    preferences.enableSelectiveSharpen = true;
    preferences.enableFocusDetection = true;
    preferences.enableGlareSuppression = true;
    preferences.strokeWeight = 3;
    preferences.sauvolaStrength = 0.4f;
    preferences.enableBlackWhite = true;
    preferences.displayColorTransform = DisplayColorTransform::kInvert;
    preferences.enableMlSuperRes = true;
    auto effective = preferences;
    effective.ApplyTextClarityMaster();
    QVERIFY(!effective.enableBackgroundFlatten && !effective.enableAdaptiveBinarization &&
            !effective.enableSmartSharpen && !effective.enableClahe && !effective.enableTwoColorText &&
            !effective.enableTextHysteresis && !effective.enableSelectiveSharpen &&
            !effective.enableFocusDetection && !effective.enableGlareSuppression);
    QVERIFY(effective.enableBlackWhite); // Dormant Adaptive cannot suppress independent B&W.
    QCOMPARE(effective.displayColorTransform, DisplayColorTransform::kInvert);
    QVERIFY(effective.enableMlSuperRes);
    QCOMPARE(effective.strokeWeight, preferences.strokeWeight);
    QCOMPARE(effective.sauvolaStrength, preferences.sauvolaStrength);
    QVERIFY(preferences.enableAdaptiveBinarization); // Effective copy never mutates saved flags.
    preferences.enableAutoTextClarity = true;
    effective = preferences;
    effective.ApplyTextClarityMaster();
    QVERIFY(effective.enableAutoTextClarity && effective.enableBackgroundFlatten &&
            effective.enableAdaptiveBinarization && effective.enableSmartSharpen &&
            effective.enableClahe && effective.enableTwoColorText && effective.enableTextHysteresis &&
            effective.enableSelectiveSharpen && effective.enableFocusDetection && effective.enableGlareSuppression);
}

void SettingsStoreTests::textClarityStageSelectionHonorsEveryRefinement()
{
    // Exercise the selection actually consumed by CUDA, not only the master
    // normalization copy. Previously CUDA re-enabled unchecked refinements.
    for (int master = 0; master < 2; ++master) {
        for (unsigned mask = 0; mask < 512; ++mask) {
            ProcessingSettings preferences;
            preferences.enableAutoTextClarity = master != 0;
            preferences.enableBackgroundFlatten = (mask & 1) != 0;
            preferences.enableAdaptiveBinarization = (mask & 2) != 0;
            preferences.enableSmartSharpen = (mask & 4) != 0;
            preferences.enableClahe = (mask & 8) != 0;
            preferences.enableTwoColorText = (mask & 16) != 0;
            preferences.enableTextHysteresis = (mask & 32) != 0;
            preferences.enableSelectiveSharpen = (mask & 64) != 0;
            preferences.enableFocusDetection = (mask & 128) != 0;
            preferences.enableGlareSuppression = (mask & 256) != 0;
            const auto stages = preferences.TextClarityStages();
            const std::array actual{stages.background, stages.adaptive, stages.smartSharpen,
                stages.clahe, stages.twoColor, stages.hysteresis, stages.selectiveSharpen,
                stages.focus, stages.glare};
            for (unsigned child = 0; child < actual.size(); ++child)
                QCOMPARE(actual[child], master != 0 && (mask & (1u << child)) != 0);
            QCOMPARE(stages.pipeline, master != 0 && (mask & (1 | 2 | 4 | 8 | 128 | 256)) != 0);
            auto effective = preferences;
            effective.ApplyTextClarityMaster();
            const auto normalized = effective.TextClarityStages();
            QCOMPARE(normalized.background, stages.background);
            QCOMPARE(normalized.adaptive, stages.adaptive);
            QCOMPARE(normalized.smartSharpen, stages.smartSharpen);
            QCOMPARE(normalized.selectiveSharpen, stages.selectiveSharpen);
            QCOMPARE(normalized.pipeline, stages.pipeline);
            // Dormant flatten strength must never leak into the combined
            // glare-only launch. The persisted preference remains available.
            QCOMPARE(effective.backgroundFlattenStrength,
                     stages.background ? preferences.backgroundFlattenStrength : 0.0f);
            QCOMPARE(preferences.backgroundFlattenStrength, 0.8f);
        }
    }
}

void SettingsStoreTests::defaultsUseLunaLow()
{
    const PersistentSettings settings;
    QCOMPARE(settings.assistive.codexModel, QStringLiteral("gpt-6-luna"));
    QCOMPARE(settings.assistive.codexReasoningEffort, QStringLiteral("low"));
    QCOMPARE(settings.currentConfig.spatialUpscaler, 1);
}

void SettingsStoreTests::modelDefaultsPreserveExplicitSelections()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    QVERIFY(WriteJson(path, QJsonObject{
        {QStringLiteral("version"), 16},
        {QStringLiteral("assistive"), QJsonObject{}}}));
    const LoadResult missingModel = LoadDetailed(path);
    QVERIFY(missingModel.settings.has_value());
    QCOMPARE(missingModel.settings->assistive.codexModel,
             QStringLiteral("gpt-6-luna"));
    QVERIFY(!missingModel.migrationApplied);

    for (const QString& model : {QStringLiteral("gpt-5.6-luna"),
                                 QStringLiteral("gpt-6-luna"),
                                 QStringLiteral("custom-model")}) {
        QVERIFY(WriteJson(path, QJsonObject{
            {QStringLiteral("version"), 16},
            {QStringLiteral("assistive"), QJsonObject{
                {QStringLiteral("codexModel"), model}}}}));
        const LoadResult loaded = LoadDetailed(path);
        QVERIFY(loaded.settings.has_value());
        QCOMPARE(loaded.settings->assistive.codexModel, model);
        QVERIFY(!loaded.migrationApplied);
    }
}

void SettingsStoreTests::migratesMisspelledTerraModel()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    QVERIFY(WriteJson(
        path,
        QJsonObject{
            {QStringLiteral("version"), 15},
            {QStringLiteral("assistive"),
             QJsonObject{
                 {QStringLiteral("codexModel"),
                  QStringLiteral(" Gpt-5.6-Tera ")}}}}));

    const LoadResult loaded = LoadDetailed(path);
    QCOMPARE(loaded.status, LoadStatus::Loaded);
    QVERIFY(loaded.settings.has_value());
    QVERIFY(loaded.migrationApplied);
    QCOMPARE(loaded.settings->assistive.codexModel,
             QStringLiteral("gpt-5.6-terra"));

    SettingsController controller(path);
    QCOMPARE(controller.Settings().assistive.codexModel,
             QStringLiteral("gpt-5.6-terra"));

    QFile migratedFile(path);
    QVERIFY(migratedFile.open(QIODevice::ReadOnly));
    const QJsonObject migratedRoot =
        QJsonDocument::fromJson(migratedFile.readAll()).object();
    QCOMPARE(migratedRoot.value(QStringLiteral("version")).toInt(), 17);
    QCOMPARE(
        migratedRoot.value(QStringLiteral("assistive"))
            .toObject()
            .value(QStringLiteral("codexModel"))
            .toString(),
        QStringLiteral("gpt-5.6-terra"));
}

void SettingsStoreTests::roundTripPreservesAdvancedConfig()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nested/settings.json"));

    PersistentSettings expected;
    expected.language = QStringLiteral("tr");
    expected.userDataRoot = QStringLiteral("D:/OkuFlow Data");
    expected.cameraIndex = 4;
    expected.cameraSymbolicLink = QStringLiteral("fixture-camera-stable-link");
    expected.microphoneEndpointId =
        QStringLiteral("{0.0.1.00000000}.test-microphone");
    expected.cameraFormatStableId =
        QStringLiteral("{test-subtype}_1920x1080@60/1");
    expected.phoneCameraZoom.insert(QStringLiteral("okuflow-phone:fixture"), 3.25f);
    expected.phoneCameraZoom.insert(QStringLiteral("okuflow-phone:fixture/physical/0/2"), 1.0f);
    expected.cameraWorkingSets.insert(QStringLiteral("fixture-camera-stable-link"), {expected.cameraFormatStableId, 3});
    expected.cameraAccelerationAttempt =
        QStringLiteral("\\\\?\\usb#vid_test");
    expected.cameraAcceleration.insert(
        QStringLiteral("\\\\?\\usb#vid_test"),
        CameraAccelerationSetting{
            CameraAccelerationMode::ForceAccelerated,
            true,
            QStringLiteral("test fallback"),
            QStringLiteral("2026-07-29T12:00:00Z"),
            QStringLiteral("zeroCopy")});
    expected.rotationQuarterTurns = 3;
    expected.virtualJoystick = true;
    expected.zoomWheelAcceleration = false;
    expected.controlsCollapsed = false;
    expected.simpleUiMode = false;
    expected.advancedPanelWidth = 744;
    expected.viewportRateMode = ViewportRateMode::Fps90;
    expected.viewportFitMode = ViewportFitModeSetting::Fit;
    expected.recordingCanvasMode = RecordingCanvasMode::Sd480;
    expected.uiSectionStates.insert(QStringLiteral("device"), false);
    expected.uiSectionStates.insert(QStringLiteral("textClarity"), true);
    expected.assistiveOverlayGeometry = QRect(12, 34, 640, 480);
    expected.assistiveOverlayDockPosition = QStringLiteral("right");
    expected.annotationColor = QStringLiteral("#00e5ff");
    expected.annotationWidthPixels = 13;
    expected.annotationCaptureOnExit = false;
    expected.annotationDashed = true;
    expected.annotationShapeKind = QStringLiteral("ellipse");
    expected.annotationTextSizePixels = 58;
    expected.setupAssistantDeclined = true;
    expected.selectedPresetId = QStringLiteral("custom-preset");
    expected.currentConfig = MakePopulatedConfig();
    expected.customColorScheme = expected.currentConfig.colorScheme;
    expected.assistive.aiProvider = QStringLiteral("codex");
    expected.assistive.codexExecutablePath = QStringLiteral("C:/tools/codex.exe");
    expected.assistive.codexModel = QStringLiteral("test-model");
    expected.assistive.codexReasoningEffort = QStringLiteral("high");
    expected.assistive.codexInternetEnabled = true;
    expected.assistive.codexCodingEnabled = true;
    expected.assistive.codexWorkspaceDirectory = QStringLiteral("C:/workspace");
    expected.assistive.assistantInstructions = QStringLiteral("Reply clearly.");
    expected.assistive.vlmApiUrl = QStringLiteral("https://example.invalid/v1");
    expected.assistive.vlmApiKey = QStringLiteral("secret");
    expected.assistive.vlmCredentialId = QStringLiteral("OkuFlow/Test Round Trip");
    expected.assistive.pendingCredentialRemovals = {QStringLiteral("fixture-delete-id")};
    expected.assistive.vlmModel = QStringLiteral("vision");
    expected.assistive.vlmPrompt = QStringLiteral("Describe.");
    expected.assistive.ttsEngine = QStringLiteral("winrt");
    expected.assistive.ttsVoiceName = QStringLiteral("Natural Voice");
    expected.assistive.ttsVoiceLocale = QStringLiteral("en-US");
    expected.assistive.ttsRate = 0.4;
    expected.assistive.lectureNotesEnabled = false;
    expected.codexConversations.push_back(
        {QStringLiteral("thread-1"), QStringLiteral("Title"), QStringLiteral("Preview"), 10, 20});
    expected.codexConversations.push_back(
        {QStringLiteral("explain-thread"), {}, QStringLiteral("Slide 2"), 30, 40, true});
    expected.explainThreadId = QStringLiteral("explain-thread");
    expected.customConfigs.push_back(expected.currentConfig);
    expected.customPresets.push_back(
        {QStringLiteral("custom-preset"), QStringLiteral("Custom"), QStringLiteral("Description"),
         expected.currentConfig.id, false});

    QVERIFY(Save(path, expected));
    const auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->assistive.vlmApiKey.isEmpty());
    QCOMPARE(loaded->assistive.vlmCredentialId,
             expected.assistive.vlmCredentialId);
    QCOMPARE(loaded->assistive.pendingCredentialRemovals,
             expected.assistive.pendingCredentialRemovals);

    QFile savedFile(path);
    QVERIFY(savedFile.open(QIODevice::ReadOnly));
    const QByteArray savedJson = savedFile.readAll();
    QVERIFY(!savedJson.contains("secret"));
    QVERIFY(!savedJson.contains("\"vlmApiKey\""));

    QCOMPARE(loaded->cameraIndex, expected.cameraIndex);
    QCOMPARE(loaded->cameraSymbolicLink, expected.cameraSymbolicLink);
    QCOMPARE(loaded->microphoneEndpointId,
             expected.microphoneEndpointId);
    QCOMPARE(loaded->language, expected.language);
    QCOMPARE(loaded->userDataRoot, expected.userDataRoot);
    QCOMPARE(loaded->cameraFormatStableId, expected.cameraFormatStableId);
    QCOMPARE(loaded->phoneCameraZoom, expected.phoneCameraZoom);
    QCOMPARE(loaded->cameraWorkingSets.size(), expected.cameraWorkingSets.size());
    QCOMPARE(loaded->cameraWorkingSets.value(expected.cameraSymbolicLink).formatStableId, expected.cameraFormatStableId);
    QCOMPARE(loaded->cameraWorkingSets.value(expected.cameraSymbolicLink).rotationQuarterTurns, 3);
    QCOMPARE(loaded->cameraAccelerationAttempt,
             expected.cameraAccelerationAttempt);
    QCOMPARE(loaded->cameraAcceleration.size(),
             expected.cameraAcceleration.size());
    const auto loadedAcceleration =
        loaded->cameraAcceleration.value(
            QStringLiteral("\\\\?\\usb#vid_test"));
    QCOMPARE(loadedAcceleration.mode,
             CameraAccelerationMode::ForceAccelerated);
    QVERIFY(loadedAcceleration.automaticFallback);
    QCOMPARE(loadedAcceleration.reason,
             QStringLiteral("test fallback"));
    QCOMPARE(loadedAcceleration.decidedOn,
             QStringLiteral("2026-07-29T12:00:00Z"));
    QCOMPARE(loadedAcceleration.lastRung,
             QStringLiteral("zeroCopy"));
    QCOMPARE(loaded->rotationQuarterTurns, expected.rotationQuarterTurns);
    QCOMPARE(loaded->virtualJoystick, expected.virtualJoystick);
    QCOMPARE(loaded->zoomWheelAcceleration, expected.zoomWheelAcceleration);
    QCOMPARE(loaded->controlsCollapsed, expected.controlsCollapsed);
    QCOMPARE(loaded->simpleUiMode, expected.simpleUiMode);
    QCOMPARE(loaded->advancedPanelWidth, expected.advancedPanelWidth);
    QCOMPARE(loaded->viewportRateMode, expected.viewportRateMode);
    QCOMPARE(loaded->viewportFitMode, expected.viewportFitMode);
    QCOMPARE(loaded->recordingCanvasMode, RecordingCanvasMode::Source);
    QVERIFY(!QJsonDocument::fromJson(savedJson).object().contains(QStringLiteral("recording")));
    // A retained pre-migration preference must also be ignored on load.
    auto legacyRoot = QJsonDocument::fromJson(savedJson).object();
    legacyRoot.insert(QStringLiteral("recording"),
        QJsonObject{{QStringLiteral("canvasMode"), static_cast<int>(RecordingCanvasMode::Sd480)}});
    const QString legacyPath = dir.filePath(QStringLiteral("legacy-canvas.json"));
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    legacyFile.write(QJsonDocument(legacyRoot).toJson());
    legacyFile.close();
    const auto legacyLoaded = Load(legacyPath);
    QVERIFY(legacyLoaded.has_value());
    QCOMPARE(legacyLoaded->recordingCanvasMode, RecordingCanvasMode::Source);
    QCOMPARE(loaded->uiSectionStates, expected.uiSectionStates);
    QCOMPARE(loaded->assistiveOverlayGeometry, expected.assistiveOverlayGeometry);
    QCOMPARE(loaded->assistiveOverlayDockPosition, expected.assistiveOverlayDockPosition);
    QCOMPARE(loaded->annotationColor, expected.annotationColor);
    QCOMPARE(loaded->annotationWidthPixels, expected.annotationWidthPixels);
    QCOMPARE(loaded->annotationCaptureOnExit, expected.annotationCaptureOnExit);
    QCOMPARE(loaded->annotationDashed, expected.annotationDashed);
    QCOMPARE(loaded->annotationShapeKind, expected.annotationShapeKind);
    QCOMPARE(loaded->annotationTextSizePixels,
             expected.annotationTextSizePixels);
    QCOMPARE(loaded->setupAssistantDeclined, expected.setupAssistantDeclined);
    QCOMPARE(loaded->selectedPresetId, expected.selectedPresetId);
    QCOMPARE(loaded->currentConfig.id, expected.currentConfig.id);
    QCOMPARE(loaded->currentConfig.name, expected.currentConfig.name);
    QCOMPARE(loaded->currentConfig.description, expected.currentConfig.description);
    QVERIFY(AreConfigsEquivalent(loaded->currentConfig, expected.currentConfig));
    QCOMPARE(loaded->currentConfig.rotationQuarterTurns,
             expected.currentConfig.rotationQuarterTurns);
    QVERIFY(color_schemes::SchemesEquivalent(
        loaded->customColorScheme, expected.customColorScheme));
    QCOMPARE(loaded->assistive.codexExecutablePath, expected.assistive.codexExecutablePath);
    QCOMPARE(loaded->assistive.codexInternetEnabled, expected.assistive.codexInternetEnabled);
    QCOMPARE(loaded->assistive.ttsVoiceName, expected.assistive.ttsVoiceName);
    QCOMPARE(loaded->assistive.ttsRate, expected.assistive.ttsRate);
    QCOMPARE(loaded->codexConversations.size(), expected.codexConversations.size());
    QVERIFY(!loaded->codexConversations.front().explain);
    QCOMPARE(loaded->codexConversations.back().threadId, QStringLiteral("explain-thread"));
    QVERIFY(loaded->codexConversations.back().explain);
    QCOMPARE(loaded->codexConversations.back().title, QString());
    QCOMPARE(loaded->codexConversations.back().preview, QStringLiteral("Slide 2"));
    QCOMPARE(loaded->codexConversations.back().updatedAt, qint64(40));
    QCOMPARE(loaded->explainThreadId, expected.explainThreadId);
    QCOMPARE(loaded->customConfigs.size(), expected.customConfigs.size());
    QCOMPARE(loaded->customPresets.size(), expected.customPresets.size());
    QVERIFY(AreConfigsEquivalent(loaded->customConfigs.front(), expected.customConfigs.front()));

    // Forgetting the Explain conversation round-trips as "start fresh".
    expected.explainThreadId.clear();
    const QString resetPath = dir.filePath(QStringLiteral("reset-settings.json"));
    QVERIFY(Save(resetPath, expected));
    const auto reset = Load(resetPath);
    QVERIFY(reset.has_value());
    QCOMPARE(reset->explainThreadId, QString());
}

void SettingsStoreTests::invalidLanguageFallsBackToMigrationDefault()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    QVERIFY(WriteJson(
        path,
        QJsonObject{{QStringLiteral("version"), 15},
                    {QStringLiteral("language"),
                     QStringLiteral("not-a-language")}}));

    const auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->language.isEmpty());
}

void SettingsStoreTests::loadsRemovedGlobalCompatibilityForMigration()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("legacy-settings.json"));
    QVERIFY(WriteJson(
        path,
        QJsonObject{
            {QStringLiteral("capture"),
             QJsonObject{
                 {QStringLiteral("widerCameraCompatibility"), true}}}}));

    const auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->legacyWiderCameraCompatibility);

    QVERIFY(Save(path, *loaded));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject capture =
        QJsonDocument::fromJson(file.readAll())
            .object()
            .value(QStringLiteral("capture"))
            .toObject();
    QVERIFY(!capture.contains(
        QStringLiteral("widerCameraCompatibility")));
}

void SettingsStoreTests::migratesLegacyV1()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("legacy.json"));
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("virtualJoystick"), true},
        {QStringLiteral("controlsCollapsed"), false},
        {QStringLiteral("rotationQuarterTurns"), 5},
        {QStringLiteral("blackWhite"),
         QJsonObject{{QStringLiteral("enabled"), true},
                     {QStringLiteral("threshold"), 2.0}}},
        {QStringLiteral("zoom"),
         QJsonObject{{QStringLiteral("enabled"), true},
                     {QStringLiteral("amount"), 3.5},
                     {QStringLiteral("centerX"), -1.0},
                     {QStringLiteral("centerY"), 2.0}}},
        {QStringLiteral("blur"),
         QJsonObject{{QStringLiteral("enabled"), true},
                     {QStringLiteral("sigma"), 2.5},
                     {QStringLiteral("radius"), 9}}},
        {QStringLiteral("temporalSmooth"),
         QJsonObject{{QStringLiteral("enabled"), true},
                     {QStringLiteral("alpha"), 0.6}}},
        {QStringLiteral("spatialSharpen"),
         QJsonObject{{QStringLiteral("enabled"), true},
                     {QStringLiteral("backend"), QStringLiteral("fsr")},
                     {QStringLiteral("sharpness"), 0.8}}}
    };
    QVERIFY(WriteJson(path, root));

    const auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->virtualJoystick);
    QVERIFY(!loaded->controlsCollapsed);
    QVERIFY(loaded->currentConfig.blackWhiteEnabled);
    QCOMPARE(loaded->currentConfig.blackWhiteThreshold, 1.0f);
    QVERIFY(loaded->currentConfig.zoomEnabled);
    QCOMPARE(loaded->currentConfig.zoomAmount, 3.5f);
    QCOMPARE(loaded->currentConfig.zoomCenterX, 0.0f);
    QCOMPARE(loaded->currentConfig.zoomCenterY, 1.0f);
    QCOMPARE(loaded->currentConfig.spatialUpscaler, 0);
    QCOMPARE(loaded->rotationQuarterTurns, loaded->currentConfig.rotationQuarterTurns);
    QVERIFY(loaded->selectedPresetId.isEmpty());
}

void SettingsStoreTests::rejectsCorruptJson()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("corrupt.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("{ this is not json") > 0);
    file.close();
    QVERIFY(!Load(path).has_value());
}

void SettingsStoreTests::clampsOutOfRangeValues()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("clamp.json"));
    const QJsonObject current{
        {QStringLiteral("blackWhiteThreshold"), 5.0},
        {QStringLiteral("zoomAmount"), -5.0},
        {QStringLiteral("zoomCenterX"), -2.0},
        {QStringLiteral("zoomCenterY"), 4.0},
        {QStringLiteral("blurSigma"), -1.0},
        {QStringLiteral("blurRadius"), -20},
        {QStringLiteral("temporalSmoothAlpha"), 8.0},
        {QStringLiteral("spatialUpscaler"), 99},
        {QStringLiteral("spatialSharpness"), -8.0},
        {QStringLiteral("rotationQuarterTurns"), 11},
        {QStringLiteral("displayColorMode"), 500},
        {QStringLiteral("contrast"), 50.0},
        {QStringLiteral("brightness"), -50.0},
        {QStringLiteral("sauvolaStrength"), 9.0},
        {QStringLiteral("binarizationSoftness"), 9.0},
        {QStringLiteral("textPolarityMode"), 99},
        {QStringLiteral("strokeWeight"), -99},
        {QStringLiteral("claheClipLimit"), 99.0},
        {QStringLiteral("focusThreshold"), -2.0},
        {QStringLiteral("mlSuperResStrength"), 7.0}
    };
    const QJsonObject root{
        {QStringLiteral("version"), 8},
        {QStringLiteral("ui"), QJsonObject{{QStringLiteral("advancedPanelWidth"), 9999}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("color"), QStringLiteral("not-a-color")},
                     {QStringLiteral("widthPixels"), 999},
                     {QStringLiteral("captureOnExit"), false},
                     {QStringLiteral("dashed"), true},
                     {QStringLiteral("shapeKind"), QStringLiteral("triangle")},
                     {QStringLiteral("textSizePixels"), 999}}},
        {QStringLiteral("currentConfig"), current}
    };
    QVERIFY(WriteJson(path, root));

    const auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->advancedPanelWidth, 1200);
    QCOMPARE(loaded->annotationColor, QStringLiteral("#fff000"));
    QCOMPARE(loaded->annotationWidthPixels, 24);
    QVERIFY(!loaded->annotationCaptureOnExit);
    QVERIFY(loaded->annotationDashed);
    QCOMPARE(loaded->annotationShapeKind, QStringLiteral("rectangle"));
    QCOMPARE(loaded->annotationTextSizePixels, 72);
    QCOMPARE(loaded->currentConfig.blackWhiteThreshold, 1.0f);
    QCOMPARE(loaded->currentConfig.zoomAmount, 1.0f);
    QCOMPARE(loaded->currentConfig.zoomCenterX, 0.0f);
    QCOMPARE(loaded->currentConfig.zoomCenterY, 1.0f);
    QCOMPARE(loaded->currentConfig.blurSigma, 0.1f);
    QCOMPARE(loaded->currentConfig.blurRadius, 0);
    QCOMPARE(loaded->currentConfig.temporalSmoothAlpha, 1.0f);
    QCOMPARE(loaded->currentConfig.spatialUpscaler, 1);
    QCOMPARE(loaded->currentConfig.spatialSharpness, 0.0f);
    QCOMPARE(loaded->currentConfig.rotationQuarterTurns, 3);
    QCOMPARE(loaded->currentConfig.displayColorMode, 16);
    QCOMPARE(loaded->currentConfig.contrast, 4.0f);
    QCOMPARE(loaded->currentConfig.brightness, -1.0f);
    QCOMPARE(loaded->currentConfig.sauvolaStrength, 0.5f);
    QCOMPARE(loaded->currentConfig.binarizationSoftness, 0.25f);
    QCOMPARE(loaded->currentConfig.textPolarityMode, 2);
    QCOMPARE(loaded->currentConfig.strokeWeight, -3);
    QCOMPARE(loaded->currentConfig.claheClipLimit, 8.0f);
    QCOMPARE(loaded->currentConfig.focusThreshold, 0.001f);
    QCOMPARE(loaded->currentConfig.mlSuperResStrength, 1.0f);
}

void SettingsStoreTests::sixteenXZoomPreservesSettingsAndBoundsProfiles()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("zoom.json"));
    PersistentSettings settings;
    settings.currentConfig.zoomEnabled = true;
    settings.currentConfig.zoomAmount = 12.0f;
    settings.currentConfig.zoomCenterX = 0.24f;
    settings.currentConfig.zoomCenterY = 0.37f;
    AdvancedConfig custom = settings.currentConfig;
    custom.id = QStringLiteral("sixteen-x");
    custom.zoomAmount = 16.0f;
    settings.customConfigs.push_back(custom);
    QVERIFY(Save(path, settings));
    auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->currentConfig.zoomAmount, 12.0f);
    QCOMPARE(loaded->currentConfig.zoomCenterX, 0.24f);
    QCOMPARE(loaded->currentConfig.zoomCenterY, 0.37f);
    QCOMPARE(loaded->customConfigs.front().zoomAmount, 16.0f);

    settings.currentConfig.zoomAmount = 16.0f;
    QVERIFY(Save(path, settings));
    loaded = Load(path);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->currentConfig.zoomAmount, 16.0f);
    QCOMPARE(loaded->customConfigs.front().zoomAmount, 16.0f);

    // Both direct serialization and parsed current/custom configurations use
    // the same bound; existing valid preferences need no schema migration.
    settings.currentConfig.zoomAmount = 20.0f;
    QCOMPARE(QJsonDocument::fromJson(Serialize(settings)).object()
                 .value(QStringLiteral("currentConfig")).toObject()
                 .value(QStringLiteral("zoomAmount")).toDouble(), 16.0);
    const QJsonObject outOfRange{{QStringLiteral("zoomAmount"), 16.5}};
    QVERIFY(WriteJson(path, QJsonObject{
        {QStringLiteral("version"), 16},
        {QStringLiteral("currentConfig"), outOfRange},
        {QStringLiteral("customConfigs"), QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("too-large")},
            {QStringLiteral("zoomAmount"), 1000.0}}}}}));
    loaded = Load(path);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->currentConfig.zoomAmount, 16.0f);
    QCOMPARE(loaded->customConfigs.front().zoomAmount, 16.0f);

    QVERIFY(WriteJson(path, QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("zoom"), QJsonObject{
            {QStringLiteral("enabled"), true},
            {QStringLiteral("amount"), 17.0}}}}));
    loaded = Load(path);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->currentConfig.zoomAmount, 16.0f);
}

void SettingsStoreTests::zoomOutPreferencesRoundTripWithoutChangingLegacyFill()
{
    QTemporaryDir dir;QVERIFY(dir.isValid());
    const auto path=dir.filePath("zoom-out.json");
    for (float amount : {0.01f,0.32f,0.75f,1.0f}) {
        PersistentSettings settings;settings.currentConfig.zoomEnabled=true;
        settings.currentConfig.zoomAmount=amount;
        QVERIFY(Save(path,settings));
        const auto loaded=Load(path);QVERIFY(loaded);
        QCOMPARE(loaded->currentConfig.zoomAmount,amount);
        QCOMPARE(loaded->viewportFitMode,ViewportFitModeSetting::Fill);
    }
}

void SettingsStoreTests::rejectsFutureSettingsVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("future.json"));
    QVERIFY(WriteJson(path,
                      QJsonObject{{QStringLiteral("version"), 999}}));

    const LoadResult result = LoadDetailed(path);
    QCOMPARE(result.status, LoadStatus::UnsupportedVersion);
    QVERIFY(!result.settings.has_value());
    QVERIFY(result.error.contains(QStringLiteral("newer")));
}

void SettingsStoreTests::saveCreatesValidBackup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));

    PersistentSettings first;
    first.cameraIndex = 3;
    QVERIFY(Save(path, first));

    PersistentSettings second = first;
    second.cameraIndex = 7;
    QVERIFY(Save(path, second));

    const auto backup = Load(path + QStringLiteral(".backup"));
    QVERIFY(backup.has_value());
    QCOMPARE(backup->cameraIndex, 3);
    const auto current = Load(path);
    QVERIFY(current.has_value());
    QCOMPARE(current->cameraIndex, 7);
}

void SettingsStoreTests::serializedBytesAreWhatSaveWrites()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    PersistentSettings settings;
    settings.assistive.assistantInstructions = QStringLiteral("Line one\nline two\r\n");
    QCOMPARE(SaveDetailed(path, settings), SaveStatus::Saved);
    const QByteArray written = ReadBytes(path);
    QCOMPARE(written, Serialize(settings));
    QCOMPARE(QFileInfo(path).size(), Serialize(settings).size());
    // Every line break is CRLF on disk; the size limit counts the CR bytes.
    QVERIFY(written.contains("\r\n"));
    QCOMPARE(written.count('\n'), written.count("\r\n"));
    const auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->assistive.assistantInstructions,
             settings.assistive.assistantInstructions);
}

void SettingsStoreTests::loadCountsRawBytesAtCap()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    // CRLF padding: a Text-mode read would strip the CRs and undercount.
    QByteArray bytes("{\"version\":16,\"cameraIndex\":2}");
    const qint64 padding = kMaxSettingsFileBytes - bytes.size();
    bytes += QByteArray("\r\n").repeated(static_cast<qsizetype>(padding / 2));
    if (padding % 2 != 0) {
        bytes += ' ';
    }
    QCOMPARE(bytes.size(), kMaxSettingsFileBytes);
    QVERIFY(WriteBytes(path, bytes));
    const LoadResult atCap = LoadDetailed(path);
    QCOMPARE(atCap.status, LoadStatus::Loaded);
    QVERIFY(atCap.settings.has_value());
    QCOMPARE(atCap.settings->cameraIndex, 2);

    QVERIFY(WriteBytes(path, bytes + ' '));
    QCOMPARE(QFileInfo(path).size(), kMaxSettingsFileBytes + 1);
    const LoadResult overCap = LoadDetailed(path);
    QCOMPARE(overCap.status, LoadStatus::TooLarge);
    QVERIFY(!overCap.settings.has_value());
    QVERIFY(overCap.error.contains(QStringLiteral("16 MiB")));
    QVERIFY(!Load(path).has_value());
}

void SettingsStoreTests::saveAtExactCapRoundTripsThroughBackup()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const PersistentSettings atCap = SettingsOfSerializedSize(kMaxSettingsFileBytes, 4);
    QCOMPARE(Serialize(atCap).size(), kMaxSettingsFileBytes);
    QCOMPARE(SaveDetailed(path, atCap), SaveStatus::Saved);
    QCOMPARE(QFileInfo(path).size(), kMaxSettingsFileBytes);
    const LoadResult loaded = LoadDetailed(path);
    QCOMPARE(loaded.status, LoadStatus::Loaded);
    QCOMPARE(loaded.settings->assistive.assistantInstructions,
             atCap.assistive.assistantInstructions);

    // Saving again inspects and rotates the at-cap document into a backup
    // that Load also accepts.
    PersistentSettings next;
    next.cameraIndex = 8;
    QCOMPARE(SaveDetailed(path, next), SaveStatus::Saved);
    const QString backupPath = path + QStringLiteral(".backup");
    QCOMPARE(QFileInfo(backupPath).size(), kMaxSettingsFileBytes);
    const LoadResult backup = LoadDetailed(backupPath);
    QCOMPARE(backup.status, LoadStatus::Loaded);
    QCOMPARE(backup.settings->cameraIndex, 4);
    QCOMPARE(StoredCameraIndex(path), 8);
}

void SettingsStoreTests::saveOverCapLeavesFilesUntouched()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString backupPath = path + QStringLiteral(".backup");
    PersistentSettings first;
    first.cameraIndex = 3;
    QVERIFY(Save(path, first));
    PersistentSettings second;
    second.cameraIndex = 7;
    QVERIFY(Save(path, second));
    const QByteArray primaryBefore = ReadBytes(path);
    const QByteArray backupBefore = ReadBytes(backupPath);
    const QStringList entriesBefore =
        QDir(dir.path()).entryList(QDir::Files | QDir::Hidden, QDir::Name);

    const PersistentSettings overCap =
        SettingsOfSerializedSize(kMaxSettingsFileBytes + 1);
    QCOMPARE(Serialize(overCap).size(), kMaxSettingsFileBytes + 1);
    QCOMPARE(SaveDetailed(path, overCap), SaveStatus::TooLarge);
    QVERIFY(!Save(path, overCap));

    QCOMPARE(ReadBytes(path), primaryBefore);
    QCOMPARE(ReadBytes(backupPath), backupBefore);
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden, QDir::Name),
             entriesBefore);
    QCOMPARE(StoredCameraIndex(path), 7);
    QCOMPARE(StoredCameraIndex(backupPath), 3);
}

void SettingsStoreTests::oversizePrimaryRecoversValidBackup_data()
{
    QTest::addColumn<bool>("withBackup");
    QTest::newRow("valid-backup") << true;
    QTest::newRow("no-backup") << false;
}

void SettingsStoreTests::oversizePrimaryRecoversValidBackup()
{
    QFETCH(bool, withBackup);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString backupPath = path + QStringLiteral(".backup");
    PersistentSettings first;
    first.cameraIndex = 3;
    QVERIFY(Save(path, first));
    PersistentSettings second;
    second.cameraIndex = 7;
    QVERIFY(Save(path, second));
    if (!withBackup) {
        QVERIFY(QFile::remove(backupPath));
    }
    const QByteArray backupBefore = ReadBytes(backupPath);
    // Otherwise valid JSON, one byte over the limit.
    const QByteArray oversize =
        Serialize(SettingsOfSerializedSize(kMaxSettingsFileBytes + 1, 11));
    QVERIFY(WriteBytes(path, oversize));

    SettingsController controller(path);
    const QString notice = controller.TakeStartupNotice();
    QVERIFY2(notice.contains(QStringLiteral("16 MiB limit")), qPrintable(notice));
    QVERIFY2(notice.contains(QStringLiteral("original file was preserved")),
             qPrintable(notice));
    const QStringList sidecars = TooLargeSidecars(dir);
    QCOMPARE(sidecars.size(), 1);
    QCOMPARE(ReadBytes(dir.filePath(sidecars.front())), oversize);

    if (withBackup) {
        QVERIFY2(notice.contains(QStringLiteral("Recovered the last valid settings backup")),
                 qPrintable(notice));
        QCOMPARE(controller.Settings().cameraIndex, 3);
        // Written back from the backup, which itself is not rotated or touched.
        QCOMPARE(ReadBytes(backupPath), backupBefore);
        QCOMPARE(StoredCameraIndex(path), 3);
    } else {
        QVERIFY2(notice.contains(QStringLiteral("safe defaults")), qPrintable(notice));
        QCOMPARE(controller.Settings().cameraIndex, -1);
        QVERIFY(!QFileInfo::exists(path));
    }
    QVERIFY(controller.Save(controller.Settings().currentConfig));
    QCOMPARE(ReadBytes(dir.filePath(sidecars.front())), oversize);
}

void SettingsStoreTests::saveSetsAsideOversizePrimaryWithoutRotation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString backupPath = path + QStringLiteral(".backup");
    PersistentSettings first;
    first.cameraIndex = 3;
    QVERIFY(Save(path, first));
    PersistentSettings second;
    second.cameraIndex = 7;
    QVERIFY(Save(path, second));
    const QByteArray backupBefore = ReadBytes(backupPath);
    // The primary grows past the limit after startup, outside OkuFlow.
    const QByteArray oversize =
        Serialize(SettingsOfSerializedSize(kMaxSettingsFileBytes + 1, 11));
    QVERIFY(WriteBytes(path, oversize));

    PersistentSettings next;
    next.cameraIndex = 9;
    QCOMPARE(SaveDetailed(path, next), SaveStatus::Saved);
    QCOMPARE(ReadBytes(backupPath), backupBefore);
    const QStringList sidecars = TooLargeSidecars(dir);
    QCOMPARE(sidecars.size(), 1);
    QCOMPARE(ReadBytes(dir.filePath(sidecars.front())), oversize);
    QCOMPARE(StoredCameraIndex(path), 9);
}

void SettingsStoreTests::refusedOversizeReplacementKeepsOriginal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString backupPath = path + QStringLiteral(".backup");
    PersistentSettings first;
    first.cameraIndex = 3;
    QVERIFY(Save(path, first));
    PersistentSettings second;
    second.cameraIndex = 7;
    QVERIFY(Save(path, second));
    const QByteArray backupBefore = ReadBytes(backupPath);
    const QByteArray oversize =
        Serialize(SettingsOfSerializedSize(kMaxSettingsFileBytes + 1, 11));
    QVERIFY(WriteBytes(path, oversize));
    const auto cleanup = qScopeGuard([&dir]() {
        // The pre-fix path can move the read-only original into a sidecar.
        for (const QString& name : QDir(dir.path()).entryList(QDir::Files)) {
            QFile::setPermissions(dir.filePath(name), QFileDevice::ReadOwner |
                QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser);
        }
    });
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser));

    PersistentSettings next;
    next.cameraIndex = 9;
    QCOMPARE(SaveDetailed(path, next), SaveStatus::Failed);
    QCOMPARE(ReadBytes(path), oversize);
    QCOMPARE(ReadBytes(backupPath), backupBefore);
    QVERIFY(TooLargeSidecars(dir).isEmpty());
}

void SettingsStoreTests::refusedStagingKeepsBackupGeneration_data()
{
    QTest::addColumn<QString>("readOnlyName");
    QTest::newRow("read-only-primary") << QStringLiteral("settings.json");
    QTest::newRow("read-only-backup") << QStringLiteral("settings.json.backup");
}

void SettingsStoreTests::refusedStagingKeepsBackupGeneration()
{
    QFETCH(QString, readOnlyName);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString backupPath = path + QStringLiteral(".backup");
    PersistentSettings first;
    first.cameraIndex = 3;
    QVERIFY(Save(path, first));
    PersistentSettings second;
    second.cameraIndex = 7;
    QVERIFY(Save(path, second));
    const QByteArray primaryBefore = ReadBytes(path);
    const QByteArray backupBefore = ReadBytes(backupPath);
    const QStringList entriesBefore = FileEntries(dir);
    const auto cleanup = qScopeGuard([&dir]() { MakeFilesWritable(dir); });
    QVERIFY(QFile::setPermissions(dir.filePath(readOnlyName),
                                  QFileDevice::ReadOwner | QFileDevice::ReadUser));

    // A refused save must not rotate the older generation out of the backup
    // first, and the staged replacement must not survive the refusal.
    PersistentSettings next;
    next.cameraIndex = 9;
    QCOMPARE(SaveDetailed(path, next), SaveStatus::Failed);
    QCOMPARE(ReadBytes(path), primaryBefore);
    QCOMPARE(ReadBytes(backupPath), backupBefore);
    QCOMPARE(FileEntries(dir), entriesBefore);
}

void SettingsStoreTests::lockedOversizePrimaryStaysInPlace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString backupPath = path + QStringLiteral(".backup");
    PersistentSettings first;
    first.cameraIndex = 3;
    QVERIFY(Save(path, first));
    PersistentSettings second;
    second.cameraIndex = 7;
    QVERIFY(Save(path, second));
    const QByteArray backupBefore = ReadBytes(backupPath);
    const QByteArray oversize =
        Serialize(SettingsOfSerializedSize(kMaxSettingsFileBytes + 1, 11));
    QVERIFY(WriteBytes(path, oversize));
    const QStringList entriesBefore = FileEntries(dir);

    PersistentSettings next;
    next.cameraIndex = 9;
    {
        // QFile opens without FILE_SHARE_DELETE, so setting the file aside
        // fails after the replacement was staged.
        QFile holder(path);
        QVERIFY(holder.open(QIODevice::ReadOnly));
        QCOMPARE(SaveDetailed(path, next), SaveStatus::Failed);
        QCOMPARE(ReadBytes(path), oversize);
        QCOMPARE(ReadBytes(backupPath), backupBefore);
        QCOMPARE(FileEntries(dir), entriesBefore);
    }

    // Once released, the same save sets the original aside and succeeds.
    QCOMPARE(SaveDetailed(path, next), SaveStatus::Saved);
    const QStringList sidecars = TooLargeSidecars(dir);
    QCOMPARE(sidecars.size(), 1);
    QCOMPARE(ReadBytes(dir.filePath(sidecars.front())), oversize);
    QCOMPARE(ReadBytes(backupPath), backupBefore);
    QCOMPARE(StoredCameraIndex(path), 9);
}

void SettingsStoreTests::commitFailureKeepsCurrentPrimary()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString backupPath = path + QStringLiteral(".backup");
    PersistentSettings first;
    first.cameraIndex = 3;
    QVERIFY(Save(path, first));
    PersistentSettings second;
    second.cameraIndex = 7;
    QVERIFY(Save(path, second));
    const QByteArray primaryBefore = ReadBytes(path);
    const QStringList entriesBefore = FileEntries(dir);

    // Staging and the backup copy succeed; the final replace fails because
    // the open handle does not share deletion.
    QFile holder(path);
    QVERIFY(holder.open(QIODevice::ReadOnly));
    PersistentSettings next;
    next.cameraIndex = 9;
    QCOMPARE(SaveDetailed(path, next), SaveStatus::Failed);
    holder.close();
    QCOMPARE(ReadBytes(path), primaryBefore);
    // The backup was rotated before the commit, so it now holds the unchanged
    // current document; QSaveFile removed its temporary file.
    QCOMPARE(ReadBytes(backupPath), primaryBefore);
    QCOMPARE(FileEntries(dir), entriesBefore);
}

void SettingsStoreTests::missingPrimaryRecoversBackup_data()
{
    QTest::addColumn<QString>("backupState");
    QTest::newRow("first-run") << QStringLiteral("none");
    QTest::newRow("valid-backup") << QStringLiteral("valid");
    QTest::newRow("corrupt-backup") << QStringLiteral("corrupt");
}

void SettingsStoreTests::missingPrimaryRecoversBackup()
{
    QFETCH(QString, backupState);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString backupPath = path + QStringLiteral(".backup");
    if (backupState != QStringLiteral("none")) {
        PersistentSettings first;
        first.cameraIndex = 3;
        QVERIFY(Save(path, first));
        PersistentSettings second;
        second.cameraIndex = 7;
        QVERIFY(Save(path, second));
        // A save interrupted after setting an oversized primary aside leaves
        // only the backup behind.
        QVERIFY(QFile::remove(path));
        if (backupState == QStringLiteral("corrupt")) {
            QVERIFY(WriteBytes(backupPath, "{ this is not json"));
        }
    }
    const QByteArray backupBefore = ReadBytes(backupPath);

    SettingsController controller(path);
    const QString notice = controller.TakeStartupNotice();
    if (backupState == QStringLiteral("valid")) {
        QVERIFY2(notice.contains(QStringLiteral("settings file was missing")),
                 qPrintable(notice));
        QVERIFY2(notice.contains(QStringLiteral("Recovered the last valid settings backup")),
                 qPrintable(notice));
        QCOMPARE(controller.Settings().cameraIndex, 3);
        QCOMPARE(StoredCameraIndex(path), 3);
    } else {
        QVERIFY2(notice.isEmpty(), qPrintable(notice));
        QCOMPARE(controller.Settings().cameraIndex, -1);
        QCOMPARE(controller.Settings().selectedPresetId, DefaultPresetId());
        QVERIFY(!QFileInfo::exists(path));
    }
    QCOMPARE(ReadBytes(backupPath), backupBefore);
}

void SettingsStoreTests::controllerReportsSizeLimitAndKeepsValidPrimary()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString backupPath = path + QStringLiteral(".backup");
    PersistentSettings first;
    first.cameraIndex = 3;
    QVERIFY(Save(path, first));
    PersistentSettings second;
    second.cameraIndex = 7;
    QVERIFY(Save(path, second));
    const QByteArray primaryBefore = ReadBytes(path);
    const QByteArray backupBefore = ReadBytes(backupPath);

    SettingsController controller(path);
    QCOMPARE(controller.Settings().cameraIndex, 7);
    controller.MutableSettings().assistive.assistantInstructions =
        QString(static_cast<qsizetype>(kMaxSettingsFileBytes), QLatin1Char('a'));
    QVERIFY(!controller.Save(controller.Settings().currentConfig));
    QVERIFY2(controller.LastError().contains(QStringLiteral("16 MiB settings file limit")),
             qPrintable(controller.LastError()));
    QCOMPARE(ReadBytes(path), primaryBefore);
    QCOMPARE(ReadBytes(backupPath), backupBefore);
    QVERIFY(TooLargeSidecars(dir).isEmpty());

    controller.MutableSettings().assistive.assistantInstructions = QStringLiteral("Short.");
    QVERIFY(controller.Save(controller.Settings().currentConfig));
    QVERIFY(controller.LastError().isEmpty());
    const auto saved = Load(path);
    QVERIFY(saved.has_value());
    QCOMPARE(saved->cameraIndex, 7);
    QCOMPARE(saved->assistive.assistantInstructions, QStringLiteral("Short."));
}

void SettingsStoreTests::committedSnapshotSeparatesLiveAndFailedSave()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("settings.json"));
    bool refuse = false;
    SettingsController::StorageOperations storage;
    storage.saveSettings = [&](const QString& target, const PersistentSettings& value) {
        return refuse ? SaveStatus::Failed : SaveDetailed(target,value);
    };
    SettingsController controller(path,storage);
    QVERIFY(!controller.LastCommittedSettings()); // defaults have no file yet
    auto initial = controller.Settings().currentConfig;
    initial.zoomAmount = 2.0f;
    controller.MutableSettings().language = QStringLiteral("pt_br");
    QVERIFY(controller.Save(initial));
    QVERIFY(controller.LastCommittedSettings());
    QCOMPARE(controller.LastCommittedSettings()->language,QStringLiteral("pt-BR"));
    const auto committedBytes = ReadBytes(path);
    QCOMPARE(Serialize(*controller.LastCommittedSettings()),committedBytes);
    controller.MutableSettings().currentConfig.zoomAmount = 4.0f;
    controller.MutableSettings().assistive.ttsRate = 0.5;
    QCOMPARE(controller.LastCommittedSettings()->currentConfig.zoomAmount,2.0f);
    refuse = true;
    auto changed = controller.Settings().currentConfig;
    changed.zoomAmount = 8.0f;
    QVERIFY(!controller.Save(changed));
    QCOMPARE(controller.Settings().currentConfig.zoomAmount,8.0f);
    QCOMPARE(controller.LastCommittedSettings()->currentConfig.zoomAmount,2.0f);
    QCOMPARE(Serialize(*controller.LastCommittedSettings()),committedBytes);
    QCOMPARE(ReadBytes(path),committedBytes);
    refuse = false;
    QVERIFY(controller.Save(changed));
    QCOMPARE(controller.LastCommittedSettings()->currentConfig.zoomAmount,8.0f);
    QCOMPARE(controller.LastCommittedSettings()->assistive.ttsRate,0.5);
    QCOMPARE(Serialize(*controller.LastCommittedSettings()),ReadBytes(path));
    SettingsController reopened(path);
    QVERIFY(reopened.LastCommittedSettings());
    QCOMPARE(Serialize(*reopened.LastCommittedSettings()),ReadBytes(path));
}

void SettingsStoreTests::committedSnapshotTracksRecoveredBackup()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("settings.json"));
    PersistentSettings backup; backup.currentConfig.zoomAmount = 3.0f;
    QVERIFY(Save(path+QStringLiteral(".backup"),backup));
    SettingsController::StorageOperations storage;
    storage.saveSettings = [](const QString&,const PersistentSettings&) { return SaveStatus::Failed; };
    SettingsController controller(path,storage);
    QVERIFY(controller.LastCommittedSettings());
    QCOMPARE(controller.LastCommittedSettings()->currentConfig.zoomAmount,3.0f);
    QVERIFY(!QFileInfo::exists(path)); // snapshot describes last valid persisted backup
    controller.MutableSettings().currentConfig.zoomAmount = 5.0f;
    QCOMPARE(controller.LastCommittedSettings()->currentConfig.zoomAmount,3.0f);
}

void SettingsStoreTests::committedSnapshotTracksPartialCredentialCommit()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("settings.json"));
    PersistentSettings initial;
    initial.assistive.vlmCredentialId = QStringLiteral("snapshot-fixture");
    QVERIFY(Save(path,initial));
    bool failRemoval = true, failCleanup = false;
    int saves = 0;
    SettingsController::StorageOperations storage;
    storage.readSecret = [](const QString&) {
        return ProtectedSecretResult{ProtectedSecretResult::Status::Found,QStringLiteral("fixture-only-secret"),{}};
    };
    storage.writeSecret = [](const QString&,const QString&,QString*) { return true; };
    storage.removeSecret = [&](const QString&,QString* error) {
        if (failRemoval) { *error = QStringLiteral("fixture removal failed"); return false; }
        return true;
    };
    storage.saveSettings = [&](const QString& target,const PersistentSettings& value) {
        ++saves;
        if (failCleanup && value.assistive.pendingCredentialRemovals.isEmpty()) return SaveStatus::Failed;
        return SaveDetailed(target,value);
    };
    SettingsController controller(path,storage);
    QVERIFY(controller.LastCommittedSettings());
    QVERIFY(controller.LastCommittedSettings()->assistive.vlmApiKey.isEmpty());
    QVERIFY(!controller.Settings().assistive.vlmApiKey.isEmpty());
    controller.MutableSettings().assistive.vlmApiKey.clear();
    auto changed = controller.Settings().currentConfig; changed.zoomAmount = 6.0f;
    QVERIFY(!controller.Save(changed)); // settings committed, later removal failed
    QCOMPARE(saves,1);
    QCOMPARE(controller.LastCommittedSettings()->currentConfig.zoomAmount,6.0f);
    QVERIFY(controller.LastCommittedSettings()->assistive.vlmCredentialId.isEmpty());
    QCOMPARE(controller.LastCommittedSettings()->assistive.pendingCredentialRemovals.size(),1);
    QCOMPARE(Serialize(*controller.LastCommittedSettings()),ReadBytes(path));
    failRemoval = false; failCleanup = true;
    QVERIFY(!controller.Save(changed)); // initial commit succeeded, cleanup commit refused
    QCOMPARE(saves,3);
    QCOMPARE(controller.LastCommittedSettings()->assistive.pendingCredentialRemovals.size(),1);
    QCOMPARE(Serialize(*controller.LastCommittedSettings()),ReadBytes(path));
    failCleanup = false;
    QVERIFY(controller.Save(changed));
    QVERIFY(controller.LastCommittedSettings()->assistive.pendingCredentialRemovals.isEmpty());
    QVERIFY(controller.LastCommittedSettings()->assistive.vlmApiKey.isEmpty());
    QCOMPARE(Serialize(*controller.LastCommittedSettings()),ReadBytes(path));
}

void SettingsStoreTests::capHoldsDocumentedCapacity()
{
    // Index entries are created with at most 60 title and 160 preview UTF-16
    // units; three-byte UTF-8 characters are the realistic worst case.
    PersistentSettings settings;
    const QString title(60, QChar(0x20AC));
    const QString preview(160, QChar(0x20AC));
    for (int i = 0; i < 10000; ++i) {
        settings.codexConversations.push_back(
            {QUuid::createUuid().toString(QUuid::WithoutBraces), title, preview,
             1790000000 + i, 1790000000 + i});
    }
    for (int i = 0; i < 500; ++i) {
        AdvancedConfig config = MakePopulatedConfig();
        config.id = QStringLiteral("custom-config-%1").arg(1790000000000LL + i);
        config.name = QStringLiteral("Custom quick option %1").arg(i);
        config.description = QStringLiteral("Custom quick option created from Advanced Tuning.");
        settings.customConfigs.push_back(config);
        settings.customPresets.push_back(
            {QStringLiteral("custom-preset-%1").arg(1790000000000LL + i),
             config.name, config.description, config.id, false});
    }
    const qint64 bytes = Serialize(settings).size();
    qInfo("10000 max-length conversations + 500 custom quick modes: %lld bytes (limit %lld)",
          bytes, kMaxSettingsFileBytes);
    QVERIFY(bytes < kMaxSettingsFileBytes);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    QCOMPARE(SaveDetailed(path, settings), SaveStatus::Saved);
    const auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->codexConversations.size(), settings.codexConversations.size());
    QCOMPARE(loaded->codexConversations.back().preview, preview);
    QCOMPARE(loaded->customConfigs.size(), settings.customConfigs.size());
    QCOMPARE(loaded->customPresets.size(), settings.customPresets.size());
}

void SettingsStoreTests::credentialDeletionRetries_data()
{
    QTest::addColumn<bool>("failSave");
    QTest::addColumn<bool>("restart");
    QTest::newRow("settings-save-failure-same-controller") << true << false;
    QTest::newRow("settings-save-failure-restart") << true << true;
    QTest::newRow("secret-remove-failure-same-controller") << false << false;
    QTest::newRow("secret-remove-failure-restart") << false << true;
}

void SettingsStoreTests::credentialDeletionRetries()
{
    QFETCH(bool, failSave);
    QFETCH(bool, restart);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    PersistentSettings initial;
    initial.assistive.vlmCredentialId = QStringLiteral("fixture-secret-id");
    QVERIFY(Save(path, initial));
    int readCalls = 0;
    int removeCalls = 0;
    int saveCalls = 0;
    bool secretExists = true;
    SettingsController::StorageOperations storage;
    storage.readSecret = [&](const QString& id) {
        ++readCalls;
        if (id != QStringLiteral("fixture-secret-id"))
            return ProtectedSecretResult{ProtectedSecretResult::Status::Error, {}, QStringLiteral("wrong fixture id")};
        return ProtectedSecretResult{ProtectedSecretResult::Status::Found,
                                     QStringLiteral("fixture-secret"), {}};
    };
    storage.writeSecret = [](const QString&, const QString&, QString*) { return false; };
    storage.removeSecret = [&](const QString& id, QString* error) {
        ++removeCalls;
        if (id != QStringLiteral("fixture-secret-id")) return false;
        if (!failSave && removeCalls == 1) {
            *error = QStringLiteral("fixture removal failure");
            return false;
        }
        secretExists = false;
        return true;
    };
    storage.saveSettings = [&](const QString& target, const PersistentSettings& value) {
        ++saveCalls;
        if (!value.assistive.vlmCredentialId.isEmpty()) return SaveStatus::Failed;
        if (failSave && saveCalls == 1) return SaveStatus::Failed;
        return SaveDetailed(target, value);
    };
    SettingsController controller(path, storage);
    QCOMPARE(readCalls, 1);
    controller.MutableSettings().assistive.vlmApiKey.clear();
    QVERIFY(!controller.Save(controller.Settings().currentConfig));
    QVERIFY(secretExists);
    QCOMPARE(removeCalls, failSave ? 0 : 1);
    QCOMPARE(controller.Settings().assistive.vlmCredentialId,
             failSave ? QStringLiteral("fixture-secret-id") : QString());
    std::unique_ptr<SettingsController> restarted;
    SettingsController* retryController = &controller;
    if (restart) {
        restarted = std::make_unique<SettingsController>(path, storage);
        restarted->MutableSettings().assistive.vlmApiKey.clear();
        retryController = restarted.get();
    }
    QVERIFY(retryController->Save(retryController->Settings().currentConfig));
    QVERIFY(!secretExists);
    QCOMPARE(removeCalls, failSave ? 1 : 2);
    QVERIFY(retryController->Settings().assistive.vlmCredentialId.isEmpty());
    const auto loaded = Load(path);
    QVERIFY(loaded.has_value());
    QVERIFY(loaded->assistive.vlmCredentialId.isEmpty());
    QVERIFY(loaded->assistive.pendingCredentialRemovals.isEmpty());
    QVERIFY(retryController->Save(retryController->Settings().currentConfig));
    QCOMPARE(removeCalls, failSave ? 1 : 2);
}

void SettingsStoreTests::pendingDeletionNeverReusesReplacementKey_data()
{
    QTest::addColumn<bool>("restart");
    QTest::newRow("same-controller-retry") << false;
    QTest::newRow("restart-after-failed-publication") << true;
}

void SettingsStoreTests::pendingDeletionNeverReusesReplacementKey()
{
    QFETCH(bool, restart);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.json"));
    const QString oldId = ProtectedSecretStore::DefaultVlmCredentialId();
    PersistentSettings initial;
    initial.assistive.pendingCredentialRemovals = {oldId};
    QVERIFY(Save(path, initial));
    QMap<QString, QString> secrets;
    secrets.insert(oldId, QStringLiteral("old fixture key"));
    QStringList removedIds;
    QString replacementId;
    bool failPublication = true;
    SettingsController::StorageOperations storage;
    storage.readSecret = [&](const QString& id) {
        if (!secrets.contains(id)) return ProtectedSecretResult{};
        return ProtectedSecretResult{ProtectedSecretResult::Status::Found, secrets.value(id), {}};
    };
    storage.writeSecret = [&](const QString& id, const QString& key, QString*) {
        replacementId = id;
        secrets.insert(id, key);
        return true;
    };
    storage.removeSecret = [&](const QString& id, QString*) {
        removedIds.append(id);
        secrets.remove(id);
        return true;
    };
    storage.saveSettings = [&](const QString& target, const PersistentSettings& value) {
        if (failPublication) return SaveStatus::Failed;
        return SaveDetailed(target, value);
    };
    SettingsController controller(path, storage);
    controller.MutableSettings().assistive.vlmApiKey = QStringLiteral("new fixture key");
    QVERIFY(!controller.Save(controller.Settings().currentConfig));
    QVERIFY(!replacementId.isEmpty());
    QVERIFY(replacementId != oldId);
    QCOMPARE(secrets.value(oldId), QStringLiteral("old fixture key"));
    QCOMPARE(secrets.value(replacementId), QStringLiteral("new fixture key"));
    QVERIFY(removedIds.isEmpty());
    const QString firstReplacementId = replacementId;
    QVERIFY(!controller.Save(controller.Settings().currentConfig));
    QCOMPARE(replacementId, firstReplacementId);
    QCOMPARE(secrets.size(), 2);
    const auto failedDisk = Load(path);
    QVERIFY(failedDisk.has_value());
    QCOMPARE(failedDisk->assistive.pendingCredentialRemovals, QStringList{oldId});
    QVERIFY(failedDisk->assistive.vlmCredentialId.isEmpty());
    failPublication = false;
    if (restart) {
        SettingsController restarted(path, storage);
        QVERIFY(restarted.Save(restarted.Settings().currentConfig));
    } else {
        QVERIFY(controller.Save(controller.Settings().currentConfig));
        QCOMPARE(controller.Settings().assistive.vlmCredentialId, replacementId);
    }
    QCOMPARE(removedIds, QStringList{oldId});
    QVERIFY(!secrets.contains(oldId));
    QCOMPARE(secrets.value(replacementId), QStringLiteral("new fixture key"));
    const auto savedDisk = Load(path);
    QVERIFY(savedDisk.has_value());
    QVERIFY(savedDisk->assistive.pendingCredentialRemovals.isEmpty());
    QCOMPARE(savedDisk->assistive.vlmCredentialId, restart ? QString() : replacementId);
}

void SettingsStoreTests::credentialRotationKeepsRecoverableEndpointBinding_data()
{
    QTest::addColumn<bool>("commitReplacement");
    QTest::addColumn<bool>("changeKey");
    QTest::newRow("failed-settings-publication") << false << true;
    QTest::newRow("recover-backup-after-rotation") << true << true;
    QTest::newRow("endpoint-only-failed-publication") << false << false;
}

void SettingsStoreTests::credentialRotationKeepsRecoverableEndpointBinding()
{
    QFETCH(bool, commitReplacement);
    QFETCH(bool, changeKey);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("settings.json"));
    const auto oldId = QStringLiteral("fixture-original-key-id");
    const auto oldKey = QStringLiteral("fixture-key-a");
    const auto newKey = changeKey ? QStringLiteral("fixture-key-b") : oldKey;
    PersistentSettings original;
    original.assistive.aiProvider = QStringLiteral("vlm");
    original.assistive.vlmApiUrl = QStringLiteral("https://endpoint-a.invalid/v1");
    original.assistive.vlmCredentialId = oldId;
    QVERIFY(Save(path, original));
    QMap<QString, QString> secrets{{oldId, oldKey}};
    int writes = 0;
    SettingsController::StorageOperations storage;
    storage.readSecret = [&](const QString& id) {
        return secrets.contains(id)
            ? ProtectedSecretResult{ProtectedSecretResult::Status::Found, secrets.value(id), {}}
            : ProtectedSecretResult{};
    };
    storage.writeSecret = [&](const QString& id, const QString& value, QString*) {
        ++writes;
        secrets.insert(id, value);
        return true;
    };
    storage.removeSecret = [&](const QString& id, QString*) { secrets.remove(id); return true; };
    storage.saveSettings = [&](const QString& target, const PersistentSettings& value) {
        return commitReplacement ? SaveDetailed(target, value) : SaveStatus::Failed;
    };
    SettingsController controller(path, storage);
    controller.MutableSettings().assistive.vlmApiUrl = QStringLiteral("https://endpoint-b.invalid/v1");
    controller.MutableSettings().assistive.vlmApiKey = newKey;
    QCOMPARE(controller.Save(controller.Settings().currentConfig), commitReplacement);
    const auto replacementId = controller.Settings().assistive.vlmCredentialId;
    QVERIFY(replacementId != oldId);
    QCOMPARE(secrets.value(oldId), oldKey);
    QCOMPARE(secrets.value(replacementId), newKey);
    QCOMPARE(writes, 1);
    if (!commitReplacement) {
        QVERIFY(!controller.Save(controller.Settings().currentConfig));
        QCOMPARE(controller.Settings().assistive.vlmCredentialId, replacementId);
        QCOMPARE(writes, 1); // retry the same staged immutable generation
    } else {
        QVERIFY(WriteBytes(path, "{ corrupt primary"));
    }
    SettingsController restarted(path, storage);
    QCOMPARE(restarted.Settings().assistive.vlmApiUrl, original.assistive.vlmApiUrl);
    QCOMPARE(restarted.Settings().assistive.vlmCredentialId, oldId);
    QCOMPARE(restarted.Settings().assistive.vlmApiKey, oldKey);
}

void SettingsStoreTests::unchangedCredentialBindingDoesNotCreateGenerations()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("settings.json"));
    PersistentSettings original;
    original.assistive.vlmApiUrl = QStringLiteral("https://endpoint-a.invalid/v1");
    original.assistive.vlmCredentialId = QStringLiteral("fixture-key-id");
    QVERIFY(Save(path, original));
    int writes = 0;
    SettingsController::StorageOperations storage;
    storage.readSecret = [](const QString&) {
        return ProtectedSecretResult{ProtectedSecretResult::Status::Found,
                                     QStringLiteral("fixture-key-a"), {}};
    };
    storage.writeSecret = [&](const QString&, const QString&, QString*) { ++writes; return true; };
    storage.removeSecret = [](const QString&, QString*) { return true; };
    storage.saveSettings = SaveDetailed;
    SettingsController controller(path, storage);
    QVERIFY(controller.Save(controller.Settings().currentConfig));
    QVERIFY(controller.Save(controller.Settings().currentConfig));
    QCOMPARE(controller.Settings().assistive.vlmCredentialId, original.assistive.vlmCredentialId);
    QCOMPARE(writes, 0);
}

void SettingsStoreTests::ignoresPlaintextApiKeyField()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    const QString secret = QStringLiteral("legacy-plaintext-secret");
    QVERIFY(WriteJson(
        path,
        QJsonObject{
            {QStringLiteral("version"), 15},
            {QStringLiteral("assistive"),
             QJsonObject{
                 {QStringLiteral("vlmApiKey"), secret}}}}));

    SettingsController controller(path);
    QVERIFY(controller.Settings().assistive.vlmApiKey.isEmpty());
    QVERIFY(controller.Settings().assistive.vlmCredentialId.isEmpty());
}

void SettingsStoreTests::equivalenceUsesUiTolerances()
{
    AdvancedConfig base = MakePopulatedConfig();
    AdvancedConfig within = base;
    within.zoomAmount += 0.004f;
    within.focusThreshold += 0.0004f;
    QVERIFY(AreConfigsEquivalent(base, within));

    AdvancedConfig outside = base;
    outside.zoomAmount += 0.02f;
    QVERIFY(!AreConfigsEquivalent(base, outside));

    outside = base;
    // Legacy per-profile rotation is ignored: orientation now lives at the
    // top level of PersistentSettings, so it must not split a preset match.
    AdvancedConfig rotated = base;
    rotated.rotationQuarterTurns = (base.rotationQuarterTurns + 1) % 4;
    QVERIFY(AreConfigsEquivalent(base, rotated));

    outside.colorScheme.stops[1] = QColor(QStringLiteral("#010203"));
    QVERIFY(!AreConfigsEquivalent(base, outside));
}

void SettingsStoreTests::quickModeSaveIsDurableBeforeReturn()
{
    QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("settings.json");
    SettingsController controller(path);auto live=controller.Settings().currentConfig;live.zoomAmount=3.25f;
    QVERIFY(controller.PromoteCurrentConfig(live,"Durable mode"));
    // Reopen without a destructor/shutdown save or a separate Save action.
    SettingsController reopened(path);
    QCOMPARE(reopened.Settings().customPresets.size(),std::size_t(1));
    const auto preset=reopened.Settings().customPresets.front();
    const auto restored=reopened.ResolvePreset(preset.id);QVERIFY(restored);
    QCOMPARE(restored->zoomAmount,3.25f);QCOMPARE(reopened.Settings().selectedPresetId,preset.id);
}

void SettingsStoreTests::failedQuickModeSavePreservesOldModesAndLiveSettings()
{
    QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("settings.json");bool refuse=false;
    SettingsController::StorageOperations storage;
    storage.saveSettings=[&](const QString& target,const PersistentSettings& value) {return refuse ? SaveStatus::Failed : SaveDetailed(target,value);};
    SettingsController controller(path,storage);QVERIFY(controller.Save(controller.Settings().currentConfig));
    const auto before=Serialize(controller.Settings());const auto bytes=ReadBytes(path);
    auto live=controller.Settings().currentConfig;live.zoomAmount=4.5f;refuse=true;
    QVERIFY(!controller.PromoteCurrentConfig(live,"Must not look saved"));
    QCOMPARE(Serialize(controller.Settings()),before);QCOMPARE(ReadBytes(path),bytes);
    QVERIFY(!controller.LastError().isEmpty());
    refuse=false;QVERIFY(controller.PromoteCurrentConfig(live,"Retry durable mode"));
    SettingsController reopened(path);QCOMPARE(reopened.Settings().customPresets.size(),std::size_t(1));
}

void SettingsStoreTests::presetIdentitySurvivesApplyTimeMutations()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    SettingsController controller(path);

    // A promoted custom preset is unique, so re-derivation cannot hide
    // behind a coincidental built-in match.
    const auto promoted = controller.PromoteCurrentConfig(
        MakePopulatedConfig(), QStringLiteral("Apply Mutation Fixture"));
    QVERIFY(promoted);const PresetDefinition preset=*promoted;
    const auto resolved = controller.ResolvePreset(preset.id);
    QVERIFY(resolved.has_value());

    // Simulate beyond-tolerance values a preset apply can produce: a
    // SuperRes minimum-zoom raise on a low-zoom preset and SuperRes forced
    // off in a build without the Text-SR adapter.
    AdvancedConfig live = *resolved;
    live.rotationQuarterTurns = (live.rotationQuarterTurns + 1) % 4;
    live.mlSuperResEnabled = false;
    live.zoomAmount += 0.67f;
    QVERIFY(!AreConfigsEquivalent(live, *resolved));
    // Re-deriving identity from the mutated live config loses the preset.
    QVERIFY(controller.MatchPreset(live, false) != preset.id);
    QVERIFY(controller.Settings().selectedPresetId != preset.id);

    // The clicked id, preserved through apply, stays authoritative.
    controller.MutableSettings().selectedPresetId = preset.id;
    QCOMPARE(controller.MatchPreset(live, true), preset.id);
    QCOMPARE(controller.Settings().selectedPresetId, preset.id);
    QCOMPARE(controller.Settings().currentConfig.id, preset.configId);

    // Every built-in preset resolves, and a legacy-rotation-only difference
    // neither breaks equivalence nor preserved identity.
    for (const PresetDefinition& builtIn : BuiltInPresets()) {
        const auto config = controller.ResolvePreset(builtIn.id);
        QVERIFY2(config.has_value(), qPrintable(builtIn.id));
        AdvancedConfig rotated = *config;
        rotated.rotationQuarterTurns = (rotated.rotationQuarterTurns + 1) % 4;
        QVERIFY2(AreConfigsEquivalent(*config, rotated),
                 qPrintable(builtIn.id));
        controller.MutableSettings().selectedPresetId = builtIn.id;
        QCOMPARE(controller.MatchPreset(rotated, true), builtIn.id);
    }
}

void SettingsStoreTests::danglingPresetResolutionLeavesSelectionUnchanged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    SettingsController controller(path);
    const QString selectedBefore = controller.Settings().selectedPresetId;
    QVERIFY(!selectedBefore.isEmpty());

    const PresetDefinition dangling{
        QStringLiteral("custom-preset-dangling-fixture"),
        QStringLiteral("Dangling Fixture"),
        QStringLiteral("Points at a config that no longer exists."),
        QStringLiteral("custom-config-missing-fixture"),
        false};
    controller.MutableSettings().customPresets.push_back(dangling);

    QVERIFY(!controller.ResolvePreset(dangling.id).has_value());
    QVERIFY(!controller.ResolvePreset(QString()).has_value());
    QCOMPARE(controller.Settings().selectedPresetId, selectedBefore);

    // Preserved matching must not honor an unresolvable selection; the app
    // rolls the carousel/list back to the still-applied config instead.
    controller.MutableSettings().selectedPresetId = dangling.id;
    QVERIFY(controller.MatchPreset(controller.Settings().currentConfig, true) !=
            dangling.id);
    QVERIFY(controller.Settings().selectedPresetId != dangling.id);
}

} // namespace okuflow::settings

QTEST_GUILESS_MAIN(okuflow::settings::SettingsStoreTests)

#include "settings_store_tests.moc"
