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
#include <string>
#include <vector>

namespace okuflow {

// Deliberate adoption and reclamation of one detached legacy adb server.
//
// Old releases started the bundled adb server detached, so it can outlive
// the application that started it. TryReclaimLegacyBundledAdbServer stops
// such a server only after it re-proves, against the exact process handle
// behind the loopback listener, that the process image is the reviewed
// source-built adb shipped inside a recognizable OkuFlow bundle (pinned
// adb-build.json manifest hash, matching adb.exe entry hash, launcher
// VERSIONINFO ProductName or an allowlisted exact launcher SHA-256, flat
// or nested bundle layout). This is a
// deliberate adoption of a reviewed OkuFlow-bundled legacy process based on
// current file/process provenance, not historical kernel proof of which
// application originally spawned it. Every unknown fails closed here. Other
// hung or blind default-port adb servers are handled by the separate
// TryStopDefaultAdbServer rule below (owner decision of 2026-11-17); no
// process is ever signalled by name or tracked by a persisted PID.
//
// The caller retains the policy preconditions: it must have externally
// confirmed an unresponsive server (at least two failed host-version probes
// spanning at least five seconds), hold phone-use intent, and have no
// live/starting phone session. The helper independently revalidates the
// listener identity and provenance, refuses while a sibling OkuFlow
// instance runs, serializes reclaimers with a per-user/session named
// mutex, tries one bounded raw host:kill on the classified endpoint, and
// only then terminates through the exact held handle. Callers must never
// invoke it for an OKUFLOW_ADB_PATH executable or an overridden adb
// endpoint: those servers are borrowed and outside this contract.
//
// Blocking file hashing, process waits and one bounded TCP exchange make
// the entry point suitable for a worker thread only, never a UI thread.

inline constexpr std::uint16_t kLegacyAdbDefaultPort = 5037;

// SHA-256 of the reviewed platform-tools/adb-build.json, pinned identically
// in scripts/adb-source/distribution.json. A new reviewed adb build must
// update both pins together; the value is a production constant, never read
// from the environment.
inline constexpr char kReviewedAdbBuildManifestSha256[] =
    "f91532bab5d146a1dd0dc646629afb8ce77a626c0cde0b015e71c8dfcbb00b1c";
// Windows VERSIONINFO ProductName the bundle launcher (oku_flow.exe) must
// carry for its bundle to count as an OkuFlow distribution.
inline constexpr wchar_t kExpectedOkuFlowLauncherProductName[] = L"OkuFlow";
// Metadata-free v0.2.3 launcher extracted from the public ZIP after its SHA-256
// matched GitHub's asset digest. Keep provenance in docs/adb_distribution.md.
inline constexpr char kReviewedLegacyLauncherSha256[] =
    "8bff15f54bfe5111762cda157a3df9d9716f549b3c0bf2d9f612100ccc288b1d";

enum class LegacyAdbRecoveryStatus {
    Recovered,        // The classified legacy server process is gone.
    NotManaged,       // No listener, or provenance did not prove our bundle.
    Busy,             // Preconditions unmet, sibling instance, or mutex held.
    IdentityChanged,  // Listener/process/provenance changed mid-sequence.
    Canceled,         // Cancellation or shutdown won over every stop stage.
    Failed            // Classified, but a bounded stop stage itself failed.
};

// Machine-readable reason behind a status, for logs and root fallthrough.
// Diagnostics never carry secrets; paths and PIDs are observation only.
enum class LegacyAdbRecoveryDetail {
    None,
    PreconditionsNotConfirmed,
    MutexBusy,
    MutexUnavailable,
    SessionsBusy,
    NoLoopbackListener,
    AmbiguousListener,
    ListenerProcessUnavailable,
    ImageNotBundledAdb,
    DistributionLayoutUnrecognized,
    DistributionOutsideExpectedRoot,
    DistributionIncomplete,
    ManifestHashMismatch,
    ManifestInvalid,
    AdbExecutableHashMismatch,
    LauncherProductMismatch,
    SiblingOkuFlowRunning,
    ListenerChanged,
    ProcessChangedOrExited,
    ProvenanceChanged,
    HostKillNotDelivered,
    TerminationFailed,
    ExitWaitTimedOut,
    ListenerQueryFailed,
    InternalError
};

// Provenance expectations. Production keeps every default; the overrides
// exist solely so private fixture tests can pin a synthetic temp
// distribution. Nothing here is read from ambient environment state, and a
// non-empty requiredDistributionRoot only ever narrows acceptance.
struct LegacyAdbProvenanceExpectations {
    std::string reviewedManifestSha256{kReviewedAdbBuildManifestSha256};
    std::wstring expectedLauncherProductName{kExpectedOkuFlowLauncherProductName};
    // Exact SHA-256 allowlist for reviewed legacy launchers that carry no
    // VERSIONINFO ProductName. A launcher is accepted when its ProductName
    // matches exactly OR its file hash matches one entry here exactly;
    // path or name alone never suffices. An empty injected allowlist means
    // ProductName proof only. Production pins v0.2.3; every entry is 64 hex.
    std::vector<std::string> reviewedLauncherSha256{kReviewedLegacyLauncherSha256};
    // When non-empty, the canonical bundle payload root must equal or live
    // under this directory. Production leaves it empty: a reviewed bundle
    // may sit at any path, including an older release location.
    std::wstring requiredDistributionRoot;
};

struct LegacyAdbRecoveryPolicy {
    std::chrono::milliseconds mutexAcquireTimeout{3000};
    std::chrono::milliseconds hostKillConnectTimeout{500};
    // Wait for the exact process to exit after host:kill. Clamped to the
    // two-second contract maximum inside the helper.
    std::chrono::milliseconds hostKillExitWait{2000};
    std::chrono::milliseconds forcedExitWait{2000};
    // Test seam only: explicit named-mutex name. Production derives a
    // per-user/per-session Local\ name instead.
    std::wstring mutexNameOverride;
    // Test seam only: invoked once after successful initial classification,
    // before the pre-stop revalidation. Production leaves it empty.
    std::function<void()> afterInitialClassificationForTest;
    // Test seam only: invoked once after the host:kill TCP connect
    // succeeds, before the mandatory post-connect revalidation and before
    // any byte is sent. Production leaves it empty.
    std::function<void()> afterHostKillConnectForTest;
    // Test seam only: when non-empty, the sibling-instance scan considers
    // only candidate processes whose canonical image lives under this
    // directory, so private fixtures can run beside a real owner instance
    // elsewhere. A candidate whose image cannot be proven still refuses.
    // Production leaves it empty: the scan stays broad and fail-closed.
    std::wstring siblingScanRootForTest;
};

struct LegacyAdbRecoveryRequest {
    std::uint16_t port{kLegacyAdbDefaultPort};
    const std::atomic<bool>* canceled{nullptr};
    // Optional distinct shutdown latch; either flag cancels the attempt.
    const std::atomic<bool>* shutdownRequested{nullptr};
    // Caller-confirmed preconditions. Any false flag refuses with Busy.
    bool externallyConfirmedUnresponsive{false};
    bool phoneUseIntent{false};
    bool noActiveOrStartingPhoneSessions{false};
    // Optional live recheck, consulted before each stop stage in addition
    // to the snapshot flag above. True means a session is busy.
    std::function<bool()> hasActiveOrStartingPhoneSessions;
    LegacyAdbProvenanceExpectations expectations;
    LegacyAdbRecoveryPolicy policy;
};

struct LegacyAdbRecoveryResult {
    LegacyAdbRecoveryStatus status{LegacyAdbRecoveryStatus::Failed};
    LegacyAdbRecoveryDetail detail{LegacyAdbRecoveryDetail::InternalError};
    std::string diagnostic;
    // Exactly one stop mechanism is credited on Recovered.
    bool stoppedViaHostKill{false};
    bool usedForcedTermination{false};
    // True when host:kill was delivered but the process outlived its wait.
    bool hostKillDelivered{false};
    // Observation only: the listener PID seen at classification and the
    // canonical image path proven for it. Never persistence input.
    DWORD listenerProcessId{0};
    std::wstring imagePath;
};

// Attempts the bounded reclamation described above. Recovered means only
// that the old server process is gone; it is not evidence that phone video
// works, which still needs a fresh owned bootstrap, discovery and frames.
LegacyAdbRecoveryResult TryReclaimLegacyBundledAdbServer(
    const LegacyAdbRecoveryRequest& request);

// Owner decision of 2026-11-17 ("kill the hanging server"): a default-port
// adb server that is confirmed hung, or that is responsive but blind to a
// phone Windows shows and ignores host:kill, is stopped. This helper acts
// only on the exact process behind the IPv4 loopback listener of `port`:
// it opens that listener's owning process, requires the same PID and
// creation time as the identity the caller observed before deciding, an
// image file name equal to `requiredImageName` (adb.exe) and a process
// other than this one, sends one raw host:kill when asked, and terminates
// through the held handle only when the process outlives the wait and still
// owns the listener. Never by name, never a tree, never a PID from storage.
// Callers keep the policy preconditions: phone-use intent, no adb endpoint
// override, no other device in state DEVICE on that server, no live session.
struct DefaultAdbStopRequest {
    std::uint16_t port{kLegacyAdbDefaultPort};
    // Identity observed when the decision was made; 0 skips the check.
    DWORD expectedProcessId{0};
    std::uint64_t expectedCreation{0};
    bool sendHostKillFirst{true};
    // False: only ask (host:kill after every identity and image check) and
    // report HostKillIgnored when the process outlives the wait.
    bool terminateIfAlive{true};
    std::wstring requiredImageName{L"adb.exe"};
    const std::atomic<bool>* canceled{nullptr};
    std::chrono::milliseconds hostKillWait{2000};
    std::chrono::milliseconds exitWait{2000};
};
enum class DefaultAdbStopStatus {
    Stopped,          // The exact listener process exited.
    NoListener,       // Nothing listens on the loopback port.
    SelfListener,     // The listener is this process (never stopped).
    NotAdb,           // Image name differs: not stopped.
    IdentityChanged,  // PID/creation/listener changed: not stopped.
    AccessDenied,     // Windows refused termination (elevated or another user).
    HostKillIgnored,  // terminateIfAlive=false: asked, and it is still running.
    Canceled,
    Failed
};
struct DefaultAdbStopResult {
    DefaultAdbStopStatus status{DefaultAdbStopStatus::Failed};
    bool hostKillDelivered{false};
    bool terminated{false};
    DWORD processId{0};
    std::string diagnostic; // No paths; image file name and PID only.
};
DefaultAdbStopResult TryStopDefaultAdbServer(const DefaultAdbStopRequest& request);

// Other running OkuFlow app/launcher processes of this user and session.
// `sameImageOnly` keeps only processes whose image equals this process's
// image (the same installation). Observation only; nothing is stopped.
struct OkuFlowSibling {
    DWORD processId{0};
    std::uint64_t creation{0};
    bool sameImage{false};
    bool hasVisibleWindow{false};
};
std::vector<OkuFlowSibling> FindOkuFlowSiblings(bool sameImageOnly);

// Leftover OkuFlow instances of this installation (iteration 2, A10).
//
// A real instance (a main window on the real Windows platform; offscreen
// and test instances never register) creates two named kernel events in
// its session namespace that carry the installation tag and its PID:
//   Local\OkuFlow.Instance.<tag>.<pid>   exists while the instance lives
//   Local\OkuFlow.Exiting.<tag>.<pid>    set when the instance begins to exit
// The tag is a hash of the canonical image path, so only instances of the
// same installation match. The kernel removes both names with the process.
inline std::wstring OkuFlowInstanceObjectName(bool exiting, const std::wstring& tag, DWORD processId)
{
    return std::wstring(exiting ? L"Local\\OkuFlow.Exiting." : L"Local\\OkuFlow.Instance.") + tag + L"." +
           std::to_wstring(processId);
}
// Tag of this process's installation (hash of its canonical image path).
std::wstring OkuFlowInstallationTag();
std::wstring OkuFlowInstallationTag(const std::wstring& canonicalImagePath);

class OkuFlowInstanceRegistration {
public:
    // Null when the objects cannot be created; nothing else depends on it.
    static std::unique_ptr<OkuFlowInstanceRegistration> Register();
    ~OkuFlowInstanceRegistration();
    OkuFlowInstanceRegistration(const OkuFlowInstanceRegistration&) = delete;
    OkuFlowInstanceRegistration& operator=(const OkuFlowInstanceRegistration&) = delete;
    // The instance begins to exit (idempotent).
    void SignalExiting();
private:
    OkuFlowInstanceRegistration(HANDLE registered, HANDLE exiting) : registered_(registered), exiting_(exiting) {}
    HANDLE registered_{nullptr};
    HANDLE exiting_{nullptr};
};

// One observation of a sibling of the same image, user and session.
struct OkuFlowSiblingObservation {
    DWORD processId{0};
    std::uint64_t creation{0};
    bool registered{false};
    bool exiting{false};
    bool hasVisibleWindow{false};
    // Any top-level window of the process answered WM_NULL within 1 s.
    bool answersWindowMessage{false};
};
// `image` and `tag` default to this process's installation; tests pass a
// fixture image and tag. Observation only: nothing is signalled.
std::vector<OkuFlowSiblingObservation> ObserveOkuFlowSiblings(const std::wstring& image = {},
                                                               const std::wstring& tag = {});

struct OkuFlowSiblingTrack {
    std::int64_t exitingSinceMs{-1};
    std::int64_t windowlessSinceMs{-1};
};
struct OkuFlowSiblingPolicy {
    // The exit signal has been set this long and the process still lives.
    std::int64_t exitingGraceMs{5000};
    // Registered, no visible window on two observations this far apart, and
    // no answer to a window message.
    std::int64_t windowlessConfirmMs{10000};
};
enum class OkuFlowSiblingVerdict {
    Windowed,               // A visible window: the user can close it; never terminated.
    Pending,                // Not yet proven a leftover (or still starting).
    Leftover,               // Proven leftover of this installation: may be terminated.
    UnregisteredWindowless  // No registration (old version, test run): named, never terminated.
};
// Pure: classifies one observation and updates its track.
OkuFlowSiblingVerdict ClassifyOkuFlowSibling(const OkuFlowSiblingObservation& observation,
                                             OkuFlowSiblingTrack& track, std::int64_t nowMs,
                                             const OkuFlowSiblingPolicy& policy = {});

// Terminates the exact proven leftover through a held handle: same PID and
// creation time, canonical image equal to `expectedImage`, not this
// process. Its owned adb child then dies with its job object. If Windows
// refuses, only that leftover's own adb child is stopped, and only when it
// is proven: parent PID equal to the leftover, created after it, canonical
// image equal to `bundledAdbImage` (this installation's platform-tools\adb.exe).
struct LeftoverInstanceStopRequest {
    DWORD processId{0};
    std::uint64_t creation{0};
    std::wstring expectedImage;   // Empty: this process's canonical image.
    std::wstring bundledAdbImage; // Empty: <this image's directory>\platform-tools\adb.exe.
    std::chrono::milliseconds exitWait{2000};
};
enum class LeftoverInstanceStopStatus {
    Stopped,          // The leftover instance exited.
    AdbChildStopped,  // The instance could not be stopped; its exact adb child was.
    Refused,          // Neither could be stopped: the user is told.
    IdentityChanged   // Not the observed process any more: nothing done.
};
LeftoverInstanceStopStatus StopLeftoverOkuFlowInstance(const LeftoverInstanceStopRequest& request);
// The adb-child step alone (also used by tests).
bool StopLeftoverAdbChild(DWORD parentId, std::uint64_t parentCreation, const std::wstring& bundledAdbImage,
                          std::chrono::milliseconds exitWait = std::chrono::milliseconds(2000));

} // namespace okuflow
#endif
