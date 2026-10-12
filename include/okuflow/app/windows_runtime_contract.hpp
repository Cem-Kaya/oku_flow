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
std::wstring QuoteWindowsArgument(const std::wstring& argument);
WindowsRuntimeInstallResult ClassifyWindowsRuntimeInstallResult(std::uint32_t exitCode, bool installed);
} // namespace okuflow
