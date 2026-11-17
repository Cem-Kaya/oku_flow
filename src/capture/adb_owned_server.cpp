#ifdef _WIN32
#include "okuflow/capture/adb_owned_server.hpp"

#include <algorithm>
#include <cwchar>
#include <map>
#include <thread>
#include <utility>

namespace okuflow {
namespace {

struct HandleGuard {
    HANDLE handle{nullptr};
    ~HandleGuard() { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    HANDLE Release() { HANDLE value = handle; handle = nullptr; return value; }
};

// Standard CreateProcess command-line quoting (CommandLineToArgvW rules).
std::wstring QuoteArgument(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
        return argument;
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') { ++backslashes; continue; }
        if (ch == L'"') quoted.append(backslashes * 2 + 1, L'\\');
        else quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted += ch;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

std::string LastErrorText(const char* operation)
{
    return std::string(operation) + " (error " + std::to_string(GetLastError()) + ")";
}

std::vector<wchar_t> ChildEnvironment(
    const std::vector<std::pair<std::wstring, std::wstring>>& overrides)
{
    struct IgnoreCase {
        bool operator()(const std::wstring& a, const std::wstring& b) const
        { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };
    std::map<std::wstring, std::wstring, IgnoreCase> values;
    if (wchar_t* inherited = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = inherited; *entry; entry += std::wcslen(entry) + 1) {
            const std::wstring value(entry);
            const auto split = value.find(L'=', value.front() == L'=' ? 1 : 0);
            if (split != std::wstring::npos)
                values[value.substr(0, split)] = value.substr(split + 1);
        }
        FreeEnvironmentStringsW(inherited);
    }
    for (const auto& [key, value] : overrides) values[key] = value;
    std::vector<wchar_t> block;
    for (const auto& [key, value] : values) {
        const auto entry = key + L'=' + value;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (block.size() == 1) block.push_back(L'\0');
    return block;
}

} // namespace

std::unique_ptr<OwnedChildProcess> OwnedChildProcess::Launch(const LaunchOptions& options,
                                                             std::string* error)
{
    const auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return std::unique_ptr<OwnedChildProcess>{};
    };
    if (options.executable.empty()) return fail("Owned child executable is empty");
    for (const auto& [key, value] : options.environmentOverrides) {
        if (key.empty() || key.find_first_of(L"=\0", 0, 2) != std::wstring::npos ||
            value.find(L'\0') != std::wstring::npos)
            return fail("Invalid owned-child environment override");
    }

    SECURITY_ATTRIBUTES jobAttributes{sizeof(jobAttributes), nullptr, FALSE};
    HandleGuard job{CreateJobObjectW(&jobAttributes, nullptr)};
    if (!job.handle) return fail(LastErrorText("Create owned-child job"));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.handle, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
        return fail(LastErrorText("Configure owned-child job"));

    // Only NUL std handles may be inherited, listed explicitly so no other
    // inheritable handle of this process (including the job) leaks.
    SECURITY_ATTRIBUTES nulAttributes{sizeof(nulAttributes), nullptr, TRUE};
    HandleGuard stdIn{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &nulAttributes, OPEN_EXISTING, 0, nullptr)};
    HandleGuard stdOut{CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &nulAttributes, OPEN_EXISTING, 0, nullptr)};
    HandleGuard stdErr{CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &nulAttributes, OPEN_EXISTING, 0, nullptr)};
    if (stdIn.handle == INVALID_HANDLE_VALUE || stdOut.handle == INVALID_HANDLE_VALUE ||
        stdErr.handle == INVALID_HANDLE_VALUE)
        return fail(LastErrorText("Open owned-child std handles"));
    HANDLE inherited[]{stdIn.handle, stdOut.handle, stdErr.handle};

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdIn.handle;
    startup.StartupInfo.hStdOutput = stdOut.handle;
    startup.StartupInfo.hStdError = stdErr.handle;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::uint8_t> attributeStorage(attributeBytes);
    startup.lpAttributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes))
        return fail(LastErrorText("Prepare owned-child attributes"));
    struct AttributeCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST list{};
        ~AttributeCleanup() { if (list) DeleteProcThreadAttributeList(list); }
    } attributeCleanup{startup.lpAttributeList};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr))
        return fail(LastErrorText("Restrict owned-child inherited handles"));

    std::wstring commandLine = QuoteArgument(options.executable);
    for (const auto& argument : options.arguments)
        commandLine += L' ' + QuoteArgument(argument);
    PROCESS_INFORMATION process{};
    auto environment = options.environmentOverrides.empty() ? std::vector<wchar_t>{}
                                                            : ChildEnvironment(options.environmentOverrides);
    const BOOL created = CreateProcessW(
        options.executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        environment.empty() ? nullptr : environment.data(),
        options.workingDirectory.empty() ? nullptr : options.workingDirectory.c_str(),
        &startup.StartupInfo, &process);
    if (!created) return fail(LastErrorText("Create owned child"));
    HandleGuard childProcess{process.hProcess};
    HandleGuard childThread{process.hThread};
    // From here the fresh suspended child exists; every failure terminates
    // exactly it through this handle before returning.
    const auto abortChild = [&](const std::string& message) {
        TerminateProcess(childProcess.handle, 1);
        WaitForSingleObject(childProcess.handle, 2000);
        return fail(message);
    };
    const bool assigned = options.assignOverride
        ? options.assignOverride(job.handle, childProcess.handle)
        : AssignProcessToJobObject(job.handle, childProcess.handle) != FALSE;
    if (!assigned) return abortChild(LastErrorText("Assign owned child to job"));
    if (ResumeThread(childThread.handle) == static_cast<DWORD>(-1)) {
        const auto resumeError = LastErrorText("Resume owned child");
        // Assignment succeeded, so job termination is the exact cleanup.
        TerminateJobObject(job.handle, 1);
        return abortChild(resumeError);
    }
    return std::unique_ptr<OwnedChildProcess>(new OwnedChildProcess(
        job.Release(), childProcess.Release(), childThread.Release(), process.dwProcessId));
}

OwnedChildProcess::~OwnedChildProcess() { Stop(); }

bool OwnedChildProcess::IsRunning() const noexcept
{
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool OwnedChildProcess::WaitForExit(std::chrono::milliseconds timeout) const noexcept
{
    if (!process_) return true;
    const auto wait = timeout.count() < 0 ? DWORD{0}
        : static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), INFINITE - 1));
    return WaitForSingleObject(process_, wait) == WAIT_OBJECT_0;
}

namespace {
bool ProcessOwnsListeningPort(HANDLE process, DWORD pid, std::uint16_t port)
{
    if (!process || WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return false;
    TcpListenerRow listener;
    return FindLoopbackListener(port, &listener) == AdbListenerLookup::Found &&
        !listener.ipv6 && listener.loopback && listener.processId == pid &&
        WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}
} // namespace

bool OwnedChildProcess::OwnsListeningPort(std::uint16_t port) const
{
    return ProcessOwnsListeningPort(process_, processId_, port);
}

HANDLE OwnedChildProcess::DuplicateObservationHandle() const
{
    HANDLE observation = nullptr;
    return process_ && DuplicateHandle(GetCurrentProcess(), process_, GetCurrentProcess(),
        &observation, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0) ? observation : nullptr;
}

void OwnedChildProcess::Stop() noexcept
{
    if (job_) TerminateJobObject(job_, 1);
    if (process_) {
        WaitForSingleObject(process_, 2000);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (thread_) { CloseHandle(thread_); thread_ = nullptr; }
    // Closing the last (non-inherited) job handle is the final backstop via
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE even if the wait above timed out.
    if (job_) { CloseHandle(job_); job_ = nullptr; }
}

OwnedAdbServer::OwnedAdbServer(ProbeFn probe, LaunchFn launch, PortPickerFn pickPort,
                               SessionsFn hasLiveSessions, OwnedAdbServerPolicy policy)
    : probe_(std::move(probe)), launch_(std::move(launch)),
      pickPort_(std::move(pickPort)), hasLiveSessions_(std::move(hasLiveSessions)),
      policy_(policy) {}

OwnedAdbServer::~OwnedAdbServer() { Stop(); }

bool OwnedAdbServer::ObserveChildReadinessLocked(OwnedServerProbe response)
{
    if (response != OwnedServerProbe::Responsive || !child_ || !child_->IsRunning() || !port_)
        return false;
    if (!answered_ && policy_.verifyListenerOwnership && !child_->OwnsListeningPort(*port_)) return false;
    answered_ = true;
    unansweredStarts_.store(0);
    return true;
}

bool OwnedAdbServer::WaitSlice(const std::atomic<bool>* canceled) const
{
    return WaitFor(policy_.pollSlice, canceled);
}

bool OwnedAdbServer::WaitFor(std::chrono::milliseconds duration, const std::atomic<bool>* canceled) const
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (Halted(canceled)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return !Halted(canceled);
}

OwnedAdbServer::EnsureResult OwnedAdbServer::LaunchAndAwaitLocked(
    const std::atomic<bool>* canceled, bool restart)
{
    for (int attempt = 0; attempt < std::max(1, policy_.maxLaunchAttemptsPerEnsure); ++attempt) {
        if (Halted(canceled)) return EnsureResult::Canceled;
        // Every restart uses a fresh private port; the prior listener's
        // accepted connections can outlive the process's exclusive bind.
        if (!port_) port_ = pickPort_();
        if (!*port_) { port_.reset(); return EnsureResult::LaunchFailed; }
        if (Halted(canceled)) return EnsureResult::Canceled;
        child_ = launch_(*port_);
        // Budget recorded even for a failed attempt so a broken executable
        // cannot be retried on every discovery cycle.
        {
            const auto base = policy_.restartCooldown;
            auto cooldown = base;
            for (int i = 0; i < launchesSinceReset_ && cooldown < policy_.maxRestartCooldown; ++i)
                cooldown *= 2;
            cooldown = std::max(base, std::min(cooldown, policy_.maxRestartCooldown));
            ++launchesSinceReset_;
            nextRestart_ = std::chrono::steady_clock::now() + cooldown;
        }
        if (!child_) { port_.reset(); return EnsureResult::LaunchFailed; }
        HandleGuard observedProcess{child_->DuplicateObservationHandle()};
        if (!observedProcess.handle) { child_.reset(); port_.reset(); return EnsureResult::LaunchFailed; }
        auto snapshot = std::make_shared<RunningSnapshot>();
        snapshot->process = observedProcess.Release();
        snapshot->port = *port_;
        observation_.store(std::move(snapshot));
        ++launches_;
        launchedAt_ = std::chrono::steady_clock::now();
        answered_ = false;
        unansweredCountedAt_ = launchedAt_;
        deathCounted_ = false;
        const auto deadline = std::chrono::steady_clock::now() + policy_.readinessDeadline;
        for (;;) {
            if (Halted(canceled)) return EnsureResult::Canceled; // Child stays owned.
            if (!child_->IsRunning()) {
                // Our child lost (e.g. the port was claimed between picking
                // and binding). Anything answering there is not ours to use
                // or stop; abandon that port and try another.
                child_.reset();
                port_.reset();
                observation_.store(nullptr);
                break;
            }
            const auto response = probe_(*port_, canceled);
            if (Halted(canceled)) return EnsureResult::Canceled;
            if (ObserveChildReadinessLocked(response))
                return restart ? EnsureResult::Restarted : EnsureResult::Launched;
            if (std::chrono::steady_clock::now() >= deadline)
                return EnsureResult::NotReady; // Owned child left for later probes.
            if (!WaitSlice(canceled)) return EnsureResult::Canceled;
        }
    }
    return EnsureResult::LaunchFailed;
}

OwnedAdbServer::EnsureResult OwnedAdbServer::EnsureRunning(const std::atomic<bool>* canceled,
                                                        bool allowLifecycleEffects,
                                                        std::function<bool()> stillWanted)
{
    std::unique_lock lock(mutex_, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(20))) {
        if (Canceled(canceled)) return EnsureResult::Canceled;
    }
    const WantedScope scope(this, std::move(stillWanted));
    if (stopped_) return EnsureResult::Stopped;
    if (Halted(canceled)) return EnsureResult::Canceled;
    ApplyBudgetResetLocked();
    CountUnansweredLocked();
    if (child_ && child_->IsRunning() && port_) {
        const auto response = probe_(*port_, canceled);
        if (Halted(canceled)) return EnsureResult::Canceled;
        if (ObserveChildReadinessLocked(response)) return EnsureResult::Running;
        if (!allowLifecycleEffects) return EnsureResult::NotReady;
        // Give a child that never proved readiness its normal start grace.
        if (!answered_ && std::chrono::steady_clock::now() - launchedAt_ < policy_.startGrace)
            return EnsureResult::NotReady;
        // Confirm with further consecutive non-responsive probes, spaced
        // apart; one slow answer never restarts a server other work may
        // depend on.
        bool confirmed = true;
        for (int probe = 1; probe < std::max(1, policy_.unresponsiveProbesToConfirm); ++probe) {
            if (!WaitFor(policy_.confirmationSpacing, canceled)) return EnsureResult::Canceled;
            if (!child_->IsRunning()) { confirmed = false; break; }
            const auto confirmedResponse = probe_(*port_, canceled);
            if (Halted(canceled)) return EnsureResult::Canceled;
            if (ObserveChildReadinessLocked(confirmedResponse)) return EnsureResult::Running;
        }
        if (confirmed && child_->IsRunning()) {
            if (Halted(canceled)) return EnsureResult::Canceled;
            const bool live = hasLiveSessions_ && hasLiveSessions_();
            if (Halted(canceled)) return EnsureResult::Canceled;
            if (live) return EnsureResult::RestartDeferredLiveSession;
            if (std::chrono::steady_clock::now() < nextRestart_)
                return EnsureResult::Throttled;
            if (Halted(canceled)) return EnsureResult::Canceled;
            child_->Stop();
            child_.reset();
            port_.reset();
            observation_.store(nullptr);
            ++restarts_;
            return LaunchAndAwaitLocked(canceled, true);
        }
    }
    if (!allowLifecycleEffects) return EnsureResult::NotReady;
    if (child_) {
        // Unexpected child exit: relaunch only outside the restart budget so
        // a crash-looping child is retried at cooldown pace, not per cycle.
        child_.reset();
        port_.reset();
        observation_.store(nullptr);
    }
    if (Halted(canceled)) return EnsureResult::Canceled;
    const bool live = hasLiveSessions_ && hasLiveSessions_();
    if (Halted(canceled)) return EnsureResult::Canceled;
    if (live) return EnsureResult::RestartDeferredLiveSession;
    // The budget survives a null/failed or already-reaped child, so every
    // subsequent discovery call observes the same crash-loop cooldown.
    if (std::chrono::steady_clock::now() < nextRestart_)
        return EnsureResult::Throttled;
    return LaunchAndAwaitLocked(canceled, launches_ > 0);
}

OwnedAdbServer::EnsureResult OwnedAdbServer::Restart(const std::atomic<bool>* canceled,
                                                   std::function<bool()> stillWanted)
{
    std::unique_lock lock(mutex_, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(20))) {
        if (Canceled(canceled)) return EnsureResult::Canceled;
    }
    const WantedScope scope(this, std::move(stillWanted));
    if (stopped_) return EnsureResult::Stopped;
    if (Halted(canceled)) return EnsureResult::Canceled;
    ApplyBudgetResetLocked();
    if (hasLiveSessions_ && hasLiveSessions_()) return EnsureResult::RestartDeferredLiveSession;
    if (Halted(canceled)) return EnsureResult::Canceled;
    // A deliberate blind restart is paced by the link doctor's own budget
    // (once after the grace, then 15 s, then 60 s); the crash-loop cooldown
    // only guards automatic relaunches of an exiting child.
    if (child_) child_->Stop();
    child_.reset();
    port_.reset();
    observation_.store(nullptr);
    ++restarts_;
    return LaunchAndAwaitLocked(canceled, true);
}

void OwnedAdbServer::CountUnansweredLocked()
{
    // A child that never proved readiness: one count per start grace
    // it stays alive, or one when it dies.
    if (!child_ || answered_) return;
    if (!child_->IsRunning()) {
        if (!deathCounted_) { deathCounted_ = true; ++unansweredStarts_; }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - unansweredCountedAt_ >= policy_.startGrace) {
        unansweredCountedAt_ = now;
        ++unansweredStarts_;
    }
}

void OwnedAdbServer::ResetBudgets()
{
    // Never takes mutex_: a worker may hold it for a whole launch.
    budgetResetRequested_.store(true);
}

void OwnedAdbServer::ApplyBudgetResetLocked()
{
    if (!budgetResetRequested_.exchange(false)) return;
    nextRestart_ = {};
    launchesSinceReset_ = 0;
}

void OwnedAdbServer::Stop()
{
    stopRequested_.store(true); // Interrupt readiness/confirmation before waiting for its lock.
    observation_.store(nullptr);
    std::unique_ptr<OwnedChildProcess> child;
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        child = std::move(child_);
        port_.reset();
    }
    if (child) child->Stop();
}

bool OwnedAdbServer::OwnsRunningServer() const
{
    return Port().has_value();
}

std::optional<std::uint16_t> OwnedAdbServer::Port() const
{
    if (stopRequested_.load()) return std::nullopt;
    const auto snapshot = observation_.load();
    return snapshot && WaitForSingleObject(snapshot->process, 0) == WAIT_TIMEOUT
        ? std::optional<std::uint16_t>(snapshot->port) : std::nullopt;
}

std::optional<AdbListenerIdentity> OwnedAdbServer::ListeningIdentity(std::uint16_t port) const
{
    const auto snapshot = observation_.load();
    if (stopRequested_.load() || !snapshot || snapshot->port != port) return std::nullopt;
    const DWORD pid = GetProcessId(snapshot->process);
    if (!pid || !ProcessOwnsListeningPort(snapshot->process, pid, port)) return std::nullopt;
    const auto identity = ReadAdbListenerIdentity(port, pid, snapshot->process);
    if (stopRequested_.load() || observation_.load() != snapshot) return std::nullopt;
    return identity;
}

int OwnedAdbServer::LaunchCount() const { std::lock_guard lock(mutex_); return launches_; }
int OwnedAdbServer::RestartCount() const { std::lock_guard lock(mutex_); return restarts_; }

} // namespace okuflow
#endif
