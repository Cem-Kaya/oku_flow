#pragma once

#include <QByteArray>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QMap>

#include "okuflow/app/color_schemes.hpp"
#include "okuflow/common/recording_contract.hpp"
#include "okuflow/common/uct_model_settings.hpp"

#include <optional>
#include <vector>

namespace okuflow::settings {

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
// OKUFLOW_* environment variables act as fallbacks when a field is empty.
struct AssistiveSettings {
    bool agentControlEnabled{true};
    bool externalMcpEnabled{false};
    QString aiProvider{QStringLiteral("codex")};
    QString codexExecutablePath;
    QString codexModel{QStringLiteral("gpt-6-luna")};
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
    QStringList pendingCredentialRemovals;
    QString vlmModel;
    QString vlmPrompt;
    QString ttsEngine;
    QString ttsVoiceName;
    QString ttsVoiceLocale;
    double ttsRate{0.0};
    bool lectureNotesEnabled{true};
};

// OkuFlow keeps a small index of only the persistent Codex conversations it
// created. The transcript itself remains in Codex's own thread store.
struct CodexConversation {
    QString threadId;
    QString title;
    QString preview;
    qint64 createdAt{0};
    qint64 updatedAt{0};
    // Opened by Explain rather than the Assistant chat. An empty title is
    // shown as the translated "Explain conversation".
    bool explain{false};
    // Empty is an unassigned/legacy entry: infer only from explain. Unknown
    // tokens are always tool-free assistant, never upgraded to agent.
    QString kind;
    QString toolsetVersion;
};

QString EffectiveConversationKind(const CodexConversation& conversation);

struct PresetDefinition {
    QString id;
    QString name;
    QString description;
    QString configId;
    bool isBuiltIn{false};
};

struct PersistentSettings {
    // UCT preference is shared across quick modes; Text Clarity gates runtime use.
    bool ultraClearTextEnabled{false};
    // Model preferences persist; show-original remains session-only.
    uct::ModelSettings ultraClearTextModels;
    // UI language preference: a supported code from app_language.hpp (an
    // explicit choice) or "system". Empty also follows the Windows display
    // language and is saved as "system".
    QString language;
    // Empty selects UserDataPaths::DefaultRoot(). Keeping the empty value in
    // settings lets the Documents folder follow Windows account policy.
    QString userDataRoot;
    int cameraIndex{-1};
    QString cameraSymbolicLink;
    // Automatic fallback is not an explicit user choice of webcam.
    bool cameraSelectionExplicit{false};
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
    int liveRecapMinutes{5}; // Camera-only RAM history, bounded to 1–5 minutes.
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
    // Codex thread every Explain continues; empty = the next Explain opens one.
    QString explainThreadId;
    std::vector<AdvancedConfig> customConfigs;
    std::vector<PresetDefinition> customPresets;
};

QString ResolveSettingsPath();
void EnsureSettingsDirectory(const QString& path);

// Raw on-disk byte limit for settings.json and its .backup, CRLF included.
// Load reads at most one byte past it, so a file that grows after any size
// query cannot bypass the bound. Save refuses larger documents before touching
// existing files. The OkuFlow-created conversation index has no count limit;
// 16 MiB holds more than 10,000 maximum-length entries plus hundreds of custom
// quick modes (see settings_store_tests capHoldsDocumentedCapacity).
inline constexpr qint64 kMaxSettingsFileBytes = 16 * 1024 * 1024;

enum class LoadStatus {
    Loaded,
    Missing,
    Unreadable,
    InvalidJson,
    UnsupportedVersion,
    TooLarge,
};

struct LoadResult {
    LoadStatus status{LoadStatus::Missing};
    std::optional<PersistentSettings> settings;
    QString error;
    bool migrationApplied{false};
};

enum class SaveStatus {
    Saved,
    // Serialized bytes exceed kMaxSettingsFileBytes; no file was changed.
    TooLarge,
    Failed,
};

LoadResult LoadDetailed(const QString& path);
std::optional<PersistentSettings> Load(const QString& path);
// Exact bytes Save writes (Windows CRLF line endings), for size accounting.
QByteArray Serialize(const PersistentSettings& settings);
SaveStatus SaveDetailed(const QString& path, const PersistentSettings& settings);
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

} // namespace okuflow::settings
