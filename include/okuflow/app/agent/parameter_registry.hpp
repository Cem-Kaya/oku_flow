#pragma once
#include "okuflow/app/agent/control_registry.hpp"
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace okuflow::agent {
enum class ParameterGroup { Image, View, Camera, Recording, Assistant, Notes, Ui, Annotation };
enum class ParameterPermission {
    View,
    Source,
    CaptureConfiguration,
    AssistantPreference,
    ProviderConfiguration,
    FrameEgress,
    OutputDestination,
    UiPreference,
    AnnotationPreference,
    AnnotationEdit,
    PhotoEgress
};
enum class ParameterAccess { WritableWithPolicy, UiOnly };
struct ParameterDescriptor {
    std::string_view id;
    ParameterGroup group;
    std::string_view schema;
    std::string_view units;
    std::string_view persistence;
    ParameterPermission permission;
    ParameterAccess access;
    bool requiresUiApproval;
    std::string_view nativeSource;
    std::string_view nativeHandler;
    std::string_view availability;
};
struct ProtectedControlDescriptor {
    std::string_view id, category;
    bool secret;
};
struct ControlActionDescriptor {
    std::string_view id, status, permission;
};
// Immutable first-party metadata. No runtime setting, device path or secret value.
std::span<const ParameterDescriptor> ParameterRegistry();
const ParameterDescriptor *FindParameter(std::string_view id);
std::span<const ProtectedControlDescriptor> ProtectedControlRegistry();
std::span<const ControlActionDescriptor> ControlActionRegistry();
std::string_view ParameterGroupName(ParameterGroup group);
std::string_view ParameterPermissionName(ParameterPermission permission);
bool ParameterGroupForTool(ToolId tool, ParameterGroup &group, bool &setter);
std::string ParameterGetSchema(ParameterGroup group);
std::string ParameterSetSchema(ParameterGroup group);
// Compact metadata only; target supplies honest runtime support/effective state.
// The page contains <=16 combined parameter/protected/action entries.
std::string ParameterMetadataPage(std::string_view group = {}, std::string_view afterField = {},
                                  std::size_t limit = 16,
                                  CatalogEdition edition = BuiltControlCatalogEdition());
} // namespace okuflow::agent
