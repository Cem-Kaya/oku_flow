#pragma once

#include <QString>

namespace openzoom::debug_log {

// Installs a Qt message tee only when the process has an attached Windows
// console. Messages emitted before the user-data root is known are buffered.
void InstallIfConsoleAttached();

// Opens one log file below directory and flushes buffered startup messages.
bool SetOutputDirectory(const QString& directory, QString* error = nullptr);

bool IsEnabled();
QString CurrentPath();

// Restores the prior Qt message handler and closes the current log.
void Shutdown();

} // namespace openzoom::debug_log
