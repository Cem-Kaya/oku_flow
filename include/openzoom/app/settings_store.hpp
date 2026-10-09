#pragma once

#include <QRect>
#include <QString>
#include <QMap>

#include "openzoom/app/color_schemes.hpp"
#include "openzoom/common/recording_contract.hpp"

#include <optional>
#include <vector>

namespace openzoom::settings {

enum class ViewportRateMode {
    AutoUpTo120 = 0,
    Fps60 = 1,
    Fps90 = 2,
    Fps120 = 3,
    MatchDisplay = 4,
};

enum class ViewportFitModeSetting {
    Fill = 0,
    Fit = 1,
};

enum class CameraAccelerationMode {
    Automatic = 0,
    ForceAccelerated = 1,
    ForceCompatibility = 2,
};

struct CameraAccelerationSetting {
    CameraAccelerationMode mode{CameraAccelerationMode::Automatic};
    bool automaticFallback{false};
    QString reason;
    QString decidedOn;
    // Diagnostic ladder result from the last successful accelerated session:
    // "zeroCopy", "acceleratedCopy", or "compatibility".
    QString lastRung;
};

struct AdvancedConfig {
    QString id;
    QString name;
    QString description;
    bool blackWhiteEnabled{false};
    float blackWhiteThreshold{0.5f};
    bool zoomEnabled{false};
    float zoomAmount{1.0f};
    float zoomCenterX{0.5f};
    float zoomCenterY{0.5f};
    bool blurEnabled{false};
    float blurSigma{1.0f};
    int blurRadius{3};
    bool temporalSmoothEnabled{false};
    float temporalSmoothAlpha{0.25f};
    bool spatialSharpenEnabled{false};
    int spatialUpscaler{1};
    float spatialSharpness{0.25f};
    bool debugView{false};
    bool focusMarker{false};
    int rotationQuarterTurns{0};
    bool vlmAssistEnabled{false};
    bool assistiveOverlayEnabled{true};
    bool stabilizationEnabled{false};
    int displayColorMode{0};
    color_schemes::ColorScheme colorScheme{};
    float contrast{1.0f};
    float brightness{0.0f};
    bool keystoneEnabled{false};
    bool autoContrastEnabled{false};
    float autoContrastStrength{0.7f};
    bool autoTextClarityEnabled{false};
    bool backgroundFlattenEnabled{false};
    float backgroundFlattenStrength{0.8f};
    bool adaptiveBinarizationEnabled{false};
    float sauvolaStrength{0.28f};
    float binarizationSoftness{0.06f};
    int textPolarityMode{0};
    int strokeWeight{0};
    bool smartSharpenEnabled{false};
    float smartSharpenStrength{0.45f};
    bool claheEnabled{false};
    float claheClipLimit{2.0f};
    bool twoColorTextEnabled{false};
    bool textHysteresisEnabled{false};
    float textHysteresisStrength{0.08f};
    bool selectiveSharpenEnabled{false};
    bool focusDetectionEnabled{false};
    float focusThreshold{0.012f};
    bool glareSuppressionEnabled{false};
    float glareSuppressionStrength{0.5f};
    bool mlSuperResEnabled{false};
    float mlSuperResStrength{0.65f};
    bool mlSuperResPrefer2x{false};
    bool mlSuperResUltra1440p{false};
};

// AI / assistive configuration. Values here take precedence; the matching
// OPENZOOM_* environment variables act as fallbacks when a field is empty.
struct AssistiveSettings {
    QString aiProvider{QStringLiteral("codex")};
    QString codexExecutablePath;
    QString codexModel{QStringLiteral("gpt-5.6-luna")};
    QString codexReasoningEffort{QStringLiteral("low")};
    bool codexInternetEnabled{false};
    bool codexCodingEnabled{false};
    QString codexWorkspaceDirectory;
    QString assistantInstructions{
        QStringLiteral("Reply in the same language as the user's request unless they ask for "
                       "another language. Keep answers concise and easy to understand.")};
    QString vlmApiUrl;
    // Runtime-only secret. SettingsStore deliberately never serializes this
    // value; SettingsController loads it from Windows Credential Manager.
    QString vlmApiKey;
    QString vlmCredentialId;
    QString vlmModel;
    QString vlmPrompt;
    QString ttsEngine;
    QString ttsVoiceName;
    QString ttsVoiceLocale;
    double ttsRate{0.0};
    bool lectureNotesEnabled{true};
};

// OpenZoom keeps a small index of only the persistent Codex conversations it
// created. The transcript itself remains in Codex's own thread store.
struct CodexConversation {
    QString threadId;
    QString title;
    QString preview;
    qint64 createdAt{0};
    qint64 updatedAt{0};
};

struct PresetDefinition {
    QString id;
    QString name;
    QString description;
    QString configId;
    bool isBuiltIn{false};
};

struct PersistentSettings {
    // Empty means this profile predates language persistence. Startup maps the
    // system locale once, then stores one of the stable tokens en/tr/de.
    QString language;
    // Empty selects UserDataPaths::DefaultRoot(). Keeping the empty value in
    // settings lets the Documents folder follow Windows account policy.
    QString userDataRoot;
    int cameraIndex{-1};
    QString cameraFormatStableId;
    QString microphoneEndpointId;
    // Load-only bridge for the removed global compatibility checkbox. The
    // first selected camera converts it to a per-camera Compatibility mode.
    bool legacyWiderCameraCompatibility{false};
    QString cameraAccelerationAttempt;
    QMap<QString, CameraAccelerationSetting> cameraAcceleration;
    int rotationQuarterTurns{0};
    bool virtualJoystick{false};
    bool zoomWheelAcceleration{true};
    bool controlsCollapsed{true};
    bool simpleUiMode{true};
    int advancedPanelWidth{520};
    ViewportRateMode viewportRateMode{ViewportRateMode::AutoUpTo120};
    ViewportFitModeSetting viewportFitMode{ViewportFitModeSetting::Fill};
    RecordingCanvasMode recordingCanvasMode{RecordingCanvasMode::Source};
    // Live transcription (plan 36): privacy-safe defaults — microphone audio
    // is only sent to the realtime service after an explicit opt-in.
    bool liveTranscriptionEnabled{false};
    bool appendTranscriptToNotes{true};
    QMap<QString, bool> uiSectionStates;
    QRect assistiveOverlayGeometry;
    QString assistiveOverlayDockPosition{QStringLiteral("floating")};
    QString annotationColor{QStringLiteral("#fff000")};
    int annotationWidthPixels{8};
    bool annotationCaptureOnExit{true};
    bool annotationDashed{false};
    QString annotationShapeKind{QStringLiteral("rectangle")};
    int annotationTextSizePixels{36};
    bool setupAssistantDeclined{false};
    QString selectedPresetId;
    AdvancedConfig currentConfig{};
    color_schemes::ColorScheme customColorScheme{};
    AssistiveSettings assistive{};
    std::vector<CodexConversation> codexConversations;
    std::vector<AdvancedConfig> customConfigs;
    std::vector<PresetDefinition> customPresets;
};

QString ResolveSettingsPath();
void EnsureSettingsDirectory(const QString& path);

enum class LoadStatus {
    Loaded,
    Missing,
    Unreadable,
    InvalidJson,
    UnsupportedVersion,
};

struct LoadResult {
    LoadStatus status{LoadStatus::Missing};
    std::optional<PersistentSettings> settings;
    QString error;
    bool migrationApplied{false};
};

LoadResult LoadDetailed(const QString& path);
std::optional<PersistentSettings> Load(const QString& path);
bool Save(const QString& path, const PersistentSettings& settings);

const std::vector<AdvancedConfig>& BuiltInConfigs();
const std::vector<PresetDefinition>& BuiltInPresets();
QString DefaultPresetId();

const AdvancedConfig* FindAdvancedConfigById(const QString& configId,
                                             const std::vector<AdvancedConfig>& customConfigs);
const PresetDefinition* FindPresetById(const QString& presetId,
                                       const std::vector<PresetDefinition>& customPresets);
std::optional<AdvancedConfig> ResolveConfigForPreset(const QString& presetId,
                                                     const std::vector<AdvancedConfig>& customConfigs,
                                                     const std::vector<PresetDefinition>& customPresets);
bool AreConfigsEquivalent(const AdvancedConfig& lhs, const AdvancedConfig& rhs);

} // namespace openzoom::settings
