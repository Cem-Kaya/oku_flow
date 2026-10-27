#include <windows.h>
#include <string>
#include <iostream>
#include <cstdlib>
#include "okuflow/app/process_exit.hpp"

namespace {
std::wstring exitReceipt;
void WriteExitReceipt(const char* value) {
    const auto file = CreateFileW(exitReceipt.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) std::abort();
    DWORD written{};
    const auto size = static_cast<DWORD>(std::char_traits<char>::length(value));
    if (!WriteFile(file, value, size, &written, nullptr) || written != size ||
        !FlushFileBuffers(file)) std::abort();
    CloseHandle(file);
}
void UnsafeGlobalCleanup() { WriteExitReceipt("unsafe global cleanup\n"); }
DWORD WINAPI PendingVendorCleanup(void* event) {
    SetEvent(static_cast<HANDLE>(event));
    Sleep(INFINITE); // process-owned work must not block application exit
    return 0;
}
}

std::string Utf8(const std::wstring& text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}
std::string Json(const std::wstring& text) {
    std::string result = "\"";
    for (const char value : Utf8(text)) {
        if (value == '\\' || value == '\"') result += '\\';
        if (value == '\n') result += "\\n";
        else if (value == '\r') result += "\\r";
        else if (value == '\t') result += "\\t";
        else result += value;
    }
    return result + "\"";
}
int wmain(int count, wchar_t** arguments) {
    if (count == 4 && std::wstring(arguments[1]) == L"--application-exit") {
        exitReceipt = arguments[2];
        std::atexit(UnsafeGlobalCleanup);
        const auto entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const auto thread = CreateThread(nullptr, 0, PendingVendorCleanup, entered, 0, nullptr);
        if (!entered || !thread || WaitForSingleObject(entered, 5000) != WAIT_OBJECT_0)
            return 97;
        CloseHandle(thread);
        CloseHandle(entered);
        {
            struct ApplicationState {
                ~ApplicationState() { WriteExitReceipt("application cleanup complete\n"); }
            } application;
        }
        okuflow::FinishWindowsApplication(_wtoi(arguments[3]));
    }
    wchar_t directory[32768]{}, environment[256]{};
    GetCurrentDirectoryW(32768, directory);
    GetEnvironmentVariableW(L"OKUFLOW_LAUNCHER_FIXTURE", environment, 256);
    std::cout << "{\"arguments\":[";
    for (int i = 1; i < count; ++i) std::cout << (i > 1 ? "," : "") << Json(arguments[i]);
    std::cout << "],\"directory\":" << Json(directory) << ",\"environment\":" << Json(environment) << "}";
    std::cerr << "fixture stderr";
    return 42;
}
