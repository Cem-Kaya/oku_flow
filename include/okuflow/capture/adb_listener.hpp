#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace okuflow {

struct AdbListenerIdentity {
    std::uint16_t port{};
    DWORD processId{};
    std::uint64_t created{};
    bool operator==(const AdbListenerIdentity&) const = default;
};

// One row of the Windows TCP listener tables (IPv4 and IPv6), reduced to
// what the adb listener lookups need.
struct TcpListenerRow {
    bool ipv6{false};
    bool loopback{false};    // 127.0.0.1 or ::1
    bool anyAddress{false};  // 0.0.0.0 or ::
    std::uint16_t port{0};
    DWORD processId{0};
};

namespace listener_detail {
inline std::mutex& RowsTransformMutex() { static std::mutex mutex; return mutex; }
inline std::function<void(std::vector<TcpListenerRow>&)>& RowsTransform()
{
    static std::function<void(std::vector<TcpListenerRow>&)> transform;
    return transform;
}
// One listener table; the table can grow between the size query and the
// read, so the read is retried.
template <typename Table>
bool ReadListenerTable(ULONG family, std::vector<std::uint8_t>* storage)
{
    ULONG bytes = 0;
    if (GetExtendedTcpTable(nullptr, &bytes, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
        return false;
    DWORD result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        storage->resize(std::max<ULONG>(bytes, sizeof(Table)));
        result = GetExtendedTcpTable(storage->data(), &bytes, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
    }
    return result == NO_ERROR;
}
} // namespace listener_detail

// IPv4 is required. IPv6 rows are included when available.
inline bool ReadTcpListenerRows(std::vector<TcpListenerRow>* rows)
{
    rows->clear();
    std::vector<std::uint8_t> v4, v6;
    if (!listener_detail::ReadListenerTable<MIB_TCPTABLE_OWNER_PID>(AF_INET, &v4)) return false;
    const auto* table4 = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(v4.data());
    for (DWORD i = 0; i < table4->dwNumEntries; ++i) {
        const auto& row = table4->table[i];
        TcpListenerRow out;
        out.port = ntohs(static_cast<u_short>(row.dwLocalPort));
        out.loopback = ntohl(row.dwLocalAddr) == INADDR_LOOPBACK;
        out.anyAddress = row.dwLocalAddr == INADDR_ANY;
        out.processId = row.dwOwningPid;
        rows->push_back(out);
    }
    // No IPv6 stack is not an error: the IPv4 rows stand alone.
    if (listener_detail::ReadListenerTable<MIB_TCP6TABLE_OWNER_PID>(AF_INET6, &v6)) {
        const auto* table6 = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(v6.data());
        for (DWORD i = 0; i < table6->dwNumEntries; ++i) {
            const auto& row = table6->table[i];
            TcpListenerRow out;
            out.ipv6 = true;
            out.port = ntohs(static_cast<u_short>(row.dwLocalPort));
            static const UCHAR any[16]{};
            static const UCHAR loopback[16]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            out.anyAddress = std::memcmp(row.ucLocalAddr, any, 16) == 0;
            out.loopback = std::memcmp(row.ucLocalAddr, loopback, 16) == 0;
            out.processId = row.dwOwningPid;
            rows->push_back(out);
        }
    }
    std::function<void(std::vector<TcpListenerRow>&)> transform;
    { std::lock_guard lock(listener_detail::RowsTransformMutex()); transform = listener_detail::RowsTransform(); }
    if (transform) transform(*rows);
    return true;
}

// IPv4 loopback rows take precedence over any-address rows. IPv6 loopback
// alone cannot accept a connection to 127.0.0.1.
inline std::vector<TcpListenerRow> AdbListenerRows(const std::vector<TcpListenerRow>& rows, std::uint16_t port)
{
    std::vector<TcpListenerRow> loopback, any;
    for (const auto& row : rows) {
        if (row.port != port) continue;
        if (!row.ipv6 && row.loopback) loopback.push_back(row);
        else if (row.anyAddress) any.push_back(row);
    }
    return loopback.empty() ? any : loopback;
}

// Test seam: model alternate ownership or addresses on private loopback
// fixtures without binding to every address.
inline void SetTcpListenerRowsTransformForTest(std::function<void(std::vector<TcpListenerRow>&)> transform)
{
    std::lock_guard lock(listener_detail::RowsTransformMutex());
    listener_detail::RowsTransform() = std::move(transform);
}

enum class AdbListenerLookup { Found, None, Ambiguous, Error };

// Collapse matching rows to a single owner before opening any process.
inline AdbListenerLookup FindLoopbackListener(std::uint16_t port, TcpListenerRow* listener,
                                              bool ipv4Only = false)
{
    std::vector<TcpListenerRow> rows;
    if (!ReadTcpListenerRows(&rows)) return AdbListenerLookup::Error;
    bool found = false;
    for (const auto& row : AdbListenerRows(rows, port)) {
        if (ipv4Only && row.ipv6) continue;
        if (found && listener->processId != row.processId) return AdbListenerLookup::Ambiguous;
        *listener = row;
        found = true;
    }
    return found ? AdbListenerLookup::Found : AdbListenerLookup::None;
}

// Identity from an already-open handle. A child must be alive at observation.
inline std::optional<AdbListenerIdentity> ReadAdbListenerIdentity(std::uint16_t port, DWORD pid, HANDLE process)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user) ||
        WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return std::nullopt;
    return AdbListenerIdentity{port, pid,
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
}

} // namespace okuflow
#endif
