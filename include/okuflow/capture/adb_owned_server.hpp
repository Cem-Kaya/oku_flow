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

#include "okuflow/capture/adb_listener.hpp"

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
    // Caller closes this non-inherited query/synchronize handle. It cannot
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
    // crash-looping a child that exits immediately. Consecutive launches
    // without an event double it up to maxRestartCooldown; ResetBudgets
    // (arrival, resume, manual pick) returns to the base.
    std::chrono::milliseconds restartCooldown{5000};
    std::chrono::milliseconds maxRestartCooldown{30000};
    std::chrono::milliseconds readinessDeadline{5000};
    std::chrono::milliseconds pollSlice{50};
    // A child that is alive and has never answered yet (antivirus scanning
    // the executable, a loaded laptop) is not judged hung before this much
    // time since its launch; only one that answered once and then stopped is.
    std::chrono::milliseconds startGrace{30000};
    // Spacing of the confirmation probes before a hang restart, so one
    // stall of a fraction of a second never restarts the server.
    std::chrono::milliseconds confirmationSpacing{1000};
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
    // `stillWanted` (optional): the caller's authority (a discovery cycle or
    // a ladder action for a source the user may leave meanwhile). Asked with
    // every cancellation check once the manager's lock is held, so a launch,
    // a restart or a replacement of the child, and the waits before them,
    // stop for a request that is no longer wanted (result Canceled).
    EnsureResult EnsureRunning(const std::atomic<bool>* canceled, bool allowLifecycleEffects = true,
                               std::function<bool()> stillWanted = {});
    // Doctor rung 3: restart a responsive but blind owned child now. Live
    // sessions and cancellation win; the doctor's budget paces it.
    EnsureResult Restart(const std::atomic<bool>* canceled, std::function<bool()> stillWanted = {});
    // Events clear the restart cooldown and its doubling. Lock-free: safe on
    // the UI thread while a worker holds the manager through a launch; the
    // request is applied by the next EnsureRunning/Restart.
    void ResetBudgets();
    // Permanent for this manager: stops the exact owned child (if any) and
    // makes later EnsureRunning calls return Stopped.
    void Stop();
    bool OwnsRunningServer() const;
    // Nonblocking snapshot of a live child, not a readiness guarantee. The
    // caller must accept only ready EnsureResult values before host traffic.
    std::optional<std::uint16_t> Port() const;
    // Verify the snapshot child still uniquely owns the requested IPv4
    // loopback listener, using the same ownership check as readiness.
    std::optional<AdbListenerIdentity> ListeningIdentity(std::uint16_t port) const;
    int LaunchCount() const;
    int RestartCount() const;
    // Starts of this manager's child that never answered: one for each
    // start grace a child stays alive without ever answering (also one that
    // is replaced, and one whose listener ownership cannot be verified), and
    // one for a child that died before it ever answered (counted by the
    // next EnsureRunning). A responsive child whose listener ownership stays
    // unverifiable after readiness also counts once per start grace.
    // Only a verified response resets the count.
    // Lock-free: readable while a worker holds the manager through a launch.
    int UnansweredStarts() const { return unansweredStarts_.load(); }
    // The same count, for a manager that is about to be dropped (the caller
    // keeps it, then calls Stop): also a child that died before it ever
    // answered and was not counted yet, and one dropped alive whose
    // readiness wait ran to its deadline unanswered (or that a probe found
    // silent past it). A start cut short by a source change or a
    // cancellation adds nothing, whatever its age. Interrupts work in flight
    // first, as Stop does (the manager serves nothing afterwards).
    int UnansweredStartsAtRetirement();
private:
    bool Canceled(const std::atomic<bool>* canceled) const
    { return stopRequested_.load() || (canceled && canceled->load()); }
    // Canceled, or the current call's request is no longer wanted. Only
    // while mutex_ is held (wanted_ belongs to the call holding it).
    bool Halted(const std::atomic<bool>* canceled) const
    { return Canceled(canceled) || (wanted_ && !wanted_()); }
    std::function<bool()> wanted_;
    struct WantedScope {
        WantedScope(OwnedAdbServer* owner, std::function<bool()> wanted) : owner(owner)
        { owner->wanted_ = std::move(wanted); }
        ~WantedScope() { owner->wanted_ = {}; }
        OwnedAdbServer* owner;
    };
    bool ObserveChildReadinessLocked(OwnedServerProbe response);
    bool WaitSlice(const std::atomic<bool>* canceled) const;
    bool WaitFor(std::chrono::milliseconds duration, const std::atomic<bool>* canceled) const;
    void ApplyBudgetResetLocked();
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
    std::chrono::steady_clock::time_point launchedAt_{};
    bool answered_{false};
    // First or most recently counted responsive probe with unverified ownership.
    std::optional<std::chrono::steady_clock::time_point> unverifiedOwnershipAt_;
    // When the current child last counted (or was launched), and whether its
    // death before answering was counted.
    std::chrono::steady_clock::time_point unansweredCountedAt_{};
    bool deathCounted_{false};
    // The current child was observed missing its readiness deadline (its
    // wait ran out, or a probe after the deadline failed).
    bool readinessMissed_{false};
    std::atomic<int> unansweredStarts_{0};
    void CountUnansweredLocked();
    std::atomic<bool> budgetResetRequested_{false};
    int launches_{0};
    int launchesSinceReset_{0};
    int restarts_{0};
    bool stopped_{false};
    std::atomic<bool> stopRequested_{false};
};

} // namespace okuflow
#endif
