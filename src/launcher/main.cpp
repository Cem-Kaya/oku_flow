#ifdef _WIN32
#include "okuflow/app/windows_runtime_contract.hpp"
#include "launcher_text.hpp"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {
// The launcher runs before Qt and the saved app setting are available, so it
// follows the Windows display language among OkuFlow's 25 UI languages.
const okuflow::launcher::Language kLanguage =
    okuflow::launcher::LanguageFromWindows(GetUserDefaultUILanguage());

std::wstring Text(okuflow::launcher::Message message, const std::wstring& value = {}) {
    std::wstring text = okuflow::launcher::Translate(kLanguage, message);
    const std::size_t placeholder = text.find(L"%1");
    if (placeholder != std::wstring::npos) text.replace(placeholder, 2, value);
    return text;
}
int Show(const std::wstring& heading, const std::wstring& message,
         TASKDIALOG_COMMON_BUTTON_FLAGS buttons, PCWSTR icon) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = okuflow::launcher::IsRightToLeft(kLanguage) ? TDF_RTL_LAYOUT : 0;
    config.dwCommonButtons = buttons;
    config.pszWindowTitle = L"OkuFlow";
    config.pszMainIcon = icon;
    config.pszMainInstruction = heading.c_str();
    config.pszContent = message.c_str();
    int button = 0;
    return SUCCEEDED(TaskDialogIndirect(&config, &button, nullptr, nullptr)) ? button : 0;
}
void Inform(const std::wstring& heading, const std::wstring& message) {
    Show(heading, message, TDCBF_OK_BUTTON, TD_WARNING_ICON);
}

bool PrepareWindowsRuntime(const std::wstring& directory) {
    if (okuflow::IsSystemWindowsRuntimeReady()) return true;
    using okuflow::launcher::Message;
    const int button = Show(Text(Message::InstallTitle), Text(Message::InstallBody),
                            TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON, TD_INFORMATION_ICON);
    if (button != IDOK) return false;

    // Convert the exact unique file owned by GetTempFileName into a private
    // directory. Cleanup removes only our known copy and that empty directory.
    wchar_t temporaryRoot[MAX_PATH]{}, temporaryName[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporaryRoot);
    if (!length || length >= MAX_PATH || !GetTempFileNameW(temporaryRoot, L"Oku", 0, temporaryName) ||
        !DeleteFileW(temporaryName) || !CreateDirectoryW(temporaryName, nullptr)) {
        Inform(Text(Message::PrepareFailedTitle), Text(Message::PrepareFailedBody));
        return false;
    }
    const std::wstring installer = std::wstring(temporaryName) + L"\\vc_redist.x64.exe";
    const auto cleanup = [&] { DeleteFileW(installer.c_str()); RemoveDirectoryW(temporaryName); };
    if (!CopyFileW((directory + L"\\vc_redist.x64.exe").c_str(), installer.c_str(), TRUE) ||
        !okuflow::VerifyWindowsRuntimeInstallerFile(installer)) {
        cleanup();
        Inform(Text(Message::VerificationFailedTitle), Text(Message::VerificationFailedBody));
        return false;
    }
    if (okuflow::IsSystemWindowsRuntimeReady()) { cleanup(); return true; }
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = installer.c_str();
    execute.lpParameters = L"/install /norestart";
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute) || !execute.hProcess) {
        const DWORD error = GetLastError();
        cleanup();
        if (error != ERROR_CANCELLED) Inform(Text(Message::StartFailedTitle), Text(Message::StartFailedBody, std::to_wstring(error)));
        return false;
    }
    DWORD exitCode = 0;
    const DWORD wait = WaitForSingleObject(execute.hProcess, INFINITE);
    const bool read = wait == WAIT_OBJECT_0 && GetExitCodeProcess(execute.hProcess, &exitCode);
    CloseHandle(execute.hProcess);
    cleanup();
    const bool ready = okuflow::IsSystemWindowsRuntimeReady();
    const auto result = read ? okuflow::ClassifyWindowsRuntimeInstallResult(exitCode, ready)
                             : okuflow::WindowsRuntimeInstallResult::Failed;
    if (result == okuflow::WindowsRuntimeInstallResult::Canceled) return false;
    if (result == okuflow::WindowsRuntimeInstallResult::RestartRequired && !ready) {
        Inform(Text(Message::RestartTitle), Text(Message::RestartBody));
        return false;
    }
    if (result == okuflow::WindowsRuntimeInstallResult::Failed || !ready) {
        Inform(Text(Message::IncompleteTitle), Text(Message::IncompleteBody, std::to_wstring(exitCode)));
        return false;
    }
    return true;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto directory = okuflow::WindowsExecutableDirectory();
    if (directory.empty()) return 1;
    const auto payload = okuflow::WindowsRuntimePayloadDirectory(directory);
    if (!PrepareWindowsRuntime(payload)) return 1;
    const std::wstring application = payload + L"\\OkuFlowApp.exe";
    int count = 0;
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 1;
    std::wstring command = okuflow::QuoteWindowsArgument(application);
    bool console = false;
    for (int index = 1; index < count; ++index) {
        command += L" " + okuflow::QuoteWindowsArgument(arguments[index]);
        if (std::wstring(arguments[index]) == L"--console") console = true;
    }
    LocalFree(arguments);
    if (console) AttachConsole(ATTACH_PARENT_PROCESS);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    const bool redirected = startup.hStdOutput && startup.hStdOutput != INVALID_HANDLE_VALUE &&
                            startup.hStdError && startup.hStdError != INVALID_HANDLE_VALUE;
    if (redirected) startup.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, redirected,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        const DWORD error = GetLastError();
        Inform(Text(okuflow::launcher::Message::OpenFailedTitle),
               Text(okuflow::launcher::Message::OpenFailedBody, std::to_wstring(error)));
        return 1;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
#endif
