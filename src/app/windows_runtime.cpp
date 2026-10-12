#ifdef _WIN32
#include "okuflow/app/windows_runtime.hpp"
namespace okuflow {
WindowsRuntimeStatus CheckWindowsRuntime() { return {IsSystemWindowsRuntimeReady()}; }
bool VerifyWindowsRuntimeInstaller(const QString& path) {
    return VerifyWindowsRuntimeInstallerFile(path.toStdWString());
}
} // namespace okuflow
#endif
