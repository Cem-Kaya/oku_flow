#pragma once

#include "okuflow/app/windows_runtime_contract.hpp"
#include <QString>

namespace okuflow {

WindowsRuntimeStatus CheckWindowsRuntime();
bool VerifyWindowsRuntimeInstaller(const QString& path);

} // namespace okuflow
