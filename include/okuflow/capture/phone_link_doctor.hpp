#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace okuflow {

// Pure, model-free decision unit for the USB phone link. It owns no clock,
// socket, process or UI: callers pass one observation of the whole chain
// (Windows ADB interface, adb server, raw device state, adbd, companion,
// read-only phone facts) plus a monotonic time, and receive one diagnosis
// and the single next recovery action. Budgets are reset by events
// (arrival, removal, resume, start, manual pick, activation after idle),
// not only by time, so a fresh event is always tried at once and repeated
// failure without an event backs off to a bounded ceiling.

enum class PhoneAdbInterface { Unknown, Present, Absent };
enum class PhoneServerKind { None, Owned, BorrowedDefault, Override };
enum class PhoneServerProbe { Unknown, Responsive, Unresponsive, NoListener };
// adb devices.proto ConnectionState, reduced to what the ladder needs.
enum class PhoneAdbState { NotListed, Device, Unauthorized, Authorizing, Connecting, Offline, NoPermissions, Other };
enum class PhoneAdbdHealth { Unknown, Answers, TimedOut };
enum class PhoneCompanionProbe { Unknown, Ready, SessionBusy, Problem, Refused, TimedOut };
enum class PhoneFact { Unknown, No, Yes };

struct PhoneShellFacts {
    PhoneFact screenAwake{PhoneFact::Unknown};
    PhoneFact keyguardShowing{PhoneFact::Unknown};
    PhoneFact appInstalled{PhoneFact::Unknown};
    PhoneFact appRunning{PhoneFact::Unknown};
    bool operator==(const PhoneShellFacts&) const = default;
};

struct PhoneLinkObservation {
    bool phoneWanted{false};
    bool endpointSupported{true};
    // MediaCapture refused further starts in this process (stop missed its deadline).
    bool captureAbandoned{false};
    // Another OkuFlow instance of this installation runs for this user/session
    // with a visible window (the user can close it).
    bool otherInstanceRunning{false};
    // Such an instance without any visible window: a leftover the user cannot
    // close from the taskbar (for example one that is stuck while exiting).
    bool leftoverInstanceRunning{false};
    // A live phone session in this process is delivering frames.
    bool sessionStreaming{false};
    PhoneAdbInterface adbInterface{PhoneAdbInterface::Unknown};
    PhoneServerKind server{PhoneServerKind::None};
    // OkuFlow's private server runs beside a default-port listener of another
    // adb (one OkuFlow started as a shadow, or one that appeared later).
    bool shadowRunning{false};
    PhoneServerProbe serverProbe{PhoneServerProbe::Unknown};
    // Facts about the default-port listener while it is borrowed or shadowed.
    bool defaultListsPhone{false};
    bool defaultHasOtherDevices{false};
    bool defaultUnresponsiveConfirmed{false};
    // Windows refused to stop the default listener (elevated/other user),
    // or it cannot be stopped at all (unreadable identity, not adb.exe).
    bool defaultStopRefused{false};
    PhoneAdbState phoneState{PhoneAdbState::NotListed};
    // The server lists a USB device, but not the wanted phone: Windows shows
    // an ADB interface because of that other device.
    bool otherUsbListed{false};
    // Consecutive failed launches of OkuFlow's own adb (missing file,
    // antivirus quarantine); reset by a running server.
    int serverLaunchFailures{0};
    PhoneAdbdHealth adbd{PhoneAdbdHealth::Unknown};
    PhoneCompanionProbe companion{PhoneCompanionProbe::Unknown};
    std::string problem; // Companion problem code, when companion == Problem.
    PhoneShellFacts shell;
};

enum class PhoneLinkDiagnosis {
    Connected,               // Ready, or a live session is using the phone.
    NotWanted,               // No phone-use intent: discovery only.
    EndpointUnsupported,     // ADB_SERVER_SOCKET etc. name an unsupported endpoint.
    RestartRequired,         // Capture was abandoned in this process.
    ServerStarting,          // Owned server launching or recovering.
    ServerUnresponsive,      // A listener exists but does not answer.
    NoAdbInterface,          // Windows shows no ADB interface: cable, charge-only, debugging off.
    NotListedByServer,       // Interface present, but the server lists no USB phone.
    Unauthorized,            // Listed unauthorized: the phone shows the Allow question.
    Authorizing,             // Listed authorizing/connecting.
    Offline,                 // Listed offline.
    TransportStalled,        // Listed, but adbd does not answer.
    PhoneLocked,             // adbd answers; the phone is asleep or locked.
    AppNotRunning,           // adbd answers, phone awake, companion listener absent.
    AppNotInstalled,
    CompanionPaused,
    CompanionBusyOtherHost,
    CompanionBlocked,
    CompanionHot,
    CompanionFailed,         // busy camera, failed, noMode or an unknown code.
    OtherInstanceHoldsPhone,
    OtherProgramHoldsPhone,
    ReplugCable,             // Restarts did not bring a present interface back: unplug and re-plug.
    CompanionNotResponding,  // The companion's listener accepts but never answers, after launches.
    CompanionListenerClosed, // The app process runs but its listener refuses (paused old companion).
    AdbCannotStart,          // OkuFlow's own adb.exe could not be launched repeatedly.
    WantedPhoneNotAttached,  // The server lists another USB phone, not the wanted one.
};
inline constexpr int kPhoneLinkDiagnosisCount = 27;

enum class PhoneLinkAction {
    None,
    Relist,                  // 1: list again with loaded-laptop budgets.
    ReconnectTransport,      // 2: host-transport-id:<id>:reconnect (also offline).
    RestartOwnedServer,      // 3: restart OkuFlow's own private server.
    StartShadowServer,       // 4a: private owned server beside a blind default.
    AskDefaultServerToExit,  // 4b: host:kill to the default-port server.
    TerminateDefaultServer,  // 4c: exact-handle termination of a hung default adb.exe.
    WakeAndLaunchApp,        // 5: KEYCODE_WAKEUP + am start (never force-stop).
    WaitForUser,             // 6: one sentence; keep probing.
    HandBackToDefault,       // 4d: the default-port server lists the phone: use it, retire the private one.
};

enum class PhoneLinkEvent { AppStart, DeviceArrival, DeviceRemoval, Resume, ManualPick, WindowActivatedAfterIdle };

std::string_view PhoneLinkDiagnosisName(PhoneLinkDiagnosis diagnosis);
std::string_view PhoneLinkActionName(PhoneLinkAction action);
std::string_view PhoneLinkEventName(PhoneLinkEvent event);

// Stateless classification of one observation.
PhoneLinkDiagnosis DiagnosePhoneLink(const PhoneLinkObservation& observation);
// True when only the user can resolve the diagnosis; such states poll at a
// constant cadence because the user's action produces no Windows event.
bool PhoneLinkWaitsForUser(PhoneLinkDiagnosis diagnosis);

struct PhoneLinkPolicy {
    // Discovery cadence after consecutive failed cycles since the last event.
    std::array<std::int64_t, 9> pollBackoffMs{1000, 1000, 1000, 1000, 1000, 2000, 3000, 5000, 10000};
    std::int64_t userWaitPollMs{2000};
    std::int64_t healthyPollMs{5000};
    std::int64_t idlePollMs{5000};
    // Rung 2: first at once, second after this, then every reconnectCeilingMs.
    std::int64_t reconnectRetryMs{3000};
    std::int64_t reconnectCeilingMs{30000};
    // Rung 3: blind/stuck grace after the last event or first observation.
    std::int64_t blindGraceMs{5000};
    std::int64_t stuckAfterReconnectMs{6000};
    std::int64_t restartRetryMs{15000};
    std::int64_t restartCeilingMs{60000};
    // Rung 4.
    std::int64_t takeoverGraceMs{6000};
    std::int64_t hostKillWaitMs{2000};
    std::int64_t takeoverCeilingMs{60000};
    // Per episode (until the next event): blind restarts of the private
    // server, host:kill requests and terminations of a default-port adb.
    // After the cap the state is named to the user instead of fighting.
    int blindRestartCap{3};
    int askExitCap{2};
    int terminateCap{3};
    // Per episode: starts of the private server beside the default plus
    // hand-backs to it. A default that alternately lists and loses the
    // phone ends in the other-program sentence instead of a swap loop.
    int handOverCap{4};
    // A phone listed offline, connecting or stalled (or with no
    // permissions) by a server OkuFlow does not own: this many targeted
    // reconnects, then the default is "holding the interface without
    // serving it" (takeover rungs) and a user endpoint gets the re-plug
    // sentence.
    int foreignReconnects{2};
    // Unauthorized for this long with no change: one targeted reconnect,
    // which makes the phone ask the Allow question again.
    std::int64_t unauthorizedReconnectMs{20000};
    // Authorizing (the Allow handshake) reads "Restoring" this long first.
    std::int64_t authorizingGraceMs{2000};
    // Connected must hold this long before every budget is renewed, so a
    // companion that flips between ready and refused is not relaunched at
    // once on every flip. A live streaming session renews at once.
    std::int64_t connectedHoldMs{10000};
    // A listener that accepts but never answers after this many launches is
    // named ("close and reopen OkuFlow Camera") instead of relaunched.
    int hangingCompanionLaunches{2};
    // Consecutive launch failures of OkuFlow's own adb before it is named.
    int adbLaunchFailuresToName{2};
    // Rung 5: at once, then these, then every launchPeriodicMs.
    std::array<std::int64_t, 2> launchRetryMs{3000, 6000};
    std::int64_t launchPeriodicMs{20000};
};

struct PhoneLinkDecision {
    PhoneLinkDiagnosis diagnosis{PhoneLinkDiagnosis::NotWanted};
    PhoneLinkAction action{PhoneLinkAction::None};
    // TerminateDefaultServer only: host:kill was already sent to this
    // listener in this episode, so termination does not send it again.
    bool hostKillAlreadySent{false};
    // Absolute monotonic time for the next discovery cycle.
    std::int64_t nextPollMs{0};
    bool diagnosisChanged{false};
};

class PhoneLinkDoctor {
public:
    explicit PhoneLinkDoctor(PhoneLinkPolicy policy = {}) : policy_(policy) {}
    // Every event resets all budgets and makes the next cycle immediate.
    void OnEvent(PhoneLinkEvent event, std::int64_t nowMs);
    // Classifies, chooses the next action and reserves its budget. The
    // caller performs the returned action once (or not at all if it is
    // canceled); the reservation stands either way.
    PhoneLinkDecision Decide(const PhoneLinkObservation& observation, std::int64_t nowMs);
    // The default server stop was attempted but Windows refused it, or the
    // listener cannot be stopped at all (unreadable identity, not adb.exe).
    void NoteDefaultStopRefused() { defaultStopRefused_ = true; }
    // A stop that ended neither stopped nor refused (identity changed,
    // failed): the second such outcome in an episode counts as refused.
    void NoteDefaultStopFailed() { if (++stopFailures_ >= 2) defaultStopRefused_ = true; }
    // The listener is gone, changed identity, or the phone is listed.
    void ClearDefaultStopRefused() { defaultStopRefused_ = false; stopFailures_ = 0; }
    // A different process now listens on the default port beside OkuFlow's
    // server: a new takeover (grace, then asked first, never terminated
    // because an earlier listener was asked). Counts toward the caps.
    void NoteDefaultListenerChanged() { askedExitMs_ = -1; shadowSinceMs_ = -1; }
    bool DefaultStopRefused() const { return defaultStopRefused_; }
    PhoneLinkDiagnosis LastDiagnosis() const { return last_; }
    std::int64_t LastEventMs() const { return eventAtMs_; }
    std::int64_t NextPollMs() const { return nextPollMs_; }
    const PhoneLinkPolicy& Policy() const { return policy_; }
private:
    struct Budget {
        int attempts{0};
        std::int64_t nextAllowedMs{0};
        bool Allowed(std::int64_t nowMs) const { return nowMs >= nextAllowedMs; }
    };
    std::int64_t SinceEventOr(std::int64_t firstSeenMs) const;
    PhoneLinkAction LadderFor(PhoneLinkDiagnosis diagnosis, const PhoneLinkObservation& observation,
                              std::int64_t nowMs, PhoneLinkDiagnosis* raised);
    void Reserve(Budget& budget, std::int64_t nowMs, std::int64_t retryMs, std::int64_t ceilingMs);
    PhoneLinkPolicy policy_;
    PhoneLinkDiagnosis last_{PhoneLinkDiagnosis::NotWanted};
    bool haveLast_{false};
    std::int64_t eventAtMs_{0};
    std::int64_t nextPollMs_{0};
    unsigned failedCycles_{0};
    // Episode markers (reset by events and by Connected).
    std::int64_t blindSinceMs_{-1};
    std::int64_t stuckSinceMs_{-1};
    std::int64_t lastReconnectMs_{-1};
    std::int64_t shadowSinceMs_{-1};
    std::int64_t askedExitMs_{-1};
    std::int64_t unauthorizedSinceMs_{-1};
    std::int64_t authorizingSinceMs_{-1};
    std::int64_t connectedSinceMs_{-1};
    bool unauthorizedReconnected_{false};
    bool hostKillSent_{false};
    bool defaultStopRefused_{false};
    int stopFailures_{0};
    int handOvers_{0};
    Budget reconnect_, restart_, shadow_, askExit_, terminate_, launch_, handback_;
};

} // namespace okuflow
