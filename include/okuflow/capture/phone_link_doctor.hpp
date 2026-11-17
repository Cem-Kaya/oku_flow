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
    // The default lists the phone in state device, but adbd timed out for
    // it there earlier in this episode (same listener): that listing does
    // not serve the phone, so it is no reason to hand USB back.
    bool defaultPhoneStalled{false};
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
    // Per episode: stops decided but not carried out (a termination aborted
    // because the default answered its pre-stop listing, as a slow server
    // whose version probes hang does; a host:kill request that was never
    // sent). They spend neither terminateCap nor askExitCap, but this many
    // of them end in the other-program sentence (the user can close the
    // other program; probing goes on at the user-wait cadence, so a server
    // that recovers or goes away ends it). The same count as terminateCap:
    // an aborted stop ends where a carried-out one does. Each rung keeps its
    // own count against this cap: the hung branch (a borrowed default whose
    // version probes fail) counts its aborted terminations, the takeover
    // branch (OkuFlow's server beside a blind default) counts its unsent asks
    // and its aborted terminations, so the end of one never ends the other.
    int abortedStopCap{3};
    // Per episode: starts of the private server beside the default plus
    // hand-backs to it (the transport's own start beside a hung default and
    // its own hand-back count too). At the cap no more private servers are
    // started beside the default, while a hand-back to a default that lists
    // the phone is still made: the count ends where the phone is listed. A
    // default that alternately lists and loses the phone ends in the
    // other-program sentence instead of a swap loop.
    int handOverCap{4};
    // A phone listed offline, connecting or stalled (or with no
    // permissions) by a server OkuFlow does not own: this many targeted
    // reconnects, then the default is "holding the interface without
    // serving it" (takeover rungs) and a user endpoint gets the re-plug
    // sentence.
    int foreignReconnects{2};
    // Windows cannot say whether the ADB interface is there (Unknown: the
    // SetupAPI query itself failed; a phone without the standard ADB
    // interface GUID reads Absent). After this long blind, OkuFlow may
    // start or restart its own server as if the interface were present
    // (restart rung and its cap; a private server beside a blind default).
    // A server OkuFlow did not start is never asked to exit or stopped
    // under Unknown: a takeover or a hung default that this would need ends
    // in the re-plug sentence (beside: after the takeover grace; hung:
    // after this long unresponsive).
    std::int64_t unknownInterfaceBlindMs{30000};
    // An endpoint the user set through the adb environment variables is
    // never taken over or stopped. Unresponsive this long, it ends in the
    // re-plug sentence (as a hung default does under Unknown); responsive
    // but not listing the phone, it does so after the blind grace.
    std::int64_t userEndpointUnresponsiveMs{30000};
    // A companion problem other than paused/busy host/blocked/hot (camera
    // busy in another phone app, failed, no mode, unknown) reads "Restoring"
    // this long, then the user is told what to do on the phone.
    std::int64_t companionFailedGraceMs{20000};
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
    // Consecutive launch failures of OkuFlow's own adb before it is named
    // (also a start that stays alive and never answers past its start grace,
    // or dies before it ever answered; also while another program's adb
    // answers on the default port and a private server is needed beside it).
    int adbLaunchFailuresToName{2};
    // Event storms. One wake raises several resume signals (two power
    // broadcasts, display on, the wall-clock jump): resumes within this long
    // of the last applied resume are one event, not a renewal of every cap.
    std::int64_t resumeCoalesceMs{5000};
    // A cable or hub that drops and returns more often than the blind grace
    // never lets a grace pass: this many removal events within flapWindowMs
    // name the cable (re-plug sentence) until the link holds
    // (connectedHoldMs) or no removal came for flapQuietMs. A manual pick or
    // a resume starts a fresh attempt. An ordinary re-plug is one removal
    // (two when the phone also switches its USB mode).
    int flapRemovals{4};
    std::int64_t flapWindowMs{60000};
    std::int64_t flapQuietMs{30000};
    // Rung 5: at once, then these, then every launchPeriodicMs.
    std::array<std::int64_t, 2> launchRetryMs{3000, 6000};
    std::int64_t launchPeriodicMs{20000};
    // After OkuFlow's own wake + launch the app process runs for 1 to 3 s
    // before its listener opens: a refusal with the process running within
    // this window is a launch still starting, not a closed listener, and
    // does not hold the launch budget (the ordinary spacing applies).
    std::int64_t launchSettleMs{5000};
};

struct PhoneLinkDecision {
    PhoneLinkDiagnosis diagnosis{PhoneLinkDiagnosis::NotWanted};
    PhoneLinkAction action{PhoneLinkAction::None};
    // TerminateDefaultServer only: host:kill was already sent to this
    // listener in this episode, so termination does not send it again.
    bool hostKillAlreadySent{false};
    // Absolute monotonic time for the next discovery cycle.
    std::int64_t nextPollMs{0};
    // The episode (events so far) the decision belongs to: an outcome of its
    // action noted after a newer event is dropped.
    std::uint64_t episode{0};
    bool diagnosisChanged{false};
};

class PhoneLinkDoctor {
public:
    explicit PhoneLinkDoctor(PhoneLinkPolicy policy = {}) : policy_(policy) {}
    // Every event resets all budgets and makes the next cycle immediate.
    // False when the event was coalesced into the previous one (a resume
    // within resumeCoalesceMs of the last applied resume): nothing reset.
    bool OnEvent(PhoneLinkEvent event, std::int64_t nowMs);
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
    // The same, tagged with the decision's episode: an outcome that arrives
    // after a newer event belongs to the old episode and is dropped. True
    // when it was recorded.
    bool NoteDefaultStopRefused(std::uint64_t episode)
    {
        if (episode != episode_) return false;
        NoteDefaultStopRefused();
        return true;
    }
    bool NoteDefaultStopFailed(std::uint64_t episode)
    {
        if (episode != episode_) return false;
        NoteDefaultStopFailed();
        return true;
    }
    // The transport started a private server beside a hung default, or
    // retired its private server for the default (the ladder's
    // HandBackToDefault once executed, or its own hand-back of a hung-default
    // shadow, now that the default lists the phone): each counts toward the
    // per-episode hand-over cap like the ladder's own starts, and only once
    // it actually happened. HandOverAllowed gates only new starts beside the
    // default; a hand-back is always allowed.
    bool HandOverAllowed() const { return handOvers_ < policy_.handOverCap; }
    void NoteHandOver() { ++handOvers_; }
    // The same, tagged with the episode the start or hand-back belongs to:
    // noted after a newer event, it is dropped. True when it was recorded.
    bool NoteHandOver(std::uint64_t episode)
    {
        if (episode != episode_) return false;
        NoteHandOver();
        return true;
    }
    // A TerminateDefaultServer of this episode was not carried out because
    // the default answered its pre-stop listing (also with an adb FAIL
    // reply): nothing was stopped, so it does not spend the termination
    // cap. The retry spacing stays as reserved. It counts toward
    // abortedStopCap instead, whose end is the other-program sentence, in
    // the count of the rung that decided it: takeover is the decision's
    // hostKillAlreadySent (only the takeover branch terminates after an
    // ask), otherwise the hung branch.
    void NoteTerminationAborted(std::uint64_t episode, bool takeover)
    {
        if (episode != episode_) return;
        if (terminate_.attempts > 0) --terminate_.attempts;
        ++(takeover ? abortedTakeoverStops_ : abortedHungStops_);
    }
    // An AskDefaultServerToExit of this episode sent no host:kill (the
    // pre-ask checks kept the server, or the request was never delivered):
    // it is not remembered as asked, so no termination follows as if the
    // request had been ignored, and it does not spend askExitCap. The retry
    // spacing stays as reserved; it counts toward abortedStopCap (takeover).
    void NoteExitRequestNotSent(std::uint64_t episode)
    {
        if (episode != episode_) return;
        GiveExitRequestBack();
        ++abortedTakeoverStops_;
    }
    // An AskDefaultServerToExit of this episode found the default already
    // gone: nothing was asked, so the ask is given back and not remembered
    // as sent, and nothing was kept, so the first one of an episode does not
    // count toward abortedStopCap. A default that is "gone" at one ask and
    // answers again at the next is a listener the lookup cannot identify
    // (for example an adb bound to every address instead of loopback): it
    // would be asked for ever with nothing shown, so every further one of
    // the episode counts and the rung ends in the other-program sentence.
    void NoteExitRequestNotNeeded(std::uint64_t episode)
    {
        if (episode != episode_) return;
        GiveExitRequestBack();
        if (exitRequestNotNeededUsed_) ++abortedTakeoverStops_;
        exitRequestNotNeededUsed_ = true;
    }
    std::uint64_t Episode() const { return episode_; }
    // The listener is gone, changed identity, or the phone is listed.
    void ClearDefaultStopRefused() { defaultStopRefused_ = false; stopFailures_ = 0; }
    // A different process now listens on the default port beside OkuFlow's
    // server: a new takeover (grace, then asked first, never terminated
    // because an earlier listener was asked). Counts toward the caps.
    void NoteDefaultListenerChanged() { askedExitMs_ = -1; shadowSinceMs_ = -1; }
    bool DefaultStopRefused() const { return defaultStopRefused_; }
    // The one per-event companion launch budget, shared by the discovery
    // ladder and the session start path (wake + launch). Refused, until the
    // next event, once the episode named a state where more launches only
    // pull the app over what the user does on the phone (its process runs
    // but its listener refuses; it accepts but never answers).
    bool ReserveLaunch(std::int64_t nowMs);
    // A deliberate pick of the phone renews the launch budget.
    void RenewLaunches() { launch_ = Budget{}; launchHeld_ = false; }
    // The phone has been missing from every listing for the blind grace
    // (as of the last Decide), so a leftover may plausibly hold it.
    bool BlindGracePassed(std::int64_t nowMs) const
    {
        return blindSinceMs_ >= 0 && nowMs - SinceEventOr(blindSinceMs_) >= policy_.blindGraceMs;
    }
    PhoneLinkDiagnosis LastDiagnosis() const { return last_; }
    std::int64_t LastEventMs() const { return eventAtMs_; }
    std::int64_t NextPollMs() const { return nextPollMs_; }
    const PhoneLinkPolicy& Policy() const { return policy_; }
private:
    struct Budget {
        // Attempts that count toward a cap (a give-back lowers it).
        int attempts{0};
        // Reservations since the last reset: the retry spacing grows with
        // these, so attempts that were given back still reach the ceiling.
        int reserved{0};
        std::int64_t nextAllowedMs{0};
        bool Allowed(std::int64_t nowMs) const { return nowMs >= nextAllowedMs; }
    };
    std::int64_t SinceEventOr(std::int64_t firstSeenMs) const;
    PhoneLinkAction LadderFor(PhoneLinkDiagnosis diagnosis, const PhoneLinkObservation& observation,
                              std::int64_t nowMs, PhoneLinkDiagnosis* raised);
    void Reserve(Budget& budget, std::int64_t nowMs, std::int64_t retryMs, std::int64_t ceilingMs);
    void TakeLaunch(std::int64_t nowMs);
    PhoneLinkPolicy policy_;
    PhoneLinkDiagnosis last_{PhoneLinkDiagnosis::NotWanted};
    bool haveLast_{false};
    std::int64_t eventAtMs_{0};
    std::int64_t nextPollMs_{0};
    unsigned failedCycles_{0};
    // Episode markers (reset by events and by Connected).
    std::int64_t blindSinceMs_{-1};
    std::int64_t unresponsiveSinceMs_{-1};
    std::int64_t stuckSinceMs_{-1};
    std::int64_t lastReconnectMs_{-1};
    std::int64_t shadowSinceMs_{-1};
    std::int64_t askedExitMs_{-1};
    std::int64_t unauthorizedSinceMs_{-1};
    std::int64_t authorizingSinceMs_{-1};
    std::int64_t connectedSinceMs_{-1};
    bool unauthorizedReconnected_{false};
    // Whose reconnects reconnect_ counts: OkuFlow's own server or another.
    bool reconnectOwned_{false};
    bool hostKillSent_{false};
    bool defaultStopRefused_{false};
    int stopFailures_{0};
    int handOvers_{0};
    // Aborted stops per rung (abortedStopCap): the hung branch's aborted
    // terminations; the takeover branch's unsent asks and aborted
    // terminations.
    int abortedHungStops_{0};
    int abortedTakeoverStops_{0};
    // The one uncounted "default already gone" ask of this episode was used.
    bool exitRequestNotNeededUsed_{false};
    void GiveExitRequestBack()
    {
        askedExitMs_ = -1;
        if (askExit_.attempts > 0) --askExit_.attempts;
    }
    bool launchHeld_{false};
    // A hold was already released at once because the app died (episode).
    bool holdReleasedByDeath_{false};
    std::int64_t lastLaunchMs_{-1};
    std::uint64_t episode_{0};
    std::int64_t failedSinceMs_{-1};
    // Event storms: the last applied resume, recent removal times (bounded)
    // and the flapping verdict (ended by a held link, a quiet period, a
    // manual pick, a resume or the start).
    std::int64_t lastResumeMs_{-1};
    std::array<std::int64_t, 16> removals_{};
    int removalCount_{0};
    std::int64_t lastRemovalMs_{-1};
    bool flapping_{false};
    Budget reconnect_, restart_, shadow_, askExit_, terminate_, launch_, handback_;
};

} // namespace okuflow
