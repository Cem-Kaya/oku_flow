#pragma once

#include <span>
#include <string>
#include <string_view>

namespace okuflow::agent {

enum class CatalogEdition { DevCore = 0, DevCapture = 1, DevCamera = 2, V1 = 3, DevFull = 4 };
enum class Tier { Read, Egress, View, Source, Capture };
enum class ToolId {
    State,
    Operation,
    Wait,
    Metrics,
    Observe,
    Modes,
    ModeActivate,
    Zoom,
    Center,
    ResetView,
    ReadingColors,
    Contrast,
    AutoAdjust,
    Orientation,
    Photo,
    RecordingStart,
    RecordingStop,
    CameraSources,
    CameraSelect,
    ControlsList,
    ImageSettingsGet,
    ImageSettingsSet,
    ViewSettingsGet,
    ViewSettingsSet,
    CameraSettingsGet,
    CameraSettingsSet,
    RecordingSettingsGet,
    RecordingSettingsSet,
    AssistantPreferencesGet,
    AssistantPreferencesSet,
    NotesPreferencesGet,
    NotesPreferencesSet,
    UiPreferencesGet,
    UiPreferencesSet,
    AnnotationPreferencesGet,
    AnnotationPreferencesSet,
    VideoState,
    VideoPause,
    VideoResume,
    VideoSeek,
    VideoJumpLatestEnd,
    VideoReturnCamera,
    VideoSaveFrame
};

// This catalog has no Qt/runtime dependency. Listing grants no authority.
struct ToolDescriptor {
    ToolId id;
    std::string_view canonicalId;
    std::string_view wireName;
    std::string_view description;
    Tier tier;
    bool readOnly;
    bool idempotent;
    bool composite;
    bool pending;
    std::string inputSchema;
    std::string outputSchema;
};

std::string_view CatalogEditionName(CatalogEdition edition);
CatalogEdition BuiltControlCatalogEdition() noexcept;
// Advisory MCP metadata only; never use this hint as admission authority.
bool ControlToolDestructiveHint(ToolId id) noexcept;
bool IsVideoControlTool(ToolId id) noexcept;
std::span<const ToolDescriptor> ControlRegistry(CatalogEdition edition);
const ToolDescriptor *FindControlTool(std::string_view name, CatalogEdition edition);

} // namespace okuflow::agent
