#pragma once
#ifdef _WIN32
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <windows.h>

namespace okuflow::mcp {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle() = default;
    explicit Handle(HANDLE v) : value(v) {}
    ~Handle();
    Handle(Handle &&other) noexcept;
    Handle &operator=(Handle &&other) noexcept;
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
class PeerIdentity final {
  public:
    // Holds both process and image. No claimed PID, argv, metadata or client
    // name is used as authority. exactExpectedImage is an app-owned sibling.
    static std::shared_ptr<PeerIdentity> Verify(HANDLE pipe, bool server,
                                                const std::filesystem::path &exactExpectedImage);
    bool Alive() const;
    bool SameProcess(const PeerIdentity &other) const;

  private:
    Handle process_, image_;
    DWORD pid_{};
    FILETIME created_{};
};
struct InstanceDescriptor {
    std::string selector, appEpoch;
    std::wstring PipeName() const;
    bool Publish() const;
    void Remove() const;
    static InstanceDescriptor Read(std::string_view selector);
};
std::filesystem::path InstanceDirectory();
std::filesystem::path CurrentImage();
std::string RandomHex(size_t bytes = 16);
bool IsHex(std::string_view value, size_t length);
Handle CreateSecurePipe(const InstanceDescriptor &instance);
Handle ConnectPipe(const InstanceDescriptor &instance);
bool AcceptPipe(HANDLE pipe, const std::atomic_bool &stop);
bool ReadFrame(HANDLE pipe, std::string &bytes, const std::atomic_bool &stop, DWORD deadlineMs = 6500);
bool WriteFrame(HANDLE pipe, std::string_view bytes, const std::atomic_bool &stop, DWORD deadlineMs = 2000);
} // namespace okuflow::mcp
#endif
