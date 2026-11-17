#ifdef _WIN32
#include "okuflow/capture/adb_legacy_recovery.hpp"

#include <sddl.h>
#include <tlhelp32.h>

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
    // A request that lost its authority (the user chose another source)
    // ends like a canceled one: before every stop stage and right before
    // host:kill's bytes are written.
    return (request.canceled && request.canceled->load()) ||
           (request.shutdownRequested && request.shutdownRequested->load()) ||
           (request.stillWanted && !request.stillWanted());
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

// The PID is used solely to open the exact handle revalidated later.
AdbListenerLookup FindLoopbackListenerPid(std::uint16_t port, DWORD* pid)
{
    TcpListenerRow listener;
    const auto result = FindLoopbackListener(port, &listener);
    if (result == AdbListenerLookup::Found) *pid = listener.processId;
    return result;
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
    case AdbListenerLookup::Found:
        if (listenerPid != target.identity.pid) {
            abort.status = Status::IdentityChanged;
            abort.detail = Detail::ListenerChanged;
            abort.diagnostic = "Loopback listener changed owner before a stop stage";
            return abort;
        }
        break;
    case AdbListenerLookup::Error:
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
        const AdbListenerLookup lookup = FindLoopbackListenerPid(port, &listenerPid);
        if (lookup == AdbListenerLookup::None) return;
        if (lookup == AdbListenerLookup::Found && listenerPid != oldPid) {
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

// Raw host:kill to the loopback port after re-checking, post-connect, that
// the listener still belongs to `pid`. No adb client process is run.
// `stillWanted` is asked once more right before the first byte is written
// (the connect and the listener check can take a while): false sends
// nothing.
bool SendRawHostKill(std::uint16_t port, DWORD pid, const std::atomic<bool>* canceled,
                     const std::function<bool()>& stillWanted, const std::function<void()>& afterConnectForTest)
{
    static std::once_flag winsockOnce;
    static int winsockResult = -1;
    std::call_once(winsockOnce, [] {
        WSADATA data{};
        winsockResult = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (winsockResult != 0) return false;
    SocketGuard guard{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (guard.socket == INVALID_SOCKET) return false;
    u_long nonBlocking = 1;
    if (ioctlsocket(guard.socket, FIONBIO, &nonBlocking) != 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(guard.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        const auto deadline = Clock::now() + std::chrono::milliseconds(500);
        for (;;) {
            if (canceled && canceled->load()) return false;
            fd_set writable{};
            FD_SET(guard.socket, &writable);
            timeval slice{0, 20 * 1000};
            const int ready = select(0, nullptr, &writable, nullptr, &slice);
            if (ready > 0) {
                int socketError = 0;
                int bytes = sizeof(socketError);
                if (getsockopt(guard.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &bytes) != 0 ||
                    socketError != 0)
                    return false;
                break;
            }
            if (ready < 0 || Clock::now() >= deadline) return false;
        }
    }
    if (afterConnectForTest) afterConnectForTest();
    DWORD listener = 0;
    if (FindLoopbackListenerPid(port, &listener) != AdbListenerLookup::Found || listener != pid) return false;
    constexpr char requestBytes[] = "0009host:kill";
    constexpr int requestLength = static_cast<int>(sizeof(requestBytes) - 1);
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    for (int sent = 0; sent < requestLength;) {
        if (canceled && canceled->load()) return false;
        fd_set writable{};
        FD_SET(guard.socket, &writable);
        timeval slice{0, 20 * 1000};
        if (select(0, nullptr, &writable, nullptr, &slice) <= 0) {
            if (Clock::now() >= deadline) return false;
            continue;
        }
        if (sent == 0 && stillWanted && !stillWanted()) return false;
        const int now = send(guard.socket, requestBytes + sent, requestLength - sent, 0);
        if (now <= 0) return false;
        sent += now;
    }
    shutdown(guard.socket, SD_SEND);
    return true;
}

std::uint64_t CreationTicks(const FILETIME& time)
{
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

struct VisibleWindowSearch { DWORD pid{0}; bool found{false}; };
BOOL CALLBACK FindVisibleWindow(HWND window, LPARAM parameter)
{
    auto* search = reinterpret_cast<VisibleWindowSearch*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == search->pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
        search->found = true;
        return FALSE;
    }
    return TRUE;
}

} // namespace

DefaultAdbStopResult TryStopDefaultAdbServer(const DefaultAdbStopRequest& request)
{
    using S = DefaultAdbStopStatus;
    DefaultAdbStopResult result;
    const auto canceled = [&] { return request.canceled && request.canceled->load(); };
    if (canceled()) { result.status = S::Canceled; return result; }
    DWORD pid = 0;
    switch (FindLoopbackListenerPid(request.port, &pid)) {
    case AdbListenerLookup::Found: break;
    case AdbListenerLookup::None:
        result.status = S::NoListener; result.diagnostic = "No loopback listener"; return result;
    case AdbListenerLookup::Ambiguous:
        result.status = S::IdentityChanged; result.diagnostic = "Several processes listen on the port"; return result;
    default:
        result.status = S::Failed; result.diagnostic = ErrorText("Query loopback listener table"); return result;
    }
    result.processId = pid;
    if (!pid || pid == GetCurrentProcessId()) {
        result.status = S::SelfListener; result.diagnostic = "The listener is this process"; return result;
    }
    if (request.expectedProcessId && request.expectedProcessId != pid) {
        result.status = S::IdentityChanged; result.diagnostic = "Listener PID changed since the decision"; return result;
    }
    HandleGuard process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid)};
    bool canTerminate = process.handle != nullptr;
    if (!process.handle) {
        const DWORD error = GetLastError();
        process.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!process.handle) {
            result.status = error == ERROR_ACCESS_DENIED ? S::AccessDenied : S::Failed;
            result.diagnostic = ErrorText("Open the listener process");
            return result;
        }
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process.handle, &created, &exited, &kernel, &user) || !ProcessAlive(process.handle)) {
        result.status = S::IdentityChanged; result.diagnostic = "Listener process exited"; return result;
    }
    if (request.expectedCreation && CreationTicks(created) != request.expectedCreation) {
        result.status = S::IdentityChanged; result.diagnostic = "Listener creation time changed"; return result;
    }
    std::wstring image;
    if (!ProcessCanonicalImage(process.handle, &image)) {
        result.status = S::Failed; result.diagnostic = "Listener image cannot be read"; return result;
    }
    const std::wstring imageName = BaseName(image);
    result.diagnostic = "listener pid " + std::to_string(pid);
    if (!EqualNoCase(imageName, request.requiredImageName)) {
        result.status = S::NotAdb; result.diagnostic += " is not the adb server image"; return result;
    }
    const auto stillSame = [&] {
        DWORD listener = 0;
        FILETIME again{}, e{}, k{}, u{};
        return ProcessAlive(process.handle) &&
            GetProcessTimes(process.handle, &again, &e, &k, &u) && CreationTicks(again) == CreationTicks(created) &&
            FindLoopbackListenerPid(request.port, &listener) == AdbListenerLookup::Found && listener == pid;
    };
    const auto unwanted = [&] { return request.stillWanted && !request.stillWanted(); };
    if (request.sendHostKillFirst) {
        if (unwanted()) { result.status = S::Canceled; result.diagnostic += ": no longer wanted"; return result; }
        result.hostKillDelivered = SendRawHostKill(request.port, pid, request.canceled, request.stillWanted,
                                                   request.afterHostKillConnectForTest);
        // Not sent because the request lost its authority meanwhile: nothing
        // further (no termination either).
        if (!result.hostKillDelivered && unwanted()) {
            result.status = S::Canceled; result.diagnostic += ": no longer wanted"; return result;
        }
        if (result.hostKillDelivered) {
            const auto deadline = Clock::now() + ClampMs(request.hostKillWait, std::chrono::milliseconds(0),
                                                         std::chrono::milliseconds(5000));
            while (Clock::now() < deadline && ProcessAlive(process.handle)) {
                if (canceled()) { result.status = S::Canceled; return result; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!ProcessAlive(process.handle)) {
                result.status = S::Stopped; result.diagnostic += " exited after host:kill"; return result;
            }
        }
    }
    if (canceled()) { result.status = S::Canceled; return result; }
    if (!request.terminateIfAlive) {
        result.status = request.sendHostKillFirst && !result.hostKillDelivered ? S::Failed : S::HostKillIgnored;
        result.diagnostic += result.hostKillDelivered ? " outlived host:kill" : ": host:kill not delivered";
        return result;
    }
    if (!stillSame()) {
        if (!ProcessAlive(process.handle)) { result.status = S::Stopped; result.diagnostic += " exited"; return result; }
        result.status = S::IdentityChanged; result.diagnostic += " changed before termination"; return result;
    }
    if (!canTerminate) {
        result.status = S::AccessDenied; result.diagnostic += ": Windows denied termination"; return result;
    }
    if (unwanted()) { result.status = S::Canceled; result.diagnostic += ": no longer wanted"; return result; }
    if (!TerminateProcess(process.handle, 1)) {
        const DWORD error = GetLastError();
        if (!ProcessAlive(process.handle)) { result.status = S::Stopped; result.diagnostic += " exited"; return result; }
        result.status = error == ERROR_ACCESS_DENIED ? S::AccessDenied : S::Failed;
        result.diagnostic += ": " + ErrorText("Terminate the listener process");
        return result;
    }
    result.terminated = true;
    const auto deadline = Clock::now() + ClampMs(request.exitWait, std::chrono::milliseconds(50),
                                                 std::chrono::milliseconds(5000));
    while (Clock::now() < deadline && ProcessAlive(process.handle) && !canceled())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (ProcessAlive(process.handle) && canceled()) {
        result.status = S::Canceled; result.diagnostic += " terminated; exit wait canceled"; return result;
    }
    result.status = ProcessAlive(process.handle) ? S::Failed : S::Stopped;
    result.diagnostic += result.status == S::Stopped ? " terminated through its exact handle"
                                                     : " did not exit after termination";
    return result;
}

std::vector<OkuFlowSibling> FindOkuFlowSiblings(bool sameImageOnly)
{
    std::vector<OkuFlowSibling> siblings;
    std::wstring ownImage;
    {
        HandleGuard self{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId())};
        if (!self.handle || !ProcessCanonicalImage(self.handle, &ownImage)) return siblings;
    }
    HandleGuard snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.handle == INVALID_HANDLE_VALUE) return siblings;
    std::map<DWORD, DWORD> parents;
    std::map<DWORD, std::wstring> names;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.handle, &entry)) return siblings;
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
    if (!ProcessIdToSessionId(currentPid, &currentSession) || !CurrentUserSid(&currentSid, nullptr)) return siblings;
    const std::wstring ownName = BaseName(ownImage);
    for (const auto& [pid, name] : names) {
        if (pid == currentPid || ancestors.contains(pid)) continue;
        if (!EqualNoCase(name, ownName) && !EqualNoCase(name, L"OkuFlowApp.exe")) continue;
        DWORD session = 0;
        if (!ProcessIdToSessionId(pid, &session) || session != currentSession) continue;
        HandleGuard process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
        if (!process.handle) continue;
        HandleGuard token;
        std::vector<unsigned char> sid;
        if (!OpenProcessToken(process.handle, TOKEN_QUERY, &token.handle) || !TokenUserSid(token.handle, &sid) ||
            sid != currentSid)
            continue;
        std::wstring image;
        OkuFlowSibling sibling;
        sibling.processId = pid;
        sibling.sameImage = ProcessCanonicalImage(process.handle, &image) && EqualNoCase(image, ownImage);
        if (sameImageOnly && !sibling.sameImage) continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(process.handle, &created, &exited, &kernel, &user)) sibling.creation = CreationTicks(created);
        VisibleWindowSearch search{pid};
        EnumWindows(FindVisibleWindow, reinterpret_cast<LPARAM>(&search));
        sibling.hasVisibleWindow = search.found;
        siblings.push_back(sibling);
    }
    return siblings;
}

namespace {
std::wstring OwnCanonicalImage()
{
    HandleGuard self{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId())};
    std::wstring image;
    if (!self.handle || !ProcessCanonicalImage(self.handle, &image)) return {};
    return image;
}

// Every top-level window of the process (bounded): a stuck UI thread is not
// vouched for by a hidden helper window of another thread that still pumps.
struct TopLevelWindowSearch { DWORD pid{0}; std::vector<HWND> windows; };
BOOL CALLBACK CollectTopLevelWindows(HWND window, LPARAM parameter)
{
    auto* search = reinterpret_cast<TopLevelWindowSearch*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == search->pid) search->windows.push_back(window);
    return search->windows.size() < 32 ? TRUE : FALSE;
}
// The windows of one thread (the instance's recorded UI thread), bounded.
BOOL CALLBACK CollectThreadWindows(HWND window, LPARAM parameter)
{
    auto* search = reinterpret_cast<TopLevelWindowSearch*>(parameter);
    search->windows.push_back(window);
    return search->windows.size() < 32 ? TRUE : FALSE;
}
// The UI thread an instance recorded at registration, if it still belongs
// to that process.
std::optional<DWORD> RecordedUiThread(const std::wstring& tag, DWORD pid)
{
    HandleGuard mapping{OpenFileMappingW(FILE_MAP_READ, FALSE, OkuFlowInstanceUiThreadName(tag, pid).c_str())};
    if (!mapping.handle) return std::nullopt;
    const void* view = MapViewOfFile(mapping.handle, FILE_MAP_READ, 0, 0, sizeof(DWORD));
    if (!view) return std::nullopt;
    DWORD threadId = 0;
    std::memcpy(&threadId, view, sizeof(threadId));
    UnmapViewOfFile(view);
    HandleGuard thread{OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId)};
    if (!threadId || !thread.handle || GetProcessIdOfThread(thread.handle) != pid) return std::nullopt;
    return threadId;
}
bool StopRequested(const std::atomic<bool>* canceled) { return canceled && canceled->load(); }
// WM_NULL within 1 s, sent in 100 ms slices that observe cancellation.
bool AnswersWindowMessage(HWND window, const std::atomic<bool>* canceled)
{
    for (int slice = 0; slice < 10; ++slice) {
        if (StopRequested(canceled)) return false;
        DWORD_PTR answer = 0;
        SetLastError(ERROR_SUCCESS);
        if (SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &answer)) return true;
        // Hung (not pumping for 5 s) or gone: no point in waiting longer.
        if (GetLastError() != ERROR_TIMEOUT || !IsWindow(window)) return false;
    }
    return false;
}
// Waits for a process to exit in short slices that observe cancellation.
void WaitForExit(HANDLE process, std::chrono::milliseconds wait, const std::atomic<bool>* canceled)
{
    const auto deadline = Clock::now() + wait;
    while (Clock::now() < deadline && !StopRequested(canceled) &&
           WaitForSingleObject(process, 20) == WAIT_TIMEOUT) {}
}
bool HasVisibleWindow(DWORD pid)
{
    VisibleWindowSearch visible{pid};
    EnumWindows(FindVisibleWindow, reinterpret_cast<LPARAM>(&visible));
    return visible.found;
}
} // namespace

std::wstring OkuFlowInstallationTag(const std::wstring& canonicalImagePath)
{
    // FNV-1a 64 over the lower-case path: stable, no secrets, short.
    std::uint64_t hash = 1469598103934665603ULL;
    for (wchar_t ch : canonicalImagePath) {
        ch = static_cast<wchar_t>(towlower(ch));
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    wchar_t text[17]{};
    swprintf_s(text, L"%016llx", static_cast<unsigned long long>(hash));
    return text;
}

std::wstring OkuFlowInstallationTag()
{
    return OkuFlowInstallationTag(OwnCanonicalImage());
}

HANDLE RecordOkuFlowUiThread(const std::wstring& tag, DWORD processId, DWORD threadId)
{
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(DWORD),
                                        OkuFlowInstanceUiThreadName(tag, processId).c_str());
    if (!mapping) return nullptr;
    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(DWORD));
    if (!view) { CloseHandle(mapping); return nullptr; }
    std::memcpy(view, &threadId, sizeof(threadId));
    UnmapViewOfFile(view);
    return mapping;
}

std::unique_ptr<OkuFlowInstanceRegistration> OkuFlowInstanceRegistration::Register()
{
    const auto tag = OkuFlowInstallationTag();
    const DWORD pid = GetCurrentProcessId();
    HANDLE registered = CreateEventW(nullptr, TRUE, FALSE, OkuFlowInstanceObjectName(false, tag, pid).c_str());
    if (!registered) return nullptr;
    HANDLE exiting = CreateEventW(nullptr, TRUE, FALSE, OkuFlowInstanceObjectName(true, tag, pid).c_str());
    if (!exiting) { CloseHandle(registered); return nullptr; }
    // The caller is the GUI thread that owns the main window. Without this
    // record an observer falls back to every top-level window.
    HANDLE uiThread = RecordOkuFlowUiThread(tag, pid, GetCurrentThreadId());
    return std::unique_ptr<OkuFlowInstanceRegistration>(new OkuFlowInstanceRegistration(registered, exiting, uiThread));
}

OkuFlowInstanceRegistration::~OkuFlowInstanceRegistration()
{
    if (uiThread_) CloseHandle(uiThread_);
    if (exiting_) CloseHandle(exiting_);
    if (registered_) CloseHandle(registered_);
}

void OkuFlowInstanceRegistration::SignalExiting()
{
    if (exiting_) SetEvent(exiting_);
}

std::vector<OkuFlowSiblingObservation> ObserveOkuFlowSiblings(const std::wstring& image, const std::wstring& tag,
                                                               const std::atomic<bool>* canceled)
{
    std::vector<OkuFlowSiblingObservation> result;
    std::wstring wantedImage = image;
    if (wantedImage.empty()) wantedImage = OwnCanonicalImage();
    else if (!CanonicalFilePath(image, &wantedImage)) return result;
    if (wantedImage.empty()) return result;
    const std::wstring wantedTag = tag.empty() ? OkuFlowInstallationTag(wantedImage) : tag;
    const std::wstring wantedName = BaseName(wantedImage);
    HandleGuard snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.handle == INVALID_HANDLE_VALUE) return result;
    std::map<DWORD, DWORD> parents;
    std::map<DWORD, std::wstring> names;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.handle, &entry)) return result;
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
    if (!ProcessIdToSessionId(currentPid, &currentSession) || !CurrentUserSid(&currentSid, nullptr)) return result;
    for (const auto& [pid, name] : names) {
        if (StopRequested(canceled)) break;
        if (pid == currentPid || ancestors.contains(pid) || !EqualNoCase(name, wantedName)) continue;
        DWORD session = 0;
        if (!ProcessIdToSessionId(pid, &session) || session != currentSession) continue;
        HandleGuard process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
        if (!process.handle) continue;
        HandleGuard token;
        std::vector<unsigned char> sid;
        if (!OpenProcessToken(process.handle, TOKEN_QUERY, &token.handle) || !TokenUserSid(token.handle, &sid) ||
            sid != currentSid)
            continue;
        std::wstring candidateImage;
        if (!ProcessCanonicalImage(process.handle, &candidateImage) || !EqualNoCase(candidateImage, wantedImage)) continue;
        OkuFlowSiblingObservation sibling;
        sibling.processId = pid;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(process.handle, &created, &exited, &kernel, &user)) sibling.creation = CreationTicks(created);
        HandleGuard registered{OpenEventW(SYNCHRONIZE, FALSE, OkuFlowInstanceObjectName(false, wantedTag, pid).c_str())};
        sibling.registered = registered.handle != nullptr;
        if (sibling.registered) {
            HandleGuard exiting{OpenEventW(SYNCHRONIZE, FALSE, OkuFlowInstanceObjectName(true, wantedTag, pid).c_str())};
            sibling.exiting = exiting.handle && WaitForSingleObject(exiting.handle, 0) == WAIT_OBJECT_0;
        }
        sibling.hasVisibleWindow = HasVisibleWindow(pid);
        // A visible window is the user's to close whatever it answers: no
        // message (up to a second each on the discovery worker).
        if (!sibling.hasVisibleWindow) {
            // Judged by the recorded UI thread's windows only; without that
            // record (an older registration), by every top-level window.
            TopLevelWindowSearch windows{pid};
            const auto uiThread = sibling.registered ? RecordedUiThread(wantedTag, pid) : std::nullopt;
            if (uiThread) EnumThreadWindows(*uiThread, CollectThreadWindows, reinterpret_cast<LPARAM>(&windows));
            else EnumWindows(CollectTopLevelWindows, reinterpret_cast<LPARAM>(&windows));
            sibling.uiThreadHasNoWindow = uiThread && windows.windows.empty();
            sibling.answersWindowMessage = !windows.windows.empty() &&
                std::all_of(windows.windows.begin(), windows.windows.end(),
                            [&](HWND window) { return AnswersWindowMessage(window, canceled); });
        }
        result.push_back(sibling);
    }
    return result;
}

OkuFlowSiblingVerdict ClassifyOkuFlowSibling(const OkuFlowSiblingObservation& o, OkuFlowSiblingTrack& track,
                                             std::int64_t nowMs, const OkuFlowSiblingPolicy& policy)
{
    using V = OkuFlowSiblingVerdict;
    if (o.hasVisibleWindow) {
        track = {};
        return V::Windowed;
    }
    if (!o.registered) return V::UnregisteredWindowless;
    if (o.exiting) {
        if (track.exitingSinceMs < 0) track.exitingSinceMs = nowMs;
        return nowMs - track.exitingSinceMs >= policy.exitingGraceMs ? V::Leftover : V::Pending;
    }
    track.exitingSinceMs = -1;
    if (o.answersWindowMessage || o.uiThreadHasNoWindow) {
        // Hidden but alive (starting, or a window being recreated), or its
        // recorded UI thread has no window to judge at all.
        track.windowlessSinceMs = -1;
        return V::Pending;
    }
    if (track.windowlessSinceMs < 0) track.windowlessSinceMs = nowMs;
    return nowMs - track.windowlessSinceMs >= policy.windowlessConfirmMs ? V::Leftover : V::Pending;
}

bool StopLeftoverAdbChild(DWORD parentId, std::uint64_t parentCreation, const std::wstring& bundledAdbImage,
                          std::chrono::milliseconds exitWait, const std::atomic<bool>* canceled)
{
    std::wstring wantedImage;
    if (StopRequested(canceled) || !CanonicalFilePath(bundledAdbImage, &wantedImage)) return false;
    // The parent is held for the whole step: its PID cannot be reused while
    // the children are walked, and it must still be the observed process.
    HandleGuard parent{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parentId)};
    if (!parent.handle) return false;
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(parent.handle, &created, &exited, &kernel, &user) || !ProcessAlive(parent.handle) ||
            CreationTicks(created) != parentCreation)
            return false;
    }
    HandleGuard snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.handle == INVALID_HANDLE_VALUE) return false;
    std::vector<DWORD> children;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.handle, &entry)) return false;
    do {
        if (entry.th32ParentProcessID == parentId && entry.th32ProcessID != GetCurrentProcessId())
            children.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot.handle, &entry));
    bool stopped = false;
    for (const DWORD child : children) {
        if (StopRequested(canceled)) break;
        HandleGuard process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
                                        FALSE, child)};
        if (!process.handle) continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        // A parent PID can be reused: the child must be younger than the parent.
        if (!GetProcessTimes(process.handle, &created, &exited, &kernel, &user) ||
            CreationTicks(created) < parentCreation || !ProcessAlive(process.handle))
            continue;
        std::wstring image;
        if (!ProcessCanonicalImage(process.handle, &image) || !EqualNoCase(image, wantedImage)) continue;
        if (!TerminateProcess(process.handle, 1)) continue;
        WaitForExit(process.handle, exitWait, canceled);
        stopped = stopped || !ProcessAlive(process.handle);
    }
    return stopped;
}

LeftoverInstanceStopStatus StopLeftoverOkuFlowInstance(const LeftoverInstanceStopRequest& request)
{
    using S = LeftoverInstanceStopStatus;
    if (!request.processId || request.processId == GetCurrentProcessId()) return S::IdentityChanged;
    std::wstring expectedImage = request.expectedImage.empty() ? OwnCanonicalImage() : request.expectedImage;
    if (!request.expectedImage.empty() && !CanonicalFilePath(request.expectedImage, &expectedImage)) return S::IdentityChanged;
    const std::wstring bundledAdb = request.bundledAdbImage.empty()
        ? JoinPath(JoinPath(DirName(expectedImage), L"platform-tools"), L"adb.exe") : request.bundledAdbImage;
    HandleGuard process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE,
                                    FALSE, request.processId)};
    bool canTerminate = process.handle != nullptr && !request.refuseTerminationForTest;
    if (!process.handle)
        process.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, request.processId);
    // Not even the identity can be checked (or the process is gone): stop
    // nothing. Whoever has that PID now is not walked for children.
    if (!process.handle) return S::Refused;
    FILETIME created{}, exited{}, kernel{}, user{};
    std::wstring image;
    if (!GetProcessTimes(process.handle, &created, &exited, &kernel, &user) || !ProcessAlive(process.handle) ||
        CreationTicks(created) != request.creation || !ProcessCanonicalImage(process.handle, &image) ||
        !EqualNoCase(image, expectedImage))
        return S::IdentityChanged;
    if (StopRequested(request.canceled)) return S::IdentityChanged;
    // A window that appeared since the observation is the user's to close.
    const bool visibleNow = request.visibleWindowForTest ? request.visibleWindowForTest(request.processId)
                                                         : HasVisibleWindow(request.processId);
    if (visibleNow) return S::IdentityChanged;
    if (canTerminate && TerminateProcess(process.handle, 1)) {
        WaitForExit(process.handle, request.exitWait, request.canceled);
        if (!ProcessAlive(process.handle)) return S::Stopped;
    }
    // Still holding the parent handle whose creation and image matched.
    return StopLeftoverAdbChild(request.processId, request.creation, bundledAdb, request.exitWait, request.canceled)
        ? S::AdbChildStopped : S::Refused;
}

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
    case AdbListenerLookup::Found:
        break;
    case AdbListenerLookup::None:
        return MakeResult(Status::NotManaged, Detail::NoLoopbackListener,
                          "No loopback IPv4 listener on the requested port");
    case AdbListenerLookup::Ambiguous:
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
            FindLoopbackListenerPid(request.port, &stablePid) != AdbListenerLookup::Found ||
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
        if (policy.afterHostKillDeliveredForTest) policy.afterHostKillDeliveredForTest();
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
