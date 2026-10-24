#pragma once

#include <string_view>
#include <utility>

namespace okuflow::agent {

// Neutral label for any key outside the finite receipt vocabulary, including
// text that did not originate from the app's own field names.
inline constexpr std::string_view kReceiptFallbackLabel = "Settings";

// Maps an app-normalized receipt key (a canonical parameter id such as
// "image.contrast" or a legacy FieldName alias such as "contrast") to the
// English source text of an existing translation-catalog entry. Callers
// translate the result with TranslateUi and replace "&&" with "&". Pure and
// desktop-free; it never echoes the key itself.
inline std::string_view ReceiptLabelSource(std::string_view key) {
    // Legacy FieldName spellings that differ from their canonical parameter id.
    static constexpr std::pair<std::string_view, std::string_view> aliases[]{
        {"zoomEnabled", "view.zoom.enabled"}, {"zoomAmount", "view.zoom.amount"},
        {"centerX", "view.center.x"}, {"centerY", "view.center.y"}, {"framing", "view.fit"},
        {"rotation", "camera.orientation"}, {"camera", "camera.selection"},
        {"blackWhite", "image.blackWhite.enabled"}, {"threshold", "image.blackWhite.threshold"},
        {"blur", "image.blur.enabled"}, {"blurSigma", "image.blur.sigma"}, {"blurRadius", "image.blur.radius"},
        {"temporal", "image.temporal.enabled"}, {"temporalAlpha", "image.temporal.alpha"},
        {"spatial", "image.spatial.enabled"}, {"upscaler", "image.spatial.backend"},
        {"sharpness", "image.spatial.sharpness"}, {"focusMarker", "view.focusMarker"},
        {"stabilization", "image.stabilization.enabled"}, {"keystone", "image.keystone.enabled"},
        {"scheme", "image.colors.schemeId"}, {"contrast", "image.contrast"}, {"brightness", "image.brightness"},
        {"autoContrast", "image.autoContrast.enabled"}, {"autoContrastStrength", "image.autoContrast.strength"},
        {"textClarity", "image.textClarity.enabled"}, {"flatten", "image.textClarity.flatten.enabled"},
        {"flattenStrength", "image.textClarity.flatten.strength"}, {"adaptive", "image.textClarity.adaptive.enabled"},
        {"sauvola", "image.textClarity.adaptive.sauvolaStrength"}, {"softness", "image.textClarity.adaptive.softness"},
        {"polarity", "image.textClarity.polarity"}, {"stroke", "image.textClarity.strokeWeight"},
        {"smartSharpen", "image.textClarity.smartSharpen.enabled"},
        {"smartStrength", "image.textClarity.smartSharpen.strength"}, {"clahe", "image.textClarity.clahe.enabled"},
        {"claheLimit", "image.textClarity.clahe.clipLimit"}, {"twoColor", "image.textClarity.twoColor"},
        {"hysteresis", "image.textClarity.hysteresis.enabled"},
        {"hysteresisStrength", "image.textClarity.hysteresis.strength"},
        {"selectiveSharpen", "image.textClarity.selectiveSharpen"},
        {"focusDetection", "image.textClarity.focusDetection.enabled"},
        {"focusThreshold", "image.textClarity.focusDetection.threshold"}, {"glare", "image.textClarity.glare.enabled"},
        {"glareStrength", "image.textClarity.glare.strength"}, {"image.debug", "image.debugView"},
        {"image.superRes.enabled", "image.superResolution.enabled"},
        {"image.superRes.strength", "image.superResolution.strength"},
        {"image.superRes.prefer2x", "image.superResolution.prefer2x"},
        {"image.superRes.ultra1440p", "image.superResolution.ultra1440p"},
        {"image.superRes.performanceOverride", "image.superResolution.performanceOverride"}};
    // Canonical ids plus legacy-only receipt keys, each bound to the caption
    // users already see for that control.
    static constexpr std::pair<std::string_view, std::string_view> labels[]{
        {"image.uct.enabled","Ultra Clear Text on this view"},
        {"image.uct.showOriginal","Original camera text shown."},
        {"image.uct.gpu.detector","GPU text detector"},
        {"image.uct.gpu.recognizer","GPU text recognizer"},
        {"image.uct.cpu.detector","CPU text detector"},
        {"image.uct.cpu.recognizer","CPU text recognizer"},
        {"view.zoom.enabled", "Zoom"}, {"view.zoom.amount", "Zoom"},
        {"view.center.x", "Horizontal position"}, {"view.center.y", "Vertical position"},
        {"view.focusMarker", "Show Focus Point"}, {"view.fit", "Viewport framing"},
        {"view.presentationRate", "Viewport motion rate"}, {"view.virtualJoystick", "Virtual Joystick"},
        {"view.zoomWheelAcceleration", "Zoom wheel acceleration"},
        {"image.blackWhite.enabled", "Black && White"}, {"image.blackWhite.threshold", "Black && White"},
        {"image.contrast", "Contrast"}, {"image.brightness", "Brightness"},
        {"image.autoContrast.enabled", "Auto Contrast"}, {"image.autoContrast.strength", "Auto Contrast"},
        {"image.blur.enabled", "Soften image (blur)"}, {"image.blur.sigma", "Blur sigma"},
        {"image.blur.radius", "Blur radius"}, {"image.temporal.enabled", "Temporal Smooth"},
        {"image.temporal.alpha", "Temporal blend"}, {"image.spatial.enabled", "Spatial Sharpen"},
        {"image.spatial.backend", "Spatial Sharpen"}, {"image.spatial.sharpness", "Sharpness"},
        {"image.stabilization.enabled", "Stabilize Image"}, {"image.keystone.enabled", "Straighten Screen (Keystone)"},
        {"image.debugView", "Debug View"}, {"image.textClarity.enabled", "Text Clarity"},
        {"image.textClarity.flatten.enabled", "Flatten Background"},
        {"image.textClarity.flatten.strength", "Flatten Background"},
        {"image.textClarity.adaptive.enabled", "Adaptive Text"},
        {"image.textClarity.adaptive.sauvolaStrength", "Adaptive Text"},
        {"image.textClarity.adaptive.softness", "Edge softness"}, {"image.textClarity.polarity", "Text polarity"},
        {"image.textClarity.strokeWeight", "Stroke weight"},
        {"image.textClarity.smartSharpen.enabled", "Smart Sharpen"},
        {"image.textClarity.smartSharpen.strength", "Smart Sharpen"},
        {"image.textClarity.clahe.enabled", "Local Contrast (CLAHE)"},
        {"image.textClarity.clahe.clipLimit", "Local Contrast (CLAHE)"},
        {"image.textClarity.twoColor", "Two-Color Reading"},
        {"image.textClarity.hysteresis.enabled", "Steady Text Edges"},
        {"image.textClarity.hysteresis.strength", "Steady Text Edges"},
        {"image.textClarity.selectiveSharpen", "Sharpen Text Only"},
        {"image.textClarity.focusDetection.enabled", "Warn When Out of Focus"},
        {"image.textClarity.focusDetection.threshold", "Warn When Out of Focus"},
        {"image.textClarity.glare.enabled", "Suppress Glare"}, {"image.textClarity.glare.strength", "Suppress Glare"},
        {"image.superResolution.enabled", "NVIDIA Super Resolution"},
        {"image.superResolution.strength", "NVIDIA Super Resolution"},
        {"image.superResolution.prefer2x", "Faster 2x mode (narrower view)"},
        {"image.superResolution.ultra1440p", "Ultra quality (full frame, up to 1440p)"},
        {"image.superResolution.performanceOverride", "Ignore 24 ms performance limit"},
        {"image.stabilization.bumpHold", "Extra Stable (hold on shake)"},
        {"image.keystone.paused", "Automatic screen correction"}, {"image.colors.schemeId", "Display colors"},
        {"image.colors.custom.mode", "Custom color mode"}, {"image.colors.custom.stopCount", "Number of color stops"},
        {"image.colors.custom.stops", "Custom colors"}, {"image.colors.custom.stepped", "Stepped colors"},
        {"customColorRecipe", "Custom display colors"},
        {"camera.orientation", "Orientation"}, {"camera.selection", "Camera"},
        {"camera.format", "Camera resolution && frame rate"}, {"camera.acceleration", "Camera acceleration"},
        {"camera.phoneTorch", "Phone light"}, {"recording", "Recording"},
        {"recording.microphone", "Microphone"}, {"recording.canvas", "Processed recording resolution"},
        {"assistant_preferences.sceneExplain", "Scene Explain"},
        {"assistant_preferences.overlayEnabled", "Assistive Overlay"},
        {"assistant_preferences.provider", "AI Provider"}, {"assistant_preferences.codexModel", "Codex Model"},
        {"assistant_preferences.codexReasoning", "Codex Reasoning"}, {"assistant_preferences.httpModel", "Model"},
        {"assistant_preferences.instructions", "Assistant Instructions"},
        {"assistant_preferences.explainPrompt", "Explain Prompt"},
        {"assistant_preferences.ttsVoice", "Read Aloud Voice"}, {"assistant_preferences.ttsRate", "Read Aloud Speed"},
        {"notes_output.lectureNotes", "Write Lecture Notes File"}, {"notes_output.root", "Notes and files"},
        {"preset", "Quick mode"}, {"presetList", "Quick mode"},
        // Layout preferences without their own caption keep the neutral label.
        {"ui.mode", kReceiptFallbackLabel}, {"ui.activeTab", kReceiptFallbackLabel},
        {"ui.sectionExpanded", kReceiptFallbackLabel}, {"ui.inspectorWidth", "Resize Advanced settings"},
        {"ui.language", "Language"}, {"ui.hidden", "Hide UI"}, {"ui.assistiveDock", "Panel position"},
        {"ui.assistiveGeometry", "Panel position"}, {"ui.setupPromptDeclined", "Setup Assistant"},
        {"annotation.color", "Pen options"}, {"annotation.width", "Annotation thickness"},
        {"annotation.textSize", "Text size"}, {"annotation.dashed", "Dashed stroke"},
        {"annotation.shape", "Shape"}, {"annotation.captureOnExit", "Save drawing to notes when leaving Draw"},
        {"annotation.tool", "Annotation tool"}, {"annotation.enabled", "Draw"}};
    for (const auto& [legacy, canonical] : aliases)
        if (legacy == key) { key = canonical; break; }
    for (const auto& [id, label] : labels)
        if (id == key) return label;
    return kReceiptFallbackLabel;
}

} // namespace okuflow::agent
