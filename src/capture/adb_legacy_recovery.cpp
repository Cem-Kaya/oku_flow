#ifdef _WIN32
#include "okuflow/capture/adb_legacy_recovery.hpp"

#include <iphlpapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <ws2tcpip.h>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace okuflow {
namespace {

using Clock = std::chrono::steady_clock;
using Status = LegacyAdbRecoveryStatus;
using Detail = LegacyAdbRecoveryDetail;

struct HandleGuard {
    HANDLE handle{nullptr};
    ~HandleGuard() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    HANDLE Release() { HANDLE value = handle; handle = nullptr; return value; }
};

struct MutexGuard {
    HANDLE handle{nullptr};
    bool acquired{false};
    ~MutexGuard()
    {
        if (handle && acquired) ReleaseMutex(handle);
        if (handle) CloseHandle(handle);
    }
};

struct SocketGuard {
    SOCKET socket{INVALID_SOCKET};
    ~SocketGuard() { if (socket != INVALID_SOCKET) closesocket(socket); }
};

bool Canceled(const LegacyAdbRecoveryRequest& request)
{
    return (request.canceled && request.canceled->load()) ||
           (request.shutdownRequested && request.shutdownRequested->load());
}

std::chrono::milliseconds ClampMs(std::chrono::milliseconds value,
                                  std::chrono::milliseconds low,
                                  std::chrono::milliseconds high)
{
    return std::min(std::max(value, low), high);
}

std::string ErrorText(const char* operation)
{
    return std::string(operation) + " (error " + std::to_string(GetLastError()) + ")";
}

LegacyAdbRecoveryResult MakeResult(Status status, Detail detail, std::string diagnostic)
{
    LegacyAdbRecoveryResult result;
    result.status = status;
    result.detail = detail;
    result.diagnostic = std::move(diagnostic);
    return result;
}

bool EqualNoCase(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::wstring DirName(const std::wstring& path)
{
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
}

std::wstring BaseName(const std::wstring& path)
{
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& name)
{
    if (directory.empty()) return name;
    if (directory.back() == L'\\' || directory.back() == L'/') return directory + name;
    return directory + L"\\" + name;
}

bool IsFile(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// True when canonical `path` equals canonical `root` or lives under it.
// Both sides must already be canonicalized by the caller.
bool PathEqualOrUnder(const std::wstring& path, const std::wstring& root)
{
    if (EqualNoCase(path, root)) return true;
    const std::wstring prefix = root +
        ((root.back() == L'\\' || root.back() == L'/') ? std::wstring{} : std::wstring{L"\\"});
    return path.size() > prefix.size() &&
           EqualNoCase(path.substr(0, prefix.size()), prefix);
}

bool IsHex64(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    });
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// SHA-256 of one bounded regular file, lowercase hex. Hashing is part of
// provenance, so any open/read/size problem is a refusal, never a guess.
bool Sha256File(const std::wstring& path, qint64 maxBytes, std::string* hex)
{
    QFile file(QString::fromStdWString(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    if (file.size() <= 0 || file.size() > maxBytes) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return false;
    *hex = QString::fromLatin1(hash.result().toHex()).toStdString();
    return true;
}

void StripExtendedPrefix(std::wstring* path)
{
    if (path->rfind(L"\\\\?\\UNC\\", 0) == 0) *path = L"\\\\" + path->substr(8);
    else if (path->rfind(L"\\\\?\\", 0) == 0) *path = path->substr(4);
}

// Resolves symlinks/junctions and 8.3 aliases through an opened handle so
// comparisons and hashing target the same final file the kernel reports.
bool CanonicalizeOpened(HANDLE handle, std::wstring* canonical)
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length || length >= buffer.size()) return false;
    buffer.resize(length);
    StripExtendedPrefix(&buffer);
    *canonical = std::move(buffer);
    return true;
}

bool CanonicalFilePath(const std::wstring& path, std::wstring* canonical)
{
    HandleGuard file{CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    return file.handle != INVALID_HANDLE_VALUE && CanonicalizeOpened(file.handle, canonical);
}

bool CanonicalDirectoryPath(const std::wstring& path, std::wstring* canonical)
{
    HandleGuard directory{CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    return directory.handle != INVALID_HANDLE_VALUE &&
           CanonicalizeOpened(directory.handle, canonical);
}

bool ProcessAlive(HANDLE process)
{
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

struct ProcessIdentity {
    DWORD pid{0};
    FILETIME creation{};
    std::wstring canonicalImagePath;
};

// Identity always comes from the held handle: PID reuse cannot change a
// handle's creation time or image, so any drift here is a changed target.
bool CaptureIdentity(HANDLE process, ProcessIdentity* identity)
{
    ProcessIdentity captured;
    captured.pid = GetProcessId(process);
    if (!captured.pid) return false;
    FILETIME exitTime{}, kernelTime{}, userTime{};
    if (!GetProcessTimes(process, &captured.creation, &exitTime, &kernelTime, &userTime))
        return false;
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &length) || !length) return false;
    image.resize(length);
    if (!CanonicalFilePath(image, &captured.canonicalImagePath)) return false;
    if (!ProcessAlive(process)) return false;
    *identity = std::move(captured);
    return true;
}

bool SameIdentity(const ProcessIdentity& left, const ProcessIdentity& right)
{
    return left.pid == right.pid &&
           left.creation.dwLowDateTime == right.creation.dwLowDateTime &&
           left.creation.dwHighDateTime == right.creation.dwHighDateTime &&
           EqualNoCase(left.canonicalImagePath, right.canonicalImagePath);
}

enum class ListenerLookup { Found, None, Ambiguous, Error };

// Observation only: maps the exact 127.0.0.1 listener for the port to one
// PID. The PID is used solely to open the exact handle revalidated later.
ListenerLookup FindLoopbackListenerPid(std::uint16_t port, DWORD* pid)
{
    ULONG bytes = 0;
    if (GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) !=
        ERROR_INSUFFICIENT_BUFFER)
        return ListenerLookup::Error;
    std::vector<std::uint8_t> storage(bytes);
    if (GetExtendedTcpTable(storage.data(), &bytes, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return ListenerLookup::Error;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
    std::set<DWORD> owners;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) == port &&
            ntohl(row.dwLocalAddr) == INADDR_LOOPBACK)
            owners.insert(row.dwOwningPid);
    }
    if (owners.empty()) return ListenerLookup::None;
    if (owners.size() > 1) return ListenerLookup::Ambiguous;
    *pid = *owners.begin();
    return ListenerLookup::Found;
}

// StringFileInfo ProductName through the file's own translation table,
// with the common Unicode code pages as fallback. No VERSIONINFO, no name.
bool ReadProductName(const std::wstring& path, std::wstring* productName)
{
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (!size || size > 4 * 1024 * 1024) return false;
    std::vector<unsigned char> bytes(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return false;
    struct Translation { WORD language; WORD codePage; };
    std::vector<Translation> translations;
    Translation* listed = nullptr;
    UINT listedBytes = 0;
    if (VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<void**>(&listed), &listedBytes) &&
        listedBytes >= sizeof(Translation)) {
        translations.assign(listed, listed + listedBytes / sizeof(Translation));
    }
    translations.push_back({0x0409, 0x04b0});
    translations.push_back({0x0409, 0x04e4});
    translations.push_back({0x0000, 0x04b0});
    for (const Translation translation : translations) {
        wchar_t query[64]{};
        swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\ProductName",
                   translation.language, translation.codePage);
        wchar_t* value = nullptr;
        UINT valueLength = 0;
        if (VerQueryValueW(bytes.data(), query, reinterpret_cast<void**>(&value), &valueLength) &&
            value && valueLength) {
            *productName = std::wstring(value, valueLength);
            while (!productName->empty() && productName->back() == L'\0') productName->pop_back();
            return !productName->empty();
        }
    }
    return false;
}

struct Provenance {
    std::wstring payloadRoot;
    std::wstring launcherPath;
    bool nested{false};
};

// Full distribution proof for one canonical image path. Every refusal names
// its detail; the manifest pin binds the reviewed build record, the entry
// hash binds this exact adb.exe file, and the launcher ProductName (or
// allowlisted launcher hash) binds the parent bundle, so an SDK copy or
// a matching adb hash alone never classifies as managed.
bool VerifyProvenance(const std::wstring& canonicalImage,
                      const LegacyAdbProvenanceExpectations& expectations,
                      Provenance* provenance, Detail* failDetail, std::string* diagnostic)
{
    const auto refuse = [&](Detail detail, std::string message) {
        *failDetail = detail;
        *diagnostic = std::move(message);
        return false;
    };
    const std::wstring platformTools = DirName(canonicalImage);
    const std::wstring payloadRoot = DirName(platformTools);
    if (!EqualNoCase(BaseName(canonicalImage), L"adb.exe") ||
        !EqualNoCase(BaseName(platformTools), L"platform-tools") || payloadRoot.empty())
        return refuse(Detail::ImageNotBundledAdb,
                      "Listener image is not a platform-tools adb.exe");

    Provenance proven;
    proven.payloadRoot = payloadRoot;
    const std::wstring flatLauncher = JoinPath(payloadRoot, L"oku_flow.exe");
    const std::wstring appExecutable = JoinPath(payloadRoot, L"OkuFlowApp.exe");
    if (IsFile(flatLauncher) && IsFile(appExecutable)) {
        proven.launcherPath = flatLauncher;
        proven.nested = false;
    } else if (EqualNoCase(BaseName(payloadRoot), L"app") && IsFile(appExecutable) &&
               IsFile(JoinPath(DirName(payloadRoot), L"oku_flow.exe"))) {
        proven.launcherPath = JoinPath(DirName(payloadRoot), L"oku_flow.exe");
        proven.nested = true;
    } else {
        return refuse(Detail::DistributionLayoutUnrecognized,
                      "No flat or nested OkuFlow bundle layout around the adb image");
    }
    std::wstring canonicalLauncher;
    if (!CanonicalFilePath(proven.launcherPath, &canonicalLauncher))
        return refuse(Detail::DistributionLayoutUnrecognized,
                      "Bundle launcher path cannot be canonicalized");
    proven.launcherPath = canonicalLauncher;

    if (!expectations.requiredDistributionRoot.empty()) {
        std::wstring canonicalPayload, canonicalRequired;
        if (!CanonicalDirectoryPath(payloadRoot, &canonicalPayload) ||
            !CanonicalDirectoryPath(expectations.requiredDistributionRoot, &canonicalRequired))
            return refuse(Detail::DistributionOutsideExpectedRoot,
                          "Distribution root cannot be canonicalized");
        const bool underRequired = PathEqualOrUnder(canonicalPayload, canonicalRequired);
        if (!underRequired)
            return refuse(Detail::DistributionOutsideExpectedRoot,
                          "Distribution is outside the expected root");
    }

    for (const wchar_t* name :
         {L"AdbWinApi.dll", L"AdbWinUsbApi.dll", L"NOTICE.txt", L"source.properties"}) {
        if (!IsFile(JoinPath(platformTools, name)))
            return refuse(Detail::DistributionIncomplete,
                          "Bundled adb distribution is missing reviewed runtime files");
    }

    const std::wstring manifestPath = JoinPath(platformTools, L"adb-build.json");
    std::string manifestHash;
    if (!Sha256File(manifestPath, 16 * 1024 * 1024, &manifestHash) ||
        !EqualNoCase(std::wstring(manifestHash.begin(), manifestHash.end()),
                     std::wstring(expectations.reviewedManifestSha256.begin(),
                                  expectations.reviewedManifestSha256.end())))
        return refuse(Detail::ManifestHashMismatch,
                      "adb-build.json does not match the reviewed manifest pin");

    QFile manifestFile(QString::fromStdWString(manifestPath));
    QJsonParseError parseError;
    const QJsonDocument manifest = QJsonDocument::fromJson(
        manifestFile.open(QIODevice::ReadOnly) ? manifestFile.readAll() : QByteArray{},
        &parseError);
    if (parseError.error != QJsonParseError::NoError || !manifest.isObject())
        return refuse(Detail::ManifestInvalid, "adb-build.json is not a JSON object");
    const QJsonObject root = manifest.object();
    if (!root.value(QStringLiteral("sourceBuilt")).toBool() ||
        root.value(QStringLiteral("protocol")).toInt() != 41)
        return refuse(Detail::ManifestInvalid,
                      "adb-build.json is not the reviewed source build at protocol 41");
    std::string entryHash;
    int adbEntries = 0;
    for (const QJsonValue value : root.value(QStringLiteral("files")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("path")).toString() ==
            QStringLiteral("platform-tools/adb.exe")) {
            ++adbEntries;
            entryHash = LowerAscii(entry.value(QStringLiteral("sha256")).toString().toStdString());
        }
    }
    if (adbEntries != 1 || !IsHex64(entryHash))
        return refuse(Detail::ManifestInvalid,
                      "adb-build.json lacks exactly one hashed platform-tools/adb.exe entry");
    std::string imageHash;
    if (!Sha256File(canonicalImage, 256 * 1024 * 1024, &imageHash) || imageHash != entryHash)
        return refuse(Detail::AdbExecutableHashMismatch,
                      "Running adb image file does not match the reviewed manifest entry");

    // The launcher binds the parent bundle by exact ProductName, or —
    // for reviewed legacy launchers shipped without VERSIONINFO — by an
    // exact SHA-256 on the pinned allowlist. Neither proof is path-only.
    std::wstring productName;
    const bool productAccepted =
        !expectations.expectedLauncherProductName.empty() &&
        ReadProductName(proven.launcherPath, &productName) &&
        productName == expectations.expectedLauncherProductName;
    bool hashAccepted = false;
    if (!productAccepted && !expectations.reviewedLauncherSha256.empty()) {
        std::string launcherHash;
        if (Sha256File(proven.launcherPath, 256 * 1024 * 1024, &launcherHash)) {
            hashAccepted = std::any_of(
                expectations.reviewedLauncherSha256.begin(),
                expectations.reviewedLauncherSha256.end(),
                [&](const std::string& allowed) { return LowerAscii(allowed) == launcherHash; });
        }
    }
    if (!productAccepted && !hashAccepted)
        return refuse(Detail::LauncherProductMismatch,
                      "Bundle launcher matches neither the expected VERSIONINFO "
                      "ProductName nor an allowlisted reviewed launcher hash");

    *provenance = std::move(proven);
    return true;
}

bool TokenUserSid(HANDLE token, std::vector<unsigned char>* sidBytes)
{
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    const DWORD sidLength = GetLengthSid(user->User.Sid);
    const auto* begin = reinterpret_cast<const unsigned char*>(user->User.Sid);
    sidBytes->assign(begin, begin + sidLength);
    return true;
}

bool CurrentUserSid(std::vector<unsigned char>* sidBytes, std::wstring* sidString)
{
    HandleGuard token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.handle)) return false;
    if (!TokenUserSid(token.handle, sidBytes)) return false;
    if (sidString) {
        wchar_t* text = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<PSID>(sidBytes->data()), &text))
            return false;
        *sidString = text;
        LocalFree(text);
    }
    return true;
}

bool ProcessCanonicalImage(HANDLE process, std::wstring* canonical)
{
    std::wstring image(32768, L'\0');
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &length) || !length) return false;
    image.resize(length);
    return CanonicalFilePath(image, canonical);
}

// True while another same-user/same-session OkuFlow app instance is alive.
// The current instance's own launcher is an ancestor waiting on this
// process, never a sibling user of the shared server. Anything named like
// a sibling that cannot be fully verified counts as present (fail closed).
// A non-empty scanRootForTest (private fixture seam) considers only
// candidates proven to live under that directory; production passes empty
// for the broad scan. An unprovable candidate image still fails closed.
bool SiblingOkuFlowRunning(const std::wstring& scanRootForTest)
{
    std::wstring canonicalScanRoot;
    if (!scanRootForTest.empty() &&
        !CanonicalDirectoryPath(scanRootForTest, &canonicalScanRoot))
        return true;
    HandleGuard snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.handle == INVALID_HANDLE_VALUE) return true;
    std::map<DWORD, DWORD> parents;
    std::map<DWORD, std::wstring> names;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.handle, &entry)) return true;
    do {
        parents[entry.th32ProcessID] = entry.th32ParentProcessID;
        names[entry.th32ProcessID] = entry.szExeFile;
    } while (Process32NextW(snapshot.handle, &entry));

    const DWORD currentPid = GetCurrentProcessId();
    std::set<DWORD> ancestors;
    for (DWORD at = currentPid, steps = 0; steps < 16; ++steps) {
        const auto found = parents.find(at);
        if (found == parents.end() || !found->second || found->second == at) break;
        ancestors.insert(found->second);
        at = found->second;
    }
    DWORD currentSession = 0;
    std::vector<unsigned char> currentSid;
    if (!ProcessIdToSessionId(currentPid, &currentSession) ||
        !CurrentUserSid(&currentSid, nullptr))
        return true;

    for (const auto& [pid, name] : names) {
        if (pid == currentPid) continue;
        const bool isApp = EqualNoCase(name, L"OkuFlowApp.exe");
        const bool isLauncher = EqualNoCase(name, L"oku_flow.exe");
        if (!isApp && !isLauncher) continue;
        if (isLauncher && ancestors.contains(pid)) continue;
        HandleGuard process;
        if (!canonicalScanRoot.empty()) {
            process.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process.handle) return true;
            std::wstring candidateImage;
            if (!ProcessCanonicalImage(process.handle, &candidateImage)) return true;
            if (!PathEqualOrUnder(candidateImage, canonicalScanRoot)) continue;
        }
        DWORD session = 0;
        if (!ProcessIdToSessionId(pid, &session)) return true;
        if (session != currentSession) continue;
        if (!process.handle) {
            process.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process.handle) return true;
        }
        // A same-named process whose real image is elsewhere could be a
        // renamed sibling copy; only the token evidence below can clear it.
        HandleGuard token;
        if (!OpenProcessToken(process.handle, TOKEN_QUERY, &token.handle)) return true;
        std::vector<unsigned char> candidateSid;
        if (!TokenUserSid(token.handle, &candidateSid)) return true;
        if (candidateSid == currentSid) return true;
    }
    return false;
}

std::wstring RecoveryMutexName(const LegacyAdbRecoveryRequest& request)
{
    if (!request.policy.mutexNameOverride.empty()) return request.policy.mutexNameOverride;
    std::vector<unsigned char> sid;
    std::wstring sidText;
    DWORD session = 0;
    if (!CurrentUserSid(&sid, &sidText) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &session))
        return {};
    return L"Local\\OkuFlow.LegacyAdbRecovery." + sidText + L"." + std::to_wstring(session);
}

enum class ExitWait { Exited, TimedOut, Canceled };

// Bounded wait on the held handle in short slices so cancellation stays
// responsive without another thread or an alertable wait.
ExitWait WaitForExit(HANDLE process, std::chrono::milliseconds budget,
                     const LegacyAdbRecoveryRequest& request, bool honorCancel)
{
    const auto deadline = Clock::now() + budget;
    for (;;) {
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return ExitWait::Exited;
        if (honorCancel && Canceled(request)) return ExitWait::Canceled;
        if (Clock::now() >= deadline) return ExitWait::TimedOut;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct Target;
std::optional<LegacyAdbRecoveryResult> Revalidate(const Target& target,
                                                  const LegacyAdbRecoveryRequest& request);

enum class KillDelivery { Delivered, Failed, Canceled, Aborted };

// One raw host:kill against the classified loopback endpoint only. No adb
// client process is ever run, so no version check can replace the server.
// After the TCP connect succeeds and before any byte is sent, the exact
// held target is fully revalidated (identity, listener, provenance,
// cancellation, sessions, sibling); a replacement listener therefore
// receives no bytes. On Aborted, *abort carries that revalidation result.
KillDelivery SendHostKill(const Target& target, const LegacyAdbRecoveryRequest& request,
                          const LegacyAdbRecoveryPolicy& policy,
                          std::optional<LegacyAdbRecoveryResult>* abort)
{
    const std::uint16_t port = request.port;
    const std::chrono::milliseconds connectTimeout = policy.hostKillConnectTimeout;
    static std::once_flag winsockOnce;
    static int winsockResult = -1;
    std::call_once(winsockOnce, [] {
        WSADATA data{};
        winsockResult = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (winsockResult != 0) return KillDelivery::Failed;
    SocketGuard guard{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (guard.socket == INVALID_SOCKET) return KillDelivery::Failed;
    u_long nonBlocking = 1;
    if (ioctlsocket(guard.socket, FIONBIO, &nonBlocking) != 0) return KillDelivery::Failed;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const int linked = connect(guard.socket, reinterpret_cast<sockaddr*>(&address),
                               sizeof(address));
    if (linked != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) return KillDelivery::Failed;
        const auto deadline = Clock::now() + connectTimeout;
        for (;;) {
            if (Canceled(request)) return KillDelivery::Canceled;
            fd_set writable{};
            FD_SET(guard.socket, &writable);
            timeval slice{0, 20 * 1000};
            const int ready = select(0, nullptr, &writable, nullptr, &slice);
            if (ready > 0) {
                int socketError = 0;
                int socketErrorBytes = sizeof(socketError);
                if (getsockopt(guard.socket, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&socketError), &socketErrorBytes) != 0 ||
                    socketError != 0)
                    return KillDelivery::Failed;
                break;
            }
            if (ready < 0 || Clock::now() >= deadline) return KillDelivery::Failed;
        }
    }
    // Connect succeeded; nothing has been sent yet. The test seam runs
    // first so fixtures can replace the listener in exactly this window,
    // then the full revalidation decides whether any byte may follow.
    if (policy.afterHostKillConnectForTest) policy.afterHostKillConnectForTest();
    if (auto revalidationAbort = Revalidate(target, request)) {
        *abort = *revalidationAbort;
        return KillDelivery::Aborted;
    }
    if (Canceled(request)) return KillDelivery::Canceled;
    constexpr char requestBytes[] = "0009host:kill";
    constexpr int requestLength = static_cast<int>(sizeof(requestBytes) - 1);
    const auto sendDeadline = Clock::now() + connectTimeout;
    for (int sentTotal = 0; sentTotal < requestLength;) {
        if (Canceled(request)) return KillDelivery::Canceled;
        fd_set writable{};
        FD_SET(guard.socket, &writable);
        timeval slice{0, 20 * 1000};
        if (select(0, nullptr, &writable, nullptr, &slice) <= 0) {
            if (Clock::now() >= sendDeadline) return KillDelivery::Failed;
            continue;
        }
        const int sent = send(guard.socket, requestBytes + sentTotal,
                              requestLength - sentTotal, 0);
        if (sent <= 0) return KillDelivery::Failed;
        sentTotal += sent;
    }
    shutdown(guard.socket, SD_SEND);
    return KillDelivery::Delivered;
}

struct Target {
    HandleGuard process;
    ProcessIdentity identity;
    Provenance provenance;
};

// Revalidation before each stop stage. A nullopt return means every check
// still holds for the exact held handle; anything else aborts the attempt
// with that result and stops nothing further.
std::optional<LegacyAdbRecoveryResult> Revalidate(const Target& target,
                                                  const LegacyAdbRecoveryRequest& request)
{
    LegacyAdbRecoveryResult abort;
    abort.listenerProcessId = target.identity.pid;
    abort.imagePath = target.identity.canonicalImagePath;
    if (Canceled(request)) {
        abort.status = Status::Canceled;
        abort.detail = Detail::None;
        abort.diagnostic = "Legacy adb recovery canceled before a stop stage";
        return abort;
    }
    if (request.hasActiveOrStartingPhoneSessions &&
        request.hasActiveOrStartingPhoneSessions()) {
        abort.status = Status::Busy;
        abort.detail = Detail::SessionsBusy;
        abort.diagnostic = "A phone session became active or starting";
        return abort;
    }
    if (!ProcessAlive(target.process.handle)) {
        abort.status = Status::IdentityChanged;
        abort.detail = Detail::ProcessChangedOrExited;
        abort.diagnostic = "Classified legacy adb process exited before a stop stage";
        return abort;
    }
    ProcessIdentity fresh;
    if (!CaptureIdentity(target.process.handle, &fresh) || !SameIdentity(fresh, target.identity)) {
        abort.status = Status::IdentityChanged;
        abort.detail = Detail::ProcessChangedOrExited;
        abort.diagnostic = "Classified legacy adb identity changed before a stop stage";
        return abort;
    }
    DWORD listenerPid = 0;
    switch (FindLoopbackListenerPid(request.port, &listenerPid)) {
    case ListenerLookup::Found:
        if (listenerPid != target.identity.pid) {
            abort.status = Status::IdentityChanged;
            abort.detail = Detail::ListenerChanged;
            abort.diagnostic = "Loopback listener changed owner before a stop stage";
            return abort;
        }
        break;
    case ListenerLookup::Error:
        abort.status = Status::Failed;
        abort.detail = Detail::ListenerQueryFailed;
        abort.diagnostic = ErrorText("Query loopback listener table");
        return abort;
    default:
        abort.status = Status::IdentityChanged;
        abort.detail = Detail::ListenerChanged;
        abort.diagnostic = "Loopback listener disappeared or became ambiguous";
        return abort;
    }
    if (SiblingOkuFlowRunning(request.policy.siblingScanRootForTest)) {
        abort.status = Status::Busy;
        abort.detail = Detail::SiblingOkuFlowRunning;
        abort.diagnostic = "Another OkuFlow instance is running";
        return abort;
    }
    Provenance freshProvenance;
    Detail failDetail = Detail::InternalError;
    std::string provenanceDiagnostic;
    if (!VerifyProvenance(fresh.canonicalImagePath, request.expectations, &freshProvenance,
                          &failDetail, &provenanceDiagnostic)) {
        abort.status = Status::IdentityChanged;
        abort.detail = Detail::ProvenanceChanged;
        abort.diagnostic = "Provenance changed before a stop stage: " + provenanceDiagnostic;
        return abort;
    }
    if (Canceled(request)) {
        abort.status = Status::Canceled;
        abort.detail = Detail::None;
        abort.diagnostic = "Legacy adb recovery canceled before a stop stage";
        return abort;
    }
    return std::nullopt;
}

// After the exact process exits, give the TCP table a short bounded moment
// to drop its row. Another listener claiming the port afterwards is noted
// but cannot un-recover the server that is already gone.
void NotePortAfterExit(std::uint16_t port, DWORD oldPid, std::string* diagnostic)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(250);
    for (;;) {
        DWORD listenerPid = 0;
        const ListenerLookup lookup = FindLoopbackListenerPid(port, &listenerPid);
        if (lookup == ListenerLookup::None) return;
        if (lookup == ListenerLookup::Found && listenerPid != oldPid) {
            *diagnostic += "; the port has since been claimed by another listener";
            return;
        }
        if (Clock::now() >= deadline) {
            *diagnostic += "; the old listener row had not cleared yet";
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

LegacyAdbRecoveryResult TryReclaimLegacyBundledAdbServer(
    const LegacyAdbRecoveryRequest& request)
{
    if (Canceled(request))
        return MakeResult(Status::Canceled, Detail::None,
                          "Legacy adb recovery canceled before classification");
    if (!request.port)
        return MakeResult(Status::Failed, Detail::InternalError,
                          "Legacy adb recovery requires a nonzero port");
    if (!IsHex64(request.expectations.reviewedManifestSha256) ||
        (request.expectations.expectedLauncherProductName.empty() &&
         request.expectations.reviewedLauncherSha256.empty()) ||
        !std::all_of(request.expectations.reviewedLauncherSha256.begin(),
                     request.expectations.reviewedLauncherSha256.end(),
                     [](const std::string& hash) { return IsHex64(hash); }))
        return MakeResult(Status::Failed, Detail::ManifestInvalid,
                          "Legacy adb provenance expectations are invalid");
    if (!request.externallyConfirmedUnresponsive || !request.phoneUseIntent ||
        !request.noActiveOrStartingPhoneSessions)
        return MakeResult(Status::Busy, Detail::PreconditionsNotConfirmed,
                          "Caller preconditions are not all confirmed (unresponsive "
                          "confirmation, phone intent, idle sessions)");
    if (request.hasActiveOrStartingPhoneSessions &&
        request.hasActiveOrStartingPhoneSessions())
        return MakeResult(Status::Busy, Detail::SessionsBusy,
                          "A phone session is active or starting");

    LegacyAdbRecoveryPolicy policy = request.policy;
    policy.mutexAcquireTimeout =
        ClampMs(policy.mutexAcquireTimeout, std::chrono::milliseconds(0),
                std::chrono::milliseconds(10000));
    policy.hostKillConnectTimeout =
        ClampMs(policy.hostKillConnectTimeout, std::chrono::milliseconds(50),
                std::chrono::milliseconds(2000));
    policy.hostKillExitWait =
        ClampMs(policy.hostKillExitWait, std::chrono::milliseconds(0),
                std::chrono::milliseconds(2000));
    policy.forcedExitWait =
        ClampMs(policy.forcedExitWait, std::chrono::milliseconds(50),
                std::chrono::milliseconds(5000));

    MutexGuard recoveryMutex;
    {
        const std::wstring mutexName = RecoveryMutexName(request);
        if (mutexName.empty())
            return MakeResult(Status::Failed, Detail::MutexUnavailable,
                              "Per-user recovery mutex name cannot be derived");
        recoveryMutex.handle = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (!recoveryMutex.handle)
            return MakeResult(Status::Failed, Detail::MutexUnavailable,
                              ErrorText("Create legacy adb recovery mutex"));
        const auto deadline = Clock::now() + policy.mutexAcquireTimeout;
        for (;;) {
            const DWORD waited = WaitForSingleObject(recoveryMutex.handle, 20);
            if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
                recoveryMutex.acquired = true;
                break;
            }
            if (waited != WAIT_TIMEOUT)
                return MakeResult(Status::Failed, Detail::MutexUnavailable,
                                  ErrorText("Wait for legacy adb recovery mutex"));
            if (Canceled(request))
                return MakeResult(Status::Canceled, Detail::None,
                                  "Legacy adb recovery canceled while waiting for the "
                                  "recovery mutex");
            if (Clock::now() >= deadline)
                return MakeResult(Status::Busy, Detail::MutexBusy,
                                  "Another legacy adb recovery attempt holds the mutex");
        }
    }
    // One process may call from several threads; the named mutex serializes
    // them too, but only one classification may act at a time here.
    static std::timed_mutex processMutex;
    std::unique_lock processLock(processMutex, std::defer_lock);
    {
        const auto deadline = Clock::now() + policy.mutexAcquireTimeout;
        while (!processLock.try_lock_for(std::chrono::milliseconds(20))) {
            if (Canceled(request))
                return MakeResult(Status::Canceled, Detail::None,
                                  "Legacy adb recovery canceled while waiting for the "
                                  "in-process recovery lock");
            if (Clock::now() >= deadline)
                return MakeResult(Status::Busy, Detail::MutexBusy,
                                  "Another legacy adb recovery attempt is in progress");
        }
    }

    if (Canceled(request))
        return MakeResult(Status::Canceled, Detail::None,
                          "Legacy adb recovery canceled before classification");

    DWORD listenerPid = 0;
    switch (FindLoopbackListenerPid(request.port, &listenerPid)) {
    case ListenerLookup::Found:
        break;
    case ListenerLookup::None:
        return MakeResult(Status::NotManaged, Detail::NoLoopbackListener,
                          "No loopback IPv4 listener on the requested port");
    case ListenerLookup::Ambiguous:
        return MakeResult(Status::NotManaged, Detail::AmbiguousListener,
                          "Several processes listen on the requested loopback port");
    default:
        return MakeResult(Status::Failed, Detail::ListenerQueryFailed,
                          ErrorText("Query loopback listener table"));
    }
    if (!listenerPid || listenerPid == GetCurrentProcessId()) {
        auto result = MakeResult(Status::NotManaged, Detail::ListenerProcessUnavailable,
                                 "Loopback listener is not another process");
        result.listenerProcessId = listenerPid;
        return result;
    }

    Target target;
    target.process.handle = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, listenerPid);
    if (!target.process.handle) {
        auto result = MakeResult(Status::NotManaged, Detail::ListenerProcessUnavailable,
                                 ErrorText("Open loopback listener process"));
        result.listenerProcessId = listenerPid;
        return result;
    }
    if (!CaptureIdentity(target.process.handle, &target.identity) ||
        target.identity.pid != listenerPid) {
        auto result = MakeResult(Status::NotManaged, Detail::ListenerProcessUnavailable,
                                 "Loopback listener identity cannot be captured");
        result.listenerProcessId = listenerPid;
        return result;
    }
    {
        Detail failDetail = Detail::InternalError;
        std::string provenanceDiagnostic;
        if (!VerifyProvenance(target.identity.canonicalImagePath, request.expectations,
                              &target.provenance, &failDetail, &provenanceDiagnostic)) {
            auto result = MakeResult(Status::NotManaged, failDetail, provenanceDiagnostic);
            result.listenerProcessId = target.identity.pid;
            result.imagePath = target.identity.canonicalImagePath;
            return result;
        }
    }
    if (SiblingOkuFlowRunning(policy.siblingScanRootForTest)) {
        auto result = MakeResult(Status::Busy, Detail::SiblingOkuFlowRunning,
                                 "Another OkuFlow instance is running");
        result.listenerProcessId = target.identity.pid;
        result.imagePath = target.identity.canonicalImagePath;
        return result;
    }
    {
        // A second stable snapshot closes the classification window: the
        // listener, creation time and image must all still agree.
        ProcessIdentity stable;
        DWORD stablePid = 0;
        if (!CaptureIdentity(target.process.handle, &stable) ||
            !SameIdentity(stable, target.identity) ||
            FindLoopbackListenerPid(request.port, &stablePid) != ListenerLookup::Found ||
            stablePid != target.identity.pid) {
            auto result = MakeResult(Status::IdentityChanged, Detail::ListenerChanged,
                                     "Listener identity changed during classification");
            result.listenerProcessId = target.identity.pid;
            result.imagePath = target.identity.canonicalImagePath;
            return result;
        }
    }
    if (policy.afterInitialClassificationForTest) policy.afterInitialClassificationForTest();

    LegacyAdbRecoveryResult outcome;
    outcome.listenerProcessId = target.identity.pid;
    outcome.imagePath = target.identity.canonicalImagePath;

    if (const auto abort = Revalidate(target, request)) return *abort;

    bool killDelivered = false;
    std::optional<LegacyAdbRecoveryResult> hostKillAbort;
    switch (SendHostKill(target, request, policy, &hostKillAbort)) {
    case KillDelivery::Delivered:
        killDelivered = true;
        break;
    case KillDelivery::Aborted:
        // Post-connect revalidation failed before any byte was sent; the
        // replacement (or changed) listener is untouched and nothing is
        // terminated from this stale classification.
        return *hostKillAbort;
    case KillDelivery::Canceled:
        outcome.status = Status::Canceled;
        outcome.detail = Detail::None;
        outcome.diagnostic = "Legacy adb recovery canceled while delivering host:kill";
        return outcome;
    default:
        outcome.detail = Detail::HostKillNotDelivered;
        outcome.diagnostic = "host:kill could not be delivered to the classified endpoint";
        break;
    }
    outcome.hostKillDelivered = killDelivered;
    if (killDelivered) {
        switch (WaitForExit(target.process.handle, policy.hostKillExitWait, request, true)) {
        case ExitWait::Exited:
            outcome.status = Status::Recovered;
            outcome.detail = Detail::None;
            outcome.stoppedViaHostKill = true;
            outcome.diagnostic = "Legacy OkuFlow-bundled adb server exited after host:kill";
            NotePortAfterExit(request.port, target.identity.pid, &outcome.diagnostic);
            return outcome;
        case ExitWait::Canceled:
            outcome.status = Status::Canceled;
            outcome.detail = Detail::None;
            outcome.diagnostic = "Legacy adb recovery canceled after host:kill; the "
                                 "classified server may still exit on its own";
            return outcome;
        default:
            break;
        }
    }

    if (auto abort = Revalidate(target, request)) {
        abort->hostKillDelivered = killDelivered;
        return *abort;
    }
    if (!TerminateProcess(target.process.handle, 1)) {
        if (!ProcessAlive(target.process.handle)) {
            outcome.status = Status::Recovered;
            outcome.detail = Detail::None;
            outcome.stoppedViaHostKill = killDelivered;
            outcome.diagnostic = "Legacy OkuFlow-bundled adb server exited before forced "
                                 "termination took effect";
            NotePortAfterExit(request.port, target.identity.pid, &outcome.diagnostic);
            return outcome;
        }
        outcome.status = Status::Failed;
        outcome.detail = Detail::TerminationFailed;
        outcome.diagnostic = ErrorText("Terminate classified legacy adb process");
        return outcome;
    }
    // Termination is already issued on the exact handle; confirm the exit
    // inside the same bound instead of reporting a cancellation race.
    if (WaitForExit(target.process.handle, policy.forcedExitWait, request, false) ==
        ExitWait::Exited) {
        outcome.status = Status::Recovered;
        outcome.detail = Detail::None;
        outcome.usedForcedTermination = true;
        outcome.diagnostic = "Legacy OkuFlow-bundled adb server terminated through its "
                             "exact held handle after host:kill did not stop it";
        NotePortAfterExit(request.port, target.identity.pid, &outcome.diagnostic);
        return outcome;
    }
    outcome.status = Status::Failed;
    outcome.detail = Detail::ExitWaitTimedOut;
    outcome.usedForcedTermination = true;
    outcome.diagnostic = "Classified legacy adb process did not exit within the forced "
                         "termination wait";
    return outcome;
}

} // namespace okuflow
#endif
