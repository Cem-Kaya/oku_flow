#include "openzoom/app/color_schemes.hpp"
#include "openzoom/app/protected_secret_store.hpp"
#include "openzoom/app/settings_controller.hpp"
#include "openzoom/app/settings_store.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

namespace openzoom::settings {

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
    config.ocrAssistEnabled = true;
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

} // namespace

class SettingsStoreTests : public QObject {
    Q_OBJECT

private slots:
    void defaultsUseTeraLow();
    void roundTripPreservesAdvancedConfig();
    void loadsRemovedGlobalCompatibilityForMigration();
    void migratesLegacyV1();
    void rejectsCorruptJson();
    void rejectsFutureSettingsVersion();
    void saveCreatesValidBackup();
    void protectedSecretRoundTrip();
    void ignoresPlaintextApiKeyField();
    void clampsOutOfRangeValues();
    void equivalenceUsesUiTolerances();
};

void SettingsStoreTests::defaultsUseTeraLow()
{
    const PersistentSettings settings;
    QCOMPARE(settings.assistive.codexModel, QStringLiteral("gpt-5.6-tera"));
    QCOMPARE(settings.assistive.codexReasoningEffort, QStringLiteral("low"));
}

void SettingsStoreTests::roundTripPreservesAdvancedConfig()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nested/settings.json"));

    PersistentSettings expected;
    expected.userDataRoot = QStringLiteral("D:/OpenZoom Data");
    expected.cameraIndex = 4;
    expected.microphoneEndpointId =
        QStringLiteral("{0.0.1.00000000}.test-microphone");
    expected.cameraFormatStableId =
        QStringLiteral("{test-subtype}_1920x1080@60/1");
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
    expected.assistive.vlmCredentialId = QStringLiteral("OpenZoom/Test Round Trip");
    expected.assistive.vlmModel = QStringLiteral("vision");
    expected.assistive.vlmPrompt = QStringLiteral("Describe.");
    expected.assistive.tesseractPath = QStringLiteral("C:/ocr/tesseract.exe");
    expected.assistive.ocrLanguage = QStringLiteral("tur");
    expected.assistive.ttsEngine = QStringLiteral("winrt");
    expected.assistive.ttsVoiceName = QStringLiteral("Natural Voice");
    expected.assistive.ttsVoiceLocale = QStringLiteral("en-US");
    expected.assistive.ttsRate = 0.4;
    expected.assistive.lectureNotesEnabled = false;
    expected.codexConversations.push_back(
        {QStringLiteral("thread-1"), QStringLiteral("Title"), QStringLiteral("Preview"), 10, 20});
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

    QFile savedFile(path);
    QVERIFY(savedFile.open(QIODevice::ReadOnly));
    const QByteArray savedJson = savedFile.readAll();
    QVERIFY(!savedJson.contains("secret"));
    QVERIFY(!savedJson.contains("\"vlmApiKey\""));

    QCOMPARE(loaded->cameraIndex, expected.cameraIndex);
    QCOMPARE(loaded->microphoneEndpointId,
             expected.microphoneEndpointId);
    QCOMPARE(loaded->userDataRoot, expected.userDataRoot);
    QCOMPARE(loaded->cameraFormatStableId, expected.cameraFormatStableId);
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
    QCOMPARE(loaded->recordingCanvasMode, expected.recordingCanvasMode);
    QCOMPARE(loaded->uiSectionStates, expected.uiSectionStates);
    QCOMPARE(loaded->assistiveOverlayGeometry, expected.assistiveOverlayGeometry);
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
    QCOMPARE(loaded->customConfigs.size(), expected.customConfigs.size());
    QCOMPARE(loaded->customPresets.size(), expected.customPresets.size());
    QVERIFY(AreConfigsEquivalent(loaded->customConfigs.front(), expected.customConfigs.front()));
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

void SettingsStoreTests::protectedSecretRoundTrip()
{
    const QString credentialId =
        QStringLiteral("OpenZoom/Test/%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QString error;
    QVERIFY2(ProtectedSecretStore::Write(
                 credentialId, QStringLiteral("test-secret-value"), &error),
             qPrintable(error));

    const ProtectedSecretResult read =
        ProtectedSecretStore::Read(credentialId);
    QCOMPARE(read.status, ProtectedSecretResult::Status::Found);
    QCOMPARE(read.value, QStringLiteral("test-secret-value"));

    QVERIFY2(ProtectedSecretStore::Remove(credentialId, &error),
             qPrintable(error));
    QCOMPARE(ProtectedSecretStore::Read(credentialId).status,
             ProtectedSecretResult::Status::NotFound);
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
    outside.colorScheme.stops[1] = QColor(QStringLiteral("#010203"));
    QVERIFY(!AreConfigsEquivalent(base, outside));
}

} // namespace openzoom::settings

QTEST_GUILESS_MAIN(openzoom::settings::SettingsStoreTests)

#include "settings_store_tests.moc"
