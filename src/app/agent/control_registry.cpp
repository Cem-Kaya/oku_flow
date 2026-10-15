#include "okuflow/app/agent/control_registry.hpp"
#include "okuflow/app/agent/parameter_registry.hpp"

#include <array>
#include <vector>

namespace okuflow::agent {
namespace {
constexpr auto kSchema = "\"$schema\":\"http://json-schema.org/draft-07/"
                         "schema#\",\"type\":\"object\",\"additionalProperties\":false,";
std::string Object(std::string_view properties, std::string_view required = "[]") {
    return std::string("{") + kSchema + "\"properties\":" + std::string(properties) +
           ",\"required\":" + std::string(required) + "}";
}
std::string Point() {
    return R"({"oneOf":[{"type":"object","additionalProperties":false,"properties":{"space":{"const":"scene"},"x":{"type":"number","minimum":0,"maximum":1},"y":{"type":"number","minimum":0,"maximum":1}},"required":["space","x","y"]},{"type":"object","additionalProperties":false,"properties":{"space":{"const":"observation"},"observationId":{"type":"string","minLength":1,"maxLength":64},"px":{"type":"number","minimum":0,"maximum":1024},"py":{"type":"number","minimum":0,"maximum":1024}},"required":["space","observationId","px","py"]}]})";
}
std::string Output(bool pending) {
    const std::string success =
        R"({"type":"object","properties":{"ok":{"const":true},"pending":{"const":false}},"required":["ok"],"not":{"required":["pending"],"properties":{"pending":{"const":true}}}})";
    const std::string asynchronous =
        R"({"type":"object","properties":{"ok":{"const":true},"pending":{"const":true},"operationId":{"type":"string"},"state":{"type":"string"}},"required":["ok","pending","operationId","state"]})";
    return std::string(
               "{\"$schema\":\"http://json-schema.org/draft-07/schema#\",\"type\":\"object\",\"oneOf\":[") +
           success + (pending ? "," + asynchronous : "") + "]}";
}
std::vector<ToolDescriptor> MakeCatalog(CatalogEdition edition) {
    std::vector<ToolDescriptor> tools;
    auto add = [&](ToolId id, std::string_view canonical, std::string_view wire, std::string_view description,
                   Tier tier, bool idempotent, bool composite, bool pending, std::string schema) {
        tools.push_back({id, canonical, wire, description, tier, tier == Tier::Read, idempotent, composite,
                         pending, std::move(schema), Output(pending)});
    };
    add(ToolId::State, "app.state.get", "app_state_get",
        "Read bounded application state and field revisions. Re-read after stale_state.", Tier::Read, true,
        false, false, Object(R"({"sinceEvent":{"type":"integer","minimum":0,"maximum":9007199254740991}})"));
    add(ToolId::Operation, "app.operation.get", "app_operation_get",
        "Read a durable operation by its opaque operationId.", Tier::Read, true, false, false,
        Object(R"({"operationId":{"type":"string","minLength":1,"maxLength":64}})", R"(["operationId"])"));
    add(ToolId::Wait, "app.wait_for", "app_wait_for",
        "Wait up to five seconds for a state condition; does not grant consent.", Tier::Read, true, false,
        false,
        Object(
            R"({"condition":{"enum":["camera_ready","view_presented","frame_processed","operation_state","grant_changed"]},"timeoutMs":{"type":"integer","minimum":0,"maximum":5000},"operationId":{"type":"string","minLength":1,"maxLength":64}})",
            R"(["condition","timeoutMs"])"));
    add(ToolId::Metrics, "view.metrics", "view_metrics",
        "Read pixel-free current-view metrics and freshness stamps.", Tier::Read, true, false, false,
        Object("{}"));
    add(ToolId::Observe, "view.observe", "view_observe",
        "Send a camera viewport to the provider under an explicit frame grant.", Tier::Egress, false, false,
        false, Object(R"({"maxLongSide":{"type":"integer","minimum":384,"maximum":1024,"default":768}})"));
    add(ToolId::Modes, "mode.list", "mode_list",
        "List mode and reading-color ids. Select by id and speak displayName.", Tier::Read, true, false,
        false, Object("{}"));
    add(ToolId::ModeActivate, "mode.activate", "mode_activate",
        "Apply an agent-applicable quick mode by presetId; does not move focus.", Tier::View, false, true,
        false, Object(R"({"presetId":{"type":"string","pattern":"^[a-z0-9-]{1,64}$"}})", R"(["presetId"])"));
    add(ToolId::Zoom, "view.zoom.set", "view_zoom_set",
        "Set camera-view magnification, not phone optical zoom. Anchor uses scene or observation "
        "coordinates.",
        Tier::View, true, false, false,
        Object(std::string(R"({"magnification":{"type":"number","minimum":1,"maximum":16},"anchor":)") +
                   Point() + "}",
               R"(["magnification"])"));
    add(ToolId::Center, "view.center.set", "view_center_set",
        "Pan to a scene point or a point in a fresh observation.", Tier::View, true, false, false,
        Object(std::string("{\"point\":") + Point() + "}", R"(["point"])"));
    add(ToolId::ResetView, "view.reset", "view_reset", "Reset magnification to one and center the view.",
        Tier::View, true, false, false, Object("{}"));
    add(ToolId::ReadingColors, "display.reading_colors.set", "display_reading_colors_set",
        "Choose grayscale for photos, or high-contrast text colors. Source text polarity remains automatic.",
        Tier::View, true, true, false,
        Object(
            R"({"intent":{"enum":["photographic_color","grayscale","high_contrast_dark_on_light","high_contrast_light_on_dark","scheme"]},"schemeId":{"type":"string","pattern":"^[a-z0-9-]{1,64}$"}})",
            R"(["intent"])"));
    add(ToolId::Contrast, "image.contrast.set", "image_contrast_set",
        "Set contrast multiplier and brightness offset, snapped to hundredths.", Tier::View, true, false,
        false,
        Object(
            R"({"contrast":{"type":"number","minimum":0.25,"maximum":4},"brightness":{"type":"number","minimum":-1,"maximum":1}})",
            R"(["contrast","brightness"])"));
    add(ToolId::AutoAdjust, "image.auto_adjust", "image_auto_adjust",
        "Apply automatic image adjustments for a goal and report the exact delta.", Tier::View, false, true,
        true,
        Object(
            R"({"goal":{"enum":["read_text","see_scene","projected_screen","board","low_light"]},"allowModeChange":{"type":"boolean"}})",
            R"(["goal","allowModeChange"])"));
    add(ToolId::Orientation, "camera.orientation.set", "camera_orientation_set",
        "Rotate camera content clockwise in quarter turns. Requires approval; forbidden during recording.",
        Tier::Source, true, false, false,
        Object(R"({"degreesClockwise":{"enum":[0,90,180,270]}})", R"(["degreesClockwise"])"));
    if (edition != CatalogEdition::DevCore) {
        add(ToolId::Photo, "capture.photo.take", "capture_photo_take",
            "Take one paired photo under a photo grant or exact approval.", Tier::Capture, false, false, true,
            Object("{}"));
        add(ToolId::RecordingStart, "capture.recording.start", "capture_recording_start",
            "Start an owned recording with saved microphone or no audio; requires capture consent.",
            Tier::Capture, false, false, true,
            Object(R"({"audio":{"enum":["saved_microphone","none"]}})", R"(["audio"])"));
        add(ToolId::RecordingStop, "capture.recording.stop", "capture_recording_stop",
            "Stop the exact owned session. Stopping a manual session requires exact approval.", Tier::Capture,
            true, false, true, Object(R"({"sessionId":{"type":"string","minLength":1,"maxLength":64}})"));
    }
    if (edition == CatalogEdition::DevCamera || edition == CatalogEdition::V1 ||
        edition == CatalogEdition::DevFull) {
        add(ToolId::CameraSources, "camera.sources.list", "camera_sources_list",
            "List session-unique camera handles and generated safe labels.", Tier::Read, true, false, false,
            Object("{}"));
        add(ToolId::CameraSelect, "camera.select", "camera_select",
            "Temporarily select a camera by session handle; requires source consent.", Tier::Source, true,
            false, true,
            Object(R"({"cameraHandle":{"type":"string","pattern":"^cam-[a-f0-9]{32}$"}})",
                   R"(["cameraHandle"])"));
    }
    if (edition == CatalogEdition::DevFull || edition == CatalogEdition::V1) {
        add(ToolId::ControlsList, "app.controls.list", "app_controls_list",
            "List up to 16 immutable control descriptors after a stable field id. Metadata grants no "
            "control; target support and native-only workflows are explicit.",
            Tier::Read, true, false, false,
            Object(
                R"({"group":{"enum":["image","view","camera","recording","assistant_preferences","notes_output","ui","annotation"]},"afterField":{"type":"string","minLength":1,"maxLength":128,"pattern":"^[A-Za-z0-9_.-]+$"},"limit":{"type":"integer","minimum":1,"maximum":16}})"));
        constexpr std::array gets{ToolId::ImageSettingsGet,        ToolId::ViewSettingsGet,
                                  ToolId::CameraSettingsGet,       ToolId::RecordingSettingsGet,
                                  ToolId::AssistantPreferencesGet, ToolId::NotesPreferencesGet,
                                  ToolId::UiPreferencesGet,        ToolId::AnnotationPreferencesGet};
        static constexpr std::array names{"image.settings",     "view.settings",         "camera.settings",
                                          "recording.settings", "assistant.preferences", "notes.preferences",
                                          "ui.preferences",     "annotation.preferences"};
        static constexpr std::array wires{"image_settings",     "view_settings",         "camera_settings",
                                          "recording_settings", "assistant_preferences", "notes_preferences",
                                          "ui_preferences",     "annotation_preferences"};
        // Descriptor names must outlive every published span.
        static const auto canonical = [] {
            std::array<std::array<std::string, 2>, 8> out;
            for (std::size_t i = 0; i < names.size(); ++i)
                out[i] = {std::string(names[i]) + ".get", std::string(names[i]) + ".set"};
            return out;
        }();
        static const auto wire = [] {
            std::array<std::array<std::string, 2>, 8> out;
            for (std::size_t i = 0; i < wires.size(); ++i)
                out[i] = {std::string(wires[i]) + "_get", std::string(wires[i]) + "_set"};
            return out;
        }();
        for (std::size_t i = 0; i < gets.size(); ++i) {
            const auto group = static_cast<ParameterGroup>(i);
            const auto getDescription =
                group == ParameterGroup::Assistant
                    ? "Read assistant preferences with valueKind all (default), requested or saved. Use "
                      "fields and projection after filterRequired; saved-only reads do not refresh current "
                      "mutation baselines. No credentials or grants."
                    : "Read finite requested/effective/saved parameters and revisions. Unsupported "
                      "parameters remain explicit; no credentials, grants or device paths.";
            add(gets[i], canonical[i][0], wire[i][0], getDescription, Tier::Read, true, false, false,
                ParameterGetSchema(group));
            const auto setter = static_cast<ToolId>(static_cast<int>(gets[i]) + 1);
            const auto tier = group == ParameterGroup::Camera      ? Tier::Source
                              : group == ParameterGroup::Recording ? Tier::Capture
                                                                   : Tier::View;
            add(setter, canonical[i][1], wire[i][1],
                "Apply a nonempty finite parameter patch through shared native setters. All revisions, "
                "per-field permissions, coupled effects and hardware availability must pass before applying; "
                "unsupported fields return unavailable.",
                tier, true, false, true, ParameterSetSchema(group));
        }
    }
    return tools;
}
} // namespace
std::string_view CatalogEditionName(CatalogEdition edition) {
    switch (edition) {
    case CatalogEdition::DevCore:
        return "dev-core";
    case CatalogEdition::DevCapture:
        return "dev-capture";
    case CatalogEdition::DevCamera:
        return "dev-camera";
    case CatalogEdition::V1:
        return "v1";
    case CatalogEdition::DevFull:
        return "dev-full";
    }
    return "invalid";
}
std::span<const ToolDescriptor> ControlRegistry(CatalogEdition edition) {
    static const std::array catalogs{MakeCatalog(CatalogEdition::DevCore),
                                     MakeCatalog(CatalogEdition::DevCapture),
                                     MakeCatalog(CatalogEdition::DevCamera), MakeCatalog(CatalogEdition::V1),
                                     MakeCatalog(CatalogEdition::DevFull)};
    const auto index = static_cast<std::size_t>(edition);
    if (index >= catalogs.size())
        return {};
    return catalogs[index];
}
bool ControlToolDestructiveHint(ToolId id) noexcept {
    switch (id) {
    case ToolId::State:
    case ToolId::Operation:
    case ToolId::Wait:
    case ToolId::Metrics:
    case ToolId::Observe:
    case ToolId::Modes:
    case ToolId::Photo:
    case ToolId::RecordingStart:
    case ToolId::CameraSources:
    case ToolId::ControlsList:
    case ToolId::ImageSettingsGet:
    case ToolId::ViewSettingsGet:
    case ToolId::CameraSettingsGet:
    case ToolId::RecordingSettingsGet:
    case ToolId::AssistantPreferencesGet:
    case ToolId::NotesPreferencesGet:
    case ToolId::UiPreferencesGet:
    case ToolId::AnnotationPreferencesGet:
        return false;
    default:
        // Replacement/reset/stop operations may modify existing state. A
        // grouped setter is conservative because one patch can replace saved
        // preferences, microphone intent, output destination or drawing state.
        return true;
    }
}
CatalogEdition BuiltControlCatalogEdition() noexcept {
#if defined(OKUFLOW_CONTROL_CATALOG_EDITION_INDEX)
    static_assert(OKUFLOW_CONTROL_CATALOG_EDITION_INDEX >= 0 && OKUFLOW_CONTROL_CATALOG_EDITION_INDEX <= 4);
    return static_cast<CatalogEdition>(OKUFLOW_CONTROL_CATALOG_EDITION_INDEX);
#else
    return CatalogEdition::DevCamera;
#endif
}
const ToolDescriptor *FindControlTool(std::string_view name, CatalogEdition edition) {
    for (const auto &tool : ControlRegistry(edition))
        if (tool.canonicalId == name || tool.wireName == name)
            return &tool;
    return nullptr;
}
} // namespace okuflow::agent
