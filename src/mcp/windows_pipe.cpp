#include "okuflow/mcp/windows_pipe.hpp"
#include "okuflow/mcp/protocol.hpp"
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#include <vector>

namespace okuflow::mcp {
namespace {
std::atomic_uint activeIo{};
struct PendingIo {
    Handle event;
    OVERLAPPED overlapped{};
    std::vector<char> buffer;
    DWORD count{};
    bool admitted{};
    explicit PendingIo(size_t bytes = 0) : buffer(bytes) {
        const auto previous = activeIo.fetch_add(1);
        if (previous >= 8) {
            activeIo.fetch_sub(1);
            return;
        }
        admitted = true;
        event = Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        overlapped.hEvent = event.value;
    }
    ~PendingIo() {
        if (admitted)
            activeIo.fetch_sub(1);
    }
};
// A broken kernel/driver cancellation must not turn a timeout into an infinite
// wait or free memory still referenced by OVERLAPPED. At most eight I/O slots
// can be retained for process lifetime; their <=512 KiB buffers are owned here.
void CancelPending(HANDLE pipe, std::unique_ptr<PendingIo> &operation) {
    CancelIoEx(pipe, &operation->overlapped);
    if (WaitForSingleObject(operation->event.value, 1000) != WAIT_OBJECT_0)
        operation.release();
}
std::vector<BYTE> Token(HANDLE process, TOKEN_INFORMATION_CLASS kind) {
    Handle token;
    HANDLE raw{};
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw))
        return {};
    token = Handle(raw);
    DWORD size{};
    GetTokenInformation(token.value, kind, nullptr, 0, &size);
    if (!size || size > 65536)
        return {};
    std::vector<BYTE> out(size);
    if (!GetTokenInformation(token.value, kind, out.data(), size, &size))
        return {};
    return out;
}
std::wstring LogonSid() {
    auto b = Token(GetCurrentProcess(), TokenGroups);
    if (b.empty())
        return {};
    auto *g = reinterpret_cast<TOKEN_GROUPS *>(b.data());
    for (DWORD i = 0; i < g->GroupCount; ++i) {
        if ((g->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
            LPWSTR str{};
            if (!ConvertSidToStringSidW(g->Groups[i].Sid, &str))
                return {};
            std::wstring result(str);
            LocalFree(str);
            return result;
        }
    }
    return {};
}
struct Security {
    PSECURITY_DESCRIPTOR sd{};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    Security() {
        auto sid = LogonSid();
        if (sid.empty())
            return;
        std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
            attributes.lpSecurityDescriptor = sd;
    }
    ~Security() {
        if (sd)
            LocalFree(sd);
    }
};
bool EquivalentTokens(HANDLE peer) {
    DWORD ourSession{}, peerSession{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ourSession) ||
        !ProcessIdToSessionId(GetProcessId(peer), &peerSession) || ourSession != peerSession)
        return false;
    auto ours = Token(GetCurrentProcess(), TokenUser), theirs = Token(peer, TokenUser);
    if (ours.empty() || theirs.empty() ||
        !EqualSid(reinterpret_cast<TOKEN_USER *>(ours.data())->User.Sid,
                  reinterpret_cast<TOKEN_USER *>(theirs.data())->User.Sid))
        return false;
    ours = Token(GetCurrentProcess(), TokenIntegrityLevel);
    theirs = Token(peer, TokenIntegrityLevel);
    if (ours.empty() || theirs.empty() ||
        !EqualSid(reinterpret_cast<TOKEN_MANDATORY_LABEL *>(ours.data())->Label.Sid,
                  reinterpret_cast<TOKEN_MANDATORY_LABEL *>(theirs.data())->Label.Sid))
        return false;
    ours = Token(GetCurrentProcess(), TokenGroups);
    theirs = Token(peer, TokenGroups);
    if (ours.empty() || theirs.empty())
        return false;
    auto *a = reinterpret_cast<TOKEN_GROUPS *>(ours.data());
    auto *b = reinterpret_cast<TOKEN_GROUPS *>(theirs.data());
    for (DWORD i = 0; i < a->GroupCount; ++i)
        if ((a->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
            for (DWORD j = 0; j < b->GroupCount; ++j)
                if ((b->Groups[j].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID)
                    return EqualSid(a->Groups[i].Sid, b->Groups[j].Sid) != FALSE;
    return false;
}
std::wstring FinalPath(HANDLE file) {
    std::vector<wchar_t> path(32768);
    auto n = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()),
                                       FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    return n && n < path.size() ? std::wstring(path.data(), n) : std::wstring();
}
bool SameFile(HANDLE a, HANDLE b) {
    FILE_ID_INFO x{}, y{};
    auto pa = FinalPath(a), pb = FinalPath(b);
    return !pa.empty() && !pb.empty() && _wcsicmp(pa.c_str(), pb.c_str()) == 0 &&
           GetFileInformationByHandleEx(a, FileIdInfo, &x, sizeof(x)) &&
           GetFileInformationByHandleEx(b, FileIdInfo, &y, sizeof(y)) &&
           x.VolumeSerialNumber == y.VolumeSerialNumber &&
           memcmp(x.FileId.Identifier, y.FileId.Identifier, sizeof(x.FileId.Identifier)) == 0;
}
bool Transfer(HANDLE pipe, void *bytes, DWORD size, bool write, const std::atomic_bool &stop,
              DWORD deadline) {
    auto started = GetTickCount64();
    DWORD moved{};
    while (moved < size) {
        if (stop.load() || GetTickCount64() - started >= deadline)
            return false;
        auto operation = std::make_unique<PendingIo>(size - moved);
        if (!operation->admitted || !operation->event)
            return false;
        if (write)
            memcpy(operation->buffer.data(), static_cast<char *>(bytes) + moved, size - moved);
        BOOL ok = write ? WriteFile(pipe, operation->buffer.data(), size - moved, &operation->count,
                                    &operation->overlapped)
                        : ReadFile(pipe, operation->buffer.data(), size - moved, &operation->count,
                                   &operation->overlapped);
        if (!ok && GetLastError() != ERROR_IO_PENDING)
            return false;
        if (!ok) {
            DWORD waited{};
            while ((waited = WaitForSingleObject(operation->event.value, 20)) == WAIT_TIMEOUT) {
                if (stop.load() || GetTickCount64() - started >= deadline) {
                    CancelPending(pipe, operation);
                    return false;
                }
            }
            if (waited != WAIT_OBJECT_0) {
                CancelPending(pipe, operation);
                return false;
            }
            if (!GetOverlappedResult(pipe, &operation->overlapped, &operation->count, FALSE))
                return false;
        }
        if (!operation->count)
            return false;
        if (!write)
            memcpy(static_cast<char *>(bytes) + moved, operation->buffer.data(), operation->count);
        moved += operation->count;
    }
    return true;
}
} // namespace
Handle::~Handle() {
    if (*this)
        CloseHandle(value);
}
Handle::Handle(Handle &&other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
Handle &Handle::operator=(Handle &&other) noexcept {
    if (this != &other) {
        if (*this)
            CloseHandle(value);
        value = other.value;
        other.value = INVALID_HANDLE_VALUE;
    }
    return *this;
}
std::filesystem::path CurrentImage() {
    std::vector<wchar_t> b(32768);
    DWORD n = static_cast<DWORD>(b.size());
    return QueryFullProcessImageNameW(GetCurrentProcess(), 0, b.data(), &n)
               ? std::filesystem::path(std::wstring(b.data(), n))
               : std::filesystem::path();
}
bool IsHex(std::string_view v, size_t n) {
    if (v.size() != n)
        return false;
    for (char c : v)
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f'))
            return false;
    return true;
}
std::string RandomHex(size_t bytes) {
    if (bytes > 64)
        return {};
    std::vector<unsigned char> b(bytes);
    if (BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return {};
    constexpr char h[] = "0123456789abcdef";
    std::string o;
    for (auto c : b) {
        o += h[c >> 4];
        o += h[c & 15];
    }
    return o;
}
std::shared_ptr<PeerIdentity> PeerIdentity::Verify(HANDLE pipe, bool server,
                                                   const std::filesystem::path &expected) {
    ULONG pid{};
    if (!(server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid)))
        return {};
    auto identity = std::shared_ptr<PeerIdentity>(new PeerIdentity);
    identity->process_ = Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!identity->process_ || !EquivalentTokens(identity->process_.value))
        return {};
    FILETIME exit{}, kernel{}, user{};
    if (!GetProcessTimes(identity->process_.value, &identity->created_, &exit, &kernel, &user))
        return {};
    std::vector<wchar_t> image(32768);
    DWORD n = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(identity->process_.value, 0, image.data(), &n))
        return {};
    identity->image_ =
        Handle(CreateFileW(std::wstring(image.data(), n).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Handle expectedFile(CreateFileW(expected.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!identity->image_ || !expectedFile || !SameFile(identity->image_.value, expectedFile.value))
        return {};
    identity->pid_ = pid;
    ULONG recheck{};
    if (!(server ? GetNamedPipeClientProcessId(pipe, &recheck)
                 : GetNamedPipeServerProcessId(pipe, &recheck)) ||
        recheck != pid || !identity->Alive())
        return {};
    return identity;
}
bool PeerIdentity::Alive() const {
    return process_ && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT;
}
bool PeerIdentity::SameProcess(const PeerIdentity &other) const {
    return Alive() && other.Alive() && pid_ == other.pid_ &&
           created_.dwHighDateTime == other.created_.dwHighDateTime &&
           created_.dwLowDateTime == other.created_.dwLowDateTime;
}
std::filesystem::path InstanceDirectory() {
    PWSTR p{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &p)))
        return {};
    std::filesystem::path dir = std::filesystem::path(p) / L"OkuFlow" / L"mcp-instances";
    CoTaskMemFree(p);
    return dir;
}
std::wstring InstanceDescriptor::PipeName() const {
    return IsHex(selector, 32) ? L"\\\\.\\pipe\\OkuFlow.Mcp." + std::wstring(selector.begin(), selector.end())
                               : std::wstring();
}
bool InstanceDescriptor::Publish() const {
    if (!IsHex(selector, 32) || !IsHex(appEpoch, 32))
        return false;
    auto dir = InstanceDirectory();
    if (dir.empty())
        return false;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return false;
    Security security;
    if (!security.sd)
        return false;
    Handle f(CreateFileW((dir / (selector + ".json")).c_str(), GENERIC_WRITE, 0, &security.attributes,
                         CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (!f)
        return false;
    std::string json =
        Json(Json::Object{
                 {"protocolVersion", ProtocolVersion}, {"selector", selector}, {"appEpoch", appEpoch}})
            .Dump();
    DWORD written{};
    return WriteFile(f.value, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) &&
           written == json.size() && FlushFileBuffers(f.value);
}
void InstanceDescriptor::Remove() const {
    if (IsHex(selector, 32)) {
        auto dir = InstanceDirectory();
        if (!dir.empty())
            DeleteFileW((dir / (selector + ".json")).c_str());
    }
}
InstanceDescriptor InstanceDescriptor::Read(std::string_view selector) {
    if (!IsHex(selector, 32))
        return {};
    auto dir = InstanceDirectory();
    if (dir.empty())
        return {};
    Handle f(CreateFileW((dir / (std::string(selector) + ".json")).c_str(), GENERIC_READ, FILE_SHARE_READ,
                         nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!f)
        return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(f.value, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || info.nFileSizeHigh ||
        info.nFileSizeLow > 1024)
        return {};
    std::string bytes(info.nFileSizeLow, '\0');
    DWORD n{};
    if (!ReadFile(f.value, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) || n != bytes.size())
        return {};
    try {
        auto j = Json::Parse(bytes);
        auto p = j.Find("protocolVersion"), s = j.Find("selector"), e = j.Find("appEpoch");
        if (!p || !p->IsString() || p->String() != ProtocolVersion || !s || !s->IsString() ||
            s->String() != selector || !e || !e->IsString() || !IsHex(e->String(), 32))
            return {};
        return {s->String(), e->String()};
    } catch (...) {
        return {};
    }
}
Handle CreateSecurePipe(const InstanceDescriptor &d) {
    Security sec;
    if (!sec.sd || d.PipeName().empty())
        return {};
    return Handle(CreateNamedPipeW(
        d.PipeName().c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
        static_cast<DWORD>(MaxIpcFrame), static_cast<DWORD>(MaxIpcFrame), 0, &sec.attributes));
}
Handle ConnectPipe(const InstanceDescriptor &d) {
    if (d.PipeName().empty())
        return {};
    return Handle(CreateFileW(d.PipeName().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                              nullptr));
}
bool AcceptPipe(HANDLE pipe, const std::atomic_bool &stop) {
    auto operation = std::make_unique<PendingIo>();
    if (!operation->admitted || !operation->event)
        return false;
    if (ConnectNamedPipe(pipe, &operation->overlapped))
        return true;
    auto err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED)
        return true;
    if (err != ERROR_IO_PENDING)
        return false;
    DWORD waited{};
    while ((waited = WaitForSingleObject(operation->event.value, 20)) == WAIT_TIMEOUT) {
        if (stop.load()) {
            CancelPending(pipe, operation);
            return false;
        }
    }
    if (waited != WAIT_OBJECT_0) {
        CancelPending(pipe, operation);
        return false;
    }
    return GetOverlappedResult(pipe, &operation->overlapped, &operation->count, FALSE) != FALSE;
}
bool ReadFrame(HANDLE p, std::string &bytes, const std::atomic_bool &stop, DWORD deadline) {
    unsigned char prefix[4];
    auto start = GetTickCount64();
    if (!Transfer(p, prefix, 4, false, stop, deadline))
        return false;
    uint32_t n = uint32_t(prefix[0]) | (uint32_t(prefix[1]) << 8) | (uint32_t(prefix[2]) << 16) |
                 (uint32_t(prefix[3]) << 24);
    if (!n || n > MaxIpcFrame)
        return false;
    bytes.resize(n);
    auto elapsed = GetTickCount64() - start;
    if (elapsed >= deadline)
        return false;
    return Transfer(p, bytes.data(), n, false, stop, static_cast<DWORD>(deadline - elapsed));
}
bool WriteFrame(HANDLE p, std::string_view bytes, const std::atomic_bool &stop, DWORD deadline) {
    if (bytes.empty() || bytes.size() > MaxIpcFrame)
        return false;
    uint32_t n = static_cast<uint32_t>(bytes.size());
    unsigned char prefix[]{static_cast<unsigned char>(n), static_cast<unsigned char>(n >> 8),
                           static_cast<unsigned char>(n >> 16), static_cast<unsigned char>(n >> 24)};
    auto start = GetTickCount64();
    if (!Transfer(p, prefix, 4, true, stop, deadline))
        return false;
    auto elapsed = GetTickCount64() - start;
    if (elapsed >= deadline)
        return false;
    return Transfer(p, const_cast<char *>(bytes.data()), n, true, stop,
                    static_cast<DWORD>(deadline - elapsed));
}
} // namespace okuflow::mcp
