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
        if (o.serverProbe == PhoneServerProbe::Unresponsive) return D::ServerUnresponsive;
        if (o.server == PhoneServerKind::None || o.serverProbe == PhoneServerProbe::NoListener ||
            o.serverProbe == PhoneServerProbe::Unknown)
            return D::ServerStarting;
        if (o.defaultStopRefused) return D::OtherProgramHoldsPhone;
        if (o.adbInterface == PhoneAdbInterface::Absent) return D::NoAdbInterface;
        if (o.otherInstanceRunning && o.adbInterface == PhoneAdbInterface::Present)
            return D::OtherInstanceHoldsPhone;
        if (o.leftoverInstanceRunning && o.adbInterface == PhoneAdbInterface::Present)
            return D::OtherProgramHoldsPhone;
        return D::NotListedByServer;
    case PhoneAdbState::Unauthorized: return D::Unauthorized;
    case PhoneAdbState::Authorizing:
    case PhoneAdbState::Connecting: return D::Authorizing;
    case PhoneAdbState::Offline:
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
        return true;
    default:
        return false;
    }
}

void PhoneLinkDoctor::OnEvent(PhoneLinkEvent, std::int64_t nowMs)
{
    eventAtMs_ = nowMs;
    nextPollMs_ = nowMs;
    failedCycles_ = 0;
    blindSinceMs_ = stuckSinceMs_ = lastReconnectMs_ = shadowSinceMs_ = askedExitMs_ = -1;
    defaultStopRefused_ = false;
    reconnect_ = restart_ = shadow_ = askExit_ = terminate_ = launch_ = Budget{};
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

PhoneLinkAction PhoneLinkDoctor::LadderFor(PhoneLinkDiagnosis diagnosis, const PhoneLinkObservation& o,
                                           std::int64_t nowMs)
{
    using D = PhoneLinkDiagnosis;
    using A = PhoneLinkAction;
    const bool owned = o.server == PhoneServerKind::Owned;
    const bool responsive = o.serverProbe == PhoneServerProbe::Responsive;
    switch (diagnosis) {
    case D::Connected:
    case D::NotWanted:
    case D::EndpointUnsupported:
        return A::None;
    case D::ServerStarting:
        return A::Relist;
    case D::ServerUnresponsive:
        if (o.server == PhoneServerKind::BorrowedDefault && o.defaultUnresponsiveConfirmed &&
            !o.defaultStopRefused && terminate_.Allowed(nowMs)) {
            Reserve(terminate_, nowMs, policy_.restartRetryMs, policy_.takeoverCeilingMs);
            return A::TerminateDefaultServer;
        }
        return A::Relist; // An owned server restarts itself after two failed probes.
    case D::NotListedByServer: {
        if (o.adbInterface != PhoneAdbInterface::Present || !responsive) return A::Relist;
        if (nowMs - SinceEventOr(blindSinceMs_) < policy_.blindGraceMs) return A::Relist;
        if (owned && !o.shadowRunning) {
            if (!restart_.Allowed(nowMs)) return A::Relist;
            Reserve(restart_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
            return A::RestartOwnedServer;
        }
        if (o.server == PhoneServerKind::BorrowedDefault) {
            if (!shadow_.Allowed(nowMs)) return A::Relist;
            Reserve(shadow_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
            return A::StartShadowServer;
        }
        if (owned && o.shadowRunning) {
            if (nowMs - SinceEventOr(shadowSinceMs_) < policy_.takeoverGraceMs || o.defaultListsPhone)
                return A::Relist;
            if (o.defaultHasOtherDevices) return A::WaitForUser; // Diagnosis is raised by Decide.
            if (askedExitMs_ < 0) {
                if (!askExit_.Allowed(nowMs)) return A::Relist;
                Reserve(askExit_, nowMs, policy_.takeoverCeilingMs, policy_.takeoverCeilingMs);
                askedExitMs_ = nowMs;
                return A::AskDefaultServerToExit;
            }
            if (nowMs - askedExitMs_ >= policy_.hostKillWaitMs && terminate_.Allowed(nowMs)) {
                Reserve(terminate_, nowMs, policy_.restartRetryMs, policy_.takeoverCeilingMs);
                return A::TerminateDefaultServer;
            }
        }
        return A::Relist;
    }
    case D::Authorizing:
    case D::Offline:
    case D::TransportStalled: {
        if (!responsive) return A::Relist;
        const std::int64_t stuckFor = nowMs - SinceEventOr(stuckSinceMs_);
        if (diagnosis == D::Authorizing && stuckFor < 2000) return A::Relist;
        if (owned && lastReconnectMs_ >= 0 && nowMs - lastReconnectMs_ >= policy_.stuckAfterReconnectMs &&
            reconnect_.attempts >= 2 && restart_.Allowed(nowMs)) {
            Reserve(restart_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
            lastReconnectMs_ = -1;
            return A::RestartOwnedServer;
        }
        if (reconnect_.Allowed(nowMs)) {
            Reserve(reconnect_, nowMs, policy_.reconnectRetryMs, policy_.reconnectCeilingMs);
            lastReconnectMs_ = nowMs;
            return A::ReconnectTransport;
        }
        if (owned && lastReconnectMs_ >= 0 && nowMs - lastReconnectMs_ >= policy_.stuckAfterReconnectMs &&
            restart_.Allowed(nowMs)) {
            Reserve(restart_, nowMs, policy_.restartRetryMs, policy_.restartCeilingMs);
            lastReconnectMs_ = -1;
            return A::RestartOwnedServer;
        }
        return A::Relist;
    }
    case D::PhoneLocked:
    case D::AppNotRunning:
        if (launch_.Allowed(nowMs)) {
            ++launch_.attempts;
            const auto index = static_cast<size_t>(launch_.attempts - 1);
            launch_.nextAllowedMs = nowMs + (index < policy_.launchRetryMs.size()
                ? policy_.launchRetryMs[index] : policy_.launchPeriodicMs);
            return A::WakeAndLaunchApp;
        }
        return launch_.attempts >= 2 ? A::WaitForUser : A::Relist;
    case D::CompanionFailed:
        return A::Relist;
    default:
        return A::WaitForUser;
    }
}

PhoneLinkDecision PhoneLinkDoctor::Decide(const PhoneLinkObservation& observation, std::int64_t nowMs)
{
    using D = PhoneLinkDiagnosis;
    PhoneLinkObservation o = observation;
    o.defaultStopRefused = o.defaultStopRefused || defaultStopRefused_;
    PhoneLinkDecision decision;
    decision.diagnosis = DiagnosePhoneLink(o);

    // Episode markers for time-based gates.
    if (decision.diagnosis == D::NotListedByServer) {
        if (blindSinceMs_ < 0) blindSinceMs_ = nowMs;
    } else {
        blindSinceMs_ = -1;
    }
    const bool stuck = decision.diagnosis == D::Authorizing || decision.diagnosis == D::Offline ||
                       decision.diagnosis == D::TransportStalled;
    if (stuck) { if (stuckSinceMs_ < 0) stuckSinceMs_ = nowMs; }
    else { stuckSinceMs_ = -1; lastReconnectMs_ = -1; }
    if (o.shadowRunning) { if (shadowSinceMs_ < 0) shadowSinceMs_ = nowMs; }
    else shadowSinceMs_ = -1;
    if (decision.diagnosis != D::NotListedByServer && decision.diagnosis != D::ServerUnresponsive)
        askedExitMs_ = -1;

    decision.action = LadderFor(decision.diagnosis, o, nowMs);
    // A responsive default that serves other devices keeps them; once the
    // private server could not reach the phone either, say so plainly.
    if (decision.diagnosis == D::NotListedByServer && decision.action == PhoneLinkAction::WaitForUser)
        decision.diagnosis = D::OtherProgramHoldsPhone;

    std::int64_t delay = 0;
    if (decision.diagnosis == D::Connected) {
        failedCycles_ = 0;
        reconnect_ = restart_ = shadow_ = askExit_ = terminate_ = launch_ = Budget{};
        delay = policy_.healthyPollMs;
    } else if (decision.diagnosis == D::NotWanted || decision.diagnosis == D::EndpointUnsupported) {
        delay = policy_.idlePollMs;
    } else if (PhoneLinkWaitsForUser(decision.diagnosis) || decision.action == PhoneLinkAction::WaitForUser) {
        delay = policy_.userWaitPollMs;
    } else {
        delay = policy_.pollBackoffMs[std::min<size_t>(failedCycles_, policy_.pollBackoffMs.size() - 1)];
        ++failedCycles_;
    }
    decision.nextPollMs = nowMs + delay;
    nextPollMs_ = decision.nextPollMs;
    decision.diagnosisChanged = !haveLast_ || last_ != decision.diagnosis;
    last_ = decision.diagnosis;
    haveLast_ = true;
    return decision;
}

} // namespace okuflow
