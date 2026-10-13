// Private fake adb server/sleeper for legacy adb recovery tests. Qt-free.
// Modes:
//   server --port <n> --ready-file <path> --kill-mode <ok|silent|ignore>
//          [--version-hang]
//       Bind 127.0.0.1:<n> (0 picks an ephemeral port), publish
//       "<pid> <port>" in the ready file, then serve raw host requests.
//       host:version answers protocol 41 unless --version-hang blocks it
//       forever; host:kill exits after an OKAY reply (ok), exits without a
//       reply (silent), or closes the connection and keeps serving
//       (ignore) so tests can reach the exact-handle termination stage.
//   sleep --ready-file <path>
//       Publish "<pid> 0" and sleep until terminated (sibling fixture).
// No real adb, no device, no window; only the loopback port and ready file
// the test passes in. Tests copy this binary as platform-tools/adb.exe,
// oku_flow.exe and OkuFlowApp.exe inside private temp distributions.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

bool WriteReadyFile(const std::wstring& path, unsigned short port)
{
    const std::wstring pending = path + L".pending";
    FILE* file = nullptr;
    if (_wfopen_s(&file, pending.c_str(), L"w") != 0 || !file) return false;
    std::fprintf(file, "%lu %u\n", static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned>(port));
    std::fclose(file);
    return MoveFileExW(pending.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

void SleepForever()
{
    for (;;) Sleep(50);
}

bool ReadExact(SOCKET socket, char* bytes, int count)
{
    for (int offset = 0; offset < count;) {
        const int received = recv(socket, bytes + offset, count - offset, 0);
        if (received <= 0) return false;
        offset += received;
    }
    return true;
}

bool SendAll(SOCKET socket, const char* bytes, int count)
{
    for (int offset = 0; offset < count;) {
        const int sent = send(socket, bytes + offset, count - offset, 0);
        if (sent <= 0) return false;
        offset += sent;
    }
    return true;
}

enum class KillMode { Ok, Silent, Ignore };

int RunServer(unsigned short requestedPort, const std::wstring& readyFile,
              KillMode killMode, bool versionHang)
{
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
    address.sin_port = htons(requestedPort);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 8) == SOCKET_ERROR) {
        closesocket(listener);
        WSACleanup();
        return 9;
    }
    sockaddr_in bound{};
    int boundLength = sizeof(bound);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0) {
        closesocket(listener);
        WSACleanup();
        return 9;
    }
    if (!WriteReadyFile(readyFile, ntohs(bound.sin_port))) {
        closesocket(listener);
        WSACleanup();
        return 3;
    }
    const auto shutdownServer = [&](int code) {
        closesocket(listener);
        WSACleanup();
        return code;
    };
    for (;;) {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        char lengthText[5]{};
        if (!ReadExact(client, lengthText, 4)) { closesocket(client); continue; }
        const unsigned long count = std::strtoul(lengthText, nullptr, 16);
        if (count == 0 || count > 4096) { closesocket(client); continue; }
        std::string request(count, '\0');
        if (!ReadExact(client, request.data(), static_cast<int>(count))) {
            closesocket(client);
            continue;
        }
        if (request == "host:kill") {
            if (killMode == KillMode::Ok) SendAll(client, "OKAY", 4);
            closesocket(client);
            if (killMode != KillMode::Ignore) return shutdownServer(0);
            continue;
        }
        if (request == "host:version") {
            if (versionHang) SleepForever();
            SendAll(client, "OKAY00040029", 12);
            closesocket(client);
            continue;
        }
        SendAll(client, "FAIL000bunsupported", 19);
        closesocket(client);
    }
    return shutdownServer(0);
}

} // namespace

int wmain(int count, wchar_t** arguments)
{
    if (count < 2) return 2;
    const std::wstring mode = arguments[1];
    std::wstring readyFile;
    std::wstring killModeText = L"ignore";
    unsigned short port = 0;
    bool versionHang = false;
    for (int i = 2; i < count; ++i) {
        const std::wstring argument = arguments[i];
        if (argument == L"--ready-file" && i + 1 < count) readyFile = arguments[++i];
        else if (argument == L"--port" && i + 1 < count)
            port = static_cast<unsigned short>(_wtoi(arguments[++i]));
        else if (argument == L"--kill-mode" && i + 1 < count) killModeText = arguments[++i];
        else if (argument == L"--version-hang") versionHang = true;
        else return 2;
    }
    if (readyFile.empty()) return 2;
    if (mode == L"sleep") {
        if (!WriteReadyFile(readyFile, 0)) return 3;
        SleepForever();
    }
    if (mode == L"server") {
        KillMode killMode = KillMode::Ignore;
        if (killModeText == L"ok") killMode = KillMode::Ok;
        else if (killModeText == L"silent") killMode = KillMode::Silent;
        else if (killModeText != L"ignore") return 2;
        return RunServer(port, readyFile, killMode, versionHang);
    }
    return 2;
}
