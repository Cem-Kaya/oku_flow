#ifdef _WIN32

#include "okuflow/app/windows_runtime_contract.hpp"
#include "okuflow/windows_runtime_pins.hpp"


#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <optional>

namespace okuflow {
namespace {

std::optional<WindowsRuntimeVersion> InstalledRuntimeVersion() {
    constexpr wchar_t keyPath[] = L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64";
    std::optional<WindowsRuntimeVersion> newest;
    for (const REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_READ | view, &key) != ERROR_SUCCESS) continue;
        const auto read = [key](const wchar_t* name, DWORD& value) {
            DWORD bytes = sizeof(value);
            return RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS;
        };
        DWORD installed = 0;
        DWORD major = 0, minor = 0, build = 0, revision = 0;
        if (read(L"Installed", installed) && installed == 1 && read(L"Major", major) &&
            read(L"Minor", minor) && read(L"Bld", build) && read(L"Rbld", revision)) {
            const WindowsRuntimeVersion version{major, minor, build, revision};
            if (!newest || version > *newest) newest = version;
        }
        RegCloseKey(key);
    }
    return newest;
}

bool RuntimeFilesReady(const std::wstring& directory) {
    for (const char* name : windows_runtime_pins::kFiles) {
        const std::string narrow(name);
        const auto path = directory + L"\\" + std::wstring(narrow.begin(), narrow.end());
        const DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
        if (!size || size > 1024 * 1024) return false;
        std::vector<unsigned char> bytes(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return false;
        VS_FIXEDFILEINFO* info = nullptr;
        UINT infoSize = 0;
        if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
            infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd) return false;
        if (!SupportsWindowsRuntimeVersion({HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                                           HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)})) return false;
    }
    return true;
}

} // namespace

bool SupportsWindowsRuntimeVersion(const WindowsRuntimeVersion& version) {
    return version[0] == windows_runtime_pins::kMinimumVersion[0] &&
           version >= windows_runtime_pins::kMinimumVersion;
}

bool IsSystemWindowsRuntimeReady() {
    const auto version = InstalledRuntimeVersion();
    if (version && !SupportsWindowsRuntimeVersion(*version)) return false;
    wchar_t directory[MAX_PATH]{};
    const UINT count = GetSystemDirectoryW(directory, MAX_PATH);
    return count && count < MAX_PATH && RuntimeFilesReady(directory);
}

std::wstring WindowsExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size);
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

bool VerifyWindowsRuntimeInstallerFile(const std::wstring& path) {
    struct Resources {
        HANDLE file{INVALID_HANDLE_VALUE};
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};
        std::vector<unsigned char> object;
        ~Resources() {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
    } resources;
    resources.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER size{};
    if (resources.file == INVALID_HANDLE_VALUE || !GetFileSizeEx(resources.file, &size) ||
        size.QuadPart <= 0 || size.QuadPart > 128 * 1024 * 1024 ||
        BCryptOpenAlgorithmProvider(&resources.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    DWORD length = 0, received = 0;
    if (BCryptGetProperty(resources.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length),
                          sizeof(length), &received, 0) < 0 || !length || length > 1024 * 1024) return false;
    resources.object.resize(length);
    if (BCryptCreateHash(resources.algorithm, &resources.hash, resources.object.data(), length, nullptr, 0, 0) < 0) return false;
    std::array<unsigned char, 65536> buffer{};
    DWORD read = 0;
    for (;;) {
        if (!ReadFile(resources.file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return false;
        if (!read) break;
        if (BCryptHashData(resources.hash, buffer.data(), read, 0) < 0) return false;
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(resources.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    for (const unsigned char byte : digest) { actual += hex[byte >> 4]; actual += hex[byte & 15]; }
    return actual == windows_runtime_pins::kInstallerSha256;
}

std::wstring QuoteWindowsArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        quoted.append(character == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        quoted += character;
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L"\"";
}

WindowsRuntimeInstallResult ClassifyWindowsRuntimeInstallResult(std::uint32_t exitCode, bool installed) {
    if (exitCode == ERROR_SUCCESS_REBOOT_REQUIRED || exitCode == ERROR_SUCCESS_REBOOT_INITIATED)
        return WindowsRuntimeInstallResult::RestartRequired;
    if (exitCode == ERROR_CANCELLED || exitCode == 0x800704c7u ||
        exitCode == ERROR_INSTALL_USEREXIT || exitCode == 0x80070642u)
        return WindowsRuntimeInstallResult::Canceled;
    if (installed && (exitCode == 0 || exitCode == ERROR_PRODUCT_VERSION || exitCode == 0x80070666u))
        return WindowsRuntimeInstallResult::Success;
    return WindowsRuntimeInstallResult::Failed;
}

} // namespace okuflow
#endif
