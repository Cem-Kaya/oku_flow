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

void PhoneLinkDoctor::OnEvent(PhoneLinkEvent, std::int64_t nowMs)
{
    ++episode_;
    failedSinceMs_ = -1;
    eventAtMs_ = nowMs;
    nextPollMs_ = nowMs;
    failedCycles_ = 0;
    blindSinceMs_ = stuckSinceMs_ = lastReconnectMs_ = shadowSinceMs_ = askedExitMs_ = -1;
    unauthorizedSinceMs_ = authorizingSinceMs_ = connectedSinceMs_ = -1;
    unauthorizedReconnected_ = false;
    defaultStopRefused_ = false;
    stopFailures_ = 0;
    handOvers_ = 0;
    launchHeld_ = false;
    lastLaunchMs_ = -1;
    reconnect_ = restart_ = shadow_ = askExit_ = terminate_ = launch_ = handback_ = Budget{};
}

std::int64_t PhoneLinkDoctor::SinceEventOr(std::int64_t firstSeenMs) const
{
    return std::max(firstSeenMs, eventAtMs_);
}

void PhoneLinkDoctor::Reserve(Budget& budget, std::int64_t nowMs, std::int64_t retryMs, std::int64_t ceilingMs)
{
    // First attempt after an event is immediate (already allowed); the second
    // waits retryMs; later attempts wait the ceiling until a new event.
    ++budget.attempts;
    budget.nextAllowedMs = nowMs + (budget.attempts <= 1 ? retryMs : ceilingMs);
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
        if (o.defaultStopRefused || !interfacePresent) return A::Relist;
        if (terminate_.attempts >= policy_.terminateCap) { *raised = D::OtherProgramHoldsPhone; return A::WaitForUser; }
        if (!terminate_.Allowed(nowMs)) return A::Relist;
        Reserve(terminate_, nowMs, policy_.restartRetryMs, policy_.takeoverCeilingMs);
        hostKillSent_ = hostKillSent;
        return A::TerminateDefaultServer;
    };
    // Rung 4a and its reverse (4d) are capped together per episode: a
    // default that alternately lists and loses the phone is named.
    const auto shadow = [&] {
        if (handOvers_ >= policy_.handOverCap) { *raised = D::OtherProgramHoldsPhone; return A::WaitForUser; }
        if (!shadow_.Allowed(nowMs)) return A::Relist;
        Reserve(shadow_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
        ++handOvers_;
        return A::StartShadowServer;
    };
    switch (diagnosis) {
    case D::Connected:
    case D::NotWanted:
    case D::EndpointUnsupported:
        return A::None;
    case D::ServerStarting:
        if (o.serverLaunchFailures >= policy_.adbLaunchFailuresToName) {
            *raised = D::AdbCannotStart;
            return A::WaitForUser;
        }
        return A::Relist;
    case D::ServerUnresponsive:
        if (o.server == PhoneServerKind::BorrowedDefault && o.defaultUnresponsiveConfirmed)
            return terminate(false);
        return A::Relist; // An owned server restarts itself after two failed probes.
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
        // Windows cannot tell (Unknown): only OkuFlow's own server, and only
        // after a long blind period, is treated as if the interface were there.
        const bool assumedPresent = o.adbInterface == PhoneAdbInterface::Unknown && owned && !o.shadowRunning &&
            nowMs - SinceEventOr(blindSinceMs_) >= policy_.unknownInterfaceBlindMs;
        if ((!interfacePresent && !assumedPresent) || !responsive) return A::Relist;
        if (nowMs - SinceEventOr(blindSinceMs_) < policy_.blindGraceMs) return A::Relist;
        if (owned && !o.shadowRunning) {
            if (restart_.attempts >= policy_.blindRestartCap) { *raised = D::ReplugCable; return A::WaitForUser; }
            if (!restart_.Allowed(nowMs)) return A::Relist;
            Reserve(restart_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
            return A::RestartOwnedServer;
        }
        if (o.server == PhoneServerKind::BorrowedDefault) return shadow();
        if (owned && o.shadowRunning) {
            // The default lists the phone usable (ready, unauthorized or
            // authorizing): use it. Offline there is not serving it, and
            // neither is "ready" for a phone whose adbd timed out there.
            if (o.defaultListsPhone && !o.defaultPhoneStalled) {
                if (handOvers_ >= policy_.handOverCap) { *raised = D::OtherProgramHoldsPhone; return A::WaitForUser; }
                if (!handback_.Allowed(nowMs)) return A::Relist;
                Reserve(handback_, nowMs, policy_.reconnectRetryMs, policy_.reconnectRetryMs);
                ++handOvers_;
                return A::HandBackToDefault;
            }
            if (nowMs - SinceEventOr(shadowSinceMs_) < policy_.takeoverGraceMs) return A::Relist;
            if (o.defaultHasOtherDevices) { *raised = D::OtherProgramHoldsPhone; return A::WaitForUser; }
            if (askedExitMs_ < 0) {
                if (askExit_.attempts >= policy_.askExitCap) { *raised = D::OtherProgramHoldsPhone; return A::WaitForUser; }
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
        // listener that hung) no longer applies; relaunch it now.
        if (launchHeld_ && o.shell.appRunning == PhoneFact::No) {
            launchHeld_ = false;
            launch_.nextAllowedMs = std::min(launch_.nextAllowedMs, nowMs);
        }
        // A listener that accepts but never answers is not helped by more
        // launches (am start only brings the hanging activity forward).
        const bool hanging = o.companion == PhoneCompanionProbe::TimedOut && o.adbd == PhoneAdbdHealth::Answers;
        // Also when the episode's launches are held (the app was launched
        // once while its listener was closed and now hangs).
        if (hanging && (launch_.attempts >= policy_.hangingCompanionLaunches || launchHeld_)) {
            launchHeld_ = true;
            *raised = D::CompanionNotResponding;
            return A::WaitForUser;
        }
        if (ReserveLaunch(nowMs)) return A::WakeAndLaunchApp;
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
    decision.action = LadderFor(decision.diagnosis, o, nowMs, &raised);
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
            launchHeld_ = false;
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
