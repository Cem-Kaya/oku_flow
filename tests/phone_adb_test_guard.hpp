#pragma once
// Test-only guard for the test targets that link phone_camera.cpp: their
// process-wide adb test hooks point at a private loopback port where nothing
// listens (Windows ADB interface reported absent), installed before the first
// test and restored whenever a test's own hooks end. A test without hooks of
// its own can then never reach the machine's default adb port.
#include "okuflow/capture/phone_camera.hpp"

#include <winsock2.h>
#include <cstdint>

namespace okuflow_test {

struct WinsockScope {
    WinsockScope() { WSADATA data{}; WSAStartup(MAKEWORD(2, 2), &data); }
    ~WinsockScope() { WSACleanup(); }
    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
};

// A loopback listener on the given port (0: any free one) that never
// accepts; Accepted reports whether a connection is waiting for it.
class LoopbackListener {
public:
    explicit LoopbackListener(std::uint16_t wanted = 0)
    {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(wanted);
        int length = sizeof(address);
        if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(socket_, 8) != 0 ||
            getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return;
        }
        port_ = ntohs(address.sin_port);
    }
    ~LoopbackListener() { if (socket_ != INVALID_SOCKET) closesocket(socket_); }
    LoopbackListener(const LoopbackListener&) = delete;
    LoopbackListener& operator=(const LoopbackListener&) = delete;
    bool Listening() const { return socket_ != INVALID_SOCKET; }
    std::uint16_t Port() const { return port_; }
    bool Accepted(int waitMs) const
    {
        if (socket_ == INVALID_SOCKET) return false;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval wait{waitMs / 1000, (waitMs % 1000) * 1000};
        return select(0, &readable, nullptr, nullptr, &wait) > 0;
    }
private:
    WinsockScope winsock_;
    SOCKET socket_{INVALID_SOCKET};
    std::uint16_t port_{};
};

// Picked once per process and closed again at once: nothing listens there.
// Port 0 (if no port could be picked) makes the endpoint unsupported, which
// reaches nothing either.
inline std::uint16_t ClosedAdbPort()
{
    static const std::uint16_t port = [] { LoopbackListener reserve; return reserve.Port(); }();
    return port;
}

inline void UseClosedAdbPort()
{
    okuflow::PhoneAdbTestHooks hooks;
    hooks.port = ClosedAdbPort();
    hooks.adbInterface = [] { return okuflow::PhoneAdbInterface::Absent; };
    okuflow::SetPhoneAdbTestHooks(hooks);
}

} // namespace okuflow_test
