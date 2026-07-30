#ifdef _WIN32

#include "openzoom/app/settings_store.hpp"
#include "openzoom/app/constants.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <utility>

namespace openzoom::settings {

namespace {

constexpr int kCurrentSettingsVersion = 15;

int SnapRotation(int turns)
{
    int value = turns % 4;
    if (value < 0) {
        value += 4;
    }
    return value;
}

float Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

QJsonObject ColorSchemeToJson(const color_schemes::ColorScheme& requested)
{
    const color_schemes::ColorScheme scheme =
        color_schemes::NormalizeColorScheme(requested);
    QJsonArray stops;
    for (const QColor& color : scheme.stops) {
        stops.append(color.name(QColor::HexRgb));
    }
    return QJsonObject{
        {QStringLiteral("id"), scheme.id},
        {QStringLiteral("name"), scheme.name},
        {QStringLiteral("mode"), color_schemes::ModeToken(scheme.mode)},
        {QStringLiteral("stops"), stops},
        {QStringLiteral("stepped"), scheme.stepped},
        {QStringLiteral("textColorAtHighLuma"), scheme.textColorAtHighLuma},
    };
}

color_schemes::ColorScheme ColorSchemeFromJson(const QJsonValue& value,
                                                int legacyMode)
{
    if (!value.isObject()) {
        return color_schemes::LegacyColorScheme(legacyMode);
    }
    const QJsonObject object = value.toObject();
    color_schemes::ColorScheme scheme;
    scheme.id = object.value(QStringLiteral("id")).toString();
    scheme.name = object.value(QStringLiteral("name")).toString();
    scheme.accessibleName = scheme.name;
    scheme.mode = color_schemes::ModeFromToken(
        object.value(QStringLiteral("mode")).toString());
    scheme.stepped = object.value(QStringLiteral("stepped")).toBool(
        scheme.mode == color_schemes::SchemeMode::Posterize);
    scheme.textColorAtHighLuma =
        object.value(QStringLiteral("textColorAtHighLuma")).toBool(true);
    const QJsonArray stops = object.value(QStringLiteral("stops")).toArray();
    const int count = std::clamp(static_cast<int>(stops.size()), 0, 8);
    for (int i = 0; i < count; ++i) {
        const QString text = stops.at(i).toString();
        const QColor color(text);
        if (!color.isValid() || text.size() != 7 || !text.startsWith(QLatin1Char('#'))) {
            return color_schemes::LegacyColorScheme(legacyMode);
        }
        scheme.stops.push_back(color);
    }
    if (scheme.stops.size() < 2) {
        return color_schemes::LegacyColorScheme(legacyMode);
    }
    if (const color_schemes::ColorScheme* builtIn =
            color_schemes::FindBuiltInColorScheme(scheme.id)) {
        scheme.accessibleName = builtIn->accessibleName;
        scheme.legacyMode = builtIn->legacyMode;
        scheme.effect = builtIn->effect;
    }
    return color_schemes::NormalizeColorScheme(scheme, legacyMode);
}

AdvancedConfig MakeConfig(QString id,
                          QString name,
                          QString description,
                          bool blackWhiteEnabled,
                          float blackWhiteThreshold,
                          bool zoomEnabled,
                          float zoomAmount,
                          float zoomCenterX,
                          float zoomCenterY,
                          bool blurEnabled,
                          float blurSigma,
                          int blurRadius,
                          bool temporalSmoothEnabled,
                          float temporalSmoothAlpha,
                          bool spatialSharpenEnabled,
                          int spatialUpscaler,
                          float spatialSharpness,
                          bool debugView,
                          bool focusMarker,
                          int rotationQuarterTurns,
                          bool ocrAssistEnabled,
                          bool vlmAssistEnabled,
                          bool assistiveOverlayEnabled)
{
    AdvancedConfig config;
    config.id = std::move(id);
    config.name = std::move(name);
    config.description = std::move(description);
    config.blackWhiteEnabled = blackWhiteEnabled;
    config.blackWhiteThreshold = Clamp01(blackWhiteThreshold);
    config.zoomEnabled = zoomEnabled;
    config.zoomAmount = std::max(1.0f, zoomAmount);
    config.zoomCenterX = Clamp01(zoomCenterX);
    config.zoomCenterY = Clamp01(zoomCenterY);
    config.blurEnabled = blurEnabled;
    config.blurSigma = std::max(0.1f, blurSigma);
    config.blurRadius = std::max(0, blurRadius);
    config.temporalSmoothEnabled = temporalSmoothEnabled;
    config.temporalSmoothAlpha = Clamp01(temporalSmoothAlpha);
    config.spatialSharpenEnabled = spatialSharpenEnabled;
    config.spatialUpscaler = (spatialUpscaler == 0) ? 0 : 1;
    config.spatialSharpness = Clamp01(spatialSharpness);
    config.debugView = debugView;
    config.focusMarker = focusMarker;
    config.rotationQuarterTurns = SnapRotation(rotationQuarterTurns);
    config.ocrAssistEnabled = ocrAssistEnabled;
    config.vlmAssistEnabled = vlmAssistEnabled;
    config.assistiveOverlayEnabled = assistiveOverlayEnabled;
    return config;
}

const std::vector<AdvancedConfig>& BuiltInConfigsStorage()
{
    static const std::vector<AdvancedConfig> kConfigs = {
        MakeConfig(QStringLiteral("preset-reading-config"),
                   QStringLiteral("Reading"),
                   QStringLiteral("Balanced magnification for reading text comfortably."),
                   false, 0.5f, true, 1.6f, 0.5f, 0.5f,
                   false, 1.0f, 3,
                   true, 0.25f,
                   true, 1, 0.25f,
                   false, false, 0,
                   false, false, true),
        MakeConfig(QStringLiteral("preset-high-contrast-config"),
                   QStringLiteral("High Contrast"),
                   QStringLiteral("Thresholded monochrome for strong text separation."),
                   true, 0.62f, true, 1.5f, 0.5f, 0.5f,
                   false, 1.0f, 3,
                   true, 0.18f,
                   false, 1, 0.25f,
                   false, false, 0,
                   false, false, true),
        MakeConfig(QStringLiteral("preset-steady-text-config"),
                   QStringLiteral("Steady Text"),
                   QStringLiteral("Favors stability and reduced shimmer during movement."),
                   false, 0.5f, true, 1.45f, 0.5f, 0.5f,
                   false, 1.0f, 3,
                   true, 0.45f,
                   false, 1, 0.25f,
                   false, false, 0,
                   false, false, true),
        MakeConfig(QStringLiteral("preset-sharp-text-config"),
                   QStringLiteral("Sharp Text"),
                   QStringLiteral("Stronger sharpening for crisp UI and document edges."),
                   false, 0.5f, true, 1.55f, 0.5f, 0.5f,
                   false, 1.0f, 3,
                   false, 0.25f,
                   true, 1, 0.45f,
                   false, false, 0,
                   false, false, true),
        MakeConfig(QStringLiteral("preset-large-zoom-config"),
                   QStringLiteral("Large Zoom"),
                   QStringLiteral("Prioritizes magnification with a simpler processing stack."),
                   false, 0.5f, true, 2.3f, 0.5f, 0.5f,
                   false, 1.0f, 3,
                   false, 0.25f,
                   false, 1, 0.25f,
                   false, true, 0,
                   false, false, true),
        MakeConfig(QStringLiteral("preset-low-light-config"),
                   QStringLiteral("Low Light"),
                   QStringLiteral("Softer smoothing-oriented preset for noisy scenes."),
                   false, 0.5f, true, 1.35f, 0.5f, 0.5f,
                   true, 1.6f, 3,
                   true, 0.35f,
                   false, 1, 0.25f,
                   false, false, 0,
                   false, false, true),
        MakeConfig(QStringLiteral("preset-ocr-assist-config"),
                   QStringLiteral("OCR Assist"),
                   QStringLiteral("Reading preset with OCR hooks enabled for future overlays."),
                   false, 0.5f, true, 1.7f, 0.5f, 0.5f,
                   false, 1.0f, 3,
                   true, 0.28f,
                   true, 1, 0.28f,
                   false, false, 0,
                   true, false, true),
        MakeConfig(QStringLiteral("preset-scene-explain-config"),
                   QStringLiteral("Scene Explain"),
                   QStringLiteral("Context-oriented preset with VLM hooks enabled for scene summaries."),
                   false, 0.5f, true, 1.25f, 0.5f, 0.5f,
                   false, 1.0f, 3,
                   true, 0.22f,
                   false, 1, 0.25f,
                   false, false, 0,
                   false, true, true),
        // MakeConfig's parameter list predates the keystone/auto-contrast and
        // stabilization fields, so the newer presets set them afterwards.
        []() {
            AdvancedConfig config = MakeConfig(QStringLiteral("preset-projector-config"),
                       QStringLiteral("Projector Screen"),
                       QStringLiteral("Straightens the projected screen and fixes washed-out colors."),
                       false, 0.5f, true, 1.4f, 0.5f, 0.5f,
                       false, 1.0f, 3,
                       true, 0.25f,
                       false, 1, 0.25f,
                       false, false, 0,
                       false, false, true);
            config.keystoneEnabled = true;
            config.autoContrastEnabled = true;
            config.autoContrastStrength = 0.7f;
            config.stabilizationEnabled = true;
            return config;
        }(),
        []() {
            AdvancedConfig config = MakeConfig(QStringLiteral("preset-whiteboard-config"),
                       QStringLiteral("Whiteboard"),
                       QStringLiteral("High contrast and sharpening for handwriting on a board."),
                       false, 0.5f, true, 1.5f, 0.5f, 0.5f,
                       false, 1.0f, 3,
                       false, 0.25f,
                       true, 1, 0.35f,
                       false, false, 0,
                       false, false, true);
            config.keystoneEnabled = true;
            config.autoContrastEnabled = true;
            config.autoContrastStrength = 0.85f;
            config.stabilizationEnabled = true;
            return config;
        }(),
        []() {
            AdvancedConfig config = MakeConfig(QStringLiteral("preset-document-config"),
                       QStringLiteral("Document"),
                       QStringLiteral("Adaptive text clarity for pages with shadows, glare, or uneven light."),
                       false, 0.5f, true, 1.6f, 0.5f, 0.5f,
                       false, 1.0f, 3,
                       false, 0.25f,
                       false, 1, 0.25f,
                       false, false, 0,
                       false, false, true);
            config.autoTextClarityEnabled = true;
            config.backgroundFlattenEnabled = true;
            config.backgroundFlattenStrength = 0.85f;
            config.adaptiveBinarizationEnabled = true;
            config.sauvolaStrength = 0.28f;
            config.binarizationSoftness = 0.06f;
            config.smartSharpenEnabled = true;
            config.smartSharpenStrength = 0.42f;
            config.textPolarityMode = 0;
            config.strokeWeight = 1;
            config.twoColorTextEnabled = true;
            config.textHysteresisEnabled = true;
            config.textHysteresisStrength = 0.08f;
            config.selectiveSharpenEnabled = true;
            config.focusDetectionEnabled = true;
            config.glareSuppressionEnabled = true;
            config.glareSuppressionStrength = 0.45f;
            return config;
        }(),
    };
    return kConfigs;
}

const std::vector<PresetDefinition>& BuiltInPresetsStorage()
{
    static const std::vector<PresetDefinition> kPresets = {
        {QStringLiteral("preset-reading"), QStringLiteral("Reading"),
         QStringLiteral("Balanced magnification for general reading."), QStringLiteral("preset-reading-config"), true},
        {QStringLiteral("preset-high-contrast"), QStringLiteral("High Contrast"),
         QStringLiteral("Monochrome thresholding for stronger contrast."), QStringLiteral("preset-high-contrast-config"), true},
        {QStringLiteral("preset-steady-text"), QStringLiteral("Steady Text"),
         QStringLiteral("More smoothing for less shimmer while moving."), QStringLiteral("preset-steady-text-config"), true},
        {QStringLiteral("preset-sharp-text"), QStringLiteral("Sharp Text"),
         QStringLiteral("Sharper rendering for crisp text edges."), QStringLiteral("preset-sharp-text-config"), true},
        {QStringLiteral("preset-large-zoom"), QStringLiteral("Large Zoom"),
         QStringLiteral("Higher magnification with fewer extra effects."), QStringLiteral("preset-large-zoom-config"), true},
        {QStringLiteral("preset-low-light"), QStringLiteral("Low Light"),
         QStringLiteral("Gentler smoothing for dim or noisy scenes."), QStringLiteral("preset-low-light-config"), true},
        {QStringLiteral("preset-ocr-assist"), QStringLiteral("OCR Assist"),
         QStringLiteral("Preset reserved for text extraction and readable overlays."), QStringLiteral("preset-ocr-assist-config"), true},
        {QStringLiteral("preset-scene-explain"), QStringLiteral("Scene Explain"),
         QStringLiteral("Preset reserved for future VLM-based scene summaries."), QStringLiteral("preset-scene-explain-config"), true},
        {QStringLiteral("preset-projector"), QStringLiteral("Projector Screen"),
         QStringLiteral("Straightens the projected screen and fixes washed-out colors."), QStringLiteral("preset-projector-config"), true},
        {QStringLiteral("preset-whiteboard"), QStringLiteral("Whiteboard"),
         QStringLiteral("High contrast and sharpening for handwriting on a board."), QStringLiteral("preset-whiteboard-config"), true},
        {QStringLiteral("preset-document"), QStringLiteral("Document"),
         QStringLiteral("Adaptive text clarity for paper, handouts, and uneven lighting."), QStringLiteral("preset-document-config"), true},
    };
    return kPresets;
}

QJsonObject ConfigToJson(const AdvancedConfig& config)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), config.id);
    object.insert(QStringLiteral("name"), config.name);
    object.insert(QStringLiteral("description"), config.description);
    object.insert(QStringLiteral("blackWhiteEnabled"), config.blackWhiteEnabled);
    object.insert(QStringLiteral("blackWhiteThreshold"), config.blackWhiteThreshold);
    object.insert(QStringLiteral("zoomEnabled"), config.zoomEnabled);
    object.insert(QStringLiteral("zoomAmount"), config.zoomAmount);
    object.insert(QStringLiteral("zoomCenterX"), config.zoomCenterX);
    object.insert(QStringLiteral("zoomCenterY"), config.zoomCenterY);
    object.insert(QStringLiteral("blurEnabled"), config.blurEnabled);
    object.insert(QStringLiteral("blurSigma"), config.blurSigma);
    object.insert(QStringLiteral("blurRadius"), config.blurRadius);
    object.insert(QStringLiteral("temporalSmoothEnabled"), config.temporalSmoothEnabled);
    object.insert(QStringLiteral("temporalSmoothAlpha"), config.temporalSmoothAlpha);
    object.insert(QStringLiteral("spatialSharpenEnabled"), config.spatialSharpenEnabled);
    object.insert(QStringLiteral("spatialUpscaler"), config.spatialUpscaler);
    object.insert(QStringLiteral("spatialSharpness"), config.spatialSharpness);
    object.insert(QStringLiteral("debugView"), config.debugView);
    object.insert(QStringLiteral("focusMarker"), config.focusMarker);
    object.insert(QStringLiteral("rotationQuarterTurns"),
                  SnapRotation(config.rotationQuarterTurns));
    object.insert(QStringLiteral("ocrAssistEnabled"), config.ocrAssistEnabled);
    object.insert(QStringLiteral("vlmAssistEnabled"), config.vlmAssistEnabled);
    object.insert(QStringLiteral("assistiveOverlayEnabled"), config.assistiveOverlayEnabled);
    object.insert(QStringLiteral("stabilizationEnabled"), config.stabilizationEnabled);
    object.insert(QStringLiteral("displayColorMode"), config.displayColorMode);
    object.insert(QStringLiteral("colorScheme"), ColorSchemeToJson(
        config.colorScheme.stops.size() >= 2
            ? config.colorScheme
            : color_schemes::LegacyColorScheme(config.displayColorMode)));
    object.insert(QStringLiteral("contrast"), config.contrast);
    object.insert(QStringLiteral("brightness"), config.brightness);
    object.insert(QStringLiteral("keystoneEnabled"), config.keystoneEnabled);
    object.insert(QStringLiteral("autoContrastEnabled"), config.autoContrastEnabled);
    object.insert(QStringLiteral("autoContrastStrength"), config.autoContrastStrength);
    object.insert(QStringLiteral("autoTextClarityEnabled"), config.autoTextClarityEnabled);
    object.insert(QStringLiteral("backgroundFlattenEnabled"), config.backgroundFlattenEnabled);
    object.insert(QStringLiteral("backgroundFlattenStrength"), config.backgroundFlattenStrength);
    object.insert(QStringLiteral("adaptiveBinarizationEnabled"), config.adaptiveBinarizationEnabled);
    object.insert(QStringLiteral("sauvolaStrength"), config.sauvolaStrength);
    object.insert(QStringLiteral("binarizationSoftness"), config.binarizationSoftness);
    object.insert(QStringLiteral("textPolarityMode"), config.textPolarityMode);
    object.insert(QStringLiteral("strokeWeight"), config.strokeWeight);
    object.insert(QStringLiteral("smartSharpenEnabled"), config.smartSharpenEnabled);
    object.insert(QStringLiteral("smartSharpenStrength"), config.smartSharpenStrength);
    object.insert(QStringLiteral("claheEnabled"), config.claheEnabled);
    object.insert(QStringLiteral("claheClipLimit"), config.claheClipLimit);
    object.insert(QStringLiteral("twoColorTextEnabled"), config.twoColorTextEnabled);
    object.insert(QStringLiteral("textHysteresisEnabled"), config.textHysteresisEnabled);
    object.insert(QStringLiteral("textHysteresisStrength"), config.textHysteresisStrength);
    object.insert(QStringLiteral("selectiveSharpenEnabled"), config.selectiveSharpenEnabled);
    object.insert(QStringLiteral("focusDetectionEnabled"), config.focusDetectionEnabled);
    object.insert(QStringLiteral("focusThreshold"), config.focusThreshold);
    object.insert(QStringLiteral("glareSuppressionEnabled"), config.glareSuppressionEnabled);
    object.insert(QStringLiteral("glareSuppressionStrength"), config.glareSuppressionStrength);
    object.insert(QStringLiteral("mlSuperResEnabled"), config.mlSuperResEnabled);
    object.insert(QStringLiteral("mlSuperResStrength"), config.mlSuperResStrength);
    object.insert(QStringLiteral("mlSuperResPrefer2x"), config.mlSuperResPrefer2x);
    object.insert(QStringLiteral("mlSuperResUltra1440p"), config.mlSuperResUltra1440p);
    return object;
}

AdvancedConfig ConfigFromJson(const QJsonObject& object, const AdvancedConfig& defaults = AdvancedConfig{})
{
    AdvancedConfig config = defaults;
    config.id = object.value(QStringLiteral("id")).toString(config.id);
    config.name = object.value(QStringLiteral("name")).toString(config.name);
    config.description = object.value(QStringLiteral("description")).toString(config.description);
    config.blackWhiteEnabled = object.value(QStringLiteral("blackWhiteEnabled")).toBool(config.blackWhiteEnabled);
    config.blackWhiteThreshold = Clamp01(static_cast<float>(object.value(QStringLiteral("blackWhiteThreshold")).toDouble(config.blackWhiteThreshold)));
    config.zoomEnabled = object.value(QStringLiteral("zoomEnabled")).toBool(config.zoomEnabled);
    config.zoomAmount = std::max(1.0f, static_cast<float>(object.value(QStringLiteral("zoomAmount")).toDouble(config.zoomAmount)));
    config.zoomCenterX = Clamp01(static_cast<float>(object.value(QStringLiteral("zoomCenterX")).toDouble(config.zoomCenterX)));
    config.zoomCenterY = Clamp01(static_cast<float>(object.value(QStringLiteral("zoomCenterY")).toDouble(config.zoomCenterY)));
    config.blurEnabled = object.value(QStringLiteral("blurEnabled")).toBool(config.blurEnabled);
    config.blurSigma = std::max(0.1f, static_cast<float>(object.value(QStringLiteral("blurSigma")).toDouble(config.blurSigma)));
    config.blurRadius = std::max(0, object.value(QStringLiteral("blurRadius")).toInt(config.blurRadius));
    config.temporalSmoothEnabled = object.value(QStringLiteral("temporalSmoothEnabled")).toBool(config.temporalSmoothEnabled);
    config.temporalSmoothAlpha = Clamp01(static_cast<float>(object.value(QStringLiteral("temporalSmoothAlpha")).toDouble(config.temporalSmoothAlpha)));
    config.spatialSharpenEnabled = object.value(QStringLiteral("spatialSharpenEnabled")).toBool(config.spatialSharpenEnabled);
    config.spatialUpscaler = object.value(QStringLiteral("spatialUpscaler")).toInt(config.spatialUpscaler) == 0 ? 0 : 1;
    config.spatialSharpness = Clamp01(static_cast<float>(object.value(QStringLiteral("spatialSharpness")).toDouble(config.spatialSharpness)));
    config.debugView = object.value(QStringLiteral("debugView")).toBool(config.debugView);
    config.focusMarker = object.value(QStringLiteral("focusMarker")).toBool(config.focusMarker);
    config.rotationQuarterTurns = SnapRotation(object.value(QStringLiteral("rotationQuarterTurns")).toInt(config.rotationQuarterTurns));
    config.ocrAssistEnabled = object.value(QStringLiteral("ocrAssistEnabled")).toBool(config.ocrAssistEnabled);
    config.vlmAssistEnabled = object.value(QStringLiteral("vlmAssistEnabled")).toBool(config.vlmAssistEnabled);
    config.assistiveOverlayEnabled = object.value(QStringLiteral("assistiveOverlayEnabled")).toBool(config.assistiveOverlayEnabled);
    config.stabilizationEnabled = object.value(QStringLiteral("stabilizationEnabled")).toBool(config.stabilizationEnabled);
    config.displayColorMode = std::clamp(
        object.value(QStringLiteral("displayColorMode")).toInt(config.displayColorMode),
        0, app_constants::kDisplayColorModeCount - 1);
    config.colorScheme = ColorSchemeFromJson(object.value(QStringLiteral("colorScheme")),
                                             config.displayColorMode);
    config.contrast = std::clamp(static_cast<float>(object.value(QStringLiteral("contrast")).toDouble(config.contrast)), 0.25f, 4.0f);
    config.brightness = std::clamp(static_cast<float>(object.value(QStringLiteral("brightness")).toDouble(config.brightness)), -1.0f, 1.0f);
    config.keystoneEnabled = object.value(QStringLiteral("keystoneEnabled")).toBool(config.keystoneEnabled);
    config.autoContrastEnabled = object.value(QStringLiteral("autoContrastEnabled")).toBool(config.autoContrastEnabled);
    config.autoContrastStrength = Clamp01(static_cast<float>(object.value(QStringLiteral("autoContrastStrength")).toDouble(config.autoContrastStrength)));
    config.autoTextClarityEnabled = object.value(QStringLiteral("autoTextClarityEnabled")).toBool(config.autoTextClarityEnabled);
    config.backgroundFlattenEnabled = object.value(QStringLiteral("backgroundFlattenEnabled")).toBool(config.backgroundFlattenEnabled);
    config.backgroundFlattenStrength = Clamp01(static_cast<float>(object.value(QStringLiteral("backgroundFlattenStrength")).toDouble(config.backgroundFlattenStrength)));
    config.adaptiveBinarizationEnabled = object.value(QStringLiteral("adaptiveBinarizationEnabled")).toBool(config.adaptiveBinarizationEnabled);
    config.sauvolaStrength = std::clamp(static_cast<float>(object.value(QStringLiteral("sauvolaStrength")).toDouble(config.sauvolaStrength)), 0.1f, 0.5f);
    config.binarizationSoftness = std::clamp(static_cast<float>(object.value(QStringLiteral("binarizationSoftness")).toDouble(config.binarizationSoftness)), 0.0f, 0.25f);
    config.textPolarityMode = std::clamp(object.value(QStringLiteral("textPolarityMode")).toInt(config.textPolarityMode), 0, 2);
    config.strokeWeight = std::clamp(object.value(QStringLiteral("strokeWeight")).toInt(config.strokeWeight), -3, 3);
    config.smartSharpenEnabled = object.value(QStringLiteral("smartSharpenEnabled")).toBool(config.smartSharpenEnabled);
    config.smartSharpenStrength = Clamp01(static_cast<float>(object.value(QStringLiteral("smartSharpenStrength")).toDouble(config.smartSharpenStrength)));
    config.claheEnabled = object.value(QStringLiteral("claheEnabled")).toBool(config.claheEnabled);
    config.claheClipLimit = std::clamp(static_cast<float>(object.value(QStringLiteral("claheClipLimit")).toDouble(config.claheClipLimit)), 1.0f, 8.0f);
    config.twoColorTextEnabled = object.value(QStringLiteral("twoColorTextEnabled")).toBool(config.twoColorTextEnabled);
    config.textHysteresisEnabled = object.value(QStringLiteral("textHysteresisEnabled")).toBool(config.textHysteresisEnabled);
    config.textHysteresisStrength = std::clamp(static_cast<float>(object.value(QStringLiteral("textHysteresisStrength")).toDouble(config.textHysteresisStrength)), 0.0f, 0.25f);
    config.selectiveSharpenEnabled = object.value(QStringLiteral("selectiveSharpenEnabled")).toBool(config.selectiveSharpenEnabled);
    config.focusDetectionEnabled = object.value(QStringLiteral("focusDetectionEnabled")).toBool(config.focusDetectionEnabled);
    config.focusThreshold = std::clamp(static_cast<float>(object.value(QStringLiteral("focusThreshold")).toDouble(config.focusThreshold)), 0.001f, 0.1f);
    config.glareSuppressionEnabled = object.value(QStringLiteral("glareSuppressionEnabled")).toBool(config.glareSuppressionEnabled);
    config.glareSuppressionStrength = Clamp01(static_cast<float>(object.value(QStringLiteral("glareSuppressionStrength")).toDouble(config.glareSuppressionStrength)));
    config.mlSuperResEnabled = object.value(QStringLiteral("mlSuperResEnabled"))
                                   .toBool(object.value(QStringLiteral("mlTextSuperResolutionEnabled"))
                                               .toBool(config.mlSuperResEnabled));
    config.mlSuperResStrength = Clamp01(static_cast<float>(
        object.value(QStringLiteral("mlSuperResStrength"))
            .toDouble(object.value(QStringLiteral("mlTextSuperResolutionStrength"))
                          .toDouble(config.mlSuperResStrength))));
    config.mlSuperResPrefer2x =
        object.value(QStringLiteral("mlSuperResPrefer2x"))
            .toBool(config.mlSuperResPrefer2x);
    config.mlSuperResUltra1440p =
        object.value(QStringLiteral("mlSuperResUltra1440p"))
            .toBool(config.mlSuperResUltra1440p);
    return config;
}

QJsonObject AssistiveToJson(const AssistiveSettings& assistive)
{
    QJsonObject object;
    object.insert(QStringLiteral("aiProvider"), assistive.aiProvider);
    object.insert(QStringLiteral("codexExecutablePath"), assistive.codexExecutablePath);
    object.insert(QStringLiteral("codexModel"), assistive.codexModel);
    object.insert(QStringLiteral("codexReasoningEffort"), assistive.codexReasoningEffort);
    object.insert(QStringLiteral("codexInternetEnabled"), assistive.codexInternetEnabled);
    object.insert(QStringLiteral("codexCodingEnabled"), assistive.codexCodingEnabled);
    object.insert(QStringLiteral("codexWorkspaceDirectory"), assistive.codexWorkspaceDirectory);
    object.insert(QStringLiteral("assistantInstructions"), assistive.assistantInstructions);
    object.insert(QStringLiteral("vlmApiUrl"), assistive.vlmApiUrl);
    if (!assistive.vlmCredentialId.trimmed().isEmpty()) {
        object.insert(QStringLiteral("vlmCredentialId"),
                      assistive.vlmCredentialId.trimmed());
    }
    object.insert(QStringLiteral("vlmModel"), assistive.vlmModel);
    object.insert(QStringLiteral("vlmPrompt"), assistive.vlmPrompt);
    object.insert(QStringLiteral("tesseractPath"), assistive.tesseractPath);
    object.insert(QStringLiteral("ocrLanguage"), assistive.ocrLanguage);
    object.insert(QStringLiteral("ttsEngine"), assistive.ttsEngine);
    object.insert(QStringLiteral("ttsVoiceName"), assistive.ttsVoiceName);
    object.insert(QStringLiteral("ttsVoiceLocale"), assistive.ttsVoiceLocale);
    object.insert(QStringLiteral("ttsRate"), assistive.ttsRate);
    object.insert(QStringLiteral("lectureNotesEnabled"), assistive.lectureNotesEnabled);
    return object;
}

AssistiveSettings AssistiveFromJson(const QJsonObject& object)
{
    AssistiveSettings assistive;
    assistive.aiProvider = object.value(QStringLiteral("aiProvider")).toString(assistive.aiProvider);
    assistive.codexExecutablePath = object.value(QStringLiteral("codexExecutablePath")).toString();
    assistive.codexModel = object.value(QStringLiteral("codexModel")).toString(assistive.codexModel);
    assistive.codexReasoningEffort = object.value(QStringLiteral("codexReasoningEffort"))
                                          .toString(assistive.codexReasoningEffort);
    assistive.codexInternetEnabled = object.value(QStringLiteral("codexInternetEnabled"))
                                          .toBool(assistive.codexInternetEnabled);
    assistive.codexCodingEnabled = object.value(QStringLiteral("codexCodingEnabled"))
                                        .toBool(assistive.codexCodingEnabled);
    assistive.codexWorkspaceDirectory = object.value(QStringLiteral("codexWorkspaceDirectory"))
                                             .toString();
    assistive.assistantInstructions = object.value(QStringLiteral("assistantInstructions"))
                                          .toString(assistive.assistantInstructions);
    assistive.vlmApiUrl = object.value(QStringLiteral("vlmApiUrl")).toString(assistive.vlmApiUrl);
    assistive.vlmCredentialId =
        object.value(QStringLiteral("vlmCredentialId")).toString();
    assistive.vlmModel = object.value(QStringLiteral("vlmModel")).toString(assistive.vlmModel);
    assistive.vlmPrompt = object.value(QStringLiteral("vlmPrompt")).toString(assistive.vlmPrompt);
    assistive.tesseractPath = object.value(QStringLiteral("tesseractPath")).toString(assistive.tesseractPath);
    assistive.ocrLanguage = object.value(QStringLiteral("ocrLanguage")).toString(assistive.ocrLanguage);
    assistive.ttsEngine = object.value(QStringLiteral("ttsEngine")).toString();
    assistive.ttsVoiceName = object.value(QStringLiteral("ttsVoiceName")).toString();
    assistive.ttsVoiceLocale = object.value(QStringLiteral("ttsVoiceLocale")).toString();
    assistive.ttsRate = std::clamp(object.value(QStringLiteral("ttsRate"))
                                        .toDouble(assistive.ttsRate),
                                    -1.0,
                                    1.0);
    assistive.lectureNotesEnabled = object.value(QStringLiteral("lectureNotesEnabled")).toBool(assistive.lectureNotesEnabled);
    return assistive;
}

QJsonObject CodexConversationToJson(const CodexConversation& conversation)
{
    return QJsonObject{{QStringLiteral("threadId"), conversation.threadId},
                       {QStringLiteral("title"), conversation.title},
                       {QStringLiteral("preview"), conversation.preview},
                       {QStringLiteral("createdAt"), conversation.createdAt},
                       {QStringLiteral("updatedAt"), conversation.updatedAt}};
}

CodexConversation CodexConversationFromJson(const QJsonObject& object)
{
    CodexConversation conversation;
    conversation.threadId = object.value(QStringLiteral("threadId")).toString();
    conversation.title = object.value(QStringLiteral("title")).toString();
    conversation.preview = object.value(QStringLiteral("preview")).toString();
    conversation.createdAt = object.value(QStringLiteral("createdAt")).toInteger();
    conversation.updatedAt = object.value(QStringLiteral("updatedAt")).toInteger();
    return conversation;
}

QJsonObject PresetToJson(const PresetDefinition& preset)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), preset.id);
    object.insert(QStringLiteral("name"), preset.name);
    object.insert(QStringLiteral("description"), preset.description);
    object.insert(QStringLiteral("configId"), preset.configId);
    object.insert(QStringLiteral("isBuiltIn"), preset.isBuiltIn);
    return object;
}

PresetDefinition PresetFromJson(const QJsonObject& object)
{
    PresetDefinition preset;
    preset.id = object.value(QStringLiteral("id")).toString();
    preset.name = object.value(QStringLiteral("name")).toString();
    preset.description = object.value(QStringLiteral("description")).toString();
    preset.configId = object.value(QStringLiteral("configId")).toString();
    preset.isBuiltIn = object.value(QStringLiteral("isBuiltIn")).toBool(false);
    return preset;
}

AdvancedConfig LegacyConfigFromRoot(const QJsonObject& root)
{
    AdvancedConfig config = BuiltInConfigsStorage().front();
    config.id = QStringLiteral("legacy-current");
    config.name = QStringLiteral("Current Setup");
    config.description = QStringLiteral("Migrated from the legacy flat settings format.");

    const QJsonObject blackWhite = root.value(QStringLiteral("blackWhite")).toObject();
    config.blackWhiteEnabled = blackWhite.value(QStringLiteral("enabled")).toBool(config.blackWhiteEnabled);
    config.blackWhiteThreshold = Clamp01(static_cast<float>(blackWhite.value(QStringLiteral("threshold")).toDouble(config.blackWhiteThreshold)));

    const QJsonObject zoom = root.value(QStringLiteral("zoom")).toObject();
    config.zoomEnabled = zoom.value(QStringLiteral("enabled")).toBool(config.zoomEnabled);
    config.zoomAmount = std::max(1.0f, static_cast<float>(zoom.value(QStringLiteral("amount")).toDouble(config.zoomAmount)));
    config.zoomCenterX = Clamp01(static_cast<float>(zoom.value(QStringLiteral("centerX")).toDouble(config.zoomCenterX)));
    config.zoomCenterY = Clamp01(static_cast<float>(zoom.value(QStringLiteral("centerY")).toDouble(config.zoomCenterY)));

    const QJsonObject blur = root.value(QStringLiteral("blur")).toObject();
    config.blurEnabled = blur.value(QStringLiteral("enabled")).toBool(config.blurEnabled);
    config.blurSigma = std::max(0.1f, static_cast<float>(blur.value(QStringLiteral("sigma")).toDouble(config.blurSigma)));
    config.blurRadius = std::max(0, blur.value(QStringLiteral("radius")).toInt(config.blurRadius));

    const QJsonObject temporal = root.value(QStringLiteral("temporalSmooth")).toObject();
    config.temporalSmoothEnabled = temporal.value(QStringLiteral("enabled")).toBool(config.temporalSmoothEnabled);
    config.temporalSmoothAlpha = Clamp01(static_cast<float>(temporal.value(QStringLiteral("alpha")).toDouble(config.temporalSmoothAlpha)));

    const QJsonObject spatial = root.value(QStringLiteral("spatialSharpen")).toObject();
    config.spatialSharpenEnabled = spatial.value(QStringLiteral("enabled")).toBool(config.spatialSharpenEnabled);
    const QString backend = spatial.value(QStringLiteral("backend")).toString();
    config.spatialUpscaler = (backend.compare(QStringLiteral("fsr"), Qt::CaseInsensitive) == 0) ? 0 : 1;
    config.spatialSharpness = Clamp01(static_cast<float>(spatial.value(QStringLiteral("sharpness")).toDouble(config.spatialSharpness)));

    config.debugView = root.value(QStringLiteral("debugView")).toBool(config.debugView);
    config.focusMarker = root.value(QStringLiteral("focusMarker")).toBool(config.focusMarker);
    config.rotationQuarterTurns = SnapRotation(root.value(QStringLiteral("rotationQuarterTurns")).toInt(config.rotationQuarterTurns));
    return config;
}

} // namespace

QString ResolveSettingsPath()
{
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (basePath.isEmpty()) {
        basePath = QCoreApplication::applicationDirPath();
    }
    QDir dir(basePath);
    return dir.filePath(QStringLiteral("settings.json"));
}

void EnsureSettingsDirectory(const QString& path)
{
    QFileInfo info(path);
    QDir dir = info.dir();
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }
}

namespace {

std::optional<PersistentSettings> ParseSettingsRoot(const QJsonObject& root)
{
    PersistentSettings settings;
    settings.userDataRoot =
        root.value(QStringLiteral("paths"))
            .toObject()
            .value(QStringLiteral("userDataRoot"))
            .toString();
    settings.cameraIndex = root.value(QStringLiteral("cameraIndex")).toInt(settings.cameraIndex);
    settings.cameraFormatStableId =
        root.value(QStringLiteral("cameraFormatStableId")).toString();
    settings.microphoneEndpointId =
        root.value(QStringLiteral("microphoneEndpointId")).toString();
    const QJsonObject capture =
        root.value(QStringLiteral("capture")).toObject();
    settings.legacyWiderCameraCompatibility =
        capture.value(QStringLiteral("widerCameraCompatibility"))
            .toBool(false);
    settings.cameraAccelerationAttempt =
        capture.value(QStringLiteral("accelerationAttempt")).toString();
    const QJsonObject cameraAcceleration =
        capture.value(QStringLiteral("cameras")).toObject();
    for (auto it = cameraAcceleration.begin();
         it != cameraAcceleration.end(); ++it) {
        if (!it.value().isObject()) {
            continue;
        }
        const QJsonObject value = it.value().toObject();
        CameraAccelerationSetting camera;
        camera.mode = static_cast<CameraAccelerationMode>(std::clamp(
            value.value(QStringLiteral("mode"))
                .toInt(static_cast<int>(camera.mode)),
            static_cast<int>(CameraAccelerationMode::Automatic),
            static_cast<int>(CameraAccelerationMode::ForceCompatibility)));
        camera.automaticFallback =
            value.value(QStringLiteral("automaticFallback")).toBool(false);
        camera.reason = value.value(QStringLiteral("reason")).toString();
        camera.decidedOn = value.value(QStringLiteral("decidedOn")).toString();
        camera.lastRung = value.value(QStringLiteral("lastRung")).toString();
        settings.cameraAcceleration.insert(it.key(), std::move(camera));
    }

    const int version = root.value(QStringLiteral("version")).toInt(1);
    if (version <= 1 && root.contains(QStringLiteral("zoom"))) {
        settings.virtualJoystick = root.value(QStringLiteral("virtualJoystick")).toBool(settings.virtualJoystick);
        settings.controlsCollapsed = root.value(QStringLiteral("controlsCollapsed")).toBool(settings.controlsCollapsed);
        settings.currentConfig = LegacyConfigFromRoot(root);
        settings.rotationQuarterTurns = settings.currentConfig.rotationQuarterTurns;
        settings.selectedPresetId.clear();
        return settings;
    }

    const QJsonObject ui = root.value(QStringLiteral("ui")).toObject();
    settings.virtualJoystick = ui.value(QStringLiteral("virtualJoystick")).toBool(settings.virtualJoystick);
    settings.zoomWheelAcceleration =
        ui.value(QStringLiteral("zoomWheelAcceleration"))
            .toBool(settings.zoomWheelAcceleration);
    settings.controlsCollapsed = ui.value(QStringLiteral("controlsCollapsed")).toBool(settings.controlsCollapsed);
    settings.simpleUiMode = ui.value(QStringLiteral("simpleUiMode")).toBool(settings.simpleUiMode);
    settings.advancedPanelWidth = std::clamp(
        ui.value(QStringLiteral("advancedPanelWidth")).toInt(settings.advancedPanelWidth),
        360, 1200);
    settings.viewportRateMode = static_cast<ViewportRateMode>(std::clamp(
        ui.value(QStringLiteral("viewportRateMode"))
            .toInt(static_cast<int>(settings.viewportRateMode)),
        static_cast<int>(ViewportRateMode::AutoUpTo120),
        static_cast<int>(ViewportRateMode::MatchDisplay)));
    settings.viewportFitMode = static_cast<ViewportFitModeSetting>(std::clamp(
        ui.value(QStringLiteral("viewportFitMode"))
            .toInt(static_cast<int>(settings.viewportFitMode)),
        static_cast<int>(ViewportFitModeSetting::Fill),
        static_cast<int>(ViewportFitModeSetting::Fit)));
    const QJsonObject recording =
        root.value(QStringLiteral("recording")).toObject();
    settings.recordingCanvasMode =
        static_cast<RecordingCanvasMode>(std::clamp(
            recording.value(QStringLiteral("canvasMode"))
                .toInt(static_cast<int>(settings.recordingCanvasMode)),
            static_cast<int>(RecordingCanvasMode::Source),
            static_cast<int>(RecordingCanvasMode::Nhd360)));
    const QJsonObject sectionStates =
        ui.value(QStringLiteral("sectionStates")).toObject();
    for (auto it = sectionStates.begin(); it != sectionStates.end(); ++it) {
        settings.uiSectionStates.insert(it.key(), it.value().toBool(true));
    }
    settings.selectedPresetId = ui.value(QStringLiteral("selectedPresetId")).toString();
    settings.setupAssistantDeclined =
        ui.value(QStringLiteral("setupAssistantDeclined")).toBool(settings.setupAssistantDeclined);
    const QJsonObject overlayGeometry =
        ui.value(QStringLiteral("assistiveOverlayGeometry")).toObject();
    const int overlayWidth = std::clamp(
        overlayGeometry.value(QStringLiteral("width")).toInt(), 0, 4096);
    const int overlayHeight = std::clamp(
        overlayGeometry.value(QStringLiteral("height")).toInt(), 0, 4096);
    if (overlayWidth > 0 && overlayHeight > 0) {
        settings.assistiveOverlayGeometry = QRect(
            std::clamp(overlayGeometry.value(QStringLiteral("x")).toInt(), -32768, 32768),
            std::clamp(overlayGeometry.value(QStringLiteral("y")).toInt(), -32768, 32768),
            overlayWidth,
            overlayHeight);
    }
    const QJsonObject annotations =
        root.value(QStringLiteral("annotations")).toObject();
    const QColor annotationColor(
        annotations.value(QStringLiteral("color"))
            .toString(settings.annotationColor));
    if (annotationColor.isValid()) {
        settings.annotationColor = annotationColor.name(QColor::HexRgb);
    }
    settings.annotationWidthPixels = std::clamp(
        annotations.value(QStringLiteral("widthPixels"))
            .toInt(settings.annotationWidthPixels),
        2,
        24);
    settings.annotationCaptureOnExit =
        annotations.value(QStringLiteral("captureOnExit"))
            .toBool(settings.annotationCaptureOnExit);
    settings.annotationDashed =
        annotations.value(QStringLiteral("dashed"))
            .toBool(settings.annotationDashed);
    const QString annotationShapeKind =
        annotations.value(QStringLiteral("shapeKind"))
            .toString(settings.annotationShapeKind)
            .toLower();
    settings.annotationShapeKind =
        annotationShapeKind == QStringLiteral("ellipse")
            ? QStringLiteral("ellipse")
            : QStringLiteral("rectangle");
    settings.annotationTextSizePixels = std::clamp(
        annotations.value(QStringLiteral("textSizePixels"))
            .toInt(settings.annotationTextSizePixels),
        12,
        72);

    settings.assistive = AssistiveFromJson(root.value(QStringLiteral("assistive")).toObject());

    const QJsonArray conversationArray = root.value(QStringLiteral("codexConversations")).toArray();
    settings.codexConversations.reserve(static_cast<std::size_t>(conversationArray.size()));
    for (const QJsonValue& value : conversationArray) {
        if (!value.isObject()) {
            continue;
        }
        CodexConversation conversation = CodexConversationFromJson(value.toObject());
        if (!conversation.threadId.isEmpty()) {
            settings.codexConversations.push_back(std::move(conversation));
        }
    }

    const AdvancedConfig defaultConfig = BuiltInConfigsStorage().front();
    settings.currentConfig = ConfigFromJson(root.value(QStringLiteral("currentConfig")).toObject(), defaultConfig);
    if (root.value(QStringLiteral("customColorScheme")).isObject()) {
        settings.customColorScheme = ColorSchemeFromJson(
            root.value(QStringLiteral("customColorScheme")), 0);
        settings.customColorScheme.id = QStringLiteral("custom");
        settings.customColorScheme.legacyMode = -1;
    }
    settings.rotationQuarterTurns = SnapRotation(
        root.value(QStringLiteral("rotationQuarterTurns")).toInt(settings.currentConfig.rotationQuarterTurns));
    if (settings.currentConfig.id.isEmpty()) {
        settings.currentConfig.id = QStringLiteral("current-live");
    }
    if (settings.currentConfig.name.isEmpty()) {
        settings.currentConfig.name = QStringLiteral("Current Setup");
    }

    const QJsonArray configArray = root.value(QStringLiteral("customConfigs")).toArray();
    settings.customConfigs.reserve(static_cast<std::size_t>(configArray.size()));
    for (const QJsonValue& value : configArray) {
        if (!value.isObject()) {
            continue;
        }
        AdvancedConfig config = ConfigFromJson(value.toObject());
        if (!config.id.isEmpty()) {
            settings.customConfigs.push_back(std::move(config));
        }
    }

    const QJsonArray presetArray = root.value(QStringLiteral("customPresets")).toArray();
    settings.customPresets.reserve(static_cast<std::size_t>(presetArray.size()));
    for (const QJsonValue& value : presetArray) {
        if (!value.isObject()) {
            continue;
        }
        PresetDefinition preset = PresetFromJson(value.toObject());
        if (!preset.id.isEmpty() && !preset.configId.isEmpty()) {
            settings.customPresets.push_back(std::move(preset));
        }
    }

    return settings;
}

} // namespace

LoadResult LoadDetailed(const QString& path)
{
    LoadResult result;
    QFile file(path);
    if (!file.exists()) {
        result.status = LoadStatus::Missing;
        return result;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.status = LoadStatus::Unreadable;
        result.error = QStringLiteral("Could not read settings: %1")
                           .arg(file.errorString());
        return result;
    }

    const QByteArray data = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        result.status = LoadStatus::Unreadable;
        result.error = QStringLiteral("Could not read settings: %1")
                           .arg(file.errorString());
        return result;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.status = LoadStatus::InvalidJson;
        result.error = QStringLiteral("Settings are not valid JSON: %1")
                           .arg(parseError.errorString());
        return result;
    }

    const QJsonObject root = document.object();
    const int version = root.value(QStringLiteral("version")).toInt(1);
    if (version > kCurrentSettingsVersion) {
        result.status = LoadStatus::UnsupportedVersion;
        result.error =
            QStringLiteral("Settings version %1 is newer than this OpenZoom build supports (%2).")
                .arg(version)
                .arg(kCurrentSettingsVersion);
        return result;
    }

    result.settings = ParseSettingsRoot(root);
    if (!result.settings) {
        result.status = LoadStatus::InvalidJson;
        result.error = QStringLiteral("Settings could not be interpreted.");
        return result;
    }
    result.status = LoadStatus::Loaded;
    return result;
}

std::optional<PersistentSettings> Load(const QString& path)
{
    return LoadDetailed(path).settings;
}

bool Save(const QString& path, const PersistentSettings& settings)
{
    EnsureSettingsDirectory(path);

    QJsonObject root;
    root.insert(QStringLiteral("version"), kCurrentSettingsVersion);
    root.insert(QStringLiteral("paths"),
                QJsonObject{{QStringLiteral("userDataRoot"),
                             settings.userDataRoot}});
    root.insert(QStringLiteral("cameraIndex"), settings.cameraIndex);
    root.insert(QStringLiteral("cameraFormatStableId"),
                settings.cameraFormatStableId);
    root.insert(QStringLiteral("microphoneEndpointId"),
                settings.microphoneEndpointId);
    root.insert(QStringLiteral("rotationQuarterTurns"), SnapRotation(settings.rotationQuarterTurns));
    QJsonObject cameraAcceleration;
    for (auto it = settings.cameraAcceleration.cbegin();
         it != settings.cameraAcceleration.cend(); ++it) {
        cameraAcceleration.insert(
            it.key(),
            QJsonObject{
                {QStringLiteral("mode"), static_cast<int>(it.value().mode)},
                {QStringLiteral("automaticFallback"),
                 it.value().automaticFallback},
                {QStringLiteral("reason"), it.value().reason},
                {QStringLiteral("decidedOn"), it.value().decidedOn},
                {QStringLiteral("lastRung"), it.value().lastRung}});
    }
    root.insert(
        QStringLiteral("capture"),
        QJsonObject{
            {QStringLiteral("accelerationAttempt"),
             settings.cameraAccelerationAttempt},
            {QStringLiteral("cameras"), cameraAcceleration}});

    QJsonObject ui;
    ui.insert(QStringLiteral("virtualJoystick"), settings.virtualJoystick);
    ui.insert(QStringLiteral("zoomWheelAcceleration"),
              settings.zoomWheelAcceleration);
    ui.insert(QStringLiteral("controlsCollapsed"), settings.controlsCollapsed);
    ui.insert(QStringLiteral("simpleUiMode"), settings.simpleUiMode);
    ui.insert(QStringLiteral("advancedPanelWidth"), settings.advancedPanelWidth);
    ui.insert(QStringLiteral("viewportRateMode"),
              static_cast<int>(settings.viewportRateMode));
    ui.insert(QStringLiteral("viewportFitMode"),
              static_cast<int>(settings.viewportFitMode));
    QJsonObject sectionStates;
    for (auto it = settings.uiSectionStates.cbegin();
         it != settings.uiSectionStates.cend(); ++it) {
        sectionStates.insert(it.key(), it.value());
    }
    ui.insert(QStringLiteral("sectionStates"), sectionStates);
    ui.insert(QStringLiteral("selectedPresetId"), settings.selectedPresetId);
    ui.insert(QStringLiteral("setupAssistantDeclined"), settings.setupAssistantDeclined);
    if (settings.assistiveOverlayGeometry.isValid()) {
        const QRect& geometry = settings.assistiveOverlayGeometry;
        ui.insert(QStringLiteral("assistiveOverlayGeometry"),
                  QJsonObject{{QStringLiteral("x"), geometry.x()},
                              {QStringLiteral("y"), geometry.y()},
                              {QStringLiteral("width"), geometry.width()},
                              {QStringLiteral("height"), geometry.height()}});
    }
    root.insert(QStringLiteral("ui"), ui);
    root.insert(
        QStringLiteral("recording"),
        QJsonObject{{QStringLiteral("canvasMode"),
                     static_cast<int>(settings.recordingCanvasMode)}});

    root.insert(
        QStringLiteral("annotations"),
        QJsonObject{
            {QStringLiteral("color"), settings.annotationColor},
            {QStringLiteral("widthPixels"),
             std::clamp(settings.annotationWidthPixels, 2, 24)},
            {QStringLiteral("captureOnExit"),
             settings.annotationCaptureOnExit},
            {QStringLiteral("dashed"), settings.annotationDashed},
            {QStringLiteral("shapeKind"),
             settings.annotationShapeKind == QStringLiteral("ellipse")
                 ? QStringLiteral("ellipse")
                 : QStringLiteral("rectangle")},
            {QStringLiteral("textSizePixels"),
             std::clamp(settings.annotationTextSizePixels, 12, 72)}});

    root.insert(QStringLiteral("assistive"), AssistiveToJson(settings.assistive));

    QJsonArray conversationArray;
    for (const CodexConversation& conversation : settings.codexConversations) {
        conversationArray.append(CodexConversationToJson(conversation));
    }
    root.insert(QStringLiteral("codexConversations"), conversationArray);

    root.insert(QStringLiteral("currentConfig"), ConfigToJson(settings.currentConfig));
    if (settings.customColorScheme.stops.size() >= 2) {
        root.insert(QStringLiteral("customColorScheme"),
                    ColorSchemeToJson(settings.customColorScheme));
    }

    QJsonArray configArray;
    for (const AdvancedConfig& config : settings.customConfigs) {
        configArray.append(ConfigToJson(config));
    }
    root.insert(QStringLiteral("customConfigs"), configArray);

    QJsonArray presetArray;
    for (const PresetDefinition& preset : settings.customPresets) {
        presetArray.append(PresetToJson(preset));
    }
    root.insert(QStringLiteral("customPresets"), presetArray);

    const QJsonDocument doc(root);
    const QByteArray serialized = doc.toJson(QJsonDocument::Indented);

    if (QFileInfo::exists(path)) {
        const LoadResult existing = LoadDetailed(path);
        if (existing.status == LoadStatus::Loaded) {
            const QString backupPath = path + QStringLiteral(".backup");
            if (QFileInfo::exists(backupPath) && !QFile::remove(backupPath)) {
                return false;
            }
            if (!QFile::copy(path, backupPath)) {
                return false;
            }
        }
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    if (file.write(serialized) != serialized.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        return false;
    }

    return true;
}

const std::vector<AdvancedConfig>& BuiltInConfigs()
{
    return BuiltInConfigsStorage();
}

const std::vector<PresetDefinition>& BuiltInPresets()
{
    return BuiltInPresetsStorage();
}

QString DefaultPresetId()
{
    return QStringLiteral("preset-reading");
}

const AdvancedConfig* FindAdvancedConfigById(const QString& configId,
                                             const std::vector<AdvancedConfig>& customConfigs)
{
    for (const AdvancedConfig& config : BuiltInConfigsStorage()) {
        if (config.id == configId) {
            return &config;
        }
    }
    for (const AdvancedConfig& config : customConfigs) {
        if (config.id == configId) {
            return &config;
        }
    }
    return nullptr;
}

const PresetDefinition* FindPresetById(const QString& presetId,
                                       const std::vector<PresetDefinition>& customPresets)
{
    for (const PresetDefinition& preset : BuiltInPresetsStorage()) {
        if (preset.id == presetId) {
            return &preset;
        }
    }
    for (const PresetDefinition& preset : customPresets) {
        if (preset.id == presetId) {
            return &preset;
        }
    }
    return nullptr;
}

std::optional<AdvancedConfig> ResolveConfigForPreset(const QString& presetId,
                                                     const std::vector<AdvancedConfig>& customConfigs,
                                                     const std::vector<PresetDefinition>& customPresets)
{
    if (presetId.isEmpty()) {
        return std::nullopt;
    }
    const PresetDefinition* preset = FindPresetById(presetId, customPresets);
    if (!preset) {
        return std::nullopt;
    }
    const AdvancedConfig* config = FindAdvancedConfigById(preset->configId, customConfigs);
    if (!config) {
        return std::nullopt;
    }
    return *config;
}

bool AreConfigsEquivalent(const AdvancedConfig& lhs, const AdvancedConfig& rhs)
{
    auto withinUiStep = [](float a, float b, float halfStep) {
        return std::abs(a - b) <= halfStep + 0.0001f;
    };

    return lhs.blackWhiteEnabled == rhs.blackWhiteEnabled &&
           withinUiStep(lhs.blackWhiteThreshold, rhs.blackWhiteThreshold, 0.5f / 255.0f) &&
           lhs.zoomEnabled == rhs.zoomEnabled &&
           withinUiStep(lhs.zoomAmount, rhs.zoomAmount, 0.005f) &&
           withinUiStep(lhs.zoomCenterX, rhs.zoomCenterX, 0.005f) &&
           withinUiStep(lhs.zoomCenterY, rhs.zoomCenterY, 0.005f) &&
           lhs.blurEnabled == rhs.blurEnabled &&
           withinUiStep(lhs.blurSigma, rhs.blurSigma, 0.05f) &&
           lhs.blurRadius == rhs.blurRadius &&
           lhs.temporalSmoothEnabled == rhs.temporalSmoothEnabled &&
           withinUiStep(lhs.temporalSmoothAlpha, rhs.temporalSmoothAlpha, 0.005f) &&
           lhs.spatialSharpenEnabled == rhs.spatialSharpenEnabled &&
           lhs.spatialUpscaler == rhs.spatialUpscaler &&
           withinUiStep(lhs.spatialSharpness, rhs.spatialSharpness, 0.005f) &&
           lhs.debugView == rhs.debugView &&
           lhs.focusMarker == rhs.focusMarker &&
           SnapRotation(lhs.rotationQuarterTurns) == SnapRotation(rhs.rotationQuarterTurns) &&
           lhs.ocrAssistEnabled == rhs.ocrAssistEnabled &&
           lhs.vlmAssistEnabled == rhs.vlmAssistEnabled &&
           lhs.assistiveOverlayEnabled == rhs.assistiveOverlayEnabled &&
           lhs.stabilizationEnabled == rhs.stabilizationEnabled &&
           color_schemes::SchemesEquivalent(
               lhs.colorScheme.stops.size() >= 2
                   ? lhs.colorScheme
                   : color_schemes::LegacyColorScheme(lhs.displayColorMode),
               rhs.colorScheme.stops.size() >= 2
                   ? rhs.colorScheme
                   : color_schemes::LegacyColorScheme(rhs.displayColorMode)) &&
           withinUiStep(lhs.contrast, rhs.contrast, 0.005f) &&
           withinUiStep(lhs.brightness, rhs.brightness, 0.005f) &&
           lhs.keystoneEnabled == rhs.keystoneEnabled &&
           lhs.autoContrastEnabled == rhs.autoContrastEnabled &&
           withinUiStep(lhs.autoContrastStrength, rhs.autoContrastStrength, 0.005f) &&
           lhs.autoTextClarityEnabled == rhs.autoTextClarityEnabled &&
           lhs.backgroundFlattenEnabled == rhs.backgroundFlattenEnabled &&
           withinUiStep(lhs.backgroundFlattenStrength, rhs.backgroundFlattenStrength, 0.005f) &&
           lhs.adaptiveBinarizationEnabled == rhs.adaptiveBinarizationEnabled &&
           withinUiStep(lhs.sauvolaStrength, rhs.sauvolaStrength, 0.005f) &&
           withinUiStep(lhs.binarizationSoftness, rhs.binarizationSoftness, 0.005f) &&
           lhs.textPolarityMode == rhs.textPolarityMode &&
           lhs.strokeWeight == rhs.strokeWeight &&
           lhs.smartSharpenEnabled == rhs.smartSharpenEnabled &&
           withinUiStep(lhs.smartSharpenStrength, rhs.smartSharpenStrength, 0.005f) &&
           lhs.claheEnabled == rhs.claheEnabled &&
           withinUiStep(lhs.claheClipLimit, rhs.claheClipLimit, 0.05f) &&
           lhs.twoColorTextEnabled == rhs.twoColorTextEnabled &&
           lhs.textHysteresisEnabled == rhs.textHysteresisEnabled &&
           withinUiStep(lhs.textHysteresisStrength, rhs.textHysteresisStrength, 0.005f) &&
           lhs.selectiveSharpenEnabled == rhs.selectiveSharpenEnabled &&
           lhs.focusDetectionEnabled == rhs.focusDetectionEnabled &&
           withinUiStep(lhs.focusThreshold, rhs.focusThreshold, 0.0005f) &&
           lhs.glareSuppressionEnabled == rhs.glareSuppressionEnabled &&
           withinUiStep(lhs.glareSuppressionStrength, rhs.glareSuppressionStrength, 0.005f) &&
           lhs.mlSuperResEnabled == rhs.mlSuperResEnabled &&
           withinUiStep(lhs.mlSuperResStrength, rhs.mlSuperResStrength, 0.005f) &&
           lhs.mlSuperResPrefer2x == rhs.mlSuperResPrefer2x &&
           lhs.mlSuperResUltra1440p == rhs.mlSuperResUltra1440p;
}

} // namespace openzoom::settings

#endif // _WIN32
