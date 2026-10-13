// Private owned-child fixture for adb lifecycle tests. Modes:
//   sleep <readyFile>                 write own PID, then sleep until killed
//   exit-after <ms> <readyFile>       write own PID, sleep, exit 0
//   parent <exit|hang> <childReady> <parentReady> <releaseFile>
//       launch `sleep <childReady>` through OwnedChildProcess, write
//       <parentReady>, then return normally (exit) or sleep (hang) so the
//       test can terminate this parent and observe job-close cleanup.
// No real adb or window. Optional fake host mode binds only a private test port.
#include "okuflow/capture/adb_owned_server.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
bool WritePid(const wchar_t* path)
{
    const auto pending = std::wstring(path) + L".pending";
    FILE* file = nullptr;
    if (_wfopen_s(&file, pending.c_str(), L"w") != 0 || !file) return false;
    std::fprintf(file, "%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    std::fclose(file);
    return MoveFileExW(pending.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}
bool FileExists(const wchar_t* path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
void SleepForever()
{
    for (;;) Sleep(50);
}
bool ReadSocket(SOCKET socket, char* bytes, int count)
{
    for (int offset = 0; offset < count;) {
        const int received = recv(socket, bytes + offset, count - offset, 0);
        if (received <= 0) return false;
        offset += received;
    }
    return true;
}
void Reply(SOCKET socket, const std::string& body)
{
    char length[5]{};
    sprintf_s(length, "%04x", static_cast<unsigned>(body.size()));
    const std::string bytes = std::string("OKAY") + length + body;
    for (size_t offset = 0; offset < bytes.size();) {
        const int sent = send(socket, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
        if (sent <= 0) return;
        offset += sent;
    }
}
int FakeAdbServer(unsigned short port, bool hang, bool exitAfterList, bool device)
{
    wchar_t emulatorSetting[8]{};
    if (!GetEnvironmentVariableW(L"ADB_EMU", emulatorSetting, 8) ||
        std::wstring(emulatorSetting) != L"0") return 7;
    wchar_t rejectKillSetting[8]{};
    if (!GetEnvironmentVariableW(L"ADB_REJECT_KILL_SERVER", rejectKillSetting, 8) ||
        std::wstring(rejectKillSetting) != L"1") return 7;
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 8;
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return 8;
    const BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 8) == SOCKET_ERROR) { closesocket(listener); WSACleanup(); return 9; }
    for (;;) {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        const DWORD readTimeout = 1000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&readTimeout), sizeof(readTimeout));
        char length[5]{};
        if (!ReadSocket(client, length, 4)) { closesocket(client); continue; }
        const auto count = std::strtoul(length, nullptr, 16);
        if (count > 65535) { closesocket(client); continue; }
        std::string request(count, '\0');
        if (!ReadSocket(client, request.data(), static_cast<int>(count))) { closesocket(client); continue; }
        if (hang) SleepForever();
        const bool listing = request == "host:track-devices-proto-binary" || request == "host:devices-l";
        if (request == "host:version") Reply(client, "0029");
        else if (listing) {
            const char protobuf[]{10, 11, 10, 3, 'a', 'b', 'c', 16, 8, 56, 1, 80, 9};
            Reply(client, device ? std::string(protobuf, sizeof(protobuf)) : std::string{});
        }
        else send(client, "FAIL000bunsupported", 19, 0);
        closesocket(client);
        if (listing && exitAfterList) break;
    }
    closesocket(listener);
    WSACleanup();
    return 0;
}
} // namespace

int wmain(int count, wchar_t** arguments)
{
    // Private production-wiring mode understands the same bind arguments as
    // adb, but serves only harmless version/empty-list host queries.
    unsigned short port = 0;
    bool hang = false, exitAfterList = false, device = false;
    for (int i = 1; i < count; ++i) {
        const std::wstring argument = arguments[i];
        if (argument == L"-L" && i + 1 < count) {
            const std::wstring socketSpec = arguments[++i];
            if (socketSpec.starts_with(L"tcp:")) port = static_cast<unsigned short>(_wtoi(socketSpec.c_str() + 4));
        } else if (argument == L"--fixture-hang") hang = true;
        else if (argument == L"--fixture-exit-after-list") exitAfterList = true;
        else if (argument == L"--fixture-device") device = true;
    }
    if (port) return FakeAdbServer(port, hang, exitAfterList, device);
    if (count < 3) return 2;
    const std::wstring mode = arguments[1];
    if (mode == L"sleep") {
        if (!WritePid(arguments[2])) return 3;
        SleepForever();
    }
    if (mode == L"exit-after" && count >= 4) {
        if (!WritePid(arguments[3])) return 3;
        Sleep(static_cast<DWORD>(_wtoi(arguments[2])));
        return 0;
    }
    if (mode == L"parent" && count >= 6) {
        wchar_t self[32768]{};
        if (!GetModuleFileNameW(nullptr, self, 32768)) return 3;
        auto child = okuflow::OwnedChildProcess::Launch(
            {self, {L"sleep", arguments[3]}, {}, {}});
        if (!child) return 4;
        for (int waited = 0; waited < 100 && !FileExists(arguments[3]); ++waited) Sleep(20);
        if (!FileExists(arguments[3])) return 5;
        if (!WritePid(arguments[4])) return 3;
        if (std::wstring(arguments[2]) == L"hang") SleepForever();
        // Exit mode: wait for the test to observe the live child before
        // returning, so there is no race with this process exiting.
        for (int waited = 0; waited < 500 && !FileExists(arguments[5]); ++waited) Sleep(20);
        return 0; // Normal exit: the OwnedChildProcess destructor runs.
    }
    return 2;
}
