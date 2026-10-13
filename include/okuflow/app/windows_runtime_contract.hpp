#pragma once
#include <array>
#include <cstdint>
#include <string>
namespace okuflow {
using WindowsRuntimeVersion = std::array<std::uint32_t, 4>;
struct WindowsRuntimeStatus {
    bool installed{};
    bool Ready() const { return installed; }
};
enum class WindowsRuntimeInstallResult { Success, RestartRequired, Canceled, Failed };
bool SupportsWindowsRuntimeVersion(const WindowsRuntimeVersion& version);
bool IsSystemWindowsRuntimeReady();
bool VerifyWindowsRuntimeInstallerFile(const std::wstring& path);
std::wstring WindowsExecutableDirectory();
// New bundles use app/; flat developer/legacy bundles remain supported.
// An existing app/ is authoritative so an incomplete update cannot fall
// back silently to a stale flat child.
std::wstring WindowsRuntimePayloadDirectory(const std::wstring& bundleDirectory);
std::wstring QuoteWindowsArgument(const std::wstring& argument);
WindowsRuntimeInstallResult ClassifyWindowsRuntimeInstallResult(std::uint32_t exitCode, bool installed);
} // namespace okuflow
