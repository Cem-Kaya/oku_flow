#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace okuflow {

// One exact child process owned through a non-inherited Windows Job Object
// (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE). The child is created suspended with
// CREATE_NO_WINDOW and an explicit inherited-handle list containing only NUL
// std handles, assigned to the job, and only then resumed. Normal owner exit
// stops it through the destructor; owner crash/kill closes the last job
// handle in the kernel, which terminates the child. Termination always uses
// the exact process/job handles held here, never a PID or process name.
class OwnedChildProcess {
public:
    struct LaunchOptions {
        std::wstring executable;
        std::vector<std::wstring> arguments;
        std::wstring workingDirectory;
        // Test seam only: replaces AssignProcessToJobObject(job, process).
        // Production launchers leave it empty.
        std::function<bool(HANDLE job, HANDLE process)> assignOverride;
        std::vector<std::pair<std::wstring, std::wstring>> environmentOverrides;
    };
    // Returns null on any failure; a created-but-unassigned/unresumed fresh
    // child is terminated through its exact handle before returning.
    static std::unique_ptr<OwnedChildProcess> Launch(const LaunchOptions& options,
                                                     std::string* error = nullptr);
    ~OwnedChildProcess();
    OwnedChildProcess(const OwnedChildProcess&) = delete;
    OwnedChildProcess& operator=(const OwnedChildProcess&) = delete;
    bool IsRunning() const noexcept;
    bool WaitForExit(std::chrono::milliseconds timeout) const noexcept;
    bool OwnsListeningPort(std::uint16_t port) const;
    // Caller closes this non-inherited SYNCHRONIZE-only handle. It cannot
    // terminate the child and remains valid while lifecycle handles change.
    HANDLE DuplicateObservationHandle() const;
    // Exact job termination plus a bounded wait; idempotent and noexcept.
    void Stop() noexcept;
    // Observation/diagnostics only; never used to find or kill a process.
    DWORD ProcessId() const noexcept { return processId_; }
private:
    OwnedChildProcess(HANDLE job, HANDLE process, HANDLE thread, DWORD processId)
        : job_(job), process_(process), thread_(thread), processId_(processId) {}
    HANDLE job_{nullptr};
    HANDLE process_{nullptr};
    HANDLE thread_{nullptr};
    DWORD processId_{0};
};

enum class OwnedServerProbe { Responsive, Unresponsive, NoListener };

struct OwnedAdbServerPolicy {
    // Minimum spacing between owned-server restarts/relaunches; also prevents
    // crash-looping a child that exits immediately.
    std::chrono::milliseconds restartCooldown{30000};
    std::chrono::milliseconds readinessDeadline{5000};
    std::chrono::milliseconds pollSlice{50};
    // Consecutive non-responsive probes required before a restart is allowed.
    int unresponsiveProbesToConfirm{2};
    int maxLaunchAttemptsPerEnsure{2};
    // Production verifies the IPv4 loopback listener belongs to our exact
    // child. Scripted, non-network lifecycle fixtures leave this disabled.
    bool verifyListenerOwnership{false};
};

// Lifecycle policy for a server this process launched itself. It can only
// act on the exact OwnedChildProcess it holds: borrowed/external servers are
// never represented here and can therefore never be stopped by it. All
// network/process effects go through the injected functions so contract
// tests run with private fixture children and scripted probes; production
// wiring lives in phone_camera.cpp.
class OwnedAdbServer {
public:
    using ProbeFn = std::function<OwnedServerProbe(std::uint16_t port,
                                                    const std::atomic<bool>* canceled)>;
    using LaunchFn = std::function<std::unique_ptr<OwnedChildProcess>(std::uint16_t port)>;
    using PortPickerFn = std::function<std::uint16_t()>;
    // True while a live/starting camera session uses this server.
    using SessionsFn = std::function<bool()>;
    enum class EnsureResult {
        Running, Launched, Restarted, NotReady, LaunchFailed,
        RestartDeferredLiveSession, Throttled, Canceled, Stopped, BorrowedUnresponsive
    };
    OwnedAdbServer(ProbeFn probe, LaunchFn launch, PortPickerFn pickPort,
                   SessionsFn hasLiveSessions, OwnedAdbServerPolicy policy = {});
    ~OwnedAdbServer();
    OwnedAdbServer(const OwnedAdbServer&) = delete;
    OwnedAdbServer& operator=(const OwnedAdbServer&) = delete;
    // Serialized: each waiter rechecks the current child before any effects.
    // Cancellation is checked before every probe batch, stop
    // and launch, and always wins over a restart.
    // allowLifecycleEffects=false performs only an exact-child health probe:
    // it never launches, restarts or retires support for ordinary enumeration.
    EnsureResult EnsureRunning(const std::atomic<bool>* canceled, bool allowLifecycleEffects = true);
    // Permanent for this manager: stops the exact owned child (if any) and
    // makes later EnsureRunning calls return Stopped.
    void Stop();
    bool OwnsRunningServer() const;
    // Nonblocking snapshot of a live child, not a readiness guarantee. The
    // caller must accept only ready EnsureResult values before host traffic.
    std::optional<std::uint16_t> Port() const;
    int LaunchCount() const;
    int RestartCount() const;
private:
    bool Canceled(const std::atomic<bool>* canceled) const
    { return stopRequested_.load() || (canceled && canceled->load()); }
    bool ResponsiveOwnedChild(OwnedServerProbe response) const;
    bool WaitSlice(const std::atomic<bool>* canceled) const;
    EnsureResult LaunchAndAwaitLocked(const std::atomic<bool>* canceled, bool restart);
    ProbeFn probe_;
    LaunchFn launch_;
    PortPickerFn pickPort_;
    SessionsFn hasLiveSessions_;
    OwnedAdbServerPolicy policy_;
    mutable std::timed_mutex mutex_;
    struct RunningSnapshot {
        HANDLE process{nullptr};
        std::uint16_t port{};
        ~RunningSnapshot() { if (process) CloseHandle(process); }
    };
    std::atomic<std::shared_ptr<const RunningSnapshot>> observation_;
    std::unique_ptr<OwnedChildProcess> child_;
    std::optional<std::uint16_t> port_;
    std::chrono::steady_clock::time_point nextRestart_{};
    int launches_{0};
    int restarts_{0};
    bool stopped_{false};
    std::atomic<bool> stopRequested_{false};
};

} // namespace okuflow
#endif
