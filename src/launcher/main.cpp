#ifdef _WIN32
#include "okuflow/app/windows_runtime_contract.hpp"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {
const wchar_t* Text(const wchar_t* english, const wchar_t* turkish, const wchar_t* german) {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_TURKISH: return turkish;
    case LANG_GERMAN: return german;
    default: return english;
    }
}
void Inform(const wchar_t* heading, const std::wstring& message) {
    TaskDialog(nullptr, nullptr, L"OkuFlow", heading, message.c_str(), TDCBF_OK_BUTTON, TD_WARNING_ICON, nullptr);
}

bool PrepareWindowsRuntime(const std::wstring& directory) {
    if (okuflow::IsSystemWindowsRuntimeReady()) return true;
    int button = 0;
    TaskDialog(nullptr, nullptr, L"OkuFlow", Text(L"Install the Microsoft Windows runtime", L"Microsoft Windows çalışma zamanını kurun", L"Microsoft Windows-Laufzeit installieren"),
               Text(L"OkuFlow needs Microsoft's Windows libraries. The installer is included in this package. Select OK to install them. Windows may ask for administrator approval.", L"OkuFlow, Microsoft Windows kitaplıklarına ihtiyaç duyar. Yükleyici bu pakete dahildir. Kurmak için Tamam’ı seçin. Windows yönetici onayı isteyebilir.", L"OkuFlow benötigt Microsofts Windows-Bibliotheken. Das Installationsprogramm ist in diesem Paket enthalten. Wählen Sie OK zur Installation. Windows kann Administratorrechte anfordern."),
               TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON, TD_INFORMATION_ICON, &button);
    if (button != IDOK) return false;

    // Convert the exact unique file owned by GetTempFileName into a private
    // directory. Cleanup removes only our known copy and that empty directory.
    wchar_t temporaryRoot[MAX_PATH]{}, temporaryName[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporaryRoot);
    if (!length || length >= MAX_PATH || !GetTempFileNameW(temporaryRoot, L"Oku", 0, temporaryName) ||
        !DeleteFileW(temporaryName) || !CreateDirectoryW(temporaryName, nullptr)) {
        Inform(Text(L"Could not prepare installation", L"Kurulum hazırlanamadı", L"Installation konnte nicht vorbereitet werden"), Text(L"The temporary installer directory could not be created.", L"Geçici yükleyici klasörü oluşturulamadı.", L"Der temporäre Installationsordner konnte nicht erstellt werden."));
        return false;
    }
    const std::wstring installer = std::wstring(temporaryName) + L"\\vc_redist.x64.exe";
    const auto cleanup = [&] { DeleteFileW(installer.c_str()); RemoveDirectoryW(temporaryName); };
    if (!CopyFileW((directory + L"\\vc_redist.x64.exe").c_str(), installer.c_str(), TRUE) ||
        !okuflow::VerifyWindowsRuntimeInstallerFile(installer)) {
        cleanup();
        Inform(Text(L"Installer verification failed", L"Yükleyici doğrulanamadı", L"Überprüfung des Installationsprogramms fehlgeschlagen"), Text(L"Download and extract the complete OkuFlow ZIP again.", L"Tam OkuFlow ZIP paketini yeniden indirip çıkarın.", L"Laden Sie das vollständige OkuFlow-ZIP erneut herunter und entpacken Sie es."));
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
        if (error != ERROR_CANCELLED) Inform(Text(L"Installation could not start", L"Kurulum başlatılamadı", L"Installation konnte nicht gestartet werden"), std::wstring(Text(L"Windows installer error: ", L"Windows yükleyicisi hatası: ", L"Windows-Installationsfehler: ")) + std::to_wstring(error));
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
        Inform(Text(L"Restart Windows to finish setup", L"Kurulumu bitirmek için Windows’u yeniden başlatın", L"Windows neu starten, um die Einrichtung abzuschließen"), Text(L"Installation completed. Restart Windows when convenient, then open OkuFlow again.", L"Kurulum tamamlandı. Uygun olduğunda Windows’u yeniden başlatın, ardından OkuFlow’u tekrar açın.", L"Die Installation ist abgeschlossen. Starten Sie Windows bei Gelegenheit neu und öffnen Sie OkuFlow erneut."));
        return false;
    }
    if (result == okuflow::WindowsRuntimeInstallResult::Failed || !ready) {
        Inform(Text(L"Installation did not complete", L"Kurulum tamamlanmadı", L"Installation wurde nicht abgeschlossen"), std::wstring(Text(L"Microsoft installer exit code: ", L"Microsoft yükleyicisi çıkış kodu: ", L"Microsoft-Installationsprogramm: Rückgabecode ")) + std::to_wstring(exitCode) + Text(L". You can run OkuFlow again to retry.", L". Yeniden denemek için OkuFlow’u tekrar açabilirsiniz.", L". Öffnen Sie OkuFlow erneut, um es noch einmal zu versuchen."));
        return false;
    }
    return true;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto directory = okuflow::WindowsExecutableDirectory();
    if (directory.empty() || !PrepareWindowsRuntime(directory)) return 1;
    const std::wstring application = directory + L"\\OkuFlowApp.exe";
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
        Inform(Text(L"OkuFlow could not open", L"OkuFlow açılamadı", L"OkuFlow konnte nicht geöffnet werden"), std::wstring(Text(L"Windows error: ", L"Windows hatası: ", L"Windows-Fehler: ")) + std::to_wstring(GetLastError()) +
               Text(L". Keep OkuFlowApp.exe and the other files from the ZIP beside oku_flow.exe.", L". OkuFlowApp.exe ve ZIP paketindeki diğer dosyaları oku_flow.exe ile aynı klasörde tutun.", L". Lassen Sie OkuFlowApp.exe und die anderen ZIP-Dateien neben oku_flow.exe."));
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
