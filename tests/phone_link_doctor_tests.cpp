// Model-free contract for the phone link decision unit: every diagnosis,
// every event reset, every budget, and the owner's reconnect sequences.
#include "okuflow/capture/phone_link_doctor.hpp"
#include <QtTest>
#include <algorithm>
#include <functional>
#include <vector>

#define QCOMPARE_MS(actual, expected) QCOMPARE(std::int64_t(actual), std::int64_t(expected))

using namespace okuflow;
using D = PhoneLinkDiagnosis;
using A = PhoneLinkAction;

namespace {
// A ready phone on OkuFlow's own server.
PhoneLinkObservation Ready()
{
    PhoneLinkObservation o;
    o.phoneWanted = true;
    o.adbInterface = PhoneAdbInterface::Present;
    o.server = PhoneServerKind::Owned;
    o.serverProbe = PhoneServerProbe::Responsive;
    o.phoneState = PhoneAdbState::Device;
    o.adbd = PhoneAdbdHealth::Answers;
    o.companion = PhoneCompanionProbe::Ready;
    return o;
}
PhoneLinkObservation With(std::function<void(PhoneLinkObservation&)> change)
{
    auto o = Ready();
    change(o);
    return o;
}
PhoneLinkObservation Blind(PhoneServerKind kind = PhoneServerKind::Owned)
{
    return With([kind](auto& o) {
        o.server = kind; o.phoneState = PhoneAdbState::NotListed;
        o.adbd = PhoneAdbdHealth::Unknown; o.companion = PhoneCompanionProbe::Unknown;
    });
}
PhoneLinkObservation Refused(PhoneFact awake, PhoneFact keyguard, PhoneFact installed = PhoneFact::Yes)
{
    return With([&](auto& o) {
        o.companion = PhoneCompanionProbe::Refused;
        o.shell = {awake, keyguard, installed, PhoneFact::No};
    });
}
PhoneLinkObservation Problem(const char* code)
{
    return With([code](auto& o) { o.companion = PhoneCompanionProbe::Problem; o.problem = code; });
}
} // namespace

Q_DECLARE_METATYPE(PhoneLinkObservation)
Q_DECLARE_METATYPE(PhoneLinkDiagnosis)

class PhoneLinkDoctorTests : public QObject {
    Q_OBJECT
private slots:
    void diagnosis_data()
    {
        QTest::addColumn<PhoneLinkObservation>("observation");
        QTest::addColumn<PhoneLinkDiagnosis>("expected");
        QTest::newRow("ready") << Ready() << D::Connected;
        QTest::newRow("live session wins") << With([](auto& o) {
            o.sessionStreaming = true; o.phoneState = PhoneAdbState::Offline; }) << D::Connected;
        QTest::newRow("not probed while occupied") << With([](auto& o) {
            o.companion = PhoneCompanionProbe::Unknown; }) << D::Connected;
        QTest::newRow("no intent") << With([](auto& o) { o.phoneWanted = false; }) << D::NotWanted;
        QTest::newRow("unsupported endpoint") << With([](auto& o) { o.endpointSupported = false; })
                                              << D::EndpointUnsupported;
        QTest::newRow("abandoned capture") << With([](auto& o) { o.captureAbandoned = true; })
                                           << D::RestartRequired;
        QTest::newRow("no server yet") << With([](auto& o) {
            o.server = PhoneServerKind::None; o.serverProbe = PhoneServerProbe::NoListener;
            o.phoneState = PhoneAdbState::NotListed; }) << D::ServerStarting;
        QTest::newRow("hung server") << With([](auto& o) {
            o.server = PhoneServerKind::BorrowedDefault; o.serverProbe = PhoneServerProbe::Unresponsive;
            o.phoneState = PhoneAdbState::NotListed; }) << D::ServerUnresponsive;
        // Iteration 3 (A2): a hung listener with the phone unplugged.
        QTest::newRow("hung server, phone unplugged") << With([](auto& o) {
            o.server = PhoneServerKind::BorrowedDefault; o.serverProbe = PhoneServerProbe::Unresponsive;
            o.phoneState = PhoneAdbState::NotListed; o.adbInterface = PhoneAdbInterface::Absent; })
                                                      << D::NoAdbInterface;
        QTest::newRow("no ADB interface") << With([](auto& o) {
            o.adbInterface = PhoneAdbInterface::Absent; o.phoneState = PhoneAdbState::NotListed; })
                                          << D::NoAdbInterface;
        QTest::newRow("blind server") << Blind() << D::NotListedByServer;
        QTest::newRow("unauthorized") << With([](auto& o) { o.phoneState = PhoneAdbState::Unauthorized; })
                                      << D::Unauthorized;
        QTest::newRow("authorizing") << With([](auto& o) { o.phoneState = PhoneAdbState::Authorizing; })
                                     << D::Authorizing;
        QTest::newRow("connecting") << With([](auto& o) { o.phoneState = PhoneAdbState::Connecting; })
                                    << D::Authorizing;
        QTest::newRow("offline") << With([](auto& o) { o.phoneState = PhoneAdbState::Offline; }) << D::Offline;
        QTest::newRow("adbd silent") << With([](auto& o) {
            o.adbd = PhoneAdbdHealth::TimedOut; o.companion = PhoneCompanionProbe::TimedOut; })
                                     << D::TransportStalled;
        QTest::newRow("locked") << Refused(PhoneFact::Yes, PhoneFact::Yes) << D::PhoneLocked;
        QTest::newRow("asleep") << Refused(PhoneFact::No, PhoneFact::Unknown) << D::PhoneLocked;
        QTest::newRow("app killed") << Refused(PhoneFact::Yes, PhoneFact::No) << D::AppNotRunning;
        // adbd answers (the shell query replied) while the listener hangs.
        QTest::newRow("frozen app") << With([](auto& o) {
            o.companion = PhoneCompanionProbe::TimedOut; o.adbd = PhoneAdbdHealth::Answers; })
                                    << D::AppNotRunning;
        QTest::newRow("app process alive, listener refuses") << With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes}; })
                                                             << D::CompanionListenerClosed;
        // Iteration 3 (C3): the listener refuses while the app process runs,
        // whatever the keyguard shows (paused on a locked phone).
        QTest::newRow("paused and locked") << With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes, PhoneFact::Yes}; })
                                           << D::CompanionListenerClosed;
        QTest::newRow("no permissions") << With([](auto& o) { o.phoneState = PhoneAdbState::NoPermissions; })
                                        << D::Offline;
        QTest::newRow("unknown state") << With([](auto& o) { o.phoneState = PhoneAdbState::Other; }) << D::Offline;
        QTest::newRow("another phone listed") << With([](auto& o) {
            o.phoneState = PhoneAdbState::NotListed; o.otherUsbListed = true; }) << D::WantedPhoneNotAttached;
        QTest::newRow("not installed") << Refused(PhoneFact::Yes, PhoneFact::No, PhoneFact::No)
                                       << D::AppNotInstalled;
        QTest::newRow("paused") << Problem("paused") << D::CompanionPaused;
        QTest::newRow("other host") << With([](auto& o) { o.companion = PhoneCompanionProbe::SessionBusy; })
                                    << D::CompanionBusyOtherHost;
        QTest::newRow("sessionBusy code") << Problem("sessionBusy") << D::CompanionBusyOtherHost;
        QTest::newRow("blocked") << Problem("blocked") << D::CompanionBlocked;
        QTest::newRow("hot") << Problem("hot") << D::CompanionHot;
        QTest::newRow("camera busy") << Problem("busy") << D::CompanionFailed;
        QTest::newRow("failed") << Problem("failed") << D::CompanionFailed;
        QTest::newRow("unknown code") << Problem("future") << D::CompanionFailed;
        QTest::newRow("two instances, phone busy") << With([](auto& o) {
            o.otherInstanceRunning = true; o.companion = PhoneCompanionProbe::SessionBusy; })
                                                   << D::OtherInstanceHoldsPhone;
        QTest::newRow("two instances, phone held") << With([](auto& o) {
            o.otherInstanceRunning = true; o.phoneState = PhoneAdbState::NotListed; })
                                                   << D::OtherInstanceHoldsPhone;
        QTest::newRow("windowless leftover instance") << With([](auto& o) {
            o.leftoverInstanceRunning = true; o.phoneState = PhoneAdbState::NotListed; })
                                                      << D::OtherProgramHoldsPhone;
        QTest::newRow("stop refused") << With([](auto& o) {
            o.defaultStopRefused = true; o.phoneState = PhoneAdbState::NotListed; })
                                      << D::OtherProgramHoldsPhone;
        // Refusal wins over the probe that the hung server keeps failing.
        QTest::newRow("stop refused, server unresponsive") << With([](auto& o) {
            o.defaultStopRefused = true; o.phoneState = PhoneAdbState::NotListed;
            o.server = PhoneServerKind::BorrowedDefault; o.serverProbe = PhoneServerProbe::Unresponsive; })
                                                           << D::OtherProgramHoldsPhone;
    }
    void diagnosis()
    {
        QFETCH(PhoneLinkObservation, observation);
        QFETCH(PhoneLinkDiagnosis, expected);
        QCOMPARE(int(DiagnosePhoneLink(observation)), int(expected));
    }
    void namesCoverEveryDiagnosis()
    {
        for (int i = 0; i < kPhoneLinkDiagnosisCount; ++i)
            QVERIFY(PhoneLinkDiagnosisName(static_cast<D>(i)) != "unknown");
        QVERIFY(PhoneLinkActionName(A::HandBackToDefault) != "unknown");
    }
    void eventMakesNextCycleImmediateAndPollBacksOffToCeiling()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        QCOMPARE_MS(doctor.NextPollMs(), 0);
        const auto starting = With([](auto& o) {
            o.server = PhoneServerKind::None; o.serverProbe = PhoneServerProbe::NoListener;
            o.phoneState = PhoneAdbState::NotListed; });
        std::int64_t now = 0;
        std::vector<std::int64_t> delays;
        for (int i = 0; i < 12; ++i) {
            const auto decision = doctor.Decide(starting, now);
            QCOMPARE(int(decision.action), int(A::Relist));
            delays.push_back(decision.nextPollMs - now);
            now = decision.nextPollMs;
        }
        QCOMPARE_MS(delays.front(), 1000);
        QCOMPARE_MS(delays[4], 1000);
        QCOMPARE_MS(delays[5], 2000);
        QCOMPARE_MS(delays.back(), 10000); // Ceiling: a lecture still recovers within seconds.
        QVERIFY(*std::max_element(delays.begin(), delays.end()) <= 10000);
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, now);
        QCOMPARE_MS(doctor.Decide(starting, now).nextPollMs - now, 1000);
    }
    void waitingForUserPollsSteadily()
    {
        // Changed deliberately (iteration 2, A11): after a quiet period in
        // Unauthorized exactly one targeted reconnect re-asks the question.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto unauthorized = With([](auto& o) { o.phoneState = PhoneAdbState::Unauthorized; });
        int reconnects = 0;
        for (std::int64_t now = 0; now < 120000; now += 2000) {
            const auto decision = doctor.Decide(unauthorized, now);
            if (decision.action == A::ReconnectTransport) {
                ++reconnects;
                QVERIFY(now >= doctor.Policy().unauthorizedReconnectMs);
            } else {
                QCOMPARE(int(decision.action), int(A::WaitForUser));
            }
            QCOMPARE(int(decision.diagnosis), int(D::Unauthorized));
            QCOMPARE_MS(decision.nextPollMs - now, 2000);
        }
        QCOMPARE(reconnects, 1);
        // A new event (re-plug) renews that single reconnect.
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 120000);
        QCOMPARE(int(doctor.Decide(unauthorized, 120000).action), int(A::WaitForUser));
        QCOMPARE(int(doctor.Decide(unauthorized, 140000).action), int(A::ReconnectTransport));
    }
    void unauthorizedThenAuthorized()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        auto first = doctor.Decide(With([](auto& o) { o.phoneState = PhoneAdbState::Unauthorized; }), 0);
        QCOMPARE(int(first.diagnosis), int(D::Unauthorized));
        QVERIFY(first.diagnosisChanged);
        auto second = doctor.Decide(Ready(), 2000);
        QCOMPARE(int(second.diagnosis), int(D::Connected));
        QVERIFY(second.diagnosisChanged);
        QCOMPARE(int(second.action), int(A::None));
    }
    void offlineThatAReconnectFixes()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::Resume, 0);
        const auto offline = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        QCOMPARE(int(doctor.Decide(offline, 0).action), int(A::ReconnectTransport)); // Immediate.
        QCOMPARE(int(doctor.Decide(Ready(), 1000).diagnosis), int(D::Connected));
        // Changed deliberately (iteration 2, C8): Connected renews the
        // budgets only after it held; a flip back half a second later waits
        // for the reconnect budget instead of resetting at once.
        QCOMPARE(int(doctor.Decide(offline, 1500).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(offline, 3000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(Ready(), 4000).diagnosis), int(D::Connected));
        QCOMPARE(int(doctor.Decide(Ready(), 14000).diagnosis), int(D::Connected)); // Held 10 s.
        QCOMPARE(int(doctor.Decide(offline, 14500).action), int(A::ReconnectTransport));
    }
    void offlineThatNeedsAServerRestart()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::Resume, 0);
        const auto offline = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        QCOMPARE(int(doctor.Decide(offline, 0).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(offline, 1000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(offline, 3000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(offline, 6000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(offline, 9000).action), int(A::RestartOwnedServer));
        // Restart budget: next one 15 s later, then the 60 s ceiling.
        int restarts = 0;
        for (std::int64_t now = 9500; now <= 40000; now += 500)
            restarts += doctor.Decide(offline, now).action == A::RestartOwnedServer;
        QCOMPARE(restarts, 1);
    }
    void stuckPhoneOnAnotherServerEscalates_data()
    {
        QTest::addColumn<PhoneLinkObservation>("stuck");
        const auto borrowed = [](PhoneAdbState state, PhoneAdbdHealth adbd = PhoneAdbdHealth::Answers) {
            return With([=](auto& o) {
                o.server = PhoneServerKind::BorrowedDefault; o.phoneState = state; o.adbd = adbd;
                if (adbd == PhoneAdbdHealth::TimedOut) o.companion = PhoneCompanionProbe::TimedOut;
            });
        };
        QTest::newRow("offline") << borrowed(PhoneAdbState::Offline);
        QTest::newRow("connecting") << borrowed(PhoneAdbState::Connecting);
        QTest::newRow("no permissions") << borrowed(PhoneAdbState::NoPermissions);
        QTest::newRow("stalled") << borrowed(PhoneAdbState::Device, PhoneAdbdHealth::TimedOut);
    }
    void stuckPhoneOnAnotherServerEscalates()
    {
        // Iteration 3 (A1), replaces borrowedServerOfflineOnlyReconnects,
        // which only asserted what did not happen: a default that keeps the
        // phone stuck holds the interface without serving it. Two targeted
        // reconnects, then the takeover rungs; never an owned restart.
        QFETCH(PhoneLinkObservation, stuck);
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        std::vector<A> sequence;
        std::int64_t now = 0;
        for (; now <= 30000; now += 500) {
            const auto decision = doctor.Decide(stuck, now);
            QVERIFY(decision.action != A::RestartOwnedServer);
            if (decision.action != A::Relist) sequence.push_back(decision.action);
            if (decision.action == A::StartShadowServer) break;
        }
        QVERIFY(sequence == (std::vector<A>{A::ReconnectTransport, A::ReconnectTransport, A::StartShadowServer}));
        // The private server now runs beside it; the default still lists the
        // phone stuck (not usable), so it is asked to exit, then stopped.
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        sequence.clear();
        for (now += 500; now <= 60000; now += 500) {
            const auto decision = doctor.Decide(beside, now);
            if (decision.action != A::Relist) sequence.push_back(decision.action);
            if (decision.action == A::TerminateDefaultServer) break;
        }
        QVERIFY(sequence == (std::vector<A>{A::AskDefaultServerToExit, A::TerminateDefaultServer}));
    }
    void stuckPhoneOnAServerServingAnotherDeviceIsNamed()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto offline = With([](auto& o) {
            o.server = PhoneServerKind::BorrowedDefault; o.phoneState = PhoneAdbState::Offline;
            o.defaultHasOtherDevices = true; });
        PhoneLinkDecision last;
        int reconnects = 0;
        for (std::int64_t now = 0; now <= 60000; now += 500) {
            last = doctor.Decide(offline, now);
            QVERIFY(last.action != A::StartShadowServer && last.action != A::AskDefaultServerToExit &&
                    last.action != A::TerminateDefaultServer && last.action != A::RestartOwnedServer);
            reconnects += last.action == A::ReconnectTransport;
        }
        QCOMPARE(reconnects, doctor.Policy().foreignReconnects);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(last.action), int(A::WaitForUser));
    }
    void stuckPhoneOnAUserEndpointAsksForAReplug()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto offline = With([](auto& o) {
            o.server = PhoneServerKind::Override; o.phoneState = PhoneAdbState::Offline; });
        PhoneLinkDecision last;
        int reconnects = 0;
        for (std::int64_t now = 0; now <= 60000; now += 500) {
            last = doctor.Decide(offline, now);
            QVERIFY(last.action == A::ReconnectTransport || last.action == A::Relist || last.action == A::WaitForUser);
            reconnects += last.action == A::ReconnectTransport;
        }
        QCOMPARE(reconnects, doctor.Policy().foreignReconnects);
        QCOMPARE(int(last.diagnosis), int(D::ReplugCable));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        // An event (re-plug) renews the reconnects.
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 61000);
        QCOMPARE(int(doctor.Decide(offline, 61000).action), int(A::ReconnectTransport));
    }
    void defaultThatAlternatelyListsAndLosesThePhoneIsNamed()
    {
        // A8: shadow, hand back, shadow, hand back... is capped per episode.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto lost = Blind(PhoneServerKind::BorrowedDefault);
        auto listing = Blind(PhoneServerKind::Owned);
        listing.shadowRunning = true;
        listing.defaultListsPhone = true;
        int handOvers = 0;
        PhoneLinkDecision last;
        std::int64_t now = 0;
        for (int round = 0; round < 10; ++round) {
            for (int i = 0; i < 200; ++i, now += 500) {
                last = doctor.Decide(lost, now);
                if (last.action != A::Relist) break;
            }
            if (last.action != A::StartShadowServer) break;
            ++handOvers;
            for (int i = 0; i < 200; ++i, now += 500) {
                last = doctor.Decide(listing, now);
                if (last.action != A::Relist) break;
            }
            if (last.action != A::HandBackToDefault) break;
            ++handOvers;
        }
        QCOMPARE(handOvers, doctor.Policy().handOverCap);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, now);
        QCOMPARE(int(doctor.Decide(listing, now).action), int(A::Relist)); // Blind grace first.
        QCOMPARE(int(doctor.Decide(listing, now + doctor.Policy().blindGraceMs).action), int(A::HandBackToDefault));
    }
    void authorizingGetsAShortGraceFirst()
    {
        // Changed deliberately (iteration 2, B3): authorizing is the Allow
        // handshake. It is never reset; after the grace the user is asked.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        const auto authorizing = With([](auto& o) { o.phoneState = PhoneAdbState::Authorizing; });
        QCOMPARE(int(doctor.Decide(authorizing, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(authorizing, 1000).action), int(A::Relist));
        for (std::int64_t now = 2000; now < 60000; now += 1000) {
            const auto decision = doctor.Decide(authorizing, now);
            QCOMPARE(int(decision.action), int(A::WaitForUser));
            QCOMPARE(int(decision.diagnosis), int(D::Authorizing));
        }
        // `connecting` is not the handshake: it keeps the reconnect ladder.
        PhoneLinkDoctor connectingDoctor;
        connectingDoctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        const auto connecting = With([](auto& o) { o.phoneState = PhoneAdbState::Connecting; });
        QCOMPARE(int(connectingDoctor.Decide(connecting, 0).action), int(A::Relist));
        QCOMPARE(int(connectingDoctor.Decide(connecting, 2000).action), int(A::ReconnectTransport));
    }
    void blindOwnedServerRestartsAfterGrace()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        const auto blind = Blind();
        QCOMPARE(int(doctor.Decide(blind, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 4999).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 5000).action), int(A::RestartOwnedServer));
        QCOMPARE(int(doctor.Decide(blind, 6000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 20000).action), int(A::RestartOwnedServer));
        QCOMPARE(int(doctor.Decide(blind, 60000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 80000).action), int(A::RestartOwnedServer));
        // Iteration 2 (A9): after the third restart of the episode the user
        // is asked to re-plug the cable; probing goes on, restarts stop.
        for (std::int64_t now = 81000; now < 300000; now += 1000) {
            const auto decision = doctor.Decide(blind, now);
            QCOMPARE(int(decision.diagnosis), int(D::ReplugCable));
            QCOMPARE(int(decision.action), int(A::WaitForUser));
            QCOMPARE_MS(decision.nextPollMs - now, doctor.Policy().userWaitPollMs);
        }
        // An event renews both grace and budget.
        doctor.OnEvent(PhoneLinkEvent::Resume, 301000);
        QCOMPARE(int(doctor.Decide(blind, 301000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 306000).action), int(A::RestartOwnedServer));
    }
    void blindWithoutWindowsInterfaceNeverEscalates()
    {
        // Changed deliberately (iteration 3, A4): Windows cannot tell
        // (Unknown) is treated as present for OkuFlow's own server after a
        // long blind period: restarts and their cap, then the re-plug
        // sentence. Before that period, and for any other server, nothing.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto unknown = Blind();
        unknown.adbInterface = PhoneAdbInterface::Unknown;
        const auto period = doctor.Policy().unknownInterfaceBlindMs;
        int restarts = 0;
        PhoneLinkDecision last;
        for (std::int64_t now = 0; now < period + 300000; now += 1000) {
            last = doctor.Decide(unknown, now);
            if (now < period) QCOMPARE(int(last.action), int(A::Relist));
            restarts += last.action == A::RestartOwnedServer;
        }
        QCOMPARE(restarts, doctor.Policy().blindRestartCap);
        QCOMPARE(int(last.diagnosis), int(D::ReplugCable));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        // Never for a server OkuFlow did not start, also after the period.
        PhoneLinkDoctor borrowedDoctor;
        borrowedDoctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto borrowed = Blind(PhoneServerKind::BorrowedDefault);
        borrowed.adbInterface = PhoneAdbInterface::Unknown;
        auto beside = Blind(PhoneServerKind::Owned);
        beside.adbInterface = PhoneAdbInterface::Unknown;
        beside.shadowRunning = true;
        for (std::int64_t now = 0; now < period + 120000; now += 1000) {
            QCOMPARE(int(borrowedDoctor.Decide(borrowed, now).action), int(A::Relist));
            QCOMPARE(int(borrowedDoctor.Decide(beside, now).action), int(A::Relist));
        }
        auto absent = Blind();
        absent.adbInterface = PhoneAdbInterface::Absent;
        const auto decision = doctor.Decide(absent, 61000);
        QCOMPARE(int(decision.diagnosis), int(D::NoAdbInterface));
        QCOMPARE(int(decision.action), int(A::WaitForUser));
    }
    void blindBorrowedServerWithoutOtherDevicesIsTakenOver()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto borrowed = Blind(PhoneServerKind::BorrowedDefault);
        QCOMPARE(int(doctor.Decide(borrowed, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(borrowed, 5000).action), int(A::StartShadowServer));
        auto shadow = Blind(PhoneServerKind::Owned);
        shadow.shadowRunning = true;
        QCOMPARE(int(doctor.Decide(shadow, 6000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 11999).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 12000).action), int(A::AskDefaultServerToExit));
        QCOMPARE(int(doctor.Decide(shadow, 13000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 14000).action), int(A::TerminateDefaultServer));
        QCOMPARE(int(doctor.Decide(shadow, 15000).action), int(A::Relist));
    }
    void blindBorrowedServerWithOtherDevicesIsNeverStopped()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        QCOMPARE(int(doctor.Decide(Blind(PhoneServerKind::BorrowedDefault), 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(Blind(PhoneServerKind::BorrowedDefault), 5000).action),
                 int(A::StartShadowServer));
        auto shadow = Blind(PhoneServerKind::Owned);
        shadow.shadowRunning = true;
        shadow.defaultHasOtherDevices = true;
        for (std::int64_t now = 6000; now < 120000; now += 1000) {
            const auto decision = doctor.Decide(shadow, now);
            QVERIFY(decision.action != A::AskDefaultServerToExit && decision.action != A::TerminateDefaultServer);
            if (now >= 12000) {
                QCOMPARE(int(decision.diagnosis), int(D::OtherProgramHoldsPhone));
                QCOMPARE(int(decision.action), int(A::WaitForUser));
            }
        }
    }
    void overriddenEndpointIsNeverStopped()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto blind = Blind(PhoneServerKind::Override);
        auto hung = blind;
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        for (std::int64_t now = 0; now < 120000; now += 1000) {
            for (const auto& o : {blind, hung}) {
                const auto action = doctor.Decide(o, now).action;
                QVERIFY(action == A::Relist);
            }
        }
    }
    void confirmedHungDefaultIsTerminatedWithBudget()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        QCOMPARE(int(doctor.Decide(hung, 0).action), int(A::Relist)); // Not confirmed yet.
        hung.defaultUnresponsiveConfirmed = true;
        const auto first = doctor.Decide(hung, 1000);
        QCOMPARE(int(first.action), int(A::TerminateDefaultServer));
        QVERIFY(!first.hostKillAlreadySent); // A hung server never got host:kill: send it first.
        QCOMPARE(int(doctor.Decide(hung, 2000).action), int(A::Relist));
        doctor.NoteDefaultStopRefused();
        // Changed deliberately (iteration 2, A5): the refusal is named while
        // the probe still says unresponsive, and nothing is retried.
        for (std::int64_t now = 20000; now < 120000; now += 1000) {
            const auto refused = doctor.Decide(hung, now);
            QCOMPARE(int(refused.diagnosis), int(D::OtherProgramHoldsPhone));
            QCOMPARE(int(refused.action), int(A::WaitForUser));
        }
    }
    void hungDefaultIsNotTerminatedWithoutTheWindowsInterface()
    {
        // C2: a slow but healthy default with no phone attached is never killed.
        for (auto present : {PhoneAdbInterface::Absent, PhoneAdbInterface::Unknown}) {
            PhoneLinkDoctor doctor;
            doctor.OnEvent(PhoneLinkEvent::Resume, 0);
            auto hung = Blind(PhoneServerKind::BorrowedDefault);
            hung.serverProbe = PhoneServerProbe::Unresponsive;
            hung.defaultUnresponsiveConfirmed = true;
            hung.adbInterface = present;
            for (std::int64_t now = 0; now < 120000; now += 1000)
                QVERIFY(doctor.Decide(hung, now).action != A::TerminateDefaultServer);
        }
    }
    void everyFailedStopOutcomeEndsInTheSentence()
    {
        // A6: identity changed / failed twice, or refused / not adb / unopenable once.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        QCOMPARE(int(doctor.Decide(hung, 0).action), int(A::TerminateDefaultServer));
        doctor.NoteDefaultStopFailed();
        QVERIFY(!doctor.DefaultStopRefused()); // One retry.
        QCOMPARE(int(doctor.Decide(hung, 15000).action), int(A::TerminateDefaultServer));
        doctor.NoteDefaultStopFailed();
        QVERIFY(doctor.DefaultStopRefused());
        const auto named = doctor.Decide(hung, 16000);
        QCOMPARE(int(named.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(named.action), int(A::WaitForUser));
        // Without any note, terminations are capped per episode too.
        PhoneLinkDoctor capped;
        capped.OnEvent(PhoneLinkEvent::AppStart, 0);
        int terminations = 0;
        PhoneLinkDecision last;
        for (std::int64_t now = 0; now < 600000; now += 1000) {
            last = capped.Decide(hung, now);
            terminations += last.action == A::TerminateDefaultServer;
        }
        QCOMPARE(terminations, capped.Policy().terminateCap);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
    }
    void stopRefusalEndsWithItsCause()
    {
        // A7: refusal is cleared once the phone is listed (or the transport
        // clears it when the listener is gone or changed).
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        doctor.NoteDefaultStopRefused();
        QCOMPARE(int(doctor.Decide(Blind(), 0).diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(doctor.Decide(Ready(), 1000).diagnosis), int(D::Connected));
        QVERIFY(!doctor.DefaultStopRefused());
        QCOMPARE(int(doctor.Decide(Blind(), 2000).diagnosis), int(D::NotListedByServer));
        doctor.NoteDefaultStopRefused();
        doctor.ClearDefaultStopRefused();
        QCOMPARE(int(doctor.Decide(Blind(), 3000).diagnosis), int(D::NotListedByServer));
    }
    void shadowedDefaultThatListsThePhoneIsHandedBack()
    {
        // A8: a default that lists the phone (also unauthorized) is used.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto shadow = Blind(PhoneServerKind::Owned);
        shadow.shadowRunning = true;
        QCOMPARE(int(doctor.Decide(shadow, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 5000).action), int(A::Relist)); // Takeover grace.
        shadow.defaultListsPhone = true;
        QCOMPARE(int(doctor.Decide(shadow, 6000).action), int(A::HandBackToDefault));
        QCOMPARE(int(doctor.Decide(shadow, 7000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 9000).action), int(A::HandBackToDefault));
        // Even a stop refusal from an earlier takeover gives way to the listing.
        doctor.NoteDefaultStopRefused();
        QCOMPARE(int(doctor.Decide(shadow, 12000).action), int(A::HandBackToDefault));
    }
    void defaultThatKeepsComingBackEndsInTheSentence()
    {
        // A2: OkuFlow's server blind beside another adb that comes back after
        // every host:kill (Android Studio restarting its adb): bounded.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        int asks = 0, terminations = 0;
        PhoneLinkDecision last;
        std::int64_t now = 0;
        for (int round = 0; round < 6; ++round) {
            // The other adb is beside us until it is asked to exit...
            for (int i = 0; i < 20; ++i, now += 1000) {
                last = doctor.Decide(beside, now);
                asks += last.action == A::AskDefaultServerToExit;
                terminations += last.action == A::TerminateDefaultServer;
                if (last.action == A::AskDefaultServerToExit) { now += 1000; break; }
            }
            // ...exits, and comes back a few seconds later.
            for (int i = 0; i < 3; ++i, now += 1000) doctor.Decide(Blind(PhoneServerKind::Owned), now);
        }
        for (int i = 0; i < 10; ++i, now += 1000) { // Back again, past the takeover grace.
            last = doctor.Decide(beside, now);
            asks += last.action == A::AskDefaultServerToExit;
        }
        QCOMPARE(asks, doctor.Policy().askExitCap);
        QCOMPARE(terminations, 0);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(last.action), int(A::WaitForUser));
    }
    void aNewListenerBesideIsAskedBeforeItIsTerminated()
    {
        // A2: the asked listener was replaced by another process before the
        // termination step: the new one gets the grace and host:kill first.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        QCOMPARE(int(doctor.Decide(beside, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(beside, 6000).action), int(A::AskDefaultServerToExit));
        doctor.NoteDefaultListenerChanged();
        QCOMPARE(int(doctor.Decide(beside, 8000).action), int(A::Relist)); // Not terminated.
        QCOMPARE(int(doctor.Decide(beside, 14000).action), int(A::AskDefaultServerToExit));
    }
    void anotherListedPhoneIsNotBlindness()
    {
        // C4: a second phone listed by the server is not "blind": no restarts.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto other = Blind();
        other.otherUsbListed = true;
        for (std::int64_t now = 0; now < 120000; now += 1000) {
            const auto decision = doctor.Decide(other, now);
            QCOMPARE(int(decision.diagnosis), int(D::WantedPhoneNotAttached));
            QCOMPARE(int(decision.action), int(A::WaitForUser));
        }
    }
    void hangingCompanionIsNamedAfterBoundedLaunches()
    {
        // A15: the listener accepts and never answers.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto hanging = With([](auto& o) {
            o.companion = PhoneCompanionProbe::TimedOut; o.adbd = PhoneAdbdHealth::Answers;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes}; });
        int launches = 0;
        PhoneLinkDecision last;
        for (std::int64_t now = 0; now < 300000; now += 1000) {
            last = doctor.Decide(hanging, now);
            launches += last.action == A::WakeAndLaunchApp;
        }
        QCOMPARE(launches, doctor.Policy().hangingCompanionLaunches);
        QCOMPARE(int(last.diagnosis), int(D::CompanionNotResponding));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        doctor.OnEvent(PhoneLinkEvent::ManualPick, 300000);
        QCOMPARE(int(doctor.Decide(hanging, 300000).action), int(A::WakeAndLaunchApp));
    }
    void oldCompanionWhilePausedIsLaunchedOnlyOnEvents()
    {
        // D1: Pause in the companion installed today: no listener, the probe
        // is refused, the app process runs. One launch per event, no loop.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto paused = With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes}; });
        int launches = 0;
        for (std::int64_t now = 0; now < 300000; now += 1000) {
            const auto decision = doctor.Decide(paused, now);
            launches += decision.action == A::WakeAndLaunchApp;
            QCOMPARE(int(decision.diagnosis), int(D::CompanionListenerClosed));
        }
        QCOMPARE(launches, 1);
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 300000);
        QCOMPARE(int(doctor.Decide(paused, 300000).action), int(A::WakeAndLaunchApp));
    }
    void companionFailureIsNamedAfterAGrace()
    {
        // Iteration 3 (A5): busy camera, failed, no mode: "Restoring" while
        // the companion retries, then the user is told; an event restarts it.
        for (const char* code : {"busy", "failed", "noMode"}) {
            PhoneLinkDoctor doctor;
            doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
            const auto failed = Problem(code);
            const auto grace = doctor.Policy().companionFailedGraceMs;
            for (std::int64_t now = 0; now < grace; now += 1000)
                QCOMPARE(int(doctor.Decide(failed, now).action), int(A::Relist));
            for (std::int64_t now = grace; now < grace + 60000; now += 1000) {
                const auto decision = doctor.Decide(failed, now);
                QCOMPARE(int(decision.diagnosis), int(D::CompanionFailed));
                QCOMPARE(int(decision.action), int(A::WaitForUser));
            }
            doctor.OnEvent(PhoneLinkEvent::ManualPick, grace + 60000);
            QCOMPARE(int(doctor.Decide(failed, grace + 60000).action), int(A::Relist));
        }
    }
    void stopOutcomeOfAnEarlierEpisodeIsDropped()
    {
        // Iteration 3 (C4): a refusal noted after a newer event does not
        // leak into the new episode.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        const auto decision = doctor.Decide(hung, 0);
        QCOMPARE(int(decision.action), int(A::TerminateDefaultServer));
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 100); // While the stop ran.
        QVERIFY(!doctor.NoteDefaultStopRefused(decision.episode));
        QVERIFY(!doctor.NoteDefaultStopFailed(decision.episode));
        QVERIFY(!doctor.DefaultStopRefused());
        QCOMPARE(int(doctor.Decide(hung, 200).action), int(A::TerminateDefaultServer));
        // The current episode's outcome is kept.
        QVERIFY(doctor.NoteDefaultStopRefused(doctor.Episode()));
        QVERIFY(doctor.DefaultStopRefused());
    }
    void sessionStartSharesThePerEventLaunchBudget()
    {
        // C3: the session start path and the ladder draw on one budget.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::ManualPick, 0);
        QVERIFY(doctor.ReserveLaunch(0));   // The start path launches first.
        QVERIFY(!doctor.ReserveLaunch(100)); // Not again at once.
        const auto paused = With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes, PhoneFact::Yes}; });
        for (std::int64_t now = 1000; now < 120000; now += 1000) {
            QCOMPARE(int(doctor.Decide(paused, now).action), int(A::WaitForUser));
            QVERIFY(!doctor.ReserveLaunch(now)); // Held until the next event.
        }
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 120000);
        QVERIFY(doctor.ReserveLaunch(120000));
        // A killed app (process gone) keeps the ordinary spaced budget.
        PhoneLinkDoctor killed;
        killed.OnEvent(PhoneLinkEvent::AppStart, 0);
        QVERIFY(killed.ReserveLaunch(0));
        QCOMPARE(int(killed.Decide(Refused(PhoneFact::Yes, PhoneFact::No), 500).action), int(A::Relist));
        QCOMPARE(int(killed.Decide(Refused(PhoneFact::Yes, PhoneFact::No), 3000).action), int(A::WakeAndLaunchApp));
        QVERIFY(!killed.ReserveLaunch(3500));
    }
    void adbThatCannotStartIsNamed()
    {
        // A16: OkuFlow's own adb.exe cannot be launched (quarantine, missing).
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto starting = With([](auto& o) {
            o.server = PhoneServerKind::Owned; o.serverProbe = PhoneServerProbe::NoListener;
            o.phoneState = PhoneAdbState::NotListed; });
        starting.serverLaunchFailures = 1;
        QCOMPARE(int(doctor.Decide(starting, 0).diagnosis), int(D::ServerStarting));
        starting.serverLaunchFailures = 2;
        const auto named = doctor.Decide(starting, 1000);
        QCOMPARE(int(named.diagnosis), int(D::AdbCannotStart));
        QCOMPARE(int(named.action), int(A::WaitForUser));
        starting.serverLaunchFailures = 0;
        QCOMPARE(int(doctor.Decide(starting, 2000).diagnosis), int(D::ServerStarting));
    }
    void flappingCompanionIsNotRelaunchedOnEveryFlip()
    {
        // C8: ready / refused / ready / refused within seconds.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto killed = Refused(PhoneFact::Yes, PhoneFact::No);
        int launches = 0;
        for (std::int64_t now = 0; now < 60000; now += 1000)
            launches += doctor.Decide((now / 1000) % 2 ? Ready() : killed, now).action == A::WakeAndLaunchApp;
        // The ordinary launch budget (0, 3, 9, 29, 49 s), not one per flip (30).
        QVERIFY2(launches <= 5, QByteArray::number(launches).constData());
        // A live session renews at once.
        auto streaming = Ready();
        streaming.sessionStreaming = true;
        doctor.Decide(streaming, 60000);
        QCOMPARE(int(doctor.Decide(killed, 60500).action), int(A::WakeAndLaunchApp));
    }
    void lockedPhoneIsWokenThenUserIsAsked()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        const auto locked = Refused(PhoneFact::No, PhoneFact::Yes);
        QCOMPARE(int(doctor.Decide(locked, 0).action), int(A::WakeAndLaunchApp));
        QCOMPARE(int(doctor.Decide(locked, 1000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(locked, 3000).action), int(A::WakeAndLaunchApp));
        QCOMPARE(int(doctor.Decide(locked, 5000).action), int(A::WaitForUser));
        QCOMPARE(int(doctor.Decide(locked, 9000).action), int(A::WakeAndLaunchApp));
        // Slow periodic retry while the user wants the phone.
        int launches = 0;
        for (std::int64_t now = 10000; now <= 50000; now += 1000)
            launches += doctor.Decide(locked, now).action == A::WakeAndLaunchApp;
        QCOMPARE(launches, 2); // 29 s and 49 s.
    }
    void appKilledIsRelaunchedAndManualPickRenewsLaunches()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto killed = Refused(PhoneFact::Yes, PhoneFact::No);
        QCOMPARE(int(doctor.Decide(killed, 0).action), int(A::WakeAndLaunchApp));
        QCOMPARE(int(doctor.Decide(killed, 100).action), int(A::Relist));
        doctor.OnEvent(PhoneLinkEvent::ManualPick, 200);
        QCOMPARE(int(doctor.Decide(killed, 200).action), int(A::WakeAndLaunchApp));
    }
    void pausedAndBusyNeverLaunchOrReset()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        for (const auto& o : {Problem("paused"), Problem("hot"), Problem("blocked"),
                              With([](auto& p) { p.companion = PhoneCompanionProbe::SessionBusy; })}) {
            for (std::int64_t now = 0; now < 30000; now += 1000) {
                const auto action = doctor.Decide(o, now).action;
                QCOMPARE(int(action), int(A::WaitForUser));
            }
        }
    }
    void sleepWithPhoneAttachedAndResume()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        QCOMPARE(int(doctor.Decide(With([](auto& o) { o.sessionStreaming = true; }), 0).diagnosis),
                 int(D::Connected));
        // Resume: pre-sleep budgets are void; the transport comes back offline first.
        doctor.OnEvent(PhoneLinkEvent::Resume, 3600000);
        const auto offline = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        QCOMPARE(int(doctor.Decide(offline, 3600000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(Ready(), 3601000).diagnosis), int(D::Connected));
    }
    void pullAndReplugWithinOneSecond()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto offline = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        QCOMPARE(int(doctor.Decide(offline, 0).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(offline, 3000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(offline, 4000).action), int(A::Relist)); // Budget spent.
        doctor.OnEvent(PhoneLinkEvent::DeviceRemoval, 4100);
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 4900);
        QCOMPARE_MS(doctor.NextPollMs(), 4900);
        QCOMPARE(int(doctor.Decide(offline, 4900).action), int(A::ReconnectTransport));
    }
    void everyEventResetsEveryBudget()
    {
        for (auto event : {PhoneLinkEvent::AppStart, PhoneLinkEvent::DeviceArrival, PhoneLinkEvent::DeviceRemoval,
                           PhoneLinkEvent::Resume, PhoneLinkEvent::ManualPick,
                           PhoneLinkEvent::WindowActivatedAfterIdle}) {
            PhoneLinkDoctor doctor;
            doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
            const auto killed = Refused(PhoneFact::Yes, PhoneFact::No);
            QCOMPARE(int(doctor.Decide(killed, 0).action), int(A::WakeAndLaunchApp));
            QCOMPARE(int(doctor.Decide(killed, 10).action), int(A::Relist));
            doctor.NoteDefaultStopRefused();
            doctor.OnEvent(event, 20);
            QVERIFY(!doctor.DefaultStopRefused());
            QCOMPARE_MS(doctor.LastEventMs(), 20);
            QCOMPARE(int(doctor.Decide(killed, 20).action), int(A::WakeAndLaunchApp));
        }
        QCOMPARE(PhoneLinkEventName(PhoneLinkEvent::Resume), std::string_view("resume"));
    }
    void twoInstancesWaitForTheUser()
    {
        // Changed deliberately (iteration 2, A12): the sibling window is
        // blamed only after the blind grace; before that "Restoring".
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto held = Blind();
        held.otherInstanceRunning = true;
        for (std::int64_t now = 0; now < 30000; now += 1000) {
            const auto decision = doctor.Decide(held, now);
            QVERIFY(decision.action != A::RestartOwnedServer); // Never restarts against a sibling.
            if (now < doctor.Policy().blindGraceMs) {
                QCOMPARE(int(decision.diagnosis), int(D::NotListedByServer));
                QCOMPARE(int(decision.action), int(A::Relist));
            } else {
                QCOMPARE(int(decision.diagnosis), int(D::OtherInstanceHoldsPhone));
                QCOMPARE(int(decision.action), int(A::WaitForUser));
            }
        }
        // A sibling while the phone is listed is not blamed for anything.
        auto listed = Ready();
        listed.otherInstanceRunning = true;
        QCOMPARE(int(doctor.Decide(listed, 31000).diagnosis), int(D::Connected));
    }
};

QTEST_APPLESS_MAIN(PhoneLinkDoctorTests)
#include "phone_link_doctor_tests.moc"
