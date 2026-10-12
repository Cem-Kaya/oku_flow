#include <windows.h>
#include <string>
#include <iostream>

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
    wchar_t directory[32768]{}, environment[256]{};
    GetCurrentDirectoryW(32768, directory);
    GetEnvironmentVariableW(L"OKUFLOW_LAUNCHER_FIXTURE", environment, 256);
    std::cout << "{\"arguments\":[";
    for (int i = 1; i < count; ++i) std::cout << (i > 1 ? "," : "") << Json(arguments[i]);
    std::cout << "],\"directory\":" << Json(directory) << ",\"environment\":" << Json(environment) << "}";
    std::cerr << "fixture stderr";
    return 42;
}
