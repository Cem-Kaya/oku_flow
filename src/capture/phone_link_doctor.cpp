#include "okuflow/capture/phone_link_doctor.hpp"

#include <algorithm>

namespace okuflow {

std::string_view PhoneLinkDiagnosisName(PhoneLinkDiagnosis diagnosis)
{
    switch (diagnosis) {
    case PhoneLinkDiagnosis::Connected: return "connected";
    case PhoneLinkDiagnosis::NotWanted: return "not-wanted";
    case PhoneLinkDiagnosis::EndpointUnsupported: return "endpoint-unsupported";
    case PhoneLinkDiagnosis::RestartRequired: return "restart-required";
    case PhoneLinkDiagnosis::ServerStarting: return "server-starting";
    case PhoneLinkDiagnosis::ServerUnresponsive: return "server-unresponsive";
    case PhoneLinkDiagnosis::NoAdbInterface: return "no-adb-interface";
    case PhoneLinkDiagnosis::NotListedByServer: return "not-listed-by-server";
    case PhoneLinkDiagnosis::Unauthorized: return "unauthorized";
    case PhoneLinkDiagnosis::Authorizing: return "authorizing";
    case PhoneLinkDiagnosis::Offline: return "offline";
    case PhoneLinkDiagnosis::TransportStalled: return "transport-stalled";
    case PhoneLinkDiagnosis::PhoneLocked: return "phone-locked";
    case PhoneLinkDiagnosis::AppNotRunning: return "app-not-running";
    case PhoneLinkDiagnosis::AppNotInstalled: return "app-not-installed";
    case PhoneLinkDiagnosis::CompanionPaused: return "companion-paused";
    case PhoneLinkDiagnosis::CompanionBusyOtherHost: return "companion-busy-other-host";
    case PhoneLinkDiagnosis::CompanionBlocked: return "companion-blocked";
    case PhoneLinkDiagnosis::CompanionHot: return "companion-hot";
    case PhoneLinkDiagnosis::CompanionFailed: return "companion-failed";
    case PhoneLinkDiagnosis::OtherInstanceHoldsPhone: return "other-instance-holds-phone";
    case PhoneLinkDiagnosis::OtherProgramHoldsPhone: return "other-program-holds-phone";
    case PhoneLinkDiagnosis::ReplugCable: return "replug-cable";
    case PhoneLinkDiagnosis::CompanionNotResponding: return "companion-not-responding";
    case PhoneLinkDiagnosis::CompanionListenerClosed: return "companion-listener-closed";
    case PhoneLinkDiagnosis::AdbCannotStart: return "adb-cannot-start";
    case PhoneLinkDiagnosis::WantedPhoneNotAttached: return "wanted-phone-not-attached";
    }
    return "unknown";
}

std::string_view PhoneLinkActionName(PhoneLinkAction action)
{
    switch (action) {
    case PhoneLinkAction::None: return "none";
    case PhoneLinkAction::Relist: return "relist";
    case PhoneLinkAction::ReconnectTransport: return "reconnect-transport";
    case PhoneLinkAction::RestartOwnedServer: return "restart-owned-server";
    case PhoneLinkAction::StartShadowServer: return "start-shadow-server";
    case PhoneLinkAction::AskDefaultServerToExit: return "ask-default-server-to-exit";
    case PhoneLinkAction::TerminateDefaultServer: return "terminate-default-server";
    case PhoneLinkAction::WakeAndLaunchApp: return "wake-and-launch-app";
    case PhoneLinkAction::WaitForUser: return "wait-for-user";
    case PhoneLinkAction::HandBackToDefault: return "hand-back-to-default";
    }
    return "unknown";
}

std::string_view PhoneLinkEventName(PhoneLinkEvent event)
{
    switch (event) {
    case PhoneLinkEvent::AppStart: return "app-start";
    case PhoneLinkEvent::DeviceArrival: return "device-arrival";
    case PhoneLinkEvent::DeviceRemoval: return "device-removal";
    case PhoneLinkEvent::Resume: return "resume";
    case PhoneLinkEvent::ManualPick: return "manual-pick";
    case PhoneLinkEvent::WindowActivatedAfterIdle: return "window-activated-after-idle";
    }
    return "unknown";
}

PhoneLinkDiagnosis DiagnosePhoneLink(const PhoneLinkObservation& o)
{
    using D = PhoneLinkDiagnosis;
    if (!o.endpointSupported) return D::EndpointUnsupported;
    if (o.sessionStreaming) return D::Connected;
    if (!o.phoneWanted) return D::NotWanted;
    if (o.captureAbandoned) return D::RestartRequired;
    switch (o.phoneState) {
    case PhoneAdbState::NotListed:
        // A default listener that cannot be stopped is named first, also
        // while it does not answer: nothing else OkuFlow can do frees USB.
        if (o.defaultStopRefused) return D::OtherProgramHoldsPhone;
        // A hung listener with the phone unplugged: the connect sentence
        // wins (nothing would be stopped without the interface anyway).
        if (o.serverProbe == PhoneServerProbe::Unresponsive)
            return o.adbInterface == PhoneAdbInterface::Absent && !o.otherUsbListed ? D::NoAdbInterface
                                                                                   : D::ServerUnresponsive;
        if (o.server == PhoneServerKind::None || o.serverProbe == PhoneServerProbe::NoListener ||
            o.serverProbe == PhoneServerProbe::Unknown)
            return D::ServerStarting;
        if (o.adbInterface == PhoneAdbInterface::Absent) return D::NoAdbInterface;
        if (o.otherUsbListed) return D::WantedPhoneNotAttached;
        if (o.otherInstanceRunning && o.adbInterface == PhoneAdbInterface::Present)
            return D::OtherInstanceHoldsPhone;
        if (o.leftoverInstanceRunning && o.adbInterface == PhoneAdbInterface::Present)
            return D::OtherProgramHoldsPhone;
        return D::NotListedByServer;
    case PhoneAdbState::Unauthorized: return D::Unauthorized;
    case PhoneAdbState::Authorizing:
    case PhoneAdbState::Connecting: return D::Authorizing;
    case PhoneAdbState::Offline:
    // `no permissions` (a USB permission problem of the adb host) and states
    // OkuFlow does not know (bootloader, recovery, sideload) take the
    // offline ladder: reconnect, restart, then ask for a re-plug.
    case PhoneAdbState::NoPermissions:
    case PhoneAdbState::Other: return D::Offline;
    case PhoneAdbState::Device: break;
    }
    if (o.adbd == PhoneAdbdHealth::TimedOut) return D::TransportStalled;
    switch (o.companion) {
    case PhoneCompanionProbe::Unknown:
    case PhoneCompanionProbe::Ready: return D::Connected;
    case PhoneCompanionProbe::SessionBusy:
        return o.otherInstanceRunning ? D::OtherInstanceHoldsPhone : D::CompanionBusyOtherHost;
    case PhoneCompanionProbe::Problem:
        if (o.problem == "paused") return D::CompanionPaused;
        if (o.problem == "sessionBusy")
            return o.otherInstanceRunning ? D::OtherInstanceHoldsPhone : D::CompanionBusyOtherHost;
        if (o.problem == "blocked") return D::CompanionBlocked;
        if (o.problem == "hot") return D::CompanionHot;
        return D::CompanionFailed;
    case PhoneCompanionProbe::Refused:
    case PhoneCompanionProbe::TimedOut:
        break;
    }
    if (o.shell.appInstalled == PhoneFact::No) return D::AppNotInstalled;
    // The app process lives but its listener refuses: the companion that is
    // installed today answers nothing while paused. Launched once per event,
    // whatever the keyguard shows (a paused app on a locked phone is woken
    // once, not every 20 s).
    if (o.companion == PhoneCompanionProbe::Refused && o.shell.appRunning == PhoneFact::Yes)
        return D::CompanionListenerClosed;
    if (o.shell.keyguardShowing == PhoneFact::Yes || o.shell.screenAwake == PhoneFact::No)
        return D::PhoneLocked;
    return D::AppNotRunning;
}

bool PhoneLinkWaitsForUser(PhoneLinkDiagnosis diagnosis)
{
    using D = PhoneLinkDiagnosis;
    switch (diagnosis) {
    case D::RestartRequired:
    case D::NoAdbInterface:
    case D::Unauthorized:
    case D::AppNotInstalled:
    case D::CompanionPaused:
    case D::CompanionBusyOtherHost:
    case D::CompanionBlocked:
    case D::CompanionHot:
    case D::OtherInstanceHoldsPhone:
    case D::OtherProgramHoldsPhone:
    case D::ReplugCable:
    case D::CompanionNotResponding:
    case D::CompanionListenerClosed:
    case D::AdbCannotStart:
    case D::WantedPhoneNotAttached:
        return true;
    default:
        return false;
    }
}

bool PhoneLinkDoctor::OnEvent(PhoneLinkEvent event, std::int64_t nowMs)
{
    if (event == PhoneLinkEvent::Resume) {
        // One wake, several signals: one event. Measured from the last
        // applied resume (not sliding), so a steady trickle of signals never
        // swallows a later genuine resume.
        if (lastResumeMs_ >= 0 && nowMs - lastResumeMs_ < policy_.resumeCoalesceMs) return false;
        lastResumeMs_ = nowMs;
    }
    if (event == PhoneLinkEvent::DeviceRemoval) {
        // Removals within the window only (bounded; the oldest is dropped).
        size_t kept = 0;
        for (size_t i = 0; i < static_cast<size_t>(removalCount_); ++i)
            if (nowMs - removals_[i] < policy_.flapWindowMs) removals_[kept++] = removals_[i];
        if (kept == removals_.size()) {
            std::move(removals_.begin() + 1, removals_.end(), removals_.begin());
            --kept;
        }
        removals_[kept++] = nowMs;
        removalCount_ = static_cast<int>(kept);
        lastRemovalMs_ = nowMs;
        if (removalCount_ >= policy_.flapRemovals) flapping_ = true;
    } else if (event == PhoneLinkEvent::ManualPick || event == PhoneLinkEvent::Resume ||
               event == PhoneLinkEvent::AppStart) {
        // A deliberate pick, a resume or the start is a fresh attempt.
        removalCount_ = 0;
        lastRemovalMs_ = -1;
        flapping_ = false;
    }
    ++episode_;
    failedSinceMs_ = -1;
    eventAtMs_ = nowMs;
    nextPollMs_ = nowMs;
    failedCycles_ = 0;
    blindSinceMs_ = unresponsiveSinceMs_ = stuckSinceMs_ = lastReconnectMs_ = shadowSinceMs_ = askedExitMs_ = -1;
    unauthorizedSinceMs_ = authorizingSinceMs_ = connectedSinceMs_ = -1;
    unauthorizedReconnected_ = false;
    defaultStopRefused_ = false;
    stopFailures_ = 0;
    handOvers_ = 0;
    abortedHungStops_ = abortedTakeoverStops_ = 0;
    exitRequestNotNeededUsed_ = false;
    launchHeld_ = false;
    holdReleasedByDeath_ = false;
    lastLaunchMs_ = -1;
    reconnect_ = restart_ = shadow_ = askExit_ = terminate_ = launch_ = handback_ = Budget{};
    return true;
}

std::int64_t PhoneLinkDoctor::SinceEventOr(std::int64_t firstSeenMs) const
{
    return std::max(firstSeenMs, eventAtMs_);
}

void PhoneLinkDoctor::Reserve(Budget& budget, std::int64_t nowMs, std::int64_t retryMs, std::int64_t ceilingMs)
{
    // First attempt after an event is immediate (already allowed); the second
    // waits retryMs; later attempts wait the ceiling until a new event. The
    // spacing follows the reservations, not the attempts a give-back lowered.
    ++budget.attempts;
    ++budget.reserved;
    budget.nextAllowedMs = nowMs + (budget.reserved <= 1 ? retryMs : ceilingMs);
}

void PhoneLinkDoctor::TakeLaunch(std::int64_t nowMs)
{
    // At once, then launchRetryMs, then every launchPeriodicMs.
    ++launch_.attempts;
    lastLaunchMs_ = nowMs;
    const auto index = static_cast<size_t>(launch_.attempts - 1);
    launch_.nextAllowedMs = nowMs + (index < policy_.launchRetryMs.size()
        ? policy_.launchRetryMs[index] : policy_.launchPeriodicMs);
}

bool PhoneLinkDoctor::ReserveLaunch(std::int64_t nowMs)
{
    if (launchHeld_ || !launch_.Allowed(nowMs)) return false;
    TakeLaunch(nowMs);
    return true;
}

PhoneLinkAction PhoneLinkDoctor::LadderFor(PhoneLinkDiagnosis diagnosis, const PhoneLinkObservation& o,
                                           std::int64_t nowMs, PhoneLinkDiagnosis* raised)
{
    using D = PhoneLinkDiagnosis;
    using A = PhoneLinkAction;
    const bool owned = o.server == PhoneServerKind::Owned;
    const bool responsive = o.serverProbe == PhoneServerProbe::Responsive;
    const bool interfacePresent = o.adbInterface == PhoneAdbInterface::Present;
    // Terminating a default-port adb: confirmed, stoppable, the Windows ADB
    // interface present (a slow but healthy server with no phone attached is
    // never killed), and a bounded number of times per episode.
    const auto terminate = [&](bool hostKillSent) {
        if (o.defaultStopRefused) return A::Relist;
        if (!interfacePresent) {
            // Windows cannot tell: a server OkuFlow did not start is never
            // stopped; a hang that lasts ends in the re-plug sentence.
            if (o.adbInterface == PhoneAdbInterface::Unknown &&
                nowMs - SinceEventOr(unresponsiveSinceMs_) >= policy_.unknownInterfaceBlindMs) {
                *raised = D::ReplugCable;
                return A::WaitForUser;
            }
            return A::Relist;
        }
        // Each rung ends at its own aborted stops (the takeover branch
        // terminates after an ask, the hung branch without one).
        const int aborted = hostKillSent ? abortedTakeoverStops_ : abortedHungStops_;
        if (terminate_.attempts >= policy_.terminateCap || aborted >= policy_.abortedStopCap) {
            *raised = D::OtherProgramHoldsPhone;
            return A::WaitForUser;
        }
        if (!terminate_.Allowed(nowMs)) return A::Relist;
        Reserve(terminate_, nowMs, policy_.restartRetryMs, policy_.takeoverCeilingMs);
        hostKillSent_ = hostKillSent;
        return A::TerminateDefaultServer;
    };
    // Rung 4a and its reverse (4d) are counted together per episode (also
    // the transport's own start beside a hung default and its hand-back):
    // at the cap no more private servers start beside the default, so a
    // default that alternately lists and loses the phone is named.
    const auto shadow = [&] {
        // OkuFlow's own adb.exe cannot be started (missing, quarantined,
        // dying or silent at start): that is what is broken, also while
        // another program's adb answers on the default port.
        // The sentence stands and the start is tried again at the ceiling
        // spacing, uncharged (nothing was handed over): nothing else starts
        // a private server beside an answering default, so without this the
        // sentence would outlast its cause (the user allowing adb.exe, or
        // security software releasing it) until OkuFlow is restarted.
        if (o.serverLaunchFailures >= policy_.adbLaunchFailuresToName) {
            *raised = D::AdbCannotStart;
            if (!shadow_.Allowed(nowMs)) return A::WaitForUser;
            Reserve(shadow_, nowMs, policy_.restartCeilingMs, policy_.restartCeilingMs);
            return A::StartShadowServer;
        }
        if (handOvers_ >= policy_.handOverCap) { *raised = D::OtherProgramHoldsPhone; return A::WaitForUser; }
        if (!shadow_.Allowed(nowMs)) return A::Relist;
        Reserve(shadow_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
        ++handOvers_;
        return A::StartShadowServer;
    };
    // OkuFlow's private server beside the default does not answer, or its
    // listing fails, while the default lists the phone usable (ready,
    // unauthorized or authorizing; not "ready" for a phone whose adbd timed
    // out there): the private server is what is broken. Its own diagnosis
    // (ServerStarting, an owned ServerUnresponsive) never looked at the
    // default, so OkuFlow stayed on it for good. Handed back under the
    // hand-back budget and the hand-over accounting (counted when the
    // transport retires it; a live session defers it), as from a blind one.
    const bool defaultServesPhone = owned && o.shadowRunning && o.defaultListsPhone && !o.defaultPhoneStalled;
    const auto handBack = [&] {
        if (!handback_.Allowed(nowMs)) return A::Relist;
        Reserve(handback_, nowMs, policy_.reconnectRetryMs, policy_.reconnectRetryMs);
        return A::HandBackToDefault;
    };
    switch (diagnosis) {
    case D::Connected:
    case D::NotWanted:
    case D::EndpointUnsupported:
        return A::None;
    case D::ServerStarting:
        if (defaultServesPhone) return handBack();
        if (o.serverLaunchFailures >= policy_.adbLaunchFailuresToName) {
            *raised = D::AdbCannotStart;
            return A::WaitForUser;
        }
        return A::Relist;
    case D::ServerUnresponsive:
        // Its listing answered and showed another device (its version
        // probes stay slow): it serves someone and is never stopped.
        if (o.server == PhoneServerKind::BorrowedDefault && o.defaultHasOtherDevices) {
            *raised = D::OtherProgramHoldsPhone;
            return A::WaitForUser;
        }
        // Each aborted stop clears the hang confirmation (the listing
        // answered), so this branch would otherwise confirm, abort and
        // confirm again for ever: once the aborted stops reach their cap the
        // sentence stands, also while the hang is being confirmed again.
        // Likewise once the terminations are spent: the last one cleared the
        // confirmation, and a default respawned faster than the confirmation
        // time would otherwise read "Restoring" for ever.
        if (o.server == PhoneServerKind::BorrowedDefault &&
            (o.defaultUnresponsiveConfirmed || abortedHungStops_ >= policy_.abortedStopCap ||
             terminate_.attempts >= policy_.terminateCap))
            return terminate(false);
        // The user's endpoint (adb environment variables) is never stopped:
        // a hang that lasts ends in the re-plug sentence.
        if (o.server == PhoneServerKind::Override &&
            nowMs - SinceEventOr(unresponsiveSinceMs_) >= policy_.userEndpointUnresponsiveMs) {
            *raised = D::ReplugCable;
            return A::WaitForUser;
        }
        // OkuFlow's own server answers its version probes (so its manager
        // keeps it) while its device listing fails: an adb stuck in a USB
        // transport after sleep or a cable pull. The restart rung and its
        // cap after the blind grace, ending in the re-plug sentence.
        if (owned) {
            if (defaultServesPhone) return handBack();
            if (nowMs - SinceEventOr(unresponsiveSinceMs_) < policy_.blindGraceMs) return A::Relist;
            if (restart_.attempts >= policy_.blindRestartCap) { *raised = D::ReplugCable; return A::WaitForUser; }
            if (!restart_.Allowed(nowMs)) return A::Relist;
            Reserve(restart_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
            return A::RestartOwnedServer;
        }
        return A::Relist;
    case D::OtherInstanceHoldsPhone:
    case D::OtherProgramHoldsPhone:
        // A sibling window (or a leftover) is blamed only after the blind
        // grace: until then it may just be showing a PDF while USB settles.
        if (o.phoneState == PhoneAdbState::NotListed && !o.defaultStopRefused &&
            nowMs - SinceEventOr(blindSinceMs_) < policy_.blindGraceMs) {
            *raised = D::NotListedByServer;
            return A::Relist;
        }
        return A::WaitForUser;
    case D::NotListedByServer: {
        const std::int64_t blindFor = nowMs - SinceEventOr(blindSinceMs_);
        if (!responsive || blindFor < policy_.blindGraceMs) return A::Relist;
        // The default lists the phone usable (ready, unauthorized or
        // authorizing): use it. Offline there is not serving it, and
        // neither is "ready" for a phone whose adbd timed out there.
        // Also at the hand-over cap: the cap prefers the configuration in
        // which the phone is listed (no more private servers beside the
        // default are started, see shadow()). The hand-back counts toward
        // the cap when the transport actually retires its private server
        // (NoteHandOver): a live session can defer it.
        if (defaultServesPhone) return handBack();
        // Windows cannot tell (Unknown): after a long blind period OkuFlow's
        // own server may be started or restarted as if the interface were
        // there; another server is never asked to exit or stopped (below).
        const bool assumedPresent = o.adbInterface == PhoneAdbInterface::Unknown &&
            blindFor >= policy_.unknownInterfaceBlindMs;
        if (!interfacePresent && !assumedPresent) return A::Relist;
        // The user's endpoint is never taken over: responsive but blind to a
        // phone Windows shows after the grace, it ends in the re-plug
        // sentence (probing goes on; a listing of the phone ends it).
        if (o.server == PhoneServerKind::Override) { *raised = D::ReplugCable; return A::WaitForUser; }
        if (owned && !o.shadowRunning) {
            if (restart_.attempts >= policy_.blindRestartCap) { *raised = D::ReplugCable; return A::WaitForUser; }
            if (!restart_.Allowed(nowMs)) return A::Relist;
            Reserve(restart_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
            return A::RestartOwnedServer;
        }
        if (o.server == PhoneServerKind::BorrowedDefault) return shadow();
        if (owned && o.shadowRunning) {
            if (nowMs - SinceEventOr(shadowSinceMs_) < policy_.takeoverGraceMs) return A::Relist;
            if (o.defaultHasOtherDevices) { *raised = D::OtherProgramHoldsPhone; return A::WaitForUser; }
            // Unknown: the default is never asked to exit or stopped.
            if (!interfacePresent) { *raised = D::ReplugCable; return A::WaitForUser; }
            if (askedExitMs_ < 0) {
                if (askExit_.attempts >= policy_.askExitCap || abortedTakeoverStops_ >= policy_.abortedStopCap) {
                    *raised = D::OtherProgramHoldsPhone;
                    return A::WaitForUser;
                }
                if (!askExit_.Allowed(nowMs)) return A::Relist;
                Reserve(askExit_, nowMs, policy_.takeoverGraceMs, policy_.takeoverCeilingMs);
                askedExitMs_ = nowMs;
                return A::AskDefaultServerToExit;
            }
            if (nowMs - askedExitMs_ >= policy_.hostKillWaitMs) return terminate(true);
        }
        return A::Relist;
    }
    case D::Unauthorized:
        // The question may have been dismissed or timed out: one targeted
        // reconnect after a quiet period makes the phone ask again.
        if (responsive && !unauthorizedReconnected_ && unauthorizedSinceMs_ >= 0 &&
            nowMs - SinceEventOr(unauthorizedSinceMs_) >= policy_.unauthorizedReconnectMs) {
            unauthorizedReconnected_ = true;
            return A::ReconnectTransport;
        }
        return A::WaitForUser;
    case D::Authorizing:
        // The Allow handshake itself is never reset; a short grace reads
        // "Restoring" so a normal re-plug does not flash the Allow sentence.
        if (o.phoneState == PhoneAdbState::Authorizing)
            return nowMs - SinceEventOr(authorizingSinceMs_) < policy_.authorizingGraceMs ? A::Relist
                                                                                          : A::WaitForUser;
        [[fallthrough]];
    case D::Offline:
    case D::TransportStalled: {
        if (!responsive) return A::Relist;
        // Reconnects to another program's server and the owned offline
        // ladder are separate: after a takeover the owned ladder starts
        // fresh instead of waiting out the foreign reconnects' ceiling.
        if (reconnectOwned_ != owned) {
            reconnect_ = Budget{};
            lastReconnectMs_ = -1;
            reconnectOwned_ = owned;
        }
        const std::int64_t stuckFor = nowMs - SinceEventOr(stuckSinceMs_);
        if (diagnosis == D::Authorizing && stuckFor < policy_.authorizingGraceMs) return A::Relist;
        if (!owned) {
            // Another program's default server, or the user's endpoint: a
            // bounded number of targeted reconnects (counted also when one
            // cannot be sent, e.g. no transport id), then an end state.
            if (reconnect_.attempts < policy_.foreignReconnects) {
                if (!reconnect_.Allowed(nowMs)) return A::Relist;
                Reserve(reconnect_, nowMs, policy_.reconnectRetryMs, policy_.reconnectCeilingMs);
                lastReconnectMs_ = nowMs;
                return A::ReconnectTransport;
            }
            if (lastReconnectMs_ >= 0 && nowMs - lastReconnectMs_ < policy_.stuckAfterReconnectMs) return A::Relist;
            // The user's endpoint is never taken over.
            if (o.server != PhoneServerKind::BorrowedDefault) { *raised = D::ReplugCable; return A::WaitForUser; }
            // A default that keeps the phone stuck holds the interface
            // without serving it: the takeover rungs, unless it serves
            // another device.
            if (o.defaultHasOtherDevices) { *raised = D::OtherProgramHoldsPhone; return A::WaitForUser; }
            return shadow();
        }
        // Once the restarts are spent the re-plug sentence stands until an
        // event or a change of state; a free reconnect slot no longer flips
        // it back to "Restoring".
        if (restart_.attempts >= policy_.blindRestartCap) { *raised = D::ReplugCable; return A::WaitForUser; }
        const bool restartDue = owned && lastReconnectMs_ >= 0 &&
            nowMs - lastReconnectMs_ >= policy_.stuckAfterReconnectMs && restart_.Allowed(nowMs);
        if (restartDue && reconnect_.attempts >= 2) {
            if (restart_.attempts >= policy_.blindRestartCap) { *raised = D::ReplugCable; return A::WaitForUser; }
            Reserve(restart_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
            lastReconnectMs_ = -1;
            return A::RestartOwnedServer;
        }
        if (reconnect_.Allowed(nowMs)) {
            Reserve(reconnect_, nowMs, policy_.reconnectRetryMs, policy_.reconnectCeilingMs);
            lastReconnectMs_ = nowMs;
            return A::ReconnectTransport;
        }
        if (restartDue) {
            if (restart_.attempts >= policy_.blindRestartCap) { *raised = D::ReplugCable; return A::WaitForUser; }
            Reserve(restart_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
            lastReconnectMs_ = -1;
            return A::RestartOwnedServer;
        }
        if (owned && restart_.attempts >= policy_.blindRestartCap) { *raised = D::ReplugCable; return A::WaitForUser; }
        return A::Relist;
    }
    case D::PhoneLocked:
    case D::AppNotRunning: {
        // The app process is gone: a hold taken while it ran (paused, or a
        // listener that hung) no longer applies. The first such release in
        // an episode relaunches at once; later ones wait the periodic launch
        // spacing after the last launch, so a paused app that a battery
        // manager keeps killing is not pulled to the front every time. The
        // immediate relaunch is spent only by a decision that launches.
        const bool firstDeathRelease = launchHeld_ && o.shell.appRunning == PhoneFact::No && !holdReleasedByDeath_;
        const std::int64_t heldNextAllowedMs = launch_.nextAllowedMs;
        if (launchHeld_ && o.shell.appRunning == PhoneFact::No) {
            launchHeld_ = false;
            if (firstDeathRelease) {
                launch_.nextAllowedMs = std::min(launch_.nextAllowedMs, nowMs);
            } else if (lastLaunchMs_ >= 0) {
                launch_.nextAllowedMs = std::max(launch_.nextAllowedMs, lastLaunchMs_ + policy_.launchPeriodicMs);
            }
        }
        // A listener that accepts but never answers is not helped by more
        // launches (am start only brings the hanging activity forward).
        const bool hanging = o.companion == PhoneCompanionProbe::TimedOut && o.adbd == PhoneAdbdHealth::Answers;
        // Also when the episode's launches are held (the app was launched
        // once while its listener was closed and now hangs).
        if (hanging && (launch_.attempts >= policy_.hangingCompanionLaunches || launchHeld_)) {
            launchHeld_ = true;
            if (firstDeathRelease) launch_.nextAllowedMs = heldNextAllowedMs; // Nothing launched.
            *raised = D::CompanionNotResponding;
            return A::WaitForUser;
        }
        if (ReserveLaunch(nowMs)) {
            if (firstDeathRelease) holdReleasedByDeath_ = true;
            return A::WakeAndLaunchApp;
        }
        return launch_.attempts >= 2 || launchHeld_ ? A::WaitForUser : A::Relist;
    }
    case D::CompanionListenerClosed:
        // Paused (companion installed today) or a listener that closed while
        // the process lives: one launch per event, never a periodic loop
        // that pulls the app over whatever the user does on the phone.
        // The session start path shares this budget: one launch per event
        // between both of them.
        if (launch_.attempts == 0 && !launchHeld_) {
            launchHeld_ = true;
            ++launch_.attempts;
            lastLaunchMs_ = nowMs;
            launch_.nextAllowedMs = nowMs + policy_.launchPeriodicMs;
            return A::WakeAndLaunchApp;
        }
        // Shortly after OkuFlow's own launch (ladder or session start) the
        // process runs before its listener opens: still starting. Not held,
        // so a start that fails is retried at the ordinary spacing; it reads
        // as the app being started ("Restoring").
        if (!launchHeld_ && lastLaunchMs_ >= 0 && nowMs - lastLaunchMs_ < policy_.launchSettleMs) {
            *raised = D::AppNotRunning;
            return A::Relist;
        }
        launchHeld_ = true;
        return A::WaitForUser;
    case D::CompanionFailed:
        // Camera busy in another phone app, failed, no mode: "Restoring" for
        // a while (the companion retries), then say what to do on the phone.
        return nowMs - SinceEventOr(failedSinceMs_) >= policy_.companionFailedGraceMs ? A::WaitForUser : A::Relist;
    default:
        return A::WaitForUser;
    }
}

PhoneLinkDecision PhoneLinkDoctor::Decide(const PhoneLinkObservation& observation, std::int64_t nowMs)
{
    using D = PhoneLinkDiagnosis;
    PhoneLinkObservation o = observation;
    // A listed phone proves nothing holds its USB interface blindly any more
    // (also when the default-port server is the one that lists it).
    if (o.phoneState != PhoneAdbState::NotListed ||
        (o.shadowRunning && o.defaultListsPhone && !o.defaultPhoneStalled))
        ClearDefaultStopRefused();
    o.defaultStopRefused = o.defaultStopRefused || defaultStopRefused_;
    PhoneLinkDecision decision;
    decision.diagnosis = DiagnosePhoneLink(o);

    // Episode markers for time-based gates.
    const bool notListed = o.phoneState == PhoneAdbState::NotListed &&
        (decision.diagnosis == D::NotListedByServer || decision.diagnosis == D::OtherInstanceHoldsPhone ||
         decision.diagnosis == D::OtherProgramHoldsPhone);
    if (notListed) {
        if (blindSinceMs_ < 0) blindSinceMs_ = nowMs;
    } else {
        blindSinceMs_ = -1;
    }
    const bool stuck = decision.diagnosis == D::Authorizing || decision.diagnosis == D::Offline ||
                       decision.diagnosis == D::TransportStalled;
    if (decision.diagnosis == D::ServerUnresponsive) { if (unresponsiveSinceMs_ < 0) unresponsiveSinceMs_ = nowMs; }
    else unresponsiveSinceMs_ = -1;
    if (stuck) { if (stuckSinceMs_ < 0) stuckSinceMs_ = nowMs; }
    else { stuckSinceMs_ = -1; lastReconnectMs_ = -1; }
    if (o.phoneState == PhoneAdbState::Authorizing) { if (authorizingSinceMs_ < 0) authorizingSinceMs_ = nowMs; }
    else authorizingSinceMs_ = -1;
    if (decision.diagnosis == D::CompanionFailed) { if (failedSinceMs_ < 0) failedSinceMs_ = nowMs; }
    else failedSinceMs_ = -1;
    if (decision.diagnosis == D::Unauthorized) { if (unauthorizedSinceMs_ < 0) unauthorizedSinceMs_ = nowMs; }
    else { unauthorizedSinceMs_ = -1; unauthorizedReconnected_ = false; }
    if (o.shadowRunning) { if (shadowSinceMs_ < 0) shadowSinceMs_ = nowMs; }
    else { shadowSinceMs_ = -1; askedExitMs_ = -1; }
    if (decision.diagnosis != D::NotListedByServer && decision.diagnosis != D::ServerUnresponsive &&
        decision.diagnosis != D::OtherProgramHoldsPhone)
        askedExitMs_ = -1;

    hostKillSent_ = false;
    PhoneLinkDiagnosis raised = decision.diagnosis;
    // A flapping cable: a quiet period ends the verdict; while it stands,
    // a phone the server does not reach names the cable and no rung runs
    // (probing goes on at the user-wait cadence). A phone that is listed
    // ready or unauthorized is reached at this moment: its own ladder and
    // its own sentence apply (tap Allow, resume streaming, install the app).
    // The re-plug sentence over those would send a user who re-plugged a
    // few times in a row back to the cable, each re-plug renewing the
    // verdict, while the phone waits for an answer on its screen.
    if (flapping_ && lastRemovalMs_ >= 0 && nowMs - lastRemovalMs_ >= policy_.flapQuietMs) {
        flapping_ = false;
        removalCount_ = 0;
    }
    const bool reached = o.phoneState == PhoneAdbState::Device || o.phoneState == PhoneAdbState::Unauthorized;
    if (flapping_ && !reached && decision.diagnosis != D::Connected && decision.diagnosis != D::NotWanted &&
        decision.diagnosis != D::EndpointUnsupported && decision.diagnosis != D::RestartRequired) {
        raised = D::ReplugCable;
        decision.action = PhoneLinkAction::WaitForUser;
    } else {
        decision.action = LadderFor(decision.diagnosis, o, nowMs, &raised);
    }
    decision.diagnosis = raised;
    decision.hostKillAlreadySent = decision.action == PhoneLinkAction::TerminateDefaultServer && hostKillSent_;

    std::int64_t delay = 0;
    if (decision.diagnosis == D::Connected) {
        failedCycles_ = 0;
        if (connectedSinceMs_ < 0) connectedSinceMs_ = nowMs;
        // Budgets renew only once the link held, or while frames flow: a
        // companion flipping between ready and refused is not relaunched
        // at once on every flip.
        if (o.sessionStreaming || nowMs - connectedSinceMs_ >= policy_.connectedHoldMs) {
            reconnect_ = restart_ = shadow_ = askExit_ = terminate_ = launch_ = handback_ = Budget{};
            stopFailures_ = 0;
            abortedHungStops_ = abortedTakeoverStops_ = 0;
            exitRequestNotNeededUsed_ = false;
            launchHeld_ = false;
            // The link holds: a flapping verdict ends.
            flapping_ = false;
            removalCount_ = 0;
            // handOvers_ is renewed only by events: a default that works for
            // a while between losing the phone still ends in the sentence.
        }
        delay = policy_.healthyPollMs;
    } else {
        connectedSinceMs_ = -1;
        if (decision.diagnosis == D::NotWanted || decision.diagnosis == D::EndpointUnsupported) {
            delay = policy_.idlePollMs;
        } else if (PhoneLinkWaitsForUser(decision.diagnosis) || decision.action == PhoneLinkAction::WaitForUser) {
            delay = policy_.userWaitPollMs;
        } else {
            delay = policy_.pollBackoffMs[std::min<size_t>(failedCycles_, policy_.pollBackoffMs.size() - 1)];
            ++failedCycles_;
        }
    }
    decision.nextPollMs = nowMs + delay;
    decision.episode = episode_;
    nextPollMs_ = decision.nextPollMs;
    decision.diagnosisChanged = !haveLast_ || last_ != decision.diagnosis;
    last_ = decision.diagnosis;
    haveLast_ = true;
    return decision;
}

} // namespace okuflow
